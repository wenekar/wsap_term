terminal_fuzz = executable('terminal-fuzz', 'terminal_fuzz.c',
  c_args: wsap_vtcore_fuzz_c_args,
  link_args: wsap_vtcore_fuzz_link_args,
  dependencies: wsap_vtcore_fuzz_dep,
  override_options: ['optimization=1', 'b_lto=false'],
  build_by_default: false,
)

test('terminal-fuzz-smoke', python,
  args: [files('../../tools/fuzz_smoke.py'), terminal_fuzz,
         meson.project_source_root() / 'assets' / 'fuzz',
         '--artifacts', meson.current_build_dir() / 'artifacts'],
  env: test_env,
  suite: 'fuzz',
  timeout: 120,
)
