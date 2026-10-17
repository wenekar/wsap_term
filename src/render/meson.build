render_src = files(
    'render.c',
    'scene_paint.c',
    'term_frame.c',
)

subdir('backends')

# Sources of the new renderer. They build into render_next_lib.
render_next_src = []
foreach f : ['render_next.c', 'plan.c', 'surface.c']
    if fs.exists(f)
        render_next_src += files(f)
    endif
endforeach

subdir('frame')
subdir('cpu')
subdir('gpu')
