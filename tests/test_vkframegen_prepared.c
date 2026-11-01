/* Actual private Vulkan command/descriptor/source ownership. Reuse the owned
 * native-UI fixture; substitute ONLY AFME pixels and its external GL bridge.
 * This establishes storage/FIFO lifetime, not AFME/device quality or speed.
 * The explicit software-GPU fixed override below is NOT evidence for adaptive
 * predictive admission or the post-hoc 30 ms actual aggregate audit. Those
 * scalar contracts are exercised in test_framegen_qcom_prepared.c. */
#define main framegen_fixture_main
#include "test_vkframegen.c"
#undef main

enum {
    PREPARED_CAPTURE_OK, PREPARED_CAPTURE_UNSUPPORTED, PREPARED_CAPTURE_WRITABLE,
    PREPARED_CAPTURE_UNKNOWN_BUFFER, PREPARED_CAPTURE_UNKNOWN_IMAGE,
    PREPARED_CAPTURE_BUFFER_CAP, PREPARED_CAPTURE_DESCRIPTOR_CAP
};
static struct {
    VkPipelineLayout layout;
    VkPipeline pipeline;
    int payload, churn, refusal;
    uint32_t value;
    VkDescriptorBufferInfo uniform, vertex, index, churn_range;
    VkDescriptorSet borrowed[2];
    void *vertex_mapping, *index_mapping;
    uint32_t uniform_words[16], indices[3];
    float vertices[6];
} prepared_owned;

static void prepared_terminal_capture(fixture *f,vk_record_draw *terminal,VkDescriptorSet sets[2]) {
    if (!atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed)) return;
    CHECK(vk_fg_terminal_owned_begin(),"owned terminal starts at the actual terminal freeze point");
    VkDescriptorImageInfo image={f->ui_sampler,vk_fg_canvas_view(),VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[0],
        .dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo=&image};
    CHECK(vk_fg_terminal_owned_binding(0,f->transfer_dsl,&write,0),
        "owned capture uses the fixture's actual canvas view, sampler, layout and image layout");
    if (prepared_owned.payload) {
        terminal->pipeline=prepared_owned.pipeline; terminal->layout=prepared_owned.layout;
        /* This otherwise-unused set exercises real descriptor payload ownership;
         * the existing transfer shaders/golden continue to sample the canvas.
         * Indexed 0/1/2 is consumed by that shader's gl_VertexIndex, so corrupting
         * the borrowed index bytes also has an observable GPU pixel consequence. */
        vk_record_test_snapshot_allocate_range(++prepared_owned.value,
            &prepared_owned.uniform.buffer,&prepared_owned.uniform.offset);
        prepared_owned.uniform.range=sizeof prepared_owned.uniform_words;
        for (unsigned i=0;i<16;i++) prepared_owned.uniform_words[i]=prepared_owned.value+i;
        sets[1]=vk_alloc_set(f->ui_dsl);
        write=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=sets[1],
            .dstBinding=0,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo=&prepared_owned.uniform};
        vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
        CHECK(vk_fg_terminal_owned_binding(1,f->ui_dsl,&write,0),
            "owned terminal copies a written managed snapshot UBO into its private descriptor set");
        const float vertices[6]={-1,-1,3,-1,-1,3};
        const uint32_t indices[3]={0,1,2};
        memcpy(prepared_owned.vertices,vertices,sizeof vertices);
        memcpy(prepared_owned.indices,indices,sizeof indices);
        prepared_owned.vertex_mapping=vk_transient(sizeof vertices,16,
            &prepared_owned.vertex.buffer,&prepared_owned.vertex.offset);
        prepared_owned.index_mapping=vk_transient(sizeof indices,16,
            &prepared_owned.index.buffer,&prepared_owned.index.offset);
        memcpy(prepared_owned.vertex_mapping,vertices,sizeof vertices);
        memcpy(prepared_owned.index_mapping,indices,sizeof indices);
        prepared_owned.vertex.range=sizeof vertices; prepared_owned.index.range=sizeof indices;
        terminal->vertex_mask=1;
        terminal->vertex[0].buffer=prepared_owned.vertex.buffer;
        terminal->vertex[0].offset=prepared_owned.vertex.offset;
        terminal->vertex[0].size=sizeof vertices; terminal->vertex[0].stride=2*sizeof(float);
        terminal->index=prepared_owned.index.buffer; terminal->index_offset=prepared_owned.index.offset;
        terminal->index_type=VK_INDEX_TYPE_UINT32;
        memcpy(prepared_owned.borrowed,sets,sizeof prepared_owned.borrowed);
    }
    if (prepared_owned.refusal) {
        VkDescriptorBufferInfo buffer=prepared_owned.uniform;
        write=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstBinding=0,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
        unsigned set=1; VkDescriptorSetLayout layout=f->ui_dsl; int writable=0;
        switch (prepared_owned.refusal) {
        case PREPARED_CAPTURE_UNSUPPORTED:write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;break;
        case PREPARED_CAPTURE_WRITABLE:write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writable=1;break;
        case PREPARED_CAPTURE_UNKNOWN_BUFFER:
            buffer=(VkDescriptorBufferInfo){f->ui_uniform.buffer,0,f->ui_uniform.size};break;
        case PREPARED_CAPTURE_UNKNOWN_IMAGE:
            set=0; layout=f->transfer_dsl; image.imageView=f->final_view;
            write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo=&image;break;
        case PREPARED_CAPTURE_BUFFER_CAP:buffer.range=(VkDeviceSize)VK_FG_TERMINAL_BYTES+1;break;
        case PREPARED_CAPTURE_DESCRIPTOR_CAP:write.descriptorCount=VK_FG_TERMINAL_DESCRIPTORS+1;break;
        }
        CHECK(!vk_fg_terminal_owned_binding(set,layout,&write,writable),
            "unsupported/writable/unknown/over-cap terminal payload refuses optional capture");
        if (prepared_owned.refusal==PREPARED_CAPTURE_UNKNOWN_BUFFER) {
            uint64_t facts[6]; vk_fg_terminal_input failure={0};
            CHECK(vk_fg_test_terminal_current(facts,&failure) && facts[2] &&
                failure.role==VK_FG_TERMINAL_INPUT_DESCRIPTOR && failure.stage==set &&
                failure.set==set && failure.binding==write.dstBinding && !failure.element &&
                failure.descriptor_type==write.descriptorType && failure.buffer==buffer.buffer &&
                failure.offset==buffer.offset && failure.bytes==buffer.range && !failure.freeze,
                "first unowned descriptor refusal identifies exact stage/set/binding/type/buffer/range");
        }
    }
}

