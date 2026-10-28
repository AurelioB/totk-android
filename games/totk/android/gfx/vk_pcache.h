/* Lifecycle calls never acquire the recording lock or perform Vulkan/file IO on the UI thread. */
#pragma once
#include <stdint.h>
#include <stddef.h>
/* Startup-only: shared validation/quarantine for both initial data and one-shot import. */
void *vk_pcache_load(const char *path, size_t *size, int primary);
void vk_pcache_start(const char *path, size_t loaded_size);
void vk_pcache_background(int background); /* pause/stop request an immediate asynchronous flush */
void vk_pcache_flush(void);                /* synchronous rendezvous with the single saver owner */
void vk_pcache_shutdown(void);             /* final flush, join, release scratch; safe before initialization */
void vk_pcache_present(uint64_t now, uint64_t draws);
typedef struct { uint64_t wall, busy; } vk_pcache_span;
vk_pcache_span vk_pcache_span_begin(void);
void vk_pcache_span_end(vk_pcache_span span, const char *operation);
#ifdef VK_VERSION_1_0
VkResult vk_pcache_graphics(const VkGraphicsPipelineCreateInfo *info, VkPipeline *pipeline);
VkResult vk_pcache_compute(const VkComputePipelineCreateInfo *info, VkPipeline *pipeline);
#endif
void vk_pcache_changed(void); /* startup import, before concurrent creation begins */
/* Immutable cache identity admission, and saver-owned portable key persistence callbacks. */
const char *vk_pcache_loaded_path(void);
const char *vk_pcache_current_path(void);
void vk_pipeline_keys_collect_save(void);
int vk_pipeline_keys_dirty(void);
void *vk_pipeline_keys_snapshot(void);
void vk_pipeline_keys_saved(const char *cache, void *snapshot);
void vk_pipeline_keys_stats(uint64_t out[6]); /* done, total, build ns, missing, failed, collected */
void vk_pipeline_keys_pressure(uint64_t now, int busy);
#ifdef TOTK_VK_TESTS
void vk_pcache_test_stats(uint64_t out[7]); /* saves, generation, saved generation, capacity, allocations, bytes written, loaded size */
#endif
