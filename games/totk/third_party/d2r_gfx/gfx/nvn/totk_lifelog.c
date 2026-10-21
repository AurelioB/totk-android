// TOTK diagnostic DM_NVN_LIFELOG: see totk_lifelog.h.
#include "totk_lifelog.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

extern uint64_t g_frame_pub;

enum { LIFE_IDS = 1 << 20, LIFE_CBS = 64, LIFE_HANDLES = 256 };
static int g_on = -1, g_fmt = -1;
static unsigned g_w, g_h;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_reg;                              // id -> serial of the latest matching registration
static const void *g_cb[LIFE_CBS]; static unsigned g_ncb;
static uint64_t g_handle[LIFE_HANDLES]; static unsigned g_nhandle;

int totk_life_on(void) {
    if (g_on < 0) {
        const char *e = getenv("DM_NVN_LIFELOG"); unsigned f = 0; unsigned w = 0, h = 0;
        int n = e ? sscanf(e, "%ux%u:%x", &w, &h, &f) : 0;
        g_w = w; g_h = h; g_fmt = n == 3 ? (int)f : -1;
        if (n >= 2 && !(g_reg = calloc(LIFE_IDS, sizeof *g_reg))) { fprintf(stderr, "[life] cannot allocate the registration shadow\n"); abort(); }
        if (n >= 2) fprintf(stderr, "[life] lifecycle log on: %ux%u format %s%x\n", w, h, g_fmt < 0 ? "any " : "0x", g_fmt < 0 ? 0 : (unsigned)g_fmt);
        g_on = n >= 2;
    }
    return g_on;
}
int totk_life_match(const tex_t *t) {
    return totk_life_on() && t && t->d.width == g_w && t->d.height == g_h && (g_fmt < 0 || (int)t->d.format == g_fmt);
}
void totk_life_log(const char *fmt, ...) {
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    uint64_t th = 0; pthread_threadid_np(NULL, &th);
    fprintf(stderr, "[life] t=%.6f th=%llu fp=%llu %s\n", (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9,
            (unsigned long long)th, (unsigned long long)g_frame_pub, b);
}
void totk_life_reg(int id, const tex_t *t) {
    if (!totk_life_on() || id < 0 || id >= LIFE_IDS) return;
    uint64_t was = __atomic_load_n(&g_reg[id], __ATOMIC_RELAXED);
    if (totk_life_match(t)) {
        __atomic_store_n(&g_reg[id], t->serial, __ATOMIC_RELAXED);
        totk_life_log("reg id=%d ser=%llu mem=%p tex_was=%llu", id, (unsigned long long)t->serial, (void *)t->mem, (unsigned long long)was);
    } else if (was) {
        __atomic_store_n(&g_reg[id], 0, __ATOMIC_RELAXED);
        totk_life_log("reg id=%d now ser=%llu fmt=%#x %ux%u mem=%p (was matching ser=%llu)", id, t ? (unsigned long long)t->serial : 0ull,
                      t ? t->d.format : 0, t ? t->d.width : 0, t ? t->d.height : 0, t ? (void *)t->mem : NULL, (unsigned long long)was);
    }
}
uint64_t totk_life_reg_serial(int id) {
    return totk_life_on() && id >= 0 && id < LIFE_IDS ? __atomic_load_n(&g_reg[id], __ATOMIC_RELAXED) : 0;
}
void totk_life_cb_mark(const void *cb) {
    pthread_mutex_lock(&g_mu);
    unsigned i = 0; while (i < g_ncb && g_cb[i] != cb) i++;
    if (i == g_ncb && g_ncb < LIFE_CBS) g_cb[g_ncb++] = cb;
    pthread_mutex_unlock(&g_mu);
}
int totk_life_cb_take(const void *cb) {
    pthread_mutex_lock(&g_mu);
    unsigned i = 0; while (i < g_ncb && g_cb[i] != cb) i++;
    int found = i < g_ncb; if (found) g_cb[i] = g_cb[--g_ncb];
    pthread_mutex_unlock(&g_mu);
    return found;
}
void totk_life_handle_add(uint64_t h) {
    pthread_mutex_lock(&g_mu); g_handle[g_nhandle++ % LIFE_HANDLES] = h; pthread_mutex_unlock(&g_mu);
}
int totk_life_handle_has(uint64_t h) {
    pthread_mutex_lock(&g_mu);
    int found = 0; for (unsigned i = 0; i < LIFE_HANDLES && !found; i++) found = g_handle[i] == h;
    pthread_mutex_unlock(&g_mu);
    return found;
}

enum { TEXCOPY_LINES = 20000, TEXCOPY_RETIRED = 4096 };
static int g_texcopy_on = -1;
static unsigned g_texcopy_w, g_texcopy_h, g_texcopy_fmt;
static int g_texcopy_has_fmt;
static pthread_once_t g_texcopy_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_texcopy_log_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_texcopy_history_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_texcopy_lines;
static uint64_t g_texcopy_from;   /* TOTK_VK_TEXCOPY_FROM=<frame>: earlier records are dropped and do not count against the cap */
static tex_t *g_texcopy_active;
static size_t g_texcopy_nactive, g_texcopy_cap;
static tex_t g_texcopy_retired[TEXCOPY_RETIRED];
static unsigned g_texcopy_nretired, g_texcopy_next;
static uint64_t g_texcopy_record;
// Not every backend exposes its submission generation. Never mislabel a record counter as one.
extern uint64_t vk_submit_serial(void) __attribute__((weak));

static void texcopy_init(void) {
    const char *enabled = getenv("TOTK_VK_TEXCOPY_TRACE"), *filter = getenv("DM_NVN_LIFELOG");
    int on = 0;
    if (enabled && !strcmp(enabled, "1") && filter && *filter >= '0' && *filter <= '9') {
        char *end;
        unsigned long w = strtoul(filter, &end, 10), h = 0, fmt = 0;
        if (*end == 'x' && end[1] >= '0' && end[1] <= '9') {
            h = strtoul(end + 1, &end, 10);
            if (*end == ':' && end[1] >= '0' && end[1] <= '9') {
                fmt = strtoul(end + 1, &end, 16); g_texcopy_has_fmt = 1;
            } else if (*end == ':' && ((end[1] >= 'a' && end[1] <= 'f') || (end[1] >= 'A' && end[1] <= 'F'))) {
                fmt = strtoul(end + 1, &end, 16); g_texcopy_has_fmt = 1;
            }
            if (!*end && w && h && w <= UINT_MAX && h <= UINT_MAX && fmt <= UINT_MAX) {
                g_texcopy_w = (unsigned)w; g_texcopy_h = (unsigned)h; g_texcopy_fmt = (unsigned)fmt; on = 1;
            }
        }
    }
    const char *from = getenv("TOTK_VK_TEXCOPY_FROM");
    if (from) g_texcopy_from = strtoull(from, NULL, 10);
    __atomic_store_n(&g_texcopy_on, on, __ATOMIC_RELEASE);
}
int totk_texcopy_on(void) {
    int on = __atomic_load_n(&g_texcopy_on, __ATOMIC_ACQUIRE);
    if (on < 0) { pthread_once(&g_texcopy_once, texcopy_init); on = __atomic_load_n(&g_texcopy_on, __ATOMIC_ACQUIRE); }
    return on;
}
int totk_texcopy_match(const tex_t *t) {
    return totk_texcopy_on() && t && t->d.width == g_texcopy_w && t->d.height == g_texcopy_h &&
           (!g_texcopy_has_fmt || t->d.format == g_texcopy_fmt);
}
void totk_texcopy_log(uint64_t sub, const char *fmt, ...) {
    if (!totk_texcopy_on() || g_frame_pub < g_texcopy_from) return;
    pthread_mutex_lock(&g_texcopy_log_mu);
    if (g_texcopy_lines < TEXCOPY_LINES) {
        if (++g_texcopy_lines == TEXCOPY_LINES) {
            fprintf(stderr, "[texcopy] fp=%llu sub=%llu op=trace result=cap limit=%u (including this line)\n",
                    (unsigned long long)g_frame_pub, (unsigned long long)sub, TEXCOPY_LINES);
        } else {
            char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
            fprintf(stderr, "[texcopy] fp=%llu sub=%llu %s\n", (unsigned long long)g_frame_pub, (unsigned long long)sub, b);
        }
    }
    pthread_mutex_unlock(&g_texcopy_log_mu);
}
void totk_texcopy_track(const tex_t *t) {
    if (!totk_texcopy_match(t) || !t->serial) return;
    pthread_mutex_lock(&g_texcopy_history_mu);
    for (size_t i = 0; i < g_texcopy_nactive; i++) if (g_texcopy_active[i].serial == t->serial) goto done;
    for (unsigned i = 0; i < g_texcopy_nretired; i++) if (g_texcopy_retired[i].serial == t->serial) goto done;
    if (g_texcopy_nactive == g_texcopy_cap) {
        size_t cap = g_texcopy_cap ? g_texcopy_cap * 2 : 64;
        tex_t *p = realloc(g_texcopy_active, cap * sizeof *p);
        if (!p) {
            totk_texcopy_log(vk_submit_serial ? vk_submit_serial() : 0, "op=nvnTextureInitialize result=history-allocation-failure");
            abort();
        }
        g_texcopy_active = p; g_texcopy_cap = cap;
    }
    g_texcopy_active[g_texcopy_nactive++] = *t;
done:
    pthread_mutex_unlock(&g_texcopy_history_mu);
}
void totk_texcopy_finalize(const tex_t *t) {
    if (!totk_texcopy_match(t) || !t->serial) return;
    pthread_mutex_lock(&g_texcopy_history_mu);
    for (unsigned i = 0; i < g_texcopy_nretired; i++) if (g_texcopy_retired[i].serial == t->serial) goto done;
    for (size_t i = 0; i < g_texcopy_nactive; i++) if (g_texcopy_active[i].serial == t->serial) {
        g_texcopy_active[i] = g_texcopy_active[--g_texcopy_nactive]; break;
    }
    g_texcopy_retired[g_texcopy_next] = *t;
    g_texcopy_next = (g_texcopy_next + 1) % TEXCOPY_RETIRED;
    if (g_texcopy_nretired < TEXCOPY_RETIRED) g_texcopy_nretired++;
done:
    pthread_mutex_unlock(&g_texcopy_history_mu);
}
int totk_texcopy_finalized(uint64_t serial) {
    if (!totk_texcopy_on() || !serial) return -1;
    int state = -1;
    pthread_mutex_lock(&g_texcopy_history_mu);
    for (size_t i = 0; i < g_texcopy_nactive; i++) if (g_texcopy_active[i].serial == serial) { state = 0; goto done; }
    for (unsigned i = 0; i < g_texcopy_nretired; i++) if (g_texcopy_retired[i].serial == serial) { state = 1; break; }
done:
    pthread_mutex_unlock(&g_texcopy_history_mu);
    return state;
}
static int texcopy_intersects(uint64_t addr, uint64_t size, const tex_t *t) {
    uint64_t base = (uint64_t)(uintptr_t)t->mem;
    return base && size && t->L.size && (addr >= base ? addr - base < t->L.size : base - addr < size);
}
int totk_texcopy_overlap(uint64_t addr, uint64_t size, tex_t *out) {
    if (!totk_texcopy_on() || !size || !out) return 0;
    uint64_t newest = 0;
    pthread_mutex_lock(&g_texcopy_history_mu);
    for (size_t i = 0; i < g_texcopy_nactive; i++) {
        const tex_t *t = &g_texcopy_active[i];
        if (t->serial > newest && texcopy_intersects(addr, size, t)) { *out = *t; newest = t->serial; }
    }
    for (unsigned i = 0; i < g_texcopy_nretired; i++) {
        const tex_t *t = &g_texcopy_retired[i];
        if (t->serial > newest && texcopy_intersects(addr, size, t)) { *out = *t; newest = t->serial; }
    }
    pthread_mutex_unlock(&g_texcopy_history_mu);
    return newest != 0;
}
size_t totk_texcopy_nonzero(const void *p, size_t n) {
    if (!p) return 0;
    const uint8_t *b = p; size_t count = 0;
    for (size_t i = 0; i < n; i++) count += b[i] != 0;
    return count;
}
static int texcopy_pitch(const tex_t *t, const NVNcopyRegion *r, ptrdiff_t row, ptrdiff_t img,
                         size_t *width, size_t *height, size_t *rp, size_t *ip, size_t *span) {
    if (!t || !r || r->width <= 0 || r->height <= 0 || r->depth < 0 || row < 0 || img < 0) return 0;
    size_t depth = r->depth ? (unsigned)r->depth : 1;
    int bw = 0, bh = 0, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb <= 0 || bw <= 0 || bh <= 0) return 0;
    size_t w = ((size_t)r->width + (unsigned)bw - 1) / (unsigned)bw;
    size_t h = ((size_t)r->height + (unsigned)bh - 1) / (unsigned)bh;
    if (w > SIZE_MAX / (unsigned)bpb) return 0;
    w *= (unsigned)bpb;
    size_t a = row ? (size_t)row : w;
    if (a < w || a > SIZE_MAX / h) return 0;
    size_t b = img ? (size_t)img : a * h;
    if (b < a * h || depth - 1 > (SIZE_MAX - a * (h - 1) - w) / b) return 0;
    *width = w; *height = h; *rp = a; *ip = b;
    *span = b * (depth - 1) + a * (h - 1) + w;
    return 1;
}
uint64_t totk_texcopy_buffer_size(const tex_t *t, const NVNcopyRegion *r, ptrdiff_t row, ptrdiff_t img) {
    size_t w, h, rp, ip, span;
    return texcopy_pitch(t, r, row, img, &w, &h, &rp, &ip, &span) ? span : 0;
}
totk_texcopy_stats totk_texcopy_payload(const tex_t *t, const NVNcopyRegion *r, const void *p, ptrdiff_t row, ptrdiff_t img) {
    totk_texcopy_stats s = {.min = {255, 255}};
    size_t w, h, rp, ip, span;
    if (!totk_texcopy_on() || !p || !texcopy_pitch(t, r, row, img, &w, &h, &rp, &ip, &span)) return s;
    size_t depth = r->depth ? (unsigned)r->depth : 1;
    if (w > SIZE_MAX / h || w * h > SIZE_MAX / depth) return s;
    s.valid = 1; s.rg = t->d.format == 0xd; s.bytes = w * h * depth;
    for (size_t z = 0; z < depth; z++) for (size_t y = 0; y < h; y++) {
        const uint8_t *b = (const uint8_t *)p + (size_t)z * ip + y * rp;
        for (size_t x = 0; x < w; x++) {
            unsigned v = b[x]; s.nonzero += v != 0;
            if (s.rg) { unsigned c = x & 1; if (v < s.min[c]) s.min[c] = v; if (v > s.max[c]) s.max[c] = v; }
        }
    }
    return s;
}
void totk_texcopy_frontend(const char *op, const char *phase, uint64_t caller, const void *cb,
                          const totk_texcopy_side *src, const totk_texcopy_side *dst, const char *details) {
    if (!totk_texcopy_on()) return;
    totk_texcopy_side a = src ? *src : (totk_texcopy_side){0}, b = dst ? *dst : (totk_texcopy_side){0};
    tex_t overlap_a, overlap_b;
    if (!a.tex && totk_texcopy_overlap(a.addr, a.bytes, &overlap_a)) a.tex = &overlap_a;
    if (!b.tex && totk_texcopy_overlap(b.addr, b.bytes, &overlap_b)) b.tex = &overlap_b;
    if (!totk_texcopy_match(a.tex) && !totk_texcopy_match(b.tex)) return;
    const NVNcopyRegion zero = {0};
    const NVNcopyRegion *ar = a.region ? a.region : &zero, *br = b.region ? b.region : &zero;
    uint64_t sub = vk_submit_serial ? vk_submit_serial() : __atomic_add_fetch(&g_texcopy_record, 1, __ATOMIC_RELAXED);
    totk_texcopy_log(sub,
        "phase=%s op=%s sub_kind=%s caller=main+%#llx cb=%p "
        "src_ser=%llu src_mem=%p src_size=%llu src_level=%u src_layer=%u src_region=%d,%d,%d,%d,%d,%d src_addr=%#llx src_bytes=%llu "
        "dst_ser=%llu dst_mem=%p dst_size=%llu dst_level=%u dst_layer=%u dst_region=%d,%d,%d,%d,%d,%d dst_addr=%#llx dst_bytes=%llu %s",
        phase, op, vk_submit_serial ? "backend-at-record" : "record", (unsigned long long)caller, cb,
        (unsigned long long)(a.tex ? a.tex->serial : 0), a.tex ? (void *)a.tex->mem : NULL, (unsigned long long)(a.tex ? a.tex->L.size : 0),
        a.view && a.view->set_levels ? a.view->base_level : 0, a.view && a.view->set_layers ? a.view->min_layer : 0,
        ar->xoffset, ar->yoffset, ar->zoffset, ar->width, ar->height, ar->depth, (unsigned long long)a.addr, (unsigned long long)a.bytes,
        (unsigned long long)(b.tex ? b.tex->serial : 0), b.tex ? (void *)b.tex->mem : NULL, (unsigned long long)(b.tex ? b.tex->L.size : 0),
        b.view && b.view->set_levels ? b.view->base_level : 0, b.view && b.view->set_layers ? b.view->min_layer : 0,
        br->xoffset, br->yoffset, br->zoffset, br->width, br->height, br->depth, (unsigned long long)b.addr, (unsigned long long)b.bytes,
        details ? details : "");
}
void totk_texcopy_pool(const char *op, const char *phase, uint64_t caller, const pool_t *p, ptrdiff_t off, size_t n, const char *result) {
    if (!totk_texcopy_on() || !p || !p->mem || !n) return;
    totk_texcopy_side range = {.addr = (uint64_t)(uintptr_t)p->mem + (uint64_t)off, .bytes = n};
    int valid = off >= 0 && (uint64_t)off <= p->size && n <= p->size - (uint64_t)off;
    pthread_mutex_lock(&g_texcopy_history_mu);
    // Report every overlap, not just the newest tile in a whole-pool invalidation.
    for (unsigned retired = 0; retired < 2; retired++) {
        size_t count = retired ? g_texcopy_nretired : g_texcopy_nactive;
        for (size_t i = 0; i < count; i++) {
            const tex_t *t = retired ? &g_texcopy_retired[i] : &g_texcopy_active[i];
            if (!texcopy_intersects(range.addr, n, t)) continue;
            range.tex = t;
            char details[240];
            snprintf(details, sizeof details, "pool=%p pool_flags=%#x off=%lld size=%zu range_valid=%d finalized=%u result=%s",
                     (const void *)p, p->flags, (long long)off, n, valid, retired, result);
            totk_texcopy_frontend(op, phase, caller, NULL, &range, &range, details);
        }
    }
    pthread_mutex_unlock(&g_texcopy_history_mu);
}
