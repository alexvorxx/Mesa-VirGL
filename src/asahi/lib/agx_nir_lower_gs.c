/*
 * Copyright 2023 Alyssa Rosenzweig
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "agx_nir_lower_gs.h"
#include "asahi/compiler/agx_compile.h"
#include "compiler/nir/nir_builder.h"
#include "gallium/include/pipe/p_defines.h"
#include "shaders/draws.h"
#include "shaders/geometry.h"
#include "util/bitscan.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "util/u_math.h"
#include "libagx_shaders.h"
#include "nir.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "nir_xfb_info.h"
#include "shader_enums.h"

/* Marks a transform feedback store, which must not be stripped from the
 * prepass since that's where the transform feedback happens. Chosen as a
 * vendored flag not to alias other flags we'll see.
 */
#define ACCESS_XFB (ACCESS_IS_SWIZZLED_AMD)

enum gs_counter {
   GS_COUNTER_VERTICES = 0,
   GS_COUNTER_PRIMITIVES,
   GS_COUNTER_XFB_PRIMITIVES,
   GS_NUM_COUNTERS
};

#define MAX_PRIM_OUT_SIZE 3

struct lower_gs_state {
   int static_count[GS_NUM_COUNTERS][MAX_VERTEX_STREAMS];
   nir_variable *outputs[NUM_TOTAL_VARYING_SLOTS][MAX_PRIM_OUT_SIZE];

   /* The count buffer contains `count_stride_el` 32-bit words in a row for each
    * input primitive, for `input_primitives * count_stride_el * 4` total bytes.
    */
   unsigned count_stride_el;

   /* The index of each counter in the count buffer, or -1 if it's not in the
    * count buffer.
    *
    * Invariant: count_stride_el == sum(count_index[i][j] >= 0).
    */
   int count_index[MAX_VERTEX_STREAMS][GS_NUM_COUNTERS];

   bool rasterizer_discard;
};

/* Helpers for loading from the geometry state buffer */
static nir_def *
load_geometry_param_offset(nir_builder *b, uint32_t offset, uint8_t bytes)
{
   nir_def *base = nir_load_geometry_param_buffer_agx(b);
   nir_def *addr = nir_iadd_imm(b, base, offset);

   assert((offset % bytes) == 0 && "must be naturally aligned");

   return nir_load_global_constant(b, addr, bytes, 1, bytes * 8);
}

static void
store_geometry_param_offset(nir_builder *b, nir_def *def, uint32_t offset,
                            uint8_t bytes)
{
   nir_def *base = nir_load_geometry_param_buffer_agx(b);
   nir_def *addr = nir_iadd_imm(b, base, offset);

   assert((offset % bytes) == 0 && "must be naturally aligned");

   nir_store_global(b, addr, 4, def, nir_component_mask(def->num_components));
}

#define store_geometry_param(b, field, def)                                    \
   store_geometry_param_offset(                                                \
      b, def, offsetof(struct agx_geometry_params, field),                     \
      sizeof(((struct agx_geometry_params *)0)->field))

#define load_geometry_param(b, field)                                          \
   load_geometry_param_offset(                                                 \
      b, offsetof(struct agx_geometry_params, field),                          \
      sizeof(((struct agx_geometry_params *)0)->field))

/* Helper for updating counters */
static void
add_counter(nir_builder *b, nir_def *counter, nir_def *increment)
{
   /* If the counter is NULL, the counter is disabled. Skip the update. */
   nir_if *nif = nir_push_if(b, nir_ine_imm(b, counter, 0));
   {
      nir_def *old = nir_load_global(b, counter, 4, 1, 32);
      nir_def *new_ = nir_iadd(b, old, increment);
      nir_store_global(b, counter, 4, new_, nir_component_mask(1));
   }
   nir_pop_if(b, nif);
}

/* Helpers for lowering I/O to variables */
static void
lower_store_to_var(nir_builder *b, nir_intrinsic_instr *intr,
                   struct agx_lower_output_to_var_state *state)
{
   b->cursor = nir_instr_remove(&intr->instr);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   unsigned component = nir_intrinsic_component(intr);
   nir_def *value = intr->src[0].ssa;

   assert(nir_src_is_const(intr->src[1]) && "no indirect outputs");
   assert(nir_intrinsic_write_mask(intr) == nir_component_mask(1) &&
          "should be scalarized");

   nir_variable *var =
      state->outputs[sem.location + nir_src_as_uint(intr->src[1])];
   if (!var) {
      assert(sem.location == VARYING_SLOT_PSIZ &&
             "otherwise in outputs_written");
      return;
   }

   unsigned nr_components = glsl_get_components(glsl_without_array(var->type));
   assert(component < nr_components);

   /* Turn it into a vec4 write like NIR expects */
   value = nir_vector_insert_imm(b, nir_undef(b, nr_components, 32), value,
                                 component);

   nir_store_var(b, var, value, BITFIELD_BIT(component));
}

bool
agx_lower_output_to_var(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   lower_store_to_var(b, intr, data);
   return true;
}

/*
 * Geometry shader invocations are compute-like:
 *
 * (primitive ID, instance ID, 1)
 */
static nir_def *
load_primitive_id(nir_builder *b)
{
   return nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
}

static nir_def *
load_instance_id(nir_builder *b)
{
   return nir_channel(b, nir_load_global_invocation_id(b, 32), 1);
}

/* Geometry shaders use software input assembly. The software vertex shader
 * is invoked for each index, and the geometry shader applies the topology. This
 * helper applies the topology.
 */
static nir_def *
vertex_id_for_topology_class(nir_builder *b, nir_def *vert, enum mesa_prim cls)
{
   nir_def *prim = nir_load_primitive_id(b);
   nir_def *flatshade_first = nir_ieq_imm(b, nir_load_provoking_last(b), 0);
   nir_def *nr = load_geometry_param(b, gs_grid[0]);
   nir_def *topology = nir_load_input_topology_agx(b);

   switch (cls) {
   case MESA_PRIM_POINTS:
      return prim;

   case MESA_PRIM_LINES:
      return libagx_vertex_id_for_line_class(b, topology, prim, vert, nr);

   case MESA_PRIM_TRIANGLES:
      return libagx_vertex_id_for_tri_class(b, topology, prim, vert,
                                            flatshade_first);

   case MESA_PRIM_LINES_ADJACENCY:
      return libagx_vertex_id_for_line_adj_class(b, topology, prim, vert);

   case MESA_PRIM_TRIANGLES_ADJACENCY:
      return libagx_vertex_id_for_tri_adj_class(b, topology, prim, vert, nr,
                                                flatshade_first);

   default:
      unreachable("invalid topology class");
   }
}

nir_def *
agx_load_per_vertex_input(nir_builder *b, nir_intrinsic_instr *intr,
                          nir_def *vertex)
{
   assert(intr->intrinsic == nir_intrinsic_load_per_vertex_input);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);

   nir_def *location = nir_iadd_imm(b, intr->src[1].ssa, sem.location);
   nir_def *addr;

   if (b->shader->info.stage == MESA_SHADER_GEOMETRY) {
      /* GS may be preceded by VS or TES so specified as param */
      addr = libagx_geometry_input_address(
         b, nir_load_geometry_param_buffer_agx(b), vertex, location);
   } else {
      assert(b->shader->info.stage == MESA_SHADER_TESS_CTRL);

      /* TCS always preceded by VS so we use the VS state directly */
      addr = libagx_vertex_output_address(b, nir_load_vs_output_buffer_agx(b),
                                          nir_load_vs_outputs_agx(b), vertex,
                                          location);
   }

   addr = nir_iadd_imm(b, addr, 4 * nir_intrinsic_component(intr));
   return nir_load_global_constant(b, addr, 4, intr->def.num_components,
                                   intr->def.bit_size);
}

