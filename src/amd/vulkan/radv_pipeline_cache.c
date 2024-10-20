/*
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_pipeline_cache.h"
#include "util/disk_cache.h"
#include "util/macros.h"
#include "util/mesa-blake3.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "nir_serialize.h"
#include "radv_debug.h"
#include "radv_descriptor_set.h"
#include "radv_pipeline.h"
#include "radv_pipeline_compute.h"
#include "radv_pipeline_graphics.h"
#include "radv_pipeline_binary.h"
#include "radv_pipeline_rt.h"
#include "radv_shader.h"
#include "vk_pipeline.h"
#include "vk_util.h"

#include "aco_interface.h"

void
radv_hash_graphics_spirv_to_nir(blake3_hash hash, const struct radv_shader_stage *stage,
                                const struct radv_spirv_to_nir_options *options)
{
   struct mesa_blake3 ctx;
   _mesa_blake3_init(&ctx);
   _mesa_blake3_update(&ctx, &stage->key, sizeof(stage->key));
   _mesa_blake3_update(&ctx, options, sizeof(*options));
   _mesa_blake3_update(&ctx, stage->shader_sha1, sizeof(stage->shader_sha1));
   _mesa_blake3_final(&ctx, hash);
}

static void
radv_shader_destroy(struct vk_device *_device, struct vk_pipeline_cache_object *object)
{
   struct radv_device *device = container_of(_device, struct radv_device, vk);
   struct radv_shader *shader = container_of(object, struct radv_shader, base);

   if (device->shader_use_invisible_vram) {
      /* Wait for any pending upload to complete, or we'll be writing into freed shader memory. */
      radv_shader_wait_for_upload(device, shader->upload_seq);
   }

   radv_free_shader_memory(device, shader->alloc);

   free(shader->code);
   free(shader->spirv);
   free(shader->nir_string);
   free(shader->disasm_string);
   free(shader->ir_string);
   free(shader->statistics);

   vk_pipeline_cache_object_finish(&shader->base);
   free(shader);
}

struct radv_shader *
radv_shader_deserialize(struct radv_device *device, const void *key_data, size_t key_size, struct blob_reader *blob)
{
   const struct radv_shader_binary *binary = blob_read_bytes(blob, sizeof(struct radv_shader_binary));

   struct radv_shader *shader;
   radv_shader_create_uncached(device, binary, false, NULL, &shader);
   if (!shader)
      return NULL;

   assert(key_size == sizeof(shader->hash));
   memcpy(shader->hash, key_data, key_size);
   blob_skip_bytes(blob, binary->total_size - sizeof(struct radv_shader_binary));

   return shader;
}

static struct vk_pipeline_cache_object *
radv_shader_cache_deserialize(struct vk_pipeline_cache *cache, const void *key_data, size_t key_size,
                              struct blob_reader *blob)
{
   struct radv_device *device = container_of(cache->base.device, struct radv_device, vk);
   struct radv_shader *shader;

   shader = radv_shader_deserialize(device, key_data, key_size, blob);

   return shader ? &shader->base : NULL;
}

void
radv_shader_serialize(struct radv_shader *shader, struct blob *blob)
{
   size_t stats_size = shader->statistics ? aco_num_statistics * sizeof(uint32_t) : 0;
   size_t code_size = shader->code_size;
   uint32_t total_size = sizeof(struct radv_shader_binary_legacy) + code_size + stats_size;

   struct radv_shader_binary_legacy binary = {
      .base =
         {
            .type = RADV_BINARY_TYPE_LEGACY,
            .config = shader->config,
            .info = shader->info,
            .total_size = total_size,
         },
      .code_size = code_size,
      .exec_size = shader->exec_size,
      .ir_size = 0,
      .disasm_size = 0,
      .stats_size = stats_size,
   };

   blob_write_bytes(blob, &binary, sizeof(struct radv_shader_binary_legacy));
   blob_write_bytes(blob, shader->statistics, stats_size);
   blob_write_bytes(blob, shader->code, code_size);
}

