#pragma once

#include "core/image.hpp"
#include "core/semaphore.hpp"
#include "core/fence.hpp"
#include "core/commandbuffer.hpp"
#include "shaders/alpha.hpp"
#include "shaders/beta.hpp"
#include "shaders/delta.hpp"
#include "shaders/gamma.hpp"
#include "shaders/generate.hpp"
#include "shaders/mipmaps.hpp"
#include "common/utils.hpp"

#include <vulkan/vulkan_core.h>

#include <vector>
#include <cstdint>
#include <array>
#include <optional>
#include <memory>

#ifdef __ANDROID__
struct AHardwareBuffer;
#endif

namespace LSFG_3_1 {

    using namespace LSFG;

    class Context {
    public:
        ///
        /// Create a context
        ///
        /// @param vk The Vulkan instance to use.
        /// @param in0 File descriptor for the first input image.
        /// @param in1 File descriptor for the second input image.
        /// @param outN File descriptors for the output images.
        /// @param extent The size of the images.
        /// @param format The format of the images.
        ///
        /// @throws LSFG::vulkan_error if the context fails to initialize.
        ///
        Context(Vulkan& vk,
            int in0, int in1, const std::vector<int>& outN,
            VkExtent2D extent, VkFormat format);

        // Pair inputs can be transport images or borrowed same-device images.
        Context(Vulkan& vk, std::array<Core::Image, 3> images,
            VkExtent2D extent, VkFormat format, bool direct = false);

#ifdef __ANDROID__
        ///
        /// Android-specific constructor: input/output images come from
        /// AHardwareBuffer instead of opaque FDs. See LSFG_3_1::createContextFromAHB.
        ///
        Context(Vulkan& vk,
            AHardwareBuffer* in0, AHardwareBuffer* in1,
            const std::vector<AHardwareBuffer*>& outN,
            VkExtent2D extent, VkFormat format);
#endif

        ///
        /// Present on the context.
        ///
        /// @param inSem Semaphore to wait on before starting the generation.
        /// @param outSem Semaphores to signal after each generation is done.
        ///
        /// @throws LSFG::vulkan_error if the context fails to present.
        ///
        void present(Vulkan& vk,
            int inSem, const std::vector<int>& outSem);

        // TOTK: process both immutable real inputs, then one finite [0,1] phase.
        // No unsubmitted-frame history is needed when admission skips ticks.
        // Default .5f retains the legacy immutable midpoint dispatch exactly.
        void recordPair(Vulkan& vk, const Core::CommandBuffer& buffer,
            VkQueryPool timestamps = VK_NULL_HANDLE, float timestamp = .5f);
        // Explicit diagnostic path; prepare/write never submit or wait.
        void probePrepare(Vulkan& vk);
        void recordProbe(Vulkan& vk, const Core::CommandBuffer& buffer,
            VkQueryPool timestamps = VK_NULL_HANDLE);
        void probeWrite(const char *directory) const;
        // The owning source ring has retired all uses before this call.
        void rebindInputs(Vulkan& vk, VkImage previous, VkImageView previousView,
            VkImage current, VkImageView currentView);

        // Trivially copyable, moveable and destructible
        Context(const Context&) = default;
        Context& operator=(const Context&) = default;
        Context(Context&&) = default;
        Context& operator=(Context&&) = default;
        ~Context() = default;
    private:
#ifdef TOTK_LSFG_HOST_TEST
        friend struct ContextTest;
#endif
        Core::Image inImg_0, inImg_1; // inImg_0 is next when fc % 2 == 0
        std::optional<std::array<Core::Image, 3>> transport;
        bool direct{};
        uint64_t frameIdx{0};
        struct Probe;
        std::shared_ptr<Probe> probe;
        void recordPairCommands(Vulkan& vk, const Core::CommandBuffer& buffer,
            VkQueryPool timestamps, bool capture, float timestamp);
        void probeSnapshot(const Core::CommandBuffer& buffer, unsigned frame) const;

        struct RenderData {
            Core::Semaphore inSemaphore; // signaled when input is ready
            std::vector<Core::Semaphore> internalSemaphores; // signaled when first step is done
            std::vector<Core::Semaphore> outSemaphores; // signaled when each pass is done
            std::vector<Core::Fence> completionFences; // fence for completion of each pass

            Core::CommandBuffer cmdBuffer1;
            std::vector<Core::CommandBuffer> cmdBuffers2; // command buffers for second step

            bool shouldWait{false};
        };
        std::array<RenderData, 8> data;

        Shaders::Mipmaps mipmaps;
        std::array<Shaders::Alpha, 7> alpha;
        Shaders::Beta beta;
        std::array<Shaders::Gamma, 7> gamma;
        std::array<Shaders::Delta, 3> delta;
        Shaders::Generate generate;
    };

}
