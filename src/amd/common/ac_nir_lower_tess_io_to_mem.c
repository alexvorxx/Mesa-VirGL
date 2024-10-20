/*
 * Copyright © 2021 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_nir.h"
#include "ac_nir_helpers.h"
#include "nir_builder.h"

/*
 * These NIR passes are used to lower NIR cross-stage I/O intrinsics into the
 * memory accesses that actually happen on the HW.
 *
 * Each input and output has a 16-byte (4 dwords) slot reserved for it, and
 * can have up to 4 components. Each component is 32 bits.
 *
 * ## VS-TCS-TES I/O - Terminology:
 *
 * * patch - Group of vertices, used instead of primitives in tessellation
 * * per-vertex - input or output which can be different for every vertex.
 * * per-patch - input output which applies to a patch (a group of vertices)
 *
 * ## VS-TCS-TES I/O - How it works:
 *
 * ```
 * SW model:    SW VS         SW TCS    tessellator    SW TES
 *                ┊             ┊             ┊          ┊
 *              ┌────┐        ┌────┐        ┌────┐    ┌─────┐
 * HW pipeline: │ LS │─╮   ╭─>│ HS │─╮   ╭─>│ FF │ ╭─>│VS/ES│
 *              └────┘ │   │  └────┘ │   │  └────┘ │  └─────┘
 * Memory:             ╰─>LDS<──╯    ╰─>VRAM───────╯
 * ```
 *
 * * SW VS runs as a HW LS (Local Shader, merged into HS on GFX9+),
 *   and SW TCS runs as HW HS (Hull Shader).
 *   SW TES runs as either HW VS or HW ES (Export Shader).
 * * LS and HS share the same LDS space.
 * * LS (SW VS) stores outputs to LDS to be read by HS (SW TCS).
 * * HS (SW TCS) stores outputs in LDS if the HS (SW TCS) reads them.
 * * HS (SW TCS) stores outputs in VRAM if the next stage (SW TES) reads them.
 *
 * Side note: some old HW supports having TES read from the same LDS space where LS/HS write, but
 * Mesa always stores HS outputs to VRAM to avoid forcing TES waves to run on the same CU as the LS/HS waves.
 *
 * ### Passing VS-TCS I/O in registers
 *
 * On GPUs that run SW VS and  SW TCS on the same HW stage (HS on GFX9+),
 * IO can be passed through registers instead of LDS when the following conditions are met:
 *
 * 1. TCS input and output patch size match
 * 2. Floating point execution modes in SW VS and SW TCS match
 * 3. The SW VS output is not written indirectly, and the corresponding SW TCS input is not read indirectly
 *
 * Some HS outputs could be passed through registers to, but this is a TODO.
 *
 * ### LDS layout used by VS-TCS:
 *
 * ```
 * TCS per-vertex inputs for patch 0  <─── 0
 * TCS per-vertex inputs for patch 1
 * TCS per-vertex inputs for patch 2  <─── hs_per_vertex_input_lds_offset (rel_patch_id = 2)
 * ...
 * TCS per-vertex outputs for patch 0 <─── output_patch0_offset
 * TCS per-patch outputs for patch 0  <─── output_patch0_patch_data_offset
 * TCS per-vertex outputs for patch 1
 * TCS per-patch outputs for patch 1
 * TCS per-vertex outputs for patch 2 <─── hs_output_lds_offset (rel_patch_id = 2, per-vertex)
 * TCS per-patch outputs for patch 2  <─── hs_output_lds_offset (rel_patch_id = 2, per-patch)
 * ...
 * ```
 *
 * ### VRAM layout used by TCS-TES I/O:
 *
 * ```
 * attr 0 of patch 0 vertex 0   <─── "off-chip LDS" offset
 * attr 0 of patch 0 vertex 1
 * attr 0 of patch 0 vertex 2
 * ...
 * attr 0 of patch 1 vertex 0
 * attr 0 of patch 1 vertex 1
 * attr 0 of patch 1 vertex 2   <─── hs_per_vertex_output_vmem_offset (attribute slot = 0, rel_patch_id = 1, vertex index = 1)
 * ...
 * attr 0 of patch 2 vertex 0
 * attr 0 of patch 2 vertex 1
 * attr 0 of patch 2 vertex 2
 * ...
 * attr 1 of patch 0 vertex 0
 * attr 1 of patch 0 vertex 1
 * attr 1 of patch 0 vertex 2
 * ...
 * ...
 * per-patch attr 0 of patch 0  <─── hs_out_patch_data_offset_amd
 * per-patch attr 0 of patch 1
 * per-patch attr 0 of patch 2  <─── hs_per_patch_output_vmem_offset (attribute slot = 0, rel_patch_id = 2)
 * ...
 * per-patch attr 1 of patch 0
 * per-patch attr 1 of patch 1
 * per-patch attr 1 of patch 2
 * ...
 * ```
 *
 */

typedef struct {
   /* Which hardware generation we're dealing with */
   enum amd_gfx_level gfx_level;

   /* I/O semantic -> real location used by lowering. */
   ac_nir_map_io_driver_location map_io;

   /* True if merged VS+TCS (on GFX9+) has the same number
    * of input and output patch size.
    */
   bool tcs_in_out_eq;

   /* Bit mask of TCS per-vertex inputs (VS outputs) which
    * are passed between the two stages only in temporaries (registers).
    *
    * A VS output can be passed to TCS in registers when:
    * - VS is known to write, and TCS is known to read it
    * - Neither VS nor TCS accesses it indirecty
    * - There are no TCS cross-invocation reads to this input
    */
   uint64_t tcs_temp_only_inputs;

   /* Bit mask of inputs read by the TCS,
    * this is used for linking VS outputs to TCS inputs.
    */
   uint64_t tcs_inputs_read;

   /* Bit mask of TCS outputs read by TES. */
   uint64_t tes_inputs_read;
   uint32_t tes_patch_inputs_read;

   /* True if the output patch fits the subgroup, so all TCS outputs are always written in the same
    * subgroup that reads them.
    */
   bool tcs_out_patch_fits_subgroup;

   /* Set if all invocations will write to all tess factors, so tess factors
    * can be passed by register.
    */
   bool tcs_pass_tessfactors_by_reg;

   /* Save TCS tess factor for tess factor writer. */
   nir_variable *tcs_tess_level_outer;
   nir_variable *tcs_tess_level_inner;
   unsigned tcs_tess_level_outer_base;
   unsigned tcs_tess_level_outer_mask;
   unsigned tcs_tess_level_inner_base;
   unsigned tcs_tess_level_inner_mask;
} lower_tess_io_state;

