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

#include <cstdint>
#include <memory>
#include <vector>

#include "gfxstream/guest/Gralloc.h"
#include "VirtGpu.h"

namespace gfxstream {

using EGLClientBuffer = void*;

class EmulatedAHardwareBuffer {
   public:
    EmulatedAHardwareBuffer(uint32_t width, uint32_t height, VirtGpuBlobPtr resource);

    ~EmulatedAHardwareBuffer();

    uint32_t getResourceId() const;

    uint32_t getWidth() const;

    uint32_t getHeight() const;

    int getAndroidFormat() const;

    uint32_t getDrmFormat() const;

    AHardwareBuffer* asAHardwareBuffer();

    buffer_handle_t asBufferHandle();

    EGLClientBuffer asEglClientBuffer();

    void acquire();
    void release();

   private:
    uint32_t mRefCount;
    uint32_t mWidth;
    uint32_t mHeight;
    VirtGpuBlobPtr mResource;
};

class EmulatedGralloc : public Gralloc {
   public:
    EmulatedGralloc();

    uint32_t createColorBuffer(void*, int width, int height, uint32_t glFormat) override;

    int allocate(uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
                 AHardwareBuffer** outputAhb) override;

    AHardwareBuffer* allocate(uint32_t width, uint32_t height, uint32_t format);

    void acquire(AHardwareBuffer* ahb) override;
    void release(AHardwareBuffer* ahb) override;

    uint32_t getHostHandle(const native_handle_t* handle) override;
    uint32_t getHostHandle(const AHardwareBuffer* handle) override;

    const native_handle_t* getNativeHandle(const AHardwareBuffer* ahb) override;

    int getFormat(const native_handle_t* handle) override;
    int getFormat(const AHardwareBuffer* handle) override;

    uint32_t getFormatDrmFourcc(const AHardwareBuffer* handle) override;

    size_t getAllocatedSize(const native_handle_t*) override;
    size_t getAllocatedSize(const AHardwareBuffer*) override;

    int getId(const AHardwareBuffer* ahb, uint64_t* id) override;

   private:
    std::vector<std::unique_ptr<EmulatedAHardwareBuffer>> mOwned;
};

}  // namespace gfxstream