static bool
lower_gs_inputs(nir_builder *b, nir_intrinsic_instr *intr, void *_)
{
   if (intr->intrinsic != nir_intrinsic_load_per_vertex_input)
      return false;

   b->cursor = nir_instr_remove(&intr->instr);

   /* Calculate the vertex ID we're pulling, based on the topology class */
   nir_def *vert_in_prim = intr->src[0].ssa;
   nir_def *vertex = vertex_id_for_topology_class(
      b, vert_in_prim, b->shader->info.gs.input_primitive);

   nir_def *verts = load_geometry_param(b, vs_grid[0]);
   nir_def *unrolled =
      nir_iadd(b, nir_imul(b, nir_load_instance_id(b), verts), vertex);

   nir_def *val = agx_load_per_vertex_input(b, intr, unrolled);
   nir_def_rewrite_uses(&intr->def, val);
   return true;
}

/*
 * Unrolled ID is the index of the primitive in the count buffer, given as
 * (instance ID * # vertices/instance) + vertex ID
 */
static nir_def *
calc_unrolled_id(nir_builder *b)
{
   return nir_iadd(
      b, nir_imul(b, load_instance_id(b), load_geometry_param(b, gs_grid[0])),
      load_primitive_id(b));
}

static unsigned
output_vertex_id_stride(nir_shader *gs)
{
   /* round up to power of two for cheap multiply/division */
   return util_next_power_of_two(MAX2(gs->info.gs.vertices_out, 1));
}

/* Variant of calc_unrolled_id that uses a power-of-two stride for indices. This
 * is sparser (acceptable for index buffer values, not for count buffer
 * indices). It has the nice property of being cheap to invert, unlike
 * calc_unrolled_id. So, we use calc_unrolled_id for count buffers and
 * calc_unrolled_index_id for index values.
 *
 * This also multiplies by the appropriate stride to calculate the final index
 * base value.
 */
static nir_def *
calc_unrolled_index_id(nir_builder *b)
{
   unsigned vertex_stride = output_vertex_id_stride(b->shader);
   nir_def *primitives_log2 = load_geometry_param(b, primitives_log2);

   nir_def *instance = nir_ishl(b, load_instance_id(b), primitives_log2);
   nir_def *prim = nir_iadd(b, instance, load_primitive_id(b));

   return nir_imul_imm(b, prim, vertex_stride);
}

static nir_def *
load_count_address(nir_builder *b, struct lower_gs_state *state,
                   nir_def *unrolled_id, unsigned stream,
                   enum gs_counter counter)
{
   int index = state->count_index[stream][counter];
   if (index < 0)
      return NULL;

   nir_def *prim_offset_el =
      nir_imul_imm(b, unrolled_id, state->count_stride_el);

   nir_def *offset_el = nir_iadd_imm(b, prim_offset_el, index);

   return nir_iadd(b, load_geometry_param(b, count_buffer),
                   nir_u2u64(b, nir_imul_imm(b, offset_el, 4)));
}

static void
write_counts(nir_builder *b, nir_intrinsic_instr *intr,
             struct lower_gs_state *state)
{
   /* Store each required counter */
   nir_def *counts[GS_NUM_COUNTERS] = {
      [GS_COUNTER_VERTICES] = intr->src[0].ssa,
      [GS_COUNTER_PRIMITIVES] = intr->src[1].ssa,
      [GS_COUNTER_XFB_PRIMITIVES] = intr->src[2].ssa,
   };

   for (unsigned i = 0; i < GS_NUM_COUNTERS; ++i) {
      nir_def *addr = load_count_address(b, state, calc_unrolled_id(b),
                                         nir_intrinsic_stream_id(intr), i);

      if (addr)
         nir_store_global(b, addr, 4, counts[i], nir_component_mask(1));
   }
}

static bool
lower_gs_count_instr(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_emit_vertex_with_counter:
   case nir_intrinsic_end_primitive_with_counter:
   case nir_intrinsic_store_output:
      /* These are for the main shader, just remove them */
      nir_instr_remove(&intr->instr);
      return true;

   case nir_intrinsic_set_vertex_and_primitive_count:
      b->cursor = nir_instr_remove(&intr->instr);
      write_counts(b, intr, data);
      return true;

   default:
      return false;
   }
}

static bool
lower_id(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   b->cursor = nir_before_instr(&intr->instr);

   nir_def *id;
   if (intr->intrinsic == nir_intrinsic_load_primitive_id)
      id = load_primitive_id(b);
   else if (intr->intrinsic == nir_intrinsic_load_instance_id)
      id = load_instance_id(b);
   else if (intr->intrinsic == nir_intrinsic_load_flat_mask)
      id = load_geometry_param(b, flat_outputs);
   else if (intr->intrinsic == nir_intrinsic_load_input_topology_agx)
      id = load_geometry_param(b, input_topology);
   else
      return false;

   b->cursor = nir_instr_remove(&intr->instr);
   nir_def_rewrite_uses(&intr->def, id);
   return true;
}

/*
 * Create a "Geometry count" shader. This is a stripped down geometry shader
 * that just write its number of emitted vertices / primitives / transform
 * feedback primitives to a count buffer. That count buffer will be prefix
 * summed prior to running the real geometry shader. This is skipped if the
 * counts are statically known.
 */
static nir_shader *
agx_nir_create_geometry_count_shader(nir_shader *gs, const nir_shader *libagx,
                                     struct lower_gs_state *state)
{
   /* Don't muck up the original shader */
   nir_shader *shader = nir_shader_clone(NULL, gs);

   if (shader->info.name) {
      shader->info.name =
         ralloc_asprintf(shader, "%s_count", shader->info.name);
   } else {
      shader->info.name = "count";
   }

   NIR_PASS(_, shader, nir_shader_intrinsics_pass, lower_gs_count_instr,
            nir_metadata_control_flow, state);

   NIR_PASS(_, shader, nir_shader_intrinsics_pass, lower_id,
            nir_metadata_control_flow, NULL);

   agx_preprocess_nir(shader, libagx);
   return shader;
}

struct lower_gs_rast_state {
   nir_def *instance_id, *primitive_id, *output_id;
   struct agx_lower_output_to_var_state outputs;
   struct agx_lower_output_to_var_state selected;
};

static void
select_rast_output(nir_builder *b, nir_intrinsic_instr *intr,
                   struct lower_gs_rast_state *state)
{
   b->cursor = nir_instr_remove(&intr->instr);

   /* We only care about the rasterization stream in the rasterization
    * shader, so just ignore emits from other streams.
    */
   if (nir_intrinsic_stream_id(intr) != 0)
      return;

   u_foreach_bit64(slot, b->shader->info.outputs_written) {
      nir_def *orig = nir_load_var(b, state->selected.outputs[slot]);
      nir_def *data = nir_load_var(b, state->outputs.outputs[slot]);

      nir_def *value = nir_bcsel(
         b, nir_ieq(b, intr->src[0].ssa, state->output_id), data, orig);

      nir_store_var(b, state->selected.outputs[slot], value,
                    nir_component_mask(value->num_components));
   }
}

