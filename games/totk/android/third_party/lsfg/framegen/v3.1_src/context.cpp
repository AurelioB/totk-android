#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "v3_1/context.hpp"
#include "common/utils.hpp"
#include "common/exception.hpp"

#include <vector>
#include <cstddef>
#include <algorithm>
#include <optional>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <cmath>

namespace {
void require_probe() {
    const char *value = std::getenv("TOTK_LSFG_PROBE");
    if (!value || value[0] != '1' || value[1] != '\0')
        throw std::runtime_error("LSFG probe requires TOTK_LSFG_PROBE=1");
}

void probe_check(VkResult result, const char *message) {
    if (result != VK_SUCCESS) throw LSFG::vulkan_error(result, message);
}

template<typename T>
uint64_t probe_handle(T handle) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(handle);
    else return static_cast<uint64_t>(handle);
}
}

using namespace LSFG_3_1;
// Readback is retained by the source-owned context, never a temporary recording
// helper. The renderer retires that context only after its covering fence.
struct Context::Probe {
    explicit Probe(VkDevice device) : device(device) {}
    ~Probe() {
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
    VkDevice device;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize size{};
    bool coherent{}, recorded{};
    struct Snapshot {
        VkDeviceSize offset{}, bytes{};
        VkExtent2D extent{};
        VkFormat format{};
        VkImage image{};
        VkImageView view{};
    };
    struct Input {
        VkImage image{};
        VkImageView view{};
        VkExtent2D extent{};
        VkFormat format{};
    };
    std::array<Snapshot, 14> snapshots{};
    std::array<Input, 2> inputs{};
};

void Context::probePrepare(Vulkan& vk) {
    require_probe();
    if (!mipmaps.hasProbeUsage())
        throw std::runtime_error("LSFG probe requires opt-in before context creation");
    if (probe) {
        probe->recorded = false;
        return;
    }
    auto pending = std::make_shared<Probe>(vk.device.handle());
    for (unsigned frame = 0; frame < 2; ++frame) {
        for (unsigned level = 0; level < 7; ++level) {
            const auto& image = mipmaps.getOutImages().at(level);
            const auto extent = image.getExtent();
            if (image.getFormat() != VK_FORMAT_R8_UNORM || !extent.width || !extent.height)
                throw std::runtime_error("LSFG probe expected seven nonempty R8 mipmaps outputs");
            // Vulkan buffer/image copy offsets must be a multiple of four.
            pending->size = (pending->size + 3) & ~VkDeviceSize(3);
            const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height;
            pending->snapshots.at(frame * 7 + level) = {
                pending->size, bytes, extent, image.getFormat(), image.handle(), image.getView()};
            pending->size += bytes;
        }
    }
    const VkBufferCreateInfo bufferInfo{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=pending->size,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    probe_check(vkCreateBuffer(pending->device,&bufferInfo,nullptr,&pending->buffer),
        "Failed to create LSFG probe readback buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(pending->device,pending->buffer,&requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(vk.device.getPhysicalDevice(),&properties);
    std::optional<uint32_t> type;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            continue;
        if (!type || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) type = i;
        if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
    }
    if (!type) throw std::runtime_error("LSFG probe HOST_VISIBLE readback memory unavailable");
    pending->coherent = properties.memoryTypes[*type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryAllocateInfo allocation{.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize=requirements.size,.memoryTypeIndex=*type};
    probe_check(vkAllocateMemory(pending->device,&allocation,nullptr,&pending->memory),
        "Failed to allocate LSFG probe readback memory");
    probe_check(vkBindBufferMemory(pending->device,pending->buffer,pending->memory,0),
        "Failed to bind LSFG probe readback memory");
    probe = std::move(pending);
}

void Context::recordProbe(Vulkan& vk, const Core::CommandBuffer& buf, VkQueryPool timestamps) {
    require_probe();
    if (!probe) throw std::runtime_error("LSFG probe was not prepared");
    probe->recorded = false;
    Core::Image* inputs[]{&inImg_0,&inImg_1};
    for (unsigned frame = 0; frame < 2; ++frame) {
        const auto& image = *inputs[frame];
        probe->inputs[frame] = {image.handle(),image.getView(),image.getExtent(),image.getFormat()};
    }
    recordPairCommands(vk,buf,timestamps,true,.5f);
    const VkBufferMemoryBarrier2 host{.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=probe->buffer,.offset=0,.size=VK_WHOLE_SIZE};
    const VkDependencyInfo availability{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&host};
    vkCmdPipelineBarrier2(buf.handle(),&availability);
    probe->recorded = true;
}

void Context::probeSnapshot(const Core::CommandBuffer& buf, unsigned frame) const {
    std::array<VkImageMemoryBarrier2,7> barriers{};
    for (unsigned level = 0; level < 7; ++level) {
        const auto& snapshot = probe->snapshots[frame * 7 + level];
        barriers[level] = {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,.srcAccessMask=VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=snapshot.image,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    }
    const VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount=static_cast<uint32_t>(barriers.size()),.pImageMemoryBarriers=barriers.data()};
    vkCmdPipelineBarrier2(buf.handle(),&dependency);
    for (unsigned level = 0; level < 7; ++level) {
        const auto& snapshot = probe->snapshots[frame * 7 + level];
        const VkBufferImageCopy region{.bufferOffset=snapshot.offset,
            .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .imageExtent={snapshot.extent.width,snapshot.extent.height,1}};
        vkCmdCopyImageToBuffer(buf.handle(),snapshot.image,VK_IMAGE_LAYOUT_GENERAL,
            probe->buffer,1,&region);
    }
    for (auto& barrier : barriers) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier2(buf.handle(),&dependency);
}

void Context::probeWrite(const char *directory) const {
    require_probe();
    if (!probe || !probe->recorded) throw std::runtime_error("LSFG probe has no recorded pair");
    if (!directory || !directory[0]) throw std::runtime_error("LSFG probe output directory is empty");
    void *mapped{};
    // Mapping the whole allocation also makes the noncoherent invalidation's
    // offset/size atom-aligned, including any memory-requirements tail padding.
    probe_check(vkMapMemory(probe->device,probe->memory,0,VK_WHOLE_SIZE,0,&mapped),
        "Failed to map LSFG probe readback memory");
    try {
        if (!probe->coherent) {
            const VkMappedMemoryRange range{.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory=probe->memory,.offset=0,.size=VK_WHOLE_SIZE};
            probe_check(vkInvalidateMappedMemoryRanges(probe->device,1,&range),
                "Failed to invalidate LSFG probe readback memory");
        }
        const std::string path = std::string(directory) + "/";
        auto write = [](const std::string& filename, const uint8_t *pixels, VkDeviceSize bytes) {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(filename,std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(pixels),static_cast<std::streamsize>(bytes));
            file.close();
        };
        for (unsigned frame = 0; frame < 2; ++frame)
            for (unsigned level = 0; level < 7; ++level) {
                const auto& snapshot = probe->snapshots[frame * 7 + level];
                const auto filename = "lsfg-mipmaps-frame" + std::to_string(frame) +
                    "-level" + std::to_string(level) + ".r8";
                write(path + filename,static_cast<const uint8_t*>(mapped) + snapshot.offset,snapshot.bytes);
            }
        // Publish metadata only after all fourteen pixel files have closed.
        std::ofstream manifest;
        manifest.exceptions(std::ios::failbit | std::ios::badbit);
        manifest.open(path + "lsfg-probe-manifest.txt",std::ios::trunc);
        manifest << "lsfg_probe_version=1\nsnapshots=14\nbuffer_bytes=" << probe->size << '\n';
        for (unsigned frame = 0; frame < 2; ++frame) {
            const auto& input = probe->inputs[frame];
            manifest << "input frame=" << frame << " image=0x" << std::hex << probe_handle(input.image)
                << " view=0x" << probe_handle(input.view) << std::dec << " format=" << input.format
                << " width=" << input.extent.width << " height=" << input.extent.height << '\n';
            for (unsigned level = 0; level < 7; ++level) {
                const auto& snapshot = probe->snapshots[frame * 7 + level];
                manifest << "snapshot frame=" << frame << " level=" << level
                    // Each preceding input runs one mipmaps + seven four-dispatch Alpha stages.
                    << " mipmaps_dispatch=" << frame + 1 << " chain_dispatch=" << frame * (1 + 7 * 4) + 1
                    << " aggregate_stage=" << frame * 8 + 1
                    << " file=lsfg-mipmaps-frame" << frame << "-level" << level << ".r8"
                    << " format=VK_FORMAT_R8_UNORM vk_format=" << snapshot.format
                    << " width=" << snapshot.extent.width << " height=" << snapshot.extent.height
                    << " offset=" << snapshot.offset << " bytes=" << snapshot.bytes
                    << " row_bytes=" << snapshot.extent.width << " image=0x" << std::hex
                    << probe_handle(snapshot.image) << " view=0x" << probe_handle(snapshot.view)
                    << std::dec << '\n';
            }
        }
        manifest.close();
    } catch (...) {
        vkUnmapMemory(probe->device,probe->memory);
        throw;
    }
    vkUnmapMemory(probe->device,probe->memory);
}

#ifdef __ANDROID__
namespace {

void add_external_acquire(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkAccessFlags2 accessMask) {
    if (!image.isExternalShared())
        return;

    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = accessMask,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .dstQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1,
        },
    });
}

void add_external_release(std::vector<VkImageMemoryBarrier2>& barriers,
        Vulkan& vk, Core::Image& image, VkAccessFlags2 accessMask) {
    if (!image.isExternalShared())
        return;

    barriers.emplace_back(VkImageMemoryBarrier2{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = accessMask,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = image.getLayout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = vk.device.getComputeFamilyIdx(),
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
        .image = image.handle(),
        .subresourceRange = {
            .aspectMask = image.getAspectFlags(),
            .levelCount = 1,
            .layerCount = 1,
        },
    });
}

void emit_external_barriers(const Core::CommandBuffer& buf,
        const std::vector<VkImageMemoryBarrier2>& barriers) {
    if (barriers.empty())
        return;

    const VkDependencyInfo dependencyInfo{
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
        .pImageMemoryBarriers = barriers.data(),
    };
    vkCmdPipelineBarrier2(buf.handle(), &dependencyInfo);
}

} // namespace
#endif

