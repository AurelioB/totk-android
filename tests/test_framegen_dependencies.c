/* Standalone provenance goldens, no Android/Vulkan/game data or device:
   cc -std=c11 -O2 -Wall -Wextra -Werror tests/test_framegen_dependencies.c
      games/totk/android/gfx/vk_canvas.c -o <scratch>/framegendeptest */
#include "../games/totk/android/gfx/framegen_dependencies.h"
#include "../games/totk/android/gfx/vk_canvas.h"
#include "../games/totk/android/gfx/framegen_passes.h"
#include <stdio.h>
#include <stdlib.h>

static fg_dep_frame frame;
static unsigned checks, failures;
#define CHECK(c, message) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); } } while (0)

static fg_dep_key key(uint64_t backing) {
    return (fg_dep_key){.backing = backing, .aspect = 1};
}
static fg_dep_value seed(uint64_t backing, fg_dep_mask origin, uint32_t pass) {
    return fg_dep_write(&frame, key(backing), NULL, pass, origin, 0, 0, 0);
}
static fg_dep_value compose(uint64_t backing, fg_dep_value a, fg_dep_value b,
                            uint32_t pass, int camera_present, fg_dep_mask matches) {
    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    fg_dep_sources_add(&frame, &sources, a);
    fg_dep_sources_add(&frame, &sources, b);
    return fg_dep_write(&frame, key(backing), &sources, pass, 0, camera_present, matches, 0);
}
static fg_dep_result select_value(fg_dep_value value, unsigned expected) {
    unsigned index = 999;
    fg_dep_result result = fg_dep_select(&frame, value, &index);
    if (result == FG_DEP_SELECTED) CHECK(index == expected, "selected candidate index matches ancestor");
    else CHECK(index == 999, "refusal does not publish a fallback index");
    return result;
}

static void independent_gbuffers(void) {
    fg_dep_reset(&frame);
    fg_dep_mask reflection = fg_dep_candidate(&frame, 1);
    fg_dep_mask main = fg_dep_candidate(&frame, 1);
    fg_dep_value reflected = seed(1, reflection, 4);
    fg_dep_value world = seed(2, main, 9);
    fg_dep_value lighting = compose(3, world, (fg_dep_value){0}, 12, 0, 0);
    fg_dep_value canvas = compose(4, lighting, (fg_dep_value){0}, 17, 0, 0);
    CHECK(frame.candidate_count == 2, "both independent G-buffers are registered");
    CHECK(reflected.origins == reflection, "unused reflection is still represented");
    CHECK(canvas.origins == main && canvas.eligible == main, "only sampled root reaches canvas");
    CHECK(select_value(canvas, 1) == FG_DEP_SELECTED, "select connected main despite second G-buffer");
    CHECK(lighting.writer == 12 && canvas.writer == 17, "writer order survives chain for diagnostics");
    CHECK(world.version < lighting.version && lighting.version < canvas.version, "chain nodes have distinct monotonic versions");
    CHECK(world.epoch == canvas.epoch, "chain is entirely from this real frame");

    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    fg_dep_sources_add(&frame, &sources, world);
    fg_dep_sources_add(&frame, &sources, fg_dep_read(&frame, key(2)));
    CHECK(sources.count == 1, "repeated or compatible-alias reads share one source version");
    CHECK(sources.source[0].writer == 9 && sources.source[0].origins == main, "immediate source evidence retains writer and root mask");
    CHECK(sources.source[0].version == world.version, "source evidence links exact consumed version");
}

