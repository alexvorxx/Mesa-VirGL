// Copyright 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "GrallocMinigbm.h"

#include <cros_gralloc/cros_gralloc_handle.h>
#include <errno.h>
#include <log/log.h>
#include <stdlib.h>
#include <sys/user.h>
#include <vndk/hardware_buffer.h>
#include <xf86drm.h>

#include <cinttypes>
#include <cstring>

#include "virtgpu_drm.h"

#if defined(PAGE_SIZE)
constexpr size_t kPageSize = PAGE_SIZE;
#else
#include <unistd.h>
static const size_t kPageSize = getpagesize();
#endif

namespace gfxstream {
namespace {

static inline uint32_t align_up(uint32_t n, uint32_t a) { return ((n + a - 1) / a) * a; }

bool getVirtioGpuResourceInfo(int fd, native_handle_t const* handle,
                              struct drm_virtgpu_resource_info* info) {
    memset(info, 0x0, sizeof(*info));
    if (fd < 0) {
        ALOGE("%s: Error, rendernode fd missing\n", __func__);
        return false;
    }

    struct drm_gem_close gem_close;
    memset(&gem_close, 0x0, sizeof(gem_close));

    cros_gralloc_handle const* cros_handle = reinterpret_cast<cros_gralloc_handle const*>(handle);

    uint32_t prime_handle;
    int ret = drmPrimeFDToHandle(fd, cros_handle->fds[0], &prime_handle);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_PRIME_FD_TO_HANDLE failed: %s (errno %d)\n", __func__, strerror(errno),
              errno);
        return false;
    }
    struct ManagedDrmGem {
        ManagedDrmGem(int fd, uint32_t handle) : m_fd(fd), m_prime_handle(handle) {}
        ManagedDrmGem(const ManagedDrmGem&) = delete;
        ~ManagedDrmGem() {
            struct drm_gem_close gem_close {
                .handle = m_prime_handle, .pad = 0,
            };
            int ret = drmIoctl(m_fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
            if (ret) {
                ALOGE("%s: DRM_IOCTL_GEM_CLOSE failed on handle %" PRIu32 ": %s(%d).", __func__,
                      m_prime_handle, strerror(errno), errno);
            }
        }

        int m_fd;
        uint32_t m_prime_handle;
    } managed_prime_handle(fd, prime_handle);

    info->bo_handle = managed_prime_handle.m_prime_handle;

    struct drm_virtgpu_3d_wait virtgpuWait {
        .handle = managed_prime_handle.m_prime_handle, .flags = 0,
    };
    // This only works for host resources by VIRTGPU_RESOURCE_CREATE ioctl.
    // We need to use a different mechanism to synchronize with the host if
    // the minigbm gralloc swiches to virtio-gpu blobs or cross-domain
    // backend.
    int retry = 0;
    do {
        if (retry > 10) {
            ALOGE("%s DRM_IOCTL_VIRTGPU_WAIT failed with EBUSY %d times.", __func__, retry);
            return false;
        }
        ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &virtgpuWait);
        ++retry;
    } while (ret < 0 && errno == EBUSY);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_VIRTGPU_WAIT failed: %s(%d)", __func__, strerror(errno), errno);
        return false;
    }

    ret = drmIoctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, info);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_VIRTGPU_RESOURCE_INFO failed: %s (errno %d)\n", __func__,
              strerror(errno), errno);
        return false;
    }

    return true;
}

}  // namespace

uint32_t MinigbmGralloc::createColorBuffer(void*, int width, int height, uint32_t glformat) {
    // Only supported format for pbuffers in gfxstream should be RGBA8
    const uint32_t kVirglFormatRGBA = 67;  // VIRGL_FORMAT_R8G8B8A8_UNORM;
    uint32_t virtgpu_format = 0;
    uint32_t bpp = 0;
    switch (glformat) {
        case kGlRGB:
            ALOGV("Note: egl wanted GL_RGB, still using RGBA");
            virtgpu_format = kVirglFormatRGBA;
            bpp = 4;
            break;
        case kGlRGBA:
            virtgpu_format = kVirglFormatRGBA;
            bpp = 4;
            break;
        default:
            ALOGV("Note: egl wanted 0x%x, still using RGBA", glformat);
            virtgpu_format = kVirglFormatRGBA;
            bpp = 4;
            break;
    }
    const uint32_t kPipeTexture2D = 2;          // PIPE_TEXTURE_2D
    const uint32_t kBindRenderTarget = 1 << 1;  // VIRGL_BIND_RENDER_TARGET
    struct drm_virtgpu_resource_create res_create;
    memset(&res_create, 0, sizeof(res_create));
    res_create.target = kPipeTexture2D;
    res_create.format = virtgpu_format;
    res_create.bind = kBindRenderTarget;
    res_create.width = width;
    res_create.height = height;
    res_create.depth = 1;
    res_create.array_size = 1;
    res_create.last_level = 0;
    res_create.nr_samples = 0;
    res_create.stride = bpp * width;
    res_create.size = align_up(bpp * width * height, kPageSize);

    int ret = drmIoctl(m_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &res_create);
    if (ret) {
        ALOGE("%s: DRM_IOCTL_VIRTGPU_RESOURCE_CREATE failed with %s (%d)\n", __func__,
              strerror(errno), errno);
        abort();
    }

    return res_create.res_handle;
}

