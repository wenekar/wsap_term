metal_shader = custom_target(
    'metal-shader-source',
    input: files('shaders.metal'),
    output: 'metal_shader_source.h',
    command: [python, files('../../../../tools/embed_metal.py'), '@INPUT@', '@OUTPUT@'],
)

metal_render_src = [metal_shader] + files('render.m')
