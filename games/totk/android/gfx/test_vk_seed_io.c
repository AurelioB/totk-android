/* Synthetic pack/loose fallback, bounded MRU warming, storage-free consumption and lifetime races. */
#include "vk_seed_io.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static _Atomic unsigned checks;
#define CHECK(x) do { atomic_fetch_add(&checks, 1); if (!(x)) { fprintf(stderr, "seed IO check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static void put32(unsigned char *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void put64(unsigned char *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = v >> (8 * i); }
static void save(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb"); CHECK(f); CHECK(fwrite(data, 1, size, f) == size); CHECK(!fclose(f));
}
static void text(const char *path, const char *data) { save(path, data, strlen(data)); }
static void fixture(const char *path, unsigned count, size_t length) {
    size_t offset = (32 + (size_t)count * 104 + 7) & ~(size_t)7;
    size_t stride = (length + 7) & ~(size_t)7;
    size_t size = offset + (count ? (count - 1) * stride + length : 0);
    unsigned char *bytes = calloc(1, size); CHECK(bytes);
    memcpy(bytes, "TOTKVKP1", 8); put32(bytes + 8, 1); put32(bytes + 12, 104);
    put64(bytes + 16, count); put64(bytes + 24, size);
    for (unsigned i = 0; i < count; i++) {
        unsigned char *record = bytes + 32 + (size_t)i * 104;
        snprintf((char *)record, 84, "key%05u.spv", i);
        put64(record + 88, offset + i * stride); put64(record + 96, length);
        memset(bytes + offset + i * stride, 'A' + i % 26, length);
    }
    save(path, bytes, size); free(bytes);
}
static void expect(const char *name, size_t length, int first) {
    size_t n = 0; char *data = vk_seed_io_read(name, &n);
    CHECK(data && n == length); if (length) { CHECK(data[0] == first); CHECK(data[n - 1] == first); }
    data[0] = '!'; free(data);
}
static void wait_warm(void) {
    vk_seed_io_stats s;
    for (unsigned i = 0; i < 10000; i++) {
        vk_seed_io_stats_get(&s);
        if (s.warm_ready + s.warm_failed + s.warm_skipped + s.warm_bypassed == s.warm_keys) return;
        usleep(1000);
    }
    CHECK(0);
}
static void fallback_cases(void) {
    CHECK(!mkdir("fallback", 0700)); text("fallback/probe.spv", "lllll");
    unsigned char corrupt[136] = {0};
    for (unsigned mode = 0; mode < 6; mode++) {
        unlink("fallback/seed.vkpack");
        memset(corrupt, 0, sizeof corrupt); memcpy(corrupt, "TOTKVKP1", 8);
        put32(corrupt + 8, 1); put32(corrupt + 12, 104); put64(corrupt + 24, sizeof corrupt);
        if (mode == 1) corrupt[0] = '!';
        if (mode == 2) put32(corrupt + 8, 99);
        if (mode == 4) put64(corrupt + 16, UINT64_MAX);
        if (mode == 5) {
            put64(corrupt + 16, 1); memcpy(corrupt + 32, "probe.spv", 10);
            put64(corrupt + 32 + 88, 136); put64(corrupt + 32 + 96, 100);
        }
        if (mode) save("fallback/seed.vkpack", corrupt, mode == 3 ? 31 : sizeof corrupt);
        vk_seed_io_start("fallback", NULL); CHECK(vk_seed_io_ready()); expect("probe.spv", 5, 'l');
        vk_seed_io_stats s; vk_seed_io_stats_get(&s); CHECK(s.pack_entries == 0); vk_seed_io_stop();
    }
    unlink("fallback/probe.spv");
    vk_seed_io_start("fallback", NULL); CHECK(!vk_seed_io_ready());
    CHECK(vk_seed_io_read("probe.spv", NULL) == NULL); vk_seed_io_stop();
    unlink("fallback/seed.vkpack"); CHECK(!rmdir("fallback"));
}
static void warm_and_replace(void) {
    CHECK(!mkdir("packed", 0700)); fixture("packed/seed.vkpack", 3, 600u << 10);
    text("learned.txt", "TOTKSEED1\nkey00001.spv\nkey00000.spv\nkey00002.spv\n");
    vk_seed_io_start("packed", "learned.txt"); CHECK(vk_seed_io_ready());
    wait_warm();
    vk_seed_io_stats a, b; vk_seed_io_stats_get(&a);
    CHECK(a.warm_keys == 3 && a.warm_ready == 1 && a.warm_skipped == 2);
    CHECK(a.warm_bytes == (600u << 10) + 1 && a.cached_bytes == (600u << 10));
    /* Truncate the pinned inode after warm-up: a warmed lookup must still work without any storage.
       A cold pread must fail safely (not SIGBUS), and atomic replacement must not invalidate the old fd. */
    int fd = open("packed/seed.vkpack", O_WRONLY | O_TRUNC); CHECK(fd >= 0); CHECK(!close(fd));
    expect("key00001.spv", 600u << 10, 'B');
    vk_seed_io_stats_get(&b); CHECK(b.warmed_hits == a.warmed_hits + 1);
    CHECK(b.demand_reads == a.demand_reads && b.warmed_storage_reads == 0 && b.cached_bytes == 0);
    CHECK(vk_seed_io_read("key00000.spv", NULL) == NULL);
    fixture("packed/replacement", 3, 8); CHECK(!rename("packed/replacement", "packed/seed.vkpack"));
    CHECK(vk_seed_io_read("key00002.spv", NULL) == NULL); /* still the pinned truncated old inode */
    vk_seed_io_report(); vk_seed_io_stop();
    vk_seed_io_start("packed", "learned.txt"); CHECK(vk_seed_io_ready());
    expect("key00000.spv", 8, 'A'); expect("key00001.spv", 8, 'B');
    vk_seed_io_stop();
    FILE *f = fopen("learned.txt", "rb"); CHECK(f); char line[100];
    CHECK(fgets(line, sizeof line, f) && !strcmp(line, "TOTKSEED1\n"));
    CHECK(fgets(line, sizeof line, f) && !strcmp(line, "key00001.spv\n")); CHECK(!fclose(f));
    unlink("packed/seed.vkpack"); CHECK(!rmdir("packed")); unlink("learned.txt");
}
typedef struct { unsigned first, count; } reader_args;
static void *reader(void *arg) {
    reader_args *r = arg;
    for (unsigned i = r->first; i < r->first + r->count; i++) {
        char name[84]; snprintf(name, sizeof name, "key%05u.spv", i % 96);
        size_t n = 0; char *data = vk_seed_io_read(name, &n);
        if (data) { CHECK(n == (512u << 10)); CHECK(data[0] == 'A' + (i % 96) % 26); free(data); }
    }
    return NULL;
}
static void concurrency(int shutdown) {
    CHECK(!mkdir("concurrent", 0700)); fixture("concurrent/seed.vkpack", 96, 512u << 10);
    FILE *f = fopen("concurrent-used.txt", "wb"); CHECK(f); CHECK(fputs("TOTKSEED1\n", f) >= 0);
    for (unsigned i = 0; i < 96; i++) CHECK(fprintf(f, "key%05u.spv\n", i) > 0); CHECK(!fclose(f));
    setenv("TOTK_SEED_WARM_MB", "64", 1);
    vk_seed_io_start("concurrent", "concurrent-used.txt"); CHECK(vk_seed_io_ready());
    pthread_t threads[6]; reader_args args[6];
    for (unsigned i = 0; i < 6; i++) { args[i] = (reader_args){i * 16, shutdown ? 512 : 16}; CHECK(!pthread_create(&threads[i], NULL, reader, &args[i])); }
    if (shutdown) vk_seed_io_stop();
    else {
        /* The loader/lookups must continue to read 512-KiB old-inode entries, even
           when the pathname is replaced with incompatible-size payloads mid-warm. */
        fixture("concurrent/replacement", 96, 8);
        CHECK(!rename("concurrent/replacement", "concurrent/seed.vkpack"));
    }
    for (unsigned i = 0; i < 6; i++) CHECK(!pthread_join(threads[i], NULL));
    if (!shutdown) {
        CHECK(vk_seed_io_ready()); vk_seed_io_stats s; vk_seed_io_stats_get(&s);
        CHECK(s.warmed_hits + s.warm_joins + s.warm_bypassed == 96 && s.demand_reads == s.warm_bypassed && s.warmed_storage_reads == 0);
        CHECK(s.cached_bytes == 0); vk_seed_io_stop();
    }
    unlink("concurrent/seed.vkpack"); CHECK(!rmdir("concurrent")); unlink("concurrent-used.txt");
}
static void bounded_history(void) {
    CHECK(!mkdir("history", 0700)); fixture("history/seed.vkpack", 16385, 0);
    vk_seed_io_start("history", "history-used.txt"); CHECK(vk_seed_io_ready());
    for (unsigned i = 0; i < 16385; i++) { char name[84]; snprintf(name, sizeof name, "key%05u.spv", i); expect(name, 0, 0); }
    vk_seed_io_stop();
    FILE *f = fopen("history-used.txt", "rb"); CHECK(f); char line[100]; unsigned rows = 0;
    CHECK(fgets(line, sizeof line, f) && !strcmp(line, "TOTKSEED1\n"));
    CHECK(fgets(line, sizeof line, f) && !strcmp(line, "key16384.spv\n")); rows++;
    while (fgets(line, sizeof line, f)) rows++;
    CHECK(rows == 16384); CHECK(!fclose(f));
    unlink("history-used.txt"); unlink("history/seed.vkpack"); CHECK(!rmdir("history"));
}
static void publication(void) {
    CHECK(!mkdir("published", 0700)); text("published/probe.spv", "p");
    char variant[65], path[200]; memset(variant, 'a', 64); variant[64] = 0;
    snprintf(path, sizeof path, "published/%s.vv", variant); text(path, "old");
    text("published-used.txt", "TOTKSEED1\naaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.vv\n");
    vk_seed_io_start("published", "published-used.txt"); CHECK(vk_seed_io_ready());
    wait_warm();
    text(path, "nnn"); vk_seed_io_published(variant);
    char name[84]; snprintf(name, sizeof name, "%s.vv", variant); expect(name, 3, 'n');
    CHECK(vk_seed_io_request("requests", variant, strdup("{}"), 2));
    vk_seed_io_stop();
    snprintf(path, sizeof path, "requests/%s.json", variant); FILE *f = fopen(path, "rb"); CHECK(f); CHECK(fgetc(f) == '{'); CHECK(!fclose(f)); unlink(path); CHECK(!rmdir("requests"));
    snprintf(path, sizeof path, "published/%s.vv", variant); unlink(path);
    unlink("published/probe.spv"); CHECK(!rmdir("published")); unlink("published-used.txt");
}
static pthread_mutex_t hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cv = PTHREAD_COND_INITIALIZER;
static int hold_read, read_entered;
void vk_seed_io_test_before_read(const char *name) {
    pthread_mutex_lock(&hold_mu);
    if (hold_read && !strcmp(name, "key00000.spv")) {
        read_entered = 1; pthread_cond_broadcast(&hold_cv);
        while (hold_read) pthread_cond_wait(&hold_cv, &hold_mu);
    }
    pthread_mutex_unlock(&hold_mu);
}
extern unsigned vk_seed_io_test_waiters(const char *name);
static void *warm_reader(void *unused) { (void)unused; expect("key00000.spv", 8, 'A'); return NULL; }
static void demand_bypasses_warm(void) {
    CHECK(!mkdir("bypass", 0700)); fixture("bypass/seed.vkpack", 3, 8);
    text("bypass-used.txt", "TOTKSEED1\nkey00000.spv\nkey00001.spv\n");
    pthread_mutex_lock(&hold_mu); hold_read = 1; read_entered = 0; pthread_mutex_unlock(&hold_mu);
    vk_seed_io_start("bypass", "bypass-used.txt"); CHECK(vk_seed_io_ready());
    pthread_mutex_lock(&hold_mu);
    while (!read_entered) pthread_cond_wait(&hold_cv, &hold_mu);
    pthread_mutex_unlock(&hold_mu);
    /* The only worker is held on key0. Both a queued learned key and an unseen
       key must complete inline, without releasing it or reading either twice. */
    expect("key00001.spv", 8, 'B'); expect("key00002.spv", 8, 'C');
    vk_seed_io_stats s; vk_seed_io_stats_get(&s);
    CHECK(s.demand_reads == 2 && s.warm_bypassed == 1 && !s.worker_reads);
    pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) CHECK(!pthread_create(&threads[i], NULL, warm_reader, NULL));
    unsigned i;
    for (i = 0; i < 10000 && vk_seed_io_test_waiters("key00000.spv") != 2; i++) usleep(100);
    CHECK(i != 10000); /* already-reading learned work is joined, never duplicated */
    pthread_mutex_lock(&hold_mu); hold_read = 0; pthread_cond_broadcast(&hold_cv); pthread_mutex_unlock(&hold_mu);
    for (i = 0; i < 2; i++) CHECK(!pthread_join(threads[i], NULL));
    wait_warm(); vk_seed_io_stats_get(&s);
    CHECK(s.worker_reads == 1 && s.warm_joins == 2 && s.demand_reads == 2 && !s.cached_bytes);
    vk_seed_io_stop();
    CHECK(!unlink("bypass/seed.vkpack")); CHECK(!unlink("bypass-used.txt")); CHECK(!rmdir("bypass"));
}
static void adaptive_ceiling(void) {
    CHECK(!mkdir("ceiling", 0700));
    int fd = open("ceiling/large.spv", O_CREAT | O_WRONLY, 0600); CHECK(fd >= 0);
    CHECK(!ftruncate(fd, 300u << 20)); CHECK(!close(fd)); /* sparse metadata only; no large allocation/read */
    text("ceiling-used.txt", "TOTKSEED1\nlarge.spv\n");
    unsetenv("TOTK_SEED_WARM_MB");
    vk_seed_io_start("ceiling", NULL); CHECK(vk_seed_io_ready()); wait_warm();
    vk_seed_io_stats floor; vk_seed_io_stats_get(&floor);
    CHECK(floor.warm_cap == (64u << 20) && !floor.learned_bytes && !floor.worker_reads);
    vk_seed_io_stop();
    vk_seed_io_start("ceiling", "ceiling-used.txt"); CHECK(vk_seed_io_ready()); wait_warm();
    vk_seed_io_stats s; vk_seed_io_stats_get(&s);
    CHECK(s.learned_bytes == (300u << 20) + 1 && s.warm_cap == (256u << 20));
    CHECK(s.warm_skipped == 1 && !s.cached_bytes && !s.worker_reads);
    vk_seed_io_stop();
    CHECK(!unlink("ceiling/large.spv")); CHECK(!unlink("ceiling-used.txt")); CHECK(!rmdir("ceiling"));
}
static uint64_t now_ns(void) {
    struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void realistic_scale(void) {
    /* 4,729 read attempts in the supplied S2 log; 20,534-byte local seed mean.
       This is synthetic, page-cache-hot host timing, not device bandwidth evidence. */
    CHECK(!mkdir("scale", 0700)); fixture("scale/seed.vkpack", 4729, 20534);
    FILE *f = fopen("scale-used.txt", "wb"); CHECK(f); CHECK(fputs("TOTKSEED1\n", f) >= 0);
    for (unsigned i = 0; i < 4729; i++) CHECK(fprintf(f, "key%05u.spv\n", i) > 0); CHECK(!fclose(f));
    setenv("TOTK_SEED_WARM_MB", "64", 1);
    uint64_t began = now_ns(); vk_seed_io_start("scale", "scale-used.txt"); CHECK(vk_seed_io_ready());
    uint64_t ready_ns = now_ns() - began;
    wait_warm(); vk_seed_io_stats s; vk_seed_io_stats_get(&s);
    CHECK(s.warm_keys == 4729 && s.warm_ready > 0 && s.warm_skipped > 0 && s.warm_failed == 0);
    CHECK(s.warm_bytes <= (64u << 20));
    printf("seed scale: 4729 keys, %llu input bytes; ready=%.3fms index=%.3fms async=%.3fms warm_bytes=%llu skipped=%llu (synthetic page-cache-hot host)\n",
        4729ull * 20534, ready_ns / 1e6, s.index_ns / 1e6, s.warm_ns / 1e6,
        (unsigned long long)s.warm_bytes, (unsigned long long)s.warm_skipped);
    vk_seed_io_stop();
    unsetenv("TOTK_SEED_WARM_MB");
    vk_seed_io_start("scale", "scale-used.txt"); CHECK(vk_seed_io_ready()); wait_warm();
    vk_seed_io_stats_get(&s);
    CHECK(s.learned_bytes == 4729ull * 20535 && s.warm_cap == (116u << 20));
    CHECK(s.warm_ready == 4729 && !s.warm_skipped && s.warm_bytes == s.learned_bytes);
    printf("seed adaptive: learned_bytes=%llu cap=%llu cached=%llu skipped=%llu\n",
        (unsigned long long)s.learned_bytes, (unsigned long long)s.warm_cap,
        (unsigned long long)s.cached_bytes, (unsigned long long)s.warm_skipped);
    vk_seed_io_stop(); unlink("scale-used.txt"); unlink("scale/seed.vkpack"); CHECK(!rmdir("scale"));
}
int main(void) {
    unsetenv("TOTK_VK_SEED_IO"); CHECK(vk_seed_io_enabled());
    setenv("TOTK_SEED_STATS", "1", 1); setenv("TOTK_SEED_WARM_MB", "1", 1); setenv("TOTK_XLATE_ONDEVICE", "0", 1);
    char scratch[] = "./.vk_seed_io_test.XXXXXX"; CHECK(mkdtemp(scratch));
    int cwd = open(".", O_RDONLY | O_DIRECTORY); CHECK(cwd >= 0); CHECK(!chdir(scratch));
    fallback_cases(); warm_and_replace(); concurrency(0); concurrency(1); bounded_history(); publication(); demand_bypasses_warm(); realistic_scale(); adaptive_ceiling();
    CHECK(!fchdir(cwd)); CHECK(!close(cwd)); CHECK(!rmdir(scratch));
    printf("seed IO: %u checks passed (fallback x6, MRU/override/adaptive cap, zero-storage warm, replacement, lookup/warm/shutdown races, bounded history, publication, immediate queued/cold demand, in-flight joins)\n", atomic_load(&checks));
    return 0;
}
