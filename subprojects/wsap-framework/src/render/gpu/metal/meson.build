# The shader source is embedded in a generated header. `render_inc` makes the
# header visible to render.m.
metal_shader = custom_target(
    'metal-shader-source',
    input: files('shaders.metal'),
    output: 'metal_shader_source.h',
    command: [
        python,
        files('../../../../tools/embed_metal.py'),
        '@INPUT@',
        '@OUTPUT@',
        meson.project_source_root() / 'src',
    ],
)

render_src += [metal_shader] + files('render.m')
render_inc += include_directories('.')
