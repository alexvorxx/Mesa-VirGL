/*
 * Copyright © 2014 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <stdarg.h>
#include <stdio.h>

#include "util/log.h"
#include "ir3.h"

#define PTRID(x) ((unsigned long)(x))

/* ansi escape sequences: */
#define RESET   "\x1b[0m"
#define RED     "\x1b[0;31m"
#define GREEN   "\x1b[0;32m"
#define BLUE    "\x1b[0;34m"
#define MAGENTA "\x1b[0;35m"

/* syntax coloring, mostly to make it easier to see different sorts of
 * srcs (immediate, constant, ssa, array, ...)
 */
#define SYN_REG(x)   RED x RESET
#define SYN_IMMED(x) GREEN x RESET
#define SYN_CONST(x) GREEN x RESET
#define SYN_SSA(x)   BLUE x RESET
#define SYN_ARRAY(x) MAGENTA x RESET

static const char *
type_name(type_t type)
{
   static const char *type_names[] = {
      /* clang-format off */
      [TYPE_F16]   = "f16",
      [TYPE_F32]   = "f32",
      [TYPE_U16]   = "u16",
      [TYPE_U32]   = "u32",
      [TYPE_S16]   = "s16",
      [TYPE_S32]   = "s32",
      [TYPE_U8]    = "u8",
      [TYPE_U8_32] = "u8_32",
      /* clang-format on */
   };
   return type_names[type];
}

