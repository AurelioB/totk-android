#include "fps_auto_scene.h"
#include "fps_auto.h"
#include <assert.h>
static void loading_rejects_decisions(void) {
    const uint64_t refresh = 16666667, start = 1000000000ull;
    fps_auto_scene scene = {0};
    fps_auto_policy p;
    fps_auto_reset(&p, start, refresh);
    uint64_t first_world = 0, changed = 0;
    unsigned inhibited = 0;
    for (unsigned i = 1; i < 2400; i++) {
        uint64_t now = start + i * 20000000ull; /* Actual 50 fps, not late READY at 60. */
        unsigned reason = fps_auto_scene_observe(&scene, now, i < 600 ? 1473 : 2400, 0);
        if (i == 600) first_world = now;
        if (reason != FPS_SCENE_READY) {
            if (!inhibited) fps_auto_inhibit(&p, now);
            inhibited = 1;
            assert(fps_auto_apply(&p, now) == 1);
        } else {
            inhibited = 0;
            fps_auto_game_present(&p, now);
            fps_auto_observe(&p, now, refresh + 3000000, refresh + 3000000);
            if (fps_auto_apply(&p, now) == 2) { changed = now; break; }
        }
    }
    assert(changed >= first_world + FPS_AUTO_SCENE_SETTLE_NS + 2 * FPS_AUTO_DROP_WINDOW_NS);
    /* A later load cannot train a return to60 despite nominally fast samples. */
    for (unsigned i = 1; i <= 400; i++) {
        uint64_t now = changed + i * 2 * refresh;
        assert(fps_auto_scene_observe(&scene, now, 2400, 1) == FPS_SCENE_LOADING);
        fps_auto_inhibit(&p, now);
        assert(fps_auto_apply(&p, now) == 2 && !p.count && p.desired == 2);
    }
}

static void grace_extends_holds(void) {
    const uint64_t second = 1000000000ull, world = 2 * second;
    fps_auto_scene s = {0};
    fps_auto_scene_grace(&s, second, second);
    assert(s.hold_until_ns == 2 * second && !s.world_seen && !s.last_ns);
    assert(fps_auto_scene_observe(&s, second, 1473, 0) == FPS_SCENE_BEFORE_WORLD);
    assert(fps_auto_scene_observe(&s, world, 2400, 0) == FPS_SCENE_SETTLING);
    uint64_t loading_deadline = world + FPS_AUTO_SCENE_SETTLE_NS;
    fps_auto_scene_grace(&s, world, second);
    assert(s.hold_until_ns == loading_deadline); /* A short event cannot undo ten seconds of settling. */
    assert(fps_auto_scene_observe(&s, loading_deadline, 2400, 0) == FPS_SCENE_READY);

    uint64_t start = loading_deadline + second;
    fps_auto_scene_grace(&s, start, second);
    assert(s.hold_until_ns == start + second);
    fps_auto_scene_grace(&s, start, second);
    fps_auto_scene_grace(&s, start + second / 4, second / 4);
    fps_auto_scene_grace(&s, start + second / 2, 0);
    assert(s.hold_until_ns == start + second); /* Repeats, contained overlaps and zero do not accumulate. */
    fps_auto_scene_grace(&s, start + second / 2, second);
    assert(s.hold_until_ns == start + second + second / 2);
    assert(fps_auto_scene_observe(&s, s.hold_until_ns - 1, 2400, 0) == FPS_SCENE_SETTLING);
    assert(fps_auto_scene_observe(&s, s.hold_until_ns, 2400, 0) == FPS_SCENE_READY);

    /* Short grace never replaces an explicit load's longer observation gate. */
    start = s.hold_until_ns + second;
    fps_auto_scene_grace(&s, start, second);
    assert(fps_auto_scene_observe(&s, start, 2400, 1) == FPS_SCENE_LOADING);
    assert(s.hold_until_ns == start + FPS_AUTO_SCENE_SETTLE_NS);
    fps_auto_scene_grace(&s, start + second, second);
    assert(s.hold_until_ns == start + FPS_AUTO_SCENE_SETTLE_NS);

    s = (fps_auto_scene){0};
    fps_auto_scene_grace(&s, 0, 0);
    assert(!s.hold_until_ns);
    fps_auto_scene_grace(&s, 0, 1);
    assert(s.hold_until_ns == 1);
    fps_auto_scene_grace(&s, UINT64_MAX - 2, 2);
    assert(s.hold_until_ns == UINT64_MAX);
    s.hold_until_ns = 0;
    fps_auto_scene_grace(&s, UINT64_MAX - 2, 3);
    assert(s.hold_until_ns == UINT64_MAX);
    fps_auto_scene_grace(&s, 1, 1);
    assert(s.hold_until_ns == UINT64_MAX);
    s.hold_until_ns = 0;
    fps_auto_scene_grace(&s, UINT64_MAX, UINT64_MAX);
    assert(s.hold_until_ns == UINT64_MAX);
}

