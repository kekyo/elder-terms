elder_terms_launcher_include_dir = include_directories('src')

elder_terms_connection_repository_test = executable(
  'connection-repository-test',
  [
    'src/connection-repository.cpp',
    'tests/connection-repository-test.cpp',
  ],
  include_directories: elder_terms_launcher_include_dir,
  dependencies: [
    elder_terms_shared_dep,
  ],
)

test(
  'connection-repository-test',
  elder_terms_connection_repository_test,
)
