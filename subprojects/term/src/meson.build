subdir('osc')

wsap_vtcore_soversion = meson.project_version().split('.')[0]

wsap_vtcore_sources = files(
  'core.c',
  'csi/fast.c',
  'escape.c',
  'hist/view.c',
  'input.c',
  'isa.c',
  'palette.c',
  'scan.c',
  'screen.c',
  'selection.c',
  'snapshot.c',
  'term/state.c',
  'text/run.c',
  'text/text.c',
) + wsap_vtcore_osc_src

wsap_vtcore_lib = both_libraries('wsap_vtcore', wsap_vtcore_sources,
  include_directories: wsap_vtcore_private_inc,
  dependencies: [m_dep, thread_dep],
  gnu_symbol_visibility: 'hidden',
  # Clang resolves sanitizer hooks from the final executable.
  override_options: cc.get_id() == 'clang' and get_option('b_sanitize') != 'none'
                    ? ['b_lundef=false'] : [],
  version: meson.project_version(),
  soversion: wsap_vtcore_soversion,
  install: true
)

if get_option('fuzzing')
  wsap_vtcore_fuzz_lib = static_library('wsap_vtcore-fuzz', wsap_vtcore_sources,
    include_directories: wsap_vtcore_private_inc,
    dependencies: [m_dep, thread_dep],
    c_args: wsap_vtcore_fuzz_c_args,
    override_options: ['optimization=1', 'b_lto=false'],
    build_by_default: false,
  )
  wsap_vtcore_fuzz_dep = declare_dependency(
    include_directories: wsap_vtcore_inc,
    link_with: wsap_vtcore_fuzz_lib,
    dependencies: [m_dep, thread_dep],
  )
endif

