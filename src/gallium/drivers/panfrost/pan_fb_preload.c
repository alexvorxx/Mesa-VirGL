/*
 * Copyright (C) 2020-2021 Collabora, Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 *   Boris Brezillon <boris.brezillon@collabora.com>
 */

#include <math.h>
#include <stdio.h>
#include "compiler/nir/nir_builder.h"
#include "util/u_math.h"
#include "pan_blend.h"
#include "pan_desc.h"
#include "pan_encoder.h"
#include "pan_fb_preload.h"
#include "pan_jc.h"
#include "pan_pool.h"
#include "pan_shader.h"
#include "pan_texture.h"

#if PAN_ARCH >= 6
/* On Midgard, the native preload infrastructure (via MFBD preloads) is broken
 * or missing in many cases. We instead use software paths as fallbacks, which
 * are done as TILER jobs. No vertex shader is necessary since we can supply
 * screen-space coordinates directly.
 *
 * This is primarily designed as a fallback for preloads but could be extended
 * for other clears/blits if needed in the future. */

static enum mali_register_file_format
nir_type_to_reg_fmt(nir_alu_type in)
{
   switch (in) {
   case nir_type_float32:
      return MALI_REGISTER_FILE_FORMAT_F32;
   case nir_type_int32:
      return MALI_REGISTER_FILE_FORMAT_I32;
   case nir_type_uint32:
      return MALI_REGISTER_FILE_FORMAT_U32;
   default:
      unreachable("Invalid type");
   }
}
#endif

/* On Valhall, the driver gives the hardware a table of resource tables.
 * Resources are addressed as the index of the table together with the index of
 * the resource within the table. For simplicity, we put one type of resource
 * in each table and fix the numbering of the tables.
 *
 * This numbering is arbitrary.
 */
enum pan_preload_resource_table {
   PAN_BLIT_TABLE_ATTRIBUTE = 0,
   PAN_BLIT_TABLE_ATTRIBUTE_BUFFER,
   PAN_BLIT_TABLE_SAMPLER,
   PAN_BLIT_TABLE_TEXTURE,

   PAN_BLIT_NUM_RESOURCE_TABLES
};

struct pan_preload_surface {
   gl_frag_result loc              : 4;
   nir_alu_type type               : 8;
   enum mali_texture_dimension dim : 2;
   bool array                      : 1;
   unsigned samples                : 5;
};

struct pan_preload_shader_key {
   struct pan_preload_surface surfaces[8];
};

struct pan_preload_shader_data {
   struct pan_preload_shader_key key;
   struct pan_shader_info info;
   mali_ptr address;
   unsigned blend_ret_offsets[8];
   nir_alu_type blend_types[8];
};

struct pan_preload_blend_shader_key {
   enum pipe_format format;
   nir_alu_type type;
   unsigned rt         : 3;
   unsigned nr_samples : 5;
   unsigned pad        : 24;
};

struct pan_preload_blend_shader_data {
   struct pan_preload_blend_shader_key key;
   mali_ptr address;
};

struct pan_preload_rsd_key {
   struct {
      enum pipe_format format;
      nir_alu_type type               : 8;
      unsigned samples                : 5;
      enum mali_texture_dimension dim : 2;
      bool array                      : 1;
   } rts[8], z, s;
};

struct pan_preload_rsd_data {
   struct pan_preload_rsd_key key;
   mali_ptr address;
};

#if PAN_ARCH >= 5
static void
pan_preload_emit_blend(unsigned rt,
                       const struct pan_image_view *iview,
                       const struct pan_preload_shader_data *preload_shader,
                       mali_ptr blend_shader, void *out)
{
   assert(blend_shader == 0 || PAN_ARCH <= 5);

   pan_pack(out, BLEND, cfg) {
      if (!iview) {
         cfg.enable = false;
#if PAN_ARCH >= 6
         cfg.internal.mode = MALI_BLEND_MODE_OFF;
#endif
         continue;
      }

      cfg.round_to_fb_precision = true;
      cfg.srgb = util_format_is_srgb(iview->format);

#if PAN_ARCH >= 6
      cfg.internal.mode = MALI_BLEND_MODE_OPAQUE;
#endif

      if (!blend_shader) {
         cfg.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
         cfg.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
         cfg.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
         cfg.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
         cfg.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
         cfg.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
         cfg.equation.color_mask = 0xf;

#if PAN_ARCH >= 6
         nir_alu_type type = preload_shader->key.surfaces[rt].type;

         cfg.internal.fixed_function.num_comps = 4;
         cfg.internal.fixed_function.conversion.memory_format = GENX(
            panfrost_dithered_format_from_pipe_format)(iview->format, false);
         cfg.internal.fixed_function.conversion.register_format =
            nir_type_to_reg_fmt(type);

         cfg.internal.fixed_function.rt = rt;
#endif
      } else {
#if PAN_ARCH <= 5
         cfg.blend_shader = true;
         cfg.shader_pc = blend_shader;
#endif
      }
   }
}
#endif

struct pan_preload_views {
   unsigned rt_count;
   const struct pan_image_view *rts[8];
   const struct pan_image_view *z;
   const struct pan_image_view *s;
};

static bool
pan_preload_is_ms(struct pan_preload_views *views)
{
   for (unsigned i = 0; i < views->rt_count; i++) {
      if (views->rts[i]) {
         if (pan_image_view_get_nr_samples(views->rts[i]) > 1)
            return true;
      }
   }

   if (views->z && pan_image_view_get_nr_samples(views->z) > 1)
      return true;

   if (views->s && pan_image_view_get_nr_samples(views->s) > 1)
      return true;

   return false;
}

#if PAN_ARCH >= 5
static void
pan_preload_emit_blends(const struct pan_preload_shader_data *preload_shader,
                        struct pan_preload_views *views,
                        mali_ptr *blend_shaders, void *out)
{
   for (unsigned i = 0; i < MAX2(views->rt_count, 1); ++i) {
      void *dest = out + pan_size(BLEND) * i;
      const struct pan_image_view *rt_view = views->rts[i];
      mali_ptr blend_shader = blend_shaders ? blend_shaders[i] : 0;

      pan_preload_emit_blend(i, rt_view, preload_shader, blend_shader, dest);
   }
}
#endif

