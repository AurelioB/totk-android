/* Included only by test_vkframegen_prepared.c. The host has no Android
 * presenter: the TEST callback substitutes its choice of an already immutable
 * graph. The chunk hook, guest walker, command buffers, FIFO, timestamps,
 * pending posts, output pixels and ownership below are production backend work.
 * --chunk-hook is fixed-override ownership proof; --chunk-hook-adaptive keeps
 * genuine measured admission and reports (rather than hides) a budget refusal. */
#include "../games/totk/android/gfx/framegen_budget.h"
#include <sys/mman.h>

typedef struct { void *physical,*alias; size_t bytes; } chunk_cpu_alias;
static chunk_cpu_alias chunk_alias_create(void) {
    chunk_cpu_alias a={.bytes=65536};
    a.physical=mmap(NULL,a.bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    a.alias=mmap(NULL,a.bytes,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    CHECK(a.physical!=MAP_FAILED && a.alias!=MAP_FAILED,"map two genuine CPU aliases of the same owned bytes");
    if (a.physical==MAP_FAILED || a.alias==MAP_FAILED) exit(1);
    /* Match test_vkrecord_spans.c's lag_cache_fixture: retain ONE donor
     * VkBuffer. POOLS_HOST mtl_vpool_add would create a second K_OWN import
     * whose tied/larger span wins over the donor alias during resolution. */
    mtl_pool_add(a.physical,a.bytes); nvn_dirty_add_range(a.physical,a.bytes);
    int shared=mtl_pool_alias(a.alias,a.physical,a.bytes) ||
        mremap(a.physical,0,a.bytes,MREMAP_MAYMOVE|MREMAP_FIXED,a.alias)==a.alias;
    CHECK(shared,"create genuine shared CPU pages using the existing donor alias fixture");
    if (!shared) exit(1);
    nvn_dirty_add_range(a.alias,a.bytes);
    vk_record_test_alias(a.alias,(uintptr_t)a.physical,a.bytes);
    VkBuffer physical,alias; uint64_t physical_offset,alias_offset;
    CHECK(vk_resolve((uintptr_t)a.physical,16,&physical,&physical_offset) &&
        vk_resolve((uintptr_t)a.alias,16,&alias,&alias_offset) &&
        physical==alias && physical_offset==alias_offset,
        "both real CPU mappings resolve to the same Vulkan alias byte range");
    return a;
}
typedef struct {
    uint32_t physical[4],alias[4],logical_physical[4],logical_alias[4];
    uint64_t pending_physical,pending_alias;
} chunk_alias_snapshot;
static void chunk_alias_observe(const chunk_cpu_alias *a,const float expected[4],
        chunk_alias_snapshot golden[4],unsigned stage,unsigned enabled) {
    chunk_alias_snapshot observed={0};
    memcpy(observed.physical,a->physical,sizeof observed.physical);
    memcpy(observed.alias,a->alias,sizeof observed.alias);
    observed.pending_physical=vk_ub_pending((uintptr_t)a->physical,sizeof observed.physical);
    observed.pending_alias=vk_ub_pending((uintptr_t)a->alias,sizeof observed.alias);
    vk_ub_read((uintptr_t)a->physical,observed.logical_physical,sizeof observed.logical_physical);
    vk_ub_read((uintptr_t)a->alias,observed.logical_alias,sizeof observed.logical_alias);
    CHECK(!memcmp(observed.physical,observed.alias,sizeof observed.physical),
        "physical and virtual CPU aliases expose identical actual bytes at every observed frontier");
    CHECK(!memcmp(observed.logical_physical,expected,sizeof observed.logical_physical) &&
        !memcmp(observed.logical_alias,expected,sizeof observed.logical_alias),
        "both ordered alias reads preserve the newest real logical update");
    /* update_ub may materialize an existing original-memory producer, and
     * vk_ub_touch/mandatory writeback may publish it before a final commit.
     * The scheduler must preserve those real baseline frontiers, not replace
     * them with a stronger final-only publication policy. These frontiers are
     * booleans, not absolute ticket IDs; child ticket renumbering is irrelevant. */
    if (!enabled) golden[stage]=observed;
    else CHECK(!memcmp(&golden[stage],&observed,sizeof observed),
        "CHUNK1 preserves exact flag-off CPU alias bytes, logical values and both publication frontiers");
    printf("chunk-alias: chunk=%u stage=%u pending-physical=%llu pending-alias=%llu cpu=%08x/%08x/%08x/%08x frontier=private-UBO-pending-bool\n",
        enabled,stage,(unsigned long long)observed.pending_physical,
        (unsigned long long)observed.pending_alias,observed.physical[0],observed.physical[1],
        observed.physical[2],observed.physical[3]);
}
static void chunk_alias_destroy(chunk_cpu_alias *a) {
    vk_gpu_idle();
    nvn_dirty_remove_range(a->alias); nvn_dirty_remove_range(a->physical);
    mtl_pool_del(a->physical); /* also removes its donor-backed alias metadata */
    munmap(a->alias,a->bytes); munmap(a->physical,a->bytes);
}

typedef struct {
    dilation_fixture gpu;
    host_buffer source[3],output[3];
    uint32_t *input[3],*result[3],golden[3][144];
} chunk_compute;
static void chunk_compute_create(chunk_compute *c) {
    memset(c,0,sizeof *c);
    VkDescriptorSetLayoutBinding binding[2];
    for (unsigned i=0;i<2;i++) binding[i]=(VkDescriptorSetLayoutBinding){.binding=i,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.descriptorCount=1,
        .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo ds={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=2,.pBindings=binding};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev,&ds,NULL,&c->gpu.dsl));
    VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,96};
    VkPipelineLayoutCreateInfo layout={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&c->gpu.dsl,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    VKCHECK(vkCreatePipelineLayout(V.dev,&layout,NULL,&c->gpu.layout));
    VkShaderModuleCreateInfo module_info={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=sizeof framegen_authority_comp_spv,.pCode=framegen_authority_comp_spv};
    VkShaderModule module; VKCHECK(vkCreateShaderModule(V.dev,&module_info,NULL,&module));
    VkComputePipelineCreateInfo pipeline={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
            .module=module,.pName="main"},.layout=c->gpu.layout};
    VKCHECK(vkCreateComputePipelines(V.dev,V.pcache,1,&pipeline,NULL,&c->gpu.pipeline));
    vkDestroyShaderModule(V.dev,module,NULL);
    for (unsigned part=0;part<3;part++) {
        c->source[part]=host_create(sizeof c->golden[0],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        c->output[part]=host_create(sizeof c->golden[0],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        c->input[part]=host_map(&c->source[part]); c->result[part]=host_map(&c->output[part]);
    }
}
static void chunk_compute_reset(chunk_compute *c) {
    float depth=fg_test_depth(2); uint32_t depth_word; memcpy(&depth_word,&depth,sizeof depth_word);
    for (unsigned part=0;part<3;part++) {
        uint32_t *input=c->input[part],*result=c->result[part];
        memset(input,0,sizeof c->golden[0]); memset(result,0xa5,sizeof c->golden[0]);
        uint32_t header[16]={8,8,1,0,depth_word,depth_word,64};
        memcpy(input,header,sizeof header); memcpy(result,header,sizeof header);
        result[10]=result[11]=0; result[12]=4;
        unsigned ux=part==1?5:1,uy=part==2?5:1;
        for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) {
            /* Authority phase 1 consumes immutable ROW prefixes, not column
             * prefixes: exactly one unknown at (ux,uy) in each 8x8 source. */
            input[16+y*8+x]=y==uy && x>=ux;
            input[80+y*8+x]=depth_word;
        }
    }
}
static void chunk_compute_record(chunk_compute *c,unsigned part) {
    /* Reuse run_interp_dilation's production compute shader in the interrupted
     * guest primary. This is backend compute, not Android OP_DISPATCH decoding. */
    vk_end_rendering(); VkCommandBuffer cb=vk_cb();
    VkDescriptorSet set=vk_alloc_set(c->gpu.dsl);
    VkDescriptorBufferInfo buffers[2]={{c->source[part].buffer,0,c->source[part].size},
        {c->output[part].buffer,0,c->output[part].size}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i=0;i<2;i++) writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo=&buffers[i]};
    vkUpdateDescriptorSets(V.dev,2,writes,0,NULL);
    dilation_barrier(cb,VK_PIPELINE_STAGE_2_HOST_BIT,VK_ACCESS_2_HOST_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,c->gpu.pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,c->gpu.layout,0,1,&set,0,NULL);
    struct { uint32_t phase,w,h,reserved; float transform[16]; uint32_t rect[4]; } push={.w=8,.h=8};
    for (unsigned phase=1;phase<=2;phase++) {
        push.phase=phase;
        vkCmdPushConstants(cb,c->gpu.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof push,&push);
        vkCmdDispatch(cb,1,1,1);
        dilation_barrier(cb,VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            phase==1?VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT:VK_PIPELINE_STAGE_2_HOST_BIT,
            phase==1?VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT:VK_ACCESS_2_HOST_READ_BIT);
    }
}
static void chunk_compute_check(chunk_compute *c,unsigned enabled) {
    for (unsigned part=0;part<3;part++) {
        uint32_t *result=c->result[part]; unsigned unknown=0,known=0;
        for (unsigned i=0;i<64;i++) { unknown+=result[80+i]==0; known+=result[80+i]==1; }
        CHECK(unknown && known && unknown+known==64 && result[3]==unknown,
            "prefix and each resumed primary really dispatch changed compute SSBO bindings and publish nontrivial GPU masks");
        if (!enabled) memcpy(c->golden[part],result,sizeof c->golden[part]);
        else CHECK(!memcmp(c->golden[part],result,sizeof c->golden[part]),
            "all resumed compute result/header/scratch words match the flag-off actual GPU golden");
    }
    CHECK(memcmp(c->result[0]+80,c->result[1]+80,64*4) &&
        memcmp(c->result[1]+80,c->result[2]+80,64*4),
        "changed compute inputs produce independently different prefix and tail masks, not a stale descriptor replay");
}
static void chunk_compute_destroy(chunk_compute *c) {
    for (unsigned part=0;part<3;part++) {
        vkUnmapMemory(V.dev,c->source[part].memory); vkUnmapMemory(V.dev,c->output[part].memory);
        host_destroy(&c->source[part]); host_destroy(&c->output[part]);
    }
    vkDestroyPipeline(V.dev,c->gpu.pipeline,NULL); vkDestroyPipelineLayout(V.dev,c->gpu.layout,NULL);
    vkDestroyDescriptorSetLayout(V.dev,c->gpu.dsl,NULL);
}

