/*
 * Copyright (C) 2022-2023 Collabora, Ltd.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "genxml/gen_macros.h"
#include "decode.h"

#if PAN_ARCH >= 10
/* Limit for Mali-G610. -1 because we're not including the active frame */
#define MAX_CALL_STACK_DEPTH (8 - 1)

struct queue_ctx {
   /* Size of CSHWIF register file in 32-bit registers */
   unsigned nr_regs;

   /* CSHWIF register file */
   uint32_t *regs;

   /* Current instruction pointer (CPU pointer for convenience) */
   uint64_t *ip;

   /* Current instruction end pointer */
   uint64_t *end;

   /* Call stack. Depth=0 means root */
   struct {
      /* Link register to return to */
      uint64_t *lr;

      /* End pointer, there is a return (or exit) after */
      uint64_t *end;
   } call_stack[MAX_CALL_STACK_DEPTH];
   uint8_t call_stack_depth;

   unsigned gpu_id;
};

static uint32_t
cs_get_u32(struct queue_ctx *qctx, uint8_t reg)
{
   assert(reg < qctx->nr_regs);
   return qctx->regs[reg];
}

static uint64_t
cs_get_u64(struct queue_ctx *qctx, uint8_t reg)
{
   return (((uint64_t)cs_get_u32(qctx, reg + 1)) << 32) | cs_get_u32(qctx, reg);
}

static void
pandecode_run_compute(struct pandecode_context *ctx, FILE *fp,
                      struct queue_ctx *qctx, struct MALI_CS_RUN_COMPUTE *I)
{
   const char *axes[4] = {"x_axis", "y_axis", "z_axis"};

   /* Print the instruction. Ignore the selects and the flags override
    * since we'll print them implicitly later.
    */
   fprintf(fp, "RUN_COMPUTE%s.%s #%u\n",
           I->progress_increment ? ".progress_inc" : "", axes[I->task_axis],
           I->task_increment);

   ctx->indent++;

   unsigned reg_srt = 0 + (I->srt_select * 2);
   unsigned reg_fau = 8 + (I->fau_select * 2);
   unsigned reg_spd = 16 + (I->spd_select * 2);
   unsigned reg_tsd = 24 + (I->tsd_select * 2);

   GENX(pandecode_resource_tables)(ctx, cs_get_u64(qctx, reg_srt), "Resources");

   mali_ptr fau = cs_get_u64(qctx, reg_fau);

   if (fau)
      GENX(pandecode_fau)(ctx, fau & BITFIELD64_MASK(48), fau >> 56, "FAU");

   GENX(pandecode_shader)
   (ctx, cs_get_u64(qctx, reg_spd), "Shader", qctx->gpu_id);

   DUMP_ADDR(ctx, LOCAL_STORAGE, cs_get_u64(qctx, reg_tsd),
             "Local Storage @%" PRIx64 ":\n", cs_get_u64(qctx, reg_tsd));

   pandecode_log(ctx, "Global attribute offset: %u\n", cs_get_u32(qctx, 32));
   DUMP_CL(ctx, COMPUTE_SIZE_WORKGROUP, &qctx->regs[33], "Workgroup size\n");
   pandecode_log(ctx, "Job offset X: %u\n", cs_get_u32(qctx, 34));
   pandecode_log(ctx, "Job offset Y: %u\n", cs_get_u32(qctx, 35));
   pandecode_log(ctx, "Job offset Z: %u\n", cs_get_u32(qctx, 36));
   pandecode_log(ctx, "Job size X: %u\n", cs_get_u32(qctx, 37));
   pandecode_log(ctx, "Job size Y: %u\n", cs_get_u32(qctx, 38));
   pandecode_log(ctx, "Job size Z: %u\n", cs_get_u32(qctx, 39));

   ctx->indent--;
}

static void
pandecode_run_compute_indirect(struct pandecode_context *ctx, FILE *fp,
                               struct queue_ctx *qctx,
                               struct MALI_CS_RUN_COMPUTE_INDIRECT *I)
{
   /* Print the instruction. Ignore the selects and the flags override
    * since we'll print them implicitly later.
    */
   fprintf(fp, "RUN_COMPUTE_INDIRECT%s #%u\n",
           I->progress_increment ? ".progress_inc" : "",
           I->workgroups_per_task);

   ctx->indent++;

   unsigned reg_srt = 0 + (I->srt_select * 2);
   unsigned reg_fau = 8 + (I->fau_select * 2);
   unsigned reg_spd = 16 + (I->spd_select * 2);
   unsigned reg_tsd = 24 + (I->tsd_select * 2);

   GENX(pandecode_resource_tables)(ctx, cs_get_u64(qctx, reg_srt), "Resources");

   mali_ptr fau = cs_get_u64(qctx, reg_fau);

   if (fau)
      GENX(pandecode_fau)(ctx, fau & BITFIELD64_MASK(48), fau >> 56, "FAU");

   GENX(pandecode_shader)
   (ctx, cs_get_u64(qctx, reg_spd), "Shader", qctx->gpu_id);

