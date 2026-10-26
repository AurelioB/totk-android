/* Standalone Linux host fixture: no GPU, Android, clocks, sleeps or real fds. */
#include "vk_present_fence.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    vk_present_fence_reason state[128];
    uint64_t time[128];
    unsigned closed[128], results;
    vk_present_fence_entry owner[128];
    vk_present_fence_reason reason[128];
    uint64_t display[128];
} fixture;
static vk_present_fence_reason query(int fd, uint64_t *display, int *error, void *context) {
    fixture *s = context;
    assert(fd >= 0 && fd < 128 && !s->closed[fd]);
    *display = s->time[fd]; *error = s->state[fd] == VK_PF_IOCTL_ERROR ? 25 : 0;
    return s->state[fd];
}
static void close_fd(int fd, void *context) {
    fixture *s = context;
    assert(fd >= 0 && fd < 128 && !s->closed[fd]); s->closed[fd]++;
}
static void result(const vk_present_fence_entry *entry, vk_present_fence_reason reason,
        uint64_t display, void *context) {
    fixture *s = context; unsigned n = s->results++;
    assert(n < 128);
    s->owner[n] = *entry; s->reason[n] = reason; s->display[n] = display;
    assert(reason == VK_PF_SIGNALED || !display);
}
static void add(vk_present_fences *f, fixture *s, int fd, uint64_t frame) {
    vk_present_fence_entry entry = {.fd = fd, .frame = frame, .apply_ns = frame * 10,
        .generation = frame / 10 + 1, .latch_ns = (int64_t)frame * 10 + 1,
        .framegen_real=frame+2000,
        .prediction = {.id = frame + 1000, .slot_ns = frame * 100, .generation = frame / 10 + 1},
        .cadence = {.id = frame, .generation = frame / 10 + 1, .desired_ns = frame * 100,
            .refresh_ns = 16666667, .interval = 2}};
    vk_present_fences_add(f, entry, query, close_fd, result, s);
    /* A recycled callback/presenter owner cannot mutate the retained IDs. */
    entry.frame = entry.apply_ns = entry.generation = entry.framegen_real = 0; entry.latch_ns = 0;
    entry.prediction = (runtime_present_prediction){0};
    entry.cadence = (vk_present_cadence_sample){0};
}
static void pending_then_signaled(void) {
    vk_present_fences f = {0}; fixture s = {0};
    s.state[0] = VK_PF_PENDING; add(&f, &s, 0, 11); /* fd zero is valid */
    assert(f.pending == 1 && !s.results && !s.closed[0]);
    vk_present_fences_sweep(&f, query, close_fd, result, &s);
    assert(f.pending == 1 && !s.results && !s.closed[0]);
    s.state[0] = VK_PF_SIGNALED; s.time[0] = 1234;
    vk_present_fences_sweep(&f, query, close_fd, result, &s);
    assert(!f.pending && s.results == 1 && s.closed[0] == 1);
    assert(s.display[0] == 1234 && s.owner[0].frame == 11 && s.owner[0].apply_ns == 110);
    assert(s.owner[0].generation == 2 && s.owner[0].latch_ns == 111 && s.owner[0].deferred);
    assert(s.owner[0].framegen_real==2011);
    assert(s.owner[0].prediction.id == 1011 && s.owner[0].prediction.slot_ns == 1100);
    assert(s.owner[0].cadence.id == 11 && s.owner[0].cadence.desired_ns == 1100);
    assert(s.owner[0].cadence.generation == 2 && s.owner[0].cadence.refresh_ns == 16666667);
    assert(s.owner[0].cadence.interval == 2 && !s.owner[0].cadence.display_ns);
    vk_present_fences_sweep(&f, query, close_fd, result, &s);
    vk_present_fences_clear(&f, close_fd, result, &s);
    assert(s.results == 1 && s.closed[0] == 1);
}
static void cadence_without_auto_or_timeline(void) {
    vk_present_fences f = {0}; fixture s = {0};
    s.state[1] = VK_PF_PENDING;
    vk_present_fence_entry entry = {.fd = 1, .framegen_real=42,
        .source_frame=123, .generated=1, .interpolated=1, .cadence = {
        .id = 1, .generation = 1, .desired_ns = 1000, .refresh_ns = 10, .apply_ns = 995, .interval = 2}};
    vk_present_fences_add(&f, entry, query, close_fd, result, &s);
    entry.cadence = (vk_present_cadence_sample){0};
    entry.framegen_real=0;
    entry.source_frame=0; entry.generated=entry.interpolated=0;
    s.state[1] = VK_PF_SIGNALED; s.time[1] = 1010;
    vk_present_fences_sweep(&f, query, close_fd, result, &s);
    assert(s.results == 1 && !s.owner[0].frame && !s.owner[0].prediction.id);
    assert(s.owner[0].framegen_real==42);
    assert(s.owner[0].source_frame==123 && s.owner[0].generated && s.owner[0].interpolated);
    assert(s.owner[0].cadence.id == 1 && s.owner[0].cadence.desired_ns == 1000);
    assert(s.owner[0].cadence.apply_ns == 995 && s.display[0] == 1010 && s.closed[1] == 1);
}
static void errors_and_absence(void) {
    vk_present_fences f = {0}; fixture s = {0};
    add(&f, &s, -1, 20);
    assert(s.results == 1 && s.reason[0] == VK_PF_NO_FD && !s.display[0]);
    for (int i = VK_PF_IOCTL_ERROR; i <= VK_PF_INVALID_TIME; i++) {
        s.state[i] = (vk_present_fence_reason)i; add(&f, &s, i, (uint64_t)i);
        assert(s.closed[i] == 1 && s.reason[s.results - 1] == (vk_present_fence_reason)i);
    }
    s.state[40] = VK_PF_PENDING; add(&f, &s, 40, 40);
    s.state[40] = VK_PF_FENCE_ERROR;
    vk_present_fences_sweep(&f, query, close_fd, result, &s);
    assert(s.closed[40] == 1 && !f.pending);
    vk_present_fences_clear(&f, close_fd, result, &s);
}
static void overflow_and_teardown(void) {
    vk_present_fences f = {0}; fixture s = {0};
    for (unsigned i = 1; i <= VK_PRESENT_FENCE_CAPACITY + 1; i++) {
        s.state[i] = VK_PF_PENDING; add(&f, &s, (int)i, 100 + i);
    }
    assert(f.pending == VK_PRESENT_FENCE_CAPACITY && s.closed[1] == 1);
    assert(s.results == 1 && s.reason[0] == VK_PF_EVICTED && s.owner[0].frame == 101);
    /* Later-generation frames cannot inherit an evicted frame's timestamps. */
    s.state[2] = VK_PF_SIGNALED; s.time[2] = 9999;
    vk_present_fences_sweep(&f, query, close_fd, result, &s);
    assert(s.owner[1].frame == 102 && s.owner[1].apply_ns == 1020 && s.display[1] == 9999);
    vk_present_fences_clear(&f, close_fd, result, &s);
    assert(!f.pending && f.reason[VK_PF_TEARDOWN] == VK_PRESENT_FENCE_CAPACITY - 1);
    for (unsigned i = 1; i <= VK_PRESENT_FENCE_CAPACITY + 1; i++) assert(s.closed[i] == 1);
    unsigned completed = s.results;
    vk_present_fences_clear(&f, close_fd, result, &s);
    assert(s.results == completed);
}
static void merged_timestamps(void) {
    struct sync_fence_info child[2] = {{.status = 1, .timestamp_ns = 100}, {.status = 1, .timestamp_ns = 200}};
    struct sync_file_info info = {.status = 1, .num_fences = 2}; uint64_t display;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_SIGNALED && display == 200);
    info.status = 0;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_PENDING && !display);
    info.status = -1;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_FENCE_ERROR && !display);
    info.status = 1; info.num_fences = 3;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_TOO_MANY && !display);
    info.num_fences = 0;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_INVALID_TIME && !display);
    info.num_fences = 2; child[1].status = 0;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_PENDING && !display);
    child[1].status = -1;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_FENCE_ERROR && !display);
    child[1].status = 1; child[1].timestamp_ns = 0;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_INVALID_TIME && !display);
    child[1].timestamp_ns = UINT64_MAX;
    assert(vk_present_fence_decode(&info, child, 2, &display) == VK_PF_INVALID_TIME && !display);
}
static vk_present_cadence_sample cadence_sample(uint64_t id, uint64_t display) {
    return (vk_present_cadence_sample){.id = id, .generation = 1,
        .desired_ns = 1000000000ull + id * 33333334, .refresh_ns = 16666667,
        .display_ns = display, .interval = 2};
}
static void cadence_order_and_missing(void) {
    vk_present_cadence c = {0};
    vk_present_cadence_sample a = cadence_sample(1, 1100000000);
    vk_present_cadence_sample b = cadence_sample(2, a.display_ns + 33333334);
    vk_present_cadence_sample d = cadence_sample(3, b.display_ns + 33333334);
    vk_present_cadence_result(&c, a);
    vk_present_cadence_result(&c, d); /* deferred older fence must not look like a reversed display */
    assert(!c.counts.pairs);
    vk_present_cadence_result(&c, b);
    assert(c.counts.results == 3 && c.counts.pairs == 2 && !c.counts.reversed);
    assert(c.counts.bins[3] == 2 && c.counts.interval2_bins[3] == 2);
    assert(c.counts.min_ns == 33333334 && c.counts.max_ns == 33333334);
    vk_present_cadence_result(&c, cadence_sample(5, d.display_ns + 2 * 33333334));
    vk_present_cadence_result(&c, cadence_sample(4, 0)); /* no latch/callback substitute */
    assert(c.counts.missing == 1 && c.counts.missing_pairs == 2 && c.counts.pairs == 2);
    vk_present_cadence_sample changed = cadence_sample(6, d.display_ns + 3 * 33333334);
    changed.generation = 2;
    vk_present_cadence_result(&c, changed);
    assert(c.counts.boundaries == 1 && c.counts.pairs == 2);
    changed.id = 7; changed.refresh_ns = 8333333;
    vk_present_cadence_result(&c, changed);
    assert(c.counts.boundaries == 2 && c.counts.pairs == 2);
}
static void cadence_bins_and_rollover(void) {
    vk_present_cadence c = {0};
    uint64_t display = 2000000000;
    const uint64_t deltas[] = {16666667, 25000000, 33333334, 50000001, 0, 10000000, 41666668, 60000000};
    vk_present_cadence_result(&c, cadence_sample(1, display));
    for (unsigned i = 0; i < sizeof deltas / sizeof *deltas; i++) {
        display += deltas[i];
        vk_present_cadence_result(&c, cadence_sample(i + 2, display));
    }
    assert(c.counts.pairs == 8 && c.counts.repeated == 1 && !c.counts.reversed);
    assert(c.counts.bins[0] == 2);
    for (unsigned i = 1; i < VK_PRESENT_CADENCE_BINS - 1; i++) assert(c.counts.bins[i] == 1);
    assert(c.counts.bins[5] == 2);
    for (unsigned i = 0; i < VK_PRESENT_CADENCE_BINS; i++)
        assert(c.counts.interval2_bins[i] == c.counts.bins[i]);
    vk_present_cadence_result(&c, cadence_sample(10, display - 1));
    assert(c.counts.reversed == 1 && c.counts.pairs == 8);
    c.counts = (vk_present_cadence_counts){0}; /* logging must not discard the adjacent endpoint */
    vk_present_cadence_result(&c, cadence_sample(11, display + 33333333));
    assert(c.counts.results == 1 && c.counts.pairs == 1 && c.counts.bins[3] == 1);
    vk_present_cadence_result(&c, cadence_sample(11 + VK_PRESENT_CADENCE_HISTORY, display + 1000000000));
    vk_present_cadence_result(&c, cadence_sample(11, display));
    assert(c.counts.history_lost == 1); /* explicitly unknown, never join nonadjacent frames */
    c = (vk_present_cadence){0};
    vk_present_cadence_sample first = cadence_sample(1, 1000000000);
    first.interval = 1;
    vk_present_cadence_result(&c, first);
    vk_present_cadence_result(&c, cadence_sample(2, 1033333334));
    vk_present_cadence_result(&c, cadence_sample(3, 1066666668));
    assert(c.counts.pairs == 2 && c.counts.bins[3] == 2 && c.counts.interval2_bins[3] == 1);
}
static void cadence_apply_and_endpoint_counts(void) {
    vk_present_cadence c = {0};
    vk_present_cadence_sample a = cadence_sample(1, 0);
    a.apply_ns = a.desired_ns + 3;
    vk_present_cadence_result(&c, a);
    a.id = 2; a.display_ns = a.desired_ns - 1; a.interval = 1;
    vk_present_cadence_result(&c, a);
    a.id = 3; a.display_ns = a.desired_ns + a.refresh_ns; a.interval = 3;
    vk_present_cadence_result(&c, a);
    a.id = 4; a.interval = 0; a.apply_ns = 0;
    vk_present_cadence_result(&c, a);
    assert(c.counts.results == 4 && c.counts.missing == 1 && c.counts.signaled == 3);
    assert(c.counts.interval1 == 1 && c.counts.interval2 == 1 && c.counts.other == 1 && c.counts.legacy == 1);
    assert(c.counts.apply_late == 3 && c.counts.worst_apply_late_ns == 3);
    assert(c.counts.before_desired == 1 && c.counts.late_refresh == 2);
    assert(c.counts.worst_late_ns == a.refresh_ns);
}
static void cadence_boundaries_percentiles_and_capacity(void) {
    const uint64_t thresholds[] = {12500000, 20900000, 29000000, 37500000, 45800000};
    for (unsigned i = 0; i < sizeof thresholds / sizeof *thresholds; i++) {
        assert(vk_present_cadence_bin(thresholds[i] - 1) == i);
        assert(vk_present_cadence_bin(thresholds[i]) == i + 1);
    }
    vk_present_cadence c = {0};
    vk_present_cadence_percentile p = vk_present_cadence_percentiles(&c.counts);
    assert(!p.p50_ns && !p.p95_ns && !p.p99_ns);
    uint64_t display = 1000000000;
    vk_present_cadence_result(&c, cadence_sample(1, display));
    /* Descending input exercises report-only sorting and nearest ranks. */
    for (unsigned i = 0; i < VK_PRESENT_CADENCE_SAMPLES; i++) {
        display += VK_PRESENT_CADENCE_SAMPLES - i;
        vk_present_cadence_result(&c, cadence_sample(i + 2, display));
    }
    assert(c.counts.sample_count == 300 && c.counts.pairs == 300 && !c.counts.sample_lost);
    assert(c.counts.samples[0] == 300 && c.counts.samples[299] == 1); /* Unsorted on hot path. */
    p = vk_present_cadence_percentiles(&c.counts);
    assert(p.p50_ns == 150 && p.p95_ns == 285 && p.p99_ns == 297);
    vk_present_cadence_result(&c, cadence_sample(302, display + 1000));
    assert(c.counts.pairs == 301 && c.counts.sample_count == 300 && c.counts.sample_lost == 1);
    c.counts = (vk_present_cadence_counts){0};
    vk_present_cadence_result(&c, cadence_sample(303, display + 2000));
    p = vk_present_cadence_percentiles(&c.counts);
    assert(c.counts.pairs == 1 && p.p50_ns == 1000 && p.p95_ns == 1000 && p.p99_ns == 1000);
}
static void cadence_tail_ownership_and_legacy(void) {
    vk_present_cadence c = {0};
    vk_present_cadence_sample a = cadence_sample(1, 1000000000);
    a.interval = 1;
    vk_present_cadence_result(&c, a);
    for (unsigned i = 2; i <= 12; i++)
        vk_present_cadence_result(&c, cadence_sample(i, 1000000000ull + (i - 1) * 16666667));
    assert(c.counts.interval2_bins[1] == 10 && c.counts.tail_count == VK_PRESENT_CADENCE_TAILS);
    assert(c.counts.tails[0].first_id == 2 && c.counts.tails[0].second_id == 3);
    assert(c.counts.tails[0].before_interval == 1);
    assert(c.counts.tails[1].before_interval == 2);
    assert(c.counts.tails[0].desired_delta_ns == 33333334);
    assert(c.counts.tails[0].display_delta_ns == 16666667);
    c = (vk_present_cadence){0};
    a = cadence_sample(1, 1000000000); a.interval = 0; a.refresh_ns = 0;
    vk_present_cadence_result(&c, a);
    a.id = 2; a.display_ns += 25000000;
    vk_present_cadence_result(&c, a);
    assert(c.counts.legacy == 2 && c.counts.pairs == 1 && c.counts.bins[2] == 1);
    assert(!c.counts.interval2_bins[2] && !c.counts.tail_count);
}
static void future_transaction_retirement(void) {
    vk_present_flight p={0};
    assert(vk_present_flight_next(&p)==0 && vk_present_flight_submit(&p,0));
    assert(!vk_present_flight_submit(&p,0));
    assert(vk_present_flight_next(&p)==1 && vk_present_flight_submit(&p,1));
    assert(vk_present_flight_next(&p)==-1 && p.count==2);
    /* Callback1 may arrive first, but its PREVIOUS buffer0 is pinned until
       callback0's context has completed and been retired in submission order. */
    assert(!vk_present_flight_retire(&p,1) && p.count==2);
    assert(vk_present_flight_retire(&p,0));
    assert(vk_present_flight_next(&p)==0 && vk_present_flight_submit(&p,0));
    assert(!vk_present_flight_retire(&p,0));
    assert(vk_present_flight_retire(&p,1) && vk_present_flight_retire(&p,0));
    assert(!vk_present_flight_retire(&p,1));
    for (unsigned i=0;i<10000;i++) {
        int index=vk_present_flight_next(&p);
        assert(index>=0 && vk_present_flight_submit(&p,(unsigned)index));
        assert(vk_present_flight_retire(&p,(unsigned)index));
    }
    assert(!p.count);
}
/* Deterministic host model of ONLY the Android callback/previous-release
 * boundary. There are no GPU, SurfaceFlinger, fd, clock or thread observations.
 * Policy, defer, context capacity and retirement use production helpers.
 * A serial apply is split at its blocking wait so the script can deliver the
 * callback and release independently; it never enters the future-flight ring.
 * Buffer 0 starts as a warm-up transaction's compositor-owned CURRENT. */
