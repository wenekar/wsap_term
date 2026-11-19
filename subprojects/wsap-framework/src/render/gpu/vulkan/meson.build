# The SPIR-V of the shaders is embedded in a generated header. `render_inc`
# makes the header visible to the backend. The header is not in the source
# tree, so a shader edit always reaches the build.
vulkan_shaders = custom_target(
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
        glslc,
        '@OUTPUT@',
        '@PRIVATE_DIR@',
        '@INPUT@',
    ],
)

render_src += [vulkan_shaders] + files(
    'device.c',
    'frame.c',
    'swapchain.c',
)
render_inc += include_directories('.')
