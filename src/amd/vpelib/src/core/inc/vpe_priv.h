/* Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#pragma once

#include "vpe_types.h"
#include "resource.h"
#include "transform.h"
#include "color.h"
#include "color_gamma.h"
#include "vpe_desc_writer.h"
#include "plane_desc_writer.h"
#include "config_writer.h"
#include "color_cs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define vpe_zalloc(size) vpe_priv->init.funcs.zalloc(vpe_priv->init.funcs.mem_ctx, size)
#define vpe_free(ptr)    vpe_priv->init.funcs.free(vpe_priv->init.funcs.mem_ctx, (ptr))
#define vpe_log(...)                                                                               \
    do {                                                                                           \
        vpe_priv->init.funcs.log(vpe_priv->init.funcs.log_ctx, "vpe: ");                           \
        vpe_priv->init.funcs.log(vpe_priv->init.funcs.log_ctx, __VA_ARGS__);                       \
    } while (0)

#define container_of(ptr, type, member) (type *)(void *)((char *)ptr - offsetof(type, member))

#define VPE_MIN_VIEWPORT_SIZE                                                                      \
    2                      // chroma viewport size is half of it, thus need to be 2 for YUV420
                           // for simplication we just use 2 for all types
#define MAX_VPE_CMD 256    // TODO Dynamic allocation

#define MAX_LINE_SIZE 1024 // without 16 pixels for the seams
#define MAX_LINE_CNT  4

enum vpe_cmd_ops {
    VPE_CMD_OPS_BLENDING,
    VPE_CMD_OPS_BG,
    VPE_CMD_OPS_COMPOSITING,
    VPE_CMD_OPS_BG_VSCF_INPUT,  // For visual confirm input
    VPE_CMD_OPS_BG_VSCF_OUTPUT, // For visual confirm output
};

enum vpe_cmd_type {
    VPE_CMD_TYPE_COMPOSITING,
    VPE_CMD_TYPE_BG,
    VPE_CMD_TYPE_BG_VSCF_INPUT,  // For visual confirm input
    VPE_CMD_TYPE_BG_VSCF_OUTPUT, // For visual confirm output
    VPE_CMD_TYPE_COUNT
};

enum vpe_stream_type {
    VPE_STREAM_TYPE_INPUT,
    VPE_STREAM_TYPE_BG_GEN,
};

/** this represents a segement context.
 * each segment has its own version of data */
struct segment_ctx {
    uint16_t           segment_idx;
    struct stream_ctx *stream_ctx;
    struct scaler_data scaler_data;
};

struct vpe_cmd_input {
    uint16_t           stream_idx;
    struct scaler_data scaler_data;
};

struct vpe_cmd_output {
    struct vpe_rect dst_viewport;
    struct vpe_rect dst_viewport_c;
};

struct vpe_cmd_info {
    enum vpe_cmd_ops ops;
    uint8_t          cd; // count down value

    // input
    uint16_t             num_inputs;
    struct vpe_cmd_input inputs[MAX_INPUT_PIPE];

    // output
    uint16_t              num_outputs;
    struct vpe_cmd_output outputs[MAX_OUTPUT_PIPE];

    bool tm_enabled;
    bool insert_start_csync;
    bool insert_end_csync;
};

struct config_record {
    uint64_t config_base_addr;
    uint64_t config_size;
};

/** represents a stream input, i.e. common to all segments */
struct stream_ctx {
    struct vpe_priv *vpe_priv;

    enum vpe_stream_type stream_type;
    int32_t              stream_idx;
    struct vpe_stream    stream; /**< stores all the input data */

    uint16_t            num_segments;
    struct segment_ctx *segment_ctx;

    // share configs that can be re-used once generated
    struct vpe_vector *configs[MAX_INPUT_PIPE];
    struct vpe_vector *stream_op_configs[MAX_INPUT_PIPE][VPE_CMD_TYPE_COUNT];

    // cached color properties
    bool                     per_pixel_alpha;
    enum color_transfer_func tf;
    enum color_space         cs;
    bool                     enable_3dlut;
    uint64_t                 uid_3dlut;                 // UID for current 3D LUT params
    bool                     geometric_scaling;
    bool                     is_yuv_input;

    union {
        struct {
            unsigned int color_space       : 1;
            unsigned int transfer_function : 1;
            unsigned int pixel_format      : 1;
            unsigned int reserved          : 1;
        };
        unsigned int u32All;
    } dirty_bits;

