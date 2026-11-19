test_c_args = ['-UNDEBUG']
# The framework tests live in the `wsap-framework` subproject. These cases
# cover the terminal, the app, and the terminal grid renderer.
test_cases = [
    ['selection-test', files('terminal/selection_test.c'), [terminal_lib], deps],
    ['terminal-input-test', files('terminal/input_test.c'), [app_lib], deps],
    ['terminal-lifecycle-test', files('terminal/lifecycle_test.c'), [app_lib], deps],
    ['terminal-scrolling-test', files('terminal/scrolling_test.c'), [app_lib], deps],
    ['pty-shell-test', files('pty/shell_test.c'), [pty_lib], [thread_dep, dl_dep]],
    ['paste-test', files('session/write_queue_test.c'), [session_lib], deps],
    ['session-cwd-test', files('session/cwd_test.c'), [session_lib], deps],
    ['session-exit-order-test', files('session/exit_order_test.c'), [session_lib], deps],
    ['session-drain-flood-test', files('session/drain_flood_test.c'), [session_lib], deps],
    ['app-reload-test', files('app/reload_test.c'), [app_lib], deps],
    ['app-input-reload-test', files('app/input_reload_test.c'), [app_lib], deps],
    ['drop-test', files('terminal/drop_test.c'), [app_lib], deps],
    ['terminal-drop-content-test', files('terminal/drop_content_test.c'), [app_lib], deps],
    ['terminal-accessibility-test', files('terminal/accessibility_test.c'), [app_lib], deps],
    ['terminal-accessibility-index-test', files('terminal/accessibility_index_test.c'), [app_lib], deps],
    ['terminal-accessibility-cache-test', files('terminal/accessibility_cache_test.c'), [app_lib], deps],
    ['terminal-accessibility-text-test', files('terminal/accessibility_text_test.c'), [app_lib], deps],
    ['terminal-kind-test', files('app/tabs/kind_test.c'), [app_lib], deps],
    ['terminal-bell-close-test', files('app/tabs/exit_close_test.c'), [app_lib], deps],
    ['app-tabs-rows-test', files('app/tabs/rows_test.c'), [app_lib], deps],
    ['app-tabs-cwd-test', files('app/tabs/cwd_test.c'), [app_lib], deps],
    ['app-tabs-transfer-test', files('app/tabs/transfer_test.c'), [app_lib], deps],
    ['app-tabs-content-model-test', files('app/tabs/content_model_test.c'), [app_lib], deps],
    ['app-tabs-service-test', files('app/tabs/service_test.c'), [app_lib], deps],
    ['app-tabs-preview-test', files('app/tabs/preview_test.c'), [app_lib], deps],
    ['pty-fast-test', files('pty/pty_test.c'), [pty_lib], [thread_dep, dl_dep]],
    ['pty-destroy-test', files('pty/pty_destroy_test.c'), [pty_lib], [thread_dep, dl_dep]],
    ['pty-ring-wake-test', files('pty/ring_wake_test.c'), [pty_lib], [thread_dep, dl_dep]],
    ['settings-test', files('app/config_test.c'), [app_lib], deps],
    ['thumbnail-test', files('app/thumbnail_test.c'), [app_lib], deps],
    ['settings-window-test', files('app/config/window_test.c'), [app_lib], deps],
    ['settings-drop-test', files('app/config/drop_test.c'), [app_lib], deps],
    ['window-ui-hosts-test', files('app/tabs/ui_hosts_test.c'), [app_lib], deps],
    ['window-ui-tree-test', files('app/tabs/ui_tree_test.c'), [app_lib], deps],
    ['drop-paste-probe-test', files('app/tabs/drop_paste_probe_test.c'), [app_lib], deps],
    ['render-test', files('render/render_test.c'), [render_term_lib], deps],
    ['render-plan-test', files('render/plan_test.c'), [render_term_lib], deps],
    ['render-decor-test', files('render/decor_test.c'), [render_term_lib], deps],
    ['render-cache-test', files('render/cache_test.c'), [render_term_lib], deps],
    ['render-raster-test', files('render/raster_test.c'), [render_term_lib], deps],
    ['render-damage-test', files('render/damage_test.c'), [render_term_lib], deps],
    ['render-incremental-test', files('render/incremental_test.c'), [render_term_lib], deps],
    ['render-cpu-canvas-test', files('render/cpu_canvas_test.c'), [render_term_lib], deps],
]

