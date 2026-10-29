gtk_dep = dependency('gtk+-3.0')
canberra_dep = dependency('libcanberra')
gio_dep = dependency('gio-2.0')
liburing_dep = dependency('liburing')
libudev_dep = dependency('libudev')
libssh_dep = dependency('libssh')
libcurl_dep = dependency('libcurl', version: '>=7.88.1')
libxml2_dep = dependency('libxml-2.0', version: '>=2.9.14')
openssl_dep = dependency('openssl')

make_program = find_program('make')
libxyzm_root_dir = meson.project_source_root() / 'deps/libxyzm'
libxyzm_include_dir = libxyzm_root_dir / 'include'
libxyzm_build_dir = meson.current_build_dir()
libxyzm_makefile = meson.current_source_dir() / 'libxyzm-cardio.mk'
libxyzm_extra_cxxflags = ''
if get_option('buildtype') == 'release'
  libxyzm_extra_cxxflags = '-g0'
endif
libxyzm_static_libraries = custom_target(
  'libxyzm-static-libraries',
  output: [
    'libxyzm.a',
    'libxyzm_async.a',
  ],
  command: [
    make_program,
    '-C',
    libxyzm_root_dir,
    '-f',
    libxyzm_makefile,
    'CARDIO_DIR=' + cardio_root_dir,
    'BUILD_DIR=' + libxyzm_build_dir,
    'LIBXYZM_EXTRA_CXXFLAGS=' + libxyzm_extra_cxxflags,
    libxyzm_build_dir / 'libxyzm.a',
    libxyzm_build_dir / 'libxyzm_async.a',
  ],
  build_by_default: true,
  build_always_stale: true,
)
libxyzm_static_library = libxyzm_static_libraries[0]
libxyzm_async_static_library = libxyzm_static_libraries[1]

