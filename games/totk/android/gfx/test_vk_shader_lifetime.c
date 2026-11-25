/* No Vulkan device: production shader/program caches and compact dictionary,
 * with only driver calls and unrelated renderer classification replaced. */
/* Count actual production classification calls, including the disabled arm. */
#define vk_spirv_io vk_spirv_io_impl
#include "vk_varyings.h"
#undef vk_spirv_io
#include <stdatomic.h>
static _Atomic unsigned io_walks;
static int vk_spirv_io(const uint32_t *w, size_t n, uint32_t storage, uint32_t *mask, uint8_t kind[32]) {
    atomic_fetch_add(&io_walks, 1);
    return vk_spirv_io_impl(w, n, storage, mask, kind);
}
#include "vk_shader.c"
#include "vk_shader_xlate.c"
#include "vk_gpuprof_compact.h"
#include <assert.h>

vk_dev V;
vk_state S;
int g_hitch_on;
vk_hitch g_hk;
static _Atomic unsigned modules;
static unsigned destroyed, layouts, compute_prepares;
static int fail_module, fail_layout;
static const uint32_t *expected_words;
static size_t expected_count;
static int inspect_modules;
/* Test hook inside the mocked driver: hold the real replacement helper in
   vkCreateShaderModule until the reporting/probe checks have completed. */
static pthread_mutex_t hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cv = PTHREAD_COND_INITIALIZER;
static int hold_armed, hold_entered, hold_release, report_done;
static uint64_t report_ns;

