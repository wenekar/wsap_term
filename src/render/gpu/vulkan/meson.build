vulkan_shader_target = custom_target(
    'vulkan-shaders-spv',
    input: files(
        'shaders/glyph.vert',
        'shaders/glyph.frag',
        'shaders/shape.vert',
        'shaders/shape.frag',
        'shaders/copy.frag',
    ),
    output: 'vulkan_shaders_spv.h',
    command: [
        python,
        files('../../../../tools/embed_spirv.py'),
        '@OUTPUT@',
        meson.current_source_dir() / 'shaders',
    ],
)

render_src += [vulkan_shader_target] + files('render.c')
render_inc += include_directories('.')
