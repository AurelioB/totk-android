/* Shared copy/AHB front-buffer sizing, independent of the game's scene resolution. */
#pragma once
#include <stdint.h>
#include <string.h>

enum { VK_NATIVE_UI_WIDTH = 1920, VK_NATIVE_UI_HEIGHT = 1080 };
typedef struct { uint32_t w, h; } vk_present_extent;
/* Experimental imported-AHB admission, after the driver has bound/decoded its
   actual layout. Reject smaller interpretations as well as overruns: neither
   an unknown layout nor ambiguous allocation padding qualifies as a match. */
static inline int vk_present_ahb_layout_matches(uint64_t allocation, uint64_t required,
        uint32_t memory_types, unsigned memory_type) {
    return allocation && required==allocation && memory_type<32 &&
        (memory_types & (UINT32_C(1)<<memory_type));
}
/* Experiment-only importer build, identified by its complete Vulkan driverInfo.
   A newer tag/version is unknown, not implicitly qualified.
   This is not a public query of an OPTIMAL AHB's actual compression. */
static inline int vk_present_ubwc_driver_known(const char *driver) {
    return driver && !strcmp(driver,"Mesa 27.0.0-devel (git-u1p4c1b809)");
}


static inline vk_present_extent vk_present_output_size(int native_ui, uint32_t app_width,
        int post_scale, uint32_t window_w, uint32_t window_h) {
    /* Guest canvas stays 720p; host UI siblings aspect-fit the window. Missing
       window dimensions use 1080p until reattachment supplies an extent. */
    if (native_ui) return window_w && window_h ? (vk_present_extent){window_w, window_h} :
        (vk_present_extent){VK_NATIVE_UI_WIDTH, VK_NATIVE_UI_HEIGHT};
    if (post_scale && window_w && window_h) return (vk_present_extent){window_w, window_h};
    return (vk_present_extent){app_width, window_w && window_h ?
        (uint32_t)((uint64_t)app_width * window_h / window_w) & ~1u : app_width * 9 / 16};
}

typedef struct { int32_t x,y; uint32_t w,h; } vk_present_rect;
/* Android buffer-transform values, not Display.ROTATION_* indices. Reject
   reflections/unknown flags instead of mistaking them for a quarter turn. */
static inline int vk_present_transform_turns(int transform) {
    switch (transform) {
    case 0: return 0;
    case 4: return 1;
    case 3: return 2;
    case 7: return 3;
    default: return -1;
    }
}
static inline int vk_present_turns_transform(unsigned turns) {
    static const int transform[4] = {0, 4, 3, 7};
    return transform[turns & 3];
}
static inline vk_present_extent vk_present_rotate_extent(uint32_t w, uint32_t h, unsigned turns) {
    return turns & 1 ? (vk_present_extent){h,w} : (vk_present_extent){w,h};
}
/* Clockwise in Android/Vulkan's positive-height, top-left-origin coordinates.
   Endpoints (not pixel centers) keep crops, bars and clipped overlay regions exact. */
static inline vk_present_rect vk_present_rotate_rect(vk_present_rect r,
        uint32_t w, uint32_t h, unsigned turns) {
    switch (turns & 3) {
    case 1: return (vk_present_rect){(int32_t)h-r.y-(int32_t)r.h,r.x,r.h,r.w};
    case 2: return (vk_present_rect){(int32_t)w-r.x-(int32_t)r.w,(int32_t)h-r.y-(int32_t)r.h,r.w,r.h};
    case 3: return (vk_present_rect){r.y,(int32_t)w-r.x-(int32_t)r.w,r.h,r.w};
    default: return r;
    }
}
/* Two rows of the vertex clip-space rotation matrix. The texture coordinates
   stay logical; rotating the existing final triangle rotates every sampled UI/post pixel. */
static inline void vk_present_rotation_matrix(unsigned turns, float matrix[4]) {
    static const float rotation[4][4] = {{1,0,0,1},{0,-1,1,0},{-1,0,0,-1},{0,1,-1,0}};
    for (unsigned i=0;i<4;i++) matrix[i]=rotation[turns & 3][i];
}

/* TOTK_ASPECT / key 4 (runtime/aspect_patch.h): the game keeps drawing into its own canvas, with a camera that sees the
 * wider view squeezed into it. The picture is shown at that ratio: fit a canvas this wide (its height times the ratio)
 * instead of its own width, so the window gets letterbox or pillarbox bars rather than a stretched or squeezed picture.
 * aspect <= 0 (off) keeps the canvas's own width. */
static inline uint32_t vk_present_shown_width(uint32_t w, uint32_t h, float aspect) {
    if (!(aspect > 0) || !w || !h) return w;
    double shown = (double)h * aspect + 0.5;
    return shown >= 1 && shown < 65536 ? (uint32_t)shown : w;
}
/* The centred destination of a w x h source shown at aspect (above) in an ow x oh output, as large as fits. */
static inline vk_present_rect vk_present_fit_shown(uint32_t w, uint32_t h, float aspect, uint32_t ow, uint32_t oh) {
    uint32_t sw = vk_present_shown_width(w, h, aspect);
    if (!sw || !h || !ow || !oh) return (vk_present_rect){0};
    double sc = (double)ow / sw < (double)oh / h ? (double)ow / sw : (double)oh / h;
    int32_t dw = (int32_t)(sw * sc + 0.5), dh = (int32_t)(h * sc + 0.5);
    return (vk_present_rect){((int32_t)ow - dw) / 2, ((int32_t)oh - dh) / 2, (uint32_t)dw, (uint32_t)dh};
}
/* Fit the current guest canvas, without upsampling, into the current window.
 * Both axes are derived together; rounding is at most one destination pixel.
 * An absent/zero extent defers work rather than inventing a startup size. */
static inline vk_present_rect vk_present_guest_fit(uint32_t guest_w, uint32_t guest_h,
        uint32_t window_w, uint32_t window_h) {
    if (!guest_w || !guest_h || !window_w || !window_h) return (vk_present_rect){0};
    uint32_t w=guest_w < window_w ? guest_w : window_w;
    uint32_t h=(uint32_t)((uint64_t)w*guest_h/guest_w);
    if (h > window_h) {
        h=window_h;
        w=(uint32_t)((uint64_t)h*guest_w/guest_h);
    }
    if (!w || !h) return (vk_present_rect){0};
    return (vk_present_rect){(int32_t)((window_w-w)/2),(int32_t)((window_h-h)/2),w,h};
}
/* Nonzero guest basis and host extent; map endpoints identically for real and
   generated native UI output, including nonzero crops and rounded edges. */
static inline vk_present_rect vk_present_native_crop(vk_present_rect r,
        uint32_t guest_w, uint32_t guest_h, uint32_t host_w, uint32_t host_h) {
    int32_t x1=(int32_t)(((uint64_t)(r.x+r.w)*host_w+guest_w/2)/guest_w);
    int32_t y1=(int32_t)(((uint64_t)(r.y+r.h)*host_h+guest_h/2)/guest_h);
    r.x=(int32_t)(((uint64_t)r.x*host_w+guest_w/2)/guest_w);
    r.y=(int32_t)(((uint64_t)r.y*host_h+guest_h/2)/guest_h);
    r.w=x1-r.x; r.h=y1-r.y;
    return r;
}
