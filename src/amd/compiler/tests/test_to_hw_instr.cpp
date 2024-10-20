/*
 * Copyright © 2020 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */
#include "helpers.h"

using namespace aco;

BEGIN_TEST(to_hw_instr.swap_subdword)
   PhysReg v0_lo{256};
   PhysReg v0_hi{256};
   PhysReg v0_b1{256};
   PhysReg v0_b3{256};
   PhysReg v1_lo{257};
   PhysReg v1_hi{257};
   PhysReg v1_b1{257};
   PhysReg v1_b3{257};
   PhysReg v128_lo{256 + 128};
   PhysReg v128_hi{256 + 128};
   PhysReg v129_lo{256 + 129};
   PhysReg v129_hi{256 + 129};
   v0_hi.reg_b += 2;
   v1_hi.reg_b += 2;
   v0_b1.reg_b += 1;
   v1_b1.reg_b += 1;
   v0_b3.reg_b += 3;
   v1_b3.reg_b += 3;
   v128_hi.reg_b += 2;
   v129_hi.reg_b += 2;

   for (amd_gfx_level lvl : {GFX8, GFX9, GFX11}) {
      if (!setup_cs(NULL, lvl))
         continue;

      //~gfx(8|9|11)>> p_unit_test 0
      //~gfx8! v1: %0:v[0] = v_alignbyte_b32 %0:v[0][0:16], %0:v[0][16:32], 2
      //~gfx(9|11)! v1: %0:v[0] = v_pack_b32_f16 hi(%0:v[0][16:32]), %0:v[0][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::zero());
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand(v0_hi, v2b), Operand(v0_lo, v2b));

      //~gfx(8|9|11)! p_unit_test 1
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx(9|11)! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //~gfx[89]! v2b: %0:v[1][16:32] = v_mov_b32 %0:v[0][16:32] dst_sel:uword1 dst_preserve src0_sel:uword1
      //~gfx11! v2b: %0:v[1][16:32] = v_mov_b16 hi(%0:v[0][16:32]) opsel_hi
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(1u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1), Definition(v1_lo, v2b),
                 Operand(v1_lo, v1), Operand(v0_lo, v2b));

      //~gfx(8|9|11)! p_unit_test 2
      //~gfx[89]! v2b: %0:v[0][16:32] = v_mov_b32 %0:v[1][16:32] dst_sel:uword1 dst_preserve src0_sel:uword1
      //~gfx[89]! v2b: %0:v[1][16:32] = v_mov_b32 %0:v[0][0:16] dst_sel:uword1 dst_preserve src0_sel:uword0
      //~gfx[89]! v2b: %0:v[1][0:16] = v_xor_b32 %0:v[1][0:16], %0:v[0][0:16] dst_sel:uword0 dst_preserve src0_sel:uword0 src1_sel:uword0
      //~gfx[89]! v2b: %0:v[0][0:16] = v_xor_b32 %0:v[1][0:16], %0:v[0][0:16] dst_sel:uword0 dst_preserve src0_sel:uword0 src1_sel:uword0
      //~gfx[89]! v2b: %0:v[1][0:16] = v_xor_b32 %0:v[1][0:16], %0:v[0][0:16] dst_sel:uword0 dst_preserve src0_sel:uword0 src1_sel:uword0
      //~gfx11! v2b: %0:v[0][16:32] = v_mov_b16 hi(%0:v[1][16:32]) opsel_hi
      //~gfx11! v2b: %0:v[1][16:32] = v_mov_b16 %0:v[0][0:16] opsel_hi
      //~gfx11! v2b: %0:v[0][0:16],  v2b: %0:v[1][0:16] = v_swap_b16 %0:v[1][0:16], %0:v[0][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(2u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1), Definition(v1_lo, v2b),
                 Definition(v1_hi, v2b), Operand(v1_lo, v1), Operand(v0_lo, v2b),
                 Operand(v0_lo, v2b));

      //~gfx(8|9|11)! p_unit_test 3
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx(9|11)! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //~gfx[89]! v2b: %0:v[1][0:16] = v_mov_b32 %0:v[0][0:16] dst_sel:uword0 dst_preserve src0_sel:uword0
      //~gfx[89]! v1b: %0:v[1][16:24] = v_mov_b32 %0:v[0][16:24] dst_sel:ubyte2 dst_preserve src0_sel:ubyte2
      //~gfx11! v2b: %0:v[1][0:16] = v_mov_b16 %0:v[0][0:16]
      //~gfx11! v1: %0:v[1] = v_perm_b32 %0:v[1], %0:v[0], 0x7020504
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(3u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1), Definition(v1_b3, v1b),
                 Operand(v1_lo, v1), Operand(v0_b3, v1b));

      //~gfx(8|9|11)! p_unit_test 4
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx(9|11)! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //~gfx[89]! v1b: %0:v[1][8:16] = v_mov_b32 %0:v[0][8:16] dst_sel:ubyte1 dst_preserve src0_sel:ubyte1
      //~gfx[89]! v2b: %0:v[1][16:32] = v_mov_b32 %0:v[0][16:32] dst_sel:uword1 dst_preserve src0_sel:uword1
      //~gfx11! v1: %0:v[1] = v_perm_b32 %0:v[1], %0:v[0], 0x7060104
      //~gfx11! v2b: %0:v[1][16:32] = v_mov_b16 hi(%0:v[0][16:32]) opsel_hi
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(4u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1), Definition(v1_lo, v1b),
                 Operand(v1_lo, v1), Operand(v0_lo, v1b));

      //~gfx(8|9|11)! p_unit_test 5
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx(9|11)! v1: %0:v[1],  v1: %0:v[0] = v_swap_b32 %0:v[0], %0:v[1]
      //~gfx[89]! v1b: %0:v[0][8:16] = v_mov_b32 %0:v[1][8:16] dst_sel:ubyte1 dst_preserve src0_sel:ubyte1
      //~gfx[89]! v1b: %0:v[0][24:32] = v_mov_b32 %0:v[1][24:32] dst_sel:ubyte3 dst_preserve src0_sel:ubyte3
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0x7060104
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0x3060504
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(5u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1b), Definition(v0_hi, v1b),
                 Definition(v1_lo, v1), Operand(v1_lo, v1b), Operand(v1_hi, v1b),
                 Operand(v0_lo, v1));

      //~gfx(8|9|11)! p_unit_test 6
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx(9|11)! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(6u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Definition(v1_lo, v1), Operand(v1_lo, v2b), Operand(v1_hi, v2b),
                 Operand(v0_lo, v1));

      //~gfx(8|9|11)! p_unit_test 7
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[0], %0:v[1]
      //~gfx(9|11)! v1: %0:v[1],  v1: %0:v[0] = v_swap_b32 %0:v[0], %0:v[1]
      //~gfx(8|9|11)! v1: %0:v[0] = v_alignbyte_b32 %0:v[0][0:16], %0:v[0][16:32], 2
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(7u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Definition(v1_lo, v1), Operand(v1_hi, v2b), Operand(v1_lo, v2b),
                 Operand(v0_lo, v1));

      //~gfx(8|9|11)! p_unit_test 8
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx(9|11)! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //~gfx[89]! v1b: %0:v[1][24:32] = v_xor_b32 %0:v[1][24:32], %0:v[0][24:32] dst_sel:ubyte3 dst_preserve src0_sel:ubyte3 src1_sel:ubyte3
      //~gfx[89]! v1b: %0:v[0][24:32] = v_xor_b32 %0:v[1][24:32], %0:v[0][24:32] dst_sel:ubyte3 dst_preserve src0_sel:ubyte3 src1_sel:ubyte3
      //~gfx[89]! v1b: %0:v[1][24:32] = v_xor_b32 %0:v[1][24:32], %0:v[0][24:32] dst_sel:ubyte3 dst_preserve src0_sel:ubyte3 src1_sel:ubyte3
      //~gfx11! v2b: %0:v[0][0:16],  v2b: %0:v[1][16:32] = v_swap_b16 hi(%0:v[1][16:32]), %0:v[0][0:16]
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], 0, 0x5060704
      //~gfx11! v2b: %0:v[0][0:16],  v2b: %0:v[1][16:32] = v_swap_b16 hi(%0:v[1][16:32]), %0:v[0][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(8u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v3b), Definition(v1_lo, v3b),
                 Operand(v1_lo, v3b), Operand(v0_lo, v3b));

      //~gfx(8|9|11)! p_unit_test 9
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[0] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx8! v1: %0:v[1] = v_xor_b32 %0:v[1], %0:v[0]
      //~gfx(9|11)! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
      //~gfx[89]! v1b: %0:v[1][24:32] = v_mov_b32 %0:v[0][24:32] dst_sel:ubyte3 dst_preserve src0_sel:ubyte3
      //~gfx11! v1: %0:v[1] = v_perm_b32 %0:v[1], %0:v[0], 0x3060504
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(9u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v3b), Definition(v1_lo, v3b),
                 Definition(v0_b3, v1b), Operand(v1_lo, v3b), Operand(v0_lo, v3b),
                 Operand(v1_b3, v1b));

      //~gfx(8|9|11)! p_unit_test 10
      //~gfx[89]! v1b: %0:v[1][8:16] = v_xor_b32 %0:v[1][8:16], %0:v[0][8:16] dst_sel:ubyte1 dst_preserve src0_sel:ubyte1 src1_sel:ubyte1
      //~gfx[89]! v1b: %0:v[0][8:16] = v_xor_b32 %0:v[1][8:16], %0:v[0][8:16] dst_sel:ubyte1 dst_preserve src0_sel:ubyte1 src1_sel:ubyte1
      //~gfx[89]! v1b: %0:v[1][8:16] = v_xor_b32 %0:v[1][8:16], %0:v[0][8:16] dst_sel:ubyte1 dst_preserve src0_sel:ubyte1 src1_sel:ubyte1
      //~gfx11! v2b: %0:v[0][16:32],  v2b: %0:v[1][0:16] = v_swap_b16 %0:v[1][0:16], %0:v[0][16:32] opsel_hi
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], 0, 0x5060704
      //~gfx11! v2b: %0:v[0][16:32],  v2b: %0:v[1][0:16] = v_swap_b16 %0:v[1][0:16], %0:v[0][16:32] opsel_hi
      //~gfx[89]! v1b: %0:v[1][16:24] = v_xor_b32 %0:v[1][16:24], %0:v[0][16:24] dst_sel:ubyte2 dst_preserve src0_sel:ubyte2 src1_sel:ubyte2
      //~gfx[89]! v1b: %0:v[0][16:24] = v_xor_b32 %0:v[1][16:24], %0:v[0][16:24] dst_sel:ubyte2 dst_preserve src0_sel:ubyte2 src1_sel:ubyte2
      //~gfx[89]! v1b: %0:v[1][16:24] = v_xor_b32 %0:v[1][16:24], %0:v[0][16:24] dst_sel:ubyte2 dst_preserve src0_sel:ubyte2 src1_sel:ubyte2
      //~gfx11! v2b: %0:v[0][0:16],  v2b: %0:v[1][16:32] = v_swap_b16 hi(%0:v[1][16:32]), %0:v[0][0:16]
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], 0, 0x7040506
      //~gfx11! v2b: %0:v[0][0:16],  v2b: %0:v[1][16:32] = v_swap_b16 hi(%0:v[1][16:32]), %0:v[0][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(10u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_b1, v2b), Definition(v1_b1, v2b),
                 Operand(v1_b1, v2b), Operand(v0_b1, v2b));

      //~gfx(8|9|11)! p_unit_test 11
      //~gfx[89]! v2b: %0:v[1][0:16] = v_mov_b32 %0:v[0][16:32] dst_sel:uword0 dst_preserve src0_sel:uword1
      //~gfx11! v2b: %0:v[1][0:16] = v_mov_b16 hi(%0:v[0][16:32])
      //~gfx(8|9|11)! v1: %0:v[0] = v_mov_b32 42
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(11u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1), Definition(v1_lo, v2b),
                 Operand::c32(42u), Operand(v0_hi, v2b));

      //~gfx(8|9|11)! p_unit_test 12
      //~gfx[89]! v1b: %0:v[0][24:32] = v_xor_b32 %0:v[0][24:32], %0:v[0][8:16] dst_sel:ubyte3 dst_preserve src0_sel:ubyte3 src1_sel:ubyte1
      //~gfx[89]! v1b: %0:v[0][8:16] = v_xor_b32 %0:v[0][24:32], %0:v[0][8:16] dst_sel:ubyte1 dst_preserve src0_sel:ubyte3 src1_sel:ubyte1
      //~gfx[89]! v1b: %0:v[0][24:32] = v_xor_b32 %0:v[0][24:32], %0:v[0][8:16] dst_sel:ubyte3 dst_preserve src0_sel:ubyte3 src1_sel:ubyte1
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], 0, 0x5060704
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(12u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_b1, v1b), Definition(v0_b3, v1b),
                 Operand(v0_b3, v1b), Operand(v0_b1, v1b));

      //~gfx(8|9|11)! p_unit_test 13
      //~gfx[89]! v2b: %0:v[129][16:32] = v_xor_b32 %0:v[129][16:32], %0:v[128][0:16] dst_sel:uword1 dst_preserve src0_sel:uword1 src1_sel:uword0
      //~gfx[89]! v2b: %0:v[128][0:16] = v_xor_b32 %0:v[129][16:32], %0:v[128][0:16] dst_sel:uword0 dst_preserve src0_sel:uword1 src1_sel:uword0
      //~gfx[89]! v2b: %0:v[129][16:32] = v_xor_b32 %0:v[129][16:32], %0:v[128][0:16] dst_sel:uword1 dst_preserve src0_sel:uword1 src1_sel:uword0
      //~gfx11! v2b: %0:v[128][0:16] = v_xor_b16 hi(%0:v[129][16:32]), %0:v[128][0:16]
      //~gfx11! v2b: %0:v[129][16:32] = v_xor_b16 hi(%0:v[129][16:32]), %0:v[128][0:16] opsel_hi
      //~gfx11! v2b: %0:v[128][0:16] = v_xor_b16 hi(%0:v[129][16:32]), %0:v[128][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(13u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v128_lo, v2b), Definition(v129_hi, v2b),
                 Operand(v129_hi, v2b), Operand(v128_lo, v2b));

      //~gfx(8|9|11)! p_unit_test 14
      //~gfx[89]! v2b: %0:v[129][0:16] = v_xor_b32 %0:v[129][0:16], %0:v[128][16:32] dst_sel:uword0 dst_preserve src0_sel:uword0 src1_sel:uword1
      //~gfx[89]! v2b: %0:v[128][16:32] = v_xor_b32 %0:v[129][0:16], %0:v[128][16:32] dst_sel:uword1 dst_preserve src0_sel:uword0 src1_sel:uword1
      //~gfx[89]! v2b: %0:v[129][0:16] = v_xor_b32 %0:v[129][0:16], %0:v[128][16:32] dst_sel:uword0 dst_preserve src0_sel:uword0 src1_sel:uword1
      //~gfx11! v2b: %0:v[128][16:32] = v_xor_b16 %0:v[129][0:16], hi(%0:v[128][16:32]) opsel_hi
      //~gfx11! v2b: %0:v[129][0:16] = v_xor_b16 %0:v[129][0:16], hi(%0:v[128][16:32])
      //~gfx11! v2b: %0:v[128][16:32] = v_xor_b16 %0:v[129][0:16], hi(%0:v[128][16:32]) opsel_hi
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(14u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v128_hi, v2b), Definition(v129_lo, v2b),
                 Operand(v129_lo, v2b), Operand(v128_hi, v2b));

      //~gfx11! s_nop
      //~gfx11! s_sendmsg sendmsg(dealloc_vgprs)
      //~gfx(8|9|11)! s_endpgm

      finish_to_hw_instr_test();
   }
