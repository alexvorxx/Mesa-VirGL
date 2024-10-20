/*
 * Copyright © 2016 Intel Corporation
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

#ifndef BLORP_GENX_EXEC_BRW_H
#define BLORP_GENX_EXEC_BRW_H

#include "blorp_priv.h"
#include "dev/intel_device_info.h"
#include "common/intel_compute_slm.h"
#include "common/intel_sample_positions.h"
#include "common/intel_l3_config.h"
#include "genxml/gen_macros.h"
#include "intel/compiler/brw_compiler.h"

/**
 * This file provides the blorp pipeline setup and execution functionality.
 * It defines the following function:
 *
 * static void
 * blorp_exec(struct blorp_context *blorp, void *batch_data,
 *            const struct blorp_params *params);
 *
 * It is the job of whoever includes this header to wrap this in something
 * to get an externally visible symbol.
 *
 * In order for the blorp_exec function to work, the driver must provide
 * implementations of the following static helper functions.
 */

static void *
blorp_emit_dwords(struct blorp_batch *batch, unsigned n);

static uint64_t
blorp_emit_reloc(struct blorp_batch *batch,
                 void *location, struct blorp_address address, uint32_t delta);

static void
blorp_measure_start(struct blorp_batch *batch,
                    const struct blorp_params *params);

static void
blorp_measure_end(struct blorp_batch *batch,
                  const struct blorp_params *params);

static void *
blorp_alloc_dynamic_state(struct blorp_batch *batch,
                          uint32_t size,
                          uint32_t alignment,
                          uint32_t *offset);

UNUSED static void *
blorp_alloc_general_state(struct blorp_batch *batch,
                          uint32_t size,
                          uint32_t alignment,
                          uint32_t *offset);

static uint32_t
blorp_get_dynamic_state(struct blorp_batch *batch,
                        enum blorp_dynamic_state name);

static void *
blorp_alloc_vertex_buffer(struct blorp_batch *batch, uint32_t size,
                          struct blorp_address *addr);
static void
blorp_vf_invalidate_for_vb_48b_transitions(struct blorp_batch *batch,
                                           const struct blorp_address *addrs,
                                           uint32_t *sizes,
                                           unsigned num_vbs);

UNUSED static struct blorp_address
blorp_get_workaround_address(struct blorp_batch *batch);

static bool
blorp_alloc_binding_table(struct blorp_batch *batch, unsigned num_entries,
                          unsigned state_size, unsigned state_alignment,
                          uint32_t *bt_offset, uint32_t *surface_offsets,
                          void **surface_maps);

static uint32_t
blorp_binding_table_offset_to_pointer(struct blorp_batch *batch,
                                      uint32_t offset);

static void
blorp_flush_range(struct blorp_batch *batch, void *start, size_t size);

static void
blorp_surface_reloc(struct blorp_batch *batch, uint32_t ss_offset,
                    struct blorp_address address, uint32_t delta);

static uint64_t
blorp_get_surface_address(struct blorp_batch *batch,
                          struct blorp_address address);

#if GFX_VER < 10
static struct blorp_address
blorp_get_surface_base_address(struct blorp_batch *batch);
#endif

static const struct intel_l3_config *
blorp_get_l3_config(struct blorp_batch *batch);

static void
blorp_pre_emit_urb_config(struct blorp_batch *batch,
                          struct intel_urb_config *urb_config);

static void
blorp_emit_urb_config(struct blorp_batch *batch,
                      struct intel_urb_config *urb_config);

static void
blorp_emit_pipeline(struct blorp_batch *batch,
                    const struct blorp_params *params);

static void
blorp_emit_pre_draw(struct blorp_batch *batch,
                    const struct blorp_params *params);
static void
blorp_emit_post_draw(struct blorp_batch *batch,
                     const struct blorp_params *params);

static inline unsigned
brw_blorp_get_urb_length(const struct brw_wm_prog_data *prog_data)
{
   if (prog_data == NULL)
      return 1;

   /* From the BSpec: 3D Pipeline - Strips and Fans - 3DSTATE_SBE
    *
    * read_length = ceiling((max_source_attr+1)/2)
    */
   return MAX2((prog_data->num_varying_inputs + 1) / 2, 1);
}

/***** BEGIN blorp_exec implementation ******/

static uint64_t
_blorp_combine_address(struct blorp_batch *batch, void *location,
                       struct blorp_address address, uint32_t delta)
{
   if (address.buffer == NULL) {
      return address.offset + delta;
   } else {
      return blorp_emit_reloc(batch, location, address, delta);
   }
}

#define __gen_address_type struct blorp_address
#define __gen_user_data struct blorp_batch
#define __gen_combine_address _blorp_combine_address

#include "genxml/genX_pack.h"
#include "common/intel_genX_state_brw.h"

#define _blorp_cmd_length(cmd) cmd ## _length
#define _blorp_cmd_length_bias(cmd) cmd ## _length_bias
#define _blorp_cmd_header(cmd) cmd ## _header
#define _blorp_cmd_pack(cmd) cmd ## _pack

#define blorp_emit(batch, cmd, name)                              \
   for (struct cmd name = { _blorp_cmd_header(cmd) },             \
        *_dst = blorp_emit_dwords(batch, _blorp_cmd_length(cmd)); \
        __builtin_expect(_dst != NULL, 1);                        \
        _blorp_cmd_pack(cmd)(batch, (void *)_dst, &name),         \
        _dst = NULL)

#define blorp_emitn(batch, cmd, n, ...) ({                  \
      uint32_t *_dw = blorp_emit_dwords(batch, n);          \
      if (_dw) {                                            \
         struct cmd template = {                            \
            _blorp_cmd_header(cmd),                         \
            .DWordLength = n - _blorp_cmd_length_bias(cmd), \
            __VA_ARGS__                                     \
         };                                                 \
         _blorp_cmd_pack(cmd)(batch, _dw, &template);       \
      }                                                     \
      _dw ? _dw + 1 : NULL; /* Array starts at dw[1] */     \
   })

#define STRUCT_ZERO(S) ({ struct S t; memset(&t, 0, sizeof(t)); t; })

#define blorp_context_upload_dynamic(context, state, name,              \
                                     align, dynamic_name)               \
   for (struct state name = STRUCT_ZERO(state), *_dst = &name;          \
        _dst != NULL;                                                   \
        ({                                                              \
           uint32_t _dw[_blorp_cmd_length(state)];                      \
           _blorp_cmd_pack(state)(NULL, (void *)_dw, &name);            \
           context->upload_dynamic_state(context, _dw,                  \
                                         _blorp_cmd_length(state) * 4,  \
                                         align, dynamic_name);          \
           _dst = NULL;                                                 \
        }))

#define blorp_emit_dynamic(batch, state, name, align, offset)           \
   for (struct state name = STRUCT_ZERO(state),                         \
        *_dst = blorp_alloc_dynamic_state(batch,                   \
                                          _blorp_cmd_length(state) * 4, \
                                          align, offset);               \
        __builtin_expect(_dst != NULL, 1);                              \
        _blorp_cmd_pack(state)(batch, (void *)_dst, &name),             \
        blorp_flush_range(batch, _dst, _blorp_cmd_length(state) * 4),   \
        _dst = NULL)

/* 3DSTATE_URB
 * 3DSTATE_URB_VS
 * 3DSTATE_URB_HS
 * 3DSTATE_URB_DS
 * 3DSTATE_URB_GS
 *
 * Assign the entire URB to the VS. Even though the VS disabled, URB space
 * is still needed because the clipper loads the VUE's from the URB. From
 * the Sandybridge PRM, Volume 2, Part 1, Section 3DSTATE,
 * Dword 1.15:0 "VS Number of URB Entries":
 *     This field is always used (even if VS Function Enable is DISABLED).
 *
 * The warning below appears in the PRM (Section 3DSTATE_URB), but we can
 * safely ignore it because this batch contains only one draw call.
 *     Because of URB corruption caused by allocating a previous GS unit
 *     URB entry to the VS unit, software is required to send a “GS NULL
 *     Fence” (Send URB fence with VS URB size == 1 and GS URB size == 0)
 *     plus a dummy DRAW call before any case where VS will be taking over
 *     GS URB space.
 *
 * If the 3DSTATE_URB_VS is emitted, than the others must be also.
 * From the Ivybridge PRM, Volume 2 Part 1, section 1.7.1 3DSTATE_URB_VS:
 *
 *     3DSTATE_URB_HS, 3DSTATE_URB_DS, and 3DSTATE_URB_GS must also be
 *     programmed in order for the programming of this state to be
 *     valid.
 */
static void
emit_urb_config(struct blorp_batch *batch,
                const struct blorp_params *params,
                UNUSED enum intel_urb_deref_block_size *deref_block_size)
{
   /* Once vertex fetcher has written full VUE entries with complete
    * header the space requirement is as follows per vertex (in bytes):
    *
    *     Header    Position    Program constants
    *   +--------+------------+-------------------+
    *   |   16   |     16     |      n x 16       |
    *   +--------+------------+-------------------+
    *
    * where 'n' stands for number of varying inputs expressed as vec4s.
    */
   struct brw_wm_prog_data *wm_prog_data = params->wm_prog_data;
   const unsigned num_varyings =
      wm_prog_data ? wm_prog_data->num_varying_inputs : 0;
   const unsigned total_needed = 16 + 16 + num_varyings * 16;

   /* The URB size is expressed in units of 64 bytes (512 bits) */
   const unsigned vs_entry_size = DIV_ROUND_UP(total_needed, 64);

   struct intel_urb_config urb_cfg = {
      .size = { vs_entry_size, 1, 1, 1 },
   };

   bool constrained;
   intel_get_urb_config(batch->blorp->compiler->brw->devinfo,
                        blorp_get_l3_config(batch),
                        false, false, &urb_cfg,
                        deref_block_size, &constrained);

   /* Tell drivers about the config. */
   blorp_pre_emit_urb_config(batch, &urb_cfg);

   for (int i = 0; i <= MESA_SHADER_GEOMETRY; i++) {
      blorp_emit(batch, GENX(3DSTATE_URB_VS), urb) {
         urb._3DCommandSubOpcode      += i;
         urb.VSURBStartingAddress      = urb_cfg.start[i];
         urb.VSURBEntryAllocationSize  = urb_cfg.size[i] - 1;
         urb.VSNumberofURBEntries      = urb_cfg.entries[i];
      }
   }

   if (batch->blorp->config.use_mesh_shading) {
#if GFX_VERx10 >= 125
      blorp_emit(batch, GENX(3DSTATE_URB_ALLOC_MESH), zero);
      blorp_emit(batch, GENX(3DSTATE_URB_ALLOC_TASK), zero);
#endif
   }
}

static void
blorp_emit_memcpy(struct blorp_batch *batch,
                  struct blorp_address dst,
                  struct blorp_address src,
                  uint32_t size);

static void
blorp_emit_vertex_data(struct blorp_batch *batch,
                       const struct blorp_params *params,
                       struct blorp_address *addr,
                       uint32_t *size)
{
   const float vertices[] = {
      /* v0 */ (float)params->x1, (float)params->y1, params->z,
      /* v1 */ (float)params->x0, (float)params->y1, params->z,
      /* v2 */ (float)params->x0, (float)params->y0, params->z,
   };

   void *data = blorp_alloc_vertex_buffer(batch, sizeof(vertices), addr);
   if (data == NULL)
      return;
   memcpy(data, vertices, sizeof(vertices));
   *size = sizeof(vertices);
   blorp_flush_range(batch, data, *size);
}

