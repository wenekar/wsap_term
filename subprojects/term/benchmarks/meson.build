foreach entry : [
  ['term-write-bench', files('term_write_bench.c')],
  ['term-resize-bench', files('term_resize_bench.c')],
  ['hist-bench', files('hist_bench.c')],
]
  benchmark(entry[0],
    executable(entry[0], entry[1],
      c_args: ['-D_POSIX_C_SOURCE=200809L'],
      dependencies: wsap_vtcore_internal_dep,
      build_by_default: false),
    timeout: 600,
  )
endforeach
