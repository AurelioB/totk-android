#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1/shaders/mipmaps.hpp"
#include "common/utils.hpp"
#include "core/image.hpp"
#include "core/commandbuffer.hpp"

#include <utility>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace LSFG_3_1::Shaders;

Mipmaps::Mipmaps(Vulkan& vk,
        Core::Image inImg_0, Core::Image inImg_1)
        : inImg_0(std::move(inImg_0)), inImg_1(std::move(inImg_1)) {
    // create resources
    this->shaderModule = vk.shaders.getShader(vk.device, "mipmaps",
        { { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
          { 1, VK_DESCRIPTOR_TYPE_SAMPLER },
          { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
          { 7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE } });
    this->pipeline = vk.shaders.getPipeline(vk.device, "mipmaps");
    this->buffer = vk.resources.getBuffer(vk.device);
    this->sampler = vk.resources.getSampler(vk.device);
    for (size_t i = 0; i < 2; i++)
        this->descriptorSets.at(i) = Core::DescriptorSet(vk.device, vk.descriptorPool, this->shaderModule);

    // create outputs
    const VkExtent2D flowExtent{
        .width = static_cast<uint32_t>(
            static_cast<float>(this->inImg_0.getExtent().width) / vk.flowScale),
        .height = static_cast<uint32_t>(
            static_cast<float>(this->inImg_0.getExtent().height) / vk.flowScale)
    };
    const char *probe = std::getenv("TOTK_LSFG_PROBE");
    this->probeUsage = probe && probe[0] == '1' && probe[1] == '\0';
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        (this->probeUsage ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    for (size_t i = 0; i < 7; i++)
        this->outImgs.at(i) = Core::Image(vk.device,
            { flowExtent.width >> i, flowExtent.height >> i },
            VK_FORMAT_R8_UNORM, usage);

    // hook up shaders
    for (size_t fc = 0; fc < 2; fc++)
        this->descriptorSets.at(fc).update(vk.device)
            .add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, this->buffer)
            .add(VK_DESCRIPTOR_TYPE_SAMPLER, this->sampler)
            .add(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, (fc % 2 == 0) ? this->inImg_0 : this->inImg_1)
            .add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, this->outImgs)
            .build();
}

void Mipmaps::Dispatch(const Core::CommandBuffer& buf, uint64_t frameCount) {
    // first pass
    const auto flowExtent = this->outImgs.at(0).getExtent();
    const uint32_t threadsX = (flowExtent.width + 63) >> 6;
    const uint32_t threadsY = (flowExtent.height + 63) >> 6;

    Utils::BarrierBuilder(buf)
        .addW2R((frameCount % 2 == 0) ? this->inImg_0 : this->inImg_1)
        .addR2W(this->outImgs)
        .build();

    this->pipeline.bind(buf);
    this->descriptorSets.at(frameCount % 2).bind(buf, this->pipeline);
    buf.dispatch(threadsX, threadsY, 1);
}

void Mipmaps::rebindInputs(const Core::Device& device) {
    VkDescriptorImageInfo images[2]={
        {.imageView=inImg_0.getView(),.imageLayout=VK_IMAGE_LAYOUT_GENERAL},
        {.imageView=inImg_1.getView(),.imageLayout=VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (unsigned i=0;i<2;i++)
        writes[i]={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=descriptorSets[i].handle(),
            .dstBinding=2,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo=&images[i]};
    vkUpdateDescriptorSets(device.handle(),2,writes,0,nullptr);
}
