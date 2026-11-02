/* companion_decor.extract_decor() and the parts of bflyt_render.py it uses (FLYT parsing, the static pane / window /
   part compositor), ported step by step: same double arithmetic and evaluation order, Python's round-half-even, and the
   Pillow operations of cg_image.c. Animations (FLAN) only serve the optional review renders and are not ported. */
#include "cg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG2RAD (M_PI / 180.0)   /* math.radians multiplies by this constant */
static int pyround(double v) { return (int)rint(v); }   /* round(): half to even */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static double lef(const uint8_t *p) { uint32_t u = le32(p); float f; memcpy(&f, &u, 4); return f; }

/* ---- FLYT ---- */
typedef struct Pane Pane;
typedef struct { char *name; int usage, basic; Pane *replacement; uint8_t info[52]; size_t ninfo; } Override;
struct Pane {
    char tag[5]; char *name; int flags, origin, alpha;
    double tr[3], rot[3], scale[2], size[2];
    Pane **children; size_t nchildren, capchildren;
    int material; uint8_t colors[4][4]; double (*coords)[4][2]; size_t ncoords;
    int inflation[4], frame_size[4], window_flags; int (*frames)[2]; size_t nframes;
    char *part; double magnify[2];
    Override *overrides; size_t noverrides;
};
typedef struct { char *name; uint32_t flags; uint8_t black[4], white[4]; size_t nmaps; const char *map_tex[3]; int map_ws[3], map_wt[3];
                 size_t nsrt; double srt[3][5]; } Mat;
typedef struct {
    char *name; double size[2]; int centered;
    char **textures; size_t ntextures; Mat *mats; size_t nmats;
    Pane **roots; size_t nroots, caproots; Pane **all; size_t nall, capall;   /* all: the panes dict, in parse order */
} Layout;

typedef struct { const uint8_t *d; size_t n; int bad; } Rd;
static const uint8_t *rd_at(Rd *r, size_t at, size_t len) { if (at > r->n || r->n - at < len) { r->bad = 1; return NULL; } return r->d + at; }
static uint32_t rd_u32(Rd *r, size_t at) { const uint8_t *p = rd_at(r, at, 4); return p ? le32(p) : 0; }
static uint16_t rd_u16(Rd *r, size_t at) { const uint8_t *p = rd_at(r, at, 2); return p ? le16(p) : 0; }
static uint8_t rd_u8(Rd *r, size_t at) { const uint8_t *p = rd_at(r, at, 1); return p ? *p : 0; }
static double rd_f(Rd *r, size_t at) { const uint8_t *p = rd_at(r, at, 4); return p ? lef(p) : 0; }
/* string(data, at, size): up to the first NUL (or the size / end), UTF-8 */
static char *rd_str(Rd *r, size_t at, size_t size) {
    if (at > r->n) { r->bad = 1; return NULL; }
    size_t end = size && at + size < r->n ? at + size : r->n;
    const uint8_t *nul = memchr(r->d + at, 0, end - at);
    size_t len = nul ? (size_t)(nul - (r->d + at)) : end - at;
    char *s = malloc(len + 1);
    if (!s) { r->bad = 1; return NULL; }
    memcpy(s, r->d + at, len); s[len] = 0;
    return s;
}
static void strip_suffix(char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n >= m && !strcmp(s + n - m, suffix)) s[n - m] = 0;
}
static int push(void ***v, size_t *n, size_t *cap, void *item) {
    if (*n == *cap) { void **g = realloc(*v, (*cap = *cap ? *cap * 2 : 8) * sizeof **v); if (!g) return -1; *v = g; }
    (*v)[(*n)++] = item;
    return 0;
}

