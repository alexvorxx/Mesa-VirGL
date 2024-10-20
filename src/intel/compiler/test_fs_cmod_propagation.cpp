/*
 * Copyright © 2015 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <gtest/gtest.h>
#include "brw_fs.h"
#include "brw_fs_builder.h"
#include "brw_cfg.h"

using namespace brw;

class cmod_propagation_test : public ::testing::Test {
protected:
   cmod_propagation_test();
   ~cmod_propagation_test() override;

   struct brw_compiler *compiler;
   struct brw_compile_params params;
   struct intel_device_info *devinfo;
   void *ctx;
   struct brw_wm_prog_data *prog_data;
   struct gl_shader_program *shader_prog;
   fs_visitor *v;
   fs_builder bld;

   void test_mov_prop(enum brw_conditional_mod cmod,
                      enum brw_reg_type add_type,
                      enum brw_reg_type mov_dst_type,
                      bool expected_cmod_prop_progress);

   void test_saturate_prop(enum brw_conditional_mod before,
                           enum opcode op,
                           enum brw_reg_type add_type,
                           enum brw_reg_type op_type,
                           bool expected_cmod_prop_progress);
};

class cmod_propagation_fs_visitor : public fs_visitor
{
public:
   cmod_propagation_fs_visitor(struct brw_compiler *compiler,
                               struct brw_compile_params *params,
                               struct brw_wm_prog_data *prog_data,
                               nir_shader *shader)
      : fs_visitor(compiler, params, NULL,
                   &prog_data->base, shader, 8, false, false) {}
};


cmod_propagation_test::cmod_propagation_test()
   : bld(NULL, 0)
{
   ctx = ralloc_context(NULL);
   compiler = rzalloc(ctx, struct brw_compiler);
   devinfo = rzalloc(ctx, struct intel_device_info);
   compiler->devinfo = devinfo;

   params = {};
   params.mem_ctx = ctx;

   prog_data = ralloc(ctx, struct brw_wm_prog_data);
   nir_shader *shader =
      nir_shader_create(ctx, MESA_SHADER_FRAGMENT, NULL, NULL);

   v = new cmod_propagation_fs_visitor(compiler, &params, prog_data, shader);

   bld = fs_builder(v).at_end();

   devinfo->ver = 9;
   devinfo->verx10 = devinfo->ver * 10;
}

cmod_propagation_test::~cmod_propagation_test()
{
   delete v;
   v = NULL;

   ralloc_free(ctx);
   ctx = NULL;
}

static fs_inst *
instruction(bblock_t *block, int num)
{
   fs_inst *inst = (fs_inst *)block->start();
   for (int i = 0; i < num; i++) {
      inst = (fs_inst *)inst->next;
   }
   return inst;
}

static bool
cmod_propagation(fs_visitor *v)
{
   const bool print = getenv("TEST_DEBUG");

   if (print) {
      fprintf(stderr, "= Before =\n");
      v->cfg->dump();
   }

   bool ret = brw_fs_opt_cmod_propagation(*v);

   if (print) {
      fprintf(stderr, "\n= After =\n");
      v->cfg->dump();
   }

   return ret;
}

TEST_F(cmod_propagation_test, basic)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), dest, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest  src0  src1
    * 1: cmp.ge.f0(8)  null  dest  0.0f
    *
    * = After =
    * 0: add.ge.f0(8)  dest  src0  src1
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, basic_other_flag)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), dest, zero, BRW_CONDITIONAL_GE)
      ->flag_subreg = 1;

   /* = Before =
    *
    * 0: add(8)         dest  src0  src1
    * 1: cmp.ge.f0.1(8) null  dest  0.0f
    *
    * = After =
    * 0: add.ge.f0.1(8) dest  src0  src1
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(1, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_nonzero)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg nonzero(brw_imm_f(1.0f));
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), dest, nonzero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest  src0  src1
    * 1: cmp.ge.f0(8)  null  dest  1.0f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, non_cmod_instruction)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg zero(brw_imm_ud(0u));
   bld.FBL(dest, src0);
   bld.CMP(bld.null_reg_ud(), dest, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: fbl(8)        dest  src0
    * 1: cmp.ge.f0(8)  null  dest  0u
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_FBL, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, non_cmod_livechannel)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_UD);
   brw_reg zero(brw_imm_d(0));
   bld.emit(SHADER_OPCODE_FIND_LIVE_CHANNEL, dest)->exec_size = 32;
   bld.CMP(bld.null_reg_d(), dest, zero, BRW_CONDITIONAL_Z)->exec_size = 32;

   /* = Before =
    *
    * 0: find_live_channel(32) dest
    * 1: cmp.z.f0.0(32)   null dest 0d
    *
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(SHADER_OPCODE_FIND_LIVE_CHANNEL, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, intervening_flag_write)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), src2, zero, BRW_CONDITIONAL_GE);
   bld.CMP(bld.null_reg_f(), dest, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest  src0  src1
    * 1: cmp.ge.f0(8)  null  src2  0.0f
    * 2: cmp.ge.f0(8)  null  dest  0.0f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 1)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 2)->conditional_mod);
}

TEST_F(cmod_propagation_test, intervening_mismatch_flag_write)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), src2, zero, BRW_CONDITIONAL_GE)
      ->flag_subreg = 1;
   bld.CMP(bld.null_reg_f(), dest, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)         dest  src0  src1
    * 1: cmp.ge.f0.1(8) null  src2  0.0f
    * 2: cmp.ge.f0(8)   null  dest  0.0f
    *
    * = After =
    * 0: add.ge.f0(8)   dest  src0  src1
    * 1: cmp.ge.f0.1(8) null  src2  0.0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 1)->conditional_mod);
   EXPECT_EQ(1, instruction(block0, 1)->flag_subreg);
}

TEST_F(cmod_propagation_test, intervening_flag_read)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest0, src0, src1);
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero));
   bld.CMP(bld.null_reg_f(), dest0, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest0 src0  src1
    * 1: (+f0) sel(8)  dest1 src2  0.0f
    * 2: cmp.ge.f0(8)  null  dest0 0.0f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 2)->conditional_mod);
}

TEST_F(cmod_propagation_test, intervening_mismatch_flag_read)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest0, src0, src1);
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero))
      ->flag_subreg = 1;
   bld.CMP(bld.null_reg_f(), dest0, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)         dest0 src0  src1
    * 1: (+f0.1) sel(8) dest1 src2  0.0f
    * 2: cmp.ge.f0(8)   null  dest0 0.0f
    *
    * = After =
    * 0: add.ge.f0(8)   dest0 src0  src1
    * 1: (+f0.1) sel(8) dest1 src2  0.0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
   EXPECT_EQ(1, instruction(block0, 1)->flag_subreg);
}

TEST_F(cmod_propagation_test, intervening_dest_write)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F, 4);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F, 2);
   brw_reg zero(brw_imm_f(0.0f));

   brw_reg tex_srcs[TEX_LOGICAL_NUM_SRCS];
   tex_srcs[TEX_LOGICAL_SRC_COORDINATE] = src2;
   tex_srcs[TEX_LOGICAL_SRC_SURFACE] = brw_imm_ud(0);
   tex_srcs[TEX_LOGICAL_SRC_COORD_COMPONENTS] = brw_imm_ud(2);
   tex_srcs[TEX_LOGICAL_SRC_GRAD_COMPONENTS] = brw_imm_ud(0);
   tex_srcs[TEX_LOGICAL_SRC_RESIDENCY] = brw_imm_ud(0);

   bld.ADD(offset(dest, bld, 2), src0, src1);
   bld.emit(SHADER_OPCODE_TEX_LOGICAL, dest, tex_srcs, TEX_LOGICAL_NUM_SRCS)
      ->size_written = 4 * REG_SIZE;
   bld.CMP(bld.null_reg_f(), offset(dest, bld, 2), zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest+2  src0    src1
    * 1: tex(8) rlen 4 dest+0  src2
    * 2: cmp.ge.f0(8)  null    dest+2  0.0f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(SHADER_OPCODE_TEX_LOGICAL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 2)->conditional_mod);
}

TEST_F(cmod_propagation_test, intervening_flag_read_same_value)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   set_condmod(BRW_CONDITIONAL_GE, bld.ADD(dest0, src0, src1));
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero));
   bld.CMP(bld.null_reg_f(), dest0, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add.ge.f0(8)  dest0 src0  src1
    * 1: (+f0) sel(8)  dest1 src2  0.0f
    * 2: cmp.ge.f0(8)  null  dest0 0.0f
    *
    * = After =
    * 0: add.ge.f0(8)  dest0 src0  src1
    * 1: (+f0) sel(8)  dest1 src2  0.0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
}

TEST_F(cmod_propagation_test, negate)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest, src0, src1);
   dest.negate = true;
   bld.CMP(bld.null_reg_f(), dest, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest  src0  src1
    * 1: cmp.ge.f0(8)  null  -dest 0.0f
    *
    * = After =
    * 0: add.le.f0(8)  dest  src0  src1
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_LE, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, movnz)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.CMP(dest, src0, src1, BRW_CONDITIONAL_GE);
   set_condmod(BRW_CONDITIONAL_NZ,
               bld.MOV(bld.null_reg_f(), dest));

   /* = Before =
    *
    * 0: cmp.ge.f0(8)  dest  src0  src1
    * 1: mov.nz.f0(8)  null  dest
    *
    * = After =
    * 0: cmp.ge.f0(8)  dest  src0  src1
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, different_types_cmod_with_zero)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_D);
   brw_reg src1 = bld.vgrf(BRW_TYPE_D);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), retype(dest, BRW_TYPE_F), zero,
           BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest:D  src0:D  src1:D
    * 1: cmp.ge.f0(8)  null:F  dest:F  0.0f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, andnz_one)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   brw_reg one(brw_imm_d(1));

   bld.CMP(retype(dest, BRW_TYPE_F), src0, zero, BRW_CONDITIONAL_L);
   set_condmod(BRW_CONDITIONAL_NZ,
               bld.AND(bld.null_reg_d(), dest, one));

   /* = Before =
    * 0: cmp.l.f0(8)     dest:F  src0:F  0F
    * 1: and.nz.f0(8)    null:D  dest:D  1D
    *
    * = After =
    * 0: cmp.l.f0(8)     dest:F  src0:F  0F
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_TRUE(retype(dest, BRW_TYPE_F)
               .equals(instruction(block0, 0)->dst));
}

TEST_F(cmod_propagation_test, andnz_non_one)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   brw_reg nonone(brw_imm_d(38));

   bld.CMP(retype(dest, BRW_TYPE_F), src0, zero, BRW_CONDITIONAL_L);
   set_condmod(BRW_CONDITIONAL_NZ,
               bld.AND(bld.null_reg_d(), dest, nonone));

   /* = Before =
    * 0: cmp.l.f0(8)     dest:F  src0:F  0F
    * 1: and.nz.f0(8)    null:D  dest:D  38D
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_AND, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_cmpnz)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0));

   bld.CMP(dst0, src0, zero, BRW_CONDITIONAL_NZ);
   bld.CMP(bld.null_reg_f(), dst0, zero, BRW_CONDITIONAL_NZ);

   /* = Before =
    * 0: cmp.nz.f0.0(8) vgrf0:F, vgrf1:F, 0f
    * 1: cmp.nz.f0.0(8) null:F, vgrf0:F, 0f
    *
    * = After =
    * 0: cmp.nz.f0.0(8) vgrf0:F, vgrf1:F, 0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_cmpg)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0));

   bld.CMP(dst0, src0, zero, BRW_CONDITIONAL_NZ);
   bld.CMP(bld.null_reg_f(), dst0, zero, BRW_CONDITIONAL_G);

   /* = Before =
    * 0: cmp.nz.f0.0(8) vgrf0:F, vgrf1:F, 0f
    * 1: cmp.g.f0.0(8) null:F, vgrf0:F, 0f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_G, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, plnnz_cmpnz)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0));

   set_condmod(BRW_CONDITIONAL_NZ, bld.PLN(dst0, src0, zero));
   bld.CMP(bld.null_reg_f(), dst0, zero, BRW_CONDITIONAL_NZ);

   /* = Before =
    * 0: pln.nz.f0.0(8) vgrf0:F, vgrf1:F, 0f
    * 1: cmp.nz.f0.0(8) null:F, vgrf0:F, 0f
    *
    * = After =
    * 0: pln.nz.f0.0(8) vgrf0:F, vgrf1:F, 0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_PLN, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, plnnz_cmpz)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0));

   set_condmod(BRW_CONDITIONAL_NZ, bld.PLN(dst0, src0, zero));
   bld.CMP(bld.null_reg_f(), dst0, zero, BRW_CONDITIONAL_Z);

   /* = Before =
    * 0: pln.nz.f0.0(8) vgrf0:F, vgrf1:F, 0f
    * 1: cmp.z.f0.0(8) null:F, vgrf0:F, 0f
    *
    * = After =
    * 0: pln.z.f0.0(8) vgrf0:F, vgrf1:F, 0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_PLN, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, plnnz_sel_cmpz)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dst1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0));

   set_condmod(BRW_CONDITIONAL_NZ, bld.PLN(dst0, src0, zero));
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dst1, src0, zero));
   bld.CMP(bld.null_reg_f(), dst0, zero, BRW_CONDITIONAL_Z);

   /* = Before =
    * 0: pln.nz.f0.0(8) vgrf0:F, vgrf2:F, 0f
    * 1: (+f0.0) sel(8) vgrf1:F, vgrf2:F, 0f
    * 2: cmp.z.f0.0(8) null:F, vgrf0:F, 0f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_PLN, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 2)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_cmpg_D)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_D);
   brw_reg zero(brw_imm_d(0));

   bld.CMP(dst0, src0, zero, BRW_CONDITIONAL_NZ);
   bld.CMP(bld.null_reg_d(), dst0, zero, BRW_CONDITIONAL_G);

   /* = Before =
    * 0: cmp.nz.f0.0(8) vgrf0:D, vgrf1:D, 0d
    * 1: cmp.g.f0.0(8) null:D, vgrf0:D, 0d
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_G, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_cmpg_UD)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg zero(brw_imm_ud(0));

   bld.CMP(dst0, src0, zero, BRW_CONDITIONAL_NZ);
   bld.CMP(bld.null_reg_ud(), dst0, zero, BRW_CONDITIONAL_G);

   /* = Before =
    * 0: cmp.nz.f0.0(8) vgrf0:UD, vgrf1:UD, 0u
    * 1: cmp.g.f0.0(8) null:UD, vgrf0:UD, 0u
    *
    * = After =
    * 0: cmp.nz.f0.0(8) vgrf0:UD, vgrf1:UD, 0u
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_cmpl_D)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_D);
   brw_reg zero(brw_imm_d(0));

   bld.CMP(dst0, src0, zero, BRW_CONDITIONAL_NZ);
   bld.CMP(bld.null_reg_d(), dst0, zero, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: cmp.nz.f0.0(8) vgrf0:D, vgrf1:D, 0d
    * 1: cmp.l.f0.0(8) null:D, vgrf0:D, 0d
    *
    * = After =
    * 0: cmp.nz.f0.0(8) vgrf0:D, vgrf1:D, 0d
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_cmpl_UD)
{
   brw_reg dst0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg zero(brw_imm_ud(0));

   bld.CMP(dst0, src0, zero, BRW_CONDITIONAL_NZ);
   bld.CMP(bld.null_reg_ud(), dst0, zero, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: cmp.nz.f0.0(8) vgrf0:UD, vgrf1:UD, 0u
    * 1: cmp.l.f0.0(8) null:UD, vgrf0:UD, 0u
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, andz_one)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   brw_reg one(brw_imm_d(1));

   bld.CMP(retype(dest, BRW_TYPE_F), src0, zero, BRW_CONDITIONAL_L);
   set_condmod(BRW_CONDITIONAL_Z,
               bld.AND(bld.null_reg_d(), dest, one));

   /* = Before =
    * 0: cmp.l.f0(8)     dest:F  src0:F  0F
    * 1: and.z.f0(8)     null:D  dest:D  1D
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_AND, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_EQ, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, add_not_merge_with_compare)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* The addition and the implicit subtraction in the compare do not compute
    * related values.
    *
    * = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * (no changes)
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, subtract_merge_with_compare)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest, src0, negate(src1));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add(8)          dest:F  src0:F  -src1:F
    * 1: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.l.f0(8)     dest:F  src0:F  -src1:F
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, subtract_immediate_merge_with_compare)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg one(brw_imm_f(1.0f));
   brw_reg negative_one(brw_imm_f(-1.0f));

   bld.ADD(dest, src0, negative_one);
   bld.CMP(bld.null_reg_f(), src0, one, BRW_CONDITIONAL_NZ);

   /* = Before =
    * 0: add(8)          dest:F  src0:F  -1.0f
    * 1: cmp.nz.f0(8)    null:F  src0:F  1.0f
    *
    * = After =
    * 0: add.nz.f0(8)    dest:F  src0:F  -1.0f
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, subtract_merge_with_compare_intervening_add)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest0, src0, negate(src1));
   bld.ADD(dest1, src0, src1);
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add(8)          dest0:F src0:F  -src1:F
    * 1: add(8)          dest1:F src0:F  src1:F
    * 2: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.l.f0(8)     dest0:F src0:F  -src1:F
    * 1: add(8)          dest1:F src0:F  src1:F
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, subtract_not_merge_with_compare_intervening_partial_write)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest0, src0, negate(src1));
   set_predicate(BRW_PREDICATE_NORMAL, bld.ADD(dest1, src0, negate(src1)));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add(8)          dest0:F src0:F  -src1:F
    * 1: (+f0) add(8)    dest1:F src0:F  -src1:F
    * 2: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * (no changes)
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 1)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 2)->conditional_mod);
}

TEST_F(cmod_propagation_test, subtract_not_merge_with_compare_intervening_add)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest0, src0, negate(src1));
   set_condmod(BRW_CONDITIONAL_EQ, bld.ADD(dest1, src0, src1));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add(8)          dest0:F src0:F  -src1:F
    * 1: add.z.f0(8)     dest1:F src0:F  src1:F
    * 2: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * (no changes)
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_EQ, instruction(block0, 1)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 2)->conditional_mod);
}

TEST_F(cmod_propagation_test, add_merge_with_compare)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest, src0, src1);
   bld.CMP(bld.null_reg_f(), src0, negate(src1), BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: cmp.l.f0(8)     null:F  src0:F  -src1:F
    *
    * = After =
    * 0: add.l.f0(8)     dest:F  src0:F  src1:F
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, negative_subtract_merge_with_compare)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest, src1, negate(src0));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* The result of the subtract is the negatiion of the result of the
    * implicit subtract in the compare, so the condition must change.
    *
    * = Before =
    * 0: add(8)          dest:F  src1:F  -src0:F
    * 1: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.g.f0(8)     dest:F  src0:F  -src1:F
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_G, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, subtract_delete_compare)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);

   set_condmod(BRW_CONDITIONAL_L, bld.ADD(dest, src0, negate(src1)));
   set_predicate(BRW_PREDICATE_NORMAL, bld.MOV(dest1, src2));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add.l.f0(8)     dest0:F src0:F  -src1:F
    * 1: (+f0) mov(0)    dest1:F src2:F
    * 2: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.l.f0(8)     dest:F  src0:F  -src1:F
    * 1: (+f0) mov(0)    dest1:F src2:F
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_MOV, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
}

TEST_F(cmod_propagation_test, subtract_delete_compare_other_flag)
{
   /* This test is the same as subtract_delete_compare but it explicitly used
    * flag f0.1 for the subtraction and the comparison.
    */
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);

   set_condmod(BRW_CONDITIONAL_L, bld.ADD(dest, src0, negate(src1)))
      ->flag_subreg = 1;
   set_predicate(BRW_PREDICATE_NORMAL, bld.MOV(dest1, src2));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L)
      ->flag_subreg = 1;

   /* = Before =
    * 0: add.l.f0.1(8)   dest0:F src0:F  -src1:F
    * 1: (+f0) mov(0)    dest1:F src2:F
    * 2: cmp.l.f0.1(8)   null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.l.f0.1(8)   dest:F  src0:F  -src1:F
    * 1: (+f0) mov(0)    dest1:F src2:F
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(1, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_MOV, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
}

TEST_F(cmod_propagation_test, subtract_to_mismatch_flag)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);

   set_condmod(BRW_CONDITIONAL_L, bld.ADD(dest, src0, negate(src1)));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L)
      ->flag_subreg = 1;

   /* = Before =
    * 0: add.l.f0(8)     dest0:F src0:F  -src1:F
    * 1: cmp.l.f0.1(8)   null:F  src0:F  src1:F
    *
    * = After =
    * No changes
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 1)->conditional_mod);
   EXPECT_EQ(1, instruction(block0, 1)->flag_subreg);
}

TEST_F(cmod_propagation_test,
       subtract_merge_with_compare_intervening_mismatch_flag_write)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);

   bld.ADD(dest0, src0, negate(src1));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L)
            ->flag_subreg = 1;
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add(8)         dest0:F src0:F  -src1:F
    * 1: cmp.l.f0.1(8)  null:F  src0:F  src1:F
    * 2: cmp.l.f0(8)    null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.l.f0(8)    dest0:F src0:F  -src1:F
    * 1: cmp.l.f0.1(8)  null:F  src0:F  src1:F
    *
    * NOTE: Another perfectly valid after sequence would be:
    *
    * 0: add.f0.1(8)    dest0:F src0:F  -src1:F
    * 1: cmp.l.f0(8)    null:F  src0:F  src1:F
    *
    * However, the optimization pass starts at the end of the basic block.
    * Because of this, the cmp.l.f0 will always be chosen.  If the pass
    * changes its strategy, this test will also need to change.
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 1)->conditional_mod);
   EXPECT_EQ(1, instruction(block0, 1)->flag_subreg);
}

TEST_F(cmod_propagation_test,
       subtract_merge_with_compare_intervening_mismatch_flag_read)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));

   bld.ADD(dest0, src0, negate(src1));
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero))
      ->flag_subreg = 1;
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add(8)         dest0:F src0:F  -src1:F
    * 1: (+f0.1) sel(8) dest1   src2    0.0f
    * 2: cmp.l.f0(8)    null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.l.f0(8)    dest0:F src0:F  -src1:F
    * 1: (+f0.1) sel(8) dest1   src2    0.0f
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
   EXPECT_EQ(1, instruction(block0, 1)->flag_subreg);
}

TEST_F(cmod_propagation_test, subtract_delete_compare_derp)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);

   set_condmod(BRW_CONDITIONAL_L, bld.ADD(dest0, src0, negate(src1)));
   set_predicate(BRW_PREDICATE_NORMAL, bld.ADD(dest1, negate(src0), src1));
   bld.CMP(bld.null_reg_f(), src0, src1, BRW_CONDITIONAL_L);

   /* = Before =
    * 0: add.l.f0(8)     dest0:F src0:F  -src1:F
    * 1: (+f0) add(0)    dest1:F -src0:F src1:F
    * 2: cmp.l.f0(8)     null:F  src0:F  src1:F
    *
    * = After =
    * 0: add.l.f0(8)     dest0:F src0:F  -src1:F
    * 1: (+f0) add(0)    dest1:F -src0:F src1:F
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_L, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
}

TEST_F(cmod_propagation_test, signed_unsigned_comparison_mismatch)
{
   brw_reg dest0 = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_D);
   src0.type = BRW_TYPE_W;

   bld.ASR(dest0, negate(src0), brw_imm_d(15));
   bld.CMP(bld.null_reg_ud(), retype(dest0, BRW_TYPE_UD),
           brw_imm_ud(0u), BRW_CONDITIONAL_LE);

   /* = Before =
    * 0: asr(8)          dest:D   -src0:W 15D
    * 1: cmp.le.f0(8)    null:UD  dest:UD 0UD
    *
    * = After =
    * (no changes)
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ASR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_LE, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, ior_f2i_nz)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_D);
   brw_reg src0 = bld.vgrf(BRW_TYPE_D);
   brw_reg src1 = bld.vgrf(BRW_TYPE_D);

   bld.OR(dest, src0, src1);
   bld.MOV(bld.null_reg_d(), retype(dest, BRW_TYPE_F))
      ->conditional_mod = BRW_CONDITIONAL_NZ;

   /* = Before =
    * 0: or(8)           dest:D  src0:D  src1:D
    * 1: mov.nz(8)       null:D  dest:F
    *
    * = After =
    * No changes.
    *
    * If src0 = 0x30000000 and src1 = 0x0f000000, then the value stored in
    * dest, interpreted as floating point, is 0.5.  This bit pattern is not
    * zero, but after the float-to-integer conversion, the value is zero.
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);

   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);

   /* This is ASSERT_EQ because if end_ip is 0, the instruction(block0, 1)
    * calls will not work properly, and the test will give weird results.
    */
   ASSERT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_MOV, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, uand_b2f_g)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);

   bld.AND(dest, src0, src1);
   bld.MOV(bld.null_reg_f(), negate(retype(dest, BRW_TYPE_D)))
   ->conditional_mod = BRW_CONDITIONAL_G;

   /* = Before =
    * 0: and(8)           dest:UD  src0:UD  src1:UD
    * 1: mov.g(8)         null:F  -dest:D
    *
    * = After =
    * No changes.
    *
    * If src0 and src1 are 0xffffffff, then dest:D will be interpreted as -1,
    * and -dest:D will be 1, which is > 0.
    * If the cmod was propagated (and.l(8) dest:UD  src0:UD  src1:UD),
    * dest:UD can never be < 0.
    *
    */
   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);

   EXPECT_EQ(BRW_OPCODE_AND, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);

   /* This is ASSERT_EQ because if end_ip is 0, the instruction(block0, 1)
    * calls will not work properly, and the test will give weird results.
    */
   ASSERT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_MOV, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_G, instruction(block0, 1)->conditional_mod);
   EXPECT_TRUE(instruction(block0, 1)->src[0].negate);
}