typedef struct {
   nir_def *outer;
   nir_def *inner;
} tess_levels;

#define TESS_LVL_MASK (VARYING_BIT_TESS_LEVEL_OUTER | VARYING_BIT_TESS_LEVEL_INNER)

static uint64_t
tcs_vram_per_vtx_out_mask(nir_shader *shader, lower_tess_io_state *st)
{
   return st->tes_inputs_read & ~TESS_LVL_MASK;
}

static uint32_t
tcs_vram_tf_out_mask(nir_shader *shader, lower_tess_io_state *st)
{
   return st->tes_inputs_read & TESS_LVL_MASK;
}

static uint32_t
tcs_vram_per_patch_out_mask(nir_shader *shader, lower_tess_io_state *st)
{
   return st->tes_patch_inputs_read;
}

static bool
tcs_output_needs_vmem(nir_intrinsic_instr *intrin,
                      nir_shader *shader,
                      lower_tess_io_state *st)
{
   /* no_varying indicates that TES doesn't read the output. */
   if (nir_intrinsic_io_semantics(intrin).no_varying)
      return false;

   const unsigned loc = nir_intrinsic_io_semantics(intrin).location;
   const bool per_vertex = intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
                           intrin->intrinsic == nir_intrinsic_load_per_vertex_output;

   if (per_vertex) {
      return tcs_vram_per_vtx_out_mask(shader, st) & BITFIELD64_BIT(loc);
   } else if (loc == VARYING_SLOT_TESS_LEVEL_OUTER || loc == VARYING_SLOT_TESS_LEVEL_INNER) {
      return false;
   } else {
      return tcs_vram_per_patch_out_mask(shader, st) & BITFIELD_BIT(loc - VARYING_SLOT_PATCH0);
   }
}

static uint64_t
tcs_lds_per_vtx_out_mask(nir_shader *shader)
{
   return shader->info.outputs_read & shader->info.outputs_written & ~TESS_LVL_MASK;
}

static uint64_t
tcs_lds_tf_out_mask(nir_shader *shader, lower_tess_io_state *st)
{
   return st->tcs_pass_tessfactors_by_reg ? 0ull : (shader->info.outputs_written & TESS_LVL_MASK);
}

static uint32_t
tcs_lds_per_patch_out_mask(nir_shader *shader)
{
   return shader->info.patch_outputs_read & shader->info.patch_outputs_written;
}

static bool
tcs_output_needs_lds(nir_intrinsic_instr *intrin,
                     nir_shader *shader,
                     lower_tess_io_state *st)
{
   const unsigned loc = nir_intrinsic_io_semantics(intrin).location;
   const bool per_vertex = intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
                           intrin->intrinsic == nir_intrinsic_load_per_vertex_output;

   if (per_vertex) {
      return tcs_lds_per_vtx_out_mask(shader) & BITFIELD64_BIT(loc);
   } else if (loc == VARYING_SLOT_TESS_LEVEL_OUTER || loc == VARYING_SLOT_TESS_LEVEL_INNER) {
      return tcs_lds_tf_out_mask(shader, st) & BITFIELD64_BIT(loc);
   } else {
      return tcs_lds_per_patch_out_mask(shader) & BITFIELD_BIT(loc - VARYING_SLOT_PATCH0);
   }
}

static bool
lower_ls_output_store(nir_builder *b,
                      nir_intrinsic_instr *intrin,
                      void *state)
{
   if (intrin->intrinsic != nir_intrinsic_store_output)
      return false;

   /* The ARB_shader_viewport_layer_array spec contains the
    * following issue:
    *
    *    2) What happens if gl_ViewportIndex or gl_Layer is
    *    written in the vertex shader and a geometry shader is
    *    present?
    *
    *    RESOLVED: The value written by the last vertex processing
    *    stage is used. If the last vertex processing stage
    *    (vertex, tessellation evaluation or geometry) does not
    *    statically assign to gl_ViewportIndex or gl_Layer, index
    *    or layer zero is assumed.
    *
    * So writes to those outputs in VS-as-LS are simply ignored.
    */
   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   if (io_sem.location == VARYING_SLOT_LAYER || io_sem.location == VARYING_SLOT_VIEWPORT) {
      nir_instr_remove(&intrin->instr);
      return true;
   }

   lower_tess_io_state *st = (lower_tess_io_state *) state;

   /* When a VS output isn't read by TCS, don't emit anything. */
   if ((io_sem.no_varying || !(st->tcs_inputs_read & BITFIELD64_BIT(io_sem.location)))) {
      nir_instr_remove(&intrin->instr);
      return true;
   }

   /* If this is a temp-only TCS input, we don't need to use shared memory at all. */
   if (st->tcs_temp_only_inputs & BITFIELD64_BIT(io_sem.location))
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *vertex_idx = nir_load_local_invocation_index(b);
   nir_def *base_off_var = nir_imul(b, vertex_idx, nir_load_lshs_vertex_stride_amd(b));

   unsigned mapped = ac_nir_map_io_location(io_sem.location, st->tcs_inputs_read & ~st->tcs_temp_only_inputs,
                                            st->map_io);
   nir_def *io_off = ac_nir_calc_io_off(b, intrin, nir_imm_int(b, 16u), 4u, mapped);
   unsigned write_mask = nir_intrinsic_write_mask(intrin);

   nir_def *off = nir_iadd_nuw(b, base_off_var, io_off);
   AC_NIR_STORE_IO(b, intrin->src[0].ssa, 0, write_mask, io_sem.high_16bits,
                   nir_store_shared, off, .write_mask = store_write_mask, .base = store_const_offset);

   /* NOTE: don't remove the store_output intrinsic on GFX9+ when tcs_in_out_eq,
    * it will be used by same-invocation TCS input loads.
    */
   if (!st->tcs_in_out_eq)
      nir_instr_remove(&intrin->instr);

   return true;
}

