/* MIT. Private, opt-in Vulkan/GLES AFME bridge; recorder ownership unless noted. */
#pragma once
#include "vk_int.h"

/* Pure selection; runtime capability failure never changes the selected setting. */
int vk_fg_qcom_requested(void);
void vk_fg_qcom_device(PFN_vkGetInstanceProcAddr instance_proc,
    PFN_vkGetDeviceProcAddr device_proc, int interop);
int vk_fg_qcom_active(void);
void vk_fg_qcom_refuse(const char *reason); /* thread-safe; adaptive refuses to real-only, locked to camera */
void vk_fg_qcom_late(void);
void vk_fg_qcom_tick(int scene);
int vk_fg_qcom_can_generate(void);
void vk_fg_qcom_prepare(uint32_t width,uint32_t height); /* starts isolated worker, never joins */
unsigned vk_fg_qcom_pending(void);
void vk_fg_qcom_rearm(void); /* walking boundary, after external resources/budget drain */
int vk_fg_qcom_picture(void); /* walking-owned, immutable engine of composed current picture */
/* Only called for an idle source-ring entry. Copies linear pre-UI RGB into a
 * private RGBA16F AHB; ordinary source/depth snapshots remain untouched. */
int vk_fg_qcom_capture(VkCommandBuffer cb, unsigned source, VkImage scene,
    unsigned level,unsigned layer,uint32_t width, uint32_t height);
/* Ends/submits the caller's primary, then server-waits Vulkan -> GL -> Vulkan.
 * The caller MUST refresh vk_cb() afterwards, including on failure.
 * Output slots are bounded by VK_PRESENT_SLOT_CAPACITY, independent of source
 * scenes. phase must be finite in [0,1]; legacy callers pass exactly .5f.
 * A successful output requires release while recording, then complete only
 * after its covering Vulkan fence retires, even if presentation was canceled. */
int vk_fg_qcom_extrapolate(unsigned previous, unsigned current, unsigned output_slot,
    uint64_t frame, float phase, VkImage *image, VkImageView *view);
/* Immutable template lane: prepare leases exact sources/output and imports
 * storage, without GL extrapolation, a ticket or pending timeout. Acquire and
 * release are record-only barriers into caller-owned closed command storage.
 * Dispatch requires two preclaimed normal recorder slots; phase is GL-only.
 * Returns 1 on success, 0 before GL, -1 for an irrecoverable bridge quarantine. */
int vk_fg_qcom_output_prepare(unsigned previous,unsigned current,unsigned slot,VkImageView *view);
int vk_fg_qcom_output_dispatch(unsigned previous,unsigned current,unsigned slot,uint64_t frame,float phase,uint64_t deadline_ns);
void vk_fg_qcom_output_cancel(unsigned slot);
void vk_fg_qcom_acquire(VkCommandBuffer cb,unsigned slot);
void vk_fg_qcom_release(VkCommandBuffer cb, unsigned output_slot);
void vk_fg_qcom_complete(unsigned output_slot);
/* Nonblocking query harvest; context access serialized with the recording owner. */
void vk_fg_qcom_harvest(void);
void vk_fg_qcom_stats(void);
/* Bridge calls these with authenticated frame identities. Charge the greater
 * of GL elapsed and the conservative input-to-output native-fence span. */
void vk_fg_qcom_cost_begin(uint64_t frame);
void vk_fg_qcom_cost_end(uint64_t frame, double ms, int valid);
