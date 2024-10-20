/*
 * Copyright © 2022 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#include "nvk_descriptor_set.h"

#include "nvk_buffer.h"
#include "nvk_buffer_view.h"
#include "nvk_descriptor_set_layout.h"
#include "nvk_device.h"
#include "nvk_entrypoints.h"
#include "nvk_format.h"
#include "nvk_image_view.h"
#include "nvk_physical_device.h"
#include "nvk_sampler.h"
#include "nvkmd/nvkmd.h"

#include "util/format/u_format.h"

static inline uint32_t
align_u32(uint32_t v, uint32_t a)
{
   assert(a != 0 && a == (a & -a));
   return (v + a - 1) & ~(a - 1);
}

static inline void *
desc_ubo_data(struct nvk_descriptor_set *set, uint32_t binding,
              uint32_t elem, uint32_t *size_out)
{
   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding];

   uint32_t offset = binding_layout->offset + elem * binding_layout->stride;
   assert(offset < set->size);

   if (size_out != NULL)
      *size_out = set->size - offset;

   return (char *)set->mapped_ptr + offset;
}

static void
write_desc(struct nvk_descriptor_set *set, uint32_t binding, uint32_t elem,
           const void *desc_data, size_t desc_size)
{
   ASSERTED uint32_t dst_size;
   void *dst = desc_ubo_data(set, binding, elem, &dst_size);
   assert(desc_size <= dst_size);
   memcpy(dst, desc_data, desc_size);
}

static void
get_sampled_image_view_desc(VkDescriptorType descriptor_type,
                            const VkDescriptorImageInfo *const info,
                            void *dst, size_t dst_size)
{
   struct nvk_sampled_image_descriptor desc[3] = { };
   uint8_t plane_count = 1;

   if (descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLER &&
       info && info->imageView != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(nvk_image_view, view, info->imageView);

      plane_count = view->plane_count;
      for (uint8_t plane = 0; plane < plane_count; plane++) {
         assert(view->planes[plane].sampled_desc_index > 0);
         assert(view->planes[plane].sampled_desc_index < (1 << 20));
         desc[plane].image_index = view->planes[plane].sampled_desc_index;
      }
   }

   if (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ||
       descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
      VK_FROM_HANDLE(nvk_sampler, sampler, info->sampler);

      plane_count = MAX2(plane_count, sampler->plane_count);

      for (uint8_t plane = 0; plane < plane_count; plane++) {
         /* We need to replicate the last sampler plane out to all image
          * planes due to sampler table entry limitations. See
          * nvk_CreateSampler in nvk_sampler.c for more details.
          */
         uint8_t sampler_plane = MIN2(plane, sampler->plane_count - 1);
         assert(sampler->planes[sampler_plane].desc_index < (1 << 12));
         desc[plane].sampler_index = sampler->planes[sampler_plane].desc_index;
      }
   }

   assert(sizeof(desc[0]) * plane_count <= dst_size);
   memcpy(dst, desc, sizeof(desc[0]) * plane_count);
}

static void
write_sampled_image_view_desc(struct nvk_descriptor_set *set,
                              const VkDescriptorImageInfo *const _info,
                              uint32_t binding, uint32_t elem,
                              VkDescriptorType descriptor_type)
{
   VkDescriptorImageInfo info = *_info;

   if (descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ||
       descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
      const struct nvk_descriptor_set_binding_layout *binding_layout =
         &set->layout->binding[binding];
      if (binding_layout->immutable_samplers != NULL) {
         info.sampler = nvk_sampler_to_handle(
            binding_layout->immutable_samplers[elem]);
      }
   }

   uint32_t dst_size;
   void *dst = desc_ubo_data(set, binding, elem, &dst_size);
   get_sampled_image_view_desc(descriptor_type, &info, dst, dst_size);
}