static void pane_free(Pane *p) {
    if (!p) return;
    free(p->name); free(p->children); free(p->coords); free(p->frames); free(p->part);
    for (size_t i = 0; i < p->noverrides; i++) { free(p->overrides[i].name); pane_free(p->overrides[i].replacement); }
    free(p->overrides); free(p);
}
static void picture_info(Pane *p, Rd *r, size_t at) {
    for (int i = 0; i < 4; i++) for (int c = 0; c < 4; c++) p->colors[i][c] = rd_u8(r, at + i * 4 + c);
    p->material = rd_u16(r, at + 16);
    p->ncoords = rd_u8(r, at + 18);
    if (!(p->coords = calloc(p->ncoords ? p->ncoords : 1, sizeof *p->coords))) { r->bad = 1; return; }
    for (size_t i = 0; i < p->ncoords; i++)
        for (int j = 0; j < 4; j++) { p->coords[i][j][0] = rd_f(r, at + 20 + i * 32 + j * 8); p->coords[i][j][1] = rd_f(r, at + 24 + i * 32 + j * 8); }
}
static Pane *parse_pane(const uint8_t *d, size_t n) {
    Rd r = { d, n, 0 };
    Pane *p = calloc(1, sizeof *p);
    if (!p || !rd_at(&r, 0, 84)) { free(p); return NULL; }
    memcpy(p->tag, d, 4);
    p->name = rd_str(&r, 12, 24); p->flags = d[8]; p->origin = d[9]; p->alpha = d[10];
    for (int i = 0; i < 3; i++) { p->tr[i] = rd_f(&r, 44 + i * 4); p->rot[i] = rd_f(&r, 56 + i * 4); }
    for (int i = 0; i < 2; i++) { p->scale[i] = rd_f(&r, 68 + i * 4); p->size[i] = rd_f(&r, 76 + i * 4); }
    p->material = -1;
    memset(p->colors, 255, sizeof p->colors);
    p->magnify[0] = p->magnify[1] = 1;
    if (!memcmp(p->tag, "pic1", 4)) picture_info(p, &r, 84);
    else if (!memcmp(p->tag, "txt1", 4)) p->material = rd_u16(&r, 88);
    else if (!memcmp(p->tag, "wnd1", 4)) {
        for (int i = 0; i < 4; i++) { p->inflation[i] = (int16_t)rd_u16(&r, 84 + i * 2); p->frame_size[i] = rd_u16(&r, 92 + i * 2); }
        p->window_flags = rd_u8(&r, 101);
        uint32_t content = rd_u32(&r, 104), table = rd_u32(&r, 108);
        picture_info(p, &r, content);
        p->nframes = rd_u8(&r, 100);
        if (!(p->frames = calloc(p->nframes ? p->nframes : 1, sizeof *p->frames))) r.bad = 1;
        for (size_t i = 0; i < p->nframes && !r.bad; i++) {
            uint32_t at = rd_u32(&r, table + i * 4);
            p->frames[i][0] = rd_u16(&r, at); p->frames[i][1] = rd_u8(&r, at + 2);
        }
    } else if (!memcmp(p->tag, "prt1", 4)) {
        uint32_t count = rd_u32(&r, 84);
        p->magnify[0] = rd_f(&r, 88); p->magnify[1] = rd_f(&r, 92);
        if (count > n / 40) r.bad = 1;
        else {
            p->part = rd_str(&r, 96 + (size_t)count * 40, 0);
            if (p->part) strip_suffix(p->part, ".bflyt");
            if (!(p->overrides = calloc(count ? count : 1, sizeof *p->overrides))) r.bad = 1;
            for (uint32_t i = 0; i < count && !r.bad; i++) {
                size_t at = 96 + (size_t)i * 40;
                char *name = rd_str(&r, at, 24);
                int usage = rd_u8(&r, at + 24), basic = rd_u8(&r, at + 25);
                uint32_t prop = rd_u32(&r, at + 28), info = rd_u32(&r, at + 36);
                if (!name || !*name) { free(name); continue; }
                Override o = { name, usage, basic, NULL, { 0 }, 0 };
                if (prop) { if (prop >= n || !(o.replacement = parse_pane(d + prop, n - prop))) r.bad = 1; }
                if (info && info < n) { o.ninfo = n - info < 52 ? n - info : 52; memcpy(o.info, d + info, o.ninfo); }
                size_t j = 0;   /* dict: a repeated name replaces the entry in place */
                while (j < p->noverrides && strcmp(p->overrides[j].name, name)) j++;
                if (j < p->noverrides) { free(p->overrides[j].name); pane_free(p->overrides[j].replacement); }
                else p->noverrides++;
                p->overrides[j] = o;
            }
        }
    }
    if (r.bad || !p->name) { pane_free(p); return NULL; }
    return p;
}
static void layout_free(Layout *l) {
    if (!l) return;
    free(l->name);
    for (size_t i = 0; i < l->ntextures; i++) free(l->textures[i]);
    free(l->textures);
    for (size_t i = 0; i < l->nmats; i++) free(l->mats[i].name);
    free(l->mats);
    for (size_t i = 0; i < l->nall; i++) pane_free(l->all[i]);
    free(l->all); free(l->roots); free(l);
}
static Layout *parse_layout(const char *name, const uint8_t *d, size_t n) {
    Layout *l = calloc(1, sizeof *l);
    Pane ***stack[64]; size_t *stackn[64], *stackcap[64]; int depth = 0; Pane *last = NULL;   /* the open child lists */
    if (!l || !(l->name = strdup(name))) goto bad;
    l->size[0] = 1920; l->size[1] = 1080; l->centered = 1;
    if (n < 20 || (memcmp(d, "FLYT", 4) && memcmp(d, "FLAN", 4)) || d[4] != 0xff || d[5] != 0xfe || le32(d + 12) != n) goto bad;
    stack[0] = &l->roots; stackn[0] = &l->nroots; stackcap[0] = &l->caproots;
    size_t pos = le16(d + 6);
    for (unsigned s = 0, ns = le16(d + 16); s < ns; s++) {
        if (pos > n || n - pos < 8) goto bad;
        size_t size = le32(d + pos + 4);
        if (size < 8 || size > n - pos) goto bad;
        const uint8_t *b = d + pos; Rd r = { b, size, 0 };
        if (!memcmp(b, "lyt1", 4)) {
            l->centered = rd_u8(&r, 8) != 0; l->size[0] = rd_f(&r, 12); l->size[1] = rd_f(&r, 16);
        } else if (!memcmp(b, "txl1", 4)) {
            size_t cnt = rd_u16(&r, 8);
            if (!(l->textures = calloc(cnt ? cnt : 1, sizeof *l->textures))) goto bad;
            for (size_t i = 0; i < cnt; i++) {
                if (!(l->textures[i] = rd_str(&r, 12 + rd_u32(&r, 12 + i * 4), 0))) goto bad;
                l->ntextures = i + 1;
                strip_suffix(l->textures[i], ".bntx");
            }
        } else if (!memcmp(b, "mat1", 4)) {
            size_t cnt = rd_u16(&r, 8);
            if (!(l->mats = calloc(cnt ? cnt : 1, sizeof *l->mats))) goto bad;
            for (size_t i = 0; i < cnt; i++) {
                Mat *m = &l->mats[i]; size_t at = rd_u32(&r, 12 + i * 4);
                l->nmats = i + 1;
                if (!(m->name = rd_str(&r, at, 28))) goto bad;
                m->flags = rd_u32(&r, at + 28);
                for (int c = 0; c < 4; c++) { m->black[c] = rd_u8(&r, at + 36 + c); m->white[c] = rd_u8(&r, at + 40 + c); }
                size_t p = at + 44;
                m->nmaps = m->flags & 3;
                for (size_t k = 0; k < m->nmaps; k++, p += 4) {
                    unsigned t = rd_u16(&r, p);
                    if (t >= l->ntextures) goto bad;
                    m->map_tex[k] = l->textures[t]; m->map_ws[k] = rd_u8(&r, p + 2) & 3; m->map_wt[k] = rd_u8(&r, p + 3) & 3;
                }
                m->nsrt = (m->flags >> 2) & 3;
                for (size_t k = 0; k < m->nsrt; k++, p += 20) for (int j = 0; j < 5; j++) m->srt[k][j] = rd_f(&r, p + j * 4);
            }
        } else if (!memcmp(b, "pan1", 4) || !memcmp(b, "pic1", 4) || !memcmp(b, "wnd1", 4) || !memcmp(b, "txt1", 4) ||
                   !memcmp(b, "prt1", 4) || !memcmp(b, "bnd1", 4) || !memcmp(b, "scr1", 4) || !memcmp(b, "ali1", 4) || !memcmp(b, "cpt1", 4)) {
            if (!(last = parse_pane(b, size)) || push((void ***)&l->all, &l->nall, &l->capall, last)) { pane_free(last); goto bad; }
            if (push((void ***)stack[depth], stackn[depth], stackcap[depth], last)) goto bad;
        } else if (!memcmp(b, "pas1", 4)) {
            if (!last || depth == 63) goto bad;
            depth++;
            stack[depth] = &last->children; stackn[depth] = &last->nchildren; stackcap[depth] = &last->capchildren;
        } else if (!memcmp(b, "pae1", 4)) {
            if (!depth) goto bad;
            depth--;
        }
        if (r.bad) goto bad;
        pos += size;
    }
    return l;
bad:
    layout_free(l);
    return NULL;
}
static const Pane *layout_pane(const Layout *l, const char *name) {
    for (size_t i = l->nall; i-- > 0;) if (!strcmp(l->all[i]->name, name)) return l->all[i];
    return NULL;
}
static const Mat *layout_mat(const Layout *l, const char *name) {
    for (size_t i = 0; i < l->nmats; i++) if (!strcmp(l->mats[i].name, name)) return &l->mats[i];
    return NULL;
}

