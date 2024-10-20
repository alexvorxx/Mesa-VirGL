/*
 * Copyright © 2019 Red Hat.
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

#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include <llvm/Config/llvm-config.h>

#include "util/macros.h"
#include "util/list.h"
#include "util/u_dynarray.h"
#include "util/simple_mtx.h"
#include "util/u_queue.h"
#include "util/u_upload_mgr.h"

#include "compiler/shader_enums.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "cso_cache/cso_context.h"
#include "nir.h"

#ifdef HAVE_LIBDRM
#include <drm-uapi/drm.h>
#include "drm-uapi/drm_fourcc.h"
#endif

#if DETECT_OS_ANDROID
#include <vndk/hardware_buffer.h>
#endif

/* Pre-declarations needed for WSI entrypoints */
struct wl_surface;
struct wl_display;
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_visualid_t;
typedef uint32_t xcb_window_t;

#define VK_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#include "lvp_entrypoints.h"
#include "vk_acceleration_structure.h"
#include "vk_buffer.h"
#include "vk_buffer_view.h"
#include "vk_device.h"
#include "vk_device_generated_commands.h"
#include "vk_instance.h"
#include "vk_image.h"
#include "vk_log.h"
#include "vk_physical_device.h"
#include "vk_shader_module.h"
#include "vk_util.h"
#include "vk_format.h"
#include "vk_cmd_queue.h"
#include "vk_command_buffer.h"
#include "vk_command_pool.h"
#include "vk_descriptor_set_layout.h"
#include "vk_graphics_state.h"
#include "vk_pipeline_layout.h"
#include "vk_queue.h"
#include "vk_sampler.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"
#include "vk_ycbcr_conversion.h"
#include "lp_jit.h"

#include "wsi_common.h"

#include <assert.h>
#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SETS         8
#define MAX_DESCRIPTORS 1000000 /* Required by vkd3d-proton */
#define MAX_PUSH_CONSTANTS_SIZE 256
#define MAX_PUSH_DESCRIPTORS 32
#define MAX_DESCRIPTOR_UNIFORM_BLOCK_SIZE 4096
#define MAX_PER_STAGE_DESCRIPTOR_UNIFORM_BLOCKS 8
#define MAX_DGC_STREAMS 16
#define MAX_DGC_TOKENS 16
/* Currently lavapipe does not support more than 1 image plane */
#define LVP_MAX_PLANE_COUNT 1

#ifdef _WIN32
#define lvp_printflike(a, b)
#else
#define lvp_printflike(a, b) __attribute__((__format__(__printf__, a, b)))
#endif

#define LVP_DEBUG_ALL_ENTRYPOINTS (1 << 0)

void __lvp_finishme(const char *file, int line, const char *format, ...)
   lvp_printflike(3, 4);

