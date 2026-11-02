/* The Pillow 12.3.0 image operations the companion generator uses (Image.resize / thumbnail / transform / crop / paste /
   alpha_composite, ImageChops.multiply / invert, the RGBA <-> RGBa / L conversions), ported from libImaging (Resample.c,
   Geometry.c, AlphaComposite.c, Convert.c, Chops.c) with the same integer and floating-point steps, so the device writes
   the pixels make_companion_assets.py writes.

   Pillow: Copyright (c) 1997-2011 by Secret Labs AB, (c) 1995-2011 by Fredrik Lundh and contributors, (c) 2010 by
   Jeffrey A. Clark and contributors; MIT-CMU licence (companion/THIRD_PARTY_LICENSES.txt). */
#include "cg.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

CgImg *cg_new(int mode, int w, int h) {
    if (w < 0 || h < 0 || (w && (size_t)h > SIZE_MAX / 4 / (size_t)w)) return NULL;
    CgImg *im = malloc(sizeof *im);
    if (!im) return NULL;
    size_t n = (size_t)w * (size_t)h * (mode == CG_L ? 1 : 4);
    if (!(im->p = calloc(n ? n : 1, 1))) { free(im); return NULL; }
    im->mode = mode; im->w = w; im->h = h;
    return im;
}
CgImg *cg_new_fill(int mode, int w, int h, const uint8_t px[4]) {
    CgImg *im = cg_new(mode, w, h);
    if (im && mode != CG_L) for (size_t i = 0; i < (size_t)w * h; i++) memcpy(im->p + i * 4, px, 4);
    else if (im) memset(im->p, px[0], (size_t)w * h);
    return im;
}
void cg_free(CgImg *im) { if (im) { free(im->p); free(im); } }
CgImg *cg_copy(const CgImg *src) {
    CgImg *im = cg_new(src->mode, src->w, src->h);
    if (im) memcpy(im->p, src->p, (size_t)src->w * src->h * cg_bpp(src));
    return im;
}

/* ImagingCrop: the box may reach outside the image; those pixels are 0. */
CgImg *cg_crop(const CgImg *src, int x0, int y0, int x1, int y1) {
    CgImg *im = cg_new(src->mode, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
    if (im) cg_paste(im, src, -x0, -y0);
    return im;
}
void cg_paste(CgImg *dst, const CgImg *src, int x, int y) {
    int bpp = cg_bpp(dst);
    int sx = x < 0 ? -x : 0, sy = y < 0 ? -y : 0, dx = x < 0 ? 0 : x, dy = y < 0 ? 0 : y;
    int w = src->w - sx, h = src->h - sy;
    if (w > dst->w - dx) w = dst->w - dx;
    if (h > dst->h - dy) h = dst->h - dy;
    for (int r = 0; r < h; r++) memcpy(cg_px(dst, dx, dy + r), cg_px(src, sx, sy + r), (size_t)w * bpp);
}

#define SHIFTFORDIV255(a) ((((a) >> 8) + (a)) >> 8)
#define MULDIV255(a, b) SHIFTFORDIV255((unsigned)(a) * (unsigned)(b) + 128)
#define CLIP8(v) ((v) <= 0 ? 0 : (v) < 256 ? (v) : 255)
CgImg *cg_convert(const CgImg *src, int mode) {
    CgImg *im = cg_new(mode, src->w, src->h);
    if (!im) return NULL;
    size_t n = (size_t)src->w * src->h;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *in = src->p + i * 4;
        if (mode == CG_L) { im->p[i] = (uint8_t)((in[0] * 19595 + in[1] * 38470 + in[2] * 7471 + 0x8000) >> 16); continue; }
        uint8_t *out = im->p + i * 4; unsigned a = in[3];
        if (src->mode == CG_RGBA && mode == CG_RGBa) {   /* rgbA2rgba */
            out[0] = (uint8_t)MULDIV255(in[0], a); out[1] = (uint8_t)MULDIV255(in[1], a); out[2] = (uint8_t)MULDIV255(in[2], a); out[3] = (uint8_t)a;
        } else if (src->mode == CG_RGBa && mode == CG_RGBA) {   /* rgba2rgbA */
            if (a == 255 || a == 0) memcpy(out, in, 3);
            else for (int c = 0; c < 3; c++) { unsigned v = 255 * in[c] / a; out[c] = (uint8_t)CLIP8(v); }
            out[3] = (uint8_t)a;
        } else { memcpy(out, in, 3); out[3] = 255; }   /* rgb2rgba / rgba2rgb */
    }
    return im;
}

