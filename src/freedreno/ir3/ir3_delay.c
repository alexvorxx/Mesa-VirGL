/*
 * Copyright © 2019 Google, Inc.
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "ir3.h"

#include "ir3_compiler.h"

/* The maximum number of nop's we may need to insert between two instructions.
 */
#define MAX_NOPS 6

/*
 * Helpers to figure out the necessary delay slots between instructions.  Used
 * both in scheduling pass(es) and the final pass to insert any required nop's
 * so that the shader program is valid.
 *
 * Note that this needs to work both pre and post RA, so we can't assume ssa
 * src iterators work.
 */

/* calculate required # of delay slots between the instruction that
 * assigns a value and the one that consumes
 */
int
ir3_delayslots(struct ir3_compiler *compiler,
               struct ir3_instruction *assigner,
               struct ir3_instruction *consumer, unsigned n, bool soft)
{
   /* generally don't count false dependencies, since this can just be
    * something like a barrier, or SSBO store.
    */
   if (__is_false_dep(consumer, n))
      return 0;

   /* worst case is cat1-3 (alu) -> cat4/5 needing 6 cycles, normal
    * alu -> alu needs 3 cycles, cat4 -> alu and texture fetch
    * handled with sync bits
    */

   if (is_meta(assigner) || is_meta(consumer))
      return 0;

   if (writes_addr0(assigner) || writes_addr1(assigner))
      return 6;

   if (soft && needs_ss(compiler, assigner, consumer))
      return soft_ss_delay(assigner);

   /* handled via sync flags: */
   if (needs_ss(compiler, assigner, consumer) ||
       is_sy_producer(assigner))
      return 0;

   /* scalar ALU -> scalar ALU depdendencies where the source and destination
    * register sizes match don't require any nops.
    */
   if (is_scalar_alu(assigner, compiler)) {
      assert(is_scalar_alu(consumer, compiler));
      /* If the sizes don't match then we need (ss) and needs_ss() should've
       * returned above.
       */
      assert((assigner->dsts[0]->flags & IR3_REG_HALF) ==
             (consumer->srcs[n]->flags & IR3_REG_HALF));
      return 0;
   }

   /* As far as we know, shader outputs don't need any delay. */
   if (consumer->opc == OPC_END || consumer->opc == OPC_CHMASK)
      return 0;

   /* assigner must be alu: */
   if (is_flow(consumer) || is_sfu(consumer) || is_tex(consumer) ||
       is_mem(consumer)) {
      return 6;
   } else {
      /* In mergedregs mode, there is an extra 2-cycle penalty when half of
       * a full-reg is read as a half-reg or when a half-reg is read as a
       * full-reg.
       */
      bool mismatched_half = (assigner->dsts[0]->flags & IR3_REG_HALF) !=
                             (consumer->srcs[n]->flags & IR3_REG_HALF);
      unsigned penalty = mismatched_half ? 3 : 0;
      if ((is_mad(consumer->opc) || is_madsh(consumer->opc)) && (n == 2)) {
         /* special case, 3rd src to cat3 not required on first cycle */
         return 1 + penalty;
      } else {
         return 3 + penalty;
      }
   }
}

unsigned
ir3_delayslots_with_repeat(struct ir3_compiler *compiler,
                           struct ir3_instruction *assigner,
                           struct ir3_instruction *consumer,
                           unsigned assigner_n, unsigned consumer_n)
{
   unsigned delay = ir3_delayslots(compiler, assigner, consumer, consumer_n, false);

   struct ir3_register *src = consumer->srcs[consumer_n];
   struct ir3_register *dst = assigner->dsts[assigner_n];

   if (assigner->repeat == 0 && consumer->repeat == 0)
      return delay;

   unsigned src_start = post_ra_reg_num(src) * reg_elem_size(src);
   unsigned dst_start = post_ra_reg_num(dst) * reg_elem_size(dst);

   /* If either side is a relative access, we can't really apply most of the
    * reasoning below because we don't know which component aliases which.
    * Just bail in this case.
    */
   if ((src->flags & IR3_REG_RELATIV) || (dst->flags & IR3_REG_RELATIV))
      return delay;

   /* MOVMSK seems to require that all users wait until the entire
    * instruction is finished, so just bail here.
    */
   if (assigner->opc == OPC_MOVMSK)
      return delay;

   /* TODO: Handle the combination of (rpt) and different component sizes
    * better like below. This complicates things significantly because the
    * components don't line up.
    */
   if ((src->flags & IR3_REG_HALF) != (dst->flags & IR3_REG_HALF))
      return delay;

   /* If an instruction has a (rpt), then it acts as a sequence of
    * instructions, reading its non-(r) sources at each cycle. First, get the
    * register num for the first instruction where they interfere:
    */

   unsigned first_num = MAX2(src_start, dst_start) / reg_elem_size(dst);

   /* Now, for that first conflicting half/full register, figure out the
    * sub-instruction within assigner/consumer it corresponds to. For (r)
    * sources, this should already return the correct answer of 0. However we
    * have to special-case the multi-mov instructions, where the
    * sub-instructions sometimes come from the src/dst indices instead.
    */
   unsigned first_src_instr;
   if (consumer->opc == OPC_SWZ || consumer->opc == OPC_GAT)
      first_src_instr = consumer_n;
   else
      first_src_instr = first_num - src->num;

   unsigned first_dst_instr;
   if (assigner->opc == OPC_SWZ || assigner->opc == OPC_SCT)
      first_dst_instr = assigner_n;
   else
      first_dst_instr = first_num - dst->num;

   /* The delay we return is relative to the *end* of assigner and the
    * *beginning* of consumer, because it's the number of nops (or other
    * things) needed between them. Any instructions after first_dst_instr
    * subtract from the delay, and so do any instructions before
    * first_src_instr. Calculate an offset to subtract from the non-rpt-aware
    * delay to account for that.
    *
    * Now, a priori, we need to go through this process for every
    * conflicting regnum and take the minimum of the offsets to make sure
    * that the appropriate number of nop's is inserted for every conflicting
    * pair of sub-instructions. However, as we go to the next conflicting
    * regnum (if any), the number of instructions after first_dst_instr
    * decreases by 1 and the number of source instructions before
    * first_src_instr correspondingly increases by 1, so the offset stays the
    * same for all conflicting registers.
    */
   unsigned offset = first_src_instr + (assigner->repeat - first_dst_instr);
   return offset > delay ? 0 : delay - offset;
}

