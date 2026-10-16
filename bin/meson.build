program = executable(
    'wsap_term',
    main_src,
    link_with: wsap_libs,
    include_directories: inc,
    dependencies: core_deps + [term_dep],
    install: true,
    install_dir: exe_install_dir,
)

preload = shared_library(
    'fastpty',
    fastpty_src,
    include_directories: inc,
    dependencies: dl_dep,
    override_options: ['b_sanitize=none'],
    install: true,
    install_dir: exe_install_dir,
)

alias_target('install-targets', program, preload)
