import { spawnSync, type SpawnSyncReturns } from 'node:child_process';
import {
  chmodSync,
  copyFileSync,
  lstatSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readlinkSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

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
  omittedPath: string | undefined
): void => {
  const directories = [
    'DEBIAN',
    'etc/xdg/autostart',
    'usr/bin',
    'usr/lib/elder-terms',
    'usr/lib/elder-terms/launcher',
    'usr/lib/elder-terms/elder-terms-vte',
    'usr/share/applications',
    'usr/share/doc/elder-terms',
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
Depends: libc6, dbus-user-session, hicolor-icon-theme
Description: GTK terminal for serial, TELNET, local shell, SSH, and SFTP connections
`
  );

  const executablePaths = [
    'usr/lib/elder-terms/libelder-terms.so',
    'usr/lib/elder-terms/launcher/elder-terms',
    'usr/lib/elder-terms/elder-terms-vte/elder-terms-vte',
    'usr/lib/elder-terms/elder-terms-vte/elder-terms-sftp',
  ];
  for (const executablePath of executablePaths) {
    copyFileSync('/bin/true', join(stageRoot, executablePath));
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
      'usr/share/applications/net.kekyo.elder-terms-vte.desktop',
      '[Desktop Entry]\nType=Application\nExec=elder-terms-vte\n',
    ],
    ['usr/lib/elder-terms/launcher/main-window.ui', '<interface/>\n'],
    ['usr/lib/elder-terms/elder-terms-vte/main-window.ui', '<interface/>\n'],
    ['usr/lib/elder-terms/elder-terms-vte/green-on.png', 'on\n'],
    ['usr/lib/elder-terms/elder-terms-vte/green-off.png', 'off\n'],
    ['usr/share/doc/elder-terms/README.md', '# elder-terms\n'],
    ['usr/share/doc/elder-terms/README_ja.md', '# elder-terms\n'],
    ['usr/share/doc/elder-terms/copyright', 'MIT\n'],
    ['usr/share/icons/hicolor/256x256/apps/elder-terms.png', 'icon\n'],
    ['usr/share/locale/ja/LC_MESSAGES/elder-terms.mo', 'locale\n'],
  ]);
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
    '../lib/elder-terms/elder-terms-vte/elder-terms-vte',
    join(stageRoot, 'usr/bin/elder-terms-vte')
  );
  symlinkSync(
    '../lib/elder-terms/elder-terms-vte/elder-terms-sftp',
    join(stageRoot, 'usr/bin/elder-terms-sftp')
  );
};

describe.sequential('multi-platform Debian package generation', () => {
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
prereq_image_for_target debian bookworm x86_64
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
        '13',
        join(
          projectRoot,
          'artifacts/deb/elder-terms-1.2.3-ubuntu-24.04-amd64.deb'
        ),
        'localhost/elder-terms-pack-deb-debian-bookworm-x86_64:latest',
        'docker.io/library/debian:trixie',
        '',
      ].join('\n')
    );
  });

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
exit 0
`
    );

    const result = run(containerScript, [], {
      ...process.env,
      PATH: `${binDirectory}:${process.env.PATH}`,
      ELDER_TERMS_BUILD_TYPE: 'release',
      ELDER_TERMS_MAKE_JOBS: '1',
      ELDER_TERMS_PACKAGE_DESCRIPTION: 'elder-terms package test',
      ELDER_TERMS_PACKAGE_MAINTAINER: 'test <test@localhost>',
      ELDER_TERMS_PACKAGE_NAME: 'elder-terms',
      ELDER_TERMS_PACKAGE_VERSION: '1.2.3-test',
      ELDER_TERMS_TEST_MESON_INVOCATION: invocationPath,
      ELDER_TERMS_TEST_PKG_CONFIG_INVOCATION: pkgConfigInvocationPath,
      ELDER_TERMS_WORK_DIR: join(temporaryRoot, 'container-work'),
    });
    expect(result.status).toBe(91);
    const invocation = readFileSync(invocationPath, 'utf8');
    expect(invocation).toContain('--prefix=/usr');
    expect(invocation).toContain('--libdir=lib');
    expect(invocation).toContain('-Dautostartdir=/etc/xdg/autostart');
    expect(invocation).toContain('-Dbuild_tests=false');
    expect(invocation).toContain('-Dwerror=false');
    expect(invocation).toContain('--buildtype=release');
    expect(invocation).toContain('-Dapplication_version=1.2.3-test');
    expect(readFileSync(pkgConfigInvocationPath, 'utf8').split('\n')).toContain(
      '--exists libcanberra'
    );
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
        'elder-terms-vte/libxyzm-static-libraries',
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
stage="$workspace/artifacts/.tmp/test-run/deb/debian/bookworm/x86_64/work/stage/elder-terms"
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
build_deb_package debian bookworm x86_64 linux/amd64
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
      'exists localhost/elder-terms-pack-deb-debian-bookworm-x86_64:latest'
    );
    expect(
      records.split('\n').filter((line) => line.startsWith('run '))
    ).toHaveLength(2);
    expect(records).toContain('./scripts/build_linux_dist_container.sh');
    expect(records).toContain(
      '--validate-package /workspace/artifacts/deb/elder-terms-1.2.3-test-debian-bookworm-amd64.deb'
    );
    expect(readFileSync(dpkgRecords, 'utf8')).toContain(
      join(
        fakeProject,
        'artifacts/deb/elder-terms-1.2.3-test-debian-bookworm-amd64.deb'
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
        'bookworm',
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
    expect(invocation).toContain('BASE_IMAGE=docker.io/amd64/debian:bookworm');
    expect(invocation).toContain(
      'localhost/elder-terms-pack-deb-debian-bookworm-x86_64:latest'
    );
    const containerfile = readFileSync(`${recordsPath}.containerfile`, 'utf8');
    for (const dependency of [
      'libcanberra-dev',
      'libgtk-3-dev',
      'libssh-dev',
      'libudev-dev',
      'liburing-dev',
      'libvte-2.91-dev',
      'libxkbcommon-dev',
      'meson',
      'npm',
    ]) {
      expect(containerfile).toContain(dependency);
    }
    expect(containerfile).toContain('npm ci --ignore-scripts');
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

    const goodStage = join(temporaryRoot, 'good-stage');
    const badStage = join(temporaryRoot, 'bad-stage');
    const goodPackage = join(temporaryRoot, 'elder-terms-good.deb');
    const badPackage = join(temporaryRoot, 'elder-terms-bad.deb');
    createPackageStage(goodStage, debianArchitecture, undefined);
    createPackageStage(
      badStage,
      debianArchitecture,
      'usr/share/applications/net.kekyo.elder-terms-vte.desktop'
    );
    for (const [stage, output] of [
      [goodStage, goodPackage],
      [badStage, badPackage],
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

    const badValidation = runSourced(
      'VERSION=1.2.3\nvalidate_deb_package "$2" "$3"',
      [badPackage, canonicalArchitecture!]
    );
    expect(badValidation.status).not.toBe(0);
    expect(badValidation.stderr).toContain(
      'usr/share/applications/net.kekyo.elder-terms-vte.desktop'
    );
  });

  it('installs package data and resolvable public commands through Meson', () => {
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
        'bin/elder-terms-vte',
        join(
          '..',
          libraryDirectory,
          'elder-terms/elder-terms-vte/elder-terms-vte'
        ),
      ],
      [
        'bin/elder-terms-sftp',
        join(
          '..',
          libraryDirectory,
          'elder-terms/elder-terms-vte/elder-terms-sftp'
        ),
      ],
    ]);
    for (const [linkPath, expectedTarget] of expectedLinks) {
      const path = join(installedRoot, linkPath);
      expect(lstatSync(path).isSymbolicLink()).toBe(true);
      expect(readlinkSync(path)).toBe(expectedTarget);
    }
    for (const relativePath of [
      join(libraryDirectory, 'elder-terms/libelder-terms.so'),
      join(libraryDirectory, 'elder-terms/launcher/elder-terms'),
      join(libraryDirectory, 'elder-terms/launcher/main-window.ui'),
      join(libraryDirectory, 'elder-terms/elder-terms-vte/elder-terms-vte'),
      join(libraryDirectory, 'elder-terms/elder-terms-vte/elder-terms-sftp'),
      join(libraryDirectory, 'elder-terms/elder-terms-vte/main-window.ui'),
      join(libraryDirectory, 'elder-terms/elder-terms-vte/green-on.png'),
      join(libraryDirectory, 'elder-terms/elder-terms-vte/green-off.png'),
      'share/applications/net.kekyo.elder-terms.desktop',
      'share/applications/net.kekyo.elder-terms-vte.desktop',
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