static uint64_t prepared_real(fixture *f,const float camera[84],unsigned slot,
        int want_template,int cancel_open,vk_fg_generation **generation) {
    *generation=NULL;
    mtl_sub_begin();
    VkCommandBuffer cb=vk_cb();
    if (prepared_owned.churn && !want_template) {
        vk_record_test_snapshot_allocate_range(++prepared_owned.value,
            &prepared_owned.churn_range.buffer,&prepared_owned.churn_range.offset);
        prepared_owned.churn_range.range=64;
    }
    vk_fg_camera(camera);
    CHECK(vk_fg_capture(cb,f->scene,0,0,f->depth,0,0,W,H),"prepared fixture captures actual source images");
    int spatial=native_ui_terminal && vk_scene_post_enabled();
    int canvas=vk_fg_prepare_canvas(cb,spatial?native_terminal_w:W,spatial?native_terminal_h:H);
    int replayed=fixture_render_replay(f,cb,canvas);
    if (want_template) CHECK(replayed,"template acceptance/refusal exercises an eligible real source pair");
    if (want_template && replayed) {
        vk_record_draw terminal={.pipeline=f->transfer_pipeline,.layout=f->transfer_layout,.count=3,.instances=1};
        terminal.dynamic.vp=(VkViewport){0,0,native_terminal_w,native_terminal_h,0,1};
        terminal.dynamic.sc=(VkRect2D){{0,0},{native_terminal_w,native_terminal_h}};
        terminal.dynamic.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkDescriptorSet sets[2]={replay_image_set(f,vk_fg_canvas_view()),0};
        const char *legacy_vertex=getenv("TOTK_TEST_LEGACY_PREPARED");
        if (legacy_vertex && !strcmp(legacy_vertex,"vertex")) {
            /* Lavapipe LSan control: original borrowed lease path, with only
             * an unconsumed transient vertex binding and no owned capture. */
            VkDeviceSize vertex_offset; VkBuffer vertex_buffer;
            float *vertices=vk_transient(6*sizeof *vertices,16,&vertex_buffer,&vertex_offset);
            const float values[6]={-1,-1,3,-1,-1,3}; memcpy(vertices,values,sizeof values);
            terminal.vertex_mask=1; terminal.vertex[0].buffer=vertex_buffer;
            terminal.vertex[0].offset=vertex_offset; terminal.vertex[0].size=sizeof values;
            terminal.vertex[0].stride=2*sizeof(float);
        }
        prepared_terminal_capture(f,&terminal,sets);
        vk_fg_adaptive_terminal(&terminal,sets,native_terminal_w,native_terminal_h,VK_FORMAT_R8G8B8A8_UNORM);
        vk_fg_generation_stamp stamp={vk_fg_real_frame_id(),1};
        uint64_t reserved=mtl_last_fence(),serial=vk_submit_serial();
        uint64_t passes[4],after[4]; vk_fg_test_pass_counts(passes);
        uint64_t previous_ownership[2]; vk_fg_test_job_ownership(previous_ownership);
        vk_fg_generation *g=vk_fg_generation_prepare(slot,native_terminal_w,native_terminal_h,0,&stamp);
        if (want_template==2) {
            CHECK(!g,"unowned terminal payload refuses OPTIONAL generation, not the real frame");
        } else {
            CHECK(g && vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARING,"prepare owns a real private recording");
            if (!g) { (void)mtl_sub_commit(); exit(1); }
            CHECK(!vk_fg_generation_prepare(slot+1,native_terminal_w,native_terminal_h,0,&stamp),
                "one native-UI descriptor lease explicitly refuses a second child");
        }
        vk_fg_test_pass_counts(after);
        CHECK(!memcmp(passes,after,sizeof passes),"record-only preparation does not consume real-stream pass state");
        CHECK(mtl_last_fence()==reserved && vk_submit_serial()==serial,
            "preparation/refusal allocates no FIFO ticket or hidden recorder split");
        uint64_t owned[2]; vk_fg_test_job_ownership(owned);
        CHECK(owned[0]==previous_ownership[0]+(g?2:0) && owned[1]==previous_ownership[1],
            "preparation adds exactly its sources; refused capture owns no sources/output");
        if (g) {
            vk_post_output output;
            CHECK(vk_fg_generation_output(g,&output) && output.image && output.view,
                "private generation exposes immutable output metadata");
            VkCommandBuffer private_cb=vk_fg_generation_prepare_cb(g);
            CHECK(private_cb && private_cb!=cb,"private command storage is not the real primary");
            vk_fg_output_copy_begin(private_cb,slot);
            if (cancel_open) {
                vk_fg_generation_cancel(g);
                CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY,"unsubmitted open template cancels safely");
            } else {
                CHECK(vk_fg_generation_prepare_finish(g),"complete private command stream closes before real seal");
                CHECK(!vk_fg_generation_prepare_cb(g),"closed private commands cannot be appended again");
                CHECK(!vk_fg_generation_dispatch(g,0.375f,host_now_ns()+UINT64_C(1000000000),&output),
                    "dispatch refuses an open real recorder without splitting it");
                CHECK(mtl_last_fence()==reserved,"open-recorder refusal creates no ticket");
            }
        }
        *generation=g;
    }
    uint64_t real=mtl_sub_commit();
    vk_fg_frame_end(real);
    CHECK(mtl_wait(real,UINT64_C(10000000000)),"source real covering ticket retires");
    return real;
}

static void ownership(uint64_t refs,uint64_t busy,const char *label) {
    uint64_t owned[2]; vk_fg_test_job_ownership(owned);
    CHECK(owned[0]==refs && owned[1]==busy,"%s: refs=%llu/%llu outputs=%llu/%llu",label,
        (unsigned long long)owned[0],(unsigned long long)refs,
        (unsigned long long)owned[1],(unsigned long long)busy);
}

