/* BNTX textures for the companion: textures(), deswizzle() and decode_texture() of make_companion_assets.py, with the
   block decoders of texture2ddecoder 1.0.6 (the decoder requirements.txt pins) ported to C so every pixel matches.

   texture2ddecoder: Copyright (c) 2020 K0lb3, MIT licence. Its BC1-5 code derives from AssetStudio's
   Texture2DDecoderNative (Copyright (c) 2016 Radu, (c) 2016-2020 Perfare, MIT), its BC7 code from bimg (Copyright
   2010-2026 Branimir Karadzic, BSD 2-clause), its ASTC code from Ishotihadus/mikunyan (MIT). Licence texts:
   companion/THIRD_PARTY_LICENSES.txt. */
#include "cg.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t color(unsigned r, unsigned g, unsigned b, unsigned a) {   /* little-endian BGRA bytes */
    return (uint8_t)b | (uint32_t)(uint8_t)g << 8 | (uint32_t)(uint8_t)r << 16 | (uint32_t)(uint8_t)a << 24;
}
static void copy_block(long bx, long by, long w, long h, long bw, long bh, const uint32_t *buf, uint32_t *image) {
    long x = bw * bx, xl = (bw * (bx + 1) > w ? w - bw * bx : bw) * 4;
    const uint32_t *end = buf + bw * bh;
    for (long y = by * bh; buf < end && y < h; buf += bw, y++) memcpy(image + y * w + x, buf, (size_t)xl);
}

/* ---- BC1/BC3/BC4/BC5 (bcn.cpp) ---- */
static void rgb565(unsigned d, unsigned *r, unsigned *g, unsigned *b) {
    *r = ((d >> 8 & 0xf8) | (d >> 13)) & 0xff;
    *g = ((d >> 3 & 0xfc) | (d >> 9 & 3)) & 0xff;
    *b = ((d << 3) | (d >> 2 & 7)) & 0xff;
}
static void bc1_block(const uint8_t *data, uint32_t *out) {
    unsigned r0, g0, b0, r1, g1, b1;
    int q0 = le16(data), q1 = le16(data + 2);
    rgb565((unsigned)q0, &r0, &g0, &b0);
    rgb565((unsigned)q1, &r1, &g1, &b1);
    uint32_t c[4] = { color(r0, g0, b0, 255), color(r1, g1, b1, 255), 0, 0 };
    if (q0 > q1) {
        c[2] = color((r0 * 2 + r1) / 3, (g0 * 2 + g1) / 3, (b0 * 2 + b1) / 3, 255);
        c[3] = color((r0 + r1 * 2) / 3, (g0 + g1 * 2) / 3, (b0 + b1 * 2) / 3, 255);
    } else {
        c[2] = color((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        c[3] = color(0, 0, 0, 255);
    }
    uint32_t d = le32(data + 4);
    for (int i = 0; i < 16; i++, d >>= 2) out[i] = c[d & 3];
}
static void bc3_alpha(const uint8_t *data, uint32_t *out, int channel) {
    unsigned a[8] = { data[0], data[1] };
    if (a[0] > a[1]) {
        a[2] = (a[0] * 6 + a[1]) / 7; a[3] = (a[0] * 5 + a[1] * 2) / 7; a[4] = (a[0] * 4 + a[1] * 3) / 7;
        a[5] = (a[0] * 3 + a[1] * 4) / 7; a[6] = (a[0] * 2 + a[1] * 5) / 7; a[7] = (a[0] + a[1] * 6) / 7;
    } else {
        a[2] = (a[0] * 4 + a[1]) / 5; a[3] = (a[0] * 3 + a[1] * 2) / 5; a[4] = (a[0] * 2 + a[1] * 3) / 5;
        a[5] = (a[0] + a[1] * 4) / 5; a[6] = 0; a[7] = 255;
    }
    uint8_t *dst = (uint8_t *)out;
    uint64_t d = le64(data) >> 16;
    for (int i = 0; i < 16; i++, d >>= 3) dst[i * 4 + channel] = (uint8_t)a[d & 7];
}
void cg_bc1(const uint8_t *data, long w, long h, uint32_t *image) {
    uint32_t buf[16];
    for (long by = 0; by < (h + 3) / 4; by++)
        for (long bx = 0; bx < (w + 3) / 4; bx++, data += 8) { bc1_block(data, buf); copy_block(bx, by, w, h, 4, 4, buf, image); }
}
void cg_bc3(const uint8_t *data, long w, long h, uint32_t *image) {
    uint32_t buf[16];
    for (long by = 0; by < (h + 3) / 4; by++)
        for (long bx = 0; bx < (w + 3) / 4; bx++, data += 16) {
            bc1_block(data + 8, buf); bc3_alpha(data, buf, 3); copy_block(bx, by, w, h, 4, 4, buf, image);
        }
}
/* BC4/BC5 keep the rest of the block buffer from its 0xff000000 start: red (and green) from the blocks, alpha 255. */
void cg_bc4(const uint8_t *data, long w, long h, uint32_t *image) {
    uint32_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = 0xff000000u;
    for (long by = 0; by < (h + 3) / 4; by++)
        for (long bx = 0; bx < (w + 3) / 4; bx++, data += 8) { bc3_alpha(data, buf, 2); copy_block(bx, by, w, h, 4, 4, buf, image); }
}
void cg_bc5(const uint8_t *data, long w, long h, uint32_t *image) {
    uint32_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = 0xff000000u;
    for (long by = 0; by < (h + 3) / 4; by++)
        for (long bx = 0; bx < (w + 3) / 4; bx++, data += 16) {
            bc3_alpha(data, buf, 2); bc3_alpha(data + 8, buf, 1); copy_block(bx, by, w, h, 4, 4, buf, image);
        }
}

/* ---- BC7 (bcn.cpp, from bimg) ---- */
typedef struct { const uint8_t *data; unsigned pos; } Bits;
static unsigned bits_word(const uint8_t *d, unsigned pos) {
    uint32_t v = 0; unsigned n = 16 - pos < 4 ? 16 - pos : 4;
    memcpy(&v, d + pos, n);   /* little-endian hosts only (arm64, x86-64) */
    return v;
}
static unsigned bits_read(Bits *b, unsigned n) {
    unsigned pos = b->pos / 8, shift = b->pos & 7;
    uint32_t v = bits_word(b->data, pos);
    b->pos += n;
    return (uint16_t)((v >> shift) & ((1u << n) - 1));
}
static unsigned bits_peek(const Bits *b, unsigned off, unsigned n) {
    unsigned bit = b->pos + off, shift = bit & 7;
    uint32_t v = bits_word(b->data, bit / 8);
    return (uint8_t)((v >> shift) & ((1u << n) - 1));
}
static const uint16_t P2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000,
    0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c,
    0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
    0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22 };
static const uint8_t A2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15 };
static const uint8_t FACTORS[3][16] = {
    { 0, 21, 43, 64 }, { 0, 9, 18, 27, 37, 46, 55, 64 }, { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 } };
static const uint32_t P3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254 };
static const uint8_t A3[2][64] = {
    { 3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
      8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3 },
    { 15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
      15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8 } };
typedef struct { uint8_t subsets, partition_bits, rotation_bits, index_sel_bits, color_bits, alpha_bits, endpoint_p, shared_p, index_bits[2]; } Bc7Mode;
static const Bc7Mode BC7[8] = {
    { 3, 4, 0, 0, 4, 0, 1, 0, { 3, 0 } }, { 2, 6, 0, 0, 6, 0, 0, 1, { 3, 0 } }, { 3, 6, 0, 0, 5, 0, 0, 0, { 2, 0 } },
    { 2, 6, 0, 0, 7, 0, 1, 0, { 2, 0 } }, { 1, 0, 2, 1, 5, 6, 0, 0, { 2, 3 } }, { 1, 0, 2, 0, 7, 8, 0, 0, { 2, 2 } },
    { 1, 0, 0, 0, 7, 7, 1, 0, { 4, 0 } }, { 2, 6, 0, 0, 5, 5, 1, 0, { 2, 0 } } };
