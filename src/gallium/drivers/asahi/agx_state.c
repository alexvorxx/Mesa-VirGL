/*
 * Copyright 2021 Alyssa Rosenzweig
 * Copyright 2019-2020 Collabora, Ltd.
 * Copyright 2014-2017 Broadcom
 * Copyright 2010 Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "agx_state.h"
#include <errno.h>
#include <stdio.h>
#include "asahi/compiler/agx_compile.h"
#include "asahi/genxml/agx_pack.h"
#include "asahi/layout/layout.h"
#include "asahi/lib/agx_abi.h"
#include "asahi/lib/agx_helpers.h"
#include "asahi/lib/agx_nir_passes.h"
#include "asahi/lib/agx_ppp.h"
#include "asahi/lib/agx_usc.h"
#include "asahi/lib/shaders/compression.h"
#include "asahi/lib/shaders/tessellator.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_serialize.h"
#include "compiler/shader_enums.h"
#include "gallium/auxiliary/nir/pipe_nir.h"
#include "gallium/auxiliary/nir/tgsi_to_nir.h"
#include "gallium/auxiliary/tgsi/tgsi_from_mesa.h"
#include "gallium/auxiliary/util/u_blend.h"
#include "gallium/auxiliary/util/u_draw.h"
#include "gallium/auxiliary/util/u_framebuffer.h"
#include "gallium/auxiliary/util/u_helpers.h"
#include "gallium/auxiliary/util/u_prim_restart.h"
#include "gallium/auxiliary/util/u_viewport.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "shaders/query.h"
#include "util/bitscan.h"
#include "util/bitset.h"
#include "util/blend.h"
#include "util/blob.h"
#include "util/compiler.h"
#include "util/format/u_format.h"
#include "util/format/u_formats.h"
#include "util/format_srgb.h"
#include "util/half_float.h"
#include "util/hash_table.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_dump.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"
#include "util/u_resource.h"
#include "util/u_transfer.h"
#include "util/u_upload_mgr.h"
#include "agx_bg_eot.h"
#include "agx_bo.h"
#include "agx_device.h"
#include "agx_disk_cache.h"
#include "agx_linker.h"
#include "agx_nir.h"
#include "agx_nir_lower_gs.h"
#include "agx_nir_lower_vbo.h"
#include "agx_tilebuffer.h"
#include "nir_builder.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "nir_lower_blend.h"
#include "nir_xfb_info.h"
#include "pool.h"

void
agx_legalize_compression(struct agx_context *ctx, struct agx_resource *rsrc,
                         enum pipe_format format)
{
   if (!ail_is_view_compatible(&rsrc->layout, format)) {
      agx_decompress(ctx, rsrc, "Incompatible formats");
   }
}

static void
agx_set_shader_images(struct pipe_context *pctx, enum pipe_shader_type shader,
                      unsigned start_slot, unsigned count,
                      unsigned unbind_num_trailing_slots,
                      const struct pipe_image_view *iviews)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_IMAGE;

   /* Unbind start_slot...start_slot+count */
   if (!iviews) {
      for (int i = start_slot;
           i < start_slot + count + unbind_num_trailing_slots; i++) {
         pipe_resource_reference(&ctx->stage[shader].images[i].resource, NULL);
      }

      ctx->stage[shader].image_mask &=
         ~BITFIELD64_MASK(count + unbind_num_trailing_slots) << start_slot;
      return;
   }

   /* Images writeable with pixel granularity are incompatible with
    * compression. Decompress if necessary.
    *
    * Driver-internal images are used by the compute blitter and are exempt
    * from these transitions, as it only uses compressed images when safe.
    *
    * We do this upfront because agx_decompress and agx_legalize_compression can
    * call set_shader_images internall.
    */
   for (int i = 0; i < count; i++) {
      const struct pipe_image_view *image = &iviews[i];
      struct agx_resource *rsrc = agx_resource(image->resource);

      if (rsrc && !(image->access & PIPE_IMAGE_ACCESS_DRIVER_INTERNAL)) {
         if (!rsrc->layout.writeable_image &&
             (image->shader_access & PIPE_IMAGE_ACCESS_WRITE)) {

            agx_decompress(ctx, rsrc, "Shader image");
         }

         /* Readable images may be compressed but are still subject to format
          * reinterpretation rules.
          */
         agx_legalize_compression(ctx, rsrc, image->format);

         if (image->shader_access & PIPE_IMAGE_ACCESS_WRITE)
            assert(rsrc->layout.writeable_image);
      }
   }

   /* Bind start_slot...start_slot+count */
   for (int i = 0; i < count; i++) {
      const struct pipe_image_view *image = &iviews[i];

      if (!image->resource) {
         util_copy_image_view(&ctx->stage[shader].images[start_slot + i], NULL);
         ctx->stage[shader].image_mask &= ~BITFIELD_BIT(start_slot + i);
      } else {
         util_copy_image_view(&ctx->stage[shader].images[start_slot + i],
                              image);
         ctx->stage[shader].image_mask |= BITFIELD_BIT(start_slot + i);
      }
   }

   /* Unbind start_slot+count...start_slot+count+unbind_num_trailing_slots */
   for (int i = 0; i < unbind_num_trailing_slots; i++) {
      ctx->stage[shader].image_mask &= ~BITFIELD_BIT(start_slot + count + i);
      util_copy_image_view(&ctx->stage[shader].images[start_slot + count + i],
                           NULL);
   }
}

static void
agx_set_shader_buffers(struct pipe_context *pctx, enum pipe_shader_type shader,
                       unsigned start, unsigned count,
                       const struct pipe_shader_buffer *buffers,
                       unsigned writable_bitmask)
{
   struct agx_context *ctx = agx_context(pctx);

   util_set_shader_buffers_mask(ctx->stage[shader].ssbo,
                                &ctx->stage[shader].ssbo_mask, buffers, start,
                                count);

   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_SSBO;
   ctx->stage[shader].ssbo_writable_mask &= ~(BITFIELD_MASK(count) << start);
   ctx->stage[shader].ssbo_writable_mask |= writable_bitmask << start;
}

static void
agx_set_blend_color(struct pipe_context *pctx,
                    const struct pipe_blend_color *state)
{
   struct agx_context *ctx = agx_context(pctx);

   if (state)
      memcpy(&ctx->blend_color, state, sizeof(*state));

   ctx->dirty |= AGX_DIRTY_BLEND_COLOR;
}

static void
agx_set_patch_vertices(struct pipe_context *pctx, unsigned char n)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->patch_vertices = n;
}

static void
agx_set_tess_state(struct pipe_context *pctx,
                   const float default_outer_level[4],
                   const float default_inner_level[2])
{
   struct agx_context *ctx = agx_context(pctx);

   memcpy(ctx->default_outer_level, default_outer_level, 4 * sizeof(float));
   memcpy(ctx->default_inner_level, default_inner_level, 2 * sizeof(float));
}

static void *
agx_create_blend_state(struct pipe_context *ctx,
                       const struct pipe_blend_state *state)
{
   struct agx_blend *so = CALLOC_STRUCT(agx_blend);
   struct agx_blend_key *key = &so->key;

   key->alpha_to_coverage = state->alpha_to_coverage;
   key->alpha_to_one = state->alpha_to_one;

   key->logicop_func =
      state->logicop_enable ? state->logicop_func : PIPE_LOGICOP_COPY;

   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) {
      unsigned rti = state->independent_blend_enable ? i : 0;
      struct pipe_rt_blend_state rt = state->rt[rti];

      if (state->logicop_enable || !rt.blend_enable) {
         /* No blending, but we get the colour mask below */
         key->rt[i] = (struct agx_blend_rt_key){
            .rgb_func = PIPE_BLEND_ADD,
            .rgb_src_factor = PIPE_BLENDFACTOR_ONE,
            .rgb_dst_factor = PIPE_BLENDFACTOR_ZERO,

            .alpha_func = PIPE_BLEND_ADD,
            .alpha_src_factor = PIPE_BLENDFACTOR_ONE,
            .alpha_dst_factor = PIPE_BLENDFACTOR_ZERO,
         };
      } else {
         key->rt[i].rgb_func = rt.rgb_func;
         key->rt[i].rgb_src_factor = rt.rgb_src_factor;
         key->rt[i].rgb_dst_factor = rt.rgb_dst_factor;

         key->rt[i].alpha_func = rt.alpha_func;
         key->rt[i].alpha_src_factor = rt.alpha_src_factor;
         key->rt[i].alpha_dst_factor = rt.alpha_dst_factor;
      }

      key->rt[i].colormask = rt.colormask;

      if (rt.colormask)
         so->store |= (PIPE_CLEAR_COLOR0 << i);
   }

   return so;
}

static void
agx_bind_blend_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->blend = cso;
   ctx->dirty |= AGX_DIRTY_BLEND;
}

static const enum agx_stencil_op agx_stencil_ops[PIPE_STENCIL_OP_INVERT + 1] = {
   [PIPE_STENCIL_OP_KEEP] = AGX_STENCIL_OP_KEEP,
   [PIPE_STENCIL_OP_ZERO] = AGX_STENCIL_OP_ZERO,
   [PIPE_STENCIL_OP_REPLACE] = AGX_STENCIL_OP_REPLACE,
   [PIPE_STENCIL_OP_INCR] = AGX_STENCIL_OP_INCR_SAT,
   [PIPE_STENCIL_OP_DECR] = AGX_STENCIL_OP_DECR_SAT,
   [PIPE_STENCIL_OP_INCR_WRAP] = AGX_STENCIL_OP_INCR_WRAP,
   [PIPE_STENCIL_OP_DECR_WRAP] = AGX_STENCIL_OP_DECR_WRAP,
   [PIPE_STENCIL_OP_INVERT] = AGX_STENCIL_OP_INVERT,
};

static void
agx_pack_stencil(struct agx_fragment_stencil_packed *out,
                 struct pipe_stencil_state st)
{
   if (st.enabled) {
      agx_pack(out, FRAGMENT_STENCIL, cfg) {
         cfg.compare = (enum agx_zs_func)st.func;
         cfg.write_mask = st.writemask;
         cfg.read_mask = st.valuemask;

         cfg.depth_pass = agx_stencil_ops[st.zpass_op];
         cfg.depth_fail = agx_stencil_ops[st.zfail_op];
         cfg.stencil_fail = agx_stencil_ops[st.fail_op];
      }
   } else {
      agx_pack(out, FRAGMENT_STENCIL, cfg) {
         cfg.compare = AGX_ZS_FUNC_ALWAYS;
         cfg.write_mask = 0xFF;
         cfg.read_mask = 0xFF;

         cfg.depth_pass = AGX_STENCIL_OP_KEEP;
         cfg.depth_fail = AGX_STENCIL_OP_KEEP;
         cfg.stencil_fail = AGX_STENCIL_OP_KEEP;
      }
   }
}

static void *
agx_create_zsa_state(struct pipe_context *ctx,
                     const struct pipe_depth_stencil_alpha_state *state)
{
   struct agx_zsa *so = CALLOC_STRUCT(agx_zsa);
   assert(!state->depth_bounds_test && "todo");

   so->base = *state;

   /* Handle the enable flag */
   enum pipe_compare_func depth_func =
      state->depth_enabled ? state->depth_func : PIPE_FUNC_ALWAYS;

   /* Z func can otherwise be used as-is */
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_NEVER == AGX_ZS_FUNC_NEVER);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_LESS == AGX_ZS_FUNC_LESS);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_EQUAL == AGX_ZS_FUNC_EQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_LEQUAL == AGX_ZS_FUNC_LEQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_GREATER == AGX_ZS_FUNC_GREATER);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_NOTEQUAL == AGX_ZS_FUNC_NOT_EQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_GEQUAL == AGX_ZS_FUNC_GEQUAL);
   STATIC_ASSERT((enum agx_zs_func)PIPE_FUNC_ALWAYS == AGX_ZS_FUNC_ALWAYS);

   agx_pack(&so->depth, FRAGMENT_FACE, cfg) {
      cfg.depth_function = (enum agx_zs_func)depth_func;
      cfg.disable_depth_write = !state->depth_writemask;
   }

   agx_pack_stencil(&so->front_stencil, state->stencil[0]);

   if (state->stencil[1].enabled) {
      agx_pack_stencil(&so->back_stencil, state->stencil[1]);
   } else {
      /* One sided stencil */
      so->back_stencil = so->front_stencil;
   }

   if (depth_func != PIPE_FUNC_NEVER && depth_func != PIPE_FUNC_ALWAYS)
      so->load |= PIPE_CLEAR_DEPTH;

   if (state->depth_writemask) {
      so->load |= PIPE_CLEAR_DEPTH;
      so->store |= PIPE_CLEAR_DEPTH;
   }

   if (state->stencil[0].enabled) {
      so->load |= PIPE_CLEAR_STENCIL; /* TODO: Optimize */
      so->store |= PIPE_CLEAR_STENCIL;
   }

   return so;
}

static void
agx_bind_zsa_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->zs = cso;
   ctx->dirty |= AGX_DIRTY_ZS;
}

static enum agx_polygon_mode
agx_translate_polygon_mode(unsigned mode)
{
   switch (mode) {
   case PIPE_POLYGON_MODE_FILL:
      return AGX_POLYGON_MODE_FILL;
   case PIPE_POLYGON_MODE_POINT:
      return AGX_POLYGON_MODE_POINT;
   case PIPE_POLYGON_MODE_LINE:
      return AGX_POLYGON_MODE_LINE;
   default:
      unreachable("Unsupported polygon mode");
   }
}

static void *
agx_create_rs_state(struct pipe_context *ctx,
                    const struct pipe_rasterizer_state *cso)
{
   struct agx_rasterizer *so = CALLOC_STRUCT(agx_rasterizer);
   so->base = *cso;

   agx_pack(so->cull, CULL, cfg) {
      cfg.cull_front = cso->cull_face & PIPE_FACE_FRONT;
      cfg.cull_back = cso->cull_face & PIPE_FACE_BACK;
      cfg.front_face_ccw = cso->front_ccw;
      cfg.depth_clip = cso->depth_clip_near;
      cfg.depth_clamp = !cso->depth_clip_near;
      cfg.flat_shading_vertex =
         cso->flatshade_first ? AGX_PPP_VERTEX_0 : AGX_PPP_VERTEX_2;
      cfg.rasterizer_discard = cso->rasterizer_discard;
   };

   /* Two-sided polygon mode doesn't seem to work on G13. Apple's OpenGL
    * implementation lowers to multiple draws with culling. Warn.
    */
   if (unlikely(cso->fill_front != cso->fill_back)) {
      agx_msg("Warning: Two-sided fill modes are unsupported, "
              "rendering may be incorrect.\n");
   }

   so->polygon_mode = agx_translate_polygon_mode(cso->fill_front);
   so->line_width = agx_pack_line_width(cso->line_width);
   so->depth_bias = util_get_offset(cso, cso->fill_front);

   return so;
}

static void
agx_bind_rasterizer_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_rasterizer *so = cso;

   bool base_cso_changed = (cso == NULL) || (ctx->rast == NULL);

   /* Check if scissor or depth bias state has changed, since scissor/depth bias
    * enable is part of the rasterizer state but everything else needed for
    * scissors and depth bias is part of the scissor/depth bias arrays */
   bool scissor_zbias_changed = base_cso_changed ||
                                (ctx->rast->base.scissor != so->base.scissor) ||
                                (ctx->rast->depth_bias != so->depth_bias);

   ctx->dirty |= AGX_DIRTY_RS;

   if (scissor_zbias_changed)
      ctx->dirty |= AGX_DIRTY_SCISSOR_ZBIAS;

   if (base_cso_changed ||
       (ctx->rast->base.sprite_coord_mode != so->base.sprite_coord_mode))
      ctx->dirty |= AGX_DIRTY_SPRITE_COORD_MODE;

   ctx->rast = so;
}

static bool
has_edgeflags(struct agx_context *ctx, enum mesa_prim mode)
{
   return ctx->stage[PIPE_SHADER_VERTEX].shader->info.has_edgeflags &&
          mode == MESA_PRIM_TRIANGLES &&
          (ctx->rast->base.fill_front != PIPE_POLYGON_MODE_FILL);
}

static enum agx_wrap
agx_wrap_from_pipe(enum pipe_tex_wrap in)
{
   switch (in) {
   case PIPE_TEX_WRAP_REPEAT:
      return AGX_WRAP_REPEAT;
   case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
      return AGX_WRAP_CLAMP_TO_EDGE;
   case PIPE_TEX_WRAP_MIRROR_REPEAT:
      return AGX_WRAP_MIRRORED_REPEAT;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
      return AGX_WRAP_CLAMP_TO_BORDER;
   case PIPE_TEX_WRAP_CLAMP:
      return AGX_WRAP_CLAMP_GL;
   case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      return AGX_WRAP_MIRRORED_CLAMP_TO_EDGE;
   default:
      unreachable("Invalid wrap mode");
   }
}

static enum agx_mip_filter
agx_mip_filter_from_pipe(enum pipe_tex_mipfilter in)
{
   switch (in) {
   case PIPE_TEX_MIPFILTER_NEAREST:
      return AGX_MIP_FILTER_NEAREST;
   case PIPE_TEX_MIPFILTER_LINEAR:
      return AGX_MIP_FILTER_LINEAR;
   case PIPE_TEX_MIPFILTER_NONE:
      return AGX_MIP_FILTER_NONE;
   }

   unreachable("Invalid mip filter");
}

static const enum agx_compare_func agx_compare_funcs[PIPE_FUNC_ALWAYS + 1] = {
   [PIPE_FUNC_NEVER] = AGX_COMPARE_FUNC_NEVER,
   [PIPE_FUNC_LESS] = AGX_COMPARE_FUNC_LESS,
   [PIPE_FUNC_EQUAL] = AGX_COMPARE_FUNC_EQUAL,
   [PIPE_FUNC_LEQUAL] = AGX_COMPARE_FUNC_LEQUAL,
   [PIPE_FUNC_GREATER] = AGX_COMPARE_FUNC_GREATER,
   [PIPE_FUNC_NOTEQUAL] = AGX_COMPARE_FUNC_NOT_EQUAL,
   [PIPE_FUNC_GEQUAL] = AGX_COMPARE_FUNC_GEQUAL,
   [PIPE_FUNC_ALWAYS] = AGX_COMPARE_FUNC_ALWAYS,
};

static const enum agx_filter agx_filters[] = {
   [PIPE_TEX_FILTER_LINEAR] = AGX_FILTER_LINEAR,
   [PIPE_TEX_FILTER_NEAREST] = AGX_FILTER_NEAREST,
};

static enum pipe_format
fixup_border_zs(enum pipe_format orig, union pipe_color_union *c)
{
   switch (orig) {
   case PIPE_FORMAT_Z24_UNORM_S8_UINT:
   case PIPE_FORMAT_Z24X8_UNORM:
      /* Z24 is internally promoted to Z32F via transfer_helper. These formats
       * are normalized so should get clamped, but Z32F does not get clamped, so
       * we clamp here.
       */
      c->f[0] = SATURATE(c->f[0]);
      return PIPE_FORMAT_Z32_FLOAT;

   case PIPE_FORMAT_X24S8_UINT:
   case PIPE_FORMAT_X32_S8X24_UINT:
      /* Separate stencil is internally promoted */
      return PIPE_FORMAT_S8_UINT;

   default:
      return orig;
   }
}

static void *
agx_create_sampler_state(struct pipe_context *pctx,
                         const struct pipe_sampler_state *state)
{
   struct agx_sampler_state *so = CALLOC_STRUCT(agx_sampler_state);
   so->base = *state;

   /* We report a max texture LOD bias of 16, so clamp appropriately */
   float lod_bias = CLAMP(state->lod_bias, -16.0, 16.0);
   so->lod_bias_as_fp16 = _mesa_float_to_half(lod_bias);

   agx_pack(&so->desc, SAMPLER, cfg) {
      cfg.minimum_lod = state->min_lod;
      cfg.maximum_lod = state->max_lod;
      cfg.maximum_anisotropy =
         util_next_power_of_two(MAX2(state->max_anisotropy, 1));
      cfg.magnify = agx_filters[state->mag_img_filter];
      cfg.minify = agx_filters[state->min_img_filter];
      cfg.mip_filter = agx_mip_filter_from_pipe(state->min_mip_filter);
      cfg.wrap_s = agx_wrap_from_pipe(state->wrap_s);
      cfg.wrap_t = agx_wrap_from_pipe(state->wrap_t);
      cfg.wrap_r = agx_wrap_from_pipe(state->wrap_r);
      cfg.pixel_coordinates = state->unnormalized_coords;
      cfg.compare_func = agx_compare_funcs[state->compare_func];
      cfg.compare_enable = state->compare_mode == PIPE_TEX_COMPARE_R_TO_TEXTURE;
      cfg.seamful_cube_maps = !state->seamless_cube_map;

      if (state->border_color_format != PIPE_FORMAT_NONE) {
         /* TODO: Optimize to use compact descriptors for black/white borders */
         so->uses_custom_border = true;
         cfg.border_colour = AGX_BORDER_COLOUR_CUSTOM;
      }
   }

   memcpy(&so->desc_without_custom_border, &so->desc, sizeof(so->desc));

   if (so->uses_custom_border) {
      union pipe_color_union border = state->border_color;
      enum pipe_format format =
         fixup_border_zs(state->border_color_format, &border);

      agx_pack_border(&so->border, border.ui, format);

      /* Neutralize the bindless-safe descriptor. XXX: This is a hack. */
      so->desc_without_custom_border.opaque[1] &= ~(1u << 23);
   }

   return so;
}

static void
agx_delete_sampler_state(struct pipe_context *ctx, void *state)
{
   struct agx_sampler_state *so = state;
   FREE(so);
}

static void
agx_bind_sampler_states(struct pipe_context *pctx, enum pipe_shader_type shader,
                        unsigned start, unsigned count, void **states)
{
   struct agx_context *ctx = agx_context(pctx);

   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_SAMPLER;

   for (unsigned i = 0; i < count; i++) {
      unsigned p = start + i;
      ctx->stage[shader].samplers[p] = states ? states[i] : NULL;
      if (ctx->stage[shader].samplers[p])
         ctx->stage[shader].valid_samplers |= BITFIELD_BIT(p);
      else
         ctx->stage[shader].valid_samplers &= ~BITFIELD_BIT(p);
   }

   ctx->stage[shader].sampler_count =
      util_last_bit(ctx->stage[shader].valid_samplers);

   /* Recalculate whether we need custom borders */
   ctx->stage[shader].custom_borders = false;

   u_foreach_bit(i, ctx->stage[shader].valid_samplers) {
      if (ctx->stage[shader].samplers[i]->uses_custom_border)
         ctx->stage[shader].custom_borders = true;
   }
}

static enum agx_texture_dimension
agx_translate_tex_dim(enum pipe_texture_target dim, unsigned samples)
{
   assert(samples >= 1);

   switch (dim) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:
      /* Lowered to 2D */
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_2D;

   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_2D:
      return samples > 1 ? AGX_TEXTURE_DIMENSION_2D_MULTISAMPLED
                         : AGX_TEXTURE_DIMENSION_2D;

   case PIPE_TEXTURE_1D_ARRAY:
      assert(samples == 1);
      /* Lowered to 2D */
      FALLTHROUGH;
   case PIPE_TEXTURE_2D_ARRAY:
      return samples > 1 ? AGX_TEXTURE_DIMENSION_2D_ARRAY_MULTISAMPLED
                         : AGX_TEXTURE_DIMENSION_2D_ARRAY;

   case PIPE_TEXTURE_3D:
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_3D;

   case PIPE_TEXTURE_CUBE:
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_CUBE;

   case PIPE_TEXTURE_CUBE_ARRAY:
      assert(samples == 1);
      return AGX_TEXTURE_DIMENSION_CUBE_ARRAY;

   default:
      unreachable("Unsupported texture dimension");
   }
}

static bool
target_is_cube(enum pipe_texture_target target)
{
   return target == PIPE_TEXTURE_CUBE || target == PIPE_TEXTURE_CUBE_ARRAY;
}

static void
agx_pack_texture(void *out, struct agx_resource *rsrc,
                 enum pipe_format format /* override */,
                 const struct pipe_sampler_view *state)
{
   const struct util_format_description *desc = util_format_description(format);

   assert(ail_is_valid_pixel_format(format));

   uint8_t format_swizzle[4] = {
      desc->swizzle[0],
      desc->swizzle[1],
      desc->swizzle[2],
      desc->swizzle[3],
   };

   if (util_format_is_depth_or_stencil(format)) {
      assert(!util_format_is_depth_and_stencil(format) &&
             "separate stencil always used");

      /* Broadcast depth and stencil */
      format_swizzle[0] = 0;
      format_swizzle[1] = 0;
      format_swizzle[2] = 0;
      format_swizzle[3] = 0;
   }

   /* We only have a single swizzle for the user swizzle and the format fixup,
    * so compose them now. */
   uint8_t out_swizzle[4];
   uint8_t view_swizzle[4] = {state->swizzle_r, state->swizzle_g,
                              state->swizzle_b, state->swizzle_a};

   util_format_compose_swizzles(format_swizzle, view_swizzle, out_swizzle);

   unsigned first_layer =
      (state->target == PIPE_BUFFER) ? 0 : state->u.tex.first_layer;

   /* Pack the descriptor into GPU memory */
   agx_pack(out, TEXTURE, cfg) {
      cfg.dimension = agx_translate_tex_dim(state->target,
                                            util_res_sample_count(&rsrc->base));
      cfg.layout = agx_translate_layout(rsrc->layout.tiling);
      cfg.channels = ail_pixel_format[format].channels;
      cfg.type = ail_pixel_format[format].type;
      cfg.swizzle_r = agx_channel_from_pipe(out_swizzle[0]);
      cfg.swizzle_g = agx_channel_from_pipe(out_swizzle[1]);
      cfg.swizzle_b = agx_channel_from_pipe(out_swizzle[2]);
      cfg.swizzle_a = agx_channel_from_pipe(out_swizzle[3]);

      if (state->target == PIPE_BUFFER) {
         unsigned size_el =
            agx_texture_buffer_size_el(format, state->u.buf.size);

         /* Use a 2D texture to increase the maximum size */
         cfg.width = AGX_TEXTURE_BUFFER_WIDTH;
         cfg.height = DIV_ROUND_UP(size_el, cfg.width);
         cfg.first_level = cfg.last_level = 0;
         cfg.buffer_size_sw = size_el;
         cfg.buffer_offset_sw = 0;
      } else {
         cfg.width = rsrc->base.width0;
         cfg.height = rsrc->base.height0;
         cfg.first_level = state->u.tex.first_level;
         cfg.last_level = state->u.tex.last_level;
      }

      cfg.srgb = (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB);
      cfg.unk_mipmapped = rsrc->mipmapped;
      cfg.srgb_2_channel = cfg.srgb && util_format_colormask(desc) == 0x3;

      if (ail_is_compressed(&rsrc->layout)) {
         cfg.compressed_1 = true;
         cfg.extended = true;
      }

      cfg.address = agx_map_texture_gpu(rsrc, first_layer);

      if (state->target == PIPE_BUFFER)
         cfg.address += state->u.buf.offset;

      if (ail_is_compressed(&rsrc->layout)) {
         cfg.acceleration_buffer =
            agx_map_texture_gpu(rsrc, 0) + rsrc->layout.metadata_offset_B +
            (first_layer * rsrc->layout.compression_layer_stride_B);
      }

      if (state->target == PIPE_TEXTURE_3D) {
         cfg.depth = rsrc->base.depth0;
      } else if (state->target == PIPE_BUFFER) {
         cfg.depth = 1;
      } else {
         unsigned layers =
            state->u.tex.last_layer - state->u.tex.first_layer + 1;

         if (target_is_cube(state->target))
            layers /= 6;

         if (rsrc->layout.tiling == AIL_TILING_LINEAR &&
             (state->target == PIPE_TEXTURE_1D_ARRAY ||
              state->target == PIPE_TEXTURE_2D_ARRAY)) {

            cfg.depth_linear = layers;
            cfg.layer_stride_linear = (rsrc->layout.layer_stride_B - 0x80);
            cfg.extended = true;
         } else {
            assert((rsrc->layout.tiling != AIL_TILING_LINEAR) || (layers == 1));
            cfg.depth = layers;
         }
      }

      if (rsrc->base.nr_samples > 1)
         cfg.samples = agx_translate_sample_count(rsrc->base.nr_samples);

      if (state->target == PIPE_BUFFER) {
         cfg.stride = (cfg.width * util_format_get_blocksize(format)) - 16;
      } else if (rsrc->layout.tiling == AIL_TILING_LINEAR) {
         cfg.stride = ail_get_linear_stride_B(&rsrc->layout, 0) - 16;
      } else {
         assert(rsrc->layout.tiling == AIL_TILING_TWIDDLED ||
                rsrc->layout.tiling == AIL_TILING_TWIDDLED_COMPRESSED);

         cfg.page_aligned_layers = rsrc->layout.page_aligned_layers;
      }
   }
}

