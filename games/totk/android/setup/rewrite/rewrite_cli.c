/* Host driver for the rewriter and the tables writer (oracle tests against the Python references).
     rewrite_cli rewrite <nso> <out dir> <main|subsdk0|sdk>     <name>.patched.bin + the six TSVs, like patch_nso.py
     rewrite_cli tables <rewritten dir> <exefs dir> <out file>  runtime-tables.bin from patched.bin + fixups/gates TSVs
     rewrite_cli code <exefs dir> <out rewritten> <out game-data> [diag dir]   the whole code stage
   Prints the output digests on stdout. */
#include "code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const MODS[3] = {"main", "subsdk0", "sdk"};

static void progress(void *ud, const char *detail, uint64_t done, uint64_t total) {
    (void)ud; fprintf(stderr, "\r%s %5.1f%%   ", detail, total ? 100.0 * (double)done / (double)total : 0.0);
}

int main(int argc, char **argv) {
    char err[512] = "";
    if (argc == 5 && !strcmp(argv[1], "rewrite")) {
        RwModule m; char sha[65];
        if (rw_rewrite_nso(argv[2], argv[4], &m, NULL, NULL, NULL, err, sizeof err) || rw_write_module(&m, argv[3], 1, 1, sha, err, sizeof err)) {
            fprintf(stderr, "rewrite_cli: %s\n", err); return 1; }
        printf("%s.patched.bin %s\n", argv[4], sha);
        rw_free_module(&m); return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "tables")) {
        TabModule *t[3] = {0}; char path[4096], sha[65]; int rc = 1;
        for (int k = 0; k < 3; k++) {
            RwLayout l; RwGate *g = NULL; size_t ng = 0; uint8_t *img = NULL, *nso = NULL; size_t n, nn;
            snprintf(path, sizeof path, "%s/%s.patched.bin", argv[2], MODS[k]);
            int bad = rw_read_layout(argv[2], MODS[k], &l, err, sizeof err) || rw_read_gates(argv[2], MODS[k], &g, &ng, err, sizeof err) ||
                      rw_read_file(path, &img, &n, err, sizeof err);
            snprintf(path, sizeof path, "%s/%s", argv[3], MODS[k]);
            if (!bad && (rw_read_file(path, &nso, &nn, err, sizeof err) || (nn < 0x100 && snprintf(err, sizeof err, "%s too small", path)))) bad = 1;
            if (!bad) t[k] = tab_module(&l, img, n, g, ng, nso, err, sizeof err);
            free(g); free(img); free(nso);
            if (!t[k]) goto tables_out;
        }
        if (tab_write(t, argv[4], sha, err, sizeof err)) goto tables_out;
        printf("runtime-tables.bin %s\n", sha); rc = 0;
    tables_out:
        if (rc) fprintf(stderr, "rewrite_cli: %s\n", err);
        for (int k = 0; k < 3; k++) tab_module_free(t[k]);
        return rc;
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "code")) {
        RwStageHooks h = {progress, NULL, NULL}; RwCodeResult r;
        int rc = rw_code_stage(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : NULL, &h, NULL, NULL, &r, err, sizeof err);
        fprintf(stderr, "\n");
        if (rc) { fprintf(stderr, "rewrite_cli: %s\n", err); return 1; }
        for (int k = 0; k < 3; k++) printf("%s.patched.bin %s\n", MODS[k], r.rewritten_sha[k]);
        printf("runtime-tables.bin %s\n", r.tables_sha);
        return 0;
    }
    fprintf(stderr, "usage: %s rewrite <nso> <out dir> <name> | tables <rewritten dir> <exefs dir> <out file> | "
                    "code <exefs dir> <out rewritten> <out game-data> [diag dir]\n", argv[0]);
    return 2;
}
