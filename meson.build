project(
    'wsap_term',
    'c',
    version: '0.1.0',
    license: 'MIT',
    license_files: 'LICENSE',
    meson_version: '>=1.12.0',
    default_options: [
        'c_std=c23',
        'buildtype=release',
        'b_lto=true',
        'b_ndebug=false',
        'warning_level=2',
    ],
)

cc = meson.get_compiler('c')
python = find_program('python3', native: true, version: '>=3.10')
system = host_machine.system()
if system not in ['linux', 'darwin']
    error('Supported operating systems are Linux and macOS')
endif
if system == 'darwin'
    add_languages('objc', native: false)
    objc = meson.get_compiler('objc')
    # Targets with Objective-C code link through the Objective-C compiler.
    # LTO then uses the LLVM of that compiler. Apple clang and upstream
    # clang write bitcode that the other LLVM cannot always read. Meson
    # picks `cc` for C and `clang` for Objective-C, so a Homebrew clang in
    # PATH gives a mixed toolchain.
    if cc.get_id() != objc.get_id() or cc.version() != objc.version()
        error('The C compiler (@0@ @1@) and the Objective-C compiler (@2@ @3@) differ. Set both to one toolchain, for example CC=cc OBJC=cc or CC=clang OBJC=clang.'.format(
            cc.get_id(), cc.version(), objc.get_id(), objc.version()))
    endif
    add_project_arguments('-fblocks', language: 'objc')
endif
project_languages = ['c']
if system == 'darwin'
    project_languages += ['objc']
endif
backend = get_option('window_backend')
if backend == 'auto'
    backend = system == 'linux' ? 'wayland' : 'metal'
endif
if backend != 'metal' and system != 'linux'
    error('The @0@ backend requires Linux'.format(backend))
endif
if backend == 'metal' and system != 'darwin'
    error('The Metal backend is macOS-only; Linux builds use Wayland or X11')
endif
if get_option('integration_tests') and (backend != 'wayland' or not get_option('tests'))
    error('Integration tests require tests=true and the Wayland backend')
endif
if get_option('kitty_tests') and not get_option('tests')
    error('Kitty tests require tests=true')
endif
isa = get_option('isa')
# A local build keeps the native target for speed. A cross or package build
# passes an explicit ISA.
if isa == 'auto'
    isa = meson.is_cross_build() ? '' : 'native'
endif
if meson.is_cross_build() and isa == 'native'
    error('Cross builds must set -Disa= or an explicit target ISA')
endif
if isa != ''
    arch_arg = '-march=' + isa
    if not cc.has_argument(arch_arg)
        if isa == 'native' and cc.has_argument('-mcpu=native')
            arch_arg = '-mcpu=native'
        else
            error('Compiler does not support ' + arch_arg)
        endif
    endif
    if system == 'darwin' and not objc.has_argument(arch_arg)
        error('Objective-C compiler does not support ' + arch_arg)
    endif
    add_project_arguments(arch_arg, language: project_languages)
    if cc.has_link_argument(arch_arg)
        add_project_link_arguments(arch_arg, language: 'c')
    endif
    if system == 'darwin' and objc.has_link_argument(arch_arg)
        add_project_link_arguments(arch_arg, language: 'objc')
    endif
endif

dev_paths = get_option('dev_paths')

dev_args = [
    '-DWSAP_DATADIR="' + (get_option('prefix') / get_option('datadir')) + '"',
]
if dev_paths
    dev_args += [
        '-DWSAP_DEV_THEMES_DIR="' + (meson.project_source_root() / 'assets' / 'themes') + '"',
        '-DWSAP_DEV_FONTS_DIR="' + (meson.project_source_root() / 'assets' / 'fonts') + '"',
    ]
endif
add_project_arguments(dev_args, language: project_languages)

dev_terminfo_dir = meson.project_build_root() / 'terminfo'
dev_terminfo_targets = []
tic = find_program('tic', native: true, required: false)
if dev_paths and tic.found()
    dev_terminfo_targets += custom_target(
        'dev-terminfo',
        input: files('assets/terminfo/wsap_term.terminfo'),
        output: 'dev-terminfo.stamp',
        command: [find_program('sh', native: true), files('assets/terminfo/install-terminfo.sh')],
        env: {
            'WSAP_TERMINFO_SRC': meson.project_source_root() / 'assets' / 'terminfo' / 'wsap_term.terminfo',
            'WSAP_TERMINFO_DEST': dev_terminfo_dir,
            'WSAP_TERMINFO_STALE': '',
        },
        capture: true,
        depend_files: files('assets/terminfo/install-terminfo.sh'),
        build_by_default: true,
    )
    add_project_arguments(
        '-DWSAP_DEV_TERMINFO_DIR="' + dev_terminfo_dir + '"',
        language: project_languages,
    )
