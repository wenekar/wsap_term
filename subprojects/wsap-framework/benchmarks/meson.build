bench_cases = [
    ['font-lookup-bench', files('font_lookup_bench.c'), font_lib, font_deps + [m_dep] + platform_deps],
]

if font_backend != 'coretext'
    bench_cases += [
        ['font-fallback-bench', files('font_fallback_bench.c'), font_lib, font_deps + [m_dep]],
    ]
endif

foreach entry : bench_cases
    exe = executable(
        entry[0],
        entry[1],
        link_with: entry[2],
        c_args: ['-D_POSIX_C_SOURCE=200809L'],
        include_directories: [inc, include_directories('.')],
        dependencies: entry[3],
        override_options: ['b_lto=false'],
    )
    benchmark(entry[0], exe, timeout: 300)
endforeach