Context::Context(Vulkan& vk,
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format) {
    // import input images
    this->inImg_0 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in0);
    this->inImg_1 = Core::Image(vk.device, extent, format,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, in1);

    // prepare render data
    for (size_t i = 0; i < 8; i++) {
        auto& data = this->data.at(i);
        data.internalSemaphores.resize(vk.generationCount);
        data.outSemaphores.resize(vk.generationCount);
        data.completionFences.resize(vk.generationCount);
        data.cmdBuffers2.resize(vk.generationCount);
    }

    // create shader chains
    this->mipmaps = Shaders::Mipmaps(vk, this->inImg_0, this->inImg_1);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(i) = Shaders::Alpha(vk, this->mipmaps.getOutImages().at(i));
    this->beta = Shaders::Beta(vk, this->alpha.at(0).getOutImages());
    for (size_t i = 0; i < 7; i++) {
        this->gamma.at(i) = Shaders::Gamma(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(std::min<size_t>(6 - i, 5)),
            (i == 0) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()));
        if (i < 4) continue;

        this->delta.at(i - 4) = Shaders::Delta(vk,
            this->alpha.at(6 - i).getOutImages(),
            this->beta.getOutImages().at(6 - i),
            (i == 4) ? std::nullopt : std::make_optional(this->gamma.at(i - 1).getOutImage()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage1()),
            (i == 4) ? std::nullopt : std::make_optional(this->delta.at(i - 5).getOutImage2()));
    }
    this->generate = Shaders::Generate(vk,
        this->inImg_0, this->inImg_1,
        this->gamma.at(6).getOutImage(),
        this->delta.at(2).getOutImage1(),
        this->delta.at(2).getOutImage2(),
        outN, format);
}

