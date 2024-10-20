/*
 * Copyright 2021 Alyssa Rosenzweig
 * Copyright 2020 Collabora Ltd.
 * Copyright 2016 Broadcom
 * SPDX-License-Identifier: MIT
 */

#include "agx_compile.h"
#include "asahi/layout/layout.h"
#include "compiler/nir/nir_builder.h"
#include "util/bitset.h"
#include "util/glheader.h"
#include "util/list.h"
#include "util/macros.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"
#include "agx_builder.h"
#include "agx_compiler.h"
#include "agx_debug.h"
#include "agx_nir.h"
#include "glsl_types.h"
#include "nir.h"
#include "nir_builtin_builder.h"
#include "nir_intrinsics.h"
#include "nir_intrinsics_indices.h"
#include "shader_enums.h"

/* Alignment for shader programs. I'm not sure what the optimal value is. */
#define AGX_CODE_ALIGN 0x100

/* clang-format off */
static const struct debug_named_value agx_debug_options[] = {
   {"shaders",   AGX_DBG_SHADERS,	"Dump shaders in NIR and AIR"},
   {"shaderdb",  AGX_DBG_SHADERDB,	"Print statistics"},
   {"verbose",   AGX_DBG_VERBOSE,	"Disassemble verbosely"},
   {"internal",  AGX_DBG_INTERNAL,	"Dump even internal shaders"},
   {"novalidate",AGX_DBG_NOVALIDATE,"Skip IR validation in debug builds"},
   {"noopt",     AGX_DBG_NOOPT,     "Disable backend optimizations"},
   {"wait",      AGX_DBG_WAIT,      "Wait after all async instructions"},
   {"nopreamble",AGX_DBG_NOPREAMBLE,"Do not use shader preambles"},
   {"demand",    AGX_DBG_DEMAND,    "Bound tightly to register demand"},
   {"nosched",   AGX_DBG_NOSCHED,   "Do not schedule the shader"},
   {"spill",     AGX_DBG_SPILL,     "Spill (almost) everything"},
   {"nopromote", AGX_DBG_NOPROMOTE, "Do not promote constants to uniforms"},
   DEBUG_NAMED_VALUE_END
};
/* clang-format on */

DEBUG_GET_ONCE_FLAGS_OPTION(agx_compiler_debug, "AGX_MESA_DEBUG",
                            agx_debug_options, 0)

int agx_compiler_debug = 0;

uint64_t
agx_get_compiler_debug(void)
{
   return debug_get_option_agx_compiler_debug();
}

static agx_index
agx_cached_preload(agx_context *ctx, unsigned base, enum agx_size size)
{
   if (agx_is_null(ctx->preloaded[base])) {
      agx_block *block = agx_start_block(ctx);
      agx_builder b = agx_init_builder(ctx, agx_before_block(block));
      ctx->preloaded[base] = agx_preload(&b, agx_register(base, size));
   }

   return ctx->preloaded[base];
}

static agx_index
agx_tess_coord_x(agx_builder *b)
{
   return agx_cached_preload(b->shader, 4, AGX_SIZE_32);
}

static agx_index
agx_tess_coord_y(agx_builder *b)
{
   return agx_cached_preload(b->shader, 6, AGX_SIZE_32);
}

static agx_index
agx_vertex_id(agx_builder *b)
{
   return agx_cached_preload(b->shader, 10, AGX_SIZE_32);
}

static agx_index
agx_instance_id(agx_builder *b)
{
   return agx_cached_preload(b->shader, 12, AGX_SIZE_32);
}

#define VARYING_NUM_COMPONENTS (VARYING_SLOT_MAX * 4)

struct coefficient_info {
   BITSET_DECLARE(smooth, VARYING_NUM_COMPONENTS);
   BITSET_DECLARE(flat, VARYING_NUM_COMPONENTS);
   BITSET_DECLARE(noperspective, VARYING_NUM_COMPONENTS);
};

static BITSET_WORD *
bitset_for_interp(struct coefficient_info *info, enum glsl_interp_mode mode)
{
   /* clang-format off */
   switch (mode) {
   case INTERP_MODE_NONE:
   case INTERP_MODE_SMOOTH:         return info->smooth;
   case INTERP_MODE_NOPERSPECTIVE:  return info->noperspective;
   case INTERP_MODE_FLAT:           return info->flat;
   default:                         unreachable("invalid interp mode");
   }
   /* clang-format on */
}

static bool
gather_cf(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   /* First handle frag coord loads */
   struct coefficient_info *info = data;
   if (intr->intrinsic == nir_intrinsic_load_frag_coord_zw) {
      BITSET_SET(info->noperspective,
                 VARYING_SLOT_POS + nir_intrinsic_component(intr));
      return false;
   }

   /* Look for input loads and grab the instruction with the interp mode */
   nir_intrinsic_instr *bary;
   unsigned nr = 1;

   if (intr->intrinsic == nir_intrinsic_load_coefficients_agx) {
      bary = intr;
      /* Always load a scalar */
   } else if (intr->intrinsic == nir_intrinsic_load_interpolated_input) {
      bary = nir_src_as_intrinsic(intr->src[0]);
      nr = intr->num_components;

      /* Perspective interpolation internally reads W */
      if (nir_intrinsic_interp_mode(bary) != INTERP_MODE_NOPERSPECTIVE)
         BITSET_SET(info->noperspective, VARYING_SLOT_POS + 3);
   } else {
      return false;
   }

   BITSET_WORD *set = bitset_for_interp(data, nir_intrinsic_interp_mode(bary));
   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   nir_src *offset = nir_get_io_offset_src(intr);

   /* Mark the exact range for direct loads to minimize CF registers, but mark a
    * conservative bounding range for indirect array access.
    */
   if (nir_src_is_const(*offset)) {
      unsigned location = sem.location + nir_src_as_uint(*offset);
      unsigned start_comp = (location * 4) + nir_intrinsic_component(intr);

      BITSET_SET_RANGE(set, start_comp, start_comp + nr - 1);
   } else {
      unsigned start_comp = (sem.location * 4) + nir_intrinsic_component(intr);
      bool compact = sem.location == VARYING_SLOT_CLIP_DIST0 ||
                     sem.location == VARYING_SLOT_CLIP_DIST1;
      unsigned stride = compact ? 1 : 4;

      /* For now we have to assign CF for the whole vec4 to make indirect
       * indexiing work. This could be optimized later.
       */
      nr = stride;

      for (unsigned i = 0; i < sem.num_slots; ++i) {
         BITSET_SET_RANGE(set, start_comp + (i * stride),
                          start_comp + (i * stride) + nr - 1);
      }
   }

   return false;
}

/*
 * We assign all coefficient registers up front to ensure we have a consistent
 * layout required for indirects to work.
 */
static void
assign_coefficient_regs(nir_shader *nir, struct agx_varyings_fs *var)
{
   struct coefficient_info info = {0};
   nir_shader_intrinsics_pass(nir, gather_cf, nir_metadata_all, &info);

   /* W */
   if (BITSET_TEST(info.noperspective, VARYING_SLOT_POS + 3)) {
      var->bindings[var->nr_bindings++] = (struct agx_cf_binding){
         .cf_base = var->nr_cf++,
         .slot = VARYING_SLOT_POS,
         .offset = 3,
         .count = 1,
         .smooth = true,
      };
   }

   /* Z */
   if (BITSET_TEST(info.noperspective, VARYING_SLOT_POS + 2)) {
      var->bindings[var->nr_bindings++] = (struct agx_cf_binding){
         .cf_base = var->nr_cf++,
         .slot = VARYING_SLOT_POS,
         .offset = 2,
         .count = 1,
         .smooth = true,
      };

      var->reads_z = true;
   }

   static_assert(VARYING_SLOT_POS == 0, "special and handled first");

   for (unsigned i = VARYING_SLOT_POS + 1; i < VARYING_SLOT_MAX; ++i) {
      bool smooth = BITSET_TEST_RANGE(info.smooth, i * 4, (i * 4) + 3);
      bool flat = BITSET_TEST_RANGE(info.flat, i * 4, (i * 4) + 3);
      bool noperspective =
         BITSET_TEST_RANGE(info.noperspective, i * 4, (i * 4) + 3);

      if (!(smooth || flat || noperspective))
         continue;

      /* From the GLSL 4.60 spec ("Input Layout Qualifiers"):
       *
       *    when location aliasing, the aliases sharing the location must have
       *    the same underlying numerical type and bit width (floating-point or
       *    integer, 32-bit versus 64-bit, etc.) and the same auxiliary storage
       *    and interpolation qualification.
       *
       * SPIR-V should obey this as well although the spec text is muddier.
       */
      assert((smooth + flat + noperspective) == 1 &&
             "slots must have consistent interpolation");

      BITSET_WORD *set = smooth ? info.smooth
                         : flat ? info.flat
                                : info.noperspective;

      /* Find the start offset */
      unsigned offset = 0;
      for (offset = 0; offset < 4 && !BITSET_TEST(set, (i * 4) + offset);
           ++offset)
         ;

      /* Find the end offset. TODO: Do we ever need to split into two bindings
       * to handle e.g. x_zw read masks?
       */
      unsigned count = 0;
      for (unsigned c = offset; c < 4; ++c) {
         if (BITSET_TEST(set, (i * 4) + c))
            count = c - offset + 1;
      }
      assert(count >= 1 && (count + offset) <= 4);

      var->bindings[var->nr_bindings++] = (struct agx_cf_binding){
         .cf_base = var->nr_cf,
         .slot = i,
         .offset = offset,
         .count = count,
         .smooth = !flat,
         .perspective = smooth,
      };

      var->nr_cf += count;
   }
}

static agx_index
agx_get_cf(agx_context *ctx, gl_varying_slot slot, unsigned offset)
{
   struct agx_varyings_fs *varyings = &ctx->out->varyings.fs;

   /* We already have an appropriate binding, find it */
   for (unsigned b = 0; b < varyings->nr_bindings; ++b) {
      if (varyings->bindings[b].slot == slot &&
          (slot != VARYING_SLOT_POS ||
           offset == varyings->bindings[b].offset)) {

         signed cf_offset = offset - varyings->bindings[b].offset;
         assert(cf_offset >= 0);

         return agx_immediate(varyings->bindings[b].cf_base + cf_offset);
      }
   }

   unreachable("all coefficient registers preassigned");
}

/* Builds a 64-bit hash table key for an index */
static uint64_t
agx_index_to_key(agx_index idx)
{
   STATIC_ASSERT(sizeof(idx) <= sizeof(uint64_t));

   uint64_t key = 0;
   memcpy(&key, &idx, sizeof(idx));
   return key;
}

/*
 * Extract a single channel out of a vector source. We split vectors with
 * p_split so we can use the split components directly, without emitting a
 * machine instruction. This has advantages of RA, as the split can usually be
 * optimized away.
 */
static agx_index
agx_emit_extract(agx_builder *b, agx_index vec, unsigned channel)
{
   agx_index *components = _mesa_hash_table_u64_search(b->shader->allocated_vec,
                                                       agx_index_to_key(vec));

   assert(components != NULL && "missing agx_emit_collect_to");

   return components[channel];
}

static agx_index
agx_extract_nir_src(agx_builder *b, nir_src src, unsigned channel)
{
   agx_index idx = agx_src_index(&src);

   /* We only deal with scalars, extract a single scalar if needed */
   if (nir_src_num_components(src) > 1)
      return agx_emit_extract(b, idx, channel);
   else
      return idx;
}

static void
agx_cache_collect(agx_builder *b, agx_index dst, unsigned nr_srcs,
                  agx_index *srcs)
{
   /* Lifetime of a hash table entry has to be at least as long as the table */
   agx_index *channels = ralloc_array(b->shader, agx_index, nr_srcs);

   for (unsigned i = 0; i < nr_srcs; ++i)
      channels[i] = srcs[i];

   _mesa_hash_table_u64_insert(b->shader->allocated_vec, agx_index_to_key(dst),
                               channels);
}

/*
 * Combine multiple scalars into a vector destination. This corresponds to
 * collect, lowered to moves (a shuffle in general) after register allocation.
 *
 * To optimize vector extractions, we record the individual channels
 */
static agx_instr *
agx_emit_collect_to(agx_builder *b, agx_index dst, unsigned nr_srcs,
                    agx_index *srcs)
{
   agx_cache_collect(b, dst, nr_srcs, srcs);

   if (nr_srcs == 1)
      return agx_mov_to(b, dst, srcs[0]);

   agx_instr *I = agx_collect_to(b, dst, nr_srcs);

   agx_foreach_src(I, s)
      I->src[s] = srcs[s];

   return I;
}

static agx_index
agx_emit_collect(agx_builder *b, unsigned nr_srcs, agx_index *srcs)
{
   agx_index dst = agx_vec_temp(b->shader, srcs[0].size, nr_srcs);
   agx_emit_collect_to(b, dst, nr_srcs, srcs);
   return dst;
}

static agx_index
agx_vec2(agx_builder *b, agx_index s0, agx_index s1)
{
   return agx_emit_collect(b, 2, (agx_index[]){s0, s1});
}

static agx_index
agx_pad_to_32(agx_builder *b, agx_index s)
{
   assert(s.size == AGX_SIZE_16);
   assert(agx_channels(s) == 1);

   agx_index srcs[2] = {s, agx_undef(AGX_SIZE_16)};
   agx_index dst = agx_vec_temp(b->shader, AGX_SIZE_32, 1);
   agx_emit_collect_to(b, dst, 2, srcs);
   return dst;
}

static agx_index
agx_recollect_vector(agx_builder *b, nir_src vec)
{
   agx_index comps[4];
   unsigned nr = nir_src_num_components(vec);

   for (unsigned i = 0; i < nr; ++i)
      comps[i] = agx_extract_nir_src(b, vec, i);

   return agx_emit_collect(b, nr, comps);
}

/*
 * Extract the lower or upper N-bits from a (2*N)-bit quantity. We use a split
 * without null destinations to let us CSE (and coalesce) the splits when both x
 * and y are split.
 */
static agx_instr *
agx_subdivide_to(agx_builder *b, agx_index dst, agx_index s0, unsigned comp)
{
   assert((s0.size == (dst.size + 1)) && "only 2x subdivide handled");
   assert((comp == 0 || comp == 1) && "too many components");

   /* Handle immediates specially so we don't have to constant fold splits. */
   if (s0.type == AGX_INDEX_IMMEDIATE) {
      unsigned bits = 16 * agx_size_align_16(dst.size);
      return agx_mov_imm_to(b, dst, (s0.value >> bits) & BITFIELD64_MASK(bits));
   }

   agx_instr *split = agx_split(b, 2, s0);
   split->dest[comp] = dst;
   split->dest[1 - comp] = agx_temp(b->shader, dst.size);
   return split;
}

void
agx_block_add_successor(agx_block *block, agx_block *successor)
{
   assert(block != NULL && successor != NULL);

   /* Cull impossible edges */
   if (block->unconditional_jumps)
      return;

   for (unsigned i = 0; i < ARRAY_SIZE(block->successors); ++i) {
      if (block->successors[i]) {
         if (block->successors[i] == successor)
            return;
         else
            continue;
      }

      block->successors[i] = successor;
      util_dynarray_append(&successor->predecessors, agx_block *, block);
      return;
   }

   unreachable("Too many successors");
}

/*
 * Splits an n-component vector (vec) into n scalar destinations (dests) using a
 * split pseudo-instruction.
 *
 * Pre-condition: dests is filled with agx_null().
 */
static void
agx_emit_split(agx_builder *b, agx_index *dests, agx_index vec, unsigned n)
{
   agx_instr *I = agx_split(b, n, vec);

   agx_foreach_dest(I, d) {
      dests[d] = agx_temp(b->shader, vec.size);
      I->dest[d] = dests[d];
   }
}

static void
agx_emit_cached_split(agx_builder *b, agx_index vec, unsigned n)
{
   agx_index dests[4] = {agx_null(), agx_null(), agx_null(), agx_null()};
   agx_emit_split(b, dests, vec, n);
   agx_cache_collect(b, vec, n, dests);
}

static void
agx_emit_load_const(agx_builder *b, nir_load_const_instr *instr)
{
   /* Ensure we've been scalarized and bit size lowered */
   unsigned bit_size = instr->def.bit_size;
   assert(instr->def.num_components == 1);

   /* Emit move, later passes can inline/push if useful */
   agx_mov_imm_to(b, agx_def_index(&instr->def),
                  nir_const_value_as_uint(instr->value[0], bit_size));
}

/*
 * Implement mul_high of 32-bit sources by doing a 32x32->64-bit multiply and
 * extracting only the high word.
 */
