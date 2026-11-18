install_framework = get_option('framework')
build_app = get_option('app')

subdir('util')
subdir('platform')
subdir('loop')
subdir('pty')
subdir('session')
subdir('input')
subdir('paint')
subdir('font')
subdir('ui')
subdir('theme')
subdir('render')
if build_app
    subdir('terminal')
    subdir('runtime')
    subdir('app')
endif

base_lib = static_library(
    'wsap-base',
    util_src + platform_src + loop_src,
    include_directories: inc,
    dependencies: [thread_dep, dl_dep, yyjson_dep, yasl_dep] + platform_deps,
    install: install_framework,
)

pty_lib = static_library(
    'wsap-pty',
    pty_src,
    link_with: [base_lib],
    include_directories: inc,
    dependencies: [thread_dep, dl_dep, yasl_dep] + platform_deps,
    install: false,
)

session_lib = static_library(
    'wsap-session',
    session_src,
    link_with: [base_lib, pty_lib],
    include_directories: inc,
    dependencies: core_deps + [yasl_dep] + [term_dep],
    install: false,
)

input_lib = static_library(
    'wsap-input',
    input_src,
    link_with: base_lib,
    include_directories: inc,
    install: install_framework,
)

paint_lib = static_library(
    'wsap-paint',
    paint_src,
    link_with: [base_lib],
    include_directories: inc,
    c_args: ['-ffp-contract=off'],
    dependencies: [thread_dep, m_dep],
    install: install_framework,
)

font_lib = static_library(
    'wsap-font',
    font_src,
    link_with: [base_lib, paint_lib],
    include_directories: inc,
    dependencies: font_deps + [m_dep] + platform_deps,
    install: install_framework,
)

ui_lib = static_library(
    'wsap-ui',
    ui_src,
    link_with: [base_lib, paint_lib],
    include_directories: [inc, ui_inc],
    dependencies: [m_dep, yyjson_dep],
    install: install_framework,
)

theme_lib = static_library(
    'wsap-theme',
    theme_src,
    link_with: [base_lib, ui_lib],
    include_directories: inc,
    dependencies: lib_deps,
    install: install_framework,
)

render_lib = static_library(
    'wsap-render',
    render_src,
    link_with: [base_lib, font_lib, paint_lib],
    include_directories: [inc] + render_inc,
    c_args: ['-ffp-contract=off'],
    dependencies: lib_deps,
    install: install_framework,
)

if build_app
    # The terminal grid renderer. It turns a terminal frame into cells and
    # draws them through render_lib. The terminal content owns it.
    render_term_lib = static_library(
        'wsap-render-term',
        render_term_src,
        link_with: [base_lib, font_lib, paint_lib, render_lib],
        include_directories: [inc] + render_inc,
        c_args: ['-ffp-contract=off'],
        dependencies: lib_deps + [term_dep],
        install: false,
    )

    ui_yasl_lib = static_library(
        'wsap-ui-yasl',
        ui_yasl_src,
        link_with: [base_lib, input_lib, font_lib, ui_lib, paint_lib],
        include_directories: inc,
        dependencies: core_deps + [yasl_dep, accesskit_dep],
        install: false,
    )

    terminal_lib = static_library(
        'wsap-terminal',
        terminal_src,
        link_with: [base_lib, ui_yasl_lib, session_lib, font_lib, ui_lib, paint_lib, render_term_lib],
        include_directories: inc,
        dependencies: core_deps + [yasl_dep] + [term_dep],
        install: false,
    )

    # The app runtime. It owns the renderer use that outlives one window:
    # frame composition helpers, scene capture, thumbnails, and the GPU idle
    # release. It sits above render and ui_yasl, and the app owns it.
    runtime_lib = static_library(
        'wsap-runtime',
        runtime_src,
        link_with: [base_lib, paint_lib, font_lib, render_lib, render_term_lib],
        include_directories: inc,
        dependencies: lib_deps + [yasl_dep] + [term_dep],
        install: false,
    )

    app_lib = static_library(
        'wsap-app',
        app_src,
        link_with: [base_lib, runtime_lib, terminal_lib, ui_yasl_lib, session_lib, font_lib, ui_lib, pty_lib, paint_lib, theme_lib, render_lib, render_term_lib],
        include_directories: [inc, app_inc],
        dependencies: core_deps + [yasl_dep] + [term_dep],
        install: false,
    )

    wsap_libs = [base_lib, pty_lib, session_lib, input_lib, paint_lib, font_lib, ui_lib, theme_lib, render_lib, render_term_lib, ui_yasl_lib, terminal_lib, runtime_lib, app_lib]
else
    wsap_libs = [base_lib, input_lib, paint_lib, font_lib, ui_lib, theme_lib, render_lib]
endif

# The installable framework SDK. Each library gets a dependency that a
# subproject or an installed pkg-config consumer can use. The terminal app
# stays outside this set.
framework_specs = [
    {
        'name': 'wsap-base',
        'lib': base_lib,
        'description': 'Base utilities of the WSAP UI framework',
        'uses': ['yyjson'],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': [thread_dep, dl_dep, m_dep, yyjson_dep] + platform_deps,
    },
    {
        'name': 'wsap-input',
        'lib': input_lib,
        'description': 'Input mapping of the WSAP UI framework',
        'uses': ['wsap-base'],
        'private_libs': [],
        'external': [],
    },
    {
        'name': 'wsap-paint',
        'lib': paint_lib,
        'description': 'Scene, damage, and paint primitives of the WSAP UI framework',
        'uses': ['wsap-base'],
        'private_libs': ['-pthread', '-lm'],
        'external': [thread_dep, m_dep],
    },
    {
        'name': 'wsap-font',
        'lib': font_lib,
        'description': 'Font discovery, shaping, and rasterization of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-paint', 'freetype2', 'fontconfig', 'harfbuzz'],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': font_deps + [m_dep] + platform_deps,
    },
    {
        'name': 'wsap-ui',
        'lib': ui_lib,
        'description': 'Widget toolkit of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-paint'],
        'private_libs': ['-pthread', '-lm'],
        'external': [m_dep, yyjson_dep],
    },
    {
        'name': 'wsap-theme',
        'lib': theme_lib,
        'description': 'Theme catalog of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-ui'],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': lib_deps,
    },
    {
        'name': 'wsap-render',
        'lib': render_lib,
        'description': 'Scene renderer of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-paint', 'wsap-font'],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': lib_deps,
    },
]

pkg = import('pkgconfig')
framework_deps = {}
foreach spec : framework_specs
    public_deps = []
    foreach name : spec['uses']
        if name in framework_deps
            public_deps += [framework_deps[name]]
        endif
    endforeach
    framework_dep = declare_dependency(
        link_with: spec['lib'],
        include_directories: inc,
        dependencies: public_deps + spec['external'],
    )
    framework_deps += {spec['name']: framework_dep}
    meson.override_dependency(spec['name'], framework_dep)
    if install_framework
        pkg.generate(
            name: spec['name'],
            description: spec['description'],
            version: meson.project_version(),
            subdirs: 'wsap',
            libraries: ['-L${libdir}', '-l' + spec['name']],
            libraries_private: spec['private_libs'],
            requires: spec['uses'],
        )
    endif
endforeach

if install_framework
    sdk_headers = run_command(
        python,
        files('../tools/ci/framework_headers.py'),
        meson.current_source_dir(),
        check: true,
    ).stdout().strip().split('\n')
    install_headers(sdk_headers, preserve_path: true, subdir: 'wsap')
endif