static bool
lower_to_gs_rast(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct lower_gs_rast_state *state = data;

   switch (intr->intrinsic) {
   case nir_intrinsic_store_output:
      lower_store_to_var(b, intr, &state->outputs);
      return true;

   case nir_intrinsic_emit_vertex_with_counter:
      select_rast_output(b, intr, state);
      return true;

   case nir_intrinsic_load_primitive_id:
      nir_def_rewrite_uses(&intr->def, state->primitive_id);
      return true;

   case nir_intrinsic_load_instance_id:
      nir_def_rewrite_uses(&intr->def, state->instance_id);
      return true;

   case nir_intrinsic_load_flat_mask:
   case nir_intrinsic_load_provoking_last:
   case nir_intrinsic_load_input_topology_agx: {
      /* Lowering the same in both GS variants */
      return lower_id(b, intr, NULL);
   }

   case nir_intrinsic_end_primitive_with_counter:
   case nir_intrinsic_set_vertex_and_primitive_count:
      nir_instr_remove(&intr->instr);
      return true;

   default:
      return false;
   }
}

/*
 * Side effects in geometry shaders are problematic with our "GS rasterization
 * shader" implementation. Where does the side effect happen? In the prepass?
 * In the rast shader? In both?
 *
 * A perfect solution is impossible with rast shaders. Since the spec is loose
 * here, we follow the principle of "least surprise":
 *
 * 1. Prefer side effects in the prepass over the rast shader. The prepass runs
 *    once per API GS invocation so will match the expectations of buggy apps
 *    not written for tilers.
 *
 * 2. If we must execute any side effect in the rast shader, try to execute all
 *    side effects only in the rast shader. If some side effects must happen in
 *    the rast shader and others don't, this gets consistent counts
 *    (i.e. if the app expects plain stores and atomics to match up).
 *
 * 3. If we must execute side effects in both rast and the prepass,
 *    execute all side effects in the rast shader and strip what we can from
 *    the prepass. This gets the "unsurprising" behaviour from #2 without
 *    falling over for ridiculous uses of atomics.
 */
static bool
strip_side_effect_from_rast(nir_builder *b, nir_intrinsic_instr *intr,
                            void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_store_global:
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
      break;
   default:
      return false;
   }

   /* If there's a side effect that's actually required, keep it. */
   if (nir_intrinsic_infos[intr->intrinsic].has_dest &&
       !list_is_empty(&intr->def.uses)) {

      bool *any = data;
      *any = true;
      return false;
   }

   /* Otherwise, remove the dead instruction. */
   nir_instr_remove(&intr->instr);
   return true;
}

static bool
strip_side_effects_from_rast(nir_shader *s, bool *side_effects_for_rast)
{
   bool progress, any;

   /* Rather than complex analysis, clone and try to remove as many side effects
    * as possible. Then we check if we removed them all. We need to loop to
    * handle complex control flow with side effects, where we can strip
    * everything but can't figure that out with a simple one-shot analysis.
    */
   nir_shader *clone = nir_shader_clone(NULL, s);

   /* Drop as much as we can */
   do {
      progress = false;
      any = false;
      NIR_PASS(progress, clone, nir_shader_intrinsics_pass,
               strip_side_effect_from_rast, nir_metadata_control_flow, &any);

      NIR_PASS(progress, clone, nir_opt_dce);
      NIR_PASS(progress, clone, nir_opt_dead_cf);
   } while (progress);

   ralloc_free(clone);

   /* If we need atomics, leave them in */
   if (any) {
      *side_effects_for_rast = true;
      return false;
   }

   /* Else strip it all */
   do {
      progress = false;
      any = false;
      NIR_PASS(progress, s, nir_shader_intrinsics_pass,
               strip_side_effect_from_rast, nir_metadata_control_flow, &any);

      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
   } while (progress);

   assert(!any);
   return progress;
}

static bool
strip_side_effect_from_main(nir_builder *b, nir_intrinsic_instr *intr,
                            void *data)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_global_atomic:
   case nir_intrinsic_global_atomic_swap:
      break;
   default:
      return false;
   }

   if (list_is_empty(&intr->def.uses)) {
      nir_instr_remove(&intr->instr);
      return true;
   }

   return false;
}

/*
 * Create a GS rasterization shader. This is a hardware vertex shader that
 * shades each rasterized output vertex in parallel.
 */
static nir_shader *
agx_nir_create_gs_rast_shader(const nir_shader *gs, const nir_shader *libagx,
                              bool *side_effects_for_rast)
{
   /* Don't muck up the original shader */
   nir_shader *shader = nir_shader_clone(NULL, gs);

   unsigned max_verts = output_vertex_id_stride(shader);

   /* Turn into a vertex shader run only for rasterization. Transform feedback
    * was handled in the prepass.
    */
   shader->info.stage = MESA_SHADER_VERTEX;
   shader->info.has_transform_feedback_varyings = false;
   memset(&shader->info.vs, 0, sizeof(shader->info.vs));
   shader->xfb_info = NULL;

   if (shader->info.name) {
      shader->info.name = ralloc_asprintf(shader, "%s_rast", shader->info.name);
   } else {
      shader->info.name = "gs rast";
   }

   nir_builder b_ =
      nir_builder_at(nir_before_impl(nir_shader_get_entrypoint(shader)));
   nir_builder *b = &b_;

   NIR_PASS(_, shader, strip_side_effects_from_rast, side_effects_for_rast);

   /* Optimize out pointless gl_PointSize outputs. Bizarrely, these occur. */
   if (shader->info.gs.output_primitive != MESA_PRIM_POINTS)
      shader->info.outputs_written &= ~VARYING_BIT_PSIZ;

   /* See calc_unrolled_index_id */
   nir_def *raw_id = nir_load_vertex_id(b);
   nir_def *output_id = nir_umod_imm(b, raw_id, max_verts);
   nir_def *unrolled = nir_udiv_imm(b, raw_id, max_verts);

   nir_def *primitives_log2 = load_geometry_param(b, primitives_log2);
   nir_def *instance_id = nir_ushr(b, unrolled, primitives_log2);
   nir_def *primitive_id = nir_iand(
      b, unrolled,
      nir_iadd_imm(b, nir_ishl(b, nir_imm_int(b, 1), primitives_log2), -1));

   struct lower_gs_rast_state rast_state = {
      .instance_id = instance_id,
      .primitive_id = primitive_id,
      .output_id = output_id,
   };

   u_foreach_bit64(slot, shader->info.outputs_written) {
      const char *slot_name =
         gl_varying_slot_name_for_stage(slot, MESA_SHADER_GEOMETRY);

      bool scalar = (slot == VARYING_SLOT_PSIZ) ||
                    (slot == VARYING_SLOT_LAYER) ||
                    (slot == VARYING_SLOT_VIEWPORT);
      unsigned comps = scalar ? 1 : 4;

      rast_state.outputs.outputs[slot] = nir_variable_create(
         shader, nir_var_shader_temp, glsl_vector_type(GLSL_TYPE_UINT, comps),
         ralloc_asprintf(shader, "%s-temp", slot_name));

      rast_state.selected.outputs[slot] = nir_variable_create(
         shader, nir_var_shader_temp, glsl_vector_type(GLSL_TYPE_UINT, comps),
         ralloc_asprintf(shader, "%s-selected", slot_name));
   }

   nir_shader_intrinsics_pass(shader, lower_to_gs_rast,
                              nir_metadata_control_flow, &rast_state);

   b->cursor = nir_after_impl(b->impl);

   /* Forward each selected output to the rasterizer */
   u_foreach_bit64(slot, shader->info.outputs_written) {
      assert(rast_state.selected.outputs[slot] != NULL);
      nir_def *value = nir_load_var(b, rast_state.selected.outputs[slot]);

      /* We set NIR_COMPACT_ARRAYS so clip/cull distance needs to come all in
       * DIST0. Undo the offset if we need to.
       */
      assert(slot != VARYING_SLOT_CULL_DIST1);
      unsigned offset = 0;
      if (slot == VARYING_SLOT_CLIP_DIST1)
         offset = 1;

      nir_store_output(b, value, nir_imm_int(b, offset),
                       .io_semantics.location = slot - offset,
                       .io_semantics.num_slots = 1,
                       .write_mask = nir_component_mask(value->num_components),
                       .src_type = nir_type_uint32);
   }

   /* It is legal to omit the point size write from the geometry shader when
    * drawing points. In this case, the point size is implicitly 1.0. To
    * implement, insert a synthetic `gl_PointSize = 1.0` write into the GS copy
    * shader, if the GS does not export a point size while drawing points.
    */
   bool is_points = gs->info.gs.output_primitive == MESA_PRIM_POINTS;

   if (!(shader->info.outputs_written & VARYING_BIT_PSIZ) && is_points) {
      nir_store_output(b, nir_imm_float(b, 1.0), nir_imm_int(b, 0),
                       .io_semantics.location = VARYING_SLOT_PSIZ,
                       .io_semantics.num_slots = 1,
                       .write_mask = nir_component_mask(1),
                       .src_type = nir_type_float32);

      shader->info.outputs_written |= VARYING_BIT_PSIZ;
   }

   nir_opt_idiv_const(shader, 16);

   agx_preprocess_nir(shader, libagx);
   return shader;
}

