/* Included by test_vkframegen.c: real Vulkan draws through production extractor
   hooks. These modes deliberately instrument the real path, not timing evidence. */
static struct {
    unsigned kind;
    VkPipeline pipeline;
    host_buffer uniform[2];
} writer_fixture;
static int writer_fixture_capture(fixture *f, const float camera[84]) {
    unsigned kind=writer_fixture.kind;
    VkRect2D rect[2]={{{90,65},{24,18}},{{124,91},{24,18}}};
    if (kind==1 || kind==2 || kind==5 || kind==9) rect[1]=rect[0];
    vk_fg_test_writer_fail(kind==6,kind==7 ? 1 : 0);
    for (unsigned draw=0;draw<2;draw++) {
        float z=kind==3 && !draw ? 2 : kind==2 && draw ? 2 : kind==9 ? 4 : draw ? 6 : 4;
        float uniform[8]={0,0,fg_test_depth(z),0,0.2f+0.3f*draw,0.3f,0.6f,1};
        memcpy(host_map(&writer_fixture.uniform[draw]),uniform,sizeof uniform);
        vkUnmapMemory(V.dev,writer_fixture.uniform[draw].memory);
        unsigned identity=kind==4 ? 0 : draw;
        unsigned classification=kind==5 && draw ? VK_FG_WRITER_PROVEN : VK_FG_WRITER_UNPROVEN;
        CHECK(vk_fg_test_writer_draw_begin(&f->scene_desc,&f->depth_desc,camera,!draw,identity,classification,&rect[draw]),
            "diagnostic case%u draw%u does not change real draw admission",kind,draw);
        if (kind==8 && !draw && vk_fg_writer_diag())
            CHECK(!vk_fg_writer_begin(vk_cb(),f->scene,0,0,"bad","bad","unsupported",VK_FG_WRITER_UNPROVEN),
                "unsupported diagnostic identity reports unavailable without authority failure");
        VkDescriptorSet set=vk_alloc_set(f->ui_dsl);
        VkDescriptorBufferInfo buffer={writer_fixture.uniform[draw].buffer,0,sizeof uniform};
        VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
        vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
        vk_record_draw packet={.pipeline=writer_fixture.pipeline,.layout=f->ui_layout,.count=3,.instances=1};
        packet.dynamic.vp=(VkViewport){0,0,W,H,0,1}; packet.dynamic.sc=rect[draw];
        packet.dynamic.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        packet.dynamic.dt=packet.dynamic.dw=1; packet.dynamic.dc=VK_COMPARE_OP_ALWAYS;
        VkDescriptorSet sets[2]={set,0}; vk_fg_replay_draw(vk_cb(),&packet,sets);
        vk_fg_test_writer_draw_note(draw==1);
    }
    vk_fg_test_writer_fail(0,0);
    return vk_fg_test_capture_reads(&f->scene_desc,NULL,0);
}
static void writer_fixture_pixels(fixture *f) {
    fixture_interp_pixels(f,0);
    for (size_t p=0;p<DEPTH_PIXELS;p++) f->depth_pixels[p]=fg_test_depth(2);
    fixture_upload(f);
    const float transparent[4]={0}; fixture_interp_ui(f,transparent);
}
static void writer_fixture_check(unsigned slot, int on, unsigned kind) {
    unsigned status[8]; vk_fg_writer_test_group groups[64];
    int valid=vk_fg_test_writer_result(slot,status,groups);
    if (!on) {
        unsigned zero[8]={0};
        CHECK(!valid && !memcmp(status,zero,sizeof zero),"off writer lane has no groups/spans/census/GPU resources");
        return;
    }
    if (kind>=6 && kind<=8) {
        CHECK(!valid && status[4] && (kind!=7 || status[5]),
            "failure/unsupported/capacity case%u is explicitly incomplete, never a stale census",kind);
        return;
    }
    const unsigned area=24*18;
    unsigned unknown=kind==2 || kind==5 ? 0 : kind==1 || kind==3 || kind==9 ? area : 2*area;
    CHECK(valid && status[3] && !status[4] && !status[5],"writer case%u exposes complete fenced diagnostics",kind);
    if (!valid) return;
    CHECK(status[0]==(kind==4 || kind==5 ? 1u : 2u) && status[1]==2 && status[1]==status[2],
        "case%u closes every diagnostic span, including same-pair production scissor expansion",kind);
    CHECK(status[6]==unknown && !status[7],
        "case%u residual census counts exact final unknown values and revokes proven changed-value owners",kind);
    CHECK(!strcmp(groups[0].vs,"writer-fixture-vs-A") && !strcmp(groups[0].fs,"writer-fixture-fs-A") &&
        !strcmp(groups[0].check,"fixture-unproven-camera") && groups[0].classification==VK_FG_WRITER_UNPROVEN,
        "exact VS/FS keys, check reason and authority classification survive to fenced readback");
    CHECK(groups[0].gross==(kind==3 ? 0u : kind==4 ? 2*area : area),
        "case%u first pair gross count is changed D32 pixels, not draw/scissor area",kind);
    CHECK(groups[0].residual==(kind==2 || kind==3 || kind==5 ? 0u : kind==4 ? 2*area : kind==1 ? 0u : area),
        "case%u first pair owns only its final last-changed VALUE",kind);
    if (status[0]==2) {
        CHECK(!strcmp(groups[1].vs,"writer-fixture-vs-B") && !strcmp(groups[1].fs,"writer-fixture-fs-B"),
            "second shader pair identity remains distinct");
        CHECK(groups[1].gross==(kind==9 ? 0u : area),"case%u overlap/restore counts gross independently",kind);
        CHECK(groups[1].residual==(kind==2 || kind==9 ? 0u : area),
            "case%u restored baseline/same-value writes do not acquire false residual ownership",kind);
    }
    CHECK(groups[0].draws==(kind==4 ? 2u : 1u),"GPU span stores exact actual draw count");
    CHECK(authority.have_result && authority.result[2]==unknown,
        "writer diagnostic never changes ordinary raw VALUE authority");
}
static int run_interp_writer(unsigned flag) {
    setenv("TOTK_FRAMEGEN","interp",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","1000",1);
    setenv("TOTK_FRAMEGEN_DIAG",flag==3 ? "0" : "1",1);
    if (flag) setenv("TOTK_FRAMEGEN_WRITER_DIAG",flag==2 ? "2" : "1",1);
    else unsetenv("TOTK_FRAMEGEN_WRITER_DIAG");
    setenv("TOTK_FRAMEGEN_UNCERTIFIED_PLANE",flag==1 ? "1" : "0",1);
    mtl_init(); int on=flag==1;
    CHECK(vk_fg_writer_diag()==on,"writer attribution requires exact opt-in1 AND DIAG1");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    writer_fixture.pipeline=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,3);
    for (unsigned i=0;i<2;i++) writer_fixture.uniform[i]=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    writer_capture=writer_fixture_capture; authority.defer_complete=1;
    float camera[84]; fg_test_camera(camera,0,0,0,0); vk_post_output out;
    for (unsigned kind=0;kind<10;kind++) {
        vk_fg_reset(); writer_fixture.kind=kind;
        for (unsigned frame=0;frame<2;frame++) {
            writer_fixture_pixels(&f);
            int generated=real_frame(&f,camera,frame,0,&out);
            CHECK(generated==(frame!=0),"writer case%u frame%u leaves ordinary interpolation cadence unchanged",kind,frame);
            if (generated) {
                writer_fixture_check(frame,on,kind);
                CHECK(vk_fg_complete(frame),"diagnostic failures do not refuse ordinary authority/output");
            }
        }
    }
    /* Hold a completed-but-not-released output while later sources record.
       The later GPU fence must not mutate earlier CPU readback/identity. */
    vk_fg_reset(); writer_fixture.kind=0;
    unsigned held_status[8]={0}; vk_fg_writer_test_group held_groups[64]={0};
    for (unsigned frame=0;frame<3;frame++) {
        writer_fixture_pixels(&f);
        CHECK(real_frame(&f,camera,frame,0,&out)==(frame!=0),"writer source lifetime frame%u records",frame);
        if (frame==1) {
            if (on) CHECK(vk_fg_test_writer_result(1,held_status,held_groups),"held output has fenced writer ownership");
        } else if (frame==2) {
            unsigned status[8]; vk_fg_writer_test_group groups[64];
            if (on) CHECK(vk_fg_test_writer_result(1,status,groups) && !memcmp(status,held_status,sizeof status) &&
                !memcmp(groups,held_groups,sizeof groups),"lagged output retains private immutable writer census and keys");
            CHECK(vk_fg_complete(2) && vk_fg_complete(1),"lagged writer sources retire only after exact output release");
        }
    }
    /* Real nominated experimental plane remains unproven in the gross table,
       but exact assumed values are excluded by ordinary final row-prefix bits. */
    if (on) {
        writer_capture=NULL; experimental.enabled=1; experimental.on=vk_fg_uncertified_plane(); experimental.kind=1;
        CHECK(experimental.on,"writer DIAG arm explicitly includes the independently opt-in experimental plane");
        experimental.pipeline=writer_fixture.pipeline;
        for (unsigned i=0;i<3;i++) experimental.uniform[i]=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        vk_fg_reset();
        for (unsigned frame=0;frame<2;frame++) {
            writer_fixture_pixels(&f); experimental.frame=frame;
            int generated=real_frame(&f,camera,frame,0,&out);
            CHECK(generated==(frame!=0),"experimental qualification writer diagnostic cadence remains unchanged");
            if (generated) {
                unsigned status[8]; vk_fg_writer_test_group groups[64];
                CHECK(vk_fg_test_writer_result(frame,status,groups) && status[0]>=1,
                    "experimental shader pair remains represented in GPU pixel attribution");
                if (experimental.on) CHECK(groups[0].classification==VK_FG_WRITER_UNCERTIFIED &&
                    groups[0].gross==24*18 && !groups[0].residual && !status[6],
                    "UNCERTIFIED qualification is separate from proof and exempt final values are excluded");
                CHECK(vk_fg_complete(frame),"experimental writer instrumentation does not change output admission");
            }
        }
        for (unsigned i=0;i<3;i++) host_destroy(&experimental.uniform[i]);
        experimental.enabled=0;
    }
    if (on) {
        /* Exercise the actual 300-real aggregation/harvest boundary, not a
           fabricated stats-only flush. Every capture keeps exact GPU goldens. */
        writer_capture=writer_fixture_capture; writer_fixture.kind=0;
        vk_fg_reset();
        for (unsigned frame=0;frame<300;frame++) {
            writer_fixture_pixels(&f);
            int generated=real_frame(&f,camera,frame%4,0,&out);
            CHECK(generated==(frame!=0),"writer reporting real frame%u preserves cadence",frame);
            if (generated) {
                writer_fixture_check(frame%4,on,0);
                CHECK(vk_fg_complete(frame%4),"writer reporting frame%u releases measured source",frame);
            }
        }
    }
    writer_capture=NULL; authority.defer_complete=0;
    vk_gpu_idle(); vkDestroyPipeline(V.dev,writer_fixture.pipeline,NULL);
    for (unsigned i=0;i<2;i++) host_destroy(&writer_fixture.uniform[i]);
    mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels);
    return failures ? 1 : 0;
}