ssh_transport_library = static_library('elder-terms-ssh-transport',
  ['src/terminal-sessions/ssh-session/ssh-channel-connection.cpp',
   'src/terminal-sessions/ssh-session/ssh-proxy.cpp',
   'src/terminal-sessions/ssh-session/ssh-socks-proxy.cpp',
   'src/terminal-sessions/tcp-connector.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libssh_dep, liburing_dep])
ssh_transport_dep = declare_dependency(link_with: ssh_transport_library,
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libssh_dep, liburing_dep])

boxrig_runtime_library = static_library('elder-terms-terminal-runtime',
  ['src/terminal-runtime.cpp', 'src/terminal-connection.cpp',
   'src/terminal-links.cpp', 'src/terminal-hyperlink-action.cpp',
   'src/terminal-received-macros.cpp',
   'src/terminal-text-send.cpp', 'src/terminal-text-send-runner.cpp',
   'src/terminal-transfer.cpp', 'src/terminal-transfer-runner.cpp',
   'src/terminal-zmodem-auto-start.cpp',
   'src/terminal-sessions/terminal-text-stream.cpp', 'src/terminal-telnet-transport.cpp', 'src/terminal-ssh-transport.cpp',
   'src/terminal-serial-transport.cpp', 'src/terminal-sessions/serial-session/serial-termios.cpp',
   'src/terminal-sessions/serial-session/serial-line-monitor.cpp',
   'src/terminal-sessions/telnet-session/telnet-protocol.cpp'],
  include_directories: elder_terms_shared_include_dir,
  cpp_args: ['-I' + libxyzm_include_dir],
  link_with: libxyzm_async_static_library,
  dependencies: [boxrig_deps['boxrig-gtk3'], boxrig_deps['boxrig-posix'], cardio_dep, ssh_transport_dep],
)
boxrig_runtime_dep = declare_dependency(
  link_with: boxrig_runtime_library,
  include_directories: elder_terms_shared_include_dir,
  dependencies: [jemalloc_dep, boxrig_deps['boxrig-gtk3'], boxrig_deps['boxrig-posix'], cardio_dep, ssh_transport_dep],
)
executable('boxrig-driver', ['src/boxrig-driver.cpp', 'src/inline-prompt.cpp', 'src/terminal-log.cpp',
    'src/sftp/sftp-client.cpp'],
  cpp_args: ['-I' + gestament_include_dir],
  dependencies: [boxrig_runtime_dep],
)

# Keep the upstream headless driver unchanged and link the same shared core as
# the terminal. It does not initialize GTK, a display server, or an IME.
boxrig_regex_flags = run_command(boxrig_pkg_config, '--cflags', '--libs', 'boxrig',
  env: boxrig_pkg_env, check: true).stdout().strip().split()
custom_target('boxrig-regex',
  input: boxrig_source / 'examples/core/regex.c', output: 'boxrig-regex',
  command: [find_program('cc'), '-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
    '@INPUT@', '-o', '@OUTPUT@', '-L' + jemalloc_library_dir, '-ljemalloc',
    '-Wl,-rpath,' + jemalloc_library_dir,
    '-Wl,-rpath,' + boxrig_sdk_dir / 'lib'] + boxrig_regex_flags,
  depends: [boxrig_sdk_target, jemalloc_build_target], build_by_default: true)

telnet_protocol_source = 'src/terminal-sessions/telnet-session/telnet-protocol.cpp'
elder_terms_sources = [
  'src/activity-indicator.cpp',
  'src/compact-header.cpp',
  'src/indicator-color.cpp',
  'src/inline-prompt.cpp',
  'src/launch-options.cpp',
  'src/main.cpp',
  'src/main-window.cpp',
  'src/terminal-background.cpp',
  'src/terminal-background-image.cpp',
  'src/sftp/sftp-client.cpp',
  'src/sftp/sftp-fixture-client.cpp',
  'src/file-transfer/file-hash.cpp',
  'src/file-transfer/ssh-file-hash.cpp',
  'src/file-transfer/file-transfer-engine.cpp',
  'src/file-transfer/file-transfer-paths.cpp',
  'src/file-transfer/file-transfer-window.cpp',
  'src/file-transfer/activity-file-client.cpp',
  'src/file-transfer/local-file-reveal.cpp',
  'src/terminal-connection-phase.cpp',
  'src/terminal-bell.cpp',
  'src/terminal-bell-player.cpp',
  'src/terminal-log.cpp',
  'src/widget-background.cpp',
  'src/terminal-presentation.cpp',
  'src/terminal-renderer-status.cpp',
]

elder_terms_terminal = executable(
  'elder-terms-terminal',
  elder_terms_sources,
  cpp_args: [
    '-I' + gestament_include_dir,
    '-I' + libxyzm_include_dir,
  ],
  dependencies: [
    jemalloc_dep,
    cardio_dep,
    elder_terms_dep,
    canberra_dep,
    gio_dep,
    gtk_dep,
    liburing_dep,
    ssh_transport_dep,
    libudev_dep,
    boxrig_runtime_dep,
    boxrig_background_dep,
  ],
  link_with: [
    libxyzm_async_static_library,
  ],
  install: true,
  install_dir: elder_terms_private_install_dir / 'elder-terms-terminal',
  install_rpath: elder_terms_private_install_rpath,
)

if get_option('build_tests')
color_filter_test = executable('terminal-color-filter-test',
  ['tests/cpp/terminal-color-filter-test.cpp'],
  dependencies: [elder_terms_dep, boxrig_deps['boxrig-ui']])
test('terminal-color-filter-test', color_filter_test)

reverse_filter_test = executable('terminal-reverse-filter-test',
  ['tests/cpp/terminal-reverse-filter-test.cpp'],
  dependencies: [elder_terms_dep, boxrig_deps['boxrig-ui']])
test('terminal-reverse-filter-test', reverse_filter_test)

color_appearance_test = executable('color-appearance-test',
  ['tests/cpp/color-appearance-test.cpp', '../shared/src/color-appearance.cpp'],
  dependencies: elder_terms_dep)
test('color-appearance-test', color_appearance_test)

color_vision_test = executable('terminal-color-vision-test',
  ['tests/cpp/terminal-color-vision-test.cpp'],
  dependencies: [elder_terms_dep, boxrig_deps['boxrig-ui']])
test('terminal-color-vision-test', color_vision_test)

color_rendering_test = executable('terminal-color-rendering-test',
  'tests/cpp/terminal-color-rendering-test.cpp', dependencies: [boxrig_runtime_dep, elder_terms_dep])
foreach backend : ['cairo', 'opengl']
  test('terminal-color-rendering-' + backend + '-test', find_program('xvfb-run'),
    args: ['-a', color_rendering_test, backend], timeout: 60,
    env: ['LIBGL_ALWAYS_SOFTWARE=1', 'GTK_IM_MODULE=gtk-im-context-simple', 'NO_AT_BRIDGE=1', 'GDK_SCALE=1'] +
      (backend == 'cairo' ? ['GDK_GL=disable'] : []))
endforeach

executable('terminal-color-vision-demo',
  ['tests/cpp/terminal-color-vision-demo.cpp'],
  dependencies: [elder_terms_dep, boxrig_deps['boxrig-ui']])

terminal_renderer_status_test = executable('terminal-renderer-status-test',
  ['tests/cpp/terminal-renderer-status-test.cpp', 'src/terminal-renderer-status.cpp'],
  dependencies: [elder_terms_dep, boxrig_deps['boxrig-gtk3']])
test('terminal-renderer-status-test', find_program('xvfb-run'),
  args: ['-a', terminal_renderer_status_test], timeout: 120,
  env: ['NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

executable('appearance-backdrop', 'tests/cpp/appearance-backdrop.cpp', dependencies: [gtk_dep])

background_image_test = executable('terminal-background-image-test',
  ['tests/cpp/terminal-background-image-test.cpp', 'src/terminal-background-image.cpp'], dependencies: [gtk_dep])
test('terminal-background-image-test', background_image_test, env: ['G_DEBUG=fatal-criticals'])

background_state_test = executable('terminal-background-state-test',
  ['tests/cpp/terminal-background-state-test.cpp', 'src/terminal-background.cpp', 'src/terminal-background-image.cpp'],
  dependencies: [boxrig_runtime_dep, boxrig_background_dep])
test('terminal-background-state-test', find_program('xvfb-run'), args: ['-a', background_state_test],
  env: ['GDK_GL=disable', 'NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals'])

terminal_dispatch_test = executable('terminal-dispatch-test',
  'tests/cpp/terminal-dispatch-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main'])
test('terminal-dispatch-test', find_program('xvfb-run'),
  args: ['-a', terminal_dispatch_test, '--test-fixture'], timeout: 30,
  env: ['GDK_GL=disable', 'GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1'])

appearance_state_test = executable('terminal-appearance-state-test',
  'tests/cpp/terminal-appearance-state-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main', '-Wl,--wrap=boxrig_gtk3_terminal_widget_frame'])
cell_width_test = executable('terminal-cell-width-test',
  'tests/cpp/terminal-cell-width-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main', '-Wl,--wrap=boxrig_gtk3_terminal_widget_frame'])
selection_state_test = executable('terminal-selection-state-test',
  'tests/cpp/terminal-selection-state-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main', '-Wl,--wrap=boxrig_gtk3_terminal_widget_frame',
    '-Wl,--wrap=boxrig_gtk3_terminal_widget_create', '-Wl,--wrap=boxrig_ui_view_update_selection'])