static void
blorp_emit_input_varying_data(struct blorp_batch *batch,
                              const struct blorp_params *params,
                              struct blorp_address *addr,
                              uint32_t *size)
{
   const unsigned vec4_size_in_bytes = 4 * sizeof(float);
   const unsigned max_num_varyings =
      DIV_ROUND_UP(sizeof(params->wm_inputs), vec4_size_in_bytes);
   struct brw_wm_prog_data *wm_prog_data = params->wm_prog_data;
   const unsigned num_varyings =
      wm_prog_data ? wm_prog_data->num_varying_inputs : 0;

   *size = 16 + num_varyings * vec4_size_in_bytes;

   const uint32_t *const inputs_src = (const uint32_t *)&params->wm_inputs;
   void *data = blorp_alloc_vertex_buffer(batch, *size, addr);
   if (data == NULL)
      return;
   uint32_t *inputs = data;

   /* Copy in the VS inputs */
   assert(sizeof(params->vs_inputs) == 16);
   memcpy(inputs, &params->vs_inputs, sizeof(params->vs_inputs));
   inputs += 4;

   if (params->wm_prog_data) {
      /* Walk over the attribute slots, determine if the attribute is used by
       * the program and when necessary copy the values from the input storage
       * to the vertex data buffer.
       */
      for (unsigned i = 0; i < max_num_varyings; i++) {
         const gl_varying_slot attr = VARYING_SLOT_VAR0 + i;

         const int input_index = wm_prog_data->urb_setup[attr];
         if (input_index < 0)
            continue;

         memcpy(inputs, inputs_src + i * 4, vec4_size_in_bytes);

         inputs += 4;
      }
   }

   blorp_flush_range(batch, data, *size);

   if (params->dst_clear_color_as_input) {
      /* In this case, the clear color isn't known statically and instead
       * comes in through an indirect which we have to copy into the vertex
       * buffer before we execute the 3DPRIMITIVE.  We already copied the
       * value of params->wm_inputs.clear_color into the vertex buffer in the
       * loop above.  Now we emit code to stomp it from the GPU with the
       * actual clear color value.
       */
      assert(num_varyings == 1);

      /* The clear color is the first thing after the header */
      struct blorp_address clear_color_input_addr = *addr;
      clear_color_input_addr.offset += 16;

      const unsigned clear_color_size =
         GFX_VER < 10 ? batch->blorp->isl_dev->ss.clear_value_size : 4 * 4;
      blorp_emit_memcpy(batch, clear_color_input_addr,
                        params->dst.clear_color_addr,
                        clear_color_size);
   }
}

static void
blorp_fill_vertex_buffer_state(struct GENX(VERTEX_BUFFER_STATE) *vb,
                               unsigned idx,
                               struct blorp_address addr, uint32_t size,
                               uint32_t stride)
{
   vb[idx].VertexBufferIndex = idx;
   vb[idx].BufferStartingAddress = addr;
   vb[idx].BufferPitch = stride;
   vb[idx].MOCS = addr.mocs;
   vb[idx].AddressModifyEnable = true;
   vb[idx].BufferSize = size;

#if GFX_VER >= 12
   vb[idx].L3BypassDisable = true;
#endif
}

static void
blorp_emit_vertex_buffers(struct blorp_batch *batch,
                          const struct blorp_params *params)
{
   struct GENX(VERTEX_BUFFER_STATE) vb[2] = {};
   const uint32_t num_vbs = ARRAY_SIZE(vb);

   struct blorp_address addrs[2] = {};
   uint32_t sizes[2] = {};
   blorp_emit_vertex_data(batch, params, &addrs[0], &sizes[0]);
   if (sizes[0] == 0)
      return;
   blorp_fill_vertex_buffer_state(vb, 0, addrs[0], sizes[0],
                                  3 * sizeof(float));

   blorp_emit_input_varying_data(batch, params, &addrs[1], &sizes[1]);
   blorp_fill_vertex_buffer_state(vb, 1, addrs[1], sizes[1], 0);

   blorp_vf_invalidate_for_vb_48b_transitions(batch, addrs, sizes, num_vbs);

   const unsigned num_dwords = 1 + num_vbs * GENX(VERTEX_BUFFER_STATE_length);
   uint32_t *dw = blorp_emitn(batch, GENX(3DSTATE_VERTEX_BUFFERS), num_dwords);
   if (!dw)
      return;

   for (unsigned i = 0; i < num_vbs; i++) {
      GENX(VERTEX_BUFFER_STATE_pack)(batch, dw, &vb[i]);
      dw += GENX(VERTEX_BUFFER_STATE_length);
   }
}

static void
blorp_emit_vertex_elements(struct blorp_batch *batch,
                           const struct blorp_params *params)
{
   struct brw_wm_prog_data *wm_prog_data = params->wm_prog_data;
   const unsigned num_varyings =
      wm_prog_data ? wm_prog_data->num_varying_inputs : 0;
   const unsigned num_elements = 2 + num_varyings;

   struct GENX(VERTEX_ELEMENT_STATE) ve[num_elements];
   memset(ve, 0, num_elements * sizeof(*ve));

   /* Setup VBO for the rectangle primitive..
    *
    * A rectangle primitive (3DPRIM_RECTLIST) consists of only three
    * vertices. The vertices reside in screen space with DirectX
    * coordinates (that is, (0, 0) is the upper left corner).
    *
    *   v2 ------ implied
    *    |        |
    *    |        |
    *   v1 ----- v0
    *
    * Since the VS is disabled, the clipper loads each VUE directly from
    * the URB. This is controlled by the 3DSTATE_VERTEX_BUFFERS and
    * 3DSTATE_VERTEX_ELEMENTS packets below. The VUE contents are as follows:
    *   dw0: Reserved, MBZ.
    *   dw1: Render Target Array Index. Below vertex fetcher gets programmed
    *        to assign this with primitive instance identifier which will be
    *        used for layered clears. All other renders have only one instance
    *        and therefore the value will be effectively zero.
    *   dw2: Viewport Index. The HiZ op disables viewport mapping and
    *        scissoring, so set the dword to 0.
    *   dw3: Point Width: The HiZ op does not emit the POINTLIST primitive,
    *        so set the dword to 0.
    *   dw4: Vertex Position X.
    *   dw5: Vertex Position Y.
    *   dw6: Vertex Position Z.
    *   dw7: Vertex Position W.
    *
    *   dw8: Flat vertex input 0
    *   dw9: Flat vertex input 1
    *   ...
    *   dwn: Flat vertex input n - 8
    *
    * For details, see the Sandybridge PRM, Volume 2, Part 1, Section 1.5.1
    * "Vertex URB Entry (VUE) Formats".
    *
    * Only vertex position X and Y are going to be variable, Z is fixed to
    * zero and W to one. Header words dw0,2,3 are zero. There is no need to
    * include the fixed values in the vertex buffer. Vertex fetcher can be
    * instructed to fill vertex elements with constant values of one and zero
    * instead of reading them from the buffer.
    * Flat inputs are program constants that are not interpolated. Moreover
    * their values will be the same between vertices.
    *
    * See the vertex element setup below.
    */
   unsigned slot = 0;

   ve[slot] = (struct GENX(VERTEX_ELEMENT_STATE)) {
      .VertexBufferIndex = 1,
      .Valid = true,
      .SourceElementFormat = ISL_FORMAT_R32G32B32A32_FLOAT,
      .SourceElementOffset = 0,
      .Component0Control = VFCOMP_STORE_SRC,

      /* From Gfx8 onwards hardware is no more instructed to overwrite
       * components using an element specifier. Instead one has separate
       * 3DSTATE_VF_SGVS (System Generated Value Setup) state packet for it.
       */
      .Component1Control = VFCOMP_STORE_0,
      .Component2Control = VFCOMP_STORE_0,
      .Component3Control = VFCOMP_STORE_0,
   };
   slot++;

   ve[slot] = (struct GENX(VERTEX_ELEMENT_STATE)) {
      .VertexBufferIndex = 0,
      .Valid = true,
      .SourceElementFormat = ISL_FORMAT_R32G32B32_FLOAT,
      .SourceElementOffset = 0,
      .Component0Control = VFCOMP_STORE_SRC,
      .Component1Control = VFCOMP_STORE_SRC,
      .Component2Control = VFCOMP_STORE_SRC,
      .Component3Control = VFCOMP_STORE_1_FP,
   };
   slot++;

   for (unsigned i = 0; i < num_varyings; ++i) {
      ve[slot] = (struct GENX(VERTEX_ELEMENT_STATE)) {
         .VertexBufferIndex = 1,
         .Valid = true,
         .SourceElementFormat = ISL_FORMAT_R32G32B32A32_FLOAT,
         .SourceElementOffset = 16 + i * 4 * sizeof(float),
         .Component0Control = VFCOMP_STORE_SRC,
         .Component1Control = VFCOMP_STORE_SRC,
         .Component2Control = VFCOMP_STORE_SRC,
         .Component3Control = VFCOMP_STORE_SRC,
      };
      slot++;
   }

   const unsigned num_dwords =
      1 + GENX(VERTEX_ELEMENT_STATE_length) * num_elements;
   uint32_t *dw = blorp_emitn(batch, GENX(3DSTATE_VERTEX_ELEMENTS), num_dwords);
   if (!dw)
      return;

   for (unsigned i = 0; i < num_elements; i++) {
      GENX(VERTEX_ELEMENT_STATE_pack)(batch, dw, &ve[i]);
      dw += GENX(VERTEX_ELEMENT_STATE_length);
   }

   blorp_emit(batch, GENX(3DSTATE_VF_STATISTICS), vf) {
      vf.StatisticsEnable = false;
   }

   /* Overwrite Render Target Array Index (2nd dword) in the VUE header with
    * primitive instance identifier. This is used for layered clears.
    */
   blorp_emit(batch, GENX(3DSTATE_VF_SGVS), sgvs) {
      sgvs.InstanceIDEnable = true;
      sgvs.InstanceIDComponentNumber = COMP_1;
      sgvs.InstanceIDElementOffset = 0;
   }

#if GFX_VER >= 11
   blorp_emit(batch, GENX(3DSTATE_VF_SGVS_2), sgvs);
#endif

   for (unsigned i = 0; i < num_elements; i++) {
      blorp_emit(batch, GENX(3DSTATE_VF_INSTANCING), vf) {
         vf.VertexElementIndex = i;
         vf.InstancingEnable = false;
      }
   }

   blorp_emit(batch, GENX(3DSTATE_VF_TOPOLOGY), topo) {
      topo.PrimitiveTopologyType = _3DPRIM_RECTLIST;
   }
}

/* 3DSTATE_VIEWPORT_STATE_POINTERS */
static uint32_t
blorp_emit_cc_viewport(struct blorp_batch *batch)
{
   uint32_t cc_vp_offset;

   /* Somehow reusing CC_VIEWPORT on Gfx9 is causing issues :
    *    https://gitlab.freedesktop.org/mesa/mesa/-/issues/11647
    */
   if (GFX_VER != 9 && batch->blorp->config.use_cached_dynamic_states) {
      cc_vp_offset = blorp_get_dynamic_state(batch, BLORP_DYNAMIC_STATE_CC_VIEWPORT);
   } else {
      blorp_emit_dynamic(batch, GENX(CC_VIEWPORT), vp, 32, &cc_vp_offset) {
         vp.MinimumDepth = batch->blorp->config.use_unrestricted_depth_range ?
                           -FLT_MAX : 0.0;
         vp.MaximumDepth = batch->blorp->config.use_unrestricted_depth_range ?
                           FLT_MAX : 1.0;
      }
   }

   blorp_emit(batch, GENX(3DSTATE_VIEWPORT_STATE_POINTERS_CC), vsp) {
      vsp.CCViewportPointer = cc_vp_offset;
   }

   return cc_vp_offset;
}

