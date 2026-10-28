/* Real Vulkan native-UI fixture: 1x scene, native UI raster, clipped clear/copy,
   aliased game sampling/readback, coordinate fallback and host resize.
   Run fresh with TOTK_NATIVE_UI=0 and =1, optionally TOTK_VK_REC_THREADS=2. */
#include "vk_int.h"
#include "vk_record.h"
#include "../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <sys/mman.h>
#include <math.h>
#include <time.h>
static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); puts(""); } } while (0)
static void exec(uint32_t op, const void *p) { vk_draw_exec(op,p); }
static void binary(int stage, int salt, uint8_t ctl[0x880], uint8_t code[0x230]) {
    memset(ctl,0,0x880); memset(code,salt,0x30);
    for (int i=0;i<0x200;i++) code[0x30+i]=(uint8_t)(i*7+salt);
    uint32_t len=0x230, c=0x880; memcpy(ctl+0x20,&c,4);
    memcpy(ctl+0x700,&len,4); memcpy(ctl+0x704,&len,4); memcpy(ctl+0x714,&stage,4);
}
static void target(tex_t *t, pool_t *pool, uint8_t *mem, int size, int serial) {
    memset(t,0,sizeof *t); t->d.target=1; t->d.format=0x25;
    t->d.width=t->d.height=size; t->d.depth=t->d.levels=1;
    nvn_tex_layout(&t->d,&t->L); t->mem=mem; t->pool=pool; t->serial=serial;
}
static void set_target(tex_t *t) { p_set_rt p={.n=1,.color={t}}; exec(OP_SET_RT,&p); }
static void scissor(int x,int y,int w,int h) { p_scissor p={x,y,w,h}; exec(OP_SCISSOR,&p); }
static void viewport(int x,int y,int w,int h) { p_viewports p={.n=1,.v={{x,y,w,h,0,1}}}; exec(OP_VIEWPORTS,&p); }
static void clear(float r,float g,float b) { p_clear_color p={0,15,{r,g,b,1}}; exec(OP_CLEAR_COLOR,&p); }
static void draw(prog_t *p) { p_bind_program b={p,3}; exec(OP_BIND_PROGRAM,&b); p_draw d={.prim=4,.count=3,.instances=1}; exec(OP_DRAW,&d); }
static uint8_t *capture(vk_tex *x, uint32_t *w, uint32_t *h, int *native) {
    vk_end_rendering(); vk_post_output o; *native=vk_tex_native_output(x,&o);
    VkImage image=*native?o.image:vk_tex_image(x); *w=*native?o.w:vk_tex_width(x,0); *h=*native?o.h:vk_tex_height(x,0);
    VkBuffer buffer; VkDeviceSize off; uint8_t *bytes=vk_transient((size_t)*w * *h *4,16,&buffer,&off);
    VkBufferImageCopy c={.bufferOffset=off,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={*w,*h,1}};
    VkCommandBuffer cb=vk_cb(); vk_barrier_all(cb); vkCmdCopyImageToBuffer(cb,image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&c);
    vk_flush_wait(); return bytes;
}
static int rgb(const uint8_t *p,int r,int g,int b) { return abs(p[0]-r)<=1 && abs(p[1]-g)<=1 && abs(p[2]-b)<=1 && p[3]==255; }
static uint64_t clock_ns(clockid_t clock) {
    struct timespec t; clock_gettime(clock,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
static int compare_double(const void *a,const void *b) { double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y); }
/* Fresh-process on/off benchmark of the actual Vulkan draw walker, AFTER a
   promoted canvas exists. No present/GPU waits/clock calls inside the loop. */
static void offcanvas_bench(tex_t *t,prog_t *program) {
    enum { SAMPLES=9,DRAWS=20000 };
    double cpu[SAMPLES],wall[SAMPLES];
    p_draw d={.prim=4,.count=3,.instances=1}; p_bind_program b={program,3};
    p_bind_handles ordinary={.stage=1,.n=1,.h={1|(1u<<20)}};
    vk_record_test_counts start,finish; uint64_t draws=0,passes=0;
    for (int sample=-2;sample<SAMPLES;sample++) {
        mtl_sub_begin(); set_target(t); viewport(0,0,32,32); scissor(0,0,1,1);
        exec(OP_BIND_TEX,&ordinary); exec(OP_BIND_PROGRAM,&b);
        exec(OP_DRAW,&d); /* compile/descriptor/attachment setup is not loop cost */
        vk_record_test_counts_get(&start);
        uint64_t w0=clock_ns(CLOCK_MONOTONIC),c0=clock_ns(CLOCK_THREAD_CPUTIME_ID);
        for (int i=0;i<DRAWS;i++) exec(OP_DRAW,&d);
        uint64_t c1=clock_ns(CLOCK_THREAD_CPUTIME_ID),w1=clock_ns(CLOCK_MONOTONIC);
        vk_record_test_counts_get(&finish);
        if (sample>=0) {
            cpu[sample]=(double)(c1-c0)/DRAWS; wall[sample]=(double)(w1-w0)/DRAWS;
            draws+=finish.draws-start.draws; passes+=finish.passes-start.passes;
        }
        mtl_sub_commit(); vk_gpu_idle();
    }
    qsort(cpu,SAMPLES,sizeof *cpu,compare_double); qsort(wall,SAMPLES,sizeof *wall,compare_double);
    CHECK(draws==SAMPLES*DRAWS && !passes,"off-canvas benchmark emits every draw without extra passes (%llu/%llu)",
        (unsigned long long)draws,(unsigned long long)passes);
    printf("native-ui offcanvas benchmark: native=%d recorder=%d draws=%llu added-passes=%llu "
        "walking-cpu median=%.2f min=%.2f max=%.2f ns/draw wall median=%.2f ns/draw\n",
        vk_native_ui_enabled(),vk_record_enabled(),(unsigned long long)draws,(unsigned long long)passes,
        cpu[SAMPLES/2],cpu[0],cpu[SAMPLES-1],wall[SAMPLES/2]);
}
static void blending(int enable) {
    blend_st blend={0,NVN_BLEND_FUNC_SRC_ALPHA,NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,
        NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,NVN_BLEND_EQUATION_ADD,NVN_BLEND_EQUATION_ADD};
    color_st color={enable,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS};
    p_bind_state b={.kind=0}; memcpy(b.bytes,&blend,sizeof blend); exec(OP_BIND_STATE,&b);
    b.kind=1; memcpy(b.bytes,&color,sizeof color); exec(OP_BIND_STATE,&b);
}
static void preservation_tests(pool_t *pool,uint8_t *mem,tex_t *canvas,tex_t *texture,prog_t *ui) {
    uint64_t a[VK_NUI_OPS],as[VK_NUI_OPS],b[VK_NUI_OPS],bs[VK_NUI_OPS];
    uint32_t w,h; int native; uint8_t *host,shadow[32*32*4];
    NVNcopyRegion whole={0,0,0,32,32,1},tex_whole={0,0,0,24,24,1};
    uint8_t alpha_blue[24*24*4];
    for (unsigned i=0;i<24*24;i++) { alpha_blue[i*4]=alpha_blue[i*4+1]=0; alpha_blue[i*4+2]=255; alpha_blue[i*4+3]=128; }
    p_bind_handles th={.stage=1,.n=1,.h={1|(1u<<20)}}; exec(OP_BIND_TEX,&th);
    vk_tex *x=vk_tex_get(canvas);
    set_target(canvas); viewport(0,0,32,32); scissor(0,0,32,32); clear(0,1,0); vk_end_rendering();
    vk_native_gpu_counts(a,as);
    vk_native_frame_end(); scissor(0,0,4,4); clear(1,0,0); vk_end_rendering();
    vk_native_gpu_counts(b,bs);
    CHECK(b[VK_NUI_RESOLVE]==a[VK_NUI_RESOLVE]+1,"previous-frame partial 1x clear keeps the required shadow resolve");
    mtl_read_texels(canvas,0,0,&whole,shadow,128,sizeof shadow);
    CHECK(rgb(shadow,255,0,0)&&rgb(shadow+(20*32+20)*4,0,255,0),"partial redraw preserves previous-frame pixels outside its rectangle");
    mtl_write_texels(texture,0,0,&tex_whole,alpha_blue,96,sizeof alpha_blue);
    vk_tex_native_ui_begin(x); blending(1); scissor(8,8,4,4); draw(ui); vk_end_rendering();
    host=capture(x,&w,&h,&native);
    CHECK(native&&w==64&&rgb(host+(18*w+18)*4,0,127,128)&&rgb(host+(40*w+40)*4,0,255,0)&&rgb(host,255,0,0),
        "partial native alpha draw blends over the preserved 1x scene and earlier patch");
    vk_native_gpu_counts(a,as);
    vk_native_frame_end(); scissor(0,0,32,32); clear(0,0,1); vk_end_rendering();
    vk_native_gpu_counts(b,bs);
    CHECK(b[VK_NUI_RESOLVE]==a[VK_NUI_RESOLVE]&&bs[VK_NUI_RESOLVE]==as[VK_NUI_RESOLVE]+1,
        "full next-frame 1x clear avoids, rather than executes, the stale shadow resolve");
    mtl_read_texels(canvas,0,0,&whole,shadow,128,sizeof shadow);
    CHECK(rgb(shadow,0,0,255)&&rgb(shadow+(31*32+31)*4,0,0,255),"full 1x clear replaces every shadow pixel");
    vk_tex_native_ui_begin(x); scissor(8,8,4,4); draw(ui); vk_end_rendering();
    host=capture(x,&w,&h,&native);
    CHECK(rgb(host,0,0,255)&&rgb(host+(18*w+18)*4,0,0,255),"next UI seed uses the new cleared scene, not previous-frame native pixels");
    /* A full clear defines a new/ resized native image without copying a seed.
       A partial draw, in contrast, must preserve on both first use and resize. */
    tex_t window,partial; target(&window,pool,mem+0x80000,32,4010); target(&partial,pool,mem+0x90000,32,4011);
    mtl_xv_window_tex(window.serial); vk_tex *wx=vk_tex_get(&window);
    set_target(&window); viewport(0,0,32,32); scissor(0,0,32,32);
    vk_native_gpu_counts(a,as); clear(1,0,0); vk_end_rendering(); vk_native_gpu_counts(b,bs);
    host=capture(wx,&w,&h,&native);
    CHECK(b[VK_NUI_WINDOW_SEED]==a[VK_NUI_WINDOW_SEED]&&bs[VK_NUI_WINDOW_SEED]==as[VK_NUI_WINDOW_SEED]+1&&
        w==64&&rgb(host,255,0,0)&&rgb(host+(63*w+63)*4,255,0,0),"first native full clear defines all pixels without a window seed");
    vk_native_window_size(96,96); vk_native_gpu_counts(a,as); clear(0,1,0); vk_end_rendering(); vk_native_gpu_counts(b,bs);
    host=capture(wx,&w,&h,&native);
    CHECK(b[VK_NUI_WINDOW_SEED]==a[VK_NUI_WINDOW_SEED]&&b[VK_NUI_RESOLVE]==a[VK_NUI_RESOLVE]&&
        bs[VK_NUI_RESOLVE]==as[VK_NUI_RESOLVE]+1&&bs[VK_NUI_WINDOW_SEED]==as[VK_NUI_WINDOW_SEED]+1&&
        w==96&&rgb(host,0,255,0)&&rgb(host+(95*w+95)*4,0,255,0),"resized full native clear replaces old content without resolve or seed");
    set_target(&partial); scissor(0,0,32,32); clear(0,1,0); vk_end_rendering();
    mtl_xv_window_tex(partial.serial); vk_tex *px=vk_tex_get(&partial);
    vk_native_gpu_counts(a,as); scissor(8,8,4,4); draw(ui); vk_end_rendering(); vk_native_gpu_counts(b,bs);
    host=capture(px,&w,&h,&native);
    CHECK(b[VK_NUI_WINDOW_SEED]==a[VK_NUI_WINDOW_SEED]+1&&w==96&&rgb(host,0,255,0)&&rgb(host+(27*w+27)*4,0,127,128),
        "first partial window draw retains the seed and correct alpha background");
    vk_native_gpu_counts(a,as); vk_native_window_size(128,128); scissor(16,16,4,4); draw(ui); vk_end_rendering(); vk_native_gpu_counts(b,bs);
    host=capture(px,&w,&h,&native);
    CHECK(b[VK_NUI_WINDOW_SEED]==a[VK_NUI_WINDOW_SEED]+1&&b[VK_NUI_RESOLVE]==a[VK_NUI_RESOLVE]+1&&
        w==128&&rgb(host,0,255,0)&&rgb(host+(36*w+36)*4,0,127,128)&&rgb(host+(68*w+68)*4,0,127,128),
        "resized partial alpha redraw retains both old UI and untouched pixels via resolve plus seed");
    /* A non-square host still aspect-fits. Presentation's black letterbox
       clear is unchanged; it must not become part of the active guest basis. */
    vk_native_window_size(160,128); scissor(24,24,4,4); draw(ui); vk_end_rendering(); host=capture(px,&w,&h,&native);
    CHECK(w==128&&h==128&&rgb(host,0,255,0),"aspect-fit host change preserves active image and leaves letterboxing to presentation");
    blending(0);
}
int main(int argc,char **argv) {
    int bench=argc==2 && !strcmp(argv[1],"--offcanvas-bench");
    if (argc!=1 && !bench) { fprintf(stderr,"usage: %s [--offcanvas-bench]\n",argv[0]); return 2; }
    setenv("TOTK_VK_COMPILE_THREADS","0",1); setenv("TOTK_POST_AA","off",1);
    setenv("TOTK_VK_CHUNK_DRAWS","0",1);
    mtl_init(); int on=vk_native_ui_enabled(); vk_native_window_size(48,48);
    int profile=getenv("TOTK_VK_PASS_PROFILE")&&atoi(getenv("TOTK_VK_PASS_PROFILE"))>0;
    if (profile) mtl_present(NULL,0,NULL);
    uint8_t *mem=mmap(NULL,4u<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (mem==MAP_FAILED) return 2; mtl_pool_add(mem,4u<<20);
    pool_t pool={.mem=mem,.size=4u<<20,.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    tex_t canvas,texture,window; target(&canvas,&pool,mem,32,4001); target(&texture,&pool,mem+0x10000,24,4002);
    target(&window,&pool,mem+0x20000,64,4003);
    uint8_t stripes[24*24*4];
    for (int y=0;y<24;y++) for (int x=0;x<24;x++) { uint8_t *p=stripes+(y*24+x)*4; p[0]=(x&1)?0:255; p[1]=(x&1)?255:0; p[2]=0; p[3]=255; }
    uint8_t ctl[3][0x880]; prog_t ui={.serial=4040,.nstage=2}, coord={.serial=4042,.nstage=2};
    for (int i=0;i<3;i++) binary(i!=0,40+i,ctl[i],mem+0x40000+i*0x1000);
    ui.sd[0].data=(uintptr_t)(mem+0x40000); ui.sd[0].control=ctl[0];
    ui.sd[1].data=(uintptr_t)(mem+0x41000); ui.sd[1].control=ctl[1];
    coord.sd[0]=ui.sd[0]; coord.sd[1].data=(uintptr_t)(mem+0x42000); coord.sd[1].control=ctl[2];
    float tri[]={-1,-1,3,-1,-1,3}; memcpy(mem+0x50000,tri,sizeof tri);
    mtl_reg_tex(1,&texture,NULL); samp_par sampler={.lod_max=1000}; mtl_reg_samp(1,&sampler);
    mtl_sub_begin();
    NVNcopyRegion fulltex={0,0,0,24,24,1}; mtl_write_texels(&texture,0,0,&fulltex,stripes,24*4,24*24*4);
    p_bind_vattr va={.n=1}; va_st a={0x16,0,0}; memcpy(&va.st[0],&a,4); exec(OP_BIND_VATTR,&va);
    p_bind_vstream vs={.n=1}; vs.st[0].stride=8; exec(OP_BIND_VSTREAM,&vs);
    p_bind_vbuf vb={.n=1,.r={{(uintptr_t)(mem+0x50000),sizeof tri}}}; exec(OP_BIND_VBUF,&vb);
    p_bind_handles th={.stage=1,.n=1,.h={1|(1u<<20)}}; exec(OP_BIND_TEX,&th);
    set_target(&canvas); viewport(0,0,32,32); scissor(0,0,32,32); clear(0,0,1); vk_end_rendering();
    vk_tex *x=vk_tex_get(&canvas); vk_post_output output;
    CHECK(!vk_tex_native_output(x,&output),"ordinary same-sized scene canvas is not promoted by size");
    vk_tex_native_promote(x); /* Synthetic fixture supplies the observed terminal-transfer identity. */
    vk_tex_native_ui_begin(x);
    viewport(4,4,16,16); scissor(8,8,8,8); draw(&ui); vk_end_rendering();
    uint32_t w,h; int native; uint8_t *host=capture(x,&w,&h,&native);
    CHECK(w==(on?48u:32u)&&h==w&&native==on,"output host size and promotion follow the switch");
    CHECK(rgb(host,0,0,255),"1x scene is upscaled once under UI");
    int bad=0; unsigned left=on?12:8,right=on?24:16,origin=on?6:4,span=on?24:16;
    for (unsigned y=0;y<h;y++) for (unsigned xx=0;xx<w;xx++) {
        const uint8_t *p=host+(y*w+xx)*4;
        if (xx>=left&&xx<right&&y>=left&&y<right) {
            unsigned tx=(unsigned)(((double)(xx-origin)+.5)*24/span); bad+=!rgb(p,(tx&1)?0:255,(tx&1)?255:0,0);
        } else bad+=!rgb(p,0,0,255);
    }
    CHECK(!bad,"viewport/scissor transform and sharp native texture pixels (%d mismatches)",bad);
    if(on) CHECK(rgb(host+(13*w+12)*4,255,0,0)&&rgb(host+(13*w+13)*4,0,255,0),"adjacent native UI pixels remain distinct (not upscaled 1x UI)");
    tex_t alias=canvas; alias.serial=4004; uint8_t shadow[32*32*4]; NVNcopyRegion whole={0,0,0,32,32,1};
    mtl_read_texels(&alias,0,0,&whole,shadow,32*4,sizeof shadow);
    CHECK(rgb(shadow,0,0,255),"aliased game readback retains 1x scene");
    int r=on?191:255,g=on?64:0;
    CHECK(rgb(shadow+(9*32+8)*4,r,g,0),"aliased readback resolves native pixels to the correct 1x content (%u,%u)",shadow[(9*32+8)*4],shadow[(9*32+8)*4+1]);
    tex_t sampled; target(&sampled,&pool,mem+0x30000,32,4005);
    mtl_reg_tex(2,&alias,NULL); th.h[0]=2|(1u<<20); exec(OP_BIND_TEX,&th);
    /* A late alias registration must join the native-descriptor filter, and a
       new native write must resolve even when its descriptor view is cached. */
    int alias_target=1; vk_tex_by_id(2,&alias_target,NULL);
    set_target(&canvas); th.h[0]=1|(1u<<20); exec(OP_BIND_TEX,&th); draw(&ui); vk_end_rendering();
    th.h[0]=2|(1u<<20); exec(OP_BIND_TEX,&th);
    set_target(&sampled); viewport(0,0,32,32); scissor(0,0,32,32); draw(&ui);
    p_copy_b2t read={.tex=&sampled,.src=(uintptr_t)(mem+0x60000),.r={0,0,0,32,32,1}};
    exec(OP_COPY_T2B,&read); vk_flush_wait();
    CHECK(!memcmp(mem+0x60000,shadow,sizeof shadow),"ordinary game sampler plus texture-to-buffer copy sees exactly the resolved 1x image");
    set_target(&canvas); th.h[0]=1|(1u<<20); exec(OP_BIND_TEX,&th);
    /* Native rectangular clears and copies preserve untouched native UI. */
    scissor(2,2,2,2); clear(1,1,0); vk_end_rendering(); host=capture(x,&w,&h,&native);
    unsigned q=on?3:2; CHECK(rgb(host+(q*w+q)*4,255,255,0)&&rgb(host+((q-1)*w+q)*4,0,0,255),"clear rectangle uses transformed pixel boundaries");
    uint8_t white[2*2*4]; memset(white,255,sizeof white); NVNcopyRegion patch={20,20,0,2,2,1};
    mtl_write_texels(&canvas,0,0,&patch,white,8,sizeof white); host=capture(x,&w,&h,&native);
    q=on?30:20; CHECK(rgb(host+(q*w+q)*4,255,255,255),"CPU copy region reaches transformed native location");
    if(on) CHECK(rgb(host+(13*w+12)*4,255,0,0)&&rgb(host+(13*w+13)*4,0,255,0),"partial copy preserves untouched native UI");
    p_copy_t2t copy={.src=&texture,.dst=&canvas,.sr={0,0,0,2,2,1},.dr={20,20,0,2,2,1}};
    exec(OP_COPY_T2T,&copy); host=capture(x,&w,&h,&native);
    CHECK(rgb(host+(q*w+q)*4,255,0,0),"GPU texture-copy region patches native destination coordinates");
    if(on) CHECK(rgb(host+(13*w+12)*4,255,0,0)&&rgb(host+(13*w+13)*4,0,255,0),"GPU copy preserves sharp UI outside its region");
    if(on) {
        p_clear_tex region={.tex=&canvas,.r={24,24,0,2,2,1},.mask=15,.c.f={0,1,1,1}};
        exec(OP_CLEAR_TEX,&region); host=capture(x,&w,&h,&native);
        CHECK(rgb(host+(36*w+36)*4,0,255,255)&&rgb(host,0,0,255),"texture-region clear remains partial on native target");
    }
    scissor(0,0,32,32); viewport(0,0,32,32); draw(&coord); vk_end_rendering();
    mtl_read_texels(&canvas,0,0,&whole,shadow,128,sizeof shadow);
    CHECK(rgb(shadow+(8*32+8)*4,68,68,0),"FragCoord.xy fallback executes with guest pixel coordinates");
    if(on) { vk_native_window_size(64,64); draw(&ui); vk_end_rendering(); host=capture(x,&w,&h,&native); CHECK(w==64&&h==64,"window resize safely replaces native sibling"); }
    /* Registration, not dimensions or DISPLAY flags, identifies the terminal image. */
    mtl_xv_window_tex(window.serial); vk_tex *wx=vk_tex_get(&window);
    CHECK(vk_tex_native_window(wx)==on,"registered window identity follows native mode");
    set_target(&window); scissor(0,0,64,64); clear(0,0,1); vk_end_rendering();
    vk_tex_native_transfer(wx,32,32); viewport(0,0,32,32); scissor(0,0,32,32); draw(&ui);
    vk_end_rendering(); host=capture(wx,&w,&h,&native);
    CHECK(native==on&&w==64,"terminal target uses active canvas basis, not its larger guest allocation");
    uint8_t window_shadow[64*64*4]; NVNcopyRegion window_whole={0,0,0,64,64,1};
    mtl_read_texels(&window,0,0,&window_whole,window_shadow,256,sizeof window_shadow);
    CHECK(rgb(window_shadow+(40*64+40)*4,0,0,255),"window readback resolves only active canvas, preserving guest padding");
    if (on) {
        scissor(0,0,64,64); clear(0,1,1); vk_end_rendering();
        mtl_read_texels(&window,0,0,&window_whole,window_shadow,256,sizeof window_shadow);
        CHECK(rgb(window_shadow,0,255,255)&&rgb(window_shadow+(63*64+63)*4,0,255,255),
            "full padded-window clear defines active pixels AND guest-only padding");
        scissor(8,8,4,4); draw(&ui); vk_end_rendering();
        mtl_read_texels(&window,0,0,&window_whole,window_shadow,256,sizeof window_shadow);
        CHECK(rgb(window_shadow+(40*64+40)*4,0,255,255),"subsequent native redraw preserves cleared guest-only padding");
    }
    /* A bound ordinary self-alias with EMPTY scissors must not split a pass:
       no feedback command is emitted, but native preparation still runs. */
    mtl_reg_tex(3,&sampled,NULL); th.h[0]=3|(1u<<20); exec(OP_BIND_TEX,&th);
    set_target(&sampled); viewport(0,0,32,32); scissor(0,0,0,0);
    vk_record_test_counts before,after; draw(&ui); vk_record_test_counts_get(&before);
    for (int i=0;i<16;i++) draw(&ui);
    vk_record_test_counts_get(&after);
    CHECK(before.passes==after.passes,"ordinary 1x aliases do not acquire native-UI pass boundaries");
    vk_end_rendering();
    if (on) {
        CHECK(vk_native_descriptor(2) && !vk_native_descriptor(1),"prefilter includes canvas aliases, excludes ordinary textures");
        mtl_reg_tex(2,&texture,NULL); CHECK(!vk_native_descriptor(2),"descriptor re-registration removes a stale promoted classification");
        mtl_reg_tex(2,&alias,NULL); CHECK(vk_native_descriptor(2),"descriptor re-registration restores promoted alias classification");
    }
    if (on) preservation_tests(&pool,mem,&canvas,&texture,&ui);
    if (vk_scene_post_enabled()) {
        /* Docked 1080p scene composition has equal guest/window extents. A
           scene opt-in still needs a valid sibling for real/generated parity. */
        vk_native_window_size(32,32); set_target(&canvas);
        viewport(0,0,32,32); scissor(0,0,32,32); vk_tex_native_ui_begin(x);
        clear(0,0,1); vk_end_rendering(); host=capture(x,&w,&h,&native);
        CHECK(native && w==32 && h==32 && rgb(host,0,0,255),
            "scene mode supports equal-size docked host composition without a second size change");
    }
    vk_native_frame_end(); CHECK(!vk_tex_native_active(x),"new real frame returns pre-UI canvas work to 1x");
    mtl_sub_commit(); vk_gpu_idle();
    if (profile) { mtl_present(NULL,0,NULL); vk_gpu_idle(); mtl_present(NULL,0,NULL); vk_gpu_idle(); }
    if (bench) offcanvas_bench(&sampled,&ui);
    printf("%s: native-ui=%d %d checks, %d failed\n",failures?"FAILED":"ok",on,checks,failures);
    return failures!=0;
}
