libgbm_dri = shared_library(
  'dri_gbm',
  files('gbm_dri.c', 'gbm_driint.h'),
  include_directories : incs_gbm,
  link_args : [ld_args_gc_sections],
  link_with : [libloader, libgallium_dri],
  dependencies : [deps_gbm, dep_dl, dep_thread, dep_libdrm, idep_mesautil, idep_xmlconfig],
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir: join_paths(get_option('libdir'), 'gbm'),
  name_prefix : '',
)