static bool
radv_shader_cache_serialize(struct vk_pipeline_cache_object *object, struct blob *blob)
{
   struct radv_shader *shader = container_of(object, struct radv_shader, base);

   radv_shader_serialize(shader, blob);
   return true;
}

static bool
radv_is_cache_disabled(const struct radv_device *device, const struct vk_pipeline_cache *cache)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);

   /* The buffer address used for debug printf is hardcoded. */
   if (device->printf.buffer_addr)
      return true;

   /* Pipeline caches can be disabled with RADV_DEBUG=nocache, with MESA_GLSL_CACHE_DISABLE=1 and
    * when ACO_DEBUG is used. MESA_GLSL_CACHE_DISABLE is done elsewhere.
    */
   if ((instance->debug_flags & RADV_DEBUG_NO_CACHE) || (pdev->use_llvm ? 0 : aco_get_codegen_flags()))
      return true;

   if (!cache) {
      /* When the application doesn't provide a pipeline cache and the in-memory cache is also
       * disabled.
       */
      cache = device->mem_cache;
      if (!cache)
         return true;
   }

   return false;
}

struct radv_shader *
radv_shader_create(struct radv_device *device, struct vk_pipeline_cache *cache, const struct radv_shader_binary *binary,
                   bool skip_cache)
{
   if (radv_is_cache_disabled(device, cache) || skip_cache) {
      struct radv_shader *shader;
      radv_shader_create_uncached(device, binary, false, NULL, &shader);
      return shader;
   }

   if (!cache)
      cache = device->mem_cache;

   blake3_hash hash;
   _mesa_blake3_compute(binary, binary->total_size, hash);

   struct vk_pipeline_cache_object *shader_obj;
   shader_obj = vk_pipeline_cache_create_and_insert_object(cache, hash, sizeof(hash), binary, binary->total_size,
                                                           &radv_shader_ops);

   return shader_obj ? container_of(shader_obj, struct radv_shader, base) : NULL;
}

const struct vk_pipeline_cache_object_ops radv_shader_ops = {
   .serialize = radv_shader_cache_serialize,
   .deserialize = radv_shader_cache_deserialize,
   .destroy = radv_shader_destroy,
};

struct radv_pipeline_cache_object {
   struct vk_pipeline_cache_object base;
   unsigned num_shaders;
   uint32_t data_size;
   void *data; /* Generic data stored alongside the shaders */
   uint8_t sha1[SHA1_DIGEST_LENGTH];
   struct radv_shader *shaders[];
};

const struct vk_pipeline_cache_object_ops radv_pipeline_ops;

static struct radv_pipeline_cache_object *
radv_pipeline_cache_object_create(struct vk_device *device, unsigned num_shaders, const void *hash, unsigned data_size)
{
   const size_t size =
      sizeof(struct radv_pipeline_cache_object) + (num_shaders * sizeof(struct radv_shader *)) + data_size;

   struct radv_pipeline_cache_object *object = vk_alloc(&device->alloc, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_CACHE);
   if (!object)
      return NULL;

   vk_pipeline_cache_object_init(device, &object->base, &radv_pipeline_ops, object->sha1, SHA1_DIGEST_LENGTH);
   object->num_shaders = num_shaders;
   object->data = &object->shaders[num_shaders];
   object->data_size = data_size;
   memcpy(object->sha1, hash, SHA1_DIGEST_LENGTH);
   memset(object->shaders, 0, sizeof(object->shaders[0]) * num_shaders);
   memset(object->data, 0, data_size);

   return object;
}

static void
radv_pipeline_cache_object_destroy(struct vk_device *_device, struct vk_pipeline_cache_object *object)
{
   struct radv_device *device = container_of(_device, struct radv_device, vk);
   struct radv_pipeline_cache_object *pipeline_obj = container_of(object, struct radv_pipeline_cache_object, base);

   for (unsigned i = 0; i < pipeline_obj->num_shaders; i++) {
      if (pipeline_obj->shaders[i])
         radv_shader_unref(device, pipeline_obj->shaders[i]);
   }

   vk_pipeline_cache_object_finish(&pipeline_obj->base);
   vk_free(&_device->alloc, pipeline_obj);
}