static agx_instr *
agx_mul_high_to(agx_builder *b, agx_index dst, agx_index P, agx_index Q,
                bool is_signed)
{
   assert(P.size == Q.size && "source sizes must match");
   assert(P.size == dst.size && "dest size must match");
   assert(P.size != AGX_SIZE_64 && "64x64 multiply should have been lowered");

   static_assert(AGX_SIZE_64 == (AGX_SIZE_32 + 1), "enum wrong");
   static_assert(AGX_SIZE_32 == (AGX_SIZE_16 + 1), "enum wrong");

   if (!is_signed) {
      P = agx_abs(P);
      Q = agx_abs(Q);
   }

   agx_index product = agx_temp(b->shader, P.size + 1);
   agx_imad_to(b, product, P, Q, agx_zero(), 0);

   return agx_subdivide_to(b, dst, product, 1);
}

static enum agx_format
agx_format_for_pipe(enum pipe_format format)
{
#define CASE(x)                                                                \
   if (format == (enum pipe_format)AIL_ISA_FORMAT_##x)                         \
      return AGX_FORMAT_##x;

   CASE(I8);
   CASE(I16);
   CASE(I32);
   CASE(F16);
   CASE(U8NORM);
   CASE(S8NORM);
   CASE(U16NORM);
   CASE(S16NORM);
   CASE(RGB10A2);
   CASE(SRGBA8);
   CASE(RG11B10F);
   CASE(RGB9E5);

#undef CASE
   unreachable("Invalid format");
}

static agx_index
cf_for_intrinsic(agx_builder *b, nir_intrinsic_instr *intr)
{
   /* Determine the base location, taking into account a constant offset */
   unsigned location = nir_intrinsic_io_semantics(intr).location;
   bool compact = location == VARYING_SLOT_CLIP_DIST0 ||
                  location == VARYING_SLOT_CLIP_DIST1;

   nir_src *offset = nir_get_io_offset_src(intr);
   if (nir_src_is_const(*offset)) {
      /* XXX: NIR is broken and uses constant offsets in slots but dynamic
       * offsets in scalars for compact varyings. This needs to be fixed
       * upstream.
       */
      location += nir_src_as_uint(*offset);
   }

   agx_index I = agx_get_cf(b->shader, location, nir_intrinsic_component(intr));

   /* If we have a non-constant offset, we add it to the CF. Offsets are in
    * vec4 slots (unless we're compact) but the CF is in components, so we need
    * to shift the offset by 2 before adding.
    */
   if (!nir_src_is_const(*offset)) {
      I = agx_iadd(b, I, agx_src_index(offset), compact ? 0 : 2);
   }

   return I;
}

static enum agx_interpolation
agx_interp_for_bary(nir_intrinsic_instr *bary, agx_index *sample_index)
{
   switch (bary->intrinsic) {
   case nir_intrinsic_load_barycentric_pixel:
      return AGX_INTERPOLATION_CENTER;

   case nir_intrinsic_load_barycentric_centroid:
      return AGX_INTERPOLATION_CENTROID;

   case nir_intrinsic_load_barycentric_at_sample:
      *sample_index = agx_src_index(&bary->src[0]);
      return AGX_INTERPOLATION_SAMPLE;

   default:
      unreachable("should have been lowered");
   }
}

static void
agx_emit_load_vary(agx_builder *b, agx_index dest, nir_intrinsic_instr *instr)
{
   ASSERTED unsigned components = instr->num_components;
   nir_intrinsic_instr *bary = nir_src_as_intrinsic(instr->src[0]);

   assert(components >= 1 && components <= 4);

   agx_index sample_index = agx_zero();
   enum agx_interpolation interp = agx_interp_for_bary(bary, &sample_index);

   bool perspective =
      nir_intrinsic_interp_mode(bary) != INTERP_MODE_NOPERSPECTIVE;

   agx_index I = cf_for_intrinsic(b, instr);

   /* For perspective interpolation, we project (multiply by 1/W) */
   if (perspective) {
      agx_index J = agx_get_cf(b->shader, VARYING_SLOT_POS, 3);
      agx_iterproj_to(b, dest, I, J, sample_index, components, interp);
   } else {
      agx_iter_to(b, dest, I, sample_index, components, interp);
   }

   agx_emit_cached_split(b, dest, components);
}

static agx_instr *
agx_emit_local_store_pixel(agx_builder *b, nir_intrinsic_instr *instr)
{
   bool explicit = nir_intrinsic_explicit_coord(instr);

   /* TODO: Reverse-engineer interactions with MRT */
   if (b->shader->stage == MESA_SHADER_FRAGMENT) {
      if (b->shader->key->fs.ignore_tib_dependencies) {
         assert(b->shader->nir->info.internal && "only for clear shaders");
      } else if (b->shader->did_writeout) {
         agx_wait_pix(b, 0x0004);
      } else {
         agx_wait_pix(b, 0x000C);
      }
   }

   /* Compact the registers according to the mask */
   agx_index compacted[4] = {agx_null()};

   unsigned compact_count = 0;
   u_foreach_bit(i, nir_intrinsic_write_mask(instr)) {
      compacted[compact_count++] = agx_extract_nir_src(b, instr->src[0], i);
   }

   agx_index collected = agx_emit_collect(b, compact_count, compacted);
   agx_index coords = explicit ? agx_src_index(&instr->src[2]) : agx_null();

   b->shader->did_writeout = true;
   b->shader->out->tag_write_disable = false;
   return agx_st_tile(b, collected, agx_src_index(&instr->src[1]), coords,
                      agx_format_for_pipe(nir_intrinsic_format(instr)),
                      nir_intrinsic_write_mask(instr),
                      nir_intrinsic_base(instr), explicit);
}

static agx_instr *
agx_emit_store_zs(agx_builder *b, nir_intrinsic_instr *instr)
{
   unsigned base = nir_intrinsic_base(instr);
   bool write_z = base & 1;
   bool write_s = base & 2;

   /* TODO: Handle better */
   assert(!b->shader->key->fs.ignore_tib_dependencies && "not used");
   agx_wait_pix(b, 0x0001);

   agx_index z = agx_src_index(&instr->src[1]);
   agx_index s = agx_src_index(&instr->src[2]);

   assert(!write_z || z.size == AGX_SIZE_32);
   assert(!write_s || s.size == AGX_SIZE_16);

   if (write_z && write_s) {
      agx_index u2u32 = agx_temp(b->shader, AGX_SIZE_32);
      agx_mov_to(b, u2u32, s);
      s = u2u32;
   }

   agx_index zs = (write_z && write_s) ? agx_vec2(b, z, s) : write_z ? z : s;

   /* Not necessarily a sample mask but overlapping hw mechanism... Should
    * maybe rename this flag to something more general.
    */
   b->shader->out->writes_sample_mask = true;

   return agx_zs_emit(b, agx_src_index(&instr->src[0]), zs, base);
}

static void
agx_emit_local_load_pixel(agx_builder *b, agx_index dest,
                          nir_intrinsic_instr *instr)
{
   /* TODO: Reverse-engineer interactions with MRT */
   assert(!b->shader->key->fs.ignore_tib_dependencies && "invalid usage");
   agx_wait_pix(b, 0x0008);
   b->shader->did_writeout = true;

   unsigned nr_comps = instr->def.num_components;
   agx_ld_tile_to(b, dest, agx_src_index(&instr->src[0]), agx_null(),
                  agx_format_for_pipe(nir_intrinsic_format(instr)),
                  BITFIELD_MASK(nr_comps), nir_intrinsic_base(instr), false);
   agx_emit_cached_split(b, dest, nr_comps);
}

static bool
nir_is_coherent(nir_intrinsic_instr *instr)
{
   return nir_intrinsic_access(instr) & (ACCESS_COHERENT | ACCESS_VOLATILE);
}

static void
agx_emit_load(agx_builder *b, agx_index dest, nir_intrinsic_instr *instr)
{
   agx_index addr = agx_src_index(&instr->src[0]);
   agx_index offset = agx_src_index(&instr->src[1]);
   enum agx_format fmt = agx_format_for_pipe(nir_intrinsic_format(instr));
   unsigned shift = nir_intrinsic_base(instr);

   /* Zero-extend offset if we're not sign-extending */
   if (!nir_intrinsic_sign_extend(instr))
      offset = agx_abs(offset);

   agx_device_load_to(b, dest, addr, offset, fmt,
                      BITFIELD_MASK(instr->def.num_components), shift,
                      nir_is_coherent(instr));
   agx_emit_cached_split(b, dest, instr->def.num_components);
}

static void
agx_emit_store(agx_builder *b, nir_intrinsic_instr *instr)
{
   agx_index addr = agx_src_index(&instr->src[1]);
   agx_index offset = agx_src_index(&instr->src[2]);
   enum agx_format fmt = agx_format_for_pipe(nir_intrinsic_format(instr));
   unsigned shift = nir_intrinsic_base(instr);

   /* Zero-extend offset if we're not sign-extending */
   if (!nir_intrinsic_sign_extend(instr))
      offset = agx_abs(offset);

   agx_device_store(b, agx_recollect_vector(b, instr->src[0]), addr, offset,
                    fmt, BITFIELD_MASK(nir_src_num_components(instr->src[0])),
                    shift, nir_is_coherent(instr));
}

/* Preambles write directly to uniform registers, so move from uniform to GPR */
static agx_instr *
agx_emit_load_preamble(agx_builder *b, agx_index dst,
                       nir_intrinsic_instr *instr)
{
   agx_index srcs[4] = {agx_null()};
   unsigned dim = instr->def.num_components;
   assert(dim <= ARRAY_SIZE(srcs) && "shouldn't see larger vectors");

   unsigned base = nir_intrinsic_base(instr);
   unsigned stride = agx_size_align_16(dst.size);

   for (unsigned i = 0; i < dim; ++i)
      srcs[i] = agx_uniform(base + i * stride, dst.size);

   return agx_emit_collect_to(b, dst, dim, srcs);
}

static agx_instr *
agx_emit_store_preamble(agx_builder *b, nir_intrinsic_instr *instr)
{
   agx_index vec = agx_src_index(&instr->src[0]);
   unsigned base = nir_intrinsic_base(instr);
   unsigned stride = agx_size_align_16(vec.size);
   unsigned nr = nir_src_num_components(instr->src[0]);

   for (unsigned i = 0; i < nr; i += (4 / stride)) {
      agx_index data[4] = {0};
      unsigned count = MIN2(4 / stride, nr - i);

      for (unsigned c = 0; c < count; ++c) {
         data[c] = agx_extract_nir_src(b, instr->src[0], i + c);
      }

      agx_uniform_store(b, agx_emit_collect(b, count, data),
                        agx_immediate(base + i * stride), BITFIELD_MASK(count));
   }

   return NULL;
}

static enum agx_dim
agx_tex_dim(enum glsl_sampler_dim dim, bool array)
{
   switch (dim) {
   case GLSL_SAMPLER_DIM_1D:
      return array ? AGX_DIM_1D_ARRAY : AGX_DIM_1D;

   case GLSL_SAMPLER_DIM_2D:
   case GLSL_SAMPLER_DIM_RECT:
   case GLSL_SAMPLER_DIM_EXTERNAL:
      return array ? AGX_DIM_2D_ARRAY : AGX_DIM_2D;

   case GLSL_SAMPLER_DIM_MS:
      return array ? AGX_DIM_2D_MS_ARRAY : AGX_DIM_2D_MS;

   case GLSL_SAMPLER_DIM_3D:
      assert(!array && "3D arrays unsupported");
      return AGX_DIM_3D;

   case GLSL_SAMPLER_DIM_CUBE:
      return array ? AGX_DIM_CUBE_ARRAY : AGX_DIM_CUBE;

   case GLSL_SAMPLER_DIM_BUF:
      unreachable("Buffer textures should have been lowered");

   default:
      unreachable("Invalid sampler dim\n");
   }
}

/*
 * In the hardware, bindless texture sources are specified as a 64-bit uniform
 * base address summed with a 32-bit register index. In NIR, we model this as a
 * vec2, where the first source is the (constant) uniform register number and
 * the second source is the (dynamic) byte offset.
 */
static agx_index
agx_translate_bindless_handle(agx_builder *b, nir_src *handle, agx_index *base)
{
   nir_scalar base_scalar = nir_scalar_resolved(handle->ssa, 0);
   assert(nir_scalar_is_const(base_scalar) && "base must be constant");

   unsigned base_uint = nir_scalar_as_uint(base_scalar);
   *base = agx_uniform(base_uint, AGX_SIZE_64);

   return agx_emit_extract(b, agx_src_index(handle), 1);
}

static agx_instr *
agx_emit_block_image_store(agx_builder *b, nir_intrinsic_instr *instr)
{
   agx_index offset = agx_src_index(&instr->src[1]);
   agx_index coords = agx_src_index(&instr->src[2]);
   enum agx_format format = agx_format_for_pipe(nir_intrinsic_format(instr));

   bool ms = nir_intrinsic_image_dim(instr) == GLSL_SAMPLER_DIM_MS;
   bool array = nir_intrinsic_image_array(instr);
   enum agx_dim dim = agx_tex_dim(nir_intrinsic_image_dim(instr), array);
   bool explicit = nir_intrinsic_explicit_coord(instr);

   /* 32-bit source physically, 16-bit in NIR, top half ignored but needed
    * logically to ensure alignment.
    */
   offset = agx_pad_to_32(b, offset);

   /* Modified coordinate descriptor */
   if (!explicit) {
      if (array) {
         agx_index layer = coords;
         coords = agx_temp(b->shader, AGX_SIZE_32);
         agx_emit_collect_to(b, coords, 2,
                             (agx_index[]){
                                ms ? agx_mov_imm(b, 16, 0) : layer,
                                ms ? layer : agx_undef(AGX_SIZE_16),
                             });
      } else {
         coords = agx_null();
      }
   }

   agx_index base, index;
   if (instr->intrinsic == nir_intrinsic_bindless_image_store_block_agx) {
      index = agx_translate_bindless_handle(b, &instr->src[0], &base);

      assert(base.size == AGX_SIZE_64);
      assert(index.size == AGX_SIZE_32);
   } else {
      base = agx_zero();
      index = agx_src_index(&instr->src[0]);

      assert(index.size == AGX_SIZE_16);
   }

   // XXX: how does this possibly work
   if (format == AGX_FORMAT_F16)
      format = AGX_FORMAT_I16;

   return agx_block_image_store(b, base, index, offset, coords, format, dim,
                                explicit);
}

static agx_instr *
agx_load_compute_dimension(agx_builder *b, agx_index dst,
                           nir_intrinsic_instr *instr, enum agx_sr base)
{
   unsigned dim = instr->def.num_components;
   unsigned size = instr->def.bit_size;
   assert(size == 16 || size == 32);

   agx_index srcs[] = {
      agx_get_sr(b, size, base + 0),
      agx_get_sr(b, size, base + 1),
      agx_get_sr(b, size, base + 2),
   };

   return agx_emit_collect_to(b, dst, dim, srcs);
}

static enum agx_atomic_opc
translate_atomic_opcode(nir_atomic_op op)
{
   /* clang-format off */
   switch (op) {
   case nir_atomic_op_iadd:    return AGX_ATOMIC_OPC_ADD;
   case nir_atomic_op_imin:    return AGX_ATOMIC_OPC_IMIN;
   case nir_atomic_op_umin:    return AGX_ATOMIC_OPC_UMIN;
   case nir_atomic_op_imax:    return AGX_ATOMIC_OPC_IMAX;
   case nir_atomic_op_umax:    return AGX_ATOMIC_OPC_UMAX;
   case nir_atomic_op_iand:    return AGX_ATOMIC_OPC_AND;
   case nir_atomic_op_ior:     return AGX_ATOMIC_OPC_OR;
   case nir_atomic_op_ixor:    return AGX_ATOMIC_OPC_XOR;
   case nir_atomic_op_xchg:    return AGX_ATOMIC_OPC_XCHG;
   case nir_atomic_op_cmpxchg: return AGX_ATOMIC_OPC_CMPXCHG;
   default: unreachable("unknown atomic opcode");
   }
   /* clang-format on */
}

/*
 * The "base" of a local load/store/atomic can be zero but no other immediates.
 * This would be a little silly to handle when inlining immediates, so we
 * instead exclude these ops from immediate inlining and just handle 0 specially
 * when translating.
 */
static agx_index
agx_local_base(nir_src src)
{
   if (nir_src_is_const(src) && nir_src_as_uint(src) == 0)
      return agx_zero();
   else
      return agx_src_index(&src);
}

static void
agx_emit_atomic(agx_builder *b, agx_index dst, nir_intrinsic_instr *instr,
                bool local)
{
   enum agx_atomic_opc op =
      translate_atomic_opcode(nir_intrinsic_atomic_op(instr));
   agx_index base =
      local ? agx_local_base(instr->src[0]) : agx_src_index(&instr->src[0]);
   agx_index value = agx_src_index(&instr->src[local ? 1 : 2]);
   agx_index index = local ? agx_zero() : agx_src_index(&instr->src[1]);

   /* cmpxchg (only) takes 2 sources, passed in consecutive registers */
   if (op == AGX_ATOMIC_OPC_CMPXCHG) {
      agx_index value2 = agx_src_index(&instr->src[local ? 2 : 3]);
      value = agx_vec2(b, value2, value);
   }

   if (local) {
      assert(base.size == AGX_SIZE_16);
      agx_local_atomic_to(b, dst, value, base, index, op);
   } else {
      assert(base.size == AGX_SIZE_64);
      agx_atomic_to(b, dst, value, base, index, op);
   }
}

static enum agx_format
format_for_bitsize(unsigned bitsize)
{
   switch (bitsize) {
   case 8:
      return AGX_FORMAT_I8;
   case 16:
      return AGX_FORMAT_I16;
   case 32:
      return AGX_FORMAT_I32;
   default:
      unreachable("should've been lowered");
   }
}

static void
agx_emit_local_load(agx_builder *b, agx_index dst, nir_intrinsic_instr *instr)
{
   agx_index base = agx_local_base(instr->src[0]);
   agx_index index = agx_zero(); /* TODO: optimize address arithmetic */
   assert(base.size == AGX_SIZE_16);

   enum agx_format format = format_for_bitsize(instr->def.bit_size);
   unsigned nr = instr->def.num_components;
   unsigned mask = BITFIELD_MASK(nr);

   agx_local_load_to(b, dst, base, index, format, mask);
   agx_emit_cached_split(b, dst, nr);
}

static void
agx_emit_local_store(agx_builder *b, nir_intrinsic_instr *instr)
{
   agx_index value = agx_src_index(&instr->src[0]);
   agx_index base = agx_local_base(instr->src[1]);
   agx_index index = agx_zero(); /* TODO: optimize address arithmetic */
   assert(base.size == AGX_SIZE_16);

   enum agx_format format = format_for_bitsize(nir_src_bit_size(instr->src[0]));
   unsigned mask = BITFIELD_MASK(
      nir_src_num_components(instr->src[0])); /* XXX: there's a write mask */

   agx_local_store(b, value, base, index, format, mask);
}

static void
agx_emit_load_scratch(agx_builder *b, agx_index dst, nir_intrinsic_instr *instr)
{
   agx_index offset = agx_src_index(&instr->src[0]);
   enum agx_format format = format_for_bitsize(instr->def.bit_size);
   unsigned nr = instr->def.num_components;
   unsigned mask = BITFIELD_MASK(nr);

   agx_stack_load_to(b, dst, offset, format, mask);
   agx_emit_cached_split(b, dst, nr);
   b->shader->any_scratch = true;
}

static void
agx_emit_store_scratch(agx_builder *b, nir_intrinsic_instr *instr)
{
   agx_index value = agx_recollect_vector(b, instr->src[0]);
   agx_index offset = agx_src_index(&instr->src[1]);
   enum agx_format format = format_for_bitsize(nir_src_bit_size(instr->src[0]));
   unsigned mask = BITFIELD_MASK(nir_src_num_components(instr->src[0]));

   agx_stack_store(b, value, offset, format, mask);
   b->shader->any_scratch = true;
}

static unsigned
agx_expand_tex_to(agx_builder *b, nir_def *def, agx_index src, bool masked)
{
   unsigned nr_channels = def->num_components;
   nir_component_mask_t mask = nir_def_components_read(def);

   if (!masked)
      mask = (nir_component_mask_t)BITFIELD_MASK(nr_channels);

   agx_index packed_channels[4] = {agx_null()};
   agx_index unpacked_channels[4] = {agx_null()};

   /* Hardware writes the masked components contiguously, expand out for NIR */
   agx_emit_split(b, packed_channels, src, 4 /* XXX: why not nr_channels */);

   for (unsigned i = 0; i < nr_channels; ++i) {
      unpacked_channels[i] =
         (mask & BITFIELD_BIT(i))
            ? packed_channels[util_bitcount(mask & BITFIELD_MASK(i))]
            : agx_undef(src.size);
   }

   agx_emit_collect_to(b, agx_def_index(def), nr_channels, unpacked_channels);
   return mask;
}

static agx_instr *
agx_emit_image_load(agx_builder *b, agx_index dst, nir_intrinsic_instr *intr)
{
   agx_index ms_index = agx_src_index(&intr->src[2]);
   agx_index lod = agx_src_index(&intr->src[3]);
   enum agx_lod_mode lod_mode = AGX_LOD_MODE_LOD_MIN;

   agx_index bindless = agx_immediate(0), texture;
   if (intr->intrinsic == nir_intrinsic_bindless_image_load)
      texture = agx_translate_bindless_handle(b, &intr->src[0], &bindless);
   else if (nir_src_is_const(intr->src[0]) &&
            nir_src_as_uint(intr->src[0]) < 0x100)
      texture = agx_immediate(nir_src_as_uint(intr->src[0]));
   else
      texture = agx_src_index(&intr->src[0]);

   assert(nir_src_num_components(intr->src[1]) == 4);
   agx_index coord[4] = {
      agx_extract_nir_src(b, intr->src[1], 0),
      agx_extract_nir_src(b, intr->src[1], 1),
      agx_extract_nir_src(b, intr->src[1], 2),
      agx_extract_nir_src(b, intr->src[1], 3),
   };

   /* Get the image dimension. Cubes are lowered to 2D, since they are logically
    * equivalent for imageLoad, but out-of-bounds behaviour for cubes on G13
    * is wrong according to Piglit's arb_shader_image_load_store-invalid.
    *
    * This requires a matching transform in the driver.
    */
   enum glsl_sampler_dim dim = nir_intrinsic_image_dim(intr);
   bool is_array = nir_intrinsic_image_array(intr);

   if (dim == GLSL_SAMPLER_DIM_CUBE) {
      dim = GLSL_SAMPLER_DIM_2D;
      is_array = true;
   }

   bool is_ms = dim == GLSL_SAMPLER_DIM_MS;
   unsigned coord_comps = glsl_get_sampler_dim_coordinate_components(dim);
   if (is_array && is_ms) {
      agx_index layer = agx_temp(b->shader, AGX_SIZE_16);
      agx_subdivide_to(b, layer, coord[coord_comps], 0);

      assert(ms_index.size == AGX_SIZE_16);
      agx_index tmp = agx_temp(b->shader, AGX_SIZE_32);
      agx_emit_collect_to(b, tmp, 2, (agx_index[]){ms_index, layer});
      coord[coord_comps++] = tmp;
   } else if (is_ms) {
      agx_index tmp = agx_temp(b->shader, AGX_SIZE_32);
      agx_mov_to(b, tmp, ms_index);
      coord[coord_comps++] = tmp;
   } else if (is_array) {
      coord_comps++;
   }

   /* Multisampled images do not support mipmapping */
   if (is_ms) {
      lod_mode = AGX_LOD_MODE_AUTO_LOD;
      lod = agx_zero();
   }

   agx_index coords = agx_emit_collect(b, coord_comps, coord);
   agx_index tmp = agx_vec_temp(b->shader, dst.size, 4);

   agx_instr *I = agx_image_load_to(
      b, tmp, coords, lod, bindless, texture, agx_immediate(0), agx_null(),
      agx_tex_dim(dim, is_array), lod_mode, 0, false, nir_is_coherent(intr));
   I->mask = agx_expand_tex_to(b, &intr->def, tmp, true);

   b->shader->out->uses_txf = true;
   return NULL;
}

static agx_instr *
agx_emit_export(agx_builder *b, unsigned base, nir_src src)
{
   agx_builder b_ = *b;
   agx_cursor after_cursor = agx_after_block(agx_exit_block(b->shader));
   b_.cursor = after_cursor;

   for (unsigned c = 0; c < nir_src_num_components(src); ++c) {
      agx_index chan = agx_extract_nir_src(b, src, c);
      unsigned stride = agx_size_align_16(chan.size);

      agx_export(&b_, chan, base + (c * stride));
   }

   if (agx_cursors_equal(b->cursor, after_cursor)) {
      b->cursor = agx_after_block_logical(b->cursor.block);
   }

   return NULL;
}

static agx_instr *
agx_load_exported_to(agx_builder *b, agx_index dst, unsigned base, unsigned nr)
{
   agx_index chans[4] = {0};
   unsigned stride = agx_size_align_16(dst.size);

   for (unsigned c = 0; c < nr; ++c) {
      chans[c] = agx_cached_preload(b->shader, base + c * stride, dst.size);
   }

   return agx_emit_collect_to(b, dst, nr, chans);
}

static agx_instr *
agx_emit_image_store(agx_builder *b, nir_intrinsic_instr *instr)
{
   /* See remarks in agx_emit_image_load */
   enum glsl_sampler_dim glsl_dim = nir_intrinsic_image_dim(instr);
   bool is_array = nir_intrinsic_image_array(instr);

   if (glsl_dim == GLSL_SAMPLER_DIM_CUBE) {
      glsl_dim = GLSL_SAMPLER_DIM_2D;
      is_array = true;
   }

   enum agx_dim dim = agx_tex_dim(glsl_dim, is_array);
   assert(glsl_dim != GLSL_SAMPLER_DIM_MS && "needs to be lowered");

   agx_index base, index;
   if (instr->intrinsic == nir_intrinsic_bindless_image_store) {
      index = agx_translate_bindless_handle(b, &instr->src[0], &base);

      assert(base.size == AGX_SIZE_64);
      assert(index.size == AGX_SIZE_32);
   } else {
      base = agx_zero();
      index = agx_src_index(&instr->src[0]);

      assert(index.size == AGX_SIZE_16);
   }

   agx_index coords4 = agx_src_index(&instr->src[1]);
   agx_index lod = agx_src_index(&instr->src[4]);
   assert(lod.size == AGX_SIZE_16);

   int coord_components = glsl_get_sampler_dim_coordinate_components(glsl_dim);
   if (is_array)
      coord_components++;

   agx_index coord_comps[4] = {};
   for (unsigned i = 0; i < coord_components; ++i)
      coord_comps[i] = agx_emit_extract(b, coords4, i);

   agx_index coords = agx_emit_collect(b, coord_components, coord_comps);
   agx_index data = agx_src_index(&instr->src[3]);

   /* If the image format has less than 4 components, nir_opt_shrink_stores can
    * shrink the store. But the IR still expects 4 components: pad with undef.
    */
   if (nir_src_num_components(instr->src[3]) < 4) {
      agx_index chan[4] = {agx_null()};

      for (unsigned i = 0; i < 4; ++i) {
         if (i < nir_src_num_components(instr->src[3]))
            chan[i] = agx_extract_nir_src(b, instr->src[3], i);
         else
            chan[i] = agx_undef(data.size);
      }

      data = agx_emit_collect(b, 4, chan);
   }

   /* Image stores act like tilebuffer stores when used for tib spilling */
   b->shader->out->tag_write_disable = false;

   return agx_image_write(b, data, coords, lod, base, index, dim,
                          nir_is_coherent(instr));
}

static enum agx_simd_op
translate_simd_op(nir_op op)
{
#define CASE(agx_, nir_)                                                       \
   case nir_op_##nir_:                                                         \
      return AGX_SIMD_OP_##agx_;

   switch (op) {
      CASE(AND, iand)
      CASE(FADD, fadd)
      CASE(OR, ior)
      CASE(FMUL, fmul)
      CASE(XOR, ixor)
      CASE(FMIN, fmin)
      CASE(FMAX, fmax)
      CASE(IADD, iadd)
      CASE(SMIN, imin)
      CASE(SMAX, imax)
      CASE(UMIN, umin)
      CASE(UMAX, umax)
   default:
      unreachable("unknown simd op");
   }
#undef CASE
}

static agx_instr *
agx_emit_intrinsic(agx_builder *b, nir_intrinsic_instr *instr)
{
   agx_index dst = nir_intrinsic_infos[instr->intrinsic].has_dest
                      ? agx_def_index(&instr->def)
                      : agx_null();
   gl_shader_stage stage = b->shader->stage;

   switch (instr->intrinsic) {
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_at_sample:
   case nir_intrinsic_load_barycentric_at_offset:
      /* handled later via load_vary */
      return NULL;
   case nir_intrinsic_load_interpolated_input:
      assert(stage == MESA_SHADER_FRAGMENT);
      agx_emit_load_vary(b, dst, instr);
      return NULL;

   case nir_intrinsic_load_coefficients_agx:
      assert(stage == MESA_SHADER_FRAGMENT);
      agx_ldcf_to(b, dst, cf_for_intrinsic(b, instr), 1);
      agx_emit_cached_split(b, dst, 3);
      return NULL;

   case nir_intrinsic_load_agx:
   case nir_intrinsic_load_constant_agx:
      agx_emit_load(b, dst, instr);
      return NULL;

   case nir_intrinsic_store_uvs_agx:
      assert(stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_TESS_EVAL);
      return agx_st_vary(b, agx_src_index(&instr->src[1]),
                         agx_src_index(&instr->src[0]));

   case nir_intrinsic_store_agx:
      agx_emit_store(b, instr);
      return NULL;

   case nir_intrinsic_store_shared:
      agx_emit_local_store(b, instr);
      return NULL;

   case nir_intrinsic_load_shared:
      agx_emit_local_load(b, dst, instr);
      return NULL;

   case nir_intrinsic_global_atomic_agx:
   case nir_intrinsic_global_atomic_swap_agx:
      agx_emit_atomic(b, dst, instr, false);
      return NULL;

   case nir_intrinsic_shared_atomic:
   case nir_intrinsic_shared_atomic_swap:
      agx_emit_atomic(b, dst, instr, true);
      return NULL;

   case nir_intrinsic_store_zs_agx:
      assert(stage == MESA_SHADER_FRAGMENT);
      return agx_emit_store_zs(b, instr);

   case nir_intrinsic_store_local_pixel_agx:
      return agx_emit_local_store_pixel(b, instr);

   case nir_intrinsic_load_local_pixel_agx:
      assert(stage == MESA_SHADER_FRAGMENT);
      agx_emit_local_load_pixel(b, dst, instr);
      return NULL;

   case nir_intrinsic_load_pixel_coord:
      return agx_emit_collect_to(
         b, dst, 2,
         (agx_index[2]){
            agx_get_sr(b, 16, AGX_SR_THREAD_POSITION_IN_GRID_X),
            agx_get_sr(b, 16, AGX_SR_THREAD_POSITION_IN_GRID_Y),
         });

   case nir_intrinsic_load_frag_coord_zw: {
      agx_index cf = agx_get_cf(b->shader, VARYING_SLOT_POS,
                                nir_intrinsic_component(instr));

      return agx_iter_to(b, dst, cf, agx_zero(), 1, AGX_INTERPOLATION_CENTER);
   }

   case nir_intrinsic_sample_mask_agx: {
      assert(stage == MESA_SHADER_FRAGMENT);
      b->shader->out->writes_sample_mask = true;

      /* We need to wait_pix before running Z/S tests, but we don't need to
       * wait_pix before merely discarding. Omit the wait_pix when the affected
       * samples are unconditionally killed.
       */
      bool no_tests =
         nir_src_is_const(instr->src[1]) && nir_src_as_uint(instr->src[1]) == 0;

      if (!no_tests)
         agx_wait_pix(b, 0x0001);

      return agx_sample_mask(b, agx_src_index(&instr->src[0]),
                             agx_src_index(&instr->src[1]));
   }

   case nir_intrinsic_load_back_face_agx:
      return agx_get_sr_to(b, dst, AGX_SR_BACKFACING);

   case nir_intrinsic_load_samples_log2_agx:
      return agx_get_sr_to(b, dst, AGX_SR_SAMPLES_LOG2);

   case nir_intrinsic_load_sample_mask_in:
      return agx_get_sr_to(b, dst, AGX_SR_INPUT_SAMPLE_MASK);

   case nir_intrinsic_load_sample_mask:
      return agx_get_sr_coverage_to(b, dst, AGX_SR_COVERAGE_MASK);

   case nir_intrinsic_load_helper_invocation:
      /* Compare special register to zero. We could lower this in NIR (letting
       * us fold in an inot) but meh?
       */
      return agx_icmp_to(b, dst,
                         agx_get_sr_coverage(b, 32, AGX_SR_IS_ACTIVE_THREAD),
                         agx_zero(), AGX_ICOND_UEQ, false);

   case nir_intrinsic_load_vertex_id:
      /* We don't assert the HW stage since we use this same ABI with SW VS */
      return agx_mov_to(b, dst, agx_abs(agx_vertex_id(b)));

   case nir_intrinsic_load_primitive_id:
      assert(stage == MESA_SHADER_TESS_EVAL);
      return agx_mov_to(b, dst, agx_abs(agx_vertex_id(b)));

   case nir_intrinsic_load_instance_id:
      return agx_mov_to(b, dst, agx_abs(agx_instance_id(b)));

   case nir_intrinsic_load_tess_coord_xy: {
      assert(stage == MESA_SHADER_TESS_EVAL);

      agx_index coords[] = {agx_tess_coord_x(b), agx_tess_coord_y(b)};
      return agx_emit_collect_to(b, dst, 2, coords);
   }

   case nir_intrinsic_load_preamble:
      return agx_emit_load_preamble(b, dst, instr);

   case nir_intrinsic_store_preamble:
      return agx_emit_store_preamble(b, instr);

   case nir_intrinsic_image_load:
   case nir_intrinsic_bindless_image_load:
      return agx_emit_image_load(b, dst, instr);

   case nir_intrinsic_image_store:
   case nir_intrinsic_bindless_image_store:
      return agx_emit_image_store(b, instr);

   case nir_intrinsic_image_store_block_agx:
   case nir_intrinsic_bindless_image_store_block_agx:
      return agx_emit_block_image_store(b, instr);

   case nir_intrinsic_load_workgroup_id:
      return agx_load_compute_dimension(b, dst, instr,
                                        AGX_SR_THREADGROUP_POSITION_IN_GRID_X);

   case nir_intrinsic_load_workgroup_size:
      return agx_load_compute_dimension(b, dst, instr,
                                        AGX_SR_THREADS_PER_THREADGROUP_X);

   case nir_intrinsic_load_global_invocation_id:
      return agx_load_compute_dimension(b, dst, instr,
                                        AGX_SR_THREAD_POSITION_IN_GRID_X);

   case nir_intrinsic_load_local_invocation_id:
      return agx_load_compute_dimension(
         b, dst, instr, AGX_SR_THREAD_POSITION_IN_THREADGROUP_X);

   case nir_intrinsic_load_local_invocation_index:
      return agx_get_sr_to(b, dst, AGX_SR_THREAD_INDEX_IN_THREADGROUP);

   case nir_intrinsic_load_layer_id:
      return agx_get_sr_to(b, dst, AGX_SR_THREADGROUP_POSITION_IN_GRID_Z);

   case nir_intrinsic_barrier: {
      assert(!b->shader->is_preamble && "invalid");

      bool needs_image_barriers = false;

      if (nir_intrinsic_memory_scope(instr) != SCOPE_NONE) {
         nir_variable_mode modes = nir_intrinsic_memory_modes(instr);

         if (modes & (nir_var_mem_global | nir_var_image)) {
            agx_memory_barrier(b);

            /* Pull out all the big hammers to make cross-workgroup memory
             * barriers work.
             */
            if (nir_intrinsic_memory_scope(instr) >= SCOPE_QUEUE_FAMILY) {
               agx_unknown_barrier_1(b);
               agx_memory_barrier_2(b);

               /* These are observed on G13D. At some point we should figure out
                * what the individual opcodes do.
                */
               agx_device_barrier_2(b);
               agx_unknown_barrier_2(b);
               agx_memory_barrier_3(b);
               agx_device_barrier_1(b);
            }
         }

         if (modes & nir_var_image) {
            agx_image_barrier_1(b);
            agx_image_barrier_2(b);
            needs_image_barriers = true;
         }
      }

      /* Nothing to do for subgroup barriers */
      if (nir_intrinsic_execution_scope(instr) >= SCOPE_WORKGROUP) {
         assert(gl_shader_stage_is_compute(b->shader->nir->info.stage));

         agx_threadgroup_barrier(b);
      }

      if (needs_image_barriers) {
         agx_image_barrier_3(b);
         agx_image_barrier_4(b);
      }

      return NULL;
   }

   case nir_intrinsic_fence_pbe_to_tex_agx: {
      agx_image_barrier_1(b);
      agx_image_barrier_2(b);
      agx_image_barrier_3(b);
      agx_image_barrier_4(b);
      return NULL;
   }

   case nir_intrinsic_fence_mem_to_tex_agx: {
      /* Flush out the atomic to main memory... Found experimentally... */
      agx_memory_barrier(b);
      agx_memory_barrier_2(b);

      /* TODO: Which ones do we actually need? */
      agx_image_barrier_1(b);
      agx_image_barrier_2(b);
      agx_image_barrier_3(b);
      agx_image_barrier_4(b);

      /* Flush out the texture cache */
      agx_flush_memory_to_texture(b);
      return NULL;
   }

   case nir_intrinsic_fence_pbe_to_tex_pixel_agx: {
      agx_image_barrier_1(b);
      agx_image_barrier_2(b);
      agx_flush_memory_to_texture(b);
      agx_image_barrier_3(b);
      return NULL;
   }

   case nir_intrinsic_fence_helper_exit_agx: {
      assert(b->shader->key->is_helper);
      agx_memory_barrier(b);
      agx_unknown_barrier_1(b);
      agx_memory_barrier_2(b);
      agx_unknown_barrier_2(b);
      agx_memory_barrier_3(b);
      return NULL;
   }

   case nir_intrinsic_begin_invocation_interlock: {
      if (!b->shader->did_writeout &&
          !b->shader->key->fs.ignore_tib_dependencies)
         agx_wait_pix(b, 0x000C);

      b->shader->did_writeout = true;
      return NULL;
   }

   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddx_fine:
      return agx_dfdx_to(b, dst, agx_src_index(&instr->src[0]));

   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_coarse:
   case nir_intrinsic_ddy_fine:
      return agx_dfdy_to(b, dst, agx_src_index(&instr->src[0]));

   case nir_intrinsic_load_subgroup_invocation:
      return agx_get_sr_to(b, dst, AGX_SR_THREAD_INDEX_IN_SUBGROUP);

   case nir_intrinsic_load_subgroup_id:
      return agx_get_sr_to(b, dst, AGX_SR_SUBGROUP_INDEX_IN_THREADGROUP);

   case nir_intrinsic_load_active_subgroup_invocation_agx:
      return agx_get_sr_coverage_to(b, dst,
                                    AGX_SR_ACTIVE_THREAD_INDEX_IN_SUBGROUP);

   case nir_intrinsic_load_active_subgroup_count_agx:
      return agx_get_sr_coverage_to(b, dst,
                                    AGX_SR_TOTAL_ACTIVE_THREADS_IN_SUBGROUP);

   case nir_intrinsic_reduce: {
      assert((instr->def.bit_size == 1 || instr->def.bit_size == 16 ||
              instr->def.bit_size == 32) &&
             "should've been lowered");

      unsigned cluster_size = nir_intrinsic_cluster_size(instr);
      assert(cluster_size == 0 || cluster_size == 4 || cluster_size >= 32);

      enum agx_simd_op op =
         translate_simd_op(nir_intrinsic_reduction_op(instr));

      agx_index src0 = agx_src_index(&instr->src[0]);

      if (cluster_size == 4)
         return agx_quad_reduce_to(b, dst, src0, op);
      else
         return agx_simd_reduce_to(b, dst, src0, op);
   }

   case nir_intrinsic_exclusive_scan: {
      assert((instr->def.bit_size == 1 || instr->def.bit_size == 16 ||
              instr->def.bit_size == 32) &&
             "should've been lowered");

      return agx_simd_prefix_to(
         b, dst, agx_src_index(&instr->src[0]),
         translate_simd_op(nir_intrinsic_reduction_op(instr)));
   }

   case nir_intrinsic_read_invocation: {
      /* TODO: Check if we're actually inside divergent control flow */
      b->shader->any_quad_divergent_shuffle |= b->shader->any_cf;

      /* Lane ID guaranteed to be uniform */
      return agx_shuffle_to(b, dst, agx_src_index(&instr->src[0]),
                            agx_src_index(&instr->src[1]));
   }

   case nir_intrinsic_quad_broadcast: {
      /* TODO: Check if we're actually inside divergent control flow */
      b->shader->any_quad_divergent_shuffle |= b->shader->any_cf;

      /* Lane ID guaranteed to be uniform */
      return agx_quad_shuffle_to(b, dst, agx_src_index(&instr->src[0]),
                                 agx_src_index(&instr->src[1]));
   }

   case nir_intrinsic_quad_swap_horizontal: {
      return agx_quad_shuffle_xor_to(b, dst, agx_src_index(&instr->src[0]),
                                     agx_immediate(1));
   }

   case nir_intrinsic_quad_swap_vertical: {
      return agx_quad_shuffle_xor_to(b, dst, agx_src_index(&instr->src[0]),
                                     agx_immediate(2));
   }

   case nir_intrinsic_quad_swap_diagonal: {
      return agx_quad_shuffle_xor_to(b, dst, agx_src_index(&instr->src[0]),
                                     agx_immediate(3));
   }

   case nir_intrinsic_ballot: {
      return agx_ballot_to(b, dst, agx_src_index(&instr->src[0]));
   }

   case nir_intrinsic_quad_ballot_agx: {
      return agx_quad_ballot_to(b, dst, agx_src_index(&instr->src[0]));
   }

   case nir_intrinsic_doorbell_agx: {
      return agx_doorbell(b, nir_src_as_uint(instr->src[0]));
   }

   case nir_intrinsic_stack_map_agx: {
      return agx_stack_map(b, agx_src_index(&instr->src[1]),
                           nir_src_as_uint(instr->src[0]));
   }

   case nir_intrinsic_stack_unmap_agx: {
      return agx_stack_unmap_to(b, dst, nir_src_as_uint(instr->src[0]));
   }

   case nir_intrinsic_load_scratch:
      agx_emit_load_scratch(b, dst, instr);
      return NULL;

   case nir_intrinsic_store_scratch:
      agx_emit_store_scratch(b, instr);
      return NULL;

   case nir_intrinsic_load_core_id_agx:
      return agx_get_sr_to(b, dst, AGX_SR_CORE_ID);

   case nir_intrinsic_load_helper_op_id_agx:
      assert(b->shader->key->is_helper);
      return agx_get_sr_barrier_to(b, dst, AGX_SR_HELPER_OP);

   case nir_intrinsic_load_helper_arg_lo_agx:
      assert(b->shader->key->is_helper);
      return agx_get_sr_barrier_to(b, dst, AGX_SR_HELPER_ARG_L);

   case nir_intrinsic_load_helper_arg_hi_agx:
      assert(b->shader->key->is_helper);
      return agx_get_sr_barrier_to(b, dst, AGX_SR_HELPER_ARG_H);

   case nir_intrinsic_load_exported_agx:
      return agx_load_exported_to(b, dst, nir_intrinsic_base(instr),
                                  instr->def.num_components);

   case nir_intrinsic_export_agx:
      return agx_emit_export(b, nir_intrinsic_base(instr), instr->src[0]);

   case nir_intrinsic_load_barycentric_sample:
   case nir_intrinsic_load_sample_id:
   case nir_intrinsic_load_sample_pos:
      unreachable("Sample shading should have been lowered");

   default:
      fprintf(stderr, "Unhandled intrinsic %s\n",
              nir_intrinsic_infos[instr->intrinsic].name);
      unreachable("Unhandled intrinsic");
   }
}

static agx_index
agx_alu_src_index(agx_builder *b, nir_alu_src src)
{
   /* Check well-formedness of the input NIR */
   ASSERTED unsigned bitsize = nir_src_bit_size(src.src);
   unsigned comps = nir_src_num_components(src.src);
   unsigned channel = src.swizzle[0];

   assert(bitsize == 1 || bitsize == 8 || bitsize == 16 || bitsize == 32 ||
          bitsize == 64);
   assert(channel < comps);

   return agx_extract_nir_src(b, src.src, channel);
}

/*
 * Emit an instruction translating (s0 * s1) + (s2 << s3). Assuming s3 is
 * constant, this is an imad instruction. If s1 == 1, then this is optimized to
 * an iadd instruction, which is faster.
 */
static agx_instr *
agx_emit_imadshl_agx(agx_builder *b, nir_alu_instr *alu, agx_index dst,
                     agx_index s0, agx_index s1, agx_index s2, agx_index s3)
{
   /* If the shift is not constant, use a variable shift. This should never
    * happen in practice but we don't want to constrain the NIR.
    */
   unsigned shift;
   if (!nir_src_is_const(alu->src[3].src)) {
      s2 = agx_bfi(b, agx_immediate(0), s2, s3, 0);
      shift = 0;
   } else {
      shift = nir_alu_src_as_uint(alu->src[3]);
   }

   assert(shift <= 4 && "domain restriction on the input NIR");

   /* Emit iadd if possible, else imad */
   if (nir_src_is_const(alu->src[1].src) &&
       nir_alu_src_as_uint(alu->src[1]) == 1) {

      return agx_iadd_to(b, dst, s0, s2, shift);
   } else {
      return agx_imad_to(b, dst, s0, s1, s2, shift);
   }
}

static bool
is_conversion_to_8bit(nir_op op)
{
   switch (op) {
   case nir_op_i2i8:
   case nir_op_u2u8:
   case nir_op_f2i8:
   case nir_op_f2u8:
   case nir_op_b2i8:
      return true;
   default:
      return false;
   }
}

static agx_instr *
agx_fminmax_to(agx_builder *b, agx_index dst, agx_index s0, agx_index s1,
               nir_alu_instr *alu)
{
   /* The hardware gtn/ltn modes are unfortunately incorrect for signed zeros */
   assert(!nir_alu_instr_is_signed_zero_preserve(alu) &&
          "should've been lowered");

   bool fmax = alu->op == nir_op_fmax;
   enum agx_fcond fcond = fmax ? AGX_FCOND_GTN : AGX_FCOND_LTN;

   /* Calculate min/max with the appropriate hardware instruction */
   agx_index tmp = agx_fcmpsel(b, s0, s1, s0, s1, fcond);

   /* G13 flushes fp32 denorms and preserves fp16 denorms. Since cmpsel
    * preserves denorms, we need to canonicalize for fp32. Canonicalizing fp16
    * would be harmless but wastes an instruction.
    */
   if (alu->def.bit_size == 32)
      return agx_fadd_to(b, dst, tmp, agx_negzero());
   else
      return agx_mov_to(b, dst, tmp);
}

static agx_instr *
agx_emit_alu(agx_builder *b, nir_alu_instr *instr)
{
   unsigned srcs = nir_op_infos[instr->op].num_inputs;
   unsigned sz = instr->def.bit_size;
   unsigned src_sz = srcs ? nir_src_bit_size(instr->src[0].src) : 0;
   ASSERTED unsigned comps = instr->def.num_components;

   assert(comps == 1 || nir_op_is_vec_or_mov(instr->op));
   assert(sz == 1 ||
          ((nir_op_is_vec_or_mov(instr->op) ||
            is_conversion_to_8bit(instr->op) || instr->op == nir_op_bcsel) &&
           sz == 8) ||
          sz == 16 || sz == 32 || sz == 64);

   agx_index dst = agx_def_index(&instr->def);
   agx_index s0 = srcs > 0 ? agx_alu_src_index(b, instr->src[0]) : agx_null();
   agx_index s1 = srcs > 1 ? agx_alu_src_index(b, instr->src[1]) : agx_null();
   agx_index s2 = srcs > 2 ? agx_alu_src_index(b, instr->src[2]) : agx_null();
   agx_index s3 = srcs > 3 ? agx_alu_src_index(b, instr->src[3]) : agx_null();

   agx_index i0 = agx_immediate(0);
   agx_index i1 = agx_immediate(1);

#define UNOP(nop, aop)                                                         \
   case nir_op_##nop:                                                          \
      return agx_##aop##_to(b, dst, s0);
#define BINOP(nop, aop)                                                        \
   case nir_op_##nop:                                                          \
      return agx_##aop##_to(b, dst, s0, s1);
#define TRIOP(nop, aop)                                                        \
   case nir_op_##nop:                                                          \
      return agx_##aop##_to(b, dst, s0, s1, s2);

   switch (instr->op) {
      BINOP(fadd, fadd);
      BINOP(fmul, fmul);
      TRIOP(ffma, fma);

      UNOP(f2f16, fmov);
      UNOP(f2f16_rtne, fmov);
      UNOP(f2f32, fmov);
      UNOP(fround_even, roundeven);
      UNOP(ftrunc, trunc);
      UNOP(ffloor, floor);
      UNOP(fceil, ceil);
      UNOP(frcp, rcp);
      UNOP(frsq, rsqrt);
      UNOP(flog2, log2);
      UNOP(fexp2, exp2);

      UNOP(mov, mov);
      UNOP(u2u32, mov);
      UNOP(bitfield_reverse, bitrev);
      UNOP(bit_count, popcount);
      UNOP(ufind_msb, ffs);
      BINOP(iand, and);
      BINOP(ior, or);
      BINOP(ixor, xor);
      BINOP(interleave_agx, intl);

   case nir_op_feq:
      return agx_fcmp_to(b, dst, s0, s1, AGX_FCOND_EQ, false);
   case nir_op_flt:
      return agx_fcmp_to(b, dst, s0, s1, AGX_FCOND_LT, false);
   case nir_op_fge:
      return agx_fcmp_to(b, dst, s0, s1, AGX_FCOND_GE, false);
   case nir_op_fneu:
      return agx_fcmp_to(b, dst, s0, s1, AGX_FCOND_EQ, true);

   case nir_op_ieq:
      return agx_icmp_to(b, dst, s0, s1, AGX_ICOND_UEQ, false);
   case nir_op_ine:
      return agx_icmp_to(b, dst, s0, s1, AGX_ICOND_UEQ, true);
   case nir_op_ilt:
      return agx_icmp_to(b, dst, s0, s1, AGX_ICOND_SLT, false);
   case nir_op_ige:
      return agx_icmp_to(b, dst, s0, s1, AGX_ICOND_SLT, true);
   case nir_op_ult:
      return agx_icmp_to(b, dst, s0, s1, AGX_ICOND_ULT, false);
   case nir_op_uge:
      return agx_icmp_to(b, dst, s0, s1, AGX_ICOND_ULT, true);

   case nir_op_inot:
      if (sz == 1)
         return agx_xor_to(b, dst, s0, i1);
      else
         return agx_not_to(b, dst, s0);

   case nir_op_b2b1:
      return agx_icmp_to(b, dst, s0, i0, AGX_ICOND_UEQ, true);

   case nir_op_fsqrt:
      return agx_fmul_to(b, dst, s0, agx_srsqrt(b, s0));
   case nir_op_fabs:
      return agx_fmov_to(b, dst, agx_abs(s0));
   case nir_op_fneg:
      return agx_fmov_to(b, dst, agx_neg(s0));

   case nir_op_fmin:
   case nir_op_fmax:
      return agx_fminmax_to(b, dst, s0, s1, instr);

   case nir_op_imin:
      return agx_icmpsel_to(b, dst, s0, s1, s0, s1, AGX_ICOND_SLT);
   case nir_op_imax:
      return agx_icmpsel_to(b, dst, s0, s1, s0, s1, AGX_ICOND_SGT);
   case nir_op_umin:
      return agx_icmpsel_to(b, dst, s0, s1, s0, s1, AGX_ICOND_ULT);
   case nir_op_umax:
      return agx_icmpsel_to(b, dst, s0, s1, s0, s1, AGX_ICOND_UGT);
   case nir_op_bounds_agx:
      /* end offset > bound ? 0 : data */
      return agx_icmpsel_to(b, dst, s1, s2, agx_zero(), s0, AGX_ICOND_UGT);

   case nir_op_iadd:
      return agx_iadd_to(b, dst, s0, s1, 0);
   case nir_op_imadshl_agx:
      return agx_emit_imadshl_agx(b, instr, dst, s0, s1, s2, s3);
   case nir_op_imsubshl_agx:
      return agx_emit_imadshl_agx(b, instr, dst, s0, s1, agx_neg(s2), s3);
   case nir_op_isub:
      return agx_iadd_to(b, dst, s0, agx_neg(s1), 0);
   case nir_op_ineg:
      return agx_iadd_to(b, dst, i0, agx_neg(s0), 0);
   case nir_op_imul:
      return agx_imad_to(b, dst, s0, s1, i0, 0);
   case nir_op_umul_2x32_64:
      return agx_imad_to(b, dst, agx_abs(s0), agx_abs(s1), i0, 0);
   case nir_op_imul_2x32_64:
      return agx_imad_to(b, dst, s0, s1, i0, 0);
   case nir_op_umul_high:
      return agx_mul_high_to(b, dst, s0, s1, false);
   case nir_op_imul_high:
      return agx_mul_high_to(b, dst, s0, s1, true);

   case nir_op_ishl:
      return agx_bfi_to(b, dst, i0, s0, s1, 0);
   case nir_op_ushr:
      return agx_ushr_to(b, dst, s0, s1);
   case nir_op_ishr:
      return agx_asr_to(b, dst, s0, s1);

   case nir_op_extr_agx:
      return agx_extr_to(b, dst, s0, s1, s2,
                         nir_alu_src_as_uint(instr->src[3]));

   case nir_op_ubitfield_extract: {
      unsigned m = nir_alu_src_as_uint(instr->src[2]);
      assert(m != 0 && "should've been optimized");

      /* Disable masking if the whole thing is used */
      if (m >= 32)
         m = 0;

      return agx_bfeil_to(b, dst, i0, s0, s1, m);
   }

   case nir_op_bcsel:
      return agx_icmpsel_to(b, dst, s0, i0, s2, s1, AGX_ICOND_UEQ);

   case nir_op_i2i32: {
      if (src_sz == 8) {
         /* Sign extend in software, NIR likes 8-bit conversions */
         agx_index ishl16 = agx_bfi(b, i0, s0, agx_immediate(8), 0);
         return agx_asr_to(b, dst, ishl16, agx_immediate(8));
      } else {
         assert(s0.size == AGX_SIZE_16 && "other conversions lowered");
         return agx_iadd_to(b, dst, s0, i0, 0);
      }
   }

   case nir_op_i2i16: {
      if (src_sz == 8) {
         /* Sign extend in software, NIR likes 8-bit conversions */
         agx_index ishl16 = agx_bfi(b, i0, s0, agx_immediate(8), 0);
         return agx_asr_to(b, dst, ishl16, agx_immediate(8));
      } else {
         assert(s0.size == AGX_SIZE_32 && "other conversions lowered");
         return agx_subdivide_to(b, dst, s0, 0);
      }
   }

   case nir_op_u2u16: {
      if (s0.size == AGX_SIZE_32)
         return agx_subdivide_to(b, dst, s0, 0);
      else
         return agx_mov_to(b, dst, s0);
   }

   /* It will be put into a 16-bit register, but zero out the garbage. We could
    * optimize this in the future but it ensures correctness for u2u16(u2u8(x))
    * sequences.
    */
   case nir_op_u2u8:
   case nir_op_i2i8:
      return agx_and_to(b, dst, s0, agx_immediate(0xFF));

   case nir_op_iadd_sat: {
      agx_instr *I = agx_iadd_to(b, dst, s0, s1, 0);
      I->saturate = true;
      return I;
   }

   case nir_op_isub_sat: {
      agx_instr *I = agx_iadd_to(b, dst, s0, agx_neg(s1), 0);
      I->saturate = true;
      return I;
   }

   case nir_op_uadd_sat: {
      agx_instr *I = agx_iadd_to(b, dst, agx_abs(s0), agx_abs(s1), 0);
      I->saturate = true;
      return I;
   }

   case nir_op_usub_sat: {
      agx_instr *I = agx_iadd_to(b, dst, agx_abs(s0), agx_neg(agx_abs(s1)), 0);
      I->saturate = true;
      return I;
   }

   case nir_op_fsat: {
      agx_instr *I = agx_fadd_to(b, dst, s0, agx_negzero());
      I->saturate = true;
      return I;
   }

   case nir_op_fsin_agx: {
      agx_index fixup = agx_sin_pt_1(b, s0);
      agx_index sinc = agx_sin_pt_2(b, fixup);
      return agx_fmul_to(b, dst, sinc, fixup);
   }

   case nir_op_f2i16:
      return agx_convert_to(b, dst, agx_immediate(AGX_CONVERT_F_TO_S16), s0,
                            AGX_ROUND_RTZ);

   case nir_op_f2i32:
      return agx_convert_to(b, dst, agx_immediate(AGX_CONVERT_F_TO_S32), s0,
                            AGX_ROUND_RTZ);

   case nir_op_f2u16:
      return agx_convert_to(b, dst, agx_immediate(AGX_CONVERT_F_TO_U16), s0,
                            AGX_ROUND_RTZ);

   case nir_op_f2u32:
      return agx_convert_to(b, dst, agx_immediate(AGX_CONVERT_F_TO_U32), s0,
                            AGX_ROUND_RTZ);

   case nir_op_u2f16:
   case nir_op_u2f32: {
      if (src_sz == 64)
         unreachable("64-bit conversions unimplemented");

      enum agx_convert mode = (src_sz == 32)   ? AGX_CONVERT_U32_TO_F
                              : (src_sz == 16) ? AGX_CONVERT_U16_TO_F
                                               : AGX_CONVERT_U8_TO_F;

      return agx_convert_to(b, dst, agx_immediate(mode), s0, AGX_ROUND_RTE);
   }

   case nir_op_i2f16:
   case nir_op_i2f32: {
      if (src_sz == 64)
         unreachable("64-bit conversions unimplemented");

      enum agx_convert mode = (src_sz == 32)   ? AGX_CONVERT_S32_TO_F
                              : (src_sz == 16) ? AGX_CONVERT_S16_TO_F
                                               : AGX_CONVERT_S8_TO_F;

      return agx_convert_to(b, dst, agx_immediate(mode), s0, AGX_ROUND_RTE);
   }

   case nir_op_pack_32_2x16_split:
   case nir_op_pack_64_2x32_split: {
      agx_index idx[] = {s0, s1};
      return agx_emit_collect_to(b, dst, 2, idx);
   }

   case nir_op_unpack_64_2x32_split_x:
   case nir_op_unpack_32_2x16_split_x:
      return agx_subdivide_to(b, dst, s0, 0);

   case nir_op_unpack_64_2x32_split_y:
   case nir_op_unpack_32_2x16_split_y:
      return agx_subdivide_to(b, dst, s0, 1);

   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4: {
      agx_index idx[] = {s0, s1, s2, s3};
      return agx_emit_collect_to(b, dst, srcs, idx);
   }

   case nir_op_vec8:
   case nir_op_vec16:
      unreachable("should've been lowered");

   default:
      fprintf(stderr, "Unhandled ALU op %s\n", nir_op_infos[instr->op].name);
      unreachable("Unhandled ALU instruction");
   }
}

static enum agx_lod_mode
agx_lod_mode_for_nir(nir_texop op, bool biased, bool min_lod, bool lod_is_zero)
{
   switch (op) {
   case nir_texop_tex:
   case nir_texop_tg4:
      /* We could support this for tex, but it's never actually seen because tex
       * is always turned into txb to implement sampler LOD bias in Vulkan.
       */
      assert(!min_lod && "unimplemented");

      return AGX_LOD_MODE_AUTO_LOD;
   case nir_texop_txb:
      return min_lod ? AGX_LOD_MODE_AUTO_LOD_BIAS_MIN
                     : AGX_LOD_MODE_AUTO_LOD_BIAS;
   case nir_texop_lod:
      assert(!min_lod);
      return biased ? AGX_LOD_MODE_AUTO_LOD_BIAS : AGX_LOD_MODE_AUTO_LOD;
   case nir_texop_txd:
      return min_lod ? AGX_LOD_MODE_LOD_GRAD_MIN : AGX_LOD_MODE_LOD_GRAD;
   case nir_texop_txl:
      assert(!min_lod);
      return AGX_LOD_MODE_LOD_MIN;
   case nir_texop_txf:
      assert(!min_lod);
      return lod_is_zero ? AGX_LOD_MODE_AUTO_LOD : AGX_LOD_MODE_LOD_MIN;
   case nir_texop_txf_ms:
      assert(!min_lod);
      assert(lod_is_zero && "no mipmapping");
      return AGX_LOD_MODE_AUTO_LOD;
   default:
      unreachable("Unhandled texture op");
   }
}

static enum agx_gather
agx_gather_for_nir(nir_tex_instr *tex)
{
   if (tex->op == nir_texop_tg4) {
      enum agx_gather components[] = {
         AGX_GATHER_R,
         AGX_GATHER_G,
         AGX_GATHER_B,
         AGX_GATHER_A,
      };

      assert(tex->component < ARRAY_SIZE(components));
      return components[tex->component];
   } else {
      return AGX_GATHER_NONE;
   }
}

static void
agx_emit_tex(agx_builder *b, nir_tex_instr *instr)
{
   agx_index coords = agx_null(), bindless = agx_immediate(0),
             texture = agx_immediate(instr->texture_index),
             sampler = agx_immediate(0), lod = agx_immediate(0),
             compare = agx_null(), packed_offset = agx_null(),
             min_lod = agx_null();

   bool lod_is_zero = true;

   for (unsigned i = 0; i < instr->num_srcs; ++i) {
      agx_index index = agx_src_index(&instr->src[i].src);

      switch (instr->src[i].src_type) {
      case nir_tex_src_backend1:
         coords = index;
         break;

      case nir_tex_src_backend2:
         packed_offset = index;
         break;

      case nir_tex_src_lod:
      case nir_tex_src_bias:
         lod = index;
         lod_is_zero = nir_src_is_const(instr->src[i].src) &&
                       nir_src_as_uint(instr->src[i].src) == 0;
         break;

      case nir_tex_src_min_lod:
         assert(index.size == AGX_SIZE_16);
         min_lod = index;
         break;

      case nir_tex_src_comparator:
         assert(index.size == AGX_SIZE_32);
         compare = index;
         break;

      case nir_tex_src_texture_offset:
         texture = index;
         break;
      case nir_tex_src_sampler_handle:
         sampler = index;
         break;

      case nir_tex_src_texture_handle:
         texture =
            agx_translate_bindless_handle(b, &instr->src[i].src, &bindless);
         break;

      case nir_tex_src_ddx: {
         int y_idx = nir_tex_instr_src_index(instr, nir_tex_src_ddy);
         assert(y_idx >= 0 && "we only handle gradients");

         int min_idx = nir_tex_instr_src_index(instr, nir_tex_src_min_lod);
         bool has_min = min_idx >= 0;
         agx_index min;

         unsigned n = nir_tex_instr_src_size(instr, y_idx);
         assert((n == 2 || n == 3) && "other sizes not supported");

         agx_index index2 = agx_src_index(&instr->src[y_idx].src);

         if (has_min) {
            min = agx_src_index(&instr->src[min_idx].src);

            /* Undef extend to 32-bit since our IR is iffy */
            min = agx_pad_to_32(b, min);
         }

         /* We explicitly don't cache about the split cache for this */
         unsigned chans = (2 * n) + (has_min ? 1 : 0);
         lod = agx_vec_temp(b->shader, AGX_SIZE_32, chans);
         agx_instr *I = agx_collect_to(b, lod, chans);

         for (unsigned i = 0; i < n; ++i) {
            I->src[(2 * i) + 0] = agx_emit_extract(b, index, i);
            I->src[(2 * i) + 1] = agx_emit_extract(b, index2, i);
         }

         if (has_min)
            I->src[2 * n] = min;

         break;
      }

      case nir_tex_src_ddy:
         /* handled above */
         break;

      default:
         unreachable("Unexpected texture source");
      }
   }

   enum agx_lod_mode lod_mode = agx_lod_mode_for_nir(
      instr->op, nir_tex_instr_src_index(instr, nir_tex_src_bias) >= 0,
      nir_tex_instr_src_index(instr, nir_tex_src_min_lod) >= 0, lod_is_zero);

   if (lod_mode == AGX_LOD_MODE_AUTO_LOD) {
      /* Ignored logically but asserted 0 */
      lod = agx_immediate(0);
   } else if (lod_mode == AGX_LOD_MODE_AUTO_LOD_BIAS_MIN) {
      /* Combine min with lod */
      lod = agx_vec2(b, lod, min_lod);
   }

   agx_index dst = agx_def_index(&instr->def);

   /* Pack shadow reference value (compare) and packed offset together */
   agx_index compare_offset = agx_null();

   if (!agx_is_null(compare) && !agx_is_null(packed_offset))
      compare_offset = agx_vec2(b, compare, packed_offset);
   else if (!agx_is_null(packed_offset))
      compare_offset = packed_offset;
   else if (!agx_is_null(compare))
      compare_offset = compare;

   agx_index tmp = agx_vec_temp(b->shader, dst.size, 4);
   agx_instr *I = agx_texture_sample_to(
      b, tmp, coords, lod, bindless, texture, sampler, compare_offset,
      agx_tex_dim(instr->sampler_dim, instr->is_array), lod_mode, 0,
      !agx_is_null(packed_offset), !agx_is_null(compare),
      instr->op == nir_texop_lod, agx_gather_for_nir(instr));

   if (instr->op == nir_texop_txf || instr->op == nir_texop_txf_ms) {
      I->op = AGX_OPCODE_TEXTURE_LOAD;
      b->shader->out->uses_txf = true;
   }

   /* Destination masking doesn't seem to work properly for gathers (because
    * it's mostly pointless), but it does show up in the lowering of
    * textureGatherOffsets. Don't try to mask the destination for gathers.
    */
   bool masked = (instr->op != nir_texop_tg4);
   I->mask = agx_expand_tex_to(b, &instr->def, tmp, masked);
}

/*
 * Determine if a NIR loop (CF list) uses a continue jump, including within
 * if-else statements but not including nested loops.
 */
static bool
cf_list_uses_continue(struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      if (node->type == nir_cf_node_block) {
         nir_block *block = nir_cf_node_as_block(node);

         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_jump &&
                nir_instr_as_jump(instr)->type == nir_jump_continue)
               return true;
         }
      } else if (node->type == nir_cf_node_if) {
         nir_if *nif = nir_cf_node_as_if(node);

         if (cf_list_uses_continue(&nif->then_list) ||
             cf_list_uses_continue(&nif->else_list))
            return true;
      } else {
         assert(node->type == nir_cf_node_loop && "don't care about nesting");
      }
   }

   return false;
}