typedef struct {
    vk_fg_generation *first,*second;
    vk_post_output output;
    uint64_t guest[8],first_ticket,ticket;
    unsigned calls,accepted,refused;
    int early,inside;
} chunk_probe;

static void chunk_context_equal(const uint64_t a[8],const uint64_t b[8],
        uint64_t games,const char *where) {
    CHECK(a[0]==b[0] && !memcmp(a+1,b+1,4*sizeof *a) && a[5]==games,
        "%s retains in-sub, guest timestamp/origin/frame, pending carry and game-slot attribution",where);
}
static void chunk_boundary(void *arg) {
    chunk_probe *p=arg;
    CHECK(!p->inside,"chunk callback never recursively reenters through child submission");
    if (p->inside) return;
    p->inside=1;
    uint64_t enter[8],leave[8]; vk_record_test_guest_context(enter);
    CHECK(!enter[0] && !enter[6] && !memcmp(enter+1,p->guest+1,4*sizeof *enter),
        "actual chunk callback is backend-only, with no open primary and unchanged immutable guest/carry context");
    p->calls++;
    uint64_t before=mtl_last_fence(),serial=vk_submit_serial();
    if (!p->early) {
        vk_fg_generation *g=p->accepted?p->second:p->first;
        int result=vk_fg_generation_dispatch(g,.375f,
            host_now_ns()+UINT64_C(10000000000),&p->output);
        CHECK(result>=0,"owned Vulkan bridge does not fabricate a quarantined external engine");
        if (result==1) {
            p->accepted++;
            p->ticket=vk_fg_generation_seal(g);
            uint64_t facts[6]; vk_fg_test_generation_facts(g,facts,NULL);
            p->first_ticket=facts[4];
            CHECK(p->accepted==1 && p->first_ticket==before+1 && p->ticket==before+2,
                "one actual immutable child consumes precisely signal/consumer tickets at this existing chunk");
            CHECK(!vk_fg_generation_dispatch(g,.5f,
                host_now_ns()+UINT64_C(10000000000),&p->output) && mtl_last_fence()==p->ticket,
                "same source cannot dispatch twice at one chunk");
            p->refused++;
            CHECK(!vk_fg_generation_dispatch(p->second,.625f,
                host_now_ns()+UINT64_C(10000000000),&p->output) && mtl_last_fence()==p->ticket,
                "distinct eligible source cannot duplicate the first child's current charge interval");
            p->refused++;
        } else {
            p->refused++;
            CHECK(mtl_last_fence()==before && vk_submit_serial()==serial,
                "negative child admission creates no ticket, split or open primary");
            CHECK(vk_fg_generation_state(g)!=VK_FG_GENERATION_RECORDING &&
                vk_fg_generation_state(g)!=VK_FG_GENERATION_SUBMITTED,
                "refusal leaves no hidden in-flight private graph or output claim");
        }
    }
    vk_record_test_guest_context(leave);
    chunk_context_equal(leave,enter,enter[5],"callback success/early refusal");
    CHECK(!leave[6],"success and every callback refusal leave no open primary");
    p->inside=0;
}
static void chunk_set_flag(int enabled) {
    setenv("TOTK_FG_ADAPTIVE_CHUNK",enabled?"1":"0",1);
    vk_record_test_chunk_selector_reset();
}
static void chunk_ticket(uint64_t ticket,const uint64_t guest[8],int backend,unsigned posts,int final_commit) {
    uint64_t v[5]={0};
    CHECK(vk_record_test_ticket_context(ticket,v),"actual FIFO ticket remains observable before ring reuse");
    CHECK(v[0]==(backend?0:guest[1]) && v[1]==(backend?0:guest[2]) &&
        v[2]==(backend?0:guest[3]) && v[3]==posts && v[4]==(unsigned)(backend || final_commit),
        "ticket %llu has %s guest attribution and exactly %u original post operations",
        (unsigned long long)ticket,backend?"zero child":"retained prefix/tail",posts);
}
static void chunk_budget(uint64_t frame,uint64_t first,uint64_t last,
        uint64_t child_first,uint64_t child_last) {
    double real=0,incremental=0;
    for (uint64_t ticket=first;ticket<=last;ticket++) {
        uint64_t row[4]={0}; double ms=NAN;
        int child=child_first && ticket>=child_first && ticket<=child_last;
        CHECK(vk_fg_test_budget_ticket(ticket,row,&ms) && row[0]==ticket && row[1]==1 &&
            row[2]==(unsigned)child && isfinite(ms) && ms>=0,
            "real timestamp row %llu classifies only exact child signal/consumer range as synthetic",
            (unsigned long long)ticket);
        if (child) incremental+=ms; else real+=ms;
    }
    uint64_t row[6]={0}; double costs[2]={NAN,NAN};
    CHECK(vk_fg_test_budget_frame(frame,row,costs) && row[0]==frame &&
        row[1]==first && row[2]==last && row[3]==FG_BUDGET_VALID &&
        row[4]==(child_first!=0) && fabs(costs[0]-real)<1e-8 &&
        fabs(costs[1]-incremental)<1e-8,
        "ordinary immutable frame aggregate counts mandatory prefix/tails once and exact child costs once");
    printf("chunk accounting: frame=%llu tickets=%llu..%llu child=%llu..%llu real_ms=%.6f incremental_ms=%.6f measured=1\n",
        (unsigned long long)frame,(unsigned long long)first,(unsigned long long)last,
        (unsigned long long)child_first,(unsigned long long)child_last,real,incremental);
}
static void chunk_draw(terminal_walker *f,const tex_t *target,unsigned part) {
    /* Separate consumed addresses prevent a CPU rewrite racing an earlier GPU
     * draw. The tail changes the real UBO, vertex/index bindings, viewport,
     * scissor and indexed geometry; no empty vk_cb continuation is evidence. */
    terminal_walker draw=*f;
    draw.vertex+=part*512; draw.index+=part*512; draw.uniform+=part*512;
    p_bind_vbuf vertex={.n=1,.r={{draw.vertex,sizeof terminal_vertices}}};
    vk_draw_exec(OP_BIND_VBUF,&vertex);
    const float tints[3][4]={{.8f,.3f,.6f,1},{.2f,.9f,.4f,1},{.9f,.4f,.1f,1}};
    terminal_walker_draw(&draw,target,1,part?320:0,part==2?180:0,320,part==2?180:360,tints[part]);
    if (part) {
        p_viewports vp={.n=1,.v={{part==1?80:160,part==1?24:60,480,288,0,1}}};
        p_scissor sc={part==1?400:80,part==1?40:220,120,100};
        vk_draw_exec(OP_VIEWPORTS,&vp); vk_draw_exec(OP_SCISSOR,&sc);
        p_draw_idx indexed={.prim=4,.itype=1,.count=3,.instances=1,.ibuf=draw.index};
        vk_draw_exec(OP_DRAW_IDX,&indexed);
    }
}
static void chunk_inputs(terminal_walker *f) {
    for (unsigned part=0;part<3;part++) {
        const float triangles[3][6]={{-1,-1,3,-1,-1,3},{-.8f,-.9f,.9f,-.8f,-.6f,.9f},
            {-1,-1,3,-1,-1,3}};
        const uint16_t indices[3][3]={{0,1,2},{2,1,0},{1,2,0}};
        memcpy((void *)(uintptr_t)(f->vertex+part*512),triangles[part],sizeof terminal_vertices);
        memcpy((void *)(uintptr_t)(f->index+part*512),indices[part],sizeof terminal_indices);
        const float cpu[4]={.125f,.125f,.125f,1};
        memcpy((void *)(uintptr_t)(f->uniform+part*512),cpu,sizeof cpu);
    }
}
static vk_fg_generation *chunk_prepare(terminal_walker *f,unsigned slot,unsigned *prepared) {
    terminal_walker_open(f,0,0);
    vk_fg_generation *g=terminal_prepare(slot,prepared);
    (void)terminal_walker_close();
    return g;
}
static uint64_t chunk_wait_held(uint64_t ticket) {
    uint64_t deadline=host_now_ns()+UINT64_C(10000000000);
    while (vk_record_test_completion_held()!=ticket && host_now_ns()<deadline) usleep(1000);
    CHECK(vk_record_test_completion_held()==ticket && !mtl_wait(ticket,0),
        "actual GPU completion reaches delayed ticket %llu without publishing the covering watermark",
        (unsigned long long)ticket);
    return ticket;
}
static void chunk_child_held(chunk_probe *p,terminal_snapshot *saved,unsigned lifecycle) {
    chunk_ticket(p->first_ticket,p->guest,1,0,0); chunk_ticket(p->ticket,p->guest,1,0,0);
    chunk_wait_held(p->ticket);
    CHECK(!vk_fg_generation_retire(p->first) && !vk_fg_complete(2),
        "delayed actual child consumer retains output/source lifetime");
    terminal_ownership(4,1,"delayed child plus second prepared interval retain exactly their source refs");
    terminal_snapshot_unchanged(p->first,saved);
    CHECK(!vk_fg_test_generation_bridge(0) && !vk_fg_test_terminal_owned(0),
        "shutdown/disable cannot detach submitted private storage");
    if (lifecycle) {
        vk_fg_generation_cancel(p->first); vk_fg_reset();
        vk_rec_lock(); vk_fg_history_cut(); vk_rec_unlock();
        CHECK(vk_fg_generation_state(p->first)==VK_FG_GENERATION_SUBMITTED,
            "surface/history loss cannot retire child resources before the real covering watermark");
    }
}
static void chunk_early_refusal(terminal_walker *f,const tex_t *target) {
    chunk_probe p={.early=1};
    chunk_set_flag(1); chunk_inputs(f);
    vk_gpu_time_guest_submit(host_now_ns(),0x1234,0x5678);
    mtl_sub_begin();
    chunk_draw(f,target,0);
    volatile uint32_t event=0; mtl_sub_event(&event,0,9);
    vk_record_test_guest_context(p.guest);
    uint64_t before=mtl_last_fence();
    vk_fg_test_slot_boundary_callback(chunk_boundary,&p);
    atomic_store_explicit(&vk_fg_slot_boundary_active,1,memory_order_relaxed);
    vk_submit_chunk();
    uint64_t after[8]; vk_record_test_guest_context(after);
    chunk_context_equal(after,p.guest,p.guest[5]+1,"early-refused actual chunk");
    CHECK(p.calls==1 && mtl_last_fence()==before+1 && !after[6] && !event,
        "early callback refusal restores guest walk without optional allocation or carry publication");
    atomic_store_explicit(&vk_fg_slot_boundary_active,0,memory_order_relaxed);
    vk_fg_test_slot_boundary_callback(NULL,NULL);
    chunk_draw(f,target,1);
    uint64_t final=mtl_sub_commit();
    CHECK(mtl_wait(final,UINT64_C(10000000000)) && event==9,
        "early-refused chunk resumes real changed guest drawing and publishes only at original final commit");
    vk_native_frame_end(); vk_canvas_frame_end();
}
static int run_prepared_chunk(int adaptive) {
    const char *zero[]={"TOTK_UNCAPPED","TOTK_FPS_AUTO","TOTK_ASYNC_REPLAY","TOTK_ACQUIRE_SLACK",
        "TOTK_ACQUIRE_CLOCK","TOTK_PACE","TOTK_MFX","DM_DETCLOCK","TOTK_VK_COMPILE_THREADS",
        "TOTK_VK_CHUNK_DRAWS","TOTK_VK_UB_RENAME"};
    for (unsigned i=0;i<sizeof zero/sizeof zero[0];i++) setenv(zero[i],"0",1);
    setenv("TOTK_FRAMEGEN",adaptive?"adaptive":"interp",1);
    setenv("TOTK_FRAMEGEN_ENGINE","qcom",1); setenv("TOTK_FRAMEGEN_JOB_TRACE","1",1);
    setenv("TOTK_FPS60",adaptive?"1":"0",1); setenv("TOTK_REALTIME",adaptive?"1":"0",1);
    setenv("TOTK_NATIVE_UI","1",1); setenv("TOTK_PRESENT_SLOTS","1",1);
    setenv("TOTK_FG_ADAPTIVE_SLOTS","1",1); setenv("TOTK_PRESENT_PREROTATE","1",1);
    setenv("TOTK_FG_ADAPTIVE_BACKPRESSURE","request",1);
    setenv("DM_PRESENT_INTERVAL","game",1); setenv("DM_PRESENT_HZ","60",1);
    setenv("TOTK_POST_AA","off",1); setenv("TOTK_POST_SCALE","off",1);
    setenv("TOTK_SCENE_AA","off",1); setenv("TOTK_SCENE_SCALE","off",1);
    if (adaptive) unsetenv("TOTK_FRAMEGEN_BUDGET_MS");
    else setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1);
    /* A separate parent matrix may select protected lag. Do not fake lag
     * authority on hosts/configurations where it was not admitted. */
    W=DW=640; H=DH=360;
    mtl_init();
    CHECK(vk_fg_enabled() && vk_native_ui_enabled() &&
        (adaptive?vk_fg_adaptive():vk_fg_interpolating()),"chunk fixture starts its declared real backend mode");
    CHECK(vk_fg_test_generation_bridge(1) && vk_fg_test_terminal_owned(1),
        "only unavailable host AFME/GL bridge is substituted; terminal storage is immutable");
    vk_fg_test_terminal_geometry_freeze(1);
    terminal_walker f; terminal_walker_create(&f,1);
    tex_t target; texture_desc(&target,&f.pool,20u<<20,0x25,82701,640,360);
    unsigned prepared=0,accepted=0,admission_refused=0;
    const char *control_env=getenv("TOTK_TEST_CHUNK_LEGACY_CONTROL");
    int legacy_control=control_env && !strcmp(control_env,"1");
    for (unsigned frame=0;frame<8;frame++) {
        terminal_walker_open(&f,0,0);
        (void)terminal_walker_close();
    }
    if (!legacy_control) chunk_early_refusal(&f,&target);
    int alias_proof=vk_lag_uniform_private();
    chunk_cpu_alias alias={0};
    if (alias_proof) alias=chunk_alias_create();
    else printf("chunk CPU alias publication: unavailable=requires admitted TOTK_FRAME_LAG=1,TOTK_VK_LAG_CAPTURE=1,TOTK_VK_UB_RENAME=0; ordinary event/counter frontier still exercised\n");
    chunk_compute compute; chunk_compute_create(&compute);
    VkEventCreateInfo event_info={VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    const size_t bytes=(size_t)640*360*4;
    const size_t child_bytes=(size_t)960*540*4;
    uint8_t *golden=malloc(bytes),*pixels=malloc(bytes),*child_pixels=malloc(child_bytes);
    CHECK(golden && pixels && child_pixels,"allocate independent resumed-guest and immutable-child GPU readbacks");
    if (!golden || !pixels || !child_pixels) exit(1);
    chunk_alias_snapshot alias_golden[4]={{0}};
    for (unsigned lifecycle=0;lifecycle<2;lifecycle++) for (unsigned arm=0;arm<2;arm++) {
        unsigned enabled=arm && !legacy_control;
        chunk_set_flag(enabled);
        /* The accounting-only scratch interval below has no captured present;
         * frame_end correctly clears its source history. Mature history with
         * a genuine extracted source before preparing the next source pair. */
        terminal_walker_open(&f,0,0); (void)terminal_walker_close();
        chunk_probe probe={0};
        probe.first=chunk_prepare(&f,2,&prepared);
        probe.second=chunk_prepare(&f,3,&prepared);
        CHECK(probe.first && probe.second,"two distinct actual source templates reach the same chunk charge interval");
        if (!probe.first || !probe.second) {
            vk_rec_lock(); vk_fg_generation_cancel(probe.first); vk_fg_generation_cancel(probe.second); vk_rec_unlock();
            continue;
        }
        terminal_snapshot saved; terminal_snapshot_save(probe.first,&saved);
        chunk_inputs(&f);
        chunk_compute_reset(&compute);
        const float alias_initial[4]={.125f,.125f,.125f,1};
        const float alias_value[4]={.25f,.75f,.5f,1};
        if (alias_proof) {
            dm_guest_write_prepare(alias.physical,sizeof alias_initial);
            memcpy(alias.physical,alias_initial,sizeof alias_initial);
        }
        volatile uint32_t event=0;
        uint64_t report[2]={UINT64_C(0xaabbccddeeff0011),UINT64_C(0x1122334455667788)};
        const uint64_t report_initial[2]={report[0],report[1]};
        uint64_t first=mtl_last_fence()+1,serial=vk_submit_serial();
        vk_gpu_time_guest_submit(host_now_ns(),UINT64_C(0x12345000),UINT64_C(0x76540000)+lifecycle);
        mtl_sub_begin();
        vk_record_test_guest_context(probe.guest);
        CHECK(probe.guest[0] && probe.guest[1] && probe.guest[2]==UINT64_C(0x12345000) &&
            probe.guest[3]==UINT64_C(0x76540000)+lifecycle,"mandatory walk has nonzero immutable guest attribution");
        p_set_rt rt={.n=1,.color={&target}}; vk_draw_exec(OP_SET_RT,&rt);
        p_scissor all={0,0,640,360}; vk_draw_exec(OP_SCISSOR,&all);
        p_clear_color clear={0,15,{.03125f,.0625f,.125f,1}}; vk_draw_exec(OP_CLEAR_COLOR,&clear);
        chunk_draw(&f,&target,0);
        chunk_compute_record(&compute,0);
        mtl_sub_event(&event,0,0x5a);
        mtl_sub_counter((uintptr_t)report,NVN_COUNTER_TYPE_TIMESTAMP);
        if (alias_proof) {
            terminal_inline((uintptr_t)alias.alias,alias_value);
            chunk_alias_observe(&alias,alias_value,alias_golden,0,enabled);
        }
        vk_record_test_guest_context(probe.guest);
        CHECK(probe.guest[4]==2,"event and counter are real pending final-commit carry");
        vk_fg_test_slot_boundary_callback(chunk_boundary,&probe);
        atomic_store_explicit(&vk_fg_slot_boundary_active,1,memory_order_relaxed);
        /* Stop the child covering watermark, not queue submission or a fake
         * generation state. Ordinary prefix work must still really finish. */
        if (enabled) vk_record_test_hold_completion(first+2);
        vk_submit_chunk();
        uint64_t after[8]; vk_record_test_guest_context(after);
        chunk_context_equal(after,probe.guest,probe.guest[5]+1,"resuming mandatory prefix");
        CHECK(!after[6] && probe.calls==enabled && mtl_last_fence()==first+(probe.accepted?2:0),
            "flag alone controls callback; ordinary prefix seals once without creating a replacement primary");
        chunk_ticket(first,probe.guest,0,0,0);
        CHECK(mtl_wait(first,UINT64_C(10000000000)) && !event && !memcmp(report,report_initial,sizeof report),
            "GPU-completed prefix/optional child cannot publish guest event or counter carry early");
        uint64_t logical=UINT64_MAX;
        CHECK(vk_counter_value((uintptr_t)report,&logical) && !logical,
            "pending report retains ordered CPU frontier rather than reading its stale memory sentinel");
        unsigned callbacks=probe.calls;
        uint64_t reserved=mtl_last_fence();
        vk_submit_chunk();
        CHECK(probe.calls==callbacks && mtl_last_fence()==reserved,
            "no-open-primary chunk is a true no-op with no callback or forced empty split");
        if (alias_proof) chunk_alias_observe(&alias,alias_value,alias_golden,1,enabled);
        unsigned first_accepted=probe.accepted;
        if (first_accepted) chunk_child_held(&probe,&saved,lifecycle);
        else vk_record_test_hold_completion(0);
        chunk_draw(&f,&target,1);
        chunk_compute_record(&compute,1);
        uint64_t tail=mtl_last_fence()+1;
        if (enabled && !probe.accepted) vk_record_test_hold_completion(tail+2);
        vk_submit_chunk();
        chunk_ticket(tail,probe.guest,0,0,0);
        vk_record_test_guest_context(after);
        chunk_context_equal(after,probe.guest,probe.guest[5]+2,"resuming changed indexed tail");
        CHECK(probe.calls==(enabled?2u:0u) && probe.accepted<=1 &&
            mtl_last_fence()==first+(probe.accepted?3:1) && !event && !memcmp(report,report_initial,sizeof report),
            "multiple real chunks preserve pending posts and cannot duplicate source or charge interval");
        if (!first_accepted && probe.accepted) chunk_child_held(&probe,&saved,lifecycle);
        if (!probe.accepted) vk_record_test_hold_completion(0);
        if (probe.accepted) terminal_snapshot_unchanged(probe.first,&saved);
        if (alias_proof) chunk_alias_observe(&alias,alias_value,alias_golden,2,enabled);
        chunk_draw(&f,&target,2);
        chunk_compute_record(&compute,2);
        uint64_t final_expected=mtl_last_fence()+1;
        VkEvent final_gate=VK_NULL_HANDLE;
        vk_record_test_hold_completion(0); /* release any delayed child */
        if (!alias_proof) {
            /* g_done's TEST hold is AFTER apply_posts; it cannot gate post
             * delivery. Reuse the real GPU VkEvent gate from test_vkreplaypin
             * before this final primary completes instead. Protected final
             * publication must remain free to wait on genuine alias fences. */
            VKCHECK(vkCreateEvent(V.dev,&event_info,NULL,&final_gate));
            vkCmdWaitEvents(vk_cb(),1,&final_gate,VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,NULL,0,NULL,0,NULL);
        }
        uint64_t charge=vk_fg_real_frame_id();
        /* This host callback stands in for presenter selection only. Remove
         * it before the unchanged begin/commit boundary, so all observed calls
         * are caused by the experiment's existing chunk seal hook. */
        atomic_store_explicit(&vk_fg_slot_boundary_active,0,memory_order_relaxed);
        vk_fg_test_slot_boundary_callback(NULL,NULL);
        uint64_t final=mtl_sub_commit();
        vk_fg_frame_end(final);
        CHECK(final==final_expected && vk_submit_serial()==serial+3+(probe.accepted?2:0),
            "resumed nonempty final commit has exactly original three real chunks plus the optional two-ticket child");
        if (!alias_proof) chunk_ticket(final,probe.guest,0,2,1);
        vk_record_test_guest_context(after);
        CHECK(!after[0] && !after[4] && after[5]==probe.guest[5]+3,
            "original final commit alone consumes carry; child never increments game-slot count");
        if (alias_proof) CHECK(!vk_ub_pending((uintptr_t)alias.physical,sizeof alias_value) &&
            !vk_ub_pending((uintptr_t)alias.alias,sizeof alias_value) &&
            !memcmp(alias.physical,alias_value,sizeof alias_value) &&
            !memcmp(alias.alias,alias_value,sizeof alias_value),
            "original final commit leaves both CPU aliases fully published with the correct final bytes");
        if (alias_proof) chunk_alias_observe(&alias,alias_value,alias_golden,3,enabled);
        if (!alias_proof) {
            CHECK(!mtl_wait(final,0) && !event && !memcmp(report,report_initial,sizeof report),
                "event/counter stores remain held at the original final GPU/post completion frontier");
            VKCHECK(vkSetEvent(V.dev,final_gate));
        }
        vk_record_test_hold_completion(0);
        CHECK(mtl_wait(final,UINT64_C(10000000000)) && event==0x5a &&
            memcmp(report,report_initial,sizeof report),"original final completion publishes actual pending event and timestamp");
        if (alias_proof) chunk_ticket(final,probe.guest,0,0,1); /* retired posts, original final join */
        if (final_gate) vkDestroyEvent(V.dev,final_gate,NULL);
        if (adaptive) chunk_budget(charge,first,final,probe.first_ticket,probe.ticket);
        chunk_compute_check(&compute,enabled);
        read_image_extent(vk_tex_image(vk_tex_get(&target)),VK_IMAGE_ASPECT_COLOR_BIT,pixels,bytes,640,360);
        size_t different=0;
        for (size_t i=4;i<bytes;i+=4) different+=memcmp(pixels,pixels+i,4)!=0;
        CHECK(different>1000 && pixels[3]==255,"resumed guest golden is nontrivial real indexed GPU output, not empty/constant work");
        const uint8_t *prefix_pixel=pixels+4*(50*640+50);
        /* NVN dynamic_state uses a negative-height Vulkan viewport. This
         * point lies inside the mirrored changed triangle, above final tail's
         * y=180 scissor, so the later tail cannot repaint its proof pixel. */
        const uint8_t *tail_pixel=pixels+4*(160*640+340);
        const uint8_t *final_pixel=pixels+4*(300*640+500);
        CHECK(prefix_pixel[2]<=1 && tail_pixel[2]<=1 && final_pixel[2]<=1 &&
            prefix_pixel[3]==255 && tail_pixel[3]==255 && final_pixel[3]==255 &&
            (prefix_pixel[0]>=200 || prefix_pixel[1]>=72) &&
            (tail_pixel[0]>=45 || tail_pixel[1]>=220) &&
            (final_pixel[0]>=225 || final_pixel[1]>=95) &&
            memcmp(prefix_pixel,tail_pixel,3) && memcmp(tail_pixel,final_pixel,3),
            "prefix and BOTH resumed indexed guest tails independently paint their changed readonly-UBO colors outside the clear golden");
        if (!enabled) memcpy(golden,pixels,bytes);
        else CHECK(!memcmp(golden,pixels,bytes),
            "actual prefix/changed UBO+vertex+index+dynamic-state tails match every flag-off GPU golden byte");
        if (probe.accepted) {
            accepted++;
            read_image_extent(probe.output.image,VK_IMAGE_ASPECT_COLOR_BIT,child_pixels,child_bytes,960,540);
            CHECK(abs((int)child_pixels[0]-24)<=2 && abs((int)child_pixels[1]-32)<=2 &&
                abs((int)child_pixels[2]-128)<=2 && child_pixels[3]==255,
                "actual immutable child GPU output consumes captured indexed scene and ordered readonly UBO tint");
            size_t varied=0;
            for (size_t i=4;i<child_bytes;i+=4) varied+=memcmp(child_pixels,child_pixels+i,4)!=0;
            CHECK(varied>1000,"immutable child readback contains the actual nonconstant captured scene/HUD");
            CHECK(vk_fg_generation_retire(probe.first) && !vk_fg_complete(2),
                "covered child retires exactly once even after disable/surface cancellation");
        }
        admission_refused+=probe.refused;
        vk_rec_lock(); vk_fg_generation_cancel(probe.first); vk_fg_generation_cancel(probe.second); vk_rec_unlock();
        terminal_ownership(0,0,"chunk/cancel cleanup returns all actual source and output references");
        free(saved.bytes); free(saved.scratch);
        vk_native_frame_end(); vk_canvas_frame_end();
        if (lifecycle && enabled) {
            terminal_walker_open(&f,0,0); (void)terminal_walker_close();
        }
    }
    CHECK(legacy_control ? accepted==0 : adaptive || accepted==2,
        "legacy control never enters chunk dispatch; positive ownership cases dispatch both children");
    CHECK(vk_fg_test_terminal_owned(0) && vk_fg_test_generation_bridge(0),
        "drained shutdown releases immutable capture and TEST bridge");
    if (alias_proof) chunk_alias_destroy(&alias);
    chunk_compute_destroy(&compute);
    mtl_tex_release(&target); terminal_walker_destroy(&f); free(golden); free(pixels); free(child_pixels);
    printf("chunk-hook: mode=%s prepared=%u dispatch=%u refusal=%u cpu_alias=%d checks=%u failures=%u; actual recorder/OP-indexed resumed-guest/backend-compute/GPU-golden/carry/tickets; host callback substitutes Android presenter selection, no AFME/device/presenter-policy claim\n",
        legacy_control?"legacy-boundary-control":adaptive?"adaptive-measured":"fixed-override-ownership",
        prepared,accepted,admission_refused,alias_proof,checks,failures);
    return failures?1:0;
}
