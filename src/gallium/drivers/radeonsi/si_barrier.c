/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_build_pm4.h"

static struct si_resource *si_get_wait_mem_scratch_bo(struct si_context *ctx,
                                                      struct radeon_cmdbuf *cs, bool is_secure)
{
   struct si_screen *sscreen = ctx->screen;

   assert(ctx->gfx_level < GFX11);

   if (likely(!is_secure)) {
      return ctx->wait_mem_scratch;
   } else {
      assert(sscreen->info.has_tmz_support);
      if (!ctx->wait_mem_scratch_tmz) {
         ctx->wait_mem_scratch_tmz =
            si_aligned_buffer_create(&sscreen->b,
                                     PIPE_RESOURCE_FLAG_UNMAPPABLE |
                                     SI_RESOURCE_FLAG_DRIVER_INTERNAL |
                                     PIPE_RESOURCE_FLAG_ENCRYPTED,
                                     PIPE_USAGE_DEFAULT, 4,
                                     sscreen->info.tcc_cache_line_size);
         si_cp_write_data(ctx, ctx->wait_mem_scratch_tmz, 0, 4, V_370_MEM, V_370_ME,
                          &ctx->wait_mem_number);
      }

      return ctx->wait_mem_scratch_tmz;
   }
}

static void prepare_cb_db_flushes(struct si_context *ctx, unsigned *flags)
{
   /* Don't flush CB and DB if there have been no draw calls. */
   if (ctx->num_draw_calls == ctx->last_cb_flush_num_draw_calls &&
       ctx->num_decompress_calls == ctx->last_cb_flush_num_decompress_calls)
      *flags &= ~SI_CONTEXT_FLUSH_AND_INV_CB;

   if (ctx->num_draw_calls == ctx->last_db_flush_num_draw_calls &&
       ctx->num_decompress_calls == ctx->last_db_flush_num_decompress_calls)
      *flags &= ~SI_CONTEXT_FLUSH_AND_INV_DB;

   /* Track the last flush. */
   if (*flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
      ctx->num_cb_cache_flushes++;
      ctx->last_cb_flush_num_draw_calls = ctx->num_draw_calls;
      ctx->last_cb_flush_num_decompress_calls = ctx->num_decompress_calls;
   }
   if (*flags & SI_CONTEXT_FLUSH_AND_INV_DB) {
      ctx->num_db_cache_flushes++;
      ctx->last_db_flush_num_draw_calls = ctx->num_draw_calls;
      ctx->last_db_flush_num_decompress_calls = ctx->num_decompress_calls;
   }
}

