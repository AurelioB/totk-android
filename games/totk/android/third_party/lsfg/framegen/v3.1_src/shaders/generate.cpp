#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1/shaders/generate.hpp"
#include "common/utils.hpp"
#include "core/commandbuffer.hpp"
#include "core/image.hpp"
#include "pool/resourcepool.hpp"

#include <vector>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <stdexcept>

using namespace LSFG_3_1::Shaders;

Generate::Generate(Vulkan& vk,
    Core::Image inImg1, Core::Image inImg2,
    Core::Image inImg3, Core::Image inImg4, Core::Image inImg5,
    const std::vector<int>& fds, VkFormat format)
        : inImg1(std::move(inImg1)), inImg2(std::move(inImg2)),
          inImg3(std::move(inImg3)), inImg4(std::move(inImg4)),
          inImg5(std::move(inImg5)) {
    // create resources
    this->shaderModule = vk.shaders.getShader(vk.device, "generate",
        { { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
          { 2, VK_DESCRIPTOR_TYPE_SAMPLER },
          { 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
          { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } });
    this->pipeline = vk.shaders.getPipeline(vk.device, "generate");
    this->samplers.at(0) = vk.resources.getSampler(vk.device);
    this->samplers.at(1) = vk.resources.getSampler(vk.device,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS);

    // create internal images/outputs
    const VkExtent2D extent = this->inImg1.getExtent();
    for (size_t i = 0; i < vk.generationCount; i++)
        this->outImgs.emplace_back(vk.device, extent, format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, fds.empty() ? -1 : fds.at(i));

    // hook up shaders
    for (size_t i = 0; i < vk.generationCount; i++) {
        auto& pass = this->passes.emplace_back();
        pass.timestamp = static_cast<float>(i + 1) / static_cast<float>(vk.generationCount + 1);
        pass.buffer = vk.resources.getBuffer(vk.device, pass.timestamp);
        pass.phaseData = vk.resources.getBufferData(pass.timestamp);
        pass.phaseBuffer = Core::Buffer(vk.device, pass.phaseData,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (unsigned fractional = 0; fractional < 2; ++fractional) {
            auto& sets = fractional ? pass.phaseDescriptors : pass.descriptorSet;
            for (size_t j = 0; j < 2; j++) {
                sets.at(j) = Core::DescriptorSet(vk.device, vk.descriptorPool, this->shaderModule);
                sets.at(j).update(vk.device)
                    .add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fractional ? pass.phaseBuffer : pass.buffer)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->samplers)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, j == 0 ? this->inImg2 : this->inImg1)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, j == 0 ? this->inImg1 : this->inImg2)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->inImg3)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->inImg4)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->inImg5)
                    .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(i))
                    .build();
            }
        }
    }
}

void Generate::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount, uint64_t pass_idx) {
    dispatch(buf, frameCount, pass_idx, false);
}

void Generate::DispatchTimestamp(const Core::CommandBuffer& buf, uint64_t frameCount,
        uint64_t pass_idx, float timestamp) {
    if (!std::isfinite(timestamp) || timestamp < 0.0f || timestamp > 1.0f)
        throw std::runtime_error("LSFG timestamp must be finite in [0,1]");
    auto& pass = passes.at(pass_idx);
    if (timestamp == pass.timestamp) {
        Dispatch(buf, frameCount, pass_idx);
        return;
    }
    // vkCmdUpdateBuffer copies these float bytes into the command stream now.
    // Distinct phases recorded in one primary (or primaries in queue order)
    // cannot all observe the last CPU write to a shared mapped uniform.
    VkBufferMemoryBarrier2 barrier{.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=pass.phaseBuffer.handle(),.offset=offsetof(Pool::ConstantBuffer, timestamp),
        .size=sizeof timestamp};
    const VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&barrier};
    vkCmdPipelineBarrier2(buf.handle(), &dependency);
    vkCmdUpdateBuffer(buf.handle(), pass.phaseBuffer.handle(),
        offsetof(Pool::ConstantBuffer, timestamp), sizeof timestamp, &timestamp);
    barrier.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask=VK_ACCESS_2_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier2(buf.handle(), &dependency);
    dispatch(buf, frameCount, pass_idx, true);
}

