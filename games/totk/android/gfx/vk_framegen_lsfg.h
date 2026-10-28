/* MIT. Optional user-DLL LSFG bridge; ordinary renderer owns the covering tickets. */
#pragma once
#include "vk_int.h"
int vk_fg_lsfg_requested(void);
int vk_fg_lsfg_probe_requested(void); /* exact TOTK_LSFG_PROBE=1; does not enable LSFG */
void vk_fg_lsfg_device(PFN_vkGetInstanceProcAddr ip, PFN_vkGetDeviceProcAddr dp, int interop);
int vk_fg_lsfg_active(void);
void vk_fg_lsfg_refuse(const char *reason);
void vk_fg_lsfg_late(void);
void vk_fg_lsfg_tick(int scene);
int vk_fg_lsfg_can_generate(void);
unsigned vk_fg_lsfg_pending(void); /* includes timestamp-quarantined work */
void vk_fg_lsfg_rearm(void); /* walking boundary, after pending work drains */
int vk_fg_lsfg_picture(void); /* immutable provenance of the composed current picture */
int vk_fg_lsfg_capture(VkCommandBuffer cb,unsigned source,VkImage scene,VkImageView view,
    VkFormat format,uint32_t width,uint32_t height);
/* Diagnostic inputs are copied only for dump-eligible generations. Tickets here
   are capture predictions/retirement observations, not exact capture submissions. */
typedef struct {
    uint64_t frame[2],epoch[2],capture_ticket[2],retire[2];
    VkCommandBuffer capture_cb[2];
    unsigned references[2];
    int history,pair_source;
} vk_fg_lsfg_provenance;
/* phase is finite in [0,1]; legacy callers pass .5f. Adaptive LSFG remains
 * disabled pending independent qualification of the doubling repair. */
int vk_fg_lsfg_interpolate(unsigned previous,unsigned current,uint64_t frame,float phase,
    const vk_fg_lsfg_provenance *provenance,VkImage *image,VkImageView *view);
/* Diagnostic seal observations only: neither changes resource/query tickets.
   Generic seals match both actual primary membership and recording incarnation;
   the final present observation leaves unmatched records UNKNOWN. */
void vk_fg_lsfg_submitting(VkCommandBuffer commands);
void vk_fg_lsfg_submitted(uint64_t actual_fence);
void vk_fg_lsfg_sealed(const VkCommandBuffer *commands,unsigned count,uint64_t actual_fence);
/* Pure attribution rule, shared with the host fixture. A reused CB handle in a
   different stream incarnation is not a match; a ticket is assigned only once. */
static inline int vk_fg_lsfg_probe_assign(uint64_t *assigned,VkCommandBuffer recorded,
        uint64_t incarnation,const VkCommandBuffer *sealed,unsigned count,
        uint64_t sealed_incarnation,uint64_t ticket) {
    if (*assigned || !recorded || !ticket || incarnation!=sealed_incarnation) return 0;
    for (unsigned i=0;i<count;i++) if (sealed[i]==recorded) {
        *assigned=ticket; return 1;
    }
    return 0;
}
void vk_fg_lsfg_release(VkCommandBuffer cb,unsigned source);
void vk_fg_lsfg_harvest(void);
void vk_fg_lsfg_result(int accepted);
void vk_fg_lsfg_stats(void);
/* Borrowed output, only while the renderer retains this source for a selected dump. */
int vk_fg_lsfg_dump_image(unsigned source,VkImage *image,VkFormat *format,uint32_t *width,uint32_t *height);
/* Caller has completed the native dump's existing covering GPU fence. */
int vk_fg_lsfg_probe_dump(unsigned source,uint64_t frame,const char *directory);