#if PAN_ARCH <= 7
static void
pan_preload_emit_rsd(const struct pan_preload_shader_data *preload_shader,
                     struct pan_preload_views *views, mali_ptr *blend_shaders,
                     void *out)
{
   UNUSED bool zs = (views->z || views->s);
   bool ms = pan_preload_is_ms(views);

   pan_pack(out, RENDERER_STATE, cfg) {
      assert(preload_shader->address);
      pan_shader_prepare_rsd(&preload_shader->info, preload_shader->address, &cfg);

      cfg.multisample_misc.sample_mask = 0xFFFF;
      cfg.multisample_misc.multisample_enable = ms;
      cfg.multisample_misc.evaluate_per_sample = ms;
      cfg.multisample_misc.depth_write_mask = views->z != NULL;
      cfg.multisample_misc.depth_function = MALI_FUNC_ALWAYS;

      cfg.stencil_mask_misc.stencil_enable = views->s != NULL;
      cfg.stencil_mask_misc.stencil_mask_front = 0xFF;
      cfg.stencil_mask_misc.stencil_mask_back = 0xFF;
      cfg.stencil_front.compare_function = MALI_FUNC_ALWAYS;
      cfg.stencil_front.stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.stencil_front.mask = 0xFF;
      cfg.stencil_back = cfg.stencil_front;

#if PAN_ARCH >= 6
      if (zs) {
         /* Writing Z/S requires late updates */
         cfg.properties.zs_update_operation = MALI_PIXEL_KILL_FORCE_LATE;
         cfg.properties.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_LATE;
      } else {
         /* Skipping ATEST requires forcing Z/S */
         cfg.properties.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
         cfg.properties.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;
      }

      /* However, while shaders writing Z/S can normally be killed, on v6
       * for frame shaders it can cause GPU timeouts, so only allow colour
       * preload shaders to be killed. */
      cfg.properties.allow_forward_pixel_to_kill = !zs;

      if (PAN_ARCH == 6)
         cfg.properties.allow_forward_pixel_to_be_killed = !zs;
#else

      mali_ptr blend_shader =
         blend_shaders
            ? panfrost_last_nonnull(blend_shaders, MAX2(views->rt_count, 1))
            : 0;

      cfg.properties.work_register_count = 4;
      cfg.properties.force_early_z = !zs;
      cfg.stencil_mask_misc.alpha_test_compare_function = MALI_FUNC_ALWAYS;

      /* Set even on v5 for erratum workaround */
#if PAN_ARCH == 5
      cfg.legacy_blend_shader = blend_shader;
#else
      cfg.blend_shader = blend_shader;
      cfg.stencil_mask_misc.write_enable = true;
      cfg.stencil_mask_misc.dither_disable = true;
      cfg.multisample_misc.blend_shader = !!blend_shader;
      cfg.blend_shader = blend_shader;
      if (!cfg.multisample_misc.blend_shader) {
         cfg.blend_equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
         cfg.blend_equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
         cfg.blend_equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
         cfg.blend_equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
         cfg.blend_equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
         cfg.blend_equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
         cfg.blend_constant = 0;

         if (views->rts[0] != NULL) {
            cfg.stencil_mask_misc.srgb =
               util_format_is_srgb(views->rts[0]->format);
            cfg.blend_equation.color_mask = 0xf;
         }
      }
#endif
#endif
   }

#if PAN_ARCH >= 5
   pan_preload_emit_blends(preload_shader, views, blend_shaders,
                           out + pan_size(RENDERER_STATE));
#endif
}
#endif

#if PAN_ARCH <= 5
static void
pan_preload_get_blend_shaders(struct pan_fb_preload_cache *cache,
                              unsigned rt_count,
                              const struct pan_image_view **rts,
                              const struct pan_preload_shader_data *preload_shader,
                              mali_ptr *blend_shaders)
{
   if (!rt_count)
      return;

   struct pan_blend_state blend_state = {
      .rt_count = rt_count,
   };

   for (unsigned i = 0; i < rt_count; i++) {
      if (!rts[i] || panfrost_blendable_formats_v7[rts[i]->format].internal)
         continue;

      struct pan_preload_blend_shader_key key = {
         .format = rts[i]->format,
         .rt = i,
         .nr_samples = pan_image_view_get_nr_samples(rts[i]),
         .type = preload_shader->blend_types[i],
      };

      pthread_mutex_lock(&cache->shaders.lock);
      struct hash_entry *he =
         _mesa_hash_table_search(cache->shaders.blend, &key);
      struct pan_preload_blend_shader_data *blend_shader = he ? he->data : NULL;
      if (blend_shader) {
         blend_shaders[i] = blend_shader->address;
         pthread_mutex_unlock(&cache->shaders.lock);
         continue;
      }

      blend_shader =
         rzalloc(cache->shaders.blend, struct pan_preload_blend_shader_data);
      blend_shader->key = key;

      blend_state.rts[i] = (struct pan_blend_rt_state){
         .format = rts[i]->format,
         .nr_samples = pan_image_view_get_nr_samples(rts[i]),
         .equation =
            {
               .blend_enable = false,
               .color_mask = 0xf,
            },
      };

      pthread_mutex_lock(&cache->blend_shader_cache->lock);
      struct pan_blend_shader_variant *b = GENX(pan_blend_get_shader_locked)(
         cache->blend_shader_cache, &blend_state,
         preload_shader->blend_types[i], nir_type_float32, /* unused */
         i);

      assert(b->work_reg_count <= 4);
      struct panfrost_ptr bin =
         pan_pool_alloc_aligned(cache->shaders.pool, b->binary.size, 64);
      memcpy(bin.cpu, b->binary.data, b->binary.size);

      blend_shader->address = bin.gpu | b->first_tag;
      pthread_mutex_unlock(&cache->blend_shader_cache->lock);
      _mesa_hash_table_insert(cache->shaders.blend, &blend_shader->key,
                              blend_shader);
      pthread_mutex_unlock(&cache->shaders.lock);
      blend_shaders[i] = blend_shader->address;
   }
}
#endif

