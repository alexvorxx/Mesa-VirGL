/*
 * Copyright © 2020 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nir_serialize.h"

#include "ir3_compiler.h"
#include "ir3_nir.h"

#define debug 0

/*
 * Shader disk-cache implementation.
 *
 * Note that at least in the EGL_ANDROID_blob_cache, we should never
 * rely on inter-dependencies between different cache entries:
 *
 *    No guarantees are made as to whether a given key/value pair is present in
 *    the cache after the set call.  If a different value has been associated
 *    with the given key in the past then it is undefined which value, if any,
 *    is associated with the key after the set call.  Note that while there are
 *    no guarantees, the cache implementation should attempt to cache the most
 *    recently set value for a given key.
 *
 * for this reason, because binning pass variants share const_state with
 * their draw-pass counterpart, both variants are serialized together.
 */

void
ir3_disk_cache_init(struct ir3_compiler *compiler)
{
   if (ir3_shader_debug & IR3_DBG_NOCACHE)
      return;

   const char *renderer = fd_dev_name(compiler->dev_id);
   const struct build_id_note *note =
      build_id_find_nhdr_for_addr(ir3_disk_cache_init);
   assert(note && build_id_length(note) == 20); /* sha1 */

   const uint8_t *id_sha1 = build_id_data(note);
   assert(id_sha1);

   char timestamp[41];
   _mesa_sha1_format(timestamp, id_sha1);

   uint64_t driver_flags = ir3_shader_debug;
   compiler->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);
}

void
ir3_disk_cache_init_shader_key(struct ir3_compiler *compiler,
                               struct ir3_shader *shader)
{
   if (!compiler->disk_cache)
      return;

   struct mesa_sha1 ctx;

   _mesa_sha1_init(&ctx);

   /* Serialize the NIR to a binary blob that we can hash for the disk
    * cache.  Drop unnecessary information (like variable names)
    * so the serialized NIR is smaller, and also to let us detect more
    * isomorphic shaders when hashing, increasing cache hits.
    */
   struct blob blob;
   blob_init(&blob);
   nir_serialize(&blob, shader->nir, true);
   _mesa_sha1_update(&ctx, blob.data, blob.size);
   blob_finish(&blob);

   _mesa_sha1_update(&ctx, &shader->options.api_wavesize,
                     sizeof(shader->options.api_wavesize));
   _mesa_sha1_update(&ctx, &shader->options.real_wavesize,
                     sizeof(shader->options.real_wavesize));
   _mesa_sha1_update(&ctx, &shader->options.nir_options,
                     sizeof(shader->options.nir_options));

   /* Note that on some gens stream-out is lowered in ir3 to stg.  For later
    * gens we maybe don't need to include stream-out in the cache key.
    */
   _mesa_sha1_update(&ctx, &shader->stream_output,
                     sizeof(shader->stream_output));

   _mesa_sha1_final(&ctx, shader->cache_key);
}

static void
compute_variant_key(struct ir3_shader *shader, struct ir3_shader_variant *v,
                    cache_key cache_key)
{
   struct blob blob;
   blob_init(&blob);

   blob_write_bytes(&blob, &shader->cache_key, sizeof(shader->cache_key));
   blob_write_bytes(&blob, &v->key, sizeof(v->key));
   blob_write_uint8(&blob, v->binning_pass);

   disk_cache_compute_key(shader->compiler->disk_cache, blob.data, blob.size,
                          cache_key);

   blob_finish(&blob);
}

static void
retrieve_variant(struct blob_reader *blob, struct ir3_shader_variant *v)
{
   blob_copy_bytes(blob, VARIANT_CACHE_PTR(v), VARIANT_CACHE_SIZE);

   /*
    * pointers need special handling:
    */

   v->bin = rzalloc_size(v, v->info.size);
   blob_copy_bytes(blob, v->bin, v->info.size);

   if (!v->binning_pass) {
      blob_copy_bytes(blob, v->const_state, sizeof(*v->const_state));
      unsigned immeds_sz = v->const_state->immediates_size *
                           sizeof(v->const_state->immediates[0]);
      v->const_state->immediates = ralloc_size(v->const_state, immeds_sz);
      blob_copy_bytes(blob, v->const_state->immediates, immeds_sz);
   }
}

