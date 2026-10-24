/* Real Vulkan host acceptance test for the optional post-processing chain.
   Fresh processes: TOTK_POST_AA=smaa|off, TOTK_POST_SCALE=sgsr|fsr|off.
   SGSR without SMAA additionally compares computed, independent CPU golden images.
   TOTK_NATIVE_UI=1 must disable either stale scaler request; SMAA remains crop-sized.
   GPU-only benchmark (run both scales with the same device and environment):
     TOTK_POST_AA=off TOTK_POST_SCALE=sgsr ./vkposttest --benchmark 1280x720 1920x1080 300
     TOTK_POST_AA=off TOTK_POST_SCALE=fsr  ./vkposttest --benchmark 1280x720 1920x1080 300
   No device, Android AHB, game shader cache or window is involved. */
#include "vk_int.h"
#include "vk_present_slots.h"
#include "test_sgsr_reference.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

#define SRC_W 128u
#define SRC_H 96u
#define CX 16
#define CY 16
#define CROP_W 96u
#define CROP_H 64u
#define UPSCALE_W 192u
#define UPSCALE_H 128u

typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; uint32_t w, h; VkFormat format; } test_image;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkDeviceSize size; } test_buffer;

static void image_create(test_image *im, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    memset(im, 0, sizeof *im); im->w = w; im->h = h; im->format = format;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(V.dev, &ci, NULL, &im->image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, im->image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &im->memory)); VKCHECK(vkBindImageMemory(V.dev, im->image, im->memory, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKCHECK(vkCreateImageView(V.dev, &vi, NULL, &im->view));
}
static void image_destroy(test_image *im) {
    if (im->view) vkDestroyImageView(V.dev, im->view, NULL);
    if (im->image) vkDestroyImage(V.dev, im->image, NULL);
    if (im->memory) vkFreeMemory(V.dev, im->memory, NULL);
}
static void buffer_create(test_buffer *b, VkDeviceSize size, VkBufferUsageFlags usage) {
    memset(b, 0, sizeof *b); b->size = size;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; ci.size = size; ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(V.dev, &ci, NULL, &b->buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b->buffer, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)vk_pick_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &b->memory)); VKCHECK(vkBindBufferMemory(V.dev, b->buffer, b->memory, 0));
}
static void buffer_destroy(test_buffer *b) {
    if (b->buffer) vkDestroyBuffer(V.dev, b->buffer, NULL);
    if (b->memory) vkFreeMemory(V.dev, b->memory, NULL);
}
static void *buffer_map(test_buffer *b) {
    void *p = NULL; VKCHECK(vkMapMemory(V.dev, b->memory, 0, b->size, 0, &p)); return p;
}
static VkCommandPool command_pool(void) {
    VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.queueFamilyIndex = V.qfam;
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool; VKCHECK(vkCreateCommandPool(V.dev, &ci, NULL, &pool)); return pool;
}
static VkCommandBuffer command_begin(VkCommandPool pool) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb; VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb, &bi)); return cb;
}
static void command_finish(VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkCommandBufferSubmitInfo command = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &command};
    VKCHECK(vkQueueSubmit2(V.queue, 1, &si, VK_NULL_HANDLE));
    VKCHECK(vkQueueWaitIdle(V.queue));   /* test-owned upload/readback only; production post never waits */
}
static void barrier_image(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                          VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = src_stage; b.dstStageMask = dst_stage;
    b.srcAccessMask = src_access; b.dstAccessMask = dst_access; b.oldLayout = old_layout; b.newLayout = new_layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &dependency);
}
static void make_source_pixels(uint8_t pixels[SRC_W * SRC_H * 4]) {
    for (uint32_t y = 0; y < SRC_H; y++) for (uint32_t x = 0; x < SRC_W; x++) {
        uint8_t *p = pixels + (y * SRC_W + x) * 4;
        /* Distinct magenta crop margins must not enter postprocessed output. */
        if (x < CX || x >= CX + CROP_W || y < CY || y >= CY + CROP_H) { p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255; }
        else {
            /* Two hard, high-contrast aliased diagonals with broad flat regions. */
            uint32_t u = x - CX, v = y - CY;
            int white = (int)(u * 2u) >= (int)(v * 3u) + 7 && (int)(u * 2u) < (int)(v * 3u) + 34;
            uint8_t c = white ? 255 : 0; p[0] = p[1] = p[2] = c; p[3] = 255;
        }
    }
}
static void upload_pixels(VkCommandPool pool, test_image *src, const uint8_t *pixels, VkImageLayout old_layout) {
    size_t bytes = (size_t)src->w * src->h * 4;
    test_buffer staging; buffer_create(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(buffer_map(&staging), pixels, bytes); vkUnmapMemory(V.dev, staging.memory);
    VkCommandBuffer cb = command_begin(pool);
    barrier_image(cb, src->image, old_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0}; copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = (VkExtent3D){src->w, src->h, 1};
    vkCmdCopyBufferToImage(cb, staging.buffer, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier_image(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    command_finish(cb); buffer_destroy(&staging);
}
static void upload_source(VkCommandPool pool, test_image *src) {
    uint8_t pixels[SRC_W * SRC_H * 4]; make_source_pixels(pixels);
    upload_pixels(pool, src, pixels, VK_IMAGE_LAYOUT_UNDEFINED);
}
static void copy_to_host_layout(VkCommandPool pool, VkImage image, VkImageLayout layout, uint32_t w, uint32_t h, uint8_t *pixels) {
    test_buffer readback; buffer_create(&readback, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBuffer cb = command_begin(pool);
    barrier_image(cb, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy = {0}; copy.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy); command_finish(cb);
    memcpy(pixels, buffer_map(&readback), (size_t)w * h * 4); vkUnmapMemory(V.dev, readback.memory); buffer_destroy(&readback);
}
static void copy_image_to_host(VkCommandPool pool, VkImage image, uint32_t w, uint32_t h, uint8_t *pixels) {
    copy_to_host_layout(pool, image, VK_IMAGE_LAYOUT_GENERAL, w, h, pixels);
}
static void reference_blit(VkCommandPool pool, test_image *src, test_image *dst) {
    VkCommandBuffer cb = command_begin(pool);
    barrier_image(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier_image(cb, dst->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit = {0}; blit.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = (VkOffset3D){CX, CY, 0}; blit.srcOffsets[1] = (VkOffset3D){CX + (int32_t)CROP_W, CY + (int32_t)CROP_H, 1};
    blit.dstSubresource = blit.srcSubresource; blit.dstOffsets[1] = (VkOffset3D){(int32_t)CROP_W, (int32_t)CROP_H, 1};
    vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    barrier_image(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier_image(cb, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    command_finish(cb);
}
static unsigned colored_crop_margin(const uint8_t *p, uint32_t w, uint32_t h) {
    unsigned n = 0; for (uint32_t i = 0; i < w * h; i++) if (p[i*4] > 180 && p[i*4+2] > 180 && p[i*4+1] < 60) n++; return n;
}
static unsigned gray_transition_pixels(const uint8_t *p, uint32_t w, uint32_t h) {
    unsigned n = 0;
    for (uint32_t y = 1; y + 1 < h; y++) for (uint32_t x = 1; x + 1 < w; x++) {
        const uint8_t *q = p + (y * w + x) * 4; if (q[0] > 12 && q[0] < 243 && abs((int)q[0] - q[1]) < 3 && abs((int)q[0] - q[2]) < 3) {
            int neighbor = 0; const int d[] = {-1, 1, -(int)w, (int)w};
            for (int k = 0; k < 4; k++) { int v = p[(size_t)((int)(y*w+x)+d[k])*4]; if (v < 8 || v > 247) neighbor++; }
            if (neighbor) n++;
        }
    }
    return n;
}

static float sgsr_control(const char *name, float fallback) {
    const char *value = getenv(name);
    if (value && *value) {
        char *end; float f = strtof(value, &end);
        if (!*end && isfinite(f) && f >= 0) return f;
    }
    return fallback;
}

typedef struct { uint64_t absolute; unsigned max, bad_rgb, bad_alpha; } image_error;
static image_error compare_pixels(const uint8_t *actual, const uint8_t *expected, size_t pixels, unsigned tolerance) {
    image_error error = {0};
    for (size_t i = 0; i < pixels; i++) {
        for (unsigned c = 0; c < 3; c++) {
            unsigned d = (unsigned)abs((int)actual[i * 4 + c] - expected[i * 4 + c]);
            error.absolute += d;
            if (d > error.max) error.max = d;
            if (d > tolerance) error.bad_rgb++;
        }
        if (actual[i * 4 + 3] != expected[i * 4 + 3]) error.bad_alpha++;
    }
    return error;
}

static int render_golden_case(VkCommandPool pool, const test_image *src, uint32_t cx, uint32_t cy,
                               uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, int srgb, uint8_t *pixels) {
    const unsigned slot = VK_PRESENT_SLOT_CAPACITY - 1;
    vk_post_output out = {0};
    mtl_sub_begin();
    int worked = vk_post_run(vk_cb(), slot, src->view, src->w, src->h, cx, cy, cw, ch, w, h, srgb, &out);
    uint64_t done = mtl_sub_commit();
    int complete = mtl_wait(done, 10000000000ull);
    CHECK(complete, "SGSR golden submission completes");
    if (!complete) return 0;
    vk_post_complete(slot);
    CHECK(worked && out.image && out.view, "SGSR golden output exists");
    CHECK(out.w == w && out.h == h, "SGSR golden/resize extent %ux%u expected %ux%u", out.w, out.h, w, h);
    CHECK(out.format == (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM),
          "SGSR golden/resize output format agrees with sRGB=%d", srgb);
    if (!worked || !out.image || out.w != w || out.h != h) return 0;
    copy_image_to_host(pool, out.image, w, h, pixels);
    return 1;
}

static void sgsr_golden_cases(void) {
    enum { GX = 17, GY = 13, GW = 80, GH = 56, MAX_W = GW * 2, MAX_H = GH * 2 };
    /* Keep the two-code numeric tolerance tight, but model both legal hardware
     * bilinear paths independently: fractional float and rounded UNORM8 samples.
     * A tiny sample difference can cross the SGSR threshold and change RGB by
     * 23 codes; raising tolerance would hide real filter errors. Require ONE
     * sampler model to match the ENTIRE image suite, not a per-pixel choice. */
    CHECK(V.props.limits.subTexelPrecisionBits >= 8,
          "SGSR tight golden requires >=8 subtexel bits (device reports %u)", V.props.limits.subTexelPrecisionBits);
    float sharp = sgsr_control("TOTK_POST_SGSR_SHARPNESS", 2.0f);
    float threshold = sgsr_control("TOTK_POST_SGSR_EDGE_THRESHOLD", 8.0f / 255.0f);
    const char *direction = getenv("TOTK_POST_SGSR_EDGE_DIRECTION");
    int edge_direction = direction && !strcmp(direction, "1");
    printf("SGSR golden: direction=%d sharpness=%.9g threshold=%.9g; RGB tolerance=2 codes, alpha/flat exact; subtexel=%u\n",
           edge_direction, sharp, threshold, V.props.limits.subTexelPrecisionBits);
    VkCommandPool pool = command_pool();
    uint8_t *source = VK_MALLOC(SRC_W * SRC_H * 4);
    uint8_t *actual = VK_MALLOC(MAX_W * MAX_H * 4), *expected = VK_MALLOC(MAX_W * MAX_H * 4);
    uint8_t *alternate = VK_MALLOC(MAX_W * MAX_H * 4);
    sgsr_ref_source input = {source, SRC_W, GX, GY, GW, GH};
    uint64_t selected_error[2] = {0}, alternate_error[2][3] = {{0}}, separation[2][3] = {{0}};
    unsigned valid_models = 3;
    size_t total_pixels = 0;
    for (unsigned pattern = 0; pattern < SGSR_PATTERNS; pattern++) {
        test_image src;
        image_create(&src, SRC_W, SRC_H, VK_FORMAT_R8G8B8A8_UNORM,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        sgsr_fixture(source, SRC_W, SRC_H, GX, GY, GW, GH, pattern, 0);
        upload_pixels(pool, &src, source, VK_IMAGE_LAYOUT_UNDEFINED);
        for (unsigned ratio = 2; ratio <= 4; ratio++) for (int srgb = 0; srgb <= 1; srgb++) {
            uint32_t w = GW * ratio / 2, h = GH * ratio / 2;
            size_t count = (size_t)w * h, bytes = count * 4;
            if (!render_golden_case(pool, &src, GX, GY, GW, GH, w, h, srgb, actual)) continue;
            unsigned case_models = 0;
            for (unsigned model = 0; model < 2; model++) {
                sgsr_reference(&input, w, h, sharp, threshold, edge_direction, model, expected);
                image_error error = compare_pixels(actual, expected, count, pattern == SGSR_FLAT ? 0 : 2);
                printf("SGSR golden %-16s %ux%u %s sampler=%s: max=%u mean=%.6f RGB codes, bad=%u alpha=%u\n",
                       sgsr_pattern_names[pattern], w, h, srgb ? "sRGB " : "UNORM", model ? "unorm8" : "float",
                       error.max, error.absolute / (double)(count * 3), error.bad_rgb, error.bad_alpha);
                if (!error.bad_rgb && !error.bad_alpha) case_models |= 1u << model;
                selected_error[model] += error.absolute;
                /* Alternate-control images also come only from original input
                 * bytes, never GPU readback. Threshold 4 is always bilinear. */
                for (unsigned control = 0; control < 3; control++) {
                    float other_sharp = control == 0 ? (sharp == 0 ? 2.0f : 0.0f) : sharp;
                    float other_threshold = control == 1 ? (threshold >= 3 ? 8.0f / 255.0f : 4.0f) : threshold;
                    int other_direction = control == 2 ? !edge_direction : edge_direction;
                    sgsr_reference(&input, w, h, other_sharp, other_threshold, other_direction, model, alternate);
                    separation[model][control] += compare_pixels(expected, alternate, count, 0).absolute;
                    alternate_error[model][control] += compare_pixels(actual, alternate, count, 0).absolute;
                }
            }
            valid_models &= case_models;
            CHECK(case_models, "%s %.1fx sRGB=%d matches an independent SGSR1 golden with RGB tolerance=2",
                  sgsr_pattern_names[pattern], ratio / 2.0, srgb);
            total_pixels += count;
            if (pattern == SGSR_CROP_BORDER) {
                /* Both bilinear and every gather footprint cross this crop.
                 * Replacing all outside bytes must leave GPU output bit exact. */
                sgsr_fixture(source, SRC_W, SRC_H, GX, GY, GW, GH, pattern, 1);
                upload_pixels(pool, &src, source, VK_IMAGE_LAYOUT_GENERAL);
                if (render_golden_case(pool, &src, GX, GY, GW, GH, w, h, srgb, alternate))
                    CHECK(!memcmp(actual, alternate, bytes), "SGSR %.1fx sRGB=%d is byte-exact under outside-crop replacement", ratio / 2.0, srgb);
                sgsr_fixture(source, SRC_W, SRC_H, GX, GY, GW, GH, pattern, 0);
                upload_pixels(pool, &src, source, VK_IMAGE_LAYOUT_GENERAL);
            }
        }
        image_destroy(&src);
    }
    CHECK(valid_models, "one coherent hardware-sampler golden model matches the complete suite (mask=%u)", valid_models);
    unsigned model = valid_models & 2 ? 1 : 0;
    printf("SGSR golden sampler model: %s (valid mask=%u)\n", model ? "unorm8" : "float", valid_models);
    const char *control_names[] = {"sharpness", "threshold", "edge-direction"};
    for (unsigned k = 0; k < 3; k++) {
        printf("SGSR %s control: CPU separation=%llu, actual L1 chosen=%llu alternative=%llu\n",
               control_names[k], (unsigned long long)separation[model][k], (unsigned long long)selected_error[model],
               (unsigned long long)alternate_error[model][k]);
        /* Some controls legitimately collapse another control (sharpness zero
         * or threshold 4). Require discrimination when their images separate. */
        if (separation[model][k] > selected_error[model] * 2)
            CHECK(alternate_error[model][k] > selected_error[model], "actual SGSR output distinguishes the configured %s control", control_names[k]);
    }
    CHECK(total_pixels > 0, "SGSR golden images were compared");
    free(source); free(actual); free(expected); free(alternate); vkDestroyCommandPool(V.dev, pool, NULL);
}
static vk_post_output scene_target(const test_image *im) {
    return (vk_post_output){im->image,im->view,im->w,im->h,im->format};
}
static void scene_aa_images(test_image images[3],uint32_t w,uint32_t h,VkFormat format) {
    VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_create(&images[0],w,h,VK_FORMAT_R8G8_UNORM,usage);
    image_create(&images[1],w,h,VK_FORMAT_R8G8B8A8_UNORM,usage);
    image_create(&images[2],w,h,format,usage);
}
static int scene_filter(VkCommandBuffer cb,const test_image *src,int cx,int cy,uint32_t cw,uint32_t ch,
        const vk_post_output *dst,const vk_post_output *scratch,const test_image images[3]) {
    vk_post_output aa[3];
    for (unsigned j=0;j<3;j++) aa[j]=scene_target(&images[j]);
    return vk_scene_post_run(cb,src->view,src->w,src->h,cx,cy,cw,ch,dst,scratch,aa);
}
static void scene_linear(VkCommandBuffer cb,const test_image *source,const test_image *target,
        int cx,int cy,uint32_t cw,uint32_t ch) {
    barrier_image(cb,target->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .srcOffsets={{cx,cy,0},{cx+(int)cw,cy+(int)ch,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .dstOffsets={{0,0,0},{(int)target->w,(int)target->h,1}}};
    vkCmdBlitImage(cb,source->image,VK_IMAGE_LAYOUT_GENERAL,target->image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_LINEAR);
    vk_barrier_all(cb);
}
static double scene_sharpness(const uint8_t *pixels,uint32_t w,uint32_t h) {
    double metric=0;
    for (uint32_t y=2;y+2<h;y++) for (uint32_t x=2;x+2<w;x++) {
        size_t p=((size_t)y*w+x)*4;
        double lap=4.0*pixels[p]-pixels[p-4]-pixels[p+4]-pixels[p-w*4]-pixels[p+w*4];
        metric+=lap*lap;
    }
    return metric/((w-4)*(h-4));
}
static int scene_cases(void) {
    CHECK(vk_scene_post_enabled() && vk_native_ui_enabled(),"scene opt-in enables pre-UI filter and host composition");
    CHECK(!vk_post_scale(),"scene mode never enables post-UI enlargement");
    VkCommandPool pool=command_pool();
    const unsigned crops[][6]={{17,13,80,56,240,120},{0,0,96,64,192,160},{23,19,72,48,216,144}};
    for (unsigned test=0;test<3;test++) {
        unsigned cx=crops[test][0],cy=crops[test][1],cw=crops[test][2],ch=crops[test][3];
        double fit=fmin((double)crops[test][4]/cw,(double)crops[test][5]/ch);
        uint32_t ow=(uint32_t)(cw*fit+.5),oh=(uint32_t)(ch*fit+.5);
        test_image src,compact,dst,other,tmp,linear;
        test_image aa[3]={0};
        if (vk_scene_aa_enabled()) scene_aa_images(aa,cw,ch,VK_FORMAT_R8G8B8A8_UNORM);
        VkImageUsageFlags target_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_create(&src,SRC_W,SRC_H,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        image_create(&compact,cw,ch,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
        image_create(&dst,ow,oh,VK_FORMAT_R8G8B8A8_UNORM,target_usage);
        image_create(&other,ow,oh,VK_FORMAT_R8G8B8A8_UNORM,target_usage);
        image_create(&tmp,ow,oh,VK_FORMAT_R8G8B8A8_UNORM,target_usage);
        image_create(&linear,ow,oh,VK_FORMAT_R8G8B8A8_UNORM,target_usage);
        uint8_t pixels[SRC_W*SRC_H*4],cropped[SRC_W*SRC_H*4];
        sgsr_fixture(pixels,SRC_W,SRC_H,cx,cy,cw,ch,SGSR_COLOR,0);
        for (unsigned i=3;i<sizeof pixels;i+=4) pixels[i]=255; /* opaque scene; unlike SGSR's alpha-discard golden */
        for (unsigned y=0;y<ch;y++) memcpy(cropped+(size_t)y*cw*4,pixels+((size_t)(cy+y)*SRC_W+cx)*4,cw*4);
        upload_pixels(pool,&src,pixels,VK_IMAGE_LAYOUT_UNDEFINED);
        upload_pixels(pool,&compact,cropped,VK_IMAGE_LAYOUT_UNDEFINED);
        vk_post_output target=scene_target(&dst),scratch=scene_target(&tmp),second=scene_target(&other);
        mtl_sub_begin(); VkCommandBuffer cb=vk_cb();
        CHECK(scene_filter(cb,&src,cx,cy,cw,ch,&target,&scratch,aa),"scene cropped filter records");
        CHECK(scene_filter(cb,&compact,0,0,cw,ch,&second,&scratch,aa),"same scene with zero-origin allocation records");
        scene_linear(cb,&src,&linear,cx,cy,cw,ch);
        CHECK(mtl_wait(mtl_sub_commit(),10000000000ull),"scene test submission retires");
        size_t bytes=(size_t)ow*oh*4;
        uint8_t *a=malloc(bytes),*b=malloc(bytes),*bilinear=malloc(bytes);
        copy_image_to_host(pool,dst.image,ow,oh,a);
        copy_image_to_host(pool,other.image,ow,oh,b);
        copy_image_to_host(pool,linear.image,ow,oh,bilinear);
        image_error error=compare_pixels(a,b,(size_t)ow*oh,1);
        CHECK(!error.bad_rgb&&!error.bad_alpha,"crop-origin/DRS invariance case %u (%ux%u -> %ux%u)",test,cw,ch,ow,oh);
        double sharp=scene_sharpness(a,ow,oh),soft=scene_sharpness(bilinear,ow,oh);
        printf("scene sharpness case=%u spatial=%.6f bilinear=%.6f ratio=%.4f window=%ux%u fit=%ux%u\n",
            test,sharp,soft,sharp/soft,crops[test][4],crops[test][5],ow,oh);
        if (!vk_scene_aa_enabled()) CHECK(sharp>soft,"synthetic edges/text-like pattern beats bilinear Laplacian energy");
        unsigned opaque=0; for (size_t i=3;i<bytes;i+=4) opaque+=a[i]!=255;
        CHECK(!opaque,"scene output has opaque alpha and fills entire fitted target");
        free(a);free(b);free(bilinear);
        image_destroy(&src);image_destroy(&compact);image_destroy(&dst);image_destroy(&other);image_destroy(&tmp);image_destroy(&linear);
        if (vk_scene_aa_enabled()) for (unsigned j=0;j<3;j++) image_destroy(&aa[j]);
    }
    vkDestroyCommandPool(V.dev,pool,NULL);
    return fails!=0;
}
static void scene_hud(VkCommandBuffer cb,const test_image *dst) {
    barrier_image(cb,dst->image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderingAttachmentInfo attachment={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=dst->view,
        .imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo render={VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{dst->w,dst->h}},
        .layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&attachment};
    vkCmdBeginRendering(cb,&render);
    /* Native one-pixel glyph-like strokes deliberately cross scene diagonals. */
    for (unsigned y=4;y<20;y++) for (unsigned x=4;x<28;x++) {
        VkClearAttachment clear={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.clearValue.color={{(x+y)%2,1,0,1}}};
        VkClearRect rect={.rect={{(int)x,(int)y},{1,1}},.layerCount=1};
        vkCmdClearAttachments(cb,1,&clear,1,&rect);
    }
    vkCmdEndRendering(cb);
    barrier_image(cb,dst->image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}
static int scene_aa_cases(void) {
    CHECK(vk_scene_aa_enabled() && vk_scene_post_enabled() && vk_native_ui_enabled(),"scene AA selects native pre-UI ownership");
    CHECK(!vk_post_enabled(),"scene AA alone never adds a final/generated post filter");
    VkCommandPool pool=command_pool();
    test_image src,dst,plain,tmp,aa[3];
    VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_create(&src,SRC_W,SRC_H,VK_FORMAT_R8G8B8A8_UNORM,usage);
    uint32_t w=vk_scene_scale_enabled()?UPSCALE_W:CROP_W,h=vk_scene_scale_enabled()?UPSCALE_H:CROP_H;
    image_create(&dst,w,h,VK_FORMAT_R8G8B8A8_UNORM,usage);
    image_create(&plain,w,h,VK_FORMAT_R8G8B8A8_UNORM,usage);
    image_create(&tmp,w,h,VK_FORMAT_R8G8B8A8_UNORM,usage);
    scene_aa_images(aa,CROP_W,CROP_H,VK_FORMAT_R8G8B8A8_UNORM);
    upload_source(pool,&src);
    vk_post_output target=scene_target(&dst),scratch=scene_target(&tmp),reference=scene_target(&plain);
    mtl_sub_begin(); VkCommandBuffer cb=vk_cb();
    CHECK(scene_filter(cb,&src,CX,CY,CROP_W,CROP_H,&target,&scratch,aa),"scene SMAA records before upscale");
    if (vk_scene_scale_enabled()) CHECK(vk_scene_scale_run(cb,src.view,SRC_W,SRC_H,CX,CY,CROP_W,CROP_H,&reference,&scratch),"un-AA scene scale reference records");
    else scene_linear(cb,&src,&plain,CX,CY,CROP_W,CROP_H);
    CHECK(mtl_wait(mtl_sub_commit(),10000000000ull),"scene AA pixel submission retires");
    size_t bytes=(size_t)w*h*4; uint8_t *a=malloc(bytes),*b=malloc(bytes);
    copy_image_to_host(pool,dst.image,w,h,a); copy_image_to_host(pool,plain.image,w,h,b);
    unsigned transitions=gray_transition_pixels(a,w,h);
    CHECK(transitions>=8 && memcmp(a,b,bytes),"SMAA changes aliased scene edges (%u transition pixels)",transitions);
    CHECK(!colored_crop_margin(a,w,h),"scene AA excludes crop margins");
    /* Readback left TRANSFER_SRC; restore before a genuine native UI render. */
    mtl_sub_begin(); cb=vk_cb();
    barrier_image(cb,dst.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier_image(cb,plain.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    scene_hud(cb,&dst); scene_hud(cb,&plain);
    CHECK(mtl_wait(mtl_sub_commit(),10000000000ull),"native HUD render retires");
    copy_image_to_host(pool,dst.image,w,h,a); copy_image_to_host(pool,plain.image,w,h,b);
    unsigned bad=0;
    for (unsigned y=4;y<20;y++) for (unsigned x=4;x<28;x++)
        bad+=memcmp(a+((size_t)y*w+x)*4,b+((size_t)y*w+x)*4,4)!=0;
    CHECK(!bad,"all 384 native HUD pixels are bit-identical with/without scene SMAA");
    printf("SCENE_AA scale=%s transitions=%u HUD_pixels=384 HUD_mismatches=%u\n",getenv("TOTK_SCENE_SCALE")?:"off",transitions,bad);
    free(a);free(b);image_destroy(&src);image_destroy(&dst);image_destroy(&plain);image_destroy(&tmp);
    for (unsigned j=0;j<3;j++) image_destroy(&aa[j]);
    vkDestroyCommandPool(V.dev,pool,NULL);
    return fails!=0;
}

static int compare_samples(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}
static int parse_extent(const char *arg, uint32_t *w, uint32_t *h) {
    char extra;
    return sscanf(arg, "%ux%u%c", w, h, &extra) == 2 && *w > 0 && *h > 0 && *w <= 16384 && *h <= 16384;
}
static int benchmark(uint32_t sw, uint32_t sh, uint32_t ow, uint32_t oh, unsigned frames, int scene) {
    const char *scale = getenv(scene?"TOTK_SCENE_SCALE":"TOTK_POST_SCALE"), *aa = getenv("TOTK_POST_AA");
    if (!scene) CHECK(scale && (!strcmp(scale, "sgsr") || !strcmp(scale, "fsr")), "benchmark selects SGSR or FSR explicitly");
    CHECK(!aa || !strcmp(aa, "off"), "benchmark disables SMAA for a scaler-only comparison");
    if (!scene) CHECK(!vk_native_ui_enabled(), "scaler benchmark requires TOTK_NATIVE_UI=0");
    CHECK(V.timestamp_bits && V.props.limits.timestampPeriod > 0, "benchmark GPU timestamps are supported");
    CHECK(sw <= V.props.limits.maxImageDimension2D && sh <= V.props.limits.maxImageDimension2D &&
          ow <= V.props.limits.maxImageDimension2D && oh <= V.props.limits.maxImageDimension2D, "benchmark dimensions fit device limits");
    if (fails) return 1;
    VkCommandPool pool = command_pool(); test_image src,target={0},scratch={0},aa_images[3]={0};
    image_create(&src, sw, sh, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    uint8_t *source = VK_MALLOC((size_t)sw * sh * 4);
    sgsr_fixture(source, sw, sh, 0, 0, sw, sh, SGSR_COLOR, 0);
    upload_pixels(pool, &src, source, VK_IMAGE_LAYOUT_UNDEFINED); free(source);
    if (scene) {
        VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_create(&target,ow,oh,VK_FORMAT_R8G8B8A8_UNORM,usage);
        image_create(&scratch,ow,oh,VK_FORMAT_R8G8B8A8_UNORM,usage);
        if (vk_scene_aa_enabled()) scene_aa_images(aa_images,sw,sh,VK_FORMAT_R8G8B8A8_UNORM);
    }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2};
    VkQueryPool queries; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &queries));
    double *samples = VK_MALLOC(frames * sizeof *samples), sum = 0;
    const unsigned warmup = 60;
    uint64_t mask = V.timestamp_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << V.timestamp_bits) - 1;
    vk_post_output out = {0};
    for (unsigned frame = 0; frame < warmup + frames; frame++) {
        unsigned slot = frame % VK_PRESENT_SLOT_CAPACITY;
        mtl_sub_begin(); VkCommandBuffer cb = vk_cb();
        vkCmdResetQueryPool(cb, queries, 0, 2);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 0);
        int worked;
        if (scene) {
            out=scene_target(&target); vk_post_output tmp=scene_target(&scratch);
            if (vk_scene_post_enabled()) worked=scene_filter(cb,&src,0,0,sw,sh,&out,&tmp,aa_images);
            else { scene_linear(cb,&src,&target,0,0,sw,sh); worked=1; }
        } else worked=vk_post_run(cb, slot, src.view, sw, sh, 0, 0, sw, sh, ow, oh, 0, &out);
        vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries, 1);
        uint64_t done = mtl_sub_commit();
        CHECK(worked && mtl_wait(done, 10000000000ull), "benchmark frame %u completes", frame);
        if (fails) break;
        vk_post_complete(slot);
        uint64_t ticks[2];
        VKCHECK(vkGetQueryPoolResults(V.dev, queries, 0, 2, sizeof ticks, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
        if (frame >= warmup) {
            double ms = (double)((ticks[1] - ticks[0]) & mask) * V.props.limits.timestampPeriod / 1e6;
            samples[frame - warmup] = ms; sum += ms;
        }
    }
    if (!fails) {
        CHECK(out.w == ow && out.h == oh, "benchmark measures requested scale extent");
        qsort(samples, frames, sizeof *samples, compare_samples);
        double median = frames & 1 ? samples[frames / 2] : (samples[frames / 2 - 1] + samples[frames / 2]) * 0.5;
        printf("GPU benchmark: algorithm=%s passes=%u source=%ux%u output=%ux%u pattern=color warmup=%u samples=%u\n",
               scale?scale:"bilinear", scale&&!strcmp(scale, "fsr") ? 2 : 1, sw, sh, ow, oh, warmup, frames);
        printf("GPU benchmark: mean=%.6f median=%.6f p95=%.6f min=%.6f max=%.6f ms (GPU timestamps; no upload/readback/startup/CPU waits)\n",
               sum / frames, median, samples[(frames * 95u + 99u) / 100u - 1u], samples[0], samples[frames - 1]);
        printf("GPU scene AA: %s (three SMAA passes before scale/linear seed)\n",getenv("TOTK_SCENE_AA")?:"off");
        printf("GPU device: %s vendor=%#x device=%#x driver=%u timestampPeriod=%.9g ns validBits=%u\n",
               V.props.deviceName, V.props.vendorID, V.props.deviceID, V.props.driverVersion,
               V.props.limits.timestampPeriod, V.timestamp_bits);
        printf("GPU controls: SGSR sharpness=%.9g threshold=%.9g direction=%s; FSR RCAS sharpness=%s; LP_NUM_THREADS=%s\n",
               sgsr_control("TOTK_POST_SGSR_SHARPNESS", 2), sgsr_control("TOTK_POST_SGSR_EDGE_THRESHOLD", 8.0f / 255.0f),
               getenv("TOTK_POST_SGSR_EDGE_DIRECTION") ?: "0", getenv("TOTK_POST_SHARPNESS") ?: "0.2",
               getenv("LP_NUM_THREADS") ?: "unset");
    }
    free(samples); vkDestroyQueryPool(V.dev, queries, NULL); image_destroy(&src); vkDestroyCommandPool(V.dev, pool, NULL);
    if (scene) { image_destroy(&target); image_destroy(&scratch); if (vk_scene_aa_enabled()) for (unsigned j=0;j<3;j++) image_destroy(&aa_images[j]); }
    return fails != 0;
}

static int run_case(void) {
    int aa = vk_post_enabled(), scale = vk_post_scale();
    const char *aa_env = getenv("TOTK_POST_AA"), *scale_env = getenv("TOTK_POST_SCALE");
    const char *native = getenv("TOTK_NATIVE_UI");
    int native_ui = native && !strcmp(native,"1");
    int want_aa = aa_env && !strcmp(aa_env, "smaa");
    int want_fsr = !native_ui && scale_env && !strcmp(scale_env, "fsr");
    int want_sgsr = !native_ui && scale_env && !strcmp(scale_env, "sgsr"), want_scale = want_fsr || want_sgsr;
    CHECK(vk_native_ui_enabled() == native_ui, "native UI state agrees with exact environment switch");
    int srgb = getenv("TOTK_POST_TEST_SRGB") && !strcmp(getenv("TOTK_POST_TEST_SRGB"), "1");
    const unsigned slot = VK_PRESENT_SLOT_CAPACITY - 1;   /* real fourth-slot allocation, output, resize and timing */
    CHECK(aa == (want_aa || want_scale), "enabled state agrees with env (enabled=%d aa=%s scale=%s)", aa, aa_env ? aa_env : "unset", scale_env ? scale_env : "unset");
    CHECK(scale == want_scale, "scale state agrees with env (scale=%d)", scale);
    VkCommandPool pool = command_pool(); test_image src, reference;
    image_create(&src, SRC_W, SRC_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    upload_source(pool, &src);
    uint32_t want_w = want_scale ? UPSCALE_W : CROP_W, want_h = want_scale ? UPSCALE_H : CROP_H;
    VkFormat want_format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    vk_post_output out; memset(&out, 0xa5, sizeof out); vk_post_output untouched = out;
    mtl_sub_begin();
    VkCommandBuffer cb = vk_cb();
    int worked = vk_post_run(cb, slot, src.view, SRC_W, SRC_H, CX, CY, CROP_W, CROP_H, UPSCALE_W, UPSCALE_H, srgb, &out);
    uint64_t ticket = mtl_sub_commit();
    CHECK(mtl_wait(ticket, 10000000000ull), "post submission completes before readback");
    vk_post_complete(slot);
    if (!want_aa && !want_scale) {
        CHECK(!worked, "disabled post chain reports no work"); CHECK(!memcmp(&out, &untouched, sizeof out), "disabled no-op leaves output struct unchanged");
        image_create(&reference, CROP_W, CROP_H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        reference_blit(pool, &src, &reference);
        uint8_t *actual = malloc(CROP_W * CROP_H * 4), *expected = malloc(CROP_W * CROP_H * 4), *source_pixels = malloc(SRC_W * SRC_H * 4);
        copy_to_host_layout(pool, reference.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, CROP_W, CROP_H, actual);
        make_source_pixels(source_pixels);
        for (uint32_t y = 0; y < CROP_H; y++) memcpy(expected + (size_t)y * CROP_W * 4, source_pixels + ((size_t)(y + CY) * SRC_W + CX) * 4, CROP_W * 4);
        CHECK(!memcmp(actual, expected, CROP_W * CROP_H * 4), "disabled legacy cropped blit matches reference bytes exactly");
        free(actual); free(expected); free(source_pixels); image_destroy(&reference);
    } else {
        CHECK(worked, "enabled chain records work"); CHECK(out.image != VK_NULL_HANDLE && out.view != VK_NULL_HANDLE, "enabled result image and view exist");
        CHECK(out.w == want_w && out.h == want_h, "result extent %ux%u expected %ux%u", out.w, out.h, want_w, want_h);
        CHECK(out.format == want_format, "result format %d expected %d", out.format, want_format);
        size_t bytes = (size_t)want_w * want_h * 4; uint8_t *pixels = malloc(bytes); copy_image_to_host(pool, out.image, want_w, want_h, pixels);
        CHECK(colored_crop_margin(pixels, want_w, want_h) == 0, "crop margins do not bleed (%u magenta pixels)", colored_crop_margin(pixels, want_w, want_h));
        if (want_aa) {
            unsigned transitions = gray_transition_pixels(pixels, want_w, want_h);
            printf("SMAA edge metric: %u gray transition pixels (unfiltered pattern: 0)\n", transitions);
            CHECK(transitions >= 8, "SMAA creates meaningful gray transition coverage (%u transition pixels)", transitions);
        }
        if (want_scale) {
            unsigned nonblack = 0, bad = 0;
            for (size_t i = 0; i < bytes; i += 4) { if (pixels[i] || pixels[i+1] || pixels[i+2]) nonblack++; if (pixels[i+3] != 255) bad++; }
            CHECK(nonblack > want_w * want_h / 8, "scaled output contains substantial image content (%u pixels)", nonblack);
            CHECK(!bad, "scaled output has opaque alpha (%u invalid alpha pixels)", bad);
        }
        free(pixels);
    }
    if (aa || scale) {
        /* Reuse this slot only after the same synchronization boundary used by presentation. */
        vk_flush_wait(); vk_post_complete(slot);
        const uint32_t crop2_w = 80, crop2_h = 48, output2_w = 160, output2_h = 96;
        vk_post_output resized = {0};
        mtl_sub_begin(); VkCommandBuffer cb2 = vk_cb();
        int resized_work = vk_post_run(cb2, slot, src.view, SRC_W, SRC_H, 24, 24, crop2_w, crop2_h,
                                       output2_w, output2_h, srgb, &resized);
        uint64_t ticket2 = mtl_sub_commit();
        CHECK(mtl_wait(ticket2, 10000000000ull), "resized same-slot submission completes");
        vk_post_complete(slot);
        uint32_t resized_w = want_scale ? output2_w : crop2_w, resized_h = want_scale ? output2_h : crop2_h;
        CHECK(resized_work && resized.image && resized.view, "same-slot resize produces an image and view");
        CHECK(resized.w == resized_w && resized.h == resized_h,
              "same-slot resize extent %ux%u expected %ux%u", resized.w, resized.h, resized_w, resized_h);
        CHECK(resized.format == want_format, "same-slot resize preserves output format %d", resized.format);
        if (resized.image) {
            size_t resized_bytes = (size_t)resized_w * resized_h * 4;
            uint8_t *pixels = malloc(resized_bytes); copy_image_to_host(pool, resized.image, resized_w, resized_h, pixels);
            CHECK(colored_crop_margin(pixels, resized_w, resized_h) == 0,
                  "resized crop excludes outside margins (%u magenta pixels)", colored_crop_margin(pixels, resized_w, resized_h));
            free(pixels);
        }
        if (want_aa && want_scale) {
            /* Two completed frames above plus 298 here exercise all four capacity slots and the 300-frame timing report.
               These waits belong to the host harness, not the production recording path. */
            for (unsigned frame = 2; frame < 300; frame++) {
                unsigned timing_slot = frame % VK_PRESENT_SLOT_CAPACITY;
                mtl_sub_begin();
                vk_post_run(vk_cb(), timing_slot, src.view, SRC_W, SRC_H, CX, CY, CROP_W, CROP_H,
                            UPSCALE_W, UPSCALE_H, srgb, &out);
                uint64_t done = mtl_sub_commit();
                CHECK(mtl_wait(done, 10000000000ull), "timing frame %u completes", frame);
                vk_post_complete(timing_slot);
            }
        }
    }
    vkDestroyCommandPool(V.dev, pool, NULL); image_destroy(&src); return fails != 0;
}
int main(int argc, char **argv) {
    uint32_t sw = 0, sh = 0, ow = 0, oh = 0;
    unsigned frames = 0; char extra;
    int scene=argc>1 && !strcmp(argv[1],"--scene");
    int scene_aa=argc>1 && !strcmp(argv[1],"--scene-aa");
    int scene_bench=argc>1 && !strcmp(argv[1],"--scene-benchmark");
    if (argc > 1 && !((scene || scene_aa) && argc==2) && (argc != 5 || (!scene_bench && strcmp(argv[1], "--benchmark")) ||
                    !parse_extent(argv[2], &sw, &sh) || !parse_extent(argv[3], &ow, &oh) ||
                    sscanf(argv[4], "%u%c", &frames, &extra) != 1 || frames < 120 || frames > 1000000)) {
        fprintf(stderr, "usage: %s [--scene | --[scene-]benchmark sourceWxH outputWxH frames>=120]\n", argv[0]); return 2;
    }
    mtl_init();
    int result = scene_aa ? scene_aa_cases() : scene ? scene_cases() : argc > 1 ? benchmark(sw, sh, ow, oh, frames,scene_bench) : run_case();
    const char *scale = getenv("TOTK_POST_SCALE"), *aa = getenv("TOTK_POST_AA");
    if (argc == 1 && vk_post_scale() && scale && !strcmp(scale, "sgsr") && (!aa || strcmp(aa, "smaa"))) sgsr_golden_cases();
    vk_gpu_idle();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails); return result || fails != 0;
}
