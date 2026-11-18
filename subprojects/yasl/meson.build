project(
    'yasl',
    'c',
    meson_version: '>=1.6.0',
    version: '0.2.0',
    default_options: [
        'c_std=c17',
        'warning_level=2',
        'b_lto=false',
    ],
)

cc = meson.get_compiler('c')
system = host_machine.system()

inc = include_directories('include', 'src')

deps = []
sources = []

has_wayland = false
has_x11 = false
has_dbus = false
has_egl = false
has_wayland_egl = false
has_vulkan = false
has_cocoa = system == 'darwin'

if system == 'linux'
    wl_client = dependency('wayland-client', required: get_option('wayland'))
    wl_cursor = dependency('wayland-cursor', required: get_option('wayland'))
    xkbcommon = dependency('xkbcommon', required: get_option('wayland'))
    has_wayland = wl_client.found() and wl_cursor.found() and xkbcommon.found()
    x11 = dependency('x11', required: get_option('x11'))
    xi = dependency('xi', required: get_option('x11'))
    x11_xkb = dependency('xkbcommon', required: get_option('x11'))
    xext = dependency('xext', required: get_option('x11'))
    xrandr = dependency('xrandr', required: false)
    has_x11 = x11.found() and xi.found() and x11_xkb.found() and xext.found()
    if has_x11
        deps += [
            x11.partial_dependency(compile_args: true, includes: true),
            xi.partial_dependency(compile_args: true, includes: true),
            x11_xkb,
            xext.partial_dependency(compile_args: true, includes: true),
        ]
        if xrandr.found()
            deps += [xrandr.partial_dependency(compile_args: true, includes: true)]
            add_project_arguments('-DYASL_HAS_XRANDR=1', language: 'c')
        endif
    endif
    dbus = dependency('dbus-1', required: get_option('dbus'))
    has_dbus = dbus.found()
    if has_dbus
        deps += [dbus]
    endif
    egl_dep = dependency('egl', required: false)
    wayland_egl_dep = dependency('wayland-egl', required: false)
    # X11 EGL needs only the EGL headers. The Wayland EGL window needs
    # <wayland-egl.h> too, so a missing wayland-egl disables EGL on Wayland alone.
    has_egl = egl_dep.found()
    has_wayland_egl = has_wayland and egl_dep.found() and wayland_egl_dep.found()
    if has_egl
        deps += egl_dep.partial_dependency(compile_args: true, includes: true)
        if has_wayland_egl
            deps += wayland_egl_dep.partial_dependency(compile_args: true, includes: true)
        endif
    endif
    vulkan_dep = dependency('vulkan', required: false)
    has_vulkan = vulkan_dep.found()
    m_dep = cc.find_library('m', required: false)
    rt_dep = cc.find_library('rt', required: false)
    dl_dep = cc.find_library('dl', required: false)
    deps += [m_dep, rt_dep, dl_dep]
    if has_wayland
        deps += [wl_client, wl_cursor, xkbcommon]
    endif
elif system == 'darwin'
    add_languages('objc', native: false)
    if not cc.compiles('''
        #if !defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) || __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 140000
        #error YASL requires macOS 14 or newer
        #endif
        int main(void) { return 0; }
        ''', name: 'macOS deployment target is at least 14')
        error('Cocoa requires macOS 14 or newer. Set MACOSX_DEPLOYMENT_TARGET=14.0 or a newer target before configuring.')
    endif
    deps += [
        dependency('appleframeworks', modules: ['AppKit', 'IOSurface', 'Metal', 'QuartzCore', 'UserNotifications']),
    ]
else
    error('YASL supports Linux and macOS.')
endif

if has_cocoa
    vulkan_dep = dependency('vulkan', required: false)
    has_vulkan = vulkan_dep.found()
endif

if not has_wayland and not has_x11 and not has_cocoa
    error('Enable at least one window backend.')
endif

# The GL example and the GL integration test need GL on every built backend.
has_gl_surfaces = has_egl and (not has_wayland or has_wayland_egl)

