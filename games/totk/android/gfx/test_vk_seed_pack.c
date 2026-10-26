#define _GNU_SOURCE
#include "vk_seed_pack.h"
#include <stdio.h>
#include <string.h>

static void put32(unsigned char *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static void put64(unsigned char *p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static int write_file(const char *path, const void *bytes, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(bytes, 1, size, f) == size;
    if (fclose(f)) ok = 0;
    return ok;
}
static int expect_rejected(const char *path) {
    vk_seed_pack pack = {0};
    int result = vk_seed_pack_open(&pack, path);
    int ok = result == -2 && !pack.mapping && !pack.size && !pack.count;
    vk_seed_pack_close(&pack);
    return ok;
}
static int compare_loose(const char *directory, const char *name, const void *data, size_t size) {
    char path[4096];
    if (snprintf(path, sizeof path, "%s/%s", directory, name) >= (int)sizeof path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long length = ftell(f);
    if (length < 0 || (uintmax_t)length != size || fseek(f, 0, SEEK_SET)) { fclose(f); return 0; }
    unsigned char buffer[16384];
    size_t at = 0;
    while (at < size) {
        size_t chunk = size - at < sizeof buffer ? size - at : sizeof buffer;
        if (fread(buffer, 1, chunk, f) != chunk || memcmp(buffer, (const unsigned char *)data + at, chunk)) {
            fclose(f); return 0;
        }
        at += chunk;
    }
    int ok = fgetc(f) == EOF && !ferror(f);
    fclose(f);
    return ok;
}
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <seed.vkpack> <loose-seed-directory>\n", argv[0]);
        return 2;
    }
    char scratch[] = "./.vk_seed_pack_test.XXXXXX";
    if (!mkdtemp(scratch)) { perror("mkdtemp"); return 2; }
    char malformed[4096];
    if (snprintf(malformed, sizeof malformed, "%s/malformed.vkpack", scratch) >= (int)sizeof malformed) {
        rmdir(scratch); return 2;
    }
    int errors = 0;
    uint64_t checked_count = 0;
    vk_seed_pack pack = {0};
    int opened = vk_seed_pack_open(&pack, argv[1]);
    if (opened) {
        fprintf(stderr, "cannot open supplied pack: %d\n", opened);
        errors++;
    } else {
        checked_count = pack.count;
        for (uint64_t i = 0; i < pack.count; i++) {
            const char *name;
            const void *data;
            size_t size;
            if (!vk_seed_pack_entry(&pack, i, &name, &data, &size) ||
                !compare_loose(argv[2], name, data, size)) {
                fprintf(stderr, "packed entry does not match loose file at index %llu\n",
                        (unsigned long long)i);
                errors++;
                break;
            }
        }
        vk_seed_pack_close(&pack);
    }

    /* Exercise rejection independently of the supplied pack contents. */
    unsigned char fixture[249] = {0};
    memcpy(fixture, "TOTKVKP1", 8);
    put32(fixture + 8, 1); put32(fixture + 12, 104);
    put64(fixture + 16, 2); put64(fixture + 24, sizeof fixture);
    memcpy(fixture + 32, "a.spv", 6); put64(fixture + 32 + 88, 240); put64(fixture + 32 + 96, 1);
    memcpy(fixture + 136, "b.vk", 5); put64(fixture + 136 + 88, 248); put64(fixture + 136 + 96, 1);
    fixture[240] = 0x11; fixture[248] = 0x22;
    if (!write_file(malformed, fixture, sizeof fixture)) errors++;
    else {
        vk_seed_pack valid_fixture = {0};
        if (vk_seed_pack_open(&valid_fixture, malformed) || valid_fixture.count != 2) errors++;
        vk_seed_pack_close(&valid_fixture);
    }
    if (!write_file(malformed, fixture, sizeof fixture)) errors++;
    else {
        unsigned char saved = fixture[0];
        fixture[0] ^= 0xff;
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
        fixture[0] = saved;
        if (!write_file(malformed, fixture, 31) || !expect_rejected(malformed)) errors++;
        memcpy(fixture + 136, "a.spv", 6);
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
        memcpy(fixture + 136, "b.vk", 5);
        put64(fixture + 32 + 88, 256);
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
        put64(fixture + 32 + 88, 240);
        put32(fixture + 8, 2);
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
        put32(fixture + 8, 1);
        put64(fixture + 24, sizeof fixture + 1);
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
        put64(fixture + 24, sizeof fixture);
        put64(fixture + 16, UINT64_MAX);
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
        put64(fixture + 16, 2);
        put64(fixture + 32 + 96, UINT64_MAX);
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
        put64(fixture + 32 + 96, 9); /* overlaps b.vk */
        if (!write_file(malformed, fixture, sizeof fixture) || !expect_rejected(malformed)) errors++;
    }
    unlink(malformed);
    rmdir(scratch);
    if (errors) {
        fprintf(stderr, "vk_seed_pack: %d failure(s)\n", errors);
        return 1;
    }
    printf("vk_seed_pack: %llu entries match loose shader files; malformed packs rejected\n",
           (unsigned long long)checked_count);
    return 0;
}
