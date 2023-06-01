// Copyright (C) 2018 The Android Open Source Project
// Copyright (C) 2018 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <vulkan/vulkan.h>

#include <functional>
#include <memory>

#include "CommandBufferStagingStream.h"
#include "VulkanHandleMapping.h"
#include "VulkanHandles.h"
#include "aemu/base/Tracing.h"
#include "goldfish_vk_transform_guest.h"

struct EmulatorFeatureInfo;

class HostConnection;

namespace gfxstream {
namespace vk {

class VkEncoder;

class ResourceTracker {
public:
    ResourceTracker();
    virtual ~ResourceTracker();
    static ResourceTracker* get();

    VulkanHandleMapping* createMapping();
    VulkanHandleMapping* unwrapMapping();
    VulkanHandleMapping* destroyMapping();
    VulkanHandleMapping* defaultMapping();

    using HostConnectionGetFunc = HostConnection* (*)();
    using VkEncoderGetFunc = VkEncoder* (*)(HostConnection*);
    using CleanupCallback = std::function<void()>;

    struct ThreadingCallbacks {
        HostConnectionGetFunc hostConnectionGetFunc = 0;
        VkEncoderGetFunc vkEncoderGetFunc = 0;
    };

    static uint32_t streamFeatureBits;
    static ThreadingCallbacks threadingCallbacks;

#define HANDLE_REGISTER_DECL(type) \
    void register_##type(type); \
    void unregister_##type(type); \

    GOLDFISH_VK_LIST_HANDLE_TYPES(HANDLE_REGISTER_DECL)

    VkResult on_vkEnumerateInstanceExtensionProperties(
        void* context,
        VkResult input_result,
        const char* pLayerName,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties);

    VkResult on_vkEnumerateDeviceExtensionProperties(
        void* context,
        VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const char* pLayerName,
        uint32_t* pPropertyCount,
        VkExtensionProperties* pProperties);

    VkResult on_vkEnumeratePhysicalDevices(
        void* context, VkResult input_result,
        VkInstance instance, uint32_t* pPhysicalDeviceCount,
        VkPhysicalDevice* pPhysicalDevices);

    void on_vkGetPhysicalDeviceFeatures2(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures);
    void on_vkGetPhysicalDeviceFeatures2KHR(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2* pFeatures);
    void on_vkGetPhysicalDeviceProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties* pProperties);
    void on_vkGetPhysicalDeviceProperties2(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties);
    void on_vkGetPhysicalDeviceProperties2KHR(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2* pProperties);

    void on_vkGetPhysicalDeviceMemoryProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties* pMemoryProperties);
    void on_vkGetPhysicalDeviceMemoryProperties2(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties2* pMemoryProperties);
    void on_vkGetPhysicalDeviceMemoryProperties2KHR(
        void* context,
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties2* pMemoryProperties);
    void on_vkGetDeviceQueue(void* context,
                             VkDevice device,
                             uint32_t queueFamilyIndex,
                             uint32_t queueIndex,
                             VkQueue* pQueue);
    void on_vkGetDeviceQueue2(void* context,
                              VkDevice device,
                              const VkDeviceQueueInfo2* pQueueInfo,
                              VkQueue* pQueue);

