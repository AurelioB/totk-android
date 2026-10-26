/* Pure host coverage; no Android/Vulkan/game data or device.
   cc -std=c11 -O2 -Wall -Wextra -Werror tests/test_framegen_passes.c
      -o /tmp/test-framegen-passes && /tmp/test-framegen-passes */
#include "../games/totk/android/gfx/framegen_passes.h"
#include <stdio.h>
#include <stdlib.h>

static fg_pass_frame frame;
static fg_dep_frame deps;
static unsigned checks, failures;
#define CHECK(c, message) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); } } while (0)

static fg_pass_attachment attachment(uint64_t backing, unsigned width, unsigned height, unsigned format) {
    return (fg_pass_attachment){.key = {.backing = backing, .aspect = 1},
        .width = width, .height = height, .samples = 1, .nvn_format = format + 700, .vk_format = format};
}
static fg_pass_key target(uint64_t backing, unsigned width, unsigned height, unsigned colors, unsigned depth_format) {
    fg_pass_key key = {.width = width, .height = height};
    for (unsigned i = 0; i < colors; i++) key.color[i] = attachment(backing + i, width, height, 97);
    if (depth_format) {
        key.depth = attachment(backing + 8, width, height, depth_format);
        key.depth.key.aspect = 6;
    }
    return key;
}
static int pass(fg_pass_key key, uint64_t draws, uint64_t depth_writes) {
    int group = fg_pass_begin(&frame, &key);
    fg_pass_end(&frame, group, draws, depth_writes);
    return group;
}

static void thor_census(void) {
    fg_pass_reset(&frame);
    unsigned tiny_draws[] = {37, 2, 2, 1};
    int tiny[4];
    for (unsigned i = 0; i < 4; i++) tiny[i] = pass(target(100 + 20 * i, 128, 128, 3, 126), tiny_draws[i], tiny_draws[i]);
    /* D24S8, just TWO MRTs, no C0, and invalid/unavailable camera: none of
       those can conceal the actual 4200-draw scene from the census. */
    fg_pass_key scene = target(1000, 1280, 720, 0, 129);
    scene.color[2] = attachment(1002, 1280, 720, 37);
    scene.color[5] = attachment(1005, 1280, 720, 44);
    int main = pass(scene, 4200, 4190);
    unsigned ranked[FG_PASS_TOP] = {0}, selected = UINT_MAX;
    CHECK(fg_pass_top5(&frame, ranked) == 5, "top five covers every group, not old candidates only");
    CHECK(ranked[0] == (unsigned)main, "4200-draw D24S8 two-MRT scene outranks 37/2/2/1");
    for (unsigned i = 0; i < 4; i++) CHECK(ranked[i + 1] == (unsigned)tiny[i], "small old candidates remain in stable all-group census");
    CHECK(frame.draws == 4242 && frame.physical_passes == 5, "actual committed deltas are all counted");
    CHECK(frame.group[main].depth_writes == 4190, "depth-write draw count is not collapsed to boolean");
    CHECK(fg_pass_main_candidate(&frame, 1280, 720, &selected) == FG_PASS_NOMINATED && selected == (unsigned)main,
          "scene nomination has no Metal format, C0, three-MRT or camera gate");
    CHECK(fg_pass_main_candidate(&frame, 1920, 1080, &selected) == FG_PASS_NO_CANDIDATE,
          "scene extent is a separate constraint, not nearest-size guessing");
    fg_pass_reset(&frame);
    fg_pass_key small = target(2000, 32, 16, 2, 124);
    main = pass(small, 1700, 1700);
    CHECK(fg_pass_main_candidate(&frame, 32, 16, &selected) == FG_PASS_NOMINATED && selected == (unsigned)main,
          "D16 and small render extents are not filtered from scene nomination");
}

