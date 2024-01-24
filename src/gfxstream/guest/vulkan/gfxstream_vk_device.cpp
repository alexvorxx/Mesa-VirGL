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

#include <errno.h>
#include <string.h>

#include "../vulkan_enc/vk_util.h"
#include "HostConnection.h"
#include "ProcessPipe.h"
#include "ResourceTracker.h"
#include "VkEncoder.h"
#include "gfxstream_vk_entrypoints.h"
#include "gfxstream_vk_private.h"
#include "vk_alloc.h"
#include "vk_device.h"
#include "vk_instance.h"
#include "vk_sync_dummy.h"

#define VK_HOST_CONNECTION(ret)                                                    \
    HostConnection* hostCon = HostConnection::getOrCreate(kCapsetGfxStreamVulkan); \
    gfxstream::vk::VkEncoder* vkEnc = hostCon->vkEncoder();                        \
    if (!vkEnc) {                                                                  \
        ALOGE("vulkan: Failed to get Vulkan encoder\n");                           \
        return ret;                                                                \
    }

namespace {

static bool process_initialized = false;
static uint32_t no_render_control_enc = 0;
static bool instance_extension_table_initialized = false;
static struct vk_instance_extension_table gfxstream_vk_instance_extensions_supported = {0};

static VkResult SetupInstanceForThread() {
    HostConnection* hostCon = HostConnection::getOrCreate(kCapsetGfxStreamVulkan);
    if (!hostCon) {
        ALOGE("vulkan: Failed to get host connection\n");
        return VK_ERROR_DEVICE_LOST;
    }

    // Legacy goldfish path: could be deleted once goldfish not used guest-side.
    if (!no_render_control_enc) {
        // Implicitly sets up sequence number
        ExtendedRCEncoderContext* rcEnc = hostCon->rcEncoder();
        if (!rcEnc) {
            ALOGE("vulkan: Failed to get renderControl encoder context\n");
            return VK_ERROR_DEVICE_LOST;
        }

        // This is technically per-process, but it should not differ
        // per-rcEncoder on a process.
        gfxstream::vk::ResourceTracker::get()->setupFeatures(rcEnc->featureInfo_const());
    }

    gfxstream::vk::VkEncoder* vkEnc = hostCon->vkEncoder();
    if (!vkEnc) {
        ALOGE("vulkan: Failed to get Vulkan encoder\n");
        return VK_ERROR_DEVICE_LOST;
    }

    return VK_SUCCESS;
}

static HostConnection* getConnection() {
    if (!process_initialized) {
        // The process must be initialized prior to this call.
        ALOGE("Call to get a host connection before process initialization!");
        return nullptr;
    }
    if (!HostConnection::isInit()) {
        ALOGW("Call to getConnection when HostConnection is not initialized - treating as normal.");
        if (SetupInstanceForThread() != VK_SUCCESS) {
            ALOGE("Failed to initialize HostConnection! Aborting!");
            return nullptr;
        }
    }
    // This ::get call should already be initialized with the proper caps
    // thanks to SetupInstanceForThread, but this should be made explicit.
    auto hostCon = HostConnection::get();
    return hostCon;
}

static gfxstream::vk::VkEncoder* getVkEncoder(HostConnection* con) { return con->vkEncoder(); }

static VkResult SetupInstanceForProcess() {
    process_initialized = true;
    gfxstream::vk::ResourceTracker::get()->setupCaps(no_render_control_enc);

    // To get the SeqnoPtr, we need the Process info, and for that we need the
    // rcEncoder to be initialized for this thread.
    auto thread_return = SetupInstanceForThread();

    gfxstream::vk::ResourceTracker::get()->setSeqnoPtr(getSeqnoPtrForProcess());
    gfxstream::vk::ResourceTracker::get()->setThreadingCallbacks({
        .hostConnectionGetFunc = getConnection,
        .vkEncoderGetFunc = getVkEncoder,
    });

    return thread_return;
}

// Provided by Mesa components only; never encoded/decoded through gfxstream
static const char* const kMesaOnlyInstanceExtension[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(LINUX_GUEST_BUILD)
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
#endif
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
};

static const char* const kMesaOnlyDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

static bool isMesaOnlyInstanceExtension(const char* name) {
    for (auto mesaExt : kMesaOnlyInstanceExtension) {
        if (!strncmp(mesaExt, name, VK_MAX_EXTENSION_NAME_SIZE)) return true;
    }
    return false;
}

static bool isMesaOnlyDeviceExtension(const char* name) {
    for (auto mesaExt : kMesaOnlyDeviceExtensions) {
        if (!strncmp(mesaExt, name, VK_MAX_EXTENSION_NAME_SIZE)) return true;
    }
    return false;
}

// Filtered extension names for encoding
static std::vector<const char*> filteredInstanceExtensionNames(uint32_t count,
                                                               const char* const* extNames) {
    std::vector<const char*> retList;
    for (uint32_t i = 0; i < count; ++i) {
        auto extName = extNames[i];
        if (!isMesaOnlyInstanceExtension(extName)) {
            retList.push_back(extName);
        }
    }
    return retList;
}

static std::vector<const char*> filteredDeviceExtensionNames(uint32_t count,
                                                             const char* const* extNames) {
    std::vector<const char*> retList;
    for (uint32_t i = 0; i < count; ++i) {
        auto extName = extNames[i];
        if (!isMesaOnlyDeviceExtension(extName)) {
            retList.push_back(extName);
        }
    }
    return retList;
}

static void get_device_extensions(VkPhysicalDevice physDevInternal,
                                  struct vk_device_extension_table* deviceExts) {
    VkResult result = (VkResult)0;
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    auto resources = gfxstream::vk::ResourceTracker::get();
    uint32_t numDeviceExts = 0;
    result = resources->on_vkEnumerateDeviceExtensionProperties(vkEnc, VK_SUCCESS, physDevInternal,
                                                                NULL, &numDeviceExts, NULL);
    if (VK_SUCCESS == result) {
        std::vector<VkExtensionProperties> extProps(numDeviceExts);
        result = resources->on_vkEnumerateDeviceExtensionProperties(
            vkEnc, VK_SUCCESS, physDevInternal, NULL, &numDeviceExts, extProps.data());
        if (VK_SUCCESS == result) {
            // device extensions from gfxstream
            for (uint32_t i = 0; i < numDeviceExts; i++) {
                for (uint32_t j = 0; j < VK_DEVICE_EXTENSION_COUNT; j++) {
                    if (0 == strncmp(extProps[i].extensionName,
                                     vk_device_extensions[j].extensionName,
                                     VK_MAX_EXTENSION_NAME_SIZE)) {
                        deviceExts->extensions[j] = true;
                        break;
                    }
                }
            }
            // device extensions from Mesa
            for (uint32_t j = 0; j < VK_DEVICE_EXTENSION_COUNT; j++) {
                if (isMesaOnlyDeviceExtension(vk_device_extensions[j].extensionName)) {
                    deviceExts->extensions[j] = true;
                    break;
                }
            }
        }
    }
}

static VkResult gfxstream_vk_physical_device_init(
    struct gfxstream_vk_physical_device* physical_device, struct gfxstream_vk_instance* instance,
    VkPhysicalDevice internal_object) {
    struct vk_device_extension_table supported_extensions = {0};
    get_device_extensions(internal_object, &supported_extensions);

    struct vk_physical_device_dispatch_table dispatch_table;
    memset(&dispatch_table, 0, sizeof(struct vk_physical_device_dispatch_table));
    vk_physical_device_dispatch_table_from_entrypoints(
        &dispatch_table, &gfxstream_vk_physical_device_entrypoints, false);
    vk_physical_device_dispatch_table_from_entrypoints(&dispatch_table,
                                                       &wsi_physical_device_entrypoints, false);

    // Initialize the mesa object
    VkResult result = vk_physical_device_init(&physical_device->vk, &instance->vk,
                                              &supported_extensions, NULL, NULL, &dispatch_table);

    if (VK_SUCCESS == result) {
        // Set the gfxstream-internal object
        physical_device->internal_object = internal_object;
        physical_device->instance = instance;
        // Note: Must use dummy_sync for correct sync object path in WSI operations
        physical_device->sync_types[0] = &vk_sync_dummy_type;
        physical_device->sync_types[1] = NULL;
        physical_device->vk.supported_sync_types = physical_device->sync_types;

        result = gfxstream_vk_wsi_init(physical_device);
    }

    return result;
}

static void gfxstream_vk_physical_device_finish(
    struct gfxstream_vk_physical_device* physical_device) {
    gfxstream_vk_wsi_finish(physical_device);

