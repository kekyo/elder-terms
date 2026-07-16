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
make_program = find_program('make')
cardio_root_dir = meson.project_source_root() / 'deps/cardio'
cardio_include_dir = cardio_root_dir / 'include'
libxyzm_root_dir = meson.project_source_root() / 'deps/libxyzm'
libxyzm_include_dir = libxyzm_root_dir / 'include'
libxyzm_build_dir = meson.current_build_dir()
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
    'CARDIO_DIR=' + cardio_root_dir,
    'BUILD_DIR=' + libxyzm_build_dir,
    libxyzm_build_dir / 'libxyzm.a',
    libxyzm_build_dir / 'libxyzm_async.a',
  ],
  build_by_default: true,
  build_always_stale: true,
)
libxyzm_static_library = libxyzm_static_libraries[0]
libxyzm_async_static_library = libxyzm_static_libraries[1]

telnet_protocol_source = 'src/terminal-sessions/telnet-session/telnet-protocol.cpp'
elder_terms_sources = [
  'src/activity-indicator.cpp',
  'src/launch-options.cpp',
  'src/main.cpp',
  'src/main-window.cpp',
  'src/terminal-transfer.cpp',
  'src/terminal-transfer-runner.cpp',
  'src/terminal-zmodem-auto-start.cpp',
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
  link_with: [
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
  link_with: [
    libxyzm_async_static_library,
  ],
)

test('terminal-transfer-runner-unit-test', terminal_transfer_runner_test,
  args: [
    '--unit',
  ])

terminal_zmodem_auto_start_test = executable(
  'terminal-zmodem-auto-start-test',
  [
    'tests/cpp/terminal-zmodem-auto-start-test.cpp',
    'src/terminal-transfer.cpp',
    'src/terminal-zmodem-auto-start.cpp',
  ],
)

test('terminal-zmodem-auto-start-test', terminal_zmodem_auto_start_test)

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

foreach protocol : terminal_transfer_test_protocols
  foreach direction : terminal_transfer_test_directions
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

executable(
  'xyzm-pause-peer',
  'tests/cpp/xyzm-pause-peer.cpp',
  cpp_args: [
    '-I' + libxyzm_include_dir,
  ],
  link_with: [
    libxyzm_static_library,
  ],
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
