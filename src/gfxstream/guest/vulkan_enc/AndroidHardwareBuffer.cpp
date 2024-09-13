/*
 * Copyright 2019 Google LLC
 * SPDX-License-Identifier: MIT
 */
#include "AndroidHardwareBuffer.h"

#if defined(__ANDROID__) || defined(__linux__)
#include <drm_fourcc.h>
#define DRM_FORMAT_YVU420_ANDROID fourcc_code('9', '9', '9', '7')
#define DRM_FORMAT_D16_UNORM fourcc_code('9', '9', '9', '6')
#define DRM_FORMAT_D24_UNORM fourcc_code('9', '9', '9', '5')
#define DRM_FORMAT_D24_UNORM_S8_UINT fourcc_code('9', '9', '9', '4')
#define DRM_FORMAT_D32_FLOAT fourcc_code('9', '9', '9', '3')
#define DRM_FORMAT_D32_FLOAT_S8_UINT fourcc_code('9', '9', '9', '2')
#define DRM_FORMAT_S8_UINT fourcc_code('9', '9', '9', '1')
#endif

#if defined(ANDROID)

#include <assert.h>

#include "gfxstream/guest/GfxStreamGralloc.h"
#include "vk_format_info.h"
#include "vk_util.h"
#include "util/log.h"

