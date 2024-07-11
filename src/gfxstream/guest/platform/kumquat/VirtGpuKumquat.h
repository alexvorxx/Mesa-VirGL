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

#pragma once

#include "VirtGpu.h"
#include "virtgpu_kumquat/virtgpu_kumquat_ffi.h"

class VirtGpuKumquatResource : public std::enable_shared_from_this<VirtGpuKumquatResource>,
                               public VirtGpuResource {
   public:
    VirtGpuKumquatResource(struct virtgpu_kumquat* virtGpu, uint32_t blobHandle,
                           uint32_t resourceHandle, uint64_t size);
    ~VirtGpuKumquatResource();

    uint32_t getResourceHandle() const override;
    uint32_t getBlobHandle() const override;
    int wait() override;

    VirtGpuResourceMappingPtr createMapping(void) override;
    int exportBlob(struct VirtGpuExternalHandle& handle) override;

    int transferFromHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
    int transferToHost(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;

   private:
    // Not owned.  Really should use a ScopedFD for this, but doesn't matter since we have a
    // singleton deviceimplemenentation anyways.
    struct virtgpu_kumquat* mVirtGpu = nullptr;

    uint32_t mBlobHandle;
    uint32_t mResourceHandle;
    uint64_t mSize;
};

class VirtGpuKumquatResourceMapping : public VirtGpuResourceMapping {
   public:
    VirtGpuKumquatResourceMapping(VirtGpuResourcePtr blob, struct virtgpu_kumquat* virtGpu,
                                  uint8_t* ptr, uint64_t size);
    ~VirtGpuKumquatResourceMapping(void);

    uint8_t* asRawPtr(void) override;

   private:
    VirtGpuResourcePtr mBlob;
    struct virtgpu_kumquat* mVirtGpu = nullptr;
    uint8_t* mPtr;
    uint64_t mSize;
};

class VirtGpuKumquatDevice : public VirtGpuDevice {
   public:
    VirtGpuKumquatDevice(enum VirtGpuCapset capset, int fd = -1);
    virtual ~VirtGpuKumquatDevice();

    virtual int64_t getDeviceHandle(void);

    virtual struct VirtGpuCaps getCaps(void);

    VirtGpuResourcePtr createBlob(const struct VirtGpuCreateBlob& blobCreate) override;
    VirtGpuResourcePtr createResource(uint32_t width, uint32_t height, uint32_t virglFormat,
                                      uint32_t target, uint32_t bind, uint32_t bpp) override;

    virtual VirtGpuResourcePtr importBlob(const struct VirtGpuExternalHandle& handle);
    virtual int execBuffer(struct VirtGpuExecBuffer& execbuffer, const VirtGpuResource* blob);

   private:
    struct virtgpu_kumquat* mVirtGpu = nullptr;
    struct VirtGpuCaps mCaps;
};
