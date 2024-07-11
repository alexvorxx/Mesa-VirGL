/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VirtGpuKumquat.h"
#include "cutils/log.h"

VirtGpuKumquatResourceMapping::VirtGpuKumquatResourceMapping(VirtGpuResourcePtr blob,
                                                             struct virtgpu_kumquat* virtGpu,
                                                             uint8_t* ptr, uint64_t size)
    : mBlob(blob), mVirtGpu(virtGpu), mPtr(ptr), mSize(size) {}

VirtGpuKumquatResourceMapping::~VirtGpuKumquatResourceMapping(void) {
    int32_t ret = virtgpu_kumquat_resource_unmap(mVirtGpu, mBlob->getBlobHandle());
    if (ret) {
        ALOGE("failed to unmap buffer");
    }
}

uint8_t* VirtGpuKumquatResourceMapping::asRawPtr(void) { return mPtr; }
