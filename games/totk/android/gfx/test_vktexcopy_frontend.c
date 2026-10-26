/* Exercise the real static NVN recording wrappers, not a second diagnostic implementation.
   Section GC keeps the unrelated game/runtime entry points out of the host draw fixture. */
/* This fixture invokes wrappers directly: it has no guest thunk table or async replay queue.
   Do not root the frontend's application constructor or optional queue-drain bridge. */
#define constructor unused
#define nvn_x_drain test_unused_nvn_x_drain
#include "../../third_party/d2r_gfx/gfx/nvn/hle_nvn.c"
#undef nvn_x_drain
#undef constructor

/* The host fixture has no loaded guest modules. Its caller is therefore explicitly unknown. */
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    (void)pc; (void)name; (void)off; return 0;
}

void test_texcopy_initialize(tex_t *t, const nvn_texdesc *desc, const pool_t *pool, ptrdiff_t off) {
    tex_builder_t builder = {.d = *desc, .pool = (pool_t *)pool, .off = off};
    memset(t, 0, sizeof *t);
    h_nvnTextureInitialize((NVNtexture *)t, (const NVNtextureBuilder *)&builder);
}

uint64_t test_texcopy_submit(const tex_t *src, const tex_t *dst, uint8_t *readback,
        const float rgba[4], uint64_t *replay_sub) {
    extern uint64_t vk_submit_serial(void);
    NVNcommandBuffer commands;
    const NVNtexture *target = (const NVNtexture *)src;
    NVNcopyRegion region = {0, 0, 0, (int)src->d.width, (int)src->d.height, 1};
    h_nvnCommandBufferInitialize(&commands, NULL);
    h_nvnCommandBufferBeginRecording(&commands);
    h_nvnCommandBufferSetRenderTargets(&commands, 1, &target, NULL, NULL, NULL);
    h_nvnCommandBufferSetScissor(&commands, 0, 0, region.width, region.height);
    h_nvnCommandBufferClearColor(&commands, 0, rgba, 0xf);
    h_nvnCommandBufferCopyTextureToTexture(&commands, (const NVNtexture *)src, NULL, &region,
        (const NVNtexture *)dst, NULL, &region, 0);
    h_nvnCommandBufferCopyTextureToBuffer(&commands, (const NVNtexture *)dst, NULL, &region,
        (NVNbufferAddress)(uintptr_t)readback, 0);
    h_nvnCommandBufferSetRenderTargets(&commands, 0, NULL, NULL, NULL, NULL);
    /* Submit the recording directly, as the other backend fixtures do. No guest queue, control-memory
       lifetime or async frontend thread is needed to exercise these actual recording entry points. */
    put_hdr(cmd_alloc(CB(&commands), sizeof(op_hdr)), OP_END, sizeof(op_hdr));
    mtl_sub_begin(); *replay_sub = vk_submit_serial(); mtl_exec(CB(&commands)->seg);
    uint64_t ticket = mtl_sub_commit();
    h_nvnCommandBufferFinalize(&commands);
    return ticket;
}

void test_texcopy_release(const tex_t *t) {
    tex_t copy = *t;
    live_del(&copy); mtl_tex_release(&copy);
}
