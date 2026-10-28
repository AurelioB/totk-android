/* Single-owner persistence; no recorder lock, pipeline-queue lock or Vulkan external-cache lock.
 * VkPipelineCache is created with flags=0: the driver synchronizes concurrent create/get-data.
 * Generation sampled BEFORE extraction ensures concurrent learning is never acknowledged early. */
#include "vk_int.h"
#include "vk_pcache.h"
#include "vk_pcache_store.h"
#include "vk_pcache_identity.h"
#include <sys/resource.h>
#include <time.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv;
static pthread_t thread;
static int started, joining, stop, background, stats;
static uint64_t request, completed, saves;
static const char *cache_path;
static pcache_policy policy;
static pcache_store store;
static pcache_identity *identity;
static int identity_ready;
static int legacy_primary;
static int primary_loaded;
const char *vk_pcache_loaded_path(void) { return primary_loaded ? cache_path : NULL; }
const char *vk_pcache_current_path(void) { return cache_path; }
/* Called after Vulkan device selection, hence after frontend env_file/A722 policy and driver load.
 * Identity is immutable before the saver starts. A hash failure disables persistence/import rather
 * than labeling an unknown custom binary as the system driver. APK identity is deliberately absent. */
static int prepare_identity(void) {
    if (identity_ready) return identity != NULL;
    identity_ready = 1;
    const char *driver = getenv("TOTK_VK_DRIVER");
    int custom = driver && *driver && strcmp(driver, "system");
    uint8_t sha[32] = {0};
    if (custom) {
        uint64_t start = host_now_ns();
        if (!pcache_driver_hash(driver, sha)) {
            RED(4, "pipeline cache: cannot hash selected driver %s: %s; persistence/import disabled", driver, strerror(errno));
            return 0;
        }
        char hex[65]; static const char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < 32; i++) { hex[2*i] = digits[sha[i] >> 4]; hex[2*i+1] = digits[sha[i] & 15]; }
        hex[64] = 0;
        LOGV("pipeline cache: copied driver sha256=%s hash_ms=%.3f", hex, (host_now_ns() - start) / 1e6);
    }
    identity = pcache_identity_create(getenv("TU_DEBUG"), sha, custom);
    if (!identity) RED(4, "pipeline cache: cannot allocate driver context; persistence/import disabled");
    return identity != NULL;
}
void *vk_pcache_load(const char *path, size_t *size, int primary) {
    *size = 0; if (!path || !prepare_identity()) return NULL;
    int result;
    void *data = pcache_read_context(path, identity, V.props.vendorID, V.props.deviceID,
                                     V.props.pipelineCacheUUID, size, &result);
    if (data) {
        if (primary) { legacy_primary = result == PCACHE_READ_LEGACY; primary_loaded = 1; }
        LOGV("pipeline cache: %zu KiB from %s (%s)", *size >> 10, path,
             result == PCACHE_READ_LEGACY ? "legacy: empty TU_DEBUG + Vulkan identity; full driver hash/history unproven" : "context matched");
    } else if (result != PCACHE_READ_NONE && result != PCACHE_READ_IO) {
        static const char *const reasons[] = {[PCACHE_READ_FORMAT] = "malformed/truncated envelope or blob",
            [PCACHE_READ_DEBUG] = "TU_DEBUG changed", [PCACHE_READ_DRIVER] = "driver SHA256/source changed",
            [PCACHE_READ_LEGACY_DEBUG] = "legacy cache cannot prove nonempty TU_DEBUG",
            [PCACHE_READ_VULKAN] = "Vulkan device/driver identity changed"};
        int kept = pcache_quarantine(path), error = errno;
        LOGV("pipeline cache: %s: %s; starting fresh (%s%s)", path, reasons[result],
             kept > 0 ? "renamed to .stale" : !kept ? "existing .stale preserved; rejected path retained until fresh save" : "quarantine failed: ",
             kept < 0 ? strerror(error) : "");
    } else if (result == PCACHE_READ_IO) LOGV("pipeline cache: %s unreadable; not used", path);
    return data;
}
#ifdef TOTK_VK_TESTS
static uint64_t published[7]; /* mu-owned completed-iteration snapshot, never expose mutable scratch/policy */
#endif
static _Atomic uint64_t generation, last_present, quiet_since;
static _Atomic int enabled, diagnostics;
/* Seqlock publishes a cumulative extraction clock. Span differences measure actual overlapping wall time,
 * including operations that begin before or end after extraction. This is NOT proof of driver-lock blocking. */