static void snapshot_pressure(fixture *f,const float camera[84],unsigned slot) {
    CHECK(!atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed),
        "legacy snapshot-pressure regressions explicitly exercise borrowed capture");
    /* Existing ownership tests never allocated replay-UBO snapshot arenas.
     * Populate ALL four: a prepared lease conservatively retains every one. */
    uint32_t *frozen[4];
    for (unsigned i=0;i<4;i++) {
        mtl_sub_begin(); (void)vk_cb();
        frozen[i]=vk_record_test_snapshot_allocate(UINT32_C(0xabc000)+i*32);
        uint64_t ticket=mtl_sub_commit();
        CHECK(mtl_wait(ticket,UINT64_C(10000000000)),"snapshot warmup real ticket retires");
    }
    int negative=getenv("TOTK_TEST_SNAPSHOT_PRESSURE_NEGATIVE")!=NULL;
    for (unsigned open=0;open<2;open++) {
        vk_fg_generation *g=NULL,*other=NULL;
        (void)prepared_real(f,camera,slot,1,0,&g);
        if (open) (void)prepared_real(f,camera,slot+1,1,0,&other);
        CHECK(g && (!open || other),"unpublished prepared templates own actual replay leases");
        vk_rec_lock();
        CHECK(vk_record_test_snapshot_next_owner()==UINT64_MAX,
            "all four allocated snapshot arenas are retained by the open template lease");
        atomic_store_explicit(&vk_fg_generation_snapshot_retained,!negative,memory_order_relaxed);
        vk_rec_unlock();
        mtl_sub_begin();
        if (open) (void)vk_cb(); /* cancellation must defer this primary's release */
        uint64_t before=mtl_last_fence(),serial=vk_submit_serial();
        alarm(10); /* Omitted-gate control hits the no-sentinel-wait invariant. */
        uint32_t *fresh=vk_record_test_snapshot_allocate(UINT32_C(0xdef000)+open*32);
        alarm(0);
        CHECK(!negative,"omitting the scheduler pressure gate cannot return from the old wait cycle");
        CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY &&
            (!other || vk_fg_generation_state(other)==VK_FG_GENERATION_EMPTY),
            "snapshot pressure cancels every unsubmitted template, not mandatory production");
        CHECK(mtl_last_fence()==before && vk_submit_serial()==serial,
            "descriptor capture never seals/splits its caller's cached primary");
        if (open) {
            CHECK(vk_record_test_snapshot_next_owner()==UINT64_MAX,
                "open primary retains deferred lease authority until its ordinary seal");
            for (unsigned i=0;i<4;i++) for (unsigned k=0;k<16;k++)
                CHECK(frozen[i][k]==UINT32_C(0xabc000)+i*32+k,
                    "append-only pressure allocation never resets retained snapshot bytes");
        }
        CHECK(fresh[0]==UINT32_C(0xdef000)+open*32,"mandatory fresh snapshot allocation progresses");
        uint64_t ticket=mtl_sub_commit();
        CHECK(mtl_wait(ticket,UINT64_C(10000000000)),"ordinary seal resolves pending lease release");
        CHECK(vk_record_test_snapshot_next_owner()!=UINT64_MAX,
            "no canceled open sentinel remains after the real covering ticket");
        ownership(0,0,"pressure cancellation returns only unsubmitted source/output references");
        atomic_store_explicit(&vk_fg_slot_boundary_active,0,memory_order_relaxed);
        /* Restore known frozen values before the next all-arena lease. */
        for (unsigned i=0;i<4;i++) {
            mtl_sub_begin(); (void)vk_cb();
            frozen[i]=vk_record_test_snapshot_allocate(UINT32_C(0xabc000)+i*32);
            uint64_t refill=mtl_sub_commit();
            CHECK(mtl_wait(refill,UINT64_C(10000000000)),"flag-off finite snapshot reuse remains live");
        }
    }
    unsigned covered[3]={0};
    for (unsigned kind=0;kind<3;kind++) for (unsigned round=0;round<4;round++) {
        /* Fresh cancellation, cancellation BEFORE entry, and a quarantined
         * graph whose presenter entry/gate has already been drained. */
        for (unsigned i=0;i<4;i++) {
            mtl_sub_begin(); (void)vk_cb();
            frozen[i]=vk_record_test_snapshot_allocate(UINT32_C(0xabc000)+i*32);
            CHECK(mtl_wait(mtl_sub_commit(),UINT64_C(10000000000)),"all-arena fixture refills finite storage");
        }
        vk_fg_generation *g=NULL;
        (void)prepared_real(f,camera,slot,1,0,&g);
        CHECK(g,"pressure state fixture has a closed unsubmitted actual graph");
        atomic_store_explicit(&vk_fg_slot_boundary_active,0,memory_order_relaxed);
        if (kind==2) {
            vk_rec_lock();
            CHECK(vk_fg_test_generation_quarantine(g,1),"inject quarantine WITHOUT any GL/GPU submission");
            vk_rec_unlock();
        }
        for (unsigned wrap=0;wrap<(kind==2?12u:1u);wrap++) {
            mtl_sub_begin(); (void)vk_cb();
            if (kind==1) {
                vk_fg_generation_cancel(g);
                CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY,
                    "early cancellation has no newly cancellable template left");
            }
            CHECK(vk_record_test_snapshot_next_owner()==UINT64_MAX,
                "already-pending/quarantined owner remains open independently of queue gate");
            unsigned arena=vk_record_test_snapshot_next_index();
            covered[kind]|=1u<<arena;
            uint64_t before=mtl_last_fence(),serial=vk_submit_serial();
            alarm(10);
            (void)vk_record_test_snapshot_allocate(UINT32_C(0x123000)+wrap*32);
            alarm(0);
            CHECK(mtl_last_fence()==before && vk_submit_serial()==serial,
                "all pressure states preserve the caller's primary and ticket ordering");
            for (unsigned i=0;i<4;i++) for (unsigned k=0;k<16;k++)
                CHECK(frozen[i][k]==UINT32_C(0xabc000)+i*32+k,
                    "each pressured arena keeps every frozen prefix across ring wrap");
            CHECK(kind==2 ? vk_fg_generation_state(g)==VK_FG_GENERATION_QUARANTINED :
                vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY,
                "pressure never releases a quarantined graph or leaves a fresh template open");
            uint64_t ticket=mtl_sub_commit();
            CHECK(mtl_wait(ticket,UINT64_C(10000000000)),"mandatory producer progresses with retained ownership");
            if (kind==2) {
                CHECK(vk_record_test_snapshot_next_owner()==UINT64_MAX,
                    "ordinary GPU retirement never forges quarantine release");
                ownership(2,0,"quarantine retains both sources across every arena/wrap");
            } else CHECK(vk_record_test_snapshot_next_owner()!=UINT64_MAX,
                "normal seal resolves already-pending and newly canceled releases");
        }
        if (kind==2) {
            vk_rec_lock();
            CHECK(vk_fg_test_generation_quarantine(g,0),"restore ONLY never-dispatched synthetic quarantine for fixture cleanup");
            vk_fg_generation_cancel(g); vk_rec_unlock();
        }
        ownership(0,0,"all non-GPU test ownership is released exactly once");
    }
    for (unsigned kind=0;kind<3;kind++)
        CHECK(covered[kind]==15,"fresh/pending/quarantine pressure each visited ALL four arena indices");
}