static void gfx10_emit_barrier(struct si_context *ctx, struct radeon_cmdbuf *cs)
{
   uint32_t gcr_cntl = 0;
   unsigned cb_db_event = 0;
   unsigned flags = ctx->flags;

   if (!flags)
      return;

   if (!ctx->has_graphics) {
      /* Only process compute flags. */
      flags &= SI_CONTEXT_INV_ICACHE | SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE |
               SI_CONTEXT_INV_L2 | SI_CONTEXT_WB_L2 | SI_CONTEXT_INV_L2_METADATA |
               SI_CONTEXT_CS_PARTIAL_FLUSH;
   }

   /* We don't need these. */
   assert(!(flags & SI_CONTEXT_FLUSH_AND_INV_DB_META));

   prepare_cb_db_flushes(ctx, &flags);

   radeon_begin(cs);

   if (flags & SI_CONTEXT_VGT_FLUSH)
      radeon_event_write(V_028A90_VGT_FLUSH);

   if (flags & SI_CONTEXT_INV_ICACHE)
      gcr_cntl |= S_586_GLI_INV(V_586_GLI_ALL);
   if (flags & SI_CONTEXT_INV_SCACHE) {
      /* TODO: When writing to the SMEM L1 cache, we need to set SEQ
       * to FORWARD when both L1 and L2 are written out (WB or INV).
       */
      gcr_cntl |= S_586_GL1_INV(1) | S_586_GLK_INV(1);
   }
   if (flags & SI_CONTEXT_INV_VCACHE)
      gcr_cntl |= S_586_GL1_INV(1) | S_586_GLV_INV(1);

   /* The L2 cache ops are:
    * - INV: - invalidate lines that reflect memory (were loaded from memory)
    *        - don't touch lines that were overwritten (were stored by gfx clients)
    * - WB: - don't touch lines that reflect memory
    *       - write back lines that were overwritten
    * - WB | INV: - invalidate lines that reflect memory
    *             - write back lines that were overwritten
    *
    * GLM doesn't support WB alone. If WB is set, INV must be set too.
    */
   if (flags & SI_CONTEXT_INV_L2) {
      /* Writeback and invalidate everything in L2. */
      gcr_cntl |= S_586_GL2_INV(1) | S_586_GL2_WB(1) |
                  (ctx->gfx_level < GFX12 ? S_586_GLM_INV(1) | S_586_GLM_WB(1) : 0);
      ctx->num_L2_invalidates++;
   } else if (flags & SI_CONTEXT_WB_L2) {
      gcr_cntl |= S_586_GL2_WB(1) |
                  (ctx->gfx_level < GFX12 ? S_586_GLM_WB(1) | S_586_GLM_INV(1) : 0);
   } else if (flags & SI_CONTEXT_INV_L2_METADATA) {
      assert(ctx->gfx_level < GFX12);
      gcr_cntl |= S_586_GLM_INV(1) | S_586_GLM_WB(1);
   }

   if (flags & (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB)) {
      /* Flush CMASK/FMASK/DCC. Will wait for idle later. */
      if (ctx->gfx_level < GFX12 && flags & SI_CONTEXT_FLUSH_AND_INV_CB)
         radeon_event_write(V_028A90_FLUSH_AND_INV_CB_META);

      /* Gfx11 can't flush DB_META and should use a TS event instead. */
      /* Flush HTILE. Will wait for idle later. */
      if (ctx->gfx_level < GFX12 && ctx->gfx_level != GFX11 &&
          flags & SI_CONTEXT_FLUSH_AND_INV_DB)
         radeon_event_write(V_028A90_FLUSH_AND_INV_DB_META);

      /* First flush CB/DB, then L1/L2. */
      gcr_cntl |= S_586_SEQ(V_586_SEQ_FORWARD);

      if ((flags & (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB)) ==
          (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB)) {
         cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
      } else if (flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
         cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
      } else if (flags & SI_CONTEXT_FLUSH_AND_INV_DB) {
         if (ctx->gfx_level == GFX11)
            cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
         else
            cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
      } else {
         assert(0);
      }
   } else {
      /* Wait for graphics shaders to go idle if requested. */
      if (flags & SI_CONTEXT_PS_PARTIAL_FLUSH) {
         radeon_event_write(V_028A90_PS_PARTIAL_FLUSH);
         /* Only count explicit shader flushes, not implicit ones. */
         ctx->num_vs_flushes++;
         ctx->num_ps_flushes++;
      } else if (flags & SI_CONTEXT_VS_PARTIAL_FLUSH) {
         radeon_event_write(V_028A90_VS_PARTIAL_FLUSH);
         ctx->num_vs_flushes++;
      }
   }

   if (flags & SI_CONTEXT_CS_PARTIAL_FLUSH && ctx->compute_is_busy) {
      radeon_event_write(V_028A90_CS_PARTIAL_FLUSH);
      ctx->num_cs_flushes++;
      ctx->compute_is_busy = false;
   }
   radeon_end();

   if (cb_db_event) {
      if (ctx->gfx_level >= GFX11) {
         si_cp_release_mem_pws(ctx, cs, cb_db_event, gcr_cntl & C_586_GLI_INV);

         /* Wait for the event and invalidate remaining caches if needed. */
         si_cp_acquire_mem_pws(ctx, cs, cb_db_event,
                               flags & SI_CONTEXT_PFP_SYNC_ME ? V_580_CP_PFP : V_580_CP_ME,
                               gcr_cntl & ~C_586_GLI_INV, /* keep only GLI_INV */
                               0, flags);

         gcr_cntl = 0; /* all done */
         /* ACQUIRE_MEM in PFP is implemented as ACQUIRE_MEM in ME + PFP_SYNC_ME. */
         flags &= ~SI_CONTEXT_PFP_SYNC_ME;
      } else {
         /* GFX10 */
         struct si_resource *wait_mem_scratch =
           si_get_wait_mem_scratch_bo(ctx, cs, ctx->ws->cs_is_secure(cs));

         /* CB/DB flush and invalidate via RELEASE_MEM.
          * Combine this with other cache flushes when possible.
          */
         uint64_t va = wait_mem_scratch->gpu_address;
         ctx->wait_mem_number++;

         /* Get GCR_CNTL fields, because the encoding is different in RELEASE_MEM. */
         unsigned glm_wb = G_586_GLM_WB(gcr_cntl);
         unsigned glm_inv = G_586_GLM_INV(gcr_cntl);
         unsigned glv_inv = G_586_GLV_INV(gcr_cntl);
         unsigned gl1_inv = G_586_GL1_INV(gcr_cntl);
         assert(G_586_GL2_US(gcr_cntl) == 0);
         assert(G_586_GL2_RANGE(gcr_cntl) == 0);
         assert(G_586_GL2_DISCARD(gcr_cntl) == 0);
         unsigned gl2_inv = G_586_GL2_INV(gcr_cntl);
         unsigned gl2_wb = G_586_GL2_WB(gcr_cntl);
         unsigned gcr_seq = G_586_SEQ(gcr_cntl);

         gcr_cntl &= C_586_GLM_WB & C_586_GLM_INV & C_586_GLV_INV & C_586_GL1_INV & C_586_GL2_INV &
                     C_586_GL2_WB; /* keep SEQ */

         si_cp_release_mem(ctx, cs, cb_db_event,
                           S_490_GLM_WB(glm_wb) | S_490_GLM_INV(glm_inv) | S_490_GLV_INV(glv_inv) |
                           S_490_GL1_INV(gl1_inv) | S_490_GL2_INV(gl2_inv) | S_490_GL2_WB(gl2_wb) |
                           S_490_SEQ(gcr_seq),
                           EOP_DST_SEL_MEM, EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM,
                           EOP_DATA_SEL_VALUE_32BIT, wait_mem_scratch, va, ctx->wait_mem_number,
                           SI_NOT_QUERY);

         if (unlikely(ctx->sqtt_enabled)) {
            si_sqtt_describe_barrier_start(ctx, &ctx->gfx_cs);
         }

         si_cp_wait_mem(ctx, cs, va, ctx->wait_mem_number, 0xffffffff, WAIT_REG_MEM_EQUAL);

         if (unlikely(ctx->sqtt_enabled)) {
            si_sqtt_describe_barrier_end(ctx, &ctx->gfx_cs, flags);
         }
      }
   }

   /* Ignore fields that only modify the behavior of other fields. */
   if (gcr_cntl & C_586_GL1_RANGE & C_586_GL2_RANGE & C_586_SEQ) {
      si_cp_acquire_mem(ctx, cs, gcr_cntl,
                        flags & SI_CONTEXT_PFP_SYNC_ME ? V_580_CP_PFP : V_580_CP_ME);
   } else if (flags & SI_CONTEXT_PFP_SYNC_ME) {
      si_cp_pfp_sync_me(cs);
   }

   radeon_begin_again(cs);
   if (flags & SI_CONTEXT_START_PIPELINE_STATS && ctx->pipeline_stats_enabled != 1) {
      radeon_event_write(V_028A90_PIPELINESTAT_START);
      ctx->pipeline_stats_enabled = 1;
   } else if (flags & SI_CONTEXT_STOP_PIPELINE_STATS && ctx->pipeline_stats_enabled != 0) {
      radeon_event_write(V_028A90_PIPELINESTAT_STOP);
      ctx->pipeline_stats_enabled = 0;
   }
   radeon_end();

   ctx->flags = 0;
}

