/*
 * Copyright © 2015 Red Hat
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
 */

#include "nir.h"
#include "nir_builder.h"

/* Lower gl_FragCoord (and ddy) to account for driver's requested coordinate-
 * origin and pixel-center vs. shader.  If transformation is required, a
 * gl_FbWposYTransform uniform is inserted (with the specified state-slots)
 * and additional instructions are inserted to transform gl_FragCoord (and
 * ddy src arg).
 *
 * This is based on the logic in emit_wpos()/emit_wpos_adjustment() in TGSI
 * compiler.
 */

typedef struct {
   const nir_lower_wpos_ytransform_options *options;
   nir_shader *shader;
   nir_builder b;
   nir_variable *transform;
   nir_def *load;
} lower_wpos_ytransform_state;

static nir_def *
get_transform(lower_wpos_ytransform_state *state)
{
   if (state->transform == NULL) {
      /* NOTE: name must be prefixed w/ "gl_" to trigger slot based
       * special handling in uniform setup:
       */
      nir_variable *var = nir_state_variable_create(state->shader,
                                                    glsl_vec4_type(),
                                                    "gl_FbWposYTransform",
                                                    state->options->state_tokens);

      var->data.how_declared = nir_var_hidden;
      state->transform = var;
      state->b.cursor = nir_before_impl(nir_shader_get_entrypoint(state->b.shader));
      state->load = nir_load_var(&state->b, state->transform);
   }
   return state->load;
}

/* NIR equiv of TGSI CMP instruction: */
static nir_def *
nir_cmp(nir_builder *b, nir_def *src0, nir_def *src1, nir_def *src2)
{
   return nir_bcsel(b, nir_flt_imm(b, src0, 0.0), src1, src2);
}

/* see emit_wpos_adjustment() in st_mesa_to_tgsi.c */
static void
emit_wpos_adjustment(lower_wpos_ytransform_state *state,
                     nir_intrinsic_instr *intr, bool invert,
                     float adjX, float adjY[2])
{
   nir_builder *b = &state->b;
   nir_def *wpos_temp_x = NULL, *wpos_temp_y = NULL, *wpos_temp, *wpos_input[4] = {NULL};
   nir_def *wpostrans = get_transform(state);

   unsigned c = 0;
   const nir_intrinsic_info *info = &nir_intrinsic_infos[intr->intrinsic];
   if (info->index_map[NIR_INTRINSIC_COMPONENT]) {
      c = nir_intrinsic_component(intr);
      /* this pass only alters the first two components */
      if (c > 1)
         return;
   }

   b->cursor = nir_after_instr(&intr->instr);
   for (unsigned i = 0; i < intr->num_components; i++)
      wpos_input[i + c] = nir_channel(b, &intr->def, i);

   /* First, apply the coordinate shift: */
   if (adjX || adjY[0] || adjY[1]) {
      if (wpos_input[0])
         wpos_temp_x = nir_fadd(b, wpos_input[0], nir_imm_float(b, adjX));
      if (wpos_input[1] && adjY[0] != adjY[1]) {
         /* Adjust the y coordinate by adjY[1] or adjY[0] respectively
          * depending on whether inversion is actually going to be applied
          * or not, which is determined by testing against the inversion
          * state variable used below, which will be either +1 or -1.
          */
         nir_def *adj_temp = nir_cmp(b,
                                     nir_channel(b, wpostrans, invert ? 2 : 0),
                                     nir_imm_float(b, adjY[0]),
                                     nir_imm_float(b, adjY[1]));

         wpos_temp_y = nir_fadd(b, wpos_input[1], adj_temp);
      } else if (wpos_input[1]) {
         wpos_temp_y = nir_fadd(b, wpos_input[1], nir_imm_float(b, adjY[0]));
      }
   } else {
      /* MOV wpos_temp, input[wpos]
       */
      wpos_temp_x = wpos_input[0];
      wpos_temp_y = wpos_input[1];
   }

   if (wpos_temp_y) {
      /* Now the conditional y flip: STATE_FB_WPOS_Y_TRANSFORM.xy/zw will be
      * inversion/identity, or the other way around if we're drawing to an FBO.
      */
      if (invert) {
         /* wpos_temp.y = wpos_temp * wpostrans.xxxx + wpostrans.yyyy */
         wpos_temp_y = nir_fadd(b, nir_fmul(b, wpos_temp_y, nir_channel(b, wpostrans, 0)),
                              nir_channel(b, wpostrans, 1));
      } else {
         /* wpos_temp.y = wpos_temp * wpostrans.zzzz + wpostrans.wwww */
         wpos_temp_y = nir_fadd(b, nir_fmul(b, wpos_temp_y, nir_channel(b, wpostrans, 2)),
                              nir_channel(b, wpostrans, 3));
      }
   }

   wpos_input[0] = wpos_temp_x;
   wpos_input[1] = wpos_temp_y;
   wpos_temp = intr->num_components > 1 ?
               nir_vec(b, &wpos_input[c], intr->num_components) :
               wpos_input[c];

   nir_def_rewrite_uses_after(&intr->def,
                              wpos_temp,
                              wpos_temp->parent_instr);
}

