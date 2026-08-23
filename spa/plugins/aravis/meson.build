aravis_camera_test = executable(
  'spa-aravis-camera-test',
  ['test-camera.c', 'camera.c'],
  dependencies: [spa_dep, aravis_dep],
)

if get_option('tests').allowed()
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
