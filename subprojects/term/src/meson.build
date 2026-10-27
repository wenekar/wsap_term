subdir('osc')

wsap_vtcore_soversion = meson.project_version().split('.')[0]

wsap_vtcore_lib = both_libraries('wsap_vtcore',
  'core.c',
  'escape.c',
  'fastcsi.c',
  'hist_view.c',
  'input.c',
  'isa.c',
  'palette.c',
  'scan.c',
  'screen.c',
  'selection.c',
  'snapshot.c',
  'term_state.c',
  'text.c',
  'text_run.c',
  wsap_vtcore_osc_src,
  include_directories: wsap_vtcore_private_inc,
  dependencies: [m_dep, thread_dep],
  gnu_symbol_visibility: 'hidden',
  version: meson.project_version(),
  soversion: wsap_vtcore_soversion,
  install: true
)

