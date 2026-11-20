subdir('config')
subdir('tabs')

# The app stylesheet is data. It becomes a generated header in `app_src`, and
# `app_inc` makes that header visible to style.c.
app_css = custom_target(
    'app-css',
    input: files('../../assets/ui/app.css'),
    output: 'app_css.h',
    command: [
        python,
        files('../../tools/embed_css.py'),
        'kAppCss',
        '@INPUT@',
        '@OUTPUT@',
    ],
)
app_inc = include_directories('.')

main_src = files('main.c')

app_src = files(
    'app.c',
    'stats.c',
    'style.c',
) + app_tabs_src + app_config_src + app_css