static uint8_t expand_q(uint8_t v, int bits) { v = (uint8_t)(v << (8 - bits)); return (uint8_t)(v | (v >> bits)); }
static void bc7_block(const uint8_t *src, uint32_t *dst) {
    Bits bit = { src, 0 };
    unsigned mode = 0;
    while (mode < 8 && !bits_read(&bit, 1)) mode++;
    if (mode == 8) { memset(dst, 0, 64); return; }
    const Bc7Mode *mi = &BC7[mode];
    unsigned pbits = mi->endpoint_p ? mi->endpoint_p : mi->shared_p;
    unsigned part = bits_read(&bit, mi->partition_bits), rot = bits_read(&bit, mi->rotation_bits);
    unsigned isel = bits_read(&bit, mi->index_sel_bits);
    uint8_t ep[4][6];   /* r g b a */
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < mi->subsets; i++) {
            ep[c][i * 2] = (uint8_t)(bits_read(&bit, mi->color_bits) << pbits);
            ep[c][i * 2 + 1] = (uint8_t)(bits_read(&bit, mi->color_bits) << pbits);
        }
    if (mi->alpha_bits)
        for (int i = 0; i < mi->subsets; i++) {
            ep[3][i * 2] = (uint8_t)(bits_read(&bit, mi->alpha_bits) << pbits);
            ep[3][i * 2 + 1] = (uint8_t)(bits_read(&bit, mi->alpha_bits) << pbits);
        }
    else memset(ep[3], 0xff, 6);
    if (pbits)
        for (int i = 0; i < mi->subsets; i++) {
            unsigned pa = bits_read(&bit, pbits), pb = mi->shared_p ? pa : bits_read(&bit, pbits);
            for (int c = 0; c < 4; c++) { ep[c][i * 2] |= (uint8_t)pa; ep[c][i * 2 + 1] |= (uint8_t)pb; }
        }
    int cbits = mi->color_bits + (int)pbits;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < mi->subsets * 2; i++) ep[c][i] = expand_q(ep[c][i], cbits);
    if (mi->alpha_bits)
        for (int i = 0; i < mi->subsets * 2; i++) ep[3][i] = expand_q(ep[3][i], mi->alpha_bits + (int)pbits);
    int has1 = mi->index_bits[1] != 0;
    const uint8_t *factors[2] = { FACTORS[mi->index_bits[0] - 2], has1 ? FACTORS[mi->index_bits[1] - 2] : FACTORS[mi->index_bits[0] - 2] };
    unsigned offset[2] = { 0, (unsigned)(mi->subsets * (16 * mi->index_bits[0] - 1)) };
    for (unsigned idx = 0; idx < 16; idx++) {
        unsigned subset = 0, anchor_idx = 0;
        if (mi->subsets == 2) { subset = (P2[part] >> idx) & 1; anchor_idx = subset ? A2[part] : 0; }
        else if (mi->subsets == 3) { subset = (P3[part] >> (2 * idx)) & 3; anchor_idx = subset ? A3[subset - 1][part] : 0; }
        unsigned anchor = idx == anchor_idx;
        unsigned num[2] = { mi->index_bits[0] - anchor, has1 ? mi->index_bits[1] - anchor : 0 };
        unsigned index[2];
        index[0] = bits_peek(&bit, offset[0], num[0]);
        index[1] = has1 ? bits_peek(&bit, offset[1], num[1]) : index[0];
        offset[0] += num[0]; offset[1] += num[1];
        unsigned fc = factors[isel][index[isel]], fa = factors[!isel][index[!isel]];
        unsigned s = subset * 2;
        uint8_t rr = (uint8_t)((uint16_t)(ep[0][s] * (64 - fc) + ep[0][s + 1] * fc + 32) >> 6);
        uint8_t gg = (uint8_t)((uint16_t)(ep[1][s] * (64 - fc) + ep[1][s + 1] * fc + 32) >> 6);
        uint8_t bb = (uint8_t)((uint16_t)(ep[2][s] * (64 - fc) + ep[2][s + 1] * fc + 32) >> 6);
        uint8_t aa = (uint8_t)((uint16_t)(ep[3][s] * (64 - fa) + ep[3][s + 1] * fa + 32) >> 6), t;
        if (rot == 1) { t = aa; aa = rr; rr = t; }
        else if (rot == 2) { t = aa; aa = gg; gg = t; }
        else if (rot == 3) { t = aa; aa = bb; bb = t; }
        dst[idx] = color(rr, gg, bb, aa);
    }
}
void cg_bc7(const uint8_t *data, long w, long h, uint32_t *image) {
    uint32_t buf[16];
    for (long by = 0; by < (h + 3) / 4; by++)
        for (long bx = 0; bx < (w + 3) / 4; bx++, data += 16) { bc7_block(data, buf); copy_block(bx, by, w, h, 4, 4, buf, image); }
}

/* ---- ASTC (astc.cpp, from mikunyan) ---- */
static const int WPREC_A[16] = { 0, 0, 0, 3, 0, 5, 3, 0, 0, 0, 5, 3, 0, 5, 3, 0 };
static const int WPREC_B[16] = { 0, 0, 1, 0, 2, 0, 1, 3, 0, 0, 1, 2, 4, 2, 3, 5 };
static const int CEM_A[19] = { 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 5, 0, 3, 0, 0 };
static const int CEM_B[19] = { 8, 6, 5, 7, 5, 4, 6, 4, 3, 5, 3, 2, 4, 2, 1, 3, 1, 2, 1 };

static unsigned rev8(unsigned c) { c &= 0xff; c = (c & 0xf0) >> 4 | (c & 0x0f) << 4; c = (c & 0xcc) >> 2 | (c & 0x33) << 2; return (c & 0xaa) >> 1 | (c & 0x55) << 1; }
static uint64_t bit_reverse_u64(uint64_t d, int bits) {   /* all 64 bits reversed, then the low `bits` kept */
#if defined(__has_builtin)
#if __has_builtin(__builtin_bitreverse64)
    return __builtin_bitreverse64(d) >> (64 - bits);
#endif
#endif
    d = (d >> 1 & 0x5555555555555555ull) | (d & 0x5555555555555555ull) << 1;
    d = (d >> 2 & 0x3333333333333333ull) | (d & 0x3333333333333333ull) << 2;
    d = (d >> 4 & 0x0f0f0f0f0f0f0f0full) | (d & 0x0f0f0f0f0f0f0f0full) << 4;
    d = __builtin_bswap64(d);
    return d >> (64 - bits);
}
/* The block is copied into 24 zero-padded bytes: the original reads whole words past bit positions near the end and
   masks them, so only in-block bits ever matter. */
