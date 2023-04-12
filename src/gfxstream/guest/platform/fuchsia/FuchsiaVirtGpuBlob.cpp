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

#include <cutils/log.h>

#include "FuchsiaVirtGpu.h"

FuchsiaVirtGpuBlob::FuchsiaVirtGpuBlob(int64_t deviceHandle, uint32_t blobHandle,
                                       uint32_t resourceHandle, uint64_t size) {}

FuchsiaVirtGpuBlob::~FuchsiaVirtGpuBlob(void) {}

uint32_t FuchsiaVirtGpuBlob::getBlobHandle() const {
    ALOGE("%s: unimplemented", __func__);
    return 0;
}

uint32_t FuchsiaVirtGpuBlob::getResourceHandle() const {
    ALOGE("%s: unimplemented", __func__);
    return 0;
}

VirtGpuBlobMappingPtr FuchsiaVirtGpuBlob::createMapping(void) {
    ALOGE("%s: unimplemented", __func__);
    return nullptr;
}

int FuchsiaVirtGpuBlob::wait() { return -1; }

int FuchsiaVirtGpuBlob::exportBlob(struct VirtGpuExternalHandle& handle) {
    ALOGE("%s: unimplemented", __func__);
    return 0;
}

int FuchsiaVirtGpuBlob::transferFromHost(uint32_t offset, uint32_t size) {
    ALOGE("%s: unimplemented", __func__);
    return 0;
}

int FuchsiaVirtGpuBlob::transferToHost(uint32_t offset, uint32_t size) {
    ALOGE("%s: unimplemented", __func__);
    return 0;
}