END_TEST

BEGIN_TEST(to_hw_instr.subdword_constant)
   PhysReg v0_lo{256};
   PhysReg v0_hi{256};
   PhysReg v0_b1{256};
   PhysReg v1_lo{257};
   PhysReg v1_hi{257};
   v0_hi.reg_b += 2;
   v0_b1.reg_b += 1;
   v1_hi.reg_b += 2;

   for (amd_gfx_level lvl : {GFX9, GFX10, GFX11}) {
      if (!setup_cs(NULL, lvl))
         continue;

      /* 16-bit pack */
      //>> p_unit_test 0
      //! v1: %_:v[0] = v_pack_b32_f16 0.5, hi(%_:v[1][16:32])
      bld.pseudo(aco_opcode::p_unit_test, Operand::zero());
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::c16(0x3800), Operand(v1_hi, v2b));

      //! p_unit_test 1
      //~gfx9! v2b: %0:v[0][16:32] = v_and_b32 0xffff0000, %0:v[1][16:32]
      //~gfx9! v1: %0:v[0] = v_or_b32 0x4205, %0:v[0]
      //~gfx(10|11)! v1: %_:v[0] = v_pack_b32_f16 0x4205, hi(%_:v[1][16:32])
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(1u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::c16(0x4205), Operand(v1_hi, v2b));

      //! p_unit_test 2
      //~gfx9! v2b: %0:v[0][16:32] = v_lshlrev_b32 16, %0:v[0][0:16]
      //~gfx9! v1: %_:v[0] = v_or_b32 0x4205, %_:v[0]
      //~gfx(10|11)! v1: %0:v[0] = v_pack_b32_f16 0x4205, %0:v[0][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(2u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::c16(0x4205), Operand(v0_lo, v2b));

      //! p_unit_test 3
      //! v1: %_:v[0] = v_mov_b32 0x3c003800
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(3u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::c16(0x3800), Operand::c16(0x3c00));

      //! p_unit_test 4
      //! v1: %_:v[0] = v_mov_b32 0x43064205
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(4u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::c16(0x4205), Operand::c16(0x4306));

      //! p_unit_test 5
      //! v1: %_:v[0] = v_mov_b32 0x38004205
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(5u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::c16(0x4205), Operand::c16(0x3800));

      /* 16-bit copy */
      //! p_unit_test 6
      //~gfx(9|10)! v2b: %_:v[0][0:16] = v_add_f16 0.5, 0 dst_sel:uword0 dst_preserve src0_sel:uword0 src1_sel:dword
      //~gfx11! v2b: %0:v[0][0:16] = v_add_f16 0.5, 0
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(6u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Operand::c16(0x3800));

      //! p_unit_test 7
      //~gfx9! v1: %_:v[0] = v_and_b32 0xffff0000, %_:v[0]
      //~gfx9! v1: %_:v[0] = v_or_b32 0x4205, %_:v[0]
      //~gfx10! v2b: %_:v[0][0:16] = v_add_u16_e64 0x4205, 0
      //~gfx11! v2b: %0:v[0][0:16] = v_mov_b16 0x4205
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(7u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Operand::c16(0x4205));

      //! p_unit_test 8
      //~gfx9! v1: %_:v[0] = v_and_b32 0xffff, %_:v[0]
      //~gfx9! v1: %_:v[0] = v_or_b32 0x42050000, %_:v[0]
      //~gfx10! v2b: %_:v[0][16:32] = v_add_u16_e64 0x4205, 0 opsel_hi
      //~gfx11! v2b: %0:v[0][16:32] = v_mov_b16 0x4205 opsel_hi
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(8u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_hi, v2b), Operand::c16(0x4205));

      //! p_unit_test 9
      //~gfx(9|10)! v1b: %_:v[0][8:16] = v_mov_b32 0 dst_sel:ubyte1 dst_preserve src0_sel:dword
      //~gfx(9|10)! v1b: %_:v[0][16:24] = v_mov_b32 56 dst_sel:ubyte2 dst_preserve src0_sel:dword
      //~gfx11! v1b: %_:v[0][8:16] = v_cvt_pk_u8_f32 0, 1, %_:v[0]
      //~gfx11! v1b: %_:v[0][16:24] = v_cvt_pk_u8_f32 0x42600000, 2, %_:v[0]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(9u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_b1, v2b), Operand::c16(0x3800));

      //! p_unit_test 10
      //~gfx(9|10)! v1b: %_:v[0][8:16] = v_mov_b32 5 dst_sel:ubyte1 dst_preserve src0_sel:dword
      //~gfx(9|10)! v1b: %_:v[0][16:24] = v_mul_u32_u24 2, 33 dst_sel:ubyte2 dst_preserve src0_sel:dword src1_sel:dword
      //~gfx11! v1b: %_:v[0][8:16] = v_cvt_pk_u8_f32 0x40a00000, 1, %_:v[0]
      //~gfx11! v1b: %_:v[0][16:24] = v_cvt_pk_u8_f32 0x42840000, 2, %_:v[0]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(10u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_b1, v2b), Operand::c16(0x4205));

      /* 8-bit copy */
      //! p_unit_test 11
      //~gfx(9|10)! v1b: %_:v[0][0:8] = v_mul_u32_u24 2, 33 dst_sel:ubyte0 dst_preserve src0_sel:dword src1_sel:dword
      //~gfx11! v1b: %_:v[0][0:8] = v_cvt_pk_u8_f32 0x42840000, 0, %_:v[0]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(11u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1b), Operand::c8(0x42));

      /* 32-bit and 8-bit copy */
      //! p_unit_test 12
      //! v1: %_:v[0] = v_mov_b32 0
      //~gfx(9|10)! v1b: %_:v[1][0:8] = v_mov_b32 0 dst_sel:ubyte0 dst_preserve src0_sel:dword
      //~gfx11! v1b: %_:v[1][0:8] = v_cvt_pk_u8_f32 0, 0, %_:v[1]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(12u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1), Definition(v1_lo, v1b),
                 Operand::zero(), Operand::zero(1));

      //! p_unit_test 13
      //~gfx9! v1: %_:v[0] = v_and_b32 0xffff0000, %_:v[0]
      //~gfx9! v1: %_:v[0] = v_or_b32 0xff, %_:v[0]
      //~gfx10! v2b: %_:v[0][0:16] = v_add_u16_e64 0xff, 0
      //~gfx11! v2b: %0:v[0][0:16] = v_mov_b16 0xff
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(13u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Operand::c16(0x00ff));

      //! p_unit_test 14
      //~gfx9! v1: %_:v[0] = v_and_b32 0xffff, %_:v[0]
      //~gfx9! v1: %_:v[0] = v_or_b32 0xff000000, %_:v[0]
      //~gfx10! v2b: %_:v[0][16:32] = v_add_u16_e64 0xff00, 0 opsel_hi
      //~gfx11! v2b: %0:v[0][16:32] = v_mov_b16 0xffffff00 opsel_hi
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(14u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_hi, v2b), Operand::c16(0xff00));

      //! p_unit_test 15
      //~gfx(9|10)! v2b: %_:v[0][0:16] = v_mov_b32 0 dst_sel:uword0 dst_preserve src0_sel:dword
      //~gfx11! v2b: %0:v[0][0:16] = v_mov_b16 0
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(15u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Operand::zero(2));

      //! p_unit_test 16
      //~gfx(9|10)! v1b: %_:v[0][0:8] = v_mov_b32 -1 dst_sel:ubyte0 dst_preserve src0_sel:dword
      //~gfx11! v1b: %_:v[0][0:8] = v_cvt_pk_u8_f32 0x437f0000, 0, %_:v[0]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(16u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1b), Operand::c8(0xff));

      //! p_unit_test 17
      //~gfx(9|10)! v1b: %_:v[0][0:8] = v_mov_b32 0 dst_sel:ubyte0 dst_preserve src0_sel:dword
      //~gfx11! v1b: %_:v[0][0:8] = v_cvt_pk_u8_f32 0, 0, %_:v[0]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(17u));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1b), Operand::zero(1));

      //~gfx11! s_nop
      //~gfx11! s_sendmsg sendmsg(dealloc_vgprs)
      //! s_endpgm

      finish_to_hw_instr_test();
   }