static nir_def *
previous_count(nir_builder *b, struct lower_gs_state *state, unsigned stream,
               nir_def *unrolled_id, enum gs_counter counter)
{
   assert(stream < MAX_VERTEX_STREAMS);
   assert(counter < GS_NUM_COUNTERS);
   int static_count = state->static_count[counter][stream];

   if (static_count >= 0) {
      /* If the number of outputted vertices per invocation is known statically,
       * we can calculate the base.
       */
      return nir_imul_imm(b, unrolled_id, static_count);
   } else {
      /* Otherwise, we need to load from the prefix sum buffer. Note that the
       * sums are inclusive, so index 0 is nonzero. This requires a little
       * fixup here. We use a saturating unsigned subtraction so we don't read
       * out-of-bounds for zero.
       *
       * TODO: Optimize this.
       */
      nir_def *prim_minus_1 = nir_usub_sat(b, unrolled_id, nir_imm_int(b, 1));
      nir_def *addr =
         load_count_address(b, state, prim_minus_1, stream, counter);

      return nir_bcsel(b, nir_ieq_imm(b, unrolled_id, 0), nir_imm_int(b, 0),
                       nir_load_global_constant(b, addr, 4, 1, 32));
   }
}

static nir_def *
previous_vertices(nir_builder *b, struct lower_gs_state *state, unsigned stream,
                  nir_def *unrolled_id)
{
   return previous_count(b, state, stream, unrolled_id, GS_COUNTER_VERTICES);
}

static nir_def *
previous_primitives(nir_builder *b, struct lower_gs_state *state,
                    unsigned stream, nir_def *unrolled_id)
{
   return previous_count(b, state, stream, unrolled_id, GS_COUNTER_PRIMITIVES);
}

static nir_def *
previous_xfb_primitives(nir_builder *b, struct lower_gs_state *state,
                        unsigned stream, nir_def *unrolled_id)
{
   return previous_count(b, state, stream, unrolled_id,
                         GS_COUNTER_XFB_PRIMITIVES);
}

static void
lower_end_primitive(nir_builder *b, nir_intrinsic_instr *intr,
                    struct lower_gs_state *state)
{
   assert((intr->intrinsic == nir_intrinsic_set_vertex_and_primitive_count ||
           b->shader->info.gs.output_primitive != MESA_PRIM_POINTS) &&
          "endprimitive for points should've been removed");

   /* The GS is the last stage before rasterization, so if we discard the
    * rasterization, we don't output an index buffer, nothing will read it.
    * Index buffer is only for the rasterization stream.
    */
   unsigned stream = nir_intrinsic_stream_id(intr);
   if (state->rasterizer_discard || stream != 0)
      return;

   libagx_end_primitive(
      b, load_geometry_param(b, output_index_buffer), intr->src[0].ssa,
      intr->src[1].ssa, intr->src[2].ssa,
      previous_vertices(b, state, 0, calc_unrolled_id(b)),
      previous_primitives(b, state, 0, calc_unrolled_id(b)),
      calc_unrolled_index_id(b),
      nir_imm_bool(b, b->shader->info.gs.output_primitive != MESA_PRIM_POINTS));
}

static unsigned
verts_in_output_prim(nir_shader *gs)
{
   return mesa_vertices_per_prim(gs->info.gs.output_primitive);
}

static void
write_xfb(nir_builder *b, struct lower_gs_state *state, unsigned stream,
          nir_def *index_in_strip, nir_def *prim_id_in_invocation)
{
   struct nir_xfb_info *xfb = b->shader->xfb_info;
   unsigned verts = verts_in_output_prim(b->shader);

   /* Get the index of this primitive in the XFB buffer. That is, the base for
    * this invocation for the stream plus the offset within this invocation.
    */
   nir_def *invocation_base =
      previous_xfb_primitives(b, state, stream, calc_unrolled_id(b));

   nir_def *prim_index = nir_iadd(b, invocation_base, prim_id_in_invocation);
   nir_def *base_index = nir_imul_imm(b, prim_index, verts);

   nir_def *xfb_prims = load_geometry_param(b, xfb_prims[stream]);
   nir_push_if(b, nir_ult(b, prim_index, xfb_prims));

   /* Write XFB for each output */
   for (unsigned i = 0; i < xfb->output_count; ++i) {
      nir_xfb_output_info output = xfb->outputs[i];

      /* Only write to the selected stream */
      if (xfb->buffer_to_stream[output.buffer] != stream)
         continue;

      unsigned buffer = output.buffer;
      unsigned stride = xfb->buffers[buffer].stride;
      unsigned count = util_bitcount(output.component_mask);

      for (unsigned vert = 0; vert < verts; ++vert) {
         /* We write out the vertices backwards, since 0 is the current
          * emitted vertex (which is actually the last vertex).
          *
          * We handle NULL var for
          * KHR-Single-GL44.enhanced_layouts.xfb_capture_struct.
          */
         unsigned v = (verts - 1) - vert;
         nir_variable *var = state->outputs[output.location][v];
         nir_def *value = var ? nir_load_var(b, var) : nir_undef(b, 4, 32);

         /* In case output.component_mask contains invalid components, write
          * out zeroes instead of blowing up validation.
          *
          * KHR-Single-GL44.enhanced_layouts.xfb_capture_inactive_output_component
          * hits this.
          */
         value = nir_pad_vector_imm_int(b, value, 0, 4);

         nir_def *rotated_vert = nir_imm_int(b, vert);
         if (verts == 3) {
            /* Map vertices for output so we get consistent winding order. For
             * the primitive index, we use the index_in_strip. This is actually
             * the vertex index in the strip, hence
             * offset by 2 relative to the true primitive index (#2 for the
             * first triangle in the strip, #3 for the second). That's ok
             * because only the parity matters.
             */
            rotated_vert = libagx_map_vertex_in_tri_strip(
               b, index_in_strip, rotated_vert,
               nir_inot(b, nir_i2b(b, nir_load_provoking_last(b))));
         }

         nir_def *addr = libagx_xfb_vertex_address(
            b, nir_load_geometry_param_buffer_agx(b), base_index, rotated_vert,
            nir_imm_int(b, buffer), nir_imm_int(b, stride),
            nir_imm_int(b, output.offset));

         nir_build_store_global(
            b, nir_channels(b, value, output.component_mask), addr,
            .align_mul = 4, .write_mask = nir_component_mask(count),
            .access = ACCESS_XFB);
      }
   }

   nir_pop_if(b, NULL);
}

