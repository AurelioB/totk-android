/* Host-only CPU microbenchmark, not Android/device performance evidence.
   Build/run (do not mix compilation into timing):
     cc -std=c11 -O2 -Wall -Wextra -Werror tests/bench_framegen_tracking.c \
        -o /tmp/bench-framegen-tracking
     /tmp/bench-framegen-tracking 8000 12345 all

   Uses the SAME fg_pass_route_draw fast guard/event routing as vk_draw.c.
   Baseline/off/on each make one noinline draw call and consume identical
   runtime volatile program inputs plus an observable draw digest. Off reads
   only the cached active integer inside the shared router. On additionally
   measures exact census/pass aggregation, event/binding budgets and synthetic
   bounded metadata callbacks with real fg_dep reads/writes/version links.
   It does NOT measure Vulkan, guest-memory reads, shader reflection, camera
   validation/math, renderer backing interning, real UBO dirty-write indexing,
   barriers/captures, logging, GPU work/waits, scheduling or generated frames.
   A metadata callback uses four synthetic reflected bindings, not game data.

   Select topology all (default), small (12 physical / 9 logical), scene
   (128 / 64), or cap (2048 / 512). All run exact 4200/5600-draw frames.
   Small retains four main-scene chunks and 37/2/2/1 tiny old-signature groups.
   Larger cases reopen every logical group two/four times, with many low-draw
   postpasses; remaining draws belong to the two-color D24S8 main group.
   Cap intentionally approaches fixed census limits; metadata/link capacity
   failures are reported as expected fail-closed outcomes, not hidden or used
   to claim complete tracing. Identity/overflow/digest failures still fail.
   Volatile VS sequences include long ordinary runs and program transitions.
   Changed contexts are no longer capped per physical pass; the same global
   event/binding budgets as production remain explicitly fail-closed. */
#define _POSIX_C_SOURCE 200809L
#include "../games/totk/android/gfx/framegen_passes.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

enum { BENCH_MAX_PASSES = 2048, BENCH_MAX_DRAWS = 5600, BENCH_WARMUP = 128, BENCH_CONTEXTS = UINT_MAX };
typedef struct {
    fg_pass_key key;
    fg_dep_key input;
    unsigned draws, first_draw;
} bench_pass;
typedef struct {
    bench_pass pass[BENCH_MAX_PASSES];
    volatile uintptr_t program[BENCH_MAX_DRAWS];
    volatile uint64_t payload[16];
    unsigned draws, physical, logical, main_draws;
} bench_input;
typedef struct {
    fg_pass_frame passes;
    fg_pass_guard guard;
    fg_dep_frame deps;
    fg_dep_sources sources;
    const bench_input *input;
    const bench_pass *pass;
    volatile int active;
    volatile uint64_t metadata_sink;
    uint64_t draw_sink, committed, callbacks;
} bench_context;
typedef struct {
    uint64_t elapsed, cpu, events, callbacks, bindings, samples, incomplete;
    uint64_t draw_digest, metadata_digest;
    unsigned failed, groups, physical;
} bench_result;
typedef void (*bench_draw_fn)(bench_context *, uintptr_t);
typedef struct {
    const char *name;
    unsigned physical, logical, capacity_stress;
} bench_topology;
static const bench_topology topologies[] = {
    {"small", 12, 9, 0}, {"scene", 128, 64, 0}, {"cap", 2048, 512, 1}
};
static bench_context context;
static bench_input inputs;

static uint64_t now_ns(clockid_t clock_id) {
    struct timespec time;
    if (clock_gettime(clock_id, &time)) { perror("clock_gettime"); exit(EXIT_FAILURE); }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}