#define lvp_finishme(format, ...) \
   __lvp_finishme(__FILE__, __LINE__, format, ##__VA_ARGS__);

#define stub_return(v) \
   do { \
      lvp_finishme("stub %s", __func__); \
      return (v); \
   } while (0)

#define stub() \
   do { \
      lvp_finishme("stub %s", __func__); \
      return; \
   } while (0)

#define LVP_SHADER_STAGES (MESA_SHADER_CALLABLE + 1)
#define LVP_STAGE_MASK BITFIELD_MASK(LVP_SHADER_STAGES)
#define LVP_STAGE_MASK_GFX (BITFIELD_MASK(PIPE_SHADER_MESH_TYPES) & ~BITFIELD_BIT(MESA_SHADER_COMPUTE))

#define lvp_foreach_stage(stage, stage_bits)                         \
   for (gl_shader_stage stage,                                       \
        __tmp = (gl_shader_stage)((stage_bits) & LVP_STAGE_MASK);    \
        stage = ffs(__tmp) - 1, __tmp;                     \
        __tmp &= ~(1 << (stage)))

#define lvp_forall_stage(stage)                                      \
   for (gl_shader_stage stage = MESA_SHADER_VERTEX; stage < LVP_SHADER_STAGES; stage++)

#define lvp_forall_gfx_stage(stage)                                  \
   for (gl_shader_stage stage,                                       \
           __tmp = (gl_shader_stage)(LVP_STAGE_MASK_GFX);            \
        stage = ffs(__tmp) - 1, __tmp;                               \
        __tmp &= ~(1 << (stage)))

struct lvp_physical_device {
   struct vk_physical_device vk;

   struct pipe_loader_device *pld;
   struct pipe_screen *pscreen;
   const nir_shader_compiler_options *drv_options[LVP_SHADER_STAGES];
   uint32_t max_images;
   bool snorm_blend;

   struct vk_sync_timeline_type sync_timeline_type;
   const struct vk_sync_type *sync_types[3];

   struct wsi_device                       wsi_device;
};

struct lvp_instance {
   struct vk_instance vk;

   uint32_t apiVersion;

   uint64_t debug_flags;

   struct pipe_loader_device *devs;
   int num_devices;
};

VkResult lvp_init_wsi(struct lvp_physical_device *physical_device);
void lvp_finish_wsi(struct lvp_physical_device *physical_device);

bool lvp_physical_device_extension_supported(struct lvp_physical_device *dev,
                                              const char *name);

struct lvp_queue {
   struct vk_queue vk;
   struct lvp_device *                         device;
   struct pipe_context *ctx;
   struct cso_context *cso;
   struct u_upload_mgr *uploader;
   struct pipe_fence_handle *last_fence;
   void *state;
   struct util_dynarray pipeline_destroys;
   simple_mtx_t lock;
};

struct lvp_pipeline_cache {
   struct vk_object_base                        base;
   struct lvp_device *                          device;
   VkAllocationCallbacks                        alloc;
};

struct lvp_device {
   struct vk_device vk;

   struct lvp_queue queue;
   struct lvp_instance *                       instance;
   struct lvp_physical_device *physical_device;
   struct pipe_screen *pscreen;
   void *noop_fs;
   simple_mtx_t bda_lock;
   struct hash_table bda;
   struct pipe_resource *zero_buffer; /* for zeroed bda */
   bool poison_mem;
   bool print_cmds;

   struct lp_texture_handle *null_texture_handle;
   struct lp_texture_handle *null_image_handle;
   struct util_dynarray bda_texture_handles;
   struct util_dynarray bda_image_handles;

   uint32_t group_handle_alloc;
};

void lvp_device_get_cache_uuid(void *uuid);

enum lvp_device_memory_type {
   LVP_DEVICE_MEMORY_TYPE_DEFAULT,
   LVP_DEVICE_MEMORY_TYPE_USER_PTR,
   LVP_DEVICE_MEMORY_TYPE_OPAQUE_FD,
   LVP_DEVICE_MEMORY_TYPE_DMA_BUF,
};

struct lvp_device_memory {
   struct vk_object_base base;
   struct pipe_memory_allocation *pmem;
   struct llvmpipe_memory_allocation mem_alloc;
   uint32_t                                     type_index;
   VkDeviceSize                                 map_size;
   VkDeviceSize                                 size;
   void *                                       map;
   enum lvp_device_memory_type memory_type;
   int                                          backed_fd;
#if DETECT_OS_ANDROID
   struct AHardwareBuffer *android_hardware_buffer;
#endif
};

struct lvp_pipe_sync {
   struct vk_sync base;

   mtx_t lock;
   cnd_t changed;

   bool signaled;
   struct pipe_fence_handle *fence;
};

extern const struct vk_sync_type lvp_pipe_sync_type;

void lvp_pipe_sync_signal_with_fence(struct lvp_device *device,
                                     struct lvp_pipe_sync *sync,
                                     struct pipe_fence_handle *fence);

static inline struct lvp_pipe_sync *
vk_sync_as_lvp_pipe_sync(struct vk_sync *sync)
{
   assert(sync->type == &lvp_pipe_sync_type);
   return container_of(sync, struct lvp_pipe_sync, base);
}

struct lvp_image_plane {
   struct pipe_resource *bo;
   struct pipe_memory_allocation *pmem;
   VkDeviceSize plane_offset;
   VkDeviceSize memory_offset;
   VkDeviceSize size;
};

struct lvp_image {
   struct vk_image vk;
   VkDeviceSize offset;
   VkDeviceSize size;
   uint32_t alignment;
   bool disjoint;
   uint8_t plane_count;
   struct lvp_image_plane planes[3];
};

struct lvp_image_view {
   struct vk_image_view vk;
   const struct lvp_image *image; /**< VkImageViewCreateInfo::image */

   enum pipe_format pformat;

   struct pipe_surface *surface; /* have we created a pipe surface for this? */
   struct lvp_image_view *multisample; //VK_EXT_multisampled_render_to_single_sampled

   uint8_t plane_count;
   struct {
      unsigned image_plane;
      struct pipe_sampler_view *sv;
      struct pipe_image_view iv;
      struct lp_texture_handle *texture_handle;
      struct lp_texture_handle *image_handle;
   } planes[3];
};

struct lvp_sampler {
   struct vk_sampler vk;
   struct lp_descriptor desc;

   struct lp_texture_handle *texture_handle;
};

struct lvp_descriptor_set_binding_layout {
   uint32_t descriptor_index;
   /* Number of array elements in this binding */
   VkDescriptorType type;
   uint32_t stride; /* used for planar samplers */
   uint32_t array_size;
   bool valid;

   uint32_t dynamic_index;

   uint32_t uniform_block_offset;
   uint32_t uniform_block_size;

   /* Immutable samplers (or NULL if no immutable samplers) */
   struct lvp_sampler **immutable_samplers;
};

struct lvp_descriptor_set_layout {
   struct vk_descriptor_set_layout vk;

   /* add new members after this */

   uint32_t immutable_sampler_count;

   /* Number of bindings in this descriptor set */
   uint32_t binding_count;

   /* Total size of the descriptor set with room for all array entries */
   uint32_t size;

   /* Shader stages affected by this descriptor set */
   uint32_t shader_stages;

   /* Number of dynamic offsets used by this descriptor set */
   uint32_t dynamic_offset_count;

   /* if this layout is comprised solely of immutable samplers, this will be a bindable set */
   struct lvp_descriptor_set *immutable_set;

   /* Bindings in this descriptor set */
   struct lvp_descriptor_set_binding_layout binding[0];
};

static inline const struct lvp_descriptor_set_layout *
vk_to_lvp_descriptor_set_layout(const struct vk_descriptor_set_layout *layout)
{
   return container_of(layout, const struct lvp_descriptor_set_layout, vk);
}

struct lvp_descriptor_set {
   struct vk_object_base base;
   struct lvp_descriptor_set_layout *layout;
   struct list_head link;

   /* Buffer holding the descriptors. */
   struct pipe_memory_allocation *pmem;
   struct pipe_resource *bo;
   void *map;
};

struct lvp_descriptor_pool {
   struct vk_object_base base;
   VkDescriptorPoolCreateFlags flags;
   uint32_t max_sets;

   struct list_head sets;
};

uint32_t lvp_descriptor_update_template_entry_size(VkDescriptorType type);

VkResult
lvp_descriptor_set_create(struct lvp_device *device,
                          struct lvp_descriptor_set_layout *layout,
                          struct lvp_descriptor_set **out_set);

void
lvp_descriptor_set_destroy(struct lvp_device *device,
                           struct lvp_descriptor_set *set);

void
lvp_descriptor_set_update_with_template(VkDevice _device, VkDescriptorSet descriptorSet,
                                        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                        const void *pData);

struct lvp_pipeline_layout {
   struct vk_pipeline_layout vk;

   uint32_t push_constant_size;
   VkShaderStageFlags push_constant_stages;
};


struct lvp_pipeline_layout *
lvp_pipeline_layout_create(struct lvp_device *device,
                           const VkPipelineLayoutCreateInfo*           pCreateInfo,
                           const VkAllocationCallbacks*                pAllocator);

struct lvp_pipeline_nir {
   int ref_cnt;
   nir_shader *nir;
};

struct lvp_pipeline_nir *
lvp_create_pipeline_nir(nir_shader *nir);

static inline void
lvp_pipeline_nir_ref(struct lvp_pipeline_nir **dst, struct lvp_pipeline_nir *src)
{
   struct lvp_pipeline_nir *old_dst = *dst;
   if (old_dst == src || (old_dst && src && old_dst->nir == src->nir))
      return;

   if (old_dst && p_atomic_dec_zero(&old_dst->ref_cnt)) {
      ralloc_free(old_dst->nir);
      ralloc_free(old_dst);
   }
   if (src)
      p_atomic_inc(&src->ref_cnt);
   *dst = src;
}

struct lvp_inline_variant {
   uint32_t mask;
   uint32_t vals[PIPE_MAX_CONSTANT_BUFFERS][MAX_INLINABLE_UNIFORMS];
   void *cso;
};

struct lvp_shader {
   struct vk_object_base base;
   struct lvp_pipeline_layout *layout;
   struct lvp_pipeline_nir *pipeline_nir;
   struct lvp_pipeline_nir *tess_ccw;
   void *shader_cso;
   void *tess_ccw_cso;
   struct {
      uint32_t uniform_offsets[PIPE_MAX_CONSTANT_BUFFERS][MAX_INLINABLE_UNIFORMS];
      uint8_t count[PIPE_MAX_CONSTANT_BUFFERS];
      bool must_inline;
      uint32_t can_inline; //bitmask
      struct set variants;
   } inlines;
   struct pipe_stream_output_info stream_output;
   struct blob blob; //preserved for GetShaderBinaryDataEXT
};

enum lvp_pipeline_type {
   LVP_PIPELINE_GRAPHICS,
   LVP_PIPELINE_COMPUTE,
   LVP_PIPELINE_RAY_TRACING,
   LVP_PIPELINE_EXEC_GRAPH,
   LVP_PIPELINE_TYPE_COUNT,
};

static inline enum lvp_pipeline_type
lvp_pipeline_type_from_bind_point(VkPipelineBindPoint bind_point)
{
   switch (bind_point) {
   case VK_PIPELINE_BIND_POINT_GRAPHICS: return LVP_PIPELINE_GRAPHICS;
   case VK_PIPELINE_BIND_POINT_COMPUTE: return LVP_PIPELINE_COMPUTE;
   case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR: return LVP_PIPELINE_RAY_TRACING;
#ifdef VK_ENABLE_BETA_EXTENSIONS
   case VK_PIPELINE_BIND_POINT_EXECUTION_GRAPH_AMDX: return LVP_PIPELINE_EXEC_GRAPH;
#endif
   default: unreachable("Unsupported VkPipelineBindPoint");
   }
}

#define LVP_RAY_TRACING_STAGES (VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |   \
                                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | \
                                VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR)