/* Handle transform feedback for a given emit_vertex_with_counter */
static void
lower_emit_vertex_xfb(nir_builder *b, nir_intrinsic_instr *intr,
                      struct lower_gs_state *state)
{
   /* Transform feedback is written for each decomposed output primitive. Since
    * we're writing strips, that means we output XFB for each vertex after the
    * first complete primitive is formed.
    */
   unsigned first_prim = verts_in_output_prim(b->shader) - 1;
   nir_def *index_in_strip = intr->src[1].ssa;

   nir_push_if(b, nir_uge_imm(b, index_in_strip, first_prim));
   {
      write_xfb(b, state, nir_intrinsic_stream_id(intr), index_in_strip,
                intr->src[3].ssa);
   }
   nir_pop_if(b, NULL);

   /* Transform feedback writes out entire primitives during the emit_vertex. To
    * do that, we store the values at all vertices in the strip in a little ring
    * buffer. Index #0 is always the most recent primitive (so non-XFB code can
    * just grab index #0 without any checking). Index #1 is the previous vertex,
    * and index #2 is the vertex before that. Now that we've written XFB, since
    * we've emitted a vertex we need to cycle the ringbuffer, freeing up index
    * #0 for the next vertex that we are about to emit. We do that by copying
    * the first n - 1 vertices forward one slot, which has to happen with a
    * backwards copy implemented here.
    *
    * If we're lucky, all of these copies will be propagated away. If we're
    * unlucky, this involves at most 2 copies per component per XFB output per
    * vertex.
    */
   u_foreach_bit64(slot, b->shader->info.outputs_written) {
      /* Note: if we're outputting points, verts_in_output_prim will be 1, so
       * this loop will not execute. This is intended: points are self-contained
       * primitives and do not need these copies.
       */
      for (int v = verts_in_output_prim(b->shader) - 1; v >= 1; --v) {
         nir_def *value = nir_load_var(b, state->outputs[slot][v - 1]);

         nir_store_var(b, state->outputs[slot][v], value,
                       nir_component_mask(value->num_components));
      }
   }
}

static bool
lower_gs_instr(nir_builder *b, nir_intrinsic_instr *intr, void *state)
{
   b->cursor = nir_before_instr(&intr->instr);

   switch (intr->intrinsic) {
   case nir_intrinsic_set_vertex_and_primitive_count:
      /* This instruction is mostly for the count shader, so just remove. But
       * for points, we write the index buffer here so the rast shader can map.
       */
      if (b->shader->info.gs.output_primitive == MESA_PRIM_POINTS) {
         lower_end_primitive(b, intr, state);
      }

      break;

   case nir_intrinsic_end_primitive_with_counter: {
      unsigned min = verts_in_output_prim(b->shader);

      /* We only write out complete primitives */
      nir_push_if(b, nir_uge_imm(b, intr->src[1].ssa, min));
      {
         lower_end_primitive(b, intr, state);
      }
      nir_pop_if(b, NULL);
      break;
   }

   case nir_intrinsic_emit_vertex_with_counter:
      /* emit_vertex triggers transform feedback but is otherwise a no-op. */
      if (b->shader->xfb_info)
         lower_emit_vertex_xfb(b, intr, state);
      break;

   default:
      return false;
   }

   nir_instr_remove(&intr->instr);
   return true;
}

static bool
collect_components(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   uint8_t *counts = data;
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   unsigned count = nir_intrinsic_component(intr) +
                    util_last_bit(nir_intrinsic_write_mask(intr));

   unsigned loc =
      nir_intrinsic_io_semantics(intr).location + nir_src_as_uint(intr->src[1]);

   uint8_t *total_count = &counts[loc];

   *total_count = MAX2(*total_count, count);
   return true;
}

/*
 * Create the pre-GS shader. This is a small compute 1x1x1 kernel that produces
 * an indirect draw to rasterize the produced geometry, as well as updates
 * transform feedback offsets and counters as applicable.
 */
static nir_shader *
agx_nir_create_pre_gs(struct lower_gs_state *state, const nir_shader *libagx,
                      bool indexed, bool restart, struct nir_xfb_info *xfb,
                      unsigned vertices_per_prim, uint8_t streams,
                      unsigned invocations)
{
   nir_builder b_ = nir_builder_init_simple_shader(
      MESA_SHADER_COMPUTE, &agx_nir_options, "Pre-GS patch up");
   nir_builder *b = &b_;

   /* Load the number of primitives input to the GS */
   nir_def *unrolled_in_prims = load_geometry_param(b, input_primitives);

   /* Setup the draw from the rasterization stream (0). */
   if (!state->rasterizer_discard) {
      libagx_build_gs_draw(
         b, nir_load_geometry_param_buffer_agx(b),
         previous_vertices(b, state, 0, unrolled_in_prims),
         restart ? previous_primitives(b, state, 0, unrolled_in_prims)
                 : nir_imm_int(b, 0));
   }

   /* Determine the number of primitives generated in each stream */
   nir_def *in_prims[MAX_VERTEX_STREAMS], *prims[MAX_VERTEX_STREAMS];

   u_foreach_bit(i, streams) {
      in_prims[i] = previous_xfb_primitives(b, state, i, unrolled_in_prims);
      prims[i] = in_prims[i];

      add_counter(b, load_geometry_param(b, prims_generated_counter[i]),
                  prims[i]);
   }

   if (xfb) {
      /* Write XFB addresses */
      nir_def *offsets[4] = {NULL};
      u_foreach_bit(i, xfb->buffers_written) {
         offsets[i] = libagx_setup_xfb_buffer(
            b, nir_load_geometry_param_buffer_agx(b), nir_imm_int(b, i));
      }

      /* Now clamp to the number that XFB captures */
      for (unsigned i = 0; i < xfb->output_count; ++i) {
         nir_xfb_output_info output = xfb->outputs[i];

         unsigned buffer = output.buffer;
         unsigned stream = xfb->buffer_to_stream[buffer];
         unsigned stride = xfb->buffers[buffer].stride;
         unsigned words_written = util_bitcount(output.component_mask);
         unsigned bytes_written = words_written * 4;

         /* Primitive P will write up to (but not including) offset:
          *
          *    xfb_offset + ((P - 1) * (verts_per_prim * stride))
          *               + ((verts_per_prim - 1) * stride)
          *               + output_offset
          *               + output_size
          *
          * Given an XFB buffer of size xfb_size, we get the inequality:
          *
          *    floor(P) <= (stride + xfb_size - xfb_offset - output_offset -
          *                     output_size) // (stride * verts_per_prim)
          */
         nir_def *size = load_geometry_param(b, xfb_size[buffer]);
         size = nir_iadd_imm(b, size, stride - output.offset - bytes_written);
         size = nir_isub(b, size, offsets[buffer]);
         size = nir_imax(b, size, nir_imm_int(b, 0));
         nir_def *max_prims = nir_udiv_imm(b, size, stride * vertices_per_prim);

         prims[stream] = nir_umin(b, prims[stream], max_prims);
      }

      nir_def *any_overflow = nir_imm_false(b);

      u_foreach_bit(i, streams) {
         nir_def *overflow = nir_ult(b, prims[i], in_prims[i]);
         any_overflow = nir_ior(b, any_overflow, overflow);

         store_geometry_param(b, xfb_prims[i], prims[i]);

         add_counter(b, load_geometry_param(b, xfb_overflow[i]),
                     nir_b2i32(b, overflow));

         add_counter(b, load_geometry_param(b, xfb_prims_generated_counter[i]),
                     prims[i]);
      }

      add_counter(b, load_geometry_param(b, xfb_any_overflow),
                  nir_b2i32(b, any_overflow));

      /* Update XFB counters */
      u_foreach_bit(i, xfb->buffers_written) {
         uint32_t prim_stride_B = xfb->buffers[i].stride * vertices_per_prim;
         unsigned stream = xfb->buffer_to_stream[i];

         nir_def *off_ptr = load_geometry_param(b, xfb_offs_ptrs[i]);
         nir_def *size = nir_imul_imm(b, prims[stream], prim_stride_B);
         add_counter(b, off_ptr, size);
      }
   }

   /* The geometry shader receives a number of input primitives. The driver
    * should disable this counter when tessellation is active TODO and count
    * patches separately.
    */
   add_counter(
      b,
      nir_load_stat_query_address_agx(b, .base = PIPE_STAT_QUERY_IA_PRIMITIVES),
      unrolled_in_prims);

   /* The geometry shader is invoked once per primitive (after unrolling
    * primitive restart). From the spec:
    *
    *    In case of instanced geometry shaders (see section 11.3.4.2) the
    *    geometry shader invocations count is incremented for each separate
    *    instanced invocation.
    */
   add_counter(b,
               nir_load_stat_query_address_agx(
                  b, .base = PIPE_STAT_QUERY_GS_INVOCATIONS),
               nir_imul_imm(b, unrolled_in_prims, invocations));

   nir_def *emitted_prims = nir_imm_int(b, 0);
   u_foreach_bit(i, streams) {
      emitted_prims =
         nir_iadd(b, emitted_prims,
                  previous_xfb_primitives(b, state, i, unrolled_in_prims));
   }

   add_counter(
      b,
      nir_load_stat_query_address_agx(b, .base = PIPE_STAT_QUERY_GS_PRIMITIVES),
      emitted_prims);

   /* Clipper queries are not well-defined, so we can emulate them in lots of
    * silly ways. We need the hardware counters to implement them properly. For
    * now, just consider all primitives emitted as passing through the clipper.
    * This satisfies spec text:
    *
    *    The number of primitives that reach the primitive clipping stage.
    *
    * and
    *
    *    If at least one vertex of the primitive lies inside the clipping
    *    volume, the counter is incremented by one or more. Otherwise, the
    *    counter is incremented by zero or more.
    */
   add_counter(
      b,
      nir_load_stat_query_address_agx(b, .base = PIPE_STAT_QUERY_C_PRIMITIVES),
      emitted_prims);

   add_counter(
      b,
      nir_load_stat_query_address_agx(b, .base = PIPE_STAT_QUERY_C_INVOCATIONS),
      emitted_prims);

   agx_preprocess_nir(b->shader, libagx);
   return b->shader;
}