static int getbits(const uint8_t *buf, int bit, int len) {
    uint32_t v = le32(buf + bit / 8);
    return (int)((v >> (bit % 8)) & ((1u << len) - 1));
}
static uint64_t getbits64(const uint8_t *buf, int bit, int len) {
    uint64_t mask = len == 64 ? ~0ull : (1ull << len) - 1, lo = le64(buf), hi = le64(buf + 8);
    if (len < 1) return 0;
    if (bit >= 64) return hi >> (bit - 64) & mask;
    if (bit <= 0) return lo << -bit & mask;
    if (bit + len <= 64) return lo >> bit & mask;
    return (lo >> bit | hi << (64 - bit)) & mask;
}
static int clamp8(int n) { return n < 0 ? 0 : n > 255 ? 255 : n; }
static int clamp_hdr(int n) { return n < 0 ? 0 : n > 0xfff ? 0xfff : n; }
static void bit_transfer_signed(int *a, int *b) {
    *b = (*b >> 1) | (*a & 0x80);
    *a = (*a >> 1) & 0x3f;
    if (*a & 0x20) *a -= 0x40;
}
static void set_ep(int e[8], int r1, int g1, int b1, int a1, int r2, int g2, int b2, int a2) {
    e[0] = r1; e[1] = g1; e[2] = b1; e[3] = a1; e[4] = r2; e[5] = g2; e[6] = b2; e[7] = a2;
}
static void set_ep_clamp(int e[8], int r1, int g1, int b1, int a1, int r2, int g2, int b2, int a2) {
    set_ep(e, clamp8(r1), clamp8(g1), clamp8(b1), clamp8(a1), clamp8(r2), clamp8(g2), clamp8(b2), clamp8(a2));
}
static void set_ep_blue(int e[8], int r1, int g1, int b1, int a1, int r2, int g2, int b2, int a2) {
    set_ep(e, (r1 + b1) >> 1, (g1 + b1) >> 1, b1, a1, (r2 + b2) >> 1, (g2 + b2) >> 1, b2, a2);
}
static void set_ep_blue_clamp(int e[8], int r1, int g1, int b1, int a1, int r2, int g2, int b2, int a2) {
    set_ep(e, clamp8((r1 + b1) >> 1), clamp8((g1 + b1) >> 1), clamp8(b1), clamp8(a1), clamp8((r2 + b2) >> 1),
           clamp8((g2 + b2) >> 1), clamp8(b2), clamp8(a2));
}
static void set_ep_hdr_clamp(int e[8], int r1, int g1, int b1, int a1, int r2, int g2, int b2, int a2) {
    set_ep(e, clamp_hdr(r1), clamp_hdr(g1), clamp_hdr(b1), clamp_hdr(a1), clamp_hdr(r2), clamp_hdr(g2), clamp_hdr(b2), clamp_hdr(a2));
}
static float half_to_float(uint16_t h) {   /* exact IEEE binary16 -> binary32 */
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = h >> 10 & 0x1f, man = h & 0x3ff, bits;
    if (exp == 0x1f) bits = sign | 0x7f800000u | man << 13;
    else if (exp) bits = sign | (exp + 112) << 23 | man << 13;
    else if (!man) bits = sign;
    else { exp = 113; while (!(man & 0x400)) { man <<= 1; exp--; } bits = sign | exp << 23 | (man & 0x3ff) << 13; }
    float f; memcpy(&f, &bits, 4); return f;
}
static unsigned select_color(int v0, int v1, int weight) {
    return (uint8_t)(((((v0 << 8 | v0) * (64 - weight) + (v1 << 8 | v1) * weight + 32) >> 6) * 255 + 32768) / 65536);
}
static unsigned select_color_hdr(int v0, int v1, int weight) {
    uint16_t c = (uint16_t)(((v0 << 4) * (64 - weight) + (v1 << 4) * weight + 32) >> 6), m = c & 0x7ff;
    if (m < 512) m = (uint16_t)(m * 3);
    else if (m < 1536) m = (uint16_t)(4 * m - 512);
    else m = (uint16_t)(5 * m - 2048);
    float f = half_to_float((uint16_t)((c >> 1 & 0x7c00) | m >> 3));
    return isfinite(f) ? (uint8_t)clamp8((int)roundf(f * 255)) : 255;
}
static unsigned f32_to_u8(float f) { float c = roundf(f * 255); return c < 0 ? 0 : c > 255 ? 255 : (unsigned)c; }

typedef struct {
    int bw, bh, width, height, part_num, dual_plane, plane_selector, weight_range, weight_num, cem[4], cem_range;
    int endpoint_value_num, endpoints[4][8], weights[144][2], partition[144];
} Block;
typedef struct { int bits, nonbits; } Seq;

/* The trit / quint unpacking of the ASTC integer sequence encoding (the decoder's TritsTable / QuintsTable). */
static void trits(unsigned t, int out[5]) {
    unsigned c;
    if ((t >> 2 & 7) == 7) { c = (t >> 5 & 7) << 2 | (t & 3); out[4] = 2; out[3] = 2; }
    else { c = t & 0x1f; if ((t >> 5 & 3) == 3) { out[4] = 2; out[3] = t >> 7 & 1; } else { out[4] = t >> 7 & 1; out[3] = t >> 5 & 3; } }
    if ((c & 3) == 3) { out[2] = 2; out[1] = c >> 4 & 1; out[0] = (int)((c >> 3 & 1) << 1 | ((c >> 2 & 1) & ~(c >> 3) & 1)); }
    else if ((c >> 2 & 3) == 3) { out[2] = 2; out[1] = 2; out[0] = c & 3; }
    else { out[2] = c >> 4 & 1; out[1] = c >> 2 & 3; out[0] = (int)((c >> 1 & 1) << 1 | ((c & 1) & ~(c >> 1) & 1)); }
}
static void quints(unsigned q, int out[3]) {
    unsigned c;
    if ((q >> 1 & 3) == 3 && (q >> 5 & 3) == 0) {
        out[2] = (int)((q & 1) << 2 | ((q >> 4 & 1) & ~q & 1) << 1 | ((q >> 3 & 1) & ~q & 1)); out[1] = 4; out[0] = 4; return;
    }
    if ((q >> 1 & 3) == 3) { out[2] = 4; c = (q >> 3 & 3) << 3 | (~q >> 5 & 3) << 1 | (q & 1); }
    else { out[2] = q >> 5 & 3; c = q & 0x1f; }
    if ((c & 7) == 5) { out[1] = 4; out[0] = c >> 3 & 3; }
    else { out[1] = c >> 3 & 3; out[0] = c & 7; }
}
static void decode_intseq(const uint8_t *buf, int offset, int a, int b, int count, int reverse, Seq *out) {
    static const int mt[] = { 0, 2, 4, 5, 7 }, mq[] = { 0, 3, 5 };
    if (count <= 0) return;
    int n = 0;
    if (a == 3) {
        int mask = (1 << b) - 1, block_count = (count + 4) / 5, last_count = (count + 4) % 5 + 1;
        int block_size = 8 + 5 * b, last_size = (block_size * last_count + 4) / 5;
        for (int i = 0, p = offset; i < block_count; i++, p += reverse ? -block_size : block_size) {
            int now = i < block_count - 1 ? block_size : last_size;
            uint64_t d = reverse ? bit_reverse_u64(getbits64(buf, p - now, now), now) : getbits64(buf, p, now);
            unsigned x = (unsigned)((d >> b & 3) | (d >> b * 2 & 0xc) | (d >> b * 3 & 0x10) | (d >> b * 4 & 0x60) | (d >> b * 5 & 0x80));
            int t[5]; trits(x, t);
            for (int j = 0; j < 5 && n < count; j++, n++) out[n] = (Seq){ (int)(d >> (mt[j] + b * j) & (uint64_t)mask), t[j] };
        }
    } else if (a == 5) {
        int mask = (1 << b) - 1, block_count = (count + 2) / 3, last_count = (count + 2) % 3 + 1;
        int block_size = 7 + 3 * b, last_size = (block_size * last_count + 2) / 3;
        for (int i = 0, p = offset; i < block_count; i++, p += reverse ? -block_size : block_size) {
            int now = i < block_count - 1 ? block_size : last_size;
            uint64_t d = reverse ? bit_reverse_u64(getbits64(buf, p - now, now), now) : getbits64(buf, p, now);
            unsigned x = (unsigned)((d >> b & 7) | (d >> b * 2 & 0x18) | (d >> b * 3 & 0x60));
            int q[3]; quints(x, q);
            for (int j = 0; j < 3 && n < count; j++, n++) out[n] = (Seq){ (int)(d >> (mq[j] + b * j) & (uint64_t)mask), q[j] };
        }
    } else if (reverse) {
        for (int p = offset - b; n < count; n++, p -= b) out[n] = (Seq){ (int)(rev8((unsigned)getbits(buf, p, b)) >> (8 - b)), 0 };
    } else {
        for (int p = offset; n < count; n++, p += b) out[n] = (Seq){ getbits(buf, p, b), 0 };
    }
}

