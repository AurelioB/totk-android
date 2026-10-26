#include "frame_fence.h"
#if defined(__ANDROID__) || defined(TOTK_FRAME_TIMELINE_TEST)
#include "frame_timeline.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* Independent of recyclable backend slots. At 64 submissions/frame the ticket
 * history lasts as long as the frame history. No allocation, worker or I/O on
 * producers. Bounded lock attempts drop evidence rather than delaying a fence;
 * every such loss is reported. No lock is held across another lock or I/O. */
enum { FF_TICKETS = FT_RING * FT_SLOTS, FF_SIGNAL_BUCKETS = FT_RING,
       FF_SIGNAL_WAYS = 8, FF_WAITS = 8, FF_LOCK_TRIES = 64, FF_GROUPS = 4 };
typedef struct { _Atomic unsigned lock; totk_ff_ticket_record data; } ff_ticket_entry;
typedef struct {
    _Atomic unsigned lock;
    totk_ff_signal_record data[FF_SIGNAL_WAYS];
} ff_signal_entry;
typedef struct {
    totk_ff_wait_record wait;
    totk_ff_signal_record signal;
    unsigned signal_status;
} ff_wait;
typedef struct {
    uint64_t frame, present_ns;
    unsigned waits, lost_waits;
    ff_wait wait[FF_WAITS];
} ff_frame;
typedef struct { _Atomic unsigned lock; ff_frame data; } ff_frame_entry;
static ff_ticket_entry tickets[FF_TICKETS];
static ff_signal_entry signals[FF_SIGNAL_BUCKETS];
static ff_frame_entry frames[FT_RING];
static _Atomic uint64_t busy_events, stale_tickets, stale_frames, stale_signals;
static _Atomic uint64_t ticket_evictions, signal_evictions, wait_overflow, conflicts;

