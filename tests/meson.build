test_c_args = ['-UNDEBUG']
ui_test_deps = [m_dep, thread_dep, yyjson_dep, icu_dep]
test_cases = [
    ['input-test', files('input/input_test.c'), input_lib, [m_dep]],
    ['glyph-intern-test', files('font/glyph/intern_test.c'), font_lib, [m_dep]],
    ['selection-test', files('terminal/selection_test.c'), terminal_lib, deps],
    ['color-test', files('util/color_test.c'), base_lib, [m_dep]],
    ['json-test', files('util/json_test.c'), base_lib, [yyjson_dep]],
    ['geom-test', files('util/geom_test.c'), base_lib, [m_dep]],
    ['proc-test', files('util/proc_test.c'), base_lib, [thread_dep, dl_dep]],
    ['paste-test', files('session/write_queue_test.c'), [session_lib, input_lib], deps],
    ['session-cwd-test', files('session/cwd_test.c'), session_lib, deps],
    ['session-exit-order-test', files('session/exit_order_test.c'), session_lib, deps],
    ['app-reload-test', files('app/reload_test.c'), app_lib, deps],
    ['drop-test', files('terminal/drop_test.c'), app_lib, deps],
    ['terminal-kind-test', files('terminal/kind_test.c'), app_lib, deps],
    ['terminal-bell-close-test', files('terminal/bell_close_test.c'), app_lib, deps],
    ['stats-test', files('ui/yasl/stats_test.c'), ui_yasl_lib, deps],
    ['pty-fast-test', files('pty/pty_test.c'), pty_lib, [thread_dep, dl_dep]],
    ['pty-destroy-test', files('pty/pty_destroy_test.c'), pty_lib, [thread_dep, dl_dep]],
    ['settings-test', files('app/config_test.c'), app_lib, deps],
    ['thumbnail-test', files('app/thumbnail_test.c'), app_lib, deps],
    ['settings-window-test', files('app/config/window_test.c'), app_lib, deps],
    ['window-ui-hosts-test', files('ui/yasl/ui_hosts_test.c'), app_lib, deps],
    ['window-ui-tree-test', files('ui/yasl/ui_tree_test.c'), app_lib, deps],
    ['window-debug-server-test', files('ui/yasl/debug_server_test.c'), app_lib, deps],
    ['render-test', files('render/render_test.c', 'stubs/render_backend.c'), app_lib, deps],
    ['resize-alloc-test', files('ui/yasl/resize_alloc_test.c'), app_lib, deps],
    ['window-ui-input-test', files('ui/yasl/ui_input_test.c'), app_lib, deps],
    font_backend == 'coretext'
        ? ['font-coretext-test', files('font/coretext_test.m'), font_lib, font_deps + [m_dep] + platform_deps]
        : ['font-test', files('font/font_test.c'), font_lib, font_deps + [m_dep]],
    ['anim-test', files('ui/anim_test.c', 'ui/stubs/scene_paint_deps.c'), app_lib, deps],
    ['ui-scene-test', files('paint/scene_test.c'), ui_lib, ui_test_deps],
    ['ui-widget-test', files('ui/widget_test.c'), ui_lib, ui_test_deps],
    ['ui-accessibility-test', files('ui/accessibility_test.c'), ui_lib, ui_test_deps],
    ['ui-focus-test', files('ui/focus_test.c'), ui_lib, ui_test_deps],
    ['ui-dispatch-test', files('ui/dispatch_test.c'), ui_lib, ui_test_deps],
    ['ui-box-test', files('ui/box_test.c'), ui_lib, ui_test_deps],
    ['ui-typed-widgets-test', files('ui/typed_widgets_test.c'), ui_lib, ui_test_deps],
    ['ui-style-context-test', files('ui/style_context_test.c'), ui_lib, ui_test_deps],
    ['ui-part-handle-test', files('ui/part_handle_test.c'), ui_lib, ui_test_deps],
    ['ui-paint-test', files('ui/paint_test.c'), ui_lib, ui_test_deps],
    ['ui-surface-test', files('ui/surface_test.c'), ui_lib, ui_test_deps],
    ['ui-core-contract-test', files('ui/core_contract_test.c'), ui_lib, ui_test_deps],
    ['ui-model-contract-test', files('ui/model_contract_test.c'), ui_lib, ui_test_deps],
    ['ui-scrollview-test', files('ui/widgets/scrollview_test.c'), ui_lib, ui_test_deps],
    ['ui-scene-text-test', files('paint/scene_text_test.c', 'ui/stubs/scene_paint_deps.c'), app_lib, deps],
    ['ui-tabbar-test', files('ui/widgets/tabbar_test.c'), ui_lib, ui_test_deps],
    ['ui-menu-test', files('ui/widgets/menu_test.c'), ui_lib, ui_test_deps],
    ['anchor-test', files('ui/anchor_test.c'), ui_lib, ui_test_deps],
    ['ui-palette-test', files('ui/widgets/palette_test.c'), ui_lib, ui_test_deps],
    ['ui-list-test', files('ui/widgets/list_test.c'), ui_lib, ui_test_deps],
    ['ui-input-test', files('ui/widgets/input_test.c'), ui_lib, ui_test_deps],
    ['ui-label-test', files('ui/widgets/label_test.c'), ui_lib, ui_test_deps],
    ['ui-dropdown-test', files('ui/widgets/dropdown_test.c'), ui_lib, ui_test_deps],
    ['ui-tab-preview-test', files('ui/widgets/tab_preview_test.c'), ui_lib, ui_test_deps],
    ['ui-acceptance-test', files('ui/acceptance_test.c'), ui_lib, ui_test_deps],
    ['ui-debug-test', files('ui/debug_test.c'), ui_lib, ui_test_deps],
    ['look-test', files('theme/look_test.c'), theme_lib, deps],
    ['css-parser-test', files('ui/css/parser_test.c'), ui_lib, ui_test_deps],
    ['css-clipping-test', files('ui/css/clipping_test.c', 'ui/stubs/scene_paint_deps.c'), app_lib, deps],
    ['css-selector-test', files('ui/css/selector_test.c'), ui_lib, ui_test_deps],
    ['css-index-test', files('ui/css/index_test.c'), ui_lib, ui_test_deps],
]

