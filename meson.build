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

boxrig_node = find_program('node')
package_build_tools = get_option('package_build_tools')
if package_build_tools == ''
  boxrig_prepare = files('scripts/prepare-boxrig.mts')
  jemalloc_prepare = files('scripts/prepare-jemalloc.mts')
  gestament_config = find_program(meson.project_source_root() / 'node_modules/.bin/gestament-config')
  gestament_include_dir = run_command(gestament_config, '--includedir', check: true).stdout().strip()
else
  boxrig_prepare = package_build_tools / 'prepare-boxrig.mjs'
  jemalloc_prepare = package_build_tools / 'prepare-jemalloc.mjs'
  gestament_include_dir = package_build_tools / 'include'
endif

jemalloc_prepare_args = [boxrig_node, jemalloc_prepare,
  meson.project_source_root(), meson.project_build_root()]
run_command(jemalloc_prepare_args, check: true)
jemalloc_build_target = custom_target(
  'jemalloc-build',
  output: 'jemalloc-build.stamp',
  command: jemalloc_prepare_args,
  build_by_default: true,
  build_always_stale: true,
)
jemalloc_library_dir = meson.project_build_root() / 'jemalloc/lib'
jemalloc_library = meson.get_compiler('cpp').find_library('jemalloc',
  dirs: jemalloc_library_dir, static: false)
jemalloc_dep = declare_dependency(
  dependencies: jemalloc_library,
  sources: jemalloc_build_target,
)
custom_target('jemalloc-private-runtime',
  output: 'libjemalloc.so.2',
  command: [find_program('cp'), jemalloc_library_dir / 'libjemalloc.so.2', '@OUTPUT@'],
  depends: jemalloc_build_target,
  build_by_default: true,
  install: true,
  install_dir: elder_terms_private_install_dir,
)
install_data('deps/jemalloc/COPYING',
  install_dir: get_option('datadir') / 'doc/elder-terms/licenses/jemalloc',
)

boxrig_prepare_args = [boxrig_node, boxrig_prepare,
  meson.project_source_root(), meson.project_build_root(), package_build_tools]
run_command(boxrig_prepare_args, check: true)
boxrig_sdk_target = custom_target(
  'boxrig-shared-sdk',
  output: 'boxrig-sdk.stamp',
  command: boxrig_prepare_args,
  build_by_default: true,
  build_always_stale: true,
)
boxrig_sdk_dir = meson.project_build_root() / 'boxrig/sdk'
boxrig_pkg_config = find_program('pkg-config')
boxrig_pkg_env = environment()
boxrig_pkg_env.prepend('PKG_CONFIG_PATH', boxrig_sdk_dir / 'lib/pkgconfig')
boxrig_compiler = meson.get_compiler('cpp')
boxrig_deps = {}
boxrig_runtime_files = []
boxrig_runtime_sources = []
foreach component : ['boxrig', 'boxrig-ui', 'boxrig-session', 'boxrig-posix', 'boxrig-gtk3']
  boxrig_link_args = []
  boxrig_link_dependencies = []
  foreach flag : run_command(boxrig_pkg_config, '--libs', component,
      env: boxrig_pkg_env, check: true).stdout().strip().split()
    if flag.startswith('-lboxrig')
      # A library dependency lets Meson remove its build RUNPATH on install.
      boxrig_link_dependencies += boxrig_compiler.find_library(flag.substring(2),
        dirs: boxrig_sdk_dir / 'lib', static: false)
    else
      boxrig_link_args += flag
    endif
  endforeach
  boxrig_deps += {component: declare_dependency(
    compile_args: run_command(boxrig_pkg_config, '--cflags', component,
      env: boxrig_pkg_env, check: true).stdout().strip().split(),
    link_args: boxrig_link_args,
    dependencies: boxrig_link_dependencies,
    sources: boxrig_sdk_target,
  )}
  runtime_name = 'lib' + component + '.so.0'
  boxrig_runtime_files += runtime_name
  boxrig_runtime_sources += boxrig_sdk_dir / 'lib' / runtime_name
endforeach
# The optional compositor adapter is static; its private X11/Wayland dependencies
# must be linked too. The terminal engine above remains shared.
boxrig_background_args = []
boxrig_background_libraries = []
foreach flag : run_command(boxrig_pkg_config, '--static', '--libs', 'boxrig-background-gtk3',
    env: boxrig_pkg_env, check: true).stdout().strip().split()
  if flag.startswith('-lboxrig-background-')
    boxrig_background_libraries += boxrig_compiler.find_library(flag.substring(2),
      dirs: boxrig_sdk_dir / 'lib', static: true)
  else
    boxrig_background_args += flag
  endif
endforeach
boxrig_background_dep = declare_dependency(
  compile_args: run_command(boxrig_pkg_config, '--cflags', 'boxrig-background-gtk3',
    env: boxrig_pkg_env, check: true).stdout().strip().split(),
  link_args: boxrig_background_args,
  dependencies: boxrig_background_libraries,
  sources: boxrig_sdk_target,
)
custom_target('boxrig-private-runtime',
  output: boxrig_runtime_files,
  command: [find_program('cp')] + boxrig_runtime_sources + ['@OUTDIR@'],
  depends: boxrig_sdk_target,
  build_by_default: true,
  install: true,
  install_dir: elder_terms_private_install_dir,
)

install_data(
  'deps/boxrig/LICENSE',
  'deps/boxrig/packaging/licenses/README.md',
  'deps/boxrig/packaging/licenses/README_ja.md',
  'deps/boxrig/packaging/licenses/sources.json',
  'deps/boxrig/deps/utf8proc/LICENSE.md',
  'deps/boxrig/deps/libpng/LICENSE',
  'deps/boxrig/deps/zlib/LICENSE',
  'deps/boxrig/resources/unicode/17.0.0/LICENSE.txt',
  'deps/boxrig/data/encoding/whatwg/LICENSE',
  rename: ['LICENSE', 'README.md', 'README_ja.md', 'sources.json',
    'utf8proc-LICENSE.md', 'libpng-LICENSE', 'zlib-LICENSE',
    'Unicode-LICENSE.txt', 'WHATWG-Encoding-LICENSE'],
  install_dir: get_option('datadir') / 'doc/elder-terms/licenses/boxrig',
)
custom_target('boxrig-wayland-protocol-notices',
  output: 'Wayland-protocols-NOTICES.md',
  command: [find_program('cp'),
    meson.project_build_root() / 'boxrig/generated/licenses/Wayland-protocols-NOTICES.md', '@OUTPUT@'],
  depends: boxrig_sdk_target,
  build_by_default: true,
  install: true,
  install_dir: get_option('datadir') / 'doc/elder-terms/licenses/boxrig',
)

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
  soversion: '0',
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
  install_rpath: '$ORIGIN',
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
subdir('elder-terms-terminal')
subdir('po')

install_data(
  [
    'README.md',
    'README_ja.md',
    'packaging/copyright',
  ],
  install_dir: get_option('datadir') / 'doc/elder-terms',
)

foreach language : ['ja', 'en']
  install_data(
    'docs' / language / 'webdav.md',
    install_dir: get_option('datadir') / 'doc/elder-terms/docs' / language,
  )
endforeach

install_data(
  'docs/en/webdav-validation.md',
  install_dir: get_option('datadir') / 'doc/elder-terms/docs/en',
)
