/* A plain-text launch history. Direct fd 1/2 writes include fatal signals and
   survive process termination. Compact before redirecting them for the next run. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TOTK_LOG_LAUNCH "[totk-log] ===== Launch "
#define TOTK_LOG_RUNS 10
#define TOTK_LOG_RETAIN_BYTES (512 * 1024)
#define TOTK_LOG_HEAD_BYTES (64 * 1024)

#ifndef F_OFD_SETLK
#define F_OFD_SETLK 37   /* Linux 3.15+; also used by the Android game lock */
#endif

static inline int log_history_write(int fd, const void *data, size_t size) {
    const char *bytes = data;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        bytes += n;
        size -= (size_t)n;
    }
    return 0;
}

static inline ssize_t log_history_read(int fd, void *data, size_t size, off_t at) {
    ssize_t n;
    do { n = pread(fd, data, size, at); } while (n < 0 && errno == EINTR);
    if (!n) errno = EIO;
    return n;
}

static inline int log_history_copy(int from, int to, off_t first, off_t last) {
    char buffer[16384];
    while (first < last) {
        size_t wanted = last - first < (off_t)sizeof buffer ? (size_t)(last - first) : sizeof buffer;
        ssize_t n = log_history_read(from, buffer, wanted, first);
        if (n <= 0 || log_history_write(to, buffer, (size_t)n)) return -1;
        first += n;
    }
    return 0;
}

/* One implicit session at offset zero covers a log saved by an older APK. */
static inline int log_history_scan(int fd, off_t size, off_t starts[TOTK_LOG_RUNS]) {
    starts[0] = 0;
    int used = 1, matched = 0;
    char buffer[16384];
    for (off_t offset = 0; offset < size;) {
        size_t wanted = size - offset < (off_t)sizeof buffer ? (size_t)(size - offset) : sizeof buffer;
        ssize_t n = log_history_read(fd, buffer, wanted, offset);
        if (n <= 0) return -1;
        for (ssize_t i = 0; i < n;) {
            if (matched < 0) {
                char *newline = memchr(buffer + i, '\n', (size_t)(n - i));
                if (!newline) break;
                i = newline - buffer + 1;
                matched = 0;
                continue;
            }
            char c = buffer[i++];
            if (c != TOTK_LOG_LAUNCH[matched]) { matched = c == '\n' ? 0 : -1; continue; }
            if (++matched != sizeof(TOTK_LOG_LAUNCH) - 1) continue;
            off_t start = offset + i - matched;
            matched = -1;
            if (!start) continue;
            if (used == TOTK_LOG_RUNS) {
                memmove(starts, starts + 1, (TOTK_LOG_RUNS - 1) * sizeof *starts);
                --used;
            }
            starts[used++] = start;
        }
        offset += n;
    }
    return used;
}

static inline int log_history_newline(int from, int to, off_t end) {
    char last;
    if (!end) return 0;
    if (log_history_read(from, &last, 1, end - 1) != 1) return -1;
    return last == '\n' ? 0 : log_history_write(to, "\n", 1);
}

/* Preserve settings at the top and the crash/exit at the bottom. The cuts
   omit any partial UTF-8 character. No space is reserved for the live log. */
static inline int log_history_retain(int from, int to, off_t first, off_t end, int legacy) {
    static const char old_heading[] = TOTK_LOG_LAUNCH "(before log history) =====\n";
    static const char omitted[] = "\n[totk-log] Middle of this launch omitted to limit size.\n";
    off_t budget = TOTK_LOG_RETAIN_BYTES - 1;   /* reserve a terminating newline */
    if (legacy) {
        if (log_history_write(to, old_heading, sizeof old_heading - 1)) return -1;
        budget -= sizeof old_heading - 1;
    }
    if (end - first <= budget) {
        if (log_history_copy(from, to, first, end)) return -1;
    } else {
        off_t head = first + TOTK_LOG_HEAD_BYTES;
        off_t tail = end - (budget - TOTK_LOG_HEAD_BYTES - (sizeof omitted - 1));
        unsigned char byte;
        for (int i = 0; i < 3 && head > first; ++i) {
            if (log_history_read(from, &byte, 1, head) != 1) return -1;
            if ((byte & 0xc0) != 0x80) break;
            --head;
        }
        for (int i = 0; i < 3 && tail < end; ++i) {
            if (log_history_read(from, &byte, 1, tail) != 1) return -1;
            if ((byte & 0xc0) != 0x80) break;
            ++tail;
        }
        if (log_history_copy(from, to, first, head) ||
            log_history_write(to, omitted, sizeof omitted - 1) ||
            log_history_copy(from, to, tail, end)) return -1;
    }
    return log_history_newline(from, to, end);
}

