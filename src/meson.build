
subdir('util')
subdir('platform')
subdir('loop')
subdir('editor')
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

editor_lib = static_library(
    'wsap-editor',
    editor_src,
    include_directories: inc,
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
    install: false,
)

paint_lib = static_library(
    'wsap-paint',
    paint_src,
    link_with: [base_lib],
    include_directories: inc,
    c_args: ['-ffp-contract=off'],
    dependencies: [thread_dep, m_dep],
    install: false,
)

font_lib = static_library(
    'wsap-font',
    font_src,
    link_with: [base_lib, paint_lib],
    include_directories: inc,
    dependencies: font_deps + [m_dep] + platform_deps,
    install: false,
)

ui_lib = static_library(
    'wsap-ui',
    ui_src,
    link_with: [base_lib, paint_lib],
    include_directories: [inc, ui_inc],
    dependencies: [m_dep, yyjson_dep],
    install: false,
)

# theme_lib still uses struct wsap_vtcore_theme and its key helpers.
# B3 removes the coupling, and then term_dep goes.
theme_lib = static_library(
    'wsap-theme',
    theme_src,
    link_with: [base_lib, ui_lib],
    include_directories: inc,
    dependencies: lib_deps + [term_dep],
    install: false,
)

# render_lib still uses the terminal frame, the VT cell types, and the
# underline and line-size enums. B4 removes the coupling, and then term_dep goes.
render_lib = static_library(
    'wsap-render',
    render_src,
    link_with: [base_lib, font_lib, paint_lib],
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

wsap_libs = [base_lib, editor_lib, pty_lib, session_lib, input_lib, paint_lib, font_lib, ui_lib, theme_lib, render_lib, ui_yasl_lib, terminal_lib, app_lib]
