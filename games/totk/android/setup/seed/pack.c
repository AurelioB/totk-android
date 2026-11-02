/* scripts/pack_shader_seed.py pack_seed() in C: every immediate regular .spv/.vk/.vv file of the loose seed, in sorted
   basename order, into one TOTKVKP1 file: a 32-byte <8sIIQQ> header (magic, version 1, record size 104, count, total
   size), sorted <84sIQQ> records (NUL-padded basename, reserved 0, absolute offset, length), payloads 8-byte aligned
   with zero padding, no timestamps. Written to a temporary file and renamed into place.

   On the phone the loose seed and the pack would need twice the space; payloads are removed once they are copied, in
   batches on a helper thread so the ~71k unlinks overlap the copy (a failed stage discards the staging folder anyway).
   The 1.5 GB pack is written back and dropped from the page cache in 128 MiB windows (st_cache_written), and each
   payload's cache is dropped once read, so the setup process does not
   carry a gigabyte of dirty cache towards the low-memory killer. The seed id is optimize_shader_seed.py's content
   identity: SHA-256 over every payload in name order of name, NUL, SHA-256(payload). */
#define _GNU_SOURCE 1
#include "seed_int.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#define NAME 84
typedef struct { char name[NAME]; uint64_t size, offset; } Entry;

/* Unlinks v[from, to) in dfd on its own thread; one batch is in flight while the next one is copied. */
#define UNLINK_BATCH 1024
typedef struct { int dfd; const Entry *v; size_t from, to; pthread_t t; int live; } Unlinker;
static void *unlink_main(void *arg) {
    Unlinker *u = arg;
    for (size_t i = u->from; i < u->to; i++) unlinkat(u->dfd, u->v[i].name, 0);
    return NULL;
}
static void unlink_join(Unlinker *u) { if (u->live) pthread_join(u->t, NULL); u->live = 0; }
static void unlink_start(Unlinker *u, size_t from, size_t to) {
    unlink_join(u);
    u->from = from; u->to = to;
    if (from < to && pthread_create(&u->t, NULL, unlink_main, u)) unlink_main(u); else u->live = from < to;
}
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static int by_name(const void *a, const void *b) { return strcmp(((const Entry *)a)->name, ((const Entry *)b)->name); }
static int payload_ext(const char *name, size_t n) {
    static const char *const ext[3] = { ".spv", ".vk", ".vv" };
    for (int i = 0; i < 3; i++) { size_t e = strlen(ext[i]); if (n >= e && !strcmp(name + n - e, ext[i])) return (int)e; }
    return 0;
}
/* [A-Za-z0-9_.-]+, at most 83 bytes, a non-empty stem */
static int valid_name(const char *name, size_t n, int ext) {
    if (n > 83 || (size_t)ext == n) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == '-')) return 0;
    }
    return 1;
}

typedef struct { int fd; uint8_t *buf; size_t n, cap; uint64_t written; StCache cache; } Out;
static int out_flush(Out *o) {
    if (o->n && st_write(o->fd, o->buf, o->n)) return -1;
    o->written += o->n; o->n = 0;
    st_cache_written(o->fd, &o->cache, o->written);
    return 0;
}
static int out_add(Out *o, const void *p, size_t n) {
    const uint8_t *s = p;
    while (n) {
        size_t k = o->cap - o->n < n ? o->cap - o->n : n;
        memcpy(o->buf + o->n, s, k); o->n += k; s += k; n -= k;
        if (o->n == o->cap && out_flush(o)) return -1;
    }
    return 0;
}