static int take(_Atomic unsigned *lock) {
    for (unsigned i = 0; i < FF_LOCK_TRIES; i++) {
        unsigned expected = 0;
        if (atomic_compare_exchange_weak_explicit(lock, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) return 1;
    }
    atomic_fetch_add_explicit(&busy_events, 1, memory_order_relaxed);
    return 0;
}
static void release(_Atomic unsigned *lock) { atomic_store_explicit(lock, 0, memory_order_release); }
static ff_frame_entry *frame_entry(uint64_t frame) {
    if (!frame || !totk_ft_on()) return NULL;
    ff_frame_entry *e = &frames[frame % FT_RING];
    if (!take(&e->lock)) return NULL;
    if (e->data.frame > frame) {
        atomic_fetch_add_explicit(&stale_frames, 1, memory_order_relaxed);
        release(&e->lock); return NULL;
    }
    if (e->data.frame != frame) {
        memset(&e->data, 0, sizeof e->data); e->data.frame = frame;
    }
    return e;
}
static unsigned frame_snapshot(uint64_t frame, ff_frame *out) {
    memset(out, 0, sizeof *out);
    if (!frame) return FF_META_MISSING;
    ff_frame_entry *e = &frames[frame % FT_RING];
    if (!take(&e->lock)) return FF_META_BUSY;
    unsigned status = e->data.frame == frame ? FF_META_OK :
                      e->data.frame > frame ? FF_META_STALE : FF_META_MISSING;
    if (status == FF_META_OK) *out = e->data;
    release(&e->lock); return status;
}
static ff_ticket_entry *ticket_entry(uint64_t ticket, uint64_t frame) {
    if (!ticket || !totk_ft_on()) return NULL;
    ff_ticket_entry *e = &tickets[ticket % FF_TICKETS];
    if (!take(&e->lock)) return NULL;
    if (e->data.ticket > ticket) {
        atomic_fetch_add_explicit(&stale_tickets, 1, memory_order_relaxed);
        release(&e->lock); return NULL;
    }
    if (e->data.ticket != ticket) {
        if (e->data.ticket) atomic_fetch_add_explicit(&ticket_evictions, 1, memory_order_relaxed);
        memset(&e->data, 0, sizeof e->data); e->data.ticket = ticket;
    }
    if (frame) {
        if (e->data.frame && e->data.frame != frame) {
            e->data.conflict = 1;
            atomic_fetch_add_explicit(&conflicts, 1, memory_order_relaxed);
            release(&e->lock); return NULL;
        }
        e->data.frame = frame;
    }
    return e;
}
static unsigned ticket_snapshot(uint64_t ticket, totk_ff_ticket_record *out) {
    memset(out, 0, sizeof *out);
    if (!ticket) return FF_META_EMPTY;
    ff_ticket_entry *e = &tickets[ticket % FF_TICKETS];
    if (!take(&e->lock)) return FF_META_BUSY;
    unsigned status = e->data.ticket == ticket ? FF_META_OK :
                      e->data.ticket > ticket ? FF_META_STALE : FF_META_MISSING;
    if (status == FF_META_OK) {
        *out = e->data;
        if (out->conflict) status = FF_META_CONFLICT;
    }
    release(&e->lock); return status;
}
static unsigned signal_bucket(uint64_t sync) {
    uint64_t key = sync >> 3;
    key ^= key >> 17; key ^= key >> 31;
    return (unsigned)(key % FF_SIGNAL_BUCKETS);
}
void totk_ff_signal(uint64_t sync, uint64_t frame, uint64_t ticket, uint64_t now,
                    uint64_t guest_thread, unsigned source, unsigned condition, unsigned flags) {
    if (!sync || !now || !totk_ft_on()) return;
    ff_signal_entry *e = &signals[signal_bucket(sync)];
    if (!take(&e->lock)) return;
    unsigned oldest = 0;
    for (unsigned i = 0; i < FF_SIGNAL_WAYS; i++) {
        if (e->data[i].time_ns < e->data[oldest].time_ns) oldest = i;
        if (e->data[i].sync == sync && e->data[i].time_ns == now) {
            /* An indistinguishable duplicate must not silently change source. */
            if (e->data[i].ticket != ticket || e->data[i].source != source ||
                e->data[i].frame != frame || e->data[i].guest_thread != guest_thread ||
                e->data[i].condition != condition || e->data[i].flags != flags) {
                e->data[i].source = 0;
                atomic_fetch_add_explicit(&conflicts, 1, memory_order_relaxed);
            }
            release(&e->lock); return;
        }
    }
    if (e->data[oldest].time_ns && now <= e->data[oldest].time_ns) {
        atomic_fetch_add_explicit(&stale_signals, 1, memory_order_relaxed);
        release(&e->lock); return;
    }
    if (e->data[oldest].time_ns) atomic_fetch_add_explicit(&signal_evictions, 1, memory_order_relaxed);
    e->data[oldest] = (totk_ff_signal_record){sync, frame, ticket, now, guest_thread, source, condition, flags};
    release(&e->lock);
}
static unsigned signal_snapshot(const totk_ff_wait_record *wait, totk_ff_signal_record *out) {
    memset(out, 0, sizeof *out);
    if (!wait->sync || !wait->enter_ns) return FF_META_MISSING;
    ff_signal_entry *e = &signals[signal_bucket(wait->sync)];
    if (!take(&e->lock)) return FF_META_BUSY;
    for (unsigned i = 0; i < FF_SIGNAL_WAYS; i++) {
        const totk_ff_signal_record *s = &e->data[i];
        if (s->sync == wait->sync && s->time_ns <= wait->enter_ns && s->time_ns > out->time_ns) *out = *s;
    }
    release(&e->lock);
    if (!out->time_ns) return FF_META_MISSING;
    if (out->ticket != wait->raw_ticket) return FF_META_TICKET_MISMATCH;
    return out->source ? FF_META_OK : FF_META_CONFLICT;
}
void totk_ff_wait(const totk_ff_wait_record *record) {
    if (!record || !record->frame || !totk_ft_on()) return;
    ff_wait w = {.wait = *record};
    /* Capture the setter at wait time: later reuse of an NVN sync cannot
     * replace this wait's source with a future setter at reporting time. */
    w.signal_status = signal_snapshot(record, &w.signal);
    ff_frame_entry *e = frame_entry(record->frame); if (!e) return;
    if (e->data.waits < FF_WAITS) e->data.wait[e->data.waits++] = w;
    else { e->data.lost_waits++; atomic_fetch_add_explicit(&wait_overflow, 1, memory_order_relaxed); }
    release(&e->lock);
}
void totk_ff_ticket_seal(uint64_t frame, uint64_t ticket, uint64_t now) {
    ff_ticket_entry *e = ticket_entry(ticket, frame); if (!e) return;
    e->data.seal_ns = now; e->data.seen |= FF_SEEN_SEAL; release(&e->lock);
}
void totk_ff_ticket_queue(uint64_t frame, uint64_t ticket, uint64_t begin, uint64_t end) {
    ff_ticket_entry *e = ticket_entry(ticket, frame); if (!e) return;
    e->data.queue_begin_ns = begin; e->data.queue_end_ns = end; e->data.seen |= FF_SEEN_QUEUE;
    release(&e->lock);
}
void totk_ff_ticket_gpu(uint64_t frame, uint64_t ticket, uint64_t done) {
    ff_ticket_entry *e = ticket_entry(ticket, frame); if (!e) return;
    e->data.gpu_observed_ns = done; e->data.seen |= FF_SEEN_GPU; release(&e->lock);
}
void totk_ff_ticket_publish(uint64_t ticket, uint64_t now, unsigned post_ops) {
    ff_ticket_entry *e = ticket_entry(ticket, 0); if (!e) return;
    e->data.publish_ns = now; e->data.post_ops = post_ops; e->data.seen |= FF_SEEN_PUBLISH;
    release(&e->lock);
}
void totk_ff_present(uint64_t frame, uint64_t now) {
    ff_frame_entry *e = frame_entry(frame); if (!e) return;
    e->data.present_ns = now; release(&e->lock);
}
static unsigned completion(const totk_ff_snapshot *s) {
    const totk_ff_wait_record *w = &s->wait;
    const totk_ff_ticket_record *t = &s->effective;
    int ticket_source = s->signal.source == FF_SIGNAL_QUEUE ||
        (s->signal.source == FF_SIGNAL_ACQUIRE && (w->raw_ticket || w->translated));
    if (!w->raw_fenced) return FF_COMPLETE_UNFENCED;
    if (!w->satisfied) return FF_COMPLETE_TIMEOUT;
    if (w->immediate) return FF_COMPLETE_IMMEDIATE;
    if (!w->enter_ns || w->exit_ns < w->enter_ns || s->signal_status != FF_META_OK ||
        !ticket_source || !w->waited_ticket || s->effective_status != FF_META_OK ||
        !(t->seen & FF_SEEN_GPU) || !t->gpu_observed_ns) return FF_COMPLETE_UNKNOWN;
    if (t->gpu_observed_ns > w->enter_ns && t->gpu_observed_ns <= w->exit_ns)
        return FF_COMPLETE_GPU_PROGRESS;
    if (t->gpu_observed_ns <= w->enter_ns && (t->seen & FF_SEEN_PUBLISH) &&
        t->publish_ns > w->enter_ns && t->publish_ns <= w->exit_ns)
        return FF_COMPLETE_CPU_PUBLICATION;
    return FF_COMPLETE_UNKNOWN;
}
static void evaluate(const ff_frame *frame, const ff_wait *wait, uint64_t prev, totk_ff_snapshot *out) {
    memset(out, 0, sizeof *out);
    out->wait = wait->wait; out->signal = wait->signal; out->signal_status = wait->signal_status;
    out->game_present_ns = frame->present_ns; out->prev_game_present_ns = prev;
    if (out->wait.translated) {
        /* Translation-submit tokens and GPU tickets are separate namespaces. */
        out->raw_status = out->effective_status = FF_META_TRANSLATION_TOKEN;
    } else {
        out->raw_status = ticket_snapshot(out->wait.raw_ticket, &out->raw);
        if (out->wait.raw_ticket == out->wait.waited_ticket) {
            out->effective = out->raw; out->effective_status = out->raw_status;
        } else out->effective_status = ticket_snapshot(out->wait.waited_ticket, &out->effective);
    }
    out->completion = completion(out);
}
static const char *const statuses[] = {"unavailable", "ok", "empty", "stale", "busy", "conflict", "ticket-mismatch", "translation-token-not-GPU-ticket"};
static const char *const completions[] = {"immediate-already-satisfied", "gpu-progress-observed", "gpu-already-observed-cpu-publication-pending", "timeout", "unfenced", "unknown"};
static const char *source(const totk_ff_snapshot *s) {
    if (s->signal_status != FF_META_OK) return "unknown";
    if (s->signal.source == FF_SIGNAL_ACQUIRE) {
        /* The diagnostic old acquire mode can carry a GPU/token ticket. An
         * acquire declaration alone does not establish CPU-ready completion. */
        if (s->wait.translated) return "acquire-translated-ticket";
        return s->wait.raw_ticket ? "acquire-ticket" : "cpu-acquire";
    }
    if (s->signal.source == FF_SIGNAL_QUEUE)
        return s->wait.translated ? "queue-translated-ticket" : s->signal.ticket ? "queue-gpu" : "queue-empty";
    return "unknown";
}
static double offset_ms(uint64_t time, uint64_t base) {
    if (!time || !base) return NAN;
    return time >= base ? (double)(time - base) / 1e6 : -(double)(base - time) / 1e6;
}
static void ticket_row(FILE *out, const char *name, uint64_t ticket, unsigned status, const totk_ff_ticket_record *t) {
    fprintf(out, " %s_ticket=%" PRIu64 " %s_meta=%s %s_frame=%" PRIu64 " %s_seen=%#x"
            " %s_seal_ns=%" PRIu64 " %s_queue_begin_ns=%" PRIu64 " %s_queue_end_ns=%" PRIu64
            " %s_gpu_observed_ns=%" PRIu64 " %s_done_publish_before_store_ns=%" PRIu64 " %s_post_ops=%u",
            name, ticket, name, statuses[status], name, status == FF_META_OK ? t->frame : 0, name, t->seen,
            name, t->seal_ns, name, t->queue_begin_ns, name, t->queue_end_ns,
            name, t->gpu_observed_ns, name, t->publish_ns, name, t->post_ops);
}
void totk_ff_report(FILE *out, uint64_t first, uint64_t last) {
    if (!out || !first || last < first || !totk_ft_on()) return;
    struct { uint64_t ns, max_ns; unsigned n; } totals[FF_GROUPS][FF_COMPLETE_N] = {{{0}}};
    unsigned missing = 0, lost = 0;
    ff_frame frame, prev; frame_snapshot(first - 1, &prev);
    for (uint64_t f = first; ; f++) {
        unsigned state = frame_snapshot(f, &frame);
        if (state != FF_META_OK) missing++;
        else {
            lost += frame.lost_waits;
            uint64_t anchor = prev.frame + 1 == f ? prev.present_ns : 0;
            for (unsigned i = 0; i < frame.waits; i++) {
                totk_ff_snapshot s; evaluate(&frame, &frame.wait[i], anchor, &s);
                const totk_ff_wait_record *w = &s.wait;
                uint64_t duration = w->enter_ns && w->exit_ns >= w->enter_ns ? w->exit_ns - w->enter_ns : 0;
                unsigned group = w->group < FF_GROUPS - 1 ? w->group : FF_GROUPS - 1;
                totals[group][s.completion].n++; totals[group][s.completion].ns += duration;
                if (duration > totals[group][s.completion].max_ns) totals[group][s.completion].max_ns = duration;
                flockfile(out);
                fprintf(out, "[frame-tl fence] frame=%" PRIu64 " group=%u sync=%#" PRIx64 " callsite=%#" PRIx64
                        " guest_thread=%#" PRIx64 " raw_fenced=%#x translated=%d satisfied=%d immediate=%d"
                        " enter_ns=%" PRIu64 " exit_ns=%" PRIu64 " wait_ms=%.6f game_present_ns=%" PRIu64
                        " prev_game_present_ns=%" PRIu64 " signal_meta=%s signal_source=%s signal_declared_source=%u signal_frame=%" PRIu64
                        " signal_ticket=%" PRIu64 " signal_setter_thread=%#" PRIx64 " signal_ns=%" PRIu64 " signal_condition=%#x signal_flags=%#x completion=%s",
                        f, w->group, w->sync, w->callsite, w->guest_thread, w->raw_fenced, w->translated, w->satisfied,
                        w->immediate, w->enter_ns, w->exit_ns, duration / 1e6, s.game_present_ns, s.prev_game_present_ns,
                        statuses[s.signal_status], source(&s), s.signal.source, s.signal.frame, s.signal.ticket, s.signal.guest_thread, s.signal.time_ns,
                        s.signal.condition, s.signal.flags, completions[s.completion]);
                ticket_row(out, "raw", w->raw_ticket, s.raw_status, &s.raw);
                ticket_row(out, "effective", w->waited_ticket, s.effective_status, &s.effective);
                fprintf(out, " relative_prev_game_ms(enter,exit,signal,effective_seal,effective_queue_begin,effective_queue_end,effective_gpu_observed,effective_publish_before_store)=[%+.6f,%+.6f,%+.6f,%+.6f,%+.6f,%+.6f,%+.6f,%+.6f]\n",
                        offset_ms(w->enter_ns, anchor), offset_ms(w->exit_ns, anchor), offset_ms(s.signal.time_ns, anchor),
                        offset_ms(s.effective.seal_ns, anchor), offset_ms(s.effective.queue_begin_ns, anchor),
                        offset_ms(s.effective.queue_end_ns, anchor), offset_ms(s.effective.gpu_observed_ns, anchor),
                        offset_ms(s.effective.publish_ns, anchor));
                funlockfile(out);
            }
        }
        prev = frame;
        if (f == last) break;
    }
    for (unsigned g = 0; g < FF_GROUPS; g++) for (unsigned c = 0; c < FF_COMPLETE_N; c++) {
        unsigned n = totals[g][c].n;
        if (!n) continue;
        fprintf(out, "[frame-tl fence-summary] frames=%" PRIu64 "..%" PRIu64 " group=%s completion=%s n=%u total_ms=%.6f mean_ms=%.6f max_ms=%.6f\n",
                first, last, g == 0 ? "0" : g == 1 ? "1" : g == 2 ? "2" : "other", completions[c], n,
                totals[g][c].ns / 1e6, totals[g][c].ns / (double)n / 1e6, totals[g][c].max_ns / 1e6);
    }
    fprintf(out, "[frame-tl fence-coverage] frames=%" PRIu64 "..%" PRIu64 " missing_frame_records=%u window_lost_waits=%u"
            " cumulative(busy,stale_ticket,stale_frame,stale_signal,ticket_evictions,signal_evictions,wait_overflow,conflicts)=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64
            " gpu=cpu-fence-observation publish=after-cpu-posts-before-done-store categories=observed-order-not-causality\n",
            first, last, missing, lost, atomic_load(&busy_events), atomic_load(&stale_tickets), atomic_load(&stale_frames),
            atomic_load(&stale_signals), atomic_load(&ticket_evictions), atomic_load(&signal_evictions),
            atomic_load(&wait_overflow), atomic_load(&conflicts));
}
#ifdef TOTK_FRAME_TIMELINE_TEST
void totk_ff_test_reset(void) {
    for (unsigned i = 0; i < FF_TICKETS; i++) { memset(&tickets[i].data, 0, sizeof tickets[i].data); atomic_store(&tickets[i].lock, 0); }
    for (unsigned i = 0; i < FF_SIGNAL_BUCKETS; i++) { memset(&signals[i].data, 0, sizeof signals[i].data); atomic_store(&signals[i].lock, 0); }
    for (unsigned i = 0; i < FT_RING; i++) { memset(&frames[i].data, 0, sizeof frames[i].data); atomic_store(&frames[i].lock, 0); }
    atomic_store(&busy_events, 0); atomic_store(&stale_tickets, 0); atomic_store(&stale_frames, 0); atomic_store(&stale_signals, 0);
    atomic_store(&ticket_evictions, 0); atomic_store(&signal_evictions, 0); atomic_store(&wait_overflow, 0); atomic_store(&conflicts, 0);
}
int totk_ff_test_snapshot(uint64_t frame, unsigned index, totk_ff_snapshot *out) {
    memset(out, 0, sizeof *out);
    ff_frame f, prev;
    if (frame_snapshot(frame, &f) != FF_META_OK || index >= f.waits) return 0;
    frame_snapshot(frame - 1, &prev);
    evaluate(&f, &f.wait[index], prev.frame + 1 == frame ? prev.present_ns : 0, out);
    return 1;
}
#endif
#endif
