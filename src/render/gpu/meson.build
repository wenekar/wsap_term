# macOS builds the Metal backend. Every other system builds the CPU-only one.
if system == 'darwin'
    subdir('metal')
else
    render_src += files('none.c')
endif
