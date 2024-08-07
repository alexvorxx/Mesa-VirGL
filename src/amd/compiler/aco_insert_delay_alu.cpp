/*
 * Copyright © 2018 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_builder.h"
#include "aco_ir.h"

#include <map>
#include <stack>
#include <vector>

namespace aco {

namespace {

/* On GFX11+ the SIMD frontend doesn't switch to issuing instructions from a different
 * wave if there is an ALU stall. Hence we have an instruction (s_delay_alu) to signal
 * that we should switch to a different wave and contains info on dependencies as to
 * when we can switch back.
 *
 * This seems to apply only for ALU->ALU dependencies as other instructions have better
 * integration with the frontend.
 *
 * Note that if we do not emit s_delay_alu things will still be correct, but the wave
 * will stall in the ALU (and the ALU will be doing nothing else). We'll use this as
 * I'm pretty sure our cycle info is wrong at times (necessarily so, e.g. wave64 VALU
 * instructions can take a different number of cycles based on the exec mask)
 */
struct alu_delay_info {
   /* These are the values directly above the max representable value, i.e. the wait
    * would turn into a no-op when we try to wait for something further back than
    * this.
    */
   static constexpr int8_t valu_nop = 5;
   static constexpr int8_t trans_nop = 4;

   /* How many VALU instructions ago this value was written */
   int8_t valu_instrs = valu_nop;
   /* Cycles until the writing VALU instruction is finished */
   int8_t valu_cycles = 0;

   /* How many Transcedent instructions ago this value was written */
   int8_t trans_instrs = trans_nop;
   /* Cycles until the writing Transcendent instruction is finished */
   int8_t trans_cycles = 0;

   /* Cycles until the writing SALU instruction is finished*/
   int8_t salu_cycles = 0;

   bool combine(const alu_delay_info& other)
   {
      bool changed = other.valu_instrs < valu_instrs || other.trans_instrs < trans_instrs ||
                     other.salu_cycles > salu_cycles || other.valu_cycles > valu_cycles ||
                     other.trans_cycles > trans_cycles;
      valu_instrs = std::min(valu_instrs, other.valu_instrs);
      trans_instrs = std::min(trans_instrs, other.trans_instrs);
      salu_cycles = std::max(salu_cycles, other.salu_cycles);
      valu_cycles = std::max(valu_cycles, other.valu_cycles);
      trans_cycles = std::max(trans_cycles, other.trans_cycles);
      return changed;
   }

   /* Needs to be called after any change to keep the data consistent. */
   bool fixup()
   {
      if (valu_instrs >= valu_nop || valu_cycles <= 0) {
         valu_instrs = valu_nop;
         valu_cycles = 0;
      }

      if (trans_instrs >= trans_nop || trans_cycles <= 0) {
         trans_instrs = trans_nop;
         trans_cycles = 0;
      }

      salu_cycles = std::max<int8_t>(salu_cycles, 0);

      return empty();
   }

   /* Returns true if a wait would be a no-op */
   bool empty() const
   {
      return valu_instrs == valu_nop && trans_instrs == trans_nop && salu_cycles == 0;
   }

   UNUSED void print(FILE* output) const
   {
      if (valu_instrs != valu_nop)
         fprintf(output, "valu_instrs: %u\n", valu_instrs);
      if (valu_cycles)
         fprintf(output, "valu_cycles: %u\n", valu_cycles);
      if (trans_instrs != trans_nop)
         fprintf(output, "trans_instrs: %u\n", trans_instrs);
      if (trans_cycles)
         fprintf(output, "trans_cycles: %u\n", trans_cycles);
      if (salu_cycles)
         fprintf(output, "salu_cycles: %u\n", salu_cycles);
   }
};

struct delay_ctx {
   Program* program;
   std::map<PhysReg, alu_delay_info> gpr_map;

   delay_ctx() {}
   delay_ctx(Program* program_) : program(program_) {}

   bool join(const delay_ctx* other)
   {
      bool changed = false;
      for (const auto& entry : other->gpr_map) {
         using iterator = std::map<PhysReg, alu_delay_info>::iterator;
         const std::pair<iterator, bool> insert_pair = gpr_map.insert(entry);
         if (insert_pair.second)
            changed = true;
         else
            changed |= insert_pair.first->second.combine(entry.second);
      }

      return changed;
   }

   UNUSED void print(FILE* output) const
   {
      for (const auto& entry : gpr_map) {
         fprintf(output, "gpr_map[%c%u] = {\n", entry.first.reg() >= 256 ? 'v' : 's',
                 entry.first.reg() & 0xff);
         entry.second.print(output);
         fprintf(output, "}\n");
      }
   }
};