static void reopens_subresources_and_resizes(void) {
    fg_pass_reset(&frame);
    fg_pass_key key = target(3000, 1280, 720, 2, 129);
    int main = pass(key, 1000, 995);
    int other = pass(target(4000, 64, 64, 1, 0), 2, 0);
    CHECK(pass(key, 1200, 1200) == main, "same exact attachment set reopens one logical group");
    CHECK(pass(key, 2000, 1999) == main, "third chunk shares the scene identity");
    CHECK(frame.group[main].draws == 4200 && frame.group[main].depth_writes == 4194, "chunk deltas aggregate once");
    CHECK(frame.group[main].physical_passes == 3 && frame.group[main].first_order == 1 && frame.group[main].last_order == 4,
          "logical group retains physical first/last order and reopen count");
    CHECK(frame.physical[1].group == other && frame.physical[2].group == main && frame.physical[2].draws == 1200,
          "physical summaries preserve interleaving instead of inventing adjacency");
    fg_pass_key changed = key;
    changed.depth.key.mip++;
    CHECK(pass(changed, 1, 1) != main, "different depth mip is not a reopen");
    changed = key; changed.color[1].key.layer++;
    CHECK(pass(changed, 1, 1) != main, "different color layer is not a reopen");
    changed = key; changed.depth.key.aspect = 2;
    CHECK(pass(changed, 1, 1) != main, "different depth aspect is not a reopen");
    changed = key; changed.color[0].vk_format++;
    CHECK(pass(changed, 1, 1) != main, "view format is part of exact attachment compatibility");
    changed = key; changed.color[0].nvn_format++;
    CHECK(pass(changed, 1, 1) == main, "NVN diagnostic enum alias does not split the same actual Vulkan image/view");
    CHECK(frame.group[main].key.color[0].nvn_format == key.color[0].nvn_format,
          "aggregated group retains first-observed NVN format for diagnostics");
    changed = key; changed.color[0].samples = 4;
    CHECK(pass(changed, 1, 1) != main, "sample count is not silently merged");
    changed = key; changed.color[0].width /= 2;
    CHECK(pass(changed, 1, 1) != main, "attachment dimensions distinguish compatible extents");
    changed = key; changed.width = 1920; changed.height = 1080;
    CHECK(pass(changed, 5, 5) != main, "render-area resize does not merge old scene group");
    unsigned selected = UINT_MAX;
    fg_pass_reset(&frame);
    int resized = pass(target(3000, 1920, 1080, 2, 129), 2000, 2000);
    CHECK(fg_pass_main_candidate(&frame, 1280, 720, &selected) == FG_PASS_NO_CANDIDATE, "old extent does not survive frame reset");
    CHECK(fg_pass_main_candidate(&frame, 1920, 1080, &selected) == FG_PASS_NOMINATED && selected == (unsigned)resized,
          "new scene extent is nominated independently in next frame");
}