static struct vk_pipeline_cache_object *
radv_pipeline_cache_object_deserialize(struct vk_pipeline_cache *cache, const void *key_data, size_t key_size,
                                       struct blob_reader *blob)
{
   struct radv_device *device = container_of(cache->base.device, struct radv_device, vk);
   assert(key_size == SHA1_DIGEST_LENGTH);
   unsigned total_size = blob->end - blob->current;
   unsigned num_shaders = blob_read_uint32(blob);
   unsigned data_size = blob_read_uint32(blob);

   struct radv_pipeline_cache_object *object;
   object = radv_pipeline_cache_object_create(&device->vk, num_shaders, key_data, data_size);
   if (!object)
      return NULL;

   object->base.data_size = total_size;

   for (unsigned i = 0; i < num_shaders; i++) {
      const uint8_t *hash = blob_read_bytes(blob, sizeof(blake3_hash));
      struct vk_pipeline_cache_object *shader =
         vk_pipeline_cache_lookup_object(cache, hash, sizeof(blake3_hash), &radv_shader_ops, NULL);

      if (!shader) {
         /* If some shader could not be created from cache, better return NULL here than having
          * an incomplete cache object which needs to be fixed up later.
          */
         vk_pipeline_cache_object_unref(&device->vk, &object->base);
         return NULL;
      }

      object->shaders[i] = container_of(shader, struct radv_shader, base);
   }

   blob_copy_bytes(blob, object->data, data_size);

   return &object->base;
}

static bool
radv_pipeline_cache_object_serialize(struct vk_pipeline_cache_object *object, struct blob *blob)
{
   struct radv_pipeline_cache_object *pipeline_obj = container_of(object, struct radv_pipeline_cache_object, base);

   blob_write_uint32(blob, pipeline_obj->num_shaders);
   blob_write_uint32(blob, pipeline_obj->data_size);

   for (unsigned i = 0; i < pipeline_obj->num_shaders; i++)
      blob_write_bytes(blob, pipeline_obj->shaders[i]->hash, sizeof(pipeline_obj->shaders[i]->hash));

   blob_write_bytes(blob, pipeline_obj->data, pipeline_obj->data_size);

   return true;
}

const struct vk_pipeline_cache_object_ops radv_pipeline_ops = {
   .serialize = radv_pipeline_cache_object_serialize,
   .deserialize = radv_pipeline_cache_object_deserialize,
   .destroy = radv_pipeline_cache_object_destroy,
};

static void
radv_report_pso_cache_stats(struct radv_device *device, const struct radv_pipeline *pipeline, bool cache_hit)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radv_instance *instance = radv_physical_device_instance(pdev);

   if (!(instance->debug_flags & RADV_DEBUG_PSO_CACHE_STATS))
      return;

   /* Only gather PSO cache stats for application pipelines. */
   if (pipeline->is_internal)
      return;

   assert(pipeline->type < ARRAY_SIZE(device->pso_cache_stats));

   simple_mtx_lock(&device->pso_cache_stats_mtx);

   if (cache_hit) {
      device->pso_cache_stats[pipeline->type].hits++;
   } else {
      device->pso_cache_stats[pipeline->type].misses++;
   }

   fprintf(
      stderr,
      "radv: PSO cache stats: gfx (hits=%d, misses=%d), gfx_lib (hits=%d, misses=%d), compute (hits=%d, misses=%d), rt "
      "(hits=%d, misses=%d)\nt",
      device->pso_cache_stats[RADV_PIPELINE_GRAPHICS].hits, device->pso_cache_stats[RADV_PIPELINE_GRAPHICS].misses,
      device->pso_cache_stats[RADV_PIPELINE_GRAPHICS_LIB].hits,
      device->pso_cache_stats[RADV_PIPELINE_GRAPHICS_LIB].misses, device->pso_cache_stats[RADV_PIPELINE_COMPUTE].hits,
      device->pso_cache_stats[RADV_PIPELINE_COMPUTE].misses, device->pso_cache_stats[RADV_PIPELINE_RAY_TRACING].hits,
      device->pso_cache_stats[RADV_PIPELINE_RAY_TRACING].misses);

   simple_mtx_unlock(&device->pso_cache_stats_mtx);
}

