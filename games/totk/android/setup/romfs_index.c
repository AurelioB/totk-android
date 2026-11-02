/* The runtime RomFS index, a C port of scripts/extract_romfs.py index_romfs (the RomFS directory / file tables walked with
   bounds, cycle and name checks, files sorted by path) and the TOTKRFS1 writer of scripts/prepare_romfs_backend.py:
   '<8sQQ' (magic, count, string offset), count x '<QQII' (absolute offset, size, name offset, name length + NUL),
   then the NUL-terminated paths. Reads only the tables, in place. */
#include "setup_internal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char *path; uint64_t offset, size; } RomfsFile;
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static int by_path(const void *a, const void *b) { return strcmp(((const RomfsFile *)a)->path, ((const RomfsFile *)b)->path); }
static int utf8_ok(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i]; size_t k = c < 0x80 ? 1 : c >= 0xc2 && c < 0xe0 ? 2 : c >= 0xe0 && c < 0xf0 ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!k || i + k > n) return 0;
        uint32_t v = k == 1 ? c : c & (k == 2 ? 0x1f : k == 3 ? 0x0f : 0x07);
        for (size_t j = 1; j < k; j++) { if ((s[i + j] & 0xc0) != 0x80) return 0; v = v << 6 | (s[i + j] & 0x3f); }
        if ((k == 3 && (v < 0x800 || (v >= 0xd800 && v < 0xe000))) || (k == 4 && (v < 0x10000 || v > 0x10ffff))) return 0;
        i += k;
    }
    return 1;
}
/* name(table, off, n) of the Python: in bounds, UTF-8, not . or .., no / \ or NUL. */
static int name_ok(const uint8_t *t, size_t len, uint64_t off, uint32_t n) {
    if (off > len || n > len - off) return 0;
    const uint8_t *s = t + off;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return 0;
    for (uint32_t i = 0; i < n; i++) if (s[i] == '/' || s[i] == '\\' || !s[i]) return 0;
    return utf8_ok(s, n);
}
static char *join(const char *prefix, const uint8_t *name, uint32_t n, int slash) {
    size_t p = strlen(prefix); char *s = malloc(p + n + 2);
    if (!s) return NULL;
    memcpy(s, prefix, p); memcpy(s + p, name, n); s[p + n] = '/'; s[p + n + slash] = 0;
    return s;
}