static VKAPI_ATTR VkResult VKAPI_CALL create_module(VkDevice device,
        const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *allocator, VkShaderModule *out) {
    (void)device; (void)allocator;
    assert(info->codeSize >= 20 && info->pCode[0] == 0x07230203);
    if (fail_module) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_lock(&hold_mu);
    if (hold_armed) {
        hold_armed = 0; hold_entered = 1; pthread_cond_broadcast(&hold_cv);
        while (!hold_release) pthread_cond_wait(&hold_cv, &hold_mu);
    }
    pthread_mutex_unlock(&hold_mu);
    unsigned id = atomic_fetch_add(&modules, 1) + 1;
    if (inspect_modules) {
        unsigned integer = 0, nan = 0, invariant = 0;
        for (size_t i = 5; i < info->codeSize / 4; i += info->pCode[i] >> 16) {
            const uint32_t *p = info->pCode + i; unsigned op = *p & 0xffff;
            if ((op == 128 || op == 83) && p[2] == 15) { assert(op == (g_int_fold ? 83u : 128u)); integer++; }
            if (op == 12 && p[2] >= 16 && p[2] <= 18) {
                const uint32_t translated[] = {37, 40, 43}, rewritten[] = {79, 80, 81};
                assert(p[4] == (g_nan_minmax ? rewritten : translated)[p[2] - 16]); nan++;
            }
            if (op == 71 && p[1] == 10 && p[2] == 18) invariant++;
        }
        assert(integer == 1 && nan == 3 && invariant == (g_position_policy & 1));
        if (!g_varying_fix && !g_int_fold && !g_nan_minmax && !g_position_policy)
            assert(info->codeSize == expected_count * 4 && !memcmp(info->pCode, expected_words, info->codeSize));
        char path[80]; snprintf(path, sizeof path, "module-%u.spv", id - 1);
        FILE *f = fopen(path, "wb"); assert(f);
        assert(fwrite(info->pCode, 1, info->codeSize, f) == info->codeSize); assert(!fclose(f));
    }
    *out = (VkShaderModule)(uintptr_t)id;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_module(VkDevice device, VkShaderModule module,
        const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    assert(module); destroyed++;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice device,
        const VkDescriptorSetLayoutCreateInfo *info, const VkAllocationCallbacks *allocator,
        VkDescriptorSetLayout *out) {
    (void)device; (void)allocator;
    assert(!info->bindingCount);
    if (fail_layout) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = (VkDescriptorSetLayout)(uintptr_t)++layouts;
    return VK_SUCCESS;
}
PFN_vkCreateShaderModule vkCreateShaderModule = create_module;
PFN_vkDestroyShaderModule vkDestroyShaderModule = destroy_module;
PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout = create_layout;

int vk_off(const char *name) { (void)name; return 0; }
int vk_unwritten_mask_on(void) { return 0; }
int vk_native_ui_enabled(void) { return 0; }
int vk_canvas_ui_shader(const char *key, int after) { (void)key; (void)after; return 0; }
int vk_canvas_transfer_shader(const char *key) { (void)key; return 0; }
unsigned vk_fg_camera_layout(const char *key) { (void)key; return 0; }
int vk_fg_raster_depth_shader(const char *key) { (void)key; return 0; }
int vk_fg_plane_shader(const char *key, unsigned stage) { (void)key; (void)stage; return 0; }
int vk_treg_kind(int tid, int *format, int *target) { (void)tid; (void)format; (void)target; return 0; }
uint64_t vk_treg_revision(void) { return 1; }
int vk_fmt_class(int format) { (void)format; return 0; }
int vk_desc_layout_fits(const VkDescriptorSetLayoutBinding *bindings, uint32_t count,
        char *why, size_t size) {
    (void)bindings; (void)why; (void)size;
    assert(!count); return 1;
}
void vk_compute_prepare(vk_shader *shader) {
    shader_key key = {0}; memcpy(key.variant, shader->key, sizeof key.variant);
    key.early_tests = shader->early_tests | shader->alpha_func << 1;
    key.position_policy = g_position_policy;
    assert(shader->stage == 5 && shader->mod && shader->dsl);
    assert(nvn_hm_get(&g_shaders, &key) == shader); /* publish before handing to a worker */
    compute_prepares++;
}

void vk_compute_report(void) {}

/* A small empty entry point is enough for the real loader's parsing/patching.
 * The mock driver deliberately does not validate rendering semantics. */
static void seed_variant(const char *name, int stage) {
    const uint32_t model = stage == 0 ? 0 : stage == 1 ? 4 : 5;
    const uint32_t head[] = {0x07230203, 0x00010300, 0, 5, 0,
        2u << 16 | 17, 1, 3u << 16 | 14, 0, 1,
        5u << 16 | 15, model, 3, 0x6e69616d, 0};
    const uint32_t fragment[] = {3u << 16 | 16, 3, 7};
    const uint32_t compute[] = {6u << 16 | 16, 3, 17, 1, 1, 1};
    const uint32_t body[] = {2u << 16 | 19, 1, 3u << 16 | 33, 2, 1,
        5u << 16 | 54, 1, 3, 0, 2, 2u << 16 | 248, 4, 1u << 16 | 253, 1u << 16 | 56};
    char path[1100]; snprintf(path, sizeof path, "%s/%s.spv", seed_dir(), name);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(head, 1, sizeof head, file) == sizeof head);
    if (stage == 1) assert(fwrite(fragment, 1, sizeof fragment, file) == sizeof fragment);
    if (stage == 5) assert(fwrite(compute, 1, sizeof compute, file) == sizeof compute);
    assert(fwrite(body, 1, sizeof body, file) == sizeof body); assert(!fclose(file));
    snprintf(path, sizeof path, "%s/%s.vk", seed_dir(), name);
    file = fopen(path, "w"); assert(file);
    assert(fprintf(file, "stage %d\nc1 01234567\n", stage) > 0); assert(!fclose(file));
}
static void variant_list_write(const char *key, const char *name) {
    char path[1100]; snprintf(path, sizeof path, "%s/%.64s.vv", seed_dir(), key);
    FILE *file = fopen(path, "w"); assert(file);
    assert(fprintf(file, "%s\t\n", name) > 0); assert(!fclose(file));
}
static gp_compact compact;

