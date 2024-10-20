/*
 * Copyright © 2018 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "aco_ir.h"

#include "util/bitset.h"
#include "util/enum_operators.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <map>
#include <optional>
#include <vector>

namespace aco {
namespace {

struct ra_ctx;
struct DefInfo;

unsigned get_subdword_operand_stride(amd_gfx_level gfx_level, const aco_ptr<Instruction>& instr,
                                     unsigned idx, RegClass rc);
void add_subdword_operand(ra_ctx& ctx, aco_ptr<Instruction>& instr, unsigned idx, unsigned byte,
                          RegClass rc);
void add_subdword_definition(Program* program, aco_ptr<Instruction>& instr, PhysReg reg,
                             bool allow_16bit_write);

struct assignment {
   PhysReg reg;
   RegClass rc;
   union {
      struct {
         bool assigned : 1;
         bool vcc : 1;
         bool m0 : 1;
         bool renamed : 1;
      };
      uint8_t _ = 0;
   };
   uint32_t affinity = 0;
   assignment() = default;
   assignment(PhysReg reg_, RegClass rc_) : reg(reg_), rc(rc_) { assigned = true; }
   void set(const Definition& def)
   {
      assigned = true;
      reg = def.physReg();
      rc = def.regClass();
   }
};

/* Iterator type for making PhysRegInterval compatible with range-based for */
struct PhysRegIterator {
   using difference_type = int;
   using value_type = unsigned;
   using reference = const unsigned&;
   using pointer = const unsigned*;
   using iterator_category = std::bidirectional_iterator_tag;

   PhysReg reg;

   PhysReg operator*() const { return reg; }

   PhysRegIterator& operator++()
   {
      reg.reg_b += 4;
      return *this;
   }

   PhysRegIterator& operator--()
   {
      reg.reg_b -= 4;
      return *this;
   }

   bool operator==(PhysRegIterator oth) const { return reg == oth.reg; }

   bool operator!=(PhysRegIterator oth) const { return reg != oth.reg; }

   bool operator<(PhysRegIterator oth) const { return reg < oth.reg; }
};

struct vector_info {
   vector_info() : is_weak(false), num_parts(0), parts(NULL) {}
   vector_info(Instruction* instr, unsigned start = 0, bool weak = false)
       : is_weak(weak), num_parts(instr->operands.size() - start),
         parts(instr->operands.begin() + start)
   {}

   /* If true, then we should stop trying to form a vector if anything goes wrong. Useful for when
    * the cost of failing does not introduce copies. */
   bool is_weak;
   uint32_t num_parts;
   Operand* parts;
};

struct ra_ctx {

   Program* program;
   Block* block = NULL;
   aco::monotonic_buffer_resource memory;
   std::vector<assignment> assignments;
   std::vector<aco::unordered_map<uint32_t, Temp>> renames;
   std::vector<uint32_t> loop_header;
   aco::unordered_map<uint32_t, Temp> orig_names;
   aco::unordered_map<uint32_t, vector_info> vectors;
   aco::unordered_map<uint32_t, Instruction*> split_vectors;
   aco_ptr<Instruction> pseudo_dummy;
   aco_ptr<Instruction> phi_dummy;
   uint16_t max_used_sgpr = 0;
   uint16_t max_used_vgpr = 0;
   uint16_t sgpr_limit;
   uint16_t vgpr_limit;
   std::bitset<512> war_hint;
   PhysRegIterator rr_sgpr_it;
   PhysRegIterator rr_vgpr_it;

   uint16_t sgpr_bounds;
   uint16_t vgpr_bounds;
   uint16_t num_linear_vgprs;

   ra_test_policy policy;

   ra_ctx(Program* program_, ra_test_policy policy_)
       : program(program_), assignments(program->peekAllocationId()),
         renames(program->blocks.size(), aco::unordered_map<uint32_t, Temp>(memory)),
         orig_names(memory), vectors(memory), split_vectors(memory), policy(policy_)
   {
      pseudo_dummy.reset(create_instruction(aco_opcode::p_parallelcopy, Format::PSEUDO, 0, 0));
      phi_dummy.reset(create_instruction(aco_opcode::p_linear_phi, Format::PSEUDO, 0, 0));
      sgpr_limit = get_addr_sgpr_from_waves(program, program->min_waves);
      vgpr_limit = get_addr_vgpr_from_waves(program, program->min_waves);

      sgpr_bounds = program->max_reg_demand.sgpr;
      vgpr_bounds = program->max_reg_demand.vgpr;
      num_linear_vgprs = 0;
   }
};

/* Half-open register interval used in "sliding window"-style for-loops */
struct PhysRegInterval {
   PhysReg lo_;
   unsigned size;

   /* Inclusive lower bound */
   PhysReg lo() const { return lo_; }

   /* Exclusive upper bound */
   PhysReg hi() const { return PhysReg{lo() + size}; }

   PhysRegInterval& operator+=(uint32_t stride)
   {
      lo_ = PhysReg{lo_.reg() + stride};
      return *this;
   }

   bool operator!=(const PhysRegInterval& oth) const { return lo_ != oth.lo_ || size != oth.size; }

   /* Construct a half-open interval, excluding the end register */
   static PhysRegInterval from_until(PhysReg first, PhysReg end) { return {first, end - first}; }

   bool contains(PhysReg reg) const { return lo() <= reg && reg < hi(); }

   bool contains(const PhysRegInterval& needle) const
   {
      return needle.lo() >= lo() && needle.hi() <= hi();
   }

   PhysRegIterator begin() const { return {lo_}; }

   PhysRegIterator end() const { return {PhysReg{lo_ + size}}; }
};

bool
intersects(const PhysRegInterval& a, const PhysRegInterval& b)
{
   return a.hi() > b.lo() && b.hi() > a.lo();
}

/* Gets the stride for full (non-subdword) registers */
uint32_t
get_stride(RegClass rc)
{
   if (rc.type() == RegType::vgpr) {
      return 1;
   } else {
      uint32_t size = rc.size();
      if (size == 2) {
         return 2;
      } else if (size >= 4) {
         return 4;
      } else {
         return 1;
      }
   }
}

PhysRegInterval
get_reg_bounds(ra_ctx& ctx, RegType type, bool linear_vgpr)
{
   uint16_t linear_vgpr_start = ctx.vgpr_bounds - ctx.num_linear_vgprs;
   if (type == RegType::vgpr && linear_vgpr) {
      return PhysRegInterval{PhysReg(256 + linear_vgpr_start), ctx.num_linear_vgprs};
   } else if (type == RegType::vgpr) {
      return PhysRegInterval{PhysReg(256), linear_vgpr_start};
   } else {
      return PhysRegInterval{PhysReg(0), ctx.sgpr_bounds};
   }
}

PhysRegInterval
get_reg_bounds(ra_ctx& ctx, RegClass rc)
{
   return get_reg_bounds(ctx, rc.type(), rc.is_linear_vgpr());
}

struct DefInfo {
   PhysRegInterval bounds;
   uint8_t size;
   uint8_t stride;
   /* Even if stride=4, we might be able to write to the high half instead without preserving the
    * low half. In that case, data_stride=2. */
   uint8_t data_stride;
   RegClass rc;

   DefInfo(ra_ctx& ctx, aco_ptr<Instruction>& instr, RegClass rc_, int operand) : rc(rc_)
   {
      size = rc.size();
      stride = get_stride(rc);
      data_stride = 0;

      bounds = get_reg_bounds(ctx, rc);

      if (rc.is_subdword() && operand >= 0) {
         /* stride in bytes */
         stride = get_subdword_operand_stride(ctx.program->gfx_level, instr, operand, rc);
      } else if (rc.is_subdword()) {
         get_subdword_definition_info(ctx.program, instr);
      } else if (instr->isMIMG() && instr->mimg().d16 && ctx.program->gfx_level <= GFX9) {
         /* Workaround GFX9 hardware bug for D16 image instructions: FeatureImageGather4D16Bug
          *
          * The register use is not calculated correctly, and the hardware assumes a
          * full dword per component. Don't use the last registers of the register file.
          * Otherwise, the instruction will be skipped.
          *
          * https://reviews.llvm.org/D81172
          */
         bool imageGather4D16Bug = operand == -1 && rc == v2 && instr->mimg().dmask != 0xF;
         assert(ctx.program->gfx_level == GFX9 && "Image D16 on GFX8 not supported.");

         if (imageGather4D16Bug)
            bounds.size -= MAX2(rc.bytes() / 4 - ctx.num_linear_vgprs, 0);
      }

      if (!data_stride)
         data_stride = rc.is_subdword() ? stride : (stride * 4);
   }

private:
   void get_subdword_definition_info(Program* program, const aco_ptr<Instruction>& instr);
};

class RegisterFile {
public:
   RegisterFile() { regs.fill(0); }

   std::array<uint32_t, 512> regs;
   std::map<uint32_t, std::array<uint32_t, 4>> subdword_regs;

   const uint32_t& operator[](PhysReg index) const { return regs[index]; }

   uint32_t& operator[](PhysReg index) { return regs[index]; }

   unsigned count_zero(PhysRegInterval reg_interval) const
   {
      unsigned res = 0;
      for (PhysReg reg : reg_interval)
         res += !regs[reg];
      return res;
   }

   /* Returns true if any of the bytes in the given range are allocated or blocked */
   bool test(PhysReg start, unsigned num_bytes) const
   {
      for (PhysReg i = start; i.reg_b < start.reg_b + num_bytes; i = PhysReg(i + 1)) {
         assert(i <= 511);
         if (regs[i] & 0x0FFFFFFF)
            return true;
         if (regs[i] == 0xF0000000) {
            auto it = subdword_regs.find(i);
            assert(it != subdword_regs.end());
            for (unsigned j = i.byte(); i * 4 + j < start.reg_b + num_bytes && j < 4; j++) {
               if (it->second[j])
                  return true;
            }
         }
      }
      return false;
   }

   void block(PhysReg start, RegClass rc)
   {
      if (rc.is_subdword())
         fill_subdword(start, rc.bytes(), 0xFFFFFFFF);
      else
         fill(start, rc.size(), 0xFFFFFFFF);
   }

   bool is_blocked(PhysReg start) const
   {
      if (regs[start] == 0xFFFFFFFF)
         return true;
      if (regs[start] == 0xF0000000) {
         auto it = subdword_regs.find(start);
         assert(it != subdword_regs.end());
         for (unsigned i = start.byte(); i < 4; i++)
            if (it->second[i] == 0xFFFFFFFF)
               return true;
      }
      return false;
   }

   bool is_empty_or_blocked(PhysReg start) const
   {
      /* Empty is 0, blocked is 0xFFFFFFFF, so to check both we compare the
       * incremented value to 1 */
      if (regs[start] == 0xF0000000) {
         auto it = subdword_regs.find(start);
         assert(it != subdword_regs.end());
         return it->second[start.byte()] + 1 <= 1;
      }
      return regs[start] + 1 <= 1;
   }

   void clear(PhysReg start, RegClass rc)
   {
      if (rc.is_subdword())
         fill_subdword(start, rc.bytes(), 0);
      else
         fill(start, rc.size(), 0);
   }

   void fill_killed_operands(Instruction* instr)
   {
      for (Operand& op : instr->operands) {
         if (op.isFixed() && op.isFirstKillBeforeDef()) {
            if (op.regClass().is_subdword())
               fill_subdword(op.physReg(), op.bytes(), op.tempId());
            else
               fill(op.physReg(), op.size(), op.tempId());
         }
      }
   }

   void clear(Operand op) { clear(op.physReg(), op.regClass()); }

   void fill(Definition def)
   {
      if (def.regClass().is_subdword())
         fill_subdword(def.physReg(), def.bytes(), def.tempId());
      else
         fill(def.physReg(), def.size(), def.tempId());
   }

   void clear(Definition def) { clear(def.physReg(), def.regClass()); }

   unsigned get_id(PhysReg reg) const
   {
      return regs[reg] == 0xF0000000 ? subdword_regs.at(reg)[reg.byte()] : regs[reg];
   }

private:
   void fill(PhysReg start, unsigned size, uint32_t val)
   {
      for (unsigned i = 0; i < size; i++)
         regs[start + i] = val;
   }

   void fill_subdword(PhysReg start, unsigned num_bytes, uint32_t val)
   {
      fill(start, DIV_ROUND_UP(num_bytes, 4), 0xF0000000);
      for (PhysReg i = start; i.reg_b < start.reg_b + num_bytes; i = PhysReg(i + 1)) {
         /* emplace or get */
         std::array<uint32_t, 4>& sub =
            subdword_regs.emplace(i, std::array<uint32_t, 4>{0, 0, 0, 0}).first->second;
         for (unsigned j = i.byte(); i * 4 + j < start.reg_b + num_bytes && j < 4; j++)
            sub[j] = val;

         if (sub == std::array<uint32_t, 4>{0, 0, 0, 0}) {
            subdword_regs.erase(i);
            regs[i] = 0;
         }
      }
   }
};

std::vector<unsigned> find_vars(ra_ctx& ctx, const RegisterFile& reg_file,
                                const PhysRegInterval reg_interval);

/* helper function for debugging */
UNUSED void
print_reg(const RegisterFile& reg_file, PhysReg reg, bool has_adjacent_variable)
{
   if (reg_file[reg] == 0xFFFFFFFF) {
      printf((const char*)u8"☐");
   } else if (reg_file[reg]) {
      const bool show_subdword_alloc = (reg_file[reg] == 0xF0000000);
      if (show_subdword_alloc) {
         auto block_chars = {
            // clang-format off
            u8"?", u8"▘", u8"▝", u8"▀",
            u8"▖", u8"▌", u8"▞", u8"▛",
            u8"▗", u8"▚", u8"▐", u8"▜",
            u8"▄", u8"▙", u8"▟", u8"▉"
            // clang-format on
         };
         unsigned index = 0;
         for (int i = 0; i < 4; ++i) {
            if (reg_file.subdword_regs.at(reg)[i]) {
               index |= 1 << i;
            }
         }
         printf("%s", (const char*)(block_chars.begin()[index]));
      } else {
         /* Indicate filled register slot */
         if (!has_adjacent_variable) {
            printf((const char*)u8"█");
         } else {
            /* Use a slightly shorter box to leave a small gap between adjacent variables */
            printf((const char*)u8"▉");
         }
      }
   } else {
      printf((const char*)u8"·");
   }
}

/* helper function for debugging */
UNUSED void
print_regs(ra_ctx& ctx, PhysRegInterval regs, const RegisterFile& reg_file)
{
   char reg_char = regs.lo().reg() >= 256 ? 'v' : 's';
   const int max_regs_per_line = 64;

   /* print markers */
   printf("       ");
   for (int i = 0; i < std::min<int>(max_regs_per_line, ROUND_DOWN_TO(regs.size, 4)); i += 4) {
      printf("%-3.2u ", i);
   }
   printf("\n");

   /* print usage */
   auto line_begin_it = regs.begin();
   while (line_begin_it != regs.end()) {
      const int regs_in_line =
         std::min<int>(max_regs_per_line, std::distance(line_begin_it, regs.end()));

      if (line_begin_it == regs.begin()) {
         printf("%cgprs: ", reg_char);
      } else {
         printf("  %+4d ", std::distance(regs.begin(), line_begin_it));
      }
      const auto line_end_it = std::next(line_begin_it, regs_in_line);

      for (auto reg_it = line_begin_it; reg_it != line_end_it; ++reg_it) {
         bool has_adjacent_variable =
            (std::next(reg_it) != line_end_it &&
             reg_file[*reg_it] != reg_file[*std::next(reg_it)] && reg_file[*std::next(reg_it)]);
         print_reg(reg_file, *reg_it, has_adjacent_variable);
      }

      line_begin_it = line_end_it;
      printf("\n");
   }

   const unsigned free_regs =
      std::count_if(regs.begin(), regs.end(), [&](auto reg) { return !reg_file[reg]; });
   printf("%u/%u used, %u/%u free\n", regs.size - free_regs, regs.size, free_regs, regs.size);

   /* print assignments ordered by registers */
   std::map<PhysReg, std::pair<unsigned, unsigned>> regs_to_vars; /* maps to byte size and temp id */
   for (unsigned id : find_vars(ctx, reg_file, regs)) {
      const assignment& var = ctx.assignments[id];
      PhysReg reg = var.reg;
      ASSERTED auto inserted = regs_to_vars.emplace(reg, std::make_pair(var.rc.bytes(), id));
      assert(inserted.second);
   }

   for (const auto& reg_and_var : regs_to_vars) {
      const auto& first_reg = reg_and_var.first;
      const auto& size_id = reg_and_var.second;

      printf("%%%u ", size_id.second);
      if (ctx.orig_names.count(size_id.second) &&
          ctx.orig_names[size_id.second].id() != size_id.second) {
         printf("(was %%%d) ", ctx.orig_names[size_id.second].id());
      }
      printf("= %c[%d", reg_char, first_reg.reg() % 256);
      PhysReg last_reg = first_reg.advance(size_id.first - 1);
      if (first_reg.reg() != last_reg.reg()) {
         assert(first_reg.byte() == 0 && last_reg.byte() == 3);
         printf("-%d", last_reg.reg() % 256);
      }
      printf("]");
      if (first_reg.byte() != 0 || last_reg.byte() != 3) {
         printf("[%d:%d]", first_reg.byte() * 8, (last_reg.byte() + 1) * 8);
      }
      printf("\n");
   }
}

