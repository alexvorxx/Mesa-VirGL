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

#include "panvk_buffer.h"
#include "panvk_cmd_buffer.h"
#include "panvk_cmd_desc_state.h"
#include "panvk_entrypoints.h"

#include "pan_pool.h"

#include "util/rounding.h"

#include "vk_alloc.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"

void
panvk_per_arch(cmd_desc_state_bind_sets)(
   struct panvk_descriptor_state *desc_state,
   const VkBindDescriptorSetsInfoKHR *info)
{
   unsigned dynoffset_idx = 0;
   for (unsigned i = 0; i < info->descriptorSetCount; ++i) {
      unsigned set_idx = i + info->firstSet;
      VK_FROM_HANDLE(panvk_descriptor_set, set, info->pDescriptorSets[i]);

      /* Invalidate the push set. */
      if (desc_state->sets[set_idx] &&
          desc_state->sets[set_idx] == desc_state->push_sets[set_idx])
         desc_state->push_sets[set_idx]->descs.dev = 0;

      desc_state->sets[set_idx] = set;

      if (!set || !set->layout->dyn_buf_count)
         continue;

      for (unsigned b = 0; b < set->layout->binding_count; b++) {
         VkDescriptorType type = set->layout->bindings[b].type;

         if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC &&
             type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            continue;

         unsigned dyn_buf_idx = set->layout->bindings[b].desc_idx;
         for (unsigned e = 0; e < set->layout->bindings[b].desc_count; e++) {
            desc_state->dyn_buf_offsets[set_idx][dyn_buf_idx++] =
               info->pDynamicOffsets[dynoffset_idx++];
         }
      }
   }

   assert(dynoffset_idx == info->dynamicOffsetCount);
}

