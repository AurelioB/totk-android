/* Runs the bundled hactool (<libdir>/libhactool.so, a PIE executable) for the container route. Inputs are inherited
   descriptors at fixed numbers, named /proc/self/fd/N on its command line; its output goes through a pipe so long hex runs
   (title keys hactool may print) are scrubbed before anything reaches the log. A SetupFollow drains the FIFO hactool
   writes its output into straight into a RomfsWriter, which hashes and stores it, so the 17 GB image is never read back. */
#define _GNU_SOURCE 1
#include "setup_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

void hactool_scrub(char *line) {
    char *w = line;
    for (char *p = line; *p;) {
        size_t n = strspn(p, "0123456789abcdefABCDEF");
        if (n >= 32) { memcpy(w, "<hex>", 5); w += 5; p += n; continue; }   /* never longer than the run it replaces */
        if (!n) n = 1;
        memmove(w, p, n); w += n; p += n;
    }
    *w = 0;
}

/* The acceptance rules of scripts/extract_update_romfs.py: hactool reports the unsigned fixed-key / NPDM signatures (and
   the ACID signature of newer updates) as FAIL without Nintendo's public keys; those lines are not data failures. Any other
   FAIL / INVALID / "Failed to" / "Error:" fails, and a RomFS merge needs all six IVFC levels GOOD. */
int hactool_log_ok(const char *log, int require_ivfc) {
    regex_t unsigned_re, acid_re, acid_sig_re, fail_re;
    if (regcomp(&unsigned_re, "^[[:space:]]*(Fixed-Key Signature|NPDM Signature) \\(FAIL\\)", REG_EXTENDED | REG_NOSUB)) return 0;
    regcomp(&acid_re, "^[[:space:]]*ACID:[[:space:]]*$", REG_EXTENDED | REG_NOSUB);
    regcomp(&acid_sig_re, "^[[:space:]]*Signature( Key)? \\(FAIL\\)", REG_EXTENDED | REG_NOSUB);
    regcomp(&fail_re, "\\(FAIL\\)|\\(INVALID\\)|Failed to|Error:", REG_EXTENDED | REG_NOSUB);
    int ok = 1, in_acid = 0, levels = 0;
    const char *p = log;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[1024]; if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n); line[n] = 0;
        p += e ? (size_t)(e - p) + 1 : strlen(p);
        if (!regexec(&acid_re, line, 0, NULL, 0)) in_acid = 1;
        else if (in_acid && strncmp(line, "        ", 8)) in_acid = 0;
        if (!regexec(&unsigned_re, line, 0, NULL, 0) || (in_acid && !regexec(&acid_sig_re, line, 0, NULL, 0))) continue;
        if (!regexec(&fail_re, line, 0, NULL, 0)) ok = 0;
        for (int i = 0; i < 6; i++) { char want[32]; snprintf(want, sizeof want, "Level %d (GOOD)", i); if (strstr(line, want)) levels |= 1 << i; }
    }
    regfree(&unsigned_re); regfree(&acid_re); regfree(&acid_sig_re); regfree(&fail_re);
    return ok && (!require_ivfc || levels == 0x3f);
}

/* Drains what the FIFO holds now (non-blocking) into the writer's ring. A read of 0 means no writer: not connected yet
   while hactool starts, or done once it has closed the output after writing. */
static int follow_pipe(SetupCtx *c, SetupFollow *f, int until_eof) {
    uint64_t budget = until_eof ? UINT64_MAX : 64u << 20;
    while (budget && !f->pipe_eof) {
        uint8_t *p; size_t room;
        int rc = romfs_writer_reserve(c, f->sink, &p, &room);
        if (rc) return rc;
        ssize_t r = read(f->pipe, p, room < budget ? room : (size_t)budget);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { if (errno == EAGAIN) break; return st_io_fail(c, "read", "the hactool output"); }
        if (!r) { if (until_eof || f->bytes) f->pipe_eof = 1; break; }
        if ((rc = romfs_writer_commit(c, f->sink, (size_t)r))) return rc;
        f->bytes += (uint64_t)r; budget -= (uint64_t)r;
        if (until_eof && setup_cancelled(c)) return SETUP_CANCELLED;
    }
    return 0;
}

