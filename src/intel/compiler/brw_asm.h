/*
 * Copyright © 2018 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#ifndef BRW_ASM_H
#define BRW_ASM_H

#include <stdio.h>
#include <stdbool.h>

struct intel_device_info;

typedef struct {
   void *bin;
   int   bin_size;
   int   inst_count;
} brw_assemble_result;

typedef enum {
   BRW_ASSEMBLE_COMPACT = 1 << 0,
   BRW_ASSEMBLE_DUMP    = 1 << 1,
} brw_assemble_flags;

brw_assemble_result brw_assemble(
   void *mem_ctx, const struct intel_device_info *devinfo,
   FILE *f, const char *filename, brw_assemble_flags flags);

#endif /* BRW_ASM_H */
