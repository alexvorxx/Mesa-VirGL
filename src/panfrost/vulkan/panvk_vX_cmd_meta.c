/*
 * Copyright © 2021 Collabora Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "panvk_cmd_meta.h"

void
panvk_per_arch(cmd_meta_compute_start)(
   struct panvk_cmd_buffer *cmdbuf,
   struct panvk_cmd_meta_compute_save_ctx *save_ctx)
{
   const struct panvk_descriptor_set *set0 =
      cmdbuf->state.compute.desc_state.sets[0];
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.compute.desc_state.push_sets[0];

   save_ctx->set0 = set0;
   if (push_set0 && push_set0 == set0) {
      save_ctx->push_set0.desc_count = push_set0->desc_count;
      save_ctx->push_set0.descs_dev_addr = push_set0->descs.dev;
      memcpy(save_ctx->push_set0.desc_storage, push_set0->descs.host,
             push_set0->desc_count * PANVK_DESCRIPTOR_SIZE);
   }

   save_ctx->push_constants = cmdbuf->state.push_constants;
   save_ctx->cs.shader = cmdbuf->state.compute.shader;
   save_ctx->cs.desc = cmdbuf->state.compute.cs.desc;
}

void
panvk_per_arch(cmd_meta_compute_end)(
   struct panvk_cmd_buffer *cmdbuf,
   const struct panvk_cmd_meta_compute_save_ctx *save_ctx)
{
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.compute.desc_state.push_sets[0];

   cmdbuf->state.compute.desc_state.sets[0] = save_ctx->set0;
   if (save_ctx->push_set0.desc_count) {
      memcpy(push_set0->descs.host, save_ctx->push_set0.desc_storage,
             save_ctx->push_set0.desc_count * PANVK_DESCRIPTOR_SIZE);
      push_set0->descs.dev = save_ctx->push_set0.descs_dev_addr;
      push_set0->desc_count = save_ctx->push_set0.desc_count;
   }

   if (memcmp(cmdbuf->state.push_constants.data, save_ctx->push_constants.data,
              sizeof(cmdbuf->state.push_constants.data))) {
      cmdbuf->state.push_constants = save_ctx->push_constants;
      cmdbuf->state.compute.push_uniforms = 0;
      cmdbuf->state.gfx.push_uniforms = 0;
   }

   cmdbuf->state.compute.shader = save_ctx->cs.shader;
   cmdbuf->state.compute.cs.desc = save_ctx->cs.desc;
}

void
panvk_per_arch(cmd_meta_gfx_start)(
   struct panvk_cmd_buffer *cmdbuf,
   struct panvk_cmd_meta_graphics_save_ctx *save_ctx)
{
   const struct panvk_descriptor_set *set0 =
      cmdbuf->state.gfx.desc_state.sets[0];
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.gfx.desc_state.push_sets[0];

   save_ctx->set0 = set0;
   if (push_set0 && push_set0 == set0) {
      save_ctx->push_set0.desc_count = push_set0->desc_count;
      save_ctx->push_set0.descs_dev_addr = push_set0->descs.dev;
      memcpy(save_ctx->push_set0.desc_storage, push_set0->descs.host,
             push_set0->desc_count * PANVK_DESCRIPTOR_SIZE);
   }

   save_ctx->push_constants = cmdbuf->state.push_constants;
   save_ctx->fs.shader = cmdbuf->state.gfx.fs.shader;
   save_ctx->fs.desc = cmdbuf->state.gfx.fs.desc;
   save_ctx->fs.rsd = cmdbuf->state.gfx.fs.rsd;
   save_ctx->vs.shader = cmdbuf->state.gfx.vs.shader;
   save_ctx->vs.desc = cmdbuf->state.gfx.vs.desc;
   save_ctx->vs.attribs = cmdbuf->state.gfx.vs.attribs;
   save_ctx->vs.attrib_bufs = cmdbuf->state.gfx.vs.attrib_bufs;

   save_ctx->dyn_state.all.vi = &save_ctx->dyn_state.vi;
   save_ctx->dyn_state.all.ms.sample_locations = &save_ctx->dyn_state.sl;
   vk_dynamic_graphics_state_copy(&save_ctx->dyn_state.all,
                                  &cmdbuf->vk.dynamic_graphics_state);
}

void
panvk_per_arch(cmd_meta_gfx_end)(
   struct panvk_cmd_buffer *cmdbuf,
   const struct panvk_cmd_meta_graphics_save_ctx *save_ctx)
{
   struct panvk_descriptor_set *push_set0 =
      cmdbuf->state.gfx.desc_state.push_sets[0];

   cmdbuf->state.gfx.desc_state.sets[0] = save_ctx->set0;
   if (save_ctx->push_set0.desc_count) {
      memcpy(push_set0->descs.host, save_ctx->push_set0.desc_storage,
             save_ctx->push_set0.desc_count * PANVK_DESCRIPTOR_SIZE);
      push_set0->descs.dev = save_ctx->push_set0.descs_dev_addr;
      push_set0->desc_count = save_ctx->push_set0.desc_count;
   }

   if (memcmp(cmdbuf->state.push_constants.data, save_ctx->push_constants.data,
              sizeof(cmdbuf->state.push_constants.data))) {
      cmdbuf->state.push_constants = save_ctx->push_constants;
      cmdbuf->state.compute.push_uniforms = 0;
      cmdbuf->state.gfx.push_uniforms = 0;
   }

   cmdbuf->state.gfx.fs.shader = save_ctx->fs.shader;
   cmdbuf->state.gfx.fs.desc = save_ctx->fs.desc;
   cmdbuf->state.gfx.fs.rsd = save_ctx->fs.rsd;
   cmdbuf->state.gfx.vs.shader = save_ctx->vs.shader;
   cmdbuf->state.gfx.vs.desc = save_ctx->vs.desc;
   cmdbuf->state.gfx.vs.attribs = save_ctx->vs.attribs;
   cmdbuf->state.gfx.vs.attrib_bufs = save_ctx->vs.attrib_bufs;

   vk_dynamic_graphics_state_copy(&cmdbuf->vk.dynamic_graphics_state,
                                  &save_ctx->dyn_state.all);
}