/*
 * Early Mali GPUs did not respect sampler LOD clamps or bias, so the Midgard
 * compiler inserts lowering code with a load_sampler_lod_parameters_pan sysval
 * that we need to lower. Our samplers do not use LOD clamps or bias, so we
 * lower to the identity settings and let constant folding get rid of the
 * unnecessary lowering.
 */
static bool
lower_sampler_parameters(nir_builder *b, nir_intrinsic_instr *intr,
                         UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_sampler_lod_parameters_pan)
      return false;

   const nir_const_value constants[4] = {
      nir_const_value_for_float(0.0f, 32),     /* min_lod */
      nir_const_value_for_float(INFINITY, 32), /* max_lod */
      nir_const_value_for_float(0.0f, 32),     /* lod_bias */
   };

   b->cursor = nir_after_instr(&intr->instr);
   nir_def_rewrite_uses(&intr->def, nir_build_imm(b, 3, 32, constants));
   return true;
}

static uint32_t
sampler_hw_index(uint32_t index)
{
   return PAN_ARCH >= 9 ? pan_res_handle(PAN_BLIT_TABLE_SAMPLER, index) : index;
}

static uint32_t
tex_hw_index(uint32_t index)
{
   return PAN_ARCH >= 9 ? pan_res_handle(PAN_BLIT_TABLE_TEXTURE, index) : index;
}

static uint32_t
attr_hw_index(uint32_t index)
{
   return PAN_ARCH >= 9 ? pan_res_handle(PAN_BLIT_TABLE_ATTRIBUTE, index)
                        : index;
}

static const struct pan_preload_shader_data *
pan_preload_get_shader(struct pan_fb_preload_cache *cache,
                       const struct pan_preload_shader_key *key)
{
   pthread_mutex_lock(&cache->shaders.lock);
   struct hash_entry *he =
      _mesa_hash_table_search(cache->shaders.preload, key);
   struct pan_preload_shader_data *shader = he ? he->data : NULL;

   if (shader)
      goto out;

   unsigned coord_comps = 0;
   unsigned sig_offset = 0;
   char sig[256];
   bool first = true;
   for (unsigned i = 0; i < ARRAY_SIZE(key->surfaces); i++) {
      const char *type_str, *dim_str;
      if (key->surfaces[i].type == nir_type_invalid)
         continue;

      switch (key->surfaces[i].type) {
      case nir_type_float32:
         type_str = "float";
         break;
      case nir_type_uint32:
         type_str = "uint";
         break;
      case nir_type_int32:
         type_str = "int";
         break;
      default:
         unreachable("Invalid type\n");
      }

      switch (key->surfaces[i].dim) {
      case MALI_TEXTURE_DIMENSION_CUBE:
         dim_str = "cube";
         break;
      case MALI_TEXTURE_DIMENSION_1D:
         dim_str = "1D";
         break;
      case MALI_TEXTURE_DIMENSION_2D:
         dim_str = "2D";
         break;
      case MALI_TEXTURE_DIMENSION_3D:
         dim_str = "3D";
         break;
      default:
         unreachable("Invalid dim\n");
      }

      coord_comps = MAX2(coord_comps, (key->surfaces[i].dim ?: 3) +
                                         (key->surfaces[i].array ? 1 : 0));

      if (sig_offset >= sizeof(sig)) {
         first = false;
         continue;
      }

      sig_offset +=
         snprintf(sig + sig_offset, sizeof(sig) - sig_offset,
                  "%s[%s;%s;%s%s;samples=%d]",
                  first ? "" : ",", gl_frag_result_name(key->surfaces[i].loc),
                  type_str, dim_str, key->surfaces[i].array ? "[]" : "",
                  key->surfaces[i].samples);

      first = false;
   }

   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, GENX(pan_shader_get_compiler_options)(),
      "pan_preload(%s)", sig);

   nir_def *barycentric = nir_load_barycentric(
      &b, nir_intrinsic_load_barycentric_pixel, INTERP_MODE_SMOOTH);
   nir_def *coord = nir_load_interpolated_input(
      &b, coord_comps, 32, barycentric, nir_imm_int(&b, 0),
      .base = attr_hw_index(0), .dest_type = nir_type_float32,
      .io_semantics.location = VARYING_SLOT_VAR0, .io_semantics.num_slots = 1);

   unsigned active_count = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(key->surfaces); i++) {
      if (key->surfaces[i].type == nir_type_invalid)
         continue;

      bool ms = key->surfaces[i].samples > 1;
      enum glsl_sampler_dim sampler_dim;

      switch (key->surfaces[i].dim) {
      case MALI_TEXTURE_DIMENSION_1D:
         sampler_dim = GLSL_SAMPLER_DIM_1D;
         break;
      case MALI_TEXTURE_DIMENSION_2D:
         sampler_dim = ms ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D;
         break;
      case MALI_TEXTURE_DIMENSION_3D:
         sampler_dim = GLSL_SAMPLER_DIM_3D;
         break;
      case MALI_TEXTURE_DIMENSION_CUBE:
         sampler_dim = GLSL_SAMPLER_DIM_CUBE;
         break;
      }


      nir_tex_instr *tex = nir_tex_instr_create(b.shader, ms ? 3 : 1);

      tex->dest_type = key->surfaces[i].type;
      tex->texture_index = tex_hw_index(active_count);
      tex->sampler_index = sampler_hw_index(0);
      tex->is_array = key->surfaces[i].array;
      tex->sampler_dim = sampler_dim;

      if (ms) {
         tex->op = nir_texop_txf_ms;

         tex->src[0] =
            nir_tex_src_for_ssa(nir_tex_src_coord, nir_f2i32(&b, coord));
         tex->coord_components = coord_comps;

         tex->src[1] =
            nir_tex_src_for_ssa(nir_tex_src_ms_index, nir_load_sample_id(&b));

         tex->src[2] = nir_tex_src_for_ssa(nir_tex_src_lod, nir_imm_int(&b, 0));
      } else {
         tex->op = nir_texop_txl;

         tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, coord);
         tex->coord_components = coord_comps;
      }

      nir_def_init(&tex->instr, &tex->def, 4, 32);
      nir_builder_instr_insert(&b, &tex->instr);

      nir_def *res = &tex->def;

      if (key->surfaces[i].loc >= FRAG_RESULT_DATA0) {
         nir_store_output(
            &b, res, nir_imm_int(&b, 0), .base = active_count,
            .src_type = key->surfaces[i].type,
            .io_semantics.location = key->surfaces[i].loc,
            .io_semantics.num_slots = 1,
            .write_mask = nir_component_mask(res->num_components));
      } else {
         unsigned c = key->surfaces[i].loc == FRAG_RESULT_STENCIL ? 1 : 0;
         nir_store_output(
            &b, nir_channel(&b, res, c), nir_imm_int(&b, 0),
            .base = active_count, .src_type = key->surfaces[i].type,
            .io_semantics.location = key->surfaces[i].loc,
            .io_semantics.num_slots = 1, .write_mask = nir_component_mask(1));
      }
      active_count++;
   }

   struct panfrost_compile_inputs inputs = {
      .gpu_id = cache->gpu_id,
      .is_blit = true,
      .no_idvs = true,
   };
   struct util_dynarray binary;

   util_dynarray_init(&binary, NULL);

   shader = rzalloc(cache->shaders.preload, struct pan_preload_shader_data);

   nir_shader_gather_info(b.shader, nir_shader_get_entrypoint(b.shader));

   for (unsigned i = 0; i < active_count; ++i)
      BITSET_SET(b.shader->info.textures_used, i);

   pan_shader_preprocess(b.shader, inputs.gpu_id);

   if (PAN_ARCH == 4) {
      NIR_PASS_V(b.shader, nir_shader_intrinsics_pass, lower_sampler_parameters,
                 nir_metadata_control_flow, NULL);
   }

   GENX(pan_shader_compile)(b.shader, &inputs, &binary, &shader->info);

   shader->key = *key;
   shader->address =
      pan_pool_upload_aligned(cache->shaders.pool, binary.data,
                              binary.size, PAN_ARCH >= 6 ? 128 : 64);

   util_dynarray_fini(&binary);
   ralloc_free(b.shader);

