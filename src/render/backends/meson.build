if backend == 'wayland'
    render_backend_src = files('none.c')
else
    subdir('metal')
    render_backend_src = metal_render_src
endif