END_TEST

BEGIN_TEST(to_hw_instr.self_intersecting_swap)
   if (!setup_cs(NULL, GFX9))
      return;

   PhysReg reg_v1{257};
   PhysReg reg_v2{258};
   PhysReg reg_v3{259};
   PhysReg reg_v7{263};

   //>> p_unit_test 0
   //! v1: %0:v[1],  v1: %0:v[2] = v_swap_b32 %0:v[2], %0:v[1]
   //! v1: %0:v[2],  v1: %0:v[3] = v_swap_b32 %0:v[3], %0:v[2]
   //! v1: %0:v[3],  v1: %0:v[7] = v_swap_b32 %0:v[7], %0:v[3]
   //! s_endpgm
   bld.pseudo(aco_opcode::p_unit_test, Operand::zero());
   // v[1:2] = v[2:3]
   // v3 = v7
   // v7 = v1
   bld.pseudo(aco_opcode::p_parallelcopy, Definition(reg_v1, v2), Definition(reg_v3, v1),
              Definition(reg_v7, v1), Operand(reg_v2, v2), Operand(reg_v7, v1),
              Operand(reg_v1, v1));

   finish_to_hw_instr_test();
END_TEST

BEGIN_TEST(to_hw_instr.extract)
   PhysReg s0_lo{0};
   PhysReg s1_lo{1};
   PhysReg v0_lo{256};
   PhysReg v1_lo{257};

   for (amd_gfx_level lvl : {GFX7, GFX8, GFX9, GFX11}) {
      for (unsigned is_signed = 0; is_signed <= 1; is_signed++) {
         if (!setup_cs(NULL, lvl, CHIP_UNKNOWN, is_signed ? "_signed" : "_unsigned"))
            continue;

#define EXT(idx, size)                                                                             \
   bld.pseudo(aco_opcode::p_extract, Definition(v0_lo, v1), Operand(v1_lo, v1), Operand::c32(idx), \
              Operand::c32(size), Operand::c32(is_signed));

         //; funcs['v_bfe'] = lambda _: 'v_bfe_i32' if variant.endswith('_signed') else 'v_bfe_u32'
         //; funcs['v_shr'] = lambda _: 'v_ashrrev_i32' if variant.endswith('_signed') else 'v_lshrrev_b32'
         //; funcs['s_bfe'] = lambda _: 's_bfe_i32' if variant.endswith('_signed') else 's_bfe_u32'
         //; funcs['s_shr'] = lambda _: 's_ashr_i32' if variant.endswith('_signed') else 's_lshr_b32'
         //; funcs['byte'] = lambda n: '%cbyte%s' % ('s' if variant.endswith('_signed') else 'u', n)

         //>> p_unit_test 0
         bld.pseudo(aco_opcode::p_unit_test, Operand::zero());
         //! v1: %_:v[0] = @v_bfe %_:v[1], 0, 8
         EXT(0, 8)
         //! v1: %_:v[0] = @v_bfe %_:v[1], 8, 8
         EXT(1, 8)
         //! v1: %_:v[0] = @v_bfe %_:v[1], 16, 8
         EXT(2, 8)
         //! v1: %_:v[0] = @v_shr 24, %_:v[1]
         EXT(3, 8)
         //~gfx(7|8|9)_.*! v1: %_:v[0] = @v_bfe %_:v[1], 0, 16
         //~gfx11_unsigned! v1: %_:v[0] = v_cvt_u32_u16 %_:v[1]
         //~gfx11_signed! v1: %_:v[0] = v_cvt_i32_i16 %_:v[1]
         EXT(0, 16)
         //! v1: %_:v[0] = @v_shr 16, %_:v[1]
         EXT(1, 16)

#undef EXT

#define EXT(idx, size)                                                                             \
   bld.pseudo(aco_opcode::p_extract, Definition(s0_lo, s1), Definition(scc, s1),                   \
              Operand(s1_lo, s1), Operand::c32(idx), Operand::c32(size), Operand::c32(is_signed));

         //>> p_unit_test 2
         bld.pseudo(aco_opcode::p_unit_test, Operand::c32(2u));
         //~gfx.*_unsigned! s1: %_:s[0],  s1: %_:scc = @s_bfe %_:s[1], 0x80000
         //~gfx.*_signed! s1: %_:s[0] = s_sext_i32_i8 %_:s[1]
         EXT(0, 8)
         //! s1: %_:s[0],  s1: %_:scc = @s_bfe %_:s[1], 0x80008
         EXT(1, 8)
         //! s1: %_:s[0],  s1: %_:scc = @s_bfe %_:s[1], 0x80010
         EXT(2, 8)
         //! s1: %_:s[0],  s1: %_:scc = @s_shr %_:s[1], 24
         EXT(3, 8)
         //~gfx(7|8)_unsigned! s1: %_:s[0],  s1: %_:scc = @s_bfe %_:s[1], 0x100000
         //~gfx(9|11)_unsigned! s1: %_:s[0] = s_pack_ll_b32_b16 %_:s[1], 0
         //~gfx.*_signed! s1: %_:s[0] = s_sext_i32_i16 %_:s[1]
         EXT(0, 16)
         //~gfx(7,8)_unsigned! s1: %_:s[0],  s1: %_:scc = @s_shr %_:s[1], 16
         //~gfx(9|11)_unsigned! s1: %_:s[0] = s_pack_hh_b32_b16 %_:s[1], 0
         //~gfx.*_signed! s1: %_:s[0],  s1: %_:scc = @s_shr %_:s[1], 16
         EXT(1, 16)

#undef EXT

#define EXT(idx, src_b)                                                                            \
   bld.pseudo(aco_opcode::p_extract, Definition(v0_lo, v2b), Operand(v1_lo.advance(src_b), v2b),   \
              Operand::c32(idx), Operand::c32(8u), Operand::c32(is_signed));

         //>> p_unit_test 4
         bld.pseudo(aco_opcode::p_unit_test, Operand::c32(4u));
         //~gfx(8|9).*! v2b: %_:v[0][0:16] = v_mov_b32 %_:v[1][0:16] dst_sel:uword0 dst_preserve src0_sel:@byte(0)
         //~gfx11_unsigned! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060c00
         //~gfx11_signed! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060000
         //~gfx11_signed! v1: %_:v[0] = v_perm_b32 %_:v[0], 0, 0x7060a04
         if (lvl != GFX7)
            EXT(0, 0)
         //~gfx(8|9).*! v2b: %_:v[0][0:16] = v_mov_b32 %_:v[1][16:32] dst_sel:uword0 dst_preserve src0_sel:@byte(2)
         //~gfx11_unsigned! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060c02
         //~gfx11_signed! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060202
         //~gfx11_signed! v1: %_:v[0] = v_perm_b32 %_:v[0], 0, 0x7060a04
         if (lvl != GFX7)
            EXT(0, 2)
         //~gfx(8|9).*! v2b: %_:v[0][0:16] = v_mov_b32 %_:v[1][0:16] dst_sel:uword0 dst_preserve src0_sel:@byte(1)
         //~gfx11_unsigned! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060c01
         //~gfx11_signed! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060801
         if (lvl != GFX7)
            EXT(1, 0)
         //~gfx(8|9).*! v2b: %_:v[0][0:16] = v_mov_b32 %_:v[1][16:32] dst_sel:uword0 dst_preserve src0_sel:@byte(3)
         //~gfx11_unsigned! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060c03
         //~gfx11_signed! v1: %_:v[0] = v_perm_b32 %_:v[0], %_:v[1], 0x7060903
         if (lvl != GFX7)
            EXT(1, 2)

#undef EXT

         finish_to_hw_instr_test();

         //~gfx11_.*! s_nop
         //~gfx11_.*! s_sendmsg sendmsg(dealloc_vgprs)
         //! s_endpgm
      }
   }
