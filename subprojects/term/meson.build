project('wsap_vtcore', 'c',
  version: '0.1.0',
  license: 'MIT',
  license_files: 'LICENSE',
  meson_version: '>=1.1.0',
  default_options: [
    'c_std=c11',
    'buildtype=release',
    'optimization=3',
    'b_lto=true',
    'warning_level=3',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)
thread_dep = dependency('threads')
if get_option('arch_flag') != ''
  add_project_arguments(get_option('arch_flag'), language: 'c')
endif

wsap_vtcore_inc = include_directories('include')
wsap_vtcore_private_inc = include_directories('src', 'include/wsap_vtcore')

subdir('src')

wsap_vtcore_dep = declare_dependency(
  include_directories: wsap_vtcore_inc,
  link_with: wsap_vtcore_lib.get_static_lib(),
  dependencies: [m_dep, thread_dep],
)
meson.override_dependency('wsap_vtcore', wsap_vtcore_dep)

wsap_vtcore_internal_dep = declare_dependency(
  include_directories: [wsap_vtcore_inc, wsap_vtcore_private_inc],
  link_with: wsap_vtcore_lib.get_static_lib(),
  dependencies: [m_dep, thread_dep],
)

wsap_vtcore_test_dep = declare_dependency(
  include_directories: include_directories('tests/helpers'),
  dependencies: wsap_vtcore_dep,
)
meson.override_dependency('wsap_vtcore-test', wsap_vtcore_test_dep)

install_headers(
  'include/wsap_vtcore/wsap_vtcore.h',
  'include/wsap_vtcore/wsap_vtcore_input.h',
  'include/wsap_vtcore/wsap_vtcore_types.h',
  subdir: 'wsap_vtcore',
)

install_headers(
  'include/wsap_vtcore/unicode/grapheme.h',
  'include/wsap_vtcore/unicode/unicode.h',
  'include/wsap_vtcore/unicode/unicode_data.inc',
  'include/wsap_vtcore/unicode/utf8.h',
  'include/wsap_vtcore/unicode/width.h',
  subdir: 'wsap_vtcore/unicode',
)

pkg = import('pkgconfig')
pkg.generate(
  wsap_vtcore_lib.get_shared_lib(),
  name: 'wsap_vtcore',
  description: 'Headless terminal emulator core library',
  version: meson.project_version(),
  subdirs: ['.'],
)

if get_option('tests')
  subdir('tests')
  subdir('benchmarks')
endif