static bool
loop_uses_continue(nir_loop *loop)
{
   return cf_list_uses_continue(&loop->body);
}

/*
 * NIR loops are treated as a pair of AGX loops:
 *
 *    do {
 *       do {
 *          ...
 *       } while (0);
 *    } while (cond);
 *
 * By manipulating the nesting counter, we may break out of nested loops, so
 * under the model, both break and continue may be implemented as breaks, where
 * break breaks out of the outer loop (2 layers) and continue breaks out of the
 * inner loop (1 layer).
 *
 * After manipulating the nesting counter directly, pop_exec #0 must be used to
 * flush the update to the execution mask.
 */
static void
agx_emit_jump(agx_builder *b, nir_jump_instr *instr)
{
   agx_context *ctx = b->shader;
   assert(instr->type == nir_jump_break || instr->type == nir_jump_continue);

   /* Break out of either one or two loops */
   unsigned nestings = b->shader->loop_nesting;

   if (instr->type == nir_jump_continue) {
      nestings += 1;
      agx_block_add_successor(ctx->current_block, ctx->continue_block);
   } else if (instr->type == nir_jump_break) {
      nestings += ctx->loop_continues ? 2 : 1;
      agx_block_add_successor(ctx->current_block, ctx->break_block);
   }

   agx_break(b, nestings, ctx->break_block);
   ctx->current_block->unconditional_jumps = true;
}

