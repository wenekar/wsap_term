watch_src = files('watch.c')
if system == 'linux'
    watch_src += files('watch_inotify.c')
else
    watch_src += files('watch_stub.c')
endif