static int block_params(const uint8_t *buf, Block *d) {
    d->dual_plane = !!(buf[1] & 4);
    d->weight_range = (buf[0] >> 4 & 1) | (buf[1] << 2 & 8);
    if (buf[0] & 3) {
        d->weight_range |= buf[0] << 1 & 6;
        switch (buf[0] & 0xc) {
        case 0: d->width = (le16(buf) >> 7 & 3) + 4; d->height = (buf[0] >> 5 & 3) + 2; break;
        case 4: d->width = (le16(buf) >> 7 & 3) + 8; d->height = (buf[0] >> 5 & 3) + 2; break;
        case 8: d->width = (buf[0] >> 5 & 3) + 2; d->height = (le16(buf) >> 7 & 3) + 8; break;
        default:
            if (buf[1] & 1) { d->width = (buf[0] >> 7 & 1) + 2; d->height = (buf[0] >> 5 & 3) + 2; }
            else { d->width = (buf[0] >> 5 & 3) + 2; d->height = (buf[0] >> 7 & 1) + 6; }
        }
    } else {
        d->weight_range |= buf[0] >> 1 & 6;
        switch (le16(buf) & 0x180) {
        case 0: d->width = 12; d->height = (buf[0] >> 5 & 3) + 2; break;
        case 0x80: d->width = (buf[0] >> 5 & 3) + 2; d->height = 12; break;
        case 0x100: d->width = (buf[0] >> 5 & 3) + 6; d->height = (buf[1] >> 1 & 3) + 6; d->dual_plane = 0; d->weight_range &= 7; break;
        default: d->width = (buf[0] & 0x20) ? 10 : 6; d->height = (buf[0] & 0x20) ? 6 : 10;
        }
    }
    d->part_num = (buf[1] >> 3 & 3) + 1;
    d->weight_num = d->width * d->height * (d->dual_plane ? 2 : 1);
    /* Beyond the 64 weights a valid block holds the decoder would overrun its arrays: an error block. */
    if (d->weight_num > 64 || d->width > d->bw || d->height > d->bh) return -1;
    int weight_bits, config_bits, cem_base = 0, wr = d->weight_range;
    if (WPREC_A[wr] == 3) weight_bits = d->weight_num * WPREC_B[wr] + (d->weight_num * 8 + 4) / 5;
    else if (WPREC_A[wr] == 5) weight_bits = d->weight_num * WPREC_B[wr] + (d->weight_num * 7 + 2) / 3;
    else weight_bits = d->weight_num * WPREC_B[wr];
    if (weight_bits > 96) return -1;
    if (d->part_num == 1) { d->cem[0] = le16(buf + 1) >> 5 & 0xf; config_bits = 17; }
    else {
        cem_base = le16(buf + 2) >> 7 & 3;
        if (!cem_base) {
            int cem = buf[3] >> 1 & 0xf;
            for (int i = 0; i < d->part_num; i++) d->cem[i] = cem;
            config_bits = 29;
        } else {
            for (int i = 0; i < d->part_num; i++) d->cem[i] = ((buf[3] >> (i + 1) & 1) + cem_base - 1) << 2;
            switch (d->part_num) {
            case 2: d->cem[0] |= buf[3] >> 3 & 3; d->cem[1] |= getbits(buf, 126 - weight_bits, 2); break;
            case 3:
                d->cem[0] |= buf[3] >> 4 & 1; d->cem[0] |= getbits(buf, 122 - weight_bits, 2) & 2;
                d->cem[1] |= getbits(buf, 124 - weight_bits, 2); d->cem[2] |= getbits(buf, 126 - weight_bits, 2); break;
            default: for (int i = 0; i < 4; i++) d->cem[i] |= getbits(buf, 120 + i * 2 - weight_bits, 2);
            }
            config_bits = 25 + d->part_num * 3;
        }
    }
    if (d->dual_plane) {
        config_bits += 2;
        d->plane_selector = getbits(buf, cem_base ? 130 - weight_bits - d->part_num * 3 : 126 - weight_bits, 2);
    }
    int remain = 128 - config_bits - weight_bits;
    d->endpoint_value_num = 0;
    for (int i = 0; i < d->part_num; i++) d->endpoint_value_num += (d->cem[i] >> 1 & 6) + 2;
    if (d->endpoint_value_num > 18) return -1;
    d->cem_range = -1;
    for (int i = 0, bits; i < 19; i++) {
        int ev = d->endpoint_value_num;
        if (CEM_A[i] == 3) bits = ev * CEM_B[i] + (ev * 8 + 4) / 5;
        else if (CEM_A[i] == 5) bits = ev * CEM_B[i] + (ev * 7 + 2) / 3;
        else bits = ev * CEM_B[i];
        if (bits <= remain) { d->cem_range = i; break; }
    }
    return d->cem_range < 0 ? -1 : 0;
}