unsigned
get_subdword_operand_stride(amd_gfx_level gfx_level, const aco_ptr<Instruction>& instr,
                            unsigned idx, RegClass rc)
{
   assert(gfx_level >= GFX8);
   if (instr->isPseudo()) {
      /* v_readfirstlane_b32 cannot use SDWA */
      if (instr->opcode == aco_opcode::p_as_uniform)
         return 4;
      else
         return rc.bytes() % 2 == 0 ? 2 : 1;
   }

   assert(rc.bytes() <= 2);
   if (instr->isVALU()) {
      if (can_use_SDWA(gfx_level, instr, false))
         return rc.bytes();
      if (can_use_opsel(gfx_level, instr->opcode, idx))
         return 2;
      if (instr->isVOP3P())
         return 2;
   }

   switch (instr->opcode) {
   case aco_opcode::v_cvt_f32_ubyte0: return 1;
   case aco_opcode::ds_write_b8:
   case aco_opcode::ds_write_b16: return gfx_level >= GFX9 ? 2 : 4;
   case aco_opcode::buffer_store_byte:
   case aco_opcode::buffer_store_short:
   case aco_opcode::buffer_store_format_d16_x:
   case aco_opcode::flat_store_byte:
   case aco_opcode::flat_store_short:
   case aco_opcode::scratch_store_byte:
   case aco_opcode::scratch_store_short:
   case aco_opcode::global_store_byte:
   case aco_opcode::global_store_short: return gfx_level >= GFX9 ? 2 : 4;
   default: return 4;
   }
}

void
add_subdword_operand(ra_ctx& ctx, aco_ptr<Instruction>& instr, unsigned idx, unsigned byte,
                     RegClass rc)
{
   amd_gfx_level gfx_level = ctx.program->gfx_level;
   if (instr->isPseudo() || byte == 0)
      return;

   assert(rc.bytes() <= 2);
   if (instr->isVALU()) {
      if (instr->opcode == aco_opcode::v_cvt_f32_ubyte0) {
         switch (byte) {
         case 0: instr->opcode = aco_opcode::v_cvt_f32_ubyte0; break;
         case 1: instr->opcode = aco_opcode::v_cvt_f32_ubyte1; break;
         case 2: instr->opcode = aco_opcode::v_cvt_f32_ubyte2; break;
         case 3: instr->opcode = aco_opcode::v_cvt_f32_ubyte3; break;
         }
         return;
      }

      /* use SDWA */
      if (can_use_SDWA(gfx_level, instr, false)) {
         convert_to_SDWA(gfx_level, instr);
         return;
      }

      /* use opsel */
      if (instr->isVOP3P()) {
         assert(byte == 2 && !instr->valu().opsel_lo[idx]);
         instr->valu().opsel_lo[idx] = true;
         instr->valu().opsel_hi[idx] = true;
         return;
      }

      assert(can_use_opsel(gfx_level, instr->opcode, idx));
      instr->valu().opsel[idx] = true;
      return;
   }

   assert(byte == 2);
   if (instr->opcode == aco_opcode::ds_write_b8)
      instr->opcode = aco_opcode::ds_write_b8_d16_hi;
   else if (instr->opcode == aco_opcode::ds_write_b16)
      instr->opcode = aco_opcode::ds_write_b16_d16_hi;
   else if (instr->opcode == aco_opcode::buffer_store_byte)
      instr->opcode = aco_opcode::buffer_store_byte_d16_hi;
   else if (instr->opcode == aco_opcode::buffer_store_short)
      instr->opcode = aco_opcode::buffer_store_short_d16_hi;
   else if (instr->opcode == aco_opcode::buffer_store_format_d16_x)
      instr->opcode = aco_opcode::buffer_store_format_d16_hi_x;
   else if (instr->opcode == aco_opcode::flat_store_byte)
      instr->opcode = aco_opcode::flat_store_byte_d16_hi;
   else if (instr->opcode == aco_opcode::flat_store_short)
      instr->opcode = aco_opcode::flat_store_short_d16_hi;
   else if (instr->opcode == aco_opcode::scratch_store_byte)
      instr->opcode = aco_opcode::scratch_store_byte_d16_hi;
   else if (instr->opcode == aco_opcode::scratch_store_short)
      instr->opcode = aco_opcode::scratch_store_short_d16_hi;
   else if (instr->opcode == aco_opcode::global_store_byte)
      instr->opcode = aco_opcode::global_store_byte_d16_hi;
   else if (instr->opcode == aco_opcode::global_store_short)
      instr->opcode = aco_opcode::global_store_short_d16_hi;
   else
      unreachable("Something went wrong: Impossible register assignment.");
   return;
}

void
DefInfo::get_subdword_definition_info(Program* program, const aco_ptr<Instruction>& instr)
{
   amd_gfx_level gfx_level = program->gfx_level;
   assert(gfx_level >= GFX8);

   stride = rc.bytes() % 2 == 0 ? 2 : 1;

   if (instr->isPseudo()) {
      if (instr->opcode == aco_opcode::p_interp_gfx11) {
         rc = RegClass(RegType::vgpr, rc.size());
         stride = 1;
      }
      return;
   }

   if (instr->isVALU()) {
      assert(rc.bytes() <= 2);

      if (can_use_SDWA(gfx_level, instr, false) || instr->opcode == aco_opcode::p_v_cvt_pk_u8_f32)
         return;

      rc = instr_is_16bit(gfx_level, instr->opcode) ? v2b : v1;
      stride = rc == v2b ? 4 : 1;
      if (instr->opcode == aco_opcode::v_fma_mixlo_f16 ||
          can_use_opsel(gfx_level, instr->opcode, -1)) {
         data_stride = 2;
         stride = rc == v2b ? 2 : stride;
      }
      return;
   }

   switch (instr->opcode) {
   case aco_opcode::v_interp_p2_f16: return;
   /* D16 loads with _hi version */
   case aco_opcode::ds_read_u8_d16:
   case aco_opcode::ds_read_i8_d16:
   case aco_opcode::ds_read_u16_d16:
   case aco_opcode::flat_load_ubyte_d16:
   case aco_opcode::flat_load_sbyte_d16:
   case aco_opcode::flat_load_short_d16:
   case aco_opcode::global_load_ubyte_d16:
   case aco_opcode::global_load_sbyte_d16:
   case aco_opcode::global_load_short_d16:
   case aco_opcode::scratch_load_ubyte_d16:
   case aco_opcode::scratch_load_sbyte_d16:
   case aco_opcode::scratch_load_short_d16:
   case aco_opcode::buffer_load_ubyte_d16:
   case aco_opcode::buffer_load_sbyte_d16:
   case aco_opcode::buffer_load_short_d16:
   case aco_opcode::buffer_load_format_d16_x: {
      assert(gfx_level >= GFX9);
      if (program->dev.sram_ecc_enabled) {
         rc = v1;
         stride = 1;
         data_stride = 2;
      } else {
         stride = 2;
      }
      return;
   }
   /* 3-component D16 loads */
   case aco_opcode::buffer_load_format_d16_xyz:
   case aco_opcode::tbuffer_load_format_d16_xyz: {
      assert(gfx_level >= GFX9);
      if (program->dev.sram_ecc_enabled) {
         rc = v2;
         stride = 1;
      } else {
         stride = 4;
      }
      return;
   }
   default: break;
   }

   if (instr->isMIMG() && instr->mimg().d16 && !program->dev.sram_ecc_enabled) {
      assert(gfx_level >= GFX9);
      stride = 4;
   } else {
      rc = RegClass(RegType::vgpr, rc.size());
      stride = 1;
   }
}

void
add_subdword_definition(Program* program, aco_ptr<Instruction>& instr, PhysReg reg,
                        bool allow_16bit_write)
{
   if (instr->isPseudo())
      return;

   if (instr->isVALU()) {
      amd_gfx_level gfx_level = program->gfx_level;
      assert(instr->definitions[0].bytes() <= 2);

      if (instr->opcode == aco_opcode::p_v_cvt_pk_u8_f32)
         return;

      if (reg.byte() == 0 && allow_16bit_write && instr_is_16bit(gfx_level, instr->opcode))
         return;

      /* use SDWA */
      if (can_use_SDWA(gfx_level, instr, false)) {
         convert_to_SDWA(gfx_level, instr);
         return;
      }

      assert(allow_16bit_write);

      if (instr->opcode == aco_opcode::v_fma_mixlo_f16) {
         instr->opcode = aco_opcode::v_fma_mixhi_f16;
         return;
      }

      /* use opsel */
      assert(reg.byte() == 2);
      assert(can_use_opsel(gfx_level, instr->opcode, -1));
      instr->valu().opsel[3] = true; /* dst in high half */
      return;
   }

   if (reg.byte() == 0)
      return;
   else if (instr->opcode == aco_opcode::v_interp_p2_f16)
      instr->opcode = aco_opcode::v_interp_p2_hi_f16;
   else if (instr->opcode == aco_opcode::buffer_load_ubyte_d16)
      instr->opcode = aco_opcode::buffer_load_ubyte_d16_hi;
   else if (instr->opcode == aco_opcode::buffer_load_sbyte_d16)
      instr->opcode = aco_opcode::buffer_load_sbyte_d16_hi;
   else if (instr->opcode == aco_opcode::buffer_load_short_d16)
      instr->opcode = aco_opcode::buffer_load_short_d16_hi;
   else if (instr->opcode == aco_opcode::buffer_load_format_d16_x)
      instr->opcode = aco_opcode::buffer_load_format_d16_hi_x;
   else if (instr->opcode == aco_opcode::flat_load_ubyte_d16)
      instr->opcode = aco_opcode::flat_load_ubyte_d16_hi;
   else if (instr->opcode == aco_opcode::flat_load_sbyte_d16)
      instr->opcode = aco_opcode::flat_load_sbyte_d16_hi;
   else if (instr->opcode == aco_opcode::flat_load_short_d16)
      instr->opcode = aco_opcode::flat_load_short_d16_hi;
   else if (instr->opcode == aco_opcode::scratch_load_ubyte_d16)
      instr->opcode = aco_opcode::scratch_load_ubyte_d16_hi;
   else if (instr->opcode == aco_opcode::scratch_load_sbyte_d16)
      instr->opcode = aco_opcode::scratch_load_sbyte_d16_hi;
   else if (instr->opcode == aco_opcode::scratch_load_short_d16)
      instr->opcode = aco_opcode::scratch_load_short_d16_hi;
   else if (instr->opcode == aco_opcode::global_load_ubyte_d16)
      instr->opcode = aco_opcode::global_load_ubyte_d16_hi;
   else if (instr->opcode == aco_opcode::global_load_sbyte_d16)
      instr->opcode = aco_opcode::global_load_sbyte_d16_hi;
   else if (instr->opcode == aco_opcode::global_load_short_d16)
      instr->opcode = aco_opcode::global_load_short_d16_hi;
   else if (instr->opcode == aco_opcode::ds_read_u8_d16)
      instr->opcode = aco_opcode::ds_read_u8_d16_hi;
   else if (instr->opcode == aco_opcode::ds_read_i8_d16)
      instr->opcode = aco_opcode::ds_read_i8_d16_hi;
   else if (instr->opcode == aco_opcode::ds_read_u16_d16)
      instr->opcode = aco_opcode::ds_read_u16_d16_hi;
   else
      unreachable("Something went wrong: Impossible register assignment.");
}

void
adjust_max_used_regs(ra_ctx& ctx, RegClass rc, unsigned reg)
{
   uint16_t max_addressible_sgpr = ctx.sgpr_limit;
   unsigned size = rc.size();
   if (rc.type() == RegType::vgpr) {
      assert(reg >= 256);
      uint16_t hi = reg - 256 + size - 1;
      assert(hi <= 255);
      ctx.max_used_vgpr = std::max(ctx.max_used_vgpr, hi);
   } else if (reg + rc.size() <= max_addressible_sgpr) {
      uint16_t hi = reg + size - 1;
      ctx.max_used_sgpr = std::max(ctx.max_used_sgpr, std::min(hi, max_addressible_sgpr));
   }
}

enum UpdateRenames {
   rename_not_killed_ops = 0x1,
   rename_precolored_ops = 0x4,
};
MESA_DEFINE_CPP_ENUM_BITFIELD_OPERATORS(UpdateRenames);

void
update_renames(ra_ctx& ctx, RegisterFile& reg_file,
               std::vector<std::pair<Operand, Definition>>& parallelcopies,
               aco_ptr<Instruction>& instr, UpdateRenames flags)
{
   /* clear operands */
   for (std::pair<Operand, Definition>& copy : parallelcopies) {
      /* the definitions with id are not from this function and already handled */
      if (copy.second.isTemp())
         continue;
      reg_file.clear(copy.first);
   }

   /* allocate id's and rename operands: this is done transparently here */
   auto it = parallelcopies.begin();
   while (it != parallelcopies.end()) {
      if (it->second.isTemp()) {
         ++it;
         continue;
      }

      /* check if we moved a definition: change the register and remove copy */
      bool is_def = false;
      for (Definition& def : instr->definitions) {
         if (def.isTemp() && def.getTemp() == it->first.getTemp()) {
            // FIXME: ensure that the definition can use this reg
            def.setFixed(it->second.physReg());
            reg_file.fill(def);
            ctx.assignments[def.tempId()].reg = def.physReg();
            it = parallelcopies.erase(it);
            is_def = true;
            break;
         }
      }
      if (is_def)
         continue;

      /* check if we moved another parallelcopy definition */
      for (std::pair<Operand, Definition>& other : parallelcopies) {
         if (!other.second.isTemp())
            continue;
         if (it->first.getTemp() == other.second.getTemp()) {
            other.second.setFixed(it->second.physReg());
            ctx.assignments[other.second.tempId()].reg = other.second.physReg();
            it = parallelcopies.erase(it);
            is_def = true;
            /* check if we moved an operand, again */
            bool fill = true;
            for (Operand& op : instr->operands) {
               if (op.isTemp() && op.tempId() == other.second.tempId()) {
                  // FIXME: ensure that the operand can use this reg
                  op.setFixed(other.second.physReg());
                  fill = !op.isKillBeforeDef();
               }
            }
            if (fill)
               reg_file.fill(other.second);
            break;
         }
      }
      if (is_def)
         continue;

      std::pair<Operand, Definition>& copy = *it;
      copy.second.setTemp(ctx.program->allocateTmp(copy.second.regClass()));
      ctx.assignments.emplace_back(copy.second.physReg(), copy.second.regClass());
      assert(ctx.assignments.size() == ctx.program->peekAllocationId());

      /* check if we moved an operand */
      bool first[2] = {true, true};
      bool fill = true;
      for (unsigned i = 0; i < instr->operands.size(); i++) {
         Operand& op = instr->operands[i];
         if (!op.isTemp())
            continue;
         if (op.tempId() == copy.first.tempId()) {
            /* only rename precolored operands if the copy-location matches */
            bool omit_renaming = (flags & rename_precolored_ops) && op.isFixed() &&
                                 op.physReg() != copy.second.physReg();

            /* Omit renaming in some cases for p_create_vector in order to avoid
             * unnecessary shuffle code. */
            if (!(flags & rename_not_killed_ops) && !op.isKillBeforeDef()) {
               omit_renaming = true;
               for (std::pair<Operand, Definition>& pc : parallelcopies) {
                  PhysReg def_reg = pc.second.physReg();
                  omit_renaming &= def_reg > copy.first.physReg()
                                      ? (copy.first.physReg() + copy.first.size() <= def_reg.reg())
                                      : (def_reg + pc.second.size() <= copy.first.physReg().reg());
               }
            }

            /* Fix the kill flags */
            if (first[omit_renaming])
               op.setFirstKill(omit_renaming || op.isKill());
            else
               op.setKill(omit_renaming || op.isKill());
            first[omit_renaming] = false;

            if (omit_renaming)
               continue;

            op.setTemp(copy.second.getTemp());
            op.setFixed(copy.second.physReg());

            fill = !op.isKillBeforeDef() || (flags & rename_precolored_ops);
         }
      }

      /* Apply changes to register file. */
      if (fill)
         reg_file.fill(copy.second);

      ++it;
   }
}

