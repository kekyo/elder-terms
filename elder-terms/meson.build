elder_terms_launcher_include_dir = include_directories('src', '.')
elder_terms_launcher_gtk_dep = dependency('gtk+-3.0')
elder_terms_launcher_gio_dep = dependency('gio-2.0')
elder_terms_launcher_gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
elder_terms_launcher_x11_dep = dependency('x11')
elder_terms_launcher_cardio_include_dir = (
  meson.project_source_root() / 'deps/cardio/include'
)
elder_terms_launcher_gestament_config = find_program('npx')
elder_terms_launcher_gestament_include_dir = run_command(
  elder_terms_launcher_gestament_config,
  ['gestament-config', '--includedir'],
  check: true,
).stdout().strip()

elder_terms_launcher_configuration = configuration_data()
elder_terms_launcher_configuration.set_quoted(
  'BUILD_ICON_THEME_PATH',
  meson.project_source_root() / 'data/icons',
)
elder_terms_launcher_configuration.set_quoted(
  'INSTALL_ICON_THEME_PATH',
  get_option('prefix') / get_option('datadir') / 'icons',
)
configure_file(
  input: 'build-config.h.in',
  output: 'build-config.h',
  configuration: elder_terms_launcher_configuration,
)

elder_terms_launcher = executable(
  'elder-terms',
  [
    'src/connection-repository.cpp',
    'src/hotkey-backend.cpp',
    'src/main-window.cpp',
    'src/main.cpp',
    'src/tray-backend.cpp',
  ],
  cpp_args: [
    '-I' + elder_terms_launcher_gestament_include_dir,
    '-I' + elder_terms_launcher_cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
  ],
  include_directories: elder_terms_launcher_include_dir,
  dependencies: [
    elder_terms_dep,
    elder_terms_launcher_gdk_pixbuf_dep,
    elder_terms_launcher_gio_dep,
    elder_terms_launcher_gtk_dep,
    elder_terms_launcher_x11_dep,
  ],
)

executable(
  'x11-map-recorder',
  'tests/x11-map-recorder.cpp',
  dependencies: [
    elder_terms_launcher_x11_dep,
  ],
)

elder_terms_connection_repository_test = executable(
  'connection-repository-test',
  [
    'src/connection-repository.cpp',
    'tests/connection-repository-test.cpp',
  ],
  include_directories: elder_terms_launcher_include_dir,
  dependencies: [
    elder_terms_dep,
  ],
)

test(
  'connection-repository-test',
  elder_terms_connection_repository_test,
)

elder_terms_tray_backend_test = executable(
  'tray-backend-test',
  [
    'src/tray-backend.cpp',
    'tests/tray-backend-test.cpp',
  ],
  include_directories: elder_terms_launcher_include_dir,
  cpp_args: [
    '-I' + elder_terms_launcher_cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
  ],
  dependencies: [
    elder_terms_launcher_gdk_pixbuf_dep,
    elder_terms_launcher_gio_dep,
    elder_terms_launcher_gtk_dep,
  ],
)

test(
  'tray-backend-test',
  elder_terms_tray_backend_test,
)

elder_terms_hotkey_backend_test = executable(
  'hotkey-backend-test',
  [
    'src/hotkey-backend.cpp',
    'tests/hotkey-backend-test.cpp',
  ],
  include_directories: elder_terms_launcher_include_dir,
  cpp_args: [
    '-I' + elder_terms_launcher_cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
  ],
  dependencies: [
    elder_terms_dep,
    elder_terms_launcher_gio_dep,
    elder_terms_launcher_gtk_dep,
    elder_terms_launcher_x11_dep,
  ],
)

test(
  'hotkey-backend-test',
  elder_terms_hotkey_backend_test,
)

configure_file(
  input: 'src/main-window.ui',
  output: 'main-window.ui',
  copy: true,
)

install_data(
  'packaging/autostart/net.kekyo.elder-terms.desktop',
  install_dir: get_option('autostartdir'),
)

foreach icon_size : ['16', '22', '24', '32', '48', '64', '128', '256']
  install_data(
    '../data/icons/hicolor/' + icon_size + 'x' + icon_size + '/apps/elder-terms.png',
    install_dir: get_option('datadir') / 'icons/hicolor' / (icon_size + 'x' + icon_size) / 'apps',
  )
endforeach
