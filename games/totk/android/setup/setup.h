/* On-device setup library (libtotk_setup.so) shared stage ABI. The importer core (setup.c) owns the context, the stage
   order, staging and install.json; each stage writes only into the output directories it is given (staging dirs under
   <game>/.import) and the orchestrator verifies and renames them into place. */
#ifndef TOTK_SETUP_H
#define TOTK_SETUP_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage return value when setup_cancelled() turned true; 0 = success, -1 = failure (after setup_fail). */
#define SETUP_CANCELLED (-2)

typedef struct SetupCtx SetupCtx;                       /* opaque */
int  setup_cancelled(const SetupCtx *);                 /* poll often; return SETUP_CANCELLED promptly */
void setup_progress(SetupCtx *, const char *stage, uint64_t done, uint64_t total, const char *detail);
/* Records the first failure (code: hash_mismatch|missing_input|bad_keys|unsupported_version|no_space|io|cancelled|
   internal, or not_available for a generator this build lacks); the caller then returns -1. Never pass key material. */
void setup_fail(SetupCtx *, const char *code, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
const char *setup_game_dir(const SetupCtx *);           /* <game> */
/* The shared shader list (APK asset assets/shader_list.tsv) when the request carries one: an fd the caller keeps open,
   read with pread at [offset, offset+length). 0 when present, else -1. */
int setup_shader_list(const SetupCtx *, int *fd, uint64_t *offset, uint64_t *length);
const char *setup_lib_dir(const SetupCtx *);            /* <libdir> (translator: <libdir>/libtotk_xlate.so) */
const char *setup_staging_dir(const SetupCtx *);        /* <game>/.import */

/* Stage entry points: return 0 on success, -1 on failure (after setup_fail), SETUP_CANCELLED on cancel. */
int setup_stage_code(SetupCtx *, const char *exefs_dir, const char *out_rewritten_dir, const char *out_game_data_dir);
int setup_stage_seed(SetupCtx *, const char *romfs_dir, const char *rewritten_dir, const char *out_seed_dir);
int setup_stage_companion(SetupCtx *, const char *romfs_dir, const char *out_companion_dir);

#ifdef __cplusplus
}
#endif
#endif
