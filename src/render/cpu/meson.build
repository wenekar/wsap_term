render_src += files(
    'isa.c',
    'kernels.c',
)

# Sources of the new renderer.
foreach f : ['isa.c', 'kernels.c', 'raster.c', 'cell_cache.c', 'pair_lut.c', 'decor.c', 'pool.c', 'scene.c']
    if fs.exists(f)
        render_next_src += files(f)
    endif
endforeach