/* ---- Renderer ---- */
typedef struct { char *name; CgImg *img; } TexCache;
typedef struct {
    CgOut *out; const CgDecorIn *in; SeedErr *err;
    Layout **layouts; size_t nlayouts, caplayouts;
    TexCache *tex; size_t ntex, captex;
    int failed;
} R;
static const SarcMember *archive_get(const R *r, const char *name) {
    for (size_t i = 0; i < r->in->narchive; i++) if (!strcmp(r->in->archive[i].name, name)) return &r->in->archive[i];
    return NULL;
}
static Layout *get_layout(R *r, const char *name) {
    for (size_t i = 0; i < r->nlayouts; i++) if (!strcmp(r->layouts[i]->name, name)) return r->layouts[i];
    char key[256];
    snprintf(key, sizeof key, "blyt/%s.bflyt", name);
    const SarcMember *m = archive_get(r, key);
    Layout *l = m ? parse_layout(name, m->data, m->size) : NULL;
    if (!l || push((void ***)&r->layouts, &r->nlayouts, &r->caplayouts, l)) {
        layout_free(l);
        if (!r->failed) seed_err(r->err, "internal", "Companion decor: cannot read layout %s", name);
        r->failed = 1;
        return NULL;
    }
    return l;
}
/* Renderer.texture(): framebuffer captures (FBLayout*) and names missing from the archive are None. */
static const CgImg *get_texture(R *r, const char *name) {
    if (!strncmp(name, "FBLayout", 8)) return NULL;
    for (size_t i = 0; i < r->ntex; i++) if (!strcmp(r->tex[i].name, name)) return r->tex[i].img;
    const CgTex *t = cg_bntx_find(r->in->tex, r->in->ntex, name);
    CgImg *img = t ? cg_tex_decode(r->in->combined, r->in->combined_n, t, r->err) : NULL;
    if (t && !img) { r->failed = 1; return NULL; }
    if (r->ntex == r->captex) {
        TexCache *g = realloc(r->tex, (r->captex = r->captex ? r->captex * 2 : 32) * sizeof *g);
        if (!g) { cg_free(img); r->failed = 1; seed_err(r->err, "internal", "Out of memory"); return NULL; }
        r->tex = g;
    }
    char *copy = strdup(name);
    if (!copy) { cg_free(img); r->failed = 1; seed_err(r->err, "internal", "Out of memory"); return NULL; }
    r->tex[r->ntex++] = (TexCache){ copy, img };
    return img;
}

static void mat_mul(const double a[6], const double b[6], double o[6]) {
    double t[6] = { a[0]*b[0]+a[1]*b[3], a[0]*b[1]+a[1]*b[4], a[0]*b[2]+a[1]*b[5]+a[2],
                    a[3]*b[0]+a[4]*b[3], a[3]*b[1]+a[4]*b[4], a[3]*b[2]+a[4]*b[5]+a[5] };
    memcpy(o, t, sizeof t);
}
static void apply_pt(const double m[6], double x, double y, double *ox, double *oy) { *ox = m[0]*x + m[1]*y + m[2]; *oy = m[3]*x + m[4]*y + m[5]; }
static int rect(const Pane *p, double out[4]) {
    double w = p->size[0], h = p->size[1];
    int ox = p->origin & 3, oy = (p->origin >> 2) & 3;
    if (ox > 2 || oy > 2) return -1;   /* the tuple index would raise */
    double l3[3] = { -w/2, 0, -w }, t3[3] = { h/2, 0, h };
    out[0] = l3[ox]; out[1] = t3[oy]; out[2] = w; out[3] = h;
    return 0;
}
static int pane_transform(const Pane *p, const Pane *parent, double o[6]) {
    double x = p->tr[0], y = p->tr[1];
    if (parent) {
        double pr[4];
        int px = (p->origin >> 4) & 3, py = (p->origin >> 6) & 3;
        if (rect(parent, pr) || px > 2 || py > 2) return -1;
        double xs[3] = { 0, pr[0], pr[0] + pr[2] }, ys[3] = { 0, pr[1], pr[1] - pr[3] };
        x += xs[px]; y += ys[py];
    }
    double rx = p->rot[0] * DEG2RAD, ry = p->rot[1] * DEG2RAD, rz = p->rot[2] * DEG2RAD, sx = p->scale[0], sy = p->scale[1];
    double c = cos(rz), s = sin(rz);
    o[0] = c*sx*cos(ry); o[1] = -s*sy*cos(rx); o[2] = x; o[3] = s*sx*cos(ry); o[4] = c*sy*cos(rx); o[5] = y;
    return 0;
}

static const uint8_t WHITE4[4][4] = { { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 255 } };
static const double UV[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };

/* gradient(): a 2x2 image of the vertex colours, bilinearly resized from its pixel centres */
static CgImg *gradient(const uint8_t colors[4][4], int w, int h) {
    CgImg *g = cg_new(CG_RGBA, 2, 2);
    if (!g) return NULL;
    memcpy(g->p, colors, 16);
    double box[4] = { .5, .5, 1.5, 1.5 };
    CgImg *out = cg_resize(g, w, h, CG_BILINEAR, box);
    cg_free(g);
    return out;
}
/* texture_sample(): build the UV cells the quad touches (clamp = stretched edge texels, mirror = flipped), then QUAD */
static CgImg *texture_sample(const CgImg *src, const double coords_in[4][2], const double srt[5], int ws, int wt, int sw, int sh) {
    double tx = srt[0], ty = srt[1], rotate = srt[2], sx = srt[3], sy = srt[4];
    double c = cos(rotate * DEG2RAD), s = sin(rotate * DEG2RAD), co[4][2];
    for (int i = 0; i < 4; i++) {
        double u = coords_in[i][0], v = coords_in[i][1];
        co[i][0] = (c*(u-.5)+s*(v-.5))/(sx != 0 ? sx : 1)+.5-tx;
        co[i][1] = (-s*(u-.5)+c*(v-.5))/(sy != 0 ? sy : 1)+.5-ty;
    }
    double minu = co[0][0], maxu = co[0][0], minv = co[0][1], maxv = co[0][1];
    for (int i = 1; i < 4; i++) {
        if (co[i][0] < minu) minu = co[i][0];
        if (co[i][0] > maxu) maxu = co[i][0];
        if (co[i][1] < minv) minv = co[i][1];
        if (co[i][1] > maxv) maxv = co[i][1];
    }
    long lo_x = (long)floor(minu), lo_y = (long)floor(minv), hi_x = (long)ceil(maxu), hi_y = (long)ceil(maxv);
    if (hi_x < lo_x + 1) hi_x = lo_x + 1;
    if (hi_y < lo_y + 1) hi_y = lo_y + 1;
    CgImg *scaled = NULL, *sheet = NULL, *out = NULL;
    const CgImg *img = src;
    int w = src->w, h = src->h;
    double cells = (double)(hi_x - lo_x) * (double)(hi_y - lo_y) * w * h;
    if (cells > 32000000) {
        double factor = sqrt(32000000 / cells);
        if (factor > 1) factor = 1;
        int nw = pyround(w * factor), nh = pyround(h * factor);
        if (!(scaled = cg_resize(src, nw < 1 ? 1 : nw, nh < 1 ? 1 : nh, CG_BILINEAR, NULL))) return NULL;
        img = scaled; w = img->w; h = img->h;
    }
    if ((hi_x - lo_x) * (double)w > 65535 || (hi_y - lo_y) * (double)h > 65535 ||
        !(sheet = cg_new(CG_RGBA, (int)((hi_x - lo_x) * w), (int)((hi_y - lo_y) * h)))) goto done;
    for (long cy = lo_y; cy < hi_y; cy++)
        for (long cx = lo_x; cx < hi_x; cx++) {
            CgImg *tile = cg_copy(img), *t2;
            if (!tile) goto done;
            if (ws == 0 && cx != 0) {
                CgImg *col = cg_crop(tile, cx < 0 ? 0 : w - 1, 0, cx < 0 ? 1 : w, h);
                t2 = col ? cg_resize(col, w, h, CG_BICUBIC, NULL) : NULL;   /* resize() default filter */
                cg_free(col); cg_free(tile); if (!(tile = t2)) goto done;
            } else if (ws == 2 && ((cx % 2) + 2) % 2) cg_flip(tile, 0);
            if (wt == 0 && cy != 0) {
                CgImg *row = cg_crop(tile, 0, cy < 0 ? 0 : h - 1, w, cy < 0 ? 1 : h);
                t2 = row ? cg_resize(row, w, h, CG_BICUBIC, NULL) : NULL;
                cg_free(row); cg_free(tile); if (!(tile = t2)) goto done;
            } else if (wt == 2 && ((cy % 2) + 2) % 2) cg_flip(tile, 1);
            cg_paste(sheet, tile, (int)((cx - lo_x) * w), (int)((cy - lo_y) * h));
            cg_free(tile);
        }
    static const int order[4] = { 0, 2, 3, 1 };
    double quad[8];
    for (int i = 0; i < 4; i++) { quad[i * 2] = (co[order[i]][0] - lo_x) * w; quad[i * 2 + 1] = (co[order[i]][1] - lo_y) * h; }
    out = cg_transform_quad(sheet, sw, sh, quad);
done:
    cg_free(scaled); cg_free(sheet);
    return out;
}
/* Renderer.surface(): map textures multiplied, lerp(black, white, texel), vertex colours, pane alpha. NULL + *missing
   when a map is a capture texture. */