    vk_physical_device_finish(&physical_device->vk);
}

static void gfxstream_vk_destroy_physical_device(struct vk_physical_device* physical_device) {
    gfxstream_vk_physical_device_finish((struct gfxstream_vk_physical_device*)physical_device);
    vk_free(&physical_device->instance->alloc, physical_device);
}

static VkResult gfxstream_vk_enumerate_devices(struct vk_instance* vk_instance) {
    VkResult result = VK_SUCCESS;
    gfxstream_vk_instance* gfxstream_instance = (gfxstream_vk_instance*)vk_instance;
    uint32_t deviceCount = 0;
    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    auto resources = gfxstream::vk::ResourceTracker::get();
    result = resources->on_vkEnumeratePhysicalDevices(
        vkEnc, VK_SUCCESS, gfxstream_instance->internal_object, &deviceCount, NULL);
    if (VK_SUCCESS != result) return result;
    std::vector<VkPhysicalDevice> internal_list(deviceCount);
    result = resources->on_vkEnumeratePhysicalDevices(
        vkEnc, VK_SUCCESS, gfxstream_instance->internal_object, &deviceCount, internal_list.data());

    if (VK_SUCCESS == result) {
        for (uint32_t i = 0; i < deviceCount; i++) {
            struct gfxstream_vk_physical_device* gfxstream_physicalDevice =
                (struct gfxstream_vk_physical_device*)vk_zalloc(
                    &gfxstream_instance->vk.alloc, sizeof(struct gfxstream_vk_physical_device), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
            if (!gfxstream_physicalDevice) {
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                break;
            }
            result = gfxstream_vk_physical_device_init(gfxstream_physicalDevice, gfxstream_instance,
                                                       internal_list[i]);
            if (VK_SUCCESS == result) {
                list_addtail(&gfxstream_physicalDevice->vk.link,
                             &gfxstream_instance->vk.physical_devices.list);
            } else {
                vk_free(&gfxstream_instance->vk.alloc, gfxstream_physicalDevice);
                break;
            }
        }
    }

    return result;
}

static struct vk_instance_extension_table* get_instance_extensions() {
    struct vk_instance_extension_table* const retTablePtr =
        &gfxstream_vk_instance_extensions_supported;
    if (!instance_extension_table_initialized) {
        VkResult result = SetupInstanceForProcess();
        if (VK_SUCCESS == result) {
            VK_HOST_CONNECTION(retTablePtr)
            auto resources = gfxstream::vk::ResourceTracker::get();
            uint32_t numInstanceExts = 0;
            result = resources->on_vkEnumerateInstanceExtensionProperties(vkEnc, VK_SUCCESS, NULL,
                                                                          &numInstanceExts, NULL);
            if (VK_SUCCESS == result) {
                std::vector<VkExtensionProperties> extProps(numInstanceExts);
                result = resources->on_vkEnumerateInstanceExtensionProperties(
                    vkEnc, VK_SUCCESS, NULL, &numInstanceExts, extProps.data());
                if (VK_SUCCESS == result) {
                    // instance extensions from gfxstream
                    for (uint32_t i = 0; i < numInstanceExts; i++) {
                        for (uint32_t j = 0; j < VK_INSTANCE_EXTENSION_COUNT; j++) {
                            if (0 == strncmp(extProps[i].extensionName,
                                             vk_instance_extensions[j].extensionName,
                                             VK_MAX_EXTENSION_NAME_SIZE)) {
                                gfxstream_vk_instance_extensions_supported.extensions[j] = true;
                                break;
                            }
                        }
                    }
                    // instance extensions from Mesa
                    for (uint32_t j = 0; j < VK_INSTANCE_EXTENSION_COUNT; j++) {
                        if (isMesaOnlyInstanceExtension(vk_instance_extensions[j].extensionName)) {
                            gfxstream_vk_instance_extensions_supported.extensions[j] = true;
                        }
                    }
                    instance_extension_table_initialized = true;
                }
            }
        }
    }
    return retTablePtr;
}

}  // namespace

VkResult gfxstream_vk_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                     const VkAllocationCallbacks* pAllocator,
                                     VkInstance* pInstance) {
    AEMU_SCOPED_TRACE("vkCreateInstance");

    struct gfxstream_vk_instance* instance;

    pAllocator = pAllocator ?: vk_default_allocator();
    instance = (struct gfxstream_vk_instance*)vk_zalloc(pAllocator, sizeof(*instance), 8,
                                                        VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    if (NULL == instance) {
        return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
    }

    VkResult result = VK_SUCCESS;
    /* Encoder call */
    {
        ALOGE("calling setup instance internally");
        result = SetupInstanceForProcess();
        if (VK_SUCCESS != result) {
            return vk_error(NULL, result);
        }
        uint32_t initialEnabledExtensionCount = pCreateInfo->enabledExtensionCount;
        const char* const* initialPpEnabledExtensionNames = pCreateInfo->ppEnabledExtensionNames;
        std::vector<const char*> filteredExts = filteredInstanceExtensionNames(
            pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);
        // Temporarily modify createInfo for the encoder call
        VkInstanceCreateInfo* mutableCreateInfo = (VkInstanceCreateInfo*)pCreateInfo;
        mutableCreateInfo->enabledExtensionCount = static_cast<uint32_t>(filteredExts.size());
        mutableCreateInfo->ppEnabledExtensionNames = filteredExts.data();

        VK_HOST_CONNECTION(VK_ERROR_DEVICE_LOST);
        result = vkEnc->vkCreateInstance(pCreateInfo, nullptr, &instance->internal_object,
                                         true /* do lock */);
        if (VK_SUCCESS != result) {
            return vk_error(NULL, result);
        }
        // Revert the createInfo the user-set data
        mutableCreateInfo->enabledExtensionCount = initialEnabledExtensionCount;
        mutableCreateInfo->ppEnabledExtensionNames = initialPpEnabledExtensionNames;
    }

    struct vk_instance_dispatch_table dispatch_table;
    memset(&dispatch_table, 0, sizeof(struct vk_instance_dispatch_table));
    vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &gfxstream_vk_instance_entrypoints,
                                                false);
    vk_instance_dispatch_table_from_entrypoints(&dispatch_table, &wsi_instance_entrypoints, false);

    result = vk_instance_init(&instance->vk, get_instance_extensions(), &dispatch_table,
                              pCreateInfo, pAllocator);

    if (result != VK_SUCCESS) {
        vk_free(pAllocator, instance);
        return vk_error(NULL, result);
    }

    instance->vk.physical_devices.enumerate = gfxstream_vk_enumerate_devices;
    instance->vk.physical_devices.destroy = gfxstream_vk_destroy_physical_device;
    // TODO: instance->vk.physical_devices.try_create_for_drm (?)

    *pInstance = gfxstream_vk_instance_to_handle(instance);
    return VK_SUCCESS;
}

