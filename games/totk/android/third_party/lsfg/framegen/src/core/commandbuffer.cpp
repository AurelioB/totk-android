#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/commandbuffer.hpp"
#include "core/device.hpp"
#include "core/commandpool.hpp"
#include "core/fence.hpp"
#include "core/semaphore.hpp"
#include "common/exception.hpp"

#include <memory>
#include <stdexcept>
#include <cstdint>
#include <optional>
#include <vector>
#include <cstdlib>

using namespace LSFG::Core;

CommandBuffer::CommandBuffer(const Core::Device& device, const CommandPool& pool) {
    // create command buffer
    const VkCommandBufferAllocateInfo desc{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool.handle(),
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer commandBufferHandle{};
    auto res = vkAllocateCommandBuffers(device.handle(), &desc, &commandBufferHandle);
    if (res != VK_SUCCESS || commandBufferHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Unable to allocate command buffer");

    // store command buffer in shared ptr
    this->state = std::make_shared<CommandBufferState>(CommandBufferState::Empty);
    this->commandBuffer = std::shared_ptr<VkCommandBuffer>(
        new VkCommandBuffer(commandBufferHandle),
        [dev = device.handle(), pool = pool.handle()](VkCommandBuffer* cmdBuffer) {
            vkFreeCommandBuffers(dev, pool, 1, cmdBuffer);
        }
    );
}

void CommandBuffer::reset() {
    auto result = vkResetCommandBuffer(*this->commandBuffer, 0);
    if (result != VK_SUCCESS)
        throw LSFG::vulkan_error(result, "Failed to reset retired command buffer");
    *this->state = CommandBufferState::Empty;
}

void CommandBuffer::begin() {
    if (*this->state != CommandBufferState::Empty)
        throw std::logic_error("Command buffer is not in Empty state");

    const VkCommandBufferBeginInfo beginInfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    auto res = vkBeginCommandBuffer(*this->commandBuffer, &beginInfo);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to begin command buffer");

    *this->state = CommandBufferState::Recording;
}

void CommandBuffer::dispatch(uint32_t x, uint32_t y, uint32_t z) const {
    if (getState() != CommandBufferState::Recording)
        throw std::logic_error("Command buffer is not in Recording state");
    // Diagnostic only: close every dispatch/transition dependency globally.
    // This deliberately over-synchronizes; never use its timing as the default.
    static const bool full = [] {
        const char *value=std::getenv("TOTK_LSFG_FULL_BARRIERS");
        return value && value[0]=='1' && value[1]=='\0';
    }();
    const VkMemoryBarrier2 memory{
        .sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT};
    const VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount=1,.pMemoryBarriers=&memory};
    if (full) vkCmdPipelineBarrier2(handle(),&dependency);
    vkCmdDispatch(handle(), x, y, z);
    if (full) vkCmdPipelineBarrier2(handle(),&dependency);
}

void CommandBuffer::end() {
    if (*this->state != CommandBufferState::Recording)
        throw std::logic_error("Command buffer is not in Recording state");

    auto res = vkEndCommandBuffer(*this->commandBuffer);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to end command buffer");

    *this->state = CommandBufferState::Full;
}

void CommandBuffer::submit(VkQueue queue, std::optional<Fence> fence,
        const std::vector<Semaphore>& waitSemaphores,
        std::optional<std::vector<uint64_t>> waitSemaphoreValues,
        const std::vector<Semaphore>& signalSemaphores,
        std::optional<std::vector<uint64_t>> signalSemaphoreValues) {
    if (*this->state != CommandBufferState::Full)
        throw std::logic_error("Command buffer is not in Full state");

    const std::vector<VkPipelineStageFlags> waitStages(waitSemaphores.size(),
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    VkTimelineSemaphoreSubmitInfo timelineInfo{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
    };
    if (waitSemaphoreValues.has_value()) {
        timelineInfo.waitSemaphoreValueCount =
            static_cast<uint32_t>(waitSemaphoreValues->size());
        timelineInfo.pWaitSemaphoreValues = waitSemaphoreValues->data();
    }
    if (signalSemaphoreValues.has_value()) {
        timelineInfo.signalSemaphoreValueCount =
            static_cast<uint32_t>(signalSemaphoreValues->size());
        timelineInfo.pSignalSemaphoreValues = signalSemaphoreValues->data();
    }

    std::vector<VkSemaphore> waitSemaphoresHandles;
    waitSemaphoresHandles.reserve(waitSemaphores.size());
    for (const auto& semaphore : waitSemaphores)
        waitSemaphoresHandles.push_back(semaphore.handle());
    std::vector<VkSemaphore> signalSemaphoresHandles;
    signalSemaphoresHandles.reserve(signalSemaphores.size());
    for (const auto& semaphore : signalSemaphores)
        signalSemaphoresHandles.push_back(semaphore.handle());

    const VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = (waitSemaphoreValues.has_value() || signalSemaphoreValues.has_value())
            ? &timelineInfo : nullptr,
        .waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size()),
        .pWaitSemaphores = waitSemaphoresHandles.data(),
        .pWaitDstStageMask = waitStages.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = &(*this->commandBuffer),
        .signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size()),
        .pSignalSemaphores = signalSemaphoresHandles.data()
    };
    auto res = vkQueueSubmit(queue, 1, &submitInfo, fence ? fence->handle() : VK_NULL_HANDLE);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Unable to submit command buffer");

    *this->state = CommandBufferState::Submitted;
}
