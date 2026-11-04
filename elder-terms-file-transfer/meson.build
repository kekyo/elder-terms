gtk_dep = dependency('gtk+-3.0')
gio_dep = dependency('gio-2.0')
liburing_dep = dependency('liburing')
libcurl_dep = dependency('libcurl', version: '>=7.88.1')
libxml2_dep = dependency('libxml-2.0', version: '>=2.9.14')
openssl_dep = dependency('openssl')

# Reuse the SFTP browser in SSH terminals without another SSH connection.
file_transfer_library = static_library('elder-terms-file-transfer-core',
  ['src/file-transfer/activity-file-client.cpp',
   'src/file-transfer/file-hash.cpp',
   'src/file-transfer/file-transfer-engine.cpp',
   'src/file-transfer/file-transfer-job-view.cpp',
   'src/file-transfer/file-transfer-job.cpp',
   'src/file-transfer/file-transfer-paths.cpp',
   'src/file-transfer/file-transfer-window.cpp',
   'src/file-transfer/local-file-reveal.cpp',
   'src/file-transfer/ssh-file-hash.cpp',
   'src/sftp/sftp-client.cpp',
   'src/sftp/sftp-fixture-client.cpp'],
  cpp_args: ['-I' + gestament_include_dir],
  dependencies: [connection_ui_dep, ssh_transport_dep])
file_transfer_dep = declare_dependency(link_with: file_transfer_library,
  dependencies: [connection_ui_dep, ssh_transport_dep])

executable(
  'elder-terms-file-transfer',
  [
    'src/ftp/ftp-client.cpp',
    'src/ftp/curl-ftp-session.cpp', 'src/tls/certificate-policy.cpp',
    'src/ftp/ftp-metadata.cpp',
    'src/webdav/webdav-application.cpp',
    'src/http/http-application.cpp', 'src/http/http-client.cpp', 'src/http/http-crawler.cpp',
    'src/webdav/webdav-client.cpp', 'src/webdav/webdav-stream.cpp',
    'src/http/curl-http-session.cpp', 'src/http/curl-http-stream.cpp',
    'src/webdav/webdav-url.cpp',
    'src/webdav/webdav-metadata.cpp',
    'src/smb/smb-application.cpp',
    'src/nfs/nfs-application.cpp', 'src/nfs/nfs-client.cpp',
    'src/nfs/nfs-v2.cpp', 'src/nfs/nfs-ssh-proxy.cpp',
    'src/nfs/nfs-name.cpp', 'src/nfs/nfs-rpc.cpp',
    'src/smb/smb-client.cpp', 'src/smb/smb1-client.cpp', 'src/smb/smb-share-list.cpp',
    'src/main.cpp',
    'src/file-transfer/file-transfer-certificate-prompt.cpp',
  ],
  cpp_args: [
    '-I' + gestament_include_dir,
  ],
  dependencies: [file_transfer_dep,
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
    nfs_dep,
  ],
  install: true,
  install_dir: elder_terms_private_install_dir / 'elder-terms-file-transfer',
  install_rpath: elder_terms_private_install_rpath,
)

smb1_worker = executable('elder-terms-smb1-worker', 'src/smb/smb1-worker.cpp',
  dependencies: [cardio_dep, libdsm_dep, dependency('gio-unix-2.0')],
  install: true, install_dir: elder_terms_private_install_dir / 'elder-terms-file-transfer',
  install_rpath: elder_terms_private_install_rpath)

