/*
 * Copyright © 2010 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "brw_eu.h"
#include "brw_fs.h"
#include "brw_fs_builder.h"
#include "brw_fs_live_variables.h"
#include "brw_nir.h"
#include "brw_cfg.h"
#include "brw_private.h"
#include "intel_nir.h"
#include "shader_enums.h"
#include "dev/intel_debug.h"
#include "dev/intel_wa.h"

#include <memory>

using namespace brw;

/**
 * Turn one of the two CENTROID barycentric modes into PIXEL mode.
 */
static enum brw_barycentric_mode
centroid_to_pixel(enum brw_barycentric_mode bary)
{
   assert(bary == BRW_BARYCENTRIC_PERSPECTIVE_CENTROID ||
          bary == BRW_BARYCENTRIC_NONPERSPECTIVE_CENTROID);
   return (enum brw_barycentric_mode) ((unsigned) bary - 1);
}

static void
calculate_urb_setup(const struct intel_device_info *devinfo,
                    const struct brw_wm_prog_key *key,
                    struct brw_wm_prog_data *prog_data,
                    const nir_shader *nir,
                    const struct brw_mue_map *mue_map)
{
   memset(prog_data->urb_setup, -1, sizeof(prog_data->urb_setup));
   memset(prog_data->urb_setup_channel, 0, sizeof(prog_data->urb_setup_channel));

   int urb_next = 0; /* in vec4s */

   const uint64_t inputs_read =
      nir->info.inputs_read & ~nir->info.per_primitive_inputs;

   /* Figure out where each of the incoming setup attributes lands. */
   if (key->mesh_input != BRW_NEVER) {
      /* Per-Primitive Attributes are laid out by Hardware before the regular
       * attributes, so order them like this to make easy later to map setup
       * into real HW registers.
       */
      if (nir->info.per_primitive_inputs) {
         uint64_t per_prim_inputs_read =
               nir->info.inputs_read & nir->info.per_primitive_inputs;

         /* In Mesh, PRIMITIVE_SHADING_RATE, VIEWPORT and LAYER slots
          * are always at the beginning, because they come from MUE
          * Primitive Header, not Per-Primitive Attributes.
          */
         const uint64_t primitive_header_bits = VARYING_BIT_VIEWPORT |
                                                VARYING_BIT_LAYER |
                                                VARYING_BIT_PRIMITIVE_SHADING_RATE;

         if (mue_map) {
            unsigned per_prim_start_dw = mue_map->per_primitive_start_dw;
            unsigned per_prim_size_dw = mue_map->per_primitive_pitch_dw;

            bool reads_header = (per_prim_inputs_read & primitive_header_bits) != 0;

            if (reads_header || mue_map->user_data_in_primitive_header) {
               /* Primitive Shading Rate, Layer and Viewport live in the same
                * 4-dwords slot (psr is dword 0, layer is dword 1, and viewport
                * is dword 2).
                */
               if (per_prim_inputs_read & VARYING_BIT_PRIMITIVE_SHADING_RATE)
                  prog_data->urb_setup[VARYING_SLOT_PRIMITIVE_SHADING_RATE] = 0;

               if (per_prim_inputs_read & VARYING_BIT_LAYER)
                  prog_data->urb_setup[VARYING_SLOT_LAYER] = 0;

               if (per_prim_inputs_read & VARYING_BIT_VIEWPORT)
                  prog_data->urb_setup[VARYING_SLOT_VIEWPORT] = 0;

               per_prim_inputs_read &= ~primitive_header_bits;
            } else {
               /* If fs doesn't need primitive header, then it won't be made
                * available through SBE_MESH, so we have to skip them when
                * calculating offset from start of per-prim data.
                */
               per_prim_start_dw += mue_map->per_primitive_header_size_dw;
               per_prim_size_dw -= mue_map->per_primitive_header_size_dw;
            }

            u_foreach_bit64(i, per_prim_inputs_read) {
               int start = mue_map->start_dw[i];

               assert(start >= 0);
               assert(mue_map->len_dw[i] > 0);

               assert(unsigned(start) >= per_prim_start_dw);
               unsigned pos_dw = unsigned(start) - per_prim_start_dw;

               prog_data->urb_setup[i] = urb_next + pos_dw / 4;
               prog_data->urb_setup_channel[i] = pos_dw % 4;
            }

            urb_next = per_prim_size_dw / 4;
         } else {
            /* With no MUE map, we never read the primitive header, and
             * per-primitive attributes won't be packed either, so just lay
             * them in varying order.
             */
            per_prim_inputs_read &= ~primitive_header_bits;

            for (unsigned i = 0; i < VARYING_SLOT_MAX; i++) {
               if (per_prim_inputs_read & BITFIELD64_BIT(i)) {
                  prog_data->urb_setup[i] = urb_next++;
               }
            }

            /* The actual setup attributes later must be aligned to a full GRF. */
            urb_next = ALIGN(urb_next, 2);
         }

         prog_data->num_per_primitive_inputs = urb_next;
      }

      const uint64_t clip_dist_bits = VARYING_BIT_CLIP_DIST0 |
                                      VARYING_BIT_CLIP_DIST1;

      uint64_t unique_fs_attrs = inputs_read & BRW_FS_VARYING_INPUT_MASK;

      if (inputs_read & clip_dist_bits) {
         assert(!mue_map || mue_map->per_vertex_header_size_dw > 8);
         unique_fs_attrs &= ~clip_dist_bits;
      }

      if (mue_map) {
         unsigned per_vertex_start_dw = mue_map->per_vertex_start_dw;
         unsigned per_vertex_size_dw = mue_map->per_vertex_pitch_dw;

         /* Per-Vertex header is available to fragment shader only if there's
          * user data there.
          */
         if (!mue_map->user_data_in_vertex_header) {
            per_vertex_start_dw += 8;
            per_vertex_size_dw -= 8;
         }

         /* In Mesh, CLIP_DIST slots are always at the beginning, because
          * they come from MUE Vertex Header, not Per-Vertex Attributes.
          */
         if (inputs_read & clip_dist_bits) {
            prog_data->urb_setup[VARYING_SLOT_CLIP_DIST0] = urb_next;
            prog_data->urb_setup[VARYING_SLOT_CLIP_DIST1] = urb_next + 1;
         } else if (mue_map && mue_map->per_vertex_header_size_dw > 8) {
            /* Clip distances are in MUE, but we are not reading them in FS. */
            per_vertex_start_dw += 8;
            per_vertex_size_dw -= 8;
         }

         /* Per-Vertex attributes are laid out ordered.  Because we always link
          * Mesh and Fragment shaders, the which slots are written and read by
          * each of them will match. */
         u_foreach_bit64(i, unique_fs_attrs) {
            int start = mue_map->start_dw[i];

            assert(start >= 0);
            assert(mue_map->len_dw[i] > 0);

            assert(unsigned(start) >= per_vertex_start_dw);
            unsigned pos_dw = unsigned(start) - per_vertex_start_dw;

            prog_data->urb_setup[i] = urb_next + pos_dw / 4;
            prog_data->urb_setup_channel[i] = pos_dw % 4;
         }

         urb_next += per_vertex_size_dw / 4;
      } else {
         /* If we don't have an MUE map, just lay down the inputs the FS reads
          * in varying order, as we do for the legacy pipeline.
          */
         if (inputs_read & clip_dist_bits) {
            prog_data->urb_setup[VARYING_SLOT_CLIP_DIST0] = urb_next++;
            prog_data->urb_setup[VARYING_SLOT_CLIP_DIST1] = urb_next++;
         }

         for (unsigned int i = 0; i < VARYING_SLOT_MAX; i++) {
            if (unique_fs_attrs & BITFIELD64_BIT(i))
               prog_data->urb_setup[i] = urb_next++;
         }
      }
   } else {
      assert(!nir->info.per_primitive_inputs);

      uint64_t vue_header_bits =
         VARYING_BIT_PSIZ | VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT;

      uint64_t unique_fs_attrs = inputs_read & BRW_FS_VARYING_INPUT_MASK;

      /* VUE header fields all live in the same URB slot, so we pass them
       * as a single FS input attribute.  We want to only count them once.
       */
      if (inputs_read & vue_header_bits) {
         unique_fs_attrs &= ~vue_header_bits;
         unique_fs_attrs |= VARYING_BIT_PSIZ;
      }

      if (util_bitcount64(unique_fs_attrs) <= 16) {
         /* The SF/SBE pipeline stage can do arbitrary rearrangement of the
          * first 16 varying inputs, so we can put them wherever we want.
          * Just put them in order.
          *
          * This is useful because it means that (a) inputs not used by the
          * fragment shader won't take up valuable register space, and (b) we
          * won't have to recompile the fragment shader if it gets paired with
          * a different vertex (or geometry) shader.
          *
          * VUE header fields share the same FS input attribute.
          */
         if (inputs_read & vue_header_bits) {
            if (inputs_read & VARYING_BIT_PSIZ)
               prog_data->urb_setup[VARYING_SLOT_PSIZ] = urb_next;
            if (inputs_read & VARYING_BIT_LAYER)
               prog_data->urb_setup[VARYING_SLOT_LAYER] = urb_next;
            if (inputs_read & VARYING_BIT_VIEWPORT)
               prog_data->urb_setup[VARYING_SLOT_VIEWPORT] = urb_next;

            urb_next++;
         }

         for (unsigned int i = 0; i < VARYING_SLOT_MAX; i++) {
            if (inputs_read & BRW_FS_VARYING_INPUT_MASK & ~vue_header_bits &
                BITFIELD64_BIT(i)) {
               prog_data->urb_setup[i] = urb_next++;
            }
         }
      } else {
         /* We have enough input varyings that the SF/SBE pipeline stage can't
          * arbitrarily rearrange them to suit our whim; we have to put them
          * in an order that matches the output of the previous pipeline stage
          * (geometry or vertex shader).
          */

         /* Re-compute the VUE map here in the case that the one coming from
          * geometry has more than one position slot (used for Primitive
          * Replication).
          */
         struct intel_vue_map prev_stage_vue_map;
         brw_compute_vue_map(devinfo, &prev_stage_vue_map,
                             key->input_slots_valid,
                             nir->info.separate_shader, 1);

         int first_slot =
            brw_compute_first_urb_slot_required(inputs_read,
                                                &prev_stage_vue_map);

         assert(prev_stage_vue_map.num_slots <= first_slot + 32);
         for (int slot = first_slot; slot < prev_stage_vue_map.num_slots;
              slot++) {
            int varying = prev_stage_vue_map.slot_to_varying[slot];
            if (varying != BRW_VARYING_SLOT_PAD &&
                (inputs_read & BRW_FS_VARYING_INPUT_MASK &
                 BITFIELD64_BIT(varying))) {
               prog_data->urb_setup[varying] = slot - first_slot;
            }
         }
         urb_next = prev_stage_vue_map.num_slots - first_slot;
      }
   }

   prog_data->num_varying_inputs = urb_next - prog_data->num_per_primitive_inputs;
   prog_data->inputs = inputs_read;

   brw_compute_urb_setup_index(prog_data);
}
static bool
is_used_in_not_interp_frag_coord(nir_def *def)
{
   nir_foreach_use_including_if(src, def) {
      if (nir_src_is_if(src))
         return true;

      if (nir_src_parent_instr(src)->type != nir_instr_type_intrinsic)
         return true;

      nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(nir_src_parent_instr(src));
      if (intrin->intrinsic != nir_intrinsic_load_frag_coord)
         return true;
   }

   return false;
}