void
check_alu(delay_ctx& ctx, alu_delay_info& delay, Instruction* instr)
{
   for (const Operand op : instr->operands) {
      if (op.isConstant() || op.isUndefined())
         continue;

      /* check consecutively read gprs */
      for (unsigned j = 0; j < op.size(); j++) {
         std::map<PhysReg, alu_delay_info>::iterator it =
            ctx.gpr_map.find(PhysReg{op.physReg() + j});
         if (it != ctx.gpr_map.end())
            delay.combine(it->second);
      }
   }
}

bool
parse_delay_alu(delay_ctx& ctx, alu_delay_info& delay, Instruction* instr)
{
   if (instr->opcode != aco_opcode::s_delay_alu)
      return false;

   unsigned imm[2] = {instr->salu().imm & 0xf, (instr->salu().imm >> 7) & 0xf};
   for (unsigned i = 0; i < 2; ++i) {
      alu_delay_wait wait = (alu_delay_wait)imm[i];
      if (wait >= alu_delay_wait::VALU_DEP_1 && wait <= alu_delay_wait::VALU_DEP_4)
         delay.valu_instrs = imm[i] - (uint32_t)alu_delay_wait::VALU_DEP_1 + 1;
      else if (wait >= alu_delay_wait::TRANS32_DEP_1 && wait <= alu_delay_wait::TRANS32_DEP_3)
         delay.trans_instrs = imm[i] - (uint32_t)alu_delay_wait::TRANS32_DEP_1 + 1;
      else if (wait >= alu_delay_wait::SALU_CYCLE_1)
         delay.salu_cycles = imm[i] - (uint32_t)alu_delay_wait::SALU_CYCLE_1 + 1;
   }

   delay.valu_cycles = instr->pass_flags & 0xffff;
   delay.trans_cycles = instr->pass_flags >> 16;

   return true;
}

void
update_alu(delay_ctx& ctx, bool is_valu, bool is_trans, int cycles)
{
   std::map<PhysReg, alu_delay_info>::iterator it = ctx.gpr_map.begin();
   while (it != ctx.gpr_map.end()) {
      alu_delay_info& entry = it->second;
      entry.valu_instrs += is_valu ? 1 : 0;
      entry.trans_instrs += is_trans ? 1 : 0;
      entry.salu_cycles -= cycles;
      entry.valu_cycles -= cycles;
      entry.trans_cycles -= cycles;
      it = it->second.fixup() ? ctx.gpr_map.erase(it) : std::next(it);
   }
}

void
kill_alu(alu_delay_info& delay, Instruction* instr, delay_ctx& ctx)
{
   if (instr->isVALU() || instr->isSALU())
      check_alu(ctx, delay, instr);

   if (!delay.empty()) {
      update_alu(ctx, false, false, MAX3(delay.salu_cycles, delay.valu_cycles, delay.trans_cycles));

      /* remove all gprs with higher counter from map */
      std::map<PhysReg, alu_delay_info>::iterator it = ctx.gpr_map.begin();
      while (it != ctx.gpr_map.end()) {
         if (delay.valu_instrs <= it->second.valu_instrs)
            it->second.valu_instrs = alu_delay_info::valu_nop;
         if (delay.trans_instrs <= it->second.trans_instrs)
            it->second.trans_instrs = alu_delay_info::trans_nop;
         it = it->second.fixup() ? ctx.gpr_map.erase(it) : std::next(it);
      }
   }
}

void
gen_alu(Instruction* instr, delay_ctx& ctx)
{
   if (instr->isEXP() || instr->isDS() || instr->isMIMG() || instr->isFlatLike() ||
       instr->isMUBUF() || instr->isMTBUF()) {
      ctx.gpr_map.clear();
      return;
   }

   Instruction_cycle_info cycle_info = get_cycle_info(*ctx.program, *instr);
   bool is_valu = instr->isVALU();
   bool is_trans = instr->isTrans();

   if (is_trans || is_valu || instr->isSALU()) {
      alu_delay_info delay;
      if (is_trans) {
         delay.trans_instrs = 0;
         delay.trans_cycles = cycle_info.latency;
      } else if (is_valu) {
         delay.valu_instrs = 0;
         delay.valu_cycles = cycle_info.latency;
      } else if (instr->isSALU()) {
         delay.salu_cycles = cycle_info.latency;
      }

      for (const Definition& def : instr->definitions) {
         for (unsigned i = 0; i < def.size(); i++) {
            auto it = ctx.gpr_map.emplace(PhysReg{def.physReg().reg() + i}, delay);
            if (!it.second)
               it.first->second.combine(delay);
         }
      }
   }

   update_alu(ctx, is_valu && instr_info.classes[(int)instr->opcode] != instr_class::wmma, is_trans,
              cycle_info.issue_cycles);
}

