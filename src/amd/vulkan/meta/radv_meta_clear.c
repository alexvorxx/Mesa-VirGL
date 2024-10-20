/*
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "nir/nir_builder.h"
#include "radv_debug.h"
#include "radv_entrypoints.h"
#include "radv_formats.h"
#include "radv_meta.h"

#include "util/format_rgb9e5.h"
#include "vk_common_entrypoints.h"
#include "vk_format.h"
#include "vk_shader_module.h"

#include "ac_formats.h"

enum { DEPTH_CLEAR_SLOW, DEPTH_CLEAR_FAST };

static void
build_color_shaders(struct radv_device *dev, struct nir_shader **out_vs, struct nir_shader **out_fs,
                    uint32_t frag_output)
{
   nir_builder vs_b = radv_meta_init_shader(dev, MESA_SHADER_VERTEX, "meta_clear_color_vs");
   nir_builder fs_b = radv_meta_init_shader(dev, MESA_SHADER_FRAGMENT, "meta_clear_color_fs-%d", frag_output);

   const struct glsl_type *position_type = glsl_vec4_type();
   const struct glsl_type *color_type = glsl_vec4_type();

   nir_variable *vs_out_pos = nir_variable_create(vs_b.shader, nir_var_shader_out, position_type, "gl_Position");
   vs_out_pos->data.location = VARYING_SLOT_POS;

   nir_def *in_color_load = nir_load_push_constant(&fs_b, 4, 32, nir_imm_int(&fs_b, 0), .range = 16);

   nir_variable *fs_out_color = nir_variable_create(fs_b.shader, nir_var_shader_out, color_type, "f_color");
   fs_out_color->data.location = FRAG_RESULT_DATA0 + frag_output;

   nir_store_var(&fs_b, fs_out_color, in_color_load, 0xf);

   nir_def *outvec = nir_gen_rect_vertices(&vs_b, NULL, NULL);
   nir_store_var(&vs_b, vs_out_pos, outvec, 0xf);

   const struct glsl_type *layer_type = glsl_int_type();
   nir_variable *vs_out_layer = nir_variable_create(vs_b.shader, nir_var_shader_out, layer_type, "v_layer");
   vs_out_layer->data.location = VARYING_SLOT_LAYER;
   vs_out_layer->data.interpolation = INTERP_MODE_FLAT;
   nir_def *inst_id = nir_load_instance_id(&vs_b);
   nir_def *base_instance = nir_load_base_instance(&vs_b);

   nir_def *layer_id = nir_iadd(&vs_b, inst_id, base_instance);
   nir_store_var(&vs_b, vs_out_layer, layer_id, 0x1);

   *out_vs = vs_b.shader;
   *out_fs = fs_b.shader;
}

static VkResult
create_pipeline(struct radv_device *device, uint32_t samples, struct nir_shader *vs_nir, struct nir_shader *fs_nir,
                const VkPipelineVertexInputStateCreateInfo *vi_state,
                const VkPipelineDepthStencilStateCreateInfo *ds_state,
                const VkPipelineColorBlendStateCreateInfo *cb_state, const VkPipelineRenderingCreateInfo *dyn_state,
                const VkPipelineLayout layout, const struct radv_graphics_pipeline_create_info *extra,
                const VkAllocationCallbacks *alloc, VkPipeline *pipeline)
{
   VkDevice device_h = radv_device_to_handle(device);
   VkResult result;

   result = radv_graphics_pipeline_create(device_h, device->meta_state.cache,
                                          &(VkGraphicsPipelineCreateInfo){
                                             .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                             .pNext = dyn_state,
                                             .stageCount = fs_nir ? 2 : 1,
                                             .pStages =
                                                (VkPipelineShaderStageCreateInfo[]){
                                                   {
                                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                      .module = vk_shader_module_handle_from_nir(vs_nir),
                                                      .pName = "main",
                                                   },
                                                   {
                                                      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                      .module = vk_shader_module_handle_from_nir(fs_nir),
                                                      .pName = "main",
                                                   },
                                                },
                                             .pVertexInputState = vi_state,
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
                                                &(VkPipelineRasterizationStateCreateInfo){
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                   .rasterizerDiscardEnable = false,
                                                   .polygonMode = VK_POLYGON_MODE_FILL,
                                                   .cullMode = VK_CULL_MODE_NONE,
                                                   .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                   .depthBiasEnable = false,
                                                   .depthBiasConstantFactor = 0.0f,
                                                   .depthBiasClamp = 0.0f,
                                                   .depthBiasSlopeFactor = 0.0f,
                                                   .lineWidth = 1.0f,
                                                },
                                             .pMultisampleState =
                                                &(VkPipelineMultisampleStateCreateInfo){
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = samples,
                                                   .sampleShadingEnable = false,
                                                   .pSampleMask = NULL,
                                                   .alphaToCoverageEnable = false,
                                                   .alphaToOneEnable = false,
                                                },
                                             .pDepthStencilState = ds_state,
                                             .pColorBlendState = cb_state,
                                             .pDynamicState =
                                                &(VkPipelineDynamicStateCreateInfo){
                                                   .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = 3,
                                                   .pDynamicStates =
                                                      (VkDynamicState[]){
                                                         VK_DYNAMIC_STATE_VIEWPORT,
                                                         VK_DYNAMIC_STATE_SCISSOR,
                                                         VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                      },
                                                },
                                             .layout = layout,
                                             .flags = 0,
                                             .renderPass = VK_NULL_HANDLE,
                                             .subpass = 0,
                                          },
                                          extra, alloc, pipeline);

   ralloc_free(vs_nir);
   ralloc_free(fs_nir);

   return result;
}

static VkResult
create_color_pipeline(struct radv_device *device, uint32_t samples, uint32_t frag_output, VkFormat format,
                      VkPipeline *pipeline)
{
   struct nir_shader *vs_nir;
   struct nir_shader *fs_nir;
   VkResult result;

   if (!device->meta_state.clear_color_p_layout) {
      const VkPushConstantRange pc_range_color = {
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
         .size = 16,
      };

      result =
         radv_meta_create_pipeline_layout(device, NULL, 1, &pc_range_color, &device->meta_state.clear_color_p_layout);
      if (result != VK_SUCCESS)
         return result;
   }

   build_color_shaders(device, &vs_nir, &fs_nir, frag_output);

   const VkPipelineVertexInputStateCreateInfo vi_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 0,
      .vertexAttributeDescriptionCount = 0,
   };

   const VkPipelineDepthStencilStateCreateInfo ds_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = false,
      .depthWriteEnable = false,
      .depthBoundsTestEnable = false,
      .stencilTestEnable = false,
      .minDepthBounds = 0.0f,
      .maxDepthBounds = 1.0f,
   };

   VkPipelineColorBlendAttachmentState blend_attachment_state[MAX_RTS] = {0};
   blend_attachment_state[frag_output] = (VkPipelineColorBlendAttachmentState){
      .blendEnable = false,
      .colorWriteMask =
         VK_COLOR_COMPONENT_A_BIT | VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT,
   };

   const VkPipelineColorBlendStateCreateInfo cb_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable = false,
      .attachmentCount = MAX_RTS,
      .pAttachments = blend_attachment_state,
      .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f}};

   VkFormat att_formats[MAX_RTS] = {0};
   att_formats[frag_output] = format;

   const VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = MAX_RTS,
      .pColorAttachmentFormats = att_formats,
   };

   struct radv_graphics_pipeline_create_info extra = {
      .use_rectlist = true,
   };
   result = create_pipeline(device, samples, vs_nir, fs_nir, &vi_state, &ds_state, &cb_state, &rendering_create_info,
                            device->meta_state.clear_color_p_layout, &extra, &device->meta_state.alloc, pipeline);

   return result;
}

static VkResult
get_color_pipeline(struct radv_device *device, uint32_t samples, uint32_t frag_output, VkFormat format,
                   VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   const uint32_t fs_key = radv_format_meta_fs_key(device, format);
   const uint32_t samples_log2 = ffs(samples) - 1;
   VkResult result = VK_SUCCESS;
   VkPipeline *pipeline;

   mtx_lock(&state->mtx);
   pipeline = &state->color_clear[samples_log2][frag_output].color_pipelines[fs_key];
   if (!*pipeline) {
      result = create_color_pipeline(device, samples, frag_output, radv_fs_key_format_exemplars[fs_key], pipeline);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = *pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

static void
finish_meta_clear_htile_mask_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;

   radv_DestroyPipeline(radv_device_to_handle(device), state->clear_htile_mask_pipeline, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->clear_htile_mask_p_layout, &state->alloc);
   device->vk.dispatch_table.DestroyDescriptorSetLayout(radv_device_to_handle(device),
                                                        state->clear_htile_mask_ds_layout, &state->alloc);
}

static void
finish_meta_clear_dcc_comp_to_single_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;

   for (uint32_t i = 0; i < 2; i++) {
      radv_DestroyPipeline(radv_device_to_handle(device), state->clear_dcc_comp_to_single_pipeline[i], &state->alloc);
   }
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->clear_dcc_comp_to_single_p_layout, &state->alloc);
   device->vk.dispatch_table.DestroyDescriptorSetLayout(radv_device_to_handle(device),
                                                        state->clear_dcc_comp_to_single_ds_layout, &state->alloc);
}

void
radv_device_finish_meta_clear_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;

   for (uint32_t i = 0; i < ARRAY_SIZE(state->color_clear); ++i) {
      for (uint32_t j = 0; j < ARRAY_SIZE(state->color_clear[0]); ++j) {
         for (uint32_t k = 0; k < ARRAY_SIZE(state->color_clear[i][j].color_pipelines); ++k) {
            radv_DestroyPipeline(radv_device_to_handle(device), state->color_clear[i][j].color_pipelines[k],
                                 &state->alloc);
         }
      }
   }
   for (uint32_t i = 0; i < ARRAY_SIZE(state->ds_clear); ++i) {
      for (uint32_t j = 0; j < NUM_DEPTH_CLEAR_PIPELINES; j++) {
         radv_DestroyPipeline(radv_device_to_handle(device), state->ds_clear[i].depth_only_pipeline[j], &state->alloc);
         radv_DestroyPipeline(radv_device_to_handle(device), state->ds_clear[i].stencil_only_pipeline[j],
                              &state->alloc);
         radv_DestroyPipeline(radv_device_to_handle(device), state->ds_clear[i].depthstencil_pipeline[j],
                              &state->alloc);

         radv_DestroyPipeline(radv_device_to_handle(device), state->ds_clear[i].depth_only_unrestricted_pipeline[j],
                              &state->alloc);
         radv_DestroyPipeline(radv_device_to_handle(device), state->ds_clear[i].stencil_only_unrestricted_pipeline[j],
                              &state->alloc);
         radv_DestroyPipeline(radv_device_to_handle(device), state->ds_clear[i].depthstencil_unrestricted_pipeline[j],
                              &state->alloc);
      }
   }
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->clear_color_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->clear_depth_p_layout, &state->alloc);
   radv_DestroyPipelineLayout(radv_device_to_handle(device), state->clear_depth_unrestricted_p_layout, &state->alloc);

   finish_meta_clear_htile_mask_state(device);
   finish_meta_clear_dcc_comp_to_single_state(device);
}

static void
emit_color_clear(struct radv_cmd_buffer *cmd_buffer, const VkClearAttachment *clear_att, const VkClearRect *clear_rect,
                 uint32_t view_mask)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   uint32_t samples;
   VkFormat format;
   VkClearColorValue clear_value = clear_att->clearValue.color;
   VkCommandBuffer cmd_buffer_h = radv_cmd_buffer_to_handle(cmd_buffer);
   VkPipeline pipeline;
   VkResult result;

   assert(clear_att->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
   assert(clear_att->colorAttachment < render->color_att_count);
   const struct radv_attachment *color_att = &render->color_att[clear_att->colorAttachment];

   /* When a framebuffer is bound to the current command buffer, get the
    * number of samples from it. Otherwise, get the number of samples from
    * the render pass because it's likely a secondary command buffer.
    */
   if (color_att->iview) {
      samples = color_att->iview->image->vk.samples;
      format = color_att->iview->vk.format;
   } else {
      samples = render->max_samples;
      format = color_att->format;
   }
   assert(format != VK_FORMAT_UNDEFINED);

   assert(util_is_power_of_two_nonzero(samples));

   result = get_color_pipeline(device, samples, clear_att->colorAttachment, format, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return;
   }

   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), device->meta_state.clear_color_p_layout,
                              VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &clear_value);

   radv_CmdBindPipeline(cmd_buffer_h, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   radv_CmdSetViewport(radv_cmd_buffer_to_handle(cmd_buffer), 0, 1,
                       &(VkViewport){.x = clear_rect->rect.offset.x,
                                     .y = clear_rect->rect.offset.y,
                                     .width = clear_rect->rect.extent.width,
                                     .height = clear_rect->rect.extent.height,
                                     .minDepth = 0.0f,
                                     .maxDepth = 1.0f});

   radv_CmdSetScissor(radv_cmd_buffer_to_handle(cmd_buffer), 0, 1, &clear_rect->rect);

   if (view_mask) {
      u_foreach_bit (i, view_mask)
         radv_CmdDraw(cmd_buffer_h, 3, 1, 0, i);
   } else {
      radv_CmdDraw(cmd_buffer_h, 3, clear_rect->layerCount, 0, clear_rect->baseArrayLayer);
   }
}

