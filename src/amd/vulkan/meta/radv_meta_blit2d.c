/*
 * Copyright © 2016 Red Hat
 *
 * based on anv driver:
 * Copyright © 2016 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir/nir_builder.h"
#include "radv_entrypoints.h"
#include "radv_meta.h"
#include "vk_common_entrypoints.h"
#include "vk_format.h"
#include "vk_shader_module.h"

enum blit2d_src_type {
   BLIT2D_SRC_TYPE_IMAGE,
   BLIT2D_SRC_TYPE_IMAGE_3D,
   BLIT2D_SRC_TYPE_BUFFER,
   BLIT2D_NUM_SRC_TYPES,
};

static VkResult get_color_pipeline(struct radv_device *device, enum blit2d_src_type src_type, VkFormat format,
                                   uint32_t log2_samples, VkPipeline *pipeline_out);

static VkResult get_depth_only_pipeline(struct radv_device *device, enum blit2d_src_type src_type,
                                        uint32_t log2_samples, VkPipeline *pipeline_out);

static VkResult get_stencil_only_pipeline(struct radv_device *device, enum blit2d_src_type src_type,
                                          uint32_t log2_samples, VkPipeline *pipeline_out);

static void
create_iview(struct radv_cmd_buffer *cmd_buffer, struct radv_meta_blit2d_surf *surf, struct radv_image_view *iview,
             VkFormat depth_format, VkImageAspectFlagBits aspects)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   VkFormat format;

   if (depth_format)
      format = depth_format;
   else
      format = surf->format;

   radv_image_view_init(iview, device,
                        &(VkImageViewCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .image = radv_image_to_handle(surf->image),
                           .viewType = radv_meta_get_view_type(surf->image),
                           .format = format,
                           .subresourceRange = {.aspectMask = aspects,
                                                .baseMipLevel = surf->level,
                                                .levelCount = 1,
                                                .baseArrayLayer = surf->layer,
                                                .layerCount = 1},
                        },
                        &(struct radv_image_view_extra_create_info){.disable_dcc_mrt = surf->disable_compression});
}

static void
create_bview(struct radv_cmd_buffer *cmd_buffer, struct radv_meta_blit2d_buffer *src, struct radv_buffer_view *bview,
             VkFormat depth_format)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   VkFormat format;

   if (depth_format)
      format = depth_format;
   else
      format = src->format;
   radv_buffer_view_init(bview, device,
                         &(VkBufferViewCreateInfo){
                            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                            .flags = 0,
                            .buffer = radv_buffer_to_handle(src->buffer),
                            .format = format,
                            .offset = src->offset,
                            .range = VK_WHOLE_SIZE,
                         });
}

struct blit2d_src_temps {
   struct radv_image_view iview;
   struct radv_buffer_view bview;
};

static void
blit2d_bind_src(struct radv_cmd_buffer *cmd_buffer, struct radv_meta_blit2d_surf *src_img,
                struct radv_meta_blit2d_buffer *src_buf, struct blit2d_src_temps *tmp, enum blit2d_src_type src_type,
                VkFormat depth_format, VkImageAspectFlagBits aspects, uint32_t log2_samples)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (src_type == BLIT2D_SRC_TYPE_BUFFER) {
      create_bview(cmd_buffer, src_buf, &tmp->bview, depth_format);

      radv_meta_push_descriptor_set(
         cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, device->meta_state.blit2d[log2_samples].p_layouts[src_type], 0, 1,
         (VkWriteDescriptorSet[]){{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstBinding = 0,
                                   .dstArrayElement = 0,
                                   .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                   .pTexelBufferView = (VkBufferView[]){radv_buffer_view_to_handle(&tmp->bview)}}});

      vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                                 device->meta_state.blit2d[log2_samples].p_layouts[src_type],
                                 VK_SHADER_STAGE_FRAGMENT_BIT, 16, 4, &src_buf->pitch);
   } else {
      create_iview(cmd_buffer, src_img, &tmp->iview, depth_format, aspects);

      if (src_type == BLIT2D_SRC_TYPE_IMAGE_3D)
         vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                                    device->meta_state.blit2d[log2_samples].p_layouts[src_type],
                                    VK_SHADER_STAGE_FRAGMENT_BIT, 16, 4, &src_img->layer);

      radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    device->meta_state.blit2d[log2_samples].p_layouts[src_type], 0, 1,
                                    (VkWriteDescriptorSet[]){{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                              .dstBinding = 0,
                                                              .dstArrayElement = 0,
                                                              .descriptorCount = 1,
                                                              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                                              .pImageInfo = (VkDescriptorImageInfo[]){
                                                                 {
                                                                    .sampler = VK_NULL_HANDLE,
                                                                    .imageView = radv_image_view_to_handle(&tmp->iview),
                                                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                                 },
                                                              }}});
   }
}

static void
radv_meta_blit2d_normal_dst(struct radv_cmd_buffer *cmd_buffer, struct radv_meta_blit2d_surf *src_img,
                            struct radv_meta_blit2d_buffer *src_buf, struct radv_meta_blit2d_surf *dst,
                            struct radv_meta_blit2d_rect *rect, enum blit2d_src_type src_type, uint32_t log2_samples)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   VkPipeline pipeline;
   VkResult result;

   radv_CmdSetViewport(radv_cmd_buffer_to_handle(cmd_buffer), 0, 1,
                       &(VkViewport){.x = rect->dst_x,
                                     .y = rect->dst_y,
                                     .width = rect->width,
                                     .height = rect->height,
                                     .minDepth = 0.0f,
                                     .maxDepth = 1.0f});

   radv_CmdSetScissor(radv_cmd_buffer_to_handle(cmd_buffer), 0, 1,
                      &(VkRect2D){
                         .offset = (VkOffset2D){rect->dst_x, rect->dst_y},
                         .extent = (VkExtent2D){rect->width, rect->height},
                      });

   u_foreach_bit (i, dst->aspect_mask) {
      unsigned aspect_mask = 1u << i;
      unsigned src_aspect_mask = aspect_mask;
      VkFormat depth_format = 0;
      if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT)
         depth_format = vk_format_stencil_only(dst->image->vk.format);
      else if (aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT)
         depth_format = vk_format_depth_only(dst->image->vk.format);
      else if (src_img)
         src_aspect_mask = src_img->aspect_mask;

      struct radv_image_view dst_iview;
      create_iview(cmd_buffer, dst, &dst_iview, depth_format, aspect_mask);

      float vertex_push_constants[4] = {
         rect->src_x,
         rect->src_y,
         rect->src_x + rect->width,
         rect->src_y + rect->height,
      };

      vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                                 device->meta_state.blit2d[log2_samples].p_layouts[src_type],
                                 VK_SHADER_STAGE_VERTEX_BIT, 0, 16, vertex_push_constants);

      const VkRenderingAttachmentInfo att_info = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
         .imageView = radv_image_view_to_handle(&dst_iview),
         .imageLayout = dst->current_layout,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      };

      VkRenderingInfo rendering_info = {
         .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
         .flags = VK_RENDERING_INPUT_ATTACHMENT_NO_CONCURRENT_WRITES_BIT_MESA,
         .renderArea =
            {
               .offset = {rect->dst_x, rect->dst_y},
               .extent = {rect->width, rect->height},
            },
         .layerCount = 1,
      };

      if (aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT || aspect_mask == VK_IMAGE_ASPECT_PLANE_0_BIT ||
          aspect_mask == VK_IMAGE_ASPECT_PLANE_1_BIT || aspect_mask == VK_IMAGE_ASPECT_PLANE_2_BIT) {
         result = get_color_pipeline(device, src_type, dst_iview.vk.format, log2_samples, &pipeline);
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd_buffer->vk, result);
            goto fail_pipeline;
         }

         rendering_info.colorAttachmentCount = 1;
         rendering_info.pColorAttachments = &att_info;

         radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      } else if (aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT) {
         result = get_depth_only_pipeline(device, src_type, log2_samples, &pipeline);
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd_buffer->vk, result);
            goto fail_pipeline;
         }

         rendering_info.pDepthAttachment = &att_info,
         rendering_info.pStencilAttachment = (dst->image->vk.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &att_info : NULL,

         radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

      } else if (aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT) {
         result = get_stencil_only_pipeline(device, src_type, log2_samples, &pipeline);
         if (result != VK_SUCCESS) {
            vk_command_buffer_set_error(&cmd_buffer->vk, result);
            goto fail_pipeline;
         }

         rendering_info.pDepthAttachment = (dst->image->vk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &att_info : NULL,
         rendering_info.pStencilAttachment = &att_info,

         radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      } else
         unreachable("Processing blit2d with multiple aspects.");

      struct blit2d_src_temps src_temps;
      blit2d_bind_src(cmd_buffer, src_img, src_buf, &src_temps, src_type, depth_format, src_aspect_mask, log2_samples);

      radv_CmdBeginRendering(radv_cmd_buffer_to_handle(cmd_buffer), &rendering_info);

      radv_CmdDraw(radv_cmd_buffer_to_handle(cmd_buffer), 3, 1, 0, 0);

      radv_CmdEndRendering(radv_cmd_buffer_to_handle(cmd_buffer));

   fail_pipeline:

      if (src_type == BLIT2D_SRC_TYPE_BUFFER)
         radv_buffer_view_finish(&src_temps.bview);
      else
         radv_image_view_finish(&src_temps.iview);

      radv_image_view_finish(&dst_iview);
   }
}

void
radv_meta_blit2d(struct radv_cmd_buffer *cmd_buffer, struct radv_meta_blit2d_surf *src_img,
                 struct radv_meta_blit2d_buffer *src_buf, struct radv_meta_blit2d_surf *dst,
                 struct radv_meta_blit2d_rect *rect)
{
   bool use_3d = (src_img && src_img->image->vk.image_type == VK_IMAGE_TYPE_3D);
   enum blit2d_src_type src_type = src_buf  ? BLIT2D_SRC_TYPE_BUFFER
                                   : use_3d ? BLIT2D_SRC_TYPE_IMAGE_3D
                                            : BLIT2D_SRC_TYPE_IMAGE;
   radv_meta_blit2d_normal_dst(cmd_buffer, src_img, src_buf, dst, rect, src_type,
                               src_img ? util_logbase2(src_img->image->vk.samples) : 0);
}

static nir_shader *
build_nir_vertex_shader(struct radv_device *device)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   const struct glsl_type *vec2 = glsl_vector_type(GLSL_TYPE_FLOAT, 2);
   nir_builder b = radv_meta_init_shader(device, MESA_SHADER_VERTEX, "meta_blit2d_vs");

   nir_variable *pos_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   pos_out->data.location = VARYING_SLOT_POS;

   nir_variable *tex_pos_out = nir_variable_create(b.shader, nir_var_shader_out, vec2, "v_tex_pos");
   tex_pos_out->data.location = VARYING_SLOT_VAR0;
   tex_pos_out->data.interpolation = INTERP_MODE_SMOOTH;

   nir_def *outvec = nir_gen_rect_vertices(&b, NULL, NULL);
   nir_store_var(&b, pos_out, outvec, 0xf);

   nir_def *src_box = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 0), .range = 16);
   nir_def *vertex_id = nir_load_vertex_id_zero_base(&b);

   /* vertex 0 - src_x, src_y */
   /* vertex 1 - src_x, src_y+h */
   /* vertex 2 - src_x+w, src_y */
   /* so channel 0 is vertex_id != 2 ? src_x : src_x + w
      channel 1 is vertex id != 1 ? src_y : src_y + w */

   nir_def *c0cmp = nir_ine_imm(&b, vertex_id, 2);
   nir_def *c1cmp = nir_ine_imm(&b, vertex_id, 1);

   nir_def *comp[2];
   comp[0] = nir_bcsel(&b, c0cmp, nir_channel(&b, src_box, 0), nir_channel(&b, src_box, 2));

   comp[1] = nir_bcsel(&b, c1cmp, nir_channel(&b, src_box, 1), nir_channel(&b, src_box, 3));
   nir_def *out_tex_vec = nir_vec(&b, comp, 2);
   nir_store_var(&b, tex_pos_out, out_tex_vec, 0x3);
   return b.shader;
}

