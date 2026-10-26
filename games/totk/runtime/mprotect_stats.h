/* Attribute repository-owned runtime permission changes alongside lag protection.
   Null/Metal builds retain the original libc syscall, without a Vulkan link dependency. */
#pragma once
#include <sys/mman.h>
#if defined(GPU_VK)
#include "../third_party/d2r_gfx/gfx/nvn/nvn_dirty.h"
#define runtime_mprotect(address, size, protection, reason) \
    nvn_dirty_mprotect((address), (size), (protection), (reason))
#else
#define runtime_mprotect(address, size, protection, reason) \
    mprotect((address), (size), (protection))
#endif