static void cameras_and_mixed_roots(void) {
    fg_dep_reset(&frame);
    fg_dep_mask main = fg_dep_candidate(&frame, 1);
    fg_dep_mask reflection = fg_dep_candidate(&frame, 1);
    fg_dep_value world = seed(1, main, 1), reflected = seed(2, reflection, 2);
    fg_dep_value mixed = compose(3, world, reflected, 3, 0, 0);
    CHECK(mixed.origins == (main | reflection), "mixed scene retains every raw ancestor");
    CHECK(select_value(mixed, 0) == FG_DEP_AMBIGUOUS, "two sampled roots without camera refuse");
    fg_dep_value canvas = compose(4, mixed, (fg_dep_value){0}, 4, 1, main);
    CHECK(canvas.origins == (main | reflection), "downstream camera does not hide diagnostic roots");
    CHECK(canvas.eligible == main, "authoritative main camera excludes reflection eligibility");
    CHECK(select_value(canvas, 0) == FG_DEP_SELECTED, "downstream camera resolves mixed reflection roots");
    fg_dep_value later = compose(5, canvas, (fg_dep_value){0}, 5, 0, reflection);
    CHECK(later.eligible == main, "absent downstream camera preserves prior eligibility");
    CHECK(select_value(later, 0) == FG_DEP_SELECTED, "camera-free postprocessing preserves selection");
    fg_dep_value mismatch = compose(6, later, (fg_dep_value){0}, 6, 1, reflection);
    CHECK(mismatch.origins == (main | reflection) && !mismatch.eligible, "later mismatch never restores a filtered raw root");
    CHECK(select_value(mismatch, 0) == FG_DEP_NO_ELIGIBLE, "candidate camera mismatch refuses");
    fg_dep_value invalid_camera = compose(7, world, (fg_dep_value){0}, 7, 1, 0);
    CHECK(select_value(invalid_camera, 0) == FG_DEP_NO_ELIGIBLE, "present camera with no valid match refuses");
    fg_dep_value unrelated_match = compose(8, world, (fg_dep_value){0}, 8, 1, reflection);
    CHECK(!unrelated_match.eligible, "camera cannot introduce an unconsumed candidate");
    CHECK(select_value(unrelated_match, 0) == FG_DEP_NO_ELIGIBLE, "unconnected camera match is not proof");

    fg_dep_reset(&frame);
    fg_dep_mask invalid = fg_dep_candidate(&frame, 0);
    main = fg_dep_candidate(&frame, 1);
    fg_dep_value bad = seed(1, invalid, 1);
    world = seed(2, main, 2);
    CHECK(bad.origins == invalid && !bad.eligible, "invalid camera still has a diagnostic origin");
    CHECK(select_value(bad, 0) == FG_DEP_NO_ELIGIBLE, "invalid candidate can never select");
    fg_dep_value inconclusive = compose(4, bad, world, 3, 0, 0);
    CHECK(inconclusive.eligible == main && inconclusive.unresolved == (invalid | main),
          "invalid extra raw root remains unresolved without a downstream camera");
    CHECK(select_value(inconclusive, 0) == FG_DEP_AMBIGUOUS,
          "a valid root cannot silently win by discarding an invalid sampled root");
    canvas = compose(3, bad, world, 3, 1, main);
    CHECK(canvas.origins == (invalid | main) && canvas.eligible == main, "invalid extra root does not erase matching main ancestry");
    CHECK(select_value(canvas, 1) == FG_DEP_SELECTED, "valid downstream match can exclude invalid candidate");
}

static void frame_and_history_separation(void) {
    fg_dep_reset(&frame);
    fg_dep_mask main = fg_dep_candidate(&frame, 1);
    fg_dep_value previous = seed(1, main, 1);
    fg_dep_sources old_sources;
    fg_dep_sources_init(&old_sources);
    fg_dep_sources_add(&frame, &old_sources, previous);
    fg_dep_reset(&frame);
    CHECK(!fg_dep_read(&frame, key(1)).known, "previous-frame resource is absent without current production");
    CHECK(frame.count == 0 && frame.candidate_count == 0, "reset separates all candidate and resource bookkeeping");
    fg_dep_value history_copy = fg_dep_copy(&frame, key(2), key(1), 1, 0);
    CHECK(!history_copy.origins && history_copy.known, "stale history copy cannot invent current roots");
    CHECK(select_value(history_copy, 0) == FG_DEP_NO_ORIGIN, "history-only composition cannot select");
    CHECK(select_value(previous, 0) == FG_DEP_STALE, "retained previous-frame snapshot refuses");

    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    fg_dep_sources_add(&frame, &sources, previous);
    fg_dep_value stale = fg_dep_write(&frame, key(3), &sources, 2, 0, 0, 0, 0);
    CHECK(stale.tainted && !stale.origins && sources.count == 0, "old snapshot creates no current-frame source edge");
    CHECK(select_value(stale, 0) == FG_DEP_TAINTED, "stale snapshot misuse remains fail closed");
    stale = fg_dep_write(&frame, key(4), &old_sources, 3, 0, 0, 0, 0);
    CHECK(stale.tainted && !stale.origins && !stale.tags, "old aggregate cannot reclassify its roots into this frame");

    fg_dep_value before_write = fg_dep_read(&frame, key(5));
    main = fg_dep_candidate(&frame, 1);
    fg_dep_value current = seed(5, main, 4);
    fg_dep_value too_early = compose(6, before_write, (fg_dep_value){0}, 5, 0, 0);
    CHECK(!before_write.known && !too_early.origins, "read-before-write snapshot never follows a later producer");
    CHECK(select_value(too_early, 0) == FG_DEP_NO_ORIGIN, "read-before-write cannot prove a current frame path");
    CHECK(select_value(current, 0) == FG_DEP_SELECTED, "new frame root can select independently of old origin bit reuse");
    fg_dep_sources_add(&frame, &old_sources, current);
    CHECK(old_sources.value.tainted && old_sources.value.origins == main && old_sources.count == 1,
          "reusing old collector removes old evidence and preserves conservative taint");
}

