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
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

#include "gfxstream/guest/ANativeWindow.h"
#include "GrallocEmulated.h"

namespace gfxstream {

class EmulatedANativeWindow {
   public:
    EmulatedANativeWindow(uint32_t width, uint32_t height, uint32_t format,
                          std::vector<std::unique_ptr<EmulatedAHardwareBuffer>> buffers);

    EGLNativeWindowType asEglNativeWindowType();

    uint32_t getWidth() const;

    uint32_t getHeight() const;

    int getFormat() const;

    int queueBuffer(EGLClientBuffer buffer, int fence);
    int dequeueBuffer(EGLClientBuffer* buffer, int* fence);
    int cancelBuffer(EGLClientBuffer buffer);

    void acquire();
    void release();

   private:
    uint32_t mRefCount;
    uint32_t mWidth;
    uint32_t mHeight;
    uint32_t mFormat;
    std::vector<std::unique_ptr<EmulatedAHardwareBuffer>> mBuffers;

    struct QueuedAHB {
        EmulatedAHardwareBuffer* ahb;
        int fence = -1;
    };
    std::deque<QueuedAHB> mBufferQueue;
};

class EmulatedANativeWindowHelper : public ANativeWindowHelper {
   public:
    bool isValid(EGLNativeWindowType window) override;
    bool isValid(EGLClientBuffer buffer) override;

    void acquire(EGLNativeWindowType window) override;
    void release(EGLNativeWindowType window) override;

    void acquire(EGLClientBuffer buffer) override;
    void release(EGLClientBuffer buffer) override;

    int getConsumerUsage(EGLNativeWindowType window, int* usage) override;
    void setUsage(EGLNativeWindowType window, int usage) override;

    int getWidth(EGLNativeWindowType window) override;
    int getHeight(EGLNativeWindowType window) override;

    int getWidth(EGLClientBuffer buffer) override;
    int getHeight(EGLClientBuffer buffer) override;

    int getFormat(EGLClientBuffer buffer, Gralloc* helper) override;

    void setSwapInterval(EGLNativeWindowType window, int interval) override;

    int queueBuffer(EGLNativeWindowType window, EGLClientBuffer buffer, int fence) override;
    int dequeueBuffer(EGLNativeWindowType window, EGLClientBuffer* buffer, int* fence) override;
    int cancelBuffer(EGLNativeWindowType window, EGLClientBuffer buffer) override;

    int getHostHandle(EGLClientBuffer buffer, Gralloc*) override;

    EGLNativeWindowType createNativeWindowForTesting(Gralloc* gralloc, uint32_t width,
                                                     uint32_t height) override;
};

}  // namespace gfxstream