std::optional<PhysReg>
get_reg_simple(ra_ctx& ctx, const RegisterFile& reg_file, DefInfo info)
{
   PhysRegInterval bounds = info.bounds;
   uint32_t size = info.size;
   uint32_t stride = info.rc.is_subdword() ? DIV_ROUND_UP(info.stride, 4) : info.stride;
   RegClass rc = info.rc;

   if (stride < size && !rc.is_subdword()) {
      DefInfo new_info = info;
      new_info.stride = stride * 2;
      if (size % new_info.stride == 0) {
         std::optional<PhysReg> res = get_reg_simple(ctx, reg_file, new_info);
         if (res)
            return res;
      }
   }

   PhysRegIterator& rr_it = rc.type() == RegType::vgpr ? ctx.rr_vgpr_it : ctx.rr_sgpr_it;
   if (stride == 1) {
      if (rr_it != bounds.begin() && bounds.contains(rr_it.reg)) {
         assert(bounds.begin() < rr_it);
         assert(rr_it < bounds.end());
         info.bounds = PhysRegInterval::from_until(rr_it.reg, bounds.hi());
         std::optional<PhysReg> res = get_reg_simple(ctx, reg_file, info);
         if (res)
            return res;
         bounds = PhysRegInterval::from_until(bounds.lo(), rr_it.reg);
      }
   }

   auto is_free = [&](PhysReg reg_index)
   { return reg_file[reg_index] == 0 && !ctx.war_hint[reg_index]; };

   for (PhysRegInterval reg_win = {bounds.lo(), size}; reg_win.hi() <= bounds.hi();
        reg_win += stride) {
      if (std::all_of(reg_win.begin(), reg_win.end(), is_free)) {
         if (stride == 1) {
            PhysRegIterator new_rr_it{PhysReg{reg_win.lo() + size}};
            if (new_rr_it < bounds.end())
               rr_it = new_rr_it;
         }
         adjust_max_used_regs(ctx, rc, reg_win.lo());
         return reg_win.lo();
      }
   }

   /* do this late because using the upper bytes of a register can require
    * larger instruction encodings or copies
    * TODO: don't do this in situations where it doesn't benefit */
   if (rc.is_subdword()) {
      for (const std::pair<const uint32_t, std::array<uint32_t, 4>>& entry :
           reg_file.subdword_regs) {
         assert(reg_file[PhysReg{entry.first}] == 0xF0000000);
         if (!bounds.contains({PhysReg{entry.first}, rc.size()}))
            continue;

         auto it = entry.second.begin();
         for (unsigned i = 0; i < 4; i += info.stride) {
            /* check if there's a block of free bytes large enough to hold the register */
            bool reg_found =
               std::all_of(std::next(it, i), std::next(it, std::min(4u, i + rc.bytes())),
                           [](unsigned v) { return v == 0; });

            /* check if also the neighboring reg is free if needed */
            if (reg_found && i + rc.bytes() > 4)
               reg_found = (reg_file[PhysReg{entry.first + 1}] == 0);

            if (reg_found) {
               PhysReg res{entry.first};
               res.reg_b += i;
               adjust_max_used_regs(ctx, rc, entry.first);
               return res;
            }
         }
      }
   }

   return {};
}

/* collect variables from a register area */
std::vector<unsigned>
find_vars(ra_ctx& ctx, const RegisterFile& reg_file, const PhysRegInterval reg_interval)
{
   std::vector<unsigned> vars;
   for (PhysReg j : reg_interval) {
      if (reg_file.is_blocked(j))
         continue;
      if (reg_file[j] == 0xF0000000) {
         for (unsigned k = 0; k < 4; k++) {
            unsigned id = reg_file.subdword_regs.at(j)[k];
            if (id && (vars.empty() || id != vars.back()))
               vars.emplace_back(id);
         }
      } else {
         unsigned id = reg_file[j];
         if (id && (vars.empty() || id != vars.back()))
            vars.emplace_back(id);
      }
   }
   return vars;
}

/* collect variables from a register area and clear reg_file
 * variables are sorted in decreasing size and
 * increasing assigned register
 */
std::vector<unsigned>
collect_vars(ra_ctx& ctx, RegisterFile& reg_file, const PhysRegInterval reg_interval)
{
   std::vector<unsigned> ids = find_vars(ctx, reg_file, reg_interval);
   std::sort(ids.begin(), ids.end(),
             [&](unsigned a, unsigned b)
             {
                assignment& var_a = ctx.assignments[a];
                assignment& var_b = ctx.assignments[b];
                return var_a.rc.bytes() > var_b.rc.bytes() ||
                       (var_a.rc.bytes() == var_b.rc.bytes() && var_a.reg < var_b.reg);
             });

   for (unsigned id : ids) {
      assignment& var = ctx.assignments[id];
      reg_file.clear(var.reg, var.rc);
   }
   return ids;
}

std::optional<PhysReg>
get_reg_for_create_vector_copy(ra_ctx& ctx, RegisterFile& reg_file,
                               std::vector<std::pair<Operand, Definition>>& parallelcopies,
                               aco_ptr<Instruction>& instr, const PhysRegInterval def_reg,
                               DefInfo info, unsigned id)
{
   PhysReg reg = def_reg.lo();
   /* dead operand: return position in vector */
   for (unsigned i = 0; i < instr->operands.size(); i++) {
      if (instr->operands[i].isTemp() && instr->operands[i].tempId() == id &&
          instr->operands[i].isKillBeforeDef()) {
         assert(!reg_file.test(reg, instr->operands[i].bytes()));
         if (info.rc.is_subdword() || reg.byte() == 0)
            return reg;
         else
            return {};
      }
      reg.reg_b += instr->operands[i].bytes();
   }

   /* GFX9+ has a VGPR swap instruction. */
   if (ctx.program->gfx_level <= GFX8 || info.rc.type() == RegType::sgpr)
      return {};

   /* check if the previous position was in vector */
   assignment& var = ctx.assignments[id];
   if (def_reg.contains(PhysRegInterval{var.reg, info.size})) {
      reg = def_reg.lo();
      /* try to use the previous register of the operand */
      for (unsigned i = 0; i < instr->operands.size(); i++) {
         if (reg != var.reg) {
            reg.reg_b += instr->operands[i].bytes();
            continue;
         }

         /* check if we can swap positions */
         if (instr->operands[i].isTemp() && instr->operands[i].isFirstKill() &&
             instr->operands[i].regClass() == info.rc) {
            assignment& op = ctx.assignments[instr->operands[i].tempId()];
            /* if everything matches, create parallelcopy for the killed operand */
            if (!intersects(def_reg, PhysRegInterval{op.reg, op.rc.size()}) && op.reg != scc &&
                reg_file.get_id(op.reg) == instr->operands[i].tempId()) {
               Definition pc_def = Definition(reg, info.rc);
               parallelcopies.emplace_back(instr->operands[i], pc_def);
               return op.reg;
            }
         }
         return {};
      }
   }
   return {};
}

bool
get_regs_for_copies(ra_ctx& ctx, RegisterFile& reg_file,
                    std::vector<std::pair<Operand, Definition>>& parallelcopies,
                    const std::vector<unsigned>& vars, aco_ptr<Instruction>& instr,
                    const PhysRegInterval def_reg)
{
   /* Variables are sorted from large to small and with increasing assigned register */
   for (unsigned id : vars) {
      assignment& var = ctx.assignments[id];
      PhysRegInterval bounds = get_reg_bounds(ctx, var.rc);
      DefInfo info = DefInfo(ctx, ctx.pseudo_dummy, var.rc, -1);
      uint32_t size = info.size;

      /* check if this is a dead operand, then we can re-use the space from the definition
       * also use the correct stride for sub-dword operands */
      bool is_dead_operand = false;
      std::optional<PhysReg> res;
      if (instr->opcode == aco_opcode::p_create_vector) {
         res =
            get_reg_for_create_vector_copy(ctx, reg_file, parallelcopies, instr, def_reg, info, id);
      } else {
         for (unsigned i = 0; !is_phi(instr) && i < instr->operands.size(); i++) {
            if (instr->operands[i].isTemp() && instr->operands[i].tempId() == id) {
               info = DefInfo(ctx, instr, var.rc, i);
               if (instr->operands[i].isKillBeforeDef()) {
                  info.bounds = def_reg;
                  res = get_reg_simple(ctx, reg_file, info);
                  is_dead_operand = true;
               }
               break;
            }
         }
      }
      if (!res && !def_reg.size) {
         /* If this is before definitions are handled, def_reg may be an empty interval. */
         info.bounds = bounds;
         res = get_reg_simple(ctx, reg_file, info);
      } else if (!res) {
         /* Try to find space within the bounds but outside of the definition */
         info.bounds = PhysRegInterval::from_until(bounds.lo(), MIN2(def_reg.lo(), bounds.hi()));
         res = get_reg_simple(ctx, reg_file, info);
         if (!res && def_reg.hi() <= bounds.hi()) {
            unsigned lo = (def_reg.hi() + info.stride - 1) & ~(info.stride - 1);
            info.bounds = PhysRegInterval::from_until(PhysReg{lo}, bounds.hi());
            res = get_reg_simple(ctx, reg_file, info);
         }
      }

      if (res) {
         /* mark the area as blocked */
         reg_file.block(*res, var.rc);

         /* create parallelcopy pair (without definition id) */
         Temp tmp = Temp(id, var.rc);
         Operand pc_op = Operand(tmp);
         pc_op.setFixed(var.reg);
         Definition pc_def = Definition(*res, pc_op.regClass());
         parallelcopies.emplace_back(pc_op, pc_def);
         continue;
      }

      PhysReg best_pos = bounds.lo();
      unsigned num_moves = 0xFF;
      unsigned num_vars = 0;

      /* we use a sliding window to find potential positions */
      unsigned stride = var.rc.is_subdword() ? 1 : info.stride;
      for (PhysRegInterval reg_win{bounds.lo(), size}; reg_win.hi() <= bounds.hi();
           reg_win += stride) {
         if (!is_dead_operand && intersects(reg_win, def_reg))
            continue;

         /* second, check that we have at most k=num_moves elements in the window
          * and no element is larger than the currently processed one */
         unsigned k = 0;
         unsigned n = 0;
         unsigned last_var = 0;
         bool found = true;
         for (PhysReg j : reg_win) {
            if (reg_file[j] == 0 || reg_file[j] == last_var)
               continue;

            if (reg_file.is_blocked(j) || k > num_moves) {
               found = false;
               break;
            }
            if (reg_file[j] == 0xF0000000) {
               k += 1;
               n++;
               continue;
            }
            /* we cannot split live ranges of linear vgprs */
            if (ctx.assignments[reg_file[j]].rc.is_linear_vgpr()) {
               found = false;
               break;
            }
            bool is_kill = false;
            for (const Operand& op : instr->operands) {
               if (op.isTemp() && op.isKillBeforeDef() && op.tempId() == reg_file[j]) {
                  is_kill = true;
                  break;
               }
            }
            if (!is_kill && ctx.assignments[reg_file[j]].rc.size() >= size) {
               found = false;
               break;
            }

            k += ctx.assignments[reg_file[j]].rc.size();
            last_var = reg_file[j];
            n++;
            if (k > num_moves || (k == num_moves && n <= num_vars)) {
               found = false;
               break;
            }
         }

         if (found) {
            best_pos = reg_win.lo();
            num_moves = k;
            num_vars = n;
         }
      }

      /* FIXME: we messed up and couldn't find space for the variables to be copied */
      if (num_moves == 0xFF)
         return false;

      PhysRegInterval reg_win{best_pos, size};

      /* collect variables and block reg file */
      std::vector<unsigned> new_vars = collect_vars(ctx, reg_file, reg_win);

      /* mark the area as blocked */
      reg_file.block(reg_win.lo(), var.rc);
      adjust_max_used_regs(ctx, var.rc, reg_win.lo());

      if (!get_regs_for_copies(ctx, reg_file, parallelcopies, new_vars, instr, def_reg))
         return false;

      /* create parallelcopy pair (without definition id) */
      Temp tmp = Temp(id, var.rc);
      Operand pc_op = Operand(tmp);
      pc_op.setFixed(var.reg);
      Definition pc_def = Definition(reg_win.lo(), pc_op.regClass());
      parallelcopies.emplace_back(pc_op, pc_def);
   }

   return true;
}

std::optional<PhysReg>
get_reg_impl(ra_ctx& ctx, const RegisterFile& reg_file,
             std::vector<std::pair<Operand, Definition>>& parallelcopies, const DefInfo& info,
             aco_ptr<Instruction>& instr)
{
   const PhysRegInterval& bounds = info.bounds;
   uint32_t size = info.size;
   uint32_t stride = info.stride;
   RegClass rc = info.rc;

   /* check how many free regs we have */
   unsigned regs_free = reg_file.count_zero(bounds);

   /* mark and count killed operands */
   unsigned killed_ops = 0;
   std::bitset<256> is_killed_operand; /* per-register */
   for (unsigned j = 0; !is_phi(instr) && j < instr->operands.size(); j++) {
      Operand& op = instr->operands[j];
      if (op.isTemp() && op.isFirstKillBeforeDef() && bounds.contains(op.physReg()) &&
          !reg_file.test(PhysReg{op.physReg().reg()}, align(op.bytes() + op.physReg().byte(), 4))) {
         assert(op.isFixed());

         for (unsigned i = 0; i < op.size(); ++i) {
            is_killed_operand[(op.physReg() & 0xff) + i] = true;
         }

         killed_ops += op.getTemp().size();
      }
   }

   assert((regs_free + ctx.num_linear_vgprs) >= size);

   /* we might have to move dead operands to dst in order to make space */
   unsigned op_moves = 0;

   if (size > (regs_free - killed_ops))
      op_moves = size - (regs_free - killed_ops);

   /* find the best position to place the definition */
   PhysRegInterval best_win = {bounds.lo(), size};
   unsigned num_moves = 0xFF;
   unsigned num_vars = 0;

   /* we use a sliding window to check potential positions */
   for (PhysRegInterval reg_win = {bounds.lo(), size}; reg_win.hi() <= bounds.hi();
        reg_win += stride) {
      /* first check if the register window starts in the middle of an
       * allocated variable: this is what we have to fix to allow for
       * num_moves > size */
      if (reg_win.lo() > bounds.lo() && !reg_file.is_empty_or_blocked(reg_win.lo()) &&
          reg_file.get_id(reg_win.lo()) == reg_file.get_id(reg_win.lo().advance(-1)))
         continue;
      if (reg_win.hi() < bounds.hi() && !reg_file.is_empty_or_blocked(reg_win.hi().advance(-1)) &&
          reg_file.get_id(reg_win.hi().advance(-1)) == reg_file.get_id(reg_win.hi()))
         continue;

      /* second, check that we have at most k=num_moves elements in the window
       * and no element is larger than the currently processed one */
      unsigned k = op_moves;
      unsigned n = 0;
      unsigned remaining_op_moves = op_moves;
      unsigned last_var = 0;
      bool found = true;
      bool aligned = rc == RegClass::v4 && reg_win.lo() % 4 == 0;
      for (const PhysReg j : reg_win) {
         /* dead operands effectively reduce the number of estimated moves */
         if (is_killed_operand[j & 0xFF]) {
            if (remaining_op_moves) {
               k--;
               remaining_op_moves--;
            }
            continue;
         }

         if (reg_file[j] == 0 || reg_file[j] == last_var)
            continue;

         if (reg_file[j] == 0xF0000000) {
            k += 1;
            n++;
            continue;
         }

         if (ctx.assignments[reg_file[j]].rc.size() >= size) {
            found = false;
            break;
         }

         /* we cannot split live ranges of linear vgprs */
         if (ctx.assignments[reg_file[j]].rc.is_linear_vgpr()) {
            found = false;
            break;
         }

         k += ctx.assignments[reg_file[j]].rc.size();
         n++;
         last_var = reg_file[j];
      }

      if (!found || k > num_moves)
         continue;
      if (k == num_moves && n < num_vars)
         continue;
      if (!aligned && k == num_moves && n == num_vars)
         continue;

      if (found) {
         best_win = reg_win;
         num_moves = k;
         num_vars = n;
      }
   }

   if (num_moves == 0xFF)
      return {};

   /* now, we figured the placement for our definition */
   RegisterFile tmp_file(reg_file);

   /* p_create_vector: also re-place killed operands in the definition space */
   if (instr->opcode == aco_opcode::p_create_vector)
      tmp_file.fill_killed_operands(instr.get());

   std::vector<unsigned> vars = collect_vars(ctx, tmp_file, best_win);

   /* re-enable killed operands */
   if (!is_phi(instr) && instr->opcode != aco_opcode::p_create_vector)
      tmp_file.fill_killed_operands(instr.get());

   std::vector<std::pair<Operand, Definition>> pc;
   if (!get_regs_for_copies(ctx, tmp_file, pc, vars, instr, best_win))
      return {};

   parallelcopies.insert(parallelcopies.end(), pc.begin(), pc.end());

   adjust_max_used_regs(ctx, rc, best_win.lo());
   return best_win.lo();
}

