/*
 * Copyright © 2015 Connor Abbott
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
 *
 * Authors:
 *    Connor Abbott (cwabbott0@gmail.com)
 *
 */

#include "nir.h"
#include "nir_builder.h"

static bool
phi_srcs_equal(nir_def *a, nir_def *b)
{
   if (a == b)
      return true;

   if (a->parent_instr->type != b->parent_instr->type)
      return false;

   if (a->parent_instr->type != nir_instr_type_alu &&
       a->parent_instr->type != nir_instr_type_load_const)
      return false;

   if (!nir_instrs_equal(a->parent_instr, b->parent_instr))
      return false;

   /* nir_instrs_equal ignores exact/fast_math */
   if (a->parent_instr->type == nir_instr_type_alu) {
      nir_alu_instr *a_alu = nir_instr_as_alu(a->parent_instr);
      nir_alu_instr *b_alu = nir_instr_as_alu(b->parent_instr);
      if (a_alu->exact != b_alu->exact || a_alu->fp_fast_math != b_alu->fp_fast_math)
         return false;
   }

   return true;
}

static bool
can_rematerialize_phi_src(nir_block *block, nir_def *def)
{
   if (def->parent_instr->type == nir_instr_type_alu) {
      /* Restrict alu to movs. */
      nir_alu_instr *alu = nir_instr_as_alu(def->parent_instr);
      if (alu->op != nir_op_mov)
         return false;
      if (!nir_block_dominates(alu->src[0].src.ssa->parent_instr->block, block->imm_dom))
         return false;
      return true;
   } else if (def->parent_instr->type == nir_instr_type_load_const) {
      return true;
   }
   return false;
}

/*
 * This is a pass for removing phi nodes that look like:
 * a = phi(b, b, b, ...)
 *
 * Note that we can't always ignore undef sources here, or else we may create a
 * situation where the definition of b isn't dominated by its uses. We're
 * allowed to do this since the definition of b must dominate all of the
 * phi node's predecessors, which means it must dominate the phi node as well
 * as all of the phi node's uses. In essence, the phi node acts as a copy
 * instruction. b can't be another phi node in the same block, since the only
 * time when phi nodes can source other phi nodes defined in the same block is
 * at the loop header, and in that case one of the sources of the phi has to
 * be from before the loop and that source can't be b.
 */

static bool
remove_phis_block(nir_block *block, nir_builder *b)
{
   bool progress = false;

   nir_foreach_phi_safe(phi, block) {
      nir_def *def = NULL;
      bool srcs_same = true;
      bool needs_remat = false;

      nir_foreach_phi_src(src, phi) {
         /* For phi nodes at the beginning of loops, we may encounter some
          * sources from backedges that point back to the destination of the
          * same phi, i.e. something like:
          *
          * a = phi(a, b, ...)
          *
          * We can safely ignore these sources, since if all of the normal
          * sources point to the same definition, then that definition must
          * still dominate the phi node, and the phi will still always take
          * the value of that definition.
          */
         if (src->src.ssa == &phi->def)
            continue;

         /* Ignore undef sources. */
         if (nir_src_is_undef(src->src))
            continue;

         if (def == NULL) {
            def = src->src.ssa;
            if (!nir_block_dominates(def->parent_instr->block, block->imm_dom)) {
               if (!can_rematerialize_phi_src(block, def)) {
                  srcs_same = false;
                  break;
               }
               needs_remat = true;
            }
         } else if (!phi_srcs_equal(src->src.ssa, def)) {
            srcs_same = false;
            break;
         }
      }

      if (!srcs_same)
         continue;

      if (!def) {
         /* In this case, the phi had no non undef sources. So turn it into an undef. */
         b->cursor = nir_after_phis(block);
         def = nir_undef(b, phi->def.num_components, phi->def.bit_size);
      } else if (needs_remat) {
         b->cursor = nir_after_phis(block);
         nir_instr *remat = nir_instr_clone(b->shader, def->parent_instr);
         nir_builder_instr_insert(b, remat);
         def = nir_instr_def(remat);
      }

      nir_def_replace(&phi->def, def);

      progress = true;
   }

   return progress;
}

bool
nir_remove_single_src_phis_block(nir_block *block)
{
   assert(block->predecessors->entries <= 1);
   bool progress = false;
   nir_foreach_phi_safe(phi, block) {
      nir_def *def = NULL;
      nir_foreach_phi_src(src, phi) {
         def = src->src.ssa;
         break;
      }

      if (!def) {
         nir_builder b = nir_builder_create(nir_cf_node_get_function(&block->cf_node));
         b.cursor = nir_after_phis(block);
         def = nir_undef(&b, phi->def.num_components, phi->def.bit_size);
      }

      nir_def_replace(&phi->def, def);
      progress = true;
   }
   return progress;
}

static bool
nir_opt_remove_phis_impl(nir_function_impl *impl)
{
   bool progress = false;
   nir_builder bld = nir_builder_create(impl);

   nir_metadata_require(impl, nir_metadata_dominance);

   nir_foreach_block(block, impl) {
      progress |= remove_phis_block(block, &bld);
   }

   if (progress) {
      nir_metadata_preserve(impl, nir_metadata_control_flow);
   } else {
      nir_metadata_preserve(impl, nir_metadata_all);
   }

   return progress;
}

bool
nir_opt_remove_phis(nir_shader *shader)
{
   bool progress = false;

   nir_foreach_function_impl(impl, shader)
      progress = nir_opt_remove_phis_impl(impl) || progress;

   return progress;
}
