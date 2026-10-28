project('wsap_vtcore-example', 'c', default_options: ['c_std=c11'])

vtcore_dep = dependency('wsap_vtcore', required: true)
executable('stdin-snapshot', 'stdin_snapshot.c', dependencies: vtcore_dep)