static void
agx_emit_phi(agx_builder *b, nir_phi_instr *instr)
{
   agx_instr *I =
      agx_phi_to(b, agx_def_index(&instr->def), exec_list_length(&instr->srcs));

   /* Deferred */
   I->phi = instr;
}

/* Look up the AGX block corresponding to a given NIR block. Used when
 * translating phi nodes after emitting all blocks.
 */
static agx_block *
agx_from_nir_block(agx_context *ctx, nir_block *block)
{
   return ctx->indexed_nir_blocks[block->index];
}

static void
agx_emit_phi_deferred(agx_context *ctx, agx_block *block, agx_instr *I)
{
   nir_phi_instr *phi = I->phi;
   I->phi = NULL;

   /* Guaranteed by lower_phis_to_scalar */
   assert(phi->def.num_components == 1);

   nir_foreach_phi_src(src, phi) {
      agx_block *pred = agx_from_nir_block(ctx, src->pred);
      unsigned i = agx_predecessor_index(block, pred);
      assert(i < I->nr_srcs);

      I->src[i] = agx_src_index(&src->src);
   }
}

static void
agx_emit_phis_deferred(agx_context *ctx)
{
   agx_foreach_block(ctx, block) {
      agx_foreach_phi_in_block(block, I)
         agx_emit_phi_deferred(ctx, block, I);
   }
}