static vk_shader *vary_vs;
static vk_shader vary_fs[64];
static void *vary_worker(void *arg) {
    size_t start = (size_t)arg;
    for (size_t i = start; i < 64; i += 4) {
        VkShaderModule m = vk_vs_module_for(vary_vs, vary_fs + i, 1);
        assert(m && (m != vary_vs->mod) == g_varying_fix);
        assert(vk_vs_module_for(vary_vs, vary_fs + i, 1) == m);
    }
    return NULL;
}
static void *held_create(void *arg) {
    assert(vk_vs_module_for(vary_vs, arg, 1) != vary_vs->mod);
    return NULL;
}
static void *report_during_create(void *arg) {
    const vk_shader *pending = arg;
    uint64_t began = host_now_ns(), n, ns, max;
    vk_shader_io_take(&n, &ns, &max); /* the actual 300-present statistics entry */
    assert(vk_vs_module_for(vary_vs, pending, 0) == VK_NULL_HANDLE);
    static vk_shader cold = {.io_known=1, .fin_mask=1u << 30};
    assert(vk_vs_module_for(vary_vs, &cold, 0) == VK_NULL_HANDLE);
    /* Probe lock contention is another deferral, never a wait for publication. */
    pthread_mutex_lock(&g_vfix_mu);
    assert(vk_vs_module_for(vary_vs, pending, 0) == VK_NULL_HANDLE);
    pthread_mutex_unlock(&g_vfix_mu);
    report_ns = host_now_ns() - began;
    /* An unrelated interface can finish while the first driver's call is held. */
    assert(vk_vs_module_for(vary_vs, &cold, 1) != vary_vs->mod);
    pthread_mutex_lock(&hold_mu);
    report_done = 1; pthread_cond_broadcast(&hold_cv);
    pthread_mutex_unlock(&hold_mu);
    return NULL;
}
static void check_nonblocking_report(void) {
    if (!g_varying_fix) return;
    static vk_shader pending = {.io_known=1, .fin_mask=1u << 29};
    assert(vk_vs_module_for(vary_vs, &pending, 0) == VK_NULL_HANDLE);
    uint64_t reads = atomic_load(&g_io_n);
    unsigned count = atomic_load(&modules);
    pthread_mutex_lock(&hold_mu); hold_armed = 1; pthread_mutex_unlock(&hold_mu);
    pthread_t creator, reporter;
    assert(!pthread_create(&creator, NULL, held_create, &pending));
    pthread_mutex_lock(&hold_mu);
    while (!hold_entered) pthread_cond_wait(&hold_cv, &hold_mu);
    pthread_mutex_unlock(&hold_mu);
    /* The reporter must finish before we let the held driver return. */
    assert(!pthread_create(&reporter, NULL, report_during_create, &pending));
    struct timespec deadline; assert(!clock_gettime(CLOCK_REALTIME, &deadline)); deadline.tv_sec += 2;
    pthread_mutex_lock(&hold_mu);
    int status = 0;
    while (!report_done && !status) status = pthread_cond_timedwait(&hold_cv, &hold_mu, &deadline);
    int returned_while_held = report_done;
    hold_release = 1; pthread_cond_broadcast(&hold_cv);
    pthread_mutex_unlock(&hold_mu);
    assert(!pthread_join(creator, NULL) && !pthread_join(reporter, NULL));
    assert(returned_while_held); /* never release creation just to unblock stats */
    assert(report_ns < 250000000); /* generous host scheduling guard, not a device budget */
    assert(atomic_load(&modules) == count + 2);
    assert(atomic_load(&g_io_n) <= reads + 2); /* only actual builds read seed words */
    assert(vk_vs_module_for(vary_vs, &pending, 0) != VK_NULL_HANDLE);
    printf("held creation: statistics/probes returned in %.3f ms before release\n", report_ns / 1e6);
}

