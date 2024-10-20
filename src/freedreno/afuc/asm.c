/*
 * Copyright © 2017 Rob Clark <robdclark@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/macros.h"
#include "util/log.h"
#include "afuc.h"
#include "asm.h"
#include "parser.h"
#include "util.h"

struct encode_state {
	unsigned gen;
};

static afuc_opc
__instruction_case(struct encode_state *s, const struct afuc_instr *instr)
{
   switch (instr->opc) {
#define ALU(name) \
   case OPC_##name: \
      if (instr->has_immed) \
         return OPC_##name##I; \
      break;

   ALU(ADD)
   ALU(ADDHI)
   ALU(SUB)
   ALU(SUBHI)
   ALU(AND)
   ALU(OR)
   ALU(XOR)
   ALU(NOT)
   ALU(SHL)
   ALU(USHR)
   ALU(ISHR)
   ALU(ROT)
   ALU(MUL8)
   ALU(MIN)
   ALU(MAX)
   ALU(CMP)
   ALU(BIC)
#undef ALU

   default:
      break;
   }

   return instr->opc;
}

#include "encode.h"

int gpuver;

/* bit lame to hard-code max but fw sizes are small */
static struct afuc_instr instructions[0x4000];
static unsigned num_instructions;

static unsigned instr_offset;

static struct asm_label labels[0x512];
static unsigned num_labels;

static int outfd;

struct afuc_instr *
next_instr(afuc_opc opc)
{
   struct afuc_instr *ai = &instructions[num_instructions++];
   assert(num_instructions < ARRAY_SIZE(instructions));
   memset(ai, 0, sizeof(*ai));
   instr_offset++;
   ai->opc = opc;
   return ai;
}

static void usage(void);

void
parse_version(struct afuc_instr *instr)
{
   if (gpuver != 0)
      return;

   int ret = afuc_util_init(afuc_get_fwid(instr->literal), &gpuver, false);
   if (ret < 0) {
      usage();
      exit(1);
   }
}

void
decl_label(const char *str)
{
   struct asm_label *label = &labels[num_labels++];

   assert(num_labels < ARRAY_SIZE(labels));

   label->offset = instr_offset;
   label->label = str;
}

void
decl_jumptbl(void)
{
   struct afuc_instr *ai = &instructions[num_instructions++];
   assert(num_instructions < ARRAY_SIZE(instructions));
   ai->opc = OPC_JUMPTBL;
   instr_offset += 0x80;
}

void
align_instr(unsigned alignment)
{
   while (instr_offset % (alignment / 4) != 0) {
      next_instr(OPC_NOP);
   }
}

static int
resolve_label(const char *str)
{
   int i;

   for (i = 0; i < num_labels; i++) {
      struct asm_label *label = &labels[i];

      if (!strcmp(str, label->label)) {
         return label->offset;
      }
   }

   fprintf(stderr, "Undeclared label: %s\n", str);
   exit(2);
}

static void
emit_jumptable(int outfd)
{
   uint32_t jmptable[0x80] = {0};
   int i;

   for (i = 0; i < num_labels; i++) {
      struct asm_label *label = &labels[i];
      int id = afuc_pm4_id(label->label);

      /* if it doesn't match a known PM4 packet-id, try to match UNKN%d: */
      if (id < 0) {
         if (sscanf(label->label, "UNKN%d", &id) != 1) {
            /* if still not found, must not belong in jump-table: */
            continue;
         }
      }

      jmptable[id] = label->offset;
   }

   write(outfd, jmptable, sizeof(jmptable));
}

static void
emit_instructions(int outfd)
{
   int i;

   struct encode_state s = {
      .gen = gpuver,
   };

   /* Expand some meta opcodes, and resolve branch targets */
   for (i = 0; i < num_instructions; i++) {
      struct afuc_instr *ai = &instructions[i];

      switch (ai->opc) {
      case OPC_BREQ:
         ai->offset = resolve_label(ai->label) - i;
         if (ai->has_bit)
            ai->opc = OPC_BREQB;
         else
            ai->opc = OPC_BREQI;
         break;

      case OPC_BRNE:
         ai->offset = resolve_label(ai->label) - i;
         if (ai->has_bit)
            ai->opc = OPC_BRNEB;
         else
            ai->opc = OPC_BRNEI;
         break;

      case OPC_JUMP:
         ai->offset = resolve_label(ai->label) - i;
         ai->opc = OPC_BRNEB;
         ai->src1 = 0;
         ai->bit = 0;
         break;

      case OPC_CALL:
      case OPC_BL:
      case OPC_JUMPA:
         ai->literal = resolve_label(ai->label);
         break;

      case OPC_MOVI:
         if (ai->label)
            ai->immed = resolve_label(ai->label);
         break;

      default:
         break;
      }

      if (ai->opc == OPC_JUMPTBL) {
         emit_jumptable(outfd);
         continue;
      }

      if (ai->opc == OPC_RAW_LITERAL) {
         if (ai->label) {
            ai->literal = afuc_nop_literal(resolve_label(ai->label), gpuver);
         }
         write(outfd, &ai->literal, 4);
         continue;
      }

      uint32_t encoded = bitmask_to_uint64_t(encode__instruction(&s, NULL, ai));
      write(outfd, &encoded, 4);
   }
}

void next_section(void)
{
   /* Sections must be aligned to 32 bytes */
   align_instr(32);

   emit_instructions(outfd);

   num_instructions = 0;
   instr_offset = 0;
   num_labels = 0;
}


unsigned
parse_control_reg(const char *name)
{
   /* skip leading "@" */
   return afuc_control_reg(name + 1);
}

unsigned
parse_sqe_reg(const char *name)
{
   /* skip leading "%" */
   return afuc_sqe_reg(name + 1);
}

static void
usage(void)
{
   fprintf(stderr, "Usage:\n"
                   "\tasm filename.asm filename.fw\n");
   exit(2);
}

int
main(int argc, char **argv)
{
   FILE *in;
   char *file, *outfile;
   int ret;

   if (optind >= (argc + 1)) {
      fprintf(stderr, "no file specified!\n");
      usage();
   }

   file = argv[optind];
   outfile = argv[optind + 1];

   outfd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   if (outfd < 0) {
      fprintf(stderr, "could not open \"%s\"\n", outfile);
      usage();
   }

   in = fopen(file, "r");
   if (!in) {
      fprintf(stderr, "could not open \"%s\"\n", file);
      usage();
   }

   yyset_in(in);

   /* there is an extra 0x00000000 which kernel strips off.. we could
    * perhaps use it for versioning.
    */
   uint32_t zero = 0;
   write(outfd, &zero, 4);

   ret = yyparse();
   if (ret) {
      fprintf(stderr, "parse failed: %d\n", ret);
      return ret;
   }

   emit_instructions(outfd);

   close(outfd);

   return 0;
}