/**
 * Return a bitfield where bit n is set if barycentric interpolation mode n
 * (see enum brw_barycentric_mode) is needed by the fragment shader.
 *
 * We examine the load_barycentric intrinsics rather than looking at input
 * variables so that we catch interpolateAtCentroid() messages too, which
 * also need the BRW_BARYCENTRIC_[NON]PERSPECTIVE_CENTROID mode set up.
 */
static unsigned
brw_compute_barycentric_interp_modes(const struct intel_device_info *devinfo,
                                     const struct brw_wm_prog_key *key,
                                     const nir_shader *shader)
{
   unsigned barycentric_interp_modes = 0;

   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            switch (intrin->intrinsic) {
            case nir_intrinsic_load_barycentric_pixel:
            case nir_intrinsic_load_barycentric_centroid:
            case nir_intrinsic_load_barycentric_sample:
            case nir_intrinsic_load_barycentric_at_sample:
            case nir_intrinsic_load_barycentric_at_offset:
               break;
            default:
               continue;
            }

            /* Ignore WPOS; it doesn't require interpolation. */
            if (!is_used_in_not_interp_frag_coord(&intrin->def))
               continue;

            nir_intrinsic_op bary_op = intrin->intrinsic;
            enum brw_barycentric_mode bary =
               brw_barycentric_mode(key, intrin);

            barycentric_interp_modes |= 1 << bary;

            if (devinfo->needs_unlit_centroid_workaround &&
                bary_op == nir_intrinsic_load_barycentric_centroid)
               barycentric_interp_modes |= 1 << centroid_to_pixel(bary);
         }
      }
   }

   return barycentric_interp_modes;
}

