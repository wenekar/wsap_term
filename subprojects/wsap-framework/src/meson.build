subdir('util')
subdir('platform')
subdir('loop')
subdir('input')
subdir('paint')
subdir('font')
subdir('ui')
subdir('theme')
subdir('render')
subdir('desktop')

base_lib = static_library(
    'wsap-base',
    util_src + platform_src + loop_src,
    include_directories: inc,
    dependencies: [thread_dep, dl_dep, yyjson_dep, yasl_dep] + platform_deps,
    install: not meson.is_subproject(),
)

input_lib = static_library(
    'wsap-input',
    input_src,
    link_with: base_lib,
    include_directories: inc,
    install: not meson.is_subproject(),
)

paint_lib = static_library(
    'wsap-paint',
    paint_src,
    link_with: base_lib,
    include_directories: inc,
    c_args: ['-ffp-contract=off'],
    dependencies: [thread_dep, m_dep],
    install: not meson.is_subproject(),
)

font_lib = static_library(
    'wsap-font',
    font_src,
    link_with: [base_lib, paint_lib],
    include_directories: inc,
    dependencies: font_deps + [m_dep] + platform_deps,
    install: not meson.is_subproject(),
)

ui_lib = static_library(
    'wsap-ui',
    ui_src,
    link_with: [base_lib, paint_lib],
    include_directories: [inc, ui_inc],
    dependencies: [m_dep, yyjson_dep],
    install: not meson.is_subproject(),
)

theme_lib = static_library(
    'wsap-theme',
    theme_src,
    link_with: [base_lib, ui_lib],
    include_directories: inc,
    dependencies: [m_dep] + platform_deps,
    install: not meson.is_subproject(),
)

render_lib = static_library(
    'wsap-render',
    render_src,
    link_with: [base_lib, font_lib, paint_lib],
    include_directories: [inc] + render_inc,
    c_args: ['-ffp-contract=off'],
    dependencies: lib_deps,
    install: not meson.is_subproject(),
)

# The window system and the UI binding. It holds pacing, presentation, input,
# clipboard, drag, IME, menus, accessibility, and the inspector server.
ui_yasl_lib = static_library(
    'wsap-ui-yasl',
    ui_yasl_src,
    link_with: [base_lib, input_lib, font_lib, ui_lib, paint_lib],
    include_directories: inc,
    dependencies: core_deps + [accesskit_dep],
    install: not meson.is_subproject(),
)

desktop_lib = static_library(
    'wsap-desktop',
    desktop_src,
    link_with: [ui_yasl_lib, ui_lib, render_lib],
    include_directories: inc,
    dependencies: [yasl_dep],
    install: not meson.is_subproject(),
)

wsap_libs = [base_lib, input_lib, paint_lib, font_lib, ui_lib, theme_lib,
             render_lib, ui_yasl_lib, desktop_lib]

# Each library gets a dependency that a subproject or an installed pkg-config
# consumer can use.
font_pc_uses = ['wsap-base', 'wsap-paint']
if font_backend != 'coretext'
    font_pc_uses += ['freetype2', 'fontconfig']
endif
font_pc_uses += ['harfbuzz']

framework_specs = [
    {
        'name': 'wsap-base',
        'lib': base_lib,
        'description': 'Base utilities of the WSAP UI framework',
        # src/loop/worker.c and src/loop/watch/watch_inotify.c include
        # yasl/yasl.h, so a consumer of wsap-base needs the yasl headers.
        'uses': ['yasl', 'yyjson'],
        'private': [],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': [yasl_dep, thread_dep, dl_dep, m_dep, yyjson_dep] + platform_deps,
    },
    {
        'name': 'wsap-input',
        'lib': input_lib,
        'description': 'Input mapping of the WSAP UI framework',
        'uses': ['wsap-base'],
        'private': [],
        'private_libs': [],
        'external': [],
    },
    {
        'name': 'wsap-paint',
        'lib': paint_lib,
        'description': 'Scene, damage, and paint primitives of the WSAP UI framework',
        'uses': ['wsap-base'],
        'private': [],
        'private_libs': ['-pthread', '-lm'],
        'external': [thread_dep, m_dep],
    },
    {
        'name': 'wsap-font',
        'lib': font_lib,
        'description': 'Font discovery, shaping, and rasterization of the WSAP UI framework',
        'uses': font_pc_uses,
        'private': [],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': font_deps + [m_dep] + platform_deps,
    },
    {
        'name': 'wsap-ui',
        'lib': ui_lib,
        'description': 'Widget toolkit of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-paint'],
        'private': [],
        'private_libs': ['-pthread', '-lm'],
        'external': [m_dep, yyjson_dep],
    },
    {
        'name': 'wsap-theme',
        'lib': theme_lib,
        'description': 'Theme catalog of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-ui'],
        'private': [],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': [m_dep] + platform_deps,
    },
    {
        'name': 'wsap-render',
        'lib': render_lib,
        'description': 'Scene renderer of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-paint', 'wsap-font'],
        'private': [],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': lib_deps,
    },
    {
        'name': 'wsap-ui-yasl',
        'lib': ui_yasl_lib,
        'description': 'Window system and UI binding of the WSAP UI framework',
        'uses': ['wsap-base', 'wsap-input', 'wsap-paint', 'wsap-font',
                 'wsap-ui', 'yasl', 'accesskit-c-0.23'],
        'private': [],
        'private_libs': ['-pthread', '-ldl', '-lm'],
        'external': core_deps + [accesskit_dep],
    },
    {
        'name': 'wsap-desktop',
        'lib': desktop_lib,
        'description': 'Default desktop window runner and frame composer',
        'uses': ['wsap-ui-yasl', 'wsap-ui', 'wsap-render'],
        'private': [],
        'private_libs': [],
        'external': [yasl_dep],
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
    if not meson.is_subproject()
        pkg.generate(
            name: spec['name'],
            description: spec['description'],
            version: meson.project_version(),
            subdirs: 'wsap',
            libraries: ['-L${libdir}', '-l' + spec['name']] + spec['private_libs'],
            requires: spec['uses'],
            requires_private: spec['private'],
        )
    endif
endforeach

# The SDK headers and the pkg-config files belong to a standalone build.
# A parent project links the libraries and installs its own files.
if not meson.is_subproject()
    sdk_headers = run_command(
        python,
        files('../tools/ci/framework_headers.py'),
        meson.current_source_dir(),
        check: true,
    ).stdout().strip().split('\n')
    install_headers(sdk_headers, preserve_path: true, subdir: 'wsap')
endif