   DUMP_ADDR(ctx, LOCAL_STORAGE, cs_get_u64(qctx, reg_tsd),
             "Local Storage @%" PRIx64 ":\n", cs_get_u64(qctx, reg_tsd));

   pandecode_log(ctx, "Global attribute offset: %u\n", cs_get_u32(qctx, 32));
   DUMP_CL(ctx, COMPUTE_SIZE_WORKGROUP, &qctx->regs[33], "Workgroup size\n");
   pandecode_log(ctx, "Job offset X: %u\n", cs_get_u32(qctx, 34));
   pandecode_log(ctx, "Job offset Y: %u\n", cs_get_u32(qctx, 35));
   pandecode_log(ctx, "Job offset Z: %u\n", cs_get_u32(qctx, 36));
   pandecode_log(ctx, "Job size X: %u\n", cs_get_u32(qctx, 37));
   pandecode_log(ctx, "Job size Y: %u\n", cs_get_u32(qctx, 38));
   pandecode_log(ctx, "Job size Z: %u\n", cs_get_u32(qctx, 39));

   ctx->indent--;
}

static void
pandecode_run_tiling(struct pandecode_context *ctx, FILE *fp,
                     struct queue_ctx *qctx, struct MALI_CS_RUN_TILING *I)
{
   /* Print the instruction. Ignore the selects and the flags override
    * since we'll print them implicitly later.
    */
   fprintf(fp, "RUN_TILING%s", I->progress_increment ? ".progress_inc" : "");

   fprintf(fp, "\n");

   ctx->indent++;

   /* Merge flag overrides with the register flags */
   uint32_t tiler_flags_raw = cs_get_u64(qctx, 56);
   tiler_flags_raw |= I->flags_override;
   pan_unpack(&tiler_flags_raw, PRIMITIVE_FLAGS, tiler_flags);

   unsigned reg_srt = I->srt_select * 2;
   unsigned reg_fau = 8 + I->fau_select * 2;
   unsigned reg_spd = 16 + I->spd_select * 2;
   unsigned reg_tsd = 24 + I->tsd_select;

   mali_ptr srt = cs_get_u64(qctx, reg_srt);
   mali_ptr fau = cs_get_u64(qctx, reg_fau);
   mali_ptr spd = cs_get_u64(qctx, reg_spd);
   mali_ptr tsd = cs_get_u64(qctx, reg_tsd);

   if (srt)
      GENX(pandecode_resource_tables)(ctx, srt, "Fragment resources");

   if (fau) {
      uint64_t lo = fau & BITFIELD64_MASK(48);
      uint64_t hi = fau >> 56;

      GENX(pandecode_fau)(ctx, lo, hi, "Fragment FAU");
   }

   if (spd) {
      GENX(pandecode_shader)
      (ctx, spd, "Fragment shader", qctx->gpu_id);
   }

   DUMP_ADDR(ctx, LOCAL_STORAGE, tsd, "Fragment Local Storage @%" PRIx64 ":\n",
             tsd);

   pandecode_log(ctx, "Global attribute offset: %u\n", cs_get_u32(qctx, 32));
   pandecode_log(ctx, "Index count: %u\n", cs_get_u32(qctx, 33));
   pandecode_log(ctx, "Instance count: %u\n", cs_get_u32(qctx, 34));

   if (tiler_flags.index_type)
      pandecode_log(ctx, "Index offset: %u\n", cs_get_u32(qctx, 35));

   pandecode_log(ctx, "Vertex offset: %d\n", cs_get_u32(qctx, 36));
   pandecode_log(ctx, "Tiler DCD flags2: %X\n", cs_get_u32(qctx, 38));

   if (tiler_flags.index_type)
      pandecode_log(ctx, "Index array size: %u\n", cs_get_u32(qctx, 39));

   GENX(pandecode_tiler)(ctx, cs_get_u64(qctx, 40), qctx->gpu_id);

   DUMP_CL(ctx, SCISSOR, &qctx->regs[42], "Scissor\n");
   pandecode_log(ctx, "Low depth clamp: %f\n", uif(cs_get_u32(qctx, 44)));
   pandecode_log(ctx, "High depth clamp: %f\n", uif(cs_get_u32(qctx, 45)));
   pandecode_log(ctx, "Occlusion: %" PRIx64 "\n", cs_get_u64(qctx, 46));
   pandecode_log(ctx, "Vertex position array: %" PRIx64 "\n",
                 cs_get_u64(qctx, 48));

   mali_ptr blend = cs_get_u64(qctx, 50);
   GENX(pandecode_blend_descs)(ctx, blend & ~7, blend & 7, 0, qctx->gpu_id);

   DUMP_ADDR(ctx, DEPTH_STENCIL, cs_get_u64(qctx, 52), "Depth/stencil");

   if (tiler_flags.index_type)
      pandecode_log(ctx, "Indices: %" PRIx64 "\n", cs_get_u64(qctx, 54));

