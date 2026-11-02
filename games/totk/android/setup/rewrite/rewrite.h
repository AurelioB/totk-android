/* On-device port of the code rewriter (third_party/d2r/patch_nso.py with DM_RULESET=native, DM_PRIVATE_GATES=0,
   DM_HOST_TP=tpidr_el0) and of the runtime-tables writer (scripts/prepare_runtime.py). Both must stay byte-identical to
   the Python references; tests/setup/test_rewrite_parity.py compares them on real modules. Only the three Tears of the
   Kingdom 1.4.3 modules are accepted: any other build id is refused before anything is rewritten. */
#ifndef TOTK_REWRITE_H
#define TOTK_REWRITE_H
#include <stddef.h>
#include <stdint.h>

#define RW_OK 0
#define RW_FAIL (-1)
#define RW_CANCELLED (-2)       /* same value as SETUP_CANCELLED */

/* Polled during the long loops; nonzero = stop with RW_CANCELLED. done/total count text words, then functions. */
typedef int (*RwPoll)(void *ud, uint64_t done, uint64_t total);

typedef struct { char *p; size_t n, cap; } RwBuf;

enum { RW_GATE_SVC, RW_GATE_CNTPCT, RW_GATE_CNTVCT };
typedef struct { uint64_t ret, original; uint32_t kind, arg; } RwGate;

typedef struct {
    char name[16], build_id[33];
    uint64_t text_end, ro_off, data_off, key_page_off, patch_off;
} RwLayout;

typedef struct {
    RwLayout l;
    uint8_t *image; size_t image_size;      /* the whole <name>.patched.bin */
    RwGate *gates; size_t ngate;            /* emission order, as gates.tsv */
    RwBuf tsv[6];                           /* fixups, gates, sites, cuts, params, obs */
} RwModule;
extern const char *const rw_tsv_names[6];

/* nso_header receives the first 0x100 bytes of the NSO (the tables writer needs its dynsym extent at 0x98). */
int rw_rewrite_nso(const char *nso_path, const char *name, RwModule *out, uint8_t nso_header[0x100],
                   RwPoll poll, void *ud, char *err, size_t errlen);
/* Writes <dir>/<name>.patched.bin when image != 0 (sha256_hex receives its digest) and the six TSVs when tsvs != 0. */
int rw_write_module(const RwModule *m, const char *dir, int image, int tsvs, char sha256_hex[65], char *err, size_t errlen);
void rw_free_module(RwModule *m);

/* ---- runtime-tables.bin (TOTKTAB1) ---- */
typedef struct TabModule TabModule;
/* image: the full patched image; gates in any order (sorted here, as the Python writer does). */
TabModule *tab_module(const RwLayout *l, const uint8_t *image, size_t size, const RwGate *gates, size_t ngate,
                      const uint8_t nso_header[0x100], char *err, size_t errlen);
void tab_module_free(TabModule *m);
/* mods: main, subsdk0, sdk in that order. Writes path atomically (path.partial then rename). */
int tab_write(TabModule *const mods[3], const char *path, char sha256_hex[65], char *err, size_t errlen);

/* Reading back a rewrite written with tsvs=1 (host tests, diagnostics). */
int rw_read_layout(const char *dir, const char *name, RwLayout *l, char *err, size_t errlen);
int rw_read_gates(const char *dir, const char *name, RwGate **gates, size_t *n, char *err, size_t errlen);

/* ---- shared helpers ---- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t fill; } RwSha256;
void rw_sha256_init(RwSha256 *s);
void rw_sha256_update(RwSha256 *s, const void *data, size_t n);
void rw_sha256_hex(RwSha256 *s, char out[65]);
int rw_read_file(const char *path, uint8_t **data, size_t *size, char *err, size_t errlen);
int rw_buf_printf(RwBuf *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#endif
