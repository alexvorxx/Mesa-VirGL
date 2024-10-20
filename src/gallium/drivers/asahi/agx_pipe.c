/*
 * Copyright 2010 Red Hat Inc.
 * Copyright 2014-2017 Broadcom
 * Copyright 2019-2020 Collabora, Ltd.
 * Copyright 2006 VMware, Inc.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <xf86drm.h>
#include "asahi/compiler/agx_compile.h"
#include "asahi/layout/layout.h"
#include "asahi/lib/decode.h"
#include "asahi/lib/unstable_asahi_drm.h"
#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "gallium/auxiliary/renderonly/renderonly.h"
#include "gallium/auxiliary/util/u_debug_cb.h"
#include "gallium/auxiliary/util/u_framebuffer.h"
#include "gallium/auxiliary/util/u_sample_positions.h"
#include "gallium/auxiliary/util/u_surface.h"
#include "gallium/auxiliary/util/u_transfer.h"
#include "gallium/auxiliary/util/u_transfer_helper.h"
#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/bitscan.h"
#include "util/format/u_format.h"
#include "util/format/u_formats.h"
#include "util/half_float.h"
#include "util/macros.h"
#include "util/simple_mtx.h"
#include "util/timespec.h"
#include "util/u_drm.h"
#include "util/u_gen_mipmap.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_process.h"
#include "util/u_resource.h"
#include "util/u_screen.h"
#include "util/u_upload_mgr.h"
#include "util/xmlconfig.h"
#include "agx_bg_eot.h"
#include "agx_device.h"
#include "agx_disk_cache.h"
#include "agx_fence.h"
#include "agx_helpers.h"
#include "agx_pack.h"
#include "agx_public.h"
#include "agx_state.h"
#include "agx_tilebuffer.h"
#include "shader_enums.h"

/* Fake values, pending UAPI upstreaming */
#ifndef DRM_FORMAT_MOD_APPLE_TWIDDLED
#define DRM_FORMAT_MOD_APPLE_TWIDDLED (2)
#endif
#ifndef DRM_FORMAT_MOD_APPLE_TWIDDLED_COMPRESSED
#define DRM_FORMAT_MOD_APPLE_TWIDDLED_COMPRESSED (3)
#endif

uint64_t agx_best_modifiers[] = {
   DRM_FORMAT_MOD_APPLE_TWIDDLED_COMPRESSED,
   DRM_FORMAT_MOD_APPLE_TWIDDLED,
   DRM_FORMAT_MOD_LINEAR,
};

/* These limits are arbitrarily chosen and subject to change as
 * we discover more workloads with heavy shadowing.
 *
 * Maximum size of a shadowed object in bytes.
 * Hint: 1024x1024xRGBA8 = 4 MiB. Go higher for compression.
 */
#define MAX_SHADOW_BYTES (6 * 1024 * 1024)

/* Maximum cumulative size to shadow an object before we flush.
 * Allows shadowing a 4MiB + meta object 8 times with the logic
 * below (+1 shadow offset implied).
 */
#define MAX_TOTAL_SHADOW_BYTES (32 * 1024 * 1024)

void agx_init_state_functions(struct pipe_context *ctx);

/*
 * resource
 */

const static char *s_tiling[] = {
   [AIL_TILING_LINEAR] = "LINR",
   [AIL_TILING_TWIDDLED] = "TWID",
   [AIL_TILING_TWIDDLED_COMPRESSED] = "COMP",
};

#define rsrc_debug(res, ...)                                                   \
   do {                                                                        \
      if (agx_device((res)->base.screen)->debug & AGX_DBG_RESOURCE)            \
         agx_msg(__VA_ARGS__);                                                 \
   } while (0)

static void
agx_resource_debug(struct agx_resource *res, const char *msg)
{
   if (!(agx_device(res->base.screen)->debug & AGX_DBG_RESOURCE))
      return;

   int ino = -1;
   if (res->bo->prime_fd >= 0) {
      struct stat sb;
      if (!fstat(res->bo->prime_fd, &sb))
         ino = sb.st_ino;
   }

   agx_msg(
      "%s%s %dx%dx%d %dL %d/%dM %dS M:%llx %s %s%s S:0x%llx LS:0x%llx CS:0x%llx "
      "Base=0x%llx Size=0x%llx Meta=0x%llx/0x%llx (%s) %s%s%s%s%s%sfd:%d(%d) @ %p\n",
      msg ?: "", util_format_short_name(res->base.format), res->base.width0,
      res->base.height0, res->base.depth0, res->base.array_size,
      res->base.last_level, res->layout.levels, res->layout.sample_count_sa,
      (long long)res->modifier, s_tiling[res->layout.tiling],
      res->layout.mipmapped_z ? "MZ " : "",
      res->layout.page_aligned_layers ? "PL " : "",
      (long long)res->layout.linear_stride_B,
      (long long)res->layout.layer_stride_B,
      (long long)res->layout.compression_layer_stride_B,
      (long long)res->bo->va->addr, (long long)res->layout.size_B,
      res->layout.metadata_offset_B
         ? ((long long)res->bo->va->addr + res->layout.metadata_offset_B)
         : 0,
      (long long)res->layout.metadata_offset_B, res->bo->label,
      res->bo->flags & AGX_BO_SHARED ? "SH " : "",
      res->bo->flags & AGX_BO_LOW_VA ? "LO " : "",
      res->bo->flags & AGX_BO_EXEC ? "EX " : "",
      res->bo->flags & AGX_BO_WRITEBACK ? "WB " : "",
      res->bo->flags & AGX_BO_SHAREABLE ? "SA " : "",
      res->bo->flags & AGX_BO_READONLY ? "RO " : "", res->bo->prime_fd, ino,
      res);
}

static void
agx_resource_setup(struct agx_device *dev, struct agx_resource *nresource)
{
   struct pipe_resource *templ = &nresource->base;

   nresource->layout = (struct ail_layout){
      .tiling = ail_drm_modifier_to_tiling(nresource->modifier),
      .mipmapped_z = templ->target == PIPE_TEXTURE_3D,
      .format = templ->format,
      .width_px = templ->width0,
      .height_px = templ->height0,
      .depth_px = templ->depth0 * templ->array_size,
      .sample_count_sa = MAX2(templ->nr_samples, 1),
      .levels = templ->last_level + 1,
      .writeable_image = templ->bind & PIPE_BIND_SHADER_IMAGE,

      /* Ostensibly this should be based on the bind, but Gallium bind flags are
       * notoriously unreliable. The only cost of setting this excessively is a
       * bit of extra memory use for layered textures, which isn't worth trying
       * to optimize.
       */
      .renderable = true,
   };
}

static struct pipe_resource *
agx_resource_from_handle(struct pipe_screen *pscreen,
                         const struct pipe_resource *templat,
                         struct winsys_handle *whandle, unsigned usage)
{
   struct agx_device *dev = agx_device(pscreen);
   struct agx_resource *rsc;
   struct pipe_resource *prsc;

   assert(whandle->type == WINSYS_HANDLE_TYPE_FD);

   rsc = CALLOC_STRUCT(agx_resource);
   if (!rsc)
      return NULL;

   rsc->modifier = whandle->modifier == DRM_FORMAT_MOD_INVALID
                      ? DRM_FORMAT_MOD_LINEAR
                      : whandle->modifier;

   /* We need strides to be aligned. ail asserts this, but we want to fail
    * gracefully so the app can handle the error.
    */
   if (rsc->modifier == DRM_FORMAT_MOD_LINEAR && (whandle->stride % 16) != 0) {
      FREE(rsc);
      return false;
   }

   prsc = &rsc->base;

   *prsc = *templat;

   pipe_reference_init(&prsc->reference, 1);
   prsc->screen = pscreen;

   rsc->bo = agx_bo_import(dev, whandle->handle);
   /* Sometimes an import can fail e.g. on an invalid buffer fd, out of
    * memory space to mmap it etc.
    */
   if (!rsc->bo) {
      FREE(rsc);
      return NULL;
   }

   agx_resource_setup(dev, rsc);

   if (rsc->layout.tiling == AIL_TILING_LINEAR) {
      rsc->layout.linear_stride_B = whandle->stride;
   } else if (whandle->stride != ail_get_wsi_stride_B(&rsc->layout, 0)) {
      FREE(rsc);
      return NULL;
   }

   assert(whandle->offset == 0);

   ail_make_miptree(&rsc->layout);

   if (prsc->target == PIPE_BUFFER) {
      assert(rsc->layout.tiling == AIL_TILING_LINEAR);
      util_range_init(&rsc->valid_buffer_range);
   }

   agx_resource_debug(rsc, "Import: ");

   return prsc;
}

static bool
agx_resource_get_handle(struct pipe_screen *pscreen, struct pipe_context *ctx,
                        struct pipe_resource *pt, struct winsys_handle *handle,
                        unsigned usage)
{
   struct agx_device *dev = agx_device(pscreen);
   struct pipe_resource *cur = pt;

   /* Even though asahi doesn't support multi-planar formats, we
    * can get here through GBM, which does. Walk the list of planes
    * to find the right one.
    */
   for (int i = 0; i < handle->plane; i++) {
      cur = cur->next;
      if (!cur)
         return false;
   }

   struct agx_resource *rsrc = agx_resource(cur);

   if (handle->type == WINSYS_HANDLE_TYPE_KMS && dev->ro) {
      rsrc_debug(rsrc, "Get handle: %p (KMS RO)\n", rsrc);

      if (!rsrc->scanout && dev->ro && (rsrc->base.bind & PIPE_BIND_SCANOUT)) {
         rsrc->scanout =
            renderonly_scanout_for_resource(&rsrc->base, dev->ro, NULL);
      }

      if (!rsrc->scanout)
         return false;

      return renderonly_get_handle(rsrc->scanout, handle);
   } else if (handle->type == WINSYS_HANDLE_TYPE_KMS) {
      rsrc_debug(rsrc, "Get handle: %p (KMS)\n", rsrc);

      handle->handle = rsrc->bo->handle;
   } else if (handle->type == WINSYS_HANDLE_TYPE_FD) {
      int fd = agx_bo_export(dev, rsrc->bo);

      if (fd < 0)
         return false;

      handle->handle = fd;
      if (dev->debug & AGX_DBG_RESOURCE) {
         struct stat sb;
         fstat(rsrc->bo->prime_fd, &sb);
         agx_msg("Get handle: %p (FD %d/%ld)\n", rsrc, fd, (long)sb.st_ino);
      }
   } else {
      /* Other handle types not supported */
      return false;
   }

   handle->stride = ail_get_wsi_stride_B(&rsrc->layout, 0);
   handle->size = rsrc->layout.size_B;
   handle->offset = rsrc->layout.level_offsets_B[0];
   handle->format = rsrc->layout.format;
   handle->modifier = rsrc->modifier;

   return true;
}

static bool
agx_resource_get_param(struct pipe_screen *pscreen, struct pipe_context *pctx,
                       struct pipe_resource *prsc, unsigned plane,
                       unsigned layer, unsigned level,
                       enum pipe_resource_param param, unsigned usage,
                       uint64_t *value)
{
   struct agx_resource *rsrc = (struct agx_resource *)prsc;

   switch (param) {
   case PIPE_RESOURCE_PARAM_STRIDE:
      *value = ail_get_wsi_stride_B(&rsrc->layout, level);
      return true;
   case PIPE_RESOURCE_PARAM_OFFSET:
      *value = rsrc->layout.level_offsets_B[level];
      return true;
   case PIPE_RESOURCE_PARAM_MODIFIER:
      *value = rsrc->modifier;
      return true;
   case PIPE_RESOURCE_PARAM_NPLANES:
      /* We don't support multi-planar formats, but we should still handle
       * this case for GBM shared resources.
       */
      *value = util_resource_num(prsc);
      return true;
   default:
      return false;
   }
}

static bool
agx_is_2d(enum pipe_texture_target target)
{
   return (target == PIPE_TEXTURE_2D || target == PIPE_TEXTURE_RECT);
}

static bool
agx_linear_allowed(const struct agx_resource *pres)
{
   /* Mipmapping not allowed with linear */
   if (pres->base.last_level != 0)
      return false;

   /* Depth/stencil buffers must not be linear */
   if (pres->base.bind & PIPE_BIND_DEPTH_STENCIL)
      return false;

   /* Multisampling not allowed with linear */
   if (pres->base.nr_samples > 1)
      return false;

   /* Block compression not allowed with linear */
   if (util_format_is_compressed(pres->base.format))
      return false;

   switch (pres->base.target) {
   /* Buffers are always linear, even with image atomics */
   case PIPE_BUFFER:

   /* Linear textures require specifying their strides explicitly, which only
    * works for 2D textures. Rectangle textures are a special case of 2D.
    *
    * 1D textures only exist in GLES and are lowered to 2D to bypass hardware
    * limitations.
    *
    * However, we don't want to support this case in the image atomic
    * implementation, so linear shader images are specially forbidden.
    */
   case PIPE_TEXTURE_1D:
   case PIPE_TEXTURE_1D_ARRAY:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_RECT:
      if (pres->base.bind & PIPE_BIND_SHADER_IMAGE)
         return false;

      break;

   /* No other texture type can specify a stride */
   default:
      return false;
   }

   return true;
}

