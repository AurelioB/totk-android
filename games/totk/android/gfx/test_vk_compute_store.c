/* Synthetic exact-key history: round trip, compatibility, full-file corruption. */
#include "vk_compute_store.h"
#include <fcntl.h>
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "compute store:%d: %s\n", __LINE__, #x); abort(); } } while (0)
int main(void) {
    char path[] = "./.compute-store.XXXXXX"; int fd = mkstemp(path); CHECK(fd >= 0); CHECK(!close(fd));
    compute_header identity = {.vendor = 42, .device = 57, .uuid = {1, 2, 3}};
    char keys[2][COMPUTE_KEY_BYTES] = {{0}};
    memset(keys[0], 'a', 64); memset(keys[1], 'b', 64); keys[1][64] = '-'; memset(keys[1] + 65, 'c', 12);
    char (*loaded)[COMPUTE_KEY_BYTES] = NULL;
    CHECK(compute_key_valid(keys[0]) && compute_key_valid(keys[1]));
    CHECK(compute_history_save(path, identity, keys, 2));
    CHECK(compute_history_load(path, identity, &loaded) == 2);
    CHECK(!memcmp(keys, loaded, sizeof keys)); free(loaded);
    compute_header other = identity; other.uuid[0]++;
    CHECK(compute_history_load(path, other, &loaded) == -1 && !loaded);
    other = identity; other.vendor++;
    CHECK(compute_history_load(path, other, &loaded) == -1 && !loaded);
    other = identity; other.device++;
    CHECK(compute_history_load(path, other, &loaded) == -1 && !loaded);
    FILE *f = fopen(path, "rb"); CHECK(f); unsigned char bytes[sizeof(compute_header) + sizeof keys];
    CHECK(fread(bytes, 1, sizeof bytes, f) == sizeof bytes); CHECK(!fclose(f));
    for (unsigned i = 0; i < sizeof bytes; i++) {
        unsigned char saved = bytes[i]; bytes[i] ^= 1;
        f = fopen(path, "wb"); CHECK(f); CHECK(fwrite(bytes, 1, sizeof bytes, f) == sizeof bytes); CHECK(!fclose(f));
        CHECK(compute_history_load(path, identity, &loaded) == -1 && !loaded); bytes[i] = saved;
    }
    for (unsigned n = 0; n < sizeof bytes; n++) {
        f = fopen(path, "wb"); CHECK(f); CHECK(fwrite(bytes, 1, n, f) == n); CHECK(!fclose(f));
        CHECK(compute_history_load(path, identity, &loaded) == -1 && !loaded);
    }
    CHECK(compute_history_save(path, identity, keys, 2));
    f = fopen(path, "ab"); CHECK(f); CHECK(fputc('!', f) != EOF); CHECK(!fclose(f));
    CHECK(compute_history_load(path, identity, &loaded) == -1 && !loaded);
    keys[0][1] = '/'; CHECK(compute_history_save(path, identity, keys, 2));
    CHECK(compute_history_load(path, identity, &loaded) == -1 && !loaded);
    CHECK(compute_history_save(path, identity, NULL, 0));
    CHECK(compute_history_load(path, identity, &loaded) == 0 && !loaded);
    CHECK(!compute_history_save(path, identity, NULL, COMPUTE_KEYS_MAX + 1));
    CHECK(!unlink(path)); CHECK(compute_history_load(path, identity, &loaded) == 0);
    printf("compute store: %u checks passed (round trip, device/driver, every byte corruption/truncation, trailing/name/limit)\n", checks);
    return 0;
}
