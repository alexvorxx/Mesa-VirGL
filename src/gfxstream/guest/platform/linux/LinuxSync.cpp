/*
 * Copyright 2023 Google
 * SPDX-License-Identifier: MIT
 */
#include "LinuxSync.h"

#if defined(__ANDROID__)
#include <sync/sync.h>
#endif
#include <unistd.h>

namespace gfxstream {

LinuxSyncHelper::LinuxSyncHelper() {}

int LinuxSyncHelper::wait(int syncFd, int timeoutMilliseconds) {
#if defined(__ANDROID__)
    return sync_wait(syncFd, timeoutMilliseconds);
#else
    (void)syncFd;
    (void)timeoutMilliseconds;
    return -1;
#endif
}

int LinuxSyncHelper::dup(int syncFd) { return ::dup(syncFd); }

int LinuxSyncHelper::close(int syncFd) { return ::close(syncFd); }

SyncHelper* osCreateSyncHelper() { return new LinuxSyncHelper(); }

}  // namespace gfxstream
