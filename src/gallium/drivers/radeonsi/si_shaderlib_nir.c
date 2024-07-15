/*
 * Copyright 2018 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "gallium/auxiliary/nir/pipe_nir.h"
#define AC_SURFACE_INCLUDE_NIR
#include "ac_surface.h"
#include "si_pipe.h"
#include "si_query.h"
#include "aco_interface.h"
#include "nir_format_convert.h"
#include "ac_nir_helpers.h"

void *si_create_shader_state(struct si_context *sctx, nir_shader *nir)
{
   sctx->b.screen->finalize_nir(sctx->b.screen, (void*)nir);
   return pipe_shader_from_nir(&sctx->b, nir);
}

/* unpack_2x16(src, x, y): x = src & 0xffff; y = src >> 16; */
static void unpack_2x16(nir_builder *b, nir_def *src, nir_def **x, nir_def **y)
{
   *x = nir_iand_imm(b, src, 0xffff);
   *y = nir_ushr_imm(b, src, 16);
}

void *si_create_dcc_retile_cs(struct si_context *sctx, struct radeon_surf *surf)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "dcc_retile");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.cs.user_data_components_amd = 3;
   b.shader->info.num_ssbos = 1;

   /* Get user data SGPRs. */
   nir_def *user_sgprs = nir_load_user_data_amd(&b);

   /* Relative offset from the displayable DCC to the non-displayable DCC in the same buffer. */
   nir_def *src_dcc_offset = nir_channel(&b, user_sgprs, 0);

   nir_def *src_dcc_pitch, *dst_dcc_pitch, *src_dcc_height, *dst_dcc_height;
   unpack_2x16(&b, nir_channel(&b, user_sgprs, 1), &src_dcc_pitch, &src_dcc_height);
   unpack_2x16(&b, nir_channel(&b, user_sgprs, 2), &dst_dcc_pitch, &dst_dcc_height);

   /* Get the 2D coordinates. */
   nir_def *coord = ac_get_global_ids(&b, 2, 32);
   nir_def *zero = nir_imm_int(&b, 0);

   /* Multiply the coordinates by the DCC block size (they are DCC block coordinates). */
   coord = nir_imul(&b, coord, nir_imm_ivec2(&b, surf->u.gfx9.color.dcc_block_width,
                                             surf->u.gfx9.color.dcc_block_height));

   nir_def *src_offset =
      ac_nir_dcc_addr_from_coord(&b, &sctx->screen->info, surf->bpe, &surf->u.gfx9.color.dcc_equation,
                                 src_dcc_pitch, src_dcc_height, zero, /* DCC slice size */
                                 nir_channel(&b, coord, 0), nir_channel(&b, coord, 1), /* x, y */
                                 zero, zero, zero); /* z, sample, pipe_xor */
   src_offset = nir_iadd(&b, src_offset, src_dcc_offset);
   nir_def *value = nir_load_ssbo(&b, 1, 8, zero, src_offset, .align_mul=1);

   nir_def *dst_offset =
      ac_nir_dcc_addr_from_coord(&b, &sctx->screen->info, surf->bpe, &surf->u.gfx9.color.display_dcc_equation,
                                 dst_dcc_pitch, dst_dcc_height, zero, /* DCC slice size */
                                 nir_channel(&b, coord, 0), nir_channel(&b, coord, 1), /* x, y */
                                 zero, zero, zero); /* z, sample, pipe_xor */
   nir_store_ssbo(&b, value, zero, dst_offset, .write_mask=0x1, .align_mul=1);

   return si_create_shader_state(sctx, b.shader);
}

void *gfx9_create_clear_dcc_msaa_cs(struct si_context *sctx, struct si_texture *tex)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "clear_dcc_msaa");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.cs.user_data_components_amd = 2;
   b.shader->info.num_ssbos = 1;

   /* Get user data SGPRs. */
   nir_def *user_sgprs = nir_load_user_data_amd(&b);
   nir_def *dcc_pitch, *dcc_height, *clear_value, *pipe_xor;
   unpack_2x16(&b, nir_channel(&b, user_sgprs, 0), &dcc_pitch, &dcc_height);
   unpack_2x16(&b, nir_channel(&b, user_sgprs, 1), &clear_value, &pipe_xor);
   clear_value = nir_u2u16(&b, clear_value);

   /* Get the 2D coordinates. */
   nir_def *coord = ac_get_global_ids(&b, 3, 32);
   nir_def *zero = nir_imm_int(&b, 0);

   /* Multiply the coordinates by the DCC block size (they are DCC block coordinates). */
   coord = nir_imul(&b, coord,
                    nir_imm_ivec3(&b, tex->surface.u.gfx9.color.dcc_block_width,
                                      tex->surface.u.gfx9.color.dcc_block_height,
                                      tex->surface.u.gfx9.color.dcc_block_depth));

   nir_def *offset =
      ac_nir_dcc_addr_from_coord(&b, &sctx->screen->info, tex->surface.bpe,
                                 &tex->surface.u.gfx9.color.dcc_equation,
                                 dcc_pitch, dcc_height, zero, /* DCC slice size */
                                 nir_channel(&b, coord, 0), nir_channel(&b, coord, 1), /* x, y */
                                 tex->buffer.b.b.array_size > 1 ? nir_channel(&b, coord, 2) : zero, /* z */
                                 zero, pipe_xor); /* sample, pipe_xor */

   /* The trick here is that DCC elements for an even and the next odd sample are next to each other
    * in memory, so we only need to compute the address for sample 0 and the next DCC byte is always
    * sample 1. That's why the clear value has 2 bytes - we're clearing 2 samples at the same time.
    */
   nir_store_ssbo(&b, clear_value, zero, offset, .write_mask=0x1, .align_mul=2);

   return si_create_shader_state(sctx, b.shader);
}

/* Create a compute shader implementing clear_buffer or copy_buffer. */
void *si_create_clear_buffer_rmw_cs(struct si_context *sctx)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "clear_buffer_rmw_cs");
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.cs.user_data_components_amd = 2;
   b.shader->info.num_ssbos = 1;

   /* address = blockID * 64 + threadID; */
   nir_def *address = ac_get_global_ids(&b, 1, 32);

   /* address = address * 16; (byte offset, loading one vec4 per thread) */
   address = nir_ishl_imm(&b, address, 4);
   
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *data = nir_load_ssbo(&b, 4, 32, zero, address, .align_mul = 4);

   /* Get user data SGPRs. */
   nir_def *user_sgprs = nir_load_user_data_amd(&b);

   /* data &= inverted_writemask; */
   data = nir_iand(&b, data, nir_channel(&b, user_sgprs, 1));
   /* data |= clear_value_masked; */
   data = nir_ior(&b, data, nir_channel(&b, user_sgprs, 0));

   nir_store_ssbo(&b, data, zero, address, .align_mul = 4);

   return si_create_shader_state(sctx, b.shader);
}

/* This is used when TCS is NULL in the VS->TCS->TES chain. In this case,
 * VS passes its outputs to TES directly, so the fixed-function shader only
 * has to write TESSOUTER and TESSINNER.
 */
void *si_create_passthrough_tcs(struct si_context *sctx)
{
   unsigned locations[PIPE_MAX_SHADER_OUTPUTS];

   struct si_shader_info *info = &sctx->shader.vs.cso->info;
   for (unsigned i = 0; i < info->num_outputs; i++) {
      locations[i] = info->output_semantic[i];
   }

   nir_shader *tcs = nir_create_passthrough_tcs_impl(sctx->screen->nir_options, locations,
                                                     info->num_outputs, sctx->patch_vertices);

   return si_create_shader_state(sctx, tcs);
}

