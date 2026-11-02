/* The translation half of prepare_shader_seed.py prepare(), with the bundled Vulkan service
   (<libdir>/libtotk_xlate.so --service --vulkan --out <dir> --chain totk-native-v5-unverified, tools/totk_xlate/Service.cs)
   instead of the Metal service + android/vk_reflect.py: the service writes each variant's .spv and .vk and the shader's
   .vv rows itself, byte for byte what the PC pipeline places on the device (android/APP.md, on-device translation).

   Several services run in parallel, one shader at a time each: a shader's jobs (its default bind state, then its listed
   ones) run in order on one service, because whether a listed job is translated and kept depends on that shader's
   earlier results; each shader's files are named after it. The result equals translating everything in job order. */
#define _GNU_SOURCE 1
#include "seed_int.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#define MAX_RESTARTS 16   /* service crashes (e.g. the low-memory killer) tolerated per run */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static const char *stage_name(int s) { return s == 0 ? "vs" : s == 1 ? "ps" : "cs"; }

typedef struct {
    SeedTranslate *t;
    pid_t pid; int in, out;
    char rbuf[8192]; size_t rn;
    Buf req;
    char **env;
} Service;

static _Atomic size_t next_group;
static _Atomic int restarts;
static pthread_mutex_t pid_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t *live_pids; static int nlive;

/* The service's environment without the switches that change translation (Service.cs Run: TOTK_SHADER_DEBUGMODE,
   TOTK_GATHER_BIAS): a seed always uses the defaults the runtime's own on-device fills use. The GC gets a 32 MiB gen0
   budget (the value is hex): on the Thor the shader stage translated 18% faster with it (41.5 s -> 34 s, same seed-id)
   for about 28 MiB more memory per service; DOTNET_gcConcurrent=0 gained less (35.5 s). */