void
cmod_propagation_test::test_mov_prop(enum brw_conditional_mod cmod,
                                     enum brw_reg_type add_type,
                                     enum brw_reg_type mov_dst_type,
                                     bool expected_cmod_prop_progress)
{
   brw_reg dest = bld.vgrf(add_type);
   brw_reg src0 = bld.vgrf(add_type);
   brw_reg src1 = bld.vgrf(add_type);

   bld.ADD(dest, src0, src1);
   bld.MOV(retype(bld.null_reg_ud(), mov_dst_type), dest)
      ->conditional_mod = cmod;

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_EQ(expected_cmod_prop_progress, cmod_propagation(v));

   const enum brw_conditional_mod add_cmod =
      expected_cmod_prop_progress ? cmod : BRW_CONDITIONAL_NONE;

   EXPECT_EQ(0, block0->start_ip);

   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(add_cmod, instruction(block0, 0)->conditional_mod);

   if (expected_cmod_prop_progress) {
      EXPECT_EQ(0, block0->end_ip);
   } else {
      /* This is ASSERT_EQ because if end_ip is 0, the instruction(block0, 1)
       * calls will not work properly, and the test will give weird results.
       */
      ASSERT_EQ(1, block0->end_ip);

      EXPECT_EQ(BRW_OPCODE_MOV, instruction(block0, 1)->opcode);
      EXPECT_EQ(cmod, instruction(block0, 1)->conditional_mod);
   }
}