static void gfx6_emit_barrier(struct si_context *sctx, struct radeon_cmdbuf *cs)
{
   uint32_t flags = sctx->flags;

   if (!flags)
      return;

   if (!sctx->has_graphics) {
      /* Only process compute flags. */
      flags &= SI_CONTEXT_INV_ICACHE | SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE |
               SI_CONTEXT_INV_L2 | SI_CONTEXT_WB_L2 | SI_CONTEXT_INV_L2_METADATA |
               SI_CONTEXT_CS_PARTIAL_FLUSH;
   }

   uint32_t cp_coher_cntl = 0;
   const uint32_t flush_cb_db = flags & (SI_CONTEXT_FLUSH_AND_INV_CB | SI_CONTEXT_FLUSH_AND_INV_DB);

   assert(sctx->gfx_level <= GFX9);

   prepare_cb_db_flushes(sctx, &flags);

   /* GFX6 has a bug that it always flushes ICACHE and KCACHE if either
    * bit is set. An alternative way is to write SQC_CACHES, but that
    * doesn't seem to work reliably. Since the bug doesn't affect
    * correctness (it only does more work than necessary) and
    * the performance impact is likely negligible, there is no plan
    * to add a workaround for it.
    */

   if (flags & SI_CONTEXT_INV_ICACHE)
      cp_coher_cntl |= S_0085F0_SH_ICACHE_ACTION_ENA(1);
   if (flags & SI_CONTEXT_INV_SCACHE)
      cp_coher_cntl |= S_0085F0_SH_KCACHE_ACTION_ENA(1);

   if (sctx->gfx_level <= GFX8) {
      if (flags & SI_CONTEXT_FLUSH_AND_INV_CB) {
         cp_coher_cntl |= S_0085F0_CB_ACTION_ENA(1) | S_0085F0_CB0_DEST_BASE_ENA(1) |
                          S_0085F0_CB1_DEST_BASE_ENA(1) | S_0085F0_CB2_DEST_BASE_ENA(1) |
                          S_0085F0_CB3_DEST_BASE_ENA(1) | S_0085F0_CB4_DEST_BASE_ENA(1) |
                          S_0085F0_CB5_DEST_BASE_ENA(1) | S_0085F0_CB6_DEST_BASE_ENA(1) |
                          S_0085F0_CB7_DEST_BASE_ENA(1);

         /* Necessary for DCC */
         if (sctx->gfx_level == GFX8)
            si_cp_release_mem(sctx, cs, V_028A90_FLUSH_AND_INV_CB_DATA_TS, 0, EOP_DST_SEL_MEM,
                              EOP_INT_SEL_NONE, EOP_DATA_SEL_DISCARD, NULL, 0, 0, SI_NOT_QUERY);
      }
      if (flags & SI_CONTEXT_FLUSH_AND_INV_DB)
         cp_coher_cntl |= S_0085F0_DB_ACTION_ENA(1) | S_0085F0_DB_DEST_BASE_ENA(1);
   }

   radeon_begin(cs);

   /* Flush CMASK/FMASK/DCC. SURFACE_SYNC will wait for idle. */
   if (flags & SI_CONTEXT_FLUSH_AND_INV_CB)
      radeon_event_write(V_028A90_FLUSH_AND_INV_CB_META);

   /* Flush HTILE. SURFACE_SYNC will wait for idle. */
   if (flags & (SI_CONTEXT_FLUSH_AND_INV_DB | SI_CONTEXT_FLUSH_AND_INV_DB_META))
      radeon_event_write(V_028A90_FLUSH_AND_INV_DB_META);

   /* Wait for shader engines to go idle.
    * VS and PS waits are unnecessary if SURFACE_SYNC is going to wait
    * for everything including CB/DB cache flushes.
    *
    * GFX6-8: SURFACE_SYNC with CB_ACTION_ENA doesn't do anything if there are no CB/DB bindings.
    * Reproducible with: piglit/arb_framebuffer_no_attachments-atomic
    *
    * GFX9: The TS event is always written after full pipeline completion regardless of CB/DB
    * bindings.
    */
   if (sctx->gfx_level <= GFX8 || !flush_cb_db) {
      if (flags & SI_CONTEXT_PS_PARTIAL_FLUSH) {
         radeon_event_write(V_028A90_PS_PARTIAL_FLUSH);
         /* Only count explicit shader flushes, not implicit ones done by SURFACE_SYNC. */
         sctx->num_vs_flushes++;
         sctx->num_ps_flushes++;
      } else if (flags & SI_CONTEXT_VS_PARTIAL_FLUSH) {
         radeon_event_write(V_028A90_VS_PARTIAL_FLUSH);
         sctx->num_vs_flushes++;
      }
   }

   if (flags & SI_CONTEXT_CS_PARTIAL_FLUSH && sctx->compute_is_busy) {
      radeon_event_write(V_028A90_CS_PARTIAL_FLUSH);
      sctx->num_cs_flushes++;
      sctx->compute_is_busy = false;
   }

   /* VGT state synchronization. */
   if (flags & SI_CONTEXT_VGT_FLUSH)
      radeon_event_write(V_028A90_VGT_FLUSH);

   radeon_end();

   /* GFX9: Wait for idle if we're flushing CB or DB. ACQUIRE_MEM doesn't
    * wait for idle on GFX9. We have to use a TS event.
    */
   if (sctx->gfx_level == GFX9 && flush_cb_db) {
      uint64_t va;
      unsigned tc_flags, cb_db_event;

      /* Set the CB/DB flush event. */
      switch (flush_cb_db) {
      case SI_CONTEXT_FLUSH_AND_INV_CB:
         cb_db_event = V_028A90_FLUSH_AND_INV_CB_DATA_TS;
         break;
      case SI_CONTEXT_FLUSH_AND_INV_DB:
         cb_db_event = V_028A90_FLUSH_AND_INV_DB_DATA_TS;
         break;
      default:
         /* both CB & DB */
         cb_db_event = V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT;
      }

      /* These are the only allowed combinations. If you need to
       * do multiple operations at once, do them separately.
       * All operations that invalidate L2 also seem to invalidate
       * metadata. Volatile (VOL) and WC flushes are not listed here.
       *
       * TC    | TC_WB         = writeback & invalidate L2
       * TC    | TC_WB | TC_NC = writeback & invalidate L2 for MTYPE == NC
       *         TC_WB | TC_NC = writeback L2 for MTYPE == NC
       * TC            | TC_NC = invalidate L2 for MTYPE == NC
       * TC    | TC_MD         = writeback & invalidate L2 metadata (DCC, etc.)
       * TCL1                  = invalidate L1
       */
      tc_flags = 0;

      if (flags & SI_CONTEXT_INV_L2_METADATA) {
         tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_MD_ACTION_ENA;
      }

      /* Ideally flush TC together with CB/DB. */
      if (flags & SI_CONTEXT_INV_L2) {
         /* Writeback and invalidate everything in L2 & L1. */
         tc_flags = EVENT_TC_ACTION_ENA | EVENT_TC_WB_ACTION_ENA;

         /* Clear the flags. */
         flags &= ~(SI_CONTEXT_INV_L2 | SI_CONTEXT_WB_L2);
         sctx->num_L2_invalidates++;
      }

      /* Do the flush (enqueue the event and wait for it). */
      struct si_resource* wait_mem_scratch =
        si_get_wait_mem_scratch_bo(sctx, cs, sctx->ws->cs_is_secure(cs));

      va = wait_mem_scratch->gpu_address;
      sctx->wait_mem_number++;

      si_cp_release_mem(sctx, cs, cb_db_event, tc_flags, EOP_DST_SEL_MEM,
                        EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM, EOP_DATA_SEL_VALUE_32BIT,
                        wait_mem_scratch, va, sctx->wait_mem_number, SI_NOT_QUERY);

      if (unlikely(sctx->sqtt_enabled)) {
         si_sqtt_describe_barrier_start(sctx, cs);
      }

      si_cp_wait_mem(sctx, cs, va, sctx->wait_mem_number, 0xffffffff, WAIT_REG_MEM_EQUAL);

      if (unlikely(sctx->sqtt_enabled)) {
         si_sqtt_describe_barrier_end(sctx, cs, sctx->flags);
      }
   }

   /* GFX6-GFX8 only: When one of the CP_COHER_CNTL.DEST_BASE flags is set, SURFACE_SYNC waits
    * for idle, so it should be last.
    *
    * cp_coher_cntl should contain everything except TC flags at this point.
    *
    * GFX6-GFX7 don't support L2 write-back.
    */
   unsigned engine = flags & SI_CONTEXT_PFP_SYNC_ME ? V_580_CP_PFP : V_580_CP_ME;

   if (flags & SI_CONTEXT_INV_L2 || (sctx->gfx_level <= GFX7 && flags & SI_CONTEXT_WB_L2)) {
      /* Invalidate L1 & L2. WB must be set on GFX8+ when TC_ACTION is set. */
      si_cp_acquire_mem(sctx, cs,
                        cp_coher_cntl | S_0085F0_TC_ACTION_ENA(1) | S_0085F0_TCL1_ACTION_ENA(1) |
                        S_0301F0_TC_WB_ACTION_ENA(sctx->gfx_level >= GFX8), engine);
      sctx->num_L2_invalidates++;
   } else {
      /* L1 invalidation and L2 writeback must be done separately, because both operations can't
       * be done together.
       */
      if (flags & SI_CONTEXT_WB_L2) {
         /* WB = write-back
          * NC = apply to non-coherent MTYPEs
          *      (i.e. MTYPE <= 1, which is what we use everywhere)
          *
          * WB doesn't work without NC.
          *
          * If we get here, the only flag that can't be executed together with WB_L2 is VMEM cache
          * invalidation.
          */
         bool last_acquire_mem = !(flags & SI_CONTEXT_INV_VCACHE);

         si_cp_acquire_mem(sctx, cs,
                           cp_coher_cntl | S_0301F0_TC_WB_ACTION_ENA(1) |
                           S_0301F0_TC_NC_ACTION_ENA(1),
                           /* If this is not the last ACQUIRE_MEM, flush in ME.
                            * We only want to synchronize with PFP in the last ACQUIRE_MEM. */
                           last_acquire_mem ? engine : V_580_CP_ME);

         if (last_acquire_mem)
            flags &= ~SI_CONTEXT_PFP_SYNC_ME;
         cp_coher_cntl = 0;
         sctx->num_L2_writebacks++;
      }

      if (flags & SI_CONTEXT_INV_VCACHE)
         cp_coher_cntl |= S_0085F0_TCL1_ACTION_ENA(1);

      /* If there are still some cache flags left... */
      if (cp_coher_cntl) {
         si_cp_acquire_mem(sctx, cs, cp_coher_cntl, engine);
         flags &= ~SI_CONTEXT_PFP_SYNC_ME;
      }

      /* This might be needed even without any cache flags, such as when doing buffer stores
       * to an index buffer.
       */
      if (flags & SI_CONTEXT_PFP_SYNC_ME)
         si_cp_pfp_sync_me(cs);
   }

   if (flags & SI_CONTEXT_START_PIPELINE_STATS && sctx->pipeline_stats_enabled != 1) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_PIPELINESTAT_START);
      radeon_end();
      sctx->pipeline_stats_enabled = 1;
   } else if (flags & SI_CONTEXT_STOP_PIPELINE_STATS && sctx->pipeline_stats_enabled != 0) {
      radeon_begin(cs);
      radeon_event_write(V_028A90_PIPELINESTAT_STOP);
      radeon_end();
      sctx->pipeline_stats_enabled = 0;
   }

   sctx->flags = 0;
}