static bool
filter_load_tcs_per_vertex_input(const nir_instr *instr,
                                 UNUSED const void *state)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   lower_tess_io_state *st = (lower_tess_io_state *) state;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic != nir_intrinsic_load_per_vertex_input)
      return false;
   if (!st->tcs_in_out_eq)
      return true;

   /* tcs_in_out_eq: a same-invocation input load, without indirect offset,
    * can use temporaries, no need to use shared memory.
    */
   nir_src *off_src = nir_get_io_offset_src(intrin);
   nir_src *vertex_index_src = nir_get_io_arrayed_index_src(intrin);
   nir_instr *vertex_index_instr = vertex_index_src->ssa->parent_instr;


   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);

   /* If this is a temp-only TCS input, we don't need to use shared memory at all. */
   if (st->tcs_temp_only_inputs & BITFIELD64_BIT(io_sem.location)) {
      ASSERTED bool can_use_temps =
         nir_src_is_const(*off_src) &&
         vertex_index_instr->type == nir_instr_type_intrinsic &&
         nir_instr_as_intrinsic(vertex_index_instr)->intrinsic == nir_intrinsic_load_invocation_id;

      assert(can_use_temps);
      return false;
   }

   return true;
}

static nir_def *
hs_per_vertex_input_lds_offset(nir_builder *b,
                               lower_tess_io_state *st,
                               nir_intrinsic_instr *instr)
{
   nir_def *tcs_in_vtxcnt = nir_load_patch_vertices_in(b);
   nir_def *rel_patch_id = nir_load_tess_rel_patch_id_amd(b);
   nir_def *vertex_index = nir_get_io_arrayed_index_src(instr)->ssa;

   nir_def *stride = nir_load_lshs_vertex_stride_amd(b);
   nir_def *tcs_in_patch_stride = nir_imul(b, tcs_in_vtxcnt, stride);
   nir_def *vertex_index_off = nir_imul(b, vertex_index, stride);

   nir_def *tcs_in_current_patch_offset = nir_imul(b, rel_patch_id, tcs_in_patch_stride);

   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(instr);
   const unsigned mapped = ac_nir_map_io_location(io_sem.location, st->tcs_inputs_read & ~st->tcs_temp_only_inputs,
                                                  st->map_io);
   nir_def *io_offset = ac_nir_calc_io_off(b, instr, nir_imm_int(b, 16u), 4u, mapped);

   return nir_iadd_nuw(b, nir_iadd_nuw(b, tcs_in_current_patch_offset, vertex_index_off), io_offset);
}

static unsigned
hs_output_lds_map_io_location(nir_shader *shader,
                              const bool per_vertex,
                              const unsigned loc,
                              lower_tess_io_state *st)
{
   if (!per_vertex) {
      const uint64_t tf_mask = tcs_lds_tf_out_mask(shader, st);
      if (loc == VARYING_SLOT_TESS_LEVEL_INNER || loc == VARYING_SLOT_TESS_LEVEL_OUTER) {
         assert(tf_mask & BITFIELD64_BIT(loc));
         return util_bitcount64(tf_mask & BITFIELD64_MASK(loc));
      }

      const uint32_t patch_out_mask = tcs_lds_per_patch_out_mask(shader);
      assert(patch_out_mask & BITFIELD_BIT(loc - VARYING_SLOT_PATCH0));
      return util_bitcount64(tf_mask) +
             util_bitcount(patch_out_mask & BITFIELD_MASK(loc - VARYING_SLOT_PATCH0));
   } else {
      const uint64_t per_vertex_mask = tcs_lds_per_vtx_out_mask(shader);
      assert(per_vertex_mask & BITFIELD64_BIT(loc));
      return util_bitcount64(per_vertex_mask & BITFIELD64_MASK(loc));
   }
}

