/*
 * Copyright © 2017 Intel Corporation
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

#include "elk_reg.h"
#include "elk_eu_defines.h"
#include "dev/intel_device_info.h"

#define INVALID (-1)

enum hw_reg_type {
   ELK_HW_REG_TYPE_UD  = 0,
   ELK_HW_REG_TYPE_D   = 1,
   ELK_HW_REG_TYPE_UW  = 2,
   ELK_HW_REG_TYPE_W   = 3,
   ELK_HW_REG_TYPE_F   = 7,
   GFX8_HW_REG_TYPE_UQ = 8,
   GFX8_HW_REG_TYPE_Q  = 9,

   ELK_HW_REG_TYPE_UB  = 4,
   ELK_HW_REG_TYPE_B   = 5,
   GFX7_HW_REG_TYPE_DF = 6,
   GFX8_HW_REG_TYPE_HF = 10,

   GFX11_HW_REG_TYPE_UD = 0,
   GFX11_HW_REG_TYPE_D  = 1,
   GFX11_HW_REG_TYPE_UW = 2,
   GFX11_HW_REG_TYPE_W  = 3,
   GFX11_HW_REG_TYPE_UB = 4,
   GFX11_HW_REG_TYPE_B  = 5,
   GFX11_HW_REG_TYPE_UQ = 6,
   GFX11_HW_REG_TYPE_Q  = 7,
   GFX11_HW_REG_TYPE_HF = 8,
   GFX11_HW_REG_TYPE_F  = 9,
   GFX11_HW_REG_TYPE_DF = 10,
   GFX11_HW_REG_TYPE_NF = 11,
};

enum hw_imm_type {
   ELK_HW_IMM_TYPE_UD  = 0,
   ELK_HW_IMM_TYPE_D   = 1,
   ELK_HW_IMM_TYPE_UW  = 2,
   ELK_HW_IMM_TYPE_W   = 3,
   ELK_HW_IMM_TYPE_F   = 7,
   GFX8_HW_IMM_TYPE_UQ = 8,
   GFX8_HW_IMM_TYPE_Q  = 9,

   ELK_HW_IMM_TYPE_UV  = 4,
   ELK_HW_IMM_TYPE_VF  = 5,
   ELK_HW_IMM_TYPE_V   = 6,
   GFX8_HW_IMM_TYPE_DF = 10,
   GFX8_HW_IMM_TYPE_HF = 11,

   GFX11_HW_IMM_TYPE_UD = 0,
   GFX11_HW_IMM_TYPE_D  = 1,
   GFX11_HW_IMM_TYPE_UW = 2,
   GFX11_HW_IMM_TYPE_W  = 3,
   GFX11_HW_IMM_TYPE_UV = 4,
   GFX11_HW_IMM_TYPE_V  = 5,
   GFX11_HW_IMM_TYPE_UQ = 6,
   GFX11_HW_IMM_TYPE_Q  = 7,
   GFX11_HW_IMM_TYPE_HF = 8,
   GFX11_HW_IMM_TYPE_F  = 9,
   GFX11_HW_IMM_TYPE_DF = 10,
   GFX11_HW_IMM_TYPE_VF = 11,
};

#define GFX12_HW_REG_TYPE_UINT(n) (n)
#define GFX12_HW_REG_TYPE_SINT(n) (0x4 | (n))
#define GFX12_HW_REG_TYPE_FLOAT(n) (0x8 | (n))

static const struct hw_type {
   enum hw_reg_type reg_type;
   enum hw_imm_type imm_type;
} gfx4_hw_type[] = {
   [0 ... ELK_REGISTER_TYPE_LAST] = {     INVALID, INVALID             },

   [ELK_REGISTER_TYPE_F]  = { ELK_HW_REG_TYPE_F,   ELK_HW_IMM_TYPE_F   },
   [ELK_REGISTER_TYPE_VF] = { INVALID,             ELK_HW_IMM_TYPE_VF  },

   [ELK_REGISTER_TYPE_D]  = { ELK_HW_REG_TYPE_D,   ELK_HW_IMM_TYPE_D   },
   [ELK_REGISTER_TYPE_UD] = { ELK_HW_REG_TYPE_UD,  ELK_HW_IMM_TYPE_UD  },
   [ELK_REGISTER_TYPE_W]  = { ELK_HW_REG_TYPE_W,   ELK_HW_IMM_TYPE_W   },
   [ELK_REGISTER_TYPE_UW] = { ELK_HW_REG_TYPE_UW,  ELK_HW_IMM_TYPE_UW  },
   [ELK_REGISTER_TYPE_B]  = { ELK_HW_REG_TYPE_B,   INVALID             },
   [ELK_REGISTER_TYPE_UB] = { ELK_HW_REG_TYPE_UB,  INVALID             },
   [ELK_REGISTER_TYPE_V]  = { INVALID,             ELK_HW_IMM_TYPE_V   },
}, gfx6_hw_type[] = {
   [0 ... ELK_REGISTER_TYPE_LAST] = {     INVALID, INVALID             },

   [ELK_REGISTER_TYPE_F]  = { ELK_HW_REG_TYPE_F,   ELK_HW_IMM_TYPE_F   },
   [ELK_REGISTER_TYPE_VF] = { INVALID,             ELK_HW_IMM_TYPE_VF  },

   [ELK_REGISTER_TYPE_D]  = { ELK_HW_REG_TYPE_D,   ELK_HW_IMM_TYPE_D   },
   [ELK_REGISTER_TYPE_UD] = { ELK_HW_REG_TYPE_UD,  ELK_HW_IMM_TYPE_UD  },
   [ELK_REGISTER_TYPE_W]  = { ELK_HW_REG_TYPE_W,   ELK_HW_IMM_TYPE_W   },
   [ELK_REGISTER_TYPE_UW] = { ELK_HW_REG_TYPE_UW,  ELK_HW_IMM_TYPE_UW  },
   [ELK_REGISTER_TYPE_B]  = { ELK_HW_REG_TYPE_B,   INVALID             },
   [ELK_REGISTER_TYPE_UB] = { ELK_HW_REG_TYPE_UB,  INVALID             },
   [ELK_REGISTER_TYPE_V]  = { INVALID,             ELK_HW_IMM_TYPE_V   },
   [ELK_REGISTER_TYPE_UV] = { INVALID,             ELK_HW_IMM_TYPE_UV  },
}, gfx7_hw_type[] = {
   [0 ... ELK_REGISTER_TYPE_LAST] = {     INVALID, INVALID             },

   [ELK_REGISTER_TYPE_DF] = { GFX7_HW_REG_TYPE_DF, INVALID             },
   [ELK_REGISTER_TYPE_F]  = { ELK_HW_REG_TYPE_F,   ELK_HW_IMM_TYPE_F   },
   [ELK_REGISTER_TYPE_VF] = { INVALID,             ELK_HW_IMM_TYPE_VF  },

   [ELK_REGISTER_TYPE_D]  = { ELK_HW_REG_TYPE_D,   ELK_HW_IMM_TYPE_D   },
   [ELK_REGISTER_TYPE_UD] = { ELK_HW_REG_TYPE_UD,  ELK_HW_IMM_TYPE_UD  },
   [ELK_REGISTER_TYPE_W]  = { ELK_HW_REG_TYPE_W,   ELK_HW_IMM_TYPE_W   },
   [ELK_REGISTER_TYPE_UW] = { ELK_HW_REG_TYPE_UW,  ELK_HW_IMM_TYPE_UW  },
   [ELK_REGISTER_TYPE_B]  = { ELK_HW_REG_TYPE_B,   INVALID             },
   [ELK_REGISTER_TYPE_UB] = { ELK_HW_REG_TYPE_UB,  INVALID             },
   [ELK_REGISTER_TYPE_V]  = { INVALID,             ELK_HW_IMM_TYPE_V   },
   [ELK_REGISTER_TYPE_UV] = { INVALID,             ELK_HW_IMM_TYPE_UV  },
}, gfx8_hw_type[] = {
   [0 ... ELK_REGISTER_TYPE_LAST] = {     INVALID, INVALID             },

   [ELK_REGISTER_TYPE_DF] = { GFX7_HW_REG_TYPE_DF, GFX8_HW_IMM_TYPE_DF },
   [ELK_REGISTER_TYPE_F]  = { ELK_HW_REG_TYPE_F,   ELK_HW_IMM_TYPE_F   },
   [ELK_REGISTER_TYPE_HF] = { GFX8_HW_REG_TYPE_HF, GFX8_HW_IMM_TYPE_HF },
   [ELK_REGISTER_TYPE_VF] = { INVALID,             ELK_HW_IMM_TYPE_VF  },

   [ELK_REGISTER_TYPE_Q]  = { GFX8_HW_REG_TYPE_Q,  GFX8_HW_IMM_TYPE_Q  },
   [ELK_REGISTER_TYPE_UQ] = { GFX8_HW_REG_TYPE_UQ, GFX8_HW_IMM_TYPE_UQ },
   [ELK_REGISTER_TYPE_D]  = { ELK_HW_REG_TYPE_D,   ELK_HW_IMM_TYPE_D   },
   [ELK_REGISTER_TYPE_UD] = { ELK_HW_REG_TYPE_UD,  ELK_HW_IMM_TYPE_UD  },
   [ELK_REGISTER_TYPE_W]  = { ELK_HW_REG_TYPE_W,   ELK_HW_IMM_TYPE_W   },
   [ELK_REGISTER_TYPE_UW] = { ELK_HW_REG_TYPE_UW,  ELK_HW_IMM_TYPE_UW  },
   [ELK_REGISTER_TYPE_B]  = { ELK_HW_REG_TYPE_B,   INVALID             },
   [ELK_REGISTER_TYPE_UB] = { ELK_HW_REG_TYPE_UB,  INVALID             },
   [ELK_REGISTER_TYPE_V]  = { INVALID,             ELK_HW_IMM_TYPE_V   },
   [ELK_REGISTER_TYPE_UV] = { INVALID,             ELK_HW_IMM_TYPE_UV  },
};

/* SNB adds 3-src instructions (MAD and LRP) that only operate on floats, so
 * the types were implied. IVB adds BFE and BFI2 that operate on doublewords
 * and unsigned doublewords, so a new field is also available in the da3src
 * struct (part of struct elk_instruction.bits1 in elk_structs.h) to select
 * dst and shared-src types.
 *
 * CNL adds support for 3-src instructions in align1 mode, and with it support
 * for most register types.
 */