typedef nir_def *(*texel_fetch_build_func)(struct nir_builder *, struct radv_device *, nir_def *, bool, bool);

static nir_def *
build_nir_texel_fetch(struct nir_builder *b, struct radv_device *device, nir_def *tex_pos, bool is_3d,
                      bool is_multisampled)
{
   enum glsl_sampler_dim dim = is_3d             ? GLSL_SAMPLER_DIM_3D
                               : is_multisampled ? GLSL_SAMPLER_DIM_MS
                                                 : GLSL_SAMPLER_DIM_2D;
   const struct glsl_type *sampler_type = glsl_sampler_type(dim, false, false, GLSL_TYPE_UINT);
   nir_variable *sampler = nir_variable_create(b->shader, nir_var_uniform, sampler_type, "s_tex");
   sampler->data.descriptor_set = 0;
   sampler->data.binding = 0;

   nir_def *tex_pos_3d = NULL;
   nir_def *sample_idx = NULL;
   if (is_3d) {
      nir_def *layer = nir_load_push_constant(b, 1, 32, nir_imm_int(b, 0), .base = 16, .range = 4);

      nir_def *chans[3];
      chans[0] = nir_channel(b, tex_pos, 0);
      chans[1] = nir_channel(b, tex_pos, 1);
      chans[2] = layer;
      tex_pos_3d = nir_vec(b, chans, 3);
   }
   if (is_multisampled) {
      sample_idx = nir_load_sample_id(b);
   }

   nir_deref_instr *tex_deref = nir_build_deref_var(b, sampler);

   if (is_multisampled) {
      return nir_txf_ms_deref(b, tex_deref, tex_pos, sample_idx);
   } else {
      return nir_txf_deref(b, tex_deref, is_3d ? tex_pos_3d : tex_pos, NULL);
   }
}