static bool
agx_twiddled_allowed(const struct agx_resource *pres)
{
   /* Certain binds force linear */
   if (pres->base.bind & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_LINEAR))
      return false;

   /* Buffers must be linear */
   if (pres->base.target == PIPE_BUFFER)
      return false;

   /* Anything else may be twiddled */
   return true;
}

static bool
agx_compression_allowed(const struct agx_resource *pres)
{
   /* Allow disabling compression for debugging */
   if (agx_device(pres->base.screen)->debug & AGX_DBG_NOCOMPRESS) {
      rsrc_debug(pres, "No compression: disabled\n");
      return false;
   }

   /* Limited to renderable */
   if (pres->base.bind &
       ~(PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET |
         PIPE_BIND_DEPTH_STENCIL | PIPE_BIND_SHARED | PIPE_BIND_SCANOUT)) {
      rsrc_debug(pres, "No compression: not renderable\n");
      return false;
   }

   if (!ail_can_compress(pres->base.format, pres->base.width0,
                         pres->base.height0, MAX2(pres->base.nr_samples, 1))) {
      rsrc_debug(pres, "No compression: incompatible layout\n");
      return false;
   }

   if (pres->base.format == PIPE_FORMAT_R9G9B9E5_FLOAT) {
      rsrc_debug(pres, "No compression: RGB9E5 copies need work\n");
      return false;
   }

   return true;
}

static uint64_t
agx_select_modifier_from_list(const struct agx_resource *pres,
                              const uint64_t *modifiers, int count)
{
   if (agx_twiddled_allowed(pres) && agx_compression_allowed(pres) &&
       drm_find_modifier(DRM_FORMAT_MOD_APPLE_TWIDDLED_COMPRESSED, modifiers,
                         count))
      return DRM_FORMAT_MOD_APPLE_TWIDDLED_COMPRESSED;

   if (agx_twiddled_allowed(pres) &&
       drm_find_modifier(DRM_FORMAT_MOD_APPLE_TWIDDLED, modifiers, count))
      return DRM_FORMAT_MOD_APPLE_TWIDDLED;

   if (agx_linear_allowed(pres) &&
       drm_find_modifier(DRM_FORMAT_MOD_LINEAR, modifiers, count))
      return DRM_FORMAT_MOD_LINEAR;

   /* We didn't find anything */
   return DRM_FORMAT_MOD_INVALID;
}

static uint64_t
agx_select_best_modifier(const struct agx_resource *pres)
{
   /* Prefer linear for staging resources, which should be as fast as possible
    * to write from the CPU.
    */
   if (agx_linear_allowed(pres) && pres->base.usage == PIPE_USAGE_STAGING)
      return DRM_FORMAT_MOD_LINEAR;

   /* For SCANOUT or SHARED resources with no explicit modifier selection, force
    * linear since we cannot expect consumers to correctly pass through the
    * modifier (unless linear is not allowed at all).
    */
   if (agx_linear_allowed(pres) &&
       pres->base.bind & (PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)) {
      return DRM_FORMAT_MOD_LINEAR;
   }

   if (agx_twiddled_allowed(pres)) {
      if (agx_compression_allowed(pres))
         return DRM_FORMAT_MOD_APPLE_TWIDDLED_COMPRESSED;
      else
         return DRM_FORMAT_MOD_APPLE_TWIDDLED;
   }

   if (agx_linear_allowed(pres))
      return DRM_FORMAT_MOD_LINEAR;
   else
      return DRM_FORMAT_MOD_INVALID;
}

static struct pipe_resource *
agx_resource_create_with_modifiers(struct pipe_screen *screen,
                                   const struct pipe_resource *templ,
                                   const uint64_t *modifiers, int count)
{
   struct agx_device *dev = agx_device(screen);
   struct agx_resource *nresource;

   nresource = CALLOC_STRUCT(agx_resource);
   if (!nresource)
      return NULL;

   nresource->base = *templ;
   nresource->base.screen = screen;

   if (modifiers) {
      nresource->modifier =
         agx_select_modifier_from_list(nresource, modifiers, count);
   } else {
      nresource->modifier = agx_select_best_modifier(nresource);
   }

   /* There may not be a matching modifier, bail if so */
   if (nresource->modifier == DRM_FORMAT_MOD_INVALID) {
      free(nresource);
      return NULL;
   }

   /* If there's only 1 layer and there's no compression, there's no harm in
    * inferring the shader image flag. Do so to avoid reallocation in case the
    * resource is later used as an image.
    */
   if (nresource->modifier != DRM_FORMAT_MOD_APPLE_TWIDDLED_COMPRESSED &&
       templ->depth0 == 1) {

      nresource->base.bind |= PIPE_BIND_SHADER_IMAGE;
   }

   nresource->mipmapped = (templ->last_level > 0);

   assert(templ->format != PIPE_FORMAT_Z24X8_UNORM &&
          templ->format != PIPE_FORMAT_Z24_UNORM_S8_UINT &&
          "u_transfer_helper should have lowered");

   agx_resource_setup(dev, nresource);

   pipe_reference_init(&nresource->base.reference, 1);

   ail_make_miptree(&nresource->layout);

   /* Fail Piglit's obnoxious allocations */
   if (nresource->layout.size_B >= (1ull << 32)) {
      free(nresource);
      return NULL;
   }

   if (templ->target == PIPE_BUFFER) {
      assert(nresource->layout.tiling == AIL_TILING_LINEAR);
      util_range_init(&nresource->valid_buffer_range);
   }

   /* Guess a label based on the bind */
   unsigned bind = templ->bind;

   const char *label = (bind & PIPE_BIND_INDEX_BUFFER)     ? "Index buffer"
                       : (bind & PIPE_BIND_SCANOUT)        ? "Scanout"
                       : (bind & PIPE_BIND_DISPLAY_TARGET) ? "Display target"
                       : (bind & PIPE_BIND_SHARED)         ? "Shared resource"
                       : (bind & PIPE_BIND_RENDER_TARGET)  ? "Render target"
                       : (bind & PIPE_BIND_DEPTH_STENCIL)
                          ? "Depth/stencil buffer"
                       : (bind & PIPE_BIND_SAMPLER_VIEW)    ? "Texture"
                       : (bind & PIPE_BIND_VERTEX_BUFFER)   ? "Vertex buffer"
                       : (bind & PIPE_BIND_CONSTANT_BUFFER) ? "Constant buffer"
                       : (bind & PIPE_BIND_GLOBAL)          ? "Global memory"
                       : (bind & PIPE_BIND_SHADER_BUFFER)   ? "Shader buffer"
                       : (bind & PIPE_BIND_SHADER_IMAGE)    ? "Shader image"
                                                            : "Other resource";

   uint32_t create_flags = 0;

   /* Default to write-combine resources, but use writeback if that is expected
    * to be beneficial.
    */
   if (nresource->base.usage == PIPE_USAGE_STAGING ||
       (nresource->base.flags & PIPE_RESOURCE_FLAG_MAP_COHERENT)) {

      create_flags |= AGX_BO_WRITEBACK;
   }

   /* Allow disabling write-combine to debug performance issues */
   if (dev->debug & AGX_DBG_NOWC) {
      create_flags |= AGX_BO_WRITEBACK;
   }

   /* Create buffers that might be shared with the SHAREABLE flag */
   if (bind & (PIPE_BIND_SCANOUT | PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SHARED))
      create_flags |= AGX_BO_SHAREABLE;

   nresource->bo =
      agx_bo_create(dev, nresource->layout.size_B, 0, create_flags, label);

   if (!nresource->bo) {
      FREE(nresource);
      return NULL;
   }

   agx_resource_debug(nresource, "New: ");
   return &nresource->base;
}

static struct pipe_resource *
agx_resource_create(struct pipe_screen *screen,
                    const struct pipe_resource *templ)
{
   return agx_resource_create_with_modifiers(screen, templ, NULL, 0);
}

static void
agx_resource_destroy(struct pipe_screen *screen, struct pipe_resource *prsrc)
{
   struct agx_resource *rsrc = (struct agx_resource *)prsrc;
   struct agx_screen *agx_screen = (struct agx_screen *)screen;

   agx_resource_debug(rsrc, "Destroy: ");

   if (prsrc->target == PIPE_BUFFER)
      util_range_destroy(&rsrc->valid_buffer_range);

   if (rsrc->scanout)
      renderonly_scanout_destroy(rsrc->scanout, agx_screen->dev.ro);

   agx_bo_unreference(&agx_screen->dev, rsrc->bo);
   FREE(rsrc);
}

void
agx_batch_track_image(struct agx_batch *batch, struct pipe_image_view *image)
{
   struct agx_resource *rsrc = agx_resource(image->resource);

   if (image->shader_access & PIPE_IMAGE_ACCESS_WRITE) {
      batch->incoherent_writes = true;

      if (rsrc->base.target == PIPE_BUFFER) {
         agx_batch_writes_range(batch, rsrc, image->u.buf.offset,
                                image->u.buf.size);
      } else {
         agx_batch_writes(batch, rsrc, image->u.tex.level);
      }
   } else {
      agx_batch_reads(batch, rsrc);
   }
}

/*
 * transfer
 */

static void
agx_transfer_flush_region(struct pipe_context *pipe,
                          struct pipe_transfer *transfer,
                          const struct pipe_box *box)
{
}

/* Reallocate the backing buffer of a resource, returns true if successful */
static bool
agx_shadow(struct agx_context *ctx, struct agx_resource *rsrc, bool needs_copy)
{
   struct agx_device *dev = agx_device(ctx->base.screen);
   struct agx_bo *old = rsrc->bo;
   size_t size = rsrc->layout.size_B;
   unsigned flags = old->flags;

   if (dev->debug & AGX_DBG_NOSHADOW)
      return false;

   /* If a resource is (or could be) shared, shadowing would desync across
    * processes. (It's also not what this path is for.)
    */
   if (flags & (AGX_BO_SHARED | AGX_BO_SHAREABLE))
      return false;

   /* Do not shadow resources that are too large */
   if (size > MAX_SHADOW_BYTES && needs_copy)
      return false;

   /* Do not shadow resources too much */
   if (rsrc->shadowed_bytes >= MAX_TOTAL_SHADOW_BYTES && needs_copy)
      return false;

   rsrc->shadowed_bytes += size;

   /* If we need to copy, we reallocate the resource with cached-coherent
    * memory. This is a heuristic: it assumes that if the app needs a shadows
    * (with a copy) now, it will again need to shadow-and-copy the same resource
    * in the future. This accelerates the later copies, since otherwise the copy
    * involves reading uncached memory.
    */
   if (needs_copy)
      flags |= AGX_BO_WRITEBACK;

   struct agx_bo *new_ = agx_bo_create(dev, size, 0, flags, old->label);

   /* If allocation failed, we can fallback on a flush gracefully*/
   if (new_ == NULL)
      return false;

   if (needs_copy) {
      perf_debug_ctx(ctx, "Shadowing %zu bytes on the CPU (%s)", size,
                     (old->flags & AGX_BO_WRITEBACK) ? "cached" : "uncached");
      agx_resource_debug(rsrc, "Shadowed: ");

      memcpy(new_->map, old->map, size);
   }

   /* Swap the pointers, dropping a reference */
   agx_bo_unreference(dev, rsrc->bo);
   rsrc->bo = new_;

   /* Reemit descriptors using this resource */
   agx_dirty_all(ctx);
   return true;
}

/*
 * Perform the required synchronization before a transfer_map operation can
 * complete. This may require syncing batches.
 */
