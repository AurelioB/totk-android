/* Actual NativeAOT service + production negative caches + real Vulkan dispatch, no game files.
   Usage: vkxlatetest /absolute/linux-x64/totk_xlate <empty-scratch-directory> [--disabled|--absent]
   The tiny Maxwell compute program contains a scheduling word and unconditional EXIT. */
#include "vk_int.h"
#include "vk_record.h"
#include "vk_shader_xlate.h"
#include "vk_seed_io.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned dispatches;
static PFN_vkCmdDispatch real_dispatch;
static VKAPI_ATTR void VKAPI_CALL count_dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    dispatches++; real_dispatch(cb, x, y, z);
}
static int run_dispatch(prog_t *program) {
    uint8_t commands[128] = {0}; size_t used = 0;
    p_bind_program bind = {program, 1u << 5}; p_dispatch dispatch = {1, 1, 1};
    const uint32_t ops[] = {OP_BIND_PROGRAM, OP_DISPATCH, OP_END};
    const void *data[] = {&bind, &dispatch, NULL}; const size_t sizes[] = {sizeof bind, sizeof dispatch, 0};
    for (int i = 0; i < 3; i++) {
        op_hdr h = {ops[i], (uint32_t)((8 + sizes[i] + 7) & ~7u)};
        memcpy(commands + used, &h, 8); if (sizes[i]) memcpy(commands + used + 8, data[i], sizes[i]); used += h.size;
    }
    mtl_sub_begin(); mtl_exec(commands);
    return mtl_wait(mtl_sub_commit(), 5000000000ull);
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <translator> <empty scratch dir> [--disabled|--absent]\n", argv[0]); return 2; }
    int disabled = argc > 3 && !strcmp(argv[3], "--disabled");
    char seed[1024], requests[1024];
    snprintf(seed, sizeof seed, "%s/seed", argv[2]); snprintf(requests, sizeof requests, "%s/requests", argv[2]);
    if (mkdir(seed, 0700) || mkdir(requests, 0700)) { perror("test directories (must not already exist)"); return 2; }
    /* Seed readiness requires a module inventory before guest resolution. This unreferenced header-only
       fixture establishes presence; the actual compute variant must still arrive through translation. */
    char marker[1100]; snprintf(marker, sizeof marker, "%s/inventory-marker.spv", seed);
    uint32_t header[5] = {0x07230203, 0x00010300, 0, 1, 0};
    FILE *initial = fopen(marker, "wb");
    if (!initial || fwrite(header, sizeof header, 1, initial) != 1 || fclose(initial)) return 2;
    int absent = argc > 3 && !strcmp(argv[3], "--absent");
    setenv("TOTK_SHADER_TRANSLATOR", argv[1], 1); setenv("TOTK_XLATE_ONDEVICE", disabled ? "0" : "1", 1);
    setenv("TOTK_SHADER_SEED_DIR", seed, 1); setenv("TOTK_SHADER_REQ_DIR", requests, 1);
    setenv("TOTK_VK_PREWARM", "0", 1); setenv("TOTK_VK_PIPELINE_CACHE", "0", 1); setenv("TOTK_FRAME_LAG", "0", 1);
    mtl_init(); real_dispatch = vkCmdDispatch; vkCmdDispatch = count_dispatch;
    uint8_t control[0x880] = {0}; uint64_t code[4] = {0, 0xe30000000007000full, 0, 0};
    uint32_t length = sizeof code, csize = sizeof control, stage = 5, local[3] = {1, 1, 1};
    memcpy(control + 0x20, &csize, 4); memcpy(control + 0x700, &length, 4); memcpy(control + 0x704, &length, 4);
    memcpy(control + 0x714, &stage, 4); memcpy(control + 0x748, local, sizeof local);
    prog_t program = {.nstage = 1, .serial = 10001}; program.sd[0].control = control; program.sd[0].data = (uintptr_t)code;
    vk_shader *sh[6]; uint64_t start = host_now_ns();
    if (vk_prog_resolve_record(&program, sh) != -1 || sh[5]) { puts("FAIL cold miss must skip"); return 1; }
    if (disabled || absent) {
        if (!run_dispatch(&program) || dispatches || vk_shader_xlate_epoch()) { puts("FAIL disabled translator changed guest draw/dispatch behavior"); return 1; }
        int saved = 0;
        for (unsigned attempt = 0; attempt < 10000 && !saved; attempt++) {
            DIR *dir = opendir(requests); struct dirent *entry;
            while (dir && (entry = readdir(dir))) {
                size_t length = strlen(entry->d_name); if (length < 5 || strcmp(entry->d_name + length - 5, ".json")) continue;
                char path[1200], json[16384]; snprintf(path, sizeof path, "%s/%s", requests, entry->d_name);
                FILE *file = fopen(path, "rb"); if (!file) continue;
                size_t n = fread(json, 1, sizeof json - 1, file); fclose(file); json[n] = 0;
                saved |= n && json[n - 1] == '\n' && strstr(json, "\"stage\":\"cs\"") && strstr(json, "\"code_hex\":\"");
            }
            if (dir) closedir(dir);
            if (!saved) usleep(1000);
        }
        if (!saved) { puts("FAIL missing shader request was not persisted"); return 1; }
        if (vk_seed_io_active()) { vk_seed_io_stats stats; vk_seed_io_stats_get(&stats); if (stats.inline_reads) return 1; }
        puts("disabled/absent translator retained PC request and skipped missing dispatch"); return 0;
    }
    /* Unchanged program/signature must stop returning its cached miss after publication. */
    while (!vk_shader_xlate_epoch() && host_now_ns() - start < 10000000000ull) usleep(1000);
    if (!vk_shader_xlate_epoch() || vk_prog_resolve_record(&program, sh) || !sh[5]) { puts("FAIL published variant did not replace negative caches"); return 1; }
    if (!run_dispatch(&program) || dispatches != 1) { puts("FAIL translated shader did not dispatch on Vulkan"); return 1; }
    uint64_t io = vk_shader_io_cum_ns();
    if (vk_prog_resolve_record(&program, sh) || vk_shader_io_cum_ns() != io) { puts("FAIL ready memo repeated seed I/O"); return 1; }
    printf("on-device protocol host acceptance passed: epoch=%llu miss-to-dispatch=%.3fms (real Vulkan)\n",
           (unsigned long long)vk_shader_xlate_epoch(), (double)(host_now_ns() - start) / 1e6);
    mtl_finish(); return 0;
}