static inline uint32_t
lvp_pipeline_types_from_shader_stages(VkShaderStageFlags stageFlags)
{
   uint32_t types = 0;
#ifdef VK_ENABLE_BETA_EXTENSIONS
   if (stageFlags & MESA_VK_SHADER_STAGE_WORKGRAPH_HACK_BIT_FIXME)
      types |= BITFIELD_BIT(LVP_PIPELINE_EXEC_GRAPH);
#endif
   if (stageFlags & LVP_RAY_TRACING_STAGES)
      types |= BITFIELD_BIT(LVP_PIPELINE_RAY_TRACING);
   if (stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
      types |= BITFIELD_BIT(LVP_PIPELINE_COMPUTE);
   if (stageFlags & (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_TASK_BIT_EXT))
      types |= BITFIELD_BIT(LVP_PIPELINE_GRAPHICS);
   return types;
}

#define LVP_RAY_TRACING_GROUP_HANDLE_SIZE 32
#define LVP_RAY_HIT_ATTRIBS_SIZE 32

struct lvp_ray_tracing_group_handle {
   uint32_t index;
};

struct lvp_ray_tracing_group {
   struct lvp_ray_tracing_group_handle handle;
   uint32_t recursive_index;
   uint32_t ahit_index;
   uint32_t isec_index;
};

struct lvp_pipeline {
   struct vk_object_base base;
   struct lvp_device *                          device;
   struct lvp_pipeline_layout *                 layout;

   enum lvp_pipeline_type type;
   VkPipelineCreateFlags2KHR flags;

   void *state_data;
   bool force_min_sample;
   struct lvp_shader shaders[LVP_SHADER_STAGES];
   gl_shader_stage last_vertex;
   struct vk_graphics_pipeline_state graphics_state;
   VkGraphicsPipelineLibraryFlagsEXT stages;
   bool line_smooth;
   bool disable_multisample;
   bool line_rectangular;
   bool library;
   bool compiled;
   bool used;

   struct {
      const char *name;
      const char *next_name;
      uint32_t index;
      uint32_t scratch_size;
   } exec_graph;

   struct {
      struct lvp_pipeline_nir **stages;
      struct lvp_ray_tracing_group *groups;
      uint32_t stage_count;
      uint32_t group_count;
   } rt;

   unsigned num_groups;
   unsigned num_groups_total;
   VkPipeline groups[0];
};

/* Minimum requirement by the spec. */
#define LVP_MAX_EXEC_GRAPH_PAYLOADS 256

struct lvp_exec_graph_shader_output {
   uint32_t payload_count;
   uint32_t node_index;
};

struct lvp_exec_graph_internal_data {
   /* inputs */
   void *payload_in;
   void *payloads;
   /* outputs */
   struct lvp_exec_graph_shader_output outputs[LVP_MAX_EXEC_GRAPH_PAYLOADS];
};

bool
lvp_lower_exec_graph(struct lvp_pipeline *pipeline, nir_shader *nir);

void
lvp_pipeline_shaders_compile(struct lvp_pipeline *pipeline, bool locked);

struct lvp_event {
   struct vk_object_base base;
   volatile uint64_t event_storage;
};

struct lvp_buffer {
   struct vk_buffer vk;

   struct lvp_device_memory *mem;
   struct pipe_resource *bo;
   uint64_t total_size;
   uint64_t offset;
   void *map;
   struct pipe_transfer *transfer;
};

struct lvp_buffer_view {
   struct vk_buffer_view vk;
   enum pipe_format pformat;
   struct pipe_sampler_view *sv;
   struct pipe_image_view iv;

   struct lp_texture_handle *texture_handle;
   struct lp_texture_handle *image_handle;
};

#define LVP_QUERY_ACCELERATION_STRUCTURE_COMPACTED_SIZE (PIPE_QUERY_TYPES)
#define LVP_QUERY_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE (PIPE_QUERY_TYPES + 1)
#define LVP_QUERY_ACCELERATION_STRUCTURE_SIZE (PIPE_QUERY_TYPES + 2)
#define LVP_QUERY_ACCELERATION_STRUCTURE_INSTANCE_COUNT (PIPE_QUERY_TYPES + 3)

struct lvp_query_pool {
   struct vk_object_base base;
   VkQueryType type;
   uint32_t count;
   VkQueryPipelineStatisticFlags pipeline_stats;
   enum pipe_query_type base_type;
   void *data; /* Used by queries that are not implemented by pipe_query */
   struct pipe_query *queries[0];
};

struct lvp_cmd_buffer {
   struct vk_command_buffer vk;

   struct lvp_device *                          device;

   uint8_t push_constants[MAX_PUSH_CONSTANTS_SIZE];
};

struct lvp_indirect_command_layout_nv {
   struct vk_object_base base;
   uint8_t stream_count;
   uint8_t token_count;
   uint16_t stream_strides[MAX_DGC_STREAMS];
   VkPipelineBindPoint bind_point;
   VkIndirectCommandsLayoutUsageFlagsNV flags;
   VkIndirectCommandsLayoutTokenNV tokens[0];
};

struct lvp_indirect_execution_set {
   struct vk_object_base base;
   bool is_shaders;
#if VK_USE_64_BIT_PTR_DEFINES
   void *array[0];
#else
   uint64_t array[0];
#endif
};

enum lvp_indirect_layout_type {
   LVP_INDIRECT_COMMAND_LAYOUT_DRAW,
   LVP_INDIRECT_COMMAND_LAYOUT_DRAW_COUNT,
   LVP_INDIRECT_COMMAND_LAYOUT_DISPATCH,
   LVP_INDIRECT_COMMAND_LAYOUT_RAYS,
};

struct lvp_indirect_command_layout_ext {
   struct vk_indirect_command_layout vk;
   enum lvp_indirect_layout_type type;
   VkIndirectCommandsLayoutTokenEXT tokens[0];
};

extern const struct vk_command_buffer_ops lvp_cmd_buffer_ops;

static inline const struct lvp_descriptor_set_layout *
get_set_layout(const struct lvp_pipeline_layout *layout, uint32_t set)
{
   return container_of(layout->vk.set_layouts[set],
                       const struct lvp_descriptor_set_layout, vk);
}

static inline const struct lvp_descriptor_set_binding_layout *
get_binding_layout(const struct lvp_pipeline_layout *layout,
                   uint32_t set, uint32_t binding)
{
   return &get_set_layout(layout, set)->binding[binding];
}

#define LVP_FROM_HANDLE(__lvp_type, __name, __handle) \
   struct __lvp_type *__name = __lvp_type ## _from_handle(__handle)

VK_DEFINE_HANDLE_CASTS(lvp_cmd_buffer, vk.base, VkCommandBuffer,
                       VK_OBJECT_TYPE_COMMAND_BUFFER)
VK_DEFINE_HANDLE_CASTS(lvp_device, vk.base, VkDevice, VK_OBJECT_TYPE_DEVICE)
VK_DEFINE_HANDLE_CASTS(lvp_instance, vk.base, VkInstance, VK_OBJECT_TYPE_INSTANCE)
VK_DEFINE_HANDLE_CASTS(lvp_physical_device, vk.base, VkPhysicalDevice,
                       VK_OBJECT_TYPE_PHYSICAL_DEVICE)
VK_DEFINE_HANDLE_CASTS(lvp_queue, vk.base, VkQueue, VK_OBJECT_TYPE_QUEUE)

VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_buffer, vk.base, VkBuffer,
                               VK_OBJECT_TYPE_BUFFER)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_buffer_view, vk.base, VkBufferView,
                               VK_OBJECT_TYPE_BUFFER_VIEW)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_descriptor_pool, base, VkDescriptorPool,
                               VK_OBJECT_TYPE_DESCRIPTOR_POOL)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_descriptor_set, base, VkDescriptorSet,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_descriptor_set_layout, vk.base, VkDescriptorSetLayout,
                               VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_device_memory, base, VkDeviceMemory,
                               VK_OBJECT_TYPE_DEVICE_MEMORY)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_event, base, VkEvent, VK_OBJECT_TYPE_EVENT)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_image, vk.base, VkImage, VK_OBJECT_TYPE_IMAGE)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_image_view, vk.base, VkImageView,
                               VK_OBJECT_TYPE_IMAGE_VIEW);
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_pipeline_cache, base, VkPipelineCache,
                               VK_OBJECT_TYPE_PIPELINE_CACHE)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_pipeline, base, VkPipeline,
                               VK_OBJECT_TYPE_PIPELINE)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_shader, base, VkShaderEXT,
                               VK_OBJECT_TYPE_SHADER_EXT)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_pipeline_layout, vk.base, VkPipelineLayout,
                               VK_OBJECT_TYPE_PIPELINE_LAYOUT)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_query_pool, base, VkQueryPool,
                               VK_OBJECT_TYPE_QUERY_POOL)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_sampler, vk.base, VkSampler,
                               VK_OBJECT_TYPE_SAMPLER)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_indirect_command_layout_nv, base, VkIndirectCommandsLayoutNV,
                               VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_NV)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_indirect_command_layout_ext, vk.base, VkIndirectCommandsLayoutEXT,
                               VK_OBJECT_TYPE_INDIRECT_COMMANDS_LAYOUT_EXT)