/* Returns a replacement fd or -1. The old log remains available on every
   failure before rename. A fixed temporary name is reclaimed on the next run. */
static inline int log_history_prepare(const char *path, int fd, off_t size) {
    if (!size) return fd;
    off_t starts[TOTK_LOG_RUNS];
    int n = log_history_scan(fd, size, starts);
    if (n < 0) return -1;
    int first = n == TOTK_LOG_RUNS ? 1 : 0;
    char magic[sizeof TOTK_LOG_LAUNCH - 1];
    ssize_t got = log_history_read(fd, magic, sizeof magic, 0);
    if (got < 0) return -1;
    int legacy = got != sizeof magic || memcmp(magic, TOTK_LOG_LAUNCH, sizeof magic);
    int rewrite = first || starts[first] || legacy;
    for (int i = first; i < n; ++i) {
        off_t end = i + 1 == n ? size : starts[i + 1];
        if (end - starts[i] >= TOTK_LOG_RETAIN_BYTES) rewrite = 1;
    }
    if (!rewrite) return fd;

    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    if (unlink(tmp) && errno != ENOENT) return -1;
    int out = open(tmp, O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) return -1;
    for (int i = first; i < n; ++i) {
        off_t end = i + 1 == n ? size : starts[i + 1];
        if (log_history_retain(fd, out, starts[i], end, legacy && !starts[i])) goto failed;
    }
    if (fsync(out) || rename(tmp, path)) goto failed;
    return out;

failed: {
        int error = errno;
        close(out);
        unlink(tmp);
        errno = error;
        return -1;
    }
}

/* Hold the lock for as long as the raw descriptors can write. The stable
   sidecar also serializes two installations pointed at the same game folder. */
static inline int log_history_open(const char *path, int *lock_fd, off_t *start, int *trim_error) {
    char lock_path[1200];
    if (snprintf(lock_path, sizeof lock_path, "%s.lock", path) >= (int)sizeof lock_path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (lock < 0) return -1;
    int fd = -1;
    struct flock exclusive = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fcntl(lock, F_OFD_SETLK, &exclusive)) goto failed;
    fd = open(path, O_RDWR | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0644);
    if (fd < 0) goto failed;
    struct stat st;
    if (fstat(fd, &st)) goto failed;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto failed; }
    int compact = log_history_prepare(path, fd, st.st_size);
    if (compact < 0) *trim_error = errno;
    else if (compact != fd) { close(fd); fd = compact; }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || log_history_newline(fd, fd, size)) goto failed;
    *start = lseek(fd, 0, SEEK_END);
    if (*start < 0) goto failed;
    char date[32] = "time unknown", heading[128];
    time_t now = time(NULL);
    struct tm utc;
    if (gmtime_r(&now, &utc)) strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &utc);
    int n = snprintf(heading, sizeof heading, TOTK_LOG_LAUNCH "%s (pid %ld) =====\n", date, (long)getpid());
    if (n < 0 || n >= (int)sizeof heading) { errno = EIO; goto failed; }
    if (log_history_write(fd, heading, (size_t)n)) goto failed;
    *lock_fd = lock;
    return fd;

failed: {
        int error = errno;
        if (fd >= 0) close(fd);
        close(lock);
        errno = error;
        return -1;
    }
}