static void grace_rejects_pending_decisions(unsigned interval) {
    const uint64_t refresh = 16666667, second = 1000000000ull;
    fps_auto_scene scene = {0};
    uint64_t now = second;
    assert(fps_auto_scene_observe(&scene, now, 2400, 0) == FPS_SCENE_SETTLING);
    now += FPS_AUTO_SCENE_SETTLE_NS;
    assert(fps_auto_scene_observe(&scene, now, 2400, 0) == FPS_SCENE_READY);
    fps_auto_policy p;
    fps_auto_reset(&p, now, refresh);
    for (unsigned i = 0; i < 1800 && p.desired == 1; i++) {
        now += 20000000ull; /* A genuine sub-55-fps raw game stream. */
        assert(fps_auto_scene_observe(&scene, now, 2400, 0) == FPS_SCENE_READY);
        fps_auto_game_present(&p, now);
        assert(fps_auto_observe(&p, now, refresh + 3000000, refresh + 3000000).valid);
    }
    assert(p.interval == 1 && p.desired == 2);
    if (interval == 2) {
        assert(fps_auto_apply(&p, now) == 2);
        for (unsigned i = 0; i < 1800 && p.desired == 2; i++) {
            now += 2 * refresh;
            assert(fps_auto_scene_observe(&scene, now, 2400, 0) == FPS_SCENE_READY);
            fps_auto_game_present(&p, now);
            assert(fps_auto_observe(&p, now, refresh - 3000000, 2 * refresh).valid);
        }
        assert(p.interval == 2 && p.desired == 1);
    }
    assert(p.count);
    uint64_t generation = p.generation;
    fps_auto_scene_grace(&scene, now, second);
    assert(fps_auto_scene_observe(&scene, now, 2400, 0) == FPS_SCENE_SETTLING);
    fps_auto_inhibit(&p, now); /* The runtime also rejects queued completions from the old epoch. */
    assert(p.generation == generation + 1 && p.reason == FPS_AUTO_REASON_INHIBITED);
    assert(!p.count && !p.have_observe && !p.drop_streak && p.desired == interval);
    assert(!p.game_count && !p.have_game_present && !p.last_game_count && !p.last_game_fps_milli);
    assert(fps_auto_apply(&p, now) == interval); /* Neither pending direction may commit at acquire. */
    assert(fps_auto_scene_observe(&scene, now + second - 1, 2400, 0) == FPS_SCENE_SETTLING);
    assert(fps_auto_apply(&p, now + second - 1) == interval && !p.count);
    now += second;
    assert(fps_auto_scene_observe(&scene, now, 2400, 0) == FPS_SCENE_READY);
    fps_auto_game_present(&p, now);
    assert(fps_auto_observe(&p, now, refresh - 3000000, interval * refresh).valid);
    assert(p.count == 1 && p.desired == interval);
    assert(p.have_game_present && p.game_count == (interval == 1 ? 1u : 0u));
    assert(fps_auto_apply(&p, now) == interval); /* Expiry needs fresh evidence, not the pre-grace decision. */
}

int main(void) {
    const uint64_t second = 1000000000ull;
    fps_auto_scene s = {0};
    for (uint64_t i = 1; i < 30; i++)
        assert(fps_auto_scene_observe(&s, i * second, 1473, 0) == FPS_SCENE_BEFORE_WORLD);
    uint64_t world = 30 * second;
    assert(fps_auto_scene_observe(&s, world, 2400, 0) == FPS_SCENE_SETTLING);
    assert(fps_auto_scene_observe(&s, world + 10 * second - 1, 2400, 0) == FPS_SCENE_SETTLING);
    assert(fps_auto_scene_observe(&s, world + 10 * second, 2400, 0) == FPS_SCENE_READY);
    assert(fps_auto_scene_observe(&s, world + 11 * second, 799, 0) == FPS_SCENE_LOW_DRAWS);
    assert(fps_auto_scene_observe(&s, world + 12 * second, 2400, 0) == FPS_SCENE_SETTLING);
    assert(fps_auto_scene_observe(&s, world + 21 * second, 2400, 0) == FPS_SCENE_READY);
    assert(fps_auto_scene_observe(&s, world + 22 * second, 2400, 1) == FPS_SCENE_LOADING);
    assert(fps_auto_scene_observe(&s, world + 32 * second - 1, 2400, 0) == FPS_SCENE_SETTLING);
    assert(fps_auto_scene_observe(&s, world + 32 * second, 2400, 0) == FPS_SCENE_READY);
    uint64_t pipes = world + 33 * second;
    for (unsigned i = 0; i < 7; i++) assert(!fps_auto_scene_pipeline(&s, pipes + i));
    assert(fps_auto_scene_observe(&s, pipes + 7, 2400, 0) == FPS_SCENE_READY);
    assert(fps_auto_scene_pipeline(&s, pipes + 8));
    assert(fps_auto_scene_observe(&s, pipes + 9, 2400, 0) == FPS_SCENE_SETTLING);
    assert(fps_auto_scene_observe(&s, pipes + 8 + 10 * second, 2400, 0) == FPS_SCENE_READY);
    for (unsigned i = 1; i < 20; i++) assert(!fps_auto_scene_pipeline(&s, pipes + (11 + i) * second + i));
    assert(fps_auto_scene_observe(&s, 1, 2400, 0) == FPS_SCENE_BEFORE_WORLD);
    assert(!s.world_seen);
    s = (fps_auto_scene){0};
    assert(fps_auto_scene_observe(&s, UINT64_MAX - 5, 2400, 0) == FPS_SCENE_SETTLING);
    assert(s.hold_until_ns == UINT64_MAX);
    assert(fps_auto_scene_observe(&s, UINT64_MAX - 1, 2400, 0) == FPS_SCENE_SETTLING);
    loading_rejects_decisions();
    grace_extends_holds();
    grace_rejects_pending_decisions(1);
    grace_rejects_pending_decisions(2);
    return 0;
}