typedef struct {
    uint64_t facts[6];
    VkDescriptorBufferInfo buffer;
    VkDescriptorSet sets[2];
    vk_record_draw draw;
    uint8_t *bytes, *scratch;
} prepared_owned_snapshot;

static void prepared_owned_save(vk_fg_generation *g,prepared_owned_snapshot *saved) {
    memset(saved,0,sizeof *saved);
    int observed=vk_fg_test_terminal_facts(g,saved->facts,&saved->buffer,saved->sets,&saved->draw);
    CHECK(observed,"live generation exposes its actual immutable source, descriptors and draw");
    if (!observed) exit(1);
    CHECK(saved->facts[0]<4 && saved->facts[1]<4 && saved->facts[0]!=saved->facts[1] &&
        saved->facts[3]==2 && saved->facts[4]==3 && saved->facts[5]==1,
        "immutable graph owns two distinct sources, two descriptor writes and UBO/vertex/index copies");
    CHECK(saved->buffer.buffer && saved->buffer.offset==0 &&
        saved->buffer.range==saved->facts[2] && saved->buffer.range<=VK_FG_TERMINAL_BYTES &&
        saved->buffer.range>=sizeof prepared_owned.uniform_words,
        "all terminal bytes fit the bounded source-private buffer");
    CHECK(saved->buffer.buffer!=prepared_owned.uniform.buffer &&
        saved->buffer.buffer!=prepared_owned.vertex.buffer && saved->buffer.buffer!=prepared_owned.index.buffer,
        "source-private payload never aliases a borrowed snapshot or ring buffer");
    CHECK(saved->sets[0] && saved->sets[1] &&
        saved->sets[0]!=prepared_owned.borrowed[0] && saved->sets[1]!=prepared_owned.borrowed[1],
        "both immutable descriptor sets are private, not borrowed real-recorder handles");
    if (!saved->buffer.range || saved->buffer.range>VK_FG_TERMINAL_BYTES) exit(1);
    saved->bytes=malloc((size_t)saved->buffer.range); saved->scratch=malloc((size_t)saved->buffer.range);
    CHECK(saved->bytes && saved->scratch,"allocate exact private-payload observations");
    if (!saved->bytes || !saved->scratch) exit(1);
    CHECK(vk_fg_test_terminal_copy(g,saved->bytes,(size_t)saved->buffer.range),
        "observe only the private frozen payload, never guest memory");
    CHECK(!memcmp(saved->bytes,prepared_owned.uniform_words,sizeof prepared_owned.uniform_words),
        "private UBO descriptor payload preserves every managed snapshot word");
    uint8_t guard=0xa5;
    CHECK(!vk_fg_test_terminal_copy(g,&guard,0) && guard==0xa5,
        "read-only payload observation refuses insufficient capacity without writing");
    int vertex=saved->draw.vertex_mask==1 && saved->draw.vertex[0].buffer==saved->buffer.buffer &&
        saved->draw.vertex[0].size==sizeof prepared_owned.vertices &&
        saved->draw.vertex[0].stride==2*sizeof(float) &&
        saved->draw.vertex[0].offset<=saved->buffer.range &&
        saved->draw.vertex[0].size<=saved->buffer.range-saved->draw.vertex[0].offset;
    CHECK(vertex,"terminal vertex binding resolves into its source-private written range");
    if (vertex) CHECK(!memcmp(saved->bytes+saved->draw.vertex[0].offset,
        prepared_owned.vertices,sizeof prepared_owned.vertices),"managed vertex copy retains exact original bytes");
    int index=saved->draw.index==saved->buffer.buffer && saved->draw.index_type==VK_INDEX_TYPE_UINT32 &&
        saved->draw.first==0 && saved->draw.count==3 && saved->draw.index_offset<=saved->buffer.range &&
        sizeof prepared_owned.indices<=saved->buffer.range-saved->draw.index_offset;
    CHECK(index,"terminal indexed draw resolves into its source-private written range");
    if (index) CHECK(!memcmp(saved->bytes+saved->draw.index_offset,prepared_owned.indices,
        sizeof prepared_owned.indices),"managed index copy retains actual triangle indices");
}

static void prepared_owned_unchanged(vk_fg_generation *g,prepared_owned_snapshot *saved) {
    uint64_t facts[6]; VkDescriptorBufferInfo buffer; VkDescriptorSet sets[2]; vk_record_draw draw;
    int observed=vk_fg_test_terminal_facts(g,facts,&buffer,sets,&draw);
    CHECK(observed,"retained immutable graph remains observable");
    if (!observed) return;
    CHECK(!memcmp(facts,saved->facts,sizeof facts) && buffer.buffer==saved->buffer.buffer &&
        buffer.offset==saved->buffer.offset && buffer.range==saved->buffer.range &&
        !memcmp(sets,saved->sets,sizeof sets) && draw.pipeline==saved->draw.pipeline &&
        draw.layout==saved->draw.layout && draw.vertex_mask==saved->draw.vertex_mask &&
        draw.vertex[0].buffer==saved->draw.vertex[0].buffer &&
        draw.vertex[0].offset==saved->draw.vertex[0].offset && draw.vertex[0].size==saved->draw.vertex[0].size &&
        draw.index==saved->draw.index && draw.index_offset==saved->draw.index_offset &&
        draw.index_type==saved->draw.index_type && draw.count==saved->draw.count,
        "source identity and private descriptors/buffer bindings never migrate before retirement");
    CHECK(vk_fg_test_terminal_copy(g,saved->scratch,(size_t)saved->buffer.range) &&
        !memcmp(saved->bytes,saved->scratch,(size_t)saved->buffer.range),
        "snapshot/ring/guest activity cannot alter any source-private descriptor, vertex or index byte");
}

