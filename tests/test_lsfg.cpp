/* MIT. Optional real-DLL test: argv[1] is read in place, never copied or cached.
 * Host-native images replace Android AHB transport, not shader execution. */
#include <volk.h>
#include "api.h"
#include "shaders.hpp"
#include "v3_1/context.hpp"
#include "core/instance.hpp"
#include "core/buffer.hpp"
#include "common/utils.hpp"
#include "pool/resourcepool.hpp"
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <filesystem>
#include <sstream>
#include <map>
#include <type_traits>

void lsfg_host_test_shutdown();
namespace LSFG_3_1 {
struct ContextTest {
    static LSFG::Core::Image& input(Context& c,unsigned i) {
        return c.transport ? c.transport->at(i) : i ? c.inImg_1 : c.inImg_0;
    }
    static LSFG::Core::Image output(Context& c) {
        return c.transport ? c.transport->at(2) : c.generate.getOutImages().at(0);
    }
    static std::vector<Core::Image> scratch(Context& c) {
        std::vector<Core::Image> images;
        for (auto image:c.mipmaps.getOutImages()) images.push_back(image);
        for (auto& alpha:c.alpha)
            for (auto& history:alpha.getOutImages())
                for (auto image:history) images.push_back(image);
        for (auto image:c.beta.getOutImages()) images.push_back(image);
        for (auto& gamma:c.gamma) images.push_back(gamma.getOutImage());
        for (auto& delta:c.delta) {
            images.push_back(delta.getOutImage1()); images.push_back(delta.getOutImage2());
        }
        for (auto image:c.generate.getOutImages()) images.push_back(image);
        return images;
    }
    static const auto& mipmaps(Context& c) { return c.mipmaps.getOutImages(); }
    static void mipmaps(Context& c,const Core::CommandBuffer& cb,unsigned frame) {
        c.mipmaps.Dispatch(cb,frame);
    }
};
}
using namespace LSFG;
static void check(VkResult r,const char *what) {
    if (r!=VK_SUCCESS) throw std::runtime_error(std::string(what)+": "+std::to_string(r));
}
static void barrier(VkCommandBuffer cb,Core::Image& image,VkAccessFlags2 source,VkAccessFlags2 dest) {
    VkImageMemoryBarrier2 b{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=source,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.dstAccessMask=dest,
        .oldLayout=image.getLayout(),.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=image.handle(),.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkDependencyInfo d{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};
    vkCmdPipelineBarrier2(cb,&d); image.setLayout(VK_IMAGE_LAYOUT_GENERAL);
}
static void completed(const Core::Fence& fence,const Core::Device& device,const char* phase) {
    // Allow native-size software-driver work under host CPU contention. This
    // host regression timeout is unrelated to the game's real-frame deadline.
    if (fence.wait(device,UINT64_C(120000000000))) return;
    std::fprintf(stderr,"FAIL LSFG: %s timed out; exiting without destroying in-flight resources\n",phase);
    std::fflush(nullptr); std::_Exit(1);
}
static std::vector<uint8_t> readback(Vulkan& vk,Core::Image output,unsigned bytes) {
    auto extent=output.getExtent();
    std::vector<uint8_t> pixels(size_t(extent.width)*extent.height*bytes);
    VkBuffer buffer{}; VkDeviceMemory memory{};
    VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=pixels.size(),.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    check(vkCreateBuffer(vk.device.handle(),&bi,nullptr,&buffer),"readback buffer");
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(vk.device.handle(),buffer,&req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk.device.getPhysicalDevice(),&mp);
    unsigned type=0;
    for (;type<mp.memoryTypeCount;type++)
        if ((req.memoryTypeBits&(1u<<type)) && (mp.memoryTypes[type].propertyFlags&
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    if (type==mp.memoryTypeCount) throw std::runtime_error("coherent readback memory unavailable");
    VkMemoryAllocateInfo ai{.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    check(vkAllocateMemory(vk.device.handle(),&ai,nullptr,&memory),"readback allocation");
    check(vkBindBufferMemory(vk.device.handle(),buffer,memory,0),"readback binding");
    Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
    barrier(cb.handle(),output,VK_ACCESS_2_MEMORY_WRITE_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
        .imageExtent={extent.width,extent.height,1}};
    vkCmdCopyImageToBuffer(cb.handle(),output.handle(),VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
    cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
    completed(fence,vk.device,"readback");
    void *mapped=nullptr; check(vkMapMemory(vk.device.handle(),memory,0,VK_WHOLE_SIZE,0,&mapped),"readback mapping");
    std::memcpy(pixels.data(),mapped,pixels.size());
    vkUnmapMemory(vk.device.handle(),memory); vkDestroyBuffer(vk.device.handle(),buffer,nullptr);
    vkFreeMemory(vk.device.handle(),memory,nullptr);
    return pixels;
}
static void barrier_masks(Vulkan& vk,const std::array<Core::Image,3>& images) {
    Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
    Core::Image read(images[0].handle(),images[0].getView(),images[0].getExtent(),
        images[0].getFormat(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    Core::Image write(images[1].handle(),images[1].getView(),images[1].getExtent(),
        images[1].getFormat(),VK_IMAGE_LAYOUT_UNDEFINED);
    static std::array<VkImageMemoryBarrier2,2> captured{};
    static bool image_only=false;
    auto original=vkCmdPipelineBarrier2;
    vkCmdPipelineBarrier2=[](VkCommandBuffer,const VkDependencyInfo *info) {
        image_only=info->sType==VK_STRUCTURE_TYPE_DEPENDENCY_INFO && !info->pNext &&
            !info->dependencyFlags && !info->memoryBarrierCount && !info->bufferMemoryBarrierCount &&
            info->imageMemoryBarrierCount==captured.size();
        if (image_only) for (size_t i=0;i<captured.size();i++)
            captured[i]=info->pImageMemoryBarriers[i];
    };
    try {
        Utils::BarrierBuilder(cb).addR2W(read).addW2R(write).build();
    } catch (...) {
        vkCmdPipelineBarrier2=original;
        throw;
    }
    vkCmdPipelineBarrier2=original;
    cb.end();
    const char *value=std::getenv("TOTK_LSFG_WIDE_BARRIERS");
    const bool wide=value && !std::strcmp(value,"1");
    const VkPipelineStageFlags2 stage=wide ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT :
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 read_access=wide ? VK_ACCESS_2_MEMORY_READ_BIT : VK_ACCESS_2_SHADER_READ_BIT;
    const VkAccessFlags2 write_access=wide ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_SHADER_WRITE_BIT;
    if (!image_only || read.getLayout()!=VK_IMAGE_LAYOUT_GENERAL ||
        write.getLayout()!=VK_IMAGE_LAYOUT_GENERAL)
        throw std::runtime_error("BarrierBuilder dependency/layout assertion failed");
    for (size_t i=0;i<captured.size();i++) {
        const auto& b=captured[i];
        if (b.sType!=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 || b.pNext ||
            b.srcStageMask!=stage || b.dstStageMask!=stage ||
            b.srcAccessMask!=(i ? write_access : read_access) ||
            b.dstAccessMask!=(i ? read_access : write_access) ||
            b.oldLayout!=(i ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
            b.newLayout!=VK_IMAGE_LAYOUT_GENERAL ||
            b.srcQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED || b.dstQueueFamilyIndex!=VK_QUEUE_FAMILY_IGNORED ||
            b.image!=images[i].handle() || b.subresourceRange.aspectMask!=images[i].getAspectFlags() ||
            b.subresourceRange.baseMipLevel || b.subresourceRange.levelCount!=1 ||
            b.subresourceRange.baseArrayLayer || b.subresourceRange.layerCount!=1)
            throw std::runtime_error("BarrierBuilder mask/image/subresource assertion failed");
    }
    std::printf("PASS BarrierBuilder exact %s R2W/W2R masks and preserved image transitions\n",
        wide ? "wide" : "default");
}
// The real ABI test runs only on request, uses the user's DLL in place and
// removes its private pixel-only temporary directory on either success/failure.
struct ProbeDirectory {
    std::filesystem::path path;
    ProbeDirectory() {
        const char *tmp=std::getenv("TMPDIR");
        std::string pattern=((tmp && *tmp ? std::filesystem::path(tmp) :
            std::filesystem::current_path())/"totk-lsfg-probe-XXXXXX").string();
        std::vector<char> name(pattern.begin(),pattern.end()); name.push_back('\0');
        const char *created=mkdtemp(name.data());
        if (!created) throw std::runtime_error("probe temporary directory failed");
        path=created;
    }
    ~ProbeDirectory() { std::error_code ignored; std::filesystem::remove_all(path,ignored); }
};
struct ProbeABI {
    ~ProbeABI() { lsfg_host_test_shutdown(); }
};
struct ProbeEnvironment {
    bool present=std::getenv("TOTK_LSFG_PROBE")!=nullptr;
    std::string value=present ? std::getenv("TOTK_LSFG_PROBE") : "";
    ~ProbeEnvironment() {
        if (present) setenv("TOTK_LSFG_PROBE",value.c_str(),1);
        else unsetenv("TOTK_LSFG_PROBE");
    }
};
struct ProbeTrace {
    inline static ProbeTrace *active{};
    PFN_vkCmdDispatch dispatch=vkCmdDispatch;
    PFN_vkCmdCopyImageToBuffer copy=vkCmdCopyImageToBuffer;
    PFN_vkCmdWriteTimestamp2 timestamp=vkCmdWriteTimestamp2;
    PFN_vkCreateBuffer create=vkCreateBuffer;
    PFN_vkDestroyBuffer destroy=vkDestroyBuffer;
    PFN_vkAllocateMemory allocate=vkAllocateMemory;
    PFN_vkFreeMemory free=vkFreeMemory;
    PFN_vkGetPhysicalDeviceMemoryProperties properties=vkGetPhysicalDeviceMemoryProperties;
    PFN_vkInvalidateMappedMemoryRanges invalidate=vkInvalidateMappedMemoryRanges;
    PFN_vkQueueSubmit submit=vkQueueSubmit;
    PFN_vkWaitForFences wait=vkWaitForFences;
    PFN_vkCreateImage createImage=vkCreateImage;
    PFN_vkCmdUpdateBuffer updateBuffer=vkCmdUpdateBuffer;
    PFN_vkAllocateDescriptorSets allocateSets=vkAllocateDescriptorSets;
    PFN_vkUpdateDescriptorSets updateSets=vkUpdateDescriptorSets;
    unsigned phaseUpdates{},refreshUpdates{},descriptorAllocations{},descriptorUpdates{};
    unsigned uniformWrites{},wrongUniformRanges{};
    float phase{};
    unsigned dispatches{},copies{},stamps{},creates{},allocations{},submits{},waits{},invalidations{};
    // Aggregate timestamp stages are28; the actual translated pair executes129 dispatches.
    static constexpr unsigned pairDispatches=129;
    unsigned transferImages{},wrongTransferImages{},destroyed{},freed{};
    VkBuffer buffer{},watchedBuffer{};
    VkDeviceMemory memory{},watchedMemory{};
    bool bad{},failAllocation{},noncoherent{};
    struct Copy { unsigned dispatch; VkImage image; VkBufferImageCopy region; };
    std::array<Copy,14> regions{};
    ProbeTrace() {
        active=this;
        vkCmdDispatch=[](VkCommandBuffer cb,uint32_t x,uint32_t y,uint32_t z) {
            ++active->dispatches; active->dispatch(cb,x,y,z);
        };
        vkCmdCopyImageToBuffer=[](VkCommandBuffer cb,VkImage image,VkImageLayout layout,
                VkBuffer buffer,uint32_t count,const VkBufferImageCopy *regions) {
            auto& t=*active;
            if (count!=1 || t.copies>=t.regions.size() || layout!=VK_IMAGE_LAYOUT_GENERAL ||
                (t.buffer && t.buffer!=buffer)) t.bad=true;
            if (count==1 && t.copies<t.regions.size())
                t.regions[t.copies]={t.dispatches,image,regions[0]};
            t.buffer=buffer; ++t.copies; t.copy(cb,image,layout,buffer,count,regions);
        };
        vkCmdWriteTimestamp2=[](VkCommandBuffer cb,VkPipelineStageFlags2 stage,VkQueryPool query,uint32_t index) {
            if (index!=active->stamps || stage!=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) active->bad=true;
            ++active->stamps; active->timestamp(cb,stage,query,index);
        };
        vkCreateBuffer=[](VkDevice device,const VkBufferCreateInfo *info,const VkAllocationCallbacks *callbacks,VkBuffer *buffer) {
            ++active->creates; return active->create(device,info,callbacks,buffer);
        };
        vkDestroyBuffer=[](VkDevice device,VkBuffer buffer,const VkAllocationCallbacks *callbacks) {
            if (!active->watchedBuffer || buffer==active->watchedBuffer) ++active->destroyed;
            active->destroy(device,buffer,callbacks);
        };
        vkAllocateMemory=[](VkDevice device,const VkMemoryAllocateInfo *info,const VkAllocationCallbacks *callbacks,VkDeviceMemory *memory) {
            ++active->allocations;
            if (active->failAllocation) return VK_ERROR_OUT_OF_HOST_MEMORY;
            auto result=active->allocate(device,info,callbacks,memory);
            if (result==VK_SUCCESS) active->memory=*memory;
            return result;
        };
        vkFreeMemory=[](VkDevice device,VkDeviceMemory memory,const VkAllocationCallbacks *callbacks) {
            if (!active->watchedMemory || memory==active->watchedMemory) ++active->freed;
            active->free(device,memory,callbacks);
        };
        vkGetPhysicalDeviceMemoryProperties=[](VkPhysicalDevice device,VkPhysicalDeviceMemoryProperties *properties) {
            active->properties(device,properties);
            if (active->noncoherent) for (unsigned i=0;i<properties->memoryTypeCount;++i)
                properties->memoryTypes[i].propertyFlags&=~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        };
        vkInvalidateMappedMemoryRanges=[](VkDevice device,uint32_t count,const VkMappedMemoryRange *ranges) {
            ++active->invalidations;
            if (count!=1 || ranges[0].offset || ranges[0].size!=VK_WHOLE_SIZE) active->bad=true;
            return active->invalidate(device,count,ranges);
        };
        vkQueueSubmit=[](VkQueue queue,uint32_t count,const VkSubmitInfo *info,VkFence fence) {
            ++active->submits; return active->submit(queue,count,info,fence);
        };
        vkWaitForFences=[](VkDevice device,uint32_t count,const VkFence *fences,VkBool32 all,uint64_t timeout) {
            ++active->waits; return active->wait(device,count,fences,all,timeout);
        };
        vkCreateImage=[](VkDevice device,const VkImageCreateInfo *info,const VkAllocationCallbacks *callbacks,VkImage *image) {
            if (info->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
                ++active->transferImages;
                if (info->format!=VK_FORMAT_R8_UNORM) ++active->wrongTransferImages;
            }
            return active->createImage(device,info,callbacks,image);
        };
        vkCmdUpdateBuffer=[](VkCommandBuffer cb,VkBuffer buffer,VkDeviceSize offset,
                VkDeviceSize size,const void *data) {
            // The default-on refresh re-writes whole 48-byte constant buffers;
            // a fractional phase update writes only the 4-byte timestamp.
            if (offset==0 && size==48 && data) { ++active->refreshUpdates; active->updateBuffer(cb,buffer,offset,size,data); return; }
            ++active->phaseUpdates;
            if (offset!=28 || size!=sizeof(float) || !data) active->bad=true;
            else std::memcpy(&active->phase,data,sizeof(float));
            active->updateBuffer(cb,buffer,offset,size,data);
        };
        vkAllocateDescriptorSets=[](VkDevice device,const VkDescriptorSetAllocateInfo *info,VkDescriptorSet *sets) {
            ++active->descriptorAllocations; return active->allocateSets(device,info,sets);
        };
        vkUpdateDescriptorSets=[](VkDevice device,uint32_t writes,const VkWriteDescriptorSet *write,
                uint32_t copies,const VkCopyDescriptorSet *copy) {
            for (uint32_t i=0;i<writes;i++) if (write[i].descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                for (uint32_t j=0;j<write[i].descriptorCount;j++) {
                    ++active->uniformWrites;
                    if (!write[i].pBufferInfo || write[i].pBufferInfo[j].offset ||
                        write[i].pBufferInfo[j].range!=48) ++active->wrongUniformRanges;
                }
            }
            ++active->descriptorUpdates; active->updateSets(device,writes,write,copies,copy);
        };
    }
    ~ProbeTrace() {
        vkCmdDispatch=dispatch; vkCmdCopyImageToBuffer=copy; vkCmdWriteTimestamp2=timestamp;
        vkCreateBuffer=create; vkDestroyBuffer=destroy; vkAllocateMemory=allocate; vkFreeMemory=free;
        vkGetPhysicalDeviceMemoryProperties=properties; vkInvalidateMappedMemoryRanges=invalidate;
        vkQueueSubmit=submit; vkWaitForFences=wait; vkCreateImage=createImage; active=nullptr;
        vkCmdUpdateBuffer=updateBuffer; vkAllocateDescriptorSets=allocateSets; vkUpdateDescriptorSets=updateSets;
    }
};
static void probe_expect(int result,bool success,const char *error,const char *what) {
    if (bool(result)!=success || (!success && !error[0]))
        throw std::runtime_error(std::string(what)+": "+error);
}
template<typename T> static uint64_t test_handle(T value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(value);
    else return static_cast<uint64_t>(value);
}
static std::map<std::string,std::string> probe_fields(const std::string& line) {
    std::istringstream stream(line);
    std::map<std::string,std::string> fields;
    for (std::string field;stream>>field;) {
        const auto equal=field.find('=');
        if (equal!=std::string::npos) fields.emplace(field.substr(0,equal),field.substr(equal+1));
    }
    return fields;
}
using ProbePixels=std::array<std::array<std::vector<uint8_t>,7>,2>;
static void probe_manifest(const std::filesystem::path& directory,
        const std::array<Core::Image,3>& owners,const ProbePixels& reference,
        const std::array<ProbeTrace::Copy,14>& regions,bool swapped) {
    std::ifstream manifest(directory/"lsfg-probe-manifest.txt");
    if (!manifest) throw std::runtime_error("probe manifest missing");
    unsigned inputs=0,snapshots=0;
    std::array<bool,2> seenInputs{};
    std::array<bool,14> seenSnapshots{};
    for (std::string line;std::getline(manifest,line);) {
        auto fields=probe_fields(line);
        if (line.starts_with("input ")) {
            const auto frame=std::stoul(fields.at("frame"));
            if (frame>=2 || seenInputs[frame]) throw std::runtime_error("probe input identity repeated");
            seenInputs[frame]=true;
            const auto& image=owners[swapped ? 1-frame : frame];
            if (std::stoull(fields.at("image"),nullptr,0)!=test_handle(image.handle()) ||
                std::stoull(fields.at("view"),nullptr,0)!=test_handle(image.getView()) ||
                std::stoul(fields.at("format"))!=unsigned(image.getFormat()) ||
                std::stoul(fields.at("width"))!=image.getExtent().width ||
                std::stoul(fields.at("height"))!=image.getExtent().height)
                throw std::runtime_error("probe metadata is not the actual bound input");
            ++inputs;
        } else if (line.starts_with("snapshot ")) {
            const auto frame=std::stoul(fields.at("frame")),level=std::stoul(fields.at("level"));
            if (frame>=2 || level>=7 || seenSnapshots[frame*7+level])
                throw std::runtime_error("probe snapshot identity repeated");
            seenSnapshots[frame*7+level]=true;
            const auto& copy=regions[frame*7+level];
            const auto& expected=reference[swapped ? 1-frame : frame][level];
            const auto filename="lsfg-mipmaps-frame"+std::to_string(frame)+"-level"+std::to_string(level)+".r8";
            if (fields.at("file")!=filename || fields.at("format")!="VK_FORMAT_R8_UNORM" ||
                std::stoul(fields.at("vk_format"))!=VK_FORMAT_R8_UNORM ||
                std::stoul(fields.at("mipmaps_dispatch"))!=frame+1 ||
                std::stoul(fields.at("chain_dispatch"))!=copy.dispatch ||
                std::stoul(fields.at("width"))!=copy.region.imageExtent.width ||
                std::stoul(fields.at("height"))!=copy.region.imageExtent.height ||
                std::stoull(fields.at("offset"))!=copy.region.bufferOffset ||
                std::stoull(fields.at("bytes"))!=expected.size() ||
                std::stoul(fields.at("row_bytes"))!=copy.region.imageExtent.width ||
                std::stoull(fields.at("image"),nullptr,0)!=test_handle(copy.image))
                throw std::runtime_error("probe snapshot extent/format/offset metadata mismatch");
            std::ifstream pixels(directory/filename,std::ios::binary);
            std::vector<uint8_t> actual(std::istreambuf_iterator<char>(pixels),{});
            if (actual!=expected) throw std::runtime_error("probe pixels differ from independent per-input mipmaps dispatch");
            ++snapshots;
        }
    }
    if (inputs!=2 || snapshots!=14 ||
        std::distance(std::filesystem::directory_iterator(directory),std::filesystem::directory_iterator{})!=15)
        throw std::runtime_error("probe export must contain fourteen raw pixel files and one manifest only");
}
static void probe_tests(VkInstance instance,Vulkan& vk,LSFG_3_1::Context& referenceContext,
        const std::array<Core::Image,3>& owners,const char *dll,bool perf,float flow) {
    const char *opt=std::getenv("TOTK_LSFG_PROBE");
    const bool enabled=opt && !std::strcmp(opt,"1");
    ProbeEnvironment environment;
    ProbeDirectory directory;
    ProbeABI abi;
    char error[256]{};
    auto expect=[&](int result,bool success,const char *what) {
        probe_expect(result,success,error,what); error[0]=0;
    };
    expect(totk_lsfg_initialize(vkGetInstanceProcAddr,instance,vk.device.getPhysicalDevice(),
        vk.device.handle(),vk.device.getComputeQueue(),vk.device.getComputeFamilyIdx(),
        vk.device.supportsNullDescriptor(),dll,perf,flow,error),true,"probe ABI initialization");
    std::array<totk_lsfg_image,3> images;
    for (unsigned i=0;i<3;++i) images[i]={owners[i].handle(),owners[i].getView(),owners[i].getFormat()};
    const auto extent=owners[0].getExtent();
    int32_t context{};
    if (!enabled) {
        {
            ProbeTrace trace;
            expect(totk_lsfg_context(images[0],images[1],images[2],extent.width,extent.height,&context,error),
                true,"strict-off process context");
            if (trace.transferImages) throw std::runtime_error("strict-off process changed image transfer usage");
        }
        {
            ProbeTrace trace;
            expect(totk_lsfg_probe_prepare(context,error),false,"strict-off process prepare");
            expect(totk_lsfg_record_probe(context,VK_NULL_HANDLE,VK_NULL_HANDLE,error),false,"strict-off process record");
            expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"strict-off process write");
            if (trace.creates || trace.allocations || trace.copies || trace.dispatches || trace.submits || trace.waits)
                throw std::runtime_error("strict-off process performed diagnostic work");
        }
        Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
        {
            ProbeTrace trace;
            expect(totk_lsfg_record(context,cb.handle(),.5f,VK_NULL_HANDLE,error),true,"strict-off ordinary recording");
            if (trace.copies || trace.creates || trace.allocations || trace.submits || trace.waits ||
                trace.dispatches!=ProbeTrace::pairDispatches)
                throw std::runtime_error("strict-off process ordinary recording captured a probe");
        }
        cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
        completed(fence,vk.device,"strict-off ordinary pair");
        totk_lsfg_retire(context);
        if (!std::filesystem::is_empty(directory.path)) throw std::runtime_error("strict-off process wrote files");
        std::puts("PASS real probe C ABI strict-off process: no transfer usage, capture, allocation, submit, wait or export");
        return;
    }
    unsetenv("TOTK_LSFG_PROBE");
    {
        ProbeTrace trace;
        expect(totk_lsfg_context(images[0],images[1],images[2],extent.width,extent.height,&context,error),
            true,"strict-off context");
        if (trace.transferImages) throw std::runtime_error("probe opt-out changed image transfer usage");
    }
    for (const char *value : std::array<const char*,9>{nullptr,"","0","01","true","1 "," 1","1\n","2"}) {
        if (value) setenv("TOTK_LSFG_PROBE",value,1); else unsetenv("TOTK_LSFG_PROBE");
        ProbeTrace trace;
        expect(totk_lsfg_probe_prepare(context,error),false,"strict opt-out prepare");
        expect(totk_lsfg_record_probe(context,VK_NULL_HANDLE,VK_NULL_HANDLE,error),false,"strict opt-out record");
        expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"strict opt-out write");
        if (trace.creates || trace.allocations || trace.copies || trace.dispatches || trace.submits || trace.waits)
            throw std::runtime_error("probe opt-out performed work");
    }
    setenv("TOTK_LSFG_PROBE","1",1);
    expect(totk_lsfg_probe_prepare(context,error),false,"late opt-in requires transfer-capable context");
    totk_lsfg_retire(context);
    {
        ProbeTrace trace;
        expect(totk_lsfg_context(images[0],images[1],images[2],extent.width,extent.height,&context,error),
            true,"probe context");
        if (trace.transferImages!=7 || trace.wrongTransferImages)
            throw std::runtime_error("probe transfer usage was not confined to seven R8 mipmaps outputs");
    }
    expect(totk_lsfg_record_probe(context,VK_NULL_HANDLE,VK_NULL_HANDLE,error),false,"unprepared probe record");
    expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"unprepared probe write");
    {
        ProbeTrace trace; trace.failAllocation=true;
        expect(totk_lsfg_probe_prepare(context,error),false,"injected readback allocation failure");
        if (trace.creates!=1 || trace.allocations!=1 || trace.destroyed!=1 || trace.submits || trace.waits)
            throw std::runtime_error("failed probe allocation leaked or submitted work");
    }
    VkDeviceMemory readbackMemory{};
    {
        // Exercise the HOST_VISIBLE/noncoherent path even on coherent-only host
        // ICDs: invalidating coherent memory is legal, but must still be called.
        ProbeTrace trace; trace.noncoherent=true;
        expect(totk_lsfg_probe_prepare(context,error),true,"probe preparation");
        readbackMemory=trace.memory;
        if (trace.creates!=1 || trace.allocations!=1 || trace.copies || trace.submits || trace.waits)
            throw std::runtime_error("probe prepare did unexpected GPU work");
    }
    expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"prepared but unrecorded probe");
    ProbePixels reference;
    for (unsigned frame=0;frame<2;++frame) {
        auto& input=LSFG_3_1::ContextTest::input(referenceContext,frame);
        Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
        barrier(cb.handle(),input,VK_ACCESS_2_MEMORY_READ_BIT,VK_ACCESS_2_SHADER_READ_BIT);
        LSFG_3_1::ContextTest::mipmaps(referenceContext,cb,frame);
        VkImageMemoryBarrier2 restore{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,.srcAccessMask=VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT,
            .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=input.handle(),.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        VkDependencyInfo dependency{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount=1,.pImageMemoryBarriers=&restore};
        vkCmdPipelineBarrier2(cb.handle(),&dependency); input.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
        completed(fence,vk.device,"independent probe mipmaps reference");
        for (unsigned level=0;level<7;++level)
            reference[frame][level]=readback(vk,LSFG_3_1::ContextTest::mipmaps(referenceContext)[level],1);
    }
    bool distinct=false;
    for (unsigned level=0;level<7;++level) distinct|=reference[0][level]!=reference[1][level];
    if (!distinct) throw std::runtime_error("probe fixture did not distinguish the two input dispatches");
    VkQueryPool query{};
    VkQueryPoolCreateInfo queryInfo{.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=TOTK_LSFG_TIMESTAMPS};
    check(vkCreateQueryPool(vk.device.handle(),&queryInfo,nullptr,&query),"probe timestamps");
    VkBuffer readbackBuffer{};
    Core::Image outputImage(images[2].image,images[2].view,extent,images[2].format,VK_IMAGE_LAYOUT_GENERAL);
    for (unsigned repeat=0;repeat<2;++repeat) {
        if (repeat) {
            expect(totk_lsfg_rebind(context,images[1],images[0],error),true,"probe rebind actual inputs");
            expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"rebind invalidates capture");
            ProbeTrace trace;
            expect(totk_lsfg_probe_prepare(context,error),true,"reuse prepared readback");
            if (trace.creates || trace.allocations || trace.submits || trace.waits)
                throw std::runtime_error("reused probe readback allocated or waited");
        }
        // Compare the same context/pair without and with the diagnostic. The
        // extra post-dispatch dependencies must not change a single output byte.
        Core::CommandBuffer baseline(vk.device,vk.commandPool); baseline.begin();
        {
            ProbeTrace trace;
            expect(totk_lsfg_record(context,baseline.handle(),.5f,VK_NULL_HANDLE,error),true,"baseline ordinary pair");
            if (trace.copies || trace.creates || trace.allocations || trace.submits || trace.waits ||
                trace.dispatches!=ProbeTrace::pairDispatches)
                throw std::runtime_error("ordinary baseline performed diagnostic work");
        }
        baseline.end(); Core::Fence baselineFence(vk.device);
        baseline.submit(vk.device.getComputeQueue(),baselineFence);
        completed(baselineFence,vk.device,"ordinary baseline pair");
        const auto ordinaryPixels=readback(vk,outputImage,8);
        expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"baseline does not capture");
        Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
        vkCmdResetQueryPool(cb.handle(),query,0,TOTK_LSFG_TIMESTAMPS);
        std::array<ProbeTrace::Copy,14> regions;
        {
            ProbeTrace trace;
            expect(totk_lsfg_record_probe(context,cb.handle(),query,error),true,"probe pair recording");
            if (trace.bad || trace.copies!=14 || trace.dispatches!=ProbeTrace::pairDispatches ||
                trace.stamps!=TOTK_LSFG_TIMESTAMPS || trace.creates || trace.allocations || trace.submits || trace.waits)
                throw std::runtime_error("probe recording changed dispatch/timestamp scheme or allocated/submitted/waited");
            for (unsigned i=0;i<14;++i) {
                const auto& copy=trace.regions[i];
                if (copy.dispatch!=(i<7 ? 1u : 30u) || copy.region.bufferOffset%4 ||
                    copy.region.bufferRowLength || copy.region.bufferImageHeight ||
                    copy.region.imageExtent.depth!=1 || copy.region.imageSubresource.mipLevel ||
                    copy.region.imageSubresource.baseArrayLayer || copy.region.imageSubresource.layerCount!=1 ||
                    copy.region.imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT ||
                    copy.region.imageOffset.x || copy.region.imageOffset.y || copy.region.imageOffset.z)
                    throw std::runtime_error("probe copies did not occur immediately after each input mipmaps dispatch");
                if (i && copy.region.bufferOffset<trace.regions[i-1].region.bufferOffset+
                        VkDeviceSize(trace.regions[i-1].region.imageExtent.width)*trace.regions[i-1].region.imageExtent.height)
                    throw std::runtime_error("probe retained readback ranges overlap");
                if (i>=7 && copy.image!=trace.regions[i-7].image)
                    throw std::runtime_error("probe did not snapshot both writes to the same seven mipmaps outputs");
            }
            if (repeat && trace.buffer!=readbackBuffer) throw std::runtime_error("probe readback was not reused");
            readbackBuffer=trace.buffer; regions=trace.regions;
        }
        cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
        completed(fence,vk.device,"probe pair");
        uint64_t times[TOTK_LSFG_TIMESTAMPS]{};
        check(vkGetQueryPoolResults(vk.device.handle(),query,0,TOTK_LSFG_TIMESTAMPS,sizeof times,times,
            sizeof(uint64_t),VK_QUERY_RESULT_64_BIT),"probe timestamp results");
        for (unsigned i=1;i<TOTK_LSFG_TIMESTAMPS;++i)
            if (times[i]<times[i-1]) throw std::runtime_error("probe timestamps are not monotonic");
        if (readback(vk,outputImage,8)!=ordinaryPixels)
            throw std::runtime_error("probe changed ordinary generated output bytes");
        const auto output=directory.path/std::to_string(repeat);
        std::filesystem::create_directory(output);
        {
            ProbeTrace trace;
            expect(totk_lsfg_probe_write(context,(output/"missing").c_str(),error),false,"probe file failure");
            expect(totk_lsfg_probe_write(context,output.c_str(),error),true,"probe pixel and manifest export");
            if (trace.bad || trace.invalidations!=2 || trace.submits || trace.waits || trace.copies || trace.allocations)
                throw std::runtime_error("probe export invalidation/fence contract failed");
        }
        probe_manifest(output,owners,reference,regions,repeat!=0);
    }
    Core::CommandBuffer normal(vk.device,vk.commandPool); normal.begin();
    {
        ProbeTrace trace;
        expect(totk_lsfg_record(context,normal.handle(),.5f,VK_NULL_HANDLE,error),true,"ordinary recording after probe");
        if (trace.copies || trace.creates || trace.allocations || trace.submits || trace.waits ||
            trace.dispatches!=ProbeTrace::pairDispatches)
            throw std::runtime_error("ordinary recording captured a probe or changed its work");
    }
    normal.end(); Core::Fence normalFence(vk.device); normal.submit(vk.device.getComputeQueue(),normalFence);
    completed(normalFence,vk.device,"ordinary no-capture pair");
    expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"ordinary record invalidates stale capture");
    vkDestroyQueryPool(vk.device.handle(),query,nullptr);
    {
        ProbeTrace trace; trace.watchedBuffer=readbackBuffer; trace.watchedMemory=readbackMemory;
        totk_lsfg_retire(context);
        if (trace.destroyed!=1 || trace.freed!=1 || trace.waits || trace.submits)
            throw std::runtime_error("context retirement did not release its readback after the covering fence");
    }
    expect(totk_lsfg_probe_write(context,directory.path.c_str(),error),false,"retired probe context");
    std::puts("PASS real probe C ABI: strict literal opt-in, targeted usage, allocation failure/reuse, both seven-level snapshots, actual rebound metadata, noncoherent invalidation, exact ordinary-output equality, no-capture and fence-owned retirement");
}
static void fractional_tests(VkInstance instance,Vulkan& vk,LSFG_3_1::Context& context,
        const std::array<Core::Image,3>& owners,const char *dll,bool perf,float flow,
        bool direct,bool hdr,unsigned bytes,const std::vector<uint8_t>& legacy) {
    // Assert the shader ABI independently of the production offsetof used by
    // vkCmdUpdateBuffer, including the non-phase fields of the full 48-byte UBO.
    static_assert(sizeof(Pool::ConstantBuffer)==48 && offsetof(Pool::ConstantBuffer,timestamp)==28);
    for (bool color:{false,true}) {
        Pool::ResourcePool resources(color,flow);
        const auto data=resources.getBufferData(.3f);
        if (data.inputOffset!=std::array<uint32_t,2>{0,0} || data.firstIter || data.firstIterS ||
            data.advancedColorKind!=(color ? 2u : 0u) || data.hdrSupport!=unsigned(color) ||
            data.resolutionInvScale!=flow || data.timestamp!=.3f || data.uiThreshold!=.5f ||
            data.pad!=std::array<uint32_t,3>{0,0,0})
            throw std::runtime_error("fractional uniform does not preserve the complete legacy shader ABI");
    }
    const std::array<float,8> phases{0.0f,std::nextafter(0.0f,1.0f),.125f,.3f,.5f,
        .75f,std::nextafter(1.0f,0.0f),1.0f};
    const std::array<float,6> invalid{NAN,INFINITY,-INFINITY,-.001f,
        std::nextafter(0.0f,-1.0f),std::nextafter(1.0f,2.0f)};
    auto recorded=[&](const ProbeTrace& trace,float phase,unsigned pairs=1) {
        const unsigned updates=phase==.5f ? 0 : pairs;
        if (trace.bad || trace.creates || trace.allocations || trace.descriptorAllocations ||
            trace.descriptorUpdates || trace.submits || trace.waits ||
            trace.dispatches!=ProbeTrace::pairDispatches*pairs || trace.phaseUpdates!=updates ||
            (updates && std::memcmp(&trace.phase,&phase,sizeof phase)))
            throw std::runtime_error("fractional record allocated resources or lost exact command-owned phase");
    };
    auto finite=[&](const std::vector<uint8_t>& pixels) {
        if (pixels.empty()) throw std::runtime_error("fractional generation returned no pixels");
        if (hdr) for (size_t i=0;i<pixels.size();i+=bytes) {
            _Float16 rgba[4]; std::memcpy(rgba,pixels.data()+i,sizeof rgba);
            for (unsigned c=0;c<3;c++) if (!std::isfinite(float(rgba[c])))
                throw std::runtime_error("fractional user-DLL RGB output is nonfinite");
        }
    };
    for (float phase:invalid) {
        ProbeTrace trace;
        bool refused=false;
        try { context.recordPair(vk,Core::CommandBuffer(VK_NULL_HANDLE),VK_NULL_HANDLE,phase); }
        catch (const std::exception&) { refused=true; }
        if (!refused || trace.dispatches || trace.phaseUpdates || trace.creates || trace.allocations)
            throw std::runtime_error("invalid fractional phase recorded commands or allocated resources");
    }
    std::array<std::vector<uint8_t>,8> outputs;
    for (unsigned i=0;i<phases.size();i++) {
        Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
        {
            ProbeTrace trace;
            context.recordPair(vk,cb,VK_NULL_HANDLE,phases[i]);
            recorded(trace,phases[i]);
        }
        cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
        completed(fence,vk.device,"fractional user-DLL generation");
        outputs[i]=readback(vk,LSFG_3_1::ContextTest::output(context),bytes);
        finite(outputs[i]);
        if (phases[i]==.5f && outputs[i]!=legacy)
            throw std::runtime_error("explicit .5 output is not byte-identical to legacy midpoint");
    }
    if (outputs[2]==outputs[3] || outputs[2]==outputs[5] || outputs[3]==outputs[5])
        throw std::runtime_error("moving user-DLL fixture did not distinguish .125/.3/.75 phases");
    // Many unique phase values in one primary must reuse exactly the same
    // preallocated resources. Drop this unsubmitted stream after inspecting it.
    {
        Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
        ProbeTrace trace;
        for (unsigned i=0;i<128;i++)
            context.recordPair(vk,cb,VK_NULL_HANDLE,float(2*i+1)/256.0f);
        recorded(trace,255.0f/256.0f,128);
        cb.end();
    }
    // Back-to-back different phases in ONE submitted stream must match the
    // separately executed outputs, not both observe a final mapped CPU write.
    {
        const auto extent=owners[0].getExtent();
        std::array<Core::Image,2> snapshots{
            Core::Image(vk.device,extent,owners[0].getFormat(),VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT),
            Core::Image(vk.device,extent,owners[0].getFormat(),VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT)};
        Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
        for (unsigned i=0;i<2;i++) {
            context.recordPair(vk,cb,VK_NULL_HANDLE,i ? .75f : .125f);
            auto output=LSFG_3_1::ContextTest::output(context);
            barrier(cb.handle(),output,VK_ACCESS_2_MEMORY_WRITE_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            barrier(cb.handle(),snapshots[i],0,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy copy{.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={extent.width,extent.height,1}};
            vkCmdCopyImage(cb.handle(),output.handle(),VK_IMAGE_LAYOUT_GENERAL,
                snapshots[i].handle(),VK_IMAGE_LAYOUT_GENERAL,1,&copy);
            barrier(cb.handle(),output,VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
        completed(fence,vk.device,"two fractional phases in one primary");
        if (readback(vk,snapshots[0],bytes)!=outputs[2] || readback(vk,snapshots[1],bytes)!=outputs[5])
            throw std::runtime_error("phases in one primary alias mutable timestamp storage");
    }
    // Record BOTH frame primaries before either submission; then queue both
    // before waiting on either fence. A CPU-updated shared timestamp would
    // deterministically give frame0 the later frame's phase even on a driver
    // that completes the first submit immediately. GPU-ordered updates must
    // preserve both immutable snapshots without descriptor/buffer recreation.
    {
        const auto extent=owners[0].getExtent();
        std::array<Core::Image,2> snapshots{
            Core::Image(vk.device,extent,owners[0].getFormat(),VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT),
            Core::Image(vk.device,extent,owners[0].getFormat(),VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT)};
        std::array<Core::CommandBuffer,2> frames{
            Core::CommandBuffer(vk.device,vk.commandPool),Core::CommandBuffer(vk.device,vk.commandPool)};
        std::array<Core::Fence,2> fences{Core::Fence(vk.device),Core::Fence(vk.device)};
        for (unsigned i=0;i<2;i++) {
            frames[i].begin();
            {
                ProbeTrace trace;
                const float phase=i ? .75f : .125f;
                context.recordPair(vk,frames[i],VK_NULL_HANDLE,phase);
                recorded(trace,phase);
            }
            auto output=LSFG_3_1::ContextTest::output(context);
            barrier(frames[i].handle(),output,VK_ACCESS_2_MEMORY_WRITE_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            barrier(frames[i].handle(),snapshots[i],0,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy copy{.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.extent={extent.width,extent.height,1}};
            vkCmdCopyImage(frames[i].handle(),output.handle(),VK_IMAGE_LAYOUT_GENERAL,
                snapshots[i].handle(),VK_IMAGE_LAYOUT_GENERAL,1,&copy);
            barrier(frames[i].handle(),output,VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_ACCESS_2_SHADER_WRITE_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT);
            frames[i].end();
        }
        {
            ProbeTrace trace;
            frames[0].submit(vk.device.getComputeQueue(),fences[0]);
            frames[1].submit(vk.device.getComputeQueue(),fences[1]);
            if (trace.submits!=2 || trace.waits || trace.creates || trace.allocations ||
                trace.descriptorAllocations || trace.descriptorUpdates)
                throw std::runtime_error("multiple in-flight frames waited early or recreated phase storage");
        }
        completed(fences[0],vk.device,"fractional in-flight frame0");
        completed(fences[1],vk.device,"fractional in-flight frame1");
        if (readback(vk,snapshots[0],bytes)!=outputs[2] || readback(vk,snapshots[1],bytes)!=outputs[5])
            throw std::runtime_error("multiple in-flight frames reused timestamp storage before prior GPU readers retired");
    }
    if (direct && hdr) {
        ProbeABI abi;
        char error[256]{};
        auto expect=[&](int result,bool success,const char *what) {
            probe_expect(result,success,error,what); error[0]=0;
        };
        expect(totk_lsfg_initialize(vkGetInstanceProcAddr,instance,vk.device.getPhysicalDevice(),
            vk.device.handle(),vk.device.getComputeQueue(),vk.device.getComputeFamilyIdx(),
            vk.device.supportsNullDescriptor(),dll,perf,flow,error),true,"fractional C ABI initialization");
        std::array<totk_lsfg_image,3> images;
        for (unsigned i=0;i<3;i++) images[i]={owners[i].handle(),owners[i].getView(),owners[i].getFormat()};
        const auto extent=owners[0].getExtent();
        int32_t id{};
        {
            ProbeTrace trace;
            expect(totk_lsfg_context(images[0],images[1],images[2],extent.width,extent.height,&id,error),
                true,"fractional C ABI context");
            if (!trace.uniformWrites || trace.wrongUniformRanges)
                throw std::runtime_error("fractional context did not bind full 48-byte shader uniforms");
        }
        for (float phase:invalid) {
            ProbeTrace trace;
            expect(totk_lsfg_record(id,VK_NULL_HANDLE,phase,VK_NULL_HANDLE,error),false,"invalid C ABI phase");
            if (trace.dispatches || trace.phaseUpdates || trace.creates || trace.allocations)
                throw std::runtime_error("invalid C phase changed the GPU command stream");
        }
        for (unsigned i=0;i<phases.size();i++) {
            Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
            {
                ProbeTrace trace;
                expect(totk_lsfg_record(id,cb.handle(),phases[i],VK_NULL_HANDLE,error),true,"fractional C ABI record");
                recorded(trace,phases[i]);
            }
            cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
            completed(fence,vk.device,"fractional C ABI generation");
            auto output=Core::Image(images[2].image,images[2].view,extent,images[2].format,VK_IMAGE_LAYOUT_GENERAL);
            if (readback(vk,output,bytes)!=outputs[i])
                throw std::runtime_error("C ABI timestamp output differs from identical C++ user-DLL phase");
        }
        totk_lsfg_retire(id);
    }
    std::puts("PASS actual user-DLL finite endpoint/near-endpoint/nonmidpoint phases, byte-exact legacy .5, bounded resources and command-owned timestamps");
}

int main(int argc,char **argv) {
 try {
    if (argc!=3 && argc!=5) { std::fprintf(stderr,"usage: lsfgtest /absolute/path/to/user/Lossless.dll rgba16f|rgb10a2|rgba8 [edge-width edge-height]\n"); return 2; }
    bool edges=argc==5;
    bool hdr=!std::strcmp(argv[2],"rgba16f"),ten=!std::strcmp(argv[2],"rgb10a2");
    if (!hdr && !ten && std::strcmp(argv[2],"rgba8")) throw std::runtime_error("unknown transport");
    VkFormat format=hdr ? VK_FORMAT_R16G16B16A16_SFLOAT :
        ten ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R8G8B8A8_UNORM;
    unsigned bytes=hdr ? 8 : 4;
    bool mock=!std::strncmp(argv[1],"mock:",5);
    bool direct=mock || std::getenv("LSFG_TEST_DIRECT");
    bool perf=std::getenv("LSFG_TEST_PERF");
    bool drs=std::getenv("LSFG_TEST_DRS_CROP");
    bool probeTest=std::getenv("LSFG_TEST_PROBE");
    if (probeTest && (mock || !direct || !hdr))
        throw std::runtime_error("LSFG_TEST_PROBE requires actual user DLL, LSFG_TEST_DIRECT=1 and rgba16f");
    if (drs && !hdr) throw std::runtime_error("DRS crop fixture requires float HDR poison");
    float flow=0.5f;
    if (const char* s=std::getenv("LSFG_TEST_FLOW")) flow=std::strtof(s,nullptr);
    auto shaders=mock ? std::function<std::vector<uint8_t>(const std::string&)>(
        [directory=std::string(argv[1]+5)](const std::string& name) {
            std::ifstream file(directory+(name=="generate" ? "/generate.spv" : "/noop.spv"),std::ios::binary);
            if (!file) throw std::runtime_error("missing mock shader");
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),{});
        }) : lsfg_user_shaders(argv[1],perf);
    Core::Instance instance;
    Core::Device owned(instance,0x1463ABAC);
    Vulkan vk{.device={owned.getPhysicalDevice(),owned.handle(),owned.getComputeQueue(),
        owned.getComputeFamilyIdx(),owned.supportsNullDescriptor()},
        .generationCount=1,.flowScale=1.0f/flow,.isHdr=hdr,.performance=perf};
    bool missing_refused=false;
    try { (void)lsfg_user_shaders(""); } catch (const std::exception&) { missing_refused=true; }
    if (!missing_refused) throw std::runtime_error("missing user DLL was not refused");
    std::puts("PASS missing user-DLL loader refusal");
    vk.commandPool=Core::CommandPool(vk.device); vk.descriptorPool=Core::DescriptorPool(vk.device);
    vk.resources=Pool::ResourcePool(hdr,1.0f/flow); vk.shaders=Pool::ShaderPool(shaders);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(vk.device.getPhysicalDevice(),&props);
    std::printf("LSFG host GPU: %s; %s shaders, %s %s, model=%s flow=%.2f\n",
        props.deviceName,mock ? "MIT mock" : "actual user-DLL",argv[2],
        direct ? "direct shared-device images" : "transfer-only transport/native copies",
        perf ? "performance" : "normal",flow);
    unsigned W=edges ? std::strtoul(argv[3],nullptr,10) : 256;
    unsigned H=edges ? std::strtoul(argv[4],nullptr,10) : 256;
    if (W<256 || H<256 || W>4096 || H>4096) throw std::runtime_error("test extent outside 256..4096");
    constexpr VkImageUsageFlags transportUsage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags usage=transportUsage | (direct ? VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT : 0);
    std::array<Core::Image,3> owners{
        Core::Image(vk.device,{W,H},format,usage),
        Core::Image(vk.device,{W,H},format,usage),
        Core::Image(vk.device,{W,H},format,usage)};
    barrier_masks(vk,owners);
    auto shared=owners;
    if (direct) for (unsigned i=0;i<3;i++)
        shared[i]=Core::Image(owners[i].handle(),owners[i].getView(),{W,H},format,VK_IMAGE_LAYOUT_UNDEFINED);
    LSFG_3_1::Context context(vk,std::move(shared),{W,H},format,direct);
    std::array<std::vector<uint8_t>,2> normalized;
    for (unsigned frame=0;frame<2;frame++) {
        const unsigned cropW=drs ? W*(frame ? 5u : 3u)/(frame ? 8u : 4u) : W;
        const unsigned cropH=drs ? (H*(frame ? 5u : 3u)/(frame ? 8u : 4u))&~7u : H;
        if (drs) std::printf("DRS real%u allocated=%ux%u valid-crop=0,0,%u,%u -> normalized pre-UI canvas=%ux%u; NaN outside crop\n",
            frame,W,H,cropW,cropH,W,H);
        std::vector<uint8_t> pixels(W*H*bytes);
        for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
            const unsigned p=(y*W+x)*bytes;
            const unsigned sx=drs ? x*W/cropW : x,sy=drs ? y*H/cropH : y;
            bool box=sx>=64+frame*8 && sx<128+frame*8 && sy>=80 && sy<176;
            if (edges) box=(sx>=W-112+frame*8 && sx<W-48+frame*8 && sy>=H-96) ||
                (sx<64+frame*8 && sy<96) || (sx>=W-64+frame*8 && sy<96);
            float rgb[4]={box ? (hdr ? 2.0f : 0.9f) : (sx/16+sy/16)%2 ? 0.12f : 0.04f,
                box ? 0.5f : 0.08f,box ? 0.15f : 0.25f,1.0f};
            if (drs && (x>=cropW || y>=cropH)) rgb[0]=rgb[1]=rgb[2]=NAN;
            if (hdr) {
                _Float16 packed[4]={(_Float16)rgb[0],(_Float16)rgb[1],(_Float16)rgb[2],(_Float16)rgb[3]};
                std::memcpy(pixels.data()+p,packed,sizeof packed);
            } else {
                unsigned bits=ten ? 10 : 8,mask=(1u<<bits)-1;
                uint32_t packed=uint32_t(std::lround(rgb[0]*mask)) |
                    uint32_t(std::lround(rgb[1]*mask))<<bits | uint32_t(std::lround(rgb[2]*mask))<<(2*bits) |
                    (ten ? 3u<<30 : 255u<<24);
                std::memcpy(pixels.data()+p,&packed,sizeof packed);
            }
        }
        auto& input=LSFG_3_1::ContextTest::input(context,frame);
        Core::Image allocation;
        Core::Image* uploaded=&input;
        if (drs) {
            allocation=Core::Image(vk.device,{W,H},format,transportUsage);
            uploaded=&allocation;
        }
        Core::Buffer staging(vk.device,pixels.data(),pixels.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Core::CommandBuffer cb(vk.device,vk.commandPool); cb.begin();
        barrier(cb.handle(),*uploaded,0,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={W,H,1}};
        vkCmdCopyBufferToImage(cb.handle(),staging.handle(),uploaded->handle(),VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        if (drs) {
            barrier(cb.handle(),*uploaded,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
            barrier(cb.handle(),input,0,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageBlit region{.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .srcOffsets={{0,0,0},{int(cropW),int(cropH),1}},
                .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},
                .dstOffsets={{0,0,0},{int(W),int(H),1}}};
            vkCmdBlitImage(cb.handle(),uploaded->handle(),VK_IMAGE_LAYOUT_GENERAL,input.handle(),
                VK_IMAGE_LAYOUT_GENERAL,1,&region,VK_FILTER_NEAREST);
        }
        cb.end(); Core::Fence fence(vk.device); cb.submit(vk.device.getComputeQueue(),fence);
        completed(fence,vk.device,"input upload");
        // Compare to actual normalized texels, avoiding CPU/driver floating-
        // point differences in nearest-blit coordinates at integer boundaries.
        if (mock && drs) normalized[frame]=readback(vk,input,bytes);
        if (direct) {
            VkImageMemoryBarrier2 b{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,.dstAccessMask=VK_ACCESS_2_SHADER_READ_BIT,
                .oldLayout=VK_IMAGE_LAYOUT_GENERAL,.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
                .image=input.handle(),.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
            VkDependencyInfo d{.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b};
            // The upload command above is already ended: use the next primary.
            Core::CommandBuffer transition(vk.device,vk.commandPool); transition.begin();
            vkCmdPipelineBarrier2(transition.handle(),&d); transition.end();
            Core::Fence f(vk.device); transition.submit(vk.device.getComputeQueue(),f);
            completed(f,vk.device,"layout");
            input.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    VkQueryPool query{};
    VkQueryPoolCreateInfo qi{.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=30};
    check(vkCreateQueryPool(vk.device.handle(),&qi,nullptr,&query),"timestamps");
    Core::CommandBuffer generated(vk.device,vk.commandPool);
    std::vector<uint8_t> reference,pixels;
    auto scratch=LSFG_3_1::ContextTest::scratch(context);
    for (unsigned repeat=0;repeat<2;repeat++) {
        if (repeat) generated.reset();
        if (direct && repeat)
            context.rebindInputs(vk,owners[0].handle(),owners[0].getView(),owners[1].handle(),owners[1].getView());
        generated.begin();
        for (auto history:scratch) {
            if (!history.valid() || (direct && history.handle()==LSFG_3_1::ContextTest::output(context).handle())) continue;
            barrier(generated.handle(),history,history.getLayout()==VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
                VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue poison{{float(repeat),float(repeat),float(repeat),float(repeat)}};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(generated.handle(),history.handle(),VK_IMAGE_LAYOUT_GENERAL,&poison,1,&range);
            barrier(generated.handle(),history,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_ACCESS_2_SHADER_READ_BIT|VK_ACCESS_2_SHADER_WRITE_BIT);
        }
        vkCmdResetQueryPool(generated.handle(),query,0,30);
        context.recordPair(vk,Core::CommandBuffer(generated.handle()),query);
        generated.end(); Core::Fence generated_fence(vk.device); generated.submit(vk.device.getComputeQueue(),generated_fence);
        completed(generated_fence,vk.device,"generation");
        pixels=readback(vk,LSFG_3_1::ContextTest::output(context),bytes);
        if (!repeat) reference=pixels;
    }
    uint64_t times[30]; check(vkGetQueryPoolResults(vk.device.handle(),query,0,30,sizeof times,times,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT),"GPU time");
    double milliseconds=(times[29]-times[0])*props.limits.timestampPeriod/1e6;
    vkDestroyQueryPool(vk.device.handle(),query,nullptr);
    uint64_t history_changes=0;
    for (size_t i=0;i<pixels.size();i++) history_changes+=pixels[i]!=reference[i];
    if (mock) {
        uint64_t wrong=0;
        for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
            _Float16 rgba[4]; std::memcpy(rgba,pixels.data()+(size_t(y)*W+x)*8,sizeof rgba);
            float expected[3]={0,0,0};
            for (unsigned frame=0;frame<2;frame++) {
                if (drs) {
                    _Float16 sample[4];
                    std::memcpy(sample,normalized[frame].data()+(size_t(y)*W+x)*8,sizeof sample);
                    for (unsigned c=0;c<3;c++) expected[c]+=float(sample[c])*0.5f;
                    continue;
                }
                const unsigned sx=x,sy=y;
                bool box=sx>=64+frame*8 && sx<128+frame*8 && sy>=80 && sy<176;
                if (edges) box=(sx>=W-112+frame*8 && sx<W-48+frame*8 && sy>=H-96) ||
                    (sx<64+frame*8 && sy<96) || (sx>=W-64+frame*8 && sy<96);
                expected[0]+=(box ? 2.0f : (sx/16+sy/16)%2 ? 0.12f : 0.04f)*0.5f;
                expected[1]+=(box ? 0.5f : 0.08f)*0.5f;
                expected[2]+=(box ? 0.15f : 0.25f)*0.5f;
            }
            for (unsigned c=0;c<3;c++) {
                bool bad=!std::isfinite(float(rgba[c])) || std::fabs(float(rgba[c])-expected[c])>0.002f;
                if (bad && wrong<8) std::fprintf(stderr,"mock mismatch x=%u y=%u c=%u expected=%.6f actual=%.6f\n",x,y,c,expected[c],float(rgba[c]));
                wrong+=bad;
            }
        }
        if (!hdr || wrong || history_changes || pixels.empty() || !std::isfinite(milliseconds) || milliseconds<=0)
            throw std::runtime_error("mock direct midpoint/layout/history/timestamp failure");
        std::puts("PASS MIT mock same-device pair, reduced-channel descriptors, borrowed device/command lifetime, all 30 timestamp markers and deterministic output");
        return 0;
    }
    uint64_t bright=0,xsum=0,nonfinite=0,bad_edges=0;
    float maximum=0;
    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
        unsigned p=(y*W+x)*bytes; float rgb[3];
        if (hdr) {
            _Float16 packed[4]; std::memcpy(packed,pixels.data()+p,sizeof packed);
            for (unsigned c=0;c<3;c++) rgb[c]=packed[c];
        } else {
            uint32_t packed; std::memcpy(&packed,pixels.data()+p,sizeof packed);
            unsigned bits=ten ? 10 : 8,mask=(1u<<bits)-1;
            for (unsigned c=0;c<3;c++) rgb[c]=float((packed>>(c*bits))&mask)/mask;
        }
        for (unsigned c=0;c<3;c++) nonfinite+=!std::isfinite(rgb[c]);
        if (edges && (x<64 || x>=W-64 || y<64 || y>=H-64))
            bad_edges+=rgb[0]<0.015f || rgb[1]<0.015f || rgb[2]<0.015f ||
                rgb[0]>(hdr ? 2.1f : 0.96f) || rgb[1]>0.6f || rgb[2]>0.35f;
        float red=rgb[0]; if (red>maximum) maximum=red;
        if (red>(hdr ? 1.5f : 0.7f)) { bright++; xsum+=x; }
    }
    double centroid=bright ? double(xsum)/bright : -1;
    std::printf("LSFG %s %ux%u moving midpoint: bright=%llu x=%.3f max=%.3f nonfinite=%llu bad-edge-pixels=%llu poisoned-scratch-byte-changes=%llu GPU-total=%.3f ms\n",
        argv[2],W,H,(unsigned long long)bright,centroid,maximum,(unsigned long long)nonfinite,
        (unsigned long long)bad_edges,(unsigned long long)history_changes,milliseconds);
    // RGB is the framegen contract: the existing affine compositor supplies alpha=1.
    if (nonfinite || bad_edges || history_changes || (!edges &&
        (bright<5000 || bright>7300 || centroid<98 || centroid>102 || maximum<(hdr ? 1.8f : 0.8f))) ||
        !std::isfinite(milliseconds) || milliseconds<=0) throw std::runtime_error("transport midpoint/edge/history/timing assertion failed");
    std::puts("PASS actual LSFG pair + complete edges + independence from poisoned scratch + GPU timestamps");
    fractional_tests(instance.handle(),vk,context,owners,argv[1],perf,flow,direct,hdr,bytes,reference);
    if (probeTest) probe_tests(instance.handle(),vk,context,owners,argv[1],perf,flow);
    return 0;
 } catch (const std::exception& e) { std::fprintf(stderr,"FAIL LSFG: %s\n",e.what()); return 1; }
}