TEST_F(cmod_propagation_test, fadd_fmov_nz)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.nz(8)       null:F  dest:F
    *
    * = After =
    * 0: add.nz(8)       dest:F  src0:F  src1:F
    */
   test_mov_prop(BRW_CONDITIONAL_NZ,
                 BRW_TYPE_F,
                 BRW_TYPE_F,
                 true);
}

TEST_F(cmod_propagation_test, fadd_fmov_z)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.z(8)        null:F  dest:F
    *
    * = After =
    * 0: add.z(8)        dest:F  src0:F  src1:F
    */
   test_mov_prop(BRW_CONDITIONAL_Z,
                 BRW_TYPE_F,
                 BRW_TYPE_F,
                 true);
}

TEST_F(cmod_propagation_test, fadd_fmov_l)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.l(8)        null:F  dest:F
    *
    * = After =
    * 0: add.l(8)        dest:F  src0:F  src1:F
    */
   test_mov_prop(BRW_CONDITIONAL_L,
                 BRW_TYPE_F,
                 BRW_TYPE_F,
                 true);
}

TEST_F(cmod_propagation_test, fadd_fmov_g)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.g(8)        null:F  dest:F
    *
    * = After =
    * 0: add.g(8)        dest:F  src0:F  src1:F
    */
   test_mov_prop(BRW_CONDITIONAL_G,
                 BRW_TYPE_F,
                 BRW_TYPE_F,
                 true);
}