bool
get_reg_specified(ra_ctx& ctx, const RegisterFile& reg_file, RegClass rc,
                  aco_ptr<Instruction>& instr, PhysReg reg, int operand)
{
   /* catch out-of-range registers */
   if (reg >= PhysReg{512})
      return false;

   DefInfo info(ctx, instr, rc, operand);

   if (reg.reg_b % info.data_stride)
      return false;

   assert(util_is_power_of_two_nonzero(info.stride));
   reg.reg_b &= ~(info.stride - 1);

   PhysRegInterval reg_win = {PhysReg(reg.reg()), info.rc.size()};
   PhysRegInterval vcc_win = {vcc, 2};
   /* VCC is outside the bounds */
   bool is_vcc =
      info.rc.type() == RegType::sgpr && vcc_win.contains(reg_win) && ctx.program->needs_vcc;
   bool is_m0 = info.rc == s1 && reg == m0 && can_write_m0(instr);
   if (!info.bounds.contains(reg_win) && !is_vcc && !is_m0)
      return false;

   if (reg_file.test(reg, info.rc.bytes()))
      return false;

   adjust_max_used_regs(ctx, info.rc, reg_win.lo());
   return true;
}

bool
increase_register_file(ra_ctx& ctx, RegClass rc)
{
   if (rc.type() == RegType::vgpr && ctx.num_linear_vgprs == 0 &&
       ctx.vgpr_bounds < ctx.vgpr_limit) {
      /* If vgpr_bounds is less than max_reg_demand.vgpr, this should be a no-op. */
      update_vgpr_sgpr_demand(
         ctx.program, RegisterDemand(ctx.vgpr_bounds + 1, ctx.program->max_reg_demand.sgpr));

      ctx.vgpr_bounds = ctx.program->max_reg_demand.vgpr;
   } else if (rc.type() == RegType::sgpr && ctx.program->max_reg_demand.sgpr < ctx.sgpr_limit) {
      update_vgpr_sgpr_demand(
         ctx.program, RegisterDemand(ctx.program->max_reg_demand.vgpr, ctx.sgpr_bounds + 1));

      ctx.sgpr_bounds = ctx.program->max_reg_demand.sgpr;
   } else {
      return false;
   }

   return true;
}

struct IDAndRegClass {
   IDAndRegClass(unsigned id_, RegClass rc_) : id(id_), rc(rc_) {}

   unsigned id;
   RegClass rc;
};

struct IDAndInfo {
   IDAndInfo(unsigned id_, DefInfo info_) : id(id_), info(info_) {}

   unsigned id;
   DefInfo info;
};

void
add_rename(ra_ctx& ctx, Temp orig_val, Temp new_val)
{
   ctx.renames[ctx.block->index][orig_val.id()] = new_val;
   ctx.orig_names.emplace(new_val.id(), orig_val);
   ctx.assignments[orig_val.id()].renamed = true;
}

/* Reallocates vars by sorting them and placing each variable after the previous
 * one. If one of the variables has 0xffffffff as an ID, the register assigned
 * for that variable will be returned.
 */
PhysReg
compact_relocate_vars(ra_ctx& ctx, const std::vector<IDAndRegClass>& vars,
                      std::vector<std::pair<Operand, Definition>>& parallelcopies, PhysReg start)
{
   /* This function assumes RegisterDemand/live_var_analysis rounds up sub-dword
    * temporary sizes to dwords.
    */
   std::vector<IDAndInfo> sorted;
   for (IDAndRegClass var : vars) {
      DefInfo info(ctx, ctx.pseudo_dummy, var.rc, -1);
      sorted.emplace_back(var.id, info);
   }

   std::sort(
      sorted.begin(), sorted.end(),
      [&ctx](const IDAndInfo& a, const IDAndInfo& b)
      {
         unsigned a_stride = a.info.stride * (a.info.rc.is_subdword() ? 1 : 4);
         unsigned b_stride = b.info.stride * (b.info.rc.is_subdword() ? 1 : 4);
         if (a_stride > b_stride)
            return true;
         if (a_stride < b_stride)
            return false;
         if (a.id == 0xffffffff || b.id == 0xffffffff)
            return a.id ==
                   0xffffffff; /* place 0xffffffff before others if possible, not for any reason */
         return ctx.assignments[a.id].reg < ctx.assignments[b.id].reg;
      });

   PhysReg next_reg = start;
   PhysReg space_reg;
   for (IDAndInfo& var : sorted) {
      unsigned stride = var.info.rc.is_subdword() ? var.info.stride : var.info.stride * 4;
      next_reg.reg_b = align(next_reg.reg_b, MAX2(stride, 4));

      /* 0xffffffff is a special variable ID used reserve a space for killed
       * operands and definitions.
       */
      if (var.id != 0xffffffff) {
         if (next_reg != ctx.assignments[var.id].reg) {
            RegClass rc = ctx.assignments[var.id].rc;
            Temp tmp(var.id, rc);

            Operand pc_op(tmp);
            pc_op.setFixed(ctx.assignments[var.id].reg);
            Definition pc_def(next_reg, rc);
            parallelcopies.emplace_back(pc_op, pc_def);
         }
      } else {
         space_reg = next_reg;
      }

      adjust_max_used_regs(ctx, var.info.rc, next_reg);

      next_reg = next_reg.advance(var.info.rc.size() * 4);
   }

   return space_reg;
}

bool
is_vector_intact(ra_ctx& ctx, const RegisterFile& reg_file, const vector_info& vec_info)
{
   unsigned size = 0;
   for (unsigned i = 0; i < vec_info.num_parts; i++)
      size += vec_info.parts[i].bytes();

   PhysReg first{512};
   int offset = 0;
   for (unsigned i = 0; i < vec_info.num_parts; i++) {
      Operand op = vec_info.parts[i];

      if (ctx.assignments[op.tempId()].assigned) {
         PhysReg reg = ctx.assignments[op.tempId()].reg;

         if (first.reg() == 512) {
            PhysRegInterval bounds = get_reg_bounds(ctx, RegType::vgpr, false);
            first = reg.advance(-offset);
            PhysRegInterval vec = PhysRegInterval{first, DIV_ROUND_UP(size, 4)};
            if (!bounds.contains(vec)) /* not enough space for other operands */
               return false;
         } else {
            if (reg != first.advance(offset)) /* not at the best position */
               return false;
         }
      } else {
         /* If there's an unexpected temporary, this operand is unlikely to be
          * placed in the best position.
          */
         if (first.reg() != 512 && reg_file.test(first.advance(offset), op.bytes()))
            return false;
      }

      offset += op.bytes();
   }

   return true;
}

std::optional<PhysReg>
get_reg_vector(ra_ctx& ctx, const RegisterFile& reg_file, Temp temp, aco_ptr<Instruction>& instr,
               int operand)
{
   const vector_info& vec = ctx.vectors[temp.id()];
   if (!vec.is_weak || is_vector_intact(ctx, reg_file, vec)) {
      unsigned our_offset = 0;
      for (unsigned i = 0; i < vec.num_parts; i++) {
         const Operand& op = vec.parts[i];
         if (op.isTemp() && op.tempId() == temp.id())
            break;
         else
            our_offset += op.bytes();
      }

      unsigned their_offset = 0;
      /* check for every operand of the vector
       * - whether the operand is assigned and
       * - we can use the register relative to that operand
       */
      for (unsigned i = 0; i < vec.num_parts; i++) {
         const Operand& op = vec.parts[i];
         if (op.isTemp() && op.tempId() != temp.id() && op.getTemp().type() == temp.type() &&
             ctx.assignments[op.tempId()].assigned) {
            PhysReg reg = ctx.assignments[op.tempId()].reg;
            reg.reg_b += (our_offset - their_offset);
            if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, reg, operand))
               return reg;

            /* return if MIMG vaddr components don't remain vector-aligned */
            if (vec.is_weak)
               return {};
         }
         their_offset += op.bytes();
      }

      /* We didn't find a register relative to other vector operands.
       * Try to find new space which fits the whole vector.
       */
      RegClass vec_rc = RegClass::get(temp.type(), their_offset);
      DefInfo info(ctx, ctx.pseudo_dummy, vec_rc, -1);
      std::optional<PhysReg> reg = get_reg_simple(ctx, reg_file, info);
      if (reg) {
         reg->reg_b += our_offset;
         /* make sure to only use byte offset if the instruction supports it */
         if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, *reg, operand))
            return reg;
      }
   }
   return {};
}

bool
compact_linear_vgprs(ra_ctx& ctx, const RegisterFile& reg_file,
                     std::vector<std::pair<Operand, Definition>>& parallelcopies)
{
   PhysRegInterval linear_vgpr_bounds = get_reg_bounds(ctx, RegType::vgpr, true);
   int zeros = reg_file.count_zero(linear_vgpr_bounds);
   if (zeros == 0)
      return false;

   std::vector<IDAndRegClass> vars;
   for (unsigned id : find_vars(ctx, reg_file, linear_vgpr_bounds))
      vars.emplace_back(id, ctx.assignments[id].rc);

   ctx.num_linear_vgprs -= zeros;
   compact_relocate_vars(ctx, vars, parallelcopies, get_reg_bounds(ctx, RegType::vgpr, true).lo());

   return true;
}

/* Allocates a linear VGPR. We allocate them at the end of the register file and keep them separate
 * from normal VGPRs. This is for two reasons:
 * - Because we only ever move linear VGPRs into an empty space or a space previously occupied by a
 *   linear one, we never have to swap a normal VGPR and a linear one.
 * - As linear VGPR's live ranges only start and end on top-level blocks, we never have to move a
 *   linear VGPR in control flow.
 */
PhysReg
alloc_linear_vgpr(ra_ctx& ctx, const RegisterFile& reg_file, aco_ptr<Instruction>& instr,
                  std::vector<std::pair<Operand, Definition>>& parallelcopies)
{
   assert(instr->opcode == aco_opcode::p_start_linear_vgpr);
   assert(instr->definitions.size() == 1 && instr->definitions[0].bytes() % 4 == 0);

   RegClass rc = instr->definitions[0].regClass();

   /* Try to choose an unused space in the linear VGPR bounds. */
   for (unsigned i = rc.size(); i <= ctx.num_linear_vgprs; i++) {
      PhysReg reg(256 + ctx.vgpr_bounds - i);
      if (!reg_file.test(reg, rc.bytes())) {
         adjust_max_used_regs(ctx, rc, reg);
         return reg;
      }
   }

   PhysRegInterval old_normal_bounds = get_reg_bounds(ctx, RegType::vgpr, false);

   /* Compact linear VGPRs, grow the bounds if necessary, and choose a space at the beginning: */
   compact_linear_vgprs(ctx, reg_file, parallelcopies);

   PhysReg reg(256 + ctx.vgpr_bounds - (ctx.num_linear_vgprs + rc.size()));
   /* Space that was for normal VGPRs, but is now for linear VGPRs. */
   PhysRegInterval new_win = PhysRegInterval::from_until(reg, MAX2(old_normal_bounds.hi(), reg));

   RegisterFile tmp_file(reg_file);
   PhysRegInterval reg_win{reg, rc.size()};
   std::vector<unsigned> blocking_vars = collect_vars(ctx, tmp_file, new_win);

   /* Re-enable killed operands */
   tmp_file.fill_killed_operands(instr.get());

   /* Find new assignments for blocking vars. */
   std::vector<std::pair<Operand, Definition>> pc;
   if (!ctx.policy.skip_optimistic_path &&
       get_regs_for_copies(ctx, tmp_file, pc, blocking_vars, instr, reg_win)) {
      parallelcopies.insert(parallelcopies.end(), pc.begin(), pc.end());
   } else {
      /* Fallback algorithm: reallocate all variables at once. */
      std::vector<IDAndRegClass> vars;
      for (unsigned id : find_vars(ctx, reg_file, old_normal_bounds))
         vars.emplace_back(id, ctx.assignments[id].rc);
      compact_relocate_vars(ctx, vars, parallelcopies, PhysReg(256));

      std::vector<IDAndRegClass> killed_op_vars;
      for (Operand& op : instr->operands) {
         if (op.isTemp() && op.isFirstKillBeforeDef() && op.regClass().type() == RegType::vgpr)
            killed_op_vars.emplace_back(op.tempId(), op.regClass());
      }
      compact_relocate_vars(ctx, killed_op_vars, parallelcopies, reg_win.lo());
   }

   /* If this is updated earlier, a killed operand can't be placed inside the definition. */
   ctx.num_linear_vgprs += rc.size();

   adjust_max_used_regs(ctx, rc, reg);
   return reg;
}

bool
should_compact_linear_vgprs(ra_ctx& ctx, const RegisterFile& reg_file)
{
   if (!(ctx.block->kind & block_kind_top_level) || ctx.block->linear_succs.empty())
      return false;

   /* Since we won't be able to copy linear VGPRs to make space when in control flow, we have to
    * ensure in advance that there is enough space for normal VGPRs. */
   unsigned max_vgpr_usage = 0;
   unsigned next_toplevel = ctx.block->index + 1;
   for (; !(ctx.program->blocks[next_toplevel].kind & block_kind_top_level); next_toplevel++) {
      max_vgpr_usage =
         MAX2(max_vgpr_usage, (unsigned)ctx.program->blocks[next_toplevel].register_demand.vgpr);
   }
   max_vgpr_usage =
      MAX2(max_vgpr_usage, (unsigned)ctx.program->blocks[next_toplevel].live_in_demand.vgpr);

   for (unsigned tmp : find_vars(ctx, reg_file, get_reg_bounds(ctx, RegType::vgpr, true)))
      max_vgpr_usage -= ctx.assignments[tmp].rc.size();

   return max_vgpr_usage > get_reg_bounds(ctx, RegType::vgpr, false).size;
}