static int varying_test(void) {
#define OP(n, op) ((uint32_t)(n) << 16 | (op))
    /* Dataless valid VS: the integer result and FMin let the mocked driver check
       that every replacement, not merely its source, ran the complete chain. */
    static const uint32_t words[] = {0x07230203, 0x00010300, 0, 32, 0,
        OP(2,17), 1, OP(6,11), 20, 0x4c534c47, 0x6474732e, 0x3035342e, 0,
        OP(3,14), 0, 1, OP(6,15), 0, 3, 0x6e69616d, 0, 10, OP(4,71), 10, 11, 0,
        OP(2,19), 1, OP(3,33), 2, 1, OP(3,22), 5, 32, OP(4,23), 6, 5, 4,
        OP(4,32), 7, 3, 6, OP(4,21), 8, 32, 0,
        OP(4,43), 5, 9, 0, OP(4,43), 5, 11, 0x3f800000,
        OP(4,43), 8, 12, 0, OP(4,43), 8, 13, 1, OP(7,44), 6, 14, 9, 9, 9, 11,
        OP(4,59), 7, 10, 3, OP(5,54), 1, 3, 0, 2, OP(2,248), 4,
        OP(5,128), 8, 15, 12, 13, OP(7,12), 5, 16, 20, 37, 9, 11,
        OP(7,12), 5, 17, 20, 40, 9, 11, OP(8,12), 5, 18, 20, 43, 9, 9, 11,
        OP(3,62), 10, 14, OP(1,253), OP(1,56)};
#undef OP
    assert(!setenv("TOTK_XLATE_ONDEVICE", "0", 1));
    assert(!mkdir("shaders", 0700));
    char key[65]; memset(key, 'a', 64); key[64] = 0;
    seed_variant(key, 0);
    char path[1100]; snprintf(path, sizeof path, "shaders/%s.spv", key);
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(words, 1, sizeof words, file) == sizeof words); assert(!fclose(file));
    char game[4096]; assert(getcwd(game, sizeof game)); vk_shader_seed_startup(game);
    expected_words = words; expected_count = sizeof words / 4; inspect_modules = 1;
    vary_vs = vk_shader_seed(key, 0, 0); assert(vary_vs);
    assert(atomic_load(&io_walks) == (unsigned)g_varying_fix);
    for (size_t i = 0; i < 64; i++) {
        vary_fs[i].io_known = 1;
        vary_fs[i].fin_mask = 1u << (i % 20 + 1);
        vary_fs[i].fin_kind[i % 20 + 1] = i % 3;
    }
    pthread_t worker[4];
    for (size_t i = 0; i < 4; i++) assert(!pthread_create(worker + i, NULL, vary_worker, (void *)i));
    /* Recorder accounting/reset runs concurrently with actual worker-side reads.
       All per-window read counts remain accounted for across resets. */
    uint64_t accounted = 0;
    for (unsigned i = 0; i < 128; i++) {
        size_t length = 0; char *bytes = read_file(path, &length); assert(bytes && length == sizeof words); free(bytes);
        uint64_t n, ns, max; vk_shader_io_take(&n, &ns, &max); accounted += n;
    }
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(worker[i], NULL));
    uint64_t n, ns, max; vk_shader_io_take(&n, &ns, &max); accounted += n;
    assert(accounted == 130 + (g_varying_fix ? 60 : 0)); /* seed spv+reflection, recorder reads, 60 mask/type sets */
    assert(g_vfix_patched == (g_varying_fix ? 64u : 0u) && !g_vfix_fallback);
    assert(atomic_load(&io_walks) == (unsigned)g_varying_fix); /* helper does not reclassify */
    if (g_varying_fix) {
        static vk_shader failure = {.io_known=1, .fin_mask=1u << 31};
        fail_module = 1; assert(vk_vs_module_for(vary_vs, &failure, 1) == vary_vs->mod);
        fail_module = 0; assert(vk_vs_module_for(vary_vs, &failure, 1) == vary_vs->mod);
        assert(g_vfix_patched == 64 && g_vfix_fallback == 1);
        varying_report();
    } else assert(!vary_vs->vfix && modules == 1);
    if (getenv("TEST_VARYING_HOLD")) check_nonblocking_report();
    vk_seed_io_stop();
    puts("varying runtime chain, off-arm identity, accounting and concurrency passed");
    return 0;
}