static nir_def *
build_nir_buffer_fetch(struct nir_builder *b, struct radv_device *device, nir_def *tex_pos, bool is_3d,
                       bool is_multisampled)
{
   const struct glsl_type *sampler_type = glsl_sampler_type(GLSL_SAMPLER_DIM_BUF, false, false, GLSL_TYPE_UINT);
   nir_variable *sampler = nir_variable_create(b->shader, nir_var_uniform, sampler_type, "s_tex");
   sampler->data.descriptor_set = 0;
   sampler->data.binding = 0;

   nir_def *width = nir_load_push_constant(b, 1, 32, nir_imm_int(b, 0), .base = 16, .range = 4);

   nir_def *pos_x = nir_channel(b, tex_pos, 0);
   nir_def *pos_y = nir_channel(b, tex_pos, 1);
   pos_y = nir_imul(b, pos_y, width);
   pos_x = nir_iadd(b, pos_x, pos_y);

   nir_deref_instr *tex_deref = nir_build_deref_var(b, sampler);
   return nir_txf_deref(b, tex_deref, pos_x, NULL);
}

static const VkPipelineVertexInputStateCreateInfo normal_vi_create_info = {
   .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
   .vertexBindingDescriptionCount = 0,
   .vertexAttributeDescriptionCount = 0,
};

