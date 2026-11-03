/* TOTK_ASPECT / key 4 (runtime/aspect_patch.h) on Android: the camera draws a wider (or narrower) view into the game's
   16:9 canvas and the presenter shows that canvas at the requested ratio (vk_present_policy.h vk_present_fit_shown), so
   the HUD drawn into the same canvas would come out stretched (21:9) or squeezed (4:3) with the picture. Its draws are
   scaled about the canvas centre instead, as the Mac backend does (nvn_mtl.m uw_ui_squeeze):
   - wider than 16:9: horizontally by 16:9 / ratio, so the HUD keeps its shape in the middle 16:9 of the picture;
   - narrower: vertically by ratio / 16:9, so the HUD keeps its shape at the full shown width, inside the shown area.
   A UI draw: into the canvas after this frame's UI has begun (vk_canvas_first_ui: the first draw with one of the UI
   vertex shaders), a quad of 6 vertices or indices drawn with one of the UI vertex shaders (vk_canvas_ui_shader, the
   Mac's uw_ui_vs list). Everything else stays: the inventory composes its 3D scene after UI draws with full-screen
   triangles and its own opaque quad, which belong to that scene. The canvas is the shared walking-owner tracker's
   (vk_canvas.c, learned from the terminal transfer into the window), switched on for a requested ratio other than
   16:9 (vk_ui_aspect_tracks); unset or 16:9 leaves every draw exactly as it was. */
#pragma once
#include "vk_canvas.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { float sx, sy, cx, cy; } vk_ui_aspect;   /* scale about (cx, cy); sx = sy = 1: unchanged */

/* TOTK_ASPECT as runtime/aspect_patch.h aspect_requested reads it; 0 when unset, "0" or malformed (the runtime refuses
   to start with a malformed value). */
static inline float vk_ui_aspect_requested(const char *e) {
    if (!e || !*e || !strcmp(e, "0")) return 0;
    double a = 0, w, h; char rest;
    if ((sscanf(e, "%lf:%lf%c", &w, &h, &rest) == 2 || sscanf(e, "%lfx%lf%c", &w, &h, &rest) == 2) && w > 0 && h > 0) a = w / h;
    else if (sscanf(e, "%lf%c", &w, &rest) == 1) a = w;
    return a >= 0.5 && a <= 8 ? (float)a : 0;
}

/* The UI's correction for a picture shown at this ratio: >1 wider than 16:9, <1 narrower, 1 for off (0) or 16:9. */
static inline double vk_ui_aspect_factor(float shown) {
    double f = shown > 0 ? shown * 9.0 / 16.0 : 1;
    return f > 1.001 || f < 0.999 ? f : 1;
}

/* Whether a requested start ratio needs the canvas tracker for the HUD. */
static inline int vk_ui_aspect_tracks(float requested) { return vk_ui_aspect_factor(requested) != 1; }

/* The scale for one draw into target (the pass's colour attachment 0) with vertex shader class ui_shader (vk_shader
   canvas_ui: nonzero for the UI shaders) and count vertices or indices, at shown ratio shown. */
static inline vk_ui_aspect vk_ui_aspect_draw(float shown, const vk_canvas_image *target, int ui_shader, uint32_t count) {
    vk_ui_aspect a = {1, 1, 0, 0};
    double f = vk_ui_aspect_factor(shown);
    if (f == 1 || !ui_shader || count != 6 || !target || !target->width || !target->height || !vk_canvas_in_ui(target)) return a;
    a.cx = target->width * 0.5f; a.cy = target->height * 0.5f;
    if (f > 1) a.sx = (float)(1 / f); else a.sy = (float)f;
    return a;
}

static inline int vk_ui_aspect_on(const vk_ui_aspect *a) { return a->sx != 1 || a->sy != 1; }

/* A guest viewport (x, y, w, h; any sign of h) scaled about the canvas centre. */
static inline void vk_ui_aspect_viewport(const vk_ui_aspect *a, float *x, float *y, float *w, float *h) {
    *x = a->cx + (*x - a->cx) * a->sx; *w *= a->sx;
    *y = a->cy + (*y - a->cy) * a->sy; *h *= a->sy;
}

/* A guest scissor [x0, x1) x [y0, y1) scaled the same way, rounded outwards so no scaled UI pixel is cut. */
static inline void vk_ui_aspect_rect(const vk_ui_aspect *a, int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
    *x0 = (int32_t)floorf(a->cx + (*x0 - a->cx) * a->sx); *x1 = (int32_t)ceilf(a->cx + (*x1 - a->cx) * a->sx);
    *y0 = (int32_t)floorf(a->cy + (*y0 - a->cy) * a->sy); *y1 = (int32_t)ceilf(a->cy + (*y1 - a->cy) * a->sy);
}
