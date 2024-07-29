/*
 * Copyright 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VirtGpuKumquatSync.h"

#include <unistd.h>

namespace gfxstream {

VirtGpuKumquatSyncHelper::VirtGpuKumquatSyncHelper() {}

int VirtGpuKumquatSyncHelper::wait(int syncFd, int timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    // So far, syncfds are EventFd in the Kumquat layer. This may change
    uint64_t count = 1;
    ssize_t bytes_read = read(syncFd, &count, sizeof(count));

    if (bytes_read < 0) {
        return bytes_read;
    }

    // A successful read decrements the eventfd's counter to zero.  In
    // case the eventfd is waited on again, or a dup is waited on, we
    // have to write to the eventfd for the next read.
    ssize_t bytes_written = write(syncFd, &count, sizeof(count));
    if (bytes_written < 0) {
        return bytes_written;
    }

    return 0;
}

int VirtGpuKumquatSyncHelper::dup(int syncFd) { return ::dup(syncFd); }

int VirtGpuKumquatSyncHelper::close(int syncFd) { return ::close(syncFd); }

SyncHelper* createPlatformSyncHelper() { return new VirtGpuKumquatSyncHelper(); }

}  // namespace gfxstream
