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
    (void)syncFd;
    (void)timeoutMilliseconds;
    return -1;
}

int VirtGpuKumquatSyncHelper::dup(int syncFd) { return ::dup(syncFd); }

int VirtGpuKumquatSyncHelper::close(int syncFd) { return ::close(syncFd); }

SyncHelper* createPlatformSyncHelper() { return new VirtGpuKumquatSyncHelper(); }

}  // namespace gfxstream
