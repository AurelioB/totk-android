#include <volk.h>

#include "pool/resourcepool.hpp"
#include "core/buffer.hpp"
#include "core/device.hpp"
#include "core/sampler.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

using namespace LSFG;
using namespace LSFG::Pool;

ConstantBuffer ResourcePool::getBufferData(float timestamp, bool firstIter, bool firstIterS) const {
    return {
        .inputOffset = { 0, 0 },
        .firstIter = firstIter ? 1U : 0U,
        .firstIterS = firstIterS ? 1U : 0U,
        .advancedColorKind = this->isHdr ? 2U : 0U,
        .hdrSupport = this->isHdr,
        .resolutionInvScale = this->flowScale,
        .timestamp = timestamp,
        .uiThreshold = 0.5F,
    };
}

Core::Buffer ResourcePool::getBuffer(
            const Core::Device& device,
            float timestamp, bool firstIter, bool firstIterS) {
    uint64_t hash = 0;
    const union { float f; uint32_t i; } u{
        .f = timestamp };
    hash |= u.i;
    hash |= static_cast<uint64_t>(firstIter) << 32;
    hash |= static_cast<uint64_t>(firstIterS) << 33;

    auto it = buffers.find(hash);
    if (it != buffers.end())
        return it->second;

    // create the buffer; the default-on refresh re-writes it with vkCmdUpdateBuffer
    const ConstantBuffer data = getBufferData(timestamp, firstIter, firstIterS);
    const bool refresh = Core::uboRefreshEnabled();
    Core::Buffer buffer(device, data, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        (refresh ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0u));
    buffers[hash] = buffer;
    if (refresh) refreshed.emplace_back(buffer, data);
    return buffer;
}

void ResourcePool::recordRefresh(const Core::CommandBuffer& commands) {
    constexpr size_t capacity = 16;
    if (refreshed.empty()) return;
    if (refreshed.size() > capacity)
        throw std::runtime_error("LSFG UBO refresh supports at most 16 pooled constant buffers");
    std::array<VkBufferMemoryBarrier2, capacity> barriers{};
    const auto count = static_cast<uint32_t>(refreshed.size());
    // Before: prior LSFG reads (compute) and the previous refresh (transfer).
    for (uint32_t i = 0; i < count; ++i)
        barriers[i] = {.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=refreshed[i].first.handle(),.offset=0,.size=sizeof(ConstantBuffer)};
    VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount=count,.pBufferMemoryBarriers=barriers.data()};
    vkCmdPipelineBarrier2(commands.handle(), &dependency);
    for (const auto& [buffer, data] : refreshed)
        vkCmdUpdateBuffer(commands.handle(), buffer.handle(), 0, sizeof data, &data);
    for (uint32_t i = 0; i < count; ++i) {
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barriers[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
    }
    vkCmdPipelineBarrier2(commands.handle(), &dependency);
    if (refreshLogged != refreshed.size()) {
        refreshLogged = refreshed.size();
        std::fprintf(stderr, "[lsfg] ubo-refresh: active=1 buffers=%zu bytes=48 via=vkCmdUpdateBuffer\n", refreshLogged);
    }
}

Core::Sampler ResourcePool::getSampler(
            const Core::Device& device,
            VkSamplerAddressMode type,
            VkCompareOp compare,
            bool isWhite) {
    uint64_t hash = 0;
    hash |= static_cast<uint64_t>(type) << 0;
    hash |= static_cast<uint64_t>(compare) << 8;
    hash |= static_cast<uint64_t>(isWhite) << 16;

    auto it = samplers.find(hash);
    if (it != samplers.end())
        return it->second;

    // create the sampler
    Core::Sampler sampler(device, type, compare, isWhite);
    samplers[hash] = sampler;
    return sampler;
}