static uint32_t
blorp_emit_sampler_state(struct blorp_batch *batch)
{
   uint32_t offset;
   blorp_emit_dynamic(batch, GENX(SAMPLER_STATE), sampler, 32, &offset) {
      sampler.MipModeFilter = MIPFILTER_NONE;
      sampler.MagModeFilter = MAPFILTER_LINEAR;
      sampler.MinModeFilter = MAPFILTER_LINEAR;
      sampler.MinLOD = 0;
      sampler.MaxLOD = 0;
      sampler.TCXAddressControlMode = TCM_CLAMP;
      sampler.TCYAddressControlMode = TCM_CLAMP;
      sampler.TCZAddressControlMode = TCM_CLAMP;
      sampler.MaximumAnisotropy = RATIO21;
      sampler.RAddressMinFilterRoundingEnable = true;
      sampler.RAddressMagFilterRoundingEnable = true;
      sampler.VAddressMinFilterRoundingEnable = true;
      sampler.VAddressMagFilterRoundingEnable = true;
      sampler.UAddressMinFilterRoundingEnable = true;
      sampler.UAddressMagFilterRoundingEnable = true;
      sampler.NonnormalizedCoordinateEnable = true;
   }

   return offset;
}

UNUSED static uint32_t
blorp_emit_sampler_state_ps(struct blorp_batch *batch)
{
   uint32_t offset = batch->blorp->config.use_cached_dynamic_states ?
      blorp_get_dynamic_state(batch, BLORP_DYNAMIC_STATE_SAMPLER) :
      blorp_emit_sampler_state(batch);

   blorp_emit(batch, GENX(3DSTATE_SAMPLER_STATE_POINTERS_PS), ssp) {
      ssp.PointertoPSSamplerState = offset;
   }

   return offset;
}

/* What follows is the code for setting up a "pipeline". */

static void
blorp_emit_vs_config(struct blorp_batch *batch,
                     const struct blorp_params *params)
{
   struct brw_vs_prog_data *vs_prog_data = params->vs_prog_data;
   assert(!vs_prog_data || GFX_VER < 11 ||
          vs_prog_data->base.dispatch_mode == INTEL_DISPATCH_MODE_SIMD8);

   blorp_emit(batch, GENX(3DSTATE_VS), vs) {
      if (vs_prog_data) {
         vs.Enable = true;

         vs.KernelStartPointer = params->vs_prog_kernel;

         vs.DispatchGRFStartRegisterForURBData =
            vs_prog_data->base.base.dispatch_grf_start_reg;
         vs.VertexURBEntryReadLength =
            vs_prog_data->base.urb_read_length;
         vs.VertexURBEntryReadOffset = 0;

         vs.MaximumNumberofThreads =
            batch->blorp->isl_dev->info->max_vs_threads - 1;

         assert(vs_prog_data->base.dispatch_mode == INTEL_DISPATCH_MODE_SIMD8);
#if GFX_VER < 20
         vs.SIMD8DispatchEnable = true;
#endif
      }
   }
}

static void
blorp_emit_sf_config(struct blorp_batch *batch,
                     const struct blorp_params *params,
                     UNUSED enum intel_urb_deref_block_size urb_deref_block_size)
{
   const struct brw_wm_prog_data *prog_data = params->wm_prog_data;

   /* 3DSTATE_SF
    *
    * Disable ViewportTransformEnable (dw2.1)
    *
    * From the SandyBridge PRM, Volume 2, Part 1, Section 1.3, "3D
    * Primitives Overview":
    *     RECTLIST: Viewport Mapping must be DISABLED (as is typical with the
    *     use of screen- space coordinates).
    *
    * A solid rectangle must be rendered, so set FrontFaceFillMode (dw2.4:3)
    * and BackFaceFillMode (dw2.5:6) to SOLID(0).
    *
    * From the Sandy Bridge PRM, Volume 2, Part 1, Section
    * 6.4.1.1 3DSTATE_SF, Field FrontFaceFillMode:
    *     SOLID: Any triangle or rectangle object found to be front-facing
    *     is rendered as a solid object. This setting is required when
    *     (rendering rectangle (RECTLIST) objects.
    */

   blorp_emit(batch, GENX(3DSTATE_SF), sf) {
#if GFX_VER >= 12
      sf.DerefBlockSize = urb_deref_block_size;
#endif
   }

   blorp_emit(batch, GENX(3DSTATE_RASTER), raster) {
      raster.CullMode = CULLMODE_NONE;
   }

   blorp_emit(batch, GENX(3DSTATE_SBE), sbe) {
      sbe.VertexURBEntryReadOffset = 1;
      if (prog_data) {
         sbe.NumberofSFOutputAttributes = prog_data->num_varying_inputs;
         sbe.VertexURBEntryReadLength = brw_blorp_get_urb_length(prog_data);
         sbe.ConstantInterpolationEnable = prog_data->flat_inputs;
      } else {
         sbe.NumberofSFOutputAttributes = 0;
         sbe.VertexURBEntryReadLength = 1;
      }
      sbe.ForceVertexURBEntryReadLength = true;
      sbe.ForceVertexURBEntryReadOffset = true;

      for (unsigned i = 0; i < 32; i++)
         sbe.AttributeActiveComponentFormat[i] = ACF_XYZW;
   }
}

static void
blorp_emit_ps_config(struct blorp_batch *batch,
                     const struct blorp_params *params)
{
   const struct brw_wm_prog_data *prog_data = params->wm_prog_data;

   /* Even when thread dispatch is disabled, max threads (dw5.25:31) must be
    * nonzero to prevent the GPU from hanging.  While the documentation doesn't
    * mention this explicitly, it notes that the valid range for the field is
    * [1,39] = [2,40] threads, which excludes zero.
    *
    * To be safe (and to minimize extraneous code) we go ahead and fully
    * configure the WM state whether or not there is a WM program.
    */

   const struct intel_device_info *devinfo = batch->blorp->compiler->brw->devinfo;

   blorp_emit(batch, GENX(3DSTATE_WM), wm);

   blorp_emit(batch, GENX(3DSTATE_PS), ps) {
      if (params->src.enabled) {
         ps.SamplerCount = 1; /* Up to 4 samplers */
         ps.BindingTableEntryCount = 2;
      } else {
         ps.BindingTableEntryCount = 1;
      }

      /* SAMPLER_STATE prefetching is broken on Gfx11 - Wa_1606682166 */
      if (GFX_VER == 11)
         ps.SamplerCount = 0;

      /* 3DSTATE_PS expects the number of threads per PSD, which is always 64
       * for pre Gfx11 and 128 for gfx11+; On gfx11+ If a programmed value is
       * k, it implies 2(k+1) threads. It implicitly scales for different GT
       * levels (which have some # of PSDs).
       */
      ps.MaximumNumberofThreadsPerPSD = devinfo->max_threads_per_psd - 1;

      switch (params->fast_clear_op) {
      case ISL_AUX_OP_NONE:
         break;
#if GFX_VER < 20
#if GFX_VER >= 10
      case ISL_AUX_OP_AMBIGUATE:
         ps.RenderTargetFastClearEnable = true;
         ps.RenderTargetResolveType = FAST_CLEAR_0;
         break;
#endif /* GFX_VER >= 10 */
      case ISL_AUX_OP_PARTIAL_RESOLVE:
         ps.RenderTargetResolveType = RESOLVE_PARTIAL;
         break;
      case ISL_AUX_OP_FULL_RESOLVE:
         /* WA 1406738321:
          * In-place full resolve of a 3D/Volume surface is not supported.
          * In order to fully resolve 3D/volume surface, copy operation must be
          * performed to a new destination (declared as uncompressed) using the
          * compressed 3D surface as a source.
          */
#if GFX_VERx10 == 120
         assert(params->src.surf.dim != ISL_SURF_DIM_3D);
#endif
         ps.RenderTargetResolveType = RESOLVE_FULL;
         break;
#endif /* GFX_VER < 20 */
      case ISL_AUX_OP_FAST_CLEAR:
         /* WA 1406738321:
          * 3D/Volumetric surfaces do not support Fast Clear operation.
          */
#if GFX_VERx10 == 120
         assert(params->dst.surf.dim != ISL_SURF_DIM_3D);
#endif
         ps.RenderTargetFastClearEnable = true;
         break;
      default:
         unreachable("Invalid fast clear op");
      }

      /* The RENDER_SURFACE_STATE page for TGL says:
       *
       *   For an 8 bpp surface with NUM_MULTISAMPLES = 1, Surface Width not
       *   multiple of 64 pixels and more than 1 mip level in the view, Fast
       *   Clear is not supported when AUX_CCS_E is set in this field.
       *
       * The granularity of a fast-clear or ambiguate operation is likely one
       * CCS element. For an 8 bpp primary surface, this maps to 32px x 4rows.
       * Due to the surface layout parameters, if LOD0's width isn't a
       * multiple of 64px, LOD1 and LOD2+ will share CCS elements. Assert that
       * these operations aren't occurring on these LODs.
       *
       * We don't explicitly check for TGL+ because the restriction is
       * technically applicable to all hardware. Platforms prior to TGL don't
       * support CCS on 8 bpp surfaces. So, these unaligned fast clear
       * operations shouldn't be occurring prior to TGL as well.
       */
      if (isl_format_get_layout(params->dst.surf.format)->bpb == 8 &&
          params->dst.surf.logical_level0_px.width % 64 != 0 &&
          params->dst.surf.levels >= 3 &&
          params->dst.view.base_level >= 1) {
         assert(params->num_samples == 1);
         assert(!ps.RenderTargetFastClearEnable);
      }

      if (prog_data) {
         intel_set_ps_dispatch_state(&ps, devinfo, prog_data,
                                     params->num_samples,
                                     0 /* msaa_flags */);

         ps.DispatchGRFStartRegisterForConstantSetupData0 =
            brw_wm_prog_data_dispatch_grf_start_reg(prog_data, ps, 0);
         ps.DispatchGRFStartRegisterForConstantSetupData1 =
            brw_wm_prog_data_dispatch_grf_start_reg(prog_data, ps, 1);
#if GFX_VER < 20
         ps.DispatchGRFStartRegisterForConstantSetupData2 =
            brw_wm_prog_data_dispatch_grf_start_reg(prog_data, ps, 2);
#endif

         ps.KernelStartPointer0 = params->wm_prog_kernel +
                                  brw_wm_prog_data_prog_offset(prog_data, ps, 0);
         ps.KernelStartPointer1 = params->wm_prog_kernel +
                                  brw_wm_prog_data_prog_offset(prog_data, ps, 1);
#if GFX_VER < 20
         ps.KernelStartPointer2 = params->wm_prog_kernel +
                                  brw_wm_prog_data_prog_offset(prog_data, ps, 2);
#endif
      }
   }

   blorp_emit(batch, GENX(3DSTATE_PS_EXTRA), psx) {
      if (params->src.enabled)
         psx.PixelShaderKillsPixel = true;

      if (prog_data) {
         psx.PixelShaderValid = true;
         psx.PixelShaderComputedDepthMode = prog_data->computed_depth_mode;
         psx.PixelShaderComputesStencil = prog_data->computed_stencil;
         psx.PixelShaderIsPerSample = prog_data->persample_dispatch;

#if INTEL_WA_18038825448_GFX_VER
         psx.EnablePSDependencyOnCPsizeChange =
            batch->flags & BLORP_BATCH_FORCE_CPS_DEPENDENCY;
#endif

#if GFX_VER < 20
         psx.AttributeEnable = prog_data->num_varying_inputs > 0;
#else
         /* Bspec 57340 (r59562):
          *
          *   For MSAA fast clear, it (clear shader) must be in per-pixel
          *   dispatch mode.
          *
          * Bspec 56424 (r58933):
          *
          *   Bit 6 of Bit Group 0: Pixel Shader Is Per Sample
          *   If this bit is DISABLED, the dispatch rate is determined by the
          *   value of Pixel Shader Is Per Coarse Pixel.
          *
          *   Bit 4 of Bit Group 0: Pixel Shader Is Per Coarse Pixel
          *   If Pixel Shader Is Per Sample is DISABLED and this bit is
          *   DISABLED, the pixel shader is dispatched at the per pixel
          *   shading rate.
          *
          * The below assertion ensures the MSAA clear shader is in per-pixel
          * dispatch mode.
          */
         if (params->fast_clear_op == ISL_AUX_OP_FAST_CLEAR &&
             params->num_samples > 1) {
            assert(!psx.PixelShaderIsPerSample &&
                   !psx.PixelShaderIsPerCoarsePixel);
         }
#endif
      }
   }
}