/* ---- Resample.c ---- */
typedef struct { double (*fn)(double); double support; } Filter;
static double bilinear_f(double x) { if (x < 0.0) x = -x; return x < 1.0 ? 1.0 - x : 0.0; }
static double bicubic_f(double x) {
    const double a = -0.5;
    if (x < 0.0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
    if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}
static double sinc_f(double x) { if (x == 0.0) return 1.0; x = x * M_PI; return sin(x) / x; }
static double lanczos_f(double x) { return (-3.0 <= x && x < 3.0) ? sinc_f(x) * sinc_f(x / 3) : 0.0; }
static const Filter F_BILINEAR = { bilinear_f, 1.0 }, F_BICUBIC = { bicubic_f, 2.0 }, F_LANCZOS = { lanczos_f, 3.0 };
#define PRECISION_BITS (32 - 8 - 2)
static uint8_t clip8(int in) {
    in >>= PRECISION_BITS;
    return (uint8_t)(in < 0 ? 0 : in > 255 ? 255 : in);
}
static int precompute_coeffs(int in_size, float in0, float in1, int out_size, const Filter *f, int **boundsp, int32_t **kkp) {
    double scale = (double)(in1 - in0) / out_size, filterscale = scale < 1.0 ? 1.0 : scale;
    double support = f->support * filterscale;
    int ksize = (int)ceil(support) * 2 + 1;
    if (out_size > INT_MAX / (ksize * (int)sizeof(double))) return 0;
    double *kk = malloc((size_t)out_size * ksize * sizeof(double));
    int *bounds = malloc((size_t)out_size * 2 * sizeof(int));
    int32_t *ik = malloc((size_t)out_size * ksize * sizeof(int32_t));
    if (!kk || !bounds || !ik) { free(kk); free(bounds); free(ik); return 0; }
    double inv_filterscale = 1.0 / filterscale;
    for (int xx = 0; xx < out_size; xx++) {
        double center = in0 + (xx + 0.5) * scale, ww = 0.0;
        int xmin = (int)(center - support + 0.5), xmax = (int)(center + support + 0.5), x;
        if (xmin < 0) xmin = 0;
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        double *k = &kk[xx * ksize];
        for (x = 0; x < xmax; x++) { double w = f->fn((x + xmin - center + 0.5) * inv_filterscale); k[x] = w; ww += w; }
        if (ww != 0.0) for (x = 0; x < xmax; x++) k[x] /= ww;
        for (; x < ksize; x++) k[x] = 0;
        bounds[xx * 2] = xmin; bounds[xx * 2 + 1] = xmax;
    }
    for (int i = 0; i < out_size * ksize; i++)   /* normalize_coeffs_8bpc */
        ik[i] = kk[i] < 0 ? (int)(-0.5 + kk[i] * (1 << PRECISION_BITS)) : (int)(0.5 + kk[i] * (1 << PRECISION_BITS));
    free(kk);
    *boundsp = bounds; *kkp = ik;
    return ksize;
}
/* bands: 1 (L), 3 (RGB: the fourth byte becomes 0) or 4 */
static int bands_of(const CgImg *im) { return im->mode == CG_L ? 1 : im->mode == CG_RGB ? 3 : 4; }
/* The 4-byte modes with the band count fixed at compile time (the inner loops of Resample.c ImagingResampleHorizontal /
   Vertical_8bpc); integer sums, so the result does not depend on how the loops are written. */
static inline void resample_h4(CgImg *out, const CgImg *in, int offset, int ksize, const int *bounds, const int32_t *kk, const int bands) {
    for (int yy = 0; yy < out->h; yy++) {
        const uint8_t *line = in->p + (size_t)(yy + offset) * in->w * 4;
        uint8_t *o = out->p + (size_t)yy * out->w * 4;
        for (int xx = 0; xx < out->w; xx++) {
            int xmin = bounds[xx * 2], xmax = bounds[xx * 2 + 1];
            const int32_t *k = &kk[xx * ksize];
            const uint8_t *l = line + (size_t)xmin * 4;
            int s0 = 1 << (PRECISION_BITS - 1), s1 = s0, s2 = s0, s3 = s0;
            for (int x = 0; x < xmax; x++, l += 4) {
                s0 += l[0] * k[x]; s1 += l[1] * k[x]; s2 += l[2] * k[x];
                if (bands == 4) s3 += l[3] * k[x];
            }
            o[xx * 4] = clip8(s0); o[xx * 4 + 1] = clip8(s1); o[xx * 4 + 2] = clip8(s2); o[xx * 4 + 3] = bands == 4 ? clip8(s3) : 0;
        }
    }
}
static void resample_h(CgImg *out, const CgImg *in, int offset, int ksize, const int *bounds, const int32_t *kk) {
    int bands = bands_of(in), bpp = cg_bpp(in);
    if (bands == 4) { resample_h4(out, in, offset, ksize, bounds, kk, 4); return; }
    if (bands == 3) { resample_h4(out, in, offset, ksize, bounds, kk, 3); return; }
    for (int yy = 0; yy < out->h; yy++) {
        const uint8_t *line = in->p + (size_t)(yy + offset) * in->w * bpp;
        uint8_t *o = out->p + (size_t)yy * out->w * bpp;
        for (int xx = 0; xx < out->w; xx++) {
            int xmin = bounds[xx * 2], xmax = bounds[xx * 2 + 1];
            const int32_t *k = &kk[xx * ksize];
            int ss = 1 << (PRECISION_BITS - 1);
            for (int x = 0; x < xmax; x++) ss += line[x + xmin] * k[x];
            o[xx] = clip8(ss);
        }
    }
}
/* vertical pass: one output row at a time, accumulated over whole input rows (row-contiguous reads) */
static inline void resample_v4(CgImg *out, const CgImg *in, int ksize, const int *bounds, const int32_t *kk, const int bands, int *acc) {
    size_t n = (size_t)out->w * 4;
    for (int yy = 0; yy < out->h; yy++) {
        const int32_t *k = &kk[yy * ksize];
        int ymin = bounds[yy * 2], ymax = bounds[yy * 2 + 1];
        for (size_t i = 0; i < n; i++) acc[i] = 1 << (PRECISION_BITS - 1);
        for (int y = 0; y < ymax; y++) {
            const uint8_t *p = in->p + (size_t)(y + ymin) * n; int ky = k[y];
            for (size_t i = 0; i < n; i++) acc[i] += p[i] * ky;
        }
        uint8_t *o = out->p + (size_t)yy * n;
        for (size_t i = 0; i < n; i++) o[i] = (bands == 3 && i % 4 == 3) ? 0 : clip8(acc[i]);
    }
}
static void resample_v(CgImg *out, const CgImg *in, int ksize, const int *bounds, const int32_t *kk) {
    int bands = bands_of(in), bpp = cg_bpp(in);
    int *acc = bands > 1 ? malloc((size_t)out->w * 4 * sizeof *acc) : NULL;
    if (acc) {
        if (bands == 4) resample_v4(out, in, ksize, bounds, kk, 4, acc);
        else resample_v4(out, in, ksize, bounds, kk, 3, acc);
        free(acc);
        return;
    }
    for (int yy = 0; yy < out->h; yy++) {
        const int32_t *k = &kk[yy * ksize];
        int ymin = bounds[yy * 2], ymax = bounds[yy * 2 + 1];
        uint8_t *o = out->p + (size_t)yy * out->w * bpp;
        for (int xx = 0; xx < out->w; xx++) {
            int ss[4] = { 1 << (PRECISION_BITS - 1), 1 << (PRECISION_BITS - 1), 1 << (PRECISION_BITS - 1), 1 << (PRECISION_BITS - 1) };
            for (int y = 0; y < ymax; y++) {
                const uint8_t *p = in->p + ((size_t)(y + ymin) * in->w + xx) * bpp;
                for (int b = 0; b < bands; b++) ss[b] += p[b] * k[y];
            }
            if (bands == 1) o[xx] = clip8(ss[0]);
            else for (int b = 0; b < 4; b++) o[xx * 4 + b] = b < bands ? clip8(ss[b]) : 0;
        }
    }
}
static CgImg *resample(const CgImg *in, int xsize, int ysize, const Filter *f, const float box[4]) {
    int need_h = xsize != in->w || box[0] || box[2] != xsize, need_v = ysize != in->h || box[1] || box[3] != ysize;
    int *bv = NULL, *bh = NULL; int32_t *kv = NULL, *kh = NULL;
    int ksv = precompute_coeffs(in->h, box[1], box[3], ysize, f, &bv, &kv);
    if (!ksv) return NULL;
    int yfirst = bv[0], ylast = bv[ysize * 2 - 2] + bv[ysize * 2 - 1];
    CgImg *tmp = NULL, *out = NULL; const CgImg *cur = in;
    if (need_h) {
        int ksh = precompute_coeffs(in->w, box[0], box[2], xsize, f, &bh, &kh);
        if (!ksh) goto done;
        for (int i = 0; i < ysize; i++) bv[i * 2] -= yfirst;
        if (!(tmp = cg_new(in->mode, xsize, ylast - yfirst))) goto done;
        resample_h(tmp, in, yfirst, ksh, bh, kh);
        cur = tmp;
    }
    if (need_v) {
        if ((out = cg_new(in->mode, cur->w, ysize))) resample_v(out, cur, ksv, bv, kv);
    } else out = tmp ? tmp : cg_copy(in), tmp = NULL;
done:
    if (tmp != out) cg_free(tmp);
    free(bv); free(kv); free(bh); free(kh);
    return out;
}
/* _imaging.c _resize(): the box arrives as single-precision floats. */
static CgImg *core_resize(const CgImg *in, int w, int h, int filter, const double dbox[4]) {
    float box[4] = { (float)dbox[0], (float)dbox[1], (float)dbox[2], (float)dbox[3] };
    if (w < 1 || h < 1 || box[0] < 0 || box[1] < 0 || box[2] > in->w || box[3] > in->h || box[2] - box[0] < 0 || box[3] - box[1] < 0) return NULL;
    if (box[0] - (int)box[0] == 0 && box[2] - box[0] == w && box[1] - (int)box[1] == 0 && box[3] - box[1] == h)
        return cg_crop(in, (int)box[0], (int)box[1], (int)box[2], (int)box[3]);
    const Filter *f = filter == CG_BILINEAR ? &F_BILINEAR : filter == CG_BICUBIC ? &F_BICUBIC : &F_LANCZOS;
    return resample(in, w, h, f, box);
}
CgImg *cg_resize(const CgImg *in, int w, int h, int filter, const double box_in[4]) {
    double box[4] = { 0, 0, in->w, in->h };
    if (box_in) memcpy(box, box_in, sizeof box);
    if (in->w == w && in->h == h && box[0] == 0 && box[1] == 0 && box[2] == in->w && box[3] == in->h) return cg_copy(in);
    if (in->mode != CG_RGBA) return core_resize(in, w, h, filter, box);
    CgImg *pre = cg_convert(in, CG_RGBa), *r = pre ? core_resize(pre, w, h, filter, box) : NULL, *out = r ? cg_convert(r, CG_RGBA) : NULL;
    cg_free(pre); cg_free(r);
    return out;
}
CgImg *cg_thumbnail(const CgImg *in, int limit, int filter) {
    int x = limit, y = limit;
    if (x >= in->w && y >= in->h) return cg_copy(in);
    double aspect = (double)in->w / in->h;
    if ((double)x / y >= aspect) {   /* round_aspect(y * aspect, key=lambda n: abs(aspect - n / y)) */
        double v = y * aspect; int lo = (int)floor(v), hi = (int)ceil(v);
        x = fabs(aspect - (double)hi / y) < fabs(aspect - (double)lo / y) ? hi : lo;
        if (x < 1) x = 1;
    } else {   /* key=lambda n: 0 if n == 0 else abs(aspect - x / n) */
        double v = x / aspect; int lo = (int)floor(v), hi = (int)ceil(v);
        double klo = lo == 0 ? 0 : fabs(aspect - (double)x / lo), khi = hi == 0 ? 0 : fabs(aspect - (double)x / hi);
        y = khi < klo ? hi : lo;
        if (y < 1) y = 1;
    }
    if (x == in->w && y == in->h) return cg_copy(in);
    return cg_resize(in, x, y, filter, NULL);   /* RGBA: the premultiplied resize drops reducing_gap */
}

/* ---- Geometry.c: ImagingGenericTransform with the bilinear filter (bilinear_filter32RGB over 4 bands) ---- */
static int ifloor(double v) { return v < 0.0 ? (int)floor(v) : (int)v; }
static int bilinear(uint8_t *out, const CgImg *im, double xin, double yin) {
    if (xin < 0.0 || xin >= im->w || yin < 0.0 || yin >= im->h) return 0;
    xin -= 0.5; yin -= 0.5;
    int x = ifloor(xin), y = ifloor(yin);
    double dx = xin - x, dy = yin - y;
    int yc = y < 0 ? 0 : y < im->h ? y : im->h - 1;
    int x0 = x < 0 ? 0 : x < im->w ? x : im->w - 1, x1 = x + 1 < 0 ? 0 : x + 1 < im->w ? x + 1 : im->w - 1;
    for (int b = 0; b < 4; b++) {
        const uint8_t *in = im->p + (size_t)yc * im->w * 4 + b;
        double v1 = in[x0 * 4] + (in[x1 * 4] - in[x0 * 4]) * dx, v2;
        if (y + 1 >= 0 && y + 1 < im->h) {
            in = im->p + (size_t)(y + 1) * im->w * 4 + b;
            v2 = in[x0 * 4] + (in[x1 * 4] - in[x0 * 4]) * dx;
        } else v2 = v1;
        v1 = v1 + (v2 - v1) * dy;
        out[b] = (uint8_t)v1;
    }
    return 1;
}
static CgImg *transform(const CgImg *src, int w, int h, const double *a, int quad) {
    CgImg *pre = cg_convert(src, CG_RGBa), *im = pre ? cg_new(CG_RGBa, w, h) : NULL, *out = NULL;
    if (!im) goto done;
    for (int y = 0; y < h; y++) {
        uint8_t *o = im->p + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, o += 4) {
            double xin = x + 0.5, yin = y + 0.5, xx, yy;
            if (quad) { xx = a[0] + a[1] * xin + a[2] * yin + a[3] * xin * yin; yy = a[4] + a[5] * xin + a[6] * yin + a[7] * xin * yin; }
            else { xx = a[0] * xin + a[1] * yin + a[2]; yy = a[3] * xin + a[4] * yin + a[5]; }
            if (!bilinear(o, pre, xx, yy)) memset(o, 0, 4);
        }
    }
    out = cg_convert(im, CG_RGBA);
done:
    cg_free(pre); cg_free(im);
    return out;
}
CgImg *cg_transform_affine(const CgImg *src, int w, int h, const double a[6]) { return transform(src, w, h, a, 0); }
CgImg *cg_transform_quad(const CgImg *src, int w, int h, const double q[8]) {
    /* Image.__transformer(): nw, sw, se, ne corners to the bilinear quad coefficients */
    double x0 = q[0], y0 = q[1], As = 1.0 / w, At = 1.0 / h;
    double a[8] = { x0, (q[6] - x0) * As, (q[2] - x0) * At, (q[4] - q[2] - q[6] + x0) * As * At,
                    y0, (q[7] - y0) * As, (q[3] - y0) * At, (q[5] - q[3] - q[7] + y0) * As * At };
    return transform(src, w, h, a, 1);
}