static void endpoints_hdr7(int *e, int *v) {
    int modeval = (v[2] >> 4 & 0x8) | (v[1] >> 5 & 0x4) | (v[0] >> 6), major, mode;
    if ((modeval & 0xc) != 0xc) { major = modeval >> 2; mode = modeval & 3; }
    else if (modeval != 0xf) { major = modeval & 3; mode = 4; }
    else { major = 0; mode = 5; }
    int c[4] = { v[0] & 0x3f, v[1] & 0x1f, v[2] & 0x1f, v[3] & 0x1f }, sh = 0;
    switch (mode) {
    case 0: c[3] |= v[3] & 0x60; c[0] |= v[3] >> 1 & 0x40; c[0] |= v[2] << 1 & 0x80; c[0] |= v[1] << 3 & 0x300; c[0] |= v[2] << 5 & 0x400; sh = 1; break;
    case 1: c[1] |= v[1] & 0x20; c[2] |= v[2] & 0x20; c[0] |= v[3] >> 1 & 0x40; c[0] |= v[2] << 1 & 0x80; c[0] |= v[1] << 2 & 0x100; c[0] |= v[3] << 4 & 0x600; sh = 1; break;
    case 2: c[3] |= v[3] & 0xe0; c[0] |= v[2] << 1 & 0xc0; c[0] |= v[1] << 3 & 0x300; sh = 2; break;
    case 3: c[1] |= v[1] & 0x20; c[2] |= v[2] & 0x20; c[3] |= v[3] & 0x60; c[0] |= v[3] >> 1 & 0x40; c[0] |= v[2] << 1 & 0x80; c[0] |= v[1] << 2 & 0x100; sh = 3; break;
    case 4: c[1] |= v[1] & 0x60; c[2] |= v[2] & 0x60; c[3] |= v[3] & 0x20; c[0] |= v[3] >> 1 & 0x40; c[0] |= v[3] << 1 & 0x80; sh = 4; break;
    default: c[1] |= v[1] & 0x60; c[2] |= v[2] & 0x60; c[3] |= v[3] & 0x60; c[0] |= v[3] >> 1 & 0x40; sh = 5; break;
    }
    for (int i = 0; i < 4; i++) c[i] <<= sh;
    if (mode != 5) { c[1] = c[0] - c[1]; c[2] = c[0] - c[2]; }
    if (major == 1) set_ep_hdr_clamp(e, c[1] - c[3], c[0] - c[3], c[2] - c[3], 0x780, c[1], c[0], c[2], 0x780);
    else if (major == 2) set_ep_hdr_clamp(e, c[2] - c[3], c[1] - c[3], c[0] - c[3], 0x780, c[2], c[1], c[0], 0x780);
    else set_ep_hdr_clamp(e, c[0] - c[3], c[1] - c[3], c[2] - c[3], 0x780, c[0], c[1], c[2], 0x780);
}
static void endpoints_hdr11(int *e, int *v, int alpha1, int alpha2) {
    int major = (v[4] >> 7) | (v[5] >> 6 & 2);
    if (major == 3) { set_ep(e, v[0] << 4, v[2] << 4, v[4] << 5 & 0xfe0, alpha1, v[1] << 4, v[3] << 4, v[5] << 5 & 0xfe0, alpha2); return; }
    int mode = (v[1] >> 7) | (v[2] >> 6 & 2) | (v[3] >> 5 & 4);
    int va = v[0] | (v[1] << 2 & 0x100), vb0 = v[2] & 0x3f, vb1 = v[3] & 0x3f, vc = v[1] & 0x3f;
    int16_t vd0, vd1;
    switch (mode) {
    case 0: case 2:
        vd0 = (int16_t)(v[4] & 0x7f); if (vd0 & 0x40) vd0 = (int16_t)(vd0 | 0xff80);
        vd1 = (int16_t)(v[5] & 0x7f); if (vd1 & 0x40) vd1 = (int16_t)(vd1 | 0xff80); break;
    case 1: case 3: case 5: case 7:
        vd0 = (int16_t)(v[4] & 0x3f); if (vd0 & 0x20) vd0 = (int16_t)(vd0 | 0xffc0);
        vd1 = (int16_t)(v[5] & 0x3f); if (vd1 & 0x20) vd1 = (int16_t)(vd1 | 0xffc0); break;
    default:
        vd0 = (int16_t)(v[4] & 0x1f); if (vd0 & 0x10) vd0 = (int16_t)(vd0 | 0xffe0);
        vd1 = (int16_t)(v[5] & 0x1f); if (vd1 & 0x10) vd1 = (int16_t)(vd1 | 0xffe0); break;
    }
    switch (mode) {
    case 0: vb0 |= v[2] & 0x40; vb1 |= v[3] & 0x40; break;
    case 1: vb0 |= v[2] & 0x40; vb1 |= v[3] & 0x40; vb0 |= v[4] << 1 & 0x80; vb1 |= v[5] << 1 & 0x80; break;
    case 2: va |= v[2] << 3 & 0x200; vc |= v[3] & 0x40; break;
    case 3: va |= v[4] << 3 & 0x200; vc |= v[5] & 0x40; vb0 |= v[2] & 0x40; vb1 |= v[3] & 0x40; break;
    case 4: va |= v[4] << 4 & 0x200; va |= v[5] << 5 & 0x400; vb0 |= v[2] & 0x40; vb1 |= v[3] & 0x40; vb0 |= v[4] << 1 & 0x80; vb1 |= v[5] << 1 & 0x80; break;
    case 5: va |= v[2] << 3 & 0x200; va |= v[3] << 4 & 0x400; vc |= v[5] & 0x40; vc |= v[4] << 1 & 0x80; break;
    case 6: va |= v[4] << 4 & 0x200; va |= v[5] << 5 & 0x400; va |= v[4] << 5 & 0x800; vc |= v[5] & 0x40; vb0 |= v[2] & 0x40; vb1 |= v[3] & 0x40; break;
    default: va |= v[2] << 3 & 0x200; va |= v[3] << 4 & 0x400; va |= v[4] << 5 & 0x800; vc |= v[5] & 0x40; break;
    }
    int shamt = (mode >> 1) ^ 3;
    va <<= shamt; vb0 <<= shamt; vb1 <<= shamt; vc <<= shamt;
    vd0 = (int16_t)(vd0 * (1 << shamt)); vd1 = (int16_t)(vd1 * (1 << shamt));
    if (major == 1) set_ep_hdr_clamp(e, va - vb0 - vc - vd0, va - vc, va - vb1 - vc - vd1, alpha1, va - vb0, va, va - vb1, alpha2);
    else if (major == 2) set_ep_hdr_clamp(e, va - vb1 - vc - vd1, va - vb0 - vc - vd0, va - vc, alpha1, va - vb1, va - vb0, va, alpha2);
    else set_ep_hdr_clamp(e, va - vc, va - vb0 - vc - vd0, va - vb1 - vc - vd1, alpha1, va, va - vb0, va - vb1, alpha2);
}
static void decode_endpoints(const uint8_t *buf, Block *d) {
    static const int TRITS[] = { 0, 204, 93, 44, 22, 11, 5 }, QUINTS[] = { 0, 113, 54, 26, 13, 6 };
    Seq seq[32]; int ev[32] = { 0 };
    int ca = CEM_A[d->cem_range], cb = CEM_B[d->cem_range], n = d->endpoint_value_num;
    decode_intseq(buf, d->part_num == 1 ? 17 : 29, ca, cb, n, 0, seq);
    if (ca == 3 || ca == 5) {
        int c = ca == 3 ? TRITS[cb] : QUINTS[cb];
        for (int i = 0; i < n; i++) {
            int a = (seq[i].bits & 1) * 0x1ff, x = seq[i].bits >> 1, b = 0;
            if (ca == 3)
                switch (cb) { case 2: b = 0x116 * x; break; case 3: b = x << 7 | x << 2 | x; break; case 4: b = x << 6 | x; break;
                              case 5: b = x << 5 | x >> 2; break; case 6: b = x << 4 | x >> 4; break; default: b = 0; }
            else
                switch (cb) { case 2: b = 0x10c * x; break; case 3: b = x << 7 | x << 1 | x >> 1; break; case 4: b = x << 6 | x >> 1; break;
                              case 5: b = x << 5 | x >> 3; break; default: b = 0; }
            ev[i] = (a & 0x80) | ((seq[i].nonbits * c + b) ^ a) >> 2;
        }
    } else {
        for (int i = 0; i < n; i++) {
            int s = seq[i].bits;
            switch (cb) {
            case 1: ev[i] = s * 0xff; break;
            case 2: ev[i] = s * 0x55; break;
            case 3: ev[i] = s << 5 | s << 2 | s >> 1; break;
            case 4: ev[i] = s << 4 | s; break;
            case 5: ev[i] = s << 3 | s >> 2; break;
            case 6: ev[i] = s << 2 | s >> 4; break;
            case 7: ev[i] = s << 1 | s >> 6; break;
            default: ev[i] = s;
            }
        }
    }
    int *v = ev;
    for (int p = 0; p < d->part_num; v += (d->cem[p] / 4 + 1) * 2, p++) {
        int *e = d->endpoints[p];
        switch (d->cem[p]) {
        case 0: set_ep(e, v[0], v[0], v[0], 255, v[1], v[1], v[1], 255); break;
        case 1: { int l0 = (v[0] >> 2) | (v[1] & 0xc0), l1 = clamp8(l0 + (v[1] & 0x3f)); set_ep(e, l0, l0, l0, 255, l1, l1, l1, 255); } break;
        case 2: {
            int y0, y1;
            if (v[0] <= v[1]) { y0 = v[0] << 4; y1 = v[1] << 4; } else { y0 = (v[1] << 4) + 8; y1 = (v[0] << 4) - 8; }
            set_ep(e, y0, y0, y0, 0x780, y1, y1, y1, 0x780);
        } break;
        case 3: {
            int y0, dd;
            if (v[0] & 0x80) { y0 = (v[1] & 0xe0) << 4 | (v[0] & 0x7f) << 2; dd = (v[1] & 0x1f) << 2; }
            else { y0 = (v[1] & 0xf0) << 4 | (v[0] & 0x7f) << 1; dd = (v[1] & 0x0f) << 1; }
            int y1 = clamp_hdr(y0 + dd);
            set_ep(e, y0, y0, y0, 0x780, y1, y1, y1, 0x780);
        } break;
        case 4: set_ep(e, v[0], v[0], v[0], v[2], v[1], v[1], v[1], v[3]); break;
        case 5:
            bit_transfer_signed(&v[1], &v[0]); bit_transfer_signed(&v[3], &v[2]); v[1] += v[0];
            set_ep_clamp(e, v[0], v[0], v[0], v[2], v[1], v[1], v[1], v[2] + v[3]); break;
        case 6: set_ep(e, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, 255, v[0], v[1], v[2], 255); break;
        case 7: endpoints_hdr7(e, v); break;
        case 8:
            if (v[0] + v[2] + v[4] <= v[1] + v[3] + v[5]) set_ep(e, v[0], v[2], v[4], 255, v[1], v[3], v[5], 255);
            else set_ep_blue(e, v[1], v[3], v[5], 255, v[0], v[2], v[4], 255);
            break;
        case 9:
            bit_transfer_signed(&v[1], &v[0]); bit_transfer_signed(&v[3], &v[2]); bit_transfer_signed(&v[5], &v[4]);
            if (v[1] + v[3] + v[5] >= 0) set_ep_clamp(e, v[0], v[2], v[4], 255, v[0] + v[1], v[2] + v[3], v[4] + v[5], 255);
            else set_ep_blue_clamp(e, v[0] + v[1], v[2] + v[3], v[4] + v[5], 255, v[0], v[2], v[4], 255);
            break;
        case 10: set_ep(e, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, v[4], v[0], v[1], v[2], v[5]); break;
        case 11: endpoints_hdr11(e, v, 0x780, 0x780); break;
        case 12:
            if (v[0] + v[2] + v[4] <= v[1] + v[3] + v[5]) set_ep(e, v[0], v[2], v[4], v[6], v[1], v[3], v[5], v[7]);
            else set_ep_blue(e, v[1], v[3], v[5], v[7], v[0], v[2], v[4], v[6]);
            break;
        case 13:
            bit_transfer_signed(&v[1], &v[0]); bit_transfer_signed(&v[3], &v[2]);
            bit_transfer_signed(&v[5], &v[4]); bit_transfer_signed(&v[7], &v[6]);
            if (v[1] + v[3] + v[5] >= 0) set_ep_clamp(e, v[0], v[2], v[4], v[6], v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7]);
            else set_ep_blue_clamp(e, v[0] + v[1], v[2] + v[3], v[4] + v[5], v[6] + v[7], v[0], v[2], v[4], v[6]);
            break;
        case 14: endpoints_hdr11(e, v, v[6], v[7]); break;
        default: {
            int mode = ((v[6] >> 7) & 1) | ((v[7] >> 6) & 2);
            v[6] &= 0x7f; v[7] &= 0x7f;
            if (mode == 3) endpoints_hdr11(e, v, v[6] << 5, v[7] << 5);
            else {
                v[6] |= (v[7] << (mode + 1)) & 0x780;
                v[7] = ((v[7] & (0x3f >> mode)) ^ (0x20 >> mode)) - (0x20 >> mode);
                v[6] <<= 4 - mode; v[7] <<= 4 - mode;
                endpoints_hdr11(e, v, v[6], clamp_hdr(v[6] + v[7]));
            }
        }
        }
    }
}
/* The bilinear weight infill of one texel (decode_weights' gs / gt arithmetic), which only depends on the block size and
   the weight grid: computed once per grid size per texture. */
