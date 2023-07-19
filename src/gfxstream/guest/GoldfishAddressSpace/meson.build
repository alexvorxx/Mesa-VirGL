# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

inc_goldfish_address_space = include_directories('include')

files_lib_goldfish_address_space = files(
  'goldfish_address_space.cpp',
)

lib_goldfish_address_space = static_library(
   'goldfish_address_space',
   files_lib_goldfish_address_space,
   cpp_args: cpp_args,
   include_directories: [inc_android_compat,
                         inc_goldfish_address_space,
                         inc_system, inc_platform],
   link_with: [lib_platform],
   dependencies: drm_dep
)