static CgImg *surface(R *r, const Layout *l, int material, const uint8_t colors[4][4], const double (*coords)[4][2], size_t ncoords,
                      int sw, int sh, int alpha, int *missing) {
    const Mat *m = &l->mats[material];
    CgImg *image = NULL;
    *missing = 0;
    for (size_t i = 0; i < m->nmaps; i++) {
        const CgImg *tex = get_texture(r, m->map_tex[i]);
        if (!tex) { cg_free(image); *missing = !r->failed; return NULL; }
        static const double ident[5] = { 0, 0, 0, 1, 1 };
        const double (*uv)[2] = ncoords ? coords[i < ncoords ? i : ncoords - 1] : UV;
        CgImg *layer = texture_sample(tex, uv, i < m->nsrt ? m->srt[i] : ident, m->map_ws[i], m->map_wt[i], sw, sh);
        if (!layer) { cg_free(image); return NULL; }
        if (!image) image = layer;
        else { CgImg *p = cg_multiply(image, layer); cg_free(image); cg_free(layer); if (!(image = p)) return NULL; }
    }
    if (!image && !(image = cg_new_fill(CG_RGBA, sw, sh, (const uint8_t[4]){ 255, 255, 255, 255 }))) return NULL;
    uint8_t lut[4][256];
    for (int c = 0; c < 4; c++)
        for (int v = 0; v < 256; v++) lut[c][v] = (uint8_t)pyround(m->black[c] + (m->white[c] - m->black[c]) * v / 255.0);
    cg_point(image, lut);
    if (memcmp(colors, WHITE4, 16)) {
        CgImg *g = gradient(colors, sw, sh), *p = g ? cg_multiply(image, g) : NULL;
        cg_free(g); cg_free(image);
        if (!(image = p)) return NULL;
    }
    if (alpha != 255)
        for (size_t i = 0; i < (size_t)sw * sh; i++) image->p[i * 4 + 3] = (uint8_t)pyround(image->p[i * 4 + 3] * alpha / 255.0);
    return image;
}
static int draw_quad(R *r, CgImg *canvas, const Layout *l, int material, const uint8_t colors[4][4], const double (*coords)[4][2],
                     size_t ncoords, const double b[4], const double m[6], int alpha) {
    if (material < 0 || (size_t)material >= l->nmats || alpha <= 0) return 0;
    double left = b[0], top = b[1], width = b[2], height = b[3];
    if (width <= 0 || height <= 0) return 0;
    double fw = ceil(width*hypot(m[0], m[3])), fh = ceil(height*hypot(m[1], m[4]));
    int sw = fw < 1 ? 1 : fw > canvas->w * 2 ? canvas->w * 2 : (int)fw;
    int sh = fh < 1 ? 1 : fh > canvas->h * 2 ? canvas->h * 2 : (int)fh;
    double cx[4], cy[4], pts[4][2] = { { left, top }, { left+width, top }, { left, top-height }, { left+width, top-height } };
    for (int i = 0; i < 4; i++) apply_pt(m, pts[i][0], pts[i][1], &cx[i], &cy[i]);
    double mnx = cx[0], mxx = cx[0], mny = cy[0], mxy = cy[0];
    for (int i = 1; i < 4; i++) {
        if (cx[i] < mnx) mnx = cx[i];
        if (cx[i] > mxx) mxx = cx[i];
        if (cy[i] < mny) mny = cy[i];
        if (cy[i] > mxy) mxy = cy[i];
    }
    double bx0 = floor(mnx), by0 = floor(mny), bx1 = ceil(mxx), by1 = ceil(mxy);
    int x0 = bx0 < 0 ? 0 : (int)bx0, y0 = by0 < 0 ? 0 : (int)by0;
    int x1 = bx1 > canvas->w ? canvas->w : (int)bx1, y1 = by1 > canvas->h ? canvas->h : (int)by1;
    if (x1 <= x0 || y1 <= y0) return 0;
    int missing;
    CgImg *image = surface(r, l, material, colors, coords, ncoords, sw, sh, alpha, &missing);
    if (!image) {
        if (missing) return 0;
        if (!r->failed) seed_err(r->err, "internal", "Out of memory rendering companion decor");
        return -1;
    }
    double a = m[0], bb = m[1], c = m[2], d = m[3], e = m[4], f = m[5], det = a*e-bb*d;
    if (fabs(det) < 1e-9) { cg_free(image); return 0; }
    double x = x0, y = y0;
    double aff[6] = { e/det*sw/width, -bb/det*sw/width, ((e*(x-c)-bb*(y-f))/det-left)*sw/width,
                      d/det*sh/height, -a/det*sh/height, (top-(-d*(x-c)+a*(y-f))/det)*sh/height };
    CgImg *layer = cg_transform_affine(image, x1 - x0, y1 - y0, aff);
    cg_free(image);
    if (!layer) { seed_err(r->err, "internal", "Out of memory rendering companion decor"); return -1; }
    cg_alpha_composite(canvas, layer, x0, y0);
    cg_free(layer);
    return 0;
}
static int draw_window(R *r, CgImg *canvas, const Layout *l, const Pane *p, const double m[6], int alpha) {
    double rc[4];
    if (rect(p, rc)) return -1;
    double left = rc[0], top = rc[1], width = rc[2], height = rc[3];
    double il = p->inflation[0], ir = p->inflation[1], it = p->inflation[2], ib = p->inflation[3];
    double fl = p->frame_size[0], fr = p->frame_size[1], ft = p->frame_size[2], fb = p->frame_size[3];
    double outer[4] = { left-il, top+it, width+il+ir, height+it+ib };
    int horizontal = ((p->window_flags >> 2) & 3) == 2;
    if (!(p->window_flags & 16) && !horizontal && draw_quad(r, canvas, l, p->material, p->colors, p->coords, p->ncoords, rc, m, alpha)) return -1;
    if (!p->nframes) return 0;
    int mi = p->frames[0][0];
    if (mi < 0 || (size_t)mi >= l->nmats) return -1;
    const Mat *mat = &l->mats[mi];
    const CgImg *tex = mat->nmaps ? get_texture(r, mat->map_tex[0]) : NULL;
    if (!tex) return r->failed ? -1 : 0;
    if (fl > outer[2]/2) fl = outer[2]/2;
    if (fr > outer[2]/2) fr = outer[2]/2;
    if (ft > outer[3]/2) ft = outer[3]/2;
    if (fb > outer[3]/2) fb = outer[3]/2;
    double xs[4] = { outer[0], outer[0]+fl, outer[0]+outer[2]-fr, outer[0]+outer[2] };
    double ys[4] = { outer[1], outer[1]-ft, outer[1]-outer[3]+fb, outer[1]-outer[3] };
    static const int cells8[8][2] = { {0,0},{2,0},{0,2},{2,2},{1,0},{1,2},{0,1},{2,1} }, cells3[3][2] = { {0,0},{2,0},{1,0} };
    int ncells = 8; const int (*cells)[2] = cells8;
    if (horizontal) { ys[1] = outer[1]-outer[3]; ncells = 3; cells = cells3; }
    static const double span[3][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 } };
    size_t nf = p->nframes;
    for (int i = 0; i < ncells; i++) {
        int cx = cells[i][0], cy = cells[i][1];
        size_t frame = (size_t)i < nf - 1 ? (size_t)i : nf - 1;
        if (nf == 4 && i >= 4) frame = (size_t)((const int[]){ 0, 2, 0, 1 })[i - 4];
        int index = p->frames[frame][0], flip = p->frames[frame][1];
        double uv[4][2];
        if (nf == 1 || (nf == 4 && i >= 4)) {
            double u0 = span[cx][0], u1 = span[cx][1], v0 = horizontal ? 0 : span[cy][0], v1 = horizontal ? 1 : span[cy][1];
            if (nf == 4) { if (cx != 1) { u0 = 0; u1 = 1; } if (cy != 1) { v0 = 0; v1 = 1; } }
            double t[4][2] = { { u0, v0 }, { u1, v0 }, { u0, v1 }, { u1, v1 } };
            memcpy(uv, t, sizeof uv);
        } else memcpy(uv, UV, sizeof uv);
        if (flip == 1) for (int k = 0; k < 4; k++) uv[k][0] = 1 - uv[k][0];
        else if (flip == 2) for (int k = 0; k < 4; k++) uv[k][1] = 1 - uv[k][1];
        else if (flip >= 3 && flip <= 5)
            for (int n = 0; n < flip - 2; n++) for (int k = 0; k < 4; k++) { double u = uv[k][0]; uv[k][0] = uv[k][1]; uv[k][1] = 1 - u; }
        double bounds[4] = { xs[cx], ys[cy], xs[cx+1]-xs[cx], ys[cy]-ys[cy+1] };
        if (draw_quad(r, canvas, l, index, (p->window_flags & 2) ? p->colors : WHITE4, (const double (*)[4][2])&uv, 1, bounds, m, alpha)) return -1;
    }
    return 0;
}