static void
agx_prepare_for_map(struct agx_context *ctx, struct agx_resource *rsrc,
                    unsigned level,
                    unsigned usage, /* a combination of PIPE_MAP_x */
                    const struct pipe_box *box, bool staging_blit)
{
   /* GPU access does not require explicit syncs, as the batch tracking logic
    * will ensure correct ordering automatically.
    */
   if (staging_blit)
      return;

   /* If the level has not been written, we may freely do CPU access (writes),
    * even if other levels are being written by the GPU. This lets us write some
    * mip levels on the CPU and some on the GPU, without stalling.
    */
   if (!agx_resource_valid(rsrc, level))
      return;

   /* Upgrade DISCARD_RANGE to WHOLE_RESOURCE if the whole resource is
    * being mapped.
    */
   if ((usage & PIPE_MAP_DISCARD_RANGE) &&
       !(rsrc->base.flags & PIPE_RESOURCE_FLAG_MAP_PERSISTENT) &&
       rsrc->base.last_level == 0 &&
       util_texrange_covers_whole_level(&rsrc->base, 0, box->x, box->y, box->z,
                                        box->width, box->height, box->depth)) {

      usage |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
   }

   /* Shadowing doesn't work separate stencil or shared resources */
   if (rsrc->separate_stencil || (rsrc->bo->flags & AGX_BO_SHARED))
      usage &= ~PIPE_MAP_DISCARD_WHOLE_RESOURCE;

   /* If the access is unsynchronized, there's nothing to do */
   if (usage & PIPE_MAP_UNSYNCHRONIZED)
      return;

   /* If the range being accessed is uninitialized, we do not need to sync. */
   if (rsrc->base.target == PIPE_BUFFER && !(rsrc->bo->flags & AGX_BO_SHARED) &&
       !util_ranges_intersect(&rsrc->valid_buffer_range, box->x,
                              box->x + box->width))
      return;

   /* Everything after this needs the context, which is not safe for
    * unsynchronized transfers when we claim
    * PIPE_CAP_MAP_UNSYNCHRONIZED_THREAD_SAFE.
    */
   assert(!(usage & PIPE_MAP_UNSYNCHRONIZED));

   /* Reading or writing from the CPU requires syncing writers. */
   agx_sync_writer(ctx, rsrc, "Unsynchronized CPU transfer");

   /* Additionally, writing needs readers synced. */
   if (!(usage & PIPE_MAP_WRITE))
      return;

   /* If there are no readers, we're done. We check at the start to
    * avoid expensive shadowing paths or duplicated checks in this hapyp path.
    */
   if (!agx_any_batch_uses_resource(ctx, rsrc)) {
      rsrc->shadowed_bytes = 0;
      return;
   }

   /* There are readers. Try to invalidate the resource to avoid a sync */
   if ((usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE) &&
       agx_shadow(ctx, rsrc, false))
      return;

   /* Or try to shadow it */
   if (!(rsrc->base.flags & PIPE_RESOURCE_FLAG_MAP_PERSISTENT) &&
       agx_shadow(ctx, rsrc, true))
      return;

   /* Otherwise, we need to sync */
   agx_sync_readers(ctx, rsrc, "Unsynchronized write");

   rsrc->shadowed_bytes = 0;
}

/*
 * Return a colour-renderable format compatible with a depth/stencil format, to
 * be used as an interchange format for depth/stencil blits. For
 * non-depth/stencil formats, returns the format itself, except when that format
 * would not round-trip so we return a compatible roundtrippable format.
 */
static enum pipe_format
agx_staging_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_Z16_UNORM:
      return PIPE_FORMAT_R16_UNORM;
   case PIPE_FORMAT_Z32_FLOAT:
      return PIPE_FORMAT_R32_FLOAT;
   case PIPE_FORMAT_S8_UINT:
      return PIPE_FORMAT_R8_UINT;
   default:
      /* Z24 and combined Z/S are lowered to one of the above formats by
       * u_transfer_helper. The caller needs to pass in the rsrc->layout.format
       * and not the rsrc->base.format to get the lowered physical format
       * (rather than the API logical format).
       */
      assert(!util_format_is_depth_or_stencil(format) &&
             "no other depth/stencil formats allowed for staging");

      /* However, snorm does not round trip, so don't use that for staging */
      return util_format_snorm_to_sint(format);
   }
}

/* Most of the time we can do CPU-side transfers, but sometimes we need to use
 * the 3D pipe for this. Let's wrap u_blitter to blit to/from staging textures.
 * Code adapted from panfrost */

static struct agx_resource *
agx_alloc_staging(struct pipe_screen *screen, struct agx_resource *rsc,
                  unsigned level, const struct pipe_box *box)
{
   struct pipe_resource tmpl = rsc->base;

   tmpl.usage = PIPE_USAGE_STAGING;
   tmpl.width0 = box->width;
   tmpl.height0 = box->height;
   tmpl.depth0 = 1;

   /* We need a linear staging resource. We have linear 2D arrays, but not
    * linear 3D or cube textures. So switch to 2D arrays if needed.
    */
   switch (tmpl.target) {
   case PIPE_TEXTURE_2D_ARRAY:
   case PIPE_TEXTURE_CUBE:
   case PIPE_TEXTURE_CUBE_ARRAY:
   case PIPE_TEXTURE_3D:
      tmpl.target = PIPE_TEXTURE_2D_ARRAY;
      tmpl.array_size = box->depth;
      break;
   default:
      assert(tmpl.array_size == 1);
      assert(box->depth == 1);
      break;
   }

   tmpl.last_level = 0;

   /* Linear is incompatible with depth/stencil, so we convert */
   tmpl.format = agx_staging_format(rsc->layout.format);
   tmpl.bind =
      PIPE_BIND_LINEAR | PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;

   struct pipe_resource *pstaging = screen->resource_create(screen, &tmpl);
   if (!pstaging)
      return NULL;

   return agx_resource(pstaging);
}

static void
agx_blit_from_staging(struct pipe_context *pctx, struct agx_transfer *trans)
{
   struct pipe_resource *dst = trans->base.resource;
   struct pipe_blit_info blit = {0};

   blit.dst.resource = dst;
   blit.dst.format = agx_staging_format(agx_resource(dst)->layout.format);
   blit.dst.level = trans->base.level;
   blit.dst.box = trans->base.box;
   blit.src.resource = trans->staging.rsrc;
   blit.src.format = blit.dst.format;
   blit.src.level = 0;
   blit.src.box = trans->staging.box;
   blit.mask = util_format_get_mask(blit.src.format);
   blit.filter = PIPE_TEX_FILTER_NEAREST;

   agx_blit(pctx, &blit);
}

static void
agx_blit_to_staging(struct pipe_context *pctx, struct agx_transfer *trans)
{
   struct pipe_resource *src = trans->base.resource;
   struct pipe_blit_info blit = {0};

   blit.src.resource = src;
   blit.src.format = agx_staging_format(agx_resource(src)->layout.format);
   blit.src.level = trans->base.level;
   blit.src.box = trans->base.box;
   blit.dst.resource = trans->staging.rsrc;
   blit.dst.format = blit.src.format;
   blit.dst.level = 0;
   blit.dst.box = trans->staging.box;
   blit.mask = util_format_get_mask(blit.dst.format);
   blit.filter = PIPE_TEX_FILTER_NEAREST;

   agx_blit(pctx, &blit);
}

static void *
agx_transfer_map(struct pipe_context *pctx, struct pipe_resource *resource,
                 unsigned level,
                 unsigned usage, /* a combination of PIPE_MAP_x */
                 const struct pipe_box *box,
                 struct pipe_transfer **out_transfer)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_resource *rsrc = agx_resource(resource);
   struct agx_device *dev = agx_device(ctx->base.screen);

   /* Can't map tiled/compressed directly */
   if ((usage & PIPE_MAP_DIRECTLY) && rsrc->modifier != DRM_FORMAT_MOD_LINEAR)
      return NULL;

   /* Can't transfer out of bounds mip levels */
   if (level >= rsrc->layout.levels)
      return NULL;

   /* For compression, we use a staging blit as we do not implement AGX
    * compression in software. In some cases, we could use this path for
    * twiddled too, but we don't have a use case for that yet.
    */
   bool staging_blit = ail_is_level_compressed(&rsrc->layout, level);

   agx_prepare_for_map(ctx, rsrc, level, usage, box, staging_blit);

   /* Track the written buffer range */
   if (resource->target == PIPE_BUFFER) {
      /* Note the ordering: DISCARD|WRITE is valid, so clear before adding. */
      if (usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE)
         util_range_set_empty(&rsrc->valid_buffer_range);
      if (usage & PIPE_MAP_WRITE) {
         util_range_add(resource, &rsrc->valid_buffer_range, box->x,
                        box->x + box->width);
      }
   }

   struct agx_transfer *transfer = CALLOC_STRUCT(agx_transfer);
   transfer->base.level = level;
   transfer->base.usage = usage;
   transfer->base.box = *box;

   pipe_resource_reference(&transfer->base.resource, resource);
   *out_transfer = &transfer->base;

   if (staging_blit) {
      /* Should never happen for buffers, and it's not safe */
      assert(resource->target != PIPE_BUFFER);

      struct agx_resource *staging =
         agx_alloc_staging(pctx->screen, rsrc, level, box);
      assert(staging);

      /* Staging resources have one LOD: level 0. Query the strides
       * on this LOD.
       */
      transfer->base.stride = ail_get_linear_stride_B(&staging->layout, 0);
      transfer->base.layer_stride = staging->layout.layer_stride_B;
      transfer->staging.rsrc = &staging->base;

      transfer->staging.box = *box;
      transfer->staging.box.x = 0;
      transfer->staging.box.y = 0;
      transfer->staging.box.z = 0;

      assert(transfer->staging.rsrc != NULL);

      if ((usage & PIPE_MAP_READ) && agx_resource_valid(rsrc, level)) {
         agx_blit_to_staging(pctx, transfer);
         agx_sync_writer(ctx, staging, "GPU read staging blit");
      }

      dev->ops.bo_mmap(dev, staging->bo);
      return staging->bo->map;
   }

   dev->ops.bo_mmap(dev, rsrc->bo);

   if (ail_is_level_twiddled_uncompressed(&rsrc->layout, level)) {
      /* Should never happen for buffers, and it's not safe */
      assert(resource->target != PIPE_BUFFER);

      transfer->base.stride =
         util_format_get_stride(rsrc->layout.format, box->width);

      transfer->base.layer_stride = util_format_get_2d_size(
         rsrc->layout.format, transfer->base.stride, box->height);

      transfer->map = calloc(transfer->base.layer_stride, box->depth);

      if ((usage & PIPE_MAP_READ) && agx_resource_valid(rsrc, level)) {
         for (unsigned z = 0; z < box->depth; ++z) {
            uint8_t *map = agx_map_texture_cpu(rsrc, level, box->z + z);
            uint8_t *dst =
               (uint8_t *)transfer->map + transfer->base.layer_stride * z;

            ail_detile(map, dst, &rsrc->layout, level, transfer->base.stride,
                       box->x, box->y, box->width, box->height);
         }
      }

      return transfer->map;
   } else {
      assert(rsrc->modifier == DRM_FORMAT_MOD_LINEAR);

      transfer->base.stride = ail_get_linear_stride_B(&rsrc->layout, level);
      transfer->base.layer_stride = rsrc->layout.layer_stride_B;

      /* Be conservative for direct writes */
      if ((usage & PIPE_MAP_WRITE) &&
          (usage &
           (PIPE_MAP_DIRECTLY | PIPE_MAP_PERSISTENT | PIPE_MAP_COHERENT))) {
         BITSET_SET(rsrc->data_valid, level);
      }

      uint32_t offset =
         ail_get_linear_pixel_B(&rsrc->layout, level, box->x, box->y, box->z);

      return ((uint8_t *)rsrc->bo->map) + offset;
   }
}

static void
agx_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *transfer)
{
   /* Gallium expects writeback here, so we tile */

   struct agx_transfer *trans = agx_transfer(transfer);
   struct pipe_resource *prsrc = transfer->resource;
   struct agx_resource *rsrc = (struct agx_resource *)prsrc;

   if (trans->staging.rsrc && (transfer->usage & PIPE_MAP_WRITE)) {
      assert(prsrc->target != PIPE_BUFFER);
      agx_blit_from_staging(pctx, trans);
      agx_flush_readers(agx_context(pctx), agx_resource(trans->staging.rsrc),
                        "GPU write staging blit");
   } else if (trans->map && (transfer->usage & PIPE_MAP_WRITE)) {
      assert(
         ail_is_level_twiddled_uncompressed(&rsrc->layout, transfer->level));

      for (unsigned z = 0; z < transfer->box.depth; ++z) {
         uint8_t *map =
            agx_map_texture_cpu(rsrc, transfer->level, transfer->box.z + z);
         uint8_t *src = (uint8_t *)trans->map + transfer->layer_stride * z;

         ail_tile(map, src, &rsrc->layout, transfer->level, transfer->stride,
                  transfer->box.x, transfer->box.y, transfer->box.width,
                  transfer->box.height);
      }
   }

   /* The level we wrote is now initialized. We do this at the end so
    * blit_from_staging can avoid reloading existing contents.
    */
   if (transfer->usage & PIPE_MAP_WRITE)
      BITSET_SET(rsrc->data_valid, transfer->level);

   /* Free the transfer */
   free(trans->map);
   pipe_resource_reference(&trans->staging.rsrc, NULL);
   pipe_resource_reference(&transfer->resource, NULL);
   FREE(transfer);
}

/*
 * clear/copy
 */
static void
agx_clear(struct pipe_context *pctx, unsigned buffers,
          const struct pipe_scissor_state *scissor_state,
          const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_batch *batch = agx_get_batch(ctx);

   if (unlikely(!agx_render_condition_check(ctx)))
      return;

   unsigned fastclear = buffers & ~(batch->draw | batch->load);
   unsigned slowclear = buffers & ~fastclear;

   assert(scissor_state == NULL && "we don't support PIPE_CAP_CLEAR_SCISSORED");

   /* Fast clears configure the batch */
   for (unsigned rt = 0; rt < PIPE_MAX_COLOR_BUFS; ++rt) {
      if (!(fastclear & (PIPE_CLEAR_COLOR0 << rt)))
         continue;

      static_assert(sizeof(color->f) == 16, "mismatched structure");

      /* Clear colour must be clamped to properly handle signed ints. */
      union pipe_color_union clamped =
         util_clamp_color(batch->key.cbufs[rt]->format, color);

      batch->uploaded_clear_color[rt] = agx_pool_upload_aligned(
         &batch->pool, clamped.f, sizeof(clamped.f), 16);
   }

   if (fastclear & PIPE_CLEAR_DEPTH)
      batch->clear_depth = depth;

   if (fastclear & PIPE_CLEAR_STENCIL)
      batch->clear_stencil = stencil;

   /* Slow clears draw a fullscreen rectangle */
   if (slowclear) {
      agx_blitter_save(ctx, ctx->blitter, false /* render cond */);
      util_blitter_clear(
         ctx->blitter, ctx->framebuffer.width, ctx->framebuffer.height,
         util_framebuffer_get_num_layers(&ctx->framebuffer), slowclear, color,
         depth, stencil,
         util_framebuffer_get_num_samples(&ctx->framebuffer) > 1);
   }

   if (fastclear)
      agx_batch_init_state(batch);

   batch->clear |= fastclear;
   batch->resolve |= buffers;
   assert((batch->draw & slowclear) == slowclear);
}

