/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "si_utrace.h"
#include "si_perfetto.h"
#include "amd/common/ac_gpu_info.h"

#include "util/u_trace_gallium.h"
#include "util/hash_table.h"


static void si_utrace_record_ts(struct u_trace *trace, void *cs, void *timestamps,
                                uint64_t offset_B, uint32_t flags)
{
   struct si_context *ctx = container_of(trace, struct si_context, trace);
   struct pipe_resource *buffer = timestamps;
   struct si_resource *ts_bo = si_resource(buffer);

   if (ctx->gfx_cs.current.buf == ctx->last_timestamp_cmd &&
       ctx->gfx_cs.current.cdw == ctx->last_timestamp_cmd_cdw) {
      uint64_t *ts = si_buffer_map(ctx, ts_bo, PIPE_MAP_READ) + offset_B;
      *ts = U_TRACE_NO_TIMESTAMP;
      return;
   }

   si_emit_ts(ctx, ts_bo, offset_B);
   ctx->last_timestamp_cmd = ctx->gfx_cs.current.buf;
   ctx->last_timestamp_cmd_cdw = ctx->gfx_cs.current.cdw;
}

static uint64_t si_utrace_read_ts(struct u_trace_context *utctx, void *timestamps,
                                  uint64_t offset_B, void *flush_data)
{
   struct si_context *ctx = container_of(utctx, struct si_context, ds.trace_context);
   struct pipe_resource *buffer = timestamps;

   uint64_t *ts = si_buffer_map(ctx, si_resource(buffer), PIPE_MAP_READ) + offset_B;

   /* Don't translate the no-timestamp marker: */
   if (*ts == U_TRACE_NO_TIMESTAMP)
      return U_TRACE_NO_TIMESTAMP;

   return (1000000 * *ts) / ctx->screen->info.clock_crystal_freq;
}

static void si_utrace_delete_flush_data(struct u_trace_context *utctx, void *flush_data)
{
   free(flush_data);
}

void si_utrace_init(struct si_context *sctx)
{
   char buf[64];
   snprintf(buf, sizeof(buf), "%u:%u:%u:%u:%u", sctx->screen->info.pci.domain,
            sctx->screen->info.pci.bus, sctx->screen->info.pci.dev,
            sctx->screen->info.pci.func, sctx->screen->info.pci_id);
   uint32_t gpu_id = _mesa_hash_string(buf);

   si_ds_device_init(&sctx->ds, &sctx->screen->info, gpu_id, AMD_DS_API_OPENGL);
   u_trace_pipe_context_init(&sctx->ds.trace_context, &sctx->b,
                             sizeof(uint64_t), 0, si_utrace_record_ts,
                             si_utrace_read_ts, NULL, NULL,
                             si_utrace_delete_flush_data);

   si_ds_device_init_queue(&sctx->ds, &sctx->ds_queue, "%s", "render");
}

void si_utrace_fini(struct si_context *sctx)
{
   si_ds_device_fini(&sctx->ds);
}

void si_utrace_flush(struct si_context *sctx, uint64_t submission_id)
{
   struct si_ds_flush_data *flush_data = malloc(sizeof(*flush_data));
   si_ds_flush_data_init(flush_data, &sctx->ds_queue, submission_id);
   u_trace_flush(&sctx->trace, flush_data, U_TRACE_FRAME_UNKNOWN, false);
}
