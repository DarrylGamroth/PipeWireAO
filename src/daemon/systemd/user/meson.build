systemd = dependency('systemd', required : get_option('systemd-user-service'))
if not systemd.found()
  subdir_done()
endif

systemd_user_services_dir = systemd.get_variable('systemduserunitdir', pkgconfig_define : [ 'prefix', prefix])
if get_option('systemd-user-unit-dir') != ''
  systemd_user_services_dir = get_option('systemd-user-unit-dir')
endif

install_data(
  sources : ['pipewire.socket'],
  rename : ['pipewire-ao.socket'],
  install_dir : systemd_user_services_dir)

systemd_config = configuration_data()
systemd_config.set('PW_BINARY', pipewire_bindir / pipewire_daemon_name)

pw_service_reqs = ''
if get_option('dbus').enabled()
  pw_service_reqs += 'dbus.service '
endif
systemd_config.set('PW_SERVICE_REQS', pw_service_reqs)

configure_file(input : 'pipewire.service.in',
               output : 'pipewire-ao.service',
               configuration : systemd_config,
               install_dir : systemd_user_services_dir)
