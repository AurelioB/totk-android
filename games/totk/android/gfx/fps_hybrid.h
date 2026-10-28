/* Acquire-owned wall-time hysteresis. No allocation, GPU, clocks or guest writes.
 * Raw present rate only drives native->interp. Counterfactual controller work,
 * never the deliberately paced30 rate, drives interp->native. */
#pragma once
#include <stdint.h>

typedef struct {
    uint64_t drop_ns, recover_ns, dwell_ns;
    double drop_fps, recover_score;
} fps_hybrid_config;
static inline fps_hybrid_config fps_hybrid_defaults(void) {
    return (fps_hybrid_config){5000000000ull,8000000000ull,10000000000ull,55.0,0.9};
}
typedef struct {
    fps_hybrid_config config;
    uint64_t changed_ns, last_ns, time_ns[2], switches;
    uint64_t rate_start, rate_last, rate_count;
    uint64_t work_start, work_last, work_good_ns, work_total_ns;
    double last_fps;
    unsigned interp, desired, slow_windows;
} fps_hybrid_policy;
static inline void fps_hybrid_clear_evidence(fps_hybrid_policy *p) {
    p->rate_start=p->rate_last=p->rate_count=0;
    p->work_start=p->work_last=p->work_good_ns=p->work_total_ns=0;
    p->slow_windows=0; p->desired=p->interp;
}
static inline void fps_hybrid_init(fps_hybrid_policy *p, fps_hybrid_config c, uint64_t now) {
    *p=(fps_hybrid_policy){.config=c,.changed_ns=now,.last_ns=now};
}
static inline void fps_hybrid_time(fps_hybrid_policy *p, uint64_t now) {
    if (now>=p->last_ns) p->time_ns[p->interp]+=now-p->last_ns;
    else { fps_hybrid_clear_evidence(p); p->changed_ns=now; }
    p->last_ns=now;
}
static inline void fps_hybrid_present(fps_hybrid_policy *p, uint64_t now, int eligible) {
    fps_hybrid_time(p,now);
    if (!eligible || (p->rate_last && (now<=p->rate_last || now-p->rate_last>250000000ull))) {
        fps_hybrid_clear_evidence(p); return;
    }
    if (p->interp) return;
    if (!p->rate_start) p->rate_start=now;
    p->rate_last=now;
    if (now>p->rate_start) p->rate_count++;
    uint64_t span=now-p->rate_start;
    if (span<p->config.drop_ns) return;
    p->last_fps=p->rate_count*1e9/(double)span;
    p->slow_windows=p->last_fps<p->config.drop_fps ? p->slow_windows+1 : 0;
    if (p->slow_windows>=2) p->desired=1;
    p->rate_start=now; p->rate_count=0;
}
/* Valid work samples cover only the preceding <=250ms tick. Unknown or frozen
 * estimates restart recovery. CPU/GPU must BOTH fit the game's one-tick score. */
static inline void fps_hybrid_work(fps_hybrid_policy *p, uint64_t now, int valid,
        double cpu_score, double gpu_score) {
    fps_hybrid_time(p,now);
    if (!p->interp) return;
    if (!valid || (p->work_last && (now<=p->work_last || now-p->work_last>250000000ull))) {
        fps_hybrid_clear_evidence(p); return;
    }
    if (!p->work_start) p->work_start=now;
    if (p->work_last) {
        uint64_t dt=now-p->work_last;
        p->work_total_ns+=dt;
        if (cpu_score<p->config.recover_score && gpu_score<p->config.recover_score)
            p->work_good_ns+=dt;
        else p->desired=1; /* A newly overloaded tick cancels a pending return. */
    }
    p->work_last=now;
    if (now-p->work_start<p->config.recover_ns) return;
    if (p->work_total_ns && p->work_good_ns>=p->work_total_ns-p->work_total_ns/20)
        p->desired=0;
    else p->desired=1;
    p->work_start=now; p->work_good_ns=p->work_total_ns=0;
}
/* The only mode-changing operation, called BEFORE pacing the next acquire. */
static inline int fps_hybrid_apply(fps_hybrid_policy *p, uint64_t now) {
    fps_hybrid_time(p,now);
    if (p->desired==p->interp || now-p->changed_ns<p->config.dwell_ns) return 0;
    p->interp=p->desired; p->changed_ns=now; p->switches++;
    fps_hybrid_clear_evidence(p);
    return 1;
}