static void overwrites_clears_and_unknowns(void) {
    fg_dep_reset(&frame);
    fg_dep_mask main = fg_dep_candidate(&frame, 1);
    fg_dep_mask reflection = fg_dep_candidate(&frame, 1);
    fg_dep_value world = seed(1, main, 1), reflected = seed(2, reflection, 2);
    fg_dep_value consumed = compose(3, world, (fg_dep_value){0}, 3, 0, 0);
    fg_dep_value clear = fg_dep_clear(&frame, key(1), 4);
    CHECK(clear.known && !clear.origins && !clear.eligible && !clear.tainted, "full clear kills old proof without adding uncertainty");
    CHECK(clear.version != world.version, "clear creates a new attachment version");
    CHECK(world.origins == main && consumed.origins == main, "clear never mutates already consumed snapshots");
    CHECK(select_value(fg_dep_copy(&frame, key(4), key(1), 5, 0), 0) == FG_DEP_NO_ORIGIN, "reading after clear cannot find old root");
    fg_dep_value replaced = seed(1, reflection, 6);
    CHECK(replaced.origins == reflection, "full replacement does not retain old destination ancestry");
    fg_dep_value partial = compose(1, fg_dep_read(&frame, key(1)), world, 7, 0, 0);
    CHECK(partial.origins == (main | reflection), "explicit LOAD/blend/partial retention combines old and new roots");
    CHECK(select_value(partial, 0) == FG_DEP_AMBIGUOUS, "partial overwrite cannot discard retained ambiguity");
    fg_dep_value copied = fg_dep_copy(&frame, key(1), key(2), 8, 0);
    CHECK(copied.origins == reflection, "full copy replaces destination root lineage");
    (void)seed(1, main, 9);
    copied = fg_dep_copy(&frame, key(1), key(2), 10, 1);
    CHECK(copied.origins == (main | reflection), "partial copy preserves old destination root");

    fg_dep_candidate_invalidate(&frame, main);
    CHECK(select_value(consumed, 0) == FG_DEP_NO_ELIGIBLE, "overwritten retained depth revokes already consumed main root");
    CHECK(select_value(reflected, 1) == FG_DEP_SELECTED, "unrelated live candidate is not revoked");
    CHECK(consumed.origins == main, "revocation keeps raw root evidence available");
    CHECK(select_value(copied, 0) == FG_DEP_AMBIGUOUS,
          "revoking one mixed root does not heuristically choose the remaining live root");
    fg_dep_value resolved = compose(7, copied, (fg_dep_value){0}, 10, 1, reflection);
    CHECK(select_value(resolved, 1) == FG_DEP_SELECTED,
          "authoritative downstream camera can exclude a revoked non-main root");

    fg_dep_value unknown = fg_dep_invalidate(&frame, key(2), 11);
    CHECK(unknown.known && unknown.tainted && !unknown.origins && !unknown.eligible,
          "unsupported compute/alias write kills prior proof and marks unknown content");
    fg_dep_value tainted = compose(5, reflected, unknown, 12, 1, reflection);
    CHECK(select_value(tainted, 0) == FG_DEP_TAINTED, "camera filtering cannot repair unknown dataflow");
    CHECK(!frame.failed, "local unknown content does not exhaust the whole frame");
    (void)fg_dep_clear(&frame, key(2), 13);
    fg_dep_value recovered = compose(6, reflected, fg_dep_read(&frame, key(2)), 14, 0, 0);
    CHECK(select_value(recovered, 1) == FG_DEP_SELECTED, "known clear permits a new unrelated clean chain");
    CHECK(tainted.tainted, "later clear does not mutate previously consumed unknown version");
}