static void si_emit_barrier_as_atom(struct si_context *sctx, unsigned index)
{
   sctx->emit_barrier(sctx, &sctx->gfx_cs);
}

static bool si_is_buffer_idle(struct si_context *sctx, struct si_resource *buf,
                              unsigned usage)
{
   return !si_cs_is_buffer_referenced(sctx, buf->buf, usage) &&
          sctx->ws->buffer_wait(sctx->ws, buf->buf, 0, usage);
}

void si_barrier_before_internal_op(struct si_context *sctx, unsigned flags,
                                   unsigned num_buffers,
                                   const struct pipe_shader_buffer *buffers,
                                   unsigned writable_buffers_mask,
                                   unsigned num_images,
                                   const struct pipe_image_view *images)
{
   for (unsigned i = 0; i < num_images; i++) {
      /* The driver doesn't decompress resources automatically for internal blits, so do it manually. */
      si_decompress_subresource(&sctx->b, images[i].resource, PIPE_MASK_RGBAZS,
                                images[i].u.tex.level, images[i].u.tex.first_layer,
                                images[i].u.tex.last_layer,
                                images[i].access & PIPE_IMAGE_ACCESS_WRITE);
   }

   /* Don't sync if buffers are idle. */
   const unsigned ps_mask = SI_BIND_CONSTANT_BUFFER(PIPE_SHADER_FRAGMENT) |
                            SI_BIND_SHADER_BUFFER(PIPE_SHADER_FRAGMENT) |
                            SI_BIND_IMAGE_BUFFER(PIPE_SHADER_FRAGMENT) |
                            SI_BIND_SAMPLER_BUFFER(PIPE_SHADER_FRAGMENT);
   const unsigned cs_mask = SI_BIND_CONSTANT_BUFFER(PIPE_SHADER_COMPUTE) |
                            SI_BIND_SHADER_BUFFER(PIPE_SHADER_COMPUTE) |
                            SI_BIND_IMAGE_BUFFER(PIPE_SHADER_COMPUTE) |
                            SI_BIND_SAMPLER_BUFFER(PIPE_SHADER_COMPUTE);

   for (unsigned i = 0; i < num_buffers; i++) {
      struct si_resource *buf = si_resource(buffers[i].buffer);

      if (!buf)
         continue;

      /* We always wait for the last write. If the buffer is used for write, also wait
       * for the last read.
       */
      if (!si_is_buffer_idle(sctx, buf, RADEON_USAGE_WRITE |
                             (writable_buffers_mask & BITFIELD_BIT(i) ? RADEON_USAGE_READ : 0))) {
         if (buf->bind_history & ps_mask)
            sctx->flags |= SI_CONTEXT_PS_PARTIAL_FLUSH;
         else
            sctx->flags |= SI_CONTEXT_VS_PARTIAL_FLUSH;

         if (buf->bind_history & cs_mask)
            sctx->flags |= SI_CONTEXT_CS_PARTIAL_FLUSH;
      }
   }

   /* Don't sync if images are idle. */
   for (unsigned i = 0; i < num_images; i++) {
      struct si_resource *img = si_resource(images[i].resource);
      bool writable = images[i].access & PIPE_IMAGE_ACCESS_WRITE;

      /* We always wait for the last write. If the buffer is used for write, also wait
       * for the last read.
       */
      if (!si_is_buffer_idle(sctx, img, RADEON_USAGE_WRITE | (writable ? RADEON_USAGE_READ : 0))) {
         si_make_CB_shader_coherent(sctx, images[i].resource->nr_samples, true,
               ((struct si_texture*)images[i].resource)->surface.u.gfx9.color.dcc.pipe_aligned);
         sctx->flags |= SI_CONTEXT_PS_PARTIAL_FLUSH | SI_CONTEXT_CS_PARTIAL_FLUSH;
      }
   }

   /* Invalidate the VMEM cache only. The SMEM cache isn't used by shader buffers. */
   sctx->flags |= SI_CONTEXT_INV_VCACHE;
   si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
}

