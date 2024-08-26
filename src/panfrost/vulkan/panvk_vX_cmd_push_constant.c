/*
 * Copyright © 2024 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "panvk_cmd_buffer.h"
#include "panvk_cmd_push_constant.h"
#include "panvk_entrypoints.h"

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdPushConstants2KHR)(
   VkCommandBuffer commandBuffer,
   const VkPushConstantsInfoKHR *pPushConstantsInfo)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);

   if (pPushConstantsInfo->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
      cmdbuf->state.gfx.push_uniforms = 0;

   if (pPushConstantsInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
      cmdbuf->state.compute.push_uniforms = 0;

   memcpy(cmdbuf->state.push_constants.data + pPushConstantsInfo->offset,
          pPushConstantsInfo->pValues, pPushConstantsInfo->size);
}