static void
blorp_emit_blend_state(struct blorp_batch *batch,
                       const struct blorp_params *params)
{
   uint32_t offset;
   if (!batch->blorp->config.use_cached_dynamic_states) {
      struct GENX(BLEND_STATE) blend = { };

      const unsigned size = 96;
      uint32_t *state = blorp_alloc_dynamic_state(batch, size, 64, &offset);
      if (state == NULL)
         return;
      uint32_t *pos = state;

      GENX(BLEND_STATE_pack)(NULL, pos, &blend);
      pos += GENX(BLEND_STATE_length);

      for (unsigned i = 0; i < params->num_draw_buffers; ++i) {
         struct GENX(BLEND_STATE_ENTRY) entry = {
            .PreBlendColorClampEnable = true,
            .PostBlendColorClampEnable = true,
            .ColorClampRange = COLORCLAMP_RTFORMAT,

            .WriteDisableRed = params->color_write_disable & 1,
            .WriteDisableGreen = params->color_write_disable & 2,
            .WriteDisableBlue = params->color_write_disable & 4,
            .WriteDisableAlpha = params->color_write_disable & 8,
         };
         GENX(BLEND_STATE_ENTRY_pack)(NULL, pos, &entry);
         pos += GENX(BLEND_STATE_ENTRY_length);
      }

      blorp_flush_range(batch, state, size);
   } else {
      /* We only cached this case. */
      assert(params->color_write_disable == 0);
      offset = blorp_get_dynamic_state(batch, BLORP_DYNAMIC_STATE_BLEND);
   }

   blorp_emit(batch, GENX(3DSTATE_BLEND_STATE_POINTERS), sp) {
      sp.BlendStatePointer = offset;
      sp.BlendStatePointerValid = true;
   }

   blorp_emit(batch, GENX(3DSTATE_PS_BLEND), ps_blend) {
      ps_blend.HasWriteableRT = true;
   }
}

static void
blorp_emit_color_calc_state(struct blorp_batch *batch,
                            UNUSED const struct blorp_params *params)
{
   uint32_t offset;

   if (batch->blorp->config.use_cached_dynamic_states)
      offset = blorp_get_dynamic_state(batch, BLORP_DYNAMIC_STATE_COLOR_CALC);
   else
      blorp_emit_dynamic(batch, GENX(COLOR_CALC_STATE), cc, 64, &offset) {}

   blorp_emit(batch, GENX(3DSTATE_CC_STATE_POINTERS), sp) {
      sp.ColorCalcStatePointer = offset;
      sp.ColorCalcStatePointerValid = true;
   }
}

static void
blorp_emit_depth_stencil_state(struct blorp_batch *batch,
                               const struct blorp_params *params)
{
   blorp_emit(batch, GENX(3DSTATE_WM_DEPTH_STENCIL), ds) {
      if (params->depth.enabled) {
         ds.DepthBufferWriteEnable = true;

         switch (params->hiz_op) {
         /* See the following sections of the Sandy Bridge PRM, Volume 2, Part1:
          *   - 7.5.3.1 Depth Buffer Clear
          *   - 7.5.3.2 Depth Buffer Resolve
          *   - 7.5.3.3 Hierarchical Depth Buffer Resolve
          */
         case ISL_AUX_OP_FULL_RESOLVE:
            ds.DepthTestEnable = true;
            ds.DepthTestFunction = COMPAREFUNCTION_NEVER;
            break;

         case ISL_AUX_OP_NONE:
         case ISL_AUX_OP_FAST_CLEAR:
         case ISL_AUX_OP_AMBIGUATE:
            ds.DepthTestEnable = false;
            break;
         case ISL_AUX_OP_PARTIAL_RESOLVE:
            unreachable("Invalid HIZ op");
         }
      }

      if (params->stencil.enabled) {
         ds.StencilBufferWriteEnable = true;
         ds.StencilTestEnable = true;
         ds.DoubleSidedStencilEnable = false;

         ds.StencilTestFunction = COMPAREFUNCTION_ALWAYS;
         ds.StencilPassDepthPassOp = STENCILOP_REPLACE;

         ds.StencilWriteMask = params->stencil_mask;
         ds.StencilReferenceValue = params->stencil_ref;
      }
   }

#if GFX_VER >= 12
   blorp_emit(batch, GENX(3DSTATE_DEPTH_BOUNDS), db) {
      db.DepthBoundsTestEnable = false;
      db.DepthBoundsTestMinValue = 0.0;
      db.DepthBoundsTestMaxValue = 1.0;
   }
#endif
}

static void
blorp_emit_3dstate_multisample(struct blorp_batch *batch,
                               const struct blorp_params *params)
{
   blorp_emit(batch, GENX(3DSTATE_MULTISAMPLE), ms) {
      ms.NumberofMultisamples       = __builtin_ffs(params->num_samples) - 1;
      ms.PixelLocation              = CENTER;
   }
}

static void
blorp_emit_pipeline(struct blorp_batch *batch,
                    const struct blorp_params *params)
{
   enum intel_urb_deref_block_size urb_deref_block_size;
   emit_urb_config(batch, params, &urb_deref_block_size);

   if (params->wm_prog_data) {
      blorp_emit_blend_state(batch, params);
   }
   blorp_emit_color_calc_state(batch, params);
   blorp_emit_depth_stencil_state(batch, params);

   UNUSED uint32_t mocs = isl_mocs(batch->blorp->isl_dev, 0, false);

#if GFX_VER >= 12
   blorp_emit(batch, GENX(3DSTATE_CONSTANT_ALL), pc) {
      /* Update empty push constants for all stages (bitmask = 11111b) */
      pc.ShaderUpdateEnable = 0x1f;
      pc.MOCS = mocs;
   }
#else
   blorp_emit(batch, GENX(3DSTATE_CONSTANT_VS), xs) { xs.MOCS = mocs; }
   blorp_emit(batch, GENX(3DSTATE_CONSTANT_HS), xs) { xs.MOCS = mocs; }
   blorp_emit(batch, GENX(3DSTATE_CONSTANT_DS), xs) { xs.MOCS = mocs; }
   blorp_emit(batch, GENX(3DSTATE_CONSTANT_GS), xs) { xs.MOCS = mocs; }
   blorp_emit(batch, GENX(3DSTATE_CONSTANT_PS), xs) { xs.MOCS = mocs; }
#endif

   if (params->src.enabled)
      blorp_emit_sampler_state_ps(batch);

   blorp_emit_3dstate_multisample(batch, params);

   blorp_emit(batch, GENX(3DSTATE_SAMPLE_MASK), mask) {
      mask.SampleMask = (1 << params->num_samples) - 1;
   }

   /* From the BSpec, 3D Pipeline > Geometry > Vertex Shader > State,
    * 3DSTATE_VS, Dword 5.0 "VS Function Enable":
    *
    *   [DevSNB] A pipeline flush must be programmed prior to a
    *   3DSTATE_VS command that causes the VS Function Enable to
    *   toggle. Pipeline flush can be executed by sending a PIPE_CONTROL
    *   command with CS stall bit set and a post sync operation.
    *
    * We've already done one at the start of the BLORP operation.
    */
   blorp_emit_vs_config(batch, params);
   blorp_emit(batch, GENX(3DSTATE_HS), hs);
   blorp_emit(batch, GENX(3DSTATE_TE), te);
   blorp_emit(batch, GENX(3DSTATE_DS), DS);
   blorp_emit(batch, GENX(3DSTATE_STREAMOUT), so);
   blorp_emit(batch, GENX(3DSTATE_GS), gs);

   blorp_emit(batch, GENX(3DSTATE_CLIP), clip) {
      clip.PerspectiveDivideDisable = true;
   }

   blorp_emit_sf_config(batch, params, urb_deref_block_size);
   blorp_emit_ps_config(batch, params);

   blorp_emit_cc_viewport(batch);

#if GFX_VER >= 12
   /* Disable Primitive Replication. */
   blorp_emit(batch, GENX(3DSTATE_PRIMITIVE_REPLICATION), pr);
#endif

   if (batch->blorp->config.use_mesh_shading) {
#if GFX_VERx10 >= 125
      blorp_emit(batch, GENX(3DSTATE_MESH_CONTROL), zero);
      blorp_emit(batch, GENX(3DSTATE_TASK_CONTROL), zero);
#endif
   }
}

/******** This is the end of the pipeline setup code ********/

static void
blorp_emit_memcpy(struct blorp_batch *batch,
                  struct blorp_address dst,
                  struct blorp_address src,
                  uint32_t size)
{
   assert(size % 4 == 0);

   for (unsigned dw = 0; dw < size; dw += 4) {
      blorp_emit(batch, GENX(MI_COPY_MEM_MEM), cp) {
         cp.DestinationMemoryAddress = dst;
         cp.SourceMemoryAddress = src;
      }
      dst.offset += 4;
      src.offset += 4;
   }
}

