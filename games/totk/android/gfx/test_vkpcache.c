/* Synthetic policy/failure fixtures plus the real lavapipe load/create/snapshot/lifecycle path. */
#include "vk_int.h"
#include "vk_pcache_store.h"
#include "vk_pcache_identity.h"
#include <sys/wait.h>
#include <time.h>
static int checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; printf("FAIL: %s\n", label); } } while (0)
static uint64_t seconds(unsigned n) { return n * PCACHE_SECOND; }
static void policies(void) {
    pcache_policy p = {.saved_size = 1024, .last_save = seconds(1)};
    CHECK(pcache_decide(&p, seconds(2), 0, 1024, 1, 1, 1) == PCACHE_CLEAN, "clean shutdown does not rewrite");
    CHECK(pcache_decide(&p, seconds(2), 1, 1025, 1, 1, 0) == PCACHE_INTERVAL, "first growth starts dirty age");
    CHECK(p.dirty_since == seconds(2), "dirty age anchored to first growth");
    CHECK(pcache_decide(&p, seconds(60), 500, PCACHE_GROWTH + 1024, 1, 1, 0) == PCACHE_INTERVAL, "interval gates substantial growth");
    CHECK(pcache_decide(&p, seconds(61), 1, 1025, 1, 1, 0) == PCACHE_GROW_SMALL, "small growth coalesces even in quiet moment");
    CHECK(pcache_decide(&p, seconds(61), 1, PCACHE_GROWTH + 1024, 1, 0, 0) == PCACHE_BUSY, "byte threshold defers in gameplay");
    CHECK(pcache_decide(&p, seconds(61), 1, PCACHE_GROWTH + 1024, 1, 1, 0) == PCACHE_QUIET, "byte threshold saves in quiet moment");
    CHECK(pcache_decide(&p, seconds(61), PCACHE_PIPELINES, 1025, 1, 1, 0) == PCACHE_QUIET, "pipeline threshold saves only with actual size growth");
    CHECK(pcache_decide(&p, seconds(121), 1, 1025, 1, 0, 0) == PCACHE_GROW_SMALL, "tiny growth waits until deadline");
    CHECK(pcache_decide(&p, seconds(122), 1, 1025, 1, 0, 0) == PCACHE_DEADLINE, "maximum delay overrides growth threshold AND quietness");
    CHECK(pcache_decide(&p, seconds(3), 1, 1025, 1, 0, 1) == PCACHE_FLUSH, "shutdown bypasses both gates when size grows");
    for (unsigned t = 3; t < 122; t++) {
        pcache_decide(&p, seconds(t), t, 1024 + t, 1, 0, 0);
        CHECK(p.dirty_since == seconds(2), "continuous low-growth trickle never resets deadline");
    }
    CHECK(pcache_decide(&p, seconds(122), 122, 1146, 1, 0, 0) == PCACHE_DEADLINE, "trickle with no quiet moments flushes on deadline");
    pcache_saved(&p, seconds(122), 120, 1146);
    CHECK(pcache_decide(&p, seconds(123), 121, 1147, 1, 1, 0) == PCACHE_INTERVAL, "creates concurrent with snapshot remain dirty");
    CHECK(p.saved_generation == 120 && p.dirty_since == seconds(123), "baseline covers only pre-extraction generation");
    for (unsigned mode = 0; mode < 3; mode++) {
        pcache_policy unchanged = {.saved_size = 1024, .last_save = seconds(1), .dirty_since = seconds(2)};
        uint64_t now = seconds(mode == 1 ? 122 : 61), gen = 1000;
        CHECK(pcache_decide(&unchanged, now, gen, 1024, 1, mode == 0, mode == 2) == PCACHE_UNCHANGED,
              "unchanged size bypasses quiet, deadline and lifecycle payload snapshots");
        CHECK(unchanged.saved_generation == gen && !unchanged.dirty_since, "unchanged size acknowledges cache-hit generations and clears dirty age");
        CHECK(unchanged.saved_size == 1024 && unchanged.last_save == seconds(1), "skip does not advance the durable file-size/time baseline");
        CHECK(pcache_decide(&unchanged, now + 1, gen, 1024, 1, 1, 1) == PCACHE_CLEAN, "acknowledged unchanged generation is clean");
        CHECK(pcache_decide(&unchanged, now + 2, gen + 1, 1025, 1, 1, 1) == PCACHE_FLUSH, "later actual growth still takes the normal snapshot path");
    }
    pcache_policy unknown = {.saved_size = 1024, .last_save = seconds(1), .dirty_since = seconds(2)};
    CHECK(pcache_decide(&unknown, seconds(122), 1, 1024, 0, 0, 0) == PCACHE_DEADLINE &&
          unknown.saved_generation == 0 && unknown.dirty_since == seconds(2), "failed/missing size query cannot acknowledge dirty generation");
}
static const unsigned char old_blob[] = "synthetic-old-cache", new_blob[] = "synthetic-new-cache-larger";
static int synthetic_query(void *unused, size_t *size, void *data) {
    (void)unused; if (data) memcpy(data, new_blob, sizeof new_blob); *size = sizeof new_blob; return 0;
}
static int matches(const char *path, const void *data, size_t size) {
    unsigned char bytes[128]; int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, bytes, sizeof bytes); close(fd); return n == (ssize_t)size && !memcmp(bytes, data, size);
}
static _Atomic int reader_stop, reader_bad;
static void *atomic_reader(void *arg) {
    const char *path = arg;
    while (!atomic_load(&reader_stop)) {
        unsigned char bytes[128]; int fd = open(path, O_RDONLY);
        if (fd < 0) { atomic_store(&reader_bad, 1); break; }
        ssize_t n = read(fd, bytes, sizeof bytes); close(fd);
        if (!((n == sizeof old_blob && !memcmp(bytes, old_blob, sizeof old_blob)) ||
              (n == sizeof new_blob && !memcmp(bytes, new_blob, sizeof new_blob)))) atomic_store(&reader_bad, 1);
    }
    return NULL;
}
static int incomplete_query(void *unused, size_t *size, void *data) {
    if (data) return VK_INCOMPLETE;
    return synthetic_query(unused, size, data);
}
static void stores(void) {
    const char *path = "pcache-atomic-fixture.bin"; size_t size;
    for (int failure = PCACHE_FAIL_ALLOC; failure <= PCACHE_FAIL_DIRSYNC; failure++) {
        pcache_store s = {0}; CHECK(pcache_reserve(&s, sizeof old_blob), "fixture allocation");
        memcpy(s.data, old_blob, sizeof old_blob); CHECK(pcache_replace(&s, path, sizeof old_blob), "old file installed durably");
        if (failure == PCACHE_FAIL_ALLOC) { free(s.data); s.data = NULL; s.capacity = 0; }
        s.fail = failure;
        pcache_policy p = {.saved_size = sizeof old_blob, .dirty_since = seconds(2), .last_save = seconds(1)};
        pcache_policy before = p;
        int ok = pcache_snapshot(&s, path, synthetic_query, NULL, &size);
        if (ok) pcache_saved(&p, seconds(5), 1, size);
        CHECK(!ok, "injected snapshot stage fails");
        CHECK(!memcmp(&p, &before, sizeof p), "failure retains dirty age, generation, size and last-save time");
        CHECK(matches(path, failure == PCACHE_FAIL_DIRSYNC ? new_blob : old_blob,
                      failure == PCACHE_FAIL_DIRSYNC ? sizeof new_blob : sizeof old_blob), "failure leaves complete old blob (post-rename directory-sync failure leaves complete new blob)");
        CHECK(access("pcache-atomic-fixture.bin.tmp", F_OK) != 0, "failed transaction removes temporary file");
        s.fail = 0; CHECK(pcache_snapshot(&s, path, synthetic_query, NULL, &size), "retry succeeds after every failure stage");
        CHECK(matches(path, new_blob, sizeof new_blob), "atomic retry installs complete replacement");
        void *pointer = s.data; size_t capacity = s.capacity; uint64_t allocations = s.allocations;
        CHECK(pcache_snapshot(&s, path, synthetic_query, NULL, &size), "subsequent snapshot succeeds");
        CHECK(s.data == pointer && s.capacity == capacity && s.allocations == allocations, "scratch is retained and reused without reallocation");
        free(s.data);
    }
    pcache_store s = {0};
    CHECK(pcache_snapshot(&s, path, synthetic_query, NULL, &size), "atomic reader fixture installed");
    CHECK(!pcache_snapshot(&s, path, incomplete_query, NULL, &size) && matches(path, new_blob, sizeof new_blob),
          "VK_INCOMPLETE never replaces the old file");
    pthread_t reader; pthread_create(&reader, NULL, atomic_reader, (void *)path);
    for (unsigned i = 0; i < 64; i++) {
        size_t n = i & 1 ? sizeof old_blob : sizeof new_blob;
        memcpy(s.data, i & 1 ? old_blob : new_blob, n);
        CHECK(pcache_replace(&s, path, n), "atomic alternating replacement");
    }
    atomic_store(&reader_stop, 1); pthread_join(reader, NULL);
    CHECK(!atomic_load(&reader_bad), "concurrent reader sees only complete old/new files");
    free(s.data);
    unlink(path);
}
static void context_fixture(const char *path, pcache_identity *id, const void *blob, size_t size) {
    pcache_store s = {.data = (void *)blob, .prefix = id, .prefix_size = id ? pcache_identity_size(id) : 0};
    CHECK(pcache_replace(&s, path, size), "context fixture installed atomically");
}
static void context_expect(const char *path, pcache_identity *id, const uint8_t uuid[16], int want, const void *blob, size_t length) {
    size_t size; int status;
    void *data = pcache_read_context(path, id, 0x5143, 0x43020100, uuid, &size, &status);
    CHECK(status == want, "exact context/legacy rejection category");
    CHECK(want == PCACHE_READ_MATCH || want == PCACHE_READ_LEGACY ?
          data && size == length && !memcmp(data, blob, size) : !data && !size,
          "only matching context reaches Vulkan payload; opaque bytes unchanged");
    free(data);
}
static void contexts(void) {
    const char *path = "pcache-context-fixture.bin";
    uint8_t driver[32] = {1}, other[32] = {2}, uuid[16] = {3}, changed_uuid[16] = {4};
    uint32_t blob[16] = {32, 1, 0x5143, 0x43020100}; memcpy(blob + 4, uuid, 16);
    pcache_identity *plain = pcache_identity_create(NULL, driver, 1);
    pcache_identity *empty = pcache_identity_create("", driver, 1);
    pcache_identity *sysmem = pcache_identity_create("sysmem", driver, 1);
    pcache_identity *flags = pcache_identity_create("sysmem,nolrz", driver, 1);
    pcache_identity *different = pcache_identity_create("", other, 1);
    pcache_identity *system = pcache_identity_create("", (uint8_t[32]){0}, 0);
    CHECK(plain && empty && sysmem && flags && different && system, "exact variable-length context allocation");
    CHECK(pcache_identity_size(plain) == pcache_identity_size(empty) &&
          !memcmp(plain, empty, pcache_identity_size(plain)), "unset and empty TU_DEBUG have identical effective flags");
    context_fixture(path, sysmem, blob, sizeof blob);
    context_expect(path, sysmem, uuid, PCACHE_READ_MATCH, blob, sizeof blob);
    context_expect(path, plain, uuid, PCACHE_READ_DEBUG, blob, sizeof blob);
    context_expect(path, flags, uuid, PCACHE_READ_DEBUG, blob, sizeof blob);
    context_fixture(path, plain, blob, sizeof blob);
    context_expect(path, different, uuid, PCACHE_READ_DRIVER, blob, sizeof blob);
    context_expect(path, system, uuid, PCACHE_READ_DRIVER, blob, sizeof blob);
    context_expect(path, plain, changed_uuid, PCACHE_READ_VULKAN, blob, sizeof blob);
    uint32_t rollback[8]; int fd = open(path, O_RDONLY);
    CHECK(fd >= 0 && pcache_read_exact(fd, rollback, sizeof rollback, 0), "rollback header readable");
    if (fd >= 0) close(fd);
    CHECK(rollback[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE,
          "universal1b rejects envelope by header version before passing bytes to driver");
    /* Test every truncated prefix/payload-header length and malformed fixed fields. */
    for (size_t size = 0; size < pcache_identity_size(plain) + 32; size++) {
        context_fixture(path, plain, blob, sizeof blob);
        CHECK(!truncate(path, (off_t)size), "truncate fixture");
        context_expect(path, plain, uuid, PCACHE_READ_FORMAT, blob, sizeof blob);
    }
    uint32_t *fields[] = {&plain->version, &plain->debug_size, &plain->custom_driver, &plain->reserved};
    for (unsigned i = 0; i < 4; i++) {
        context_fixture(path, plain, blob, sizeof blob);
        uint32_t saved = *fields[i]; *fields[i] = UINT32_MAX;
        fd = open(path, O_WRONLY);
        CHECK(fd >= 0 && pwrite(fd, plain, sizeof *plain, 0) == sizeof *plain, "malformed fixed envelope field");
        if (fd >= 0) close(fd);
        *fields[i] = saved;
        context_expect(path, plain, uuid, PCACHE_READ_FORMAT, blob, sizeof blob);
    }
    context_fixture(path, NULL, blob, sizeof blob);
    context_expect(path, plain, uuid, PCACHE_READ_LEGACY, blob, sizeof blob);
    context_expect(path, different, uuid, PCACHE_READ_LEGACY, blob, sizeof blob);
    context_expect(path, plain, changed_uuid, PCACHE_READ_VULKAN, blob, sizeof blob);
    context_expect(path, sysmem, uuid, PCACHE_READ_LEGACY_DEBUG, blob, sizeof blob);
    /* Atomic prefix+payload: failed writes/renames and abandoned temp files never expose a prefix alone. */
    for (int fail = PCACHE_FAIL_OPEN; fail <= PCACHE_FAIL_DIRSYNC; fail++) {
        context_fixture(path, plain, blob, sizeof blob);
        pcache_store s = {.data = blob, .prefix = sysmem, .prefix_size = pcache_identity_size(sysmem), .fail = fail};
        CHECK(!pcache_replace(&s, path, sizeof blob), "enveloped transaction failure retained for retry");
        context_expect(path, fail == PCACHE_FAIL_DIRSYNC ? sysmem : plain, uuid, PCACHE_READ_MATCH, blob, sizeof blob);
    }
    context_fixture(path, plain, blob, sizeof blob);
    fd = open("pcache-context-fixture.bin.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0 && write(fd, sysmem, 8) == 8, "interrupted save leaves partial temp only");
    if (fd >= 0) close(fd);
    context_expect(path, plain, uuid, PCACHE_READ_MATCH, blob, sizeof blob);
    unlink("pcache-context-fixture.bin.tmp");
    unlink("pcache-context-fixture.bin.stale");
    CHECK(pcache_quarantine(path) == 1 && access(path, F_OK), "first mismatch renamed to sole stale backup");
    context_fixture(path, sysmem, blob, sizeof blob);
    for (unsigned i = 0; i < 3; i++) CHECK(pcache_quarantine(path) == 0, "repeated mismatch preserves first stale, never overwrites it");
    context_expect("pcache-context-fixture.bin.stale", plain, uuid, PCACHE_READ_MATCH, blob, sizeof blob);
    context_expect(path, sysmem, uuid, PCACHE_READ_MATCH, blob, sizeof blob);
    CHECK(pcache_quarantine("/nonexistent-parent/cache.bin") == -1, "failed quarantine rename remains visible");
    unlink(path); unlink("pcache-context-fixture.bin.stale");
    free(plain); free(empty); free(sysmem); free(flags); free(different); free(system);
    /* Real streaming hash, bounded 64-KiB scratch, 14-MiB driver-sized input. No target execution. */
    fd = open("pcache-driver-fixture.bin", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    static const uint8_t zeros[65536];
    for (unsigned i = 0; i < (14u << 20) / sizeof zeros; i++)
        CHECK(fd >= 0 && write(fd, zeros, sizeof zeros) == sizeof zeros, "14-MiB driver fixture write");
    if (fd >= 0) close(fd);
    static const uint8_t expected[32] = {0xe8,0x6b,0xae,0x8c,0x05,0x98,0xc4,0xff,0x83,0xc6,0x95,0xf4,0x67,0xda,0xa4,0xa1,
        0xe8,0xfa,0x01,0xd5,0x7f,0x91,0x40,0x37,0x29,0x93,0x36,0x62,0x04,0x02,0x2a,0x4d};
    uint64_t start = host_now_ns();
    CHECK(pcache_driver_hash("pcache-driver-fixture.bin", driver) && !memcmp(driver, expected, 32), "streamed SHA256 agrees with independent 14-MiB fixture digest");
    printf("pcache driver hash: 14 MiB %.3f ms (host, page-cache-warm fixture)\n", (host_now_ns() - start) / 1e6);
    unlink("pcache-driver-fixture.bin");
    CHECK(!pcache_driver_hash("pcache-driver-fixture.bin", driver), "missing selected driver is not a usable identity");
}
/* Minimal synthetic no-op compute module, generated from 'layout(local_size_x=1) in; void main() {}'. */
static const uint32_t spirv[] = {0x07230203,0x00010000,0,6,0,0x00020011,1,0x0003000e,0,1,
    0x0005000f,5,4,0x6e69616d,0,0x00060010,4,17,1,1,1,0x00020013,1,0x00030021,2,1,
    0x00050036,1,4,0,2,0x000200f8,5,0x000100fd,0x00010038};
static VkComputePipelineCreateInfo ci;
static _Atomic int creation_failed;
static void *create_pipelines(void *unused) {
    (void)unused;
    for (int i = 0; i < 32; i++) {
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vk_pcache_compute(&ci, &pipeline)) atomic_store(&creation_failed, 1);
        if (pipeline) vkDestroyPipeline(V.dev, pipeline, NULL);
    }
    return NULL;
}
static pthread_mutex_t hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cv = PTHREAD_COND_INITIALIZER;
static int hold_data, entered, release_data;
static int hold_size_phase, size_entered, release_size;
static PFN_vkGetPipelineCacheData real_query;
/* Lavapipe may retain only a header for this no-op shader. Controlled zero padding makes size-growth
 * stimuli deterministic; the underlying header/payload extraction and pipeline creation are real Vulkan.
 * This test-only dispatch wrapper is not a production cache format or opaque-blob concatenation. */
static _Atomic size_t padding;
static _Atomic unsigned data_queries;
static _Atomic unsigned size_queries;
static void hold_size_probe(int phase) {
    pthread_mutex_lock(&hold_mu);
    if (hold_size_phase == phase) {
        size_entered = phase; pthread_cond_broadcast(&hold_cv);
        while (!release_size) pthread_cond_wait(&hold_cv, &hold_mu);
        hold_size_phase = 0;
    }
    pthread_mutex_unlock(&hold_mu);
}
static VKAPI_ATTR VkResult VKAPI_CALL held_query(VkDevice dev, VkPipelineCache cache, size_t *size, void *data) {
    pthread_mutex_lock(&hold_mu);
    if (data && hold_data) {
        entered = 1; pthread_cond_broadcast(&hold_cv);
        while (!release_data) pthread_cond_wait(&hold_cv, &hold_mu);
        hold_data = 0;
    }
    pthread_mutex_unlock(&hold_mu);
    if (!data) hold_size_probe(1); /* generation sampled, size not yet observed */
    size_t actual = 0, extra = atomic_load(&padding);
    VkResult result = real_query(dev, cache, &actual, NULL); if (result) return result;
    if (!data) {
        *size = actual + extra; atomic_fetch_add(&size_queries, 1);
        hold_size_probe(2); /* size observed, policy's saved-generation update not yet reached */
        return VK_SUCCESS;
    }
    atomic_fetch_add(&data_queries, 1);
    if (*size < actual + extra) return VK_INCOMPLETE;
    *size = actual; result = real_query(dev, cache, size, data);
    if (!result) { memset((char *)data + *size, 0, extra); *size += extra; }
    return result;
}
static void *flush(void *unused) { (void)unused; vk_pcache_flush(); return NULL; }
static void *lifecycle(void *unused) {
    (void)unused;
    for (int i = 0; i < 16; i++) { vk_pcache_background(1); vk_pcache_background(0); vk_pcache_flush(); }
    return NULL;
}
static void *shutdown_thread(void *unused) { (void)unused; vk_pcache_shutdown(); return NULL; }
static int real_context_case(unsigned mode) {
    /* Same production entry for primary persistence and one-shot import, in a fresh process each. */
    unsigned variant = mode % 5; int importing = mode >= 5;
    char original[1100], primary[1100], incoming[1100];
    const char *source = vk_shader_cache_path("state/pcache-reload.bin", original, sizeof original);
    int fd = open(source, O_RDONLY); struct stat st; pcache_identity h;
    CHECK(fd >= 0 && !fstat(fd, &st) && pcache_read_exact(fd, &h, sizeof h, 0), "real saved fixture header");
    size_t offset = pcache_identity_size(&h), length = (size_t)st.st_size - offset;
    void *blob = malloc(length);
    CHECK(blob && pcache_read_exact(fd, blob, length, (off_t)offset), "real saved opaque bytes");
    if (fd >= 0) close(fd);
    const char *path = vk_shader_cache_path("state/pcache-context-real.bin", primary, sizeof primary);
    const char *ip = vk_shader_cache_path("state/pcache-context-import.bin", incoming, sizeof incoming);
    unlink(path); unlink(ip);
    char stale[1200], imported[1200];
    snprintf(stale, sizeof stale, "%s.stale", importing ? ip : path); unlink(stale);
    snprintf(imported, sizeof imported, "%s.imported", ip); unlink(imported);
    uint8_t sha[32] = {0}; if (variant == 2) sha[0] = 1;
    pcache_identity *id = pcache_identity_create(variant == 1 ? "sysmem" : "", sha, 0);
    context_fixture(importing ? ip : path, variant >= 3 ? NULL : id, blob, length);
    free(blob); free(id);
    setenv("TOTK_VK_PIPELINE_CACHE", "state/pcache-context-real.bin", 1);
    setenv("TOTK_VK_PIPELINE_CACHE_IMPORT", importing ? "state/pcache-context-import.bin" : "0", 1);
    setenv("TOTK_VK_PREWARM", "0", 1);
    if (variant == 4) setenv("TU_DEBUG", "sysmem", 1); else unsetenv("TU_DEBUG");
    mtl_init();
    int accepted = variant == 0 || variant == 3;
    uint64_t counts[7]; vk_pcache_flush(); vk_pcache_test_stats(counts);
    CHECK(counts[6] == (!importing && accepted ? length : 0), "only accepted primary payload initializes real Vulkan cache");
    CHECK(importing ? (access(imported, F_OK) == 0) == accepted : 1, "only accepted import is merged and renamed");
    CHECK((access(stale, F_OK) == 0) == !accepted, "real mismatches quarantine for both load paths");
    if (accepted && (importing || variant == 3)) {
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0 && pcache_read_exact(fd, &h, sizeof h, 0) && !memcmp(h.magic, "TOTKPC01", 8),
              "accepted legacy primary and import persist an envelope without discarding entries");
        if (fd >= 0) close(fd);
    }
    vk_pcache_shutdown();
    printf("pcache context %s variant=%u: %d checks, %d failed\n", importing ? "import" : "primary", variant, checks, failures);
    return failures != 0;
}
int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--context")) return real_context_case((unsigned)atoi(argv[2]));
    int reload = argc > 1 && !strcmp(argv[1], "--reload");
    if (!reload) { policies(); stores(); contexts(); }
    setenv("TOTK_VK_PIPELINE_CACHE", "state/pcache-reload.bin", 1); setenv("TOTK_VK_PIPELINE_CACHE_IMPORT", "0", 1);
    setenv("TOTK_VK_PREWARM", "0", 1); setenv("TOTK_PCACHE_STATS", "1", 1);
    mkdir("state", 0700);
    char buffer[1100]; const char *path = vk_shader_cache_path("state/pcache-reload.bin", buffer, sizeof buffer);
    if (!reload) unlink(path);
    vk_pcache_background(1); /* lifecycle before device creation is safe */
    vk_pcache_background(0); mtl_init();
    uint64_t counts[7] = {0};
    if (reload) {
        vk_pcache_flush(); vk_pcache_test_stats(counts);
        struct stat st;
        pcache_identity h; int fd = open(path, O_RDONLY);
        int header_ok = fd >= 0 && pcache_read_exact(fd, &h, sizeof h, 0) && !memcmp(h.magic, "TOTKPC01", 8);
        if (fd >= 0) close(fd);
        CHECK(header_ok && !stat(path, &st) && st.st_size >= 32 &&
              counts[6] + pcache_identity_size(&h) == (uint64_t)st.st_size,
              "next process loaded opaque bytes from matching persisted envelope");
        real_query = vkGetPipelineCacheData;
        size_t actual = 0; VKCHECK(real_query(V.dev, V.pcache, &actual, NULL));
        if (counts[6] > actual) atomic_store(&padding, counts[6] - actual);
        vkGetPipelineCacheData = held_query;
        vk_pcache_changed(); vk_pcache_shutdown(); vk_pcache_test_stats(counts);
        CHECK(counts[0] == 0 && counts[1] == counts[2] && counts[5] == 0 && !atomic_load(&data_queries),
              "shutdown acknowledges unchanged size without payload extraction or a write");
        printf("reload: %d checks, %d failed\n", checks, failures); return failures != 0;
    }
    real_query = vkGetPipelineCacheData; vkGetPipelineCacheData = held_query;
    VkShaderModule module; VkPipelineLayout layout;
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof spirv, .pCode = spirv};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VKCHECK(vkCreateShaderModule(V.dev, &sm, NULL, &module)); VKCHECK(vkCreatePipelineLayout(V.dev, &pl, NULL, &layout));
    ci = (VkComputePipelineCreateInfo){VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = layout};
    pthread_mutex_lock(&hold_mu); hold_data = 1; pthread_mutex_unlock(&hold_mu);
    vk_pcache_changed(); pthread_t flusher; pthread_create(&flusher, NULL, flush, NULL);
    pthread_mutex_lock(&hold_mu);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 10;
    while (!entered && !pthread_cond_timedwait(&hold_cv, &hold_mu, &deadline)) {}
    int observed = entered; pthread_mutex_unlock(&hold_mu); CHECK(observed, "single saver reaches held extraction");
    pthread_t creators[2];
    for (int i = 0; i < 2; i++) pthread_create(&creators[i], NULL, create_pipelines, NULL);
    for (int i = 0; i < 2; i++) pthread_join(creators[i], NULL);
    CHECK(!atomic_load(&creation_failed), "two creators complete real pipelines while extraction is in progress (no coarse host cache lock)");
    vk_rec_lock(); usleep(1000); vk_rec_unlock(); /* exercises recording overlap diagnostics */
    pthread_mutex_lock(&hold_mu); release_data = 1; pthread_cond_broadcast(&hold_cv); pthread_mutex_unlock(&hold_mu);
    pthread_join(flusher, NULL); vk_pcache_test_stats(counts);
    CHECK(counts[0] == 1 && counts[2] < 65, "held snapshot acknowledges only generation sampled before concurrent creates");
    uint64_t written = counts[5]; unsigned extracted = atomic_load(&data_queries);
    vk_pcache_flush(); vk_pcache_test_stats(counts);
    CHECK(counts[0] == 1 && counts[1] == counts[2] && counts[5] == written && atomic_load(&data_queries) == extracted,
          "cache-hit generations with unchanged size are acknowledged without another extraction/write");
    vk_pcache_changed(); vk_pcache_background(1); vk_pcache_flush(); vk_pcache_background(0); vk_pcache_test_stats(counts);
    CHECK(counts[0] == 1 && counts[1] == counts[2] && counts[5] == written && atomic_load(&data_queries) == extracted,
          "background/lifecycle flush also skips unchanged size");
    for (int phase = 1; phase <= 2; phase++) {
        uint64_t baseline = counts[2]; unsigned probes = atomic_load(&size_queries);
        pthread_mutex_lock(&hold_mu); hold_size_phase = phase; size_entered = release_size = 0; pthread_mutex_unlock(&hold_mu);
        vk_pcache_changed(); pthread_create(&flusher, NULL, flush, NULL);
        pthread_mutex_lock(&hold_mu); clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 10;
        while (!size_entered && !pthread_cond_timedwait(&hold_cv, &hold_mu, &deadline)) {}
        observed = size_entered == phase; pthread_mutex_unlock(&hold_mu);
        CHECK(observed, "unchanged-size check reaches deterministic before/after-probe interleaving");
        VkPipeline concurrent = VK_NULL_HANDLE;
        CHECK(vk_pcache_compute(&ci, &concurrent) == VK_SUCCESS, "cache-hit create completes at held size-probe boundary");
        if (concurrent) vkDestroyPipeline(V.dev, concurrent, NULL);
        pthread_mutex_lock(&hold_mu); release_size = 1; pthread_cond_broadcast(&hold_cv); pthread_mutex_unlock(&hold_mu);
        pthread_join(flusher, NULL); vk_pcache_test_stats(counts);
        CHECK(counts[2] == baseline + 1 && counts[0] == 1 && counts[5] == written,
              "unchanged-size acknowledgement saves exactly the generation sampled BEFORE the probe, not the concurrent create");
        vk_pcache_flush(); vk_pcache_test_stats(counts);
        CHECK(counts[2] == baseline + 2 && counts[1] == counts[2] && atomic_load(&size_queries) == probes + 2,
              "concurrent generation remains dirty and requires its own subsequent size probe");
        CHECK(counts[0] == 1 && counts[5] == written && atomic_load(&data_queries) == extracted,
              "both interleaved unchanged-size probes avoid payload extraction and writes");
    }
    atomic_fetch_add(&padding, 1); vk_pcache_changed(); vk_pcache_flush(); vk_pcache_test_stats(counts);
    CHECK(counts[0] == 2 && counts[5] > written && atomic_load(&data_queries) == extracted + 1,
          "actual queried size growth takes normal snapshot path");
    CHECK(counts[4] == 1, "growing real Vulkan snapshots reuse scratch allocation");
    atomic_fetch_add(&padding, 1);
    for (int i = 0; i < 2; i++) pthread_create(&creators[i], NULL, create_pipelines, NULL);
    pthread_create(&flusher, NULL, lifecycle, NULL);
    for (int i = 0; i < 2; i++) pthread_join(creators[i], NULL);
    pthread_join(flusher, NULL); vk_pcache_flush(); vk_pcache_test_stats(counts);
    CHECK(!atomic_load(&creation_failed) && counts[1] == counts[2], "concurrent real extraction, creators, background/resume and flushes preserve all completed learning");
    atomic_fetch_add(&padding, 1); vk_pcache_changed();
    pthread_t closers[2];
    for (int i = 0; i < 2; i++) pthread_create(&closers[i], NULL, shutdown_thread, NULL);
    for (int i = 0; i < 2; i++) pthread_join(closers[i], NULL);
    uint64_t after[7]; vk_pcache_test_stats(after);
    CHECK(after[0] > counts[0] && after[1] == after[2], "concurrent shutdown calls perform one final low-growth flush and join scratch owner");
    vk_pcache_flush(); vk_pcache_shutdown(); vk_pcache_background(1); /* safe after shutdown */
    vkDestroyPipelineLayout(V.dev, layout, NULL); vkDestroyShaderModule(V.dev, module, NULL);
    pid_t child = fork();
    if (!child) { execl(argv[0], argv[0], "--reload", NULL); _exit(127); }
    int status = 0; CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status), "persisted cache reloads in a fresh process");
    for (unsigned mode = 0; mode < 10; mode++) {
        char text[16]; snprintf(text, sizeof text, "%u", mode);
        child = fork();
        if (!child) { execl(argv[0], argv[0], "--context", text, NULL); _exit(127); }
        CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status),
              "real primary/import match, TU_DEBUG/driver mismatch and legacy compatibility in fresh process");
    }
    printf("pcache: %d checks, %d failed\n", checks, failures); return failures != 0;
}
