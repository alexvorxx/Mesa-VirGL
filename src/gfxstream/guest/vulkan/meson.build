# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

vk_api_xml = files('vk.xml')
vk_icd_gen = files('vk_icd_gen.py')

files_lib_vulkan_cereal = files(
  'goldfish_vulkan.cpp',
)

lib_vulkan_cereal = shared_library(
   'vulkan_cereal',
   files_lib_vulkan_cereal,
   cpp_args: cpp_args,
   include_directories: [inc_android_emu, inc_android_compat, inc_opengl_system,
			 inc_host, inc_opengl_codec, inc_render_enc,
                         inc_vulkan_enc],
   link_with: [lib_android_compat, lib_emu_android_base, lib_stream,
               lib_vulkan_enc],
   install: true,
)

cereal_icd = custom_target(
  'cereal_icd',
  input : [vk_icd_gen, vk_api_xml],
  output : 'cereal_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT0@',
    '--api-version', '1.0', '--xml', '@INPUT1@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'),
                             'libvulkan_cereal.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)