foreach backend : ['cairo', 'opengl']
  foreach scale : ['1', '2']
    appearance_env = ['GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1', 'G_DEBUG=fatal-criticals',
      'GDK_SCALE=' + scale, 'LIBGL_ALWAYS_SOFTWARE=1']
    if backend == 'cairo'
      appearance_env += ['GDK_GL=disable']
    endif
    test('terminal-appearance-state-' + backend + '-' + scale, find_program('xvfb-run'),
      args: ['-a', '-s', '-screen 0 2560x2048x24', appearance_state_test], timeout: 60, env: appearance_env)
    test('terminal-selection-state-' + backend + '-' + scale, find_program('xvfb-run'),
      args: ['-a', '-s', '-screen 0 2560x2048x24', selection_state_test], timeout: 60,
      env: appearance_env + ['GTK_IM_MODULE=gtk-im-context-simple'])
    foreach width : ['narrow', 'wide', 'font', 'default']
      test('terminal-cell-width-' + backend + '-' + scale + '-' + width, find_program('xvfb-run'),
        args: ['-a', '-s', '-screen 0 2560x2048x24', cell_width_test, width], timeout: 60,
        env: appearance_env + ['FONTCONFIG_FILE=' + meson.project_build_root() / 'test-fonts/fonts.conf'])
    endforeach
    if scale == '1'
      foreach limit : ['rows', 'ranges', 'bytes', 'allocation']
        test('terminal-selection-limit-' + backend + '-' + limit, find_program('xvfb-run'),
          args: ['-a', '-s', '-screen 0 2560x2048x24', selection_state_test, limit], timeout: 60,
          env: appearance_env + ['GTK_IM_MODULE=gtk-im-context-simple'])
      endforeach
    endif
  endforeach
endforeach
endif

executable(
  'elder-terms-file-transfer',
  [
    'src/compact-header.cpp',
    'src/inline-prompt.cpp',
    'src/launch-options.cpp',
    'src/ftp/ftp-client.cpp',
    'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp',
    'src/ftp/ftp-metadata.cpp',
    'src/webdav/webdav-application.cpp',
    'src/webdav/webdav-client.cpp', 'src/webdav/webdav-stream.cpp',
    'src/webdav/curl-http-session.cpp',
    'src/webdav/webdav-url.cpp',
    'src/webdav/webdav-metadata.cpp',
    'src/smb/smb-application.cpp',
    'src/smb/smb-client.cpp',
    'src/smb/smb-ssh-proxy.cpp',
    'src/sftp/sftp-client.cpp',
    'src/sftp/sftp-fixture-client.cpp',
    'src/file-transfer/file-hash.cpp',
    'src/file-transfer/ssh-file-hash.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
    'src/file-transfer/file-transfer-main.cpp',
    'src/file-transfer/file-transfer-certificate-prompt.cpp',
    'src/file-transfer/file-transfer-paths.cpp',
    'src/file-transfer/file-transfer-window.cpp',
    'src/file-transfer/activity-file-client.cpp',
    'src/activity-indicator.cpp',
    'src/indicator-color.cpp',
    'src/file-transfer/local-file-reveal.cpp',
    'src/widget-background.cpp',
  ],
  cpp_args: [
    '-I' + gestament_include_dir,
  ],
  dependencies: [
    jemalloc_dep,
    cardio_dep,
    elder_terms_dep,
    gio_dep,
    gtk_dep,
    liburing_dep,
    ssh_transport_dep,
    libcurl_dep,
    libxml2_dep,
    openssl_dep,
    libsmb2_dep,
  ],
  install: true,
  install_dir: elder_terms_private_install_dir / 'elder-terms-terminal',
  install_rpath: elder_terms_private_install_rpath,
)

