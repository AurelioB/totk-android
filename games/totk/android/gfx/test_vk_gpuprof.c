/* GPU profiler integration fixture: real NVN replay, recorder workers and Vulkan
 * timestamps/statistics, no game/device/WSI. Build vkgpuproftest and vkdraw-seed;
 * tests/test_gpuprof.py drives fresh processes for each launch-time switch.
 * The only profiler test hooks mark a frame (the ordinary present boundary,
 * without WSI) and flush a terminal partial window AFTER real fence retirement. */
#include "vk_int.h"
#include "vk_record.h"
#include "vk_gpuprof_time.h"
#include <sys/mman.h>

static unsigned checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void timestamp_contract(void) {
    vk_gpuprof_time_domain domain = {1, 2, 0, 48, 1};
    vk_gpuprof_timestamp first = {100, 1}, last = {200, 1};
    vk_gpuprof_interval value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && !value.wrapped && fabs(value.ms - 0.0001) < 1e-12,
        "same-domain available timestamp interval uses advertised units");
    first.available = 0; first.ticks = UINT64_MAX;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_UNAVAILABLE && !value.has_value, "unavailable timestamp cannot reuse stale value");
    first = (vk_gpuprof_timestamp){0, 1}; last = first;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.ms == 0, "available zero counter is legal");
    uint64_t mask = vk_gpuprof_tick_mask(48);
    first.ticks = mask - 9; last.ticks = 10;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.wrapped && fabs(value.ms - 0.00002) < 1e-12,
        "real 48-bit wrap remains a bounded valid interval");
    first.ticks = mask - 3; last.ticks = 0;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.wrapped && value.ms == 0.000004,
        "zero end timestamp at a real wrap is not fabricated unavailability");
    first.ticks = (UINT64_C(1) << 60) | 100; last.ticks = (UINT64_C(2) << 60) | 200;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && !value.wrapped && fabs(value.ms - 0.0001) < 1e-12,
        "only timestampValidBits participate in interval math");
    first.ticks = 100000000; last.ticks = 99999900; domain.period_ns = (double)(float)52.0833321;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_IMPLAUSIBLE && value.has_value && value.ms > 1e10,
        "Thor-style backwards endpoint is flagged, not accepted as a 169-day wrap");
    domain.period_ns = 1; domain.valid_bits = 64;
    first.ticks = UINT64_MAX - 3; last.ticks = 5;
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.wrapped && value.ms == 0.000009,
        "64-bit wrap avoids undefined shift and preserves exact delta");
    first.ticks = 0; last.ticks = UINT64_C(10000000000);
    value = vk_gpuprof_interval_check(first, last, domain, domain);
    CHECK(value.status == GP_TIME_OK && value.ms == VK_GPUPROF_MAX_FRAME_MS, "sanity limit is inclusive");
    last.ticks++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_IMPLAUSIBLE,
        "positive implausible duration is rejected too");
    vk_gpuprof_time_domain other = domain; other.queue++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "different queues cannot feed one span");
    other = domain; other.queue_family++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "queue families are explicit timestamp domains");
    other = domain; other.device++;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "different devices cannot feed one span");
    other = domain; other.period_ns = 2;
    CHECK(vk_gpuprof_interval_check(first, last, domain, other).status == GP_TIME_DOMAIN,
        "different timestamp units cannot feed one span");
    domain.valid_bits = 0;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_DOMAIN,
        "zero timestamp precision is unavailable");
    domain.valid_bits = 65;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_DOMAIN,
        "invalid timestamp precision never reaches a shift");
    domain.valid_bits = 48; domain.period_ns = NAN;
    CHECK(vk_gpuprof_interval_check(first, last, domain, domain).status == GP_TIME_DOMAIN,
        "invalid timestamp period is rejected");
}
static void unavailable_query_contract(void) {
    if (!V.has_host_query_reset) return;
    VkQueryPool pool;
    VkQueryPoolCreateInfo info = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VKCHECK(vkCreateQueryPool(V.dev, &info, NULL, &pool));
    vkResetQueryPool(V.dev, pool, 0, 2);
    vk_gpuprof_timestamp data[2] = {{UINT64_MAX, UINT64_MAX}, {UINT64_MAX, UINT64_MAX}};
    VkResult result = vkGetQueryPoolResults(V.dev, pool, 0, 2, sizeof data, data, sizeof data[0],
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    CHECK(result == VK_NOT_READY && !data[0].available && !data[1].available,
        "real reset-but-unwritten Vulkan queries return availability zero without waiting");
    vk_gpuprof_time_domain domain = {(uintptr_t)V.dev, (uintptr_t)V.queue, V.qfam,
        V.timestamp_bits, V.props.limits.timestampPeriod};
    CHECK(vk_gpuprof_interval_check(data[0], data[1], domain, domain).status == GP_TIME_UNAVAILABLE,
        "real unavailable query payload cannot form a valid span");
    vkDestroyQueryPool(V.dev, pool, NULL);
}
static uint8_t recording[16384];
static size_t used;
static void op(uint32_t code, const void *payload, uint32_t bytes) {
    uint32_t size = (8 + bytes + 7) & ~7u;
    if (used + size > sizeof recording) DIE("GPU profiler fixture recording overflow");
    op_hdr header = {code, size}; memcpy(recording + used, &header, sizeof header);
    if (bytes) memcpy(recording + used + 8, payload, bytes);
    memset(recording + used + 8 + bytes, 0, size - 8 - bytes); used += size;
}
static void shader_binary(unsigned stage, unsigned salt, uint8_t control[0x880], uint8_t code[0x230]) {
    /* Existing test_vkdraw_seed.py salts37/38: position-only VS and four
     * genuinely consumed fragment UBOs. No new shader/compiler convention. */
    memset(control, 0, 0x880);
    for (unsigned i = 0; i < 0x30; i++) code[i] = (uint8_t)salt;
    for (unsigned i = 0; i < 0x200; i++) code[0x30 + i] = (uint8_t)(i * 7 + salt);
    uint32_t header = 0x880, bytes = 0x230;
    memcpy(control + 0x20, &header, 4); memcpy(control + 0x700, &bytes, 4);
    memcpy(control + 0x704, &bytes, 4); memcpy(control + 0x714, &stage, 4);
}
static void wait_submission(uint64_t ticket) {
    int completed = ticket && mtl_wait(ticket, UINT64_C(5000000000));
    CHECK(completed, "real GPU fence retires before inspecting output or flushing profiler");
    if (!completed) { fflush(stderr); _Exit(1); }
}
static void submit(int frame_end) {
    op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(recording); used = 0;
    if (frame_end) { vk_end_rendering(); vk_gpuprof_test_frame(vk_cb()); }
    wait_submission(mtl_sub_commit());
}
static void record_draws(tex_t *target, prog_t *program, uint8_t *memory, unsigned draws) {
    p_set_rt rt = {.n = 1}; rt.color[0] = target; op(OP_SET_RT, &rt, sizeof rt);
    p_bind_program bind = {program, 3}; op(OP_BIND_PROGRAM, &bind, sizeof bind);
    p_bind_vattr attribute = {.n = 1}; va_st position = {0x16, 0, 0};
    memcpy(&attribute.st[0], &position, sizeof position); op(OP_BIND_VATTR, &attribute, sizeof attribute);
    p_bind_vstream stream = {.n = 1}; stream.st[0].stride = 8; op(OP_BIND_VSTREAM, &stream, sizeof stream);
    p_bind_vbuf vertex = {.n = 1};
    vertex.r[0].address = (NVNbufferAddress)(uintptr_t)(memory + 0x20000); vertex.r[0].size = 64;
    op(OP_BIND_VBUF, &vertex, sizeof vertex);
    for (unsigned slot = 0; slot < 4; slot++) {
        p_bind_buf uniform = {1, slot, (uint64_t)(uintptr_t)(memory + 0x10000 + slot * 0x1000), 1024};
        op(OP_BIND_UBO, &uniform, sizeof uniform);
    }
    p_viewports viewport = {.n = 1}; viewport.v[0][2] = (float)target->d.width;
    viewport.v[0][3] = (float)target->d.height; viewport.v[0][5] = 1;
    op(OP_VIEWPORTS, &viewport, sizeof viewport);
    p_scissor scissor = {0, 0, target->d.width, target->d.height}; op(OP_SCISSOR, &scissor, sizeof scissor);
    /* Keep each target's clear full-area so size is the only key difference,
     * rather than alternating LOAD vs CLEAR attachment operations. */
    p_clear_color clear = {0, 15, {0, 0, 0, 1}}; op(OP_CLEAR_COLOR, &clear, sizeof clear);
    p_draw draw = {4, 0, 3, 0, 0};
    for (unsigned i = 0; i < draws; i++) op(OP_DRAW, &draw, sizeof draw);
}
static uint8_t *readback_bytes(tex_t *target, uint8_t *memory) {
    return memory + 0x40000 + (target->d.width == 64 ? 0 : 0x10000);
}
static void record_readback(tex_t *target, uint8_t *memory) {
    p_copy_b2t readback = {.src = (uint64_t)(uintptr_t)readback_bytes(target, memory), .tex = target,
        .r = {0, 0, 0, target->d.width, target->d.height, 1}};
    op(OP_COPY_T2B, &readback, sizeof readback);
}
static void check_pixels(tex_t *target, uint8_t *memory) {
    size_t bad = 0, pixels = (size_t)target->d.width * target->d.height * 4;
    uint8_t *readback = readback_bytes(target, memory);
    for (size_t i = 0; i < pixels; i++) bad += readback[i] != 255;
    CHECK(!bad, "profiled draw produces exact all-white real Vulkan readback at both render extents");
}
static int finish_fixture(unsigned frames, unsigned passes, unsigned draws, int supported) {
    int mode = getenv("TOTK_VK_GPU_PROFILE") ? atoi(getenv("TOTK_VK_GPU_PROFILE")) : 0;
    int workers = getenv("TOTK_VK_REC_THREADS") ? atoi(getenv("TOTK_VK_REC_THREADS")) : 0;
    int stats = getenv("TOTK_VK_GPU_PROFILE_STATS") && !strcmp(getenv("TOTK_VK_GPU_PROFILE_STATS"), "1");
    CHECK(vk_gpuprof_on == mode, "launch-time GPU profile mode remains effective");
    CHECK(!!vk_record_enabled() == !!workers, "GPU profiling does not force requested recorder workers inline");
    CHECK(!!vk_gpuprof_stats_mask() == !!(mode && stats && V.has_pipeline_statistics),
        "statistics are available only when requested and supported");
    if (workers && draws) {
        uint64_t leases[4]; vk_record_test_leases(leases);
        CHECK(leases[1] && leases[2], "parallel recorder secondary leases are actually used and returned");
    }
    vk_rec_lock(); vk_gpuprof_test_flush(); vk_rec_unlock();
    vk_gpuprof_shutdown();
    vk_gpuprof_shutdown(); /* normal teardown is idempotent */
    vk_rec_lock(); vk_gpuprof_test_flush(); vk_rec_unlock(); /* late rows cannot follow session_end */
    printf("gpuprof-fixture frames=%u passes=%u draws=%u mode=%d workers=%d stats_supported=%d stats_enabled=%d checks=%u failures=%u\n",
        frames, passes, draws, mode, workers, supported,
        !!vk_gpuprof_stats_mask(), checks, failures);
    return failures != 0;
}
static void area_texture(tex_t *target, pool_t *gpu, size_t *offset,
                         unsigned format, unsigned width, unsigned height, uint64_t serial) {
    target->d = (nvn_texdesc){.target = 1, .format = format, .width = width,
        .height = height, .depth = 1, .levels = 1};
    nvn_tex_layout(&target->d, &target->L);
    *offset = (*offset + 0xffff) & ~(size_t)0xffff;
    if (*offset > gpu->size || target->L.size > gpu->size - *offset) DIE("GPU profiler area fixture texture overflow");
    target->mem = gpu->mem + *offset; target->pool = gpu; target->serial = serial;
    *offset += target->L.size;
}
static uint8_t *area_readback(const tex_t *target, uint8_t *memory) {
    return memory + (target->mem - target->pool->mem);
}
static void record_area_clear(tex_t *colors, tex_t *depth, uint8_t *memory) {
    p_set_rt rt = {.n = colors ? 2 : 0, .depth = depth};
    if (colors) { rt.color[0] = &colors[0]; rt.color[1] = &colors[1]; }
    op(OP_SET_RT, &rt, sizeof rt);
    p_scissor scissor = {0, 0, depth->d.width, depth->d.height}; op(OP_SCISSOR, &scissor, sizeof scissor);
    for (int i = 0; i < rt.n; i++) {
        p_clear_color clear = {i, 15, {i == 0, i == 1, 0, 1}};
        op(OP_CLEAR_COLOR, &clear, sizeof clear);
    }
    p_clear_ds clear = {0.25f, 1, 0, 0}; op(OP_CLEAR_DS, &clear, sizeof clear);
    /* A copy forces pending load-clears into the real dynamic-rendering path.
     * It also proves the supplied NVN metadata describes actual GPU images. */
    for (int i = 0; i <= rt.n; i++) {
        tex_t *target = i < rt.n ? &colors[i] : depth;
        memset(area_readback(target, memory), 0xc7, (size_t)target->d.width * target->d.height * 4);
        p_copy_b2t copy = {.src = (uint64_t)(uintptr_t)area_readback(target, memory), .tex = target,
            .r = {0, 0, 0, target->d.width, target->d.height, 1}};
        op(OP_COPY_T2B, &copy, sizeof copy);
    }
}
static void check_area_pixels(const tex_t *target, uint8_t *memory, int color) {
    const uint8_t *pixels = area_readback(target, memory);
    size_t bad = 0, count = (size_t)target->d.width * target->d.height;
    for (size_t i = 0; i < count; i++) {
        if (color < 0) {
            float depth; memcpy(&depth, pixels + i * 4, sizeof depth); bad += depth != 0.25f;
        } else {
            const uint8_t *p = pixels + i * 4;
            bad += p[0] != (color == 0 ? 255 : 0) || p[1] != (color == 1 ? 255 : 0) || p[2] || p[3] != 255;
        }
    }
    CHECK(!bad, "area fixture real Vulkan color/depth clear has exact readback pixels");
}
static int run_areas(uint8_t *memory, pool_t *gpu, int supported) {
    tex_t scene[2][3] = {0}, fixed[2] = {0}; size_t offset = 0;
    for (unsigned extent = 0; extent < 2; extent++) for (unsigned slot = 0; slot < 3; slot++)
        area_texture(&scene[extent][slot], gpu, &offset, slot == 2 ? 0x34 : 0x25,
            64u << extent, 36u << extent, 86000 + extent * 3 + slot);
    area_texture(&fixed[0], gpu, &offset, 0x34, 360, 360, 86006);
    area_texture(&fixed[1], gpu, &offset, 0x34, 128, 128, 86007);
    for (unsigned frame = 0; frame < 3; frame++) {
        unsigned geometry = frame == 0 ? 1 : frame == 1 ? 2 : 0;
        unsigned prepass = frame < 2, transfers = frame == 0 ? 6 : frame == 1 ? 9 : 2;
        unsigned width = frame < 2 ? 64u << frame : 0, height = frame < 2 ? 36u << frame : 0;
        if (prepass) {
            record_area_clear(NULL, &scene[frame][2], memory);
            record_area_clear(scene[frame], &scene[frame][2], memory);
        }
        /* The smaller MRT comes last: scene selection must take the largest
         * render area, not the most recent pass or the larger fixed shadow.
         * This exact key also has two distinct completed-frame references. */
        if (frame == 1) record_area_clear(scene[0], &scene[0][2], memory);
        record_area_clear(NULL, &fixed[0], memory);
        record_area_clear(NULL, &fixed[1], memory);
        submit(1);
        if (frame < 2) for (unsigned extent = 0; extent <= frame; extent++) {
            for (int color = 0; color < 2; color++) check_area_pixels(&scene[extent][color], memory, color);
            check_area_pixels(&scene[extent][2], memory, -1);
        }
        check_area_pixels(&fixed[0], memory, -1); check_area_pixels(&fixed[1], memory, -1);
        printf("gpuprof-areas-frame frame=%u scene_width=%u scene_height=%u passes=%u prepass=%u geometry=%u shadow=1 other_depth=1 transfers=%u\n",
            frame + 1, width, height, prepass + geometry + 2, prepass, geometry, transfers);
    }
    printf("gpuprof-areas frames=3 passes=11 draws=0 exact_keys=6 families=5 transfers=17\n");
    return finish_fixture(3, 11, 0, supported);
}
static int run_unavailable_span(int supported) {
    /* Both command buffers are real. Only the first is submitted, so the
     * reserved frame/slot end queries must remain unavailable after its fence. */
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = V.qfam};
    VKCHECK(vkCreateCommandPool(V.dev, &pi, NULL, &pool));
    VkCommandBuffer cb[2];
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(cb[0], &bi)); VKCHECK(vkBeginCommandBuffer(cb[1], &bi));
    vk_gpuprof_slot *slot = NULL;
    vk_rec_lock();
    vk_gpuprof_slot_begin(&slot, cb[0]);
    vk_gpuprof_test_frame(cb[1]); vk_gpuprof_slot_end(slot, cb[1]); vk_gpuprof_slot_seal(slot);
    vk_rec_unlock();
    VKCHECK(vkEndCommandBuffer(cb[0])); VKCHECK(vkEndCommandBuffer(cb[1]));
    VkFence fence; VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(V.dev, &fi, NULL, &fence));
    VkCommandBufferSubmitInfo command = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb[0]};
    VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &command};
    VKCHECK(vkQueueSubmit2(V.queue, 1, &submit, fence));
    VKCHECK(vkWaitForFences(V.dev, 1, &fence, VK_TRUE, UINT64_C(5000000000)));
    vk_rec_lock(); vk_gpuprof_slot_complete(slot); vk_rec_unlock();
    vkDestroyFence(V.dev, fence, NULL); vkDestroyCommandPool(V.dev, pool, NULL);
    printf("gpuprof-unavailable expected_frames=1 expected_invalid_spans=1\n");
    return finish_fixture(1, 0, 0, supported);
}
int main(int argc, char **argv) {
    timestamp_contract();
    unsigned frames = 302;
    int areas = argc == 2 && !strcmp(argv[1], "--areas");
    int unavailable_span = argc == 2 && !strcmp(argv[1], "--unavailable-span");
    if (argc == 3 && !strcmp(argv[1], "--frames")) {
        char *end; unsigned long value = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || !value || value > 10000) return 2;
        frames = (unsigned)value;
    } else if (argc != 1 && !areas && !unavailable_span) return 2;
    /* Force short secondary chunks so workers=2 tests a real parallel pass,
     * rather than merely checking an enabled worker setting on inline draws. */
    setenv("TOTK_VK_REC_CHUNK_DRAWS", "1", 1); setenv("TOTK_VK_REC_LEAD_DRAWS", "1", 1);
    setenv("TOTK_VK_REC_INLINE_DRAWS", "0", 1); setenv("TOTK_VK_CHUNK_DRAWS", "0", 1);
    setenv("TOTK_POST_AA", "off", 1);
    const size_t bytes = 4u << 20;
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("gpuprof fixture mmap"); return 1; }
    mtl_init();
    unavailable_query_contract();
    int supported = V.has_pipeline_statistics;
    if (getenv("GPUPROF_TEST_UNSUPPORTED")) V.has_pipeline_statistics = 0;
    if (unavailable_span) return run_unavailable_span(supported);
    mtl_pool_add(memory, bytes);
    pool_t pool = {.mem = memory, .size = bytes,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_t gpu = pool; gpu.mem = memory + (2u << 20); gpu.size = 2u << 20;
    gpu.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    if (areas) return run_areas(memory, &gpu, supported);
    tex_t targets[2]; memset(targets, 0, sizeof targets);
    for (unsigned i = 0; i < 2; i++) {
        targets[i].d = (nvn_texdesc){.target = 1, .format = 0x25, .width = 64u << i,
            .height = 64u << i, .depth = 1, .levels = 1};
        nvn_tex_layout(&targets[i].d, &targets[i].L);
        targets[i].mem = gpu.mem + i * 0x40000; targets[i].pool = &gpu; targets[i].serial = 84000 + i;
    }
    static uint8_t control[2][0x880]; prog_t programs[2]; memset(programs, 0, sizeof programs);
    for (unsigned copy = 0; copy < 2; copy++) {
        programs[copy].nstage = 2; programs[copy].serial = 85000 + copy;
        for (unsigned stage = 0; stage < 2; stage++) {
            uint8_t *code = memory + copy * 0x2000 + stage * 0x1000;
            shader_binary(stage, stage ? 38 : 37, control[stage], code);
            programs[copy].sd[stage].control = control[stage];
            programs[copy].sd[stage].data = (NVNbufferAddress)(uintptr_t)code;
        }
    }
    for (unsigned slot = 0; slot < 4; slot++) {
        float *uniform = (float *)(memory + 0x10000 + slot * 0x1000);
        for (unsigned i = 0; i < 256; i++) uniform[i] = 1;
    }
    const float triangle[] = {-1, -1, 3, -1, -1, 3}; memcpy(memory + 0x20000, triangle, sizeof triangle);
    unsigned expected_draws = 0, expected_passes = 0;
    for (unsigned frame = 0; frame < frames; frame++) {
        tex_t *target = &targets[frame & 1]; unsigned draws = frame & 1 ? 3 : 1;
        record_draws(target, &programs[(frame / 2) & 1], memory, draws);
        record_readback(target, memory);
        expected_draws += draws; expected_passes++;
        if (frame % 11 == 7) {
            /* A frame spanning two actual submissions must have one frame row,
             * no lost first segment and pass ordinals independent of slot reuse. */
            submit(0); record_draws(target, &programs[(frame / 2) & 1], memory, draws);
            record_readback(target, memory);
            expected_draws += draws; expected_passes++;
        }
        submit(1);
        check_pixels(target, memory);
    }
    /* Record a genuine pre upload while the main render record is OPEN. The
     * uploader must restore that parent rather than losing its timestamp/key/
     * statistics. Two frame markers then share one unsubmitted main buffer. */
    memset(memory + 0x40000, 0xc7, 0x20000);
    record_draws(&targets[0], &programs[0], memory, 1);
    op(OP_END, NULL, 0); mtl_sub_begin(); mtl_exec(recording); used = 0;
    VkBuffer upload_buffer; VkDeviceSize upload_offset;
    uint32_t *upload = vk_transient(256, 256, &upload_buffer, &upload_offset);
    memset(upload, 0, 256);
    VkCommandBuffer pre = vk_pre_cb();
    if (vk_gpuprof_on) vk_gpuprof_scope_begin(pre, "upload");
    vkCmdFillBuffer(pre, upload_buffer, upload_offset, 256, 0x13579bdf);
    if (vk_gpuprof_on) vk_gpuprof_scope_end(pre);
    record_readback(&targets[0], memory);
    op(OP_END, NULL, 0); mtl_exec(recording); used = 0;
    vk_end_rendering(); vk_gpuprof_test_frame(vk_cb());
    record_draws(&targets[1], &programs[1], memory, 3); record_readback(&targets[1], memory);
    op(OP_END, NULL, 0); mtl_exec(recording); used = 0;
    vk_end_rendering(); vk_gpuprof_test_frame(vk_cb());
    wait_submission(mtl_sub_commit());
    unsigned bad_upload = 0;
    for (unsigned i = 0; i < 64; i++) bad_upload += upload[i] != 0x13579bdf;
    CHECK(!bad_upload, "direct profiled pre scope really fills bytes before the open main rendering executes");
    check_pixels(&targets[0], memory); check_pixels(&targets[1], memory);
    frames += 2; expected_passes += 2; expected_draws += 4;
    return finish_fixture(frames, expected_passes, expected_draws, supported);
}