PhysReg
get_reg(ra_ctx& ctx, const RegisterFile& reg_file, Temp temp,
        std::vector<std::pair<Operand, Definition>>& parallelcopies, aco_ptr<Instruction>& instr,
        int operand_index = -1)
{
   auto split_vec = ctx.split_vectors.find(temp.id());
   if (split_vec != ctx.split_vectors.end()) {
      unsigned offset = 0;
      for (Definition def : split_vec->second->definitions) {
         if (ctx.assignments[def.tempId()].affinity) {
            assignment& affinity = ctx.assignments[ctx.assignments[def.tempId()].affinity];
            if (affinity.assigned) {
               PhysReg reg = affinity.reg;
               reg.reg_b -= offset;
               if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, reg, operand_index))
                  return reg;
            }
         }
         offset += def.bytes();
      }
   }

   if (ctx.assignments[temp.id()].affinity) {
      assignment& affinity = ctx.assignments[ctx.assignments[temp.id()].affinity];
      if (affinity.assigned) {
         if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, affinity.reg, operand_index))
            return affinity.reg;
      }
   }
   if (ctx.assignments[temp.id()].vcc) {
      if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, vcc, operand_index))
         return vcc;
   }
   if (ctx.assignments[temp.id()].m0) {
      if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, m0, operand_index))
         return m0;
   }

   std::optional<PhysReg> res;

   if (ctx.vectors.find(temp.id()) != ctx.vectors.end()) {
      res = get_reg_vector(ctx, reg_file, temp, instr, operand_index);
      if (res)
         return *res;
   }

   if (temp.size() == 1 && operand_index == -1) {
      for (const Operand& op : instr->operands) {
         if (op.isTemp() && op.isFirstKillBeforeDef() && op.regClass() == temp.regClass()) {
            assert(op.isFixed());
            if (op.physReg() == vcc || op.physReg() == vcc_hi)
               continue;
            if (get_reg_specified(ctx, reg_file, temp.regClass(), instr, op.physReg(),
                                  operand_index))
               return op.physReg();
         }
      }
   }

   DefInfo info(ctx, instr, temp.regClass(), operand_index);

   if (!ctx.policy.skip_optimistic_path) {
      /* try to find space without live-range splits */
      res = get_reg_simple(ctx, reg_file, info);

      if (res)
         return *res;
   }

   /* try to find space with live-range splits */
   res = get_reg_impl(ctx, reg_file, parallelcopies, info, instr);

   if (res)
      return *res;

   /* try compacting the linear vgprs to make more space */
   std::vector<std::pair<Operand, Definition>> pc;
   if (info.rc.type() == RegType::vgpr && (ctx.block->kind & block_kind_top_level) &&
       compact_linear_vgprs(ctx, reg_file, pc)) {
      parallelcopies.insert(parallelcopies.end(), pc.begin(), pc.end());

      /* We don't need to fill the copy definitions in because we don't care about the linear VGPR
       * space here. */
      RegisterFile tmp_file(reg_file);
      for (std::pair<Operand, Definition>& copy : pc)
         tmp_file.clear(copy.first);

      return get_reg(ctx, tmp_file, temp, parallelcopies, instr, operand_index);
   }

   /* We should only fail here because keeping under the limit would require
    * too many moves. */
   assert(reg_file.count_zero(info.bounds) >= info.size);

   /* try using more registers */
   if (!increase_register_file(ctx, info.rc)) {
      /* fallback algorithm: reallocate all variables at once (linear VGPRs should already be
       * compact at the end) */
      unsigned def_size = info.rc.size();
      for (Definition def : instr->definitions) {
         if (ctx.assignments[def.tempId()].assigned && def.regClass().type() == info.rc.type())
            def_size += def.regClass().size();
      }

      unsigned killed_op_size = 0;
      for (Operand op : instr->operands) {
         if (op.isTemp() && op.isFirstKillBeforeDef() && op.regClass().type() == info.rc.type())
            killed_op_size += op.regClass().size();
      }

      const PhysRegInterval regs = get_reg_bounds(ctx, info.rc);

      /* reallocate passthrough variables and non-killed operands */
      std::vector<IDAndRegClass> vars;
      for (unsigned id : find_vars(ctx, reg_file, regs))
         vars.emplace_back(id, ctx.assignments[id].rc);
      vars.emplace_back(0xffffffff, RegClass(info.rc.type(), MAX2(def_size, killed_op_size)));

      PhysReg space = compact_relocate_vars(ctx, vars, parallelcopies, regs.lo());

      /* reallocate killed operands */
      std::vector<IDAndRegClass> killed_op_vars;
      for (Operand op : instr->operands) {
         if (op.isFirstKillBeforeDef() && op.regClass().type() == info.rc.type())
            killed_op_vars.emplace_back(op.tempId(), op.regClass());
      }
      compact_relocate_vars(ctx, killed_op_vars, parallelcopies, space);

      /* reallocate definitions */
      std::vector<IDAndRegClass> def_vars;
      for (Definition def : instr->definitions) {
         if (ctx.assignments[def.tempId()].assigned && def.regClass().type() == info.rc.type())
            def_vars.emplace_back(def.tempId(), def.regClass());
      }
      def_vars.emplace_back(0xffffffff, info.rc);
      return compact_relocate_vars(ctx, def_vars, parallelcopies, space);
   }

   return get_reg(ctx, reg_file, temp, parallelcopies, instr, operand_index);
}

PhysReg
get_reg_create_vector(ra_ctx& ctx, const RegisterFile& reg_file, Temp temp,
                      std::vector<std::pair<Operand, Definition>>& parallelcopies,
                      aco_ptr<Instruction>& instr)
{
   RegClass rc = temp.regClass();
   /* create_vector instructions have different costs w.r.t. register coalescing */
   uint32_t size = rc.size();
   uint32_t bytes = rc.bytes();
   uint32_t stride = get_stride(rc);
   PhysRegInterval bounds = get_reg_bounds(ctx, rc);

   // TODO: improve p_create_vector for sub-dword vectors

   PhysReg best_pos{0xFFF};
   unsigned num_moves = 0xFF;
   bool best_avoid = true;
   uint32_t correct_pos_mask = 0;

   /* test for each operand which definition placement causes the least shuffle instructions */
   for (unsigned i = 0, offset = 0; i < instr->operands.size();
        offset += instr->operands[i].bytes(), i++) {
      // TODO: think about, if we can alias live operands on the same register
      if (!instr->operands[i].isTemp() || !instr->operands[i].isKillBeforeDef() ||
          instr->operands[i].getTemp().type() != rc.type())
         continue;

      if (offset > instr->operands[i].physReg().reg_b)
         continue;

      unsigned reg_lower = instr->operands[i].physReg().reg_b - offset;
      if (reg_lower % 4)
         continue;
      PhysRegInterval reg_win = {PhysReg{reg_lower / 4}, size};
      unsigned k = 0;

      /* no need to check multiple times */
      if (reg_win.lo() == best_pos)
         continue;

      /* check borders */
      // TODO: this can be improved */
      if (!bounds.contains(reg_win) || reg_win.lo() % stride != 0)
         continue;
      if (reg_win.lo() > bounds.lo() && reg_file[reg_win.lo()] != 0 &&
          reg_file.get_id(reg_win.lo()) == reg_file.get_id(reg_win.lo().advance(-1)))
         continue;
      if (reg_win.hi() < bounds.hi() && reg_file[reg_win.hi().advance(-4)] != 0 &&
          reg_file.get_id(reg_win.hi().advance(-1)) == reg_file.get_id(reg_win.hi()))
         continue;

      /* count variables to be moved and check "avoid" */
      bool avoid = false;
      bool linear_vgpr = false;
      for (PhysReg j : reg_win) {
         if (reg_file[j] != 0) {
            if (reg_file[j] == 0xF0000000) {
               PhysReg reg;
               reg.reg_b = j * 4;
               unsigned bytes_left = bytes - ((unsigned)j - reg_win.lo()) * 4;
               for (unsigned byte_idx = 0; byte_idx < MIN2(bytes_left, 4); byte_idx++, reg.reg_b++)
                  k += reg_file.test(reg, 1);
            } else {
               k += 4;
               linear_vgpr |= ctx.assignments[reg_file[j]].rc.is_linear_vgpr();
            }
         }
         avoid |= ctx.war_hint[j];
      }

      /* we cannot split live ranges of linear vgprs */
      if (linear_vgpr)
         continue;

      if (avoid && !best_avoid)
         continue;

      /* count operands in wrong positions */
      uint32_t correct_pos_mask_new = 0;
      for (unsigned j = 0, offset2 = 0; j < instr->operands.size();
           offset2 += instr->operands[j].bytes(), j++) {
         Operand& op = instr->operands[j];
         if (op.isTemp() && op.physReg().reg_b == reg_win.lo() * 4 + offset2)
            correct_pos_mask_new |= 1 << j;
         else
            k += op.bytes();
      }
      bool aligned = rc == RegClass::v4 && reg_win.lo() % 4 == 0;
      if (k > num_moves || (!aligned && k == num_moves))
         continue;

      best_pos = reg_win.lo();
      num_moves = k;
      best_avoid = avoid;
      correct_pos_mask = correct_pos_mask_new;
   }

   /* too many moves: try the generic get_reg() function */
   if (num_moves >= 2 * bytes) {
      return get_reg(ctx, reg_file, temp, parallelcopies, instr);
   } else if (num_moves > bytes) {
      DefInfo info(ctx, instr, rc, -1);
      std::optional<PhysReg> res = get_reg_simple(ctx, reg_file, info);
      if (res)
         return *res;
   }

   /* re-enable killed operands which are in the wrong position */
   RegisterFile tmp_file(reg_file);
   tmp_file.fill_killed_operands(instr.get());

   for (unsigned i = 0; i < instr->operands.size(); i++) {
      if ((correct_pos_mask >> i) & 1u && instr->operands[i].isKill())
         tmp_file.clear(instr->operands[i]);
   }

   /* collect variables to be moved */
   std::vector<unsigned> vars = collect_vars(ctx, tmp_file, PhysRegInterval{best_pos, size});

   bool success = false;
   std::vector<std::pair<Operand, Definition>> pc;
   success = get_regs_for_copies(ctx, tmp_file, pc, vars, instr, PhysRegInterval{best_pos, size});

   if (!success) {
      if (!increase_register_file(ctx, temp.regClass())) {
         /* use the fallback algorithm in get_reg() */
         return get_reg(ctx, reg_file, temp, parallelcopies, instr);
      }
      return get_reg_create_vector(ctx, reg_file, temp, parallelcopies, instr);
   }

   parallelcopies.insert(parallelcopies.end(), pc.begin(), pc.end());
   adjust_max_used_regs(ctx, rc, best_pos);

   return best_pos;
}

void
handle_pseudo(ra_ctx& ctx, const RegisterFile& reg_file, Instruction* instr)
{
   if (instr->format != Format::PSEUDO)
      return;

   /* all instructions which use handle_operands() need this information */
   switch (instr->opcode) {
   case aco_opcode::p_extract_vector:
   case aco_opcode::p_create_vector:
   case aco_opcode::p_split_vector:
   case aco_opcode::p_parallelcopy:
   case aco_opcode::p_start_linear_vgpr: break;
   default: return;
   }

   bool writes_linear = false;
   /* if all definitions are logical vgpr, no need to care for SCC */
   for (Definition& def : instr->definitions) {
      if (def.getTemp().regClass().is_linear())
         writes_linear = true;
   }
   /* if all operands are constant, no need to care either */
   bool reads_linear = false;
   for (Operand& op : instr->operands) {
      if (op.isTemp() && op.getTemp().regClass().is_linear())
         reads_linear = true;
   }

   if (!writes_linear || !reads_linear || !reg_file[scc])
      return;

   instr->pseudo().needs_scratch_reg = true;
   instr->pseudo().tmp_in_scc = reg_file[scc];

   int reg = ctx.max_used_sgpr;
   for (; reg >= 0 && reg_file[PhysReg{(unsigned)reg}]; reg--)
      ;
   if (reg < 0) {
      reg = ctx.max_used_sgpr + 1;
      for (; reg < ctx.program->max_reg_demand.sgpr && reg_file[PhysReg{(unsigned)reg}]; reg++)
         ;
   }

   adjust_max_used_regs(ctx, s1, reg);
   instr->pseudo().scratch_sgpr = PhysReg{(unsigned)reg};
}

bool
operand_can_use_reg(amd_gfx_level gfx_level, aco_ptr<Instruction>& instr, unsigned idx, PhysReg reg,
                    RegClass rc)
{
   if (reg.byte()) {
      unsigned stride = get_subdword_operand_stride(gfx_level, instr, idx, rc);
      if (reg.byte() % stride)
         return false;
   }

   switch (instr->format) {
   case Format::SMEM:
      return reg != scc && reg != exec &&
             (reg != m0 || idx == 1 || idx == 3) && /* offset can be m0 */
             (reg != vcc || (instr->definitions.empty() && idx == 2) ||
              gfx_level >= GFX10); /* sdata can be vcc */
   case Format::MUBUF:
   case Format::MTBUF: return idx != 2 || gfx_level < GFX12 || reg != scc;
   default:
      // TODO: there are more instructions with restrictions on registers
      return true;
   }
}

void
handle_fixed_operands(ra_ctx& ctx, RegisterFile& register_file,
                      std::vector<std::pair<Operand, Definition>>& parallelcopy,
                      aco_ptr<Instruction>& instr)
{
   assert(instr->operands.size() <= 128);
   assert(parallelcopy.empty());

   RegisterFile tmp_file(register_file);

   BITSET_DECLARE(mask, 128) = {0};

   for (unsigned i = 0; i < instr->operands.size(); i++) {
      Operand& op = instr->operands[i];

      if (!op.isTemp() || !op.isFixed())
         continue;

      PhysReg src = ctx.assignments[op.tempId()].reg;
      adjust_max_used_regs(ctx, op.regClass(), op.physReg());

      if (op.physReg() == src) {
         tmp_file.block(op.physReg(), op.regClass());
         continue;
      }

      /* An instruction can have at most one operand precolored to the same register. */
      assert(std::none_of(parallelcopy.begin(), parallelcopy.end(),
                          [&](auto copy) { return copy.second.physReg() == op.physReg(); }));

      /* clear from register_file so fixed operands are not collected be collect_vars() */
      tmp_file.clear(src, op.regClass()); // TODO: try to avoid moving block vars to src

      BITSET_SET(mask, i);

      Operand pc_op(instr->operands[i].getTemp(), src);
      Definition pc_def = Definition(op.physReg(), pc_op.regClass());
      parallelcopy.emplace_back(pc_op, pc_def);
   }

   if (BITSET_IS_EMPTY(mask))
      return;

   unsigned i;
   std::vector<unsigned> blocking_vars;
   BITSET_FOREACH_SET (i, mask, instr->operands.size()) {
      Operand& op = instr->operands[i];
      PhysRegInterval target{op.physReg(), op.size()};
      std::vector<unsigned> blocking_vars2 = collect_vars(ctx, tmp_file, target);
      blocking_vars.insert(blocking_vars.end(), blocking_vars2.begin(), blocking_vars2.end());

      /* prevent get_regs_for_copies() from using these registers */
      tmp_file.block(op.physReg(), op.regClass());
   }

   get_regs_for_copies(ctx, tmp_file, parallelcopy, blocking_vars, instr, PhysRegInterval());
   update_renames(ctx, register_file, parallelcopy, instr,
                  rename_not_killed_ops | rename_precolored_ops);
}

void
get_reg_for_operand(ra_ctx& ctx, RegisterFile& register_file,
                    std::vector<std::pair<Operand, Definition>>& parallelcopy,
                    aco_ptr<Instruction>& instr, Operand& operand, unsigned operand_index)
{
   /* clear the operand in case it's only a stride mismatch */
   PhysReg src = ctx.assignments[operand.tempId()].reg;
   register_file.clear(src, operand.regClass());
   PhysReg dst = get_reg(ctx, register_file, operand.getTemp(), parallelcopy, instr, operand_index);

   Operand pc_op = operand;
   pc_op.setFixed(src);
   Definition pc_def = Definition(dst, pc_op.regClass());
   parallelcopy.emplace_back(pc_op, pc_def);
   update_renames(ctx, register_file, parallelcopy, instr, rename_not_killed_ops);
   register_file.fill(Definition(operand.getTemp(), dst));
}