void gfxstream_vk_DestroyInstance(VkInstance _instance, const VkAllocationCallbacks* pAllocator) {
    AEMU_SCOPED_TRACE("vkDestroyInstance");
    if (VK_NULL_HANDLE == _instance) return;

    VK_FROM_HANDLE(gfxstream_vk_instance, instance, _instance);

    VK_HOST_CONNECTION()
    vkEnc->vkDestroyInstance(instance->internal_object, pAllocator, true /* do lock */);

    vk_instance_finish(&instance->vk);
    vk_free(&instance->vk.alloc, instance);

    // To make End2EndTests happy, since now the host connection is statically linked to
    // libvulkan_ranchu.so [separate HostConnections now].
#if defined(END2END_TESTS)
    hostCon->exit();
    processPipeRestart();
#endif
}

VkResult gfxstream_vk_EnumerateInstanceExtensionProperties(const char* pLayerName,
                                                           uint32_t* pPropertyCount,
                                                           VkExtensionProperties* pProperties) {
    AEMU_SCOPED_TRACE("vkvkEnumerateInstanceExtensionProperties");
    (void)pLayerName;

    return vk_enumerate_instance_extension_properties(get_instance_extensions(), pPropertyCount,
                                                      pProperties);
}

VkResult gfxstream_vk_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                         const char* pLayerName,
                                                         uint32_t* pPropertyCount,
                                                         VkExtensionProperties* pProperties) {
    AEMU_SCOPED_TRACE("vkEnumerateDeviceExtensionProperties");
    (void)pLayerName;
    VK_FROM_HANDLE(vk_physical_device, pdevice, physicalDevice);

    VK_OUTARRAY_MAKE_TYPED(VkExtensionProperties, out, pProperties, pPropertyCount);

    for (int i = 0; i < VK_DEVICE_EXTENSION_COUNT; i++) {
        if (!pdevice->supported_extensions.extensions[i]) continue;

        vk_outarray_append_typed(VkExtensionProperties, &out, prop) {
            *prop = vk_device_extensions[i];
        }
    }

    return vk_outarray_status(&out);
}

