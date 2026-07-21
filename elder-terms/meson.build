elder_terms_launcher_include_dir = include_directories('src')
elder_terms_launcher_gtk_dep = dependency('gtk+-3.0')
elder_terms_launcher_gio_dep = dependency('gio-2.0')
elder_terms_launcher_cardio_include_dir = (
  meson.project_source_root() / 'deps/cardio/include'
)
elder_terms_launcher_gestament_config = find_program('npx')
elder_terms_launcher_gestament_include_dir = run_command(
  elder_terms_launcher_gestament_config,
  ['gestament-config', '--includedir'],
  check: true,
).stdout().strip()

elder_terms_launcher = executable(
  'elder-terms',
  [
    'src/connection-repository.cpp',
    'src/main-window.cpp',
    'src/main.cpp',
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
    elder_terms_launcher_gio_dep,
    elder_terms_launcher_gtk_dep,
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

configure_file(
  input: 'src/main-window.ui',
  output: 'main-window.ui',
  copy: true,
)