static _Atomic unsigned busy_seq;
static _Atomic uint64_t busy_total, busy_start;
static uint64_t cpu_ns(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * PCACHE_SECOND + t.tv_nsec; }
static uint64_t busy_clock(void) {
    for (;;) {
        unsigned a = atomic_load_explicit(&busy_seq, memory_order_acquire); if (a & 1) continue;
        uint64_t total = atomic_load_explicit(&busy_total, memory_order_relaxed);
        uint64_t start = atomic_load_explicit(&busy_start, memory_order_relaxed), now = host_now_ns();
        atomic_thread_fence(memory_order_acquire);
        if (a == atomic_load_explicit(&busy_seq, memory_order_relaxed)) return total + (start ? now - start : 0);
    }
}
static void extraction_begin(void) {
    if (!stats) return;
    atomic_fetch_add_explicit(&busy_seq, 1, memory_order_acq_rel);
    atomic_store_explicit(&busy_start, host_now_ns(), memory_order_relaxed);
    atomic_fetch_add_explicit(&busy_seq, 1, memory_order_release);
}
static void extraction_end(void) {
    if (!stats) return;
    atomic_fetch_add_explicit(&busy_seq, 1, memory_order_acq_rel);
    uint64_t start = atomic_load_explicit(&busy_start, memory_order_relaxed);
    atomic_fetch_add_explicit(&busy_total, host_now_ns() - start, memory_order_relaxed);
    atomic_store_explicit(&busy_start, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&busy_seq, 1, memory_order_release);
}
vk_pcache_span vk_pcache_span_begin(void) {
    if (!atomic_load_explicit(&diagnostics, memory_order_relaxed)) return (vk_pcache_span){0};
    return (vk_pcache_span){host_now_ns(), busy_clock()};
}
void vk_pcache_span_end(vk_pcache_span span, const char *operation) {
    if (!span.wall) return;
    uint64_t overlap = busy_clock() - span.busy, elapsed = host_now_ns() - span.wall;
    if (overlap) LOGV("[pcache-stats] overlap operation=%s wall_ms=%.3f extraction_overlap_ms=%.3f cpu=%d (concurrent wall time, not attributed lock wait)",
                     operation, elapsed / 1e6, overlap / 1e6, sched_getcpu());
}
static void learned(VkResult result) {
    if (result == VK_SUCCESS) atomic_fetch_add_explicit(&generation, 1, memory_order_release);
}
void vk_pcache_changed(void) { atomic_fetch_add_explicit(&generation, 1, memory_order_release); }
VkResult vk_pcache_graphics(const VkGraphicsPipelineCreateInfo *info, VkPipeline *pipeline) {
    vk_pcache_span span = vk_pcache_span_begin();
    VkResult result = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, info, NULL, pipeline);
    learned(result); vk_pcache_span_end(span, "graphics-create"); return result;
}
VkResult vk_pcache_compute(const VkComputePipelineCreateInfo *info, VkPipeline *pipeline) {
    vk_pcache_span span = vk_pcache_span_begin();
    VkResult result = vkCreateComputePipelines(V.dev, V.pcache, 1, info, NULL, pipeline);
    learned(result); vk_pcache_span_end(span, "compute-create"); return result;
}
void vk_pcache_present(uint64_t now, uint64_t draws) {
    if (!atomic_load_explicit(&enabled, memory_order_relaxed)) return;
    atomic_store_explicit(&last_present, now, memory_order_relaxed);
    /* No guest pause/load decoder is available here. A sustained low-draw screen is a conservative hint,
     * never a claimed authoritative game state. Real Android background events are separate. */
    if (draws > 128) atomic_store_explicit(&quiet_since, 0, memory_order_relaxed);
    else if (!atomic_load_explicit(&quiet_since, memory_order_relaxed)) atomic_store_explicit(&quiet_since, now, memory_order_relaxed);
}
static int query(void *context, size_t *size, void *data) {
    (void)context; extraction_begin();
    VkResult result = vkGetPipelineCacheData(V.dev, V.pcache, size, data);
    extraction_end(); if (result) errno = EIO; return result;
}
static void place_thread(void) {
    totk_thread_start("totk-pcache", NULL);
    errno = 0; int priority_error = setpriority(PRIO_PROCESS, 0, 10) ? errno : 0;
    cpu_set_t allowed, preferred, actual;
    const char *source = "unavailable", *decision = "unchanged";
    int affinity_error = 0;
    if (sched_getaffinity(0, sizeof allowed, &allowed)) affinity_error = errno;
    else {
        unsigned capacity[CPU_SETSIZE];
        totk_thread_background_select("/sys/devices/system/cpu", &allowed,
                                      &preferred, capacity, &source, &decision);
        if (!CPU_EQUAL(&preferred, &allowed) &&
            sched_setaffinity(0, sizeof preferred, &preferred)) affinity_error = errno;
    }
    char mask[CPU_SETSIZE * 6];
    if (!sched_getaffinity(0, sizeof actual, &actual)) totk_thread_mask_text(&actual, mask, sizeof mask); else strcpy(mask, "?");
    errno = 0; int nice_value = getpriority(PRIO_PROCESS, 0), nice_error = errno;
    /* Placement failures must be visible even with diagnostics off; never claim the intent was applied. */
    if (stats || priority_error || affinity_error || nice_error)
        LOGV("[pcache-stats] placement nice=%d nice_set_errno=%d nice_get_errno=%d affinity=%s affinity_source=%s affinity_decision=%s affinity_errno=%d cpu=%d", nice_value, priority_error, nice_error, mask, source, decision, affinity_error, sched_getcpu());
}
static void *saver(void *unused) {
    (void)unused; place_thread();
    static const char *const reasons[] = {"clean", "unchanged-size", "min-interval", "small-growth", "gameplay", "quiet", "max-delay", "lifecycle-flush"};
    pthread_mutex_lock(&mu);
    for (;;) {
        uint64_t ticket = request, now = host_now_ns(), gen = atomic_load_explicit(&generation, memory_order_acquire);
        if (!identity || !cache_path) gen = policy.saved_generation; /* collection also works with opaque persistence disabled */
        int finish = stop, flush = finish || ticket != completed, bg = background;
        uint64_t present = atomic_load_explicit(&last_present, memory_order_relaxed);
        uint64_t low = atomic_load_explicit(&quiet_since, memory_order_relaxed);
        int idle = present && now - present >= 2 * PCACHE_SECOND;
        int low_draw = low && now - low >= 2 * PCACHE_SECOND;
        const char *quiet_reason = bg ? "background" : idle ? "no-present-idle" : low_draw ? "low-draw-screen-heuristic" : "none";
        pthread_mutex_unlock(&mu);
        uint64_t start = host_now_ns(), cpu = stats ? cpu_ns() : 0, before_bytes = store.bytes_written;
        size_t size = policy.saved_size, old_size = policy.saved_size;
        uint64_t pipelines = gen - policy.saved_generation;
        /* gen was sampled before the probe: an unchanged-size acknowledgement must not consume later creates. */
        int probed = pipelines && (flush || now - policy.last_save >= PCACHE_MIN_NS);
        int query_failed = probed && query(NULL, &size, NULL) != 0;
        int decision = pcache_decide(&policy, now, gen, size, probed && !query_failed, bg || idle || low_draw, flush);
        uint64_t dirty_age = policy.dirty_since ? now - policy.dirty_since : 0;
        if (cache_path && identity && vk_pipeline_keys_dirty() && (flush || ((bg || idle || low_draw) && now-policy.last_save >= PCACHE_MIN_NS)))
            decision = PCACHE_FLUSH; /* Progress needs a real extraction even when cache-hit creates did not grow it. */
        int save = decision >= PCACHE_QUIET, ok = 0;
        if (save && !query_failed) {
            void *key_snapshot = vk_pipeline_keys_snapshot();
            ok = pcache_snapshot(&store, cache_path, query, NULL, &size);
            if (ok) {
                pcache_saved(&policy, now, gen, size); saves++;
                if (atomic_load_explicit(&generation, memory_order_acquire) != gen) policy.dirty_since = now;
            }
            else RED(4, "pipeline cache: snapshot failed for %s: %s (dirty state retained)", cache_path, strerror(errno));
            if (ok) vk_pipeline_keys_saved(cache_path, key_snapshot); else free(key_snapshot);
        }
        if (stats) LOGV("[pcache-stats] %s reason=%s quiet=%s saves=%llu bytes=%llu total_bytes=%llu growth=%lld pipelines=%llu dirty_age_ms=%.0f duration_ms=%.3f thread_cpu_ms=%.3f cpu=%d capacity=%zu allocations=%llu change_detection=size-growth-heuristic",
            query_failed || (save && !ok) ? "failed" : save ? "taken" : decision <= PCACHE_UNCHANGED ? "skipped" : "deferred", reasons[decision], quiet_reason,
            (unsigned long long)saves, (unsigned long long)(store.bytes_written - before_bytes), (unsigned long long)store.bytes_written,
            (long long)size - (long long)old_size, (unsigned long long)pipelines,
            dirty_age / 1e6, (host_now_ns() - start) / 1e6, stats ? (cpu_ns() - cpu) / 1e6 : 0,
            sched_getcpu(), store.capacity, (unsigned long long)store.allocations);
        vk_pipeline_keys_collect_save();
        pthread_mutex_lock(&mu);
#ifdef TOTK_VK_TESTS
        published[0] = saves; published[1] = gen; published[2] = policy.saved_generation;
        published[3] = store.capacity; published[4] = store.allocations; published[5] = store.bytes_written;
#endif
        completed = ticket; pthread_cond_broadcast(&cv);
        if (finish) break;
        if (request == completed) {
            struct timespec wake; clock_gettime(CLOCK_MONOTONIC, &wake); wake.tv_sec += 5;
            pthread_cond_timedwait(&cv, &mu, &wake);
        }
    }
    free(store.data); store.data = NULL; store.capacity = 0;
    pthread_mutex_unlock(&mu); return NULL;
}
void vk_pcache_start(const char *path, size_t loaded_size) {
    const char *collect = getenv("TOTK_PSO_COLLECT");
#ifndef TOTK_PSO_COLLECT_DEFAULT
#define TOTK_PSO_COLLECT_DEFAULT 0
#endif
    int collection = collect ? atoi(collect) != 0 : TOTK_PSO_COLLECT_DEFAULT;
    if (!path && !collection) return;
    if (path && !prepare_identity() && !collection) return;
    /* Identity failure/cache-off may still use the single saver for identifier-only collection. */
    pthread_mutex_lock(&mu);
    cache_path = path; policy.saved_size = loaded_size; policy.last_save = host_now_ns();
    store.prefix = identity; store.prefix_size = identity ? pcache_identity_size(identity) : 0;
    if (legacy_primary) {
        /* Preserve all accepted legacy entries; force one ordinary coalesced save to record context. */
        policy.saved_size = 0; vk_pcache_changed();
    }
#ifdef TOTK_VK_TESTS
    published[6] = loaded_size;
#endif
    if (background) request++;
    const char *e = getenv("TOTK_PCACHE_STATS"); stats = e && !strcmp(e, "1");
    pthread_condattr_t attr; pthread_condattr_init(&attr); pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&cv, &attr); pthread_condattr_destroy(&attr);
    if (!pthread_create(&thread, NULL, saver, NULL)) {
        started = 1; atomic_store(&enabled, 1); atomic_store(&diagnostics, stats); atexit(vk_pcache_shutdown);
    } else RED(4, "pipeline cache: saver thread could not start");
    pthread_mutex_unlock(&mu);
}
void vk_pcache_background(int value) {
    pthread_mutex_lock(&mu);
    background = value;
    if (started && !joining && value) { request++; pthread_cond_signal(&cv); }
    pthread_mutex_unlock(&mu);
}
void vk_pcache_flush(void) {
    pthread_mutex_lock(&mu);
    if (started && !joining) {
        uint64_t ticket = ++request; pthread_cond_signal(&cv);
        while (completed < ticket) pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu);
}
void vk_pcache_shutdown(void) {
    pthread_mutex_lock(&mu);
    while (joining) pthread_cond_wait(&cv, &mu);
    if (!started) { pthread_mutex_unlock(&mu); return; }
    joining = stop = 1; request++; pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu); pthread_join(thread, NULL); pthread_mutex_lock(&mu);
    started = joining = 0; atomic_store(&enabled, 0); pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}
#ifdef TOTK_VK_TESTS
/* Completed-iteration publication is protected independently of scratch and driver extraction. */
void vk_pcache_test_stats(uint64_t out[7]) {
    pthread_mutex_lock(&mu); memcpy(out, published, sizeof published); pthread_mutex_unlock(&mu);
}
#endif
