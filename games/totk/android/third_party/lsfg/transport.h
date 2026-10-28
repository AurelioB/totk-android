/* MIT. Shared probe: transport images never require storage-image usage. */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan_core.h>
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
static inline int totk_lsfg_probe_ahb(PFN_vkGetPhysicalDeviceImageFormatProperties2 query,
    VkPhysicalDevice device, VkFormat format, VkImageUsageFlags usage,
    uint32_t width, uint32_t height, uint64_t *android_usage) {
    VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.pNext=&external,
        .format=format,.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage};
    VkAndroidHardwareBufferUsageANDROID ahb = {
        .sType=VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
    VkExternalImageFormatProperties external_properties = {
        .sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,.pNext=&ahb};
    VkImageFormatProperties2 properties = {
        .sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,.pNext=&external_properties};
    if (!query || query(device,&info,&properties)!=VK_SUCCESS) return 0;
    const VkExternalMemoryProperties *memory=&external_properties.externalMemoryProperties;
    if (!(memory->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        !(memory->compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) ||
        properties.imageFormatProperties.maxExtent.width<width ||
        properties.imageFormatProperties.maxExtent.height<height ||
        !(properties.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) return 0;
    *android_usage=ahb.androidHardwareBufferUsage;
    return 1;
}
#endif