void Context::present(Vulkan& vk,
        int inSem, const std::vector<int>& outSem) {
    auto& data = this->data.at(this->frameIdx % 8);

    // 3. wait for completion of previous frame in this slot
    if (data.shouldWait)
        for (auto& fence : data.completionFences)
            if (!fence.wait(vk.device, UINT64_MAX))
                throw LSFG::vulkan_error(VK_TIMEOUT, "Fence wait timed out");
    data.shouldWait = true;

    // 1. create mipmaps and process input image
    if (inSem >= 0) data.inSemaphore = Core::Semaphore(vk.device, inSem);
    for (size_t i = 0; i < vk.generationCount; i++)
        data.internalSemaphores.at(i) = Core::Semaphore(vk.device);

    data.cmdBuffer1 = Core::CommandBuffer(vk.device, vk.commandPool);
    data.cmdBuffer1.begin();

#ifdef __ANDROID__
    {
        std::vector<VkImageMemoryBarrier2> acquireBarriers;
        acquireBarriers.reserve(2);
        add_external_acquire(acquireBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);
        add_external_acquire(acquireBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);
        emit_external_barriers(data.cmdBuffer1, acquireBarriers);
    }
#endif

    this->mipmaps.Dispatch(data.cmdBuffer1, this->frameIdx);
    for (size_t i = 0; i < 7; i++)
        this->alpha.at(6 - i).Dispatch(data.cmdBuffer1, this->frameIdx);
    this->beta.Dispatch(data.cmdBuffer1, this->frameIdx);

    data.cmdBuffer1.end();
    std::vector<Core::Semaphore> waits = { data.inSemaphore };
    if (inSem < 0) waits.clear();
    data.cmdBuffer1.submit(vk.device.getComputeQueue(), std::nullopt,
        waits, std::nullopt,
        data.internalSemaphores, std::nullopt);

    // 2. generate intermediary frames
    for (size_t pass = 0; pass < vk.generationCount; pass++) {
        auto& internalSemaphore = data.internalSemaphores.at(pass);
        auto& outSemaphore = data.outSemaphores.at(pass);
        if (inSem >= 0) outSemaphore = Core::Semaphore(vk.device, outSem.empty() ? -1 : outSem.at(pass));
        auto& completionFence = data.completionFences.at(pass);
        completionFence = Core::Fence(vk.device);

        auto& buf2 = data.cmdBuffers2.at(pass);
        buf2 = Core::CommandBuffer(vk.device, vk.commandPool);
        buf2.begin();

#ifdef __ANDROID__
        {
            std::vector<VkImageMemoryBarrier2> acquireBarriers;
            acquireBarriers.reserve(1);
            add_external_acquire(acquireBarriers, vk, this->generate.getOutImages().at(pass),
                VK_ACCESS_2_SHADER_WRITE_BIT);
            emit_external_barriers(buf2, acquireBarriers);
        }
#endif

        for (size_t i = 0; i < 7; i++) {
            this->gamma.at(i).Dispatch(buf2, this->frameIdx, pass);
            if (i >= 4)
                this->delta.at(i - 4).Dispatch(buf2, this->frameIdx, pass);
        }
        this->generate.Dispatch(buf2, this->frameIdx, pass);

#ifdef __ANDROID__
        {
            std::vector<VkImageMemoryBarrier2> releaseBarriers;
            releaseBarriers.reserve(pass + 1 == vk.generationCount ? 3 : 1);
            add_external_release(releaseBarriers, vk, this->generate.getOutImages().at(pass),
                VK_ACCESS_2_SHADER_WRITE_BIT);
            if (pass + 1 == vk.generationCount) {
                add_external_release(releaseBarriers, vk, this->inImg_0, VK_ACCESS_2_SHADER_READ_BIT);
                add_external_release(releaseBarriers, vk, this->inImg_1, VK_ACCESS_2_SHADER_READ_BIT);
            }
            emit_external_barriers(buf2, releaseBarriers);
        }
#endif

        buf2.end();
        std::vector<Core::Semaphore> signals = { outSemaphore };
        if (inSem < 0) signals.clear();
        buf2.submit(vk.device.getComputeQueue(), completionFence,
            { internalSemaphore }, std::nullopt,
            signals, std::nullopt);
    }

    this->frameIdx++;
}

