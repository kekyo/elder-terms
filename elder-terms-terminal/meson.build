gtk_dep = dependency('gtk+-3.0')
canberra_dep = dependency('libcanberra')
gio_dep = dependency('gio-2.0')
liburing_dep = dependency('liburing')
libudev_dep = dependency('libudev')

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
executable('boxrig-driver', ['src/boxrig-driver.cpp', 'src/terminal-log.cpp'],
  cpp_args: ['-I' + gestament_include_dir],
  dependencies: [file_transfer_dep, boxrig_runtime_dep],
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
  'src/gtk-initialization.cpp',
  'src/main.cpp',
  'src/main-window.cpp',
  'src/terminal-background.cpp',
  'src/terminal-background-image.cpp',
  'src/terminal-bell.cpp',
  'src/terminal-bell-player.cpp',
  'src/terminal-log.cpp',
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
    file_transfer_dep,
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
titlebar_effect_state_test = executable('titlebar-effect-state-test',
  ['tests/cpp/titlebar-effect-state-test.cpp', '../shared/src/connection-runtime/titlebar-effect-state.cpp'], dependencies: elder_terms_dep)
test('titlebar-effect-state-test', titlebar_effect_state_test)
titlebar_effects_gtk_test = executable('titlebar-effects-gtk-test',
  ['tests/cpp/titlebar-effects-gtk-test.cpp', '../shared/src/connection-runtime/titlebar-effect-state.cpp', '../shared/src/connection-runtime/titlebar-effects.cpp'],
  dependencies: [elder_terms_dep, gtk_dep], link_args: ['-Wl,--wrap=cairo_image_surface_create'])
test('titlebar-effects-gtk-test', find_program('xvfb-run'),
  args: ['-a', titlebar_effects_gtk_test], timeout: 30,
  env: ['G_DEBUG=fatal-criticals', 'NO_AT_BRIDGE=1'])
if find_program('kwin_x11', required: false).found()
  test('titlebar-effects-window-manager-test', find_program('xvfb-run'),
    args: ['-a', 'dbus-run-session', '--', 'bash',
      meson.current_source_dir() / 'tests/titlebar-window-manager.sh', titlebar_effects_gtk_test],
    timeout: 45, env: ['G_DEBUG=fatal-criticals', 'NO_AT_BRIDGE=1'])
endif
executable('titlebar-effects-demo',
  ['tests/cpp/titlebar-effects-demo.cpp'],
  dependencies: [connection_ui_dep, gtk_dep])

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
  dependencies: [file_transfer_dep, cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main'])
test('terminal-dispatch-test', find_program('xvfb-run'),
  args: ['-a', terminal_dispatch_test, '--test-fixture'], timeout: 30,
  env: ['GDK_GL=disable', 'GSETTINGS_BACKEND=memory', 'NO_AT_BRIDGE=1'])

appearance_state_test = executable('terminal-appearance-state-test',
  'tests/cpp/terminal-appearance-state-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [file_transfer_dep, cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main', '-Wl,--wrap=boxrig_gtk3_terminal_widget_frame'])
terminal_titlebar_test = executable('terminal-titlebar-test',
  'tests/cpp/terminal-titlebar-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [file_transfer_dep, cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main'])
test('terminal-titlebar-test', find_program('xvfb-run'), args: ['-a', terminal_titlebar_test],
  timeout: 30, env: ['G_DEBUG=fatal-criticals', 'GDK_GL=disable', 'NO_AT_BRIDGE=1'])
cell_width_test = executable('terminal-cell-width-test',
  'tests/cpp/terminal-cell-width-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [file_transfer_dep, cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
    liburing_dep, ssh_transport_dep, libudev_dep, boxrig_runtime_dep, boxrig_background_dep],
  link_with: libxyzm_async_static_library,
  link_args: ['-Wl,--wrap=main', '-Wl,--wrap=boxrig_gtk3_terminal_widget_frame'])
selection_state_test = executable('terminal-selection-state-test',
  'tests/cpp/terminal-selection-state-test.cpp',
  objects: elder_terms_terminal.extract_all_objects(recursive: false),
  dependencies: [file_transfer_dep, cardio_dep, elder_terms_dep, canberra_dep, gio_dep, gtk_dep,
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

if get_option('build_tests')
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

boxrig_local_links_test = executable('boxrig-local-links-test',
  ['tests/cpp/boxrig-local-links-test.cpp', 'src/terminal-links.cpp',
   'src/terminal-hyperlink-action.cpp'],
  dependencies: [boxrig_deps['boxrig-ui'], elder_terms_dep])
test('boxrig-local-links-test', boxrig_local_links_test)

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

gtk_initialization_test = executable('gtk-initialization-test',
  ['tests/cpp/gtk-initialization-test.cpp', 'src/gtk-initialization.cpp'], dependencies: gtk_dep)
foreach mode : ['default', 'override']
  test('gtk-initialization-' + mode + '-test', find_program('xvfb-run'),
    args: ['-a', gtk_initialization_test, mode], env: ['GDK_BACKEND=x11'], timeout: 30)
endforeach

boxrig_presentation_test = executable('boxrig-presentation-test',
  ['tests/cpp/boxrig-presentation-test.cpp', 'src/gtk-initialization.cpp'],
  dependencies: [boxrig_runtime_dep, dependency('epoxy')])
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

ssh_channel_connection_test = executable(
  'ssh-channel-connection-test',
  [
    'tests/cpp/ssh-channel-connection-test.cpp',
    telnet_protocol_source,
  ],
  dependencies: [
    cardio_dep,
    elder_terms_dep,
    gio_dep,
    file_transfer_dep,
    liburing_dep,
  ],
  link_args: ['-Wl,--wrap=ssh_channel_write', '-Wl,--wrap=ssh_channel_open_forward'],
)

test('ssh-channel-connection-test', ssh_channel_connection_test,
  timeout: 60)

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
    '../shared/indicators/green-on.png',
    '../shared/indicators/green-off.png',
  ],
  install_dir: elder_terms_private_install_dir / 'elder-terms-terminal',
)

install_symlink(
  'elder-terms-terminal',
  pointing_to: '..' / elder_terms_private_install_dir / 'elder-terms-terminal/elder-terms-terminal',
  install_dir: get_option('bindir'),
)

install_data(
  'packaging/applications/net.kekyo.elder-terms-terminal.desktop',
  install_dir: get_option('datadir') / 'applications',
)

configure_file(
  input: '../shared/indicators/green-on.png',
  output: 'green-on.png',
  copy: true,
)

configure_file(
  input: '../shared/indicators/green-off.png',
  output: 'green-off.png',
  copy: true,
)
