# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

inc_platform = include_directories('include')

subdir('kumquat')
subdir('linux')

files_lib_platform = files(
  'VirtGpu.cpp',
)

lib_platform = static_library(
   'platform',
   files_lib_platform,
   cpp_args: gfxstream_guest_args,
   include_directories: [inc_platform, inc_android_compat],
   link_with: [lib_platform_kumquat, lib_platform_linux]
)