static void
agx_emit_undef(agx_builder *b, nir_undef_instr *instr)
{
   /* For now, just lower undefs to zero. This doesn't matter too much, since
    * the lowering happens in NIR and this just allows for late lowering passes
    * to result in undefs.
    */
   if (instr->def.num_components > 1) {
      assert(instr->def.num_components <= 4);
      agx_index zero = agx_mov_imm(b, instr->def.bit_size, 0);

      agx_emit_collect_to(b, agx_def_index(&instr->def),
                          instr->def.num_components,
                          (agx_index[4]){zero, zero, zero, zero});
   } else {
      agx_mov_imm_to(b, agx_def_index(&instr->def), 0);
   }
}

static void
agx_emit_instr(agx_builder *b, struct nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_load_const:
      agx_emit_load_const(b, nir_instr_as_load_const(instr));
      break;

   case nir_instr_type_intrinsic:
      agx_emit_intrinsic(b, nir_instr_as_intrinsic(instr));
      break;

   case nir_instr_type_alu:
      agx_emit_alu(b, nir_instr_as_alu(instr));
      break;

   case nir_instr_type_tex:
      agx_emit_tex(b, nir_instr_as_tex(instr));
      break;

   case nir_instr_type_jump:
      agx_emit_jump(b, nir_instr_as_jump(instr));
      break;

   case nir_instr_type_phi:
      agx_emit_phi(b, nir_instr_as_phi(instr));
      break;

   case nir_instr_type_undef:
      agx_emit_undef(b, nir_instr_as_undef(instr));
      break;

   default:
      unreachable("should've been lowered");
   }
}

