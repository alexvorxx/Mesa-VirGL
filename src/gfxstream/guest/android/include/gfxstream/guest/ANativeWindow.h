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

#pragma once

#if defined(ANDROID)

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "GfxStreamGralloc.h"

using EGLClientBuffer = void*;

namespace gfxstream {

// Abstraction around libnativewindow to support testing.
class ANativeWindowHelper {
   public:
    virtual ~ANativeWindowHelper() {}

    virtual bool isValid(EGLNativeWindowType window) = 0;
    virtual bool isValid(EGLClientBuffer buffer) = 0;

    virtual void acquire(EGLNativeWindowType window) = 0;
    virtual void release(EGLNativeWindowType window) = 0;

    virtual void acquire(EGLClientBuffer buffer) = 0;
    virtual void release(EGLClientBuffer buffer) = 0;

    virtual int getConsumerUsage(EGLNativeWindowType window, int* usage) = 0;
    virtual void setUsage(EGLNativeWindowType window, int usage) = 0;

    virtual int getWidth(EGLNativeWindowType window) = 0;
    virtual int getHeight(EGLNativeWindowType window) = 0;

    virtual int getWidth(EGLClientBuffer buffer) = 0;
    virtual int getHeight(EGLClientBuffer buffer) = 0;
    virtual int getFormat(EGLClientBuffer buffer, Gralloc* helper) = 0;
    virtual int getHostHandle(EGLClientBuffer buffer, Gralloc* helper) = 0;

    virtual void setSwapInterval(EGLNativeWindowType window, int interval) = 0;

    virtual int queueBuffer(EGLNativeWindowType window, EGLClientBuffer buffer, int fence) = 0;
    virtual int dequeueBuffer(EGLNativeWindowType window, EGLClientBuffer* buffer, int* fence) = 0;
    virtual int cancelBuffer(EGLNativeWindowType window, EGLClientBuffer buffer) = 0;

    virtual EGLNativeWindowType createNativeWindowForTesting(Gralloc* /*gralloc*/,
                                                             uint32_t /*width*/,
                                                             uint32_t /*height*/) {
        return (EGLNativeWindowType)0;
    }
};

ANativeWindowHelper* createPlatformANativeWindowHelper();

}  // namespace gfxstream

#endif  // defined(ANDROID)