static struct pipe_sampler_view *
agx_create_sampler_view(struct pipe_context *pctx,
                        struct pipe_resource *orig_texture,
                        const struct pipe_sampler_view *state)
{
   struct agx_resource *rsrc = agx_resource(orig_texture);
   struct agx_sampler_view *so = CALLOC_STRUCT(agx_sampler_view);

   if (!so)
      return NULL;

   struct pipe_resource *texture = orig_texture;
   enum pipe_format format = state->format;

   const struct util_format_description *desc = util_format_description(format);

   /* Separate stencil always used on G13, so we need to fix up for Z32S8 */
   if (util_format_has_stencil(desc) && rsrc->separate_stencil) {
      if (util_format_has_depth(desc)) {
         /* Reinterpret as the depth-only part */
         format = util_format_get_depth_only(format);
      } else {
         /* Use the stencil-only-part */
         rsrc = rsrc->separate_stencil;
         texture = &rsrc->base;
         format = texture->format;
      }
   }

   agx_legalize_compression(agx_context(pctx), rsrc, format);

   /* Save off the resource that we actually use, with the stencil fixed up */
   so->rsrc = rsrc;
   so->format = format;

   so->base = *state;
   so->base.texture = NULL;
   pipe_resource_reference(&so->base.texture, orig_texture);
   pipe_reference_init(&so->base.reference, 1);
   so->base.context = pctx;
   return &so->base;
}

static void
agx_set_sampler_views(struct pipe_context *pctx, enum pipe_shader_type shader,
                      unsigned start, unsigned count,
                      unsigned unbind_num_trailing_slots, bool take_ownership,
                      struct pipe_sampler_view **views)
{
   struct agx_context *ctx = agx_context(pctx);
   unsigned new_nr = 0;
   unsigned i;

   assert(start == 0);

   if (!views)
      count = 0;

   for (i = 0; i < count; ++i) {
      if (take_ownership) {
         pipe_sampler_view_reference(
            (struct pipe_sampler_view **)&ctx->stage[shader].textures[i], NULL);
         ctx->stage[shader].textures[i] = (struct agx_sampler_view *)views[i];
      } else {
         pipe_sampler_view_reference(
            (struct pipe_sampler_view **)&ctx->stage[shader].textures[i],
            views[i]);
      }
   }

   for (; i < count + unbind_num_trailing_slots; i++) {
      pipe_sampler_view_reference(
         (struct pipe_sampler_view **)&ctx->stage[shader].textures[i], NULL);
   }

   for (unsigned t = 0; t < MAX2(ctx->stage[shader].texture_count, count);
        ++t) {
      if (ctx->stage[shader].textures[t])
         new_nr = t + 1;
   }

   ctx->stage[shader].texture_count = new_nr;
   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_IMAGE;
}

static void
agx_sampler_view_destroy(struct pipe_context *ctx,
                         struct pipe_sampler_view *pview)
{
   struct agx_sampler_view *view = (struct agx_sampler_view *)pview;
   pipe_resource_reference(&view->base.texture, NULL);
   FREE(view);
}

static struct pipe_surface *
agx_create_surface(struct pipe_context *ctx, struct pipe_resource *texture,
                   const struct pipe_surface *surf_tmpl)
{
   agx_legalize_compression(agx_context(ctx), agx_resource(texture),
                            surf_tmpl->format);

   struct pipe_surface *surface = CALLOC_STRUCT(pipe_surface);

   if (!surface)
      return NULL;

   unsigned level = surf_tmpl->u.tex.level;

   pipe_reference_init(&surface->reference, 1);
   pipe_resource_reference(&surface->texture, texture);

   assert(texture->target != PIPE_BUFFER && "buffers are not renderable");

   surface->context = ctx;
   surface->format = surf_tmpl->format;
   surface->nr_samples = surf_tmpl->nr_samples;
   surface->width = u_minify(texture->width0, level);
   surface->height = u_minify(texture->height0, level);
   surface->texture = texture;
   surface->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
   surface->u.tex.last_layer = surf_tmpl->u.tex.last_layer;
   surface->u.tex.level = level;

   return surface;
}

static void
agx_set_clip_state(struct pipe_context *ctx,
                   const struct pipe_clip_state *state)
{
}

static void
agx_set_polygon_stipple(struct pipe_context *pctx,
                        const struct pipe_poly_stipple *state)
{
   struct agx_context *ctx = agx_context(pctx);

   memcpy(ctx->poly_stipple, state->stipple, sizeof(ctx->poly_stipple));
   ctx->dirty |= AGX_DIRTY_POLY_STIPPLE;
}

static void
agx_set_sample_mask(struct pipe_context *pipe, unsigned sample_mask)
{
   struct agx_context *ctx = agx_context(pipe);

   /* Optimization: At most MSAA 4x supported, so normalize to avoid pointless
    * dirtying switching between e.g. 0xFFFF and 0xFFFFFFFF masks.
    */
   unsigned new_mask = sample_mask & BITFIELD_MASK(4);

   if (ctx->sample_mask != new_mask) {
      ctx->sample_mask = new_mask;
      ctx->dirty |= AGX_DIRTY_SAMPLE_MASK;
   }
}

static void
agx_set_scissor_states(struct pipe_context *pctx, unsigned start_slot,
                       unsigned num_scissors,
                       const struct pipe_scissor_state *scissor)
{
   struct agx_context *ctx = agx_context(pctx);

   STATIC_ASSERT(sizeof(ctx->scissor[0]) == sizeof(*scissor));
   assert(start_slot + num_scissors <= AGX_MAX_VIEWPORTS);

   memcpy(&ctx->scissor[start_slot], scissor, sizeof(*scissor) * num_scissors);
   ctx->dirty |= AGX_DIRTY_SCISSOR_ZBIAS;
}

static void
agx_set_stencil_ref(struct pipe_context *pctx,
                    const struct pipe_stencil_ref state)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->stencil_ref = state;
   ctx->dirty |= AGX_DIRTY_STENCIL_REF;
}

static void
agx_set_viewport_states(struct pipe_context *pctx, unsigned start_slot,
                        unsigned num_viewports,
                        const struct pipe_viewport_state *vp)
{
   struct agx_context *ctx = agx_context(pctx);

   STATIC_ASSERT(sizeof(ctx->viewport[0]) == sizeof(*vp));
   assert(start_slot + num_viewports <= AGX_MAX_VIEWPORTS);

   memcpy(&ctx->viewport[start_slot], vp, sizeof(*vp) * num_viewports);
   ctx->dirty |= AGX_DIRTY_VIEWPORT;
}

static void
agx_get_scissor_extents(const struct pipe_viewport_state *vp,
                        const struct pipe_scissor_state *ss,
                        const struct pipe_framebuffer_state *fb, unsigned *minx,
                        unsigned *miny, unsigned *maxx, unsigned *maxy)
{
   float trans_x = vp->translate[0], trans_y = vp->translate[1];
   float abs_scale_x = fabsf(vp->scale[0]), abs_scale_y = fabsf(vp->scale[1]);

   /* Calculate the extent of the viewport. Note if a particular dimension of
    * the viewport is an odd number of pixels, both the translate and the scale
    * will have a fractional part of 0.5, so adding and subtracting them yields
    * an integer. Therefore we don't need to round explicitly */
   *minx = CLAMP((int)(trans_x - abs_scale_x), 0, fb->width);
   *miny = CLAMP((int)(trans_y - abs_scale_y), 0, fb->height);
   *maxx = CLAMP((int)(trans_x + abs_scale_x), 0, fb->width);
   *maxy = CLAMP((int)(trans_y + abs_scale_y), 0, fb->height);

   if (ss) {
      *minx = MAX2(ss->minx, *minx);
      *miny = MAX2(ss->miny, *miny);
      *maxx = MIN2(ss->maxx, *maxx);
      *maxy = MIN2(ss->maxy, *maxy);
   }
}

static void
agx_upload_viewport_scissor(struct agx_pool *pool, struct agx_batch *batch,
                            uint8_t **out, const struct pipe_viewport_state *vp,
                            const struct pipe_scissor_state *ss,
                            bool clip_halfz, bool multi_viewport)
{
   /* Number of viewports/scissors isn't precisely determinable in Gallium, so
    * just key off whether we can write to anything other than viewport 0. This
    * could be tuned in the future.
    */
   unsigned count = multi_viewport ? AGX_MAX_VIEWPORTS : 1;

   /* Allocate scissor descriptors */
   unsigned index = batch->scissor.size / AGX_SCISSOR_LENGTH;
   struct agx_scissor_packed *scissors =
      util_dynarray_grow_bytes(&batch->scissor, count, AGX_SCISSOR_LENGTH);

   unsigned minx[AGX_MAX_VIEWPORTS], miny[AGX_MAX_VIEWPORTS];
   unsigned maxx[AGX_MAX_VIEWPORTS], maxy[AGX_MAX_VIEWPORTS];

   /* Upload each scissor */
   for (unsigned i = 0; i < count; ++i) {
      agx_get_scissor_extents(&vp[i], ss ? &ss[i] : NULL, &batch->key, &minx[i],
                              &miny[i], &maxx[i], &maxy[i]);

      float minz, maxz;
      util_viewport_zmin_zmax(vp, clip_halfz, &minz, &maxz);

      agx_pack(scissors + i, SCISSOR, cfg) {
         cfg.min_x = minx[i];
         cfg.min_y = miny[i];
         cfg.min_z = minz;
         cfg.max_x = maxx[i];
         cfg.max_y = maxy[i];
         cfg.max_z = maxz;
      }
   }

   /* Upload state */
   struct AGX_PPP_HEADER present = {
      .depth_bias_scissor = true,
      .region_clip = true,
      .viewport = true,
      .viewport_count = count,
   };

   size_t size = agx_ppp_update_size(&present);
   struct agx_ptr T = agx_pool_alloc_aligned(&batch->pool, size, 64);
   struct agx_ppp_update ppp = agx_new_ppp_update(T, size, &present);

   agx_ppp_push(&ppp, DEPTH_BIAS_SCISSOR, cfg) {
      cfg.scissor = index;

      /* Use the current depth bias, we allocate linearly */
      unsigned count = batch->depth_bias.size / AGX_DEPTH_BIAS_LENGTH;
      cfg.depth_bias = count ? count - 1 : 0;
   };

   for (unsigned i = 0; i < count; ++i) {
      agx_ppp_push(&ppp, REGION_CLIP, cfg) {
         cfg.enable = true;
         cfg.min_x = minx[i] / 32;
         cfg.min_y = miny[i] / 32;
         cfg.max_x = DIV_ROUND_UP(MAX2(maxx[i], 1), 32);
         cfg.max_y = DIV_ROUND_UP(MAX2(maxy[i], 1), 32);
      }
   }

   agx_ppp_push(&ppp, VIEWPORT_CONTROL, cfg)
      ;

   /* Upload viewports */
   for (unsigned i = 0; i < count; ++i) {
      agx_ppp_push(&ppp, VIEWPORT, cfg) {
         cfg.translate_x = vp[i].translate[0];
         cfg.translate_y = vp[i].translate[1];
         cfg.translate_z = vp[i].translate[2];
         cfg.scale_x = vp[i].scale[0];
         cfg.scale_y = vp[i].scale[1];
         cfg.scale_z = vp[i].scale[2];

         if (!clip_halfz) {
            cfg.translate_z -= cfg.scale_z;
            cfg.scale_z *= 2;
         }
      }
   }

   agx_ppp_fini(out, &ppp);
}

static void
agx_upload_depth_bias(struct agx_batch *batch,
                      const struct pipe_rasterizer_state *rast)
{
   void *ptr =
      util_dynarray_grow_bytes(&batch->depth_bias, 1, AGX_DEPTH_BIAS_LENGTH);

   agx_pack(ptr, DEPTH_BIAS, cfg) {
      cfg.depth_bias = rast->offset_units * 2.0f;
      cfg.slope_scale = rast->offset_scale;
      cfg.clamp = rast->offset_clamp;
   }
}

/* A framebuffer state can be reused across batches, so it doesn't make sense
 * to add surfaces to the BO list here. Instead we added them when flushing.
 */

static void
agx_set_framebuffer_state(struct pipe_context *pctx,
                          const struct pipe_framebuffer_state *state)
{
   struct agx_context *ctx = agx_context(pctx);

   if (!state)
      return;

   util_copy_framebuffer_state(&ctx->framebuffer, state);
   ctx->batch = NULL;
   agx_dirty_all(ctx);
}

/*
 * To write out render targets, each render target surface is bound as a
 * writable shader image, written with the end-of-tile program. This helper
 * constructs the internal pipe_image_view used.
 */
static struct pipe_image_view
image_view_for_surface(struct pipe_surface *surf)
{
   return (struct pipe_image_view){
      .resource = surf->texture,
      .format = surf->format,
      .access = PIPE_IMAGE_ACCESS_READ_WRITE,
      .shader_access = PIPE_IMAGE_ACCESS_READ_WRITE,
      .u.tex.single_layer_view =
         surf->u.tex.first_layer == surf->u.tex.last_layer,
      .u.tex.first_layer = surf->u.tex.first_layer,
      .u.tex.last_layer = surf->u.tex.last_layer,
      .u.tex.level = surf->u.tex.level,
   };
}

/* Similarly, to read render targets, surfaces are bound as textures */
static struct pipe_sampler_view
sampler_view_for_surface(struct pipe_surface *surf)
{
   bool layered = surf->u.tex.last_layer > surf->u.tex.first_layer;

   return (struct pipe_sampler_view){
      /* To reduce shader variants, we always use a 2D texture. For reloads of
       * arrays and cube maps, we map a single layer as a 2D image.
       */
      .target = layered ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D,
      .swizzle_r = PIPE_SWIZZLE_X,
      .swizzle_g = PIPE_SWIZZLE_Y,
      .swizzle_b = PIPE_SWIZZLE_Z,
      .swizzle_a = PIPE_SWIZZLE_W,
      .u.tex =
         {
            .first_layer = surf->u.tex.first_layer,
            .last_layer = surf->u.tex.last_layer,
            .first_level = surf->u.tex.level,
            .last_level = surf->u.tex.level,
         },
   };
}

static bool
target_is_array(enum pipe_texture_target target)
{
   switch (target) {
   case PIPE_TEXTURE_3D:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_1D_ARRAY:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_CUBE_ARRAY:
      return true;
   default:
      return false;
   }
}

static void
agx_batch_upload_pbe(struct agx_batch *batch, struct agx_pbe_packed *out,
                     struct pipe_image_view *view, bool block_access,
                     bool arrays_as_2d, bool force_2d_array, bool emrt)
{
   struct agx_resource *tex = agx_resource(view->resource);
   const struct util_format_description *desc =
      util_format_description(view->format);
   enum pipe_texture_target target = tex->base.target;
   bool is_buffer = (target == PIPE_BUFFER);

   if (!is_buffer && view->u.tex.single_layer_view)
      target = PIPE_TEXTURE_2D;

   arrays_as_2d |= (view->access & PIPE_IMAGE_ACCESS_DRIVER_INTERNAL);

   /* To reduce shader variants, spilled layered render targets are accessed as
    * 2D Arrays regardless of the actual target, so force in that case.
    *
    * Likewise, cubes are accessed as arrays for consistency with NIR.
    */
   if ((arrays_as_2d && target_is_array(target)) || target_is_cube(target) ||
       force_2d_array)
      target = PIPE_TEXTURE_2D_ARRAY;

   unsigned level = is_buffer ? 0 : view->u.tex.level;
   unsigned layer = is_buffer ? 0 : view->u.tex.first_layer;

   agx_pack(out, PBE, cfg) {
      cfg.dimension =
         agx_translate_tex_dim(target, util_res_sample_count(&tex->base));
      cfg.layout = agx_translate_layout(tex->layout.tiling);
      cfg.channels = ail_pixel_format[view->format].channels;
      cfg.type = ail_pixel_format[view->format].type;
      cfg.srgb = util_format_is_srgb(view->format);

      assert(desc->nr_channels >= 1 && desc->nr_channels <= 4);

      for (unsigned i = 0; i < desc->nr_channels; ++i) {
         if (desc->swizzle[i] == 0)
            cfg.swizzle_r = i;
         else if (desc->swizzle[i] == 1)
            cfg.swizzle_g = i;
         else if (desc->swizzle[i] == 2)
            cfg.swizzle_b = i;
         else if (desc->swizzle[i] == 3)
            cfg.swizzle_a = i;
      }

      cfg.buffer = agx_map_texture_gpu(tex, layer);
      cfg.unk_mipmapped = tex->mipmapped;

      if (is_buffer) {
         unsigned size_el =
            agx_texture_buffer_size_el(view->format, view->u.buf.size);

         /* Buffers uniquely have offsets (in bytes, not texels) */
         cfg.buffer += view->u.buf.offset;

         /* Use a 2D texture to increase the maximum size */
         cfg.width = AGX_TEXTURE_BUFFER_WIDTH;
         cfg.height = DIV_ROUND_UP(size_el, cfg.width);
         cfg.level = 0;
         cfg.stride = (cfg.width * util_format_get_blocksize(view->format)) - 4;
         cfg.layers = 1;
         cfg.levels = 1;
      } else if (util_res_sample_count(&tex->base) > 1 && !block_access) {
         /* Multisampled images are bound like buffer textures, with
          * addressing arithmetic to determine the texel to write.
          *
          * Note that the end-of-tile program uses real multisample images with
          * image_write_block instructions.
          */
         unsigned blocksize_B = util_format_get_blocksize(view->format);
         unsigned size_px =
            (tex->layout.size_B - tex->layout.layer_stride_B * layer) /
            blocksize_B;

         cfg.dimension = AGX_TEXTURE_DIMENSION_2D;
         cfg.layout = AGX_LAYOUT_LINEAR;
         cfg.width = AGX_TEXTURE_BUFFER_WIDTH;
         cfg.height = DIV_ROUND_UP(size_px, cfg.width);
         cfg.stride = (cfg.width * blocksize_B) - 4;
         cfg.layers = 1;
         cfg.levels = 1;

         cfg.buffer += tex->layout.level_offsets_B[level];
         cfg.level = 0;
      } else {
         cfg.width = view->resource->width0;
         cfg.height = view->resource->height0;
         cfg.level = level;

         unsigned layers = view->u.tex.last_layer - layer + 1;

         if (tex->layout.tiling == AIL_TILING_LINEAR &&
             (target == PIPE_TEXTURE_1D_ARRAY ||
              target == PIPE_TEXTURE_2D_ARRAY)) {

            cfg.depth_linear = layers;
            cfg.layer_stride_linear = (tex->layout.layer_stride_B - 0x80);
            cfg.extended = true;
         } else {
            assert((tex->layout.tiling != AIL_TILING_LINEAR) || (layers == 1));
            cfg.layers = layers;
         }

         if (tex->layout.tiling == AIL_TILING_LINEAR) {
            cfg.stride = ail_get_linear_stride_B(&tex->layout, level) - 4;
            cfg.levels = 1;
         } else {
            cfg.page_aligned_layers = tex->layout.page_aligned_layers;
            cfg.levels = tex->base.last_level + 1;
         }

         if (tex->base.nr_samples > 1)
            cfg.samples = agx_translate_sample_count(tex->base.nr_samples);
      }

      if (ail_is_compressed(&tex->layout) && !emrt) {
         cfg.compressed_1 = true;
         cfg.extended = true;

         cfg.acceleration_buffer =
            agx_map_texture_gpu(tex, 0) + tex->layout.metadata_offset_B +
            (layer * tex->layout.compression_layer_stride_B);
      }

      /* When the descriptor isn't extended architecturally, we can use the last
       * 8 bytes as a sideband. We use it to provide metadata for image atomics.
       */
      if (!cfg.extended && (tex->layout.writeable_image || emrt) &&
          tex->base.target != PIPE_BUFFER) {

         if (util_res_sample_count(&tex->base) > 1) {
            cfg.aligned_width_msaa_sw =
               align(u_minify(view->resource->width0, level),
                     tex->layout.tilesize_el[level].width_el);
         } else {
            cfg.level_offset_sw =
               ail_get_level_offset_B(&tex->layout, cfg.level);
         }

         cfg.sample_count_log2_sw = util_logbase2(tex->base.nr_samples);

         if (tex->layout.tiling == AIL_TILING_TWIDDLED || emrt) {
            struct ail_tile tile_size = tex->layout.tilesize_el[level];
            cfg.tile_width_sw = tile_size.width_el;
            cfg.tile_height_sw = tile_size.height_el;

            cfg.layer_stride_sw = tex->layout.layer_stride_B;
         }
      }
   };
}

/* Likewise constant buffers, textures, and samplers are handled in a common
 * per-draw path, with dirty tracking to reduce the costs involved.
 */

static void
agx_set_constant_buffer(struct pipe_context *pctx, enum pipe_shader_type shader,
                        uint index, bool take_ownership,
                        const struct pipe_constant_buffer *cb)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_stage *s = &ctx->stage[shader];
   struct pipe_constant_buffer *constants = &s->cb[index];

   util_copy_constant_buffer(&s->cb[index], cb, take_ownership);

   /* Upload user buffer immediately */
   if (constants->user_buffer && !constants->buffer) {
      u_upload_data(ctx->base.const_uploader, 0, constants->buffer_size, 64,
                    constants->user_buffer, &constants->buffer_offset,
                    &constants->buffer);
   }

   unsigned mask = (1 << index);

   if (cb)
      s->cb_mask |= mask;
   else
      s->cb_mask &= ~mask;

   ctx->stage[shader].dirty |= AGX_STAGE_DIRTY_CONST;
}

static void
agx_surface_destroy(struct pipe_context *ctx, struct pipe_surface *surface)
{
   pipe_resource_reference(&surface->texture, NULL);
   FREE(surface);
}

static void
agx_delete_state(struct pipe_context *ctx, void *state)
{
   FREE(state);
}

/* BOs added to the batch in the uniform upload path */

static void
agx_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                       const struct pipe_vertex_buffer *buffers)
{
   struct agx_context *ctx = agx_context(pctx);

   util_set_vertex_buffers_mask(ctx->vertex_buffers, &ctx->vb_mask, buffers,
                                count, true);

   ctx->dirty |= AGX_DIRTY_VERTEX;
}

static void *
agx_create_vertex_elements(struct pipe_context *ctx, unsigned count,
                           const struct pipe_vertex_element *state)
{
   assert(count <= AGX_MAX_ATTRIBS);

   struct agx_vertex_elements *so = calloc(1, sizeof(*so));

   for (unsigned i = 0; i < count; ++i) {
      const struct pipe_vertex_element ve = state[i];

      const struct util_format_description *desc =
         util_format_description(ve.src_format);
      unsigned chan_size = desc->channel[0].size / 8;
      assert((ve.src_offset & (chan_size - 1)) == 0);

      so->buffers[i] = ve.vertex_buffer_index;
      so->src_offsets[i] = ve.src_offset;

      so->key[i] = (struct agx_velem_key){
         .stride = ve.src_stride,
         .format = ve.src_format,
         .divisor = ve.instance_divisor,
         .instanced = ve.instance_divisor > 0,
      };
   }

   return so;
}

static void
agx_bind_vertex_elements_state(struct pipe_context *pctx, void *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   ctx->attributes = cso;
   ctx->dirty |= AGX_DIRTY_VERTEX;
}

DERIVE_HASH_TABLE(asahi_vs_shader_key);
DERIVE_HASH_TABLE(asahi_gs_shader_key);
DERIVE_HASH_TABLE(asahi_fs_shader_key);
DERIVE_HASH_TABLE(agx_fast_link_key);

/* No compute variants */
static uint32_t
asahi_cs_shader_key_hash(const void *key)
{
   return 0;
}

static bool
asahi_cs_shader_key_equal(const void *a, const void *b)
{
   return true;
}

/* Dynamic lowered I/O version of nir_lower_clip_halfz */
static bool
agx_nir_lower_clip_m1_1(nir_builder *b, nir_intrinsic_instr *intr,
                        UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;
   if (nir_intrinsic_io_semantics(intr).location != VARYING_SLOT_POS)
      return false;

   assert(nir_intrinsic_component(intr) == 0 && "not yet scalarized");
   b->cursor = nir_before_instr(&intr->instr);

   nir_def *pos = intr->src[0].ssa;
   nir_def *z = nir_channel(b, pos, 2);
   nir_def *w = nir_channel(b, pos, 3);
   nir_def *c = nir_load_clip_z_coeff_agx(b);

   /* Lerp. If c = 0, reduces to z. If c = 1/2, reduces to (z + w)/2 */
   nir_def *new_z = nir_ffma(b, nir_fneg(b, z), c, nir_ffma(b, w, c, z));
   nir_src_rewrite(&intr->src[0], nir_vector_insert_imm(b, pos, new_z, 2));
   return true;
}

