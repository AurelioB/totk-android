/* MIT. Private ABI between the C renderer and the isolated C++/volk backend. */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Calls are serialized by the renderer; borrowed objects remain renderer-owned.
 * Every context update/delete requires retirement of its covering source ticket. */
enum { TOTK_LSFG_TIMESTAMPS=30, TOTK_LSFG_PASSES=28 };
typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
} totk_lsfg_image;
int totk_lsfg_initialize(PFN_vkGetInstanceProcAddr loader, VkInstance instance,
    VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family,
    int null_descriptor, const char *dll, int performance, float flow, char error[256]);
int totk_lsfg_context(totk_lsfg_image previous, totk_lsfg_image current,
    totk_lsfg_image output, uint32_t width, uint32_t height, int32_t *context, char error[256]);
int totk_lsfg_rebind(int32_t context, totk_lsfg_image previous, totk_lsfg_image current, char error[256]);
void totk_lsfg_retire(int32_t context);
/* Records into the caller's existing primary. No queue submit, wait or allocation.
 * Inputs enter/leave SHADER_READ_ONLY_OPTIMAL; output leaves GENERAL, sampled.
 * Query pool has TOTK_LSFG_TIMESTAMPS entries, owned/reset by the caller.
 * timestamp is finite in [0,1], copied into command-owned update data. Exactly
 * .5f preserves the immutable legacy uniform/descriptor/dispatch path. */
int totk_lsfg_record(int32_t context, VkCommandBuffer commands, float timestamp,
    VkQueryPool timestamps, char error[256]);
/* Private, default-off diagnostic ABI; all three calls require the exact
 * environment value TOTK_LSFG_PROBE=1, set before context creation.
 * prepare allocates reusable context-owned readback outside recording.
 * record_probe records the ordinary pair plus all seven R8 outputs immediately
 * after EACH input's mipmaps dispatch; its 30 timestamp markers are unchanged.
 * Neither call submits or waits, and record_probe allocates nothing.
 * write requires the caller's covering GPU fence to have completed. It exports
 * lsfg-mipmaps-frame{0,1}-level{0..6}.r8 and lsfg-probe-manifest.txt (tightly
 * packed extents, buffer offsets/byte counts, actual bound input image/views).
 * Normal recording/rebinding invalidates an older probe. All return 0 with an
 * error on opt-out, unavailable capture/resources, allocation or file failure.
 * retire frees readback under the same existing covering-ticket ownership. */
int totk_lsfg_probe_prepare(int32_t context, char error[256]);
int totk_lsfg_record_probe(int32_t context, VkCommandBuffer commands,
    VkQueryPool timestamps, char error[256]);
int totk_lsfg_probe_write(int32_t context, const char *directory, char error[256]);
const char *totk_lsfg_notices(void);
#ifdef __cplusplus
}
#endif
