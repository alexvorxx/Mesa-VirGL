/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef LP_TEXTURE_H
#define LP_TEXTURE_H


#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "lp_limits.h"
#include "util/bitset.h"
#if MESA_DEBUG
#include "util/list.h"
#endif


enum lp_texture_usage
{
   LP_TEX_USAGE_READ = 100,
   LP_TEX_USAGE_READ_WRITE,
   LP_TEX_USAGE_WRITE_ALL
};

enum llvmpipe_memory_fd_type
{
   LLVMPIPE_MEMORY_FD_TYPE_OPAQUE,
   LLVMPIPE_MEMORY_FD_TYPE_DMA_BUF,
};

struct pipe_context;
struct pipe_screen;
struct pipe_memory_object;
struct llvmpipe_context;
struct llvmpipe_screen;

struct sw_displaytarget;

/**
 * llvmpipe subclass of pipe_resource.  A texture, drawing surface,
 * vertex buffer, const buffer, etc.
 * Textures are stored differently than other types of objects such as
 * vertex buffers and const buffers.
 * The latter are simple malloc'd blocks of memory.
 */
struct llvmpipe_resource
{
   struct pipe_resource base;

   /** an extra screen pointer to avoid crashing in driver trace */
   struct llvmpipe_screen *screen;

   /** Row stride in bytes */
   unsigned row_stride[LP_MAX_TEXTURE_LEVELS];
   /** Image stride (for cube maps, array or 3D textures) in bytes */
   uint64_t img_stride[LP_MAX_TEXTURE_LEVELS];
   /** Offset to start of mipmap level, in bytes */
   uint64_t mip_offsets[LP_MAX_TEXTURE_LEVELS];
   /** allocated total size (for non-display target texture resources only) */
   uint64_t total_alloc_size;

   /**
    * Display target, for textures with the PIPE_BIND_DISPLAY_TARGET
    * usage.
    */
   struct sw_displaytarget *dt;
   enum pipe_format dt_format;

   /**
    * Malloc'ed data for regular textures, or a mapping to dt above.
    */
   void *tex_data;

   BITSET_WORD *residency;

   /**
    * Data for non-texture resources.
    */
   void *data;

   bool user_ptr;  /** Is this a user-space buffer? */
   unsigned timestamp;

   unsigned id;  /**< temporary, for debugging */

   unsigned sample_stride;

   uint64_t size_required;
   uint64_t backing_offset;
#ifdef HAVE_LIBDRM
   struct llvmpipe_memory_allocation *dmabuf_alloc;
#endif
   bool backable;
   struct pipe_memory_object *imported_memory;
   bool dmabuf;
#if MESA_DEBUG
   struct list_head list;
#endif
};


struct llvmpipe_transfer
{
   struct pipe_transfer base;
   void *map;
   struct pipe_box block_box;
};

struct llvmpipe_memory_allocation
{
   int fd;
   uint64_t offset;
   void *cpu_addr;
   uint64_t size;
   enum llvmpipe_memory_fd_type type;
   int mem_fd;
   int dmabuf_fd;
};

struct llvmpipe_memory_object
{
   struct pipe_memory_object b;
   struct pipe_reference reference;
   struct llvmpipe_memory_allocation *mem_alloc;
   uint64_t size;
};


/** cast wrappers */
static inline struct llvmpipe_resource *
llvmpipe_resource(struct pipe_resource *pt)
{
   return (struct llvmpipe_resource *) pt;
}


static inline const struct llvmpipe_resource *
llvmpipe_resource_const(const struct pipe_resource *pt)
{
   return (const struct llvmpipe_resource *) pt;
}


static inline struct llvmpipe_transfer *
llvmpipe_transfer(struct pipe_transfer *pt)
{
   return (struct llvmpipe_transfer *) pt;
}


static inline struct llvmpipe_memory_object *
llvmpipe_memory_object(struct pipe_memory_object *pt)
{
   return (struct llvmpipe_memory_object *) pt;
}


void llvmpipe_init_screen_resource_funcs(struct pipe_screen *screen);
void llvmpipe_init_context_resource_funcs(struct pipe_context *pipe);


static inline bool
llvmpipe_resource_is_texture(const struct pipe_resource *resource)
{
   switch (resource->target) {
   case PIPE_BUFFER:
      return false;
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_3D:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      return true;
   default:
      assert(0);
      return false;
   }
}


static inline bool
llvmpipe_resource_is_1d(const struct pipe_resource *resource)
{
   switch (resource->target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
      return true;
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_3D:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
      return false;
   default:
      assert(0);
      return false;
   }
}


static inline unsigned
llvmpipe_layer_stride(struct pipe_resource *resource,
                      unsigned level)
{
   struct llvmpipe_resource *lpr = llvmpipe_resource(resource);
   assert(level < LP_MAX_TEXTURE_2D_LEVELS);
   return lpr->img_stride[level];
}


static inline unsigned
llvmpipe_resource_stride(struct pipe_resource *resource,
                         unsigned level)
{
   struct llvmpipe_resource *lpr = llvmpipe_resource(resource);
   assert(level < LP_MAX_TEXTURE_2D_LEVELS);
   return lpr->row_stride[level];
}


static inline unsigned
llvmpipe_sample_stride(struct pipe_resource *resource)
{
   struct llvmpipe_resource *lpr = llvmpipe_resource(resource);
   return lpr->sample_stride;
}


void *
llvmpipe_resource_map(struct pipe_resource *resource,
                      unsigned level,
                      unsigned layer,
                      enum lp_texture_usage tex_usage);

void
llvmpipe_resource_unmap(struct pipe_resource *resource,
                        unsigned level,
                        unsigned layer);


void *
llvmpipe_resource_data(struct pipe_resource *resource);


unsigned
llvmpipe_resource_size(const struct pipe_resource *resource);


uint8_t *
llvmpipe_get_texture_image_address(struct llvmpipe_resource *lpr,
                                   unsigned face_slice, unsigned level);


extern void
llvmpipe_print_resources(void);


#define LP_UNREFERENCED         0
#define LP_REFERENCED_FOR_READ  (1 << 0)
#define LP_REFERENCED_FOR_WRITE (1 << 1)


unsigned int
llvmpipe_is_resource_referenced(struct pipe_context *pipe,
                                struct pipe_resource *presource,
                                unsigned level);

unsigned
llvmpipe_get_format_alignment(enum pipe_format format);


void *
llvmpipe_transfer_map_ms(struct pipe_context *pipe,
                         struct pipe_resource *resource,
                         unsigned level,
                         unsigned usage,
                         unsigned sample,
                         const struct pipe_box *box,
                         struct pipe_transfer **transfer);

uint32_t
llvmpipe_get_texel_offset(struct pipe_resource *resource,
                          uint32_t level, uint32_t x,
                          uint32_t y, uint32_t z);

#endif /* LP_TEXTURE_H */