endif

add_project_arguments(
    cc.get_supported_arguments(
        '-Werror=implicit-function-declaration',
        '-Werror=incompatible-pointer-types',
        '-Werror=return-type',
        '-ffunction-sections',
        '-fdata-sections',
    ),
    language: project_languages,
)
add_project_link_arguments(
    cc.get_supported_link_arguments('-Wl,--gc-sections'),
    language: project_languages,
)

# A release binary must not hold absolute source paths in __FILE__. The prefix
# map is a global argument so that it also applies to the subprojects. The
# system root mapping keeps the system headers readable.
if get_option('buildtype') == 'release'
    prefix_map_args = cc.get_supported_arguments(
        '-fmacro-prefix-map=' + meson.project_source_root() + '=.',
    )
    if system == 'darwin'
        prefix_map_args += objc.get_supported_arguments(
            '-fmacro-prefix-map=' + meson.project_source_root() + '=.',
        )
    endif
    if prefix_map_args.length() > 0
        add_global_arguments(prefix_map_args, language: project_languages)
    endif
endif

if get_option('b_lto')
    add_project_arguments(
        cc.get_supported_arguments('-fno-ipa-cp-clone'),
        language: project_languages,
    )
    # Fat LTO objects hold the machine code next to the LTO bytecode. An
    # installed archive then links with clang and with another GCC version,
    # which cannot read the GCC LTO bytecode. The project's own link still
    # uses LTO.
    add_project_arguments(
        cc.get_supported_arguments('-ffat-lto-objects'),
        language: project_languages,
    )
endif

if get_option('hardening') and get_option('b_sanitize') == 'none'
    hardening_args = cc.get_supported_arguments(
        '-fstack-protector-strong',
    )
    if system == 'linux'
        hardening_args += cc.get_supported_arguments(
            '-fstack-clash-protection',
            '-fcf-protection=full',
        )
    endif
    if get_option('optimization') not in ['0', 'g']
        hardening_args += cc.get_supported_arguments(
            '-U_FORTIFY_SOURCE',
            '-D_FORTIFY_SOURCE=3',
        )
    endif
    add_project_arguments(hardening_args, language: project_languages)
    add_project_link_arguments(
        cc.get_supported_link_arguments('-Wl,-z,relro', '-Wl,-z,now'),
        language: project_languages,
    )
endif

font_backend = get_option('font_backend')
if font_backend == 'auto'
    if system == 'darwin'
        font_backend = 'coretext'
    else
        font_backend = 'freetype'
    endif
endif

if font_backend == 'coretext' and system != 'darwin'
    error('CoreText font backend is only supported on macOS')
endif
if font_backend == 'freetype' and system == 'darwin'
    add_project_arguments('-DWSAP_USE_FREETYPE', language: ['c', 'objc'])
endif

inc_dirs = ['src']
inc = include_directories(inc_dirs)
harfbuzz_dep = dependency('harfbuzz', version: '>=7.0.0')
if font_backend == 'coretext'
    font_deps = [harfbuzz_dep]
else
    font_deps = [
        dependency('freetype2'),
        dependency('fontconfig'),
        harfbuzz_dep,
    ]
endif
dl_dep = cc.find_library('dl', required: system == 'linux')
m_dep = cc.find_library('m')
thread_dep = dependency('threads')
yyjson_dep = dependency('yyjson', version: '>=0.12.0')
accesskit_dep = dependency(
    'accesskit-c-0.23',
    version: ['>=0.23.2', '<0.24.0'],
    static: true,
    fallback: ['accesskit-c', 'accesskit_c_dep'],
    default_options: ['default_library=static'],
)
platform_deps = []
if system == 'darwin'
    platform_deps += [dependency('appleframeworks', modules: ['AppKit', 'CoreFoundation', 'Foundation', 'QuartzCore', 'Metal', 'UserNotifications', 'CoreText', 'CoreGraphics'])]
endif
base_deps = [thread_dep, m_dep, yyjson_dep] + platform_deps

