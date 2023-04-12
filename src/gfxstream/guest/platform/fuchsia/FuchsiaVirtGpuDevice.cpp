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

FuchsiaVirtGpuDevice::FuchsiaVirtGpuDevice(enum VirtGpuCapset capset) : VirtGpuDevice(capset) {}

FuchsiaVirtGpuDevice::~FuchsiaVirtGpuDevice() {}

int64_t FuchsiaVirtGpuDevice::getDeviceHandle(void) {
    ALOGE("%s: unimplemented", __func__);
    return 0;
}

VirtGpuBlobPtr FuchsiaVirtGpuDevice::createBlob(const struct VirtGpuCreateBlob& blobCreate) {
    ALOGE("%s: unimplemented", __func__);
    return nullptr;
}

VirtGpuBlobPtr FuchsiaVirtGpuDevice::createVirglBlob(uint32_t width, uint32_t height,
                                                     uint32_t virglFormat) {
    ALOGE("%s: unimplemented", __func__);
    return nullptr;
}

VirtGpuBlobPtr FuchsiaVirtGpuDevice::importBlob(const struct VirtGpuExternalHandle& handle) {
    ALOGE("%s: unimplemented", __func__);
    return nullptr;
}

int FuchsiaVirtGpuDevice::execBuffer(struct VirtGpuExecBuffer& execbuffer,
                                     const VirtGpuBlob* blob) {
    ALOGE("%s: unimplemented", __func__);
    return 0;
}

struct VirtGpuCaps FuchsiaVirtGpuDevice::getCaps(void) { return {}; }

VirtGpuDevice* createPlatformVirtGpuDevice(enum VirtGpuCapset capset, int fd) {
    // We don't handle the VirtioGpuPipeStream case.
    if (fd >= 0) {
        ALOGE("Fuchsia: fd not handled");
        abort();
    }
    return new FuchsiaVirtGpuDevice(capset);
}