PhysReg
get_reg_phi(ra_ctx& ctx, IDSet& live_in, RegisterFile& register_file,
            std::vector<aco_ptr<Instruction>>& instructions, Block& block,
            aco_ptr<Instruction>& phi, Temp tmp)
{
   std::vector<std::pair<Operand, Definition>> parallelcopy;
   PhysReg reg = get_reg(ctx, register_file, tmp, parallelcopy, phi);
   update_renames(ctx, register_file, parallelcopy, phi, rename_not_killed_ops);

   /* process parallelcopy */
   for (std::pair<Operand, Definition> pc : parallelcopy) {
      /* see if it's a copy from a different phi */
      // TODO: prefer moving some previous phis over live-ins
      // TODO: somehow prevent phis fixed before the RA from being updated (shouldn't be a
      // problem in practice since they can only be fixed to exec)
      Instruction* prev_phi = NULL;
      for (auto phi_it = instructions.begin(); phi_it != instructions.end(); ++phi_it) {
         if ((*phi_it)->definitions[0].tempId() == pc.first.tempId())
            prev_phi = phi_it->get();
      }
      if (prev_phi) {
         /* if so, just update that phi's register */
         prev_phi->definitions[0].setFixed(pc.second.physReg());
         register_file.fill(prev_phi->definitions[0]);
         ctx.assignments[prev_phi->definitions[0].tempId()] = {pc.second.physReg(),
                                                               pc.second.regClass()};
         continue;
      }

      /* rename */
      auto orig_it = ctx.orig_names.find(pc.first.tempId());
      Temp orig = orig_it != ctx.orig_names.end() ? orig_it->second : pc.first.getTemp();
      add_rename(ctx, orig, pc.second.getTemp());

      /* otherwise, this is a live-in and we need to create a new phi
       * to move it in this block's predecessors */
      aco_opcode opcode =
         pc.first.getTemp().is_linear() ? aco_opcode::p_linear_phi : aco_opcode::p_phi;
      Block::edge_vec& preds =
         pc.first.getTemp().is_linear() ? block.linear_preds : block.logical_preds;
      aco_ptr<Instruction> new_phi{create_instruction(opcode, Format::PSEUDO, preds.size(), 1)};
      new_phi->definitions[0] = pc.second;
      for (unsigned i = 0; i < preds.size(); i++)
         new_phi->operands[i] = Operand(pc.first);
      instructions.emplace_back(std::move(new_phi));

      /* Remove from live_in, because handle_loop_phis() would re-create this phi later if this is
       * a loop header.
       */
      live_in.erase(orig.id());
   }

   return reg;
}

void
get_regs_for_phis(ra_ctx& ctx, Block& block, RegisterFile& register_file,
                  std::vector<aco_ptr<Instruction>>& instructions, IDSet& live_in)
{
   /* move all phis to instructions */
   for (aco_ptr<Instruction>& phi : block.instructions) {
      if (!is_phi(phi))
         break;
      if (!phi->definitions[0].isKill())
         instructions.emplace_back(std::move(phi));
   }

   /* assign phis with all-matching registers to that register */
   for (aco_ptr<Instruction>& phi : instructions) {
      Definition& definition = phi->definitions[0];
      if (definition.isFixed())
         continue;

      if (!phi->operands[0].isTemp())
         continue;

      PhysReg reg = phi->operands[0].physReg();
      auto OpsSame = [=](const Operand& op) -> bool
      { return op.isTemp() && (!op.isFixed() || op.physReg() == reg); };
      bool all_same = std::all_of(phi->operands.cbegin() + 1, phi->operands.cend(), OpsSame);
      if (!all_same)
         continue;

      if (!get_reg_specified(ctx, register_file, definition.regClass(), phi, reg, -1))
         continue;

      definition.setFixed(reg);
      register_file.fill(definition);
      ctx.assignments[definition.tempId()].set(definition);
   }

   /* try to find a register that is used by at least one operand */
   for (aco_ptr<Instruction>& phi : instructions) {
      Definition& definition = phi->definitions[0];
      if (definition.isFixed())
         continue;

      /* use affinity if available */
      if (ctx.assignments[definition.tempId()].affinity &&
          ctx.assignments[ctx.assignments[definition.tempId()].affinity].assigned) {
         assignment& affinity = ctx.assignments[ctx.assignments[definition.tempId()].affinity];
         assert(affinity.rc == definition.regClass());
         if (get_reg_specified(ctx, register_file, definition.regClass(), phi, affinity.reg, -1)) {
            definition.setFixed(affinity.reg);
            register_file.fill(definition);
            ctx.assignments[definition.tempId()].set(definition);
            continue;
         }
      }

      /* by going backwards, we aim to avoid copies in else-blocks */
      for (int i = phi->operands.size() - 1; i >= 0; i--) {
         const Operand& op = phi->operands[i];
         if (!op.isTemp() || !op.isFixed())
            continue;

         PhysReg reg = op.physReg();
         if (get_reg_specified(ctx, register_file, definition.regClass(), phi, reg, -1)) {
            definition.setFixed(reg);
            register_file.fill(definition);
            ctx.assignments[definition.tempId()].set(definition);
            break;
         }
      }
   }

   /* find registers for phis where the register was blocked or no operand was assigned */

   /* Don't use iterators because get_reg_phi() can add phis to the end of the vector. */
   for (unsigned i = 0; i < instructions.size(); i++) {
      aco_ptr<Instruction>& phi = instructions[i];
      Definition& definition = phi->definitions[0];
      if (definition.isFixed())
         continue;

      definition.setFixed(
         get_reg_phi(ctx, live_in, register_file, instructions, block, phi, definition.getTemp()));

      register_file.fill(definition);
      ctx.assignments[definition.tempId()].set(definition);
   }
}

inline Temp
read_variable(ra_ctx& ctx, Temp val, unsigned block_idx)
{
   /* This variable didn't get renamed, yet. */
   if (!ctx.assignments[val.id()].renamed)
      return val;

   auto it = ctx.renames[block_idx].find(val.id());
   if (it == ctx.renames[block_idx].end())
      return val;
   else
      return it->second;
}

Temp
handle_live_in(ra_ctx& ctx, Temp val, Block* block)
{
   /* This variable didn't get renamed, yet. */
   if (!ctx.assignments[val.id()].renamed)
      return val;

   Block::edge_vec& preds = val.is_linear() ? block->linear_preds : block->logical_preds;
   if (preds.size() == 0)
      return val;

   if (preds.size() == 1) {
      /* if the block has only one predecessor, just look there for the name */
      return read_variable(ctx, val, preds[0]);
   }

   /* there are multiple predecessors and the block is sealed */
   Temp* const ops = (Temp*)alloca(preds.size() * sizeof(Temp));

   /* get the rename from each predecessor and check if they are the same */
   Temp new_val;
   bool needs_phi = false;
   for (unsigned i = 0; i < preds.size(); i++) {
      ops[i] = read_variable(ctx, val, preds[i]);
      if (i == 0)
         new_val = ops[i];
      else
         needs_phi |= !(new_val == ops[i]);
   }

   if (needs_phi) {
      assert(!val.regClass().is_linear_vgpr());

      /* the variable has been renamed differently in the predecessors: we need to insert a phi */
      aco_opcode opcode = val.is_linear() ? aco_opcode::p_linear_phi : aco_opcode::p_phi;
      aco_ptr<Instruction> phi{create_instruction(opcode, Format::PSEUDO, preds.size(), 1)};
      new_val = ctx.program->allocateTmp(val.regClass());
      phi->definitions[0] = Definition(new_val);
      ctx.assignments.emplace_back();
      assert(ctx.assignments.size() == ctx.program->peekAllocationId());
      for (unsigned i = 0; i < preds.size(); i++) {
         /* update the operands so that it uses the new affinity */
         phi->operands[i] = Operand(ops[i]);
         assert(ctx.assignments[ops[i].id()].assigned);
         assert(ops[i].regClass() == new_val.regClass());
         phi->operands[i].setFixed(ctx.assignments[ops[i].id()].reg);
      }
      block->instructions.insert(block->instructions.begin(), std::move(phi));
   }

   return new_val;
}

void
handle_loop_phis(ra_ctx& ctx, const IDSet& live_in, uint32_t loop_header_idx,
                 uint32_t loop_exit_idx)
{
   Block& loop_header = ctx.program->blocks[loop_header_idx];
   aco::unordered_map<uint32_t, Temp> renames(ctx.memory);

   /* create phis for variables renamed during the loop */
   for (unsigned t : live_in) {
      if (!ctx.assignments[t].renamed)
         continue;

      Temp val = Temp(t, ctx.program->temp_rc[t]);
      Temp prev = read_variable(ctx, val, loop_header_idx - 1);
      Temp renamed = handle_live_in(ctx, val, &loop_header);
      if (renamed == prev)
         continue;

      /* insert additional renames at block end, but don't overwrite */
      renames[prev.id()] = renamed;
      ctx.orig_names[renamed.id()] = val;
      for (unsigned idx = loop_header_idx; idx < loop_exit_idx; idx++) {
         auto it = ctx.renames[idx].emplace(val.id(), renamed);
         /* if insertion is unsuccessful, update if necessary */
         if (!it.second && it.first->second == prev)
            it.first->second = renamed;
      }

      /* update loop-carried values of the phi created by handle_live_in() */
      for (unsigned i = 1; i < loop_header.instructions[0]->operands.size(); i++) {
         Operand& op = loop_header.instructions[0]->operands[i];
         if (op.getTemp() == prev)
            op.setTemp(renamed);
      }

      /* use the assignment from the loop preheader and fix def reg */
      assignment& var = ctx.assignments[prev.id()];
      ctx.assignments[renamed.id()] = var;
      loop_header.instructions[0]->definitions[0].setFixed(var.reg);
   }

   /* rename loop carried phi operands */
   for (unsigned i = renames.size(); i < loop_header.instructions.size(); i++) {
      aco_ptr<Instruction>& phi = loop_header.instructions[i];
      if (!is_phi(phi))
         break;
      const Block::edge_vec& preds =
         phi->opcode == aco_opcode::p_phi ? loop_header.logical_preds : loop_header.linear_preds;
      for (unsigned j = 1; j < phi->operands.size(); j++) {
         Operand& op = phi->operands[j];
         if (!op.isTemp())
            continue;

         /* Find the original name, since this operand might not use the original name if the phi
          * was created after init_reg_file().
          */
         auto it = ctx.orig_names.find(op.tempId());
         Temp orig = it != ctx.orig_names.end() ? it->second : op.getTemp();

         op.setTemp(read_variable(ctx, orig, preds[j]));
         op.setFixed(ctx.assignments[op.tempId()].reg);
      }
   }

   /* return early if no new phi was created */
   if (renames.empty())
      return;

   /* propagate new renames through loop */
   for (unsigned idx = loop_header_idx; idx < loop_exit_idx; idx++) {
      Block& current = ctx.program->blocks[idx];
      /* rename all uses in this block */
      for (aco_ptr<Instruction>& instr : current.instructions) {
         /* phis are renamed after RA */
         if (idx == loop_header_idx && is_phi(instr))
            continue;

         for (Operand& op : instr->operands) {
            if (!op.isTemp())
               continue;

            auto rename = renames.find(op.tempId());
            if (rename != renames.end()) {
               assert(rename->second.id());
               op.setTemp(rename->second);
            }
         }
      }
   }
}

/**
 * This function serves the purpose to correctly initialize the register file
 * at the beginning of a block (before any existing phis).
 * In order to do so, all live-in variables are entered into the RegisterFile.
 * Reg-to-reg moves (renames) from previous blocks are taken into account and
 * the SSA is repaired by inserting corresponding phi-nodes.
 */
RegisterFile
init_reg_file(ra_ctx& ctx, const std::vector<IDSet>& live_out_per_block, Block& block)
{
   if (block.kind & block_kind_loop_exit) {
      uint32_t header = ctx.loop_header.back();
      ctx.loop_header.pop_back();
      handle_loop_phis(ctx, live_out_per_block[header], header, block.index);
   }

   RegisterFile register_file;
   const IDSet& live_in = live_out_per_block[block.index];
   assert(block.index != 0 || live_in.empty());

   if (block.kind & block_kind_loop_header) {
      ctx.loop_header.emplace_back(block.index);
      /* already rename phis incoming value */
      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (!is_phi(instr))
            break;
         Operand& operand = instr->operands[0];
         if (operand.isTemp()) {
            operand.setTemp(read_variable(ctx, operand.getTemp(), block.index - 1));
            operand.setFixed(ctx.assignments[operand.tempId()].reg);
         }
      }
      for (unsigned t : live_in) {
         Temp val = Temp(t, ctx.program->temp_rc[t]);
         Temp renamed = read_variable(ctx, val, block.index - 1);
         if (renamed != val)
            add_rename(ctx, val, renamed);
         assignment& var = ctx.assignments[renamed.id()];
         assert(var.assigned);
         register_file.fill(Definition(renamed, var.reg));
      }
   } else {
      /* rename phi operands */
      for (aco_ptr<Instruction>& instr : block.instructions) {
         if (!is_phi(instr))
            break;
         const Block::edge_vec& preds =
            instr->opcode == aco_opcode::p_phi ? block.logical_preds : block.linear_preds;

         for (unsigned i = 0; i < instr->operands.size(); i++) {
            Operand& operand = instr->operands[i];
            if (!operand.isTemp())
               continue;
            operand.setTemp(read_variable(ctx, operand.getTemp(), preds[i]));
            operand.setFixed(ctx.assignments[operand.tempId()].reg);
         }
      }
      for (unsigned t : live_in) {
         Temp val = Temp(t, ctx.program->temp_rc[t]);
         Temp renamed = handle_live_in(ctx, val, &block);
         assignment& var = ctx.assignments[renamed.id()];
         /* due to live-range splits, the live-in might be a phi, now */
         if (var.assigned) {
            register_file.fill(Definition(renamed, var.reg));
         }
         if (renamed != val) {
            add_rename(ctx, val, renamed);
         }
      }
   }

   return register_file;
}

bool
vop3_can_use_vop2acc(ra_ctx& ctx, Instruction* instr)
{
   if (!instr->isVOP3() && !instr->isVOP3P())
      return false;

   switch (instr->opcode) {
   case aco_opcode::v_mad_f32:
   case aco_opcode::v_mad_f16:
   case aco_opcode::v_mad_legacy_f16: break;
   case aco_opcode::v_fma_f32:
   case aco_opcode::v_pk_fma_f16:
   case aco_opcode::v_fma_f16:
   case aco_opcode::v_dot4_i32_i8:
      if (ctx.program->gfx_level < GFX10)
         return false;
      break;
   case aco_opcode::v_mad_legacy_f32:
      if (!ctx.program->dev.has_mac_legacy32)
         return false;
      break;
   case aco_opcode::v_fma_legacy_f32:
      if (!ctx.program->dev.has_fmac_legacy32)
         return false;
      break;
   default: return false;
   }

   if (!instr->operands[2].isOfType(RegType::vgpr) || !instr->operands[2].isKillBeforeDef() ||
       (!instr->operands[0].isOfType(RegType::vgpr) && !instr->operands[1].isOfType(RegType::vgpr)))
      return false;

   if (instr->isVOP3P()) {
      for (unsigned i = 0; i < 3; i++) {
         if (instr->operands[i].isLiteral())
            continue;

         if (instr->valu().opsel_lo[i])
            return false;

         /* v_pk_fmac_f16 inline constants are replicated to hi bits starting with gfx11. */
         if (instr->valu().opsel_hi[i] ==
             (instr->operands[i].isConstant() && ctx.program->gfx_level >= GFX11))
            return false;
      }
   } else {
      if (instr->valu().opsel & (ctx.program->gfx_level < GFX11 ? 0xf : ~0x3))
         return false;
      for (unsigned i = 0; i < 2; i++) {
         if (!instr->operands[i].isOfType(RegType::vgpr) && instr->valu().opsel[i])
            return false;
      }
   }

   unsigned im_mask = instr->isDPP16() && instr->isVOP3() ? 0x3 : 0;
   if (instr->valu().omod || instr->valu().clamp || (instr->valu().abs & ~im_mask) ||
       (instr->valu().neg & ~im_mask))
      return false;

   return true;
}

bool
sop2_can_use_sopk(ra_ctx& ctx, Instruction* instr)
{
   if (instr->opcode != aco_opcode::s_add_i32 && instr->opcode != aco_opcode::s_add_u32 &&
       instr->opcode != aco_opcode::s_mul_i32 && instr->opcode != aco_opcode::s_cselect_b32)
      return false;

   if (instr->opcode == aco_opcode::s_add_u32 && !instr->definitions[1].isKill())
      return false;

   uint32_t literal_idx = 0;

   if (instr->opcode != aco_opcode::s_cselect_b32 && instr->operands[1].isLiteral())
      literal_idx = 1;

   if (!instr->operands[!literal_idx].isTemp() || !instr->operands[!literal_idx].isKillBeforeDef())
      return false;

   if (!instr->operands[literal_idx].isLiteral())
      return false;

   const uint32_t i16_mask = 0xffff8000u;
   uint32_t value = instr->operands[literal_idx].constantValue();
   if ((value & i16_mask) && (value & i16_mask) != i16_mask)
      return false;

   return true;
}

