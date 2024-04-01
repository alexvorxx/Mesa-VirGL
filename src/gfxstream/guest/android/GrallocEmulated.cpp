// Copyright 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "GrallocEmulated.h"

#include <cutils/log.h>

#include <optional>

#include "drm_fourcc.h"

namespace gfxstream {
namespace {

static constexpr int numFds = 0;
static constexpr int numInts = 1;

std::optional<uint32_t> GlFormatToDrmFormat(uint32_t glFormat) {
    switch (glFormat) {
        case kGlRGB:
            return DRM_FORMAT_BGR888;
        case kGlRGB565:
            return DRM_FORMAT_BGR565;
        case kGlRGBA:
            return DRM_FORMAT_ABGR8888;
    }
    return std::nullopt;
}

std::optional<uint32_t> AhbToDrmFormat(uint32_t ahbFormat) {
    switch (ahbFormat) {
        case GFXSTREAM_AHB_FORMAT_R8G8B8A8_UNORM:
            return DRM_FORMAT_ABGR8888;
        case GFXSTREAM_AHB_FORMAT_R8G8B8X8_UNORM:
            return DRM_FORMAT_XBGR8888;
        case GFXSTREAM_AHB_FORMAT_R8G8B8_UNORM:
            return DRM_FORMAT_BGR888;
        /*
        * Confusingly, AHARDWAREBUFFER_FORMAT_RGB_565 is defined as:
        *
        * "16-bit packed format that has 5-bit R, 6-bit G, and 5-bit B components, in that
        *  order, from the  most-sigfinicant bits to the least-significant bits."
        *
        * so the order of the components is intentionally not flipped between the pixel
        * format and the DRM format.
        */
        case GFXSTREAM_AHB_FORMAT_R5G6B5_UNORM:
            return DRM_FORMAT_RGB565;
        case GFXSTREAM_AHB_FORMAT_BLOB:
        case GFXSTREAM_AHB_FORMAT_R8_UNORM:
            return DRM_FORMAT_R8;
        case GFXSTREAM_AHB_FORMAT_YV12:
            return DRM_FORMAT_YVU420;
        case GFXSTREAM_AHB_FORMAT_R16G16B16A16_FLOAT:
            return DRM_FORMAT_ABGR16161616F;
        case GFXSTREAM_AHB_FORMAT_R10G10B10A2_UNORM:
            return DRM_FORMAT_ABGR2101010;
    }
    return std::nullopt;
}

std::optional<uint32_t> DrmToVirglFormat(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_ABGR8888:
            return VIRGL_FORMAT_R8G8B8A8_UNORM;
        case DRM_FORMAT_BGR888:
            return VIRGL_FORMAT_R8G8B8_UNORM;
        case DRM_FORMAT_BGR565:
            return VIRGL_FORMAT_B5G6R5_UNORM;
    }
    return std::nullopt;
}

}  // namespace

EmulatedAHardwareBuffer::EmulatedAHardwareBuffer(uint32_t width, uint32_t height,
                                                 VirtGpuResourcePtr resource)
    : mRefCount(1), mWidth(width), mHeight(height), mResource(resource) {}

EmulatedAHardwareBuffer::~EmulatedAHardwareBuffer() {}

uint32_t EmulatedAHardwareBuffer::getResourceId() const { return mResource->getResourceHandle(); }

uint32_t EmulatedAHardwareBuffer::getWidth() const { return mWidth; }

uint32_t EmulatedAHardwareBuffer::getHeight() const { return mHeight; }

int EmulatedAHardwareBuffer::getAndroidFormat() const {
    return /*AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM=*/1;
}

uint32_t EmulatedAHardwareBuffer::getDrmFormat() const { return DRM_FORMAT_ABGR8888; }

AHardwareBuffer* EmulatedAHardwareBuffer::asAHardwareBuffer() {
    return reinterpret_cast<AHardwareBuffer*>(this);
}

buffer_handle_t EmulatedAHardwareBuffer::asBufferHandle() {
    return reinterpret_cast<buffer_handle_t>(this);
}

EGLClientBuffer EmulatedAHardwareBuffer::asEglClientBuffer() {
    return reinterpret_cast<EGLClientBuffer>(this);
}

void EmulatedAHardwareBuffer::acquire() { ++mRefCount; }

void EmulatedAHardwareBuffer::release() {
    --mRefCount;
    if (mRefCount == 0) {
        delete this;
    }
}

int EmulatedAHardwareBuffer::lock(uint8_t** ptr) {
    if (!mMapped) {
        mMapped = mResource->createMapping();
        if (!mMapped) {
            ALOGE("Failed to lock EmulatedAHardwareBuffer: failed to create mapping.");
            return -1;
        }

        mResource->transferFromHost(0, 0, mWidth, mHeight);
        mResource->wait();
    }

    *ptr = (*mMapped)->asRawPtr();
    return 0;
}

