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

// See virgl_hw.h and p_defines.h
#define VIRGL_FORMAT_R8_UNORM 64
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define VIRGL_FORMAT_B5G6R5_UNORM 7
#define VIRGL_FORMAT_R8G8B8_UNORM 66
#define VIRGL_FORMAT_R8G8B8A8_UNORM 67

#define VIRGL_BIND_RENDER_TARGET (1 << 1)
#define VIRGL_BIND_CUSTOM (1 << 17)
#define PIPE_BUFFER 0
#define PIPE_TEXTURE_2D 2

enum VirtGpuParamId : uint32_t {
    kParam3D = 0,
    kParamCapsetFix = 1,
    kParamResourceBlob = 2,
    kParamHostVisible = 3,
    kParamCrossDevice = 4,
    kParamContextInit = 5,
    kParamSupportedCapsetIds = 6,
    kParamExplicitDebugName = 7,
    kParamCreateGuestHandle = 8,
    kParamMax = 9,
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
    kCapsetGfxStreamVulkan = 3,
    kCapsetVenus = 4,
    kCapsetCrossDomain = 5,
    kCapsetDrm = 6,
    kCapsetGfxStreamMagma = 7,
    kCapsetGfxStreamGles = 8,
    kCapsetGfxStreamComposer = 9,
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
    struct vulkanCapset vulkanCapset;
    struct magmaCapset magmaCapset;
    struct glesCapset glesCapset;
    struct composerCapset composerCapset;
};

class VirtGpuBlobMapping;
class VirtGpuBlob;
using VirtGpuBlobPtr = std::shared_ptr<VirtGpuBlob>;
using VirtGpuBlobMappingPtr = std::shared_ptr<VirtGpuBlobMapping>;

class VirtGpuBlob {
  public:
    virtual ~VirtGpuBlob() {}

    virtual uint32_t getResourceHandle() const = 0;
    virtual uint32_t getBlobHandle() const = 0;
    virtual int wait() = 0;

    virtual VirtGpuBlobMappingPtr createMapping(void) = 0;
    virtual int exportBlob(struct VirtGpuExternalHandle& handle) = 0;

    virtual int transferFromHost(uint32_t offset, uint32_t size) = 0;
    virtual int transferToHost(uint32_t offset, uint32_t size) = 0;
};

class VirtGpuBlobMapping {
  public:
    virtual ~VirtGpuBlobMapping(void) {}

    virtual uint8_t* asRawPtr(void) = 0;
};

class VirtGpuDevice {
  public:
    static VirtGpuDevice* getInstance(enum VirtGpuCapset capset = kCapsetNone);
    static void resetInstance();

    static void setInstanceForTesting(VirtGpuDevice* device);

    VirtGpuDevice(enum VirtGpuCapset capset) : mCapset(capset) {}
    virtual ~VirtGpuDevice() {}

    enum VirtGpuCapset capset() { return mCapset; }

    virtual int64_t getDeviceHandle(void) = 0;

    virtual struct VirtGpuCaps getCaps(void) = 0;

    virtual VirtGpuBlobPtr createBlob(const struct VirtGpuCreateBlob& blobCreate) = 0;
    virtual VirtGpuBlobPtr createVirglBlob(uint32_t width, uint32_t height, uint32_t virglFormat) = 0;
    virtual VirtGpuBlobPtr importBlob(const struct VirtGpuExternalHandle& handle) = 0;

    virtual int execBuffer(struct VirtGpuExecBuffer& execbuffer, const VirtGpuBlob* blob) = 0;

   private:
    enum VirtGpuCapset mCapset;
};

VirtGpuDevice* createPlatformVirtGpuDevice(enum VirtGpuCapset capset = kCapsetNone, int fd = -1);

// HACK: We can use gfxstream::guest::EnumFlags, but we'll have to do more guest
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
