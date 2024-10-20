/*
 * Copyright 2021 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#include "agx_pack.h"
#include "layout.h"

#define AIL_ISA_FORMAT__ PIPE_FORMAT_NONE

#define AIL_FMT(pipe, channels_, type_, renderable_)                           \
   [PIPE_FORMAT_##pipe] = {                                                    \
      .channels = AGX_CHANNELS_##channels_,                                    \
      .type = AGX_TEXTURE_TYPE_##type_,                                        \
      .texturable = true,                                                      \
      .renderable = (enum pipe_format)AIL_ISA_FORMAT_##renderable_,            \
   }

/* clang-format off */
const struct ail_pixel_format_entry ail_pixel_format[PIPE_FORMAT_COUNT] = {
   AIL_FMT(R5G6B5_UNORM,            R5G6B5,                UNORM, F16),
   AIL_FMT(B5G6R5_UNORM,            R5G6B5,                UNORM, F16),

   AIL_FMT(R5G5B5A1_UNORM,          R5G5B5A1,              UNORM, F16),
   AIL_FMT(B5G5R5A1_UNORM,          R5G5B5A1,              UNORM, F16),
   AIL_FMT(R5G5B5X1_UNORM,          R5G5B5A1,              UNORM, F16),
   AIL_FMT(B5G5R5X1_UNORM,          R5G5B5A1,              UNORM, F16),

   AIL_FMT(R4G4B4A4_UNORM,          R4G4B4A4,              UNORM, F16),
   AIL_FMT(B4G4R4A4_UNORM,          R4G4B4A4,              UNORM, F16),
   AIL_FMT(A4B4G4R4_UNORM,          R4G4B4A4,              UNORM, F16),
   AIL_FMT(A4R4G4B4_UNORM,          R4G4B4A4,              UNORM, F16),

   AIL_FMT(R8_UNORM,                R8,                    UNORM, U8NORM),
   AIL_FMT(R8G8_UNORM,              R8G8,                  UNORM, U8NORM),
   AIL_FMT(R8G8B8A8_UNORM,          R8G8B8A8,              UNORM, U8NORM),
   AIL_FMT(A8R8G8B8_UNORM,          R8G8B8A8,              UNORM, U8NORM),
   AIL_FMT(A8B8G8R8_UNORM,          R8G8B8A8,              UNORM, U8NORM),
   AIL_FMT(B8G8R8A8_UNORM,          R8G8B8A8,              UNORM, U8NORM),
   AIL_FMT(R8G8B8X8_UNORM,          R8G8B8A8,              UNORM, U8NORM),
   AIL_FMT(X8R8G8B8_UNORM,          R8G8B8A8,              UNORM, U8NORM),
   AIL_FMT(X8B8G8R8_UNORM,          R8G8B8A8,              UNORM, U8NORM),
   AIL_FMT(B8G8R8X8_UNORM,          R8G8B8A8,              UNORM, U8NORM),

   AIL_FMT(R16_UNORM,               R16,                   UNORM, U16NORM),
   AIL_FMT(R16G16_UNORM,            R16G16,                UNORM, U16NORM),
   AIL_FMT(R16G16B16A16_UNORM,      R16G16B16A16,          UNORM, U16NORM),
   AIL_FMT(R16_SNORM,               R16,                   SNORM, S16NORM),
   AIL_FMT(R16G16_SNORM,            R16G16,                SNORM, S16NORM),
   AIL_FMT(R16G16B16A16_SNORM,      R16G16B16A16,          SNORM, S16NORM),

   AIL_FMT(R8_SRGB,                 R8,                    UNORM, SRGBA8),
   AIL_FMT(R8G8_SRGB,               R8G8,                  UNORM, SRGBA8),
   AIL_FMT(R8G8B8A8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),
   AIL_FMT(A8R8G8B8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),
   AIL_FMT(A8B8G8R8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),
   AIL_FMT(B8G8R8A8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),
   AIL_FMT(R8G8B8X8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),
   AIL_FMT(X8R8G8B8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),
   AIL_FMT(X8B8G8R8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),
   AIL_FMT(B8G8R8X8_SRGB,           R8G8B8A8,              UNORM, SRGBA8),

   AIL_FMT(R8_SNORM,                R8,                    SNORM, S8NORM),
   AIL_FMT(R8G8_SNORM,              R8G8,                  SNORM, S8NORM),
   AIL_FMT(R8G8B8A8_SNORM,          R8G8B8A8,              SNORM, S8NORM),
   AIL_FMT(A8R8G8B8_SNORM,          R8G8B8A8,              SNORM, S8NORM),
   AIL_FMT(A8B8G8R8_SNORM,          R8G8B8A8,              SNORM, S8NORM),
   AIL_FMT(B8G8R8A8_SNORM,          R8G8B8A8,              SNORM, S8NORM),
   AIL_FMT(R8G8B8X8_SNORM,          R8G8B8A8,              SNORM, S8NORM),
   AIL_FMT(X8R8G8B8_SNORM,          R8G8B8A8,              SNORM, S8NORM),
   AIL_FMT(X8B8G8R8_SNORM,          R8G8B8A8,              SNORM, S8NORM),
   AIL_FMT(B8G8R8X8_SNORM,          R8G8B8A8,              SNORM, S8NORM),

   AIL_FMT(R16_FLOAT,               R16,                   FLOAT, F16),
   AIL_FMT(R16G16_FLOAT,            R16G16,                FLOAT, F16),
   AIL_FMT(R16G16B16X16_FLOAT,      R16G16B16A16,          FLOAT, F16),
   AIL_FMT(R16G16B16A16_FLOAT,      R16G16B16A16,          FLOAT, F16),

   AIL_FMT(R32_FLOAT,               R32,                   FLOAT, I32),
   AIL_FMT(R32G32_FLOAT,            R32G32,                FLOAT, I32),
   AIL_FMT(R32G32B32X32_FLOAT,      R32G32B32A32,          FLOAT, I32),
   AIL_FMT(R32G32B32A32_FLOAT,      R32G32B32A32,          FLOAT, I32),

   AIL_FMT(R8_UINT,                 R8,                    UINT,  I8),
   AIL_FMT(R8G8_UINT,               R8G8,                  UINT,  I8),
   AIL_FMT(R8G8B8X8_UINT,           R8G8B8A8,              UINT,  I8),
   AIL_FMT(R8G8B8A8_UINT,           R8G8B8A8,              UINT,  I8),
   AIL_FMT(B8G8R8X8_UINT,           R8G8B8A8,              UINT,  I8),
   AIL_FMT(B8G8R8A8_UINT,           R8G8B8A8,              UINT,  I8),

   AIL_FMT(R16_UINT,                R16,                   UINT,  I16),
   AIL_FMT(R16G16_UINT,             R16G16,                UINT,  I16),
   AIL_FMT(R16G16B16X16_UINT,       R16G16B16A16,          UINT,  I16),
   AIL_FMT(R16G16B16A16_UINT,       R16G16B16A16,          UINT,  I16),

   AIL_FMT(R32_UINT,                R32,                   UINT,  I32),
   AIL_FMT(R32G32_UINT,             R32G32,                UINT,  I32),
   AIL_FMT(R32G32B32X32_UINT,       R32G32B32A32,          UINT,  I32),
   AIL_FMT(R32G32B32A32_UINT,       R32G32B32A32,          UINT,  I32),

   AIL_FMT(R8_SINT,                 R8,                    SINT,  I8),
   AIL_FMT(R8G8_SINT,               R8G8,                  SINT,  I8),
   AIL_FMT(R8G8B8X8_SINT,           R8G8B8A8,              SINT,  I8),
   AIL_FMT(R8G8B8A8_SINT,           R8G8B8A8,              SINT,  I8),
   AIL_FMT(B8G8R8X8_SINT,           R8G8B8A8,              SINT,  I8),
   AIL_FMT(B8G8R8A8_SINT,           R8G8B8A8,              SINT,  I8),

   AIL_FMT(R16_SINT,                R16,                   SINT,  I16),
   AIL_FMT(R16G16_SINT,             R16G16,                SINT,  I16),
   AIL_FMT(R16G16B16X16_SINT,       R16G16B16A16,          SINT,  I16),
   AIL_FMT(R16G16B16A16_SINT,       R16G16B16A16,          SINT,  I16),

   AIL_FMT(R32_SINT,                R32,                   SINT,  I32),
   AIL_FMT(R32G32_SINT,             R32G32,                SINT,  I32),
   AIL_FMT(R32G32B32X32_SINT,       R32G32B32A32,          SINT,  I32),
   AIL_FMT(R32G32B32A32_SINT,       R32G32B32A32,          SINT,  I32),

   AIL_FMT(Z16_UNORM,               R16,                   UNORM, _),
   AIL_FMT(Z32_FLOAT,               R32,                   FLOAT, _),
   AIL_FMT(Z32_FLOAT_S8X24_UINT,    R32,                   FLOAT, _),
   AIL_FMT(S8_UINT,                 R8,                    UINT,  _),

   /* The stencil part of Z32F + S8 is just S8 */
   AIL_FMT(X32_S8X24_UINT,          R8,                    UINT,  _),

   /* These must be lowered by u_transfer_helper to Z32F + S8 */
   AIL_FMT(Z24X8_UNORM,             R32,                   FLOAT, _),
   AIL_FMT(Z24_UNORM_S8_UINT,       R32,                   FLOAT, _),

   AIL_FMT(R10G10B10A2_UNORM,       R10G10B10A2,           UNORM, RGB10A2),
   AIL_FMT(R10G10B10X2_UNORM,       R10G10B10A2,           UNORM, RGB10A2),
   AIL_FMT(B10G10R10A2_UNORM,       R10G10B10A2,           UNORM, RGB10A2),
   AIL_FMT(B10G10R10X2_UNORM,       R10G10B10A2,           UNORM, RGB10A2),

   AIL_FMT(R10G10B10A2_UINT,        R10G10B10A2,           UINT,  I16),
   AIL_FMT(B10G10R10A2_UINT,        R10G10B10A2,           UINT,  I16),

   /* I don't see why this wouldn't be renderable, but it doesn't seem to work
    * properly and it's not in Metal.
    */
   AIL_FMT(R10G10B10A2_SINT,        R10G10B10A2,           SINT,  _),
   AIL_FMT(B10G10R10A2_SINT,        R10G10B10A2,           SINT,  _),

   AIL_FMT(R11G11B10_FLOAT,         R11G11B10,             FLOAT, RG11B10F),
   AIL_FMT(R9G9B9E5_FLOAT,          R9G9B9E5,              FLOAT, RGB9E5),

   /* These formats are emulated for texture buffers only */
   AIL_FMT(R32G32B32_FLOAT,         R32G32B32_EMULATED,    FLOAT, _),
   AIL_FMT(R32G32B32_UINT,          R32G32B32_EMULATED,    UINT,  _),
   AIL_FMT(R32G32B32_SINT,          R32G32B32_EMULATED,    SINT,  _),

   /* Likewise, luminance/alpha/intensity formats are supported for texturing,
    * because they are required for texture buffers in the compat profile and
    * mesa/st is unable to emulate them for texture buffers. Our Gallium driver
    * handles the swizzles appropriately, so we just need to plumb through the
    * enums.
    *
    * If mesa/st grows emulation for these formats later, we can drop this.
    */
   AIL_FMT(A8_UNORM,                R8,                    UNORM, _),
   AIL_FMT(A16_UNORM,               R16,                   UNORM, _),
   AIL_FMT(A8_SINT,                 R8,                    SINT,  _),
   AIL_FMT(A16_SINT,                R16,                   SINT,  _),
   AIL_FMT(A32_SINT,                R32,                   SINT,  _),
   AIL_FMT(A8_UINT,                 R8,                    UINT,  _),
   AIL_FMT(A16_UINT,                R16,                   UINT,  _),
   AIL_FMT(A32_UINT,                R32,                   UINT,  _),
   AIL_FMT(A16_FLOAT,               R16,                   FLOAT, _),
   AIL_FMT(A32_FLOAT,               R32,                   FLOAT, _),

   AIL_FMT(L8_UNORM,                R8,                    UNORM, _),
   AIL_FMT(L16_UNORM,               R16,                   UNORM, _),
   AIL_FMT(L8_SINT,                 R8,                    SINT,  _),
   AIL_FMT(L16_SINT,                R16,                   SINT,  _),
   AIL_FMT(L32_SINT,                R32,                   SINT,  _),
   AIL_FMT(L8_UINT,                 R8,                    UINT,  _),
   AIL_FMT(L16_UINT,                R16,                   UINT,  _),
   AIL_FMT(L32_UINT,                R32,                   UINT,  _),
   AIL_FMT(L16_FLOAT,               R16,                   FLOAT, _),
   AIL_FMT(L32_FLOAT,               R32,                   FLOAT, _),

   AIL_FMT(L8A8_UNORM,              R8G8,                  UNORM, _),
   AIL_FMT(L16A16_UNORM,            R16G16,                UNORM, _),
   AIL_FMT(L8A8_SINT,               R8G8,                  SINT,  _),
   AIL_FMT(L16A16_SINT,             R16G16,                SINT,  _),
   AIL_FMT(L32A32_SINT,             R32G32,                SINT,  _),
   AIL_FMT(L8A8_UINT,               R8G8,                  UINT,  _),
   AIL_FMT(L16A16_UINT,             R16G16,                UINT,  _),
   AIL_FMT(L32A32_UINT,             R32G32,                UINT,  _),
   AIL_FMT(L16A16_FLOAT,            R16G16,                FLOAT, _),
   AIL_FMT(L32A32_FLOAT,            R32G32,                FLOAT, _),

   AIL_FMT(I8_UNORM,                R8,                    UNORM, _),
   AIL_FMT(I16_UNORM,               R16,                   UNORM, _),
   AIL_FMT(I8_SINT,                 R8,                    SINT,  _),
   AIL_FMT(I16_SINT,                R16,                   SINT,  _),
   AIL_FMT(I32_SINT,                R32,                   SINT,  _),
   AIL_FMT(I8_UINT,                 R8,                    UINT,  _),
   AIL_FMT(I16_UINT,                R16,                   UINT,  _),
   AIL_FMT(I32_UINT,                R32,                   UINT,  _),
   AIL_FMT(I16_FLOAT,               R16,                   FLOAT, _),
   AIL_FMT(I32_FLOAT,               R32,                   FLOAT, _),

   AIL_FMT(ETC1_RGB8,               ETC2_RGB8,             UNORM, _),
   AIL_FMT(ETC2_RGB8,               ETC2_RGB8,             UNORM, _),
   AIL_FMT(ETC2_SRGB8,              ETC2_RGB8,             UNORM, _),
   AIL_FMT(ETC2_RGB8A1,             ETC2_RGB8A1,           UNORM, _),
   AIL_FMT(ETC2_SRGB8A1,            ETC2_RGB8A1,           UNORM, _),
   AIL_FMT(ETC2_RGBA8,              ETC2_RGBA8,            UNORM, _),
   AIL_FMT(ETC2_SRGBA8,             ETC2_RGBA8,            UNORM, _),
   AIL_FMT(ETC2_R11_UNORM,          EAC_R11,               UNORM, _),
   AIL_FMT(ETC2_R11_SNORM,          EAC_R11,               SNORM, _),
   AIL_FMT(ETC2_RG11_UNORM,         EAC_RG11,              UNORM, _),
   AIL_FMT(ETC2_RG11_SNORM,         EAC_RG11,              SNORM, _),

   AIL_FMT(ASTC_4x4,                ASTC_4X4,              UNORM, _),
   AIL_FMT(ASTC_5x4,                ASTC_5X4,              UNORM, _),
   AIL_FMT(ASTC_5x5,                ASTC_5X5,              UNORM, _),
   AIL_FMT(ASTC_6x5,                ASTC_6X5,              UNORM, _),
   AIL_FMT(ASTC_6x6,                ASTC_6X6,              UNORM, _),
   AIL_FMT(ASTC_8x5,                ASTC_8X5,              UNORM, _),
   AIL_FMT(ASTC_8x6,                ASTC_8X6,              UNORM, _),
   AIL_FMT(ASTC_8x8,                ASTC_8X8,              UNORM, _),
   AIL_FMT(ASTC_10x5,               ASTC_10X5,             UNORM, _),
   AIL_FMT(ASTC_10x6,               ASTC_10X6,             UNORM, _),
   AIL_FMT(ASTC_10x8,               ASTC_10X8,             UNORM, _),
   AIL_FMT(ASTC_10x10,              ASTC_10X10,            UNORM, _),
   AIL_FMT(ASTC_12x10,              ASTC_12X10,            UNORM, _),
   AIL_FMT(ASTC_12x12,              ASTC_12X12,            UNORM, _),

   AIL_FMT(ASTC_4x4_SRGB,           ASTC_4X4,              UNORM, _),
   AIL_FMT(ASTC_5x4_SRGB,           ASTC_5X4,              UNORM, _),
   AIL_FMT(ASTC_5x5_SRGB,           ASTC_5X5,              UNORM, _),
   AIL_FMT(ASTC_6x5_SRGB,           ASTC_6X5,              UNORM, _),
   AIL_FMT(ASTC_6x6_SRGB,           ASTC_6X6,              UNORM, _),
   AIL_FMT(ASTC_8x5_SRGB,           ASTC_8X5,              UNORM, _),
   AIL_FMT(ASTC_8x6_SRGB,           ASTC_8X6,              UNORM, _),
   AIL_FMT(ASTC_8x8_SRGB,           ASTC_8X8,              UNORM, _),
   AIL_FMT(ASTC_10x5_SRGB,          ASTC_10X5,             UNORM, _),
   AIL_FMT(ASTC_10x6_SRGB,          ASTC_10X6,             UNORM, _),
   AIL_FMT(ASTC_10x8_SRGB,          ASTC_10X8,             UNORM, _),
   AIL_FMT(ASTC_10x10_SRGB,         ASTC_10X10,            UNORM, _),
   AIL_FMT(ASTC_12x10_SRGB,         ASTC_12X10,            UNORM, _),
   AIL_FMT(ASTC_12x12_SRGB,         ASTC_12X12,            UNORM, _),

   AIL_FMT(DXT1_RGB,                BC1,                   UNORM, _),
   AIL_FMT(DXT1_RGBA,               BC1,                   UNORM, _),
   AIL_FMT(DXT1_SRGB,               BC1,                   UNORM, _),
   AIL_FMT(DXT1_SRGBA,              BC1,                   UNORM, _),
   AIL_FMT(DXT3_RGBA,               BC2,                   UNORM, _),
   AIL_FMT(DXT3_SRGBA,              BC2,                   UNORM, _),
   AIL_FMT(DXT5_RGBA,               BC3,                   UNORM, _),
   AIL_FMT(DXT5_SRGBA,              BC3,                   UNORM, _),
   AIL_FMT(RGTC1_UNORM,             BC4,                   UNORM, _),
   AIL_FMT(RGTC1_SNORM,             BC4,                   SNORM, _),
   AIL_FMT(RGTC2_UNORM,             BC5,                   UNORM, _),
   AIL_FMT(RGTC2_SNORM,             BC5,                   SNORM, _),
   AIL_FMT(BPTC_RGB_FLOAT,          BC6H,                  FLOAT, _),
   AIL_FMT(BPTC_RGB_UFLOAT,         BC6H_UFLOAT,           FLOAT, _),
   AIL_FMT(BPTC_RGBA_UNORM,         BC7,                   UNORM, _),
   AIL_FMT(BPTC_SRGBA,              BC7,                   UNORM, _),
};
/* clang-format on */
