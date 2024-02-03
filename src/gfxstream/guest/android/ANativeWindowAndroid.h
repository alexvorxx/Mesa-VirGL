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

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "gfxstream/guest/ANativeWindow.h"

namespace gfxstream {

class ANativeWindowHelperAndroid : public ANativeWindowHelper {
   public:
    ANativeWindowHelperAndroid() = default;

    bool isValid(EGLNativeWindowType window);
    bool isValid(EGLClientBuffer buffer);

    void acquire(EGLNativeWindowType window);
    void release(EGLNativeWindowType window);

    void acquire(EGLClientBuffer buffer);
    void release(EGLClientBuffer buffer);

    int getConsumerUsage(EGLNativeWindowType window, int* usage);
    void setUsage(EGLNativeWindowType window, int usage);

    int getWidth(EGLNativeWindowType window);
    int getHeight(EGLNativeWindowType window);

    int getWidth(EGLClientBuffer buffer);
    int getHeight(EGLClientBuffer buffer);

    int getFormat(EGLClientBuffer buffer, Gralloc* helper);

    void setSwapInterval(EGLNativeWindowType window, int interval);

    int queueBuffer(EGLNativeWindowType window, EGLClientBuffer buffer, int fence);
    int dequeueBuffer(EGLNativeWindowType window, EGLClientBuffer* buffer, int* fence);
    int cancelBuffer(EGLNativeWindowType window, EGLClientBuffer buffer);

    int getHostHandle(EGLClientBuffer buffer, Gralloc* helper);
};

}  // namespace gfxstream