/**
 * Return a bitfield where bit n is set if barycentric interpolation
 * mode n (see enum brw_barycentric_mode) is needed by the fragment
 * shader barycentric intrinsics that take an explicit offset or
 * sample as argument.
 */
static unsigned
brw_compute_offset_barycentric_interp_modes(const struct brw_wm_prog_key *key,
                                            const nir_shader *shader)
{
   unsigned barycentric_interp_modes = 0;

   nir_foreach_function_impl(impl, shader) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
            if (intrin->intrinsic == nir_intrinsic_load_barycentric_at_offset ||
                intrin->intrinsic == nir_intrinsic_load_barycentric_at_sample)
               barycentric_interp_modes |= 1 << brw_barycentric_mode(key, intrin);
         }
      }
   }

   return barycentric_interp_modes;
}

static void
brw_compute_flat_inputs(struct brw_wm_prog_data *prog_data,
                        const nir_shader *shader)
{
   prog_data->flat_inputs = 0;

   nir_foreach_shader_in_variable(var, shader) {
      /* flat shading */
      if (var->data.interpolation != INTERP_MODE_FLAT)
         continue;

      if (var->data.per_primitive)
         continue;

      unsigned slots = glsl_count_attribute_slots(var->type, false);
      for (unsigned s = 0; s < slots; s++) {
         int input_index = prog_data->urb_setup[var->data.location + s];

         if (input_index >= 0)
            prog_data->flat_inputs |= 1 << input_index;
      }
   }
}

