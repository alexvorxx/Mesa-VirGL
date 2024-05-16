/*
 * Copyright © 2024, Google Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <lvp_private.h>
#include <hardware/gralloc.h>

#if ANDROID_API_LEVEL >= 26
#include <hardware/gralloc1.h>
#endif

#include <hardware/hardware.h>
#include <hardware/hwvulkan.h>

#include "util/libsync.h"
#include "util/os_file.h"
#include "util/libsync.h"

#include "vk_fence.h"
#include "vk_semaphore.h"

static int
lvp_hal_open(const struct hw_module_t *mod,
            const char *id,
            struct hw_device_t **dev);
static int
lvp_hal_close(struct hw_device_t *dev);

static_assert(HWVULKAN_DISPATCH_MAGIC == ICD_LOADER_MAGIC, "");

struct hw_module_methods_t HAL_MODULE_METHODS = {
   .open = lvp_hal_open,
};

PUBLIC struct hwvulkan_module_t HAL_MODULE_INFO_SYM = {
   .common =
     {
       .tag = HARDWARE_MODULE_TAG,
       .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
       .hal_api_version = HARDWARE_MAKE_API_VERSION(1, 0),
       .id = HWVULKAN_HARDWARE_MODULE_ID,
       .name = "Lavapipe Vulkan HAL",
       .author = "Mesa3D",
       .methods = &HAL_MODULE_METHODS,
     },
};

static int
lvp_hal_open(const struct hw_module_t *mod,
            const char *id,
            struct hw_device_t **dev)
{
   assert(mod == &HAL_MODULE_INFO_SYM.common);
   assert(strcmp(id, HWVULKAN_DEVICE_0) == 0);

   hwvulkan_device_t *hal_dev = (hwvulkan_device_t *) malloc(sizeof(*hal_dev));
   if (!hal_dev)
      return -1;

   *hal_dev = (hwvulkan_device_t){
      .common =
        {
          .tag = HARDWARE_DEVICE_TAG,
          .version = HWVULKAN_DEVICE_API_VERSION_0_1,
          .module = &HAL_MODULE_INFO_SYM.common,
          .close = lvp_hal_close,
        },
      .EnumerateInstanceExtensionProperties =
        lvp_EnumerateInstanceExtensionProperties,
      .CreateInstance = lvp_CreateInstance,
      .GetInstanceProcAddr = lvp_GetInstanceProcAddr,
   };

   *dev = &hal_dev->common;
   return 0;
}

static int
lvp_hal_close(struct hw_device_t *dev)
{
   /* hwvulkan.h claims that hw_device_t::close() is never called. */
   return -1;
}

VKAPI_ATTR VkResult VKAPI_CALL
lvp_GetSwapchainGrallocUsageANDROID(VkDevice device_h,
                                   VkFormat format,
                                   VkImageUsageFlags imageUsage,
                                   int *grallocUsage)
{
   *grallocUsage = GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN;

   return VK_SUCCESS;
}

#if ANDROID_API_LEVEL >= 26
VKAPI_ATTR VkResult VKAPI_CALL
lvp_GetSwapchainGrallocUsage2ANDROID(VkDevice device_h,
                                    VkFormat format,
                                    VkImageUsageFlags imageUsage,
                                    VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
                                    uint64_t *grallocConsumerUsage,
                                    uint64_t *grallocProducerUsage)
{
   *grallocConsumerUsage = 0;
   *grallocProducerUsage = GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN | GRALLOC1_PRODUCER_USAGE_CPU_READ_OFTEN;

   return VK_SUCCESS;
}
#endif

VKAPI_ATTR VkResult VKAPI_CALL
lvp_AcquireImageANDROID(VkDevice _device,
                        VkImage image,
                        int nativeFenceFd,
                        VkSemaphore semaphore,
                        VkFence fence)
{
   VK_FROM_HANDLE(vk_device, vk_device, _device);
   VkResult result = VK_SUCCESS;

   if(nativeFenceFd >= 0)
   {
      sync_wait(nativeFenceFd, -1);
      close(nativeFenceFd);
   }

   if(fence != VK_NULL_HANDLE)
   {
      VK_FROM_HANDLE(vk_fence, vk_fence, fence);
      result = vk_sync_signal(vk_device, &vk_fence->permanent, 0);
   }

   if(result == VK_SUCCESS && semaphore != VK_NULL_HANDLE)
   {
      VK_FROM_HANDLE(vk_semaphore, vk_semaphore, semaphore);
      result = vk_sync_signal(vk_device, &vk_semaphore->permanent, 0);
   }

   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
lvp_QueueSignalReleaseImageANDROID(VkQueue _queue,
                                   uint32_t waitSemaphoreCount,
                                   const VkSemaphore *pWaitSemaphores,
                                   VkImage image,
                                   int *pNativeFenceFd)
{
   VK_FROM_HANDLE(vk_queue, queue, _queue);
   struct vk_device *device = queue->base.device;

   device->dispatch_table.QueueWaitIdle(_queue);

   *pNativeFenceFd = -1;

   return VK_SUCCESS;
}