static nir_shader *
build_nir_copy_fragment_shader(struct radv_device *device, texel_fetch_build_func txf_func, const char *name,
                               bool is_3d, bool is_multisampled)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   const struct glsl_type *vec2 = glsl_vector_type(GLSL_TYPE_FLOAT, 2);
   nir_builder b = radv_meta_init_shader(device, MESA_SHADER_FRAGMENT, "%s", name);

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in, vec2, "v_tex_pos");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "f_color");
   color_out->data.location = FRAG_RESULT_DATA0;

   nir_def *pos_int = nir_f2i32(&b, nir_load_var(&b, tex_pos_in));
   nir_def *tex_pos = nir_trim_vector(&b, pos_int, 2);

   nir_def *color = txf_func(&b, device, tex_pos, is_3d, is_multisampled);
   nir_store_var(&b, color_out, color, 0xf);

   b.shader->info.fs.uses_sample_shading = is_multisampled;

   return b.shader;
}

static nir_shader *
build_nir_copy_fragment_shader_depth(struct radv_device *device, texel_fetch_build_func txf_func, const char *name,
                                     bool is_3d, bool is_multisampled)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   const struct glsl_type *vec2 = glsl_vector_type(GLSL_TYPE_FLOAT, 2);
   nir_builder b = radv_meta_init_shader(device, MESA_SHADER_FRAGMENT, "%s", name);

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in, vec2, "v_tex_pos");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "f_color");
   color_out->data.location = FRAG_RESULT_DEPTH;

   nir_def *pos_int = nir_f2i32(&b, nir_load_var(&b, tex_pos_in));
   nir_def *tex_pos = nir_trim_vector(&b, pos_int, 2);

   nir_def *color = txf_func(&b, device, tex_pos, is_3d, is_multisampled);
   nir_store_var(&b, color_out, color, 0x1);

   b.shader->info.fs.uses_sample_shading = is_multisampled;

   return b.shader;
}

static nir_shader *
build_nir_copy_fragment_shader_stencil(struct radv_device *device, texel_fetch_build_func txf_func, const char *name,
                                       bool is_3d, bool is_multisampled)
{
   const struct glsl_type *vec4 = glsl_vec4_type();
   const struct glsl_type *vec2 = glsl_vector_type(GLSL_TYPE_FLOAT, 2);
   nir_builder b = radv_meta_init_shader(device, MESA_SHADER_FRAGMENT, "%s", name);

   nir_variable *tex_pos_in = nir_variable_create(b.shader, nir_var_shader_in, vec2, "v_tex_pos");
   tex_pos_in->data.location = VARYING_SLOT_VAR0;

   nir_variable *color_out = nir_variable_create(b.shader, nir_var_shader_out, vec4, "f_color");
   color_out->data.location = FRAG_RESULT_STENCIL;

   nir_def *pos_int = nir_f2i32(&b, nir_load_var(&b, tex_pos_in));
   nir_def *tex_pos = nir_trim_vector(&b, pos_int, 2);

   nir_def *color = txf_func(&b, device, tex_pos, is_3d, is_multisampled);
   nir_store_var(&b, color_out, color, 0x1);

   b.shader->info.fs.uses_sample_shading = is_multisampled;

   return b.shader;
}

