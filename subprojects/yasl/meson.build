project(
    'yasl',
    'c',
    meson_version: '>=1.6.0',
    version: '0.2.0',
    default_options: [
        'c_std=c23',
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
        deps += [x11, xi, x11_xkb, xext]
        if xrandr.found()
            deps += [xrandr]
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
    has_egl = egl_dep.found() and (not has_wayland or wayland_egl_dep.found())
    if has_egl
        deps += egl_dep
        if has_wayland
            deps += wayland_egl_dep
        endif
    endif
    vulkan_dep = dependency('vulkan', required: false)
    has_vulkan = vulkan_dep.found()
    m_dep = cc.find_library('m', required: false)
    rt_dep = cc.find_library('rt', required: false)
    deps += [m_dep, rt_dep]
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
        dependency('appleframeworks', modules: ['AppKit', 'Metal', 'QuartzCore', 'UserNotifications']),
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

add_project_arguments(
    '-Werror=deprecated-declarations',
    '-DYASL_HAS_EGL=' + (has_egl ? '1' : '0'),
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

yasl_link_args = []
yasl_link_depends = []
if system == 'linux'
    # Compiler visibility does not hide linker-generated symbols.
    yasl_link_depends = files('assets/yasl.map')
    yasl_link_args = ['-Wl,--version-script=' + yasl_link_depends[0].full_path()]
endif

yasl_lib = both_libraries(
    'yasl',
    sources,
    version: meson.project_version(),
    # Until 1.0, every minor release may break the ABI.
    soversion: '0.2',
    include_directories: inc,
    dependencies: deps,
    gnu_symbol_visibility: 'hidden',
    link_args: yasl_link_args,
    link_depends: yasl_link_depends,
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
pkgconfig.generate(yasl_lib, name: 'yasl',
    description: 'Desktop surfaces for application-owned rendering',
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

if has_egl
    game_example = executable(
        'yasl-game',
        'examples/game.c',
        dependencies: yasl_dep,
    )
endif

subdir('tests')