typedef struct { const char *names[32]; int n; } Ancestry;
static int walk(R *r, CgImg *canvas, Layout *cur, const Pane *node, const double matrix[6], const Pane *parent, int alpha,
                const Ancestry *anc, const Pane *ov_owner) {
    Pane tmp;
    if (ov_owner) {
        for (size_t i = ov_owner->noverrides; i-- > 0;) {
            const Override *o = &ov_owner->overrides[i];
            if (strcmp(o->name, node->name)) continue;
            if (o->replacement) {   /* replace(replacement, children=node.children) */
                tmp = *o->replacement; tmp.children = node->children; tmp.nchildren = node->nchildren; node = &tmp;
            } else if (o->ninfo && o->basic) {
                if (o->ninfo < 52) return -1;
                tmp = *node;
                if (o->basic & 1) for (int k = 0; k < 3; k++) tmp.tr[k] = lef(o->info + 8 + k * 4);
                if (o->basic & 2) for (int k = 0; k < 3; k++) tmp.rot[k] = lef(o->info + 20 + k * 4);
                if (o->basic & 4) for (int k = 0; k < 2; k++) tmp.scale[k] = lef(o->info + 32 + k * 4);
                if (o->basic & 8) for (int k = 0; k < 2; k++) tmp.size[k] = lef(o->info + 40 + k * 4);
                if (o->basic & 16) tmp.alpha = o->info[48];
                node = &tmp;
            }
            break;
        }
    }
    if (!(node->flags & 1)) return 0;
    double t[6], m[6];
    if (pane_transform(node, parent, t)) return -1;
    mat_mul(matrix, t, m);
    int ga = pyround(alpha * node->alpha / 255.0);
    if (!memcmp(node->tag, "pic1", 4)) {
        double rc[4];
        if (rect(node, rc) || draw_quad(r, canvas, cur, node->material, node->colors, node->coords, node->ncoords, rc, m, ga)) return -1;
    } else if (!memcmp(node->tag, "wnd1", 4)) {
        if (draw_window(r, canvas, cur, node, m, ga)) return -1;
    } else if (!memcmp(node->tag, "prt1", 4) && node->part && *node->part) {
        char key[256];
        snprintf(key, sizeof key, "blyt/%s.bflyt", node->part);
        int seen = 0;
        for (int i = 0; i < anc->n; i++) seen |= !strcmp(anc->names[i], node->part);
        if (archive_get(r, key) && !seen) {
            Layout *part = get_layout(r, node->part);
            if (!part || anc->n == 32) return -1;
            double pm[6], mag[6] = { node->magnify[0], 0, 0, 0, node->magnify[1], 0 };
            mat_mul(m, mag, pm);
            Ancestry a2 = *anc; a2.names[a2.n++] = node->part;
            for (size_t i = 0; i < part->nroots; i++)
                if (walk(r, canvas, part, part->roots[i], pm, NULL, ga, &a2, node)) return -1;
        }
    }
    for (size_t i = 0; i < node->nchildren; i++)
        if (walk(r, canvas, cur, node->children[i], m, node, (node->flags & 2) ? ga : alpha, anc, ov_owner)) return -1;
    return 0;
}
/* Renderer.render(name, pane, size, view) */
static CgImg *render(R *r, const char *name, const char *pane, const int *size_in, const double *view_in) {
    Layout *l = get_layout(r, name);
    if (!l) return NULL;
    int size[2] = { size_in ? size_in[0] : pyround(l->size[0]), size_in ? size_in[1] : pyround(l->size[1]) };
    CgImg *canvas = cg_new(CG_RGBA, size[0], size[1]);
    if (!canvas) { seed_err(r->err, "internal", "Out of memory"); return NULL; }
    double dv[4] = { -l->size[0]/2, l->size[1]/2, l->size[0], l->size[1] }, view[4];
    memcpy(view, view_in ? view_in : dv, sizeof view);
    double x = view[0], y = view[1], w = view[2], h = view[3];
    double screen[6] = { size[0]/w, 0, -x*size[0]/w, 0, -size[1]/h, y*size[1]/h };
    if (!l->centered && !memcmp(view, dv, sizeof dv)) { screen[2] = 0; screen[5] = 0; }
    Ancestry anc = { { l->name }, 1 };
    int rc = 0;
    if (pane) {
        const Pane *p = layout_pane(l, pane);
        if (!p) { seed_err(r->err, "internal", "Companion decor: %s has no pane %s", name, pane); rc = -1; }
        else rc = walk(r, canvas, l, p, screen, NULL, 255, &anc, NULL);
    } else
        for (size_t i = 0; i < l->nroots && !rc; i++) rc = walk(r, canvas, l, l->roots[i], screen, NULL, 255, &anc, NULL);
    if (rc) { if (!r->failed) seed_err(r->err, "internal", "Companion decor: cannot render %s", name); r->failed = 1; cg_free(canvas); return NULL; }
    return canvas;
}