static void aliases_and_subresources(void) {
    fg_dep_reset(&frame);
    fg_dep_mask a = fg_dep_candidate(&frame, 1), b = fg_dep_candidate(&frame, 1);
    fg_dep_key mip0 = key(7), mip1 = key(7), layer1 = key(7), depth = key(7);
    mip1.mip = 1; layer1.layer = 1; depth.aspect = 2;
    fg_dep_value root = fg_dep_write(&frame, mip0, NULL, 1, a, 0, 0, 0);
    CHECK(fg_dep_read(&frame, key(7)).version == root.version, "compatible descriptor aliases use the same canonical backing");
    CHECK(!fg_dep_read(&frame, key(8)).known, "incompatible descriptor intern ID is never treated as an alias");
    CHECK(!fg_dep_read(&frame, mip1).known && !fg_dep_read(&frame, layer1).known && !fg_dep_read(&frame, depth).known,
          "mip layer and aspect cannot borrow another subresource's root");
    fg_dep_value other_mip = fg_dep_write(&frame, mip1, NULL, 2, b, 0, 0, 0);
    (void)fg_dep_write(&frame, layer1, NULL, 3, b, 0, 0, 0);
    (void)fg_dep_write(&frame, depth, NULL, 4, b, 0, 0, 0);
    CHECK(root.version != other_mip.version, "subresource writes have separate versions");
    (void)fg_dep_clear(&frame, mip0, 5);
    CHECK(fg_dep_read(&frame, mip1).origins == b && fg_dep_read(&frame, layer1).origins == b,
          "clearing one subresource does not destroy independent mip/layer evidence");
    CHECK(!fg_dep_read(&frame, mip0).origins, "compatible alias sees the overwritten version rather than stale root");
    fg_dep_value unrelated = seed(8, a, 6);
    fg_dep_invalidate_backing(&frame, 7, 7);
    CHECK(fg_dep_read(&frame, mip0).tainted && fg_dep_read(&frame, mip1).tainted &&
          fg_dep_read(&frame, layer1).tainted && fg_dep_read(&frame, depth).tainted,
          "unknown backing reuse invalidates every recorded subresource and aspect");
    CHECK(fg_dep_read(&frame, key(8)).version == unrelated.version && !fg_dep_read(&frame, key(8)).tainted,
          "backing invalidation cannot spill into a different canonical descriptor");
    CHECK(select_value(fg_dep_copy(&frame, key(9), mip1, 8, 0), 0) == FG_DEP_TAINTED,
          "sampling invalidated alias storage refuses");
}

static void same_pass_feedback_and_tags(void) {
    fg_dep_reset(&frame);
    fg_dep_mask a = fg_dep_candidate(&frame, 1), b = fg_dep_candidate(&frame, 1);
    fg_dep_value first = seed(1, a, 10), second = seed(2, b, 10);
    fg_dep_sources input_a, input_b;
    fg_dep_sources_init(&input_a); fg_dep_sources_init(&input_b);
    fg_dep_sources_add(&frame, &input_a, first);
    fg_dep_sources_add(&frame, &input_b, second);
    fg_dep_value out_a = fg_dep_write(&frame, key(1), &input_b, 10, 0, 0, 0, 1);
    fg_dep_value out_b = fg_dep_write(&frame, key(2), &input_a, 10, 0, 0, 0, 2);
    CHECK(out_a.origins == b && out_b.origins == a, "same-pass feedback consumes pre-write snapshots of all outputs");
    CHECK(first.origins == a && second.origins == b, "later draws in same pass do not mutate consumed versions");
    CHECK(out_a.writer == out_b.writer && out_a.version != out_b.version, "same pass order still distinguishes writer versions");
    CHECK(input_a.source[0].version == first.version && input_b.source[0].version == second.version,
          "feedback diagnostics name old source versions, never newly written outputs");
    fg_dep_value canvas = compose(3, out_a, out_b, 11, 1, a);
    CHECK(canvas.origins == (a | b) && canvas.eligible == a && canvas.tags == 3,
          "camera filters eligibility but retains raw roots and canvas lineage tags");
    fg_dep_value present = fg_dep_copy(&frame, key(4), key(3), 12, 0);
    CHECK(present.tags == 3 && present.origins == (a | b), "transitive copies preserve boundary tags and root evidence");
    CHECK(select_value(present, 0) == FG_DEP_SELECTED, "feedback chain with authoritative camera selects main");
    fg_dep_value self_copy = fg_dep_copy(&frame, key(4), key(4), 13, 1);
    CHECK(self_copy.tags == present.tags && self_copy.origins == present.origins && self_copy.version > present.version,
          "same-backing self copy snapshots once and publishes a new version");
    CHECK(!fg_dep_clear(&frame, key(4), 14).tags, "full clear cannot leave a stale presented-canvas tag");
    CHECK(!fg_dep_invalidate(&frame, key(3), 15).tags, "unknown overwrite cannot retain a false canvas proof");
}