static void
blorp_emit_surface_state(struct blorp_batch *batch,
                         const struct blorp_surface_info *surface,
                         UNUSED enum isl_aux_op aux_op,
                         void *state, uint32_t state_offset,
                         uint8_t color_write_disable,
                         bool is_render_target)
{
   const struct isl_device *isl_dev = batch->blorp->isl_dev;
   struct isl_surf surf = surface->surf;

   if (surf.dim == ISL_SURF_DIM_1D &&
       surf.dim_layout == ISL_DIM_LAYOUT_GFX4_2D) {
      assert(surf.logical_level0_px.height == 1);
      surf.dim = ISL_SURF_DIM_2D;
   }

   if (isl_aux_usage_has_hiz(surface->aux_usage)) {
      /* BLORP doesn't render with depth so we can't use HiZ */
      assert(!is_render_target);
      /* We can't reinterpret HiZ */
      assert(surface->surf.format == surface->view.format);
   }

   enum isl_aux_usage aux_usage = surface->aux_usage;

   /* On gfx12, implicit CCS has no aux buffer */
   bool use_aux_address = (aux_usage != ISL_AUX_USAGE_NONE) &&
                          (surface->aux_addr.buffer != NULL);

   const bool use_clear_address =
      GFX_VER >= 10 && (surface->clear_color_addr.buffer != NULL);

   /* On gfx12 (and optionally on gfx11), hardware will read and write to the
    * clear color address, converting the raw clear color channels to a pixel
    * during a fast-clear. To avoid the restrictions associated with the
    * hardware feature, we instead write a software-converted pixel ourselves.
    * If we're performing a fast-clear, provide a substitute address to avoid
    * a collision with hardware. Outside of gfx11 and gfx12, indirect clear
    * color BOs are not used during fast-clears.
    */
   const struct blorp_address op_clear_addr =
      aux_op == ISL_AUX_OP_FAST_CLEAR ? blorp_get_workaround_address(batch) :
                                        surface->clear_color_addr;

   isl_surf_fill_state(batch->blorp->isl_dev, state,
                       .surf = &surf, .view = &surface->view,
                       .aux_surf = &surface->aux_surf, .aux_usage = aux_usage,
                       .address =
                          blorp_get_surface_address(batch, surface->addr),
                       .aux_address = !use_aux_address ? 0 :
                          blorp_get_surface_address(batch, surface->aux_addr),
                       .clear_address = !use_clear_address ? 0 :
                          blorp_get_surface_address(batch, op_clear_addr),
                       .mocs = surface->addr.mocs,
                       .clear_color = surface->clear_color,
                       .use_clear_address = use_clear_address);

   blorp_surface_reloc(batch, state_offset + isl_dev->ss.addr_offset,
                       surface->addr, 0);

   if (use_aux_address) {
      /* On gfx7 and prior, the bottom 12 bits of the MCS base address are
       * used to store other information.  This should be ok, however, because
       * surface buffer addresses are always 4K page alinged.
       */
      assert((surface->aux_addr.offset & 0xfff) == 0);
      uint32_t *aux_addr = state + isl_dev->ss.aux_addr_offset;
      blorp_surface_reloc(batch, state_offset + isl_dev->ss.aux_addr_offset,
                          surface->aux_addr, *aux_addr);
   }

   if (aux_usage != ISL_AUX_USAGE_NONE && surface->clear_color_addr.buffer) {
#if GFX_VER >= 10
      assert((surface->clear_color_addr.offset & 0x3f) == 0);
      uint32_t *clear_addr = state + isl_dev->ss.clear_color_state_offset;
      blorp_surface_reloc(batch, state_offset +
                          isl_dev->ss.clear_color_state_offset,
                          op_clear_addr, *clear_addr);
#else
      /* Fast clears just whack the AUX surface and don't actually use the
       * clear color for anything.  We can avoid the MI memcpy on that case.
       */
      if (aux_op != ISL_AUX_OP_FAST_CLEAR) {
         struct blorp_address dst_addr = blorp_get_surface_base_address(batch);
         dst_addr.offset += state_offset + isl_dev->ss.clear_value_offset;
         blorp_emit_memcpy(batch, dst_addr, surface->clear_color_addr,
                           isl_dev->ss.clear_value_size);
      }
#endif
   }

   blorp_flush_range(batch, state, GENX(RENDER_SURFACE_STATE_length) * 4);
}

static void
blorp_emit_null_surface_state(struct blorp_batch *batch,
                              const struct blorp_surface_info *surface,
                              uint32_t *state)
{
   struct GENX(RENDER_SURFACE_STATE) ss = {
      .SurfaceType = SURFTYPE_NULL,
      .SurfaceFormat = ISL_FORMAT_R8G8B8A8_UNORM,
      .Width = surface->surf.logical_level0_px.width - 1,
      .Height = surface->surf.logical_level0_px.height - 1,
      .MIPCountLOD = surface->view.base_level,
      .MinimumArrayElement = surface->view.base_array_layer,
      .Depth = surface->view.array_len - 1,
      .RenderTargetViewExtent = surface->view.array_len - 1,
      .NumberofMultisamples = ffs(surface->surf.samples) - 1,
      .MOCS = isl_mocs(batch->blorp->isl_dev, 0, false),

      .SurfaceArray = surface->surf.dim != ISL_SURF_DIM_3D,

#if GFX_VERx10 >= 125
      .TileMode = TILE4,
#else
      .TileMode = YMAJOR,
#endif
   };

   GENX(RENDER_SURFACE_STATE_pack)(NULL, state, &ss);

   blorp_flush_range(batch, state, GENX(RENDER_SURFACE_STATE_length) * 4);
}

static uint32_t
blorp_setup_binding_table(struct blorp_batch *batch,
                           const struct blorp_params *params)
{
   const struct isl_device *isl_dev = batch->blorp->isl_dev;
   uint32_t surface_offsets[2], bind_offset = 0;
   void *surface_maps[2];

   if (params->use_pre_baked_binding_table) {
      bind_offset = params->pre_baked_binding_table_offset;
   } else {
      unsigned num_surfaces = 1 + params->src.enabled;
      if (!blorp_alloc_binding_table(batch, num_surfaces,
                                     isl_dev->ss.size, isl_dev->ss.align,
                                     &bind_offset, surface_offsets, surface_maps))
         return 0;

      if (params->dst.enabled) {
         blorp_emit_surface_state(batch, &params->dst,
                                  params->fast_clear_op,
                                  surface_maps[BLORP_RENDERBUFFER_BT_INDEX],
                                  surface_offsets[BLORP_RENDERBUFFER_BT_INDEX],
                                  params->color_write_disable, true);
      } else {
         assert(params->depth.enabled || params->stencil.enabled);
         const struct blorp_surface_info *surface =
            params->depth.enabled ? &params->depth : &params->stencil;
         blorp_emit_null_surface_state(batch, surface,
                                       surface_maps[BLORP_RENDERBUFFER_BT_INDEX]);
      }

      if (params->src.enabled) {
         blorp_emit_surface_state(batch, &params->src,
                                  params->fast_clear_op,
                                  surface_maps[BLORP_TEXTURE_BT_INDEX],
                                  surface_offsets[BLORP_TEXTURE_BT_INDEX],
                                  0, false);
      }
   }

   return bind_offset;
}

static void
blorp_emit_btp(struct blorp_batch *batch, uint32_t bind_offset)
{
   blorp_emit(batch, GENX(3DSTATE_BINDING_TABLE_POINTERS_VS), bt);
   blorp_emit(batch, GENX(3DSTATE_BINDING_TABLE_POINTERS_HS), bt);
   blorp_emit(batch, GENX(3DSTATE_BINDING_TABLE_POINTERS_DS), bt);
   blorp_emit(batch, GENX(3DSTATE_BINDING_TABLE_POINTERS_GS), bt);

   blorp_emit(batch, GENX(3DSTATE_BINDING_TABLE_POINTERS_PS), bt) {
      bt.PointertoPSBindingTable =
         blorp_binding_table_offset_to_pointer(batch, bind_offset);
   }
}

static void
blorp_emit_depth_stencil_config(struct blorp_batch *batch,
                                const struct blorp_params *params)
{
   const struct isl_device *isl_dev = batch->blorp->isl_dev;
   const struct intel_device_info *devinfo =
      batch->blorp->compiler->brw->devinfo;

   uint32_t *dw = blorp_emit_dwords(batch, isl_dev->ds.size / 4);
   if (dw == NULL)
      return;

   struct isl_depth_stencil_hiz_emit_info info = { };

   if (params->depth.enabled) {
      info.view = &params->depth.view;
      info.mocs = params->depth.addr.mocs;
   } else if (params->stencil.enabled) {
      info.view = &params->stencil.view;
      info.mocs = params->stencil.addr.mocs;
   } else {
      info.mocs = isl_mocs(isl_dev, 0, false);
   }

   if (params->depth.enabled) {
      info.depth_surf = &params->depth.surf;

      info.depth_address =
         blorp_emit_reloc(batch, dw + isl_dev->ds.depth_offset / 4,
                          params->depth.addr, 0);

      info.hiz_usage = params->depth.aux_usage;
      if (isl_aux_usage_has_hiz(info.hiz_usage)) {
         info.hiz_surf = &params->depth.aux_surf;

         struct blorp_address hiz_address = params->depth.aux_addr;

         info.hiz_address =
            blorp_emit_reloc(batch, dw + isl_dev->ds.hiz_offset / 4,
                             hiz_address, 0);

         info.depth_clear_value = params->depth.clear_color.f32[0];
      }
   }

   if (params->stencil.enabled) {
      info.stencil_surf = &params->stencil.surf;

      info.stencil_aux_usage = params->stencil.aux_usage;
      struct blorp_address stencil_address = params->stencil.addr;

      info.stencil_address =
         blorp_emit_reloc(batch, dw + isl_dev->ds.stencil_offset / 4,
                          stencil_address, 0);
   }

   isl_emit_depth_stencil_hiz_s(isl_dev, dw, &info);

   if (intel_needs_workaround(devinfo, 1408224581) ||
       intel_needs_workaround(devinfo, 14014097488) ||
       intel_needs_workaround(devinfo, 14016712196)) {
      /* Wa_1408224581
       *
       * Workaround: Gfx12LP Astep only An additional pipe control with
       * post-sync = store dword operation would be required.( w/a is to
       * have an additional pipe control after the stencil state whenever
       * the surface state bits of this state is changing).
       *
       * This also seems sufficient to handle Wa_14014097488 and
       * Wa_14016712196.
       */
      blorp_emit(batch, GENX(PIPE_CONTROL), pc) {
         pc.PostSyncOperation = WriteImmediateData;
         pc.Address = blorp_get_workaround_address(batch);
      }
   }
}

/* Emits the Optimized HiZ sequence specified in the BDW+ PRMs. The
 * depth/stencil buffer extents are ignored to handle APIs which perform
 * clearing operations without such information.
 * */