void
radv_device_finish_meta_blit2d_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;

   for (unsigned log2_samples = 0; log2_samples < MAX_SAMPLES_LOG2; ++log2_samples) {
      for (unsigned src = 0; src < BLIT2D_NUM_SRC_TYPES; src++) {
         radv_DestroyPipelineLayout(radv_device_to_handle(device), state->blit2d[log2_samples].p_layouts[src],
                                    &state->alloc);
         device->vk.dispatch_table.DestroyDescriptorSetLayout(
            radv_device_to_handle(device), state->blit2d[log2_samples].ds_layouts[src], &state->alloc);

         for (unsigned j = 0; j < NUM_META_FS_KEYS; ++j) {
            radv_DestroyPipeline(radv_device_to_handle(device), state->blit2d[log2_samples].pipelines[src][j],
                                 &state->alloc);
         }

         radv_DestroyPipeline(radv_device_to_handle(device), state->blit2d[log2_samples].depth_only_pipeline[src],
                              &state->alloc);
         radv_DestroyPipeline(radv_device_to_handle(device), state->blit2d[log2_samples].stencil_only_pipeline[src],
                              &state->alloc);
      }
   }
}

static VkResult
meta_blit2d_create_pipe_layout(struct radv_device *device, int idx, uint32_t log2_samples)
{
   VkResult result = VK_SUCCESS;

   if (!device->meta_state.blit2d[log2_samples].ds_layouts[idx]) {
      VkDescriptorType desc_type =
         (idx == BLIT2D_SRC_TYPE_BUFFER) ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;

      const VkDescriptorSetLayoutBinding binding = {
         .binding = 0,
         .descriptorType = desc_type,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      };

      result = radv_meta_create_descriptor_set_layout(device, 1, &binding,
                                                      &device->meta_state.blit2d[log2_samples].ds_layouts[idx]);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!device->meta_state.blit2d[log2_samples].p_layouts[idx]) {
      const VkPushConstantRange push_constant_ranges[] = {
         {VK_SHADER_STAGE_VERTEX_BIT, 0, 16},
         {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 4},
      };
      int num_push_constant_range = (idx != BLIT2D_SRC_TYPE_IMAGE || log2_samples > 0) ? 2 : 1;

      result = radv_meta_create_pipeline_layout(device, &device->meta_state.blit2d[log2_samples].ds_layouts[idx],
                                                num_push_constant_range, push_constant_ranges,
                                                &device->meta_state.blit2d[log2_samples].p_layouts[idx]);
   }

   return result;
}

static VkResult
create_color_pipeline(struct radv_device *device, enum blit2d_src_type src_type, VkFormat format, uint32_t log2_samples,
                      VkPipeline *pipeline)
{
   VkResult result;
   const char *name;

   result = meta_blit2d_create_pipe_layout(device, src_type, log2_samples);
   if (result != VK_SUCCESS)
      return result;

   texel_fetch_build_func src_func;
   switch (src_type) {
   case BLIT2D_SRC_TYPE_IMAGE:
      src_func = build_nir_texel_fetch;
      name = "meta_blit2d_image_fs";
      break;
   case BLIT2D_SRC_TYPE_IMAGE_3D:
      src_func = build_nir_texel_fetch;
      name = "meta_blit3d_image_fs";
      break;
   case BLIT2D_SRC_TYPE_BUFFER:
      src_func = build_nir_buffer_fetch;
      name = "meta_blit2d_buffer_fs";
      break;
   default:
      unreachable("unknown blit src type\n");
      break;
   }

   const VkPipelineVertexInputStateCreateInfo *vi_create_info;
   nir_shader *fs =
      build_nir_copy_fragment_shader(device, src_func, name, src_type == BLIT2D_SRC_TYPE_IMAGE_3D, log2_samples > 0);
   nir_shader *vs = build_nir_vertex_shader(device);

   vi_create_info = &normal_vi_create_info;

   VkPipelineShaderStageCreateInfo pipeline_shader_stages[] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vk_shader_module_handle_from_nir(vs),
       .pName = "main",
       .pSpecializationInfo = NULL},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = vk_shader_module_handle_from_nir(fs),
       .pName = "main",
       .pSpecializationInfo = NULL},
   };

   const VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &format,
   };

   const VkGraphicsPipelineCreateInfo vk_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_create_info,
      .stageCount = ARRAY_SIZE(pipeline_shader_stages),
      .pStages = pipeline_shader_stages,
      .pVertexInputState = vi_create_info,
      .pInputAssemblyState =
         &(VkPipelineInputAssemblyStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },
      .pViewportState =
         &(VkPipelineViewportStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
         },
      .pRasterizationState =
         &(VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                   .rasterizerDiscardEnable = false,
                                                   .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = VK_CULL_MODE_NONE,
                                                   .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                   .depthBiasConstantFactor = 0.0f,
                                                   .depthBiasClamp = 0.0f,
                                                   .depthBiasSlopeFactor = 0.0f,
                                                   .lineWidth = 1.0f},
      .pMultisampleState =
         &(VkPipelineMultisampleStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = 1 << log2_samples,
            .sampleShadingEnable = log2_samples > 1,
            .minSampleShading = 1.0,
            .pSampleMask = (VkSampleMask[]){UINT32_MAX},
         },
      .pColorBlendState =
         &(VkPipelineColorBlendStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments =
               (VkPipelineColorBlendAttachmentState[]){
                  {.colorWriteMask = VK_COLOR_COMPONENT_A_BIT | VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT},
               },
            .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f}},
      .pDynamicState =
         &(VkPipelineDynamicStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates =
               (VkDynamicState[]){
                  VK_DYNAMIC_STATE_VIEWPORT,
                  VK_DYNAMIC_STATE_SCISSOR,
               },
         },
      .flags = 0,
      .layout = device->meta_state.blit2d[log2_samples].p_layouts[src_type],
      .renderPass = VK_NULL_HANDLE,
      .subpass = 0,
   };

   const struct radv_graphics_pipeline_create_info radv_pipeline_info = {.use_rectlist = true};

   result = radv_graphics_pipeline_create(radv_device_to_handle(device), device->meta_state.cache, &vk_pipeline_info,
                                          &radv_pipeline_info, &device->meta_state.alloc, pipeline);

   ralloc_free(vs);
   ralloc_free(fs);
   return result;
}

