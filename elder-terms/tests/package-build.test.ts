// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawnSync, type SpawnSyncReturns } from 'node:child_process';
import {
  chmodSync,
  copyFileSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readlinkSync,
  realpathSync,
  renameSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';

const projectRoot = fileURLToPath(new URL('../..', import.meta.url));
const packageScript = join(projectRoot, 'build_package.sh');
const containerScript = join(
  projectRoot,
  'scripts/build_linux_dist_container.sh'
);
const prerequisiteScript = join(projectRoot, 'prereq.sh');
const allPackagesScript = join(projectRoot, 'build_package_all.sh');
const dpkgDeb = '/usr/bin/dpkg-deb';
const debianTrixieI386PackageTestEnabled =
  process.env.ELDER_TERMS_TEST_DEBIAN_TRIXIE_I386 === '1';

const run = (
  command: string,
  commandArguments: string[],
  environment: NodeJS.ProcessEnv = process.env
): SpawnSyncReturns<string> =>
  spawnSync(command, commandArguments, {
    encoding: 'utf8',
    env: environment,
    maxBuffer: 64 * 1024 * 1024,
  });

const runSourced = (
  body: string,
  extraArguments: string[] = [],
  environment: NodeJS.ProcessEnv = process.env
): SpawnSyncReturns<string> =>
  run(
    'sh',
    [
      '-c',
      `
ELDER_TERMS_PACKAGE_PROJECT_ROOT=$1
ELDER_TERMS_PACKAGE_SOURCE_ONLY=1
. "$1/build_package.sh"
${body}
`,
      'elder-terms-package-test',
      projectRoot,
      ...extraArguments,
    ],
    environment
  );

const writeExecutable = (path: string, contents: string): void => {
  writeFileSync(path, contents);
  chmodSync(path, 0o755);
};

const expectSuccess = (
  result: SpawnSyncReturns<string>,
  operation: string
): void => {
  expect(
    result.status,
    `${operation}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
  ).toBe(0);
};

const createPackageStage = (
  stageRoot: string,
  debianArchitecture: string,
  omittedPath: string | undefined,
  includeOpenSshClient: boolean,
  includeXdgUtils: boolean
): void => {
  const directories = [
    'DEBIAN',
    'etc/xdg/autostart',
    'usr/bin',
    'usr/lib/elder-terms',
    'usr/lib/elder-terms/launcher',
    'usr/lib/elder-terms/elder-terms-terminal',
    'usr/share/applications',
    'usr/share/doc/elder-terms',
    'usr/share/doc/elder-terms/docs/ja',
    'usr/share/doc/elder-terms/docs/en',
    'usr/share/doc/elder-terms/licenses/boxrig',
    'usr/share/doc/elder-terms/licenses/jemalloc',
    'usr/share/doc/elder-terms/licenses/connection-icons',
    'usr/share/icons/hicolor/256x256/apps',
    'usr/share/locale/ja/LC_MESSAGES',
  ];
  for (const directory of directories) {
    mkdirSync(join(stageRoot, directory), { recursive: true });
  }

  writeFileSync(
    join(stageRoot, 'DEBIAN/control'),
    `Package: elder-terms
Version: 1.2.3
Section: x11
Priority: optional
Architecture: ${debianArchitecture}
Maintainer: elder-terms packager <packager@localhost>
Depends: libc6, ca-certificates, dbus-user-session, hicolor-icon-theme${includeOpenSshClient ? ', openssh-client' : ''}${includeXdgUtils ? ', xdg-utils' : ''}
Description: GTK terminal for serial, TELNET, local shell, SSH, SFTP, and FTP connections
`
  );

  const executablePaths = [
    'usr/bin/etctl',
    'usr/lib/elder-terms/libelder-terms.so.0',
    'usr/lib/elder-terms/libcardio.so.0',
    'usr/lib/elder-terms/libboxrig.so.0',
    'usr/lib/elder-terms/libboxrig-ui.so.0',
    'usr/lib/elder-terms/libboxrig-session.so.0',
    'usr/lib/elder-terms/libboxrig-posix.so.0',
    'usr/lib/elder-terms/libboxrig-gtk3.so.0',
    'usr/lib/elder-terms/libjemalloc.so.2',
    'usr/lib/elder-terms/launcher/elder-terms',
    'usr/lib/elder-terms/elder-terms-terminal/elder-terms-terminal',
    'usr/lib/elder-terms/elder-terms-terminal/elder-terms-file-transfer',
  ];
  for (const executablePath of executablePaths) {
    if (executablePath !== omittedPath) {
      copyFileSync('/bin/true', join(stageRoot, executablePath));
    }
  }

  const textFiles = new Map<string, string>([
    [
      'etc/xdg/autostart/net.kekyo.elder-terms.desktop',
      '[Desktop Entry]\nType=Application\nExec=elder-terms\n',
    ],
    [
      'usr/share/applications/net.kekyo.elder-terms.desktop',
      '[Desktop Entry]\nType=Application\nExec=elder-terms\n',
    ],
    [
      'usr/share/applications/net.kekyo.elder-terms-terminal.desktop',
      '[Desktop Entry]\nType=Application\nExec=elder-terms-terminal\n',
    ],
    ['usr/lib/elder-terms/launcher/main-window.ui', '<interface/>\n'],
    [
      'usr/lib/elder-terms/elder-terms-terminal/main-window.ui',
      '<interface/>\n',
    ],
    ['usr/lib/elder-terms/elder-terms-terminal/green-on.png', 'on\n'],
    ['usr/lib/elder-terms/elder-terms-terminal/green-off.png', 'off\n'],
    ['usr/share/doc/elder-terms/README.md', '# elder-terms\n'],
    ['usr/share/doc/elder-terms/README_ja.md', '# elder-terms\n'],
    ['usr/share/doc/elder-terms/docs/ja/webdav.md', '# WebDAV\n'],
    ['usr/share/doc/elder-terms/docs/en/webdav.md', '# WebDAV\n'],
    [
      'usr/share/doc/elder-terms/docs/en/webdav-validation.md',
      '# WebDAV validation\n',
    ],
    ['usr/share/doc/elder-terms/copyright', 'MIT\n'],
    ['usr/share/icons/hicolor/256x256/apps/elder-terms.png', 'icon\n'],
    ['usr/share/locale/ja/LC_MESSAGES/elder-terms.mo', 'locale\n'],
  ]);
  for (const name of [
    'LICENSE',
    'README.md',
    'README_ja.md',
    'sources.json',
    'utf8proc-LICENSE.md',
    'libpng-LICENSE',
    'zlib-LICENSE',
    'Unicode-LICENSE.txt',
    'WHATWG-Encoding-LICENSE',
    'Wayland-protocols-NOTICES.md',
  ]) {
    textFiles.set(
      `usr/share/doc/elder-terms/licenses/boxrig/${name}`,
      'license fixture\n'
    );
  }
  textFiles.set(
    'usr/share/doc/elder-terms/licenses/jemalloc/COPYING',
    'license fixture\n'
  );
  for (const name of [
    'NOTO-LICENSE',
    'NOTO-FLAGS-NOTICE',
    'MATERIAL-LICENSE',
    'UNICODE-LICENSE',
  ]) {
    textFiles.set(
      `usr/share/doc/elder-terms/licenses/connection-icons/${name}`,
      'icon license fixture\n'
    );
  }
  for (const [relativePath, contents] of textFiles) {
    if (relativePath !== omittedPath) {
      writeFileSync(join(stageRoot, relativePath), contents);
    }
  }

  symlinkSync(
    '../lib/elder-terms/launcher/elder-terms',
    join(stageRoot, 'usr/bin/elder-terms')
  );
  symlinkSync(
    '../lib/elder-terms/elder-terms-terminal/elder-terms-terminal',
    join(stageRoot, 'usr/bin/elder-terms-terminal')
  );
  symlinkSync(
    '../lib/elder-terms/elder-terms-terminal/elder-terms-file-transfer',
    join(stageRoot, 'usr/bin/elder-terms-file-transfer')
  );
};

describe('Debian package generation', { concurrent: false }, () => {
  let temporaryRoot: string;

  beforeAll(() => {
    temporaryRoot = mkdtempSync(join(tmpdir(), 'elder-terms-package-test-'));
  });

  afterAll(() => {
    rmSync(temporaryRoot, { recursive: true, force: true });
  });

  it('resolves versions and every supported distribution target', () => {
    const binDirectory = join(temporaryRoot, 'version-bin');
    const invocationPath = join(temporaryRoot, 'version-invocation.txt');
    mkdirSync(binDirectory);
    writeExecutable(
      join(binDirectory, 'npx'),
      `#!/bin/sh
set -eu
[ "\${ELDER_TERMS_TEST_FORBID_VERSION_LOOKUP:-0}" = 0 ] || exit 99
printf '%s\\n' "$@" >"$ELDER_TERMS_TEST_VERSION_INVOCATION"
printf '7.6.5-test\\n'
`
    );

    const detectedVersion = run(packageScript, ['--print-version'], {
      ...process.env,
      PATH: `${binDirectory}:${process.env.PATH}`,
      ELDER_TERMS_TEST_VERSION_INVOCATION: invocationPath,
    });
    expectSuccess(detectedVersion, 'default package version resolution failed');
    expect(detectedVersion.stdout).toBe('7.6.5-test\n');
    expect(readFileSync(invocationPath, 'utf8')).toBe(
      ['screw-up', 'format', '-e', '{version}', '-f', ''].join('\n')
    );

    const explicitVersion = run(
      packageScript,
      ['--version', '2.4.6-custom', '--print-version'],
      {
        ...process.env,
        PATH: `${binDirectory}:${process.env.PATH}`,
        ELDER_TERMS_TEST_FORBID_VERSION_LOOKUP: '1',
      }
    );
    expectSuccess(explicitVersion, 'explicit package version failed');
    expect(explicitVersion.stdout).toBe('2.4.6-custom\n');

    const mappings = runSourced(`
DISTRO_FILTER=''
RELEASE_FILTER=''
ARCH_FILTER=''
VERSION=1.2.3
canonical_arch amd64
canonical_arch i386
canonical_arch armhf
canonical_release noble
canonical_release resolute
count_deb_builds
deb_artifact_path elder-terms ubuntu 24.04 x86_64
prereq_image_for_target debian trixie x86_64
container_image_for_target debian trixie riscv64
`);
    expectSuccess(mappings, 'package target mappings failed');
    expect(mappings.stdout).toBe(
      [
        'x86_64',
        'i686',
        'armv7l',
        '24.04',
        '26.04',
        '9',
        join(
          projectRoot,
          'artifacts/deb/elder-terms-1.2.3-ubuntu-24.04-amd64.deb'
        ),
        'localhost/elder-terms-pack-deb-debian-trixie-x86_64:latest',
        'docker.io/library/debian:trixie',
        '',
      ].join('\n')
    );
  });

  it('reports the boxrig release version from the build SDK', () => {
    const versions = run(
      'pkg-config',
      [
        '--modversion',
        'boxrig',
        'boxrig-ui',
        'boxrig-session',
        'boxrig-posix',
        'boxrig-gtk3',
      ],
      {
        ...process.env,
        PKG_CONFIG_PATH: join(projectRoot, '.build/boxrig/sdk/lib/pkgconfig'),
      }
    );
    expectSuccess(versions, 'build SDK version lookup failed');
    expect(versions.stdout.trim().split(/\s+/)).toEqual([
      '0.6.0',
      '0.6.0',
      '0.6.0',
      '0.6.0',
      '0.6.0',
    ]);
  });

  it('builds a usable SDK from relocated host-prepared tools without target npm', () => {
    const prepared = join(temporaryRoot, 'prepared-tools');
    expectSuccess(
      run(process.execPath, [
        join(projectRoot, 'scripts/prepare-package-tools.mts'),
        prepared,
      ]),
      'host build-tool preparation failed'
    );
    const relocated = join(temporaryRoot, 'relocated-tools');
    renameSync(prepared, relocated);
    expect(lstatSync(join(relocated, 'prepare-jemalloc.mjs')).isFile()).toBe(
      true
    );
    const resourceDriver = join(temporaryRoot, 'material-resource-driver.c');
    const resourceExecutable = join(temporaryRoot, 'material-resource-driver');
    writeFileSync(
      resourceDriver,
      `#include <gdk-pixbuf/gdk-pixbuf.h>

int main(void) {
  const char *paths[] = {
      "/net/kekyo/elder-terms/presets/material/action/home.png",
      "/net/kekyo/elder-terms/presets/material-white/action/home.png"
  };
  for (int variant = 0; variant < 2; ++variant) {
    GError *error = NULL;
    GdkPixbuf *image = gdk_pixbuf_new_from_resource(paths[variant], &error);
    if (image == NULL) {
      if (error != NULL) g_error_free(error);
      return 1;
    }
    const int width = gdk_pixbuf_get_width(image);
    const int height = gdk_pixbuf_get_height(image);
    const int stride = gdk_pixbuf_get_rowstride(image);
    const int channels = gdk_pixbuf_get_n_channels(image);
    if (width != 256 || height != 256 ||
        !gdk_pixbuf_get_has_alpha(image) || channels != 4) {
      g_object_unref(image);
      return 2;
    }
    const guchar *pixels = gdk_pixbuf_read_pixels(image);
    const guchar *center = pixels + 128 * stride + 128 * channels;
    const guchar *padding = pixels + 128 * stride + 32 * channels;
    const guchar *outline = pixels + 190 * stride + 90 * channels;
    const int foreground = variant == 0 ? 0 : 255;
    const int valid = center[0] == foreground && center[1] == foreground &&
        center[2] == foreground && center[3] == 255 && padding[3] == 0 &&
        outline[0] == 255 - foreground && outline[1] == 255 - foreground &&
        outline[2] == 255 - foreground && outline[3] == 255;
    g_object_unref(image);
    if (!valid) return 2;
  }
  return 0;
}
`
    );
    const pixbufFlags = run('pkg-config', [
      '--cflags',
      '--libs',
      'gdk-pixbuf-2.0',
      'gio-2.0',
    ]);
    expectSuccess(pixbufFlags, 'GResource test compiler flags unavailable');
    expectSuccess(
      run('cc', [
        '-std=c99',
        '-Wall',
        '-Wextra',
        '-Werror',
        resourceDriver,
        join(relocated, 'elder-terms-material-presets.c'),
        '-o',
        resourceExecutable,
        ...pixbufFlags.stdout.trim().split(/\s+/u),
      ]),
      'relocated Material icon resource did not compile'
    );
    expectSuccess(
      run(resourceExecutable, []),
      'relocated Material icon resource did not decode into the chosen artwork'
    );
    const commands = join(temporaryRoot, 'target-commands');
    mkdirSync(commands);
    writeExecutable(
      join(commands, 'npm'),
      '#!/bin/sh\necho "Target must use host-prepared tools" >&2\nexit 97\n'
    );
    const build = join(temporaryRoot, 'prepared-sdk-build');
    expectSuccess(
      run(
        process.execPath,
        [
          '--no-experimental-strip-types',
          join(relocated, 'prepare-boxrig.mjs'),
          projectRoot,
          build,
          relocated,
        ],
        { ...process.env, PATH: `${commands}:${process.env.PATH}` }
      ),
      'SDK build with host-prepared tools failed'
    );
    const sdk = join(build, 'boxrig/sdk');
    const flags = run(
      'pkg-config',
      ['--cflags', '--libs', 'boxrig-ui', 'boxrig-posix'],
      {
        ...process.env,
        PKG_CONFIG_PATH: join(sdk, 'lib/pkgconfig'),
      }
    );
    expectSuccess(flags, 'prepared SDK metadata lookup failed');
    const versions = run(
      'pkg-config',
      ['--modversion', 'boxrig-ui', 'boxrig-posix'],
      { ...process.env, PKG_CONFIG_PATH: join(sdk, 'lib/pkgconfig') }
    );
    expectSuccess(versions, 'prepared SDK version lookup failed');
    expect(versions.stdout.trim().split(/\s+/)).toEqual(['0.6.0', '0.6.0']);
    const consumer = join(temporaryRoot, 'prepared-sdk-consumer');
    expectSuccess(
      run('cc', [
        '-std=c99',
        '-Wall',
        '-Wextra',
        '-Werror',
        join(projectRoot, 'deps/boxrig/tests/shared/consumer.c'),
        ...flags.stdout.trim().split(/\s+/),
        '-ldl',
        '-o',
        consumer,
      ]),
      'prepared SDK consumer compilation failed'
    );
    expectSuccess(
      run(consumer, [], { ...process.env, LD_LIBRARY_PATH: join(sdk, 'lib') }),
      'prepared SDK public API execution failed'
    );
  }, 180_000);

  it('configures Meson for a relocatable package installation', () => {
    const binDirectory = join(temporaryRoot, 'container-bin');
    const invocationPath = join(temporaryRoot, 'meson-invocation.txt');
    const pkgConfigInvocationPath = join(
      temporaryRoot,
      'pkg-config-invocation.txt'
    );
    mkdirSync(binDirectory);
    writeExecutable(
      join(binDirectory, 'meson'),
      `#!/bin/sh
printf '%s\\n' "$@" >"$ELDER_TERMS_TEST_MESON_INVOCATION"
exit 91
`
    );
    writeExecutable(
      join(binDirectory, 'pkg-config'),
      `#!/bin/sh
printf '%s\\n' "$*" >>"$ELDER_TERMS_TEST_PKG_CONFIG_INVOCATION"
if [ "\${ELDER_TERMS_TEST_MISSING_CURL:-0}" = 1 ] && [ "$*" = "--exists libcurl" ]; then exit 1; fi
exit 0
`
    );

    const environment = {
      ...process.env,
      PATH: `${binDirectory}:${process.env.PATH}`,
      ELDER_TERMS_BUILD_TYPE: 'release',
      ELDER_TERMS_MAKE_JOBS: '1',
      ELDER_TERMS_PACKAGE_DESCRIPTION: 'elder-terms package test',
      ELDER_TERMS_PACKAGE_MAINTAINER: 'test <test@localhost>',
      ELDER_TERMS_PACKAGE_NAME: 'elder-terms',
      ELDER_TERMS_PACKAGE_VERSION: '1.2.3-test',
      ELDER_TERMS_PACKAGE_TOOLS: join(temporaryRoot, 'container-tools'),
      ELDER_TERMS_TEST_MESON_INVOCATION: invocationPath,
      ELDER_TERMS_TEST_PKG_CONFIG_INVOCATION: pkgConfigInvocationPath,
      ELDER_TERMS_WORK_DIR: join(temporaryRoot, 'container-work'),
    };
    const result = run(containerScript, [], environment);
    expect(result.status).toBe(91);
    const invocation = readFileSync(invocationPath, 'utf8');
    expect(invocation).toContain('--prefix=/usr');
    expect(invocation).toContain('--libdir=lib');
    expect(invocation).toContain('-Dautostartdir=/etc/xdg/autostart');
    expect(invocation).toContain('-Dbuild_tests=false');
    expect(invocation).toContain('-Dwerror=true');
    expect(invocation).toContain('--buildtype=release');
    expect(invocation).toContain('-Dapplication_version=1.2.3-test');
    expect(invocation).toContain(
      `-Dpackage_build_tools=${environment.ELDER_TERMS_PACKAGE_TOOLS}`
    );
    expect(readFileSync(pkgConfigInvocationPath, 'utf8').split('\n')).toContain(
      '--exists libcanberra'
    );
    expect(
      readFileSync(pkgConfigInvocationPath, 'utf8').split('\n')
    ).not.toContain('--exists libpcre2-8');

    rmSync(invocationPath);
    const missingCurl = run(containerScript, [], {
      ...environment,
      ELDER_TERMS_TEST_MISSING_CURL: '1',
    });
    expect(missingCurl.status).toBe(1);
    expect(missingCurl.stderr).toContain(
      'Missing required pkg-config module: libcurl'
    );
    expect(() => lstatSync(invocationPath)).toThrow();
  });

  it('rejects compiler warnings before creating a release package', () => {
    const source = join(temporaryRoot, 'warning-source');
    const work = join(temporaryRoot, 'warning-work');
    mkdirSync(source);
    writeFileSync(
      join(source, 'meson.build'),
      "project('package-warning-check', 'cpp', default_options: ['warning_level=3'])\n" +
        "executable('warning-fixture', 'warning.cpp')\n"
    );
    writeFileSync(
      join(source, 'meson_options.txt'),
      [
        "option('autostartdir', type: 'string', value: '')",
        "option('application_version', type: 'string', value: '')",
        "option('package_build_tools', type: 'string', value: '')",
        "option('build_tests', type: 'boolean', value: false)",
        '',
      ].join('\n')
    );
    writeFileSync(
      join(source, 'warning.cpp'),
      'int main() { int unused; return 0; }\n'
    );

    const result = run(
      'sh',
      [
        '-c',
        'cd "$1" && exec "$2"',
        'package-warning-test',
        source,
        containerScript,
      ],
      {
        ...process.env,
        ELDER_TERMS_BUILD_TYPE: 'release',
        ELDER_TERMS_MAKE_JOBS: '1',
        ELDER_TERMS_PACKAGE_DESCRIPTION: 'elder-terms warning test',
        ELDER_TERMS_PACKAGE_MAINTAINER: 'test <test@localhost>',
        ELDER_TERMS_PACKAGE_NAME: 'elder-terms',
        ELDER_TERMS_PACKAGE_VERSION: '1.2.3-test',
        ELDER_TERMS_PACKAGE_TOOLS: join(temporaryRoot, 'warning-tools'),
        ELDER_TERMS_WORK_DIR: work,
      }
    );
    expect(result.status).not.toBe(0);
    expect(`${result.stdout}\n${result.stderr}`).toContain(
      '-Werror=unused-variable'
    );
    expect(() => lstatSync(join(work, 'build', 'warning-fixture'))).toThrow();
  });

  it('disables libxyzm debug information for release package builds', () => {
    const binDirectory = join(temporaryRoot, 'libxyzm-bin');
    const buildDirectory = join(temporaryRoot, 'libxyzm-release-build');
    const invocationPath = join(temporaryRoot, 'libxyzm-make-invocation.txt');
    mkdirSync(binDirectory);
    writeExecutable(
      join(binDirectory, 'make'),
      `#!/bin/sh
set -eu
# Only intercept the libxyzm invocation; Meson also prepares the boxrig SDK.
case " $* " in
  *" LIBXYZM_EXTRA_CXXFLAGS="*) ;;
  *) exec /usr/bin/make "$@" ;;
esac
printf '%s\n' "$@" >"$ELDER_TERMS_TEST_LIBXYZM_MAKE_INVOCATION"
build_dir=''
for argument in "$@"; do
  case $argument in BUILD_DIR=*) build_dir=\${argument#BUILD_DIR=} ;; esac
done
[ -n "$build_dir" ] || exit 81
mkdir -p "$build_dir"
: >"$build_dir/libxyzm.a"
: >"$build_dir/libxyzm_async.a"
`
    );
    const environment = {
      ...process.env,
      PATH: `${binDirectory}:${process.env.PATH}`,
      ELDER_TERMS_TEST_LIBXYZM_MAKE_INVOCATION: invocationPath,
    };

    const setup = run(
      'meson',
      [
        'setup',
        buildDirectory,
        projectRoot,
        '--buildtype=release',
        '-Dbuild_tests=false',
      ],
      environment
    );
    expectSuccess(setup, 'release Meson setup failed');
    const compile = run(
      'meson',
      [
        'compile',
        '-C',
        buildDirectory,
        'elder-terms-terminal/libxyzm-static-libraries',
      ],
      environment
    );
    expectSuccess(compile, 'release libxyzm build failed');
    expect(readFileSync(invocationPath, 'utf8').split('\n')).toContain(
      'LIBXYZM_EXTRA_CXXFLAGS=-g0'
    );
  });

  it('orchestrates isolated build and installation-validation containers', () => {
    const fakeProject = join(temporaryRoot, 'orchestration-project');
    const binDirectory = join(temporaryRoot, 'orchestration-bin');
    const containerRecords = join(temporaryRoot, 'container-records.txt');
    const dpkgRecords = join(temporaryRoot, 'dpkg-records.txt');
    mkdirSync(fakeProject);
    mkdirSync(binDirectory);

    const containerEngine = join(binDirectory, 'container-engine');
    writeExecutable(
      containerEngine,
      `#!/bin/sh
set -eu
if [ "\${1:-}" = image ] && [ "\${2:-}" = exists ]; then
  printf 'exists %s\\n' "\${3:-}" >>"$ELDER_TERMS_TEST_CONTAINER_RECORDS"
  exit 0
fi
[ "\${1:-}" = run ] || exit 2
printf 'run %s\\n' "$*" >>"$ELDER_TERMS_TEST_CONTAINER_RECORDS"
workspace=''
previous=''
for argument in "$@"; do
  if [ "$previous" = -v ]; then workspace=\${argument%%:/workspace*}; fi
  previous=$argument
done
case " $* " in *" --validate-package "*) exit 0 ;; esac
stage="$workspace/artifacts/.tmp/test-run/deb/debian/trixie/x86_64/work/stage/elder-terms"
mkdir -p "$stage/DEBIAN"
printf 'Package: elder-terms\\n' >"$stage/DEBIAN/control"
`
    );
    writeExecutable(
      join(binDirectory, 'dpkg-deb'),
      `#!/bin/sh
