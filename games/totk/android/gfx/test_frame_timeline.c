/* cc -D_GNU_SOURCE -DTOTK_FRAME_TIMELINE_TEST -pthread test + runtime/frame_timeline.c + runtime/frame_fence.c -lm.
 * Synthetic monotonic timestamps only; no game, GPU, device or sleeps. */
#include "../../runtime/frame_timeline.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void synthetic(uint64_t frame, uint64_t begin, uint64_t walk_ns, uint64_t present) {
    totk_ft_wake(frame, begin, begin - 100000, 0x123400, 0x1001, NULL);
    totk_ft_acquire(frame, begin + 100000, begin + 300000);
    totk_ft_acquire_wait(frame, 120000, 80000);
    /* Two walks aggregate to the same present ID without a per-event ring lock. */
    for (unsigned walk = 0; walk < 2; walk++) {
        totk_ft_work_begin(frame);
        assert(totk_ft_work_frame() == frame);
        for (unsigned c = 0; c < FT_COUNT_N; c++) totk_ft_count((enum totk_ft_counter)c, frame * (c + 1));
        totk_ft_work_end();
        assert(!totk_ft_work_frame());
    }
    totk_ft_count(FT_COUNT_DRAWS, 999); /* outside a walk is not attributed */
    totk_ft_walk(frame, begin + 400000, begin + 400000 + walk_ns, walk_ns - 1000000);
    totk_ft_walk_wait(frame, 500000);
    uint64_t seal = begin + 400000 + walk_ns;
    totk_ft_slot_seal(frame, frame * 2, seal);
    totk_ft_slot_queue(frame, frame * 2, seal + 100000, seal + 200000);
    totk_ft_slot_gpu(frame, frame * 2, seal + 1200000, 900000);
    totk_ft_handoff(frame, present + 100000);
    totk_ft_compositor(frame, present + 200000, present + 1000000);
    totk_ft_submission(frame, present + 200000, FT_SUBMIT_AHB_APPLY);
    totk_ft_present(frame, 33333333, present);
}
static void *slot_writer(void *arg) {
    unsigned writer = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < FT_SLOTS / 4; i++) {
        uint64_t ticket = 1 + writer * (FT_SLOTS / 4) + i, t = 100000 + ticket * 1000;
        /* Queue/completion may arrive before another owner's metadata write. */
        totk_ft_input_read(700, t, 0x2000 + writer, ticket);
        totk_ft_slot_queue(700, ticket, t + 10, t + 20);
        totk_ft_slot_seal(700, ticket, t);
        totk_ft_slot_gpu(700, ticket, t + 30, 5);
        totk_ft_count_frame(700, FT_COUNT_DESC_CREATES, 1);
        totk_ft_work_begin(710 + writer);
        totk_ft_count(FT_COUNT_DRAWS, ticket);
        totk_ft_work_end();
    }
    totk_ft_input_snapshot group;
    totk_ft_input_take(&group);
    assert(group.reads.reads == FT_SLOTS / 4);
    assert(group.reads.first_thread == 0x2000 + writer && group.reads.last_thread == 0x2000 + writer);
    assert(!group.reads.mixed_threads);
    totk_ft_input_snapshot again;
    totk_ft_input_take(&again);
    assert(!again.reads.reads);
    return NULL;
}
static char *report_text(uint64_t first, uint64_t last) {
    char *text = NULL; size_t size = 0;
    FILE *out = open_memstream(&text, &size);
    assert(out); totk_ft_test_report(out, first, last); assert(!fclose(out));
    return text;
}
static void *empty_input_sender(void *out) {
    totk_ft_input_take(out);
    return NULL;
}
static void *migrated_input_sender(void *arg) {
    const uint64_t *p = arg;
    totk_ft_input_read(p[0], p[1], 0x103a, p[2]);
    totk_ft_input_read(p[0], p[1] + 1000000, 0x103a, p[2] + 1);
    return NULL;
}
static void input_cases(uint64_t base) {
    const uint64_t ms = 1000000;
    totk_ft_input_snapshot empty, group, next, drained;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, empty_input_sender, &empty));
    assert(!pthread_join(thread, NULL));
    assert(!empty.reads.reads);
    totk_ft_present(19, 33333333, base + 90 * ms);
    assert(totk_ft_current() == 20);
    /* Arrival order is not timestamp order. Capture actual sample IDs at each
     * endpoint rather than assuming ring-lock acquisition determines either. */
    totk_ft_input_read(20, base + 100 * ms, 0x1001, 101);
    totk_ft_input_read(20, base + 120 * ms, 0x1001, 103);
    totk_ft_input_read(20, base + 110 * ms, 0x1001, 102);
    totk_ft_input_take(&group);
    assert(group.reads.reads == 3);
    assert(group.reads.first == base + 100 * ms && group.reads.last == base + 120 * ms);
    assert(group.reads.first_sampling == 101 && group.reads.last_sampling == 103);
    /* Main samples again while the renderer still owns global frame 20. The
     * immutable earlier wake snapshot belongs on resumed render frame 21. */
    totk_ft_input_read(20, base + 130 * ms, 0x1001, 104);
    totk_ft_present(20, 33333333, base + 135 * ms);
    assert(totk_ft_current() == 21);
    totk_ft_wake(totk_ft_current(), base + 141 * ms, base + 140 * ms, 0xabc, 0x1001, &group);
    totk_ft_compositor(21, base + 150 * ms, base + 160 * ms);
    totk_ft_submission(21, base + 150 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_record r;
    assert(totk_ft_snapshot(20, &r) && r.input_window.reads == 4);
    assert(r.input_window.last_sampling == 104);
    assert(totk_ft_snapshot(21, &r) && !r.input_window.reads && r.input_main_wake_received);
    assert(r.input_main_wake.reads.reads == 3 && r.input_main_wake.reads.last_sampling == 103);
    totk_ft_input_take(&next);
    assert(next.reads.reads == 1 && next.reads.first_sampling == 104);
    totk_ft_wake(22, base + 145 * ms, base + 144 * ms, 0xabc, 0x1001, &next);
    totk_ft_compositor(22, base + 150 * ms, base + 160 * ms);
    totk_ft_submission(22, base + 150 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_input_take(&drained);
    assert(!drained.reads.reads);
    totk_ft_wake(23, base + 155 * ms, base + 154 * ms, 0xabc, 0x1001, &drained);
    totk_ft_compositor(23, base + 160 * ms, 0);
    totk_ft_submission(23, base + 160 * ms, FT_SUBMIT_AHB_APPLY);
    /* Missing reads are distinct from missing handoff and missing display. */
    totk_ft_wake(24, base + 181 * ms, base + 180 * ms, 0xabc, 0x1001, &empty);
    totk_ft_compositor(24, base + 190 * ms, base + 200 * ms);
    totk_ft_submission(24, base + 190 * ms, FT_SUBMIT_AHB_APPLY);
    /* Multiple guest readers remain explicitly mixed; a middle reader cannot
     * disappear merely because the first and last readers are the same. */
    totk_ft_input_read(25, base + 190 * ms, 0x2001, 200);
    totk_ft_input_read(25, base + 200 * ms, 0x2002, 201);
    totk_ft_input_read(25, base + 210 * ms, 0x2001, 202);
    assert(totk_ft_snapshot(25, &r) && r.input_window.reads == 3 && r.input_window.mixed_threads);
    assert(r.input_window.first_thread == 0x2001 && r.input_window.last_thread == 0x2001);
    totk_ft_input_read(26, base + 220 * ms, 0, 203);
    assert(totk_ft_snapshot(26, &r) && r.input_window.unknown_thread_reads == 1);
    totk_ft_input_read(27, 0, 0x2003, 204);
    totk_ft_compositor(27, base + 230 * ms, base + 240 * ms);
    totk_ft_submission(27, base + 230 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_input_read(28, base + 240 * ms, 0x2003, 205);
    totk_ft_compositor(28, base + 230 * ms, base + 235 * ms);
    totk_ft_submission(28, base + 230 * ms, FT_SUBMIT_AHB_APPLY);
    /* Out-of-order completion/apply publication retains captured frame IDs. */
    totk_ft_input_read(29, base + 250 * ms, 0x2003, 206);
    totk_ft_compositor(29, 0, base + 270 * ms);
    totk_ft_compositor(29, base + 260 * ms, 0);
    totk_ft_submission(29, base + 260 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_compositor(31, base + 260 * ms, base + 270 * ms);
    totk_ft_submission(31, base + 260 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_input_read(32, base + 240 * ms, 0x2003, 207);
    totk_ft_compositor(32, base + 260 * ms, base + 250 * ms);
    totk_ft_submission(32, base + 260 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_input_read(33, base + 280 * ms, 0x2003, 208);
    totk_ft_compositor(33, base + 280 * ms, base + 280 * ms);
    totk_ft_submission(33, base + 280 * ms, FT_SUBMIT_AHB_APPLY);
    /* A genuinely slow update may still carry its explicitly captured sample;
     * no arbitrary age cutoff hides that latency. Empty takes never repeat it. */
    totk_ft_wake(34, base + 491 * ms, base + 490 * ms, 0xabc, 0x1001, &next);
    totk_ft_compositor(34, base + 500 * ms, base + 510 * ms);
    totk_ft_submission(34, base + 500 * ms, FT_SUBMIT_AHB_APPLY);
    /* Inconsistent sender ownership must not become a valid latency sample. */
    totk_ft_wake(35, base + 491 * ms, base + 490 * ms, 0xabc, 0x2001, &next);
    totk_ft_compositor(35, base + 500 * ms, base + 510 * ms);
    totk_ft_submission(35, base + 500 * ms, FT_SUBMIT_AHB_APPLY);
    /* Copy submission precedes its acknowledgement and cannot claim an AHB
     * display time, even if a bad caller supplies one. */
    totk_ft_input_read(36, base + 300 * ms, 0x2003, 209);
    totk_ft_submission(36, base + 310 * ms, FT_SUBMIT_COPY_POST);
    totk_ft_compositor(36, base + 330 * ms, base + 340 * ms);
    totk_ft_submission(36, 0, FT_SUBMIT_AHB_APPLY);
    assert(totk_ft_snapshot(36, &r) && r.submit == base + 310 * ms);
    assert(r.submit_source == FT_SUBMIT_COPY_POST && r.apply == base + 330 * ms);
    /* First and last anchors are validated independently when the raw window
     * overlaps submission; a missing new endpoint never falls back to ACK. */
    totk_ft_input_read(37, base + 320 * ms, 0x2003, 210);
    totk_ft_input_read(37, base + 340 * ms, 0x2003, 211);
    totk_ft_submission(37, base + 330 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_compositor(37, base + 330 * ms, base + 350 * ms);
    totk_ft_input_read(38, base + 360 * ms, 0x2003, 212);
    totk_ft_compositor(38, base + 370 * ms, base + 380 * ms);
    totk_ft_wake(39, base + 491 * ms, base + 490 * ms, 0xabc, 0x1001, &next);
    totk_ft_submission(39, base + 480 * ms, FT_SUBMIT_AHB_APPLY);
    totk_ft_compositor(39, base + 480 * ms, base + 510 * ms);
    char *text = report_text(21, 21);
    assert(strstr(text, "frame=21 association=main-wake-snapshot handoff=observed reads=3"));
    assert(strstr(text, "first_sampling=101 last_sampling=103 first_guest_thread=0x1001 last_guest_thread=0x1001"));
    assert(strstr(text, "first_to_submit_ms=50.000 last_to_submit_ms=30.000 first_to_AHB_fence_ms=60.000 last_to_AHB_fence_ms=40.000"));
    assert(strstr(text, "association=main-wake-snapshot metric=first-read-to-compositor-submit n=1 p50=50.000 p95=50.000")); free(text);
    text = report_text(22, 23);
    assert(strstr(text, "frame=23 association=main-wake-snapshot handoff=observed reads=0"));
    assert(strstr(text, "association=main-wake-snapshot metric=first-read-to-AHB-present-fence n=1 p50=30.000 p95=30.000 ms missing_frame=0 missing_handoff=0 no_read=1 missing_endpoint=0 invalid=0"));
    assert(strstr(text, "association=main-wake-snapshot metric=first-read-to-compositor-submit n=1 p50=20.000 p95=20.000 ms missing_frame=0 missing_handoff=0 no_read=1"));
    assert(!strstr(text, "read_group=reused") && !strstr(text, "read_group=new")); free(text);
    text = report_text(24, 24);
    assert(strstr(text, "association=main-wake-snapshot metric=first-read-to-compositor-submit n=0 p50=nan p95=nan ms missing_frame=0 missing_handoff=0 no_read=1")); free(text);
    text = report_text(27, 28);
    assert(strstr(text, "invalid_reads=1"));
    assert(strstr(text, "association=global-window-observation metric=first-read-to-compositor-submit n=0 p50=nan p95=nan ms missing_frame=0 missing_handoff=0 no_read=0 missing_endpoint=0 invalid=2")); free(text);
    text = report_text(29, 31);
    assert(strstr(text, "association=global-window-observation metric=last-read-to-AHB-present-fence n=1 p50=20.000 p95=20.000 ms missing_frame=1 missing_handoff=0 no_read=1"));
    assert(strstr(text, "association=main-wake-snapshot metric=last-read-to-AHB-present-fence n=0 p50=nan p95=nan ms missing_frame=1 missing_handoff=2")); free(text);
    text = report_text(32, 33);
    assert(strstr(text, "association=global-window-observation metric=first-read-to-AHB-present-fence n=1 p50=0.000 p95=0.000 ms missing_frame=0 missing_handoff=0 no_read=0 missing_endpoint=0 invalid=1")); free(text);
    text = report_text(34, 35);
    assert(strstr(text, "association=main-wake-snapshot metric=last-read-to-compositor-submit n=1 p50=370.000 p95=370.000 ms missing_frame=0 missing_handoff=0 no_read=0 missing_endpoint=0 invalid=1")); free(text);
    text = report_text(36, 36);
    assert(strstr(text, "AHB_apply=0 copy_buffer_post=1 missing_submission=0"));
    assert(strstr(text, "submit_source=copy-buffer-post first_to_submit_ms=10.000 last_to_submit_ms=10.000 first_to_AHB_fence_ms=nan last_to_AHB_fence_ms=nan"));
    assert(strstr(text, "association=global-window-observation metric=first-read-to-copy-buffer-post n=1 p50=10.000 p95=10.000"));
    assert(strstr(text, "association=global-window-observation metric=first-read-to-AHB-present-fence n=0 p50=nan p95=nan ms missing_frame=0 missing_handoff=0 no_read=0 missing_endpoint=0 invalid=0 read_group=all other_source=1")); free(text);
    text = report_text(37, 37);
    assert(strstr(text, "association=global-window-observation metric=first-read-to-compositor-submit n=1 p50=10.000 p95=10.000"));
    assert(strstr(text, "association=global-window-observation metric=last-read-to-compositor-submit n=0 p50=nan p95=nan ms missing_frame=0 missing_handoff=0 no_read=0 missing_endpoint=0 invalid=1"));
    assert(strstr(text, "first_to_AHB_fence_ms=30.000 last_to_AHB_fence_ms=10.000")); free(text);
    text = report_text(38, 38);
    assert(strstr(text, "association=global-window-observation metric=first-read-to-compositor-submit n=0 p50=nan p95=nan ms missing_frame=0 missing_handoff=0 no_read=0 missing_endpoint=1")); free(text);
    text = report_text(39, 39);
    assert(strstr(text, "association=main-wake-snapshot metric=first-read-to-compositor-submit n=0 p50=nan p95=nan ms missing_frame=0 missing_handoff=0 no_read=0 missing_endpoint=0 invalid=1")); free(text);
    /* Device regression: boot reads on main stop when gameplay moves input to
     * another guest thread. Fresh raw reads must not recycle main's boot stamp
     * into hundreds of seconds of fake per-frame latency at later wakes. */
    totk_ft_input_take(&drained);
    totk_ft_input_read(40, base + 1000 * ms, 0x1001, 3227);
    totk_ft_input_take(&group);
    assert(group.reads.last_sampling == 3227);
    for (uint64_t frame = 41; frame <= 44; frame++) {
        uint64_t args[] = {frame, base + (100000 + frame) * ms, 13281 + 2 * frame};
        assert(!pthread_create(&thread, NULL, migrated_input_sender, args));
        assert(!pthread_join(thread, NULL));
        totk_ft_input_take(&drained);
        assert(!drained.reads.reads);
        totk_ft_wake(frame, args[1] + 3 * ms, args[1] + 2 * ms, 0xabc, 0x1001, &drained);
        totk_ft_submission(frame, args[1] + 20 * ms, FT_SUBMIT_COPY_POST);
    }
    text = report_text(41, 44);
    assert(strstr(text, "association=global-window-observation metric=last-read-to-copy-buffer-post n=4 p50=19.000 p95=19.000"));
    assert(strstr(text, "association=main-wake-snapshot metric=last-read-to-copy-buffer-post n=0 p50=nan p95=nan ms missing_frame=0 missing_handoff=0 no_read=4"));
    assert(!strstr(text, "first_sampling=3227") && !strstr(text, "last_sampling=3227")); free(text);
}
static void reserve_cases(uint64_t base) {
    const uint64_t p = 33333333;
    /* At slot 100: two ready successors; at 101: one; at 102: none.
     * A delayed presenter call must retain the independent ready stamp. */
    for (uint64_t f = 99; f <= 106; f++) {
        uint64_t slot = base + (f - 98) * p;
        uint64_t ready = f <= 102 ? base + p : slot + 1000000;
        totk_ft_handoff(f, ready - 100);
        totk_ft_present_slot(f, slot, f == 104 ? 3 : 0);
        totk_ft_present_ready(f, ready);
        totk_ft_submission(f, slot, FT_SUBMIT_AHB_APPLY);
        totk_ft_compositor(f, slot, slot + (f == 103 ? 16666667 : 0));
        totk_ft_present(f, p, base + (f - 98) * 45000000);
    }
    char *text = report_text(100, 104);
    assert(strstr(text, "banked-after-slot(0/1/2)=6/1/1 hitches(absorbed/shown)=3/2 late-ready=2 skipped-slots=3 missing=0"));
    assert(strstr(text, "display-interval-hitches/pairs=1/5"));
    free(text);
    /* Missing readiness is not zero-latency/on-time evidence. */
    totk_ft_handoff(107, base);
    totk_ft_present_slot(107, base + 10 * p, 0);
    text = report_text(107, 107);
    assert(strstr(text, "banked-after-slot(0/1/2)=0/0/0"));
    assert(strstr(text, "missing=1 incomplete-lookahead=0"));
    free(text);
}
int main(int argc, char **argv) {
    int disabled = argc > 1 && !strcmp(argv[1], "off");
    assert(atomic_load(&totk_ft_enabled) == -1);
    assert(!totk_ft_work_frame());
    totk_ft_count(FT_COUNT_DRAWS, 1);
    assert(atomic_load(&totk_ft_enabled) == -1); /* hot counters never initialize the diagnostic */
    assert(!totk_ft_active_work.value[FT_COUNT_DRAWS]);
    setenv("TOTK_FRAME_TIMELINE", disabled ? "0" : "1", 1);
    assert(totk_ft_on() == !disabled);
    totk_ft_record r;
    if (disabled) {
        totk_ft_walk(1, 10, 20, 5); totk_ft_present(1, 30, 100);
        totk_ft_work_begin(1); totk_ft_count(FT_COUNT_DRAWS, 100); totk_ft_work_end();
        assert(!totk_ft_active_frame && !totk_ft_active_work.value[FT_COUNT_DRAWS]);
        totk_ft_count_frame(1, FT_COUNT_DESC_CREATES, 1);
        totk_ft_input_read(1, 99, 0x1001, 1);
        totk_ft_input_snapshot input; totk_ft_input_take(&input);
        assert(!input.reads.reads);
        totk_ft_submission(1, 100, FT_SUBMIT_COPY_POST);
        totk_ft_present_slot(1, 200, 0); totk_ft_present_ready(1, 100);
        assert(!totk_ft_work_frame());
        assert(!totk_ft_snapshot(1, &r)); assert(totk_ft_current() == 1);
        puts("frame timeline: disabled path passed"); return 0;
    }
    const uint64_t base = 1000000000, period = 33333333;
    synthetic(1, base, 20000000, base + 25000000);
    synthetic(2, base + period, 30000000, base + period + 35000000);
    synthetic(3, base + period * 2, 20000000, base + period * 2 + 25000000);
    assert(totk_ft_current() == 4);
    assert(totk_ft_snapshot(2, &r));
    assert(r.wake == base + period && r.ready == r.wake - 100000);
    assert(r.walks == 1 && r.walk_ns == 30000000 && r.walk_cpu_ns == 29000000);
    assert(r.join_ns == 500000 && r.slots == 1 && r.slot[0].ticket == 4);
    assert(r.slot[0].gpu_done == base + period + 31600000 && r.slot[0].gpu_ns == 900000);
    for (unsigned c = 0; c < FT_COUNT_N; c++) assert(r.work.value[c] == 4 * (c + 1));
    assert(r.acquire_pacing_ns == 120000 && r.acquire_texture_ns == 80000);
    /* A later submit callback with display=0 must not erase actual display. */
    uint64_t display = r.display;
    totk_ft_compositor(2, r.apply, 0);
    assert(totk_ft_snapshot(2, &r) && r.display == display);
    /* Repeated address waits after the first walk cannot replace frame-start wake. */
    totk_ft_wake(2, base + period + 999, 0, 0x99, 0x1001, NULL);
    assert(totk_ft_snapshot(2, &r) && r.wake_addr == 0x123400);
    char *text = NULL; size_t size = 0; FILE *out = open_memstream(&text, &size);
    assert(out); totk_ft_test_report(out, 1, 3); assert(!fclose(out));
    assert(strstr(text, "clock=game n=2"));
    assert(strstr(text, "[frame-tl late] frame=2 "));
    assert(!strstr(text, "[frame-tl late] frame=3 "));
    assert(strstr(text, "dominant=walk growth=10.000"));
    assert(strstr(text, "late=1 dominant(wake/walk/submit/gpu/acquire/present/unknown)=0/1/0/0/0/0/0"));
    assert(strstr(text, "slots=[4:"));
    assert(strstr(text, " draws=4 passes=8 chunks=12"));
    assert(strstr(text, "class=late n=1 means_per_frame draws=4.000 passes=8.000 chunks=12.000"));
    assert(strstr(text, "class=on-time n=1 means_per_frame draws=6.000 passes=12.000 chunks=18.000"));
    assert(strstr(text, "acquire_pacing_ms=0.120 acquire_texture_ms=0.080")); free(text);
    /* Entirely missing display/compositor evidence remains missing, not copied
     * from game-present or GPU completion timestamps. */
    totk_ft_present(4, period, base + period * 3 + 25000000);
    text = NULL; size = 0; out = open_memstream(&text, &size); assert(out);
    totk_ft_test_report(out, 4, 4); assert(!fclose(out));
    assert(strstr(text, "clock=display n=0 missing")); free(text);
    /* Interval changes use the current frame's target, with a strict >4ms gate. */
    totk_ft_present(5, 16666667, base + period * 3 + 25000000 + 20666667);
    text = NULL; size = 0; out = open_memstream(&text, &size); assert(out);
    totk_ft_test_report(out, 5, 5); assert(!fclose(out));
    assert(!strstr(text, "[frame-tl late]")); free(text);
    totk_ft_present(6, 16666667, base + period * 3 + 25000000 + 20666667 * 2 + 1);
    text = NULL; size = 0; out = open_memstream(&text, &size); assert(out);
    totk_ft_test_report(out, 6, 6); assert(!fclose(out));
    assert(strstr(text, "[frame-tl late] frame=6 ")); free(text);
    input_cases(base);
    reserve_cases(base);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, slot_writer, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 1000; i++) {
        if (totk_ft_snapshot(700, &r)) {
            assert(r.slots <= FT_SLOTS);
            assert(r.input_window.reads <= FT_SLOTS);
            if (r.input_window.reads) assert(r.input_window.first && r.input_window.last >= r.input_window.first);
            for (unsigned s = 0; s < r.slots; s++) if (r.slot[s].gpu_done) assert(r.slot[s].gpu_done >= r.slot[s].queue_end);
        }
    }
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(totk_ft_snapshot(700, &r) && r.slots == FT_SLOTS && !r.lost_slots);
    assert(r.input_window.reads == FT_SLOTS && r.input_window.mixed_threads);
    assert(r.input_window.first == 101000 && r.input_window.last == 164000);
    assert(r.input_window.first_thread == 0x2000 && r.input_window.last_thread == 0x2003);
    for (unsigned i = 0; i < r.slots; i++) assert(r.slot[i].seal && r.slot[i].queue_end && r.slot[i].gpu_done);
    assert(r.work.value[FT_COUNT_DESC_CREATES] == FT_SLOTS);
    for (unsigned writer = 0; writer < 4; writer++) {
        assert(totk_ft_snapshot(710 + writer, &r));
        uint64_t expected = 0;
        for (unsigned i = 0; i < FT_SLOTS / 4; i++) expected += 1 + writer * (FT_SLOTS / 4) + i;
        assert(r.work.value[FT_COUNT_DRAWS] == expected);
    }
    totk_ft_slot_seal(700, 1000, 123); assert(totk_ft_snapshot(700, &r) && r.lost_slots == 1);
    for (unsigned i = 0; i < FT_WALKS + 2; i++) totk_ft_walk(701, 100 + i * 10, 105 + i * 10, 3);
    assert(totk_ft_snapshot(701, &r) && r.walks == FT_WALKS && r.lost_walks == 2 && r.walk_ns == (FT_WALKS + 2) * 5);
    totk_ft_present(1 + FT_RING, period, base + period * FT_RING);
    assert(!totk_ft_snapshot(1, &r));
    totk_ft_slot_gpu(1, 2, 999999, 999); /* expired owner cannot corrupt new generation */
    totk_ft_count_frame(1, FT_COUNT_PIPE_CREATES, 99);
    totk_ft_input_read(1, base, 0x1001, 999); /* expired raw window is rejected too */
    assert(totk_ft_snapshot(1 + FT_RING, &r) && !r.slots && r.present == base + period * FT_RING);
    assert(!r.work.value[FT_COUNT_PIPE_CREATES]);
    assert(!r.input_window.reads && !r.input_main_wake_received);
    totk_ft_input_snapshot expired_input;
    totk_ft_input_take(&expired_input);
    assert(expired_input.reads.last_sampling == 999);
    /* Full report windows compare means, not sums or the previous frame's work.
     * Frame 2000 is only the interval anchor and must not enter either group. */
    uint64_t now = base * 100;
    totk_ft_present(2000, period, now);
    for (uint64_t frame = 2001; frame <= 2300; frame++) {
        int late = frame & 1;
        now += late ? 50000000 : 20000000;
        uint64_t read_first = now - (frame - 2000) * 1000000;
        totk_ft_input_read(frame, read_first, 0x1001, frame * 2);
        totk_ft_input_read(frame, read_first + 500000, 0x1001, frame * 2 + 1);
        totk_ft_input_snapshot input;
        totk_ft_input_take(&input);
        totk_ft_wake(frame, now - 100000, now - 200000, 0xabc, 0x1001, &input);
        totk_ft_compositor(frame, now, now + 5000000);
        totk_ft_submission(frame, now, FT_SUBMIT_AHB_APPLY);
        totk_ft_work_begin(frame);
        for (unsigned c = 0; c < FT_COUNT_N; c++)
            totk_ft_count((enum totk_ft_counter)c, (late ? 10 : 2) * (c + 1));
        totk_ft_work_end();
        totk_ft_present(frame, period, now);
    }
    text = NULL; size = 0; out = open_memstream(&text, &size); assert(out);
    totk_ft_test_report(out, 2001, 2300); assert(!fclose(out));
    assert(strstr(text, "class=late n=150 means_per_frame draws=10.000 passes=20.000 chunks=30.000"));
    assert(strstr(text, "class=on-time n=150 means_per_frame draws=2.000 passes=4.000 chunks=6.000"));
    assert(strstr(text, "late=150 dominant("));
    assert(strstr(text, "association=main-wake-snapshot metric=first-read-to-compositor-submit n=300 p50=150.000 p95=285.000"));
    assert(strstr(text, "association=main-wake-snapshot metric=last-read-to-compositor-submit n=300 p50=149.500 p95=284.500"));
    assert(strstr(text, "association=main-wake-snapshot metric=first-read-to-AHB-present-fence n=300 p50=155.000 p95=290.000"));
    assert(strstr(text, "association=global-window-observation metric=last-read-to-AHB-present-fence n=300 p50=154.500 p95=289.500")); free(text);
    puts("frame timeline: frame IDs, intervals, input read groups, main-wake ownership/no-read and reader migration, latency percentiles/missing/invalid evidence, concurrent owners, capacity and wraparound passed");
    return 0;
}