static void ranking_ties_and_ambiguity(void) {
    fg_pass_reset(&frame);
    int a = pass(target(5000, 1280, 720, 2, 129), 2000, 2000);
    int b = pass(target(6000, 1280, 720, 2, 124), 2000, 2000);
    (void)pass(target(7000, 1280, 720, 1, 129), 5000, 5000);
    (void)pass(target(8000, 1280, 720, 2, 129), 6000, 0);
    unsigned ranked[FG_PASS_TOP], candidate = 12345;
    CHECK(fg_pass_top5(&frame, ranked) == 4 && ranked[2] == (unsigned)a && ranked[3] == (unsigned)b,
          "all-group ranking includes ineligible groups and keeps ties deterministic");
    CHECK(fg_pass_main_candidate(&frame, 1280, 720, &candidate) == FG_PASS_AMBIGUOUS && candidate == 12345,
          "equal main candidates refuse instead of choosing physical order");
    (void)pass(target(9000, 1280, 720, 2, 129), 2001, 2001);
    CHECK(fg_pass_main_candidate(&frame, 1280, 720, &candidate) == FG_PASS_NOMINATED && candidate == 4,
          "a unique greater eligible count resolves the census tie only");
    fg_dep_reset(&deps);
    fg_dep_mask main = fg_dep_candidate(&deps, 1), other = fg_dep_candidate(&deps, 0);
    fg_dep_value world = fg_dep_write(&deps, (fg_dep_key){.backing = 1, .aspect = 1}, NULL, 1, main, 0, 0, 0);
    fg_dep_value extra = fg_dep_write(&deps, (fg_dep_key){.backing = 2, .aspect = 1}, NULL, 2, other, 0, 0, 0);
    fg_dep_sources sources;
    fg_dep_sources_init(&sources);
    fg_dep_sources_add(&deps, &sources, world);
    fg_dep_sources_add(&deps, &sources, extra);
    fg_dep_value canvas = fg_dep_write(&deps, (fg_dep_key){.backing = 3, .aspect = 1}, &sources, 3, 0, 0, 0, 0);
    CHECK(fg_dep_select(&deps, canvas, NULL) == FG_DEP_AMBIGUOUS,
          "invalid extra unresolved origin still refuses despite unique census nomination");
    sources.value.tainted = 1;
    canvas = fg_dep_write(&deps, (fg_dep_key){.backing = 4, .aspect = 1}, &sources, 4, 0, 0, 0, 0);
    CHECK(fg_dep_select(&deps, canvas, NULL) == FG_DEP_TAINTED, "incomplete tracing cannot be replaced by draw-count guessing");
}

typedef struct { unsigned calls, first, programs, samples; uint64_t sink; } callback_state;
static void metadata(void *opaque, unsigned event) {
    callback_state *state = opaque;
    state->calls++;
    state->first += !!(event & FG_PASS_EVENT_FIRST);
    state->programs += !!(event & FG_PASS_EVENT_PROGRAM);
    if (event & FG_PASS_EVENT_SAMPLE) {
        state->samples++;
        CHECK(fg_pass_take_bindings(&frame, 4), "sample callback has bounded binding allowance");
    }
    state->sink = state->sink * 33 + event;
}
static void guarded_metadata_events(void) {
    fg_pass_reset(&frame);
    callback_state state = {0};
    /* NULL state pointers prove the disabled branch does not inspect them. */
    for (unsigned i = 0; i < 5600; i++)
        CHECK(!fg_pass_route_draw(0, NULL, NULL, i, metadata, &state), "cached off branch has no callback or tracking dereference");
    CHECK(!state.calls && !frame.events_used, "off path has no slow events");
    fg_pass_key key = target(10000, 1280, 720, 2, 129);
    int group = fg_pass_begin(&frame, &key);
    fg_pass_guard guard;
    fg_pass_guard_reset(&guard, 3);
    for (unsigned i = 0; i < 4200; i++)
        fg_pass_route_draw(1, &frame, &guard, 0, metadata, &state);
    CHECK(state.calls == 1 && state.first == 1 && state.programs == 1 && state.samples == 1,
          "4200 ordinary draws with even a null VS run metadata only on first draw");
    CHECK(frame.events_used == 2 && frame.bindings_used == 4 && frame.draw_events == 1,
          "pass event and first draw alone consume budgets; ordinary draws write no tracking counters");
    for (uintptr_t vs = 1; vs < 10; vs++) {
        fg_pass_route_draw(1, &frame, &guard, vs, metadata, &state);
        fg_pass_route_draw(1, &frame, &guard, vs, metadata, &state);
    }
    CHECK(state.calls == 10 && state.first == 1 && state.samples == 3, "program checks run once per transition; reflected contexts are capped");
    CHECK(!frame.group[group].contexts_complete && frame.incomplete_contexts == 7,
          "omitted transition contexts cannot be advertised as complete input proof");
    fg_pass_end(&frame, group, 4218, 4200);
    group = fg_pass_begin(&frame, &key);
    fg_pass_guard_reset(&guard, 0);
    fg_pass_route_draw(1, &frame, &guard, 9, metadata, &state);
    CHECK(state.first == 2 && state.samples == 3, "a reopened physical pass snapshots its first draw without unbounded samples");
    fg_pass_end(&frame, group, 1, 1);
}

