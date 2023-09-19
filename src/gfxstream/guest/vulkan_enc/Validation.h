// Copyright (C) 2018 The Android Open Source Project
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

class Validation {
   public:
    VkResult on_vkFlushMappedMemoryRanges(void* context, VkResult input_result, VkDevice device,
                                          uint32_t memoryRangeCount,
                                          const VkMappedMemoryRange* pMemoryRanges);
    VkResult on_vkInvalidateMappedMemoryRanges(void* context, VkResult input_result,
                                               VkDevice device, uint32_t memoryRangeCount,
                                               const VkMappedMemoryRange* pMemoryRanges);
};

}  // namespace vk
}  // namespace gfxstream