static void
print_instr_name(struct log_stream *stream, struct ir3_instruction *instr,
                 bool flags)
{
   if (!instr)
      return;
#if MESA_DEBUG
   mesa_log_stream_printf(stream, "%04u:", instr->serialno);
#endif
   mesa_log_stream_printf(stream, "%04u:", instr->ip);
   if (instr->flags & IR3_INSTR_UNUSED) {
      mesa_log_stream_printf(stream, "XXX: ");
   } else {
      mesa_log_stream_printf(stream, "%03u: ", instr->use_count);
   }

   if (flags) {
      mesa_log_stream_printf(stream, "\t");
      if (instr->flags & IR3_INSTR_SY)
         mesa_log_stream_printf(stream, "(sy)");
      if (instr->flags & IR3_INSTR_SS)
         mesa_log_stream_printf(stream, "(ss)");
      if (instr->flags & IR3_INSTR_JP)
         mesa_log_stream_printf(stream, "(jp)");
      if (instr->repeat)
         mesa_log_stream_printf(stream, "(rpt%d)", instr->repeat);
      if (instr->nop)
         mesa_log_stream_printf(stream, "(nop%d)", instr->nop);
      if (instr->flags & IR3_INSTR_UL)
         mesa_log_stream_printf(stream, "(ul)");
      if (instr->flags & IR3_INSTR_SAT)
         mesa_log_stream_printf(stream, "(sat)");
   } else {
      mesa_log_stream_printf(stream, " ");
   }

   if (is_meta(instr)) {
      switch (instr->opc) {
      case OPC_META_INPUT:
         mesa_log_stream_printf(stream, "_meta:in");
         break;
      case OPC_META_SPLIT:
         mesa_log_stream_printf(stream, "_meta:split");
         break;
      case OPC_META_COLLECT:
         mesa_log_stream_printf(stream, "_meta:collect");
         break;
      case OPC_META_TEX_PREFETCH:
         mesa_log_stream_printf(stream, "_meta:tex_prefetch");
         break;
      case OPC_META_PARALLEL_COPY:
         mesa_log_stream_printf(stream, "_meta:parallel_copy");
         break;
      case OPC_META_PHI:
         mesa_log_stream_printf(stream, "_meta:phi");
         break;

      /* shouldn't hit here.. just for debugging: */
      default:
         mesa_log_stream_printf(stream, "_meta:%d", instr->opc);
         break;
      }
   } else if (opc_cat(instr->opc) == 1) {
      if (instr->opc == OPC_MOV) {
         if (instr->cat1.src_type == instr->cat1.dst_type)
            mesa_log_stream_printf(stream, "mov");
         else
            mesa_log_stream_printf(stream, "cov");
      } else {
         mesa_log_stream_printf(stream, "%s",
                                disasm_a3xx_instr_name(instr->opc));
      }

      if (instr->opc == OPC_SCAN_MACRO ||
          instr->opc == OPC_SCAN_CLUSTERS_MACRO) {
         switch (instr->cat1.reduce_op) {
         case REDUCE_OP_ADD_U:
            mesa_log_stream_printf(stream, ".add.u");
            break;
         case REDUCE_OP_ADD_F:
            mesa_log_stream_printf(stream, ".add.f");
            break;
         case REDUCE_OP_MUL_U:
            mesa_log_stream_printf(stream, ".mul.u");
            break;
         case REDUCE_OP_MUL_F:
            mesa_log_stream_printf(stream, ".mul.f");
            break;
         case REDUCE_OP_MIN_U:
            mesa_log_stream_printf(stream, ".min.u");
            break;
         case REDUCE_OP_MIN_S:
            mesa_log_stream_printf(stream, ".min.s");
            break;
         case REDUCE_OP_MIN_F:
            mesa_log_stream_printf(stream, ".min.f");
            break;
         case REDUCE_OP_MAX_U:
            mesa_log_stream_printf(stream, ".max.u");
            break;
         case REDUCE_OP_MAX_S:
            mesa_log_stream_printf(stream, ".max.s");
            break;
         case REDUCE_OP_MAX_F:
            mesa_log_stream_printf(stream, ".max.f");
            break;
         case REDUCE_OP_AND_B:
            mesa_log_stream_printf(stream, ".and.b");
            break;
         case REDUCE_OP_OR_B:
            mesa_log_stream_printf(stream, ".or.b");
            break;
         case REDUCE_OP_XOR_B:
            mesa_log_stream_printf(stream, ".xor.b");
            break;
         }
      }

      if (instr->opc != OPC_MOVMSK && instr->opc != OPC_SCAN_MACRO &&
          instr->opc != OPC_PUSH_CONSTS_LOAD_MACRO) {
         mesa_log_stream_printf(stream, ".%s%s",
                                type_name(instr->cat1.src_type),
                                type_name(instr->cat1.dst_type));
      }
   } else {
      mesa_log_stream_printf(stream, "%s", disasm_a3xx_instr_name(instr->opc));
      if (instr->flags & IR3_INSTR_3D)
         mesa_log_stream_printf(stream, ".3d");
      if (instr->flags & IR3_INSTR_A)
         mesa_log_stream_printf(stream, ".a");
      if (instr->flags & IR3_INSTR_O)
         mesa_log_stream_printf(stream, ".o");
      if (instr->flags & IR3_INSTR_P)
         mesa_log_stream_printf(stream, ".p");
      if (instr->flags & IR3_INSTR_S)
         mesa_log_stream_printf(stream, ".s");
      if (instr->flags & IR3_INSTR_V)
         mesa_log_stream_printf(stream, ".v");
      if (instr->flags & IR3_INSTR_A1EN)
         mesa_log_stream_printf(stream, ".a1en");
      if (instr->flags & IR3_INSTR_U)
         mesa_log_stream_printf(stream, ".u");
      if (instr->opc == OPC_LDC)
         mesa_log_stream_printf(stream, ".offset%d", instr->cat6.d);
      if (instr->opc == OPC_LDC_K)
         mesa_log_stream_printf(stream, ".%d", instr->cat6.iim_val);
      if (instr->flags & IR3_INSTR_B) {
         mesa_log_stream_printf(
            stream, ".base%d",
            is_tex(instr) ? instr->cat5.tex_base : instr->cat6.base);
      }
      if (instr->flags & IR3_INSTR_S2EN)
         mesa_log_stream_printf(stream, ".s2en");

      static const char *cond[0x7] = {
         "lt", "le", "gt", "ge", "eq", "ne",
      };

      switch (instr->opc) {
      case OPC_CMPS_F:
      case OPC_CMPS_U:
      case OPC_CMPS_S:
      case OPC_CMPV_F:
      case OPC_CMPV_U:
      case OPC_CMPV_S:
         mesa_log_stream_printf(stream, ".%s",
                                cond[instr->cat2.condition & 0x7]);
         break;
      case OPC_BRAC:
         mesa_log_stream_printf(stream, ".%u", instr->cat0.idx);
         break;
      case OPC_SHFL:
         switch (instr->cat6.shfl_mode) {
         case SHFL_XOR:
            mesa_log_stream_printf(stream, ".xor");
            break;
         case SHFL_UP:
            mesa_log_stream_printf(stream, ".up");
            break;
         case SHFL_DOWN:
            mesa_log_stream_printf(stream, ".down");
            break;
         case SHFL_RUP:
            mesa_log_stream_printf(stream, ".rup");
            break;
         case SHFL_RDOWN:
            mesa_log_stream_printf(stream, ".rdown");
            break;
         default:
            mesa_log_stream_printf(stream, ".%u", instr->cat6.shfl_mode);
            break;
         }

         mesa_log_stream_printf(stream, ".%s", type_name(instr->cat6.type));
         break;
      default:
         break;
      }
   }
}

