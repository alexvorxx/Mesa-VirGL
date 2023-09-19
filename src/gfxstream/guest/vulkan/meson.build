# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

files_lib_vulkan_gfxstream = files(
  'gfxstream_vk_device.cpp',
  'gfxstream_vk_cmd.cpp',
  'gfxstream_vk_wsi.cpp'
)

lib_vulkan_gfxstream = shared_library(
   'vulkan_gfxstream',
   files_lib_vulkan_gfxstream + files_lib_vulkan_enc + gfxstream_vk_entrypoints,
   cpp_args: cpp_args,
   include_directories: [inc_vulkan_headers, inc_opengl_headers, inc_android_emu,
                         inc_android_compat, inc_opengl_system, inc_guest_iostream,
                         inc_opengl_codec, inc_render_enc, inc_vulkan_enc, inc_platform,
                         inc_goldfish_address_space, inc_system, inc_include, inc_src,
                         inc_platform, inc_codec_common],
   link_with: [lib_android_compat, lib_emu_android_base, lib_stream,
               libvulkan_wsi, lib_platform],
   link_args: [vulkan_icd_link_args, ld_args_bsymbolic, ld_args_gc_sections],
   link_depends: vulkan_icd_link_depends,
   dependencies: [dependency('libdrm'), idep_vulkan_wsi_headers,
                  idep_vulkan_runtime_headers, idep_vulkan_runtime,
                  idep_vulkan_util_headers, idep_vulkan_wsi],
   install: true,
)

gfxstream_icd = custom_target(
  'gfxstream_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'gfxstream_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.1', '--xml', '@INPUT1@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'),
                             'libvulkan_gfxstream.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)
