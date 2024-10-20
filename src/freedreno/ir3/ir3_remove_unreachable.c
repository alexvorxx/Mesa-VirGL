/*
 * Copyright © 2021 Valve Corporation
 * SPDX-License-Identifier: MIT
 */

#include "ir3.h"

/* Sometimes we can get unreachable blocks from NIR. In particular this happens
 * for blocks after an if where both sides end in a break/continue. These blocks
 * are then reachable only via the physical CFG. This pass deletes these blocks
 * and reroutes the physical edge past it.
 */

static void
delete_block(struct ir3 *ir, struct ir3_block *block)
{
   struct ir3_instruction *end = NULL;
   foreach_instr (instr, &block->instr_list) {
      if (instr->opc == OPC_END) {
         end = instr;
         break;
      }
   }

   /* The end block can be legitimately unreachable if the shader only exits via
    * discarding. ir3_legalize will then insert a branch to the end. Keep the
    * block around but delete all the other instructions and make the end not
    * take any sources, so that we don't have any dangling references to other
    * unreachable blocks.
    */
   if (end) {
      foreach_instr_safe (instr, &block->instr_list) {
         if (instr != end)
            list_delinit(&instr->node);
      }
      end->srcs_count = 0;
      return;
   }

   for (unsigned i = 0; i < 2; i++) {
      struct ir3_block *succ = block->successors[i];
      if (!succ)
         continue;

      unsigned pred_idx = ir3_block_get_pred_index(succ, block);

      /* If this isn't the last predecessor, we swap it with the last before
       * removing it.
       */
      bool swap_pred = pred_idx != succ->predecessors_count - 1;

      foreach_instr (phi, &succ->instr_list) {
         if (phi->opc != OPC_META_PHI)
            break;

         if (swap_pred)
            phi->srcs[pred_idx] = phi->srcs[phi->srcs_count - 1];
         phi->srcs_count--;
      }
      if (swap_pred) {
         succ->predecessors[pred_idx] =
            succ->predecessors[succ->predecessors_count - 1];
      }
      succ->predecessors_count--;
   }
}

bool
ir3_remove_unreachable(struct ir3 *ir)
{
   bool progress = false;
   foreach_block_safe (block, &ir->block_list) {
      if (block != ir3_start_block(ir) && block->predecessors_count == 0) {
         delete_block(ir, block);
         list_del(&block->node);
         progress = true;
      }
   }

   return progress;
}