static void
get_storage_image_view_desc(const VkDescriptorImageInfo *const info,
                            void *dst, size_t dst_size)
{
   struct nvk_storage_image_descriptor desc = { };

   if (info && info->imageView != VK_NULL_HANDLE) {
      VK_FROM_HANDLE(nvk_image_view, view, info->imageView);

      /* Storage images are always single plane */
      assert(view->plane_count == 1);
      uint8_t plane = 0;

      assert(view->planes[plane].storage_desc_index > 0);
      assert(view->planes[plane].storage_desc_index < (1 << 20));

      desc.image_index = view->planes[plane].storage_desc_index;

      const struct nil_Extent4D_Samples px_extent_sa =
         nil_px_extent_sa(view->planes[plane].sample_layout);
      desc.sw_log2 = util_logbase2(px_extent_sa.width);
      desc.sh_log2 = util_logbase2(px_extent_sa.height);

      const enum nil_sample_layout slayout = view->planes[plane].sample_layout;
      if (slayout != NIL_SAMPLE_LAYOUT_1X1) {
         uint32_t samples = nil_sample_layout_samples(slayout);
         assert(samples <= 16);
         for (uint32_t s = 0; s < samples; s++) {
            const struct nil_sample_offset off = nil_sample_offset(slayout, s);
            assert(off.x < 4 && off.y < 4);
            uint32_t s_xy = off.y << 2 | off.x;
            desc.sample_map |= s_xy << (s * 4);
         }
      }
   }

   assert(sizeof(desc) <= dst_size);
   memcpy(dst, &desc, sizeof(desc));
}

static void
write_storage_image_view_desc(struct nvk_descriptor_set *set,
                              const VkDescriptorImageInfo *const info,
                              uint32_t binding, uint32_t elem)
{
   uint32_t dst_size;
   void *dst = desc_ubo_data(set, binding, elem, &dst_size);
   get_storage_image_view_desc(info, dst, dst_size);
}

static union nvk_buffer_descriptor
ubo_desc(struct nvk_physical_device *pdev,
         struct nvk_addr_range addr_range)
{
   const uint32_t min_cbuf_alignment = nvk_min_cbuf_alignment(&pdev->info);

   assert(addr_range.addr % min_cbuf_alignment == 0);
   assert(addr_range.range <= NVK_MAX_CBUF_SIZE);

   addr_range.addr = align64(addr_range.addr, min_cbuf_alignment);
   addr_range.range = align(addr_range.range, min_cbuf_alignment);

   if (nvk_use_bindless_cbuf(&pdev->info)) {
      return (union nvk_buffer_descriptor) { .cbuf = {
         .base_addr_shift_4 = addr_range.addr >> 4,
         .size_shift_4 = addr_range.range >> 4,
      }};
   } else {
      return (union nvk_buffer_descriptor) { .addr = {
         .base_addr = addr_range.addr,
         .size = addr_range.range,
      }};
   }
}

static void
write_ubo_desc(struct nvk_physical_device *pdev,
               struct nvk_descriptor_set *set,
               const VkDescriptorBufferInfo *const info,
               uint32_t binding, uint32_t elem)
{
   VK_FROM_HANDLE(nvk_buffer, buffer, info->buffer);
   struct nvk_addr_range addr_range =
      nvk_buffer_addr_range(buffer, info->offset, info->range);

   const union nvk_buffer_descriptor desc = ubo_desc(pdev, addr_range);
   write_desc(set, binding, elem, &desc, sizeof(desc));
}

static void
write_dynamic_ubo_desc(struct nvk_physical_device *pdev,
                       struct nvk_descriptor_set *set,
                       const VkDescriptorBufferInfo *const info,
                       uint32_t binding, uint32_t elem)
{
   VK_FROM_HANDLE(nvk_buffer, buffer, info->buffer);
   struct nvk_addr_range addr_range =
      nvk_buffer_addr_range(buffer, info->offset, info->range);

   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding];
   set->dynamic_buffers[binding_layout->dynamic_buffer_index + elem] =
      ubo_desc(pdev, addr_range);
}

static union nvk_buffer_descriptor
ssbo_desc(struct nvk_addr_range addr_range)
{
   assert(addr_range.addr % NVK_MIN_SSBO_ALIGNMENT == 0);
   assert(addr_range.range <= UINT32_MAX);

   addr_range.addr = align64(addr_range.addr, NVK_MIN_SSBO_ALIGNMENT);
   addr_range.range = align(addr_range.range, NVK_SSBO_BOUNDS_CHECK_ALIGNMENT);

   return (union nvk_buffer_descriptor) { .addr = {
      .base_addr = addr_range.addr,
      .size = addr_range.range,
   }};
}