static nir_def *
hs_output_lds_offset(nir_builder *b,
                     lower_tess_io_state *st,
                     nir_intrinsic_instr *intrin)
{
   bool per_vertex = intrin &&
                     (intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
                      intrin->intrinsic == nir_intrinsic_load_per_vertex_output);

   const uint64_t per_vertex_mask = tcs_lds_per_vtx_out_mask(b->shader);
   const uint64_t tf_mask = tcs_lds_tf_out_mask(b->shader, st);
   const uint32_t patch_out_mask = tcs_lds_per_patch_out_mask(b->shader);

   unsigned tcs_num_reserved_outputs = util_bitcount64(per_vertex_mask);
   unsigned tcs_num_reserved_patch_outputs = util_bitcount64(tf_mask) + util_bitcount(patch_out_mask);
   unsigned output_vertex_size = tcs_num_reserved_outputs * 16u;
   unsigned pervertex_output_patch_size = b->shader->info.tess.tcs_vertices_out * output_vertex_size;
   unsigned output_patch_stride = pervertex_output_patch_size + tcs_num_reserved_patch_outputs * 16u;

   nir_def *off = NULL;

   if (intrin) {
      const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
      const unsigned mapped = hs_output_lds_map_io_location(b->shader, per_vertex, io_sem.location, st);
      off = ac_nir_calc_io_off(b, intrin, nir_imm_int(b, 16u), 4, mapped);
   } else {
      off = nir_imm_int(b, 0);
   }

   nir_def *rel_patch_id = nir_load_tess_rel_patch_id_amd(b);
   nir_def *patch_offset = nir_imul_imm(b, rel_patch_id, output_patch_stride);

   nir_def *tcs_in_vtxcnt = nir_load_patch_vertices_in(b);
   nir_def *tcs_num_patches = nir_load_tcs_num_patches_amd(b);
   nir_def *input_patch_size = nir_imul(b, tcs_in_vtxcnt, nir_load_lshs_vertex_stride_amd(b));
   nir_def *output_patch0_offset = nir_imul(b, input_patch_size, tcs_num_patches);
   nir_def *output_patch_offset = nir_iadd_nuw(b, patch_offset, output_patch0_offset);

   if (per_vertex) {
      nir_def *vertex_index = nir_get_io_arrayed_index_src(intrin)->ssa;
      nir_def *vertex_index_off = nir_imul_imm(b, vertex_index, output_vertex_size);

      off = nir_iadd_nuw(b, off, vertex_index_off);
      return nir_iadd_nuw(b, off, output_patch_offset);
   } else {
      off = nir_iadd_imm_nuw(b, off, pervertex_output_patch_size);
      return nir_iadd_nuw(b, off, output_patch_offset);
   }
}

static unsigned
hs_output_vram_map_io_location(nir_shader *shader,
                               const bool per_vertex,
                               const unsigned loc,
                               lower_tess_io_state *st)
{
   /* Unlinked shaders:
    * We are unaware of TES inputs while lowering TCS outputs.
    * The driver needs to pass a callback to map varyings to a fixed location.
    */
   if (st->map_io)
      return st->map_io(loc);

   /* Linked shaders:
    * Take advantage of having knowledge of TES inputs while lowering TCS outputs.
    * Map varyings to a prefix sum of the IO mask to save space in VRAM.
    */
   if (!per_vertex) {
      const uint64_t tf_mask = tcs_vram_tf_out_mask(shader, st);
      if (loc == VARYING_SLOT_TESS_LEVEL_INNER || loc == VARYING_SLOT_TESS_LEVEL_OUTER) {
         assert(tf_mask & BITFIELD64_BIT(loc));
         return util_bitcount64(tf_mask & BITFIELD64_MASK(loc));
      }

      const uint32_t patch_out_mask = tcs_vram_per_patch_out_mask(shader, st);
      assert(patch_out_mask & BITFIELD_BIT(loc - VARYING_SLOT_PATCH0));
      return util_bitcount64(tf_mask) +
             util_bitcount(patch_out_mask & BITFIELD_MASK(loc - VARYING_SLOT_PATCH0));
   } else {
      const uint64_t per_vertex_mask = tcs_vram_per_vtx_out_mask(shader, st);
      assert(per_vertex_mask & BITFIELD64_BIT(loc));
      return util_bitcount64(per_vertex_mask & BITFIELD64_MASK(loc));
   }
}

static nir_def *
hs_per_vertex_output_vmem_offset(nir_builder *b,
                                 lower_tess_io_state *st,
                                 nir_intrinsic_instr *intrin)
{
   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);

   nir_def *out_vertices_per_patch = b->shader->info.stage == MESA_SHADER_TESS_CTRL
                                         ? nir_imm_int(b, b->shader->info.tess.tcs_vertices_out)
                                         : nir_load_patch_vertices_in(b);

   nir_def *tcs_num_patches = nir_load_tcs_num_patches_amd(b);
   nir_def *attr_stride = nir_imul(b, tcs_num_patches, nir_imul_imm(b, out_vertices_per_patch, 16u));
   nir_def *io_offset =
      ac_nir_calc_io_off(b, intrin, attr_stride, 4u,
                                   hs_output_vram_map_io_location(b->shader, true, io_sem.location, st));

   nir_def *rel_patch_id = nir_load_tess_rel_patch_id_amd(b);
   nir_def *patch_offset = nir_imul(b, rel_patch_id, nir_imul_imm(b, out_vertices_per_patch, 16u));

   nir_def *vertex_index = nir_get_io_arrayed_index_src(intrin)->ssa;
   nir_def *vertex_index_off = nir_imul_imm(b, vertex_index, 16u);

   return nir_iadd_nuw(b, nir_iadd_nuw(b, patch_offset, vertex_index_off), io_offset);
}

static nir_def *
hs_per_patch_output_vmem_offset(nir_builder *b,
                                lower_tess_io_state *st,
                                nir_intrinsic_instr *intrin,
                                unsigned const_base_offset)
{
   nir_def *tcs_num_patches = nir_load_tcs_num_patches_amd(b);
   nir_def *per_patch_data_offset = nir_load_hs_out_patch_data_offset_amd(b);

   nir_def * off =
      intrin
      ? ac_nir_calc_io_off(b, intrin, nir_imul_imm(b, tcs_num_patches, 16u), 4u,
                                     hs_output_vram_map_io_location(b->shader, false, nir_intrinsic_io_semantics(intrin).location, st))
      : nir_imm_int(b, 0);

   if (const_base_offset)
      off = nir_iadd_nuw(b, off, nir_imul_imm(b, tcs_num_patches, const_base_offset));

   nir_def *rel_patch_id = nir_load_tess_rel_patch_id_amd(b);
   nir_def *patch_offset = nir_imul_imm(b, rel_patch_id, 16u);
   off = nir_iadd_nuw(b, off, per_patch_data_offset);
   return nir_iadd_nuw(b, off, patch_offset);
}

