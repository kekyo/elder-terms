gtk_dep = dependency('gtk+-3.0')
gio_dep = dependency('gio-2.0')
liburing_dep = dependency('liburing')
libudev_dep = dependency('libudev')
vte_dep = dependency('vte-2.91')

gestament_config = find_program('npx')
gestament_include_dir = run_command(
  gestament_config,
  ['gestament-config', '--includedir'],
  check: true,
).stdout().strip()
cardio_include_dir = meson.current_source_dir() / '../../cardio/include'
libxyzm_root_dir = meson.current_source_dir() / '../../libxyzm'
libxyzm_include_dir = libxyzm_root_dir / 'include'
libxyzm_async_static_library = libxyzm_root_dir / 'build/libxyzm_async.a'

telnet_protocol_source = 'src/terminal-sessions/telnet-session/telnet-protocol.cpp'
elder_terms_sources = [
  'src/activity-indicator.cpp',
  'src/launch-options.cpp',
  'src/main.cpp',
  'src/main-window.cpp',
  'src/terminal-transfer.cpp',
  'src/terminal-transfer-runner.cpp',
  'src/terminal-sessions/local-session/local-session.cpp',
  'src/terminal-sessions/serial-session/serial-device-event-monitor.cpp',
  'src/terminal-sessions/serial-session/serial-device-resolver.cpp',
  'src/terminal-sessions/serial-session/serial-line-monitor.cpp',
  'src/terminal-sessions/serial-session/serial-session.cpp',
  'src/terminal-sessions/serial-session/serial-termios.cpp',
  telnet_protocol_source,
  'src/terminal-sessions/telnet-session/telnet-session.cpp',
  'src/terminal-sessions/terminal-view-io.cpp',
  'src/terminal-layout.cpp',
  'src/terminal-session.cpp',
]

executable(
  'elder-terms-vte',
  elder_terms_sources,
  cpp_args: [
    '-I' + gestament_include_dir,
    '-I' + cardio_include_dir,
    '-I' + libxyzm_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
    '-DCARDIO_WITH_LINUX_IO_URING=1',
  ],
  dependencies: [
    elder_terms_shared_dep,
    gio_dep,
    gtk_dep,
    liburing_dep,
    libudev_dep,
    vte_dep,
  ],
  link_args: [
    libxyzm_async_static_library,
  ],
)

telnet_protocol_test = executable(
  'telnet-protocol-test',
  [
    'tests/cpp/telnet-protocol-test.cpp',
    telnet_protocol_source,
  ],
)

test('telnet-protocol-test', telnet_protocol_test)

activity_indicator_test = executable(
  'activity-indicator-test',
  [
    'tests/cpp/activity-indicator-test.cpp',
    'src/activity-indicator.cpp',
  ],
  dependencies: [
    gtk_dep,
  ],
)

test('activity-indicator-test', activity_indicator_test)

terminal_transfer_runner_test = executable(
  'terminal-transfer-runner-test',
  [
    'tests/cpp/terminal-transfer-runner-test.cpp',
    'src/terminal-transfer.cpp',
    'src/terminal-transfer-runner.cpp',
  ],
  cpp_args: [
    '-I' + cardio_include_dir,
    '-I' + libxyzm_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
  ],
  dependencies: [
    gio_dep,
  ],
  link_args: [
    libxyzm_async_static_library,
  ],
)

test('terminal-transfer-runner-test', terminal_transfer_runner_test,
  timeout: 90)

serial_device_resolver_test = executable(
  'serial-device-resolver-test',
  [
    'tests/cpp/serial-device-resolver-test.cpp',
    'src/terminal-sessions/serial-session/serial-device-resolver.cpp',
  ],
  dependencies: [
    elder_terms_shared_dep,
  ],
)

test('serial-device-resolver-test', serial_device_resolver_test)

serial_device_event_monitor_test = executable(
  'serial-device-event-monitor-test',
  [
    'tests/cpp/serial-device-event-monitor-test.cpp',
    'src/terminal-sessions/serial-session/serial-device-event-monitor.cpp',
  ],
  cpp_args: [
    '-DELDER_TERMS_ENABLE_TEST_DOUBLES=1',
  ],
  dependencies: [
    gio_dep,
    libudev_dep,
  ],
)

test('serial-device-event-monitor-test', serial_device_event_monitor_test)

serial_termios_test = executable(
  'serial-termios-test',
  [
    'tests/cpp/serial-termios-test.cpp',
    'src/terminal-sessions/serial-session/serial-termios.cpp',
  ],
  dependencies: [
    elder_terms_shared_dep,
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
    elder_terms_shared_dep,
  ],
)

test('serial-line-monitor-test', serial_line_monitor_test)

executable(
  'serial-pty-helper',
  'tests/cpp/serial-pty-helper.cpp',
)

configure_file(
  input: 'src/main-window.ui',
  output: 'main-window.ui',
  copy: true,
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