static bool
rewrite_invocation_id(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_invocation_id)
      return false;

   b->cursor = nir_instr_remove(&intr->instr);
   nir_def_rewrite_uses(&intr->def, nir_u2uN(b, data, intr->def.bit_size));
   return true;
}

/*
 * Geometry shader instancing allows a GS to run multiple times. The number of
 * times is statically known and small. It's easiest to turn this into a loop
 * inside the GS, to avoid the feature "leaking" outside and affecting e.g. the
 * counts.
 */
static void
agx_nir_lower_gs_instancing(nir_shader *gs)
{
   unsigned nr_invocations = gs->info.gs.invocations;
   nir_function_impl *impl = nir_shader_get_entrypoint(gs);

   /* Each invocation can produce up to the shader-declared max_vertices, so
    * multiply it up for proper bounds check. Emitting more than the declared
    * max_vertices per invocation results in undefined behaviour, so erroneously
    * emitting more as asked on early invocations is a perfectly cromulent
    * behvaiour.
    */
   gs->info.gs.vertices_out *= gs->info.gs.invocations;

   /* Get the original function */
   nir_cf_list list;
   nir_cf_extract(&list, nir_before_impl(impl), nir_after_impl(impl));

   /* Create a builder for the wrapped function */
   nir_builder b = nir_builder_at(nir_after_block(nir_start_block(impl)));

   nir_variable *i =
      nir_local_variable_create(impl, glsl_uintN_t_type(16), NULL);
   nir_store_var(&b, i, nir_imm_intN_t(&b, 0, 16), ~0);
   nir_def *index = NULL;

   /* Create a loop in the wrapped function */
   nir_loop *loop = nir_push_loop(&b);
   {
      index = nir_load_var(&b, i);
      nir_push_if(&b, nir_uge_imm(&b, index, nr_invocations));
      {
         nir_jump(&b, nir_jump_break);
      }
      nir_pop_if(&b, NULL);

      b.cursor = nir_cf_reinsert(&list, b.cursor);
      nir_store_var(&b, i, nir_iadd_imm(&b, index, 1), ~0);

      /* Make sure we end the primitive between invocations. If the geometry
       * shader already ended the primitive, this will get optimized out.
       */
      nir_end_primitive(&b);
   }
   nir_pop_loop(&b, loop);

   /* We've mucked about with control flow */
   nir_metadata_preserve(impl, nir_metadata_none);

   /* Use the loop counter as the invocation ID each iteration */
   nir_shader_intrinsics_pass(gs, rewrite_invocation_id,
                              nir_metadata_control_flow, index);
}

static void
link_libagx(nir_shader *nir, const nir_shader *libagx)
{
   nir_link_shader_functions(nir, libagx);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_lower_indirect_derefs, nir_var_function_temp, 64);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types,
            nir_var_shader_temp | nir_var_function_temp | nir_var_mem_shared,
            glsl_get_cl_type_size_align);
   NIR_PASS(_, nir, nir_opt_deref);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_explicit_io,
            nir_var_shader_temp | nir_var_function_temp | nir_var_mem_shared |
               nir_var_mem_global,
            nir_address_format_62bit_generic);
}

bool
agx_nir_lower_gs(nir_shader *gs, const nir_shader *libagx,
                 bool rasterizer_discard, nir_shader **gs_count,
                 nir_shader **gs_copy, nir_shader **pre_gs,
                 enum mesa_prim *out_mode, unsigned *out_count_words)
{
   /* Lower I/O as assumed by the rest of GS lowering */
   if (gs->xfb_info != NULL) {
      NIR_PASS(_, gs, nir_io_add_const_offset_to_base,
               nir_var_shader_in | nir_var_shader_out);
      NIR_PASS(_, gs, nir_io_add_intrinsic_xfb_info);
   }

   NIR_PASS(_, gs, nir_lower_io_to_scalar, nir_var_shader_out, NULL, NULL);

   /* Collect output component counts so we can size the geometry output buffer
    * appropriately, instead of assuming everything is vec4.
    */
   uint8_t component_counts[NUM_TOTAL_VARYING_SLOTS] = {0};
   nir_shader_intrinsics_pass(gs, collect_components, nir_metadata_all,
                              component_counts);

   /* If geometry shader instancing is used, lower it away before linking
    * anything. Otherwise, smash the invocation ID to zero.
    */
   if (gs->info.gs.invocations != 1) {
      agx_nir_lower_gs_instancing(gs);
   } else {
      nir_function_impl *impl = nir_shader_get_entrypoint(gs);
      nir_builder b = nir_builder_at(nir_before_impl(impl));

      nir_shader_intrinsics_pass(gs, rewrite_invocation_id,
                                 nir_metadata_control_flow, nir_imm_int(&b, 0));
   }