static void
build_depthstencil_shader(struct radv_device *dev, struct nir_shader **out_vs, struct nir_shader **out_fs,
                          bool unrestricted)
{
   nir_builder vs_b = radv_meta_init_shader(
      dev, MESA_SHADER_VERTEX, unrestricted ? "meta_clear_depthstencil_unrestricted_vs" : "meta_clear_depthstencil_vs");
   nir_builder fs_b =
      radv_meta_init_shader(dev, MESA_SHADER_FRAGMENT,
                            unrestricted ? "meta_clear_depthstencil_unrestricted_fs" : "meta_clear_depthstencil_fs");

   const struct glsl_type *position_out_type = glsl_vec4_type();

   nir_variable *vs_out_pos = nir_variable_create(vs_b.shader, nir_var_shader_out, position_out_type, "gl_Position");
   vs_out_pos->data.location = VARYING_SLOT_POS;

   nir_def *z;
   if (unrestricted) {
      nir_def *in_color_load = nir_load_push_constant(&fs_b, 1, 32, nir_imm_int(&fs_b, 0), .range = 4);

      nir_variable *fs_out_depth = nir_variable_create(fs_b.shader, nir_var_shader_out, glsl_int_type(), "f_depth");
      fs_out_depth->data.location = FRAG_RESULT_DEPTH;
      nir_store_var(&fs_b, fs_out_depth, in_color_load, 0x1);

      z = nir_imm_float(&vs_b, 0.0);
   } else {
      z = nir_load_push_constant(&vs_b, 1, 32, nir_imm_int(&vs_b, 0), .range = 4);
   }

   nir_def *outvec = nir_gen_rect_vertices(&vs_b, z, NULL);
   nir_store_var(&vs_b, vs_out_pos, outvec, 0xf);

   const struct glsl_type *layer_type = glsl_int_type();
   nir_variable *vs_out_layer = nir_variable_create(vs_b.shader, nir_var_shader_out, layer_type, "v_layer");
   vs_out_layer->data.location = VARYING_SLOT_LAYER;
   vs_out_layer->data.interpolation = INTERP_MODE_FLAT;
   nir_def *inst_id = nir_load_instance_id(&vs_b);
   nir_def *base_instance = nir_load_base_instance(&vs_b);

   nir_def *layer_id = nir_iadd(&vs_b, inst_id, base_instance);
   nir_store_var(&vs_b, vs_out_layer, layer_id, 0x1);

   *out_vs = vs_b.shader;
   *out_fs = fs_b.shader;
}

static VkResult
create_depthstencil_pipeline(struct radv_device *device, VkImageAspectFlags aspects, uint32_t samples, int index,
                             bool unrestricted, VkPipeline *pipeline)
{
   struct nir_shader *vs_nir, *fs_nir;
   VkResult result;

   if (!device->meta_state.clear_depth_p_layout) {
      const VkPushConstantRange pc_range_depth = {
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
         .size = 4,
      };

      result =
         radv_meta_create_pipeline_layout(device, NULL, 1, &pc_range_depth, &device->meta_state.clear_depth_p_layout);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!device->meta_state.clear_depth_unrestricted_p_layout) {
      const VkPushConstantRange pc_range_depth_unrestricted = {
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
         .size = 4,
      };

      result = radv_meta_create_pipeline_layout(device, NULL, 1, &pc_range_depth_unrestricted,
                                                &device->meta_state.clear_depth_unrestricted_p_layout);
      if (result != VK_SUCCESS)
         return result;
   }

   build_depthstencil_shader(device, &vs_nir, &fs_nir, unrestricted);

   const VkPipelineVertexInputStateCreateInfo vi_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 0,
      .vertexAttributeDescriptionCount = 0,
   };

   const VkPipelineDepthStencilStateCreateInfo ds_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = !!(aspects & VK_IMAGE_ASPECT_DEPTH_BIT),
      .depthCompareOp = VK_COMPARE_OP_ALWAYS,
      .depthWriteEnable = !!(aspects & VK_IMAGE_ASPECT_DEPTH_BIT),
      .depthBoundsTestEnable = false,
      .stencilTestEnable = !!(aspects & VK_IMAGE_ASPECT_STENCIL_BIT),
      .front =
         {
            .passOp = VK_STENCIL_OP_REPLACE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .writeMask = UINT32_MAX,
            .reference = 0, /* dynamic */
         },
      .back = {0 /* dont care */},
      .minDepthBounds = 0.0f,
      .maxDepthBounds = 1.0f,
   };

   const VkPipelineColorBlendStateCreateInfo cb_state = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .logicOpEnable = false,
      .attachmentCount = 0,
      .pAttachments = NULL,
      .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
   };

   const VkPipelineRenderingCreateInfo rendering_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .depthAttachmentFormat = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED,
      .stencilAttachmentFormat = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_FORMAT_S8_UINT : VK_FORMAT_UNDEFINED,
   };

   struct radv_graphics_pipeline_create_info extra = {
      .use_rectlist = true,
   };

   if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
      extra.db_depth_clear = index == DEPTH_CLEAR_SLOW ? false : true;
   }
   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      extra.db_stencil_clear = index == DEPTH_CLEAR_SLOW ? false : true;
   }
   result = create_pipeline(device, samples, vs_nir, fs_nir, &vi_state, &ds_state, &cb_state, &rendering_create_info,
                            device->meta_state.clear_depth_p_layout, &extra, &device->meta_state.alloc, pipeline);

   return result;
}

static bool radv_can_fast_clear_depth(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                                      VkImageLayout image_layout, VkImageAspectFlags aspects,
                                      const VkClearRect *clear_rect, const VkClearDepthStencilValue clear_value,
                                      uint32_t view_mask);