/* Ordered VALUE certificates are exercised with actual explicit-depth GPU
   draws and the production extraction hooks. Rectangles specify these fixture
   draws, not a claim that a real shader necessarily covers its whole scissor. */
enum {
    ORDER_NOOP_THEN_PROVEN46, ORDER_UNKNOWN_THEN_PROVEN, ORDER_PROVEN_THEN_UNKNOWN,
    ORDER_OVERLAP, ORDER_DISJOINT, ORDER_RESTORE, ORDER_UNKNOWN_SAME_VALUE,
    ORDER_KNOWN_SAME_VALUE, ORDER_PROVEN_IDENTICAL, ORDER_PROVEN_HIDDEN_RESTORE,
    ORDER_PROVEN_BASELINE_RESTORE, ORDER_RESTORE_NEW_PROVEN, ORDER_CONTAINED,
    ORDER_EXPANDING, ORDER_EDGE, ORDER_DISJOINT_THIRD, ORDER_EXPT_UNCHANGED,
    ORDER_EXPT_CHANGED, ORDER_EXPT_OVERWRITTEN, ORDER_EXPT_SAME_VALUE,
    ORDER_EXPT_PROVEN_IDENTICAL, ORDER_EXPT_PROVEN_THEN_UNKNOWN, ORDER_UNSAFE46,
    ORDER_CASES, ORDER_FAIL_BEGIN=ORDER_CASES, ORDER_FAIL_END, ORDER_UNCLOSED,
    ORDER_RESET, ORDER_UNSUPPORTED, ORDER_CAPACITY, ORDER_FAULTS
};
typedef struct {
    VkRect2D rect;
    float distance;
    unsigned classification, identity;
    int close;
} ordering_draw;
static struct {
    unsigned kind, frame, count, admitted, known_draw, known_x, known_y;
    int have_known;
    VkPipeline pipeline;
    host_buffer uniform;
    VkDeviceSize stride;
    ordering_draw draw[257];
    uint8_t *raw;
} ordering;
static VkRect2D ordering_rect(int x,int y,unsigned width,unsigned height) {
    return (VkRect2D){{x,y},{width,height}};
}
static void ordering_add(VkRect2D rect,float distance,unsigned classification,int close) {
    CHECK(ordering.count<257,"ordered fixture draw plan fits its exact capacity negative");
    if (ordering.count>=257) exit(1);
    unsigned index=ordering.count++;
    ordering.draw[index]=(ordering_draw){rect,distance,classification,index&1,close};
}
static void ordering_unknown(VkRect2D rect) {
    unsigned x0=(unsigned)rect.offset.x,y0=(unsigned)rect.offset.y;
    unsigned x1=x0+rect.extent.width<DW ? x0+rect.extent.width : DW;
    unsigned y1=y0+rect.extent.height<DH ? y0+rect.extent.height : DH;
    for (unsigned y=y0;y<y1;y++) for (unsigned x=x0;x<x1;x++)
        ordering.raw[(size_t)y*DW+x]=1;
}
static const char *ordering_name(unsigned kind) {
    static const char *const names[]={
        "unchanged unknown then proven46%", "unknown then changed proven",
        "proven then unknown", "overlapping third unknown", "disjoint proven",
        "unknown restores last proven", "unknown same-value color change",
        "known same-value color change", "identical actual proven write",
        "hidden proven change and restore", "hidden proven restore to baseline",
        "restore newly observed proven", "contained odd scissor cross-pair batch",
        "expanding odd scissor", "partial right/bottom edge", "disjoint third unknown",
        "qualified unchanged after unknown", "qualified changed after unknown",
        "unknown overwrites experimental", "unknown same-value keeps experimental",
        "identical proven keeps experimental assumption", "experimental then proven then unknown",
        "changed unknown46% threshold", "before-copy failure", "after-copy failure",
        "unclosed engine span", "reset pending source", "unsupported source", "span capacity"
    };
    return names[kind];
}
static void ordering_plan(unsigned kind) {
    ordering.kind=kind; ordering.count=ordering.admitted=0; ordering.have_known=0;
    memset(ordering.raw,0,DEPTH_PIXELS);
    VkRect2D a=ordering_rect(91,65,25,19),b=ordering_rect(143,97,25,19);
    VkRect2D c=ordering_rect(103,73,25,19),large=ordering_rect(0,0,W*46/100,H);
    unsigned u=VK_FG_WRITER_UNPROVEN,p=VK_FG_WRITER_PROVEN,e=VK_FG_WRITER_UNCERTIFIED;
    switch (kind) {
    case ORDER_NOOP_THEN_PROVEN46:
        ordering_add(ordering_rect(0,0,W,H),2,u,0); ordering_add(large,4,p,1);
        ordering.have_known=1; ordering.known_draw=1; ordering.known_x=50; ordering.known_y=74;
        break;
    case ORDER_UNKNOWN_THEN_PROVEN:
        ordering_add(a,4,u,0); ordering_add(a,6,p,1);
        ordering.have_known=1; ordering.known_draw=1; break;
    case ORDER_PROVEN_THEN_UNKNOWN:
        ordering_add(a,4,p,0); ordering_add(a,6,u,1); ordering_unknown(a); break;
    case ORDER_OVERLAP:
        ordering_add(a,4,u,0); ordering_add(a,6,p,0); ordering_add(c,8,u,1);
        ordering_unknown(c); break;
    case ORDER_DISJOINT:
        ordering_add(a,4,u,0); ordering_add(b,6,p,1); ordering_unknown(a); break;
    case ORDER_RESTORE:
        ordering_add(a,4,u,1); ordering_add(a,2,u,1);
        ordering.have_known=1; ordering.known_draw=1; break;
    case ORDER_UNKNOWN_SAME_VALUE:
        ordering_add(a,4,u,1); ordering_add(a,4,u,1); ordering_unknown(a); break;
    case ORDER_KNOWN_SAME_VALUE:
        ordering_add(a,4,p,0); ordering_add(a,4,u,1);
        ordering.have_known=1; ordering.known_draw=1; break;
    case ORDER_PROVEN_IDENTICAL:
        /* Actual proven coverage is deliberately NOT inferred from scissor.
           A copy-only value observation cannot see this identical write. */
        ordering_add(a,4,u,1); ordering_add(a,4,p,1); ordering_unknown(a); break;
    case ORDER_PROVEN_HIDDEN_RESTORE:
        /* Both proven writes are real, but neither changes the final observed
           gap value. Retain unknown rather than fabricate a coverage proof. */
        ordering_add(a,4,u,1); ordering_add(a,6,p,0); ordering_add(a,4,p,1);
        ordering_unknown(a); break;
    case ORDER_PROVEN_BASELINE_RESTORE:
        ordering_add(a,4,u,1); ordering_add(a,6,p,0); ordering_add(a,2,p,1);
        ordering.have_known=1; ordering.known_draw=2; break;
    case ORDER_RESTORE_NEW_PROVEN:
        ordering_add(a,4,u,0); ordering_add(a,6,p,0); ordering_add(a,8,u,1);
        ordering_add(a,6,u,1); ordering.have_known=1; ordering.known_draw=3; break;
    case ORDER_CONTAINED:
        a=ordering_rect(89,63,29,23); b=ordering_rect(95,69,17,11);
        ordering_add(a,4,u,0); ordering_add(b,6,u,1); ordering_unknown(a); break;
    case ORDER_EXPANDING:
        a=ordering_rect(91,65,11,9); b=ordering_rect(89,63,27,21);
        ordering_add(a,4,u,0); ordering_add(b,6,u,1); ordering_unknown(b); break;
    case ORDER_EDGE:
        a=ordering_rect((int)W-7,(int)H-5,17,13);
        ordering_add(a,4,u,1); ordering_unknown(a); break;
    case ORDER_DISJOINT_THIRD:
        c=ordering_rect(193,133,25,19);
        ordering_add(a,4,u,0); ordering_add(b,6,p,0); ordering_add(c,8,u,1);
        ordering_unknown(a); ordering_unknown(c); break;
    case ORDER_EXPT_UNCHANGED:
        ordering_add(a,4,u,1); ordering_add(a,4,e,1); ordering_unknown(a); break;
    case ORDER_EXPT_CHANGED:
        ordering_add(a,4,u,1); ordering_add(a,6,e,1); ordering.admitted=25*19;
        ordering.have_known=1; ordering.known_draw=1; break;
    case ORDER_EXPT_OVERWRITTEN:
        ordering_add(a,4,e,0); ordering_add(a,6,u,1); ordering_unknown(a); break;
    case ORDER_EXPT_SAME_VALUE:
        ordering_add(a,4,e,1); ordering_add(a,4,u,1); ordering.admitted=25*19;
        ordering.have_known=1; ordering.known_draw=1; break;
    case ORDER_EXPT_PROVEN_IDENTICAL:
        ordering_add(a,4,e,1); ordering_add(a,4,p,1); ordering.admitted=25*19;
        ordering.have_known=1; ordering.known_draw=1; break;
    case ORDER_EXPT_PROVEN_THEN_UNKNOWN:
        ordering_add(a,4,e,0); ordering_add(a,6,p,0); ordering_add(a,8,u,1);
        ordering_unknown(a); break;
    case ORDER_UNSAFE46:
        ordering_add(large,4,u,1); ordering_unknown(large); break;
    case ORDER_CAPACITY:
        for (unsigned i=0;i<257;i++) ordering_add(ordering_rect(91,65,1,1),i&1 ? 6 : 4,u,1);
        break;
    default:
        ordering_add(a,4,u,1); break;
    }
    if (ordering.have_known && kind!=ORDER_NOOP_THEN_PROVEN46) {
        ordering.known_x=103; ordering.known_y=74;
    }
    writer_capture_refused=kind>=ORDER_CASES;
}
static int ordering_capture(fixture *f,const float camera[84]) {
    float *mapped=host_map(&ordering.uniform);
    for (unsigned i=0;i<ordering.count;i++) {
        float uniform[8]={0,0,fg_test_depth(ordering.draw[i].distance),0,
            0.2f+0.12f*(i%5)+0.1f*(ordering.frame&1),0.3f,0.6f,1};
        memcpy((uint8_t *)mapped+i*ordering.stride,uniform,sizeof uniform);
    }
    vkUnmapMemory(V.dev,ordering.uniform.memory);
    for (unsigned i=0;i<ordering.count;i++) {
        const ordering_draw *draw=&ordering.draw[i];
        int accepted;
        vk_fg_test_uncertified_fail(ordering.kind==ORDER_FAIL_BEGIN);
        if (draw->classification==VK_FG_WRITER_UNCERTIFIED) {
            unsigned result=vk_fg_test_uncertified_draw_begin(&f->scene_desc,&f->depth_desc,
                camera,!i,i&1,&draw->rect);
            accepted=!!(result&1);
            CHECK(result&2,"ordered %s uses exact opt-in runtime-qualified shader pair",ordering_name(ordering.kind));
        } else accepted=vk_fg_test_writer_draw_begin(&f->scene_desc,&f->depth_desc,camera,!i,
            draw->identity,draw->classification,&draw->rect);
        CHECK(accepted==(ordering.kind!=ORDER_FAIL_BEGIN && !(ordering.kind==ORDER_CAPACITY && i==256)),
            "ordered %s draw%u takes production admission or exact forced refusal",ordering_name(ordering.kind),i);
        vk_fg_test_uncertified_fail(0);
        VkDescriptorSet set=vk_alloc_set(f->ui_dsl);
        VkDescriptorBufferInfo buffer={ordering.uniform.buffer,i*ordering.stride,8*sizeof(float)};
        VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,
            .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
        vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
        vk_record_draw packet={.pipeline=ordering.pipeline,.layout=f->ui_layout,.count=3,.instances=1};
        packet.dynamic.vp=(VkViewport){0,0,W,H,0,1}; packet.dynamic.sc=draw->rect;
        packet.dynamic.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        packet.dynamic.dt=packet.dynamic.dw=1; packet.dynamic.dc=VK_COMPARE_OP_ALWAYS;
        VkDescriptorSet sets[2]={set,0}; vk_fg_replay_draw(vk_cb(),&packet,sets);
        vk_fg_test_uncertified_fail(ordering.kind==ORDER_FAIL_END);
        vk_fg_test_writer_draw_note(draw->close);
        vk_fg_test_uncertified_fail(0);
    }
    if (ordering.kind==ORDER_UNCLOSED)
        CHECK(vk_fg_authority_span_begin(vk_cb(),f->depth,0,0,91,65,25,19,0),
            "unclosed negative records a real private before observation");
    else if (ordering.kind==ORDER_RESET) vk_fg_reset();
    else if (ordering.kind==ORDER_UNSUPPORTED)
        CHECK(!vk_fg_authority_span_begin(vk_cb(),f->scene,0,0,91,65,25,19,0),
            "color image never provides a depth VALUE certificate");
    return vk_fg_test_capture_reads(&f->scene_desc,NULL,0);
}
static unsigned ordering_raw_count(const uint8_t *raw) {
    unsigned count=0;
    for (size_t i=0;i<DEPTH_PIXELS;i++) count+=raw[i]!=0;
    return count;
}
static void ordering_check_depth(fixture *f) {
    float *actual=malloc(DEPTH_PIXELS*sizeof *actual),*expected=malloc(DEPTH_PIXELS*sizeof *expected);
    CHECK(actual && expected,"allocate actual ordered GPU depth golden");
    if (!actual || !expected) exit(1);
    for (size_t i=0;i<DEPTH_PIXELS;i++) expected[i]=fg_test_depth(2);
    for (unsigned i=0;i<ordering.count;i++) {
        VkRect2D r=ordering.draw[i].rect;
        unsigned hi_x=(unsigned)r.offset.x+r.extent.width,hi_y=(unsigned)r.offset.y+r.extent.height;
        if (hi_x>DW) hi_x=DW;
        if (hi_y>DH) hi_y=DH;
        for (unsigned y=(unsigned)r.offset.y;y<hi_y;y++)
            for (unsigned x=(unsigned)r.offset.x;x<hi_x;x++)
                expected[(size_t)y*DW+x]=fg_test_depth(ordering.draw[i].distance);
    }
    read_image_extent(vk_tex_image(f->depth),VK_IMAGE_ASPECT_DEPTH_BIT,actual,
        DEPTH_PIXELS*sizeof *actual,DW,DH);
    unsigned differences=0;
    for (size_t i=0;i<DEPTH_PIXELS;i++) differences+=memcmp(actual+i,expected+i,sizeof *actual)!=0;
    CHECK(!differences,"ordered %s actually renders every planned depth and clips only at image edges (%u mismatches)",
        ordering_name(ordering.kind),differences);
    free(actual); free(expected);
}
/* Independent exact native-pixel dilation, intentionally not the GPU's coarse
   tile algorithm. Every unsafe exact tap must still select the encoded older
   real image; extra conservative tiled fallback is allowed, never less. */