#if PAN_ARCH >= 6
   for (unsigned i = 0; i < ARRAY_SIZE(shader->blend_ret_offsets); i++) {
      shader->blend_ret_offsets[i] =
         shader->info.bifrost.blend[i].return_offset;
      shader->blend_types[i] = shader->info.bifrost.blend[i].type;
   }
#endif

   _mesa_hash_table_insert(cache->shaders.preload, &shader->key, shader);

out:
   pthread_mutex_unlock(&cache->shaders.lock);
   return shader;
}

static struct pan_preload_shader_key
pan_preload_get_key(struct pan_preload_views *views)
{
   struct pan_preload_shader_key key = {0};

   if (views->z) {
      key.surfaces[0].loc = FRAG_RESULT_DEPTH;
      key.surfaces[0].type = nir_type_float32;
      key.surfaces[0].samples = pan_image_view_get_nr_samples(views->z);
      key.surfaces[0].dim = views->z->dim;
      key.surfaces[0].array = views->z->first_layer != views->z->last_layer;
   }

   if (views->s) {
      key.surfaces[1].loc = FRAG_RESULT_STENCIL;
      key.surfaces[1].type = nir_type_uint32;
      key.surfaces[1].samples = pan_image_view_get_nr_samples(views->s);
      key.surfaces[1].dim = views->s->dim;
      key.surfaces[1].array = views->s->first_layer != views->s->last_layer;
   }

   for (unsigned i = 0; i < views->rt_count; i++) {
      if (!views->rts[i])
         continue;

      key.surfaces[i].loc = FRAG_RESULT_DATA0 + i;
      key.surfaces[i].type =
         util_format_is_pure_uint(views->rts[i]->format) ? nir_type_uint32
         : util_format_is_pure_sint(views->rts[i]->format)
            ? nir_type_int32
            : nir_type_float32;
      key.surfaces[i].samples =
         pan_image_view_get_nr_samples(views->rts[i]);
      key.surfaces[i].dim = views->rts[i]->dim;
      key.surfaces[i].array =
         views->rts[i]->first_layer != views->rts[i]->last_layer;
   }

   return key;
}

#if PAN_ARCH <= 7
static mali_ptr
pan_preload_get_rsd(struct pan_fb_preload_cache *cache,
                    struct pan_preload_views *views)
{
   struct pan_preload_rsd_key rsd_key = {0};

   assert(!views->rt_count || (!views->z && !views->s));

   struct pan_preload_shader_key preload_key = pan_preload_get_key(views);

   if (views->z) {
      rsd_key.z.format = views->z->format;
      rsd_key.z.type = preload_key.surfaces[0].type;
      rsd_key.z.samples = preload_key.surfaces[0].samples;
      rsd_key.z.dim = preload_key.surfaces[0].dim;
      rsd_key.z.array = preload_key.surfaces[0].array;
   }

   if (views->s) {
      rsd_key.s.format = views->s->format;
      rsd_key.s.type = preload_key.surfaces[1].type;
      rsd_key.s.samples = preload_key.surfaces[1].samples;
      rsd_key.s.dim = preload_key.surfaces[1].dim;
      rsd_key.s.array = preload_key.surfaces[1].array;
   }

   for (unsigned i = 0; i < views->rt_count; i++) {
      if (!views->rts[i])
         continue;

      rsd_key.rts[i].format = views->rts[i]->format;
      rsd_key.rts[i].type = preload_key.surfaces[i].type;
      rsd_key.rts[i].samples = preload_key.surfaces[i].samples;
      rsd_key.rts[i].dim = preload_key.surfaces[i].dim;
      rsd_key.rts[i].array = preload_key.surfaces[i].array;
   }

   pthread_mutex_lock(&cache->rsds.lock);
   struct hash_entry *he =
      _mesa_hash_table_search(cache->rsds.rsds, &rsd_key);
   struct pan_preload_rsd_data *rsd = he ? he->data : NULL;
   if (rsd)
      goto out;

