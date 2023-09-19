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
#include "HostVisibleMemoryVirtualization.h"

#include <log/log.h>

#include <set>

#include "../OpenglSystemCommon/EmulatorFeatureInfo.h"
#include "ResourceTracker.h"
#include "Resources.h"
#include "VkEncoder.h"
#include "aemu/base/AndroidSubAllocator.h"

using gfxstream::guest::SubAllocator;

namespace gfxstream {
namespace vk {

bool isHostVisible(const VkPhysicalDeviceMemoryProperties* memoryProps, uint32_t index) {
    return memoryProps->memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

CoherentMemory::CoherentMemory(VirtGpuBlobMappingPtr blobMapping, uint64_t size, VkDevice device,
                               VkDeviceMemory memory)
    : mSize(size), mBlobMapping(blobMapping), mDevice(device), mMemory(memory) {
    mAllocator =
        std::make_unique<gfxstream::guest::SubAllocator>(blobMapping->asRawPtr(), mSize, 4096);
}

#if defined(__ANDROID__)
CoherentMemory::CoherentMemory(GoldfishAddressSpaceBlockPtr block, uint64_t gpuAddr, uint64_t size,
                               VkDevice device, VkDeviceMemory memory)
    : mSize(size), mBlock(block), mDevice(device), mMemory(memory) {
    void* address = block->mmap(gpuAddr);
    mAllocator = std::make_unique<gfxstream::guest::SubAllocator>(address, mSize, kLargestPageSize);
}
#endif  // defined(__ANDROID__)

CoherentMemory::~CoherentMemory() {
    ResourceTracker::getThreadLocalEncoder()->vkFreeMemorySyncGOOGLE(mDevice, mMemory, nullptr,
                                                                     false);
}

VkDeviceMemory CoherentMemory::getDeviceMemory() const { return mMemory; }

bool CoherentMemory::subAllocate(uint64_t size, uint8_t** ptr, uint64_t& offset) {
    auto address = mAllocator->alloc(size);
    if (!address) return false;

    *ptr = (uint8_t*)address;
    offset = mAllocator->getOffset(address);
    return true;
}

bool CoherentMemory::release(uint8_t* ptr) {
    mAllocator->free(ptr);
    return true;
}

}  // namespace vk
}  // namespace gfxstream