int EmulatedAHardwareBuffer::unlock() {
    if (!mMapped) {
        ALOGE("Failed to unlock EmulatedAHardwareBuffer: never locked?");
        return -1;
    }
    mResource->transferToHost(0, 0, mWidth, mHeight);
    mResource->wait();
    mMapped.reset();
    return 0;
}

EmulatedGralloc::EmulatedGralloc() {}

uint32_t EmulatedGralloc::createColorBuffer(void*, int width, int height, uint32_t glFormat) {
    auto drmFormat = GlFormatToDrmFormat(glFormat);
    if (!drmFormat) {
        ALOGE("Unhandled format");
    }

    auto ahb = allocate(width, height, *drmFormat);

    EmulatedAHardwareBuffer* rahb = reinterpret_cast<EmulatedAHardwareBuffer*>(ahb);

    mOwned.emplace_back(rahb);

    return rahb->getResourceId();
}

int EmulatedGralloc::allocate(uint32_t width, uint32_t height, uint32_t ahbFormat, uint64_t usage,
                              AHardwareBuffer** outputAhb) {
    (void)usage;

    auto drmFormat = AhbToDrmFormat(ahbFormat);
    if (!drmFormat) {
        ALOGE("Unhandled AHB format:%u", ahbFormat);
        return -1;
    }

    *outputAhb = allocate(width, height, *drmFormat);
    return 0;
}

AHardwareBuffer* EmulatedGralloc::allocate(uint32_t width, uint32_t height, uint32_t format) {
    ALOGE("Allocating AHB w:%u, h:%u, format %u", width, height, format);

    auto device = VirtGpuDevice::getInstance();
    if (!device) {
        ALOGE("Failed to allocate: no virtio gpu device.");
        return nullptr;
    }

    auto virglFormat = DrmToVirglFormat(format);
    if (!virglFormat) {
        ALOGE("Unhandled DRM format:%u", format);
        return nullptr;
    }

    auto resource = device->createResource(width, height, *virglFormat);
    if (!resource) {
        ALOGE("Failed to allocate: failed to create virtio resource.");
        return nullptr;
    }

    resource->wait();

    return reinterpret_cast<AHardwareBuffer*>(
        new EmulatedAHardwareBuffer(width, height, std::move(resource)));
}

void EmulatedGralloc::acquire(AHardwareBuffer* ahb) {
    auto* rahb = reinterpret_cast<EmulatedAHardwareBuffer*>(ahb);
    rahb->acquire();
}

void EmulatedGralloc::release(AHardwareBuffer* ahb) {
    auto* rahb = reinterpret_cast<EmulatedAHardwareBuffer*>(ahb);
    rahb->release();
}

int EmulatedGralloc::lock(AHardwareBuffer* ahb, uint8_t** ptr) {
    auto* rahb = reinterpret_cast<EmulatedAHardwareBuffer*>(ahb);
    return rahb->lock(ptr);
}

int EmulatedGralloc::unlock(AHardwareBuffer* ahb) {
    auto* rahb = reinterpret_cast<EmulatedAHardwareBuffer*>(ahb);
    return rahb->unlock();
}

uint32_t EmulatedGralloc::getHostHandle(const native_handle_t* handle) {
    const auto* ahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(handle);
    return ahb->getResourceId();
}

uint32_t EmulatedGralloc::getHostHandle(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(handle);
    return ahb->getResourceId();
}

const native_handle_t* EmulatedGralloc::getNativeHandle(const AHardwareBuffer* ahb) {
    return reinterpret_cast<const native_handle_t*>(ahb);
}

int EmulatedGralloc::getFormat(const native_handle_t* handle) {
    const auto* ahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(handle);
    return ahb->getAndroidFormat();
}

int EmulatedGralloc::getFormat(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(handle);
    return ahb->getAndroidFormat();
}

uint32_t EmulatedGralloc::getFormatDrmFourcc(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(handle);
    return ahb->getDrmFormat();
}

uint32_t EmulatedGralloc::getWidth(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(handle);
    return ahb->getWidth();
}

uint32_t EmulatedGralloc::getHeight(const AHardwareBuffer* handle) {
    const auto* ahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(handle);
    return ahb->getHeight();
}

size_t EmulatedGralloc::getAllocatedSize(const native_handle_t*) {
    ALOGE("Unimplemented.");
    return 0;
}

size_t EmulatedGralloc::getAllocatedSize(const AHardwareBuffer*) {
    ALOGE("Unimplemented.");
    return 0;
}

int EmulatedGralloc::getId(const AHardwareBuffer* ahb, uint64_t* id) {
    const auto* rahb = reinterpret_cast<const EmulatedAHardwareBuffer*>(ahb);
    *id = rahb->getResourceId();
    return 0;
}

Gralloc* createPlatformGralloc(int /*deviceFd*/) {
    return new EmulatedGralloc();
}

}  // namespace gfxstream
