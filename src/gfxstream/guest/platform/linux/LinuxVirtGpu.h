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

class LinuxVirtGpuBlob : public std::enable_shared_from_this<LinuxVirtGpuBlob>, public VirtGpuBlob {
   public:
    LinuxVirtGpuBlob(int64_t deviceHandle, uint32_t blobHandle, uint32_t resourceHandle,
                     uint64_t size);
    ~LinuxVirtGpuBlob();

    uint32_t getResourceHandle() const override;
    uint32_t getBlobHandle() const override;
    int wait() override;

    VirtGpuBlobMappingPtr createMapping(void) override;
    int exportBlob(struct VirtGpuExternalHandle& handle) override;

    int transferFromHost(uint32_t offset, uint32_t size) override;
    int transferToHost(uint32_t offset, uint32_t size) override;

   private:
    // Not owned.  Really should use a ScopedFD for this, but doesn't matter since we have a
    // singleton deviceimplemenentation anyways.
    int64_t mDeviceHandle;

    uint32_t mBlobHandle;
    uint32_t mResourceHandle;
    uint64_t mSize;
};

class LinuxVirtGpuBlobMapping : public VirtGpuBlobMapping {
   public:
    LinuxVirtGpuBlobMapping(VirtGpuBlobPtr blob, uint8_t* ptr, uint64_t size);
    ~LinuxVirtGpuBlobMapping(void);

    uint8_t* asRawPtr(void) override;

   private:
    VirtGpuBlobPtr mBlob;
    uint8_t* mPtr;
    uint64_t mSize;
};

class LinuxVirtGpuDevice : public VirtGpuDevice {
   public:
    LinuxVirtGpuDevice(enum VirtGpuCapset capset, int fd = -1);
    virtual ~LinuxVirtGpuDevice();

    virtual int64_t getDeviceHandle(void);

    virtual struct VirtGpuCaps getCaps(void);

    VirtGpuBlobPtr createBlob(const struct VirtGpuCreateBlob& blobCreate) override;
    VirtGpuBlobPtr createVirglBlob(uint32_t width, uint32_t height, uint32_t virglFormat) override;

    virtual VirtGpuBlobPtr importBlob(const struct VirtGpuExternalHandle& handle);
    virtual int execBuffer(struct VirtGpuExecBuffer& execbuffer, const VirtGpuBlob* blob);

   private:
    int64_t mDeviceHandle;
    struct VirtGpuCaps mCaps;
};
