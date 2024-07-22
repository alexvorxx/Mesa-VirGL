# Copyright © 2017 Intel Corporation
# SPDX-License-Identifier: MIT

inc_loader_dri3 = include_directories('.')

if with_platform_x11 and with_dri3
  libloader_dri3_helper = static_library(
    'loader_dri3_helper',
    'loader_dri3_helper.c',
    gnu_symbol_visibility : 'hidden',
    include_directories : [inc_include, inc_src, inc_loader, inc_gallium, inc_mesa, inc_st_dri],
    dependencies : [
      idep_mesautil,
      dep_libdrm, dep_xcb_dri3, dep_xcb_present, dep_xcb_sync, dep_xshmfence,
      dep_xcb_xfixes,
    ],
    build_by_default : false,
  )
else
  libloader_dri3_helper = []
endif