static void
transition_resource(struct pipe_context *pctx, struct agx_resource *rsrc,
                    struct pipe_resource *templ)
{
   struct agx_resource *new_res =
      agx_resource(pctx->screen->resource_create(pctx->screen, templ));

   assert(new_res);
   assert(!(rsrc->base.bind & PIPE_BIND_SHARED) && "cannot swap BOs if shared");

   int level;
   BITSET_FOREACH_SET(level, rsrc->data_valid, PIPE_MAX_TEXTURE_LEVELS) {
      /* Copy each valid level */
      struct pipe_box box;
      u_box_3d(0, 0, 0, u_minify(rsrc->layout.width_px, level),
               u_minify(rsrc->layout.height_px, level),
               util_num_layers(&rsrc->base, level), &box);

      agx_resource_copy_region(pctx, &new_res->base, level, 0, 0, 0,
                               &rsrc->base, level, &box);
   }

   /* Flush the blits out, to make sure the old resource is no longer used */
   agx_flush_writer(agx_context(pctx), new_res, "flush_resource");

   /* Copy the bind flags and swap the BOs */
   struct agx_bo *old = rsrc->bo;
   rsrc->base.bind = new_res->base.bind;
   rsrc->layout = new_res->layout;
   rsrc->modifier = new_res->modifier;
   rsrc->bo = new_res->bo;
   new_res->bo = old;

   /* Free the new resource, which now owns the old BO */
   pipe_resource_reference((struct pipe_resource **)&new_res, NULL);
}

void
agx_decompress(struct agx_context *ctx, struct agx_resource *rsrc,
               const char *reason)
{
   if (rsrc->layout.tiling == AIL_TILING_TWIDDLED_COMPRESSED) {
      perf_debug_ctx(ctx, "Decompressing resource due to %s", reason);
   } else if (!rsrc->layout.writeable_image) {
      perf_debug_ctx(ctx, "Reallocating image due to %s", reason);
   }

   struct pipe_resource templ = rsrc->base;
   assert(!(templ.bind & PIPE_BIND_SHADER_IMAGE) && "currently compressed");
   templ.bind |= PIPE_BIND_SHADER_IMAGE /* forces off compression */;
   transition_resource(&ctx->base, rsrc, &templ);
}

static void
agx_flush_resource(struct pipe_context *pctx, struct pipe_resource *pres)
{
   struct agx_resource *rsrc = agx_resource(pres);

   /* flush_resource is used to prepare resources for sharing, so if this is not
    * already a shareabe resource, make it so
    */
   struct agx_bo *old = rsrc->bo;
   if (!(old->flags & AGX_BO_SHAREABLE)) {
      assert(rsrc->layout.levels == 1 &&
             "Shared resources must not be mipmapped");
      assert(rsrc->layout.sample_count_sa == 1 &&
             "Shared resources must not be multisampled");
      assert(rsrc->bo);
      assert(!(pres->bind & PIPE_BIND_SHARED));

      struct pipe_resource templ = *pres;
      templ.bind |= PIPE_BIND_SHARED;
      transition_resource(pctx, rsrc, &templ);
   } else {
      /* Otherwise just claim it's already shared */
      pres->bind |= PIPE_BIND_SHARED;
      agx_flush_writer(agx_context(pctx), rsrc, "flush_resource");
   }
}

#define MAX_ATTACHMENTS 16

struct attachments {
   struct drm_asahi_attachment list[MAX_ATTACHMENTS];
   size_t count;
};

static void
asahi_add_attachment(struct attachments *att, struct agx_resource *rsrc,
                     struct pipe_surface *surf)
{
   assert(att->count < MAX_ATTACHMENTS);
   int idx = att->count++;

   att->list[idx].size = rsrc->layout.size_B;
   att->list[idx].pointer = rsrc->bo->va->addr;
   att->list[idx].order = 1; // TODO: What does this do?
   att->list[idx].flags = 0;
}

static bool
is_aligned(unsigned x, unsigned pot_alignment)
{
   assert(util_is_power_of_two_nonzero(pot_alignment));
   return (x & (pot_alignment - 1)) == 0;
}

static void
agx_cmdbuf(struct agx_device *dev, struct drm_asahi_cmd_render *c,
           struct attachments *att, struct agx_pool *pool,
           struct agx_batch *batch, struct pipe_framebuffer_state *framebuffer,
           uint64_t encoder_ptr, uint64_t encoder_id, uint64_t cmd_ta_id,
           uint64_t cmd_3d_id, uint64_t scissor_ptr, uint64_t depth_bias_ptr,
           uint64_t visibility_result_ptr, struct asahi_bg_eot pipeline_clear,
           struct asahi_bg_eot pipeline_load,
           struct asahi_bg_eot pipeline_store, bool clear_pipeline_textures,
           double clear_depth, unsigned clear_stencil,
           struct agx_tilebuffer_layout *tib)
{
   memset(c, 0, sizeof(*c));

   c->encoder_ptr = encoder_ptr;
   c->encoder_id = encoder_id;
   c->cmd_3d_id = cmd_3d_id;
   c->cmd_ta_id = cmd_ta_id;

   c->fragment_usc_base = dev->shader_base;
   c->vertex_usc_base = dev->shader_base;

   /* bit 0 specifies OpenGL clip behaviour. Since ARB_clip_control is
    * advertised, we don't set it and lower in the vertex shader.
    */
   c->ppp_ctrl = 0x202;

   c->fb_width = framebuffer->width;
   c->fb_height = framebuffer->height;

   c->iogpu_unk_214 = 0xc000;

   c->isp_bgobjvals = 0x300;

   struct agx_resource *zres = NULL, *sres = NULL;

   agx_pack(&c->zls_ctrl, ZLS_CONTROL, zls_control) {

      if (framebuffer->zsbuf) {
         struct pipe_surface *zsbuf = framebuffer->zsbuf;
         struct agx_resource *zsres = agx_resource(zsbuf->texture);

         unsigned level = zsbuf->u.tex.level;
         unsigned first_layer = zsbuf->u.tex.first_layer;

         const struct util_format_description *desc = util_format_description(
            agx_resource(zsbuf->texture)->layout.format);

         assert(desc->format == PIPE_FORMAT_Z32_FLOAT ||
                desc->format == PIPE_FORMAT_Z16_UNORM ||
                desc->format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT ||
                desc->format == PIPE_FORMAT_S8_UINT);

         c->depth_dimensions =
            (framebuffer->width - 1) | ((framebuffer->height - 1) << 15);

         if (util_format_has_depth(desc))
            zres = zsres;
         else
            sres = zsres;

         if (zsres->separate_stencil)
            sres = zsres->separate_stencil;

         if (zres) {
            bool clear = (batch->clear & PIPE_CLEAR_DEPTH);
            bool load = (batch->load & PIPE_CLEAR_DEPTH);

            zls_control.z_store_enable = (batch->resolve & PIPE_CLEAR_DEPTH);
            zls_control.z_load_enable = !clear && load;

            c->depth_buffer_load = agx_map_texture_gpu(zres, first_layer) +
                                   ail_get_level_offset_B(&zres->layout, level);

            c->depth_buffer_store = c->depth_buffer_load;
            c->depth_buffer_partial = c->depth_buffer_load;

            /* Main stride in pages */
            assert((zres->layout.depth_px == 1 ||
                    is_aligned(zres->layout.layer_stride_B, AIL_PAGESIZE)) &&
                   "Page aligned Z layers");

            unsigned stride_pages = zres->layout.layer_stride_B / AIL_PAGESIZE;
            c->depth_buffer_load_stride = ((stride_pages - 1) << 14) | 1;
            c->depth_buffer_store_stride = c->depth_buffer_load_stride;
            c->depth_buffer_partial_stride = c->depth_buffer_load_stride;

            assert(zres->layout.tiling != AIL_TILING_LINEAR && "must tile");

            if (ail_is_compressed(&zres->layout)) {
               c->depth_meta_buffer_load =
                  agx_map_texture_gpu(zres, 0) +
                  zres->layout.metadata_offset_B +
                  (first_layer * zres->layout.compression_layer_stride_B) +
                  zres->layout.level_offsets_compressed_B[level];

               /* Meta stride in cache lines */
               assert(is_aligned(zres->layout.compression_layer_stride_B,
                                 AIL_CACHELINE) &&
                      "Cacheline aligned Z meta layers");
               unsigned stride_lines =
                  zres->layout.compression_layer_stride_B / AIL_CACHELINE;
               c->depth_meta_buffer_load_stride = (stride_lines - 1) << 14;

               c->depth_meta_buffer_store = c->depth_meta_buffer_load;
               c->depth_meta_buffer_store_stride =
                  c->depth_meta_buffer_load_stride;
               c->depth_meta_buffer_partial = c->depth_meta_buffer_load;
               c->depth_meta_buffer_partial_stride =
                  c->depth_meta_buffer_load_stride;

               zls_control.z_compress_1 = true;
               zls_control.z_compress_2 = true;
            }

            if (zres->base.format == PIPE_FORMAT_Z16_UNORM) {
               const float scale = 0xffff;
               c->isp_bgobjdepth =
                  (uint16_t)(SATURATE(clear_depth) * scale + 0.5f);
               zls_control.z_format = AGX_ZLS_FORMAT_16;
               c->iogpu_unk_214 |= 0x40000;
            } else {
               c->isp_bgobjdepth = fui(clear_depth);
               zls_control.z_format = AGX_ZLS_FORMAT_32F;
            }
         }

         if (sres) {
            bool clear = (batch->clear & PIPE_CLEAR_STENCIL);
            bool load = (batch->load & PIPE_CLEAR_STENCIL);

            zls_control.s_store_enable = (batch->resolve & PIPE_CLEAR_STENCIL);
            zls_control.s_load_enable = !clear && load;

            c->stencil_buffer_load =
               agx_map_texture_gpu(sres, first_layer) +
               ail_get_level_offset_B(&sres->layout, level);

            c->stencil_buffer_store = c->stencil_buffer_load;
            c->stencil_buffer_partial = c->stencil_buffer_load;

            /* Main stride in pages */
            assert((sres->layout.depth_px == 1 ||
                    is_aligned(sres->layout.layer_stride_B, AIL_PAGESIZE)) &&
                   "Page aligned S layers");
            unsigned stride_pages = sres->layout.layer_stride_B / AIL_PAGESIZE;
            c->stencil_buffer_load_stride = ((stride_pages - 1) << 14) | 1;
            c->stencil_buffer_store_stride = c->stencil_buffer_load_stride;
            c->stencil_buffer_partial_stride = c->stencil_buffer_load_stride;

            if (ail_is_compressed(&sres->layout)) {
               c->stencil_meta_buffer_load =
                  agx_map_texture_gpu(sres, 0) +
                  sres->layout.metadata_offset_B +
                  (first_layer * sres->layout.compression_layer_stride_B) +
                  sres->layout.level_offsets_compressed_B[level];

               /* Meta stride in cache lines */
               assert(is_aligned(sres->layout.compression_layer_stride_B,
                                 AIL_CACHELINE) &&
                      "Cacheline aligned S meta layers");
               unsigned stride_lines =
                  sres->layout.compression_layer_stride_B / AIL_CACHELINE;
               c->stencil_meta_buffer_load_stride = (stride_lines - 1) << 14;

               c->stencil_meta_buffer_store = c->stencil_meta_buffer_load;
               c->stencil_meta_buffer_store_stride =
                  c->stencil_meta_buffer_load_stride;
               c->stencil_meta_buffer_partial = c->stencil_meta_buffer_load;
               c->stencil_meta_buffer_partial_stride =
                  c->stencil_meta_buffer_load_stride;

               zls_control.s_compress_1 = true;
               zls_control.s_compress_2 = true;
            }

            c->isp_bgobjvals |= clear_stencil;
         }
      }
   }

   if (clear_pipeline_textures)
      c->flags |= ASAHI_RENDER_SET_WHEN_RELOADING_Z_OR_S;
   else
      c->flags |= ASAHI_RENDER_NO_CLEAR_PIPELINE_TEXTURES;

   if (zres && !(batch->clear & PIPE_CLEAR_DEPTH))
      c->flags |= ASAHI_RENDER_SET_WHEN_RELOADING_Z_OR_S;

   if (sres && !(batch->clear & PIPE_CLEAR_STENCIL))
      c->flags |= ASAHI_RENDER_SET_WHEN_RELOADING_Z_OR_S;

