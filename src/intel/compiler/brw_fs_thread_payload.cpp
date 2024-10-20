/*
 * Copyright © 2006-2022 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "brw_fs.h"
#include "brw_fs_builder.h"

using namespace brw;

vs_thread_payload::vs_thread_payload(const fs_visitor &v)
{
   unsigned r = 0;

   /* R0: Thread header. */
   r += reg_unit(v.devinfo);

   /* R1: URB handles. */
   urb_handles = brw_ud8_grf(r, 0);
   r += reg_unit(v.devinfo);

   num_regs = r;
}

tcs_thread_payload::tcs_thread_payload(const fs_visitor &v)
{
   struct brw_vue_prog_data *vue_prog_data = brw_vue_prog_data(v.prog_data);
   struct brw_tcs_prog_data *tcs_prog_data = brw_tcs_prog_data(v.prog_data);
   struct brw_tcs_prog_key *tcs_key = (struct brw_tcs_prog_key *) v.key;

   if (vue_prog_data->dispatch_mode == INTEL_DISPATCH_MODE_TCS_SINGLE_PATCH) {
      patch_urb_output = brw_ud1_grf(0, 0);
      primitive_id = brw_vec1_grf(0, 1);

      /* r1-r4 contain the ICP handles. */
      icp_handle_start = brw_ud8_grf(1, 0);

      num_regs = 5;
   } else {
      assert(vue_prog_data->dispatch_mode == INTEL_DISPATCH_MODE_TCS_MULTI_PATCH);
      assert(tcs_key->input_vertices <= BRW_MAX_TCS_INPUT_VERTICES);

      unsigned r = 0;

      r += reg_unit(v.devinfo);

      patch_urb_output = brw_ud8_grf(r, 0);
      r += reg_unit(v.devinfo);

      if (tcs_prog_data->include_primitive_id) {
         primitive_id = brw_vec8_grf(r, 0);
         r += reg_unit(v.devinfo);
      }

      /* ICP handles occupy the next 1-32 registers. */
      icp_handle_start = brw_ud8_grf(r, 0);
      r += brw_tcs_prog_key_input_vertices(tcs_key) * reg_unit(v.devinfo);

      num_regs = r;
   }
}

tes_thread_payload::tes_thread_payload(const fs_visitor &v)
{
   unsigned r = 0;

   /* R0: Thread Header. */
   patch_urb_input = retype(brw_vec1_grf(0, 0), BRW_TYPE_UD);
   primitive_id = brw_vec1_grf(0, 1);
   r += reg_unit(v.devinfo);

   /* R1-3: gl_TessCoord.xyz. */
   for (unsigned i = 0; i < 3; i++) {
      coords[i] = brw_vec8_grf(r, 0);
      r += reg_unit(v.devinfo);
   }

   /* R4: URB output handles. */
   urb_output = brw_ud8_grf(r, 0);
   r += reg_unit(v.devinfo);

   num_regs = r;
}

gs_thread_payload::gs_thread_payload(fs_visitor &v)
{
   struct brw_vue_prog_data *vue_prog_data = brw_vue_prog_data(v.prog_data);
   struct brw_gs_prog_data *gs_prog_data = brw_gs_prog_data(v.prog_data);
   const fs_builder bld = fs_builder(&v).at_end();

   /* R0: thread header. */
   unsigned r = reg_unit(v.devinfo);

   /* R1: output URB handles. */
   urb_handles = bld.vgrf(BRW_TYPE_UD);
   bld.AND(urb_handles, brw_ud8_grf(r, 0),
         v.devinfo->ver >= 20 ? brw_imm_ud(0xFFFFFF) : brw_imm_ud(0xFFFF));

   /* R1: Instance ID stored in bits 31:27 */
   instance_id = bld.vgrf(BRW_TYPE_UD);
   bld.SHR(instance_id, brw_ud8_grf(r, 0), brw_imm_ud(27u));

   r += reg_unit(v.devinfo);

   if (gs_prog_data->include_primitive_id) {
      primitive_id = brw_ud8_grf(r, 0);
      r += reg_unit(v.devinfo);
   }

   /* Always enable VUE handles so we can safely use pull model if needed.
    *
    * The push model for a GS uses a ton of register space even for trivial
    * scenarios with just a few inputs, so just make things easier and a bit
    * safer by always having pull model available.
    */
   gs_prog_data->base.include_vue_handles = true;

   /* R3..RN: ICP Handles for each incoming vertex (when using pull model) */
   icp_handle_start = brw_ud8_grf(r, 0);
   r += v.nir->info.gs.vertices_in * reg_unit(v.devinfo);

   num_regs = r;

   /* Use a maximum of 24 registers for push-model inputs. */
   const unsigned max_push_components = 24;

   /* If pushing our inputs would take too many registers, reduce the URB read
    * length (which is in HWords, or 8 registers), and resort to pulling.
    *
    * Note that the GS reads <URB Read Length> HWords for every vertex - so we
    * have to multiply by VerticesIn to obtain the total storage requirement.
    */
   if (8 * vue_prog_data->urb_read_length * v.nir->info.gs.vertices_in >
       max_push_components) {
      vue_prog_data->urb_read_length =
         ROUND_DOWN_TO(max_push_components / v.nir->info.gs.vertices_in, 8) / 8;
   }
}