static void unknown_cause_diagnostics(void) {
    fg_dep_reset(&frame);
    fg_dep_mask main=fg_dep_candidate(&frame,1);
    fg_dep_value world=seed(1,main,1);
    fg_dep_invalidate(&frame,key(2),FG_PASS_WRITER_OPERATION|1);
    fg_dep_resource *unknown=fg_dep_find(&frame,key(2),0);
    unknown->value.tags=UINT64_C(1)<<63; /* diagnostic identity only, not eligibility */
    fg_dep_value consumed=unknown->value;
    fg_dep_value canvas=compose(3,world,consumed,2,0,0);
    CHECK(canvas.tags==(UINT64_C(1)<<63),"unknown cause bit63 survives scene composition");
    CHECK(canvas.origins==main && canvas.unresolved==main,"diagnostic causes do not change scene masks");
    CHECK(select_value(canvas,0)==FG_DEP_TAINTED,"diagnostic identity never authorizes unknown content");
    fg_dep_clear(&frame,key(2),3);
    CHECK(!fg_dep_read(&frame,key(2)).tags,"later replacement drops its own obsolete cause");
    CHECK(canvas.tags==(UINT64_C(1)<<63),"a sampled cause survives later overwrite of its source");
    fg_dep_value copied=fg_dep_copy(&frame,key(4),key(3),4,0);
    CHECK(copied.tags==canvas.tags && copied.tainted==canvas.tainted,"copy forwards exact sampled unknown causes");
    fg_dep_invalidate(&frame,key(2),FG_PASS_WRITER_OPERATION|2);
    fg_dep_find(&frame,key(2),0)->value.tags=1;
    fg_dep_value mixed=compose(5,copied,fg_dep_read(&frame,key(2)),5,1,main);
    CHECK(mixed.tags==((UINT64_C(1)<<63)|1),"multiple unknown resources retain separate diagnostic bits");
    CHECK(select_value(mixed,0)==FG_DEP_TAINTED,"camera filtering cannot hide a diagnostic unknown");
    fg_dep_reset(&frame);
    CHECK(!fg_dep_read(&frame,key(5)).known,"cause identities never survive a real-frame epoch reset");
}

static void partial_clear_writers(void) {
    fg_dep_reset(&frame);
    fg_dep_mask main=fg_dep_candidate(&frame,1);
    fg_dep_value world=seed(1,main,1);
    fg_dep_key layer0={2,1,0,2},layer1={2,1,1,2},small={3,0,0,1};
    fg_dep_value d0=fg_dep_partial_clear(&frame,layer0,FG_PASS_WRITER_OPERATION|1);
    fg_dep_value d1=fg_dep_partial_clear(&frame,layer1,FG_PASS_WRITER_OPERATION|2);
    fg_dep_value rgba=fg_dep_partial_clear(&frame,small,FG_PASS_WRITER_OPERATION|3);
    CHECK(d0.known && d1.known && rgba.known,"first-touch regional clears publish current-frame versions");
    CHECK(!d0.tainted && !d1.tainted && !rgba.tainted,"absent prior-frame provenance is not an unknown writer");
    CHECK(!d0.origins && !d1.origins && !rgba.origins,"constant clear plus unproduced remainder invents no scene edge");
    CHECK(d0.version<d1.version && d1.version<rgba.version,"mip/layer clears are distinct ordered writers");
    CHECK(!fg_dep_read(&frame,(fg_dep_key){2,0,0,2}).known,"mip1 clear cannot manufacture mip0 provenance");
    fg_dep_value canvas=compose(4,world,d0,2,0,0);
    canvas=compose(4,canvas,d1,3,0,0);
    canvas=compose(4,canvas,rgba,4,0,0);
    CHECK(select_value(canvas,0)==FG_DEP_SELECTED,"scene plus rootless regional-clear auxiliaries selects the main scene");
    fg_dep_value retained=fg_dep_partial_clear(&frame,key(1),FG_PASS_WRITER_OPERATION|4);
    CHECK(retained.origins==main && retained.version>world.version,"partial clear retains previous current-frame scene ancestry");
    CHECK(select_value(retained,0)==FG_DEP_SELECTED,"deterministic clear does not taint the preserved scene remainder");
    fg_dep_invalidate(&frame,small,FG_PASS_WRITER_OPERATION|5);
    rgba=fg_dep_partial_clear(&frame,small,FG_PASS_WRITER_OPERATION|6);
    CHECK(rgba.tainted,"partial clear cannot certify pixels left by an explicit unknown writer");
    canvas=compose(4,world,rgba,5,0,0);
    CHECK(select_value(canvas,0)==FG_DEP_TAINTED,"a genuinely untracked writer in the remainder still refuses");
    fg_dep_clear(&frame,small,FG_PASS_WRITER_OPERATION|7);
    CHECK(!fg_dep_partial_clear(&frame,small,FG_PASS_WRITER_OPERATION|8).tainted,"a subsequent full clear really replaces the unknown content");
}


