/* One pinned pack descriptor, an in-memory index, and a learned exact-seed demand set.
   Startup asynchronously reads known payloads; recording consumes owned warmed RAM.
   Loose files remain necessary for live translator publication and old installations. */
#include "vk_seed_io.h"
#include "vk_seed_pack.h"
#include "vk_shader_xlate.h"
#include "nvn_hmap.h"
#include "thread_place.h"
#include "host_os.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

#define SEED_NAME 84
#define SEED_PATH_SIZE (PATH_MAX + 96)
#define REQUEST_LIMIT (128u << 20)
#define LEARN_HEADER "TOTKSEED1\n"
#define LEARN_LIMIT 16384u
enum { IO_EMPTY, IO_QUEUED, IO_READING, IO_READY };
typedef struct seed_file {
    char name[SEED_NAME];
    struct seed_file *next, *queue, *recent_prev, *recent_next;
    char *data;
    size_t size, length;
    uint64_t offset, generation;
    int packed, state, learned, warmed;
    unsigned waiters;
    size_t reserved;
} seed_file;
typedef struct seed_request {
    struct seed_request *next;
    char *directory, *json;
    char variant[80];
    size_t size;
} seed_request;
static pthread_mutex_t io_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_cv = PTHREAD_COND_INITIALIZER;
static nvn_hmap inventory = {.klen = SEED_NAME};
static seed_file *files, *files_tail, *warm_head, *warm_tail, *recent_head, *recent_tail;
static seed_request *request_head, *request_tail;
static char *seed_directory, *learn_path;
static int started, stopping, index_ready, warm_done, has_modules;
static _Atomic int stats_on;
static size_t request_bytes, warm_limit, warm_planned;
static unsigned recent_count, readers;
static int learn_dirty;
static uint64_t save_after;
static pthread_t io_thread;
static vk_seed_pack packed;
static vk_seed_io_stats stats, window;

