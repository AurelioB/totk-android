/* The companion stage (setup_stage_companion, stage_companion.c): a C port of scripts/make_companion_assets.py +
   scripts/companion_decor.py + scripts/bflyt_render.py that writes the same <game>/companion tree, byte for byte where
   the arithmetic allows. Internal to android/setup/companion/. RomFS, Zstandard and SARC come from the shader seed
   stage's reader (seed/romfs_read.c, seed_int.h). */
#ifndef TOTK_CG_H
#define TOTK_CG_H
#include "seed_int.h"
#include <stddef.h>
#include <stdint.h>

/* The layout renderer and the resampling filters repeat CPython's / Pillow's double arithmetic step by step; a fused
   multiply-add (clang's default on arm64) would round differently and move pixels. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

/* ---- images (cg_image.c): the Pillow 12.3 operations the Python generator uses, ported from libImaging ---- */
enum { CG_L, CG_RGB, CG_RGBA, CG_RGBa };   /* RGB* keep Pillow's 4-byte pixels; RGBa = premultiplied alpha */
typedef struct { int mode, w, h; uint8_t *p; } CgImg;   /* row-major, 1 byte (L) or 4 bytes per pixel */
enum { CG_BILINEAR = 2, CG_BICUBIC = 3, CG_LANCZOS = 1 };   /* Image.Resampling values */
CgImg *cg_new(int mode, int w, int h);                   /* zero-filled; NULL on OOM */
CgImg *cg_new_fill(int mode, int w, int h, const uint8_t px[4]);
void cg_free(CgImg *);
CgImg *cg_copy(const CgImg *);
static inline int cg_bpp(const CgImg *im) { return im->mode == CG_L ? 1 : 4; }
static inline uint8_t *cg_px(const CgImg *im, int x, int y) { return im->p + ((size_t)y * im->w + x) * cg_bpp(im); }
CgImg *cg_crop(const CgImg *, int x0, int y0, int x1, int y1);   /* ImagingCrop: outside = 0 */
void cg_paste(CgImg *dst, const CgImg *src, int x, int y);       /* Image.paste without mask, same pixel size */
CgImg *cg_convert(const CgImg *, int mode);   /* RGBA<->RGBa, RGBA/RGB->L, RGB<->RGBA */
/* Image.resize(size, resample, box) (box NULL = whole image), including the RGBA premultiplication round trip. */
CgImg *cg_resize(const CgImg *, int w, int h, int filter, const double box[4]);
/* Image.thumbnail((limit, limit), resample) result; returns a copy when no resize is needed. */
CgImg *cg_thumbnail(const CgImg *, int limit, int filter);
/* Image.transform(size, AFFINE (6 values) | QUAD (8 values), data, BILINEAR) of an RGBA image. */
CgImg *cg_transform_affine(const CgImg *, int w, int h, const double a[6]);
CgImg *cg_transform_quad(const CgImg *, int w, int h, const double q[8]);
void cg_alpha_composite(CgImg *dst, const CgImg *src, int x, int y);   /* Image.alpha_composite(src, (x, y)), RGBA */
CgImg *cg_multiply(const CgImg *, const CgImg *);   /* ImageChops.multiply */
void cg_invert(CgImg *);                            /* ImageChops.invert, in place */
int cg_alpha_bbox(const CgImg *, int box[4]);       /* getchannel('A').getbbox(); 0 = empty */
void cg_point(CgImg *, const uint8_t lut[4][256]);  /* per-channel point() of an RGBA image */
void cg_flip(CgImg *, int vertical);                /* transpose FLIP_LEFT_RIGHT / FLIP_TOP_BOTTOM, in place */