static VkResult
get_depth_stencil_pipeline(struct radv_device *device, int samples_log2, VkImageAspectFlags aspects, bool fast,
                           VkPipeline *pipeline_out)
{
   struct radv_meta_state *meta_state = &device->meta_state;
   bool unrestricted = device->vk.enabled_extensions.EXT_depth_range_unrestricted;
   int index = fast ? DEPTH_CLEAR_FAST : DEPTH_CLEAR_SLOW;
   VkResult result = VK_SUCCESS;
   VkPipeline *pipeline;

   mtx_lock(&meta_state->mtx);

   switch (aspects) {
   case VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT:
      pipeline = unrestricted ? &meta_state->ds_clear[samples_log2].depthstencil_unrestricted_pipeline[index]
                              : &meta_state->ds_clear[samples_log2].depthstencil_pipeline[index];
      break;
   case VK_IMAGE_ASPECT_DEPTH_BIT:
      pipeline = unrestricted ? &meta_state->ds_clear[samples_log2].depth_only_unrestricted_pipeline[index]
                              : &meta_state->ds_clear[samples_log2].depth_only_pipeline[index];
      break;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      pipeline = unrestricted ? &meta_state->ds_clear[samples_log2].stencil_only_unrestricted_pipeline[index]
                              : &meta_state->ds_clear[samples_log2].stencil_only_pipeline[index];
      break;
   default:
      unreachable("expected depth or stencil aspect");
   }

   if (!*pipeline) {
      result = create_depthstencil_pipeline(device, aspects, 1u << samples_log2, index, unrestricted, pipeline);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = *pipeline;

fail:
   mtx_unlock(&meta_state->mtx);
   return result;
}

static void
emit_depthstencil_clear(struct radv_cmd_buffer *cmd_buffer, VkClearDepthStencilValue clear_value,
                        VkImageAspectFlags aspects, const VkClearRect *clear_rect, uint32_t view_mask,
                        bool can_fast_clear)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   uint32_t samples, samples_log2;
   VkCommandBuffer cmd_buffer_h = radv_cmd_buffer_to_handle(cmd_buffer);
   VkPipeline pipeline;
   VkResult result;

   /* When a framebuffer is bound to the current command buffer, get the
    * number of samples from it. Otherwise, get the number of samples from
    * the render pass because it's likely a secondary command buffer.
    */
   struct radv_image_view *iview = render->ds_att.iview;
   if (iview) {
      samples = iview->image->vk.samples;
   } else {
      assert(render->ds_att.format != VK_FORMAT_UNDEFINED);
      samples = render->max_samples;
   }

   assert(util_is_power_of_two_nonzero(samples));
   samples_log2 = ffs(samples) - 1;

   result = get_depth_stencil_pipeline(device, samples_log2, aspects, can_fast_clear, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return;
   }

   if (!(aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
      clear_value.depth = 1.0f;

   if (device->vk.enabled_extensions.EXT_depth_range_unrestricted) {
      vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                                 device->meta_state.clear_depth_unrestricted_p_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 4, &clear_value.depth);
   } else {
      vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), device->meta_state.clear_depth_p_layout,
                                 VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &clear_value.depth);
   }

   uint32_t prev_reference = cmd_buffer->state.dynamic.vk.ds.stencil.front.reference;
   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      radv_CmdSetStencilReference(cmd_buffer_h, VK_STENCIL_FACE_FRONT_BIT, clear_value.stencil);
   }

   radv_CmdBindPipeline(cmd_buffer_h, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

   if (can_fast_clear)
      radv_update_ds_clear_metadata(cmd_buffer, iview, clear_value, aspects);

   radv_CmdSetViewport(radv_cmd_buffer_to_handle(cmd_buffer), 0, 1,
                       &(VkViewport){.x = clear_rect->rect.offset.x,
                                     .y = clear_rect->rect.offset.y,
                                     .width = clear_rect->rect.extent.width,
                                     .height = clear_rect->rect.extent.height,
                                     .minDepth = 0.0f,
                                     .maxDepth = 1.0f});

   radv_CmdSetScissor(radv_cmd_buffer_to_handle(cmd_buffer), 0, 1, &clear_rect->rect);

   if (view_mask) {
      u_foreach_bit (i, view_mask)
         radv_CmdDraw(cmd_buffer_h, 3, 1, 0, i);
   } else {
      radv_CmdDraw(cmd_buffer_h, 3, clear_rect->layerCount, 0, clear_rect->baseArrayLayer);
   }

   if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
      radv_CmdSetStencilReference(cmd_buffer_h, VK_STENCIL_FACE_FRONT_BIT, prev_reference);
   }
}

static nir_shader *
build_clear_htile_mask_shader(struct radv_device *dev)
{
   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_clear_htile_mask");
   b.shader->info.workgroup_size[0] = 64;

   nir_def *global_id = get_global_ids(&b, 1);

   nir_def *offset = nir_imul_imm(&b, global_id, 16);
   offset = nir_channel(&b, offset, 0);

   nir_def *buf = radv_meta_load_descriptor(&b, 0, 0);

   nir_def *constants = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 0), .range = 8);

   nir_def *load = nir_load_ssbo(&b, 4, 32, buf, offset, .align_mul = 16);

   /* data = (data & ~htile_mask) | (htile_value & htile_mask) */
   nir_def *data = nir_iand(&b, load, nir_channel(&b, constants, 1));
   data = nir_ior(&b, data, nir_channel(&b, constants, 0));

   nir_store_ssbo(&b, data, buf, offset, .access = ACCESS_NON_READABLE, .align_mul = 16);

   return b.shader;
}

static VkResult
create_clear_htile_mask_pipeline(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result;

   const VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };

   result = radv_meta_create_descriptor_set_layout(device, 1, &binding, &state->clear_htile_mask_ds_layout);
   if (result != VK_SUCCESS)
      return result;

   const VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .size = 8,
   };

   result = radv_meta_create_pipeline_layout(device, &state->clear_htile_mask_ds_layout, 1, &pc_range,
                                             &state->clear_htile_mask_p_layout);
   if (result != VK_SUCCESS)
      return result;

   nir_shader *cs = build_clear_htile_mask_shader(device);

   result = radv_meta_create_compute_pipeline(device, cs, state->clear_htile_mask_p_layout,
                                              &state->clear_htile_mask_pipeline);

   ralloc_free(cs);
   return result;
}

static VkResult
get_clear_htile_mask_pipeline(struct radv_device *device, VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result = VK_SUCCESS;

   mtx_lock(&state->mtx);
   if (!state->clear_htile_mask_pipeline) {
      result = create_clear_htile_mask_pipeline(device);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = state->clear_htile_mask_pipeline;

fail:
   mtx_unlock(&state->mtx);
   return result;
}

static uint32_t
clear_htile_mask(struct radv_cmd_buffer *cmd_buffer, const struct radv_image *image, struct radeon_winsys_bo *bo,
                 uint64_t offset, uint64_t size, uint32_t htile_value, uint32_t htile_mask)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_state *state = &device->meta_state;
   uint64_t block_count = DIV_ROUND_UP(size, 1024);
   struct radv_meta_saved_state saved_state;
   struct radv_buffer dst_buffer;
   VkPipeline pipeline;
   VkResult result;

   result = get_clear_htile_mask_pipeline(device, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return 0;
   }

   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SAVE_DESCRIPTORS);

   radv_buffer_init(&dst_buffer, device, bo, size, offset);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   radv_meta_push_descriptor_set(
      cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, state->clear_htile_mask_p_layout, 0, 1,
      (VkWriteDescriptorSet[]){
         {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstBinding = 0,
          .dstArrayElement = 0,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo =
             &(VkDescriptorBufferInfo){.buffer = radv_buffer_to_handle(&dst_buffer), .offset = 0, .range = size}}});

   const unsigned constants[2] = {
      htile_value & htile_mask,
      ~htile_mask,
   };

   vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), state->clear_htile_mask_p_layout,
                              VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, constants);

   vk_common_CmdDispatch(radv_cmd_buffer_to_handle(cmd_buffer), block_count, 1, 1);

   radv_buffer_finish(&dst_buffer);

   radv_meta_restore(&saved_state, cmd_buffer);

   return RADV_CMD_FLAG_CS_PARTIAL_FLUSH | radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                                 VK_ACCESS_2_SHADER_WRITE_BIT, image);
}

static uint32_t
radv_get_htile_fast_clear_value(const struct radv_device *device, const struct radv_image *image,
                                VkClearDepthStencilValue value)
{
   uint32_t max_zval = 0x3fff; /* maximum 14-bit value. */
   uint32_t zmask = 0, smem = 0;
   uint32_t htile_value;
   uint32_t zmin, zmax;

   /* Convert the depth value to 14-bit zmin/zmax values. */
   zmin = lroundf(value.depth * max_zval);
   zmax = zmin;

   if (radv_image_tile_stencil_disabled(device, image)) {
      /* Z only (no stencil):
       *
       * |31     18|17      4|3     0|
       * +---------+---------+-------+
       * |  Max Z  |  Min Z  | ZMask |
       */
      htile_value = (((zmax & 0x3fff) << 18) | ((zmin & 0x3fff) << 4) | ((zmask & 0xf) << 0));
   } else {

      /* Z and stencil:
       *
       * |31       12|11 10|9    8|7   6|5   4|3     0|
       * +-----------+-----+------+-----+-----+-------+
       * |  Z Range  |     | SMem | SR1 | SR0 | ZMask |
       *
       * Z, stencil, 4 bit VRS encoding:
       * |31       12| 11      10 |9    8|7         6 |5   4|3     0|
       * +-----------+------------+------+------------+-----+-------+
       * |  Z Range  | VRS Y-rate | SMem | VRS X-rate | SR0 | ZMask |
       */
      uint32_t delta = 0;
      uint32_t zrange = ((zmax << 6) | delta);
      uint32_t sresults = 0xf; /* SR0/SR1 both as 0x3. */

      if (radv_image_has_vrs_htile(device, image))
         sresults = 0x3;

      htile_value = (((zrange & 0xfffff) << 12) | ((smem & 0x3) << 8) | ((sresults & 0xf) << 4) | ((zmask & 0xf) << 0));
   }

   return htile_value;
}

static uint32_t
radv_get_htile_mask(const struct radv_device *device, const struct radv_image *image, VkImageAspectFlags aspects)
{
   uint32_t mask = 0;

   if (radv_image_tile_stencil_disabled(device, image)) {
      /* All the HTILE buffer is used when there is no stencil. */
      mask = UINT32_MAX;
   } else {
      if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
         mask |= 0xfffffc0f;
      if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
         mask |= 0x000003f0;
   }

   return mask;
}

static bool
radv_is_fast_clear_depth_allowed(VkClearDepthStencilValue value)
{
   return value.depth == 1.0f || value.depth == 0.0f;
}

static bool
radv_is_fast_clear_stencil_allowed(VkClearDepthStencilValue value)
{
   return value.stencil == 0;
}

static bool
radv_can_fast_clear_depth(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                          VkImageLayout image_layout, VkImageAspectFlags aspects, const VkClearRect *clear_rect,
                          const VkClearDepthStencilValue clear_value, uint32_t view_mask)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);

   if (!iview || !iview->support_fast_clear)
      return false;

   if (!radv_layout_is_htile_compressed(device, iview->image, image_layout,
                                        radv_image_queue_family_mask(iview->image, cmd_buffer->qf, cmd_buffer->qf)))
      return false;

   if (clear_rect->rect.offset.x || clear_rect->rect.offset.y ||
       clear_rect->rect.extent.width != iview->image->vk.extent.width ||
       clear_rect->rect.extent.height != iview->image->vk.extent.height)
      return false;

   if (view_mask && (iview->image->vk.array_layers >= 32 || (1u << iview->image->vk.array_layers) - 1u != view_mask))
      return false;
   if (!view_mask && clear_rect->baseArrayLayer != 0)
      return false;
   if (!view_mask && clear_rect->layerCount != iview->image->vk.array_layers)
      return false;

   if (device->vk.enabled_extensions.EXT_depth_range_unrestricted && (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
       (clear_value.depth < 0.0 || clear_value.depth > 1.0))
      return false;

   if (radv_image_is_tc_compat_htile(iview->image) &&
       (((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) && !radv_is_fast_clear_depth_allowed(clear_value)) ||
        ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) && !radv_is_fast_clear_stencil_allowed(clear_value))))
      return false;

   if (iview->image->vk.mip_levels > 1) {
      uint32_t last_level = iview->vk.base_mip_level + iview->vk.level_count - 1;
      if (last_level >= iview->image->planes[0].surface.num_meta_levels) {
         /* Do not fast clears if one level can't be fast cleared. */
         return false;
      }
   }

   return true;
}

