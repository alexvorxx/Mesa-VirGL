# Copyright © 2024 Igalia S.L.
# SPDX-License-Identifier: MIT

_compiler_rs_sources = [
  'bitset.rs',
  'cfg.rs',
]

_compiler_rs_sources = structured_sources([
  # lib.rs has to go first
  'lib.rs',
  _compiler_rs_sources,
])

_libcompiler_rs = static_library(
  'compiler',
  _compiler_rs_sources,
  gnu_symbol_visibility : 'hidden',
  rust_abi : 'rust',
)

idep_compiler_rs = declare_dependency(
  link_with : _libcompiler_rs,
)
