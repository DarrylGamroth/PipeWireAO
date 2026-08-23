aravis_lib = shared_library(
  'spa-aravis',
  ['plugin.c', 'source.c', 'camera.c', 'params.c'],
  dependencies: [spa_dep, aravis_dep],
  install: true,
  install_dir: spa_plugindir / 'aravis',
)

aravis_camera_test = executable(
  'spa-aravis-camera-test',
  ['test-camera.c', 'camera.c'],
  dependencies: [spa_dep, aravis_dep],
)

if get_option('tests').allowed()
  aravis_factory_test = executable(
    'spa-aravis-factory-test',
    'test-factory.c',
    dependencies: [spa_dep, dl_lib],
  )
  test(
    'spa-aravis-factory',
    aravis_factory_test,
    args: [aravis_lib.full_path()],
  )

  aravis_capture_test = executable(
    'spa-aravis-capture-test',
    'test-capture.c',
    dependencies: [spa_dep, dl_lib],
  )
  test(
    'spa-aravis-capture-euresys',
    aravis_capture_test,
    args: [aravis_lib.full_path(),
      'Euresys-ChinaVision-GE34GM-Device-dc-a5-01-2e-83-c3'],
    env: [
      'GENICAM_GENTL64_PATH=/opt/euresys/egrabber/lib/x86_64',
      'LD_LIBRARY_PATH=/opt/euresys/egrabber/lib/x86_64',
    ],
    is_parallel: false,
    timeout: 15,
  )

  test(
    'spa-aravis-camera-euresys',
    aravis_camera_test,
    args: ['Euresys-ChinaVision-GE34GM-Device-dc-a5-01-2e-83-c3'],
    env: [
      'GENICAM_GENTL64_PATH=/opt/euresys/egrabber/lib/x86_64',
      'LD_LIBRARY_PATH=/opt/euresys/egrabber/lib/x86_64',
    ],
    is_parallel: false,
    timeout: 15,
  )
endif