static nir_def *
nir_channel_or_undef(nir_builder *b, nir_def *def, signed int channel)
{
   if (channel >= 0 && channel < def->num_components)
      return nir_channel(b, def, channel);
   else
      return nir_undef(b, 1, def->bit_size);
}

/*
 * To implement point sprites, we'll replace TEX0...7 with point coordinate
 * reads as required. However, the .zw needs to read back 0.0/1.0. This pass
 * fixes up TEX loads of Z and W according to a uniform passed in a sideband,
 * eliminating shader variants.
 */
static bool
agx_nir_lower_point_sprite_zw(nir_builder *b, nir_intrinsic_instr *intr,
                              UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_input &&
       intr->intrinsic != nir_intrinsic_load_interpolated_input)
      return false;

   gl_varying_slot loc = nir_intrinsic_io_semantics(intr).location;
   if (!(loc >= VARYING_SLOT_TEX0 && loc <= VARYING_SLOT_TEX7))
      return false;

   b->cursor = nir_after_instr(&intr->instr);
   unsigned component = nir_intrinsic_component(intr);

   nir_def *mask = nir_load_tex_sprite_mask_agx(b);
   nir_def *location = nir_iadd_imm(b, nir_get_io_offset_src(intr)->ssa,
                                    loc - VARYING_SLOT_TEX0);
   nir_def *bit = nir_ishl(b, nir_imm_intN_t(b, 1, 16), location);
   nir_def *replace = nir_i2b(b, nir_iand(b, mask, bit));

   nir_def *vec = nir_pad_vec4(b, &intr->def);
   nir_def *chans[4] = {NULL, NULL, nir_imm_floatN_t(b, 0.0, vec->bit_size),
                        nir_imm_floatN_t(b, 1.0, vec->bit_size)};

   for (unsigned i = 0; i < 4; ++i) {
      nir_def *chan = nir_channel_or_undef(b, vec, i - component);
      chans[i] = chans[i] ? nir_bcsel(b, replace, chans[i], chan) : chan;
   }

   nir_def *new_vec = nir_vec(b, &chans[component], intr->def.num_components);
   nir_def_rewrite_uses_after(&intr->def, new_vec, new_vec->parent_instr);
   return true;
}

/*
 * Compile a NIR shader. The only lowering left at this point is sysvals. The
 * shader key should have already been applied. agx_compile_variant may call
 * this multiple times if there are auxiliary shaders.
 */
static struct agx_compiled_shader *
agx_compile_nir(struct agx_device *dev, nir_shader *nir,
                struct util_debug_callback *debug, enum pipe_shader_type stage,
                bool internal_kernel, bool terminal, bool secondary,
                unsigned cf_base, BITSET_WORD *attrib_components_read)
{
   struct agx_compiled_shader *compiled = CALLOC_STRUCT(agx_compiled_shader);
   compiled->stage = stage;
   if (attrib_components_read)
      BITSET_COPY(compiled->attrib_components_read, attrib_components_read);

   struct agx_shader_key key = {
      .dev = agx_gather_device_key(dev),
      .libagx = dev->libagx,
      .has_scratch = !secondary,
      .promote_constants = true,
      .no_stop = !terminal,
      .secondary = secondary,
   };

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, agx_nir_lower_interpolation);
   }

   /* We always use dynamic sample shading in the GL driver. Indicate that. */
   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       nir->info.fs.uses_sample_shading)
      key.fs.inside_sample_loop = true;

   if (internal_kernel) {
      key.reserved_preamble = 8;
   } else if (!secondary) {
      NIR_PASS(_, nir, agx_nir_lower_sysvals, stage, true);
      NIR_PASS(_, nir, agx_nir_layout_uniforms, compiled,
               &key.reserved_preamble);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      key.fs.cf_base = cf_base;
   }

   agx_compile_shader_nir(nir, &key, debug, &compiled->b);

   if (compiled->b.binary_size && !secondary) {
      compiled->bo = agx_bo_create(dev, compiled->b.binary_size, 0,
                                   AGX_BO_EXEC | AGX_BO_LOW_VA, "Executable");

      memcpy(compiled->bo->map, compiled->b.binary, compiled->b.binary_size);
   }

   return compiled;
}

static struct agx_compiled_shader *
agx_build_meta_shader_internal(struct agx_context *ctx,
                               meta_shader_builder_t builder, void *data,
                               size_t data_size, bool prolog, bool epilog,
                               unsigned cf_base, bool internal_kernel);

/* Does not take ownership of key. Clones if necessary. */
static struct agx_compiled_shader *
agx_compile_variant(struct agx_device *dev, struct pipe_context *pctx,
                    struct agx_uncompiled_shader *so,
                    struct util_debug_callback *debug,
                    union asahi_shader_key *key_)
{
   struct blob_reader reader;
   blob_reader_init(&reader, so->serialized_nir.data, so->serialized_nir.size);
   nir_shader *nir = nir_deserialize(NULL, &agx_nir_options, &reader);

   /* Auxiliary programs */
   enum mesa_prim gs_out_prim = MESA_PRIM_MAX;
   uint64_t outputs = 0;
   struct agx_fs_epilog_link_info epilog_key = {false};
   unsigned gs_out_count_words = 0;
   nir_shader *gs_count = NULL;
   nir_shader *gs_copy = NULL;
   nir_shader *pre_gs = NULL;
   BITSET_DECLARE(attrib_components_read, VERT_ATTRIB_MAX * 4) = {0};

   /* This can happen at inopportune times and cause jank, log it */
   perf_debug(dev, "Compiling %s shader variant #%u",
              _mesa_shader_stage_to_abbrev(so->type),
              _mesa_hash_table_num_entries(so->variants));

   struct agx_unlinked_uvs_layout uvs = {0};
   bool translucent = false;

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      struct asahi_vs_shader_key *key = &key_->vs;

      NIR_PASS(_, nir, agx_nir_lower_vs_input_to_prolog,
               attrib_components_read);

      if (key->hw) {
         NIR_PASS(_, nir, agx_nir_lower_point_size, true);
         NIR_PASS(_, nir, nir_shader_intrinsics_pass, agx_nir_lower_clip_m1_1,
                  nir_metadata_control_flow, NULL);

         NIR_PASS(_, nir, nir_lower_io_to_scalar, nir_var_shader_out, NULL,
                  NULL);
         NIR_PASS(_, nir, agx_nir_lower_cull_distance_vs);
         NIR_PASS(_, nir, agx_nir_lower_uvs, &uvs);
      } else {
         NIR_PASS(_, nir, agx_nir_lower_vs_before_gs, dev->libagx);

         /* Turn into a compute shader now that we're free of vertexisms */
         nir->info.stage = MESA_SHADER_COMPUTE;
         memset(&nir->info.cs, 0, sizeof(nir->info.cs));
         nir->xfb_info = NULL;
         outputs = nir->info.outputs_written;
      }
   } else if (nir->info.stage == MESA_SHADER_TESS_CTRL) {
      NIR_PASS_V(nir, agx_nir_lower_tcs, dev->libagx);
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      struct asahi_gs_shader_key *key = &key_->gs;

      NIR_PASS(_, nir, agx_nir_lower_gs, dev->libagx, key->rasterizer_discard,
               &gs_count, &gs_copy, &pre_gs, &gs_out_prim, &gs_out_count_words);
   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      struct asahi_fs_shader_key *key = &key_->fs;

      /* Discards must be lowering before lowering MSAA to handle discards */
      NIR_PASS(_, nir, agx_nir_lower_discard_zs_emit);
      NIR_PASS(_, nir, agx_nir_lower_fs_output_to_epilog, &epilog_key);

      if (nir->info.fs.uses_fbfetch_output) {
         struct agx_tilebuffer_layout tib = agx_build_tilebuffer_layout(
            key->rt_formats, ARRAY_SIZE(key->rt_formats), key->nr_samples,
            true);

         if (dev->debug & AGX_DBG_SMALLTILE)
            tib.tile_size = (struct agx_tile_size){16, 16};

         /* XXX: don't replicate this all over the driver */
         unsigned rt_spill_base = BITSET_LAST_BIT(nir->info.textures_used) +
                                  (2 * BITSET_LAST_BIT(nir->info.images_used));
         unsigned rt_spill = rt_spill_base;
         NIR_PASS(_, nir, agx_nir_lower_tilebuffer, &tib, NULL, &rt_spill, NULL,
                  &translucent);
      }

      if (nir->info.fs.uses_sample_shading) {
         /* Ensure the sample ID is preserved in register */
         nir_builder b =
            nir_builder_at(nir_after_impl(nir_shader_get_entrypoint(nir)));
         nir_export_agx(
            &b,
            nir_load_exported_agx(&b, 1, 16, .base = AGX_ABI_FIN_SAMPLE_MASK),
            .base = AGX_ABI_FOUT_SAMPLE_MASK);

         NIR_PASS(_, nir, agx_nir_lower_to_per_sample);
      }

      NIR_PASS(_, nir, agx_nir_lower_sample_mask);
      NIR_PASS(_, nir, agx_nir_lower_fs_active_samples_to_register);
   }

   NIR_PASS(_, nir, agx_nir_lower_multisampled_image_store);

   struct agx_compiled_shader *compiled = agx_compile_nir(
      dev, nir, debug, so->type, false, so->type != PIPE_SHADER_FRAGMENT, false,
      0, attrib_components_read);

   if (so->type == PIPE_SHADER_FRAGMENT) {
      /* XXX: don't replicate this all over the driver */
      epilog_key.rt_spill_base = BITSET_LAST_BIT(nir->info.textures_used) +
                                 (2 * BITSET_LAST_BIT(nir->info.images_used));

      compiled->epilog_key = epilog_key;
      compiled->b.info.reads_tib |= translucent;
   }

   compiled->so = so;
   compiled->uvs = uvs;

   /* Compile auxiliary programs */
   if (gs_count) {
      compiled->gs_count = agx_compile_nir(dev, gs_count, debug, so->type,
                                           false, true, false, 0, NULL);
      compiled->gs_count->so = so;
   }

   if (pre_gs) {
      compiled->pre_gs = agx_compile_nir(
         dev, pre_gs, debug, PIPE_SHADER_COMPUTE, false, true, false, 0, NULL);
   }

   if (gs_copy) {
      /* Replace the point size write if present, but do not insert a write:
       * the GS rast program writes point size iff we have points.
       */
      NIR_PASS(_, gs_copy, agx_nir_lower_point_size, false);

      NIR_PASS(_, gs_copy, nir_shader_intrinsics_pass, agx_nir_lower_clip_m1_1,
               nir_metadata_control_flow, NULL);

      NIR_PASS(_, gs_copy, nir_lower_io_to_scalar, nir_var_shader_out, NULL,
               NULL);
      NIR_PASS(_, gs_copy, agx_nir_lower_cull_distance_vs);

      struct agx_unlinked_uvs_layout uvs = {0};
      NIR_PASS(_, gs_copy, agx_nir_lower_uvs, &uvs);

      compiled->gs_copy =
         agx_compile_nir(dev, gs_copy, debug, PIPE_SHADER_GEOMETRY, false, true,
                         false, 0, NULL);
      compiled->gs_copy->so = so;
      compiled->gs_copy->stage = so->type;
      compiled->gs_copy->uvs = uvs;
   }

   compiled->gs_output_mode = gs_out_prim;
   compiled->gs_count_words = gs_out_count_words;
   compiled->b.info.outputs = outputs;

   ralloc_free(nir);
   ralloc_free(pre_gs);
   ralloc_free(gs_count);
   return compiled;
}

static struct agx_compiled_shader *
agx_get_shader_variant(struct agx_screen *screen, struct pipe_context *pctx,
                       struct agx_uncompiled_shader *so,
                       struct util_debug_callback *debug,
                       union asahi_shader_key *key)
{
   struct agx_compiled_shader *compiled =
      agx_disk_cache_retrieve(screen, so, key);

   if (!compiled) {
      compiled = agx_compile_variant(&screen->dev, pctx, so, debug, key);
      agx_disk_cache_store(screen->disk_cache, so, key, compiled);
   }

   /* key may be destroyed after we return, so clone it before using it as a
    * hash table key. The clone is logically owned by the hash table.
    */
   union asahi_shader_key *cloned_key =
      rzalloc(so->variants, union asahi_shader_key);

   if (so->type == PIPE_SHADER_FRAGMENT) {
      memcpy(cloned_key, key, sizeof(struct asahi_fs_shader_key));
   } else if (so->type == PIPE_SHADER_VERTEX ||
              so->type == PIPE_SHADER_TESS_EVAL) {
      memcpy(cloned_key, key, sizeof(struct asahi_vs_shader_key));
   } else if (so->type == PIPE_SHADER_GEOMETRY) {
      memcpy(cloned_key, key, sizeof(struct asahi_gs_shader_key));
   } else {
      assert(gl_shader_stage_is_compute(so->type) ||
             so->type == PIPE_SHADER_TESS_CTRL);
      /* No key */
   }

   _mesa_hash_table_insert(so->variants, cloned_key, compiled);

   return compiled;
}

static int
glsl_type_size(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

static void
agx_shader_initialize(struct agx_device *dev, struct agx_uncompiled_shader *so,
                      nir_shader *nir, bool support_lod_bias, bool robust)
{
   if (nir->info.stage == MESA_SHADER_KERNEL)
      nir->info.stage = MESA_SHADER_COMPUTE;

   blob_init(&so->early_serialized_nir);
   nir_serialize(&so->early_serialized_nir, nir, true);

   nir_lower_robust_access_options robustness = {
      /* Images accessed through the texture or PBE hardware are robust, so we
       * don't set lower_image. However, buffer images and image atomics are
       * lowered so require robustness lowering.
       */
      .lower_buffer_image = true,
      .lower_image_atomic = true,

      /* Buffer access is based on raw pointers and hence needs lowering to be
         robust */
      .lower_ubo = robust,
      .lower_ssbo = robust,
   };

   /* We need to lower robustness before bindings, since robustness lowering
    * affects the bindings used.
    */
   NIR_PASS(_, nir, nir_lower_robust_access, &robustness);

   /* Similarly, we need to do early texture lowering before bindings */
   NIR_PASS(_, nir, agx_nir_lower_texture_early, support_lod_bias);

   /* We need to lower binding tables before calling agx_preprocess_nir, since
    * that does texture lowering that needs to know the binding model.
    */
   NIR_PASS(_, nir, agx_nir_lower_bindings, &so->uses_bindless_samplers);

   /* We need to do some I/O lowering before lowering textures */
   so->info.nr_bindful_textures = BITSET_LAST_BIT(nir->info.textures_used);
   so->info.nr_bindful_images = BITSET_LAST_BIT(nir->info.images_used);

   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out,
            glsl_type_size, nir_lower_io_lower_64bit_to_32);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      struct agx_interp_info interp = agx_gather_interp_info(nir);

      /* Interpolate varyings at fp16 and write to the tilebuffer at fp16. As an
       * exception, interpolate flat shaded at fp32. This works around a
       * hardware limitation. The resulting code (with an extra f2f16 at the end
       * if needed) matches what Metal produces.
       */
      if (likely(!(dev->debug & AGX_DBG_NO16))) {
         uint64_t texcoord = agx_gather_texcoords(nir);

         NIR_PASS(_, nir, nir_lower_mediump_io,
                  nir_var_shader_in | nir_var_shader_out,
                  ~(interp.flat | texcoord), false);
      }

      so->info.inputs_flat_shaded = interp.flat;
      so->info.inputs_linear_shaded = interp.linear;
      so->info.uses_fbfetch = nir->info.fs.uses_fbfetch_output;
   } else if (nir->info.stage == MESA_SHADER_VERTEX ||
              nir->info.stage == MESA_SHADER_TESS_EVAL) {
      so->info.has_edgeflags = nir->info.outputs_written & VARYING_BIT_EDGE;
      so->info.cull_distance_size = nir->info.cull_distance_array_size;
   }

   NIR_PASS(_, nir, agx_nir_lower_texture, true);
   NIR_PASS(_, nir, nir_lower_ssbo, NULL);

   agx_preprocess_nir(nir, dev->libagx);

   if (nir->info.stage == MESA_SHADER_FRAGMENT &&
       (nir->info.inputs_read & VARYING_BITS_TEX_ANY)) {

      NIR_PASS(_, nir, nir_shader_intrinsics_pass,
               agx_nir_lower_point_sprite_zw, nir_metadata_control_flow, NULL);
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, agx_nir_lower_sample_intrinsics, true);
   }

   so->type = pipe_shader_type_from_mesa(nir->info.stage);

   if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS(_, nir, agx_nir_lower_tes, dev->libagx, true);
   }

   blob_init(&so->serialized_nir);
   nir_serialize(&so->serialized_nir, nir, true);
   _mesa_sha1_compute(so->serialized_nir.data, so->serialized_nir.size,
                      so->nir_sha1);

   so->has_xfb_info = (nir->xfb_info != NULL);

   static_assert(
      ARRAY_SIZE(so->xfb_strides) == ARRAY_SIZE(nir->info.xfb_stride),
      "known target count");

   if (so->has_xfb_info) {
      struct nir_xfb_info *xfb = nir->xfb_info;

      for (unsigned i = 0; i < ARRAY_SIZE(so->xfb_strides); ++i) {
         so->xfb_strides[i] = xfb->buffers[i].stride;
      }
   }
}

static void *
agx_create_shader_state(struct pipe_context *pctx,
                        const struct pipe_shader_state *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_uncompiled_shader *so =
      rzalloc(NULL, struct agx_uncompiled_shader);
   struct agx_device *dev = agx_device(pctx->screen);

   if (!so)
      return NULL;

   so->base = *cso;

   nir_shader *nir = cso->type == PIPE_SHADER_IR_NIR
                        ? cso->ir.nir
                        : tgsi_to_nir(cso->tokens, pctx->screen, false);

   if (nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_TESS_EVAL) {
      so->variants = asahi_vs_shader_key_table_create(so);
      so->linked_shaders = agx_fast_link_key_table_create(so);
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      so->variants = asahi_gs_shader_key_table_create(so);
   } else if (nir->info.stage == MESA_SHADER_TESS_CTRL) {
      /* No variants */
      so->variants = _mesa_hash_table_create(NULL, asahi_cs_shader_key_hash,
                                             asahi_cs_shader_key_equal);
   } else {
      so->variants = asahi_fs_shader_key_table_create(so);
      so->linked_shaders = agx_fast_link_key_table_create(so);
   }

   if (nir->info.stage == MESA_SHADER_TESS_EVAL ||
       nir->info.stage == MESA_SHADER_TESS_CTRL) {

      so->tess.ccw = nir->info.tess.ccw;
      so->tess.point_mode = nir->info.tess.point_mode;
      so->tess.spacing = nir->info.tess.spacing;
      so->tess.output_patch_size = nir->info.tess.tcs_vertices_out;
      so->tess.primitive = nir->info.tess._primitive_mode;
      so->tess.per_vertex_outputs = agx_tcs_per_vertex_outputs(nir);
      so->tess.nr_patch_outputs =
         util_last_bit(nir->info.patch_outputs_written);
      if (nir->info.stage == MESA_SHADER_TESS_CTRL)
         so->tess.output_stride = agx_tcs_output_stride(nir);
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      so->gs_mode = nir->info.gs.output_primitive;
   }

   agx_shader_initialize(dev, so, nir, ctx->support_lod_bias, ctx->robust);
   gl_shader_stage next_stage = nir->info.next_stage;

   /* We're done with the NIR, throw it away */
   ralloc_free(nir);
   nir = NULL;

   /* Precompile shaders that have a small key. For shader-db, precompile a
    * shader with a default key. This could be improved but hopefully this is
    * acceptable for now.
    */
   if ((so->type == PIPE_SHADER_TESS_CTRL) ||
       (so->type == PIPE_SHADER_FRAGMENT && !so->info.uses_fbfetch)) {
      union asahi_shader_key key = {0};
      agx_get_shader_variant(agx_screen(pctx->screen), pctx, so, &pctx->debug,
                             &key);
   } else if (so->type == PIPE_SHADER_VERTEX) {
      union asahi_shader_key key = {
         .vs.hw = next_stage == MESA_SHADER_FRAGMENT,
      };
      agx_get_shader_variant(agx_screen(pctx->screen), pctx, so, &pctx->debug,
                             &key);

      if (!next_stage) {
         key.vs.hw = true;
         agx_get_shader_variant(agx_screen(pctx->screen), pctx, so,
                                &pctx->debug, &key);
      }
   } else if (dev->debug & AGX_DBG_PRECOMPILE) {
      union asahi_shader_key key = {0};

      switch (so->type) {
      case PIPE_SHADER_GEOMETRY:
         break;

      case PIPE_SHADER_TESS_EVAL:
         /* TODO: Tessellation shaders with shader-db */
         return so;

      case PIPE_SHADER_FRAGMENT:
         key.fs.nr_samples = 1;
         break;
      default:
         unreachable("Unknown shader stage in shader-db precompile");
      }

      agx_compile_variant(dev, pctx, so, &pctx->debug, &key);
   }

   return so;
}

static void *
agx_create_compute_state(struct pipe_context *pctx,
                         const struct pipe_compute_state *cso)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_device *dev = agx_device(pctx->screen);
   struct agx_uncompiled_shader *so =
      rzalloc(NULL, struct agx_uncompiled_shader);

   if (!so)
      return NULL;

   so->variants = _mesa_hash_table_create(so, asahi_cs_shader_key_hash,
                                          asahi_cs_shader_key_equal);

   union asahi_shader_key key = {0};

   assert(cso->ir_type == PIPE_SHADER_IR_NIR && "TGSI kernels unsupported");
   nir_shader *nir = (void *)cso->prog;

   agx_shader_initialize(dev, so, nir, ctx->support_lod_bias, ctx->robust);
   agx_get_shader_variant(agx_screen(pctx->screen), pctx, so, &pctx->debug,
                          &key);

   /* We're done with the NIR, throw it away */
   ralloc_free(nir);
   return so;
}

static void
agx_get_compute_state_info(struct pipe_context *pctx, void *cso,
                           struct pipe_compute_state_object_info *info)
{
   union asahi_shader_key key = {0};
   struct agx_compiled_shader *so = agx_get_shader_variant(
      agx_screen(pctx->screen), pctx, cso, &pctx->debug, &key);

   info->max_threads =
      agx_occupancy_for_register_count(so->b.info.nr_gprs).max_threads;
   info->private_memory = 0;
   info->preferred_simd_size = 32;
   info->simd_sizes = 32;
}

/* Does not take ownership of key. Clones if necessary. */
static bool
agx_update_shader(struct agx_context *ctx, struct agx_compiled_shader **out,
                  enum pipe_shader_type stage, union asahi_shader_key *key)
{
   struct agx_uncompiled_shader *so = ctx->stage[stage].shader;
   assert(so != NULL);

   struct hash_entry *he = _mesa_hash_table_search(so->variants, key);

   if (he) {
      if ((*out) == he->data)
         return false;

      *out = he->data;
      return true;
   }

   struct agx_screen *screen = agx_screen(ctx->base.screen);
   *out = agx_get_shader_variant(screen, &ctx->base, so, &ctx->base.debug, key);
   return true;
}

static enum mesa_prim
rast_prim(enum mesa_prim mode, unsigned fill_mode)
{
   if (u_reduced_prim(mode) == MESA_PRIM_TRIANGLES) {
      if (fill_mode == PIPE_POLYGON_MODE_POINT)
         return MESA_PRIM_POINTS;
      else if (fill_mode == PIPE_POLYGON_MODE_LINE)
         return MESA_PRIM_LINES;
   }

   return mode;
}

static bool
lower_fs_prolog_abi(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *_)
{
   if (intr->intrinsic == nir_intrinsic_load_polygon_stipple_agx) {
      b->cursor = nir_instr_remove(&intr->instr);

      nir_def *root = nir_load_preamble(b, 1, 64, .base = 12);
      off_t stipple_offs = offsetof(struct agx_draw_uniforms, polygon_stipple);
      nir_def *stipple_ptr_ptr = nir_iadd_imm(b, root, stipple_offs);
      nir_def *base = nir_load_global_constant(b, stipple_ptr_ptr, 4, 1, 64);

      nir_def *row = intr->src[0].ssa;
      nir_def *addr = nir_iadd(b, base, nir_u2u64(b, nir_imul_imm(b, row, 4)));

      nir_def *pattern = nir_load_global_constant(b, addr, 4, 1, 32);
      nir_def_rewrite_uses(&intr->def, pattern);
      return true;
   } else if (intr->intrinsic == nir_intrinsic_load_stat_query_address_agx) {
      b->cursor = nir_instr_remove(&intr->instr);

      /* ABI: root descriptor address in u6_u7 */
      nir_def *root = nir_load_preamble(b, 1, intr->def.bit_size, .base = 12);

      off_t offs = offsetof(struct agx_draw_uniforms,
                            pipeline_statistics[nir_intrinsic_base(intr)]);

      nir_def *ptr = nir_iadd_imm(b, root, offs);
      nir_def *load = nir_load_global_constant(b, ptr, 4, 1, 64);
      nir_def_rewrite_uses(&intr->def, load);
      return true;
   } else {
      return false;
   }
}

static void
build_fs_prolog(nir_builder *b, const void *key)
{
   agx_nir_fs_prolog(b, key);

   NIR_PASS(_, b->shader, nir_shader_intrinsics_pass, lower_fs_prolog_abi,
            nir_metadata_control_flow, NULL);
}

static struct agx_linked_shader *
asahi_fast_link(struct agx_context *ctx, struct agx_uncompiled_shader *so,
                struct agx_fast_link_key *key)
{
   /* Try the cache */
   struct hash_entry *ent = _mesa_hash_table_search(so->linked_shaders, key);
   if (ent)
      return ent->data;

   struct agx_compiled_shader *prolog = NULL, *epilog = NULL;

   /* Build the prolog/epilog now */
   if (so->type == MESA_SHADER_FRAGMENT) {
      prolog = agx_build_meta_shader_internal(
         ctx, build_fs_prolog, &key->prolog.fs, sizeof(key->prolog.fs), true,
         false, key->prolog.fs.cf_base, false);

      epilog = agx_build_meta_shader_internal(
         ctx, agx_nir_fs_epilog, &key->epilog.fs, sizeof(key->epilog.fs), false,
         true, 0, false);

   } else {
      assert(so->type == MESA_SHADER_VERTEX ||
             so->type == MESA_SHADER_TESS_EVAL);

      prolog = agx_build_meta_shader_internal(
         ctx, agx_nir_vs_prolog, &key->prolog.vs, sizeof(key->prolog.vs), true,
         false, 0, false);
   }

   /* Fast-link it all together */
   struct agx_device *dev = agx_device(ctx->base.screen);

   struct agx_linked_shader *linked =
      rzalloc(so->linked_shaders, struct agx_linked_shader);
   agx_fast_link(linked, dev, so->type == PIPE_SHADER_FRAGMENT, &key->main->b,
                 &prolog->b, &epilog->b, key->nr_samples_shaded);