   if (dev->debug & AGX_DBG_NOCLUSTER)
      c->flags |= ASAHI_RENDER_NO_VERTEX_CLUSTERING;

   /* XXX is this for just MSAA+Z+S or MSAA+(Z|S)? */
   if (tib->nr_samples > 1 && framebuffer->zsbuf)
      c->flags |= ASAHI_RENDER_MSAA_ZS;

   memcpy(&c->load_pipeline_bind, &pipeline_clear.counts,
          sizeof(struct agx_counts_packed));

   memcpy(&c->store_pipeline_bind, &pipeline_store.counts,
          sizeof(struct agx_counts_packed));

   memcpy(&c->partial_reload_pipeline_bind, &pipeline_load.counts,
          sizeof(struct agx_counts_packed));

   memcpy(&c->partial_store_pipeline_bind, &pipeline_store.counts,
          sizeof(struct agx_counts_packed));

   /* XXX is this correct? */
   c->load_pipeline = pipeline_clear.usc | (framebuffer->nr_cbufs >= 4 ? 8 : 4);
   c->store_pipeline = pipeline_store.usc | 4;
   c->partial_reload_pipeline = pipeline_load.usc | 4;
   c->partial_store_pipeline = pipeline_store.usc | 4;

   c->utile_width = tib->tile_size.width;
   c->utile_height = tib->tile_size.height;

   c->samples = tib->nr_samples;
   c->layers = MAX2(util_framebuffer_get_num_layers(framebuffer), 1);

   c->ppp_multisamplectl = batch->uniforms.ppp_multisamplectl;
   c->sample_size = tib->sample_size_B;

   /* XXX OR 0x80 with eMRT? */
   c->tib_blocks = ALIGN_POT(agx_tilebuffer_total_size(tib), 2048) / 2048;

   float tan_60 = 1.732051f;
   c->merge_upper_x = fui(tan_60 / framebuffer->width);
   c->merge_upper_y = fui(tan_60 / framebuffer->height);

   c->scissor_array = scissor_ptr;
   c->depth_bias_array = depth_bias_ptr;
   c->visibility_result_buffer = visibility_result_ptr;

   c->vertex_sampler_array =
      batch->sampler_heap.bo ? batch->sampler_heap.bo->va->addr : 0;
   c->vertex_sampler_count = batch->sampler_heap.count;
   c->vertex_sampler_max = batch->sampler_heap.count + 1;

   /* In the future we could split the heaps if useful */
   c->fragment_sampler_array = c->vertex_sampler_array;
   c->fragment_sampler_count = c->vertex_sampler_count;
   c->fragment_sampler_max = c->vertex_sampler_max;

   /* If a tile is empty, we do not want to process it, as the redundant
    * roundtrip of memory-->tilebuffer-->memory wastes a tremendous amount of
    * memory bandwidth. Any draw marks a tile as non-empty, so we only need to
    * process empty tiles if the background+EOT programs have a side effect.
    * This is the case exactly when there is an attachment we are clearing (some
    * attachment A in clear and in resolve <==> non-empty intersection).
    *
    * This case matters a LOT for performance in workloads that split batches.
    */
   if (batch->clear & batch->resolve)
      c->flags |= ASAHI_RENDER_PROCESS_EMPTY_TILES;

   for (unsigned i = 0; i < framebuffer->nr_cbufs; ++i) {
      if (!framebuffer->cbufs[i])
         continue;

      asahi_add_attachment(att, agx_resource(framebuffer->cbufs[i]->texture),
                           framebuffer->cbufs[i]);
   }

   if (framebuffer->zsbuf) {
      struct agx_resource *rsrc = agx_resource(framebuffer->zsbuf->texture);

      asahi_add_attachment(att, rsrc, framebuffer->zsbuf);

      if (rsrc->separate_stencil) {
         asahi_add_attachment(att, rsrc->separate_stencil, framebuffer->zsbuf);
      }
   }

   c->fragment_attachments = (uint64_t)(uintptr_t)&att->list[0];
   c->fragment_attachment_count = att->count;

   if (batch->vs_scratch) {
      c->flags |= ASAHI_RENDER_VERTEX_SPILLS;
      c->vertex_helper_arg = batch->ctx->scratch_vs.buf->va->addr;
      c->vertex_helper_cfg = batch->vs_preamble_scratch << 16;
      c->vertex_helper_program = dev->helper->va->addr | 1;
   }
   if (batch->fs_scratch) {
      c->fragment_helper_arg = batch->ctx->scratch_fs.buf->va->addr;
      c->fragment_helper_cfg = batch->fs_preamble_scratch << 16;
      c->fragment_helper_program = dev->helper->va->addr | 1;
   }
}

/*
 * context
 */
static void
agx_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence,
          unsigned flags)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_screen *screen = agx_screen(ctx->base.screen);

   agx_flush_all(ctx, "Gallium flush");

   if (!(flags & (PIPE_FLUSH_DEFERRED | PIPE_FLUSH_ASYNC)) &&
       ctx->flush_last_seqid) {
      /* Ensure other contexts in this screen serialize against the last
       * submission (and all prior submissions).
       */
      simple_mtx_lock(&screen->flush_seqid_lock);

      uint64_t val = p_atomic_read(&screen->flush_wait_seqid);
      if (val < ctx->flush_last_seqid)
         p_atomic_set(&screen->flush_wait_seqid, ctx->flush_last_seqid);

      /* Note: it's possible for the max() logic above to be "wrong" due
       * to a race in agx_batch_submit causing out-of-order timeline point
       * updates, making the larger value not actually a later submission.
       * However, see the comment in agx_batch.c for why this doesn't matter
       * because this corner case is handled conservatively in the kernel.
       */

      simple_mtx_unlock(&screen->flush_seqid_lock);

      /* Optimization: Avoid serializing against our own queue by
       * recording the last seen foreign seqid when flushing, and our own
       * flush seqid. If we then try to sync against our own seqid, we'll
       * instead sync against the last possible foreign one. This is *not*
       * the `val` we got above, because another context might flush with a
       * seqid between `val` and `flush_last_seqid` (which would not update
       * `flush_wait_seqid` per the logic above). This is somewhat
       * conservative: it means that if *any* foreign context flushes, then
       * on next flush of this context we will start waiting for *all*
       * prior submits on *all* contexts (even if unflushed) at that point,
       * including any local submissions prior to the latest one. That's
       * probably fine, it creates a one-time "wait for the second-previous
       * batch" wait on this queue but that still allows for at least
       * the previous batch to pipeline on the GPU and it's one-time
       * until another foreign flush happens. Phew.
       */
      if (val && val != ctx->flush_my_seqid)
         ctx->flush_other_seqid = ctx->flush_last_seqid - 1;

      ctx->flush_my_seqid = ctx->flush_last_seqid;
   }

   /* At this point all pending work has been submitted. Since jobs are
    * started and completed sequentially from a UAPI perspective, and since
    * we submit all jobs with compute+render barriers on the prior job,
    * waiting on the last submitted job is sufficient to guarantee completion
    * of all GPU work thus far, so we can create a fence out of the latest
    * syncobj.
    *
    * See this page for more info on how the GPU/UAPI queueing works:
    * https://github.com/AsahiLinux/docs/wiki/SW:AGX-driver-notes#queues
    */

   if (fence) {
      struct pipe_fence_handle *f = agx_fence_create(ctx);
      pctx->screen->fence_reference(pctx->screen, fence, NULL);
      *fence = f;
   }
}

static void
agx_flush_compute(struct agx_context *ctx, struct agx_batch *batch,
                  struct drm_asahi_cmd_compute *cmdbuf)
{
   struct agx_device *dev = agx_device(ctx->base.screen);

   /* Finalize the encoder */
   agx_pack(batch->cdm.current, CDM_STREAM_TERMINATE, _)
      ;

   agx_batch_add_bo(batch, batch->cdm.bo);

   if (batch->cs_scratch)
      agx_batch_add_bo(batch, ctx->scratch_cs.buf);

   unsigned cmdbuf_id = agx_get_global_id(dev);
   unsigned encoder_id = agx_get_global_id(dev);

   *cmdbuf = (struct drm_asahi_cmd_compute){
      .flags = 0,
      .encoder_ptr = batch->cdm.bo->va->addr,
      .encoder_end = batch->cdm.bo->va->addr +
                     (batch->cdm.current - (uint8_t *)batch->cdm.bo->map),
      .usc_base = dev->shader_base,
      .helper_arg = 0,
      .helper_cfg = 0,
      .helper_program = 0,
      .iogpu_unk_40 = 0,
      .sampler_array =
         batch->sampler_heap.bo ? batch->sampler_heap.bo->va->addr : 0,
      .sampler_count = batch->sampler_heap.count,
      .sampler_max = batch->sampler_heap.count + 1,
      .encoder_id = encoder_id,
      .cmd_id = cmdbuf_id,
      .unk_mask = 0xffffffff,
   };

   if (batch->cs_scratch) {
      // The commented out lines *may* be related to subgroup-level preemption,
      // which we can't support without implementing threadgroup memory in the
      // helper. Disable them for now.

      // cmdbuf->iogpu_unk_40 = 0x1c;
      cmdbuf->helper_arg = ctx->scratch_cs.buf->va->addr;
      cmdbuf->helper_cfg = batch->cs_preamble_scratch << 16;
      // cmdbuf->helper_cfg |= 0x40;
      cmdbuf->helper_program = dev->helper->va->addr | 1;
   }
}

static void
agx_flush_render(struct agx_context *ctx, struct agx_batch *batch,
                 struct drm_asahi_cmd_render *cmdbuf, struct attachments *att)
{
   struct agx_device *dev = agx_device(ctx->base.screen);

   if (batch->vs_scratch)
      agx_batch_add_bo(batch, ctx->scratch_vs.buf);
   if (batch->fs_scratch)
      agx_batch_add_bo(batch, ctx->scratch_fs.buf);

   assert(batch->initialized);

   /* Finalize the encoder */
   uint8_t stop[5 + 64] = {0x00, 0x00, 0x00, 0xc0, 0x00};
   memcpy(batch->vdm.current, stop, sizeof(stop));

   struct asahi_bg_eot pipeline_background =
      agx_build_bg_eot(batch, false, false);

   struct asahi_bg_eot pipeline_background_partial =
      agx_build_bg_eot(batch, false, true);

   struct asahi_bg_eot pipeline_store = agx_build_bg_eot(batch, true, false);

   bool clear_pipeline_textures =
      agx_tilebuffer_spills(&batch->tilebuffer_layout);

   for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
      struct pipe_surface *surf = batch->key.cbufs[i];

      clear_pipeline_textures |=
         surf && surf->texture && !(batch->clear & (PIPE_CLEAR_COLOR0 << i));
   }

   /* Scissor and depth bias arrays are staged to dynamic arrays on the CPU. At
    * submit time, they're done growing and are uploaded to GPU memory attached
    * to the batch.
    */
   uint64_t scissor = agx_pool_upload_aligned(&batch->pool, batch->scissor.data,
                                              batch->scissor.size, 64);
   uint64_t zbias = agx_pool_upload_aligned(
      &batch->pool, batch->depth_bias.data, batch->depth_bias.size, 64);

   /* BO list for a given batch consists of:
    *  - BOs for the batch's pools
    *  - BOs for the encoder
    *  - BO for internal shaders
    *  - BOs added to the batch explicitly
    */
   agx_batch_add_bo(batch, batch->vdm.bo);

   unsigned cmd_ta_id = agx_get_global_id(dev);
   unsigned cmd_3d_id = agx_get_global_id(dev);
   unsigned encoder_id = agx_get_global_id(dev);

   agx_cmdbuf(dev, cmdbuf, att, &batch->pool, batch, &batch->key,
              batch->vdm.bo->va->addr, encoder_id, cmd_ta_id, cmd_3d_id,
              scissor, zbias, agx_get_occlusion_heap(batch),
              pipeline_background, pipeline_background_partial, pipeline_store,
              clear_pipeline_textures, batch->clear_depth, batch->clear_stencil,
              &batch->tilebuffer_layout);
}

void
agx_flush_batch(struct agx_context *ctx, struct agx_batch *batch)
{
   assert(agx_batch_is_active(batch));
   assert(!agx_batch_is_submitted(batch));

   struct attachments att = {.count = 0};
   struct drm_asahi_cmd_render render;
   struct drm_asahi_cmd_compute compute;
   bool has_vdm = false, has_cdm = false;

   if (batch->cdm.bo) {
      agx_flush_compute(ctx, batch, &compute);
      has_cdm = true;
   }

   if (batch->vdm.bo && (batch->clear || batch->initialized)) {
      agx_flush_render(ctx, batch, &render, &att);
      has_vdm = true;
   }

   if (!has_cdm && !has_vdm) {
      agx_batch_reset(ctx, batch);
      return;
   }

   agx_batch_submit(ctx, batch, has_cdm ? &compute : NULL,
                    has_vdm ? &render : NULL);
}

