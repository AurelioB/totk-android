/* RomFS access for the shader seed: the runtime index (TOTKRFS1: '<8sQQ' magic, count, string offset; count x '<QQII'
   absolute offset, size, name offset, name length + NUL; sorted paths) and pread on the image (romfs.bin or its parts,
   RomfsImage), as runtime/romfs.c reads them; prepare_shader_seed.py decoder() (Zstandard with the dictionaries of
   Pack/ZsDic.pack.zs) and sarc(). */
#define _GNU_SOURCE 1
#include "seed_int.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define ZSTD_STATIC_LINKING_ONLY 1
#include <zstd.h>

void seed_err(SeedErr *e, const char *code, const char *fmt, ...) {
    pthread_mutex_lock(&e->mu);
    if (!e->set) {
        e->set = 1;
        snprintf(e->code, sizeof e->code, "%s", code);
        va_list ap; va_start(ap, fmt); vsnprintf(e->msg, sizeof e->msg, fmt, ap); va_end(ap);
    }
    pthread_mutex_unlock(&e->mu);
}
int seed_err_set(SeedErr *e) { pthread_mutex_lock(&e->mu); int s = e->set; pthread_mutex_unlock(&e->mu); return s; }

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint16_t rd16(const uint8_t *p, int big) { return big ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p, int big) { return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3] : le32(p); }

/* ---- index ---- */
int seed_romfs_open(SeedRomfs *r, const char *dir, SeedErr *err) {
    char path[SETUP_PATH];
    memset(r, 0, sizeof *r);
    if (st_path(path, sizeof path, "%s/runtime-index.bin", dir)) { seed_err(err, "io", "RomFS path too long"); return -1; }
    char *text; size_t n;
    if (st_read_file(path, &text, &n, 256u << 20)) { seed_err(err, "missing_input", "Cannot read %s: %s", path, strerror(errno)); return -1; }
    r->index = (uint8_t *)text; r->index_size = n;
    if (n < 24 || memcmp(text, "TOTKRFS1", 8)) goto bad;
    r->count = le64(r->index + 8);
    uint64_t strings = le64(r->index + 16);
    if (r->count > (n - 24) / 24 || strings != 24 + r->count * 24) goto bad;
    for (uint64_t i = 0; i < r->count; i++) {
        const uint8_t *row = r->index + 24 + i * 24;
        uint64_t so = le32(row + 16), sl = le32(row + 20);
        if (!sl || so > n - strings || sl > n - strings - so || r->index[strings + so + sl - 1] || strlen(seed_romfs_name(r, i)) != sl - 1) goto bad;
        if (i && strcmp(seed_romfs_name(r, i - 1), seed_romfs_name(r, i)) >= 0) goto bad;
    }
    if (romfs_image_open(&r->img, dir)) { seed_err(err, "missing_input", "Cannot open the RomFS image in %s: %s", dir, strerror(errno)); return -1; }
    r->opened = 1; r->image_size = r->img.size;
    for (uint64_t i = 0; i < r->count; i++) {
        uint64_t off, size; seed_romfs_entry(r, i, &off, &size);
        if (off > r->image_size || size > r->image_size - off) goto bad;
    }
    return 0;
bad:
    seed_err(err, "missing_input", "The RomFS index %s/runtime-index.bin is damaged; import the game again", dir);
    return -1;
}
void seed_romfs_close(SeedRomfs *r) { if (r->opened) romfs_image_close(&r->img); free(r->index); memset(r, 0, sizeof *r); }
const char *seed_romfs_name(const SeedRomfs *r, uint64_t i) {
    const uint8_t *row = r->index + 24 + i * 24;
    return (const char *)r->index + 24 + r->count * 24 + le32(row + 16);
}
void seed_romfs_entry(const SeedRomfs *r, uint64_t i, uint64_t *offset, uint64_t *size) {
    const uint8_t *row = r->index + 24 + i * 24; *offset = le64(row); *size = le64(row + 8);
}
int64_t seed_romfs_find(const SeedRomfs *r, const char *path) {
    uint64_t lo = 0, hi = r->count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2; int c = strcmp(seed_romfs_name(r, mid), path);
        if (!c) return (int64_t)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}
int seed_romfs_read(const SeedRomfs *r, uint64_t i, uint8_t **out, SeedErr *err) {
    uint64_t off, size; seed_romfs_entry(r, i, &off, &size);
    if (size > SIZE_MAX - 1 || !(*out = malloc(size ? size : 1))) { seed_err(err, "internal", "Out of memory reading %s", seed_romfs_name(r, i)); return -1; }
    if (romfs_image_pread(&r->img, *out, size, off)) {
        free(*out); *out = NULL;
        seed_err(err, "io", "Cannot read %s from romfs.bin: %s", seed_romfs_name(r, i), strerror(errno)); return -1;
    }
    romfs_image_drop(&r->img, off, size);   /* each file is read once; keep 17 GB of reads from piling up as cache */
    return 0;
}

