project(
    'wsap-framework',
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
project_languages = ['c']
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

isa = get_option('isa')
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

dev_args = [
    '-DWSAP_DATADIR="' + (get_option('prefix') / get_option('datadir')) + '"',
]
if get_option('dev_paths')
    dev_args += [
        '-DWSAP_DEV_FONTS_DIR="' + (meson.project_source_root() / 'assets' / 'fonts') + '"',
    ]
endif
# A parent project passes its own theme source directory, because this project
# ships no theme files. The macro then points at the parent source tree.
if get_option('dev_themes_dir') != ''
    dev_args += [
        '-DWSAP_DEV_THEMES_DIR="' + get_option('dev_themes_dir') + '"',
    ]
endif
add_project_arguments(dev_args, language: project_languages)

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

# A release binary must not hold source paths in __FILE__. The prefix map is
# a global argument so that it also applies to the subprojects. A build
# directory outside the source tree passes the source path relative to the
# build directory, so the map covers that spelling too.
# Meson rejects a global argument in a subproject. The parent project owns
# them.
if get_option('buildtype') == 'release' and not meson.is_subproject()
    fs = import('fs')
    source_from_build = fs.relative_to(meson.project_source_root(),
                                       meson.project_build_root())
    prefix_maps = ['-fmacro-prefix-map=' + meson.project_source_root() + '=.']
    if source_from_build != '.'
        prefix_maps += ['-fmacro-prefix-map=' + source_from_build + '=.']
    endif
    prefix_map_args = cc.get_supported_arguments(prefix_maps)
    if system == 'darwin'
        prefix_map_args += objc.get_supported_arguments(prefix_maps)
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
    # The framework build ships its archives. LTO bytecode holds the source
    # and build paths of this machine, and it needs the same compiler to read
    # it. A global argument also reaches the subprojects, whose archives
    # install next to these.
    # A parent app build keeps LTO. Only a standalone SDK build drops it, and
    # Meson rejects a global argument in a subproject.
    if not meson.is_subproject()
        no_lto = cc.get_supported_arguments('-fno-lto')
        add_global_arguments(no_lto, language: project_languages)
        add_global_link_arguments(cc.get_supported_link_arguments('-fno-lto'), language: 'c')
    endif
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
    add_project_arguments('-DWSAP_USE_FREETYPE', language: project_languages)
endif

inc = include_directories('src')
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
# An app that builds the framework as a subproject links yasl statically and
# installs nothing of it. The standalone SDK installs yasl for its consumers.
yasl_options = [
    'default_library=static',
    'install=' + (not meson.is_subproject()).to_string(),
]
if backend == 'x11'
    yasl_options += ['wayland=disabled', 'x11=enabled']
endif
yasl_dep = dependency(
    'yasl',
    fallback: 'yasl',
    default_options: yasl_options,
)

if backend == 'wayland'
    backend_deps = [dl_dep]
else
    backend_deps = []
endif

# The Vulkan backend needs Vulkan 1.3 and glslc, which compiles its shaders.
# `auto` builds the CPU-only renderer when one of them is missing.
has_vulkan = false
vulkan_opt = get_option('vulkan_render')
if vulkan_opt != 'disabled' and system != 'darwin'
    vulkan_dep = dependency('vulkan', version: '>=1.3.0', required: vulkan_opt == 'enabled')
    glslc = find_program('glslc', native: true, required: vulkan_opt == 'enabled')
    if vulkan_dep.found() and glslc.found()
        has_vulkan = true
        backend_deps += [vulkan_dep.partial_dependency(compile_args: true, includes: true), dl_dep]
    endif
endif
lib_deps = base_deps + font_deps + backend_deps
core_deps = lib_deps + [yasl_dep]

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

# Meson rejects a second default test setup in one build. A parent project
# defines its own setups.
if not meson.is_subproject()
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
endif

subdir('src')
if get_option('benchmarks')
    subdir('benchmarks')
endif
if get_option('tests')
    subdir('tests')
endif

# The license texts of the third-party code and data that the framework ships.
# An app that builds the framework as a subproject reads this variable and
# installs the files with its own license, because its binary holds the code.
third_party_licenses = files(
    'LICENSES/OFL-1.1.txt',
    'LICENSES/Unicode-3.0.txt',
    'LICENSES/NOTICE.md',
    'LICENSES/accesskit-c-MIT.txt',
    'LICENSES/accesskit-c-APACHE-2.0.txt',
    'LICENSES/accesskit-c-BSD-3-Clause-chromium.txt',
    'LICENSES/accesskit-c-LGPL-2.1.txt',
)
if not meson.is_subproject()
    license_dir = get_option('datadir') / 'licenses' / meson.project_name()
    install_data('LICENSE', install_dir: license_dir)
    install_data(
        'assets/fonts/LICENSE',
        install_dir: license_dir,
    )
    install_data(third_party_licenses, install_dir: license_dir)
endif

summary(
    {
        'Platform': system,
        'Backend': backend,
        'Font backend': font_backend,
        'ISA': isa,
        'Hardening': get_option('hardening'),
        'Tests': get_option('tests'),
        'Benchmarks': get_option('benchmarks'),
        'Vulkan': has_vulkan,
        'Dev paths': get_option('dev_paths'),
    },
    section: 'wsap-framework',
)