static void
agx_destroy_context(struct pipe_context *pctx)
{
   struct agx_device *dev = agx_device(pctx->screen);
   struct agx_context *ctx = agx_context(pctx);
   struct agx_screen *screen = agx_screen(pctx->screen);

   /* Batch state needs to be freed on completion, and we don't want to yank
    * buffers out from in-progress GPU jobs to avoid faults, so just wait until
    * everything in progress is actually done on context destroy. This will
    * ensure everything is cleaned up properly.
    */
   agx_sync_all(ctx, "destroy context");

   if (pctx->stream_uploader)
      u_upload_destroy(pctx->stream_uploader);

   if (ctx->blitter)
      util_blitter_destroy(ctx->blitter);

   util_unreference_framebuffer_state(&ctx->framebuffer);

   agx_bg_eot_cleanup(&ctx->bg_eot);
   agx_destroy_meta_shaders(ctx);

   agx_bo_unreference(dev, ctx->result_buf);

   /* Lock around the syncobj destruction, to avoid racing
    * command submission in another context.
    **/
   u_rwlock_wrlock(&screen->destroy_lock);

   drmSyncobjDestroy(dev->fd, ctx->in_sync_obj);
   drmSyncobjDestroy(dev->fd, ctx->dummy_syncobj);
   if (ctx->in_sync_fd != -1)
      close(ctx->in_sync_fd);

   for (unsigned i = 0; i < AGX_MAX_BATCHES; ++i) {
      if (ctx->batches.slots[i].syncobj)
         drmSyncobjDestroy(dev->fd, ctx->batches.slots[i].syncobj);
   }

   u_rwlock_wrunlock(&screen->destroy_lock);

   pipe_resource_reference(&ctx->heap, NULL);

   agx_scratch_fini(&ctx->scratch_vs);
   agx_scratch_fini(&ctx->scratch_fs);
   agx_scratch_fini(&ctx->scratch_cs);

   agx_destroy_command_queue(dev, ctx->queue_id);

   ralloc_free(ctx);
}

static void
agx_invalidate_resource(struct pipe_context *pctx,
                        struct pipe_resource *resource)
{
   struct agx_context *ctx = agx_context(pctx);
   struct agx_batch *batch = agx_get_batch(ctx);

   /* Handle the glInvalidateFramebuffer case */
   if (batch->key.zsbuf && batch->key.zsbuf->texture == resource)
      batch->resolve &= ~PIPE_CLEAR_DEPTHSTENCIL;

   for (unsigned i = 0; i < batch->key.nr_cbufs; ++i) {
      struct pipe_surface *surf = batch->key.cbufs[i];

      if (surf && surf->texture == resource)
         batch->resolve &= ~(PIPE_CLEAR_COLOR0 << i);
   }
}

static enum pipe_reset_status
asahi_get_device_reset_status(struct pipe_context *pipe)
{
   struct agx_context *ctx = agx_context(pipe);

   return ctx->any_faults ? PIPE_GUILTY_CONTEXT_RESET : PIPE_NO_RESET;
}

static struct pipe_context *
agx_create_context(struct pipe_screen *screen, void *priv, unsigned flags)
{
   struct agx_context *ctx = rzalloc(NULL, struct agx_context);
   struct pipe_context *pctx = &ctx->base;
   int ret;

   if (!ctx)
      return NULL;

   pctx->screen = screen;
   pctx->priv = priv;

   util_dynarray_init(&ctx->writer, ctx);
   util_dynarray_init(&ctx->global_buffers, ctx);

   pctx->stream_uploader = u_upload_create_default(pctx);
   if (!pctx->stream_uploader) {
      FREE(pctx);
      return NULL;
   }
   pctx->const_uploader = pctx->stream_uploader;

   uint32_t priority = 2;
   if (flags & PIPE_CONTEXT_PRIORITY_LOW)
      priority = 3;
   else if (flags & PIPE_CONTEXT_PRIORITY_MEDIUM)
      priority = 2;
   else if (flags & PIPE_CONTEXT_PRIORITY_HIGH)
      priority = 1;

   ctx->queue_id = agx_create_command_queue(agx_device(screen),
                                            DRM_ASAHI_QUEUE_CAP_RENDER |
                                               DRM_ASAHI_QUEUE_CAP_BLIT |
                                               DRM_ASAHI_QUEUE_CAP_COMPUTE,
                                            priority);

   pctx->destroy = agx_destroy_context;
   pctx->flush = agx_flush;
   pctx->clear = agx_clear;
   pctx->resource_copy_region = agx_resource_copy_region;
   pctx->blit = agx_blit;
   pctx->flush_resource = agx_flush_resource;

   pctx->buffer_map = u_transfer_helper_transfer_map;
   pctx->buffer_unmap = u_transfer_helper_transfer_unmap;
   pctx->texture_map = u_transfer_helper_transfer_map;
   pctx->texture_unmap = u_transfer_helper_transfer_unmap;
   pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;

   pctx->buffer_subdata = u_default_buffer_subdata;
   pctx->clear_buffer = u_default_clear_buffer;
   pctx->texture_subdata = u_default_texture_subdata;
   pctx->set_debug_callback = u_default_set_debug_callback;
   pctx->get_sample_position = u_default_get_sample_position;
   pctx->invalidate_resource = agx_invalidate_resource;
   pctx->memory_barrier = agx_memory_barrier;

   pctx->create_fence_fd = agx_create_fence_fd;
   pctx->fence_server_sync = agx_fence_server_sync;

   pctx->get_device_reset_status = asahi_get_device_reset_status;

   agx_init_state_functions(pctx);
   agx_init_query_functions(pctx);
   agx_init_streamout_functions(pctx);

   agx_bg_eot_init(&ctx->bg_eot, agx_device(screen));
   agx_init_meta_shaders(ctx);

   ctx->blitter = util_blitter_create(pctx);
   ctx->compute_blitter.blit_cs = asahi_blit_key_table_create(ctx);

   ctx->result_buf =
      agx_bo_create(agx_device(screen),
                    (2 * sizeof(union agx_batch_result)) * AGX_MAX_BATCHES, 0,
                    AGX_BO_WRITEBACK, "Batch result buffer");
   assert(ctx->result_buf);

   /* Sync object/FD used for NATIVE_FENCE_FD. */
   ctx->in_sync_fd = -1;
   ret = drmSyncobjCreate(agx_device(screen)->fd, 0, &ctx->in_sync_obj);
   assert(!ret);

   /* Dummy sync object used before any work has been submitted. */
   ret = drmSyncobjCreate(agx_device(screen)->fd, DRM_SYNCOBJ_CREATE_SIGNALED,
                          &ctx->dummy_syncobj);
   assert(!ret);
   ctx->syncobj = ctx->dummy_syncobj;

   /* By default all samples are enabled */
   ctx->sample_mask = ~0;

   ctx->support_lod_bias = !(flags & PIPE_CONTEXT_NO_LOD_BIAS);
   ctx->robust = (flags & PIPE_CONTEXT_ROBUST_BUFFER_ACCESS);

   agx_scratch_init(agx_device(screen), &ctx->scratch_vs);
   agx_scratch_init(agx_device(screen), &ctx->scratch_fs);
   agx_scratch_init(agx_device(screen), &ctx->scratch_cs);

   return pctx;
}

static const char *
agx_get_vendor(struct pipe_screen *pscreen)
{
   return "Mesa";
}

static const char *
agx_get_device_vendor(struct pipe_screen *pscreen)
{
   return "Apple";
}

static const char *
agx_get_name(struct pipe_screen *pscreen)
{
   struct agx_device *dev = agx_device(pscreen);

   return dev->name;
}

static void
agx_query_memory_info(struct pipe_screen *pscreen,
                      struct pipe_memory_info *info)
{
   uint64_t mem_B = 0;
   os_get_total_physical_memory(&mem_B);

   uint64_t mem_kB = mem_B / 1024;

   *info = (struct pipe_memory_info){
      .total_device_memory = mem_kB,
      .avail_device_memory = mem_kB,
   };
}