namespace gfxstream {
namespace vk {

// From Intel ANV implementation.
/* Construct ahw usage mask from image usage bits, see
 * 'AHardwareBuffer Usage Equivalence' in Vulkan spec.
 */
uint64_t getAndroidHardwareBufferUsageFromVkUsage(const VkImageCreateFlags vk_create,
                                                  const VkImageUsageFlags vk_usage) {
    uint64_t ahw_usage = 0;

    if (vk_usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    }
    if (vk_usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) {
        ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    }
    if (vk_usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
        ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    }
    if (vk_usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    }
    if (vk_create & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) {
        ahw_usage |= AHARDWAREBUFFER_USAGE_GPU_CUBE_MAP;
    }
    if (vk_create & VK_IMAGE_CREATE_PROTECTED_BIT) {
        ahw_usage |= AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT;
    }

    /* No usage bits set - set at least one GPU usage. */
    if (ahw_usage == 0) {
        ahw_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    }

    return ahw_usage;
}

VkResult getAndroidHardwareBufferPropertiesANDROID(
    gfxstream::Gralloc* grallocHelper, const AHardwareBuffer* buffer,
    VkAndroidHardwareBufferPropertiesANDROID* pProperties) {
    VkAndroidHardwareBufferFormatPropertiesANDROID* ahbFormatProps =
        vk_find_struct<VkAndroidHardwareBufferFormatPropertiesANDROID>(pProperties);

    const auto format = grallocHelper->getFormat(buffer);
    if (ahbFormatProps) {
        switch (format) {
            case AHARDWAREBUFFER_FORMAT_R8_UNORM:
                ahbFormatProps->format = VK_FORMAT_R8_UNORM;
                ahbFormatProps->externalFormat = DRM_FORMAT_R8;
                break;
            case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
                ahbFormatProps->format = VK_FORMAT_R8G8B8A8_UNORM;
                ahbFormatProps->externalFormat = DRM_FORMAT_ABGR8888;
                break;
            case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
                ahbFormatProps->format = VK_FORMAT_R8G8B8A8_UNORM;
                ahbFormatProps->externalFormat = DRM_FORMAT_XBGR8888;
                break;
            case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
                ahbFormatProps->format = VK_FORMAT_R8G8B8_UNORM;
                ahbFormatProps->externalFormat = DRM_FORMAT_BGR888;
                break;
            case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
                ahbFormatProps->format = VK_FORMAT_R5G6B5_UNORM_PACK16;
                ahbFormatProps->externalFormat = DRM_FORMAT_RGB565;
                break;
            case AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT:
                ahbFormatProps->format = VK_FORMAT_R16G16B16A16_SFLOAT;
                ahbFormatProps->externalFormat = DRM_FORMAT_ABGR16161616F;
                break;
            case AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM:
                ahbFormatProps->format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
                ahbFormatProps->externalFormat = DRM_FORMAT_ABGR2101010;
                break;
            case AHARDWAREBUFFER_FORMAT_D16_UNORM:
                ahbFormatProps->format = VK_FORMAT_D16_UNORM;
                ahbFormatProps->externalFormat = DRM_FORMAT_D16_UNORM;
                break;
            case AHARDWAREBUFFER_FORMAT_D24_UNORM:
                ahbFormatProps->format = VK_FORMAT_X8_D24_UNORM_PACK32;
                ahbFormatProps->externalFormat = DRM_FORMAT_D24_UNORM;
                break;
            case AHARDWAREBUFFER_FORMAT_D24_UNORM_S8_UINT:
                ahbFormatProps->format = VK_FORMAT_D24_UNORM_S8_UINT;
                ahbFormatProps->externalFormat = DRM_FORMAT_D24_UNORM_S8_UINT;
                break;
            case AHARDWAREBUFFER_FORMAT_D32_FLOAT:
                ahbFormatProps->format = VK_FORMAT_D32_SFLOAT;
                ahbFormatProps->externalFormat = DRM_FORMAT_D32_FLOAT;
                break;
            case AHARDWAREBUFFER_FORMAT_D32_FLOAT_S8_UINT:
                ahbFormatProps->format = VK_FORMAT_D32_SFLOAT_S8_UINT;
                ahbFormatProps->externalFormat = DRM_FORMAT_D32_FLOAT_S8_UINT;
                break;
            case AHARDWAREBUFFER_FORMAT_S8_UINT:
                ahbFormatProps->format = VK_FORMAT_S8_UINT;
                ahbFormatProps->externalFormat = DRM_FORMAT_S8_UINT;
                break;
            default:
                ahbFormatProps->format = VK_FORMAT_UNDEFINED;
                ahbFormatProps->externalFormat = DRM_FORMAT_INVALID;
        }

        // The formatFeatures member must include
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT and at least one of
        // VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT or
        // VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT, and should include
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT and
        // VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT.

        // org.skia.skqp.SkQPRunner#UnitTest_VulkanHardwareBuffer* requires the following:
        // VK_FORMAT_FEATURE_TRANSFER_SRC_BIT
        // VK_FORMAT_FEATURE_TRANSFER_DST_BIT
        // VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
        ahbFormatProps->formatFeatures =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
            VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;

        // "Implementations may not always be able to determine the color model,
        // numerical range, or chroma offsets of the image contents, so the values in
        // VkAndroidHardwareBufferFormatPropertiesANDROID are only suggestions.
        // Applications should treat these values as sensible defaults to use in the
        // absence of more reliable information obtained through some other means."

        ahbFormatProps->samplerYcbcrConversionComponents.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        ahbFormatProps->samplerYcbcrConversionComponents.a = VK_COMPONENT_SWIZZLE_IDENTITY;

#if defined(__ANDROID__) || defined(__linux__)
        if (android_format_is_yuv(format)) {
            uint32_t drmFormat = grallocHelper->getFormatDrmFourcc(buffer);
            ahbFormatProps->externalFormat = static_cast<uint64_t>(drmFormat);
            if (drmFormat) {
                // The host renderer is not aware of the plane ordering for YUV formats used
                // in the guest and simply knows that the format "layout" is one of:
                //
                //  * VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
                //  * VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
                //  * VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM
                //
                // With this, the guest needs to adjust the component swizzle based on plane
                // ordering to ensure that the channels are interpreted correctly.
                //
                // From the Vulkan spec's "Sampler Y'CBCR Conversion" section:
                //
                //  * Y comes from the G-channel (after swizzle)
                //  * U (CB) comes from the B-channel (after swizzle)
                //  * V (CR) comes from the R-channel (after swizzle)
                //
                // See
                // https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html#textures-sampler-YCbCr-conversion
                //
                // To match the above, the guest needs to swizzle such that:
                //
                //  * Y ends up in the G-channel
                //  * U (CB) ends up in the B-channel
                //  * V (CB) ends up in the R-channel
                switch (drmFormat) {
                    case DRM_FORMAT_NV12:
                        // NV12 is a Y-plane followed by a interleaved UV-plane and is
                        // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM on the host.
                        break;
                    case DRM_FORMAT_P010:
                        // P010 is a Y-plane followed by a interleaved UV-plane and is
                        // VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 on the host.
                        break;
                    case DRM_FORMAT_YUV420:
                        // YUV420 is a Y-plane, then a U-plane, and then a V-plane and is
                        // VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM on the host.
                        break;
                    case DRM_FORMAT_NV21:
                        // NV21 is a Y-plane followed by a interleaved VU-plane and is
                        // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM on the host.
                    case DRM_FORMAT_YVU420:
                        // YVU420 is a Y-plane, then a V-plane, and then a U-plane and is
                        // VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM on the host.
                    case DRM_FORMAT_YVU420_ANDROID:
                        // DRM_FORMAT_YVU420_ANDROID is the same as DRM_FORMAT_YVU420 with
                        // Android's extra alignement requirements.
                        ahbFormatProps->samplerYcbcrConversionComponents.r = VK_COMPONENT_SWIZZLE_B;
                        ahbFormatProps->samplerYcbcrConversionComponents.b = VK_COMPONENT_SWIZZLE_R;
                        break;

                    default:
                        mesa_loge("Unhandled YUV drm format:%u", drmFormat);
                        break;
                }
            }
        }
#endif
        ahbFormatProps->suggestedYcbcrModel = android_format_is_yuv(format)
                                                  ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601
                                                  : VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
        ahbFormatProps->suggestedYcbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;

        ahbFormatProps->suggestedXChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        ahbFormatProps->suggestedYChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    }

    uint32_t colorBufferHandle = grallocHelper->getHostHandle(buffer);
    if (!colorBufferHandle) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    pProperties->allocationSize = grallocHelper->getAllocatedSize(buffer);

    return VK_SUCCESS;
}

// Based on Intel ANV implementation.
VkResult getMemoryAndroidHardwareBufferANDROID(gfxstream::Gralloc* gralloc,
                                               struct AHardwareBuffer** pBuffer) {
    /* Some quotes from Vulkan spec:
     *
     * "If the device memory was created by importing an Android hardware
     * buffer, vkGetMemoryAndroidHardwareBufferANDROID must return that same
     * Android hardware buffer object."
     *
     * "VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID must
     * have been included in VkExportMemoryAllocateInfo::handleTypes when
     * memory was created."
     */

