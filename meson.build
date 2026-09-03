project(
  'elder-terms',
  'cpp',
  version: '0.0.1',
  default_options: [
    'cpp_std=c++20',
    'warning_level=3',
    'werror=true',
  ],
)

elder_terms_private_install_dir = get_option('libdir') / 'elder-terms'
elder_terms_private_install_rpath = '$ORIGIN/..'

cardio_root_dir = meson.project_source_root() / 'deps/cardio'
cardio_include_dir = include_directories('deps/cardio/include')
cardio_gio_dep = dependency('gio-2.0')
cardio_liburing_dep = dependency('liburing')
cardio_cpp_args = [
  '-DCARDIO_SHARED_LIB=1',
  '-DCARDIO_WITH_GLIB=1',
  '-DCARDIO_WITH_GIO=1',
  '-DCARDIO_WITH_LINUX_IO_URING=1',
]

cardio_shared_library = shared_library(
  'cardio',
  'deps/cardio/samples/libcardio/libcardio.cpp',
  include_directories: cardio_include_dir,
  cpp_args: cardio_cpp_args + [
    '-DCARDIO_BUILD_SHARED_LIB=1',
  ],
  dependencies: [
    cardio_gio_dep,
    cardio_liburing_dep,
  ],
  install: true,
  install_dir: elder_terms_private_install_dir,
)

cardio_dep = declare_dependency(
  include_directories: cardio_include_dir,
  compile_args: cardio_cpp_args,
  link_with: cardio_shared_library,
  dependencies: [
    cardio_gio_dep,
    cardio_liburing_dep,
  ],
)

subdir('shared')
subdir('elder-terms')
subdir('elder-terms-vte')
subdir('po')

install_data(
  [
    'README.md',
    'README_ja.md',
    'packaging/copyright',
  ],
  install_dir: get_option('datadir') / 'doc/elder-terms',
)
