/* CPU-only regression using the real scheduler, wait queues, grant path and
 * diagnostic snapshots. Only time, output and linkage are observed/replaced;
 * admission and pthread condition-variable waits are production code. */
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

static int observed_mutex_lock(pthread_mutex_t *mutex);
static int observed_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
static int observed_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline);
static int observed_clock_gettime(clockid_t clock, struct timespec *ts);
static void *observed_calloc(size_t count, size_t size);
static int observed_fprintf(FILE *stream, const char *format, ...);
static int observed_fputs(const char *text, FILE *stream);
static int observed_fputc(int ch, FILE *stream);
#define pthread_mutex_lock observed_mutex_lock
#define pthread_cond_wait observed_cond_wait
#define pthread_cond_timedwait observed_cond_timedwait
#define clock_gettime observed_clock_gettime
#define calloc observed_calloc
#define fprintf observed_fprintf
#define fputs observed_fputs
#define fputc observed_fputc
#include "../../runtime/threads.c"
#undef pthread_mutex_lock
#undef pthread_cond_wait
#undef pthread_cond_timedwait
#undef clock_gettime
#undef calloc
#undef fprintf
#undef fputs
#undef fputc

static _Atomic uint64_t fake_ns = 100, now_reads, locks, cond_entries, cpu_reads, allocations;
static _Atomic uint64_t fake_cpu = 1000;
static char output[65536];
static size_t output_size;
static int checking_output, remove_on_output;
static thr_t *disposable;
static int enabled, diag;

uint64_t runtime_now_ns(void) { atomic_fetch_add(&now_reads, 1); return atomic_load(&fake_ns); }
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest symbol"); }
void runtime_thread_init(void) {}
static void need(int condition, const char *message) { if (!condition) runtime_fail(message); }
static int observed_mutex_lock(pthread_mutex_t *mutex) {
    if (mutex == &G) atomic_fetch_add(&locks, 1);
    return pthread_mutex_lock(mutex);
}
static int observed_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    atomic_fetch_add(&cond_entries, 1);
    return pthread_cond_wait(cond, mutex);
}
static int observed_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline) {
    atomic_fetch_add(&cond_entries, 1);
    return pthread_cond_timedwait(cond, mutex, deadline);
}
static int observed_clock_gettime(clockid_t clock, struct timespec *ts) {
    if (clock == CLOCK_REALTIME || clock == CLOCK_MONOTONIC) return clock_gettime(clock, ts);
    atomic_fetch_add(&cpu_reads, 1);
    uint64_t ns = atomic_load(&fake_cpu);
    ts->tv_sec = (time_t)(ns / 1000000000ull); ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}
