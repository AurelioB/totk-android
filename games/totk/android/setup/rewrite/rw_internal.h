/* Internal pieces shared by rewrite.c and tables.c. */
#ifndef TOTK_RW_INTERNAL_H
#define TOTK_RW_INTERNAL_H
#include "rewrite.h"

static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* NSO0 → memory image exactly as patch_nso.py builds it: text/ro/data placed at their memory offsets, bss zeroed, then
   the 0x4000-byte key page appended to the end (KEY_PAGE_OFF itself is the 16 KiB-aligned end, which may lie past it). */
typedef struct {
    uint8_t *img; size_t len;                   /* len includes the appended key page */
    uint8_t header[0x100];
    uint64_t text_end, ro_off, data_off, key_page_off, patch_off;
    char build_id[33];
} RwNso;
int rw_nso_load(const char *path, RwNso *n, char *err, size_t errlen);

/* ELF dynamic table: later duplicates win, as the Python dict does. */
typedef struct { int64_t tag; uint64_t val; } RwTag;
typedef struct { RwTag *v; size_t n; } RwTags;
int rw_tag(const RwTags *t, int64_t tag, uint64_t *val);
typedef struct { uint64_t where, addend; } RwRelr;
/* relr.py decode_relr: returns RW_OK with *out NULL / *n 0 when the module has no RELR table. */
int rw_decode_relr(const uint8_t *img, size_t len, const RwTags *tags, uint64_t lower, uint64_t upper, RwRelr **out, size_t *n,
                   char *err, size_t errlen);
#endif
