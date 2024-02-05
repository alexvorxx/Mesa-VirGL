# Copyright 2022 Android Open Source Project
# SPDX-License-Identifier: MIT

inc_platform = include_directories('include')
if with_guest_test
  subdir('rutabaga')
else
  subdir('linux')
  subdir('stub')
endif