/* ---- extract_decor() ---- */
typedef struct { R *r; Buf *decor, *sources; int count; } D;
static void rgba_hex(Buf *b, const uint8_t c[4]) { buf_printf(b, "\"#%02X%02X%02X%02X\"", c[0], c[1], c[2], c[3]); }
static int emit(D *d, const char *name, CgImg *image, const char *source, const char *usage, const int *nine, int mask,
                const uint8_t *color, int tile) {
    int box[4];
    if (!image || !cg_alpha_bbox(image, box)) {
        cg_free(image);
        if (!d->r->failed) seed_err(d->r->err, "internal", "Empty companion decor: %s (%s)", name, source);
        return -1;
    }
    if (mask) for (size_t i = 0; i < (size_t)image->w * image->h; i++) memset(image->p + i * 4, 255, 3);
    char path[128];
    snprintf(path, sizeof path, "decor/%s.webp", name);
    int rc = cg_save_webp(d->r->out, image, path, 95);
    if (!rc) {
        Buf *b = d->decor;
        buf_add(b, d->count ? "," : "", d->count ? 1 : 0);
        cg_json_str(b, name); buf_printf(b, ":{\"path\":"); cg_json_str(b, path);
        buf_printf(b, ",\"w\":%d,\"h\":%d", image->w, image->h);
        if (nine) buf_printf(b, ",\"nine\":[%d,%d,%d,%d]", nine[0], nine[1], nine[2], nine[3]);
        if (mask) buf_puts(b, ",\"mask\":true");
        if (color) { buf_puts(b, ",\"color\":"); rgba_hex(b, color); }
        if (tile) buf_puts(b, ",\"tile\":true");
        buf_puts(b, "}");
        b = d->sources;
        buf_add(b, d->count ? "," : "", d->count ? 1 : 0);
        cg_json_str(b, name); buf_puts(b, ":{\"source\":"); cg_json_str(b, source);
        buf_puts(b, ",\"usage\":"); cg_json_str(b, usage); buf_puts(b, "}");
        d->count++;
    }
    cg_free(image);
    return rc;
}
static const int NINE8[4] = { 8, 8, 8, 8 };
/* panes(): render each selected pane in turn, composite, alpha-crop unless crop=False */
static int panes(D *d, const char *name, const char *layout, const char *const *sel, const char *usage, const int *nine, int crop,
                 const int *size, const double *view) {
    CgImg *image = NULL;
    if (cg_stopped(d->r->out)) return -1;   /* cancelled, or another task failed */
    char source[256]; size_t at = (size_t)snprintf(source, sizeof source, "%s/", layout);
    for (int i = 0; sel[i]; i++) {
        at += (size_t)snprintf(source + at, at < sizeof source ? sizeof source - at : 0, "%s%s", i ? "+" : "", sel[i]);
        CgImg *layer = render(d->r, layout, sel[i], size, view);
        if (!layer) { cg_free(image); return -1; }
        if (!image) image = layer;
        else { cg_alpha_composite(image, layer, 0, 0); cg_free(layer); }
    }
    int box[4];
    if (crop && image && cg_alpha_bbox(image, box)) {
        CgImg *c = cg_crop(image, box[0], box[1], box[2], box[3]);
        cg_free(image);
        if (!(image = c)) { seed_err(d->r->err, "internal", "Out of memory"); return -1; }
    }
    return emit(d, name, image, source, usage, nine, 0, NULL, 0);
}
#define SEL(...) ((const char *const[]){ __VA_ARGS__, NULL })
static int texture(D *d, const char *name, const char *tex, const char *usage, int mask, const uint8_t *color, int tile) {
    if (cg_stopped(d->r->out)) return -1;
    const CgImg *t = get_texture(d->r, tex);
    if (d->r->failed) return -1;
    char source[256];
    snprintf(source, sizeof source, "timg/__Combined.bntx/%s", tex);
    return emit(d, name, t ? cg_copy(t) : NULL, source, usage, NULL, mask, color, tile);
}
static const uint8_t *material_white(D *d, const char *layout, const char *mat) {
    Layout *l = get_layout(d->r, layout);
    const Mat *m = l ? layout_mat(l, mat) : NULL;
    if (!m && !d->r->failed) { seed_err(d->r->err, "internal", "Companion decor: %s has no material %s", layout, mat); d->r->failed = 1; }
    return m ? m->white : NULL;
}