static uint8_t
computed_depth_mode(const nir_shader *shader)
{
   if (shader->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
      switch (shader->info.fs.depth_layout) {
      case FRAG_DEPTH_LAYOUT_NONE:
      case FRAG_DEPTH_LAYOUT_ANY:
         return BRW_PSCDEPTH_ON;
      case FRAG_DEPTH_LAYOUT_GREATER:
         return BRW_PSCDEPTH_ON_GE;
      case FRAG_DEPTH_LAYOUT_LESS:
         return BRW_PSCDEPTH_ON_LE;
      case FRAG_DEPTH_LAYOUT_UNCHANGED:
         /* We initially set this to OFF, but having the shader write the
          * depth means we allocate register space in the SEND message. The
          * difference between the SEND register count and the OFF state
          * programming makes the HW hang.
          *
          * Removing the depth writes also leads to test failures. So use
          * LesserThanOrEqual, which fits writing the same value
          * (unchanged/equal).
          *
          */
         return BRW_PSCDEPTH_ON_LE;
      }
   }
   return BRW_PSCDEPTH_OFF;
}

static void
brw_nir_populate_wm_prog_data(nir_shader *shader,
                              const struct intel_device_info *devinfo,
                              const struct brw_wm_prog_key *key,
                              struct brw_wm_prog_data *prog_data,
                              const struct brw_mue_map *mue_map)
{
   prog_data->uses_kill = shader->info.fs.uses_discard;
   prog_data->uses_omask = !key->ignore_sample_mask_out &&
      (shader->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_SAMPLE_MASK));
   prog_data->max_polygons = 1;
   prog_data->computed_depth_mode = computed_depth_mode(shader);
   prog_data->computed_stencil =
      shader->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_STENCIL);

   prog_data->sample_shading =
      shader->info.fs.uses_sample_shading ||
      shader->info.outputs_read;

   assert(key->multisample_fbo != BRW_NEVER ||
          key->persample_interp == BRW_NEVER);

   prog_data->persample_dispatch = key->persample_interp;
   if (prog_data->sample_shading)
      prog_data->persample_dispatch = BRW_ALWAYS;

   /* We can only persample dispatch if we have a multisample FBO */
   prog_data->persample_dispatch = MIN2(prog_data->persample_dispatch,
                                        key->multisample_fbo);

   /* Currently only the Vulkan API allows alpha_to_coverage to be dynamic. If
    * persample_dispatch & multisample_fbo are not dynamic, Anv should be able
    * to definitively tell whether alpha_to_coverage is on or off.
    */
   prog_data->alpha_to_coverage = key->alpha_to_coverage;

   prog_data->uses_sample_mask =
      BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_SAMPLE_MASK_IN);

   /* From the Ivy Bridge PRM documentation for 3DSTATE_PS:
    *
    *    "MSDISPMODE_PERSAMPLE is required in order to select
    *    POSOFFSET_SAMPLE"
    *
    * So we can only really get sample positions if we are doing real
    * per-sample dispatch.  If we need gl_SamplePosition and we don't have
    * persample dispatch, we hard-code it to 0.5.
    */
   prog_data->uses_pos_offset =
      prog_data->persample_dispatch != BRW_NEVER &&
      (BITSET_TEST(shader->info.system_values_read,
                   SYSTEM_VALUE_SAMPLE_POS) ||
       BITSET_TEST(shader->info.system_values_read,
                   SYSTEM_VALUE_SAMPLE_POS_OR_CENTER));

   prog_data->early_fragment_tests = shader->info.fs.early_fragment_tests;
   prog_data->post_depth_coverage = shader->info.fs.post_depth_coverage;
   prog_data->inner_coverage = shader->info.fs.inner_coverage;

   prog_data->barycentric_interp_modes =
      brw_compute_barycentric_interp_modes(devinfo, key, shader);

   /* From the BDW PRM documentation for 3DSTATE_WM:
    *
    *    "MSDISPMODE_PERSAMPLE is required in order to select Perspective
    *     Sample or Non- perspective Sample barycentric coordinates."
    *
    * So cleanup any potentially set sample barycentric mode when not in per
    * sample dispatch.
    */
   if (prog_data->persample_dispatch == BRW_NEVER) {
      prog_data->barycentric_interp_modes &=
         ~BITFIELD_BIT(BRW_BARYCENTRIC_PERSPECTIVE_SAMPLE);
   }

   if (devinfo->ver >= 20) {
      const unsigned offset_bary_modes =
         brw_compute_offset_barycentric_interp_modes(key, shader);

      prog_data->uses_npc_bary_coefficients =
         offset_bary_modes & BRW_BARYCENTRIC_NONPERSPECTIVE_BITS;
      prog_data->uses_pc_bary_coefficients =
         offset_bary_modes & ~BRW_BARYCENTRIC_NONPERSPECTIVE_BITS;
      prog_data->uses_sample_offsets =
         offset_bary_modes & ((1 << BRW_BARYCENTRIC_PERSPECTIVE_SAMPLE) |
                              (1 << BRW_BARYCENTRIC_NONPERSPECTIVE_SAMPLE));
   }

   prog_data->uses_nonperspective_interp_modes =
      (prog_data->barycentric_interp_modes & BRW_BARYCENTRIC_NONPERSPECTIVE_BITS) ||
      prog_data->uses_npc_bary_coefficients;

   /* The current VK_EXT_graphics_pipeline_library specification requires
    * coarse to specified at compile time. But per sample interpolation can be
    * dynamic. So we should never be in a situation where coarse &
    * persample_interp are both respectively true & BRW_ALWAYS.
    *
    * Coarse will dynamically turned off when persample_interp is active.
    */
   assert(!key->coarse_pixel || key->persample_interp != BRW_ALWAYS);

   prog_data->coarse_pixel_dispatch =
      brw_sometimes_invert(prog_data->persample_dispatch);
   if (!key->coarse_pixel ||
       prog_data->uses_omask ||
       prog_data->sample_shading ||
       prog_data->uses_sample_mask ||
       (prog_data->computed_depth_mode != BRW_PSCDEPTH_OFF) ||
       prog_data->computed_stencil) {
      prog_data->coarse_pixel_dispatch = BRW_NEVER;
   }

   /* ICL PRMs, Volume 9: Render Engine, Shared Functions Pixel Interpolater,
    * Message Descriptor :
    *
    *    "Message Type. Specifies the type of message being sent when
    *     pixel-rate evaluation is requested :
    *
    *     Format = U2
    *       0: Per Message Offset (eval_snapped with immediate offset)
    *       1: Sample Position Offset (eval_sindex)
    *       2: Centroid Position Offset (eval_centroid)
    *       3: Per Slot Offset (eval_snapped with register offset)
    *
    *     Message Type. Specifies the type of message being sent when
    *     coarse-rate evaluation is requested :
    *
    *     Format = U2
    *       0: Coarse to Pixel Mapping Message (internal message)
    *       1: Reserved
    *       2: Coarse Centroid Position (eval_centroid)
    *       3: Per Slot Coarse Pixel Offset (eval_snapped with register offset)"
    *
    * The Sample Position Offset is marked as reserved for coarse rate
    * evaluation and leads to hangs if we try to use it. So disable coarse
    * pixel shading if we have any intrinsic that will result in a pixel
    * interpolater message at sample.
    */
   if (intel_nir_pulls_at_sample(shader))
      prog_data->coarse_pixel_dispatch = BRW_NEVER;

   /* We choose to always enable VMask prior to XeHP, as it would cause
    * us to lose out on the eliminate_find_live_channel() optimization.
    */
   prog_data->uses_vmask = devinfo->verx10 < 125 ||
                           shader->info.fs.needs_quad_helper_invocations ||
                           shader->info.uses_wide_subgroup_intrinsics ||
                           prog_data->coarse_pixel_dispatch != BRW_NEVER;

   prog_data->uses_src_w =
      BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
   prog_data->uses_src_depth =
      BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_FRAG_COORD) &&
      prog_data->coarse_pixel_dispatch != BRW_ALWAYS;
   prog_data->uses_depth_w_coefficients = prog_data->uses_pc_bary_coefficients ||
      (BITSET_TEST(shader->info.system_values_read, SYSTEM_VALUE_FRAG_COORD) &&
       prog_data->coarse_pixel_dispatch != BRW_NEVER);

   calculate_urb_setup(devinfo, key, prog_data, shader, mue_map);
   brw_compute_flat_inputs(prog_data, shader);
}