static void capacities_fail_closed(void) {
    fg_pass_reset(&frame);
    for (unsigned i = 0; i < FG_PASS_MAX_GROUPS; i++) (void)pass(target(20000 + i * 16, 1280, 720, 2, 129), i + 1, i + 1);
    CHECK(!(frame.failed & FG_PASS_FAILED_GROUPS), "exact group capacity remains usable");
    CHECK(pass(target(1000000, 1280, 720, 2, 129), 10000, 10000) == -1, "extra identity does not alias an existing group");
    unsigned selected = 99, ranked[FG_PASS_TOP];
    CHECK((frame.failed & FG_PASS_FAILED_GROUPS) && fg_pass_main_candidate(&frame, 1280, 720, &selected) == FG_PASS_INCOMPLETE && selected == 99,
          "capacity failure refuses generation, not a best retained fallback");
    CHECK(fg_pass_top5(&frame, ranked) == FG_PASS_TOP && frame.group[ranked[0]].draws == FG_PASS_MAX_GROUPS,
          "bounded top-five diagnostics survive failure");
    CHECK(frame.draws == (uint64_t)FG_PASS_MAX_GROUPS * (FG_PASS_MAX_GROUPS + 1) / 2 + 10000,
          "unrepresented overflow pass still contributes actual frame draw count");
    fg_pass_reset(&frame);
    fg_pass_key key = target(1100000, 1280, 720, 2, 129);
    for (unsigned i = 0; i <= FG_PASS_MAX_PHYSICAL; i++) (void)pass(key, 2, 1);
    CHECK(frame.count == 1 && frame.group[0].physical_passes == FG_PASS_MAX_PHYSICAL + 1,
          "physical capacity exhaustion does not duplicate logical groups or lose reopen counts");
    CHECK((frame.failed & FG_PASS_FAILED_PHYSICAL) && frame.group[0].draws == 2u * (FG_PASS_MAX_PHYSICAL + 1),
          "physical summary overflow is fail-closed and still counts committed draws");
    fg_pass_reset(&frame);
    callback_state state = {0};
    fg_pass_guard guard;
    int group = fg_pass_begin(&frame, &key);
    fg_pass_guard_reset(&guard, 0);
    for (uintptr_t i = 0; i < FG_PASS_MAX_EVENTS + 100u; i++)
        fg_pass_route_draw(1, &frame, &guard, i, metadata, &state);
    CHECK(state.calls == FG_PASS_MAX_EVENTS - 1 && frame.events_used == FG_PASS_MAX_EVENTS,
          "event storm cannot exceed configured frame metadata budget including begin");
    CHECK(frame.failed & FG_PASS_FAILED_EVENTS, "event exhaustion is explicit proof failure");
    fg_pass_end(&frame, group, FG_PASS_MAX_EVENTS + 100u, 0);
    fg_pass_reset(&frame);
    CHECK(fg_pass_take_bindings(&frame, FG_PASS_MAX_BINDINGS), "binding budget accepts exact capacity");
    CHECK(!fg_pass_take_bindings(&frame, 1) && (frame.failed & FG_PASS_FAILED_BINDINGS), "binding overflow fails before doing binding work");
    CHECK(frame.bindings_used == FG_PASS_MAX_BINDINGS, "failed budget reservations cannot wrap counters");
    fg_pass_reset(&frame);
    (void)pass(key, UINT64_MAX, UINT64_MAX);
    (void)pass(key, 1, 1);
    CHECK((frame.failed & FG_PASS_FAILED_OVERFLOW) && frame.draws == UINT64_MAX && frame.group[0].draws == UINT64_MAX,
          "draw counters saturate and fail closed instead of reordering ranking on overflow");
}

