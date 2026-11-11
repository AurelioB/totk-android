/* Included by nvn_vk.c: the section pools share the backend's slot lifetime,
 * but are exclusively walking-owned. Only the main pool is assembler-owned. */
#include "nvn_hmap.h"
int vk_one_primary_on;
typedef struct {
    VkRenderingInfo info;
    VkRenderingAttachmentInfo color[8], depth, stencil;
    VkFormat formats[8], depth_format, stencil_format;
    VkSampleCountFlagBits samples;
    VkCommandBuffer cb;
    void *pass;
    int rendering, worker;
} ordered_section;
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer primary, *buffers;
    uint32_t nbuffers, capbuffers, used;
    ordered_section *section;
    uint32_t count, capacity, cursor, open;
    int begun;
} ordered_slot;
static ordered_slot ordered_slots[8];
/* Walking ownership is serialized by vk_rec_lock, not tied to an OS thread.
 * Publishing only the stable token lets workers/assembler reject their own
 * handles without reading mutable walker state or g_cur. */
static _Atomic(ordered_slot *) ordered_current;
static _Atomic uint64_t ordered_counts[6];
static _Atomic uint64_t ordered_auxiliary_begins;
static int ordered_fallback;
static void ordered_note_primary(int auxiliary) {
    if (auxiliary) atomic_fetch_add_explicit(&ordered_auxiliary_begins, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&ordered_counts[0], 1, memory_order_relaxed);
}
uint64_t vk_sections_auxiliary_count(void) {
    return atomic_load_explicit(&ordered_auxiliary_begins, memory_order_relaxed);
}
static pthread_mutex_t ordered_views_mu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap ordered_images, ordered_views;
void vk_sections_counts(uint64_t out[6]) {
    for (unsigned i = 0; i < 6; i++) out[i] = atomic_load_explicit(&ordered_counts[i], memory_order_relaxed);
}
#ifdef TOTK_VK_TESTS
void vk_sections_test_counts(uint64_t out[6]) { vk_sections_counts(out); }
#endif
void vk_sections_init(void) {
    const char *e = getenv("TOTK_VK_REC_ONE_PRIMARY");
    vk_one_primary_on = e && !strcmp(e, "1");
    /* These primary query spans cannot inherit queries with the device features
     * currently enabled. Select legacy before any slot is opened, never mid-slot. */
    const char *occlusion = getenv("TOTK_VK_OCCLUSION"), *stats = getenv("TOTK_VK_GPU_PROFILE_STATS");
    const char *workers = getenv("TOTK_VK_REC_THREADS");
    int nworkers = workers ? atoi(workers) :
#if defined(__ANDROID__) || (defined(TOTK_VK_TESTS) && defined(TOTK_TEST_ANDROID_FRAME_POLICY))
        2;
#else
        0;
#endif
    const char *diagnostics[] = {"TOTK_VK_PASS_DUMP", "TOTK_VK_PASS_PROFILE", "TOTK_VK_PASSPROBE", "TOTK_VK_PASS_MERGE"};
    for (unsigned i = 0; i < sizeof diagnostics / sizeof *diagnostics; i++) {
        const char *value = getenv(diagnostics[i]);
        if (value && *value && strcmp(value, "0")) nworkers = 0;
    }
    const char *framegen = getenv("TOTK_FRAMEGEN");
    int inline_capture = framegen && (!strcmp(framegen, "1") || !strcmp(framegen, "interp") ||
        !strcmp(framegen, "hybrid") || !strcmp(framegen, "locked") || !strcmp(framegen, "adaptive"));
    if (vk_one_primary_on && ((occlusion && *occlusion && strcmp(occlusion, "0")) ||
            (stats && !strcmp(stats, "1") && (nworkers <= 0 || inline_capture)))) {
        vk_one_primary_on = 0;
        ordered_fallback = 1;
        LOGV("one-primary fallback: primary occlusion/inline statistics query inheritance unavailable");
    }
    if (vk_one_primary_on) {
        nvn_hm_init(&ordered_images, sizeof(VkImage));
        nvn_hm_init(&ordered_views, sizeof(VkImageView));
    }
    LOGV("TOTK_VK_REC_ONE_PRIMARY=%d (requested=%s; default=0; one main primary per slot, pre/adopted unchanged)",
        vk_one_primary_on, e ? e : "unset");
}
VkResult vk_sections_create_image(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *alloc, VkImage *image) {
    VkResult result = (vkCreateImage)(dev, info, alloc, image);
    if (vk_one_primary_on && !result) {
        pthread_mutex_lock(&ordered_views_mu);
        nvn_hm_put(&ordered_images, image, (void *)(uintptr_t)info->samples);
        pthread_mutex_unlock(&ordered_views_mu);
    }
    return result;
}
void vk_sections_destroy_image(VkDevice dev, VkImage image, const VkAllocationCallbacks *alloc) {
    if (vk_one_primary_on) {
        pthread_mutex_lock(&ordered_views_mu); nvn_hm_del(&ordered_images, &image); pthread_mutex_unlock(&ordered_views_mu);
    }
    (vkDestroyImage)(dev, image, alloc);
}
VkResult vk_sections_create_view(VkDevice dev, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkImageView *view) {
    VkResult result = (vkCreateImageView)(dev, info, alloc, view);
    if (vk_one_primary_on && !result) {
        pthread_mutex_lock(&ordered_views_mu);
        uintptr_t samples = (uintptr_t)nvn_hm_get(&ordered_images, &info->image);
        if (!samples) DIE("one-primary image view has no image sample metadata");
        uintptr_t metadata = (samples << 32) | (uint32_t)info->format;
        nvn_hm_put(&ordered_views, view, (void *)metadata);
        pthread_mutex_unlock(&ordered_views_mu);
    }
    return result;
}
void vk_sections_destroy_view(VkDevice dev, VkImageView view, const VkAllocationCallbacks *alloc) {
    if (vk_one_primary_on) {
        pthread_mutex_lock(&ordered_views_mu); nvn_hm_del(&ordered_views, &view); pthread_mutex_unlock(&ordered_views_mu);
    }
    (vkDestroyImageView)(dev, view, alloc);
}
static VkFormat ordered_attachment(VkImageView view, VkSampleCountFlagBits *samples) {
    if (!view) return VK_FORMAT_UNDEFINED;
    uintptr_t metadata = (uintptr_t)nvn_hm_get(&ordered_views, &view);
    if (!metadata) DIE("one-primary rendering attachment has no view metadata");
    *samples = (VkSampleCountFlagBits)(metadata >> 32);
    return (VkFormat)(uint32_t)metadata;
}
static void ordered_close(ordered_slot *s) {
    if (!s->open) return;
    ordered_section *section = &s->section[s->open - 1];
    if (section->cb) VKCHECK(vkEndCommandBuffer(section->cb));
    vk_draw_secondary_reset();
    s->open = 0;
}
static ordered_section *ordered_append(ordered_slot *s) {
    if (s->count == s->capacity) {
        uint32_t capacity = s->capacity ? s->capacity * 2 : 32;
        if (capacity < s->capacity) DIE("one-primary section count overflow");
        s->section = VK_REALLOC(s->section, (size_t)capacity * sizeof *s->section); s->capacity = capacity;
    }
    ordered_section *section = &s->section[s->count++];
    memset(section, 0, sizeof *section); s->open = s->count;
    return section;
}
static VkCommandBuffer ordered_secondary(ordered_slot *s, const ordered_section *section) {
    if (s->used == s->nbuffers) {
        if (s->nbuffers == s->capbuffers) {
            uint32_t capacity = s->capbuffers ? s->capbuffers * 2 : 32;
            if (capacity < s->capbuffers) DIE("one-primary secondary count overflow");
            s->buffers = VK_REALLOC(s->buffers, (size_t)capacity * sizeof *s->buffers); s->capbuffers = capacity;
        }
        uint32_t count = s->capbuffers - s->nbuffers;
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = s->pool, .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = count};
        VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, s->buffers + s->nbuffers)); s->nbuffers += count;
    }
    VkCommandBuffer cb = s->buffers[s->used++];
    VkCommandBufferInheritanceRenderingInfo rendering = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
        .viewMask = section->info.viewMask, .colorAttachmentCount = section->info.colorAttachmentCount,
        .pColorAttachmentFormats = section->formats, .depthAttachmentFormat = section->depth_format,
        .stencilAttachmentFormat = section->stencil_format, .rasterizationSamples = section->samples};
    VkCommandBufferInheritanceInfo inheritance = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
        .pNext = section->rendering ? &rendering : NULL};
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
            (section->rendering ? VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT : 0), .pInheritanceInfo = &inheritance};
    VKCHECK(vkBeginCommandBuffer(cb, &begin));
    atomic_fetch_add_explicit(&ordered_counts[4], 1, memory_order_relaxed);
    return cb;
}
void vk_sections_reset(unsigned slot, VkCommandBuffer primary) {
    ordered_slot *s = &ordered_slots[slot];
    if (atomic_load_explicit(&ordered_current, memory_order_acquire)) DIE("one-primary slot opened before previous walk sealed");
    if (!s->pool) {
        VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &info, NULL, &s->pool));
    } else VKCHECK(vkResetCommandPool(V.dev, s->pool, 0));
    if (s->primary && s->primary != primary) DIE("one-primary slot changed its stable handle");
    if (!s->primary) s->primary = primary;
    s->count = s->cursor = s->open = s->used = 0; s->begun = 0;
    atomic_store_explicit(&ordered_current, s, memory_order_release);
}
VkCommandBuffer vk_sections_route(VkCommandBuffer cb) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!s || cb != s->primary) return cb;
    ordered_section *section = s->open ? &s->section[s->open - 1] : ordered_append(s);
    if (section->worker) DIE("one-primary direct command inside a worker-rendered pass");
    if (!section->cb) {
        section->cb = ordered_secondary(s, section);
        if (!section->rendering) atomic_fetch_add_explicit(&ordered_counts[2], 1, memory_order_relaxed);
    }
    return section->cb;
}
void vk_sections_begin(VkCommandBuffer cb, const VkRenderingInfo *info) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!vk_one_primary_on || !s || cb != s->primary) { (vkCmdBeginRendering)(cb, info); return; }
    if (s->open && s->section[s->open - 1].rendering) DIE("one-primary nested rendering");
    ordered_close(s);
    ordered_section *section = ordered_append(s);
    section->rendering = 1; section->worker = !!(info->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    if (info->pNext || info->colorAttachmentCount > 8 ||
            (info->flags & (VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT)))
        DIE("one-primary unsupported rendering description");
    section->info = *info;
    if (info->colorAttachmentCount) memcpy(section->color, info->pColorAttachments, info->colorAttachmentCount * sizeof *section->color);
    if (info->pDepthAttachment) section->depth = *info->pDepthAttachment;
    if (info->pStencilAttachment) section->stencil = *info->pStencilAttachment;
    /* Store presence, never stack pointers; materialize pointers at emission. */
    section->info.pColorAttachments = NULL;
    section->info.pDepthAttachment = info->pDepthAttachment ? &section->depth : NULL;
    section->info.pStencilAttachment = info->pStencilAttachment ? &section->stencil : NULL;
    section->samples = VK_SAMPLE_COUNT_1_BIT;
    if (!section->worker) {
        pthread_mutex_lock(&ordered_views_mu);
        for (uint32_t i = 0; i < info->colorAttachmentCount; i++) section->formats[i] = ordered_attachment(section->color[i].imageView, &section->samples);
        section->depth_format = ordered_attachment(section->depth.imageView, &section->samples);
        section->stencil_format = ordered_attachment(section->stencil.imageView, &section->samples);
        pthread_mutex_unlock(&ordered_views_mu);
        atomic_fetch_add_explicit(&ordered_counts[3], 1, memory_order_relaxed);
    } else atomic_fetch_add_explicit(&ordered_counts[1], 1, memory_order_relaxed);
}
void vk_sections_end(VkCommandBuffer cb) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!vk_one_primary_on || !s || cb != s->primary) { (vkCmdEndRendering)(cb); return; }
    if (!s->open || !s->section[s->open - 1].rendering) DIE("one-primary end outside rendering");
    ordered_close(s);
}
void vk_sections_park(void *pass) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!s || !s->open || !s->section[s->open - 1].worker) DIE("one-primary worker pass missing rendering section");
    s->section[s->open - 1].pass = pass; ordered_close(s);
}
void vk_sections_seal(void) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (!s) return;
    if (s->open && s->section[s->open - 1].rendering) DIE("one-primary sealed inside rendering");
    ordered_close(s); atomic_store_explicit(&ordered_current, NULL, memory_order_release);
}
void vk_sections_resume(unsigned slot) {
    atomic_store_explicit(&ordered_current, &ordered_slots[slot], memory_order_release);
}
void vk_sections_cancel(void) {
    ordered_slot *s = atomic_load_explicit(&ordered_current, memory_order_acquire);
    if (s) { ordered_close(s); atomic_store_explicit(&ordered_current, NULL, memory_order_release); }
}
void vk_sections_until(unsigned slot, void *pass) {
    ordered_slot *s = &ordered_slots[slot];
    if (!s->begun) {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHECK(vkBeginCommandBuffer(s->primary, &begin)); s->begun = 1;
        atomic_fetch_add_explicit(&ordered_counts[0], 1, memory_order_relaxed);
    }
    /* A synchronous test-only early assembly may leave the walk active. Raw
     * calls below deliberately bypass token routing, never the public macros. */
    while (s->cursor < s->count) {
        ordered_section *section = &s->section[s->cursor++];
        if (section->rendering) {
            VkRenderingInfo info = section->info;
            info.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
            info.pColorAttachments = section->color;
            info.pDepthAttachment = section->info.pDepthAttachment ? &section->depth : NULL;
            info.pStencilAttachment = section->info.pStencilAttachment ? &section->stencil : NULL;
            (vkCmdBeginRendering)(s->primary, &info);
            if (section->pass) {
                if (section->pass != pass) DIE("one-primary recorder section order mismatch");
                return; /* recorder emits its jobs, EndRendering and profiler end */
            }
            if (section->cb) (vkCmdExecuteCommands)(s->primary, 1, &section->cb);
            (vkCmdEndRendering)(s->primary);
        } else if (section->cb) (vkCmdExecuteCommands)(s->primary, 1, &section->cb);
    }
    if (pass) DIE("one-primary recorder section not found");
}
void vk_sections_finish(unsigned slot) {
    ordered_slot *s = &ordered_slots[slot];
    if (!s->begun || s->cursor != s->count) DIE("one-primary queue before assembly");
    VKCHECK(vkEndCommandBuffer(s->primary));
}