static nir_def *
lower_hs_per_vertex_input_load(nir_builder *b,
                               nir_instr *instr,
                               void *state)
{
   lower_tess_io_state *st = (lower_tess_io_state *) state;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   nir_def *off = hs_per_vertex_input_lds_offset(b, st, intrin);
   nir_def *load = NULL;

   AC_NIR_LOAD_IO(load, b, intrin->def.num_components, intrin->def.bit_size, io_sem.high_16bits,
                  nir_load_shared, off);

   return load;
}

static nir_def *
lower_hs_output_store(nir_builder *b,
                      nir_intrinsic_instr *intrin,
                      lower_tess_io_state *st)
{
   assert(intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
          intrin->intrinsic == nir_intrinsic_store_output);

   nir_io_semantics semantics = nir_intrinsic_io_semantics(intrin);
   nir_def *store_val = intrin->src[0].ssa;
   const unsigned write_mask = nir_intrinsic_write_mask(intrin);
   const bool write_to_vmem = tcs_output_needs_vmem(intrin, b->shader, st);
   const bool write_to_lds =  tcs_output_needs_lds(intrin, b->shader, st);

   if (write_to_vmem) {
      nir_def *vmem_off = intrin->intrinsic == nir_intrinsic_store_per_vertex_output
                            ? hs_per_vertex_output_vmem_offset(b, st, intrin)
                            : hs_per_patch_output_vmem_offset(b, st, intrin, 0);

      nir_def *hs_ring_tess_offchip = nir_load_ring_tess_offchip_amd(b);
      nir_def *offchip_offset = nir_load_ring_tess_offchip_offset_amd(b);
      nir_def *zero = nir_imm_int(b, 0);
      AC_NIR_STORE_IO(b, store_val, 0, write_mask, semantics.high_16bits,
                      nir_store_buffer_amd, hs_ring_tess_offchip, vmem_off, offchip_offset, zero,
                      .write_mask = store_write_mask, .base = store_const_offset,
                      .memory_modes = nir_var_shader_out, .access = ACCESS_COHERENT);
   }

   if (write_to_lds) {
      nir_def *lds_off = hs_output_lds_offset(b, st, intrin);
      AC_NIR_STORE_IO(b, store_val, 0, write_mask, semantics.high_16bits,
                      nir_store_shared, lds_off, .write_mask = store_write_mask, .base = store_const_offset);
   }

   /* Save tess factor to be used by tess factor writer or reconstruct
    * store output instruction later.
    */
   if (semantics.location == VARYING_SLOT_TESS_LEVEL_INNER ||
       semantics.location == VARYING_SLOT_TESS_LEVEL_OUTER) {
      const unsigned base = nir_intrinsic_base(intrin);
      const unsigned component = nir_intrinsic_component(intrin);

      if (semantics.location == VARYING_SLOT_TESS_LEVEL_INNER) {
         st->tcs_tess_level_inner_base = base;
         st->tcs_tess_level_inner_mask |= write_mask << component;

         if (st->tcs_pass_tessfactors_by_reg)
            ac_nir_store_var_components(b, st->tcs_tess_level_inner, store_val,
                                        component, write_mask);
      } else {
         st->tcs_tess_level_outer_base = base;
         st->tcs_tess_level_outer_mask |= write_mask << component;

         if (st->tcs_pass_tessfactors_by_reg)
            ac_nir_store_var_components(b, st->tcs_tess_level_outer, store_val,
                                        component, write_mask);
      }
   }

   return NIR_LOWER_INSTR_PROGRESS_REPLACE;
}

static nir_def *
lower_hs_output_load(nir_builder *b,
                     nir_intrinsic_instr *intrin,
                     lower_tess_io_state *st)
{
   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   const bool is_tess_factor = io_sem.location == VARYING_SLOT_TESS_LEVEL_INNER ||
                               io_sem.location == VARYING_SLOT_TESS_LEVEL_OUTER;

   if (is_tess_factor && st->tcs_pass_tessfactors_by_reg) {
      const unsigned component = nir_intrinsic_component(intrin);
      const unsigned num_components = intrin->def.num_components;
      const unsigned bit_size = intrin->def.bit_size;

      nir_def *var =
         io_sem.location == VARYING_SLOT_TESS_LEVEL_OUTER
            ? nir_load_var(b, st->tcs_tess_level_outer)
            : nir_load_var(b, st->tcs_tess_level_inner);

      return nir_extract_bits(b, &var, 1, component * bit_size, num_components, bit_size);
   }

   /* If an output is not stored by the shader, replace the output load by undef. */
   if (!tcs_output_needs_lds(intrin, b->shader, st))
      return nir_undef(b, intrin->def.num_components, intrin->def.bit_size);

   nir_def *off = hs_output_lds_offset(b, st, intrin);
   nir_def *load = NULL;

   AC_NIR_LOAD_IO(load, b, intrin->def.num_components, intrin->def.bit_size, io_sem.high_16bits,
                  nir_load_shared, off);

   return load;
}

static void
update_hs_barrier(nir_intrinsic_instr *intrin, lower_tess_io_state *st)
{
   /* Output loads and stores are lowered to shared memory access,
    * so we have to update the barriers to also reflect this.
    */
   unsigned mem_modes = nir_intrinsic_memory_modes(intrin);
   if (mem_modes & nir_var_shader_out) {
      mem_modes |= nir_var_mem_shared;
      mem_modes &= ~nir_var_shader_out;
   }
   nir_intrinsic_set_memory_modes(intrin, mem_modes);

   mesa_scope exec_scope = nir_intrinsic_execution_scope(intrin);
   if (exec_scope == SCOPE_WORKGROUP && st->tcs_out_patch_fits_subgroup)
      nir_intrinsic_set_execution_scope(intrin, SCOPE_SUBGROUP);

   mesa_scope mem_scope = nir_intrinsic_memory_scope(intrin);
   if (mem_scope == SCOPE_WORKGROUP && st->tcs_out_patch_fits_subgroup)
      nir_intrinsic_set_memory_scope(intrin, SCOPE_SUBGROUP);
}

