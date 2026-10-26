/* A single low-priority child translates saved misses. The recording thread never waits for it.
   The child publishes .spv/.vk before .vv; a successful reply releases an epoch to the recorder,
   which alone owns Vulkan objects and shader/program maps. Requests remain available for PC fills. */
#include "vk_shader_xlate.h"
#include "vk_seed_io.h"
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
typedef struct xlate_job { struct xlate_job *next; char *request; char variant[80]; uint64_t miss_ns; } xlate_job;
static pthread_mutex_t xlate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t xlate_cv = PTHREAD_COND_INITIALIZER;
static xlate_job *xlate_head, *xlate_tail, *xlate_ready;
static pthread_once_t xlate_once = PTHREAD_ONCE_INIT;
static char xlate_exe[PATH_MAX], xlate_seed[PATH_MAX];
static int xlate_started;
static _Atomic int xlate_stopped;
static _Atomic uint64_t xlate_epoch;
static _Atomic int xlate_pid;

static uint64_t xlate_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
uint64_t vk_shader_xlate_epoch(void) { return atomic_load_explicit(&xlate_epoch, memory_order_acquire); }
uint64_t vk_shader_xlate_take_miss(const char *variant) {
    uint64_t miss = 0;
    pthread_mutex_lock(&xlate_mu);
    for (xlate_job **p = &xlate_ready; *p; p = &(*p)->next) if (!strcmp((*p)->variant, variant)) {
        xlate_job *job = *p; *p = job->next; miss = job->miss_ns; free(job); break;
    }
    pthread_mutex_unlock(&xlate_mu);
    return miss;
}
static void xlate_config(void) {
    const char *on = getenv("TOTK_XLATE_ONDEVICE");
    if (on && !strcmp(on, "0")) return;
    const char *override = getenv("TOTK_SHADER_TRANSLATOR");
    char path[PATH_MAX];
    if (override && *override) snprintf(path, sizeof path, "%s", override);
    else {
        Dl_info info;
        if (!dladdr((const void *)vk_shader_xlate_epoch, &info) || !info.dli_fname) return;
        const char *slash = strrchr(info.dli_fname, '/');
        if (!slash || (size_t)(slash - info.dli_fname) + sizeof "/libtotk_xlate.so" > sizeof path) return;
        snprintf(path, sizeof path, "%.*s/libtotk_xlate.so", (int)(slash - info.dli_fname), info.dli_fname);
    }
    if (!realpath(path, xlate_exe) || access(xlate_exe, X_OK)) { xlate_exe[0] = 0; return; }
    struct stat st;
    if (stat(xlate_exe, &st) || !S_ISREG(st.st_mode)) xlate_exe[0] = 0;
}
static void xlate_at_exit(void) {
    int pid = atomic_load(&xlate_pid);
    if (pid > 0) kill(pid, SIGTERM);
}
/* Parent pipe ends are nonblocking: a broken/hung service cannot trap the worker forever.
   SIGPIPE is blocked in this worker only; the game's signal dispositions are untouched. */