END_TEST

BEGIN_TEST(to_hw_instr.insert)
   PhysReg s0_lo{0};
   PhysReg s1_lo{1};
   PhysReg v0_lo{256};
   PhysReg v1_lo{257};

   for (amd_gfx_level lvl : {GFX7, GFX8, GFX9, GFX11}) {
      if (!setup_cs(NULL, lvl))
         continue;

#define INS(idx, size)                                                                             \
   bld.pseudo(aco_opcode::p_insert, Definition(v0_lo, v1), Operand(v1_lo, v1), Operand::c32(idx),  \
              Operand::c32(size));

      //>> p_unit_test 0
      bld.pseudo(aco_opcode::p_unit_test, Operand::zero());
      //! v1: %_:v[0] = v_bfe_u32 %_:v[1], 0, 8
      INS(0, 8)
      //~gfx7! v1: %0:v[0] = v_bfe_u32 %0:v[1], 0, 8
      //~gfx7! v1: %0:v[0] = v_lshlrev_b32 8, %0:v[0]
      //~gfx(8|9)! v1: %0:v[0] = v_mov_b32 %0:v[1] dst_sel:ubyte1 src0_sel:dword
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0xc0c000c
      INS(1, 8)
      //~gfx7! v1: %0:v[0] = v_bfe_u32 %0:v[1], 0, 8
      //~gfx7! v1: %0:v[0] = v_lshlrev_b32 16, %0:v[0]
      //~gfx(8|9)! v1: %0:v[0] = v_mov_b32 %0:v[1] dst_sel:ubyte2 src0_sel:dword
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0xc000c0c
      INS(2, 8)
      //! v1: %0:v[0] = v_lshlrev_b32 24, %0:v[1]
      INS(3, 8)
      //! v1: %0:v[0] = v_bfe_u32 %0:v[1], 0, 16
      INS(0, 16)
      //! v1: %0:v[0] = v_lshlrev_b32 16, %0:v[1]
      INS(1, 16)

#undef INS

#define INS(idx, size)                                                                             \
   bld.pseudo(aco_opcode::p_insert, Definition(s0_lo, s1), Definition(scc, s1),                    \
              Operand(s1_lo, s1), Operand::c32(idx), Operand::c32(size));

      //>> p_unit_test 1
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(1u));
      //! s1: %_:s[0],  s1: %_:scc = s_bfe_u32 %_:s[1], 0x80000
      INS(0, 8)
      //! s1: %_:s[0],  s1: %_:scc = s_bfe_u32 %_:s[1], 0x80000
      //! s1: %_:s[0],  s1: %_:scc = s_lshl_b32 %_:s[0], 8
      INS(1, 8)
      //! s1: %_:s[0],  s1: %_:scc = s_bfe_u32 %_:s[1], 0x80000
      //! s1: %_:s[0],  s1: %_:scc = s_lshl_b32 %_:s[0], 16
      INS(2, 8)
      //! s1: %_:s[0],  s1: %_:scc = s_lshl_b32 %_:s[1], 24
      INS(3, 8)
      //~gfx(7|8)! s1: %_:s[0],  s1: %_:scc = s_bfe_u32 %_:s[1], 0x100000
      //~gfx(9|11)! s1: %_:s[0] = s_pack_ll_b32_b16 %_:s[1], 0
      INS(0, 16)
      //~gfx(7|8)! s1: %_:s[0],  s1: %_:scc = s_lshl_b32 %_:s[1], 16
      //~gfx(9|11)! s1: %_:s[0] = s_pack_ll_b32_b16 0, %_:s[1]
      INS(1, 16)

