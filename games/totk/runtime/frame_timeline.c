#include "frame_timeline.h"
#include "frame_fence.h"
#if defined(__ANDROID__) || defined(TOTK_FRAME_TIMELINE_TEST)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef TOTK_FRAME_TIMELINE_TEST
#include <errno.h>
#include <semaphore.h>
#endif

/* Locks cover only a bounded memory update/copy, never I/O or another lock.
 * An expired async writer is rejected, rather than corrupting a reused entry. */
typedef struct { atomic_flag lock; totk_ft_record data; } ft_entry;
static ft_entry ring[FT_RING];
static pthread_once_t once = PTHREAD_ONCE_INIT;
_Atomic int totk_ft_enabled = -1;
static _Atomic uint64_t current = 1, expired;
_Thread_local uint64_t totk_ft_active_frame;
_Thread_local totk_ft_work_counts totk_ft_active_work;
/* Written and consumed only by the sender's host thread. A wake copies this
 * value into the target thr_t under G; the target never reads sender TLS. */
static _Thread_local totk_ft_input_reads input_pending;
static _Thread_local uint32_t input_owner;
#ifndef TOTK_FRAME_TIMELINE_TEST
static _Atomic uint64_t report_until;
static sem_t report_sem;
static void *report_worker(void *unused);
#endif
static void initialize(void) {
    for (unsigned i = 0; i < FT_RING; i++) atomic_flag_clear(&ring[i].lock);
    const char *e = getenv("TOTK_FRAME_TIMELINE");
    int on = e && !strcmp(e, "1");
#ifndef TOTK_FRAME_TIMELINE_TEST
    if (on) {
        pthread_t thread;
        if (sem_init(&report_sem, 0, 0) || pthread_create(&thread, NULL, report_worker, NULL)) {
            fprintf(stderr, "[frame-tl] reporter initialization failed; disabled\n"); on = 0;
        } else pthread_detach(thread);
    }
#endif
    if (on) {
        fprintf(stderr, "[frame-tl] enabled CLOCK_MONOTONIC ring=%u walks=%u slots=%u report=300 delay=30; offcpu=wall-threadCPU\n", FT_RING, FT_WALKS, FT_SLOTS);
        fprintf(stderr, "[frame-tl input] domain=connected-Npad-state-formed-to-AHB-apply-copy-buffer-post-or-AHB-present-fence clock=CLOCK_MONOTONIC association=main-wake-snapshot-or-global-window-observation not-causal-image-ownership not-hardware-event-to-photon; empty-main-wake=no-new-read missing-handoff=unknown\n");
    }
    atomic_store_explicit(&totk_ft_enabled, on, memory_order_release);
}
int totk_ft_init(void) { pthread_once(&once, initialize); return atomic_load(&totk_ft_enabled); }
uint64_t totk_ft_current(void) { return atomic_load_explicit(&current, memory_order_relaxed); }
static void lock_entry(ft_entry *e) {
    while (atomic_flag_test_and_set_explicit(&e->lock, memory_order_acquire)) {}
}
static void unlock_entry(ft_entry *e) { atomic_flag_clear_explicit(&e->lock, memory_order_release); }
static ft_entry *entry(uint64_t frame) {
    if (!frame || !totk_ft_on()) return NULL;
    ft_entry *e = &ring[frame % FT_RING]; lock_entry(e);
    if (e->data.frame > frame) {
        atomic_fetch_add_explicit(&expired, 1, memory_order_relaxed); unlock_entry(e); return NULL;
    }
    if (e->data.frame != frame) { memset(&e->data, 0, sizeof e->data); e->data.frame = frame; }
    return e;
}
static int snapshot(uint64_t frame, totk_ft_record *out) {
    ft_entry *e = &ring[frame % FT_RING]; lock_entry(e);
    int found = frame && e->data.frame == frame;
    if (found) *out = e->data; else memset(out, 0, sizeof *out);
    unlock_entry(e); return found;
}
void totk_ft_work_begin(uint64_t frame) {
    totk_ft_active_frame = frame && totk_ft_on() ? frame : 0;
    if (frame) memset(&totk_ft_active_work, 0, sizeof totk_ft_active_work);
}
void totk_ft_work_end(void) {
    uint64_t frame = totk_ft_active_frame;
    totk_ft_active_frame = 0;
    ft_entry *e = entry(frame); if (!e) return;
    for (unsigned i = 0; i < FT_COUNT_N; i++) e->data.work.value[i] += totk_ft_active_work.value[i];
    unlock_entry(e);
}
void totk_ft_count_frame(uint64_t frame, enum totk_ft_counter counter, uint64_t n) {
    if (!frame || !n) return;
    if (frame == totk_ft_active_frame) { totk_ft_count(counter, n); return; }
    ft_entry *e = entry(frame); if (!e) return;
    e->data.work.value[counter] += n; unlock_entry(e);
}
void totk_ft_acquire_wait(uint64_t frame, uint64_t pacing_ns, uint64_t texture_ns) {
    ft_entry *e = entry(frame); if (!e) return;
    e->data.acquire_pacing_ns += pacing_ns; e->data.acquire_texture_ns += texture_ns;
    unlock_entry(e);
}
static void input_add(totk_ft_input_reads *r, uint64_t now, uint32_t thread, uint64_t sampling) {
    if (!r->reads) r->first_thread = r->last_thread = thread;
    else if (r->first_thread != thread) r->mixed_threads = 1;
    r->reads++;
    if (!thread) r->unknown_thread_reads++;
    if (!now) { r->invalid_reads++; return; }
    if (!r->first || now < r->first || (now == r->first && sampling < r->first_sampling)) {
        r->first = now; r->first_thread = thread; r->first_sampling = sampling;
    }
    if (!r->last || now > r->last || (now == r->last && sampling > r->last_sampling)) {
        r->last = now; r->last_thread = thread; r->last_sampling = sampling;
    }
}
void totk_ft_input_read(uint64_t f, uint64_t now, uint32_t thread, uint64_t sampling) {
    if (!totk_ft_on()) return;
    if (input_owner != thread) {
        memset(&input_pending, 0, sizeof input_pending);
        input_owner = thread;
    }
    input_add(&input_pending, now, thread, sampling);
    ft_entry *e = entry(f); if (!e) return;
    input_add(&e->data.input_window, now, thread, sampling);
    unlock_entry(e);
}
void totk_ft_input_take(totk_ft_input_snapshot *out) {
    memset(out, 0, sizeof *out);
    if (!totk_ft_on()) return;
    out->reads = input_pending;
    memset(&input_pending, 0, sizeof input_pending);
}
void totk_ft_wake(uint64_t f, uint64_t now, uint64_t ready, uint64_t addr, uint64_t waker,
                  const totk_ft_input_snapshot *input) {
    ft_entry *e = entry(f); if (!e) return;
    /* First matching main-thread address wake before the frame's first walk. */
    if (!e->data.wake && !e->data.walks) {
        e->data.wake = now; e->data.ready = ready; e->data.wake_addr = addr; e->data.wake_waker = waker;
        if (input) {
            e->data.input_main_wake = *input;
            e->data.input_main_wake_received = 1;
        }
    }
    unlock_entry(e);
}
void totk_ft_walk(uint64_t f, uint64_t begin, uint64_t end, uint64_t cpu) {
    ft_entry *e = entry(f); if (!e) return;
    if (end >= begin) {
        e->data.walk_ns += end - begin; e->data.walk_cpu_ns += cpu;
        if (e->data.walks < FT_WALKS) e->data.walk[e->data.walks++] = (totk_ft_walk_record){begin, end, cpu};
        else e->data.lost_walks++;
    }
    unlock_entry(e);
}
void totk_ft_walk_wait(uint64_t f, uint64_t ns) {
    ft_entry *e = entry(f); if (!e) return; e->data.join_ns += ns; unlock_entry(e);
}
void totk_ft_acquire(uint64_t f, uint64_t begin, uint64_t end) {
    ft_entry *e = entry(f); if (!e) return;
    if (!e->data.acquire_begin || begin < e->data.acquire_begin) e->data.acquire_begin = begin;
    if (end > e->data.acquire_end) e->data.acquire_end = end;
    unlock_entry(e);
}
static totk_ft_slot_record *slot(totk_ft_record *r, uint64_t ticket) {
    for (unsigned i = 0; i < r->slots; i++) if (r->slot[i].ticket == ticket) return &r->slot[i];
    if (r->slots == FT_SLOTS) { r->lost_slots++; return NULL; }
    totk_ft_slot_record *s = &r->slot[r->slots++]; s->ticket = ticket; return s;
}
void totk_ft_slot_seal(uint64_t f, uint64_t ticket, uint64_t now) {
    ft_entry *e = entry(f);
    if (e) {
        totk_ft_slot_record *s = slot(&e->data, ticket); if (s) s->seal = now;
        unlock_entry(e);
    }
    totk_ff_ticket_seal(f, ticket, now);
}
void totk_ft_slot_queue(uint64_t f, uint64_t ticket, uint64_t begin, uint64_t end) {
    ft_entry *e = entry(f);
    if (e) {
        totk_ft_slot_record *s = slot(&e->data, ticket); if (s) { s->queue_begin = begin; s->queue_end = end; }
        unlock_entry(e);
    }
    totk_ff_ticket_queue(f, ticket, begin, end);
}
void totk_ft_slot_gpu(uint64_t f, uint64_t ticket, uint64_t done, uint64_t gpu_ns) {
    ft_entry *e = entry(f);
    if (e) {
        totk_ft_slot_record *s = slot(&e->data, ticket); if (s) { s->gpu_done = done; s->gpu_ns = gpu_ns; }
        unlock_entry(e);
    }
    totk_ff_ticket_gpu(f, ticket, done);
}
void totk_ft_handoff(uint64_t f, uint64_t now) {
    ft_entry *e = entry(f); if (!e) return; e->data.handoff = now; unlock_entry(e);
}
void totk_ft_compositor(uint64_t f, uint64_t apply, uint64_t display) {
    ft_entry *e = entry(f); if (!e) return;
    if (apply) e->data.apply = apply;
    if (display) e->data.display = display;
    unlock_entry(e);
}
void totk_ft_present_slot(uint64_t f, uint64_t desired, uint64_t skipped) {
    ft_entry *e = entry(f); if (!e) return;
    e->data.present_slot = desired; e->data.present_skipped = skipped;
    unlock_entry(e);
}
void totk_ft_present_ready(uint64_t f, uint64_t ready) {
    ft_entry *e = entry(f); if (!e) return;
    e->data.present_ready = ready; unlock_entry(e);
}
void totk_ft_submission(uint64_t f, uint64_t now, unsigned source) {
    if (!now || (source != FT_SUBMIT_COPY_POST && source != FT_SUBMIT_AHB_APPLY)) return;
    ft_entry *e = entry(f); if (!e) return;
    e->data.submit = now; e->data.submit_source = source;
    unlock_entry(e);
}
static uint64_t span(uint64_t a, uint64_t b) { return a && b >= a ? b - a : 0; }
static double offset_ms(uint64_t a, uint64_t base) { return a && base ? ((double)a - (double)base) / 1e6 : NAN; }
typedef struct {
    uint64_t first_walk, last_walk, first_queue, last_queue, last_gpu;
    uint64_t submit_latency, driver_ns, gpu_ns, gpu_tail, gpu_backlog, acquire_ns, present_delay;
    int gpu_complete;
} ft_metrics;
static ft_metrics metrics(const totk_ft_record *r) {
    ft_metrics m = {0}; const totk_ft_slot_record *last = NULL;
    for (unsigned i = 0; i < r->walks; i++) {
        if (!m.first_walk || r->walk[i].begin < m.first_walk) m.first_walk = r->walk[i].begin;
        if (r->walk[i].end > m.last_walk) m.last_walk = r->walk[i].end;
    }
    m.gpu_complete = r->slots != 0;
    for (unsigned i = 0; i < r->slots; i++) {
        const totk_ft_slot_record *s = &r->slot[i];
        if (!last || s->ticket > last->ticket) last = s;
        if (s->queue_begin && (!m.first_queue || s->queue_begin < m.first_queue)) m.first_queue = s->queue_begin;
        if (s->queue_end > m.last_queue) m.last_queue = s->queue_end;
        uint64_t latency = span(s->seal, s->queue_end);
        if (latency > m.submit_latency) m.submit_latency = latency;
        m.driver_ns += span(s->queue_begin, s->queue_end); m.gpu_ns += s->gpu_ns;
        if (!s->gpu_done) m.gpu_complete = 0;
    }
    if (last) {
        m.last_gpu = last->gpu_done; m.gpu_tail = span(last->queue_end, last->gpu_done);
        m.gpu_backlog = last->gpu_ns && m.gpu_tail > last->gpu_ns ? m.gpu_tail - last->gpu_ns : 0;
    }
    m.acquire_ns = span(r->acquire_begin, r->acquire_end);
    m.present_delay = span(r->handoff, r->apply);
    return m;
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y);
}
static void distribution(FILE *out, const char *name, uint64_t first, uint64_t last, int which) {
    uint64_t values[300], sum = 0; unsigned n = 0, buckets[6] = {0};
    totk_ft_record prev = {0}, r;
    snapshot(first - 1, &prev);
    for (uint64_t f = first; f <= last; f++) {
        snapshot(f, &r);
        uint64_t a = which == 0 ? prev.present : which == 1 ? prev.apply : prev.display;
        uint64_t b = which == 0 ? r.present : which == 1 ? r.apply : r.display;
        if (prev.frame + 1 == r.frame && a && b > a && n < 300) {
            uint64_t d = b - a, target = r.target ? r.target : 33333333;
            values[n++] = d; sum += d;
            int64_t excess = (int64_t)d - (int64_t)target;
            unsigned bucket = excess < -4000000 ? 0 : excess <= 4000000 ? 1 : excess <= 12000000 ? 2 : excess <= 20000000 ? 3 : excess <= 36000000 ? 4 : 5;
            buckets[bucket]++;
        }
        prev = r;
    }
    if (!n) { fprintf(out, "[frame-tl dist] frames=%llu..%llu clock=%s n=0 missing\n", (unsigned long long)first, (unsigned long long)last, name); return; }
    qsort(values, n, sizeof values[0], cmp_u64);
    fprintf(out, "[frame-tl dist] frames=%llu..%llu clock=%s n=%u mean=%.3f min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f ms delta_target_bins(<-4,[-4,4],(4,12],(12,20],(20,36],>36)=%u/%u/%u/%u/%u/%u\n",
            (unsigned long long)first, (unsigned long long)last, name, n, sum / (double)n / 1e6, values[0] / 1e6,
            values[(n - 1) * 50 / 100] / 1e6, values[(n - 1) * 95 / 100] / 1e6, values[(n - 1) * 99 / 100] / 1e6, values[n - 1] / 1e6,
            buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);
}
enum { FT_INPUT_SUBMIT_FIRST, FT_INPUT_SUBMIT_LAST, FT_INPUT_COPY_FIRST, FT_INPUT_COPY_LAST,
       FT_INPUT_DISPLAY_FIRST, FT_INPUT_DISPLAY_LAST, FT_INPUT_LATENCIES };
