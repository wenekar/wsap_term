# Select OS support independently of the windowing backends. A future Win32
# port supplies its own loop and yasl_platform_sync.h here.
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
