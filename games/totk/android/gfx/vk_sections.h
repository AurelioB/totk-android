/* Ordered one-primary sections. The walking main handle remains a stable token:
 * helpers may retain it across pass boundaries. Only that thread's active token
 * is routed. Worker, pre, adopted and independently owned buffers are untouched.
 * Raw PFNs remain available as (vkCmdFoo)(...), notably to the assembler. */
#pragma once
extern int vk_one_primary_on;
#include "vk_load.h"
#include <stdint.h>
#include <stdatomic.h>
/* A full cache line, not just aligned storage: counter updates must never
 * invalidate the token line read by non-walking command-buffer owners. */
typedef struct {
    _Alignas(64) _Atomic(VkCommandBuffer) cb;
    unsigned char padding[64 - sizeof(_Atomic(VkCommandBuffer))];
} vk_sections_token_state;
extern vk_sections_token_state vk_sections_token;
void vk_sections_init(void);
void vk_sections_reset(unsigned slot, VkCommandBuffer primary);
void vk_sections_seal(void);
void vk_sections_cancel(void);
void vk_sections_resume(unsigned slot); /* synchronous test-only early assembly */
void vk_sections_park(void *pass);
void vk_sections_until(unsigned slot, void *pass);
void vk_sections_finish(unsigned slot);
VkCommandBuffer vk_sections_route(VkCommandBuffer cb);
static inline __attribute__((always_inline)) VkCommandBuffer vk_sections_command_buffer(VkCommandBuffer cb) {
    if (vk_one_primary_on &&
            cb == atomic_load_explicit(&vk_sections_token.cb, memory_order_relaxed))
        return vk_sections_route(cb);
    return cb;
}
void vk_sections_begin(VkCommandBuffer cb, const VkRenderingInfo *info);
void vk_sections_end(VkCommandBuffer cb);
void vk_sections_counts(uint64_t out[6]);
uint64_t vk_sections_auxiliary_count(void);
void vk_sections_times(uint64_t out[2]); /* cumulative walker secondary / assembler until ns */
void vk_draw_secondary_reset(void);
#ifdef TOTK_VK_TESTS
void vk_sections_test_counts(uint64_t out[6]);
#endif
VkResult vk_sections_create_image(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *alloc, VkImage *image);
void vk_sections_destroy_image(VkDevice dev, VkImage image, const VkAllocationCallbacks *alloc);
VkResult vk_sections_create_view(VkDevice dev, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *alloc, VkImageView *view);
void vk_sections_destroy_view(VkDevice dev, VkImageView view, const VkAllocationCallbacks *alloc);
#define VK_SECTION_CB(cb) vk_sections_command_buffer(cb)
#define vkCmdWaitEvents(cb, ...) vkCmdWaitEvents(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdResetQueryPool(cb, ...) vkCmdResetQueryPool(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdBeginQuery(cb, ...) vkCmdBeginQuery(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdEndQuery(cb, ...) vkCmdEndQuery(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdCopyQueryPoolResults(cb, ...) vkCmdCopyQueryPoolResults(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdWriteTimestamp2(cb, ...) vkCmdWriteTimestamp2(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdPipelineBarrier2(cb, ...) vkCmdPipelineBarrier2(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdCopyBuffer(cb, ...) vkCmdCopyBuffer(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdUpdateBuffer(cb, ...) vkCmdUpdateBuffer(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdFillBuffer(cb, ...) vkCmdFillBuffer(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdCopyBufferToImage(cb, ...) vkCmdCopyBufferToImage(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdCopyImageToBuffer(cb, ...) vkCmdCopyImageToBuffer(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdCopyImage(cb, ...) vkCmdCopyImage(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdBlitImage(cb, ...) vkCmdBlitImage(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdClearColorImage(cb, ...) vkCmdClearColorImage(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdClearDepthStencilImage(cb, ...) vkCmdClearDepthStencilImage(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdClearAttachments(cb, ...) vkCmdClearAttachments(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdPushConstants(cb, ...) vkCmdPushConstants(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdBeginRendering(cb, info) (vk_one_primary_on ? vk_sections_begin(cb, info) : (vkCmdBeginRendering)(cb, info))
#define vkCmdEndRendering(cb) (vk_one_primary_on ? vk_sections_end(cb) : (vkCmdEndRendering)(cb))
#define vkCmdBindPipeline(cb, ...) vkCmdBindPipeline(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdBindDescriptorSets(cb, ...) vkCmdBindDescriptorSets(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdBindVertexBuffers2(cb, ...) vkCmdBindVertexBuffers2(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdBindIndexBuffer(cb, ...) vkCmdBindIndexBuffer(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdDraw(cb, ...) vkCmdDraw(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdDrawIndexed(cb, ...) vkCmdDrawIndexed(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdDispatch(cb, ...) vkCmdDispatch(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetViewport(cb, ...) vkCmdSetViewport(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetScissor(cb, ...) vkCmdSetScissor(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetDepthBias(cb, ...) vkCmdSetDepthBias(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetBlendConstants(cb, ...) vkCmdSetBlendConstants(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetStencilCompareMask(cb, ...) vkCmdSetStencilCompareMask(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetStencilWriteMask(cb, ...) vkCmdSetStencilWriteMask(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetStencilReference(cb, ...) vkCmdSetStencilReference(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetCullMode(cb, ...) vkCmdSetCullMode(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetFrontFace(cb, ...) vkCmdSetFrontFace(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetPrimitiveTopology(cb, ...) vkCmdSetPrimitiveTopology(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetDepthTestEnable(cb, ...) vkCmdSetDepthTestEnable(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetDepthWriteEnable(cb, ...) vkCmdSetDepthWriteEnable(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetDepthCompareOp(cb, ...) vkCmdSetDepthCompareOp(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetStencilTestEnable(cb, ...) vkCmdSetStencilTestEnable(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetStencilOp(cb, ...) vkCmdSetStencilOp(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetDepthBiasEnable(cb, ...) vkCmdSetDepthBiasEnable(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetPrimitiveRestartEnable(cb, ...) vkCmdSetPrimitiveRestartEnable(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetRasterizerDiscardEnable(cb, ...) vkCmdSetRasterizerDiscardEnable(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCmdSetLineWidth(cb, ...) vkCmdSetLineWidth(VK_SECTION_CB(cb), __VA_ARGS__)
#define vkCreateImage(dev, info, alloc, image) (vk_one_primary_on ? vk_sections_create_image(dev, info, alloc, image) : (vkCreateImage)(dev, info, alloc, image))
#define vkDestroyImage(dev, image, alloc) (vk_one_primary_on ? vk_sections_destroy_image(dev, image, alloc) : (vkDestroyImage)(dev, image, alloc))
#define vkCreateImageView(dev, info, alloc, view) (vk_one_primary_on ? vk_sections_create_view(dev, info, alloc, view) : (vkCreateImageView)(dev, info, alloc, view))
#define vkDestroyImageView(dev, view, alloc) (vk_one_primary_on ? vk_sections_destroy_view(dev, view, alloc) : (vkDestroyImageView)(dev, view, alloc))