static nir_def *
lower_hs_output_access(nir_builder *b,
                       nir_instr *instr,
                       void *state)
{
   lower_tess_io_state *st = (lower_tess_io_state *) state;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   if (intrin->intrinsic == nir_intrinsic_store_output ||
       intrin->intrinsic == nir_intrinsic_store_per_vertex_output) {
      return lower_hs_output_store(b, intrin, st);
   } else if (intrin->intrinsic == nir_intrinsic_load_output ||
              intrin->intrinsic == nir_intrinsic_load_per_vertex_output) {
      return lower_hs_output_load(b, intrin, st);
   } else if (intrin->intrinsic == nir_intrinsic_barrier) {
      update_hs_barrier(intrin, st);
      return NIR_LOWER_INSTR_PROGRESS;
   } else {
      unreachable("intrinsic not supported by lower_hs_output_access");
   }
}

static tess_levels
hs_load_tess_levels(nir_builder *b,
                    lower_tess_io_state *st)
{
   unsigned outer_comps, inner_comps;
   mesa_count_tess_level_components(b->shader->info.tess._primitive_mode,
                                    &outer_comps, &inner_comps);

   nir_def *outer = NULL;
   nir_def *inner = NULL;

   if (st->tcs_pass_tessfactors_by_reg) {
      if (st->tcs_tess_level_outer_mask) {
         outer = nir_load_var(b, st->tcs_tess_level_outer);
         outer = nir_trim_vector(b, outer, outer_comps);
      }

      if (inner_comps && st->tcs_tess_level_inner_mask) {
         inner = nir_load_var(b, st->tcs_tess_level_inner);
         inner = nir_trim_vector(b, inner, inner_comps);
      }
   } else {
      /* Base LDS address of per-patch outputs in the current patch. */
      nir_def *lds_base = hs_output_lds_offset(b, st, NULL);

      /* Load all tessellation factors (aka. tess levels) from LDS. */
      if (st->tcs_tess_level_outer_mask) {
         const unsigned mapped = hs_output_lds_map_io_location(b->shader, false, VARYING_SLOT_TESS_LEVEL_OUTER, st);
         outer = nir_load_shared(b, outer_comps, 32, lds_base, .base = mapped * 16);
      }

      if (inner_comps && st->tcs_tess_level_inner_mask) {
         const unsigned mapped = hs_output_lds_map_io_location(b->shader, false, VARYING_SLOT_TESS_LEVEL_INNER, st);
         inner = nir_load_shared(b, inner_comps, 32, lds_base, .base = mapped * 16);
      }
   }

   /* Set tess factor to zero if the shader did not write them. */
   if (!outer)
      outer = nir_imm_zero(b, outer_comps, 32);
   if (inner_comps && !inner)
      inner = nir_imm_zero(b, inner_comps, 32);

   tess_levels r = {
      .outer = outer,
      .inner = inner,
   };

   return r;
}

static void
hs_store_dynamic_control_word_gfx6(nir_builder *b)
{
   nir_def *rel_patch_id = nir_load_tess_rel_patch_id_amd(b);
   nir_def *tessfactor_ring = nir_load_ring_tess_factors_amd(b);
   nir_def *tess_factors_base = nir_load_ring_tess_factors_offset_amd(b);

   /* Store the dynamic HS control word. */
   nir_if *rel_patch_id_zero = nir_push_if(b, nir_ieq_imm(b, rel_patch_id, 0));
   nir_def *zero = nir_imm_int(b, 0);
   nir_def *ctrlw = nir_imm_int(b, 0x80000000u);
   nir_store_buffer_amd(b, ctrlw, tessfactor_ring, zero, tess_factors_base, zero,
                        .access = ACCESS_COHERENT);
   nir_pop_if(b, rel_patch_id_zero);
}

static nir_def *
hs_resize_tess_factor(nir_builder *b, nir_def *tf, unsigned comps)
{
   if (!comps)
      return NULL;
   else if (!tf)
      return nir_imm_zero(b, comps, 32);
   else if (comps > tf->num_components)
      return nir_pad_vector_imm_int(b, tf, 0, comps);
   else if (comps < tf->num_components)
      return nir_trim_vector(b, tf, comps);
   else
      return tf;
}

static void
hs_store_tess_factors_for_tessellator(nir_builder *b, enum amd_gfx_level gfx_level,
                                      enum tess_primitive_mode prim_mode,
                                      tess_levels tessfactors)
{
   nir_def *rel_patch_id = nir_load_tess_rel_patch_id_amd(b);
   nir_def *tessfactor_ring = nir_load_ring_tess_factors_amd(b);
   nir_def *tess_factors_base = nir_load_ring_tess_factors_offset_amd(b);
   nir_def *zero = nir_imm_int(b, 0);

   const unsigned tess_factors_const_offset = gfx_level <= GFX8 ? 4 : 0;
   unsigned outer_comps, inner_comps;

   mesa_count_tess_level_components(prim_mode, &outer_comps, &inner_comps);

   nir_def *tess_factors_offset =
      nir_imul_imm(b, rel_patch_id, (inner_comps + outer_comps) * 4u);

   nir_def *tf_outer = hs_resize_tess_factor(b, tessfactors.outer, outer_comps);
   nir_def *tf_inner = hs_resize_tess_factor(b, tessfactors.inner, inner_comps);

   /* Store tess factors for the tessellator */
   if (prim_mode == TESS_PRIMITIVE_ISOLINES) {
      /* LINES reversal */
      nir_def *t = nir_vec2(b, nir_channel(b, tf_outer, 1), nir_channel(b, tf_outer, 0));
      nir_store_buffer_amd(b, t, tessfactor_ring, tess_factors_offset, tess_factors_base, zero,
                           .base = tess_factors_const_offset, .access = ACCESS_COHERENT | ACCESS_CP_GE_COHERENT_AMD);
   } else if (prim_mode == TESS_PRIMITIVE_TRIANGLES) {
      nir_def *t = nir_vec4(b, nir_channel(b, tf_outer, 0), nir_channel(b, tf_outer, 1),
                               nir_channel(b, tf_outer, 2), nir_channel(b, tf_inner, 0));
      nir_store_buffer_amd(b, t, tessfactor_ring, tess_factors_offset, tess_factors_base, zero,
                           .base = tess_factors_const_offset, .access = ACCESS_COHERENT | ACCESS_CP_GE_COHERENT_AMD);
   } else {
      nir_store_buffer_amd(b, tf_outer, tessfactor_ring, tess_factors_offset, tess_factors_base, zero,
                           .base = tess_factors_const_offset, .access = ACCESS_COHERENT | ACCESS_CP_GE_COHERENT_AMD);
      nir_store_buffer_amd(b, tf_inner, tessfactor_ring, tess_factors_offset, tess_factors_base, zero,
                           .base = tess_factors_const_offset + 4u * outer_comps,
                           .access = ACCESS_COHERENT | ACCESS_CP_GE_COHERENT_AMD);
   }
}

