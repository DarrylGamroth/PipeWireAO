queue_test_dep = declare_dependency(
  include_directories: include_directories('..'),
  link_with: queue_state,
  dependencies: [pipewire_dep, spa_dep, pthread_lib, dl_lib],
  compile_args: ['-DPACKAGE_VERSION="@0@"'.format(meson.project_version())],
)
env = environment()
env.set('PIPEWIREAO_SPA_PLUGIN_DIR', spa_dep.get_variable('plugindir'))
env.set('PIPEWIREAO_CONFIG_DIR', pipewire_dep.get_variable('confdatadir'))
env.set('PIPEWIREAO_MODULE_DIR', pipewire_dep.get_variable('moduledir'))
foreach name : ['lifetime', 'queue', 'metadata', 'properties', 'buffer-transfer']
  binary = executable('queue-test-' + name, 'test-' + name + '.c', dependencies: queue_test_dep)
  test('pipewireao-queue-' + name, binary, suite: 'queue')
endforeach
callbacks = executable('queue-test-callbacks', 'test-callbacks.c',
  dependencies: queue_test_dep,
  link_args: [
    '-Wl,--wrap=pw_stream_dequeue_buffer',
    '-Wl,--wrap=pw_stream_queue_buffer',
    '-Wl,--wrap=pw_stream_return_buffer',
    '-Wl,--wrap=pw_stream_get_data_loop',
    '-Wl,--wrap=pw_stream_get_node_id',
    '-Wl,--wrap=pw_stream_get_state',
    '-Wl,--wrap=pw_stream_update_properties',
    '-Wl,--wrap=pw_stream_set_error',
    '-Wl,--wrap=pw_impl_module_update_properties',
  ],
)
test('pipewireao-queue-callbacks', callbacks, suite: 'queue')
test('pipewireao-queue-capture-withdrawal', callbacks, args: ['capture-withdrawal'], env: env, suite: 'queue')
queue_loader = executable('queue-test-module', 'test-module.c', dependencies: queue_test_dep)
live = executable('queue-test-live', 'test-live.c', dependencies: queue_test_dep)
live_deps = [spa_support_lib, pipewire_module_protocol_native, pipewire_module_client_node,
  pipewire_module_link_factory, pipewire_module_scheduler_v1, pipewire_module_access]
test('pipewireao-queue-module', queue_loader, args: [queue_module], depends: live_deps, env: env, suite: 'queue')
test('pipewireao-queue-live', live, args: [queue_module], depends: live_deps, env: env, suite: 'queue')
test('pipewireao-queue-live-withdrawal', live, args: [queue_module, '--withdrawal'], depends: live_deps, env: env, suite: 'queue')
benchmark('pipewireao-queue-producer-cycle', live, args: [queue_module, '--benchmark'],
  depends: live_deps, env: env, timeout: 30)

shell = find_program('sh')
remote = executable('queue-test-remote', 'test-remote-registry.c', dependencies: queue_test_dep)
module_dir = pipewire_dep.get_variable('moduledir')
test('pipewireao-queue-remote', shell, suite: 'queue', env: env,
  args: [files('test-remote.sh'), remote, pipewire_exec, files('test-remote.conf'), module_dir],
  depends: live_deps + [queue_module], timeout: 15)
remote_props = executable('queue-test-remote-properties', 'test-remote-properties.c', dependencies: queue_test_dep)
foreach name, options : {
  'pipewireao-queue-remote-properties': [],
  'pipewireao-queue-remote-opaque': ['--queue-only', '--opaque'],
  'pipewireao-queue-remote-opaque-required-missing': ['--queue-only', '--opaque-missing'],
  'pipewireao-queue-remote-buffer-properties': ['--metadata-records'],
  'pipewireao-queue-remote-buffer-properties-queue-only': ['--queue-only', '--metadata-records'],
  'pipewireao-queue-remote-buffer-properties-lease': ['--lease', '--queue-only', '--metadata-records'],
  'pipewireao-queue-remote-properties-header': ['--header-only'],
  'pipewireao-queue-remote-properties-pause': ['--pause'],
  'pipewireao-queue-remote-properties-shared-loop': ['--same-loop'],
  'pipewireao-queue-remote-video-properties': ['--video', '--queue-only'],
  'pipewireao-queue-remote-video-header': ['--video', '--queue-only', '--header-only'],
  'pipewireao-queue-remote-video-lease-properties': ['--lease', '--video', '--queue-only'],
  'pipewireao-queue-remote-video-lease-header': ['--lease', '--video', '--queue-only', '--header-only'],
}
  test(name, shell, suite: 'queue', env: env,
    args: [files('test-remote-properties.sh'), remote_props, pipewire_exec,
      files('test-remote-properties.conf'), module_dir] + options,
    depends: live_deps + [queue_module], timeout: 25)
endforeach