VkResult gfxstream_vk_CreateDevice(VkPhysicalDevice physicalDevice,
                                   const VkDeviceCreateInfo* pCreateInfo,
                                   const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    AEMU_SCOPED_TRACE("vkCreateDevice");
    VK_FROM_HANDLE(gfxstream_vk_physical_device, gfxstream_physicalDevice, physicalDevice);
    VkResult result = (VkResult)0;

    /*
     * Android's libvulkan implements VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT, but
     * passes it to the underlying driver anyways.  See:
     *
     * https://android-review.googlesource.com/c/platform/hardware/google/gfxstream/+/2839438
     *
     * and associated bugs. Mesa VK runtime also checks this, so we have to filter out before
     * reaches it. vk_find_struct<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT>(..) doesn't
     * work for some reason.
     */
    VkBaseInStructure* extensionCreateInfo = (VkBaseInStructure*)(pCreateInfo->pNext);
    while (extensionCreateInfo) {
        if (extensionCreateInfo->sType ==
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT) {
            auto swapchainMaintenance1Features =
                reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(
                    extensionCreateInfo);
            swapchainMaintenance1Features->swapchainMaintenance1 = VK_FALSE;
        }
        extensionCreateInfo = (VkBaseInStructure*)(extensionCreateInfo->pNext);
    }

    const VkAllocationCallbacks* pMesaAllocator =
        pAllocator ?: &gfxstream_physicalDevice->instance->vk.alloc;
    struct gfxstream_vk_device* gfxstream_device = (struct gfxstream_vk_device*)vk_zalloc(
        pMesaAllocator, sizeof(struct gfxstream_vk_device), 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    result = gfxstream_device ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (VK_SUCCESS == result) {
        uint32_t initialEnabledExtensionCount = pCreateInfo->enabledExtensionCount;
        const char* const* initialPpEnabledExtensionNames = pCreateInfo->ppEnabledExtensionNames;
        std::vector<const char*> filteredExts = filteredDeviceExtensionNames(
            pCreateInfo->enabledExtensionCount, pCreateInfo->ppEnabledExtensionNames);
        // Temporarily modify createInfo for the encoder call
        VkDeviceCreateInfo* mutableCreateInfo = (VkDeviceCreateInfo*)pCreateInfo;
        mutableCreateInfo->enabledExtensionCount = static_cast<uint32_t>(filteredExts.size());
        mutableCreateInfo->ppEnabledExtensionNames = filteredExts.data();

        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        result = vkEnc->vkCreateDevice(gfxstream_physicalDevice->internal_object, pCreateInfo,
                                       pAllocator, &gfxstream_device->internal_object,
                                       true /* do lock */);
        // Revert the createInfo the user-set data
        mutableCreateInfo->enabledExtensionCount = initialEnabledExtensionCount;
        mutableCreateInfo->ppEnabledExtensionNames = initialPpEnabledExtensionNames;
    }
    if (VK_SUCCESS == result) {
        struct vk_device_dispatch_table dispatch_table;
        memset(&dispatch_table, 0, sizeof(struct vk_device_dispatch_table));
        vk_device_dispatch_table_from_entrypoints(&dispatch_table, &gfxstream_vk_device_entrypoints,
                                                  false);
        vk_device_dispatch_table_from_entrypoints(&dispatch_table, &wsi_device_entrypoints, false);

        result = vk_device_init(&gfxstream_device->vk, &gfxstream_physicalDevice->vk,
                                &dispatch_table, pCreateInfo, pMesaAllocator);
    }
    if (VK_SUCCESS == result) {
        gfxstream_device->physical_device = gfxstream_physicalDevice;
        // TODO: Initialize cmd_dispatch for emulated secondary command buffer support?
        gfxstream_device->vk.command_dispatch_table = &gfxstream_device->cmd_dispatch;
        *pDevice = gfxstream_vk_device_to_handle(gfxstream_device);
    } else {
        vk_free(pMesaAllocator, gfxstream_device);
    }

    return result;
}

void gfxstream_vk_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    AEMU_SCOPED_TRACE("vkDestroyDevice");
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    if (VK_NULL_HANDLE == device) return;

    auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
    vkEnc->vkDestroyDevice(gfxstream_device->internal_object, pAllocator, true /* do lock */);

    /* Must destroy device queues manually */
    vk_foreach_queue_safe(queue, &gfxstream_device->vk) {
        vk_queue_finish(queue);
        vk_free(&gfxstream_device->vk.alloc, queue);
    }
    vk_device_finish(&gfxstream_device->vk);
    vk_free(&gfxstream_device->vk.alloc, gfxstream_device);
}

void gfxstream_vk_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex,
                                 VkQueue* pQueue) {
    AEMU_SCOPED_TRACE("vkGetDeviceQueue");
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    struct gfxstream_vk_queue* gfxstream_queue = (struct gfxstream_vk_queue*)vk_zalloc(
        &gfxstream_device->vk.alloc, sizeof(struct gfxstream_vk_queue), 8,
        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    VkResult result = gfxstream_queue ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (VK_SUCCESS == result) {
        VkDeviceQueueCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .queueFamilyIndex = queueFamilyIndex,
            .queueCount = 1,
            .pQueuePriorities = NULL,
        };
        result =
            vk_queue_init(&gfxstream_queue->vk, &gfxstream_device->vk, &createInfo, queueIndex);
    }
    if (VK_SUCCESS == result) {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        vkEnc->vkGetDeviceQueue(gfxstream_device->internal_object, queueFamilyIndex, queueIndex,
                                &gfxstream_queue->internal_object, true /* do lock */);

        gfxstream_queue->device = gfxstream_device;
        *pQueue = gfxstream_vk_queue_to_handle(gfxstream_queue);
    } else {
        *pQueue = VK_NULL_HANDLE;
    }
}

