/* App-owned context around opaque Vulkan bytes. APK updates do not invalidate shader-content keys. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../runtime/host_os.h"
/* Hash the actual private copy selected by the loader, not the shared source or driver name. */
static inline int pcache_driver_hash(const char *path, uint8_t out[32]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    host_sha256_ctx sha; host_sha256_init(&sha);
    uint8_t bytes[65536]; ssize_t n;
    for (;;) {
        n = read(fd, bytes, sizeof bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        host_sha256_update(&sha, bytes, (size_t)n);
    }
    int error = errno; close(fd); errno = error;
    if (n < 0) return 0;
    host_sha256_final(&sha, out); return 1;
}
#define PCACHE_BLOB_MAX (256u << 20)
typedef struct {
    uint8_t magic[8];
    uint32_t version, debug_size;
    uint8_t driver_sha256[32];
    uint32_t custom_driver, reserved;
} pcache_identity;
_Static_assert(sizeof(pcache_identity) == 56, "stable cache envelope layout");
static inline size_t pcache_identity_size(const pcache_identity *id) { return sizeof *id + id->debug_size; }
static inline pcache_identity *pcache_identity_create(const char *debug, const uint8_t driver[32], int custom) {
    if (!debug) debug = "";
    size_t n = strlen(debug) + 1;
    if (n > UINT32_MAX || n > SIZE_MAX - sizeof(pcache_identity)) { errno = EOVERFLOW; return NULL; }
    pcache_identity *id = calloc(1, sizeof *id + n);
    if (!id) return NULL;
    memcpy(id->magic, "TOTKPC01", 8); id->version = 1; id->debug_size = (uint32_t)n;
    memcpy(id->driver_sha256, driver, 32); id->custom_driver = !!custom;
    memcpy(id + 1, debug, n); return id;
}
enum { PCACHE_READ_NONE, PCACHE_READ_MATCH, PCACHE_READ_LEGACY, PCACHE_READ_IO,
       PCACHE_READ_FORMAT, PCACHE_READ_DEBUG, PCACHE_READ_DRIVER, PCACHE_READ_LEGACY_DEBUG,
       PCACHE_READ_VULKAN };
static inline int pcache_read_exact(int fd, void *data, size_t size, off_t offset) {
    size_t got = 0;
    while (got < size) {
        ssize_t n = pread(fd, (char *)data + got, size - got, offset + (off_t)got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        got += (size_t)n;
    }
    return 1;
}
/* Legacy inference: an empty TU_DEBUG and matching Vulkan UUID are all the old format can prove.
 * Turnip includes its driver build-id in that UUID, but legacy files cannot prove the whole .so hash
 * or their historical TU_DEBUG. Nonempty current flags therefore require a new, recorded context. */
static inline void *pcache_read_context(const char *path, const pcache_identity *id, uint32_t vendor,
                                       uint32_t device, const uint8_t uuid[16], size_t *size, int *status) {
    *size = 0; *status = PCACHE_READ_NONE;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (errno != ENOENT) *status = PCACHE_READ_IO; return NULL; }
    struct stat st; uint8_t magic[8]; void *data = NULL; size_t offset = 0;
    *status = PCACHE_READ_FORMAT;
    if (fstat(fd, &st) || st.st_size < 32 || st.st_size > PCACHE_BLOB_MAX || !pcache_read_exact(fd, magic, 8, 0)) goto done;
    int wrapped = !memcmp(magic, "TOTKPC01", 8);
    if (wrapped) {
        pcache_identity h;
        if (!pcache_read_exact(fd, &h, sizeof h, 0) || h.version != 1 || h.reserved || !h.debug_size ||
            h.custom_driver > 1 || h.debug_size > (uint64_t)st.st_size - sizeof h ||
            (uint64_t)sizeof h + h.debug_size + 32 > (uint64_t)st.st_size) goto done;
        if (h.custom_driver != id->custom_driver || memcmp(h.driver_sha256, id->driver_sha256, 32)) {
            *status = PCACHE_READ_DRIVER; goto done;
        }
        if (h.debug_size != id->debug_size) { *status = PCACHE_READ_DEBUG; goto done; }
        offset = pcache_identity_size(id);
        uint8_t part[256];
        for (size_t at = sizeof h; at < offset;) {
            size_t n = offset - at < sizeof part ? offset - at : sizeof part;
            if (!pcache_read_exact(fd, part, n, (off_t)at)) goto done;
            if (memcmp(part, (const char *)id + at, n)) { *status = PCACHE_READ_DEBUG; goto done; }
            at += n;
        }
    } else if (id->debug_size != 1) { *status = PCACHE_READ_LEGACY_DEBUG; goto done; }
    size_t n = (size_t)st.st_size - offset;
    uint32_t h[8];
    if (!pcache_read_exact(fd, h, sizeof h, (off_t)offset)) goto done;
    if (h[0] < 32 || h[0] > n || h[1] != 1 || h[2] != vendor || h[3] != device || memcmp(h + 4, uuid, 16)) {
        *status = PCACHE_READ_VULKAN; goto done;
    }
    data = malloc(n);
    if (!data || !pcache_read_exact(fd, data, n, (off_t)offset)) {
        free(data); data = NULL; *status = PCACHE_READ_IO; goto done;
    }
    *size = n; *status = wrapped ? PCACHE_READ_MATCH : PCACHE_READ_LEGACY;
done:
    close(fd); return data;
}
/* Startup is the only owner before the saver starts. Preserve the FIRST quarantine: a later
 * mismatch must never overwrite the user's potentially last working .stale. Fresh saves replace
 * only path; if .stale exists, leave the rejected path in place until that atomic replacement. */
static inline int pcache_quarantine(const char *path) {
    char stale[1200]; struct stat st;
    if (snprintf(stale, sizeof stale, "%s.stale", path) >= (int)sizeof stale) { errno = ENAMETOOLONG; return -1; }
    if (!lstat(stale, &st)) return 0;
    if (errno != ENOENT) return -1;
    return rename(path, stale) ? -1 : 1;
}