   NIR_PASS(_, gs, nir_shader_intrinsics_pass, lower_gs_inputs,
            nir_metadata_control_flow, NULL);

   /* Lower geometry shader writes to contain all of the required counts, so we
    * know where in the various buffers we should write vertices.
    */
   NIR_PASS(_, gs, nir_lower_gs_intrinsics,
            nir_lower_gs_intrinsics_count_primitives |
               nir_lower_gs_intrinsics_per_stream |
               nir_lower_gs_intrinsics_count_vertices_per_primitive |
               nir_lower_gs_intrinsics_overwrite_incomplete |
               nir_lower_gs_intrinsics_always_end_primitive |
               nir_lower_gs_intrinsics_count_decomposed_primitives);

   /* Clean up after all that lowering we did */
   bool progress = false;
   do {
      progress = false;
      NIR_PASS(progress, gs, nir_lower_var_copies);
      NIR_PASS(progress, gs, nir_lower_variable_initializers,
               nir_var_shader_temp);
      NIR_PASS(progress, gs, nir_lower_vars_to_ssa);
      NIR_PASS(progress, gs, nir_copy_prop);
      NIR_PASS(progress, gs, nir_opt_constant_folding);
      NIR_PASS(progress, gs, nir_opt_algebraic);
      NIR_PASS(progress, gs, nir_opt_cse);
      NIR_PASS(progress, gs, nir_opt_dead_cf);
      NIR_PASS(progress, gs, nir_opt_dce);

      /* Unrolling lets us statically determine counts more often, which
       * otherwise would not be possible with multiple invocations even in the
       * simplest of cases.
       */
      NIR_PASS(progress, gs, nir_opt_loop_unroll);
   } while (progress);

   /* If we know counts at compile-time we can simplify, so try to figure out
    * the counts statically.
    */
   struct lower_gs_state gs_state = {
      .rasterizer_discard = rasterizer_discard,
   };

   nir_gs_count_vertices_and_primitives(
      gs, gs_state.static_count[GS_COUNTER_VERTICES],
      gs_state.static_count[GS_COUNTER_PRIMITIVES],
      gs_state.static_count[GS_COUNTER_XFB_PRIMITIVES], 4);

   /* Anything we don't know statically will be tracked by the count buffer.
    * Determine the layout for it.
    */
   for (unsigned i = 0; i < MAX_VERTEX_STREAMS; ++i) {
      for (unsigned c = 0; c < GS_NUM_COUNTERS; ++c) {
         gs_state.count_index[i][c] =
            (gs_state.static_count[c][i] < 0) ? gs_state.count_stride_el++ : -1;
      }
   }

   bool side_effects_for_rast = false;
   *gs_copy = agx_nir_create_gs_rast_shader(gs, libagx, &side_effects_for_rast);

   NIR_PASS(_, gs, nir_shader_intrinsics_pass, lower_id,
            nir_metadata_control_flow, NULL);

   link_libagx(gs, libagx);

   NIR_PASS(_, gs, nir_lower_idiv,
            &(const nir_lower_idiv_options){.allow_fp16 = true});

   /* All those variables we created should've gone away by now */
   NIR_PASS(_, gs, nir_remove_dead_variables, nir_var_function_temp, NULL);

   /* If there is any unknown count, we need a geometry count shader */
   if (gs_state.count_stride_el > 0)
      *gs_count = agx_nir_create_geometry_count_shader(gs, libagx, &gs_state);
   else
      *gs_count = NULL;

   /* Geometry shader outputs are staged to temporaries */
   struct agx_lower_output_to_var_state state = {0};

   u_foreach_bit64(slot, gs->info.outputs_written) {
      /* After enough optimizations, the shader metadata can go out of sync, fix
       * with our gathered info. Otherwise glsl_vector_type will assert fail.
       */
      if (component_counts[slot] == 0) {
         gs->info.outputs_written &= ~BITFIELD64_BIT(slot);
         continue;
      }

      const char *slot_name =
         gl_varying_slot_name_for_stage(slot, MESA_SHADER_GEOMETRY);

      for (unsigned i = 0; i < MAX_PRIM_OUT_SIZE; ++i) {
         gs_state.outputs[slot][i] = nir_variable_create(
            gs, nir_var_shader_temp,
            glsl_vector_type(GLSL_TYPE_UINT, component_counts[slot]),
            ralloc_asprintf(gs, "%s-%u", slot_name, i));
      }

      state.outputs[slot] = gs_state.outputs[slot][0];
   }

   NIR_PASS(_, gs, nir_shader_instructions_pass, agx_lower_output_to_var,
            nir_metadata_control_flow, &state);

   NIR_PASS(_, gs, nir_shader_intrinsics_pass, lower_gs_instr,
            nir_metadata_none, &gs_state);

   /* Determine if we are guaranteed to rasterize at least one vertex, so that
    * we can strip the prepass of side effects knowing they will execute in the
    * rasterization shader.
    */
   bool rasterizes_at_least_one_vertex =
      !rasterizer_discard && gs_state.static_count[0][0] > 0;

   /* Clean up after all that lowering we did */
   nir_lower_global_vars_to_local(gs);
   do {
      progress = false;
      NIR_PASS(progress, gs, nir_lower_var_copies);
      NIR_PASS(progress, gs, nir_lower_variable_initializers,
               nir_var_shader_temp);
      NIR_PASS(progress, gs, nir_lower_vars_to_ssa);
      NIR_PASS(progress, gs, nir_copy_prop);
      NIR_PASS(progress, gs, nir_opt_constant_folding);
      NIR_PASS(progress, gs, nir_opt_algebraic);
      NIR_PASS(progress, gs, nir_opt_cse);
      NIR_PASS(progress, gs, nir_opt_dead_cf);
      NIR_PASS(progress, gs, nir_opt_dce);
      NIR_PASS(progress, gs, nir_opt_loop_unroll);

   } while (progress);

   /* When rasterizing, we try to handle side effects sensibly. */
   if (rasterizes_at_least_one_vertex && side_effects_for_rast) {
      do {
         progress = false;
         NIR_PASS(progress, gs, nir_shader_intrinsics_pass,
                  strip_side_effect_from_main, nir_metadata_control_flow, NULL);

         NIR_PASS(progress, gs, nir_opt_dce);
         NIR_PASS(progress, gs, nir_opt_dead_cf);
      } while (progress);
   }

   /* All those variables we created should've gone away by now */
   NIR_PASS(_, gs, nir_remove_dead_variables, nir_var_function_temp, NULL);

   NIR_PASS(_, gs, nir_opt_sink, ~0);
   NIR_PASS(_, gs, nir_opt_move, ~0);

   NIR_PASS(_, gs, nir_shader_intrinsics_pass, lower_id,
            nir_metadata_control_flow, NULL);

   /* Create auxiliary programs */
   *pre_gs = agx_nir_create_pre_gs(
      &gs_state, libagx, true, gs->info.gs.output_primitive != MESA_PRIM_POINTS,
      gs->xfb_info, verts_in_output_prim(gs), gs->info.gs.active_stream_mask,
      gs->info.gs.invocations);

   /* Signal what primitive we want to draw the GS Copy VS with */
   *out_mode = gs->info.gs.output_primitive;
   *out_count_words = gs_state.count_stride_el;
   return true;
}

