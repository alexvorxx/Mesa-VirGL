// Copyright 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "GrallocGoldfish.h"

#include <gralloc_cb_bp.h>
#include <vndk/hardware_buffer.h>

#include "renderControl_enc.h"

namespace gfxstream {

uint32_t GoldfishGralloc::createColorBuffer(void* rcEnc, int width, int height, uint32_t glformat) {
    auto* rc = reinterpret_cast<renderControl_client_context_t*>(rcEnc);
    return rc->rcCreateColorBuffer(rc, width, height, glformat);
}

int GoldfishGralloc::allocate(uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
                              AHardwareBuffer** outputAhb) {
    struct AHardwareBuffer_Desc desc = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = usage,
    };

    return AHardwareBuffer_allocate(&desc, outputAhb);
}

void GoldfishGralloc::acquire(AHardwareBuffer* ahb) { AHardwareBuffer_acquire(ahb); }

void GoldfishGralloc::release(AHardwareBuffer* ahb) { AHardwareBuffer_release(ahb); }

int GoldfishGralloc::lock(AHardwareBuffer* ahb, uint8_t** ptr) {
    return AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr,
                                reinterpret_cast<void**>(ptr));
}

int GoldfishGralloc::unlock(AHardwareBuffer* ahb) { return AHardwareBuffer_unlock(ahb, nullptr); }

uint32_t GoldfishGralloc::getHostHandle(native_handle_t const* handle) {
    const uint32_t INVALID_HOST_HANDLE = 0;

    const cb_handle_t* cb = cb_handle_t::from(handle);
    if (cb) {
        return cb->hostHandle;
    } else {
        return INVALID_HOST_HANDLE;
    }
}

uint32_t GoldfishGralloc::getHostHandle(const AHardwareBuffer* ahb) {
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(ahb);
    return getHostHandle(handle);
}

const native_handle_t* GoldfishGralloc::getNativeHandle(const AHardwareBuffer* ahb) {
    return AHardwareBuffer_getNativeHandle(ahb);
}

int GoldfishGralloc::getFormat(const native_handle_t* handle) {
    return cb_handle_t::from(handle)->format;
}

int GoldfishGralloc::getFormat(const AHardwareBuffer* ahb) {
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(ahb);
    return getFormat(handle);
}

size_t GoldfishGralloc::getAllocatedSize(const native_handle_t* handle) {
    return static_cast<size_t>(cb_handle_t::from(handle)->allocatedSize());
}

size_t GoldfishGralloc::getAllocatedSize(const AHardwareBuffer* ahb) {
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(ahb);
    return getAllocatedSize(handle);
}

int GoldfishGralloc::getId(const AHardwareBuffer* ahb, uint64_t* id) {
#if ANDROID_API_LEVEL >= 31
    return AHardwareBuffer_getId(ahb, id);
#else
    (void)ahb;
    *id = 0;
    return 0;
#endif
}

bool GoldfishGralloc::treatBlobAsImage() { return true; }

}  // namespace gfxstream