static VkResult
get_color_pipeline(struct radv_device *device, enum blit2d_src_type src_type, VkFormat format, uint32_t log2_samples,
                   VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   const unsigned fs_key = radv_format_meta_fs_key(device, format);
   VkResult result = VK_SUCCESS;
   VkPipeline *pipeline;

   mtx_lock(&state->mtx);
   pipeline = &device->meta_state.blit2d[log2_samples].pipelines[src_type][fs_key];
   if (!*pipeline) {
      result = create_color_pipeline(device, src_type, radv_fs_key_format_exemplars[fs_key], log2_samples, pipeline);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = *pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

static VkResult
create_depth_only_pipeline(struct radv_device *device, enum blit2d_src_type src_type, uint32_t log2_samples,
                           VkPipeline *pipeline)
{
   VkResult result;
   const char *name;

   result = meta_blit2d_create_pipe_layout(device, src_type, log2_samples);
   if (result != VK_SUCCESS)
      return result;

   texel_fetch_build_func src_func;
   switch (src_type) {
   case BLIT2D_SRC_TYPE_IMAGE:
      src_func = build_nir_texel_fetch;
      name = "meta_blit2d_depth_image_fs";
      break;
   case BLIT2D_SRC_TYPE_IMAGE_3D:
      src_func = build_nir_texel_fetch;
      name = "meta_blit3d_depth_image_fs";
      break;
   case BLIT2D_SRC_TYPE_BUFFER:
      src_func = build_nir_buffer_fetch;
      name = "meta_blit2d_depth_buffer_fs";
      break;
   default:
      unreachable("unknown blit src type\n");
      break;
   }

   const VkPipelineVertexInputStateCreateInfo *vi_create_info;
   nir_shader *fs = build_nir_copy_fragment_shader_depth(device, src_func, name, src_type == BLIT2D_SRC_TYPE_IMAGE_3D,
                                                         log2_samples > 0);
   nir_shader *vs = build_nir_vertex_shader(device);

   vi_create_info = &normal_vi_create_info;

   VkPipelineShaderStageCreateInfo pipeline_shader_stages[] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vk_shader_module_handle_from_nir(vs),
       .pName = "main",
       .pSpecializationInfo = NULL},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = vk_shader_module_handle_from_nir(fs),
       .pName = "main",
       .pSpecializationInfo = NULL},
   };

   const VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
   };

   const VkGraphicsPipelineCreateInfo vk_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_create_info,
      .stageCount = ARRAY_SIZE(pipeline_shader_stages),
      .pStages = pipeline_shader_stages,
      .pVertexInputState = vi_create_info,
      .pInputAssemblyState =
         &(VkPipelineInputAssemblyStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },
      .pViewportState =
         &(VkPipelineViewportStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
         },
      .pRasterizationState =
         &(VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                   .rasterizerDiscardEnable = false,
                                                   .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = VK_CULL_MODE_NONE,
                                                   .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                   .depthBiasConstantFactor = 0.0f,
                                                   .depthBiasClamp = 0.0f,
                                                   .depthBiasSlopeFactor = 0.0f,
                                                   .lineWidth = 1.0f},
      .pMultisampleState =
         &(VkPipelineMultisampleStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = 1 << log2_samples,
            .sampleShadingEnable = false,
            .pSampleMask = (VkSampleMask[]){UINT32_MAX},
         },
      .pColorBlendState =
         &(VkPipelineColorBlendStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 0,
            .pAttachments = NULL,
            .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
         },
      .pDepthStencilState =
         &(VkPipelineDepthStencilStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,
            .depthWriteEnable = true,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .front =
               {
                  .failOp = VK_STENCIL_OP_KEEP,
                  .passOp = VK_STENCIL_OP_KEEP,
                  .depthFailOp = VK_STENCIL_OP_KEEP,
                  .compareOp = VK_COMPARE_OP_NEVER,
                  .compareMask = UINT32_MAX,
                  .writeMask = UINT32_MAX,
                  .reference = 0u,
               },
            .back =
               {
                  .failOp = VK_STENCIL_OP_KEEP,
                  .passOp = VK_STENCIL_OP_KEEP,
                  .depthFailOp = VK_STENCIL_OP_KEEP,
                  .compareOp = VK_COMPARE_OP_NEVER,
                  .compareMask = UINT32_MAX,
                  .writeMask = UINT32_MAX,
                  .reference = 0u,
               },
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f,
         },
      .pDynamicState =
         &(VkPipelineDynamicStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates =
               (VkDynamicState[]){
                  VK_DYNAMIC_STATE_VIEWPORT,
                  VK_DYNAMIC_STATE_SCISSOR,
               },
         },
      .flags = 0,
      .layout = device->meta_state.blit2d[log2_samples].p_layouts[src_type],
      .renderPass = VK_NULL_HANDLE,
      .subpass = 0,
   };

   const struct radv_graphics_pipeline_create_info radv_pipeline_info = {.use_rectlist = true};

   result = radv_graphics_pipeline_create(radv_device_to_handle(device), device->meta_state.cache, &vk_pipeline_info,
                                          &radv_pipeline_info, &device->meta_state.alloc, pipeline);

   ralloc_free(vs);
   ralloc_free(fs);

   return result;
}