#undef INS

#define INS(idx, def_b, op_b)                                                                      \
   bld.pseudo(aco_opcode::p_insert, Definition(v0_lo.advance(def_b), v2b),                         \
              Operand(v1_lo.advance(op_b), v2b), Operand::c32(idx), Operand::c32(8u));

      //>> p_unit_test 2
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(2u));
      //~gfx(8|9)! v2b: %0:v[0][0:16] = v_mov_b32 %0:v[1][0:16] dst_sel:uword0 dst_preserve src0_sel:ubyte0
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0x7060c00
      if (lvl != GFX7)
         INS(0, 0, 0)
      //~gfx(8|9)! v2b: %0:v[0][16:32] = v_mov_b32 %0:v[1][0:16] dst_sel:uword1 dst_preserve src0_sel:ubyte0
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0xc000504
      if (lvl != GFX7)
         INS(0, 2, 0)
      //~gfx(8|9)! v2b: %0:v[0][0:16] = v_mov_b32 %0:v[1][16:32] dst_sel:uword0 dst_preserve src0_sel:ubyte2
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0x7060c02
      if (lvl != GFX7)
         INS(0, 0, 2)
      //~gfx(8|9)! v2b: %0:v[0][16:32] = v_mov_b32 %0:v[1][16:32] dst_sel:uword1 dst_preserve src0_sel:ubyte2
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0xc020504
      if (lvl != GFX7)
         INS(0, 2, 2)
      //~gfx8! v1b: %0:v[0][8:16] = v_mov_b32 %0:v[1][0:16] dst_sel:ubyte1 dst_preserve src0_sel:ubyte0
      //~gfx8! v2b: %0:v[0][0:16] = v_and_b32 0xffffff00, %0:v[1]
      //~gfx9! v2b: %0:v[0][0:16] = v_lshlrev_b32 8, %0:v[1][0:16] dst_sel:uword0 dst_preserve src0_sel:dword src1_sel:ubyte0
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0x706000c
      if (lvl != GFX7)
         INS(1, 0, 0)
      //~gfx8! v1b: %0:v[0][24:32] = v_mov_b32 %0:v[1][0:16] dst_sel:ubyte3 dst_preserve src0_sel:ubyte0
      //~gfx8! v2b: %0:v[0][16:32] = v_and_b32 0xff00ffff, %0:v[1]
      //~gfx9! v2b: %0:v[0][16:32] = v_lshlrev_b32 8, %0:v[1][0:16] dst_sel:uword1 dst_preserve src0_sel:dword src1_sel:ubyte0
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0xc0504
      if (lvl != GFX7)
         INS(1, 2, 0)
      //~gfx8! v1b: %0:v[0][8:16] = v_mov_b32 %0:v[1][16:32] dst_sel:ubyte1 dst_preserve src0_sel:ubyte2
      //~gfx8! v2b: %0:v[0][0:16] = v_and_b32 0xffffff00, %0:v[1]
      //~gfx9! v2b: %0:v[0][0:16] = v_lshlrev_b32 8, %0:v[1][16:32] dst_sel:uword0 dst_preserve src0_sel:dword src1_sel:ubyte2
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0x706020c
      if (lvl != GFX7)
         INS(1, 0, 2)
      //~gfx8! v1b: %0:v[0][24:32] = v_mov_b32 %0:v[1][16:32] dst_sel:ubyte3 dst_preserve src0_sel:ubyte2
      //~gfx8! v2b: %0:v[0][16:32] = v_and_b32 0xff00ffff, %0:v[1]
      //~gfx9! v2b: %0:v[0][16:32] = v_lshlrev_b32 8, %0:v[1][16:32] dst_sel:uword1 dst_preserve src0_sel:dword src1_sel:ubyte2
      //~gfx11! v1: %0:v[0] = v_perm_b32 %0:v[0], %0:v[1], 0x20c0504
      if (lvl != GFX7)
         INS(1, 2, 2)