/* ---- zstd ---- */
#define SEED_MAX_DICTS 16
struct SeedDicts { ZSTD_DDict *dict[SEED_MAX_DICTS]; unsigned id[SEED_MAX_DICTS]; int n; };
struct SeedDecoder { const SeedDicts *dicts; ZSTD_DCtx *dctx; };
static const uint8_t ZSTD_MAGIC[4] = {0x28, 0xb5, 0x2f, 0xfd};
int seed_is_zstd(const uint8_t *p, size_t n) { return n >= 4 && !memcmp(p, ZSTD_MAGIC, 4); }

/* get_frame_info(): the first frame's content size must be known and at most 512 MiB. The buffer holds every frame
   (decompress() decodes concatenated frames), so it is the sum of all frames' sizes. */
int seed_zstd_size(const uint8_t *raw, size_t n, uint64_t *size, SeedErr *err) {
    unsigned long long first = ZSTD_getFrameContentSize(raw, n);
    if (first == ZSTD_CONTENTSIZE_ERROR) { seed_err(err, "internal", "Invalid Zstandard frame"); return -1; }
    if (first == ZSTD_CONTENTSIZE_UNKNOWN || first > SEED_MAX_DECODED) { seed_err(err, "internal", "unbounded shader package"); return -1; }
    unsigned long long all = ZSTD_findDecompressedSize(raw, n);
    if (all == ZSTD_CONTENTSIZE_ERROR || all == ZSTD_CONTENTSIZE_UNKNOWN || all > (8ull << 30)) { seed_err(err, "internal", "unbounded shader package"); return -1; }
    *size = all;
    return 0;
}
int seed_decode(SeedDecoder *d, const uint8_t *raw, size_t n, uint8_t **out, size_t *out_n, SeedErr *err) {
    *out = NULL; *out_n = 0;
    if (!seed_is_zstd(raw, n)) return 0;
    uint64_t size;
    if (seed_zstd_size(raw, n, &size, err)) return -1;
    unsigned id = ZSTD_getDictID_fromFrame(raw, n);
    const ZSTD_DDict *dict = NULL;
    if (id) {
        for (int i = 0; i < d->dicts->n && !dict; i++) if (d->dicts->id[i] == id) dict = d->dicts->dict[i];
        if (!dict) { seed_err(err, "internal", "Zstandard frame with unknown dictionary %u", id); return -1; }
    }
    uint8_t *buf = malloc(size ? size : 1);
    if (!buf) { seed_err(err, "internal", "Out of memory (%llu bytes)", (unsigned long long)size); return -1; }
    size_t got = dict ? ZSTD_decompress_usingDDict(d->dctx, buf, size, raw, n, dict) : ZSTD_decompressDCtx(d->dctx, buf, size, raw, n);
    if (ZSTD_isError(got) || got != size) {
        free(buf); seed_err(err, "internal", "Zstandard: %s", ZSTD_isError(got) ? ZSTD_getErrorName(got) : "size mismatch"); return -1;
    }
    *out = buf; *out_n = got;
    return 0;
}
SeedDecoder *seed_decoder_new(const SeedDicts *dicts) {
    SeedDecoder *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->dicts = dicts;
    if (!(d->dctx = ZSTD_createDCtx())) { free(d); return NULL; }
    return d;
}
void seed_decoder_free(SeedDecoder *d) { if (d) { ZSTD_freeDCtx(d->dctx); free(d); } }