static agx_block *
agx_create_block(agx_context *ctx)
{
   agx_block *blk = rzalloc(ctx, agx_block);

   util_dynarray_init(&blk->predecessors, blk);

   return blk;
}

static agx_block *
emit_block(agx_context *ctx, nir_block *block)
{
   if (ctx->after_block) {
      ctx->current_block = ctx->after_block;
      ctx->after_block = NULL;
   } else {
      ctx->current_block = agx_create_block(ctx);
   }

   agx_block *blk = ctx->current_block;
   list_addtail(&blk->link, &ctx->blocks);
   list_inithead(&blk->instructions);

   ctx->indexed_nir_blocks[block->index] = blk;

   agx_builder _b = agx_init_builder(ctx, agx_after_block(blk));

   nir_foreach_instr(instr, block) {
      agx_emit_instr(&_b, instr);
   }

   return blk;
}

static agx_block *emit_cf_list(agx_context *ctx, struct exec_list *list);

/* Emit if-else as
 *
 *    if_icmp cond != 0
 *       ...
 *    else_icmp cond == 0
 *       ...
 *    pop_exec
 *
 * If the else is empty, we can omit the else_icmp. This happens elsewhere, as
 * an empty else block can become nonempty after RA due to phi lowering. This is
 * not usually optimal, but it's a start.
 */

static void
emit_if(agx_context *ctx, nir_if *nif)
{
   agx_block *first_block = ctx->current_block;
   agx_builder _b = agx_init_builder(ctx, agx_after_block(first_block));
   agx_index cond = agx_src_index(&nif->condition);

   agx_instr *if_ = agx_if_icmp(&_b, cond, agx_zero(), 1, AGX_ICOND_UEQ, true,
                                NULL /* filled in later */);
   ctx->loop_nesting++;
   ctx->total_nesting++;

   /* Emit the two subblocks. */
   agx_block *if_block = emit_cf_list(ctx, &nif->then_list);
   agx_block *end_then = ctx->current_block;

   _b.cursor = agx_after_block(ctx->current_block);

   agx_block *else_block = emit_cf_list(ctx, &nif->else_list);
   agx_block *end_else = ctx->current_block;

   /* If the "if" fails, we fallthrough to the else */
   if_->target = else_block;

   /* Insert an else instruction at the beginning of the else block. We use
    * "else_fcmp 0.0, 0.0, eq" as unconditional else, matching the blob.
    *
    * If it fails, we fall through to the logical end of the last else block.
    */
   _b.cursor = agx_before_block(else_block);
   agx_else_fcmp(&_b, agx_zero(), agx_zero(), 1, AGX_FCOND_EQ, false, end_else);

   ctx->after_block = agx_create_block(ctx);

   agx_block_add_successor(first_block, if_block);
   agx_block_add_successor(first_block, else_block);
   agx_block_add_successor(end_then, ctx->after_block);
   agx_block_add_successor(end_else, ctx->after_block);

   _b.cursor = agx_after_block(ctx->current_block);
   agx_pop_exec(&_b, 1);
   ctx->loop_nesting--;
   ctx->total_nesting--;
}

static void
emit_loop(agx_context *ctx, nir_loop *nloop)
{
   assert(!nir_loop_has_continue_construct(nloop));
   /* We only track nesting within the innermost loop, so push and reset */
   unsigned pushed_nesting = ctx->loop_nesting;
   ctx->loop_nesting = 0;
   ctx->total_nesting++;

   bool old_continues = ctx->loop_continues;
   ctx->loop_continues = loop_uses_continue(nloop);

   agx_block *popped_break = ctx->break_block;
   agx_block *popped_continue = ctx->continue_block;

   ctx->break_block = agx_create_block(ctx);
   ctx->continue_block = agx_create_block(ctx);

   /* If we are emitting a loop inside other control flow, there might be
    * threads masked off (TODO: divergence analysis), so push_exec them so
    * we get the lower nesting count values to ourselves.
    */
   agx_builder _b = agx_init_builder(ctx, agx_after_block(ctx->current_block));
   if (ctx->total_nesting > 1)
      agx_push_exec(&_b, ctx->loop_continues ? 2 : 1);

   /* Fallthrough to body */
   agx_block_add_successor(ctx->current_block, ctx->continue_block);

   /* Emit the body */
   ctx->after_block = ctx->continue_block;
   ctx->after_block->loop_header = true;
   agx_block *start_block = emit_cf_list(ctx, &nloop->body);

   /* If we used any continue jumps, we need to reactivate the continued
    * threads. We do this with an always true while_icmp, which behaves like:
    *
    *    if (r0l == 1) {
    *       r0l = 0;
    *    }
    *    update_exec
    *
    * If we did not use continue, this would be a no-op so it is omitted.
    */
   _b.cursor = agx_after_block(ctx->current_block);

   if (ctx->loop_continues) {
      agx_while_icmp(
         &_b, agx_zero(), agx_zero(), 2, AGX_ICOND_UEQ, false,
         NULL /* no semantic target, used purely for side effects */);
   }

   agx_jmp_exec_any(&_b, start_block);
   agx_pop_exec(&_b, ctx->loop_continues ? 2 : 1);
   agx_block_add_successor(ctx->current_block, ctx->continue_block);

   /* Pop off */
   ctx->after_block = ctx->break_block;
   ctx->break_block = popped_break;
   ctx->continue_block = popped_continue;

   /* Update shader-db stats */
   ++ctx->loop_count;

   /* All nested control flow must have finished */
   assert(ctx->loop_nesting == 0);

   /* Restore loop nesting (we might be inside an if inside an outer loop) */
   ctx->loop_nesting = pushed_nesting;
   ctx->total_nesting--;
   ctx->loop_continues = old_continues;
}

/* Before the first control flow structure, the nesting counter needs to be
 * zeroed for correct operation. This only happens at most once, since by
 * definition this occurs at the end of the first block, which dominates the
 * rest of the program. */

static void
emit_first_cf(agx_context *ctx)
{
   if (ctx->any_cf)
      return;

   agx_builder _b = agx_init_builder(ctx, agx_after_block(ctx->current_block));
   agx_begin_cf(&_b);
   ctx->any_cf = true;
}

static agx_block *
emit_cf_list(agx_context *ctx, struct exec_list *list)
{
   agx_block *start_block = NULL;

   foreach_list_typed(nir_cf_node, node, node, list) {
      switch (node->type) {
      case nir_cf_node_block: {
         agx_block *block = emit_block(ctx, nir_cf_node_as_block(node));

         if (!start_block)
            start_block = block;

         break;
      }

      case nir_cf_node_if:
         emit_first_cf(ctx);
         emit_if(ctx, nir_cf_node_as_if(node));
         break;

      case nir_cf_node_loop:
         emit_first_cf(ctx);
         emit_loop(ctx, nir_cf_node_as_loop(node));
         break;

      default:
         unreachable("Unknown control flow");
      }
   }

   return start_block;
}

static void
agx_set_st_vary_final(agx_context *ctx)
{
   agx_foreach_instr_global_rev(ctx, I) {
      if (I->op == AGX_OPCODE_ST_VARY) {
         I->last = true;
         return;
      }
   }

   /* If we got here, there was no varying written. We need to mark that. */
   agx_block *last_block = list_last_entry(&ctx->blocks, agx_block, link);
   agx_builder _b = agx_init_builder(ctx, agx_after_block_logical(last_block));
   agx_no_varyings(&_b);
}

static int
agx_dump_stats(agx_context *ctx, unsigned size, char **out)
{
   unsigned nr_ins = 0, spills = 0, fills = 0;

   /* Count instructions */
   agx_foreach_instr_global(ctx, I) {
      nr_ins++;

      if (I->op == AGX_OPCODE_STACK_STORE)
         spills++;
      else if (I->op == AGX_OPCODE_STACK_LOAD)
         fills++;
   }

   struct agx_cycle_estimate cycles = agx_estimate_cycles(ctx);

   unsigned nr_threads =
      agx_occupancy_for_register_count(ctx->max_reg).max_threads;

   return asprintf(
      out,
      "%s shader: %u inst, %u alu, %u fscib, %u ic, %u bytes, %u regs, "
      "%u uniforms, %u scratch, %u threads, %u loops, "
      "%u:%u spills:fills",
      gl_shader_stage_name(ctx->stage), nr_ins, cycles.alu, cycles.f_scib,
      cycles.ic, size, ctx->max_reg, ctx->out->push_count, ctx->scratch_size_B,
      nr_threads, ctx->loop_count, spills, fills);
}

static bool
agx_lower_sincos_filter(const nir_instr *instr, UNUSED const void *_)
{
   if (instr->type != nir_instr_type_alu)
      return false;

   nir_alu_instr *alu = nir_instr_as_alu(instr);
   return alu->op == nir_op_fsin || alu->op == nir_op_fcos;
}

/* Sine and cosine are implemented via the sin_pt_1 and sin_pt_2 opcodes for
 * heavy lifting. sin_pt_2 implements sinc in the first quadrant, expressed in
 * turns (sin (tau x) / x), while sin_pt_1 implements a piecewise sign/offset
 * fixup to transform a quadrant angle [0, 4] to [-1, 1]. The NIR opcode
 * fsin_agx models the fixup, sinc, and multiply to obtain sine, so we just
 * need to change units from radians to quadrants modulo turns. Cosine is
 * implemented by shifting by one quadrant: cos(x) = sin(x + tau/4).
 */

static nir_def *
agx_lower_sincos_impl(struct nir_builder *b, nir_instr *instr, UNUSED void *_)
{
   nir_alu_instr *alu = nir_instr_as_alu(instr);
   nir_def *x = nir_mov_alu(b, alu->src[0], 1);
   nir_def *turns = nir_fmul_imm(b, x, M_1_PI * 0.5f);

   if (alu->op == nir_op_fcos)
      turns = nir_fadd_imm(b, turns, 0.25f);

   nir_def *quadrants = nir_fmul_imm(b, nir_ffract(b, turns), 4.0);
   return nir_fsin_agx(b, quadrants);
}

static bool
agx_lower_sincos(nir_shader *shader)
{
   return nir_shader_lower_instructions(shader, agx_lower_sincos_filter,
                                        agx_lower_sincos_impl, NULL);
}

static bool
agx_lower_front_face(struct nir_builder *b, nir_intrinsic_instr *intr,
                     UNUSED void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_front_face)
      return false;

   nir_def *def = &intr->def;
   assert(def->bit_size == 1);

   b->cursor = nir_before_instr(&intr->instr);
   nir_def_rewrite_uses(def, nir_inot(b, nir_load_back_face_agx(b, 1)));
   return true;
}

/*
 * Standard NIR optimization loop. This is run in agx_preprocess_nir, then once
 * again at shader variant compile time. Unless there was a complex shader key,
 * the latter run should be almost a no-op.
 */
static void
agx_optimize_loop_nir(nir_shader *nir)
{
   bool progress;

   do {
      progress = false;

      NIR_PASS(progress, nir, nir_copy_prop);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_peephole_select, 64, false, true);
      NIR_PASS(progress, nir, nir_opt_phi_precision);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_undef);
      NIR_PASS(progress, nir, nir_opt_shrink_vectors, true);
      NIR_PASS(progress, nir, nir_opt_loop_unroll);
   } while (progress);
}

static bool
mem_vectorize_cb(unsigned align_mul, unsigned align_offset, unsigned bit_size,
                 unsigned num_components, unsigned hole_size,
                 nir_intrinsic_instr *low, nir_intrinsic_instr *high, void *data)
{
   if (hole_size)
      return false;

   /* Must be aligned to the size of the load */
   unsigned align = nir_combined_align(align_mul, align_offset);
   if ((bit_size / 8) > align)
      return false;

   if (num_components > 4)
      return false;

   if (bit_size > 32)
      return false;

   return true;
}

static bool
set_speculate(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *_)
{
   if (!nir_intrinsic_has_access(intr))
      return false;

   nir_intrinsic_set_access(intr,
                            ACCESS_CAN_SPECULATE | nir_intrinsic_access(intr));
   return true;
}

