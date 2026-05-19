import { chmod, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';

interface ExitingShellFixture {
  readonly markerPath: string;
  readonly shellPath: string;
}

const shellQuote = (value: string): string =>
  `'${value.split("'").join("'\\''")}'`;

const createExitingShellFixture = async (
  directory: string
): Promise<ExitingShellFixture> => {
  const markerPath = join(directory, 'shell-exited.txt');
  const shellPath = join(directory, 'exit-shell.sh');
  await writeFile(
    shellPath,
    `#!/bin/sh\nprintf exited > ${shellQuote(markerPath)}\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);

  return {
    markerPath,
    shellPath,
  };
};

const waitForShellExit = async (markerPath: string): Promise<void> => {
  await toPass(
    async () => {
      expect(await readFile(markerPath, 'utf8')).toBe('exited');
    },
    {
      message: 'local shell fixture should exit',
      timeoutMs: 5_000,
    }
  );
};

describe.concurrent('elder-terms-vte local session', () => {
  it('exits when the local shell exits and terminal auto_close is enabled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createExitingShellFixture(directory);

      await runGtkTest(
        context,
        [],
        async (app, evidence) => {
          await waitForShellExit(shell.markerPath);

          const output = await waitForResult(
            async () => {
              const currentOutput = await app.output();
              expect(currentOutput.exitCode).toBe(0);
              expect(currentOutput.exitSignal).toBeNull();
              return currentOutput;
            },
            {
              message: 'app should exit after the local shell exits',
              timeoutMs: 5_000,
            }
          );
          await evidence.log('local shell exit auto closed app', {
            exitCode: output.exitCode,
            exitSignal: output.exitSignal,
          });
        },
        {
          env: {
            SHELL: shell.shellPath,
          },
        }
      );
    });
  });

  it('keeps running when the local shell exits and terminal auto_close is disabled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createExitingShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app, evidence) => {
          await waitForShellExit(shell.markerPath);
          await delay(1_000);

          const output = await app.output();
          expect(output.exitCode).toBeNull();
          expect(output.exitSignal).toBeNull();
          await evidence.log('local shell exit left app running', {
            exitCode: output.exitCode,
            exitSignal: output.exitSignal,
          });
        },
        {
          env: {
            SHELL: shell.shellPath,
          },
        }
      );
    });
  });
});