void
create_phi_vector_affinities(ra_ctx& ctx, aco_ptr<Instruction>& instr,
                             std::map<Operand*, std::vector<vector_info>>& vector_phis)
{
   auto it = ctx.vectors.find(instr->definitions[0].tempId());
   if (it == ctx.vectors.end())
      return;
   vector_info& dest_vector = it->second;

   auto pair = vector_phis.try_emplace(dest_vector.parts, instr->operands.size(), dest_vector);
   std::vector<vector_info>& src_vectors = pair.first->second;
   if (pair.second) {
      RegType type = instr->definitions[0].regClass().type();

      for (vector_info& src_vector : src_vectors) {
         src_vector.parts =
            (Operand*)ctx.memory.allocate(sizeof(Operand) * src_vector.num_parts, alignof(Operand));
         for (unsigned j = 0; j < src_vector.num_parts; j++)
            src_vector.parts[j] = Operand(RegClass::get(type, dest_vector.parts[j].bytes()));
      }
   }

   unsigned index = 0;
   for (; index < dest_vector.num_parts; index++) {
      if (dest_vector.parts[index].isTemp() &&
          dest_vector.parts[index].tempId() == instr->definitions[0].tempId())
         break;
   }
   assert(index != dest_vector.num_parts);

   for (int i = instr->operands.size() - 1; i >= 0; i--) {
      const Operand& op = instr->operands[i];
      if (!op.isTemp() || op.regClass() != instr->definitions[0].regClass())
         continue;

      src_vectors[i].parts[index] = op;
      ctx.vectors[op.tempId()] = src_vectors[i];
   }
}

void
get_affinities(ra_ctx& ctx)
{
   std::vector<std::vector<Temp>> phi_resources;
   aco::unordered_map<uint32_t, uint32_t> temp_to_phi_resources(ctx.memory);

   for (auto block_rit = ctx.program->blocks.rbegin(); block_rit != ctx.program->blocks.rend();
        block_rit++) {
      Block& block = *block_rit;

      std::vector<aco_ptr<Instruction>>::reverse_iterator rit;
      for (rit = block.instructions.rbegin(); rit != block.instructions.rend(); ++rit) {
         aco_ptr<Instruction>& instr = *rit;
         if (is_phi(instr))
            break;

         /* add vector affinities */
         if (instr->opcode == aco_opcode::p_create_vector) {
            for (const Operand& op : instr->operands) {
               if (op.isTemp() && op.isFirstKill() &&
                   op.getTemp().type() == instr->definitions[0].getTemp().type())
                  ctx.vectors[op.tempId()] = vector_info(instr.get());
            }
         } else if (instr->format == Format::MIMG && instr->operands.size() > 4 &&
                    !instr->mimg().strict_wqm && ctx.program->gfx_level < GFX12) {
            for (unsigned i = 3; i < instr->operands.size(); i++)
               ctx.vectors[instr->operands[i].tempId()] = vector_info(instr.get(), 3, true);
         } else if (instr->opcode == aco_opcode::p_split_vector &&
                    instr->operands[0].isFirstKillBeforeDef()) {
            ctx.split_vectors[instr->operands[0].tempId()] = instr.get();
         } else if (instr->isVOPC() && !instr->isVOP3()) {
            if (!instr->isSDWA() || ctx.program->gfx_level == GFX8)
               ctx.assignments[instr->definitions[0].tempId()].vcc = true;
         } else if (instr->isVOP2() && !instr->isVOP3()) {
            if (instr->operands.size() == 3 && instr->operands[2].isTemp() &&
                instr->operands[2].regClass().type() == RegType::sgpr)
               ctx.assignments[instr->operands[2].tempId()].vcc = true;
            if (instr->definitions.size() == 2)
               ctx.assignments[instr->definitions[1].tempId()].vcc = true;
         } else if (instr->opcode == aco_opcode::s_and_b32 ||
                    instr->opcode == aco_opcode::s_and_b64) {
            /* If SCC is used by a branch, we might be able to use
             * s_cbranch_vccz/s_cbranch_vccnz if the operand is VCC.
             */
            if (!instr->definitions[1].isKill() && instr->operands[0].isTemp() &&
                instr->operands[1].isFixed() && instr->operands[1].physReg() == exec)
               ctx.assignments[instr->operands[0].tempId()].vcc = true;
         } else if (instr->opcode == aco_opcode::s_sendmsg) {
            ctx.assignments[instr->operands[0].tempId()].m0 = true;
         }

         int op_fixed_to_def0 = get_op_fixed_to_def(instr.get());
         for (unsigned i = 0; i < instr->definitions.size(); i++) {
            const Definition& def = instr->definitions[i];
            if (!def.isTemp())
               continue;
            /* mark last-seen phi operand */
            auto it = temp_to_phi_resources.find(def.tempId());
            if (it != temp_to_phi_resources.end() &&
                def.regClass() == phi_resources[it->second][0].regClass()) {
               phi_resources[it->second][0] = def.getTemp();
               /* try to coalesce phi affinities with parallelcopies */
               Operand op;
               if (instr->opcode == aco_opcode::p_parallelcopy) {
                  op = instr->operands[i];
               } else if (i == 0 && op_fixed_to_def0 != -1) {
                  op = instr->operands[op_fixed_to_def0];
               } else if (vop3_can_use_vop2acc(ctx, instr.get())) {
                  op = instr->operands[2];
               } else if (i == 0 && sop2_can_use_sopk(ctx, instr.get())) {
                  op = instr->operands[instr->operands[0].isLiteral()];
               } else {
                  continue;
               }

               if (op.isTemp() && op.isFirstKillBeforeDef() && def.regClass() == op.regClass()) {
                  phi_resources[it->second].emplace_back(op.getTemp());
                  temp_to_phi_resources[op.tempId()] = it->second;
               }
            }
         }
      }

      /* collect phi affinities */
      std::map<Operand*, std::vector<vector_info>> vector_phis;
      for (; rit != block.instructions.rend(); ++rit) {
         aco_ptr<Instruction>& instr = *rit;
         assert(is_phi(instr));

         if (instr->definitions[0].isKill() || instr->definitions[0].isFixed())
            continue;

         assert(instr->definitions[0].isTemp());
         auto it = temp_to_phi_resources.find(instr->definitions[0].tempId());
         unsigned index = phi_resources.size();
         std::vector<Temp>* affinity_related;
         if (it != temp_to_phi_resources.end()) {
            index = it->second;
            phi_resources[index][0] = instr->definitions[0].getTemp();
            affinity_related = &phi_resources[index];
         } else {
            phi_resources.emplace_back(std::vector<Temp>{instr->definitions[0].getTemp()});
            affinity_related = &phi_resources.back();
         }

         for (const Operand& op : instr->operands) {
            if (op.isTemp() && op.isKill() && op.regClass() == instr->definitions[0].regClass()) {
               affinity_related->emplace_back(op.getTemp());
               if (block.kind & block_kind_loop_header)
                  continue;
               temp_to_phi_resources[op.tempId()] = index;
            }
         }

         create_phi_vector_affinities(ctx, instr, vector_phis);
      }

      /* visit the loop header phis first in order to create nested affinities */
      if (block.kind & block_kind_loop_exit) {
         /* find loop header */
         auto header_rit = block_rit;
         while ((header_rit + 1)->loop_nest_depth > block.loop_nest_depth)
            header_rit++;

         for (aco_ptr<Instruction>& phi : header_rit->instructions) {
            if (!is_phi(phi))
               break;
            if (phi->definitions[0].isKill() || phi->definitions[0].isFixed())
               continue;

            /* create an (empty) merge-set for the phi-related variables */
            auto it = temp_to_phi_resources.find(phi->definitions[0].tempId());
            unsigned index = phi_resources.size();
            if (it == temp_to_phi_resources.end()) {
               temp_to_phi_resources[phi->definitions[0].tempId()] = index;
               phi_resources.emplace_back(std::vector<Temp>{phi->definitions[0].getTemp()});
            } else {
               index = it->second;
            }
            for (unsigned i = 1; i < phi->operands.size(); i++) {
               const Operand& op = phi->operands[i];
               if (op.isTemp() && op.isKill() && op.regClass() == phi->definitions[0].regClass()) {
                  temp_to_phi_resources[op.tempId()] = index;
               }
            }
         }
      }
   }
   /* create affinities */
   for (std::vector<Temp>& vec : phi_resources) {
      for (unsigned i = 1; i < vec.size(); i++)
         if (vec[i].id() != vec[0].id())
            ctx.assignments[vec[i].id()].affinity = vec[0].id();
   }
}

void
optimize_encoding_vop2(ra_ctx& ctx, RegisterFile& register_file, aco_ptr<Instruction>& instr)
{
   if (!vop3_can_use_vop2acc(ctx, instr.get()))
      return;

   for (unsigned i = ctx.program->gfx_level < GFX11 ? 0 : 2; i < 3; i++) {
      if (instr->operands[i].physReg().byte())
         return;
   }

   unsigned def_id = instr->definitions[0].tempId();
   if (ctx.assignments[def_id].affinity) {
      assignment& affinity = ctx.assignments[ctx.assignments[def_id].affinity];
      if (affinity.assigned && affinity.reg != instr->operands[2].physReg() &&
          !register_file.test(affinity.reg, instr->operands[2].bytes()))
         return;
   }

   if (!instr->operands[1].isOfType(RegType::vgpr))
      instr->valu().swapOperands(0, 1);

   if (instr->isVOP3P() && instr->operands[0].isLiteral()) {
      unsigned literal = instr->operands[0].constantValue();
      unsigned lo = (literal >> (instr->valu().opsel_lo[0] * 16)) & 0xffff;
      unsigned hi = (literal >> (instr->valu().opsel_hi[0] * 16)) & 0xffff;
      instr->operands[0] = Operand::literal32(lo | (hi << 16));
   }

   instr->format = (Format)(((unsigned)withoutVOP3(instr->format) & ~(unsigned)Format::VOP3P) |
                            (unsigned)Format::VOP2);
   instr->valu().opsel_lo = 0;
   instr->valu().opsel_hi = 0;
   switch (instr->opcode) {
   case aco_opcode::v_mad_f32: instr->opcode = aco_opcode::v_mac_f32; break;
   case aco_opcode::v_fma_f32: instr->opcode = aco_opcode::v_fmac_f32; break;
   case aco_opcode::v_mad_f16:
   case aco_opcode::v_mad_legacy_f16: instr->opcode = aco_opcode::v_mac_f16; break;
   case aco_opcode::v_fma_f16: instr->opcode = aco_opcode::v_fmac_f16; break;
   case aco_opcode::v_pk_fma_f16: instr->opcode = aco_opcode::v_pk_fmac_f16; break;
   case aco_opcode::v_dot4_i32_i8: instr->opcode = aco_opcode::v_dot4c_i32_i8; break;
   case aco_opcode::v_mad_legacy_f32: instr->opcode = aco_opcode::v_mac_legacy_f32; break;
   case aco_opcode::v_fma_legacy_f32: instr->opcode = aco_opcode::v_fmac_legacy_f32; break;
   default: break;
   }
}

void
optimize_encoding_sopk(ra_ctx& ctx, RegisterFile& register_file, aco_ptr<Instruction>& instr)
{
   /* try to optimize sop2 with literal source to sopk */
   if (!sop2_can_use_sopk(ctx, instr.get()))
      return;
   unsigned literal_idx = instr->operands[1].isLiteral();

   if (instr->operands[!literal_idx].physReg() >= 128)
      return;

   unsigned def_id = instr->definitions[0].tempId();
   if (ctx.assignments[def_id].affinity) {
      assignment& affinity = ctx.assignments[ctx.assignments[def_id].affinity];
      if (affinity.assigned && affinity.reg != instr->operands[!literal_idx].physReg() &&
          !register_file.test(affinity.reg, instr->operands[!literal_idx].bytes()))
         return;
   }

   instr->format = Format::SOPK;
   instr->salu().imm = instr->operands[literal_idx].constantValue() & 0xffff;
   if (literal_idx == 0)
      std::swap(instr->operands[0], instr->operands[1]);
   if (instr->operands.size() > 2)
      std::swap(instr->operands[1], instr->operands[2]);
   instr->operands.pop_back();

   switch (instr->opcode) {
   case aco_opcode::s_add_u32:
   case aco_opcode::s_add_i32: instr->opcode = aco_opcode::s_addk_i32; break;
   case aco_opcode::s_mul_i32: instr->opcode = aco_opcode::s_mulk_i32; break;
   case aco_opcode::s_cselect_b32: instr->opcode = aco_opcode::s_cmovk_i32; break;
   default: unreachable("illegal instruction");
   }
}

void
optimize_encoding(ra_ctx& ctx, RegisterFile& register_file, aco_ptr<Instruction>& instr)
{
   if (instr->isVALU())
      optimize_encoding_vop2(ctx, register_file, instr);
   if (instr->isSALU())
      optimize_encoding_sopk(ctx, register_file, instr);
}

void
emit_parallel_copy_internal(ra_ctx& ctx, std::vector<std::pair<Operand, Definition>>& parallelcopy,
                            aco_ptr<Instruction>& instr,
                            std::vector<aco_ptr<Instruction>>& instructions, bool temp_in_scc,
                            RegisterFile& register_file)
{
   if (parallelcopy.empty())
      return;

   aco_ptr<Instruction> pc;
   pc.reset(create_instruction(aco_opcode::p_parallelcopy, Format::PSEUDO, parallelcopy.size(),
                               parallelcopy.size()));
   bool linear_vgpr = false;
   bool sgpr_operands_alias_defs = false;
   uint64_t sgpr_operands[4] = {0, 0, 0, 0};
   for (unsigned i = 0; i < parallelcopy.size(); i++) {
      linear_vgpr |= parallelcopy[i].first.regClass().is_linear_vgpr();

      if (temp_in_scc && parallelcopy[i].first.isTemp() &&
          parallelcopy[i].first.getTemp().type() == RegType::sgpr) {
         if (!sgpr_operands_alias_defs) {
            unsigned reg = parallelcopy[i].first.physReg().reg();
            unsigned size = parallelcopy[i].first.getTemp().size();
            sgpr_operands[reg / 64u] |= u_bit_consecutive64(reg % 64u, size);

            reg = parallelcopy[i].second.physReg().reg();
            size = parallelcopy[i].second.getTemp().size();
            if (sgpr_operands[reg / 64u] & u_bit_consecutive64(reg % 64u, size))
               sgpr_operands_alias_defs = true;
         }
      }

      pc->operands[i] = parallelcopy[i].first;
      pc->definitions[i] = parallelcopy[i].second;
      assert(pc->operands[i].size() == pc->definitions[i].size());

      /* it might happen that the operand is already renamed. we have to restore the
       * original name. */
      auto it = ctx.orig_names.find(pc->operands[i].tempId());
      Temp orig = it != ctx.orig_names.end() ? it->second : pc->operands[i].getTemp();
      add_rename(ctx, orig, pc->definitions[i].getTemp());
   }

   if (temp_in_scc && (sgpr_operands_alias_defs || linear_vgpr)) {
      /* disable definitions and re-enable operands */
      RegisterFile tmp_file(register_file);
      for (const Definition& def : instr->definitions) {
         if (def.isTemp() && !def.isKill())
            tmp_file.clear(def);
      }
      for (const Operand& op : instr->operands) {
         if (op.isTemp() && op.isFirstKill())
            tmp_file.block(op.physReg(), op.regClass());
      }

      handle_pseudo(ctx, tmp_file, pc.get());
   } else {
      pc->pseudo().needs_scratch_reg = sgpr_operands_alias_defs || linear_vgpr;
      pc->pseudo().tmp_in_scc = false;
   }

   instructions.emplace_back(std::move(pc));

   parallelcopy.clear();
}

void
emit_parallel_copy(ra_ctx& ctx, std::vector<std::pair<Operand, Definition>>& parallelcopy,
                   aco_ptr<Instruction>& instr, std::vector<aco_ptr<Instruction>>& instructions,
                   bool temp_in_scc, RegisterFile& register_file)
{
   if (parallelcopy.empty())
      return;

   std::vector<std::pair<Operand, Definition>> linear_vgpr;
   if (ctx.num_linear_vgprs) {
      unsigned next = 0;
      for (unsigned i = 0; i < parallelcopy.size(); i++) {
         if (parallelcopy[i].first.regClass().is_linear_vgpr()) {
            linear_vgpr.push_back(parallelcopy[i]);
            continue;
         }

         if (next != i)
            parallelcopy[next] = parallelcopy[i];
         next++;
      }
      parallelcopy.resize(next);
   }

   /* Because of how linear VGPRs are allocated, we should never have to move a linear VGPR into the
    * space of a normal one. This means the copy can be done entirely before normal VGPR copies. */
   emit_parallel_copy_internal(ctx, linear_vgpr, instr, instructions, temp_in_scc,
                               register_file);
   emit_parallel_copy_internal(ctx, parallelcopy, instr, instructions, temp_in_scc,
                               register_file);
}

} /* end namespace */