/* Store the clear color at the beginning of every 256B block. This is required when we clear DCC
 * to GFX11_DCC_CLEAR_SINGLE.
 */
void *si_clear_image_dcc_single_shader(struct si_context *sctx, bool is_msaa, unsigned wg_dim)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "write_clear_color_dcc_single");
   b.shader->info.num_images = 1;
   if (is_msaa)
      BITSET_SET(b.shader->info.msaa_images, 0);
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.cs.user_data_components_amd = 5;

   const struct glsl_type *img_type =
      glsl_image_type(is_msaa ? GLSL_SAMPLER_DIM_MS : GLSL_SAMPLER_DIM_2D, true, GLSL_TYPE_FLOAT);
   nir_variable *output_img = nir_variable_create(b.shader, nir_var_image, img_type, "out_img");
   output_img->data.binding = 0;

   nir_def *global_id = nir_pad_vector_imm_int(&b, ac_get_global_ids(&b, wg_dim, 32), 0, 3);
   nir_def *clear_color = nir_trim_vector(&b, nir_load_user_data_amd(&b), 4);

   nir_def *dcc_block_width, *dcc_block_height;
   unpack_2x16(&b, nir_channel(&b, nir_load_user_data_amd(&b), 4), &dcc_block_width,
               &dcc_block_height);

   /* Compute the coordinates. */
   nir_def *coord = nir_trim_vector(&b, global_id, 2);
   coord = nir_imul(&b, coord, nir_vec2(&b, dcc_block_width, dcc_block_height));
   coord = nir_vec4(&b, nir_channel(&b, coord, 0), nir_channel(&b, coord, 1),
                    nir_channel(&b, global_id, 2), nir_undef(&b, 1, 32));

   /* Store the clear color. */
   nir_image_deref_store(&b, &nir_build_deref_var(&b, output_img)->def, coord, nir_imm_int(&b, 0),
                         clear_color, nir_imm_int(&b, 0));

   return si_create_shader_state(sctx, b.shader);
}

void *si_create_ubyte_to_ushort_compute_shader(struct si_context *sctx)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "ubyte_to_ushort");
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = 2;

   nir_def *load_address = ac_get_global_ids(&b, 1, 32);
   nir_def *store_address = nir_imul_imm(&b, load_address, 2);

   nir_def *ubyte_value = nir_load_ssbo(&b, 1, 8, nir_imm_int(&b, 1),
                                        load_address, .access = ACCESS_RESTRICT);
   nir_store_ssbo(&b, nir_u2u16(&b, ubyte_value), nir_imm_int(&b, 0),
                  store_address, .access = ACCESS_RESTRICT);

   return si_create_shader_state(sctx, b.shader);
}

/* This is regular load_ssbo with special handling for sparse buffers. Normally, sparse buffer
 * loads return 0 for all components if a sparse load starts on a non-resident page, crosses
 * the page boundary, and ends on a resident page. For copy_buffer, we want it to return 0 only
 * for the portion of the load that's non-resident, and load values for the portion that's
 * resident. The workaround is to scalarize such loads and disallow vectorization.
 */
static nir_def *
load_ssbo_sparse(nir_builder *b, unsigned num_components, unsigned bit_size, nir_def *buf,
                 nir_def *offset, struct _nir_load_ssbo_indices params, bool sparse)
{
   if (sparse && num_components > 1) {
      nir_def *vec[NIR_MAX_VEC_COMPONENTS];

      /* Split the vector load into scalar loads. */
      for (unsigned i = 0; i < num_components; i++) {
         unsigned elem_offset = i * bit_size / 8;
         unsigned align_offset = (params.align_offset + elem_offset) % params.align_mul;

         vec[i] = nir_load_ssbo(b, 1, bit_size, buf,
                                nir_iadd_imm(b, offset, elem_offset),
                                .access = params.access | ACCESS_KEEP_SCALAR,
                                .align_mul = params.align_mul,
                                .align_offset = align_offset);
      }
      return nir_vec(b, vec, num_components);
   } else {
      return nir_load_ssbo(b, num_components, bit_size, buf, offset,
                           .access = params.access,
                           .align_mul = params.align_mul,
                           .align_offset = params.align_offset);
   }
}