int cg_decor(CgOut *out, const CgDecorIn *in, Buf *decor, Buf *palette, Buf *page_icons, Buf *sources) {
    R r = { .out = out, .in = in, .err = cg_err(out) };
    D d = { &r, decor, sources, 0 };
    int rc = -1;
    static const struct { const char *key, *layout, *mat; } PALETTE[] = {
        { "text", "PaPouchContentInfo_00", "T_Caption_00" }, { "textDim", "PaAppMapCursor_00", "T_Hint_00" },
        { "accent", "PaSplitLine_00", "P_SplitLine_00" }, { "accentDim", "PauseMenuOverlay_00", "P_DecoL_00" },
        { "panel", "PaPouchActionWindow_00", "P_Base_00" }, { "gold", "PaIconBtnLight_01", "W_FrameBornOut_00LT" },
        { "cursor", "PaIconBtnLight_01", "P_Decide_00" }, { "background", "PaPouchActionWindow_00", "P_Base_00" },
        { "equipped", "PaIconBtnLight_01", "W_FrameBornOut_00LT" }, { "stamina", "PaStaminaGauge_00", "P_GaugeMain_00" },
        { "energy", "PaEnergyGauge_00", "P_Main_04" } };
    buf_puts(palette, "{");
    for (size_t i = 0; i < sizeof PALETTE / sizeof *PALETTE; i++) {
        const uint8_t *w = material_white(&d, PALETTE[i].layout, PALETTE[i].mat);
        if (!w) goto done;
        uint8_t c[4] = { w[0], w[1], w[2], w[3] };
        if (!strcmp(PALETTE[i].key, "textDim")) {   /* the hint material's RGB with its pane's alpha */
            const Layout *l = get_layout(&r, "PaAppMapCursor_00"); const Pane *p = l ? layout_pane(l, "T_Hint_00") : NULL;
            if (!p) { seed_err(r.err, "internal", "Companion decor: PaAppMapCursor_00 has no pane T_Hint_00"); goto done; }
            c[3] = (uint8_t)p->alpha;
        }
        if (i) buf_puts(palette, ",");
        cg_json_str(palette, PALETTE[i].key); buf_puts(palette, ":"); rgba_hex(palette, c);
    }
    buf_puts(palette, "}");
    buf_puts(decor, "{"); buf_puts(sources, "{");

    const uint8_t *wash = material_white(&d, "PaPouchActionWindow_00", "P_Base_00");
    const Layout *aw = wash ? get_layout(&r, "PaPouchActionWindow_00") : NULL;
    const Pane *base = aw ? layout_pane(aw, "P_Base_00") : NULL;
    if (!base) { if (!r.failed) seed_err(r.err, "internal", "Companion decor: PaPouchActionWindow_00 has no pane P_Base_00"); goto done; }
    uint8_t bg[4] = { wash[0], wash[1], wash[2], (uint8_t)base->alpha };
    if (emit(&d, "background", cg_new_fill(CG_RGBA, 1920, 1080, bg), "PaPouchActionWindow_00/P_Base_00 material+alpha",
             "stretch; authored dark warm translucent wash", NULL, 0, NULL, 0)) goto done;
    const uint8_t *deco = material_white(&d, "PauseMenuOverlay_00", "P_DecoL_00"), *line = material_white(&d, "PaSplitLine_00", "P_SplitLine_00");
    const uint8_t *tab_text = material_white(&d, "PaPouchTab_00", "T_Text_00");
    if (!deco || !line || !tab_text) goto done;
    if (texture(&d, "background_pattern", "AppMenuBGStar_00^s", "tile; subtle authored star/noise mask", 1, deco, 1) ||
        panes(&d, "vignette", "AppMenuBG_00", SEL("W_Shadow_00"), "stretch full view; alpha-cropped soft black edge fade", NULL, 1, NULL, NULL) ||
        panes(&d, "deco_left", "PauseMenuOverlay_00", SEL("P_DecoL_00"), "uniform scale; gold ripples anchored bottom-left", NULL, 1, NULL, NULL) ||
        panes(&d, "deco_right", "PauseMenuOverlay_00", SEL("P_DecoR_00"), "uniform scale; gold ripples anchored bottom-right", NULL, 1, NULL, NULL) ||
        panes(&d, "header_line", "PaSplitLine_00", SEL("P_SplitLine_00"), "stretch horizontally; warm cream title rule", NULL, 1, NULL, NULL) ||
        panes(&d, "divider", "PaSplitLine_00", SEL("P_SplitLine_00"), "stretch horizontally; cream 2px rule", NULL, 1, NULL, NULL) ||
        panes(&d, "footer", "PaSplitLine_00", SEL("P_SplitLine_00"), "stretch horizontally; thin cream footer rule (not the raw solid footer prototype)", NULL, 1, NULL, NULL) ||
        panes(&d, "tab", "PaCommonBtnThin_00", SEL("P_Base_00"), "nine-slice; clean dark unselected Pouch plate", NINE8, 1, NULL, NULL) ||
        panes(&d, "tab_selected", "PaCommonBtnThin_00", SEL("P_Base_00", "W_Frame_00"), "nine-slice; dark/gold selected Pouch button", NINE8, 1, NULL, NULL) ||
        panes(&d, "slot", "PaIconBtnLight_01", SEL("P_Base_00", "W_Frame_00"), "nine-slice; empty inventory cell backing", NINE8, 1, NULL, NULL) ||
        panes(&d, "slot_selected", "PaIconBtnLight_01", SEL("W_FrameBornOut_00"), "nine-slice; gold cursor overlay with transparent center", NINE8, 1, NULL, NULL) ||
        panes(&d, "info_panel", "PaPouchActionWindow_00", SEL("P_Base_01"), "stretch; dark item information backing (capture-free material)", NULL, 1, NULL, NULL) ||
        panes(&d, "window", "PaPouchActionWindow_00", SEL("P_Base_01", "W_Frame_01"), "nine-slice; dark/gold generic window", NINE8, 1, NULL, NULL) ||
        panes(&d, "title_plate", "PaCommonBtnThin_00", SEL("P_Base_00", "W_Frame_00"), "nine-slice; clean dark Pouch plate with thin gold border", NINE8, 1, NULL, NULL) ||
        panes(&d, "scrollbar_track", "PaScrollBar_01", SEL("P_Line_00", "P_Base_00"), "stretch vertically; inventory scrollbar outline and black track", NULL, 1, NULL, NULL) ||
        panes(&d, "scrollbar_thumb", "PaScrollBar_01", SEL("P_ScrollBar_00"), "stretch vertically; cream inventory thumb", NULL, 1, NULL, NULL) ||
        panes(&d, "map_frame", "AppMenuOverlay_00", SEL("N_FrameAlbNrm_00"), "stretch; authored Purah Pad stone side frame", NULL, 0, NULL, NULL) ||
        panes(&d, "map_north", "PaAppMapNorthMark_00", SEL("RootPane"), "uniform scale; north arrow and backing", NULL, 1, NULL, NULL) ||
        texture(&d, "map_cursor", "MapCursorCenter_00^s", "uniform scale; actual map cursor center mask in warm menu palette", 1, line, 0) ||
        panes(&d, "gauge_stamina", "PaStaminaGauge_00", SEL("P_GaugeMain_02"), "uniform scale; one stamina ring (1000 capacity), clip for fractional fill", NULL, 1, NULL, NULL) ||
        panes(&d, "gauge_stamina_empty", "PaStaminaGauge_00", SEL("P_BaseGauge_02"), "uniform scale; one empty stamina ring (1000 capacity)", NULL, 1, NULL, NULL))
        goto done;
    static const int battery_size[2] = { 128, 88 };
    static const double battery_view[4] = { -64, 44, 128, 88 };
    if (panes(&d, "gauge_battery", "PaEnergyGauge_00", SEL("P_Base_01", "P_Main_04"), "uniform scale; battery frame and green cells", NULL, 0, battery_size, battery_view) ||
        panes(&d, "gauge_battery_empty", "PaEnergyGauge_00", SEL("P_Base_00", "P_Base_01"), "uniform scale; empty battery frame", NULL, 0, battery_size, battery_view) ||
        texture(&d, "layer_ground", "MapTabIcon_Ground^s", "uniform scale; tintable map layer glyph", 1, line, 0) ||
        texture(&d, "layer_sky", "MapTabIcon_Sky^s", "uniform scale; tintable map layer glyph", 1, line, 0) ||
        texture(&d, "layer_depths", "MapTabIcon_Cave^s", "uniform scale; tintable map layer glyph", 1, line, 0))
        goto done;
    static const char *const PAGES[3][2] = { { "adventure", "Nt_PauseIconCh_06^s" }, { "inventory", "Nt_PauseIcon_10^s" }, { "map", "MapTabIcon_Ground^s" } };
    buf_puts(page_icons, "{");
    for (int i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof name, "page_%s", PAGES[i][0]);
        if (texture(&d, name, PAGES[i][1], "uniform scale; tintable page glyph", 1, tab_text, 0)) goto done;
        if (i) buf_puts(page_icons, ",");
        cg_json_str(page_icons, PAGES[i][0]); buf_printf(page_icons, ":\"decor/%s.webp\"", name);
    }
    buf_puts(page_icons, "}");
    buf_puts(decor, "}"); buf_puts(sources, "}");
    rc = 0;
done:
    for (size_t i = 0; i < r.nlayouts; i++) layout_free(r.layouts[i]);
    free(r.layouts);
    for (size_t i = 0; i < r.ntex; i++) { free(r.tex[i].name); cg_free(r.tex[i].img); }
    free(r.tex);
    return rc;
}