if backend == 'metal'
    test_cases += [
        ['metal-render-test', files('render/metal_render_test.m'), app_lib, deps],
    ]
endif

if backend == 'wayland'
    wayland_scanner = find_program('wayland-scanner', native: true)
endif

test_common_deps = [yasl_dep, term_dep, term_test_dep]
test_env = environment()
test_env.set('LD_PRELOAD', '')
test_env.set('DYLD_INSERT_LIBRARIES', '')
test_env.set('WSAP_NO_PRELOAD', '1')
bash = find_program('bash', native: true)

extlib_tests = [
    'font-test',
    'font-coretext-test',
    'render-test',
    'resize-alloc-test',
]

msan_safe_tests = [
    'ui-scene-test',
    'ui-widget-test',
    'ui-focus-test',
    'ui-box-test',
    'ui-typed-widgets-test',
    'ui-style-context-test',
    'ui-paint-test',
    'ui-surface-test',
    'ui-core-contract-test',
    'ui-model-contract-test',
    'ui-scrollview-test',
    'ui-tabbar-test',
    'ui-menu-test',
    'anchor-test',
    'ui-palette-test',
    'ui-list-test',
    'ui-input-test',
    'ui-acceptance-test',
    'css-parser-test',
    'css-clipping-test',
    'css-selector-test',
]