void Generate::dispatch(const Core::CommandBuffer& buf, uint64_t frameCount,
        uint64_t pass_idx, bool fractional) {
    auto& pass = this->passes.at(pass_idx);

    // first pass
    const auto extent = this->inImg1.getExtent();
    const uint32_t threadsX = (extent.width + 15) >> 4;
    const uint32_t threadsY = (extent.height + 15) >> 4;

    Utils::BarrierBuilder(buf)
        .addW2R(this->inImg1)
        .addW2R(this->inImg2)
        .addW2R(this->inImg3)
        .addW2R(this->inImg4)
        .addW2R(this->inImg5)
        .addR2W(this->outImgs.at(pass_idx))
        .build();

    this->pipeline.bind(buf);
    (fractional ? pass.phaseDescriptors : pass.descriptorSet).at(frameCount % 2).bind(buf, this->pipeline);
    buf.dispatch(threadsX, threadsY, 1);
}

Generate::Generate(Vulkan& vk,
    Core::Image inImg1, Core::Image inImg2,
    Core::Image inImg3, Core::Image inImg4, Core::Image inImg5,
    std::vector<Core::Image> outImgs)
        : inImg1(std::move(inImg1)), inImg2(std::move(inImg2)),
          inImg3(std::move(inImg3)), inImg4(std::move(inImg4)),
          inImg5(std::move(inImg5)),
          outImgs(std::move(outImgs)) {
    // create resources (same as the FD-based ctor)
    this->shaderModule = vk.shaders.getShader(vk.device, "generate",
        { { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
          { 2, VK_DESCRIPTOR_TYPE_SAMPLER },
          { 5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
          { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } });
    this->pipeline = vk.shaders.getPipeline(vk.device, "generate");
    this->samplers.at(0) = vk.resources.getSampler(vk.device);
    this->samplers.at(1) = vk.resources.getSampler(vk.device,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_COMPARE_OP_ALWAYS);

    // hook up shaders — outImgs already populated by the caller
    for (size_t i = 0; i < vk.generationCount; i++) {
        auto& pass = this->passes.emplace_back();
        pass.timestamp = static_cast<float>(i + 1) / static_cast<float>(vk.generationCount + 1);
        pass.buffer = vk.resources.getBuffer(vk.device, pass.timestamp);
        pass.phaseData = vk.resources.getBufferData(pass.timestamp);
        pass.phaseBuffer = Core::Buffer(vk.device, pass.phaseData,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for (unsigned fractional = 0; fractional < 2; ++fractional) {
            auto& sets = fractional ? pass.phaseDescriptors : pass.descriptorSet;
            for (size_t j = 0; j < 2; j++) {
                sets.at(j) = Core::DescriptorSet(vk.device, vk.descriptorPool, this->shaderModule);
                sets.at(j).update(vk.device)
                    .add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fractional ? pass.phaseBuffer : pass.buffer)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->samplers)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, j == 0 ? this->inImg2 : this->inImg1)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, j == 0 ? this->inImg1 : this->inImg2)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->inImg3)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->inImg4)
                    .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, this->inImg5)
                    .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs.at(i))
                    .build();
            }
        }
    }
}

void Generate::recordRefresh(const Core::CommandBuffer& buf) const {
    for (const auto& pass : passes) {
        VkBufferMemoryBarrier2 barrier{.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            .srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=pass.phaseBuffer.handle(),.offset=0,.size=sizeof pass.phaseData};
        const VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&barrier};
        vkCmdPipelineBarrier2(buf.handle(), &dependency);
        vkCmdUpdateBuffer(buf.handle(), pass.phaseBuffer.handle(), 0, sizeof pass.phaseData, &pass.phaseData);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
        vkCmdPipelineBarrier2(buf.handle(), &dependency);
    }
}

void Generate::rebindInputs(const Core::Device& device) {
    VkDescriptorImageInfo images[2]={
        {.imageView=inImg1.getView(),.imageLayout=VK_IMAGE_LAYOUT_GENERAL},
        {.imageView=inImg2.getView(),.imageLayout=VK_IMAGE_LAYOUT_GENERAL}};
    for (auto& pass:passes) {
        for (auto& sets : {&pass.descriptorSet, &pass.phaseDescriptors}) {
            VkWriteDescriptorSet writes[4]{};
            for (unsigned parity=0;parity<2;parity++) for (unsigned input=0;input<2;input++)
                writes[parity*2+input]={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet=sets->at(parity).handle(),.dstBinding=3+input,
                    .descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                    .pImageInfo=&images[parity ? input : 1-input]};
            vkUpdateDescriptorSets(device.handle(),4,writes,0,nullptr);
        }
    }
}
