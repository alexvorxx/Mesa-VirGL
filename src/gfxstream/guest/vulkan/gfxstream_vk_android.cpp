/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <hardware/hwvulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "HostConnection.h"
#include "ProcessPipe.h"
#include "ResourceTracker.h"
#include "VkEncoder.h"
#include "func_table.h"

namespace {

#define VK_HOST_CONNECTION(ret)                                                    \
    HostConnection* hostCon = HostConnection::getOrCreate(kCapsetGfxStreamVulkan); \
    gfxstream::vk::VkEncoder* vkEnc = hostCon->vkEncoder();                        \
    if (!vkEnc) {                                                                  \
        ALOGE("vulkan: Failed to get Vulkan encoder\n");                           \
        return ret;                                                                \
    }

HostConnection* getConnection(void) {
    auto hostCon = HostConnection::get();
    return hostCon;
}

gfxstream::vk::VkEncoder* getVkEncoder(HostConnection* con) { return con->vkEncoder(); }

gfxstream::vk::ResourceTracker::ThreadingCallbacks threadingCallbacks = {
    .hostConnectionGetFunc = getConnection,
    .vkEncoderGetFunc = getVkEncoder,
};

VkResult SetupInstance(void) {
    HostConnection* hostCon = HostConnection::getOrCreate(kCapsetGfxStreamVulkan);
    if (!hostCon) {
        ALOGE("vulkan: Failed to get host connection\n");
        return VK_ERROR_DEVICE_LOST;
    }

    uint32_t noRenderControlEnc = 0;
    gfxstream::vk::ResourceTracker::get()->setupCaps(noRenderControlEnc);
    // Legacy goldfish path: could be deleted once goldfish not used guest-side.
    if (!noRenderControlEnc) {
        // Implicitly sets up sequence number
        ExtendedRCEncoderContext* rcEnc = hostCon->rcEncoder();
        if (!rcEnc) {
            ALOGE("vulkan: Failed to get renderControl encoder context\n");
            return VK_ERROR_DEVICE_LOST;
        }

        gfxstream::vk::ResourceTracker::get()->setupFeatures(rcEnc->featureInfo_const());
    }

    gfxstream::vk::ResourceTracker::get()->setThreadingCallbacks(threadingCallbacks);
    gfxstream::vk::ResourceTracker::get()->setSeqnoPtr(getSeqnoPtrForProcess());
    gfxstream::vk::VkEncoder* vkEnc = hostCon->vkEncoder();
    if (!vkEnc) {
        ALOGE("vulkan: Failed to get Vulkan encoder\n");
        return VK_ERROR_DEVICE_LOST;
    }

    return VK_SUCCESS;
}

VKAPI_ATTR
VkResult EnumerateInstanceExtensionProperties(const char* layer_name, uint32_t* count,
                                              VkExtensionProperties* properties) {
    VkResult res = SetupInstance();
    if (res != VK_SUCCESS) {
        return res;
    }

    VK_HOST_CONNECTION(VK_ERROR_DEVICE_LOST)

    res = gfxstream::vk::ResourceTracker::get()->on_vkEnumerateInstanceExtensionProperties(
        vkEnc, VK_SUCCESS, layer_name, count, properties);

    return res;
}

VKAPI_ATTR
VkResult CreateInstance(const VkInstanceCreateInfo* create_info,
                        const VkAllocationCallbacks* allocator, VkInstance* out_instance) {
    VkResult res = SetupInstance();
    if (res != VK_SUCCESS) {
        return res;
    }

    VK_HOST_CONNECTION(VK_ERROR_DEVICE_LOST)
    res = vkEnc->vkCreateInstance(create_info, nullptr, out_instance, true /* do lock */);

    return res;
}

PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* name) {
    VK_HOST_CONNECTION(nullptr)

    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        return (PFN_vkVoidFunction)(GetDeviceProcAddr);
    }
    return (
        PFN_vkVoidFunction)(gfxstream::vk::goldfish_vulkan_get_device_proc_address(device, name));
}

VKAPI_ATTR
PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* name) {
    VkResult res = SetupInstance();
    if (res != VK_SUCCESS) {
        return nullptr;
    }

    VK_HOST_CONNECTION(nullptr)

    if (!strcmp(name, "vkEnumerateInstanceExtensionProperties")) {
        return (PFN_vkVoidFunction)EnumerateInstanceExtensionProperties;
    }
    if (!strcmp(name, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)CreateInstance;
    }
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        return (PFN_vkVoidFunction)(GetDeviceProcAddr);
    }
    return (PFN_vkVoidFunction)(gfxstream::vk::goldfish_vulkan_get_instance_proc_address(instance,
                                                                                         name));
}

}  // namespace

int OpenDevice(const hw_module_t* /*module*/, const char* id, hw_device_t** device);
int CloseDevice(struct hw_device_t* /*device*/);

hw_module_methods_t gfxstream_vulkan_module_methods = {
    .open = OpenDevice,
};

__attribute__((visibility("default"))) hwvulkan_module_t HAL_MODULE_INFO_SYM = {
    .common =
        {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = HWVULKAN_HARDWARE_MODULE_ID,
            .name = "Gfxstream Vulkan Driver",
            .author = "The Android Open Source Project",
            .methods = &gfxstream_vulkan_module_methods,
        },
};

hwvulkan_device_t gfxstream_vulkan_device = {
    .common =
        {
            .tag = HARDWARE_DEVICE_TAG,
            .version = HWVULKAN_DEVICE_API_VERSION_0_1,
            .module = &HAL_MODULE_INFO_SYM.common,
            .close = CloseDevice,
        },
    .EnumerateInstanceExtensionProperties = EnumerateInstanceExtensionProperties,
    .CreateInstance = CreateInstance,
    .GetInstanceProcAddr = GetInstanceProcAddr,
};

int OpenDevice(const hw_module_t* /*module*/, const char* id, hw_device_t** device) {
    if (strcmp(id, HWVULKAN_DEVICE_0) == 0) {
        *device = &gfxstream_vulkan_device.common;
        gfxstream::vk::ResourceTracker::get();
        return 0;
    }
    return -ENOENT;
}

int CloseDevice(struct hw_device_t* /*device*/) { return 0; }