standalone_tests = [
    'pty-fast-test', 'font-test',
    'font-coretext-test', 'render-test', 'resize-alloc-test',
    'anim-test', 'ui-scene-text-test', 'css-clipping-test',
    'metal-render-test',
]
test_groups = {}
test_executables = {}
foreach entry : test_cases
    if not get_option('test_link_groups') or entry[0] in standalone_tests
        test_executables += {entry[0]: executable(
            entry[0],
            entry[1],
            link_with: entry[2],
            c_args: test_c_args,
            include_directories: inc,
            dependencies: entry[3] + test_common_deps,
        )}
    else
        libraries = [entry[2]]
        if entry[0] == 'paste-test'
            group = session_lib.name()
        else
            group = entry[2].name()
        endif
        symbol = 'test_' + entry[0].underscorify()
        obj = static_library(
            entry[0] + '-entry',
            entry[1],
            c_args: test_c_args + ['-Dmain=' + symbol],
            include_directories: inc,
            dependencies: entry[3] + test_common_deps,
            pic: false,
            build_by_default: false,
        )
        data = test_groups.get(group, {
            'objects': [], 'libraries': [], 'deps': [],
            'declarations': '', 'entries': '', 'names': [],
        })
        data += {
            'objects': data['objects'] + obj.extract_all_objects(recursive: false),
            'libraries': data['libraries'] + libraries,
            'deps': data['deps'] + entry[3],
            'declarations': data['declarations'] + 'int ' + symbol + '(void);\n',
            'entries': data['entries'] + '    {"' + entry[0] + '", ' + symbol + '},\n',
            'names': data['names'] + [entry[0]],
        }
        test_groups += {group: data}
    endif
endforeach

foreach group, data : test_groups
    config = configuration_data()
    config.set('DECLARATIONS', data['declarations'])
    config.set('ENTRIES', data['entries'])
    runner_source = configure_file(
        input: 'helpers/runner.c.in',
        output: group + '-runner.c',
        configuration: config,
    )
    runner = executable(
        group + '-tests',
        runner_source,
        objects: data['objects'],
        link_with: data['libraries'],
        dependencies: data['deps'] + test_common_deps,
    )
    foreach name : data['names']
        test_executables += {name: custom_target(
            name,
            input: runner,
            output: name,
            command: [python, files('helpers/link_test.py'), '@INPUT@', '@OUTPUT@'],
            build_by_default: true,
        )}
    endforeach
endforeach

foreach entry : test_cases
    exe = test_executables[entry[0]]
    suites = ['unit']
    if entry[0] in ['font-test', 'font-coretext-test', 'render-test', 'pty-fast-test']
        suites += ['isa']
    endif
    if entry[0] in extlib_tests
        suites += ['extlib']
    endif
    if entry[0] in msan_safe_tests
        suites += ['msan-safe']
    endif
    test(
        entry[0],
        exe,
        env: test_env,
        suite: suites,
        timeout: 180,
        depends: entry[0] == 'pty-fast-test' ? dev_terminfo_targets : [],
    )
    if entry[0] == 'render-test'
        benchmark('render-bench', exe, args: ['--bench'], timeout: 300)
    elif entry[0] == 'metal-render-test'
        benchmark('metal-render-bench', exe, args: ['--bench'], timeout: 300)
    endif
endforeach

if backend == 'wayland'
    fastpty_helper = executable(
        'fastpty-exec-test',
        'pty/fastpty_exec_test.c',
        c_args: test_c_args,
        override_options: ['b_sanitize=none'],
    )

    fastpty_env = environment()
    fastpty_env.set('LD_PRELOAD', '')
    fastpty_env.set('DYLD_INSERT_LIBRARIES', '')
    fastpty_env.set('WSAP_NO_PRELOAD', '0')
    test(
        'fastpty-test',
        python,
        args: [files('pty/fastpty_test.py'), preload, fastpty_helper],
        env: fastpty_env,
        suite: ['unit', 'extlib'],
        timeout: 180,
    )
    pty_route_test = executable(
        'pty-route-test',
        'pty/pty_route_test.c',
        c_args: test_c_args,
        link_with: pty_lib,
        dependencies: [thread_dep, dl_dep, yasl_dep],
        include_directories: inc,
    )
    test(
        'pty-route-test',
        pty_route_test,
        args: [preload, fastpty_helper],
        env: fastpty_env,
        suite: ['unit', 'extlib'],
        timeout: 60,
    )
endif

test(
    'ui-api-boundary-test',
    bash,
    args: [files('ui/api_boundary_test.sh'), meson.project_source_root()],
    suite: 'unit',
)

