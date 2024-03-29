/*
 * Copyright 2010 Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_screen;
struct renderonly;

struct pipe_screen *agx_screen_create(int fd, struct renderonly *ro,
                                      const struct pipe_screen_config *config);

#ifdef __cplusplus
}
#endif