static VkResult
get_depth_only_pipeline(struct radv_device *device, enum blit2d_src_type src_type, uint32_t log2_samples,
                        VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result = VK_SUCCESS;
   VkPipeline *pipeline;

   mtx_lock(&state->mtx);
   pipeline = &device->meta_state.blit2d[log2_samples].depth_only_pipeline[src_type];
   if (!*pipeline) {
      result = create_depth_only_pipeline(device, src_type, log2_samples, pipeline);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = *pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

static VkResult
create_stencil_only_pipeline(struct radv_device *device, enum blit2d_src_type src_type, uint32_t log2_samples,
                             VkPipeline *pipeline_out)
{
   VkResult result;
   const char *name;

   result = meta_blit2d_create_pipe_layout(device, src_type, log2_samples);
   if (result != VK_SUCCESS)
      return result;

   texel_fetch_build_func src_func;
   switch (src_type) {
   case BLIT2D_SRC_TYPE_IMAGE:
      src_func = build_nir_texel_fetch;
      name = "meta_blit2d_stencil_image_fs";
      break;
   case BLIT2D_SRC_TYPE_IMAGE_3D:
      src_func = build_nir_texel_fetch;
      name = "meta_blit3d_stencil_image_fs";
      break;
   case BLIT2D_SRC_TYPE_BUFFER:
      src_func = build_nir_buffer_fetch;
      name = "meta_blit2d_stencil_buffer_fs";
      break;
   default:
      unreachable("unknown blit src type\n");
      break;
   }

   const VkPipelineVertexInputStateCreateInfo *vi_create_info;
   nir_shader *fs = build_nir_copy_fragment_shader_stencil(device, src_func, name, src_type == BLIT2D_SRC_TYPE_IMAGE_3D,
                                                           log2_samples > 0);
   nir_shader *vs = build_nir_vertex_shader(device);

   vi_create_info = &normal_vi_create_info;

   VkPipelineShaderStageCreateInfo pipeline_shader_stages[] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = vk_shader_module_handle_from_nir(vs),
       .pName = "main",
       .pSpecializationInfo = NULL},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = vk_shader_module_handle_from_nir(fs),
       .pName = "main",
       .pSpecializationInfo = NULL},
   };

   const VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .stencilAttachmentFormat = VK_FORMAT_S8_UINT,
   };

   const VkGraphicsPipelineCreateInfo vk_pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering_create_info,
      .stageCount = ARRAY_SIZE(pipeline_shader_stages),
      .pStages = pipeline_shader_stages,
      .pVertexInputState = vi_create_info,
      .pInputAssemblyState =
         &(VkPipelineInputAssemblyStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },
      .pViewportState =
         &(VkPipelineViewportStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
         },
      .pRasterizationState =
         &(VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                   .rasterizerDiscardEnable = false,
                                                   .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = VK_CULL_MODE_NONE,
                                                   .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                   .depthBiasConstantFactor = 0.0f,
                                                   .depthBiasClamp = 0.0f,
                                                   .depthBiasSlopeFactor = 0.0f,
                                                   .lineWidth = 1.0f},
      .pMultisampleState =
         &(VkPipelineMultisampleStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = 1 << log2_samples,
            .sampleShadingEnable = false,
            .pSampleMask = (VkSampleMask[]){UINT32_MAX},
         },
      .pColorBlendState =
         &(VkPipelineColorBlendStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 0,
            .pAttachments = NULL,
            .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
         },
      .pDepthStencilState =
         &(VkPipelineDepthStencilStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .stencilTestEnable = true,
            .front = {.failOp = VK_STENCIL_OP_REPLACE,
                      .passOp = VK_STENCIL_OP_REPLACE,
                      .depthFailOp = VK_STENCIL_OP_REPLACE,
                      .compareOp = VK_COMPARE_OP_ALWAYS,
                      .compareMask = 0xff,
                      .writeMask = 0xff,
                      .reference = 0},
            .back = {.failOp = VK_STENCIL_OP_REPLACE,
                     .passOp = VK_STENCIL_OP_REPLACE,
                     .depthFailOp = VK_STENCIL_OP_REPLACE,
                     .compareOp = VK_COMPARE_OP_ALWAYS,
                     .compareMask = 0xff,
                     .writeMask = 0xff,
                     .reference = 0},
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f,
         },
      .pDynamicState =
         &(VkPipelineDynamicStateCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates =
               (VkDynamicState[]){
                  VK_DYNAMIC_STATE_VIEWPORT,
                  VK_DYNAMIC_STATE_SCISSOR,
               },
         },
      .flags = 0,
      .layout = device->meta_state.blit2d[log2_samples].p_layouts[src_type],
      .renderPass = VK_NULL_HANDLE,
      .subpass = 0,
   };

   const struct radv_graphics_pipeline_create_info radv_pipeline_info = {.use_rectlist = true};

   result = radv_graphics_pipeline_create(radv_device_to_handle(device), device->meta_state.cache, &vk_pipeline_info,
                                          &radv_pipeline_info, &device->meta_state.alloc,
                                          &device->meta_state.blit2d[log2_samples].stencil_only_pipeline[src_type]);

   ralloc_free(vs);
   ralloc_free(fs);

   return result;
}

