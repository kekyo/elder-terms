gtk_dep = dependency('gtk+-3.0')
canberra_dep = dependency('libcanberra')
gio_dep = dependency('gio-2.0')
liburing_dep = dependency('liburing')
libudev_dep = dependency('libudev')
libssh_dep = dependency('libssh')
vte_dep = dependency('vte-2.91')

gestament_config = find_program('gestament-config')
gestament_include_dir = run_command(
  gestament_config,
  ['--includedir'],
  check: true,
).stdout().strip()
make_program = find_program('make')
cardio_root_dir = meson.project_source_root() / 'deps/cardio'
cardio_include_dir = cardio_root_dir / 'include'
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

telnet_protocol_source = 'src/terminal-sessions/telnet-session/telnet-protocol.cpp'
elder_terms_sources = [
  'src/activity-indicator.cpp',
  'src/launch-options.cpp',
  'src/main.cpp',
  'src/main-window.cpp',
  'src/sftp/sftp-client.cpp',
  'src/sftp/sftp-fixture-client.cpp',
  'src/sftp/sftp-paths.cpp',
  'src/file-transfer/file-transfer-engine.cpp',
  'src/file-transfer/file-transfer-window.cpp',
  'src/terminal-connection-phase.cpp',
  'src/terminal-bell.cpp',
  'src/terminal-bell-player.cpp',
  'src/terminal-font.cpp',
  'src/terminal-hyperlink-resolver.cpp',
  'src/terminal-log.cpp',
  'src/terminal-macro-runner.cpp',
  'src/terminal-text-send.cpp',
  'src/terminal-text-send-runner.cpp',
  'src/terminal-transfer.cpp',
  'src/terminal-transfer-runner.cpp',
  'src/terminal-zmodem-auto-start.cpp',
  'src/terminal-sessions/local-session/local-session.cpp',
  'src/terminal-sessions/serial-session/serial-line-monitor.cpp',
  'src/terminal-sessions/serial-session/serial-session.cpp',
  'src/terminal-sessions/serial-session/serial-termios.cpp',
  'src/terminal-sessions/ssh-session/ssh-channel-connection.cpp',
  'src/terminal-sessions/ssh-session/ssh-session.cpp',
  'src/terminal-sessions/tcp-connector.cpp',
  telnet_protocol_source,
  'src/terminal-sessions/telnet-session/telnet-session.cpp',
  'src/terminal-sessions/terminal-keyboard-protocol.cpp',
  'src/terminal-sessions/terminal-text-codec.cpp',
  'src/terminal-sessions/terminal-view-io.cpp',
  'src/terminal-layout.cpp',
  'src/terminal-session.cpp',
  'src/widget-background.cpp',
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
    elder_terms_dep,
    canberra_dep,
    gio_dep,
    gtk_dep,
    liburing_dep,
    libssh_dep,
    libudev_dep,
    vte_dep,
  ],
  link_with: [
    libxyzm_async_static_library,
  ],
  install: true,
  install_dir: elder_terms_private_install_dir / 'elder-terms-vte',
  install_rpath: elder_terms_private_install_rpath,
)

executable(
  'elder-terms-sftp',
  [
    'src/launch-options.cpp',
    'src/sftp/sftp-client.cpp',
    'src/sftp/sftp-fixture-client.cpp',
    'src/sftp/sftp-main.cpp',
    'src/sftp/sftp-paths.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
    'src/file-transfer/file-transfer-window.cpp',
    'src/terminal-sessions/ssh-session/ssh-channel-connection.cpp',
    'src/terminal-sessions/tcp-connector.cpp',
    'src/widget-background.cpp',
  ],
  cpp_args: [
    '-I' + gestament_include_dir,
    '-I' + cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
    '-DCARDIO_WITH_LINUX_IO_URING=1',
  ],
  dependencies: [
    elder_terms_dep,
    gio_dep,
    gtk_dep,
    liburing_dep,
    libssh_dep,
  ],
  install: true,
  install_dir: elder_terms_private_install_dir / 'elder-terms-vte',
  install_rpath: elder_terms_private_install_rpath,
)

if get_option('build_tests')
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

ftp_protocol_test = executable(
  'ftp-protocol-test',
  [
    'tests/cpp/ftp-protocol-test.cpp',
    'src/ftp/ftp-protocol.cpp',
  ],
)

test('ftp-protocol-test', ftp_protocol_test)

terminal_text_codec_test = executable(
  'terminal-text-codec-test',
  [
    'tests/cpp/terminal-text-codec-test.cpp',
    'src/terminal-sessions/terminal-text-codec.cpp',
  ],
  dependencies: [
    elder_terms_dep,
  ],
)

test('terminal-text-codec-test', terminal_text_codec_test)

