#ifndef TOTK_FRAMEGEN_BUDGET_EXTERNAL_H
#define TOTK_FRAMEGEN_BUDGET_EXTERNAL_H
#include "framegen_budget.h"

/* Only an admitted external engine uses this sidecar. Keep the ordinary
 * policy's layout and finalize/sample/seal implementation unchanged. */
typedef struct { uint64_t frame; double ms; } fg_budget_external_cost;
typedef struct {
    uint64_t frame;
    double ms;
    unsigned pending;
    int sealed, valid;
} fg_budget_external_group;
typedef struct { fg_budget_external_cost cost[FG_BUDGET_FRAMES]; } fg_budget_external;
static inline void fg_budget_external_finalize(fg_budget_policy *p, fg_budget_external *e) {
    uint64_t last=p->last_frame;
    while (p->finalize && p->finalize<=last) {
        uint64_t id=p->finalize;
        fg_budget_frame *f=&p->frames[id % FG_BUDGET_FRAMES];
        fg_budget_external_cost *c=&e->cost[id % FG_BUDGET_FRAMES];
        if (f->frame==id && f->status==FG_BUDGET_PENDING &&
            (f->last>p->last_sample || (c->frame==id && isnan(c->ms)))) break;
        /* Finalize exactly this interval using the unchanged Vulkan policy. */
        p->last_frame=id; fg_budget_finalize(p); p->last_frame=last;
        if (f->frame==id && f->status==FG_BUDGET_VALID && c->frame==id) {
            if (!isfinite(c->ms) || c->ms<0) f->status=FG_BUDGET_UNAVAILABLE;
            else { f->incremental_ms+=c->ms; f->synthetic=1; }
        }
    }
}
static inline void fg_budget_external_begin(fg_budget_external *e, uint64_t frame) {
    e->cost[frame % FG_BUDGET_FRAMES]=(fg_budget_external_cost){frame,NAN};
}
static inline void fg_budget_external_end(fg_budget_policy *p, fg_budget_external *e, uint64_t frame, double ms, int valid) {
    fg_budget_external_cost *c=&e->cost[frame % FG_BUDGET_FRAMES];
    if (c->frame!=frame || !isnan(c->ms)) return;
    c->ms=valid && isfinite(ms) && ms>=0 ? ms : -1;
    fg_budget_external_finalize(p,e);
}
static inline void fg_budget_external_sample(fg_budget_policy *p, fg_budget_external *e, uint64_t ticket, double ms, int valid) {
    uint64_t finalize=p->finalize; p->finalize=0;
    fg_budget_sample(p,ticket,ms,valid); p->finalize=finalize;
    fg_budget_external_finalize(p,e);
}
static inline void fg_budget_external_seal(fg_budget_policy *p, fg_budget_external *e, uint64_t frame, uint64_t ticket) {
    if (!frame || frame<=p->last_frame) return;
    fg_budget_frame *f=&p->frames[frame % FG_BUDGET_FRAMES];
    *f=(fg_budget_frame){.frame=frame,.first=p->last_ticket+1,.last=ticket};
    if (!p->boundary || frame!=p->last_frame+1 || ticket<=p->last_ticket ||
        ticket-p->last_ticket>FG_BUDGET_SLOTS || p->invalid_interval)
        f->status=FG_BUDGET_UNAVAILABLE;
    if (!p->finalize || frame-p->finalize>=FG_BUDGET_FRAMES) p->finalize=frame;
    p->last_frame=frame; p->last_ticket=ticket; p->boundary=ticket!=0;
    p->invalid_interval=0;
    fg_budget_external_finalize(p,e);
}
/* Adaptive can issue several target phases from one real pair. Account their
 * sum exactly once, only once the real interval is sealed AND every GL job has
 * returned timing. A first completed job must not finalize the others as free. */
static inline void fg_budget_external_group_begin(fg_budget_external *e,
        fg_budget_external_group *g,uint64_t frame) {
    if (g->frame!=frame) {
        *g=(fg_budget_external_group){.frame=frame,.valid=1};
        fg_budget_external_begin(e,frame);
    }
    g->pending++;
}
static inline void fg_budget_external_group_finish(fg_budget_policy *p,
        fg_budget_external *e,fg_budget_external_group *g) {
    if (g->sealed && !g->pending)
        fg_budget_external_end(p,e,g->frame,g->ms,g->valid);
}
static inline void fg_budget_external_group_end(fg_budget_policy *p,
        fg_budget_external *e,fg_budget_external_group *g,uint64_t frame,double ms,int valid) {
    if (g->frame!=frame || !g->pending) return;
    g->pending--;
    g->valid&=valid && isfinite(ms) && ms>=0;
    if (valid && isfinite(ms) && ms>=0) g->ms+=ms;
    fg_budget_external_group_finish(p,e,g);
}
static inline void fg_budget_external_group_seal(fg_budget_policy *p,
        fg_budget_external *e,fg_budget_external_group *g,uint64_t frame) {
    if (g->frame!=frame) return;
    g->sealed=1;
    fg_budget_external_group_finish(p,e,g);
}
#endif