enum { FLIGHT_HOST_BUFFERS = 3, FLIGHT_HOST_JOBS = 16, FLIGHT_HOST_STATES = 16 };
typedef struct {
    unsigned active, deferred, id, callback, release;
    int previous, current;
} flight_host_context;
typedef struct {
    vk_present_flight flight;
    flight_host_context context[VK_PRESENT_FLIGHT_CAPACITY];
    unsigned busy[FLIGHT_HOST_BUFFERS], uses[FLIGHT_HOST_BUFFERS], releases[FLIGHT_HOST_BUFFERS];
    unsigned completed[FLIGHT_HOST_JOBS], retired[FLIGHT_HOST_JOBS], closed[FLIGHT_HOST_JOBS];
    unsigned submitted, serial, stopping, removed, surface_released;
    int current;
} flight_host;
typedef struct {
    int policy;
    unsigned count;
    vk_present_flight state[FLIGHT_HOST_STATES];
} flight_host_trace;
enum { FLIGHT_MODE_OFF, FLIGHT_MODE_LOCKED, FLIGHT_MODE_ADAPTIVE };
static int flight_policy(int mode, const char *selector, const char *setting, int api, int symbol) {
    return vk_present_backpressure_policy(mode == FLIGHT_MODE_LOCKED ||
        vk_present_adaptive_backpressure_requested(mode == FLIGHT_MODE_ADAPTIVE, selector),
        setting, api, symbol);
}
static void flight_host_check(const flight_host *s) {
    assert(VK_PRESENT_FLIGHT_CAPACITY == 2 && s->flight.count <= 2);
    assert(!s->serial || !s->flight.count);
    assert(s->submitted < FLIGHT_HOST_JOBS);
    for (unsigned i = 0; i < FLIGHT_HOST_BUFFERS; i++) {
        assert(s->busy[i] <= 1 && s->uses[i] == s->releases[i] + s->busy[i]);
    }
    for (unsigned i = 0; i < VK_PRESENT_FLIGHT_CAPACITY; i++) {
        const flight_host_context *c = &s->context[i];
        unsigned queued = (i + VK_PRESENT_FLIGHT_CAPACITY - s->flight.head) %
            VK_PRESENT_FLIGHT_CAPACITY < s->flight.count;
        assert(c->active == (queued || (s->serial && i == 0)));
        if (!c->active) continue;
        assert(c->id && c->id <= s->submitted && !s->completed[c->id]);
        assert(c->deferred == queued);
        if (c->previous >= 0) {
            assert(c->previous < FLIGHT_HOST_BUFFERS && s->busy[c->previous]);
        }
        if (c->current >= 0) {
            assert(c->current < FLIGHT_HOST_BUFFERS && s->busy[c->current]);
        }
    }
    for (unsigned id = 1; id <= s->submitted; id++) {
        assert(s->completed[id] <= 1 && s->retired[id] <= 1 && s->closed[id] <= 1);
        assert(s->retired[id] <= s->completed[id] && s->closed[id] == s->completed[id]);
    }
    if (s->current >= 0) assert(s->busy[s->current]);
}
static void flight_host_init(flight_host *s) {
    memset(s, 0, sizeof *s);
    s->current = 0; s->busy[0] = s->uses[0] = 1;
    flight_host_check(s);
}
static unsigned flight_host_apply(flight_host *s, int policy, unsigned buffer) {
    assert(buffer < FLIGHT_HOST_BUFFERS);
    /* Fixed synthetic units: a future target, not an actual host/GPU clock. */
    int defer = vk_present_flight_defer(policy, 16, 200, 100);
    if (s->stopping || s->serial || s->busy[buffer] ||
        s->flight.count == VK_PRESENT_FLIGHT_CAPACITY || (!defer && s->flight.count)) return 0;
    int index = defer ? vk_present_flight_next(&s->flight) : 0;
    assert(index >= 0 && !s->context[index].active && s->submitted + 1 < FLIGHT_HOST_JOBS);
    flight_host_context *c = &s->context[index];
    memset(c, 0, sizeof *c);
    c->active = 1; c->deferred = defer; c->id = ++s->submitted;
    c->previous = s->current; c->current = (int)buffer;
    s->current = (int)buffer; s->busy[buffer] = 1; s->uses[buffer]++;
    if (defer) assert(vk_present_flight_submit(&s->flight, (unsigned)index));
    else s->serial = 1;
    flight_host_check(s);
    return c->id;
}
static void flight_host_event(flight_host *s, unsigned id, int release) {
    for (unsigned i = 0; i < VK_PRESENT_FLIGHT_CAPACITY; i++) {
        flight_host_context *c = &s->context[i];
        if (!c->active || c->id != id) continue;
        if (release) { assert(c->previous >= 0 && !c->release); c->release = 1; }
        else { assert(!c->callback); c->callback = 1; }
        flight_host_check(s);
        return;
    }
    assert(!"event must name an outstanding immutable callback context");
}
static int flight_host_finish(flight_host *s) {
    if (!s->serial && !s->flight.count) return 0;
    unsigned index = s->serial ? 0 : s->flight.head;
    flight_host_context *c = &s->context[index];
    assert(c->active);
    /* A signaled fence before callback delivery does not complete the context;
     * callback delivery alone does not release the PREVIOUS buffer either. */
    if (!c->callback || !c->release) return 0;
    assert(c->previous >= 0 && s->busy[c->previous]);
    s->busy[c->previous] = 0; s->releases[c->previous]++;
    assert(++s->completed[c->id] == 1 && ++s->closed[c->id] == 1);
    if (c->deferred) {
        assert(vk_present_flight_retire(&s->flight, index));
        assert(++s->retired[c->id] == 1);
    } else s->serial = 0;
    if (c->current < 0) { s->removed = 1; s->current = -1; }
    c->active = 0;
    flight_host_check(s);
    return 1;
}
static void flight_host_reject_apply(flight_host *s, int policy, unsigned buffer) {
    flight_host before; memcpy(&before, s, sizeof before);
    assert(!flight_host_apply(s, policy, buffer));
    assert(!memcmp(&before, s, sizeof before));
}
static void flight_host_reject_retire(flight_host *s, unsigned index) {
    flight_host before; memcpy(&before, s, sizeof before);
    assert(!vk_present_flight_retire(&s->flight, index));
    assert(!memcmp(&before, s, sizeof before));
}
static void flight_host_record(const flight_host *s, flight_host_trace *trace) {
    if (!trace) return;
    assert(trace->count < FLIGHT_HOST_STATES);
    memcpy(&trace->state[trace->count++], &s->flight, sizeof s->flight);
}
static void flight_policy_lifecycle(int policy, flight_host_trace *trace) {
    flight_host s; flight_host_init(&s);
    if (trace) { memset(trace, 0, sizeof *trace); trace->policy = policy; }
    flight_host_record(&s, trace);
    assert(flight_host_apply(&s, policy, 1) == 1);
    flight_host_record(&s, trace);
    flight_host_reject_apply(&s, policy, 1); /* CURRENT is never reusable. */
    flight_host_record(&s, trace);
    if (policy == VK_PRESENT_BP_SERIAL) {
        assert(s.serial && !s.flight.count);
        flight_host_reject_apply(&s, policy, 2); /* Free buffer, blocked presenter. */
        flight_host_record(&s, trace);
        flight_host_event(&s, 1, 0);
        assert(!flight_host_finish(&s) && s.busy[0]);
        flight_host_record(&s, trace);
        flight_host_event(&s, 1, 1); assert(flight_host_finish(&s));
        flight_host_record(&s, trace);
        assert(!s.busy[0] && s.busy[1] && !s.serial);
        assert(flight_host_apply(&s, policy, 2) == 2 && s.serial && !s.flight.count);
        flight_host_record(&s, trace);
        flight_host_event(&s, 2, 1); /* Release may signal before callback delivery. */
        assert(!flight_host_finish(&s) && s.busy[1]);
        flight_host_record(&s, trace);
        flight_host_event(&s, 2, 0); assert(flight_host_finish(&s));
        assert(!s.busy[0] && !s.busy[1] && s.busy[2]);
    } else {
        assert(flight_host_apply(&s, policy, 2) == 2 && s.flight.count == 2 && !s.serial);
        flight_host_record(&s, trace);
        flight_host_reject_apply(&s, policy, 0); /* No third context. */
        flight_host_event(&s, 2, 0); flight_host_event(&s, 2, 1);
        flight_host_reject_retire(&s, 1);
        assert(!flight_host_finish(&s) && s.busy[0] && s.busy[1]);
        flight_host_record(&s, trace);
        flight_host_event(&s, 1, 0);
        assert(!flight_host_finish(&s) && s.busy[0]); /* Delayed release fence. */
        flight_host_record(&s, trace);
        flight_host_event(&s, 1, 1); assert(flight_host_finish(&s));
        flight_host_record(&s, trace);
        flight_host_reject_retire(&s, 0); /* The same context cannot retire twice. */
        flight_host_record(&s, trace);
        assert(!s.busy[0] && s.busy[1] && s.busy[2]);
        assert(flight_host_apply(&s, policy, 0) == 3 && s.flight.count == 2);
        flight_host_record(&s, trace); /* Reuse only after the old PREVIOUS release. */
        assert(flight_host_finish(&s) && !s.busy[1] && s.busy[2] && s.busy[0]);
        flight_host_record(&s, trace);
        flight_host_event(&s, 3, 0); assert(!flight_host_finish(&s));
        flight_host_record(&s, trace);
        flight_host_event(&s, 3, 1); assert(flight_host_finish(&s));
        assert(s.busy[0] && !s.busy[1] && !s.busy[2]);
    }
    flight_host_record(&s, trace);
    assert(!s.flight.count && !s.serial && !flight_host_finish(&s));
    flight_host_reject_retire(&s, 0); flight_host_reject_retire(&s, 1);
    flight_host_record(&s, trace);
    for (unsigned id = 1; id <= s.submitted; id++) {
        assert(s.completed[id] == 1 && s.closed[id] == 1);
        assert(s.retired[id] == (policy != VK_PRESENT_BP_SERIAL));
    }
}
static void adaptive_selector_capability_and_locked_reference(void) {
    static const struct { const char *value; int request; } selectors[] = {
        {NULL, 0}, {"", 0}, {"serial", 0}, {"request", 1}, {"Request", 0}, {"REQUEST", 0},
        {" request", 0}, {"request ", 0}, {"request\n", 0}, {"requests", 0},
        {"request=1", 0}, {"0", 0}, {"1", 0}
    };
    const char *settings[] = {NULL, "0", "1", "00", "invalid"};
    const int apis[] = {0, 30, 31, 35};
    for (unsigned v = 0; v < sizeof selectors / sizeof *selectors; v++) {
        for (int adaptive = 0; adaptive <= 1; adaptive++)
            assert(vk_present_adaptive_backpressure_requested(adaptive, selectors[v].value) ==
                (adaptive && selectors[v].request));
        for (int mode = FLIGHT_MODE_OFF; mode <= FLIGHT_MODE_ADAPTIVE; mode++)
        for (unsigned setting = 0; setting < sizeof settings / sizeof *settings; setting++)
        for (unsigned api = 0; api < sizeof apis / sizeof *apis; api++)
        for (int symbol = 0; symbol <= 1; symbol++) {
            int policy = flight_policy(mode, selectors[v].value, settings[setting], apis[api], symbol);
            int enabled = mode == FLIGHT_MODE_LOCKED ||
                (mode == FLIGHT_MODE_ADAPTIVE && selectors[v].request);
            int expected = !enabled || apis[api] < 31 || !symbol ? VK_PRESENT_BP_SERIAL :
                setting == 1 ? VK_PRESENT_BP_DIAGNOSTIC_OFF : VK_PRESENT_BP_REQUEST;
            assert(policy == expected);
            assert(vk_present_flight_defer(policy, 16, 200, 100) == (policy != VK_PRESENT_BP_SERIAL));
            assert(!vk_present_flight_defer(policy, 0, 200, 100));
            assert(!vk_present_flight_defer(policy, 16, 100, 100));
            assert(!vk_present_flight_defer(policy, 16, 99, 100));
            assert(vk_present_flight_defer(policy, 1, UINT64_MAX, UINT64_MAX - 1) ==
                (policy != VK_PRESENT_BP_SERIAL));
            assert(!vk_present_flight_defer(policy, 1, UINT64_MAX, UINT64_MAX));
            if (mode == FLIGHT_MODE_LOCKED) {
                /* Before: unchanged production helper with the old locked gate.
                 * After: production helper with locked || adaptive opt-in.
                 * Compare policy AND every recorded flight-state byte, including
                 * full capacity, rejected operations, wraparound and final drain. */
                int reference = vk_present_backpressure_policy(1, settings[setting], apis[api], symbol);
                flight_host_trace before, after;
                assert(!memcmp(&reference, &policy, sizeof policy));
                flight_policy_lifecycle(reference, &before);
                flight_policy_lifecycle(policy, &after);
                assert(!memcmp(&before, &after, sizeof before));
            } else flight_policy_lifecycle(policy, NULL);
        }
    }
}
static void callback_and_release_orderings(void) {
    const int policies[] = {
        flight_policy(FLIGHT_MODE_ADAPTIVE, "request", NULL, 35, 1),
        flight_policy(FLIGHT_MODE_ADAPTIVE, "request", "0", 35, 1)
    };
    for (unsigned p = 0; p < sizeof policies / sizeof *policies; p++) {
        unsigned schedules = 0;
        /* All 24 interleavings of two callbacks and two independently delayed
         * release signals, including a newer callback/fence arriving first. */
        for (unsigned a = 0; a < 4; a++)
        for (unsigned b = 0; b < 4; b++)
        for (unsigned c = 0; c < 4; c++)
        for (unsigned d = 0; d < 4; d++) {
            if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
            unsigned order[] = {a, b, c, d}, seen = 0;
            flight_host s; flight_host_init(&s); schedules++;
            assert(flight_host_apply(&s, policies[p], 1) == 1);
            assert(flight_host_apply(&s, policies[p], 2) == 2);
            assert(s.flight.count == 2 && vk_present_flight_next(&s.flight) == -1);
            assert(!flight_host_finish(&s));
            for (unsigned event = 0; event < 4; event++) {
                unsigned op = order[event]; seen |= 1u << op;
                flight_host_event(&s, op % 2 + 1, op >= 2);
                while (flight_host_finish(&s)) {}
                unsigned first = (seen & 5) == 5, second = first && (seen & 10) == 10;
                assert(s.completed[1] == first && s.completed[2] == second);
                assert(s.flight.count == 2 - first - second);
                assert(s.busy[0] == !first && s.busy[1] == !second && s.busy[2]);
                if (!first) {
                    flight_host_reject_apply(&s, policies[p], 0);
                    flight_host_reject_retire(&s, 1);
                } else flight_host_reject_retire(&s, 0);
                if (!second) flight_host_reject_apply(&s, policies[p], 1);
                flight_host_reject_apply(&s, policies[p], 2);
            }
            assert(!s.flight.count && s.retired[1] == 1 && s.retired[2] == 1);
            assert(s.closed[1] == 1 && s.closed[2] == 1);
            flight_host_reject_retire(&s, 0); flight_host_reject_retire(&s, 1);
        }
        assert(schedules == 24);
    }
}
static void in_flight_mode_transitions(void) {
    /* Every directed locked/adaptive/off transition, without throwing away an
     * already-applied transaction. OFF means FG off, not diagnostic BP off. */
    const int modes[] = {FLIGHT_MODE_LOCKED, FLIGHT_MODE_ADAPTIVE, FLIGHT_MODE_OFF};
    for (unsigned from = 0; from < sizeof modes / sizeof *modes; from++)
    for (unsigned to = 0; to < sizeof modes / sizeof *modes; to++) {
        if (from == to) continue;
        int old_policy = flight_policy(modes[from], "request", NULL, 35, 1);
        int new_policy = flight_policy(modes[to], "request", NULL, 35, 1);
        flight_host s; flight_host_init(&s);
        assert(flight_host_apply(&s, old_policy, 1) == 1);
        if (old_policy == VK_PRESENT_BP_SERIAL) {
            /* The old serial callback is still outstanding when mode changes;
             * even an async new policy cannot reuse its context or overtake it. */
            flight_host_reject_apply(&s, new_policy, 2);
            flight_host_event(&s, 1, 0); assert(!flight_host_finish(&s));
            flight_host_event(&s, 1, 1); assert(flight_host_finish(&s));
            assert(flight_host_apply(&s, new_policy, 2) == 2);
            assert(flight_host_apply(&s, new_policy, 0) == 3 && s.flight.count == 2);
            flight_host_event(&s, 3, 0); flight_host_event(&s, 3, 1);
            assert(!flight_host_finish(&s));
            flight_host_event(&s, 2, 0); assert(!flight_host_finish(&s));
            flight_host_event(&s, 2, 1); assert(flight_host_finish(&s));
            assert(flight_host_finish(&s));
        } else {
            assert(flight_host_apply(&s, old_policy, 2) == 2 && s.flight.count == 2);
            flight_host_event(&s, 2, 0); flight_host_event(&s, 2, 1);
            flight_host_event(&s, 1, 0);
            assert(!flight_host_finish(&s) && s.busy[0]);
            flight_host_event(&s, 1, 1); assert(flight_host_finish(&s));
            assert(s.flight.count == 1 && !s.busy[0] && s.busy[1] && s.busy[2]);
            flight_host_reject_retire(&s, 0);
            if (new_policy == VK_PRESENT_BP_SERIAL) {
                /* Free buffer notwithstanding, switching to serial must first
                 * drain the remaining old async callback/release context. */
                flight_host_reject_apply(&s, new_policy, 0);
                assert(flight_host_finish(&s) && !s.flight.count);
                assert(flight_host_apply(&s, new_policy, 0) == 3 && s.serial);
            } else {
                assert(flight_host_apply(&s, new_policy, 0) == 3 && s.flight.count == 2);
                assert(flight_host_finish(&s) && s.flight.count == 1);
            }
            flight_host_event(&s, 3, 0); assert(!flight_host_finish(&s) && s.busy[2]);
            flight_host_event(&s, 3, 1); assert(flight_host_finish(&s));
        }
        assert(!s.serial && !s.flight.count && s.busy[0] && !s.busy[1] && !s.busy[2]);
        for (unsigned id = 1; id <= s.submitted; id++) assert(s.completed[id] == 1);
        assert(!flight_host_finish(&s));
        flight_host_reject_retire(&s, 0); flight_host_reject_retire(&s, 1);
    }
}
static unsigned flight_host_remove(flight_host *s) {
    if (!s->stopping || s->flight.count || s->serial || s->removed) return 0;
    assert(s->current >= 0 && !s->context[0].active && s->submitted + 1 < FLIGHT_HOST_JOBS);
    flight_host_context *c = &s->context[0];
    memset(c, 0, sizeof *c);
    c->active = 1; c->id = ++s->submitted; c->previous = s->current; c->current = -1;
    s->serial = 1; flight_host_check(s);
    return c->id;
}
static int flight_host_release_surface(flight_host *s) {
    if (!s->stopping || !s->removed || s->surface_released || s->serial || s->flight.count) return 0;
    for (unsigned i = 0; i < FLIGHT_HOST_BUFFERS; i++) if (s->busy[i]) return 0;
    s->surface_released = 1;
    return 1;
}
static void shutdown_with_outstanding_transactions(void) {
    const int policies[] = {
        flight_policy(FLIGHT_MODE_ADAPTIVE, "serial", NULL, 35, 1),
        flight_policy(FLIGHT_MODE_ADAPTIVE, "request", NULL, 35, 1),
        flight_policy(FLIGHT_MODE_ADAPTIVE, "request", "0", 35, 1)
    };
    for (unsigned p = 0; p < sizeof policies / sizeof *policies; p++) {
        flight_host s; flight_host_init(&s);
        assert(flight_host_apply(&s, policies[p], 1) == 1);
        if (policies[p] != VK_PRESENT_BP_SERIAL) assert(flight_host_apply(&s, policies[p], 2) == 2);
        s.stopping = 1; /* Shutdown forbids new work but cannot erase ownership. */
        flight_host_reject_apply(&s, policies[p], 0);
        assert(!flight_host_remove(&s) && !flight_host_release_surface(&s));
        if (s.flight.count == 2) {
            flight_host_event(&s, 2, 0); flight_host_event(&s, 2, 1);
            flight_host_reject_retire(&s, 1);
        }
        assert(!flight_host_finish(&s));
        flight_host_event(&s, 1, 0);
        assert(!flight_host_finish(&s) && s.busy[0]);
        assert(!flight_host_remove(&s) && !flight_host_release_surface(&s));
        flight_host_event(&s, 1, 1); assert(flight_host_finish(&s));
        if (s.flight.count) {
            assert(!flight_host_remove(&s) && !flight_host_release_surface(&s));
            assert(flight_host_finish(&s));
        }
        assert(!s.flight.count && !s.serial && s.current >= 0 && s.busy[s.current]);
        assert(!flight_host_release_surface(&s)); /* Last CURRENT needs removal's release. */
        unsigned removal = flight_host_remove(&s);
        assert(removal && !flight_host_remove(&s) && !flight_host_release_surface(&s));
        flight_host_event(&s, removal, 0);
        assert(!flight_host_finish(&s) && !flight_host_release_surface(&s));
        flight_host_event(&s, removal, 1); assert(flight_host_finish(&s));
        assert(s.removed && s.current == -1 && flight_host_release_surface(&s));
        assert(!flight_host_release_surface(&s) && !flight_host_finish(&s));
        flight_host_reject_retire(&s, 0); flight_host_reject_retire(&s, 1);
        for (unsigned i = 0; i < FLIGHT_HOST_BUFFERS; i++)
            assert(!s.busy[i] && s.releases[i] == s.uses[i]);
        for (unsigned id = 1; id <= s.submitted; id++) {
            assert(s.completed[id] == 1 && s.closed[id] == 1);
            assert(s.retired[id] == (id != removal && policies[p] != VK_PRESENT_BP_SERIAL));
        }
    }
}
int main(void) {
    pending_then_signaled(); errors_and_absence(); overflow_and_teardown(); merged_timestamps();
    future_transaction_retirement();
    adaptive_selector_capability_and_locked_reference();
    callback_and_release_orderings(); in_flight_mode_transitions(); shutdown_with_outstanding_transactions();
    cadence_without_auto_or_timeline(); cadence_order_and_missing(); cadence_bins_and_rollover(); cadence_apply_and_endpoint_counts();
    cadence_boundaries_percentiles_and_capacity(); cadence_tail_ownership_and_legacy();
    puts("present fences: immutable ownership, adaptive selector/capability and locked reference, host callback/release flight lifetimes, mode transitions, shutdown, errors and cadence samples passed");
    return 0;
}
