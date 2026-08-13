elder_terms_shared_glib_dep = dependency('glib-2.0')
elder_terms_shared_gio_dep = dependency('gio-2.0')
elder_terms_shared_gtk_dep = dependency('gtk+-3.0')
elder_terms_shared_gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
elder_terms_shared_libudev_dep = dependency('libudev')
elder_terms_shared_xkbcommon_dep = dependency('xkbcommon')
elder_terms_shared_include_dir = include_directories('include')
elder_terms_symbol_map = meson.current_source_dir() / 'elder-terms.map'
elder_terms_gnome = import('gnome')

elder_terms_shared_resources = elder_terms_gnome.compile_resources(
  'elder-terms-resources',
  'elder-terms.gresource.xml',
  c_name: 'elder_terms_resources',
  source_dir: meson.project_source_root() / 'data',
)

elder_terms_localization_configuration = configuration_data()
elder_terms_localization_configuration.set_quoted(
  'ELDER_TERMS_BUILD_LOCALE_DIR',
  meson.project_build_root() / 'po',
)
elder_terms_localization_configuration.set_quoted(
  'ELDER_TERMS_LOCALE_DIR',
  get_option('prefix') / get_option('localedir'),
)
configure_file(
  input: 'localization-config.h.in',
  output: 'localization-config.h',
  configuration: elder_terms_localization_configuration,
)

elder_terms_shared_sources = elder_terms_shared_resources + [
  'src/application-icon.cpp',
  'src/key-binding.cpp',
  'src/key-binding-input-widget.cpp',
  'src/localization.cpp',
  'src/serial-device.cpp',
  'src/serial-device-event-monitor.cpp',
  'src/settings/application-settings.cpp',
  'src/settings/general-settings.cpp',
  'src/settings/hyperlink-settings.cpp',
  'src/settings/local-session-settings.cpp',
  'src/settings/log-settings.cpp',
  'src/settings/macro-settings.cpp',
  'src/settings/regex-capture-template.cpp',
  'src/settings/serial-settings.cpp',
  'src/settings/settings.cpp',
  'src/settings/settings-store.cpp',
  'src/settings/sftp-settings.cpp',
  'src/settings/ssh-settings.cpp',
  'src/settings/telnet-settings.cpp',
  'src/settings/terminal-settings.cpp',
  'src/settings/terminal-type-settings.cpp',
  'src/settings/transfer-settings.cpp',
  'src/settings-widget/settings-presentation.cpp',
  'src/settings-widget/settings-widget.cpp',
]

elder_terms_library = shared_library(
  'elder-terms',
  elder_terms_shared_sources,
  include_directories: elder_terms_shared_include_dir,
  dependencies: [
    elder_terms_shared_glib_dep,
    elder_terms_shared_gio_dep,
    elder_terms_shared_gtk_dep,
    elder_terms_shared_gdk_pixbuf_dep,
    elder_terms_shared_libudev_dep,
    elder_terms_shared_xkbcommon_dep,
  ],
  gnu_symbol_visibility: 'inlineshidden',
  link_args: [
    '-Wl,--version-script=' + elder_terms_symbol_map,
  ],
  link_depends: elder_terms_symbol_map,
  install: true,
  install_dir: elder_terms_private_install_dir,
)

elder_terms_dep = declare_dependency(
  include_directories: elder_terms_shared_include_dir,
  link_with: elder_terms_library,
  dependencies: [
    elder_terms_shared_glib_dep,
    elder_terms_shared_gio_dep,
    elder_terms_shared_gtk_dep,
    elder_terms_shared_libudev_dep,
  ],
)

if get_option('build_tests')
elder_terms_shared_test = executable(
  'elder-terms-shared-test',
  'tests/settings-test.cpp',
  dependencies: [
    elder_terms_dep,
  ],
)

test('elder-terms-shared-test', elder_terms_shared_test)

elder_terms_serial_device_test = executable(
  'elder-terms-serial-device-test',
  'tests/serial-device-test.cpp',
  dependencies: [
    elder_terms_dep,
  ],
)

test('elder-terms-serial-device-test', elder_terms_serial_device_test)

executable(
  'elder-terms-settings-widget-fixture',
  'tests/settings-widget-fixture.cpp',
  dependencies: [
    elder_terms_dep,
  ],
)

executable(
  'elder-terms-localization-fixture',
  'tests/localization-fixture.cpp',
  dependencies: [
    elder_terms_dep,
  ],
)
endif
