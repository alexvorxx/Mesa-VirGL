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

SyncHelper* createPlatformSyncHelper() { return new LinuxSyncHelper(); }

}  // namespace gfxstream