static void
write_ssbo_desc(struct nvk_descriptor_set *set,
                const VkDescriptorBufferInfo *const info,
                uint32_t binding, uint32_t elem)
{
   VK_FROM_HANDLE(nvk_buffer, buffer, info->buffer);
   struct nvk_addr_range addr_range =
      nvk_buffer_addr_range(buffer, info->offset, info->range);

   const union nvk_buffer_descriptor desc = ssbo_desc(addr_range);
   write_desc(set, binding, elem, &desc, sizeof(desc));
}

static void
write_dynamic_ssbo_desc(struct nvk_descriptor_set *set,
                        const VkDescriptorBufferInfo *const info,
                        uint32_t binding, uint32_t elem)
{
   VK_FROM_HANDLE(nvk_buffer, buffer, info->buffer);
   struct nvk_addr_range addr_range =
      nvk_buffer_addr_range(buffer, info->offset, info->range);

   const struct nvk_descriptor_set_binding_layout *binding_layout =
      &set->layout->binding[binding];
   set->dynamic_buffers[binding_layout->dynamic_buffer_index + elem] =
      ssbo_desc(addr_range);
}

static void
get_edb_buffer_view_desc(struct nvk_device *dev,
                         const VkDescriptorAddressInfoEXT *info,
                         void *dst, size_t dst_size)
{
   struct nvk_edb_buffer_view_descriptor desc = { };
   if (info != NULL && info->address != 0) {
      enum pipe_format format = nvk_format_to_pipe_format(info->format);
      desc = nvk_edb_bview_cache_get_descriptor(dev, &dev->edb_bview_cache,
                                                info->address, info->range,
                                                format);
   }
   assert(sizeof(desc) <= dst_size);
   memcpy(dst, &desc, sizeof(desc));
}

static void
write_buffer_view_desc(struct nvk_physical_device *pdev,
                       struct nvk_descriptor_set *set,
                       const VkBufferView bufferView,
                       uint32_t binding, uint32_t elem)
{
   VK_FROM_HANDLE(nvk_buffer_view, view, bufferView);

   if (nvk_use_edb_buffer_views(pdev)) {
      struct nvk_edb_buffer_view_descriptor desc = { };
      if (view != NULL)
         desc = view->edb_desc;
      write_desc(set, binding, elem, &desc, sizeof(desc));
   } else {
      struct nvk_buffer_view_descriptor desc = { };
      if (view != NULL)
         desc = view->desc;
      write_desc(set, binding, elem, &desc, sizeof(desc));
   }
}

static void
write_inline_uniform_data(struct nvk_descriptor_set *set,
                          const VkWriteDescriptorSetInlineUniformBlock *info,
                          uint32_t binding, uint32_t offset)
{
   assert(set->layout->binding[binding].stride == 1);
   write_desc(set, binding, offset, info->pData, info->dataSize);
}

