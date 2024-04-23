// Copyright (C) 2023 The Android Open Source Project
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

#include "gfxstream_vk_private.h"

#include "vk_sync_dummy.h"

/* Under the assumption that Mesa VK runtime queue submission is used, WSI flow
 * sets this temporary state to a dummy sync type (when no explicit dma-buf
 * synchronization is available). For gfxstream, ignore this sync object when
 * this is the case. Synchronization will be done on the host.
 */

static bool isNoopFence(gfxstream_vk_fence* fence) {
    return (fence && fence->vk.temporary && vk_sync_type_is_dummy(fence->vk.temporary->type));
}

static bool isNoopSemaphore(gfxstream_vk_semaphore* semaphore) {
    return (semaphore && semaphore->vk.temporary &&
            vk_sync_type_is_dummy(semaphore->vk.temporary->type));
}

std::vector<VkFence> transformVkFenceList(const VkFence* pFences, uint32_t fenceCount) {
    std::vector<VkFence> outFences;
    for (uint32_t j = 0; j < fenceCount; ++j) {
        VK_FROM_HANDLE(gfxstream_vk_fence, gfxstream_fence, pFences[j]);
        if (!isNoopFence(gfxstream_fence)) {
            outFences.push_back(gfxstream_fence->internal_object);
        }
    }
    return outFences;
}

std::vector<VkSemaphore> transformVkSemaphoreList(const VkSemaphore* pSemaphores,
                                                  uint32_t semaphoreCount) {
    std::vector<VkSemaphore> outSemaphores;
    for (uint32_t j = 0; j < semaphoreCount; ++j) {
        VK_FROM_HANDLE(gfxstream_vk_semaphore, gfxstream_semaphore, pSemaphores[j]);
        if (!isNoopSemaphore(gfxstream_semaphore)) {
            outSemaphores.push_back(gfxstream_semaphore->internal_object);
        }
    }
    return outSemaphores;
}

std::vector<VkSemaphoreSubmitInfo> transformVkSemaphoreSubmitInfoList(
    const VkSemaphoreSubmitInfo* pSemaphoreSubmitInfos, uint32_t semaphoreSubmitInfoCount) {
    std::vector<VkSemaphoreSubmitInfo> outSemaphoreSubmitInfo;
    for (uint32_t j = 0; j < semaphoreSubmitInfoCount; ++j) {
        VkSemaphoreSubmitInfo outInfo = pSemaphoreSubmitInfos[j];
        VK_FROM_HANDLE(gfxstream_vk_semaphore, gfxstream_semaphore, outInfo.semaphore);
        if (!isNoopSemaphore(gfxstream_semaphore)) {
            outInfo.semaphore = gfxstream_semaphore->internal_object;
            outSemaphoreSubmitInfo.push_back(outInfo);
        }
    }
    return outSemaphoreSubmitInfo;
}
