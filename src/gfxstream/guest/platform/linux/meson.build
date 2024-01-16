# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_platform = files(
  '../VirtGpu.cpp',
  'LinuxVirtGpuDevice.cpp',
  'LinuxVirtGpuBlobMapping.cpp',
  'LinuxVirtGpuBlob.cpp',
  'LinuxSync.cpp',
)

lib_platform = static_library(
   'platform',
   files_lib_platform,
   cpp_args: gfxstream_guest_args,
   include_directories: [inc_platform, inc_android_compat],
   dependencies: drm_dep,
)