/* ---- textures (cg_texture.c): BNTX, Tegra X1 block-linear, BC1-5/BC7 and ASTC LDR as texture2ddecoder 1.0.6 ---- */
typedef struct { const char *name; uint32_t fmt, w, h, block; uint64_t mip0; uint8_t sel[4]; uint16_t tile; } CgTex;
int cg_bntx(const uint8_t *, size_t, CgTex **out, size_t *count, SeedErr *);   /* in file order, names point into data */
const CgTex *cg_bntx_find(const CgTex *, size_t count, const char *name);     /* the last one of that name, as dict() */
CgImg *cg_tex_decode(const uint8_t *, size_t, const CgTex *, SeedErr *);     /* decode_texture(): RGBA */
void cg_bc1(const uint8_t *, long w, long h, uint32_t *bgra);
void cg_bc3(const uint8_t *, long w, long h, uint32_t *bgra);
void cg_bc4(const uint8_t *, long w, long h, uint32_t *bgra);
void cg_bc5(const uint8_t *, long w, long h, uint32_t *bgra);
void cg_bc7(const uint8_t *, long w, long h, uint32_t *bgra);
void cg_astc(const uint8_t *, long w, long h, int bw, int bh, uint32_t *bgra);

/* ---- WebP (cg_webp.c, libwebp 1.6.0 as Pillow 12.3 drives it) ---- */
int cg_webp_encode(const CgImg *, int quality, int method, uint8_t **out, size_t *n);   /* malloc'd; RGB or RGBA */
CgImg *cg_webp_decode(const uint8_t *, size_t);   /* Image.open(): RGBA when the file has alpha, else RGB */

/* ---- data formats (cg_formats.c) ---- */
enum { CGV_NULL, CGV_BOOL, CGV_INT, CGV_UINT, CGV_FLOAT, CGV_STR, CGV_ARR, CGV_DICT };
typedef struct CgVal { int type; int64_t i; uint64_t u; double f; const char *s;   /* s points into the BYML */
                       size_t n; struct CgVal *items; const char **keys; } CgVal;
CgVal *cg_byml(const uint8_t *, size_t, SeedErr *);   /* the little-endian BYML subset of byml() */
void cg_val_free(CgVal *);
const CgVal *cg_get(const CgVal *dict, const char *key);   /* NULL when absent or not a dict */
int cg_val_str(const CgVal *, char *out, size_t cap);      /* str(value) for ints and strings; -1 otherwise */
typedef struct { char *label; char *text; } CgMsg;         /* UTF-8, NUL-terminated */
int cg_msbt(const uint8_t *, size_t, CgMsg **out, size_t *count, SeedErr *);   /* msbt(): dict order and values */
void cg_msbt_free(CgMsg *, size_t);
const char *cg_msg_get(const CgMsg *, size_t, const char *label);
/* json.dumps(ensure_ascii=False) pieces */
void cg_json_str(Buf *, const char *utf8);
void cg_json_float(Buf *, double);   /* float.__repr__ */
void cg_json_val(Buf *, const CgVal *);   /* scalars only (markers) */
int cg_font(const uint8_t *, size_t, uint8_t **out, size_t *n, int *otf, SeedErr *);   /* extract_font() decoding */

/* ---- output (stage_companion.c) ---- */
typedef struct CgOut CgOut;
/* save_webp(image, out/rel, quality): method 3, exact; counts the bytes toward the 120 MiB limit. */
int cg_save_webp(CgOut *, const CgImg *, const char *rel, int quality);
SeedErr *cg_err(CgOut *);
int cg_stopped(CgOut *);

/* ---- decor (cg_layout.c): companion_decor.extract_decor() with the bflyt_render.Renderer it needs ---- */
typedef struct {
    const uint8_t *combined; size_t combined_n; const CgTex *tex; size_t ntex;   /* timg/__Combined.bntx */
    const SarcMember *archive; size_t narchive;                                   /* Common.Product.140 blarc */
} CgDecorIn;
/* Writes decor/<name>.webp; appends the JSON values of index.json's decor, palette, pageIcons and decorSources. */
int cg_decor(CgOut *, const CgDecorIn *, Buf *decor, Buf *palette, Buf *page_icons, Buf *sources);
#endif
