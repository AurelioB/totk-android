/* Small helpers shared by the rewriter, the tables writer and the code stage: SHA-256, whole-file reads, growable text. */
#include "rewrite.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void block(RwSha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ w[i-15] >> 3, s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ w[i-2] >> 10;
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
void rw_sha256_init(RwSha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, sizeof iv); s->len = 0; s->fill = 0;
}
void rw_sha256_update(RwSha256 *s, const void *data, size_t n) {
    const uint8_t *p = data; s->len += n;
    if (s->fill) {
        size_t k = 64 - s->fill < n ? 64 - s->fill : n; memcpy(s->buf + s->fill, p, k); s->fill += k; p += k; n -= k;
        if (s->fill < 64) return;
        block(s, s->buf); s->fill = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(s, p);
    memcpy(s->buf, p, n); s->fill = n;
}
void rw_sha256_hex(RwSha256 *s, char out[65]) {
    uint64_t bits = s->len * 8; uint8_t pad[72] = {0x80}; size_t k = (s->fill < 56 ? 56 : 120) - s->fill;
    for (int i = 0; i < 8; i++) pad[k + i] = (uint8_t)(bits >> (56 - 8 * i));
    rw_sha256_update(s, pad, k + 8);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", (unsigned)(s->h[i / 4] >> (24 - 8 * (i % 4)) & 0xff));
}

int rw_read_file(const char *path, uint8_t **data, size_t *size, char *err, size_t errlen) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) { snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno)); if (fd >= 0) close(fd); return RW_FAIL; }
    uint8_t *p = malloc(st.st_size ? (size_t)st.st_size : 1); size_t got = 0;
    if (!p) { close(fd); snprintf(err, errlen, "out of memory reading %s", path); return RW_FAIL; }
    while (got < (size_t)st.st_size) {
        ssize_t r = pread(fd, p + got, (size_t)st.st_size - got, (off_t)got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { snprintf(err, errlen, "cannot read %s: %s", path, r ? strerror(errno) : "short file"); free(p); close(fd); return RW_FAIL; }
        got += (size_t)r;
    }
    close(fd); *data = p; *size = got; return RW_OK;
}

int rw_buf_printf(RwBuf *b, const char *fmt, ...) {
    for (;;) {
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (n < 0) return RW_FAIL;
        if (b->p && b->n + (size_t)n < b->cap) { b->n += (size_t)n; return RW_OK; }
        size_t cap = b->cap ? b->cap : 4096;
        while (cap <= b->n + (size_t)n) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) return RW_FAIL;
        b->p = p; b->cap = cap;
    }
}