void
emit_delay_alu(delay_ctx& ctx, std::vector<aco_ptr<Instruction>>& instructions,
               alu_delay_info& delay)
{
   uint32_t imm = 0;
   if (delay.trans_instrs != delay.trans_nop) {
      imm |= (uint32_t)alu_delay_wait::TRANS32_DEP_1 + delay.trans_instrs - 1;
   }

   if (delay.valu_instrs != delay.valu_nop) {
      imm |= ((uint32_t)alu_delay_wait::VALU_DEP_1 + delay.valu_instrs - 1) << (imm ? 7 : 0);
   }

   /* Note that we can only put 2 wait conditions in the instruction, so if we have all 3 we just
    * drop the SALU one. Here we use that this doesn't really affect correctness so occasionally
    * getting this wrong isn't an issue. */
   if (delay.salu_cycles && imm <= 0xf) {
      unsigned cycles = std::min<uint8_t>(3, delay.salu_cycles);
      imm |= ((uint32_t)alu_delay_wait::SALU_CYCLE_1 + cycles - 1) << (imm ? 7 : 0);
   }

   Instruction* inst = create_instruction(aco_opcode::s_delay_alu, Format::SOPP, 0, 0);
   inst->salu().imm = imm;
   inst->pass_flags = (delay.valu_cycles | (delay.trans_cycles << 16));
   instructions.emplace_back(inst);
   delay = alu_delay_info();
}

void
handle_block(Program* program, Block& block, delay_ctx& ctx)
{
   std::vector<aco_ptr<Instruction>> new_instructions;
   alu_delay_info queued_delay;

   for (size_t i = 0; i < block.instructions.size(); i++) {
      aco_ptr<Instruction>& instr = block.instructions[i];
      bool is_delay_alu = parse_delay_alu(ctx, queued_delay, instr.get());

      kill_alu(queued_delay, instr.get(), ctx);
      gen_alu(instr.get(), ctx);

      if (!is_delay_alu) {
         if (!queued_delay.empty())
            emit_delay_alu(ctx, new_instructions, queued_delay);
         new_instructions.emplace_back(std::move(instr));
      }
   }

   if (!queued_delay.empty())
      emit_delay_alu(ctx, new_instructions, queued_delay);
   block.instructions.swap(new_instructions);
}

} /* end namespace */

void
insert_delay_alu(Program* program)
{
   /* per BB ctx */
   std::vector<bool> done(program->blocks.size());
   std::vector<delay_ctx> in_ctx(program->blocks.size(), delay_ctx(program));
   std::vector<delay_ctx> out_ctx(program->blocks.size(), delay_ctx(program));

   std::stack<unsigned, std::vector<unsigned>> loop_header_indices;
   unsigned loop_progress = 0;

   for (unsigned i = 0; i < program->blocks.size();) {
      Block& current = program->blocks[i++];

      if (current.kind & block_kind_discard_early_exit) {
         /* Because the jump to the discard early exit block may happen anywhere in a block, it's
          * not possible to join it with its predecessors this way.
          */
         continue;
      }

      delay_ctx ctx = in_ctx[current.index];

      if (current.kind & block_kind_loop_header) {
         loop_header_indices.push(current.index);
      } else if (current.kind & block_kind_loop_exit) {
         bool repeat = false;
         if (loop_progress == loop_header_indices.size()) {
            i = loop_header_indices.top();
            repeat = true;
         }
         loop_header_indices.pop();
         loop_progress = std::min<unsigned>(loop_progress, loop_header_indices.size());
         if (repeat)
            continue;
      }

      bool changed = false;
      for (unsigned b : current.linear_preds)
         changed |= ctx.join(&out_ctx[b]);

      if (done[current.index] && !changed) {
         in_ctx[current.index] = std::move(ctx);
         continue;
      } else {
         in_ctx[current.index] = ctx;
      }

      loop_progress = std::max<unsigned>(loop_progress, current.loop_nest_depth);
      done[current.index] = true;

      handle_block(program, current, ctx);

      out_ctx[current.index] = std::move(ctx);
   }
}

void
combine_delay_alu(Program* program)
{
   /* Combine s_delay_alu using the skip field. */
   for (Block& block : program->blocks) {
      int i = 0;
      int prev_delay_alu = -1;
      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (instr->opcode != aco_opcode::s_delay_alu) {
            block.instructions[i++] = std::move(instr);
            continue;
         }

         uint16_t imm = instr->salu().imm;
         int skip = i - prev_delay_alu - 1;
         if (imm >> 7 || prev_delay_alu < 0 || skip >= 6) {
            if (imm >> 7 == 0)
               prev_delay_alu = i;
            block.instructions[i++] = std::move(instr);
            continue;
         }

         block.instructions[prev_delay_alu]->salu().imm |= (skip << 4) | (imm << 7);
         prev_delay_alu = -1;
      }
      block.instructions.resize(i);
   }
}

} // namespace aco