static void sampled_links_and_lifetimes(void) {
    fg_pass_reset(&frame);
    fg_dep_reset(&deps);
    fg_pass_key producer = target(1200000, 1280, 720, 2, 129);
    int group = pass(producer, 4200, 4200);
    fg_dep_value source = fg_dep_write(&deps, producer.color[0].key, NULL, 1, 0, 0, 0, 0);
    fg_pass_key consumer = target(1300000, 1280, 720, 1, 0);
    int compose = fg_pass_begin(&frame, &consumer);
    CHECK(fg_pass_link(&frame, deps.epoch, source), "actual earlier-pass current-frame sampled source records a link");
    CHECK(fg_pass_link(&frame, deps.epoch, source) && frame.link_count == 1, "repeated input version in one pass is deduplicated");
    CHECK(frame.link[0].source.writer == 1 && frame.link[0].consumer_order == 2 && frame.link[0].consumer_group == compose,
          "diagnostic link stores real producer version and physical consumer identity");
    fg_dep_value operation = source;
    operation.version++;
    operation.writer = FG_PASS_WRITER_OPERATION | 1u;
    CHECK(fg_pass_link(&frame, deps.epoch, operation) && frame.link_count == 2,
          "copy/compute producer keeps explicit operation identity, not a fabricated raster order");
    fg_pass_end(&frame, compose, 2, 0);
    CHECK(fg_pass_begin(&frame, &producer) == group, "later producer reopening keeps logical identity");
    CHECK(fg_pass_link(&frame, deps.epoch, source) && frame.link_count == 3, "same sampled version in another physical consumer gets a separate link");
    fg_pass_end(&frame, group, 1, 1);
    fg_pass_reset(&frame);
    fg_dep_reset(&deps);
    CHECK(frame.link_count == 0 && frame.count == 0, "old groups and sampled links are not current-frame data");
    compose = fg_pass_begin(&frame, &consumer);
    CHECK(!fg_pass_link(&frame, deps.epoch, source) && (frame.failed & FG_PASS_FAILED_IDENTITY), "old-frame known snapshot cannot become a new-frame sampled link");
    fg_pass_end(&frame, compose, 1, 0);
    fg_pass_reset(&frame);
    (void)pass(producer, 1, 1);
    compose = fg_pass_begin(&frame, &consumer);
    source.epoch = deps.epoch;
    source.writer = 1;
    for (unsigned i = 0; i < FG_PASS_MAX_LINKS; i++) {
        source.version = i + 1;
        CHECK(fg_pass_link(&frame, deps.epoch, source), "sample-link table accepts bounded complete prefix");
    }
    source.version++;
    CHECK(!fg_pass_link(&frame, deps.epoch, source) && (frame.failed & FG_PASS_FAILED_LINKS) && frame.link_count == FG_PASS_MAX_LINKS,
          "sample-link overflow remains bounded and explicitly incomplete");
    fg_pass_end(&frame, compose, 1, 0);
    fg_pass_reset(&frame);
    compose = fg_pass_begin(&frame, &consumer);
    source.writer = 1;
    CHECK(!fg_pass_link(&frame, deps.epoch, source), "same/future physical writer is not a prior-pass sampled edge");
    fg_pass_end(&frame, compose, 0, 0);
}