/* Create a compute shader implementing clear_buffer or copy_buffer. */
void *si_create_dma_compute_shader(struct si_context *sctx, union si_cs_clear_copy_buffer_key *key)
{
   if (si_can_dump_shader(sctx->screen, MESA_SHADER_COMPUTE, SI_DUMP_SHADER_KEY)) {
      fprintf(stderr, "Internal shader: dma\n");
      fprintf(stderr, "   key.is_clear = %u\n", key->is_clear);
      fprintf(stderr, "   key.dwords_per_thread = %u\n", key->dwords_per_thread);
      fprintf(stderr, "   key.clear_value_size_is_12 = %u\n", key->clear_value_size_is_12);
      fprintf(stderr, "   key.src_is_sparse = %u\n", key->src_is_sparse);
      fprintf(stderr, "   key.src_align_offset = %u\n", key->src_align_offset);
      fprintf(stderr, "   key.dst_align_offset = %u\n", key->dst_align_offset);
      fprintf(stderr, "   key.dst_last_thread_bytes = %u\n", key->dst_last_thread_bytes);
      fprintf(stderr, "   key.dst_single_thread_unaligned = %u\n", key->dst_single_thread_unaligned);
      fprintf(stderr, "\n");
   }

   assert(key->dwords_per_thread && key->dwords_per_thread <= 4);

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "create_dma_compute");
   b.shader->info.workgroup_size[0] = 64;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ssbos = key->is_clear ? 1 : 2;
   b.shader->info.cs.user_data_components_amd =
      (key->is_clear ? (key->clear_value_size_is_12 ? 3 : key->dwords_per_thread) : 0);

   /* Add the last thread ID value. */
   if (key->dst_last_thread_bytes)
      b.shader->info.cs.user_data_components_amd = key->is_clear ? 5 : 1;

   nir_def *thread_id = ac_get_global_ids(&b, 1, 32);
   /* Convert the global thread ID into bytes. */
   nir_def *offset = nir_imul_imm(&b, thread_id, 4 * key->dwords_per_thread);
   nir_def *value;

   if (key->is_clear) {
      value = nir_trim_vector(&b, nir_load_user_data_amd(&b), key->dwords_per_thread);

      /* We store 4 dwords per thread, but the clear value has 3 dwords. Swizzle it to 4 dwords.
       * Storing 4 dwords per thread is faster even when the ALU cost is worse.
       */
      if (key->clear_value_size_is_12 && key->dwords_per_thread == 4) {
         nir_def *dw_offset = nir_imul_imm(&b, thread_id, key->dwords_per_thread);
         nir_def *vec[3];

         /* Swizzle a 3-component clear value to get a 4-component clear value. Example:
          * 0 1 2 3 | 4 5 6 7 | 8 9 10 11  // dw_offset
          *              |
          *              V
          * 0 1 2 0 | 1 2 0 1 | 2 0 1 2    // clear value component indices
          */
         for (unsigned i = 0; i < 3; i++) {
            vec[i] = nir_vector_extract(&b, value,
                                        nir_umod_imm(&b, nir_iadd_imm(&b, dw_offset, i), 3));
         }
         value = nir_vec4(&b, vec[0], vec[1], vec[2], vec[0]);
      }
   } else {
      /* The hw doesn't support unaligned 32-bit loads, and only supports single-component
       * unaligned 1-byte and 2-byte loads. Luckily, we don't have to use single-component loads
       * because ac_nir_lower_subdword_load converts 1-byte and 2-byte vector loads with unaligned
       * offsets into aligned 32-bit loads by loading an extra dword and then bit-shifting all bits
       * to get the expected result. We only have to set bit_size to 8 or 16 and align_offset to
       * 1..3 to indicate that this is an unaligned load. align_offset is the amount of
       * unalignment.
       *
       * Since the buffer binding offsets are rounded down to the clear/copy size of the thread
       * (i.e. dst_align_offset is subtracted from dst_offset, and src_align_offset is subtracted
       * from src_offset), the stores expect the loaded value to be byte-shifted accordingly.
       * realign_offset is the amount of byte-shifting we have to do.
       */
      assert(util_is_power_of_two_nonzero(key->dwords_per_thread));
      int realign_offset = key->src_align_offset - key->dst_align_offset;
      unsigned alignment = (unsigned)realign_offset % 4 == 0 ? 4 :
                           (unsigned)realign_offset % 2 == 0 ? 2 : 1;
      unsigned bit_size = alignment * 8;
      unsigned num_comps = key->dwords_per_thread * 4 / alignment;
      nir_if *if_first_thread = NULL;
      nir_def *value0 = NULL;

      if (realign_offset < 0) {
         /* if src_align_offset is less than dst_align_offset, realign_offset is
          * negative, which causes the first thread to use a negative buffer offset, which goes
          * entirely out of bounds because the offset is treated as unsigned. Instead of that,
          * the first thread should load from offset 0 by not loading the bytes before
          * the beginning of the buffer.
          */
         if_first_thread = nir_push_if(&b, nir_ieq_imm(&b, thread_id, 0));
         {
            unsigned num_removed_comps = -realign_offset / alignment;
            unsigned num_inbounds_comps = num_comps - num_removed_comps;

            /* Only 8 and 16 component vectors are valid after 5 in NIR. */
            while (!nir_num_components_valid(num_inbounds_comps))
               num_inbounds_comps = util_next_power_of_two(num_inbounds_comps);

            value0 = load_ssbo_sparse(&b, num_inbounds_comps, bit_size, nir_imm_int(&b, 0), offset,
                                      (struct _nir_load_ssbo_indices){
                                         .access = ACCESS_RESTRICT,
                                         .align_mul = 4,
                                         .align_offset = 0
                                      }, key->src_is_sparse);

            /* Add the components that we didn't load as undef. */
            nir_def *comps[16];
            assert(num_comps <= ARRAY_SIZE(comps));
            for (unsigned i = 0; i < num_comps; i++) {
               if (i < num_removed_comps)
                  comps[i] = nir_undef(&b, 1, bit_size);
               else
                  comps[i] = nir_channel(&b, value0, i - num_removed_comps);
            }
            value0 = nir_vec(&b, comps, num_comps);
         }
         nir_push_else(&b, if_first_thread);
      }

      value = load_ssbo_sparse(&b, num_comps, bit_size, nir_imm_int(&b, 0),
                               nir_iadd_imm(&b, offset, realign_offset),
                               (struct _nir_load_ssbo_indices){
                                  .access = ACCESS_RESTRICT,
                                  .align_mul = 4,
                                  .align_offset = (unsigned)realign_offset % 4
                               }, key->src_is_sparse);


      if (if_first_thread) {
         nir_pop_if(&b, if_first_thread);
         value = nir_if_phi(&b, value0, value);
      }

      /* Bitcast the vector to 32 bits. */
      if (value->bit_size != 32)
         value = nir_extract_bits(&b, &value, 1, 0, key->dwords_per_thread, 32);
   }

   nir_def *dst_buf = nir_imm_int(&b, !key->is_clear);
   nir_if *if_first_thread = NULL, *if_last_thread = NULL;

   if (!key->dst_single_thread_unaligned) {
      /* dst_align_offset means how many bytes the first thread should skip because the offset of
       * the buffer binding is rounded down to the clear/copy size of thread, causing the bytes
       * before dst_align_offset to be writable. Above we used realign_offset to byte-shift
       * the value to compensate for the rounded-down offset, so that all stores are dword stores
       * regardless of the offset/size alignment except that the first thread shouldn't store
       * the first dst_align_offset bytes, and the last thread should only store the first
       * dst_last_thread_bytes. In both cases, there is a dword that must be only partially
       * written by splitting it into 8-bit and 16-bit stores.
       */
      if (key->dst_align_offset) {
          if_first_thread = nir_push_if(&b, nir_ieq_imm(&b, thread_id, 0));
          {
             unsigned local_offset = key->dst_align_offset;
             nir_def *first_dword = nir_channel(&b, value, local_offset / 4);

             if (local_offset % 2 == 1) {
                nir_store_ssbo(&b, nir_channel(&b, nir_unpack_32_4x8(&b, first_dword), local_offset % 4),
                               dst_buf, nir_iadd_imm_nuw(&b, offset, local_offset),
                               .access = ACCESS_RESTRICT);
                local_offset++;
             }

             if (local_offset % 4 == 2) {
                nir_store_ssbo(&b, nir_unpack_32_2x16_split_y(&b, first_dword), dst_buf,
                               nir_iadd_imm_nuw(&b, offset, local_offset),
                               .access = ACCESS_RESTRICT);
                local_offset += 2;
             }

             assert(local_offset % 4 == 0);
             unsigned num_dw_remaining = key->dwords_per_thread - local_offset / 4;

             if (num_dw_remaining) {
                nir_def *dwords =
                   nir_channels(&b, value, BITFIELD_RANGE(local_offset / 4, num_dw_remaining));

                nir_store_ssbo(&b, dwords, dst_buf, nir_iadd_imm_nuw(&b, offset, local_offset),
                               .access = ACCESS_RESTRICT);
             }
          }
          nir_push_else(&b, if_first_thread);
      }

      if (key->dst_last_thread_bytes) {
         nir_def *last_thread_id =
            nir_channel(&b, nir_load_user_data_amd(&b), key->is_clear ? 4 : 0);

         if_last_thread = nir_push_if(&b, nir_ieq(&b, thread_id, last_thread_id));
         {
            unsigned num_dwords = key->dst_last_thread_bytes / 4;
            bool write_short = (key->dst_last_thread_bytes - num_dwords * 4) / 2;
            bool write_byte = key->dst_last_thread_bytes % 2;
            nir_def *last_dword = nir_channel(&b, value, num_dwords);

            if (num_dwords) {
               nir_def *dwords = nir_channels(&b, value, BITFIELD_MASK(num_dwords));
               nir_store_ssbo(&b, dwords, dst_buf, offset, .access = ACCESS_RESTRICT);
            }

            if (write_short) {
               nir_store_ssbo(&b, nir_u2u16(&b, last_dword), dst_buf,
                              nir_iadd_imm_nuw(&b, offset, num_dwords * 4),
                              .access = ACCESS_RESTRICT);
            }

            if (write_byte) {
               nir_store_ssbo(&b, nir_channel(&b, nir_unpack_32_4x8(&b, last_dword), write_short * 2),
                              dst_buf, nir_iadd_imm_nuw(&b, offset, num_dwords * 4 + write_short * 2),
                              .access = ACCESS_RESTRICT);
            }
         }
         nir_push_else(&b, if_last_thread);
      }

      nir_store_ssbo(&b, value, dst_buf, offset, .access = ACCESS_RESTRICT);

      if (if_last_thread)
         nir_pop_if(&b, if_last_thread);
      if (if_first_thread)
         nir_pop_if(&b, if_first_thread);
   } else {
      /* This shader only executes a single thread (tiny copy or clear) and it's unaligned at both
       * the beginning and the end. Walk the individual dwords/words/bytes that should be written
       * to split the store accordingly.
       */
      for (unsigned local_offset = key->dst_align_offset;
           local_offset < key->dst_last_thread_bytes;) {
         unsigned remaining = key->dst_last_thread_bytes - local_offset;
         nir_def *src_dword = nir_channel(&b, value, local_offset / 4);

         if (local_offset % 2 == 1 || remaining == 1) {
            /* 1-byte store. */
            nir_def *src_dword4x8 = nir_unpack_32_4x8(&b, src_dword);
            nir_store_ssbo(&b, nir_channel(&b, src_dword4x8, local_offset % 4), dst_buf,
                           nir_iadd_imm_nuw(&b, offset, local_offset), .access = ACCESS_RESTRICT);
            local_offset++;
         } else if (local_offset % 4 == 2 || remaining == 2 || remaining == 3) {
            /* 2-byte store. */
            nir_def *src_dword2x16 = nir_unpack_32_2x16(&b, src_dword);
            nir_store_ssbo(&b, nir_channel(&b, src_dword2x16, (local_offset / 2) % 2), dst_buf,
                           nir_iadd_imm_nuw(&b, offset, local_offset), .access = ACCESS_RESTRICT);
            local_offset += 2;
         } else {
            /* 1-N dwords. */
            unsigned dw_size = remaining / 4;
            assert(dw_size);
            assert(local_offset % 4 == 0);

            nir_store_ssbo(&b, nir_channels(&b, value, BITFIELD_RANGE(local_offset / 4, dw_size)),
                           dst_buf, nir_iadd_imm_nuw(&b, offset, local_offset),
                           .access = ACCESS_RESTRICT);
            local_offset += dw_size * 4;
         }
      }
   }

   return si_create_shader_state(sctx, b.shader);
}