void si_barrier_after_internal_op(struct si_context *sctx, unsigned flags,
                                  unsigned num_buffers,
                                  const struct pipe_shader_buffer *buffers,
                                  unsigned writable_buffers_mask,
                                  unsigned num_images,
                                  const struct pipe_image_view *images)
{
   sctx->flags |= SI_CONTEXT_CS_PARTIAL_FLUSH;

   if (num_images) {
      /* Make sure image stores are visible to CB, which doesn't use L2 on GFX6-8. */
      sctx->flags |= sctx->gfx_level <= GFX8 ? SI_CONTEXT_WB_L2 : 0;
      /* Make sure image stores are visible to all CUs. */
      sctx->flags |= SI_CONTEXT_INV_VCACHE;
   }

   /* Make sure buffer stores are visible to all CUs and also as index/indirect buffers. */
   if (num_buffers)
      sctx->flags |= SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE | SI_CONTEXT_PFP_SYNC_ME;

   /* We must set TC_L2_dirty for buffers because:
    * - GFX6,12: CP DMA doesn't use L2.
    * - GFX6-7,12: Index buffer reads don't use L2.
    * - GFX6-8,12: CP doesn't use L2.
    * - GFX6-8: CB/DB don't use L2.
    *
    * TC_L2_dirty is checked explicitly when buffers are used in those cases to enforce coherency.
    */
   while (writable_buffers_mask)
      si_resource(buffers[u_bit_scan(&writable_buffers_mask)].buffer)->TC_L2_dirty = true;

   /* Make sure RBs see our DCC image stores if RBs and TCCs (L2 instances) are non-coherent. */
   if (sctx->gfx_level >= GFX10 && sctx->screen->info.tcc_rb_non_coherent) {
      for (unsigned i = 0; i < num_images; i++) {
         if (vi_dcc_enabled((struct si_texture*)images[i].resource, images[i].u.tex.level) &&
             images[i].access & PIPE_IMAGE_ACCESS_WRITE &&
             (sctx->screen->always_allow_dcc_stores ||
              images[i].access & SI_IMAGE_ACCESS_ALLOW_DCC_STORE)) {
            sctx->flags |= SI_CONTEXT_INV_L2;
            break;
         }
      }
   }

   si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
}