   DUMP_UNPACKED(ctx, PRIMITIVE_FLAGS, tiler_flags, "Primitive flags\n");
   DUMP_CL(ctx, DCD_FLAGS_0, &qctx->regs[57], "DCD Flags 0\n");
   DUMP_CL(ctx, DCD_FLAGS_1, &qctx->regs[58], "DCD Flags 1\n");
   pandecode_log(ctx, "Vertex bounds: %u\n", cs_get_u32(qctx, 59));
   DUMP_CL(ctx, PRIMITIVE_SIZE, &qctx->regs[60], "Primitive size\n");

   ctx->indent--;
}
static void
pandecode_run_idvs(struct pandecode_context *ctx, FILE *fp,
                   struct queue_ctx *qctx, struct MALI_CS_RUN_IDVS *I)
{
   /* Print the instruction. Ignore the selects and the flags override
    * since we'll print them implicitly later.
    */
   fprintf(fp, "RUN_IDVS%s%s", I->progress_increment ? ".progress_inc" : "",
           I->malloc_enable ? "" : ".no_malloc");

   if (I->draw_id_register_enable)
      fprintf(fp, " r%u", I->draw_id);

   fprintf(fp, "\n");

   ctx->indent++;

   /* Merge flag overrides with the register flags */
   uint32_t tiler_flags_raw = cs_get_u64(qctx, 56);
   tiler_flags_raw |= I->flags_override;
   pan_unpack(&tiler_flags_raw, PRIMITIVE_FLAGS, tiler_flags);

   unsigned reg_position_srt = 0;
   unsigned reg_position_fau = 8;
   unsigned reg_position_tsd = 24;

   unsigned reg_vary_srt = I->varying_srt_select ? 2 : 0;
   unsigned reg_vary_fau = I->varying_fau_select ? 10 : 8;
   unsigned reg_vary_tsd = I->varying_tsd_select ? 26 : 24;

   unsigned reg_frag_srt = I->fragment_srt_select ? 4 : 0;
   unsigned reg_frag_fau = 12;
   unsigned reg_frag_tsd = I->fragment_tsd_select ? 28 : 24;

   uint64_t position_srt = cs_get_u64(qctx, reg_position_srt);
   uint64_t vary_srt = cs_get_u64(qctx, reg_vary_srt);
   uint64_t frag_srt = cs_get_u64(qctx, reg_frag_srt);

   if (position_srt)
      GENX(pandecode_resource_tables)(ctx, position_srt, "Position resources");

   if (vary_srt)
      GENX(pandecode_resource_tables)(ctx, vary_srt, "Varying resources");

   if (frag_srt)
      GENX(pandecode_resource_tables)(ctx, frag_srt, "Fragment resources");

   mali_ptr position_fau = cs_get_u64(qctx, reg_position_fau);
   mali_ptr vary_fau = cs_get_u64(qctx, reg_vary_fau);
   mali_ptr fragment_fau = cs_get_u64(qctx, reg_frag_fau);

   if (position_fau) {
      uint64_t lo = position_fau & BITFIELD64_MASK(48);
      uint64_t hi = position_fau >> 56;

      GENX(pandecode_fau)(ctx, lo, hi, "Position FAU");
   }

   if (vary_fau) {
      uint64_t lo = vary_fau & BITFIELD64_MASK(48);
      uint64_t hi = vary_fau >> 56;

      GENX(pandecode_fau)(ctx, lo, hi, "Varying FAU");
   }

   if (fragment_fau) {
      uint64_t lo = fragment_fau & BITFIELD64_MASK(48);
      uint64_t hi = fragment_fau >> 56;

      GENX(pandecode_fau)(ctx, lo, hi, "Fragment FAU");
   }

   if (cs_get_u64(qctx, 16)) {
      GENX(pandecode_shader)
      (ctx, cs_get_u64(qctx, 16), "Position shader", qctx->gpu_id);
   }

   if (tiler_flags.secondary_shader) {
      uint64_t ptr = cs_get_u64(qctx, 18);

      GENX(pandecode_shader)(ctx, ptr, "Varying shader", qctx->gpu_id);
   }

   if (cs_get_u64(qctx, 20)) {
      GENX(pandecode_shader)
      (ctx, cs_get_u64(qctx, 20), "Fragment shader", qctx->gpu_id);
   }

   DUMP_ADDR(ctx, LOCAL_STORAGE, cs_get_u64(qctx, reg_position_tsd),
             "Position Local Storage @%" PRIx64 ":\n",
             cs_get_u64(qctx, reg_position_tsd));
   DUMP_ADDR(ctx, LOCAL_STORAGE, cs_get_u64(qctx, reg_vary_tsd),
             "Varying Local Storage @%" PRIx64 ":\n",
             cs_get_u64(qctx, reg_vary_tsd));
   DUMP_ADDR(ctx, LOCAL_STORAGE, cs_get_u64(qctx, reg_frag_tsd),
             "Fragment Local Storage @%" PRIx64 ":\n",
             cs_get_u64(qctx, reg_frag_tsd));

   pandecode_log(ctx, "Global attribute offset: %u\n", cs_get_u32(qctx, 32));
   pandecode_log(ctx, "Index count: %u\n", cs_get_u32(qctx, 33));
   pandecode_log(ctx, "Instance count: %u\n", cs_get_u32(qctx, 34));