void gfxstream_vk_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo,
                                  VkQueue* pQueue) {
    AEMU_SCOPED_TRACE("vkGetDeviceQueue2");
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    struct gfxstream_vk_queue* gfxstream_queue = (struct gfxstream_vk_queue*)vk_zalloc(
        &gfxstream_device->vk.alloc, sizeof(struct gfxstream_vk_queue), 8,
        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    VkResult result = gfxstream_queue ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (VK_SUCCESS == result) {
        VkDeviceQueueCreateInfo createInfo = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = NULL,
            .flags = pQueueInfo->flags,
            .queueFamilyIndex = pQueueInfo->queueFamilyIndex,
            .queueCount = 1,
            .pQueuePriorities = NULL,
        };
        result = vk_queue_init(&gfxstream_queue->vk, &gfxstream_device->vk, &createInfo,
                               pQueueInfo->queueIndex);
    }
    if (VK_SUCCESS == result) {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        vkEnc->vkGetDeviceQueue2(gfxstream_device->internal_object, pQueueInfo,
                                 &gfxstream_queue->internal_object, true /* do lock */);

        gfxstream_queue->device = gfxstream_device;
        *pQueue = gfxstream_vk_queue_to_handle(gfxstream_queue);
    } else {
        *pQueue = VK_NULL_HANDLE;
    }
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
extern "C" PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

extern "C" PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return gfxstream_vk_GetInstanceProcAddr(instance, pName);
}

/* vk_icd.h does not declare this function, so we declare it here to
 * suppress Wmissing-prototypes.
 */
extern "C" PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion);

extern "C" PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    *pSupportedVersion = std::min(*pSupportedVersion, 3u);
    return VK_SUCCESS;
}

/* With version 4+ of the loader interface the ICD should expose
 * vk_icdGetPhysicalDeviceProcAddr()
 */
extern "C" PUBLIC VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetPhysicalDeviceProcAddr(VkInstance _instance, const char* pName);

PFN_vkVoidFunction vk_icdGetPhysicalDeviceProcAddr(VkInstance _instance, const char* pName) {
    VK_FROM_HANDLE(gfxstream_vk_instance, instance, _instance);

    return vk_instance_get_physical_device_proc_addr(&instance->vk, pName);
}

PFN_vkVoidFunction gfxstream_vk_GetInstanceProcAddr(VkInstance _instance, const char* pName) {
    VK_FROM_HANDLE(gfxstream_vk_instance, instance, _instance);
    return vk_instance_get_proc_addr(&instance->vk, &gfxstream_vk_instance_entrypoints, pName);
}

PFN_vkVoidFunction gfxstream_vk_GetDeviceProcAddr(VkDevice _device, const char* pName) {
    AEMU_SCOPED_TRACE("vkGetDeviceProcAddr");
    VK_FROM_HANDLE(gfxstream_vk_device, device, _device);
    return vk_device_get_proc_addr(&device->vk, pName);
}

VkResult gfxstream_vk_AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo,
                                     const VkAllocationCallbacks* pAllocator,
                                     VkDeviceMemory* pMemory) {
    AEMU_SCOPED_TRACE("vkAllocateMemory");
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    VkResult vkAllocateMemory_VkResult_return = (VkResult)0;
    struct gfxstream_vk_device_memory* gfxstream_pMemory =
        (struct gfxstream_vk_device_memory*)vk_device_memory_create(
            (vk_device*)gfxstream_device, pAllocateInfo, pAllocator,
            sizeof(struct gfxstream_vk_device_memory));
    /* VkMemoryDedicatedAllocateInfo */
    VkMemoryDedicatedAllocateInfo* dedicatedAllocInfoPtr =
        (VkMemoryDedicatedAllocateInfo*)vk_find_struct<VkMemoryDedicatedAllocateInfo>(
            pAllocateInfo);
    if (dedicatedAllocInfoPtr) {
        if (dedicatedAllocInfoPtr->buffer) {
            VK_FROM_HANDLE(gfxstream_vk_buffer, gfxstream_buffer, dedicatedAllocInfoPtr->buffer);
            dedicatedAllocInfoPtr->buffer = gfxstream_buffer->internal_object;
        }
        if (dedicatedAllocInfoPtr->image) {
            VK_FROM_HANDLE(gfxstream_vk_image, gfxstream_image, dedicatedAllocInfoPtr->image);
            dedicatedAllocInfoPtr->image = gfxstream_image->internal_object;
        }
    }
    vkAllocateMemory_VkResult_return = gfxstream_pMemory ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (VK_SUCCESS == vkAllocateMemory_VkResult_return) {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        auto resources = gfxstream::vk::ResourceTracker::get();
        vkAllocateMemory_VkResult_return = resources->on_vkAllocateMemory(
            vkEnc, VK_SUCCESS, gfxstream_device->internal_object, pAllocateInfo, pAllocator,
            &gfxstream_pMemory->internal_object);
    }
    *pMemory = gfxstream_vk_device_memory_to_handle(gfxstream_pMemory);
    return vkAllocateMemory_VkResult_return;
}

void gfxstream_vk_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                     const VkRenderPassBeginInfo* pRenderPassBegin,
                                     VkSubpassContents contents) {
    AEMU_SCOPED_TRACE("vkCmdBeginRenderPass");
    VK_FROM_HANDLE(gfxstream_vk_command_buffer, gfxstream_commandBuffer, commandBuffer);
    {
        auto vkEnc = gfxstream::vk::ResourceTracker::getCommandBufferEncoder(
            gfxstream_commandBuffer->internal_object);
        VkRenderPassBeginInfo internal_pRenderPassBegin = vk_make_orphan_copy(*pRenderPassBegin);
        vk_struct_chain_iterator structChainIter =
            vk_make_chain_iterator(&internal_pRenderPassBegin);
        /* VkRenderPassBeginInfo::renderPass */
        VK_FROM_HANDLE(gfxstream_vk_render_pass, gfxstream_renderPass,
                       internal_pRenderPassBegin.renderPass);
        internal_pRenderPassBegin.renderPass = gfxstream_renderPass->internal_object;
        /* VkRenderPassBeginInfo::framebuffer */
        VK_FROM_HANDLE(gfxstream_vk_framebuffer, gfxstream_framebuffer,
                       internal_pRenderPassBegin.framebuffer);
        internal_pRenderPassBegin.framebuffer = gfxstream_framebuffer->internal_object;
        /* pNext = VkRenderPassAttachmentBeginInfo */
        std::vector<VkImageView> internal_pAttachments;
        VkRenderPassAttachmentBeginInfo internal_renderPassAttachmentBeginInfo;
        VkRenderPassAttachmentBeginInfo* pRenderPassAttachmentBeginInfo =
            (VkRenderPassAttachmentBeginInfo*)vk_find_struct<VkRenderPassAttachmentBeginInfo>(
                pRenderPassBegin);
        if (pRenderPassAttachmentBeginInfo) {
            internal_renderPassAttachmentBeginInfo = *pRenderPassAttachmentBeginInfo;
            /* VkRenderPassAttachmentBeginInfo::pAttachments */
            internal_pAttachments.reserve(internal_renderPassAttachmentBeginInfo.attachmentCount);
            for (uint32_t i = 0; i < internal_renderPassAttachmentBeginInfo.attachmentCount; i++) {
                VK_FROM_HANDLE(gfxstream_vk_image_view, gfxstream_image_view,
                               internal_renderPassAttachmentBeginInfo.pAttachments[i]);
                internal_pAttachments[i] = gfxstream_image_view->internal_object;
            }
            internal_renderPassAttachmentBeginInfo.pAttachments = internal_pAttachments.data();
            vk_append_struct(&structChainIter, &internal_renderPassAttachmentBeginInfo);
        }
        vkEnc->vkCmdBeginRenderPass(gfxstream_commandBuffer->internal_object,
                                    &internal_pRenderPassBegin, contents, true /* do lock */);
    }
}

