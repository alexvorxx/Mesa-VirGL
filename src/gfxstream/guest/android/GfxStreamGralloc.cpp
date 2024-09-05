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

#include "gfxstream/guest/GfxStreamGralloc.h"

#if defined(__ANDROID__)

#include <string>

#include "GrallocGoldfish.h"
#include "GrallocMinigbm.h"
#include "android-base/properties.h"

namespace gfxstream {

Gralloc* createPlatformGralloc(int32_t descriptor) {
    const std::string value = android::base::GetProperty("ro.hardware.gralloc", "");
    if (value == "minigbm") {
        auto gralloc = new MinigbmGralloc(descriptor);
        return gralloc;
    }
    return new GoldfishGralloc();
}

}  // namespace gfxstream

#endif