int setup_romfs_index(SetupCtx *c, const RomfsImage *img, const char *out_index, char index_sha[65]) {
    int rc = -1;
    uint8_t h[80];
    uint8_t *dirs = NULL, *files = NULL, *seen_d = NULL, *seen_f = NULL, *out = NULL;
    struct Todo { uint32_t off; char *parent; } *todo = NULL; size_t ntodo = 0, captodo = 0;
    char **prefixes = NULL; size_t nprefix = 0, capprefix = 0;
    RomfsFile *list = NULL; size_t nlist = 0, caplist = 0;
#define FAIL(...) do { setup_fail(c, "missing_input", __VA_ARGS__); goto out; } while (0)
#define OOM() do { setup_fail(c, "internal", "Out of memory"); goto out; } while (0)
    uint64_t length = img->size;
    if (length < 80 || romfs_image_pread(img, h, 80, 0)) FAIL("RomFS header is truncated");
    uint64_t hv[10]; for (int i = 0; i < 10; i++) hv[i] = le64(h + 8 * i);
    if (hv[0] != 80) FAIL("RomFS header size is not 80");
    for (int i = 1; i < 9; i += 2) if (hv[i] > length || hv[i + 1] > length - hv[i]) FAIL("RomFS table out of bounds");
    uint64_t dlen = hv[4], flen = hv[8], data = hv[9];
    if (dlen > (256u << 20) || flen > (256u << 20)) FAIL("RomFS tables are too large");
    dirs = malloc(dlen ? dlen : 1); files = malloc(flen ? flen : 1);
    seen_d = calloc(dlen / 8 + 1, 1); seen_f = calloc(flen / 8 + 1, 1);
    if (!dirs || !files || !seen_d || !seen_f) OOM();
    if ((dlen && romfs_image_pread(img, dirs, dlen, hv[3])) || (flen && romfs_image_pread(img, files, flen, hv[7]))) { st_io_fail(c, "read", "RomFS tables"); goto out; }
    setup_progress(c, "index", 0, 0, "reading the RomFS tables");
    captodo = 64; todo = malloc(captodo * sizeof *todo);
    capprefix = 64; prefixes = malloc(capprefix * sizeof *prefixes);
    if (!todo || !prefixes || !(prefixes[0] = strdup(""))) OOM();
    nprefix = 1; todo[ntodo++] = (struct Todo){ 0, prefixes[0] };
    while (ntodo) {
        if (setup_cancelled(c)) { rc = SETUP_CANCELLED; goto out; }
        struct Todo t = todo[--ntodo];
        if (t.off >= dlen || seen_d[t.off / 8] >> (t.off % 8) & 1 || (uint64_t)t.off + 24 > dlen) FAIL("Invalid or cyclic RomFS directory");
        seen_d[t.off / 8] |= (uint8_t)(1 << (t.off % 8));
        const uint8_t *d = dirs + t.off;
        uint32_t sibling = le32(d + 4), child = le32(d + 8), file = le32(d + 12), n = le32(d + 20);
        if (!name_ok(dirs, dlen, (uint64_t)t.off + 24, n)) FAIL("Unsafe RomFS directory name");
        char *prefix = t.parent;
        if (n) {
            if (nprefix == capprefix) { char **p = realloc(prefixes, (capprefix *= 2) * sizeof *p); if (!p) OOM(); prefixes = p; }
            if (!(prefix = prefixes[nprefix] = join(t.parent, d + 24, n, 1))) OOM();
            nprefix++;
        }
        if (ntodo + 2 > captodo) { struct Todo *p = realloc(todo, (captodo *= 2) * sizeof *p); if (!p) OOM(); todo = p; }
        if (sibling != 0xffffffffu) todo[ntodo++] = (struct Todo){ sibling, t.parent };
        if (child != 0xffffffffu) todo[ntodo++] = (struct Todo){ child, prefix };
        while (file != 0xffffffffu) {
            if (file >= flen || seen_f[file / 8] >> (file % 8) & 1 || (uint64_t)file + 32 > flen) FAIL("Invalid or cyclic RomFS file");
            seen_f[file / 8] |= (uint8_t)(1 << (file % 8));
            const uint8_t *f = files + file;
            uint32_t next = le32(f + 4), fn = le32(f + 28); uint64_t off = le64(f + 8), size = le64(f + 16);
            if (!name_ok(files, flen, (uint64_t)file + 32, fn)) FAIL("Unsafe RomFS file name");
            if (!fn) FAIL("Empty RomFS file name");
            if (data > length || off > length - data || size > length - data - off) FAIL("RomFS file out of bounds");
            if (nlist == caplist) { RomfsFile *p = realloc(list, (caplist = caplist ? caplist * 2 : 4096) * sizeof *p); if (!p) OOM(); list = p; }
            if (!(list[nlist].path = join(prefix, f + 32, fn, 0))) OOM();
            list[nlist].offset = data + off; list[nlist].size = size; nlist++;
            file = next;
        }
    }
    qsort(list, nlist, sizeof *list, by_path);
    uint64_t strings = 0;
    for (size_t i = 0; i < nlist; i++) {
        if (i && !strcmp(list[i - 1].path, list[i].path)) FAIL("Duplicate RomFS file path");
        strings += strlen(list[i].path) + 1;
    }
    uint64_t total = 24 + (uint64_t)nlist * 24 + strings;
    if (!(out = malloc(total))) OOM();
    memcpy(out, "TOTKRFS1", 8); put64(out + 8, nlist); put64(out + 16, 24 + (uint64_t)nlist * 24);
    uint64_t so = 0;
    for (size_t i = 0; i < nlist; i++) {
        uint8_t *row = out + 24 + i * 24; size_t n = strlen(list[i].path) + 1;
        put64(row, list[i].offset); put64(row + 8, list[i].size); put32(row + 16, (uint32_t)so); put32(row + 20, (uint32_t)n);
        memcpy(out + 24 + nlist * 24 + so, list[i].path, n); so += n;
    }
    sha256_buf_hex(out, total, index_sha);
    if (strcmp(index_sha, c->pins.index_sha256)) { setup_fail(c, "hash_mismatch", "The RomFS index does not match 1.4.3 (SHA-256 differs)"); goto out; }
    if (st_write_file(out_index, out, total, 0644)) { st_io_fail(c, "write", out_index); goto out; }
    setup_progress(c, "index", nlist, nlist, "RomFS index written");
    rc = 0;
out:
#undef FAIL
#undef OOM
    for (size_t i = 0; i < nlist; i++) free(list[i].path);
    for (size_t i = 0; i < nprefix; i++) free(prefixes[i]);
    free(list); free(prefixes); free(todo); free(dirs); free(files); free(seen_d); free(seen_f); free(out);
    return rc;
}

int setup_romfs_config(SetupCtx *c, const char *path, uint64_t size, const char *image_sha, const char *index_sha) {
    char text[256];
    snprintf(text, sizeof text, "ROMFS_IMAGE_SIZE %llu\nROMFS_IMAGE_SHA256 %s\nROMFS_INDEX_SHA256 %s\n", (unsigned long long)size, image_sha, index_sha);
    return st_write_file(path, text, strlen(text), 0644) ? st_io_fail(c, "write", path) : 0;
}