static struct radv_pipeline_cache_object *
radv_pipeline_cache_object_search(struct radv_device *device, struct vk_pipeline_cache *cache,
                                  const struct radv_pipeline *pipeline, bool *found_in_application_cache)
{
   *found_in_application_cache = false;

   if (radv_is_cache_disabled(device, cache))
      return false;

   bool *found = found_in_application_cache;
   if (!cache) {
      cache = device->mem_cache;
      found = NULL;
   }

   struct vk_pipeline_cache_object *object =
      vk_pipeline_cache_lookup_object(cache, pipeline->sha1, SHA1_DIGEST_LENGTH, &radv_pipeline_ops, found);

   radv_report_pso_cache_stats(device, pipeline, !!object);

   if (!object)
      return false;

   return container_of(object, struct radv_pipeline_cache_object, base);
}

bool
radv_graphics_pipeline_cache_search(struct radv_device *device, struct vk_pipeline_cache *cache,
                                    struct radv_graphics_pipeline *pipeline, bool *found_in_application_cache)
{
   struct radv_pipeline_cache_object *pipeline_obj;

   pipeline_obj = radv_pipeline_cache_object_search(device, cache, &pipeline->base, found_in_application_cache);
   if (!pipeline_obj)
      return false;

   for (unsigned i = 0; i < pipeline_obj->num_shaders; i++) {
      gl_shader_stage s = pipeline_obj->shaders[i]->info.stage;
      if (s == MESA_SHADER_VERTEX && i > 0) {
         /* The GS copy-shader is a VS placed after all other stages */
         assert(i == pipeline_obj->num_shaders - 1 && pipeline->base.shaders[MESA_SHADER_GEOMETRY]);
         pipeline->base.gs_copy_shader = radv_shader_ref(pipeline_obj->shaders[i]);
      } else {
         pipeline->base.shaders[s] = radv_shader_ref(pipeline_obj->shaders[i]);
      }
   }

   pipeline->base.cache_object = &pipeline_obj->base;
   return true;
}

bool
radv_compute_pipeline_cache_search(struct radv_device *device, struct vk_pipeline_cache *cache,
                                   struct radv_compute_pipeline *pipeline, bool *found_in_application_cache)
{
   struct radv_pipeline_cache_object *pipeline_obj;

   pipeline_obj = radv_pipeline_cache_object_search(device, cache, &pipeline->base, found_in_application_cache);
   if (!pipeline_obj)
      return false;

   assert(pipeline_obj->num_shaders == 1);
   pipeline->base.shaders[MESA_SHADER_COMPUTE] = radv_shader_ref(pipeline_obj->shaders[0]);

   pipeline->base.cache_object = &pipeline_obj->base;
   return true;
}

void
radv_pipeline_cache_insert(struct radv_device *device, struct vk_pipeline_cache *cache, struct radv_pipeline *pipeline)
{
   if (radv_is_cache_disabled(device, cache))
      return;

   if (!cache)
      cache = device->mem_cache;

   /* Count shaders */
   unsigned num_shaders = 0;
   for (unsigned i = 0; i < MESA_VULKAN_SHADER_STAGES; ++i)
      num_shaders += pipeline->shaders[i] ? 1 : 0;
   num_shaders += pipeline->gs_copy_shader ? 1 : 0;

   struct radv_pipeline_cache_object *pipeline_obj;
   pipeline_obj = radv_pipeline_cache_object_create(&device->vk, num_shaders, pipeline->sha1, 0);

   if (!pipeline_obj)
      return;

   unsigned idx = 0;
   for (unsigned i = 0; i < MESA_VULKAN_SHADER_STAGES; ++i) {
      if (pipeline->shaders[i])
         pipeline_obj->shaders[idx++] = radv_shader_ref(pipeline->shaders[i]);
   }
   /* Place the GS copy-shader after all other stages */
   if (pipeline->gs_copy_shader)
      pipeline_obj->shaders[idx++] = radv_shader_ref(pipeline->gs_copy_shader);

   assert(idx == num_shaders);

   /* Add the object to the cache */
   pipeline->cache_object = vk_pipeline_cache_add_object(cache, &pipeline_obj->base);
}