int vk_seed_io_enabled(void) {
    const char *e = getenv("TOTK_VK_SEED_IO");
    return !e || atoi(e) != 0;
}
static int shader_name(const char *name) {
    size_t length;
    unsigned char key[SEED_NAME] = {0};
    size_t n = strnlen(name, SEED_NAME);
    if (n >= SEED_NAME) return 0;
    memcpy(key, name, n);
    return vk_seed_pack_valid_name(key, &length);
}
static seed_file *entry(const char *name, int create) { /* io_mu, except private initial inventory */
    char key[SEED_NAME] = {0};
    if (!shader_name(name)) return NULL;
    memcpy(key, name, strlen(name));
    seed_file *f = nvn_hm_get(&inventory, key);
    if (!f && create) {
        f = calloc(1, sizeof *f);
        if (!f) { fprintf(stderr, "[seed-io] out of memory building seed inventory\n"); abort(); }
        memcpy(f->name, key, sizeof key); nvn_hm_put(&inventory, key, f); stats.entries++;
        if (files_tail) files_tail->next = f; else files = f; files_tail = f;
    }
    return f;
}
#ifdef TOTK_VK_TESTS
void vk_seed_io_test_before_read(const char *name) __attribute__((weak));
unsigned vk_seed_io_test_waiters(const char *name) {
    pthread_mutex_lock(&io_mu); seed_file *f = entry(name, 0);
    unsigned n = f ? f->waiters : 0; pthread_mutex_unlock(&io_mu); return n;
}
#endif
char *vk_seed_file_read(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb"); if (!file) return NULL;
    struct stat st;
    if (fstat(fileno(file), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > SIZE_MAX - 1) { fclose(file); return NULL; }
    size_t length = (size_t)st.st_size;
    char *data = malloc(length + 1); if (!data) { fclose(file); return NULL; }
    if (fread(data, 1, length, file) != length) { free(data); fclose(file); return NULL; }
    fclose(file); data[length] = 0; if (size) *size = length; return data;
}
static int seed_path(char path[SEED_PATH_SIZE], const char *name) {
    int n = snprintf(path, SEED_PATH_SIZE, "%s/%s", seed_directory, name);
    return n >= 0 && n < SEED_PATH_SIZE;
}
static void inventory_build(void) {
    const char *use_pack = getenv("TOTK_VK_SEED_PACK");
    char path[SEED_PATH_SIZE];
    if ((!use_pack || atoi(use_pack)) && seed_path(path, "seed.vkpack")) {
        int result = vk_seed_pack_open(&packed, path);
        if (result) fprintf(stderr, "[seed-io] pack %s: %s; falling back to indexed loose seed\n", path,
            result == -2 ? "invalid/truncated/incompatible" : "missing or unreadable");
        else for (uint64_t i = 0; i < packed.count; i++) {
            const char *name; const void *data; size_t size;
            if (!vk_seed_pack_entry(&packed, i, &name, &data, &size)) continue;
            seed_file *f = entry(name, 1); if (!f) continue;
            f->packed = 1; f->offset = (const unsigned char *)data - (const unsigned char *)packed.mapping;
            f->size = size; stats.pack_entries++;
        }
    }
    /* Existing loose overlays win, in particular expanded .vv lists from live translation.
       A fresh packed deployment scans only seed.vkpack, marker and any small overlay. */
    DIR *dir = opendir(seed_directory);
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir))) {
            if (!shader_name(de->d_name)) continue;
            struct stat st;
            if (fstatat(dirfd(dir), de->d_name, &st, 0) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > SIZE_MAX - 1) continue;
            seed_file *f = entry(de->d_name, 1); if (!f) continue;
            f->packed = 0; f->size = (size_t)st.st_size;
        }
        closedir(dir);
    }
    for (seed_file *f = files; f; f = f->next) {
        size_t n = strlen(f->name);
        if (n > 4 && !strcmp(f->name + n - 4, ".spv")) has_modules = 1;
    }
}
static void queue(seed_file **head, seed_file **tail, seed_file *f) {
    f->queue = NULL; f->state = IO_QUEUED;
    if (*tail) (*tail)->queue = f; else *head = f;
    *tail = f;
}
static seed_file *pop(seed_file **head, seed_file **tail) {
    seed_file *f = *head;
    if (f) { *head = f->queue; if (!*head) *tail = NULL; f->queue = NULL; }
    return f;
}
static void release_reservation(seed_file *f) {
    warm_planned -= f->reserved; f->reserved = 0;
}
static void learned_load(void) {
    FILE *file = learn_path ? fopen(learn_path, "rb") : NULL;
    if (!file) return;
    char line[SEED_NAME + 2];
    if (!fgets(line, sizeof line, file) || strcmp(line, LEARN_HEADER)) { fclose(file); return; }
    for (unsigned row = 0; row < LEARN_LIMIT && fgets(line, sizeof line, file); row++) {
        char *end = strchr(line, '\n');
        if (!end) break; /* truncated/oversized final record is never a key */
        *end = 0; seed_file *f = entry(line, 0);
        if (!f || f->learned) continue;
        f->learned = 1; f->recent_prev = recent_tail;
        if (recent_tail) recent_tail->recent_next = f; else recent_head = f;
        recent_tail = f; recent_count++; stats.warm_keys++;
        uint64_t bytes = (uint64_t)f->size + 1;
        stats.learned_bytes = bytes > UINT64_MAX - stats.learned_bytes ? UINT64_MAX : stats.learned_bytes + bytes;
    }
    fclose(file);
}
static void warm_plan(void) { /* index sizes, not payload reads; io_mu */
    if (!warm_limit) {
        uint64_t floor = UINT64_C(64) << 20, ceiling = UINT64_C(256) << 20;
        uint64_t bytes = stats.learned_bytes;
        uint64_t wanted = bytes >= ceiling ? ceiling : bytes + bytes / 4;
        if (wanted < floor) wanted = floor;
        if (wanted > ceiling) wanted = ceiling;
        warm_limit = (size_t)((wanted + ((1u << 20) - 1)) & ~UINT64_C(1048575));
    }
    stats.warm_cap = warm_limit;
    for (seed_file *f = recent_head; f; f = f->recent_next) {
        if (f->size < warm_limit && f->size + 1 <= warm_limit - warm_planned) {
            f->reserved = f->size + 1; warm_planned += f->reserved;
            queue(&warm_head, &warm_tail, f);
        } else stats.warm_skipped++;
    }
}
static void cancel_warm(seed_file *f) { /* io_mu; demand never waits for queued warming */
    seed_file **link = &warm_head, *previous = NULL;
    while (*link && *link != f) { previous = *link; link = &(*link)->queue; }
    if (!*link) return;
    *link = f->queue; if (warm_tail == f) warm_tail = previous;
    f->queue = NULL; f->state = IO_EMPTY;
    release_reservation(f); stats.warm_bypassed++;
}
static char *payload_read(seed_file *f, size_t *length, uint64_t *reads) {
    if (!f->packed) {
        char path[SEED_PATH_SIZE];
        if (!seed_path(path, f->name)) return NULL;
        (*reads)++; return vk_seed_file_read(path, length);
    }
    /* pread, not mapping memcpy: every on-demand storage access is countable, and a
       truncated/replaced pack cannot SIGBUS the recording thread. fd pins the old inode. */
    char *data = malloc(f->size + 1); if (!data) return NULL;
    size_t at = 0;
    while (at < f->size) {
        size_t chunk = f->size - at;
        if (chunk > (1u << 20)) chunk = 1u << 20;
        (*reads)++;
        ssize_t n = pread(packed.fd, data + at, chunk, (off_t)(f->offset + at));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { free(data); return NULL; }
        at += (size_t)n;
    }
    data[at] = 0; *length = at; return data;
}
static void remember(seed_file *f) { /* io_mu; MRU order, bounded atomic snapshots on the worker */
    if (f == recent_head) return;
    if (f->learned) {
        if (f->recent_prev) f->recent_prev->recent_next = f->recent_next;
        if (f->recent_next) f->recent_next->recent_prev = f->recent_prev;
        if (recent_tail == f) recent_tail = f->recent_prev;
    } else { f->learned = 1; recent_count++; }
    f->recent_prev = NULL; f->recent_next = recent_head;
    if (recent_head) recent_head->recent_prev = f; else recent_tail = f;
    recent_head = f;
    if (recent_count > LEARN_LIMIT) {
        seed_file *old = recent_tail; recent_tail = old->recent_prev;
        recent_tail->recent_next = NULL; old->recent_prev = old->recent_next = NULL; old->learned = 0; recent_count--;
    }
    if (!learn_dirty) save_after = host_now_ns() + UINT64_C(5000000000);
    learn_dirty = 1; pthread_cond_signal(&io_cv);
}
static void learned_save(const char (*names)[SEED_NAME], unsigned count) {
    char temporary[SEED_PATH_SIZE];
    if (!learn_path || snprintf(temporary, sizeof temporary, "%s.tmp", learn_path) >= (int)sizeof temporary) return;
    FILE *file = fopen(temporary, "wb"); int ok = file != NULL;
    if (file) {
        ok = fputs(LEARN_HEADER, file) >= 0;
        for (unsigned i = 0; ok && i < count; i++) ok = fprintf(file, "%s\n", names[i]) >= 0;
        if (fflush(file) || fsync(fileno(file))) ok = 0;
        if (fclose(file)) ok = 0;
    }
    if (!ok || rename(temporary, learn_path)) {
        unlink(temporary);
        fprintf(stderr, "[seed-io] learned index save failed: %s; previous index retained\n", learn_path);
    }
}
static void request_save(seed_request *r) {
    char path[SEED_PATH_SIZE];
    int n = snprintf(path, sizeof path, "%s/%s.json", r->directory, r->variant);
    mkdir(r->directory, 0755);
    if (n >= 0 && (size_t)n < sizeof path) {
        FILE *file = fopen(path, "wb"); int ok = file && fwrite(r->json, 1, r->size, file) == r->size;
        if (file && fclose(file)) ok = 0;
        if (ok) vk_shader_xlate_enqueue(path, r->variant, seed_directory);
        else fprintf(stderr, "[seed-io] request write failed: %s: %s\n", path, strerror(errno));
    }
}
static void *seed_worker(void *unused) {
    (void)unused; totk_thread_start("totk-seed-io", NULL);
    int error = setpriority(PRIO_PROCESS, 0, 10) ? errno : 0;
#if defined(__linux__)
    cpu_set_t allowed, preferred; int placed = 0, affinity_error = 0;
    const char *source = "unavailable", *decision = "unchanged";
    if (!sched_getaffinity(0, sizeof allowed, &allowed)) {
        unsigned capacity[CPU_SETSIZE];
        totk_thread_background_select("/sys/devices/system/cpu", &allowed,
                                      &preferred, capacity, &source, &decision);
        if (CPU_EQUAL(&preferred, &allowed)) placed = 1;
        else if (!(affinity_error = sched_setaffinity(0, sizeof preferred, &preferred))) placed = 1;
        else affinity_error = errno;
    } else affinity_error = errno;
    char placement[96];
    snprintf(placement, sizeof placement, "10; capacity=%s decision=%s", source, decision);
    totk_thread_setting_log("seed-background-nice", placement, placed ? 0 : -1,
                            error ? error : affinity_error);
#else
    (void)error;
#endif
    /* Holding the mutex during initial metadata publication also serializes shutdown/startup. */
    uint64_t began = host_now_ns();
    pthread_mutex_lock(&io_mu);
    inventory_build(); learned_load(); warm_plan(); index_ready = 1;
    stats.index_ns = host_now_ns() - began;
    fprintf(stderr, "[seed-io] indexed %llu files (%llu packed), learned %llu keys learned_bytes=%llu cap=%zu mode=%s, index=%.3fms directory %s\n",
        (unsigned long long)stats.entries, (unsigned long long)stats.pack_entries,
        (unsigned long long)stats.warm_keys, (unsigned long long)stats.learned_bytes, warm_limit,
        getenv("TOTK_SEED_WARM_MB") && *getenv("TOTK_SEED_WARM_MB") ? "override" : "adaptive", stats.index_ns / 1e6, seed_directory);
    pthread_cond_broadcast(&io_cv);
    save_after = host_now_ns() + UINT64_C(5000000000);
    for (;;) {
        seed_file *f = pop(&warm_head, &warm_tail);
        if (!f && !warm_done) {
            warm_done = 1;
            stats.warm_ns = host_now_ns() - began - stats.index_ns;
            fprintf(stderr, "[seed-io] warm complete keys=%llu ready=%llu bytes=%llu cap=%zu skipped=%llu bypassed=%llu failed=%llu async=%.3fms\n",
                (unsigned long long)stats.warm_keys, (unsigned long long)stats.warm_ready,
                (unsigned long long)stats.warm_bytes, warm_limit, (unsigned long long)stats.warm_skipped,
                (unsigned long long)stats.warm_bypassed, (unsigned long long)stats.warm_failed, stats.warm_ns / 1e6);
            pthread_cond_broadcast(&io_cv);
        }
        if (stopping && !f && !learn_dirty && !request_head) break;
        if (f) {
            f->state = IO_READING; f->warmed = 1; uint64_t generation = f->generation;
            /* Publication changes metadata only while io_mu is held. Snapshot it for IO. */
            seed_file source = *f; size_t length = source.size; uint64_t storage_reads = 0;
            pthread_mutex_unlock(&io_mu);
#ifdef TOTK_VK_TESTS
            if (vk_seed_io_test_before_read) vk_seed_io_test_before_read(source.name);
#endif
            char *data = payload_read(&source, &length, &storage_reads);
            pthread_mutex_lock(&io_mu);
            if (stats_on) { stats.worker_reads++; stats.worker_bytes += data ? length : 0; }
            if (generation != f->generation) {
                free(data); queue(&warm_head, &warm_tail, f);
            } else {
                f->data = data; f->length = data ? length : 0; f->state = IO_READY;
                f->warmed = data != NULL; stats.cached_bytes += f->length;
                if (!data) release_reservation(f);
                if (data) { stats.warm_ready++; stats.warm_bytes += length + 1; } else stats.warm_failed++;
            }
            pthread_cond_broadcast(&io_cv); continue;
        }
        if (learn_dirty && (stopping || host_now_ns() >= save_after)) {
            unsigned count = recent_count;
            char (*names)[SEED_NAME] = malloc((size_t)count * SEED_NAME);
            if (names) {
                unsigned i = 0;
                for (seed_file *l = recent_head; l; l = l->recent_next) memcpy(names[i++], l->name, SEED_NAME);
                learn_dirty = 0;
                pthread_mutex_unlock(&io_mu); learned_save(names, count); free(names);
                pthread_mutex_lock(&io_mu); continue;
            }
            learn_dirty = 0; /* no spin on allocation failure; a later use schedules another snapshot */
        }
        if (request_head) {
            seed_request *r = request_head; request_head = r->next; if (!request_head) request_tail = NULL;
            pthread_mutex_unlock(&io_mu); request_save(r);
            pthread_mutex_lock(&io_mu); request_bytes -= r->size;
            free(r->json); free(r->directory); free(r); pthread_cond_broadcast(&io_cv); continue;
        }
        if (learn_dirty) {
            struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec++;
            pthread_cond_timedwait(&io_cv, &io_mu, &until);
        } else pthread_cond_wait(&io_cv, &io_mu);
    }
    pthread_mutex_unlock(&io_mu);
    return NULL;
}
void vk_seed_io_start(const char *directory, const char *learned_index) {
    if (!vk_seed_io_enabled()) return;
    pthread_mutex_lock(&io_mu);
    if (!started && !stopping) {
        seed_directory = strdup(directory); learn_path = learned_index ? strdup(learned_index) : NULL;
        const char *e = getenv("TOTK_SEED_STATS"); stats_on = e && atoi(e) != 0;
        e = getenv("TOTK_SEED_WARM_MB"); long mb = e && *e ? strtol(e, NULL, 10) : 0;
        warm_limit = e && *e ? (size_t)(mb < 1 ? 1 : mb > 512 ? 512 : mb) << 20 : 0;
        int error = seed_directory && (!learned_index || learn_path) ? pthread_create(&io_thread, NULL, seed_worker, NULL) : ENOMEM;
        if (error) {
            fprintf(stderr, "[seed-io] worker unavailable (%s); original inline seed IO retained\n", strerror(error));
            free(seed_directory); free(learn_path); seed_directory = learn_path = NULL;
        } else { started = 1; atexit(vk_seed_io_stop); }
    }
    pthread_mutex_unlock(&io_mu);
}
int vk_seed_io_active(void) {
    pthread_mutex_lock(&io_mu); int active = started && !stopping; pthread_mutex_unlock(&io_mu); return active;
}
int vk_seed_io_ready(void) {
    pthread_mutex_lock(&io_mu);
    while (started && !stopping && !index_ready) pthread_cond_wait(&io_cv, &io_mu);
    int ready = started && !stopping && has_modules; pthread_mutex_unlock(&io_mu); return ready;
}
char *vk_seed_io_read(const char *name, size_t *size) {
    uint64_t began = stats_on ? host_now_ns() : 0;
    pthread_mutex_lock(&io_mu);
    readers++;
    while (started && !stopping && !index_ready) pthread_cond_wait(&io_cv, &io_mu);
    if (!started || stopping) { readers--; pthread_cond_broadcast(&io_cv); pthread_mutex_unlock(&io_mu); return NULL; }
    seed_file *f = entry(name, 0); char *data = NULL; size_t length = 0;
    if (stats_on) { stats.lookups++; window.lookups++; }
    if (!f) goto done;
    if (stats_on && f->packed) { stats.pack_hits++; window.pack_hits++; }
    int was_warmed = f->state == IO_READY && f->warmed;
    int joined_warm = f->state == IO_READING && f->warmed;
    f->waiters++;
    while (!stopping && f->state != IO_READY) {
        if (f->state == IO_QUEUED) cancel_warm(f);
        if (f->state == IO_EMPTY) {
            /* Pin lifetime with readers, snapshot publication metadata, and read
               immediately. Unrelated worker IO/fsync cannot delay cold demand. */
            f->state = IO_READING; f->warmed = 0;
            seed_file source = *f; size_t n = source.size; uint64_t reads = 0;
            pthread_mutex_unlock(&io_mu);
#ifdef TOTK_VK_TESTS
            if (vk_seed_io_test_before_read) vk_seed_io_test_before_read(source.name);
#endif
            char *bytes = payload_read(&source, &n, &reads);
            pthread_mutex_lock(&io_mu);
            if (stats_on) {
                stats.demand_reads += reads; window.demand_reads += reads;
                stats.demand_bytes += bytes ? n : 0; window.demand_bytes += bytes ? n : 0;
            }
            if (source.generation != f->generation) { free(bytes); f->state = IO_EMPTY; }
            else {
                f->data = bytes; f->length = bytes ? n : 0; f->state = IO_READY;
                stats.cached_bytes += f->length;
            }
            pthread_cond_broadcast(&io_cv);
        } else pthread_cond_wait(&io_cv, &io_mu); /* join one actual in-flight read */
    }
    f->waiters--;
    if (stopping) goto done;
    length = f->length;
    /* All concurrent waiters consume this one completed read. Parsers require
       mutable owned bytes, so copy only when another waiter still needs them. */
    if (f->waiters) {
        if (f->data) {
            data = malloc(length + 1);
            if (data) memcpy(data, f->data, length + 1);
        }
    } else {
        data = f->data; f->data = NULL; stats.cached_bytes -= f->length;
        f->length = 0; f->state = IO_EMPTY; f->warmed = 0;
        release_reservation(f);
    }
    if (stats_on && was_warmed) { stats.warmed_hits++; window.warmed_hits++; }
    if (stats_on && joined_warm) { stats.warm_joins++; window.warm_joins++; }
    if (data) { if (size) *size = length; remember(f); }
done:
    if (stats_on) {
        uint64_t elapsed = host_now_ns() - began;
        stats.blocking_ns += elapsed; window.blocking_ns += elapsed;
        if (elapsed > stats.blocking_max_ns) stats.blocking_max_ns = elapsed;
        if (elapsed > window.blocking_max_ns) window.blocking_max_ns = elapsed;
    }
    readers--; pthread_cond_broadcast(&io_cv);
    pthread_mutex_unlock(&io_mu); return data;
}
int vk_seed_io_request(const char *directory, const char *variant, char *json, size_t size) {
    seed_request *r = calloc(1, sizeof *r); if (!r) return 0;
    r->directory = strdup(directory); if (!r->directory) { free(r); return 0; }
    snprintf(r->variant, sizeof r->variant, "%s", variant); r->json = json; r->size = size;
    pthread_mutex_lock(&io_mu);
    while (started && !stopping && request_bytes && (size > REQUEST_LIMIT || request_bytes > REQUEST_LIMIT - size)) pthread_cond_wait(&io_cv, &io_mu);
    if (!started || stopping) { pthread_mutex_unlock(&io_mu); free(r->directory); free(r); return 0; }
    request_bytes += size;
    if (request_tail) request_tail->next = r; else request_head = r; request_tail = r;
    pthread_cond_signal(&io_cv); pthread_mutex_unlock(&io_mu); return 1;
}
void vk_seed_io_published(const char *variant) {
    pthread_mutex_lock(&io_mu);
    while (started && !stopping && !index_ready) pthread_cond_wait(&io_cv, &io_mu);
    if (!started || stopping) { pthread_mutex_unlock(&io_mu); return; }
    const char *ext[] = {"spv", "vk", "vv"};
    for (unsigned i = 0; i < 3; i++) {
        char name[SEED_NAME]; snprintf(name, sizeof name, i == 2 ? "%.64s.%s" : "%s.%s", variant, ext[i]);
        char path[SEED_PATH_SIZE]; struct stat st;
        if (!seed_path(path, name) || stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > SIZE_MAX - 1) continue;
        seed_file *f = entry(name, 1); if (!f) continue;
        f->generation++; f->packed = 0; f->size = (size_t)st.st_size; f->warmed = 0;
        if (f->state == IO_READY) {
            stats.cached_bytes -= f->length; free(f->data); f->data = NULL;
            f->length = 0; f->state = IO_EMPTY; release_reservation(f);
        }
    }
    pthread_cond_broadcast(&io_cv); pthread_mutex_unlock(&io_mu);
}
void vk_seed_io_stats_get(vk_seed_io_stats *out) {
    pthread_mutex_lock(&io_mu);
    while (started && !stopping && !index_ready) pthread_cond_wait(&io_cv, &io_mu);
    *out = stats; pthread_mutex_unlock(&io_mu);
}
void vk_seed_io_report(void) {
    pthread_mutex_lock(&io_mu);
    if (started && stats_on) {
        fprintf(stderr, "[seed-stats] 300-present lookups=%llu pack_hits=%llu warmed_hits=%llu demand_storage_reads=%llu demand_bytes=%llu warmed_storage_reads=%llu block_total=%.3fms block_max=%.3fms worker_reads=%llu worker_bytes=%llu cached=%llu warm_bytes=%llu cap=%zu skipped=%llu warm_joins=%llu learned_bytes=%llu warm_bypassed=%llu\n",
            (unsigned long long)window.lookups, (unsigned long long)window.pack_hits, (unsigned long long)window.warmed_hits,
            (unsigned long long)window.demand_reads, (unsigned long long)window.demand_bytes,
            (unsigned long long)window.warmed_storage_reads, window.blocking_ns / 1e6, window.blocking_max_ns / 1e6,
            (unsigned long long)stats.worker_reads, (unsigned long long)stats.worker_bytes, (unsigned long long)stats.cached_bytes,
            (unsigned long long)stats.warm_bytes, warm_limit, (unsigned long long)stats.warm_skipped,
            (unsigned long long)window.warm_joins, (unsigned long long)stats.learned_bytes,
            (unsigned long long)stats.warm_bypassed);
        memset(&window, 0, sizeof window);
    }
    pthread_mutex_unlock(&io_mu);
}
void vk_seed_io_stop(void) {
    pthread_mutex_lock(&io_mu);
    if (!started || stopping) { pthread_mutex_unlock(&io_mu); return; }
    stopping = 1; pthread_cond_broadcast(&io_cv); pthread_mutex_unlock(&io_mu);
    pthread_join(io_thread, NULL);
    pthread_mutex_lock(&io_mu);
    while (readers) pthread_cond_wait(&io_cv, &io_mu);
    /* Readers either own detached copies or observe stopping under the same mutex. */
    while (files) { seed_file *next = files->next; free(files->data); free(files); files = next; }
    free(inventory.keys); free(inventory.vals); inventory = (nvn_hmap){.klen = SEED_NAME};
    vk_seed_pack_close(&packed); free(seed_directory); free(learn_path); seed_directory = learn_path = NULL;
    files_tail = warm_head = warm_tail = recent_head = recent_tail = NULL;
    recent_count = learn_dirty = 0; warm_planned = request_bytes = 0;
    started = stopping = index_ready = warm_done = has_modules = 0;
    stats = window = (vk_seed_io_stats){0}; pthread_mutex_unlock(&io_mu);
}