static VkResult
get_stencil_only_pipeline(struct radv_device *device, enum blit2d_src_type src_type, uint32_t log2_samples,
                          VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result = VK_SUCCESS;
   VkPipeline *pipeline;

   mtx_lock(&state->mtx);
   pipeline = &device->meta_state.blit2d[log2_samples].stencil_only_pipeline[src_type];
   if (!*pipeline) {
      result = create_stencil_only_pipeline(device, src_type, log2_samples, pipeline);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = *pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

VkResult
radv_device_init_meta_blit2d_state(struct radv_device *device, bool on_demand)
{
   VkResult result;

   if (on_demand)
      return VK_SUCCESS;

   for (unsigned log2_samples = 0; log2_samples < MAX_SAMPLES_LOG2; log2_samples++) {
      for (unsigned src = 0; src < BLIT2D_NUM_SRC_TYPES; src++) {
         /* Don't need to handle copies between buffers and multisample images. */
         if (src == BLIT2D_SRC_TYPE_BUFFER && log2_samples > 0)
            continue;

         /* There are no multisampled 3D images. */
         if (src == BLIT2D_SRC_TYPE_IMAGE_3D && log2_samples > 0)
            continue;

         for (unsigned j = 0; j < NUM_META_FS_KEYS; ++j) {
            VkFormat format = radv_fs_key_format_exemplars[j];
            unsigned fs_key = radv_format_meta_fs_key(device, format);

            result = create_color_pipeline(device, src, format, log2_samples,
                                           &device->meta_state.blit2d[log2_samples].pipelines[src][fs_key]);
            if (result != VK_SUCCESS)
               return result;
         }

         result = create_depth_only_pipeline(device, src, log2_samples,
                                             &device->meta_state.blit2d[log2_samples].depth_only_pipeline[src]);
         if (result != VK_SUCCESS)
            return result;

         result = create_stencil_only_pipeline(device, src, log2_samples,
                                               &device->meta_state.blit2d[log2_samples].stencil_only_pipeline[src]);
         if (result != VK_SUCCESS)
            return result;
      }
   }

   return VK_SUCCESS;
}
