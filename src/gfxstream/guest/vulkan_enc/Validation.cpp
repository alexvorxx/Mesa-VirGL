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
#include "Validation.h"

#include "ResourceTracker.h"
#include "Resources.h"

namespace gfxstream {
namespace vk {

VkResult Validation::on_vkFlushMappedMemoryRanges(void*, VkResult, VkDevice,
                                                  uint32_t memoryRangeCount,
                                                  const VkMappedMemoryRange* pMemoryRanges) {
    auto resources = ResourceTracker::get();

    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        if (!resources->isValidMemoryRange(pMemoryRanges[i])) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    return VK_SUCCESS;
}

VkResult Validation::on_vkInvalidateMappedMemoryRanges(void*, VkResult, VkDevice,
                                                       uint32_t memoryRangeCount,
                                                       const VkMappedMemoryRange* pMemoryRanges) {
    auto resources = ResourceTracker::get();

    for (uint32_t i = 0; i < memoryRangeCount; ++i) {
        if (!resources->isValidMemoryRange(pMemoryRanges[i])) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    return VK_SUCCESS;
}

}  // namespace vk
}  // namespace gfxstream
