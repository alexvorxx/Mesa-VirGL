/*
 * Copyright © 2018 Intel Corporation
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

#ifndef ANV_ANDROID_H
#define ANV_ANDROID_H

#include "util/detect_os.h"

#if DETECT_OS_ANDROID && ANDROID_API_LEVEL >= 26
#include <vndk/hardware_buffer.h>
#endif
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <vulkan/vk_android_native_buffer.h>

struct anv_device_memory;
struct anv_device;
struct anv_image;
struct u_gralloc_buffer_handle;
enum isl_tiling;

VkResult
anv_android_get_tiling(struct anv_device *device,
                       struct u_gralloc_buffer_handle *gr_handle,
                       enum isl_tiling *tiling_out);

VkResult anv_image_init_from_gralloc(struct anv_device *device,
                                     struct anv_image *image,
                                     const VkImageCreateInfo *base_info,
                                     const VkNativeBufferANDROID *gralloc_info);

VkResult anv_image_bind_from_gralloc(struct anv_device *device,
                                     struct anv_image *image,
                                     const VkNativeBufferANDROID *gralloc_info);

unsigned anv_ahb_format_for_vk_format(VkFormat vk_format);

VkResult anv_import_ahw_memory(VkDevice device_h,
                               struct anv_device_memory *mem);

VkResult anv_create_ahw_memory(VkDevice device_h,
                               struct anv_device_memory *mem,
                               const VkMemoryDedicatedAllocateInfo *dedicated_info);
#endif /* ANV_ANDROID_H */
