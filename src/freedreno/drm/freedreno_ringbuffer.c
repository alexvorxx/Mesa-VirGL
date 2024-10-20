/*
 * Copyright © 2012-2018 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <assert.h>

#include "freedreno_drmif.h"
#include "freedreno_priv.h"
#include "freedreno_ringbuffer.h"

struct fd_submit *
fd_submit_new(struct fd_pipe *pipe)
{
   struct fd_submit *submit = pipe->funcs->submit_new(pipe);
   submit->refcnt = 1;
   submit->pipe = fd_pipe_ref(pipe);
   submit->dev = fd_device_ref(pipe->dev);
   return submit;
}

void
fd_submit_del(struct fd_submit *submit)
{
   if (!unref(&submit->refcnt))
      return;

   if (submit->primary)
      fd_ringbuffer_del(submit->primary);

   struct fd_pipe *pipe = submit->pipe;
   struct fd_device *dev = submit->dev;

   submit->funcs->destroy(submit);

   fd_pipe_del(pipe);
   fd_device_del(dev);
}

struct fd_submit *
fd_submit_ref(struct fd_submit *submit)
{
   ref(&submit->refcnt);
   return submit;
}

struct fd_fence *
fd_submit_flush(struct fd_submit *submit, int in_fence_fd, bool use_fence_fd)
{
   submit->fence = fd_pipe_emit_fence(submit->pipe, submit->primary);
   return submit->funcs->flush(submit, in_fence_fd, use_fence_fd);
}

struct fd_ringbuffer *
fd_submit_new_ringbuffer(struct fd_submit *submit, uint32_t size,
                         enum fd_ringbuffer_flags flags)
{
   assert(!(flags & _FD_RINGBUFFER_OBJECT));
   if (flags & FD_RINGBUFFER_STREAMING) {
      assert(!(flags & FD_RINGBUFFER_GROWABLE));
      assert(!(flags & FD_RINGBUFFER_PRIMARY));
   }
   struct fd_ringbuffer *ring =
         submit->funcs->new_ringbuffer(submit, size, flags);

   if (flags & FD_RINGBUFFER_PRIMARY) {
      assert(!submit->primary);
      submit->primary = fd_ringbuffer_ref(ring);
   }

   return ring;
}

struct fd_ringbuffer *
fd_ringbuffer_new_object(struct fd_pipe *pipe, uint32_t size)
{
   return pipe->funcs->ringbuffer_new_object(pipe, size);
}
