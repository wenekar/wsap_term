fs = import('fs')
python = find_program('python3', native: true, version: '>=3.10')
bash = find_program('bash', native: true)

test_c_args = ['-UNDEBUG']

test_env = environment()
test_env.set('LD_PRELOAD', '')
test_env.set('DYLD_INSERT_LIBRARIES', '')
nm = find_program('nm', native: true)
if cc.get_id() == 'clang'
  llvm_nm = run_command(cc.cmd_array() + ['-print-prog-name=llvm-nm'],
                        check: false).stdout().strip()
  if fs.is_absolute(llvm_nm) and fs.is_file(llvm_nm)
    nm = find_program(llvm_nm)
  else
    nm = find_program('llvm-nm-18', 'llvm-nm', 'nm', native: true)
  endif
endif
test_env.set('NM', nm.full_path())
test_env.set('CC', ' '.join(cc.cmd_array()))

wsap_vtcore_tests = [
  ['unicode-test', files('unicode/unicode_test.c'), [], []],
  ['term-input-test', files('input_test.c'), [], []],
  ['term-test', files('term_test.c'), [thread_dep], ['isa']],
  ['write-fuzz-test', files('write_fuzz_test.c'), [], ['isa']],
  ['spec-test', files('spec_test.c'), [], []],
  ['model-test', files('model_test.c', 'model.c'), [], ['isa']],
  ['model-row-test', files('model_row_test.c', 'model.c'), [], []],
  ['gstore-test', files('gstore_test.c'), [], []],
  ['hist-test', files('hist_test.c'), [], []],
  ['history-text-test', files('history_text_test.c'), [thread_dep], []],
  ['snapshot-scroll-arena-test', files('snapshot_scroll_arena_test.c'), [], []],
  ['view-test', files('view_test.c'), [], []],
  ['resize-property-test', files('resize_property_test.c'), [], []],
  ['dcs-test', files('dcs_test.c'), [], []],
  ['osc-test', files('osc_test.c'), [], []],
  ['api-args-test', files('api_args_test.c'), [], []],
  ['public-helpers-test', files('public_helpers_test.c'), [], []],
  ['snapshot-lifetime-test', files('snapshot_lifetime_test.c'), [], []],
]
test_helpers_inc = include_directories('helpers')

wsap_vtcore_test_exes = {}
foreach entry : wsap_vtcore_tests
  exe = executable(entry[0], entry[1],
    c_args: test_c_args,
    include_directories: test_helpers_inc,
    dependencies: [wsap_vtcore_internal_dep] + entry[2],
  )
  wsap_vtcore_test_exes += {entry[0]: exe}
  test(entry[0], exe,
    args: entry[0] == 'write-fuzz-test' ? [get_option('write_fuzz_seeds').to_string()] : [],
    env: test_env,
    suite: ['unit'] + entry[3],
    timeout: entry[0] == 'model-test' ? 600 : 180,
  )
endforeach

if host_machine.cpu_family() == 'x86_64' and cc.get_argument_syntax() == 'gcc' and \
    cc.get_define('__AVX2__', args: get_option('arch_flag') != '' ? [get_option('arch_flag')] : []) == ''
  foreach level : ['base', 'avx2']
    level_env = test_env
    level_env.set('WSAP_VTCORE_ISA', level)
    foreach name : ['term-test', 'spec-test', 'model-test', 'view-test', 'term-input-test',
                    'hist-test', 'history-text-test', 'snapshot-lifetime-test',
                    'public-helpers-test', 'write-fuzz-test']
      test(name + '-' + level, wsap_vtcore_test_exes[name], env: level_env,
        args: name == 'write-fuzz-test' ? [get_option('write_fuzz_seeds').to_string()] : [],
        suite: ['unit', 'isa'], timeout: name == 'model-test' ? 600 : 180)
    endforeach
  endforeach
endif