static void
lower_fragcoord(lower_wpos_ytransform_state *state, nir_intrinsic_instr *intr)
{
   const nir_lower_wpos_ytransform_options *options = state->options;
   float adjX = 0.0f;
   float adjY[2] = { 0.0f, 0.0f };
   bool invert = false;

   /* Based on logic in emit_wpos():
    *
    * Query the pixel center conventions supported by the pipe driver and set
    * adjX, adjY to help out if it cannot handle the requested one internally.
    *
    * The bias of the y-coordinate depends on whether y-inversion takes place
    * (adjY[1]) or not (adjY[0]), which is in turn dependent on whether we are
    * drawing to an FBO (causes additional inversion), and whether the pipe
    * driver origin and the requested origin differ (the latter condition is
    * stored in the 'invert' variable).
    *
    * For height = 100 (i = integer, h = half-integer, l = lower, u = upper):
    *
    * center shift only:
    * i -> h: +0.5
    * h -> i: -0.5
    *
    * inversion only:
    * l,i -> u,i: ( 0.0 + 1.0) * -1 + 100 = 99
    * l,h -> u,h: ( 0.5 + 0.0) * -1 + 100 = 99.5
    * u,i -> l,i: (99.0 + 1.0) * -1 + 100 = 0
    * u,h -> l,h: (99.5 + 0.0) * -1 + 100 = 0.5
    *
    * inversion and center shift:
    * l,i -> u,h: ( 0.0 + 0.5) * -1 + 100 = 99.5
    * l,h -> u,i: ( 0.5 + 0.5) * -1 + 100 = 99
    * u,i -> l,h: (99.0 + 0.5) * -1 + 100 = 0.5
    * u,h -> l,i: (99.5 + 0.5) * -1 + 100 = 0
    */

   if (state->shader->info.fs.origin_upper_left) {
      /* Fragment shader wants origin in upper-left */
      if (options->fs_coord_origin_upper_left) {
         /* the driver supports upper-left origin */
      } else if (options->fs_coord_origin_lower_left) {
         /* the driver supports lower-left origin, need to invert Y */
         invert = true;
      } else {
         unreachable("invalid options");
      }
   } else {
      /* Fragment shader wants origin in lower-left */
      if (options->fs_coord_origin_lower_left) {
         /* the driver supports lower-left origin */
      } else if (options->fs_coord_origin_upper_left) {
         /* the driver supports upper-left origin, need to invert Y */
         invert = true;
      } else {
         unreachable("invalid options");
      }
   }

   if (state->shader->info.fs.pixel_center_integer) {
      /* Fragment shader wants pixel center integer */
      if (options->fs_coord_pixel_center_integer) {
         /* the driver supports pixel center integer */
         adjY[1] = 1.0f;
      } else if (options->fs_coord_pixel_center_half_integer) {
         /* the driver supports pixel center half integer, need to bias X,Y */
         adjX = -0.5f;
         adjY[0] = -0.5f;
         adjY[1] = 0.5f;
      } else {
         unreachable("invalid options");
      }
   } else {
      /* Fragment shader wants pixel center half integer */
      if (options->fs_coord_pixel_center_half_integer) {
         /* the driver supports pixel center half integer */
      } else if (options->fs_coord_pixel_center_integer) {
         /* the driver supports pixel center integer, need to bias X,Y */
         adjX = adjY[0] = adjY[1] = 0.5f;
      } else {
         unreachable("invalid options");
      }
   }

   emit_wpos_adjustment(state, intr, invert, adjX, adjY);
}

/* turns 'ddy(p)' into 'ddy(fmul(p, transform.x))' */
static void
lower_ddy(lower_wpos_ytransform_state *state, nir_intrinsic_instr *ddy)
{
   nir_builder *b = &state->b;
   nir_def *wpostrans = get_transform(state);

   b->cursor = nir_before_instr(&ddy->instr);

   nir_def *p = ddy->src[0].ssa;
   nir_def *trans = nir_f2fN(b, nir_channel(b, wpostrans, 0), p->bit_size);
   nir_def *pt = nir_fmul(b, p, trans);

   nir_src_rewrite(&ddy->src[0], pt);
}