static void
blorp_emit_gfx8_hiz_op(struct blorp_batch *batch,
                       const struct blorp_params *params)
{
   /* We should be performing an operation on a depth or stencil buffer.
    */
   assert(params->depth.enabled || params->stencil.enabled);

   blorp_measure_start(batch, params);

   /* The stencil buffer should only be enabled if a fast clear operation is
    * requested.
    */
   if (params->stencil.enabled)
      assert(params->hiz_op == ISL_AUX_OP_FAST_CLEAR);

   /* From the BDW PRM Volume 2, 3DSTATE_WM_HZ_OP:
    *
    * 3DSTATE_MULTISAMPLE packet must be used prior to this packet to change
    * the Number of Multisamples. This packet must not be used to change
    * Number of Multisamples in a rendering sequence.
    *
    * Since HIZ may be the first thing in a batch buffer, play safe and always
    * emit 3DSTATE_MULTISAMPLE.
    */
   blorp_emit_3dstate_multisample(batch, params);

   /* From the BDW PRM Volume 7, Depth Buffer Clear:
    *
    *    The clear value must be between the min and max depth values
    *    (inclusive) defined in the CC_VIEWPORT. If the depth buffer format is
    *    D32_FLOAT, then +/-DENORM values are also allowed.
    *
    * Set the bounds to match our hardware limits.
    */
   if (params->depth.enabled && params->hiz_op == ISL_AUX_OP_FAST_CLEAR)
      blorp_emit_cc_viewport(batch);

   /* According to the SKL PRM formula for WM_INT::ThreadDispatchEnable, the
    * 3DSTATE_WM::ForceThreadDispatchEnable field can force WM thread dispatch
    * even when WM_HZ_OP is active.  However, WM thread dispatch is normally
    * disabled for HiZ ops and it appears that force-enabling it can lead to
    * GPU hangs on at least Skylake.  Since we don't know the current state of
    * the 3DSTATE_WM packet, just emit a dummy one prior to 3DSTATE_WM_HZ_OP.
    */
   blorp_emit(batch, GENX(3DSTATE_WM), wm);

   /* If we can't alter the depth stencil config and multiple layers are
    * involved, the HiZ op will fail. This is because the op requires that a
    * new config is emitted for each additional layer.
    */
   if (batch->flags & BLORP_BATCH_NO_EMIT_DEPTH_STENCIL) {
      assert(params->num_layers <= 1);
   } else {
      blorp_emit_depth_stencil_config(batch, params);
   }

   /* TODO - If we ever start using 3DSTATE_WM_HZ_OP::StencilBufferResolveEnable
    * we need to implement required steps, flushes documented in Wa_1605967699.
    */
   blorp_emit(batch, GENX(3DSTATE_WM_HZ_OP), hzp) {
      switch (params->hiz_op) {
      case ISL_AUX_OP_FAST_CLEAR:
         hzp.StencilBufferClearEnable = params->stencil.enabled;
         hzp.DepthBufferClearEnable = params->depth.enabled;
         hzp.StencilClearValue = params->stencil_ref;
         hzp.FullSurfaceDepthandStencilClear = params->full_surface_hiz_op;
#if GFX_VER >= 20
         hzp.DepthClearValue = params->depth.clear_color.f32[0];

         /* From the Xe2 Bspec 56437 (r61349):
          *
          *    The Depth Clear value cannot be a NAN (Not-A-Number) if the
          *    depth format is Float32.
          *
          * We're not required to support NaN in APIs, so flush to zero.
          */
         if (util_is_nan(hzp.DepthClearValue))
            hzp.DepthClearValue = 0;
#endif
         break;
      case ISL_AUX_OP_FULL_RESOLVE:
         assert(params->full_surface_hiz_op);
         hzp.DepthBufferResolveEnable = true;
         break;
      case ISL_AUX_OP_AMBIGUATE:
         assert(params->full_surface_hiz_op);
         hzp.HierarchicalDepthBufferResolveEnable = true;
         break;
      case ISL_AUX_OP_PARTIAL_RESOLVE:
      case ISL_AUX_OP_NONE:
         unreachable("Invalid HIZ op");
      }

      hzp.NumberofMultisamples = ffs(params->num_samples) - 1;
      hzp.SampleMask = 0xFFFF;

      /* Due to a hardware issue, this bit MBZ */
      assert(hzp.ScissorRectangleEnable == false);

      /* Contrary to the HW docs both fields are inclusive */
      hzp.ClearRectangleXMin = params->x0;
      hzp.ClearRectangleYMin = params->y0;

      /* Contrary to the HW docs both fields are exclusive */
      hzp.ClearRectangleXMax = params->x1;
      hzp.ClearRectangleYMax = params->y1;
   }

   /* PIPE_CONTROL w/ all bits clear except for “Post-Sync Operation” must set
    * to “Write Immediate Data” enabled.
    */
   blorp_emit(batch, GENX(PIPE_CONTROL), pc) {
      pc.PostSyncOperation = WriteImmediateData;
      pc.Address = blorp_get_workaround_address(batch);
   }

   blorp_emit(batch, GENX(3DSTATE_WM_HZ_OP), hzp);

   blorp_measure_end(batch, params);
}

static bool
blorp_uses_bti_rt_writes(const struct blorp_batch *batch, const struct blorp_params *params)
{
   if (batch->flags & (BLORP_BATCH_USE_BLITTER | BLORP_BATCH_USE_COMPUTE))
      return false;

   /* HIZ clears use WM_HZ ops rather than a clear shader using RT writes. */
   return params->hiz_op == ISL_AUX_OP_NONE;
}

static void
blorp_exec_3d(struct blorp_batch *batch, const struct blorp_params *params)
{
   if (params->hiz_op != ISL_AUX_OP_NONE) {
      blorp_emit_gfx8_hiz_op(batch, params);
      return;
   }

   blorp_emit_vertex_buffers(batch, params);
   blorp_emit_vertex_elements(batch, params);

   blorp_emit_pipeline(batch, params);

   blorp_emit_btp(batch, blorp_setup_binding_table(batch, params));

   if (!(batch->flags & BLORP_BATCH_NO_EMIT_DEPTH_STENCIL))
      blorp_emit_depth_stencil_config(batch, params);

   const UNUSED bool use_tbimr = false;
   blorp_emit_pre_draw(batch, params);
   blorp_emit(batch, GENX(3DPRIMITIVE), prim) {
      prim.VertexAccessType = SEQUENTIAL;
      prim.PrimitiveTopologyType = _3DPRIM_RECTLIST;
      prim.PredicateEnable = batch->flags & BLORP_BATCH_PREDICATE_ENABLE;
#if GFX_VERx10 >= 125
      prim.TBIMREnable = use_tbimr;
#endif
      prim.VertexCountPerInstance = 3;
      prim.InstanceCount = params->num_layers;
   }
   blorp_emit_post_draw(batch, params);
}

static void
blorp_get_compute_push_const(struct blorp_batch *batch,
                             const struct blorp_params *params,
                             uint32_t threads,
                             uint32_t *state_offset,
                             unsigned *state_size)
{
   const struct brw_cs_prog_data *cs_prog_data = params->cs_prog_data;
   const unsigned push_const_size =
      ALIGN(brw_cs_push_const_total_size(cs_prog_data, threads), 64);
   assert(cs_prog_data->push.cross_thread.size +
          cs_prog_data->push.per_thread.size == sizeof(params->wm_inputs));

   if (push_const_size == 0) {
      *state_offset = 0;
      *state_size = 0;
      return;
   }

   uint32_t push_const_offset;
   uint32_t *push_const =
      GFX_VERx10 >= 125 ?
      blorp_alloc_general_state(batch, push_const_size, 64,
                                &push_const_offset) :
      blorp_alloc_dynamic_state(batch, push_const_size, 64,
                                &push_const_offset);
   if (push_const == NULL) {
      *state_offset = 0;
      *state_size = 0;
      return;
   }
   memset(push_const, 0x0, push_const_size);

   void *dst = push_const;
   const void *src = (char *)&params->wm_inputs;

   if (cs_prog_data->push.cross_thread.size > 0) {
      memcpy(dst, src, cs_prog_data->push.cross_thread.size);
      dst += cs_prog_data->push.cross_thread.size;
      src += cs_prog_data->push.cross_thread.size;
   }

   assert(GFX_VERx10 < 125 || cs_prog_data->push.per_thread.size == 0);
#if GFX_VERx10 < 125
   if (cs_prog_data->push.per_thread.size > 0) {
      for (unsigned t = 0; t < threads; t++) {
         memcpy(dst, src, (cs_prog_data->push.per_thread.dwords - 1) * 4);

         uint32_t *subgroup_id = dst + cs_prog_data->push.per_thread.size - 4;
         *subgroup_id = t;

         dst += cs_prog_data->push.per_thread.size;
      }
   }
#endif

   *state_offset = push_const_offset;
   *state_size = push_const_size;
}