enum hw_3src_reg_type {
   GFX7_3SRC_TYPE_F  = 0,
   GFX7_3SRC_TYPE_D  = 1,
   GFX7_3SRC_TYPE_UD = 2,
   GFX7_3SRC_TYPE_DF = 3,
   GFX8_3SRC_TYPE_HF = 4,

   /** When ExecutionDatatype is 1: @{ */
   GFX10_ALIGN1_3SRC_REG_TYPE_HF = 0b000,
   GFX10_ALIGN1_3SRC_REG_TYPE_F  = 0b001,
   GFX10_ALIGN1_3SRC_REG_TYPE_DF = 0b010,
   GFX11_ALIGN1_3SRC_REG_TYPE_NF = 0b011,
   /** @} */

   /** When ExecutionDatatype is 0: @{ */
   GFX10_ALIGN1_3SRC_REG_TYPE_UD = 0b000,
   GFX10_ALIGN1_3SRC_REG_TYPE_D  = 0b001,
   GFX10_ALIGN1_3SRC_REG_TYPE_UW = 0b010,
   GFX10_ALIGN1_3SRC_REG_TYPE_W  = 0b011,
   GFX10_ALIGN1_3SRC_REG_TYPE_UB = 0b100,
   GFX10_ALIGN1_3SRC_REG_TYPE_B  = 0b101,
   /** @} */
};

