/*
 * Copyright 2023 Google
 * SPDX-License-Identifier: MIT
 */

#include "LinuxSync.h"

#include <unistd.h>

#include "util/libsync.h"

namespace gfxstream {

LinuxSyncHelper::LinuxSyncHelper() {}

int LinuxSyncHelper::wait(int syncFd, int timeoutMilliseconds) {
    return sync_wait(syncFd, timeoutMilliseconds);
}

int LinuxSyncHelper::dup(int syncFd) { return ::dup(syncFd); }

int LinuxSyncHelper::close(int syncFd) { return ::close(syncFd); }

SyncHelper* osCreateSyncHelper() { return new LinuxSyncHelper(); }

}  // namespace gfxstream
