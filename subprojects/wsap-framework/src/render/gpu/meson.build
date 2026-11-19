# build.c is the CPU half of a GPU backend. Both GPU backends use it.
if system == 'darwin'
    render_src += files('build.c')
    subdir('metal')
elif has_vulkan
    render_src += files('build.c')
    subdir('vulkan')
else
    render_src += files('none.c')
endif