   rsd = rzalloc(cache->rsds.rsds, struct pan_preload_rsd_data);
   rsd->key = rsd_key;

#if PAN_ARCH == 4
   struct panfrost_ptr rsd_ptr =
      pan_pool_alloc_desc(cache->rsds.pool, RENDERER_STATE);
#else
   unsigned bd_count = PAN_ARCH >= 5 ? MAX2(views->rt_count, 1) : 0;
   struct panfrost_ptr rsd_ptr = pan_pool_alloc_desc_aggregate(
      cache->rsds.pool, PAN_DESC(RENDERER_STATE),
      PAN_DESC_ARRAY(bd_count, BLEND));
#endif

   mali_ptr blend_shaders[8] = {0};

   const struct pan_preload_shader_data *preload_shader =
      pan_preload_get_shader(cache, &preload_key);

#if PAN_ARCH <= 5
   pan_preload_get_blend_shaders(cache,
                                 views->rt_count, views->rts, preload_shader,
                                 blend_shaders);
#endif

   pan_preload_emit_rsd(preload_shader, views, blend_shaders, rsd_ptr.cpu);
   rsd->address = rsd_ptr.gpu;
   _mesa_hash_table_insert(cache->rsds.rsds, &rsd->key, rsd);

out:
   pthread_mutex_unlock(&cache->rsds.lock);
   return rsd->address;
}
#endif

static struct pan_preload_views
pan_preload_get_views(const struct pan_fb_info *fb, bool zs,
                      struct pan_image_view *patched_s)
{
   struct pan_preload_views views = {0};

   if (zs) {
      if (fb->zs.preload.z)
         views.z = fb->zs.view.zs;

      if (fb->zs.preload.s) {
         const struct pan_image_view *view = fb->zs.view.s ?: fb->zs.view.zs;
         enum pipe_format fmt = util_format_get_depth_only(view->format);

         switch (view->format) {
         case PIPE_FORMAT_Z24_UNORM_S8_UINT:
            fmt = PIPE_FORMAT_X24S8_UINT;
            break;
         case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
            fmt = PIPE_FORMAT_X32_S8X24_UINT;
            break;
         default:
            fmt = view->format;
            break;
         }

         if (fmt != view->format) {
            *patched_s = *view;
            patched_s->format = fmt;
            views.s = patched_s;
         } else {
            views.s = view;
         }
      }
   } else {
      for (unsigned i = 0; i < fb->rt_count; i++) {
         if (fb->rts[i].preload)
            views.rts[i] = fb->rts[i].view;
      }

      views.rt_count = fb->rt_count;
   }

   return views;
}

static bool
pan_preload_needed(const struct pan_fb_info *fb, bool zs)
{
   if (zs) {
      if (fb->zs.preload.z || fb->zs.preload.s)
         return true;
   } else {
      for (unsigned i = 0; i < fb->rt_count; i++) {
         if (fb->rts[i].preload)
            return true;
      }
   }

   return false;
}

static mali_ptr
pan_preload_emit_varying(struct pan_pool *pool)
{
   struct panfrost_ptr varying = pan_pool_alloc_desc(pool, ATTRIBUTE);

   pan_pack(varying.cpu, ATTRIBUTE, cfg) {
      cfg.buffer_index = 0;
      cfg.offset_enable = PAN_ARCH <= 5;
      cfg.format =
         GENX(panfrost_format_from_pipe_format)(PIPE_FORMAT_R32G32B32_FLOAT)->hw;

#if PAN_ARCH >= 9
      cfg.attribute_type = MALI_ATTRIBUTE_TYPE_1D;
      cfg.table = PAN_BLIT_TABLE_ATTRIBUTE_BUFFER;
      cfg.frequency = MALI_ATTRIBUTE_FREQUENCY_VERTEX;
      cfg.stride = 4 * sizeof(float);
#endif
   }

   return varying.gpu;
}

static mali_ptr
pan_preload_emit_varying_buffer(struct pan_pool *pool, mali_ptr coordinates)
{
#if PAN_ARCH >= 9
   struct panfrost_ptr varying_buffer = pan_pool_alloc_desc(pool, BUFFER);

   pan_pack(varying_buffer.cpu, BUFFER, cfg) {
      cfg.address = coordinates;
      cfg.size = 4 * sizeof(float) * 4;
   }
#else
   /* Bifrost needs an empty desc to mark end of prefetching */
   bool padding_buffer = PAN_ARCH >= 6;

   struct panfrost_ptr varying_buffer = pan_pool_alloc_desc_array(
      pool, (padding_buffer ? 2 : 1), ATTRIBUTE_BUFFER);

   pan_pack(varying_buffer.cpu, ATTRIBUTE_BUFFER, cfg) {
      cfg.pointer = coordinates;
      cfg.stride = 4 * sizeof(float);
      cfg.size = cfg.stride * 4;
   }

   if (padding_buffer) {
      pan_pack(varying_buffer.cpu + pan_size(ATTRIBUTE_BUFFER),
               ATTRIBUTE_BUFFER, cfg)
         ;
   }
#endif

   return varying_buffer.gpu;
}

static mali_ptr
pan_preload_emit_sampler(struct pan_pool *pool, bool nearest_filter)
{
   struct panfrost_ptr sampler = pan_pool_alloc_desc(pool, SAMPLER);

   pan_pack(sampler.cpu, SAMPLER, cfg) {
      cfg.seamless_cube_map = false;
      cfg.normalized_coordinates = false;
      cfg.minify_nearest = nearest_filter;
      cfg.magnify_nearest = nearest_filter;
   }

   return sampler.gpu;
}