if get_option('build_tests')
smb_client_test = executable('smb-client-test',
  ['tests/smb/smb-client-test.cpp', 'src/smb/smb-client.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep])
smb_fault_test = executable('smb-fault-test',
  ['tests/smb/smb-fault-test.cpp', 'src/smb/smb-client.cpp',
   'src/file-transfer/file-transfer-engine.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep])
test('smb-client', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_client_test, smb_fault_test], timeout: 180)
smb_transfer_test = executable('smb-transfer-test',
  ['tests/smb/smb-transfer-test.cpp', 'src/smb/smb-client.cpp',
   'src/file-transfer/file-transfer-engine.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep])
test('smb-transfer', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_transfer_test], timeout: 180)

boxrig_serial_transport_test = executable('boxrig-serial-transport-test',
  'tests/cpp/boxrig-serial-transport-test.cpp', dependencies: boxrig_runtime_dep,
  link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=tcsetattr'])
test('boxrig-serial-settings-test', boxrig_serial_transport_test, timeout: 30)
test('boxrig-serial-signals-test', boxrig_serial_transport_test, args: ['signals'], timeout: 30)

boxrig_macros_test = executable('boxrig-macros-test',
  ['tests/cpp/boxrig-macros-test.cpp', 'src/terminal-received-macros.cpp'],
  dependencies: [boxrig_deps['boxrig'], elder_terms_dep])
test('boxrig-macros-test', boxrig_macros_test)

boxrig_links_test = executable('boxrig-links-test',
  ['tests/cpp/boxrig-links-test.cpp', 'src/terminal-links.cpp',
   'src/terminal-hyperlink-action.cpp'],
  dependencies: [boxrig_deps['boxrig-ui'], elder_terms_dep])
test('boxrig-links-test', boxrig_links_test)

boxrig_links_cache_test = executable('boxrig-links-cache-test',
  ['tests/cpp/boxrig-links-cache-test.cpp', 'src/terminal-links.cpp',
   'src/terminal-hyperlink-action.cpp'],
  cpp_args: ['-DELDER_TERMS_CACHE_TESTING'],
  dependencies: [boxrig_deps['boxrig-ui'], elder_terms_dep])
test('boxrig-links-cache-test', boxrig_links_cache_test)

executable('osc8-resolve-cost',
  ['tests/performance/osc8-resolve-cost.cpp', 'src/terminal-links.cpp',
   'src/terminal-hyperlink-action.cpp'],
  dependencies: [boxrig_deps['boxrig-ui'], elder_terms_dep])

boxrig_presentation_test = executable('boxrig-presentation-test',
  'tests/cpp/boxrig-presentation-test.cpp', dependencies: [boxrig_runtime_dep, dependency('epoxy')])
foreach backend : ['opengl', 'cairo']
  test('boxrig-presentation-' + backend + '-test', find_program('xvfb-run'),
    args: ['-a', boxrig_presentation_test, backend], timeout: 30,
    env: ['LIBGL_ALWAYS_SOFTWARE=1', 'GTK_IM_MODULE=gtk-im-context-simple'] +
      (backend == 'cairo' ? ['GDK_GL=disable'] : []))
endforeach

# Compile unchanged C protocol fixtures separately from the C++ application.
# Adding C to the project would also change how generated GTK resources build.
foreach ime : ['fcitx4', 'fcitx5', 'ibus']
  fixture_name = 'boxrig-' + ime + (ime == 'ibus' ? '-engine' : '-server')
  fixture_source = ime == 'ibus' ? '../deps/boxrig/tests/wayland/ibus_engine.c' : '../deps/boxrig/tests/ime/' + ime + '_server.c'
  fixture_args = []
  foreach flag : run_command(boxrig_pkg_config, '--cflags', '--libs', ime == 'ibus' ? 'ibus-1.0' : 'gio-2.0', check: true).stdout().strip().split()
    fixture_args += flag.startswith('-I') ? flag.replace('-I', '-isystem') : flag
  endforeach
  set_variable('boxrig_' + ime + '_fixture', custom_target(fixture_name,
    input: fixture_source, output: fixture_name,
    command: [find_program('cc'), '-std=c99', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
      '@INPUT@', '-o', '@OUTPUT@'] + fixture_args,
    build_by_default: true))
endforeach

boxrig_connection_test = executable('boxrig-connection-test',
  'tests/cpp/boxrig-connection-test.cpp',
  dependencies: [boxrig_runtime_dep],
)
test('boxrig-connection-test', boxrig_connection_test, timeout: 60)

boxrig_runtime_test = executable('boxrig-runtime-test',
  'tests/cpp/boxrig-runtime-test.cpp', dependencies: boxrig_runtime_dep)
test('boxrig-runtime-test', find_program('xvfb-run'),
  args: ['-a', boxrig_runtime_test], timeout: 30,
  env: ['GDK_GL=disable', 'GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1'])

terminal_render_work_test = executable('terminal-render-work-test',
  'tests/cpp/terminal-render-work-test.cpp', dependencies: boxrig_runtime_dep,
  link_args: ['-Wl,--wrap=boxrig_gtk3_terminal_widget_frame'])
test('terminal-render-work-test', find_program('xvfb-run'),
  args: ['-a', terminal_render_work_test], timeout: 30,
  env: ['GDK_GL=disable', 'GTK_IM_MODULE=gtk-im-context-simple', 'GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1'])
test('terminal-render-work-opengl-test', find_program('xvfb-run'),
  args: ['-a', terminal_render_work_test, 'opengl'], timeout: 30,
  env: ['LIBGL_ALWAYS_SOFTWARE=1', 'GTK_IM_MODULE=gtk-im-context-simple', 'GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1'])

boxrig_runtime_macros_test = executable('boxrig-runtime-macros-test',
  'tests/cpp/boxrig-runtime-macros-test.cpp', dependencies: boxrig_runtime_dep)
test('boxrig-runtime-macros-test', find_program('xvfb-run'),
  args: ['-a', boxrig_runtime_macros_test], timeout: 60,
  env: ['GDK_GL=disable', 'GTK_IM_MODULE=gtk-im-context-simple', 'GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1'])

boxrig_ime_test = executable('boxrig-ime-test', 'tests/cpp/boxrig-ime-test.cpp', dependencies: boxrig_runtime_dep)
test('boxrig-fcitx4-runtime-test', find_program('xvfb-run'),
  args: ['-a', 'dbus-run-session', '--config-file=' + meson.project_source_root() / 'deps/boxrig/tests/ime/session.conf',
    '--', 'env', '-u', 'GTK_IM_MODULE', '-u', 'FCITX_DBUS_ADDRESS', 'XMODIFIERS=@im=fcitx', 'IBUS_ADDRESS=',
    'bash', meson.project_source_root() / 'deps/boxrig/tests/ime/run_fcitx4.sh', '--display',
    boxrig_fcitx4_fixture, boxrig_ime_test],
  timeout: 60, env: ['GDK_GL=disable', 'GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1'])

# Compile the unchanged upstream probe with the application's C++20 toolchain.
boxrig_png_isolation_source = configure_file(
  input: '../deps/boxrig/tests/gtk/test_png_isolation.c',
  output: 'boxrig-png-isolation-test.cpp',
  copy: true,
)
boxrig_png_isolation_test = executable('boxrig-png-isolation-test',
  boxrig_png_isolation_source,
  cpp_args: ['-UNDEBUG'],
  dependencies: [boxrig_deps['boxrig'], dependency('gdk-pixbuf-2.0'), gio_dep],
)
test('boxrig-png-isolation-test', boxrig_png_isolation_test)

executable(
  'clipboard-read-helper',
  'tests/cpp/clipboard-read-helper.cpp',
  dependencies: [gtk_dep],
)

executable(
  'clipboard-write-helper',
  'tests/cpp/clipboard-write-helper.cpp',
  dependencies: [gtk_dep],
)

telnet_protocol_test = executable(
  'telnet-protocol-test',
  [
    'tests/cpp/telnet-protocol-test.cpp',
    telnet_protocol_source,
  ],
)

test('telnet-protocol-test', telnet_protocol_test)

webdav_metadata_test = executable(
  'webdav-metadata-test',
  ['tests/cpp/webdav-metadata-test.cpp', 'src/webdav/webdav-metadata.cpp', 'src/webdav/webdav-url.cpp'],
  include_directories: include_directories('src/webdav'),
  dependencies: [cardio_dep, elder_terms_dep, libcurl_dep, ssh_transport_dep, libxml2_dep],
)
test('webdav-metadata-test', webdav_metadata_test)

ssh_proxy_test_server = executable('ssh-proxy-test-server',
  'tests/cpp/ssh-proxy-test-server.cpp', dependencies: [libssh_dep])

smb_share_probe = executable('smb-share-probe',
  ['tests/smb/smb-share-probe.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb-ssh-proxy.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep, ssh_transport_dep])
test('smb-share-probe', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_share_probe], timeout: 180)
test('smb-share-probe-proxy', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_share_probe, '--proxy', ssh_proxy_test_server], timeout: 180)

smb_ssh_proxy_test = executable('smb-ssh-proxy-test',
  ['tests/smb/smb-ssh-proxy-test.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb-ssh-proxy.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep, ssh_transport_dep])
test('smb-ssh-proxy', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_ssh_proxy_test, '--proxy', ssh_proxy_test_server], timeout: 180)

webdav_client_test = executable(
  'webdav-client-test',
  ['tests/cpp/webdav-client-test.cpp', 'src/file-transfer/file-transfer-engine.cpp', 'src/webdav/webdav-client.cpp', 'src/webdav/webdav-stream.cpp',
   'src/webdav/curl-http-session.cpp', 'src/tls/certificate-policy.cpp', 'src/webdav/webdav-metadata.cpp', 'src/webdav/webdav-url.cpp'],
  include_directories: include_directories('src/webdav'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, libxml2_dep, openssl_dep],
)
test('webdav-client-test', find_program('node'),
  args: [files('tests/webdav-client-runner.mjs'), webdav_client_test, ssh_proxy_test_server], timeout: 300)

ftp_metadata_test = executable(
  'ftp-metadata-test',
  [
    'tests/cpp/ftp-metadata-test.cpp',
    'src/ftp/ftp-metadata.cpp',
  ],
  include_directories: include_directories('src/ftp'),
)

test('ftp-metadata-test', ftp_metadata_test)

curl_ftp_session_test = executable(
  'curl-ftp-session-test',
  [
    'tests/cpp/curl-ftp-session-test.cpp',
    'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp',
  ],
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
)

foreach case_name : ['receive', 'cancel-final', 'cancel-paused', 'callback-failure', 'unanswered-quit']
  test('curl-ftp-session-' + case_name, curl_ftp_session_test, args: [case_name], timeout: 120)
endforeach

ftp_test_server = executable(
  'ftp-test-server',
  'tests/cpp/ftp-test-server.cpp',
  dependencies: [openssl_dep, dependency('threads')],
)

ftps_client_test = executable(
  'ftps-client-test',
  ['tests/cpp/ftps-client-test.cpp', 'src/ftp/ftp-client.cpp',
   'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp', 'src/ftp/ftp-metadata.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
)
test('ssh-proxy-ftp-test', find_program('node'),
  args: [files('tests/ssh-proxy-ftp-runner.mjs'), ftps_client_test, ftp_test_server, ssh_proxy_test_server], timeout: 180)



curl_ftp_client_test = executable(
  'curl-ftp-client-test',
  [
    'tests/cpp/curl-ftp-client-test.cpp',
    'src/ftp/ftp-client.cpp',
    'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp',
    'src/ftp/ftp-metadata.cpp',
  ],
  include_directories: include_directories('src/ftp'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
)

foreach case_name : [
  'passive', 'active', 'legacy-passive', 'legacy-active',
  'ipv6-passive', 'ipv6-active', 'unix', 'dos', 'mlsd-unavailable',
  'login', 'empty-user', 'unsafe-password', 'denied-listing',
  'temporary-listing', 'foreign-active', 'fifo',
]
  test('curl-ftp-client-' + case_name, curl_ftp_client_test,
    args: [ftp_test_server, case_name], timeout: 120)
endforeach

curl_ftp_stream_test = executable(
  'curl-ftp-stream-test',
  [
    'tests/cpp/curl-ftp-stream-test.cpp',
    'src/ftp/ftp-client.cpp',
    'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp',
    'src/ftp/ftp-metadata.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
  ],
  include_directories: include_directories('src/ftp'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
)

foreach case_name : [
  'passive', 'active', 'legacy-passive', 'legacy-active',
  'backpressure', 'cancel-backpressure', 'held-upload', 'held-download',
  'cancel-final-upload', 'cancel-final-download', 'store-refused',
  'upload-error', 'download-error', 'upload-disconnect', 'download-disconnect',
  'truncated', 'large-size', 'abandon', 'stop-reader', 'engine', 'engine-failure',
]
  test('curl-ftp-stream-' + case_name, curl_ftp_stream_test,
    args: [ftp_test_server, case_name], timeout: 120)
endforeach

ftp_client_lifetime_test = executable(
  'ftp-client-lifetime-test',
  [
    'tests/cpp/ftp-client-lifetime-test.cpp',
    'src/ftp/ftp-client.cpp',
    'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp',
    'src/ftp/ftp-metadata.cpp',
  ],
  include_directories: include_directories('src/ftp'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
)

foreach case_name : ['normal', 'login', 'cancel']
  test('ftp-client-lifetime-' + case_name, ftp_client_lifetime_test,
    args: [ftp_test_server, case_name], timeout: 120)
endforeach

ftps_certificate_policy_test = executable(
  'ftps-certificate-policy-test',
  ['tests/cpp/ftps-certificate-policy-test.cpp', 'src/tls/certificate-policy.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
  install: false)

ftps_hostname_test = executable(
  'ftps-hostname-test',
  ['tests/cpp/ftps-hostname-test.cpp', 'src/tls/certificate-policy.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
  install: false)

test('ftps-client-test', find_program('node'),
  args: [files('tests/ftps-client-runner.mjs'), ftps_client_test, ftp_test_server,
         curl_ftp_stream_test, ftp_client_lifetime_test, ftps_certificate_policy_test, ftps_hostname_test], timeout: 600)

# The optional integration test modifies only a disposable container's CA store.
# Run it alone so no other test observes its temporary trust entries.
test('ftps-vsftpd', find_program('node'),
  args: [files('tests/ftps-vsftpd-runner.mjs'), ftps_client_test],
  is_parallel: false, timeout: 600)

ftp_runtime_capabilities_test = executable(
  'ftp-runtime-capabilities-test',
  [
    'tests/cpp/ftp-runtime-capabilities-test.cpp',
    'src/ftp/ftp-client.cpp',
    'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp',
    'src/ftp/ftp-metadata.cpp',
  ],
  include_directories: include_directories('src/ftp'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
  link_args: ['-Wl,--wrap=curl_version_info'],
)

foreach case_name : ['active-explicit', 'active-implicit', 'supported', 'no-dns', 'no-ftp', 'old-version', 'no-tls', 'no-ftps', 'non-openssl', 'non-openssl-prompt', 'openssl-mismatch']
  test('ftp-runtime-capabilities-' + case_name, ftp_runtime_capabilities_test,
    args: [ftp_test_server, case_name], timeout: 120)
endforeach

file_transfer_window_lifetime_test = executable(
  'file-transfer-window-lifetime-test',
  [
    'src/compact-header.cpp',
    'tests/cpp/file-transfer-window-lifetime-test.cpp',
    'src/inline-prompt.cpp',
    'src/widget-background.cpp',
    'src/sftp/sftp-fixture-client.cpp',
    'src/file-transfer/file-hash.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
    'src/file-transfer/file-transfer-paths.cpp',
    'src/file-transfer/file-transfer-window.cpp',
    'src/file-transfer/activity-file-client.cpp',
    'src/activity-indicator.cpp',
    'src/indicator-color.cpp',
    'src/file-transfer/local-file-reveal.cpp',
  ],
  include_directories: include_directories('src/file-transfer'),
  cpp_args: ['-I' + gestament_include_dir],
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, gtk_dep],
)

xvfb_run = find_program('xvfb-run')
widget_background_test = executable(
  'widget-background-test',
  ['tests/cpp/widget-background-test.cpp', 'src/widget-background.cpp'],
  dependencies: [elder_terms_dep, gtk_dep],
)
test('widget-background-test', xvfb_run,
  args: ['-a', widget_background_test], timeout: 120)

test('file-transfer-window-lifetime', xvfb_run,
  args: ['-a', file_transfer_window_lifetime_test], timeout: 120)

terminal_text_stream_test = executable(
  'terminal-text-stream-test',
  [
    'tests/cpp/terminal-text-stream-test.cpp',
    'src/terminal-sessions/terminal-text-stream.cpp',
  ],
  dependencies: [
    elder_terms_dep,
  ],
)

test('terminal-text-stream-test', terminal_text_stream_test)



terminal_bell_test = executable(
  'terminal-bell-test',
  [
    'tests/cpp/terminal-bell-test.cpp',
    'src/terminal-bell.cpp',
  ],
  include_directories: include_directories('src'),
  dependencies: [
    elder_terms_dep,
  ],
)

test('terminal-bell-test', terminal_bell_test)


terminal_hyperlink_action_test = executable(
  'terminal-hyperlink-action-test',
  [
    'tests/cpp/terminal-hyperlink-action-test.cpp',
    'src/terminal-hyperlink-action.cpp',
  ],
  dependencies: [
    elder_terms_dep,
  ],
)

test('terminal-hyperlink-action-test', terminal_hyperlink_action_test)

terminal_text_send_runner_test = executable(
  'terminal-text-send-runner-test',
  [
    'tests/cpp/terminal-text-send-runner-test.cpp',
    'src/terminal-text-send.cpp',
    'src/terminal-text-send-runner.cpp',
    'src/terminal-sessions/terminal-text-stream.cpp',
  ],
  dependencies: [
    cardio_dep,
    elder_terms_dep,
    gio_dep,
  ],
)

test('terminal-text-send-runner-test', terminal_text_send_runner_test)

cardio_lifetime_test = executable(
  'cardio-lifetime-test',
  'tests/cpp/cardio-lifetime-test.cpp',
  dependencies: [
    cardio_dep,
    gio_dep,
  ],
)

test('cardio-lifetime-test', cardio_lifetime_test)

activity_indicator_test = executable(
  'activity-indicator-test',
  [
    'tests/cpp/activity-indicator-test.cpp',
    'src/activity-indicator.cpp',
    'src/indicator-color.cpp',
  ],
  dependencies: [
    gtk_dep,
    elder_terms_dep,
  ],
)

test('activity-indicator-test', xvfb_run,
  args: ['-a', activity_indicator_test], timeout: 120)

executable('file-manager-fixture', 'tests/cpp/file-manager-fixture.cpp',
  dependencies: [gio_dep])

executable('file-manager-selection-helper', 'tests/cpp/file-manager-selection-helper.cpp',
  dependencies: [dependency('atspi-2'), dependency('gobject-2.0')])

file_transfer_contract_test = executable(
  'file-transfer-contract-test',
  'tests/cpp/file-transfer-contract-test.cpp',
  dependencies: [
    cardio_dep,
    gio_dep,
  ],
)

test('file-transfer-contract-test', file_transfer_contract_test)

file_hash_test = executable(
  'file-hash-test',
  [
    'tests/cpp/file-hash-test.cpp',
    'src/file-transfer/file-hash.cpp',
  ],
  dependencies: [
    cardio_dep,
    gio_dep,
  ],
)

test('file-hash-test', file_hash_test)

terminal_connection_phase_test = executable(
  'terminal-connection-phase-test',
  [
    'tests/cpp/terminal-connection-phase-test.cpp',
    'src/terminal-connection-phase.cpp',
  ],
  include_directories: include_directories('src'),
  dependencies: [elder_terms_dep],
)

test('terminal-connection-phase-test', terminal_connection_phase_test)


ssh_channel_connection_test = executable(
  'ssh-channel-connection-test',
  [
    'tests/cpp/ssh-channel-connection-test.cpp',
    telnet_protocol_source,
    'src/file-transfer/file-hash.cpp',
    'src/file-transfer/ssh-file-hash.cpp',
    'src/sftp/sftp-client.cpp',
  ],
  dependencies: [
    cardio_dep,
    elder_terms_dep,
    gio_dep,
    ssh_transport_dep,
    liburing_dep,
  ],
  link_args: ['-Wl,--wrap=ssh_channel_write', '-Wl,--wrap=ssh_channel_open_forward'],
)

test('ssh-channel-connection-test', ssh_channel_connection_test,
  timeout: 60)

ssh_channel_write_batch_test = executable(
  'ssh-channel-write-batch-test',
  'tests/cpp/ssh-channel-write-batch-test.cpp',
)

test('ssh-channel-write-batch-test', ssh_channel_write_batch_test)

sftp_transfer_engine_test = executable(
  'sftp-transfer-engine-test',
  [
    'tests/cpp/sftp-transfer-engine-test.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
  ],
  dependencies: [
    cardio_dep,
    gio_dep,
  ],
)

test('sftp-transfer-engine-test', sftp_transfer_engine_test)

terminal_transfer_runner_test = executable(
  'terminal-transfer-runner-test',
  [
    'tests/cpp/terminal-transfer-runner-test.cpp',
    'src/terminal-transfer.cpp',
    'src/terminal-transfer-runner.cpp',
  ],
  cpp_args: [
    '-I' + libxyzm_include_dir,
  ],
  dependencies: [
    cardio_dep,
    gio_dep,
  ],
  link_with: [
    libxyzm_async_static_library,
  ],
)

test('terminal-transfer-runner-unit-test', terminal_transfer_runner_test,
  args: [
    '--unit',
  ])

terminal_transfer_source_buffer_test = executable(
  'terminal-transfer-source-buffer-test',
  'tests/cpp/terminal-transfer-source-buffer-test.cpp',
  dependencies: [cardio_dep],
)

test('terminal-transfer-source-buffer-test',
  terminal_transfer_source_buffer_test)

terminal_transfer_sink_buffer_test = executable(
  'terminal-transfer-sink-buffer-test',
  'tests/cpp/terminal-transfer-sink-buffer-test.cpp',
  dependencies: [cardio_dep],
)
test('terminal-transfer-sink-buffer-test', terminal_transfer_sink_buffer_test)

terminal_transfer_send_buffer_test = executable(
  'terminal-transfer-send-buffer-test',
  'tests/cpp/terminal-transfer-send-buffer-test.cpp',
  cpp_args: ['-I' + libxyzm_include_dir],
  dependencies: [cardio_dep],
)
test('terminal-transfer-send-buffer-test', terminal_transfer_send_buffer_test)

terminal_socket_read_test = executable(
  'terminal-socket-read-test',
  'tests/cpp/terminal-socket-read-test.cpp',
  dependencies: [cardio_dep],
)
test('terminal-socket-read-test', terminal_socket_read_test)

terminal_zmodem_auto_start_test = executable(
  'terminal-zmodem-auto-start-test',
  [
    'tests/cpp/terminal-zmodem-auto-start-test.cpp',
    'src/terminal-transfer.cpp',
    'src/terminal-zmodem-auto-start.cpp',
  ],
)

test('terminal-zmodem-auto-start-test', terminal_zmodem_auto_start_test)

terminal_log_test = executable(
  'terminal-log-test',
  [
    'tests/cpp/terminal-log-test.cpp',
    'src/terminal-log.cpp',
  ],
  dependencies: [
    cardio_dep,
    elder_terms_dep,
    gio_dep,
    liburing_dep,
  ],
)

test('terminal-log-test', terminal_log_test)

terminal_transfer_test_protocols = ['xmodem', 'ymodem', 'zmodem']
terminal_transfer_test_directions = ['send', 'receive']
terminal_transfer_test_delays = ['250', '1000', '3000']
terminal_transfer_option_cases = [
  ['xmodem', 'send', 'xmodem-128'],
  ['xmodem', 'send', 'xmodem-1k'],
  ['xmodem', 'receive', 'xmodem-checksum'],
  ['xmodem', 'receive', 'xmodem-crc'],
  ['ymodem', 'receive', 'ymodem-g'],
]

xyzm_pause_peer = executable(
  'xyzm-pause-peer',
  'tests/cpp/xyzm-pause-peer.cpp',
  cpp_args: [
    '-I' + libxyzm_include_dir,
  ],
  link_with: [
    libxyzm_static_library,
  ],
)

foreach throughput_case : [
  ['zmodem', 'send', 'standard'],
  ['zmodem', 'receive', 'standard'],
  ['ymodem', 'send', 'ymodem-g'],
  ['ymodem', 'receive', 'ymodem-g'],
]
  test(
    'terminal-transfer-runner-throughput-' + throughput_case[0] + '-' +
      throughput_case[1],
    terminal_transfer_runner_test,
    args: ['--throughput-case', throughput_case[0], throughput_case[1],
           '0', throughput_case[2]],
    depends: xyzm_pause_peer,
    timeout: 45,
  )
endforeach

foreach protocol : terminal_transfer_test_protocols
  foreach direction : terminal_transfer_test_directions
    test(
      'terminal-transfer-runner-progress-' + protocol + '-' + direction,
      terminal_transfer_runner_test,
      args: ['--progress-case', protocol, direction, '0'],
      timeout: 45,
    )
    foreach delay : terminal_transfer_test_delays
      test(
        'terminal-transfer-runner-' + protocol + '-' + direction + '-' +
          delay + 'ms',
        terminal_transfer_runner_test,
        args: [
          '--transfer-case',
          protocol,
          direction,
          delay,
        ],
        timeout: 45,
      )
    endforeach
  endforeach
endforeach

foreach option_case : terminal_transfer_option_cases
  foreach delay : terminal_transfer_test_delays
    test(
      'terminal-transfer-runner-' + option_case[2] + '-' +
        option_case[1] + '-' + delay + 'ms',
      terminal_transfer_runner_test,
      args: [
        '--transfer-case',
        option_case[0],
        option_case[1],
        delay,
        option_case[2],
      ],
      timeout: 45,
    )
  endforeach
endforeach

foreach direction : terminal_transfer_test_directions
  test(
    'terminal-transfer-runner-zmodem-resume-' + direction,
    terminal_transfer_runner_test,
    args: [
      '--transfer-case',
      'zmodem',
      direction,
      '250',
      'zmodem-resume',
    ],
    timeout: 45,
  )
endforeach

serial_device_event_monitor_test = executable(
  'serial-device-event-monitor-test',
  [
    'tests/cpp/serial-device-event-monitor-test.cpp',
    '../shared/src/serial-device.cpp',
    '../shared/src/serial-device-event-monitor.cpp',
  ],
  cpp_args: [
    '-DELDER_TERMS_ENABLE_TEST_DOUBLES=1',
  ],
  dependencies: [
    gio_dep,
    libudev_dep,
  ],
  include_directories: elder_terms_shared_include_dir,
)

test('serial-device-event-monitor-test', serial_device_event_monitor_test)

serial_termios_test = executable(
  'serial-termios-test',
  [
    'tests/cpp/serial-termios-test.cpp',
    'src/terminal-sessions/serial-session/serial-termios.cpp',
  ],
  dependencies: [
    elder_terms_dep,
  ],
)

test('serial-termios-test', serial_termios_test)

serial_line_monitor_test = executable(
  'serial-line-monitor-test',
  [
    'tests/cpp/serial-line-monitor-test.cpp',
    'src/terminal-sessions/serial-session/serial-line-monitor.cpp',
  ],
  cpp_args: [
    '-DELDER_TERMS_ENABLE_TEST_DOUBLES=1',
  ],
  dependencies: [
    elder_terms_dep,
  ],
)

test('serial-line-monitor-test', serial_line_monitor_test)

executable(
  'serial-pty-helper',
  'tests/cpp/serial-pty-helper.cpp',
)

endif

configure_file(
  input: 'src/main-window.ui',
  output: 'main-window.ui',
  copy: true,
)

install_data(
  [
    'src/main-window.ui',
    'src/indicators/green-on.png',
    'src/indicators/green-off.png',
  ],
  install_dir: elder_terms_private_install_dir / 'elder-terms-terminal',
)

install_symlink(
  'elder-terms-terminal',
  pointing_to: '..' / elder_terms_private_install_dir / 'elder-terms-terminal/elder-terms-terminal',
  install_dir: get_option('bindir'),
)

install_symlink(
  'elder-terms-file-transfer',
  pointing_to: '..' / elder_terms_private_install_dir / 'elder-terms-terminal/elder-terms-file-transfer',
  install_dir: get_option('bindir'),
)

install_data(
  'packaging/applications/net.kekyo.elder-terms-terminal.desktop',
  install_dir: get_option('datadir') / 'applications',
)

configure_file(
  input: 'src/indicators/green-on.png',
  output: 'green-on.png',
  copy: true,
)

configure_file(
  input: 'src/indicators/green-off.png',
  output: 'green-off.png',
  copy: true,
)

webdav_tls_session_test = executable(
  'webdav-tls-session-test',
  ['tests/cpp/webdav-tls-session-test.cpp', 'src/webdav/curl-http-session.cpp',
   'src/tls/certificate-policy.cpp'],
  include_directories: include_directories('src/webdav'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
)
test('webdav-tls-session-test', find_program('node'),
  args: [files('tests/webdav-tls-session-runner.mjs'), webdav_tls_session_test], timeout: 120)
if get_option('build_tests')
test('webdav-proxy-tls-session-test', find_program('node'),
  args: [files('tests/webdav-tls-session-runner.mjs'), webdav_tls_session_test, ssh_proxy_test_server], timeout: 120)
endif

webdav_apache_test = executable(
  'webdav-apache-test',
  ['tests/cpp/webdav-apache-test.cpp', 'src/file-transfer/file-transfer-engine.cpp', 'src/webdav/webdav-client.cpp', 'src/webdav/webdav-stream.cpp',
   'src/webdav/curl-http-session.cpp', 'src/tls/certificate-policy.cpp', 'src/webdav/webdav-metadata.cpp', 'src/webdav/webdav-url.cpp'],
  include_directories: include_directories('src/webdav'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, libxml2_dep, openssl_dep],
)
test('webdav-apache-test', find_program('node'),
  args: [files('tests/webdav-apache-runner.mjs'), webdav_apache_test], timeout: 900, is_parallel: false)
