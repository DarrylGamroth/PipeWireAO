discard_link_args = []
if get_option('b_coverage') and cc.get_id() == 'gcc' and host_machine.system() == 'linux'
  discard_link_args += '-Wl,--exclude-libs,libgcov.a'
endif

discard_lib = shared_library('spa-pipewireao-discard', ['plugin.c', 'sink.c'],
  dependencies: spa_dep,
  link_args: discard_link_args,
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: spa_plugindir / 'discard',
)
if get_option('tests').allowed()
  discard_test = executable('test-spa-discard', 'test-sink.c', dependencies: [spa_dep, dl_lib])
  test('spa-pipewireao-discard', discard_test, args: [discard_lib], suite: 'discard')
  if host_machine.system() == 'linux'
    test('spa-pipewireao-discard-exports', find_program('sh'),
      args: [files('test-exports.sh'), find_program('nm').full_path(), discard_lib], suite: 'discard')
  endif
endif
