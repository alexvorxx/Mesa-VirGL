# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_platform_linux = files(
  'LinuxVirtGpuDevice.cpp',
  'LinuxVirtGpuBlobMapping.cpp',
  'LinuxVirtGpuBlob.cpp',
  'LinuxSync.cpp',
)

lib_platform_linux = static_library(
   'platform_linux',
   files_lib_platform_linux,
   cpp_args: gfxstream_guest_args,
   include_directories: [inc_platform, inc_android_compat],
   dependencies: drm_dep,
)