static bool
optimize_bounds(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_constant_agx)
      return false;

   assert(intr->def.bit_size > 1 && "no if-uses");
   nir_scalar srcs[2] = {{NULL}};
   unsigned use_count = 0;

   nir_alu_instr *first_use = NULL;

   nir_foreach_use(use, &intr->def) {
      /* All uses need to be bounds_agx */
      nir_instr *parent = nir_src_parent_instr(use);
      if (parent->type != nir_instr_type_alu)
         return false;

      nir_alu_instr *alu = nir_instr_as_alu(parent);
      if ((alu->op != nir_op_bounds_agx) || (alu->src[0].src.ssa != &intr->def))
         return false;

      assert(alu->def.num_components == 1 && alu->def.bit_size == 32);

      /* All bounds checks need a common offset and bounds */
      for (unsigned s = 0; s < 2; ++s) {
         nir_scalar this = nir_scalar_resolved(alu->src[1 + s].src.ssa,
                                               alu->src[1 + s].swizzle[0]);

         if (srcs[s].def == NULL)
            srcs[s] = this;
         else if (!nir_scalar_equal(srcs[s], this))
            return false;

         /* To avoid dominance problems, we must sink loads. */
         if (this.def->parent_instr->block != intr->instr.block) {
            return false;
         }
      }

      if (!first_use || first_use->def.index > alu->def.index) {
         first_use = alu;
      }

      ++use_count;
   }

   /* We've matched. Freeze the set of uses before chaning things. */
   nir_alu_instr **uses = alloca(sizeof(nir_alu_instr *) * use_count);

   unsigned i = 0;
   nir_foreach_use(use, &intr->def) {
      nir_instr *parent = nir_src_parent_instr(use);
      uses[i++] = nir_instr_as_alu(parent);
   }
   assert(i == use_count && "should not have changed");

   /* Sink the load */
   nir_instr_remove(&intr->instr);
   b->cursor = nir_before_instr(&first_use->instr);
   nir_builder_instr_insert(b, &intr->instr);

   /* Now start rewriting. Grab some common variables */
   b->cursor = nir_before_instr(&intr->instr);
   nir_def *offset = nir_channel(b, srcs[0].def, srcs[0].comp);
   nir_def *bounds = nir_channel(b, srcs[1].def, srcs[1].comp);

   nir_def *in_bounds = nir_uge(b, bounds, offset);
   nir_def *zero = nir_imm_int(b, 0);

   nir_src *base_src = &intr->src[0];
   nir_src *offs_src = &intr->src[1];

   nir_def *base_lo = nir_unpack_64_2x32_split_x(b, base_src->ssa);
   nir_def *base_hi = nir_unpack_64_2x32_split_y(b, base_src->ssa);

   /* Bounds check the base/offset instead. We currently reserve the bottom
    * 2^36 of VA (this is driver/compiler ABI). With soft fault enabled, that
    * means any read of the lower region will return zero as required.
    *
    * Therefore, when out-of-bounds, we clamp the index to zero and the high
    * half of the address to zero. We don't need to clamp the low half of the
    * address. The resulting sum is thus:
    *
    *    0*(2^32) + lo + (index << shift)
    *
    * ...which will be in the unmapped zero region provided shift < 4.
    */
   base_hi = nir_bcsel(b, in_bounds, base_hi, zero);

   /* Clamp index if the shift is too large or sign-extension used */
   if (nir_intrinsic_base(intr) >= 2 || nir_intrinsic_sign_extend(intr)) {
      nir_src_rewrite(offs_src, nir_bcsel(b, in_bounds, offs_src->ssa, zero));
   }

   nir_src_rewrite(base_src, nir_pack_64_2x32_split(b, base_lo, base_hi));

   /* Now that the load itself is bounds checked, all that's left is removing
    * the bounds checks on the output. This requires a little care to avoid an
    * infinite loop.
    *
    * Also note we cannot remove the uses here, because it would invalidate the
    * iterator inside intrinsics_pass. I hate C, don't you?
    */
   for (unsigned i = 0; i < use_count; ++i) {
      b->cursor = nir_after_instr(&uses[i]->instr);
      nir_def *chan = nir_channel(b, &intr->def, uses[i]->src[0].swizzle[0]);
      nir_def_rewrite_uses(&uses[i]->def, chan);
   }

   return true;
}

static void
agx_optimize_nir(nir_shader *nir, bool soft_fault, unsigned *preamble_size)
{
   /* This runs only once up front since other optimizations don't affect it */
   NIR_PASS(_, nir, nir_opt_shrink_stores, true);

   agx_optimize_loop_nir(nir);

   /* If soft fault is enabled, we can freely speculate everything. That lets us
    * peephole select and form preambles more aggressively.
    */
   if (soft_fault) {
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, set_speculate,
               nir_metadata_control_flow, NULL);
   }

   /* Peephole select again after setting the speculate flag but before
    * vectorizing. This cleans up short-circuit loads in unrolled loops.
    *
    * XXX: Set indirect_load_ok once we can investigate CTS flakes.
    */
   NIR_PASS(_, nir, nir_opt_peephole_select, 64, false, true);

   NIR_PASS(_, nir, nir_opt_load_store_vectorize,
            &(const nir_load_store_vectorize_options){
               .modes = nir_var_mem_global | nir_var_mem_constant,
               .callback = mem_vectorize_cb,
            });
   NIR_PASS(_, nir, nir_lower_pack);

   nir_convert_to_lcssa(nir, true, true);
   NIR_PASS_V(nir, nir_divergence_analysis);
   bool progress = false;

   static const nir_lower_subgroups_options subgroups_options = {
      .ballot_bit_size = 32,
      .ballot_components = 1,
      .lower_elect = true,
      .lower_subgroup_masks = true,
   };

   NIR_PASS(progress, nir, nir_opt_uniform_atomics, true);
   NIR_PASS(progress, nir, nir_opt_uniform_subgroup, &subgroups_options);

   /* The above create operations that need lowering/optimizing */
   if (progress) {
      NIR_PASS(_, nir, agx_nir_lower_subgroups);
      NIR_PASS(_, nir, nir_opt_algebraic);
   }

   progress = false;
   NIR_PASS(progress, nir, agx_nir_lower_address);

   /* If address lowering made progress, clean up before forming preambles.
    * Otherwise the optimized preambles might just be constants! Do it before
    * lowering int64 too, to avoid lowering constant int64 arithmetic.
    */
   if (progress) {
      NIR_PASS(_, nir, nir_opt_constant_folding);
      NIR_PASS(_, nir, nir_opt_dce);
   }

   /* Only lower int64 after optimizing address arithmetic, so that u2u64/i2i64
    * conversions remain.
    */
   progress = false;
   NIR_PASS(progress, nir, nir_lower_int64);

   /* If we lowered actual int64 arithmetic (not folded into the address
    * calculations), then clean up after the lowering.
    */
   if (progress) {
      do {
         progress = false;

         NIR_PASS(progress, nir, nir_opt_algebraic);
         NIR_PASS(progress, nir, nir_opt_constant_folding);
         NIR_PASS(progress, nir, nir_opt_dce);
      } while (progress);
   }

   if (preamble_size && (!(agx_compiler_debug & AGX_DBG_NOPREAMBLE)))
      NIR_PASS(_, nir, agx_nir_opt_preamble, preamble_size);

   /* Forming preambles may dramatically reduce the instruction count
    * in certain blocks, causing some if-else statements to become
    * trivial. We want to peephole select those, given that control flow
    * prediction instructions are costly.
    *
    * We need to lower int64 again to deal with the resulting 64-bit csels.
    */
   NIR_PASS(_, nir, nir_opt_peephole_select, 64, false, true);
   NIR_PASS(_, nir, nir_lower_int64);

   NIR_PASS(_, nir, nir_opt_algebraic_late);

   /* Fuse add/sub/multiplies/shifts after running opt_algebraic_late to fuse
    * isub but before shifts are lowered.
    */
   do {
      progress = false;

      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, agx_nir_fuse_algebraic_late);
   } while (progress);

   /* Before optimizing bounds checks, we need to clean up and index defs so
    * optimize_bounds does the right thing.
    */
   NIR_PASS(_, nir, nir_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);

   nir_foreach_function_impl(impl, nir) {
      nir_index_ssa_defs(impl);
   }

   if (soft_fault) {
      NIR_PASS(_, nir, nir_shader_intrinsics_pass, optimize_bounds,
               nir_metadata_control_flow, NULL);
   }

   /* Do remaining lowering late, since this inserts &s for shifts so we want to
    * do it after fusing constant shifts. Constant folding will clean up.
    */
   NIR_PASS(_, nir, agx_nir_lower_algebraic_late);
   NIR_PASS(_, nir, agx_nir_fuse_selects);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_combine_barriers, NULL, NULL);
   NIR_PASS(_, nir, nir_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_opt_cse);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);

   /* Cleanup optimizations */
   nir_move_options move_all = nir_move_const_undef | nir_move_load_ubo |
                               nir_move_load_input | nir_move_comparisons |
                               nir_move_copies | nir_move_load_ssbo |
                               nir_move_alu;

   NIR_PASS(_, nir, nir_opt_sink, move_all);
   NIR_PASS(_, nir, nir_opt_move, move_all);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, true);
}

/*
 * Varyings that are used as texture coordinates should be kept at fp32, because
 * fp16 does not have enough precision for large textures. It's technically
 * conformant not to, but every app gets this wrong.
 */
static bool
gather_texcoords(nir_builder *b, nir_instr *instr, void *data)
{
   uint64_t *mask = data;

   if (instr->type != nir_instr_type_tex)
      return false;

   nir_tex_instr *tex = nir_instr_as_tex(instr);

   int coord_idx = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   if (coord_idx < 0)
      return false;

   nir_src src = tex->src[coord_idx].src;
   nir_scalar x = nir_scalar_resolved(src.ssa, 0);
   nir_scalar y = nir_scalar_resolved(src.ssa, 1);

   if (x.def != y.def)
      return false;

   nir_instr *parent = x.def->parent_instr;

   if (parent->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intr = nir_instr_as_intrinsic(parent);

   if (intr->intrinsic != nir_intrinsic_load_interpolated_input)
      return false;

   nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
   *mask |= BITFIELD64_BIT(sem.location);
   return false;
}

static bool
gather_interp(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct agx_interp_info *masks = data;

   if (intr->intrinsic == nir_intrinsic_load_input) {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      masks->flat |= BITFIELD64_RANGE(sem.location, sem.num_slots);
   } else if (intr->intrinsic == nir_intrinsic_load_interpolated_input &&
              nir_intrinsic_interp_mode(nir_src_as_intrinsic(intr->src[0])) ==
                 INTERP_MODE_NOPERSPECTIVE) {
      nir_io_semantics sem = nir_intrinsic_io_semantics(intr);
      masks->linear |= BITFIELD64_RANGE(sem.location, sem.num_slots);
   }

   return false;
}

/*
 * Build a bit mask of varyings (by location) that are flatshaded and linear
 * shaded. This information is needed by the driver.
 */
struct agx_interp_info
agx_gather_interp_info(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);

   struct agx_interp_info masks = {0};
   nir_shader_intrinsics_pass(nir, gather_interp, nir_metadata_all, &masks);
   return masks;
}

/*
 * Build a bit mask of varyings (by location) that are used as texture
 * coordinates. This information is needed by lower_mediump_io.
 */
uint64_t
agx_gather_texcoords(nir_shader *nir)
{
   assert(nir->info.stage == MESA_SHADER_FRAGMENT);

   uint64_t mask = 0;
   nir_shader_instructions_pass(nir, gather_texcoords, nir_metadata_all, &mask);
   return mask;
}

static nir_mem_access_size_align
mem_access_size_align_cb(nir_intrinsic_op intrin, uint8_t bytes,
                         uint8_t bit_size, uint32_t align,
                         uint32_t align_offset, bool offset_is_const,
                         const void *cb_data)
{
   align = nir_combined_align(align, align_offset);

   assert(util_is_power_of_two_nonzero(align));

   if ((bytes & 1) || (align == 1))
      bit_size = 8;
   else if ((bytes & 2) || (align == 2))
      bit_size = 16;
   else if (bit_size >= 32)
      bit_size = 32;

   return (nir_mem_access_size_align){
      .num_components = MIN2(bytes / (bit_size / 8), 4),
      .bit_size = bit_size,
      .align = bit_size / 8,
   };
}

static unsigned
lower_bit_size_callback(const nir_instr *instr, UNUSED void *_)
{
   if (instr->type == nir_instr_type_intrinsic) {
      /* Handle small subgroup ops */
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

      switch (intr->intrinsic) {
      case nir_intrinsic_reduce:
      case nir_intrinsic_exclusive_scan:
      case nir_intrinsic_inclusive_scan:
         /* The identity for iand doesn't work for lowered 1-bit booleans, so
          * lower that explicitly.
          */
         if (nir_intrinsic_reduction_op(intr) == nir_op_iand &&
             intr->def.bit_size == 1)
            return 16;

         /* In general, we have 16-bit ops instead of 8-bit, so lower those. */
         return intr->def.bit_size == 8 ? 16 : 0;
      default:
         return 0;
      }
   } else if (instr->type == nir_instr_type_alu) {
      /* Lower 8-bit ALU to 16-bit. We check the destination, as we do not want
       * to lower conversions from 8-bit to larger types. Those conversions get
       * implemented natively.
       */
      nir_alu_instr *alu = nir_instr_as_alu(instr);
      if (alu->def.bit_size == 8 && !is_conversion_to_8bit(alu->op))
         return 16;
      else if (alu->def.bit_size == 1 && alu->src[0].src.ssa->bit_size == 8)
         return 16 /* comparisons */;
   }

   return 0;
}

static bool
lower_load_from_texture_handle(nir_builder *b, nir_intrinsic_instr *intr,
                               void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_from_texture_handle_agx)
      return false;

   /* Bindless handles are a vec2, where the first source is the (constant)
    * uniform register number and the second source is the byte offset.
    */
   nir_scalar uniform = nir_scalar_resolved(intr->src[0].ssa, 0);
   unsigned uniform_idx = nir_scalar_as_uint(uniform);

   b->cursor = nir_instr_remove(&intr->instr);
   nir_def *base = nir_load_preamble(b, 1, 64, uniform_idx);
   nir_def *offset = nir_u2u64(b, nir_channel(b, intr->src[0].ssa, 1));

   nir_def_rewrite_uses(&intr->def, nir_iadd(b, base, offset));
   return true;
}

static void
agx_remove_unreachable_block(agx_block *block)
{
   /* Delete the edges */
   agx_foreach_successor(block, succ) {
      unsigned block_idx = agx_predecessor_index(succ, block);

      /* Remove the corresponding predecessor from the successor */
      struct util_dynarray *blocks = &succ->predecessors;
      int remaining = agx_num_predecessors(succ) - (block_idx + 1);
      assert(remaining >= 0);

      memcpy(util_dynarray_element(blocks, agx_block *, block_idx),
             util_dynarray_element(blocks, agx_block *, block_idx + 1),
             remaining * sizeof(agx_block *));
      blocks->size -= sizeof(agx_block *);

      /* Remove the corresponding source from the phis */
      agx_foreach_phi_in_block(succ, phi) {
         assert(block_idx + 1 <= phi->nr_srcs);

         memcpy(phi->src + block_idx, phi->src + block_idx + 1,
                (phi->nr_srcs - (block_idx + 1)) * sizeof(phi->src[0]));

         phi->nr_srcs--;

         /* This might cause phis to become trivial. Lower 1-source phis to
          * moves and let copyprop take it from here.
          */
         if (phi->nr_srcs == 1) {
            phi->op = AGX_OPCODE_MOV;
         }
      }
   }

   /* Remove the successor from the predecessor. */
   block->successors[0] = NULL;
   block->successors[1] = NULL;

   /* Note: we do not remove the block itself, although it is now fully orphaned
    * in the control flow graph. We still need it in source order if it has any
    * pop_exec instructions, for a loop continue block.
    *
    * TODO: Is there a better way to handle this?
    *
    * Affects: dEQP-VK.graphicsfuzz.cov-matching-if-always-true-inside-loop
    */
}

/*
 * NIR sometimes contains unreachable blocks (e.g. due to infinite loops). These
 * blocks have no predecessors, but do have successors and can contribute to
 * phis. They are dead and do not need to be here. Further, they violate the IR
 * invariant:
 *
 *    Live-in sources are live-out in all predecessors.
 *
 * ...which RA depends on when handling live range splits. The simplest solution
 * is to simply delete these dead blocks. Fortunately, because they are
 * unreachable, this does not have any ill effects. Notably, this cannot
 * introduce critical edges.
 *
 * Deleting a block may cause a successor to become unreachable, so we use a
 * fixed-point algorithm to converge.
 */
static void
agx_remove_unreachable_blocks(agx_context *ctx)
{
   agx_block *start = agx_start_block(ctx);
   bool progress;

   do {
      progress = false;

      agx_foreach_block_safe(ctx, pred) {
         if (pred != start && agx_num_predecessors(pred) == 0 &&
             agx_num_successors(pred) > 0) {

            agx_remove_unreachable_block(pred);
            progress = true;
         }
      }
   } while (progress);
}

static bool
agx_should_dump(nir_shader *nir, unsigned agx_dbg_bit)
{
   return (agx_compiler_debug & agx_dbg_bit) &&
          !(nir->info.internal && !(agx_compiler_debug & AGX_DBG_INTERNAL));
}