/* ---- AlphaComposite.c, in place over the clipped destination box ---- */
void cg_alpha_composite(CgImg *dst, const CgImg *src, int x, int y) {
    for (int sy = 0; sy < src->h; sy++) {
        int dy = y + sy;
        if (dy < 0 || dy >= dst->h) continue;
        for (int sx = 0; sx < src->w; sx++) {
            int dx = x + sx;
            if (dx < 0 || dx >= dst->w) continue;
            const uint8_t *s = cg_px(src, sx, sy); uint8_t *d = cg_px(dst, dx, dy);
            if (!s[3]) continue;
            uint32_t blend = d[3] * (255u - s[3]), outa255 = s[3] * 255u + blend;
            uint32_t coef1 = s[3] * 255u * 255u * (1u << 7) / outa255, coef2 = 255u * (1u << 7) - coef1;
            for (int c = 0; c < 3; c++) {
                uint32_t t = s[c] * coef1 + d[c] * coef2;
                d[c] = (uint8_t)(SHIFTFORDIV255(t + (0x80u << 7)) >> 7);
            }
            d[3] = (uint8_t)SHIFTFORDIV255(outa255 + 0x80);
        }
    }
}

CgImg *cg_multiply(const CgImg *a, const CgImg *b) {
    CgImg *im = cg_new(a->mode, a->w < b->w ? a->w : b->w, a->h < b->h ? a->h : b->h);
    if (!im) return NULL;
    int bpp = cg_bpp(a);
    for (int y = 0; y < im->h; y++) {
        const uint8_t *p = cg_px(a, 0, y), *q = cg_px(b, 0, y); uint8_t *o = cg_px(im, 0, y);
        for (int i = 0; i < im->w * bpp; i++) o[i] = (uint8_t)(p[i] * q[i] / 255);
    }
    return im;
}
void cg_invert(CgImg *im) { for (size_t i = 0; i < (size_t)im->w * im->h * cg_bpp(im); i++) im->p[i] = (uint8_t)(255 - im->p[i]); }
int cg_alpha_bbox(const CgImg *im, int box[4]) {
    int x0 = im->w, y0 = im->h, x1 = 0, y1 = 0;
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w; x++)
            if (cg_px(im, x, y)[3]) { if (x < x0) x0 = x; if (y < y0) y0 = y; if (x + 1 > x1) x1 = x + 1; if (y + 1 > y1) y1 = y + 1; }
    if (x1 <= x0) return 0;
    box[0] = x0; box[1] = y0; box[2] = x1; box[3] = y1;
    return 1;
}
void cg_point(CgImg *im, const uint8_t lut[4][256]) {
    for (size_t i = 0; i < (size_t)im->w * im->h; i++)
        for (int c = 0; c < 4; c++) im->p[i * 4 + c] = lut[c][im->p[i * 4 + c]];
}
void cg_flip(CgImg *im, int vertical) {
    int bpp = cg_bpp(im); uint8_t t[4];
    if (vertical) {
        size_t row = (size_t)im->w * bpp;
        uint8_t *tmp = malloc(row ? row : 1);
        if (!tmp) return;
        for (int y = 0; y < im->h / 2; y++) {
            memcpy(tmp, cg_px(im, 0, y), row); memcpy(cg_px(im, 0, y), cg_px(im, 0, im->h - 1 - y), row); memcpy(cg_px(im, 0, im->h - 1 - y), tmp, row);
        }
        free(tmp);
        return;
    }
    for (int y = 0; y < im->h; y++)
        for (int x = 0; x < im->w / 2; x++) {
            uint8_t *a = cg_px(im, x, y), *b = cg_px(im, im->w - 1 - x, y);
            memcpy(t, a, bpp); memcpy(a, b, bpp); memcpy(b, t, bpp);
        }
}