terminal_keyboard_protocol_test = executable(
  'terminal-keyboard-protocol-test',
  [
    'tests/cpp/terminal-keyboard-protocol-test.cpp',
    'src/terminal-sessions/terminal-keyboard-protocol.cpp',
  ],
)

test('terminal-keyboard-protocol-test', terminal_keyboard_protocol_test)

terminal_font_test = executable(
  'terminal-font-test',
  [
    'tests/cpp/terminal-font-test.cpp',
    'src/terminal-font.cpp',
  ],
  dependencies: [
    elder_terms_dep,
    gtk_dep,
  ],
)

test('terminal-font-test', terminal_font_test)

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

terminal_macro_runner_test = executable(
  'terminal-macro-runner-test',
  [
    'tests/cpp/terminal-macro-runner-test.cpp',
    'src/terminal-macro-runner.cpp',
  ],
  dependencies: [
    elder_terms_dep,
  ],
)

test('terminal-macro-runner-test', terminal_macro_runner_test)

terminal_hyperlink_resolver_test = executable(
  'terminal-hyperlink-resolver-test',
  [
    'tests/cpp/terminal-hyperlink-resolver-test.cpp',
    'src/terminal-hyperlink-resolver.cpp',
  ],
  dependencies: [
    elder_terms_dep,
  ],
)

test('terminal-hyperlink-resolver-test', terminal_hyperlink_resolver_test)

terminal_text_send_runner_test = executable(
  'terminal-text-send-runner-test',
  [
    'tests/cpp/terminal-text-send-runner-test.cpp',
    'src/terminal-text-send.cpp',
    'src/terminal-text-send-runner.cpp',
    'src/terminal-sessions/terminal-text-codec.cpp',
  ],
  cpp_args: [
    '-I' + cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
  ],
  dependencies: [
    elder_terms_dep,
    gio_dep,
  ],
)

test('terminal-text-send-runner-test', terminal_text_send_runner_test)

cardio_lifetime_test = executable(
  'cardio-lifetime-test',
  'tests/cpp/cardio-lifetime-test.cpp',
  cpp_args: [
    '-I' + cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
  ],
  dependencies: [
    gio_dep,
  ],
)

test('cardio-lifetime-test', cardio_lifetime_test)

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

file_transfer_contract_test = executable(
  'file-transfer-contract-test',
  'tests/cpp/file-transfer-contract-test.cpp',
  cpp_args: [
    '-I' + cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
  ],
  dependencies: [
    gio_dep,
  ],
)

test('file-transfer-contract-test', file_transfer_contract_test)

terminal_connection_phase_test = executable(
  'terminal-connection-phase-test',
  [
    'tests/cpp/terminal-connection-phase-test.cpp',
    'src/terminal-connection-phase.cpp',
  ],
  include_directories: include_directories('src'),
)

test('terminal-connection-phase-test', terminal_connection_phase_test)

ssh_channel_connection_test = executable(
  'ssh-channel-connection-test',
  [
    'tests/cpp/ssh-channel-connection-test.cpp',
    'src/sftp/sftp-client.cpp',
    'src/terminal-sessions/ssh-session/ssh-channel-connection.cpp',
    'src/terminal-sessions/tcp-connector.cpp',
  ],
  cpp_args: [
    '-I' + cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
    '-DCARDIO_WITH_LINUX_IO_URING=1',
  ],
  dependencies: [
    elder_terms_dep,
    gio_dep,
    libssh_dep,
    liburing_dep,
  ],
)

test('ssh-channel-connection-test', ssh_channel_connection_test,
  timeout: 60)

sftp_transfer_engine_test = executable(
  'sftp-transfer-engine-test',
  [
    'tests/cpp/sftp-transfer-engine-test.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
  ],
  cpp_args: [
    '-I' + cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
  ],
  dependencies: [
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

terminal_log_test = executable(
  'terminal-log-test',
  [
    'tests/cpp/terminal-log-test.cpp',
    'src/terminal-log.cpp',
  ],
  cpp_args: [
    '-I' + cardio_include_dir,
    '-DCARDIO_WITH_GLIB=1',
    '-DCARDIO_WITH_GIO=1',
    '-DCARDIO_WITH_LINUX_IO_URING=1',
  ],
  dependencies: [
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
  install_dir: elder_terms_private_install_dir / 'elder-terms-vte',
)

install_symlink(
  'elder-terms-vte',
  pointing_to: '..' / elder_terms_private_install_dir / 'elder-terms-vte/elder-terms-vte',
  install_dir: get_option('bindir'),
)

install_symlink(
  'elder-terms-sftp',
  pointing_to: '..' / elder_terms_private_install_dir / 'elder-terms-vte/elder-terms-sftp',
  install_dir: get_option('bindir'),
)

install_data(
  'packaging/applications/net.kekyo.elder-terms-vte.desktop',
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