# The UI framework is the vendored subproject. It owns the base, input, paint,
# font, ui, theme, render, and ui_yasl libraries and the yasl window system.
# The options follow this build so that both halves agree.
wsap_fw = subproject(
    'wsap-framework',
    default_options: [
        'tests=false',
        'benchmarks=false',
        'window_backend=' + backend,
        'font_backend=' + font_backend,
        'vulkan_render=' + get_option('vulkan_render'),
        'isa=' + isa,
        'hardening=' + get_option('hardening').to_string(),
        'dev_paths=' + dev_paths.to_string(),
        'dev_themes_dir=' + (dev_paths
            ? (meson.project_source_root() / 'assets' / 'themes')
            : ''),
    ],
)
wsap_base_dep = dependency('wsap-base')
wsap_input_dep = dependency('wsap-input')
wsap_paint_dep = dependency('wsap-paint')
wsap_font_dep = dependency('wsap-font')
wsap_ui_dep = dependency('wsap-ui')
wsap_theme_dep = dependency('wsap-theme')
wsap_render_dep = dependency('wsap-render')
wsap_ui_yasl_dep = dependency('wsap-ui-yasl')
yasl_dep = dependency('yasl')
term_dep = dependency(
    'wsap_vtcore',
    fallback: 'term',
    default_options: [
        'default_library=static',
        'arch_flag=' + (isa != '' ? arch_arg : ''),
        'b_ndebug=' + get_option('b_ndebug'),
        'tests=' + get_option('tests').to_string(),
        'kitty=' + (get_option('kitty_tests') ? 'enabled' : 'auto'),
        'write_fuzz_seeds=' + get_option('write_fuzz_seeds').to_string(),
    ],
)
term_test_dep = dependency('wsap_vtcore-test', fallback: 'term')

if backend == 'wayland'
    backend_deps = [
        dl_dep,
    ]
else
    backend_deps = []
endif

# The framework detects Vulkan and glslc once. The terminal reads that result.
has_vulkan = wsap_fw.get_variable('has_vulkan')
if has_vulkan
    vulkan_dep = dependency('vulkan', version: '>=1.3.0')
    glslc = find_program('glslc', native: true)
    backend_deps += [vulkan_dep.partial_dependency(compile_args: true, includes: true), dl_dep]
endif
lib_deps = base_deps + font_deps + backend_deps
wsap_deps = [
    wsap_base_dep, wsap_input_dep, wsap_paint_dep, wsap_font_dep,
    wsap_ui_dep, wsap_theme_dep, wsap_render_dep, wsap_ui_yasl_dep,
]
core_deps = wsap_deps + lib_deps + [yasl_dep, term_dep]
deps = core_deps

sanitizer_common = 'halt_on_error=1:abort_on_error=1:print_summary=1'
ubsan_strict = sanitizer_common + ':print_stacktrace=1'
asan_strict = sanitizer_common + ':detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:strict_init_order=1:detect_invalid_pointer_pairs=2'
lsan_strict = 'report_objects=1'
if system == 'linux'
    asan_strict += ':detect_leaks=1'
    lsan_strict += ':suppressions=' + meson.current_source_dir() / 'tests' / 'lsan.supp'
elif system == 'darwin'
    asan_strict += ':suppressions=' + meson.current_source_dir() / 'tests' / 'asan.supp'
endif
tsan_strict = sanitizer_common + ':history_size=7:second_deadlock_stack=1'
if system == 'linux'
    tsan_strict += ':suppressions=' + meson.current_source_dir() / 'tests' / 'tsan.supp'
endif
msan_strict = sanitizer_common + ':print_stats=1'

default_env = {}
b_sanitize = get_option('b_sanitize')
if b_sanitize.contains('address')
    default_env += {
        'ASAN_OPTIONS': asan_strict,
        'UBSAN_OPTIONS': ubsan_strict,
        'LSAN_OPTIONS': lsan_strict,
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    }
elif b_sanitize.contains('thread')
    default_env += {
        'TSAN_OPTIONS': tsan_strict,
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    }
elif b_sanitize.contains('memory')
    default_env += {
        'MSAN_OPTIONS': msan_strict,
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    }
elif b_sanitize.contains('undefined')
    default_env += {'UBSAN_OPTIONS': ubsan_strict}
endif

add_test_setup(
    'default',
    is_default: true,
    env: default_env,
)
add_test_setup(
    'asan',
    env: {
        'ASAN_OPTIONS': asan_strict,
        'UBSAN_OPTIONS': ubsan_strict,
        'LSAN_OPTIONS': lsan_strict,
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    },
)
add_test_setup(
    'tsan',
    env: {
        'TSAN_OPTIONS': tsan_strict,
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    },
)
add_test_setup(
    'msan',
    env: {
        'MSAN_OPTIONS': msan_strict,
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    },
    exclude_suites: ['extlib'],
)

