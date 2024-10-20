/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * Based on u_format.h which is:
 * Copyright 2009-2010 VMware, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_FORMATS_H
#define RADV_FORMATS_H

#include <assert.h>
#include <util/macros.h>
#include <vulkan/vulkan.h>

#include "amd_family.h"

#include "vk_format.h"

static inline enum pipe_format
radv_format_to_pipe_format(enum VkFormat vkformat)
{
   switch (vkformat) {
   case VK_FORMAT_R10X6_UNORM_PACK16:
   case VK_FORMAT_R12X4_UNORM_PACK16:
      return PIPE_FORMAT_R16_UNORM;
   case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
   case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
      return PIPE_FORMAT_R16G16_UNORM;
   default:
      return vk_format_to_pipe_format(vkformat);
   }
}

/**
 * Return the index of the first non-void channel
 * -1 if no non-void channels
 */
static inline int
vk_format_get_first_non_void_channel(VkFormat format)
{
   return util_format_get_first_non_void_channel(radv_format_to_pipe_format(format));
}

static inline enum pipe_swizzle
radv_swizzle_conv(VkComponentSwizzle component, const unsigned char chan[4], VkComponentSwizzle vk_swiz)
{
   if (vk_swiz == VK_COMPONENT_SWIZZLE_IDENTITY)
      vk_swiz = component;
   switch (vk_swiz) {
   case VK_COMPONENT_SWIZZLE_ZERO:
      return PIPE_SWIZZLE_0;
   case VK_COMPONENT_SWIZZLE_ONE:
      return PIPE_SWIZZLE_1;
   case VK_COMPONENT_SWIZZLE_R:
   case VK_COMPONENT_SWIZZLE_G:
   case VK_COMPONENT_SWIZZLE_B:
   case VK_COMPONENT_SWIZZLE_A:
      return (enum pipe_swizzle)chan[vk_swiz - VK_COMPONENT_SWIZZLE_R];
   default:
      unreachable("Illegal swizzle");
   }
}

static inline void
vk_format_compose_swizzles(const VkComponentMapping *mapping, const unsigned char swz[4], enum pipe_swizzle dst[4])
{
   dst[0] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_R, swz, mapping->r);
   dst[1] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_G, swz, mapping->g);
   dst[2] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_B, swz, mapping->b);
   dst[3] = radv_swizzle_conv(VK_COMPONENT_SWIZZLE_A, swz, mapping->a);
}

static inline bool
vk_format_is_subsampled(VkFormat format)
{
   return util_format_is_subsampled_422(radv_format_to_pipe_format(format));
}

static inline VkFormat
vk_format_no_srgb(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8_SRGB:
      return VK_FORMAT_R8_UNORM;
   case VK_FORMAT_R8G8_SRGB:
      return VK_FORMAT_R8G8_UNORM;
   case VK_FORMAT_R8G8B8_SRGB:
      return VK_FORMAT_R8G8B8_UNORM;
   case VK_FORMAT_B8G8R8_SRGB:
      return VK_FORMAT_B8G8R8_UNORM;
   case VK_FORMAT_R8G8B8A8_SRGB:
      return VK_FORMAT_R8G8B8A8_UNORM;
   case VK_FORMAT_B8G8R8A8_SRGB:
      return VK_FORMAT_B8G8R8A8_UNORM;
   case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
      return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
   case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
   case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
   case VK_FORMAT_BC2_SRGB_BLOCK:
      return VK_FORMAT_BC2_UNORM_BLOCK;
   case VK_FORMAT_BC3_SRGB_BLOCK:
      return VK_FORMAT_BC3_UNORM_BLOCK;
   case VK_FORMAT_BC7_SRGB_BLOCK:
      return VK_FORMAT_BC7_UNORM_BLOCK;
   case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
      return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
   case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
      return VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK;
   case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
      return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
   default:
      assert(!vk_format_is_srgb(format));
      return format;
   }
}

struct radv_physical_device;

uint32_t radv_translate_buffer_numformat(const struct util_format_description *desc, int first_non_void);

uint32_t radv_translate_tex_dataformat(const struct radv_physical_device *pdev,
                                       const struct util_format_description *desc, int first_non_void);

uint32_t radv_translate_tex_numformat(const struct util_format_description *desc, int first_non_void);

bool radv_is_atomic_format_supported(VkFormat format);

bool radv_is_storage_image_format_supported(const struct radv_physical_device *dev, VkFormat format);

bool radv_is_buffer_format_supported(VkFormat format, bool *scaled);

bool radv_is_colorbuffer_format_supported(const struct radv_physical_device *pdev, VkFormat format);

bool radv_is_format_emulated(const struct radv_physical_device *pdev, VkFormat format);

bool radv_format_pack_clear_color(VkFormat format, uint32_t clear_vals[2], VkClearColorValue *value);

bool radv_dcc_formats_compatible(enum amd_gfx_level gfx_level, VkFormat format1, VkFormat format2,
                                 bool *sign_reinterpret);

#endif /* RADV_FORMATS_H */
