"""Two-thread fd-backed reuse while exact-buffer view retirement is paused.

Runs the real add/remove/unback and resource-retirement slices. Vulkan objects
are observers; memfd mappings, page restoration and thread synchronization are
real. The immutable pre-fix source is a negative control, not a patched baseline.
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

PRE_FIX = 'c37d72e26a4ad61d6bc231cf07f573ac77b3c3b2'
POOL_PATH = 'games/totk/android/gfx/nvn_vk.c'

SCENARIO = r'''
static pthread_mutex_t pause_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cv=PTHREAD_COND_INITIALIZER;
static int retirement_entered, registration_done;
static unsigned next_buffer=10000;
static void pause_retirement(void) {
    pthread_mutex_lock(&pause_mu);
    retirement_entered=1;
    pthread_cond_signal(&pause_cv);
    while(!registration_done) pthread_cond_wait(&pause_cv,&pause_mu);
    pthread_mutex_unlock(&pause_mu);
}
static int fd_backing(pool_ent *e) {
    size_t size=e->end-e->base;
    int fd=memfd_create("pool-reregister",MFD_CLOEXEC);
    assert(fd>=0 && !ftruncate(fd,(off_t)size));
    uint8_t *alias=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    assert(alias!=MAP_FAILED);
    memcpy(alias,(void *)e->base,size);
    assert(mmap((void *)e->base,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==(void *)e->base);
    e->fd=fd;e->buf=(VkBuffer)(uintptr_t)++next_buffer;
    if(g_wb_alias_enabled) {
        e->wb_alias=alias;g_wb_alias_maps++;g_wb_alias_bytes+=size;
    } else assert(!munmap(alias,size));
    return 1;
}
static void *remove_thread(void *guest) {
    del_range(guest,0);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);
    g_wb_alias_enabled=atoi(argv[1]);
    g_pool_mode=POOLS_EXPORT;
    backing_hook=fd_backing;
    uint8_t *guest=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(guest!=MAP_FAILED);memset(guest,0x35,4096);
    mtl_pool_add(guest,4096);
    pool_ent *old=g_pools[0];
    uint8_t *old_mirror=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,old->fd,0);
    assert(old_mirror!=MAP_FAILED);
    registration.t=(tex_t){.serial=1,.mem=guest,
        .d={.target=10,.width=16,.format=0x25},.L={.size=4096}};
    V.props.limits.maxTexelBufferElements=4096;
    V.props.limits.minTexelBufferOffsetAlignment=16;
    resolved_buffer=old->buf;
    assert(tbuf_by_id(1,0,NULL,NULL) && live_views==1);
    retire_view_hook=pause_retirement;
    pthread_t remover;assert(!pthread_create(&remover,NULL,remove_thread,guest));
    pthread_mutex_lock(&pause_mu);
    while(!retirement_entered) pthread_cond_wait(&pause_cv,&pause_mu);
    pthread_mutex_unlock(&pause_mu);

    /* The worker is inside vk_res_buffer_released, with the resource mutex held.
       The second participant must register without that mutex or a deadlock. */
    old_mirror[0]=0x66;
    int restored_before_retirement=guest[0]==0x35;
    mtl_pool_add(guest,4096);
    pthread_mutex_lock(&g_pool_mu);
    assert(g_npool==1 && g_pools[0]->kind==K_OWN);
    pool_ent *replacement=g_pools[0];
    uint8_t *new_mirror=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,replacement->fd,0);
    assert(new_mirror!=MAP_FAILED);
    pthread_mutex_unlock(&g_pool_mu);
    pthread_mutex_lock(&pause_mu);
    registration_done=1;pthread_cond_signal(&pause_cv);
    pthread_mutex_unlock(&pause_mu);
    assert(!pthread_join(remover,NULL));
    new_mirror[0]=0x79;
    int replacement_still_shared=guest[0]==0x79;
    assert(destroyed_buffers==1 && live_views==0 && !destroy_with_view);
    retire_view_hook=NULL;
    del_range(guest,0);
    assert(!g_npool && !g_wb_alias_maps && !g_wb_alias_bytes);
    assert(!munmap(old_mirror,4096) && !munmap(new_mirror,4096) && !munmap(guest,4096));
    printf("{\"restored_before_retirement\":%d,\"replacement_still_shared\":%d}\n",
           restored_before_retirement,replacement_still_shared);
    return 0;
}
'''


@unittest.skipUnless(fixture.CC and shutil.which(fixture.CC[0]) and shutil.which('git'),
                     'needs Linux memfd, a host compiler and git')
class PoolReregisterConcurrency(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = fixture.ROOT / '.scratch'
        scratch.mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='pool-reregister-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        before = subprocess.run(['git', '-C', str(fixture.ROOT), 'show', PRE_FIX + ':' + POOL_PATH],
                                check=True, capture_output=True, text=True).stdout
        sources = {'before': before, 'fixed': (fixture.ROOT / POOL_PATH).read_text()}
        flags = [arg for name in ('CPPFLAGS', 'CFLAGS', 'LDFLAGS')
                 for arg in shlex.split(os.environ.get(name, ''))]
        cls.observed = {}
        for label, pools in sources.items():
            path = Path(cls.temp.name) / (label + '.c')
            path.write_text(fixture.resource_probe(pools=pools, scenarios=SCENARIO))
            exe = path.with_suffix('')
            command = [*fixture.CC, '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-Wno-unused-variable', '-Wno-missing-field-initializers',
                       '-Wno-misleading-indentation', '-I' + str(fixture.GFX), '-I' + str(fixture.NVN),
                       '-I' + str(fixture.NVN / 'include'), '-I' + str(fixture.RUNTIME),
                       '-I' + str(fixture.ROOT / 'games/totk/third_party/d2r_gfx/host'),
                       '-I' + str(fixture.ROOT / 'games/totk/android/compat/include'),
                       '-include', str(fixture.ROOT / 'games/totk/android/compat/include/darwin_compat.h'),
                       str(path), '-pthread', '-lm', *flags, '-o', str(exe)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.observed[label] = {}
            for alias in ('0', '1'):
                env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_'))}
                ran = subprocess.run([str(exe), alias], capture_output=True, text=True, env=env, timeout=60)
                if ran.returncode:
                    raise AssertionError(f'{label}/alias={alias}: {ran.stdout}{ran.stderr}')
                cls.observed[label][alias] = json.loads(ran.stdout)

    def test_reuse_cannot_interleave_old_page_restoration(self):
        for alias in ('0', '1'):
            with self.subTest(alias=alias):
                self.assertEqual(self.observed['fixed'][alias],
                                 {'restored_before_retirement': 1, 'replacement_still_shared': 1})

    def test_pre_fix_source_reproduces_clobbered_replacement(self):
        for alias in ('0', '1'):
            with self.subTest(alias=alias):
                self.assertEqual(self.observed['before'][alias],
                                 {'restored_before_retirement': 0, 'replacement_still_shared': 0})


if __name__ == '__main__':
    unittest.main()
