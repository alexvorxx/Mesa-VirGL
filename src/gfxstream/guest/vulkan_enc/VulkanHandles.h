// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <vulkan/vulkan.h>

namespace gfxstream {
namespace vk {

#define GOLDFISH_VK_LIST_TRIVIAL_DISPATCHABLE_HANDLE_TYPES(f) f(VkPhysicalDevice)

#define GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(f) \
    f(VkInstance)                                     \
    f(VkDevice)                                       \
    f(VkCommandBuffer)                                \
    f(VkQueue)                                        \
    GOLDFISH_VK_LIST_TRIVIAL_DISPATCHABLE_HANDLE_TYPES(f)

#ifdef VK_NVX_binary_import

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NVX_BINARY_IMPORT(f) \
    f(VkCuModuleNVX)                                                          \
    f(VkCuFunctionNVX)

#else

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NVX_BINARY_IMPORT(f)

#endif  // VK_NVX_binary_import

#ifdef VK_NVX_device_generated_commands

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NVX_DEVICE_GENERATED_COMMANDS(f) \
    f(VkObjectTableNVX)                                                                   \
    f(VkIndirectCommandsLayoutNVX)

#else

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NVX_DEVICE_GENERATED_COMMANDS(f)

#endif  // VK_NVX_device_generated_commands

#ifdef VK_NV_device_generated_commands

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NV_DEVICE_GENERATED_COMMANDS(f) \
    f(VkIndirectCommandsLayoutNV)

#else

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NV_DEVICE_GENERATED_COMMANDS(f)

#endif  // VK_NV_device_generated_commands

#ifdef VK_NV_ray_tracing

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NV_RAY_TRACING(f) \
    f(VkAccelerationStructureNV)

#else

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NV_RAY_TRACING(f)

#endif  // VK_NV_ray_tracing

#ifdef VK_KHR_acceleration_structure

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_KHR_ACCELERATION_STRUCTURE(f) \
    f(VkAccelerationStructureKHR)

#else

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_KHR_ACCELERATION_STRUCTURE(f)

#endif  // VK_KHR_acceleration_structure

#ifdef VK_USE_PLATFORM_FUCHSIA

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_FUCHSIA(f) f(VkBufferCollectionFUCHSIA)

#else

#define __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_FUCHSIA(f)

#endif  // VK_USE_PLATFORM_FUCHSIA

#define GOLDFISH_VK_LIST_TRIVIAL_NON_DISPATCHABLE_HANDLE_TYPES(f)                     \
    f(VkBufferView)                                                                   \
    f(VkImageView)                                                                    \
    f(VkShaderModule)                                                                 \
    f(VkPipeline)                                                                     \
    f(VkPipelineCache)                                                                \
    f(VkPipelineLayout)                                                               \
    f(VkRenderPass)                                                                   \
    f(VkFramebuffer)                                                                  \
    f(VkEvent)                                                                        \
    f(VkQueryPool)                                                                    \
    f(VkSamplerYcbcrConversion)                                                       \
    f(VkSurfaceKHR)                                                                   \
    f(VkSwapchainKHR)                                                                 \
    f(VkDisplayKHR)                                                                   \
    f(VkDisplayModeKHR)                                                               \
    f(VkValidationCacheEXT)                                                           \
    f(VkDebugReportCallbackEXT)                                                       \
    f(VkDebugUtilsMessengerEXT)                                                       \
    f(VkMicromapEXT)                                                                  \
    __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NVX_BINARY_IMPORT(f)             \
    __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NVX_DEVICE_GENERATED_COMMANDS(f) \
    __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NV_DEVICE_GENERATED_COMMANDS(f)  \
    __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_NV_RAY_TRACING(f)                \
    __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_KHR_ACCELERATION_STRUCTURE(f)

#define GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(f)       \
    f(VkDeviceMemory)                                           \
    f(VkBuffer)                                                 \
    f(VkImage)                                                  \
    f(VkSemaphore)                                              \
    f(VkDescriptorUpdateTemplate)                               \
    f(VkFence)                                                  \
    f(VkDescriptorPool)                                         \
    f(VkDescriptorSet)                                          \
    f(VkDescriptorSetLayout)                                    \
    f(VkCommandPool)                                            \
    f(VkSampler)                                                \
    __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_FUCHSIA(f) \
    GOLDFISH_VK_LIST_TRIVIAL_NON_DISPATCHABLE_HANDLE_TYPES(f)

#define GOLDFISH_VK_LIST_HANDLE_TYPES(f)          \
    GOLDFISH_VK_LIST_DISPATCHABLE_HANDLE_TYPES(f) \
    GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES(f)

#define GOLDFISH_VK_LIST_TRIVIAL_HANDLE_TYPES(f)          \
    GOLDFISH_VK_LIST_TRIVIAL_DISPATCHABLE_HANDLE_TYPES(f) \
    GOLDFISH_VK_LIST_TRIVIAL_NON_DISPATCHABLE_HANDLE_TYPES(f)

#define GOLDFISH_VK_LIST_AUTODEFINED_STRUCT_DISPATCHABLE_HANDLE_TYPES(f) \
    f(VkInstance)                                                        \
    f(VkDevice)                                                          \
    f(VkQueue)                                                           \
    GOLDFISH_VK_LIST_TRIVIAL_DISPATCHABLE_HANDLE_TYPES(f)

#define GOLDFISH_VK_LIST_AUTODEFINED_STRUCT_NON_DISPATCHABLE_HANDLE_TYPES(f) \
    f(VkDeviceMemory)                                                        \
    f(VkBuffer)                                                              \
    f(VkImage)                                                               \
    f(VkSemaphore)                                                           \
    f(VkFence)                                                               \
    f(VkDescriptorUpdateTemplate)                                            \
    f(VkCommandPool)                                                         \
    f(VkSampler)                                                             \
    __GOLDFISH_VK_LIST_NON_DISPATCHABLE_HANDLE_TYPES_FUCHSIA(f)              \
    GOLDFISH_VK_LIST_TRIVIAL_NON_DISPATCHABLE_HANDLE_TYPES(f)

#define GOLDFISH_VK_LIST_MANUAL_STRUCT_NON_DISPATCHABLE_HANDLE_TYPES(f) \
    f(VkDescriptorPool)                                                 \
    f(VkDescriptorSetLayout)                                            \
    f(VkDescriptorSet)

}  // namespace vk
}  // namespace gfxstream