static void
radv_fast_clear_depth(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                      VkClearDepthStencilValue clear_value, VkImageAspectFlags aspects,
                      enum radv_cmd_flush_bits *pre_flush, enum radv_cmd_flush_bits *post_flush)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   uint32_t clear_word, flush_bits;

   clear_word = radv_get_htile_fast_clear_value(device, iview->image, clear_value);

   if (pre_flush) {
      enum radv_cmd_flush_bits bits =
         radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, iview->image) |
         radv_dst_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                               iview->image);
      cmd_buffer->state.flush_bits |= bits & ~*pre_flush;
      *pre_flush |= cmd_buffer->state.flush_bits;
   }

   VkImageSubresourceRange range = {
      .aspectMask = aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };

   flush_bits = radv_clear_htile(cmd_buffer, iview->image, &range, clear_word);

   if (iview->image->planes[0].surface.has_stencil &&
       !(aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
      /* Synchronize after performing a depth-only or a stencil-only
       * fast clear because the driver uses an optimized path which
       * performs a read-modify-write operation, and the two separate
       * aspects might use the same HTILE memory.
       */
      cmd_buffer->state.flush_bits |= flush_bits;
   }

   radv_update_ds_clear_metadata(cmd_buffer, iview, clear_value, aspects);
   if (post_flush) {
      *post_flush |= flush_bits;
   }
}

/* Clear DCC using comp-to-single by storing the clear value at the beginning of every 256B block.
 * For MSAA images, clearing the first sample should be enough as long as CMASK is also cleared.
 */
static nir_shader *
build_clear_dcc_comp_to_single_shader(struct radv_device *dev, bool is_msaa)
{
   enum glsl_sampler_dim dim = is_msaa ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D;
   const struct glsl_type *img_type = glsl_image_type(dim, true, GLSL_TYPE_FLOAT);

   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_COMPUTE, "meta_clear_dcc_comp_to_single-%s",
                                         is_msaa ? "multisampled" : "singlesampled");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;

   nir_def *global_id = get_global_ids(&b, 3);

   /* Load the dimensions in pixels of a block that gets compressed to one DCC byte. */
   nir_def *dcc_block_size = nir_load_push_constant(&b, 2, 32, nir_imm_int(&b, 0), .range = 8);

   /* Compute the coordinates. */
   nir_def *coord = nir_trim_vector(&b, global_id, 2);
   coord = nir_imul(&b, coord, dcc_block_size);
   coord = nir_vec4(&b, nir_channel(&b, coord, 0), nir_channel(&b, coord, 1), nir_channel(&b, global_id, 2),
                    nir_undef(&b, 1, 32));

   nir_variable *output_img = nir_variable_create(b.shader, nir_var_image, img_type, "out_img");
   output_img->data.descriptor_set = 0;
   output_img->data.binding = 0;

   /* Load the clear color values. */
   nir_def *clear_values = nir_load_push_constant(&b, 4, 32, nir_imm_int(&b, 8), .range = 24);

   nir_def *data = nir_vec4(&b, nir_channel(&b, clear_values, 0), nir_channel(&b, clear_values, 1),
                            nir_channel(&b, clear_values, 2), nir_channel(&b, clear_values, 3));

   /* Store the clear color values. */
   nir_def *sample_id = is_msaa ? nir_imm_int(&b, 0) : nir_undef(&b, 1, 32);
   nir_image_deref_store(&b, &nir_build_deref_var(&b, output_img)->def, coord, sample_id, data, nir_imm_int(&b, 0),
                         .image_dim = dim, .image_array = true);

   return b.shader;
}

static VkResult
create_dcc_comp_to_single_pipeline(struct radv_device *device, bool is_msaa, VkPipeline *pipeline)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result;

   if (!state->clear_dcc_comp_to_single_ds_layout) {
      const VkDescriptorSetLayoutBinding binding = {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      };

      result = radv_meta_create_descriptor_set_layout(device, 1, &binding, &state->clear_dcc_comp_to_single_ds_layout);
      if (result != VK_SUCCESS)
         return result;
   }

   if (!state->clear_dcc_comp_to_single_p_layout) {
      const VkPushConstantRange pc_range = {
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
         .size = 24,
      };

      result = radv_meta_create_pipeline_layout(device, &state->clear_dcc_comp_to_single_ds_layout, 1, &pc_range,
                                                &state->clear_dcc_comp_to_single_p_layout);
      if (result != VK_SUCCESS)
         return result;
   }

   nir_shader *cs = build_clear_dcc_comp_to_single_shader(device, is_msaa);

   result = radv_meta_create_compute_pipeline(device, cs, state->clear_dcc_comp_to_single_p_layout, pipeline);

   ralloc_free(cs);
   return result;
}

static VkResult
init_meta_clear_dcc_comp_to_single_state(struct radv_device *device)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result;

   for (uint32_t i = 0; i < 2; i++) {
      result = create_dcc_comp_to_single_pipeline(device, !!i, &state->clear_dcc_comp_to_single_pipeline[i]);
      if (result != VK_SUCCESS)
         return result;
   }

   return result;
}

VkResult
radv_device_init_meta_clear_state(struct radv_device *device, bool on_demand)
{
   VkResult res;
   struct radv_meta_state *state = &device->meta_state;

   if (on_demand)
      return VK_SUCCESS;

   res = init_meta_clear_dcc_comp_to_single_state(device);
   if (res != VK_SUCCESS)
      return res;

   res = create_clear_htile_mask_pipeline(device);
   if (res != VK_SUCCESS)
      return res;

   for (uint32_t i = 0; i < ARRAY_SIZE(state->color_clear); ++i) {
      uint32_t samples = 1 << i;

      /* Only precompile meta pipelines for attachment 0 as other are uncommon. */
      for (uint32_t j = 0; j < NUM_META_FS_KEYS; ++j) {
         VkFormat format = radv_fs_key_format_exemplars[j];
         unsigned fs_key = radv_format_meta_fs_key(device, format);
         assert(!state->color_clear[i][0].color_pipelines[fs_key]);

         res = create_color_pipeline(device, samples, 0, format, &state->color_clear[i][0].color_pipelines[fs_key]);
         if (res != VK_SUCCESS)
            return res;
      }
   }
   for (uint32_t i = 0; i < ARRAY_SIZE(state->ds_clear); ++i) {
      uint32_t samples = 1 << i;

      for (uint32_t j = 0; j < NUM_DEPTH_CLEAR_PIPELINES; j++) {
         res = create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_DEPTH_BIT, samples, j, false,
                                            &state->ds_clear[i].depth_only_pipeline[j]);
         if (res != VK_SUCCESS)
            return res;

         res = create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_STENCIL_BIT, samples, j, false,
                                            &state->ds_clear[i].stencil_only_pipeline[j]);
         if (res != VK_SUCCESS)
            return res;

         res = create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, samples, j,
                                            false, &state->ds_clear[i].depthstencil_pipeline[j]);
         if (res != VK_SUCCESS)
            return res;

         res = create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_DEPTH_BIT, samples, j, true,
                                            &state->ds_clear[i].depth_only_unrestricted_pipeline[j]);
         if (res != VK_SUCCESS)
            return res;

         res = create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_STENCIL_BIT, samples, j, true,
                                            &state->ds_clear[i].stencil_only_unrestricted_pipeline[j]);
         if (res != VK_SUCCESS)
            return res;

         res = create_depthstencil_pipeline(device, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, samples, j,
                                            true, &state->ds_clear[i].depthstencil_unrestricted_pipeline[j]);
         if (res != VK_SUCCESS)
            return res;
      }
   }
   return VK_SUCCESS;
}

static uint32_t
radv_get_cmask_fast_clear_value(const struct radv_image *image)
{
   uint32_t value = 0; /* Default value when no DCC. */

   /* The fast-clear value is different for images that have both DCC and
    * CMASK metadata.
    */
   if (radv_image_has_dcc(image)) {
      /* DCC fast clear with MSAA should clear CMASK to 0xC. */
      return image->vk.samples > 1 ? 0xcccccccc : 0xffffffff;
   }

   return value;
}

uint32_t
radv_clear_cmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range,
                 uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t cmask_offset = image->planes[0].surface.cmask_offset;
   uint64_t size;

   if (pdev->info.gfx_level == GFX9) {
      /* TODO: clear layers. */
      size = image->planes[0].surface.cmask_size;
   } else {
      unsigned slice_size = image->planes[0].surface.cmask_slice_size;

      cmask_offset += slice_size * range->baseArrayLayer;
      size = slice_size * vk_image_subresource_layer_count(&image->vk, range);
   }

   return radv_fill_buffer(cmd_buffer, image, image->bindings[0].bo, radv_image_get_va(image, 0) + cmask_offset, size,
                           value);
}

uint32_t
radv_clear_fmask(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range,
                 uint32_t value)
{
   uint64_t fmask_offset = image->planes[0].surface.fmask_offset;
   unsigned slice_size = image->planes[0].surface.fmask_slice_size;
   uint64_t size;

   /* MSAA images do not support mipmap levels. */
   assert(range->baseMipLevel == 0 && vk_image_subresource_level_count(&image->vk, range) == 1);

   fmask_offset += slice_size * range->baseArrayLayer;
   size = slice_size * vk_image_subresource_layer_count(&image->vk, range);

   return radv_fill_buffer(cmd_buffer, image, image->bindings[0].bo, radv_image_get_va(image, 0) + fmask_offset, size,
                           value);
}