/* Load samples from the image, and copy them to the same image. This looks like
 * a no-op, but it's not. Loads use FMASK, while stores don't, so samples are
 * reordered to match expanded FMASK.
 *
 * After the shader finishes, FMASK should be cleared to identity.
 */
void *si_create_fmask_expand_cs(struct si_context *sctx, unsigned num_samples, bool is_array)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "create_fmask_expand_cs");
   b.shader->info.workgroup_size[0] = 8;
   b.shader->info.workgroup_size[1] = 8;
   b.shader->info.workgroup_size[2] = 1;

   /* Return an empty compute shader */
   if (num_samples == 0)
      return si_create_shader_state(sctx, b.shader);

   b.shader->info.num_images = 1;

   const struct glsl_type *img_type = glsl_image_type(GLSL_SAMPLER_DIM_MS, is_array, GLSL_TYPE_FLOAT);
   nir_variable *img = nir_variable_create(b.shader, nir_var_image, img_type, "image");
   img->data.access = ACCESS_RESTRICT;

   nir_def *z = nir_undef(&b, 1, 32);
   if (is_array) {
      z = nir_channel(&b, nir_load_workgroup_id(&b), 2);
   }

   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *address = ac_get_global_ids(&b, 2, 32);

   nir_def *sample[8], *addresses[8];
   assert(num_samples <= ARRAY_SIZE(sample));

   nir_def *img_def = &nir_build_deref_var(&b, img)->def;

   /* Load samples, resolving FMASK. */
   for (unsigned i = 0; i < num_samples; i++) {
      nir_def *it = nir_imm_int(&b, i);
      sample[i] = nir_vec4(&b, nir_channel(&b, address, 0), nir_channel(&b, address, 1), z, it);
      addresses[i] = nir_image_deref_load(&b, 4, 32, img_def, sample[i], it, zero,
                                          .access = ACCESS_RESTRICT,
                                          .image_dim = GLSL_SAMPLER_DIM_2D,
                                          .image_array = is_array);
   }

   /* Store samples, ignoring FMASK. */
   for (unsigned i = 0; i < num_samples; i++) {
      nir_image_deref_store(&b, img_def, sample[i], nir_imm_int(&b, i), addresses[i], zero,
                            .access = ACCESS_RESTRICT,
                            .image_dim = GLSL_SAMPLER_DIM_2D,
                            .image_array = is_array);
   }

   return si_create_shader_state(sctx, b.shader);
}

/* This is just a pass-through shader with 1-3 MOV instructions. */
void *si_get_blitter_vs(struct si_context *sctx, enum blitter_attrib_type type, unsigned num_layers)
{
   unsigned vs_blit_property;
   void **vs;

   switch (type) {
   case UTIL_BLITTER_ATTRIB_NONE:
      vs = num_layers > 1 ? &sctx->vs_blit_pos_layered : &sctx->vs_blit_pos;
      vs_blit_property = SI_VS_BLIT_SGPRS_POS;
      break;
   case UTIL_BLITTER_ATTRIB_COLOR:
      vs = num_layers > 1 ? &sctx->vs_blit_color_layered : &sctx->vs_blit_color;
      vs_blit_property = SI_VS_BLIT_SGPRS_POS_COLOR;
      break;
   case UTIL_BLITTER_ATTRIB_TEXCOORD_XY:
   case UTIL_BLITTER_ATTRIB_TEXCOORD_XYZW:
      assert(num_layers == 1);
      vs = &sctx->vs_blit_texcoord;
      vs_blit_property = SI_VS_BLIT_SGPRS_POS_TEXCOORD;
      break;
   default:
      assert(0);
      return NULL;
   }

   if (*vs)
      return *vs;

   /* Add 1 for the attribute ring address. */
   if (sctx->gfx_level >= GFX11 && type != UTIL_BLITTER_ATTRIB_NONE)
      vs_blit_property++;

   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, sctx->screen->nir_options,
                                                  "get_blitter_vs");

   /* Tell the shader to load VS inputs from SGPRs: */
   b.shader->info.vs.blit_sgprs_amd = vs_blit_property;
   b.shader->info.vs.window_space_position = true;

   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_copy_var(&b,
                nir_create_variable_with_location(b.shader, nir_var_shader_out,
                                                  VARYING_SLOT_POS, vec4),
                nir_create_variable_with_location(b.shader, nir_var_shader_in,
                                                  VERT_ATTRIB_GENERIC0, vec4));

   if (type != UTIL_BLITTER_ATTRIB_NONE) {
      nir_copy_var(&b,
                   nir_create_variable_with_location(b.shader, nir_var_shader_out,
                                                     VARYING_SLOT_VAR0, vec4),
                   nir_create_variable_with_location(b.shader, nir_var_shader_in,
                                                     VERT_ATTRIB_GENERIC1, vec4));
   }

   if (num_layers > 1) {
      nir_variable *out_layer =
         nir_create_variable_with_location(b.shader, nir_var_shader_out,
                                           VARYING_SLOT_LAYER, glsl_int_type());
      out_layer->data.interpolation = INTERP_MODE_NONE;

      nir_copy_var(&b, out_layer,
                   nir_create_variable_with_location(b.shader, nir_var_system_value,
                                                     SYSTEM_VALUE_INSTANCE_ID, glsl_int_type()));
   }

   *vs = si_create_shader_state(sctx, b.shader);
   return *vs;
}

