// TOTK diagnostic DM_NVN_LIFELOG=WxH[:nvnfmt] (e.g. 1600x900:0xa): CPU-side timeline of the textures of that size
// (and format), to follow one resource through the game's frame pipeline. Logged: Initialize / Finalize, texture-pool
// registrations, render targets and texture handles recorded into command buffers, EndRecording / CallCommands of the
// command buffers that recorded them, every queue submission, and (replay side) the texture each draw resolves for a
// handle. Lines carry the time, the thread and the CPU frame (presents so far), so the order across the game's threads
// and submissions can be read off. Off by default; costs one branch per hooked call when off.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "nvn_obj.h"

int totk_life_on(void);
int totk_life_match(const tex_t *t);
void totk_life_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// texture-pool shadow: the serial of the latest matching registration of an id (0 = none or not matching)
void totk_life_reg(int id, const tex_t *t);
uint64_t totk_life_reg_serial(int id);
// command buffers / handles that carried a matching texture, so their EndRecording / CallCommands are logged
void totk_life_cb_mark(const void *cb);
int totk_life_cb_take(const void *cb);
void totk_life_handle_add(uint64_t h);
int totk_life_handle_has(uint64_t h);

// TOTK_VK_TEXCOPY_TRACE=1 additionally requires an explicit, valid DM_NVN_LIFELOG filter.
// All frontend/backend records share a 20000-line cap (including its final announcement).
int totk_texcopy_on(void);
int totk_texcopy_match(const tex_t *t);
void totk_texcopy_log(uint64_t sub, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void totk_texcopy_track(const tex_t *t);
void totk_texcopy_finalize(const tex_t *t);
// Finalized state: 0 active, 1 finalized, -1 unknown (including evicted retired history).
int totk_texcopy_finalized(uint64_t serial);
// Newest overlapping selected storage, retaining all active and the last 4096 retired copies.
int totk_texcopy_overlap(uint64_t addr, uint64_t size, tex_t *out);
size_t totk_texcopy_nonzero(const void *p, size_t n);

typedef struct {
    const tex_t *tex;
    const view_t *view;
    const NVNcopyRegion *region;
    uint64_t addr, bytes;             // optional linear buffer address/range, separate from texture storage
} totk_texcopy_side;
// Caller is the existing bounded guest-main LR lookup, never a guest-chain dump.
uint64_t totk_texcopy_caller(void);
void totk_texcopy_frontend(const char *op, const char *phase, uint64_t caller, const void *cb,
                          const totk_texcopy_side *src, const totk_texcopy_side *dst, const char *details);
void totk_texcopy_pool(const char *op, const char *phase, uint64_t caller, const pool_t *p,
                      ptrdiff_t off, size_t n, const char *result);
typedef struct {
    size_t bytes, nonzero;
    unsigned min[2], max[2];
    int valid, rg;
} totk_texcopy_stats;
// Counts only region bytes, never row/image stride padding. Invalid/unsupported regions are explicit.
totk_texcopy_stats totk_texcopy_payload(const tex_t *t, const NVNcopyRegion *r, const void *p,
                                      ptrdiff_t row, ptrdiff_t img);
uint64_t totk_texcopy_buffer_size(const tex_t *t, const NVNcopyRegion *r, ptrdiff_t row, ptrdiff_t img);