   if (tiler_flags.index_type)
      pandecode_log(ctx, "Index offset: %u\n", cs_get_u32(qctx, 35));

   pandecode_log(ctx, "Vertex offset: %d\n", cs_get_u32(qctx, 36));
   pandecode_log(ctx, "Instance offset: %u\n", cs_get_u32(qctx, 37));
   pandecode_log(ctx, "Tiler DCD flags2: %X\n", cs_get_u32(qctx, 38));

   if (tiler_flags.index_type)
      pandecode_log(ctx, "Index array size: %u\n", cs_get_u32(qctx, 39));

   GENX(pandecode_tiler)(ctx, cs_get_u64(qctx, 40), qctx->gpu_id);

   DUMP_CL(ctx, SCISSOR, &qctx->regs[42], "Scissor\n");
   pandecode_log(ctx, "Low depth clamp: %f\n", uif(cs_get_u32(qctx, 44)));
   pandecode_log(ctx, "High depth clamp: %f\n", uif(cs_get_u32(qctx, 45)));
   pandecode_log(ctx, "Occlusion: %" PRIx64 "\n", cs_get_u64(qctx, 46));

   if (tiler_flags.secondary_shader)
      pandecode_log(ctx, "Varying allocation: %u\n", cs_get_u32(qctx, 48));

   mali_ptr blend = cs_get_u64(qctx, 50);
   GENX(pandecode_blend_descs)(ctx, blend & ~7, blend & 7, 0, qctx->gpu_id);

   DUMP_ADDR(ctx, DEPTH_STENCIL, cs_get_u64(qctx, 52), "Depth/stencil");

   if (tiler_flags.index_type)
      pandecode_log(ctx, "Indices: %" PRIx64 "\n", cs_get_u64(qctx, 54));

   DUMP_UNPACKED(ctx, PRIMITIVE_FLAGS, tiler_flags, "Primitive flags\n");
   DUMP_CL(ctx, DCD_FLAGS_0, &qctx->regs[57], "DCD Flags 0\n");
   DUMP_CL(ctx, DCD_FLAGS_1, &qctx->regs[58], "DCD Flags 1\n");
   DUMP_CL(ctx, PRIMITIVE_SIZE, &qctx->regs[60], "Primitive size\n");

   ctx->indent--;
}

static void
pandecode_run_fragment(struct pandecode_context *ctx, FILE *fp,
                       struct queue_ctx *qctx, struct MALI_CS_RUN_FRAGMENT *I)
{
   static const char *tile_order[] = {
      "zorder",  "horizontal",     "vertical",     "unknown",
      "unknown", "rev_horizontal", "rev_vertical", "unknown",
      "unknown", "unknown",        "unknown",      "unknown",
      "unknown", "unknown",        "unknown",      "unknown",
   };

   fprintf(fp, "RUN_FRAGMENT%s.tile_order=%s%s\n",
           I->enable_tem ? ".tile_enable_map_enable" : "",
           tile_order[I->tile_order],
           I->progress_increment ? ".progress_inc" : "");

   ctx->indent++;

   DUMP_CL(ctx, SCISSOR, &qctx->regs[42], "Scissor\n");

   /* TODO: Tile enable map */
   GENX(pandecode_fbd)
   (ctx, cs_get_u64(qctx, 40) & ~0x3full, true, qctx->gpu_id);

   ctx->indent--;
}

static void
pandecode_run_fullscreen(struct pandecode_context *ctx, FILE *fp,
                         struct queue_ctx *qctx,
                         struct MALI_CS_RUN_FULLSCREEN *I)
{
   fprintf(fp, "RUN_FULLSCREEN%s\n",
           I->progress_increment ? ".progress_inc" : "");

   ctx->indent++;

   /* Merge flag overrides with the register flags */
   uint32_t tiler_flags_raw = cs_get_u64(qctx, 56);
   tiler_flags_raw |= I->flags_override;
   pan_unpack(&tiler_flags_raw, PRIMITIVE_FLAGS, tiler_flags);
   DUMP_UNPACKED(ctx, PRIMITIVE_FLAGS, tiler_flags, "Primitive flags\n");

   GENX(pandecode_tiler)(ctx, cs_get_u64(qctx, 40), qctx->gpu_id);

   DUMP_CL(ctx, SCISSOR, &qctx->regs[42], "Scissor\n");

   pan_unpack(PANDECODE_PTR(ctx, cs_get_u64(qctx, I->dcd), void), DRAW, dcd);
   GENX(pandecode_dcd)(ctx, &dcd, 0, qctx->gpu_id);

   ctx->indent--;
}

static void
print_indirect(unsigned address, int16_t offset, FILE *fp)
{
   if (offset)
      fprintf(fp, "[d%u + %d]", address, offset);
   else
      fprintf(fp, "[d%u]", address);
}

static void
print_reg_tuple(unsigned base, uint16_t mask, FILE *fp)
{
   bool first_reg = true;

   u_foreach_bit(i, mask) {
      fprintf(fp, "%sr%u", first_reg ? "" : ":", base + i);
      first_reg = false;
   }

   if (mask == 0)
      fprintf(fp, "_");
}

