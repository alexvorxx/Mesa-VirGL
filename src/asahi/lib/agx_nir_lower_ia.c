/*
 * Copyright 2023 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "asahi/compiler/agx_compile.h"
#include "compiler/nir/nir_builder.h"
#include "shaders/geometry.h"
#include "util/compiler.h"
#include "agx_nir_lower_gs.h"
#include "libagx_shaders.h"
#include "nir.h"
#include "nir_builder_opcodes.h"
#include "nir_intrinsics.h"
#include "shader_enums.h"

/*
 * This file implements basic input assembly in software. It runs on software
 * vertex shaders, as part of geometry/tessellation lowering. It does not apply
 * the topology, which happens in the geometry shader.
 */
static nir_def *
load_vertex_id(nir_builder *b, unsigned index_size_B)
{
   nir_def *id = nir_load_primitive_id(b);

   /* If drawing with an index buffer, pull the vertex ID. Otherwise, the
    * vertex ID is just the index as-is.
    */
   if (index_size_B) {
      nir_def *ia = nir_load_input_assembly_buffer_agx(b);

      nir_def *index =
         libagx_load_index_buffer(b, ia, id, nir_imm_int(b, index_size_B));

      id = nir_u2uN(b, index, id->bit_size);
   }

   /* Add the "start", either an index bias or a base vertex. This must happen
    * after indexing for proper index bias behaviour.
    */
   return nir_iadd(b, id, nir_load_first_vertex(b));
}

static bool
lower_vertex_id(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_vertex_id)
      return false;

   unsigned *index_size_B = data;
   b->cursor = nir_instr_remove(&intr->instr);
   assert(intr->def.bit_size == 32);
   nir_def_rewrite_uses(&intr->def, load_vertex_id(b, *index_size_B));
   return true;
}

bool
agx_nir_lower_index_buffer(nir_shader *s, unsigned index_size_B)
{
   return nir_shader_intrinsics_pass(s, lower_vertex_id,
                                     nir_metadata_control_flow, &index_size_B);
}
