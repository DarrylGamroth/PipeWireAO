egrabber_sources = [
  'camera.cpp',
  'device.cpp',
  'feature.cpp',
  'frame_layout.cpp',
  'frame_sequence.cpp',
  'manager.cpp',
  'options.cpp',
  'pixel_format.cpp',
  'plugin.cpp',
  'remote_control.cpp',
  'source.cpp',
  'timestamp_mapper.cpp',
]

egrabber_lib = shared_library(
  'spa-egrabber',
  egrabber_sources,
  dependencies: [spa_dep, egrabber_dep, pthread_lib],
  cpp_args: ['-Wno-pedantic'],
  install: true,
  install_dir: spa_plugindir / 'egrabber',
)

if get_option('tests').allowed()
  egrabber_frame_layout_test = executable(
    'spa-egrabber-frame-layout-test',
    ['test-frame-layout.cpp', 'frame_layout.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-frame-layout', egrabber_frame_layout_test)

  egrabber_frame_sequence_test = executable(
    'spa-egrabber-frame-sequence-test',
    ['test-frame-sequence.cpp', 'frame_sequence.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-frame-sequence', egrabber_frame_sequence_test)

  egrabber_pixel_format_test = executable(
    'spa-egrabber-pixel-format-test',
    ['test-pixel-format.cpp', 'pixel_format.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-pixel-format', egrabber_pixel_format_test)

  egrabber_timestamp_mapper_test = executable(
    'spa-egrabber-timestamp-mapper-test',
    ['test-timestamp-mapper.cpp', 'timestamp_mapper.cpp'],
    dependencies: [spa_dep],
  )
  test('spa-egrabber-timestamp-mapper', egrabber_timestamp_mapper_test)

  egrabber_factory_test = executable(
    'spa-egrabber-factory-test',
    'test-factory.cpp',
    dependencies: [spa_dep, dl_lib],
  )

  egrabber_device_test = executable(
    'spa-egrabber-device-test',
    'test-device.cpp',
    dependencies: [spa_dep, dl_lib],
  )
  test(
    'spa-egrabber-device',
    egrabber_device_test,
    args: [egrabber_lib.full_path()],
    is_parallel: false,
  )
  test(
    'spa-egrabber-factory',
    egrabber_factory_test,
    args: [egrabber_lib.full_path()],
  )

  egrabber_capture_test = executable(
    'spa-egrabber-capture-test',
    'test-capture.cpp',
    dependencies: [spa_dep, dl_lib],
  )
  test(
    'spa-egrabber-capture',
    egrabber_capture_test,
    args: [egrabber_lib.full_path()],
    is_parallel: false,
    timeout: 10,
  )
endif
