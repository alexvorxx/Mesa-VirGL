/*
 * Copyright © 2024 Collabora Ltd.
 *
 * Derived from tu_cmd_buffer.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "genxml/gen_macros.h"

#include "panvk_cmd_alloc.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_cmd_meta.h"
#include "panvk_cmd_push_constant.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_meta.h"
#include "panvk_physical_device.h"

#include "pan_desc.h"
#include "pan_encoder.h"
#include "pan_props.h"

#include <vulkan/vulkan_core.h>

static VkResult
prepare_driver_set(struct panvk_cmd_buffer *cmdbuf)
{
   struct panvk_shader_desc_state *cs_desc_state =
      &cmdbuf->state.compute.cs.desc;

   if (cs_desc_state->driver_set.dev_addr)
      return VK_SUCCESS;

   const struct panvk_descriptor_state *desc_state =
      &cmdbuf->state.compute.desc_state;
   const struct panvk_shader *cs = cmdbuf->state.compute.shader;
   uint32_t desc_count = cs->desc_info.dyn_bufs.count + 1;
   struct panfrost_ptr driver_set = panvk_cmd_alloc_dev_mem(
      cmdbuf, desc, desc_count * PANVK_DESCRIPTOR_SIZE, PANVK_DESCRIPTOR_SIZE);
   struct panvk_opaque_desc *descs = driver_set.cpu;

   if (!driver_set.gpu)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Dummy sampler always comes first. */
   pan_pack(&descs[0], SAMPLER, _) {
   }

   panvk_per_arch(cmd_fill_dyn_bufs)(desc_state, cs,
                                     (struct mali_buffer_packed *)(&descs[1]));

   cs_desc_state->driver_set.dev_addr = driver_set.gpu;
   cs_desc_state->driver_set.size = desc_count * PANVK_DESCRIPTOR_SIZE;
   return VK_SUCCESS;
}