uint32_t
radv_clear_dcc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, const VkImageSubresourceRange *range,
               uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);
   uint32_t layer_count = vk_image_subresource_layer_count(&image->vk, range);
   uint32_t flush_bits = 0;

   /* Mark the image as being compressed. */
   radv_update_dcc_metadata(cmd_buffer, image, range, true);

   for (uint32_t l = 0; l < level_count; l++) {
      uint64_t dcc_offset = image->planes[0].surface.meta_offset;
      uint32_t level = range->baseMipLevel + l;
      uint64_t size;

      if (pdev->info.gfx_level >= GFX10) {
         /* DCC for mipmaps+layers is currently disabled. */
         dcc_offset += image->planes[0].surface.meta_slice_size * range->baseArrayLayer +
                       image->planes[0].surface.u.gfx9.meta_levels[level].offset;
         size = image->planes[0].surface.u.gfx9.meta_levels[level].size * layer_count;
      } else if (pdev->info.gfx_level == GFX9) {
         /* Mipmap levels and layers aren't implemented. */
         assert(level == 0);
         size = image->planes[0].surface.meta_size;
      } else {
         const struct legacy_surf_dcc_level *dcc_level = &image->planes[0].surface.u.legacy.color.dcc_level[level];

         /* If dcc_fast_clear_size is 0 (which might happens for
          * mipmaps) the fill buffer operation below is a no-op.
          * This can only happen during initialization as the
          * fast clear path fallbacks to slow clears if one
          * level can't be fast cleared.
          */
         dcc_offset += dcc_level->dcc_offset + dcc_level->dcc_slice_fast_clear_size * range->baseArrayLayer;
         size = dcc_level->dcc_slice_fast_clear_size * vk_image_subresource_layer_count(&image->vk, range);
      }

      /* Do not clear this level if it can't be compressed. */
      if (!size)
         continue;

      flush_bits |= radv_fill_buffer(cmd_buffer, image, image->bindings[0].bo, radv_image_get_va(image, 0) + dcc_offset,
                                     size, value);
   }

   return flush_bits;
}

static VkResult
get_clear_dcc_comp_to_single_pipeline(struct radv_device *device, bool is_msaa, VkPipeline *pipeline_out)
{
   struct radv_meta_state *state = &device->meta_state;
   VkResult result = VK_SUCCESS;

   mtx_lock(&state->mtx);
   if (!state->clear_dcc_comp_to_single_pipeline[is_msaa]) {
      result = create_dcc_comp_to_single_pipeline(device, is_msaa, &state->clear_dcc_comp_to_single_pipeline[is_msaa]);
      if (result != VK_SUCCESS)
         goto fail;
   }

   *pipeline_out = state->clear_dcc_comp_to_single_pipeline[is_msaa];

fail:
   mtx_unlock(&state->mtx);
   return result;
}

static uint32_t
radv_clear_dcc_comp_to_single(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image,
                              const VkImageSubresourceRange *range, uint32_t color_values[4])
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   unsigned bytes_per_pixel = vk_format_get_blocksize(image->vk.format);
   unsigned layer_count = vk_image_subresource_layer_count(&image->vk, range);
   struct radv_meta_saved_state saved_state;
   bool is_msaa = image->vk.samples > 1;
   struct radv_image_view iview;
   VkPipeline pipeline;
   VkResult result;
   VkFormat format;

   switch (bytes_per_pixel) {
   case 1:
      format = VK_FORMAT_R8_UINT;
      break;
   case 2:
      format = VK_FORMAT_R16_UINT;
      break;
   case 4:
      format = VK_FORMAT_R32_UINT;
      break;
   case 8:
      format = VK_FORMAT_R32G32_UINT;
      break;
   case 16:
      format = VK_FORMAT_R32G32B32A32_UINT;
      break;
   default:
      unreachable("Unsupported number of bytes per pixel");
   }

   result = get_clear_dcc_comp_to_single_pipeline(device, is_msaa, &pipeline);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, result);
      return 0;
   }

   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_DESCRIPTORS | RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_CONSTANTS);

   radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

   for (uint32_t l = 0; l < vk_image_subresource_level_count(&image->vk, range); l++) {
      uint32_t width, height;

      /* Do not write the clear color value for levels without DCC. */
      if (!radv_dcc_enabled(image, range->baseMipLevel + l))
         continue;

      width = u_minify(image->vk.extent.width, range->baseMipLevel + l);
      height = u_minify(image->vk.extent.height, range->baseMipLevel + l);

      radv_image_view_init(&iview, device,
                           &(VkImageViewCreateInfo){
                              .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                              .image = radv_image_to_handle(image),
                              .viewType = VK_IMAGE_VIEW_TYPE_2D,
                              .format = format,
                              .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                   .baseMipLevel = range->baseMipLevel + l,
                                                   .levelCount = 1,
                                                   .baseArrayLayer = range->baseArrayLayer,
                                                   .layerCount = layer_count},
                           },
                           &(struct radv_image_view_extra_create_info){.disable_compression = true});

      radv_meta_push_descriptor_set(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    device->meta_state.clear_dcc_comp_to_single_p_layout, 0, 1,
                                    (VkWriteDescriptorSet[]){{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                              .dstBinding = 0,
                                                              .dstArrayElement = 0,
                                                              .descriptorCount = 1,
                                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                              .pImageInfo = (VkDescriptorImageInfo[]){
                                                                 {
                                                                    .sampler = VK_NULL_HANDLE,
                                                                    .imageView = radv_image_view_to_handle(&iview),
                                                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                                 },
                                                              }}});

      unsigned dcc_width = DIV_ROUND_UP(width, image->planes[0].surface.u.gfx9.color.dcc_block_width);
      unsigned dcc_height = DIV_ROUND_UP(height, image->planes[0].surface.u.gfx9.color.dcc_block_height);

      const unsigned constants[6] = {
         image->planes[0].surface.u.gfx9.color.dcc_block_width,
         image->planes[0].surface.u.gfx9.color.dcc_block_height,
         color_values[0],
         color_values[1],
         color_values[2],
         color_values[3],
      };

      vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer),
                                 device->meta_state.clear_dcc_comp_to_single_p_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 24, constants);

      radv_unaligned_dispatch(cmd_buffer, dcc_width, dcc_height, layer_count);

      radv_image_view_finish(&iview);
   }

   radv_meta_restore(&saved_state, cmd_buffer);

   return RADV_CMD_FLAG_CS_PARTIAL_FLUSH | radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                                 VK_ACCESS_2_SHADER_WRITE_BIT, image);
}

uint32_t
radv_clear_htile(struct radv_cmd_buffer *cmd_buffer, const struct radv_image *image,
                 const VkImageSubresourceRange *range, uint32_t value)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t level_count = vk_image_subresource_level_count(&image->vk, range);
   uint32_t flush_bits = 0;
   uint32_t htile_mask;

   htile_mask = radv_get_htile_mask(device, image, range->aspectMask);

   if (level_count != image->vk.mip_levels) {
      assert(pdev->info.gfx_level >= GFX10);

      /* Clear individuals levels separately. */
      for (uint32_t l = 0; l < level_count; l++) {
         uint32_t level = range->baseMipLevel + l;
         uint64_t htile_offset =
            image->planes[0].surface.meta_offset + image->planes[0].surface.u.gfx9.meta_levels[level].offset;
         uint32_t size = image->planes[0].surface.u.gfx9.meta_levels[level].size;

         /* Do not clear this level if it can be compressed. */
         if (!size)
            continue;

         if (htile_mask == UINT_MAX) {
            /* Clear the whole HTILE buffer. */
            flush_bits |= radv_fill_buffer(cmd_buffer, image, image->bindings[0].bo,
                                           radv_image_get_va(image, 0) + htile_offset, size, value);
         } else {
            /* Only clear depth or stencil bytes in the HTILE buffer. */
            flush_bits |= clear_htile_mask(cmd_buffer, image, image->bindings[0].bo,
                                           image->bindings[0].offset + htile_offset, size, value, htile_mask);
         }
      }
   } else {
      unsigned layer_count = vk_image_subresource_layer_count(&image->vk, range);
      uint64_t size = image->planes[0].surface.meta_slice_size * layer_count;
      uint64_t htile_offset =
         image->planes[0].surface.meta_offset + image->planes[0].surface.meta_slice_size * range->baseArrayLayer;

      if (htile_mask == UINT_MAX) {
         /* Clear the whole HTILE buffer. */
         flush_bits = radv_fill_buffer(cmd_buffer, image, image->bindings[0].bo,
                                       radv_image_get_va(image, 0) + htile_offset, size, value);
      } else {
         /* Only clear depth or stencil bytes in the HTILE buffer. */
         flush_bits = clear_htile_mask(cmd_buffer, image, image->bindings[0].bo,
                                       image->bindings[0].offset + htile_offset, size, value, htile_mask);
      }
   }

   return flush_bits;
}

enum {
   RADV_DCC_CLEAR_0000 = 0x00000000U,
   RADV_DCC_GFX8_CLEAR_0001 = 0x40404040U,
   RADV_DCC_GFX8_CLEAR_1110 = 0x80808080U,
   RADV_DCC_GFX8_CLEAR_1111 = 0xC0C0C0C0U,
   RADV_DCC_GFX8_CLEAR_REG = 0x20202020U,
   RADV_DCC_GFX9_CLEAR_SINGLE = 0x10101010U,
   RADV_DCC_GFX11_CLEAR_SINGLE = 0x01010101U,
   RADV_DCC_GFX11_CLEAR_0000 = 0x00000000U,
   RADV_DCC_GFX11_CLEAR_1111_UNORM = 0x02020202U,
   RADV_DCC_GFX11_CLEAR_1111_FP16 = 0x04040404U,
   RADV_DCC_GFX11_CLEAR_1111_FP32 = 0x06060606U,
   RADV_DCC_GFX11_CLEAR_0001_UNORM = 0x08080808U,
   RADV_DCC_GFX11_CLEAR_1110_UNORM = 0x0A0A0A0AU,
};

static uint32_t
radv_dcc_single_clear_value(const struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   return pdev->info.gfx_level >= GFX11 ? RADV_DCC_GFX11_CLEAR_SINGLE : RADV_DCC_GFX9_CLEAR_SINGLE;
}