int hactool_run(SetupCtx *c, const char *stage, const char *const *args, const SetupFdMap *fds, int nfds,
                const char *const *env, const char *log_path, uint64_t progress_total,
                SetupFollow *follow, char **log_text, int *exit_code) {
    char exe[SETUP_PATH];
    if (st_path(exe, sizeof exe, "%s/libhactool.so", c->lib) || access(exe, X_OK)) {
        setup_fail(c, "internal", "The bundled hactool is missing (%s); reinstall the app", "libhactool.so");
        return -1;
    }
    int pipefd[2] = { -1, -1 }, high[16], nh = 0, rc = -1, logfd = -1;
    uint64_t dropped = 0;
    pid_t pid = -1;
    Buf log = {0}, partial = {0};
    char **argv = NULL, **envp = NULL;
    posix_spawn_file_actions_t fa; int fa_ok = 0;
    if (nfds > 16) { setup_fail(c, "internal", "Too many hactool inputs"); return -1; }
    if (pipe2(pipefd, O_CLOEXEC)) { st_io_fail(c, "create a pipe for", "hactool"); goto out; }
    logfd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (logfd < 0) { st_io_fail(c, "create", log_path); goto out; }
    /* Move every source above the fixed child numbers first so one mapping can never clobber another's source. */
    for (int i = 0; i < nfds; i++) {
        high[i] = fcntl(fds[i].src_fd, F_DUPFD_CLOEXEC, 100);
        if (high[i] < 0) { st_io_fail(c, "duplicate", "an input descriptor"); goto out; }
        nh++;
    }
    size_t na = 0, ne = 0, nenv = 0;
    while (args[na]) na++;
    while (env && env[ne]) ne++;
    while (environ && environ[nenv]) nenv++;
    argv = calloc(na + 2, sizeof *argv); envp = calloc(nenv + ne + 1, sizeof *envp);
    if (!argv || !envp) { setup_fail(c, "internal", "Out of memory"); goto out; }
    argv[0] = exe;
    for (size_t i = 0; i < na; i++) argv[i + 1] = (char *)args[i];
    size_t k = 0;
    /* The parent's MNG_* hooks never leak in, and our variables replace the parent's of the same name. */
    for (size_t i = 0; i < nenv; i++) {
        int keep = strncmp(environ[i], "MNG_", 4) != 0;
        const char *eq = strchr(environ[i], '=');
        for (size_t j = 0; j < ne && keep && eq; j++) if (!strncmp(env[j], environ[i], (size_t)(eq - environ[i]) + 1)) keep = 0;
        if (keep) envp[k++] = environ[i];
    }
    for (size_t i = 0; i < ne; i++) envp[k++] = (char *)env[i];
    if (posix_spawn_file_actions_init(&fa)) { setup_fail(c, "internal", "posix_spawn_file_actions_init failed"); goto out; }
    fa_ok = 1;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
    for (int i = 0; i < nfds; i++) posix_spawn_file_actions_adddup2(&fa, high[i], fds[i].child_fd);
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, envp);
    if (e) { errno = e; pid = -1; st_io_fail(c, "start", "hactool"); goto out; }
    close(pipefd[1]); pipefd[1] = -1;
    int status = 0, exited = 0, eof = 0, killed = 0;
    uint64_t last = 0;
    int sink = follow && follow->sink;
    while (!exited || !eof) {
        if (!killed && setup_cancelled(c)) { kill(pid, SIGKILL); killed = 1; }
        /* A FIFO output is polled too: hactool blocks on it as soon as its pipe buffer is full. */
        struct pollfd pf[2] = { { eof ? -1 : pipefd[0], POLLIN, 0 }, { sink && !follow->pipe_eof && !killed ? follow->pipe : -1, POLLIN, 0 } };
        if (poll(pf, 2, 100) > 0 && pf[0].revents) {
            char tmp[4096]; ssize_t r = read(pipefd[0], tmp, sizeof tmp);
            if (r > 0) {
                buf_add(&partial, tmp, (size_t)r);
                char *nl;
                while (partial.p && (nl = memchr(partial.p, '\n', partial.n))) {
                    size_t n = (size_t)(nl - partial.p) + 1;
                    char *line = malloc(n + 1); if (!line) break;
                    memcpy(line, partial.p, n); line[n] = 0;
                    hactool_scrub(line);
                    st_write(logfd, line, strlen(line));
                    if (log.n < (4u << 20)) buf_puts(&log, line);
                    free(line);
                    memmove(partial.p, partial.p + n, partial.n - n); partial.n -= n; partial.p[partial.n] = 0;
                }
            } else if (r == 0 || (errno != EINTR && errno != EAGAIN)) eof = 1;
        }
        if (!exited) { pid_t w = waitpid(pid, &status, WNOHANG); if (w == pid) exited = 1; }
        if (sink && !killed) { int f = follow_pipe(c, follow, 0); if (f) { kill(pid, SIGKILL); killed = 1; } }
        uint64_t now = st_now_ms();
        if (sink && progress_total && now - last >= 100) {
            setup_progress(c, stage, follow->bytes, progress_total, "hactool");
            last = now;
            /* The writer bounds the cache of what it writes; drop what the child has read of its inputs. */
            if (follow->bytes >= dropped + (64ull << 20)) { dropped = follow->bytes; for (int i = 0; i < nh; i++) st_cache_drop(high[i], 0, 0); }
        }
    }
    pid = -1;
    if (partial.n) { hactool_scrub(partial.p); st_write(logfd, partial.p, strlen(partial.p)); buf_puts(&log, partial.p); }
    if (killed) { if (!c->failed) rc = SETUP_CANCELLED; goto out; }
    if (sink) { int f = follow_pipe(c, follow, 1); if (f) { rc = f; goto out; } }
    if (!WIFEXITED(status)) { setup_fail(c, "internal", "hactool crashed (signal %d)", WIFSIGNALED(status) ? WTERMSIG(status) : 0); goto out; }
    *exit_code = WEXITSTATUS(status);
    if (log.oom) { setup_fail(c, "internal", "Out of memory"); goto out; }
    rc = c->failed ? -1 : 0;
out:
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    if (fa_ok) posix_spawn_file_actions_destroy(&fa);
    for (int i = 0; i < nh; i++) close(high[i]);
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (logfd >= 0) { fsync(logfd); close(logfd); }
    free(argv); free(envp); buf_free(&partial);
    if (log_text && rc != SETUP_CANCELLED) { *log_text = log.p ? log.p : strdup(""); log.p = NULL; }
    buf_free(&log);
    return rc;
}