TEST_F(cmod_propagation_test, fadd_fmov_le)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.le(8)       null:F  dest:F
    *
    * = After =
    * 0: add.le(8)        dest:F  src0:F  src1:F
    */
   test_mov_prop(BRW_CONDITIONAL_LE,
                 BRW_TYPE_F,
                 BRW_TYPE_F,
                 true);
}

TEST_F(cmod_propagation_test, fadd_fmov_ge)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.ge(8)       null:F  dest:F
    *
    * = After =
    * 0: add.ge(8)       dest:F  src0:F  src1:F
    */
   test_mov_prop(BRW_CONDITIONAL_GE,
                 BRW_TYPE_F,
                 BRW_TYPE_F,
                 true);
}

TEST_F(cmod_propagation_test, iadd_imov_nz)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.nz(8)       null:D  dest:D
    *
    * = After =
    * 0: add.nz(8)       dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_NZ,
                 BRW_TYPE_D,
                 BRW_TYPE_D,
                 true);
}

TEST_F(cmod_propagation_test, iadd_imov_z)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.z(8)        null:D  dest:D
    *
    * = After =
    * 0: add.z(8)        dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_Z,
                 BRW_TYPE_D,
                 BRW_TYPE_D,
                 true);
}

TEST_F(cmod_propagation_test, iadd_imov_l)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.l(8)        null:D  dest:D
    *
    * = After =
    * 0: add.l(8)        dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_L,
                 BRW_TYPE_D,
                 BRW_TYPE_D,
                 true);
}