static int
agx_get_param(struct pipe_screen *pscreen, enum pipe_cap param)
{
   struct agx_device *dev = agx_device(pscreen);

   switch (param) {
   case PIPE_CAP_CLIP_HALFZ:
   case PIPE_CAP_NPOT_TEXTURES:
   case PIPE_CAP_SHADER_STENCIL_EXPORT:
   case PIPE_CAP_MIXED_COLOR_DEPTH_BITS:
   case PIPE_CAP_FRAGMENT_SHADER_TEXTURE_LOD:
   case PIPE_CAP_VERTEX_COLOR_UNCLAMPED:
   case PIPE_CAP_DEPTH_CLIP_DISABLE:
   case PIPE_CAP_MIXED_FRAMEBUFFER_SIZES:
   case PIPE_CAP_FRAGMENT_SHADER_DERIVATIVES:
   case PIPE_CAP_FRAMEBUFFER_NO_ATTACHMENT:
   case PIPE_CAP_SHADER_PACK_HALF_FLOAT:
   case PIPE_CAP_FS_FINE_DERIVATIVE:
   case PIPE_CAP_GLSL_TESS_LEVELS_AS_INPUTS:
   case PIPE_CAP_DOUBLES:
      return 1;

   case PIPE_CAP_MAX_RENDER_TARGETS:
   case PIPE_CAP_FBFETCH:
   case PIPE_CAP_FBFETCH_COHERENT:
      return 8;
   case PIPE_CAP_MAX_DUAL_SOURCE_RENDER_TARGETS:
      return 1;

   case PIPE_CAP_OCCLUSION_QUERY:
   case PIPE_CAP_QUERY_TIMESTAMP:
   case PIPE_CAP_QUERY_TIME_ELAPSED:
   case PIPE_CAP_QUERY_SO_OVERFLOW:
   case PIPE_CAP_QUERY_MEMORY_INFO:
   case PIPE_CAP_PRIMITIVE_RESTART:
   case PIPE_CAP_PRIMITIVE_RESTART_FIXED_INDEX:
   case PIPE_CAP_ANISOTROPIC_FILTER:
   case PIPE_CAP_NATIVE_FENCE_FD:
   case PIPE_CAP_TEXTURE_BARRIER:
      return true;

   case PIPE_CAP_TIMER_RESOLUTION:
      /* Timer resolution is the length of a single tick in nanos */
      return agx_gpu_time_to_ns(dev, 1);

   case PIPE_CAP_SAMPLER_VIEW_TARGET:
   case PIPE_CAP_TEXTURE_SWIZZLE:
   case PIPE_CAP_BLEND_EQUATION_SEPARATE:
   case PIPE_CAP_INDEP_BLEND_ENABLE:
   case PIPE_CAP_INDEP_BLEND_FUNC:
   case PIPE_CAP_ACCELERATED:
   case PIPE_CAP_UMA:
   case PIPE_CAP_TEXTURE_FLOAT_LINEAR:
   case PIPE_CAP_TEXTURE_HALF_FLOAT_LINEAR:
   case PIPE_CAP_TEXTURE_MIRROR_CLAMP_TO_EDGE:
   case PIPE_CAP_SHADER_ARRAY_COMPONENTS:
   case PIPE_CAP_PACKED_UNIFORMS:
   case PIPE_CAP_QUADS_FOLLOW_PROVOKING_VERTEX_CONVENTION:
   case PIPE_CAP_VS_INSTANCEID:
   case PIPE_CAP_VERTEX_ELEMENT_INSTANCE_DIVISOR:
   case PIPE_CAP_CONDITIONAL_RENDER:
   case PIPE_CAP_CONDITIONAL_RENDER_INVERTED:
   case PIPE_CAP_SEAMLESS_CUBE_MAP:
   case PIPE_CAP_LOAD_CONSTBUF:
   case PIPE_CAP_SEAMLESS_CUBE_MAP_PER_TEXTURE:
   case PIPE_CAP_TEXTURE_BUFFER_OBJECTS:
   case PIPE_CAP_NULL_TEXTURES:
   case PIPE_CAP_TEXTURE_MULTISAMPLE:
   case PIPE_CAP_IMAGE_LOAD_FORMATTED:
   case PIPE_CAP_IMAGE_STORE_FORMATTED:
   case PIPE_CAP_COMPUTE:
   case PIPE_CAP_INT64:
   case PIPE_CAP_SAMPLE_SHADING:
   case PIPE_CAP_START_INSTANCE:
   case PIPE_CAP_DRAW_PARAMETERS:
   case PIPE_CAP_MULTI_DRAW_INDIRECT:
   case PIPE_CAP_MULTI_DRAW_INDIRECT_PARAMS:
   case PIPE_CAP_CULL_DISTANCE:
   case PIPE_CAP_GL_SPIRV:
   case PIPE_CAP_POLYGON_OFFSET_CLAMP:
      return 1;
   case PIPE_CAP_SURFACE_SAMPLE_COUNT:
      /* TODO: MSRTT */
      return 0;

   case PIPE_CAP_CUBE_MAP_ARRAY:
      return 1;

   case PIPE_CAP_COPY_BETWEEN_COMPRESSED_AND_PLAIN_FORMATS:
      return 1;

   case PIPE_CAP_MAX_STREAM_OUTPUT_BUFFERS:
      return PIPE_MAX_SO_BUFFERS;

   case PIPE_CAP_MAX_STREAM_OUTPUT_SEPARATE_COMPONENTS:
   case PIPE_CAP_MAX_STREAM_OUTPUT_INTERLEAVED_COMPONENTS:
      return PIPE_MAX_SO_OUTPUTS;

   case PIPE_CAP_STREAM_OUTPUT_PAUSE_RESUME:
   case PIPE_CAP_STREAM_OUTPUT_INTERLEAVE_BUFFERS:
      return 1;

   case PIPE_CAP_MAX_TEXTURE_ARRAY_LAYERS:
      return 2048;

   case PIPE_CAP_GLSL_FEATURE_LEVEL:
   case PIPE_CAP_GLSL_FEATURE_LEVEL_COMPATIBILITY:
      return 460;
   case PIPE_CAP_ESSL_FEATURE_LEVEL:
      return 320;

   /* Settings from iris, may need tuning */
   case PIPE_CAP_MAX_VERTEX_STREAMS:
      return 4;
   case PIPE_CAP_MAX_GEOMETRY_OUTPUT_VERTICES:
      return 256;
   case PIPE_CAP_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS:
      return 1024;
   case PIPE_CAP_MAX_GS_INVOCATIONS:
      return 32;
   case PIPE_CAP_CONSTANT_BUFFER_OFFSET_ALIGNMENT:
      return 16;

   case PIPE_CAP_MAX_TEXEL_BUFFER_ELEMENTS_UINT:
      return AGX_TEXTURE_BUFFER_MAX_SIZE;

   case PIPE_CAP_TEXTURE_BUFFER_OFFSET_ALIGNMENT:
      return 64;

   case PIPE_CAP_VERTEX_INPUT_ALIGNMENT:
      return PIPE_VERTEX_INPUT_ALIGNMENT_ELEMENT;

   case PIPE_CAP_QUERY_PIPELINE_STATISTICS_SINGLE:
      return true;

   case PIPE_CAP_MAX_TEXTURE_2D_SIZE:
      return 16384;
   case PIPE_CAP_MAX_TEXTURE_CUBE_LEVELS:
      /* Max 16384x16384 */
      return 15;
   case PIPE_CAP_MAX_TEXTURE_3D_LEVELS:
      /* Max 2048x2048x2048 */
      return 12;

   case PIPE_CAP_FS_COORD_ORIGIN_UPPER_LEFT:
   case PIPE_CAP_FS_COORD_PIXEL_CENTER_INTEGER:
   case PIPE_CAP_TGSI_TEXCOORD:
   case PIPE_CAP_FS_FACE_IS_INTEGER_SYSVAL:
   case PIPE_CAP_FS_POSITION_IS_SYSVAL:
      return true;
   case PIPE_CAP_FS_COORD_ORIGIN_LOWER_LEFT:
   case PIPE_CAP_FS_COORD_PIXEL_CENTER_HALF_INTEGER:
   case PIPE_CAP_FS_POINT_IS_SYSVAL:
      return false;

   case PIPE_CAP_MAX_VERTEX_ELEMENT_SRC_OFFSET:
      return 0xffff;

   case PIPE_CAP_TEXTURE_TRANSFER_MODES:
      return PIPE_TEXTURE_TRANSFER_BLIT;

   case PIPE_CAP_ENDIANNESS:
      return PIPE_ENDIAN_LITTLE;

   case PIPE_CAP_SHADER_GROUP_VOTE:
   case PIPE_CAP_SHADER_BALLOT:
      return true;

   case PIPE_CAP_MAX_TEXTURE_GATHER_COMPONENTS:
      return 4;
   case PIPE_CAP_MIN_TEXTURE_GATHER_OFFSET:
      return -8;
   case PIPE_CAP_MAX_TEXTURE_GATHER_OFFSET:
      return 7;
   case PIPE_CAP_DRAW_INDIRECT:
   case PIPE_CAP_TEXTURE_QUERY_SAMPLES:
   case PIPE_CAP_TEXTURE_QUERY_LOD:
   case PIPE_CAP_TEXTURE_SHADOW_LOD:
      return true;

   case PIPE_CAP_MAX_VIEWPORTS:
      return AGX_MAX_VIEWPORTS;

   case PIPE_CAP_VIDEO_MEMORY: {
      uint64_t system_memory;

      if (!os_get_total_physical_memory(&system_memory))
         return 0;

      return (int)(system_memory >> 20);
   }

   case PIPE_CAP_DEVICE_RESET_STATUS_QUERY:
   case PIPE_CAP_ROBUST_BUFFER_ACCESS_BEHAVIOR:
      return true;

   case PIPE_CAP_SHADER_BUFFER_OFFSET_ALIGNMENT:
      return 4;

   case PIPE_CAP_MAX_SHADER_PATCH_VARYINGS:
      return 32;
   case PIPE_CAP_MAX_VARYINGS:
      /* TODO: Probably should bump to 32? */
      return 16;

   case PIPE_CAP_FLATSHADE:
   case PIPE_CAP_TWO_SIDED_COLOR:
   case PIPE_CAP_ALPHA_TEST:
   case PIPE_CAP_CLIP_PLANES:
   case PIPE_CAP_NIR_IMAGES_AS_DEREF:
      return 0;

   case PIPE_CAP_QUERY_BUFFER_OBJECT:
      return true;

   case PIPE_CAP_TEXTURE_BORDER_COLOR_QUIRK:
      return PIPE_QUIRK_TEXTURE_BORDER_COLOR_SWIZZLE_FREEDRENO;

   case PIPE_CAP_SUPPORTED_PRIM_MODES:
   case PIPE_CAP_SUPPORTED_PRIM_MODES_WITH_RESTART:
      return BITFIELD_BIT(MESA_PRIM_POINTS) | BITFIELD_BIT(MESA_PRIM_LINES) |
             BITFIELD_BIT(MESA_PRIM_LINE_STRIP) |
             BITFIELD_BIT(MESA_PRIM_LINE_LOOP) |
             BITFIELD_BIT(MESA_PRIM_TRIANGLES) |
             BITFIELD_BIT(MESA_PRIM_TRIANGLE_STRIP) |
             BITFIELD_BIT(MESA_PRIM_TRIANGLE_FAN) |
             BITFIELD_BIT(MESA_PRIM_LINES_ADJACENCY) |
             BITFIELD_BIT(MESA_PRIM_LINE_STRIP_ADJACENCY) |
             BITFIELD_BIT(MESA_PRIM_TRIANGLES_ADJACENCY) |
             BITFIELD_BIT(MESA_PRIM_TRIANGLE_STRIP_ADJACENCY) |
             BITFIELD_BIT(MESA_PRIM_PATCHES);

   case PIPE_CAP_MAP_UNSYNCHRONIZED_THREAD_SAFE:
      return 1;

   case PIPE_CAP_VS_LAYER_VIEWPORT:
   case PIPE_CAP_TES_LAYER_VIEWPORT:
      return true;

   case PIPE_CAP_CONTEXT_PRIORITY_MASK:
      return PIPE_CONTEXT_PRIORITY_LOW | PIPE_CONTEXT_PRIORITY_MEDIUM |
             PIPE_CONTEXT_PRIORITY_HIGH;

   default:
      return u_pipe_screen_get_param_defaults(pscreen, param);
   }
}

static float
agx_get_paramf(struct pipe_screen *pscreen, enum pipe_capf param)
{
   switch (param) {
   case PIPE_CAPF_MIN_LINE_WIDTH:
   case PIPE_CAPF_MIN_LINE_WIDTH_AA:
   case PIPE_CAPF_MIN_POINT_SIZE:
   case PIPE_CAPF_MIN_POINT_SIZE_AA:
      return 1;

   case PIPE_CAPF_POINT_SIZE_GRANULARITY:
   case PIPE_CAPF_LINE_WIDTH_GRANULARITY:
      return 0.1;

   case PIPE_CAPF_MAX_LINE_WIDTH:
   case PIPE_CAPF_MAX_LINE_WIDTH_AA:
      return 16.0; /* Off-by-one fixed point 4:4 encoding */

   case PIPE_CAPF_MAX_POINT_SIZE:
   case PIPE_CAPF_MAX_POINT_SIZE_AA:
      return 511.95f;

   case PIPE_CAPF_MAX_TEXTURE_ANISOTROPY:
      return 16.0;

   case PIPE_CAPF_MAX_TEXTURE_LOD_BIAS:
      return 16.0; /* arbitrary */

   case PIPE_CAPF_MIN_CONSERVATIVE_RASTER_DILATE:
   case PIPE_CAPF_MAX_CONSERVATIVE_RASTER_DILATE:
   case PIPE_CAPF_CONSERVATIVE_RASTER_DILATE_GRANULARITY:
      return 0.0f;

   default:
      debug_printf("Unexpected PIPE_CAPF %d query\n", param);
      return 0.0;
   }
}

static int
agx_get_shader_param(struct pipe_screen *pscreen, enum pipe_shader_type shader,
                     enum pipe_shader_cap param)
{
   bool is_no16 = agx_device(pscreen)->debug & AGX_DBG_NO16;

   switch (shader) {
   case PIPE_SHADER_VERTEX:
   case PIPE_SHADER_FRAGMENT:
   case PIPE_SHADER_COMPUTE:
   case PIPE_SHADER_GEOMETRY:
   case PIPE_SHADER_TESS_CTRL:
   case PIPE_SHADER_TESS_EVAL:
      break;
   default:
      return false;
   }

   /* this is probably not totally correct.. but it's a start: */
   switch (param) {
   case PIPE_SHADER_CAP_MAX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_ALU_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INSTRUCTIONS:
   case PIPE_SHADER_CAP_MAX_TEX_INDIRECTIONS:
      return 16384;

   case PIPE_SHADER_CAP_MAX_CONTROL_FLOW_DEPTH:
      return 1024;

   case PIPE_SHADER_CAP_MAX_INPUTS:
      return shader == PIPE_SHADER_VERTEX ? 16 : 32;

   case PIPE_SHADER_CAP_MAX_OUTPUTS:
      /* For vertex, the spec min/max is 16. We need more to handle dmat3
       * correctly, though. The full 32 is undesirable since it would require
       * shenanigans to handle.
       */
      return shader == PIPE_SHADER_FRAGMENT ? 8
             : shader == PIPE_SHADER_VERTEX ? 24
                                            : 32;

   case PIPE_SHADER_CAP_MAX_TEMPS:
      return 256; /* GL_MAX_PROGRAM_TEMPORARIES_ARB */

   case PIPE_SHADER_CAP_MAX_CONST_BUFFER0_SIZE:
      return 16 * 1024 * sizeof(float);

   case PIPE_SHADER_CAP_MAX_CONST_BUFFERS:
      return 16;

   case PIPE_SHADER_CAP_CONT_SUPPORTED:
      return 1;

   case PIPE_SHADER_CAP_SUBROUTINES:
   case PIPE_SHADER_CAP_TGSI_SQRT_SUPPORTED:
      return 0;

   case PIPE_SHADER_CAP_INDIRECT_INPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_OUTPUT_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_TEMP_ADDR:
   case PIPE_SHADER_CAP_INDIRECT_CONST_ADDR:
   case PIPE_SHADER_CAP_INTEGERS:
      return true;

   case PIPE_SHADER_CAP_FP16:
   case PIPE_SHADER_CAP_GLSL_16BIT_CONSTS:
   case PIPE_SHADER_CAP_FP16_DERIVATIVES:
      return !is_no16;
   case PIPE_SHADER_CAP_INT16:
      /* GLSL compiler is broken. Flip this on when Panfrost does. */
      return false;
   case PIPE_SHADER_CAP_FP16_CONST_BUFFERS:
      /* This cap is broken, see 9a38dab2d18 ("zink: disable
       * PIPE_SHADER_CAP_FP16_CONST_BUFFERS") */
      return false;

   case PIPE_SHADER_CAP_INT64_ATOMICS:
   case PIPE_SHADER_CAP_TGSI_ANY_INOUT_DECL_RANGE:
      return 0;

   case PIPE_SHADER_CAP_MAX_TEXTURE_SAMPLERS:
      /* TODO: Enable when fully baked */
      if (strcmp(util_get_process_name(), "blender") == 0)
         return PIPE_MAX_SAMPLERS;
      else if (strcmp(util_get_process_name(), "run") == 0)
         return PIPE_MAX_SAMPLERS;
      else if (strcasestr(util_get_process_name(), "ryujinx") != NULL)
         return PIPE_MAX_SAMPLERS;
      else
         return 16;

   case PIPE_SHADER_CAP_MAX_SAMPLER_VIEWS:
      return PIPE_MAX_SHADER_SAMPLER_VIEWS;

   case PIPE_SHADER_CAP_SUPPORTED_IRS:
      return (1 << PIPE_SHADER_IR_NIR);

   case PIPE_SHADER_CAP_MAX_SHADER_BUFFERS:
      return PIPE_MAX_SHADER_BUFFERS;

   case PIPE_SHADER_CAP_MAX_SHADER_IMAGES:
      return PIPE_MAX_SHADER_IMAGES;

   case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTERS:
   case PIPE_SHADER_CAP_MAX_HW_ATOMIC_COUNTER_BUFFERS:
      return 0;

   default:
      /* Other params are unknown */
      return 0;
   }