struct radv_ray_tracing_stage_cache_data {
   uint32_t stack_size : 31;
   uint32_t has_shader : 1;
   uint8_t sha1[SHA1_DIGEST_LENGTH];
   struct radv_ray_tracing_stage_info info;
};

struct radv_ray_tracing_pipeline_cache_data {
   uint32_t has_traversal_shader : 1;
   uint32_t is_library : 1;
   uint32_t num_stages;
   struct radv_ray_tracing_stage_cache_data stages[];
};

bool
radv_ray_tracing_pipeline_cache_search(struct radv_device *device, struct vk_pipeline_cache *cache,
                                       struct radv_ray_tracing_pipeline *pipeline,
                                       bool *found_in_application_cache)
{
   struct radv_pipeline_cache_object *pipeline_obj;

   pipeline_obj = radv_pipeline_cache_object_search(device, cache, &pipeline->base.base, found_in_application_cache);
   if (!pipeline_obj)
      return false;

   struct radv_ray_tracing_pipeline_cache_data *data = pipeline_obj->data;

   bool complete = true;
   unsigned idx = 0;

   if (data->has_traversal_shader)
      pipeline->base.base.shaders[MESA_SHADER_INTERSECTION] = radv_shader_ref(pipeline_obj->shaders[idx++]);

   const uint32_t num_stages = data->num_stages;
   for (unsigned i = 0; i < num_stages; i++) {
      pipeline->stages[i].stack_size = data->stages[i].stack_size;
      pipeline->stages[i].info = data->stages[i].info;
      memcpy(pipeline->stages[i].sha1, data->stages[i].sha1, sizeof(pipeline->stages[i].sha1));

      if (data->stages[i].has_shader)
         pipeline->stages[i].shader = radv_shader_ref(pipeline_obj->shaders[idx++]);

      if (data->is_library) {
         pipeline->stages[i].nir = radv_pipeline_cache_lookup_nir_handle(device, cache, pipeline->stages[i].sha1);
         complete &= pipeline->stages[i].nir != NULL;
      }
   }

   assert(idx == pipeline_obj->num_shaders);

   pipeline->base.base.cache_object = &pipeline_obj->base;
   return complete;
}

void
radv_ray_tracing_pipeline_cache_insert(struct radv_device *device, struct vk_pipeline_cache *cache,
                                       struct radv_ray_tracing_pipeline *pipeline, unsigned num_stages)
{
   if (radv_is_cache_disabled(device, cache))
      return;

   if (!cache)
      cache = device->mem_cache;

   /* Skip insertion on cache hit.
    * This branch can be triggered if a cache_object was found but not all NIR shaders could be
    * looked up. The cache_object is already complete in that case.
    */
   if (pipeline->base.base.cache_object)
      return;

   /* Count compiled shaders excl. library shaders */
   unsigned num_shaders = pipeline->base.base.shaders[MESA_SHADER_INTERSECTION] ? 1 : 0;
   for (unsigned i = 0; i < num_stages; ++i)
      num_shaders += pipeline->stages[i].shader ? 1 : 0;

   uint32_t data_size = sizeof(struct radv_ray_tracing_pipeline_cache_data) +
                        num_stages * sizeof(struct radv_ray_tracing_stage_cache_data);

   struct radv_pipeline_cache_object *pipeline_obj =
      radv_pipeline_cache_object_create(&device->vk, num_shaders, pipeline->base.base.sha1, data_size);
   struct radv_ray_tracing_pipeline_cache_data *data = pipeline_obj->data;