TEST_F(cmod_propagation_test, iadd_imov_g)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.g(8)        null:D  dest:D
    *
    * = After =
    * 0: add.g(8)        dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_G,
                 BRW_TYPE_D,
                 BRW_TYPE_D,
                 true);
}

TEST_F(cmod_propagation_test, iadd_imov_le)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.le(8)       null:D  dest:D
    *
    * = After =
    * 0: add.le(8)       dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_LE,
                 BRW_TYPE_D,
                 BRW_TYPE_D,
                 true);
}

TEST_F(cmod_propagation_test, iadd_imov_ge)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.ge(8)       null:D  dest:D
    *
    * = After =
    * 0: add.ge(8)       dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_GE,
                 BRW_TYPE_D,
                 BRW_TYPE_D,
                 true);
}

TEST_F(cmod_propagation_test, iadd_umov_nz)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.nz(8)       null:UD dest:D
    *
    * = After =
    * 0: add.nz(8)       dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_NZ,
                 BRW_TYPE_D,
                 BRW_TYPE_UD,
                 true);
}

TEST_F(cmod_propagation_test, iadd_umov_z)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.z(8)        null:UD dest:D
    *
    * = After =
    * 0: add.z(8)        dest:D  src0:D  src1:D
    */
   test_mov_prop(BRW_CONDITIONAL_Z,
                 BRW_TYPE_D,
                 BRW_TYPE_UD,
                 true);
}

TEST_F(cmod_propagation_test, iadd_umov_l)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.l(8)        null:UD dest:D
    *
    * = After =
    * No changes.
    *
    * Due to the signed-to-usigned type conversion, the conditional modifier
    * cannot be propagated to the ADD without changing at least the
    * destination type of the add.
    *
    * This particular tests is a little silly.  Unsigned less than zero is a
    * contradiction, and earlier optimization passes should have eliminated
    * it.
    */
   test_mov_prop(BRW_CONDITIONAL_L,
                 BRW_TYPE_D,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, iadd_umov_g)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.g(8)        null:UD dest:D
    *
    * = After =
    * No changes.
    *
    * In spite of the type conversion, this could be made to work by
    * propagating NZ instead of G to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_G,
                 BRW_TYPE_D,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, iadd_umov_le)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.le(8)       null:UD dest:D
    *
    * = After =
    * No changes.
    *
    * In spite of the type conversion, this could be made to work by
    * propagating Z instead of LE to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_LE,
                 BRW_TYPE_D,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, iadd_umov_ge)
{
   /* = Before =
    * 0: add(8)          dest:D  src0:D  src1:D
    * 1: mov.ge(8)       null:UD dest:D
    *
    * = After =
    * No changes.
    *
    * Due to the signed-to-usigned type conversion, the conditional modifier
    * cannot be propagated to the ADD without changing at least the
    * destination type of the add.
    *
    * This particular tests is a little silly.  Unsigned greater than or equal
    * to zero is a tautology, and earlier optimization passes should have
    * eliminated it.
    */
   test_mov_prop(BRW_CONDITIONAL_GE,
                 BRW_TYPE_D,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2u_nz)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.nz(8)       null:UD dest:F
    *
    * = After =
    * No changes.  The MOV changes the type from float to unsigned integer.
    * If dest is in the range [-Inf, 1), the conversion will clamp it to zero.
    * If dest is NaN, the conversion will also clamp it to zero.  It is not
    * safe to propagate the NZ back to the ADD.
    *
    * It's tempting to try to propagate G to the ADD in place of the NZ.  This
    * fails for values (0, 1).  For example, if dest is 0.5, add.g would set
    * the flag, but mov.nz would not because the 0.5 would get rounded down to
    * zero.
    */
   test_mov_prop(BRW_CONDITIONAL_NZ,
                 BRW_TYPE_F,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2u_z)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.z(8)        null:UD dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to unsigned integer.  If dest is in
    * the range [-Inf, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the Z back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_Z,
                 BRW_TYPE_F,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2u_l)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.l(8)        null:UD dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to unsigned integer.  If dest is in
    * the range [-Inf, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the L back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_L,
                 BRW_TYPE_F,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2u_g)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.g(8)        null:UD dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to unsigned integer.  If dest is in
    * the range [-Inf, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the G back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_G,
                 BRW_TYPE_F,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2u_le)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.le(8)       null:UD dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to unsigned integer.  If dest is in
    * the range [-Inf, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the LE back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_LE,
                 BRW_TYPE_F,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2u_ge)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.ge(8)       null:UD dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to unsigned integer.  If dest is in
    * the range [-Inf, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the GE back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_GE,
                 BRW_TYPE_F,
                 BRW_TYPE_UD,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2i_nz)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.nz(8)       null:D  dest:F
    *
    * = After =
    * No changes.  The MOV changes the type from float to signed integer.  If
    * dest is in the range (-1, 1), the conversion will clamp it to zero.  If
    * dest is NaN, the conversion will also clamp it to zero.  It is not safe
    * to propagate the NZ back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_NZ,
                 BRW_TYPE_F,
                 BRW_TYPE_D,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2i_z)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.z(8)        null:D  dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to signed integer.  If dest is in
    * the range (-1, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the Z back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_Z,
                 BRW_TYPE_F,
                 BRW_TYPE_D,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2i_l)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.l(8)        null:D  dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to signed integer.  If dest is in
    * the range (-1, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the L back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_L,
                 BRW_TYPE_F,
                 BRW_TYPE_D,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2i_g)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.g(8)        null:D  dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to signed integer.  If dest is in
    * the range (-1, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the G back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_G,
                 BRW_TYPE_F,
                 BRW_TYPE_D,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2i_le)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.le(8)       null:D  dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to signed integer.  If dest is in
    * the range (-1, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the LE back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_LE,
                 BRW_TYPE_F,
                 BRW_TYPE_D,
                 false);
}