   /* Cache the fast linked program */
   union asahi_shader_key *cloned_key =
      ralloc_memdup(so->linked_shaders, key, sizeof(*key));
   _mesa_hash_table_insert(so->linked_shaders, cloned_key, linked);
   return linked;
}

static bool
agx_update_vs(struct agx_context *ctx, unsigned index_size_B)
{
   /* Only proceed if the shader or anything the key depends on changes
    *
    * vb_mask, attributes, vertex_buffers: VERTEX
    */
   if (!((ctx->dirty & (AGX_DIRTY_VS_PROG | AGX_DIRTY_VERTEX | AGX_DIRTY_XFB)) ||
         ctx->stage[PIPE_SHADER_TESS_EVAL].dirty ||
         ctx->stage[PIPE_SHADER_GEOMETRY].dirty ||
         ctx->stage[PIPE_SHADER_TESS_EVAL].shader ||
         ctx->stage[PIPE_SHADER_GEOMETRY].shader || ctx->in_tess))
      return false;

   struct asahi_vs_shader_key key = {
      .hw = !((ctx->stage[PIPE_SHADER_TESS_EVAL].shader && !ctx->in_tess) ||
              ctx->stage[PIPE_SHADER_GEOMETRY].shader),
   };

   agx_update_shader(ctx, &ctx->vs, PIPE_SHADER_VERTEX,
                     (union asahi_shader_key *)&key);

   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_fast_link_key link_key = {
      .prolog.vs.hw = key.hw,
      .prolog.vs.sw_index_size_B = key.hw ? 0 : index_size_B,

      .prolog.vs.robustness.level =
         ctx->robust ? AGX_ROBUSTNESS_GL : AGX_ROBUSTNESS_DISABLED,

      .prolog.vs.robustness.soft_fault = agx_has_soft_fault(dev),
      .main = ctx->vs,
   };

   STATIC_ASSERT(sizeof(link_key.prolog.vs.component_mask) ==
                 sizeof(ctx->vs->attrib_components_read));
   BITSET_COPY(link_key.prolog.vs.component_mask,
               ctx->vs->attrib_components_read);

   memcpy(link_key.prolog.vs.attribs, &ctx->attributes->key,
          sizeof(link_key.prolog.vs.attribs));

   void *old = ctx->linked.vs;

   ctx->linked.vs =
      asahi_fast_link(ctx, ctx->stage[PIPE_SHADER_VERTEX].shader, &link_key);

   return old != ctx->linked.vs;
}

static bool
agx_update_tcs(struct agx_context *ctx, const struct pipe_draw_info *info)
{
   assert(info->mode == MESA_PRIM_PATCHES);

   ctx->tcs = _mesa_hash_table_next_entry(
                 ctx->stage[PIPE_SHADER_TESS_CTRL].shader->variants, NULL)
                 ->data;
   return true;
}

static bool
agx_update_gs(struct agx_context *ctx, const struct pipe_draw_info *info,
              const struct pipe_draw_indirect_info *indirect)
{
   /* Only proceed if there is a geometry shader. Due to input assembly
    * dependence, we don't bother to dirty track right now.
    */
   if (!ctx->stage[PIPE_SHADER_GEOMETRY].shader) {
      ctx->gs = NULL;
      return false;
   }

   /* Transform feedback always happens via the geometry shader, so look there
    * to get the XFB strides.
    */
   struct agx_uncompiled_shader *gs = ctx->stage[PIPE_SHADER_GEOMETRY].shader;

   for (unsigned i = 0; i < ctx->streamout.num_targets; ++i) {
      struct agx_streamout_target *tgt =
         agx_so_target(ctx->streamout.targets[i]);

      if (tgt != NULL)
         tgt->stride = gs->xfb_strides[i];
   }

   struct asahi_gs_shader_key key = {
      .rasterizer_discard = ctx->rast->base.rasterizer_discard,
   };

   return agx_update_shader(ctx, &ctx->gs, PIPE_SHADER_GEOMETRY,
                            (union asahi_shader_key *)&key);
}

static enum pipe_blendfactor
optimize_blend_factor_w_1(enum pipe_blendfactor f)
{
   if (f == PIPE_BLENDFACTOR_SRC_ALPHA)
      return PIPE_BLENDFACTOR_ONE;
   else if (f == PIPE_BLENDFACTOR_INV_SRC_ALPHA)
      return PIPE_BLENDFACTOR_ZERO;
   else
      return f;
}

static bool
agx_update_fs(struct agx_batch *batch)
{
   struct agx_context *ctx = batch->ctx;

   /* Only proceed if the shader or anything the key depends on changes
    *
    * batch->key: implicitly dirties everything, no explicit check
    * rast: RS
    * blend: BLEND
    * sample_mask: SAMPLE_MASK
    * reduced_prim: PRIM
    */
   if (!(ctx->dirty & (AGX_DIRTY_VS_PROG | AGX_DIRTY_FS_PROG | AGX_DIRTY_RS |
                       AGX_DIRTY_BLEND | AGX_DIRTY_SAMPLE_MASK |
                       AGX_DIRTY_PRIM | AGX_DIRTY_QUERY)))
      return false;

   struct agx_device *dev = agx_device(ctx->base.screen);
   unsigned nr_samples = util_framebuffer_get_num_samples(&batch->key);

   /* Get main shader */
   struct asahi_fs_shader_key key = {0};

   if (ctx->stage[PIPE_SHADER_FRAGMENT].shader->info.uses_fbfetch) {
      key.nr_samples = nr_samples;

      for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
         struct pipe_surface *surf = batch->key.cbufs[i];

         key.rt_formats[i] = surf ? surf->format : PIPE_FORMAT_NONE;
      }
   }

   agx_update_shader(ctx, &ctx->fs, PIPE_SHADER_FRAGMENT,
                     (union asahi_shader_key *)&key);

   /* Fast link with prolog/epilog */
   bool msaa = ctx->rast->base.multisample;
   unsigned sample_mask = ctx->sample_mask & BITFIELD_MASK(nr_samples);

   struct agx_fast_link_key link_key = {
      .prolog.fs.statistics =
         ctx->pipeline_statistics[PIPE_STAT_QUERY_PS_INVOCATIONS],

      .prolog.fs.cull_distance_size =
         ctx->stage[MESA_SHADER_VERTEX].shader->info.cull_distance_size,

      .prolog.fs.polygon_stipple =
         ctx->rast->base.poly_stipple_enable &&
         rast_prim(batch->reduced_prim, ctx->rast->base.fill_front) ==
            MESA_PRIM_TRIANGLES,

      .prolog.fs.api_sample_mask =
         (msaa && nr_samples > 1 && sample_mask != BITFIELD_MASK(nr_samples))
            ? sample_mask
            : 0xff,

      .epilog.fs.nr_samples = nr_samples,
      .epilog.fs.link = ctx->fs->epilog_key,
      .epilog.fs.force_small_tile = dev->debug & AGX_DBG_SMALLTILE,

      .main = ctx->fs,
      .nr_samples_shaded = ctx->fs->epilog_key.sample_shading ? nr_samples : 0,
   };

   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) {
      struct pipe_surface *surf = batch->key.cbufs[i];

      link_key.epilog.fs.rt_formats[i] = surf ? surf->format : PIPE_FORMAT_NONE;
      link_key.epilog.fs.remap[i] =
         link_key.epilog.fs.link.broadcast_rt0 ? 0 : i;
   }

   memcpy(&link_key.epilog.fs.blend, &ctx->blend->key,
          sizeof(link_key.epilog.fs.blend));

   /* Normalize */
   if (!agx_tilebuffer_spills(&batch->tilebuffer_layout))
      link_key.epilog.fs.link.rt_spill_base = 0;

   /* Try to disable blending to get rid of some fsats */
   if (link_key.epilog.fs.link.loc0_w_1) {
      struct agx_blend_rt_key *k = &link_key.epilog.fs.blend.rt[0];

      k->rgb_src_factor = optimize_blend_factor_w_1(k->rgb_src_factor);
      k->rgb_dst_factor = optimize_blend_factor_w_1(k->rgb_dst_factor);

      k->alpha_src_factor = optimize_blend_factor_w_1(k->alpha_src_factor);
      k->alpha_dst_factor = optimize_blend_factor_w_1(k->alpha_dst_factor);
   }

   link_key.epilog.fs.blend.alpha_to_coverage &= msaa;

   /* The main shader must not run tests if the epilog will */
   bool epilog_discards = link_key.epilog.fs.blend.alpha_to_coverage;
   batch->uniforms.no_epilog_discard = !epilog_discards ? ~0 : 0;

   bool prolog_discards = (link_key.prolog.fs.api_sample_mask != 0xff ||
                           link_key.prolog.fs.cull_distance_size ||
                           link_key.prolog.fs.polygon_stipple);

   /* The prolog runs tests if neither the main shader nor epilog will */
   link_key.prolog.fs.run_zs_tests = !ctx->fs->b.info.writes_sample_mask &&
                                     !epilog_discards && prolog_discards;

   if (link_key.prolog.fs.cull_distance_size)
      link_key.prolog.fs.cf_base = ctx->fs->b.info.varyings.fs.nr_cf;

   void *old = ctx->linked.fs;

   ctx->linked.fs =
      asahi_fast_link(ctx, ctx->stage[PIPE_SHADER_FRAGMENT].shader, &link_key);

   return old != ctx->linked.fs;
}

static void
agx_bind_shader_state(struct pipe_context *pctx, void *cso,
                      enum pipe_shader_type stage)
{
   struct agx_context *ctx = agx_context(pctx);

   if (stage == PIPE_SHADER_VERTEX)
      ctx->dirty |= AGX_DIRTY_VS_PROG;
   else if (stage == PIPE_SHADER_FRAGMENT)
      ctx->dirty |= AGX_DIRTY_FS_PROG;
   else
      ctx->stage[stage].dirty = ~0;

   ctx->stage[stage].shader = cso;
}

static void
agx_bind_vs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, PIPE_SHADER_VERTEX);
}

static void
agx_bind_fs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, PIPE_SHADER_FRAGMENT);
}

static void
agx_bind_gs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, PIPE_SHADER_GEOMETRY);
}

static void
agx_bind_tcs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, PIPE_SHADER_TESS_CTRL);
}

static void
agx_bind_tes_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, PIPE_SHADER_TESS_EVAL);
}

static void
agx_bind_cs_state(struct pipe_context *pctx, void *cso)
{
   agx_bind_shader_state(pctx, cso, PIPE_SHADER_COMPUTE);
}

/* Forward declare because of the recursion hit with geometry shaders */
static void agx_delete_uncompiled_shader(struct agx_device *dev,
                                         struct agx_uncompiled_shader *so);

static void
agx_delete_compiled_shader(struct agx_device *dev,
                           struct agx_compiled_shader *so)
{
   if (so->gs_count)
      agx_delete_compiled_shader(dev, so->gs_count);

   if (so->pre_gs)
      agx_delete_compiled_shader(dev, so->pre_gs);

   if (so->gs_copy)
      agx_delete_compiled_shader(dev, so->gs_copy);

   agx_bo_unreference(dev, so->bo);
   FREE(so);
}

static void
agx_delete_uncompiled_shader(struct agx_device *dev,
                             struct agx_uncompiled_shader *so)
{
   hash_table_foreach(so->variants, ent) {
      agx_delete_compiled_shader(dev, ent->data);
   }

   _mesa_hash_table_destroy(so->variants, NULL);
   blob_finish(&so->serialized_nir);
   blob_finish(&so->early_serialized_nir);

   for (unsigned i = 0; i < MESA_PRIM_COUNT; ++i) {
      for (unsigned j = 0; j < 3; ++j) {
         for (unsigned k = 0; k < 2; ++k) {
            if (so->passthrough_progs[i][j][k])
               agx_delete_uncompiled_shader(dev,
                                            so->passthrough_progs[i][j][k]);
         }
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(so->passthrough_tcs); ++i) {
      if (so->passthrough_tcs[i])
         agx_delete_uncompiled_shader(dev, so->passthrough_tcs[i]);
   }

   ralloc_free(so);
}

static void
agx_delete_shader_state(struct pipe_context *ctx, void *cso)
{
   struct agx_device *dev = agx_device(ctx->screen);
   agx_delete_uncompiled_shader(dev, cso);
}

struct agx_generic_meta_key {
   meta_shader_builder_t builder;
   size_t key_size;
   uint8_t key[];
};

static uint32_t
meta_key_hash(const void *key_)
{
   const struct agx_generic_meta_key *key = key_;

   return _mesa_hash_data(key,
                          sizeof(struct agx_generic_meta_key) + key->key_size);
}

static bool
meta_key_equal(const void *a_, const void *b_)
{
   const struct agx_generic_meta_key *a = a_;
   const struct agx_generic_meta_key *b = b_;

   return a->builder == b->builder && a->key_size == b->key_size &&
          memcmp(a->key, b->key, a->key_size) == 0;
}

void
agx_init_meta_shaders(struct agx_context *ctx)
{
   ctx->generic_meta =
      _mesa_hash_table_create(ctx, meta_key_hash, meta_key_equal);
}

void
agx_destroy_meta_shaders(struct agx_context *ctx)
{
   struct agx_device *dev = agx_device(ctx->base.screen);
   hash_table_foreach(ctx->generic_meta, ent) {
      agx_delete_compiled_shader(dev, ent->data);
   }

   _mesa_hash_table_destroy(ctx->generic_meta, NULL);
}

static struct agx_compiled_shader *
agx_build_meta_shader_internal(struct agx_context *ctx,
                               meta_shader_builder_t builder, void *data,
                               size_t data_size, bool prolog, bool epilog,
                               unsigned cf_base, bool internal_kernel)
{
   /* Build the meta shader key */
   size_t total_key_size = sizeof(struct agx_generic_meta_key) + data_size;
   struct agx_generic_meta_key *key = alloca(total_key_size);

   *key = (struct agx_generic_meta_key){
      .builder = builder,
      .key_size = data_size,
   };

   if (data_size)
      memcpy(key->key, data, data_size);

   /* Try to get the cached shader */
   struct hash_entry *ent = _mesa_hash_table_search(ctx->generic_meta, key);
   if (ent)
      return ent->data;

   /* Otherwise, compile the shader fresh */
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "AGX meta shader");

   builder(&b, data);

   struct agx_device *dev = agx_device(ctx->base.screen);
   if (!prolog) {
      /* We need to link libagx and assign shared before preprocessing, matching
       * what the driver would otherwise produce.
       */
      agx_link_libagx(b.shader, dev->libagx);

      NIR_PASS(_, b.shader, nir_lower_vars_to_explicit_types,
               nir_var_mem_shared, glsl_get_cl_type_size_align);

      NIR_PASS(_, b.shader, nir_lower_explicit_io, nir_var_mem_shared,
               nir_address_format_62bit_generic);

      agx_preprocess_nir(b.shader, NULL);
      NIR_PASS(_, b.shader, agx_nir_lower_texture, true);
      NIR_PASS(_, b.shader, agx_nir_lower_multisampled_image_store);
   }

   struct agx_compiled_shader *shader = agx_compile_nir(
      dev, b.shader, NULL, PIPE_SHADER_COMPUTE, internal_kernel,
      !prolog && !(b.shader->info.stage == MESA_SHADER_FRAGMENT &&
                   b.shader->info.fs.uses_sample_shading),
      prolog || epilog, cf_base, NULL);

   ralloc_free(b.shader);

   /* ..and cache it before we return. The key is on the stack right now, so
    * clone it before using it as a hash table key. The clone is logically owned
    * by the hash table.
    */
   void *cloned_key = rzalloc_size(ctx->generic_meta, total_key_size);
   memcpy(cloned_key, key, total_key_size);

   _mesa_hash_table_insert(ctx->generic_meta, cloned_key, shader);
   return shader;
}

struct agx_compiled_shader *
agx_build_meta_shader(struct agx_context *ctx, meta_shader_builder_t builder,
                      void *data, size_t data_size)
{
   return agx_build_meta_shader_internal(ctx, builder, data, data_size, false,
                                         false, 0, false);
}

static unsigned
sampler_count(struct agx_context *ctx, enum pipe_shader_type stage)
{
   /* We reserve sampler #0 for txf so add 1 to the API count */
   return ctx->stage[stage].sampler_count + 1;
}

static inline enum agx_sampler_states
translate_sampler_state_count(struct agx_context *ctx,
                              struct agx_compiled_shader *cs,
                              enum pipe_shader_type stage)
{
   /* Clamp to binding table maximum, anything larger will be bindless */
   return agx_translate_sampler_state_count(MIN2(sampler_count(ctx, stage), 16),
                                            ctx->stage[stage].custom_borders);
}

static uint32_t
agx_nr_tex_descriptors_without_spilled_rts(const struct agx_compiled_shader *cs)
{
   if (!cs || !cs->so)
      return 0;

   /* 2 descriptors per image, 1 descriptor per texture */
   return cs->so->info.nr_bindful_textures +
          (2 * cs->so->info.nr_bindful_images);
}

static uint32_t
agx_nr_tex_descriptors(struct agx_batch *batch, struct agx_compiled_shader *cs)
{
   unsigned n = agx_nr_tex_descriptors_without_spilled_rts(cs);

   /* We add on texture/PBE descriptors for spilled render targets */
   bool spilled_rt = cs->stage == PIPE_SHADER_FRAGMENT &&
                     agx_tilebuffer_spills(&batch->tilebuffer_layout);
   if (spilled_rt)
      n += (batch->key.nr_cbufs * 2);

   return n;
}

/*
 * For spilled render targets, upload a texture/PBE pair for each surface to
 * allow loading/storing to the render target from the shader.
 */
static void
agx_upload_spilled_rt_descriptors(struct agx_texture_packed *out,
                                  struct agx_batch *batch)
{
   for (unsigned rt = 0; rt < batch->key.nr_cbufs; ++rt) {
      struct agx_texture_packed *texture = out + (2 * rt);
      struct agx_pbe_packed *pbe = (struct agx_pbe_packed *)(texture + 1);

      struct pipe_surface *surf = batch->key.cbufs[rt];
      if (!surf)
         continue;

      struct agx_resource *rsrc = agx_resource(surf->texture);
      struct pipe_image_view view = image_view_for_surface(surf);
      struct pipe_sampler_view sampler_view = sampler_view_for_surface(surf);
      sampler_view.target = PIPE_TEXTURE_2D_ARRAY;

      agx_pack_texture(texture, rsrc, surf->format, &sampler_view);
      agx_batch_upload_pbe(batch, pbe, &view, false, false, true, true);
   }
}

static void
agx_upload_textures(struct agx_batch *batch, struct agx_compiled_shader *cs,
                    enum pipe_shader_type stage)
{
   struct agx_context *ctx = batch->ctx;

   /* This can occur for meta shaders */
   if (!cs->so) {
      batch->texture_count[stage] = 0;
      batch->stage_uniforms[stage].texture_base = 0;
      return;
   }

   unsigned nr_textures = cs->so->info.nr_bindful_textures;

   unsigned nr_active_textures = ctx->stage[stage].texture_count;
   unsigned nr_tex_descriptors = agx_nr_tex_descriptors(batch, cs);
   unsigned nr_images = cs->so->info.nr_bindful_images;

   struct agx_ptr T_tex = agx_pool_alloc_aligned(
      &batch->pool, AGX_TEXTURE_LENGTH * nr_tex_descriptors, 64);

   struct agx_texture_packed *textures = T_tex.cpu;

   for (unsigned i = 0; i < MIN2(nr_textures, nr_active_textures); ++i) {
      struct agx_sampler_view *tex = ctx->stage[stage].textures[i];

      if (tex == NULL) {
         agx_set_null_texture(&textures[i], T_tex.gpu);
         continue;
      }

      struct agx_resource *rsrc = tex->rsrc;
      agx_batch_reads(batch, tex->rsrc);

      /* Re-emit state because the layout might have changed from under us.
       * TODO: optimize this somehow?
       */
      agx_pack_texture(&tex->desc, rsrc, tex->format, &tex->base);

      textures[i] = tex->desc;
   }

   for (unsigned i = nr_active_textures; i < nr_textures; ++i)
      agx_set_null_texture(&textures[i], T_tex.gpu);

   for (unsigned i = 0; i < nr_images; ++i) {
      /* Image descriptors come in pairs after the textures */
      struct agx_texture_packed *texture =
         ((struct agx_texture_packed *)T_tex.cpu) + nr_textures + (2 * i);

      struct agx_pbe_packed *pbe = (struct agx_pbe_packed *)(texture + 1);

      if (!(ctx->stage[stage].image_mask & BITFIELD_BIT(i))) {
         agx_set_null_texture(texture, T_tex.gpu);
         agx_set_null_pbe(pbe, agx_pool_alloc_aligned(&batch->pool, 1, 64).gpu);
         continue;
      }

      struct pipe_image_view *view = &ctx->stage[stage].images[i];
      agx_batch_track_image(batch, view);

      struct pipe_sampler_view sampler_view = util_image_to_sampler_view(view);

      /* For the texture descriptor, lower cubes to 2D arrays. This matches the
       * transform done in the compiler. Also, force 2D arrays for internal
       * blitter images, this helps reduce shader variants.
       */
      bool internal = (view->access & PIPE_IMAGE_ACCESS_DRIVER_INTERNAL);

      if (target_is_cube(sampler_view.target) ||
          (sampler_view.target == PIPE_TEXTURE_3D && internal))
         sampler_view.target = PIPE_TEXTURE_2D_ARRAY;

      agx_pack_texture(texture, agx_resource(view->resource), view->format,
                       &sampler_view);
      agx_batch_upload_pbe(batch, pbe, view, false, false, false, false);
   }

   if (stage == PIPE_SHADER_FRAGMENT &&
       agx_tilebuffer_spills(&batch->tilebuffer_layout)) {

      struct agx_texture_packed *out =
         ((struct agx_texture_packed *)T_tex.cpu) +
         agx_nr_tex_descriptors_without_spilled_rts(cs);

      agx_upload_spilled_rt_descriptors(out, batch);
   }

   batch->texture_count[stage] = nr_tex_descriptors;
   batch->stage_uniforms[stage].texture_base = T_tex.gpu;
}

uint16_t
agx_sampler_heap_add(struct agx_device *dev, struct agx_sampler_heap *heap,
                     struct agx_sampler_packed *sampler)
{
   /* Allocate (maximally sized) BO if we haven't already */
   if (!heap->bo) {
      heap->bo = agx_bo_create(dev, AGX_SAMPLER_HEAP_SIZE * AGX_SAMPLER_LENGTH,
                               0, AGX_BO_WRITEBACK, "Sampler heap");

      assert(heap->count == 0);
   }

   /* TODO search */

   /* Precondition: there is room in the heap */
   assert(heap->count < AGX_SAMPLER_HEAP_SIZE);
   struct agx_sampler_packed *samplers = heap->bo->map;
   memcpy(samplers + heap->count, sampler, sizeof(*sampler));

   return heap->count++;
}

static void
agx_upload_samplers(struct agx_batch *batch, struct agx_compiled_shader *cs,
                    enum pipe_shader_type stage)
{
   struct agx_context *ctx = batch->ctx;

   unsigned nr_samplers = sampler_count(ctx, stage);
   bool custom_borders = ctx->stage[stage].custom_borders;

   size_t sampler_length =
      AGX_SAMPLER_LENGTH + (custom_borders ? AGX_BORDER_LENGTH : 0);

   struct agx_ptr T =
      agx_pool_alloc_aligned(&batch->pool, sampler_length * nr_samplers, 64);

   /* Sampler #0 is reserved for txf */
   agx_pack_txf_sampler(T.cpu);

   /* Remaining samplers are API samplers */
   uint8_t *out_sampler = (uint8_t *)T.cpu + sampler_length;
   for (unsigned i = 0; i < ctx->stage[stage].sampler_count; ++i) {
      struct agx_sampler_state *sampler = ctx->stage[stage].samplers[i];
      struct agx_sampler_packed *out = (struct agx_sampler_packed *)out_sampler;

      if (sampler) {
         *out = sampler->desc;

         if (custom_borders) {
            STATIC_ASSERT(sizeof(sampler->border) == AGX_BORDER_LENGTH);

            memcpy(out_sampler + AGX_SAMPLER_LENGTH, &sampler->border,
                   AGX_BORDER_LENGTH);
         } else {
            assert(!sampler->uses_custom_border && "invalid combination");
         }
      } else {
         memset(out, 0, sampler_length);
      }

      out_sampler += sampler_length;
   }

   batch->sampler_count[stage] = nr_samplers;
   batch->samplers[stage] = T.gpu;
}

static void
agx_update_descriptors(struct agx_batch *batch, struct agx_compiled_shader *cs)
{
   struct agx_context *ctx = batch->ctx;
   if (!cs)
      return;

   enum pipe_shader_type stage = cs->stage;
   if (!ctx->stage[stage].dirty)
      return;

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_CONST)
      agx_set_cbuf_uniforms(batch, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_SSBO)
      agx_set_ssbo_uniforms(batch, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_IMAGE)
      agx_upload_textures(batch, cs, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_SAMPLER)
      agx_set_sampler_uniforms(batch, stage);

   if (ctx->stage[stage].dirty & AGX_STAGE_DIRTY_SAMPLER)
      agx_upload_samplers(batch, cs, stage);

   struct agx_stage_uniforms *unif = &batch->stage_uniforms[stage];

   batch->uniforms.tables[AGX_SYSVAL_STAGE(stage)] =
      agx_pool_upload_aligned(&batch->pool, unif, sizeof(*unif), 16);
}

static void
agx_usc_immediates(struct agx_usc_builder *b, struct agx_batch *batch,
                   struct agx_compiled_shader *cs)
{
   unsigned constant_push_ranges =
      DIV_ROUND_UP(cs->b.info.immediate_size_16, 64);

   if (cs->b.info.immediate_size_16) {
      /* XXX: do ahead of time */
      uint64_t ptr =
         agx_pool_upload_aligned(&batch->pool, cs->b.info.immediates,
                                 cs->b.info.immediate_size_16 * 2, 64);

      for (unsigned range = 0; range < constant_push_ranges; ++range) {
         unsigned offset = 64 * range;
         assert(offset < cs->b.info.immediate_size_16);

         agx_usc_uniform(b, cs->b.info.immediate_base_uniform + offset,
                         MIN2(64, cs->b.info.immediate_size_16 - offset),
                         ptr + (offset * 2));
      }
   }
}

