// Copyright (C) 2023 The Android Open Source Project
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
#include "wsi_common.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
gfxstream_vk_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char* pName) {
    VK_FROM_HANDLE(gfxstream_vk_physical_device, pdevice, physicalDevice);
    return vk_instance_get_proc_addr_unchecked(&pdevice->instance->vk, pName);
}

VkResult gfxstream_vk_wsi_init(struct gfxstream_vk_physical_device* physical_device) {
    VkResult result = (VkResult)0;

    const struct wsi_device_options options = {.sw_device = false};
    result = wsi_device_init(
        &physical_device->wsi_device, gfxstream_vk_physical_device_to_handle(physical_device),
        gfxstream_vk_wsi_proc_addr, &physical_device->instance->vk.alloc, -1, NULL, &options);
    if (result != VK_SUCCESS) return result;

    // Allow guest-side modifier code paths
    physical_device->wsi_device.supports_modifiers = true;
    // For DRM, uses the buffer-blit path for WSI images
    physical_device->wsi_device.supports_scanout = false;

    physical_device->vk.wsi_device = &physical_device->wsi_device;

    return result;
}

void gfxstream_vk_wsi_finish(struct gfxstream_vk_physical_device* physical_device) {
    physical_device->vk.wsi_device = NULL;
    wsi_device_finish(&physical_device->wsi_device, &physical_device->instance->vk.alloc);
}