TEST_F(cmod_propagation_test, fadd_f2i_ge)
{
   /* = Before =
    * 0: add(8)          dest:F  src0:F  src1:F
    * 1: mov.ge(8)       null:D  dest:F
    *
    * = After =
    * No changes.
    *
    * The MOV changes the type from float to signed integer.  If dest is in
    * the range (-1, 1), the conversion will clamp it to zero.  If dest is
    * NaN, the conversion will also clamp it to zero.  It is not safe to
    * propagate the GE back to the ADD.
    */
   test_mov_prop(BRW_CONDITIONAL_GE,
                 BRW_TYPE_F,
                 BRW_TYPE_D,
                 false);
}

void
cmod_propagation_test::test_saturate_prop(enum brw_conditional_mod before,
                                          enum opcode op,
                                          enum brw_reg_type add_type,
                                          enum brw_reg_type op_type,
                                          bool expected_cmod_prop_progress)
{
   brw_reg dest = bld.vgrf(add_type);
   brw_reg src0 = bld.vgrf(add_type);
   brw_reg src1 = bld.vgrf(add_type);
   brw_reg zero(brw_imm_ud(0));

   bld.ADD(dest, src0, src1)->saturate = true;

   assert(op == BRW_OPCODE_CMP || op == BRW_OPCODE_MOV);
   if (op == BRW_OPCODE_CMP) {
      bld.CMP(bld.vgrf(op_type, 0),
              retype(dest, op_type),
              retype(zero, op_type),
              before);
   } else {
      bld.MOV(bld.vgrf(op_type, 0), retype(dest, op_type))
         ->conditional_mod = before;
   }

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_EQ(expected_cmod_prop_progress, cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);

   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(add_type, instruction(block0, 0)->dst.type);
   EXPECT_EQ(add_type, instruction(block0, 0)->src[0].type);
   EXPECT_EQ(add_type, instruction(block0, 0)->src[1].type);
   EXPECT_TRUE(instruction(block0, 0)->saturate);

   if (expected_cmod_prop_progress) {
      EXPECT_EQ(0, block0->end_ip);
      EXPECT_EQ(before, instruction(block0, 0)->conditional_mod);
   } else {
      EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);

      /* This is ASSERT_EQ because if end_ip is 0, the instruction(block0, 1)
       * calls will not work properly, and the test will give weird results.
       */
      ASSERT_EQ(1, block0->end_ip);
      EXPECT_EQ(op, instruction(block0, 1)->opcode);
      EXPECT_EQ(op_type, instruction(block0, 1)->dst.type);
      EXPECT_EQ(op_type, instruction(block0, 1)->src[0].type);
      EXPECT_FALSE(instruction(block0, 1)->saturate);
      EXPECT_EQ(before, instruction(block0, 1)->conditional_mod);
   }
}