static uint32_t
agx_build_pipeline(struct agx_batch *batch, struct agx_compiled_shader *cs,
                   struct agx_linked_shader *linked,
                   enum pipe_shader_type phys_stage,
                   unsigned variable_shared_mem, size_t max_subgroups)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   unsigned constant_push_ranges =
      DIV_ROUND_UP(cs->b.info.immediate_size_16, 64);

   size_t usc_size =
      agx_usc_size(constant_push_ranges + cs->push_range_count + 2);

   struct agx_ptr t =
      agx_pool_alloc_aligned(&batch->pipeline_pool, usc_size, 64);

   struct agx_usc_builder b = agx_usc_builder(t.cpu, usc_size);

   enum pipe_shader_type stage = cs->stage;

   if (batch->texture_count[stage]) {
      agx_usc_pack(&b, TEXTURE, cfg) {
         cfg.start = 0;
         cfg.count =
            MIN2(batch->texture_count[stage], AGX_NUM_TEXTURE_STATE_REGS);
         cfg.buffer = batch->stage_uniforms[stage].texture_base;
      }
   }

   if (batch->sampler_count[stage]) {
      agx_usc_pack(&b, SAMPLER, cfg) {
         cfg.start = 0;
         cfg.count = batch->sampler_count[stage];
         cfg.buffer = batch->samplers[stage];
      }
   }

   for (unsigned i = 0; i < cs->push_range_count; ++i) {
      unsigned table = cs->push[i].table;
      uint64_t table_ptr = batch->uniforms.tables[table];

      /* Params may be omitted if the VS prolog does not read them, but the
       * reservation is always there in the API shader just in case.
       */
      if (table == AGX_SYSVAL_TABLE_PARAMS && !table_ptr)
         continue;

      assert(table_ptr);

      agx_usc_uniform(&b, cs->push[i].uniform, cs->push[i].length,
                      table_ptr + cs->push[i].offset);
   }

   agx_usc_immediates(&b, batch, cs);

   uint32_t max_scratch_size =
      MAX2(cs->b.info.scratch_size, cs->b.info.preamble_scratch_size);

   if (max_scratch_size > 0) {
      unsigned preamble_size = (cs->b.info.preamble_scratch_size > 0) ? 1 : 0;

      switch (phys_stage) {
      case PIPE_SHADER_FRAGMENT:
         agx_scratch_alloc(&ctx->scratch_fs, max_scratch_size, max_subgroups);
         batch->fs_scratch = true;
         batch->fs_preamble_scratch =
            MAX2(batch->fs_preamble_scratch, preamble_size);
         break;
      case PIPE_SHADER_VERTEX:
         agx_scratch_alloc(&ctx->scratch_vs, max_scratch_size, max_subgroups);
         batch->vs_scratch = true;
         batch->vs_preamble_scratch =
            MAX2(batch->vs_preamble_scratch, preamble_size);
         break;
      default:
         agx_scratch_alloc(&ctx->scratch_cs, max_scratch_size, max_subgroups);
         batch->cs_scratch = true;
         batch->cs_preamble_scratch =
            MAX2(batch->cs_preamble_scratch, preamble_size);
         break;
      }
   }

   if (stage == PIPE_SHADER_FRAGMENT) {
      agx_usc_push_packed(&b, SHARED, &batch->tilebuffer_layout.usc);
   } else {
      agx_usc_shared_non_fragment(&b, &cs->b.info, variable_shared_mem);
   }

   if (linked) {
      agx_usc_push_packed(&b, SHADER, linked->shader);
      agx_usc_push_packed(&b, REGISTERS, linked->regs);

      if (stage == PIPE_SHADER_FRAGMENT)
         agx_usc_push_packed(&b, FRAGMENT_PROPERTIES, linked->fragment_props);
   } else {
      agx_usc_pack(&b, SHADER, cfg) {
         cfg.code =
            agx_usc_addr(dev, cs->bo->va->addr + cs->b.info.main_offset);
         cfg.unk_2 = 3;
      }

      agx_usc_pack(&b, REGISTERS, cfg) {
         cfg.register_count = cs->b.info.nr_gprs;
         cfg.spill_size = cs->b.info.scratch_size
                             ? agx_scratch_get_bucket(cs->b.info.scratch_size)
                             : 0;
      }
   }

   if (cs->b.info.has_preamble) {
      agx_usc_pack(&b, PRESHADER, cfg) {
         cfg.code =
            agx_usc_addr(dev, cs->bo->va->addr + cs->b.info.preamble_offset);
      }
   } else {
      agx_usc_pack(&b, NO_PRESHADER, cfg)
         ;
   }

   return agx_usc_addr(dev, t.gpu);
}

static uint32_t
agx_build_internal_usc(struct agx_batch *batch, struct agx_compiled_shader *cs,
                       uint64_t data)
{
   struct agx_device *dev = agx_device(batch->ctx->base.screen);
   bool needs_sampler = cs->b.info.uses_txf;
   size_t usc_size = agx_usc_size(12 + (needs_sampler ? 1 : 0));

   struct agx_ptr t =
      agx_pool_alloc_aligned(&batch->pipeline_pool, usc_size, 64);

   struct agx_usc_builder b = agx_usc_builder(t.cpu, usc_size);

   agx_usc_uniform(&b, 0, 4, agx_pool_upload(&batch->pool, &data, 8));
   agx_usc_immediates(&b, batch, cs);

   if (needs_sampler) {
      /* TODO: deduplicate */
      struct agx_ptr t = agx_pool_alloc_aligned(
         &batch->pool, sizeof(struct agx_sampler_packed), 64);

      agx_pack_txf_sampler((struct agx_sampler_packed *)t.cpu);

      agx_usc_pack(&b, SAMPLER, cfg) {
         cfg.start = 0;
         cfg.count = 1;
         cfg.buffer = t.gpu;
      }
   }

   assert(cs->b.info.scratch_size == 0 && "internal kernels don't spill");
   assert(cs->b.info.preamble_scratch_size == 0 && "internal doesn't spill");

   unsigned local_size = cs->b.info.local_size;

   agx_usc_pack(&b, SHARED, cfg) {
      cfg.layout = AGX_SHARED_LAYOUT_VERTEX_COMPUTE;
      cfg.bytes_per_threadgroup = local_size > 0 ? local_size : 65536;
      cfg.uses_shared_memory = local_size > 0;
   }

   agx_usc_pack(&b, SHADER, cfg) {
      cfg.code = agx_usc_addr(dev, cs->bo->va->addr + cs->b.info.main_offset);
      cfg.unk_2 = 3;
   }

   agx_usc_pack(&b, REGISTERS, cfg) {
      cfg.register_count = cs->b.info.nr_gprs;
      cfg.spill_size = 0;
   }

   if (cs->b.info.has_preamble) {
      agx_usc_pack(&b, PRESHADER, cfg) {
         cfg.code =
            agx_usc_addr(dev, cs->bo->va->addr + cs->b.info.preamble_offset);
      }
   } else {
      agx_usc_pack(&b, NO_PRESHADER, cfg)
         ;
   }

   return agx_usc_addr(dev, t.gpu);
}

static void
agx_launch_with_uploaded_data(struct agx_batch *batch,
                              const struct agx_grid *grid,
                              meta_shader_builder_t builder, void *key,
                              size_t key_size, uint64_t data)
{
   struct agx_compiled_shader *cs = agx_build_meta_shader_internal(
      batch->ctx, builder, key, key_size, false, false, 0, true);

   uint32_t usc = agx_build_internal_usc(batch, cs, data);
   agx_launch_internal(batch, grid, cs, PIPE_SHADER_COMPUTE, usc);
}

void
agx_launch_with_data(struct agx_batch *batch, const struct agx_grid *grid,
                     meta_shader_builder_t builder, void *key, size_t key_size,
                     void *data, size_t data_size)
{
   uint64_t upload = agx_pool_upload_aligned(&batch->pool, data, data_size, 4);
   agx_launch_with_uploaded_data(batch, grid, builder, key, key_size, upload);
}

struct asahi_bg_eot
agx_build_bg_eot(struct agx_batch *batch, bool store, bool partial_render)
{
   struct agx_context *ctx = batch->ctx;

   /* Construct the key */
   struct agx_bg_eot_key key = {.tib = batch->tilebuffer_layout};

   bool needs_textures_for_spilled_rts =
      agx_tilebuffer_spills(&batch->tilebuffer_layout) && !partial_render &&
      !store;

   for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
      struct pipe_surface *surf = batch->key.cbufs[rt];

      if (surf == NULL)
         continue;

      if (store) {
         /* TODO: Suppress stores to discarded render targets */
         key.op[rt] = AGX_EOT_STORE;
      } else if (batch->tilebuffer_layout.spilled[rt] && partial_render) {
         /* Partial render programs exist only to store/load the tilebuffer to
          * main memory. When render targets are already spilled to main memory,
          * there's nothing to do.
          */
         key.op[rt] = AGX_BG_EOT_NONE;
      } else {
         bool valid = (batch->load & (PIPE_CLEAR_COLOR0 << rt));
         bool clear = (batch->clear & (PIPE_CLEAR_COLOR0 << rt));
         bool load = valid && !clear;

         /* Don't read back spilled render targets, they're already in memory */
         load &= !batch->tilebuffer_layout.spilled[rt];

         /* The background program used for partial renders must always load
          * whatever was stored in the mid-frame end-of-tile program.
          */
         load |= partial_render;

         key.op[rt] = load    ? AGX_BG_LOAD
                      : clear ? AGX_BG_CLEAR
                              : AGX_BG_EOT_NONE;
      }
   }

   /* Begin building the pipeline */
   size_t usc_size = agx_usc_size(3 + PIPE_MAX_COLOR_BUFS);
   struct agx_ptr t =
      agx_pool_alloc_aligned(&batch->pipeline_pool, usc_size, 64);
   struct agx_usc_builder b = agx_usc_builder(t.cpu, usc_size);

   bool needs_sampler = false;
   unsigned uniforms = 0;
   unsigned nr_tex = 0;

   for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
      if (key.op[rt] == AGX_BG_LOAD) {
         /* Each reloaded render target is textured */
         needs_sampler = true;

         /* Will be uploaded later, this would be clobbered */
         if (needs_textures_for_spilled_rts)
            continue;

         struct agx_ptr texture =
            agx_pool_alloc_aligned(&batch->pool, AGX_TEXTURE_LENGTH, 64);
         struct pipe_surface *surf = batch->key.cbufs[rt];
         assert(surf != NULL && "cannot load nonexistent attachment");

         struct agx_resource *rsrc = agx_resource(surf->texture);
         struct pipe_sampler_view sampler_view = sampler_view_for_surface(surf);

         agx_pack_texture(texture.cpu, rsrc, surf->format, &sampler_view);

         agx_usc_pack(&b, TEXTURE, cfg) {
            /* Shifted to match eMRT indexing, could be optimized */
            cfg.start = rt * 2;
            cfg.count = 1;
            cfg.buffer = texture.gpu;
         }

         nr_tex = (rt * 2) + 1;
      } else if (key.op[rt] == AGX_BG_CLEAR) {
         assert(batch->uploaded_clear_color[rt] && "set when cleared");
         agx_usc_uniform(&b, 4 + (8 * rt), 8, batch->uploaded_clear_color[rt]);
         uniforms = MAX2(uniforms, 4 + (8 * rt) + 8);
      } else if (key.op[rt] == AGX_EOT_STORE) {
         struct pipe_image_view view =
            image_view_for_surface(batch->key.cbufs[rt]);
         struct agx_ptr pbe =
            agx_pool_alloc_aligned(&batch->pool, AGX_PBE_LENGTH, 256);

         /* The tilebuffer is already in sRGB space if needed. Do not convert */
         view.format = util_format_linear(view.format);

         agx_batch_upload_pbe(batch, pbe.cpu, &view, true, true, false, false);

         agx_usc_pack(&b, TEXTURE, cfg) {
            cfg.start = rt;
            cfg.count = 1;
            cfg.buffer = pbe.gpu;
         }

         nr_tex = rt + 1;
      }
   }

   if (needs_textures_for_spilled_rts) {
      /* Upload texture/PBE descriptors for each render target so we can clear
       * spilled render targets.
       */
      struct agx_ptr descs = agx_pool_alloc_aligned(
         &batch->pool, AGX_TEXTURE_LENGTH * 2 * batch->key.nr_cbufs, 64);
      agx_upload_spilled_rt_descriptors(descs.cpu, batch);

      agx_usc_pack(&b, TEXTURE, cfg) {
         cfg.start = 0;
         cfg.count = 2 * batch->key.nr_cbufs;
         cfg.buffer = descs.gpu;
      }

      nr_tex = MAX2(nr_tex, 2 * batch->key.nr_cbufs);

      /* Bind the base as u0_u1 for bindless access */
      agx_usc_uniform(&b, 0, 4,
                      agx_pool_upload_aligned(&batch->pool, &descs.gpu, 8, 8));
      uniforms = MAX2(uniforms, 4);
   }

   /* All render targets share a sampler */
   if (needs_sampler) {
      struct agx_ptr sampler =
         agx_pool_alloc_aligned(&batch->pool, AGX_SAMPLER_LENGTH, 64);

      agx_pack(sampler.cpu, SAMPLER, cfg) {
         cfg.magnify = AGX_FILTER_LINEAR;
         cfg.minify = AGX_FILTER_NEAREST;
         cfg.mip_filter = AGX_MIP_FILTER_NONE;
         cfg.wrap_s = AGX_WRAP_CLAMP_TO_EDGE;
         cfg.wrap_t = AGX_WRAP_CLAMP_TO_EDGE;
         cfg.wrap_r = AGX_WRAP_CLAMP_TO_EDGE;
         cfg.pixel_coordinates = true;
         cfg.compare_func = AGX_COMPARE_FUNC_ALWAYS;
      }

      agx_usc_pack(&b, SAMPLER, cfg) {
         cfg.start = 0;
         cfg.count = 1;
         cfg.buffer = sampler.gpu;
      }
   }

   agx_usc_push_packed(&b, SHARED, &batch->tilebuffer_layout.usc);

   /* Get the shader */
   key.reserved_preamble = uniforms;
   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_bg_eot_shader *shader = agx_get_bg_eot_shader(&ctx->bg_eot, &key);
   agx_batch_add_bo(batch, shader->bo);

   agx_usc_pack(&b, SHADER, cfg) {
      cfg.code = agx_usc_addr(dev, shader->ptr);
      cfg.unk_2 = 0;
   }

   agx_usc_pack(&b, REGISTERS, cfg)
      cfg.register_count = shader->info.nr_gprs;

   if (shader->info.has_preamble) {
      agx_usc_pack(&b, PRESHADER, cfg) {
         cfg.code =
            agx_usc_addr(dev, shader->ptr + shader->info.preamble_offset);
      }
   } else {
      agx_usc_pack(&b, NO_PRESHADER, cfg)
         ;
   }

   struct asahi_bg_eot ret = {.usc = t.gpu};

   agx_pack(&ret.counts, COUNTS, cfg) {
      cfg.uniform_register_count = shader->info.push_count;
      cfg.preshader_register_count = shader->info.nr_preamble_gprs;
      cfg.texture_state_register_count = nr_tex;
      cfg.sampler_state_register_count =
         agx_translate_sampler_state_count(needs_sampler ? 1 : 0, false);

      if (!store)
         cfg.unknown = 0xFFFF;
   }

   return ret;
}

/*
 * Return the standard sample positions, packed into a 32-bit word with fixed
 * point nibbles for each x/y component of the (at most 4) samples. This is
 * suitable for programming the PPP_MULTISAMPLECTL control register.
 */
static uint32_t
agx_default_sample_positions(unsigned nr_samples)
{
   switch (nr_samples) {
   case 1:
      return 0x88;
   case 2:
      return 0x44cc;
   case 4:
      return 0xeaa26e26;
   default:
      unreachable("Invalid sample count");
   }
}

void
agx_batch_init_state(struct agx_batch *batch)
{
   if (batch->initialized)
      return;

   if (agx_batch_is_compute(batch)) {
      batch->initialized = true;

      struct agx_context *ctx = batch->ctx;
      struct agx_device *dev = agx_device(ctx->base.screen);
      uint8_t *out = batch->cdm.current;

      /* See below */
      agx_push(out, CDM_BARRIER, cfg) {
         cfg.usc_cache_inval = true;
         cfg.unk_5 = true;
         cfg.unk_6 = true;
         cfg.unk_8 = true;
         // cfg.unk_11 = true;
         // cfg.unk_20 = true;
         if (dev->params.num_clusters_total > 1) {
            // cfg.unk_24 = true;
            if (dev->params.gpu_generation == 13) {
               cfg.unk_4 = true;
               // cfg.unk_26 = true;
            }
         }
      }

      return;
   }

   /* Emit state on the batch that we don't change and so don't dirty track */
   uint8_t *out = batch->vdm.current;

   /* Barrier to enforce GPU-CPU coherency, in case this batch is back to back
    * with another that caused stale data to be cached and the CPU wrote to it
    * in the meantime.
    */
   agx_push(out, VDM_BARRIER, cfg) {
      cfg.usc_cache_inval = true;
   }

   struct AGX_PPP_HEADER present = {
      .w_clamp = true,
      .occlusion_query_2 = true,
      .output_unknown = true,
      .varying_word_2 = true,
      .viewport_count = 1, /* irrelevant */
   };

   size_t size = agx_ppp_update_size(&present);
   struct agx_ptr T = agx_pool_alloc_aligned(&batch->pool, size, 64);
   struct agx_ppp_update ppp = agx_new_ppp_update(T, size, &present);

   /* clang-format off */
   agx_ppp_push(&ppp, W_CLAMP, cfg) cfg.w_clamp = 1e-10;
   agx_ppp_push(&ppp, FRAGMENT_OCCLUSION_QUERY_2, cfg);
   agx_ppp_push(&ppp, OUTPUT_UNKNOWN, cfg);
   agx_ppp_push(&ppp, VARYING_2, cfg);
   /* clang-format on */

   agx_ppp_fini(&out, &ppp);
   batch->vdm.current = out;

   /* Mark it as initialized now, since agx_batch_writes() will check this. */
   batch->initialized = true;

   /* Choose a tilebuffer layout given the framebuffer key */
   enum pipe_format formats[PIPE_MAX_COLOR_BUFS] = {0};
   for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
      struct pipe_surface *surf = batch->key.cbufs[i];
      if (surf)
         formats[i] = surf->format;
   }

   batch->tilebuffer_layout = agx_build_tilebuffer_layout(
      formats, batch->key.nr_cbufs,
      util_framebuffer_get_num_samples(&batch->key),
      util_framebuffer_get_num_layers(&batch->key) > 1);

   if (agx_device(batch->ctx->base.screen)->debug & AGX_DBG_SMALLTILE)
      batch->tilebuffer_layout.tile_size = (struct agx_tile_size){16, 16};

   /* If the layout spilled render targets, we need to decompress those render
    * targets to ensure we can write to them.
    */
   if (agx_tilebuffer_spills(&batch->tilebuffer_layout)) {
      for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
         if (!batch->tilebuffer_layout.spilled[i])
            continue;

         struct pipe_surface *surf = batch->key.cbufs[i];
         if (!surf)
            continue;

         struct agx_resource *rsrc = agx_resource(surf->texture);
         struct ail_layout *layout = &rsrc->layout;
         unsigned level = surf->u.tex.level;

         if (!ail_is_level_compressed(layout, level))
            continue;

         if (true || (rsrc->base.bind & PIPE_BIND_SHARED)) {
            struct agx_context *ctx = batch->ctx;
            struct agx_device *dev = agx_device(ctx->base.screen);

            perf_debug(dev, "Decompressing in-place");

            if (!batch->cdm.bo)
               batch->cdm = agx_encoder_allocate(batch, dev);

            struct agx_ptr data = agx_pool_alloc_aligned(
               &batch->pool, sizeof(struct libagx_decompress_push), 64);
            struct libagx_decompress_push *push = data.cpu;
            agx_fill_decompress_push(push, layout, surf->u.tex.first_layer,
                                     level, agx_map_texture_gpu(rsrc, 0));

            struct pipe_sampler_view sampler_view =
               sampler_view_for_surface(surf);
            sampler_view.target = PIPE_TEXTURE_2D_ARRAY;
            struct pipe_image_view view = image_view_for_surface(surf);
            agx_pack_texture(&push->compressed, rsrc, surf->format,
                             &sampler_view);
            agx_batch_upload_pbe(batch, &push->uncompressed, &view, false, true,
                                 true, true);

            struct agx_grid grid = agx_grid_direct(
               ail_metadata_width_tl(layout, level) * 32,
               ail_metadata_height_tl(layout, level),
               surf->u.tex.last_layer - surf->u.tex.first_layer + 1, 32, 1, 1);

            struct agx_decompress_key key = {
               .nr_samples = layout->sample_count_sa,
            };

            agx_launch_with_uploaded_data(batch, &grid, agx_nir_decompress,
                                          &key, sizeof(key), data.gpu);
         } else {
            agx_decompress(batch->ctx, rsrc, "Render target spilled");
         }
      }
   }

   if (batch->key.zsbuf) {
      unsigned level = batch->key.zsbuf->u.tex.level;
      struct agx_resource *rsrc = agx_resource(batch->key.zsbuf->texture);

      agx_batch_writes(batch, rsrc, level);

      if (rsrc->separate_stencil)
         agx_batch_writes(batch, rsrc->separate_stencil, level);
   }

   for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
      if (batch->key.cbufs[i]) {
         struct agx_resource *rsrc = agx_resource(batch->key.cbufs[i]->texture);
         unsigned level = batch->key.cbufs[i]->u.tex.level;

         if (agx_resource_valid(rsrc, level))
            batch->load |= PIPE_CLEAR_COLOR0 << i;

         agx_batch_writes(batch, rsrc, batch->key.cbufs[i]->u.tex.level);
      }
   }

   /* Set up standard sample positions */
   batch->uniforms.ppp_multisamplectl =
      agx_default_sample_positions(batch->tilebuffer_layout.nr_samples);
}

static enum agx_object_type
agx_point_object_type(struct agx_rasterizer *rast)
{
   return (rast->base.sprite_coord_mode == PIPE_SPRITE_COORD_UPPER_LEFT)
             ? AGX_OBJECT_TYPE_POINT_SPRITE_UV01
             : AGX_OBJECT_TYPE_POINT_SPRITE_UV10;
}

#define MAX_PPP_UPDATES 2
#define IS_DIRTY(ST)    !!(ctx->dirty & AGX_DIRTY_##ST)