static inline void
setup_fs_payload_gfx20(fs_thread_payload &payload,
                       const fs_visitor &v,
                       bool &source_depth_to_render_target)
{
   struct brw_wm_prog_data *prog_data = brw_wm_prog_data(v.prog_data);
   const unsigned payload_width = 16;
   assert(v.dispatch_width % payload_width == 0);
   assert(v.devinfo->ver >= 20);

   for (unsigned j = 0; j < v.dispatch_width / payload_width; j++) {
      /* R0-1: PS thread payload header, masks and pixel X/Y coordinates. */
      payload.num_regs++;
      payload.subspan_coord_reg[j] = payload.num_regs++;
   }

   for (unsigned j = 0; j < v.dispatch_width / payload_width; j++) {
      /* R2-13: Barycentric interpolation coordinates.  These appear
       * in the same order that they appear in the brw_barycentric_mode
       * enum.  Each set of coordinates occupies 2 64B registers per
       * SIMD16 half.  Coordinates only appear if they were enabled
       * using the "Barycentric Interpolation Mode" bits in WM_STATE.
       */
      for (int i = 0; i < BRW_BARYCENTRIC_MODE_COUNT; ++i) {
         if (prog_data->barycentric_interp_modes & (1 << i)) {
            payload.barycentric_coord_reg[i][j] = payload.num_regs;
            payload.num_regs += payload_width / 4;
         }
      }

      /* R14: Interpolated depth if "Pixel Shader Uses Source Depth" is set. */
      if (prog_data->uses_src_depth) {
         payload.source_depth_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R15: Interpolated W if "Pixel Shader Uses Source W" is set. */
      if (prog_data->uses_src_w) {
         payload.source_w_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R16: MSAA input coverage mask if "Pixel Shader Uses Input
       * Coverage Mask" is set.
       */
      if (prog_data->uses_sample_mask) {
         payload.sample_mask_in_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R19: MSAA position XY offsets if "Position XY Offset Select"
       * is either POSOFFSET_CENTROID or POSOFFSET_SAMPLE.  Note that
       * this is delivered as a single SIMD32 vector, inconsistently
       * with most other PS payload fields.
       */
      if (prog_data->uses_pos_offset && j == 0) {
         for (unsigned k = 0; k < 2; k++) {
            payload.sample_pos_reg[k] = payload.num_regs;
            payload.num_regs++;
         }
      }

      /* R22: Sample offsets. */
      if (prog_data->uses_sample_offsets && j == 0) {
         payload.sample_offsets_reg = payload.num_regs;
         payload.num_regs += 2;
      }
   }

   /* RP0: Source Depth and/or W Attribute Vertex Deltas and/or
    * Perspective Bary Planes.
    */
   if (prog_data->uses_depth_w_coefficients ||
       prog_data->uses_pc_bary_coefficients) {
      payload.depth_w_coef_reg = payload.pc_bary_coef_reg = payload.num_regs;
      payload.num_regs += 2 * v.max_polygons;
   }

   /* RP4: Non-Perspective Bary planes. */
   if (prog_data->uses_npc_bary_coefficients) {
      payload.npc_bary_coef_reg = payload.num_regs;
      payload.num_regs += 2 * v.max_polygons;
   }

   if (v.nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
      source_depth_to_render_target = true;
   }
}

static inline void
setup_fs_payload_gfx9(fs_thread_payload &payload,
                      const fs_visitor &v,
                      bool &source_depth_to_render_target)
{
   struct brw_wm_prog_data *prog_data = brw_wm_prog_data(v.prog_data);

   const unsigned payload_width = MIN2(16, v.dispatch_width);
   assert(v.dispatch_width % payload_width == 0);
   assert(v.devinfo->ver < 20);

   payload.num_regs = 0;

   /* R0: PS thread payload header. */
   payload.num_regs++;

   for (unsigned j = 0; j < v.dispatch_width / payload_width; j++) {
      /* R1: masks, pixel X/Y coordinates. */
      payload.subspan_coord_reg[j] = payload.num_regs++;
   }

   for (unsigned j = 0; j < v.dispatch_width / payload_width; j++) {
      /* R3-26: barycentric interpolation coordinates.  These appear in the
       * same order that they appear in the brw_barycentric_mode enum.  Each
       * set of coordinates occupies 2 registers if dispatch width == 8 and 4
       * registers if dispatch width == 16.  Coordinates only appear if they
       * were enabled using the "Barycentric Interpolation Mode" bits in
       * WM_STATE.
       */
      for (int i = 0; i < BRW_BARYCENTRIC_MODE_COUNT; ++i) {
         if (prog_data->barycentric_interp_modes & (1 << i)) {
            payload.barycentric_coord_reg[i][j] = payload.num_regs;
            payload.num_regs += payload_width / 4;
         }
      }

      /* R27-28: interpolated depth if uses source depth */
      if (prog_data->uses_src_depth) {
         payload.source_depth_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R29-30: interpolated W set if GFX6_WM_USES_SOURCE_W. */
      if (prog_data->uses_src_w) {
         payload.source_w_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }

      /* R31: MSAA position offsets. */
      if (prog_data->uses_pos_offset) {
         payload.sample_pos_reg[j] = payload.num_regs;
         payload.num_regs++;
      }

      /* R32-33: MSAA input coverage mask */
      if (prog_data->uses_sample_mask) {
         payload.sample_mask_in_reg[j] = payload.num_regs;
         payload.num_regs += payload_width / 8;
      }
   }

   /* R66: Source Depth and/or W Attribute Vertex Deltas. */
   if (prog_data->uses_depth_w_coefficients) {
      payload.depth_w_coef_reg = payload.num_regs;
      payload.num_regs += v.max_polygons;
   }

   /* R68: Perspective bary planes. */
   if (prog_data->uses_pc_bary_coefficients) {
      payload.pc_bary_coef_reg = payload.num_regs;
      payload.num_regs += v.max_polygons;
   }

   /* R70: Non-perspective bary planes. */
   if (prog_data->uses_npc_bary_coefficients) {
      payload.npc_bary_coef_reg = payload.num_regs;
      payload.num_regs += v.max_polygons;
   }

   /* R72: Sample offsets. */
   if (prog_data->uses_sample_offsets) {
      payload.sample_offsets_reg = payload.num_regs;
      payload.num_regs++;
   }

   if (v.nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
      source_depth_to_render_target = true;
   }
}

fs_thread_payload::fs_thread_payload(const fs_visitor &v,
                                     bool &source_depth_to_render_target)
  : subspan_coord_reg(),
    source_depth_reg(),
    source_w_reg(),
    aa_dest_stencil_reg(),
    dest_depth_reg(),
    sample_pos_reg(),
    sample_mask_in_reg(),
    barycentric_coord_reg(),
    depth_w_coef_reg(),
    pc_bary_coef_reg(),
    npc_bary_coef_reg(),
    sample_offsets_reg()
{
   if (v.devinfo->ver >= 20)
      setup_fs_payload_gfx20(*this, v, source_depth_to_render_target);
   else
      setup_fs_payload_gfx9(*this, v, source_depth_to_render_target);
}

cs_thread_payload::cs_thread_payload(const fs_visitor &v)
{
   struct brw_cs_prog_data *prog_data = brw_cs_prog_data(v.prog_data);

   unsigned r = reg_unit(v.devinfo);

   /* See nir_setup_uniforms for subgroup_id in earlier versions. */
   if (v.devinfo->verx10 >= 125) {
      subgroup_id_ = brw_ud1_grf(0, 2);

      for (int i = 0; i < 3; i++) {
         if (prog_data->generate_local_id & (1 << i)) {
            local_invocation_id[i] = brw_uw8_grf(r, 0);
            r += reg_unit(v.devinfo);
            if (v.devinfo->ver < 20 && v.dispatch_width == 32)
               r += reg_unit(v.devinfo);
         } else {
            local_invocation_id[i] = brw_imm_uw(0);
         }
      }

      /* TODO: Fill out uses_btd_stack_ids automatically */
      if (prog_data->uses_btd_stack_ids)
         r += reg_unit(v.devinfo);

      if (v.stage == MESA_SHADER_COMPUTE && prog_data->uses_inline_data) {
         inline_parameter = brw_ud1_grf(r, 0);
         r += reg_unit(v.devinfo);
      }
   }

   num_regs = r;
}

void
cs_thread_payload::load_subgroup_id(const fs_builder &bld,
                                    brw_reg &dest) const
{
   auto devinfo = bld.shader->devinfo;
   dest = retype(dest, BRW_TYPE_UD);

   if (subgroup_id_.file != BAD_FILE) {
      assert(devinfo->verx10 >= 125);
      bld.AND(dest, subgroup_id_, brw_imm_ud(INTEL_MASK(7, 0)));
   } else {
      assert(devinfo->verx10 < 125);
      assert(gl_shader_stage_is_compute(bld.shader->stage));
      int index = brw_get_subgroup_id_param_index(devinfo,
                                                  bld.shader->prog_data);
      bld.MOV(dest, brw_uniform_reg(index, BRW_TYPE_UD));
   }
}

task_mesh_thread_payload::task_mesh_thread_payload(fs_visitor &v)
   : cs_thread_payload(v)
{
   /* Task and Mesh Shader Payloads (SIMD8 and SIMD16)
    *
    *  R0: Header
    *  R1: Local_ID.X[0-7 or 0-15]
    *  R2: Inline Parameter
    *
    * Task and Mesh Shader Payloads (SIMD32)
    *
    *  R0: Header
    *  R1: Local_ID.X[0-15]
    *  R2: Local_ID.X[16-31]
    *  R3: Inline Parameter
    *
    * Local_ID.X values are 16 bits.
    *
    * Inline parameter is optional but always present since we use it to pass
    * the address to descriptors.
    */

   const fs_builder bld = fs_builder(&v).at_end();

   unsigned r = 0;
   assert(subgroup_id_.file != BAD_FILE);
   extended_parameter_0 = retype(brw_vec1_grf(0, 3), BRW_TYPE_UD);

   if (v.devinfo->ver >= 20) {
      urb_output = brw_ud1_grf(1, 0);
   } else {
      urb_output = bld.vgrf(BRW_TYPE_UD);
      /* In both mesh and task shader payload, lower 16 bits of g0.6 is
       * an offset within Slice's Local URB, which says where shader is
       * supposed to output its data.
       */
      bld.AND(urb_output, brw_ud1_grf(0, 6), brw_imm_ud(0xFFFF));
   }

   if (v.stage == MESA_SHADER_MESH) {
      /* g0.7 is Task Shader URB Entry Offset, which contains both an offset
       * within Slice's Local USB (bits 0:15) and a slice selector
       * (bits 16:24). Slice selector can be non zero when mesh shader
       * is spawned on slice other than the one where task shader was run.
       * Bit 24 says that Slice ID is present and bits 16:23 is the Slice ID.
       */
      task_urb_input = brw_ud1_grf(0, 7);
   }
   r += reg_unit(v.devinfo);

   local_index = brw_uw8_grf(r, 0);
   r += reg_unit(v.devinfo);
   if (v.devinfo->ver < 20 && v.dispatch_width == 32)
      r += reg_unit(v.devinfo);

   struct brw_cs_prog_data *prog_data = brw_cs_prog_data(v.prog_data);
   if (prog_data->uses_inline_data) {
      inline_parameter = brw_ud1_grf(r, 0);
      r += reg_unit(v.devinfo);
   }

   num_regs = r;
}

bs_thread_payload::bs_thread_payload(const fs_visitor &v)
{
   unsigned r = 0;

   /* R0: Thread header. */
   r += reg_unit(v.devinfo);

   /* R1: Stack IDs. */
   r += reg_unit(v.devinfo);

   /* R2: Inline Parameter.  Used for argument addresses. */
   global_arg_ptr = brw_ud1_grf(r, 0);
   local_arg_ptr = brw_ud1_grf(r, 2);
   r += reg_unit(v.devinfo);

   num_regs = r;
}

void
bs_thread_payload::load_shader_type(const fs_builder &bld, brw_reg &dest) const
{
   brw_reg ud_dest = retype(dest, BRW_TYPE_UD);
   bld.MOV(ud_dest, retype(brw_vec1_grf(0, 3), ud_dest.type));
   bld.AND(ud_dest, ud_dest, brw_imm_ud(0xf));
}
