/*
 * Copyright © 2021 Google
 * Copyright © 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "nir.h"
#include "nir_builder.h"
#include "nir_deref.h"
#include "radv_constants.h"
#include "radv_nir.h"

struct lower_hit_attrib_deref_args {
   nir_variable_mode mode;
   uint32_t base_offset;
};

static bool
lower_hit_attrib_deref(nir_builder *b, nir_instr *instr, void *data)
{
   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);
   if (intrin->intrinsic != nir_intrinsic_load_deref && intrin->intrinsic != nir_intrinsic_store_deref)
      return false;

   struct lower_hit_attrib_deref_args *args = data;
   nir_deref_instr *deref = nir_src_as_deref(intrin->src[0]);
   if (!nir_deref_mode_is(deref, args->mode))
      return false;

   b->cursor = nir_after_instr(instr);

   nir_variable *var = nir_deref_instr_get_variable(deref);
   uint32_t location = args->base_offset + var->data.driver_location +
                       nir_deref_instr_get_const_offset(deref, glsl_get_natural_size_align_bytes);

   if (intrin->intrinsic == nir_intrinsic_load_deref) {
      uint32_t num_components = intrin->def.num_components;
      uint32_t bit_size = intrin->def.bit_size;

      nir_def *components[NIR_MAX_VEC_COMPONENTS];

      for (uint32_t comp = 0; comp < num_components; comp++) {
         uint32_t offset = location + comp * DIV_ROUND_UP(bit_size, 8);
         uint32_t base = offset / 4;
         uint32_t comp_offset = offset % 4;

         if (bit_size == 64) {
            components[comp] = nir_pack_64_2x32_split(b, nir_load_hit_attrib_amd(b, .base = base),
                                                      nir_load_hit_attrib_amd(b, .base = base + 1));
         } else if (bit_size == 32) {
            components[comp] = nir_load_hit_attrib_amd(b, .base = base);
         } else if (bit_size == 16) {
            components[comp] =
               nir_channel(b, nir_unpack_32_2x16(b, nir_load_hit_attrib_amd(b, .base = base)), comp_offset / 2);
         } else if (bit_size == 8) {
            components[comp] =
               nir_channel(b, nir_unpack_bits(b, nir_load_hit_attrib_amd(b, .base = base), 8), comp_offset);
         } else {
            assert(bit_size == 1);
            components[comp] = nir_i2b(b, nir_load_hit_attrib_amd(b, .base = base));
         }
      }

      nir_def_rewrite_uses(&intrin->def, nir_vec(b, components, num_components));
   } else {
      nir_def *value = intrin->src[1].ssa;
      uint32_t num_components = value->num_components;
      uint32_t bit_size = value->bit_size;

      for (uint32_t comp = 0; comp < num_components; comp++) {
         uint32_t offset = location + comp * DIV_ROUND_UP(bit_size, 8);
         uint32_t base = offset / 4;
         uint32_t comp_offset = offset % 4;

         nir_def *component = nir_channel(b, value, comp);

         if (bit_size == 64) {
            nir_store_hit_attrib_amd(b, nir_unpack_64_2x32_split_x(b, component), .base = base);
            nir_store_hit_attrib_amd(b, nir_unpack_64_2x32_split_y(b, component), .base = base + 1);
         } else if (bit_size == 32) {
            nir_store_hit_attrib_amd(b, component, .base = base);
         } else if (bit_size == 16) {
            nir_def *prev = nir_unpack_32_2x16(b, nir_load_hit_attrib_amd(b, .base = base));
            nir_def *components[2];
            for (uint32_t word = 0; word < 2; word++)
               components[word] = (word == comp_offset / 2) ? nir_channel(b, value, comp) : nir_channel(b, prev, word);
            nir_store_hit_attrib_amd(b, nir_pack_32_2x16(b, nir_vec(b, components, 2)), .base = base);
         } else if (bit_size == 8) {
            nir_def *prev = nir_unpack_bits(b, nir_load_hit_attrib_amd(b, .base = base), 8);
            nir_def *components[4];
            for (uint32_t byte = 0; byte < 4; byte++)
               components[byte] = (byte == comp_offset) ? nir_channel(b, value, comp) : nir_channel(b, prev, byte);
            nir_store_hit_attrib_amd(b, nir_pack_32_4x8(b, nir_vec(b, components, 4)), .base = base);
         } else {
            assert(bit_size == 1);
            nir_store_hit_attrib_amd(b, nir_b2i32(b, component), .base = base);
         }
      }
   }

   nir_instr_remove(instr);
   return true;
}

static bool
radv_lower_payload_arg_to_offset(nir_builder *b, nir_intrinsic_instr *instr, void *data)
{
   if (instr->intrinsic != nir_intrinsic_trace_ray)
      return false;

   nir_deref_instr *payload = nir_src_as_deref(instr->src[10]);
   assert(payload->deref_type == nir_deref_type_var);

   b->cursor = nir_before_instr(&instr->instr);
   nir_def *offset = nir_imm_int(b, payload->var->data.driver_location);

   nir_src_rewrite(&instr->src[10], offset);

   return true;
}

static bool
radv_nir_lower_rt_vars(nir_shader *shader, nir_variable_mode mode, uint32_t base_offset)
{
   bool progress = false;

   progress |= nir_lower_indirect_derefs(shader, mode, UINT32_MAX);

   progress |= nir_lower_vars_to_explicit_types(shader, mode, glsl_get_natural_size_align_bytes);

   if (shader->info.stage == MESA_SHADER_RAYGEN && mode == nir_var_function_temp)
      progress |= nir_shader_intrinsics_pass(shader, radv_lower_payload_arg_to_offset, nir_metadata_control_flow, NULL);

   struct lower_hit_attrib_deref_args args = {
      .mode = mode,
      .base_offset = base_offset,
   };

   progress |= nir_shader_instructions_pass(shader, lower_hit_attrib_deref, nir_metadata_control_flow, &args);

   if (progress) {
      nir_remove_dead_derefs(shader);
      nir_remove_dead_variables(shader, mode, NULL);
   }

   return progress;
}

bool
radv_nir_lower_hit_attrib_derefs(nir_shader *shader)
{
   return radv_nir_lower_rt_vars(shader, nir_var_ray_hit_attrib, 0);
}

bool
radv_nir_lower_ray_payload_derefs(nir_shader *shader, uint32_t offset)
{
   bool progress = radv_nir_lower_rt_vars(shader, nir_var_function_temp, RADV_MAX_HIT_ATTRIB_SIZE + offset);
   progress |= radv_nir_lower_rt_vars(shader, nir_var_shader_call_data, RADV_MAX_HIT_ATTRIB_SIZE + offset);
   return progress;
}
