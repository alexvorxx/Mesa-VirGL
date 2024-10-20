/*
 * Copyright © 2020 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <err.h>
#include <stdio.h>

#include "ir3.h"
#include "ir3_assembler.h"
#include "ir3_shader.h"

/*
 * A test for delay-slot calculation.  Each test specifies ir3 assembly
 * for one or more instructions and the last instruction that consumes
 * the previously produced values.  And the expected number of delay
 * slots that would be needed before that last instruction.  Any source
 * registers in the last instruction which are not written in a previous
 * instruction are not counted.
 */

/* clang-format off */
#define TEST(n, ...) { # __VA_ARGS__, n }
/* clang-format on */

static const struct test {
   const char *asmstr;
   unsigned expected_delay;
} tests[] = {
   /* clang-format off */
   TEST(6,
      add.f r0.x, r2.x, r2.y
      rsq r0.x, r0.x
   ),
   TEST(3,
      mov.f32f32 r0.x, c0.x
      mov.f32f32 r0.y, c0.y
      add.f r0.x, r0.x, r0.y
   ),
   TEST(2,
      mov.f32f32 r0.x, c0.x
      mov.f32f32 r0.y, c0.y
      mov.f32f32 r0.z, c0.z
      mad.f32 r0.x, r0.x, r0.y, r0.z
   ),
   TEST(0,
      mov.f32f32 r0.x, c0.x
      rcp r0.x, r0.y
      add.f r0.x, r0.x, c0.x
   ),
   TEST(2,
      mov.f32f32 r0.x, c0.x
      mov.f32f32 r0.y, c0.y
      (rpt1)add.f r0.x, (r)r0.x, (r)c0.x
   ),
   TEST(2,
      (rpt1)mov.f32f32 r0.x, c0.x
      (rpt1)add.f r0.x, (r)r0.x, (r)c0.x
   ),
   TEST(3,
      mov.f32f32 r0.y, c0.y
      mov.f32f32 r0.x, c0.x
      (rpt1)add.f r0.x, (r)r0.x, (r)c0.x
   ),
   TEST(1,
      (rpt2)mov.f32f32 r0.x, (r)c0.x
      add.f r0.x, r0.x, c0.x
   ),
   TEST(2,
      (rpt2)mov.f32f32 r0.x, (r)c0.x
      add.f r0.x, r0.x, r0.y
   ),
   TEST(2,
      (rpt1)mov.f32f32 r0.x, (r)c0.x
      (rpt1)add.f r0.x, (r)r0.x, c0.x
   ),
   TEST(1,
      (rpt1)mov.f32f32 r0.y, (r)c0.x
      (rpt1)add.f r0.x, (r)r0.x, c0.x
   ),
   TEST(3,
      (rpt1)mov.f32f32 r0.x, (r)c0.x
      (rpt1)add.f r0.x, (r)r0.y, c0.x
   ),
   /* clang-format on */
};

static struct ir3_shader *
parse_asm(struct ir3_compiler *c, const char *asmstr)
{
   struct ir3_kernel_info info = {};
   FILE *in = fmemopen((void *)asmstr, strlen(asmstr), "r");
   struct ir3_shader *shader = ir3_parse_asm(c, &info, in);

   fclose(in);

   if (!shader)
      errx(-1, "assembler failed");

   return shader;
}

/**
 * ir3_delay_calc_* relies on the src/dst wrmask being correct even for ALU
 * instructions, so this sets it here.
 *
 * Note that this is not clever enough to know how many src/dst there are
 * for various tex/mem instructions.  But the rules for tex consuming alu
 * are the same as sfu consuming alu.
 */
static void
fixup_wrmask(struct ir3 *ir)
{
   struct ir3_block *block = ir3_start_block(ir);

   foreach_instr_safe (instr, &block->instr_list) {
      instr->dsts[0]->wrmask = MASK(instr->repeat + 1);
      foreach_src (reg, instr) {
         if (reg->flags & (IR3_REG_CONST | IR3_REG_IMMED))
            continue;

         if (reg->flags & IR3_REG_R)
            reg->wrmask = MASK(instr->repeat + 1);
         else
            reg->wrmask = 1;
      }
   }
}

/* Calculate the number of nops added before the last instruction by
 * ir3_legalize.
 */
static unsigned
calc_nops(struct ir3_block *block, struct ir3_instruction *last)
{
   unsigned nops = 0;

   foreach_instr_rev (instr, &block->instr_list) {
      if (instr == last)
         continue;

      if (instr->opc == OPC_NOP) {
         nops += 1 + instr->repeat;
      } else {
         if (is_alu(instr))
            nops += instr->nop;
         break;
      }
   }

   return nops;
}

int
main(int argc, char **argv)
{
   struct ir3_compiler *c;
   int result = 0;

   struct fd_dev_id dev_id = {
         .gpu_id = 630,
   };

   c = ir3_compiler_create(NULL, &dev_id, fd_dev_info_raw(&dev_id), &(struct ir3_compiler_options){});

   for (int i = 0; i < ARRAY_SIZE(tests); i++) {
      const struct test *test = &tests[i];
      struct ir3_shader *shader = parse_asm(c, test->asmstr);
      struct ir3 *ir = shader->variants->ir;

      fixup_wrmask(ir);

      ir3_debug_print(ir, "AFTER fixup_wrmask");

      struct ir3_block *block =
         list_first_entry(&ir->block_list, struct ir3_block, node);
      struct ir3_instruction *last = NULL;

      foreach_instr_rev (instr, &block->instr_list) {
         if (is_meta(instr))
            continue;
         last = instr;
         break;
      }

      int max_bary;
      ir3_legalize(ir, shader->variants, &max_bary);

      unsigned n = calc_nops(block, last);

      if (n != test->expected_delay) {
         printf("%d: FAIL: Expected delay %u, but got %u, for:\n%s\n", i,
                test->expected_delay, n, test->asmstr);
         result = -1;
      } else {
         printf("%d: PASS\n", i);
      }

      ir3_shader_destroy(shader);
   }

   ir3_compiler_destroy(c);

   return result;
}