static void capacities_fail_closed(void) {
    fg_dep_reset(&frame);
    fg_dep_mask last = 0;
    for (unsigned i = 0; i < FG_DEP_MAX_CANDIDATES; i++) last = fg_dep_candidate(&frame, 1);
    fg_dep_value root = seed(1, last, 1);
    CHECK(select_value(root, FG_DEP_MAX_CANDIDATES - 1) == FG_DEP_SELECTED, "highest representable candidate bit selects without truncation");
    CHECK(!fg_dep_candidate(&frame, 1) && frame.failed, "candidate capacity overflow poisons selection instead of choosing subset");
    CHECK(select_value(root, 0) == FG_DEP_FAILED, "earlier valid snapshot cannot bypass candidate overflow");

    fg_dep_reset(&frame);
    fg_dep_mask main = fg_dep_candidate(&frame, 1);
    root = seed(1, main, 1);
    for (unsigned i = 2; i <= FG_DEP_MAX_RESOURCES; i++)
        (void)fg_dep_clear(&frame, key(i), i);
    CHECK(frame.count == FG_DEP_MAX_RESOURCES && !frame.failed, "resource table accepts its exact fixed capacity");
    CHECK(!fg_dep_read(&frame, key(FG_DEP_MAX_RESOURCES + 1)).known && !frame.failed,
          "absent material/history read does not consume a resource slot even when full");
    (void)seed(1, main, FG_DEP_MAX_RESOURCES + 1);
    CHECK(frame.count == FG_DEP_MAX_RESOURCES && !frame.failed, "rewriting an existing resource needs no extra table slot");
    (void)fg_dep_clear(&frame, key(FG_DEP_MAX_RESOURCES + 1), FG_DEP_MAX_RESOURCES + 2);
    CHECK(frame.failed, "untracked extra writer fails closed");
    CHECK(select_value(root, 0) == FG_DEP_FAILED, "resource capacity failure rejects even an earlier clean boundary");

    fg_dep_reset(&frame);
    main = fg_dep_candidate(&frame, 1);
    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    for (unsigned i = 0; i < FG_DEP_MAX_SOURCES; i++) {
        fg_dep_value value = seed(i + 1, main, i + 1);
        fg_dep_sources_add(&frame, &sources, value);
        fg_dep_sources_add(&frame, &sources, value);
    }
    CHECK(sources.count == FG_DEP_MAX_SOURCES && !frame.failed, "unique sources fit exact capacity; duplicate reads cost no extra slot");
    fg_dep_sources_add(&frame, &sources, seed(FG_DEP_MAX_SOURCES + 1, main, FG_DEP_MAX_SOURCES + 1));
    CHECK(frame.failed && sources.value.tainted, "source capacity overflow does not silently drop sampled dependencies");
    root = fg_dep_write(&frame, key(FG_DEP_MAX_SOURCES + 2), &sources, 100, 0, 0, 0, 0);
    CHECK(select_value(root, 0) == FG_DEP_FAILED, "source overflow cannot become a selected subset");

    fg_dep_reset(&frame);
    main = fg_dep_candidate(&frame, 1);
    root = seed(1, main, 1);
    for (uint32_t i = 2; i <= 40000; i++)
        root = compose(1, root, (fg_dep_value){0}, i, 0, 0);
    CHECK(frame.count == 1 && !frame.failed && root.version == 40000, "many draws in one pass do not require an ever-growing node pool");
    CHECK(select_value(root, 0) == FG_DEP_SELECTED, "bounded per-resource versioning survives a long real frame");

    fg_dep_reset(&frame);
    main = fg_dep_candidate(&frame, 1);
    root = seed(1, main, 1);
    (void)fg_dep_write(&frame, key(2), NULL, 2, main << 1, 0, 0, 0);
    CHECK(select_value(root, 0) == FG_DEP_FAILED, "unknown candidate identity fails closed");
    fg_dep_reset(&frame);
    (void)fg_dep_read(&frame, key(0));
    CHECK(frame.failed, "zero backing cannot masquerade as an actual resource identity");
    fg_dep_reset(&frame);
    CHECK(!frame.failed && !frame.count && !frame.candidate_count, "new real frame clears all bounded failure and candidate state");
}

