import { execFile } from 'node:child_process';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createGtkAppLauncher, type GtkAppEnvironment } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const execFileAsync = promisify(execFile);
const buildDirectory = fileURLToPath(new URL('../../.build/', import.meta.url));
const launcherPath = fileURLToPath(
  new URL('../../.build/elder-terms/elder-terms', import.meta.url)
);
const desktopFileName = 'net.kekyo.elder-terms.desktop';
const applicationBusName = 'net.kekyo.elder-terms';

const execute = async (
  program: string,
  args: readonly string[],
  environment: GtkAppEnvironment | undefined = undefined
): Promise<{ readonly stdout: string; readonly stderr: string }> => {
  const result = await execFileAsync(program, [...args], {
    encoding: 'utf8',
    env:
      environment === undefined
        ? process.env
        : { ...process.env, ...environment },
    timeout: 15_000,
    killSignal: 'SIGKILL',
  });
  return { stdout: result.stdout, stderr: result.stderr };
};

const launcherWindowIds = async (
  environment: GtkAppEnvironment
): Promise<readonly string[]> => {
  const tree = await execute('xwininfo', ['-root', '-tree'], environment);
  const pattern = /^\s+(0x[0-9a-f]+) "elder-terms":.*?\s(\d+)x(\d+)[+-]/gmu;
  return [...tree.stdout.matchAll(pattern)]
    .filter((match) => Number(match[2]) >= 100 && Number(match[3]) >= 100)
    .map((match) => match[1]);
};

const applicationProcessId = async (
  environment: GtkAppEnvironment
): Promise<number> => {
  const reply = await execute(
    'gdbus',
    [
      'call',
      '--session',
      '--dest',
      'org.freedesktop.DBus',
      '--object-path',
      '/org/freedesktop/DBus',
      '--method',
      'org.freedesktop.DBus.GetConnectionUnixProcessID',
      applicationBusName,
    ],
    environment
  );
  const processId = reply.stdout.match(/\buint32 (\d+)\b/u)?.[1];
  if (processId === undefined) {
    throw new Error(`Cannot parse elder-terms process ID: ${reply.stdout}`);
  }
  return Number(processId);
};

const applicationHasOwner = async (
  environment: GtkAppEnvironment
): Promise<boolean> => {
  const reply = await execute(
    'gdbus',
    [
      'call',
      '--session',
      '--dest',
      'org.freedesktop.DBus',
      '--object-path',
      '/org/freedesktop/DBus',
      '--method',
      'org.freedesktop.DBus.NameHasOwner',
      applicationBusName,
    ],
    environment
  );
  return reply.stdout.includes('true');
};

describe('elder-terms XDG autostart', () => {
  it.each([
    { startupMode: 'window', expectedWindowCount: 1 },
    { startupMode: 'tray', expectedWindowCount: 0 },
  ] as const)(
    'launches with the configured $startupMode presentation',
    async ({ startupMode, expectedWindowCount }) => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-autostart-'));
      const stagingDirectory = join(directory, 'stage');
      const configHome = join(directory, 'config');
      const connections = join(configHome, 'elder-terms', 'connections');
      await mkdir(stagingDirectory, { recursive: true });
      await mkdir(connections, { recursive: true });
      await writeFile(
        join(configHome, 'elder-terms', 'global.ini'),
        `[general]\nstartup_mode=${startupMode}\nopen_application=\n`
      );

      const launcher = createGtkAppLauncher({
        appPath: launcherPath,
        env: {
          LANGUAGE: 'C',
          LC_ALL: 'C.UTF-8',
          XDG_CONFIG_HOME: configHome,
        },
        xvfbPool: {
          type: 'xvfb',
        },
        xvfbTrayHost: false,
      });
      let processId: number | undefined;
      try {
        await execute('meson', [
          'install',
          '-C',
          buildDirectory,
          '--no-rebuild',
          '--destdir',
          stagingDirectory,
        ]);
        const desktopFile = join(
          stagingDirectory,
          'etc',
          'xdg',
          'autostart',
          desktopFileName
        );
        await execute('desktop-file-validate', [desktopFile]);

        const environment = {
          ...(await launcher.environment()),
          PATH: `${dirname(launcherPath)}:${process.env.PATH ?? ''}`,
        };
        await execute('gio', ['launch', desktopFile], environment);
        await execute(
          'gdbus',
          ['wait', '--session', '--timeout', '10', applicationBusName],
          environment
        );
        processId = await applicationProcessId(environment);

        if (startupMode === 'tray') {
          await execute(launcherPath, ['--autostart'], environment);
        }
        await waitForResult(async () => {
          expect(await launcherWindowIds(environment)).toHaveLength(
            expectedWindowCount
          );
        });
        if (startupMode === 'tray') {
          await execute(launcherPath, [], environment);
          await waitForResult(async () => {
            expect(await launcherWindowIds(environment)).toHaveLength(1);
          });
        }
      } finally {
        if (processId !== undefined) {
          try {
            process.kill(processId, 'SIGTERM');
            const environment = await launcher.environment();
            await waitForResult(async () => {
              expect(await applicationHasOwner(environment)).toBe(false);
            });
          } catch (error) {
            if (!(
              error instanceof Error &&
              'code' in error &&
              error.code === 'ESRCH'
            )) {
              throw error;
            }
          }
        }
        await launcher.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  );
});