typedef struct { uint8_t v, w00, w01, w10, w11; } Texel;
typedef struct { Texel *grid[13 * 13]; } Infill;   /* by width * 13 + height (2..12 each) */
static const Texel *infill(Infill *cache, const Block *d, Texel *local) {
    int key = d->width * 13 + d->height;
    if (cache && cache->grid[key]) return cache->grid[key];
    Texel *t = cache ? malloc((size_t)(d->bw * d->bh) * sizeof *t) : NULL;
    if (!t) t = local;   /* no memory for the cache: computed for this block only */
    int ds = (1024 + d->bw / 2) / (d->bw - 1), dt = (1024 + d->bh / 2) / (d->bh - 1);
    for (int y = 0, i = 0; y < d->bh; y++)
        for (int x = 0; x < d->bw; x++, i++) {
            int gs = (ds * x * (d->width - 1) + 32) >> 6, gt = (dt * y * (d->height - 1) + 32) >> 6;
            int fs = gs & 0xf, ft = gt & 0xf, v = (gs >> 4) + (gt >> 4) * d->width;
            int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            t[i] = (Texel){ (uint8_t)v, (uint8_t)w00, (uint8_t)w01, (uint8_t)w10, (uint8_t)w11 };
        }
    if (t != local) cache->grid[key] = t;
    return t;
}
static void decode_weights(const uint8_t *buf, Block *d, Infill *cache) {
    /* wv: the n decoded weights, then zeros the infill may read just past them ((v + width + 1) * pn + p) */
    Seq seq[64]; int wv[256], wa = WPREC_A[d->weight_range], wb = WPREC_B[d->weight_range], n = d->weight_num;
    int pn = d->dual_plane ? 2 : 1;
    memset(wv, 0, (size_t)((d->width * d->height + d->width + 1) * pn) * sizeof *wv);
    decode_intseq(buf, 128, wa, wb, n, 1, seq);
    if (wa == 0) {
        for (int i = 0; i < n; i++) {
            int s = seq[i].bits;
            switch (wb) {
            case 1: wv[i] = s ? 63 : 0; break;
            case 2: wv[i] = s << 4 | s << 2 | s; break;
            case 3: wv[i] = s << 3 | s; break;
            case 4: wv[i] = s << 2 | s >> 2; break;
            case 5: wv[i] = s << 1 | s >> 4; break;
            }
            if (wv[i] > 32) ++wv[i];
        }
    } else if (wb == 0) {
        for (int i = 0; i < n; i++) wv[i] = seq[i].nonbits * (wa == 3 ? 32 : 16);
    } else {
        for (int i = 0; i < n; i++) {
            if (wa == 3) {
                if (wb == 1) wv[i] = seq[i].nonbits * 50;
                else if (wb == 2) { wv[i] = seq[i].nonbits * 23; if (seq[i].bits & 2) wv[i] += 0x45; }
                else if (wb == 3) wv[i] = seq[i].nonbits * 11 + ((seq[i].bits << 4 | seq[i].bits >> 1) & 0x63);
            } else if (wa == 5) {
                if (wb == 1) wv[i] = seq[i].nonbits * 28;
                else if (wb == 2) { wv[i] = seq[i].nonbits * 13; if (seq[i].bits & 2) wv[i] += 0x42; }
            }
            int a = (seq[i].bits & 1) * 0x7f;
            wv[i] = (a & 0x20) | ((wv[i] ^ a) >> 2);
            if (wv[i] > 32) ++wv[i];
        }
    }
    Texel local[144];
    const Texel *tx = infill(cache, d, local);
    for (int i = 0; i < d->bw * d->bh; i++) {
        int v = tx[i].v, w00 = tx[i].w00, w01 = tx[i].w01, w10 = tx[i].w10, w11 = tx[i].w11;
        for (int p = 0; p < pn; p++)
            d->weights[i][p] = (wv[v * pn + p] * w00 + wv[(v + 1) * pn + p] * w01 + wv[(v + d->width) * pn + p] * w10 +
                                wv[(v + d->width + 1) * pn + p] * w11 + 8) >> 4;
    }
}
static void select_partition(const uint8_t *buf, Block *d) {
    int small = d->bw * d->bh < 31, seed = ((int)le32(buf) >> 13 & 0x3ff) | (d->part_num - 1) << 10;
    uint32_t r = (uint32_t)seed;
    r ^= r >> 15; r -= r << 17; r += r << 7; r += r << 4; r ^= r >> 5; r += r << 16; r ^= r >> 7; r ^= r >> 3; r ^= r << 6; r ^= r >> 17;
    int seeds[8];
    for (int i = 0; i < 8; i++) { seeds[i] = (int)((r >> (i * 4)) & 0xf); seeds[i] *= seeds[i]; }
    int sh[2] = { seed & 2 ? 4 : 5, d->part_num == 3 ? 6 : 5 };
    for (int i = 0; i < 8; i++) seeds[i] >>= (seed & 1) ? sh[i % 2] : sh[1 - i % 2];
    for (int t = 0, i = 0; t < d->bh; t++)
        for (int s = 0; s < d->bw; s++, i++) {
            int x = small ? s << 1 : s, y = small ? t << 1 : t;
            int a = (int)((seeds[0] * x + seeds[1] * y + (int)(r >> 14)) & 0x3f);
            int b = (int)((seeds[2] * x + seeds[3] * y + (int)(r >> 10)) & 0x3f);
            int c = d->part_num < 3 ? 0 : (int)((seeds[4] * x + seeds[5] * y + (int)(r >> 6)) & 0x3f);
            int e = d->part_num < 4 ? 0 : (int)((seeds[6] * x + seeds[7] * y + (int)(r >> 2)) & 0x3f);
            d->partition[i] = (a >= b && a >= c && a >= e) ? 0 : (b >= c && b >= e) ? 1 : (c >= e) ? 2 : 3;
        }
}
/* FuncTableC / FuncTableA: which colour endpoint modes interpolate as HDR. */
static int hdr_c(int cem) { return cem == 2 || cem == 3 || cem == 7 || cem == 11 || cem == 14 || cem == 15; }
static int hdr_a(int cem) { return cem == 2 || cem == 3 || cem == 7 || cem == 11 || cem == 15; }
/* select_color() for LDR endpoints (0..255, so unsigned arithmetic gives the same value as the signed original) */
static inline unsigned select_ldr(unsigned v0, unsigned v1, unsigned w) {
    return ((((v0 * 257u) * (64u - w) + (v1 * 257u) * w + 32u) >> 6) * 255u + 32768u) >> 16;
}
static void apply_color(const Block *d, uint32_t *out) {
    int ps[4] = { 0, 0, 0, 0 }, hdr = 0;
    if (d->dual_plane) ps[d->plane_selector] = 1;
    for (int p = 0; p < d->part_num; p++) hdr |= hdr_c(d->cem[p]) | hdr_a(d->cem[p]);
    if (!hdr) {   /* every endpoint pair is LDR: the common case, without the per-channel mode test */
        for (int i = 0; i < d->bw * d->bh; i++) {
            const int *e = d->endpoints[d->part_num > 1 ? d->partition[i] : 0];
            unsigned w0 = (unsigned)d->weights[i][0], w1 = d->dual_plane ? (unsigned)d->weights[i][1] : w0;
            out[i] = color(select_ldr((unsigned)e[0], (unsigned)e[4], ps[0] ? w1 : w0), select_ldr((unsigned)e[1], (unsigned)e[5], ps[1] ? w1 : w0),
                           select_ldr((unsigned)e[2], (unsigned)e[6], ps[2] ? w1 : w0), select_ldr((unsigned)e[3], (unsigned)e[7], ps[3] ? w1 : w0));
        }
        return;
    }
    for (int i = 0; i < d->bw * d->bh; i++) {
        int p = d->part_num > 1 ? d->partition[i] : 0, cem = d->cem[p];
        const int *e = d->endpoints[p];
        unsigned c[4];
        for (int k = 0; k < 4; k++) {
            int w = d->weights[i][d->dual_plane ? ps[k] : 0];
            c[k] = (k < 3 ? hdr_c(cem) : hdr_a(cem)) ? select_color_hdr(e[k], e[k + 4], w) : select_color(e[k], e[k + 4], w);
        }
        out[i] = color(c[0], c[1], c[2], c[3]);
    }
}
static void astc_block(const uint8_t *src, int bw, int bh, uint32_t *out, Infill *cache) {
    uint8_t buf[24] = { 0 };
    memcpy(buf, src, 16);
    if (buf[0] == 0xfc && (buf[1] & 1) == 1) {
        uint32_t c = (buf[1] & 2) ? color(f32_to_u8(half_to_float(le16(buf + 8))), f32_to_u8(half_to_float(le16(buf + 10))),
                                          f32_to_u8(half_to_float(le16(buf + 12))), f32_to_u8(half_to_float(le16(buf + 14))))
                                  : color(buf[9], buf[11], buf[13], buf[15]);
        for (int i = 0; i < bw * bh; i++) out[i] = c;
        return;
    }
    /* only the header fields need zeros: endpoints / weights / partition are written before they are read */
    Block d; memset(&d, 0, offsetof(Block, endpoints));
    d.bw = bw; d.bh = bh;
    if (((buf[0] & 0xc3) == 0xc0 && (buf[1] & 1) == 1) || (buf[0] & 0xf) == 0 || block_params(buf, &d)) {
        for (int i = 0; i < bw * bh; i++) out[i] = color(255, 0, 255, 255);
        return;
    }
    decode_endpoints(buf, &d);
    decode_weights(buf, &d, cache);
    if (d.part_num > 1) select_partition(buf, &d);
    apply_color(&d, out);
}
void cg_astc(const uint8_t *data, long w, long h, int bw, int bh, uint32_t *image) {
    uint32_t buf[144]; Infill cache = { { 0 } };
    for (long by = 0; by < (h + bh - 1) / bh; by++)
        for (long bx = 0; bx < (w + bw - 1) / bw; bx++, data += 16) { astc_block(data, bw, bh, buf, &cache); copy_block(bx, by, w, h, bw, bh, buf, image); }
    for (size_t k = 0; k < sizeof cache.grid / sizeof *cache.grid; k++) free(cache.grid[k]);
}