int seed_pack(SetupCtx *c, const char *dir, const char *pack_path, char seed_id[65], uint64_t *files, SeedErr *err) {
    Entry *v = NULL; size_t n = 0, cap = 0; int rc = -1, dfd = -1; Out o = { -1, NULL, 0, 1 << 20, 0, {0, 0, 0} };
    Unlinker gone = { .live = 0 };
    char tmp[SETUP_PATH];
    if (st_path(tmp, sizeof tmp, "%s.tmp", pack_path)) { seed_err(err, "io", "Seed path too long"); return -1; }
    DIR *d = opendir(dir);
    if (!d) { seed_err(err, "io", "Cannot open %s: %s", dir, strerror(errno)); return -1; }
    dfd = dirfd(d);
    for (struct dirent *e; (e = readdir(d));) {
        size_t len = strlen(e->d_name); int ext = payload_ext(e->d_name, len);
        if (!ext) continue;
        if (!valid_name(e->d_name, len, ext)) { seed_err(err, "internal", "invalid shader payload basename: %s", e->d_name); goto out; }
        struct stat st;
        if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) {
            seed_err(err, "internal", "shader payload must be a regular file: %s", e->d_name); goto out;
        }
        if (n == cap) { Entry *p = realloc(v, (cap = cap ? cap * 2 : 4096) * sizeof *p); if (!p) { seed_err(err, "internal", "Out of memory"); goto out; } v = p; }
        memset(v[n].name, 0, NAME); memcpy(v[n].name, e->d_name, len); v[n].size = (uint64_t)st.st_size; n++;
    }
    qsort(v, n, sizeof *v, by_name);
    uint64_t size = 32 + (uint64_t)n * 104;
    for (size_t i = 0; i < n; i++) { v[i].offset = (size + 7) & ~7ull; size = v[i].offset + v[i].size; }
    if (!(o.buf = malloc(o.cap))) { seed_err(err, "internal", "Out of memory"); goto out; }
    o.fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (o.fd < 0) { seed_err(err, "io", "Cannot create %s: %s", tmp, strerror(errno)); goto out; }
    uint8_t h[32]; memcpy(h, "TOTKVKP1", 8); put32(h + 8, 1); put32(h + 12, 104); put64(h + 16, n); put64(h + 24, size);
    if (out_add(&o, h, 32)) goto write_fail;
    for (size_t i = 0; i < n; i++) {
        uint8_t r[104] = {0};
        memcpy(r, v[i].name, NAME); put32(r + 84, 0); put64(r + 88, v[i].offset); put64(r + 96, v[i].size);
        if (out_add(&o, r, 104)) goto write_fail;
    }
    Sha256 id; sha256_init(&id);
    uint8_t *data = NULL; size_t data_cap = 0, batch = 0;
    gone.dfd = dfd; gone.v = v;
    for (size_t i = 0; i < n; i++) {
        static const uint8_t zero[8];
        if (out_add(&o, zero, (size_t)(v[i].offset - (o.written + o.n)))) { free(data); goto write_fail; }
        if (v[i].size > data_cap) {
            uint8_t *p = realloc(data, v[i].size); if (!p) { free(data); seed_err(err, "internal", "Out of memory"); goto out; }
            data = p; data_cap = v[i].size;
        }
        int fd = openat(dfd, v[i].name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != v[i].size || (v[i].size && st_pread(fd, data, v[i].size, 0))) {
            if (fd >= 0) close(fd);
            free(data); seed_err(err, "io", "shader payload changed while packing: %s", v[i].name); goto out;
        }
        st_cache_drop(fd, 0, v[i].size);
        close(fd);
        uint8_t digest[32];
        Sha256 one; sha256_init(&one); sha256_update(&one, data, v[i].size); sha256_final(&one, digest);
        sha256_update(&id, v[i].name, strlen(v[i].name) + 1); sha256_update(&id, digest, 32);
        if (out_add(&o, data, v[i].size)) { free(data); goto write_fail; }
        if (i + 1 - batch == UNLINK_BATCH || i + 1 == n) { unlink_start(&gone, batch, i + 1); batch = i + 1; }
        if (i % 256 == 0) {
            setup_progress(c, "shaders", i, n, "packing the shader seed");
            if (setup_cancelled(c)) { free(data); rc = SETUP_CANCELLED; goto out; }
        }
    }
    free(data);
    if (out_flush(&o) || fsync(o.fd)) goto write_fail;
    st_cache_drop(o.fd, 0, 0);   /* all on disk now; the runtime reads the pack later through its own index */
    close(o.fd); o.fd = -1;
    if (rename(tmp, pack_path)) { seed_err(err, "io", "Cannot publish %s: %s", pack_path, strerror(errno)); goto out; }
    uint8_t digest[32]; sha256_final(&id, digest); sha256_hex(digest, seed_id);
    *files = n;
    rc = 0;
    goto out;
write_fail:
    seed_err(err, errno == ENOSPC ? "no_space" : "io", "Cannot write %s: %s", tmp, strerror(errno));
out:
    if (o.fd >= 0) { close(o.fd); unlink(tmp); }
    unlink_join(&gone);   /* before v and the directory go */
    free(o.buf); free(v); closedir(d);
    return rc;
}