static void reopened_scene_canvas_transfer_chain(void) {
    vk_canvas_init(0,1,0);
    vk_canvas_image canvas={.image=100,.storage=1000,.size=1280*720*8,.incarnation=1,
        .width=1280,.height=720,.format=97};
    vk_canvas_image window={.image=200,.storage=2000,.size=1920*1080*4,.incarnation=2,
        .width=1920,.height=1080,.format=37};
    vk_canvas_transfer(&canvas,&window,1); vk_canvas_frame_end(); /* exact identity discovery */
    fg_dep_reset(&frame);
    fg_dep_mask root=fg_dep_candidate(&frame,1);
    fg_dep_value gbuffer={0};
    for(unsigned chunk=0;chunk<10;chunk++)
        gbuffer=compose(1,gbuffer,seed(2,root,chunk+1),chunk+1,0,0);
    fg_dep_value lighting=compose(3,gbuffer,(fg_dep_value){0},11,0,0);
    /* The same fullscreen VS can be UI-capable AND compose the pre-UI scene.
       A zero optional descriptor is the real emitter's known dummy. */
    fg_dep_sources inputs; fg_dep_sources_init(&inputs);
    if (!fg_pass_ui_context(0,2,1)) fg_dep_sources_add(&frame,&inputs,lighting);
    fg_dep_sources_add(&frame,&inputs,(fg_dep_value){0});
    fg_dep_value pre_ui=fg_dep_write(&frame,key(canvas.image),&inputs,12,0,0,0,0);
    CHECK(!inputs.value.tainted && inputs.value.origins==root,
          "UI-capable pre-boundary composition retains scene and unbound dummy adds no unknown edge");
    CHECK(select_value(pre_ui,0)==FG_DEP_SELECTED,"four/six chunk roots share one accepted depth/camera origin");
    CHECK(vk_canvas_first_ui(&canvas,"7f2113e2"),"shared exact canvas exposes pre-UI boundary");
    vk_canvas_ui_commit(&canvas);
    vk_canvas_transfer(&canvas,&window,0);
    CHECK(!vk_canvas_present(&window),"uncommitted transfer nomination cannot prove present");
    vk_canvas_transfer(&canvas,&window,1);
    vk_canvas_image unrelated=window; unrelated.incarnation++;
    CHECK(!vk_canvas_present(&unrelated),"recycled window handle is not the presented generation");
    CHECK(vk_canvas_present(&window)&&vk_canvas_current()->ui_committed,
          "checked scene -> pre-UI canvas -> committed transfer -> exact present chain completes");
    vk_canvas_frame_end();
    CHECK(!vk_canvas_present(&window)&&!vk_canvas_current()->first_ui,"loading/menu frame cannot reuse preceding chain");
    fg_dep_reset(&frame);
    CHECK(select_value(pre_ui,0)==FG_DEP_STALE,"previous scene version is not current canvas provenance");
    fg_dep_value menu=fg_dep_clear(&frame,key(canvas.image),1);
    CHECK(select_value(menu,0)==FG_DEP_NO_ORIGIN,"menu/loading canvas without G-buffer refuses");
    unrelated=canvas; unrelated.width=640;
    CHECK(!vk_canvas_first_ui(&unrelated,"7f2113e2"),"changed cutscene canvas structure requires fresh identity");
    fg_dep_reset(&frame);
    root=fg_dep_candidate(&frame,1); gbuffer=seed(1,root,1);
    fg_dep_value storage_result=fg_dep_invalidate(&frame,key(99),2);
    pre_ui=compose(canvas.image,gbuffer,(fg_dep_value){0},3,0,0);
    CHECK(select_value(pre_ui,0)==FG_DEP_SELECTED,
          "unrelated storage write is not an input and cannot globally poison scene/canvas");
    pre_ui=compose(canvas.image,gbuffer,storage_result,4,0,0);
    CHECK(select_value(pre_ui,0)==FG_DEP_TAINTED,
          "actually sampled unknown storage result still refuses instead of suppressing taint");
}

static void sparse_large_sample_views(void) {
    fg_dep_reset(&frame);
    fg_dep_mask root=fg_dep_candidate(&frame,1);
    fg_dep_key inside={700,2,900,1}, outside={700,2,1300,1};
    (void)fg_dep_write(&frame,inside,NULL,1,root,0,0,0);
    (void)fg_dep_invalidate(&frame,outside,2);
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    unsigned work=fg_dep_sample_view(&frame,&sources,700,0,4,0,1024,1);
    CHECK(work==frame.count && sources.count==1 && sources.value.origins==root && !sources.value.tainted,
          "1024-layer view visits only produced versions inside its exact range");
    fg_dep_sources_init(&sources);
    (void)fg_dep_sample_view(&frame,&sources,999,0,64,0,100000,1);
    CHECK(!sources.count && !sources.value.tainted && !frame.failed,
          "large unproduced static array is neutral, not an artificial64-source truncation");
    fg_dep_key unknown={700,2,901,1}; (void)fg_dep_invalidate(&frame,unknown,3);
    fg_dep_sources_init(&sources);
    (void)fg_dep_sample_view(&frame,&sources,700,0,4,0,1024,1);
    CHECK(sources.value.tainted && sources.value.origins==root,
          "actual unknown produced layer inside a large view still poisons proof");
    fg_dep_reset(&frame); fg_dep_sources_init(&sources);
    CHECK(!fg_dep_sample_view(&frame,&sources,700,0,4,0,1024,1) && !sources.count,
          "epoch reset hides all old occupied range slots");
    root=fg_dep_candidate(&frame,1);
    for (unsigned layer=0;layer<=FG_DEP_MAX_SOURCES;layer++)
        (void)fg_dep_write(&frame,(fg_dep_key){700,0,layer,1},NULL,layer+1,root,0,0,0);
    fg_dep_sources_init(&sources);
    (void)fg_dep_sample_view(&frame,&sources,700,0,1,0,100000,1);
    CHECK(frame.failed && sources.value.tainted && sources.count==FG_DEP_MAX_SOURCES,
          "too many actually produced sampled versions still fail at the fixed source capacity");
    fg_dep_reset(&frame); fg_dep_sources_init(&sources);
    (void)fg_dep_sample_view(&frame,&sources,0,0,4,0,1024,1);
    CHECK(frame.failed && sources.value.tainted,"zero image identity in a large view still fails closed");
}

