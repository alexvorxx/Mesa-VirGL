/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVK_SAMPLER_H
#define NVK_SAMPLER_H 1

#include "nvk_private.h"
#include "nvk_physical_device.h"

#include "vk_sampler.h"
#include "vk_ycbcr_conversion.h"

#include "vk_format.h"

struct nvk_sampler {
   struct vk_sampler vk;

   uint8_t plane_count;

   struct {
      uint32_t desc_index;
   } planes[2];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(nvk_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)

struct nvk_sampler_capture {
   struct {
      uint32_t desc_index;
   } planes[2];
};

static void
nvk_sampler_fill_header(const struct nvk_physical_device *pdev,
                        const struct VkSamplerCreateInfo *info,
                        const struct vk_sampler *vk_sampler,
                        uint32_t *samp);

#endif