int MinigbmGralloc::allocate(uint32_t width, uint32_t height, uint32_t format, uint64_t usage,
                             AHardwareBuffer** outputAhb) {
    struct AHardwareBuffer_Desc desc = {
        .width = width,
        .height = height,
        .layers = 1,
        .format = format,
        .usage = usage,
    };

    return AHardwareBuffer_allocate(&desc, outputAhb);
}

void MinigbmGralloc::acquire(AHardwareBuffer* ahb) { AHardwareBuffer_acquire(ahb); }

void MinigbmGralloc::release(AHardwareBuffer* ahb) { AHardwareBuffer_release(ahb); }

int MinigbmGralloc::lock(AHardwareBuffer* ahb, uint8_t** ptr) {
    return AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr,
                                reinterpret_cast<void**>(ptr));
}

int MinigbmGralloc::unlock(AHardwareBuffer* ahb) { return AHardwareBuffer_unlock(ahb, nullptr); }

uint32_t MinigbmGralloc::getHostHandle(const native_handle_t* handle) {
    struct drm_virtgpu_resource_info info;
    if (!getVirtioGpuResourceInfo(m_fd, handle, &info)) {
        ALOGE("%s: failed to get resource info\n", __func__);
        return 0;
    }

    return info.res_handle;
}

uint32_t MinigbmGralloc::getHostHandle(const AHardwareBuffer* ahb) {
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(ahb);
    return getHostHandle(handle);
}

const native_handle_t* MinigbmGralloc::getNativeHandle(const AHardwareBuffer* ahb) {
    return AHardwareBuffer_getNativeHandle(ahb);
}

int MinigbmGralloc::getFormat(const native_handle_t* handle) {
    return ((cros_gralloc_handle*)handle)->droid_format;
}

int MinigbmGralloc::getFormat(const AHardwareBuffer* ahb) {
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(ahb);

    return ((cros_gralloc_handle*)handle)->droid_format;
}

uint32_t MinigbmGralloc::getFormatDrmFourcc(const native_handle_t* handle) {
    return ((cros_gralloc_handle*)handle)->format;
}

uint32_t MinigbmGralloc::getFormatDrmFourcc(const AHardwareBuffer* ahb) {
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(ahb);
    return getFormatDrmFourcc(handle);
}

uint32_t MinigbmGralloc::getWidth(const AHardwareBuffer* ahb) {
    AHardwareBuffer_Desc desc = {};
    AHardwareBuffer_describe(ahb, &desc);
    return desc.width;
}

uint32_t MinigbmGralloc::getHeight(const AHardwareBuffer* ahb) {
    AHardwareBuffer_Desc desc = {};
    AHardwareBuffer_describe(ahb, &desc);
    return desc.height;
}

size_t MinigbmGralloc::getAllocatedSize(const native_handle_t* handle) {
    struct drm_virtgpu_resource_info info;
    if (!getVirtioGpuResourceInfo(m_fd, handle, &info)) {
        ALOGE("%s: failed to get resource info\n", __func__);
        return 0;
    }
    return info.size;
}

size_t MinigbmGralloc::getAllocatedSize(const AHardwareBuffer* ahb) {
    const native_handle_t* handle = AHardwareBuffer_getNativeHandle(ahb);
    return getAllocatedSize(handle);
}

int MinigbmGralloc::getId(const AHardwareBuffer* ahb, uint64_t* id) {
#if ANDROID_API_LEVEL >= 31
    return AHardwareBuffer_getId(ahb, id);
#else
    (void)ahb;
    *id = 0;
    return 0;
#endif
}

}  // namespace gfxstream