static fg_pass_attachment make_attachment(uint64_t backing, unsigned width, unsigned height, unsigned format) {
    return (fg_pass_attachment){.key = {.backing = backing, .aspect = 1},
        .nvn_format = format + 700, .vk_format = format, .width = width, .height = height, .samples = 1};
}
static fg_pass_key make_key(unsigned logical, unsigned width, unsigned height, unsigned colors, unsigned depth) {
    fg_pass_key key = {.width = width, .height = height};
    for (unsigned i = 0; i < colors; i++) key.color[i] = make_attachment(100 + logical * 16 + i, width, height, 97);
    if (depth) {
        key.depth = make_attachment(108 + logical * 16, width, height, depth);
        key.depth.key.aspect = 6;
    }
    return key;
}
static void fill_inputs(unsigned total_draws, uint64_t seed, const bench_topology *topology) {
    static const unsigned small_logical[12] = {1, 2, 0, 5, 3, 0, 4, 0, 0, 6, 7, 8};
    unsigned small_draws[12] = {37, 2, 0, 90, 2, 0, 1, 0, 0, 4, 4, 60};
    unsigned reopens = topology->physical / topology->logical;
    unsigned main_draws;
    if (topology->physical == 12) {
        unsigned quarter = (total_draws - 200) / 4;
        small_draws[2] = small_draws[5] = small_draws[7] = small_draws[8] = quarter;
        main_draws = total_draws - 200;
        reopens = 4;
    } else {
        /* Every non-main physical pass commits at least one draw. The first
           four tiny groups have 37/2/2/1 draws in their first occurrence. */
        main_draws = total_draws - (topology->physical - reopens) - 38;
    }
    unsigned cursor = 0, main_occurrence = 0;
    for (unsigned i = 0; i < topology->physical; i++) {
        unsigned id = topology->physical == 12 ? small_logical[i] : i % topology->logical;
        bench_pass *pass = &inputs.pass[i];
        pass->input = (fg_dep_key){0};
        unsigned interval;
        if (topology->physical == 12) {
            unsigned width = id >= 6 || !id ? 1280 : 128;
            unsigned height = id >= 6 || !id ? 720 : 128;
            pass->key = make_key(id, width, height, id >= 5 ? 1 : id ? 3 : 2, id >= 6 ? 0 : id ? 126 : 129);
            if (id >= 6) pass->input = inputs.pass[id == 6 ? 8 : id == 7 ? 9 : 10].key.color[0].key;
            pass->draws = small_draws[i];
            interval = !id ? 256 : id == 8 ? 8 : 32;
        } else {
            unsigned shift = !id ? 0 : id % 4;
            unsigned width = 1280u >> shift, height = 720u >> shift;
            unsigned colors = !id || id < 5 ? 2 : 1;
            pass->key = make_key(id, width, height, colors, !id ? 129 : id % 8 ? 0 : 124);
            if (id >= 5) pass->input = inputs.pass[i - 1].key.color[0].key;
            if (!id) {
                pass->draws = main_draws / reopens + (main_occurrence < main_draws % reopens);
                main_occurrence++;
            } else pass->draws = i == 1 ? 37 : i == 2 || i == 3 ? 2 : 1;
            interval = !id ? 256 : 32;
        }
        pass->first_draw = cursor;
        for (unsigned draw = 0; draw < pass->draws; draw++) {
            /* Identical runtime, volatile inputs in every measured mode. */
            uintptr_t vs = (uintptr_t)((seed & 0xffffu) + 1 + id * 32 + draw / interval % 8);
            inputs.program[cursor++] = vs;
        }
    }
    inputs.draws = cursor;
    inputs.physical = topology->physical;
    inputs.logical = topology->logical;
    inputs.main_draws = main_draws;
    for (unsigned i = 0; i < 16; i++) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        inputs.payload[i] = seed;
    }
    if (cursor != total_draws) { fprintf(stderr, "invalid benchmark draw schedule\n"); exit(EXIT_FAILURE); }
}

/* All draw variants execute the same observable stand-in for pre-existing
   renderer work. No tracking counters are added to ordinary production draws. */
static inline void common_draw(bench_context *state, uintptr_t program) {
    state->draw_sink = (state->draw_sink << 7 | state->draw_sink >> 57) ^ (uint64_t)program;
    state->committed++;
}
static NOINLINE void metadata(void *opaque, unsigned event) {
    bench_context *state = opaque;
    uint64_t value = state->metadata_sink ^ event;
    state->callbacks++;
    if (event & FG_PASS_EVENT_SAMPLE) {
        if (state->passes.failed || state->deps.failed || !fg_pass_take_bindings(&state->passes, 4))
            fg_pass_skip_context(&state->passes);
        else for (unsigned i = 0; i < 4; i++) {
            value = value * 33 + state->input->payload[(i + event) & 15];
            if (state->pass->input.backing) {
                fg_dep_value source = fg_dep_read(&state->deps, state->pass->input);
                fg_dep_sources_add(&state->deps, &state->sources, source);
                if (!fg_pass_link(&state->passes, state->deps.epoch, source)) {
                    fg_pass_skip_context(&state->passes);
                    break;
                }
            }
        }
    }
    state->metadata_sink = value;
}
static NOINLINE void baseline_draw(bench_context *state, uintptr_t program) {
    common_draw(state, program);
}
static NOINLINE void tracked_draw(bench_context *state, uintptr_t program) {
    (void)fg_pass_route_draw(state->active, &state->passes, &state->guard,
                             program, metadata, state);
    common_draw(state, program);
}