static void operation_sampled_links(void) {
    fg_pass_reset(&frame);
    fg_dep_reset(&deps);
    fg_pass_key producer = target(1400000, 1280, 720, 2, 129);
    (void)pass(producer, 4200, 4200);
    fg_dep_value source = fg_dep_write(&deps, producer.color[0].key, NULL, 1, 0, 0, 0, 0);
    unsigned copy = FG_PASS_WRITER_OPERATION | 1u, compute = FG_PASS_WRITER_OPERATION | 2u;
    CHECK(fg_pass_operation_link(&frame, deps.epoch, copy, source),
          "copy consumer can sample the just-ended physical producer outside any open pass");
    CHECK(frame.link_count == 1 && frame.link[0].consumer_order == copy && frame.link[0].consumer_group == -1,
          "operation consumer is explicitly tagged and never mislabeled a physical pass");
    CHECK(fg_pass_operation_link(&frame, deps.epoch, copy, source) && frame.link_count == 1,
          "operation consumers share the same sampled-version deduplication");
    fg_dep_key copied_key = {.backing = 1500000, .aspect = 1};
    fg_dep_value copied = fg_dep_copy(&deps, copied_key, producer.color[0].key, copy, 0);
    CHECK(fg_pass_operation_link(&frame, deps.epoch, compute, copied) && frame.link_count == 2,
          "copy-to-compute chain retains actual intermediate source version and operation writer");
    CHECK(frame.link[1].source.version == copied.version && frame.link[1].source.writer == copy,
          "operation chain is not flattened into an invented raster edge");
    CHECK(!fg_pass_operation_link(&frame, deps.epoch, copy, copied) && (frame.failed & FG_PASS_FAILED_IDENTITY),
          "self/future operation cannot masquerade as a prior sampled producer");
    fg_pass_reset(&frame);
    CHECK(!fg_pass_operation_link(&frame, deps.epoch, FG_PASS_WRITER_OPERATION, source),
          "operation ordinal zero is not an identity");
    fg_pass_reset(&frame);
    CHECK(!fg_pass_operation_link(&frame, deps.epoch, 1, source),
          "untagged operation consumer cannot collide with physical order");
    fg_pass_reset(&frame);
    fg_dep_reset(&deps);
    CHECK(!fg_pass_operation_link(&frame, deps.epoch, compute, copied),
          "old-frame operation producer is rejected just like old-frame raster producer");
}

static void census_has_no_attachment_gate(void) {
    fg_pass_reset(&frame);
    fg_pass_key raster_only = {.width = 16, .height = 8};
    int raster = pass(raster_only, 6000, 0);
    fg_pass_key depth_only = target(1600000, 32, 32, 0, 130);
    int depth = pass(depth_only, 5000, 5000);
    int single = pass(target(1700000, 8, 8, 1, 0), 4000, 0);
    for (unsigned i = 0; i < 6; i++) (void)pass(target(1800000 + i * 16, 4, 4, 1, 0), i + 1, 0);
    unsigned ranked[FG_PASS_TOP], candidate = 999;
    CHECK(fg_pass_top5(&frame, ranked) == FG_PASS_TOP && ranked[0] == (unsigned)raster &&
          ranked[1] == (unsigned)depth && ranked[2] == (unsigned)single,
          "raster-only, depth-only and tiny single-color passes are all in all-pass census ranking");
    CHECK(frame.group[ranked[3]].draws == 6 && frame.group[ranked[4]].draws == 5,
          "top-five insertion replaces lesser groups rather than retaining first five");
    CHECK(fg_pass_main_candidate(&frame, 32, 32, &candidate) == FG_PASS_NO_CANDIDATE && candidate == 999,
          "counting every pass does not turn depth-only census leader into an MRT candidate");
}

static void unknown_attachments_remain_ungrouped(void) {
    fg_pass_reset(&frame);
    fg_pass_key scene = target(1900000, 1280, 720, 2, 129);
    int main = pass(scene, 4200, 4190);
    fg_pass_key raster_only = {.width = 1280, .height = 720};
    int raster = pass(raster_only, 3, 0);
    int unknown = fg_pass_begin_unknown(&frame);
    CHECK(unknown == -1 && frame.open && frame.current_group == -1,
          "missing bound-attachment identity opens an explicitly ungrouped physical pass");
    fg_pass_end(&frame, unknown, 5000, 4999);
    CHECK(frame.count == 2 && frame.physical_passes == 3 && frame.events_used == 3,
          "unknown pass preserves physical/event accounting without creating an empty-attachment group");
    CHECK(frame.draws == 9203 && frame.physical[2].group == -1 &&
          frame.physical[2].draws == 5000 && frame.physical[2].depth_writes == 4999,
          "unknown pass still retains its actual committed draws and depth-write summary");
    CHECK(frame.group[raster].draws == 3 && frame.group[main].draws == 4200,
          "missing identity never aliases valid attachment-less or known scene attachments");
    unsigned ranked[FG_PASS_TOP], candidate = 99;
    CHECK(fg_pass_top5(&frame, ranked) == 2 && ranked[0] == (unsigned)main,
          "known top-group diagnostics survive an unrepresentable attachment identity");
    CHECK((frame.failed & FG_PASS_FAILED_IDENTITY) &&
          fg_pass_main_candidate(&frame, 1280, 720, &candidate) == FG_PASS_INCOMPLETE && candidate == 99,
          "an unknown larger pass cannot be silently discarded to select the retained main candidate");
    CHECK(pass(scene, 2, 1) == main && frame.group[main].draws == 4202 && frame.draws == 9205,
          "known-group aggregation and total draws continue after unknown-pass failure");
}