   return 0;
}

static int
agx_get_compute_param(struct pipe_screen *pscreen, enum pipe_shader_ir ir_type,
                      enum pipe_compute_cap param, void *ret)
{
#define RET(x)                                                                 \
   do {                                                                        \
      if (ret)                                                                 \
         memcpy(ret, x, sizeof(x));                                            \
      return sizeof(x);                                                        \
   } while (0)

   switch (param) {
   case PIPE_COMPUTE_CAP_ADDRESS_BITS:
      RET((uint32_t[]){64});

   case PIPE_COMPUTE_CAP_IR_TARGET:
      if (ret)
         sprintf(ret, "agx");
      return strlen("agx") * sizeof(char);

   case PIPE_COMPUTE_CAP_GRID_DIMENSION:
      RET((uint64_t[]){3});

   case PIPE_COMPUTE_CAP_MAX_GRID_SIZE:
      RET(((uint64_t[]){65535, 65535, 65535}));

   case PIPE_COMPUTE_CAP_MAX_BLOCK_SIZE:
      RET(((uint64_t[]){1024, 1024, 1024}));

   case PIPE_COMPUTE_CAP_MAX_THREADS_PER_BLOCK:
      RET((uint64_t[]){1024});

   case PIPE_COMPUTE_CAP_MAX_GLOBAL_SIZE:
   case PIPE_COMPUTE_CAP_MAX_MEM_ALLOC_SIZE: {
      uint64_t system_memory;

      if (!os_get_total_physical_memory(&system_memory))
         return 0;

      RET((uint64_t[]){system_memory});
   }

   case PIPE_COMPUTE_CAP_MAX_LOCAL_SIZE:
      RET((uint64_t[]){32768});

   case PIPE_COMPUTE_CAP_MAX_PRIVATE_SIZE:
   case PIPE_COMPUTE_CAP_MAX_INPUT_SIZE:
      RET((uint64_t[]){4096});

   case PIPE_COMPUTE_CAP_MAX_CLOCK_FREQUENCY:
      RET((uint32_t[]){800 /* MHz -- TODO */});

   case PIPE_COMPUTE_CAP_MAX_COMPUTE_UNITS:
      RET((uint32_t[]){4 /* TODO */});

   case PIPE_COMPUTE_CAP_IMAGES_SUPPORTED:
      RET((uint32_t[]){1});

   case PIPE_COMPUTE_CAP_SUBGROUP_SIZES:
      RET((uint32_t[]){32});

   case PIPE_COMPUTE_CAP_MAX_SUBGROUPS:
      RET((uint32_t[]){0 /* TODO */});

   case PIPE_COMPUTE_CAP_MAX_VARIABLE_THREADS_PER_BLOCK:
      RET((uint64_t[]){1024}); // TODO
   }

   return 0;
}

static bool
agx_is_format_supported(struct pipe_screen *pscreen, enum pipe_format format,
                        enum pipe_texture_target target, unsigned sample_count,
                        unsigned storage_sample_count, unsigned usage)
{
   assert(target == PIPE_BUFFER || target == PIPE_TEXTURE_1D ||
          target == PIPE_TEXTURE_1D_ARRAY || target == PIPE_TEXTURE_2D ||
          target == PIPE_TEXTURE_2D_ARRAY || target == PIPE_TEXTURE_RECT ||
          target == PIPE_TEXTURE_3D || target == PIPE_TEXTURE_CUBE ||
          target == PIPE_TEXTURE_CUBE_ARRAY);

   if (sample_count > 1 && sample_count != 4 && sample_count != 2)
      return false;

   if (sample_count > 1 && agx_device(pscreen)->debug & AGX_DBG_NOMSAA)
      return false;

   if (MAX2(sample_count, 1) != MAX2(storage_sample_count, 1))
      return false;

   if ((usage & PIPE_BIND_VERTEX_BUFFER) && !agx_vbo_supports_format(format))
      return false;

   /* For framebuffer_no_attachments, fake support for "none" images */
   if (format == PIPE_FORMAT_NONE)
      return true;

   if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW |
                PIPE_BIND_SHADER_IMAGE)) {
      enum pipe_format tex_format = format;

      /* Mimic the fixup done in create_sampler_view and u_transfer_helper so we
       * advertise GL_OES_texture_stencil8. Alternatively, we could make mesa/st
       * less stupid?
       */
      if (tex_format == PIPE_FORMAT_X24S8_UINT)
         tex_format = PIPE_FORMAT_S8_UINT;

      struct ail_pixel_format_entry ent = ail_pixel_format[tex_format];

      if (!ail_is_valid_pixel_format(tex_format))
         return false;

      /* RGB32, luminance/alpha/intensity emulated for texture buffers only */
      if ((ent.channels == AGX_CHANNELS_R32G32B32_EMULATED ||
           util_format_is_luminance(tex_format) ||
           util_format_is_alpha(tex_format) ||
           util_format_is_luminance_alpha(tex_format) ||
           util_format_is_intensity(tex_format)) &&
          target != PIPE_BUFFER)
         return false;

      /* XXX: sort out rgb9e5 rendering */
      if ((usage & PIPE_BIND_RENDER_TARGET) &&
          (!ent.renderable || (tex_format == PIPE_FORMAT_R9G9B9E5_FLOAT)))
         return false;
   }

   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      switch (format) {
      /* natively supported */
      case PIPE_FORMAT_Z16_UNORM:
      case PIPE_FORMAT_Z32_FLOAT:
      case PIPE_FORMAT_S8_UINT:

      /* lowered by u_transfer_helper to one of the above */
      case PIPE_FORMAT_Z24X8_UNORM:
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
         break;

      default:
         return false;
      }
   }

   return true;
}

static void
agx_query_dmabuf_modifiers(struct pipe_screen *screen, enum pipe_format format,
                           int max, uint64_t *modifiers,
                           unsigned int *external_only, int *out_count)
{
   int i;

   if (max == 0) {
      *out_count = ARRAY_SIZE(agx_best_modifiers);
      return;
   }

   for (i = 0; i < ARRAY_SIZE(agx_best_modifiers) && i < max; i++) {
      if (external_only)
         external_only[i] = 0;

      modifiers[i] = agx_best_modifiers[i];
   }

   /* Return the number of modifiers copied */
   *out_count = i;
}

static bool
agx_is_dmabuf_modifier_supported(struct pipe_screen *screen, uint64_t modifier,
                                 enum pipe_format format, bool *external_only)
{
   if (external_only)
      *external_only = false;

   for (unsigned i = 0; i < ARRAY_SIZE(agx_best_modifiers); ++i) {
      if (agx_best_modifiers[i] == modifier)
         return true;
   }

   return false;
}

static void
agx_destroy_screen(struct pipe_screen *pscreen)
{
   struct agx_screen *screen = agx_screen(pscreen);

   drmSyncobjDestroy(screen->dev.fd, screen->flush_syncobj);

   if (screen->dev.ro)
      screen->dev.ro->destroy(screen->dev.ro);

   u_transfer_helper_destroy(pscreen->transfer_helper);
   agx_close_device(&screen->dev);
   disk_cache_destroy(screen->disk_cache);
   ralloc_free(screen);
}

static const void *
agx_get_compiler_options(struct pipe_screen *pscreen, enum pipe_shader_ir ir,
                         enum pipe_shader_type shader)
{
   return &agx_nir_options;
}

static void
agx_resource_set_stencil(struct pipe_resource *prsrc,
                         struct pipe_resource *stencil)
{
   agx_resource(prsrc)->separate_stencil = agx_resource(stencil);
}

static struct pipe_resource *
agx_resource_get_stencil(struct pipe_resource *prsrc)
{
   return (struct pipe_resource *)agx_resource(prsrc)->separate_stencil;
}

static enum pipe_format
agx_resource_get_internal_format(struct pipe_resource *prsrc)
{
   return agx_resource(prsrc)->layout.format;
}

static struct disk_cache *
agx_get_disk_shader_cache(struct pipe_screen *pscreen)
{
   return agx_screen(pscreen)->disk_cache;
}

static const struct u_transfer_vtbl transfer_vtbl = {
   .resource_create = agx_resource_create,
   .resource_destroy = agx_resource_destroy,
   .transfer_map = agx_transfer_map,
   .transfer_unmap = agx_transfer_unmap,
   .transfer_flush_region = agx_transfer_flush_region,
   .get_internal_format = agx_resource_get_internal_format,
   .set_stencil = agx_resource_set_stencil,
   .get_stencil = agx_resource_get_stencil,
};

static int
agx_screen_get_fd(struct pipe_screen *pscreen)
{
   return agx_device(pscreen)->fd;
}

static uint64_t
agx_get_timestamp(struct pipe_screen *pscreen)
{
   struct agx_device *dev = agx_device(pscreen);
   return agx_gpu_time_to_ns(dev, agx_get_gpu_timestamp(dev));
}

static void
agx_screen_get_device_uuid(struct pipe_screen *pscreen, char *uuid)
{
   agx_get_device_uuid(agx_device(pscreen), uuid);
}

static void
agx_screen_get_driver_uuid(struct pipe_screen *pscreen, char *uuid)
{
   agx_get_driver_uuid(uuid);
}

static const char *
agx_get_cl_cts_version(struct pipe_screen *pscreen)
{
   struct agx_device *dev = agx_device(pscreen);

   /* https://www.khronos.org/conformance/adopters/conformant-products/opencl#submission_433 */
   if (dev->params.gpu_generation < 15)
      return "v2024-08-08-00";

   return NULL;
}

struct pipe_screen *
agx_screen_create(int fd, struct renderonly *ro,
                  const struct pipe_screen_config *config)
{
   struct agx_screen *agx_screen;
   struct pipe_screen *screen;

   /* Refuse to probe. There is no stable UAPI yet. Upstream Mesa cannot be used
    * yet with Asahi. Do not try. Do not patch out this check. Do not teach
    * others about patching this check. Do not distribute upstream Mesa with
    * this check patched out.
    */
   return NULL;

   agx_screen = rzalloc(NULL, struct agx_screen);
   if (!agx_screen)
      return NULL;

   screen = &agx_screen->pscreen;

   /* parse driconf configuration now for device specific overrides */
   driParseConfigFiles(config->options, config->options_info, 0, "asahi", NULL,
                       NULL, NULL, 0, NULL, 0);

   /* Forward no16 flag from driconf */
   if (driQueryOptionb(config->options, "no_fp16"))
      agx_screen->dev.debug |= AGX_DBG_NO16;

   agx_screen->dev.fd = fd;
   agx_screen->dev.ro = ro;
   u_rwlock_init(&agx_screen->destroy_lock);

   /* Try to open an AGX device */
   if (!agx_open_device(agx_screen, &agx_screen->dev)) {
      ralloc_free(agx_screen);
      return NULL;
   }

   int ret =
      drmSyncobjCreate(agx_device(screen)->fd, 0, &agx_screen->flush_syncobj);
   assert(!ret);

   simple_mtx_init(&agx_screen->flush_seqid_lock, mtx_plain);

   screen->destroy = agx_destroy_screen;
   screen->get_screen_fd = agx_screen_get_fd;
   screen->get_name = agx_get_name;
   screen->get_vendor = agx_get_vendor;
   screen->get_device_vendor = agx_get_device_vendor;
   screen->get_param = agx_get_param;
   screen->get_shader_param = agx_get_shader_param;
   screen->get_compute_param = agx_get_compute_param;
   screen->get_paramf = agx_get_paramf;
   screen->get_device_uuid = agx_screen_get_device_uuid;
   screen->get_driver_uuid = agx_screen_get_driver_uuid;
   screen->is_format_supported = agx_is_format_supported;
   screen->query_dmabuf_modifiers = agx_query_dmabuf_modifiers;
   screen->query_memory_info = agx_query_memory_info;
   screen->is_dmabuf_modifier_supported = agx_is_dmabuf_modifier_supported;
   screen->context_create = agx_create_context;
   screen->resource_from_handle = agx_resource_from_handle;
   screen->resource_get_handle = agx_resource_get_handle;
   screen->resource_get_param = agx_resource_get_param;
   screen->resource_create_with_modifiers = agx_resource_create_with_modifiers;
   screen->get_timestamp = agx_get_timestamp;
   screen->fence_reference = agx_fence_reference;
   screen->fence_finish = agx_fence_finish;
   screen->fence_get_fd = agx_fence_get_fd;
   screen->get_compiler_options = agx_get_compiler_options;
   screen->get_disk_shader_cache = agx_get_disk_shader_cache;
   screen->get_cl_cts_version = agx_get_cl_cts_version;

   screen->resource_create = u_transfer_helper_resource_create;
   screen->resource_destroy = u_transfer_helper_resource_destroy;
   screen->transfer_helper = u_transfer_helper_create(
      &transfer_vtbl,
      U_TRANSFER_HELPER_SEPARATE_Z32S8 | U_TRANSFER_HELPER_SEPARATE_STENCIL |
         U_TRANSFER_HELPER_MSAA_MAP | U_TRANSFER_HELPER_Z24_IN_Z32F);

   agx_disk_cache_init(agx_screen);

   return screen;
}