VK_DEFINE_NONDISP_HANDLE_CASTS(lvp_indirect_execution_set, base, VkIndirectExecutionSetEXT,
                               VK_OBJECT_TYPE_INDIRECT_EXECUTION_SET_EXT)

void lvp_add_enqueue_cmd_entrypoints(struct vk_device_dispatch_table *disp);

VkResult lvp_buffer_bind_sparse(struct lvp_device *device,
                                struct lvp_queue *queue,
                                VkSparseBufferMemoryBindInfo *bind);
VkResult lvp_image_bind_opaque_sparse(struct lvp_device *device,
                                      struct lvp_queue *queue,
                                      VkSparseImageOpaqueMemoryBindInfo *bind);
VkResult lvp_image_bind_sparse(struct lvp_device *device,
                               struct lvp_queue *queue,
                               VkSparseImageMemoryBindInfo *bind);

VkResult lvp_execute_cmds(struct lvp_device *device,
                          struct lvp_queue *queue,
                          struct lvp_cmd_buffer *cmd_buffer);
size_t
lvp_get_rendering_state_size(void);
struct lvp_image *lvp_swapchain_get_image(VkSwapchainKHR swapchain,
                                          uint32_t index);

static inline enum pipe_format
lvp_vk_format_to_pipe_format(VkFormat format)
{
   /* Some formats cause problems with CTS right now.*/
   switch (format) {
   case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
   case VK_FORMAT_R8_SRGB:
   case VK_FORMAT_R8G8_SRGB:
   case VK_FORMAT_R64G64B64A64_SFLOAT:
   case VK_FORMAT_R64_SFLOAT:
   case VK_FORMAT_R64G64_SFLOAT:
   case VK_FORMAT_R64G64B64_SFLOAT:
   case VK_FORMAT_A2R10G10B10_SINT_PACK32:
   case VK_FORMAT_A2B10G10R10_SINT_PACK32:
   case VK_FORMAT_D16_UNORM_S8_UINT:
      return PIPE_FORMAT_NONE;
   case VK_FORMAT_R10X6_UNORM_PACK16:
   case VK_FORMAT_R12X4_UNORM_PACK16:
      return PIPE_FORMAT_R16_UNORM;
   case VK_FORMAT_R10X6G10X6_UNORM_2PACK16:
   case VK_FORMAT_R12X4G12X4_UNORM_2PACK16:
      return PIPE_FORMAT_R16G16_UNORM;
   default:
      return vk_format_to_pipe_format(format);
   }
}