static void bounded_sample_work(void) {
    static fg_pass_frame budget;
    fg_dep_reset(&frame); fg_pass_reset(&budget);
    fg_dep_mask root=fg_dep_candidate(&frame,1);
    (void)fg_dep_write(&frame,(fg_dep_key){700,2,5,1},NULL,1,root,0,0,0);
    (void)fg_dep_write(&frame,(fg_dep_key){700,3,6,1},NULL,2,root,0,0,0);
    for (unsigned i=0;i<198;i++)
        (void)fg_dep_write(&frame,(fg_dep_key){1000+i,0,0,1},NULL,3+i,0,0,0,0);
    CHECK(frame.count==200 && fg_dep_sample_work(&frame,4,2)==8,
          "a small mip/layer view reserves eight exact lookups, not all200 produced resources");
    CHECK(fg_dep_sample_work(&frame,UINT32_MAX,UINT32_MAX)==200,
          "oversized view arithmetic cannot wrap to an undercharged work reservation");
    for (unsigned context=0;context<300;context++) {
        fg_dep_sources sources; fg_dep_sources_init(&sources);
        unsigned work=fg_dep_sample_work(&frame,4,2);
        CHECK(fg_pass_take_bindings(&budget,4) && fg_pass_take_bindings(&budget,work),
              "reflected descriptor traversal and real view work fit the unchanged frame budget");
        CHECK(fg_dep_sample_view(&frame,&sources,700,1,4,5,2,1)==work &&
              sources.count==2 && sources.value.origins==root && !sources.value.tainted,
              "pre-reserved work equals complete view traversal without losing ancestry");
    }
    CHECK(budget.bindings_used==3600 && !budget.failed &&
          300u*(4u+frame.count)>FG_PASS_MAX_BINDINGS,
          "small-view lookup removes actual redundant scans rather than raising or bypassing capacity");
    CHECK(fg_pass_take_bindings(&budget,FG_PASS_MAX_BINDINGS-budget.bindings_used) &&
          !fg_pass_take_bindings(&budget,1) && (budget.failed&FG_PASS_FAILED_BINDINGS),
          "optimized view traversal retains exact fail-closed budget exhaustion");
    (void)fg_dep_invalidate(&frame,(fg_dep_key){700,4,5,1},300);
    for (unsigned mip=0;mip<6;mip++) for (unsigned layers=1;layers<=64;layers*=2) {
        fg_dep_sources actual,reference;
        fg_dep_sources_init(&actual); fg_dep_sources_init(&reference);
        unsigned work=fg_dep_sample_work(&frame,5,layers);
        CHECK(fg_dep_sample_view(&frame,&actual,700,mip,5,5,layers,1)==work,
              "both exact and occupied traversals match their reserved work");
        for (unsigned i=0;i<frame.count;i++) {
            const fg_dep_resource *r=&frame.resource[frame.used[i]];
            if (r->key.backing==700 && r->key.aspect==1 && r->key.mip>=mip &&
                r->key.mip-mip<5 && r->key.layer>=5 && r->key.layer-5<layers)
                fg_dep_sources_add(&frame,&reference,r->value);
        }
        CHECK(actual.count==reference.count && actual.value.origins==reference.value.origins &&
              actual.value.eligible==reference.value.eligible &&
              actual.value.unresolved==reference.value.unresolved &&
              actual.value.tainted==reference.value.tainted && actual.value.tags==reference.value.tags,
              "small and large view strategies preserve the original full-scan dependency union");
    }
    fg_dep_sources sources; fg_dep_sources_init(&sources);
    (void)fg_dep_sample_view(&frame,&sources,700,UINT32_MAX-1,4,5,1,1);
    CHECK(!sources.count,"overflowing mip range never wraps onto low-mip resources");
}

int main(void) {
    independent_gbuffers();
    cameras_and_mixed_roots();
    frame_and_history_separation();
    overwrites_clears_and_unknowns();
    aliases_and_subresources();
    same_pass_feedback_and_tags();
    unknown_cause_diagnostics();
    partial_clear_writers();
    capacities_fail_closed();
    reopened_scene_canvas_transfer_chain();
    sparse_large_sample_views();
    bounded_sample_work();
    printf("framegen dependency goldens: %u checks, %u failed\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