/* ---- BNTX ---- */
int cg_bntx(const uint8_t *d, size_t n, CgTex **out, size_t *count, SeedErr *err) {
    *out = NULL; *count = 0;
    if (n < 0x40 || memcmp(d, "BNTX", 4)) { seed_err(err, "internal", "Expected BNTX"); return -1; }
    size_t nx = 0;
    while (nx + 4 <= 0x40 && memcmp(d + nx, "NX  ", 4)) nx++;
    if (nx + 16 > 0x40) { seed_err(err, "internal", "Invalid BNTX header"); return -1; }
    uint32_t cnt = le32(d + nx + 4); uint64_t table = le64(d + nx + 8);
    if (table > n || cnt > (n - table) / 8) { seed_err(err, "internal", "Invalid BNTX texture table"); return -1; }
    CgTex *t = calloc(cnt ? cnt : 1, sizeof *t);
    if (!t) { seed_err(err, "internal", "Out of memory"); return -1; }
    for (uint32_t i = 0; i < cnt; i++) {
        uint64_t at = le64(d + table + i * 8);
        if (at > n || n - at < 0x78 || memcmp(d + at, "BRTI", 4)) goto bad;
        uint64_t name_at = le64(d + at + 0x60), ptrs = le64(d + at + 0x70);
        if (name_at > n - 2 || ptrs > n - 8) goto bad;
        const uint8_t *nul = memchr(d + name_at + 2, 0, n - name_at - 2);
        if (!nul) goto bad;
        t[i] = (CgTex){ (const char *)d + name_at + 2, le32(d + at + 0x1c) >> 8, le32(d + at + 0x24), le32(d + at + 0x28),
                        le32(d + at + 0x34) & 7, le64(d + ptrs), { d[at + 0x58], d[at + 0x59], d[at + 0x5a], d[at + 0x5b] },
                        le16(d + at + 0x12) };
    }
    *out = t; *count = cnt;
    return 0;
bad:
    free(t); seed_err(err, "internal", "Invalid BRTI");
    return -1;
}
const CgTex *cg_bntx_find(const CgTex *t, size_t count, const char *name) {
    for (size_t i = count; i-- > 0;) if (!strcmp(t[i].name, name)) return &t[i];
    return NULL;
}

