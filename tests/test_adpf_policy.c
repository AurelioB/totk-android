#include "../games/totk/runtime/adpf_policy.h"
#include <assert.h>
#include <stdio.h>

static unsigned creates, closes, updates, sets, reports;
static int refuse_create, refuse_update, refuse_set, refuse_report;
static int64_t actual, target_seen;
static int marker;
static void *create(void *manager, const int32_t *tids, size_t n, int64_t target) {
    assert(manager == &marker && n && tids[0] > 0); creates++; target_seen = target;
    return refuse_create ? NULL : &marker;
}
static void close_session(void *s) { assert(s == &marker); closes++; }
static int update(void *s, int64_t target) { assert(s == &marker); updates++; target_seen = target; return refuse_update; }
static int set_threads(void *s, const int32_t *tids, size_t n) { assert(s == &marker && tids[0] > 0 && n); sets++; return refuse_set; }
static int report(void *s, int64_t ns) { assert(s == &marker); reports++; actual = ns; return refuse_report; }
int main(void) {
    assert(totk_adpf_target(16666667, 0, 0) == 33333333);
    assert(totk_adpf_target(33333333, 0, 1) == 33333333);
    assert(totk_adpf_target(16666667, 1, 1) == 33333333);
    assert(totk_adpf_target(16666667, 1, 0) == 16666667);
    assert(totk_adpf_target(33333333, 1, 0) == 33333333); /* auto drop */
    assert(totk_adpf_target(0, 1, 0) == 33333333);
    assert(totk_frame_rate(30, 0, 0) == 30);
    assert(totk_frame_rate(30, 0, 1) == 60);
    assert(totk_frame_rate(30, 1, 0) == 60);
    assert(totk_frame_rate(60, 0, 0) == 60);
    assert(totk_adpf_work(0, 100, 0) == 0);
    assert(totk_adpf_work(100, 99, 0) == 0);
    assert(totk_adpf_work(100, 40000100, 10000000) == 30000000);
    assert(totk_adpf_work(100, 40000100, 40000000) == 0);
    assert(totk_adpf_work(100, 250000100, 0) == 0);
    assert(totk_adpf_work(100, 250000099, 0) == 249999999);
    totk_adpf_api api = {.manager=&marker, .create=create, .update=update,
        .report=report, .set_threads=set_threads, .close=close_session};
    totk_adpf_state state = {0}; int32_t tids[] = {11, 22, 33};
    assert(!totk_adpf_sync(&state, &api, tids, 0, 33333333));
    assert(!totk_adpf_sync(&state, &api, tids, 1, 0));
    assert(totk_adpf_sync(&state, &api, tids, 1, 33333333) == TOTK_ADPF_CREATED);
    assert(creates == 1 && target_seen == 33333333);
    for (int frame=0; frame<1000; frame++) {
        assert(!totk_adpf_sync(&state, &api, tids, 1, 33333333));
        assert(!totk_adpf_report(&state, &api, 23000000));
    }
    assert(creates == 1 && sets == 0 && updates == 0 && reports == 1000 && actual == 23000000);
    assert(!totk_adpf_report(&state, &api, 0));
    assert(!totk_adpf_report(&state, &api, 250000000)); assert(reports == 1000);
    assert(totk_adpf_sync(&state, &api, tids, 2, 16666667) == (TOTK_ADPF_THREADS | TOTK_ADPF_TARGET));
    assert(sets == 1 && updates == 1 && creates == 1 && target_seen == 16666667);
    api.set_threads = NULL; /* Android33 membership change */
    assert(totk_adpf_sync(&state, &api, tids, 3, 16666667) == TOTK_ADPF_CREATED);
    assert(creates == 2 && closes == 1 && state.count == 3);
    refuse_update = 1;
    assert(totk_adpf_sync(&state, &api, tids, 3, 33333333) == TOTK_ADPF_CREATED);
    assert(creates == 3 && closes == 2 && state.target == 33333333);
    refuse_report = 1;
    assert(totk_adpf_report(&state, &api, 22000000) == TOTK_ADPF_REPORT_ERROR);
    totk_adpf_close(&state, &api); assert(!state.session && closes == 3);
    state = (totk_adpf_state){0}; refuse_create = 1;
    assert(totk_adpf_sync(&state, &api, tids, 1, 33333333) == TOTK_ADPF_REFUSED);
    for (int frame=0; frame<1000; frame++) assert(!totk_adpf_sync(&state, &api, tids, 1, 33333333));
    assert(creates == 4); /* no per-frame allocation/retry after refusal */
    refuse_create = refuse_update = refuse_report = 0;
    assert(totk_adpf_sync(&state, &api, tids, 1, 16666667) == TOTK_ADPF_CREATED);
    api.set_threads = set_threads; refuse_set = 1;
    assert(totk_adpf_sync(&state, &api, tids, 2, 16666667) == TOTK_ADPF_CREATED);
    assert(creates == 6 && closes == 4);
    totk_adpf_close(&state, &api);
    state = (totk_adpf_state){0}; api.manager = NULL;
    assert(totk_adpf_sync(&state, &api, tids, 1, 33333333) == TOTK_ADPF_REFUSED);
    assert(!totk_adpf_report(&state, &api, 22000000));
    puts("ADPF policy: durations, lattice, fake API lifecycle, failures and steady-state passed");
    return 0;
}