wsap_vtcore_amalgamation = custom_target(
  'term-amalgamation',
  output: ['wsap_vtcore.c', 'wsap_vtcore.h'],
  depfile: 'term-amalgamation.d',
  command: [python, files('../tools/amalgamate.py'),
            meson.project_source_root(), '@OUTDIR@', '@DEPFILE@'],
)
wsap_vtcore_amalgamation_lib = static_library(
  'term-amalgamation',
  wsap_vtcore_amalgamation[0],
  dependencies: [m_dep, thread_dep],
  override_options: ['c_std=c11', 'warning_level=3', 'werror=true'],
  build_by_default: false,
)
foreach entry : wsap_vtcore_tests
  if entry[0] in ['term-test', 'spec-test', 'view-test', 'dcs-test', 'osc-test',
                  'history-text-test', 'term-input-test', 'model-test', 'model-row-test',
                  'resize-property-test', 'api-args-test', 'public-helpers-test',
                  'snapshot-lifetime-test']
    test(entry[0] + '-amalgamation',
      executable(entry[0] + '-amalgamation', entry[1],
        c_args: test_c_args,
        include_directories: [wsap_vtcore_inc, test_helpers_inc],
        link_with: wsap_vtcore_amalgamation_lib,
        dependencies: [m_dep, thread_dep] + entry[2],
        build_by_default: false),
      env: test_env, suite: ['unit', 'amalgamation'],
      timeout: entry[0] == 'model-test' ? 600 : 180)
  endif
endforeach

test('term-symbols-test', bash,
  args: [files('symbols_test.sh'), wsap_vtcore_lib.get_static_lib(),
         meson.project_source_root() / 'include' / 'wsap_vtcore',
         wsap_vtcore_lib.get_shared_lib()],
  env: test_env,
  suite: 'unit',
)
test('term-header-test', bash,
  args: [files('header_test.sh'), meson.project_source_root()] + cc.cmd_array(),
  env: test_env,
  suite: 'unit',
)
if cc.has_link_argument('-Wl,--wrap=realloc') and cc.has_link_argument('-Wl,--wrap=malloc')
  snapshot_failure_test = executable('snapshot-failure-test', 'snapshot_failure_test.c',
    c_args: test_c_args,
    link_args: ['-Wl,--wrap=realloc', '-Wl,--wrap=malloc'],
    dependencies: [wsap_vtcore_internal_dep],
  )
  test('snapshot-failure-test', snapshot_failure_test,
    env: test_env,
    suite: 'unit',
  )
endif
if cc.has_link_argument('-Wl,--wrap=malloc') and cc.has_link_argument('-Wl,--wrap=free')
  alloc_failure_test = executable('alloc-failure-test', 'alloc_failure_test.c',
    c_args: test_c_args,
    link_args: ['-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc',
                '-Wl,--wrap=free'],
    dependencies: [wsap_vtcore_internal_dep],
  )
  test('alloc-failure-test', alloc_failure_test,
    env: test_env,
    suite: 'unit',
    timeout: 300,
  )
endif
if cc.has_link_argument('-Wl,--wrap=realloc') and cc.has_link_argument('-Wl,--wrap=calloc')
  resize_failure_test = executable('term-resize-failure-test', 'resize_failure_test.c',
    c_args: test_c_args,
    include_directories: test_helpers_inc,
    link_args: ['-Wl,--wrap=realloc', '-Wl,--wrap=calloc'],
    dependencies: [wsap_vtcore_internal_dep],
  )
  test('term-resize-failure-test', resize_failure_test,
    env: test_env,
    suite: 'unit',
  )
endif

version_test = executable('version-test', 'version_test.c',
  c_args: test_c_args + ['-DWSAP_VTCORE_TEST_PROJECT_VERSION="@0@"'.format(meson.project_version())],
  dependencies: [wsap_vtcore_internal_dep],
)
test('version-test', version_test,
  env: test_env,
  suite: 'unit',
)
test('term-cross-test', bash,
  args: [files('cross_test.sh'), meson.project_source_root()],
  suite: 'portability',
  timeout: 600,
)
test('term-package-test', bash,
  args: [files('package_test.sh'), meson.project_source_root()],
  env: test_env,
  suite: 'packaging',
  timeout: 300,
)

if get_option('fuzzing')
  subdir('fuzz')
endif

kitty_tests = get_option('kitty').require(
  host_machine.system() == 'linux' and get_option('b_sanitize') == 'none',
  error_message: 'The Kitty tests need an unsanitized Linux build',
)
wsap_vtcore_kitty_tests = kitty_tests.allowed()
if wsap_vtcore_kitty_tests
  kitty_bridge = shared_library('kitty_bridge', 'extern/kitty/screen_bridge.c',
    dependencies: wsap_vtcore_internal_dep,
  )
  kitty_env = environment()
  kitty_env.set('WSAP_KITTY_LIB', kitty_bridge.full_path())
  test('kitty-test', python,
    args: files('extern/kitty/parser.py'),
    depends: kitty_bridge,
    env: kitty_env,
    suite: ['unit', 'kitty'],
    is_parallel: false,
  )
endif
