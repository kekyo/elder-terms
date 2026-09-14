# WebDAV validation

The final distribution and installed-package checks completed by 2026-09-12T21:34:40.131Z (UTC). All 13 build/package/install targets and their complete C++ suites passed. All three UI workspaces passed on the development host and the two installed-package environments below. This record covers standard WebDAV operations; Nextcloud, IIS and NAS products were not tested. See [Using WebDAV](webdav.md) for connection settings and supported operations.

## Distribution results

| Target | Execution | libcurl | OpenSSL | libxml2 | Apache | C++ |
| --- | --- | --- | --- | --- | --- | --- |
| debian-bookworm-x86_64 | Native container | 7.88.1 | 3.0.20 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-bookworm-i686 | Native container | 7.88.1 | 3.0.20 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-bookworm-arm64 | QEMU system guest | 7.88.1 | 3.0.20 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-bookworm-armv7l | QEMU system guest | 7.88.1 | 3.0.20 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-trixie-x86_64 | Native container | 8.14.1 | 3.5.7 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-trixie-i686 | Native container | 8.14.1 | 3.5.7 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-trixie-arm64 | QEMU system guest | 8.14.1 | 3.5.7 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-trixie-armv7l | QEMU system guest | 8.14.1 | 3.5.7 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| debian-trixie-riscv64 | QEMU system guest | 8.14.1 | 3.5.7 | 2.9.14 | 2.4.68 (Debian) | 128 passed / 1 skipped |
| ubuntu-24.04-x86_64 | Native container | 8.5.0 | 3.0.13 | 2.9.14 | 2.4.58 (Ubuntu) | 128 passed / 1 skipped |
| ubuntu-24.04-arm64 | QEMU system guest | 8.5.0 | 3.0.13 | 2.9.14 | 2.4.58 (Ubuntu) | 128 passed / 1 skipped |
| ubuntu-26.04-x86_64 | Native container | 8.18.0 | 3.5.5 | 2.15.2 | 2.4.66 (Ubuntu) | 128 passed / 1 skipped |
| ubuntu-26.04-arm64 | QEMU system guest | 8.18.0 | 3.5.5 | 2.15.2 | 2.4.66 (Ubuntu) | 128 passed / 1 skipped |

The optional vsftpd check is skipped in these minimal environments. Apache WebDAV is required and passed all 39 cases in every row. Foreign applications execute their complete C++ suite in real QEMU system guests with a Linux kernel and loopback networking. The packages use the target distribution's libraries.

## Installed application and UI results

| Environment | Shared UI | Launcher | File transfer / terminal UI |
| --- | --- | --- | --- |
| Development host | 115 passed (115) | 92 passed \| 1 skipped (93) | 402 passed (402) |
| bookworm-x86_64 | 115 passed (115) | 92 passed \| 1 skipped (93) | 402 passed (402) |
| trixie-i686 | 115 passed (115) | 92 passed \| 1 skipped (93) | 402 passed (402) |

The WebDAV and live FTP/FTPS window cases launch the installed file-transfer executable. The complete workspace regression suites also exercise their compiled launcher, terminal and settings fixtures in the same distribution environment. Both installed-package environments also pass all 129 C++ tests as container root, including independent Apache and vsftpd checks. Their ordinary-user whole suites skip these two optional privileged checks. An optional separate i386 packaging test remains skipped unless explicitly enabled. The i386 installed application is a 32-bit ELF executable; its Node.js GUI automation driver is 64-bit.

The same shared settings and file browser cover HTTP/HTTPS, Basic/Digest/automatic/no authentication, list navigation, upload/download, recursive transfers, new folders, rename, deletion, destination conflicts and cancellation. Certificate decisions run through the common FTPS/WebDAV overlay. Tests exercise default cancellation, Escape, Enter, closing the window, changed certificates and failure reasons, distinct sessions, and failure of a mutation without automatic replay. Lossless videos and saved frames verify the confirmation sequence.

## Independent Apache coverage

The 39 cases comprise seven HTTP cases and 32 HTTPS cases. They cover no authentication, Basic, Digest and automatic selection, rejected passwords, TLS 1.2/1.3, custom CA trust, untrusted rejection, explicit approval and the system CA store. Success cases transfer a 40 MiB + 13 byte patterned file, an empty file, names containing Unicode and special characters, and nested empty folders; then verify replacement, conflict preservation, rename and deletion. Server traces verify PROPFIND, GET, PUT, MKCOL, MOVE and DELETE and the negotiated TLS version.

The runner is registered in the complete Meson suite. Enable it only in a disposable validation container or marked guest with Apache, its authentication utilities and certificate tools installed:

```sh
ELDER_TERMS_TEST_APACHE_WEBDAV=1 meson test -C .build --no-rebuild --num-processes 4 --print-errorlogs
```

