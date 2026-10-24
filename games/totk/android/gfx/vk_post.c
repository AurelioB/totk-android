/* Optional presentation SMAA 1x and shared FSR1/SGSR1 spatial filters.
   Post intermediates belong to fenced present slots. Scene targets belong to
   their existing texture/source-ring owner and are filtered before any UI. */
#include "vk_int.h"
#include "vk_ui_aspect.h"
#include "vk_post_spv.h"
#include "vk_post_lut.h"
#include "vk_present_slots.h"
#include "vk_overlay.h"
#include <math.h>

enum { POST_EDGE, POST_WEIGHTS, POST_BLEND, POST_EASU, POST_RCAS, POST_SGSR, POST_STAGES, POST_SLOTS = VK_PRESENT_SLOT_CAPACITY };
enum { POST_SCALE_OFF, POST_SCALE_FSR, POST_SCALE_SGSR };
typedef struct {
    VkImage image; VkImageView raw, display; vk_alloc memory;
    uint32_t w, h; VkFormat format;
} post_image;
typedef struct { post_image image[POST_STAGES]; int timed; } post_slot;
static struct {
    int native_ui, aa, scene_aa, scale, scene_scale, ready, uploaded;
    float sharpness, scene_sharpness, sgsr_sharpness, sgsr_edge_threshold;
    VkBool32 sgsr_edge_direction;
    VkSampler sampler; VkDescriptorSetLayout dsl; VkPipelineLayout layout;
    VkPipeline pipeline[POST_STAGES]; VkQueryPool queries;
    post_image area, search; post_slot slot[POST_SLOTS];
    struct { VkFormat format; VkPipeline pipeline[3]; } scene[8];
    unsigned nscene;
    struct { VkFormat format; VkPipeline blend; } scene_aa_pipeline[8];
    unsigned nscene_aa;
} P;
#ifdef TOTK_VK_TESTS
static uint64_t scene_aa_runs;
uint64_t vk_scene_test_aa_runs(void) { return scene_aa_runs; }
#endif