static void
print_ssa_def_name(struct log_stream *stream, struct ir3_register *reg)
{
   mesa_log_stream_printf(stream, SYN_SSA("ssa_%u"), reg->instr->serialno);
   if (reg->name != 0)
      mesa_log_stream_printf(stream, ":%u", reg->name);
}

static void
print_ssa_name(struct log_stream *stream, struct ir3_register *reg, bool dst)
{
   if (!dst) {
      if (!reg->def)
         mesa_log_stream_printf(stream, SYN_SSA("undef"));
      else
         print_ssa_def_name(stream, reg->def);
   } else {
      print_ssa_def_name(stream, reg);
   }

   if (reg->num != INVALID_REG && !(reg->flags & IR3_REG_ARRAY)) {
      const char *prefix = "r";
      unsigned num = reg_num(reg);

      if (reg->flags & IR3_REG_PREDICATE) {
         prefix = "p";
         num = 0;
      }

      mesa_log_stream_printf(stream, "(" SYN_REG("%s%u.%c") ")", prefix, num,
                             "xyzw"[reg_comp(reg)]);
   }
}

static void
print_reg_name(struct log_stream *stream, struct ir3_instruction *instr,
               struct ir3_register *reg, bool dest)
{
   if ((reg->flags & (IR3_REG_FABS | IR3_REG_SABS)) &&
       (reg->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT)))
      mesa_log_stream_printf(stream, "(absneg)");
   else if (reg->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT))
      mesa_log_stream_printf(stream, "(neg)");
   else if (reg->flags & (IR3_REG_FABS | IR3_REG_SABS))
      mesa_log_stream_printf(stream, "(abs)");

   if (reg->flags & IR3_REG_FIRST_KILL)
      mesa_log_stream_printf(stream, "(kill)");
   if (reg->flags & IR3_REG_UNUSED)
      mesa_log_stream_printf(stream, "(unused)");

   if (reg->flags & IR3_REG_R)
      mesa_log_stream_printf(stream, "(r)");

   if (reg->flags & IR3_REG_EARLY_CLOBBER)
      mesa_log_stream_printf(stream, "(early_clobber)");

   /* Right now all instructions that use tied registers only have one
    * destination register, so we can just print (tied) as if it's a flag,
    * although it's more convenient for RA if it's a pointer.
    */
   if (reg->tied)
      mesa_log_stream_printf(stream, "(tied)");

   if (instr->opc == OPC_BR || instr->opc == OPC_BRAA ||
       instr->opc == OPC_BRAO) {
      bool inv = reg == instr->srcs[0] ? instr->cat0.inv1 : instr->cat0.inv2;
      if (inv)
         mesa_log_stream_printf(stream, "!");
   }

   if (reg->flags & IR3_REG_SHARED)
      mesa_log_stream_printf(stream, "s");
   if (reg->flags & IR3_REG_HALF)
      mesa_log_stream_printf(stream, "h");
   if (reg->flags & IR3_REG_PREDICATE)
      mesa_log_stream_printf(stream, "p");

   if (reg->flags & IR3_REG_IMMED) {
      mesa_log_stream_printf(stream, SYN_IMMED("imm[%f,%d,0x%x]"), reg->fim_val,
                             reg->iim_val, reg->iim_val);
   } else if (reg->flags & IR3_REG_ARRAY) {
      if (reg->flags & IR3_REG_SSA) {
         print_ssa_name(stream, reg, dest);
         mesa_log_stream_printf(stream, ":");
      }
      mesa_log_stream_printf(stream,
                             SYN_ARRAY("arr[id=%u, offset=%d, size=%u]"),
                             reg->array.id, reg->array.offset, reg->size);
      if (reg->array.base != INVALID_REG)
         mesa_log_stream_printf(stream, "(" SYN_REG("r%u.%c") ")",
                                reg->array.base >> 2,
                                "xyzw"[reg->array.base & 0x3]);
   } else if (reg->flags & IR3_REG_SSA) {
      print_ssa_name(stream, reg, dest);
   } else if (reg->flags & IR3_REG_RELATIV) {
      if (reg->flags & IR3_REG_CONST)
         mesa_log_stream_printf(stream, SYN_CONST("c<a0.x + %d>"),
                                reg->array.offset);
      else
         mesa_log_stream_printf(stream, SYN_REG("r<a0.x + %d>") " (%u)",
                                reg->array.offset, reg->size);
   } else {
      if (reg->flags & IR3_REG_CONST)
         mesa_log_stream_printf(stream, SYN_CONST("c%u.%c"), reg_num(reg),
                                "xyzw"[reg_comp(reg)]);
      else if (reg->flags & IR3_REG_PREDICATE)
         mesa_log_stream_printf(stream, SYN_REG("p0.%c"),
                                "xyzw"[reg_comp(reg)]);
      else
         mesa_log_stream_printf(stream, SYN_REG("r%u.%c"), reg_num(reg),
                                "xyzw"[reg_comp(reg)]);
   }

   if (reg->wrmask > 0x1)
      mesa_log_stream_printf(stream, " (wrmask=0x%x)", reg->wrmask);
}

