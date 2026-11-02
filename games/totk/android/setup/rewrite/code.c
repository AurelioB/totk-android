/* The code stage without the setup context: rewrite main / subsdk0 / sdk one at a time (one image in memory at once),
   write each <name>.patched.bin, collect its tables, then write runtime-tables.bin. Shared by setup_stage_code
   (stage_code.c) and the host driver (rewrite_cli.c). */
#include "code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *const MODS[3] = {"main", "subsdk0", "sdk"};

typedef struct { const RwStageHooks *h; uint64_t base, span, total, last; const char *name; char detail[64]; } Prog;

static int poll_cb(void *ud, uint64_t done, uint64_t total) {
    Prog *p = ud;
    if (p->h && p->h->cancelled && p->h->cancelled(p->h->ud)) return 1;
    if (p->h && p->h->progress && total) {
        uint64_t at = p->base + (uint64_t)((double)p->span * ((double)done / (double)total));
        if (at >= p->last + p->total / 400 || !p->last) {   /* every 0.25 %: the UI does not need each function */
            p->last = at ? at : 1; snprintf(p->detail, sizeof p->detail, "rewriting %s", p->name);
            p->h->progress(p->h->ud, p->detail, at, p->total);
        }
    }
    return 0;
}

int rw_code_stage(const char *exefs, const char *out_rewritten, const char *out_game_data, const char *diag_dir,
                  const RwStageHooks *hooks, const char *const expect_rewritten[3], const char *expect_tables,
                  RwCodeResult *res, char *err, size_t errlen) {
    TabModule *tabs[3] = {0}; char path[4096]; uint64_t w[3], total = 0; int rc = RW_FAIL;
    memset(res, 0, sizeof *res);
    for (int k = 0; k < 3; k++) {            /* progress weight: the compressed module sizes (main dominates) */
        struct stat st; snprintf(path, sizeof path, "%s/%s", exefs, MODS[k]);
        if (stat(path, &st) < 0) { snprintf(err, errlen, "missing ExeFS module %s", MODS[k]); return RW_MISSING; }
        w[k] = (uint64_t)st.st_size; total += w[k];
    }
    Prog p = {hooks, 0, 0, total + total / 50, 0, NULL, {0}};     /* the last 2 % are the tables */
    for (int k = 0; k < 3; k++) {
        RwModule m; uint8_t hdr[0x100];
        p.span = w[k]; p.name = MODS[k];
        snprintf(path, sizeof path, "%s/%s", exefs, MODS[k]);
        int r = rw_rewrite_nso(path, MODS[k], &m, hdr, poll_cb, &p, err, errlen);
        if (r) { rc = r; goto out; }
        r = rw_write_module(&m, out_rewritten, 1, 0, res->rewritten_sha[k], err, errlen);
        if (!r && diag_dir) r = rw_write_module(&m, diag_dir, 0, 1, NULL, err, errlen);
        if (r) { rw_free_module(&m); goto out; }
        if (expect_rewritten && expect_rewritten[k] && strcmp(expect_rewritten[k], res->rewritten_sha[k])) {
            snprintf(err, errlen, "rewritten %s does not match the pinned digest (got %s)", MODS[k], res->rewritten_sha[k]);
            rw_free_module(&m); rc = RW_MISMATCH; goto out;
        }
        tabs[k] = tab_module(&m.l, m.image, m.image_size, m.gates, m.ngate, hdr, err, errlen);
        rw_free_module(&m);
        if (!tabs[k]) goto out;
        p.base += w[k];
        if (hooks && hooks->cancelled && hooks->cancelled(hooks->ud)) { rc = RW_CANCELLED; snprintf(err, errlen, "cancelled"); goto out; }
    }
    snprintf(path, sizeof path, "%s/runtime-tables.bin", out_game_data);
    if (tab_write(tabs, path, res->tables_sha, err, errlen)) goto out;
    if (expect_tables && strcmp(expect_tables, res->tables_sha)) {
        snprintf(err, errlen, "runtime-tables.bin does not match the pinned digest (got %s)", res->tables_sha); rc = RW_MISMATCH; goto out; }
    if (hooks && hooks->progress) hooks->progress(hooks->ud, "runtime tables written", p.total, p.total);
    rc = RW_OK;
out:
    for (int k = 0; k < 3; k++) tab_module_free(tabs[k]);
    return rc;
}