VKAPI_ATTR void VKAPI_CALL
nvk_UpdateDescriptorSets(VkDevice device,
                         uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   struct nvk_physical_device *pdev = nvk_device_physical(dev);

   for (uint32_t w = 0; w < descriptorWriteCount; w++) {
      const VkWriteDescriptorSet *write = &pDescriptorWrites[w];
      VK_FROM_HANDLE(nvk_descriptor_set, set, write->dstSet);

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_sampled_image_view_desc(set, write->pImageInfo + j,
                                          write->dstBinding,
                                          write->dstArrayElement + j,
                                          write->descriptorType);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_storage_image_view_desc(set, write->pImageInfo + j,
                                          write->dstBinding,
                                          write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_buffer_view_desc(pdev, set, write->pTexelBufferView[j],
                                   write->dstBinding, write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_ubo_desc(pdev, set, write->pBufferInfo + j,
                           write->dstBinding,
                           write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_ssbo_desc(set, write->pBufferInfo + j,
                            write->dstBinding,
                            write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_dynamic_ubo_desc(pdev, set, write->pBufferInfo + j,
                                   write->dstBinding,
                                   write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_dynamic_ssbo_desc(set, write->pBufferInfo + j,
                                    write->dstBinding,
                                    write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK: {
         const VkWriteDescriptorSetInlineUniformBlock *write_inline =
            vk_find_struct_const(write->pNext,
                                 WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK);
         assert(write_inline->dataSize == write->descriptorCount);
         write_inline_uniform_data(set, write_inline, write->dstBinding,
                                   write->dstArrayElement);
         break;
      }

      default:
         break;
      }
   }

   for (uint32_t i = 0; i < descriptorCopyCount; i++) {
      const VkCopyDescriptorSet *copy = &pDescriptorCopies[i];
      VK_FROM_HANDLE(nvk_descriptor_set, src, copy->srcSet);
      VK_FROM_HANDLE(nvk_descriptor_set, dst, copy->dstSet);

      const struct nvk_descriptor_set_binding_layout *src_binding_layout =
         &src->layout->binding[copy->srcBinding];
      const struct nvk_descriptor_set_binding_layout *dst_binding_layout =
         &dst->layout->binding[copy->dstBinding];

      if (dst_binding_layout->stride > 0 && src_binding_layout->stride > 0) {
         for (uint32_t j = 0; j < copy->descriptorCount; j++) {
            ASSERTED uint32_t dst_max_size, src_max_size;
            void *dst_map = desc_ubo_data(dst, copy->dstBinding,
                                          copy->dstArrayElement + j,
                                          &dst_max_size);
            const void *src_map = desc_ubo_data(src, copy->srcBinding,
                                                copy->srcArrayElement + j,
                                                &src_max_size);
            const uint32_t copy_size = MIN2(dst_binding_layout->stride,
                                            src_binding_layout->stride);
            assert(copy_size <= dst_max_size && copy_size <= src_max_size);
            memcpy(dst_map, src_map, copy_size);
         }
      }

      switch (src_binding_layout->type) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
         const uint32_t dst_dyn_start =
            dst_binding_layout->dynamic_buffer_index + copy->dstArrayElement;
         const uint32_t src_dyn_start =
            src_binding_layout->dynamic_buffer_index + copy->srcArrayElement;
         typed_memcpy(&dst->dynamic_buffers[dst_dyn_start],
                      &src->dynamic_buffers[src_dyn_start],
                      copy->descriptorCount);
         break;
      }
      default:
         break;
      }
   }
}

void
nvk_push_descriptor_set_update(struct nvk_device *dev,
                               struct nvk_push_descriptor_set *push_set,
                               struct nvk_descriptor_set_layout *layout,
                               uint32_t write_count,
                               const VkWriteDescriptorSet *writes)
{
   struct nvk_physical_device *pdev = nvk_device_physical(dev);

   assert(layout->non_variable_descriptor_buffer_size < sizeof(push_set->data));
   struct nvk_descriptor_set set = {
      .layout = layout,
      .size = sizeof(push_set->data),
      .mapped_ptr = push_set->data,
   };

   for (uint32_t w = 0; w < write_count; w++) {
      const VkWriteDescriptorSet *write = &writes[w];
      assert(write->dstSet == VK_NULL_HANDLE);

      switch (write->descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_sampled_image_view_desc(&set, write->pImageInfo + j,
                                          write->dstBinding,
                                          write->dstArrayElement + j,
                                          write->descriptorType);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_storage_image_view_desc(&set, write->pImageInfo + j,
                                          write->dstBinding,
                                          write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_buffer_view_desc(pdev, &set, write->pTexelBufferView[j],
                                   write->dstBinding, write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_ubo_desc(pdev, &set, write->pBufferInfo + j,
                           write->dstBinding,
                           write->dstArrayElement + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         for (uint32_t j = 0; j < write->descriptorCount; j++) {
            write_ssbo_desc(&set, write->pBufferInfo + j,
                            write->dstBinding,
                            write->dstArrayElement + j);
         }
         break;

      default:
         break;
      }
   }
}

static void
nvk_descriptor_pool_free(struct nvk_descriptor_pool *pool,
                         uint64_t addr, uint64_t size);

static void
nvk_descriptor_set_destroy(struct nvk_device *dev,
                           struct nvk_descriptor_pool *pool,
                           struct nvk_descriptor_set *set)
{
   list_del(&set->link);
   if (set->size > 0)
      nvk_descriptor_pool_free(pool, set->addr, set->size);
   vk_descriptor_set_layout_unref(&dev->vk, &set->layout->vk);

   vk_object_free(&dev->vk, NULL, set);
}

static void
nvk_destroy_descriptor_pool(struct nvk_device *dev,
                            const VkAllocationCallbacks *pAllocator,
                            struct nvk_descriptor_pool *pool)
{
   list_for_each_entry_safe(struct nvk_descriptor_set, set, &pool->sets, link)
      nvk_descriptor_set_destroy(dev, pool, set);

   util_vma_heap_finish(&pool->heap);

   if (pool->mem != NULL)
      nvkmd_mem_unref(pool->mem);

   vk_object_free(&dev->vk, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_CreateDescriptorPool(VkDevice _device,
                         const VkDescriptorPoolCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDescriptorPool *pDescriptorPool)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   struct nvk_physical_device *pdev = nvk_device_physical(dev);
   struct nvk_descriptor_pool *pool;
   VkResult result;

   pool = vk_object_zalloc(&dev->vk, pAllocator, sizeof(*pool),
                           VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   if (!pool)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   list_inithead(&pool->sets);

   const VkMutableDescriptorTypeCreateInfoEXT *mutable_info =
      vk_find_struct_const(pCreateInfo->pNext,
                           MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT);

   uint32_t max_align = 0;
   for (unsigned i = 0; i < pCreateInfo->poolSizeCount; ++i) {
      const VkMutableDescriptorTypeListEXT *type_list = NULL;
      if (pCreateInfo->pPoolSizes[i].type == VK_DESCRIPTOR_TYPE_MUTABLE_EXT &&
          mutable_info && i < mutable_info->mutableDescriptorTypeListCount)
            type_list = &mutable_info->pMutableDescriptorTypeLists[i];

      uint32_t stride, alignment;
      nvk_descriptor_stride_align_for_type(pdev, 0 /* not DESCRIPTOR_BUFFER */,
                                           pCreateInfo->pPoolSizes[i].type,
                                           type_list, &stride, &alignment);
      max_align = MAX2(max_align, alignment);
   }

   uint64_t mem_size = 0;
   for (unsigned i = 0; i < pCreateInfo->poolSizeCount; ++i) {
      const VkMutableDescriptorTypeListEXT *type_list = NULL;
      if (pCreateInfo->pPoolSizes[i].type == VK_DESCRIPTOR_TYPE_MUTABLE_EXT &&
          mutable_info && i < mutable_info->mutableDescriptorTypeListCount)
            type_list = &mutable_info->pMutableDescriptorTypeLists[i];

      uint32_t stride, alignment;
      nvk_descriptor_stride_align_for_type(pdev, 0 /* not DESCRIPTOR_BUFFER */,
                                           pCreateInfo->pPoolSizes[i].type,
                                           type_list, &stride, &alignment);
      mem_size += MAX2(stride, max_align) *
                 pCreateInfo->pPoolSizes[i].descriptorCount;
   }

   /* Individual descriptor sets are aligned to the min UBO alignment to
    * ensure that we don't end up with unaligned data access in any shaders.
    * This means that each descriptor buffer allocated may burn up to 16B of
    * extra space to get the right alignment.  (Technically, it's at most 28B
    * because we're always going to start at least 4B aligned but we're being
    * conservative here.)  Allocate enough extra space that we can chop it
    * into maxSets pieces and align each one of them to 32B.
    */
   mem_size += nvk_min_cbuf_alignment(&pdev->info) * pCreateInfo->maxSets;

   if (mem_size) {
      result = nvkmd_dev_alloc_mapped_mem(dev->nvkmd, &dev->vk.base,
                                          mem_size, 0, NVKMD_MEM_LOCAL,
                                          NVKMD_MEM_MAP_WR, &pool->mem);
      if (result != VK_SUCCESS) {
         nvk_destroy_descriptor_pool(dev, pAllocator, pool);
         return result;
      }

      /* The BO may be larger thanks to GPU page alignment.  We may as well
       * make that extra space available to the client.
       */
      assert(pool->mem->size_B >= mem_size);
      util_vma_heap_init(&pool->heap, pool->mem->va->addr, pool->mem->size_B);
   } else {
      util_vma_heap_init(&pool->heap, 0, 0);
   }

   *pDescriptorPool = nvk_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

static VkResult
nvk_descriptor_pool_alloc(struct nvk_descriptor_pool *pool,
                          uint64_t size, uint64_t alignment,
                          uint64_t *addr_out, void **map_out)
{
   assert(size > 0);
   assert(size % alignment == 0);

   if (size > pool->heap.free_size)
      return VK_ERROR_OUT_OF_POOL_MEMORY;

   uint64_t addr = util_vma_heap_alloc(&pool->heap, size, alignment);
   if (addr == 0)
      return VK_ERROR_FRAGMENTED_POOL;

   assert(addr >= pool->mem->va->addr);
   assert(addr + size <= pool->mem->va->addr + pool->mem->size_B);
   uint64_t offset = addr - pool->mem->va->addr;

   *addr_out = addr;
   *map_out = pool->mem->map + offset;

   return VK_SUCCESS;
}

static void
nvk_descriptor_pool_free(struct nvk_descriptor_pool *pool,
                         uint64_t addr, uint64_t size)
{
   assert(size > 0);
   assert(addr >= pool->mem->va->addr);
   assert(addr + size <= pool->mem->va->addr + pool->mem->size_B);
   util_vma_heap_free(&pool->heap, addr, size);
}

static VkResult
nvk_descriptor_set_create(struct nvk_device *dev,
                          struct nvk_descriptor_pool *pool,
                          struct nvk_descriptor_set_layout *layout,
                          uint32_t variable_count,
                          struct nvk_descriptor_set **out_set)
{
   struct nvk_physical_device *pdev = nvk_device_physical(dev);
   struct nvk_descriptor_set *set;
   VkResult result;

   uint32_t mem_size = sizeof(struct nvk_descriptor_set) +
      layout->dynamic_buffer_count * sizeof(struct nvk_buffer_address);

   set = vk_object_zalloc(&dev->vk, NULL, mem_size,
                          VK_OBJECT_TYPE_DESCRIPTOR_SET);
   if (!set)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   set->size = layout->non_variable_descriptor_buffer_size;

   if (layout->binding_count > 0 &&
       (layout->binding[layout->binding_count - 1].flags &
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)) {
      uint32_t stride = layout->binding[layout->binding_count-1].stride;
      set->size += stride * variable_count;
   }

   uint32_t alignment = nvk_min_cbuf_alignment(&pdev->info);
   set->size = align64(set->size, alignment);

   if (set->size > 0) {
      result = nvk_descriptor_pool_alloc(pool, set->size, alignment,
                                         &set->addr, &set->mapped_ptr);
      if (result != VK_SUCCESS) {
         vk_object_free(&dev->vk, NULL, set);
         return result;
      }
   }

   vk_descriptor_set_layout_ref(&layout->vk);
   set->layout = layout;

   for (uint32_t b = 0; b < layout->binding_count; b++) {
      if (layout->binding[b].type != VK_DESCRIPTOR_TYPE_SAMPLER &&
          layout->binding[b].type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
         continue;

      if (layout->binding[b].immutable_samplers == NULL)
         continue;

      uint32_t array_size = layout->binding[b].array_size;
      if (layout->binding[b].flags &
          VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)
         array_size = variable_count;

      const VkDescriptorImageInfo empty = {};
      for (uint32_t j = 0; j < array_size; j++) {
         write_sampled_image_view_desc(set, &empty, b, j,
                                       layout->binding[b].type);
      }
   }

   list_addtail(&set->link, &pool->sets);
   *out_set = set;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_AllocateDescriptorSets(VkDevice device,
                           const VkDescriptorSetAllocateInfo *pAllocateInfo,
                           VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, pAllocateInfo->descriptorPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   struct nvk_descriptor_set *set = NULL;

   const VkDescriptorSetVariableDescriptorCountAllocateInfo *var_desc_count =
      vk_find_struct_const(pAllocateInfo->pNext,
                           DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO);

   /* allocate a set of buffers for each shader to contain descriptors */
   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      VK_FROM_HANDLE(nvk_descriptor_set_layout, layout,
                     pAllocateInfo->pSetLayouts[i]);
      /* If descriptorSetCount is zero or this structure is not included in
       * the pNext chain, then the variable lengths are considered to be zero.
       */
      const uint32_t variable_count =
         var_desc_count && var_desc_count->descriptorSetCount > 0 ?
         var_desc_count->pDescriptorCounts[i] : 0;

      result = nvk_descriptor_set_create(dev, pool, layout,
                                         variable_count, &set);
      if (result != VK_SUCCESS)
         break;

      pDescriptorSets[i] = nvk_descriptor_set_to_handle(set);
   }

   if (result != VK_SUCCESS) {
      nvk_FreeDescriptorSets(device, pAllocateInfo->descriptorPool, i, pDescriptorSets);
      for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
         pDescriptorSets[i] = VK_NULL_HANDLE;
      }
   }
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_FreeDescriptorSets(VkDevice device,
                       VkDescriptorPool descriptorPool,
                       uint32_t descriptorSetCount,
                       const VkDescriptorSet *pDescriptorSets)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, descriptorPool);

   for (uint32_t i = 0; i < descriptorSetCount; i++) {
      VK_FROM_HANDLE(nvk_descriptor_set, set, pDescriptorSets[i]);

      if (set)
         nvk_descriptor_set_destroy(dev, pool, set);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
nvk_DestroyDescriptorPool(VkDevice device,
                          VkDescriptorPool _pool,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, _pool);

   if (!_pool)
      return;

   nvk_destroy_descriptor_pool(dev, pAllocator, pool);
}

VKAPI_ATTR VkResult VKAPI_CALL
nvk_ResetDescriptorPool(VkDevice device,
                        VkDescriptorPool descriptorPool,
                        VkDescriptorPoolResetFlags flags)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_descriptor_pool, pool, descriptorPool);

   list_for_each_entry_safe(struct nvk_descriptor_set, set, &pool->sets, link)
      nvk_descriptor_set_destroy(dev, pool, set);

   return VK_SUCCESS;
}

static void
nvk_descriptor_set_write_template(struct nvk_device *dev,
                                  struct nvk_descriptor_set *set,
                                  const struct vk_descriptor_update_template *template,
                                  const void *data)
{
   struct nvk_physical_device *pdev = nvk_device_physical(dev);

   for (uint32_t i = 0; i < template->entry_count; i++) {
      const struct vk_descriptor_template_entry *entry =
         &template->entries[i];

      switch (entry->type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         for (uint32_t j = 0; j < entry->array_count; j++) {
            const VkDescriptorImageInfo *info =
               data + entry->offset + j * entry->stride;

            write_sampled_image_view_desc(set, info,
                                          entry->binding,
                                          entry->array_element + j,
                                          entry->type);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
         for (uint32_t j = 0; j < entry->array_count; j++) {
            const VkDescriptorImageInfo *info =
               data + entry->offset + j * entry->stride;

            write_storage_image_view_desc(set, info,
                                          entry->binding,
                                          entry->array_element + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         for (uint32_t j = 0; j < entry->array_count; j++) {
            const VkBufferView *bview =
               data + entry->offset + j * entry->stride;

            write_buffer_view_desc(pdev, set, *bview,
                                   entry->binding,
                                   entry->array_element + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
         for (uint32_t j = 0; j < entry->array_count; j++) {
            const VkDescriptorBufferInfo *info =
               data + entry->offset + j * entry->stride;

            write_ubo_desc(pdev, set, info,
                           entry->binding,
                           entry->array_element + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
         for (uint32_t j = 0; j < entry->array_count; j++) {
            const VkDescriptorBufferInfo *info =
               data + entry->offset + j * entry->stride;

            write_ssbo_desc(set, info,
                            entry->binding,
                            entry->array_element + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < entry->array_count; j++) {
            const VkDescriptorBufferInfo *info =
               data + entry->offset + j * entry->stride;

            write_dynamic_ubo_desc(pdev, set, info,
                                   entry->binding,
                                   entry->array_element + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         for (uint32_t j = 0; j < entry->array_count; j++) {
            const VkDescriptorBufferInfo *info =
               data + entry->offset + j * entry->stride;

            write_dynamic_ssbo_desc(set, info,
                                    entry->binding,
                                    entry->array_element + j);
         }
         break;

      case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
         write_desc(set,
                    entry->binding,
                    entry->array_element,
                    data + entry->offset,
                    entry->array_count);
         break;

      default:
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
nvk_UpdateDescriptorSetWithTemplate(VkDevice device,
                                    VkDescriptorSet descriptorSet,
                                    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                    const void *pData)
{
   VK_FROM_HANDLE(nvk_device, dev, device);
   VK_FROM_HANDLE(nvk_descriptor_set, set, descriptorSet);
   VK_FROM_HANDLE(vk_descriptor_update_template, template,
                  descriptorUpdateTemplate);

   nvk_descriptor_set_write_template(dev, set, template, pData);
}

void
nvk_push_descriptor_set_update_template(
   struct nvk_device *dev,
   struct nvk_push_descriptor_set *push_set,
   struct nvk_descriptor_set_layout *layout,
   const struct vk_descriptor_update_template *template,
   const void *data)
{
   struct nvk_descriptor_set tmp_set = {
      .layout = layout,
      .size = sizeof(push_set->data),
      .mapped_ptr = push_set->data,
   };
   nvk_descriptor_set_write_template(dev, &tmp_set, template, data);
}

VKAPI_ATTR void VKAPI_CALL
nvk_GetDescriptorEXT(VkDevice _device,
                     const VkDescriptorGetInfoEXT *pDescriptorInfo,
                     size_t dataSize, void *pDescriptor)
{
   VK_FROM_HANDLE(nvk_device, dev, _device);
   struct nvk_physical_device *pdev = nvk_device_physical(dev);

   switch (pDescriptorInfo->type) {
   case VK_DESCRIPTOR_TYPE_SAMPLER: {
      const VkDescriptorImageInfo info = {
         .sampler = *pDescriptorInfo->data.pSampler,
      };
      get_sampled_image_view_desc(VK_DESCRIPTOR_TYPE_SAMPLER,
                                  &info, pDescriptor, dataSize);
      break;
   }

   case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      get_sampled_image_view_desc(pDescriptorInfo->type,
                                  pDescriptorInfo->data.pCombinedImageSampler,
                                  pDescriptor, dataSize);
      break;

   case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      get_sampled_image_view_desc(pDescriptorInfo->type,
                                  pDescriptorInfo->data.pSampledImage,
                                  pDescriptor, dataSize);
      break;

   case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      get_storage_image_view_desc(pDescriptorInfo->data.pStorageImage,
                                  pDescriptor, dataSize);
      break;

   case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      get_edb_buffer_view_desc(dev, pDescriptorInfo->data.pUniformTexelBuffer,
                               pDescriptor, dataSize);
      break;

   case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
      get_edb_buffer_view_desc(dev, pDescriptorInfo->data.pStorageTexelBuffer,
                               pDescriptor, dataSize);
      break;

   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: {
      struct nvk_addr_range addr_range = { };
      if (pDescriptorInfo->data.pUniformBuffer != NULL &&
          pDescriptorInfo->data.pUniformBuffer->address != 0) {
         addr_range = (const struct nvk_addr_range) {
            .addr = pDescriptorInfo->data.pUniformBuffer->address,
            .range = pDescriptorInfo->data.pUniformBuffer->range,
         };
      }
      union nvk_buffer_descriptor desc = ubo_desc(pdev, addr_range);
      assert(sizeof(desc) <= dataSize);
      memcpy(pDescriptor, &desc, sizeof(desc));
      break;
   }

   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
      struct nvk_addr_range addr_range = { };
      if (pDescriptorInfo->data.pUniformBuffer != NULL &&
          pDescriptorInfo->data.pUniformBuffer->address != 0) {
         addr_range = (const struct nvk_addr_range) {
            .addr = pDescriptorInfo->data.pUniformBuffer->address,
            .range = pDescriptorInfo->data.pUniformBuffer->range,
         };
      }
      union nvk_buffer_descriptor desc = ssbo_desc(addr_range);
      assert(sizeof(desc) <= dataSize);
      memcpy(pDescriptor, &desc, sizeof(desc));
      break;
   }

   case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
      get_sampled_image_view_desc(pDescriptorInfo->type,
                                  pDescriptorInfo->data.pInputAttachmentImage,
                                  pDescriptor, dataSize);
      break;

   default:
      unreachable("Unknown descriptor type");
   }
}
