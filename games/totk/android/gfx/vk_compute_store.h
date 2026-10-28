/* Exact compute variant identities, never Vulkan handles or game payloads. */
#ifndef TOTK_VK_COMPUTE_STORE_H
#define TOTK_VK_COMPUTE_STORE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define COMPUTE_KEYS_MAX 4096u
#define COMPUTE_KEY_BYTES 80u
typedef struct {
    uint32_t magic, version, count, vendor, device, reserved;
    unsigned char uuid[16];
    uint64_t checksum;
} compute_header;
static int compute_key_valid(const char key[COMPUTE_KEY_BYTES]) {
    size_t n = strnlen(key, COMPUTE_KEY_BYTES);
    if (n != 64 && n != 77) return 0;
    for (size_t i = 0; i < n; i++)
        if (i == 64 ? key[i] != '-' : !((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f'))) return 0;
    for (size_t i = n; i < COMPUTE_KEY_BYTES; i++) if (key[i]) return 0;
    return 1;
}
static uint64_t compute_checksum(const void *bytes, size_t n) {
    const unsigned char *p = bytes; uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h; /* accidental corruption detection, not authentication */
}
static int compute_history_load(const char *path, compute_header identity, char (**out)[COMPUTE_KEY_BYTES]) {
    *out = NULL;
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    compute_header h = {0};
    int ok = fread(&h, sizeof h, 1, f) == 1;
    identity.magic = 0x50434b56; identity.version = 1; identity.count = h.count; identity.checksum = h.checksum;
    ok = ok && !memcmp(&h, &identity, sizeof h) && h.count <= COMPUTE_KEYS_MAX;
    char (*keys)[COMPUTE_KEY_BYTES] = ok && h.count ? calloc(h.count, COMPUTE_KEY_BYTES) : NULL;
    if (ok) ok = (!h.count || keys) && fread(keys, COMPUTE_KEY_BYTES, h.count, f) == h.count && fgetc(f) == EOF;
    if (ok) ok = h.checksum == compute_checksum(keys, (size_t)h.count * COMPUTE_KEY_BYTES);
    for (uint32_t i = 0; ok && i < h.count; i++) ok = compute_key_valid(keys[i]);
    fclose(f);
    if (!ok) { free(keys); return -1; }
    *out = keys; return (int)h.count;
}
static int compute_history_save(const char *path, compute_header h, const char (*keys)[COMPUTE_KEY_BYTES], uint32_t count) {
    if (count > COMPUTE_KEYS_MAX) return 0;
    char temporary[1200];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) >= (int)sizeof temporary) return 0;
    h.magic = 0x50434b56; h.version = 1; h.count = count;
    h.checksum = compute_checksum(keys, (size_t)count * COMPUTE_KEY_BYTES);
    FILE *f = fopen(temporary, "wb"); int ok = f != NULL;
    if (f) {
        ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(keys, COMPUTE_KEY_BYTES, count, f) == count;
        if (fflush(f) || fsync(fileno(f))) ok = 0;
        if (fclose(f)) ok = 0;
    }
    if (ok && !rename(temporary, path)) return 1;
    unlink(temporary); return 0;
}
#endif