#undef INS

      finish_to_hw_instr_test();

      //~gfx11! s_nop
      //~gfx11! s_sendmsg sendmsg(dealloc_vgprs)
      //! s_endpgm
   }
END_TEST

BEGIN_TEST(to_hw_instr.copy_linear_vgpr_scc)
   if (!setup_cs(NULL, GFX10))
      return;

   PhysReg v0_lo{256};
   PhysReg v1_lo{257};

   //>> p_unit_test 0
   bld.pseudo(aco_opcode::p_unit_test, Operand::zero());

   /* It would be better if the scc=s0 copy was done later, but handle_operands() is complex
    * enough
    */

   //! v1: %0:v[0] = v_mov_b32 %0:v[1]
   //! s1: %0:m0 = s_mov_b32 %0:scc
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   //! v1: %0:v[0] = v_mov_b32 %0:v[1]
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   //! s1: %0:scc = s_cmp_lg_i32 %0:m0, 0
   Instruction* instr = bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v1.as_linear()),
                                   Operand(v1_lo, v1.as_linear()));
   instr->pseudo().scratch_sgpr = m0;
   instr->pseudo().tmp_in_scc = true;

   finish_to_hw_instr_test();
END_TEST

BEGIN_TEST(to_hw_instr.swap_linear_vgpr)
   if (!setup_cs(NULL, GFX10))
      return;

   PhysReg reg_v0{256};
   PhysReg reg_v1{257};
   RegClass v1_linear = v1.as_linear();

   //>> p_unit_test 0
   bld.pseudo(aco_opcode::p_unit_test, Operand::zero());

   //! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   //! v1: %0:v[0],  v1: %0:v[1] = v_swap_b32 %0:v[1], %0:v[0]
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   Instruction* instr = bld.pseudo(aco_opcode::p_parallelcopy, Definition(reg_v0, v1_linear),
                                   Definition(reg_v1, v1_linear), Operand(reg_v1, v1_linear),
                                   Operand(reg_v0, v1_linear));
   instr->pseudo().scratch_sgpr = m0;

   finish_to_hw_instr_test();