static void
hs_store_tess_factors_for_tes(nir_builder *b, tess_levels tessfactors, lower_tess_io_state *st)
{
   nir_def *hs_ring_tess_offchip = nir_load_ring_tess_offchip_amd(b);
   nir_def *offchip_offset = nir_load_ring_tess_offchip_offset_amd(b);
   nir_def *zero = nir_imm_int(b, 0);

   /* For linked shaders, we must only write the tess factors that the TES actually reads,
    * otherwise we would write to a memory location reserved for another per-patch output.
    */
   const bool tes_reads_outer = st->tes_inputs_read & VARYING_BIT_TESS_LEVEL_OUTER;
   const bool tes_reads_inner = st->tes_inputs_read & VARYING_BIT_TESS_LEVEL_INNER;

   if (st->tcs_tess_level_outer_mask && tes_reads_outer) {
      const unsigned tf_outer_loc = hs_output_vram_map_io_location(b->shader, false, VARYING_SLOT_TESS_LEVEL_OUTER, st);
      nir_def *vmem_off_outer = hs_per_patch_output_vmem_offset(b, st, NULL, tf_outer_loc * 16);

      nir_store_buffer_amd(b, tessfactors.outer, hs_ring_tess_offchip,
                           vmem_off_outer, offchip_offset, zero,
                           .memory_modes = nir_var_shader_out,
                           .access = ACCESS_COHERENT);
   }

   if (tessfactors.inner && st->tcs_tess_level_inner_mask && tes_reads_inner) {
      const unsigned tf_inner_loc = hs_output_vram_map_io_location(b->shader, false, VARYING_SLOT_TESS_LEVEL_INNER, st);
      nir_def *vmem_off_inner = hs_per_patch_output_vmem_offset(b, st, NULL, tf_inner_loc * 16);

      nir_store_buffer_amd(b, tessfactors.inner, hs_ring_tess_offchip,
                           vmem_off_inner, offchip_offset, zero,
                           .memory_modes = nir_var_shader_out,
                           .access = ACCESS_COHERENT);
   }
}

static nir_if *
hs_if_invocation_id_zero(nir_builder *b)
{
   nir_def *invocation_id = nir_load_invocation_id(b);

   /* Only the 1st invocation of each patch needs to do this. */
   nir_if *invocation_id_zero = nir_push_if(b, nir_ieq_imm(b, invocation_id, 0));

   /* When the output patch size is <= 32 then we can flatten the branch here
    * because we know for sure that at least 1 invocation in all waves will
    * take the branch.
    */
   if (b->shader->info.tess.tcs_vertices_out <= 32)
      invocation_id_zero->control = nir_selection_control_divergent_always_taken;

   return invocation_id_zero;
}

static void
hs_finale(nir_shader *shader,
          lower_tess_io_state *st)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(shader);
   assert(impl);
   nir_block *last_block = nir_impl_last_block(impl);
   assert(last_block);

   nir_builder builder = nir_builder_at(nir_after_block(last_block));
   nir_builder *b = &builder; /* This is to avoid the & */

   /* If tess factors are load from LDS, wait previous LDS stores done. */
   if (!st->tcs_pass_tessfactors_by_reg) {
      mesa_scope scope = st->tcs_out_patch_fits_subgroup ? SCOPE_SUBGROUP : SCOPE_WORKGROUP;
      nir_barrier(b, .execution_scope = scope, .memory_scope = scope,
                     .memory_semantics = NIR_MEMORY_ACQ_REL, .memory_modes = nir_var_mem_shared);
   }

   /* Only the 1st invocation of each patch needs to access VRAM and/or LDS. */
   nir_if *if_invocation_id_zero = hs_if_invocation_id_zero(b);
   {
      tess_levels tessfactors = hs_load_tess_levels(b, st);

      if (st->gfx_level <= GFX8)
         hs_store_dynamic_control_word_gfx6(b);

      nir_def *prim_mode = nir_load_tcs_primitive_mode_amd(b);
      nir_if *if_triangles = nir_push_if(b, nir_ieq_imm(b, prim_mode, TESS_PRIMITIVE_TRIANGLES));
      {
         hs_store_tess_factors_for_tessellator(b, st->gfx_level, TESS_PRIMITIVE_TRIANGLES, tessfactors);
      }
      nir_push_else(b, if_triangles);
      {
         nir_if *if_isolines = nir_push_if(b, nir_ieq_imm(b, prim_mode, TESS_PRIMITIVE_ISOLINES));
         {
            hs_store_tess_factors_for_tessellator(b, st->gfx_level, TESS_PRIMITIVE_ISOLINES, tessfactors);
         }
         nir_push_else(b, if_isolines);
         {
            hs_store_tess_factors_for_tessellator(b, st->gfx_level, TESS_PRIMITIVE_QUADS, tessfactors);
         }
         nir_pop_if(b, if_isolines);
      }
      nir_pop_if(b, if_triangles);

      nir_if *if_tes_reads_tf = nir_push_if(b, nir_load_tcs_tess_levels_to_tes_amd(b));
      {
         hs_store_tess_factors_for_tes(b, tessfactors, st);
      }
      nir_pop_if(b, if_tes_reads_tf);
   }

   nir_pop_if(b, if_invocation_id_zero);

   nir_metadata_preserve(impl, nir_metadata_none);
}

