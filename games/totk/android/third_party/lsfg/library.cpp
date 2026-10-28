/* MIT. C ABI isolation keeps volk's mutable dispatch symbols out of libtotk. */
#include <volk.h>
#include "api.h"
#include "shaders.hpp"
#include "v3_1/context.hpp"
#include "notices.h"
#include <cstdio>
#include <exception>
#include <optional>
#include <unordered_map>
#include <stdexcept>
#define API extern "C" __attribute__((visibility("default")))
namespace {
std::optional<LSFG::Vulkan> shared;
std::unordered_map<int32_t,LSFG_3_1::Context> contexts;
int32_t nextContext=1;
int failed(char error[256],const std::exception& e) {
    std::snprintf(error,256,"%s",e.what()); return 0;
}
LSFG::Core::Image borrow(totk_lsfg_image image,VkExtent2D extent,VkImageLayout layout) {
    return {image.image,image.view,extent,image.format,layout};
}
}
API int totk_lsfg_initialize(PFN_vkGetInstanceProcAddr loader,VkInstance instance,
        VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,
        int nullDescriptor,const char *dll,int performance,float flow,char error[256]) {
    try {
        auto shaders=lsfg_user_shaders(dll,performance!=0);
        volkInitializeCustom(loader); volkLoadInstance(instance);
        shared.emplace(LSFG::Vulkan{.device={physical,device,queue,family,nullDescriptor!=0},
            .generationCount=1,.flowScale=1.0f/flow,.isHdr=true,.performance=performance!=0});
        shared->descriptorPool=LSFG::Core::DescriptorPool(shared->device);
        shared->resources=LSFG::Pool::ResourcePool(true,1.0f/flow);
        shared->shaders=LSFG::Pool::ShaderPool(shaders);
        return 1;
    } catch (const std::exception& e) { return failed(error,e); }
}
API int totk_lsfg_context(totk_lsfg_image previous,totk_lsfg_image current,totk_lsfg_image output,
        uint32_t width,uint32_t height,int32_t *context,char error[256]) {
    try {
        if (!shared) throw std::runtime_error("LSFG shared device unavailable");
        VkExtent2D extent{width,height};
        int32_t id=nextContext++;
        contexts.emplace(id,LSFG_3_1::Context(*shared,std::array<LSFG::Core::Image,3>{
            borrow(previous,extent,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            borrow(current,extent,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            borrow(output,extent,VK_IMAGE_LAYOUT_UNDEFINED)},extent,output.format,true));
        *context=id; return 1;
    } catch (const std::exception& e) { return failed(error,e); }
}
API int totk_lsfg_rebind(int32_t context,totk_lsfg_image previous,totk_lsfg_image current,char error[256]) {
    try {
        contexts.at(context).rebindInputs(*shared,previous.image,previous.view,current.image,current.view);
        return 1;
    } catch (const std::exception& e) { return failed(error,e); }
}
API void totk_lsfg_retire(int32_t context) { contexts.erase(context); }
API int totk_lsfg_record(int32_t context,VkCommandBuffer commands,float timestamp,
        VkQueryPool timestamps,char error[256]) {
    try {
        contexts.at(context).recordPair(*shared,LSFG::Core::CommandBuffer(commands),timestamps,timestamp);
        return 1;
    } catch (const std::exception& e) { return failed(error,e); }
}
API int totk_lsfg_probe_prepare(int32_t context,char error[256]) {
    try {
        contexts.at(context).probePrepare(*shared);
        return 1;
    } catch (const std::exception& e) { return failed(error,e); }
}
API int totk_lsfg_record_probe(int32_t context,VkCommandBuffer commands,VkQueryPool timestamps,char error[256]) {
    try {
        contexts.at(context).recordProbe(*shared,LSFG::Core::CommandBuffer(commands),timestamps);
        return 1;
    } catch (const std::exception& e) { return failed(error,e); }
}
API int totk_lsfg_probe_write(int32_t context,const char *directory,char error[256]) {
    try {
        contexts.at(context).probeWrite(directory);
        return 1;
    } catch (const std::exception& e) { return failed(error,e); }
}
API const char *totk_lsfg_notices(void) { return lsfg_notices; }
#ifdef TOTK_LSFG_HOST_TEST
// The host owns its VkDevice on the stack; release borrowed-device globals
// before that device dies. Android keeps its backend for the renderer lifetime.
void lsfg_host_test_shutdown() {
    contexts.clear();
    shared.reset();
}
#endif