static char **service_env(void) {
    static char gen0[] = "DOTNET_GCgen0size=2000000";
    size_t n = 0;
    while (environ && environ[n]) n++;
    char **env = calloc(n + 2, sizeof *env);
    if (!env) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (strncmp(environ[i], "TOTK_SHADER_DEBUGMODE=", 22) && strncmp(environ[i], "TOTK_GATHER_BIAS=", 17)
            && strncmp(environ[i], "DOTNET_GCgen0size=", 18)) env[k++] = environ[i];
    env[k] = gen0;
    return env;
}
static void service_stop(Service *s, int kill_it) {
    if (s->in >= 0) close(s->in);
    if (s->out >= 0) close(s->out);
    s->in = s->out = -1;
    if (s->pid > 0) {
        /* Forget the pid before reaping it, so the cancel path never signals a reused pid. */
        pthread_mutex_lock(&pid_mu);
        for (int i = 0; i < nlive; i++) if (live_pids[i] == s->pid) live_pids[i] = 0;
        pthread_mutex_unlock(&pid_mu);
        if (kill_it) kill(s->pid, SIGKILL);
        while (waitpid(s->pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    s->pid = 0; s->rn = 0;
}
static int service_start(Service *s, int slot) {
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return -1;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return -1; }
    posix_spawn_file_actions_t a;
    int error = posix_spawn_file_actions_init(&a);
    if (!error) error = posix_spawn_file_actions_adddup2(&a, in[0], STDIN_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&a, out[1], STDOUT_FILENO);
    char *argv[] = { (char *)s->t->translator, "--service", "--vulkan", "--out", (char *)s->t->out_dir, "--chain", SEED_CHAIN, NULL };
    pid_t pid = -1;
    /* Spawned from this thread, whose nice value (+10) the child inherits. */
    if (!error) error = posix_spawn(&pid, s->t->translator, &a, NULL, argv, s->env);
    posix_spawn_file_actions_destroy(&a);
    close(in[0]); close(out[1]);
    if (error) { close(in[1]); close(out[0]); errno = error; return -1; }
    s->pid = pid; s->in = in[1]; s->out = out[0]; s->rn = 0;
    pthread_mutex_lock(&pid_mu); live_pids[slot] = pid; pthread_mutex_unlock(&pid_mu);
    /* Nonblocking parent ends: a service that stops reading or replying cannot hold this thread past its deadline. */
    if (fcntl(s->in, F_SETFL, O_NONBLOCK) || fcntl(s->out, F_SETFL, O_NONBLOCK)) { service_stop(s, 1); return -1; }
    return 0;
}
/* Waits for fd until the deadline, in short slices so a stop is seen promptly. 0 ready, -1 gone, -2 stopped, -3 late. */
static int wait_fd(Service *s, int fd, short events, uint64_t deadline) {
    for (;;) {
        if (atomic_load(s->t->stop)) return -2;
        uint64_t now = st_now_ms();
        if (now >= deadline) return -3;
        struct pollfd p = { .fd = fd, .events = events };
        int ms = deadline - now < 250 ? (int)(deadline - now) : 250;
        int n = poll(&p, 1, ms);
        if (n > 0) return p.revents & events ? 0 : -1;   /* POLLHUP / POLLERR without data: the service is gone */
        if (n < 0 && errno != EINTR) return -1;
    }
}
/* One request line out, one reply line back within timeout_ms (the watchdog of a hung service).
   0 ok, -1 the service is gone (crashed or killed), -2 stopping, -3 no reply in time. */
static int exchange(Service *s, const char *line, size_t n, char *reply, size_t cap) {
    uint64_t deadline = st_now_ms() + s->t->timeout_ms;
    for (size_t i = 0; i < n; ) {
        ssize_t k = write(s->in, line + i, n - i);
        if (k > 0) { i += (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && errno != EAGAIN) return -1;
        int rc = wait_fd(s, s->in, POLLOUT, deadline);
        if (rc) return rc;
    }
    for (;;) {
        char *nl = memchr(s->rbuf, '\n', s->rn);
        if (nl) {
            size_t len = (size_t)(nl - s->rbuf), m = len < cap - 1 ? len : cap - 1;
            memcpy(reply, s->rbuf, m); reply[m] = 0;
            memmove(s->rbuf, nl + 1, s->rn - len - 1); s->rn -= len + 1;
            return 0;
        }
        if (s->rn >= sizeof s->rbuf) s->rn = 0;   /* an over-long reply: keep only its end */
        char *at = s->rbuf + s->rn; size_t room = sizeof s->rbuf - s->rn;
        ssize_t k = read(s->out, at, room < sizeof s->rbuf ? room : sizeof s->rbuf);
        if (k > 0) { s->rn += (size_t)k; continue; }
        if (k == 0) return -1;
        if (errno == EINTR) continue;
        if (errno != EAGAIN) return -1;
        int rc = wait_fd(s, s->out, POLLIN, deadline);
        if (rc) return rc;
    }
}

static void hex(Buf *b, const uint8_t *p, size_t n) {
    static const char x[] = "0123456789abcdef";
    char tmp[4096];
    for (size_t i = 0; i < n; ) {
        size_t k = 0;
        for (; i < n && k < sizeof tmp; i++) { tmp[k++] = x[p[i] >> 4]; tmp[k++] = x[p[i] & 15]; }
        buf_add(b, tmp, k);
    }
}
/* The request prepare_shader_seed.py sends (code as code_hex, as the runtime's own requests do). */
static void request(Buf *b, const SeedJob *j, const uint8_t *control, const uint8_t *code) {
    const SeedShader *s = j->shader;
    uint32_t c1 = le32(control + 0x700), length = le32(control + 0x704), skip = s->stage == 5 ? 0 : 0x30;
    b->n = 0; if (b->p) b->p[0] = 0;
    buf_printf(b, "{\"variant\":\"%s\",\"stage\":\"%s\",\"code_hex\":\"", j->variant, stage_name(s->stage));
    if (length > skip) hex(b, code + skip, length - skip);
    buf_puts(b, "\",\"c1\":\"");
    hex(b, code + c1, length - c1);
    /* Only the 0x880-byte NVN layout defines the early-tests byte. */
    int early = s->stage == 1 && s->control_size == 0x880 && control[0x718];
    buf_printf(b, "\",\"depth_minus_w\":true,\"early_fragment_tests\":%s", early ? "true" : "false");
    static const char *const order[4] = { "tex", "img", "attr", "fout" };   /* {**ANSWERS, **deps} key order */
    static const int kind_index[4] = { 3, 2, 0, 1 };
    for (int k = 0; k < 4; k++) {
        buf_printf(b, ",\"%s\":", order[k]);
        const SeedListed *l = j->listed; int ki = kind_index[k];
        if (l && l->kind_len[ki]) buf_add(b, l->deps + l->kind_off[ki], l->kind_len[ki]); else buf_puts(b, "{}");
    }
    if (s->stage == 5) buf_printf(b, ",\"local\":[%u, %u, %u]", le32(control + 0x748), le32(control + 0x74c), le32(control + 0x750));
    buf_puts(b, "}\n");
}

/* validate_spirv_stage(): the SPIR-V's only entry point has the requested execution model. */
static int spirv_stage_ok(const char *path, int stage) {
    char *data; size_t n;
    if (st_read_file(path, &data, &n, 256u << 20)) return 0;
    const uint8_t *w = (const uint8_t *)data;
    int ok = n >= 20 && n % 4 == 0 && le32(w) == 0x07230203, models = 0;
    uint32_t expected = stage == 0 ? 0 : stage == 1 ? 4 : 5;
    for (size_t i = 5; ok && i < n / 4; ) {
        uint32_t word = le32(w + 4 * i), len = word >> 16, op = word & 0xffff;
        if (!len || i + len > n / 4) { ok = 0; break; }
        if (op == 15) { if (len < 4 || le32(w + 4 * (i + 1)) != expected) ok = 0; models++; }
        i += len;
    }
    free(data);
    return ok && models == 1;
}
/* The flat deps the service advertised for a variant (its row in <key>.vv). */
static int vv_row(const char *dir, const char *key, const char *variant, char **flat) {
    char path[SETUP_PATH], *text; size_t n;
    *flat = NULL;
    if (st_path(path, sizeof path, "%s/%s.vv", dir, key) || st_read_file(path, &text, &n, 64u << 20)) return -1;
    size_t vl = strlen(variant);
    for (char *line = text; line < text + n; ) {
        char *end = memchr(line, '\n', (size_t)(text + n - line)); if (!end) end = text + n;
        if ((size_t)(end - line) > vl && !memcmp(line, variant, vl) && line[vl] == '\t') {
            *flat = strndup(line + vl + 1, (size_t)(end - line) - vl - 1);
            break;
        }
        line = end + 1;
    }
    free(text);
    return *flat ? 0 : -1;
}
/* The deps of a flat row as canonical JSON (json.dumps(deps, sort_keys=True)) with all four kinds, to compare with the
   answers of a listed job as prepare_shader_seed.py compares them with the translator's reflection 'deps'. Slots sort as
   strings there; an image slot is {"fmt", "type"} ("type/fmt"), {"type"} or {} (empty). */
typedef struct { const char *slot; size_t sn; const char *value; size_t vn; } Dep;
static int by_slot(const void *a, const void *b) {
    const Dep *x = a, *y = b; int c = memcmp(x->slot, y->slot, x->sn < y->sn ? x->sn : y->sn);
    return c ? c : (x->sn > y->sn) - (x->sn < y->sn);
}
static char *flat_json(const char *flat) {
    static const char *const kinds[4] = { "attr", "fout", "img", "tex" };
    size_t count = 1; for (const char *p = flat; *p; p++) count += *p == ';';
    Dep *d = calloc(count, sizeof *d); Buf b = {0};
    if (!d) return NULL;
    buf_puts(&b, "{");
    for (int k = 0; k < 4; k++) {
        size_t kl = strlen(kinds[k]), n = 0;
        for (const char *p = flat; *p; ) {
            const char *end = strchr(p, ';'); if (!end) end = p + strlen(p);
            const char *colon = memchr(p, ':', (size_t)(end - p)), *eq = colon ? memchr(colon, '=', (size_t)(end - colon)) : NULL;
            if (colon && eq && (size_t)(colon - p) == kl && !memcmp(p, kinds[k], kl))
                d[n++] = (Dep){ colon + 1, (size_t)(eq - colon - 1), eq + 1, (size_t)(end - eq - 1) };
            p = *end ? end + 1 : end;
        }
        qsort(d, n, sizeof *d, by_slot);
        buf_printf(&b, "%s\"%s\":{", k ? "," : "", kinds[k]);
        for (size_t i = 0; i < n; i++) {
            buf_printf(&b, "%s\"%.*s\":", i ? "," : "", (int)d[i].sn, d[i].slot);
            if (k == 2) {
                const char *slash = memchr(d[i].value, '/', d[i].vn);
                if (slash) buf_printf(&b, "{\"fmt\":\"%.*s\",\"type\":\"%.*s\"}", (int)(d[i].vn - (size_t)(slash - d[i].value) - 1), slash + 1,
                                      (int)(slash - d[i].value), d[i].value);
                else if (d[i].vn) buf_printf(&b, "{\"type\":\"%.*s\"}", (int)d[i].vn, d[i].value);
                else buf_puts(&b, "{}");
            } else buf_printf(&b, "\"%.*s\"", (int)d[i].vn, d[i].value);
        }
        buf_puts(&b, "}");
    }
    buf_puts(&b, "}");
    free(d);
    if (b.oom) { buf_free(&b); return NULL; }
    return b.p;
}

typedef struct { const char *variant; char *flat, *json; } Kept;
static void unlink_variant(const char *dir, const char *variant) {
    char p[SETUP_PATH];
    if (!st_path(p, sizeof p, "%s/%s.spv", dir, variant)) unlink(p);
    if (!st_path(p, sizeof p, "%s/%s.vk", dir, variant)) unlink(p);
}
/* The loose seed is a work folder the pack replaces (a failed or cancelled run keeps nothing, and the service of this
   group has already replied), so its .vv needs neither fsync nor a rename: two fewer fsyncs per shader. */
static int write_vv(SeedTranslate *t, const char *key, Kept *kept, size_t n) {
    char path[SETUP_PATH];
    if (st_path(path, sizeof path, "%s/%s.vv", t->out_dir, key)) return -1;
    if (!n) { unlink(path); return 0; }
    Buf b = {0};
    for (size_t i = 0; i < n; i++) { buf_puts(&b, kept[i].variant); buf_puts(&b, "\t"); buf_puts(&b, kept[i].flat); buf_puts(&b, "\n"); }
    int rc = -1, fd = b.oom ? -1 : open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) { rc = st_write(fd, b.p, b.n); int e = errno; if (close(fd)) rc = -1; else errno = e; }
    buf_free(&b);
    return rc;
}
static void log_failure(SeedTranslate *t, const SeedJob *j, const char *error) {
    char key[65]; sha256_hex(j->shader->key, key);
    pthread_mutex_lock(&t->log_mu);
    buf_printf(&t->log, "%s\t%s\t%s\t%s\t%s\n", key, j->variant, j->listed ? "listed" : "default", stage_name(j->shader->stage), error);
    pthread_mutex_unlock(&t->log_mu);
}

/* ---- resume: <work>/translated.journal lists the shaders whose files are complete in out_dir ----
   One line per finished shader group: <key> <kept> <skipped> <failed>. Lines are appended in batches, each only after a
   syncfs of out_dir, so a listed shader's files are on disk even after a power loss; a lost tail only repeats work. A run
   that resumes the same scratch (stage_seed.c) skips the listed shaders and starts every other one over. */
#define JOURNAL_BATCH 256
#define JOURNAL_MS 2000
static pthread_mutex_t journal_mu = PTHREAD_MUTEX_INITIALIZER, flush_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int journal_fd = -1; static int out_fd = -1;
static Buf journal_pending; static size_t journal_n; static uint64_t journal_last;
static uint8_t *group_done;

/* Writes a taken batch: syncfs first, so the batch's spv/vk/vv files reach the disk before the lines that vouch for
   them. Only the worker that took the batch waits for it; a failure ends journaling (a resume then repeats more). */
static void journal_write(Buf *batch) {
    pthread_mutex_lock(&flush_mu);
    int fd = atomic_load(&journal_fd);
    if (fd >= 0 && (batch->oom || syncfs(out_fd) || st_write(fd, batch->p, batch->n))) { atomic_store(&journal_fd, -1); close(fd); }
    pthread_mutex_unlock(&flush_mu);
    buf_free(batch);
}
static void journal_done(const char *key, size_t kept, uint64_t skipped, uint64_t failed) {
    if (atomic_load(&journal_fd) < 0) return;
    Buf batch = {0};
    pthread_mutex_lock(&journal_mu);
    buf_printf(&journal_pending, "%s %zu %llu %llu\n", key, kept, (unsigned long long)skipped, (unsigned long long)failed);
    if (++journal_n >= JOURNAL_BATCH || st_now_ms() - journal_last >= JOURNAL_MS) {
        batch = journal_pending; memset(&journal_pending, 0, sizeof journal_pending); journal_n = 0; journal_last = st_now_ms();
    }
    pthread_mutex_unlock(&journal_mu);
    if (batch.n || batch.oom) journal_write(&batch);
}
typedef struct { uint8_t key[32]; uint64_t kept, skipped, failed; } Journaled;
static int by_key(const void *a, const void *b) { return memcmp(a, b, 32); }
static int unhex(const char *s, uint8_t out[32]) {
    for (int i = 0; i < 64; i++) {
        int c = s[i], v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return -1;
        if (i & 1) out[i / 2] |= (uint8_t)v; else out[i / 2] = (uint8_t)(v << 4);
    }
    return 0;
}
/* Marks the groups the journal lists as done (when resuming), rewrites it with its whole lines only, and opens it for
   appending. Best effort: without a journal the run simply cannot be resumed. */
static void journal_open(SeedTranslate *t) {
    atomic_store(&journal_fd, -1); journal_n = 0; memset(&journal_pending, 0, sizeof journal_pending); journal_last = st_now_ms();
    t->resumed = 0;
    if (!t->journal || (out_fd = open(t->out_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) return;
    char *text = NULL; size_t n = 0; Journaled *v = NULL; size_t nv = 0;
    Buf keep = {0};
    if (t->resuming && !st_read_file(t->journal, &text, &n, 64u << 20) && (v = calloc(n / 70 + 1, sizeof *v))) {
        for (char *line = text; line < text + n; ) {
            char *end = memchr(line, '\n', (size_t)(text + n - line));
            if (!end) break;   /* a torn last line */
            *end = 0;
            unsigned long long a, b, c; char tail;
            if (end - line > 65 && line[64] == ' ' && !unhex(line, v[nv].key) && sscanf(line + 65, "%llu %llu %llu%c", &a, &b, &c, &tail) == 3) {
                v[nv].kept = a; v[nv].skipped = b; v[nv].failed = c; nv++;
                buf_puts(&keep, line); buf_puts(&keep, "\n");
            }
            line = end + 1;
        }
        qsort(v, nv, sizeof *v, by_key);
        for (size_t g = 0; g < t->plan->ngroups; g++) {
            const SeedGroup *gr = &t->plan->groups[g]; SeedShader *s = t->plan->jobs[gr->first].shader;
            const Journaled *j = nv ? bsearch(s->key, v, nv, sizeof *v, by_key) : NULL;
            if (!j) continue;
            group_done[g] = 1; t->resumed++;
            if (j->kept) s->produced = 1;
            atomic_fetch_add(&t->ok, j->kept); atomic_fetch_add(&t->skipped, j->skipped); atomic_fetch_add(&t->failed, j->failed);
            atomic_fetch_add(&t->done, gr->n);
        }
    }
    if (!keep.oom && !st_write_atomic(t->journal, keep.p ? keep.p : "", keep.n))
        atomic_store(&journal_fd, open(t->journal, O_WRONLY | O_APPEND | O_CLOEXEC));
    free(text); free(v); buf_free(&keep);
}
static void journal_close(void) {
    Buf batch = journal_pending;   /* the workers have been joined: every listed group is finished */
    memset(&journal_pending, 0, sizeof journal_pending); journal_n = 0;
    if (batch.n || batch.oom) journal_write(&batch); else buf_free(&batch);
    int fd = atomic_exchange(&journal_fd, -1);
    if (fd >= 0) close(fd);
    if (out_fd >= 0) close(out_fd);
    out_fd = -1;
}

/* One shader's jobs. Returns -1 on a stage failure (recorded), -2 when stopping. */
static int run_group(Service *s, int slot, const SeedGroup *g) {
    SeedTranslate *t = s->t; SeedShader *shader = t->plan->jobs[g->first].shader;
    uint8_t *control = NULL, *code = NULL;
    Kept *kept = calloc(g->n, sizeof *kept); size_t nkept = 0;
    char key[65]; sha256_hex(shader->key, key);
    int rc = 0; uint64_t gskip = 0, gfail = 0;
    if (t->resuming) {   /* an interrupted run may have left part of this shader's files: start it over */
        for (size_t i = 0; i < g->n; i++) unlink_variant(t->out_dir, t->plan->jobs[g->first + i].variant);
        char vv[SETUP_PATH]; if (!st_path(vv, sizeof vv, "%s/%s.vv", t->out_dir, key)) unlink(vv);
    }
    if (!kept || seed_table_bytes(t->table, shader, &control, &code)) {
        seed_err(t->err, "io", "Cannot read the shader work file: %s", strerror(errno ? errno : ENOMEM)); rc = -1; goto out;
    }
    for (size_t i = 0; i < g->n && !rc; i++) {
        const SeedJob *j = &t->plan->jobs[g->first + i];
        int skip = 0;
        for (size_t k = 0; k < nkept && !skip; k++) skip = !strcmp(kept[k].variant, j->variant);
        if (!skip && j->listed) {   /* a listed state the shader's earlier variants already cover */
            Buf a = {0}; seed_answers_json(j->listed, &a);
            for (size_t k = 0; k < nkept && !skip && a.p; k++) skip = !strcmp(kept[k].json, a.p);
            buf_free(&a);
        }
        if (skip) { gskip++; atomic_fetch_add(&t->skipped, 1); atomic_fetch_add(&t->done, 1); continue; }
        request(&s->req, j, control, code);
        if (s->req.oom) { seed_err(t->err, "internal", "Out of memory"); rc = -1; break; }
        char reply[1024]; int tries = 0;
        for (;;) {
            if (atomic_load(t->stop)) { rc = -2; break; }
            if (s->pid <= 0 && service_start(s, slot)) {
                seed_err(t->err, "not_available", "Cannot start the shader translator %s: %s", t->translator, strerror(errno)); rc = -1; break;
            }
            int x = exchange(s, s->req.p, s->req.n, reply, sizeof reply);
            if (!x) break;
            service_stop(s, 1);
            unlink_variant(t->out_dir, j->variant);   /* whatever a killed service may have published for it */
            if (x == -2 || atomic_load(t->stop)) { rc = -2; break; }
            /* prepare_shader_seed.py stops on a service exit. On a phone the low-memory killer can end one; restart
               it and retry the job once, then count the job as failed rather than discarding the whole seed. A
               service with no reply in time is killed the same way, but its job is not retried: it would only hang
               again. Both count towards one bound, so a translator that never answers fails the stage. */
            if (atomic_fetch_add(&restarts, 1) >= MAX_RESTARTS) {
                seed_err(t->err, "internal", "The shader translator keeps exiting or hanging (last: %s)", j->variant); rc = -1; break;
            }
            if (x == -3) { snprintf(reply, sizeof reply, "{\"ok\": false, \"error\": \"timeout: no reply in %llu ms\"}", (unsigned long long)t->timeout_ms); break; }
            if (++tries == 2) { snprintf(reply, sizeof reply, "{\"ok\": false, \"error\": \"translator exited\"}"); break; }
        }
        if (rc) break;
        if (!strncmp(reply, "{\"ok\": true", 11)) {
            char spv[SETUP_PATH], *flat = NULL, *json = NULL;
            if (st_path(spv, sizeof spv, "%s/%s.spv", t->out_dir, j->variant) || !spirv_stage_ok(spv, shader->stage)) {
                seed_err(t->err, "internal", "prepared shader stage mismatch for %s", j->variant); rc = -1; break;
            }
            if (vv_row(t->out_dir, key, j->variant, &flat) || !(json = flat_json(flat))) {
                free(flat); seed_err(t->err, "internal", "The translator did not advertise %s", j->variant); rc = -1; break;
            }
            int dup = 0;
            for (size_t k = 0; k < nkept && !dup; k++) dup = !strcmp(kept[k].flat, flat);
            if (dup) { unlink_variant(t->out_dir, j->variant); free(flat); free(json); gskip++; atomic_fetch_add(&t->skipped, 1); }
            else { kept[nkept++] = (Kept){ j->variant, flat, json }; atomic_fetch_add(&t->ok, 1); }
        } else {
            if (strstr(reply, "\"stage_mismatch\": true")) { seed_err(t->err, "internal", "prepared shader stage mismatch for %s", j->variant); rc = -1; break; }
            const char *e = strstr(reply, "\"error\": ");
            log_failure(t, j, e ? e + 9 : reply);
            gfail++; atomic_fetch_add(&t->failed, 1);
        }
        atomic_fetch_add(&t->done, 1);
    }
    if (!rc) {
        if (write_vv(t, key, kept, nkept)) { seed_err(t->err, errno == ENOSPC ? "no_space" : "io", "Cannot write %s.vv: %s", key, strerror(errno)); rc = -1; }
        else { if (nkept) shader->produced = 1; journal_done(key, nkept, gskip, gfail); }
    }
out:
    for (size_t k = 0; k < nkept; k++) { free(kept[k].flat); free(kept[k].json); }
    free(kept); free(control); free(code);
    return rc;
}

typedef struct { SeedTranslate *t; int slot; } WorkerArg;
static _Atomic int running;
static void *worker(void *arg) {
    WorkerArg *w = arg; SeedTranslate *t = w->t;
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGPIPE); pthread_sigmask(SIG_BLOCK, &mask, NULL);
    setpriority(PRIO_PROCESS, 0, 10);   /* Linux nice is per thread; the services spawned here inherit it */
    Service s; memset(&s, 0, sizeof s); s.t = t; s.in = s.out = -1;
    if (!(s.env = service_env())) { seed_err(t->err, "internal", "Out of memory"); atomic_store(t->stop, 1); atomic_fetch_sub(&running, 1); return NULL; }
    for (;;) {
        if (atomic_load(t->stop)) break;
        size_t g = atomic_fetch_add(&next_group, 1);
        if (g >= t->plan->ngroups) break;
        if (group_done[g]) continue;   /* finished by the interrupted run this one resumes */
        int rc = run_group(&s, w->slot, &t->plan->groups[g]);
        if (rc) { if (rc == -1) atomic_store(t->stop, 1); break; }
    }
    service_stop(&s, atomic_load(t->stop));   /* closing stdin ends a healthy service */
    buf_free(&s.req); free(s.env);
    atomic_fetch_sub(&running, 1);
    return NULL;
}

int seed_translate_run(SeedTranslate *t) {
    atomic_store(&next_group, 0); atomic_store(&restarts, 0);
    pthread_t *threads = calloc((size_t)t->workers, sizeof *threads);
    WorkerArg *args = calloc((size_t)t->workers, sizeof *args);
    live_pids = calloc((size_t)t->workers, sizeof *live_pids); nlive = t->workers;
    group_done = calloc(t->plan->ngroups ? t->plan->ngroups : 1, 1);
    if (!threads || !args || !live_pids || !group_done) {
        free(threads); free(args); free(live_pids); free(group_done); group_done = NULL; seed_err(t->err, "internal", "Out of memory"); return -1;
    }
    journal_open(t);
    int started = 0;
    atomic_store(&running, 0);
    for (int i = 0; i < t->workers; i++) {
        args[i] = (WorkerArg){ t, i };
        atomic_fetch_add(&running, 1);
        if (pthread_create(&threads[i], NULL, worker, &args[i])) { atomic_fetch_sub(&running, 1); break; }
        started++;
    }
    if (!started) { seed_err(t->err, "internal", "Cannot start a thread"); atomic_store(t->stop, 1); }
    uint64_t total = t->plan->njobs, began = st_now_ms();
    /* This thread reports progress and polls cancellation; on cancel it ends the services so workers stop at once. */
    for (int killed = 0;;) {
        uint64_t done = atomic_load(&t->done);
        if (!atomic_load(&running)) break;
        if (setup_cancelled(t->c)) atomic_store(t->stop, 1);
        if (atomic_load(t->stop) && !killed) {
            killed = 1;
            pthread_mutex_lock(&pid_mu);
            for (int i = 0; i < nlive; i++) if (live_pids[i] > 0) kill(live_pids[i], SIGKILL);
            pthread_mutex_unlock(&pid_mu);
        }
        char detail[160]; uint64_t secs = (st_now_ms() - began) / 1000;
        snprintf(detail, sizeof detail, "translating shaders for Vulkan: %llu of %llu (%llu skipped), %llu:%02llu",
                 (unsigned long long)done, (unsigned long long)total, (unsigned long long)atomic_load(&t->failed),
                 (unsigned long long)(secs / 60), (unsigned long long)(secs % 60));
        setup_progress(t->c, "shaders", done, total, detail);
        struct timespec ts = { 0, 200000000 }; nanosleep(&ts, NULL);
    }
    for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    journal_close();
    free(threads); free(args); free(live_pids); live_pids = NULL; nlive = 0; free(group_done); group_done = NULL;
    if (seed_err_set(t->err)) return -1;
    if (atomic_load(t->stop) || setup_cancelled(t->c)) return SETUP_CANCELLED;
    return 0;
}
