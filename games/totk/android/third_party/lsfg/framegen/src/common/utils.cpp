#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "common/utils.hpp"
#include "core/buffer.hpp"
#include "core/image.hpp"
#include "core/device.hpp"
#include "core/commandpool.hpp"
#include "core/fence.hpp"
#include "common/exception.hpp"

#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <ios>
#include <system_error>
#include <vector>

using namespace LSFG;
using namespace LSFG::Utils;

namespace {
    struct BarrierMasks {
        VkPipelineStageFlags2 stage;
        VkAccessFlags2 read;
        VkAccessFlags2 write;
    };

    const BarrierMasks& barrierMasks() {
        // Diagnostic only: replace the existing image-barrier masks, not the
        // separate global barriers around dispatches in FULL_BARRIERS mode.
        static const BarrierMasks masks = [] {
            const char *value = std::getenv("TOTK_LSFG_WIDE_BARRIERS");
            const bool wide = value && value[0] == '1' && value[1] == '\0';
            return wide
                ? BarrierMasks{VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT}
                : BarrierMasks{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT, VK_ACCESS_2_SHADER_WRITE_BIT};
        }();
        return masks;
    }
}

BarrierBuilder& BarrierBuilder::addR2W(Core::Image& image) {
    if (!image.valid()) return *this;
    const auto& masks = barrierMasks();
    this->barriers.at(count++) = VkImageMemoryBarrier2 {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = masks.stage,
        .srcAccessMask = masks.read,
        .dstStageMask = masks.stage,
        .dstAccessMask = masks.write,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    };
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    return *this;
}

BarrierBuilder& BarrierBuilder::addW2R(Core::Image& image) {
    if (!image.valid()) return *this;
    const auto& masks = barrierMasks();
    this->barriers.at(count++) = VkImageMemoryBarrier2 {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = masks.stage,
        .srcAccessMask = masks.write,
        .dstStageMask = masks.stage,
        .dstAccessMask = masks.read,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    };
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    return *this;
}

void BarrierBuilder::build() const {
    const VkDependencyInfo dependencyInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(this->count),
        .pImageMemoryBarriers = this->barriers.data()
    };
    vkCmdPipelineBarrier2(this->commandBuffer->handle(), &dependencyInfo);
}

void Utils::uploadImage(const Core::Device& device, const Core::CommandPool& commandPool,
        Core::Image& image, const std::string& path) {
    // read image bytecode
    std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw std::system_error(errno, std::generic_category(), "Failed to open image: " + path);

    std::streamsize size = file.tellg();
    size -= 124 + 4; // dds header and magic bytes
    std::vector<char> code(static_cast<size_t>(size));

    file.seekg(124 + 4, std::ios::beg);
    if (!file.read(code.data(), size))
        throw std::system_error(errno, std::generic_category(), "Failed to read image: " + path);

    file.close();

    // copy data to buffer
    const Core::Buffer stagingBuffer(
        device, code.data(), static_cast<uint32_t>(code.size()),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    );

    // perform the upload
    Core::CommandBuffer commandBuffer(device, commandPool);
    commandBuffer.begin();

    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_NONE,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    };
    image.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(
        commandBuffer.handle(),
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );

    auto extent = image.getExtent();
    const VkBufferImageCopy region{
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = image.getAspectFlags(),
            .layerCount = 1
        },
        .imageExtent = { extent.width, extent.height, 1 }
    };
    vkCmdCopyBufferToImage(
        commandBuffer.handle(),
        stagingBuffer.handle(), image.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region
    );

    commandBuffer.end();

    Core::Fence fence(device);
    commandBuffer.submit(device.getComputeQueue(), fence);

    // wait for the upload to complete
    if (!fence.wait(device))
        throw LSFG::vulkan_error(VK_TIMEOUT, "Upload operation timed out");
}

void Utils::clearImage(const Core::Device& device, Core::Image& image, bool white) {
    Core::Fence fence(device);
    const Core::CommandPool cmdPool(device);
    Core::CommandBuffer cmdBuf(device, cmdPool);
    cmdBuf.begin();

    const VkImageMemoryBarrier2 barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1
        }
    };
    const VkDependencyInfo dependencyInfo = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier
    };
    image.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier2(cmdBuf.handle(), &dependencyInfo);

    const float clearValue = white ? 1.0F : 0.0F;
    const VkClearColorValue clearColor = {{ clearValue, clearValue, clearValue, clearValue }};
    const VkImageSubresourceRange subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1,
        .layerCount = 1
    };
    vkCmdClearColorImage(cmdBuf.handle(),
        image.handle(), image.getLayout(),
        &clearColor,
        1, &subresourceRange);
    const VkImageMemoryBarrier2 ready{
        .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image.handle(),.subresourceRange=subresourceRange};
    const VkDependencyInfo readyInfo{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount=1,.pImageMemoryBarriers=&ready};
    vkCmdPipelineBarrier2(cmdBuf.handle(),&readyInfo);
    image.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    cmdBuf.end();

    cmdBuf.submit(device.getComputeQueue(), fence);
    if (!fence.wait(device))
        throw LSFG::vulkan_error(VK_TIMEOUT, "Failed to wait for clearing fence.");
}