static int xlate_spawn(int *input, int *output) {
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return -1;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return -1; }
    posix_spawn_file_actions_t a;
    int error = posix_spawn_file_actions_init(&a);
    if (error) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); errno = error; return -1; }
    error = posix_spawn_file_actions_adddup2(&a, in[0], STDIN_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&a, out[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&a, in[1]);
    if (!error) error = posix_spawn_file_actions_addclose(&a, out[0]);
    char *argv[] = {xlate_exe, "--service", "--vulkan", "--out", xlate_seed, "--chain", "totk-native-v5-unverified", NULL};
    pid_t pid = -1;
    if (!error) error = posix_spawn(&pid, xlate_exe, &a, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&a);
    close(in[0]); close(out[1]);
    if (error) { close(in[1]); close(out[0]); errno = error; return -1; }
    atomic_store(&xlate_pid, pid);
    if (fcntl(in[1], F_SETFL, O_NONBLOCK) || fcntl(out[0], F_SETFL, O_NONBLOCK)) {
        close(in[1]); close(out[0]); kill(pid, SIGTERM); waitpid(pid, NULL, 0); atomic_store(&xlate_pid, 0); return -1;
    }
    *input = in[1]; *output = out[0];
    fprintf(stderr, "[shader-xlate] service pid=%d nice=10 executable=%s\n", (int)pid, xlate_exe);
    return 0;
}
static int xlate_poll(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = xlate_now();
        if (now >= deadline) { errno = ETIMEDOUT; return -1; }
        struct pollfd p = {.fd = fd, .events = events};
        int ms = (int)((deadline - now + 999999u) / 1000000u);
        int n = poll(&p, 1, ms);
        if (n > 0) { if (p.revents & events) return 0; errno = EPIPE; return -1; }
        if (n < 0 && errno != EINTR) return -1;
    }
}
static int xlate_exchange(int input, int output, const char *request, size_t n, char reply[1024]) {
    uint64_t deadline = xlate_now() + 120000000000ull;
    for (size_t i = 0; i < n; ) {
        if (xlate_poll(input, POLLOUT, deadline)) return -1;
        ssize_t k = write(input, request + i, n - i);
        if (k > 0) i += (size_t)k;
        else if (k < 0 && errno != EAGAIN && errno != EINTR) return -1;
    }
    size_t used = 0;
    while (used < 1023) {
        if (xlate_poll(output, POLLIN, deadline)) return -1;
        ssize_t k = read(output, reply + used, 1023 - used);
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (k <= 0) { errno = EPIPE; return -1; }
        used += (size_t)k; reply[used] = 0;
        if (strchr(reply, '\n')) return 0;
    }
    errno = EOVERFLOW; return -1;
}
static char *xlate_request(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    struct stat st;
    /* Two hex copies of at most 16 MiB (code + c1), plus JSON/answers. */
    if (fstat(fileno(f), &st) || st.st_size <= 0 || st.st_size > (65 << 20)) { fclose(f); errno = EINVAL; return NULL; }
    *n = (size_t)st.st_size;
    char *s = malloc(*n + 2);
    if (!s) { fclose(f); return NULL; }
    if (fread(s, 1, *n, f) != *n) { free(s); fclose(f); errno = EIO; return NULL; }
    fclose(f);
    if (s[*n - 1] != '\n') s[(*n)++] = '\n';
    s[*n] = 0; return s;
}
static void *xlate_worker(void *unused) {
    (void)unused;
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGPIPE); pthread_sigmask(SIG_BLOCK, &mask, NULL);
    setpriority(PRIO_PROCESS, 0, 10);   /* Linux nice is per thread; the child inherits it. */
    int input = -1, output = -1;
    if (xlate_spawn(&input, &output)) {
        fprintf(stderr, "[shader-xlate] startup failed: %s; PC requests retained\n", strerror(errno));
        atomic_store(&xlate_stopped, 1);
    }
    for (;;) {
        pthread_mutex_lock(&xlate_mu);
        while (!xlate_head && !atomic_load(&xlate_stopped)) pthread_cond_wait(&xlate_cv, &xlate_mu);
        xlate_job *job = xlate_head;
        if (job) { xlate_head = job->next; if (!xlate_head) xlate_tail = NULL; }
        pthread_mutex_unlock(&xlate_mu);
        if (!job) break;
        if (!atomic_load(&xlate_stopped)) {
            size_t n = 0; char *request = xlate_request(job->request, &n), reply[1024] = {0};
            uint64_t start = xlate_now();
            if (!request) fprintf(stderr, "[shader-xlate] %s request read failed: %s\n", job->variant, strerror(errno));
            else if (xlate_exchange(input, output, request, n, reply)) {
                fprintf(stderr, "[shader-xlate] %s service failed: %s; PC requests retained\n", job->variant, strerror(errno));
                atomic_store(&xlate_stopped, 1);
            } else {
                const char *ok = strstr(reply, "\"ok\""); ok = ok ? strchr(ok, ':') : NULL;
                if (ok) { ok++; while (isspace((unsigned char)*ok)) ok++; }
                if (ok && !strncmp(ok, "true", 4)) {
                    vk_seed_io_published(job->variant); /* index/loose overlays become visible before the epoch */
                    fprintf(stderr, "[shader-xlate] ready %s translate=%.3fms miss-to-ready=%.3fms\n", job->variant,
                            (double)(xlate_now() - start) / 1e6, (double)(xlate_now() - job->miss_ns) / 1e6);
                    pthread_mutex_lock(&xlate_mu);
                    free(job->request); job->request = NULL;
                    job->next = xlate_ready; xlate_ready = job; job = NULL;
                    pthread_mutex_unlock(&xlate_mu);
                    atomic_fetch_add_explicit(&xlate_epoch, 1, memory_order_release);
                } else fprintf(stderr, "[shader-xlate] %s translation rejected: %s", job->variant, reply);
            }
            free(request);
        }
        if (job) { free(job->request); free(job); }
    }
    if (input >= 0) close(input);
    if (output >= 0) close(output);
    int pid = atomic_exchange(&xlate_pid, 0);
    if (pid > 0) { kill(pid, SIGKILL); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} }
    return NULL;
}
void vk_shader_xlate_enqueue(const char *request, const char *variant, const char *seed) {
    pthread_once(&xlate_once, xlate_config);
    if (!xlate_exe[0] || atomic_load(&xlate_stopped)) return;
    xlate_job *job = calloc(1, sizeof *job);
    if (!job) return;
    job->request = strdup(request);
    if (!job->request) { free(job); return; }
    snprintf(job->variant, sizeof job->variant, "%s", variant); job->miss_ns = xlate_now();
    pthread_mutex_lock(&xlate_mu);
    if (atomic_load(&xlate_stopped)) { pthread_mutex_unlock(&xlate_mu); free(job->request); free(job); return; }
    if (!xlate_started) {
        if (!realpath(seed, xlate_seed)) {
            pthread_mutex_unlock(&xlate_mu); free(job->request); free(job); return;
        }
        pthread_t thread;
        int error = pthread_create(&thread, NULL, xlate_worker, NULL);
        if (error) {
            atomic_store(&xlate_stopped, 1); pthread_mutex_unlock(&xlate_mu); free(job->request); free(job);
            fprintf(stderr, "[shader-xlate] worker failed: %s\n", strerror(error)); return;
        }
        pthread_detach(thread); atexit(xlate_at_exit); xlate_started = 1;
    }
    if (xlate_tail) xlate_tail->next = job; else xlate_head = job;
    xlate_tail = job; pthread_cond_signal(&xlate_cv); pthread_mutex_unlock(&xlate_mu);
}