static void stable_scene_and_complete_contexts(void) {
    fg_pass_key scene = target(1, 1280, 720, 0, 126);
    scene.color[0] = attachment(2,1280,720,37);
    scene.color[1] = attachment(3,1280,720,43);
    scene.color[3] = attachment(4,1280,720,37);
    scene.color[5] = attachment(5,1280,720,122);
    uint64_t signature = fg_pass_scene_signature(&scene);
    for (unsigned real = 0; real < 3; real++) {
        fg_pass_reset(&frame);
        for (unsigned i = 0; i < 8; i++) if (scene.color[i].key.backing) scene.color[i].key.backing += 100;
        scene.depth.key.backing += 100;
        CHECK(fg_pass_scene_signature(&scene) == signature, "three census scene shapes survive allocation/serial churn");
        int group = -1;
        for (unsigned chunk = 0; chunk < 4; chunk++) {
            int next = pass(scene, 200, 195);
            if (chunk) CHECK(next == group, "chunk threshold and reopen order do not split logical identity");
            group = next;
        }
        CHECK(frame.group[group].draws == 800 && frame.group[group].physical_passes == 4,
              "four census-like physical chunks aggregate exactly once");
    }
    fg_pass_key changed = scene; changed.color[5].samples = 4;
    CHECK(fg_pass_scene_signature(&changed) != signature, "cross-frame identity includes samples");
    changed = scene; changed.depth.vk_format++;
    CHECK(fg_pass_scene_signature(&changed) != signature, "cross-frame identity includes depth format");
    changed = scene; changed.color[7]=attachment(9,1280,720,100);
    CHECK(fg_pass_scene_signature(&changed)!=signature,"census signatures distinguish four/five-MRT attachment shapes");
    uint64_t expanded_signature=fg_pass_scene_signature(&changed);
    changed.color[1].vk_format++;
    CHECK(fg_pass_scene_signature(&changed)!=expanded_signature,"color-format changes remain distinct census groups, not camera identity");
    fg_pass_reset(&frame);
    int group = fg_pass_begin(&frame,&scene);
    fg_pass_guard guard; fg_pass_guard_reset(&guard,UINT_MAX);
    callback_state state = {0};
    for (unsigned draw = 0; draw < 5600; draw++) {
        /* Same VS/program, changing reflected input context: the old VS-only
           guard and four-context cap silently missed these draws. */
        fg_pass_guard_invalidate(&guard);
        fg_pass_route_draw(1,&frame,&guard,7,metadata,&state);
    }
    CHECK(fg_pass_route_programs(1,&frame,&guard,7,1,metadata,&state),"FS-only transition captures reflected context");
    CHECK(!fg_pass_route_programs(1,&frame,&guard,7,1,metadata,&state),"identical VS/FS ordinary draw does no metadata work");
    CHECK(fg_pass_route_programs(1,&frame,&guard,7,2,metadata,&state),"second FS-only transition is not hidden by stable VS");
    CHECK(!fg_pass_route_programs(0,NULL,NULL,7,2,metadata,NULL),"disabled VS/FS guard dereferences no tracking state");
    fg_pass_end(&frame,group,5602,5602);
    CHECK(state.calls == 5602 && state.samples == 5602 && !frame.incomplete_contexts && !frame.failed,
          "all changed FS/binding contexts fit production budget without per-pass truncation");
}

