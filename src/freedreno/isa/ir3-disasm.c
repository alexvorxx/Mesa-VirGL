/*
 * Copyright © 2020 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util/os_file.h"

#include "freedreno_dev_info.h"
#include "ir3-isa.h"

static void
disasm_instr_cb(void *d, unsigned n, void *instr)
{
   uint32_t *dwords = (uint32_t *)instr;
   printf("%3d[%08x_%08x] ", n, dwords[1], dwords[0]);
}

static void
usage(const char *prog)
{
   fprintf(stderr,
           "Usage: %s [-g GPU_ID | -c CHIP_ID] [-x HEX | FILE]\n"
           " -g GPU_ID: specify GPU ID\n"
           " -c CHIP_ID: specify GPU chip ID in hex\n"
           " -x HEX: disassemble instruction encoded as HEX\n",
           prog);
}

int
main(int argc, char **argv)
{
   size_t sz;
   void *raw = NULL;
   uint64_t raw_hex;
   const struct fd_dev_info *info = NULL;
   int opt;

   while ((opt = getopt(argc, argv, "g:c:x:")) != -1) {
      switch (opt) {
      case 'g':
         info = fd_dev_info_raw_by_name(optarg);
         if (!info) {
            fprintf(stderr, "Unknown GPU name: %s\n", optarg);
            usage(argv[0]);
            return EXIT_FAILURE;
         }
         break;
      case 'c': {
         uint64_t chip_id;
         if (sscanf(optarg, "%" PRIx64, &chip_id) != 1) {
            fprintf(stderr, "Invalid chip ID: %s\n", optarg);
            usage(argv[0]);
            return EXIT_FAILURE;
         }

         struct fd_dev_id id = {.chip_id = chip_id};
         info = fd_dev_info_raw(&id);
         if (!info) {
            fprintf(stderr, "Unknown chip ID: %s\n", optarg);
            usage(argv[0]);
            return EXIT_FAILURE;
         }
         break;
      }
      case 'x':
         if (sscanf(optarg, "%" PRIx64, &raw_hex) != 1) {
            fprintf(stderr, "Invalid hex number: %s\n", optarg);
            usage(argv[0]);
            return EXIT_FAILURE;
         }

         raw = &raw_hex;
         sz = sizeof(raw_hex);
         break;
      default:
         usage(argv[0]);
         return EXIT_FAILURE;
      }
   }

   if (!raw) {
      if (optind >= argc) {
         fprintf(stderr, "No file specified\n");
         usage(argv[0]);
         return EXIT_FAILURE;
      }

      raw = os_read_file(argv[optind], &sz);
   }

   unsigned chip = info ? info->chip : 7;

   ir3_isa_disasm(raw, sz, stdout,
                  &(struct isa_decode_options){
                     .show_errors = true,
                     .branch_labels = true,
                     .pre_instr_cb = disasm_instr_cb,
                     .gpu_id = chip * 100,
                  });

   return 0;
}
