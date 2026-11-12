# build.c is the CPU half of a GPU backend. The Vulkan backend uses it. The
# Metal backend still holds its own copy of that logic.
if system == 'darwin'
    subdir('metal')
elif has_vulkan
    render_src += files('build.c')
    subdir('vulkan')
else
    render_src += files('none.c')
endif