static void
gfx8_get_fast_clear_parameters(struct radv_device *device, const struct radv_image_view *iview,
                               const VkClearColorValue *clear_value, uint32_t *reset_value,
                               bool *can_avoid_fast_clear_elim)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   bool values[4] = {0};
   int extra_channel;
   bool main_value = false;
   bool extra_value = false;
   bool has_color = false;
   bool has_alpha = false;

   /* comp-to-single allows to perform DCC fast clears without requiring a FCE. */
   if (iview->image->support_comp_to_single) {
      *reset_value = RADV_DCC_GFX9_CLEAR_SINGLE;
      *can_avoid_fast_clear_elim = true;
   } else {
      *reset_value = RADV_DCC_GFX8_CLEAR_REG;
      *can_avoid_fast_clear_elim = false;
   }

   const struct util_format_description *desc = vk_format_description(iview->vk.format);
   if (iview->vk.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || iview->vk.format == VK_FORMAT_R5G6B5_UNORM_PACK16 ||
       iview->vk.format == VK_FORMAT_B5G6R5_UNORM_PACK16)
      extra_channel = -1;
   else if (desc->layout == UTIL_FORMAT_LAYOUT_PLAIN) {
      if (ac_alpha_is_on_msb(&pdev->info, radv_format_to_pipe_format(iview->vk.format)))
         extra_channel = desc->nr_channels - 1;
      else
         extra_channel = 0;
   } else
      return;

   for (int i = 0; i < 4; i++) {
      int index = desc->swizzle[i] - PIPE_SWIZZLE_X;
      if (desc->swizzle[i] < PIPE_SWIZZLE_X || desc->swizzle[i] > PIPE_SWIZZLE_W)
         continue;

      if (desc->channel[i].pure_integer && desc->channel[i].type == UTIL_FORMAT_TYPE_SIGNED) {
         /* Use the maximum value for clamping the clear color. */
         int max = u_bit_consecutive(0, desc->channel[i].size - 1);

         values[i] = clear_value->int32[i] != 0;
         if (clear_value->int32[i] != 0 && MIN2(clear_value->int32[i], max) != max)
            return;
      } else if (desc->channel[i].pure_integer && desc->channel[i].type == UTIL_FORMAT_TYPE_UNSIGNED) {
         /* Use the maximum value for clamping the clear color. */
         unsigned max = u_bit_consecutive(0, desc->channel[i].size);

         values[i] = clear_value->uint32[i] != 0U;
         if (clear_value->uint32[i] != 0U && MIN2(clear_value->uint32[i], max) != max)
            return;
      } else {
         values[i] = clear_value->float32[i] != 0.0F;
         if (clear_value->float32[i] != 0.0F && clear_value->float32[i] != 1.0F)
            return;
      }

      if (index == extra_channel) {
         extra_value = values[i];
         has_alpha = true;
      } else {
         main_value = values[i];
         has_color = true;
      }
   }

   /* If alpha isn't present, make it the same as color, and vice versa. */
   if (!has_alpha)
      extra_value = main_value;
   else if (!has_color)
      main_value = extra_value;

   for (int i = 0; i < 4; ++i)
      if (values[i] != main_value && desc->swizzle[i] - PIPE_SWIZZLE_X != extra_channel &&
          desc->swizzle[i] >= PIPE_SWIZZLE_X && desc->swizzle[i] <= PIPE_SWIZZLE_W)
         return;

   /* Only DCC clear code 0000 is allowed for signed<->unsigned formats. */
   if ((main_value || extra_value) && iview->image->dcc_sign_reinterpret)
      return;

   *can_avoid_fast_clear_elim = true;

   if (main_value) {
      if (extra_value)
         *reset_value = RADV_DCC_GFX8_CLEAR_1111;
      else
         *reset_value = RADV_DCC_GFX8_CLEAR_1110;
   } else {
      if (extra_value)
         *reset_value = RADV_DCC_GFX8_CLEAR_0001;
      else
         *reset_value = RADV_DCC_CLEAR_0000;
   }
}

static bool
gfx11_get_fast_clear_parameters(struct radv_device *device, const struct radv_image_view *iview,
                                const VkClearColorValue *clear_value, uint32_t *reset_value)
{
   const struct util_format_description *desc = vk_format_description(iview->vk.format);
   unsigned start_bit = UINT_MAX;
   unsigned end_bit = 0;

   /* TODO: 8bpp and 16bpp fast DCC clears don't work. */
   if (desc->block.bits <= 16)
      return false;

   /* Find the used bit range. */
   for (unsigned i = 0; i < 4; i++) {
      unsigned swizzle = desc->swizzle[i];

      if (swizzle >= PIPE_SWIZZLE_0)
         continue;

      start_bit = MIN2(start_bit, desc->channel[swizzle].shift);
      end_bit = MAX2(end_bit, desc->channel[swizzle].shift + desc->channel[swizzle].size);
   }

   union {
      uint8_t ub[16];
      uint16_t us[8];
      uint32_t ui[4];
   } value;
   memset(&value, 0, sizeof(value));
   util_format_pack_rgba(radv_format_to_pipe_format(iview->vk.format), &value, clear_value, 1);

   /* Check the cases where all components or bits are either all 0 or all 1. */
   bool all_bits_are_0 = true;
   bool all_bits_are_1 = true;
   bool all_words_are_fp16_1 = false;
   bool all_words_are_fp32_1 = false;

   for (unsigned i = start_bit; i < end_bit; i++) {
      bool bit = value.ub[i / 8] & BITFIELD_BIT(i % 8);

      all_bits_are_0 &= !bit;
      all_bits_are_1 &= bit;
   }

   if (start_bit % 16 == 0 && end_bit % 16 == 0) {
      all_words_are_fp16_1 = true;
      for (unsigned i = start_bit / 16; i < end_bit / 16; i++)
         all_words_are_fp16_1 &= value.us[i] == 0x3c00;
   }

   if (start_bit % 32 == 0 && end_bit % 32 == 0) {
      all_words_are_fp32_1 = true;
      for (unsigned i = start_bit / 32; i < end_bit / 32; i++)
         all_words_are_fp32_1 &= value.ui[i] == 0x3f800000;
   }

   if (all_bits_are_0 || all_bits_are_1 || all_words_are_fp16_1 || all_words_are_fp32_1) {
      if (all_bits_are_0)
         *reset_value = RADV_DCC_CLEAR_0000;
      else if (all_bits_are_1)
         *reset_value = RADV_DCC_GFX11_CLEAR_1111_UNORM;
      else if (all_words_are_fp16_1)
         *reset_value = RADV_DCC_GFX11_CLEAR_1111_FP16;
      else if (all_words_are_fp32_1)
         *reset_value = RADV_DCC_GFX11_CLEAR_1111_FP32;
      return true;
   }

   if (desc->nr_channels == 2 && desc->channel[0].size == 8) {
      if (value.ub[0] == 0x00 && value.ub[1] == 0xff) {
         *reset_value = RADV_DCC_GFX11_CLEAR_0001_UNORM;
         return true;
      } else if (value.ub[0] == 0xff && value.ub[1] == 0x00) {
         *reset_value = RADV_DCC_GFX11_CLEAR_1110_UNORM;
         return true;
      }
   } else if (desc->nr_channels == 4 && desc->channel[0].size == 8) {
      if (value.ub[0] == 0x00 && value.ub[1] == 0x00 && value.ub[2] == 0x00 && value.ub[3] == 0xff) {
         *reset_value = RADV_DCC_GFX11_CLEAR_0001_UNORM;
         return true;
      } else if (value.ub[0] == 0xff && value.ub[1] == 0xff && value.ub[2] == 0xff && value.ub[3] == 0x00) {
         *reset_value = RADV_DCC_GFX11_CLEAR_1110_UNORM;
         return true;
      }
   } else if (desc->nr_channels == 4 && desc->channel[0].size == 16) {
      if (value.us[0] == 0x0000 && value.us[1] == 0x0000 && value.us[2] == 0x0000 && value.us[3] == 0xffff) {
         *reset_value = RADV_DCC_GFX11_CLEAR_0001_UNORM;
         return true;
      } else if (value.us[0] == 0xffff && value.us[1] == 0xffff && value.us[2] == 0xffff && value.us[3] == 0x0000) {
         *reset_value = RADV_DCC_GFX11_CLEAR_1110_UNORM;
         return true;
      }
   }

   if (iview->image->support_comp_to_single) {
      *reset_value = RADV_DCC_GFX11_CLEAR_SINGLE;
      return true;
   }

   return false;
}

static bool
radv_can_fast_clear_color(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                          VkImageLayout image_layout, const VkClearRect *clear_rect, VkClearColorValue clear_value,
                          uint32_t view_mask)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t clear_color[2];

   if (!iview || !iview->support_fast_clear)
      return false;

   if (!radv_layout_can_fast_clear(device, iview->image, iview->vk.base_mip_level, image_layout,
                                   radv_image_queue_family_mask(iview->image, cmd_buffer->qf, cmd_buffer->qf)))
      return false;

   if (clear_rect->rect.offset.x || clear_rect->rect.offset.y ||
       clear_rect->rect.extent.width != iview->image->vk.extent.width ||
       clear_rect->rect.extent.height != iview->image->vk.extent.height)
      return false;

   if (view_mask && (iview->image->vk.array_layers >= 32 || (1u << iview->image->vk.array_layers) - 1u != view_mask))
      return false;
   if (!view_mask && clear_rect->baseArrayLayer != 0)
      return false;
   if (!view_mask && clear_rect->layerCount != iview->image->vk.array_layers)
      return false;

   /* DCC */

   /* Images that support comp-to-single clears don't have clear values. */
   if (!iview->image->support_comp_to_single) {
      if (!radv_format_pack_clear_color(iview->vk.format, clear_color, &clear_value))
         return false;

      if (!radv_image_has_clear_value(iview->image) && (clear_color[0] != 0 || clear_color[1] != 0))
         return false;
   }

   if (radv_dcc_enabled(iview->image, iview->vk.base_mip_level)) {
      bool can_avoid_fast_clear_elim;
      uint32_t reset_value;

      if (pdev->info.gfx_level >= GFX11) {
         if (!gfx11_get_fast_clear_parameters(device, iview, &clear_value, &reset_value))
            return false;
      } else {
         gfx8_get_fast_clear_parameters(device, iview, &clear_value, &reset_value, &can_avoid_fast_clear_elim);
      }

      if (iview->image->vk.mip_levels > 1) {
         if (pdev->info.gfx_level >= GFX9) {
            uint32_t last_level = iview->vk.base_mip_level + iview->vk.level_count - 1;
            if (last_level >= iview->image->planes[0].surface.num_meta_levels) {
               /* Do not fast clears if one level can't be fast cleard. */
               return false;
            }
         } else {
            for (uint32_t l = 0; l < iview->vk.level_count; l++) {
               uint32_t level = iview->vk.base_mip_level + l;
               struct legacy_surf_dcc_level *dcc_level =
                  &iview->image->planes[0].surface.u.legacy.color.dcc_level[level];

               /* Do not fast clears if one level can't be
                * fast cleared.
                */
               if (!dcc_level->dcc_fast_clear_size)
                  return false;
            }
         }
      }
   }

   return true;
}