static uint8_t *
agx_encode_state(struct agx_batch *batch, uint8_t *out)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   /* If nothing is dirty, encode nothing */
   if (!ctx->dirty)
      return out;

   struct agx_rasterizer *rast = ctx->rast;
   unsigned ppp_updates = 0;

   struct agx_compiled_shader *vs = ctx->vs;
   if (ctx->gs)
      vs = ctx->gs->gs_copy;

   bool varyings_dirty = false;

   if (IS_DIRTY(VS_PROG) || IS_DIRTY(FS_PROG) || IS_DIRTY(RS) ||
       IS_DIRTY(PRIM)) {

      unsigned bindings = ctx->linked.fs->cf.nr_bindings;
      if (bindings) {
         size_t linkage_size =
            AGX_CF_BINDING_HEADER_LENGTH + (bindings * AGX_CF_BINDING_LENGTH);

         struct agx_ptr t =
            agx_pool_alloc_aligned(&batch->pipeline_pool, linkage_size, 16);

         agx_link_varyings_vs_fs(t.cpu, &batch->linked_varyings,
                                 vs->uvs.user_size, &ctx->linked.fs->cf,
                                 ctx->rast->base.flatshade_first ? 0 : 2,
                                 (batch->reduced_prim == MESA_PRIM_POINTS)
                                    ? ctx->rast->base.sprite_coord_enable
                                    : 0,
                                 &batch->generate_primitive_id);

         batch->varyings = agx_usc_addr(dev, t.gpu);
      } else {
         batch->varyings = 0;
      }

      varyings_dirty = true;
      ppp_updates++;
   }

   if (IS_DIRTY(VS) || varyings_dirty) {
      agx_push(out, VDM_STATE, cfg) {
         cfg.vertex_shader_word_0_present = true;
         cfg.vertex_shader_word_1_present = true;
         cfg.vertex_outputs_present = true;
         cfg.vertex_unknown_present = true;
      }

      agx_push(out, VDM_STATE_VERTEX_SHADER_WORD_0, cfg) {
         cfg.uniform_register_count = vs->b.info.push_count;
         cfg.preshader_register_count = vs->b.info.nr_preamble_gprs;
         cfg.texture_state_register_count = agx_nr_tex_descriptors(batch, vs);
         cfg.sampler_state_register_count =
            translate_sampler_state_count(ctx, vs, vs->stage);
      }

      agx_push(out, VDM_STATE_VERTEX_SHADER_WORD_1, cfg) {
         cfg.pipeline =
            agx_build_pipeline(batch, vs, ctx->gs ? NULL : ctx->linked.vs,
                               PIPE_SHADER_VERTEX, 0, 0);
      }

      agx_push_packed(out, vs->uvs.vdm, VDM_STATE_VERTEX_OUTPUTS);

      agx_push(out, VDM_STATE_VERTEX_UNKNOWN, cfg) {
         cfg.flat_shading_control = ctx->rast->base.flatshade_first
                                       ? AGX_VDM_VERTEX_0
                                       : AGX_VDM_VERTEX_2;
         cfg.unknown_4 = cfg.unknown_5 = ctx->rast->base.rasterizer_discard;

         cfg.generate_primitive_id = batch->generate_primitive_id;
      }

      /* Pad up to a multiple of 8 bytes */
      memset(out, 0, 4);
      out += 4;
   }

   struct agx_pool *pool = &batch->pool;

   if ((ctx->dirty & AGX_DIRTY_RS) && ctx->rast->depth_bias) {
      agx_upload_depth_bias(batch, &ctx->rast->base);
      ctx->dirty |= AGX_DIRTY_SCISSOR_ZBIAS;
   }

   if (ctx->dirty & (AGX_DIRTY_VIEWPORT | AGX_DIRTY_SCISSOR_ZBIAS |
                     AGX_DIRTY_RS | AGX_DIRTY_VS)) {

      agx_upload_viewport_scissor(pool, batch, &out, ctx->viewport,
                                  ctx->rast->base.scissor ? ctx->scissor : NULL,
                                  ctx->rast->base.clip_halfz,
                                  vs->b.info.nonzero_viewport);
   }

   bool is_points = batch->reduced_prim == MESA_PRIM_POINTS;
   bool is_lines = batch->reduced_prim == MESA_PRIM_LINES;

   bool object_type_dirty =
      IS_DIRTY(PRIM) || (is_points && IS_DIRTY(SPRITE_COORD_MODE));

   bool fragment_face_dirty =
      IS_DIRTY(ZS) || IS_DIRTY(STENCIL_REF) || IS_DIRTY(RS);

   enum agx_object_type object_type = is_points  ? agx_point_object_type(rast)
                                      : is_lines ? AGX_OBJECT_TYPE_LINE
                                                 : AGX_OBJECT_TYPE_TRIANGLE;

   struct AGX_PPP_HEADER dirty = {
      .fragment_control =
         IS_DIRTY(ZS) || IS_DIRTY(RS) || IS_DIRTY(PRIM) || IS_DIRTY(QUERY),
      .fragment_control_2 = IS_DIRTY(FS_PROG) || IS_DIRTY(RS),
      .fragment_front_face = fragment_face_dirty,
      .fragment_front_face_2 = object_type_dirty || IS_DIRTY(FS_PROG),
      .fragment_front_stencil = IS_DIRTY(ZS),
      .fragment_back_face = fragment_face_dirty,
      .fragment_back_face_2 = object_type_dirty || IS_DIRTY(FS_PROG),
      .fragment_back_stencil = IS_DIRTY(ZS),
      .output_select = varyings_dirty,
      .varying_counts_32 = varyings_dirty,
      .varying_counts_16 = varyings_dirty,
      .cull = IS_DIRTY(RS),
      .cull_2 = varyings_dirty,
      .fragment_shader =
         IS_DIRTY(FS) || varyings_dirty || IS_DIRTY(SAMPLE_MASK),
      .occlusion_query = IS_DIRTY(QUERY),
      .output_size = IS_DIRTY(VS_PROG),
      .viewport_count = 1, /* irrelevant */
   };

   size_t size = agx_ppp_update_size(&dirty);
   struct agx_ptr T = agx_pool_alloc_aligned(&batch->pool, size, 64);
   struct agx_ppp_update ppp = agx_new_ppp_update(T, size, &dirty);

   if (dirty.fragment_control) {
      agx_ppp_push(&ppp, FRAGMENT_CONTROL, cfg) {
         if (ctx->active_queries && ctx->occlusion_query) {
            if (ctx->occlusion_query->type == PIPE_QUERY_OCCLUSION_COUNTER)
               cfg.visibility_mode = AGX_VISIBILITY_MODE_COUNTING;
            else
               cfg.visibility_mode = AGX_VISIBILITY_MODE_BOOLEAN;
         }

         cfg.stencil_test_enable = ctx->zs->base.stencil[0].enabled;
         cfg.two_sided_stencil = ctx->zs->base.stencil[1].enabled;
         cfg.depth_bias_enable =
            rast->depth_bias && object_type == AGX_OBJECT_TYPE_TRIANGLE;

         /* Always enable scissoring so we may scissor to the viewport (TODO:
          * optimize this out if the viewport is the default and the app does
          * not use the scissor test)
          */
         cfg.scissor_enable = true;

         /* This avoids broken derivatives along primitive edges */
         cfg.disable_tri_merging = is_lines || is_points;
      }
   }

   if (dirty.fragment_control_2) {
      /* Annoying, rasterizer_discard seems to be ignored (sometimes?) in the
       * main fragment control word and has to be combined into the secondary
       * word for reliable behaviour.
       */
      agx_ppp_push_merged(&ppp, FRAGMENT_CONTROL, cfg,
                          ctx->linked.fs->fragment_control) {
         cfg.tag_write_disable = rast->base.rasterizer_discard;
      }
   }

   if (dirty.fragment_front_face) {
      agx_ppp_push_merged(&ppp, FRAGMENT_FACE, cfg, ctx->zs->depth) {
         cfg.stencil_reference = ctx->stencil_ref.ref_value[0];
         cfg.line_width = rast->line_width;
         cfg.polygon_mode = rast->polygon_mode;
      }
   }

   if (dirty.fragment_front_face_2)
      agx_ppp_fragment_face_2(&ppp, object_type, &ctx->fs->b.info);

   if (dirty.fragment_front_stencil) {
      agx_ppp_push_packed(&ppp, ctx->zs->front_stencil.opaque,
                          FRAGMENT_STENCIL);
   }

   if (dirty.fragment_back_face) {
      agx_ppp_push_merged(&ppp, FRAGMENT_FACE, cfg, ctx->zs->depth) {
         bool twosided = ctx->zs->base.stencil[1].enabled;
         cfg.stencil_reference = ctx->stencil_ref.ref_value[twosided ? 1 : 0];
         cfg.line_width = rast->line_width;
         cfg.polygon_mode = rast->polygon_mode;
      }
   }

   if (dirty.fragment_back_face_2)
      agx_ppp_fragment_face_2(&ppp, object_type, &ctx->fs->b.info);

   if (dirty.fragment_back_stencil)
      agx_ppp_push_packed(&ppp, ctx->zs->back_stencil.opaque, FRAGMENT_STENCIL);

   assert(dirty.varying_counts_32 == dirty.varying_counts_16);
   assert(dirty.varying_counts_32 == dirty.output_select);

   if (dirty.output_select) {
      agx_ppp_push_merged_blobs(&ppp, AGX_OUTPUT_SELECT_LENGTH, &vs->uvs.osel,
                                &ctx->linked.fs->osel);

      agx_ppp_push_packed(&ppp, &batch->linked_varyings.counts_32,
                          VARYING_COUNTS);

      agx_ppp_push_packed(&ppp, &batch->linked_varyings.counts_16,
                          VARYING_COUNTS);
   }

   if (dirty.cull)
      agx_ppp_push_packed(&ppp, ctx->rast->cull, CULL);

   if (dirty.cull_2) {
      agx_ppp_push(&ppp, CULL_2, cfg) {
         cfg.needs_primitive_id = batch->generate_primitive_id;
      }
   }

   if (dirty.fragment_shader) {
      unsigned frag_tex_count = ctx->stage[PIPE_SHADER_FRAGMENT].texture_count;

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_0, cfg) {
         cfg.uniform_register_count = ctx->fs->b.info.push_count;
         cfg.preshader_register_count = ctx->fs->b.info.nr_preamble_gprs;
         cfg.texture_state_register_count =
            agx_nr_tex_descriptors(batch, ctx->fs);
         cfg.sampler_state_register_count =
            translate_sampler_state_count(ctx, ctx->fs, PIPE_SHADER_FRAGMENT);
         cfg.cf_binding_count = ctx->linked.fs->cf.nr_bindings;
      }

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_1, cfg) {
         cfg.pipeline = agx_build_pipeline(batch, ctx->fs, ctx->linked.fs,
                                           PIPE_SHADER_FRAGMENT, 0, 0);
      }

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_2, cfg) {
         cfg.cf_bindings = batch->varyings;
      }

      agx_ppp_push(&ppp, FRAGMENT_SHADER_WORD_3, cfg) {
         /* XXX: This is wrong */
         cfg.unknown = frag_tex_count >= 4;
      }
   }

   if (dirty.occlusion_query) {
      agx_ppp_push(&ppp, FRAGMENT_OCCLUSION_QUERY, cfg) {
         if (ctx->active_queries && ctx->occlusion_query) {
            cfg.index = agx_get_oq_index(batch, ctx->occlusion_query);
         }
      }
   }

   if (dirty.output_size) {
      agx_ppp_push(&ppp, OUTPUT_SIZE, cfg)
         cfg.count = vs->uvs.size;
   }

   agx_ppp_fini(&out, &ppp);
   ppp_updates++;

   assert(ppp_updates <= MAX_PPP_UPDATES);
   return out;
}

static enum agx_primitive
agx_primitive_for_pipe(enum mesa_prim mode)
{
   switch (mode) {
   case MESA_PRIM_POINTS:
      return AGX_PRIMITIVE_POINTS;
   case MESA_PRIM_LINES:
      return AGX_PRIMITIVE_LINES;
   case MESA_PRIM_LINE_STRIP:
      return AGX_PRIMITIVE_LINE_STRIP;
   case MESA_PRIM_LINE_LOOP:
      return AGX_PRIMITIVE_LINE_LOOP;
   case MESA_PRIM_TRIANGLES:
      return AGX_PRIMITIVE_TRIANGLES;
   case MESA_PRIM_TRIANGLE_STRIP:
      return AGX_PRIMITIVE_TRIANGLE_STRIP;
   case MESA_PRIM_TRIANGLE_FAN:
      return AGX_PRIMITIVE_TRIANGLE_FAN;
   case MESA_PRIM_QUADS:
      return AGX_PRIMITIVE_QUADS;
   case MESA_PRIM_QUAD_STRIP:
      return AGX_PRIMITIVE_QUAD_STRIP;
   default:
      unreachable("todo: other primitive types");
   }
}

static uint64_t
agx_index_buffer_rsrc_ptr(struct agx_batch *batch,
                          const struct pipe_draw_info *info, size_t *extent)
{
   assert(!info->has_user_indices && "cannot use user pointers with indirect");

   struct agx_resource *rsrc = agx_resource(info->index.resource);
   agx_batch_reads(batch, rsrc);

   *extent = ALIGN_POT(rsrc->layout.size_B, 4);
   return rsrc->bo->va->addr;
}

static uint64_t
agx_index_buffer_direct_ptr(struct agx_batch *batch,
                            const struct pipe_draw_start_count_bias *draw,
                            const struct pipe_draw_info *info, size_t *extent)
{
   off_t offset = draw->start * info->index_size;
   uint32_t max_extent = draw->count * info->index_size;

   if (!info->has_user_indices) {
      uint64_t base = agx_index_buffer_rsrc_ptr(batch, info, extent);

      *extent = ALIGN_POT(MIN2(*extent - offset, max_extent), 4);
      return base + offset;
   } else {
      *extent = ALIGN_POT(max_extent, 4);

      return agx_pool_upload_aligned(&batch->pool,
                                     ((uint8_t *)info->index.user) + offset,
                                     draw->count * info->index_size, 64);
   }
}

static uint64_t
agx_index_buffer_ptr(struct agx_batch *batch, const struct pipe_draw_info *info,
                     const struct pipe_draw_start_count_bias *draw,
                     size_t *extent)
{
   if (draw)
      return agx_index_buffer_direct_ptr(batch, draw, info, extent);
   else
      return agx_index_buffer_rsrc_ptr(batch, info, extent);
}

static void
agx_ensure_cmdbuf_has_space(struct agx_batch *batch, struct agx_encoder *enc,
                            size_t space)
{
   bool vdm = enc == &batch->vdm;
   assert(vdm || (enc == &batch->cdm));

   size_t link_length =
      vdm ? AGX_VDM_STREAM_LINK_LENGTH : AGX_CDM_STREAM_LINK_LENGTH;

   /* Assert that we have space for a link tag */
   assert((enc->current + link_length) <= enc->end && "Encoder overflowed");

   /* Always leave room for a link tag, in case we run out of space later,
    * plus padding because VDM apparently overreads?
    *
    * 0x200 is not enough. 0x400 seems to work. 0x800 for safety.
    */
   space += link_length + 0x800;

   /* If there is room in the command buffer, we're done */
   if (likely((enc->end - enc->current) >= space))
      return;

   /* Otherwise, we need to allocate a new command buffer. We use memory owned
    * by the batch to simplify lifetime management for the BO.
    */
   size_t size = 65536;
   struct agx_ptr T = agx_pool_alloc_aligned(&batch->pool, size, 256);

   /* Jump from the old command buffer to the new command buffer */
   if (vdm) {
      agx_pack(enc->current, VDM_STREAM_LINK, cfg) {
         cfg.target_lo = T.gpu & BITFIELD_MASK(32);
         cfg.target_hi = T.gpu >> 32;
      }
   } else {
      agx_pack(enc->current, CDM_STREAM_LINK, cfg) {
         cfg.target_lo = T.gpu & BITFIELD_MASK(32);
         cfg.target_hi = T.gpu >> 32;
      }
   }

   /* Swap out the command buffer */
   enc->current = T.cpu;
   enc->end = enc->current + size;
}

static void
agx_ia_update(struct agx_batch *batch, const struct pipe_draw_info *info,
              uint64_t draw, uint64_t ib, uint64_t ib_range_el)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   struct agx_increment_ia_counters_key key = {
      .index_size_B = info->primitive_restart ? info->index_size : 0,
   };

   struct libagx_increment_ia_counters args = {
      .ia_vertices = agx_get_query_address(
         batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_IA_VERTICES]),

      .vs_invocations = agx_get_query_address(
         batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_VS_INVOCATIONS]),

      .restart_index = info->restart_index,
      .index_buffer = ib,
      .index_buffer_range_el = ib_range_el,
      .draw = draw,
   };

   uint64_t wg_size = key.index_size_B ? 1024 : 1;
   struct agx_grid grid = agx_grid_direct(wg_size, 1, 1, wg_size, 1, 1);

   if (!batch->cdm.bo) {
      batch->cdm = agx_encoder_allocate(batch, dev);
   }

   perf_debug(dev, "Input assembly counters");
   agx_launch_with_data(batch, &grid, agx_nir_increment_ia_counters, &key,
                        sizeof(key), &args, sizeof(args));
}

static uint64_t
agx_batch_geometry_state(struct agx_batch *batch)
{
   struct agx_context *ctx = batch->ctx;

   if (!batch->geometry_state) {
      uint32_t size = 128 * 1024 * 1024;

      if (!ctx->heap) {
         ctx->heap = pipe_buffer_create(ctx->base.screen, PIPE_BIND_GLOBAL,
                                        PIPE_USAGE_DEFAULT, size);
      }

      struct agx_geometry_state state = {
         .heap = agx_resource(ctx->heap)->bo->va->addr,
         .heap_size = size,
      };

      agx_batch_writes(batch, agx_resource(ctx->heap), 0);

      batch->geometry_state =
         agx_pool_upload_aligned(&batch->pool, &state, sizeof(state), 8);
   }

   return batch->geometry_state;
}

static uint64_t
agx_batch_geometry_params(struct agx_batch *batch, uint64_t input_index_buffer,
                          size_t index_buffer_size_B,
                          const struct pipe_draw_info *info,
                          const struct pipe_draw_start_count_bias *draw,
                          const struct pipe_draw_indirect_info *indirect)
{
   struct agx_ia_state ia = {
      .index_buffer = input_index_buffer,
      .index_buffer_range_el = index_buffer_size_B / info->index_size,
      .verts_per_instance = draw ? draw->count : 0,
   };

   batch->uniforms.input_assembly =
      agx_pool_upload_aligned(&batch->pool, &ia, sizeof(ia), 8);

   struct agx_geometry_params params = {
      .state = agx_batch_geometry_state(batch),
      .indirect_desc = batch->geom_indirect,
      .flat_outputs =
         batch->ctx->stage[PIPE_SHADER_FRAGMENT].shader->info.inputs_flat_shaded,
      .input_topology = info->mode,
   };

   for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->streamout.targets); ++i) {
      struct agx_streamout_target *so =
         agx_so_target(batch->ctx->streamout.targets[i]);
      struct agx_resource *rsrc = so ? agx_resource(so->offset) : NULL;

      uint32_t size;
      params.xfb_base_original[i] = agx_batch_get_so_address(batch, i, &size);
      params.xfb_size[i] = size;

      if (rsrc) {
         params.xfb_offs_ptrs[i] = rsrc->bo->va->addr;
         agx_batch_writes(batch, rsrc, 0);
         batch->incoherent_writes = true;
      } else {
         params.xfb_offs_ptrs[i] = 0;
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->prims_generated); ++i) {
      params.prims_generated_counter[i] =
         agx_get_query_address(batch, batch->ctx->prims_generated[i]);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->tf_prims_generated); ++i) {
      params.xfb_prims_generated_counter[i] =
         agx_get_query_address(batch, batch->ctx->tf_prims_generated[i]);
   }

   if (batch->ctx->active_queries && batch->ctx->streamout.num_targets > 0) {
      for (unsigned i = 0; i < ARRAY_SIZE(batch->ctx->tf_overflow); ++i) {
         params.xfb_overflow[i] =
            agx_get_query_address(batch, batch->ctx->tf_overflow[i]);
      }

      params.xfb_any_overflow =
         agx_get_query_address(batch, batch->ctx->tf_any_overflow);
   }

   /* Calculate input primitive count for direct draws, and allocate the vertex
    * & count buffers. GPU calculates and allocates for indirect draws.
    */
   unsigned count_buffer_stride = batch->ctx->gs->gs_count_words * 4;
   batch->uniforms.vertex_outputs = batch->ctx->vs->b.info.outputs;
   params.input_mask = batch->uniforms.vertex_outputs;

   if (indirect) {
      params.count_buffer_stride = count_buffer_stride;
      batch->uniforms.vertex_output_buffer_ptr =
         agx_pool_alloc_aligned(&batch->pool, 8, 8).gpu;

      params.vs_grid[2] = params.gs_grid[2] = 1;
   } else {
      params.vs_grid[0] = draw->count;
      params.gs_grid[0] =
         u_decomposed_prims_for_vertices(info->mode, draw->count);

      params.primitives_log2 = util_logbase2_ceil(params.gs_grid[0]);

      params.input_primitives = params.gs_grid[0] * info->instance_count;

      unsigned vb_size = libagx_tcs_in_size(draw->count * info->instance_count,
                                            batch->uniforms.vertex_outputs);
      unsigned size = params.input_primitives * count_buffer_stride;

      if (size) {
         params.count_buffer =
            agx_pool_alloc_aligned(&batch->pool, size, 4).gpu;
      }

      if (vb_size) {
         uint64_t addr = agx_pool_alloc_aligned(&batch->pool, vb_size, 4).gpu;
         batch->uniforms.vertex_output_buffer_ptr =
            agx_pool_upload(&batch->pool, &addr, 8);

         params.input_buffer = addr;
      }
   }

   return agx_pool_upload_aligned_with_bo(&batch->pool, &params, sizeof(params),
                                          8, &batch->geom_params_bo);
}

static uint64_t
agx_indirect_buffer_ptr(struct agx_batch *batch,
                        const struct pipe_draw_indirect_info *indirect)
{
   assert(indirect->buffer && "drawauto already handled");

   struct agx_resource *rsrc = agx_resource(indirect->buffer);
   agx_batch_reads(batch, rsrc);
   return rsrc->bo->va->addr + indirect->offset;
}

static void
agx_launch_gs_prerast(struct agx_batch *batch,
                      const struct pipe_draw_info *info,
                      const struct pipe_draw_start_count_bias *draws,
                      const struct pipe_draw_indirect_info *indirect)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_compiled_shader *gs = ctx->gs;

   if (ctx->stage[PIPE_SHADER_GEOMETRY].shader->is_xfb_passthrough)
      perf_debug(dev, "Transform feedbck");
   else
      perf_debug(dev, "Geometry shader");

   /* This is a graphics batch, so it may not have had a CDM encoder allocated
    * yet. Allocate that so we can start enqueueing compute work.
    */
   if (!batch->cdm.bo) {
      batch->cdm = agx_encoder_allocate(batch, dev);
   }

   agx_ensure_cmdbuf_has_space(
      batch, &batch->cdm,
      8 * (AGX_CDM_LAUNCH_WORD_0_LENGTH + AGX_CDM_LAUNCH_WORD_1_LENGTH +
           AGX_CDM_UNK_G14X_LENGTH + AGX_CDM_INDIRECT_LENGTH +
           AGX_CDM_GLOBAL_SIZE_LENGTH + AGX_CDM_LOCAL_SIZE_LENGTH +
           AGX_CDM_BARRIER_LENGTH));

   assert(!info->primitive_restart && "should have been lowered");

   struct agx_grid grid_vs, grid_gs;

   /* Setup grids */
   if (indirect) {
      struct agx_gs_setup_indirect_key key = {
         .prim = info->mode,
      };

      uint64_t ib = 0;
      size_t ib_extent = 0;

      if (info->index_size) {
         ib = agx_index_buffer_ptr(batch, info, indirect ? NULL : draws,
                                   &ib_extent);
      }

      struct agx_gs_setup_indirect_params gsi = {
         .index_buffer = ib,
         .index_buffer_range_el = ib_extent / info->index_size,
         .draw = agx_indirect_buffer_ptr(batch, indirect),
         .vertex_buffer = batch->uniforms.vertex_output_buffer_ptr,
         .ia = batch->uniforms.input_assembly,
         .geom = batch->uniforms.geometry_params,
         .vs_outputs = batch->uniforms.vertex_outputs,
         .index_size_B = info->index_size,
      };

      const struct agx_grid grid_setup = agx_grid_direct(1, 1, 1, 1, 1, 1);
      agx_launch_with_data(batch, &grid_setup, agx_nir_gs_setup_indirect, &key,
                           sizeof(key), &gsi, sizeof(gsi));

      uint64_t gp = batch->uniforms.geometry_params;

      grid_vs = agx_grid_indirect(
         gp + offsetof(struct agx_geometry_params, vs_grid), 1, 1, 1);

      grid_gs = agx_grid_indirect(
         gp + offsetof(struct agx_geometry_params, gs_grid), 1, 1, 1);
   } else {
      grid_vs =
         agx_grid_direct(draws->count, info->instance_count, 1, 64, 1, 1);

      grid_gs = agx_grid_direct(
         u_decomposed_prims_for_vertices(info->mode, draws->count),
         info->instance_count, 1, 64, 1, 1);
   }

   /* Launch the vertex shader first */
   agx_launch(batch, &grid_vs, ctx->vs, ctx->linked.vs, ctx->vs->stage, 0);

   /* If there is a count shader, launch it and prefix sum the results. */
   if (gs->gs_count) {
      perf_debug(dev, "Geometry shader count");
      agx_launch(batch, &grid_gs, gs->gs_count, NULL, PIPE_SHADER_GEOMETRY, 0);

      unsigned words = gs->gs_count_words;
      struct agx_grid grid =
         agx_grid_direct(1024 * gs->gs_count_words, 1, 1, 1024, 1, 1);

      agx_launch(batch, &grid,
                 agx_build_meta_shader(ctx, agx_nir_prefix_sum_gs, &words,
                                       sizeof(words)),
                 NULL, PIPE_SHADER_COMPUTE, 0);
   }

   /* Pre-GS shader */
   struct agx_grid grid = agx_grid_direct(1, 1, 1, 1, 1, 1);
   agx_launch(batch, &grid, gs->pre_gs, NULL, PIPE_SHADER_COMPUTE, 0);

   /* Pre-rast geometry shader */
   agx_launch(batch, &grid_gs, gs, NULL, PIPE_SHADER_GEOMETRY, 0);
}

static void
agx_draw_without_restart(struct agx_batch *batch,
                         const struct pipe_draw_info *info,
                         unsigned drawid_offset,
                         const struct pipe_draw_indirect_info *indirect,
                         const struct pipe_draw_start_count_bias *draw)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   perf_debug(dev, "Unrolling primitive restart due to GS/XFB");

   agx_batch_init_state(batch);

   size_t ib_extent = 0;
   uint64_t ib;

   /* The rest of this function handles only the general case of indirect
    * multidraws, so synthesize an indexed indirect draw now if we need one for
    * a direct draw (necessarily only one). This unifies the code paths.
    */
   struct pipe_draw_indirect_info indirect_synthesized = {.draw_count = 1};

   if (!indirect) {
      /* Adds in the offset so set to 0 in the desc */
      ib = agx_index_buffer_direct_ptr(batch, draw, info, &ib_extent);

      uint32_t desc[5] = {draw->count, info->instance_count, 0,
                          draw->index_bias, info->start_instance};

      u_upload_data(ctx->base.const_uploader, 0, sizeof(desc), 4, &desc,
                    &indirect_synthesized.offset, &indirect_synthesized.buffer);

      indirect = &indirect_synthesized;
   } else {
      /* Does not add in offset, the unroll kernel uses the desc's offset */
      ib = agx_index_buffer_rsrc_ptr(batch, info, &ib_extent);
   }

   /* Next, we unroll the index buffer used by the indirect draw */
   if (!batch->cdm.bo)
      batch->cdm = agx_encoder_allocate(batch, dev);

   struct agx_unroll_restart_key key = {
      .prim = info->mode,
      .index_size_B = info->index_size,
   };

   /* Allocate output indirect draw descriptors. This is exact. */
   struct agx_resource out_draws_rsrc = {0};
   struct agx_ptr out_draws = agx_pool_alloc_aligned_with_bo(
      &batch->pool, 5 * sizeof(uint32_t) * indirect->draw_count, 4,
      &out_draws_rsrc.bo);

   struct agx_resource *indirect_rsrc = agx_resource(indirect->buffer);
   agx_batch_reads(batch, indirect_rsrc);

   struct agx_restart_unroll_params unroll = {
      .heap = agx_batch_geometry_state(batch),
      .index_buffer = ib,
      .out_draws = out_draws.gpu,
      .restart_index = info->restart_index,
      .index_buffer_size_el = ib_extent / info->index_size,
      .flatshade_first = batch->ctx->rast->base.flatshade_first,
      .draws = indirect_rsrc->bo->va->addr + indirect->offset,
   };

   /* Unroll the index buffer for each draw */
   const struct agx_grid grid_setup =
      agx_grid_direct(1024 * indirect->draw_count, 1, 1, 1024, 1, 1);

   agx_launch_with_data(batch, &grid_setup, agx_nir_unroll_restart, &key,
                        sizeof(key), &unroll, sizeof(unroll));

   /* Now draw the results without restart */
   struct pipe_draw_info new_info = {
      .mode = u_decomposed_prim(info->mode),
      .index_size = info->index_size,
      .index.resource = ctx->heap,
      .increment_draw_id = info->increment_draw_id,
      .index_bias_varies = info->index_bias_varies,
   };

   struct pipe_draw_indirect_info new_indirect = *indirect;
   new_indirect.buffer = &out_draws_rsrc.base;
   new_indirect.offset = out_draws.gpu - out_draws_rsrc.bo->va->addr;
   new_indirect.stride = 5 * sizeof(uint32_t);

   ctx->active_draw_without_restart = true;
   ctx->base.draw_vbo(&ctx->base, &new_info, drawid_offset, &new_indirect, NULL,
                      1);
   ctx->active_draw_without_restart = false;
}

static bool
agx_needs_passthrough_gs(struct agx_context *ctx,
                         const struct pipe_draw_info *info,
                         const struct pipe_draw_indirect_info *indirect,
                         bool *xfb_only)
{
   /* If there is already a geometry shader in the pipeline, we do not need to
    * apply a passthrough GS of our own.
    */
   if (ctx->stage[PIPE_SHADER_GEOMETRY].shader)
      return false;

   /* Rendering adjacency requires a GS, add a passthrough since we don't have
    * one.
    */
   if (info->mode == MESA_PRIM_LINES_ADJACENCY ||
       info->mode == MESA_PRIM_TRIANGLES_ADJACENCY ||
       info->mode == MESA_PRIM_TRIANGLE_STRIP_ADJACENCY ||
       info->mode == MESA_PRIM_LINE_STRIP_ADJACENCY) {
      perf_debug_ctx(ctx, "Using passthrough GS due to adjacency primitives");
      return true;
   }

   /* TODO: Handle fans properly, we need to plumb a sysval. */
   if (info->mode == MESA_PRIM_TRIANGLE_FAN &&
       ctx->rast->base.flatshade_first &&
       ctx->stage[MESA_SHADER_FRAGMENT].shader->info.inputs_flat_shaded) {

      perf_debug_ctx(ctx, "Using passthrough GS due to first tri fans");
      return true;
   }

   /* TODO: this is really sloppy, we should add a VDM kernel for this. */
   if ((indirect || info->mode == MESA_PRIM_PATCHES) && ctx->active_queries &&
       ctx->prims_generated[0]) {
      perf_debug_ctx(ctx, "Using passthrough GS due to indirect prim query");
      return true;
   }

   /* Edge flags are emulated with a geometry shader */
   if (has_edgeflags(ctx, info->mode)) {
      perf_debug_ctx(ctx, "Using passthrough GS due to edge flags");
      return true;
   }

   /* Various pipeline statistics are implemented in the pre-GS shader. */
   if (ctx->pipeline_statistics[PIPE_STAT_QUERY_IA_PRIMITIVES] ||
       ctx->pipeline_statistics[PIPE_STAT_QUERY_C_PRIMITIVES] ||
       ctx->pipeline_statistics[PIPE_STAT_QUERY_C_INVOCATIONS]) {
      perf_debug_ctx(ctx, "Using passthrough GS due to pipeline statistics");
      return true;
   }

   /* Transform feedback is layered on geometry shaders, so if transform
    * feedback is used, we need a GS.
    */
   struct agx_uncompiled_shader *last_vtx =
      ctx->stage[PIPE_SHADER_TESS_EVAL].shader
         ?: ctx->stage[PIPE_SHADER_VERTEX].shader;

   if (last_vtx->has_xfb_info && ctx->streamout.num_targets) {
      *xfb_only = true;
      return true;
   }

   /* Otherwise, we don't need one */
   return false;
}

