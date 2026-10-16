fastpty_src = files('route.c', 'shim.c')
if system == 'linux'
    fastpty_src += files(
        'preload_env.c',
        'shim_linux.c',
    )
else
    fastpty_src += files('shim_darwin.c')
endif