   data->is_library = !!(pipeline->base.base.create_flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR);
   data->has_traversal_shader = !!pipeline->base.base.shaders[MESA_SHADER_INTERSECTION];

   unsigned idx = 0;
   if (data->has_traversal_shader)
      pipeline_obj->shaders[idx++] = radv_shader_ref(pipeline->base.base.shaders[MESA_SHADER_INTERSECTION]);

   data->num_stages = num_stages;

   for (unsigned i = 0; i < num_stages; ++i) {
      data->stages[i].stack_size = pipeline->stages[i].stack_size;
      data->stages[i].info = pipeline->stages[i].info;
      data->stages[i].has_shader = !!pipeline->stages[i].shader;
      memcpy(data->stages[i].sha1, pipeline->stages[i].sha1, sizeof(pipeline->stages[i].sha1));

      if (pipeline->stages[i].shader)
         pipeline_obj->shaders[idx++] = radv_shader_ref(pipeline->stages[i].shader);
   }
   assert(idx == num_shaders);

   /* Add the object to the cache */
   pipeline->base.base.cache_object = vk_pipeline_cache_add_object(cache, &pipeline_obj->base);
}

nir_shader *
radv_pipeline_cache_lookup_nir(struct radv_device *device, struct vk_pipeline_cache *cache, gl_shader_stage stage,
                               const blake3_hash key)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (radv_is_cache_disabled(device, cache))
      return NULL;

   if (!cache)
      cache = device->mem_cache;

   return vk_pipeline_cache_lookup_nir(cache, key, sizeof(blake3_hash), &pdev->nir_options[stage], NULL, NULL);
}

void
radv_pipeline_cache_insert_nir(struct radv_device *device, struct vk_pipeline_cache *cache, const blake3_hash key,
                               const nir_shader *nir)
{
   if (radv_is_cache_disabled(device, cache))
      return;

   if (!cache)
      cache = device->mem_cache;

   vk_pipeline_cache_add_nir(cache, key, sizeof(blake3_hash), nir);
}

struct vk_pipeline_cache_object *
radv_pipeline_cache_lookup_nir_handle(struct radv_device *device, struct vk_pipeline_cache *cache, const uint8_t *sha1)
{
   if (radv_is_cache_disabled(device, cache))
      return NULL;

   if (!cache)
      cache = device->mem_cache;

   return vk_pipeline_cache_lookup_object(cache, sha1, SHA1_DIGEST_LENGTH, &vk_raw_data_cache_object_ops, NULL);
}

struct nir_shader *
radv_pipeline_cache_handle_to_nir(struct radv_device *device, struct vk_pipeline_cache_object *object)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct blob_reader blob;
   struct vk_raw_data_cache_object *nir_object = container_of(object, struct vk_raw_data_cache_object, base);
   blob_reader_init(&blob, nir_object->data, nir_object->data_size);
   nir_shader *nir = nir_deserialize(NULL, NULL, &blob);

   if (blob.overrun) {
      ralloc_free(nir);
      return NULL;
   }
   nir->options = &pdev->nir_options[nir->info.stage];

   return nir;
}

struct vk_pipeline_cache_object *
radv_pipeline_cache_nir_to_handle(struct radv_device *device, struct vk_pipeline_cache *cache, struct nir_shader *nir,
                                  const uint8_t *sha1, bool cached)
{
   if (!cache)
      cache = device->mem_cache;

   struct blob blob;
   blob_init(&blob);
   nir_serialize(&blob, nir, true);

   if (blob.out_of_memory) {
      blob_finish(&blob);
      return NULL;
   }

   void *data;
   size_t size;
   blob_finish_get_buffer(&blob, &data, &size);
   struct vk_pipeline_cache_object *object;

   if (cached && !radv_is_cache_disabled(device, cache)) {
      object = vk_pipeline_cache_create_and_insert_object(cache, sha1, SHA1_DIGEST_LENGTH, data, size,
                                                          &vk_raw_data_cache_object_ops);
   } else {
      struct vk_raw_data_cache_object *nir_object =
         vk_raw_data_cache_object_create(&device->vk, sha1, SHA1_DIGEST_LENGTH, data, size);
      object = nir_object ? &nir_object->base : NULL;
   }

   free(data);
   return object;
}

