subdir('pty')
subdir('session')
subdir('terminal')
subdir('runtime')
subdir('app')

# The six libraries of the terminal tree. The `wsap-*` dependencies come from
# the `wsap-framework` subproject, which supplies the base, input, paint, font,
# ui, theme, render, and ui_yasl code.

pty_lib = static_library(
    'wsap-pty',
    pty_src,
    include_directories: inc,
    dependencies: [wsap_base_dep, yasl_dep, thread_dep, dl_dep] + platform_deps,
    install: false,
)

session_lib = static_library(
    'wsap-session',
    session_src,
    link_with: [pty_lib],
    include_directories: inc,
    dependencies: [wsap_base_dep, yasl_dep, term_dep],
    install: false,
)

# The terminal grid renderer: it turns a terminal frame into cells and draws
# them through the framework renderer. It is part of the terminal content, not
# of the framework.
render_term_lib = static_library(
    'wsap-render-term',
    render_term_src,
    include_directories: inc,
    c_args: ['-ffp-contract=off'],
    dependencies: [wsap_render_dep, wsap_font_dep, wsap_paint_dep,
                   wsap_base_dep, term_dep],
    install: false,
)

terminal_lib = static_library(
    'wsap-terminal',
    terminal_src,
    link_with: [session_lib, render_term_lib],
    include_directories: inc,
    dependencies: [wsap_ui_yasl_dep, wsap_ui_dep, wsap_font_dep,
                   wsap_paint_dep, wsap_base_dep, yasl_dep, term_dep, png_dep],
    install: false,
)

# The app runtime: frame composition helpers, scene capture, thumbnails, and
# the GPU idle release. Its work outlives one window, and the app owns it.
runtime_lib = static_library(
    'wsap-runtime',
    runtime_src,
    link_with: [render_term_lib],
    include_directories: inc,
    dependencies: [wsap_render_dep, wsap_font_dep, wsap_paint_dep,
                   wsap_base_dep, wsap_ui_yasl_dep, yasl_dep, term_dep],
    install: false,
)

app_lib = static_library(
    'wsap-app',
    app_src,
    link_with: [runtime_lib, terminal_lib, session_lib, pty_lib, render_term_lib],
    include_directories: [inc, app_inc],
    dependencies: [wsap_ui_yasl_dep, wsap_theme_dep, wsap_render_dep,
                   wsap_ui_dep, wsap_font_dep, wsap_paint_dep, wsap_input_dep,
                   wsap_base_dep, yasl_dep, term_dep],
    install: false,
)

wsap_libs = [pty_lib, session_lib, render_term_lib, terminal_lib, runtime_lib, app_lib]
