/* Opaque Vulkan blobs stay whole. This policy/store has no Vulkan or game dependency. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define PCACHE_SECOND 1000000000ull
#define PCACHE_MIN_NS (60 * PCACHE_SECOND)
#define PCACHE_MAX_NS (120 * PCACHE_SECOND)
#define PCACHE_GROWTH (256u << 10)
#define PCACHE_PIPELINES 256u
/* All fields are saver-owned. generation is sampled before extraction: concurrent creates stay dirty. */
typedef struct { uint64_t saved_generation, dirty_since, last_save; size_t saved_size; } pcache_policy;
enum { PCACHE_CLEAN, PCACHE_UNCHANGED, PCACHE_INTERVAL, PCACHE_GROW_SMALL, PCACHE_BUSY, PCACHE_QUIET, PCACHE_DEADLINE, PCACHE_FLUSH };
/* size_probed is true only after a successful size-only query. Cache-hit creates need no payload snapshot.
 * Treat non-growing append-style caches as unchanged, retaining the actual file-size/time baseline. */
static inline int pcache_decide(pcache_policy *p, uint64_t now, uint64_t generation, size_t size, int size_probed, int quiet, int flush) {
    if (generation == p->saved_generation) return PCACHE_CLEAN;
    if (!p->dirty_since) p->dirty_since = now;
    if (size_probed && size <= p->saved_size) {
        p->saved_generation = generation; p->dirty_since = 0; return PCACHE_UNCHANGED;
    }
    if (flush) return PCACHE_FLUSH;
    if (now - p->dirty_since >= PCACHE_MAX_NS) return PCACHE_DEADLINE; /* Overrides BOTH gates. */
    if (now - p->last_save < PCACHE_MIN_NS) return PCACHE_INTERVAL;
    size_t growth = size > p->saved_size ? size - p->saved_size : 0;
    if (growth < PCACHE_GROWTH && generation - p->saved_generation < PCACHE_PIPELINES) return PCACHE_GROW_SMALL;
    return quiet ? PCACHE_QUIET : PCACHE_BUSY;
}
static inline void pcache_saved(pcache_policy *p, uint64_t now, uint64_t generation, size_t size) {
    *p = (pcache_policy){.saved_generation = generation, .saved_size = size, .last_save = now};
}
enum { PCACHE_FAIL_NONE, PCACHE_FAIL_ALLOC, PCACHE_FAIL_QUERY, PCACHE_FAIL_DATA,
       PCACHE_FAIL_OPEN, PCACHE_FAIL_WRITE, PCACHE_FAIL_FSYNC, PCACHE_FAIL_RENAME, PCACHE_FAIL_DIRSYNC };
typedef struct { void *data; size_t capacity; const void *prefix; size_t prefix_size; uint64_t allocations, bytes_written;
#ifdef TOTK_VK_TESTS
    int fail;
#endif
} pcache_store;
static inline int pcache_fault(pcache_store *s, int step) {
#ifdef TOTK_VK_TESTS
    if (s->fail == step) { errno = EIO; return 1; }
#else
    (void)s; (void)step;
#endif
    return 0;
}
/* Retain geometric headroom: no allocation or page disposal on subsequent small growth. */
static inline int pcache_reserve(pcache_store *s, size_t size) {
    if (size <= s->capacity) return 1;
    if (pcache_fault(s, PCACHE_FAIL_ALLOC) || size > SIZE_MAX - size / 8 - (1u << 20)) return 0;
    size_t capacity = size + size / 8 + (1u << 20);
    void *data = realloc(s->data, capacity); if (!data) return 0;
    s->data = data; s->capacity = capacity; s->allocations++; return 1;
}
static inline int pcache_replace(pcache_store *s, const char *path, size_t size) {
    char tmp[1200], dir[1200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp || strlen(path) >= sizeof dir) { errno = ENAMETOOLONG; return 0; }
    strcpy(dir, path); char *slash = strrchr(dir, '/');
    if (!slash) strcpy(dir, "."); else if (slash == dir) slash[1] = 0; else *slash = 0;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (dfd < 0) return 0;
    int fd = pcache_fault(s, PCACHE_FAIL_OPEN) ? -1 : open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int ok = fd >= 0;
    const void *parts[] = {s->prefix, s->data}; size_t sizes[] = {s->prefix_size, size};
    for (unsigned part = 0; ok && part < 2; part++) {
        size_t written = 0;
        while (ok && written < sizes[part]) {
            ssize_t n = pcache_fault(s, PCACHE_FAIL_WRITE) ? -1 : write(fd, (const char *)parts[part] + written, sizes[part] - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { if (!n) errno = EIO; ok = 0; } else { written += (size_t)n; s->bytes_written += (size_t)n; }
        }
    }
    if (ok) ok = !pcache_fault(s, PCACHE_FAIL_FSYNC) && !fsync(fd);
    int error = errno;
    if (fd >= 0 && close(fd) && ok) { ok = 0; error = errno; }
    if (ok) { ok = !pcache_fault(s, PCACHE_FAIL_RENAME) && !rename(tmp, path); error = errno; }
    /* After rename either old or new COMPLETE blob is visible. A directory sync failure leaves dirty state set. */
    if (ok) { ok = !pcache_fault(s, PCACHE_FAIL_DIRSYNC) && !fsync(dfd); error = errno; }
    close(dfd); if (!ok) unlink(tmp); errno = error; return ok;
}
typedef int (*pcache_query)(void *context, size_t *size, void *data);
static inline int pcache_snapshot(pcache_store *s, const char *path, pcache_query query, void *context, size_t *size) {
    *size = 0;
    if (pcache_fault(s, PCACHE_FAIL_QUERY) || query(context, size, NULL) || !*size || !pcache_reserve(s, *size)) return 0;
    *size = s->capacity;
    if (pcache_fault(s, PCACHE_FAIL_DATA) || query(context, size, s->data)) return 0; /* Includes VK_INCOMPLETE: retry later. */
    return pcache_replace(s, path, *size);
}
