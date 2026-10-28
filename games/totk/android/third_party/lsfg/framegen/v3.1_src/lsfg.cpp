#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "lsfg_3_1.hpp"
#include "v3_1/context.hpp"
#include "core/commandpool.hpp"
#include "core/descriptorpool.hpp"
#include "core/instance.hpp"
#include "pool/shaderpool.hpp"
#include "common/exception.hpp"
#include "common/utils.hpp"

#include <cstdint>
#include <optional>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace LSFG;
using namespace LSFG_3_1;

namespace {
    std::optional<Core::Instance> instance;
    std::optional<Vulkan> device;
    std::unordered_map<int32_t, Context> contexts;
}

void LSFG_3_1::initialize(uint64_t deviceUUID,
        bool isHdr, float flowScale, uint64_t generationCount,
        const std::function<std::vector<uint8_t>(const std::string&)>& loader) {
    if (instance.has_value() || device.has_value())
        return;

    instance.emplace();
    device.emplace(Vulkan {
        .device{*instance, deviceUUID},
        .generationCount = generationCount,
        .flowScale = flowScale,
        .isHdr = isHdr
    });
    contexts = std::unordered_map<int32_t, Context>();

    device->commandPool = Core::CommandPool(device->device);
    device->descriptorPool = Core::DescriptorPool(device->device);

    device->resources = Pool::ResourcePool(device->isHdr, device->flowScale);
    device->shaders = Pool::ShaderPool(loader);

    std::srand(static_cast<uint32_t>(std::time(nullptr)));
}

int32_t LSFG_3_1::createContext(
        int in0, int in1, const std::vector<int>& outN,
        VkExtent2D extent, VkFormat format) {
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");

    const int32_t id = std::rand();
    contexts.emplace(id, Context(*device, in0, in1, outN, extent, format));
    return id;
}

void LSFG_3_1::presentContext(int32_t id, int inSem, const std::vector<int>& outSem) {
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");

    auto it = contexts.find(id);
    if (it == contexts.end())
        throw LSFG::vulkan_error(VK_ERROR_UNKNOWN, "Context not found");

    it->second.present(*device, inSem, outSem);
}

void LSFG_3_1::deleteContext(int32_t id) {
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");

    auto it = contexts.find(id);
    if (it == contexts.end())
        throw LSFG::vulkan_error(VK_ERROR_DEVICE_LOST, "No such context");

    vkDeviceWaitIdle(device->device.handle());
    contexts.erase(it);
}

void LSFG_3_1::finalize() {
    if (!instance.has_value() || !device.has_value())
        return;

    vkDeviceWaitIdle(device->device.handle());
    contexts.clear();
    device.reset();
    instance.reset();
}

#ifdef __ANDROID__

#include <android/hardware_buffer.h>
#include "../../../transport.h"

bool LSFG_3_1::selectTransport(VkFormat format,VkExtent2D extent,uint64_t androidUsage[2]) {
    if (!device) throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,"LSFG not initialized");
    VkPhysicalDevice pd=device->device.getPhysicalDevice();
    if (!totk_lsfg_probe_ahb(vkGetPhysicalDeviceImageFormatProperties2,pd,format,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,extent.width,extent.height,&androidUsage[0]) ||
        !totk_lsfg_probe_ahb(vkGetPhysicalDeviceImageFormatProperties2,pd,format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,extent.width,extent.height,&androidUsage[1])) return false;
    VkPhysicalDeviceImageFormatInfo2 info{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format=format,.type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImageFormatProperties2 props{.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(pd,&info,&props)!=VK_SUCCESS ||
        props.imageFormatProperties.maxExtent.width<extent.width ||
        props.imageFormatProperties.maxExtent.height<extent.height) return false;
    VkFormatProperties features;
    vkGetPhysicalDeviceFormatProperties(pd,format,&features);
    constexpr VkFormatFeatureFlags need=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((features.optimalTilingFeatures & need)!=need) return false;
    bool hdr=format==VK_FORMAT_R16G16B16A16_SFLOAT;
    if (device->isHdr!=hdr) {
        if (!contexts.empty()) throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,"LSFG transport changed with live contexts");
        device->isHdr=hdr;
        device->resources=Pool::ResourcePool(hdr,device->flowScale);
    }
    return true;
}

int32_t LSFG_3_1::createContextFromAHB(
        AHardwareBuffer* in0, AHardwareBuffer* in1,
        const std::vector<AHardwareBuffer*>& outN,
        VkExtent2D extent, VkFormat format) {
    if (!instance.has_value() || !device.has_value())
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "LSFG not initialized");

    const int32_t id = std::rand();
    contexts.emplace(id, Context(*device, in0, in1, outN, extent, format));
    return id;
}

#endif // __ANDROID__

#ifdef __ANDROID__
void LSFG_3_1::waitIdle() {
    if (!device.has_value()) return;
    vkDeviceWaitIdle(device->device.handle());
}
#endif