if get_option('build_tests')
smb1_client_test = executable('smb1-client-test',
  ['tests/smb/smb1-client-test.cpp', 'src/file-transfer/file-transfer-engine.cpp', 'src/smb/smb1-client.cpp',
   'src/smb/smb-client.cpp', 'src/smb/smb-share-list.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep])
smb1_library_probe = executable('smb1-library-probe', 'tests/smb/smb1-library-probe.cpp',
  dependencies: [libdsm_dep])
test('smb1-library', find_program('node'),
  args: [files('tests/smb/run-smb1-library.mts'), smb1_library_probe], timeout: 180)
test('smb1-client', find_program('node'),
  args: [files('tests/smb/run-smb1-library.mts'), smb1_client_test, '--client'],
  depends: smb1_worker, timeout: 180)
test('smb1-fault', find_program('node'),
  args: [files('tests/smb/run-smb1-fault.mts'), smb1_client_test],
  depends: smb1_worker, timeout: 180)
test('smb1-known-share', find_program('node'),
  args: [files('tests/smb/run-smb1-library.mts'), smb1_client_test, '--client'],
  env: ['ELDER_SMB_DENY_ENUMERATION=1'], depends: smb1_worker, timeout: 180)
test('smb1-fragmented-shares', find_program('node'),
  args: [files('tests/smb/run-smb1-library.mts'), smb1_client_test, '--client'],
  env: ['ELDER_SMB_SHARE_COUNT=200'], depends: smb1_worker, timeout: 180)
smb_client_test = executable('smb-client-test',
  ['tests/smb/smb-client-test.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb1-client.cpp', 'src/smb/smb-share-list.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep])
smb_fault_test = executable('smb-fault-test',
  ['tests/smb/smb-fault-test.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb1-client.cpp', 'src/smb/smb-share-list.cpp',
   'src/file-transfer/file-transfer-engine.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep])
test('smb-client', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_client_test, smb_fault_test], timeout: 180)
smb_transfer_test = executable('smb-transfer-test',
  ['tests/smb/smb-transfer-test.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb1-client.cpp', 'src/smb/smb-share-list.cpp',
   'src/file-transfer/file-transfer-engine.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep])
test('smb-transfer', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_transfer_test], timeout: 180)

http_client_test = executable(
  'http-client-test',
  ['tests/cpp/http-client-test.cpp', 'src/http/http-client.cpp', 'src/http/http-crawler.cpp',
   'src/file-transfer/file-transfer-job.cpp',
   'src/file-transfer/file-transfer-engine.cpp', 'src/http/curl-http-session.cpp', 'src/http/curl-http-stream.cpp', 'src/tls/certificate-policy.cpp'],
  include_directories: include_directories('src/http'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, libxml2_dep, openssl_dep],
)
test('http-client-test', find_program('node'),
  args: [files('tests/http-client-runner.mjs'), http_client_test], timeout: 60)

http_crawler_test = executable(
  'http-crawler-test',
  ['tests/cpp/http-crawler-test.cpp', 'src/http/http-crawler.cpp',
   'src/http/curl-http-session.cpp', 'src/http/curl-http-stream.cpp', 'src/tls/certificate-policy.cpp'],
  include_directories: include_directories('src/http'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, libxml2_dep, openssl_dep],
)
test('http-crawler-test', find_program('node'),
  args: [files('tests/http-crawler-runner.mjs'), http_crawler_test], timeout: 60)

webdav_metadata_test = executable(
  'webdav-metadata-test',
  ['tests/cpp/webdav-metadata-test.cpp', 'src/webdav/webdav-metadata.cpp', 'src/webdav/webdav-url.cpp'],
  include_directories: include_directories('src/webdav'),
  dependencies: [cardio_dep, elder_terms_dep, libcurl_dep, ssh_transport_dep, libxml2_dep],
)
test('webdav-metadata-test', webdav_metadata_test)

nfs_ssh_proxy_test = executable('nfs-ssh-proxy-test',
  ['tests/nfs/nfs-ssh-proxy-test.cpp', 'src/nfs/nfs-ssh-proxy.cpp',
   'src/nfs/nfs-client.cpp', 'src/nfs/nfs-v2.cpp', 'src/nfs/nfs-rpc.cpp', 'src/nfs/nfs-name.cpp'],
  dependencies: [file_transfer_dep, nfs_dep])
test('nfs-ssh-proxy-discovery', find_program('node'),
  args: [files('tests/nfs/run-proxy-discovery.mts'), nfs_ssh_proxy_test, ssh_proxy_test_server], timeout: 60)

smb_share_probe = executable('smb-share-probe',
  ['tests/smb/smb-share-probe.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb1-client.cpp', 'src/smb/smb-share-list.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep, ssh_transport_dep])
test('smb-share-probe', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_share_probe], timeout: 180)
test('smb-share-probe-proxy', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_share_probe, '--proxy', ssh_proxy_test_server], timeout: 180)

smb_server_test = executable('smb-server-test',
  ['tests/smb/smb-server-test.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb1-client.cpp', 'src/smb/smb-share-list.cpp',
   'src/file-transfer/activity-file-client.cpp', 'src/file-transfer/file-transfer-engine.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep, ssh_transport_dep])
test('smb-server', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_server_test], timeout: 180)
test('smb-server-proxy', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_server_test, '--server-proxy', ssh_proxy_test_server], timeout: 180)
if meson.get_compiler('cpp').has_link_argument('-fsanitize=leak')
  # Standalone LeakSanitizer needs only a different link of the same driver.
  smb_server_lifetime_test = executable('smb-server-lifetime-test',
    objects: smb_server_test.extract_all_objects(recursive: false),
    dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep, ssh_transport_dep],
    link_language: 'cpp', link_args: ['-fsanitize=leak'])
  test('smb-server-proxy-lifetime', find_program('node'),
    args: [files('tests/smb/run-client.mts'), smb_server_lifetime_test, '--server-proxy', ssh_proxy_test_server],
    env: ['LSAN_OPTIONS=detect_leaks=1:exitcode=23'], timeout: 180)
endif

smb_share_list_test = executable('smb-share-list-test',
  ['tests/smb/smb-share-list-test.cpp', 'src/smb/smb-share-list.cpp'],
  dependencies: [cardio_dep, libsmb2_dep])
test('smb-share-list', smb_share_list_test)

smb_ssh_proxy_test = executable('smb-ssh-proxy-test',
  ['tests/smb/smb-ssh-proxy-test.cpp', 'src/smb/smb-client.cpp', 'src/smb/smb1-client.cpp', 'src/smb/smb-share-list.cpp'],
  dependencies: [cardio_dep, elder_terms_dep, libsmb2_dep, ssh_transport_dep])
test('smb-ssh-proxy', find_program('node'),
  args: [files('tests/smb/run-client.mts'), smb_ssh_proxy_test, '--proxy', ssh_proxy_test_server], timeout: 180)

webdav_client_test = executable(
  'webdav-client-test',
  ['tests/cpp/webdav-client-test.cpp', 'src/file-transfer/file-transfer-engine.cpp', 'src/webdav/webdav-client.cpp', 'src/webdav/webdav-stream.cpp',
   'src/http/curl-http-session.cpp', 'src/http/curl-http-stream.cpp', 'src/tls/certificate-policy.cpp', 'src/webdav/webdav-metadata.cpp', 'src/webdav/webdav-url.cpp'],
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
    '../shared/src/connection-runtime/compact-header.cpp',
    'tests/cpp/file-transfer-window-lifetime-test.cpp',
    '../shared/src/connection-runtime/titlebar-effect-state.cpp',
    '../shared/src/connection-runtime/titlebar-effects.cpp',
    '../shared/src/connection-runtime/inline-prompt.cpp',
    '../shared/src/connection-runtime/widget-background.cpp',
    'src/sftp/sftp-fixture-client.cpp',
    'src/file-transfer/file-hash.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
    'src/file-transfer/file-transfer-paths.cpp',
    'src/file-transfer/file-transfer-window.cpp',
    'src/file-transfer/file-transfer-job.cpp',
    'src/file-transfer/file-transfer-job-view.cpp',
    'src/file-transfer/activity-file-client.cpp',
    '../shared/src/connection-runtime/activity-indicator.cpp',
    '../shared/src/connection-runtime/indicator-color.cpp',
    'src/file-transfer/local-file-reveal.cpp',
  ],
  include_directories: include_directories('src/file-transfer'),
  cpp_args: ['-I' + gestament_include_dir],
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, gtk_dep],
)

xvfb_run = find_program('xvfb-run')
test('file-transfer-window-lifetime', xvfb_run,
  args: ['-a', file_transfer_window_lifetime_test], timeout: 120)

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

sftp_transfer_engine_test = executable(
  'sftp-transfer-engine-test',
  [
    'tests/cpp/sftp-transfer-engine-test.cpp',
    'src/file-transfer/activity-file-client.cpp',
    'src/file-transfer/file-transfer-job.cpp',
    'src/file-transfer/file-transfer-engine.cpp',
  ],
  dependencies: [
    cardio_dep,
    gio_dep,
  ],
)

test('sftp-transfer-engine-test', sftp_transfer_engine_test)

webdav_tls_session_test = executable(
  'webdav-tls-session-test',
  ['tests/cpp/webdav-tls-session-test.cpp', 'src/http/curl-http-session.cpp', 'src/http/curl-http-stream.cpp',
   'src/tls/certificate-policy.cpp'],
  include_directories: include_directories('src/webdav'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, openssl_dep],
)
test('webdav-tls-session-test', find_program('node'),
  args: [files('tests/webdav-tls-session-runner.mjs'), webdav_tls_session_test], timeout: 120)
test('webdav-proxy-tls-session-test', find_program('node'),
  args: [files('tests/webdav-tls-session-runner.mjs'), webdav_tls_session_test, ssh_proxy_test_server], timeout: 120)

webdav_apache_test = executable(
  'webdav-apache-test',
  ['tests/cpp/webdav-apache-test.cpp', 'src/file-transfer/file-transfer-engine.cpp', 'src/webdav/webdav-client.cpp', 'src/webdav/webdav-stream.cpp',
   'src/http/curl-http-session.cpp', 'src/http/curl-http-stream.cpp', 'src/tls/certificate-policy.cpp', 'src/webdav/webdav-metadata.cpp', 'src/webdav/webdav-url.cpp'],
  include_directories: include_directories('src/webdav'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, libxml2_dep, openssl_dep],
)
test('webdav-apache-test', find_program('node'),
  args: [files('tests/webdav-apache-runner.mjs'), webdav_apache_test], timeout: 900, is_parallel: false)

http_integration_test = executable(
  'http-integration-test',
  ['tests/cpp/http-integration-test.cpp', 'src/http/http-client.cpp', 'src/http/http-crawler.cpp',
   'src/file-transfer/file-transfer-engine.cpp', 'src/http/curl-http-session.cpp', 'src/http/curl-http-stream.cpp', 'src/tls/certificate-policy.cpp'],
  include_directories: include_directories('src/http'),
  dependencies: [cardio_dep, elder_terms_dep, gio_dep, libcurl_dep, ssh_transport_dep, libxml2_dep, openssl_dep],
)
test('http-integration-test', find_program('node'),
  args: [files('tests/http-integration-runner.mjs'), http_integration_test, ssh_proxy_test_server], timeout: 120)
endif

install_symlink(
  'elder-terms-file-transfer',
  pointing_to: '..' / elder_terms_private_install_dir / 'elder-terms-file-transfer/elder-terms-file-transfer',
  install_dir: get_option('bindir'),
)

foreach icon : ['green-on.png', 'green-off.png']
  configure_file(
    input: '../shared/indicators/' + icon,
    output: icon,
    copy: true,
    install: true,
    install_dir: elder_terms_private_install_dir / 'elder-terms-file-transfer',
  )
endforeach
