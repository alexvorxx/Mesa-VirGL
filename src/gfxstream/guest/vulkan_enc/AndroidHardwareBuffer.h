/// Copyright (C) 2019 The Android Open Source Project
// Copyright (C) 2019 Google Inc.
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

#include "../OpenglSystemCommon/Gralloc.h"
#include "HostVisibleMemoryVirtualization.h"

// Structure similar to
// https://github.com/mesa3d/mesa/blob/master/src/intel/vulkan/anv_android.c

namespace gfxstream {
namespace vk {

uint64_t getAndroidHardwareBufferUsageFromVkUsage(const VkImageCreateFlags vk_create,
                                                  const VkImageUsageFlags vk_usage);

void updateMemoryTypeBits(uint32_t* memoryTypeBits, uint32_t colorBufferMemoryIndex);

VkResult getAndroidHardwareBufferPropertiesANDROID(
    gfxstream::Gralloc* grallocHelper, const AHardwareBuffer* buffer,
    VkAndroidHardwareBufferPropertiesANDROID* pProperties);

VkResult getMemoryAndroidHardwareBufferANDROID(gfxstream::Gralloc* grallocHelper,
                                               struct AHardwareBuffer** pBuffer);

VkResult importAndroidHardwareBuffer(gfxstream::Gralloc* grallocHelper,
                                     const VkImportAndroidHardwareBufferInfoANDROID* info,
                                     struct AHardwareBuffer** importOut);

VkResult createAndroidHardwareBuffer(gfxstream::Gralloc* grallocHelper, bool hasDedicatedImage,
                                     bool hasDedicatedBuffer, const VkExtent3D& imageExtent,
                                     uint32_t imageLayers, VkFormat imageFormat,
                                     VkImageUsageFlags imageUsage,
                                     VkImageCreateFlags imageCreateFlags, VkDeviceSize bufferSize,
                                     VkDeviceSize allocationInfoAllocSize,
                                     struct AHardwareBuffer** out);

}  // namespace vk
}  // namespace gfxstream