/* From the SKL PRM, Volume 16, Workarounds:
 *
 *   0877  3D   Pixel Shader Hang possible when pixel shader dispatched with
 *              only header phases (R0-R2)
 *
 *   WA: Enable a non-header phase (e.g. push constant) when dispatch would
 *       have been header only.
 *
 * Instead of enabling push constants one can alternatively enable one of the
 * inputs. Here one simply chooses "layer" which shouldn't impose much
 * overhead.
 */
static void
gfx9_ps_header_only_workaround(struct brw_wm_prog_data *wm_prog_data)
{
   if (wm_prog_data->num_varying_inputs)
      return;

   if (wm_prog_data->base.curb_read_length)
      return;

   wm_prog_data->urb_setup[VARYING_SLOT_LAYER] = 0;
   wm_prog_data->num_varying_inputs = 1;

   brw_compute_urb_setup_index(wm_prog_data);
}

static bool
run_fs(fs_visitor &s, bool allow_spilling, bool do_rep_send)
{
   const struct intel_device_info *devinfo = s.devinfo;
   struct brw_wm_prog_data *wm_prog_data = brw_wm_prog_data(s.prog_data);
   brw_wm_prog_key *wm_key = (brw_wm_prog_key *) s.key;
   const fs_builder bld = fs_builder(&s).at_end();
   const nir_shader *nir = s.nir;

   assert(s.stage == MESA_SHADER_FRAGMENT);

   s.payload_ = new fs_thread_payload(s, s.source_depth_to_render_target);

   if (nir->info.ray_queries > 0)
      s.limit_dispatch_width(16, "SIMD32 not supported with ray queries.\n");

   if (do_rep_send) {
      assert(s.dispatch_width == 16);
      s.emit_repclear_shader();
   } else {
      if (nir->info.inputs_read > 0 ||
          BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FRAG_COORD) ||
          (nir->info.outputs_read > 0 && !wm_key->coherent_fb_fetch)) {
         s.emit_interpolation_setup();
      }

      /* We handle discards by keeping track of the still-live pixels in f0.1.
       * Initialize it with the dispatched pixels.
       */
      if (devinfo->ver >= 20 || wm_prog_data->uses_kill) {
         const unsigned lower_width = MIN2(s.dispatch_width, 16);
         for (unsigned i = 0; i < s.dispatch_width / lower_width; i++) {
            /* According to the "PS Thread Payload for Normal
             * Dispatch" pages on the BSpec, the dispatch mask is
             * stored in R0.15/R1.15 on gfx20+ and in R1.7/R2.7 on
             * gfx6+.
             */
            const brw_reg dispatch_mask =
               devinfo->ver >= 20 ? xe2_vec1_grf(i, 15) :
                                    brw_vec1_grf(i + 1, 7);
            bld.exec_all().group(1, 0)
               .MOV(brw_sample_mask_reg(bld.group(lower_width, i)),
                    retype(dispatch_mask, BRW_TYPE_UW));
         }
      }

      if (nir->info.writes_memory)
         wm_prog_data->has_side_effects = true;

      nir_to_brw(&s);

      if (s.failed)
	 return false;

      s.emit_fb_writes();

      s.calculate_cfg();

      brw_fs_optimize(s);

      s.assign_curb_setup();

      if (devinfo->ver == 9)
         gfx9_ps_header_only_workaround(wm_prog_data);

      s.assign_urb_setup();

      brw_fs_lower_3src_null_dest(s);
      brw_fs_workaround_memory_fence_before_eot(s);
      brw_fs_workaround_emit_dummy_mov_instruction(s);

      s.allocate_registers(allow_spilling);
   }

   return !s.failed;
}

