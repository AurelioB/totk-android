/* NSO0 loading (with its own bounds-checked LZ4 block decoder), dynamic tags and DT_RELR decoding for the rewriter and
   the tables writer. Mirrors third_party/d2r/patch_nso.py (image build) and relr.py (decode_relr). */
#include "rw_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LZ4 block format (https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md). Like python-lz4's
   decompress(uncompressed_size=n), the block must produce exactly n bytes. */
static int lz4_block(const uint8_t *s, size_t sn, uint8_t *d, size_t dn) {
    size_t i = 0, o = 0;
    for (;;) {
        if (i >= sn) return -1;
        unsigned tok = s[i++]; size_t lit = tok >> 4, b;
        if (lit == 15) do { if (i >= sn) return -1; b = s[i++]; lit += b; } while (b == 255);
        if (lit > sn - i || lit > dn - o) return -1;
        memcpy(d + o, s + i, lit); i += lit; o += lit;
        if (i == sn) break;                                 /* the last sequence carries literals only */
        if (sn - i < 2) return -1;
        size_t off = s[i] | (size_t)s[i + 1] << 8, ml = tok & 15; i += 2;
        if (!off || off > o) return -1;
        if (ml == 15) do { if (i >= sn) return -1; b = s[i++]; ml += b; } while (b == 255);
        ml += 4;
        if (ml > dn - o) return -1;
        if (off >= ml) memcpy(d + o, d + o - off, ml);
        else for (size_t k = 0; k < ml; k++) d[o + k] = d[o + k - off];   /* overlapping copy repeats the pattern */
        o += ml;
    }
    return o == dn ? 0 : -1;
}

int rw_nso_load(const char *path, RwNso *n, char *err, size_t errlen) {
    uint8_t *d; size_t dn;
    memset(n, 0, sizeof *n);
    if (rw_read_file(path, &d, &dn, err, errlen)) return RW_FAIL;
    if (dn < 0x100 || memcmp(d, "NSO0", 4)) { snprintf(err, errlen, "%s is not an NSO0 module", path); free(d); return RW_FAIL; }
    memcpy(n->header, d, 0x100);
    uint32_t flags = rd32(d + 0xc), fo[3], mo[3], sz[3], cs[3], bss = rd32(d + 0x3c);
    for (int i = 0; i < 3; i++) { fo[i] = rd32(d + 0x10 + 0x10 * i); mo[i] = rd32(d + 0x14 + 0x10 * i); sz[i] = rd32(d + 0x18 + 0x10 * i); cs[i] = rd32(d + 0x60 + 4 * i); }
    for (int i = 0; i < 16; i++) snprintf(n->build_id + 2 * i, 3, "%02x", d[0x40 + i]);
    uint64_t base = (uint64_t)mo[2] + sz[2] + bss;
    n->key_page_off = (base + 0x3FFF) & ~(uint64_t)0x3FFF;
    n->len = base + 0x4000;
    n->patch_off = (n->len + 0xFFFF) & ~(uint64_t)0xFFFF;
    n->img = calloc(1, n->len);
    if (!n->img) { snprintf(err, errlen, "out of memory for the %s image", path); free(d); return RW_FAIL; }
    for (int i = 0; i < 3; i++) {
        /* An uncompressed segment of another length would resize the Python bytearray; such modules are not TOTK's. */
        if ((uint64_t)fo[i] + cs[i] > dn || (uint64_t)mo[i] + sz[i] > base || (!(flags >> i & 1) && cs[i] != sz[i])) {
            snprintf(err, errlen, "%s: segment %d outside the module", path, i); goto fail; }
        if (flags >> i & 1) { if (lz4_block(d + fo[i], cs[i], n->img + mo[i], sz[i])) { snprintf(err, errlen, "%s: segment %d does not decompress", path, i); goto fail; } }
        else memcpy(n->img + mo[i], d + fo[i], sz[i]);
    }
    n->text_end = sz[0]; n->ro_off = mo[1]; n->data_off = mo[2];
    free(d); return RW_OK;
fail:
    free(d); free(n->img); n->img = NULL; return RW_FAIL;
}

int rw_tag(const RwTags *t, int64_t tag, uint64_t *val) {
    for (size_t i = t->n; i-- > 0;) if (t->v[i].tag == tag) { if (val) *val = t->v[i].val; return 1; }
    return 0;
}

static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

int rw_decode_relr(const uint8_t *img, size_t len, const RwTags *tags, uint64_t lower, uint64_t upper, RwRelr **out, size_t *n,
                   char *err, size_t errlen) {
    uint64_t sz = 0, table = 0, ent = 0; int h35 = rw_tag(tags, 35, &sz), h36 = rw_tag(tags, 36, &table), h37 = rw_tag(tags, 37, &ent);
    *out = NULL; *n = 0;
    if (!h35 && !h36 && !h37) return RW_OK;
    if (!(h35 && h36 && h37) || ent != 8 || sz % 8) { snprintf(err, errlen, "Invalid ELF64 RELR dynamic tags"); return RW_FAIL; }
    if (table > len || sz > len - table) { snprintf(err, errlen, "RELR table outside image"); return RW_FAIL; }
    size_t cap = 0, cnt = 0; RwRelr *v = NULL; uint64_t cursor = 0; int have = 0;
    for (uint64_t o = table; o < table + sz; o += 8) {
        uint64_t e = rd64(img + o), addrs[64]; int k = 0;
        if (!(e & 1)) { addrs[k++] = e; if (e > UINT64_MAX - 8) goto range; cursor = e + 8; have = 1; }
        else {
            if (!have) { snprintf(err, errlen, "RELR bitmap before initial address"); goto fail; }
            for (int bit = 1; bit < 64; bit++) if (e >> bit & 1) addrs[k++] = cursor + (uint64_t)(bit - 1) * 8;
            if (cursor > UINT64_MAX / 2) goto range;
            cursor += 63 * 8;
        }
        for (int i = 0; i < k; i++) {
            uint64_t a = addrs[i];
            if (a % 8 || a < lower || upper < 8 || a > upper - 8 || a > len - 8) { snprintf(err, errlen, "RELR relocation outside writable image: %#llx", (unsigned long long)a); goto fail; }
            uint64_t addend = rd64(img + a);
            if (addend >= upper) { snprintf(err, errlen, "RELR addend outside module: %#llx", (unsigned long long)a); goto fail; }
            if (cnt == cap) { cap = cap ? cap * 2 : 4096; RwRelr *p = realloc(v, cap * sizeof *v); if (!p) { snprintf(err, errlen, "out of memory"); goto fail; } v = p; }
            v[cnt].where = a; v[cnt].addend = addend; cnt++;
        }
    }
    if (cnt) {   /* relr.py refuses a relocation listed twice */
        uint64_t *s = malloc(cnt * sizeof *s);
        if (!s) { snprintf(err, errlen, "out of memory"); goto fail; }
        for (size_t i = 0; i < cnt; i++) s[i] = v[i].where;
        qsort(s, cnt, sizeof *s, cmp_u64);
        for (size_t i = 1; i < cnt; i++) if (s[i] == s[i - 1]) { snprintf(err, errlen, "Duplicate RELR relocation: %#llx", (unsigned long long)s[i]); free(s); goto fail; }
        free(s);
    }
    *out = v; *n = cnt; return RW_OK;
range:
    snprintf(err, errlen, "RELR relocation outside writable image");
fail:
    free(v); return RW_FAIL;
}
