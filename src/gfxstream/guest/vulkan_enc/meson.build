# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

gfxstream_vk_entrypoints = custom_target(
  'gfxstream_vk_entrypoints',
  input : [vk_entrypoints_gen, vk_api_xml],
  output : ['gfxstream_vk_entrypoints.h', 'gfxstream_vk_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--proto', '--weak',
    '--out-h', '@OUTPUT0@', '--out-c', '@OUTPUT1@', '--prefix', 'gfxstream_vk',
    '--beta', with_vulkan_beta.to_string()
  ],
)

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
  'gfxstream_vk_private.cpp',
)
