/*
 * Copyright (C) 2019 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include "pan_texture.h"

/* Translate a PIPE swizzle quad to a 12-bit Mali swizzle code. PIPE
 * swizzles line up with Mali swizzles for the XYZW01, but PIPE swizzles have
 * an additional "NONE" field that we have to mask out to zero. Additionally,
 * PIPE swizzles are sparse but Mali swizzles are packed */

unsigned
panfrost_translate_swizzle_4(const unsigned char swizzle[4])
{
   unsigned out = 0;

   for (unsigned i = 0; i < 4; ++i) {
      assert(swizzle[i] <= PIPE_SWIZZLE_1);
      out |= (swizzle[i] << (3 * i));
   }

   return out;
}

void
panfrost_invert_swizzle(const unsigned char *in, unsigned char *out)
{
   /* First, default to all zeroes, both to prevent uninitialized junk
      and to provide a known baseline so we can tell when components
      have been modified
    */

   for (unsigned c = 0; c < 4; ++c)
      out[c] = PIPE_SWIZZLE_0;

   /* Now "do" what the swizzle says */

   for (unsigned c = 0; c < 4; ++c) {
      unsigned char i = in[c];

      /* Who cares? */
      assert(PIPE_SWIZZLE_X == 0);
      if (i > PIPE_SWIZZLE_W)
         continue;

      /* Invert (only if we haven't already applied) */
      unsigned idx = i - PIPE_SWIZZLE_X;
      if (out[idx] == PIPE_SWIZZLE_0)
         out[idx] = PIPE_SWIZZLE_X + c;
   }
}
