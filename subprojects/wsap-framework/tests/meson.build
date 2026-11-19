test_c_args = ['-UNDEBUG']
test_deps = core_deps

test_cases = [
    ['json-test', files('util/json_test.c'), base_lib],
    ['geom-test', files('util/geom_test.c'), base_lib],
    ['app-identity-test', files('util/app_identity_test.c'), base_lib],
    ['proc-test', files('util/proc_test.c'), base_lib],
    ['worker-test', files('loop/worker_test.c'), base_lib],
    ['watch-test', files('loop/watch_test.c'), base_lib],
    ['input-test', files('input/input_test.c'), input_lib],
    ['ui-scene-test', files('paint/scene_test.c'), ui_lib],
    ['ui-widget-test', files('ui/widget_test.c'), ui_lib],
    ['ui-accessibility-test', files('ui/accessibility_test.c'), ui_lib],
    ['ui-focus-test', files('ui/focus_test.c'), ui_lib],
    ['ui-dispatch-test', files('ui/dispatch_test.c'), ui_lib],
    ['ui-box-test', files('ui/box_test.c'), ui_lib],
    ['ui-typed-widgets-test', files('ui/typed_widgets_test.c'), ui_lib],
    ['ui-style-context-test', files('ui/style_context_test.c'), ui_lib],
    ['ui-part-handle-test', files('ui/part_handle_test.c'), ui_lib],
    ['ui-paint-test', files('ui/paint_test.c'), ui_lib],
    ['ui-surface-test', files('ui/surface_test.c'), ui_lib],
    ['ui-transition-test', files('ui/transition_test.c'), ui_lib],
    ['ui-core-contract-test', files('ui/core_contract_test.c'), ui_lib],
    ['ui-model-contract-test', files('ui/model_contract_test.c'), ui_lib],
    ['ui-scrollview-test', files('ui/widgets/scrollview_test.c'), ui_lib],
    ['ui-menu-test', files('ui/widgets/menu_test.c'), ui_lib],
    ['anchor-test', files('ui/anchor_test.c'), ui_lib],
    ['ui-list-test', files('ui/widgets/list_test.c'), ui_lib],
    ['ui-toggle-test', files('ui/widgets/toggle_test.c'), ui_lib],
    ['ui-controls-test', files('ui/widgets/controls_test.c'), ui_lib],
    ['ui-input-test', files('ui/widgets/input_test.c'), ui_lib],
    ['ui-input-oom-test', files('ui/widgets/input_oom_test.c'), ui_lib],
    ['ui-view-oom-test', files('ui/view_oom_test.c'), ui_lib],
    ['ui-drag-event-test', files('ui/drag_event_test.c'), ui_lib],
    ['ui-label-test', files('ui/widgets/label_test.c'), ui_lib],
    ['ui-dropdown-test', files('ui/widgets/dropdown_test.c'), ui_lib],
    ['ui-acceptance-test', files('ui/acceptance_test.c'), ui_lib],
    ['ui-debug-test', files('ui/debug_test.c'), ui_lib],
    ['ui-text-break-test', files('ui/text_break_test.c'), ui_lib],
    ['ui-text-select-test', files('ui/text_select_test.c'), ui_lib],
    ['css-parser-test', files('ui/css/parser_test.c'), ui_lib],
    ['css-selector-test', files('ui/css/selector_test.c'), ui_lib],
    ['css-lookup-test', files('ui/css/lookup_test.c'), ui_lib],
    ['render-kernels-test', files('render/cpu/kernels_test.c'), render_lib],
    ['render-pool-test', files('render/cpu/pool_test.c'), render_lib],
    ['render-scene-test', files('render/scene_test.c'), render_lib],
    ['render-gpu-build-test', files('render/gpu_build_test.c'), render_lib],
    ['render-plan-ink-test', files('render/plan_ink_test.c'), render_lib],
    ['ui-yasl-drop-route-test', files('ui/yasl/drop_route_test.c'), ui_yasl_lib],
    ['ui-yasl-key-route-test', files('ui/yasl/key_route_test.c'), ui_yasl_lib],
    ['ui-yasl-drop-route-regress-test', files('ui/yasl/drop_route_regress_test.c'), ui_yasl_lib],
    ['ui-yasl-native-input-test', files('ui/yasl/native_input_test.c'), ui_yasl_lib],
    ['ui-yasl-text-menu-test', files('ui/yasl/text_menu_test.c'), ui_yasl_lib],
    ['ui-yasl-window-present-test', files('ui/yasl/window_present_test.c'), ui_yasl_lib],
]

scene_tests = [
    ['anim-test', files('ui/anim_test.c'), [ui_lib, render_lib]],
    ['ui-scene-text-test', files('paint/scene_text_test.c'), [ui_lib, render_lib]],
    ['css-clipping-test', files('ui/css/clipping_test.c'), [ui_lib, render_lib]],
]
foreach entry : scene_tests
    test_cases += [entry + [files('ui/stubs/scene_paint_deps.c')]]
endforeach

test_env = environment()
test_env.set('LD_PRELOAD', '')
test_env.set('DYLD_INSERT_LIBRARIES', '')
test_env.set('WSAP_NO_PRELOAD', '1')

bash = find_program('bash', native: true)
test(
    'ui-api-boundary-test',
    bash,
    args: [files('ui/api_boundary_test.sh'), meson.project_source_root()],
    suite: 'unit',
)

foreach entry : test_cases
    name = entry[0]
    sources = entry[1]
    libraries = entry[2]
    if entry.length() == 4
        sources += entry[3]
    endif
    exe = executable(
        name,
        sources,
        link_with: libraries,
        c_args: test_c_args,
        include_directories: [inc, include_directories('..')],
        dependencies: test_deps,
        override_options: ['b_lto=false'],
    )
    test(name, exe, env: test_env, suite: 'unit', timeout: 180)
endforeach

# The options parser reads files and processes. The test needs the sources of
# the parser alone, so it compiles them without the rest of the base library.
options_test = executable(
    'options-test',
    files(
        'util/options_test.c',
        '../src/util/options.c',
        '../src/util/config.c',
        '../src/util/fs.c',
        '../src/util/path.c',
        '../src/util/proc.c',
        '../src/util/app_identity.c',
    ),
    c_args: test_c_args,
    include_directories: inc,
    dependencies: test_deps,
)
test('options-test', options_test, env: test_env, suite: 'unit', timeout: 180)

latency_probe_test = executable(
    'latency-probe-test',
    files('loop/latency_probe_test.c'),
    c_args: test_c_args,
    include_directories: inc,
    link_with: base_lib,
    dependencies: [thread_dep],
    override_options: ['b_lto=false'],
)
test('latency-probe-test', latency_probe_test, env: test_env, suite: 'unit')

# The SDK tools ship in this tree, so their checks run here.
test(
    'sdk-source-paths-test',
    bash,
    args: files('tools/sdk_source_paths_test.sh'),
    suite: 'unit',
)
test(
    'sdk-public-header-test',
    python,
    args: files('tools/sdk_public_header_test.py'),
    suite: 'unit',
)
