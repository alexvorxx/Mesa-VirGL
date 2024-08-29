/*
 * Copyright 2024 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "GfxStreamRenderControl.h"
#include <cerrno>

GfxStreamTransportType renderControlGetTransport() {
    return GFXSTREAM_TRANSPORT_VIRTIO_GPU_ADDRESS_SPACE;
}

int32_t renderControlInit(GfxStreamConnectionManager* mgr, void* vkInfo) { return -EINVAL; }