static void
store_variant(struct blob *blob, const struct ir3_shader_variant *v)
{
   blob_write_bytes(blob, VARIANT_CACHE_PTR(v), VARIANT_CACHE_SIZE);

   /*
    * pointers need special handling:
    */

   blob_write_bytes(blob, v->bin, v->info.size);

   /* No saving constant_data, it's already baked into bin at this point. */

   if (!v->binning_pass) {
      blob_write_bytes(blob, v->const_state, sizeof(*v->const_state));
      unsigned immeds_sz = v->const_state->immediates_size *
                           sizeof(v->const_state->immediates[0]);
      blob_write_bytes(blob, v->const_state->immediates, immeds_sz);
   }
}

struct ir3_shader_variant *
ir3_retrieve_variant(struct blob_reader *blob, struct ir3_compiler *compiler,
                     void *mem_ctx)
{
   struct ir3_shader_variant *v = rzalloc_size(mem_ctx, sizeof(*v));

   v->id = 0;
   v->compiler = compiler;
   v->binning_pass = false;
   v->nonbinning = NULL;
   v->binning = NULL;
   blob_copy_bytes(blob, &v->key, sizeof(v->key));
   v->type = blob_read_uint32(blob);
   v->mergedregs = blob_read_uint32(blob);
   v->const_state = rzalloc_size(v, sizeof(*v->const_state));

   retrieve_variant(blob, v);

   if (v->type == MESA_SHADER_VERTEX && ir3_has_binning_vs(&v->key)) {
      v->binning = rzalloc_size(v, sizeof(*v->binning));
      v->binning->id = 0;
      v->binning->compiler = compiler;
      v->binning->binning_pass = true;
      v->binning->nonbinning = v;
      v->binning->key = v->key;
      v->binning->type = MESA_SHADER_VERTEX;
      v->binning->mergedregs = v->mergedregs;
      v->binning->const_state = v->const_state;

      retrieve_variant(blob, v->binning);
   }
   
   return v;
}

void
ir3_store_variant(struct blob *blob, const struct ir3_shader_variant *v)
{
   blob_write_bytes(blob, &v->key, sizeof(v->key));
   blob_write_uint32(blob, v->type);
   blob_write_uint32(blob, v->mergedregs);

   store_variant(blob, v);

   if (v->type == MESA_SHADER_VERTEX && ir3_has_binning_vs(&v->key)) {
      store_variant(blob, v->binning);
   }
}

bool
ir3_disk_cache_retrieve(struct ir3_shader *shader,
                        struct ir3_shader_variant *v)
{
   if (!shader->compiler->disk_cache)
      return false;

   cache_key cache_key;

   compute_variant_key(shader, v, cache_key);

   if (debug) {
      char sha1[41];
      _mesa_sha1_format(sha1, cache_key);
      fprintf(stderr, "[mesa disk cache] retrieving variant %s: ", sha1);
   }

   size_t size;
   void *buffer = disk_cache_get(shader->compiler->disk_cache, cache_key, &size);

   if (debug)
      fprintf(stderr, "%s\n", buffer ? "found" : "missing");

   if (!buffer)
      return false;

   struct blob_reader blob;
   blob_reader_init(&blob, buffer, size);

   retrieve_variant(&blob, v);

   if (v->binning)
      retrieve_variant(&blob, v->binning);

   free(buffer);

   return true;
}

void
ir3_disk_cache_store(struct ir3_shader *shader,
                     struct ir3_shader_variant *v)
{
   if (!shader->compiler->disk_cache)
      return;

   cache_key cache_key;

   compute_variant_key(shader, v, cache_key);

   if (debug) {
      char sha1[41];
      _mesa_sha1_format(sha1, cache_key);
      fprintf(stderr, "[mesa disk cache] storing variant %s\n", sha1);
   }

   struct blob blob;
   blob_init(&blob);

   store_variant(&blob, v);

   if (v->binning)
      store_variant(&blob, v->binning);

   disk_cache_put(shader->compiler->disk_cache, cache_key, blob.data, blob.size, NULL);
   blob_finish(&blob);
}