static void post_image_free(post_image *im) {
    if (im->display && im->display != im->raw) vkDestroyImageView(V.dev, im->display, NULL);
    if (im->raw) vkDestroyImageView(V.dev, im->raw, NULL);
    if (im->image) vkDestroyImage(V.dev, im->image, NULL);
    vk_mem_free(&im->memory); memset(im, 0, sizeof *im);
}
static void post_image_make(post_image *im, uint32_t w, uint32_t h, VkFormat format, int lookup) {
    if (im->image && im->w == w && im->h == h && im->format == format) return;
    post_image_free(im);
    VkFormat raw = format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : format;
    VkFormat formats[2] = {raw, format};
    VkImageFormatListCreateInfo list = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = 2, .pViewFormats = formats};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, raw != format ? &list : NULL,
        .flags = raw != format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {w,h,1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            (lookup ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)};
    VKCHECK(vkCreateImage(V.dev, &ci, NULL, &im->image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, im->image, &req);
    if (!vk_mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->memory)) DIE("post image allocation %ux%u",w,h);
    VKCHECK(vkBindImageMemory(V.dev, im->image, im->memory.mem, im->memory.off));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = raw, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VKCHECK(vkCreateImageView(V.dev,&vi,NULL,&im->raw));
    im->display = im->raw;
    if (raw != format) { vi.format = format; VKCHECK(vkCreateImageView(V.dev,&vi,NULL,&im->display)); }
    im->w=w; im->h=h; im->format=format;
}
static void post_transition(VkCommandBuffer cb, post_image *im, VkImageLayout old, VkImageLayout next,
                            VkPipelineStageFlags2 src, VkAccessFlags2 read, VkPipelineStageFlags2 dst, VkAccessFlags2 write) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask=src,.srcAccessMask=read,.dstStageMask=dst,.dstAccessMask=write,
        .oldLayout=old,.newLayout=next,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=im->image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo d={VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};
    vkCmdPipelineBarrier2(cb,&d);
}
static float post_sgsr_option(const char *name, float fallback) {
    const char *value = getenv(name); if (!value || !*value) return fallback;
    char *end; float parsed = strtof(value, &end);
    return !*end && isfinite(parsed) && parsed >= 0 ? parsed : fallback;
}
static VkResult post_pipeline_make(unsigned stage, VkFormat format, VkShaderModule vertex,
        VkShaderModule fragment, int hdr, VkPipeline *pipeline) {
    VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att={.colorWriteMask=0xf};
    VkPipelineColorBlendStateCreateInfo blend={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&att};
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic};
    VkPipelineRenderingCreateInfo rendering={VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
    VkBool32 values[2]={P.sgsr_edge_direction,hdr};
    VkSpecializationMapEntry entries[2]={{0,0,sizeof(VkBool32)},{1,sizeof(VkBool32),sizeof(VkBool32)}};
    VkSpecializationInfo spec={.mapEntryCount=2,.pMapEntries=entries,.dataSize=sizeof values,.pData=values};
    VkPipelineShaderStageCreateInfo stages[2]={
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main",
            .pSpecializationInfo=stage==POST_SGSR?&spec:NULL}};
    VkGraphicsPipelineCreateInfo pi={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,&rendering,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,
        .pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&blend,.pDynamicState=&dy,.layout=P.layout};
    return vk_pcache_graphics(&pi,pipeline);
}
void vk_post_init(void) {
    const char *aa=getenv("TOTK_POST_AA"), *scale=getenv("TOTK_POST_SCALE"), *sharp=getenv("TOTK_POST_SHARPNESS");
    const char *native_ui=getenv("TOTK_NATIVE_UI");
    P.native_ui=native_ui && !strcmp(native_ui,"1"); /* frontend validates 0/1 before graphics startup */
    const char *scene=getenv("TOTK_SCENE_SCALE");
    P.scene_scale=scene&&!strcmp(scene,"sgsr")?POST_SCALE_SGSR:scene&&!strcmp(scene,"fsr")?POST_SCALE_FSR:POST_SCALE_OFF;
    if (scene && *scene && strcmp(scene,"0") && strcmp(scene,"off") && !P.scene_scale)
        LOGV("[scene-scale] refusing TOTK_SCENE_SCALE=%s: use off, sgsr or fsr; existing path retained",scene);
    const char *scene_aa=getenv("TOTK_SCENE_AA");
    P.scene_aa=scene_aa && !strcmp(scene_aa,"smaa");
    if (scene_aa && *scene_aa && strcmp(scene_aa,"0") && strcmp(scene_aa,"off") && !P.scene_aa)
        LOGV("[scene-aa] refusing TOTK_SCENE_AA=%s: use off or smaa; existing path retained",scene_aa);
#ifdef TOTK_GAME_V143
    if (P.scene_scale) {
        LOGV("[scene-scale] %s requested; pre-UI canvas -> spatial upscale -> native UI -> terminal -> window; native composition effective=1 (requested=%s); residual game scene -> canvas filter retained",
            scene,native_ui?native_ui:"unset");
        P.native_ui=1;
    }
    if (P.scene_aa) {
        P.native_ui=1;
        LOGV("[scene-aa] SMAA1x requested: pre-UI canvas AA -> optional scene upscale -> native UI; real seeds only");
    }
#else
    if (P.scene_scale) { LOGV("[scene-scale] refusing: requires 1.4.3 canvas profile"); P.scene_scale=0; }
    if (P.scene_aa) { LOGV("[scene-aa] refusing: requires 1.4.3 canvas profile"); P.scene_aa=0; }
#endif
    const char *framegen=getenv("TOTK_FRAMEGEN");
    vk_canvas_init(P.native_ui,framegen && (!strcmp(framegen,"1") || !strcmp(framegen,"interp") || !strcmp(framegen,"hybrid") || !strcmp(framegen,"locked") || !strcmp(framegen,"adaptive")),
        vk_ui_aspect_tracks(vk_ui_aspect_requested(getenv("TOTK_ASPECT"))));
#ifndef TOTK_GAME_V143
    if (P.native_ui) DIE("TOTK_NATIVE_UI=1 requires game version 1.4.3 (TOTK_GAME_V143)");
#endif
    P.aa=aa && !strcmp(aa,"smaa");
    P.scale=scale && !strcmp(scale,"fsr")?POST_SCALE_FSR:scale && !strcmp(scale,"sgsr")?POST_SCALE_SGSR:POST_SCALE_OFF;
    if (P.native_ui) {
        if (P.scale!=POST_SCALE_OFF)
            LOGV("native UI v2: ignoring TOTK_POST_SCALE=%s (no host FSR/RCAS/SGSR; UI host target is native-sized)",scale);
        P.scale=POST_SCALE_OFF;
    }
    if (!P.aa && !P.scale && !P.scene_scale && !P.scene_aa) return;
    P.sharpness=0.2f;
    if (sharp && *sharp) { char *end; float f=strtof(sharp,&end); if (!*end && isfinite(f)) P.sharpness=fminf(1,fmaxf(0,f)); }
    P.scene_sharpness=0.2f;
    const char *scene_sharp=getenv("TOTK_SCENE_SHARPNESS");
    if (scene_sharp && *scene_sharp) { char *end; float f=strtof(scene_sharp,&end); if (!*end && isfinite(f)) P.scene_sharpness=fminf(1,fmaxf(0,f)); }
    P.sgsr_sharpness=post_sgsr_option("TOTK_POST_SGSR_SHARPNESS",2.0f);
    P.sgsr_edge_threshold=post_sgsr_option("TOTK_POST_SGSR_EDGE_THRESHOLD",8.0f/255.0f);
    const char *direction=getenv("TOTK_POST_SGSR_EDGE_DIRECTION");
    P.sgsr_edge_direction=direction && !strcmp(direction,"1");
    VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VKCHECK(vkCreateSampler(V.dev,&si,NULL,&P.sampler));
    VkDescriptorSetLayoutBinding bindings[4];
    for (unsigned i=0;i<4;i++) bindings[i]=(VkDescriptorSetLayoutBinding){.binding=i,
        .descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.descriptorCount=1,
        .stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT,.pImmutableSamplers=&P.sampler};
    VkDescriptorSetLayoutCreateInfo ds={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=4,.pBindings=bindings};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev,&ds,NULL,&P.dsl));
    VkPushConstantRange range={VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,64};
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&P.dsl,
        .pushConstantRangeCount=1,.pPushConstantRanges=&range};
    VKCHECK(vkCreatePipelineLayout(V.dev,&li,NULL,&P.layout));
    const uint32_t *code[]={post_vert_spv,post_edge_spv,post_weights_spv,post_blend_spv,post_easu_spv,post_rcas_spv,post_sgsr_spv};
    const size_t bytes[]={sizeof post_vert_spv,sizeof post_edge_spv,sizeof post_weights_spv,sizeof post_blend_spv,sizeof post_easu_spv,sizeof post_rcas_spv,sizeof post_sgsr_spv};
    VkShaderModule modules[POST_STAGES+1]={0};
    const int enabled[POST_STAGES]={P.aa||P.scene_aa,P.aa||P.scene_aa,P.aa,P.scale==POST_SCALE_FSR,P.scale==POST_SCALE_FSR,P.scale==POST_SCALE_SGSR};
    for (unsigned i=0;i<POST_STAGES+1;i++) {
        if (i && !enabled[i-1]) continue;
        VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=bytes[i],.pCode=code[i]};
        VKCHECK(vkCreateShaderModule(V.dev,&mi,NULL,&modules[i]));
    }
    for (unsigned i=0;i<POST_STAGES;i++) if (modules[i+1])
        VKCHECK(post_pipeline_make(i,i==POST_EDGE?VK_FORMAT_R8G8_UNORM:VK_FORMAT_R8G8B8A8_UNORM,
            modules[0],modules[i+1],0,&P.pipeline[i]));
    for (unsigned i=0;i<POST_STAGES+1;i++) if (modules[i]) vkDestroyShaderModule(V.dev,modules[i],NULL);
    if (P.aa || P.scene_aa) { post_image_make(&P.area,160,560,VK_FORMAT_R8G8_UNORM,1); post_image_make(&P.search,64,16,VK_FORMAT_R8_UNORM,1); }
    if (V.timestamp_bits && V.props.limits.timestampPeriod>0) {
        VkQueryPoolCreateInfo qi={VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=POST_SLOTS*2};
        VKCHECK(vkCreateQueryPool(V.dev,&qi,NULL,&P.queries));
    } else LOGV("post GPU timing unavailable on this queue");
    P.ready=1;
    const char *scale_name=P.scale==POST_SCALE_SGSR?"SGSR1":P.scale==POST_SCALE_FSR?"FSR1":"off";
    LOGV("post: AA=%s scale=%s sharpness=%.2f (display-encoded RGB)",P.aa?"SMAA1x":"off",scale_name,P.sharpness);
    if (P.scale==POST_SCALE_SGSR)
        LOGV("post SGSR1: EdgeSharpness=%.4g EdgeThreshold=%.6g UseEdgeDirection=%u (single pass)",
             P.sgsr_sharpness,P.sgsr_edge_threshold,P.sgsr_edge_direction);
}
int vk_native_ui_enabled(void) { return P.native_ui; }
int vk_post_enabled(void) { return P.ready && (P.aa || P.scale); }
int vk_post_scale(void) { return P.scale!=POST_SCALE_OFF; }
int vk_scene_scale_enabled(void) { return P.ready && P.scene_scale; }
int vk_scene_scale_fsr(void) { return P.scene_scale==POST_SCALE_FSR; }
int vk_scene_aa_enabled(void) { return P.ready && P.scene_aa; }
int vk_scene_post_enabled(void) { return P.ready && (P.scene_aa || P.scene_scale); }
static void post_upload(VkCommandBuffer cb, post_image *im, const void *bytes, size_t n) {
    VkBuffer buffer; VkDeviceSize offset; void *mapped=vk_transient(n,4,&buffer,&offset); memcpy(mapped,bytes,n);
    post_transition(cb,im,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r={.bufferOffset=offset,.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={im->w,im->h,1}};
    vkCmdCopyBufferToImage(cb,buffer,im->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&r);
    post_transition(cb,im,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
static void post_draw_pipeline(VkCommandBuffer cb, unsigned stage, post_image *dst, VkImageView source, VkImageView aux, const float push[16], VkPipeline pipeline) {
    /* This slot is idle; discarding its previous contents needs no CPU wait. */
    post_transition(cb,dst,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkDescriptorSet set=vk_alloc_set(P.dsl);
    int lut=(P.aa || P.scene_aa) && stage<=POST_BLEND;
    VkDescriptorImageInfo images[4]={
        {P.sampler,source,VK_IMAGE_LAYOUT_GENERAL},{P.sampler,aux?aux:source,VK_IMAGE_LAYOUT_GENERAL},
        {P.sampler,lut?P.area.raw:source,lut?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL},
        {P.sampler,lut?P.search.raw:source,lut?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4];
    for (unsigned i=0;i<4;i++) writes[i]=(VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo=&images[i]};
    vkUpdateDescriptorSets(V.dev,4,writes,0,NULL);
    VkRenderingAttachmentInfo attachment={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=dst->raw,
        .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp=stage==POST_EDGE?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.clearValue.color={{0,0,0,0}}};
    VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{dst->w,dst->h}},.layerCount=1,
        .colorAttachmentCount=1,.pColorAttachments=&attachment};
    vkCmdBeginRendering(cb,&ri);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,P.layout,0,1,&set,0,NULL);
    vkCmdPushConstants(cb,P.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,64,push);
    VkViewport viewport={0,0,(float)dst->w,(float)dst->h,0,1}; VkRect2D scissor={{0,0},{dst->w,dst->h}};
    vkCmdSetViewport(cb,0,1,&viewport); vkCmdSetScissor(cb,0,1,&scissor); vkCmdDraw(cb,3,1,0,0); vkCmdEndRendering(cb);
    post_transition(cb,dst,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
}
static void post_draw(VkCommandBuffer cb, unsigned stage, post_image *dst, VkImageView source, VkImageView aux, const float push[16]) {
    post_draw_pipeline(cb,stage,dst,source,aux,push,P.pipeline[stage]);
}
int vk_scene_scale_run(VkCommandBuffer cb, VkImageView source, uint32_t sw, uint32_t sh,
        int32_t cx, int32_t cy, uint32_t cw, uint32_t ch,
        const vk_post_output *target, const vk_post_output *scratch) {
    if (!vk_scene_scale_enabled()) return 0;
    if (!source || !target || !target->image || !target->view || !cw || !ch ||
        cx<0 || cy<0 || (uint64_t)cx+cw>sw || (uint64_t)cy+ch>sh ||
        !target->w || !target->h || target->w>V.props.limits.maxImageDimension2D ||
        target->h>V.props.limits.maxImageDimension2D) return 0;
    unsigned index=0;
    for (;index<P.nscene;index++) if (P.scene[index].format==target->format) break;
    if (index==P.nscene) {
        if (index==8) return 0;
        P.scene[index].format=target->format; P.nscene++;
        VkFormatProperties properties; V.get_format_props(V.pd,target->format,&properties);
        VkFormatFeatureFlags need=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures&need)!=need) {
            static int warned;
            if (!warned) { warned=1; LOGV("[scene-scale] refusing format=%u: color attachment/linear sampling unsupported; linear scene seed retained",target->format); }
            return 0;
        }
        VkShaderModule vertex=VK_NULL_HANDLE, fragment=VK_NULL_HANDLE;
        VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof post_vert_spv,.pCode=post_vert_spv};
        if (vkCreateShaderModule(V.dev,&mi,NULL,&vertex)) return 0;
        const uint32_t *code[]={post_easu_spv,post_rcas_spv,post_sgsr_spv};
        const size_t bytes[]={sizeof post_easu_spv,sizeof post_rcas_spv,sizeof post_sgsr_spv};
        int hdr=target->format==VK_FORMAT_R16G16B16A16_SFLOAT || target->format==VK_FORMAT_R32G32B32A32_SFLOAT ||
            target->format==VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        int ok=1;
        for (unsigned j=0;j<3 && ok;j++) {
            if (P.scene_scale==POST_SCALE_SGSR ? j!=2 : j==2) continue;
            mi.codeSize=bytes[j]; mi.pCode=code[j];
            ok=vkCreateShaderModule(V.dev,&mi,NULL,&fragment)==VK_SUCCESS;
            if (ok) {
                ok=post_pipeline_make(POST_EASU+j,target->format,vertex,fragment,hdr,&P.scene[index].pipeline[j])==VK_SUCCESS;
                vkDestroyShaderModule(V.dev,fragment,NULL);
            }
        }
        vkDestroyShaderModule(V.dev,vertex,NULL);
        if (!ok) {
            for (unsigned j=0;j<3;j++) if (P.scene[index].pipeline[j]) {
                vkDestroyPipeline(V.dev,P.scene[index].pipeline[j],NULL); P.scene[index].pipeline[j]=VK_NULL_HANDLE;
            }
            LOGV("[scene-scale] refusing format=%u pipeline; linear scene seed retained",target->format);
            return 0;
        }
    }
    if (!P.scene[index].pipeline[P.scene_scale==POST_SCALE_SGSR?2:0]) return 0;
    float push[16]={(float)cx,(float)cy,(float)cw,(float)ch,(float)target->w,(float)target->h,(float)sw,(float)sh,P.scene_sharpness};
    post_image dst={.image=target->image,.raw=target->view,.w=target->w,.h=target->h};
    vk_barrier_all(cb);
    if (P.scene_scale==POST_SCALE_FSR) {
        if (!scratch || !scratch->image || !scratch->view || scratch->w!=target->w || scratch->h!=target->h || scratch->format!=target->format) return 0;
        post_image tmp={.image=scratch->image,.raw=scratch->view,.w=scratch->w,.h=scratch->h};
        post_draw_pipeline(cb,POST_EASU,&tmp,source,0,push,P.scene[index].pipeline[0]);
        float local[16]={0,0,(float)target->w,(float)target->h,(float)target->w,(float)target->h,(float)target->w,(float)target->h,P.scene_sharpness};
        post_draw_pipeline(cb,POST_RCAS,&dst,scratch->view,0,local,P.scene[index].pipeline[1]);
    } else {
        push[8]=P.sgsr_sharpness; push[9]=P.sgsr_edge_threshold;
        push[12]=(float)cw/target->w; push[13]=(float)ch/target->h;
        push[14]=1.0f/sw; push[15]=1.0f/sh;
        post_draw_pipeline(cb,POST_SGSR,&dst,source,0,push,P.scene[index].pipeline[2]);
    }
    static int once;
    if (!once) {
        once=1;
        LOGV("[scene-scale] active: render=%s -> %ux%u canvas crop=%d,%d+%ux%u -> %ux%u %s -> native UI -> equal-size terminal/present; format=%u RCAS=%.3f; interpolation retains the real upscale; extrapolation scales before UI",
            getenv("TOTK_RENDER_RES")?getenv("TOTK_RENDER_RES"):"game/DRS",sw,sh,cx,cy,cw,ch,target->w,target->h,
            P.scene_scale==POST_SCALE_FSR?"FSR1 EASU+RCAS":"SGSR1",target->format,P.scene_sharpness);
    }
    return 1;
}
/* Scratch images belong to the native canvas backing, not present slots.
   A repeated UI resolve must never call this again for that real frame. */
