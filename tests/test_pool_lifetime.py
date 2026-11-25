"""Keep the abandoned A/B lifetime scenarios unchanged against main bad3012.

Adapted from the prior branch's test_main_defects/test_pool_lifetime scenarios.
Both source versions run the same production-slice fixture and real shared-page
MapVirtual syscalls. Vulkan allocation, uploads, dirty tracking and waits remain
observers; this is not evidence of submitted shader sampling or device pixels.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

import test_main_defects as fixture

BASE = 'bad30123ed8f797118211367ed3d0533b633ec5b'
GFX_PATH = 'games/totk/android/gfx/'
VPOOL_PATH = 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_vpool.c'

EXTRA = r'''
#include "totk_vpool.h"
#define LOG LOGV
#define die DIE
#define F_VIRTUAL NVN_MEMORY_POOL_FLAGS_VIRTUAL
#define F_PHYSICAL NVN_MEMORY_POOL_FLAGS_PHYSICAL
#define POOL_MAGIC 0x504f4f4c
static pthread_mutex_t g_map_mu=PTHREAD_MUTEX_INITIALIZER;
static void vmap_note(uintptr_t va,uint64_t n,uintptr_t cpu) { (void)va;(void)n;(void)cpu; }
int mtl_vpool_replace(void *dst,const void *src,uint64_t n) { (void)dst;(void)src;(void)n;return 0; }
int mtl_pool_alias(void *dst,const void *src,uint64_t n) { (void)dst;(void)src;(void)n;return 0; }
static int (*replace_hook)(void *,const void *,uint64_t)=mtl_vpool_replace;
static int (*alias_hook)(void *,const void *,uint64_t)=mtl_pool_alias;
static void (*del_hook)(const void *)=mtl_vpool_del;
static void (*add_hook)(const void *,uint64_t)=mtl_vpool_add;
#define mtl_vpool_replace replace_hook
#define mtl_pool_alias alias_hook
#define mtl_vpool_del del_hook
#define mtl_vpool_add add_hook
static uint64_t g_stat_published;
static int writeback_notes;
static void gpu_write_range_note(const vk_tex *x) { (void)x; }
static int wb_tile_match(const tex_t *t) { (void)t;return 0; }
static void wb_note_locked(vk_tex *x) { (void)x;writeback_notes++; }
'''

SCENARIOS = r'''
static uint8_t *pages(size_t n) {
    void *p=mmap(NULL,n,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(p!=MAP_FAILED);memset(p,0x23,n);return p;
}
static tex_t tex(pool_t *p,uint64_t serial,uint64_t off) {
    return (tex_t){.pool=p,.mem=p->mem+off,.serial=serial,.has_layout=1,
        .d={.width=8,.height=8,.depth=1,.levels=1,.target=1,.format=0x25},.L={.size=4096}};
}
static void release_texture(tex_t *t) { tex_drop_locked(nvn_hm_del(&g_by_serial,&t->serial)); }
static void sample(tex_t *t,vk_tex *x,vk_img *im,tex_stamp stamp) {
    int before=upload_calls;*t->mem=0x67;dirty_written=1;submission++;
    assert(tex_get_inner(t)==x && x->im==im);
    printf("\"same_image\":1,\"stamp_current\":%d,\"uploads\":%d,\"byte\":%d,",
        tex_stamp_current(stamp),upload_calls-before,last_uploaded_byte);
    tex_mark_gpu_written_inner(x);
    printf("\"published\":%d,\"writeback_notes\":%d,\"cancellations\":%d}",
        im->shared && nvn_hm_get(&g_by_storage,&im->k)==im,writeback_notes,wb_cancel_calls);
}
static void nested(int parent_first,int replace) {
    pool_t p={.mem=pages(3*TOTK_VPOOL_PAGE),.size=3*TOTK_VPOOL_PAGE,
        .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    pool_t c={.mem=p.mem+TOTK_VPOOL_PAGE,.size=TOTK_VPOOL_PAGE,
        .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED};
    mtl_pool_add(p.mem,p.size);mtl_pool_add(c.mem,c.size);assert(g_pools[1]->kind==K_VIEW);
    tex_t t=tex(parent_first?&c:&p,1,parent_first?0:TOTK_VPOOL_PAGE);
    vk_tex *x=tex_get_inner(&t);vk_img *im=x->im;tex_stamp stamp=x->identity;
    VkBufferView view=VK_NULL_HANDLE;
    if(!parent_first) {
        resolved_buffer=g_pools[0]->buf;registration.t=t;registration.t.serial=2;
        registration.t.d.target=10;registration.t.d.width=16;
        V.props.limits.maxTexelBufferElements=4096;V.props.limits.minTexelBufferOffsetAlignment=16;
        view=tbuf_by_id(1,0,NULL,NULL);assert(view);
        registration.t.serial=3;assert(tbuf_by_id(1,0,NULL,NULL));release_texture(&registration.t);
    }
    if(parent_first) {
        mtl_pool_del(p.mem);assert(g_npool==1 && g_pools[0]->kind==K_CPU && !g_pools[0]->owner);
        if(replace) {
            backing_failure=replace==2;mtl_pool_add(p.mem,p.size);
            assert(g_pools[1]->kind==(replace==2?K_CPU:K_OWN));
        }
    } else mtl_pool_del(c.mem);
    printf("{");
    if(!parent_first) {
        registration.t.serial=2;assert(tbuf_by_id(1,0,NULL,NULL)==view);
        assert(g_nretired==1 && live_views==2);
        printf("\"cached_and_deferred_preserved\":1,");
    }
    sample(&t,x,im,stamp);
}
static void stale_child(void) {
    pool_t p={.mem=pages(3*TOTK_VPOOL_PAGE),.size=3*TOTK_VPOOL_PAGE};
    pool_t c={.mem=p.mem+TOTK_VPOOL_PAGE,.size=TOTK_VPOOL_PAGE};
    mtl_pool_add(p.mem,p.size);mtl_pool_add(c.mem,c.size);
    tex_t old=tex(&p,1,TOTK_VPOOL_PAGE);vk_tex *a=tex_get_inner(&old);
    tex_mark_gpu_written_inner(a);assert(a->im->shared);
    mtl_pool_del(p.mem);assert(g_pools[0]->kind==K_CPU);
    mtl_pool_add(p.mem,p.size);tex_t fresh=old;fresh.serial=2;vk_tex *b=tex_get_inner(&fresh);
    tex_stamp stamp=b->identity;mtl_pool_del(c.mem);
    printf("{\"old_storage_reused\":%d,\"stamp_current\":%d,\"cancellations\":%d}",
        a->im==b->im,tex_stamp_current(stamp),wb_cancel_calls);
}
static void remap(int mode,int kind) {
    g_pool_mode=mode;
    pool_t v={.mem=pages(3*TOTK_VPOOL_PAGE),.size=3*TOTK_VPOOL_PAGE,
        .flags=F_VIRTUAL|NVN_MEMORY_POOL_FLAGS_CPU_CACHED,.magic=POOL_MAGIC};
    pool_t p={.mem=pages(TOTK_VPOOL_PAGE),.size=TOTK_VPOOL_PAGE,.flags=F_PHYSICAL,.magic=POOL_MAGIC};
    memset(p.mem,0x66,p.size);mtl_vpool_add(v.mem,v.size);
    assert(g_pools[0]->kind==(mode==POOLS_HOST?K_OWN:K_VIRT));
    tex_t old=tex(&v,1,0),untouched=tex(&v,2,2*TOTK_VPOOL_PAGE);
    vk_tex *a=tex_get_inner(&old),*u=tex_get_inner(&untouched);
    vk_img *original=a->im,*unaffected=u->im;tex_stamp stamp=u->identity;
    if(kind==2) { tex_mark_gpu_written_inner(a);release_texture(&old); }
    totk_mapping_request r={.physical_pool=(NVNmemoryPool *)&p,.size=TOTK_VPOOL_PAGE};
    assert(totk_vpool_map(&v,kind?1:0,kind?&r:NULL));
    int before=upload_calls;*old.mem=0x79;*untouched.mem=0x78;dirty_written=1;submission++;
    assert(tex_get_inner(&untouched)==u && u->im==unaffected);
    printf("{\"untouched_stamp\":%d,\"untouched_uploads\":%d,\"untouched_byte\":%d,",
        tex_stamp_current(stamp),upload_calls-before,last_uploaded_byte);
    before=upload_calls;
    if(kind==2) old.serial=3;
    vk_tex *b=tex_get_inner(&old);assert(b && b->im);
    printf("\"old_image_reused\":%d,\"changed_uploads\":%d,\"changed_byte\":%d,\"cancellations\":%d}",
        b->im==original,upload_calls-before,last_uploaded_byte,wb_cancel_calls);
}
static void texel_remap(void) {
    g_pool_mode=POOLS_EXPORT;
    pool_t v={.mem=pages(3*TOTK_VPOOL_PAGE),.size=3*TOTK_VPOOL_PAGE,
        .flags=F_VIRTUAL,.magic=POOL_MAGIC};
    mtl_vpool_add(v.mem,v.size);resolved_buffer=(VkBuffer)(uintptr_t)1;
    registration.t=tex(&v,11,0);registration.t.d.target=10;registration.t.d.width=16;
    V.props.limits.maxTexelBufferElements=4096;V.props.limits.minTexelBufferOffsetAlignment=16;
    VkBufferView before=tbuf_by_id(1,0,NULL,NULL);
    totk_mapping_request r={.size=TOTK_VPOOL_PAGE};assert(totk_vpool_map(&v,1,&r));
    resolved_buffer=(VkBuffer)(uintptr_t)2;
    printf("{\"cached_view_reused\":%d,\"resolves\":%d,\"retired\":%d}",
        tbuf_by_id(1,0,NULL,NULL)==before,resolve_calls,g_nretired);
}
static void views(int lag) {
    atomic_store(&vk_lag_capture_state,lag);
    pool_t p={.mem=pages(TOTK_VPOOL_PAGE),.size=TOTK_VPOOL_PAGE};mtl_pool_add(p.mem,p.size);
    resolved_buffer=g_pools[0]->buf;VkBuffer buffer=resolved_buffer;
    registration.t=tex(&p,1,0);registration.t.d.target=10;registration.t.d.width=16;
    V.props.limits.maxTexelBufferElements=4096;V.props.limits.minTexelBufferOffsetAlignment=16;
    VkBufferView active=tbuf_by_id(1,0,NULL,NULL);assert(active);
    registration.t.serial=2;assert(tbuf_by_id(1,0,NULL,NULL));
    release_texture(&registration.t);assert(g_nretired==1);
    resolved_buffer=(VkBuffer)(uintptr_t)9;registration.t.serial=3;
    VkBufferView unrelated=tbuf_by_id(1,0,NULL,NULL);assert(unrelated);
    registration.t.serial=4;assert(tbuf_by_id(1,0,NULL,NULL));release_texture(&registration.t);
    assert(g_nretired==2);int before=resolve_calls;
    mtl_pool_del(p.mem);
    assert(view_buffers[(uintptr_t)unrelated]==resolved_buffer);
    int same_buffer_deferred=0;
    for(int i=0;i<g_nretired;i++) same_buffer_deferred+=g_retired[i].bv_buffer==buffer;
    printf("{\"destroy_with_view\":%d,\"active_cleared\":%d,\"deferred_cleared\":%d,",
        destroy_with_view,!view_buffers[(uintptr_t)active],!same_buffer_deferred);
    registration.t.serial=1;VkBufferView after=tbuf_by_id(1,0,NULL,NULL);
    printf("\"rebuilt\":%d,\"resolves\":%d,\"unrelated_views\":%d}",
        after!=active,resolve_calls-before,live_views-(after!=active));
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"nested-view")) nested(0,0);
    else if(!strcmp(argv[1],"parent-before-child")) nested(1,0);
    else if(!strcmp(argv[1],"host-reback")) nested(1,1);
    else if(!strcmp(argv[1],"failed-backing")) nested(1,2);
    else if(!strcmp(argv[1],"stale-child")) stale_child();
    else if(!strcmp(argv[1],"host-zero")) remap(POOLS_HOST,0);
    else if(!strcmp(argv[1],"host-retained")) remap(POOLS_HOST,1);
    else if(!strcmp(argv[1],"host-published")) remap(POOLS_HOST,2);
    else if(!strcmp(argv[1],"export-zero")) remap(POOLS_EXPORT,0);
    else if(!strcmp(argv[1],"export-remap")) remap(POOLS_EXPORT,1);
    else if(!strcmp(argv[1],"heap-remap")) remap(POOLS_HEAP,1);
    else if(!strcmp(argv[1],"texel-remap")) texel_remap();
    else if(!strcmp(argv[1],"views")) views(0);
    else if(!strcmp(argv[1],"views-lag")) views(1);
    else abort();
    puts("");return 0;
}
'''


def probe(resources, pools, vpool):
    code = fixture.resource_probe(resources, pools, scenarios='') + EXTRA
    code += fixture.function(resources, 'tex_mark_gpu_written_inner')
    for name in ('check_request', 'apply_request', 'totk_vpool_map'):
        code += fixture.function(vpool, name)
    return code + SCENARIOS


def baseline(path):
    return subprocess.run(['git', '-C', str(fixture.ROOT), 'show', BASE + ':' + path],
                          check=True, capture_output=True, text=True).stdout


@unittest.skipUnless(fixture.CC and shutil.which(fixture.CC[0]) and shutil.which('git'),
                     'needs a host compiler and git with the main baseline')
class PoolLifetimeParity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = fixture.ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='pool-parity-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        paths = (GFX_PATH + 'vk_res.c', GFX_PATH + 'nvn_vk.c', VPOOL_PATH)
        cls.sources = {'main': tuple(baseline(p) for p in paths),
                       'acd': tuple((fixture.ROOT / p).read_text() for p in paths)}
        cls.observed = {}
        flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for arg in shlex.split(os.environ.get(name, ''))]
        for label, sources in cls.sources.items():
            path = Path(cls.temp.name) / (label + '.c')
            path.write_text(probe(*sources))
            exe = path.with_suffix('')
            command = [*fixture.CC, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-Wno-unused-variable', '-Wno-missing-field-initializers',
                       '-Wno-misleading-indentation', '-Wno-format', '-I' + str(fixture.GFX), '-I' + str(fixture.NVN),
                       '-I' + str(fixture.NVN / 'include'), '-I' + str(fixture.RUNTIME),
                       '-I' + str(fixture.ROOT / 'games/totk/third_party/d2r_gfx/host'),
                       '-I' + str(fixture.ROOT / 'games/totk/android/compat/include'),
                       '-include', str(fixture.ROOT / 'games/totk/android/compat/include/darwin_compat.h'),
                       str(path), '-pthread', '-lm', *flags, '-o', str(exe)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.observed[label] = {}
            for scenario in ('nested-view', 'parent-before-child', 'host-reback', 'failed-backing',
                             'stale-child', 'host-zero', 'host-retained', 'host-published',
                             'export-zero', 'export-remap', 'heap-remap', 'texel-remap', 'views', 'views-lag'):
                env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_'))}
                ran = subprocess.run([str(exe), scenario], capture_output=True, text=True,
                                     env=env, timeout=60)
                if ran.returncode:
                    raise AssertionError(f'{label}/{scenario}: {ran.stdout}{ran.stderr}')
                cls.observed[label][scenario] = json.loads(ran.stdout)

    def unchanged(self, *scenarios):
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                self.assertEqual(self.observed['main'][scenario], self.observed['acd'][scenario])

    def test_nested_view_removal_is_unchanged(self):
        self.unchanged('nested-view')

    def test_parent_before_child_and_surviving_child_upload_are_unchanged(self):
        self.unchanged('parent-before-child')
        self.assertEqual(self.observed['acd']['parent-before-child']['uploads'], 1)
        self.assertEqual(self.observed['acd']['parent-before-child']['byte'], 0x67)

    def test_stale_child_at_reused_address_is_unchanged(self):
        self.unchanged('stale-child')
        # B intentionally remains: surviving refs still permit stale storage sharing.
        self.assertEqual(self.observed['acd']['stale-child']['old_storage_reused'], 1)

    def test_host_rebacking_and_failed_backing_are_unchanged(self):
        self.unchanged('host-reback', 'failed-backing')
        for scenario in ('host-reback', 'failed-backing'):
            self.assertEqual(self.observed['acd'][scenario]['uploads'], 1)
            self.assertEqual(self.observed['acd'][scenario]['writeback_notes'], 1)

    def test_mapvirtual_zero_and_changed_pages_are_unchanged(self):
        self.unchanged('host-zero', 'host-retained', 'host-published',
                       'export-zero', 'export-remap', 'heap-remap', 'texel-remap')
        self.assertEqual(self.observed['acd']['host-retained']['changed_uploads'], 1)
        self.assertEqual(self.observed['acd']['host-retained']['changed_byte'], 0x79)

    def test_only_destroyed_buffer_views_change_including_lag(self):
        for scenario in ('views', 'views-lag'):
            with self.subTest(scenario=scenario):
                self.assertGreater(self.observed['main'][scenario]['destroy_with_view'], 0)
                actual = self.observed['acd'][scenario]
                self.assertEqual(actual, {'destroy_with_view': 0, 'active_cleared': 1,
                    'deferred_cleared': 1, 'rebuilt': 1, 'resolves': 1, 'unrelated_views': 2})

    def test_image_lookup_and_lifecycle_routing_remain_byte_for_byte_main(self):
        main_res, main_pool, main_vpool = self.sources['main']
        acd_res, acd_pool, acd_vpool = self.sources['acd']
        for name in ('vk_tex_pool_gone', 'stor_key_of', 'tex_drop_locked', 'wb_unref',
                     'tex_mark_gpu_written_inner', 'vk_wb_range_gone'):
            self.assertEqual(fixture.function(main_res, name), fixture.function(acd_res, name), name)
        for name in ('add_range', 'back_pool', 'pool_remove_locked', 'pool_retired', 'mtl_vpool_del',
                     'mtl_vpool_add', 'mtl_vpool_release', 'mtl_pool_finalize'):
            self.assertEqual(fixture.function(main_pool, name), fixture.function(acd_pool, name), name)
        # The sole unback exception is view retirement AFTER guest-page restoration.
        retirement = """    /* Guest pages and private aliases are restored under g_pool_mu before
       allowing another registration to back the same range. Only the detached
       Vulkan buffer remains: resource -> pool resolution forbids holding the
       pool mutex while retiring its views. No guest mapping changes follow. */
    pthread_mutex_unlock(&g_pool_mu);
    vk_res_buffer_released(e->buf);
    pthread_mutex_lock(&g_pool_mu);
"""
        main_unback = fixture.function(main_pool, 'unback_pool')
        self.assertEqual(fixture.function(acd_pool, 'unback_pool'),
                         main_unback.replace('    vkDestroyBuffer', retirement + '    vkDestroyBuffer', 1))
        self.assertEqual(main_vpool, acd_vpool)


if __name__ == '__main__':
    unittest.main()
