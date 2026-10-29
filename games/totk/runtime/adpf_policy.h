/* Pure ADPF session state, shared by Android and the fake-API host tests. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { TOTK_ADPF_MAX_THREADS = 16 };
typedef struct {
    void *manager;
    void *(*create)(void *, const int32_t *, size_t, int64_t);
    int (*update)(void *, int64_t);
    int (*report)(void *, int64_t);
    int (*set_threads)(void *, const int32_t *, size_t);
    void (*close)(void *);
} totk_adpf_api;
typedef struct {
    void *session;
    int64_t target;
    int32_t tids[TOTK_ADPF_MAX_THREADS];
    size_t count;
    unsigned attempted;
} totk_adpf_state;
enum { TOTK_ADPF_CREATED = 1, TOTK_ADPF_TARGET = 2, TOTK_ADPF_THREADS = 4,
       TOTK_ADPF_REFUSED = 8, TOTK_ADPF_REPORT_ERROR = 16 };

/* The real tick, never the synthetic midpoint or measured display refresh.
 * Shipping/locked 30 stays 33.3ms; native60/auto follows its effective interval. */
static inline int64_t totk_adpf_target(uint64_t period_ns, int fps60, int framegen) {
    return !framegen && fps60 && period_ns && period_ns < 25000000 ? 16666667 : 33333333;
}
static inline unsigned totk_frame_rate(unsigned real_hz, int framegen, int lattice) {
    /* A 30-Hz physical mode would turn the two-slot 30fps lattice into 15fps. */
    return framegen || lattice || real_hz >= 60 ? 60 : 30;
}
static inline uint64_t totk_adpf_work(uint64_t previous, uint64_t now, uint64_t pacing) {
    if (!previous || now <= previous || pacing >= now - previous) return 0;
    uint64_t work = now - previous - pacing;
    return work < 250000000 ? work : 0; /* background/loading discontinuity */
}
static inline void totk_adpf_close(totk_adpf_state *s, const totk_adpf_api *api) {
    if (s->session) api->close(s->session);
    s->session = NULL;
}
/* Called with canonical sorted unique positive tids. Failed configurations are
 * attempted once, not allocated/retried every frame. A real change permits retry.
 * API33 has no setThreads: recreate only at membership changes. */
static inline unsigned totk_adpf_sync(totk_adpf_state *s, const totk_adpf_api *api,
        const int32_t *tids, size_t count, int64_t target) {
    if (!count || count > TOTK_ADPF_MAX_THREADS || target <= 0) return 0;
    int threads = count != s->count || memcmp(s->tids, tids, count * sizeof *tids);
    int duration = target != s->target;
    if (s->attempted && !threads && !duration) return 0;
    unsigned result = 0;
    if (s->session && threads) {
        if (!api->set_threads || api->set_threads(s->session, tids, count)) totk_adpf_close(s, api);
        else result |= TOTK_ADPF_THREADS;
    }
    if (s->session && duration) {
        if (!api->update || api->update(s->session, target)) totk_adpf_close(s, api);
        else result |= TOTK_ADPF_TARGET;
    }
    memcpy(s->tids, tids, count * sizeof *tids); s->count = count;
    s->target = target; s->attempted = 1;
    if (!s->session) {
        s->session = api->manager && api->create && api->report && api->close ?
            api->create(api->manager, tids, count, target) : NULL;
        result |= s->session ? TOTK_ADPF_CREATED : TOTK_ADPF_REFUSED;
    }
    return result;
}
static inline unsigned totk_adpf_report(totk_adpf_state *s, const totk_adpf_api *api, uint64_t ns) {
    if (!s->session || !ns || ns >= 250000000) return 0;
    return api->report(s->session, (int64_t)ns) ? TOTK_ADPF_REPORT_ERROR : 0;
}