static inline uint8_t
lvp_image_aspects_to_plane(ASSERTED const struct lvp_image *image,
                           VkImageAspectFlags aspectMask)
{
   /* If we are requesting the first plane of image with only one plane, return that */
   if (image->vk.aspects == VK_IMAGE_ASPECT_COLOR_BIT && aspectMask == VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)
      return 0;

   /* Verify that the aspects are actually in the image */
   assert(!(aspectMask & ~image->vk.aspects));

   /* Must only be one aspect unless it's depth/stencil */
   assert(aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT |
                         VK_IMAGE_ASPECT_STENCIL_BIT) ||
          util_bitcount(aspectMask) == 1);

   switch(aspectMask) {
   case VK_IMAGE_ASPECT_PLANE_1_BIT: return 1;
   case VK_IMAGE_ASPECT_PLANE_2_BIT: return 2;
   default: return 0;
   }
}

void
lvp_pipeline_destroy(struct lvp_device *device, struct lvp_pipeline *pipeline, bool locked);

void
queue_thread_noop(void *data, void *gdata, int thread_index);

VkResult
lvp_spirv_to_nir(struct lvp_pipeline *pipeline, const VkPipelineShaderStageCreateInfo *sinfo,
                 nir_shader **out_nir);

void
lvp_shader_init(struct lvp_shader *shader, nir_shader *nir);

