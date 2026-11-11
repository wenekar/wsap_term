if system == 'darwin'
    subdir('metal')
elif has_vulkan
    subdir('vulkan')
else
    render_src += files('none.c')
endif
