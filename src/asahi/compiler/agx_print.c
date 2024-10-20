/*
 * Copyright 2021 Alyssa Rosenzweig
 * Copyright 2019-2020 Collabora, Ltd.
 * SPDX-License-Identifier: MIT
 */

#include "agx_builder.h"
#include "agx_compiler.h"

static void
agx_print_sized(char prefix, unsigned value, enum agx_size size, FILE *fp)
{
   switch (size) {
   case AGX_SIZE_16:
      fprintf(fp, "%c%u%c", prefix, value >> 1, (value & 1) ? 'h' : 'l');
      return;
   case AGX_SIZE_32:
      assert((value & 1) == 0);
      fprintf(fp, "%c%u", prefix, value >> 1);
      return;
   case AGX_SIZE_64:
      assert((value & 1) == 0);
      fprintf(fp, "%c%u:%c%u", prefix, value >> 1, prefix, (value >> 1) + 1);
      return;
   }

   unreachable("Invalid size");
}

static void
agx_print_reg(agx_index index, unsigned value, FILE *fp)
{
   agx_print_sized('r', value, index.size, fp);

   if (agx_channels(index) > 1) {
      unsigned last =
         value + agx_size_align_16(index.size) * (agx_channels(index) - 1);

      fprintf(fp, "...");

      if (index.memory)
         fprintf(fp, "m");
      agx_print_sized('r', last, index.size, fp);
   }
}

void
agx_print_index(agx_index index, bool is_float, FILE *fp)
{
   if (index.memory)
      fprintf(fp, "m");

   switch (index.type) {
   case AGX_INDEX_NULL:
      fprintf(fp, "_");
      return;

   case AGX_INDEX_NORMAL:
      if (index.cache)
         fprintf(fp, "$");

      if (index.discard)
         fprintf(fp, "`");

      if (index.kill)
         fprintf(fp, "*");

      fprintf(fp, "%u", index.value);
      break;

   case AGX_INDEX_IMMEDIATE:
      if (is_float) {
         assert(index.value < 0x100);
         fprintf(fp, "#%f", agx_minifloat_decode(index.value));
      } else {
         fprintf(fp, "#%u", index.value);
      }

      break;

   case AGX_INDEX_UNDEF:
      fprintf(fp, "undef");
      break;

   case AGX_INDEX_UNIFORM:
      agx_print_sized('u', index.value, index.size, fp);
      break;

   case AGX_INDEX_REGISTER:
      agx_print_reg(index, index.value, fp);
      break;

   default:
      unreachable("Invalid index type");
   }

   if (index.type == AGX_INDEX_NORMAL) {
      /* Print length suffixes if not implied */
      if (index.size == AGX_SIZE_16)
         fprintf(fp, "h");
      else if (index.size == AGX_SIZE_64)
         fprintf(fp, "d");

      /* Print assigned register if we have one */
      if (index.has_reg) {
         fprintf(fp, "(");
         if (index.memory)
            fprintf(fp, "m");

         agx_print_reg(index, index.reg, fp);
         fprintf(fp, ")");
      }
   }

   if (index.abs)
      fprintf(fp, ".abs");

   if (index.neg)
      fprintf(fp, ".neg");
}

static struct agx_opcode_info
agx_get_opcode_info_for_print(const agx_instr *I)
{
   struct agx_opcode_info info = agx_opcodes_info[I->op];

   if (I->op == AGX_OPCODE_BITOP) {
      const char *bitops[16] = {
         [AGX_BITOP_NOR] = "nor",     [AGX_BITOP_ANDN2] = "andn2",
         [AGX_BITOP_ANDN1] = "andn1", [AGX_BITOP_XOR] = "xor",
         [AGX_BITOP_NAND] = "nand",   [AGX_BITOP_AND] = "and",
         [AGX_BITOP_XNOR] = "xnor",   [AGX_BITOP_ORN2] = "orn2",
         [AGX_BITOP_ORN1] = "orn1",   [AGX_BITOP_OR] = "or",
      };

      if (bitops[I->truth_table] != NULL) {
         info.name = bitops[I->truth_table];
         info.immediates &= ~AGX_IMMEDIATE_TRUTH_TABLE;
      }
   }

   return info;
}

void
agx_print_instr(const agx_instr *I, FILE *fp)
{
   assert(I->op < AGX_NUM_OPCODES);
   struct agx_opcode_info info = agx_get_opcode_info_for_print(I);
   bool print_comma = false;

   fprintf(fp, "   ");

   agx_foreach_dest(I, d) {
      if (print_comma)
         fprintf(fp, ", ");
      else
         print_comma = true;

      agx_print_index(I->dest[d], false, fp);
   }

   if (I->nr_dests) {
      fprintf(fp, " = ");
      print_comma = false;
   }

   fprintf(fp, "%s", info.name);

   if (I->saturate)
      fprintf(fp, ".sat");

   if (I->last)
      fprintf(fp, ".last");

   fprintf(fp, " ");

   agx_foreach_src(I, s) {
      if (print_comma)
         fprintf(fp, ", ");
      else
         print_comma = true;

      agx_print_index(I->src[s],
                      agx_opcodes_info[I->op].is_float &&
                         !(s >= 2 && I->op == AGX_OPCODE_FCMPSEL),
                      fp);
   }

   if (I->mask) {
      fprintf(fp, ", ");

      for (unsigned i = 0; i < 4; ++i) {
         if (I->mask & (1 << i))
            fprintf(fp, "%c", "xyzw"[i]);
      }
   }

   /* TODO: Do better for enums, truth tables, etc */
   if (info.immediates) {
      if (print_comma)
         fprintf(fp, ", ");
      else
         print_comma = true;

      fprintf(fp, "#%" PRIx64, I->imm);
   }

   if (info.immediates & AGX_IMMEDIATE_DIM) {
      if (print_comma)
         fprintf(fp, ", ");
      else
         print_comma = true;

      fputs(agx_dim_as_str(I->dim), fp);
   }

   if (info.immediates & AGX_IMMEDIATE_SCOREBOARD) {
      if (print_comma)
         fprintf(fp, ", ");
      else
         print_comma = true;

      fprintf(fp, "slot %u", I->scoreboard);
   }

   if (info.immediates & AGX_IMMEDIATE_NEST) {
      if (print_comma)
         fprintf(fp, ", ");
      else
         print_comma = true;

      fprintf(fp, "n=%u", I->nest);
   }

   if ((info.immediates & AGX_IMMEDIATE_INVERT_COND) && I->invert_cond) {
      if (print_comma)
         fprintf(fp, ", ");
      else
         print_comma = true;

      fprintf(fp, "inv");
   }

   fprintf(fp, "\n");
}

void
agx_print_block(const agx_block *block, FILE *fp)
{
   fprintf(fp, "block%u {\n", block->index);

   agx_foreach_instr_in_block(block, ins)
      agx_print_instr(ins, fp);

   fprintf(fp, "}");

   if (block->successors[0]) {
      fprintf(fp, " -> ");

      agx_foreach_successor(block, succ)
         fprintf(fp, "block%u ", succ->index);
   }

   if (block->predecessors.size) {
      fprintf(fp, " from");

      agx_foreach_predecessor(block, pred)
         fprintf(fp, " block%u", (*pred)->index);
   }

   fprintf(fp, "\n\n");
}

void
agx_print_shader(const agx_context *ctx, FILE *fp)
{
   agx_foreach_block(ctx, block)
      agx_print_block(block, fp);
}
