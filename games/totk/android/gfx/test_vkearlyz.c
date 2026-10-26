/* Standalone host Vulkan correctness fixture; no game shaders or backend state.
   Run test_vkearlyz.py to compile synthetic GLSL and this file in .scratch/earlyz.
   Compare compiler-declared early tests with stripped/restored modules, including
   depth and shader storage, not just colors. A late depth-writing shader is the
   negative control: globally adding EarlyFragmentTests must change its results. */
#include <vulkan/vulkan.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "vk_earlyz.h"

enum { WIDTH = 8, HEIGHT = 8, PIXELS = WIDTH * HEIGHT, COLOR_BYTES = PIXELS * 4 };
static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define VK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) fatal("%s: VkResult %d", #call, r_); } while (0)

static void fatal(const char *format, ...) {
    va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
    fputc('\n', stderr); exit(2);
}
static void *allocate(size_t bytes) {
    void *p = malloc(bytes); if (!p) fatal("allocating %zu bytes", bytes); return p;
}
typedef struct { uint32_t *words; size_t nw; } module;
static module clone_module(module source) {
    module m = {allocate(source.nw * sizeof(uint32_t)), source.nw};
    memcpy(m.words, source.words, m.nw * sizeof(uint32_t)); return m;
}
static module load_module(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) fatal("opening %s", path);
    if (fseek(f, 0, SEEK_END)) fatal("seeking %s", path);
    long bytes = ftell(f);
    if (bytes < 20 || bytes % 4 || fseek(f, 0, SEEK_SET)) fatal("invalid SPIR-V file %s", path);
    module m = {allocate((size_t)bytes), (size_t)bytes / 4};
    if (fread(m.words, 1, (size_t)bytes, f) != (size_t)bytes || fclose(f)) fatal("reading %s", path);
    return m;
}
static size_t instruction(module m, uint32_t opcode) {
    for (size_t i = 5; i < m.nw; ) {
        size_t n = m.words[i] >> 16;
        if (!n || n > m.nw - i) fatal("malformed compiler output");
        if ((m.words[i] & 0xffff) == opcode) return i;
        i += n;
    }
    fatal("missing SPIR-V opcode %u", opcode); return 0;
}
static size_t early_mode(module m) {
    for (size_t i = 5; i < m.nw; ) {
        size_t n = m.words[i] >> 16;
        if (!n || n > m.nw - i) fatal("malformed patched output");
        if ((m.words[i] & 0xffff) == 16 && n >= 3 && m.words[i + 2] == 9) return i;
        i += n;
    }
    return 0;
}
static void insert(module *m, size_t at, const uint32_t *words, size_t count) {
    uint32_t *p = allocate((m->nw + count) * sizeof(uint32_t));
    memcpy(p, m->words, at * sizeof(uint32_t));
    memcpy(p + at, words, count * sizeof(uint32_t));
    memcpy(p + at + count, m->words + at, (m->nw - at) * sizeof(uint32_t));
    free(m->words); m->words = p; m->nw += count;
}
static void invalid_case(const char *label, module m) {
    module before = clone_module(m); uint32_t *pointer = m.words;
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 1) == -1, "%s rejects malformed module", label);
    CHECK(m.words == pointer && m.nw == before.nw && !memcmp(m.words, before.words, m.nw * 4),
          "%s leaves invalid input untouched", label);
    free(before.words); free(m.words);
}
static void helper_tests(module vertex, module original, module late) {
    CHECK(vk_earlyz_enabled(NULL), "absent EARLYZ override honors the original shader's early tests");
    CHECK(vk_earlyz_enabled("1"), "EARLYZ=1 explicitly honors the original shader's early tests");
    CHECK(!vk_earlyz_enabled("0"), "EARLYZ=0 selects the legacy diagnostic path");
    uint8_t control[0x880] = {0}; control[0x718] = 1;
    CHECK(vk_nvn_early_tests(control, sizeof control, 1), "full fragment control carries EarlyZForce");
    CHECK(!vk_nvn_early_tests(control, sizeof control, 0) && !vk_nvn_early_tests(control, sizeof control, 5),
          "vertex and compute controls cannot request fragment early tests");
    CHECK(!vk_nvn_early_tests(control, 0x7d1, 1) && !vk_nvn_early_tests(control, 0x848, 1),
          "short compiler controls do not interpret TOTK fragment metadata");
    control[0x718] = 0; control[0x719] = control[0x71c] = 1;
    CHECK(!vk_nvn_early_tests(control, sizeof control, 1), "post-depth/depth-output bytes do not imply EarlyZForce");
    module m = clone_module(original); uint32_t *pointer = m.words;
    CHECK(early_mode(m) != 0, "compiler emitted declared EarlyFragmentTests");
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 1) == 0 && m.words == pointer,
          "already-early enable is allocation-free and idempotent");
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 0) == 1 && m.words == pointer && m.nw + 3 == original.nw,
          "removal edits exactly three words in place");
    module stripped = clone_module(m);
    size_t at = early_mode(original);
    CHECK(at && m.nw + 3 == original.nw && !memcmp(m.words, original.words, at * 4)
          && !memcmp(m.words + at, original.words + at + 3, (m.nw - at) * 4),
          "removal preserves every other SPIR-V word");
    CHECK(!early_mode(m), "removal removes EarlyFragmentTests");
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 0) == 0 && m.words == pointer,
          "already-late removal is allocation-free and idempotent");
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 1) == 1 && m.nw == original.nw && early_mode(m),
          "restoration inserts EarlyFragmentTests");
    pointer = m.words;
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 1) == 0 && m.words == pointer,
          "restored enable is allocation-free and idempotent");
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 0) == 1 && m.words == pointer && m.nw == stripped.nw
          && !memcmp(m.words, stripped.words, m.nw * 4), "strip/add/strip is byte-identical");
    free(stripped.words); free(m.words);
    m = clone_module(late); pointer = m.words;
    CHECK(!early_mode(m) && vk_spirv_early_tests(&m.words, &m.nw, 0) == 0
          && m.words == pointer && m.nw == late.nw && !memcmp(m.words, late.words, m.nw * 4),
          "originally late shader remains byte-identical");
    CHECK(vk_spirv_early_tests(&m.words, &m.nw, 1) == 1
          && vk_spirv_early_tests(&m.words, &m.nw, 0) == 1 && m.nw == late.nw
          && !memcmp(m.words, late.words, m.nw * 4), "late add/remove is byte-identical");
    free(m.words);

    m = clone_module(original); m.nw = 4; invalid_case("short header", m);
    m = clone_module(original); m.words[0] = 0; invalid_case("bad magic", m);
    m = clone_module(original); m.words[5] &= 0xffff; invalid_case("zero instruction word count", m);
    m = clone_module(original); m.words[5] = (0xffffu << 16) | (m.words[5] & 0xffff);
    invalid_case("truncated instruction", m);
    m = clone_module(original); size_t entry = instruction(m, 15);
    m.words[entry] = (m.words[entry] & 0xffff0000u) | 0; invalid_case("missing entry", m);
    invalid_case("non-fragment entry", clone_module(vertex));
    m = clone_module(original); entry = instruction(m, 15);
    size_t entry_words = m.words[entry] >> 16;
    uint32_t *copy = allocate(entry_words * 4); memcpy(copy, m.words + entry, entry_words * 4);
    insert(&m, m.nw, copy, entry_words); free(copy); invalid_case("multiple entries", m);
    m = clone_module(original); uint32_t duplicate[3]; memcpy(duplicate, m.words + early_mode(m), sizeof duplicate);
    insert(&m, m.nw, duplicate, 3); invalid_case("duplicate early mode", m);
    m = clone_module(original); size_t mode = early_mode(m); uint32_t extra = 0;
    m.words[mode] = (4u << 16) | 16; insert(&m, mode + 3, &extra, 1);
    invalid_case("malformed early mode length", m);
}

typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; } buffer;
typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; } image;
typedef struct {
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    VkCommandPool commands; VkCommandBuffer command;
    VkRenderPass pass; VkFramebuffer framebuffer;
    VkDescriptorSetLayout descriptors; VkDescriptorPool descriptor_pool; VkDescriptorSet descriptor;
    VkPipelineLayout layout; VkShaderModule vertex;
    image color, depth; buffer storage, readback;
} context;
typedef struct { uint8_t color[COLOR_BYTES]; float depth[PIXELS]; uint32_t storage[PIXELS]; } snapshot;

static uint32_t memory_type(context *c, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < c->memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    fatal("no Vulkan memory type for flags 0x%x", flags); return 0;
}
static buffer make_buffer(context *c, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    buffer b = {0};
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = usage};
    VK(vkCreateBuffer(c->device, &info, NULL, &b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(c->device, b.buffer, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK(vkAllocateMemory(c->device, &allocation, NULL, &b.memory));
    VK(vkBindBufferMemory(c->device, b.buffer, b.memory, 0));
    VK(vkMapMemory(c->device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped)); return b;
}
static image make_image(context *c, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(c->physical, format, &properties);
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        | (aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    if ((properties.optimalTilingFeatures & needed) != needed) fatal("Vulkan format %d lacks attachment/readback support", format);
    image im = {0};
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {WIDTH, HEIGHT, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VK(vkCreateImage(c->device, &info, NULL, &im.image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(c->device, im.image, &req);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VK(vkAllocateMemory(c->device, &allocation, NULL, &im.memory));
    VK(vkBindImageMemory(c->device, im.image, im.memory, 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = {aspect, 0, 1, 0, 1}};
    VK(vkCreateImageView(c->device, &view, NULL, &im.view)); return im;
}
static VkShaderModule make_shader(context *c, module m) {
    VkShaderModule shader;
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = m.nw * sizeof(uint32_t), .pCode = m.words};
    VK(vkCreateShaderModule(c->device, &info, NULL, &shader)); return shader;
}
static void initialize(context *c, module vertex) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "early-fragment fixture", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VK(vkCreateInstance(&instance, NULL, &c->instance));
    uint32_t count = 0; VK(vkEnumeratePhysicalDevices(c->instance, &count, NULL));
    if (!count) fatal("no Vulkan devices (install/select the Lavapipe ICD)");
    VkPhysicalDevice *devices = allocate(count * sizeof *devices);
    VK(vkEnumeratePhysicalDevices(c->instance, &count, devices));
    c->physical = devices[0];
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { c->physical = devices[i]; break; }
    }
    free(devices);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(c->physical, &properties);
    printf("Vulkan device: %s (type %u, API %u.%u.%u)\n", properties.deviceName, properties.deviceType,
           VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
    VkPhysicalDeviceFeatures available; vkGetPhysicalDeviceFeatures(c->physical, &available);
    if (!available.fragmentStoresAndAtomics) fatal("Vulkan device lacks fragmentStoresAndAtomics");
    vkGetPhysicalDeviceMemoryProperties(c->physical, &c->memory);
    vkGetPhysicalDeviceQueueFamilyProperties(c->physical, &count, NULL);
    VkQueueFamilyProperties *families = allocate(count * sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(c->physical, &count, families);
    uint32_t family = 0;
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) family++;
    free(families); if (family == count) fatal("no graphics queue");
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    VkPhysicalDeviceFeatures features = {.fragmentStoresAndAtomics = VK_TRUE};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue, .pEnabledFeatures = &features};
    VK(vkCreateDevice(c->physical, &device, NULL, &c->device)); vkGetDeviceQueue(c->device, family, 0, &c->queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VK(vkCreateCommandPool(c->device, &pool, NULL, &c->commands));
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(c->device, &command, &c->command));
    c->color = make_image(c, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    c->depth = make_image(c, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    c->storage = make_buffer(c, PIXELS * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    c->readback = make_buffer(c, COLOR_BYTES + PIXELS * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkAttachmentDescription attachments[2] = {
        {.format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL},
        {.format = VK_FORMAT_D32_SFLOAT, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}
    };
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &color, .pDepthStencilAttachment = &depth};
    VkPipelineStageFlags attachment_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkAccessFlags attachment_writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dependencies[2] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         .dstStageMask = attachment_stages, .dstAccessMask = attachment_writes},
        {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = attachment_stages,
         .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT, .srcAccessMask = attachment_writes, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT}
    };
    VkRenderPassCreateInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2,
        .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 2, .pDependencies = dependencies};
    VK(vkCreateRenderPass(c->device, &pass, NULL, &c->pass));
    VkImageView views[2] = {c->color.view, c->depth.view};
    VkFramebufferCreateInfo framebuffer = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = c->pass, .attachmentCount = 2, .pAttachments = views, .width = WIDTH, .height = HEIGHT, .layers = 1};
    VK(vkCreateFramebuffer(c->device, &framebuffer, NULL, &c->framebuffer));
    VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    VK(vkCreateDescriptorSetLayout(c->device, &descriptors, NULL, &c->descriptors));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo descriptor_pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    VK(vkCreateDescriptorPool(c->device, &descriptor_pool, NULL, &c->descriptor_pool));
    VkDescriptorSetAllocateInfo descriptor = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = c->descriptor_pool, .descriptorSetCount = 1, .pSetLayouts = &c->descriptors};
    VK(vkAllocateDescriptorSets(c->device, &descriptor, &c->descriptor));
    VkDescriptorBufferInfo storage = {c->storage.buffer, 0, PIXELS * sizeof(uint32_t)};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = c->descriptor,
        .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &storage};
    vkUpdateDescriptorSets(c->device, 1, &write, 0, NULL);
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &c->descriptors, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VK(vkCreatePipelineLayout(c->device, &layout, NULL, &c->layout)); c->vertex = make_shader(c, vertex);
}
static VkPipeline make_pipeline(context *c, VkShaderModule fragment, VkCompareOp compare, VkBool32 depth_write) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = c->vertex, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragment, .pName = "main"}
    };
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport = {0, 0, WIDTH, HEIGHT, 0, 1}; VkRect2D scissor = {{0, 0}, {WIDTH, HEIGHT}};
    VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = depth_write, .depthCompareOp = compare};
    VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &input, .pInputAssemblyState = &assembly,
        .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pDepthStencilState = &depth, .pColorBlendState = &blend, .layout = c->layout, .renderPass = c->pass};
    VkPipeline result; VK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &result)); return result;
}
static snapshot render(context *c, module fragment, float raster_depth, VkCompareOp compare, VkBool32 depth_write) {
    VkShaderModule shader = make_shader(c, fragment); VkPipeline pipeline = make_pipeline(c, shader, compare, depth_write);
    memset(c->storage.mapped, 0, PIXELS * sizeof(uint32_t));
    memset(c->readback.mapped, 0xcd, COLOR_BYTES + PIXELS * sizeof(float));
    VK(vkResetCommandPool(c->device, c->commands, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK(vkBeginCommandBuffer(c->command, &begin));
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(c->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &host, 0, NULL, 0, NULL);
    VkClearValue clear[2] = {{.color = {.float32 = {0, 0, 0, 1}}}, {.depthStencil = {0.5f, 0}}};
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = c->pass, .framebuffer = c->framebuffer, .renderArea = {{0, 0}, {WIDTH, HEIGHT}},
        .clearValueCount = 2, .pClearValues = clear};
    vkCmdBeginRenderPass(c->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(c->command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(c->command, VK_PIPELINE_BIND_POINT_GRAPHICS, c->layout, 0, 1, &c->descriptor, 0, NULL);
    vkCmdPushConstants(c->command, c->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof raster_depth, &raster_depth);
    vkCmdDraw(c->command, 3, 1, 0, 0); vkCmdEndRenderPass(c->command);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {WIDTH, HEIGHT, 1}};
    vkCmdCopyImageToBuffer(c->command, c->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->readback.buffer, 1, &copy);
    copy.bufferOffset = COLOR_BYTES; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImageToBuffer(c->command, c->depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->readback.buffer, 1, &copy);
    VkMemoryBarrier read = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(c->command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &read, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(c->command));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c->command};
    VK(vkQueueSubmit(c->queue, 1, &submit, VK_NULL_HANDLE)); VK(vkQueueWaitIdle(c->queue));
    snapshot result;
    memcpy(result.color, c->readback.mapped, sizeof result.color);
    memcpy(result.depth, (uint8_t *)c->readback.mapped + COLOR_BYTES, sizeof result.depth);
    memcpy(result.storage, c->storage.mapped, sizeof result.storage);
    vkDestroyPipeline(c->device, pipeline, NULL); vkDestroyShaderModule(c->device, shader, NULL); return result;
}
static void identical(const char *label, const snapshot *a, const snapshot *b) {
    int color = !memcmp(a->color, b->color, sizeof a->color);
    int depth = !memcmp(a->depth, b->depth, sizeof a->depth);
    int storage = !memcmp(a->storage, b->storage, sizeof a->storage);
    CHECK(color, "%s color is byte-identical", label); CHECK(depth, "%s depth is byte-identical", label);
    CHECK(storage, "%s SSBO is byte-identical", label);
    printf("%s: color=%s depth=%s SSBO=%s (%d pixels)\n", label,
           color ? "identical" : "DIFFERENT", depth ? "identical" : "DIFFERENT", storage ? "identical" : "DIFFERENT", PIXELS);
}
static void differences(const char *label, const snapshot *a, const snapshot *b) {
    unsigned color = 0, depth = 0, storage = 0;
    for (int i = 0; i < PIXELS; i++) {
        color += memcmp(a->color + i * 4, b->color + i * 4, 4) != 0;
        depth += memcmp(a->depth + i, b->depth + i, sizeof(float)) != 0;
        storage += a->storage[i] != b->storage[i];
    }
    printf("%s: differing pixels color=%u depth=%u SSBO=%u (intentional control)\n", label, color, depth, storage);
}
/* kind: 0=depth-rejected store, 1=alpha kill, 2=late depth write, 3=forced late. */
static void expected(const char *label, const snapshot *s, int kind, int early) {
    for (int i = 0; i < PIXELS; i++) {
        uint8_t color[4] = {0, 0, 0, 255}; float depth = 0.5f; uint32_t hits = 0;
        if (kind == 0) hits = early ? 0 : 1;
        if (kind == 1) {
            hits = 1;
            if (early || ((i % WIDTH) & 1)) depth = 0.25f;
            if ((i % WIDTH) & 1) color[1] = 255;
        }
        if (kind == 2) { color[2] = 255; depth = 0.25f; hits = 1; }
        CHECK(!memcmp(s->color + i * 4, color, 4), "%s pixel %d expected RGBA", label, i);
        CHECK(s->depth[i] == depth, "%s pixel %d depth %.9g expected %.9g", label, i, s->depth[i], depth);
        CHECK(s->storage[i] == hits, "%s pixel %d SSBO %u expected %u", label, i, s->storage[i], hits);
    }
}
static void early_case(context *c, const char *label, module original, int kind, float raster_depth) {
    module disabled = clone_module(original);
    CHECK(vk_spirv_early_tests(&disabled.words, &disabled.nw, vk_earlyz_enabled("0")) == 1, "%s disables original mode", label);
    module restored = clone_module(disabled);
    CHECK(vk_spirv_early_tests(&restored.words, &restored.nw, vk_earlyz_enabled(NULL)) == 1, "%s restores original mode by default", label);
    printf("%s module bytes: compiler-original/restored %s\n", label,
           restored.nw == original.nw && !memcmp(restored.words, original.words, original.nw * 4)
           ? "identical" : "execution-mode placement differs");
    snapshot declared = render(c, original, raster_depth, VK_COMPARE_OP_LESS, VK_TRUE), baseline = render(c, disabled, raster_depth, VK_COMPARE_OP_LESS, VK_TRUE);
    snapshot faithful = render(c, restored, raster_depth, VK_COMPARE_OP_LESS, VK_TRUE);
    identical(label, &declared, &faithful); expected(label, &declared, kind, 1); expected(label, &baseline, kind, 0);
    differences(label, &declared, &baseline);
    if (kind == 0) CHECK(memcmp(declared.storage, baseline.storage, sizeof declared.storage), "depth rejection control changes SSBO despite identical colors");
    if (kind == 1) CHECK(memcmp(declared.depth, baseline.depth, sizeof declared.depth), "discard control changes depth despite identical colors");
    free(disabled.words); free(restored.words);
}
/* G-buffer-style EQUAL with read-only depth: verify declared/default-restored semantics for both a matching sample
   and a rejected sample. This exercises the contract, not the device-specific early/late rounding discrepancy. */
static void equal_case(context *c, module original) {
    module disabled = clone_module(original);
    CHECK(vk_spirv_early_tests(&disabled.words, &disabled.nw, 0) == 1, "EQUAL diagnostic removes early mode");
    module restored = clone_module(disabled);
    CHECK(vk_spirv_early_tests(&restored.words, &restored.nw, vk_earlyz_enabled(NULL)) == 1, "EQUAL defaults restore early mode");
    for (int match = 0; match < 2; match++) {
        const char *label = match ? "EQUAL depth-read-only match" : "EQUAL depth-read-only rejection";
        float z = match ? 0.5f : 0.75f;
        snapshot declared = render(c, original, z, VK_COMPARE_OP_EQUAL, VK_FALSE);
        snapshot faithful = render(c, restored, z, VK_COMPARE_OP_EQUAL, VK_FALSE);
        snapshot legacy = render(c, disabled, z, VK_COMPARE_OP_EQUAL, VK_FALSE);
        identical(label, &declared, &faithful);
        for (int i = 0; i < PIXELS; i++) {
            uint8_t color[4] = {match ? 255 : 0, 0, 0, 255};
            CHECK(!memcmp(declared.color + i * 4, color, 4), "%s pixel %d expected RGBA", label, i);
            CHECK(declared.depth[i] == 0.5f && legacy.depth[i] == 0.5f, "%s pixel %d never writes depth", label, i);
            CHECK(declared.storage[i] == (uint32_t)match && legacy.storage[i] == 1, "%s pixel %d side effects follow early tests", label, i);
        }
    }
    free(disabled.words); free(restored.words);
}
static void late_case(context *c, module original) {
    module faithful = clone_module(original);
    CHECK(vk_spirv_early_tests(&faithful.words, &faithful.nw, 0) == 0
          && faithful.nw == original.nw && !memcmp(faithful.words, original.words, original.nw * 4),
          "originally late depth-writing module must not gain early tests");
    module forced = clone_module(original);
    CHECK(vk_spirv_early_tests(&forced.words, &forced.nw, 1) == 1, "global-force negative control inserts early tests");
    snapshot declared = render(c, original, 0.75f, VK_COMPARE_OP_LESS, VK_TRUE), preserved = render(c, faithful, 0.75f, VK_COMPARE_OP_LESS, VK_TRUE);
    snapshot wrong = render(c, forced, 0.75f, VK_COMPARE_OP_LESS, VK_TRUE);
    identical("late-depth-write remains late", &declared, &preserved);
    expected("late-depth-write original", &declared, 2, 0); expected("late-depth-write globally forced", &wrong, 3, 1);
    differences("late-depth-write global forcing", &declared, &wrong);
    CHECK(memcmp(declared.storage, wrong.storage, sizeof declared.storage), "global forcing changes late shader SSBO side effects");
    CHECK(memcmp(declared.depth, wrong.depth, sizeof declared.depth), "global forcing changes late shader depth");
    free(faithful.words); free(forced.words);
}
static void destroy_image(context *c, image im) {
    vkDestroyImageView(c->device, im.view, NULL); vkDestroyImage(c->device, im.image, NULL); vkFreeMemory(c->device, im.memory, NULL);
}
static void destroy_buffer(context *c, buffer b) {
    vkUnmapMemory(c->device, b.memory); vkDestroyBuffer(c->device, b.buffer, NULL); vkFreeMemory(c->device, b.memory, NULL);
}
static void destroy(context *c) {
    vkDestroyShaderModule(c->device, c->vertex, NULL); vkDestroyPipelineLayout(c->device, c->layout, NULL);
    vkDestroyDescriptorPool(c->device, c->descriptor_pool, NULL); vkDestroyDescriptorSetLayout(c->device, c->descriptors, NULL);
    vkDestroyFramebuffer(c->device, c->framebuffer, NULL); vkDestroyRenderPass(c->device, c->pass, NULL);
    destroy_image(c, c->color); destroy_image(c, c->depth); destroy_buffer(c, c->storage); destroy_buffer(c, c->readback);
    vkDestroyCommandPool(c->device, c->commands, NULL); vkDestroyDevice(c->device, NULL); vkDestroyInstance(c->instance, NULL);
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s fullscreen.vert.spv store.frag.spv discard.frag.spv depth.frag.spv\n", argv[0]); return 2; }
    module vertex = load_module(argv[1]), store = load_module(argv[2]), discard = load_module(argv[3]), depth = load_module(argv[4]);
    helper_tests(vertex, store, depth);
    if (!failures) {
        context c = {0}; initialize(&c, vertex);
        early_case(&c, "early storage with depth rejection", store, 0, 0.75f);
        early_case(&c, "early alpha discard", discard, 1, 0.25f);
        equal_case(&c, store);
        late_case(&c, depth); destroy(&c);
    }
    free(vertex.words); free(store.words); free(discard.words); free(depth.words);
    printf("early-fragment fixture: %d checks, %d failures\n", checks, failures); return failures != 0;
}