static void
blorp_exec_compute(struct blorp_batch *batch, const struct blorp_params *params)
{
   assert(!(batch->flags & BLORP_BATCH_PREDICATE_ENABLE));
   assert(params->hiz_op == ISL_AUX_OP_NONE);

   blorp_measure_start(batch, params);

   const struct intel_device_info *devinfo = batch->blorp->compiler->brw->devinfo;
   const struct brw_cs_prog_data *cs_prog_data = params->cs_prog_data;
   const struct brw_stage_prog_data *prog_data = &cs_prog_data->base;
   const struct intel_cs_dispatch_info dispatch =
      brw_cs_get_dispatch_info(devinfo, cs_prog_data, NULL);

   uint32_t group_x0 = params->x0 / cs_prog_data->local_size[0];
   uint32_t group_y0 = params->y0 / cs_prog_data->local_size[1];
   uint32_t group_z0 = params->dst.z_offset;
   uint32_t group_x1 = DIV_ROUND_UP(params->x1, cs_prog_data->local_size[0]);
   uint32_t group_y1 = DIV_ROUND_UP(params->y1, cs_prog_data->local_size[1]);
   assert(params->num_layers >= 1);
   uint32_t group_z1 = params->dst.z_offset + params->num_layers;
   assert(cs_prog_data->local_size[2] == 1);

#if GFX_VERx10 >= 125
   assert(cs_prog_data->push.per_thread.regs == 0);
   blorp_emit(batch, GENX(COMPUTE_WALKER), cw) {
      cw.SIMDSize                       = dispatch.simd_size / 16;
      cw.MessageSIMD                    = dispatch.simd_size / 16,
      cw.LocalXMaximum                  = cs_prog_data->local_size[0] - 1;
      cw.LocalYMaximum                  = cs_prog_data->local_size[1] - 1;
      cw.LocalZMaximum                  = cs_prog_data->local_size[2] - 1;
      cw.ThreadGroupIDStartingX         = group_x0;
      cw.ThreadGroupIDStartingY         = group_y0;
      cw.ThreadGroupIDStartingZ         = group_z0;
      cw.ThreadGroupIDXDimension        = group_x1;
      cw.ThreadGroupIDYDimension        = group_y1;
      cw.ThreadGroupIDZDimension        = group_z1;
      cw.ExecutionMask                  = 0xffffffff;
      cw.PostSync.MOCS                  = isl_mocs(batch->blorp->isl_dev, 0, false);

      uint32_t surfaces_offset = blorp_setup_binding_table(batch, params);

      uint32_t samplers_offset =
         params->src.enabled ? blorp_emit_sampler_state(batch) : 0;

      uint32_t push_const_offset;
      unsigned push_const_size;
      blorp_get_compute_push_const(batch, params, dispatch.threads,
                                   &push_const_offset, &push_const_size);
      cw.IndirectDataStartAddress       = push_const_offset;
      cw.IndirectDataLength             = push_const_size;

#if GFX_VERx10 >= 125
      cw.GenerateLocalID                = cs_prog_data->generate_local_id != 0;
      cw.EmitLocal                      = cs_prog_data->generate_local_id;
      cw.WalkOrder                      = cs_prog_data->walk_order;
      cw.TileLayout = cs_prog_data->walk_order == INTEL_WALK_ORDER_YXZ ?
                      TileY32bpe : Linear;
#endif

      cw.InterfaceDescriptor = (struct GENX(INTERFACE_DESCRIPTOR_DATA)) {
         .KernelStartPointer = params->cs_prog_kernel,
         .SamplerStatePointer = samplers_offset,
         .SamplerCount = params->src.enabled ? 1 : 0,
         .BindingTableEntryCount = params->src.enabled ? 2 : 1,
         .BindingTablePointer = surfaces_offset,
         .NumberofThreadsinGPGPUThreadGroup = dispatch.threads,
         .SharedLocalMemorySize =
            intel_compute_slm_encode_size(GFX_VER, prog_data->total_shared),
         .PreferredSLMAllocationSize =
            intel_compute_preferred_slm_calc_encode_size(devinfo,
                                                         prog_data->total_shared,
                                                         dispatch.group_size,
                                                         dispatch.simd_size),
         .NumberOfBarriers = cs_prog_data->uses_barrier,
      };
   }

#else

   /* The MEDIA_VFE_STATE documentation for Gfx8+ says:
    *
    * "A stalling PIPE_CONTROL is required before MEDIA_VFE_STATE unless
    *  the only bits that are changed are scoreboard related: Scoreboard
    *  Enable, Scoreboard Type, Scoreboard Mask, Scoreboard * Delta. For
    *  these scoreboard related states, a MEDIA_STATE_FLUSH is sufficient."
    *
    * Earlier generations say "MI_FLUSH" instead of "stalling PIPE_CONTROL",
    * but MI_FLUSH isn't really a thing, so we assume they meant PIPE_CONTROL.
    */
   blorp_emit(batch, GENX(PIPE_CONTROL), pc) {
      pc.CommandStreamerStallEnable = true;
      pc.StallAtPixelScoreboard = true;
   }

   blorp_emit(batch, GENX(MEDIA_VFE_STATE), vfe) {
      assert(prog_data->total_scratch == 0);
      vfe.MaximumNumberofThreads =
         devinfo->max_cs_threads * devinfo->subslice_total - 1;
      vfe.NumberofURBEntries = 2;
#if GFX_VER < 11
      vfe.ResetGatewayTimer =
         Resettingrelativetimerandlatchingtheglobaltimestamp;
#endif
      vfe.URBEntryAllocationSize = 2;

      const uint32_t vfe_curbe_allocation =
         ALIGN(cs_prog_data->push.per_thread.regs * dispatch.threads +
               cs_prog_data->push.cross_thread.regs, 2);
      vfe.CURBEAllocationSize = vfe_curbe_allocation;
   }

   uint32_t push_const_offset;
   unsigned push_const_size;
   blorp_get_compute_push_const(batch, params, dispatch.threads,
                                &push_const_offset, &push_const_size);

   blorp_emit(batch, GENX(MEDIA_CURBE_LOAD), curbe) {
      curbe.CURBETotalDataLength = push_const_size;
      curbe.CURBEDataStartAddress = push_const_offset;
   }

   uint32_t surfaces_offset = blorp_setup_binding_table(batch, params);

   uint32_t samplers_offset =
      params->src.enabled ? blorp_emit_sampler_state(batch) : 0;

   struct GENX(INTERFACE_DESCRIPTOR_DATA) idd = {
      .KernelStartPointer = params->cs_prog_kernel,
      .SamplerStatePointer = samplers_offset,
      .SamplerCount = params->src.enabled ? 1 : 0,
      .BindingTableEntryCount = params->src.enabled ? 2 : 1,
      .BindingTablePointer = surfaces_offset,
      .ConstantURBEntryReadLength = cs_prog_data->push.per_thread.regs,
      .NumberofThreadsinGPGPUThreadGroup = dispatch.threads,
      .SharedLocalMemorySize = intel_compute_slm_encode_size(GFX_VER,
                                                             prog_data->total_shared),
      .BarrierEnable = cs_prog_data->uses_barrier,
      .CrossThreadConstantDataReadLength =
         cs_prog_data->push.cross_thread.regs,
   };

   uint32_t idd_offset;
   uint32_t size = GENX(INTERFACE_DESCRIPTOR_DATA_length) * sizeof(uint32_t);
   void *state = blorp_alloc_dynamic_state(batch, size, 64, &idd_offset);
   if (state == NULL)
      return;
   GENX(INTERFACE_DESCRIPTOR_DATA_pack)(NULL, state, &idd);

   blorp_emit(batch, GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD), mid) {
      mid.InterfaceDescriptorTotalLength        = size;
      mid.InterfaceDescriptorDataStartAddress   = idd_offset;
   }

   blorp_emit(batch, GENX(GPGPU_WALKER), ggw) {
      ggw.SIMDSize                     = dispatch.simd_size / 16;
      ggw.ThreadDepthCounterMaximum    = 0;
      ggw.ThreadHeightCounterMaximum   = 0;
      ggw.ThreadWidthCounterMaximum    = dispatch.threads - 1;
      ggw.ThreadGroupIDStartingX       = group_x0;
      ggw.ThreadGroupIDStartingY       = group_y0;
      ggw.ThreadGroupIDStartingResumeZ = group_z0;
      ggw.ThreadGroupIDXDimension      = group_x1;
      ggw.ThreadGroupIDYDimension      = group_y1;
      ggw.ThreadGroupIDZDimension      = group_z1;
      ggw.RightExecutionMask           = dispatch.right_mask;
      ggw.BottomExecutionMask          = 0xffffffff;
   }

#endif

   blorp_measure_end(batch, params);
}

/* -----------------------------------------------------------------------
 * -- BLORP on blitter
 * -----------------------------------------------------------------------
 */

#include "isl/isl_genX_helpers.h"

#if GFX_VER >= 12
static uint32_t
xy_bcb_tiling(const struct isl_surf *surf)
{
   switch (surf->tiling) {
   case ISL_TILING_LINEAR:
      return XY_TILE_LINEAR;
#if GFX_VERx10 >= 125
   case ISL_TILING_X:
      return XY_TILE_X;
   case ISL_TILING_4:
      return XY_TILE_4;
   case ISL_TILING_64:
   case ISL_TILING_64_XE2:
      return XY_TILE_64;
#else
   case ISL_TILING_Y0:
      return XY_TILE_Y;
#endif
   default:
      unreachable("Invalid tiling for XY_BLOCK_COPY_BLT");
   }
}

static uint32_t
xy_color_depth(const struct isl_format_layout *fmtl)
{
   switch (fmtl->bpb) {
   case 128: return XY_BPP_128_BIT;
   case  96: return XY_BPP_96_BIT;
   case  64: return XY_BPP_64_BIT;
   case  32: return XY_BPP_32_BIT;
   case  16: return XY_BPP_16_BIT;
   case   8: return XY_BPP_8_BIT;
   default:
      unreachable("Invalid bpp");
   }
}
#endif

#if GFX_VERx10 >= 125
static uint32_t
xy_bcb_surf_dim(const struct isl_surf *surf)
{
   switch (surf->dim) {
   case ISL_SURF_DIM_1D:
      return XY_SURFTYPE_1D;
   case ISL_SURF_DIM_2D:
      return XY_SURFTYPE_2D;
   case ISL_SURF_DIM_3D:
      return XY_SURFTYPE_3D;
   default:
      unreachable("Invalid dimensionality for XY_BLOCK_COPY_BLT");
   }
}

static uint32_t
xy_bcb_surf_depth(const struct isl_surf *surf)
{
   return surf->dim == ISL_SURF_DIM_3D ? surf->logical_level0_px.depth
                                       : surf->logical_level0_px.array_len;
}

#if GFX_VER < 20
static uint32_t
xy_aux_mode(const struct blorp_surface_info *info)
{
   switch (info->aux_usage) {
   case ISL_AUX_USAGE_CCS_E:
   case ISL_AUX_USAGE_FCV_CCS_E:
   case ISL_AUX_USAGE_STC_CCS:
      return XY_CCS_E;
   case ISL_AUX_USAGE_NONE:
      return XY_NONE;
   default:
      unreachable("Unsupported aux mode");
   }
}
#endif // GFX_VER < 20
#endif // GFX_VERx10 >= 125

UNUSED static void
blorp_xy_block_copy_blt(struct blorp_batch *batch,
                        const struct blorp_params *params)
{
#if GFX_VER < 12
   unreachable("Blitter is only supported on Gfx12+");
#else
   UNUSED const struct isl_device *isl_dev = batch->blorp->isl_dev;

   assert(batch->flags & BLORP_BATCH_USE_BLITTER);
   assert(!(batch->flags & BLORP_BATCH_PREDICATE_ENABLE));
   assert(params->hiz_op == ISL_AUX_OP_NONE);

   assert(params->num_layers == 1);
   assert(params->dst.view.levels == 1);
   assert(params->src.view.levels == 1);

#if GFX_VERx10 < 125
   assert(params->dst.view.base_array_layer == 0);
   assert(params->dst.z_offset == 0);
#endif

   unsigned dst_x0 = params->x0;
   unsigned dst_x1 = params->x1;
   unsigned src_x0 =
      dst_x0 - params->wm_inputs.coord_transform[0].offset;
   ASSERTED unsigned src_x1 =
      dst_x1 - params->wm_inputs.coord_transform[0].offset;
   unsigned dst_y0 = params->y0;
   unsigned dst_y1 = params->y1;
   unsigned src_y0 =
      dst_y0 - params->wm_inputs.coord_transform[1].offset;
   ASSERTED unsigned src_y1 =
      dst_y1 - params->wm_inputs.coord_transform[1].offset;

   assert(src_x1 - src_x0 == dst_x1 - dst_x0);
   assert(src_y1 - src_y0 == dst_y1 - dst_y0);

   const struct isl_surf *src_surf = &params->src.surf;
   const struct isl_surf *dst_surf = &params->dst.surf;

   const struct isl_format_layout *fmtl =
      isl_format_get_layout(params->dst.view.format);

   if (fmtl->bpb == 96) {
      assert(src_surf->tiling == ISL_TILING_LINEAR &&
             dst_surf->tiling == ISL_TILING_LINEAR);
   }

   assert(src_surf->samples == 1);
   assert(dst_surf->samples == 1);

   unsigned dst_pitch_unit = dst_surf->tiling == ISL_TILING_LINEAR ? 1 : 4;
   unsigned src_pitch_unit = src_surf->tiling == ISL_TILING_LINEAR ? 1 : 4;

#if GFX_VERx10 >= 125
   struct isl_extent3d src_align = isl_get_image_alignment(src_surf);
   struct isl_extent3d dst_align = isl_get_image_alignment(dst_surf);
#endif

   blorp_emit(batch, GENX(XY_BLOCK_COPY_BLT), blt) {
      blt.ColorDepth = xy_color_depth(fmtl);

      blt.DestinationPitch = (dst_surf->row_pitch_B / dst_pitch_unit) - 1;
      blt.DestinationMOCS = params->dst.addr.mocs;
      blt.DestinationTiling = xy_bcb_tiling(dst_surf);
      blt.DestinationX1 = dst_x0;
      blt.DestinationY1 = dst_y0;
      blt.DestinationX2 = dst_x1;
      blt.DestinationY2 = dst_y1;
      blt.DestinationBaseAddress = params->dst.addr;
      blt.DestinationXOffset = params->dst.tile_x_sa;
      blt.DestinationYOffset = params->dst.tile_y_sa;

#if GFX_VERx10 >= 125
      blt.DestinationSurfaceType = xy_bcb_surf_dim(dst_surf);
      blt.DestinationSurfaceWidth = dst_surf->logical_level0_px.w - 1;
      blt.DestinationSurfaceHeight = dst_surf->logical_level0_px.h - 1;
      blt.DestinationSurfaceDepth = xy_bcb_surf_depth(dst_surf) - 1;
      blt.DestinationArrayIndex =
         params->dst.view.base_array_layer + params->dst.z_offset;
      blt.DestinationSurfaceQPitch = isl_get_qpitch(dst_surf) >> 2;
      blt.DestinationLOD = params->dst.view.base_level;
      blt.DestinationMipTailStartLOD = dst_surf->miptail_start_level;
      blt.DestinationHorizontalAlign = isl_encode_halign(dst_align.width);
      blt.DestinationVerticalAlign = isl_encode_valign(dst_align.height);
#if GFX_VER < 20
      /* XY_BLOCK_COPY_BLT only supports AUX_CCS. */
      blt.DestinationDepthStencilResource =
         params->dst.aux_usage == ISL_AUX_USAGE_STC_CCS;
#endif
      blt.DestinationTargetMemory =
         params->dst.addr.local_hint ? XY_MEM_LOCAL : XY_MEM_SYSTEM;

      if (params->dst.aux_usage != ISL_AUX_USAGE_NONE) {
#if GFX_VER < 20
         blt.DestinationAuxiliarySurfaceMode = xy_aux_mode(&params->dst);
         blt.DestinationCompressionEnable = true;
#endif
         blt.DestinationCompressionFormat =
            isl_get_render_compression_format(dst_surf->format);
         blt.DestinationClearValueEnable = !!params->dst.clear_color_addr.buffer;
         blt.DestinationClearAddress = params->dst.clear_color_addr;
      }
#endif

      blt.SourceX1 = src_x0;
      blt.SourceY1 = src_y0;
      blt.SourcePitch = (src_surf->row_pitch_B / src_pitch_unit) - 1;
      blt.SourceMOCS = params->src.addr.mocs;
      blt.SourceTiling = xy_bcb_tiling(src_surf);
      blt.SourceBaseAddress = params->src.addr;
      blt.SourceXOffset = params->src.tile_x_sa;
      blt.SourceYOffset = params->src.tile_y_sa;

#if GFX_VERx10 >= 125
      blt.SourceSurfaceType = xy_bcb_surf_dim(src_surf);
      blt.SourceSurfaceWidth = src_surf->logical_level0_px.w - 1;
      blt.SourceSurfaceHeight = src_surf->logical_level0_px.h - 1;
      blt.SourceSurfaceDepth = xy_bcb_surf_depth(src_surf) - 1;
      blt.SourceArrayIndex =
         params->src.view.base_array_layer + params->src.z_offset;
      blt.SourceSurfaceQPitch = isl_get_qpitch(src_surf) >> 2;
      blt.SourceLOD = params->src.view.base_level;
      blt.SourceMipTailStartLOD = src_surf->miptail_start_level;
      blt.SourceHorizontalAlign = isl_encode_halign(src_align.width);
      blt.SourceVerticalAlign = isl_encode_valign(src_align.height);
#if GFX_VER < 20
      /* XY_BLOCK_COPY_BLT only supports AUX_CCS. */
      blt.SourceDepthStencilResource =
         params->src.aux_usage == ISL_AUX_USAGE_STC_CCS;
#endif
      blt.SourceTargetMemory =
         params->src.addr.local_hint ? XY_MEM_LOCAL : XY_MEM_SYSTEM;

      if (params->src.aux_usage != ISL_AUX_USAGE_NONE) {
#if GFX_VER < 20
         blt.SourceAuxiliarySurfaceMode = xy_aux_mode(&params->src);
         blt.SourceCompressionEnable = true;
#endif
         blt.SourceCompressionFormat =
            isl_get_render_compression_format(src_surf->format);
         blt.SourceClearValueEnable = !!params->src.clear_color_addr.buffer;
         blt.SourceClearAddress = params->src.clear_color_addr;
      }
#endif
   }
#endif
}