static enum mesa_prim
agx_tess_output_prim(struct agx_uncompiled_shader *tcs,
                     struct agx_uncompiled_shader *tes)
{
   if ((tcs && tcs->tess.point_mode) || tes->tess.point_mode) {
      return MESA_PRIM_POINTS;
   } else if (TESS_PRIMITIVE_ISOLINES ==
              MAX2(tcs ? tcs->tess.primitive : 0, tes->tess.primitive)) {
      return MESA_PRIM_LINES;
   } else {
      return MESA_PRIM_TRIANGLES;
   }
}

static struct agx_uncompiled_shader *
agx_get_passthrough_gs(struct agx_context *ctx,
                       struct agx_uncompiled_shader *prev_cso,
                       enum mesa_prim mode, bool xfb_passthrough)
{
   bool edgeflags = has_edgeflags(ctx, mode);

   if (mode == MESA_PRIM_PATCHES) {
      mode = agx_tess_output_prim(ctx->stage[MESA_SHADER_TESS_CTRL].shader,
                                  ctx->stage[MESA_SHADER_TESS_EVAL].shader);
   }

   /* Only handle the polygon mode when edge flags are in use, because
    * nir_passthrough_gs doesn't handle transform feedback + polygon mode
    * properly. Technically this can break edge flags + transform feedback
    * but that's firmly in "doctor, it hurts when I do this" territory, and
    * I'm not sure that's even possible to hit. TODO: Reevaluate.
    */
   unsigned poly_mode =
      edgeflags ? ctx->rast->base.fill_front : PIPE_POLYGON_MODE_FILL;

   if (prev_cso->passthrough_progs[mode][poly_mode][edgeflags])
      return prev_cso->passthrough_progs[mode][poly_mode][edgeflags];

   struct blob_reader reader;
   blob_reader_init(&reader, prev_cso->early_serialized_nir.data,
                    prev_cso->early_serialized_nir.size);
   nir_shader *prev = nir_deserialize(NULL, &agx_nir_options, &reader);

   nir_shader *gs = nir_create_passthrough_gs(
      &agx_nir_options, prev, mode, rast_prim(mode, poly_mode), edgeflags,
      false /* force line strip out */);

   ralloc_free(prev);

   struct agx_uncompiled_shader *cso = pipe_shader_from_nir(&ctx->base, gs);
   cso->is_xfb_passthrough = xfb_passthrough;
   prev_cso->passthrough_progs[mode][poly_mode][edgeflags] = cso;
   return cso;
}

static void
agx_apply_passthrough_gs(struct agx_context *ctx,
                         const struct pipe_draw_info *info,
                         unsigned drawid_offset,
                         const struct pipe_draw_indirect_info *indirect,
                         const struct pipe_draw_start_count_bias *draws,
                         unsigned num_draws, bool xfb_passthrough)
{
   enum pipe_shader_type prev_stage = ctx->stage[PIPE_SHADER_TESS_EVAL].shader
                                         ? PIPE_SHADER_TESS_EVAL
                                         : PIPE_SHADER_VERTEX;
   struct agx_uncompiled_shader *prev_cso = ctx->stage[prev_stage].shader;

   assert(ctx->stage[PIPE_SHADER_GEOMETRY].shader == NULL);

   /* Draw with passthrough */
   ctx->base.bind_gs_state(
      &ctx->base,
      agx_get_passthrough_gs(ctx, prev_cso, info->mode, xfb_passthrough));
   ctx->base.draw_vbo(&ctx->base, info, drawid_offset, indirect, draws,
                      num_draws);
   ctx->base.bind_gs_state(&ctx->base, NULL);
}

static void
util_draw_multi_unroll_indirect(struct pipe_context *pctx,
                                const struct pipe_draw_info *info,
                                const struct pipe_draw_indirect_info *indirect,
                                const struct pipe_draw_start_count_bias *draws)
{
   for (unsigned i = 0; i < indirect->draw_count; ++i) {
      const struct pipe_draw_indirect_info subindirect = {
         .buffer = indirect->buffer,
         .count_from_stream_output = indirect->count_from_stream_output,
         .offset = indirect->offset + (i * indirect->stride),
         .draw_count = 1,
      };

      pctx->draw_vbo(pctx, info, i, &subindirect, draws, 1);
   }
}

static void
util_draw_multi_upload_indirect(struct pipe_context *pctx,
                                const struct pipe_draw_info *info,
                                const struct pipe_draw_indirect_info *indirect,
                                const struct pipe_draw_start_count_bias *draws)
{
   struct pipe_draw_indirect_info indirect_ = *indirect;
   u_upload_data(pctx->const_uploader, 0, 4, 4, &indirect->draw_count,
                 &indirect_.indirect_draw_count_offset,
                 &indirect_.indirect_draw_count);

   pctx->draw_vbo(pctx, info, 0, &indirect_, draws, 1);
}

static void
agx_upload_draw_params(struct agx_batch *batch,
                       const struct pipe_draw_indirect_info *indirect,
                       const struct pipe_draw_start_count_bias *draws,
                       const struct pipe_draw_info *info)
{
   if (indirect) {
      struct agx_resource *indirect_rsrc = agx_resource(indirect->buffer);
      uint64_t address = indirect_rsrc->bo->va->addr + indirect->offset;
      agx_batch_reads(batch, indirect_rsrc);

      /* To implement draw parameters, we use the last 2 words of the
       * indirect draw descriptor. Offset by 3 words for indexed draw (5
       * total) and 2 words for non-indexed (4 total).  See the layouts of
       * indexed vs non-indexed draw descriptors.
       *
       * This gives us a consistent layout
       *
       *    uint32_t first_vertex;
       *    uint32_t base_instance;
       *
       * and we can implement load_first_vertex & load_base_instance without
       * checking for indexing.
       */
      uint32_t offset = info->index_size ? 3 : 2;
      batch->uniforms.tables[AGX_SYSVAL_TABLE_PARAMS] = address + offset * 4;
   } else {
      /* Upload just those two words. */
      uint32_t params[2] = {
         info->index_size ? draws->index_bias : draws->start,
         info->start_instance,
      };

      batch->uniforms.tables[AGX_SYSVAL_TABLE_PARAMS] =
         agx_pool_upload_aligned(&batch->pool, params, sizeof(params), 4);
   }
}

static void
agx_draw_patches(struct agx_context *ctx, const struct pipe_draw_info *info,
                 unsigned drawid_offset,
                 const struct pipe_draw_indirect_info *indirect,
                 const struct pipe_draw_start_count_bias *draws,
                 unsigned num_draws)
{
   struct agx_device *dev = agx_device(ctx->base.screen);
   perf_debug(dev, "Tessellation");

   struct agx_uncompiled_shader *tcs = ctx->stage[MESA_SHADER_TESS_CTRL].shader;
   struct agx_uncompiled_shader *tes = ctx->stage[MESA_SHADER_TESS_EVAL].shader;

   assert(tes != NULL && "required with patches");

   unsigned patch_vertices = ctx->patch_vertices;

   /* OpenGL allows omitting the tcs, fill in a passthrough program if needed.
    * In principle, we could optimize this case, but I don't think it matters.
    */
   bool unbind_tcs_when_done = false;
   if (!tcs) {
      struct agx_uncompiled_shader *vs = ctx->stage[MESA_SHADER_VERTEX].shader;

      assert(patch_vertices >= 1 &&
             patch_vertices <= ARRAY_SIZE(vs->passthrough_tcs));

      if (!vs->passthrough_tcs[patch_vertices - 1]) {
         struct blob_reader reader;
         blob_reader_init(&reader, vs->early_serialized_nir.data,
                          vs->early_serialized_nir.size);
         nir_shader *vs_nir = nir_deserialize(NULL, &agx_nir_options, &reader);
         nir_shader *nir = nir_create_passthrough_tcs(&agx_nir_options, vs_nir,
                                                      patch_vertices);
         ralloc_free(vs_nir);

         /* Lower the tess level sysvals and gather info, since mesa/st won't do
          * either for us.
          */
         NIR_PASS(_, nir, nir_lower_system_values);

         nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));

         vs->passthrough_tcs[patch_vertices - 1] =
            pipe_shader_from_nir(&ctx->base, nir);
      }

      tcs = vs->passthrough_tcs[patch_vertices - 1];
      ctx->base.bind_tcs_state(&ctx->base, tcs);
      unbind_tcs_when_done = true;
   }

   enum tess_primitive_mode mode =
      MAX2(tcs->tess.primitive, tes->tess.primitive);
   enum gl_tess_spacing spacing = MAX2(tcs->tess.spacing, tes->tess.spacing);

   enum pipe_tess_spacing pspacing = spacing == TESS_SPACING_EQUAL
                                        ? PIPE_TESS_SPACING_EQUAL
                                     : spacing == TESS_SPACING_FRACTIONAL_ODD
                                        ? PIPE_TESS_SPACING_FRACTIONAL_ODD
                                        : PIPE_TESS_SPACING_FRACTIONAL_EVEN;

   bool point_mode = MAX2(tcs->tess.point_mode, tes->tess.point_mode);
   enum mesa_prim out_prim = agx_tess_output_prim(tcs, tes);

   enum libagx_tess_partitioning partitioning =
      (enum libagx_tess_partitioning)pspacing;

   enum libagx_tess_output_primitive prim =
      point_mode       ? LIBAGX_TESS_OUTPUT_POINT
      : !tes->tess.ccw ? LIBAGX_TESS_OUTPUT_TRIANGLE_CCW
                       : LIBAGX_TESS_OUTPUT_TRIANGLE_CW;

   struct agx_bo *draw_bo = NULL;
   bool with_counts =
      indirect || ctx->stage[MESA_SHADER_GEOMETRY].shader != NULL;
   size_t draw_stride =
      ((!with_counts && point_mode) ? 4 : 6) * sizeof(uint32_t);

   struct agx_batch *batch = agx_get_batch(ctx);
   agx_batch_init_state(batch);

   if (!batch->cdm.bo) {
      batch->cdm = agx_encoder_allocate(batch, dev);
   }

   uint64_t ib = 0;
   size_t ib_extent = 0;

   if (info->index_size)
      ib = agx_index_buffer_ptr(batch, info, draws, &ib_extent);

   struct agx_ia_state ia = {
      .index_buffer = ib,
      .index_buffer_range_el = ib_extent,
      .verts_per_instance = draws ? draws->count : 0,
   };

   batch->uniforms.input_assembly =
      agx_pool_upload_aligned(&batch->pool, &ia, sizeof(ia), 8);

   agx_upload_draw_params(batch, indirect, draws, info);

   /* Setup parameters */
   uint64_t geom_state = agx_batch_geometry_state(batch);
   assert((tcs->tess.output_stride & 3) == 0 && "must be aligned");

   struct libagx_tess_args args = {
      .heap = geom_state,
      .tcs_stride_el = tcs->tess.output_stride / 4,
      .statistic = agx_get_query_address(
         batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_DS_INVOCATIONS]),
      .input_patch_size = patch_vertices,
      .output_patch_size = tcs->tess.output_patch_size,
      .tcs_patch_constants = tcs->tess.nr_patch_outputs,
      .tcs_per_vertex_outputs = tcs->tess.per_vertex_outputs,
      .patch_coord_buffer = agx_resource(ctx->heap)->bo->va->addr,
   };

   memcpy(&args.tess_level_outer_default, ctx->default_outer_level,
          sizeof(ctx->default_outer_level));
   memcpy(&args.tess_level_inner_default, ctx->default_inner_level,
          sizeof(ctx->default_inner_level));

   struct agx_grid vs_grid, tcs_grid, tess_grid;
   unsigned tess_wg_size = 64;

   agx_upload_vbos(batch);
   agx_update_vs(ctx, info->index_size);
   agx_update_tcs(ctx, info);
   /* XXX */
   ctx->stage[PIPE_SHADER_TESS_CTRL].dirty = ~0;
   ctx->stage[PIPE_SHADER_TESS_EVAL].dirty = ~0;
   agx_update_descriptors(batch, ctx->vs);
   agx_update_descriptors(batch, ctx->tcs);
   agx_batch_add_bo(batch, ctx->vs->bo);
   agx_batch_add_bo(batch, ctx->linked.vs->bo);

   batch->uniforms.vertex_outputs = ctx->vs->b.info.outputs;

   if (indirect == NULL) {
      unsigned in_patches = draws->count / patch_vertices;
      if (in_patches == 0)
         return;

      /* TCS invocation counter increments once per-patch */
      agx_query_increment_cpu(
         ctx, ctx->pipeline_statistics[PIPE_STAT_QUERY_HS_INVOCATIONS],
         in_patches);

      unsigned unrolled_patches = in_patches * info->instance_count;

      uint32_t alloc = 0;
      uint32_t tcs_out_offs = alloc;
      alloc += unrolled_patches * tcs->tess.output_stride;

      uint32_t patch_coord_offs = alloc;
      alloc += unrolled_patches * 4;

      uint32_t count_offs = alloc;
      if (with_counts)
         alloc += unrolled_patches * sizeof(uint32_t);

      uint32_t draw_offs = alloc;

      if (with_counts) {
         alloc += draw_stride;
      } else {
         /* Padding added because VDM overreads */
         alloc +=
            (draw_stride * unrolled_patches) + (AGX_VDM_BARRIER_LENGTH + 0x800);
      }

      struct agx_ptr blob =
         agx_pool_alloc_aligned_with_bo(&batch->pool, alloc, 4, &draw_bo);

      args.tcs_buffer = blob.gpu + tcs_out_offs;
      args.patches_per_instance = in_patches;
      args.coord_allocs = blob.gpu + patch_coord_offs;
      args.nr_patches = unrolled_patches;
      args.out_draws = blob.gpu + draw_offs;

      if (with_counts) {
         args.counts = blob.gpu + count_offs;
      } else {
         /* Arrange so we return after all generated draws */
         uint8_t *ret =
            (uint8_t *)blob.cpu + draw_offs + (draw_stride * unrolled_patches);

         agx_pack(ret, VDM_BARRIER, cfg) {
            cfg.returns = true;
         }
      }

      unsigned vb_size = libagx_tcs_in_size(draws->count * info->instance_count,
                                            batch->uniforms.vertex_outputs);
      uint64_t addr = agx_pool_alloc_aligned(&batch->pool, vb_size, 4).gpu;
      batch->uniforms.vertex_output_buffer_ptr =
         agx_pool_upload(&batch->pool, &addr, 8);

      vs_grid =
         agx_grid_direct(draws->count, info->instance_count, 1, 64, 1, 1);

      tcs_grid = agx_grid_direct(in_patches * tcs->tess.output_patch_size,
                                 info->instance_count, 1,
                                 tcs->tess.output_patch_size, 1, 1);

      tess_grid = agx_grid_direct(unrolled_patches, 1, 1, tess_wg_size, 1, 1);
   } else if (indirect) {
      args.tcs_statistic = agx_get_query_address(
         batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_HS_INVOCATIONS]);

      args.indirect = agx_indirect_buffer_ptr(batch, indirect);

      /* Allocate 3x indirect global+local grids for VS/TCS/tess */
      uint32_t grid_stride = sizeof(uint32_t) * 6;
      args.grids = agx_pool_alloc_aligned(&batch->pool, grid_stride * 3, 4).gpu;

      vs_grid = agx_grid_indirect_local(args.grids + 0 * grid_stride);
      tcs_grid = agx_grid_indirect_local(args.grids + 1 * grid_stride);
      tess_grid = agx_grid_indirect_local(args.grids + 2 * grid_stride);

      args.vertex_outputs = ctx->vs->b.info.outputs;
      args.vertex_output_buffer_ptr =
         agx_pool_alloc_aligned(&batch->pool, 8, 8).gpu;

      batch->uniforms.vertex_output_buffer_ptr = args.vertex_output_buffer_ptr;

      if (with_counts) {
         args.out_draws = agx_pool_alloc_aligned_with_bo(
                             &batch->pool, draw_stride, 4, &draw_bo)
                             .gpu;
      } else {
         unreachable("need an extra indirection...");
      }
   }

   uint64_t state =
      agx_pool_upload_aligned(&batch->pool, &args, sizeof(args), 4);

   if (indirect) {
      const struct agx_grid indirect_grid = agx_grid_direct(1, 1, 1, 1, 1, 1);
      struct agx_tess_setup_indirect_key indirect_key = {
         .point_mode = point_mode,
         .with_counts = with_counts,
      };

      agx_launch_with_uploaded_data(batch, &indirect_grid,
                                    agx_nir_tess_setup_indirect, &indirect_key,
                                    sizeof(indirect_key), state);
   }

   batch->uniforms.tess_params = state;

   agx_launch(batch, &vs_grid, ctx->vs, ctx->linked.vs, PIPE_SHADER_VERTEX, 0);
   agx_launch(batch, &tcs_grid, ctx->tcs, NULL, PIPE_SHADER_TESS_CTRL, 0);
   batch->uniforms.vertex_output_buffer_ptr = 0;

   struct agx_tessellator_key key = {
      .prim = mode,
      .output_primitive = prim,
      .partitioning = partitioning,
   };

   if (with_counts) {
      /* Generate counts */
      key.mode = LIBAGX_TESS_MODE_COUNT;
      agx_launch_with_uploaded_data(batch, &tess_grid, agx_nir_tessellate, &key,
                                    sizeof(key), state);

      /* Prefix sum counts, allocating index buffer space. */
      const struct agx_grid prefix_sum_grid =
         agx_grid_direct(1024, 1, 1, 1024, 1, 1);

      agx_launch_with_uploaded_data(batch, &prefix_sum_grid,
                                    agx_nir_prefix_sum_tess, NULL, 0, state);

      key.mode = LIBAGX_TESS_MODE_WITH_COUNTS;
   } else {
      key.mode = LIBAGX_TESS_MODE_VDM;
   }

   /* Now we can tessellate */
   agx_launch_with_uploaded_data(batch, &tess_grid, agx_nir_tessellate, &key,
                                 sizeof(key), state);

   /* Run TES as VS */
   void *vs_cso = ctx->stage[PIPE_SHADER_VERTEX].shader;
   void *tes_cso = ctx->stage[PIPE_SHADER_TESS_EVAL].shader;
   ctx->base.bind_vs_state(&ctx->base, tes_cso);
   ctx->in_tess = true;
   ctx->in_generated_vdm = !with_counts;

   struct pipe_draw_info draw_info = {
      .mode = out_prim,
      .index_size = with_counts ? 4 : (point_mode ? 0 : 2),
      .index.resource = (!with_counts && point_mode) ? NULL : ctx->heap,
      .instance_count = 1,
   };

   /* Wrap the pool allocation in a fake resource for meta-Gallium use */
   struct agx_resource indirect_rsrc = {.bo = draw_bo};

   struct pipe_draw_indirect_info copy_indirect = {
      .buffer = &indirect_rsrc.base,
      .offset = args.out_draws - draw_bo->va->addr,
      .stride = draw_stride,
      .draw_count = 1,
   };

   ctx->base.draw_vbo(&ctx->base, &draw_info, 0, &copy_indirect, NULL, 1);

   /* Restore vertex state */
   ctx->base.bind_vs_state(&ctx->base, vs_cso);
   ctx->in_generated_vdm = false;
   ctx->in_tess = false;

   if (unbind_tcs_when_done) {
      ctx->base.bind_tcs_state(&ctx->base, NULL);
   }
}

/*
 * From the ARB_texture_barrier spec:
 *
 *  Specifically, the values of rendered fragments are undefined if any
 *  shader stage fetches texels and the same texels are written via fragment
 *  shader outputs, even if the reads and writes are not in the same Draw
 *  call, unless any of the following exceptions apply:
 *
 *  - The reads and writes are from/to disjoint sets of texels (after
 *    accounting for texture filtering rules).
 *
 *  - There is only a single read and write of each texel, and the read is in
 *    the fragment shader invocation that writes the same texel (e.g. using
 *    "texelFetch2D(sampler, ivec2(gl_FragCoord.xy), 0);").
 *
 *  - If a texel has been written, then in order to safely read the result
 *    a texel fetch must be in a subsequent Draw separated by the command
 *
 *      void TextureBarrier(void);
 *
 *    TextureBarrier() will guarantee that writes have completed and caches
 *    have been invalidated before subsequent Draws are executed."
 *
 * The wording is subtle, but we are not required to flush implicitly for
 * feedback loops, even though we're a tiler. What we are required to do is
 * decompress framebuffers involved in feedback loops, because otherwise
 * the hardware will race itself with exception #1, where we have a disjoint
 * group texels that intersects a compressed tile being written out.
 */
static void
agx_legalize_feedback_loops(struct agx_context *ctx)
{
   /* Trust that u_blitter knows what it's doing */
   if (ctx->blitter->running)
      return;

   for (unsigned stage = 0; stage < ARRAY_SIZE(ctx->stage); ++stage) {
      if (!(ctx->stage[stage].dirty & AGX_STAGE_DIRTY_IMAGE))
         continue;

      for (unsigned i = 0; i < ctx->stage[stage].texture_count; ++i) {
         if (!ctx->stage[stage].textures[i])
            continue;

         struct agx_resource *rsrc = ctx->stage[stage].textures[i]->rsrc;

         for (unsigned cb = 0; cb < ctx->framebuffer.nr_cbufs; ++cb) {
            if (ctx->framebuffer.cbufs[cb] &&
                agx_resource(ctx->framebuffer.cbufs[cb]->texture) == rsrc) {

               if (rsrc->layout.tiling == AIL_TILING_TWIDDLED_COMPRESSED) {
                  /* Decompress if we can and shadow if we can't. */
                  if (rsrc->base.bind & PIPE_BIND_SHARED)
                     unreachable("TODO");
                  else
                     agx_decompress(ctx, rsrc, "Texture feedback loop");
               }

               /* Not required by the spec, just for debug */
               if (agx_device(ctx->base.screen)->debug & AGX_DBG_FEEDBACK)
                  agx_flush_writer(ctx, rsrc, "Feedback loop");
            }
         }
      }
   }
}

static void
agx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
             unsigned drawid_offset,
             const struct pipe_draw_indirect_info *indirect,
             const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct agx_context *ctx = agx_context(pctx);

   if (unlikely(!agx_render_condition_check(ctx)))
      return;

   if (num_draws > 1) {
      util_draw_multi(pctx, info, drawid_offset, indirect, draws, num_draws);
      return;
   }

   if (indirect && indirect->draw_count > 1 && !indirect->indirect_draw_count) {
      assert(drawid_offset == 0);
      assert(num_draws == 1);

      util_draw_multi_unroll_indirect(pctx, info, indirect, draws);
      return;
   }

   if (indirect && indirect->count_from_stream_output) {
      agx_draw_vbo_from_xfb(pctx, info, drawid_offset, indirect);
      return;
   }

   /* TODO: stop cheating */
   if (indirect && indirect->indirect_draw_count) {
      perf_debug_ctx(ctx, "multi-draw indirect");
      util_draw_indirect(pctx, info, drawid_offset, indirect);
      return;
   }

   bool xfb_passthrough = false;
   if (agx_needs_passthrough_gs(ctx, info, indirect, &xfb_passthrough)) {
      agx_apply_passthrough_gs(ctx, info, drawid_offset, indirect, draws,
                               num_draws, xfb_passthrough);
      return;
   }

   if (info->mode == MESA_PRIM_PATCHES) {
      agx_draw_patches(ctx, info, drawid_offset, indirect, draws, num_draws);
      return;
   }

   agx_legalize_feedback_loops(ctx);

   /* Only the rasterization stream counts */
   if (ctx->active_queries && ctx->prims_generated[0] &&
       !ctx->stage[PIPE_SHADER_GEOMETRY].shader) {

      assert(!indirect && "we force a passthrough GS for this");
      agx_primitives_update_direct(ctx, info, draws);
   }

   struct agx_batch *batch = agx_get_batch(ctx);

   uint64_t ib = 0;
   size_t ib_extent = 0;

   if (info->index_size) {
      ib =
         agx_index_buffer_ptr(batch, info, indirect ? NULL : draws, &ib_extent);
   }

   if (ctx->active_queries && !ctx->active_draw_without_restart &&
       (ctx->pipeline_statistics[PIPE_STAT_QUERY_IA_VERTICES] ||
        ctx->pipeline_statistics[PIPE_STAT_QUERY_VS_INVOCATIONS])) {

      uint64_t ptr;
      if (indirect) {
         ptr = agx_indirect_buffer_ptr(batch, indirect);
      } else {
         uint32_t desc[] = {draws->count, info->instance_count, 0};
         ptr = agx_pool_upload(&batch->pool, &desc, sizeof(desc));
      }

      agx_ia_update(batch, info, ptr, ib,
                    info->index_size ? ib_extent / info->index_size : 1);
   }

   if (ctx->stage[PIPE_SHADER_GEOMETRY].shader && info->primitive_restart &&
       info->index_size) {

      agx_draw_without_restart(batch, info, drawid_offset, indirect, draws);
      return;
   }

   agx_batch_add_timestamp_query(batch, ctx->time_elapsed);

#ifndef NDEBUG
   if (unlikely(agx_device(pctx->screen)->debug & AGX_DBG_DIRTY))
      agx_dirty_all(ctx);