static void ordering_exact_mask(const uint8_t *raw,unsigned rx,unsigned ry,uint8_t *mask) {
    size_t stride=(size_t)DW+1;
    uint32_t *integral=calloc(stride*(DH+1),sizeof *integral);
    CHECK(integral!=NULL,"allocate independent ordered unsafe-tap golden");
    if (!integral) exit(1);
    for (unsigned y=0;y<DH;y++) for (unsigned x=0;x<DW;x++) {
        size_t p=(size_t)(y+1)*stride+x+1;
        integral[p]=(raw[(size_t)y*DW+x]!=0)+integral[p-1]+integral[p-stride]-integral[p-stride-1];
    }
    for (unsigned y=0;y<DH;y++) for (unsigned x=0;x<DW;x++) {
        unsigned x0=x>rx ? x-rx : 0,y0=y>ry ? y-ry : 0;
        unsigned x1=x+rx<DW ? x+rx+1 : DW,y1=y+ry<DH ? y+ry+1 : DH;
        mask[(size_t)y*DW+x]=integral[(size_t)y1*stride+x1]-integral[(size_t)y0*stride+x1]-
            integral[(size_t)y1*stride+x0]+integral[(size_t)y0*stride+x0]!=0;
    }
    free(integral);
}
static void ordering_check_output(const uint8_t *pixels,const uint8_t *earlier,
                                  const uint8_t *old_raw,const uint8_t *current_raw,
                                  const uint32_t pair[12]) {
    uint8_t *mask=malloc(2*DEPTH_PIXELS);
    CHECK(mask!=NULL,"allocate both independent source tap masks");
    if (!mask) exit(1);
    ordering_exact_mask(old_raw,pair[4],pair[5],mask);
    ordering_exact_mask(current_raw,pair[10],pair[11],mask+DEPTH_PIXELS);
    unsigned checked=0,changed=0;
    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
        size_t p=(size_t)y*W+x;
        int unsafe=0;
        for (unsigned source=0;source<2;source++) {
            const uint8_t *m=mask+source*DEPTH_PIXELS;
            unsafe|=m[p]!=0;
            if (x>1 && x+2<W && y>1 && y+2<H)
                unsafe|=m[p-1] || m[p+1] || m[p-W] || m[p+W] || m[p+W+1];
        }
        if (!unsafe) continue;
        checked++; changed+=memcmp(pixels+4*p,earlier+4*p,4)!=0;
    }
    CHECK(!changed,"ordered %s preserves encoded older real at EVERY unsafe exact depth/color tap (%u checked, %u changed)",
        ordering_name(ordering.kind),checked,changed);
    if (ordering_raw_count(old_raw) || ordering_raw_count(current_raw))
        CHECK(checked>0,"unknown ordered source exercises nonempty bitwise fallback golden");
    free(mask);
}
static void ordering_pixels(fixture *f,unsigned frame) {
    fixture_interp_pixels(f,frame&1);
    for (size_t i=0;i<DEPTH_PIXELS;i++) f->depth_pixels[i]=fg_test_depth(2);
    fixture_upload(f);
    const float transparent[4]={0}; fixture_interp_ui(f,transparent);
}
static void ordering_run_case(fixture *f,unsigned kind,int lag,const float camera[84]) {
    unsigned frames=lag ? 4 : 2,source_kind[4]={kind,kind,ORDER_EDGE,ORDER_DISJOINT_THIRD},source_admitted[4];
    uint8_t *raw=malloc(4*DEPTH_PIXELS),*real=malloc(4*PIXELS*4),*pixels=malloc(PIXELS*4);
    uint8_t *retained=malloc(3*PIXELS*4);
    uint32_t saved_pair[3][12],saved_result[3][4],saved_experimental[3][2];
    vk_post_output output[3];
    CHECK(raw && real && pixels && retained,"allocate ordered retained source/output goldens");
    if (!raw || !real || !pixels || !retained) exit(1);
    vk_fg_reset();
    for (unsigned frame=0;frame<frames;frame++) {
        ordering_plan(source_kind[frame]); ordering.frame=frame;
        source_admitted[frame]=ordering.admitted;
        memcpy(raw+frame*DEPTH_PIXELS,ordering.raw,DEPTH_PIXELS);
        ordering_pixels(f,frame);
        vk_post_output out;
        uint64_t span_before[5],span_after[5];
        vk_fg_test_authority_span_counts(span_before);
        int generated=real_frame(f,camera,frame,0,&out);
        vk_fg_test_authority_span_counts(span_after);
        if (ordering.kind==ORDER_CONTAINED || ordering.kind==ORDER_EXPANDING ||
            ordering.kind==ORDER_EDGE || ordering.kind==ORDER_NOOP_THEN_PROVEN46) {
            uint64_t spans=ordering.kind==ORDER_EXPANDING ? 2 : 1;
            uint64_t copied=ordering.kind==ORDER_CONTAINED ? 2*29*23 :
                ordering.kind==ORDER_EXPANDING ? 2*(11*9+27*21) :
                ordering.kind==ORDER_EDGE ? 2*7*5 : 2*DEPTH_PIXELS;
            CHECK(span_after[0]-span_before[0]==spans && span_after[1]-span_before[1]==spans &&
                span_after[2]-span_before[2]==spans && span_after[3]-span_before[3]==copied &&
                span_after[4]==span_before[4],
                "ordered %s copies only clipped native rectangles, batches contained cross-pair draws and splits expansion",
                ordering_name(ordering.kind));
        }
        CHECK(generated==(frame!=0),"ordered %s frame%u records exact adjacent source pair",ordering_name(kind),frame);
        ordering_check_depth(f);
        read_image(vk_tex_image(f->final),VK_IMAGE_ASPECT_COLOR_BIT,real+frame*PIXELS*4,PIXELS*4);
        for (size_t p=0;p<PIXELS;p++) real[(frame*PIXELS+p)*4+3]=255;
        if (!generated) continue;
        unsigned index=frame-1;
        output[index]=out;
        CHECK(authority.have_result && authority.have_pair && experimental.have_result,
            "ordered %s frame%u exposes exact fenced ordinary/experimental pair census",ordering_name(kind),frame);
        unsigned old_count=ordering_raw_count(raw+(frame-1)*DEPTH_PIXELS);
        unsigned current_count=ordering_raw_count(raw+frame*DEPTH_PIXELS);
        CHECK(authority.result[0]==DW && authority.result[1]==DH && authority.result[2]==current_count,
            "ordered %s frame%u raw unknown=%u, not an unordered before-FIRST/final difference",
            ordering_name(kind),frame,current_count);
        CHECK(authority.pair[0]==DW && authority.pair[1]==DH && authority.pair[2]==old_count &&
            authority.pair[6]==DW && authority.pair[7]==DH && authority.pair[8]==current_count,
            "each output owns its older/current source's exact raw VALUE census");
        CHECK(experimental.admitted[0]==source_admitted[frame-1] && experimental.admitted[1]==source_admitted[frame],
            "ordered %s counts only still-final experimental assumptions, never unchanged laundering",ordering_name(ordering.kind));
        memcpy(saved_pair[index],authority.pair,sizeof authority.pair);
        memcpy(saved_result[index],authority.result,sizeof authority.result);
        memcpy(saved_experimental[index],experimental.admitted,sizeof experimental.admitted);
        if (out.image) {
            read_image(out.image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
            ordering_check_output(pixels,real+(frame-1)*PIXELS*4,
                raw+(frame-1)*DEPTH_PIXELS,raw+frame*DEPTH_PIXELS,authority.pair);
            memcpy(retained+index*PIXELS*4,pixels,PIXELS*4);
            if (frame==1 && ordering.have_known) {
                float rgb[3]={0.25f+0.12f*(ordering.known_draw%5),0.3f,0.6f};
                check_interp_linear(pixels,ordering.known_x,ordering.known_y,rgb,
                    "ordered recertified VALUE generates true midpoint, not older-real fallback");
                if (kind==ORDER_NOOP_THEN_PROVEN46)
                    CHECK(memcmp(pixels+((size_t)ordering.known_y*W+ordering.known_x)*4,
                        real+((size_t)ordering.known_y*W+ordering.known_x)*4,3),
                        "46-percent ordinary late geometry no longer yields a false fallback image");
            }
        }
        for (unsigned held=0;held<index;held++) {
            uint32_t pair[12],result[4],admitted[2];
            CHECK(vk_fg_test_authority_pair(held+1,pair) && !memcmp(pair,saved_pair[held],sizeof pair) &&
                vk_fg_test_authority_result(held+1,result) && !memcmp(result,saved_result[held],sizeof result) &&
                vk_fg_test_uncertified_result(held+1,admitted) &&
                !memcmp(admitted,saved_experimental[held],sizeof admitted),
                "later disjoint spans never mutate a pinned earlier output's private ordinary/experimental source/pair census");
            if (output[held].image) {
                read_image(output[held].image,VK_IMAGE_ASPECT_COLOR_BIT,pixels,PIXELS*4);
                CHECK(!memcmp(pixels,retained+held*PIXELS*4,PIXELS*4),
                    "later frames cannot overwrite any pixel of a still-pinned generated output");
            }
        }
    }
    for (unsigned frame=1;frame<frames;frame++) {
        int safe=source_kind[frame-1]!=ORDER_UNSAFE46 && source_kind[frame]!=ORDER_UNSAFE46;
        CHECK(vk_fg_complete(frame)==safe,
            "ordered %s output%u keeps unchanged35-percent refusal and independent source retirement",ordering_name(kind),frame);
    }
    free(raw); free(real); free(pixels); free(retained);
}
static void ordering_failures(fixture *f,const float camera[84]) {
    for (unsigned fault=ORDER_FAIL_BEGIN;fault<ORDER_FAULTS;fault++) {
        vk_fg_reset();
        for (unsigned frame=0;frame<5;frame++) {
            ordering_plan(frame==2 ? fault : ORDER_KNOWN_SAME_VALUE); ordering.frame=frame;
            ordering_pixels(f,frame);
            vk_post_output out;
            int generated=real_frame(f,camera,frame%4,0,&out);
            CHECK(generated==(frame==1 || frame==4),
                "ordered %s refuses exact failed source, clears history and requires fresh adjacent recovery frame%u",
                ordering_name(fault),frame);
            if (generated) CHECK(vk_fg_complete(frame%4),"valid pair around ordered %s remains independently releasable",ordering_name(fault));
            else {
                uint32_t pair[12],result[4];
                CHECK(!authority.have_result && !authority.have_pair &&
                    !vk_fg_test_authority_pair(frame%4,pair) && !vk_fg_test_authority_result(frame%4,result),
                    "ordered %s never exposes a stale mask/census from a previous output",ordering_name(fault));
            }
        }
    }
}
static int run_interp_authority_ordering(int lag) {
    setenv("TOTK_FRAMEGEN","interp",1); setenv("TOTK_FRAMEGEN_BUDGET_MS","1000",1);
    setenv("TOTK_FRAMEGEN_UNKNOWN_PERCENT","35",1);
    setenv("TOTK_FRAMEGEN_UNCERTIFIED_PLANE","1",1);
    unsetenv("TOTK_FRAMEGEN_WRITER_DIAG");
    mtl_init();
    CHECK(vk_fg_enabled() && vk_fg_interpolating() && vk_fg_uncertified_plane() && !vk_fg_writer_diag(),
        "ordered production fixture enables explicit experiment but no diagnostic-induced span boundaries");
    fixture f; fixture_create(&f); f.ui_repeats=1;
    ordering.pipeline=replay_pipeline(f.ui_layout,VK_FORMAT_R32G32B32A32_SFLOAT,3);
    ordering.stride=V.props.limits.minUniformBufferOffsetAlignment;
    if (ordering.stride<8*sizeof(float)) ordering.stride=8*sizeof(float);
    ordering.uniform=host_create(257*ordering.stride,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    ordering.raw=malloc(DEPTH_PIXELS);
    CHECK(ordering.raw!=NULL,"allocate explicit raw VALUE goldens");
    if (!ordering.raw) exit(1);
    writer_capture=ordering_capture; authority.defer_complete=1;
    float camera[84]; fg_test_camera(camera,0,0,0,0); camera[65]=(1-camera[38])*0.5f;
    for (unsigned kind=0;kind<ORDER_CASES;kind++) ordering_run_case(&f,kind,lag,camera);
    ordering_failures(&f,camera);
    writer_capture=NULL; writer_capture_refused=0; authority.defer_complete=0;
    vk_gpu_idle(); vkDestroyPipeline(V.dev,ordering.pipeline,NULL); host_destroy(&ordering.uniform);
    free(ordering.raw); ordering.raw=NULL;
    mtl_tex_release(&f.scene_desc); mtl_tex_release(&f.depth_desc); mtl_tex_release(&f.final_desc); mtl_tex_release(&f.canvas_desc);
    fixture_replay_destroy(&f); free(f.pool.mem); free(f.scene_pixels); free(f.depth_pixels);
    free(f.depth_baseline); free(f.final_pixels);
    return failures ? 1 : 0;
}
