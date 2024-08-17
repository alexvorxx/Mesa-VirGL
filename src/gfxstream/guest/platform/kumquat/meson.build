# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

if with_kumquat
  files_lib_platform_kumquat = files(
    'VirtGpuKumquatDevice.cpp',
    'VirtGpuKumquatBlobMapping.cpp',
    'VirtGpuKumquatBlob.cpp',
    'VirtGpuKumquatSync.cpp',
  )
else
  files_lib_platform_kumquat = files('VirtGpuKumquatStub.cpp')
endif

lib_platform_kumquat = static_library(
   'platform_kumquat',
   files_lib_platform_kumquat,
   cpp_args: gfxstream_guest_args,
   include_directories: [inc_platform, inc_src, inc_android_compat],
   dependencies: virtgpu_kumquat_dep,
)