enum { FT_INPUT_OK, FT_INPUT_MISSING_HANDOFF, FT_INPUT_NO_READ, FT_INPUT_MISSING_ENDPOINT,
       FT_INPUT_INVALID, FT_INPUT_OTHER_SOURCE };
static int input_latency(const totk_ft_record *r, unsigned main_wake, unsigned which, uint64_t *ns) {
    *ns = 0;
    if (main_wake && !r->input_main_wake_received) return FT_INPUT_MISSING_HANDOFF;
    const totk_ft_input_reads *in = main_wake ? &r->input_main_wake.reads : &r->input_window;
    if (!in->reads) return FT_INPUT_NO_READ;
    uint64_t begin = which & 1 ? in->last : in->first;
    unsigned expected_source = which >= FT_INPUT_COPY_FIRST && which < FT_INPUT_DISPLAY_FIRST ?
                               FT_SUBMIT_COPY_POST : FT_SUBMIT_AHB_APPLY;
    if (!r->submit || !r->submit_source) return FT_INPUT_MISSING_ENDPOINT;
    if (r->submit_source != expected_source) return FT_INPUT_OTHER_SOURCE;
    uint64_t end = which < FT_INPUT_DISPLAY_FIRST ? r->submit : r->display;
    if (!end) return FT_INPUT_MISSING_ENDPOINT;
    if (!begin || end < begin ||
        (which >= FT_INPUT_DISPLAY_FIRST && end < r->submit) ||
        (main_wake && (!r->ready || r->wake < r->ready || r->submit < r->wake ||
                       in->last > r->ready || in->mixed_threads || in->unknown_thread_reads ||
                       in->first_thread != r->wake_waker || in->last_thread != r->wake_waker))) return FT_INPUT_INVALID;
    *ns = end - begin;
    return FT_INPUT_OK;
}
typedef struct {
    uint64_t values[300];
    unsigned n, missing_frame, missing_handoff, no_read, missing_endpoint, invalid, other_source;
} ft_input_distribution;
static void input_distribution_add(ft_input_distribution *d, int status, uint64_t ns) {
    switch (status) {
    case FT_INPUT_OK: if (d->n < 300) d->values[d->n++] = ns; break;
    case FT_INPUT_MISSING_HANDOFF: d->missing_handoff++; break;
    case FT_INPUT_NO_READ: d->no_read++; break;
    case FT_INPUT_MISSING_ENDPOINT: d->missing_endpoint++; break;
    case FT_INPUT_INVALID: d->invalid++; break;
    case FT_INPUT_OTHER_SOURCE: d->other_source++; break;
    }
}
static void input_report(FILE *out, uint64_t first, uint64_t last) {
    static const char *const associations[] = {"global-window-observation", "main-wake-snapshot"};
    static const char *const names[] = {"first-read-to-compositor-submit", "last-read-to-compositor-submit",
                                       "first-read-to-copy-buffer-post", "last-read-to-copy-buffer-post",
                                       "first-read-to-AHB-present-fence", "last-read-to-AHB-present-fence"};
    /* A main wake carries only new sender reads, never a retained boot sample.
     * Raw observations remain separate and do not fill missing sender evidence. */
    ft_input_distribution dist[2][FT_INPUT_LATENCIES] = {0};
    unsigned submit_sources[3] = {0}, missing_submission = 0;
    totk_ft_record r;
    for (uint64_t f = first; f <= last; f++) {
        int found = snapshot(f, &r);
        if (found) {
            if (r.submit && r.submit_source >= FT_SUBMIT_COPY_POST && r.submit_source <= FT_SUBMIT_AHB_APPLY)
                submit_sources[r.submit_source]++;
            else missing_submission++;
        }
        for (unsigned source = 0; source < 2; source++) {
            double ms[FT_INPUT_LATENCIES];
            for (unsigned which = 0; which < FT_INPUT_LATENCIES; which++) {
                ft_input_distribution *d = &dist[source][which];
                ms[which] = NAN;
                if (!found) { d->missing_frame++; continue; }
                uint64_t ns;
                int status = input_latency(&r, source, which, &ns);
                input_distribution_add(d, status, ns);
                if (status == FT_INPUT_OK) ms[which] = ns / 1e6;
            }
            if (!found) continue;
            const totk_ft_input_reads *in = source ? &r.input_main_wake.reads : &r.input_window;
            unsigned si = r.submit_source == FT_SUBMIT_COPY_POST ? FT_INPUT_COPY_FIRST : FT_INPUT_SUBMIT_FIRST;
            const char *submit_source = r.submit_source == FT_SUBMIT_COPY_POST ? "copy-buffer-post" :
                                        r.submit_source == FT_SUBMIT_AHB_APPLY ? "AHB-apply" : "missing";
            fprintf(out, "[frame-tl input] frame=%llu association=%s handoff=%s reads=%llu first_ns=%llu last_ns=%llu first_sampling=%llu last_sampling=%llu first_guest_thread=%#x last_guest_thread=%#x mixed_threads=%u unknown_thread_reads=%llu invalid_reads=%llu submit_ns=%llu submit_source=%s first_to_submit_ms=%.3f last_to_submit_ms=%.3f first_to_AHB_fence_ms=%.3f last_to_AHB_fence_ms=%.3f\n",
                    (unsigned long long)f, associations[source],
                    source ? (r.input_main_wake_received ? "observed" : "missing") : "not-applicable",
                    (unsigned long long)in->reads,
                    (unsigned long long)in->first, (unsigned long long)in->last,
                    (unsigned long long)in->first_sampling, (unsigned long long)in->last_sampling,
                    in->first_thread, in->last_thread, in->mixed_threads,
                    (unsigned long long)in->unknown_thread_reads, (unsigned long long)in->invalid_reads,
                    (unsigned long long)r.submit, submit_source,
                    ms[si], ms[si + 1], ms[FT_INPUT_DISPLAY_FIRST], ms[FT_INPUT_DISPLAY_LAST]);
        }
    }
    fprintf(out, "[frame-tl input-sources] frames=%llu..%llu AHB_apply=%u copy_buffer_post=%u missing_submission=%u; compositor-submit=AHB-apply copy-buffer-post=successful-ANativeWindow-post display=AHB-present-fence-only\n",
            (unsigned long long)first, (unsigned long long)last,
            submit_sources[FT_SUBMIT_AHB_APPLY], submit_sources[FT_SUBMIT_COPY_POST], missing_submission);
    for (unsigned source = 0; source < 2; source++) for (unsigned which = 0; which < FT_INPUT_LATENCIES; which++) {
        ft_input_distribution *d = &dist[source][which];
        qsort(d->values, d->n, sizeof d->values[0], cmp_u64);
        fprintf(out, "[frame-tl input-dist] frames=%llu..%llu association=%s metric=%s n=%u p50=%.3f p95=%.3f ms missing_frame=%u missing_handoff=%u no_read=%u missing_endpoint=%u invalid=%u read_group=all other_source=%u\n",
                (unsigned long long)first, (unsigned long long)last, associations[source], names[which], d->n,
                d->n ? d->values[(d->n - 1) * 50 / 100] / 1e6 : NAN,
                d->n ? d->values[(d->n - 1) * 95 / 100] / 1e6 : NAN,
                d->missing_frame, d->missing_handoff, d->no_read, d->missing_endpoint, d->invalid, d->other_source);
    }
}
/* Retrospective sampling at each immutable game slot, after the existing
 * 30-frame reporting delay. Count FIFO-ready successors AFTER serving the due
 * picture, not pending credits (which can include unfinished GPU work).
 * No callback/GPU-lock nesting or extra readiness polls on the render path. */
