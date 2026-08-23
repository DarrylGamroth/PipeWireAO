if alsa_dep.found()
  subdir('alsa')
endif
if get_option('avb').require(host_machine.system() == 'linux', error_message: 'AVB support is only available on Linux').allowed()
  subdir('avb')
endif
if get_option('audioconvert').allowed()
  subdir('audioconvert')
endif
if get_option('audiomixer').allowed()
  subdir('audiomixer')
endif
if get_option('control').allowed()
  subdir('control')
endif
if get_option('audiotestsrc').allowed()
  subdir('audiotestsrc')
endif
if bluez_deps_found
  subdir('bluez5')
endif
if avcodec_dep.found()
  subdir('ffmpeg')
endif
if jack_dep.found()
  subdir('jack')
endif
if get_option('support').allowed()
  subdir('support')
endif
if get_option('test').allowed()
  subdir('test')
endif
if get_option('videoconvert').allowed()
  subdir('videoconvert')
endif
if get_option('videotestsrc').allowed()
  subdir('videotestsrc')
endif
if get_option('volume').allowed()
  subdir('volume')
endif
if have_vulkan
  subdir('vulkan')
endif

v4l2_header_found = cc.has_header('linux/videodev2.h', required: get_option('v4l2'))
summary({'V4L2 kernel header': v4l2_header_found}, bool_yn: true, section: 'Backend')
summary({'V4L2 enabled': v4l2_header_found}, bool_yn: true, section: 'Backend')
if v4l2_header_found
  subdir('v4l2')
endif
if libcamera_dep.found()
  subdir('libcamera')
endif

egrabber_found = false
if have_cpp and get_option('egrabber').allowed()
  egrabber_include = include_directories(get_option('egrabber-prefix') / 'include')
  egrabber_dep = declare_dependency(include_directories: egrabber_include)
  egrabber_found = cxx.has_header(
    'EGrabber.h',
    dependencies: egrabber_dep,
    required: get_option('egrabber'),
  )
endif
summary({'eGrabber': egrabber_found}, bool_yn: true, section: 'Backend')
if egrabber_found
  subdir('egrabber')
endif

bgapi2_found = false
if get_option('bgapi2').allowed()
  bgapi2_prefix = get_option('bgapi2-prefix')
  bgapi2_include = include_directories(bgapi2_prefix / 'include')
  bgapi2_library = cc.find_library(
    'bgapi2_genicam',
    dirs: [bgapi2_prefix / 'lib'],
    required: get_option('bgapi2'),
  )
  bgapi2_dep = declare_dependency(
    include_directories: bgapi2_include,
    dependencies: bgapi2_library,
  )
  bgapi2_found = bgapi2_library.found() and cc.has_header(
    'bgapi2_genicam/bgapi2_genicam.h',
    dependencies: bgapi2_dep,
    required: get_option('bgapi2'),
  )
endif
summary({'BGAPI2': bgapi2_found}, bool_yn: true, section: 'Backend')
if bgapi2_found
  subdir('bgapi2')
endif

subdir('aec')
plugin_dependencies = []
if get_option('audioconvert').allowed()
  subdir('filter-graph')
endif
