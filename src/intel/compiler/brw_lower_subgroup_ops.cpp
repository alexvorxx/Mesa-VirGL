/*
 * Copyright 2024 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include "util/half_float.h"

#include "brw_fs.h"
#include "brw_fs_builder.h"

using namespace brw;

struct brw_reduction_info {
   brw_reg             identity;
   enum opcode         op;
   brw_conditional_mod cond_mod;
};

static brw_reduction_info
brw_get_reduction_info(brw_reduce_op red_op, brw_reg_type type)
{
   struct brw_reduction_info info;

   info.op = BRW_OPCODE_SEL;
   info.cond_mod = BRW_CONDITIONAL_NONE;

   switch (red_op) {
   case BRW_REDUCE_OP_ADD: info.op = BRW_OPCODE_ADD;           break;
   case BRW_REDUCE_OP_MUL: info.op = BRW_OPCODE_MUL;           break;
   case BRW_REDUCE_OP_AND: info.op = BRW_OPCODE_AND;           break;
   case BRW_REDUCE_OP_OR:  info.op = BRW_OPCODE_OR;            break;
   case BRW_REDUCE_OP_XOR: info.op = BRW_OPCODE_XOR;           break;
   case BRW_REDUCE_OP_MIN: info.cond_mod = BRW_CONDITIONAL_L;  break;
   case BRW_REDUCE_OP_MAX: info.cond_mod = BRW_CONDITIONAL_GE; break;
   default:
      unreachable("invalid reduce op");
   }

   switch (red_op) {
   case BRW_REDUCE_OP_ADD:
   case BRW_REDUCE_OP_XOR:
   case BRW_REDUCE_OP_OR:
      info.identity = retype(brw_imm_u64(0), type);
      return info;
   case BRW_REDUCE_OP_AND:
      info.identity = retype(brw_imm_u64(~0ull), type);
      return info;
   default:
      /* Continue below. */
      break;
   }

   brw_reg id;
   const unsigned size = brw_type_size_bytes(type);

   switch (red_op) {
   case BRW_REDUCE_OP_MUL: {
      if (brw_type_is_int(type)) {
         id = size < 4  ? brw_imm_uw(1) :
              size == 4 ? brw_imm_ud(1) :
                          brw_imm_u64(1);
      } else {
         assert(brw_type_is_float(type));
         id = size == 2 ? brw_imm_uw(_mesa_float_to_half(1.0)) :
              size == 4 ? brw_imm_f(1.0) :
                          brw_imm_df(1.0);
      }
      break;
   }

   case BRW_REDUCE_OP_MIN: {
      if (brw_type_is_uint(type)) {
         id = brw_imm_u64(~0ull);
      } else if (brw_type_is_sint(type)) {
         id = size == 1 ? brw_imm_w(INT8_MAX) :
              size == 2 ? brw_imm_w(INT16_MAX) :
              size == 4 ? brw_imm_d(INT32_MAX) :
                          brw_imm_q(INT64_MAX);
      } else {
         assert(brw_type_is_float(type));
         id = size == 2 ? brw_imm_uw(_mesa_float_to_half(INFINITY)) :
              size == 4 ? brw_imm_f(INFINITY) :
                          brw_imm_df(INFINITY);
      }
      break;
   }

   case BRW_REDUCE_OP_MAX: {
      if (brw_type_is_uint(type)) {
         id = brw_imm_u64(0);
      } else if (brw_type_is_sint(type)) {
         id = size == 1 ? brw_imm_w(INT8_MIN) :
              size == 2 ? brw_imm_w(INT16_MIN) :
              size == 4 ? brw_imm_d(INT32_MIN) :
                          brw_imm_q(INT64_MIN);
      } else {
         assert(brw_type_is_float(type));
         id = size == 2 ? brw_imm_uw(_mesa_float_to_half(-INFINITY)) :
              size == 4 ? brw_imm_f(-INFINITY) :
                          brw_imm_df(-INFINITY);
      }
      break;
   }

   default:
      unreachable("invalid reduce op");
   }

   /* For some cases above (e.g. all bits zeros, all bits ones, first bit one)
    * either the size or the signedness was ignored, so adjust the final type
    * now.
    *
    * B/UB types can't have immediates, so used W/UW above and here.
    */
   if      (type == BRW_TYPE_UB) type = BRW_TYPE_UW;
   else if (type == BRW_TYPE_B)  type = BRW_TYPE_W;

   info.identity = retype(id, type);

   return info;
}

static bool
brw_lower_reduce(fs_visitor &s, bblock_t *block, fs_inst *inst)
{
   const fs_builder bld(&s, block, inst);

   assert(inst->dst.type == inst->src[0].type);
   brw_reg dst = inst->dst;
   brw_reg src = inst->src[0];

   assert(inst->src[1].file == IMM);
   enum brw_reduce_op op = (enum brw_reduce_op)inst->src[1].ud;

   assert(inst->src[2].file == IMM);
   unsigned cluster_size = inst->src[2].ud;

   assert(cluster_size > 0);
   assert(cluster_size <= s.dispatch_width);

   struct brw_reduction_info info = brw_get_reduction_info(op, src.type);

   /* Set up a register for all of our scratching around and initialize it
    * to reduction operation's identity value.
    */
   brw_reg scan = bld.vgrf(src.type);
   bld.exec_all().emit(SHADER_OPCODE_SEL_EXEC, scan, src, info.identity);

   bld.emit_scan(info.op, scan, cluster_size, info.cond_mod);

   if (cluster_size * brw_type_size_bytes(src.type) >= REG_SIZE * 2) {
      /* In this case, CLUSTER_BROADCAST instruction isn't needed because
       * the distance between clusters is at least 2 GRFs.  In this case,
       * we don't need the weird striding of the CLUSTER_BROADCAST
       * instruction and can just do regular MOVs.
       */
      assert((cluster_size * brw_type_size_bytes(src.type)) % (REG_SIZE * 2) == 0);
      const unsigned groups =
         (s.dispatch_width * brw_type_size_bytes(src.type)) / (REG_SIZE * 2);
      const unsigned group_size = s.dispatch_width / groups;
      for (unsigned i = 0; i < groups; i++) {
         const unsigned cluster = (i * group_size) / cluster_size;
         const unsigned comp = cluster * cluster_size + (cluster_size - 1);
         bld.group(group_size, i).MOV(horiz_offset(dst, i * group_size),
                                      component(scan, comp));
      }
   } else {
      bld.emit(SHADER_OPCODE_CLUSTER_BROADCAST, dst, scan,
               brw_imm_ud(cluster_size - 1), brw_imm_ud(cluster_size));
   }
   inst->remove(block);
   return true;
}

bool
brw_fs_lower_subgroup_ops(fs_visitor &s)
{
   bool progress = false;

   foreach_block_and_inst_safe(block, fs_inst, inst, s.cfg) {
      switch (inst->opcode) {
      case SHADER_OPCODE_REDUCE:
         progress |= brw_lower_reduce(s, block, inst);
         break;

      default:
         /* Nothing to do. */
         break;
      }
   }

   if (progress)
      s.invalidate_analysis(DEPENDENCY_INSTRUCTIONS | DEPENDENCY_VARIABLES);

   return progress;
}
