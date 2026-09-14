# FTPS validation

This document describes the integration checks for the libcurl FTP backend.
Validation results are recorded after each complete run in
[plan-ftps.md](../plan-ftps.md).

## Development tree

Install the project's build and test dependencies and run all workspaces:

```sh
npm ci
ELDER_TERMS_VTE_TEST_FILE_PARALLELISM=true npm run test --workspace=elder-terms-shared --workspace=elder-terms --workspace=elder-terms-vte -- --fileParallelism --maxWorkers=4
```

Workspace builds (including `make build` and the build preceding tests) prepare
Noto Sans Mono, DejaVu Sans Mono, IPAGothic, and Noto Sans CJK JP automatically.
Fontconfig must match the requested family exactly; a substitute does not count.
Missing fonts are downloaded from pinned [Debian packages](https://deb.debian.org/debian/pool/main/f/)
and the official [Noto CJK Sans 2.004 release](https://github.com/notofonts/noto-cjk/tree/523d033d6cb47f4a80c58a35753646f5c3608a78),
verified with SHA-256, and saved with their licenses under `.build/test-fonts/`.
Installed fonts and valid cached downloads need no network access. A damaged cache
is downloaded again; download or verification failure stops the build.
The preparation script requires Node.js with TypeScript support, Fontconfig tools,
and `dpkg-deb` for extracting Debian font packages.

The test runners automatically use the private Fontconfig configuration and cache;
no manual `FONTCONFIG_FILE` setting or host font installation is needed. If supplied,
an existing `FONTCONFIG_FILE` is included as the base configuration. The fallback
test isolates the four families, including the Japanese face of a system TTC,
and compares rendered glyphs before and after changing fallback order.

The UI suite also needs a Japanese UTF-8 locale. Match the reference capture environment with
Yaru icons, the SVG image loader, grayscale antialiasing, and `TZ=Asia/Tokyo`
for the file-list timestamps captured in the existing fixtures. Ensure that
`org.gnome.desktop.interface icon-theme` selects `Yaru`: mounting its icon files
alone leaves Debian's `Adwaita` default active. A disposable image can provide
a [GSettings default override](https://docs.gtk.org/gio/class.Settings.html#vendor-overrides)
for this setting without modifying the host desktop or product. With older Cairo versions, explicitly set
`rgba` to `none` in a `match target="font"` rule after including the system
configuration. GTK's Xft setting alone did not produce grayscale captures in
Debian 12. See the [Fontconfig configuration reference](https://fontconfig.pages.freedesktop.org/fontconfig/fontconfig-user.html).
Without an XSettings manager, GTK 3 on X11 reads
`/etc/gtk-3.0/settings.ini`. Set `gtk-icon-theme-name=Yaru` in its
`[Settings]` section as well: a direct GtkSettings probe on Debian 12 showed
Adwaita despite the GSettings override.
[GTK settings documentation](https://docs.gtk.org/gtk3/class.Settings.html)

The local file reveal checks need Nautilus for an actual file-selection check.
They run in isolated D-Bus and Xvfb sessions with temporary desktop associations.
The service fixture exercises [OpenDirectory and its request response](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.OpenURI.html#org-freedesktop-portal-openuri-opendirectory),
FileManager1 `ShowItems`, and directory association fallback. It also checks
symbolic links, unreadable files, user cancellation, and window closure while
waiting for a response. Successful or cancelled portal interactions must not
launch a second file manager. The Nautilus check reads the actual file-grid
selection through [AT-SPI](https://gnome.pages.gitlab.gnome.org/at-spi2-core/libatspi/method.Accessible.get_state_set.html),
isolated by the file manager's process ID. It checks a Japanese filename with
spaces and `#` and verifies that the other file remains unselected.

The installed-package visual runs also use a private copy of the reference
FreeType rasterizer, Ubuntu 24.04's `libfreetype6 2.13.2+dfsg-1ubuntu0.1`,
for the target architecture. Noto Sans with medium/full hinting produced small
coverage differences with the native Debian rasterizers; `pango-view` with the
same font, DPI, and hinting became pixel-identical with this private library.
Only `libfreetype.so.6` is placed in a separate `LD_LIBRARY_PATH` directory;
GTK, Cairo, application libraries, libcurl and OpenSSL remain those of the
target distribution. The package and C++ validation first run with native
distribution libraries. The library is a validation dependency and is never
added to a delivered package. Record both normal and visual-run `ldd` output.
Use the exact `LD_LIBRARY_PATH` supplied to the test runner, including the i386
directory for a 32-bit application. The final runs retain this additional
record as `visual-runtime-links.txt` beside each environment's validation log.
See the [Ubuntu FreeType package](https://packages.ubuntu.com/noble-updates/libfreetype6)
and the [Pango viewer options](https://manpages.ubuntu.com/manpages/noble/man1/pango-view.1.html).

Do not overlap builds and tests in the same build directory. When invoking the workspace runners directly, finish `npm run build --workspaces` and the complete
`meson test -C .build --num-processes 4` first. Run the shared UI, launcher, and VTE commands below
sequentially, so the timing-sensitive protocol tests have a separate execution
period. Collect every exit status and wait for all suites before
editing or rebuilding:

```sh
npm exec --no --workspace=elder-terms-shared -- vitest run --fileParallelism --maxWorkers=4
npm exec --no --workspace=elder-terms -- vitest run --fileParallelism --maxWorkers=4
npm exec --no --workspace=elder-terms-vte -- vitest run --fileParallelism --maxWorkers=4
```

These are the same complete workspace suites as the root test command, without
file or test-name filters. Keep the configured test budgets and protocol
deadlines. After interrupted diagnostic runs, check for abandoned Xvfb servers
before starting another suite. Validation found 138 servers from completed runs;
their original project working directories, start times, adopted parent and
absence of X clients were verified before terminating those specific processes.
Do not terminate a display used by an active test or desktop session.
An earlier 60-second default-budget experiment still encountered an explicit
60-second visual-case timeout; it was withdrawn when the abandoned processes
were discovered. The [npm workspace execution option](https://docs.npmjs.com/cli/v11/commands/npm-exec/)
sets each command's working directory to its workspace.

The C++ suite exercises both FTPS modes, active/passive and IPv4/IPv6,
TLS 1.0 through 1.3, version bounds, cipher and AUTH selection, certificate
verification, session reuse, stream completion/cancellation, and client lifetime.
Certificate policy tests use real TLS handshakes over memory BIO pairs to check
hostname matching and the scope of explicit exceptions. GUI tests cover actual
file operations, certificate decisions, settings persistence, and launcher
integration. Certificate confirmation and cancellation videos are checked against
captured frames; `test-results/` contains the evidence. The terminal background
fixture materializes its first row before clearing the screen, so VTE 0.70 and
0.76 retain the same history position instead of relying on different empty-ring
initial states. See the [VTE screen-clear implementation](https://github.com/GNOME/vte/blob/0.70.6/src/vteseq.cc#L248).

## Distribution builds

`prereq.sh` creates the existing Debian/Ubuntu packaging images. Use its
`--distro`, `--release`, and `--arch` filters to prepare particular targets, or
omit filters for the complete matrix in `build_package.sh`. Additional C++ test
requirements are Xvfb, xauth, Fontconfig, and the fixture fonts.

Inside a separate source copy for each architecture, run the complete Meson
suite before building and installing the package:

```sh
meson setup .build . --buildtype=debug
meson compile -C .build -j 4
meson test -C .build --num-processes 4 --timeout-multiplier 10 --print-errorlogs
```

The timeout multiplier accommodates emulated architectures without changing the
protocol deadlines under test or selecting a subset of tests. It is an existing
[Meson test option](https://mesonbuild.com/Commands.html#test).
Generate release packages through `build_package.sh`, which checks their files,
architecture, dependencies, and installation. Record `dpkg --print-architecture`,
`pkg-config --modversion libcurl openssl gtk+-3.0 vte-2.91`, the installed
file-transfer executable's ELF class and `ldd` output, and the package's `Depends`
field. OpenSSL selected through pkg-config must match libcurl's TLS backend.

For the full installed-package UI checks, add the normal GUI test dependencies
(Xvfb, D-Bus, FFmpeg, SSH server, pulseaudio, lrzsz, socat, fonts and locale) and
vsftpd to a disposable test image, then run all workspaces with:

```sh
export ELDER_TERMS_TEST_FTP_APP=/usr/lib/elder-terms/elder-terms-vte/elder-terms-file-transfer
export ELDER_TERMS_VTE_TEST_MAX_CONCURRENCY=4
export ELDER_TERMS_VTE_TEST_FILE_PARALLELISM=true
# As an ordinary user, run the three complete workspace commands above.
# The privileged server check is below.
```

Distribution validation limits concurrent VTE scenarios to four through the
existing test configuration. This preserves every protocol, start delay, payload
size, and assertion while bounding the CPU load of multiple container runs.

Run the existing optional i386 package regression from the host with
`ELDER_TERMS_TEST_DEBIAN_TRIXIE_I386=1` on the same complete workspace command.
It requires Podman and the Debian trixie i686 prerequisite image.

The i386 GUI validation uses a 64-bit Node.js automation driver and its GTK/AT-SPI
runtime libraries in a Debian i386 container. The tested application, compiler,
GTK, libcurl, and OpenSSL remain i386. This accommodates the absence of a Linux
ia32 binary in the installed Rolldown dependency; see
[Rolldown's supported platforms](https://rolldown.rs/guide/getting-started).
The driver uses [Node.js 24.11.1](https://nodejs.org/dist/v24.11.1/); the x64 tarball
SHA-256 is `60e3b0a8500819514aca603487c254298cd776de0698d3cd08f11dba5b8289a8`.
Verify the application ELF class and package library directories separately from
the automation driver's architecture.

## Independent server and system trust

`elder-terms-vte/tests/ftps-vsftpd-runner.mjs` is registered with Meson. It exits
with the standard optional-test skip status unless `ELDER_TERMS_TEST_VSFTPD=1`.
When enabled it requires container root and Podman's container marker. Use a
disposable container without a host CA-store mount. Run the complete C++ suite
as container root with `ELDER_TERMS_TEST_VSFTPD=1 meson test -C .build`, and
preserve its logs. Run the complete GUI/workspace suite as an ordinary container
user with this optional flag unset. Existing GUI tests require permission-denial
semantics and an SSH server configured with `PermitRootLogin no`; running them
as root would change what they verify. The ordinary user's account must permit
public-key SSH login. The container harness uses `useradd -m`, a non-login
password marker (`usermod -p '*'`), and `runuser`; it does not alter host users.
OpenSSH's [Linux account-lock setting](https://github.com/openssh/openssh-portable/blob/V_9_2_P1/configure.ac#L844)
and [account check](https://github.com/openssh/openssh-portable/blob/V_9_2_P1/platform.c#L189)
explain the password marker used by this test environment.

The runner creates its own certificate, anonymous chroot, and server config.
It requires encrypted login and data, and `require_ssl_reuse=YES`. It performs
binary upload/download, navigation, rename and deletion in explicit/implicit
mode with TLS 1.2/1.3 and active/passive data connections. Each combination checks
private-CA success, untrusted rejection, explicit certificate approval, and
system-CA success. The server log must confirm data TLS session reuse.

The system-CA case temporarily adds the test CA under
`/usr/local/share/ca-certificates` in that container and runs
`update-ca-certificates`; it removes the entry afterward. Meson runs this test
alone so other tests do not observe its temporary trust entries. The host trust
store is never used as a writable test fixture.

The settings follow the [vsftpd configuration reference](https://security.appspot.com/vsftpd/vsftpd_conf.html)
and the installed package's manual. The runner prints the server, libcurl and
OpenSSL versions. Unsupported TLS negotiation remains an error; neither this
runner nor the application silently reduces TLS requirements.

The visual fixtures use a fixed font and icon collection. For installed-package
GUI runs, mount the same fonts, Fontconfig configuration, and Yaru/Adwaita icons as the
reference environment read-only; the application and GTK libraries remain those
of the target distribution. The isolated GUI environment also needs AT-SPI,
desktop-file-utils, xdotool, inetutils-telnetd, lrzsz, and an OpenSSH SFTP server.
The desktop validator must understand the application's Desktop Entry Version
1.5; Debian 12's version 0.26 rejects that field, while 0.27 validates the same
installed file. See the [Desktop Entry specification](https://specifications.freedesktop.org/desktop-entry-spec/latest/recognized-keys.html).
Nested network tests use rootless Podman with a cached Ubuntu image, subordinate
UID/GID ranges, slirp4netns, the TUN device, and a namespace-capable outer
container. The host's rootless user namespace continues to isolate this setup. The network
fixture uses a late resolved.conf.d drop-in and explicit per-link mDNS/LLMNR
settings, because distribution defaults can override the main configuration
or disable a link independently. See systemd's
[configuration ordering](https://github.com/systemd/systemd/blob/main/man/standard-conf.xml)
and [per-link controls](https://github.com/systemd/systemd/blob/v252/man/resolvectl.xml).

## Backend and emulator limits

Active FTPS requires libcurl 8.0.0 or later. libcurl 7.88.1 still supports both
FTPS modes over passive data connections; active FTPS is rejected before login.
The complete suite checks that rejection in place of unsupported active
transfers, including the independent-server scenarios. See curl's
[active FTPS issue](https://github.com/curl/curl/issues/10666) and
[8.0.0 change log](https://curl.se/ch/8.0.0.html).

Legacy compatibility never permits unencrypted data. TLS 1.2 cipher lists exclude
anonymous and NULL encryption, and TLS 1.3 lists reject the integrity-only
`TLS_SHA256_SHA256` and `TLS_SHA384_SHA384` suites introduced by OpenSSL 3.5.
The [OpenSSL cipher API](https://docs.openssl.org/3.5/man3/SSL_CTX_set_cipher_list/)
describes the TLS 1.3 list and its supported suite names.

QEMU user emulation 8.2.2 lacks the io_uring system calls required by the existing
asynchronous runtime; see its
[system-call implementation](https://github.com/qemu/qemu/blob/v8.2.2/linux-user/syscall.c).
Build foreign-architecture binaries using their distribution compiler, then run
the complete C++ suite under QEMU system emulation with a Linux guest kernel.
The application keeps its original asynchronous I/O implementation. Native
containers need a seccomp profile that permits `io_uring_setup`,
`io_uring_enter`, and `io_uring_register`.

The Ubuntu 26.04 ARM64 archive utility also reaches an unsupported `openat2`
system call under this host's user emulator (observed with `QEMU_STRACE=1`).
The validation build can use Ubuntu 24.04's ARM64 GNU tar binary in the disposable
container while retaining Ubuntu 26.04's compiler and application libraries.
Guest tests run with the target distribution's original archive utility and a
Linux kernel. GNU's [openat2 integration](https://lists.gnu.org/r/bug-gnulib/2025-10/msg00108.html)
describes the archive traversal operation; this workaround affects only the
validation environment, not the delivered package.

The GCC 14 armhf compiler in the validation image crashes while emitting debug
information for vendored libxyzm. The armhf matrix therefore uses the existing
release configuration for the full C++ suite as well as the delivered package;
the vendor sources are unchanged. The observed failure is in
[`xyzm_ymodem.cpp`](https://github.com/kekyo/libxyzm/blob/caefbaf6727156fc2cc9fbc720a4e553848c7e3c/src/async/xyzm_ymodem.cpp#L1046):

```text
internal compiler error: Segmentation fault
c_common_finalize_early_debug()
symbol_table::finalize_compilation_unit()
```

This was observed under user emulation; it is not a claim that the same compiler
fails on native armhf hardware. GCC 12 coroutine ownership regressions are
handled in application call sites by constructing owning arguments and promises
before awaiting them. See the compiler's
[owning-lambda regression case](https://github.com/gcc-mirror/gcc/blob/releases/gcc-13.1.0/gcc/testsuite/g%2B%2B.dg/coroutines/pr101367.C).

Active FTP and FTPS use a new transport for each operation. In the supported libcurl implementations, reusing a control
connection can skip TYPE and complete a download while accepting the data
connection or its TLS handshake is still pending. This was reproduced in 16 fixture cases with controlled command and
handshake ordering; see the [download state machine](https://github.com/curl/curl/blob/curl-8_14_1/lib/ftp.c#L3647)
and the [corresponding upstream upload report](https://github.com/curl/curl/issues/17394).
The worker copies the result and entry path before closing the transport with
[curl_easy_cleanup](https://curl.se/libcurl/c/curl_easy_cleanup.html). It closes
the old connection before opening the next one, so a server that handles one
control connection at a time can continue. This adds connection/authentication
overhead. Certificate exceptions still belong
to the logical application session. With libcurl before 8.9.0, Passive also uses
fresh control connections whenever the configured range allows TLS 1.3.
This uses [CURLOPT_FRESH_CONNECT](https://curl.se/libcurl/c/CURLOPT_FRESH_CONNECT.html).
A full Debian 12 run lost data-session reuse after an upload. A standalone
libcurl program against vsftpd reproduced failure on the following download,
while 50 alternating uploads/downloads succeeded with fresh connections.
This avoids carrying unusable TLS 1.3 session state into the next transfer.
Passive reuses connections on newer libcurl or with an explicit maximum of
TLS 1.2 or below. See the [resumption report](https://github.com/curl/curl/issues/4654),
[older shutdown limitations](https://github.com/curl/curl/blob/curl-8_8_0/docs/KNOWN_BUGS#L417),
and [8.9.0 FTP shutdown changes](https://github.com/curl/curl/pull/13904).


## External receiver used by the i386 regression suite

The trixie i386 full GUI validation uses an isolated lrzsz receiver built from
the unchanged `lrzsz_0.12.21rc.orig.tar.gz` source with
`CFLAGS='-O2 -std=gnu89 -ftrivial-auto-var-init=zero'` and
`./configure --disable-nls`. The distribution receiver can assign uninitialized
modification-time and mode values when a valid YMODEM header omits optional
attributes. The original full suite reported EACCES reading a received file;
an independent protocol probe also observed arbitrary historical modification
times when none were sent in ten completed file-attribute probes. The short
probe did not reproduce EACCES itself; attributing that failure to the mode
value is an inference from the receiver source and original full-suite error.
Automatic variable initialization prevents these
undefined attributes in the test peer. The application, vendor source, and
delivered package are unchanged. Native installed-package C++ results with
the original receiver are retained separately. Record source/binary hashes,
compiler options, and the original failure beside the full suite results.
See [lrzsz source](https://github.com/UweOhse/lrzsz/blob/master/src/lrz.c),
[Debian's source archive](https://deb.debian.org/debian/pool/main/l/lrzsz/lrzsz_0.12.21rc.orig.tar.gz),
and [GCC automatic variable initialization](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html#index-ftrivial-auto-var-init).

## Progress notice recording

The ZMODEM send notice is recorded before the peer starts transferring. The test
selects a frame whose progress bar has reached 45% but not 55%, by checking a filled prefix and an empty suffix in that same frame, after
the peer starts. This excludes the initial indeterminate pulse. It compares the recorded frame with the unchanged
notice fixture and original thresholds, then verifies the received content and
completion state. This replaces a separate AT-SPI progress read followed by a
later screenshot: a full host run observed 45% at the read and about 97% in the
capture 1.5 seconds later. No transfer rate or protocol deadline was changed.
The lossless recording and selected frame remain in test evidence. See the
[FFmpeg X11 recorder](https://ffmpeg.org/ffmpeg-devices.html#x11grab) and
[rawvideo format](https://ffmpeg.org/ffmpeg-formats.html#rawvideo).

## Older libcurl response handling

Before libcurl 8.5.0, evicting a cached FTP connection can overwrite the current
operation's response code with the older connection's QUIT reply. The independent
libcurl/vsftpd probe observed a successful 226 followed by 221 with a one-entry
cache; the complete Debian 12 suite rejected the resulting directory listings.
The application preserves the response immediately before a QUIT sent by its
own handle and suppresses that shutdown response from operation callbacks.
A subsequent command clears this saved value. Only FTPS on affected libcurl versions installs this callback; it neither retains nor logs credentials,
protocol payloads, or diagnostic text. The regular transfer result and failure
checks still apply. See the upstream
[8.5.0 cache eviction fix](https://curl.se/ch/8.5.0.html) and the public
[debug callback](https://curl.se/libcurl/c/CURLOPT_DEBUGFUNCTION.html).

## Final source consistency

The foreign builds first produced clean release packages. After the response-code
and active-connection fixes, the unchanged release and test configurations were rebuilt incrementally,
restaged, packaged and installed again. Source hashes confirmed that the only
changes from those clean builds were the FTP session source and the already
validated progress-recording test. The full C++ suites use the updated binaries;
source hashes are checked again for every target before recording final results.

## Isolated Debian 12 recheck

One parallel installed-package run failed the existing runtime terminal-grid
save case: it observed 80 by 24 cells instead of the requested 81 by 25 after
the settings dialog closed. All 26 FTP/FTPS window cases passed in that run.
The terminal settings application and layout sources were unchanged from the
pre-FTPS baseline. The cause of this single failure remains undetermined.

After both parallel environments finished, Debian 12's complete ordinary-user
suite was rerun alone with the same source, package, assertions, timeouts,
libraries, and visual settings. All tests passed, including the runtime-grid
save case and all 381 VTE cases. Package SHA-256 was checked before and after
the rerun. The already successful native C++ and vsftpd records for that same
package were retained; the complete build, C++, shared UI, launcher, and VTE
stages ran again in the fresh ordinary-user environment. The initial failure
logs are retained separately as `full-suite-before-grid-recheck`. This result
does not establish a fix for the initial runtime-grid failure.

## Recorded validation results

These results come from complete suites, without test-file or test-name filters.
The optional host i386 package regression was enabled.

| Environment | Shared UI | Launcher | VTE UI |
| --- | --- | --- | --- |
| Development host | 113 | 93 | 381 |
| Installed package: bookworm-x86_64 | 113 | 92 + 1 optional skip | 381 |
| Installed package: trixie-i686 | 113 | 92 + 1 optional skip | 381 |

bookworm-x86_64: 125 C++ tests, 212 FTPS fixture cases, and 32 vsftpd 3.0.3-13+b2 cases.

trixie-i686: 125 C++ tests, 212 FTPS fixture cases, and 32 vsftpd 3.0.5-0.2 cases.

| Package target | libcurl | OpenSSL | C++ passed | FTPS cases | CA dependency |
| --- | --- | --- | --- | --- | --- |
| debian-bookworm-arm64 | 7.88.1 | 3.0.20 | 124 | 212 | present |
| debian-bookworm-armv7l | 7.88.1 | 3.0.20 | 124 | 212 | present |
| debian-bookworm-i686 | 7.88.1 | 3.0.20 | 124 | 212 | present |
| debian-bookworm-x86_64 | 7.88.1 | 3.0.20 | 124 | 212 | present |
| debian-trixie-arm64 | 8.14.1 | 3.5.7 | 124 | 212 | present |
| debian-trixie-armv7l | 8.14.1 | 3.5.7 | 124 | 212 | present |
| debian-trixie-i686 | 8.14.1 | 3.5.7 | 124 | 212 | present |
| debian-trixie-riscv64 | 8.14.1 | 3.5.7 | 124 | 212 | present |
| debian-trixie-x86_64 | 8.14.1 | 3.5.7 | 124 | 212 | present |
| ubuntu-24.04-arm64 | 8.5.0 | 3.0.13 | 124 | 212 | present |
| ubuntu-24.04-x86_64 | 8.5.0 | 3.0.13 | 124 | 212 | present |
| ubuntu-26.04-arm64 | 8.18.0 | 3.5.5 | 124 | 212 | present |
| ubuntu-26.04-x86_64 | 8.18.0 | 3.5.5 | 124 | 212 | present |

Each matrix target built and installed its release package and passed package
validation. The optional vsftpd test is skipped in the minimal architecture
images and explicitly enabled in both full installed-package environments above.

Validation completed on 2026-09-12 (Asia/Tokyo). The development host and both
installed environments passed their complete build, C++, shared UI, launcher,
and VTE stages. Their ordinary-user C++ runs passed 124 tests and skipped only
the optional privileged vsftpd runner. ARM and RISC-V matrix results were
executed under QEMU system emulation; x86_64 and i686 results used native
containers.

All 42 changed non-documentation files matched the final development tree in
each of the 13 matrix source copies. Both README files were extracted from
all 15 validated packages and matched the final source documents (30 checks).
Guest run timestamps followed their final successful rebuilds. The FTP session
source SHA-256 was
`1a4055105ebeea9d12890dd62c0dd2570535d015852c3ed34a7032034ffdca92`.

The complete logs, initial failure records, source manifest, and package
documentation audit are retained under `/tmp/elder-ftps-work/`. Final records
are in `stage6-complete-host/`, `stage6-complete-installed/`, and
`stage6-final-matrix/`; the consistency records are
`resumption-source-manifest.json` and `active-package-documentation.json`.
The Debian 12 UI result includes the isolated recheck described above; it is
not evidence that the initial terminal-grid issue has been fixed.