TEST_F(cmod_propagation_test, float_saturate_nz_cmp)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.nz.f0(8)  null  dest  0.0f
    *
    * = After =
    * 0: add.sat.nz.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_NZ, BRW_OPCODE_CMP,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_nz_mov)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.nz.f0(8)  null  dest
    *
    * = After =
    * 0: add.sat.nz.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_NZ, BRW_OPCODE_MOV,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_z_cmp)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.z.f0(8)   null  dest  0.0f
    *
    * = After =
    * 0: add.sat.z.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_Z, BRW_OPCODE_CMP,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_z_mov)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.z.f0(8)   null  dest
    *
    * = After =
    * 0: add.sat.z.f0(8) dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_Z, BRW_OPCODE_MOV,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_g_cmp)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.g.f0(8)   null  dest  0.0f
    *
    * = After =
    * 0: add.sat.g.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_G, BRW_OPCODE_CMP,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_g_mov)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.g.f0(8)   null  dest
    *
    * = After =
    * 0: add.sat.g.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_G, BRW_OPCODE_MOV,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_le_cmp)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.le.f0(8)  null  dest  0.0f
    *
    * = After =
    * 0: add.sat.le.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_LE, BRW_OPCODE_CMP,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_le_mov)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].  (sat(x) <= 0) == (x <= 0).
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.le.f0(8)  null  dest
    *
    * = After =
    * 0: add.sat.le.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_LE, BRW_OPCODE_MOV,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_l_cmp)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.l.f0(8)  null  dest  0.0f
    *
    * = After =
    * 0: add.sat.l.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_L, BRW_OPCODE_CMP,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_l_mov)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.l.f0(8)   null  dest
    *
    * = After =
    * 0: add.sat.l.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_L, BRW_OPCODE_MOV,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_ge_cmp)
{
   /* With the saturate modifier, the comparison happens after clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.ge.f0(8)  null  dest  0.0f
    *
    * = After =
    * 0: add.sat.ge.f0(8)  dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_GE, BRW_OPCODE_CMP,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, float_saturate_ge_mov)
{
   /* With the saturate modifier, the comparison happens before clamping to
    * [0, 1].
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.ge.f0(8)  null  dest
    *
    * = After =
    * 0: add.sat.ge.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_GE, BRW_OPCODE_MOV,
                      BRW_TYPE_F, BRW_TYPE_F,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_nz_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.nz.f0(8)  null  dest  0
    *
    * = After =
    * 0: add.sat.nz.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_NZ, BRW_OPCODE_CMP,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, uint_saturate_nz_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest:UD  src0:UD  src1:UD
    * 1: cmp.nz.f0(8)  null:D   dest:D   0
    *
    * = After =
    * 0: add.sat.nz.f0(8)    dest:UD  src0:UD  src1:UD
    */
   test_saturate_prop(BRW_CONDITIONAL_NZ, BRW_OPCODE_CMP,
                      BRW_TYPE_UD, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_nz_mov)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.nz.f0(8)  null  dest
    *
    * = After =
    * 0: add.sat.nz.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_NZ, BRW_OPCODE_MOV,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_z_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.z.f0(8)   null  dest  0
    *
    * = After =
    * 0: add.sat.z.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_Z, BRW_OPCODE_CMP,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, uint_saturate_z_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)   dest:UD  src0:UD  src1:UD
    * 1: cmp.z.f0(8)  null:D   dest:D   0
    *
    * = After =
    * 0: add.sat.z.f0(8)    dest:UD  src0:UD  src1:UD
    */
   test_saturate_prop(BRW_CONDITIONAL_Z, BRW_OPCODE_CMP,
                      BRW_TYPE_UD, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_z_mov)
{
   /* With the saturate modifier, the comparison happens before clamping to
    * [0, 1].  (sat(x) == 0) == (x <= 0).
    *
    * = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.z.f0(8)   null  dest
    *
    * = After =
    * 0: add.sat.z.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_Z, BRW_OPCODE_MOV,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_g_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.g.f0(8)   null  dest  0
    *
    * = After =
    * 0: add.sat.g.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_G, BRW_OPCODE_CMP,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_g_mov)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.g.f0(8)   null  dest
    *
    * = After =
    * 0: add.sat.g.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_G, BRW_OPCODE_MOV,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_le_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.le.f0(8)  null  dest  0
    *
    * = After =
    * 0: add.sat.le.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_LE, BRW_OPCODE_CMP,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_le_mov)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.le.f0(8)  null  dest
    *
    * = After =
    * 0: add.sat.le.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_LE, BRW_OPCODE_MOV,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_l_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.l.f0(8)  null  dest  0
    *
    * = After =
    * 0: add.sat.l.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_L, BRW_OPCODE_CMP,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_l_mov)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.l.f0(8)  null  dest  0
    *
    * = After =
    * 0: add.sat.l.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_L, BRW_OPCODE_MOV,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_ge_cmp)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: cmp.ge.f0(8)  null  dest  0
    *
    * = After =
    * 0: add.sat.ge.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_GE, BRW_OPCODE_CMP,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, int_saturate_ge_mov)
{
   /* = Before =
    *
    * 0: add.sat(8)    dest  src0  src1
    * 1: mov.ge.f0(8)  null  dest
    *
    * = After =
    * 0: add.sat.ge.f0(8)    dest  src0  src1
    */
   test_saturate_prop(BRW_CONDITIONAL_GE, BRW_OPCODE_MOV,
                      BRW_TYPE_D, BRW_TYPE_D,
                      true);
}