void gfxstream_vk_CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                                         const VkRenderPassBeginInfo* pRenderPassBegin,
                                         const VkSubpassBeginInfo* pSubpassBeginInfo) {
    AEMU_SCOPED_TRACE("vkCmdBeginRenderPass2KHR");
    VK_FROM_HANDLE(gfxstream_vk_command_buffer, gfxstream_commandBuffer, commandBuffer);
    {
        auto vkEnc = gfxstream::vk::ResourceTracker::getCommandBufferEncoder(
            gfxstream_commandBuffer->internal_object);
        VkRenderPassBeginInfo internal_pRenderPassBegin = vk_make_orphan_copy(*pRenderPassBegin);
        vk_struct_chain_iterator structChainIter =
            vk_make_chain_iterator(&internal_pRenderPassBegin);
        /* VkRenderPassBeginInfo::renderPass */
        VK_FROM_HANDLE(gfxstream_vk_render_pass, gfxstream_renderPass,
                       internal_pRenderPassBegin.renderPass);
        internal_pRenderPassBegin.renderPass = gfxstream_renderPass->internal_object;
        /* VkRenderPassBeginInfo::framebuffer */
        VK_FROM_HANDLE(gfxstream_vk_framebuffer, gfxstream_framebuffer,
                       internal_pRenderPassBegin.framebuffer);
        internal_pRenderPassBegin.framebuffer = gfxstream_framebuffer->internal_object;
        /* pNext = VkRenderPassAttachmentBeginInfo */
        std::vector<VkImageView> internal_pAttachments;
        VkRenderPassAttachmentBeginInfo internal_renderPassAttachmentBeginInfo;
        VkRenderPassAttachmentBeginInfo* pRenderPassAttachmentBeginInfo =
            (VkRenderPassAttachmentBeginInfo*)vk_find_struct<VkRenderPassAttachmentBeginInfo>(
                pRenderPassBegin);
        if (pRenderPassAttachmentBeginInfo) {
            internal_renderPassAttachmentBeginInfo = *pRenderPassAttachmentBeginInfo;
            /* VkRenderPassAttachmentBeginInfo::pAttachments */
            internal_pAttachments.reserve(internal_renderPassAttachmentBeginInfo.attachmentCount);
            for (uint32_t i = 0; i < internal_renderPassAttachmentBeginInfo.attachmentCount; i++) {
                VK_FROM_HANDLE(gfxstream_vk_image_view, gfxstream_image_view,
                               internal_renderPassAttachmentBeginInfo.pAttachments[i]);
                internal_pAttachments[i] = gfxstream_image_view->internal_object;
            }
            internal_renderPassAttachmentBeginInfo.pAttachments = internal_pAttachments.data();
            vk_append_struct(&structChainIter, &internal_renderPassAttachmentBeginInfo);
        }
        vkEnc->vkCmdBeginRenderPass2KHR(gfxstream_commandBuffer->internal_object,
                                        &internal_pRenderPassBegin, pSubpassBeginInfo,
                                        true /* do lock */);
    }
}

VkResult gfxstream_vk_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR* pGetFdInfo,
                                     int* pFd) {
    AEMU_SCOPED_TRACE("vkGetMemoryFdKHR");
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    VkResult vkGetMemoryFdKHR_VkResult_return = (VkResult)0;

    {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        std::vector<VkMemoryGetFdInfoKHR> internal_pGetFdInfo(1);
        for (uint32_t i = 0; i < 1; ++i) {
            internal_pGetFdInfo[i] = pGetFdInfo[i];
            /* VkMemoryGetFdInfoKHR::memory */
            VK_FROM_HANDLE(gfxstream_vk_device_memory, gfxstream_memory,
                           internal_pGetFdInfo[i].memory);
            internal_pGetFdInfo[i].memory = gfxstream_memory->internal_object;
        }
        auto resources = gfxstream::vk::ResourceTracker::get();
        vkGetMemoryFdKHR_VkResult_return = resources->on_vkGetMemoryFdKHR(
            vkEnc, VK_SUCCESS, gfxstream_device->internal_object, internal_pGetFdInfo.data(), pFd);
    }
    return vkGetMemoryFdKHR_VkResult_return;
}

VkResult gfxstream_vk_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
                                                       VkLayerProperties* pProperties) {
    AEMU_SCOPED_TRACE("vkEnumerateInstanceLayerProperties");
    auto result = SetupInstanceForProcess();
    if (VK_SUCCESS != result) {
        return vk_error(NULL, result);
    }

    VkResult vkEnumerateInstanceLayerProperties_VkResult_return = (VkResult)0;
    {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        vkEnumerateInstanceLayerProperties_VkResult_return =
            vkEnc->vkEnumerateInstanceLayerProperties(pPropertyCount, pProperties,
                                                      true /* do lock */);
    }
    return vkEnumerateInstanceLayerProperties_VkResult_return;
}