void
register_allocation(Program* program, ra_test_policy policy)
{
   ra_ctx ctx(program, policy);
   get_affinities(ctx);

   for (Block& block : program->blocks) {
      ctx.block = &block;

      /* initialize register file */
      RegisterFile register_file = init_reg_file(ctx, program->live.live_in, block);
      ctx.war_hint.reset();
      ctx.rr_vgpr_it = {PhysReg{256}};
      ctx.rr_sgpr_it = {PhysReg{0}};

      std::vector<aco_ptr<Instruction>> instructions;
      instructions.reserve(block.instructions.size());

      /* this is a slight adjustment from the paper as we already have phi nodes:
       * We consider them incomplete phis and only handle the definition. */
      get_regs_for_phis(ctx, block, register_file, instructions,
                        program->live.live_in[block.index]);

      /* If this is a merge block, the state of the register file at the branch instruction of the
       * predecessors corresponds to the state after phis at the merge block. So, we allocate a
       * register for the predecessor's branch definitions as if there was a phi.
       */
      if (!block.linear_preds.empty() &&
          (block.linear_preds.size() != 1 ||
           program->blocks[block.linear_preds[0]].linear_succs.size() == 1)) {
         PhysReg br_reg = get_reg_phi(ctx, program->live.live_in[block.index], register_file,
                                      instructions, block, ctx.phi_dummy, Temp(0, s2));
         for (unsigned pred : block.linear_preds) {
            program->blocks[pred].scc_live_out = register_file[scc];
            aco_ptr<Instruction>& br = program->blocks[pred].instructions.back();

            assert(br->definitions.size() == 1 && br->definitions[0].regClass() == s2 &&
                   br->definitions[0].isKill());

            br->definitions[0].setFixed(br_reg);
         }
      }

      /* Handle all other instructions of the block */
      auto NonPhi = [](aco_ptr<Instruction>& instr) -> bool { return instr && !is_phi(instr); };
      auto instr_it = std::find_if(block.instructions.begin(), block.instructions.end(), NonPhi);
      for (; instr_it != block.instructions.end(); ++instr_it) {
         aco_ptr<Instruction>& instr = *instr_it;
         std::vector<std::pair<Operand, Definition>> parallelcopy;
         bool temp_in_scc = register_file[scc];

         if (instr->opcode == aco_opcode::p_branch) {
            /* unconditional branches are handled after phis of the target */
            instructions.emplace_back(std::move(instr));
            break;
         }

         assert(!is_phi(instr));

         /* handle operands */
         bool fixed = false;
         for (unsigned i = 0; i < instr->operands.size(); ++i) {
            auto& operand = instr->operands[i];
            if (!operand.isTemp())
               continue;

            /* rename operands */
            operand.setTemp(read_variable(ctx, operand.getTemp(), block.index));
            assert(ctx.assignments[operand.tempId()].assigned);

            fixed |=
               operand.isFixed() && ctx.assignments[operand.tempId()].reg != operand.physReg();
         }

         bool is_writelane = instr->opcode == aco_opcode::v_writelane_b32 ||
                             instr->opcode == aco_opcode::v_writelane_b32_e64;
         if (program->gfx_level <= GFX9 && is_writelane && instr->operands[0].isTemp() &&
             instr->operands[1].isTemp()) {
            /* v_writelane_b32 can take two sgprs but only if one is m0. */
            if (ctx.assignments[instr->operands[0].tempId()].reg != m0 &&
                ctx.assignments[instr->operands[1].tempId()].reg != m0) {
               instr->operands[0].setFixed(m0);
               fixed = true;
            }
         }

         if (fixed)
            handle_fixed_operands(ctx, register_file, parallelcopy, instr);

         for (unsigned i = 0; i < instr->operands.size(); ++i) {
            auto& operand = instr->operands[i];
            if (!operand.isTemp() || operand.isFixed())
               continue;

            PhysReg reg = ctx.assignments[operand.tempId()].reg;
            if (operand_can_use_reg(program->gfx_level, instr, i, reg, operand.regClass()))
               operand.setFixed(reg);
            else
               get_reg_for_operand(ctx, register_file, parallelcopy, instr, operand, i);

            if (instr->isEXP() || (instr->isVMEM() && i == 3 && ctx.program->gfx_level == GFX6) ||
                (instr->isDS() && instr->ds().gds)) {
               for (unsigned j = 0; j < operand.size(); j++)
                  ctx.war_hint.set(operand.physReg().reg() + j);
            }
         }

         /* remove dead vars from register file */
         for (const Operand& op : instr->operands) {
            if (op.isTemp() && op.isFirstKillBeforeDef())
               register_file.clear(op);
         }

         optimize_encoding(ctx, register_file, instr);

         /* Handle definitions which must have the same register as an operand.
          * We expect that the definition has the same size as the operand, otherwise the new
          * location for the operand (if it's not killed) might intersect with the old one.
          * We can't read from the old location because it's corrupted, and we can't write the new
          * location because that's used by a live-through operand.
          */
         int op_fixed_to_def = get_op_fixed_to_def(instr.get());
         if (op_fixed_to_def != -1)
            instr->definitions[0].setFixed(instr->operands[op_fixed_to_def].physReg());

         /* handle fixed definitions first */
         for (unsigned i = 0; i < instr->definitions.size(); ++i) {
            auto& definition = instr->definitions[i];
            if (!definition.isFixed())
               continue;

            adjust_max_used_regs(ctx, definition.regClass(), definition.physReg());
            /* check if the target register is blocked */
            if (register_file.test(definition.physReg(), definition.bytes())) {
               const PhysRegInterval def_regs{definition.physReg(), definition.size()};

               /* create parallelcopy pair to move blocking vars */
               std::vector<unsigned> vars = collect_vars(ctx, register_file, def_regs);

               RegisterFile tmp_file(register_file);
               /* re-enable the killed operands, so that we don't move the blocking vars there */
               tmp_file.fill_killed_operands(instr.get());

               ASSERTED bool success = false;
               success = get_regs_for_copies(ctx, tmp_file, parallelcopy, vars, instr, def_regs);
               assert(success);

               update_renames(ctx, register_file, parallelcopy, instr, (UpdateRenames)0);
            }

            if (!definition.isTemp())
               continue;

            ctx.assignments[definition.tempId()].set(definition);
            register_file.fill(definition);
         }

         /* handle all other definitions */
         for (unsigned i = 0; i < instr->definitions.size(); ++i) {
            Definition* definition = &instr->definitions[i];

            if (definition->isFixed() || !definition->isTemp())
               continue;

            /* find free reg */
            if (instr->opcode == aco_opcode::p_start_linear_vgpr) {
               /* Allocation of linear VGPRs is special. */
               definition->setFixed(alloc_linear_vgpr(ctx, register_file, instr, parallelcopy));
               update_renames(ctx, register_file, parallelcopy, instr, rename_not_killed_ops);
            } else if (instr->opcode == aco_opcode::p_split_vector) {
               PhysReg reg = instr->operands[0].physReg();
               RegClass rc = definition->regClass();
               for (unsigned j = 0; j < i; j++)
                  reg.reg_b += instr->definitions[j].bytes();
               if (get_reg_specified(ctx, register_file, rc, instr, reg, -1)) {
                  definition->setFixed(reg);
               } else if (i == 0) {
                  RegClass vec_rc = RegClass::get(rc.type(), instr->operands[0].bytes());
                  DefInfo info(ctx, ctx.pseudo_dummy, vec_rc, -1);
                  std::optional<PhysReg> res = get_reg_simple(ctx, register_file, info);
                  if (res && get_reg_specified(ctx, register_file, rc, instr, *res, -1))
                     definition->setFixed(*res);
               } else if (instr->definitions[i - 1].isFixed()) {
                  reg = instr->definitions[i - 1].physReg();
                  reg.reg_b += instr->definitions[i - 1].bytes();
                  if (get_reg_specified(ctx, register_file, rc, instr, reg, -1))
                     definition->setFixed(reg);
               }
            } else if (instr->opcode == aco_opcode::p_parallelcopy) {
               PhysReg reg = instr->operands[i].physReg();
               if (instr->operands[i].isTemp() &&
                   instr->operands[i].getTemp().type() == definition->getTemp().type() &&
                   !register_file.test(reg, definition->bytes()))
                  definition->setFixed(reg);
            } else if (instr->opcode == aco_opcode::p_extract_vector) {
               PhysReg reg = instr->operands[0].physReg();
               reg.reg_b += definition->bytes() * instr->operands[1].constantValue();
               if (get_reg_specified(ctx, register_file, definition->regClass(), instr, reg, -1))
                  definition->setFixed(reg);
            } else if (instr->opcode == aco_opcode::p_create_vector) {
               PhysReg reg = get_reg_create_vector(ctx, register_file, definition->getTemp(),
                                                   parallelcopy, instr);
               update_renames(ctx, register_file, parallelcopy, instr, (UpdateRenames)0);
               definition->setFixed(reg);
            } else if (instr_info.classes[(int)instr->opcode] == instr_class::wmma &&
                       instr->operands[2].isTemp() && instr->operands[2].isKill() &&
                       instr->operands[2].regClass() == definition->regClass()) {
               /* For WMMA, the dest needs to either be equal to operands[2], or not overlap it.
                * Here we set a policy of forcing them the same if operands[2] gets killed (and
                * otherwise they don't overlap). This may not be optimal if RA would select a
                * different location due to affinity, but that gets complicated very quickly. */
               definition->setFixed(instr->operands[2].physReg());
            }

            if (!definition->isFixed()) {
               Temp tmp = definition->getTemp();
               if (definition->regClass().is_subdword() && definition->bytes() < 4) {
                  PhysReg reg = get_reg(ctx, register_file, tmp, parallelcopy, instr);
                  definition->setFixed(reg);
                  if (reg.byte() || register_file.test(reg, 4)) {
                     bool allow_16bit_write = reg.byte() % 2 == 0 && !register_file.test(reg, 2);
                     add_subdword_definition(program, instr, reg, allow_16bit_write);
                     definition = &instr->definitions[i]; /* add_subdword_definition can invalidate
                                                             the reference */
                  }
               } else {
                  definition->setFixed(get_reg(ctx, register_file, tmp, parallelcopy, instr));
               }
               update_renames(ctx, register_file, parallelcopy, instr,
                              instr->opcode != aco_opcode::p_create_vector ? rename_not_killed_ops
                                                                           : (UpdateRenames)0);
            }

            assert(
               definition->isFixed() &&
               ((definition->getTemp().type() == RegType::vgpr && definition->physReg() >= 256) ||
                (definition->getTemp().type() != RegType::vgpr && definition->physReg() < 256)));
            ctx.assignments[definition->tempId()].set(*definition);
            register_file.fill(*definition);
         }

         handle_pseudo(ctx, register_file, instr.get());

         /* kill definitions and late-kill operands and ensure that sub-dword operands can actually
          * be read */
         for (const Definition& def : instr->definitions) {
            if (def.isTemp() && def.isKill())
               register_file.clear(def);
         }
         for (unsigned i = 0; i < instr->operands.size(); i++) {
            const Operand& op = instr->operands[i];
            if (op.isTemp() && op.isFirstKill() && op.isLateKill())
               register_file.clear(op);
            if (op.isTemp() && op.physReg().byte() != 0)
               add_subdword_operand(ctx, instr, i, op.physReg().byte(), op.regClass());
         }

         emit_parallel_copy(ctx, parallelcopy, instr, instructions, temp_in_scc, register_file);

         /* some instructions need VOP3 encoding if operand/definition is not assigned to VCC */
         bool instr_needs_vop3 =
            !instr->isVOP3() &&
            ((withoutDPP(instr->format) == Format::VOPC &&
              instr->definitions[0].physReg() != vcc) ||
             (instr->opcode == aco_opcode::v_cndmask_b32 && instr->operands[2].physReg() != vcc) ||
             ((instr->opcode == aco_opcode::v_add_co_u32 ||
               instr->opcode == aco_opcode::v_addc_co_u32 ||
               instr->opcode == aco_opcode::v_sub_co_u32 ||
               instr->opcode == aco_opcode::v_subb_co_u32 ||
               instr->opcode == aco_opcode::v_subrev_co_u32 ||
               instr->opcode == aco_opcode::v_subbrev_co_u32) &&
              instr->definitions[1].physReg() != vcc) ||
             ((instr->opcode == aco_opcode::v_addc_co_u32 ||
               instr->opcode == aco_opcode::v_subb_co_u32 ||
               instr->opcode == aco_opcode::v_subbrev_co_u32) &&
              instr->operands[2].physReg() != vcc));
         if (instr_needs_vop3) {

            /* If the first operand is a literal, we have to move it to an sgpr
             * for generations without VOP3+literal support.
             * Both literals and sgprs count towards the constant bus limit,
             * so this is always valid.
             */
            if (instr->operands.size() && instr->operands[0].isLiteral() &&
                program->gfx_level < GFX10) {
               /* Re-use the register we already allocated for the definition.
                * This works because the instruction cannot have any other SGPR operand.
                */
               Temp tmp = program->allocateTmp(instr->operands[0].size() == 2 ? s2 : s1);
               const Definition& def =
                  instr->isVOPC() ? instr->definitions[0] : instr->definitions.back();
               assert(def.regClass() == s2);
               ctx.assignments.emplace_back(def.physReg(), tmp.regClass());

               Instruction* copy =
                  create_instruction(aco_opcode::p_parallelcopy, Format::PSEUDO, 1, 1);
               copy->operands[0] = instr->operands[0];
               if (copy->operands[0].bytes() < 4)
                  copy->operands[0] = Operand::c32(copy->operands[0].constantValue());
               copy->definitions[0] = Definition(tmp);
               copy->definitions[0].setFixed(def.physReg());

               instr->operands[0] = Operand(tmp);
               instr->operands[0].setFixed(def.physReg());
               instr->operands[0].setFirstKill(true);

               instructions.emplace_back(copy);
            }

            /* change the instruction to VOP3 to enable an arbitrary register pair as dst */
            instr->format = asVOP3(instr->format);
         }

         instructions.emplace_back(std::move(*instr_it));

      } /* end for Instr */

      if ((block.kind & block_kind_top_level) && block.linear_succs.empty()) {
         /* Reset this for block_kind_resume. */
         ctx.num_linear_vgprs = 0;

         ASSERTED PhysRegInterval vgpr_bounds = get_reg_bounds(ctx, RegType::vgpr, false);
         ASSERTED PhysRegInterval sgpr_bounds = get_reg_bounds(ctx, RegType::sgpr, false);
         assert(register_file.count_zero(vgpr_bounds) == ctx.vgpr_bounds);
         assert(register_file.count_zero(sgpr_bounds) == ctx.sgpr_bounds);
      } else if (should_compact_linear_vgprs(ctx, register_file)) {
         aco_ptr<Instruction> br = std::move(instructions.back());
         instructions.pop_back();

         bool temp_in_scc =
            register_file[scc] || (!br->operands.empty() && br->operands[0].physReg() == scc);

         std::vector<std::pair<Operand, Definition>> parallelcopy;
         compact_linear_vgprs(ctx, register_file, parallelcopy);
         update_renames(ctx, register_file, parallelcopy, br, rename_not_killed_ops);
         emit_parallel_copy_internal(ctx, parallelcopy, br, instructions, temp_in_scc, register_file);

         instructions.push_back(std::move(br));
      }

      block.instructions = std::move(instructions);
   } /* end for BB */

   /* num_gpr = rnd_up(max_used_gpr + 1) */
   program->config->num_vgprs =
      std::min<uint16_t>(get_vgpr_alloc(program, ctx.max_used_vgpr + 1), 256);
   program->config->num_sgprs = get_sgpr_alloc(program, ctx.max_used_sgpr + 1);

   program->progress = CompilationProgress::after_ra;
}

} // namespace aco