static void
radv_fast_clear_color(struct radv_cmd_buffer *cmd_buffer, const struct radv_image_view *iview,
                      const VkClearAttachment *clear_att, enum radv_cmd_flush_bits *pre_flush,
                      enum radv_cmd_flush_bits *post_flush)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkClearColorValue clear_value = clear_att->clearValue.color;
   uint32_t clear_color[4], flush_bits = 0;
   uint32_t cmask_clear_value;
   VkImageSubresourceRange range = {
      .aspectMask = iview->vk.aspects,
      .baseMipLevel = iview->vk.base_mip_level,
      .levelCount = iview->vk.level_count,
      .baseArrayLayer = iview->vk.base_array_layer,
      .layerCount = iview->vk.layer_count,
   };

   if (pre_flush) {
      enum radv_cmd_flush_bits bits = radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, iview->image);
      cmd_buffer->state.flush_bits |= bits & ~*pre_flush;
      *pre_flush |= cmd_buffer->state.flush_bits;
   }

   /* DCC */
   radv_format_pack_clear_color(iview->vk.format, clear_color, &clear_value);

   cmask_clear_value = radv_get_cmask_fast_clear_value(iview->image);

   /* clear cmask buffer */
   bool need_decompress_pass = false;
   if (radv_dcc_enabled(iview->image, iview->vk.base_mip_level)) {
      uint32_t reset_value;
      bool can_avoid_fast_clear_elim = true;

      if (pdev->info.gfx_level >= GFX11) {
         ASSERTED bool result = gfx11_get_fast_clear_parameters(device, iview, &clear_value, &reset_value);
         assert(result);
      } else {
         gfx8_get_fast_clear_parameters(device, iview, &clear_value, &reset_value, &can_avoid_fast_clear_elim);
      }

      if (radv_image_has_cmask(iview->image)) {
         flush_bits = radv_clear_cmask(cmd_buffer, iview->image, &range, cmask_clear_value);
      }

      if (!can_avoid_fast_clear_elim)
         need_decompress_pass = true;

      flush_bits |= radv_clear_dcc(cmd_buffer, iview->image, &range, reset_value);

      if (reset_value == radv_dcc_single_clear_value(device)) {
         /* Write the clear color to the first byte of each 256B block when the image supports DCC
          * fast clears with comp-to-single.
          */
         if (vk_format_get_blocksize(iview->image->vk.format) == 16) {
            flush_bits |= radv_clear_dcc_comp_to_single(cmd_buffer, iview->image, &range, clear_value.uint32);
         } else {
            clear_color[2] = clear_color[3] = 0;
            flush_bits |= radv_clear_dcc_comp_to_single(cmd_buffer, iview->image, &range, clear_color);
         }
      }
   } else {
      flush_bits = radv_clear_cmask(cmd_buffer, iview->image, &range, cmask_clear_value);

      /* Fast clearing with CMASK should always be eliminated. */
      need_decompress_pass = true;
   }

   if (post_flush) {
      *post_flush |= flush_bits;
   }

   /* Update the FCE predicate to perform a fast-clear eliminate. */
   radv_update_fce_metadata(cmd_buffer, iview->image, &range, need_decompress_pass);

   radv_update_color_clear_metadata(cmd_buffer, iview, clear_att->colorAttachment, clear_color);
}

/**
 * The parameters mean that same as those in vkCmdClearAttachments.
 */
static void
emit_clear(struct radv_cmd_buffer *cmd_buffer, const VkClearAttachment *clear_att, const VkClearRect *clear_rect,
           enum radv_cmd_flush_bits *pre_flush, enum radv_cmd_flush_bits *post_flush, uint32_t view_mask)
{
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   VkImageAspectFlags aspects = clear_att->aspectMask;

   if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      assert(clear_att->colorAttachment < render->color_att_count);
      const struct radv_attachment *color_att = &render->color_att[clear_att->colorAttachment];

      if (color_att->format == VK_FORMAT_UNDEFINED)
         return;

      VkClearColorValue clear_value = clear_att->clearValue.color;

      if (radv_can_fast_clear_color(cmd_buffer, color_att->iview, color_att->layout, clear_rect, clear_value,
                                    view_mask)) {
         radv_fast_clear_color(cmd_buffer, color_att->iview, clear_att, pre_flush, post_flush);
      } else {
         emit_color_clear(cmd_buffer, clear_att, clear_rect, view_mask);
      }
   } else {
      const struct radv_attachment *ds_att = &render->ds_att;

      if (ds_att->format == VK_FORMAT_UNDEFINED)
         return;

      VkClearDepthStencilValue clear_value = clear_att->clearValue.depthStencil;

      assert(aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
      bool can_fast_clear_depth = false;
      bool can_fast_clear_stencil = false;
      if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) &&
          ds_att->layout != ds_att->stencil_layout) {
         can_fast_clear_depth = radv_can_fast_clear_depth(cmd_buffer, ds_att->iview, ds_att->layout, aspects,
                                                          clear_rect, clear_value, view_mask);
         can_fast_clear_stencil = radv_can_fast_clear_depth(cmd_buffer, ds_att->iview, ds_att->stencil_layout, aspects,
                                                            clear_rect, clear_value, view_mask);
      } else {
         VkImageLayout layout = aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? ds_att->layout : ds_att->stencil_layout;
         can_fast_clear_depth =
            radv_can_fast_clear_depth(cmd_buffer, ds_att->iview, layout, aspects, clear_rect, clear_value, view_mask);
         can_fast_clear_stencil = can_fast_clear_depth;
      }

      if (can_fast_clear_depth && can_fast_clear_stencil) {
         radv_fast_clear_depth(cmd_buffer, ds_att->iview, clear_att->clearValue.depthStencil, clear_att->aspectMask,
                               pre_flush, post_flush);
      } else if (!can_fast_clear_depth && !can_fast_clear_stencil) {
         emit_depthstencil_clear(cmd_buffer, clear_att->clearValue.depthStencil, clear_att->aspectMask, clear_rect,
                                 view_mask, false);
      } else {
         if (can_fast_clear_depth) {
            radv_fast_clear_depth(cmd_buffer, ds_att->iview, clear_att->clearValue.depthStencil,
                                  VK_IMAGE_ASPECT_DEPTH_BIT, pre_flush, post_flush);
         } else {
            emit_depthstencil_clear(cmd_buffer, clear_att->clearValue.depthStencil, VK_IMAGE_ASPECT_DEPTH_BIT,
                                    clear_rect, view_mask, can_fast_clear_depth);
         }

         if (can_fast_clear_stencil) {
            radv_fast_clear_depth(cmd_buffer, ds_att->iview, clear_att->clearValue.depthStencil,
                                  VK_IMAGE_ASPECT_STENCIL_BIT, pre_flush, post_flush);
         } else {
            emit_depthstencil_clear(cmd_buffer, clear_att->clearValue.depthStencil, VK_IMAGE_ASPECT_STENCIL_BIT,
                                    clear_rect, view_mask, can_fast_clear_stencil);
         }
      }
   }
}

static bool
radv_rendering_needs_clear(const VkRenderingInfo *pRenderingInfo)
{
   for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; i++) {
      if (pRenderingInfo->pColorAttachments[i].imageView != VK_NULL_HANDLE &&
          pRenderingInfo->pColorAttachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
         return true;
   }

   if (pRenderingInfo->pDepthAttachment != NULL && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE &&
       pRenderingInfo->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
      return true;

   if (pRenderingInfo->pStencilAttachment != NULL && pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE &&
       pRenderingInfo->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
      return true;

   return false;
}

static void
radv_subpass_clear_attachment(struct radv_cmd_buffer *cmd_buffer, const VkClearAttachment *clear_att,
                              enum radv_cmd_flush_bits *pre_flush, enum radv_cmd_flush_bits *post_flush)
{
   const struct radv_rendering_state *render = &cmd_buffer->state.render;

   VkClearRect clear_rect = {
      .rect = render->area,
      .baseArrayLayer = 0,
      .layerCount = render->layer_count,
   };

   radv_describe_begin_render_pass_clear(cmd_buffer, clear_att->aspectMask);

   emit_clear(cmd_buffer, clear_att, &clear_rect, pre_flush, post_flush, render->view_mask);

   radv_describe_end_render_pass_clear(cmd_buffer);
}

/**
 * Emit any pending attachment clears for the current subpass.
 *
 * @see radv_attachment_state::pending_clear_aspects
 */
void
radv_cmd_buffer_clear_rendering(struct radv_cmd_buffer *cmd_buffer, const VkRenderingInfo *pRenderingInfo)
{
   const struct radv_rendering_state *render = &cmd_buffer->state.render;
   struct radv_meta_saved_state saved_state;
   enum radv_cmd_flush_bits pre_flush = 0;
   enum radv_cmd_flush_bits post_flush = 0;

   if (!radv_rendering_needs_clear(pRenderingInfo))
      return;

   /* Subpass clear should not be affected by conditional rendering. */
   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_GRAPHICS_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SUSPEND_PREDICATING);

   assert(render->color_att_count == pRenderingInfo->colorAttachmentCount);
   for (uint32_t i = 0; i < render->color_att_count; i++) {
      if (render->color_att[i].iview == NULL ||
          pRenderingInfo->pColorAttachments[i].loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR)
         continue;

      VkClearAttachment clear_att = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .colorAttachment = i,
         .clearValue = pRenderingInfo->pColorAttachments[i].clearValue,
      };

      radv_subpass_clear_attachment(cmd_buffer, &clear_att, &pre_flush, &post_flush);
   }

   if (render->ds_att.iview != NULL) {
      VkClearAttachment clear_att = {.aspectMask = 0};

      if (pRenderingInfo->pDepthAttachment != NULL && pRenderingInfo->pDepthAttachment->imageView != VK_NULL_HANDLE &&
          pRenderingInfo->pDepthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         clear_att.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
         clear_att.clearValue.depthStencil.depth = pRenderingInfo->pDepthAttachment->clearValue.depthStencil.depth;
      }

      if (pRenderingInfo->pStencilAttachment != NULL &&
          pRenderingInfo->pStencilAttachment->imageView != VK_NULL_HANDLE &&
          pRenderingInfo->pStencilAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         clear_att.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
         clear_att.clearValue.depthStencil.stencil =
            pRenderingInfo->pStencilAttachment->clearValue.depthStencil.stencil;
      }

      if (clear_att.aspectMask != 0) {
         radv_subpass_clear_attachment(cmd_buffer, &clear_att, &pre_flush, &post_flush);
      }
   }

   radv_meta_restore(&saved_state, cmd_buffer);
   cmd_buffer->state.flush_bits |= post_flush;
}

