/*
 * Copyright 2023 Google LLC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fidl/fuchsia.logger/cpp/wire.h>
#include <lib/syslog/global.h>
#include <lib/zx/channel.h>
#include <lib/zx/socket.h>
#include <lib/zxio/zxio.h>
#include <unistd.h>

#include "TraceProviderFuchsia.h"
#include "services/service_connector.h"

class VulkanDevice {
   public:
    VulkanDevice() : mHostSupportsGoldfish(IsAccessible(QEMU_PIPE_PATH)) {
        InitLogger();
        InitTraceProvider();
        gfxstream::vk::ResourceTracker::get();
    }

    static void InitLogger();

    static bool IsAccessible(const char* name) {
        zx_handle_t handle = GetConnectToServiceFunction()(name);
        if (handle == ZX_HANDLE_INVALID) return false;

        zxio_storage_t io_storage;
        zx_status_t status = zxio_create(handle, &io_storage);
        if (status != ZX_OK) return false;

        status = zxio_close(&io_storage.io, /*should_wait=*/true);
        if (status != ZX_OK) return false;

        return true;
    }

    static VulkanDevice& GetInstance() {
        static VulkanDevice g_instance;
        return g_instance;
    }

    PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* name) {
        return ::GetInstanceProcAddr(instance, name);
    }

   private:
    void InitTraceProvider();

    TraceProviderFuchsia mTraceProvider;
    const bool mHostSupportsGoldfish;
};

void VulkanDevice::InitLogger() {
    auto log_socket = ([]() -> std::optional<zx::socket> {
        fidl::ClientEnd<fuchsia_logger::LogSink> channel{
            zx::channel{GetConnectToServiceFunction()("/svc/fuchsia.logger.LogSink")}};
        if (!channel.is_valid()) return std::nullopt;

        zx::socket local_socket, remote_socket;
        zx_status_t status = zx::socket::create(ZX_SOCKET_DATAGRAM, &local_socket, &remote_socket);
        if (status != ZX_OK) return std::nullopt;

        auto result = fidl::WireCall(channel)->Connect(std::move(remote_socket));

        if (!result.ok()) return std::nullopt;

        return local_socket;
    })();
    if (!log_socket) return;

    fx_logger_config_t config = {
        .min_severity = FX_LOG_INFO,
        .log_sink_socket = log_socket->release(),
        .tags = nullptr,
        .num_tags = 0,
    };

    fx_log_reconfigure(&config);
}

void VulkanDevice::InitTraceProvider() {
    if (!mTraceProvider.Initialize()) {
        ALOGE("Trace provider failed to initialize");
    }
}

typedef VkResult(VKAPI_PTR* PFN_vkOpenInNamespaceAddr)(const char* pName, uint32_t handle);

namespace {

PFN_vkOpenInNamespaceAddr g_vulkan_connector;

zx_handle_t LocalConnectToServiceFunction(const char* pName) {
    zx::channel remote_endpoint, local_endpoint;
    zx_status_t status;
    if ((status = zx::channel::create(0, &remote_endpoint, &local_endpoint)) != ZX_OK) {
        ALOGE("zx::channel::create failed: %d", status);
        return ZX_HANDLE_INVALID;
    }
    if ((status = g_vulkan_connector(pName, remote_endpoint.release())) != ZX_OK) {
        ALOGE("vulkan_connector failed: %d", status);
        return ZX_HANDLE_INVALID;
    }
    return local_endpoint.release();
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void vk_icdInitializeOpenInNamespaceCallback(
    PFN_vkOpenInNamespaceAddr callback) {
    g_vulkan_connector = callback;
    SetConnectToServiceFunction(&LocalConnectToServiceFunction);
}