/* Create the compute shader that is used to collect the results.
 *
 * One compute grid with a single thread is launched for every query result
 * buffer. The thread (optionally) reads a previous summary buffer, then
 * accumulates data from the query result buffer, and writes the result either
 * to a summary buffer to be consumed by the next grid invocation or to the
 * user-supplied buffer.
 *
 * Data layout:
 *
 * CONST
 *  0.x = end_offset
 *  0.y = result_stride
 *  0.z = result_count
 *  0.w = bit field:
 *          1: read previously accumulated values
 *          2: write accumulated values for chaining
 *          4: write result available
 *          8: convert result to boolean (0/1)
 *         16: only read one dword and use that as result
 *         32: apply timestamp conversion
 *         64: store full 64 bits result
 *        128: store signed 32 bits result
 *        256: SO_OVERFLOW mode: take the difference of two successive half-pairs
 *  1.x = fence_offset
 *  1.y = pair_stride
 *  1.z = pair_count
 *
 */
void *si_create_query_result_cs(struct si_context *sctx)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "create_query_result_cs");
   b.shader->info.workgroup_size[0] = 1;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ubos = 1;
   b.shader->info.num_ssbos = 3;
   b.shader->num_uniforms = 2;

   nir_def *var_undef = nir_undef(&b, 1, 32);
   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *one = nir_imm_int(&b, 1);
   nir_def *two = nir_imm_int(&b, 2);
   nir_def *four = nir_imm_int(&b, 4);
   nir_def *eight = nir_imm_int(&b, 8);
   nir_def *sixteen = nir_imm_int(&b, 16);
   nir_def *thirty_one = nir_imm_int(&b, 31);
   nir_def *sixty_four = nir_imm_int(&b, 64);

   /* uint32_t x, y, z = 0; */
   nir_function_impl *e = nir_shader_get_entrypoint(b.shader);
   nir_variable *x = nir_local_variable_create(e, glsl_uint_type(), "x");
   nir_store_var(&b, x, var_undef, 0x1);
   nir_variable *y = nir_local_variable_create(e, glsl_uint_type(), "y");
   nir_store_var(&b, y, var_undef, 0x1);
   nir_variable *z = nir_local_variable_create(e, glsl_uint_type(), "z");
   nir_store_var(&b, z, zero, 0x1);

   /* uint32_t buff_0[4] = load_ubo(0, 0); */
   nir_def *buff_0 = nir_load_ubo(&b, 4, 32, zero, zero, .range_base = 0, .range = 16);
   /* uint32_t buff_1[4] = load_ubo(1, 16); */
   nir_def *buff_1 = nir_load_ubo(&b, 4, 32, zero, sixteen, .range_base = 16, .range = 16);

   /* uint32_t b0_bitfield = buff_0.w; */
   nir_def *b0_bitfield = nir_channel(&b, buff_0, 3);

   /* Check result availability.
    *    if (b0_bitfield & (1u << 4)) {
    *       ...
    */
   nir_def *is_one_dword_result = nir_i2b(&b, nir_iand(&b, b0_bitfield, sixteen));
   nir_if *if_one_dword_result = nir_push_if(&b, is_one_dword_result); {

      /*   int32_t value = load_ssbo(0, fence_offset);
       *   z = ~(value >> 31);
       */
      nir_def *value = nir_load_ssbo(&b, 1, 32, zero, nir_channel(&b, buff_1, 0));
      nir_def *bitmask = nir_inot(&b, nir_ishr(&b, value, thirty_one));
      nir_store_var(&b, z, bitmask, 0x1);

      /* Load result if available.
       *    if (value < 0) {
       *       uint32_t result[2] = load_ssbo(0, 0);
       *       x = result[0];
       *       y = result[1];
       *    }
       */
      nir_if *if_negative = nir_push_if(&b, nir_ilt(&b, value, zero)); {
         nir_def *result = nir_load_ssbo(&b, 2, 32, zero, zero);
         nir_store_var(&b, x, nir_channel(&b, result, 0), 0x1);
         nir_store_var(&b, y, nir_channel(&b, result, 1), 0x1);
      }
      nir_pop_if(&b, if_negative);
   } nir_push_else(&b, if_one_dword_result); {

      /* } else {
       *    x = 0; y = 0;
       */
      nir_store_var(&b, x, zero, 0x1);
      nir_store_var(&b, y, zero, 0x1);

      /* Load previously accumulated result if requested.
       *    if (b0_bitfield & (1u << 0)) {
       *       uint32_t result[3] = load_ssbo(1, 0);
       *       x = result[0];
       *       y = result[1];
       *       z = result[2];
       *    }
       */
      nir_def *is_prev_acc_result = nir_i2b(&b, nir_iand(&b, b0_bitfield, one));
      nir_if *if_prev_acc_result = nir_push_if(&b, is_prev_acc_result); {
         nir_def *result = nir_load_ssbo(&b, 3, 32, one, zero);
         nir_store_var(&b, x, nir_channel(&b, result, 0), 0x1);
         nir_store_var(&b, y, nir_channel(&b, result, 1), 0x1);
         nir_store_var(&b, z, nir_channel(&b, result, 2), 0x1);
      }
      nir_pop_if(&b, if_prev_acc_result);

      /* if (!z) {
       *    uint32_t result_index = 0;
       *    uint32_t pitch = 0;
       *    ...
       */
      nir_def *z_value = nir_load_var(&b, z);
      nir_if *if_not_z = nir_push_if(&b, nir_ieq(&b, z_value, zero)); {
         nir_variable *outer_loop_iter =
            nir_local_variable_create(e, glsl_uint_type(), "outer_loop_iter");
         nir_store_var(&b, outer_loop_iter, zero, 0x1);
         nir_variable *pitch = nir_local_variable_create(e, glsl_uint_type(), "pitch");
         nir_store_var(&b, pitch, zero, 0x1);

         /* Outer loop.
          *   while (result_index <= result_count) {
          *      ...
          */
         nir_loop *loop_outer = nir_push_loop(&b); {
            nir_def *result_index = nir_load_var(&b, outer_loop_iter);
            nir_def *is_result_index_out_of_bound =
               nir_uge(&b, result_index, nir_channel(&b, buff_0, 2));
            nir_if *if_out_of_bound = nir_push_if(&b, is_result_index_out_of_bound); {
               nir_jump(&b, nir_jump_break);
            }
            nir_pop_if(&b, if_out_of_bound);

            /* Load fence and check result availability.
             *    pitch = i * result_stride;
             *    uint32_t address = fence_offset + pitch;
             *    int32_t value = load_ssbo(0, address);
             *    z = ~(value >> 31);
             */
            nir_def *pitch_outer_loop = nir_imul(&b, result_index, nir_channel(&b, buff_0, 1));
            nir_store_var(&b, pitch, pitch_outer_loop, 0x1);
            nir_def *address = nir_iadd(&b, pitch_outer_loop, nir_channel(&b, buff_1, 0));
            nir_def *value = nir_load_ssbo(&b, 1, 32, zero, address);
            nir_def *bitmask = nir_inot(&b, nir_ishr(&b, value, thirty_one));
            nir_store_var(&b, z, bitmask, 0x1);

            /*    if (z) {
             *       break;
             *    }
             */
            nir_if *if_result_available = nir_push_if(&b, nir_i2b(&b, bitmask)); {
               nir_jump(&b, nir_jump_break);
            }
            nir_pop_if(&b, if_result_available);

            /* Inner loop iterator.
             *    uint32_t i = 0;
             */
            nir_variable *inner_loop_iter =
               nir_local_variable_create(e, glsl_uint_type(), "inner_loop_iter");
            nir_store_var(&b, inner_loop_iter, zero, 0x1);

            /* Inner loop.
             *    do {
             *       ...
             */
            nir_loop *loop_inner = nir_push_loop(&b); {
               nir_def *pitch_inner_loop = nir_load_var(&b, pitch);
               nir_def *i = nir_load_var(&b, inner_loop_iter);

               /* Load start and end.
                *    uint64_t first = load_ssbo(0, pitch);
                *    uint64_t second = load_ssbo(0, pitch + end_offset);
                *    uint64_t start_half_pair = second - first;
                */
               nir_def *first = nir_load_ssbo(&b, 1, 64, zero, pitch_inner_loop);
               nir_def *new_pitch = nir_iadd(&b, pitch_inner_loop, nir_channel(&b, buff_0, 0));
               nir_def *second = nir_load_ssbo(&b, 1, 64, zero, new_pitch);
               nir_def *start_half_pair = nir_isub(&b, second, first);

               /* Load second start/end half-pair and take the difference.
                *    if (b0_bitfield & (1u << 8)) {
                *       uint64_t first = load_ssbo(0, pitch + 8);
                *       uint64_t second = load_ssbo(0, pitch + end_offset + 8);
                *       uint64_t end_half_pair = second - first;
                *       uint64_t difference = start_half_pair - end_half_pair;
                *    }
                */
               nir_def *difference;
               nir_def *is_so_overflow_mode = nir_i2b(&b, nir_iand_imm(&b, b0_bitfield, 256));
               nir_if *if_so_overflow_mode = nir_push_if(&b, is_so_overflow_mode); {
                  first = nir_load_ssbo(&b, 1, 64, zero, nir_iadd(&b, pitch_inner_loop, eight));
                  second = nir_load_ssbo(&b, 1, 64, zero, nir_iadd(&b, new_pitch, eight));
                  nir_def *end_half_pair = nir_isub(&b, second, first);
                  difference = nir_isub(&b, start_half_pair, end_half_pair);
               }
               nir_pop_if(&b, if_so_overflow_mode);

               /* uint64_t sum = (x | (uint64_t) y << 32) + difference; */
               nir_def *sum = nir_iadd(&b,
                                       nir_pack_64_2x32_split(&b,
                                                              nir_load_var(&b, x),
                                                              nir_load_var(&b, y)),
                                       nir_if_phi(&b, difference, start_half_pair));
               sum = nir_unpack_64_2x32(&b, sum);

               /* Increment inner loop iterator.
                *    i++;
                */
               i = nir_iadd(&b, i, one);
               nir_store_var(&b, inner_loop_iter, i, 0x1);

               /* Update pitch value.
                *    pitch = i * pair_stride + pitch;
                */
               nir_def *incremented_pitch = nir_iadd(&b,
                                             nir_imul(&b, i, nir_channel(&b, buff_1, 1)),
                                             pitch_outer_loop);
               nir_store_var(&b, pitch, incremented_pitch, 0x1);

               /* Update x and y.
                *    x = sum.x;
                *    y = sum.x >> 32;
                */
               nir_store_var(&b, x, nir_channel(&b, sum, 0), 0x1);
               nir_store_var(&b, y, nir_channel(&b, sum, 1), 0x1);

               /* } while (i < pair_count);
               */
               nir_def *is_pair_count_exceeded = nir_uge(&b, i, nir_channel(&b, buff_1, 2));
               nir_if *if_pair_count_exceeded = nir_push_if(&b, is_pair_count_exceeded); {
                  nir_jump(&b, nir_jump_break);
               }
               nir_pop_if(&b, if_pair_count_exceeded);
            }
            nir_pop_loop(&b, loop_inner);

            /* Increment pair iterator.
             *    result_index++;
             */
            nir_store_var(&b, outer_loop_iter, nir_iadd(&b, result_index, one), 0x1);
         }
         nir_pop_loop(&b, loop_outer);
      }
      nir_pop_if(&b, if_not_z);
   }
   nir_pop_if(&b, if_one_dword_result);

   nir_def *x_value = nir_load_var(&b, x);
   nir_def *y_value = nir_load_var(&b, y);
   nir_def *z_value = nir_load_var(&b, z);

   /* Store accumulated data for chaining.
    *    if (b0_bitfield & (1u << 1)) {
    *       store_ssbo(<x, y, z>, 2, 0);
    */
   nir_def *is_acc_chaining = nir_i2b(&b, nir_iand(&b, b0_bitfield, two));
   nir_if *if_acc_chaining = nir_push_if(&b, is_acc_chaining); {
      nir_store_ssbo(&b, nir_vec3(&b, x_value, y_value, z_value), two, zero);
   } nir_push_else(&b, if_acc_chaining); {

      /* Store result availability.
       *    } else {
       *       if (b0_bitfield & (1u << 2)) {
       *          store_ssbo((~z & 1), 2, 0);
       *          ...
       */
      nir_def *is_result_available = nir_i2b(&b, nir_iand(&b, b0_bitfield, four));
      nir_if *if_result_available = nir_push_if(&b, is_result_available); {
         nir_store_ssbo(&b, nir_iand(&b, nir_inot(&b, z_value), one), two, zero);

         /* Store full 64 bits result.
          *    if (b0_bitfield & (1u << 6)) {
          *       store_ssbo(<0, 0>, 2, 0);
          *    }
          */
         nir_def *is_result_64_bits = nir_i2b(&b, nir_iand(&b, b0_bitfield, sixty_four));
         nir_if *if_result_64_bits = nir_push_if(&b, is_result_64_bits); {
            nir_store_ssbo(&b, nir_imm_ivec2(&b, 0, 0), two, zero,
                           .write_mask = (1u << 1));
         }
         nir_pop_if(&b, if_result_64_bits);
      } nir_push_else(&b, if_result_available); {

         /* } else {
          *    if (~z) {
          *       ...
          */
         nir_def *is_bitwise_not_z = nir_i2b(&b, nir_inot(&b, z_value));
         nir_if *if_bitwise_not_z = nir_push_if(&b, is_bitwise_not_z); {
            nir_def *ts_x, *ts_y;

            /* Apply timestamp conversion.
             *    if (b0_bitfield & (1u << 5)) {
             *       uint64_t xy_million = (x | (uint64_t) y << 32) * (uint64_t) 1000000;
             *       uint64_t ts_converted = xy_million / (uint64_t) clock_crystal_frequency;
             *       x = ts_converted.x;
             *       y = ts_converted.x >> 32;
             *    }
             */
            nir_def *is_apply_timestamp = nir_i2b(&b, nir_iand_imm(&b, b0_bitfield, 32));
            nir_if *if_apply_timestamp = nir_push_if(&b, is_apply_timestamp); {
               /* Add the frequency into the shader for timestamp conversion
                * so that the backend can use the full range of optimizations
                * for divide-by-constant.
                */
               nir_def *clock_crystal_frequency =
                  nir_imm_int64(&b, sctx->screen->info.clock_crystal_freq);

               nir_def *xy_million = nir_imul(&b,
                                           nir_pack_64_2x32_split(&b, x_value, y_value),
                                           nir_imm_int64(&b, 1000000));
               nir_def *ts_converted = nir_udiv(&b, xy_million, clock_crystal_frequency);
               ts_converted = nir_unpack_64_2x32(&b, ts_converted);
               ts_x = nir_channel(&b, ts_converted, 0);
               ts_y = nir_channel(&b, ts_converted, 1);
            }
            nir_pop_if(&b, if_apply_timestamp);

            nir_def *nx = nir_if_phi(&b, ts_x, x_value);
            nir_def *ny = nir_if_phi(&b, ts_y, y_value);

            /* x = b0_bitfield & (1u << 3) ? ((x | (uint64_t) y << 32) != 0) : x;
             * y = b0_bitfield & (1u << 3) ? 0 : y;
             */
            nir_def *is_convert_to_bool = nir_i2b(&b, nir_iand(&b, b0_bitfield, eight));
            nir_def *xy = nir_pack_64_2x32_split(&b, nx, ny);
            nir_def *is_xy = nir_b2i32(&b, nir_ine(&b, xy, nir_imm_int64(&b, 0)));
            nx = nir_bcsel(&b, is_convert_to_bool, is_xy, nx);
            ny = nir_bcsel(&b, is_convert_to_bool, zero, ny);

            /* if (b0_bitfield & (1u << 6)) {
             *    store_ssbo(<x, y>, 2, 0);
             * }
             */
            nir_def *is_result_64_bits = nir_i2b(&b, nir_iand(&b, b0_bitfield, sixty_four));
            nir_if *if_result_64_bits = nir_push_if(&b, is_result_64_bits); {
               nir_store_ssbo(&b, nir_vec2(&b, nx, ny), two, zero);
            } nir_push_else(&b, if_result_64_bits); {

               /* Clamping.
                *    } else {
                *       x = y ? UINT32_MAX : x;
                *       x = b0_bitfield & (1u << 7) ? min(x, INT_MAX) : x;
                *       store_ssbo(x, 2, 0);
                *    }
                */
               nir_def *is_y = nir_ine(&b, ny, zero);
               nx = nir_bcsel(&b, is_y, nir_imm_int(&b, UINT32_MAX), nx);
               nir_def *is_signed_32bit_result = nir_i2b(&b, nir_iand_imm(&b, b0_bitfield, 128));
               nir_def *min = nir_umin(&b, nx, nir_imm_int(&b, INT_MAX));
               nx = nir_bcsel(&b, is_signed_32bit_result, min, nx);
               nir_store_ssbo(&b, nx, two, zero);
            }
            nir_pop_if(&b, if_result_64_bits);
         }
         nir_pop_if(&b, if_bitwise_not_z);
      }
      nir_pop_if(&b, if_result_available);
   }
   nir_pop_if(&b, if_acc_chaining);

   return si_create_shader_state(sctx, b.shader);
}