int main(void) {
    if (getenv("TEST_VARYINGS")) return varying_test();
    /* Freeze precision to the ordinary arm before the production pthread_once. */
    assert(!setenv("TOTK_VK_POS_INVARIANT", "0", 1));
    assert(!setenv("TOTK_VK_POS_NOCONTRACT", "0", 1));
    assert(!setenv("TOTK_XLATE_ONDEVICE", "0", 1));
    assert(!mkdir("shaders", 0700));
    uint8_t control[0x880] = {0}, code[64] = {0};
    uint32_t length = sizeof code; memcpy(control + 0x704, &length, sizeof length);
    prog_t program = {.serial = 71, .nstage = 1};
    program.sd[0].control = control; program.sd[0].data = (uintptr_t)code;
    vk_prog *pr = prog_of(&program); assert(pr && pr->has[0]);
    char first[80], later[80], fragment[65], compute[65], failure[65];
    snprintf(first, sizeof first, "%.64s-000000000001", pr->key[0]);
    snprintf(later, sizeof later, "%.64s-000000000002", pr->key[0]);
    memset(fragment, 'b', 64); fragment[64] = 0;
    memset(compute, 'c', 64); compute[64] = 0;
    memset(failure, 'd', 64); failure[64] = 0;
    seed_variant(first, 0); seed_variant(fragment, 1); seed_variant(compute, 5); seed_variant(failure, 0);
    variant_list_write(pr->key[0], first);
    char game[4096]; assert(getcwd(game, sizeof game)); vk_shader_seed_startup(game);
    int indexed = vk_seed_io_active();
    const char *io = getenv("TOTK_VK_SEED_IO");
    assert(indexed == (!io || atoi(io) != 0));
    vk_shader *bound[6]; assert(!vk_prog_resolve(&program, bound) && bound[0]);
    vk_shader *vs = bound[0], *fs = vk_shader_seed(fragment, 1, 0);
    vk_shader *early = vk_shader_seed(fragment, 1, 1), *cs = vk_shader_seed(compute, 5, 0);
    assert(fs && early && early != fs && cs && compute_prepares == 1);
    variant_list *list = variants_of(pr->key[0]); assert(list && list->n == 1);
    assert(vs->key != list->v[0].name && !strcmp(vs->key, list->v[0].name));
    gp_compact_key pending = {.pipeline = 0x100, .shader = {vs->key, fs->key, NULL}};
    gp_compact_key pending_compute = {.pipeline = 0x200, .compute = 1, .shader = {NULL, NULL, cs->key}};
    compact.start = 1;
    gp_compact_entry *original = gp_compact_get(&compact, &pending); assert(original);
    uint64_t original_id = original->id;
    unsigned capacity = g_shaders.cap, published = g_shaders.n;

    /* Failure cleanup must never publish a partial object or affect earlier names. */
    fail_module = 1; assert(!vk_shader_seed(failure, 0, 0));
    assert(g_shaders.n == published && !destroyed);
    fail_module = 0; fail_layout = 1; assert(!vk_shader_seed(failure, 0, 0));
    assert(g_shaders.n == published && destroyed == 1);
    fail_layout = 0; assert(vk_shader_seed(failure, 0, 0));
    assert(!vk_shader_alpha(fs, NVN_ALPHA_FUNC_LESS)); /* empty fragment has no colour-zero output */
    assert(g_shaders.n == published + 1);

    /* Model a successful service reply using its actual index-before-epoch protocol.
     * Refresh frees the old variant-list strings, not the published vk_shader. */
    seed_variant(later, 0); variant_list_write(pr->key[0], later);
    vk_seed_io_published(later);
    atomic_fetch_add_explicit(&xlate_epoch, 1, memory_order_release);
    list = variants_of(pr->key[0]); assert(list->n == 1 && !strcmp(list->v[0].name, later));
    assert(!vk_prog_resolve(&program, bound) && bound[0] == vs); /* ready variants survive an epoch */
    vk_shader *new_vs = vk_shader_seed(list->v[0].name, 0, 0); assert(new_vs && new_vs != vs);
    for (unsigned i = 0; i < 192; i++) {
        char name[80]; snprintf(name, sizeof name, "%.64s-%012x", pr->key[0], i + 16);
        seed_variant(name, 0); vk_seed_io_published(name);
        vk_shader *loaded = vk_shader_seed(name, 0, 0);
        assert(loaded && !strcmp(loaded->key, name));
        memset(name, 'x', sizeof name); /* caller scratch never owns the cached name */
    }
    assert(g_shaders.cap > capacity);
    mtl_prog_release(&program);
    assert(!nvn_hm_get(&g_progs, &program.serial));
    vk_seed_io_stop(); assert(!vk_seed_io_active()); /* frees index/warmed file buffers */

    /* A delayed lease/dictionary consumer still sees exact names after all churn. */
    assert(pending.shader[0] == vs->key && !strcmp(pending.shader[0], first));
    assert(!strcmp(pending.shader[1], fragment) && !strcmp(pending_compute.shader[2], compute));
    assert(vk_shader_seed(first, 0, 0) == vs && vk_shader_seed(compute, 5, 0) == cs);
    assert(compute_prepares == 1);
    assert(gp_compact_get(&compact, &pending)->id == original_id);
    gp_compact_key same_text = pending; same_text.shader[1] = early->key;
    assert(early->key != fs->key && !strcmp(early->key, fs->key));
    assert(gp_compact_get(&compact, &same_text)->id == original_id);
    gp_compact_key reused_handle = pending; reused_handle.shader[0] = new_vs->key;
    gp_compact_entry *replacement = gp_compact_get(&compact, &reused_handle);
    assert(replacement && replacement->id != original_id);
    assert(!strcmp(original->shader[0], first) && !strcmp(replacement->shader[0], later));
    gp_compact_entry *compute_entry = gp_compact_get(&compact, &pending_compute);
    assert(compute_entry && !strcmp(compute_entry->shader[2], compute));
    assert(destroyed == 1); /* only the failed, unpublished layout's module was destroyed */
    puts("borrowed shader lifetime passed");
    return 0;
}
