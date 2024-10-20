/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2023 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_nir.h"
#include "nir.h"
#include "nir_builder.h"
#include "radv_device.h"
#include "radv_nir.h"
#include "radv_physical_device.h"
#include "radv_shader.h"

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

void
radv_nir_lower_io_to_scalar_early(nir_shader *nir, nir_variable_mode mask)
{
   bool progress = false;

   NIR_PASS(progress, nir, nir_lower_io_to_scalar_early, mask);
   if (progress) {
      /* Optimize the new vector code and then remove dead vars */
      NIR_PASS(_, nir, nir_copy_prop);
      NIR_PASS(_, nir, nir_opt_shrink_vectors, true);

      if (mask & nir_var_shader_out) {
         /* Optimize swizzled movs of load_const for nir_link_opt_varyings's constant propagation. */
         NIR_PASS(_, nir, nir_opt_constant_folding);

         /* For nir_link_opt_varyings's duplicate input opt */
         NIR_PASS(_, nir, nir_opt_cse);
      }

      /* Run copy-propagation to help remove dead output variables (some shaders have useless copies
       * to/from an output), so compaction later will be more effective.
       *
       * This will have been done earlier but it might not have worked because the outputs were
       * vector.
       */
      if (nir->info.stage == MESA_SHADER_TESS_CTRL)
         NIR_PASS(_, nir, nir_opt_copy_prop_vars);

      NIR_PASS(_, nir, nir_opt_dce);
      NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_function_temp | nir_var_shader_in | nir_var_shader_out, NULL);
   }
}

void
radv_nir_lower_io(struct radv_device *device, nir_shader *nir)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in, type_size_vec4, 0);
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_out, type_size_vec4, nir_lower_io_lower_64bit_to_32);
   } else {
      NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size_vec4,
               nir_lower_io_lower_64bit_to_32);
   }

   /* This pass needs actual constants */
   NIR_PASS(_, nir, nir_opt_constant_folding);

   NIR_PASS(_, nir, nir_io_add_const_offset_to_base, nir_var_shader_in | nir_var_shader_out);

   if (nir->xfb_info) {
      NIR_PASS(_, nir, nir_io_add_intrinsic_xfb_info);

      if (pdev->use_ngg_streamout) {
         /* The total number of shader outputs is required for computing the pervertex LDS size for
          * VS/TES when lowering NGG streamout.
          */
         nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, nir->info.stage);
      }
   }

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      /* Recompute FS input intrinsic bases to make sure that there are no gaps
       * between the FS input slots.
       */
      nir_recompute_io_bases(nir, nir_var_shader_in);
   }

   NIR_PASS_V(nir, nir_opt_dce);
   NIR_PASS_V(nir, nir_remove_dead_variables, nir_var_shader_in | nir_var_shader_out, NULL);
}

/* IO slot layout for stages that aren't linked. */
enum {
   RADV_IO_SLOT_POS = 0,
   RADV_IO_SLOT_CLIP_DIST0,
   RADV_IO_SLOT_CLIP_DIST1,
   RADV_IO_SLOT_PSIZ,
   RADV_IO_SLOT_VAR0, /* 0..31 */
};

unsigned
radv_map_io_driver_location(unsigned semantic)
{
   if ((semantic >= VARYING_SLOT_PATCH0 && semantic < VARYING_SLOT_TESS_MAX) ||
       semantic == VARYING_SLOT_TESS_LEVEL_INNER || semantic == VARYING_SLOT_TESS_LEVEL_OUTER)
      return ac_shader_io_get_unique_index_patch(semantic);

   switch (semantic) {
   case VARYING_SLOT_POS:
      return RADV_IO_SLOT_POS;
   case VARYING_SLOT_CLIP_DIST0:
      return RADV_IO_SLOT_CLIP_DIST0;
   case VARYING_SLOT_CLIP_DIST1:
      return RADV_IO_SLOT_CLIP_DIST1;
   case VARYING_SLOT_PSIZ:
      return RADV_IO_SLOT_PSIZ;
   default:
      assert(semantic >= VARYING_SLOT_VAR0 && semantic <= VARYING_SLOT_VAR31);
      return RADV_IO_SLOT_VAR0 + (semantic - VARYING_SLOT_VAR0);
   }
}

bool
radv_nir_lower_io_to_mem(struct radv_device *device, struct radv_shader_stage *stage)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_shader_info *info = &stage->info;
   ac_nir_map_io_driver_location map_input = info->inputs_linked ? NULL : radv_map_io_driver_location;
   ac_nir_map_io_driver_location map_output = info->outputs_linked ? NULL : radv_map_io_driver_location;
   nir_shader *nir = stage->nir;

   if (nir->info.stage == MESA_SHADER_VERTEX) {
      if (info->vs.as_ls) {
         NIR_PASS_V(nir, ac_nir_lower_ls_outputs_to_mem, map_output, info->vs.tcs_in_out_eq,
                    info->vs.hs_inputs_read, info->vs.tcs_temp_only_input_mask);
         return true;
      } else if (info->vs.as_es) {
         NIR_PASS_V(nir, ac_nir_lower_es_outputs_to_mem, map_output, pdev->info.gfx_level, info->esgs_itemsize, info->gs_inputs_read);
         return true;
      }
   } else if (nir->info.stage == MESA_SHADER_TESS_CTRL) {
      NIR_PASS_V(nir, ac_nir_lower_hs_inputs_to_mem, map_input, info->vs.tcs_in_out_eq, info->vs.tcs_temp_only_input_mask);
      NIR_PASS_V(nir, ac_nir_lower_hs_outputs_to_mem, map_output, pdev->info.gfx_level,
                 info->tcs.tes_inputs_read, info->tcs.tes_patch_inputs_read, info->wave_size, false);

      return true;
   } else if (nir->info.stage == MESA_SHADER_TESS_EVAL) {
      NIR_PASS_V(nir, ac_nir_lower_tes_inputs_to_mem, map_input);

      if (info->tes.as_es) {
         NIR_PASS_V(nir, ac_nir_lower_es_outputs_to_mem, map_output, pdev->info.gfx_level, info->esgs_itemsize, info->gs_inputs_read);
      }

      return true;
   } else if (nir->info.stage == MESA_SHADER_GEOMETRY) {
      NIR_PASS_V(nir, ac_nir_lower_gs_inputs_to_mem, map_input, pdev->info.gfx_level, false);
      return true;
   } else if (nir->info.stage == MESA_SHADER_TASK) {
      ac_nir_lower_task_outputs_to_mem(nir, AC_TASK_PAYLOAD_ENTRY_BYTES, pdev->task_info.num_entries,
                                       info->cs.has_query);
      return true;
   } else if (nir->info.stage == MESA_SHADER_MESH) {
      ac_nir_lower_mesh_inputs_to_mem(nir, AC_TASK_PAYLOAD_ENTRY_BYTES, pdev->task_info.num_entries);
      return true;
   }

   return false;
}

static bool
radv_nir_lower_draw_id_to_zero_callback(struct nir_builder *b, nir_intrinsic_instr *intrin, UNUSED void *state)
{
   if (intrin->intrinsic != nir_intrinsic_load_draw_id)
      return false;

   nir_def *replacement = nir_imm_zero(b, intrin->def.num_components, intrin->def.bit_size);
   nir_def_replace(&intrin->def, replacement);
   nir_instr_free(&intrin->instr);

   return true;
}

bool
radv_nir_lower_draw_id_to_zero(nir_shader *shader)
{
   return nir_shader_intrinsics_pass(shader, radv_nir_lower_draw_id_to_zero_callback, nir_metadata_control_flow, NULL);
}
