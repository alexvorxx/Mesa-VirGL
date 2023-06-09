/*
 * Copyright (c) 2012-2013 Etnaviv Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Common debug stuffl */
#ifndef H_ETNA_DEBUG
#define H_ETNA_DEBUG

#include "util/u_debug.h"
#include "util/log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Logging */
#define ETNA_DBG_MSGS            0x1 /* Warnings and non-fatal errors */
#define ETNA_DBG_FRAME_MSGS      0x2
#define ETNA_DBG_RESOURCE_MSGS   0x4
#define ETNA_DBG_COMPILER_MSGS   0x8
#define ETNA_DBG_LINKER_MSGS     0x10
#define ETNA_DBG_DUMP_SHADERS    0x20
#define ETNA_DRM_MSGS            0x40 /* Debug messages from DRM */
#define ETNA_DBG_PERF            0x80

/* Bypasses */
#define ETNA_DBG_NO_TS           0x1000   /* Disable TS */
#define ETNA_DBG_NO_AUTODISABLE  0x2000   /* Disable autodisable */
#define ETNA_DBG_NO_SUPERTILE    0x4000   /* Disable supertile */
#define ETNA_DBG_NO_EARLY_Z      0x8000   /* Disable early z */
#define ETNA_DBG_CFLUSH_ALL      0x10000  /* Flush before every state update + draw call */
#define ETNA_DBG_FINISH_ALL      0x20000  /* Finish on every flush */
#define ETNA_DBG_FLUSH_ALL       0x40000 /* Flush after every rendered primitive */
#define ETNA_DBG_ZERO            0x80000 /* Zero all resources after allocation */
#define ETNA_DBG_DRAW_STALL      0x100000 /* Stall FE/PE after every draw op */
#define ETNA_DBG_SHADERDB        0x200000 /* dump program compile information */
#define ETNA_DBG_NO_SINGLEBUF    0x400000 /* disable single buffer feature */
#define ETNA_DBG_DEQP            0x800000 /* Hacks to run dEQP GLES3 tests */
#define ETNA_DBG_NOCACHE         0x1000000 /* Disable shader cache */
#define ETNA_DBG_LINEAR_PE       0x2000000 /* Enable linear PE */
#define ETNA_DBG_MSAA            0x4000000 /* Enable MSAA */
#define ETNA_DBG_SHARED_TS       0x8000000 /* Enable TS sharing */

extern int etna_mesa_debug; /* set in etnaviv_screen.c from ETNA_MESA_DEBUG */

#define DBG_ENABLED(flag) unlikely(etna_mesa_debug & (flag))

#define DBG_F(flag, fmt, ...)                             \
   do {                                                   \
      if (DBG_ENABLED(flag))                              \
         mesa_logd("%s:%d: " fmt, __func__, __LINE__,     \
                   ##__VA_ARGS__);                        \
   } while (0)

#define DBG(fmt, ...)                                     \
   do {                                                   \
      if (DBG_ENABLED(ETNA_DBG_MSGS))                     \
         mesa_logd("%s:%d: " fmt, __func__, __LINE__,     \
                   ##__VA_ARGS__);                        \
   } while (0)

/* A serious bug, show this even in non-debug mode */
#define BUG(fmt, ...)                                                  \
   do {                                                                \
      mesa_loge("%s:%d: " fmt, __func__, __LINE__, ##__VA_ARGS__);     \
   } while (0)

#define perf_debug_message(debug, type, ...)                           \
   do {                                                                \
      if (DBG_ENABLED(ETNA_DBG_PERF))                                  \
         mesa_logw(__VA_ARGS__);                                       \
      struct util_debug_callback *__d = (debug);                       \
      if (__d)                                                         \
         util_debug_message(__d, type, __VA_ARGS__);                   \
   } while (0)

#define perf_debug_ctx(ctx, ...)                                                 \
   do {                                                                          \
      struct etna_context *__c = (ctx);                                          \
      perf_debug_message(__c ? &__c->base.debug : NULL, PERF_INFO, __VA_ARGS__); \
   } while (0)

#define perf_debug(...) perf_debug_ctx(NULL, PERF_INFO, __VA_ARGS__)

#endif