END_TEST

BEGIN_TEST(to_hw_instr.copy_linear_vgpr_v3)
   if (!setup_cs(NULL, GFX10))
      return;

   PhysReg reg_v0{256};
   PhysReg reg_v4{256 + 4};
   RegClass v3_linear = v3.as_linear();

   //>> p_unit_test 0
   bld.pseudo(aco_opcode::p_unit_test, Operand::zero());

   //! v2: %0:v[0-1] = v_lshrrev_b64 0, %0:v[4-5]
   //! v1: %0:v[2] = v_mov_b32 %0:v[6]
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   //! v2: %0:v[0-1] = v_lshrrev_b64 0, %0:v[4-5]
   //! v1: %0:v[2] = v_mov_b32 %0:v[6]
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   Instruction* instr = bld.pseudo(aco_opcode::p_parallelcopy, Definition(reg_v0, v3_linear),
                                   Operand(reg_v4, v3_linear));
   instr->pseudo().scratch_sgpr = m0;

   finish_to_hw_instr_test();
END_TEST

BEGIN_TEST(to_hw_instr.copy_linear_vgpr_coalesce)
   if (!setup_cs(NULL, GFX10))
      return;

   PhysReg reg_v0{256};
   PhysReg reg_v1{256 + 1};
   PhysReg reg_v4{256 + 4};
   PhysReg reg_v5{256 + 5};
   RegClass v1_linear = v1.as_linear();

   //>> p_unit_test 0
   //! v2: %0:v[0-1] = v_lshrrev_b64 0, %0:v[4-5]
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   //! v2: %0:v[0-1] = v_lshrrev_b64 0, %0:v[4-5]
   //! s2: %0:exec,  s1: %0:scc = s_not_b64 %0:exec
   bld.pseudo(aco_opcode::p_unit_test, Operand::zero());

   Instruction* instr = bld.pseudo(aco_opcode::p_parallelcopy, Definition(reg_v0, v1_linear),
                                   Definition(reg_v1, v1_linear), Operand(reg_v4, v1_linear),
                                   Operand(reg_v5, v1_linear));
   instr->pseudo().scratch_sgpr = m0;

   finish_to_hw_instr_test();