static void si_set_dst_src_barrier_buffers(struct pipe_shader_buffer *buffers,
                                           struct pipe_resource *dst, struct pipe_resource *src)
{
   assert(dst);
   memset(buffers, 0, sizeof(buffers[0]) * 2);
   /* Only the "buffer" field is going to be used. */
   buffers[0].buffer = dst;
   buffers[1].buffer = src;
}

/* This is for simple buffer ops that have 1 dst and 0-1 src. */
void si_barrier_before_simple_buffer_op(struct si_context *sctx, unsigned flags,
                                        struct pipe_resource *dst, struct pipe_resource *src)
{
   struct pipe_shader_buffer barrier_buffers[2];
   si_set_dst_src_barrier_buffers(barrier_buffers, dst, src);
   si_barrier_before_internal_op(sctx, flags, src ? 2 : 1, barrier_buffers, 0x1, 0, NULL);
}

/* This is for simple buffer ops that have 1 dst and 0-1 src. */
void si_barrier_after_simple_buffer_op(struct si_context *sctx, unsigned flags,
                                       struct pipe_resource *dst, struct pipe_resource *src)
{
   struct pipe_shader_buffer barrier_buffers[2];
   si_set_dst_src_barrier_buffers(barrier_buffers, dst, src);
   si_barrier_after_internal_op(sctx, flags, src ? 2 : 1, barrier_buffers, 0x1, 0, NULL);
}

