#pragma once

#if HSH_ENABLE_VULKAN

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_NO_EXCEPTIONS
#define VK_USE_PLATFORM_XCB_KHR
#include <vulkan/vulkan.hpp>

#define VMA_USE_STL_CONTAINERS 1
#define VMA_USE_STL_SHARED_MUTEX 1
#include "vk_mem_alloc_hsh.h"

#endif