static nir_def *
lower_tes_input_load(nir_builder *b,
                     nir_instr *instr,
                     void *state)
{
   lower_tess_io_state *st = (lower_tess_io_state *) state;
   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   const nir_io_semantics io_sem = nir_intrinsic_io_semantics(intrin);
   nir_def *offchip_ring = nir_load_ring_tess_offchip_amd(b);
   nir_def *offchip_offset = nir_load_ring_tess_offchip_offset_amd(b);
   nir_def *off = intrin->intrinsic == nir_intrinsic_load_per_vertex_input
                    ? hs_per_vertex_output_vmem_offset(b, st, intrin)
                    : hs_per_patch_output_vmem_offset(b, st, intrin, 0);

   nir_def *zero = nir_imm_int(b, 0);
   nir_def *load = NULL;

   AC_NIR_LOAD_IO(load, b, intrin->def.num_components, intrin->def.bit_size, io_sem.high_16bits,
                  nir_load_buffer_amd, offchip_ring, off, offchip_offset, zero, .access = ACCESS_COHERENT);

   return load;
}

static bool
filter_hs_output_access(const nir_instr *instr,
                         UNUSED const void *st)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_store_output ||
          intrin->intrinsic == nir_intrinsic_store_per_vertex_output ||
          intrin->intrinsic == nir_intrinsic_load_output ||
          intrin->intrinsic == nir_intrinsic_load_per_vertex_output ||
          intrin->intrinsic == nir_intrinsic_barrier;
}

static bool
filter_any_input_access(const nir_instr *instr,
                        UNUSED const void *st)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   return intrin->intrinsic == nir_intrinsic_load_input ||
          intrin->intrinsic == nir_intrinsic_load_per_vertex_input;
}

void
ac_nir_lower_ls_outputs_to_mem(nir_shader *shader,
                               ac_nir_map_io_driver_location map,
                               bool tcs_in_out_eq,
                               uint64_t tcs_inputs_read,
                               uint64_t tcs_temp_only_inputs)
{
   assert(shader->info.stage == MESA_SHADER_VERTEX);

   lower_tess_io_state state = {
      .tcs_in_out_eq = tcs_in_out_eq,
      .tcs_inputs_read = tcs_inputs_read,
      .tcs_temp_only_inputs = tcs_in_out_eq ? tcs_temp_only_inputs : 0,
      .map_io = map,
   };

   nir_shader_intrinsics_pass(shader, lower_ls_output_store,
                                nir_metadata_control_flow,
                                &state);
}

void
ac_nir_lower_hs_inputs_to_mem(nir_shader *shader,
                              ac_nir_map_io_driver_location map,
                              bool tcs_in_out_eq,
                              uint64_t tcs_temp_only_inputs)
{
   assert(shader->info.stage == MESA_SHADER_TESS_CTRL);

   lower_tess_io_state state = {
      .tcs_inputs_read = shader->info.inputs_read,
      .tcs_in_out_eq = tcs_in_out_eq,
      .tcs_temp_only_inputs = tcs_in_out_eq ? tcs_temp_only_inputs : 0,
      .map_io = map,
   };

   nir_shader_lower_instructions(shader,
                                 filter_load_tcs_per_vertex_input,
                                 lower_hs_per_vertex_input_load,
                                 &state);
}

void
ac_nir_lower_hs_outputs_to_mem(nir_shader *shader,
                               ac_nir_map_io_driver_location map,
                               enum amd_gfx_level gfx_level,
                               uint64_t tes_inputs_read,
                               uint32_t tes_patch_inputs_read,
                               unsigned wave_size,
                               bool pass_tessfactors_by_reg)
{
   assert(shader->info.stage == MESA_SHADER_TESS_CTRL);

   lower_tess_io_state state = {
      .gfx_level = gfx_level,
      .tes_inputs_read = tes_inputs_read,
      .tes_patch_inputs_read = tes_patch_inputs_read,
      .tcs_out_patch_fits_subgroup = wave_size % shader->info.tess.tcs_vertices_out == 0,
      .tcs_pass_tessfactors_by_reg = pass_tessfactors_by_reg,
      .map_io = map,
   };

   if (pass_tessfactors_by_reg) {
      nir_function_impl *impl = nir_shader_get_entrypoint(shader);
      state.tcs_tess_level_outer =
         nir_local_variable_create(impl, glsl_vec4_type(), "tess outer");
      state.tcs_tess_level_inner =
         nir_local_variable_create(impl, glsl_vec4_type(), "tess inner");
   }

   nir_shader_lower_instructions(shader,
                                 filter_hs_output_access,
                                 lower_hs_output_access,
                                 &state);

   hs_finale(shader, &state);
}

void
ac_nir_lower_tes_inputs_to_mem(nir_shader *shader,
                               ac_nir_map_io_driver_location map)
{
   assert(shader->info.stage == MESA_SHADER_TESS_EVAL);

   lower_tess_io_state state = {
      .map_io = map,
      .tes_inputs_read = shader->info.inputs_read,
      .tes_patch_inputs_read = shader->info.patch_inputs_read,
   };

   nir_shader_lower_instructions(shader,
                                 filter_any_input_access,
                                 lower_tes_input_load,
                                 &state);
}
