
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
subdir('terminal')
subdir('app')

base_lib = static_library(
    'wsap-base',
    util_src + platform_src + loop_src,
    include_directories: inc,
    dependencies: [thread_dep, dl_dep, yyjson_dep, yasl_dep] + platform_deps,
    install: false,
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
    dependencies: [term_dep],
    install: false,
)

paint_lib = static_library(
    'wsap-paint',
    paint_src,
    link_with: [base_lib],
    include_directories: inc,
    dependencies: [thread_dep, m_dep] + [term_dep],
    install: false,
)

font_lib = static_library(
    'wsap-font',
    font_src,
    link_with: [base_lib, paint_lib],
    include_directories: inc,
    dependencies: font_deps + [m_dep] + platform_deps + [term_dep],
    install: false,
)

ui_lib = static_library(
    'wsap-ui',
    ui_src,
    link_with: [base_lib, paint_lib],
    include_directories: inc,
    dependencies: [m_dep, yyjson_dep, icu_dep] + [term_dep],
    install: false,
)

theme_lib = static_library(
    'wsap-theme',
    theme_src,
    link_with: [base_lib, ui_lib],
    include_directories: inc,
    dependencies: lib_deps + [term_dep],
    install: false,
)

render_lib = static_library(
    'wsap-render',
    render_src + render_backend_src,
    link_with: [base_lib, font_lib, paint_lib],
    include_directories: inc,
    dependencies: lib_deps + [term_dep],
    install: false,
)

ui_yasl_lib = static_library(
    'wsap-ui-yasl',
    ui_yasl_src,
    link_with: [base_lib, input_lib, font_lib, ui_lib, paint_lib],
    include_directories: inc,
    dependencies: core_deps + [yasl_dep, term_dep] + ui_yasl_accessibility_deps,
    install: false,
)

terminal_lib = static_library(
    'wsap-terminal',
    terminal_src,
    link_with: [base_lib, ui_yasl_lib, session_lib, font_lib, ui_lib, paint_lib],
    include_directories: inc,
    dependencies: core_deps + [yasl_dep] + [term_dep],
    install: false,
)

app_lib = static_library(
    'wsap-app',
    app_src,
    link_with: [base_lib, terminal_lib, ui_yasl_lib, session_lib, font_lib, ui_lib, pty_lib, paint_lib, theme_lib, render_lib],
    include_directories: inc,
    dependencies: core_deps + [yasl_dep] + [term_dep],
    install: false,
)

wsap_libs = [base_lib, pty_lib, session_lib, input_lib, paint_lib, font_lib, ui_lib, theme_lib, render_lib, ui_yasl_lib, terminal_lib, app_lib]
