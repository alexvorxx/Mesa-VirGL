# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_platform = files(
  '../VirtGpu.cpp',
  'VirtGpuKumquatDevice.cpp',
  'VirtGpuKumquatBlobMapping.cpp',
  'VirtGpuKumquatBlob.cpp',
  'VirtGpuKumquatSync.cpp',
)

lib_platform = static_library(
   'platform',
   files_lib_platform,
   cpp_args: gfxstream_guest_args,
   include_directories: [inc_platform, inc_android_compat],
   dependencies: virtgpu_kumquat_dep,
)