set -eu
for argument in "$@"; do output_path=$argument; done
mkdir -p "$(dirname "$output_path")"
printf 'stub-deb\\n' >"$output_path"
printf '%s\\n' "$*" >>"$ELDER_TERMS_TEST_DPKG_RECORDS"
`
    );

    const result = runSourced(
      `
PROJECT_ROOT=$2
ARTIFACT_ROOT=$PROJECT_ROOT/artifacts
DEB_ARTIFACT_ROOT=$ARTIFACT_ROOT/deb
RUN_ID=test-run
TMP_ROOT=$ARTIFACT_ROOT/.tmp/$RUN_ID
VERSION=1.2.3-test
CONTAINER_ENGINE_BIN=$3
MAKE_JOBS=1
BUILD_TYPE=release
build_deb_package debian trixie x86_64 linux/amd64
`,
      [fakeProject, containerEngine],
      {
        ...process.env,
        PATH: `${binDirectory}:${process.env.PATH}`,
        ELDER_TERMS_TEST_CONTAINER_RECORDS: containerRecords,
        ELDER_TERMS_TEST_DPKG_RECORDS: dpkgRecords,
      }
    );
    expectSuccess(result, 'deb package orchestration failed');
    const records = readFileSync(containerRecords, 'utf8');
    expect(records).toContain(
      'exists localhost/elder-terms-pack-deb-debian-trixie-x86_64:latest'
    );
    expect(
      records.split('\n').filter((line) => line.startsWith('run '))
    ).toHaveLength(2);
    expect(records).toContain('./scripts/build_linux_dist_container.sh');
    expect(records).toContain(
      '--validate-package /workspace/artifacts/deb/elder-terms-1.2.3-test-debian-trixie-amd64.deb'
    );
    expect(readFileSync(dpkgRecords, 'utf8')).toContain(
      join(
        fakeProject,
        'artifacts/deb/elder-terms-1.2.3-test-debian-trixie-amd64.deb'
      )
    );
  });

  it('builds filtered prerequisite images and forwards all-target options', () => {
    const fakeProject = join(temporaryRoot, 'prerequisite-project');
    const binDirectory = join(temporaryRoot, 'prerequisite-bin');
    const recordsPath = join(temporaryRoot, 'prerequisite-records.txt');
    mkdirSync(fakeProject);
    mkdirSync(binDirectory);
    symlinkSync(packageScript, join(fakeProject, 'build_package.sh'));

    const containerEngine = join(binDirectory, 'prerequisite-engine');
    writeExecutable(
      containerEngine,
      `#!/bin/sh