int vk_scene_post_run(VkCommandBuffer cb, VkImageView source, uint32_t sw, uint32_t sh,
        int32_t cx, int32_t cy, uint32_t cw, uint32_t ch,
        const vk_post_output *target, const vk_post_output *scratch, const vk_post_output aa[3]) {
    if (!vk_scene_post_enabled()) return 0;
    if (!P.scene_aa) return vk_scene_scale_run(cb,source,sw,sh,cx,cy,cw,ch,target,scratch);
    if (!source || !target || !target->image || !target->view || !cw || !ch ||
        cx<0 || cy<0 || (uint64_t)cx+cw>sw || (uint64_t)cy+ch>sh ||
        !target->w || !target->h || !aa) return 0;
    const VkFormat formats[3]={VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,target->format};
    for (unsigned j=0;j<3;j++)
        if (!aa[j].image || !aa[j].view || aa[j].w!=cw || aa[j].h!=ch || aa[j].format!=formats[j] ||
            aa[j].image==target->image) return 0;
    unsigned index=0;
    for (;index<P.nscene_aa;index++) if (P.scene_aa_pipeline[index].format==target->format) break;
    if (index==P.nscene_aa) {
        if (index==8) return 0;
        VkShaderModule vertex=VK_NULL_HANDLE,fragment=VK_NULL_HANDLE;
        VkShaderModuleCreateInfo mi={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof post_vert_spv,.pCode=post_vert_spv};
        VKCHECK(vkCreateShaderModule(V.dev,&mi,NULL,&vertex));
        mi.codeSize=sizeof post_blend_spv; mi.pCode=post_blend_spv;
        VKCHECK(vkCreateShaderModule(V.dev,&mi,NULL,&fragment));
        VkResult result=post_pipeline_make(POST_BLEND,target->format,vertex,fragment,0,&P.scene_aa_pipeline[index].blend);
        vkDestroyShaderModule(V.dev,fragment,NULL); vkDestroyShaderModule(V.dev,vertex,NULL);
        if (result) return 0;
        P.scene_aa_pipeline[index].format=target->format; P.nscene_aa++;
    }
    if (!P.uploaded) {
        post_upload(cb,&P.area,post_area_bytes,sizeof post_area_bytes);
        post_upload(cb,&P.search,post_search_bytes,sizeof post_search_bytes); P.uploaded=1;
    }
    vk_barrier_all(cb);
    float push[16]={(float)cx,(float)cy,(float)cw,(float)ch,(float)cw,(float)ch,(float)sw,(float)sh};
    post_image images[3];
    for (unsigned j=0;j<3;j++) images[j]=(post_image){.image=aa[j].image,.raw=aa[j].view,.w=cw,.h=ch};
    post_draw(cb,POST_EDGE,&images[0],source,0,push);
    post_draw(cb,POST_WEIGHTS,&images[1],source,aa[0].view,push);
    post_draw_pipeline(cb,POST_BLEND,&images[2],source,aa[1].view,push,P.scene_aa_pipeline[index].blend);
#ifdef TOTK_VK_TESTS
    scene_aa_runs++;
#endif
    if (P.scene_scale) return vk_scene_scale_run(cb,aa[2].view,cw,ch,0,0,cw,ch,target,scratch);
    vk_barrier_all(cb);
    post_image dst={.image=target->image};
    post_transition(cb,&dst,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BLIT_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)cw,(int32_t)ch,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int32_t)target->w,(int32_t)target->h,1}}};
    vkCmdBlitImage(cb,aa[2].image,VK_IMAGE_LAYOUT_GENERAL,target->image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_LINEAR);
    vk_barrier_all(cb);
    return 1;
}
int vk_post_run(VkCommandBuffer cb, unsigned slot, VkImageView source, uint32_t source_w, uint32_t source_h,
                int32_t cx, int32_t cy, uint32_t w, uint32_t h, uint32_t output_w, uint32_t output_h, int srgb, vk_post_output *out) {
    if (!vk_post_enabled()) return 0;
    if (slot>=POST_SLOTS || !source || !w || !h || !output_w || !output_h) DIE("invalid post chain input");
    post_slot *s=&P.slot[slot];
    VkFormat format=srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
    if (P.aa && !P.uploaded) {
        post_upload(cb,&P.area,post_area_bytes,sizeof post_area_bytes);
        post_upload(cb,&P.search,post_search_bytes,sizeof post_search_bytes); P.uploaded=1;
    }
    for (unsigned j=0;j<POST_STAGES;j++) if (P.pipeline[j])
        post_image_make(&s->image[j],j<POST_EASU?w:output_w,j<POST_EASU?h:output_h,
            j==POST_EDGE?VK_FORMAT_R8G8_UNORM:j==POST_WEIGHTS?VK_FORMAT_R8G8B8A8_UNORM:format,0);
    if (P.queries) {
        vkCmdResetQueryPool(cb,P.queries,slot*2,2);
        vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,P.queries,slot*2); s->timed=1;
    }
    float push[16]={(float)cx,(float)cy,(float)w,(float)h,(float)w,(float)h,(float)source_w,(float)source_h,P.sharpness};
    post_image *result=NULL;
    if (P.aa) {
        post_draw(cb,POST_EDGE,&s->image[POST_EDGE],source,0,push);
        post_draw(cb,POST_WEIGHTS,&s->image[POST_WEIGHTS],source,s->image[POST_EDGE].raw,push);
        post_draw(cb,POST_BLEND,&s->image[POST_BLEND],source,s->image[POST_WEIGHTS].raw,push);
        result=&s->image[POST_BLEND]; source=result->raw;
        push[0]=push[1]=0; push[6]=w; push[7]=h;
    }
    if (P.scale==POST_SCALE_FSR) {
        push[4]=output_w; push[5]=output_h;
        post_draw(cb,POST_EASU,&s->image[POST_EASU],source,0,push);
        float local[16]={0,0,(float)output_w,(float)output_h,(float)output_w,(float)output_h,(float)output_w,(float)output_h,P.sharpness};
        post_draw(cb,POST_RCAS,&s->image[POST_RCAS],s->image[POST_EASU].raw,0,local);
        result=&s->image[POST_RCAS];
    }
    if (P.scale==POST_SCALE_SGSR) {
        push[4]=output_w; push[5]=output_h;
        push[8]=P.sgsr_sharpness; push[9]=P.sgsr_edge_threshold;
        push[12]=(float)w/output_w; push[13]=(float)h/output_h;
        push[14]=1.0f/push[6]; push[15]=1.0f/push[7];
        post_draw(cb,POST_SGSR,&s->image[POST_SGSR],source,0,push);
        result=&s->image[POST_SGSR];
    }
    if (P.queries) vkCmdWriteTimestamp2(cb,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,P.queries,slot*2+1);
    *out=(vk_post_output){result->image,result->display,result->w,result->h,result->format};
    vk_overlay_set_scale_stage(VK_OVERLAY_STAGE_HOST,
        P.scale == POST_SCALE_SGSR ? VK_OVERLAY_SCALE_SGSR :
        P.scale == POST_SCALE_FSR ? VK_OVERLAY_SCALE_FSR1 : VK_OVERLAY_SCALE_NONE);
    return 1;
}
void vk_post_complete(unsigned slot) {
    if (!P.ready || slot>=POST_SLOTS || !P.slot[slot].timed) return;
    P.slot[slot].timed=0;
    uint64_t t[2];
    VkResult r=vkGetQueryPoolResults(V.dev,P.queries,slot*2,2,sizeof t,t,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
    if (r) { LOGV("post GPU timestamps unavailable after present fence (%d)",r); return; }
    uint64_t mask=V.timestamp_bits>=64?UINT64_MAX:(UINT64_C(1)<<V.timestamp_bits)-1;
    double ms=(double)((t[1]-t[0])&mask)*V.props.limits.timestampPeriod/1e6;
    static double sum,max; static unsigned count;
    sum+=ms; if (ms>max) max=ms;
    if (++count==300) {
        LOGV("post GPU last 300 frames: mean %.3f ms, max %.3f ms, latest %.3f ms (SMAA=%d FSR1=%d SGSR1=%d)",
             sum/300,max,ms,P.aa,P.scale==POST_SCALE_FSR,P.scale==POST_SCALE_SGSR);
        sum=max=0; count=0;
    }
}