static void prepared_owned_reuse(fixture *f,const float camera[84],unsigned slot,
        const prepared_owned_snapshot *released) {
    unsigned reused=0;
    for (unsigned i=0;i<8;i++) {
        vk_fg_generation *g=NULL;
        fixture_interp_pixels(f,0); fixture_upload(f);
        (void)prepared_real(f,camera,slot,1,0,&g);
        uint64_t facts[6]; VkDescriptorBufferInfo buffer;
        int observed=vk_fg_test_terminal_facts(g,facts,&buffer,NULL,NULL);
        CHECK(observed,"retired/canceled source can prepare a new immutable interval");
        if (observed && facts[0]==released->facts[0]) {
            reused++;
            CHECK(buffer.buffer==released->buffer.buffer,"source reuse keeps its bounded private allocation");
            uint32_t words[16];
            CHECK(vk_fg_test_terminal_copy(g,released->scratch,(size_t)released->buffer.range),
                "reused source exposes a complete fresh payload");
            memcpy(words,released->scratch,sizeof words);
            CHECK(!memcmp(words,prepared_owned.uniform_words,sizeof words) &&
                memcmp(words,released->bytes,sizeof words),
                "source payload is replaced only after its former generation relinquished ownership");
        }
        vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
        CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY,"reused unsubmitted generation cancels exactly once");
        ownership(0,0,"source reuse leaves no phantom references");
    }
    CHECK(reused,"formerly retained source is actually reused after retirement/cancellation/quarantine cleanup");
}

