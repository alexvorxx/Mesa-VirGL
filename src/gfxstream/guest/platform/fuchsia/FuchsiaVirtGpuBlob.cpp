/*
 * Copyright 2023 The Android Open Source Project
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

#include "FuchsiaVirtGpu.h"
#include "util/log.h"

FuchsiaVirtGpuResource::FuchsiaVirtGpuResource(int64_t deviceHandle, uint32_t blobHandle,
                                               uint32_t resourceHandle, uint64_t size) {}

FuchsiaVirtGpuResource::~FuchsiaVirtGpuResource(void) {}

uint32_t FuchsiaVirtGpuResource::getBlobHandle() const {
    mesa_loge("%s: unimplemented", __func__);
    return 0;
}

uint32_t FuchsiaVirtGpuResource::getResourceHandle() const {
    mesa_loge("%s: unimplemented", __func__);
    return 0;
}

VirtGpuResourceMappingPtr FuchsiaVirtGpuResource::createMapping(void) {
    mesa_loge("%s: unimplemented", __func__);
    return nullptr;
}

int FuchsiaVirtGpuResource::wait() { return -1; }

int FuchsiaVirtGpuResource::exportBlob(struct VirtGpuExternalHandle& handle) {
    mesa_loge("%s: unimplemented", __func__);
    return 0;
}

int FuchsiaVirtGpuResource::transferFromHost(uint32_t offset, uint32_t size) {
    mesa_loge("%s: unimplemented", __func__);
    return 0;
}

int FuchsiaVirtGpuResource::transferToHost(uint32_t offset, uint32_t size) {
    mesa_loge("%s: unimplemented", __func__);
    return 0;
}
