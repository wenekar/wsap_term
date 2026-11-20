# OS support: the loop and the sync primitives that the windowing backends use.
if system == 'linux' or system == 'darwin'
    subdir('posix')
endif
# Desktop services shared by the Linux backends.
if system == 'linux'
    subdir('freedesktop')
endif

if has_wayland
    subdir('wayland')
endif
if has_x11
    subdir('x11')
endif
if has_cocoa
    subdir('cocoa')
endif
