#pragma once

#include "core/device.hpp"
#include "core/buffer.hpp"
#include "core/sampler.hpp"
#include "core/commandbuffer.hpp"

#include "vulkan/vulkan_core.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LSFG::Pool {

    // Uniform ABI consumed by the translated LSFG shaders. Fractional
    // generation must retain every field, not substitute a single float UBO.
    struct ConstantBuffer {
        std::array<uint32_t, 2> inputOffset;
        uint32_t firstIter;
        uint32_t firstIterS;
        uint32_t advancedColorKind;
        uint32_t hdrSupport;
        float resolutionInvScale;
        float timestamp;
        float uiThreshold;
        std::array<uint32_t, 3> pad;
    };
    static_assert(sizeof(ConstantBuffer) == 48);
    static_assert(offsetof(ConstantBuffer, timestamp) == 28);

    ///
    /// Resource pool for each Vulkan device.
    ///
    class ResourcePool {
    public:
        ResourcePool() noexcept = default;

        ///
        /// Create the resource pool.
        ///
        /// @param isHdr HDR support stored in buffers.
        /// @param flowScale Scale factor stored in buffers.
        ///
        /// @throws std::runtime_error if the resource pool cannot be created.
        ///
        ResourcePool(bool isHdr, float flowScale)
            : isHdr(isHdr), flowScale(flowScale) {}

        ///
        /// Retrieve a buffer with given parameters or create it.
        ///
        /// @param timestamp Timestamp stored in buffer
        /// @param firstIter First iteration stored in buffer
        /// @param firstIterS First special iteration stored in buffer
        /// @return Created or cached buffer
        ///
        /// @throws LSFG::vulkan_error if the buffer cannot be created.
        ///
        Core::Buffer getBuffer(
            const Core::Device& device,
            float timestamp = 0.0F, bool firstIter = false, bool firstIterS = false);

        /// Shared uniform packing for immutable cached and fixed phase buffers.
        [[nodiscard]] ConstantBuffer getBufferData(
            float timestamp = 0.0F, bool firstIter = false, bool firstIterS = false) const;

        ///
        /// Retrieve a sampler by type or create it.
        ///
        /// @param type Type of the sampler
        /// @param compare Compare operation for the sampler
        /// @param isWhite Whether the sampler is white
        /// @return Created or cached sampler
        ///
        /// @throws LSFG::vulkan_error if the sampler cannot be created.
        ///
        Core::Sampler getSampler(
            const Core::Device& device,
            VkSamplerAddressMode type = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            VkCompareOp compare = VK_COMPARE_OP_NEVER,
            bool isWhite = false);

        /// TOTK_LSFG_UBO_REFRESH (default on): re-write every pooled constant buffer
        /// from its creation bytes with vkCmdUpdateBuffer, ordered before this
        /// pair's reads. Odin's pooled buffers were overwritten after upload by an
        /// unidentified writer; this is the workaround, not a root-cause fix.
        void recordRefresh(const Core::CommandBuffer& buffer);

    private:
        std::unordered_map<uint64_t, Core::Buffer> buffers;
        std::vector<std::pair<Core::Buffer, ConstantBuffer>> refreshed; // creation order
        std::unordered_map<uint64_t, Core::Sampler> samplers;
        bool isHdr{};
        float flowScale{};
        size_t refreshLogged{};
    };

}
