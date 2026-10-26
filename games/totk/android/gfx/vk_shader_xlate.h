/* Asynchronous shader-service client. Only its publication epoch crosses back to the recorder. */
#ifndef TOTK_VK_SHADER_XLATE_H
#define TOTK_VK_SHADER_XLATE_H
#include <stdint.h>
uint64_t vk_shader_xlate_epoch(void);
uint64_t vk_shader_xlate_take_miss(const char *variant);
void vk_shader_xlate_enqueue(const char *request, const char *variant, const char *seed);
#endif
