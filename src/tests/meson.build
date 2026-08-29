test_apps = [
  'test-endpoint',
  'test-interfaces',
  # 'test-remote',
  'test-stream',
  'test-filter',
  'test-ndarray-filter',
  'test-polling-data-loop',
  'test-security-context',
]

foreach a : test_apps
  test('pw-' + a,
    executable('pw-' + a, a + '.c',
      dependencies : [pipewire_dep],
      include_directories: [includes_inc],
      install : installed_tests_enabled,
      install_dir : installed_tests_execdir),
    env : [
      'PIPEWIREAO_SPA_PLUGIN_DIR=@0@'.format(spa_dep.get_variable('plugindir')),
      'PIPEWIREAO_CONFIG_DIR=@0@'.format(pipewire_dep.get_variable('confdatadir')),
      'PIPEWIREAO_MODULE_DIR=@0@'.format(pipewire_dep.get_variable('moduledir')),
      ])

  if installed_tests_enabled
    test_conf = configuration_data()
    test_conf.set('exec', installed_tests_execdir / 'pw-' + a)
    configure_file(
      input: installed_tests_template,
      output: 'pw-' + a + '.test',
      install_dir: installed_tests_metadir,
      configuration: test_conf
    )
  endif
endforeach

polling_remote_client = executable(
  'pw-test-polling-remote-client',
  'test-polling-remote-client.c',
  dependencies: [pipewire_dep],
)
polling_remote_env = environment()
polling_remote_env.set(
  'PIPEWIREAO_SPA_PLUGIN_DIR',
  spa_dep.get_variable('plugindir'),
)
polling_remote_env.set(
  'PIPEWIREAO_CONFIG_DIR',
  pipewire_dep.get_variable('confdatadir'),
)
polling_remote_env.set(
  'PIPEWIREAO_MODULE_DIR',
  pipewire_dep.get_variable('moduledir'),
)
test(
  'pw-test-polling-remote',
  find_program('python3'),
  args: [
    files('test-polling-remote.py'),
    pipewire_exec.full_path(),
    meson.project_build_root() / 'src/tools/pwao-link',
    polling_remote_client.full_path(),
  ],
  depends: [pipewire_exec, polling_remote_client],
  env: polling_remote_env,
  timeout: 45,
)

if have_cpp
  test_cpp = executable('pw-test-cpp', 'test-cpp.cpp',
                          dependencies : [pipewire_dep],
                          install : installed_tests_enabled,
                          install_dir : installed_tests_execdir)
  test('pw-test-cpp', test_cpp)

  if installed_tests_enabled
    test_conf = configuration_data()
    test_conf.set('exec', installed_tests_execdir / 'pw-test-cpp')
    configure_file(
      input: installed_tests_template,
      output: 'pw-test-cpp.test',
      install_dir: installed_tests_metadir,
      configuration: test_conf
    )
  endif
endif
