/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: MIT
 */

#pragma once

#if defined(ANDROID)

#include <cutils/native_handle.h>
#include <stddef.h>
#include <stdint.h>

#include <vector>

typedef struct AHardwareBuffer AHardwareBuffer;

namespace gfxstream {

constexpr uint32_t kGlRGB = 0x1907;
constexpr uint32_t kGlRGBA = 0x1908;
constexpr uint32_t kGlRGB565 = 0x8D62;

// Buffer pixel formats mirrored from Android to avoid extra
// build dependencies on Android libraries.
enum {
    GFXSTREAM_AHB_FORMAT_R8G8B8A8_UNORM           = 1,
    GFXSTREAM_AHB_FORMAT_R8G8B8X8_UNORM           = 2,
    GFXSTREAM_AHB_FORMAT_R8G8B8_UNORM             = 3,
    GFXSTREAM_AHB_FORMAT_R5G6B5_UNORM             = 4,
    GFXSTREAM_AHB_FORMAT_B8G8R8A8_UNORM           = 5,
    GFXSTREAM_AHB_FORMAT_B5G5R5A1_UNORM           = 6,
    GFXSTREAM_AHB_FORMAT_B4G4R4A4_UNORM           = 7,
    GFXSTREAM_AHB_FORMAT_R16G16B16A16_FLOAT       = 0x16,
    GFXSTREAM_AHB_FORMAT_R10G10B10A2_UNORM        = 0x2b,
    GFXSTREAM_AHB_FORMAT_BLOB                     = 0x21,
    GFXSTREAM_AHB_FORMAT_D16_UNORM                = 0x30,
    GFXSTREAM_AHB_FORMAT_D24_UNORM                = 0x31,
    GFXSTREAM_AHB_FORMAT_D24_UNORM_S8_UINT        = 0x32,
    GFXSTREAM_AHB_FORMAT_D32_FLOAT                = 0x33,
    GFXSTREAM_AHB_FORMAT_D32_FLOAT_S8_UINT        = 0x34,
    GFXSTREAM_AHB_FORMAT_S8_UINT                  = 0x35,
    GFXSTREAM_AHB_FORMAT_Y8Cb8Cr8_420             = 0x23,
    GFXSTREAM_AHB_FORMAT_YV12                     = 0x32315659,
    GFXSTREAM_AHB_FORMAT_IMPLEMENTATION_DEFINED   = 0x22,
    GFXSTREAM_AHB_FORMAT_R8_UNORM                 = 0x38,
};

enum GrallocType {
    GRALLOC_TYPE_GOLDFISH = 1,
    GRALLOC_TYPE_MINIGBM = 2,
    GRALLOC_TYPE_EMULATED = 3,
};

// Abstraction for gralloc handle conversion
class Gralloc {
   public:
    virtual ~Gralloc() {}

    virtual GrallocType getGrallocType() = 0;
    virtual uint32_t createColorBuffer(int width, int height, uint32_t glformat) = 0;

    virtual void acquire(AHardwareBuffer* ahb) = 0;
    virtual void release(AHardwareBuffer* ahb) = 0;

    virtual int allocate(uint32_t width, uint32_t height, uint32_t ahbFormat, uint64_t usage,
                         AHardwareBuffer** outputAhb) = 0;

    virtual int lock(AHardwareBuffer* ahb, uint8_t** ptr) = 0;
    struct LockedPlane {
        uint8_t* data = nullptr;
        uint32_t pixelStrideBytes = 0;
        uint32_t rowStrideBytes = 0;
    };
    // If AHB is a YUV format, always returns Y, then U, then V.
    virtual int lockPlanes(AHardwareBuffer* ahb, std::vector<LockedPlane>* ahbPlanes) = 0;
    virtual int unlock(AHardwareBuffer* ahb) = 0;

    virtual const native_handle_t* getNativeHandle(const AHardwareBuffer* ahb) = 0;

    virtual uint32_t getHostHandle(const native_handle_t* handle) = 0;
    virtual uint32_t getHostHandle(const AHardwareBuffer* handle) = 0;

    virtual int getFormat(const native_handle_t* handle) = 0;
    virtual int getFormat(const AHardwareBuffer* handle) = 0;

    virtual uint32_t getFormatDrmFourcc(const AHardwareBuffer* /*handle*/) {
        // Equal to DRM_FORMAT_INVALID -- see <drm_fourcc.h>
        return 0;
    }
    virtual uint32_t getFormatDrmFourcc(const native_handle_t* /*handle*/) {
        // Equal to DRM_FORMAT_INVALID -- see <drm_fourcc.h>
        return 0;
    }

    virtual uint32_t getWidth(const AHardwareBuffer* ahb) = 0;
    virtual uint32_t getHeight(const AHardwareBuffer* ahb) = 0;

    virtual size_t getAllocatedSize(const native_handle_t* handle) = 0;
    virtual size_t getAllocatedSize(const AHardwareBuffer* handle) = 0;

    virtual int getId(const AHardwareBuffer* ahb, uint64_t* id) = 0;

    virtual bool treatBlobAsImage() { return false; }
};

Gralloc* createPlatformGralloc(int32_t descriptor = -1);

}  // namespace gfxstream

#endif  // defined(ANDROID)
