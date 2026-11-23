"""Known main-branch defects: execute production C slices, without game/device data.

Only the behavioral assertions are expected failures. Compilation, probe execution
and negative controls run outside expectedFailure, so broken fixtures fail normally.
Vulkan entry points below are deterministic lifetime/allocation observers, not a
GPU simulator or evidence of pixels, driver crashes, or shipped scene frequency.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/android/gfx'
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
RUNTIME = ROOT / 'games/totk/runtime'
CC = shlex.split(os.environ.get('CC', 'cc'))


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def function(source, name):
    match = re.search(r'^.*\b' + re.escape(name) + r'\([^;\n]*\)\s*\{', source, re.M)
    if match is None:
        raise AssertionError('missing production function ' + name)
    brace = source.index('{', match.start())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


RESOURCE_PREFIX = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include "vk_int.h"
#include "vk_record.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "frame_timeline.h"
#define MINV(a,b) ((a)<(b)?(a):(b))
#define MAXV(a,b) ((a)>(b)?(a):(b))
vk_dev V;
int vk_canvas_on, vk_rec_timing_on;
_Atomic int vk_lag_capture_state;
static uint64_t submission=1;
static int allocation_attempts, fail_allocation, destroyed_images;
static int live_views, destroy_with_view, resolve_calls;
static VkBuffer resolved_buffer;
static vk_treg registration;
static unsigned next_view=100;
uint64_t vk_submit_serial(void) { return submission; }
uint64_t mtl_last_fence(void) { return submission; }
int vk_rec_profile_push(int category) { return category; }
void vk_rec_profile_pop(int previous) { (void)previous; }
int vk_one_primary_on; /* production default path; sections are separately tested */
VkResult vk_sections_create_image(VkDevice d,const VkImageCreateInfo *i,const VkAllocationCallbacks *a,VkImage *o) {
    (void)d;(void)i;(void)a;(void)o;abort();
}
void vk_sections_destroy_image(VkDevice d,VkImage i,const VkAllocationCallbacks *a) { (void)d;(void)i;(void)a;abort(); }
VkImageAspectFlags vk_fmt_aspect(int format) { (void)format;return VK_IMAGE_ASPECT_COLOR_BIT; }
int vk_tex_memo_on(void) { return 0; }
int vk_lag_capture_init(void) { return 0; }
int nvn_dirty_tracked(const void *p,size_t n) { (void)p;(void)n;return 1; }
int nvn_dirty_since(const void *p,size_t n,uint64_t stamp) { (void)p;(void)n;(void)stamp;return 0; }
int vk_fmt(int format,VkFormat *out,VkComponentMapping *swz) {
    (void)format;(void)swz;*out=VK_FORMAT_R8G8B8A8_UNORM;return 1;
}
int nvn_fmt_bpb(int format,int *w,int *h) { (void)format;*w=*h=1;return 4; }
int vk_fmt_is_depth(int format) { (void)format;return 0; }
int vk_fmt_has_stencil(int format) { (void)format;return 0; }
static int mirshare_on(void) { return 1; }
static int compalias_on(void) { return 1; }
static int fmtsplit_on(void) { return 0; }
static int stor_compressed(const tex_t *t) { (void)t;return 0; }
static uint32_t fmt_class(int f) { return (uint32_t)f; }
static int ubwc_lists_on(void) { return 0; }
static VkFormat srgb_twin(VkFormat f) { (void)f;return VK_FORMAT_UNDEFINED; }
static VkFormatFeatureFlags fmt_features(VkFormat f) {
    (void)f;return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_image(VkDevice d,const VkImageCreateInfo *ci,
        const VkAllocationCallbacks *a,VkImage *image) {
    (void)d;(void)ci;(void)a;*image=(VkImage)(uintptr_t)(1000+allocation_attempts);return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_image(VkDevice d,VkImage i,const VkAllocationCallbacks *a) {
    (void)d;(void)i;(void)a;destroyed_images++;
}
static VKAPI_ATTR void VKAPI_CALL image_requirements(VkDevice d,VkImage i,VkMemoryRequirements *r) {
    (void)d;(void)i;*r=(VkMemoryRequirements){.size=4096,.alignment=256,.memoryTypeBits=1};
}
static VKAPI_ATTR VkResult VKAPI_CALL bind_image(VkDevice d,VkImage i,VkDeviceMemory m,VkDeviceSize o) {
    (void)d;(void)i;(void)m;(void)o;return VK_SUCCESS;
}
PFN_vkCreateImage vkCreateImage=create_image;
PFN_vkDestroyImage vkDestroyImage=destroy_image;
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements=image_requirements;
PFN_vkBindImageMemory vkBindImageMemory=bind_image;
int vk_mem_alloc(const VkMemoryRequirements *r,VkMemoryPropertyFlags flags,vk_alloc *out) {
    (void)r;(void)flags;allocation_attempts++;
    if (fail_allocation) { fail_allocation=0;return 0; }
    *out=(vk_alloc){.mem=(VkDeviceMemory)(uintptr_t)2000};return 1;
}
VkCommandBuffer vk_pre_cb(void) { return VK_NULL_HANDLE; }
static void image_barrier_init(VkCommandBuffer cb,VkImage image,VkImageAspectFlags aspect) {
    (void)cb;(void)image;(void)aspect;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice d,const VkBufferViewCreateInfo *ci,
        const VkAllocationCallbacks *a,VkBufferView *view) {
    (void)d;(void)a;assert(ci->buffer==resolved_buffer);live_views++;
    *view=(VkBufferView)(uintptr_t)++next_view;return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice d,VkBufferView v,const VkAllocationCallbacks *a) {
    (void)d;(void)v;(void)a;live_views--;
}
static VKAPI_ATTR void VKAPI_CALL destroy_buffer(VkDevice d,VkBuffer b,const VkAllocationCallbacks *a) {
    (void)d;(void)b;(void)a;if(live_views) destroy_with_view++;
}
PFN_vkCreateBufferView vkCreateBufferView=create_view;
PFN_vkDestroyBufferView vkDestroyBufferView=destroy_view;
PFN_vkDestroyBuffer vkDestroyBuffer=destroy_buffer;
int vk_treg_get(int id,vk_treg *out) { (void)id;*out=registration;return 1; }
int vk_resolve(uint64_t addr,uint64_t size,VkBuffer *buffer,uint64_t *offset) {
    (void)addr;(void)size;resolve_calls++;*buffer=resolved_buffer;*offset=0;return 1;
}
void vk_ub_touch(uint64_t a,uint64_t n) { (void)a;(void)n; }
void vk_gpu_write(uint64_t a,uint64_t n) { (void)a;(void)n; }
void vk_gpu_write_record(uint64_t a,uint64_t n) { (void)a;(void)n; }
int vk_lag_read(uint64_t a,uint64_t n,VkDeviceSize align,VkDescriptorBufferInfo *out,vk_lag_kind k) {
    (void)a;(void)n;(void)align;(void)out;(void)k;return 0;
}
int vk_lag_read_record(uint64_t a,uint64_t n,VkDeviceSize align,VkDescriptorBufferInfo *out,
        uint32_t *reason,vk_lag_kind k) {
    (void)reason;return vk_lag_read(a,n,align,out,k);
}
'''

RESOURCE_AFTER_TYPES = r'''
static pthread_mutex_t g_tex_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_stat_uploads,g_stat_reuploads,g_stat_images,g_stat_shared_hits,g_stat_switched;
static uint64_t g_stat_xfmt,g_stat_compsplit;
static nvn_hmap g_comp_owner={.klen=sizeof(stor_key)};
static void upload(vk_tex *x) { (void)x; }
static void texcopy_resource(const char *event,vk_tex *x) { (void)event;(void)x; }
static int retired_images;
static void retire_img(vk_img *im) { (void)im;retired_images++; }
static void retire_host(void *p) { (void)p; }
static void tex_epoch_bump(void) {}
static void native_index_remove(vk_img *im) { (void)im; }
'''

RESOURCE_MAIN = r'''
#define g_dev V.dev
static uint64_t g_wb_alias_maps,g_wb_alias_bytes;
static void mapping_forget(uintptr_t lo,uintptr_t hi) { (void)lo;(void)hi; }
void vk_memory_release(VkDeviceMemory mem,vk_memory_purpose purpose) { (void)mem;(void)purpose; }
/* These cases have no writeback work. B's wbref case invokes pool_gone alone;
 * the real pool retirement cancels writeback before calling it (report explains). */
void vk_wb_range_gone(uintptr_t lo,uintptr_t hi) { (void)lo;(void)hi; }
'''

RESOURCE_SCENARIOS = r'''
static tex_t texture(unsigned serial,int mirrored) {
    static uint8_t bytes[4096];static pool_t cpu={.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    tex_t t={.serial=serial,.mem=bytes,.pool=mirrored?&cpu:NULL,
        .d={.width=8,.height=8,.depth=1,.levels=1,.target=1,.format=0x25},.L={.size=4096}};
    return t;
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"pool-view") || !strcmp(argv[1],"arena-view")) {
        registration.t=texture(1,0);registration.t.d.target=10;registration.t.d.width=16;
        V.props.limits.maxTexelBufferElements=4096;V.props.limits.minTexelBufferOffsetAlignment=16;
        resolved_buffer=(VkBuffer)(uintptr_t)1;
        VkBufferView before=tbuf_by_id(1,0,NULL,NULL);assert(before && live_views==1);
        pool_ent *pool=calloc(1,sizeof *pool);pool->kind=K_OWN;pool->fd=-1;
        pool->base=(uintptr_t)registration.t.mem;pool->end=pool->base+4096;pool->buf=resolved_buffer;
        if(!strcmp(argv[1],"arena-view")) vk_res_buffer_released(pool->buf);
        unback_pool(pool);pool_retired(pool);
        resolved_buffer=(VkBuffer)(uintptr_t)2;
        VkBufferView after=tbuf_by_id(1,0,NULL,NULL);
        printf("{\"destroy_with_view\":%d,\"same_view\":%d,\"resolves\":%d}\n",
            destroy_with_view,before==after,resolve_calls);
    } else if(!strcmp(argv[1],"storage-ref") || !strcmp(argv[1],"storage-wbref") ||
              !strcmp(argv[1],"storage-unpinned")) {
        tex_t old=texture(1,0),fresh=old;fresh.serial=2;
        vk_tex *x=tex_get_inner(&old);assert(x && x->im && x->im->shared);
        vk_img *original=x->im;
        if(!strcmp(argv[1],"storage-wbref")) { original->refs=0;original->wbrefs=1; }
        if(!strcmp(argv[1],"storage-unpinned")) {
            nvn_hm_del(&g_by_serial,&old.serial);tex_drop_locked(x);
        }
        vk_tex_pool_gone((uintptr_t)old.mem,(uintptr_t)old.mem+old.L.size);
        vk_tex *nu=tex_get_inner(&fresh);assert(nu && nu->im);
        printf("{\"same_image\":%d,\"allocations\":%d,\"retired\":%d}\n",
            nu->im==original,allocation_attempts,retired_images);
    } else {
        int mirrored=!strcmp(argv[1],"retry-mirrored");
        tex_t t=texture(1,mirrored);fail_allocation=1;
        assert(!tex_get_inner(&t) && allocation_attempts==1 && destroyed_images==1);
        assert(nvn_hm_get(&g_by_serial,&t.serial)); /* failed serial was published */
        submission++;
        if(!strcmp(argv[1],"retry-recreated")) {
            vk_tex *failed=nvn_hm_del(&g_by_serial,&t.serial);tex_drop_locked(failed);t.serial++;
        }
        vk_tex *second=tex_get_inner(&t);
        printf("{\"available\":%d,\"allocations\":%d}\n",second!=NULL,allocation_attempts);
    }
    return 0;
}
'''


def resource_probe():
    resources = (GFX / 'vk_res.c').read_text()
    pools = (GFX / 'nvn_vk.c').read_text()
    code = RESOURCE_PREFIX
    code += section(resources, 'typedef struct tex_generation', 'static _Atomic uint64_t g_native_extent')
    code += RESOURCE_AFTER_TYPES
    code += section(resources, 'typedef struct { VkImage img; vk_alloc mem; view_ent *views;', '/* The game finalises textures')
    code += function(resources, 'retire_bview')
    for name in ('vk_res_buffer_released', 'stor_key_of', 'tex_mirrored', 'image_layers',
                 'img_create_ex', 'img_create', 'tex_drop_locked', 'tex_get_inner', 'vk_tex_get',
                 'vk_tex_pool_gone', 'tbuf_by_id'):
        code += function(resources, name)
    code += section(pools, 'enum { K_OWN = 1', 'typedef struct { uintptr_t va, end; pool_ent *owner;')
    code += RESOURCE_MAIN
    code += function(pools, 'unback_pool') + function(pools, 'pool_retired')
    return code + RESOURCE_SCENARIOS


def descriptor_probe():
    draw = (GFX / 'vk_draw.c').read_text()
    code = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vk_int.h"
#define MINV(a,b) ((a)<(b)?(a):(b))
'''
    code += section(draw, 'typedef struct { VkDescriptorBufferInfo b;', 'static int ubo_desc(')
    # All three consumers must use the same packed constructor.
    for call in ('dinfo_write(set,b,in,first,', 'dinfo_write(replay_set,b,in,element,',
                 'dinfo_write(VK_NULL_HANDLE,b,in,element,'):
        assert call in draw
    code += r'''
int main(void) {
    dinfo info[65]={0};
    for(unsigned e=0;e<65;e++) {
        info[e].b=(VkDescriptorBufferInfo){(VkBuffer)(uintptr_t)(10+e),20+e,30+e};
        info[e].i=(VkDescriptorImageInfo){(VkSampler)(uintptr_t)(40+e),
            (VkImageView)(uintptr_t)(50+e),VK_IMAGE_LAYOUT_GENERAL};
        info[e].t=(VkBufferView)(uintptr_t)(60+e);
    }
    unsigned counts[]={1,2,3,32,33,65};
    for(unsigned type=0;type<=7;type++) for(unsigned c=0;c<6;c++) {
        vk_bind binding={.binding=7,.count=counts[c],.dtype=type};
        for(unsigned first=0;first<binding.count;first+=DINFO_BATCH) {
            dinfo_scratch scratch;
            VkWriteDescriptorSet w=dinfo_write((VkDescriptorSet)(uintptr_t)1,&binding,info,first,&scratch);
            assert(w.dstBinding==7 && w.dstArrayElement==first);
            assert(w.descriptorCount==MINV(binding.count-first,DINFO_BATCH));
            for(unsigned e=0;e<w.descriptorCount;e++) {
                if(type==6 || type==7) assert(!memcmp(&w.pBufferInfo[e],&info[first+e].b,sizeof info[0].b));
                else if(type==4 || type==5) assert(w.pTexelBufferView[e]==info[first+e].t);
                else assert(!memcmp(&w.pImageInfo[e],&info[first+e].i,sizeof info[0].i));
            }
        }
    }
    puts("{\"immediate\":0,\"replay\":0,\"terminal\":0}");
    return 0;
}
'''
    return code


def pacing_probe():
    producer = (NVN / 'hle_nvn.c').read_text()
    frontend = (RUNTIME / 'frontend_android.c').read_text()
    stubs = (GFX / 'nvn_stubs.c').read_text()
    code = r'''
#define _GNU_SOURCE
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vk_present_slots.h"
#define LOG(...) do { if (0) fprintf(stderr, __VA_ARGS__); } while (0)
enum { MFX_OFF,MFX_HALF,MFX_30,MFX_MAX };
#define UNCAPPED_HZ 120
typedef struct { int interval; } win_t;
static int g_uncapped,g_paced_hz,g_hz=-1,g_display_mode;
static unsigned g_fps_auto_ticks;
static int display_pace_on,present_slots_on=1,acquire_clock_on_flag,display_pace_ready=1;
static pthread_mutex_t display_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t display_period_ns;
static struct { uint64_t frame_ns; } display_clock={.frame_ns=1000000000};
static int mfx_enabled(void) { return MFX_OFF; }
static int dm_detclock_enabled(void) { return 0; }
static int acquire_clock_on(void) { return acquire_clock_on_flag; }
static int runtime_fps_adaptive_selected(void) { return 0; }
static int runtime_android_fps_auto_active(void) { return 0; }
static int runtime_fps_hybrid_selected(void) { return 0; }
static int runtime_android_display_pace_active(void) { return display_pace_on; }
static int runtime_android_display_hz(void) { return (int)((1000000000+display_period_ns/2)/display_period_ns); }
static unsigned (*runtime_cutscene_present_ticks)(unsigned,uint64_t);
static int uncapped_interval_on(void) { const char *e=getenv("TOTK_UNCAPPED_INTERVAL");return e && atoi(e); }
static void env_default(const char *name,const char *format,const char *value) {
    assert(!strcmp(format,"%s"));assert(!setenv(name,value,0));
}
static void defaults(void) {
'''
    code += section(frontend, '    env_default("TOTK_FPS60",', '    if (!strcmp(getenv("TOTK_ACQUIRE_SLACK"), "2"))')
    code += '}\n'
    # The frontend's state variable and the producer's predicate share a name in separate files.
    period = function(frontend, 'runtime_android_display_period')
    if '!acquire_clock_on)' not in period:
        raise AssertionError('runtime_android_display_period no longer reads acquire_clock_on as expected')
    code += period.replace('!acquire_clock_on)', '!acquire_clock_on_flag)')
    code += 'static uint64_t runtime_android_acquire_period(unsigned interval) { return acquire_clock_on_flag ? runtime_android_display_period(interval) : 0; }\n'
    code += section(stubs, 'int mtl_display_hz(void) { return runtime_android_display_pace_active()', 'int mtl_link_wait_tick(')
    code += section(producer, 'static int uncapped_now(void)', '/* 30+30 on a display')
    code += section(producer, 'static int mfx_own_clock(void)', '/* Opt-in until live scene/clock')
    code += section(producer, 'static int uncapped_cap(', 'void nvn_present_config(')
    code += section(producer, 'static void present_cfg(', '/* Exercise production policy,')
    code += r'''
int main(int argc,char **argv) {
    assert(argc==2);defaults();win_t w={.interval=2};
    display_period_ns=8333333;
    if(!strcmp(argv[1],"display")) display_pace_on=1;
    if(!strcmp(argv[1],"legacy")) present_slots_on=0;
    if(!strcmp(argv[1],"acquire")) acquire_clock_on_flag=1;
    if(!strcmp(argv[1],"40") || !strcmp(argv[1],"unlimited")) {
        assert(!setenv("TOTK_FPS60","1",1) && !setenv("TOTK_UNCAPPED","1",1));
        assert(!setenv("TOTK_UNCAPPED_HZ",!strcmp(argv[1],"40")?"40":"120",1));
        assert(!setenv("TOTK_UNCAPPED_INTERVAL","1",1));present_slots_on=0;
    }
    if(!strcmp(argv[1],"60hz")) display_period_ns=16666667;
    uint64_t period=present_period(&w);
    printf("{\"period_ns\":%llu,\"guest_ticks\":2,\"effective_ticks\":%u,\"slots\":%u}\n",
        (unsigned long long)period,present_ticks(&w),vk_present_slots_interval(period/1e9,display_period_ns));
    return 0;
}
'''
    return code


@unittest.skipUnless(CC and shutil.which(CC[0]), 'needs a host C compiler')
class MainDefectReproductions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='main-defects-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.probes = {}
        flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for arg in shlex.split(os.environ.get(name, ''))]
        for name, source in (('resources', resource_probe()), ('descriptors', descriptor_probe()),
                             ('pacing', pacing_probe())):
            path = Path(cls.temp.name) / (name + '.c')
            executable = path.with_suffix('')
            path.write_text(source)
            result = subprocess.run([*CC, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-variable', '-Wno-missing-field-initializers',
                '-Wno-misleading-indentation', '-I' + str(GFX), '-I' + str(NVN),
                '-I' + str(NVN / 'include'), '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/host'),
                '-I' + str(RUNTIME),
                '-I' + str(ROOT / 'games/totk/android/compat/include'),
                '-include', str(ROOT / 'games/totk/android/compat/include/darwin_compat.h'),
                str(path), '-pthread', '-lm', *flags, '-o', str(executable)],
                capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.probes[name] = executable
        # Probe execution and JSON shape must succeed before any expected failure.
        cls.observed = {}
        for probe, scenarios in (
                ('resources', ('pool-view', 'arena-view', 'storage-ref', 'storage-wbref',
                               'storage-unpinned', 'retry-shared', 'retry-mirrored', 'retry-recreated')),
                ('descriptors', ('',)),
                ('pacing', ('default', '60hz', 'legacy', 'display', 'acquire', '40', 'unlimited'))):
            for scenario in scenarios:
                env = {k: v for k, v in os.environ.items()
                       if not k.startswith(('TOTK_', 'DM_PRESENT_', 'DM_DETCLOCK'))}
                command = [str(cls.probes[probe])] + ([scenario] if scenario else [])
                result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
                if result.returncode:
                    raise AssertionError(f'{probe}/{scenario}: {result.stdout}{result.stderr}')
                cls.observed[scenario or probe] = json.loads(result.stdout)

    @unittest.expectedFailure
    def test_a_pool_views_retire_before_buffer_destruction(self):
        self.assertEqual(self.observed['pool-view'],
                         {'destroy_with_view': 0, 'same_view': 0, 'resolves': 2})

    def test_a_arena_release_control_invalidates_and_rebuilds(self):
        self.assertEqual(self.observed['arena-view'],
                         {'destroy_with_view': 0, 'same_view': 0, 'resolves': 2})

    @unittest.expectedFailure
    def test_b_finalized_storage_with_texture_ref_has_new_identity(self):
        self.assertEqual(self.observed['storage-ref']['same_image'], 0)

    @unittest.expectedFailure
    def test_b_pool_gone_detaches_writeback_pinned_identity(self):
        self.assertEqual(self.observed['storage-wbref']['same_image'], 0)

    def test_b_unpinned_storage_control_gets_new_identity(self):
        self.assertEqual(self.observed['storage-unpinned'],
                         {'same_image': 0, 'allocations': 2, 'retired': 1})

    def test_c_array_writes_use_contiguous_vulkan_elements(self):
        self.assertEqual(self.observed['descriptors'], {'immediate': 0, 'replay': 0, 'terminal': 0})

    @unittest.expectedFailure
    def test_d_shared_texture_retries_transient_allocation_failure(self):
        self.assertEqual(self.observed['retry-shared'], {'available': 1, 'allocations': 2})

    @unittest.expectedFailure
    def test_d_mirrored_texture_retries_transient_allocation_failure(self):
        self.assertEqual(self.observed['retry-mirrored'], {'available': 1, 'allocations': 2})

    def test_d_new_serial_control_can_allocate_after_failure(self):
        self.assertEqual(self.observed['retry-recreated'], {'available': 1, 'allocations': 2})

    @unittest.expectedFailure
    def test_e_default_shipping30_retains_guest60_units_on_120hz(self):
        self.assertEqual(self.observed['default']['slots'], 4)
        self.assertAlmostEqual(self.observed['default']['period_ns'], 1000000000 / 30, delta=4)

    def test_e_sixty_hz_and_opted_out_slots_preserve_thirty(self):
        self.assertAlmostEqual(self.observed['60hz']['period_ns'], 1000000000 / 30, delta=4)
        self.assertAlmostEqual(self.observed['legacy']['period_ns'], 1000000000 / 30, delta=4)

    def test_e_explicit_physical_clocks_follow_documented_refresh_units(self):
        for mode in ('display', 'acquire'):
            self.assertEqual(self.observed[mode]['period_ns'], 16666666)
            self.assertEqual(self.observed[mode]['effective_ticks'], 2)

    def test_e_explicit_40_and_unlimited_caps_are_panel_independent(self):
        self.assertEqual(self.observed['40']['period_ns'], 25000000)
        self.assertEqual(self.observed['40']['slots'], 3)
        self.assertEqual(self.observed['unlimited']['period_ns'], 8333333)


if __name__ == '__main__':
    unittest.main()