TEST_F(cmod_propagation_test, not_to_or)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    */
   brw_reg dest = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);
   bld.OR(dest, src0, src1);
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest));

   /* = Before =
    *
    * 0: or(8)         dest  src0  src1
    * 1: not.nz.f0(8)  null  dest
    *
    * = After =
    * 0: or.z.f0(8)    dest  src0  src1
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, not_to_and)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    */
   brw_reg dest = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);
   bld.AND(dest, src0, src1);
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest));

   /* = Before =
    *
    * 0: and(8)        dest  src0  src1
    * 1: not.nz.f0(8)  null  dest
    *
    * = After =
    * 0: and.z.f0(8)   dest  src0  src1
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_AND, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, not_to_uadd)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    *
    * The optimization pass currently restricts to just OR and AND.  It's
    * possible that this is too restrictive, and the actual, necessary
    * restriction is just the the destination type of the ALU instruction is
    * the same as the source type of the NOT instruction.
    */
   brw_reg dest = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);
   bld.ADD(dest, src0, src1);
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest));

   /* = Before =
    *
    * 0: add(8)        dest  src0  src1
    * 1: not.nz.f0(8)  null  dest
    *
    * = After =
    * No changes
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_NOT, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, not_to_fadd_to_ud)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    *
    * The optimization pass currently restricts to just OR and AND.  It's
    * possible that this is too restrictive, and the actual, necessary
    * restriction is just the the destination type of the ALU instruction is
    * the same as the source type of the NOT instruction.
    */
   brw_reg dest = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest, src0, src1);
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest));

   /* = Before =
    *
    * 0: add(8)        dest.ud src0.f  src1.f
    * 1: not.nz.f0(8)  null    dest.ud
    *
    * = After =
    * No changes
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_NOT, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, not_to_fadd)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    *
    * The optimization pass currently restricts to just OR and AND.  It's
    * possible that this is too restrictive, and the actual, necessary
    * restriction is just the the destination type of the ALU instruction is
    * the same as the source type of the NOT instruction.
    */
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   bld.ADD(dest, src0, src1);
   set_condmod(BRW_CONDITIONAL_NZ,
               bld.NOT(bld.null_reg_ud(),
                       retype(dest, BRW_TYPE_UD)));

   /* = Before =
    *
    * 0: add(8)        dest.f  src0.f  src1.f
    * 1: not.nz.f0(8)  null    dest.ud
    *
    * = After =
    * No changes
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_NOT, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, not_to_or_intervening_flag_read_compatible_value)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    */
   brw_reg dest0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   set_condmod(BRW_CONDITIONAL_Z, bld.OR(dest0, src0, src1));
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero));
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest0));

   /* = Before =
    *
    * 0: or.z.f0(8)    dest0 src0  src1
    * 1: (+f0) sel(8)  dest1 src2  0.0f
    * 2: not.nz.f0(8)  null  dest0
    *
    * = After =
    * 0: or.z.f0(8)    dest0 src0  src1
    * 1: (+f0) sel(8)  dest1 src2  0.0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
}

TEST_F(cmod_propagation_test,
       not_to_or_intervening_flag_read_compatible_value_mismatch_flag)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    */
   brw_reg dest0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   set_condmod(BRW_CONDITIONAL_Z, bld.OR(dest0, src0, src1))
      ->flag_subreg = 1;
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero));
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest0));

   /* = Before =
    *
    * 0: or.z.f0.1(8)  dest0 src0  src1
    * 1: (+f0) sel(8)  dest1 src2  0.0f
    * 2: not.nz.f0(8)  null  dest0
    *
    * = After =
    * No changes
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(1, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
   EXPECT_EQ(BRW_OPCODE_NOT, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 2)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 2)->flag_subreg);
}

TEST_F(cmod_propagation_test, not_to_or_intervening_flag_read_incompatible_value)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    */
   brw_reg dest0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   set_condmod(BRW_CONDITIONAL_NZ, bld.OR(dest0, src0, src1));
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero));
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest0));

   /* = Before =
    *
    * 0: or.nz.f0(8)   dest0 src0  src1
    * 1: (+f0) sel(8)  dest1 src2  0.0f
    * 2: not.nz.f0(8)  null  dest0
    *
    * = After =
    * No changes
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
   EXPECT_EQ(BRW_OPCODE_NOT, instruction(block0, 2)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 2)->conditional_mod);
}

TEST_F(cmod_propagation_test, not_to_or_intervening_mismatch_flag_write)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    */
   brw_reg dest0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);

   bld.OR(dest0, src0, src1);
   set_condmod(BRW_CONDITIONAL_Z, bld.OR(dest1, src0, src1))
      ->flag_subreg = 1;
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest0));

   /* = Before =
    *
    * 0: or(8)          dest0 src0  src1
    * 1: or.z.f0.1(8)   dest1 src0  src1
    * 2: not.nz.f0(8)   null  dest0
    *
    * = After =
    * 0: or.z.f0(8)     dest0 src0  src1
    * 1: or.z.f0.1(8)   dest1 src0  src1
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 1)->conditional_mod);
   EXPECT_EQ(1, instruction(block0, 1)->flag_subreg);
}

TEST_F(cmod_propagation_test, not_to_or_intervening_mismatch_flag_read)
{
   /* Exercise propagation of conditional modifier from a NOT instruction to
    * another ALU instruction as performed by cmod_propagate_not.
    */
   brw_reg dest0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src1 = bld.vgrf(BRW_TYPE_UD);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));

   bld.OR(dest0, src0, src1);
   set_predicate(BRW_PREDICATE_NORMAL, bld.SEL(dest1, src2, zero))
      ->flag_subreg = 1;
   set_condmod(BRW_CONDITIONAL_NZ, bld.NOT(bld.null_reg_ud(), dest0));

   /* = Before =
    *
    * 0: or(8)          dest0 src0  src1
    * 1: (+f0.1) sel(8) dest1 src2  0.0f
    * 2: not.nz.f0(8)   null  dest0
    *
    * = After =
    * 0: or.z.f0(8)     dest0 src0  src1
    * 1: (+f0.1) sel(8) dest1 src2  0.0f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_OR, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_Z, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(0, instruction(block0, 0)->flag_subreg);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_PREDICATE_NORMAL, instruction(block0, 1)->predicate);
   EXPECT_EQ(1, instruction(block0, 1)->flag_subreg);
}

TEST_F(cmod_propagation_test, cmp_to_add_float_e)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg neg10(brw_imm_f(-10.0f));
   brw_reg pos10(brw_imm_f(10.0f));

   bld.ADD(dest, src0, neg10)->saturate = true;
   bld.CMP(bld.null_reg_f(), src0, pos10, BRW_CONDITIONAL_EQ);

   /* = Before =
    * 0: add.sat(8) vgrf0:F, vgrf1:F, -10f
    * 1: cmp.z.f0.0(8) null:F, vgrf1:F, 10f
    *
    * = After =
    * (no changes)
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_FALSE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NONE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_EQ, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_to_add_float_g)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg neg10(brw_imm_f(-10.0f));
   brw_reg pos10(brw_imm_f(10.0f));

   bld.ADD(dest, src0, neg10)->saturate = true;
   bld.CMP(bld.null_reg_f(), src0, pos10, BRW_CONDITIONAL_G);

   /* = Before =
    * 0: add.sat(8) vgrf0:F, vgrf1:F, -10f
    * 1: cmp.g.f0.0(8) null:F, vgrf1:F, 10f
    *
    * = After =
    * 0: add.sat.g.f0.0(8) vgrf0:F, vgrf1:F, -10f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_G, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, cmp_to_add_float_le)
{
   brw_reg dest = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg neg10(brw_imm_f(-10.0f));
   brw_reg pos10(brw_imm_f(10.0f));

   bld.ADD(dest, src0, neg10)->saturate = true;
   bld.CMP(bld.null_reg_f(), src0, pos10, BRW_CONDITIONAL_LE);

   /* = Before =
    * 0: add.sat(8) vgrf0:F, vgrf1:F, -10f
    * 1: cmp.le.f0.0(8) null:F, vgrf1:F, 10f
    *
    * = After =
    * 0: add.sat.le.f0.0(8) vgrf0:F, vgrf1:F, -10f
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_LE, instruction(block0, 0)->conditional_mod);
}

TEST_F(cmod_propagation_test, prop_across_sel)
{
   brw_reg dest1 = bld.vgrf(BRW_TYPE_F);
   brw_reg dest2 = bld.vgrf(BRW_TYPE_F);
   brw_reg src0 = bld.vgrf(BRW_TYPE_F);
   brw_reg src1 = bld.vgrf(BRW_TYPE_F);
   brw_reg src2 = bld.vgrf(BRW_TYPE_F);
   brw_reg src3 = bld.vgrf(BRW_TYPE_F);
   brw_reg zero(brw_imm_f(0.0f));
   bld.ADD(dest1, src0, src1);
   bld.emit_minmax(dest2, src2, src3, BRW_CONDITIONAL_GE);
   bld.CMP(bld.null_reg_f(), dest1, zero, BRW_CONDITIONAL_GE);

   /* = Before =
    *
    * 0: add(8)        dest1 src0  src1
    * 1: sel.ge(8)     dest2 src2  src3
    * 2: cmp.ge.f0(8)  null  dest1 0.0f
    *
    * = After =
    * 0: add.ge.f0(8)  dest1 src0  src1
    * 1: sel.ge(8)     dest2 src2  src3
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(2, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);
   EXPECT_EQ(BRW_OPCODE_ADD, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 0)->conditional_mod);
   EXPECT_EQ(BRW_OPCODE_SEL, instruction(block0, 1)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_GE, instruction(block0, 1)->conditional_mod);
}

TEST_F(cmod_propagation_test, Boolean_size_conversion)
{
   brw_reg dest1 = bld.vgrf(BRW_TYPE_W);
   brw_reg src0 = bld.vgrf(BRW_TYPE_W);
   brw_reg zero(brw_imm_w(0));

   bld.CMP(dest1, src0, zero, BRW_CONDITIONAL_NZ);
   set_condmod(BRW_CONDITIONAL_NZ, bld.MOV(bld.null_reg_d(), dest1));

   /* = Before =
    * 0: cmp.nz.f0     dest1:W    src0:W    0W
    * 1: mov.nz.f0     null:D     dest1:W
    *
    * = After =
    * 0: cmp.nz.f0     dest1:W    src0:W    0W
    */

   brw_calculate_cfg(*v);
   bblock_t *block0 = v->cfg->blocks[0];

   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(1, block0->end_ip);

   EXPECT_TRUE(cmod_propagation(v));
   EXPECT_EQ(0, block0->start_ip);
   EXPECT_EQ(0, block0->end_ip);

   EXPECT_EQ(BRW_OPCODE_CMP, instruction(block0, 0)->opcode);
   EXPECT_EQ(BRW_CONDITIONAL_NZ, instruction(block0, 0)->conditional_mod);
}