#define AGX_PASS(shader, pass, ...)                                            \
   do {                                                                        \
      pass(shader, ##__VA_ARGS__);                                             \
      agx_validate(ctx, #pass);                                                \
   } while (0)

static unsigned
agx_compile_function_nir(nir_shader *nir, nir_function_impl *impl,
                         struct agx_shader_key *key,
                         struct util_debug_callback *debug,
                         struct util_dynarray *binary,
                         struct agx_shader_info *out)
{
   nir_index_blocks(impl);
   nir_index_ssa_defs(impl);

   agx_context *ctx = rzalloc(NULL, agx_context);
   ctx->nir = nir;
   ctx->is_preamble = impl->function->is_preamble;
   ctx->out = out;
   ctx->key = key;
   ctx->stage = nir->info.stage;
   ctx->allocated_vec = _mesa_hash_table_u64_create(ctx);
   ctx->indexed_nir_blocks = rzalloc_array(ctx, agx_block *, impl->num_blocks);
   list_inithead(&ctx->blocks);

   if (ctx->stage == MESA_SHADER_FRAGMENT && !ctx->is_preamble) {
      ctx->any_cf = key->fs.inside_sample_loop;
   }

   ctx->alloc = impl->ssa_alloc;
   emit_cf_list(ctx, &impl->body);
   agx_emit_phis_deferred(ctx);

   /* Index blocks now that we're done emitting so the order is consistent. Do
    * this before agx_remove_unreachable_blocks so we match NIR indexing. This
    * makes for easier debugging.
    */
   agx_foreach_block(ctx, block) {
      block->index = ctx->num_blocks++;
   }

   agx_remove_unreachable_blocks(ctx);

   /* Only allocate scratch if it's statically used, regardless of if the NIR
    * info claims otherwise.
    */
   if (ctx->any_scratch) {
      assert(!ctx->is_preamble && "preambles don't use scratch");
      ctx->scratch_size_B = ALIGN(nir->scratch_size, 16);
   }

   /* Stop the main shader or preamble shader after the exit block. For real
    * functions, we would return here.
    */
   if (!ctx->key->no_stop || ctx->is_preamble) {
      agx_block *last_block = list_last_entry(&ctx->blocks, agx_block, link);
      agx_builder _b = agx_init_builder(ctx, agx_after_block(last_block));
      agx_stop(&_b);
   }

   agx_validate(ctx, "IR translation");

   if (likely(!(agx_compiler_debug & AGX_DBG_NOOPT))) {
      /* Eliminate dead instructions before CSE to avoid silly scheduling */
      AGX_PASS(ctx, agx_dce, false);

      /* CSE before eliminating dead destinations so that subdivision is
       * optimized properly.
       */
      AGX_PASS(ctx, agx_opt_cse);

      /* After DCE, use counts are right so we can run the optimizer. */
      AGX_PASS(ctx, agx_optimizer_backward);
      AGX_PASS(ctx, agx_optimizer_forward);
      AGX_PASS(ctx, agx_opt_compact_constants);

      /* After inlining constants, promote what's left */
      if (key->promote_constants && !key->secondary &&
          !(agx_compiler_debug & AGX_DBG_NOPROMOTE)) {
         AGX_PASS(ctx, agx_opt_promote_constants);
      }
   }

   /* For correctness, lower uniform sources after copyprop (for correctness,
    * as copyprop creates uniform sources). To keep register pressure in
    * check, lower after CSE, since moves are cheaper than registers.
    */
   AGX_PASS(ctx, agx_lower_uniform_sources);

   /* RA correctness depends on DCE */
   AGX_PASS(ctx, agx_dce, true);

   if (agx_should_dump(nir, AGX_DBG_SHADERS))
      agx_print_shader(ctx, stdout);

   if (likely(!(agx_compiler_debug & AGX_DBG_NOSCHED))) {
      AGX_PASS(ctx, agx_pressure_schedule);
   }

   if (agx_should_dump(nir, AGX_DBG_SHADERS))
      agx_print_shader(ctx, stdout);

   AGX_PASS(ctx, agx_ra);
   agx_lower_64bit_postra(ctx);

   if (ctx->scratch_size_B > 0) {
      /* Apple always allocate 40 more bytes in the entrypoint and align to 4. */
      uint64_t stack_size = ALIGN(DIV_ROUND_UP(ctx->scratch_size_B, 4) + 10, 4);

      assert(stack_size < INT16_MAX);

      agx_block *start_block = agx_start_block(ctx);
      agx_builder _b = agx_init_builder(ctx, agx_before_block(start_block));
      agx_stack_adjust(&_b, stack_size);

      /* If we're going to execute multiple times, make sure we clean up after
       * ourselves, else the hardware faults.
       */
      if (ctx->stage == MESA_SHADER_FRAGMENT && !ctx->is_preamble &&
          ctx->key->fs.inside_sample_loop) {

         _b = agx_init_builder(ctx, agx_after_block(agx_end_block(ctx)));
         agx_stack_adjust(&_b, -stack_size);
      }

      if (ctx->is_preamble)
         out->preamble_scratch_size = stack_size;
      else
         out->scratch_size = stack_size;
   }

   if ((ctx->stage == MESA_SHADER_VERTEX ||
        ctx->stage == MESA_SHADER_TESS_EVAL) &&
       !impl->function->is_preamble && !ctx->key->secondary)
      agx_set_st_vary_final(ctx);

   agx_insert_waits(ctx);
   agx_opt_empty_else(ctx);
   agx_opt_break_if(ctx);
   agx_opt_jmp_none(ctx);

   if (ctx->any_quad_divergent_shuffle)
      agx_lower_divergent_shuffle(ctx);

   agx_lower_pseudo(ctx);

   if (agx_should_dump(nir, AGX_DBG_SHADERS))
      agx_print_shader(ctx, stdout);

   /* Pad binary */
   if (binary->size % AGX_CODE_ALIGN) {
      unsigned ngrow = AGX_CODE_ALIGN - (binary->size % AGX_CODE_ALIGN);
      memset(util_dynarray_grow_bytes(binary, ngrow, 1), 0, ngrow);
   }

   unsigned offset = binary->size;
   assert((offset % AGX_CODE_ALIGN) == 0);

   agx_pack_binary(ctx, binary);

   unsigned nr_gprs = ctx->max_reg + 1;

   /* If the preamble uses scratch (due to spilling), we need to set maximal
    * GPRs. Do it here so the driver doesn't have to worry about it.
    */
   if (impl->function->is_preamble)
      out->nr_preamble_gprs = ctx->scratch_size_B ? 256 : nr_gprs;
   else
      out->nr_gprs = nr_gprs;

   /* Don't dump statistics for preambles, since they're not worth optimizing */
   if (!impl->function->is_preamble) {
      char *stats;
      int ret = agx_dump_stats(ctx, binary->size, &stats);

      if (ret >= 0) {
         if (agx_should_dump(nir, AGX_DBG_SHADERDB)) {
            fprintf(stderr, "SHADER-DB: %s - %s\n", nir->info.label ?: "",
                    stats);
         }

         if (debug)
            util_debug_message(debug, SHADER_INFO, "%s", stats);

         free(stats);
      }
   }

   ralloc_free(ctx);

   return offset;
}

void
agx_link_libagx(nir_shader *nir, const nir_shader *libagx)
{
   nir_link_shader_functions(nir, libagx);
   NIR_PASS(_, nir, nir_inline_functions);
   nir_remove_non_entrypoints(nir);
   NIR_PASS(_, nir, nir_opt_deref);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_remove_dead_derefs);
   NIR_PASS(_, nir, nir_remove_dead_variables,
            nir_var_function_temp | nir_var_shader_temp, NULL);
   NIR_PASS(_, nir, nir_lower_vars_to_explicit_types,
            nir_var_shader_temp | nir_var_function_temp,
            glsl_get_cl_type_size_align);
}

/*
 * The hardware frcp instruction is sometimes off by 1 ULP. For correctly
 * rounded frcp, a refinement step is required. This routine has been
 * exhaustively tested with a modified math_bruteforce.
 *
 * While Khronos APIs allow 2.5 ULP error for divides, nir_lower_idiv relies on
 * correctly rounded frcp. This is therefore load bearing for integer division
 * on all APIs.
 */
static nir_def *
libagx_frcp(nir_builder *b, nir_def *x)
{
   nir_def *u = nir_frcp(b, x);

   /* Do 1 Newton-Raphson refinement step.
    *
    * Define f(u) = xu - 1. Then f(u) = 0 iff u = 1/x. Newton's method gives:
    *
    * u_2 = u - f(u) / f'(u) = u - (xu - 1) / x
    *
    * Our original guess is close, so we approximate (1 / x) by u:
    *
    * u_2 = u - u(xu - 1) = u + u(1 - xu)
    *     = fma(fma(-x, u, 1), u, u)
    */
   nir_def *one = nir_imm_float(b, 1.0);
   nir_def *u_2 = nir_ffma(b, nir_ffma(b, nir_fneg(b, x), u, one), u, u);

   /* If the original value was infinite, frcp will generate the correct zero.
    * However, the Newton-Raphson step would multiply 0 * Inf and get a NaN. So
    * skip the refinement step for infinite inputs. We do this backwards,
    * checking whether the refined result is NaN, since we can implement this
    * check in a single fcmpsel instruction. The other case where the refinement
    * is NaN is a NaN input, in which skipping refinement is acceptable.
    */
   return nir_bcsel(b, nir_fisnan(b, u_2), u, u_2);
}

static bool
agx_nir_lower_fdiv(nir_builder *b, nir_alu_instr *alu, void *_)
{
   if (alu->op != nir_op_frcp || !alu->exact || alu->def.bit_size != 32)
      return false;

   b->cursor = nir_before_instr(&alu->instr);
   nir_def_replace(&alu->def, libagx_frcp(b, nir_ssa_for_alu_src(b, alu, 0)));
   return true;
}

/* Preprocess NIR independent of shader state */
void
agx_preprocess_nir(nir_shader *nir, const nir_shader *libagx)
{
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   /* Lower large arrays to scratch and small arrays to csel */
   NIR_PASS(_, nir, nir_lower_vars_to_scratch, nir_var_function_temp, 256,
            glsl_get_natural_size_align_bytes,
            glsl_get_natural_size_align_bytes);
   NIR_PASS(_, nir, nir_lower_indirect_derefs, nir_var_function_temp, ~0);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_lower_var_copies);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      NIR_PASS(_, nir, agx_nir_lower_frag_sidefx);
   }

   /* Clean up deref gunk after lowering I/O */
   NIR_PASS(_, nir, nir_opt_dce);

   agx_link_libagx(nir, libagx);

   /* Runs before we lower away idiv, to work at all. But runs after lowering
    * textures, since the cube map array lowering generates division by 6.
    */
   NIR_PASS(_, nir, nir_opt_idiv_const, 16);

   nir_lower_idiv_options idiv_options = {
      .allow_fp16 = true,
   };

   NIR_PASS(_, nir, nir_lower_idiv, &idiv_options);
   NIR_PASS(_, nir, nir_lower_frexp);
   NIR_PASS(_, nir, nir_lower_alu);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);
   NIR_PASS(_, nir, nir_lower_flrp, 16 | 32 | 64, false);
   NIR_PASS(_, nir, agx_lower_sincos);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, agx_lower_front_face,
            nir_metadata_control_flow, NULL);
   NIR_PASS(_, nir, agx_nir_lower_subgroups);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, true);
   NIR_PASS(_, nir, nir_shader_alu_pass, agx_nir_lower_fdiv,
            nir_metadata_control_flow, NULL);

   /* After lowering, run through the standard suite of NIR optimizations. We
    * will run through the loop later, once we have the shader key, but if we
    * run now, that run will ideally be almost a no-op.
    */
   agx_optimize_loop_nir(nir);

   NIR_PASS(_, nir, nir_opt_deref);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   /* We're lowered away all variables. Remove them all for smaller shaders. */
   NIR_PASS(_, nir, nir_remove_dead_variables, nir_var_all, NULL);
   nir->info.io_lowered = true;

   /* Move before lowering */
   nir_move_options move_all = nir_move_const_undef | nir_move_load_ubo |
                               nir_move_load_input | nir_move_comparisons |
                               nir_move_copies | nir_move_load_ssbo |
                               nir_move_alu;

   NIR_PASS(_, nir, nir_opt_sink, move_all);
   NIR_PASS(_, nir, nir_opt_move, move_all);
   NIR_PASS(_, nir, agx_nir_lower_shared_bitsize);
}

void
agx_compile_shader_nir(nir_shader *nir, struct agx_shader_key *key,
                       struct util_debug_callback *debug,
                       struct agx_shader_part *out)
{
   agx_compiler_debug = agx_get_compiler_debug();
   struct agx_shader_info *info = &out->info;

   struct util_dynarray binary;
   util_dynarray_init(&binary, NULL);

   memset(out, 0, sizeof *out);

   assert(nir->info.io_lowered &&
          "agx_preprocess_nir is called first, then the shader is specalized,"
          "then the specialized shader is compiled");

   /* If required, tag writes will be enabled by instruction selection */
   if (nir->info.stage == MESA_SHADER_FRAGMENT)
      info->tag_write_disable = !nir->info.writes_memory;

   bool needs_libagx = true /* TODO: Optimize */;

   NIR_PASS(_, nir, nir_lower_frag_coord_to_pixel_coord);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);

   if (needs_libagx) {
      agx_link_libagx(nir, key->libagx);

      NIR_PASS(_, nir, nir_opt_deref);
      NIR_PASS(_, nir, nir_lower_vars_to_ssa);
      NIR_PASS(_, nir, nir_lower_explicit_io,
               nir_var_shader_temp | nir_var_function_temp |
                  nir_var_mem_shared | nir_var_mem_global,
               nir_address_format_62bit_generic);
   }

   /* Late sysval lowering creates large loads. Load lowering creates unpacks */
   nir_lower_mem_access_bit_sizes_options lower_mem_access_options = {
      .modes = nir_var_mem_ssbo | nir_var_mem_constant |
               nir_var_mem_task_payload | nir_var_shader_temp |
               nir_var_function_temp | nir_var_mem_global | nir_var_mem_shared,
      .callback = mem_access_size_align_cb,
   };
   NIR_PASS(_, nir, nir_lower_mem_access_bit_sizes, &lower_mem_access_options);

   /* Cleanup 8-bit math before lowering */
   bool progress;
   do {
      progress = false;

      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dce);
   } while (progress);

   NIR_PASS(_, nir, nir_lower_bit_size, lower_bit_size_callback, NULL);

   /* Late blend lowering creates vectors */
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_load_const_to_scalar);

   /* Late VBO lowering creates constant udiv instructions */
   NIR_PASS(_, nir, nir_opt_idiv_const, 16);

   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_shader_intrinsics_pass, lower_load_from_texture_handle,
            nir_metadata_control_flow, NULL);

   info->push_count = key->reserved_preamble;
   agx_optimize_nir(nir, key->dev.soft_fault,
                    key->secondary ? NULL : &info->push_count);

   if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      info->varyings.fs.nr_cf = key->fs.cf_base;
      assign_coefficient_regs(nir, &info->varyings.fs);
   }

   if (agx_should_dump(nir, AGX_DBG_SHADERS))
      nir_print_shader(nir, stdout);

   info->local_size = nir->info.shared_size;

   nir_foreach_function_with_impl(func, impl, nir) {
      unsigned offset =
         agx_compile_function_nir(nir, impl, key, debug, &binary, &out->info);

      if (func->is_preamble) {
         info->preamble_offset = offset;
         info->has_preamble = true;
      } else if (func->is_entrypoint) {
         info->main_offset = offset;
         info->main_size = binary.size - offset;
      } else {
         unreachable("General functions not yet supported");
      }
   }

   info->stage = nir->info.stage;

   /* Check these outside the stage check since nir->info.stage is the hardware
    * stage and these are read in the vertex *software* stage.
    */
   info->uses_draw_id =
      BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_DRAW_ID);

   info->uses_base_param =
      BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_FIRST_VERTEX) ||
      BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BASE_VERTEX) ||
      BITSET_TEST(nir->info.system_values_read, SYSTEM_VALUE_BASE_INSTANCE);

   if (nir->info.stage == MESA_SHADER_VERTEX ||
       nir->info.stage == MESA_SHADER_TESS_EVAL) {
      info->nonzero_viewport = nir->info.outputs_written & VARYING_BIT_VIEWPORT;

      info->writes_layer_viewport =
         nir->info.outputs_written & (VARYING_BIT_LAYER | VARYING_BIT_VIEWPORT);

   } else if (nir->info.stage == MESA_SHADER_FRAGMENT) {
      info->disable_tri_merging = nir->info.uses_wide_subgroup_intrinsics ||
                                  nir->info.fs.needs_quad_helper_invocations ||
                                  nir->info.writes_memory;

      /* Writing the sample mask requires tag writes */
      info->tag_write_disable &= !info->writes_sample_mask;

      /* Report a canonical depth layout. This happens at the end because the
       * sample mask lowering affects it.
       */
      enum gl_frag_depth_layout layout = nir->info.fs.depth_layout;

      if (!(nir->info.outputs_written & BITFIELD64_BIT(FRAG_RESULT_DEPTH)))
         info->depth_layout = FRAG_DEPTH_LAYOUT_UNCHANGED;
      else if (layout == FRAG_DEPTH_LAYOUT_NONE)
         info->depth_layout = FRAG_DEPTH_LAYOUT_ANY;
      else
         info->depth_layout = layout;

      info->reads_tib = nir->info.fs.uses_fbfetch_output;
      info->early_fragment_tests = nir->info.fs.early_fragment_tests;
   } else if (nir->info.stage == MESA_SHADER_COMPUTE) {
      info->imageblock_stride = nir->info.cs.image_block_size_per_thread_agx;
   }

   out->binary = binary.data;
   out->binary_size = binary.size;
}
