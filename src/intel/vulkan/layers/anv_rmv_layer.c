/*
 * Copyright © 2023 Intel Corporation
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

#include "rmv/vk_rmv_common.h"
#include "rmv/vk_rmv_tokens.h"
#include "anv_private.h"
#include "vk_common_entrypoints.h"

VkResult anv_rmv_QueuePresentKHR(
    VkQueue                                  _queue,
    const VkPresentInfoKHR*                  pPresentInfo)
{
   ANV_FROM_HANDLE(anv_queue, queue, _queue);
   struct anv_device *device = queue->device;

   VkResult res = anv_QueuePresentKHR(_queue, pPresentInfo);
   if ((res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) ||
       !device->vk.memory_trace_data.is_enabled)
      return res;

   vk_rmv_log_misc_token(&device->vk, VK_RMV_MISC_EVENT_TYPE_PRESENT);

   return VK_SUCCESS;
}

VkResult anv_rmv_FlushMappedMemoryRanges(
    VkDevice                                    _device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   VkResult res = anv_FlushMappedMemoryRanges(_device, memoryRangeCount, pMemoryRanges);
   if (res != VK_SUCCESS || !device->vk.memory_trace_data.is_enabled)
      return res;

   vk_rmv_log_misc_token(&device->vk, VK_RMV_MISC_EVENT_TYPE_FLUSH_MAPPED_RANGE);

   return VK_SUCCESS;
}

VkResult anv_rmv_InvalidateMappedMemoryRanges(
    VkDevice                                    _device,
    uint32_t                                    memoryRangeCount,
    const VkMappedMemoryRange*                  pMemoryRanges)
{
   ANV_FROM_HANDLE(anv_device, device, _device);

   VkResult res = anv_InvalidateMappedMemoryRanges(_device, memoryRangeCount, pMemoryRanges);
   if (res != VK_SUCCESS || !device->vk.memory_trace_data.is_enabled)
      return res;

   vk_rmv_log_misc_token(&device->vk, VK_RMV_MISC_EVENT_TYPE_INVALIDATE_RANGES);

   return VK_SUCCESS;
}

VkResult anv_rmv_SetDebugUtilsObjectNameEXT(
    VkDevice                                    _device,
    const VkDebugUtilsObjectNameInfoEXT*        pNameInfo)
{
   assert(pNameInfo->sType == VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT);
   ANV_FROM_HANDLE(anv_device, device, _device);

   VkResult result = vk_common_SetDebugUtilsObjectNameEXT(_device, pNameInfo);
   if (result != VK_SUCCESS || !device->vk.memory_trace_data.is_enabled)
      return result;

   switch (pNameInfo->objectType) {
   /* only name object types we care about */
   case VK_OBJECT_TYPE_BUFFER:
   case VK_OBJECT_TYPE_DEVICE_MEMORY:
   case VK_OBJECT_TYPE_IMAGE:
   case VK_OBJECT_TYPE_EVENT:
   case VK_OBJECT_TYPE_QUERY_POOL:
   case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
   case VK_OBJECT_TYPE_PIPELINE:
      break;
   default:
      return VK_SUCCESS;
   }

   struct vk_object_base *object =
      vk_object_base_from_u64_handle(pNameInfo->objectHandle,
                                     pNameInfo->objectType);

   simple_mtx_lock(&device->vk.memory_trace_data.token_mtx);
   struct vk_rmv_userdata_token token;
   token.name = vk_strdup(&device->vk.alloc, object->object_name,
                          VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   token.resource_id = vk_rmv_get_resource_id_locked(&device->vk, pNameInfo->objectHandle);

   vk_rmv_emit_token(&device->vk.memory_trace_data, VK_RMV_TOKEN_TYPE_USERDATA, &token);
   simple_mtx_unlock(&device->vk.memory_trace_data.token_mtx);

   return VK_SUCCESS;
}