add_project_arguments(
    '-Werror=deprecated-declarations',
    '-DYASL_HAS_EGL=' + (has_egl ? '1' : '0'),
    '-DYASL_HAS_WAYLAND_EGL=' + (has_wayland_egl ? '1' : '0'),
    '-DYASL_HAS_VULKAN=' + (has_vulkan ? '1' : '0'),
    '-DYASL_HAS_WAYLAND=' + (has_wayland ? '1' : '0'),
    '-DYASL_HAS_X11=' + (has_x11 ? '1' : '0'),
    '-DYASL_HAS_COCOA=' + (has_cocoa ? '1' : '0'),
    '-DYASL_HAS_DBUS=' + (has_dbus ? '1' : '0'),
    language: has_cocoa ? ['c', 'objc'] : ['c'],
)
if has_cocoa
    add_project_arguments('-fblocks', language: 'objc')
endif

subdir('src')

yasl_lib = both_libraries(
    'yasl',
    sources,
    version: meson.project_version(),
    # Until 1.0, every minor release may break the ABI.
    soversion: '0.2',
    include_directories: inc,
    dependencies: deps,
    gnu_symbol_visibility: 'hidden',
    install: true,
)

yasl_dep = declare_dependency(
    link_with: yasl_lib.get_static_lib(),
    include_directories: include_directories('include'),
    dependencies: deps,
)
meson.override_dependency('yasl', yasl_dep)

install_headers('include/yasl/yasl.h', 'include/yasl/keycodes.h', subdir: 'yasl')
if has_vulkan
    install_headers('include/yasl/vulkan.h', subdir: 'yasl')
endif
pkgconfig = import('pkgconfig')
# yasl calls no Vulkan entry point. The application supplies the loader and
# links it. A consumer of <yasl/vulkan.h> still needs <vulkan/vulkan.h>, so the
# private pkg-config requirement supplies the Vulkan Cflags.
vulkan_pc_requirement = (has_vulkan and vulkan_dep.type_name() == 'pkgconfig') ? 'vulkan' : []
pkgconfig.generate(yasl_lib, name: 'yasl',
    description: 'Desktop surfaces for application-owned rendering',
    requires_private: vulkan_pc_requirement,
    libraries_private: deps,
)


hello_example = executable(
    'yasl-hello',
    'examples/hello.c',
    include_directories: inc,
    link_with: yasl_lib.get_shared_lib(),
)

interactive_example = executable(
    'yasl-example',
    'examples/simple_window.c',
    include_directories: inc,
    link_with: yasl_lib.get_shared_lib(),
    dependencies: deps,
)

tablet_example = executable(
    'yasl-tablet',
    'examples/tablet.c',
    dependencies: yasl_dep,
)

notepad_example = executable(
    'yasl-notepad',
    'examples/notepad.c',
    dependencies: yasl_dep,
)

if has_gl_surfaces
    game_example = executable(
        'yasl-game',
        'examples/game.c',
        dependencies: yasl_dep,
    )
endif

# meson test --setup NAME needs the setup in each project with tests. The
# parent project defines the same setups.
sanitizer_common = 'halt_on_error=1:abort_on_error=1:print_summary=1'
asan_options = sanitizer_common + ':detect_stack_use_after_return=1' + \
    ':strict_string_checks=1:check_initialization_order=1' + \
    ':strict_init_order=1:detect_invalid_pointer_pairs=2'
if system == 'linux'
    asan_options += ':detect_leaks=1'
elif system == 'darwin'
    asan_options += ':suppressions=' + meson.current_source_dir() / 'tests' / 'asan.supp'
endif
add_test_setup(
    'asan',
    env: {
        'ASAN_OPTIONS': asan_options,
        'UBSAN_OPTIONS': sanitizer_common + ':print_stacktrace=1',
        'LSAN_OPTIONS': 'report_objects=1',
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    },
)
add_test_setup(
    'tsan',
    env: {
        'TSAN_OPTIONS': sanitizer_common + ':history_size=7:second_deadlock_stack=1',
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    },
)
add_test_setup(
    'msan',
    env: {
        'MSAN_OPTIONS': sanitizer_common + ':print_stats=1',
        'LD_PRELOAD': '',
        'DYLD_INSERT_LIBRARIES': '',
    },
    exclude_suites: ['extlib'],
)

subdir('tests')
