elder_terms_shared_glib_dep = dependency('glib-2.0')
elder_terms_shared_gtk_dep = dependency('gtk+-3.0')
elder_terms_shared_xkbcommon_dep = dependency('xkbcommon')
elder_terms_shared_include_dir = include_directories('include')
elder_terms_symbol_map = meson.current_source_dir() / 'elder-terms.map'

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

elder_terms_shared_sources = [
  'src/key-binding.cpp',
  'src/key-binding-input-widget.cpp',
  'src/localization.cpp',
  'src/settings/application-settings.cpp',
  'src/settings/general-settings.cpp',
  'src/settings/local-session-settings.cpp',
  'src/settings/log-settings.cpp',
  'src/settings/macro-settings.cpp',
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
    elder_terms_shared_gtk_dep,
    elder_terms_shared_xkbcommon_dep,
  ],
  gnu_symbol_visibility: 'inlineshidden',
  link_args: [
    '-Wl,--version-script=' + elder_terms_symbol_map,
  ],
  link_depends: elder_terms_symbol_map,
)

elder_terms_dep = declare_dependency(
  include_directories: elder_terms_shared_include_dir,
  link_with: elder_terms_library,
  dependencies: [
    elder_terms_shared_glib_dep,
    elder_terms_shared_gtk_dep,
  ],
)

elder_terms_shared_test = executable(
  'elder-terms-shared-test',
  'tests/settings-test.cpp',
  dependencies: [
    elder_terms_dep,
  ],
)

test('elder-terms-shared-test', elder_terms_shared_test)

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