static void *observed_calloc(size_t count, size_t size) {
    atomic_fetch_add(&allocations, 1);
    return calloc(count, size);
}
static void check_output_unlocked(void) {
    if (!checking_output) return;
    need(!pthread_mutex_trylock(&G), "slow diagnostic output must run without scheduler G");
    if (remove_on_output) {
        remove_on_output = 0;
        /* An owner/row may disappear immediately after the snapshot. Free it
         * during the very first output operation, before later rows print. */
        for (int c = 0; c < NCORE; c++) if (g_core_owner[c] == disposable) g_core_owner[c] = NULL;
        thr_free(disposable); disposable = NULL;
    }
    pthread_mutex_unlock(&G);
}
static void append_output(const char *text, size_t count) {
    need(count < sizeof output - output_size, "bounded diagnostic capture");
    memcpy(output + output_size, text, count); output_size += count; output[output_size] = 0;
}
static int observed_fprintf(FILE *stream, const char *format, ...) {
    (void)stream; check_output_unlocked();
    char text[2048]; va_list args; va_start(args, format);
    int n = vsnprintf(text, sizeof text, format, args); va_end(args);
    need(n >= 0 && (size_t)n < sizeof text, "bounded diagnostic operation");
    append_output(text, (size_t)n); return n;
}
static int observed_fputs(const char *text, FILE *stream) {
    (void)stream; check_output_unlocked(); append_output(text, strlen(text)); return 0;
}
static int observed_fputc(int ch, FILE *stream) {
    (void)stream; check_output_unlocked(); char byte = (char)ch; append_output(&byte, 1); return ch;
}
static void clear_output(void) { output_size = 0; output[0] = 0; }
static void init(thr_t *t, uint32_t handle, int priority, uint64_t mask) {
    memset(t, 0, sizeof *t); t->handle = handle; t->prio = priority; t->mask = mask;
    t->core = 0; t->cur_core = -1; t->started = 1;
    t->pt = pthread_self(); t->mport = host_thread_port_self();
    need(!pthread_cond_init(&t->cv, NULL), "initialize thread condition");
}
static void check_totals(thr_t *t, uint64_t ns, uint64_t count) {
    need(t->core_wait_ns == (enabled ? ns : 0), "per-thread completed wait nanoseconds");
    need(t->core_nwait == (enabled ? count : 0), "per-thread completed wait count");
    need(g_core_wait_all_ns == (enabled ? ns : 0) && g_core_wait_all_n == (enabled ? count : 0), "all-thread completed waits");
}
static void await_wait(thr_t *t, uint64_t entries) {
    struct timespec start, now; clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        pthread_mutex_lock(&G);
        int ready = t->cqueued && atomic_load(&cond_entries) >= entries;
        pthread_mutex_unlock(&G);
        if (ready) return;
        clock_gettime(CLOCK_MONOTONIC, &now);
        need(now.tv_sec - start.tv_sec < 3, "scheduler condition wait deadline"); sched_yield();
    }
}
static void *take_core(void *argument) {
    thr_t *t = argument;
    pthread_setspecific(g_keythr, t);
    pthread_mutex_lock(&G);
    t->pt = pthread_self(); t->mport = host_thread_port_self();
    core_take(t);
    need(t->cur_core == 0 && g_core_owner[0] == t, "real blocked acquisition owns eligible core");
    pthread_mutex_unlock(&G);
    return NULL;
}
static void stats(thr_t *self, uint64_t ns, uint64_t count) {
    pthread_setspecific(g_keythr, self);
    uint64_t own = 99, all = 99, n = 99;
    dm_core_wait_stats(&own, &all, &n);
    need(own == (enabled && self ? ns : 0) && all == (enabled ? ns : 0) && n == (enabled ? count : 0), "presenter cumulative contract");
}
static void acquisition_and_completion(void) {
    thr_t owner, waiter; init(&owner, 0x1001, 48, 1); init(&waiter, 0x1002, 30, 1);
    owner.all = &waiter; g_all = &owner;
    pthread_setspecific(g_keythr, &owner);
    pthread_mutex_lock(&G);
    uint64_t reads = atomic_load(&now_reads);
    core_take(&owner);
    need(atomic_load(&now_reads) == reads + 1, "uncontended acquisition uses only existing scheduler clock");
    need(owner.cur_core == 0 && !owner.core_nwait, "uncontended acquisition is not a wait");
    atomic_store(&fake_ns, 200);
    pthread_t worker; need(!pthread_create(&worker, NULL, take_core, &waiter), "start actual scheduler waiter");
    pthread_mutex_unlock(&G); await_wait(&waiter, 1);
    pthread_mutex_lock(&G);
    need(g_core_owner[1] == NULL && g_core_owner[2] == NULL && waiter.cur_core < 0, "free cores outside mask do not relax arbitration");
    check_totals(&waiter, 0, 0);
    uint64_t entries = atomic_load(&cond_entries);
    atomic_store(&fake_ns, 300); pthread_cond_signal(&waiter.cv);
    pthread_mutex_unlock(&G); await_wait(&waiter, entries + 1);
    pthread_mutex_lock(&G);
    check_totals(&waiter, 0, 0); /* A wake or diagnostic timeout is not a grant. */
    atomic_store(&fake_ns, 500); core_put(&owner);
    check_totals(&waiter, 300, 1);
    need(waiter.cur_core == 0 && !waiter.core_stats_since, "wait completes at grant while host waiter cannot run");
    atomic_store(&fake_ns, 900); pthread_mutex_unlock(&G);
    need(!pthread_join(worker, NULL), "join acquired waiter");
    pthread_mutex_lock(&G);
    waiter.pt = pthread_self(); waiter.mport = host_thread_port_self();
    check_totals(&waiter, 300, 1);
    atomic_store(&fake_ns, 1000); core_put(&waiter);
    need(owner.core_held_ns == (diag ? 400u : 0u) && waiter.core_held_ns == (diag ? 500u : 0u), "held accounting includes grant-to-host-dispatch gap only when enabled");

    atomic_store(&fake_ns, 2000); core_take(&owner);
    atomic_store(&fake_ns, 2100); wake_queue(&waiter);
    need(waiter.cqueued && waiter.cur_core < 0, "ready-at-wake joins actual scheduler queue");
    uint64_t sequence = waiter.cseq; wake_queue(&waiter);
    need(waiter.cseq == sequence, "duplicate wake does not restart wait");
    check_totals(&waiter, 300, 1);
    atomic_store(&fake_ns, 2500); core_put(&owner);
    check_totals(&waiter, 700, 2);
    atomic_store(&fake_ns, 2900); core_take(&waiter);
    check_totals(&waiter, 700, 2); /* Already granted: no dispatch-tail charge. */
    atomic_store(&fake_ns, 3000); core_put(&waiter);

    atomic_store(&fake_ns, 4000); core_take(&owner);
    atomic_store(&fake_ns, 4100); wake_queue(&waiter);
    waiter.mask = 2; atomic_store(&fake_ns, 4500); core_take(&waiter);
    need(waiter.cur_core == 1 && g_core_owner[0] == &owner && !waiter.cqueued && !g_core_wait, "masked direct acquisition unqueues prior reservation");
    check_totals(&waiter, 1100, 3);
    atomic_store(&fake_ns, 4600); core_put(&waiter);
    atomic_store(&fake_ns, 4700); core_put(&owner);
    if (!enabled) need(!owner.core_stats_since && !waiter.core_stats_since && !owner.core_held_prev && !waiter.core_wait_prev, "disabled scheduler never starts diagnostic counters");
    pthread_mutex_unlock(&G);
    checking_output = 1; stats(&waiter, 1100, 3); stats(NULL, 1100, 3); checking_output = 0;
    g_all = NULL;
    pthread_cond_destroy(&owner.cv); pthread_cond_destroy(&waiter.cv);
}
static void snapshot_safety(void) {
    thr_t waiter; init(&waiter, 0x2001, 30, 1);
    disposable = calloc(1, sizeof *disposable); need(disposable != NULL, "snapshot owner allocation");
    init(disposable, 0xd00d, 66, 2); disposable->last_svc = 0x34;
    disposable->cur_core = 1; disposable->core_since = 5000;
    pthread_mutex_lock(&G);
    g_all = disposable; g_core_owner[1] = disposable; atomic_store(&fake_ns, 9000);
    clear_output(); checking_output = remove_on_output = 1;
    core_wait_report(&waiter, 1000);
    checking_output = 0;
    need(pthread_mutex_trylock(&G) == EBUSY, "owner report returns holding scheduler G");
    need(!disposable && !g_all && !g_core_owner[1], "owner can be destroyed during slow logging");
    need(strstr(output, "thread 0x2001") && strstr(output, "core 1 (not in mask): 0xd00d") && strstr(output, "last svc 0x34") && strstr(output, "core 2 (not in mask) free"), "owner/mask diagnostics use complete immutable scalar snapshot");
    pthread_mutex_unlock(&G);

    disposable = calloc(1, sizeof *disposable); need(disposable != NULL, "table thread allocation");
    init(disposable, 0xbeef, 55, 4); disposable->core_wait_ns = 7000000; disposable->core_nwait = 2; disposable->core_held_ns = 60000000;
    pthread_mutex_lock(&G); g_all = disposable;
    core_table_row_t row[1]; int truncated = 0;
    int n = core_table_snapshot(row, 1, 10000, 1, &truncated);
    need(n == 1 && !truncated && row[0].cpu_valid && row[0].cpu == 1000 && row[0].wait == 7000000 && row[0].count == 2 && row[0].held == 60000000, "snapshot captures exact CPU/held/completed-wait fields");
    atomic_store(&fake_cpu, 1300);
    n = core_table_snapshot(row, 1, 11000, 1, &truncated);
    need(n == 1 && row[0].cpu == 300 && !row[0].held && !row[0].wait && !row[0].count, "table windows use cumulative deltas without double charging");
    disposable->exited = 1; uint64_t reads = atomic_load(&cpu_reads);
    n = core_table_snapshot(row, 1, 12000, 1, &truncated);
    need(n == 1 && !row[0].cpu_valid && atomic_load(&cpu_reads) == reads, "exited host descriptors are never sampled");
    disposable->exited = 0;
    thr_t extra; init(&extra, 0x2002, 50, 1); disposable->all = &extra;
    n = core_table_snapshot(row, 1, 13000, 1, &truncated);
    need(n == 1 && truncated, "bounded table reports truncation");
    disposable->all = NULL;
    disposable->core_held_ns += 60000000; /* Make the next non-first window report this row. */
    atomic_store(&fake_ns, CORE_TABLE_NS + 20000); pthread_mutex_unlock(&G);
    clear_output(); checking_output = remove_on_output = 1;
    core_table(); checking_output = 0;
    need(!disposable && !g_all && strstr(output, "thread 0xbeef"), "table thread may be freed before copied rows print");
    pthread_cond_destroy(&extra.cv); pthread_cond_destroy(&waiter.cv);
}
int main(int argc, char **argv) {
    need(argc == 2, "pass disabled, tsv, or diag mode");
    diag = !strcmp(argv[1], "diag"); enabled = diag || !strcmp(argv[1], "tsv");
    need(enabled || !strcmp(argv[1], "disabled"), "known diagnostic mode");
    setenv("TOTK_STARVE", "0", 1); setenv("TOTK_ASYNC_PREEMPT", "0", 1);
    g_cores_on = 1; g_svc_profile = 0;
#ifdef GAME_BOTW
    g_async_preempt = 0; g_wake_queue_on = 1;
#else
    g_tk_async = 0; g_ready_cores = 1;
#endif
    need(!pthread_key_create(&g_keythr, NULL), "scheduler TLS key");
    sync_config(); clear_output();
    need(dm_core_diag_enabled() == diag && dm_wait_stats_enabled() == enabled, "launch-time switches");
    setenv("TOTK_CORE_DIAG", diag ? "0" : "1", 1); setenv("DM_NVN_TSV", enabled ? "" : "changed-after-launch", 1);
    need(dm_core_diag_enabled() == diag && dm_wait_stats_enabled() == enabled, "switches remain atomically cached after environment changes");
    if (!enabled) {
        uint64_t reads = atomic_load(&now_reads), lock_count = atomic_load(&locks), alloc_count = atomic_load(&allocations);
        pthread_mutex_lock(&G); stats(NULL, 0, 0); core_table(); pthread_mutex_unlock(&G);
        need(atomic_load(&now_reads) == reads && atomic_load(&locks) == lock_count && atomic_load(&allocations) == alloc_count && !atomic_load(&cpu_reads), "disabled snapshot performs no clocks, locks, allocations or CPU samples");
    }
    acquisition_and_completion();
    if (diag) snapshot_safety();
    else need(!atomic_load(&cpu_reads) && !g_core_reports && !output_size, "TSV-only and disabled modes never collect tables/owner samples");
    need(!atomic_load(&allocations), "all diagnostic modes avoid heap allocation");
    puts("PASS core-wait-stats: production acquisition, mask isolation, spurious wakes, actual-grant completion, wake reservations, direct queued grant, cached switches, disabled overhead, immutable unlocked snapshots");
    return 0;
}
