# Copyright © 2024 Igalia S.L.
# SPDX-License-Identifier: MIT

_compiler_rs_sources = [
  'as_slice.rs',
  'bitset.rs',
  'cfg.rs',
  'nir.rs',
]

bindgen_version = find_program('bindgen').version()

if bindgen_version == 'unknown'
  error('Failed to detect bindgen version. If you are using bindgen 0.69.0, ' +
        'please either update to 0.69.1 or downgrade to 0.68.1. You can ' +
        'install the latest version for your user with ' +
        '`cargo install bindgen-cli`.')
endif

if bindgen_version.version_compare('< 0.65')
  error('NAK requires bindgen 0.65 or newer. If your distribution does not ' +
        'ship a recent enough version, you can install the latest version ' +
        'for your user with `cargo install bindgen-cli`.')
endif

_compiler_binding_types = [
  'exec_list',
  'exec_node',
  'float_controls',
  'gc_ctx',
  'gl_access_qualifier',
  'gl_frag_result',
  'gl_interp_mode',
  'gl_shader_stage',
  'gl_subgroup_size',
  'gl_system_value',
  'gl_tess_spacing',
  'gl_varying_slot',
  'gl_vert_attrib',
  'glsl_type',
  'nir_.*',
  'mesa_scope',
  'mesa_prim',
  'pipe_shader_type',
  'shader_info',
  'tess_primitive_mode',
]

_compiler_bindgen_args = [
  '--raw-line', '#![allow(non_camel_case_types)]',
  '--raw-line', '#![allow(non_snake_case)]',
  '--raw-line', '#![allow(non_upper_case_globals)]',
  '--allowlist-var', 'nir_.*_infos',
  '--allowlist-function', 'glsl_.*',
  '--allowlist-function', '_mesa_shader_stage_to_string',
  '--allowlist-function', 'nir_.*',
  '--no-prepend-enum-name',
]

foreach type : _compiler_binding_types
  _compiler_bindgen_args += ['--allowlist-type', type]
endforeach

_compiler_bindings_rs = rust.bindgen(
  input : ['bindings.h'],
  output : 'bindings.rs',
  c_args : [
    pre_args,
  ],
  args : _compiler_bindgen_args,
  dependencies : [
    idep_nir_headers,
  ],
)

compiler_rs_bindgen_blocklist = []
foreach type : _compiler_binding_types
  compiler_rs_bindgen_blocklist += ['--blocklist-type', type]
endforeach

_compiler_rs_sources = structured_sources([
  # lib.rs has to go first
  'lib.rs',
  _compiler_bindings_rs,
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

dep_syn = dependency('syn',
  version : '>= 2.0.15',
  fallback : ['syn', 'dep_syn'],
  required : true,
)

_libcompiler_proc_rs = static_library(
  'compiler_proc',
  'proc/lib.rs',
  gnu_symbol_visibility : 'hidden',
  dependencies : [dep_syn],
  rust_abi : 'rust',
  native : true,
)

idep_compiler_proc_rs = declare_dependency(
  link_with : _libcompiler_proc_rs,
)