static mali_ptr
pan_preload_emit_textures(struct pan_pool *pool, const struct pan_fb_info *fb,
                          bool zs, unsigned *tex_count_out)
{
   const struct pan_image_view *views[8];
   struct pan_image_view patched_s_view;
   unsigned tex_count = 0;

   if (zs) {
      if (fb->zs.preload.z)
         views[tex_count++] = fb->zs.view.zs;

      if (fb->zs.preload.s) {
         const struct pan_image_view *view = fb->zs.view.s ?: fb->zs.view.zs;
         enum pipe_format fmt = util_format_get_depth_only(view->format);

         switch (view->format) {
         case PIPE_FORMAT_Z24_UNORM_S8_UINT:
            fmt = PIPE_FORMAT_X24S8_UINT;
            break;
         case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
            fmt = PIPE_FORMAT_X32_S8X24_UINT;
            break;
         default:
            fmt = view->format;
            break;
         }

         if (fmt != view->format) {
            patched_s_view = *view;
            patched_s_view.format = fmt;
            view = &patched_s_view;
         }
         views[tex_count++] = view;
      }
   } else {
      for (unsigned i = 0; i < fb->rt_count; i++) {
         if (fb->rts[i].preload)
            views[tex_count++] = fb->rts[i].view;
      }
   }

   *tex_count_out = tex_count;

#if PAN_ARCH >= 6
   struct panfrost_ptr textures =
      pan_pool_alloc_desc_array(pool, tex_count, TEXTURE);

   for (unsigned i = 0; i < tex_count; i++) {
      void *texture = textures.cpu + (pan_size(TEXTURE) * i);
      size_t payload_size =
         GENX(panfrost_estimate_texture_payload_size)(views[i]);
      struct panfrost_ptr surfaces =
         pan_pool_alloc_aligned(pool, payload_size, 64);

      GENX(panfrost_new_texture)(views[i], texture, &surfaces);
   }

   return textures.gpu;
#else
   mali_ptr textures[8] = {0};

   for (unsigned i = 0; i < tex_count; i++) {
      size_t sz = pan_size(TEXTURE) +
                  GENX(panfrost_estimate_texture_payload_size)(views[i]);
      struct panfrost_ptr texture =
         pan_pool_alloc_aligned(pool, sz, pan_alignment(TEXTURE));
      struct panfrost_ptr surfaces = {
         .cpu = texture.cpu + pan_size(TEXTURE),
         .gpu = texture.gpu + pan_size(TEXTURE),
      };

      GENX(panfrost_new_texture)(views[i], texture.cpu, &surfaces);
      textures[i] = texture.gpu;
   }

   return pan_pool_upload_aligned(pool, textures, tex_count * sizeof(mali_ptr),
                                  sizeof(mali_ptr));
#endif
}

#if PAN_ARCH >= 8
/* TODO: cache */
static mali_ptr
pan_preload_emit_zs(struct pan_pool *pool, bool z, bool s)
{
   struct panfrost_ptr zsd = pan_pool_alloc_desc(pool, DEPTH_STENCIL);

   pan_pack(zsd.cpu, DEPTH_STENCIL, cfg) {
      cfg.depth_function = MALI_FUNC_ALWAYS;
      cfg.depth_write_enable = z;

      if (z)
         cfg.depth_source = MALI_DEPTH_SOURCE_SHADER;

      cfg.stencil_test_enable = s;
      cfg.stencil_from_shader = s;

      cfg.front_compare_function = MALI_FUNC_ALWAYS;
      cfg.front_stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.front_depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.front_depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.front_write_mask = 0xFF;
      cfg.front_value_mask = 0xFF;

      cfg.back_compare_function = MALI_FUNC_ALWAYS;
      cfg.back_stencil_fail = MALI_STENCIL_OP_REPLACE;
      cfg.back_depth_fail = MALI_STENCIL_OP_REPLACE;
      cfg.back_depth_pass = MALI_STENCIL_OP_REPLACE;
      cfg.back_write_mask = 0xFF;
      cfg.back_value_mask = 0xFF;

      cfg.depth_cull_enable = false;
   }

   return zsd.gpu;
}
#else
static mali_ptr
pan_preload_emit_viewport(struct pan_pool *pool, uint16_t minx, uint16_t miny,
                          uint16_t maxx, uint16_t maxy)
{
   struct panfrost_ptr vp = pan_pool_alloc_desc(pool, VIEWPORT);

   pan_pack(vp.cpu, VIEWPORT, cfg) {
      cfg.scissor_minimum_x = minx;
      cfg.scissor_minimum_y = miny;
      cfg.scissor_maximum_x = maxx;
      cfg.scissor_maximum_y = maxy;
   }

   return vp.gpu;
}
#endif

