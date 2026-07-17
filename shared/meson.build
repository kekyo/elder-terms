elder_terms_shared_glib_dep = dependency('glib-2.0')
elder_terms_shared_gtk_dep = dependency('gtk+-3.0')
elder_terms_shared_xkbcommon_dep = dependency('xkbcommon')
elder_terms_shared_include_dir = include_directories('include')

elder_terms_shared_sources = [
  'src/key-binding.cpp',
  'src/key-binding-input-widget.cpp',
  'src/settings/general-settings.cpp',
  'src/settings/local-session-settings.cpp',
  'src/settings/serial-settings.cpp',
  'src/settings/settings.cpp',
  'src/settings/settings-store.cpp',
  'src/settings/telnet-settings.cpp',
  'src/settings/terminal-settings.cpp',
  'src/settings/transfer-settings.cpp',
  'src/settings-widget/settings-widget.cpp',
]

elder_terms_shared_library = static_library(
  'elder-terms-shared',
  elder_terms_shared_sources,
  include_directories: elder_terms_shared_include_dir,
  dependencies: [
    elder_terms_shared_glib_dep,
    elder_terms_shared_gtk_dep,
    elder_terms_shared_xkbcommon_dep,
  ],
)

elder_terms_shared_dep = declare_dependency(
  include_directories: elder_terms_shared_include_dir,
  link_with: elder_terms_shared_library,
  dependencies: [
    elder_terms_shared_glib_dep,
    elder_terms_shared_gtk_dep,
    elder_terms_shared_xkbcommon_dep,
  ],
)

elder_terms_shared_test = executable(
  'elder-terms-shared-test',
  'tests/settings-test.cpp',
  dependencies: [
    elder_terms_shared_dep,
  ],
)

test('elder-terms-shared-test', elder_terms_shared_test)

executable(
  'elder-terms-settings-widget-fixture',
  'tests/settings-widget-fixture.cpp',
  dependencies: [
    elder_terms_shared_dep,
  ],
)
