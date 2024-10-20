/*
 * Copyright 2022 Alyssa Rosenzweig
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "util/format/u_formats.h"
#include "agx_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum render targets per framebuffer. This is NOT architectural, but it
 * is the ~universal API limit so there's no point in allowing more.
 */
#define AGX_MAX_RENDER_TARGETS (8)

/* Forward declarations to keep the header lean */
struct nir_shader;
struct nir_def;
struct nir_builder;

struct agx_tile_size {
   uint8_t width;
   uint8_t height;
};

struct agx_tilebuffer_layout {
   /* Logical format of each render target. Use agx_tilebuffer_physical_format
    * to get the physical format.
    */
   enum pipe_format logical_format[AGX_MAX_RENDER_TARGETS];

   /* Which render targets are spilled. */
   bool spilled[AGX_MAX_RENDER_TARGETS];

   /* Offset into the sample of each render target. If a render target is
    * spilled, its offset is UNDEFINED. Use agx_tilebuffer_offset_B to access.
    */
   uint8_t _offset_B[AGX_MAX_RENDER_TARGETS];

   /* Total bytes per sample, rounded up as needed. Spilled render targets do
    * not count against this.
    */
   uint8_t sample_size_B;

   /* Number of samples per pixel */
   uint8_t nr_samples;

   /* If layered rendering is used */
   bool layered;

   /* Selected tile size */
   struct agx_tile_size tile_size;

   /* USC word corresponding to this configuration of the tilebuffer */
   struct agx_usc_shared_packed usc;
};

/*
 * _offset_B is undefined for non-spilled render targets. This safe accessor
 * asserts that render targets are not spilled rather than returning garbage.
 */
static inline uint8_t
agx_tilebuffer_offset_B(struct agx_tilebuffer_layout *layout, unsigned rt)
{
   assert(rt < AGX_MAX_RENDER_TARGETS);
   assert(!layout->spilled[rt] && "precondition");

   return layout->_offset_B[rt];
}

static inline bool
agx_tilebuffer_spills(struct agx_tilebuffer_layout *layout)
{
   for (unsigned rt = 0; rt < AGX_MAX_RENDER_TARGETS; ++rt) {
      if (layout->spilled[rt])
         return true;
   }

   return false;
}

struct agx_tilebuffer_layout
agx_build_tilebuffer_layout(const enum pipe_format *formats, uint8_t nr_cbufs,
                            uint8_t nr_samples, bool layered);

void agx_tilebuffer_set_samples(struct agx_tilebuffer_layout *tib,
                                unsigned nr_samples);

bool agx_nir_lower_tilebuffer(struct nir_shader *shader,
                              struct agx_tilebuffer_layout *tib,
                              uint8_t *colormasks, unsigned *bindless_base,
                              struct nir_def *write_samples, bool *translucent);

bool agx_nir_lower_to_per_sample(struct nir_shader *shader);

bool agx_nir_lower_monolithic_msaa(struct nir_shader *shader,
                                   uint8_t nr_samples);

bool agx_nir_lower_sample_intrinsics(struct nir_shader *shader,
                                     bool ignore_sample_mask_without_msaa);

bool agx_nir_lower_alpha_to_coverage(struct nir_shader *shader,
                                     uint8_t nr_samples);

bool agx_nir_lower_alpha_to_one(struct nir_shader *shader);

uint32_t agx_tilebuffer_total_size(struct agx_tilebuffer_layout *tib);

enum pipe_format
agx_tilebuffer_physical_format(struct agx_tilebuffer_layout *tib, unsigned rt);

bool agx_tilebuffer_supports_mask(struct agx_tilebuffer_layout *tib,
                                  unsigned rt);

void agx_tilebuffer_pack_usc(struct agx_tilebuffer_layout *tib);

#ifdef __cplusplus
} /* extern C */
#endif