UNUSED static void
blorp_xy_fast_color_blit(struct blorp_batch *batch,
                         const struct blorp_params *params)
{
#if GFX_VER < 12
   unreachable("Blitter is only supported on Gfx12+");
#else
   UNUSED const struct isl_device *isl_dev = batch->blorp->isl_dev;
   const struct isl_surf *dst_surf = &params->dst.surf;
   const struct isl_format_layout *fmtl =
      isl_format_get_layout(params->dst.view.format);

   assert(batch->flags & BLORP_BATCH_USE_BLITTER);
   assert(!(batch->flags & BLORP_BATCH_PREDICATE_ENABLE));
   assert(params->hiz_op == ISL_AUX_OP_NONE);

   assert(params->num_layers == 1);
   assert(params->dst.view.levels == 1);
   assert(dst_surf->samples == 1);
   assert(fmtl->bpb != 96 || dst_surf->tiling == ISL_TILING_LINEAR);

#if GFX_VERx10 < 125
   assert(params->dst.view.base_array_layer == 0);
   assert(params->dst.z_offset == 0);
#endif

   unsigned dst_pitch_unit = dst_surf->tiling == ISL_TILING_LINEAR ? 1 : 4;

#if GFX_VERx10 >= 125
   struct isl_extent3d dst_align = isl_get_image_alignment(dst_surf);
#endif

#if INTEL_NEEDS_WA_16021021469
   assert(fmtl->bpb != 96);
#endif

   blorp_emit(batch, GENX(XY_FAST_COLOR_BLT), blt) {
      blt.ColorDepth = xy_color_depth(fmtl);

      blt.DestinationPitch = (dst_surf->row_pitch_B / dst_pitch_unit) - 1;
      blt.DestinationTiling = xy_bcb_tiling(dst_surf);
      blt.DestinationX1 = params->x0;
      blt.DestinationY1 = params->y0;
      blt.DestinationX2 = params->x1;
      blt.DestinationY2 = params->y1;
      blt.DestinationBaseAddress = params->dst.addr;
      blt.DestinationXOffset = params->dst.tile_x_sa;
      blt.DestinationYOffset = params->dst.tile_y_sa;

      isl_color_value_pack((union isl_color_value *)
                           params->wm_inputs.clear_color,
                           params->dst.view.format, blt.FillColor);

#if GFX_VERx10 >= 125
      blt.DestinationSurfaceType = xy_bcb_surf_dim(dst_surf);
      blt.DestinationSurfaceWidth = dst_surf->logical_level0_px.w - 1;
      blt.DestinationSurfaceHeight = dst_surf->logical_level0_px.h - 1;
      blt.DestinationSurfaceDepth = xy_bcb_surf_depth(dst_surf) - 1;
      blt.DestinationArrayIndex =
         params->dst.view.base_array_layer + params->dst.z_offset;
      blt.DestinationSurfaceQPitch = isl_get_qpitch(dst_surf) >> 2;
      blt.DestinationLOD = params->dst.view.base_level;
      blt.DestinationMipTailStartLOD = dst_surf->miptail_start_level;
      blt.DestinationHorizontalAlign = isl_encode_halign(dst_align.width);
      blt.DestinationVerticalAlign = isl_encode_valign(dst_align.height);
      /* XY_FAST_COLOR_BLT only supports AUX_CCS. */
      blt.DestinationDepthStencilResource =
         params->dst.aux_usage == ISL_AUX_USAGE_STC_CCS;
      blt.DestinationTargetMemory =
         params->dst.addr.local_hint ? XY_MEM_LOCAL : XY_MEM_SYSTEM;

      if (params->dst.aux_usage != ISL_AUX_USAGE_NONE) {
#if GFX_VERx10 == 125
         blt.DestinationAuxiliarySurfaceMode = xy_aux_mode(&params->dst);
         blt.DestinationCompressionEnable = true;
         blt.DestinationClearValueEnable = !!params->dst.clear_color_addr.buffer;
         blt.DestinationClearAddress = params->dst.clear_color_addr;
#endif
         blt.DestinationCompressionFormat =
            isl_get_render_compression_format(dst_surf->format);
      }

      blt.DestinationMOCS = params->dst.addr.mocs;
#endif
   }
#endif
}

static void
blorp_exec_blitter(struct blorp_batch *batch,
                   const struct blorp_params *params)
{
   blorp_measure_start(batch, params);

   if (params->src.enabled)
      blorp_xy_block_copy_blt(batch, params);
   else
      blorp_xy_fast_color_blit(batch, params);

   blorp_measure_end(batch, params);
}

/**
 * \brief Execute a blit or render pass operation.
 *
 * To execute the operation, this function manually constructs and emits a
 * batch to draw a rectangle primitive. The batchbuffer is flushed before
 * constructing and after emitting the batch.
 *
 * This function alters no GL state.
 */
static void
blorp_exec(struct blorp_batch *batch, const struct blorp_params *params)
{
   if (batch->flags & BLORP_BATCH_USE_BLITTER) {
      blorp_exec_blitter(batch, params);
   } else if (batch->flags & BLORP_BATCH_USE_COMPUTE) {
      blorp_exec_compute(batch, params);
   } else {
      blorp_exec_3d(batch, params);
   }
}

static void
blorp_init_dynamic_states(struct blorp_context *context)
{
   {
      struct GENX(BLEND_STATE) blend = { };

      uint32_t dws[GENX(BLEND_STATE_length) * 4 +
                   GENX(BLEND_STATE_ENTRY_length) * 4 * 8 /* MAX_RTS */];
      uint32_t *pos = dws;

      GENX(BLEND_STATE_pack)(NULL, pos, &blend);
      pos += GENX(BLEND_STATE_length);

      for (unsigned i = 0; i < 8; ++i) {
         struct GENX(BLEND_STATE_ENTRY) entry = {
            .PreBlendColorClampEnable = true,
            .PostBlendColorClampEnable = true,
            .ColorClampRange = COLORCLAMP_RTFORMAT,
         };
         GENX(BLEND_STATE_ENTRY_pack)(NULL, pos, &entry);
         pos += GENX(BLEND_STATE_ENTRY_length);
      }

      context->upload_dynamic_state(context, dws, sizeof(dws), 64,
                                    BLORP_DYNAMIC_STATE_BLEND);
   }

   blorp_context_upload_dynamic(context, GENX(CC_VIEWPORT), vp, 32,
                                BLORP_DYNAMIC_STATE_CC_VIEWPORT) {
      vp.MinimumDepth = context->config.use_unrestricted_depth_range ?
                        -FLT_MAX : 0.0;
      vp.MaximumDepth = context->config.use_unrestricted_depth_range ?
                        FLT_MAX : 1.0;
   }

   blorp_context_upload_dynamic(context, GENX(COLOR_CALC_STATE), cc, 64,
                                BLORP_DYNAMIC_STATE_COLOR_CALC) {
      /* Nothing */
   }

   blorp_context_upload_dynamic(context, GENX(SAMPLER_STATE), sampler, 32,
                                BLORP_DYNAMIC_STATE_SAMPLER) {
      sampler.MipModeFilter = MIPFILTER_NONE;
      sampler.MagModeFilter = MAPFILTER_LINEAR;
      sampler.MinModeFilter = MAPFILTER_LINEAR;
      sampler.MinLOD = 0;
      sampler.MaxLOD = 0;
      sampler.TCXAddressControlMode = TCM_CLAMP;
      sampler.TCYAddressControlMode = TCM_CLAMP;
      sampler.TCZAddressControlMode = TCM_CLAMP;
      sampler.MaximumAnisotropy = RATIO21;
      sampler.RAddressMinFilterRoundingEnable = true;
      sampler.RAddressMagFilterRoundingEnable = true;
      sampler.VAddressMinFilterRoundingEnable = true;
      sampler.VAddressMagFilterRoundingEnable = true;
      sampler.UAddressMinFilterRoundingEnable = true;
      sampler.UAddressMagFilterRoundingEnable = true;
      sampler.NonnormalizedCoordinateEnable = true;
   }
}

#endif /* BLORP_GENX_EXEC_BRW_H */
