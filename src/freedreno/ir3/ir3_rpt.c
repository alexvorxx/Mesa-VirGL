/*
 * Copyright 2024 Igalia S.L.
 * SPDX-License-Identifier: MIT
 */

#include "ir3_nir.h"

static void
rpt_list_split(struct list_head *list, struct list_head *at)
{
   struct list_head *new_last = at->prev;
   new_last->next = list;
   at->prev = list->prev;
   list->prev->next = at;
   list->prev = new_last;
}

static enum ir3_register_flags
rpt_compatible_src_flags(struct ir3_register *src)
{
   return src->flags &
          (IR3_REG_SABS | IR3_REG_SNEG | IR3_REG_FABS | IR3_REG_FNEG |
           IR3_REG_BNOT | IR3_REG_CONST | IR3_REG_IMMED | IR3_REG_SSA |
           IR3_REG_HALF | IR3_REG_SHARED);
}

static enum ir3_register_flags
rpt_compatible_dst_flags(struct ir3_instruction *instr)
{
   return instr->dsts[0]->flags & (IR3_REG_SSA | IR3_REG_HALF | IR3_REG_SHARED);
}

static enum ir3_register_flags
rpt_illegal_src_flags(struct ir3_register *src)
{
   return src->flags & (IR3_REG_ARRAY | IR3_REG_RELATIV);
}

static enum ir3_instruction_flags
rpt_compatible_instr_flags(struct ir3_instruction *instr)
{
   return instr->flags & IR3_INSTR_SAT;
}

static bool
srcs_can_rpt(struct ir3_register *src, struct ir3_register *rpt_src)
{
   if (rpt_illegal_src_flags(src) != 0 || rpt_illegal_src_flags(rpt_src) != 0)
      return false;
   if (rpt_compatible_src_flags(src) != rpt_compatible_src_flags(rpt_src))
      return false;
   if ((src->flags & IR3_REG_IMMED) && src->uim_val != rpt_src->uim_val)
      return false;
   return true;
}

static bool
can_rpt(struct ir3_instruction *instr, struct ir3_instruction *rpt,
        unsigned rpt_n)
{
   if (rpt->ip != instr->ip + rpt_n)
      return false;
   if (rpt->opc != instr->opc)
      return false;
   if (rpt_compatible_instr_flags(rpt) != rpt_compatible_instr_flags(instr))
      return false;
   if (rpt_compatible_dst_flags(rpt) != rpt_compatible_dst_flags(instr))
      return false;
   if (instr->srcs_count != rpt->srcs_count)
      return false;

   foreach_src_n (src, src_n, instr) {
      if (!srcs_can_rpt(src, rpt->srcs[src_n]))
         return false;
   }

   return true;
}

static bool
cleanup_rpt_instr(struct ir3_instruction *instr)
{
   if (!ir3_instr_is_first_rpt(instr))
      return false;

   unsigned rpt_n = 1;
   foreach_instr_rpt_excl (rpt, instr) {
      if (!can_rpt(instr, rpt, rpt_n++)) {
         rpt_list_split(&instr->rpt_node, &rpt->rpt_node);

         /* We have to do this recursively since later repetitions might come
          * before the first in the instruction list.
          */
         cleanup_rpt_instr(rpt);
         return true;
      }
   }

   return false;
}

/* Pre-RA pass to clean up repetition groups that can never be merged into a rpt
 * instruction. This ensures we don't needlessly allocate merge sets for them.
 */
bool
ir3_cleanup_rpt(struct ir3 *ir, struct ir3_shader_variant *v)
{
   ir3_count_instructions(ir);
   bool progress = false;

   foreach_block (block, &ir->block_list) {
      foreach_instr (instr, &block->instr_list)
         progress |= cleanup_rpt_instr(instr);
   }

   return progress;
}
