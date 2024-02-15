// Copyright (C) 2024 The Android Open Source Project
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

#include "gfxstream_vk_entrypoints.h"
#include "gfxstream_vk_private.h"

VkResult gfxstream_vk_wsi_init(struct gfxstream_vk_physical_device* physical_device) {
    return VK_SUCCESS;
}

void gfxstream_vk_wsi_finish(struct gfxstream_vk_physical_device* physical_device) {}
