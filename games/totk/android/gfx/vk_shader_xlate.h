/* Asynchronous shader-service client. Only its publication epoch crosses back to the recorder. */
#ifndef TOTK_VK_SHADER_XLATE_H
#define TOTK_VK_SHADER_XLATE_H
#include <stdint.h>
uint64_t vk_shader_xlate_epoch(void);
uint64_t vk_shader_xlate_take_miss(const char *variant);
void vk_shader_xlate_enqueue(const char *request, const char *variant, const char *seed);
/* For a caller that waits for a variant (vk_shader.c miss_wait): 1 while the service can still answer; 1 once the
   service refused that variant or failed on it (waiting longer is useless). */
int vk_shader_xlate_available(void);
int vk_shader_xlate_rejected(const char *variant);
#endif
