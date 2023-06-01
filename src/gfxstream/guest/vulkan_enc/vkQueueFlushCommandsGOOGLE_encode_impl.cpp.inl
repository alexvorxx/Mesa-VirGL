// Manual inline for
// void VkEncoder::vkQueueFlushCommandsGOOGLE( VkQueue queue, VkCommandBuffer commandBuffer, VkDeviceSize dataSize, const void* pData, uint32_t doLock);

// We won't use the lock if this command is used (VulkanQueueSubmitWithCommands is enabled)
(void)doLock;

auto stream = mImpl->stream();
auto pool = mImpl->pool();
VkQueue local_queue;
VkCommandBuffer local_commandBuffer;
VkDeviceSize local_dataSize;
void* local_pData;
local_queue = queue;
local_commandBuffer = commandBuffer;
local_dataSize = dataSize;
// Avoiding deepcopy for pData
local_pData = (void*)pData;
size_t count = 0;
size_t* countPtr = &count;
{
    uint64_t cgen_var_1405;
    *countPtr += 1 * 8;
    uint64_t cgen_var_1406;
    *countPtr += 1 * 8;
    *countPtr += sizeof(VkDeviceSize);
    *countPtr += ((dataSize)) * sizeof(uint8_t);
}
bool queueSubmitWithCommandsEnabled = sFeatureBits & VULKAN_STREAM_FEATURE_QUEUE_SUBMIT_WITH_COMMANDS_BIT;
uint32_t packetSize_vkQueueFlushCommandsGOOGLE = 4 + 4 + (queueSubmitWithCommandsEnabled ? 4 : 0) + count;
healthMonitorAnnotation_packetSize =
    std::make_optional(packetSize_vkQueueFlushCommandsGOOGLE);
uint8_t* streamPtr = stream->reserve(packetSize_vkQueueFlushCommandsGOOGLE - local_dataSize);
uint8_t* packetBeginPtr = streamPtr;
uint8_t** streamPtrPtr = &streamPtr;
uint32_t opcode_vkQueueFlushCommandsGOOGLE = OP_vkQueueFlushCommandsGOOGLE;
uint32_t seqno = ResourceTracker::nextSeqno();
healthMonitorAnnotation_seqno = std::make_optional(seqno);
memcpy(streamPtr, &opcode_vkQueueFlushCommandsGOOGLE, sizeof(uint32_t)); streamPtr += sizeof(uint32_t);
memcpy(streamPtr, &packetSize_vkQueueFlushCommandsGOOGLE, sizeof(uint32_t)); streamPtr += sizeof(uint32_t);
memcpy(streamPtr, &seqno, sizeof(uint32_t)); streamPtr += sizeof(uint32_t);
uint64_t cgen_var_1407;
*&cgen_var_1407 = get_host_u64_VkQueue((*&local_queue));
memcpy(*streamPtrPtr, (uint64_t*)&cgen_var_1407, 1 * 8);
*streamPtrPtr += 1 * 8;
uint64_t cgen_var_1408;
*&cgen_var_1408 = get_host_u64_VkCommandBuffer((*&local_commandBuffer));
memcpy(*streamPtrPtr, (uint64_t*)&cgen_var_1408, 1 * 8);
*streamPtrPtr += 1 * 8;
memcpy(*streamPtrPtr, (VkDeviceSize*)&local_dataSize, sizeof(VkDeviceSize));
*streamPtrPtr += sizeof(VkDeviceSize);
if (watchdog) {
    size_t watchdogBufSize = std::min<size_t>(
        static_cast<size_t>(packetSize_vkQueueFlushCommandsGOOGLE), kWatchdogBufferMax);
    healthMonitorAnnotation_packetContents.resize(watchdogBufSize);
    memcpy(&healthMonitorAnnotation_packetContents[0], packetBeginPtr, watchdogBufSize);
}

AEMU_SCOPED_TRACE("vkQueueFlush large xfer");
stream->flush();
stream->writeLarge(local_pData, dataSize);

++encodeCount;;
if (0 == encodeCount % POOL_CLEAR_INTERVAL)
{
    pool->freeAll();
    stream->clearPool();
}
