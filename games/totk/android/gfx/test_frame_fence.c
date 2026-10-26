/* Standalone host test: link runtime/frame_fence.c + frame_timeline.c with
 * -D_GNU_SOURCE -DTOTK_FRAME_TIMELINE_TEST -pthread -lm. No GPU/device/sleeps. */
#include "../../runtime/frame_fence.h"
#include "../../runtime/frame_timeline.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t ms = 1000000;
static totk_ff_wait_record wait_record(uint64_t frame, uint64_t sync, uint64_t raw, uint64_t effective,
                                      uint64_t enter, uint64_t exit) {
    return (totk_ff_wait_record){.frame = frame, .sync = sync, .raw_ticket = raw, .waited_ticket = effective,
        .enter_ns = enter, .exit_ns = exit, .callsite = 0x2db96c, .guest_thread = 0x1001,
        .group = 1, .raw_fenced = 1, .satisfied = 1};
}
static void ticket(uint64_t frame, uint64_t value, uint64_t gpu, uint64_t publish, unsigned posts) {
    totk_ff_ticket_seal(frame, value, 80 * ms);
    totk_ff_ticket_queue(frame, value, 90 * ms, 95 * ms);
    totk_ff_ticket_gpu(frame, value, gpu);
    totk_ff_ticket_publish(value, publish, posts);
}
static void signal_for(const totk_ff_wait_record *w, unsigned source) {
    totk_ff_signal(w->sync, w->frame, w->raw_ticket, 100 * ms, 0x2002, source, 3, 5);
}
static totk_ff_snapshot get(uint64_t frame, unsigned index) {
    totk_ff_snapshot s; assert(totk_ff_test_snapshot(frame, index, &s)); return s;
}
static char *report(uint64_t first, uint64_t last, int core) {
    char *text = NULL; size_t size = 0;
    FILE *out = open_memstream(&text, &size); assert(out);
    if (core) totk_ft_test_report(out, first, last); else totk_ff_report(out, first, last);
    assert(!fclose(out)); return text;
}
static unsigned occurrences(const char *text, const char *needle) {
    unsigned n = 0;
    while ((text = strstr(text, needle))) { n++; text += strlen(needle); }
    return n;
}
static void core_forwarding(void) {
    const uint64_t base = 1000000000, period = 33333333;
    totk_ft_present(1, period, base);
    totk_ft_slot_seal(2, 1, base + 1000);
    totk_ft_slot_queue(2, 1, base + 2000, base + 3000);
    totk_ft_slot_gpu(2, 1, base + 15000, 10000);
    totk_ff_ticket_publish(1, base + 19000, 2);
    totk_ff_signal(0x9000, 2, 1, base + 5000, 0x1001, FF_SIGNAL_QUEUE, 1, 0);
    totk_ff_wait_record w = wait_record(2, 0x9000, 1, 1, base + 10000, base + 20000);
    totk_ff_wait(&w); totk_ft_present(2, period, base + period);
    w = wait_record(3, 0x9100, 0, 0, base + period + 1000, base + period + 1001); w.immediate = 1;
    totk_ff_signal(w.sync, 3, 0, base + period, 0x1001, FF_SIGNAL_ACQUIRE, 0, 0);
    totk_ff_wait(&w); totk_ft_present(3, period, base + 2 * period);
    totk_ff_snapshot s = get(2, 0);
    assert(s.raw.frame == 2 && s.raw.seal_ns == base + 1000 && s.raw.queue_end_ns == base + 3000);
    assert(s.effective.gpu_observed_ns == base + 15000 && s.effective.publish_ns == base + 19000);
    assert(s.completion == FF_COMPLETE_GPU_PROGRESS);
    assert(s.game_present_ns == base + period && s.prev_game_present_ns == base);
    char *text = report(2, 3, 1);
    assert(!strstr(text, "[frame-tl late]"));
    assert(occurrences(text, "[frame-tl fence] ") == 2);
    assert(strstr(text, "[frame-tl fence] frame=2 group=1 "));
    assert(strstr(text, "[frame-tl fence] frame=3 group=1 "));
    assert(strstr(text, "game_present_ns=1033333333 prev_game_present_ns=1000000000"));
    assert(strstr(text, "effective_gpu_observed_ns=1000015000 effective_done_publish_before_store_ns=1000019000 effective_post_ops=2"));
    free(text);
}
static void categories_and_owners(void) {
    totk_ff_test_reset();
    ticket(9, 100, 115 * ms, 119 * ms, 2);
    ticket(10, 101, 160 * ms, 170 * ms, 0);
    totk_ff_present(9, 100 * ms);
    totk_ff_wait_record w = wait_record(10, 0xa000, 101, 100, 110 * ms, 120 * ms);
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w); totk_ff_present(10, 133 * ms);
    totk_ff_snapshot s = get(10, 0);
    assert(s.raw_status == FF_META_OK && s.effective_status == FF_META_OK);
    assert(s.raw.frame == 10 && s.effective.frame == 9 && !s.wait.translated);
    assert(s.signal.frame == 10 && s.signal.ticket == 101 && s.signal.guest_thread == 0x2002);
    assert(s.signal.condition == 3 && s.signal.flags == 5 && s.signal.time_ns == 100 * ms);
    assert(s.completion == FF_COMPLETE_GPU_PROGRESS);
    assert(s.game_present_ns == 133 * ms && s.prev_game_present_ns == 100 * ms);

    /* GPU was already observed before entry; only the later CPU completion
     * publication anchor falls inside this wait. Posts are not an NVN signal. */
    ticket(11, 102, 105 * ms, 125 * ms, 4);
    w = wait_record(11, 0xa100, 102, 102, 110 * ms, 130 * ms); w.group = 2;
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w);
    s = get(11, 0); assert(s.completion == FF_COMPLETE_CPU_PUBLICATION && s.effective.post_ops == 4);
    for (unsigned i = 0; i < 3; i++) {
        w = wait_record(12 + i, 0xb000 + i * 0x100, i == 2 ? 103 : 0, i == 2 ? 103 : 0,
                        110 * ms, 110 * ms + i); w.immediate = 1;
        if (i == 2) ticket(14, 103, 105 * ms, 108 * ms, 0);
        if (i == 0) w.raw_fenced = 3;
        signal_for(&w, i == 0 ? FF_SIGNAL_ACQUIRE : FF_SIGNAL_QUEUE); totk_ff_wait(&w);
        s = get(12 + i, 0); assert(s.completion == FF_COMPLETE_IMMEDIATE);
        assert(s.signal.source == (i == 0 ? FF_SIGNAL_ACQUIRE : FF_SIGNAL_QUEUE));
        assert(s.effective_status == (i == 2 ? FF_META_OK : FF_META_EMPTY));
    }
    ticket(15, 104, 150 * ms, 155 * ms, 1);
    w = wait_record(15, 0xc000, 104, 104, 110 * ms, 120 * ms); w.satisfied = 0;
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w); assert(get(15, 0).completion == FF_COMPLETE_TIMEOUT);
    w = wait_record(16, 0xc100, 105, 105, 110 * ms, 110 * ms); w.raw_fenced = 0; w.satisfied = 0;
    totk_ff_wait(&w); assert(get(16, 0).completion == FF_COMPLETE_UNFENCED);
    w = wait_record(17, 0xc200, 777, 777, 110 * ms, 120 * ms);
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w);
    s = get(17, 0); assert(s.completion == FF_COMPLETE_UNKNOWN && s.effective_status == FF_META_MISSING);
    w = wait_record(18, 0xc300, 103, 103, 110 * ms, 120 * ms); totk_ff_wait(&w);
    s = get(18, 0); assert(s.completion == FF_COMPLETE_UNKNOWN && s.signal_status == FF_META_MISSING);
    w = wait_record(19, 0xc400, 0, 0, 110 * ms, 110 * ms); w.immediate = 1; totk_ff_wait(&w);
    s = get(19, 0); assert(s.completion == FF_COMPLETE_IMMEDIATE && s.signal_status == FF_META_MISSING);
    char *text = report(10, 19, 0);
    assert(occurrences(text, "[frame-tl fence] ") == 10);
    assert(strstr(text, "raw_ticket=101 raw_meta=ok raw_frame=10"));
    assert(strstr(text, "effective_ticket=100 effective_meta=ok effective_frame=9"));
    assert(strstr(text, "signal_source=queue-gpu signal_declared_source=1 signal_frame=10 signal_ticket=101"));
    assert(strstr(text, "signal_source=cpu-acquire")); assert(strstr(text, "signal_source=queue-empty"));
    assert(strstr(text, "group=2 completion=gpu-already-observed-cpu-publication-pending n=1 total_ms=20.000000"));
    assert(strstr(text, "group=1 completion=immediate-already-satisfied n=4"));
    assert(strstr(text, "relative_prev_game_ms(enter,exit,signal,effective_seal,effective_queue_begin,effective_queue_end,effective_gpu_observed,effective_publish_before_store)=[+10.000000,+20.000000"));
    free(text);
}
static void acquire_ticket_sources(void) {
    totk_ff_test_reset();
    /* FF_SIGNAL_ACQUIRE declares the API setter, not necessarily CPU-ready:
     * diagnostic old mode carries a real ticket, potentially translated. */
    ticket(40, 400, 115 * ms, 119 * ms, 0);
    ticket(41, 401, 160 * ms, 170 * ms, 0);
    ticket(42, 402, 105 * ms, 118 * ms, 3);
    ticket(43, 403, 105 * ms, 108 * ms, 0);
    for (unsigned i = 0; i < 5; i++) {
        uint64_t raw = i == 4 ? 400 : 400 + i;
        uint64_t effective = i == 1 ? 400 : raw;
        totk_ff_wait_record w = wait_record(40 + i, 0x13000 + i * 0x100, raw, effective, 110 * ms, 120 * ms);
        w.raw_fenced = i == 4 ? 7 : 3;
        w.translated = i == 4; /* token namespace, not a lag-adjusted GPU fence */
        if (i == 3) w.immediate = 1;
        signal_for(&w, FF_SIGNAL_ACQUIRE); totk_ff_wait(&w);
        totk_ff_snapshot s = get(40 + i, 0);
        assert(s.signal.source == FF_SIGNAL_ACQUIRE);
        assert(s.completion == (i == 2 ? FF_COMPLETE_CPU_PUBLICATION :
                                i == 3 ? FF_COMPLETE_IMMEDIATE : i == 4 ? FF_COMPLETE_UNKNOWN : FF_COMPLETE_GPU_PROGRESS));
        if (i == 1) assert(s.raw.frame == 41 && s.effective.frame == 40 && !s.wait.translated);
        if (i == 4) assert(s.raw_status == FF_META_TRANSLATION_TOKEN && s.effective_status == FF_META_TRANSLATION_TOKEN && !s.raw.frame);
    }
    char *text = report(40, 44, 0);
    assert(!strstr(text, "signal_source=cpu-acquire"));
    assert(occurrences(text, "signal_source=acquire-ticket ") == 4);
    assert(occurrences(text, "signal_source=acquire-translated-ticket ") == 1);
    free(text);
    totk_ff_wait_record w = wait_record(45, 0x14000, 0, 0, 110 * ms, 110 * ms);
    w.immediate = 1; w.raw_fenced = 3; signal_for(&w, FF_SIGNAL_ACQUIRE); totk_ff_wait(&w);
    assert(get(45, 0).completion == FF_COMPLETE_IMMEDIATE);
    text = report(45, 45, 0); assert(strstr(text, "signal_source=cpu-acquire ")); free(text);
    /* A translation token numerically equal to real GPU ticket 401 must NOT
     * inherit frame 41 or any of that ticket's GPU completion timestamps. */
    w = wait_record(46, 0x14100, 401, 401, 110 * ms, 120 * ms);
    w.raw_fenced = 5; w.translated = 1;
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w);
    totk_ff_snapshot s = get(46, 0);
    assert(s.raw_status == FF_META_TRANSLATION_TOKEN && s.effective_status == FF_META_TRANSLATION_TOKEN);
    assert(!s.raw.frame && !s.effective.frame && !s.effective.gpu_observed_ns && s.completion == FF_COMPLETE_UNKNOWN);
    text = report(46, 46, 0);
    assert(strstr(text, "signal_source=queue-translated-ticket "));
    assert(strstr(text, "raw_meta=translation-token-not-GPU-ticket raw_frame=0"));
    free(text);
}
static void all_frame_window(void) {
    totk_ff_test_reset();
    uint64_t now = 1000000000;
    totk_ff_present(199, now);
    for (uint64_t f = 200; f < 500; f++) {
        for (unsigned group = 1; group <= 2; group++) {
            totk_ff_wait_record w = wait_record(f, 0xd000 + group * 0x100, 0, 0, now + 1000, now + 2000);
            w.group = group; w.immediate = 1;
            totk_ff_signal(w.sync, f, 0, now + 500, 0x1001, FF_SIGNAL_ACQUIRE, 0, 0);
            totk_ff_wait(&w);
        }
        now += (f & 1) ? 50000000 : 20000000;
        totk_ff_present(f, now);
    }
    char *text = report(200, 499, 0);
    assert(occurrences(text, "[frame-tl fence] ") == 600);
    assert(strstr(text, "[frame-tl fence] frame=200 group=1 "));
    assert(strstr(text, "[frame-tl fence] frame=499 group=2 "));
    assert(strstr(text, "group=1 completion=immediate-already-satisfied n=300 total_ms=0.300000"));
    assert(strstr(text, "group=2 completion=immediate-already-satisfied n=300 total_ms=0.300000"));
    assert(strstr(text, "missing_frame_records=0 window_lost_waits=0"));
    for (uint64_t f = 200; f < 500; f++) {
        totk_ff_snapshot s = get(f, 0), prev = {0};
        assert(s.game_present_ns && s.prev_game_present_ns);
        assert(s.game_present_ns - s.prev_game_present_ns == ((f & 1) ? 50000000u : 20000000u));
        if (f > 200) { prev = get(f - 1, 0); assert(s.prev_game_present_ns == prev.game_present_ns); }
    }
    free(text);
}
static void out_of_order_and_reuse(void) {
    totk_ff_test_reset();
    totk_ff_wait_record w = wait_record(600, 0xe000, 601, 601, 110 * ms, 120 * ms);
    /* A future setter must never replace the setter which was visible at entry. */
    signal_for(&w, FF_SIGNAL_QUEUE);
    totk_ff_signal(w.sync, 601, 999, 111 * ms, 0x9999, FF_SIGNAL_ACQUIRE, 8, 9);
    totk_ff_wait(&w);
    totk_ff_ticket_publish(601, 119 * ms, 7);
    totk_ff_ticket_gpu(600, 601, 115 * ms);
    totk_ff_ticket_queue(600, 601, 90 * ms, 95 * ms);
    totk_ff_ticket_seal(600, 601, 80 * ms);
    totk_ff_snapshot s = get(600, 0);
    assert(s.signal.source == FF_SIGNAL_QUEUE && s.signal.ticket == 601);
    assert(s.effective.frame == 600 && s.effective.seen == 15 && s.effective.post_ops == 7);
    assert(s.completion == FF_COMPLETE_GPU_PROGRESS);
    /* Reporting long after object reassignment still uses the captured setter. */
    totk_ff_signal(w.sync, 602, 1000, 130 * ms, 0x3003, FF_SIGNAL_QUEUE, 1, 2);
    s = get(600, 0); assert(s.signal.frame == 600 && s.signal.guest_thread == 0x2002);
    w.frame = 601; w.enter_ns = 112 * ms; w.exit_ns = 120 * ms;
    totk_ff_wait(&w); s = get(601, 0);
    assert(s.signal_status == FF_META_TICKET_MISMATCH && s.completion == FF_COMPLETE_UNKNOWN);
    /* Contradictory frame ownership invalidates attribution, not just the last
     * caller's interpretation. No partial record may acquire another owner. */
    totk_ff_ticket_seal(999, 601, 80 * ms);
    s = get(600, 0); assert(s.effective_status == FF_META_CONFLICT && s.completion == FF_COMPLETE_UNKNOWN);

    /* More signal history than fits in a bucket: the evicted setter becomes
     * unavailable. A stale late writer cannot evict a newer retained setter. */
    for (unsigned i = 0; i < 20; i++) totk_ff_signal(0xf000, 610 + i, 2000 + i, (200 + i) * ms, 0x1001, FF_SIGNAL_QUEUE, 0, 0);
    totk_ff_signal(0xf000, 610, 2000, 200 * ms, 0x1001, FF_SIGNAL_QUEUE, 0, 0);
    w = wait_record(630, 0xf000, 2000, 2000, 200 * ms, 201 * ms); totk_ff_wait(&w);
    assert(get(630, 0).signal_status == FF_META_MISSING);
    w = wait_record(631, 0xf000, 2019, 2019, 219 * ms, 220 * ms); totk_ff_wait(&w);
    assert(get(631, 0).signal.ticket == 2019);
    char *text = report(600, 631, 0); assert(strstr(text, "signal_meta=ticket-mismatch"));
    assert(strstr(text, "effective_meta=conflict effective_frame=0")); free(text);
}
static void wrap_and_overflow(void) {
    totk_ff_test_reset();
    const uint64_t old = 21, newer = old + FT_RING * FT_SLOTS;
    ticket(700, old, 105 * ms, 108 * ms, 1);
    ticket(701, newer, 115 * ms, 118 * ms, 2);
    totk_ff_ticket_gpu(700, old, 999 * ms); /* expired writer */
    totk_ff_wait_record w = wait_record(700, 0x11000, old, old, 110 * ms, 120 * ms);
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w);
    totk_ff_snapshot s = get(700, 0);
    assert(s.effective_status == FF_META_STALE && !s.effective.ticket && !s.effective.frame);
    assert(s.completion == FF_COMPLETE_UNKNOWN);
    w = wait_record(701, 0x11100, newer, newer, 110 * ms, 120 * ms);
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w); s = get(701, 0);
    assert(s.effective.ticket == newer && s.effective.frame == 701 && s.effective.gpu_observed_ns == 115 * ms);
    assert(s.completion == FF_COMPLETE_GPU_PROGRESS);
    w = wait_record(800, 0x11200, 0, 0, 110 * ms, 110 * ms); w.immediate = 1;
    signal_for(&w, FF_SIGNAL_ACQUIRE); totk_ff_wait(&w); totk_ff_present(800, 123);
    w.frame += FT_RING; totk_ff_wait(&w); totk_ff_present(w.frame, 456);
    w.frame = 800; totk_ff_wait(&w); totk_ff_present(800, 999);
    assert(!totk_ff_test_snapshot(800, 0, &s));
    s = get(800 + FT_RING, 0); assert(s.game_present_ns == 456 && s.wait.frame == 800 + FT_RING);
    w.frame = 801;
    for (unsigned i = 0; i < 100; i++) totk_ff_wait(&w);
    char *text = report(700, 801, 0);
    assert(strstr(text, "effective_meta=stale effective_frame=0"));
    assert(strstr(text, "window_lost_waits=92"));
    assert(occurrences(text, "[frame-tl fence] frame=801 ") == 8);
    free(text);
}
static void *writer(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < 500; i++) {
        totk_ff_ticket_publish(2000, 119 * ms, 3);
        totk_ff_ticket_gpu(1000, 2000, 115 * ms);
        totk_ff_ticket_queue(1000, 2000, 90 * ms, 95 * ms);
        totk_ff_ticket_seal(1000, 2000, 80 * ms);
        totk_ff_present(1000, 133 * ms);
    }
    return NULL;
}
static void concurrent_snapshots(void) {
    totk_ff_test_reset();
    totk_ff_wait_record w = wait_record(1000, 0x12000, 2000, 2000, 110 * ms, 120 * ms);
    signal_for(&w, FF_SIGNAL_QUEUE); totk_ff_wait(&w);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, writer, NULL));
    for (unsigned i = 0; i < 2000; i++) {
        totk_ff_snapshot s;
        if (!totk_ff_test_snapshot(1000, 0, &s)) continue;
        assert(s.wait.frame == 1000 && s.wait.raw_ticket == 2000);
        assert(s.game_present_ns == 0 || s.game_present_ns == 133 * ms);
        if (s.effective_status != FF_META_OK) continue;
        assert(s.effective.ticket == 2000 && (s.effective.frame == 0 || s.effective.frame == 1000));
        if (s.effective.seen & FF_SEEN_QUEUE) assert(s.effective.queue_begin_ns == 90 * ms && s.effective.queue_end_ns == 95 * ms);
        if (s.effective.seen & FF_SEEN_GPU) assert(s.effective.gpu_observed_ns == 115 * ms);
        if (s.effective.seen & FF_SEEN_PUBLISH) assert(s.effective.publish_ns == 119 * ms && s.effective.post_ops == 3);
    }
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    /* Bounded producers may explicitly drop contended events. A final
     * uncontended complete update makes the final assertions deterministic. */
    ticket(1000, 2000, 115 * ms, 119 * ms, 3); totk_ff_present(1000, 133 * ms);
    totk_ff_snapshot s = get(1000, 0);
    assert(s.effective.seen == 15 && s.effective.frame == 1000 && s.completion == FF_COMPLETE_GPU_PROGRESS);
}
int main(int argc, char **argv) {
    int off = argc > 1 && !strcmp(argv[1], "off");
    setenv("TOTK_FRAME_TIMELINE", off ? "0" : "1", 1);
    assert(totk_ft_on() == !off);
    if (off) {
        totk_ff_wait_record w = wait_record(1, 0x1000, 1, 1, 10, 20);
        totk_ff_signal(w.sync, 1, 1, 5, 0x1001, FF_SIGNAL_QUEUE, 0, 0); totk_ff_wait(&w);
        ticket(1, 1, 15, 19, 1); totk_ff_present(1, 30);
        totk_ff_snapshot s; assert(!totk_ff_test_snapshot(1, 0, &s));
        char *text = report(1, 1, 0); assert(!*text); free(text);
        puts("frame fence: disabled path passed"); return 0;
    }
    core_forwarding(); categories_and_owners(); acquire_ticket_sources(); all_frame_window();
    out_of_order_and_reuse(); wrap_and_overflow(); concurrent_snapshots();
    puts("frame fence: raw/effective owners, GPU/CPU publication, immediate sources, timeout/unfenced/missing, all-frame anchors, out-of-order/reuse, stale/overflow and concurrent snapshots passed");
    return 0;
}
