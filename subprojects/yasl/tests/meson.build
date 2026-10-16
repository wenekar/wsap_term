python = find_program('python3', native: true)
# yasl/vulkan.h includes <vulkan/vulkan.h>, so the header path must come with the library.
shared_dep = declare_dependency(link_with: yasl_lib.get_shared_lib(),
    include_directories: include_directories('../include'),
    dependencies: has_vulkan ? [vulkan_dep] : [])
foreach kind : ['static', 'shared']
    dep = kind == 'static' ? yasl_dep : shared_dep
    public_test = executable('yasl-' + kind + '-api-test', 'test_lifecycle.c', dependencies: dep)
    test(kind + '-api', public_test)
endforeach
pre_init_test = executable('yasl-public-api-test', 'common/test_public_api.c', dependencies: yasl_dep)
test('public-api', pre_init_test)
foreach std : ['c11', 'c17']
    header_test = executable('yasl-header-' + std + '-test', 'test_header_c11.c',
        include_directories: include_directories('../include'),
        c_args: ['-pedantic-errors'], override_options: ['c_std=' + std])
    test('header-' + std, header_test)
endforeach
if add_languages('cpp', native: false, required: false)
    cpp_api = executable('yasl-cpp-api-test', 'test_api.cpp', dependencies: shared_dep,
        cpp_args: ['-pedantic-errors',
            '-DYASL_HAS_VULKAN=' + (has_vulkan ? '1' : '0'),
            '-DYASL_HAS_WAYLAND=' + (has_wayland ? '1' : '0'),
            '-DYASL_HAS_X11=' + (has_x11 ? '1' : '0'),
            '-DYASL_HAS_COCOA=' + (has_cocoa ? '1' : '0')], override_options: ['cpp_std=c++11'])
    test('cpp-api', cpp_api)
endif
headers = files('../include/yasl/yasl.h')
if has_vulkan
    headers += files('../include/yasl/vulkan.h')
endif
nm = find_program('nm')
export_args = ['--platform=' + system]
if get_option('b_sanitize').contains('address')
    export_args += ['--allow-export=__start_asan_globals', '--allow-export=__stop_asan_globals']
endif
test('public-exports', python, args: [files('helpers/check_exports.py')] + export_args +
    [nm, yasl_lib.get_shared_lib(), headers])

foreach unit : ['contracts', 'utf8', 'timers', 'loop', 'selection', 'events', 'gl_options']
    exe = executable('yasl-' + unit + '-test', 'common/test_' + unit + '.c',
        include_directories: inc, dependencies: yasl_dep)
    test(unit, exe, is_parallel: not has_cocoa or unit != 'timers')
endforeach
keyboard_sources = files('common/test_keyboard.c')
if not has_wayland
    keyboard_sources += files('../src/platform/wayland/keymap.c')
endif
if not has_x11
    keyboard_sources += files('../src/platform/x11/keymap.c')
endif
if not has_cocoa
    keyboard_sources += files('../src/platform/cocoa/keymap.c')
endif
keyboard = executable('yasl-keyboard-test', keyboard_sources, include_directories: inc, dependencies: yasl_dep)
test('keyboard', keyboard)
alloc = executable('yasl-input-alloc-test', ['common/test_input_alloc.c', '../src/common/input.c'],
    include_directories: inc, c_args: ['-Dmalloc=yasl_private_test_malloc'])
test('input-allocation-failure', alloc)
if system == 'linux'
    desktop = executable('yasl-desktop-test', 'common/test_desktop.c', include_directories: inc, dependencies: yasl_dep)
    dbus_run = find_program('dbus-run-session', required: false)
    if has_dbus and dbus_run.found()
        test('desktop', dbus_run, args: ['--', desktop])
    else
        test('desktop', desktop)
    endif
endif
if has_wayland
    foreach unit : ['state', 'clipboard', 'keyboard', 'input', 'tablet', 'cursor', 'relative_pointer', 'app_input', 'popup_positioner']
        exe = executable('yasl-' + unit + '-wayland-test', 'wayland/test_' + unit + '.c',
            include_directories: inc, dependencies: yasl_dep)
        test(unit + '-wayland', exe)
    endforeach
endif
if has_x11
    foreach unit : ['state', 'tablet']
        exe = executable('yasl-' + unit + '-x11-test', 'x11/test_' + unit + '.c',
            include_directories: inc, dependencies: yasl_dep)
        test(unit + '-x11', exe)
    endforeach
    if get_option('x11_tablet_tests')
        xorg = find_program('/usr/lib/Xorg', '/usr/lib/xorg/Xorg', 'Xorg')
        native_tablet = executable('yasl-tablet-x11-native-test', 'x11/test_tablet_native.c',
            include_directories: inc, dependencies: yasl_dep)
        test('tablet-x11-native', python, args: [files('helpers/run_x11_tablet.py'), xorg, native_tablet], timeout: 30)
    endif
endif
x11_input_args = []
if has_x11 and get_option('integration_tests')
    x11_input_args += [executable('yasl-input-x11-test', 'x11/test_input.c',
        include_directories: inc, dependencies: yasl_dep)]
endif
if get_option('integration_tests')
    display_test_sources = has_cocoa ? files('cocoa/display.m') : []
    integration = executable('yasl-surface-integration', ['common/test_surface_integration.c'] + display_test_sources,
        include_directories: inc, dependencies: yasl_dep)
    gl_integration = '-'
    if has_egl
        gl_integration = executable('yasl-gl-integration', 'common/test_gl.c',
            dependencies: yasl_dep)
    endif
    vulkan_integration = '-'
    if has_vulkan
        vulkan_integration = executable('yasl-vulkan-integration', 'common/test_vulkan.c', dependencies: yasl_dep)
    endif
    if system == 'linux'
        weston = find_program('weston')
        xwayland = find_program('Xwayland')
        test('surface-backends', python, args: [files('helpers/run_surfaces.py'), weston, xwayland,
            integration, hello_example, tablet_example, gl_integration, vulkan_integration, notepad_example] +
            x11_input_args, timeout: 120)
        if has_wayland and has_egl
            reconnect = executable('yasl-reconnect-test', 'wayland/test_reconnect.c', dependencies: yasl_dep,
                c_args: ['-DTEST_VULKAN=' + (has_vulkan ? '1' : '0')])
            test('reconnect-wayland', python, args: [files('helpers/run_reconnect.py'), weston, reconnect], timeout: 60)
        endif
    elif has_cocoa
        test('surface-cocoa', integration, args: ['cocoa'], timeout: 30, is_parallel: false)
        foreach unit : ['native', 'ownership', 'keyboard', 'tablet', 'cursor']
            extension = unit == 'native' ? '.c' : '.m'
            exe = executable('yasl-' + unit + '-cocoa-test', 'cocoa/test_' + unit + extension,
                include_directories: inc, dependencies: yasl_dep)
            test(unit + '-cocoa', exe, timeout: 30, is_parallel: false)
        endforeach
    endif
endif