struct panvk_descriptor_set *
panvk_per_arch(cmd_push_descriptors)(struct vk_command_buffer *vk_cmdbuf,
                                     struct panvk_descriptor_state *desc_state,
                                     uint32_t set_idx)
{
   struct panvk_cmd_buffer *cmdbuf =
      container_of(vk_cmdbuf, struct panvk_cmd_buffer, vk);
   struct panvk_cmd_pool *pool =
      container_of(cmdbuf->vk.pool, struct panvk_cmd_pool, vk);
   struct panvk_push_set *push_set;

   assert(set_idx < MAX_SETS);

   if (likely(desc_state->push_sets[set_idx])) {
      push_set = container_of(desc_state->push_sets[set_idx],
                              struct panvk_push_set, set);
   } else if (!list_is_empty(&pool->push_sets)) {
      push_set =
         list_first_entry(&pool->push_sets, struct panvk_push_set, base.node);
      list_del(&push_set->base.node);
      list_addtail(&push_set->base.node, &cmdbuf->push_sets);
   } else {
      push_set = vk_zalloc(&pool->vk.alloc, sizeof(*push_set), 8,
                           VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      list_addtail(&push_set->base.node, &cmdbuf->push_sets);
   }

   if (unlikely(!push_set)) {
      vk_command_buffer_set_error(&cmdbuf->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return NULL;
   }

   if (desc_state->push_sets[set_idx] == NULL) {
      desc_state->push_sets[set_idx] = &push_set->set;
      push_set->set.descs.host = push_set->descs;
   }

   struct panvk_descriptor_set *set = desc_state->push_sets[set_idx];

   /* Pushing descriptors replaces whatever sets are bound */
   desc_state->sets[set_idx] = set;
   return set;
}

#if PAN_ARCH <= 7
void
panvk_per_arch(cmd_prepare_dyn_ssbos)(
   struct pan_pool *desc_pool, const struct panvk_descriptor_state *desc_state,
   const struct panvk_shader *shader,
   struct panvk_shader_desc_state *shader_desc_state)
{
   if (!shader || !shader->desc_info.dyn_ssbos.count ||
       shader_desc_state->dyn_ssbos)
      return;

   struct panfrost_ptr ptr = pan_pool_alloc_aligned(
      desc_pool, shader->desc_info.dyn_ssbos.count * PANVK_DESCRIPTOR_SIZE,
      PANVK_DESCRIPTOR_SIZE);

   struct panvk_ssbo_addr *ssbos = ptr.cpu;
   for (uint32_t i = 0; i < shader->desc_info.dyn_ssbos.count; i++) {
      uint32_t src_handle = shader->desc_info.dyn_ssbos.map[i];
      uint32_t set_idx = COPY_DESC_HANDLE_EXTRACT_TABLE(src_handle);
      uint32_t dyn_buf_idx = COPY_DESC_HANDLE_EXTRACT_INDEX(src_handle);
      const struct panvk_descriptor_set *set = desc_state->sets[set_idx];
      const uint32_t dyn_buf_offset =
         desc_state->dyn_buf_offsets[set_idx][dyn_buf_idx];

      assert(set_idx < MAX_SETS);
      assert(set);

      ssbos[i] = (struct panvk_ssbo_addr){
         .base_addr = set->dyn_bufs[dyn_buf_idx].dev_addr + dyn_buf_offset,
         .size = set->dyn_bufs[dyn_buf_idx].size,
      };
   }

   shader_desc_state->dyn_ssbos = ptr.gpu;
}

static void
panvk_cmd_fill_dyn_ubos(const struct panvk_descriptor_state *desc_state,
                        const struct panvk_shader *shader,
                        struct mali_uniform_buffer_packed *ubos,
                        uint32_t ubo_count)
{
   for (uint32_t i = 0; i < shader->desc_info.dyn_ubos.count; i++) {
      uint32_t src_handle = shader->desc_info.dyn_ubos.map[i];
      uint32_t set_idx = COPY_DESC_HANDLE_EXTRACT_TABLE(src_handle);
      uint32_t dyn_buf_idx = COPY_DESC_HANDLE_EXTRACT_INDEX(src_handle);
      uint32_t ubo_idx =
         i + shader->desc_info.others.count[PANVK_BIFROST_DESC_TABLE_UBO];
      const struct panvk_descriptor_set *set = desc_state->sets[set_idx];
      const uint32_t dyn_buf_offset =
         desc_state->dyn_buf_offsets[set_idx][dyn_buf_idx];

      assert(set_idx < MAX_SETS);
      assert(set);
      assert(ubo_idx < ubo_count);

      pan_pack(&ubos[ubo_idx], UNIFORM_BUFFER, cfg) {
         cfg.pointer = set->dyn_bufs[dyn_buf_idx].dev_addr + dyn_buf_offset;
         cfg.entries = DIV_ROUND_UP(set->dyn_bufs[dyn_buf_idx].size, 16);
      }
   }
}

void
panvk_per_arch(cmd_prepare_shader_desc_tables)(
   struct pan_pool *desc_pool, const struct panvk_descriptor_state *desc_state,
   const struct panvk_shader *shader,
   struct panvk_shader_desc_state *shader_desc_state)
{
   if (!shader)
      return;

   for (uint32_t i = 0; i < ARRAY_SIZE(shader->desc_info.others.count); i++) {
      uint32_t desc_count =
         shader->desc_info.others.count[i] +
         (i == PANVK_BIFROST_DESC_TABLE_UBO ? shader->desc_info.dyn_ubos.count
                                            : 0);
      uint32_t desc_size =
         i == PANVK_BIFROST_DESC_TABLE_UBO ? 8 : PANVK_DESCRIPTOR_SIZE;

      if (!desc_count || shader_desc_state->tables[i])
         continue;

      struct panfrost_ptr ptr = pan_pool_alloc_aligned(
         desc_pool, desc_count * desc_size, PANVK_DESCRIPTOR_SIZE);

      shader_desc_state->tables[i] = ptr.gpu;

      if (i == PANVK_BIFROST_DESC_TABLE_UBO)
         panvk_cmd_fill_dyn_ubos(desc_state, shader, ptr.cpu, desc_count);

      /* The image table being actually the attribute table, this is handled
       * separately for vertex shaders. */
      if (i == PANVK_BIFROST_DESC_TABLE_IMG &&
          shader->info.stage != MESA_SHADER_VERTEX) {
         assert(!shader_desc_state->img_attrib_table);

         ptr = pan_pool_alloc_desc_array(desc_pool, desc_count, ATTRIBUTE);
         shader_desc_state->img_attrib_table = ptr.gpu;
      }
   }

   uint32_t tex_count =
      shader->desc_info.others.count[PANVK_BIFROST_DESC_TABLE_TEXTURE];
   uint32_t sampler_count =
      shader->desc_info.others.count[PANVK_BIFROST_DESC_TABLE_SAMPLER];

   if (tex_count && !sampler_count) {
      struct panfrost_ptr sampler = pan_pool_alloc_desc(desc_pool, SAMPLER);

      /* Emit a dummy sampler if we have to. */
      pan_pack(sampler.cpu, SAMPLER, _) {
      }

      shader_desc_state->tables[PANVK_BIFROST_DESC_TABLE_SAMPLER] = sampler.gpu;
   }
}
#else
void
panvk_per_arch(cmd_fill_dyn_bufs)(
   struct pan_pool *desc_pool, const struct panvk_descriptor_state *desc_state,
   const struct panvk_shader *shader, struct mali_buffer_packed *buffers)
{
   if (!shader)
      return;

   for (uint32_t i = 0; i < shader->desc_info.dyn_bufs.count; i++) {
      uint32_t src_handle = shader->desc_info.dyn_bufs.map[i];
      uint32_t set_idx = COPY_DESC_HANDLE_EXTRACT_TABLE(src_handle);
      uint32_t dyn_buf_idx = COPY_DESC_HANDLE_EXTRACT_INDEX(src_handle);
      const struct panvk_descriptor_set *set = desc_state->sets[set_idx];
      const uint32_t dyn_buf_offset =
         desc_state->dyn_buf_offsets[set_idx][dyn_buf_idx];

      assert(set_idx < MAX_SETS);
      assert(set);

      pan_pack(&buffers[i], BUFFER, cfg) {
         cfg.size = set->dyn_bufs[dyn_buf_idx].size;
         cfg.address = set->dyn_bufs[dyn_buf_idx].dev_addr + dyn_buf_offset;
      }
   }
}

void
panvk_per_arch(cmd_prepare_shader_res_table)(
   struct pan_pool *desc_pool, const struct panvk_descriptor_state *desc_state,
   const struct panvk_shader *shader,
   struct panvk_shader_desc_state *shader_desc_state)
{
   if (!shader || shader_desc_state->res_table)
      return;

   uint32_t first_unused_set = util_last_bit(shader->desc_info.used_set_mask);
   uint32_t res_count = 1 + first_unused_set;
   struct panfrost_ptr ptr =
      pan_pool_alloc_desc_array(desc_pool, res_count, RESOURCE);
   struct mali_resource_packed *res_table = ptr.cpu;

   /* First entry is the driver set table, where we store the vertex attributes,
    * the dummy sampler, the dynamic buffers and the vertex buffers. */
   pan_pack(&res_table[0], RESOURCE, cfg) {
      cfg.address = shader_desc_state->driver_set.dev_addr;
      cfg.size = shader_desc_state->driver_set.size;
      cfg.contains_descriptors = cfg.size > 0;
   }

   for (uint32_t i = 0; i < first_unused_set; i++) {
      const struct panvk_descriptor_set *set = desc_state->sets[i];

      pan_pack(&res_table[i + 1], RESOURCE, cfg) {
         if (shader->desc_info.used_set_mask & BITFIELD_BIT(i)) {
            cfg.address = set->descs.dev;
            cfg.contains_descriptors = true;
            cfg.size = set->desc_count * PANVK_DESCRIPTOR_SIZE;
         } else {
            cfg.address = 0;
            cfg.contains_descriptors = false;
            cfg.size = 0;
         }
      }
   }

   shader_desc_state->res_table = ptr.gpu | res_count;
}
#endif

void
panvk_per_arch(cmd_prepare_push_descs)(struct pan_pool *desc_pool,
                                       struct panvk_descriptor_state *desc_state,
                                       uint32_t used_set_mask)
{
   for (unsigned i = 0; i < ARRAY_SIZE(desc_state->push_sets); i++) {
      struct panvk_descriptor_set *push_set = desc_state->push_sets[i];

      if (!(used_set_mask & BITFIELD_BIT(i)) || !push_set ||
          desc_state->sets[i] != push_set || push_set->descs.dev)
         continue;

      push_set->descs.dev = pan_pool_upload_aligned(
         desc_pool, push_set->descs.host,
         push_set->desc_count * PANVK_DESCRIPTOR_SIZE, PANVK_DESCRIPTOR_SIZE);
   }
}