const unsigned *
brw_compile_fs(const struct brw_compiler *compiler,
               struct brw_compile_fs_params *params)
{
   struct nir_shader *nir = params->base.nir;
   const struct brw_wm_prog_key *key = params->key;
   struct brw_wm_prog_data *prog_data = params->prog_data;
   bool allow_spilling = params->allow_spilling;
   const bool debug_enabled =
      brw_should_print_shader(nir, params->base.debug_flag ?
                                   params->base.debug_flag : DEBUG_WM);

   prog_data->base.stage = MESA_SHADER_FRAGMENT;
   prog_data->base.ray_queries = nir->info.ray_queries;
   prog_data->base.total_scratch = 0;

   const struct intel_device_info *devinfo = compiler->devinfo;
   const unsigned max_subgroup_size = 32;

   brw_nir_apply_key(nir, compiler, &key->base, max_subgroup_size);
   brw_nir_lower_fs_inputs(nir, devinfo, key);
   brw_nir_lower_fs_outputs(nir);

   /* From the SKL PRM, Volume 7, "Alpha Coverage":
    *  "If Pixel Shader outputs oMask, AlphaToCoverage is disabled in
    *   hardware, regardless of the state setting for this feature."
    */
   if (key->alpha_to_coverage != BRW_NEVER) {
      /* Run constant fold optimization in order to get the correct source
       * offset to determine render target 0 store instruction in
       * emit_alpha_to_coverage pass.
       */
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, brw_nir_lower_alpha_to_coverage, key, prog_data);
   }

   NIR_PASS(_, nir, brw_nir_move_interpolation_to_top);
   brw_postprocess_nir(nir, compiler, debug_enabled,
                       key->base.robust_flags);

   brw_nir_populate_wm_prog_data(nir, compiler->devinfo, key, prog_data,
                                 params->mue_map);

   std::unique_ptr<fs_visitor> v8, v16, v32, vmulti;
   cfg_t *simd8_cfg = NULL, *simd16_cfg = NULL, *simd32_cfg = NULL,
      *multi_cfg = NULL;
   float throughput = 0;
   bool has_spilled = false;

   if (devinfo->ver < 20) {
      v8 = std::make_unique<fs_visitor>(compiler, &params->base, key,
                                        prog_data, nir, 8, 1,
                                        params->base.stats != NULL,
                                        debug_enabled);
      if (!run_fs(*v8, allow_spilling, false /* do_rep_send */)) {
         params->base.error_str = ralloc_strdup(params->base.mem_ctx,
                                                v8->fail_msg);
         return NULL;
      } else if (INTEL_SIMD(FS, 8)) {
         simd8_cfg = v8->cfg;

         assert(v8->payload().num_regs % reg_unit(devinfo) == 0);
         prog_data->base.dispatch_grf_start_reg = v8->payload().num_regs / reg_unit(devinfo);

         const performance &perf = v8->performance_analysis.require();
         throughput = MAX2(throughput, perf.throughput);
         has_spilled = v8->spilled_any_registers;
         allow_spilling = false;
      }
   }

   if (key->coarse_pixel && devinfo->ver < 20) {
      if (prog_data->dual_src_blend) {
         v8->limit_dispatch_width(8, "SIMD16 coarse pixel shading cannot"
                                  " use SIMD8 messages.\n");
      }
      v8->limit_dispatch_width(16, "SIMD32 not supported with coarse"
                               " pixel shading.\n");
   }

   if (!has_spilled &&
       (!v8 || v8->max_dispatch_width >= 16) &&
       (INTEL_SIMD(FS, 16) || params->use_rep_send)) {
      /* Try a SIMD16 compile */
      v16 = std::make_unique<fs_visitor>(compiler, &params->base, key,
                                         prog_data, nir, 16, 1,
                                         params->base.stats != NULL,
                                         debug_enabled);
      if (v8)
         v16->import_uniforms(v8.get());
      if (!run_fs(*v16, allow_spilling, params->use_rep_send)) {
         brw_shader_perf_log(compiler, params->base.log_data,
                             "SIMD16 shader failed to compile: %s\n",
                             v16->fail_msg);
      } else {
         simd16_cfg = v16->cfg;

         assert(v16->payload().num_regs % reg_unit(devinfo) == 0);
         prog_data->dispatch_grf_start_reg_16 = v16->payload().num_regs / reg_unit(devinfo);

         const performance &perf = v16->performance_analysis.require();
         throughput = MAX2(throughput, perf.throughput);
         has_spilled = v16->spilled_any_registers;
         allow_spilling = false;
      }
   }

   const bool simd16_failed = v16 && !simd16_cfg;

   /* Currently, the compiler only supports SIMD32 on SNB+ */
   if (!has_spilled &&
       (!v8 || v8->max_dispatch_width >= 32) &&
       (!v16 || v16->max_dispatch_width >= 32) && !params->use_rep_send &&
       !simd16_failed &&
       INTEL_SIMD(FS, 32)) {
      /* Try a SIMD32 compile */
      v32 = std::make_unique<fs_visitor>(compiler, &params->base, key,
                                         prog_data, nir, 32, 1,
                                         params->base.stats != NULL,
                                         debug_enabled);
      if (v8)
         v32->import_uniforms(v8.get());
      else if (v16)
         v32->import_uniforms(v16.get());

      if (!run_fs(*v32, allow_spilling, false)) {
         brw_shader_perf_log(compiler, params->base.log_data,
                             "SIMD32 shader failed to compile: %s\n",
                             v32->fail_msg);
      } else {
         const performance &perf = v32->performance_analysis.require();

         if (!INTEL_DEBUG(DEBUG_DO32) && throughput >= perf.throughput) {
            brw_shader_perf_log(compiler, params->base.log_data,
                                "SIMD32 shader inefficient\n");
         } else {
            simd32_cfg = v32->cfg;

            assert(v32->payload().num_regs % reg_unit(devinfo) == 0);
            prog_data->dispatch_grf_start_reg_32 = v32->payload().num_regs / reg_unit(devinfo);

            throughput = MAX2(throughput, perf.throughput);
         }
      }
   }

   if (devinfo->ver >= 12 && !has_spilled &&
       params->max_polygons >= 2 && !key->coarse_pixel) {
      fs_visitor *vbase = v8 ? v8.get() : v16 ? v16.get() : v32.get();
      assert(vbase);

      if (devinfo->ver >= 20 &&
          params->max_polygons >= 4 &&
          vbase->max_dispatch_width >= 32 &&
          4 * prog_data->num_varying_inputs <= MAX_VARYING &&
          INTEL_SIMD(FS, 4X8)) {
         /* Try a quad-SIMD8 compile */
         vmulti = std::make_unique<fs_visitor>(compiler, &params->base, key,
                                               prog_data, nir, 32, 4,
                                               params->base.stats != NULL,
                                               debug_enabled);
         vmulti->import_uniforms(vbase);
         if (!run_fs(*vmulti, false, params->use_rep_send)) {
            brw_shader_perf_log(compiler, params->base.log_data,
                                "Quad-SIMD8 shader failed to compile: %s\n",
                                vmulti->fail_msg);
         } else {
            multi_cfg = vmulti->cfg;
            assert(!vmulti->spilled_any_registers);
         }
      }

      if (!multi_cfg && devinfo->ver >= 20 &&
          vbase->max_dispatch_width >= 32 &&
          2 * prog_data->num_varying_inputs <= MAX_VARYING &&
          INTEL_SIMD(FS, 2X16)) {
         /* Try a dual-SIMD16 compile */
         vmulti = std::make_unique<fs_visitor>(compiler, &params->base, key,
                                               prog_data, nir, 32, 2,
                                               params->base.stats != NULL,
                                               debug_enabled);
         vmulti->import_uniforms(vbase);
         if (!run_fs(*vmulti, false, params->use_rep_send)) {
            brw_shader_perf_log(compiler, params->base.log_data,
                                "Dual-SIMD16 shader failed to compile: %s\n",
                                vmulti->fail_msg);
         } else {
            multi_cfg = vmulti->cfg;
            assert(!vmulti->spilled_any_registers);
         }
      }

      if (!multi_cfg && vbase->max_dispatch_width >= 16 &&
          2 * prog_data->num_varying_inputs <= MAX_VARYING &&
          INTEL_SIMD(FS, 2X8)) {
         /* Try a dual-SIMD8 compile */
         vmulti = std::make_unique<fs_visitor>(compiler, &params->base, key,
                                               prog_data, nir, 16, 2,
                                               params->base.stats != NULL,
                                               debug_enabled);
         vmulti->import_uniforms(vbase);
         if (!run_fs(*vmulti, allow_spilling, params->use_rep_send)) {
            brw_shader_perf_log(compiler, params->base.log_data,
                                "Dual-SIMD8 shader failed to compile: %s\n",
                                vmulti->fail_msg);
         } else {
            multi_cfg = vmulti->cfg;
         }
      }

      if (multi_cfg) {
         assert(vmulti->payload().num_regs % reg_unit(devinfo) == 0);
         prog_data->base.dispatch_grf_start_reg = vmulti->payload().num_regs / reg_unit(devinfo);
      }
   }

   /* When the caller requests a repclear shader, they want SIMD16-only */
   if (params->use_rep_send)
      simd8_cfg = NULL;

   fs_generator g(compiler, &params->base, &prog_data->base,
                  MESA_SHADER_FRAGMENT);

   if (unlikely(debug_enabled)) {
      g.enable_debug(ralloc_asprintf(params->base.mem_ctx,
                                     "%s fragment shader %s",
                                     nir->info.label ?
                                        nir->info.label : "unnamed",
                                     nir->info.name));
   }

   struct brw_compile_stats *stats = params->base.stats;
   uint32_t max_dispatch_width = 0;

   if (multi_cfg) {
      prog_data->dispatch_multi = vmulti->dispatch_width;
      prog_data->max_polygons = vmulti->max_polygons;
      g.generate_code(multi_cfg, vmulti->dispatch_width, vmulti->shader_stats,
                      vmulti->performance_analysis.require(),
                      stats, vmulti->max_polygons);
      stats = stats ? stats + 1 : NULL;
      max_dispatch_width = vmulti->dispatch_width;

   } else if (simd8_cfg) {
      prog_data->dispatch_8 = true;
      g.generate_code(simd8_cfg, 8, v8->shader_stats,
                      v8->performance_analysis.require(), stats, 1);
      stats = stats ? stats + 1 : NULL;
      max_dispatch_width = 8;
   }

   if (simd16_cfg) {
      prog_data->dispatch_16 = true;
      prog_data->prog_offset_16 = g.generate_code(
         simd16_cfg, 16, v16->shader_stats,
         v16->performance_analysis.require(), stats, 1);
      stats = stats ? stats + 1 : NULL;
      max_dispatch_width = 16;
   }

   if (simd32_cfg) {
      prog_data->dispatch_32 = true;
      prog_data->prog_offset_32 = g.generate_code(
         simd32_cfg, 32, v32->shader_stats,
         v32->performance_analysis.require(), stats, 1);
      stats = stats ? stats + 1 : NULL;
      max_dispatch_width = 32;
   }

   for (struct brw_compile_stats *s = params->base.stats; s != NULL && s != stats; s++)
      s->max_dispatch_width = max_dispatch_width;

   g.add_const_data(nir->constant_data, nir->constant_data_size);
   return g.get_assembly();
}
