"""Production storage lookup/owner retirement slices; Vulkan calls are observers."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

import test_main_defects as fixture

BASE = '8c6c6218d6cd82ff4a04d9a8a072eb8eedc50c72'
RESOURCE = 'games/totk/android/gfx/vk_res.c'
SCENARIO = r'''
static tex_t texture(uint64_t serial, uint8_t *mem) {
    return (tex_t){.serial=serial,.mem=mem,
        .d={.width=8,.height=8,.depth=1,.levels=1,.target=1,.format=0x25},.L={.size=4096}};
}
static void drop(tex_t *t) { tex_drop_locked(nvn_hm_del(&g_by_serial,&t->serial)); }
int main(int argc,char **argv) {
    assert(argc==2);int wb=atoi(argv[1]);
    uint8_t *mem=mmap(NULL,16384,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(mem!=MAP_FAILED);
    if(wb==4) {
        tex_t left=texture(1,mem),cross=texture(2,mem+4096),right=texture(3,mem+12288);
        cross.L.size=8192;
        vk_tex *a=tex_get_inner(&left),*b=tex_get_inner(&cross),*c=tex_get_inner(&right);
        vk_tex_pool_gone((uintptr_t)mem+8192,(uintptr_t)mem+12288);
        tex_t next=cross;next.serial=4;
        printf("{\"cross_reused\":%d,\"left\":%d,\"right\":%d}\n",
            tex_get_inner(&next)->im==b->im,tex_get_inner(&left)==a,tex_get_inner(&right)==c);
        return 0;
    }
    tex_t old=texture(1,mem),adjacent=texture(2,mem+8192);
    vk_tex *x=tex_get_inner(&old),*adj=tex_get_inner(&adjacent);vk_img *im=x->im;
    if(wb) wb_ref(im);
    if(wb==2) drop(&old); /* only a real write-back pin remains */
    pool_ent *pool=calloc(1,sizeof *pool);pool->kind=K_OWN;pool->fd=-1;
    pool->lo=pool->base=(uintptr_t)mem;pool->end=pool->base+8192;
    pool->buf=(VkBuffer)(uintptr_t)1;g_pools[g_npool++]=pool;
    mtl_pool_del(mem);
    int before=retired_images,held=wb==2 || (tex_get_inner(&old)==x && x->im==im);
    tex_t fresh=old;fresh.serial=3;vk_tex *nu=tex_get_inner(&fresh);
    int reused=nu->im==im,adjacent_ok=tex_get_inner(&adjacent)==adj;
    if(wb==1) { drop(&old);assert(retired_images==before);wb_unref(im); }
    else if(wb==3) { wb_unref(im);assert(retired_images==before);drop(&old); }
    else if(wb==2) wb_unref(im);
    else drop(&old);
    int retired=retired_images-before,map_ok=nvn_hm_get(&g_by_storage,&nu->im->k)==nu->im;
    int repeat_fresh=1;
    for(unsigned i=0;i<32;i++) {
        vk_img *previous=nu->im;
        vk_tex_pool_gone((uintptr_t)mem,(uintptr_t)mem+8192);
        tex_t next=fresh;next.serial++;
        nu=tex_get_inner(&next);repeat_fresh &= nu->im!=previous;
        drop(&fresh);fresh=next;
    }
    printf("{\"reused\":%d,\"held\":%d,\"retired\":%d,\"map_ok\":%d,\"adjacent\":%d,\"repeat_fresh\":%d}\n",
        reused,held,retired,map_ok,adjacent_ok,repeat_fresh);
    return 0;
}
'''


def probe(resources):
    code = fixture.resource_probe(resources, scenarios='')
    code += fixture.function(resources, 'wb_ref')
    code += fixture.function(resources, 'wb_unref')
    return code + SCENARIO


@unittest.skipUnless(fixture.CC and shutil.which(fixture.CC[0]), 'needs a host C compiler')
class StorageDetach(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = fixture.ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='storage-detach-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        baseline = subprocess.run(['git', '-C', str(fixture.ROOT), 'show', BASE + ':' + RESOURCE],
                                  check=True, capture_output=True, text=True).stdout
        cls.observed = {}
        flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for arg in shlex.split(os.environ.get(name, ''))]
        for label, source in (('baseline', baseline), ('current', (fixture.ROOT / RESOURCE).read_text())):
            path = Path(cls.temp.name) / (label + '.c')
            path.write_text(probe(source))
            executable = path.with_suffix('')
            command = [*fixture.CC, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-Wno-unused-variable', '-Wno-missing-field-initializers',
                       '-Wno-misleading-indentation', '-I' + str(fixture.GFX), '-I' + str(fixture.NVN),
                       '-I' + str(fixture.NVN / 'include'), '-I' + str(fixture.RUNTIME),
                       '-I' + str(fixture.ROOT / 'games/totk/third_party/d2r_gfx/host'),
                       '-I' + str(fixture.ROOT / 'games/totk/android/compat/include'),
                       '-include', str(fixture.ROOT / 'games/totk/android/compat/include/darwin_compat.h'),
                       str(path), '-pthread', '-lm', *flags, '-o', str(executable)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.observed[label] = []
            for order in range(5):
                ran = subprocess.run([str(executable), str(order)], capture_output=True, text=True, timeout=10)
                if ran.returncode:
                    raise AssertionError(f'{label}/{order}: {ran.stdout}{ran.stderr}')
                cls.observed[label].append(json.loads(ran.stdout))

    def test_main_negative_control_reuses_live_and_writeback_pinned_images(self):
        for actual in self.observed['baseline'][:4]:
            self.assertEqual(actual, {'reused': 1, 'held': 1, 'retired': 0, 'map_ok': 1,
                                      'adjacent': 1, 'repeat_fresh': 0})

    def test_detached_owners_retire_in_either_order_without_removing_replacement(self):
        for actual in self.observed['current'][:4]:
            self.assertEqual(actual, {'reused': 0, 'held': 1, 'retired': 1, 'map_ok': 1,
                                      'adjacent': 1, 'repeat_fresh': 1})

    def test_overlap_detaches_without_touching_adjacent_images(self):
        self.assertEqual(self.observed['baseline'][4], {'cross_reused': 1, 'left': 1, 'right': 1})
        self.assertEqual(self.observed['current'][4], {'cross_reused': 0, 'left': 1, 'right': 1})


if __name__ == '__main__':
    unittest.main()