set -eu
[ "\${1:-}" = build ] || exit 2
previous=''
for argument in "$@"; do
  if [ "$previous" = -f ]; then containerfile=$argument; fi
  previous=$argument
done
printf '%s\\n' "$*" >"$ELDER_TERMS_TEST_PREREQUISITE_RECORDS"
cp "$containerfile" "$ELDER_TERMS_TEST_PREREQUISITE_RECORDS.containerfile"
`
    );
    const prerequisite = run(
      prerequisiteScript,
      [
        '--distro',
        'debian',
        '--release',
        'trixie',
        '--arch',
        'amd64',
        '--jobs',
        '1',
        '--force',
      ],
      {
        ...process.env,
        CONTAINER_ENGINE: containerEngine,
        ELDER_TERMS_PREREQ_PROJECT_ROOT: fakeProject,
        ELDER_TERMS_TEST_PREREQUISITE_RECORDS: recordsPath,
      }
    );
    expectSuccess(prerequisite, 'filtered prerequisite image build failed');
    const invocation = readFileSync(recordsPath, 'utf8');
    expect(invocation).toContain('build --no-cache --platform linux/amd64');
    expect(invocation).toContain('BASE_IMAGE=docker.io/amd64/debian:trixie');
    expect(invocation).toContain(
      'localhost/elder-terms-pack-deb-debian-trixie-x86_64:latest'
    );
    const containerfile = readFileSync(`${recordsPath}.containerfile`, 'utf8');
    for (const dependency of [
      'autoconf',
      'libcanberra-dev',
      'libcanberra-pulse',
      'libgtk-3-dev',
      'libcairo2-dev',
      'libpango1.0-dev',
      'libfontconfig1-dev',
      'libepoxy-dev',
      'libibus-1.0-dev',
      'libwayland-dev',
      'libgl-dev',
      'libegl-dev',
      'libssh-dev',
      'libudev-dev',
      'libsystemd-dev',
      'liburing-dev',
      'libxkbcommon-dev',
      'meson',
      'nodejs',
      'openssh-client',
      'xdg-utils',
    ]) {
      expect(containerfile).toContain(dependency);
    }
    expect(prerequisite.stdout).toContain('Prerequisite images are ready.');

    const wrapperCapture = join(temporaryRoot, 'wrapper-arguments.txt');
    const wrapperStub = join(binDirectory, 'wrapper-stub');
    writeExecutable(
      wrapperStub,
      '#!/bin/sh\nset -eu\nprintf \'%s\\n\' "$@" >"$ELDER_TERMS_TEST_WRAPPER_CAPTURE"\n'
    );
    const wrapper = run(
      allPackagesScript,
      ['--jobs', '3', '--arch', 'amd64', '--refresh-base'],
      {
        ...process.env,
        BUILD_PACKAGE_SCRIPT: wrapperStub,
        ELDER_TERMS_TEST_WRAPPER_CAPTURE: wrapperCapture,
      }
    );
    expectSuccess(wrapper, 'all-target wrapper failed');
    expect(readFileSync(wrapperCapture, 'utf8')).toBe(
      ['--target', 'all', '--jobs', '3', '--arch', 'amd64', ''].join('\n')
    );
    expect(wrapper.stderr).toContain(
      'Run ./prereq.sh --force to rebuild prerequisite images.'
    );
  });

  it('accepts a complete deb and rejects one missing runtime data', () => {
    const architecture = run('/usr/bin/dpkg', ['--print-architecture']);
    expectSuccess(architecture, 'host Debian architecture lookup failed');
    const debianArchitecture = architecture.stdout.trim();
    const canonicalArchitecture = new Map([
      ['amd64', 'x86_64'],
      ['i386', 'i686'],
      ['arm64', 'arm64'],
      ['armhf', 'armv7l'],
      ['riscv64', 'riscv64'],
    ]).get(debianArchitecture);
    expect(canonicalArchitecture).toBeDefined();

    const missingCaStage = join(temporaryRoot, 'missing-ca-stage');
    const missingCaPackage = join(temporaryRoot, 'missing-ca.deb');
    const systemJemallocStage = join(temporaryRoot, 'system-jemalloc-stage');
    const systemJemallocPackage = join(temporaryRoot, 'system-jemalloc.deb');
    const goodStage = join(temporaryRoot, 'good-stage');
    const badStage = join(temporaryRoot, 'bad-stage');
    const goodPackage = join(temporaryRoot, 'elder-terms-good.deb');
    const badPackage = join(temporaryRoot, 'elder-terms-bad.deb');
    const missingFileTransferStage = join(
      temporaryRoot,
      'missing-file-transfer-stage'
    );
    const missingFileTransferPackage = join(
      temporaryRoot,
      'elder-terms-missing-file-transfer.deb'
    );
    const missingOpenSshClientStage = join(
      temporaryRoot,
      'missing-openssh-client-stage'
    );
    const missingOpenSshClientPackage = join(
      temporaryRoot,
      'elder-terms-missing-openssh-client.deb'
    );
    const missingXdgUtilsStage = join(temporaryRoot, 'missing-xdg-utils-stage');
    const missingXdgUtilsPackage = join(
      temporaryRoot,
      'elder-terms-missing-xdg-utils.deb'
    );
    createPackageStage(goodStage, debianArchitecture, undefined, true, true);
    createPackageStage(
      missingCaStage,
      debianArchitecture,
      undefined,
      true,
      true
    );
    createPackageStage(
      systemJemallocStage,
      debianArchitecture,
      undefined,
      true,
      true
    );
    const systemJemallocControl = join(systemJemallocStage, 'DEBIAN/control');
    writeFileSync(
      systemJemallocControl,
      readFileSync(systemJemallocControl, 'utf8').replace(
        'Depends: libc6',
        'Depends: libc6, libjemalloc2 (>= 5.4.0)'
      )
    );
    const missingCaControl = join(missingCaStage, 'DEBIAN/control');
    writeFileSync(
      missingCaControl,
      readFileSync(missingCaControl, 'utf8').replace(', ca-certificates', '')
    );
    createPackageStage(
      badStage,
      debianArchitecture,
      'usr/share/applications/net.kekyo.elder-terms-terminal.desktop',
      true,
      true
    );
    createPackageStage(
      missingFileTransferStage,
      debianArchitecture,
      'usr/lib/elder-terms/elder-terms-terminal/elder-terms-file-transfer',
      true,
      true
    );
    createPackageStage(
      missingOpenSshClientStage,
      debianArchitecture,
      undefined,
      false,
      true
    );
    createPackageStage(
      missingXdgUtilsStage,
      debianArchitecture,
      undefined,
      true,
      false
    );
    for (const [stage, output] of [
      [goodStage, goodPackage],
      [missingCaStage, missingCaPackage],
      [systemJemallocStage, systemJemallocPackage],
      [badStage, badPackage],
      [missingFileTransferStage, missingFileTransferPackage],
      [missingOpenSshClientStage, missingOpenSshClientPackage],
      [missingXdgUtilsStage, missingXdgUtilsPackage],
    ]) {
      const built = run(dpkgDeb, [
        '--root-owner-group',
        '--build',
        stage,
        output,
      ]);
      expectSuccess(built, `test package creation failed: ${output}`);
    }

    const goodValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [goodPackage, canonicalArchitecture!]
    );
    expectSuccess(goodValidation, 'complete deb package was rejected');
    const systemJemallocValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [systemJemallocPackage, canonicalArchitecture!]
    );
    expect(systemJemallocValidation.status).not.toBe(0);
    expect(systemJemallocValidation.stderr).toContain('libjemalloc2');
    const missingControlStage = join(temporaryRoot, 'missing-control-stage');
    const missingControlPackage = join(temporaryRoot, 'missing-control.deb');
    createPackageStage(
      missingControlStage,
      debianArchitecture,
      'usr/bin/etctl',
      true,
      true
    );
    expectSuccess(
      run(dpkgDeb, [
        '--root-owner-group',
        '--build',
        missingControlStage,
        missingControlPackage,
      ]),
      'control test package creation failed'
    );
    const missingControlValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [missingControlPackage, canonicalArchitecture!]
    );
    expect(missingControlValidation.status).not.toBe(0);
    expect(missingControlValidation.stderr).toContain('usr/bin/etctl');
    for (const language of ['ja', 'en']) {
      const documents =
        language === 'en'
          ? ['webdav.md', 'webdav-validation.md']
          : ['webdav.md'];
      for (const document of documents) {
        const missingPath = `usr/share/doc/elder-terms/docs/${language}/${document}`;
        const stage = join(
          temporaryRoot,
          `missing-${language}-${document}-stage`
        );
        const output = join(
          temporaryRoot,
          `missing-${language}-${document}.deb`
        );
        createPackageStage(stage, debianArchitecture, missingPath, true, true);
        const built = run(dpkgDeb, [
          '--root-owner-group',
          '--build',
          stage,
          output,
        ]);
        expectSuccess(built, `test package creation failed: ${output}`);
        const validation = runSourced(
          'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
          [output, canonicalArchitecture!]
        );
        expect(validation.status).not.toBe(0);
        expect(validation.stderr).toContain(missingPath);
      }
    }

    for (const missingPath of [
      ...[
        'libcardio.so.0',
        'libjemalloc.so.2',
        'libboxrig.so.0',
        'libboxrig-ui.so.0',
        'libboxrig-session.so.0',
        'libboxrig-posix.so.0',
        'libboxrig-gtk3.so.0',
      ].map((name) => `usr/lib/elder-terms/${name}`),
      ...[
        'LICENSE',
        'README.md',
        'README_ja.md',
        'sources.json',
        'utf8proc-LICENSE.md',
        'libpng-LICENSE',
        'zlib-LICENSE',
        'Unicode-LICENSE.txt',
        'WHATWG-Encoding-LICENSE',
        'Wayland-protocols-NOTICES.md',
      ].map((name) => `usr/share/doc/elder-terms/licenses/boxrig/${name}`),
      'usr/share/doc/elder-terms/licenses/jemalloc/COPYING',
      ...[
        'NOTO-LICENSE',
        'NOTO-FLAGS-NOTICE',
        'MATERIAL-LICENSE',
        'UNICODE-LICENSE',
      ].map(
        (name) => `usr/share/doc/elder-terms/licenses/connection-icons/${name}`
      ),
    ]) {
      const name = missingPath.replace(/\//g, '-');
      const stage = join(temporaryRoot, `missing-${name}-stage`);
      const output = join(temporaryRoot, `missing-${name}.deb`);
      createPackageStage(stage, debianArchitecture, missingPath, true, true);
      expectSuccess(
        run(dpkgDeb, ['--root-owner-group', '--build', stage, output]),
        'incomplete runtime package creation failed'
      );
      const validation = runSourced(
        'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
        [output, canonicalArchitecture!]
      );
      expect(validation.status, missingPath).not.toBe(0);
      expect(validation.stderr).toContain(missingPath);
    }

    const missingCaValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [missingCaPackage, canonicalArchitecture!]
    );
    expect(missingCaValidation.status).not.toBe(0);
    expect(missingCaValidation.stderr).toContain('ca-certificates');

    const badValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [badPackage, canonicalArchitecture!]
    );
    expect(badValidation.status).not.toBe(0);
    expect(badValidation.stderr).toContain(
      'usr/share/applications/net.kekyo.elder-terms-terminal.desktop'
    );

    const missingFileTransferValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [missingFileTransferPackage, canonicalArchitecture!]
    );
    expect(missingFileTransferValidation.status).not.toBe(0);
    expect(missingFileTransferValidation.stderr).toContain(
      'usr/lib/elder-terms/elder-terms-terminal/elder-terms-file-transfer'
    );

    const missingOpenSshClientValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [missingOpenSshClientPackage, canonicalArchitecture!]
    );
    expect(missingOpenSshClientValidation.status).not.toBe(0);
    expect(missingOpenSshClientValidation.stderr).toContain('openssh-client');

    const missingXdgUtilsValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [missingXdgUtilsPackage, canonicalArchitecture!]
    );
    expect(missingXdgUtilsValidation.status).not.toBe(0);
    expect(missingXdgUtilsValidation.stderr).toContain('xdg-utils');
  });

  it('installs package data and resolvable public commands through Meson', async () => {
    const destination = join(temporaryRoot, 'meson-install');
    const buildDirectory = join(projectRoot, '.build');
    const introspection = run('meson', [
      'introspect',
      '--buildoptions',
      buildDirectory,
    ]);
    expectSuccess(introspection, 'Meson build option introspection failed');
    const buildOptions = JSON.parse(introspection.stdout) as Array<{
      name: string;
      value: unknown;
    }>;
    const stringOption = (name: string): string => {
      const value = buildOptions.find((option) => option.name === name)?.value;
      expect(typeof value).toBe('string');
      return value as string;
    };
    const prefix = stringOption('prefix');
    const libraryDirectory = stringOption('libdir');
    const install = run('meson', [
      'install',
      '-C',
      buildDirectory,
      // The workspace build has finished; parallel UI cases use these binaries.
      '--no-rebuild',
      '--destdir',
      destination,
    ]);
    expectSuccess(install, 'Meson package installation failed');

    const installedRoot = join(destination, prefix);
    const expectedLinks = new Map([
      [
        'bin/elder-terms',
        join('..', libraryDirectory, 'elder-terms/launcher/elder-terms'),
      ],
      [
        'bin/elder-terms-terminal',
        join(
          '..',
          libraryDirectory,
          'elder-terms/elder-terms-terminal/elder-terms-terminal'
        ),
      ],
      [
        'bin/elder-terms-file-transfer',
        join(
          '..',
          libraryDirectory,
          'elder-terms/elder-terms-terminal/elder-terms-file-transfer'
        ),
      ],
    ]);
    for (const [linkPath, expectedTarget] of expectedLinks) {
      const path = join(installedRoot, linkPath);
      expect(lstatSync(path).isSymbolicLink()).toBe(true);
      expect(readlinkSync(path)).toBe(expectedTarget);
    }
    for (const relativePath of [
      join(libraryDirectory, 'elder-terms/libelder-terms.so.0'),
      join(libraryDirectory, 'elder-terms/libjemalloc.so.2'),
      join(libraryDirectory, 'elder-terms/launcher/elder-terms'),
      join(libraryDirectory, 'elder-terms/launcher/main-window.ui'),
      join(
        libraryDirectory,
        'elder-terms/elder-terms-terminal/elder-terms-terminal'
      ),
      join(
        libraryDirectory,
        'elder-terms/elder-terms-terminal/elder-terms-file-transfer'
      ),
      join(libraryDirectory, 'elder-terms/elder-terms-terminal/main-window.ui'),
      join(libraryDirectory, 'elder-terms/elder-terms-terminal/green-on.png'),
      join(libraryDirectory, 'elder-terms/elder-terms-terminal/green-off.png'),
      'share/applications/net.kekyo.elder-terms.desktop',
      'share/applications/net.kekyo.elder-terms-terminal.desktop',
      'share/doc/elder-terms/licenses/jemalloc/COPYING',
    ]) {
      expect(lstatSync(join(installedRoot, relativePath)).isFile()).toBe(true);
    }

    const launcher = join(
      installedRoot,
      libraryDirectory,
      'elder-terms/launcher/elder-terms'
    );
    const dynamicSection = run('readelf', ['-d', launcher]);
    expectSuccess(dynamicSection, 'installed launcher ELF inspection failed');
    expect(dynamicSection.stdout).toContain('[$ORIGIN/..]');

    const terminal = join(installedRoot, 'bin/elder-terms-terminal');
    const environment = { ...process.env };
    delete environment.LD_LIBRARY_PATH;
    delete environment.LD_PRELOAD;
    // ldd's direct loader invocation does not resolve executable symlinks as exec does.
    const loaded = run('ldd', [realpathSync(terminal)], environment);
    expectSuccess(loaded, 'installed terminal dependency resolution failed');
    expect(loaded.stdout).not.toContain('not found');
    for (const component of [
      'elder-terms',
      'cardio',
      'boxrig',
      'boxrig-ui',
      'boxrig-session',
      'boxrig-posix',
      'boxrig-gtk3',
      'jemalloc',
    ]) {
      const name =
        component === 'jemalloc' ? 'libjemalloc.so.2' : `lib${component}.so.0`;
      const expectedPath = join(
        installedRoot,
        libraryDirectory,
        'elder-terms',
        name
      );
      const resolved = loaded.stdout
        .split('\n')
        .find((line) => line.trimStart().startsWith(`${name} => `))
        ?.split(' => ')[1]
        ?.split(' (')[0];
      expect(
        resolved,
        `${name} must be loaded from the installation`
      ).toBeDefined();
      expect(realpathSync(resolved!)).toBe(realpathSync(expectedPath));
      const libraryDynamicSection = run('readelf', ['-d', expectedPath]);
      expectSuccess(libraryDynamicSection, `${name} ELF inspection failed`);
      expect(libraryDynamicSection.stdout).toContain(
        `Library soname: [${name}]`
      );
      if (component !== 'jemalloc') {
        expect(libraryDynamicSection.stdout).toContain('[$ORIGIN]');
      }
    }

    const configPath = join(temporaryRoot, 'installed-terminal.ini');
    const peerPath = join(temporaryRoot, 'installed-peer.mjs');
    const receivedPath = join(temporaryRoot, 'installed-input.txt');
    writeFileSync(
      peerPath,
      `import { appendFileSync } from 'node:fs';