static void
pan_preload_emit_dcd(struct pan_fb_preload_cache *cache,
                     struct pan_pool *pool, struct pan_fb_info *fb, bool zs,
                     mali_ptr coordinates, mali_ptr tsd, void *out,
                     bool always_write)
{
   unsigned tex_count = 0;
   mali_ptr textures = pan_preload_emit_textures(pool, fb, zs, &tex_count);
   mali_ptr samplers = pan_preload_emit_sampler(pool, true);
   mali_ptr varyings = pan_preload_emit_varying(pool);
   mali_ptr varying_buffers =
      pan_preload_emit_varying_buffer(pool, coordinates);

   /* Tiles updated by preload shaders are still considered clean (separate
    * for colour and Z/S), allowing us to suppress unnecessary writeback
    */
   UNUSED bool clean_fragment_write = !always_write;

   /* Image view used when patching stencil formats for combined
    * depth/stencil preloads.
    */
   struct pan_image_view patched_s;

   struct pan_preload_views views = pan_preload_get_views(fb, zs, &patched_s);

#if PAN_ARCH <= 7
   pan_pack(out, DRAW, cfg) {
      uint16_t minx = 0, miny = 0, maxx, maxy;

      if (PAN_ARCH == 4) {
         maxx = fb->width - 1;
         maxy = fb->height - 1;
      } else {
         /* Align on 32x32 tiles */
         minx = fb->extent.minx & ~31;
         miny = fb->extent.miny & ~31;
         maxx = MIN2(ALIGN_POT(fb->extent.maxx + 1, 32), fb->width) - 1;
         maxy = MIN2(ALIGN_POT(fb->extent.maxy + 1, 32), fb->height) - 1;
      }

      cfg.thread_storage = tsd;
      cfg.state = pan_preload_get_rsd(cache, &views);

      cfg.position = coordinates;
      cfg.viewport = pan_preload_emit_viewport(pool, minx, miny, maxx, maxy);

      cfg.varyings = varyings;
      cfg.varying_buffers = varying_buffers;
      cfg.textures = textures;
      cfg.samplers = samplers;

#if PAN_ARCH >= 6
      cfg.clean_fragment_write = clean_fragment_write;
#endif
   }
#else
   struct panfrost_ptr T;
   unsigned nr_tables = PAN_BLIT_NUM_RESOURCE_TABLES;

   /* Although individual resources need only 16 byte alignment, the
    * resource table as a whole must be 64-byte aligned.
    */
   T = pan_pool_alloc_aligned(pool, nr_tables * pan_size(RESOURCE), 64);
   memset(T.cpu, 0, nr_tables * pan_size(RESOURCE));

   panfrost_make_resource_table(T, PAN_BLIT_TABLE_TEXTURE, textures, tex_count);
   panfrost_make_resource_table(T, PAN_BLIT_TABLE_SAMPLER, samplers, 1);
   panfrost_make_resource_table(T, PAN_BLIT_TABLE_ATTRIBUTE, varyings, 1);
   panfrost_make_resource_table(T, PAN_BLIT_TABLE_ATTRIBUTE_BUFFER,
                                varying_buffers, 1);

   struct pan_preload_shader_key key = pan_preload_get_key(&views);
   const struct pan_preload_shader_data *preload_shader =
      pan_preload_get_shader(cache, &key);

   bool z = fb->zs.preload.z;
   bool s = fb->zs.preload.s;
   bool ms = pan_preload_is_ms(&views);

   struct panfrost_ptr spd = pan_pool_alloc_desc(pool, SHADER_PROGRAM);
   pan_pack(spd.cpu, SHADER_PROGRAM, cfg) {
      cfg.stage = MALI_SHADER_STAGE_FRAGMENT;
      cfg.fragment_coverage_bitmask_type = MALI_COVERAGE_BITMASK_TYPE_GL;
      cfg.register_allocation = MALI_SHADER_REGISTER_ALLOCATION_32_PER_THREAD;
      cfg.binary = preload_shader->address;
      cfg.preload.r48_r63 = preload_shader->info.preload >> 48;
   }

   unsigned bd_count = views.rt_count;
   struct panfrost_ptr blend = pan_pool_alloc_desc_array(pool, bd_count, BLEND);

   if (!zs) {
      pan_preload_emit_blends(preload_shader, &views, NULL, blend.cpu);
   }

   pan_pack(out, DRAW, cfg) {
      if (zs) {
         /* ZS_EMIT requires late update/kill */
         cfg.zs_update_operation = MALI_PIXEL_KILL_FORCE_LATE;
         cfg.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_LATE;
         cfg.blend_count = 0;
      } else {
         /* Skipping ATEST requires forcing Z/S */
         cfg.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
         cfg.pixel_kill_operation = MALI_PIXEL_KILL_FORCE_EARLY;

         cfg.blend = blend.gpu;
         cfg.blend_count = bd_count;
         cfg.render_target_mask = 0x1;
      }

      cfg.allow_forward_pixel_to_kill = !zs;
      cfg.allow_forward_pixel_to_be_killed = true;
      cfg.depth_stencil = pan_preload_emit_zs(pool, z, s);
      cfg.sample_mask = 0xFFFF;
      cfg.multisample_enable = ms;
      cfg.evaluate_per_sample = ms;
      cfg.maximum_z = 1.0;
      cfg.clean_fragment_write = clean_fragment_write;
      cfg.shader.resources = T.gpu | nr_tables;
      cfg.shader.shader = spd.gpu;
      cfg.shader.thread_storage = tsd;
   }
#endif
}

#if PAN_ARCH >= 6
static void
pan_preload_fb_alloc_pre_post_dcds(struct pan_pool *desc_pool,
                                   struct pan_fb_info *fb)
{
   if (fb->bifrost.pre_post.dcds.gpu)
      return;

   fb->bifrost.pre_post.dcds = pan_pool_alloc_desc_array(desc_pool, 3, DRAW);
}

static void
pan_preload_emit_pre_frame_dcd(struct pan_fb_preload_cache *cache,
                               struct pan_pool *desc_pool,
                               struct pan_fb_info *fb, bool zs, mali_ptr coords,
                               mali_ptr tsd)
{
   unsigned dcd_idx = zs ? 1 : 0;
   pan_preload_fb_alloc_pre_post_dcds(desc_pool, fb);
   assert(fb->bifrost.pre_post.dcds.cpu);
   void *dcd = fb->bifrost.pre_post.dcds.cpu + (dcd_idx * pan_size(DRAW));

   /* We only use crc_rt to determine whether to force writes for updating
    * the CRCs, so use a conservative tile size (16x16).
    */
   int crc_rt = GENX(pan_select_crc_rt)(fb, 16 * 16);

   bool always_write = false;

   /* If CRC data is currently invalid and this batch will make it valid,
    * write even clean tiles to make sure CRC data is updated. */
   if (crc_rt >= 0) {
      bool *valid = fb->rts[crc_rt].crc_valid;
      bool full = !fb->extent.minx && !fb->extent.miny &&
                  fb->extent.maxx == (fb->width - 1) &&
                  fb->extent.maxy == (fb->height - 1);

      if (full && !(*valid))
         always_write = true;
   }

   pan_preload_emit_dcd(cache, desc_pool, fb, zs, coords, tsd, dcd,
                        always_write);
   if (zs) {
      enum pipe_format fmt = fb->zs.view.zs
                                ? fb->zs.view.zs->planes[0]->layout.format
                                : fb->zs.view.s->planes[0]->layout.format;
      bool always = false;

      /* If we're dealing with a combined ZS resource and only one
       * component is cleared, we need to reload the whole surface
       * because the zs_clean_pixel_write_enable flag is set in that
       * case.
       */
      if (util_format_is_depth_and_stencil(fmt) &&
          fb->zs.clear.z != fb->zs.clear.s)
         always = true;

      /* We could use INTERSECT on Bifrost v7 too, but
       * EARLY_ZS_ALWAYS has the advantage of reloading the ZS tile
       * buffer one or more tiles ahead, making ZS data immediately
       * available for any ZS tests taking place in other shaders.
       * Thing's haven't been benchmarked to determine what's
       * preferable (saving bandwidth vs having ZS preloaded
       * earlier), so let's leave it like that for now.
       */
      fb->bifrost.pre_post.modes[dcd_idx] =
         PAN_ARCH > 6
            ? MALI_PRE_POST_FRAME_SHADER_MODE_EARLY_ZS_ALWAYS
         : always ? MALI_PRE_POST_FRAME_SHADER_MODE_ALWAYS
                  : MALI_PRE_POST_FRAME_SHADER_MODE_INTERSECT;
   } else {
      fb->bifrost.pre_post.modes[dcd_idx] =
         always_write ? MALI_PRE_POST_FRAME_SHADER_MODE_ALWAYS
                      : MALI_PRE_POST_FRAME_SHADER_MODE_INTERSECT;
   }
}
#else
static struct panfrost_ptr
pan_preload_emit_tiler_job(struct pan_fb_preload_cache *cache, struct pan_pool *desc_pool,
                           struct pan_fb_info *fb, bool zs, mali_ptr coords,
                           mali_ptr tsd)
{
   struct panfrost_ptr job = pan_pool_alloc_desc(desc_pool, TILER_JOB);

   pan_preload_emit_dcd(cache, desc_pool, fb, zs, coords, tsd,
                        pan_section_ptr(job.cpu, TILER_JOB, DRAW), false);

   pan_section_pack(job.cpu, TILER_JOB, PRIMITIVE, cfg) {
      cfg.draw_mode = MALI_DRAW_MODE_TRIANGLE_STRIP;
      cfg.index_count = 4;
      cfg.job_task_split = 6;
   }

   pan_section_pack(job.cpu, TILER_JOB, PRIMITIVE_SIZE, cfg) {
      cfg.constant = 1.0f;
   }

   void *invoc = pan_section_ptr(job.cpu, TILER_JOB, INVOCATION);
   panfrost_pack_work_groups_compute(invoc, 1, 4, 1, 1, 1, 1, true, false);

   return job;
}
#endif

