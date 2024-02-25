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
#include <os_dirent.h>
#include <services/service_connector.h>
#include <unistd.h>

#include <climits>
#include <cstdio>
#include <cstdlib>

#include "FuchsiaVirtGpu.h"

FuchsiaVirtGpuDevice::FuchsiaVirtGpuDevice(enum VirtGpuCapset capset, magma_device_t device)
    : VirtGpuDevice(capset), device_(device) {}

FuchsiaVirtGpuDevice::~FuchsiaVirtGpuDevice() { magma_device_release(device_); }

int64_t FuchsiaVirtGpuDevice::getDeviceHandle(void) { return device_; }

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
        return nullptr;
    }

    const char kDevGpu[] = "/loader-gpu-devices/class/gpu";

    struct os_dirent* de;
    os_dir_t* dir = os_opendir(kDevGpu);
    if (!dir) {
        ALOGE("Error opening %s", kDevGpu);
        return nullptr;
    }

    ALOGD("Opened dir %s", kDevGpu);

    VirtGpuDevice* gpu_device = nullptr;

    while ((de = os_readdir(dir)) != NULL) {
        ALOGD("Got name %s", de->d_name);

        if (strcmp(de->d_name, ".") == 0) {
            continue;
        }
        // extra +1 ensures space for null termination
        char name[sizeof(kDevGpu) + sizeof('/') + sizeof(de->d_name) + 1];
        snprintf(name, sizeof(name), "%s/%s", kDevGpu, de->d_name);

        zx_handle_t device_channel = GetConnectToServiceFunction()(name);
        if (device_channel == ZX_HANDLE_INVALID) {
            ALOGE("Failed to open device: %s", name);
            continue;
        }

        magma_device_t magma_device;
        magma_status_t status = magma_device_import(device_channel, &magma_device);
        if (status != MAGMA_STATUS_OK) {
            ALOGE("magma_device_import failed: %d", status);
            continue;
        }

        gpu_device = new FuchsiaVirtGpuDevice(capset, magma_device);
        break;
    }
    os_closedir(dir);

    return gpu_device;
}
