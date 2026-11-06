/* GPU profiler integration fixture: real NVN replay, recorder workers and Vulkan
 * timestamps/statistics, no game/device/WSI. Build vkgpuproftest and vkdraw-seed;
 * tests/test_gpuprof.py drives fresh processes for each launch-time switch.
 * The only profiler test hooks mark a frame (the ordinary present boundary,
 * without WSI) and flush a terminal partial window AFTER real fence retirement. */
#include "vk_int.h"
#include "vk_record.h"
#include <sys/mman.h>

static unsigned checks, failures;
#define CHECK(c, label) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
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
    /* Change the clear's scissor with the target, otherwise alternating extents
     * intentionally create LOAD vs CLEAR keys rather than testing scale invariance. */
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
int main(int argc, char **argv) {
    unsigned frames = 302;
    if (argc == 3 && !strcmp(argv[1], "--frames")) {
        char *end; unsigned long value = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || !value || value > 10000) return 2;
        frames = (unsigned)value;
    } else if (argc != 1) return 2;
    /* Force short secondary chunks so workers=2 tests a real parallel pass,
     * rather than merely checking an enabled worker setting on inline draws. */
    setenv("TOTK_VK_REC_CHUNK_DRAWS", "1", 1); setenv("TOTK_VK_REC_LEAD_DRAWS", "1", 1);
    setenv("TOTK_VK_REC_INLINE_DRAWS", "0", 1); setenv("TOTK_VK_CHUNK_DRAWS", "0", 1);
    setenv("TOTK_POST_AA", "off", 1);
    const size_t bytes = 4u << 20;
    uint8_t *memory = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("gpuprof fixture mmap"); return 1; }
    mtl_init();
    int supported = V.has_pipeline_statistics;
    if (getenv("GPUPROF_TEST_UNSUPPORTED")) V.has_pipeline_statistics = 0;
    mtl_pool_add(memory, bytes);
    pool_t pool = {.mem = memory, .size = bytes,
        .flags = NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    pool_t gpu = pool; gpu.mem = memory + (2u << 20); gpu.size = 2u << 20;
    gpu.flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
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
    int mode = getenv("TOTK_VK_GPU_PROFILE") ? atoi(getenv("TOTK_VK_GPU_PROFILE")) : 0;
    int workers = getenv("TOTK_VK_REC_THREADS") ? atoi(getenv("TOTK_VK_REC_THREADS")) : 0;
    int stats = getenv("TOTK_VK_GPU_PROFILE_STATS") && !strcmp(getenv("TOTK_VK_GPU_PROFILE_STATS"), "1");
    CHECK(vk_gpuprof_on == mode, "launch-time GPU profile mode remains effective");
    CHECK(!!vk_record_enabled() == !!workers, "GPU profiling does not force requested recorder workers inline");
    CHECK(!!vk_gpuprof_stats_mask() == !!(mode && stats && V.has_pipeline_statistics),
        "statistics are available only when requested and supported");
    if (workers) {
        uint64_t leases[4]; vk_record_test_leases(leases);
        CHECK(leases[1] && leases[2], "parallel recorder secondary leases are actually used and returned");
    }
    vk_rec_lock(); vk_gpuprof_test_flush(); vk_rec_unlock();
    printf("gpuprof-fixture frames=%u passes=%u draws=%u mode=%d workers=%d stats_supported=%d stats_enabled=%d checks=%u failures=%u\n",
        frames, expected_passes, expected_draws, mode, workers, supported,
        !!vk_gpuprof_stats_mask(), checks, failures);
    return failures != 0;
}