VkResult
radv_pipeline_cache_get_binaries(struct radv_device *device, const VkAllocationCallbacks *pAllocator,
                                 const unsigned char *sha1, struct util_dynarray *pipeline_binaries,
                                 uint32_t *num_binaries, bool *found_in_internal_cache)
{
   struct vk_pipeline_cache *cache = device->mem_cache;
   VkResult result;

   *found_in_internal_cache = false;

   if (radv_is_cache_disabled(device, cache))
      return VK_SUCCESS;

   struct vk_pipeline_cache_object *object =
      vk_pipeline_cache_lookup_object(cache, sha1, SHA1_DIGEST_LENGTH, &radv_pipeline_ops, NULL);
   if (!object)
      return VK_SUCCESS;

   struct radv_pipeline_cache_object *pipeline_obj = container_of(object, struct radv_pipeline_cache_object, base);

   bool complete = true;
   bool is_rt = false;
   for (unsigned i = 0; i < pipeline_obj->num_shaders; i++) {
      if (gl_shader_stage_is_rt(pipeline_obj->shaders[i]->info.stage)) {
         is_rt = true;
         break;
      }
   }

   if (is_rt) {
      struct radv_ray_tracing_pipeline_cache_data *data = pipeline_obj->data;
      struct radv_shader *traversal_shader = NULL;
      unsigned idx = 0;

      if (data->has_traversal_shader)
         traversal_shader = pipeline_obj->shaders[idx++];

      for (unsigned i = 0; i < data->num_stages; i++) {
         const struct radv_ray_tracing_stage_cache_data *stage_data = &data->stages[i];
         struct vk_pipeline_cache_object *nir = NULL;
         struct radv_shader *shader = NULL;

         if (stage_data->has_shader)
            shader = pipeline_obj->shaders[idx++];

         if (data->is_library)
            nir = radv_pipeline_cache_lookup_nir_handle(device, cache, data->stages[i].sha1);

         result = radv_create_pipeline_binary_from_rt_shader(device, pAllocator, shader, false, data->stages[i].sha1,
                                                             &stage_data->info, stage_data->stack_size, nir,
                                                             pipeline_binaries, num_binaries);

         if (data->is_library)
            complete &= nir != NULL;

         if (nir)
            vk_pipeline_cache_object_unref(&device->vk, nir);

         if (result != VK_SUCCESS)
            goto fail;
      }

      if (traversal_shader) {
         result = radv_create_pipeline_binary_from_rt_shader(device, pAllocator, traversal_shader, true,
                                                             traversal_shader->hash, NULL, 0, NULL, pipeline_binaries,
                                                             num_binaries);
         if (result != VK_SUCCESS)
            goto fail;
      }
   } else {
      struct radv_shader *gs_copy_shader = NULL;

      for (unsigned i = 0; i < pipeline_obj->num_shaders; i++) {
         struct radv_shader *shader = pipeline_obj->shaders[i];
         gl_shader_stage s = shader->info.stage;

         if (s == MESA_SHADER_VERTEX && i > 0) {
            /* The GS copy-shader is a VS placed after all other stages */
            gs_copy_shader = shader;
         } else {
            result =
               radv_create_pipeline_binary_from_shader(device, pAllocator, shader, pipeline_binaries, num_binaries);
            if (result != VK_SUCCESS)
               goto fail;
         }
      }

      if (gs_copy_shader) {
         result = radv_create_pipeline_binary_from_shader(device, pAllocator, gs_copy_shader, pipeline_binaries,
                                                          num_binaries);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   *found_in_internal_cache = complete;

fail:
   vk_pipeline_cache_object_unref(&device->vk, &pipeline_obj->base);
   return result;
}