#endif

   agx_batch_init_state(batch);

   /* Dirty track the reduced prim: lines vs points vs triangles. Happens before
    * agx_update_vs/agx_update_fs, which specialize based on primitive.
    */
   enum mesa_prim reduced_prim = u_reduced_prim(info->mode);
   if (reduced_prim != batch->reduced_prim)
      ctx->dirty |= AGX_DIRTY_PRIM;
   batch->reduced_prim = reduced_prim;

   /* Update shaders first so we can use them after */
   if (agx_update_vs(ctx, info->index_size)) {
      ctx->dirty |= AGX_DIRTY_VS | AGX_DIRTY_VS_PROG;
      ctx->stage[PIPE_SHADER_VERTEX].dirty = ~0;

      agx_batch_add_bo(batch, ctx->vs->bo);
      if (ctx->linked.vs)
         agx_batch_add_bo(batch, ctx->linked.vs->bo);
   } else if (ctx->stage[PIPE_SHADER_VERTEX].dirty ||
              (ctx->dirty & AGX_DIRTY_VERTEX))
      ctx->dirty |= AGX_DIRTY_VS;

   agx_update_gs(ctx, info, indirect);

   if (ctx->gs) {
      batch->geom_indirect = agx_pool_alloc_aligned_with_bo(
                                &batch->pool, 64, 4, &batch->geom_indirect_bo)
                                .gpu;

      batch->uniforms.geometry_params =
         agx_batch_geometry_params(batch, ib, ib_extent, info, draws, indirect);

      agx_batch_add_bo(batch, ctx->gs->bo);
      agx_batch_add_bo(batch, ctx->gs->gs_copy->bo);
   }

   if (ctx->dirty & (AGX_DIRTY_VS_PROG | AGX_DIRTY_FS_PROG)) {
      struct agx_compiled_shader *vs = ctx->vs;
      if (ctx->gs)
         vs = ctx->gs->gs_copy;

      agx_assign_uvs(
         &batch->linked_varyings, &vs->uvs,
         ctx->stage[PIPE_SHADER_FRAGMENT].shader->info.inputs_flat_shaded,
         ctx->stage[PIPE_SHADER_FRAGMENT].shader->info.inputs_linear_shaded);

      for (unsigned i = 0; i < VARYING_SLOT_MAX; ++i) {
         batch->uniforms.uvs_index[i] = batch->linked_varyings.slots[i];
      }
   }

   /* Set draw ID */
   if (ctx->vs->b.info.uses_draw_id) {
      batch->uniforms.draw_id = drawid_offset;

      ctx->dirty |= AGX_DIRTY_VS;
   }

   if (agx_update_fs(batch)) {
      ctx->dirty |= AGX_DIRTY_FS | AGX_DIRTY_FS_PROG;
      ctx->stage[PIPE_SHADER_FRAGMENT].dirty = ~0;

      if (ctx->fs->bo)
         agx_batch_add_bo(batch, ctx->fs->bo);

      agx_batch_add_bo(batch, ctx->linked.fs->bo);
   } else if ((ctx->stage[PIPE_SHADER_FRAGMENT].dirty) ||
              (ctx->dirty & (AGX_DIRTY_BLEND_COLOR | AGX_DIRTY_SAMPLE_MASK))) {
      ctx->dirty |= AGX_DIRTY_FS;
   }

   if (ctx->linked.vs->uses_base_param || ctx->gs) {
      agx_upload_draw_params(batch, indirect, draws, info);

      batch->uniforms.is_indexed_draw = (info->index_size > 0);
      ctx->dirty |= AGX_DIRTY_VS;
   }

   agx_update_descriptors(batch, ctx->vs);
   agx_update_descriptors(batch, ctx->gs);
   agx_update_descriptors(batch, ctx->fs);

   if (IS_DIRTY(VS) || IS_DIRTY(FS) || ctx->gs || IS_DIRTY(VERTEX) ||
       IS_DIRTY(BLEND_COLOR) || IS_DIRTY(QUERY) || IS_DIRTY(POLY_STIPPLE) ||
       IS_DIRTY(RS) || IS_DIRTY(PRIM) || ctx->in_tess) {

      if (IS_DIRTY(VERTEX)) {
         agx_upload_vbos(batch);
      }

      if (IS_DIRTY(BLEND_COLOR)) {
         memcpy(batch->uniforms.blend_constant, &ctx->blend_color,
                sizeof(ctx->blend_color));
      }

      if (IS_DIRTY(RS)) {
         struct pipe_rasterizer_state *rs = &ctx->rast->base;

         batch->uniforms.fixed_point_size =
            rs->point_size_per_vertex ? 0.0 : rs->point_size;

         /* TODO: tri fans */
         batch->uniforms.provoking_vertex = !rs->flatshade_first ? 2 : 0;
      }

      if (IS_DIRTY(QUERY)) {
         for (unsigned i = 0; i < ARRAY_SIZE(ctx->pipeline_statistics); ++i) {
            struct agx_query *query = ctx->pipeline_statistics[i];
            batch->uniforms.pipeline_statistics[i] =
               agx_get_query_address(batch, query);
         }
      }

      if (IS_DIRTY(POLY_STIPPLE)) {
         STATIC_ASSERT(sizeof(ctx->poly_stipple) == 32 * 4);

         batch->uniforms.polygon_stipple = agx_pool_upload_aligned(
            &batch->pool, ctx->poly_stipple, sizeof(ctx->poly_stipple), 4);
      }

      agx_upload_uniforms(batch);
   }

   struct pipe_draw_info info_gs;
   struct pipe_draw_indirect_info indirect_gs;

   /* Wrap the pool allocation in a fake resource for meta-Gallium use */
   struct agx_resource indirect_rsrc = {.bo = batch->geom_indirect_bo};

   if (ctx->gs) {
      /* Launch the pre-rasterization parts of the geometry shader */
      agx_launch_gs_prerast(batch, info, draws, indirect);

      if (ctx->rast->base.rasterizer_discard)
         return;

      /* Setup to rasterize the GS results */
      info_gs = (struct pipe_draw_info){
         .mode = ctx->gs->gs_output_mode,
         .index_size = 4,
         .primitive_restart = ctx->gs->gs_output_mode != MESA_PRIM_POINTS,
         .restart_index = ~0,
         .index.resource = ctx->heap,
         .instance_count = 1,
      };

      indirect_gs = (struct pipe_draw_indirect_info){
         .draw_count = 1,
         .buffer = &indirect_rsrc.base,
         .offset = batch->geom_indirect - indirect_rsrc.bo->va->addr,
      };

      info = &info_gs;
      indirect = &indirect_gs;

      /* TODO: Deduplicate? */
      batch->reduced_prim = u_reduced_prim(info->mode);
      ctx->dirty |= AGX_DIRTY_PRIM;

      if (info_gs.index_size) {
         ib = agx_resource(ctx->heap)->bo->va->addr;
         ib_extent = agx_resource(ctx->heap)->bo->size;
      } else {
         ib = 0;
         ib_extent = 0;
      }

      /* We need to reemit geometry descriptors since the txf sampler may change
       * between the GS prepass and the GS rast program.
       */
      agx_update_descriptors(batch, ctx->gs->gs_copy);
   }

   assert((!indirect || !indirect->indirect_draw_count) && "multidraw handled");

   /* Update batch masks based on current state */
   if (ctx->dirty & AGX_DIRTY_BLEND) {
      /* TODO: Any point to tracking load? */
      batch->draw |= ctx->blend->store;
      batch->resolve |= ctx->blend->store;
   }

   if (ctx->dirty & AGX_DIRTY_ZS) {
      batch->load |= ctx->zs->load;
      batch->draw |= ctx->zs->store;
      batch->resolve |= ctx->zs->store;
   }

   /* When we approach the end of a command buffer, cycle it out for a new one.
    * We only need to do this once per draw as long as we conservatively
    * estimate the maximum bytes of VDM commands that this draw will emit.
    */
   agx_ensure_cmdbuf_has_space(
      batch, &batch->vdm,
      (AGX_VDM_STATE_LENGTH * 2) + (AGX_PPP_STATE_LENGTH * MAX_PPP_UPDATES) +
         AGX_VDM_STATE_RESTART_INDEX_LENGTH +
         AGX_VDM_STATE_VERTEX_SHADER_WORD_0_LENGTH +
         AGX_VDM_STATE_VERTEX_SHADER_WORD_1_LENGTH +
         AGX_VDM_STATE_VERTEX_OUTPUTS_LENGTH +
         AGX_VDM_STATE_VERTEX_UNKNOWN_LENGTH + 4 /* padding */ +
         AGX_INDEX_LIST_LENGTH + AGX_INDEX_LIST_BUFFER_LO_LENGTH +
         AGX_INDEX_LIST_COUNT_LENGTH + AGX_INDEX_LIST_INSTANCES_LENGTH +
         AGX_INDEX_LIST_START_LENGTH + AGX_INDEX_LIST_BUFFER_SIZE_LENGTH);

   uint8_t *out = agx_encode_state(batch, batch->vdm.current);

   if (ctx->in_generated_vdm) {
      struct agx_resource *indirect_rsrc = agx_resource(indirect->buffer);
      uint64_t address = indirect_rsrc->bo->va->addr + indirect->offset;

      agx_push(out, VDM_STREAM_LINK, cfg) {
         cfg.target_lo = address & BITFIELD_MASK(32);
         cfg.target_hi = address >> 32;
         cfg.with_return = true;
      }
   } else {

      if (info->index_size && info->primitive_restart) {
         agx_push(out, VDM_STATE, cfg)
            cfg.restart_index_present = true;

         agx_push(out, VDM_STATE_RESTART_INDEX, cfg)
            cfg.value = info->restart_index;
      }

      agx_push(out, INDEX_LIST, cfg) {
         cfg.primitive = agx_primitive_for_pipe(info->mode);

         if (indirect != NULL) {
            cfg.indirect_buffer_present = true;
         } else {
            cfg.instance_count_present = true;
            cfg.index_count_present = true;
            cfg.start_present = true;
         }

         if (info->index_size) {
            cfg.restart_enable = info->primitive_restart;
            cfg.index_buffer_hi = (ib >> 32);
            cfg.index_size = agx_translate_index_size(info->index_size);
            cfg.index_buffer_present = true;
            cfg.index_buffer_size_present = true;
         }
      }

      if (info->index_size) {
         agx_push(out, INDEX_LIST_BUFFER_LO, cfg) {
            cfg.buffer_lo = ib & BITFIELD_MASK(32);
         }
      }

      if (indirect) {
         struct agx_resource *indirect_rsrc = agx_resource(indirect->buffer);
         uint64_t address = indirect_rsrc->bo->va->addr + indirect->offset;

         agx_push(out, INDEX_LIST_INDIRECT_BUFFER, cfg) {
            cfg.address_hi = address >> 32;
            cfg.address_lo = address & BITFIELD_MASK(32);
         }
      } else {
         agx_push(out, INDEX_LIST_COUNT, cfg)
            cfg.count = draws->count;

         agx_push(out, INDEX_LIST_INSTANCES, cfg)
            cfg.count = info->instance_count;

         agx_push(out, INDEX_LIST_START, cfg) {
            cfg.start = info->index_size ? draws->index_bias : draws->start;
         }
      }

      if (info->index_size) {
         agx_push(out, INDEX_LIST_BUFFER_SIZE, cfg) {
            cfg.size = ib_extent;
         }
      }
   }

   batch->vdm.current = out;
   assert((batch->vdm.current + AGX_VDM_STREAM_LINK_LENGTH) <= batch->vdm.end &&
          "Failed to reserve sufficient space in encoder");
   agx_dirty_reset_graphics(ctx);

   assert(batch == agx_get_batch(ctx) && "batch should not change under us");

   batch->draws++;

   /* The scissor/zbias arrays are indexed with 16-bit integers, imposigin a
    * maximum of UINT16_MAX descriptors. Flush if the next draw would overflow
    */
   if (unlikely(
          (((batch->scissor.size / AGX_SCISSOR_LENGTH) + AGX_MAX_VIEWPORTS) >
           UINT16_MAX) ||
          (batch->depth_bias.size / AGX_DEPTH_BIAS_LENGTH) >= UINT16_MAX)) {
      agx_flush_batch_for_reason(ctx, batch, "Scissor/depth bias overflow");
   } else if (unlikely(batch->draws > 100000)) {
      /* Mostly so drawoverhead doesn't OOM */
      agx_flush_batch_for_reason(ctx, batch, "Absurd number of draws");
   } else if (unlikely(batch->sampler_heap.count >
                       (AGX_SAMPLER_HEAP_SIZE - (PIPE_MAX_SAMPLERS * 6)))) {
      agx_flush_batch_for_reason(ctx, batch, "Sampler heap overflow");
   }
}

static void
agx_texture_barrier(struct pipe_context *pipe, unsigned flags)
{
   struct agx_context *ctx = agx_context(pipe);

   /* Framebuffer fetch is coherent, so barriers are a no-op. */
   if (flags == PIPE_TEXTURE_BARRIER_FRAMEBUFFER)
      return;

   agx_flush_all(ctx, "Texture barrier");
}

void
agx_launch_internal(struct agx_batch *batch, const struct agx_grid *grid,
                    struct agx_compiled_shader *cs, enum pipe_shader_type stage,
                    uint32_t usc)
{
   struct agx_context *ctx = batch->ctx;
   struct agx_device *dev = agx_device(ctx->base.screen);

   /* TODO: Ensure space if we allow multiple kernels in a batch */
   uint8_t *out = batch->cdm.current;

   agx_push(out, CDM_LAUNCH_WORD_0, cfg) {
      cfg.mode = grid->mode;
      cfg.uniform_register_count = cs->b.info.push_count;
      cfg.preshader_register_count = cs->b.info.nr_preamble_gprs;
      cfg.texture_state_register_count = agx_nr_tex_descriptors(batch, cs);
      cfg.sampler_state_register_count =
         translate_sampler_state_count(ctx, cs, stage);
   }

   agx_push(out, CDM_LAUNCH_WORD_1, cfg) {
      cfg.pipeline = usc;
   }

   /* Added in G14X */
   if (dev->params.gpu_generation >= 14 && dev->params.num_clusters_total > 1) {
      agx_push(out, CDM_UNK_G14X, cfg)
         ;
   }

   if (grid->mode == AGX_CDM_MODE_DIRECT) {
      agx_push(out, CDM_GLOBAL_SIZE, cfg) {
         cfg.x = grid->global[0];
         cfg.y = grid->global[1];
         cfg.z = grid->global[2];
      }
   } else {
      agx_push(out, CDM_INDIRECT, cfg) {
         cfg.address_hi = grid->indirect >> 32;
         cfg.address_lo = grid->indirect & BITFIELD64_MASK(32);
      }
   }

   if (grid->mode != AGX_CDM_MODE_INDIRECT_LOCAL) {
      agx_push(out, CDM_LOCAL_SIZE, cfg) {
         cfg.x = grid->local[0];
         cfg.y = grid->local[1];
         cfg.z = grid->local[2];
      }
   }

   agx_push(out, CDM_BARRIER, cfg) {
      cfg.unk_5 = true;
      cfg.unk_6 = true;
      cfg.unk_8 = true;
      // cfg.unk_11 = true;
      // cfg.unk_20 = true;
      if (dev->params.num_clusters_total > 1) {
         // cfg.unk_24 = true;
         if (dev->params.gpu_generation == 13) {
            cfg.unk_4 = true;
            // cfg.unk_26 = true;
         }
      }

      /* With multiple launches in the same CDM stream, we can get cache
       * coherency (? or sync?) issues. We hit this with blits, which need - in
       * between dispatches - need the PBE cache to be flushed and the texture
       * cache to be invalidated. Until we know what bits mean what exactly,
       * let's just set these after every launch to be safe. We can revisit in
       * the future when we figure out what the bits mean.
       */
      cfg.unk_0 = true;
      cfg.unk_1 = true;
      cfg.unk_2 = true;
      cfg.usc_cache_inval = true;
      cfg.unk_4 = true;
      cfg.unk_5 = true;
      cfg.unk_6 = true;
      cfg.unk_7 = true;
      cfg.unk_8 = true;
      cfg.unk_9 = true;
      cfg.unk_10 = true;
      cfg.unk_11 = true;
      cfg.unk_12 = true;
      cfg.unk_13 = true;
      cfg.unk_14 = true;
      cfg.unk_15 = true;
      cfg.unk_16 = true;
      cfg.unk_17 = true;
      cfg.unk_18 = true;
      cfg.unk_19 = true;
   }

   batch->cdm.current = out;
   assert(batch->cdm.current <= batch->cdm.end &&
          "Failed to reserve sufficient space in encoder");
}

void
agx_launch(struct agx_batch *batch, const struct agx_grid *grid,
           struct agx_compiled_shader *cs, struct agx_linked_shader *linked,
           enum pipe_shader_type stage, unsigned variable_shared_mem)
{
   struct agx_context *ctx = batch->ctx;

   /* To implement load_num_workgroups, the number of workgroups needs to be
    * available in GPU memory. This is either the indirect buffer, or just a
    * buffer we upload ourselves if not indirect.
    */
   if (grid->mode == AGX_CDM_MODE_DIRECT) {
      uint32_t groups[3] = {
         grid->global[0] / grid->local[0],
         grid->global[1] / grid->local[1],
         grid->global[2] / grid->local[2],
      };

      batch->uniforms.tables[AGX_SYSVAL_TABLE_GRID] =
         agx_pool_upload_aligned(&batch->pool, groups, sizeof(groups), 4);
   } else {
      batch->uniforms.tables[AGX_SYSVAL_TABLE_GRID] = grid->indirect;
   }

   util_dynarray_foreach(&ctx->global_buffers, struct pipe_resource *, res) {
      if (!*res)
         continue;

      struct agx_resource *buffer = agx_resource(*res);
      agx_batch_writes(batch, buffer, 0);
      batch->incoherent_writes = true;
   }

   agx_batch_add_bo(batch, cs->bo);

   agx_update_descriptors(batch, cs);
   agx_upload_uniforms(batch);

   // TODO: This is broken.
   size_t subgroups_per_core = 0;
#if 0
   if (!info->indirect) {
      size_t subgroups_per_workgroup =
         DIV_ROUND_UP(info->block[0] * info->block[1] * info->block[2], 32);
      subgroups_per_core =
         local_workgroups *
         DIV_ROUND_UP(info->grid[0] * info->grid[1] * info->grid[2],
                     ctx->scratch_cs.num_cores);
   }
#endif

   uint32_t usc = agx_build_pipeline(batch, cs, linked, PIPE_SHADER_COMPUTE,
                                     variable_shared_mem, subgroups_per_core);

   agx_launch_internal(batch, grid, cs, stage, usc);
}

static void
agx_launch_grid(struct pipe_context *pipe, const struct pipe_grid_info *info)
{
   struct agx_context *ctx = agx_context(pipe);
   if (unlikely(!ctx->compute_blitter.active &&
                !agx_render_condition_check(ctx)))
      return;

   struct agx_batch *batch = agx_get_compute_batch(ctx);

   uint64_t indirect = 0;
   if (info->indirect) {
      struct agx_resource *rsrc = agx_resource(info->indirect);
      agx_batch_reads(batch, rsrc);
      indirect = rsrc->bo->va->addr + info->indirect_offset;
   }

   /* Increment the pipeline stats query.
    *
    * TODO: Can we use the hardware counter for this?
    */
   if (ctx->pipeline_statistics[PIPE_STAT_QUERY_CS_INVOCATIONS]) {
      unsigned blocksize = info->block[0] * info->block[1] * info->block[2];

      if (info->indirect) {
         struct libagx_cs_invocation_params p = {
            .grid = indirect,
            .local_size_threads = blocksize,
            .statistic = agx_get_query_address(
               batch, ctx->pipeline_statistics[PIPE_STAT_QUERY_CS_INVOCATIONS]),
         };

         const struct agx_grid g = agx_grid_direct(1, 1, 1, 1, 1, 1);
         agx_launch_with_data(batch, &g, agx_nir_increment_cs_invocations, NULL,
                              0, &p, sizeof(p));
      } else {
         agx_query_increment_cpu(
            ctx, ctx->pipeline_statistics[PIPE_STAT_QUERY_CS_INVOCATIONS],
            libagx_cs_invocations(blocksize, info->grid[0], info->grid[1],
                                  info->grid[2]));
      }
   }

   agx_batch_add_timestamp_query(batch, ctx->time_elapsed);

   agx_batch_init_state(batch);

   struct agx_uncompiled_shader *uncompiled =
      ctx->stage[PIPE_SHADER_COMPUTE].shader;

   /* There is exactly one variant, get it */
   struct agx_compiled_shader *cs =
      _mesa_hash_table_next_entry(uncompiled->variants, NULL)->data;

   struct agx_grid grid = {
      .local[0] = info->block[0],
      .local[1] = info->block[1],
      .local[2] = info->block[2],
   };

   if (info->indirect) {
      struct agx_resource *indirect = agx_resource(info->indirect);
      agx_batch_reads(batch, indirect);

      grid.mode = AGX_CDM_MODE_INDIRECT_GLOBAL;
      grid.indirect = indirect->bo->va->addr + info->indirect_offset;
   } else {
      grid.mode = AGX_CDM_MODE_DIRECT;

      for (unsigned d = 0; d < 3; ++d) {
         grid.global[d] = ((info->grid[d] - 1) * info->block[d]) +
                          (info->last_block[d] ?: info->block[d]);
      }
   }

   agx_launch(batch, &grid, cs, NULL, PIPE_SHADER_COMPUTE,
              info->variable_shared_mem);

   /* TODO: Dirty tracking? */
   agx_dirty_all(ctx);

   batch->uniforms.tables[AGX_SYSVAL_TABLE_GRID] = 0;

   /* If the next dispatch might overflow, flush now. TODO: If this is ever hit
    * in practice, we can use CDM stream links.
    */
   size_t dispatch_upper_bound =
      AGX_CDM_LAUNCH_WORD_0_LENGTH + AGX_CDM_LAUNCH_WORD_1_LENGTH +
      AGX_CDM_UNK_G14X_LENGTH + AGX_CDM_INDIRECT_LENGTH +
      AGX_CDM_GLOBAL_SIZE_LENGTH + AGX_CDM_LOCAL_SIZE_LENGTH +
      AGX_CDM_BARRIER_LENGTH;

   if (batch->cdm.current + dispatch_upper_bound >= batch->cdm.end)
      agx_flush_batch_for_reason(ctx, batch, "CDM overfull");
}

static void
agx_set_global_binding(struct pipe_context *pipe, unsigned first,
                       unsigned count, struct pipe_resource **resources,
                       uint32_t **handles)
{
   struct agx_context *ctx = agx_context(pipe);
   unsigned old_size =
      util_dynarray_num_elements(&ctx->global_buffers, *resources);

   if (old_size < first + count) {
      /* we are screwed no matter what */
      if (!util_dynarray_grow(&ctx->global_buffers, *resources,
                              (first + count) - old_size))
         unreachable("out of memory");

      for (unsigned i = old_size; i < first + count; i++)
         *util_dynarray_element(&ctx->global_buffers, struct pipe_resource *,
                                i) = NULL;
   }

   for (unsigned i = 0; i < count; ++i) {
      struct pipe_resource **res = util_dynarray_element(
         &ctx->global_buffers, struct pipe_resource *, first + i);
      if (resources && resources[i]) {
         pipe_resource_reference(res, resources[i]);

         /* The handle points to uint32_t, but space is allocated for 64
          * bits. We need to respect the offset passed in. This interface
          * is so bad.
          */
         uint64_t addr = 0;
         struct agx_resource *rsrc = agx_resource(resources[i]);

         memcpy(&addr, handles[i], sizeof(addr));
         addr += rsrc->bo->va->addr;
         memcpy(handles[i], &addr, sizeof(addr));
      } else {
         pipe_resource_reference(res, NULL);
      }
   }
}

void agx_init_state_functions(struct pipe_context *ctx);

void
agx_init_state_functions(struct pipe_context *ctx)
{
   ctx->create_blend_state = agx_create_blend_state;
   ctx->create_depth_stencil_alpha_state = agx_create_zsa_state;
   ctx->create_fs_state = agx_create_shader_state;
   ctx->create_rasterizer_state = agx_create_rs_state;
   ctx->create_sampler_state = agx_create_sampler_state;
   ctx->create_sampler_view = agx_create_sampler_view;
   ctx->create_surface = agx_create_surface;
   ctx->create_vertex_elements_state = agx_create_vertex_elements;
   ctx->create_vs_state = agx_create_shader_state;
   ctx->create_gs_state = agx_create_shader_state;
   ctx->create_tcs_state = agx_create_shader_state;
   ctx->create_tes_state = agx_create_shader_state;
   ctx->create_compute_state = agx_create_compute_state;
   ctx->bind_blend_state = agx_bind_blend_state;
   ctx->bind_depth_stencil_alpha_state = agx_bind_zsa_state;
   ctx->bind_sampler_states = agx_bind_sampler_states;
   ctx->bind_fs_state = agx_bind_fs_state;
   ctx->bind_rasterizer_state = agx_bind_rasterizer_state;
   ctx->bind_vertex_elements_state = agx_bind_vertex_elements_state;
   ctx->bind_vs_state = agx_bind_vs_state;
   ctx->bind_gs_state = agx_bind_gs_state;
   ctx->bind_tcs_state = agx_bind_tcs_state;
   ctx->bind_tes_state = agx_bind_tes_state;
   ctx->bind_compute_state = agx_bind_cs_state;
   ctx->delete_blend_state = agx_delete_state;
   ctx->delete_depth_stencil_alpha_state = agx_delete_state;
   ctx->delete_fs_state = agx_delete_shader_state;
   ctx->delete_compute_state = agx_delete_shader_state;
   ctx->delete_rasterizer_state = agx_delete_state;
   ctx->delete_sampler_state = agx_delete_sampler_state;
   ctx->delete_vertex_elements_state = agx_delete_state;
   ctx->delete_vs_state = agx_delete_shader_state;
   ctx->delete_gs_state = agx_delete_shader_state;
   ctx->delete_tcs_state = agx_delete_shader_state;
   ctx->delete_tes_state = agx_delete_shader_state;
   ctx->set_blend_color = agx_set_blend_color;
   ctx->set_clip_state = agx_set_clip_state;
   ctx->set_constant_buffer = agx_set_constant_buffer;
   ctx->set_shader_buffers = agx_set_shader_buffers;
   ctx->set_shader_images = agx_set_shader_images;
   ctx->set_sampler_views = agx_set_sampler_views;
   ctx->set_framebuffer_state = agx_set_framebuffer_state;
   ctx->set_polygon_stipple = agx_set_polygon_stipple;
   ctx->set_patch_vertices = agx_set_patch_vertices;
   ctx->set_sample_mask = agx_set_sample_mask;
   ctx->set_scissor_states = agx_set_scissor_states;
   ctx->set_stencil_ref = agx_set_stencil_ref;
   ctx->set_vertex_buffers = agx_set_vertex_buffers;
   ctx->set_viewport_states = agx_set_viewport_states;
   ctx->sampler_view_destroy = agx_sampler_view_destroy;
   ctx->surface_destroy = agx_surface_destroy;
   ctx->draw_vbo = agx_draw_vbo;
   ctx->launch_grid = agx_launch_grid;
   ctx->set_global_binding = agx_set_global_binding;
   ctx->texture_barrier = agx_texture_barrier;
   ctx->get_compute_state_info = agx_get_compute_state_info;
   ctx->set_tess_state = agx_set_tess_state;
}