static void si_texture_barrier(struct pipe_context *ctx, unsigned flags)
{
   struct si_context *sctx = (struct si_context *)ctx;

   si_update_fb_dirtiness_after_rendering(sctx);

   /* Multisample surfaces are flushed in si_decompress_textures. */
   if (sctx->framebuffer.uncompressed_cb_mask) {
      si_make_CB_shader_coherent(sctx, sctx->framebuffer.nr_samples,
                                 sctx->framebuffer.CB_has_shader_readable_metadata,
                                 sctx->framebuffer.all_DCC_pipe_aligned);
   }
}

/* This only ensures coherency for shader image/buffer stores. */
static void si_memory_barrier(struct pipe_context *ctx, unsigned flags)
{
   struct si_context *sctx = (struct si_context *)ctx;

   if (!(flags & ~PIPE_BARRIER_UPDATE))
      return;

   /* Subsequent commands must wait for all shader invocations to
    * complete. */
   sctx->flags |= SI_CONTEXT_PS_PARTIAL_FLUSH | SI_CONTEXT_CS_PARTIAL_FLUSH |
                  SI_CONTEXT_PFP_SYNC_ME;

   if (flags & PIPE_BARRIER_CONSTANT_BUFFER)
      sctx->flags |= SI_CONTEXT_INV_SCACHE | SI_CONTEXT_INV_VCACHE;

   if (flags & (PIPE_BARRIER_VERTEX_BUFFER | PIPE_BARRIER_SHADER_BUFFER | PIPE_BARRIER_TEXTURE |
                PIPE_BARRIER_IMAGE | PIPE_BARRIER_STREAMOUT_BUFFER | PIPE_BARRIER_GLOBAL_BUFFER)) {
      /* As far as I can tell, L1 contents are written back to L2
       * automatically at end of shader, but the contents of other
       * L1 caches might still be stale. */
      sctx->flags |= SI_CONTEXT_INV_VCACHE;

      if (flags & (PIPE_BARRIER_IMAGE | PIPE_BARRIER_TEXTURE) &&
          sctx->screen->info.tcc_rb_non_coherent)
         sctx->flags |= SI_CONTEXT_INV_L2;
   }

   if (flags & PIPE_BARRIER_INDEX_BUFFER) {
      /* Indices are read through TC L2 since GFX8.
       * L1 isn't used.
       */
      if (sctx->screen->info.gfx_level <= GFX7)
         sctx->flags |= SI_CONTEXT_WB_L2;
   }

   /* MSAA color, any depth and any stencil are flushed in
    * si_decompress_textures when needed.
    */
   if (flags & PIPE_BARRIER_FRAMEBUFFER && sctx->framebuffer.uncompressed_cb_mask) {
      sctx->flags |= SI_CONTEXT_FLUSH_AND_INV_CB;

      if (sctx->gfx_level <= GFX8)
         sctx->flags |= SI_CONTEXT_WB_L2;
   }

   /* Indirect buffers use TC L2 on GFX9, but not older hw. */
   if (sctx->screen->info.gfx_level <= GFX8 && flags & PIPE_BARRIER_INDIRECT_BUFFER)
      sctx->flags |= SI_CONTEXT_WB_L2;

   /* Indices and draw indirect don't use GL2. */
   if (sctx->screen->info.cp_sdma_ge_use_system_memory_scope &&
       flags & (PIPE_BARRIER_INDEX_BUFFER | PIPE_BARRIER_INDIRECT_BUFFER))
      sctx->flags |= SI_CONTEXT_WB_L2;

   si_mark_atom_dirty(sctx, &sctx->atoms.s.barrier);
}

void si_init_barrier_functions(struct si_context *sctx)
{
   if (sctx->gfx_level >= GFX10)
      sctx->emit_barrier = gfx10_emit_barrier;
   else
      sctx->emit_barrier = gfx6_emit_barrier;

   sctx->atoms.s.barrier.emit = si_emit_barrier_as_atom;

   sctx->b.memory_barrier = si_memory_barrier;
   sctx->b.texture_barrier = si_texture_barrier;
}