static VkResult
prepare_push_uniforms(struct panvk_cmd_buffer *cmdbuf)
{
   cmdbuf->state.compute.push_uniforms = panvk_per_arch(
      cmd_prepare_push_uniforms)(cmdbuf, &cmdbuf->state.compute.sysvals,
                                 sizeof(cmdbuf->state.compute.sysvals));
   return cmdbuf->state.compute.push_uniforms ? VK_SUCCESS
                                              : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatchBase)(VkCommandBuffer commandBuffer,
                                uint32_t baseGroupX, uint32_t baseGroupY,
                                uint32_t baseGroupZ, uint32_t groupCountX,
                                uint32_t groupCountY, uint32_t groupCountZ)
{
   VK_FROM_HANDLE(panvk_cmd_buffer, cmdbuf, commandBuffer);
   const struct panvk_shader *shader = cmdbuf->state.compute.shader;
   VkResult result;

   /* If there's no compute shader, we can skip the dispatch. */
   if (!panvk_priv_mem_dev_addr(shader->spd))
      return;

   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(cmdbuf->vk.base.device->physical);
   struct panvk_descriptor_state *desc_state =
      &cmdbuf->state.compute.desc_state;
   struct panvk_shader_desc_state *cs_desc_state =
      &cmdbuf->state.compute.cs.desc;

   struct panfrost_ptr tsd = panvk_cmd_alloc_desc(cmdbuf, LOCAL_STORAGE);
   if (!tsd.gpu)
      return;

   struct pan_tls_info tlsinfo = {
      .tls.size = shader->info.tls_size,
      .wls.size = shader->info.wls_size,
   };
   unsigned core_id_range;

   panfrost_query_core_count(&phys_dev->kmod.props, &core_id_range);

   if (tlsinfo.wls.size) {
      /* TODO: Clamp WLS instance to some maximum WLS budget. */
      struct pan_compute_dim dim = {groupCountX, groupCountY, groupCountZ};

      tlsinfo.wls.instances = pan_wls_instances(&dim);

      unsigned wls_total_size = pan_wls_adjust_size(tlsinfo.wls.size) *
                                tlsinfo.wls.instances * core_id_range;

      tlsinfo.wls.ptr =
         panvk_cmd_alloc_dev_mem(cmdbuf, tls, wls_total_size, 4096).gpu;
      if (!tlsinfo.wls.ptr)
         return;
   }

   cmdbuf->state.tls.info.tls.size =
      MAX2(shader->info.tls_size, cmdbuf->state.tls.info.tls.size);

   if (!cmdbuf->state.tls.desc.gpu) {
      cmdbuf->state.tls.desc = panvk_cmd_alloc_desc(cmdbuf, LOCAL_STORAGE);
      if (!cmdbuf->state.tls.desc.gpu)
         return;
   }

   GENX(pan_emit_tls)(&tlsinfo, tsd.cpu);

   result = panvk_per_arch(cmd_prepare_push_descs)(
      cmdbuf, desc_state, shader->desc_info.used_set_mask);
   if (result != VK_SUCCESS)
      return;

   struct panvk_compute_sysvals *sysvals = &cmdbuf->state.compute.sysvals;
   sysvals->num_work_groups.x = groupCountX;
   sysvals->num_work_groups.y = groupCountY;
   sysvals->num_work_groups.z = groupCountZ;
   sysvals->local_group_size.x = shader->local_size.x;
   sysvals->local_group_size.y = shader->local_size.y;
   sysvals->local_group_size.z = shader->local_size.z;

   result = prepare_driver_set(cmdbuf);
   if (result != VK_SUCCESS)
      return;

   cmdbuf->state.compute.push_uniforms = 0;
   result = prepare_push_uniforms(cmdbuf);
   if (result != VK_SUCCESS)
      return;

   result = panvk_per_arch(cmd_prepare_shader_res_table)(cmdbuf, desc_state,
                                                         shader, cs_desc_state);
   if (result != VK_SUCCESS)
      return;

   struct cs_builder *b = panvk_get_cs_builder(cmdbuf, PANVK_SUBQUEUE_COMPUTE);
   unsigned task_axis = MALI_TASK_AXIS_X;
   unsigned task_increment = 0;

   /* Copy the global TLS pointer to the per-job TSD. */
   cs_move64_to(b, cs_scratch_reg64(b, 0), tsd.gpu);
   cs_load64_to(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 0), 8);
   cs_wait_slot(b, SB_ID(LS), false);
   cs_move64_to(b, cs_scratch_reg64(b, 0), cmdbuf->state.tls.desc.gpu);
   cs_store64(b, cs_scratch_reg64(b, 2), cs_scratch_reg64(b, 0), 8);
   cs_wait_slot(b, SB_ID(LS), false);

   cs_update_compute_ctx(b) {
      cs_move64_to(b, cs_sr_reg64(b, 0), cs_desc_state->res_table);
      uint32_t push_size = 256 + sizeof(struct panvk_compute_sysvals);
      uint64_t fau_count = DIV_ROUND_UP(push_size, 8);
      mali_ptr fau_ptr =
         cmdbuf->state.compute.push_uniforms | (fau_count << 56);
      cs_move64_to(b, cs_sr_reg64(b, 8), fau_ptr);
      cs_move64_to(b, cs_sr_reg64(b, 16), panvk_priv_mem_dev_addr(shader->spd));
      cs_move64_to(b, cs_sr_reg64(b, 24), tsd.gpu);

      /* Global attribute offset */
      cs_move32_to(b, cs_sr_reg32(b, 32), 0);

      struct mali_compute_size_workgroup_packed wg_size;
      pan_pack(&wg_size, COMPUTE_SIZE_WORKGROUP, cfg) {
         cfg.workgroup_size_x = shader->local_size.x;
         cfg.workgroup_size_y = shader->local_size.y;
         cfg.workgroup_size_z = shader->local_size.z;
         cfg.allow_merging_workgroups = false;
      }
      cs_move32_to(b, cs_sr_reg32(b, 33), wg_size.opaque[0]);
      cs_move32_to(b, cs_sr_reg32(b, 34), baseGroupX * shader->local_size.x);
      cs_move32_to(b, cs_sr_reg32(b, 35), baseGroupY * shader->local_size.y);
      cs_move32_to(b, cs_sr_reg32(b, 36), baseGroupZ * shader->local_size.z);
      cs_move32_to(b, cs_sr_reg32(b, 37), groupCountX);
      cs_move32_to(b, cs_sr_reg32(b, 38), groupCountY);
      cs_move32_to(b, cs_sr_reg32(b, 39), groupCountZ);

      /* Pick the task_axis and task_increment to maximize thread utilization. */
      unsigned threads_per_wg =
         shader->local_size.x * shader->local_size.y * shader->local_size.z;
      unsigned max_thread_cnt = panfrost_compute_max_thread_count(
         &phys_dev->kmod.props, shader->info.work_reg_count);
      unsigned threads_per_task = threads_per_wg;
      unsigned local_size[3] = {
         shader->local_size.x,
         shader->local_size.y,
         shader->local_size.z,
      };

      for (unsigned i = 0; i < 3; i++) {
         if (threads_per_task * local_size[i] >= max_thread_cnt) {
            /* We reached out thread limit, stop at the current axis and
             * calculate the increment so it doesn't exceed the per-core
             * thread capacity.
             */
            task_increment = max_thread_cnt / threads_per_task;
            break;
         } else if (task_axis == MALI_TASK_AXIS_Z) {
            /* We reached the Z axis, and there's still room to stuff more
             * threads. Pick the current axis grid size as our increment
             * as there's no point using something bigger.
             */
            task_increment = local_size[i];
            break;
         }

         threads_per_task *= local_size[i];
         task_axis++;
      }
   }

   assert(task_axis <= MALI_TASK_AXIS_Z);
   assert(task_increment > 0);

   panvk_per_arch(cs_pick_iter_sb)(cmdbuf, PANVK_SUBQUEUE_COMPUTE);

   cs_req_res(b, CS_COMPUTE_RES);
   cs_run_compute(b, task_increment, task_axis, false,
                  cs_shader_res_sel(0, 0, 0, 0));
   cs_req_res(b, 0);

   struct cs_index sync_addr = cs_scratch_reg64(b, 0);
   struct cs_index iter_sb = cs_scratch_reg32(b, 2);
   struct cs_index cmp_scratch = cs_scratch_reg32(b, 3);
   struct cs_index add_val = cs_scratch_reg64(b, 4);

   cs_load_to(b, cs_scratch_reg_tuple(b, 0, 3), cs_subqueue_ctx_reg(b),
              BITFIELD_MASK(3),
              offsetof(struct panvk_cs_subqueue_context, syncobjs));
   cs_wait_slot(b, SB_ID(LS), false);

   cs_add64(b, sync_addr, sync_addr,
            PANVK_SUBQUEUE_COMPUTE * sizeof(struct panvk_cs_sync64));
   cs_move64_to(b, add_val, 1);

   cs_match(b, iter_sb, cmp_scratch) {
#define CASE(x)                                                                \
      cs_case(b, x) {                                                          \
         cs_sync64_add(b, true, MALI_CS_SYNC_SCOPE_CSG,                        \
                       add_val, sync_addr,                                     \
                       cs_defer(SB_WAIT_ITER(x), SB_ID(DEFERRED_SYNC)));       \
         cs_move32_to(b, iter_sb, next_iter_sb(x));                            \
      }

      CASE(0)
      CASE(1)
      CASE(2)
      CASE(3)
      CASE(4)
#undef CASE
   }

   cs_store32(b, iter_sb, cs_subqueue_ctx_reg(b),
              offsetof(struct panvk_cs_subqueue_context, iter_sb));
   cs_wait_slot(b, SB_ID(LS), false);

   ++cmdbuf->state.cs[PANVK_SUBQUEUE_COMPUTE].relative_sync_point;
}

VKAPI_ATTR void VKAPI_CALL
panvk_per_arch(CmdDispatchIndirect)(VkCommandBuffer commandBuffer,
                                    VkBuffer _buffer, VkDeviceSize offset)
{
   panvk_stub();
}
