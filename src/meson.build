
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
subdir('runtime')
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

theme_lib = static_library(
    'wsap-theme',
    theme_src,
    link_with: [base_lib, ui_lib],
    include_directories: inc,
    dependencies: lib_deps,
    install: false,
)

render_lib = static_library(
    'wsap-render',
    render_src,
    link_with: [base_lib, font_lib, paint_lib],
    include_directories: [inc] + render_inc,
    c_args: ['-ffp-contract=off'],
    dependencies: lib_deps,
    install: false,
)

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

# The app runtime. It owns the renderer use that outlives one window: frame
# composition helpers, scene capture, thumbnails, and the GPU idle release. It
# sits above render and ui_yasl, and the app owns it.
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

wsap_libs = [base_lib, editor_lib, pty_lib, session_lib, input_lib, paint_lib, font_lib, ui_lib, theme_lib, render_lib, render_term_lib, ui_yasl_lib, terminal_lib, runtime_lib, app_lib]
