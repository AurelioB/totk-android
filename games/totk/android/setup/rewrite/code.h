/* The whole code stage (three modules + runtime-tables.bin) behind plain callbacks, so the host driver and the setup
   library run the same path. */
#ifndef TOTK_REWRITE_CODE_H
#define TOTK_REWRITE_CODE_H
#include "rewrite.h"

#define RW_MISMATCH (-3)        /* an output does not match its pinned digest */
#define RW_MISSING (-4)         /* an input module is missing */

typedef struct {
    void (*progress)(void *ud, const char *detail, uint64_t done, uint64_t total);
    int (*cancelled)(void *ud);
    void *ud;
} RwStageHooks;
typedef struct { char rewritten_sha[3][65], tables_sha[65]; } RwCodeResult;   /* main, subsdk0, sdk */

/* exefs: folder with main / subsdk0 / sdk. Writes {main,subsdk0,sdk}.patched.bin to out_rewritten and
   runtime-tables.bin to out_game_data; diag_dir (optional) also receives the patched images and their six TSVs.
   expect_* (optional, lowercase hex): checked as soon as each output exists. */
int rw_code_stage(const char *exefs, const char *out_rewritten, const char *out_game_data, const char *diag_dir,
                  const RwStageHooks *hooks, const char *const expect_rewritten[3], const char *expect_tables,
                  RwCodeResult *res, char *err, size_t errlen);
#endif