// TOTK addition: one admitted pair is self-contained, including after cuts or
// budget/cooldown skips. Existing present() and all shader dispatches are unchanged.
void Context::recordPair(Vulkan& vk, const Core::CommandBuffer& buf,
        VkQueryPool timestamps, float timestamp) {
    // Validate before any command, tracked-layout or diagnostic state change.
    if (!std::isfinite(timestamp) || timestamp < 0.0f || timestamp > 1.0f)
        throw std::runtime_error("LSFG timestamp must be finite in [0,1]");
    if (probe) probe->recorded = false;
    recordPairCommands(vk,buf,timestamps,false,timestamp);
}

void Context::recordPairCommands(Vulkan& vk, const Core::CommandBuffer& buf,
        VkQueryPool timestamps, bool capture, float timestamp) {
    uint32_t marker=0;
    auto stamp=[&] {
        if (timestamps) vkCmdWriteTimestamp2(buf.handle(),VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            timestamps,marker++);
    };
    stamp();
    // Default-on constant-buffer refresh (TOTK_LSFG_UBO_REFRESH=0 opts out): GPU
    // re-write of the pooled and pass-owned phase creation bytes, inside the
    // first timed pass so its cost is part of the LSFG GPU time.
    if (Core::uboRefreshEnabled()) {
        vk.resources.recordRefresh(buf);
        generate.recordRefresh(buf);
    }
#ifdef __ANDROID__
    auto external = [&](bool acquire) {
        std::array<Core::Image*, 3> images = transport ?
            std::array<Core::Image*, 3>{&transport->at(0), &transport->at(1), &transport->at(2)} :
            std::array<Core::Image*, 3>{&inImg_0, &inImg_1, &generate.getOutImages().at(0)};
        std::array<VkImageMemoryBarrier2, 3> barriers{};
        for (size_t i = 0; i < images.size(); ++i) {
            VkAccessFlags2 access = transport ?
                (i == 2 ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT) :
                (i == 2 ? VK_ACCESS_2_SHADER_WRITE_BIT : VK_ACCESS_2_SHADER_READ_BIT);
            barriers[i] = VkImageMemoryBarrier2{
                .sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask=acquire ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask=acquire ? 0 : access,
                .dstStageMask=acquire ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
                .dstAccessMask=acquire ? access : 0,
                .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex=acquire ? VK_QUEUE_FAMILY_EXTERNAL : vk.device.getComputeFamilyIdx(),
                .dstQueueFamilyIndex=acquire ? vk.device.getComputeFamilyIdx() : VK_QUEUE_FAMILY_EXTERNAL,
                .image=images[i]->handle(),
                .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        }
        VkDependencyInfo info{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount=3,.pImageMemoryBarriers=barriers.data()};
        vkCmdPipelineBarrier2(buf.handle(), &info);
    };
    if (!direct) external(true);
#endif
    // Both transport copies are inside the caller's LSFG timestamp bracket.
    auto barrier = [&](Core::Image& image, VkAccessFlags2 destination) {
        VkImageMemoryBarrier2 b{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask=image.getLayout() == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.dstAccessMask=destination,
            .oldLayout=image.getLayout(),.newLayout=VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=image.handle(),.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        VkDependencyInfo d{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};
        vkCmdPipelineBarrier2(buf.handle(),&d); image.setLayout(VK_IMAGE_LAYOUT_GENERAL);
    };
    auto copy = [&](Core::Image& source, Core::Image& destination) {
        barrier(source,VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(destination,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkExtent2D extent=source.getExtent();
        VkImageCopy region{.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
            .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={extent.width,extent.height,1}};
        vkCmdCopyImage(buf.handle(),source.handle(),VK_IMAGE_LAYOUT_GENERAL,
            destination.handle(),VK_IMAGE_LAYOUT_GENERAL,1,&region);
    };
    if (transport) {
        copy(transport->at(0),inImg_0); barrier(inImg_0,VK_ACCESS_2_SHADER_READ_BIT);
        copy(transport->at(1),inImg_1); barrier(inImg_1,VK_ACCESS_2_SHADER_READ_BIT);
    }
    if (direct) {
        barrier(inImg_0,VK_ACCESS_2_SHADER_READ_BIT);
        barrier(inImg_1,VK_ACCESS_2_SHADER_READ_BIT);
    }
    for (uint64_t frame = 0; frame < 2; ++frame) {
        mipmaps.Dispatch(buf, frame); stamp();
        if (capture) probeSnapshot(buf,static_cast<unsigned>(frame));
        for (size_t i = 0; i < 7; ++i) { alpha.at(6 - i).Dispatch(buf, frame); stamp(); }
    }
    // Pair contexts bind beta's missing pre-previous history to the older
    // frame. Do not run beta until both actual feature histories are ready.
    beta.Dispatch(buf,1); stamp();
    for (size_t i = 0; i < 7; ++i) {
        gamma.at(i).Dispatch(buf, 1, 0); stamp();
        if (i >= 4) { delta.at(i - 4).Dispatch(buf, 1, 0); stamp(); }
    }
    generate.DispatchTimestamp(buf, 1, 0, timestamp); stamp();
    if (transport) copy(generate.getOutImages().at(0),transport->at(2));
#ifdef __ANDROID__
    if (!direct) external(false);
#endif
    if (direct) {
        // The renderer retains these sampled scenes and their tracked layout.
        std::array<VkImageMemoryBarrier2,2> barriers{};
        Core::Image* inputs[]={&inImg_0,&inImg_1};
        for (unsigned i=0;i<2;i++) {
            barriers[i]={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,.srcAccessMask=VK_ACCESS_2_SHADER_READ_BIT,
                .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT,
                .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=inputs[i]->handle(),.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            inputs[i]->setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        VkDependencyInfo info{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount=2,.pImageMemoryBarriers=barriers.data()};
        vkCmdPipelineBarrier2(buf.handle(),&info);
        barrier(generate.getOutImages().at(0),VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    if (capture) {
        const VkBufferMemoryBarrier2 barrier{.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
            .srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .buffer=probe->buffer,.offset=0,.size=VK_WHOLE_SIZE};
        const VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&barrier};
        vkCmdPipelineBarrier2(buf.handle(),&dependency);
    }
    stamp();
}

void Context::rebindInputs(Vulkan& vk, VkImage previous, VkImageView previousView,
        VkImage current, VkImageView currentView) {
    if (probe) probe->recorded = false;
    inImg_0.rebind(previous,previousView); inImg_1.rebind(current,currentView);
    mipmaps.rebindInputs(vk.device); generate.rebindInputs(vk.device);
}

Context::Context(Vulkan& vk, std::array<Core::Image,3> shared,
        VkExtent2D extent,VkFormat format,bool direct) : direct(direct) {
    std::vector<Core::Image> outputs;
    if (direct) {
        inImg_0=std::move(shared[0]); inImg_1=std::move(shared[1]);
        outputs.push_back(std::move(shared[2]));
    } else {
        transport=std::move(shared);
        constexpr VkImageUsageFlags inputUsage=VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        inImg_0=Core::Image(vk.device,extent,format,inputUsage,VK_IMAGE_ASPECT_COLOR_BIT);
        inImg_1=Core::Image(vk.device,extent,format,inputUsage,VK_IMAGE_ASPECT_COLOR_BIT);
        outputs.emplace_back(vk.device,extent,format,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    mipmaps=Shaders::Mipmaps(vk,inImg_0,inImg_1);
    for (size_t i=0;i<7;i++) alpha.at(i)=Shaders::Alpha(vk,mipmaps.getOutImages().at(i));
    auto pairHistory=alpha.at(0).getOutImages();
    // Beta consumes THREE temporal feature sets. An admitted pair only has
    // two real frames, so its pre-previous feature set is the older frame,
    // not the never-written third alpha slot or another pair's stale data.
    pairHistory.at(2)=pairHistory.at(0);
    beta=Shaders::Beta(vk,std::move(pairHistory));
    for (size_t i=0;i<7;i++) {
        gamma.at(i)=Shaders::Gamma(vk,alpha.at(6-i).getOutImages(),
            beta.getOutImages().at(std::min<size_t>(6-i,5)),
            i==0 ? std::nullopt : std::make_optional(gamma.at(i-1).getOutImage()));
        if (i<4) continue;
        delta.at(i-4)=Shaders::Delta(vk,alpha.at(6-i).getOutImages(),
            beta.getOutImages().at(6-i),
            i==4 ? std::nullopt : std::make_optional(gamma.at(i-1).getOutImage()),
            i==4 ? std::nullopt : std::make_optional(delta.at(i-5).getOutImage1()),
            i==4 ? std::nullopt : std::make_optional(delta.at(i-5).getOutImage2()));
    }
    generate=Shaders::Generate(vk,inImg_0,inImg_1,gamma.at(6).getOutImage(),
        delta.at(2).getOutImage1(),delta.at(2).getOutImage2(),std::move(outputs));
}

#ifdef __ANDROID__

#include <android/hardware_buffer.h>

Context::Context(Vulkan& vk, AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN, VkExtent2D extent,VkFormat format)
    : Context(vk,std::array<Core::Image,3>{
        Core::Image(vk.device,extent,format,VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT,in0),
        Core::Image(vk.device,extent,format,VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT,in1),
        Core::Image(vk.device,extent,format,VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_IMAGE_ASPECT_COLOR_BIT,outN.at(0))
    },extent,format) {}

#endif // __ANDROID__