static void refusing_probe_and_post_contexts(void) {
    fg_pass_probe probe={0};
    unsigned probes=0;
    for (unsigned frame=0;frame<320;frame++) {
        probes += fg_pass_probe_due(&probe);
        fg_pass_probe_end(&probe,0,0);
    }
    CHECK(probes==10 && fg_pass_probe_due(&probe),"refusing state probes exactly once per32 real frames");
    fg_pass_probe_end(&probe,1,0);
    CHECK(fg_pass_probe_due(&probe),"eligible probe resumes immediately for adjacent real cameras");
    fg_pass_probe_end(&probe,1,0);
    CHECK(fg_pass_probe_due(&probe),"sustained eligible frames never enter backoff");
    CHECK(!fg_pass_ui_context(0,2,1),"UI-capable post/composite VS must retain pre-boundary scene inputs");
    CHECK(!fg_pass_ui_context(1,2,0),"UI-capable shader on another target still tracks scene inputs");
    CHECK(!fg_pass_ui_context(1,0,1),"non-UI shader after boundary is not exempt from scene checks");
    CHECK(fg_pass_ui_context(1,2,1),"only exact canvas UI after nominated boundary skips scene sampling");
}

static void capacity_probe_recovery(void) {
    fg_pass_probe probe={0};
    fg_pass_probe_end(&probe,0,1);
    CHECK(probe.wait==1 && probe.failed,"first capacity refusal skips only one tick");
    fg_pass_probe_end(&probe,0,0);
    CHECK(fg_pass_probe_due(&probe) && probe.failed,"backoff tick does not reset failure history");
    fg_pass_probe_end(&probe,1,0);
    CHECK(fg_pass_probe_due(&probe) && !probe.failed,"successful retry resets the consecutive failure streak");
    fg_pass_probe_end(&probe,0,1);
    CHECK(probe.wait==1,"an isolated later overflow gets a fresh short retry");
    fg_pass_probe_end(&probe,0,0);
    fg_pass_probe_end(&probe,0,1);
    CHECK(probe.wait==31,"second consecutive overflow escalates to the existing cooldown");
    for (unsigned i=0;i<31;i++) fg_pass_probe_end(&probe,0,0);
    CHECK(fg_pass_probe_due(&probe),"long cooldown preserves exact thirty-one skipped ticks");
    fg_pass_probe_end(&probe,1,0);
    CHECK(!probe.failed && !probe.wait,"recovery after escalated overflow resumes adjacent tracking");
    probe=(fg_pass_probe){0};
    unsigned probes=0;
    for (unsigned tick=0;tick<322;tick++) {
        probes+=fg_pass_probe_due(&probe);
        fg_pass_probe_end(&probe,0,1);
    }
    CHECK(probes==11 && fg_pass_probe_due(&probe),
          "persistent capacity exhaustion gets one short retry then stays bounded once per32 ticks");
    probe=(fg_pass_probe){0};
    fg_pass_probe_end(&probe,0,0);
    CHECK(probe.wait==31,"initial unsupported/load/transition refusal retains original backoff");
    for (unsigned i=0;i<31;i++) fg_pass_probe_end(&probe,0,0);
    fg_pass_probe_end(&probe,0,0);
    CHECK(probe.wait==31,"persistent unsupported scenes never use fast capacity retries");
}

int main(void) {
    thor_census();
    reopens_subresources_and_resizes();
    ranking_ties_and_ambiguity();
    guarded_metadata_events();
    capacities_fail_closed();
    sampled_links_and_lifetimes();
    operation_sampled_links();
    census_has_no_attachment_gate();
    unknown_attachments_remain_ungrouped();
    stable_scene_and_complete_contexts();
    refusing_probe_and_post_contexts();
    capacity_probe_recovery();
    printf("%s: framegen passes %u checks, %u failed\n", failures ? "FAILED" : "ok", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
