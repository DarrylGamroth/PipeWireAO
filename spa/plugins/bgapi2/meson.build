bgapi2_sources = [
  'camera.cpp',
]

bgapi2_dependencies = [spa_dep, bgapi2_dep]
bgapi2_install_rpath = get_option('bgapi2-prefix') / 'lib'
bgapi2_c_args = ['-Wno-strict-prototypes']
bgapi2_cpp_args = ['-Wno-pedantic']

if get_option('tests').allowed()
  bgapi2_camera_test = executable(
    'spa-bgapi2-camera-test',
    ['test-camera.c', 'camera.cpp'],
    dependencies: bgapi2_dependencies,
    c_args: bgapi2_c_args,
    cpp_args: bgapi2_cpp_args,
    build_rpath: bgapi2_install_rpath,
  )
  test(
    'spa-bgapi2-camera-euresys',
    bgapi2_camera_test,
    args: ['/opt/euresys/egrabber/lib/x86_64/gigelink.cti'],
    is_parallel: false,
    timeout: 10,
  )
  test(
    'spa-bgapi2-camera-baumer',
    bgapi2_camera_test,
    args: [get_option('bgapi2-prefix') / 'lib/libbgapi2_gige.cti'],
    is_parallel: false,
    timeout: 10,
  )
endif
