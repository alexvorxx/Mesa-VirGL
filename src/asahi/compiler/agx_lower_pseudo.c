/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "agx_builder.h"
#include "agx_compiler.h"
#include "agx_opcodes.h"

/* Lower pseudo instructions created during optimization. */

static agx_instr *
while_for_break_if(agx_builder *b, agx_instr *I)
{
   if (I->op == AGX_OPCODE_BREAK_IF_FCMP) {
      return agx_while_fcmp(b, I->src[0], I->src[1], I->nest, I->fcond,
                            !I->invert_cond, NULL);
   } else {
      return agx_while_icmp(b, I->src[0], I->src[1], I->nest, I->icond,
                            !I->invert_cond, NULL);
   }
}

static agx_instr *
cmpsel_for_break_if(agx_builder *b, agx_instr *I)
{
   agx_index r0l = agx_register(0, AGX_SIZE_16);

   /* If the condition is true, set r0l to nest to break */
   agx_index t = agx_immediate(I->nest);
   agx_index f = r0l;

   if (I->invert_cond) {
      agx_index temp = t;
      t = f;
      f = temp;
   }

   if (I->op == AGX_OPCODE_BREAK_IF_FCMP)
      agx_fcmpsel_to(b, r0l, I->src[0], I->src[1], t, f, I->fcond);
   else
      agx_icmpsel_to(b, r0l, I->src[0], I->src[1], t, f, I->icond);

   return agx_push_exec(b, 0);
}

static void
swap(agx_builder *b, agx_index x, agx_index y)
{
   assert(!x.memory && "already lowered");
   assert(!y.memory && "already lowered");

   /* We can swap lo/hi halves of a 32-bit register with a 32-bit extr */
   if (x.size == AGX_SIZE_16 && (x.value >> 1) == (y.value >> 1)) {

      assert(((x.value & 1) == (1 - (y.value & 1))) &&
             "no trivial swaps, and only 2 halves of a register");

      /* r0 = extr r0, r0, #16
       *    = (((r0 << 32) | r0) >> 16) & 0xFFFFFFFF
       *    = (((r0 << 32) >> 16) & 0xFFFFFFFF) | (r0 >> 16)
       *    = (r0l << 16) | r0h
       */
      agx_index reg32 = agx_register(x.value & ~1, AGX_SIZE_32);
      agx_extr_to(b, reg32, reg32, reg32, agx_immediate(16), 0);
   } else {
      /* Otherwise, we're swapping GPRs and fallback on a XOR swap. */
      agx_xor_to(b, x, x, y);
      agx_xor_to(b, y, x, y);
      agx_xor_to(b, x, x, y);
   }
}

static agx_instr *
lower(agx_builder *b, agx_instr *I)
{
   switch (I->op) {

   /* Various instructions are implemented as bitwise truth tables */
   case AGX_OPCODE_MOV:
      return agx_bitop_to(b, I->dest[0], I->src[0], agx_zero(), AGX_BITOP_MOV);

   case AGX_OPCODE_NOT:
      return agx_bitop_to(b, I->dest[0], I->src[0], agx_zero(), AGX_BITOP_NOT);

   /* Unfused comparisons are fused with a 0/1 select */
   case AGX_OPCODE_ICMP:
      return agx_icmpsel_to(b, I->dest[0], I->src[0], I->src[1],
                            agx_immediate(I->invert_cond ? 0 : 1),
                            agx_immediate(I->invert_cond ? 1 : 0), I->icond);

   case AGX_OPCODE_FCMP:
      return agx_fcmpsel_to(b, I->dest[0], I->src[0], I->src[1],
                            agx_immediate(I->invert_cond ? 0 : 1),
                            agx_immediate(I->invert_cond ? 1 : 0), I->fcond);

   case AGX_OPCODE_BALLOT:
      return agx_icmp_ballot_to(b, I->dest[0], I->src[0], agx_zero(),
                                AGX_ICOND_UEQ, true /* invert */);

   case AGX_OPCODE_QUAD_BALLOT:
      return agx_icmp_quad_ballot_to(b, I->dest[0], I->src[0], agx_zero(),
                                     AGX_ICOND_UEQ, true /* invert */);

   /* Writes to the nesting counter lowered to the real register */
   case AGX_OPCODE_BEGIN_CF:
      return agx_mov_imm_to(b, agx_register(0, AGX_SIZE_16), 0);

   case AGX_OPCODE_BREAK:
      agx_mov_imm_to(b, agx_register(0, AGX_SIZE_16), I->nest);
      return agx_pop_exec(b, 0);

   case AGX_OPCODE_BREAK_IF_ICMP:
   case AGX_OPCODE_BREAK_IF_FCMP: {
      if (I->nest == 1)
         return while_for_break_if(b, I);
      else
         return cmpsel_for_break_if(b, I);
   }

   case AGX_OPCODE_SWAP:
      swap(b, I->src[0], I->src[1]);
      return (void *)true;

   case AGX_OPCODE_EXPORT:
      /* We already lowered exports during RA, we just need to remove them late
       * after inserting waits.
       */
      return (void *)true;

   default:
      return NULL;
   }
}

void
agx_lower_pseudo(agx_context *ctx)
{
   agx_foreach_instr_global_safe(ctx, I) {
      agx_builder b = agx_init_builder(ctx, agx_before_instr(I));

      if (lower(&b, I))
         agx_remove_instruction(I);
   }
}