static void reserve_report(FILE *out, uint64_t first, uint64_t last) {
    uint64_t depth[3] = {0}, skipped = 0;
    unsigned absorbed = 0, shown = 0, late_ready = 0, missing = 0, incomplete = 0;
    unsigned display_pairs = 0, display_hitches = 0;
    totk_ft_record r, next, prev = {0};
    snapshot(first - 1, &prev);
    for (uint64_t f = first; f <= last; f++) {
        if (!snapshot(f, &r)) { missing++; prev = (totk_ft_record){0}; continue; }
        if (prev.frame + 1 == f && prev.submit_source == FT_SUBMIT_AHB_APPLY &&
            r.submit_source == FT_SUBMIT_AHB_APPLY && prev.display && r.display > prev.display) {
            display_pairs++;
            uint64_t delta = r.display - prev.display;
            display_hitches += delta > r.target && delta - r.target > 4000000;
        }
        skipped += r.present_skipped;
        depth[0] += r.present_skipped;
        if (!r.present_slot || !r.present_ready || !r.handoff) { missing++; prev = r; continue; }
        uint64_t ready = r.present_ready > r.handoff ? r.present_ready : r.handoff;
        unsigned banked = 0;
        if (ready <= r.present_slot) {
            for (unsigned ahead = 1; ahead <= 2; ahead++) {
                if (!snapshot(f + ahead, &next) || !next.present_slot ||
                    !next.present_ready || !next.handoff) { incomplete++; break; }
                if (next.present_slot <= r.present_slot || next.present_ready > r.present_slot ||
                    next.handoff > r.present_slot) break;
                banked++;
            }
        } else late_ready++;
        depth[banked]++;
        /* Same >4ms game-interval threshold as the timeline late rows.
         * This is a readiness proxy, NOT a counterfactual scanout claim. */
        if (prev.frame + 1 == f && prev.present && r.present > prev.present &&
            r.present - prev.present > r.target && r.present - prev.present - r.target > 4000000) {
            if (ready <= r.present_slot) absorbed++; else shown++;
        }
        prev = r;
    }
    fprintf(out, "[frame-tl reserve] frames=%llu..%llu banked-after-slot(0/1/2)=%llu/%llu/%llu hitches(absorbed/shown)=%u/%u late-ready=%u skipped-slots=%llu missing=%u incomplete-lookahead=%u display-interval-hitches/pairs=%u/%u; FIFO handoff+GPU-publication readiness proxy, NOT display; display intervals use AHB fences only; added latency versus slack1: compare [frame-tl input-dist] matched A/B\n",
            (unsigned long long)first, (unsigned long long)last,
            (unsigned long long)depth[0], (unsigned long long)depth[1], (unsigned long long)depth[2],
            absorbed, shown, late_ready, (unsigned long long)skipped, missing, incomplete,
            display_hitches, display_pairs);
}
#define FT_COUNTER_NAME(symbol, label) #label,
static const char *const counter_names[FT_COUNT_N] = { FT_COUNTERS(FT_COUNTER_NAME) };
#undef FT_COUNTER_NAME
static void work_row(FILE *out, const totk_ft_work_counts *counts, double divisor) {
    for (unsigned i = 0; i < FT_COUNT_N; i++) {
        if (divisor) fprintf(out, " %s=%.3f", counter_names[i], counts->value[i] / divisor);
        else fprintf(out, " %s=%llu", counter_names[i], (unsigned long long)counts->value[i]);
    }
}
static void report(FILE *out, uint64_t first, uint64_t last) {
    static const char *names[] = {"wake", "walk", "submit", "gpu", "acquire", "present", "unknown"};
    unsigned dominant[7] = {0}, late = 0, missing = 0, overflow = 0;
    totk_ft_work_counts work_sum[2] = {{{0}}, {{0}}};
    unsigned work_frames[2] = {0};
    uint64_t pacing_sum[2] = {0}, texture_sum[2] = {0};
    distribution(out, "game", first, last, 0); distribution(out, "compositor-submit", first, last, 1); distribution(out, "display", first, last, 2);
    input_report(out, first, last);
    reserve_report(out, first, last);
    totk_ft_record prev = {0}, r; snapshot(first - 1, &prev);
    for (uint64_t f = first; f <= last; f++) {
        if (!snapshot(f, &r)) { missing++; memset(&prev, 0, sizeof prev); continue; }
        if (prev.frame + 1 != f) { prev = r; continue; }
        uint64_t target = r.target ? r.target : 33333333, game = span(prev.present, r.present), apply = span(prev.apply, r.apply), display = span(prev.display, r.display);
        overflow += !!(r.lost_walks || r.lost_slots);
        int is_late = game > target + 4000000 || apply > target + 4000000 || display > target + 4000000;
        /* Means include only complete game intervals, with the identical late
         * classification used by detailed rows (any observed clock is late). */
        if (prev.present && r.present && game) {
            unsigned group = !!is_late; work_frames[group]++;
            for (unsigned i = 0; i < FT_COUNT_N; i++) work_sum[group].value[i] += r.work.value[i];
            pacing_sum[group] += r.acquire_pacing_ns; texture_sum[group] += r.acquire_texture_ns;
        }
        if (!is_late) { prev = r; continue; }
        ft_metrics m = metrics(&r), p = metrics(&prev);
        uint64_t growth[6] = {0};
        uint64_t wake_gap = span(prev.wake, r.wake);
        growth[0] = wake_gap > target ? wake_gap - target : 0;
        growth[1] = r.walk_ns > prev.walk_ns ? r.walk_ns - prev.walk_ns : 0;
        growth[2] = m.submit_latency > p.submit_latency ? m.submit_latency - p.submit_latency : 0;
        uint64_t gpu = m.gpu_tail > m.gpu_ns ? m.gpu_tail : m.gpu_ns, oldgpu = p.gpu_tail > p.gpu_ns ? p.gpu_tail : p.gpu_ns;
        growth[3] = m.gpu_complete && p.gpu_complete && gpu > oldgpu ? gpu - oldgpu : 0;
        growth[4] = m.acquire_ns > p.acquire_ns ? m.acquire_ns - p.acquire_ns : 0;
        growth[5] = m.present_delay > p.present_delay ? m.present_delay - p.present_delay : 0;
        unsigned stage = 6; uint64_t largest = 0;
        for (unsigned i = 0; i < 6; i++) if (growth[i] > largest) { largest = growth[i]; stage = i; }
        dominant[stage]++; late++;
        uint64_t offcpu = r.walk_ns > r.walk_cpu_ns ? r.walk_ns - r.walk_cpu_ns : 0;
        flockfile(out);
        fprintf(out, "[frame-tl late] frame=%llu target=%.3f game_iv=%.3f apply_iv=%.3f display_iv=%.3f dominant=%s growth=%.3f ms wake_gap=%.3f dispatch=%.3f walk=%.3f offcpu=%.3f join=%.3f submit_latency=%.3f queue_cpu=%.3f gpu=%.3f gpu_tail=%.3f gpu_backlog_or_observe=%.3f gpu_complete=%d acquire=%.3f present_delay=%.3f wake_addr=%#llx waker=%#llx relative_prev_game_ms(wake,ready,walk_begin,walk_end,queue_begin,queue_end,gpu,acquire_in,acquire_out,handoff,apply,display)=[%+.3f,%+.3f,%+.3f,%+.3f,%+.3f,%+.3f,%+.3f,%+.3f,%+.3f,%+.3f,%+.3f,%+.3f] walks=",
                (unsigned long long)f, target / 1e6, game ? game / 1e6 : NAN, apply ? apply / 1e6 : NAN, display ? display / 1e6 : NAN,
                names[stage], largest / 1e6, wake_gap ? wake_gap / 1e6 : NAN, span(r.ready, r.wake) / 1e6, r.walk_ns / 1e6, offcpu / 1e6, r.join_ns / 1e6,
                m.submit_latency / 1e6, m.driver_ns / 1e6, m.gpu_ns / 1e6, m.gpu_tail / 1e6, m.gpu_backlog / 1e6, m.gpu_complete, m.acquire_ns / 1e6, m.present_delay / 1e6,
                (unsigned long long)r.wake_addr, (unsigned long long)r.wake_waker,
                offset_ms(r.wake, prev.present), offset_ms(r.ready, prev.present), offset_ms(m.first_walk, prev.present), offset_ms(m.last_walk, prev.present),
                offset_ms(m.first_queue, prev.present), offset_ms(m.last_queue, prev.present), offset_ms(m.last_gpu, prev.present), offset_ms(r.acquire_begin, prev.present),
                offset_ms(r.acquire_end, prev.present), offset_ms(r.handoff, prev.present), offset_ms(r.apply, prev.present), offset_ms(r.display, prev.present));
        for (unsigned i = 0; i < r.walks; i++) fprintf(out, "%s[%.3f,%.3f,cpu=%.3f]", i ? "," : "", offset_ms(r.walk[i].begin, prev.present), offset_ms(r.walk[i].end, prev.present), r.walk[i].cpu / 1e6);
        fprintf(out, " slots=");
        for (unsigned i = 0; i < r.slots; i++) {
            const totk_ft_slot_record *s = &r.slot[i];
            fprintf(out, "%s[%llu:%.3f,%.3f,%.3f,%.3f,gpu=%.3f]", i ? "," : "", (unsigned long long)s->ticket,
                    offset_ms(s->seal, prev.present), offset_ms(s->queue_begin, prev.present), offset_ms(s->queue_end, prev.present), offset_ms(s->gpu_done, prev.present), s->gpu_ns / 1e6);
        }
        work_row(out, &r.work, 0);
        fprintf(out, " acquire_pacing=%.3f acquire_texture=%.3f lost_walks=%u lost_slots=%u\n",
                r.acquire_pacing_ns / 1e6, r.acquire_texture_ns / 1e6, r.lost_walks, r.lost_slots); funlockfile(out);
        prev = r;
    }
    fprintf(out, "[frame-tl summary] frames=%llu..%llu late=%u dominant(wake/walk/submit/gpu/acquire/present/unknown)=%u/%u/%u/%u/%u/%u/%u missing=%u overflow_frames=%u expired_events=%llu attribution=largest-positive-growth-vs-previous-frame-not-causality\n",
            (unsigned long long)first, (unsigned long long)last, late, dominant[0], dominant[1], dominant[2], dominant[3], dominant[4], dominant[5], dominant[6], missing, overflow, (unsigned long long)atomic_load(&expired));
    for (unsigned group = 0; group < 2; group++) {
        flockfile(out);
        fprintf(out, "[frame-tl work] frames=%llu..%llu class=%s n=%u means_per_frame",
                (unsigned long long)first, (unsigned long long)last, group ? "late" : "on-time", work_frames[group]);
        work_row(out, &work_sum[group], work_frames[group] ? (double)work_frames[group] : 1.0);
        fprintf(out, " acquire_pacing_ms=%.3f acquire_texture_ms=%.3f%s\n",
                work_frames[group] ? pacing_sum[group] / (double)work_frames[group] / 1e6 : 0,
                work_frames[group] ? texture_sum[group] / (double)work_frames[group] / 1e6 : 0,
                work_frames[group] ? "" : " missing");
        funlockfile(out);
    }
    totk_ff_report(out, first, last);
}
#ifndef TOTK_FRAME_TIMELINE_TEST
static void *report_worker(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "totk-frame-tl");
    uint64_t first = 1;
    for (;;) {
        while (sem_wait(&report_sem) && errno == EINTR) {}
        uint64_t until = atomic_load_explicit(&report_until, memory_order_acquire);
        while (first + 299 <= until) {
            report(stderr, first, first + 299); first += 300;
        }
    }
    return NULL;
}
#endif
void totk_ft_present(uint64_t f, uint64_t target, uint64_t now) {
    ft_entry *e = entry(f); if (!e) return;
    e->data.present = now; e->data.target = target; unlock_entry(e);
    totk_ff_present(f, now);
    atomic_store_explicit(&current, f + 1, memory_order_relaxed);
#ifndef TOTK_FRAME_TIMELINE_TEST
    if (f > 30 && (f - 30) % 300 == 0) {
        atomic_store_explicit(&report_until, f - 30, memory_order_release);
        sem_post(&report_sem);
    }
#endif
}
#ifdef TOTK_FRAME_TIMELINE_TEST
int totk_ft_snapshot(uint64_t f, totk_ft_record *out) { return snapshot(f, out); }
void totk_ft_test_report(FILE *out, uint64_t first, uint64_t last) { report(out, first, last); }
#endif
#endif