if get_option('integration_tests')
    integration_env = environment()
    integration_env.set('BIN', program.full_path())
    integration_env.set('WSAP_NO_PRELOAD', '0')
    integration_tests = [
    ]
    if backend == 'wayland'
        vptr_xml = files('helpers/wlr-virtual-pointer-unstable-v1.xml')
        vptr = executable(
            'virtual-pointer',
            'helpers/virtual_pointer.c',
            custom_target(
                'wlr-virtual-pointer-client-header',
                output: 'wlr-virtual-pointer-unstable-v1-client-protocol.h',
                input: vptr_xml,
                command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
            ),
            custom_target(
                'wlr-virtual-pointer-code',
                output: 'wlr-virtual-pointer-unstable-v1-protocol.c',
                input: vptr_xml,
                command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
            ),
            dependencies: dependency('wayland-client'),
        )
        test(
            'tab-drag-test',
            bash,
            args: files('ui/yasl/tab_drag_test.sh'),
            depends: [program, preload, vptr],
            env: {
                'BIN': program.full_path(),
                'VPTR': vptr.full_path(),
                'WSAP_NO_PRELOAD': '0',
            },
            suite: 'integration',
            is_parallel: false,
            timeout: 120,
        )
        vkbd_xml = files('helpers/virtual-keyboard-unstable-v1.xml')
        vkbd = executable(
            'virtual-keyboard',
            'helpers/virtual_keyboard.c',
            custom_target(
                'virtual-keyboard-client-header',
                output: 'virtual-keyboard-unstable-v1-client-protocol.h',
                input: vkbd_xml,
                command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
            ),
            custom_target(
                'virtual-keyboard-code',
                output: 'virtual-keyboard-unstable-v1-protocol.c',
                input: vkbd_xml,
                command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
            ),
            dependencies: [dependency('wayland-client'), dependency('xkbcommon')],
        )
        foreach case : ['repeat', 'paste']
            test(
                case == 'repeat' ? 'key-repeat-test' : 'clipboard-paste-test',
                python,
                args: [files('ui/yasl/display_wake_test.py'), case],
                depends: [program, vkbd],
                env: {'BIN': program.full_path(), 'VKEYBOARD': vkbd.full_path()},
                suite: 'integration',
                is_parallel: false,
                timeout: 60,
            )
        endforeach
        test(
            'frame-pacing-test',
            python,
            args: files('ui/yasl/frame_pacing_test.py'),
            depends: [program],
            env: {'BIN': program.full_path()},
            suite: 'integration',
            is_parallel: false,
            timeout: 60,
        )
        test(
            'popup-dismiss-test',
            python,
            args: files('ui/yasl/popup_dismiss_test.py'),
            depends: [program, vptr],
            env: {'BIN': program.full_path(), 'VPOINTER': vptr.full_path()},
            suite: 'integration',
            is_parallel: false,
            timeout: 60,
        )
        test(
            'exit-test',
            python,
            args: files('ui/yasl/exit_test.py'),
            depends: [program],
            env: {'BIN': program.full_path()},
            suite: 'integration',
            is_parallel: false,
            timeout: 60,
        )
        test(
            'display-lost-test',
            python,
            args: files('ui/yasl/display_lost_test.py'),
            depends: [program],
            env: {'BIN': program.full_path()},
            suite: 'integration',
            is_parallel: false,
            timeout: 60,
        )
    endif
    foreach it : integration_tests
        test(
            it[0],
            bash,
            args: it[1],
            depends: [program, preload],
            env: integration_env,
            suite: 'integration',
            is_parallel: false,
            timeout: 180,
        )
    endforeach
    test(
        'pty-order-test',
        python,
        args: files('pty/pty_order_test.py'),
        depends: [program, preload],
        env: integration_env,
        suite: 'integration',
        is_parallel: false,
        timeout: 180,
    )
endif

options_test = executable(
    'options-test',
    files(
        'util/options_test.c',
        '../src/util/options.c',
        '../src/util/config.c',
        '../src/util/fs.c',
        '../src/util/path.c',
        '../src/util/proc.c',
        '../src/util/name.c',
    ),
    c_args: test_c_args,
    include_directories: inc,
)
test('options-test', options_test, env: test_env, suite: ['unit'], timeout: 180)

latency_probe_test = executable(
    'latency-probe-test',
    files('loop/latency_probe_test.c'),
    c_args: test_c_args,
    include_directories: inc,
    link_with: base_lib,
)
test('latency-probe-test', latency_probe_test, env: test_env, suite: ['unit'])