// TOTK additions. The host's covering continuation ticket protects these
// objects, so none of the interop operations waits for the GPU on the CPU.
#include <memory>
#include <cmath>
#include <stdexcept>
#include <unistd.h>
namespace {
    struct InteropRun {
        VkDevice dev{};
        Core::CommandBuffer commands;
        VkSemaphore input{}, output{};
        VkQueryPool query{};
        uint64_t mask{};
        double period{};
        bool recorded{};
        ~InteropRun() {
            if (query) vkDestroyQueryPool(dev, query, nullptr);
            if (input) vkDestroySemaphore(dev, input, nullptr);
            if (output) vkDestroySemaphore(dev, output, nullptr);
        }
    };
    std::unordered_map<int32_t, std::unique_ptr<InteropRun>> interopRuns;
    InteropRun& interopRun(int32_t id) {
        auto& entry = interopRuns[id];
        if (entry) return *entry;
        auto run = std::make_unique<InteropRun>();
        run->dev = device->device.handle();
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device->device.getPhysicalDevice(), &props);
        uint32_t count{};
        vkGetPhysicalDeviceQueueFamilyProperties(device->device.getPhysicalDevice(), &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device->device.getPhysicalDevice(), &count, queues.data());
        uint32_t bits = queues.at(device->device.getComputeFamilyIdx()).timestampValidBits;
        if (!bits || !std::isfinite(props.limits.timestampPeriod) || props.limits.timestampPeriod <= 0)
            throw std::runtime_error("LSFG compute GPU timestamps unavailable");
        run->mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
        run->period = props.limits.timestampPeriod;
        VkQueryPoolCreateInfo query{.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        auto result = vkCreateQueryPool(run->dev, &query, nullptr, &run->query);
        if (result != VK_SUCCESS) throw vulkan_error(result, "LSFG timestamp pool");
        VkSemaphoreCreateInfo sem{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        result = vkCreateSemaphore(run->dev, &sem, nullptr, &run->input);
        if (result != VK_SUCCESS) throw vulkan_error(result, "LSFG input semaphore");
        VkExportSemaphoreCreateInfo exportInfo{.sType=VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        sem.pNext = &exportInfo;
        result = vkCreateSemaphore(run->dev, &sem, nullptr, &run->output);
        if (result != VK_SUCCESS) throw vulkan_error(result, "LSFG output semaphore");
        run->commands = Core::CommandBuffer(device->device, device->commandPool);
        entry = std::move(run);
        return *entry;
    }
}
void LSFG_3_1::useVulkanLoader(PFN_vkGetInstanceProcAddr loader) {
    if (instance || device || !loader) throw std::runtime_error("LSFG loader must be selected before initialize");
    volkInitializeCustom(loader);
}
int LSFG_3_1::presentPairContext(int32_t id, int inputSyncFd) {
    // This function takes ownership of inputSyncFd on every outcome.
    struct Fd {
        int value;
        ~Fd() { if (value >= 0) close(value); }
    } inputFd{inputSyncFd};
    if (!device || !contexts.contains(id)) throw std::runtime_error("LSFG pair context unavailable");
    auto& run = interopRun(id);
    VkImportSemaphoreFdInfoKHR imp{.sType=VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore=run.input,.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,.fd=inputFd.value};
    auto result = vkImportSemaphoreFdKHR(run.dev, &imp);
    if (result != VK_SUCCESS) throw vulkan_error(result, "LSFG input SYNC_FD import");
    inputFd.value = -1;
    if (run.recorded) run.commands.reset();
    run.commands.begin();
    vkCmdResetQueryPool(run.commands.handle(), run.query, 0, 2);
    vkCmdWriteTimestamp2(run.commands.handle(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, run.query, 0);
    contexts.at(id).recordPair(*device, run.commands);
    vkCmdWriteTimestamp2(run.commands.handle(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, run.query, 1);
    run.commands.end();
    VkCommandBuffer command = run.commands.handle();
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,
        .pWaitSemaphores=&run.input,.pWaitDstStageMask=&stage,.commandBufferCount=1,
        .pCommandBuffers=&command,.signalSemaphoreCount=1,.pSignalSemaphores=&run.output};
    result = vkQueueSubmit(device->device.getComputeQueue(), 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) throw vulkan_error(result, "LSFG pair submission");
    run.recorded = true;
    VkSemaphoreGetFdInfoKHR out{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore=run.output,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -1;
    result = vkGetSemaphoreFdKHR(run.dev, &out, &fd);
    if (result != VK_SUCCESS) throw vulkan_error(result, "LSFG output SYNC_FD export");
    return fd;
}
bool LSFG_3_1::pairTimestamp(int32_t id, double& milliseconds) {
    auto it = interopRuns.find(id);
    if (it == interopRuns.end() || !it->second->recorded) return false;
    auto& run = *it->second;
    uint64_t time[2];
    if (vkGetQueryPoolResults(run.dev, run.query, 0, 2, sizeof time, time, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return false;
    milliseconds = static_cast<double>((time[1] - time[0]) & run.mask) * run.period / 1e6;
    return std::isfinite(milliseconds) && milliseconds > 0;
}
void LSFG_3_1::deleteRetiredContext(int32_t id) {
    interopRuns.erase(id);
    contexts.erase(id);
}
