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
#ifndef VIRTGPU_DEVICE_H
#define VIRTGPU_DEVICE_H

#include <cstdint>
#include <memory>

#include "virtgpu_gfxstream_protocol.h"

enum VirtGpuParamId : uint32_t {
    kParam3D = 0,
    kParamCapsetFix = 1,
    kParamResourceBlob = 2,
    kParamHostVisible = 3,
    kParamCrossDevice = 4,
    kParamContextInit = 5,
    kParamSupportedCapsetIds = 6,
    kParamCreateGuestHandle = 7,
    kParamMax = 8,
};

enum VirtGpuExecBufferFlags : uint32_t {
    kFenceIn = 0x0001,
    kFenceOut = 0x0002,
    kRingIdx = 0x0004,
};

enum VirtGpuCapset {
    kCapsetNone = 0,
    kCapsetVirgl = 1,
    kCapsetVirgl2 = 2,
    kCapsetGfxStream = 3,
    kCapsetVenus = 4,
    kCapsetCrossDomain = 5,
};

// Try to keep aligned with vulkan-cereal / rutabaga.
enum VirtGpuHandleType {
    kMemHandleOpaqueFd = 0x0001,
    kMemHandleDmabuf = 0x0002,
    kMemHandleOpaqueWin32 = 0x0003,
    kMemHandleShm = 0x0004,
    kMemHandleZircon = 0x0008,
    kFenceHandleOpaqueFd = 0x0010,
    kFenceHandleSyncFd = 0x0020,
    kFenceHandleOpaqueWin32 = 0x0040,
    kFenceHandleZircon = 0x0080,
};

enum VirtGpuBlobFlags : uint32_t {
    kBlobFlagMappable = 0x0001,
    kBlobFlagShareable = 0x0002,
    kBlobFlagCrossDevice = 0x0004,
    kBlobFlagCreateGuestHandle = 0x0008,
};

enum VirtGpuBlobMem {
    kBlobMemGuest = 0x0001,
    kBlobMemHost3d = 0x0002,
    kBlobMemHost3dGuest = 0x0003,
};

struct VirtGpuExternalHandle {
    int64_t osHandle;
    enum VirtGpuHandleType type;
};

struct VirtGpuExecBuffer {
    void* command;
    uint32_t command_size;
    uint32_t ring_idx;
    enum VirtGpuExecBufferFlags flags;
    struct VirtGpuExternalHandle handle;
};

struct VirtGpuParam {
    uint64_t param;
    const char* name;
    uint64_t value;
};

struct VirtGpuCreateBlob {
    uint64_t size;
    enum VirtGpuBlobFlags flags;
    enum VirtGpuBlobMem blobMem;
    uint64_t blobId;
};

struct VirtGpuCaps {
    uint64_t params[kParamMax];
    struct gfxstreamCapset gfxstreamCapset;
};

class VirtGpuBlobMapping;
class VirtGpuBlob;
using VirtGpuBlobPtr = std::shared_ptr<VirtGpuBlob>;
using VirtGpuBlobMappingPtr = std::shared_ptr<VirtGpuBlobMapping>;

class VirtGpuBlob : public std::enable_shared_from_this<VirtGpuBlob> {
  public:
    VirtGpuBlob(int64_t deviceHandle, uint32_t blobHandle, uint32_t resourceHandle, uint64_t size);
    ~VirtGpuBlob();

    uint32_t getResourceHandle(void);
    uint32_t getBlobHandle(void);
    int wait(void);

    VirtGpuBlobMappingPtr createMapping(void);
    int exportBlob(struct VirtGpuExternalHandle& handle);

  private:
    // Not owned.  Really should use a ScopedFD for this, but doesn't matter since we have a
    // singleton deviceimplemenentation anyways.
    int64_t mDeviceHandle;

    uint32_t mBlobHandle;
    uint32_t mResourceHandle;
    uint64_t mSize;
};

class VirtGpuBlobMapping {
  public:
    VirtGpuBlobMapping(VirtGpuBlobPtr blob, uint8_t* ptr, uint64_t size);
    ~VirtGpuBlobMapping(void);

    uint8_t* asRawPtr(void);

  private:
    VirtGpuBlobPtr mBlob;
    uint8_t* mPtr;
    uint64_t mSize;
};

class VirtGpuDevice {
  public:
    static VirtGpuDevice& getInstance(enum VirtGpuCapset capset = kCapsetNone);
    int64_t getDeviceHandle(void);

    struct VirtGpuCaps getCaps(void);

    VirtGpuBlobPtr createBlob(const struct VirtGpuCreateBlob& blobCreate);
    VirtGpuBlobPtr createPipeBlob(uint32_t size);
    VirtGpuBlobPtr importBlob(const struct VirtGpuExternalHandle& handle);

    int execBuffer(struct VirtGpuExecBuffer& execbuffer, VirtGpuBlobPtr blob);

  private:
    VirtGpuDevice(enum VirtGpuCapset capset);
    ~VirtGpuDevice();
    VirtGpuDevice(VirtGpuDevice const&);
    void operator=(VirtGpuDevice const&);

    static VirtGpuDevice mInstance;
    int64_t mDeviceHandle;

    struct VirtGpuCaps mCaps;
};

// HACK: We can use android::base::EnumFlags, but we'll have to do more guest
// refactorings to figure out our end goal.  We can either depend more on base or
// try to transition to something else (b:202552093) [atleast for guests].
constexpr enum VirtGpuBlobFlags operator |(const enum VirtGpuBlobFlags self,
                                           const enum VirtGpuBlobFlags other) {
    return (enum VirtGpuBlobFlags)(uint32_t(self) | uint32_t(other));
}

constexpr enum  VirtGpuExecBufferFlags operator |(const enum VirtGpuExecBufferFlags self,
                                                  const enum VirtGpuExecBufferFlags other) {
    return (enum VirtGpuExecBufferFlags)(uint32_t(self) | uint32_t(other));
}

#endif
