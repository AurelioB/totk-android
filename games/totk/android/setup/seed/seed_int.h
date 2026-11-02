/* The shader seed stage (setup_stage_seed, stage_seed.c): a C port of scripts/prepare_shader_seed.py for game 'totk'
   with the Vulkan translator service, then scripts/pack_shader_seed.py. Internal to android/setup/seed/. */
#ifndef TOTK_SEED_INT_H
#define TOTK_SEED_INT_H
#include "setup_internal.h"
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define SEED_CHAIN "totk-native-v5-unverified"   /* prepare_shader_seed.py CHAIN, android/gfx/vk_shader_xlate.c */
#define SEED_MIN_SHADERS 8000                    /* MIN_SHADERS: startup shaders a seed needs for first-launch warm-up */
#define SEED_MAX_DECODED (512ull << 20)          /* decoder(): "unbounded shader package" above this */

/* The first failure of any thread; the stage thread hands it to setup_fail (which is not thread-safe). */
typedef struct { pthread_mutex_t mu; int set; char code[32]; char msg[768]; } SeedErr;
void seed_err(SeedErr *, const char *code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int seed_err_set(SeedErr *);

/* ---- RomFS through runtime-index.bin (TOTKRFS1, scripts/prepare_romfs_backend.py) + the image, one romfs.bin or its
   parts (RomfsImage, romfs_read.c). A zeroed SeedRomfs is closed. ---- */
typedef struct { RomfsImage img; int opened; uint8_t *index; size_t index_size; uint64_t count; uint64_t image_size; } SeedRomfs;
int seed_romfs_open(SeedRomfs *, const char *dir, SeedErr *);
void seed_romfs_close(SeedRomfs *);
const char *seed_romfs_name(const SeedRomfs *, uint64_t i);
void seed_romfs_entry(const SeedRomfs *, uint64_t i, uint64_t *offset, uint64_t *size);
int64_t seed_romfs_find(const SeedRomfs *, const char *path);   /* -1 when absent */
int seed_romfs_read(const SeedRomfs *, uint64_t i, uint8_t **out, SeedErr *);   /* whole file, malloc'd */

/* decoder(): Zstandard with the game's dictionaries (Pack/ZsDic.pack.zs); other data is returned as is (*out = NULL). */
typedef struct SeedDicts SeedDicts;
SeedDicts *seed_dicts_load(const SeedRomfs *, SeedErr *);
void seed_dicts_free(SeedDicts *);
typedef struct SeedDecoder SeedDecoder;   /* one per thread (a ZSTD_DCtx) */
SeedDecoder *seed_decoder_new(const SeedDicts *);
void seed_decoder_free(SeedDecoder *);
int seed_is_zstd(const uint8_t *, size_t);
/* The decoded size of a zstd file before decoding it (for memory budgets), or an error. */
int seed_zstd_size(const uint8_t *, size_t, uint64_t *size, SeedErr *);
int seed_decode(SeedDecoder *, const uint8_t *raw, size_t n, uint8_t **out, size_t *out_n, SeedErr *);

typedef struct { const char *name; const uint8_t *data; size_t size; } SarcMember;   /* name: NUL-terminated, in data */
int seed_sarc(const uint8_t *, size_t, SarcMember **members, size_t *count, SeedErr *);

/* ---- shaders found (inventory.c) ---- */
typedef struct {
    uint8_t key[32];
    uint8_t stage;      /* NVN stage of control+0x714: 0 vs, 1 ps, 5 cs */
    uint8_t startup;    /* translated for the default bind state */
    uint8_t produced;   /* at least one translated variant kept */
    uint8_t used;
    uint8_t located;    /* found where the shared list locates it, not by the inventory */
    uint32_t control_size, code_size;
    uint64_t spill;     /* control then code in the spill file */
} SeedShader;
typedef struct {
    pthread_mutex_t mu;
    SeedShader *v; size_t cap, n;   /* open addressing on the key */
    int spill_fd; uint64_t spill_end;
    SeedErr *err;
} SeedTable;
int seed_table_init(SeedTable *, const char *spill_path, SeedErr *);
void seed_table_free(SeedTable *);
SeedShader *seed_table_get(SeedTable *, const uint8_t key[32]);
/* Adds a shader the first time its key is seen (control and code are spilled); startup marks it for the default state. */
int seed_table_add(SeedTable *, const uint8_t key[32], int stage, const uint8_t *control, size_t control_size,
                   const uint8_t *code, size_t code_size, int startup);
int seed_table_bytes(SeedTable *, const SeedShader *, uint8_t **control, uint8_t **code);   /* malloc'd copies */

uint32_t seed_control_key_len(const uint8_t *control, size_t n);   /* control_key_len() */

typedef struct {
    SetupCtx *c; SeedErr *err; SeedTable *table;
    const SeedRomfs *romfs; const SeedDicts *dicts;
    const char *modules;          /* rewritten/<version> */
    int threads; uint64_t budget;  /* worker count and decoded-bytes budget */
    const uint64_t *files; size_t nfiles;   /* RomFS entries to scan (the shipped list, shader_files.c), NULL: all */
    uint8_t *bearing;             /* optional, one per RomFS entry: set when the file yielded at least one shader */
    _Atomic uint64_t done_bytes; uint64_t total_bytes;
    atomic_int *stop;
} SeedInventory;
int seed_inventory_run(SeedInventory *);   /* blocking; the caller's thread only waits (see stage_seed.c) */
/* located(): shaders the list places outside BNSH files, kept only when they hash to the listed key. */
typedef struct { uint8_t key[32]; const char *source; uint64_t control, code, length; } SeedLocator;
int seed_located(SeedInventory *, SeedLocator *rows, size_t n, size_t *found);

/* ---- the shared shader list (shader_list.c) ---- */
typedef struct { uint8_t key[32]; const char *deps; uint32_t deps_len; uint32_t kind_off[4], kind_len[4]; } SeedListed;
typedef struct {
    SeedListed *v; size_t n, cap;          /* V rows, sorted by (key, deps) and unique */
    SeedLocator *loc; size_t nloc, caploc; /* L rows */
    char **arena; size_t narena;           /* row texts */
    char sha256[65];
} SeedList;
int seed_list_load(SetupCtx *, SeedList *, int fd, uint64_t offset, uint64_t length, SeedErr *);
void seed_list_free(SeedList *);

/* ---- plan (shader_list.c) and translation (translate.c) ---- */
typedef struct {
    SeedShader *shader;
    const SeedListed *listed;     /* NULL: the default bind state */
    char variant[78];             /* <key>-<12 hex> */
} SeedJob;
typedef struct { size_t first, n; } SeedGroup;   /* one shader's jobs, in plan order */
typedef struct { SeedJob *jobs; size_t njobs; SeedGroup *groups; size_t ngroups; size_t listed; } SeedPlan;
int seed_plan(SeedTable *, const SeedList *, SeedPlan *, SeedErr *);
void seed_plan_free(SeedPlan *);
/* The variant-name suffix of answers: sha256 of their canonical JSON (prepare_shader_seed.py suffix()), 12 hex. */
void seed_answers_json(const SeedListed *, Buf *);

typedef struct {
    SetupCtx *c; SeedErr *err; SeedTable *table; SeedPlan *plan;
    const char *translator, *out_dir;
    int workers;
    uint64_t timeout_ms;   /* per request: a service with no reply by then is killed and its job counted as failed */
    const char *journal;   /* the resume journal (translate.c), NULL: none */
    int resuming;          /* out_dir holds an interrupted run's files: skip its journaled shaders, redo the rest */
    size_t resumed;        /* out: shader groups taken from the journal */
    atomic_int *stop;
    _Atomic uint64_t done, ok, failed, skipped;
    pthread_mutex_t log_mu; Buf log;   /* failed translations, as prepare_shader_seed.py reports 'skipped' */
} SeedTranslate;
int seed_translate_run(SeedTranslate *);

/* ---- seed.vkpack (pack.c) ---- */
/* Packs every .spv/.vk/.vv of dir into pack_path (TOTKVKP1), unlinking each payload once copied; seed_id = the
   optimize_shader_seed.py identity (sha256 over name NUL sha256(payload), sorted names). */
int seed_pack(SetupCtx *, const char *dir, const char *pack_path, char seed_id[65], uint64_t *files, SeedErr *);

/* ---- the RomFS files that hold shaders (shader_files.c, generated by seed_cli --files-out) ----
   Identifiers only: the paths of the files of one RomFS whose scan yields at least one shader, and the SHA-256 of that
   RomFS's runtime-index.bin (the pinned index). The inventory reads only these files when the index matches; their
   shaders are all the shaders of the full scan, so the inventory, plan and seed stay the same. */
extern const char SEED_SHADER_FILES_INDEX_SHA256[65];
extern const char *const SEED_SHADER_FILES[];
extern const size_t SEED_SHADER_FILES_COUNT;

/* ---- the stage (stage_seed.c) ---- */
/* Host test hooks (seed_cli.c); setup_stage_seed passes none. plan_out: write the inventory and the plan (identifiers
   only) there; plan_only: stop after it; sample_every > 1: translate only every n-th shader group (no minimum count);
   workers > 0: that many translator services; any_count: no minimum number of translated startup shaders;
   request_timeout_ms > 0: the translation watchdog instead of SEED_REQUEST_TIMEOUT_MS; full_scan: scan every RomFS
   file even when the shipped list applies; files_out: write shader_files.c for this RomFS there (needs full_scan). */
typedef struct {
    const char *plan_out; int plan_only; size_t sample_every; int workers; int any_count; uint64_t request_timeout_ms;
    int full_scan; const char *files_out;
} SeedOptions;
int seed_run(SetupCtx *, const char *romfs_dir, const char *rewritten_dir, const char *out_seed_dir, const SeedOptions *);
#endif