static int astc_dims(uint32_t fmt, int *bw, int *bh) {
    static const uint8_t dims[14][2] = { {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12} };
    if (fmt < 0x2d || fmt > 0x3a) return 0;
    *bw = dims[fmt - 0x2d][0]; *bh = dims[fmt - 0x2d][1];
    return 1;
}
/* deswizzle(): Tegra X1 block-linear (GOBs of 64 bytes x 8 rows, block height shrunk for small images). */
static uint8_t *deswizzle(const uint8_t *data, size_t n, size_t width, size_t height, size_t bpp, unsigned log_height) {
    size_t bh = (size_t)1 << log_height;
    while (bh > 1 && (height + 7) / 8 <= bh / 2) bh /= 2;
    size_t gobs = (width * bpp + 63) / 64, row = width * bpp;
    size_t bytes = row * height;
    uint8_t *out = malloc(bytes ? bytes : 1);
    if (!out) return NULL;
    for (size_t y = 0; y < height; y++) {
        size_t gy = y / (8 * bh) * 512 * bh * gobs + y % (8 * bh) / 8 * 512, yp = y % 8 / 2 * 64 + y % 2 * 16;
        uint8_t *dest = out + y * row;
        for (size_t x = 0; x < row; x += bpp, dest += bpp) {
            size_t src = gy + x / 64 * 512 * bh + x % 64 / 32 * 256 + yp + x % 32 / 16 * 32 + x % 16;
            if (src > n || n - src < bpp) { free(out); errno = ERANGE; return NULL; }
            memcpy(dest, data + src, bpp);
        }
    }
    return out;
}

CgImg *cg_tex_decode(const uint8_t *data, size_t n, const CgTex *t, SeedErr *err) {
    uint32_t fmt = t->fmt; int bw = 4, bh = 4; size_t bpp;
    int astc = astc_dims(fmt, &bw, &bh);
    if (astc) bpp = 16;
    else if (fmt == 0x0b) { bw = bh = 1; bpp = 4; }
    else if (fmt == 0x02) { bw = bh = 1; bpp = 1; }
    else if (fmt == 0x07 || fmt == 0x09) { bw = bh = 1; bpp = 2; }
    else if (fmt == 0x1a || fmt == 0x1c || fmt == 0x1d || fmt == 0x1e || fmt == 0x20) bpp = (fmt == 0x1a || fmt == 0x1d) ? 8 : 16;
    else { seed_err(err, "internal", "Unsupported BNTX format %#x (%s)", fmt, t->name); return NULL; }
    for (int i = 0; i < 4; i++)
        if (t->sel[i] > 5) { seed_err(err, "internal", "Invalid BNTX channel selectors (%s)", t->name); return NULL; }
    size_t w = t->w, h = t->h, wb = (w + (size_t)bw - 1) / (size_t)bw, hb = (h + (size_t)bh - 1) / (size_t)bh;
    if (!w || !h || w > 16384 || h > 16384 || t->mip0 > n) { seed_err(err, "internal", "Invalid BNTX texture %s", t->name); return NULL; }
    uint8_t *linear;
    if (t->tile == 1) {
        if (n - t->mip0 < wb * hb * bpp) { seed_err(err, "internal", "Truncated BNTX mip (%s)", t->name); return NULL; }
        linear = malloc(wb * hb * bpp);
        if (linear) memcpy(linear, data + t->mip0, wb * hb * bpp);
    } else {
        linear = deswizzle(data + t->mip0, n - t->mip0, wb, hb, bpp, t->block);
        if (!linear && errno == ERANGE) { seed_err(err, "internal", "Truncated BNTX mip (%s)", t->name); return NULL; }
    }
    CgImg *src = linear ? cg_new(CG_RGBA, (int)w, (int)h) : NULL, *out = NULL;
    uint32_t *bgra = src ? malloc(w * h * 4) : NULL;
    if (!bgra) { seed_err(err, "internal", "Out of memory decoding %s", t->name); goto done; }
    uint8_t *px = src->p;
    if (astc || fmt >= 0x1a) {
        if (astc) cg_astc(linear, (long)w, (long)h, bw, bh, bgra);
        else if (fmt == 0x1a) cg_bc1(linear, (long)w, (long)h, bgra);
        else if (fmt == 0x1c) cg_bc3(linear, (long)w, (long)h, bgra);
        else if (fmt == 0x1d) cg_bc4(linear, (long)w, (long)h, bgra);
        else if (fmt == 0x1e) cg_bc5(linear, (long)w, (long)h, bgra);
        else cg_bc7(linear, (long)w, (long)h, bgra);
        for (size_t i = 0; i < w * h; i++) {   /* frombytes(..., 'raw', 'BGRA') */
            uint32_t v = bgra[i];
            px[i * 4] = (uint8_t)(v >> 16); px[i * 4 + 1] = (uint8_t)(v >> 8); px[i * 4 + 2] = (uint8_t)v; px[i * 4 + 3] = (uint8_t)(v >> 24);
        }
    } else {
        for (size_t i = 0; i < w * h; i++) {
            uint8_t *o = px + i * 4;
            if (fmt == 0x02) { o[0] = linear[i]; o[1] = o[2] = 0; o[3] = 255; }
            else if (fmt == 0x07) {   /* Pillow's 'BGR;16' unpacker */
                int v = linear[i * 2] | linear[i * 2 + 1] << 8;
                o[2] = (uint8_t)((v & 31) * 255 / 31); o[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
                o[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31); o[3] = 255;
            } else if (fmt == 0x09) { o[0] = linear[i * 2]; o[1] = linear[i * 2 + 1]; o[2] = 0; o[3] = 255; }
            else memcpy(o, linear + i * 4, 4);
        }
    }
    /* Image.merge('RGBA', [(0, 255, R, G, B, A)[s] for s in selectors]); the identity (R, G, B, A) changes nothing */
    if (t->sel[0] != 2 || t->sel[1] != 3 || t->sel[2] != 4 || t->sel[3] != 5)
        for (size_t i = 0; i < w * h; i++) {
            uint8_t *o = px + i * 4, ch[6] = { 0, 255, o[0], o[1], o[2], o[3] };
            for (int c = 0; c < 4; c++) o[c] = ch[t->sel[c]];
        }
    out = src; src = NULL;
done:
    free(bgra); free(linear); cg_free(src);
    return out;
}