static const struct hw_3src_type {
   enum hw_3src_reg_type reg_type;
   enum gfx10_align1_3src_exec_type exec_type;
} gfx6_hw_3src_type[] = {
   [0 ... ELK_REGISTER_TYPE_LAST] = { INVALID },

   [ELK_REGISTER_TYPE_F]  = { GFX7_3SRC_TYPE_F  },
}, gfx7_hw_3src_type[] = {
   [0 ... ELK_REGISTER_TYPE_LAST] = { INVALID },

   [ELK_REGISTER_TYPE_F]  = { GFX7_3SRC_TYPE_F  },
   [ELK_REGISTER_TYPE_D]  = { GFX7_3SRC_TYPE_D  },
   [ELK_REGISTER_TYPE_UD] = { GFX7_3SRC_TYPE_UD },
   [ELK_REGISTER_TYPE_DF] = { GFX7_3SRC_TYPE_DF },
}, gfx8_hw_3src_type[] = {
   [0 ... ELK_REGISTER_TYPE_LAST] = { INVALID },

   [ELK_REGISTER_TYPE_F]  = { GFX7_3SRC_TYPE_F  },
   [ELK_REGISTER_TYPE_D]  = { GFX7_3SRC_TYPE_D  },
   [ELK_REGISTER_TYPE_UD] = { GFX7_3SRC_TYPE_UD },
   [ELK_REGISTER_TYPE_DF] = { GFX7_3SRC_TYPE_DF },
   [ELK_REGISTER_TYPE_HF] = { GFX8_3SRC_TYPE_HF },
#undef E
};

