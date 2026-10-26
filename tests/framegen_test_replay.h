/* Synthetic, owned shader/data fixture for the production frozen-packet emitter. */
#include "../games/totk/android/gfx/vk_framegen_test_spv.h"
/* ui=2 deliberately uses RGB transmittance unrelated to source alpha. */
static VkPipeline replay_pipeline(VkPipelineLayout layout, VkFormat format, int ui_mode) {
    int ui=ui_mode==1 || ui_mode==2, depth=ui_mode==3;
    const uint32_t *code[2] = {ui ? test_ui_vert_spv : test_transfer_vert_spv,
        depth ? test_depth_frag_spv : ui ? test_ui_frag_spv : test_transfer_frag_spv};
    size_t bytes[2] = {ui ? sizeof test_ui_vert_spv : sizeof test_transfer_vert_spv,
        depth ? sizeof test_depth_frag_spv : ui ? sizeof test_ui_frag_spv : sizeof test_transfer_frag_spv};
    VkShaderModule module[2];
    for (unsigned i=0;i<2;i++) {
        VkShaderModuleCreateInfo ci={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes[i],.pCode=code[i]};
        VKCHECK(vkCreateShaderModule(V.dev,&ci,NULL,&module[i]));
    }
    VkPipelineShaderStageCreateInfo stages[2]={
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=module[0],.pName="main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=module[1],.pName="main"}};
    VkVertexInputBindingDescription binding={0,2*sizeof(float),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute={0,0,VK_FORMAT_R32G32_SFLOAT,0};
    VkPipelineVertexInputStateCreateInfo vertex={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount=ui,.pVertexBindingDescriptions=&binding,.vertexAttributeDescriptionCount=ui,.pVertexAttributeDescriptions=&attribute};
    VkPipelineInputAssemblyStateCreateInfo assembly={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo raster={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo samples={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment={.blendEnable=ui,
        .srcColorBlendFactor=ui_mode==2 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor=ui_mode==2 ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp=VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp=VK_BLEND_OP_ADD,.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&attachment};
    const VkDynamicState state[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_CULL_MODE,VK_DYNAMIC_STATE_FRONT_FACE,VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,VK_DYNAMIC_STATE_STENCIL_OP,VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dynamic={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount=sizeof state/sizeof state[0],.pDynamicStates=state};
    VkPipelineRenderingCreateInfo rendering={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,
        .pColorAttachmentFormats=&format,.depthAttachmentFormat=depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED};
    VkPipelineDepthStencilStateCreateInfo ds={VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vertex,.pInputAssemblyState=&assembly,.pViewportState=&viewport,.pRasterizationState=&raster,
        .pMultisampleState=&samples,.pColorBlendState=&blend,.pDynamicState=&dynamic,.pDepthStencilState=depth ? &ds : NULL,.layout=layout};
    VkPipeline pipeline; VKCHECK(vkCreateGraphicsPipelines(V.dev,VK_NULL_HANDLE,1,&ci,NULL,&pipeline));
    for(unsigned i=0;i<2;i++) vkDestroyShaderModule(V.dev,module[i],NULL);
    return pipeline;
}
static VkDescriptorSet replay_image_set(fixture *f, VkImageView view) {
    VkDescriptorSet set=vk_alloc_set(f->transfer_dsl);
    VkDescriptorImageInfo image={f->ui_sampler,view,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.dstBinding=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&image};
    vkUpdateDescriptorSets(V.dev,1,&write,0,NULL); return set;
}
static void fixture_replay_init(fixture *f) {
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.descriptorCount=1,
        .stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo dsl={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev,&dsl,NULL,&f->ui_dsl));
    binding.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; binding.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VKCHECK(vkCreateDescriptorSetLayout(V.dev,&dsl,NULL,&f->transfer_dsl));
    VkPipelineLayoutCreateInfo layout={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&f->ui_dsl};
    VKCHECK(vkCreatePipelineLayout(V.dev,&layout,NULL,&f->ui_layout));
    layout.pSetLayouts=&f->transfer_dsl; VKCHECK(vkCreatePipelineLayout(V.dev,&layout,NULL,&f->transfer_layout));
    f->ui_pipeline=replay_pipeline(f->ui_layout,
        fixture_rgb1110 ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R32G32B32A32_SFLOAT,1);
    f->transfer_pipeline=replay_pipeline(f->transfer_layout,VK_FORMAT_R8G8B8A8_UNORM,0);
    VkSamplerCreateInfo sampler={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_NEAREST,.minFilter=VK_FILTER_NEAREST,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VKCHECK(vkCreateSampler(V.dev,&sampler,NULL,&f->ui_sampler));
    f->ui_vertices=host_create(6*sizeof(float),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    f->ui_uniform=host_create(8*sizeof(float),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float vertices[]={-1,-1,3,-1,-1,3};
    float uniform[]={0,0,0,0,1,1,1,1};
    memcpy(f->guest_vertices,vertices,sizeof vertices); memcpy(f->guest_uniform,uniform,sizeof uniform);
    /* These are the real draw's immutable per-frame copies. Replay gets only
       their Vulkan handles, never either guest address. */
    memcpy(host_map(&f->ui_vertices),f->guest_vertices,sizeof vertices); vkUnmapMemory(V.dev,f->ui_vertices.memory);
    memcpy(host_map(&f->ui_uniform),f->guest_uniform,sizeof uniform); vkUnmapMemory(V.dev,f->ui_uniform.memory);
    if (replay_uniform_mutation.enabled) {
        size_t offset=f->pool.size-4096;
        f->ui_uniform_address=(uint64_t)(uintptr_t)(f->pool.mem+offset);
        memcpy((void *)(uintptr_t)f->ui_uniform_address,uniform,sizeof uniform);
        mtl_pool_add((void *)(uintptr_t)f->ui_uniform_address,4096);
        uint64_t live_offset=0;
        CHECK(vk_resolve(f->ui_uniform_address,sizeof uniform,&f->ui_uniform_live.buffer,&live_offset),
            "resolve pool-backed UI uniform through the production guest-buffer resolver");
        if (!f->ui_uniform_live.buffer) exit(1);
        f->ui_uniform_live.offset=live_offset;
        f->ui_uniform_live.range=sizeof uniform;
    }
    f->ui_repeats=128;
}
static void replay_real_begin_extent(VkCommandBuffer cb, VkImageView view, unsigned w, unsigned h) {
    VkRenderingAttachmentInfo attachment={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
        .loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo rendering={VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{w,h}},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&attachment};
    vkCmdBeginRendering(cb,&rendering);
}
static void replay_real_begin(VkCommandBuffer cb, VkImageView view) {
    replay_real_begin_extent(cb,view,W,H);
}
static int fixture_render_replay(fixture *f, VkCommandBuffer cb, int prepared) {
    int spatial=native_ui_terminal && vk_scene_post_enabled();
    image_barrier(cb,vk_tex_image(f->scene),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    image_barrier(cb,vk_tex_image(f->canvas),VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL);
    VkImageCopy copy={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={W,H,1}};
    vkCmdCopyImage(cb,vk_tex_image(f->scene),VK_IMAGE_LAYOUT_GENERAL,vk_tex_image(f->canvas),VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    vk_barrier_all(cb);
    if (spatial) {
        vk_native_frame_end();
        vk_tex_native_promote(f->canvas);
        vk_tex_native_written(f->canvas,0);
        vk_tex_native_ui_begin(f->canvas);
    }
    VkDescriptorSet ui_set=vk_alloc_set(f->ui_dsl);
    VkDescriptorBufferInfo buffer=f->ui_uniform_live.buffer ? f->ui_uniform_live :
        (VkDescriptorBufferInfo){f->ui_uniform.buffer,0,8*sizeof(float)};
    if (replay_uniform_mutation.enabled) {
        VkDescriptorBufferInfo frozen={0};
        VkDeviceSize align=V.props.limits.minUniformBufferOffsetAlignment;
        if (!align) align=16;
        int captured=vk_ub_replay_read(f->ui_uniform_address,8*sizeof(float),align,&frozen);
        CHECK(captured,"production replay freeze snapshots the pool-backed original UI uniform bytes");
        if (captured && !replay_uniform_mutation.negative_live) buffer=frozen;
    }
    VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ui_set,.dstBinding=0,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.pBufferInfo=&buffer};
    vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
    vk_record_draw ui={.pipeline=f->ui_pipeline,.layout=f->ui_layout,.count=3,.instances=1,.vertex_mask=1};
    ui.dynamic.vp=(VkViewport){0,0,W,H,0,1};
    ui.dynamic.sc=replay_uniform_mutation.enabled ?
        (VkRect2D){{W/2-40,H/2-30},{80,60}} : (VkRect2D){{220,30},{40,30}};
    ui.dynamic.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    ui.vertex[0].buffer=f->ui_vertices.buffer; ui.vertex[0].size=6*sizeof(float); ui.vertex[0].stride=2*sizeof(float);
    VkDescriptorSet sets[2]={ui_set,0};
    unsigned ui_w=spatial?native_terminal_w:W,ui_h=spatial?native_terminal_h:H;
    if (spatial) {
        ui.dynamic.vp.width=ui_w; ui.dynamic.vp.height=ui_h;
        ui.dynamic.sc=(VkRect2D){{330,45},{60,45}};
    }
    VkImageView real_canvas=spatial?vk_tex_native_view(f->canvas,NULL,1):vk_tex_view(f->canvas,&f->canvas_desc,NULL,0);
    replay_real_begin_extent(cb,real_canvas,ui_w,ui_h);
    VkClearAttachment clear={VK_IMAGE_ASPECT_COLOR_BIT,0,{.color={{0,1,0,1}}}};
    VkClearRect clear_rect={{{10,10},{8,8}},0,1};
    if (spatial) clear_rect.rect=(VkRect2D){{15,15},{12,12}};
    if (!replay_no_ui) {
        for(unsigned i=0;i<f->ui_repeats;i++) vk_fg_replay_draw(cb,&ui,sets);
        vkCmdClearAttachments(cb,1,&clear,1,&clear_rect);
    }
    vkCmdEndRendering(cb); vk_barrier_all(cb); f->real_passes++;
    if (spatial) vk_tex_native_written(f->canvas,1);
    if (replay_uniform_mutation.enabled && prepared) {
        const float changed_color[4]={0,0,0,0.35f};
        vkCmdUpdateBuffer(cb,f->ui_uniform_live.buffer,f->ui_uniform_live.offset+4*sizeof(float),
            sizeof changed_color,changed_color);
        VkBufferMemoryBarrier2 uniform_barrier={VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask=VK_ACCESS_2_UNIFORM_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=f->ui_uniform_live.buffer,
            .offset=f->ui_uniform_live.offset,.size=8*sizeof(float)};
        VkDependencyInfo uniform_dependency={VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&uniform_barrier};
        vkCmdPipelineBarrier2(cb,&uniform_dependency);
    }
    vk_record_draw transfer={.pipeline=f->transfer_pipeline,.layout=f->transfer_layout,.count=3,.instances=1};
    unsigned terminal_w=native_ui_terminal ? native_terminal_w : W;
    unsigned terminal_h=native_ui_terminal ? native_terminal_h : H;
    transfer.dynamic.vp=(VkViewport){0,0,terminal_w,terminal_h,0,1};
    transfer.dynamic.sc=(VkRect2D){{0,0},{terminal_w,terminal_h}};
    transfer.dynamic.topo=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkDescriptorSet real_sets[2]={replay_image_set(f,spatial?vk_tex_native_view(f->canvas,NULL,0):vk_tex_view(f->canvas,&f->canvas_desc,NULL,0)),0};
    VkImageView terminal_view=f->final_view;
    if (native_ui_terminal) {
        terminal_view=vk_tex_native_view(f->final,NULL,1);
        CHECK(terminal_view!=VK_NULL_HANDLE,"native terminal rendering attaches the actual sibling view");
    }
    replay_real_begin_extent(cb,terminal_view,terminal_w,terminal_h); vk_fg_replay_draw(cb,&transfer,real_sets);
    if (native_ui_terminal && !scene_scale_fixture) {
        /* Native-only pixels cannot be recovered by sampling the guest backing.
           Put a different real-frame color inside the unsafe-depth footprint. */
        VkClearAttachment native_clear={VK_IMAGE_ASPECT_COLOR_BIT,0,
            {.color={{authority.frame ? 0.75f : 0.125f,0.75f,0.25f,173.0f/255.0f}}}};
        VkClearRect native_rect={{{138,102},{30,18}},0,1};
        vkCmdClearAttachments(cb,1,&native_clear,1,&native_rect);
    }
    vkCmdEndRendering(cb); vk_barrier_all(cb); f->real_passes++;
    if (native_ui_terminal) vk_tex_native_written(f->final,1);
    if (prepared && vk_fg_interpolating())
        CHECK(vk_fg_capture_real(cb,f->final,0,0),"capture the actual encoded real terminal before synthetic replay");
    /* Simulate the game producing the next frame before synthetic work. A
       delayed guest-memory re-read would move/recolour the HUD and fail. */
    for(unsigned i=0;i<6;i++) f->guest_vertices[i]=1000;
    for(unsigned i=0;i<8;i++) f->guest_uniform[i]=0;
    if (!prepared) return 0;
    int depth=vk_fg_replay_depth(cb,NULL,0,0);
    CHECK(depth,"UI replay needs no live guest depth");
    if (!depth) return 0;
    int interp=vk_fg_interpolating();
    for (unsigned pass=0;pass<(interp ? 2u : 1u);pass++) {
        int final=pass ? 2 : 0;
        int begun=vk_fg_replay_begin(cb,ui_w,ui_h,replay_probe_format,final);
        CHECK(begun,"%s",interp ? "begin frozen UI capture over black/white" : "begin one batched private UI pass");
        if (!begun) return 0;
        if (!replay_no_ui) {
            for(unsigned i=0;i<f->ui_repeats;i++) vk_fg_replay_draw(cb,&ui,sets);
            vkCmdClearAttachments(cb,1,&clear,1,&clear_rect);
        }
        vk_fg_replay_end(cb,final);
    }
    /* Warmup and rejected pairs still retain both current UI layers. Only a
       composed pair has a defined canvas for the terminal transfer to sample. */
    if (interp && !vk_fg_interp_compose(cb)) return 0;
    VkDescriptorSet generated_sets[2]={replay_image_set(f,vk_fg_canvas_view()),0};
    int begun=vk_fg_replay_begin(cb,terminal_w,terminal_h,VK_FORMAT_R8G8B8A8_UNORM,1);
    CHECK(begun,"begin exact terminal transfer replay");
    if (!begun) return 0;
    vk_fg_replay_draw(cb,&transfer,generated_sets); vk_fg_replay_end(cb,1);
    return 1;
}
static void fixture_replay_destroy(fixture *f) {
    mtl_tex_release(&f->canvas_desc);
    if (f->ui_uniform_address) mtl_pool_del((void *)(uintptr_t)f->ui_uniform_address);
    host_destroy(&f->ui_uniform); host_destroy(&f->ui_vertices);
    vkDestroyPipeline(V.dev,f->ui_pipeline,NULL); vkDestroyPipeline(V.dev,f->transfer_pipeline,NULL);
    vkDestroyPipelineLayout(V.dev,f->ui_layout,NULL); vkDestroyPipelineLayout(V.dev,f->transfer_layout,NULL);
    vkDestroyDescriptorSetLayout(V.dev,f->ui_dsl,NULL); vkDestroyDescriptorSetLayout(V.dev,f->transfer_dsl,NULL);
    vkDestroySampler(V.dev,f->ui_sampler,NULL);
}