if backend == 'wayland'
    wayland_scanner = find_program('wayland-scanner', native: true)
endif

test_common_deps = [
    wsap_ui_yasl_dep, wsap_ui_dep, wsap_font_dep, wsap_paint_dep,
    wsap_theme_dep, wsap_render_dep, wsap_input_dep, wsap_base_dep,
    yasl_dep, term_dep, term_test_dep,
]
test_env = environment()
test_env.set('LD_PRELOAD', '')
test_env.set('DYLD_INSERT_LIBRARIES', '')
test_env.set('WSAP_NO_PRELOAD', '1')
bash = find_program('bash', native: true)

# The deploy tool ships in this tree, so its check runs here.
test(
    'deploy-fonts-test',
    python,
    args: files('tools/deploy_test.py'),
    suite: ['unit'],
)

# Tests that call uninstrumented system libraries, such as fontconfig or the
# system yyjson. The msan setup skips them: MSan reports false positives there.
extlib_tests = [
    'render-test',
    'render-plan-test',
    'render-decor-test',
    'render-cache-test',
    'render-raster-test',
    'render-damage-test',
    'render-incremental-test',
    'render-cpu-canvas-test',
    'vulkan-render-test',
    'terminal-input-test',
    'terminal-lifecycle-test',
    'terminal-scrolling-test',
    'terminal-drop-content-test',
    'terminal-accessibility-test',
    'terminal-accessibility-cache-test',
    'terminal-accessibility-text-test',
    'terminal-kind-test',
    'terminal-bell-close-test',
    'app-reload-test',
    'app-input-reload-test',
    'app-tabs-rows-test',
    'app-tabs-cwd-test',
    'app-tabs-transfer-test',
    'app-tabs-service-test',
    'app-tabs-preview-test',
    'thumbnail-test',
    'settings-window-test',
    'settings-drop-test',
    'window-ui-hosts-test',
    'window-ui-tree-test',
    'drop-paste-probe-test',
]

isa_tests = [
    'render-test',
    'render-plan-test',
    'render-decor-test',
    'render-cache-test',
    'render-raster-test',
    'render-damage-test',
    'render-incremental-test',
    'pty-fast-test',
]

standalone_tests = [
    'pty-fast-test', 'render-test',
    'render-plan-test', 'render-decor-test', 'render-cache-test',
    'render-raster-test', 'render-damage-test', 'render-incremental-test',
]

# The window cadence test drives a real Cocoa window and its Metal layer. It
# builds on its own, because the link group renames main for C sources only.
if system == 'darwin'
    test_cases += [['render-gpu-slot-test', files('render/gpu_slot_test.m'), [render_term_lib], deps]]
    test_cases += [['metal-render-test', files('render/metal_render_test.m'), [render_term_lib], deps]]
    standalone_tests += ['render-gpu-slot-test']
endif
if has_vulkan
    test_cases += [['vulkan-render-test', files('render/vulkan_render_test.c'), [render_term_lib], deps]]
    standalone_tests += ['vulkan-render-test']
endif
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
        libraries = entry[2]
        if entry[0] == 'paste-test'
            group = session_lib.name()
        else
            group = entry[2][0].name()
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
    if entry[0] in isa_tests
        suites += ['isa']
    endif
    if entry[0] in extlib_tests
        suites += ['extlib']
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
    # The preload waits this long for a sync ack. The default of 200 ms is
    # too short on a loaded machine, and the transport check then fails.
    fastpty_env.set('WSAP_SYNC_TIMEOUT_MS', '5000')
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
        dependencies: [thread_dep, dl_dep, yasl_dep, wsap_base_dep],
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

if get_option('integration_tests')
    integration_env = environment()
    integration_env.set('BIN', program.full_path())
    integration_env.set('WSAP_NO_PRELOAD', '0')
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