static void
tab(struct log_stream *stream, int lvl)
{
   for (int i = 0; i < lvl; i++)
      mesa_log_stream_printf(stream, "\t");
}

static void
print_instr(struct log_stream *stream, struct ir3_instruction *instr, int lvl)
{
   tab(stream, lvl);

   print_instr_name(stream, instr, true);

   if (is_tex(instr)) {
      if (instr->opc == OPC_BRCST_ACTIVE)
         mesa_log_stream_printf(stream, ".w%d", instr->cat5.cluster_size);
      mesa_log_stream_printf(stream, " (%s)(", type_name(instr->cat5.type));
      for (unsigned i = 0; i < 4; i++)
         if (instr->dsts[0]->wrmask & (1 << i))
            mesa_log_stream_printf(stream, "%c", "xyzw"[i]);
      mesa_log_stream_printf(stream, ")");
   } else if ((instr->srcs_count > 0 || instr->dsts_count > 0)) {
      /* NOTE the b(ranch) instruction has a suffix, which is
       * handled below
       */
      mesa_log_stream_printf(stream, " ");
   }

   if (opc_cat(instr->opc) == 1) {
      switch (instr->cat1.round) {
      case ROUND_ZERO:
         break;
      case ROUND_EVEN:
         mesa_log_stream_printf(stream, "(even)");
         break;
      case ROUND_POS_INF:
         mesa_log_stream_printf(stream, "(pos_infinity)");
         break;
      case ROUND_NEG_INF:
         mesa_log_stream_printf(stream, "(neg_infinity)");
         break;
      }
   }

   bool first = true;
   foreach_dst (reg, instr) {
      if (reg->wrmask == 0)
         continue;
      if (!first)
         mesa_log_stream_printf(stream, ", ");
      print_reg_name(stream, instr, reg, true);
      first = false;
   }
   foreach_src_n (reg, n, instr) {
      if (!first)
         mesa_log_stream_printf(stream, ", ");
      print_reg_name(stream, instr, reg, false);
      if (instr->opc == OPC_END || instr->opc == OPC_CHMASK)
         mesa_log_stream_printf(stream, " (%u)", instr->end.outidxs[n]);
      first = false;
   }

   if (is_tex(instr) && !(instr->flags & IR3_INSTR_S2EN) &&
       !is_tex_shuffle(instr)) {
      if (!!(instr->flags & IR3_INSTR_B) && !!(instr->flags & IR3_INSTR_A1EN)) {
         mesa_log_stream_printf(stream, ", s#%d", instr->cat5.samp);
      } else {
         mesa_log_stream_printf(stream, ", s#%d, t#%d", instr->cat5.samp,
                                instr->cat5.tex);
      }
   }

   if (instr->opc == OPC_META_SPLIT) {
      mesa_log_stream_printf(stream, ", off=%d", instr->split.off);
   } else if (instr->opc == OPC_META_TEX_PREFETCH) {
      mesa_log_stream_printf(stream, ", tex=%d, samp=%d, input_offset=%d",
                             instr->prefetch.tex, instr->prefetch.samp,
                             instr->prefetch.input_offset);
   } else if (instr->opc == OPC_PUSH_CONSTS_LOAD_MACRO) {
      mesa_log_stream_printf(
         stream, " dst_offset=%d, src_offset = %d, src_size = %d",
         instr->push_consts.dst_base, instr->push_consts.src_base,
         instr->push_consts.src_size);
   } else if (instr->opc == OPC_SPILL_MACRO) {
      mesa_log_stream_printf(stream, " dst_offset=%d", instr->cat6.dst_offset);
   }

   if (is_flow(instr) && instr->cat0.target) {
      mesa_log_stream_printf(stream, " target=block%u",
                             block_id(instr->cat0.target));
   }

   if (instr->deps_count) {
      mesa_log_stream_printf(stream, ", false-deps:");
      unsigned n = 0;
      for (unsigned i = 0; i < instr->deps_count; i++) {
         if (!instr->deps[i])
            continue;
         if (n++ > 0)
            mesa_log_stream_printf(stream, ", ");
         mesa_log_stream_printf(stream, SYN_SSA("ssa_%u"),
                                instr->deps[i]->serialno);
      }
   }

   if (ir3_instr_is_rpt(instr)) {
      mesa_log_stream_printf(stream, ", rpt: ");

      if (ir3_instr_is_first_rpt(instr)) {
         mesa_log_stream_printf(stream, "first");
      } else {
         mesa_log_stream_printf(stream, "%u",
                                ir3_instr_prev_rpt(instr)->serialno);
      }
   }

   mesa_log_stream_printf(stream, "\n");
}