static void
radv_clear_image_layer(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout image_layout,
                       const VkImageSubresourceRange *range, VkFormat format, int level, unsigned layer_count,
                       const VkClearValue *clear_val)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_image_view iview;
   uint32_t width = u_minify(image->vk.extent.width, range->baseMipLevel + level);
   uint32_t height = u_minify(image->vk.extent.height, range->baseMipLevel + level);

   radv_image_view_init(&iview, device,
                        &(VkImageViewCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .image = radv_image_to_handle(image),
                           .viewType = radv_meta_get_view_type(image),
                           .format = format,
                           .subresourceRange = {.aspectMask = range->aspectMask,
                                                .baseMipLevel = range->baseMipLevel + level,
                                                .levelCount = 1,
                                                .baseArrayLayer = range->baseArrayLayer,
                                                .layerCount = layer_count},
                        },
                        NULL);

   VkClearAttachment clear_att = {
      .aspectMask = range->aspectMask,
      .colorAttachment = 0,
      .clearValue = *clear_val,
   };

   VkClearRect clear_rect = {
      .rect =
         {
            .offset = {0, 0},
            .extent = {width, height},
         },
      .baseArrayLayer = 0,
      .layerCount = layer_count,
   };

   VkRenderingAttachmentInfo att = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = radv_image_view_to_handle(&iview),
      .imageLayout = image_layout,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
   };

   VkRenderingInfo rendering_info = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .flags = VK_RENDERING_INPUT_ATTACHMENT_NO_CONCURRENT_WRITES_BIT_MESA,
      .renderArea =
         {
            .offset = {0, 0},
            .extent = {width, height},
         },
      .layerCount = layer_count,
   };

   if (image->vk.aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
      rendering_info.colorAttachmentCount = 1;
      rendering_info.pColorAttachments = &att;
   }
   if (image->vk.aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
      rendering_info.pDepthAttachment = &att;
   if (image->vk.aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
      rendering_info.pStencilAttachment = &att;

   radv_CmdBeginRendering(radv_cmd_buffer_to_handle(cmd_buffer), &rendering_info);

   emit_clear(cmd_buffer, &clear_att, &clear_rect, NULL, NULL, 0);

   radv_CmdEndRendering(radv_cmd_buffer_to_handle(cmd_buffer));

   radv_image_view_finish(&iview);
}

/**
 * Return TRUE if a fast color or depth clear has been performed.
 */
static bool
radv_fast_clear_range(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkFormat format,
                      VkImageLayout image_layout, const VkImageSubresourceRange *range, const VkClearValue *clear_val)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_image_view iview;
   bool fast_cleared = false;

   radv_image_view_init(&iview, device,
                        &(VkImageViewCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .image = radv_image_to_handle(image),
                           .viewType = radv_meta_get_view_type(image),
                           .format = image->vk.format,
                           .subresourceRange =
                              {
                                 .aspectMask = range->aspectMask,
                                 .baseMipLevel = range->baseMipLevel,
                                 .levelCount = vk_image_subresource_level_count(&image->vk, range),
                                 .baseArrayLayer = range->baseArrayLayer,
                                 .layerCount = vk_image_subresource_layer_count(&image->vk, range),
                              },
                        },
                        NULL);

   VkClearRect clear_rect = {
      .rect =
         {
            .offset = {0, 0},
            .extent =
               {
                  u_minify(image->vk.extent.width, range->baseMipLevel),
                  u_minify(image->vk.extent.height, range->baseMipLevel),
               },
         },
      .baseArrayLayer = range->baseArrayLayer,
      .layerCount = vk_image_subresource_layer_count(&image->vk, range),
   };

   VkClearAttachment clear_att = {
      .aspectMask = range->aspectMask,
      .colorAttachment = 0,
      .clearValue = *clear_val,
   };

   if (vk_format_is_color(format)) {
      if (radv_can_fast_clear_color(cmd_buffer, &iview, image_layout, &clear_rect, clear_att.clearValue.color, 0)) {
         radv_fast_clear_color(cmd_buffer, &iview, &clear_att, NULL, NULL);
         fast_cleared = true;
      }
   } else {
      if (radv_can_fast_clear_depth(cmd_buffer, &iview, image_layout, range->aspectMask, &clear_rect,
                                    clear_att.clearValue.depthStencil, 0)) {
         radv_fast_clear_depth(cmd_buffer, &iview, clear_att.clearValue.depthStencil, clear_att.aspectMask, NULL, NULL);
         fast_cleared = true;
      }
   }

   radv_image_view_finish(&iview);
   return fast_cleared;
}

static void
radv_cmd_clear_image(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout image_layout,
                     const VkClearValue *clear_value, uint32_t range_count, const VkImageSubresourceRange *ranges,
                     bool cs)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkFormat format = image->vk.format;
   VkClearValue internal_clear_value;

   if (ranges->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)
      internal_clear_value.color = clear_value->color;
   else
      internal_clear_value.depthStencil = clear_value->depthStencil;

   bool disable_compression = false;

   if (format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) {
      if (cs ? !radv_is_storage_image_format_supported(pdev, format)
             : !radv_is_colorbuffer_format_supported(pdev, format)) {
         format = VK_FORMAT_R32_UINT;
         internal_clear_value.color.uint32[0] = float3_to_rgb9e5(clear_value->color.float32);

         uint32_t queue_mask = radv_image_queue_family_mask(image, cmd_buffer->qf, cmd_buffer->qf);

         for (uint32_t r = 0; r < range_count; r++) {
            const VkImageSubresourceRange *range = &ranges[r];

            /* Don't use compressed image stores because they will use an incompatible format. */
            if (radv_layout_dcc_compressed(device, image, range->baseMipLevel, image_layout, queue_mask)) {
               disable_compression = cs;
               break;
            }
         }
      }
   }

   if (format == VK_FORMAT_R4G4_UNORM_PACK8) {
      uint8_t r, g;
      format = VK_FORMAT_R8_UINT;
      r = float_to_ubyte(clear_value->color.float32[0]) >> 4;
      g = float_to_ubyte(clear_value->color.float32[1]) >> 4;
      internal_clear_value.color.uint32[0] = (r << 4) | (g & 0xf);
   }

   for (uint32_t r = 0; r < range_count; r++) {
      const VkImageSubresourceRange *range = &ranges[r];

      /* Try to perform a fast clear first, otherwise fallback to
       * the legacy path.
       */
      if (!cs && radv_fast_clear_range(cmd_buffer, image, format, image_layout, range, &internal_clear_value)) {
         continue;
      }

      for (uint32_t l = 0; l < vk_image_subresource_level_count(&image->vk, range); ++l) {
         const uint32_t layer_count = image->vk.image_type == VK_IMAGE_TYPE_3D
                                         ? u_minify(image->vk.extent.depth, range->baseMipLevel + l)
                                         : vk_image_subresource_layer_count(&image->vk, range);
         if (cs) {
            for (uint32_t s = 0; s < layer_count; ++s) {
               struct radv_meta_blit2d_surf surf;
               surf.format = format;
               surf.image = image;
               surf.level = range->baseMipLevel + l;
               surf.layer = range->baseArrayLayer + s;
               surf.aspect_mask = range->aspectMask;
               surf.disable_compression = disable_compression;
               radv_meta_clear_image_cs(cmd_buffer, &surf, &internal_clear_value.color);
            }
         } else {
            assert(!disable_compression);
            radv_clear_image_layer(cmd_buffer, image, image_layout, range, format, l, layer_count,
                                   &internal_clear_value);
         }
      }
   }

   if (disable_compression) {
      enum radv_cmd_flush_bits flush_bits = 0;
      for (unsigned i = 0; i < range_count; i++) {
         if (radv_dcc_enabled(image, ranges[i].baseMipLevel))
            flush_bits |= radv_clear_dcc(cmd_buffer, image, &ranges[i], 0xffffffffu);
      }
      cmd_buffer->state.flush_bits |= flush_bits;
   }
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image_h, VkImageLayout imageLayout,
                        const VkClearColorValue *pColor, uint32_t rangeCount, const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_image, image, image_h);
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   struct radv_meta_saved_state saved_state;
   bool cs;

   cs = cmd_buffer->qf == RADV_QUEUE_COMPUTE || !radv_image_is_renderable(device, image);

   /* Clear commands (except vkCmdClearAttachments) should not be affected by conditional rendering.
    */
   enum radv_meta_save_flags save_flags = RADV_META_SAVE_CONSTANTS | RADV_META_SUSPEND_PREDICATING;
   if (cs)
      save_flags |= RADV_META_SAVE_COMPUTE_PIPELINE | RADV_META_SAVE_DESCRIPTORS;
   else
      save_flags |= RADV_META_SAVE_GRAPHICS_PIPELINE;

   radv_meta_save(&saved_state, cmd_buffer, save_flags);

   radv_cmd_clear_image(cmd_buffer, image, imageLayout, (const VkClearValue *)pColor, rangeCount, pRanges, cs);

   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer, VkImage image_h, VkImageLayout imageLayout,
                               const VkClearDepthStencilValue *pDepthStencil, uint32_t rangeCount,
                               const VkImageSubresourceRange *pRanges)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(radv_image, image, image_h);
   struct radv_meta_saved_state saved_state;

   /* Clear commands (except vkCmdClearAttachments) should not be affected by conditional rendering. */
   radv_meta_save(&saved_state, cmd_buffer,
                  RADV_META_SAVE_GRAPHICS_PIPELINE | RADV_META_SAVE_CONSTANTS | RADV_META_SUSPEND_PREDICATING);

   radv_cmd_clear_image(cmd_buffer, image, imageLayout, (const VkClearValue *)pDepthStencil, rangeCount, pRanges,
                        false);

   radv_meta_restore(&saved_state, cmd_buffer);
}

VKAPI_ATTR void VKAPI_CALL
radv_CmdClearAttachments(VkCommandBuffer commandBuffer, uint32_t attachmentCount, const VkClearAttachment *pAttachments,
                         uint32_t rectCount, const VkClearRect *pRects)
{
   VK_FROM_HANDLE(radv_cmd_buffer, cmd_buffer, commandBuffer);
   struct radv_meta_saved_state saved_state;
   enum radv_cmd_flush_bits pre_flush = 0;
   enum radv_cmd_flush_bits post_flush = 0;

   if (!cmd_buffer->state.render.active)
      return;

   radv_meta_save(&saved_state, cmd_buffer, RADV_META_SAVE_GRAPHICS_PIPELINE | RADV_META_SAVE_CONSTANTS);

   /* FINISHME: We can do better than this dumb loop. It thrashes too much
    * state.
    */
   for (uint32_t a = 0; a < attachmentCount; ++a) {
      for (uint32_t r = 0; r < rectCount; ++r) {
         emit_clear(cmd_buffer, &pAttachments[a], &pRects[r], &pre_flush, &post_flush,
                    cmd_buffer->state.render.view_mask);
      }
   }

   radv_meta_restore(&saved_state, cmd_buffer);
   cmd_buffer->state.flush_bits |= post_flush;
}
