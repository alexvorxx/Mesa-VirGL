# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_vulkan_enc = files(
  'CommandBufferStagingStream.cpp',
  'DescriptorSetVirtualization.cpp',
  'HostVisibleMemoryVirtualization.cpp',
  'ResourceTracker.cpp',
  'Resources.cpp',
  'Validation.cpp',
  'VkEncoder.cpp',
  'VulkanHandleMapping.cpp',
  'VulkanStreamGuest.cpp',
  'func_table.cpp',
  'goldfish_vk_counting_guest.cpp',
  'goldfish_vk_counting_guest.h',
  'goldfish_vk_deepcopy_guest.cpp',
  'goldfish_vk_extension_structs_guest.cpp',
  'goldfish_vk_marshaling_guest.cpp',
  'goldfish_vk_reserved_marshaling_guest.cpp',
  'goldfish_vk_transform_guest.cpp',
)

lib_vulkan_enc = static_library(
   'vulkan_enc',
   files_lib_vulkan_enc,
   cpp_args: cpp_args,
   include_directories: [inc_android_emu, inc_host, inc_android_compat,
                         inc_opengl_codec, inc_render_enc, inc_system,
			 inc_goldfish_address_space, inc_platform],
   link_with: [lib_platform],
   dependencies: dependency('libdrm'),
)