void
ir3_print_instr_stream(struct log_stream *stream, struct ir3_instruction *instr)
{
   print_instr(stream, instr, 0);
}

void
ir3_print_instr(struct ir3_instruction *instr)
{
   struct log_stream *stream = mesa_log_streami();
   print_instr(stream, instr, 0);
   mesa_log_stream_destroy(stream);
}

static void
print_block(struct ir3_block *block, int lvl)
{
   struct log_stream *stream = mesa_log_streami();

   tab(stream, lvl);
   mesa_log_stream_printf(stream, "%sblock%u {\n",
                          block->reconvergence_point ? "(jp)" : "",
                          block_id(block));

   if (block->predecessors_count > 0) {
      tab(stream, lvl + 1);
      mesa_log_stream_printf(stream, "pred: ");
      for (unsigned i = 0; i < block->predecessors_count; i++) {
         struct ir3_block *pred = block->predecessors[i];
         if (i != 0)
            mesa_log_stream_printf(stream, ", ");
         mesa_log_stream_printf(stream, "block%u", block_id(pred));
      }
      mesa_log_stream_printf(stream, "\n");
   }

   if (block->physical_predecessors_count > 0) {
      tab(stream, lvl + 1);
      mesa_log_stream_printf(stream, "physical pred: ");
      for (unsigned i = 0; i < block->physical_predecessors_count; i++) {
         struct ir3_block *pred = block->physical_predecessors[i];
         if (i != 0)
            mesa_log_stream_printf(stream, ", ");
         mesa_log_stream_printf(stream, "block%u", block_id(pred));
      }
      mesa_log_stream_printf(stream, "\n");
   }

   foreach_instr (instr, &block->instr_list) {
      print_instr(stream, instr, lvl + 1);
   }

   tab(stream, lvl + 1);
   mesa_log_stream_printf(stream, "/* keeps:\n");
   for (unsigned i = 0; i < block->keeps_count; i++) {
      print_instr(stream, block->keeps[i], lvl + 2);
   }
   tab(stream, lvl + 1);
   mesa_log_stream_printf(stream, " */\n");

   if (block->successors[0]) {
      tab(stream, lvl + 1);
      mesa_log_stream_printf(stream, "/* succs: block%u",
                             block_id(block->successors[0]));
      if (block->successors[1]) {
         mesa_log_stream_printf(stream, ", block%u",
                                block_id(block->successors[1]));

         mesa_log_stream_printf(stream, " (%s)",
                                block->divergent_condition ? "div" : "con");
      }
      mesa_log_stream_printf(stream, " */\n");
   }
   if (block->physical_successors_count > 0) {
      tab(stream, lvl + 1);
      mesa_log_stream_printf(stream, "/* physical succs: ");
      for (unsigned i = 0; i < block->physical_successors_count; i++) {
         mesa_log_stream_printf(stream, "block%u",
                                block_id(block->physical_successors[i]));
         if (i < block->physical_successors_count - 1)
            mesa_log_stream_printf(stream, ", ");
      }
      mesa_log_stream_printf(stream, " */\n");
   }
   tab(stream, lvl);
   mesa_log_stream_printf(stream, "}\n");
}

void
ir3_print(struct ir3 *ir)
{
   foreach_block (block, &ir->block_list)
      print_block(block, 0);
}