/* Multiply interp_deref_at_offset's or load_barycentric_at_offset's offset
 * by transform.x to flip it.
 */
static void
lower_interp_deref_or_load_baryc_at_offset(lower_wpos_ytransform_state *state,
                                           nir_intrinsic_instr *intr,
                                           unsigned offset_src)
{
   nir_builder *b = &state->b;
   nir_def *offset;
   nir_def *flip_y;
   nir_def *wpostrans = get_transform(state);

   b->cursor = nir_before_instr(&intr->instr);

   offset = intr->src[offset_src].ssa;
   flip_y = nir_fmul(b, nir_channel(b, offset, 1),
                     nir_channel(b, wpostrans, 0));
   nir_src_rewrite(&intr->src[offset_src],
                   nir_vec2(b, nir_channel(b, offset, 0), flip_y));
}

static void
lower_load_sample_pos(lower_wpos_ytransform_state *state,
                      nir_intrinsic_instr *intr)
{
   nir_builder *b = &state->b;
   nir_def *wpostrans = get_transform(state);
   b->cursor = nir_after_instr(&intr->instr);

   nir_def *pos = &intr->def;
   nir_def *scale = nir_channel(b, wpostrans, 0);
   nir_def *neg_scale = nir_channel(b, wpostrans, 2);
   /* Either y or 1-y for scale equal to 1 or -1 respectively. */
   nir_def *flipped_y =
      nir_fadd(b, nir_fmax(b, neg_scale, nir_imm_float(b, 0.0)),
               nir_fmul(b, nir_channel(b, pos, 1), scale));
   nir_def *flipped_pos = nir_vec2(b, nir_channel(b, pos, 0), flipped_y);

   nir_def_rewrite_uses_after(&intr->def, flipped_pos,
                              flipped_pos->parent_instr);
}

static bool
lower_wpos_ytransform_instr(nir_builder *b, nir_instr *instr,
                            void *data)
{
   lower_wpos_ytransform_state *state = data;
   state->b = *b;

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic == nir_intrinsic_load_deref) {
         nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
         nir_variable *var = nir_deref_instr_get_variable(deref);
         if ((var->data.mode == nir_var_shader_in &&
              var->data.location == VARYING_SLOT_POS) ||
             (var->data.mode == nir_var_system_value &&
              var->data.location == SYSTEM_VALUE_FRAG_COORD)) {
            /* gl_FragCoord should not have array/struct derefs: */
            lower_fragcoord(state, intr);
         } else if (var->data.mode == nir_var_system_value &&
                    var->data.location == SYSTEM_VALUE_SAMPLE_POS) {
            lower_load_sample_pos(state, intr);
         }
      } else if (intr->intrinsic == nir_intrinsic_load_interpolated_input) {
         nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
         if (sem.location == VARYING_SLOT_POS)
            lower_fragcoord(state, intr);
      } else if (intr->intrinsic == nir_intrinsic_load_frag_coord) {
         lower_fragcoord(state, intr);
      } else if (intr->intrinsic == nir_intrinsic_load_sample_pos) {
         lower_load_sample_pos(state, intr);
      } else if (intr->intrinsic == nir_intrinsic_interp_deref_at_offset) {
         lower_interp_deref_or_load_baryc_at_offset(state, intr, 1);
      } else if (intr->intrinsic == nir_intrinsic_load_barycentric_at_offset) {
         lower_interp_deref_or_load_baryc_at_offset(state, intr, 0);
      } else if (intr->intrinsic == nir_intrinsic_ddy ||
                 intr->intrinsic == nir_intrinsic_ddy_fine ||
                 intr->intrinsic == nir_intrinsic_ddy_coarse) {
         lower_ddy(state, intr);
      }
   }

   return state->transform != NULL;
}

bool
nir_lower_wpos_ytransform(nir_shader *shader,
                          const nir_lower_wpos_ytransform_options *options)
{
   lower_wpos_ytransform_state state = {
      .options = options,
      .shader = shader,
   };

   assert(shader->info.stage == MESA_SHADER_FRAGMENT);

   return nir_shader_instructions_pass(shader,
                                       lower_wpos_ytransform_instr,
                                       nir_metadata_control_flow,
                                       &state);
}