/*
 * Vertex shaders (tessellation evaluation shaders) before a geometry shader run
 * as a dedicated compute prepass. They are invoked as (count, instances, 1).
 * Their linear ID is therefore (instances * num vertices) + vertex ID.
 *
 * This function lowers their vertex shader I/O to compute.
 *
 * Vertex ID becomes an index buffer pull (without applying the topology). Store
 * output becomes a store into the global vertex output buffer.
 */
static bool
lower_vs_before_gs(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_store_output)
      return false;

   b->cursor = nir_instr_remove(&intr->instr);
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   nir_def *location = nir_iadd_imm(b, intr->src[1].ssa, sem.location);

   /* We inline the outputs_written because it's known at compile-time, even
    * with shader objects. This lets us constant fold a bit of address math.
    */
   nir_def *mask = nir_imm_int64(b, b->shader->info.outputs_written);

   nir_def *buffer;
   nir_def *nr_verts;
   if (b->shader->info.stage == MESA_SHADER_VERTEX) {
      buffer = nir_load_vs_output_buffer_agx(b);
      nr_verts =
         libagx_input_vertices(b, nir_load_input_assembly_buffer_agx(b));
   } else {
      assert(b->shader->info.stage == MESA_SHADER_TESS_EVAL);

      /* Instancing is unrolled during tessellation so nr_verts is ignored. */
      nr_verts = nir_imm_int(b, 0);
      buffer = libagx_tes_buffer(b, nir_load_tess_param_buffer_agx(b));
   }

   nir_def *linear_id = nir_iadd(b, nir_imul(b, load_instance_id(b), nr_verts),
                                 load_primitive_id(b));

   nir_def *addr =
      libagx_vertex_output_address(b, buffer, mask, linear_id, location);

   assert(nir_src_bit_size(intr->src[0]) == 32);
   addr = nir_iadd_imm(b, addr, nir_intrinsic_component(intr) * 4);

   nir_store_global(b, addr, 4, intr->src[0].ssa,
                    nir_intrinsic_write_mask(intr));
   return true;
}

bool
agx_nir_lower_vs_before_gs(struct nir_shader *vs,
                           const struct nir_shader *libagx)
{
   bool progress = false;

   /* Lower vertex stores to memory stores */
   progress |= nir_shader_intrinsics_pass(vs, lower_vs_before_gs,
                                          nir_metadata_control_flow, NULL);

   /* Link libagx, used in lower_vs_before_gs */
   if (progress)
      link_libagx(vs, libagx);

   return progress;
}

void
agx_nir_prefix_sum_gs(nir_builder *b, const void *data)
{
   const unsigned *words = data;

   b->shader->info.workgroup_size[0] = 1024;

   libagx_prefix_sum(b, load_geometry_param(b, count_buffer),
                     load_geometry_param(b, input_primitives),
                     nir_imm_int(b, *words),
                     nir_channel(b, nir_load_workgroup_id(b), 0));
}

void
agx_nir_prefix_sum_tess(nir_builder *b, const void *data)
{
   b->shader->info.workgroup_size[0] = 1024;
   libagx_prefix_sum_tess(b, nir_load_preamble(b, 1, 64, .base = 0));
}

void
agx_nir_gs_setup_indirect(nir_builder *b, const void *data)
{
   const struct agx_gs_setup_indirect_key *key = data;

   libagx_gs_setup_indirect(b, nir_load_preamble(b, 1, 64, .base = 0),
                            nir_imm_int(b, key->prim),
                            nir_channel(b, nir_load_local_invocation_id(b), 0));
}

void
agx_nir_unroll_restart(nir_builder *b, const void *data)
{
   const struct agx_unroll_restart_key *key = data;
   b->shader->info.workgroup_size[0] = 1024;

   nir_def *ia = nir_load_preamble(b, 1, 64, .base = 0);
   nir_def *draw = nir_channel(b, nir_load_workgroup_id(b), 0);
   nir_def *lane = nir_channel(b, nir_load_local_invocation_id(b), 0);
   nir_def *mode = nir_imm_int(b, key->prim);

   if (key->index_size_B == 1)
      libagx_unroll_restart_u8(b, ia, mode, draw, lane);
   else if (key->index_size_B == 2)
      libagx_unroll_restart_u16(b, ia, mode, draw, lane);
   else if (key->index_size_B == 4)
      libagx_unroll_restart_u32(b, ia, mode, draw, lane);
   else
      unreachable("invalid index size");
}

void
agx_nir_tessellate(nir_builder *b, const void *data)
{
   const struct agx_tessellator_key *key = data;
   b->shader->info.workgroup_size[0] = 64;

   nir_def *params = nir_load_preamble(b, 1, 64, .base = 0);
   nir_def *patch = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def *mode = nir_imm_int(b, key->mode);
   nir_def *partitioning = nir_imm_int(b, key->partitioning);
   nir_def *output_prim = nir_imm_int(b, key->output_primitive);

   if (key->prim == TESS_PRIMITIVE_ISOLINES)
      libagx_tess_isoline(b, params, mode, partitioning, output_prim, patch);
   else if (key->prim == TESS_PRIMITIVE_TRIANGLES)
      libagx_tess_tri(b, params, mode, partitioning, output_prim, patch);
   else if (key->prim == TESS_PRIMITIVE_QUADS)
      libagx_tess_quad(b, params, mode, partitioning, output_prim, patch);
   else
      unreachable("invalid tess primitive");
}

void
agx_nir_tess_setup_indirect(nir_builder *b, const void *data)
{
   const struct agx_tess_setup_indirect_key *key = data;

   nir_def *params = nir_load_preamble(b, 1, 64, .base = 0);
   nir_def *with_counts = nir_imm_bool(b, key->with_counts);
   nir_def *point_mode = nir_imm_bool(b, key->point_mode);

   libagx_tess_setup_indirect(b, params, with_counts, point_mode);
}

void
agx_nir_increment_statistic(nir_builder *b, const void *data)
{
   libagx_increment_statistic(b, nir_load_preamble(b, 1, 64, .base = 0));
}

void
agx_nir_increment_cs_invocations(nir_builder *b, const void *data)
{
   libagx_increment_cs_invocations(b, nir_load_preamble(b, 1, 64, .base = 0));
}

void
agx_nir_increment_ia_counters(nir_builder *b, const void *data)
{
   const struct agx_increment_ia_counters_key *key = data;
   b->shader->info.workgroup_size[0] = key->index_size_B ? 1024 : 1;

   nir_def *params = nir_load_preamble(b, 1, 64, .base = 0);
   nir_def *index_size_B = nir_imm_int(b, key->index_size_B);
   nir_def *thread = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);

   libagx_increment_ia_counters(b, params, index_size_B, thread);
}

void
agx_nir_predicate_indirect(nir_builder *b, const void *data)
{
   const struct agx_predicate_indirect_key *key = data;

   nir_def *params = nir_load_preamble(b, 1, 64, .base = 0);
   nir_def *indexed = nir_imm_bool(b, key->indexed);
   nir_def *thread = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);

   libagx_predicate_indirect(b, params, thread, indexed);
}

void
agx_nir_decompress(nir_builder *b, const void *data)
{
   const struct agx_decompress_key *key = data;

   nir_def *params = nir_load_preamble(b, 1, 64, .base = 0);
   nir_def *tile = nir_load_workgroup_id(b);
   nir_def *local = nir_channel(b, nir_load_local_invocation_id(b), 0);
   nir_def *samples = nir_imm_int(b, key->nr_samples);

   libagx_decompress(b, params, tile, local, samples);
}