static struct panfrost_ptr
pan_preload_fb_part(struct pan_fb_preload_cache *cache, struct pan_pool *pool,
                    struct pan_fb_info *fb, bool zs, mali_ptr coords,
                    mali_ptr tsd)
{
   struct panfrost_ptr job = {0};

#if PAN_ARCH >= 6
   pan_preload_emit_pre_frame_dcd(cache, pool, fb, zs, coords, tsd);
#else
   job = pan_preload_emit_tiler_job(cache, pool, fb, zs, coords, tsd);
#endif
   return job;
}

unsigned
GENX(pan_preload_fb)(struct pan_fb_preload_cache *cache, struct pan_pool *pool,
                     struct pan_fb_info *fb, mali_ptr tsd,
                     struct panfrost_ptr *jobs)
{
   bool preload_zs = pan_preload_needed(fb, true);
   bool preload_rts = pan_preload_needed(fb, false);
   mali_ptr coords;

   if (!preload_zs && !preload_rts)
      return 0;

   float rect[] = {
      0.0,       0.0,        0, 1.0,
      fb->width, 0.0,        0, 1.0,
      0.0,       fb->height, 0, 1.0,
      fb->width, fb->height, 0, 1.0,
   };

   coords = pan_pool_upload_aligned(pool, rect, sizeof(rect), 64);

   unsigned njobs = 0;
   if (preload_zs) {
      struct panfrost_ptr job =
         pan_preload_fb_part(cache, pool, fb, true, coords, tsd);
      if (jobs && job.cpu)
         jobs[njobs++] = job;
   }

   if (preload_rts) {
      struct panfrost_ptr job =
         pan_preload_fb_part(cache, pool, fb, false, coords, tsd);
      if (jobs && job.cpu)
         jobs[njobs++] = job;
   }

   return njobs;
}

DERIVE_HASH_TABLE(pan_preload_shader_key);
DERIVE_HASH_TABLE(pan_preload_blend_shader_key);
DERIVE_HASH_TABLE(pan_preload_rsd_key);

static void
pan_preload_prefill_preload_shader_cache(struct pan_fb_preload_cache *cache)
{
   static const struct pan_preload_shader_key prefill[] = {
      {
         .surfaces[0] =
            {
               .loc = FRAG_RESULT_DEPTH,
               .type = nir_type_float32,
               .dim = MALI_TEXTURE_DIMENSION_2D,
               .samples = 1,
            },
      },
      {
         .surfaces[1] =
            {
               .loc = FRAG_RESULT_STENCIL,
               .type = nir_type_uint32,
               .dim = MALI_TEXTURE_DIMENSION_2D,
               .samples = 1,
            },
      },
      {
         .surfaces[0] =
            {
               .loc = FRAG_RESULT_DATA0,
               .type = nir_type_float32,
               .dim = MALI_TEXTURE_DIMENSION_2D,
               .samples = 1,
            },
      },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(prefill); i++)
      pan_preload_get_shader(cache, &prefill[i]);
}

void
GENX(pan_fb_preload_cache_init)(
   struct pan_fb_preload_cache *cache, unsigned gpu_id,
   struct pan_blend_shader_cache *blend_shader_cache, struct pan_pool *bin_pool,
   struct pan_pool *desc_pool)
{
   cache->gpu_id = gpu_id;
   cache->shaders.preload = pan_preload_shader_key_table_create(NULL);
   cache->shaders.blend = pan_preload_blend_shader_key_table_create(NULL);
   cache->shaders.pool = bin_pool;
   pthread_mutex_init(&cache->shaders.lock, NULL);
   pan_preload_prefill_preload_shader_cache(cache);

   cache->rsds.pool = desc_pool;
   cache->rsds.rsds = pan_preload_rsd_key_table_create(NULL);
   pthread_mutex_init(&cache->rsds.lock, NULL);
   cache->blend_shader_cache = blend_shader_cache;
}

void
GENX(pan_fb_preload_cache_cleanup)(struct pan_fb_preload_cache *cache)
{
   _mesa_hash_table_destroy(cache->shaders.preload, NULL);
   _mesa_hash_table_destroy(cache->shaders.blend, NULL);
   pthread_mutex_destroy(&cache->shaders.lock);
   _mesa_hash_table_destroy(cache->rsds.rsds, NULL);
   pthread_mutex_destroy(&cache->rsds.lock);
}