    if (!pBuffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!(*pBuffer)) return VK_ERROR_OUT_OF_HOST_MEMORY;

    gralloc->acquire(*pBuffer);
    return VK_SUCCESS;
}

VkResult importAndroidHardwareBuffer(gfxstream::Gralloc* grallocHelper,
                                     const VkImportAndroidHardwareBufferInfoANDROID* info,
                                     struct AHardwareBuffer** importOut) {
    if (!info || !info->buffer) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    auto ahb = info->buffer;

    uint32_t colorBufferHandle = grallocHelper->getHostHandle(ahb);
    if (!colorBufferHandle) {
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }

    grallocHelper->acquire(ahb);

    if (importOut) *importOut = ahb;

    return VK_SUCCESS;
}

VkResult createAndroidHardwareBuffer(gfxstream::Gralloc* gralloc, bool hasDedicatedImage,
                                     bool hasDedicatedBuffer, const VkExtent3D& imageExtent,
                                     uint32_t imageLayers, VkFormat imageFormat,
                                     VkImageUsageFlags imageUsage,
                                     VkImageCreateFlags imageCreateFlags, VkDeviceSize bufferSize,
                                     VkDeviceSize allocationInfoAllocSize,
                                     struct AHardwareBuffer** out) {
    uint32_t w = 0;
    uint32_t h = 1;
    uint32_t layers = 1;
    uint32_t format = 0;
    uint64_t usage = 0;

    /* If caller passed dedicated information. */
    if (hasDedicatedImage) {
        w = imageExtent.width;
        h = imageExtent.height;
        layers = imageLayers;
        format = android_format_from_vk(imageFormat);
        usage = getAndroidHardwareBufferUsageFromVkUsage(imageCreateFlags, imageUsage);
    } else if (hasDedicatedBuffer) {
        w = bufferSize;
        format = AHARDWAREBUFFER_FORMAT_BLOB;
        usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    } else {
        w = allocationInfoAllocSize;
        format = AHARDWAREBUFFER_FORMAT_BLOB;
        usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
    }

    struct AHardwareBuffer* ahb = NULL;

    if (gralloc->allocate(w, h, format, usage, &ahb) != 0) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *out = ahb;

    return VK_SUCCESS;
}

}  // namespace vk
}  // namespace gfxstream

#endif  // ANDROID
