subdir('widgets')
subdir('yasl')

# The default stylesheets are data, and the UI library embeds them in generated
# headers. The headers are not in the source tree, so a CSS edit always
# reaches the build. `ui_inc` makes them visible to css.c.
ui_css_headers = [
    custom_target(
        'ui-core-css',
        input: files('../../assets/ui/core.css'),
        output: 'ui_core_css.h',
        command: [
            python,
            files('../../tools/embed_css.py'),
            'kUiCoreCss',
            '@INPUT@',
            '@OUTPUT@',
        ],
    ),
    custom_target(
        'ui-app-css',
        input: files('../../assets/ui/app.css'),
        output: 'ui_app_css.h',
        command: [
            python,
            files('../../tools/embed_css.py'),
            'kUiAppCss',
            '@INPUT@',
            '@OUTPUT@',
        ],
    ),
]
ui_inc = include_directories('.')

ui_src = files(
    'accessibility.c',
    'anchor.c',
    'box.c',
    'css.c',
    'damage.c',
    'debug.c',
    'describe.c',
    'layout.c',
    'paint.c',
    'style_context.c',
    'surface.c',
    'text_break.c',
    'widget.c',
) + ui_widgets_src + ui_css_headers