VkResult gfxstream_vk_EnumerateInstanceVersion(uint32_t* pApiVersion) {
    AEMU_SCOPED_TRACE("vkEnumerateInstanceVersion");
    auto result = SetupInstanceForProcess();
    if (VK_SUCCESS != result) {
        return vk_error(NULL, result);
    }

    VkResult vkEnumerateInstanceVersion_VkResult_return = (VkResult)0;
    {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        vkEnumerateInstanceVersion_VkResult_return =
            vkEnc->vkEnumerateInstanceVersion(pApiVersion, true /* do lock */);
    }
    return vkEnumerateInstanceVersion_VkResult_return;
}

VkResult gfxstream_vk_CreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
                                             uint32_t createInfoCount,
                                             const VkComputePipelineCreateInfo* pCreateInfos,
                                             const VkAllocationCallbacks* pAllocator,
                                             VkPipeline* pPipelines) {
    AEMU_SCOPED_TRACE("vkCreateComputePipelines");
    VkResult vkCreateComputePipelines_VkResult_return = (VkResult)0;
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    VK_FROM_HANDLE(gfxstream_vk_pipeline_cache, gfxstream_pipelineCache, pipelineCache);
    struct gfxstream_vk_pipeline* gfxstream_pPipelines = (gfxstream_vk_pipeline*)vk_object_zalloc(
        &gfxstream_device->vk, pAllocator, sizeof(gfxstream_vk_pipeline), VK_OBJECT_TYPE_PIPELINE);
    vkCreateComputePipelines_VkResult_return =
        gfxstream_pPipelines ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (VK_SUCCESS == vkCreateComputePipelines_VkResult_return) {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        std::vector<VkComputePipelineCreateInfo> internal_pCreateInfos(createInfoCount);
        std::vector<VkPipelineShaderStageCreateInfo> internal_VkComputePipelineCreateInfo_stage(createInfoCount);
        for (uint32_t i = 0; i < createInfoCount; ++i) {
            internal_pCreateInfos[i] = pCreateInfos[i];
            /* VkComputePipelineCreateInfo::stage */
            {
                internal_VkComputePipelineCreateInfo_stage[i] = internal_pCreateInfos[i].stage;
                /* VkPipelineShaderStageCreateInfo::module */
                if (internal_VkComputePipelineCreateInfo_stage[i].module) {
                    VK_FROM_HANDLE(gfxstream_vk_shader_module, gfxstream_module,
                                   internal_VkComputePipelineCreateInfo_stage[i].module);
                    internal_VkComputePipelineCreateInfo_stage[i].module =
                        gfxstream_module->internal_object;
                }
                internal_pCreateInfos[i].stage = internal_VkComputePipelineCreateInfo_stage[i];
            }
            /* VkComputePipelineCreateInfo::layout */
            VK_FROM_HANDLE(gfxstream_vk_pipeline_layout, gfxstream_layout,
                           internal_pCreateInfos[i].layout);
            internal_pCreateInfos[i].layout = gfxstream_layout->internal_object;
            /* VkComputePipelineCreateInfo::basePipelineHandle */
            if (internal_pCreateInfos[i].basePipelineHandle) {
                VK_FROM_HANDLE(gfxstream_vk_pipeline, gfxstream_basePipelineHandle,
                               internal_pCreateInfos[i].basePipelineHandle);
                internal_pCreateInfos[i].basePipelineHandle =
                    gfxstream_basePipelineHandle->internal_object;
            }
        }
        vkCreateComputePipelines_VkResult_return = vkEnc->vkCreateComputePipelines(
            gfxstream_device->internal_object,
            gfxstream_pipelineCache ? gfxstream_pipelineCache->internal_object : VK_NULL_HANDLE,
            createInfoCount, internal_pCreateInfos.data(), pAllocator,
            &gfxstream_pPipelines->internal_object, true /* do lock */);
    }
    *pPipelines = gfxstream_vk_pipeline_to_handle(gfxstream_pPipelines);
    return vkCreateComputePipelines_VkResult_return;
}

struct DescriptorSetTransformStorage {
    std::vector<std::vector<VkDescriptorImageInfo>> imageInfos;
    std::vector<std::vector<VkDescriptorBufferInfo>> bufferInfos;
    std::vector<std::vector<VkBufferView>> texelBuffers;
};