/**
 * Convert a elk_reg_type enumeration value into the hardware representation.
 *
 * The hardware encoding may depend on whether the value is an immediate.
 */
unsigned
elk_reg_type_to_hw_type(const struct intel_device_info *devinfo,
                        enum elk_reg_file file,
                        enum elk_reg_type type)
{
   const struct hw_type *table;

   if (devinfo->ver >= 8) {
      assert(type < ARRAY_SIZE(gfx8_hw_type));
      table = gfx8_hw_type;
   } else if (devinfo->ver >= 7) {
      assert(type < ARRAY_SIZE(gfx7_hw_type));
      table = gfx7_hw_type;
   } else if (devinfo->ver >= 6) {
      assert(type < ARRAY_SIZE(gfx6_hw_type));
      table = gfx6_hw_type;
   } else {
      assert(type < ARRAY_SIZE(gfx4_hw_type));
      table = gfx4_hw_type;
   }

   if (file == ELK_IMMEDIATE_VALUE) {
      assert(table[type].imm_type != (enum hw_imm_type)INVALID);
      return table[type].imm_type;
   } else {
      assert(table[type].reg_type != (enum hw_reg_type)INVALID);
      return table[type].reg_type;
   }
}

/**
 * Convert the hardware representation into a elk_reg_type enumeration value.
 *
 * The hardware encoding may depend on whether the value is an immediate.
 */
enum elk_reg_type
elk_hw_type_to_reg_type(const struct intel_device_info *devinfo,
                        enum elk_reg_file file, unsigned hw_type)
{
   const struct hw_type *table;

   if (devinfo->ver >= 8) {
      table = gfx8_hw_type;
   } else if (devinfo->ver >= 7) {
      table = gfx7_hw_type;
   } else if (devinfo->ver >= 6) {
      table = gfx6_hw_type;
   } else {
      table = gfx4_hw_type;
   }

   if (file == ELK_IMMEDIATE_VALUE) {
      for (enum elk_reg_type i = 0; i <= ELK_REGISTER_TYPE_LAST; i++) {
         if (table[i].imm_type == (enum hw_imm_type)hw_type) {
            return i;
         }
      }
   } else {
      for (enum elk_reg_type i = 0; i <= ELK_REGISTER_TYPE_LAST; i++) {
         if (table[i].reg_type == (enum hw_reg_type)hw_type) {
            return i;
         }
      }
   }
   return INVALID_REG_TYPE;
}

/**
 * Convert a elk_reg_type enumeration value into the hardware representation
 * for a 3-src align16 instruction
 */
unsigned
elk_reg_type_to_a16_hw_3src_type(const struct intel_device_info *devinfo,
                                 enum elk_reg_type type)
{
   const struct hw_3src_type *table;

