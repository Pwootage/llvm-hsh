#pragma once

#if HSH_ENABLE_VULKAN

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>

#ifdef HSH_IMPLEMENTATION
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#define VMA_IMPLEMENTATION
#endif

#define VMA_USE_STL_CONTAINERS 1
#define VMA_USE_STL_SHARED_MUTEX 1
#include "vk_mem_alloc_hsh.h"

#endif