static const char *conditions_str[] = {
   "le", "gt", "eq", "ne", "lt", "ge", "always",
};

static void
disassemble_ceu_instr(struct pandecode_context *ctx, uint64_t dword,
                      unsigned indent, bool verbose, FILE *fp,
                      struct queue_ctx *qctx)
{
   if (verbose) {
      fprintf(fp, " ");
      for (unsigned b = 0; b < 8; ++b)
         fprintf(fp, " %02x", (uint8_t)(dword >> (8 * b)));
   }

   for (int i = 0; i < indent; ++i)
      fprintf(fp, "  ");

   /* Unpack the base so we get the opcode */
   uint8_t *bytes = (uint8_t *)&dword;
   pan_unpack(bytes, CS_BASE, base);

   switch (base.opcode) {
   case MALI_CS_OPCODE_NOP: {
      pan_unpack(bytes, CS_NOP, I);

      if (I.ignored)
         fprintf(fp, "NOP // 0x%" PRIX64 "\n", I.ignored);
      else
         fprintf(fp, "NOP\n");
      break;
   }

   case MALI_CS_OPCODE_MOVE: {
      pan_unpack(bytes, CS_MOVE, I);

      fprintf(fp, "MOVE d%u, #0x%" PRIX64 "\n", I.destination, I.immediate);
      break;
   }

   case MALI_CS_OPCODE_MOVE32: {
      pan_unpack(bytes, CS_MOVE32, I);
      fprintf(fp, "MOVE32 r%u, #0x%X\n", I.destination, I.immediate);
      break;
   }

   case MALI_CS_OPCODE_WAIT: {
      bool first = true;
      pan_unpack(bytes, CS_WAIT, I);
      fprintf(fp, "WAIT%s ", I.progress_increment ? ".progress_inc" : "");

      u_foreach_bit(i, I.wait_mask) {
         fprintf(fp, "%s%u", first ? "" : ",", i);
         first = false;
      }

      fprintf(fp, "\n");
      break;
   }

   case MALI_CS_OPCODE_RUN_COMPUTE: {
      pan_unpack(bytes, CS_RUN_COMPUTE, I);
      pandecode_run_compute(ctx, fp, qctx, &I);
      break;
   }

   case MALI_CS_OPCODE_RUN_TILING: {
      pan_unpack(bytes, CS_RUN_TILING, I);
      pandecode_run_tiling(ctx, fp, qctx, &I);
      break;
   }

   case MALI_CS_OPCODE_RUN_IDVS: {
      pan_unpack(bytes, CS_RUN_IDVS, I);
      pandecode_run_idvs(ctx, fp, qctx, &I);
      break;
   }

   case MALI_CS_OPCODE_RUN_FRAGMENT: {
      pan_unpack(bytes, CS_RUN_FRAGMENT, I);
      pandecode_run_fragment(ctx, fp, qctx, &I);
      break;
   }

   case MALI_CS_OPCODE_RUN_FULLSCREEN: {
      pan_unpack(bytes, CS_RUN_FULLSCREEN, I);
      pandecode_run_fullscreen(ctx, fp, qctx, &I);
      break;
   }

   case MALI_CS_OPCODE_FINISH_TILING: {
      pan_unpack(bytes, CS_FINISH_TILING, I);
      fprintf(fp, "FINISH_TILING%s\n",
              I.progress_increment ? ".progress_inc" : "");
      break;
   }

   case MALI_CS_OPCODE_FINISH_FRAGMENT: {
      pan_unpack(bytes, CS_FINISH_FRAGMENT, I);
      fprintf(fp, "FINISH_FRAGMENT.%s, d%u, d%u, #%x, #%u\n",
              I.increment_fragment_completed ? ".frag_end" : "",
              I.last_heap_chunk, I.first_heap_chunk, I.wait_mask,
              I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_ADD_IMMEDIATE32: {
      pan_unpack(bytes, CS_ADD_IMMEDIATE32, I);

      fprintf(fp, "ADD_IMMEDIATE32 r%u, r%u, #%d\n", I.destination, I.source,
              I.immediate);
      break;
   }

   case MALI_CS_OPCODE_ADD_IMMEDIATE64: {
      pan_unpack(bytes, CS_ADD_IMMEDIATE64, I);

      fprintf(fp, "ADD_IMMEDIATE64 d%u, d%u, #%d\n", I.destination, I.source,
              I.immediate);
      break;
   }

   case MALI_CS_OPCODE_UMIN32: {
      pan_unpack(bytes, CS_UMIN32, I);

      fprintf(fp, "UMIN32 r%u, r%u, r%u\n", I.destination, I.source_1,
              I.source_2);
      break;
   }

   case MALI_CS_OPCODE_LOAD_MULTIPLE: {
      pan_unpack(bytes, CS_LOAD_MULTIPLE, I);

      fprintf(fp, "LOAD_MULTIPLE ");
      print_reg_tuple(I.base_register, I.mask, fp);
      fprintf(fp, ", ");
      print_indirect(I.address, I.offset, fp);
      fprintf(fp, "\n");
      break;
   }

   case MALI_CS_OPCODE_STORE_MULTIPLE: {
      pan_unpack(bytes, CS_STORE_MULTIPLE, I);

      fprintf(fp, "STORE_MULTIPLE ");
      print_indirect(I.address, I.offset, fp);
      fprintf(fp, ", ");
      print_reg_tuple(I.base_register, I.mask, fp);
      fprintf(fp, "\n");
      break;
   }

   case MALI_CS_OPCODE_BRANCH: {
      pan_unpack(bytes, CS_BRANCH, I);
      fprintf(fp, "BRANCH.%s r%u, #%d\n", conditions_str[I.condition], I.value,
              I.offset);
      break;
   }

   case MALI_CS_OPCODE_SET_SB_ENTRY: {
      pan_unpack(bytes, CS_SET_SB_ENTRY, I);
      fprintf(fp, "SET_SB_ENTRY #%u, #%u\n", I.endpoint_entry, I.other_entry);
      break;
   }

   case MALI_CS_OPCODE_PROGRESS_WAIT: {
      pan_unpack(bytes, CS_PROGRESS_WAIT, I);
      fprintf(fp, "PROGRESS_WAIT d%u, #%u\n", I.source, I.queue);
      break;
   }

   case MALI_CS_OPCODE_SET_EXCEPTION_HANDLER: {
      pan_unpack(bytes, CS_SET_EXCEPTION_HANDLER, I);
      fprintf(fp, "SET_EXCEPTION_HANDLER d%u, r%u\n", I.address, I.length);
      break;
   }

   case MALI_CS_OPCODE_CALL: {
      pan_unpack(bytes, CS_CALL, I);
      fprintf(fp, "CALL d%u, r%u\n", I.address, I.length);
      break;
   }

   case MALI_CS_OPCODE_JUMP: {
      pan_unpack(bytes, CS_JUMP, I);
      fprintf(fp, "JUMP d%u, r%u\n", I.address, I.length);
      break;
   }

   case MALI_CS_OPCODE_REQ_RESOURCE: {
      pan_unpack(bytes, CS_REQ_RESOURCE, I);

      fprintf(fp, "REQ_RESOURCE");
      if (I.compute)
         fprintf(fp, ".compute");
      if (I.fragment)
         fprintf(fp, ".fragment");
      if (I.tiler)
         fprintf(fp, ".tiler");
      if (I.idvs)
         fprintf(fp, ".idvs");
      fprintf(fp, "\n");
      break;
   }

   case MALI_CS_OPCODE_FLUSH_CACHE2: {
      pan_unpack(bytes, CS_FLUSH_CACHE2, I);
      static const char *mode[] = {
         "nop",
         "clean",
         "INVALID",
         "clean_invalidate",
      };

      fprintf(fp, "FLUSH_CACHE2.%s_l2.%s_lsc%s r%u, #%x, #%u\n",
              mode[I.l2_flush_mode], mode[I.lsc_flush_mode],
              I.other_invalidate ? ".invalidate_other" : ".nop_other",
              I.latest_flush_id, I.wait_mask, I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_SYNC_ADD32: {
      pan_unpack(bytes, CS_SYNC_ADD32, I);
      fprintf(fp, "SYNC_ADD32%s%s [d%u], r%u, #%x, #%u\n",
              I.error_propagate ? ".error_propagate" : "",
              I.scope == MALI_CS_SYNC_SCOPE_CSG ? ".csg" : ".system", I.address,
              I.data, I.wait_mask, I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_SYNC_SET32: {
      pan_unpack(bytes, CS_SYNC_SET32, I);
      fprintf(fp, "SYNC_SET32.%s%s [d%u], r%u, #%x, #%u\n",
              I.error_propagate ? ".error_propagate" : "",
              I.scope == MALI_CS_SYNC_SCOPE_CSG ? ".csg" : ".system", I.address,
              I.data, I.wait_mask, I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_SYNC_WAIT32: {
      pan_unpack(bytes, CS_SYNC_WAIT32, I);
      fprintf(fp, "SYNC_WAIT32%s%s d%u, r%u\n", conditions_str[I.condition],
              I.error_reject ? ".reject" : ".inherit", I.address, I.data);
      break;
   }

   case MALI_CS_OPCODE_STORE_STATE: {
      static const char *states_str[] = {
         "SYSTEM_TIMESTAMP",
         "CYCLE_COUNT",
         "DISJOINT_COUNT",
         "ERROR_STATE",
      };

      pan_unpack(bytes, CS_STORE_STATE, I);
      fprintf(fp, "STORE_STATE.%s d%u, #%i, #%x, #%u\n",
              I.state >= ARRAY_SIZE(states_str) ? "UNKNOWN_STATE"
                                                : states_str[I.state],
              I.address, I.offset, I.wait_mask, I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_PROT_REGION: {
      pan_unpack(bytes, CS_PROT_REGION, I);
      fprintf(fp, "PROT_REGION #%u\n", I.size);
      break;
   }

   case MALI_CS_OPCODE_PROGRESS_STORE: {
      pan_unpack(bytes, CS_PROGRESS_STORE, I);
      fprintf(fp, "PROGRESS_STORE d%u\n", I.source);
      break;
   }

   case MALI_CS_OPCODE_PROGRESS_LOAD: {
      pan_unpack(bytes, CS_PROGRESS_LOAD, I);
      fprintf(fp, "PROGRESS_LOAD d%u\n", I.destination);
      break;
   }

   case MALI_CS_OPCODE_RUN_COMPUTE_INDIRECT: {
      pan_unpack(bytes, CS_RUN_COMPUTE_INDIRECT, I);
      pandecode_run_compute_indirect(ctx, fp, qctx, &I);
      break;
   }

   case MALI_CS_OPCODE_ERROR_BARRIER: {
      pan_unpack(bytes, CS_ERROR_BARRIER, I);
      fprintf(fp, "ERROR_BARRIER");
      break;
   }

   case MALI_CS_OPCODE_HEAP_SET: {
      pan_unpack(bytes, CS_HEAP_SET, I);
      fprintf(fp, "HEAP_SET d%u\n", I.address);
      break;
   }

   case MALI_CS_OPCODE_HEAP_OPERATION: {
      pan_unpack(bytes, CS_HEAP_OPERATION, I);
      const char *counter_names[] = {"vt_start", "vt_end", NULL, "frag_end"};
      fprintf(fp, "HEAP_OPERATION.%s #%x, #%d\n", counter_names[I.operation],
              I.wait_mask, I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_TRACE_POINT: {
      pan_unpack(bytes, CS_TRACE_POINT, I);
      fprintf(fp, "TRACE_POINT r%d:r%d, #%x, #%u\n", I.base_register,
              I.base_register + I.register_count - 1, I.wait_mask,
              I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_SYNC_ADD64: {
      pan_unpack(bytes, CS_SYNC_ADD64, I);
      fprintf(fp, "SYNC_ADD64%s%s [d%u], d%u, #%x, #%u\n",
              I.error_propagate ? ".error_propagate" : "",
              I.scope == MALI_CS_SYNC_SCOPE_CSG ? ".csg" : ".system", I.address,
              I.data, I.wait_mask, I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_SYNC_SET64: {
      pan_unpack(bytes, CS_SYNC_SET64, I);
      fprintf(fp, "SYNC_SET64.%s%s [d%u], d%u, #%x, #%u\n",
              I.error_propagate ? ".error_propagate" : "",
              I.scope == MALI_CS_SYNC_SCOPE_CSG ? ".csg" : ".system", I.address,
              I.data, I.wait_mask, I.signal_slot);
      break;
   }

   case MALI_CS_OPCODE_SYNC_WAIT64: {
      pan_unpack(bytes, CS_SYNC_WAIT64, I);

      fprintf(fp, "SYNC_WAIT64%s%s d%u, d%u\n", conditions_str[I.condition],
              I.error_reject ? ".reject" : ".inherit", I.address, I.data);
      break;
   }

   default: {
      fprintf(fp, "UNKNOWN_%u 0x%" PRIX64 "\n", base.opcode, base.data);
      break;
   }
   }
}

static bool
interpret_ceu_jump(struct pandecode_context *ctx, struct queue_ctx *qctx,
                   uint64_t reg_address, uint32_t reg_length)
{
   uint32_t address_lo = qctx->regs[reg_address];
   uint32_t address_hi = qctx->regs[reg_address + 1];
   uint32_t length = qctx->regs[reg_length];

   if (length % 8) {
      fprintf(stderr, "CS call alignment error\n");
      return false;
   }

   /* Map the entire subqueue now */
   uint64_t address = ((uint64_t)address_hi << 32) | address_lo;
   uint64_t *cs = pandecode_fetch_gpu_mem(ctx, address, length);

   qctx->ip = cs;
   qctx->end = cs + (length / 8);

   /* Skip the usual IP update */
   return true;
}

static bool
eval_cond(struct queue_ctx *qctx, enum mali_cs_condition cond, uint32_t reg)
{
   int32_t val = qctx->regs[reg];

   switch (cond) {
   case MALI_CS_CONDITION_LEQUAL:
      return val <= 0;
   case MALI_CS_CONDITION_EQUAL:
      return val == 0;
   case MALI_CS_CONDITION_LESS:
      return val < 0;
   case MALI_CS_CONDITION_GREATER:
      return val > 0;
   case MALI_CS_CONDITION_NEQUAL:
      return val != 0;
   case MALI_CS_CONDITION_GEQUAL:
      return val >= 0;
   case MALI_CS_CONDITION_ALWAYS:
      return true;
   default:
      assert(!"Invalid condition");
      return false;
   }
}

static void
interpret_ceu_branch(struct pandecode_context *ctx, struct queue_ctx *qctx,
                     int16_t offset, enum mali_cs_condition cond,
                     uint32_t reg)
{
   if (eval_cond(qctx, cond, reg))
      qctx->ip += offset;
}

/*
 * Interpret a single instruction of the CS, updating the register file,
 * instruction pointer, and call stack. Memory access and GPU controls are
 * ignored for now.
 *
 * Returns true if execution should continue.
 */
static bool
interpret_ceu_instr(struct pandecode_context *ctx, struct queue_ctx *qctx)
{
   /* Unpack the base so we get the opcode */
   uint8_t *bytes = (uint8_t *)qctx->ip;
   pan_unpack(bytes, CS_BASE, base);

   assert(qctx->ip < qctx->end);

   switch (base.opcode) {
   case MALI_CS_OPCODE_MOVE: {
      pan_unpack(bytes, CS_MOVE, I);

      qctx->regs[I.destination + 0] = (uint32_t)I.immediate;
      qctx->regs[I.destination + 1] = (uint32_t)(I.immediate >> 32);
      break;
   }

   case MALI_CS_OPCODE_MOVE32: {
      pan_unpack(bytes, CS_MOVE32, I);

      qctx->regs[I.destination] = I.immediate;
      break;
   }

   case MALI_CS_OPCODE_LOAD_MULTIPLE: {
      pan_unpack(bytes, CS_LOAD_MULTIPLE, I);
      mali_ptr addr =
         ((uint64_t)qctx->regs[I.address + 1] << 32) | qctx->regs[I.address];
      addr += I.offset;

      uint32_t *src =
         pandecode_fetch_gpu_mem(ctx, addr, util_last_bit(I.mask) * 4);

      for (uint32_t i = 0; i < 16; i++) {
         if (I.mask & BITFIELD_BIT(i))
            qctx->regs[I.base_register + i] = src[i];
      }
      break;
   }

   case MALI_CS_OPCODE_ADD_IMMEDIATE32: {
      pan_unpack(bytes, CS_ADD_IMMEDIATE32, I);

      qctx->regs[I.destination] = qctx->regs[I.source] + I.immediate;
      break;
   }

   case MALI_CS_OPCODE_ADD_IMMEDIATE64: {
      pan_unpack(bytes, CS_ADD_IMMEDIATE64, I);

      int64_t value =
         (qctx->regs[I.source] | ((int64_t)qctx->regs[I.source + 1] << 32)) +
         I.immediate;

      qctx->regs[I.destination] = value;
      qctx->regs[I.destination + 1] = value >> 32;
      break;
   }

   case MALI_CS_OPCODE_CALL: {
      pan_unpack(bytes, CS_CALL, I);

      if (qctx->call_stack_depth == MAX_CALL_STACK_DEPTH) {
         fprintf(stderr, "CS call stack overflow\n");
         return false;
      }

      assert(qctx->call_stack_depth < MAX_CALL_STACK_DEPTH);

      qctx->ip++;

      /* Note: tail calls are not optimized in the hardware. */
      assert(qctx->ip <= qctx->end);

      unsigned depth = qctx->call_stack_depth++;

      qctx->call_stack[depth].lr = qctx->ip;
      qctx->call_stack[depth].end = qctx->end;

      return interpret_ceu_jump(ctx, qctx, I.address, I.length);
   }

   case MALI_CS_OPCODE_JUMP: {
      pan_unpack(bytes, CS_JUMP, I);

      if (qctx->call_stack_depth == 0) {
         fprintf(stderr, "Cannot jump from the entrypoint\n");
         return false;
      }

      return interpret_ceu_jump(ctx, qctx, I.address, I.length);
   }

   case MALI_CS_OPCODE_BRANCH: {
      pan_unpack(bytes, CS_BRANCH, I);

      interpret_ceu_branch(ctx, qctx, I.offset, I.condition, I.value);
      break;
   }

   default:
      break;
   }

   /* Update IP first to point to the next instruction, so call doesn't
    * require special handling (even for tail calls).
    */
   qctx->ip++;

   while (qctx->ip == qctx->end) {
      /* Graceful termination */
      if (qctx->call_stack_depth == 0)
         return false;

      /* Pop off the call stack */
      unsigned old_depth = --qctx->call_stack_depth;

      qctx->ip = qctx->call_stack[old_depth].lr;
      qctx->end = qctx->call_stack[old_depth].end;
   }

   return true;
}

void
GENX(pandecode_cs)(struct pandecode_context *ctx, mali_ptr queue, uint32_t size,
                   unsigned gpu_id, uint32_t *regs)
{
   pandecode_dump_file_open(ctx);

   uint64_t *cs = pandecode_fetch_gpu_mem(ctx, queue, size);

   /* Mali-G610 has 96 registers. Other devices not yet supported, we can make
    * this configurable later when we encounter new Malis.
    */
   struct queue_ctx qctx = {
      .nr_regs = 96,
      .regs = regs,
      .ip = cs,
      .end = cs + (size / 8),
      .gpu_id = gpu_id,

      /* If this is a kernel mode queue, we don't see the root ring buffer and
       * we must adjust the initial call stack depth accordingly.
       */
      .call_stack_depth = ctx->usermode_queue ? 0 : 1,
   };

   if (size) {
      do {
         disassemble_ceu_instr(ctx, *(qctx.ip), 1 + qctx.call_stack_depth, true,
                               ctx->dump_stream, &qctx);
      } while (interpret_ceu_instr(ctx, &qctx));
   }

   fflush(ctx->dump_stream);
   pandecode_map_read_write(ctx);
}
#endif