static std::vector<VkWriteDescriptorSet> transformDescriptorSetList(
    const VkWriteDescriptorSet* pDescriptorSets,
    uint32_t descriptorSetCount,
    DescriptorSetTransformStorage& storage) {
    std::vector<VkWriteDescriptorSet> outDescriptorSets(descriptorSetCount);
    for (uint32_t i = 0; i < descriptorSetCount; ++i) {
        const auto& srcDescriptorSet = pDescriptorSets[i];
        const uint32_t descriptorCount = srcDescriptorSet.descriptorCount;

        VkWriteDescriptorSet& outDescriptorSet = outDescriptorSets[i];
        outDescriptorSet = srcDescriptorSet;

        storage.imageInfos.push_back(std::vector<VkDescriptorImageInfo>());
        storage.imageInfos[i].reserve(descriptorCount);
        memset(&storage.imageInfos[i][0], 0, sizeof(VkDescriptorImageInfo) * descriptorCount);
        for (uint32_t j = 0; j < descriptorCount; ++j) {
            const auto* srcImageInfo = srcDescriptorSet.pImageInfo;
            if (srcImageInfo) {
                storage.imageInfos[i][j] = srcImageInfo[j];
                storage.imageInfos[i][j].imageView = VK_NULL_HANDLE;
                if (vk_descriptor_type_has_image_view(srcDescriptorSet.descriptorType) &&
                    srcImageInfo[j].imageView) {
                    VK_FROM_HANDLE(gfxstream_vk_image_view, gfxstreamImageView,
                                   srcImageInfo[j].imageView);
                    storage.imageInfos[i][j].imageView = gfxstreamImageView->internal_object;
                }
            }
        }
        outDescriptorSet.pImageInfo = storage.imageInfos[i].data();

        storage.bufferInfos.push_back(std::vector<VkDescriptorBufferInfo>());
        storage.bufferInfos[i].reserve(descriptorCount);
        memset(&storage.bufferInfos[i][0], 0, sizeof(VkDescriptorBufferInfo) * descriptorCount);
        for (uint32_t j = 0; j < descriptorCount; ++j) {
            const auto* srcBufferInfo = srcDescriptorSet.pBufferInfo;
            if (srcBufferInfo) {
                storage.bufferInfos[i][j] = srcBufferInfo[j];
                storage.bufferInfos[i][j].buffer = VK_NULL_HANDLE;
                if (vk_descriptor_type_has_descriptor_buffer(srcDescriptorSet.descriptorType) &&
                    srcBufferInfo[j].buffer) {
                    VK_FROM_HANDLE(gfxstream_vk_buffer, gfxstreamBuffer, srcBufferInfo[j].buffer);
                    storage.bufferInfos[i][j].buffer = gfxstreamBuffer->internal_object;
                }
            }
        }
        outDescriptorSet.pBufferInfo = storage.bufferInfos[i].data();

        storage.texelBuffers.push_back(std::vector<VkBufferView>());
        storage.texelBuffers[i].reserve(descriptorCount);
        memset(&storage.texelBuffers[i][0], 0, sizeof(VkBufferView) * descriptorCount);
        for (uint32_t j = 0; j < descriptorCount; ++j) {
            const auto* srcBufferView = srcDescriptorSet.pTexelBufferView;
            if (vk_descriptor_type_has_texel_buffer(srcDescriptorSet.descriptorType) &&
                srcBufferView) {
                VK_FROM_HANDLE(gfxstream_vk_buffer_view, gfxstreamBufferView, srcBufferView[j]);
                storage.texelBuffers[i][j] =
                    gfxstreamBufferView->internal_object;
            }
        }
        outDescriptorSet.pTexelBufferView = storage.texelBuffers[i].data();
    }
    return outDescriptorSets;
}

void gfxstream_vk_UpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                                       const VkWriteDescriptorSet* pDescriptorWrites,
                                       uint32_t descriptorCopyCount,
                                       const VkCopyDescriptorSet* pDescriptorCopies) {
    AEMU_SCOPED_TRACE("vkUpdateDescriptorSets");
    VK_FROM_HANDLE(gfxstream_vk_device, gfxstream_device, device);
    {
        auto vkEnc = gfxstream::vk::ResourceTracker::getThreadLocalEncoder();
        DescriptorSetTransformStorage descriptorSetTransformStorage;
        std::vector<VkWriteDescriptorSet> internal_pDescriptorWrites =
            transformDescriptorSetList(pDescriptorWrites, descriptorWriteCount,
                                       descriptorSetTransformStorage);
        auto resources = gfxstream::vk::ResourceTracker::get();
        resources->on_vkUpdateDescriptorSets(
            vkEnc, gfxstream_device->internal_object, descriptorWriteCount,
            internal_pDescriptorWrites.data(), descriptorCopyCount, pDescriptorCopies);
    }
}

void gfxstream_vk_QueueCommitDescriptorSetUpdatesGOOGLE(
    VkQueue queue, uint32_t descriptorPoolCount, const VkDescriptorPool* pDescriptorPools,
    uint32_t descriptorSetCount, const VkDescriptorSetLayout* pSetLayouts,
    const uint64_t* pDescriptorSetPoolIds, const uint32_t* pDescriptorSetWhichPool,
    const uint32_t* pDescriptorSetPendingAllocation,
    const uint32_t* pDescriptorWriteStartingIndices, uint32_t pendingDescriptorWriteCount,
    const VkWriteDescriptorSet* pPendingDescriptorWrites) {
    AEMU_SCOPED_TRACE("vkQueueCommitDescriptorSetUpdatesGOOGLE");
    VK_FROM_HANDLE(gfxstream_vk_queue, gfxstream_queue, queue);
    {
        auto vkEnc =
            gfxstream::vk::ResourceTracker::getQueueEncoder(gfxstream_queue->internal_object);
        std::vector<VkDescriptorPool> internal_pDescriptorPools(descriptorPoolCount);
        for (uint32_t i = 0; i < descriptorPoolCount; ++i) {
            VK_FROM_HANDLE(gfxstream_vk_descriptor_pool, gfxstream_pDescriptorPools,
                           pDescriptorPools[i]);
            internal_pDescriptorPools[i] = gfxstream_pDescriptorPools->internal_object;
        }
        std::vector<VkDescriptorSetLayout> internal_pSetLayouts(descriptorSetCount);
        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            VK_FROM_HANDLE(gfxstream_vk_descriptor_set_layout, gfxstream_pSetLayouts,
                           pSetLayouts[i]);
            internal_pSetLayouts[i] = gfxstream_pSetLayouts->internal_object;
        }
        DescriptorSetTransformStorage descriptorSetTransformStorage;
        std::vector<VkWriteDescriptorSet> internal_pPendingDescriptorWrites =
            transformDescriptorSetList(pPendingDescriptorWrites, pendingDescriptorWriteCount,
                                       descriptorSetTransformStorage);
        vkEnc->vkQueueCommitDescriptorSetUpdatesGOOGLE(
            gfxstream_queue->internal_object, descriptorPoolCount, internal_pDescriptorPools.data(),
            descriptorSetCount, internal_pSetLayouts.data(), pDescriptorSetPoolIds,
            pDescriptorSetWhichPool, pDescriptorSetPendingAllocation,
            pDescriptorWriteStartingIndices, pendingDescriptorWriteCount,
            internal_pPendingDescriptorWrites.data(), true /* do lock */);
    }
}
