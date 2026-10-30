elder_terms_shared_glib_dep = dependency('glib-2.0')
elder_terms_shared_gio_dep = dependency('gio-2.0')
elder_terms_shared_gtk_dep = dependency('gtk+-3.0')
elder_terms_shared_gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
elder_terms_shared_libudev_dep = dependency('libudev')
elder_terms_shared_xkbcommon_dep = dependency('xkbcommon')
elder_terms_shared_wayland_dep = dependency('wayland-client')
elder_terms_shared_gdk_wayland_dep = dependency('gdk-wayland-3.0')
elder_terms_shared_gdk_x11_dep = dependency('gdk-x11-3.0')
elder_terms_shared_gmodule_dep = dependency('gmodule-2.0')
add_languages('c', native: false)
elder_terms_wayland_scanner = find_program('wayland-scanner')
elder_terms_icon_protocol_xml = meson.project_source_root() / 'deps/boxrig/deps/wayland-protocols/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml'
elder_terms_xdg_shell_xml = meson.project_source_root() / 'deps/boxrig/deps/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
elder_terms_icon_protocol_header = custom_target('elder-terms-icon-protocol-header',
  input: elder_terms_icon_protocol_xml, output: 'xdg-toplevel-icon-v1-client-protocol.h',
  command: [elder_terms_wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
elder_terms_icon_protocol_code = custom_target('elder-terms-icon-protocol-code',
  input: elder_terms_icon_protocol_xml, output: 'xdg-toplevel-icon-v1-protocol.c',
  command: [elder_terms_wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
elder_terms_icon_protocol_server_header = custom_target('elder-terms-icon-protocol-server-header',
  input: elder_terms_icon_protocol_xml, output: 'xdg-toplevel-icon-v1-server-protocol.h',
  command: [elder_terms_wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
elder_terms_xdg_shell_protocol_code = custom_target('elder-terms-xdg-shell-protocol-code',
  input: elder_terms_xdg_shell_xml, output: 'elder-terms-xdg-shell-protocol.c',
  command: [elder_terms_wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
elder_terms_shared_include_dir = include_directories('include')
elder_terms_symbol_map = meson.current_source_dir() / 'elder-terms.map'
elder_terms_gnome = import('gnome')

install_data(
  '../data/connection-icons/NOTO-LICENSE',
  '../data/connection-icons/NOTO-FLAGS-NOTICE',
  '../data/connection-icons/MATERIAL-LICENSE',
  '../data/connection-icons/UNICODE-LICENSE',
  install_dir: get_option('datadir') / 'doc/elder-terms/licenses/connection-icons',
)

elder_terms_shared_resources = elder_terms_gnome.compile_resources(
  'elder-terms-resources',
  'elder-terms.gresource.xml',
  c_name: 'elder_terms_resources',
  source_dir: meson.project_source_root() / 'data',
)

elder_terms_noto_preset_resources = elder_terms_gnome.compile_resources(
  'elder-terms-noto-presets',
  'connection-icon-noto.gresource.xml',
  c_name: 'elder_terms_noto_presets',
  source_dir: meson.project_source_root() / 'data',
)
if package_build_tools == ''
  subdir('material-icon-sources')
  elder_terms_material_convert = find_program('convert')
  elder_terms_material_resource_compiler = find_program('glib-compile-resources')
  elder_terms_material_thumbnailer = find_program('gdk-pixbuf-thumbnailer')
  elder_terms_material_preset_resources = custom_target('elder-terms-material-presets',
    input: 'connection-icon-material.gresource.xml',
    output: 'elder-terms-material-presets.c',
    command: [
      find_program('node'),
      files('../scripts/render-material-connection-icons.mts'),
      meson.project_source_root() / 'data/connection-icons/material-svg',
      '@INPUT@', '@OUTPUT@',
      meson.current_build_dir() / 'material-presets-rendered',
      elder_terms_material_convert,
      elder_terms_material_resource_compiler,
      elder_terms_material_thumbnailer,
    ],
    depend_files: material_icon_svg_sources,
  )
else
  elder_terms_material_preset_resources = package_build_tools / 'elder-terms-material-presets.c'
endif

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

elder_terms_application_version = get_option('application_version')
if elder_terms_application_version == ''
  elder_terms_application_version = run_command(
    files('../build_package.sh'),
    '--print-version',
    check: true,
  ).stdout().strip()
endif
elder_terms_application_version_configuration = configuration_data()
elder_terms_application_version_configuration.set_quoted(
  'ELDER_TERMS_APPLICATION_VERSION',
  elder_terms_application_version,
)
configure_file(
  input: 'application-version-config.h.in',
  output: 'application-version-config.h',
  configuration: elder_terms_application_version_configuration,
)

elder_terms_shared_sources = elder_terms_shared_resources + elder_terms_noto_preset_resources + [elder_terms_material_preset_resources] + [
  'src/connection-identity.cpp',
  'src/connection-desktop.cpp',
  'src/connection-desktop-session.cpp',
  'src/application-icon.cpp',
  'src/connection-icon.cpp',
  'src/connection-icon-presets.cpp',
  'src/connection-icon-image.cpp',
  'src/native-window-icon.cpp',
  'src/application-version.cpp',
  'src/key-binding.cpp',
  'src/key-binding-input-widget.cpp',
  'src/localization.cpp',
  'src/modal-dialog.cpp',
  'src/tracked-window.cpp',
  'src/modal-color-button.cpp',
  'src/ip-scanner.cpp',
  'src/ip-scan-name-resolver.cpp',
  'src/serial-device.cpp',
  'src/serial-device-event-monitor.cpp',
  'src/settings/application-settings.cpp',
  'src/settings/ftp-settings.cpp',
  'src/settings/webdav-settings.cpp',
  'src/settings/smb-settings.cpp',
  'src/settings/general-settings.cpp',
  'src/settings/hyperlink-settings.cpp',
  'src/settings/local-session-settings.cpp',
  'src/settings/log-settings.cpp',
  'src/settings/macro-settings.cpp',
  'src/settings/regular-expression.cpp',
  'src/settings/regex-capture-template.cpp',
  'src/settings/serial-settings.cpp',
  'src/settings/settings.cpp',
  'src/settings/settings-store.cpp',
  'src/settings/sftp-settings.cpp',
  'src/settings/ssh-settings.cpp',
  'src/settings/ssh-proxy-settings.cpp',
  'src/settings/telnet-settings.cpp',
  'src/settings/terminal-settings.cpp',
  'src/settings/transfer-settings.cpp',
  'src/settings-widget/settings-presentation.cpp',
  'src/settings-widget/application-settings-widget.cpp',
  'src/settings-widget/connection-icon-editor.cpp',
  'src/settings-widget/connection-icon-picker.cpp',
  'src/settings-widget/hyperlink-settings-editor.cpp',
  'src/settings-widget/webdav-settings-editor.cpp',
  'src/settings-widget/smb-settings-editor.cpp',
  'src/settings-widget/ssh-proxy-settings-editor.cpp',
  'src/settings-widget/settings-widget.cpp',
  'src/settings-widget/terminal-background-editor.cpp',
  'src/settings-widget/terminal-color-preview.cpp',
  'src/terminal-color-filter.cpp',
  'src/terminal-color-palette.cpp',
  'src/color-appearance.cpp',
] + [elder_terms_icon_protocol_header, elder_terms_icon_protocol_code,
     elder_terms_xdg_shell_protocol_code]

elder_terms_library = shared_library(
  'elder-terms',
  elder_terms_shared_sources,
  soversion: '0',
  include_directories: elder_terms_shared_include_dir,
  dependencies: [
    cardio_dep,
    elder_terms_shared_glib_dep,
    elder_terms_shared_gio_dep,
    elder_terms_shared_gtk_dep,
    elder_terms_shared_gdk_pixbuf_dep,
    elder_terms_shared_libudev_dep,
    elder_terms_shared_xkbcommon_dep,
    elder_terms_shared_wayland_dep,
    elder_terms_shared_gdk_wayland_dep,
    elder_terms_shared_gdk_x11_dep,
    elder_terms_shared_gmodule_dep,
    boxrig_deps['boxrig'],
    boxrig_deps['boxrig-ui'],
  ],
  gnu_symbol_visibility: 'inlineshidden',
  # glib-compile-resources emits one string literal for the whole resource blob.
  c_args: ['-Wno-overlength-strings'],
  link_args: [
    '-Wl,--version-script=' + elder_terms_symbol_map,
  ],
  link_depends: elder_terms_symbol_map,
  install: true,
  install_dir: elder_terms_private_install_dir,
  install_rpath: '$ORIGIN',
)

elder_terms_dep = declare_dependency(
  include_directories: elder_terms_shared_include_dir,
  link_with: elder_terms_library,
  dependencies: [
    jemalloc_dep,
    boxrig_deps['boxrig'],
    boxrig_deps['boxrig-ui'],
    cardio_dep,
    elder_terms_shared_glib_dep,
    elder_terms_shared_gio_dep,
    elder_terms_shared_gtk_dep,
    elder_terms_shared_libudev_dep,
  ],
)

if get_option('build_tests')
native_xdg_shell_server_header = custom_target('native-xdg-shell-server-header',
  input: elder_terms_xdg_shell_xml, output: 'native-xdg-shell-server-protocol.h',
  command: [elder_terms_wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
native_window_icon_compositor = executable('native-window-icon-compositor',
  'tests/native-window-icon-compositor.cpp',
  elder_terms_icon_protocol_server_header, native_xdg_shell_server_header,
  elder_terms_icon_protocol_code, elder_terms_xdg_shell_protocol_code,
  dependencies: dependency('wayland-server'))

native_window_icon_test = executable('native-window-icon-test',
  'tests/native-window-icon-test.cpp',
  dependencies: [elder_terms_dep, elder_terms_shared_gdk_x11_dep, dependency('x11')])
test('native-window-icon-test', find_program('xvfb-run'),
  args: ['-a', native_window_icon_test], env: ['GDK_BACKEND=x11'])

shared_regex_test = executable('shared-regex-test', 'tests/regex-test.cpp', dependencies: elder_terms_dep)
test('shared-regex-test', shared_regex_test)

tracked_window_test = executable('tracked-window-test',
  'tests/tracked-window-test.cpp', dependencies: elder_terms_dep)
test('tracked-window-test', find_program('dbus-run-session'),
  args: ['--', find_program('xvfb-run').full_path(), '-a', tracked_window_test],
  env: ['GDK_BACKEND=x11', 'NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])
test('tracked-window-no-bus-test', find_program('xvfb-run'),
  args: ['-a', tracked_window_test, '--no-bus'],
  env: ['GDK_BACKEND=x11', 'NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

ssh_proxy_widget_test = executable(
  'ssh-proxy-widget-test', 'tests/ssh-proxy-widget-test.cpp',
  dependencies: elder_terms_dep,
)
test('ssh-proxy-widget-test', find_program('xvfb-run'),
  args: ['-a', ssh_proxy_widget_test], timeout: 120,
  env: ['GSETTINGS_BACKEND=memory'])

settings_widget_theme_test = executable(
  'settings-widget-theme-test', 'tests/settings-widget-theme-test.cpp',
  dependencies: elder_terms_dep,
)
test('settings-widget-theme-test', find_program('xvfb-run'),
  args: ['-a', settings_widget_theme_test], timeout: 120,
  env: ['GSETTINGS_BACKEND=memory'])

elder_terms_modal_dialog_test = executable(
  'elder-terms-modal-dialog-test',
  'tests/modal-dialog-test.cpp',
  dependencies: elder_terms_dep,
)
test('elder-terms-modal-dialog-test', find_program('xvfb-run'),
  args: ['-a', elder_terms_modal_dialog_test], timeout: 120,
  env: ['GSETTINGS_BACKEND=memory'])

elder_terms_cpp_compiler = meson.get_compiler('cpp')
elder_terms_dl_dep = elder_terms_cpp_compiler.find_library('dl')
elder_terms_cardio_test_include_dir = include_directories(
  '../deps/cardio/tests',
)

elder_terms_cardio_runtime_test_module = shared_module(
  'elder-terms-cardio-runtime-test-module',
  '../deps/cardio/tests/cardio_shared_plugin.cpp',
  include_directories: elder_terms_cardio_test_include_dir,
  dependencies: cardio_dep,
)

elder_terms_cardio_runtime_test = executable(
  'elder-terms-cardio-runtime-test',
  '../deps/cardio/tests/cardio_shared_host_test.cpp',
  include_directories: elder_terms_cardio_test_include_dir,
  dependencies: [
    cardio_dep,
    elder_terms_dl_dep,
  ],
)

test(
  'elder-terms-cardio-runtime-test',
  elder_terms_cardio_runtime_test,
  args: elder_terms_cardio_runtime_test_module.full_path(),
  depends: elder_terms_cardio_runtime_test_module,
)

elder_terms_shared_test = executable(
  'elder-terms-shared-test',
  'tests/settings-test.cpp',
  dependencies: [
    elder_terms_dep,
  ],
)

test('elder-terms-shared-test', elder_terms_shared_test)

connection_icon_test = executable('connection-icon-test',
  'tests/connection-icon-test.cpp', dependencies: elder_terms_dep)
test('connection-icon-test', connection_icon_test)

connection_identity_test = executable('connection-identity-test',
  'tests/connection-identity-test.cpp', dependencies: elder_terms_dep)
test('connection-identity-test', connection_identity_test)

executable('connection-desktop-driver', 'tests/connection-desktop-driver.cpp',
  dependencies: elder_terms_dep)

connection_window_icon_test = executable('connection-window-icon-test',
  'tests/connection-window-icon-test.cpp',
  dependencies: [elder_terms_dep, elder_terms_shared_gdk_x11_dep, dependency('x11')])
test('connection-window-icon-test', find_program('xvfb-run'),
  args: ['-a', connection_window_icon_test],
  env: ['GDK_BACKEND=x11', 'NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

connection_icon_presets_test = executable('connection-icon-presets-test',
  'tests/connection-icon-presets-test.cpp', dependencies: elder_terms_dep)
test('connection-icon-presets-test', connection_icon_presets_test)

settings_actions_test = executable('settings-actions-test', 'tests/settings-actions-test.cpp',
  dependencies: [elder_terms_dep])
test('settings-actions-test', find_program('xvfb-run'), args: ['-a', settings_actions_test],
  env: ['NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

settings_icon_test = executable('settings-icon-test', 'tests/settings-icon-test.cpp',
  dependencies: [elder_terms_dep])
test('settings-icon-test', find_program('xvfb-run'), args: ['-a', settings_icon_test],
  env: ['NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

preset_icon_picker_test = executable('preset-icon-picker-test',
  'tests/preset-icon-picker-test.cpp', dependencies: [elder_terms_dep])
test('preset-icon-picker-test', find_program('xvfb-run'), args: ['-a', preset_icon_picker_test],
  env: ['NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

settings_tabs_test = executable('settings-tabs-test', 'tests/settings-tabs-test.cpp',
  dependencies: [elder_terms_dep])
test('settings-tabs-test', find_program('xvfb-run'), args: ['-a', settings_tabs_test],
  env: ['NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

elder_terms_serial_device_test = executable(
  'elder-terms-serial-device-test',
  'tests/serial-device-test.cpp',
  dependencies: [
    elder_terms_dep,
  ],
)

test('elder-terms-serial-device-test', elder_terms_serial_device_test)

elder_terms_ip_scanner_test = executable(
  'elder-terms-ip-scanner-test',
  [
    'src/ip-scanner.cpp',
    'src/ip-scan-name-resolver.cpp',
    'tests/ip-scanner-test.cpp',
  ],
  include_directories: elder_terms_shared_include_dir,
  dependencies: [
    cardio_dep,
    elder_terms_shared_gio_dep,
  ],
)

elder_terms_ip_scan_name_resolver_test = executable(
  'elder-terms-ip-scan-name-resolver-test',
  ['src/ip-scan-name-resolver.cpp', 'tests/ip-scan-name-resolver-test.cpp'],
  include_directories: elder_terms_shared_include_dir,
  dependencies: [cardio_dep, elder_terms_shared_gio_dep],
)
test('elder-terms-ip-scan-name-resolver-test',
     elder_terms_ip_scan_name_resolver_test)

test('elder-terms-ip-scanner-test', elder_terms_ip_scanner_test)

ip_scan_dialog_test = executable(
  'ip-scan-dialog-test', 'tests/ip-scan-dialog-test.cpp',
  dependencies: [elder_terms_dep, cardio_dep],
)
test('ip-scan-dialog-test', find_program('xvfb-run'),
  args: ['-a', ip_scan_dialog_test], timeout: 30,
)

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