static void immutable_terminal(fixture *f,const float camera[84],unsigned slot,
        uint8_t *golden,uint8_t *pixels,size_t bytes) {
    CHECK(vk_fg_test_terminal_owned(1),"enable explicit test-only source-private terminal capture");
    VkDescriptorSetLayout layouts[2]={f->transfer_dsl,f->ui_dsl};
    VkPipelineLayoutCreateInfo layout={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=2,.pSetLayouts=layouts};
    VKCHECK(vkCreatePipelineLayout(V.dev,&layout,NULL,&prepared_owned.layout));
    prepared_owned.pipeline=replay_pipeline(prepared_owned.layout,VK_FORMAT_R8G8B8A8_UNORM,0);
    prepared_owned.payload=prepared_owned.churn=1; prepared_owned.value=UINT32_C(0x567000);
    for (unsigned i=0;i<4;i++) {
        mtl_sub_begin(); (void)vk_cb();
        (void)vk_record_test_snapshot_allocate(UINT32_C(0x456000)+i*32);
        CHECK(mtl_wait(mtl_sub_commit(),UINT64_C(10000000000)),"immutable fixture populates each real snapshot arena");
    }
    CHECK(vk_record_test_snapshot_mask()==15 && !vk_record_test_snapshot_external_until(),
        "all four snapshot arenas exist before immutable admission, without an old external pin");
    vk_fg_generation *g=NULL;
    (void)prepared_real(f,camera,slot,1,1,&g);
    CHECK(g && vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY,
        "owned capture also cancels an open PREPARING private command stream");
    ownership(0,0,"owned open-preparation cancellation releases its sources");
    for (unsigned disposition=0;disposition<4;disposition++) {
        fixture_interp_pixels(f,0); fixture_upload(f);
        uint64_t real=prepared_real(f,camera,slot,1,0,&g);
        prepared_owned_snapshot saved; prepared_owned_save(g,&saved);
        unsigned snapshots=~0u;
        uint64_t until=0,skips=0;
        CHECK(vk_record_test_replay_pin(real,&until,&skips) && until==UINT64_MAX &&
            vk_record_test_replay_snapshots(real,&snapshots) && !snapshots,
            "immutable PREPARED graph pins its real recorder slot but ZERO allocated snapshot arenas");
        CHECK(!vk_fg_test_terminal_owned(0) && !vk_fg_test_terminal_owned(1) &&
            atomic_load_explicit(&vk_fg_terminal_owned_active,memory_order_relaxed),
            "test override cannot change capture mode while any generation exists");
        if (disposition==3) {
            vk_rec_lock();
            CHECK(vk_fg_test_generation_quarantine(g,1),"quarantine only a never-dispatched real private graph");
            vk_rec_unlock();
        }
        if (disposition) {
            memset(prepared_owned.vertex_mapping,0,sizeof prepared_owned.vertices);
            memset(prepared_owned.index_mapping,0,sizeof prepared_owned.indices);
            VkDescriptorImageInfo image={f->ui_sampler,f->final_view,VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet=prepared_owned.borrowed[0],.dstBinding=0,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
            vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
            VkDescriptorBufferInfo buffer={f->ui_uniform.buffer,0,f->ui_uniform.size};
            write=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet=prepared_owned.borrowed[1],.dstBinding=0,.descriptorCount=1,
                .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
            vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
            /* Updating a borrowed descriptor and the originally frozen mapped
             * bytes is safe after the real fence; immutable commands must not
             * read any of them when eventually dispatched. */
        }
        vk_fg_snapshot_pressure_counts pressure=vk_fg_snapshot_pressure_get();
        CHECK(pressure.observations && pressure.cancellations && pressure.appends && pressure.quarantined,
            "enabled diagnostic counters witnessed legacy pressure, making the immutable no-pressure check non-vacuous");
        unsigned arenas=0,recycled=0;
        for (unsigned newer=0;newer<12;newer++) {
            unsigned arena=vk_record_test_snapshot_next_index(); arenas|=1u<<arena;
            CHECK(vk_record_test_snapshot_next_owner()!=UINT64_MAX,
                "mandatory snapshot capture has no open immutable snapshot owner to cancel/wait");
            uint64_t life_before[3],life_after[3]; vk_fg_test_source_lifecycle(life_before);
            fixture_interp_pixels(f,disposition?1:0); fixture_upload(f);
            uint64_t before=mtl_last_fence(),serial=vk_submit_serial();
            vk_fg_generation *unused=NULL;
            alarm(10);
            uint64_t newer_real=prepared_real(f,camera,slot,0,0,&unused);
            alarm(0);
            vk_fg_test_source_lifecycle(life_after);
            CHECK(newer_real==before+1 && vk_submit_serial()==serial+1 &&
                life_after[1]==life_before[1]+1,
                "mandatory newer real/snapshot interval makes ordinary unsplit FIFO progress");
            CHECK(life_after[0] && life_after[0]!=saved.facts[0]+1 && life_after[0]!=saved.facts[1]+1,
                "both immutable source refs exclude their image/descriptor slots from real reuse");
            CHECK(vk_fg_generation_state(g)==(disposition==3 ?
                VK_FG_GENERATION_QUARANTINED:VK_FG_GENERATION_PREPARED),
                "twelve newer snapshot intervals never pressure-cancel or retire this template");
            CHECK(vk_record_test_replay_snapshots(real,&snapshots) && !snapshots &&
                !vk_record_test_snapshot_external_until(),"real churn never resurrects a snapshot pin");
            if (prepared_owned.churn_range.buffer==prepared_owned.uniform.buffer &&
                prepared_owned.churn_range.offset==prepared_owned.uniform.offset) {
                uint32_t words[16];
                CHECK(vk_replay_frozen_buffer_copy(prepared_owned.churn_range.buffer,
                    prepared_owned.churn_range.offset,sizeof words,words) &&
                    words[0]==prepared_owned.value && memcmp(words,prepared_owned.uniform_words,sizeof words),
                    "the original managed UBO storage is genuinely recycled and overwritten");
                recycled++;
            }
            prepared_owned_unchanged(g,&saved);
            ownership(2,0,"newer real keeps both source refs but owns no generation output");
        }
        vk_fg_snapshot_pressure_counts after=vk_fg_snapshot_pressure_get();
        CHECK(arenas==15 && recycled>=3,"twelve newer real intervals wrap ALL four arenas and recycle original UBO storage");
        CHECK(after.observations==pressure.observations && after.cancellations==pressure.cancellations &&
            after.appends==pressure.appends && after.pending==pressure.pending &&
            after.quarantined==pressure.quarantined,"immutable churn never even enters legacy snapshot-pressure cancellation");
        if (disposition<2) {
            vk_post_output output;
            uint64_t continuation=0;
            if (disposition==1) {
                /* Existing chunk boundary, NOT a product scheduler hook:
                 * remain inside the guest submission while the ordinary
                 * chunk seals its primary and drains producer service. */
                mtl_sub_begin(); (void)vk_cb();
                uint64_t before_chunk=mtl_last_fence();
                CHECK(!vk_fg_generation_dispatch(g,0.375f,host_now_ns()+UINT64_C(10000000000),&output) &&
                    mtl_last_fence()==before_chunk,"open guest primary cannot be split by optional dispatch");
                vk_submit_chunk();
                CHECK(mtl_last_fence()==before_chunk+1,"existing chunk seals exactly the ordinary mandatory prefix");
            } else vk_rec_lock();
            uint64_t expected=mtl_last_fence()+2;
            /* Production optional admission only TRYLOCKs the ring mutex and skips the
             * child when the completion thread holds it. Let completions drain, then
             * allow a bounded number of attempts that leave no submission behind;
             * any other refusal still fails the assertions below. */
            (void)mtl_wait(mtl_last_fence(),UINT64_C(10000000000));
            vk_record_test_hold_completion(expected);
            int dispatched=0;
            for (unsigned attempt=0;attempt<64 && !dispatched;attempt++) {
                uint64_t fence=mtl_last_fence();
                dispatched=vk_fg_generation_dispatch(g,0.375f,host_now_ns()+UINT64_C(10000000000),&output);
                if (dispatched || mtl_last_fence()!=fence ||
                    vk_fg_generation_state(g)!=VK_FG_GENERATION_PREPARED) break;
                usleep(1000);
            }
            CHECK(dispatched==1,"immutable graph dispatches its actual indexed terminal through the real GPU FIFO");
            CHECK(!vk_record_test_snapshot_external_until() &&
                vk_record_test_replay_snapshots(real,&snapshots) && !snapshots,
                "immutable optional split never resurrects GLOBAL or source snapshot pins");
            uint64_t ticket=dispatched?vk_fg_generation_seal(g):0;
            uint64_t facts[6]; vk_fg_test_generation_facts(g,facts,NULL);
            if (disposition==1) {
                (void)vk_cb();
                continuation=mtl_sub_commit();
                CHECK(continuation==ticket+1,"mandatory guest continuation resumes after the exact child range");
            } else vk_rec_unlock();
            CHECK(ticket==expected && facts[4] && vk_record_test_replay_snapshots(facts[4],&snapshots) &&
                !snapshots && !vk_record_test_snapshot_external_until(),
                "actual signal/consumer tickets retain recorder ownership only, never snapshot arenas");
            if (!ticket) { vk_record_test_hold_completion(0); exit(1); }
            uint64_t deadline=host_now_ns()+UINT64_C(10000000000);
            while (vk_record_test_completion_held()!=ticket && host_now_ns()<deadline) usleep(1000);
            CHECK(vk_record_test_completion_held()==ticket && !mtl_wait(ticket,0) &&
                !vk_fg_generation_retire(g) && !vk_fg_complete(slot),
                "immutable GPU ownership cannot retire before the delayed actual covering watermark");
            CHECK(vk_record_test_replay_pin(real,&until,&skips) && until>=ticket &&
                !vk_record_test_snapshot_external_until(),"submitted immutable lease extends only the real recorder pin");
            CHECK(!vk_fg_test_terminal_owned(0),"submitted immutable graph also refuses capture-mode changes");
            ownership(2,1,"submitted immutable graph retains source/private-command/output ownership");
            prepared_owned_unchanged(g,&saved);
            vk_record_test_hold_completion(0);
            CHECK(mtl_wait(ticket,UINT64_C(10000000000)),"actual immutable terminal consumer completes on the GPU");
            CHECK(!continuation || mtl_wait(continuation,UINT64_C(10000000000)),
                "resumed mandatory continuation retires without borrowing the child graph");
            read_image_extent(output.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,bytes,native_terminal_w,native_terminal_h);
            if (!disposition) memcpy(golden,pixels,bytes);
            else CHECK(!memcmp(golden,pixels,bytes),
                "snapshot overwrite, borrowed descriptor mutation and index corruption cannot change immutable GPU output");
            CHECK(vk_fg_complete(slot) && !vk_fg_complete(slot),"immutable output completes and releases ownership exactly once");
        } else if (disposition==2) {
            mtl_sub_begin(); (void)vk_cb();
            uint64_t before=mtl_last_fence(),serial=vk_submit_serial();
            vk_fg_generation_cancel(g);
            CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY &&
                mtl_last_fence()==before && vk_submit_serial()==serial,
                "immutable cancellation under an open real primary creates no hidden seal/ticket");
            CHECK(vk_record_test_replay_pin(real,&until,&skips) && until==UINT64_MAX,
                "canceled immutable recorder lease defers release until that primary's ordinary seal");
            (void)vk_record_test_snapshot_allocate(UINT32_C(0x789000));
            CHECK(mtl_wait(mtl_sub_commit(),UINT64_C(10000000000)),"ordinary real seal retires canceled immutable lease");
        } else {
            vk_rec_lock();
            vk_fg_generation_cancel(g);
            CHECK(!vk_fg_generation_cancel_prepared() && vk_fg_generation_state(g)==VK_FG_GENERATION_QUARANTINED,
                "cancel/pressure cannot release quarantined private storage or sources");
            CHECK(!vk_fg_test_terminal_owned(0),"quarantine also refuses capture-mode changes");
            ownership(2,0,"quarantine remains source-owned after every mandatory real retired");
            CHECK(vk_fg_test_generation_quarantine(g,0),"restore ONLY the fixture's never-dispatched quarantine");
            vk_fg_generation_cancel(g); vk_rec_unlock();
        }
        CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY &&
            !vk_fg_test_terminal_facts(g,NULL,NULL,NULL,NULL),
            "retired/canceled graph exposes no live private-source authority");
        ownership(0,0,"immutable retirement/cancellation releases every source/output reference");
        prepared_owned_reuse(f,camera,slot,&saved);
        free(saved.bytes); free(saved.scratch);
    }
    for (int refusal=PREPARED_CAPTURE_UNSUPPORTED;refusal<=PREPARED_CAPTURE_DESCRIPTOR_CAP;refusal++) {
        prepared_owned.refusal=refusal;
        uint64_t before=mtl_last_fence(),life_before[3],life_after[3];
        vk_fg_test_source_lifecycle(life_before);
        uint64_t real=prepared_real(f,camera,slot,2,0,&g);
        vk_fg_test_source_lifecycle(life_after);
        CHECK(!g && real==before+1 && life_after[1]==life_before[1]+1 && life_after[0] && life_after[2]==3,
            "unsupported/writable/unknown/over-cap capture preserves real publication and complete UI ancestry");
        ownership(0,0,"optional payload refusal never takes source/output ownership");
        prepared_owned.refusal=PREPARED_CAPTURE_OK;
        (void)prepared_real(f,camera,slot,1,0,&g);
        CHECK(g && vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARED,
            "capture refusal poisons only that optional interval, not later real progress or admission");
        vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
        ownership(0,0,"recovery interval cancels without leaked ownership");
    }
    prepared_owned.payload=prepared_owned.churn=0;
    CHECK(vk_fg_test_terminal_owned(0),"drained immutable fixture restores explicit legacy capture");
    vkDestroyPipeline(V.dev,prepared_owned.pipeline,NULL);
    vkDestroyPipelineLayout(V.dev,prepared_owned.layout,NULL);
    prepared_owned.pipeline=VK_NULL_HANDLE; prepared_owned.layout=VK_NULL_HANDLE;
}