void
lvp_shader_optimize(nir_shader *nir);
bool
lvp_find_inlinable_uniforms(struct lvp_shader *shader, nir_shader *nir);
void
lvp_inline_uniforms(nir_shader *nir, const struct lvp_shader *shader, const uint32_t *uniform_values, uint32_t ubo);
void *
lvp_shader_compile(struct lvp_device *device, struct lvp_shader *shader, nir_shader *nir, bool locked);
bool
lvp_nir_lower_ray_queries(struct nir_shader *shader);
bool
lvp_nir_lower_sparse_residency(struct nir_shader *shader);
enum vk_cmd_type
lvp_nv_dgc_token_to_cmd_type(const VkIndirectCommandsLayoutTokenNV *token);

#if DETECT_OS_ANDROID
VkResult
lvp_import_ahb_memory(struct lvp_device *device, struct lvp_device_memory *mem,
                      const VkImportAndroidHardwareBufferInfoANDROID *info);
VkResult
lvp_create_ahb_memory(struct lvp_device *device, struct lvp_device_memory *mem,
                      const VkMemoryAllocateInfo *pAllocateInfo);
#endif

enum vk_cmd_type
lvp_ext_dgc_token_to_cmd_type(const struct lvp_indirect_command_layout_ext *elayout, const VkIndirectCommandsLayoutTokenEXT *token);
size_t
lvp_ext_dgc_token_size(const struct lvp_indirect_command_layout_ext *elayout, const VkIndirectCommandsLayoutTokenEXT *token);
#ifdef __cplusplus
}
#endif