SeedDicts *seed_dicts_load(const SeedRomfs *r, SeedErr *err) {
    SeedDicts *dicts = calloc(1, sizeof *dicts);
    SeedDecoder *d = dicts ? seed_decoder_new(dicts) : NULL;
    uint8_t *raw = NULL, *pack = NULL; size_t pack_n = 0; SarcMember *m = NULL; size_t count = 0;
    int64_t i = seed_romfs_find(r, "Pack/ZsDic.pack.zs");
    if (!d) { seed_err(err, "internal", "Out of memory"); goto fail; }
    if (i < 0) { seed_err(err, "missing_input", "The RomFS has no Pack/ZsDic.pack.zs"); goto fail; }
    if (seed_romfs_read(r, (uint64_t)i, &raw, err)) goto fail;
    uint64_t off, size; seed_romfs_entry(r, (uint64_t)i, &off, &size);
    if (!seed_is_zstd(raw, size)) { seed_err(err, "internal", "Pack/ZsDic.pack.zs is not Zstandard"); goto fail; }
    if (seed_decode(d, raw, size, &pack, &pack_n, err) || seed_sarc(pack, pack_n, &m, &count, err)) goto fail;
    for (size_t k = 0; k < count; k++) {
        if (dicts->n == SEED_MAX_DICTS) { seed_err(err, "internal", "Too many Zstandard dictionaries"); goto fail; }
        ZSTD_DDict *dd = ZSTD_createDDict(m[k].data, m[k].size);
        if (!dd) { seed_err(err, "internal", "Invalid Zstandard dictionary %s", m[k].name); goto fail; }
        /* ZstdDict(raw).dict_id; a later dictionary of the same id replaces the earlier one, as the dict assignment does */
        unsigned id = ZSTD_getDictID_fromDDict(dd); int j = 0;
        while (j < dicts->n && dicts->id[j] != id) j++;
        if (j < dicts->n) ZSTD_freeDDict(dicts->dict[j]); else dicts->n++;
        dicts->dict[j] = dd; dicts->id[j] = id;
    }
    free(m); free(pack); free(raw); seed_decoder_free(d);
    return dicts;
fail:
    free(m); free(pack); free(raw); seed_decoder_free(d); seed_dicts_free(dicts);
    return NULL;
}
void seed_dicts_free(SeedDicts *dicts) {
    if (!dicts) return;
    for (int i = 0; i < dicts->n; i++) ZSTD_freeDDict(dicts->dict[i]);
    free(dicts);
}

/* ---- SARC ---- */
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
static int unsafe_name(const char *name) {
    if (name[0] == '/' || strchr(name, '\\')) return 1;
    for (const char *p = name;;) {   /* every '/'-separated part non-empty and not . or .. */
        const char *e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 1;
        if (!e) return 0;
        p = e + 1;
    }
}
static int by_name(const void *a, const void *b) { return strcmp(((const SarcMember *)a)->name, ((const SarcMember *)b)->name); }

int seed_sarc(const uint8_t *d, size_t n, SarcMember **members, size_t *count, SeedErr *err) {
    *members = NULL; *count = 0;
#define BAD(msg) do { seed_err(err, "internal", "%s", msg); goto fail; } while (0)
    SarcMember *m = NULL, *sorted = NULL;
    if (n < 4 || memcmp(d, "SARC", 4)) BAD("expected SARC");
    if (n < 16) BAD("invalid SARC header");
    int big = !(d[6] == 0xff && d[7] == 0xfe);
    uint64_t header = rd16(d + 4, big), size = rd32(d + 8, big), start = rd32(d + 12, big);
    if (size != n || header < 20 || header + 4 > n || memcmp(d + header, "SFAT", 4)) BAD("invalid SARC header");
    if (header + 8 > n) BAD("invalid SARC header");
    uint64_t nfiles = rd16(d + header + 6, big), names = header + 12 + nfiles * 16;
    if (names + 4 > n || memcmp(d + names, "SFNT", 4)) BAD("invalid SARC string table");
    names += 8;
    if (nfiles && !(m = calloc(nfiles, sizeof *m))) BAD("Out of memory");
    for (uint64_t i = 0; i < nfiles; i++) {
        const uint8_t *e = d + header + 12 + i * 16;   /* in bounds: names > it */
        uint32_t attribute = rd32(e + 4, big); uint64_t lo = rd32(e + 8, big), hi = rd32(e + 12, big);
        uint64_t off = names + (uint64_t)(attribute & 0xffffff) * 4;
        if (attribute >> 24 != 1 || off < names || off >= start || hi < lo) BAD("invalid SARC member");
        uint64_t limit = start < n ? start : n;   /* data.index(0, off, start) */
        const uint8_t *nul = off < limit ? memchr(d + off, 0, limit - off) : NULL;
        if (!nul) BAD("invalid SARC member name");
        if (!utf8_ok(d + off, (size_t)(nul - (d + off)))) BAD("invalid SARC member name");
        const char *name = (const char *)d + off;
        if (name[0] == '/') name++;   /* Breath of the Wild's gamedata.ssarc names its members "/<name>.bgdata" */
        if (unsafe_name(name)) BAD("unsafe or duplicate SARC name");
        if (start + lo > n || hi - lo > n - start - lo) BAD("shader container range outside file");
        m[i] = (SarcMember){ name, d + start + lo, (size_t)(hi - lo) };
    }
    if (nfiles > 1) {
        if (!(sorted = malloc(nfiles * sizeof *sorted))) BAD("Out of memory");
        memcpy(sorted, m, nfiles * sizeof *sorted);
        qsort(sorted, nfiles, sizeof *sorted, by_name);
        for (uint64_t i = 1; i < nfiles; i++) if (!strcmp(sorted[i - 1].name, sorted[i].name)) BAD("unsafe or duplicate SARC name");
        free(sorted);
    }
    *members = m; *count = nfiles;
    return 0;
fail:
#undef BAD
    free(m); free(sorted);
    return -1;
}
