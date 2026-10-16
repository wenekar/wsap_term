subdir('osc')

# The SONAME carries the major version. An ABI break bumps it, so an old
# binary keeps the old library.
wsap_vtcore_soversion = meson.project_version().split('.')[0]

# The x86-64 SIMD kernels pick their level at run time (see simd.h), so the
# library needs no ISA flags. -march in c_args sets the lowest level.
wsap_vtcore_lib = both_libraries('wsap_vtcore',
  'core.c',
  'escape.c',
  'fastcsi.c',
  'gstore.c',
  'hist.c',
  'hist_view.c',
  'input.c',
  'isa.c',
  'palette.c',
  'scan.c',
  'screen.c',
  'selection.c',
  'snapshot.c',
  'term.c',
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