/* A volatile function selection is read once per frame, not once per draw.
   All modes have equal noinline/indirect call overhead; the compiler cannot
   replace the baseline with an empty loop or fold the cached active branch. */
static bench_draw_fn volatile selected_draw;
static NOINLINE void run_frames(unsigned frames, int mode, bench_result *result) {
    bench_context *state = &context;
    for (unsigned frame = 0; frame < frames; frame++) {
        bench_draw_fn draw = selected_draw;
        state->active = mode == 2;
        if (mode == 2) {
            fg_pass_reset(&state->passes);
            fg_dep_reset(&state->deps);
        }
        uint64_t frame_start = state->committed;
        for (unsigned p = 0; p < state->input->physical; p++) {
            const bench_pass *pass = &state->input->pass[p];
            state->pass = pass;
            int group = -1;
            if (mode == 2) {
                group = fg_pass_begin(&state->passes, &pass->key);
                fg_pass_guard_reset(&state->guard, BENCH_CONTEXTS);
                fg_dep_sources_init(&state->sources);
            }
            uint64_t before = state->committed;
            for (unsigned d = 0; d < pass->draws; d++)
                draw(state, state->input->program[pass->first_draw + d]);
            if (mode == 2) {
                uint64_t committed = state->committed - before;
                fg_pass_end(&state->passes, group, committed, pass->key.depth.key.backing ? committed : 0);
                if (!state->passes.failed && !state->deps.failed) {
                    for (unsigned c = 0; c < FG_PASS_COLORS; c++)
                        if (pass->key.color[c].key.backing)
                            (void)fg_dep_write(&state->deps, pass->key.color[c].key, &state->sources,
                                               state->passes.physical_passes, 0, 0, 0, 0);
                    if (pass->key.depth.key.backing)
                        (void)fg_dep_write(&state->deps, pass->key.depth.key, NULL,
                                           state->passes.physical_passes, 0, 0, 0, 0);
                }
            }
        }
        if (state->committed - frame_start != state->input->draws) result->failed |= FG_PASS_FAILED_IDENTITY;
        if (mode == 2) {
            result->events += state->passes.events_used;
            result->samples += state->passes.sample_events;
            result->bindings += state->passes.bindings_used;
            result->incomplete += state->passes.incomplete_contexts;
            result->failed |= state->passes.failed | state->deps.failed;
            result->groups = state->passes.count;
            result->physical = state->passes.physical_passes;
        }
    }
}
static bench_result measure(unsigned frames, int mode, uint64_t seed) {
    bench_result warmup = {0}, result = {0};
    context.input = &inputs;
    context.draw_sink = seed;
    context.metadata_sink = seed;
    context.committed = context.callbacks = 0;
    selected_draw = mode ? tracked_draw : baseline_draw;
    run_frames(BENCH_WARMUP, mode, &warmup);
    context.draw_sink = seed;
    context.metadata_sink = seed;
    context.committed = context.callbacks = 0;
    uint64_t cpu = now_ns(CLOCK_THREAD_CPUTIME_ID), elapsed = now_ns(CLOCK_MONOTONIC);
    run_frames(frames, mode, &result);
    result.elapsed = now_ns(CLOCK_MONOTONIC) - elapsed;
    result.cpu = now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
    result.callbacks = context.callbacks;
    result.draw_digest = context.draw_sink;
    result.metadata_digest = context.metadata_sink;
    result.failed |= warmup.failed;
    return result;
}
static unsigned parse_frames(const char *text) {
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *end || value < 128 || value > 1000000) {
        fprintf(stderr, "frames must be an integer in [128, 1000000]\n");
        exit(EXIT_FAILURE);
    }
    return (unsigned)value;
}
int main(int argc, char **argv) {
    if (argc > 4) { fprintf(stderr, "usage: %s [frames=8000] [seed] [topology=all|small|scene|cap]\n", argv[0]); return EXIT_FAILURE; }
    unsigned frames = argc > 1 ? parse_frames(argv[1]) : 8000;
    uint64_t seed = now_ns(CLOCK_MONOTONIC) | 1;
    if (argc > 2) {
        char *end;
        errno = 0;
        seed = strtoull(argv[2], &end, 0);
        if (errno || !*argv[2] || *end) { fprintf(stderr, "invalid seed\n"); return EXIT_FAILURE; }
        seed |= 1;
    }
    const char *selection = argc > 3 ? argv[3] : "all";
    if (strcmp(selection, "all") && strcmp(selection, "small") && strcmp(selection, "scene") && strcmp(selection, "cap")) {
        fprintf(stderr, "topology must be all, small, scene, or cap\n");
        return EXIT_FAILURE;
    }
    printf("framegen host shared-router benchmark: frames=%u warmup=%u seed=%" PRIu64 "\n", frames, BENCH_WARMUP, seed);
    printf("caps: groups=%u physical=%u metadata_events=%u bindings=%u sampled_links=%u sample_contexts/pass=%u\n",
           FG_PASS_MAX_GROUPS, FG_PASS_MAX_PHYSICAL, FG_PASS_MAX_EVENTS, FG_PASS_MAX_BINDINGS, FG_PASS_MAX_LINKS, BENCH_CONTEXTS);
    puts("baseline = equal noinline draw call + volatile inputs/draw digest; off = exact cached-active router; on = router + census + bounded synthetic callbacks/dependencies");
    puts("Excluded: real shader reflection/camera/guest memory/backing interning/UBO indexing/Vulkan/barriers/capture/logging/GPU/presentation; no device/FPS claim.");
    static const char *const names[] = {"baseline", "off", "on"};
    unsigned failed = 0;
    unsigned capacity_flags = FG_PASS_FAILED_GROUPS | FG_PASS_FAILED_PHYSICAL |
        FG_PASS_FAILED_EVENTS | FG_PASS_FAILED_BINDINGS | FG_PASS_FAILED_LINKS;
    for (unsigned t = 0; t < sizeof topologies / sizeof topologies[0]; t++) {
        const bench_topology *topology = &topologies[t];
        if (strcmp(selection, "all") && strcmp(selection, topology->name)) continue;
        for (unsigned draws = 4200; draws <= 5600; draws += 1400) {
            fill_inputs(draws, seed, topology);
            double baseline = 0;
            uint64_t baseline_digest = 0;
            for (int mode = 0; mode < 3; mode++) {
                bench_result result = measure(frames, mode, seed);
                double per_draw = result.elapsed / ((double)frames * draws);
                if (!mode) { baseline = per_draw; baseline_digest = result.draw_digest; }
                if (result.draw_digest != baseline_digest) result.failed |= FG_PASS_FAILED_IDENTITY;
                unsigned expected_closed = topology->capacity_stress ? result.failed & capacity_flags : 0;
                printf("topology=%s draws/frame=%u main=%u mode=%s elapsed_ns/draw=%.3f cpu_ns/draw=%.3f delta_baseline_ns/draw=%+.3f "
                       "slow_events/frame=%.2f routed_callbacks/frame=%.2f sample_permissions/frame=%.2f bindings/frame=%.2f "
                       "omitted_contexts/frame=%.2f groups=%u/%u physical=%u/%u failed=0x%x expected_capacity_closed=0x%x "
                       "draw_digest=%016" PRIx64 " callback_digest=%016" PRIx64 "\n",
                       topology->name, draws, inputs.main_draws, names[mode], per_draw, result.cpu / ((double)frames * draws), per_draw - baseline,
                       result.events / (double)frames, result.callbacks / (double)frames, result.samples / (double)frames,
                       result.bindings / (double)frames, result.incomplete / (double)frames, result.groups, inputs.logical,
                       result.physical, inputs.physical, result.failed, expected_closed, result.draw_digest, result.metadata_digest);
                failed |= result.failed & ~expected_closed;
            }
        }
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
