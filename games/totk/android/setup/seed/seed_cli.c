/* seed_cli: the shader seed stage alone on a Linux host or an Android shell, for the oracle test
   (tests/setup/test_seed_parity.py) and device measurements; not part of libtotk_setup.so (SEED_CLI_SRCS).
     seed_cli --romfs D --rewritten R --out O --libdir L [--shader-list F] [--plan-out F] [--plan-only]
              [--sample N] [--workers N] [--any-count] [--request-timeout-ms N] [--staging S] [--full-scan [--files-out F]]
   --full-scan scans every RomFS file instead of the shipped list (shader_files.c); --files-out regenerates that list.
   --work-dir W plays the app's noBackupDir: the scratch is W/setup-work/shaders-<id> and an interrupted run resumes it
   (without it the scratch is <out>/.work and is never resumed).
   It provides the few context functions the stage uses (setup.c owns them in the library), prints progress to stderr
   and one JSON line on stdout. Exit 0 ok, 1 failure, 3 cancelled (SIGINT / SIGTERM). */
#include "seed_int.h"
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static atomic_int cancel_flag;
static SetupRequest request;
static uint64_t last_ms;
static void on_signal(int sig) { (void)sig; atomic_store(&cancel_flag, 1); }

int setup_cancelled(const SetupCtx *c) { return c->cancel && atomic_load(c->cancel); }
void setup_progress(SetupCtx *c, const char *stage, uint64_t done, uint64_t total, const char *detail) {
    (void)c;
    uint64_t now = st_now_ms();
    if (now - last_ms < 2000 && !(total && done == total)) return;
    last_ms = now;
    fprintf(stderr, "[%s] %llu/%llu %s\n", stage, (unsigned long long)done, (unsigned long long)total, detail);
}
void setup_fail(SetupCtx *c, const char *code, const char *fmt, ...) {
    if (c->failed) return;
    c->failed = 1;
    snprintf(c->fail_code, sizeof c->fail_code, "%s", code);
    va_list ap; va_start(ap, fmt); vsnprintf(c->fail_msg, sizeof c->fail_msg, fmt, ap); va_end(ap);
}
const char *setup_game_dir(const SetupCtx *c) { return c->game; }
const char *setup_lib_dir(const SetupCtx *c) { return c->lib; }
const char *setup_staging_dir(const SetupCtx *c) { return c->staging; }
int setup_shader_list(const SetupCtx *c, int *fd, uint64_t *offset, uint64_t *length) {
    if (!c->req || !c->req->shader_list.present) return -1;
    *fd = c->req->shader_list.fd; *offset = c->req->shader_list.offset; *length = c->req->shader_list.size;
    return 0;
}

int main(int argc, char **argv) {
    const char *romfs = NULL, *rewritten = NULL, *out = NULL, *lib = NULL, *list = NULL, *staging = NULL, *workdir = NULL;
    SeedOptions opt = {0};
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--plan-only")) { opt.plan_only = 1; continue; }
        if (!strcmp(a, "--any-count")) { opt.any_count = 1; continue; }
        if (!strcmp(a, "--full-scan")) { opt.full_scan = 1; continue; }
        if (!v) { fprintf(stderr, "usage: see the header of android/setup/seed/seed_cli.c\n"); return 2; }
        if (!strcmp(a, "--romfs")) romfs = v;
        else if (!strcmp(a, "--rewritten")) rewritten = v;
        else if (!strcmp(a, "--out")) out = v;
        else if (!strcmp(a, "--libdir")) lib = v;
        else if (!strcmp(a, "--shader-list")) list = v;
        else if (!strcmp(a, "--plan-out")) opt.plan_out = v;
        else if (!strcmp(a, "--sample")) opt.sample_every = strtoull(v, NULL, 10);
        else if (!strcmp(a, "--workers")) opt.workers = atoi(v);
        else if (!strcmp(a, "--request-timeout-ms")) opt.request_timeout_ms = strtoull(v, NULL, 10);
        else if (!strcmp(a, "--staging")) staging = v;
        else if (!strcmp(a, "--work-dir")) workdir = v;
        else if (!strcmp(a, "--files-out")) opt.files_out = v;
        else { fprintf(stderr, "unknown argument %s\n", a); return 2; }
        i++;
    }
    if (!romfs || !rewritten || !out || !lib) { fprintf(stderr, "usage: see the header of android/setup/seed/seed_cli.c\n"); return 2; }
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    SetupCtx *c = calloc(1, sizeof *c);
    if (!c) return 1;
    snprintf(c->lib, sizeof c->lib, "%s", lib);
    snprintf(c->staging, sizeof c->staging, "%s", staging ? staging : out);
    if (workdir) snprintf(c->nobackup, sizeof c->nobackup, "%s", workdir);
    c->cancel = &cancel_flag; c->req = &request;
    if (list) {
        struct stat st;
        request.shader_list.fd = open(list, O_RDONLY | O_CLOEXEC);
        if (request.shader_list.fd < 0 || fstat(request.shader_list.fd, &st)) { perror(list); return 2; }
        request.shader_list.size = (uint64_t)st.st_size; request.shader_list.present = 1;
    }
    st_mkdirs(out); st_mkdirs(c->staging);
    uint64_t t0 = st_now_ms();
    int rc = seed_run(c, romfs, rewritten, out, &opt);
    Buf b = {0};
    if (!rc) buf_printf(&b, "{\"ok\":true,\"ms\":%llu}", (unsigned long long)(st_now_ms() - t0));
    else {
        buf_puts(&b, "{\"ok\":false,\"code\":"); buf_jstr(&b, rc == SETUP_CANCELLED ? "cancelled" : c->fail_code);
        buf_puts(&b, ",\"message\":"); buf_jstr(&b, c->fail_msg); buf_puts(&b, "}");
    }
    puts(b.p ? b.p : "{\"ok\":false}");
    buf_free(&b); free(c);
    return rc == SETUP_CANCELLED ? 3 : rc ? 1 : 0;
}