process.stdin.setRawMode(true);
process.stdin.on('data', (bytes) => appendFileSync(process.argv[2], bytes));
process.stdout.write('INSTALLED-TERMINAL-READY');
`
    );
    writeFileSync(
      configPath,
      `[general]\nauto_close=false\n[local]\ncommand_line=${process.execPath} ${peerPath} ${receivedPath}\n[log]\nenabled=true\nbase_directory=${temporaryRoot}\nfile_name_format=installed-output.log\n`
    );
    const appLauncher = createGtkAppLauncher({
      appPath: terminal,
      args: ['-c', configPath],
      env: {
        XDG_CONFIG_HOME: join(temporaryRoot, 'installed-config'),
        XDG_DATA_HOME: join(temporaryRoot, 'installed-data'),
        LD_LIBRARY_PATH: '',
        LD_PRELOAD: '',
        LIBGL_ALWAYS_SOFTWARE: '1',
        GTK_IM_MODULE: 'gtk-im-context-simple',
      },
    });
    try {
      const app = await appLauncher.launch();
      const view = await app.getById('terminal_view');
      expect((await view.info()).interfaces).toContain('Text');
      await toPass(() => {
        expect(
          readFileSync(join(temporaryRoot, 'installed-output.log'), 'utf8')
        ).toBe('INSTALLED-TERMINAL-READY');
      });
      const bounds = (await view.capture()).bounds;
      await app.input.moveMouseTo(bounds.x + 20, bounds.y + 20);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await app.input.pressKey('x');
      await toPass(() => {
        expect(readFileSync(receivedPath, 'utf8')).toBe('x');
      });
    } finally {
      await appLauncher.release();
    }
  });

  it('re-includes package data filtered by minimal distribution images', () => {
    const binDirectory = join(temporaryRoot, 'validation-bin');
    const invocationPath = join(temporaryRoot, 'dpkg-install.txt');
    mkdirSync(binDirectory);
    writeExecutable(
      join(binDirectory, 'dpkg'),
      '#!/bin/sh\nprintf \'%s\\n\' "$@" >"$ELDER_TERMS_TEST_DPKG_INSTALL"\nexit 73\n'
    );
    writeExecutable(join(binDirectory, 'dpkg-query'), '#!/bin/sh\nexit 74\n');
    const packagePath = join(temporaryRoot, 'elder-terms-good.deb');
    const result = run(containerScript, ['--validate-package', packagePath], {
      ...process.env,
      PATH: `${binDirectory}:${process.env.PATH}`,
      ELDER_TERMS_PACKAGE_NAME: 'elder-terms',
      ELDER_TERMS_PACKAGE_VERSION: '1.2.3',
      ELDER_TERMS_TEST_DPKG_INSTALL: invocationPath,
    });
    expect(result.status).toBe(73);
    expect(readFileSync(invocationPath, 'utf8')).toBe(
      [
        '--path-include=/usr/share/doc/elder-terms/*',
        '--path-include=/usr/share/locale/*/LC_MESSAGES/elder-terms.mo',
        '-i',
        packagePath,
        '',
      ].join('\n')
    );
  });

  it.runIf(debianTrixieI386PackageTestEnabled)(
    'builds the Debian trixie i386 release package',
    () => {
      const prerequisiteImage = run('podman', [
        'image',
        'exists',
        'localhost/elder-terms-pack-deb-debian-trixie-i686:latest',
      ]);
      expectSuccess(
        prerequisiteImage,
        'Debian trixie i386 prerequisite image is unavailable'
      );

      const result = run(packageScript, [
        '--version',
        '1.2.3-i386-regression',
        '--target',
        'deb',
        '--distro',
        'debian',
        '--release',
        'trixie',
        '--arch',
        'i386',
        '--jobs',
        '1',
      ]);
      expectSuccess(result, 'Debian trixie i386 package build failed');
    },
    1_200_000
  );
});