Temporary system trust changes are made and removed inside that environment. The host CA store is never mounted writable. The independent Node.js fixture additionally checks partial/error responses, malformed or unsafe XML and paths, unknown sizes, compressed responses, bounded streaming, lost final replies, source-size changes and cancellation. Neither a failed HTTP operation nor an uncertain mutation is counted as success.

## Source and package consistency

All 15 validated source copies use one immutable archive and match the final implementation. The audit checks 85 changed non-Markdown files and the absence of removed files, with no per-environment source exceptions. Each package is built fresh; completed compiler outputs from the same target may seed the debug build, but every complete suite is executed again. Source copies stay fixed during each run.

The Ubuntu 26.04 regression exposed retained libcurl pause state after a canceled receive. The corrected transport restores the current operation's send/receive pause state when its connection becomes available, preserving Digest authentication without replaying canceled callbacks. On libcurl 7.88.1, a retained send-pause bit also prevented completion after a final refusal; the transport releases that completed direction once no socket interests remain. Authentication challenges retain their normal negotiation. Complete regression tests cover canceled paused downloads and uploads, early rejection without requesting more input, and subsequent requests on the same session. See the public [libcurl pause API](https://curl.se/libcurl/c/curl_easy_pause.html). Failed whole-suite attempts and the rejected first correction are retained in the validation history.

Earlier installed whole suites reproduced native path-entry focus failures after successful file operations. The test now activates its window before native input, as specified by the public [gestament API](https://github.com/kekyo/gestament/); its focus assertion and real file-operation checks remain enabled. Earlier concurrent validation also exposed the pre-existing TELNET/XMODEM 20-second timing assertion despite matching received bytes, and CPU affinity alone did not eliminate it. The final complete host and installed UI suites therefore run sequentially after all distribution builds and whole guests finish. Builds use CPUs 8–23, guests use CPUs 0–7 with two guest slots, and subsequent UI suites use CPUs 0–7. In the distribution matrix, Ubuntu ARM64 builds use six compiler jobs and other targets use four. Installed-package UI environments build their packages with two jobs; their debug builds and all C++ suites use four workers. The final package-document check verifies that completing the two languages' documentation changes no tested binary, other payload, permissions, symbolic links or package metadata.

## Additional memory diagnostics

A complete GCC 12 AddressSanitizer/UndefinedBehaviorSanitizer comparison exposed a temporary cancellation-state lifetime issue at the shared upload opening boundary. Keeping the selected signal and opening promise in named variables resolves the WebDAV and SFTP transfer diagnostics. The corrected complete diagnostic run reports 126 passed, two failed and one optional skip: the remaining reports are in the unchanged IP scanner and SSH TCP resolver. These existing diagnostics are separate from the successful normal distribution/UI suites; this record does not claim that the whole sanitizer suite passes.

The diagnostic container disables address randomization to avoid sanitizer startup faults; this does not change application builds or system policy on the host. Related compiler temporary-lifetime behavior is described in [GCC's coroutine discussion](https://gcc.gnu.org/pipermail/gcc-bugs/2022-November/805094.html); the observed case is not asserted to be that exact report.

## Validation environment constraints

The installed UI checks reuse the reference fonts, Yaru icons and private reference FreeType solely for reproducible rendering. Application, GTK, libcurl and OpenSSL remain the target versions; package and C++ checks use native libraries first. See [FTPS validation](ftps-validation.md) and the [Ubuntu reference FreeType package](https://packages.ubuntu.com/noble-updates/libfreetype6).

QEMU user emulation does not provide the required io_uring behavior, so foreign runtime tests use system guests; see [QEMU 8.2.2 Linux-user syscall handling](https://github.com/qemu/qemu/blob/v8.2.2/linux-user/syscall.c). The Ubuntu 26.04 arm64 build uses an older tar executable only in the validation environment for the [openat2 compatibility issue](https://lists.gnu.org/r/bug-gnulib/2025-10/msg00108.html). ARMv7 uses the established release-mode, `werror=false` test configuration; warnings remain recorded, including the [GCC 12 string-concatenation warning](https://gcc.gnu.org/pipermail/gcc-bugs/2022-June/790598.html). Release mode also avoids the existing GCC/vendor coroutine debug-information compilation failure described in [FTPS validation](ftps-validation.md); the [vendor source is unchanged](https://github.com/kekyo/libxyzm/blob/caefbaf6727156fc2cc9fbc720a4e553848c7e3c/src/async/xyzm_ymodem.cpp#L1046).

The i386 terminal-transfer test peer uses unchanged [lrzsz source](https://github.com/UweOhse/lrzsz/blob/master/src/lrz.c) with [automatic-variable initialization](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html#index-ftrivial-auto-var-init), as in the existing FTPS validation environment. This test peer is not delivered in the application package. The two C++ WebDAV runners emit their shared TypeScript fixture before importing it on older Node.js versions, using the public [TypeScript compiler API](https://github.com/microsoft/TypeScript/wiki/Using-the-Compiler-API#a-simple-transform-function); all three workspace TypeScript projects also pass a separate full type check.