exe_install_dir = get_option('bindir')
if system == 'darwin'
    fs = import('fs')
    app_dir = get_option('macos_app_dir')
    if app_dir == ''
        home = fs.expanduser('~')
        prefix = get_option('prefix')
        per_user = prefix == home or prefix.startswith(home + '/')
        app_dir = per_user ? home / 'Applications' : '/Applications'
    endif
    app_contents = app_dir / 'wsap_term.app' / 'Contents'
    exe_install_dir = app_contents / 'MacOS'
endif

subdir('src')
subdir('bin')
if get_option('benchmarks')
    subdir('benchmarks')
endif
if get_option('tests')
    subdir('tests')
endif
if system == 'linux' and get_option('install_desktop')
    install_data(
        'assets/icons/wsap-term.svg',
        install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
    )
    install_data(
        'assets/wsap_term.desktop',
        install_dir: get_option('datadir') / 'applications',
    )
endif
if system == 'darwin'
    configure_file(
        input: 'assets/macos/Info.plist.in',
        output: 'Info.plist',
        configuration: {'VERSION': meson.project_version()},
        install: true,
        install_dir: app_contents,
    )
    install_data(
        'assets/icons/wsap-term.icns',
        rename: 'AppIcon.icns',
        install_dir: app_contents / 'Resources',
    )
    install_subdir(
        'assets/fonts',
        install_dir: app_contents / 'Resources' / 'fonts',
        strip_directory: true,
    )
    cli_launcher = configure_file(
        input: 'assets/macos/wsap_term.in',
        output: 'wsap_term-cli.sh',
        configuration: {'APP_EXECUTABLE': exe_install_dir / 'wsap_term'},
    )
    install_data(
        cli_launcher,
        rename: 'wsap_term',
        install_dir: get_option('bindir'),
        install_mode: 'rwxr-xr-x',
    )
endif
install_data(
    'assets/terminfo/wsap_term.terminfo',
    install_dir: get_option('datadir') / 'wsap_term' / 'terminfo',
)
install_subdir(
    'assets/fonts',
    install_dir: get_option('datadir') / 'wsap_term' / 'fonts',
    strip_directory: true,
)
if get_option('install_shell')
    install_data(
        'assets/shell/wsap-ssh',
        'assets/terminfo/install-terminfo.sh',
        install_dir: get_option('bindir'),
        install_mode: 'rwxr-xr-x',
    )
    install_data(
        'assets/shell/wsap-ssh.fish',
        'assets/shell/wsap-ssh.sh',
        install_dir: get_option('datadir') / 'wsap_term' / 'shell',
    )
endif
install_subdir(
    'assets/themes',
    install_dir: get_option('datadir') / 'wsap_term' / 'themes',
    strip_directory: true,
)
install_data(
    'LICENSE',
    install_dir: get_option('datadir') / 'licenses' / 'wsap_term',
)
if system == 'darwin'
    meson.add_install_script(
        python,
        files('tools/install/main.py'),
        '--datadir',
        get_option('datadir'),
        '--bindir',
        get_option('bindir'),
        '--macos-app-dir',
        app_dir,
    )
else
    meson.add_install_script(
        python,
        files('tools/install/main.py'),
        '--datadir',
        get_option('datadir'),
        '--bindir',
        get_option('bindir'),
    )
endif
pkg_format = get_option('package_format')
if pkg_format == 'auto'
    pkg_format = system == 'darwin' ? 'app' : 'tar'
endif

meson.add_dist_script(
    python,
    files('tools/deploy/main.py'),
    '--format',
    pkg_format,
    '--version',
    meson.project_version(),
    '--license',
    meson.project_license()[0],
)
summary(
    {
        'Platform': system,
        'Backend': backend,
        'Framework': 'subproject (wsap-framework)',
        'Vulkan': has_vulkan,
        'ISA': isa,
        'Hardening': get_option('hardening'),
        'Tests': get_option('tests'),
        'Benchmarks': get_option('benchmarks'),
        'Integration tests': get_option('integration_tests'),
        'Kitty tests': subproject('term').get_variable('term_kitty_tests', false),
        'Dev paths': dev_paths,
        'Package format': pkg_format,
    },
    section: 'wsap_term',
)
