/* Small diagnostic receipts, only on NativeActivity's UI thread and the existing
 * save-safe close poller. Never called by the guest, renderer or save writer.
 * onDestroy is deliberately NOT an orderly-close authority. */
#ifndef TOTK_ANDROID_EXIT_NOTICE_H
#define TOTK_ANDROID_EXIT_NOTICE_H
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static atomic_int android_exit_ready;
static char android_exit_session[1400];
static char android_exit_state[1200];
static inline unsigned long long android_exit_start_ticks(void) {
    char text[4096];
    FILE *in = fopen("/proc/self/stat", "r");
    if (!in) return 0;
    int ok = fgets(text, sizeof text, in) != NULL;
    fclose(in);
    char *p = ok ? strrchr(text, ')') : NULL;
    if (!p) return 0;
    p += 2;
    for (int field = 3; field < 22; field++) {
        p = strchr(p, ' ');
        if (!p) return 0;
        p++;
    }
    return strtoull(p, NULL, 10);
}
static inline long long android_exit_wall_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now)) return 0;
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static inline int android_exit_atomic(const char *path, const char *text) {
    char temporary[1450];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) return 0;
    FILE *out = fopen(temporary, "w");
    if (!out) return 0;
    int ok = fputs(text, out) >= 0;
    if (fflush(out) || fsync(fileno(out))) ok = 0;
    if (fclose(out)) ok = 0;
    if (ok && rename(temporary, path)) ok = 0;
    if (!ok) unlink(temporary);
    return ok;
}
/* History is finite too. Bound receipts to the newest 16 starts, never overwrite
 * the preceding unchecked receipt merely because direct-game startup runs first. */
static inline void android_exit_prune(const char *directory) {
    for (;;) {
        DIR *dir = opendir(directory);
        if (!dir) return;
        int count = 0;
        long long oldest = 0;
        char name[128] = "";
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            long long stamp; int pid, n = 0; unsigned long long ticks;
            if (sscanf(entry->d_name, "session-%lld-%d-%llu%n", &stamp, &pid, &ticks, &n) != 3 || entry->d_name[n]) continue;
            count++;
            if (!oldest || stamp < oldest) { oldest = stamp; snprintf(name, sizeof name, "%.127s", entry->d_name); }
        }
        closedir(dir);
        if (count < 16 || !name[0]) return;
        char path[1450];
        snprintf(path, sizeof path, "%s/%s", directory, name); unlink(path);
        snprintf(path, sizeof path, "%s/%s.closed", directory, name); unlink(path);
        snprintf(path, sizeof path, "%s/%s.background", directory, name); unlink(path);
    }
}
static inline void android_exit_notice_start(const char *private_files, const char *log, const char *data) {
    unsigned long long ticks = android_exit_start_ticks();
    long long now = android_exit_wall_ms();
    char directory[1300], text[3600];
    if (!ticks || !now) return;
    snprintf(android_exit_state, sizeof android_exit_state, "%s/state", private_files);
    snprintf(directory, sizeof directory, "%s/exit-notice", android_exit_state);
    if (mkdir(directory, 0700) && errno != EEXIST) return;
    android_exit_prune(directory);
    snprintf(android_exit_session, sizeof android_exit_session, "%s/session-%lld-%d-%llu", directory, now, (int)getpid(), ticks);
    snprintf(text, sizeof text, "1\n%d\n%llu\n%lld\n%s\n%s/totk.env\n", (int)getpid(), ticks, now, log, data);
    if (!android_exit_atomic(android_exit_session, text)) {
        android_exit_session[0] = 0;
        fprintf(stderr, "[TotkExitNotice] session receipt unavailable\n");
    } else atomic_store_explicit(&android_exit_ready, 1, memory_order_release);
}
static inline void android_exit_notice_close(void) {
    if (!atomic_load_explicit(&android_exit_ready, memory_order_acquire)) return;
    char path[1450];
    snprintf(path, sizeof path, "%s.closed", android_exit_session);
    if (!android_exit_atomic(path, "save-safe close completed\n"))
        fprintf(stderr, "[TotkExitNotice] orderly-close receipt unavailable\n");
}
static inline void android_exit_notice_background(void) {
    if (!atomic_load_explicit(&android_exit_ready, memory_order_acquire)) return;
    char path[1450], text[256], generation[128] = "unavailable";
    snprintf(path, sizeof path, "%s/save-generation", android_exit_state);
    FILE *in = fopen(path, "r");
    if (in) { if (!fgets(generation, sizeof generation, in)) strcpy(generation, "unavailable"); fclose(in); }
    generation[strcspn(generation, "\r\n")] = 0;
    snprintf(path, sizeof path, "%s.background", android_exit_session);
    snprintf(text, sizeof text, "timestamp_ms=%lld last_observed_commit=%s\n", android_exit_wall_ms(), generation);
    android_exit_atomic(path, text); /* Observed Commit generation is not a new checkpoint. */
}
#endif