   if (devinfo->ver >= 8) {
      assert(type < ARRAY_SIZE(gfx8_hw_3src_type));
      table = gfx8_hw_3src_type;
   } else if (devinfo->ver >= 7) {
      assert(type < ARRAY_SIZE(gfx7_hw_3src_type));
      table = gfx7_hw_3src_type;
   } else {
      assert(type < ARRAY_SIZE(gfx6_hw_3src_type));
      table = gfx6_hw_3src_type;
   }

   assert(table[type].reg_type != (enum hw_3src_reg_type)INVALID);
   return table[type].reg_type;
}

/**
 * Convert the hardware representation for a 3-src align16 instruction into a
 * elk_reg_type enumeration value.
 */
enum elk_reg_type
elk_a16_hw_3src_type_to_reg_type(const struct intel_device_info *devinfo,
                                 unsigned hw_type)
{
   const struct hw_3src_type *table = NULL;

   if (devinfo->ver >= 8) {
      table = gfx8_hw_3src_type;
   } else if (devinfo->ver >= 7) {
      table = gfx7_hw_3src_type;
   } else if (devinfo->ver >= 6) {
      table = gfx6_hw_3src_type;
   }

   for (enum elk_reg_type i = 0; i <= ELK_REGISTER_TYPE_LAST; i++) {
      if (table[i].reg_type == hw_type) {
         return i;
      }
   }
   return INVALID_REG_TYPE;
}

/**
 * Return the element size given a register type.
 */
unsigned
elk_reg_type_to_size(enum elk_reg_type type)
{
   static const unsigned type_size[] = {
      [ELK_REGISTER_TYPE_NF] = 8,
      [ELK_REGISTER_TYPE_DF] = 8,
      [ELK_REGISTER_TYPE_F]  = 4,
      [ELK_REGISTER_TYPE_HF] = 2,
      [ELK_REGISTER_TYPE_VF] = 4,

      [ELK_REGISTER_TYPE_Q]  = 8,
      [ELK_REGISTER_TYPE_UQ] = 8,
      [ELK_REGISTER_TYPE_D]  = 4,
      [ELK_REGISTER_TYPE_UD] = 4,
      [ELK_REGISTER_TYPE_W]  = 2,
      [ELK_REGISTER_TYPE_UW] = 2,
      [ELK_REGISTER_TYPE_B]  = 1,
      [ELK_REGISTER_TYPE_UB] = 1,
      [ELK_REGISTER_TYPE_V]  = 2,
      [ELK_REGISTER_TYPE_UV] = 2,
   };
   if (type >= ARRAY_SIZE(type_size))
      return -1;

   return type_size[type];
}

/**
 * Converts a ELK_REGISTER_TYPE_* enum to a short string (F, UD, and so on).
 *
 * This is different than reg_encoding from elk_disasm.c in that it operates
 * on the abstract enum values, rather than the generation-specific encoding.
 */
const char *
elk_reg_type_to_letters(enum elk_reg_type type)
{
   static const char letters[][3] = {
      [ELK_REGISTER_TYPE_NF] = "NF",
      [ELK_REGISTER_TYPE_DF] = "DF",
      [ELK_REGISTER_TYPE_F]  = "F",
      [ELK_REGISTER_TYPE_HF] = "HF",
      [ELK_REGISTER_TYPE_VF] = "VF",

      [ELK_REGISTER_TYPE_Q]  = "Q",
      [ELK_REGISTER_TYPE_UQ] = "UQ",
      [ELK_REGISTER_TYPE_D]  = "D",
      [ELK_REGISTER_TYPE_UD] = "UD",
      [ELK_REGISTER_TYPE_W]  = "W",
      [ELK_REGISTER_TYPE_UW] = "UW",
      [ELK_REGISTER_TYPE_B]  = "B",
      [ELK_REGISTER_TYPE_UB] = "UB",
      [ELK_REGISTER_TYPE_V]  = "V",
      [ELK_REGISTER_TYPE_UV] = "UV",
   };
   if (type >= ARRAY_SIZE(letters))
      return "INVALID";

   assert(type < ARRAY_SIZE(letters));
   return letters[type];
}
