bench_cases = [
    ['font-lookup-bench', files('font_lookup_bench.c'), font_lib, font_deps + [m_dep] + platform_deps],
    ['input-queue-bench', files('input_queue_bench.c'), app_lib, deps],
    ['ui-bench', files('ui_bench.c'), [ui_yasl_lib, ui_lib, font_lib, render_lib], deps + [m_dep]],
    ['ui-frame-bench', files('ui_frame_bench.c'), [ui_yasl_lib, ui_lib, font_lib, render_lib], deps + [m_dep]],
]

if system == 'linux'
    bench_cases += [
        ['transport-bench', files('transport_bench.c'), pty_lib, [thread_dep, dl_dep]],
    ]
endif

if system == 'linux'
    latency_deps = []
    latency_src = files('input_latency_bench.c')
    latency_args = ['-D_POSIX_C_SOURCE=200809L']
    if backend == 'wayland'
        wayland_scanner = find_program('wayland-scanner', native: true)
        vk_xml = files('protocols/virtual-keyboard-unstable-v1.xml')
        latency_src += custom_target(
            'virtual-keyboard-client-protocol',
            output: 'virtual-keyboard-unstable-v1-client-protocol.h',
            input: vk_xml,
            command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
        )
        latency_src += custom_target(
            'virtual-keyboard-protocol',
            output: 'virtual-keyboard-unstable-v1-protocol.c',
            input: vk_xml,
            command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
        )
        latency_deps += [dependency('wayland-client'), dependency('xkbcommon')]
        latency_args += ['-DWSAP_LATENCY_WAYLAND']
    endif

    executable(
        'input-latency-bench',
        latency_src,
        dependencies: latency_deps,
        c_args: latency_args,
        include_directories: [inc, include_directories('.')],
    )
endif

foreach entry : bench_cases
    exe = executable(
        entry[0],
        entry[1],
        link_with: entry[2],
        c_args: ['-D_POSIX_C_SOURCE=200809L'],
        include_directories: [inc, include_directories('.')],
        dependencies: entry[3] + [term_dep],
    )
    benchmark(entry[0], exe, timeout: 300)
endforeach

run_target(
    'term-live-bench',
    command: [
        find_program('sh', native: true),
        files('term-live-bench.sh'),
        program,
    ],
    depends: preload,
)
