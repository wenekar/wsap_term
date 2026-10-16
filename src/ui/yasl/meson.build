subdir('accessibility')

ui_yasl_src = files(
    'debug.c',
    'debug_cmd.c',
    'debug_server.c',
    'host.c',
    'input.c',
    'native_input.c',
    'stats.c',
    'ui.c',
    'ui_accessibility.c',
    'ui_draw.c',
    'ui_input.c',
    'ui_menu.c',
    'ui_root.c',
    'ui_route.c',
    'window.c',
) + ui_yasl_accessibility_src

if system == 'darwin'
    ui_yasl_src += files('metal.m')
else
    ui_yasl_src += files('metal.c')
endif
