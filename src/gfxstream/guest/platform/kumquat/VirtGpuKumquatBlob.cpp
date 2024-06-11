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

#include <cutils/log.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "VirtGpuKumquat.h"
#include "virtgpu_kumquat/virtgpu_kumquat_ffi.h"

VirtGpuKumquatResource::VirtGpuKumquatResource(struct virtgpu_kumquat* virtGpu, uint32_t blobHandle,
                                               uint32_t resourceHandle, uint64_t size)
    : mVirtGpu(virtGpu), mBlobHandle(blobHandle), mResourceHandle(resourceHandle), mSize(size) {}

VirtGpuKumquatResource::~VirtGpuKumquatResource() {
    struct drm_kumquat_resource_unref unref {
        .bo_handle = mBlobHandle, .pad = 0,
    };

    int ret = virtgpu_kumquat_resource_unref(mVirtGpu, &unref);
    if (ret) {
        ALOGE("Closed failed with : [%s, blobHandle %u, resourceHandle: %u]", strerror(errno),
              mBlobHandle, mResourceHandle);
    }
}

uint32_t VirtGpuKumquatResource::getBlobHandle() const { return mBlobHandle; }

uint32_t VirtGpuKumquatResource::getResourceHandle() const { return mResourceHandle; }

VirtGpuResourceMappingPtr VirtGpuKumquatResource::createMapping() {
    int ret;
    struct drm_kumquat_map map {
        .bo_handle = mBlobHandle, .ptr = NULL, .size = mSize,
    };

    ret = virtgpu_kumquat_resource_map(mVirtGpu, &map);
    if (ret < 0) {
        ALOGE("Mapping failed with %s", strerror(errno));
        return nullptr;
    }

    return std::make_shared<VirtGpuKumquatResourceMapping>(shared_from_this(), (uint8_t*)map.ptr,
                                                           mSize);
}

int VirtGpuKumquatResource::exportBlob(struct VirtGpuExternalHandle& handle) {
    int ret;
    struct drm_kumquat_resource_export exp = {0};

    exp.bo_handle = mBlobHandle;

    ret = virtgpu_kumquat_resource_export(mVirtGpu, &exp);
    if (ret) {
        ALOGE("Failed to export blob with %s", strerror(errno));
        return ret;
    }

    handle.osHandle = static_cast<int64_t>(exp.os_handle);
    handle.type = static_cast<VirtGpuHandleType>(exp.handle_type);
    return 0;
}

int VirtGpuKumquatResource::wait() {
    int ret;
    struct drm_kumquat_wait wait = {
        .handle = mBlobHandle,
        .flags = 0,
    };

    ret = virtgpu_kumquat_wait(mVirtGpu, &wait);
    if (ret < 0) {
        ALOGE("Wait failed with %s", strerror(errno));
        return ret;
    }

    return 0;
}

int VirtGpuKumquatResource::transferToHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    int ret;
    struct drm_kumquat_transfer_to_host xfer = {0};

    xfer.box.x = x;
    xfer.box.y = y;
    xfer.box.w = w;
    xfer.box.h = h;
    xfer.box.d = 1;
    xfer.bo_handle = mBlobHandle;

    ret = virtgpu_kumquat_transfer_to_host(mVirtGpu, &xfer);
    if (ret < 0) {
        ALOGE("Transfer to host failed with %s", strerror(errno));
        return ret;
    }

    return 0;
}

int VirtGpuKumquatResource::transferFromHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    int ret;
    struct drm_kumquat_transfer_from_host xfer = {0};

    xfer.box.x = x;
    xfer.box.y = y;
    xfer.box.w = w;
    xfer.box.h = h;
    xfer.box.d = 1;
    xfer.bo_handle = mBlobHandle;

    ret = virtgpu_kumquat_transfer_from_host(mVirtGpu, &xfer);
    if (ret < 0) {
        ALOGE("Transfer from host failed with %s", strerror(errno));
        return ret;
    }

    return 0;
}
