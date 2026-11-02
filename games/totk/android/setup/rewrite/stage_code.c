/* setup_stage_code: the 1.4.3 ExeFS modules → rewritten modules + runtime-tables.bin, every input and output checked
   against assets/expected-v1.4.3.json (via totk_pins.h). Only <name>.patched.bin land in out_rewritten_dir and
   runtime-tables.bin in out_game_data_dir; the rewriter's TSVs stay in <staging>/rewrite-diag for diagnostics. */
#include "../setup.h"
#include "code.h"
#include "totk_pins.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TOTK_PIN_ANDROID_RUNTIME_TABLES_SHA256
#error "totk_pins.h has no TOTK_PIN_ANDROID_RUNTIME_TABLES_SHA256: regenerate it from assets/expected-v1.4.3.json"
#endif

static void progress(void *ud, const char *detail, uint64_t done, uint64_t total) { setup_progress(ud, "code", done, total, detail); }
static int cancelled(void *ud) { return setup_cancelled(ud); }

/* 0 = matches, 1 = differs, -1 = unreadable (errno kept), -2 = cancelled */
static int check_file(SetupCtx *ctx, const char *path, const char *want) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    uint8_t buf[1 << 16]; RwSha256 s; char hex[65]; ssize_t r; off_t at = 0;
    rw_sha256_init(&s);
    while ((r = pread(fd, buf, sizeof buf, at)) != 0) {
        if (r < 0) { if (errno == EINTR) continue; int e = errno; close(fd); errno = e; return -1; }
        rw_sha256_update(&s, buf, (size_t)r); at += r;
        if (setup_cancelled(ctx)) { close(fd); return -2; }
    }
    close(fd); rw_sha256_hex(&s, hex);
    return strcmp(hex, want) ? 1 : 0;
}

int setup_stage_code(SetupCtx *ctx, const char *exefs_dir, const char *out_rewritten_dir, const char *out_game_data_dir) {
    static const char *const names[3] = {"main", "subsdk0", "sdk"};
    static const char *const exefs_pins[3] = {TOTK_PIN_EXEFS_SHA256_MAIN, TOTK_PIN_EXEFS_SHA256_SUBSDK0, TOTK_PIN_EXEFS_SHA256_SDK};
    static const char *const rewritten_pins[3] = {TOTK_PIN_ANDROID_REWRITTEN_SHA256_MAIN, TOTK_PIN_ANDROID_REWRITTEN_SHA256_SUBSDK0, TOTK_PIN_ANDROID_REWRITTEN_SHA256_SDK};
    char path[4096], err[512] = "";
    for (int k = 0; k < 3; k++) {
        setup_progress(ctx, "code", 0, 1, "checking the ExeFS");
        snprintf(path, sizeof path, "%s/%s", exefs_dir, names[k]);
        int r = check_file(ctx, path, exefs_pins[k]);
        if (r == -2) return SETUP_CANCELLED;
        if (r == -1) { setup_fail(ctx, errno == ENOENT ? "missing_input" : "io", "cannot read the ExeFS module %s: %s", names[k], strerror(errno)); return -1; }
        if (r) { setup_fail(ctx, "hash_mismatch", "the ExeFS module %s is not the one of Tears of the Kingdom 1.4.3", names[k]); return -1; }
    }
    /* Diagnostics are best effort: without the folder the stage still runs. */
    const char *diag = NULL; char dpath[4096];
    if (setup_staging_dir(ctx)) {
        snprintf(dpath, sizeof dpath, "%s/rewrite-diag", setup_staging_dir(ctx));
        if (!mkdir(dpath, 0755) || errno == EEXIST) diag = dpath;
    }
    RwStageHooks h = {progress, cancelled, ctx}; RwCodeResult res;
    int rc = rw_code_stage(exefs_dir, out_rewritten_dir, out_game_data_dir, diag, &h, rewritten_pins, TOTK_PIN_ANDROID_RUNTIME_TABLES_SHA256,
                           &res, err, sizeof err);
    switch (rc) {
    case RW_OK: return 0;
    case RW_CANCELLED: return SETUP_CANCELLED;
    case RW_MISMATCH: setup_fail(ctx, "hash_mismatch", "%s", err); return -1;
    case RW_MISSING: setup_fail(ctx, "missing_input", "%s", err); return -1;
    default:
        setup_fail(ctx, strstr(err, strerror(ENOSPC)) ? "no_space" : (!strncmp(err, "cannot ", 7) ? "io" : "internal"), "%s", err);
        return -1;
    }
}