    VkResult on_vkCreateInstance(
        void* context,
        VkResult input_result,
        const VkInstanceCreateInfo* createInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance);
    VkResult on_vkCreateDevice(
        void* context,
        VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice);
    void on_vkDestroyDevice_pre(
        void* context,
        VkDevice device,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkAllocateMemory(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkMemoryAllocateInfo* pAllocateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory);
    void on_vkFreeMemory(
        void* context,
        VkDevice device,
        VkDeviceMemory memory,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkMapMemory(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkDeviceMemory memory,
        VkDeviceSize offset,
        VkDeviceSize size,
        VkMemoryMapFlags,
        void** ppData);

    void on_vkUnmapMemory(
        void* context,
        VkDevice device,
        VkDeviceMemory memory);

    VkResult on_vkCreateImage(
        void* context, VkResult input_result,
        VkDevice device, const VkImageCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkImage *pImage);
    void on_vkDestroyImage(
        void* context,
        VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator);

    void on_vkGetImageMemoryRequirements(
        void *context, VkDevice device, VkImage image,
        VkMemoryRequirements *pMemoryRequirements);
    void on_vkGetImageMemoryRequirements2(
        void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements);
    void on_vkGetImageMemoryRequirements2KHR(
        void *context, VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements);

    VkResult on_vkBindImageMemory(
        void* context, VkResult input_result,
        VkDevice device, VkImage image, VkDeviceMemory memory,
        VkDeviceSize memoryOffset);
    VkResult on_vkBindImageMemory2(
        void* context, VkResult input_result,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos);
    VkResult on_vkBindImageMemory2KHR(
        void* context, VkResult input_result,
        VkDevice device, uint32_t bindingCount, const VkBindImageMemoryInfo* pBindInfos);

    VkResult on_vkCreateBuffer(
        void* context, VkResult input_result,
        VkDevice device, const VkBufferCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkBuffer *pBuffer);
    void on_vkDestroyBuffer(
        void* context,
        VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator);

    void on_vkGetBufferMemoryRequirements(
        void* context, VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements);
    void on_vkGetBufferMemoryRequirements2(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements);
    void on_vkGetBufferMemoryRequirements2KHR(
        void* context, VkDevice device, const VkBufferMemoryRequirementsInfo2* pInfo,
        VkMemoryRequirements2* pMemoryRequirements);

    VkResult on_vkBindBufferMemory(
        void* context, VkResult input_result,
        VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset);
    VkResult on_vkBindBufferMemory2(
        void* context, VkResult input_result,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos);
    VkResult on_vkBindBufferMemory2KHR(
        void* context, VkResult input_result,
        VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos);

    VkResult on_vkCreateSemaphore(
        void* context, VkResult,
        VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSemaphore* pSemaphore);
    void on_vkDestroySemaphore(
        void* context,
        VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator);
    VkResult on_vkGetSemaphoreFdKHR(
        void* context, VkResult,
        VkDevice device,
        const VkSemaphoreGetFdInfoKHR* pGetFdInfo,
        int* pFd);
    VkResult on_vkImportSemaphoreFdKHR(
        void* context, VkResult,
        VkDevice device,
        const VkImportSemaphoreFdInfoKHR* pImportSemaphoreFdInfo);

    VkResult on_vkQueueSubmit(
        void* context, VkResult input_result,
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);

    VkResult on_vkQueueWaitIdle(
        void* context, VkResult input_result,
        VkQueue queue);

    void unwrap_VkNativeBufferANDROID(
        const VkImageCreateInfo* pCreateInfo,
        VkImageCreateInfo* local_pCreateInfo);
    void unwrap_vkAcquireImageANDROID_nativeFenceFd(int fd, int* fd_out);

#ifdef VK_USE_PLATFORM_FUCHSIA
    VkResult on_vkGetMemoryZirconHandleFUCHSIA(
        void* context, VkResult input_result,
        VkDevice device,
        const VkMemoryGetZirconHandleInfoFUCHSIA* pInfo,
        uint32_t* pHandle);
    VkResult on_vkGetMemoryZirconHandlePropertiesFUCHSIA(
        void* context, VkResult input_result,
        VkDevice device,
        VkExternalMemoryHandleTypeFlagBits handleType,
        uint32_t handle,
        VkMemoryZirconHandlePropertiesFUCHSIA* pProperties);
    VkResult on_vkGetSemaphoreZirconHandleFUCHSIA(
        void* context, VkResult input_result,
        VkDevice device,
        const VkSemaphoreGetZirconHandleInfoFUCHSIA* pInfo,
        uint32_t* pHandle);
    VkResult on_vkImportSemaphoreZirconHandleFUCHSIA(
        void* context, VkResult input_result,
        VkDevice device,
        const VkImportSemaphoreZirconHandleInfoFUCHSIA* pInfo);
    VkResult on_vkCreateBufferCollectionFUCHSIA(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkBufferCollectionCreateInfoFUCHSIA* pInfo,
        const VkAllocationCallbacks* pAllocator,
        VkBufferCollectionFUCHSIA* pCollection);
    void on_vkDestroyBufferCollectionFUCHSIA(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkBufferCollectionFUCHSIA collection,
        const VkAllocationCallbacks* pAllocator);
    VkResult on_vkSetBufferCollectionBufferConstraintsFUCHSIA(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkBufferCollectionFUCHSIA collection,
        const VkBufferConstraintsInfoFUCHSIA* pBufferConstraintsInfo);
    VkResult on_vkSetBufferCollectionImageConstraintsFUCHSIA(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkBufferCollectionFUCHSIA collection,
        const VkImageConstraintsInfoFUCHSIA* pImageConstraintsInfo);
    VkResult on_vkGetBufferCollectionPropertiesFUCHSIA(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkBufferCollectionFUCHSIA collection,
        VkBufferCollectionPropertiesFUCHSIA* pProperties);
#endif

#ifdef VK_USE_PLATFORM_ANDROID_KHR
    VkResult on_vkGetAndroidHardwareBufferPropertiesANDROID(
        void* context, VkResult input_result,
        VkDevice device,
        const AHardwareBuffer* buffer,
        VkAndroidHardwareBufferPropertiesANDROID* pProperties);
    VkResult on_vkGetMemoryAndroidHardwareBufferANDROID(
        void* context, VkResult input_result,
        VkDevice device,
        const VkMemoryGetAndroidHardwareBufferInfoANDROID *pInfo,
        struct AHardwareBuffer** pBuffer);
#endif

    VkResult on_vkCreateSamplerYcbcrConversion(
        void* context, VkResult input_result,
        VkDevice device,
        const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSamplerYcbcrConversion* pYcbcrConversion);
    void on_vkDestroySamplerYcbcrConversion(
        void* context,
        VkDevice device,
        VkSamplerYcbcrConversion ycbcrConversion,
        const VkAllocationCallbacks* pAllocator);
    VkResult on_vkCreateSamplerYcbcrConversionKHR(
        void* context, VkResult input_result,
        VkDevice device,
        const VkSamplerYcbcrConversionCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSamplerYcbcrConversion* pYcbcrConversion);
    void on_vkDestroySamplerYcbcrConversionKHR(
        void* context,
        VkDevice device,
        VkSamplerYcbcrConversion ycbcrConversion,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkCreateSampler(
        void* context, VkResult input_result,
        VkDevice device,
        const VkSamplerCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkSampler* pSampler);

    void on_vkGetPhysicalDeviceExternalFenceProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
        VkExternalFenceProperties* pExternalFenceProperties);

    void on_vkGetPhysicalDeviceExternalFencePropertiesKHR(
        void* context,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceExternalFenceInfo* pExternalFenceInfo,
        VkExternalFenceProperties* pExternalFenceProperties);

    VkResult on_vkCreateFence(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkFenceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator, VkFence* pFence);

    void on_vkDestroyFence(
        void* context,
        VkDevice device,
        VkFence fence,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkResetFences(
        void* context,
        VkResult input_result,
        VkDevice device,
        uint32_t fenceCount,
        const VkFence* pFences);

    VkResult on_vkImportFenceFdKHR(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkImportFenceFdInfoKHR* pImportFenceFdInfo);

    VkResult on_vkGetFenceFdKHR(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkFenceGetFdInfoKHR* pGetFdInfo,
        int* pFd);

    VkResult on_vkWaitForFences(
        void* context,
        VkResult input_result,
        VkDevice device,
        uint32_t fenceCount,
        const VkFence* pFences,
        VkBool32 waitAll,
        uint64_t timeout);

    VkResult on_vkCreateDescriptorPool(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkDescriptorPoolCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorPool* pDescriptorPool);

    void on_vkDestroyDescriptorPool(
        void* context,
        VkDevice device,
        VkDescriptorPool descriptorPool,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkResetDescriptorPool(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkDescriptorPool descriptorPool,
        VkDescriptorPoolResetFlags flags);

    VkResult on_vkAllocateDescriptorSets(
        void* context,
        VkResult input_result,
        VkDevice                                    device,
        const VkDescriptorSetAllocateInfo*          pAllocateInfo,
        VkDescriptorSet*                            pDescriptorSets);

    VkResult on_vkFreeDescriptorSets(
        void* context,
        VkResult input_result,
        VkDevice                                    device,
        VkDescriptorPool                            descriptorPool,
        uint32_t                                    descriptorSetCount,
        const VkDescriptorSet*                      pDescriptorSets);

    VkResult on_vkCreateDescriptorSetLayout(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorSetLayout* pSetLayout);

    void on_vkUpdateDescriptorSets(
        void* context,
        VkDevice device,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet* pDescriptorWrites,
        uint32_t descriptorCopyCount,
        const VkCopyDescriptorSet* pDescriptorCopies);

    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE_pre(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkDeviceMemory memory,
        uint64_t* pAddress);
    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkDeviceMemory memory,
        uint64_t* pAddress);

    VkResult on_vkCreateDescriptorUpdateTemplate(
        void* context, VkResult input_result,
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate);

    VkResult on_vkCreateDescriptorUpdateTemplateKHR(
        void* context, VkResult input_result,
        VkDevice device,
        const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate);

    void on_vkUpdateDescriptorSetWithTemplate(
        void* context,
        VkDevice device,
        VkDescriptorSet descriptorSet,
        VkDescriptorUpdateTemplate descriptorUpdateTemplate,
        const void* pData);

    VkResult on_vkGetPhysicalDeviceImageFormatProperties2(
        void* context, VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
        VkImageFormatProperties2* pImageFormatProperties);

    VkResult on_vkGetPhysicalDeviceImageFormatProperties2KHR(
        void* context, VkResult input_result,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceImageFormatInfo2* pImageFormatInfo,
        VkImageFormatProperties2* pImageFormatProperties);

    void on_vkGetPhysicalDeviceExternalSemaphoreProperties(
        void* context,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
        VkExternalSemaphoreProperties* pExternalSemaphoreProperties);

    void on_vkGetPhysicalDeviceExternalSemaphorePropertiesKHR(
        void* context,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceExternalSemaphoreInfo* pExternalSemaphoreInfo,
        VkExternalSemaphoreProperties* pExternalSemaphoreProperties);

    void registerEncoderCleanupCallback(const VkEncoder* encoder, void* handle, CleanupCallback callback);
    void unregisterEncoderCleanupCallback(const VkEncoder* encoder, void* handle);
    void onEncoderDeleted(const VkEncoder* encoder);

    uint32_t syncEncodersForCommandBuffer(VkCommandBuffer commandBuffer, VkEncoder* current);
    uint32_t syncEncodersForQueue(VkQueue queue, VkEncoder* current);

    CommandBufferStagingStream::Alloc getAlloc();
    CommandBufferStagingStream::Free getFree();

    VkResult on_vkBeginCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo);
    VkResult on_vkEndCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer);
    VkResult on_vkResetCommandBuffer(
        void* context, VkResult input_result,
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags);

    VkResult on_vkCreateImageView(
        void* context, VkResult input_result,
        VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkImageView* pView);

    void on_vkCmdExecuteCommands(
        void* context,
        VkCommandBuffer commandBuffer,
        uint32_t commandBufferCount,
        const VkCommandBuffer* pCommandBuffers);

    void on_vkCmdBindDescriptorSets(
        void* context,
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t firstSet,
        uint32_t descriptorSetCount,
        const VkDescriptorSet* pDescriptorSets,
        uint32_t dynamicOffsetCount,
        const uint32_t* pDynamicOffsets);

    void on_vkCmdPipelineBarrier(
        void* context,
        VkCommandBuffer commandBuffer,
        VkPipelineStageFlags srcStageMask,
        VkPipelineStageFlags dstStageMask,
        VkDependencyFlags dependencyFlags,
        uint32_t memoryBarrierCount,
        const VkMemoryBarrier* pMemoryBarriers,
        uint32_t bufferMemoryBarrierCount,
        const VkBufferMemoryBarrier* pBufferMemoryBarriers,
        uint32_t imageMemoryBarrierCount,
        const VkImageMemoryBarrier* pImageMemoryBarriers);

    void on_vkDestroyDescriptorSetLayout(
        void* context,
        VkDevice device,
        VkDescriptorSetLayout descriptorSetLayout,
        const VkAllocationCallbacks* pAllocator);

    VkResult on_vkAllocateCommandBuffers(
        void* context,
        VkResult input_result,
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkCommandBuffer* pCommandBuffers);

    VkResult on_vkQueueSignalReleaseImageANDROID(
        void* context,
        VkResult input_result,
        VkQueue queue,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        VkImage image,
        int* pNativeFenceFd);

    VkResult on_vkCreateGraphicsPipelines(
        void* context,
        VkResult input_result,
        VkDevice device,
        VkPipelineCache pipelineCache,
        uint32_t createInfoCount,
        const VkGraphicsPipelineCreateInfo* pCreateInfos,
        const VkAllocationCallbacks* pAllocator,
        VkPipeline* pPipelines);

    uint8_t* getMappedPointer(VkDeviceMemory memory);
    VkDeviceSize getMappedSize(VkDeviceMemory memory);
    VkDeviceSize getNonCoherentExtendedSize(VkDevice device, VkDeviceSize basicSize) const;
    bool isValidMemoryRange(const VkMappedMemoryRange& range) const;

    void setupFeatures(const EmulatorFeatureInfo* features);
    void setupCaps(void);

    void setThreadingCallbacks(const ThreadingCallbacks& callbacks);
    bool hostSupportsVulkan() const;
    bool usingDirectMapping() const;
    uint32_t getStreamFeatures() const;
    uint32_t getApiVersionFromInstance(VkInstance instance) const;
    uint32_t getApiVersionFromDevice(VkDevice device) const;
    bool hasInstanceExtension(VkInstance instance, const std::string& name) const;
    bool hasDeviceExtension(VkDevice instance, const std::string& name) const;
    VkDevice getDevice(VkCommandBuffer commandBuffer) const;
    void addToCommandPool(VkCommandPool commandPool,
                          uint32_t commandBufferCount,
                          VkCommandBuffer* pCommandBuffers);
    void resetCommandPoolStagingInfo(VkCommandPool commandPool);

#ifdef __GNUC__
    #define ALWAYS_INLINE
#elif
    #define ALWAYS_INLINE __attribute__((always_inline))
#endif

    static VkEncoder* getCommandBufferEncoder(VkCommandBuffer commandBuffer);
    static VkEncoder* getQueueEncoder(VkQueue queue);
    static VkEncoder* getThreadLocalEncoder();

    static void setSeqnoPtr(uint32_t* seqnoptr);
    static ALWAYS_INLINE uint32_t nextSeqno();
    static ALWAYS_INLINE uint32_t getSeqno();

    // Transforms
    void deviceMemoryTransform_tohost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount);
    void deviceMemoryTransform_fromhost(
        VkDeviceMemory* memory, uint32_t memoryCount,
        VkDeviceSize* offset, uint32_t offsetCount,
        VkDeviceSize* size, uint32_t sizeCount,
        uint32_t* typeIndex, uint32_t typeIndexCount,
        uint32_t* typeBits, uint32_t typeBitsCount);

    void transformImpl_VkExternalMemoryProperties_fromhost(
        VkExternalMemoryProperties* pProperties,
        uint32_t);
    void transformImpl_VkExternalMemoryProperties_tohost(
        VkExternalMemoryProperties* pProperties,
        uint32_t);
    void transformImpl_VkImageCreateInfo_fromhost(const VkImageCreateInfo*, uint32_t);
    void transformImpl_VkImageCreateInfo_tohost(const VkImageCreateInfo*, uint32_t);

#define DEFINE_TRANSFORMED_TYPE_PROTOTYPE(type)          \
    void transformImpl_##type##_tohost(type*, uint32_t); \
    void transformImpl_##type##_fromhost(type*, uint32_t);

    LIST_TRIVIAL_TRANSFORMED_TYPES(DEFINE_TRANSFORMED_TYPE_PROTOTYPE)

private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

}  // namespace vk
}  // namespace gfxstream