    struct bias_and_scale       *bias_scale;
    struct transfer_func        *input_tf;
    struct vpe_csc_matrix       *input_cs;
    struct colorspace_transform *gamut_remap;
    struct transfer_func        *in_shaper_func; // for shaper lut
    struct vpe_3dlut            *lut3d_func;     // for 3dlut
    struct transfer_func        *blend_tf;       // for 1dlut
    white_point_gain             white_point_gain;
    bool                         flip_horizonal_output;
    struct vpe_color_adjust      color_adjustments; // stores the current color adjustments params
    struct fixed31_32            tf_scaling_factor; // a gain applied on a transfer function
};

struct output_ctx {
    // stores the paramters built for generating vpep configs
    struct vpe_surface_info    surface;
    struct vpe_color           bg_color;
    struct vpe_rect            target_rect;
    enum vpe_alpha_mode        alpha_mode;
    struct vpe_clamping_params clamping_params;

    // cached color properties
    enum color_transfer_func tf;
    enum color_space         cs;

    // store generated per-pipe configs that can be reused
    struct vpe_vector *configs[MAX_OUTPUT_PIPE];

    union {
        struct {
            unsigned int color_space       : 1;
            unsigned int transfer_function : 1;
            unsigned int lut3d             : 1;
            unsigned int reserved          : 1;
        };
        unsigned int u32All;
    } dirty_bits;

    struct transfer_func        *output_tf;
    const struct transfer_func  *in_shaper_func; // for shaper lut
    const struct vpe_3dlut      *lut3d_func;     // for 3dlut
    const struct transfer_func  *blend_tf;       // for 1dlut
    struct colorspace_transform *gamut_remap;    // post blend gamut remap

    struct {
        uint32_t hdr_metadata : 1;
        uint32_t reserved     : 31;
    } flags;
    struct vpe_hdr_metadata hdr_metadata;
};

#define PIPE_CTX_NO_OWNER ((uint32_t)(-1))

struct pipe_ctx {
    uint32_t pipe_idx;
    uint32_t owner; // stream_idx
    bool     is_top_pipe;
    int32_t  top_pipe_idx;
};

struct config_frontend_cb_ctx {
    struct vpe_priv  *vpe_priv;
    uint32_t          stream_idx;
    bool              stream_sharing;
    bool              stream_op_sharing;
    enum vpe_cmd_type cmd_type; // command type, i.e. bg or compositing
};

struct config_backend_cb_ctx {
    struct vpe_priv *vpe_priv;
    bool             share; // add to output_ctx if true
};

/** internal vpe instance */
struct vpe_priv {
    /** public */
    struct vpe pub; /**< public member */

    /** internal */
    struct vpe_init_data    init;
    struct resource         resource;
    struct calculate_buffer cal_buffer;
    struct vpe_bufs_req     bufs_required; /**< cached required buffer size for the checked ops */

    struct vpe_vector  *vpe_cmd_vector;
    bool                ops_support;

    // writers
    struct vpe_desc_writer        vpe_desc_writer;
    struct plane_desc_writer      plane_desc_writer;
    struct config_writer          config_writer;
    struct config_frontend_cb_ctx fe_cb_ctx;
    struct config_backend_cb_ctx  be_cb_ctx;

    // input ctx
    uint32_t           num_virtual_streams; // streams created by VPE
    uint32_t           num_input_streams;   // streams inputed from build params
    uint32_t           num_streams;         // input streams + virtual streams
    struct stream_ctx *stream_ctx;          // input streams allocated first, then virtual streams

    // output ctx
    struct output_ctx output_ctx;

    uint16_t        num_pipe;
    struct pipe_ctx pipe_ctx[MAX_INPUT_PIPE];

    // internal temp structure for creating pure BG filling
    struct vpe_build_param *dummy_input_param;
    struct vpe_stream      *dummy_stream;
    bool scale_yuv_matrix; // this is a flag that forces scaling the yuv->rgb matrix
                           //  when embedding the color adjustments

#ifdef VPE_BUILD_1_1
    // collaborate sync data counter
    int32_t  collaborate_sync_index;
    uint16_t vpe_num_instance;
    bool     collaboration_mode;
#endif
    enum vpe_expansion_mode expansion_mode;
};

#ifdef __cplusplus
}
#endif