/* Create the compute shader that is used to collect the results of gfx10+
 * shader queries.
 *
 * One compute grid with a single thread is launched for every query result
 * buffer. The thread (optionally) reads a previous summary buffer, then
 * accumulates data from the query result buffer, and writes the result either
 * to a summary buffer to be consumed by the next grid invocation or to the
 * user-supplied buffer.
 *
 * Data layout:
 *
 * CONST
 *  0.x = config;
 *          [0:2] the low 3 bits indicate the mode:
 *             0: sum up counts
 *             1: determine result availability and write it as a boolean
 *             2: SO_OVERFLOW
 *          3: SO_ANY_OVERFLOW
 *        the remaining bits form a bitfield:
 *          8: write result as a 64-bit value
 *  0.y = offset in bytes to counts or stream for SO_OVERFLOW mode
 *  0.z = chain bit field:
 *          1: have previous summary buffer
 *          2: write next summary buffer
 *  0.w = result_count
 */
void *gfx11_create_sh_query_result_cs(struct si_context *sctx)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, sctx->screen->nir_options,
                                                  "gfx11_create_sh_query_result_cs");
   b.shader->info.workgroup_size[0] = 1;
   b.shader->info.workgroup_size[1] = 1;
   b.shader->info.workgroup_size[2] = 1;
   b.shader->info.num_ubos = 1;
   b.shader->info.num_ssbos = 3;
   b.shader->num_uniforms = 1;

   nir_def *zero = nir_imm_int(&b, 0);
   nir_def *one = nir_imm_int(&b, 1);
   nir_def *two = nir_imm_int(&b, 2);
   nir_def *four = nir_imm_int(&b, 4);
   nir_def *minus_one = nir_imm_int(&b, 0xffffffff);

   /* uint32_t acc_result = 0, acc_missing = 0; */
   nir_function_impl *e = nir_shader_get_entrypoint(b.shader);
   nir_variable *acc_result = nir_local_variable_create(e, glsl_uint_type(), "acc_result");
   nir_store_var(&b, acc_result, zero, 0x1);
   nir_variable *acc_missing = nir_local_variable_create(e, glsl_uint_type(), "acc_missing");
   nir_store_var(&b, acc_missing, zero, 0x1);

   /* uint32_t buff_0[4] = load_ubo(0, 0); */
   nir_def *buff_0 = nir_load_ubo(&b, 4, 32, zero, zero, .range_base = 0, .range = 16);

   /* if((chain & 1) {
    *    uint32_t result[2] = load_ssbo(1, 0);
    *    acc_result = result[0];
    *    acc_missing = result[1];
    * }
    */
   nir_def *is_prev_summary_buffer = nir_i2b(&b, nir_iand(&b, nir_channel(&b, buff_0, 2), one));
   nir_if *if_prev_summary_buffer = nir_push_if(&b, is_prev_summary_buffer); {
      nir_def *result = nir_load_ssbo(&b, 2, 32, one, zero);
         nir_store_var(&b, acc_result, nir_channel(&b, result, 0), 0x1);
         nir_store_var(&b, acc_missing, nir_channel(&b, result, 1), 0x1);
   }
   nir_pop_if(&b, if_prev_summary_buffer);

   /* uint32_t mode = config & 0b111;
    * bool is_overflow = mode >= 2;
    */
   nir_def *mode = nir_iand_imm(&b, nir_channel(&b, buff_0, 0), 0b111);
   nir_def *is_overflow = nir_uge(&b, mode, two);

   /* uint32_t result_remaining = (is_overflow && acc_result) ? 0 : result_count; */
   nir_variable *result_remaining = nir_local_variable_create(e, glsl_uint_type(), "result_remaining");
   nir_variable *base_offset = nir_local_variable_create(e, glsl_uint_type(), "base_offset");
   nir_def *state = nir_iand(&b,
                             nir_isub(&b, zero, nir_b2i32(&b, is_overflow)),
                             nir_load_var(&b, acc_result));
   nir_def *value = nir_bcsel(&b, nir_i2b(&b, state), zero, nir_channel(&b, buff_0, 3));
   nir_store_var(&b, result_remaining, value, 0x1);

   /* uint32_t base_offset = 0; */
   nir_store_var(&b, base_offset, zero, 0x1);

   /* Outer loop begin.
    *   while (!result_remaining) {
    *      ...
    */
   nir_loop *loop_outer = nir_push_loop(&b); {
      nir_def *condition = nir_load_var(&b, result_remaining);
      nir_if *if_not_condition = nir_push_if(&b, nir_ieq(&b, condition, zero)); {
         nir_jump(&b, nir_jump_break);
      }
      nir_pop_if(&b, if_not_condition);

      /* result_remaining--; */
      condition = nir_iadd(&b, condition, minus_one);
      nir_store_var(&b, result_remaining, condition, 0x1);

      /* uint32_t fence = load_ssbo(0, base_offset + sizeof(gfx11_sh_query_buffer_mem.stream)); */
      nir_def *b_offset = nir_load_var(&b, base_offset);
      uint64_t buffer_mem_stream_size = sizeof(((struct gfx11_sh_query_buffer_mem*)0)->stream);
      nir_def *fence = nir_load_ssbo(&b, 1, 32, zero,
                                    nir_iadd_imm(&b, b_offset, buffer_mem_stream_size));

      /* if (!fence) {
       *    acc_missing = ~0u;
       *    break;
       * }
       */
      nir_def *is_zero = nir_ieq(&b, fence, zero);
      nir_def *y_value = nir_isub(&b, zero, nir_b2i32(&b, is_zero));
      nir_store_var(&b, acc_missing, y_value, 0x1);
      nir_if *if_ssbo_zero = nir_push_if(&b, is_zero); {
         nir_jump(&b, nir_jump_break);
      }
      nir_pop_if(&b, if_ssbo_zero);

      /* stream_offset = base_offset + offset; */
      nir_def *s_offset = nir_iadd(&b, b_offset, nir_channel(&b, buff_0, 1));

      /* if (!(config & 7)) {
       *    acc_result += buffer[0]@stream_offset;
       * }
       */
      nir_if *if_sum_up_counts = nir_push_if(&b, nir_ieq(&b, mode, zero)); {
         nir_def *x_value = nir_load_ssbo(&b, 1, 32, zero, s_offset);
         x_value = nir_iadd(&b, nir_load_var(&b, acc_result), x_value);
         nir_store_var(&b, acc_result, x_value, 0x1);
      }
      nir_pop_if(&b, if_sum_up_counts);

      /* if (is_overflow) {
       *    uint32_t count = (config & 1) ? 4 : 1;
       *    ...
       */
      nir_if *if_overflow = nir_push_if(&b, is_overflow); {
         nir_def *is_result_available = nir_i2b(&b, nir_iand(&b, mode, one));
         nir_def *initial_count = nir_bcsel(&b, is_result_available, four, one);

         nir_variable *count =
            nir_local_variable_create(e, glsl_uint_type(), "count");
         nir_store_var(&b, count, initial_count, 0x1);

         nir_variable *stream_offset =
            nir_local_variable_create(e, glsl_uint_type(), "stream_offset");
         nir_store_var(&b, stream_offset, s_offset, 0x1);

         /* Inner loop begin.
          *    do {
          *       ...
          */
         nir_loop *loop_inner = nir_push_loop(&b); {
            /* uint32_t buffer[4] = load_ssbo(0, stream_offset + 2 * sizeof(uint64_t)); */
            nir_def *stream_offset_value = nir_load_var(&b, stream_offset);
            nir_def *buffer =
               nir_load_ssbo(&b, 4, 32, zero,
                             nir_iadd_imm(&b, stream_offset_value, 2 * sizeof(uint64_t)));

            /* if (generated != emitted) {
             *    acc_result = 1;
             *    base_offset = 0;
             *    break;
             * }
             */
            nir_def *generated = nir_channel(&b, buffer, 0);
            nir_def *emitted = nir_channel(&b, buffer, 2);
            nir_if *if_not_equal = nir_push_if(&b, nir_ine(&b, generated, emitted)); {
               nir_store_var(&b, acc_result, one, 0x1);
               nir_store_var(&b, base_offset, zero, 0x1);
               nir_jump(&b, nir_jump_break);
            }
            nir_pop_if(&b, if_not_equal);

            /* stream_offset += sizeof(gfx11_sh_query_buffer_mem.stream[0]); */
            uint64_t buffer_mem_stream0_size =
               sizeof(((struct gfx11_sh_query_buffer_mem*)0)->stream[0]);
            stream_offset_value = nir_iadd_imm(&b, stream_offset_value, buffer_mem_stream0_size);
            nir_store_var(&b, stream_offset, stream_offset_value, 0x1);

            /* } while(count--); */
            nir_def *loop_count = nir_load_var(&b, count);
            loop_count = nir_iadd(&b, loop_count, minus_one);
            nir_store_var(&b, count, loop_count, 0x1);

            nir_if *if_zero = nir_push_if(&b, nir_ieq(&b, loop_count, zero)); {
               nir_jump(&b, nir_jump_break);
            }
            nir_pop_if(&b, if_zero);
         }
         nir_pop_loop(&b, loop_inner); /* Inner loop end */
      }
      nir_pop_if(&b, if_overflow);

      /* base_offset += sizeof(gfx11_sh_query_buffer_mem); */
      nir_def *buffer_mem_size = nir_imm_int(&b, sizeof(struct gfx11_sh_query_buffer_mem));
      nir_store_var(&b, base_offset, nir_iadd(&b, nir_load_var(&b, base_offset), buffer_mem_size), 0x1);
   }
   nir_pop_loop(&b, loop_outer); /* Outer loop end */

   nir_def *acc_result_value = nir_load_var(&b, acc_result);
   nir_def *y_value = nir_load_var(&b, acc_missing);

   /* if ((chain & 2)) {
    *    store_ssbo(<acc_result, acc_missing>, 2, 0);
    *    ...
    */
   nir_def *is_write_summary_buffer = nir_i2b(&b, nir_iand(&b, nir_channel(&b, buff_0, 2), two));
   nir_if *if_write_summary_buffer = nir_push_if(&b, is_write_summary_buffer); {
      nir_store_ssbo(&b, nir_vec2(&b, acc_result_value, y_value), two, zero);
   } nir_push_else(&b, if_write_summary_buffer); {

      /* } else {
       *    if ((config & 7) == 1) {
       *       acc_result = acc_missing ? 0 : 1;
       *       acc_missing = 0;
       *    }
       *    ...
       */
      nir_def *is_result_available = nir_ieq(&b, mode, one);
      nir_def *is_zero = nir_ieq(&b, y_value, zero);
      acc_result_value = nir_bcsel(&b, is_result_available, nir_b2i32(&b, is_zero), acc_result_value);
      nir_def *ny = nir_bcsel(&b, is_result_available, zero, y_value);

      /* if (!acc_missing) {
       *    store_ssbo(acc_result, 2, 0);
       *    if (config & 8)) {
       *       store_ssbo(0, 2, 4)
       *    }
       * }
       */
      nir_if *if_zero = nir_push_if(&b, nir_ieq(&b, ny, zero)); {
         nir_store_ssbo(&b, acc_result_value, two, zero);

         nir_def *is_so_any_overflow = nir_i2b(&b, nir_iand_imm(&b, nir_channel(&b, buff_0, 0), 8));
         nir_if *if_so_any_overflow = nir_push_if(&b, is_so_any_overflow); {
            nir_store_ssbo(&b, zero, two, four);
         }
         nir_pop_if(&b, if_so_any_overflow);
      }
      nir_pop_if(&b, if_zero);
   }
   nir_pop_if(&b, if_write_summary_buffer);

   return si_create_shader_state(sctx, b.shader);
}