#include "framegen_test_chunk.h"

int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--chunk-hook")) return run_prepared_chunk(0);
    if (argc==2 && !strcmp(argv[1],"--chunk-hook-adaptive")) return run_prepared_chunk(1);
    setenv("TOTK_NATIVE_UI","1",1);
    setenv("TOTK_FRAMEGEN","interp",1);
    setenv("TOTK_FRAMEGEN_BUDGET_MS","10000",1); /* interp ownership ONLY; adaptive refuses overrides */
    setenv("TOTK_POST_AA","off",1);
    setenv("TOTK_FRAMEGEN_JOB_TRACE","1",1);
    native_ui_terminal=1;
    native_terminal_w=FG_TEST_W*3/2; native_terminal_h=FG_TEST_H*3/2;
    mtl_init();
    CHECK(vk_fg_test_generation_bridge(1),"enable explicit test-only Vulkan bridge");
    CHECK(vk_fg_test_terminal_owned(0),"legacy ownership and snapshot-pressure cases explicitly disable owned capture");
    CHECK(vk_native_ui_enabled() && vk_fg_interpolating(),"actual native-UI interpolation fixture admitted");
    vk_native_window_size(native_terminal_w,native_terminal_h);
    fixture f; fixture_create(&f); f.ui_repeats=1;
    if (vk_scene_post_enabled()) mtl_xv_window_tex(f.final_desc.serial);
    else vk_tex_native_promote(f.final);
    vk_tex_native_ui_begin(f.final);
    CHECK(vk_tex_native_view(f.final,NULL,1)!=VK_NULL_HANDLE,"native terminal sibling is allocated");
    const float transparent[4]={0}; float camera[84]; fg_test_camera(camera,0,0,0,0);
    fixture_interp_pixels(&f,0); fixture_upload(&f); fixture_interp_ui(&f,transparent);
    const unsigned slot=2;
    vk_fg_generation *g=NULL;
    (void)prepared_real(&f,camera,slot,0,0,&g); /* history warmup */
    (void)prepared_real(&f,camera,slot,1,1,&g);
    CHECK(g,"open-preparation cancellation ran on an eligible pair");
    ownership(0,0,"open cancellation returns both source references");
    (void)prepared_real(&f,camera,slot,1,0,&g);
    CHECK(g,"closed-preparation cancellation ran on an eligible pair");
    uint64_t before_cancel=mtl_last_fence();
    vk_rec_lock(); vk_fg_generation_cancel(g); vk_rec_unlock();
    CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY && mtl_last_fence()==before_cancel,
        "closed unsubmitted template cancels without a ticket");
    ownership(0,0,"closed cancellation returns both source references");
    snapshot_pressure(&f,camera,slot);
    vk_fg_generation *other=NULL;
    (void)prepared_real(&f,camera,slot,1,0,&g);
    (void)prepared_real(&f,camera,slot+1,1,0,&other);
    CHECK(g && other,"two eligible source intervals can reach one charge boundary");
    vk_post_output aggregate_output;
    vk_rec_lock();
    int first=vk_fg_generation_dispatch(g,0.5f,host_now_ns()+UINT64_C(10000000000),&aggregate_output);
    CHECK(first==1,"first source owns the charge interval's sole accepted template");
    uint64_t aggregate_ticket=first ? vk_fg_generation_seal(g) : 0;
    uint64_t before_second=mtl_last_fence();
    CHECK(!vk_fg_generation_dispatch(other,0.75f,host_now_ns()+UINT64_C(10000000000),&aggregate_output) &&
        mtl_last_fence()==before_second,"second eligible source cannot double-charge the same interval");
    vk_fg_generation_cancel(other);
    vk_rec_unlock();
    CHECK(aggregate_ticket && mtl_wait(aggregate_ticket,UINT64_C(10000000000)),
        "aggregate interval's sole actual GPU output retires");
    if (!aggregate_ticket) return 1;
    CHECK(vk_fg_complete(slot),"fixed-override ownership output completes after second source is refused");
    CHECK(!vk_fg_complete(slot),"duplicate completion cannot charge or release the same output twice");
    ownership(0,0,"aggregate refusal has no phantom source/output ownership");
    uint64_t pool=0,command=0;
    size_t bytes=(size_t)native_terminal_w*native_terminal_h*4;
    uint8_t *golden=malloc(bytes),*pixels=malloc(bytes);
    CHECK(golden && pixels,"allocate immutable native output goldens");
    if (!golden || !pixels) return 1;
    /* TOTK_TEST_LEGACY_PREPARED=1 skips owned capture; =vertex additionally
     * binds an unconsumed transient vertex buffer in the borrowed path. These
     * reproduce Lavapipe's process-exit LSan report without the new code. */
    const char *legacy=getenv("TOTK_TEST_LEGACY_PREPARED");
    if (!legacy || (strcmp(legacy,"1") && strcmp(legacy,"vertex")))
        immutable_terminal(&f,camera,slot,golden,pixels,bytes);
    for (unsigned frame=0;frame<32;frame++) {
        fixture_interp_pixels(&f,0); fixture_upload(&f); fixture_interp_ui(&f,transparent);
        uint64_t real=prepared_real(&f,camera,slot,1,0,&g);
        CHECK(g && vk_fg_generation_state(g)==VK_FG_GENERATION_PREPARED,"closed template survives source real publication");
        if (!g) return 1;
        uint64_t facts[6]; float phase;
        vk_fg_test_generation_facts(g,facts,&phase);
        CHECK(facts[0] && !facts[1] && facts[2] && facts[3],"preparation owns a lease/pool/CB but no covering ticket");
        if (!pool) { pool=facts[2]; command=facts[3]; }
        CHECK(pool==facts[2] && command==facts[3],"same bounded private pool/CB reuses only after prior retirement");
        uint64_t until=0,skips=0;
        CHECK(vk_record_test_replay_pin(real,&until,&skips) && until==UINT64_MAX,
            "actual source recorder descriptors remain leased after real fence");
        vk_post_output output;
        uint64_t reserved=mtl_last_fence();
        CHECK(!vk_fg_generation_dispatch(g,0.375f,host_now_ns(),&output) && mtl_last_fence()==reserved,
            "late optional work skips without GL dispatch or ticket allocation");
        if (frame==1) {
            /* Twelve later reals wrap recorder and source rings while this
             * exact native replay descriptor and source pair stay pinned. */
            for (unsigned newer=0;newer<12;newer++) {
                fixture_interp_pixels(&f,1); fixture_upload(&f);
                vk_fg_generation *unused;
                (void)prepared_real(&f,camera,slot,0,0,&unused);
                ownership(2,0,"newer real does not reuse pinned immutable sources");
            }
        }
        ownership(2,0,"prepared template has no pending Vulkan output");
        uint64_t expected=mtl_last_fence()+2;
        vk_record_test_hold_completion(expected);
        vk_rec_lock();
        int dispatched=vk_fg_generation_dispatch(g,0.375f,host_now_ns()+UINT64_C(10000000000),&output);
        CHECK(dispatched==1,"chosen phase dispatches through real normal FIFO split/consumer");
        uint64_t ticket=dispatched ? vk_fg_generation_seal(g) : 0;
        vk_rec_unlock();
        CHECK(ticket==expected,"dispatch allocates exactly two in-order tickets, never a prepare reservation");
        if (!ticket) { vk_record_test_hold_completion(0); return 1; }
        uint64_t deadline=host_now_ns()+UINT64_C(10000000000);
        while (vk_record_test_completion_held()!=ticket && host_now_ns()<deadline) usleep(1000);
        CHECK(vk_record_test_completion_held()==ticket,"actual GPU completion reaches the delayed g_done gate");
        CHECK(!mtl_wait(ticket,0) && !vk_fg_generation_retire(g) && !vk_fg_complete(slot),
            "neither retire nor completion can release before the covering watermark");
        CHECK(vk_record_test_replay_pin(real,&until,&skips) && until>=ticket,
            "source descriptor lease extends to the actual generation consumer ticket");
        ownership(2,1,"delayed covering ticket retains command/source/output ownership");
        CHECK(!vk_fg_generation_cancel_prepared() &&
            vk_fg_generation_state(g)==VK_FG_GENERATION_SUBMITTED,
            "snapshot pressure cannot cancel or release submitted GPU ownership");
        vk_fg_test_generation_facts(g,facts,&phase);
        CHECK(phase==0.375f && facts[2]==pool && facts[3]==command,"phase chosen only at dispatch; private command storage unchanged");
        if (frame==2) {
            CHECK(!vk_fg_test_generation_bridge(0),"shutdown cannot detach a still-submitted private graph");
            vk_fg_generation_cancel(g); /* disable/shutdown disposition */
            vk_fg_reset(); /* real surface-loss request */
            vk_rec_lock(); vk_fg_history_cut(); vk_rec_unlock();
            CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_SUBMITTED,"surface loss never retires an uncompleted graph");
            ownership(2,1,"cancel/disable/reset retain delayed ownership");
        }
        vk_record_test_hold_completion(0);
        CHECK(mtl_wait(ticket,UINT64_C(10000000000)),"release covering completion watermark");
        if (frame<2) {
            read_image_extent(output.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,bytes,native_terminal_w,native_terminal_h);
            if (!frame) memcpy(golden,pixels,bytes);
            else CHECK(!memcmp(golden,pixels,bytes),"later source/UI/ring activity cannot change the prepared native output");
        }
        int accepted=vk_fg_complete(slot);
        CHECK(frame==2 ? !accepted : accepted,
            "canceled output rejects; other fenced private outputs complete under ownership-only override");
        CHECK(vk_fg_generation_state(g)==VK_FG_GENERATION_EMPTY && !vk_fg_complete(slot),
            "exactly-once completion releases the private template");
        ownership(0,0,"completed template returns every source/output lease");
        if (frame==2) { /* actual reset discarded history, not retained resources */
            fixture_interp_pixels(&f,0); fixture_upload(&f);
            vk_fg_generation *unused; (void)prepared_real(&f,camera,slot,0,0,&unused);
        }
    }
    CHECK(vk_fg_test_generation_bridge(0),"drained shutdown can release the test bridge");
    vk_native_frame_end(); vk_gpu_idle();
    mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels); free(golden); free(pixels);
    printf("prepared Vulkan ownership: %u checks, %u failures; source4/private-CB/native-descriptor/FIFO, fixed-override ownership only, no adaptive-budget/AFME/device claim\n",checks,failures);
    return failures ? 1 : 0;
}