END_TEST

BEGIN_TEST(to_hw_instr.pack2x16_constant)
   PhysReg v0_lo{256};
   PhysReg v0_hi{256};
   PhysReg v1_lo{257};
   PhysReg v1_hi{257};
   v0_hi.reg_b += 2;
   v1_hi.reg_b += 2;

   for (amd_gfx_level lvl : {GFX10, GFX11}) {
      if (!setup_cs(NULL, lvl))
         continue;

      /* prevent usage of v_pack_b32_f16 */
      program->blocks[0].fp_mode.denorm16_64 = fp_denorm_flush;

      //>> p_unit_test 0
      //! v1: %_:v[0] = v_alignbyte_b32 0x3800, %_:v[1][16:32], 2
      bld.pseudo(aco_opcode::p_unit_test, Operand::zero());
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand(v1_hi, v2b), Operand::c16(0x3800));

      //! p_unit_test 1
      //! v2b: %_:v[0][0:16] = v_lshrrev_b32 16, %_:v[1][16:32]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(1));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand(v1_hi, v2b), Operand::zero(2));

      //! p_unit_test 2
      //~gfx10! v2b: %_:v[0][0:16] = v_and_b32 0xffff, %_:v[1][0:16]
      //~gfx11! v1: %_:v[0] = v_cvt_u32_u16 %_:v[1][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(2));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand(v1_lo, v2b), Operand::zero(2));

      //! p_unit_test 3
      //! v2b: %_:v[0][16:32] = v_and_b32 0xffff0000, %_:v[1][16:32]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(3));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::zero(2), Operand(v1_hi, v2b));

      //! p_unit_test 4
      //! v2b: %_:v[0][16:32] = v_lshlrev_b32 16, %_:v[1][0:16]
      bld.pseudo(aco_opcode::p_unit_test, Operand::c32(4));
      bld.pseudo(aco_opcode::p_parallelcopy, Definition(v0_lo, v2b), Definition(v0_hi, v2b),
                 Operand::zero(2), Operand(v1_lo, v2b));

      //~gfx11! s_nop
      //~gfx11! s_sendmsg sendmsg(dealloc_vgprs)
      //! s_endpgm

      finish_to_hw_instr_test();
   }
END_TEST
