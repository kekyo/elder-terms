import { chmod, readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import type { GtkApp, GtkCapture } from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import {
  assertTerminalCaptureMatches,
  assertDisconnectedNoticeMatches,
  expectDisconnectedNoticeHidden,
  expectMainWindowTitle,
  localTerminalDisconnectedDimPath,
  runGtkTest,
  withTemporaryDirectory,
} from './gtk-test-helpers';
import {
  activateTextSend,
  expectTextSendActive,
  expectTextSendFinished,
} from './text-send-test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

interface ExitingShellFixture {
  readonly markerPath: string;
  readonly shellPath: string;
}

interface TriggeredOutputShellFixture {
  readonly shellPath: string;
  readonly triggerPath: string;
}

interface RawInputShellFixture {
  readonly markerPath: string;
  readonly readyPath: string;
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

const createDelayedExitShellFixture = async (
  directory: string
): Promise<ExitingShellFixture> => {
  const markerPath = join(directory, 'delayed-shell-exited.txt');
  const shellPath = join(directory, 'delayed-exit-shell.sh');
  const output = 'LOCAL_CONN_DIM_MARKER '.repeat(60);
  await writeFile(
    shellPath,
    `#!/bin/sh\nprintf '%s\\n' ${shellQuote(output)}\nsleep 1\nprintf exited > ${shellQuote(markerPath)}\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);

  return {
    markerPath,
    shellPath,
  };
};

const createOutputShellFixture = async (directory: string): Promise<string> => {
  const shellPath = join(directory, 'output-shell.sh');
  const output = 'LOCAL_OUTPUT_MARKER '.repeat(40);
  await writeFile(
    shellPath,
    `#!/bin/sh\nprintf '%s\\n' ${shellQuote(output)}\nsleep 1\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);
  return shellPath;
};

const createRepeatingOutputShellFixture = async (
  directory: string
): Promise<TriggeredOutputShellFixture> => {
  const triggerPath = join(directory, 'repeating-output-trigger.txt');
  const shellPath = join(directory, 'repeating-output-shell.sh');
  await writeFile(
    shellPath,
    `#!/bin/sh\nwhile [ ! -f ${shellQuote(triggerPath)} ]; do\n  sleep 0.02\ndone\ni=0\nwhile [ "$i" -lt 200 ]; do\n  printf 'LOCAL_BLINK_MARKER %s\\n' "$i"\n  i=$((i + 1))\n  sleep 0.03\ndone\nsleep 1\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);
  return {
    shellPath,
    triggerPath,
  };
};

const createInputShellFixture = async (
  directory: string
): Promise<ExitingShellFixture> => {
  const markerPath = join(directory, 'shell-input.txt');
  const shellPath = join(directory, 'input-shell.sh');
  await writeFile(
    shellPath,
    `#!/bin/sh\nIFS= read -r input\nprintf '%s' "$input" > ${shellQuote(markerPath)}\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);

  return {
    markerPath,
    shellPath,
  };
};

const createRawInputShellFixture = async (
  directory: string
): Promise<RawInputShellFixture> => {
  const markerPath = join(directory, 'shell-raw-input.bin');
  const readyPath = join(directory, 'shell-raw-input-ready.txt');
  const shellPath = join(directory, 'raw-input-shell.sh');
  await writeFile(
    shellPath,
    `#!/bin/sh\nstty raw -echo\nprintf ready > ${shellQuote(
      readyPath
    )}\ndd bs=1 count=1 of=${shellQuote(markerPath)} 2>/dev/null\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);

  return {
    markerPath,
    readyPath,
    shellPath,
  };
};

const createSizeShellFixture = async (
  directory: string
): Promise<ExitingShellFixture> => {
  const markerPath = join(directory, 'shell-size.txt');
  const shellPath = join(directory, 'size-shell.sh');
  await writeFile(
    shellPath,
    `#!/bin/sh\nstty size > ${shellQuote(markerPath)}\nexit 0\n`,
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

const waitForFileText = async (
  path: string,
  expectedText: string
): Promise<void> => {
  await toPass(
    async () => {
      expect(await readFile(path, 'utf8')).toBe(expectedText);
    },
    {
      message: `file should contain ${expectedText}`,
      timeoutMs: 5_000,
    }
  );
};

const waitForRepeatedCharacter = async (
  path: string,
  character: string
): Promise<void> => {
  await toPass(
    async () => {
      expect(await readFile(path, 'utf8')).toMatch(
        new RegExp(`^${character}+$`)
      );
    },
    {
      message: `file should contain one or more ${character} characters`,
      timeoutMs: 5_000,
    }
  );
};

const expectNoVteSpawnRuntimeWarning = (stderr: string): void => {
  expect(stderr).not.toContain('VTE-WARNING');
  expect(stderr).not.toContain('runtime check failed');
  expect(stderr).not.toContain('ignored_spawn_flags');
};

const focusTerminal = async (app: GtkApp): Promise<void> => {
  const terminal = await app.getById('terminal_view');
  const terminalCapture = await terminal.capture();
  const terminalBounds = terminalCapture.bounds;
  await app.input.moveMouseTo(
    terminalBounds.x + terminalBounds.width / 2,
    terminalBounds.y + terminalBounds.height / 2
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
};

const pressKeyUntilSdIndicatorOn = async (
  app: GtkApp,
  key: string
): Promise<void> => {
  await focusTerminal(app);
  await toPass(
    async () => {
      await app.input.pressKey(key);
      await waitForActivityIndicatorImageState(app, 'sd', 'on', 400);
    },
    {
      message: 'SD indicator should light after local PTY input',
      timeoutMs: 5_000,
    }
  );
};

const brightPixelCount = (capture: GtkCapture): number => {
  const image = PNG.sync.read(capture.image);
  let count = 0;
  for (let offset = 0; offset < image.data.length; offset += 4) {
    const red = image.data[offset];
    const green = image.data[offset + 1];
    const blue = image.data[offset + 2];
    const alpha = image.data[offset + 3];
    if (alpha > 0 && red > 150 && green > 150 && blue > 150) {
      ++count;
    }
  }
  return count;
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
          expectNoVteSpawnRuntimeWarning(output.stderr);
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

  it('shows CONN while the local shell is running and clears it after exit', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createDelayedExitShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app, evidence) => {
          const connectedTitle = 'elder-terms: local terminal';
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await expectMainWindowTitle(app, connectedTitle);
          await expectDisconnectedNoticeHidden(app);

          await waitForShellExit(shell.markerPath);
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await expectMainWindowTitle(app, `${connectedTitle} (Disconnected)`);
          await assertDisconnectedNoticeMatches(app, evidence);
          await focusTerminal(app);
          await app.input.pressKey('z');
          await delay(300);
          await waitForActivityIndicatorImageState(app, 'sd', 'off');

          const output = await app.output();
          expect(output.exitCode).toBeNull();
          expect(output.exitSignal).toBeNull();
          await evidence.log('local CONN tracked shell lifetime', {
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

  it('dims the terminal image after the local shell disconnects', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createDelayedExitShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app, evidence) => {
          const terminal = await app.getById('terminal_view');
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await waitForShellExit(shell.markerPath);
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await assertTerminalCaptureMatches(
            terminal,
            'local-terminal-disconnected-dim',
            localTerminalDisconnectedDimPath,
            evidence,
            {
              // The disconnected VTE cursor may be captured in either blink phase.
              maxDiffPixels: 160,
            }
          );
        },
        {
          env: {
            SHELL: shell.shellPath,
          },
        }
      );
    });
  });

  it('feeds local shell output into the VTE terminal', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shellPath = await createOutputShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app, evidence) => {
          const terminal = await app.getById('terminal_view');
          const capture = await waitForResult(
            async () => {
              const currentCapture = await terminal.capture();
              expect(brightPixelCount(currentCapture)).toBeGreaterThan(1_000);
              return currentCapture;
            },
            {
              message: 'local shell output should be visible in the terminal',
              timeoutMs: 5_000,
            }
          );
          await evidence.captureEvidence(
            'local shell output',
            async () => capture
          );
        },
        {
          env: {
            SHELL: shellPath,
          },
        }
      );
    });
  });

  it('blinks RD when local shell output is read from the PTY', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createRepeatingOutputShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app) => {
          await writeFile(shell.triggerPath, 'start', 'utf8');
          await waitForActivityIndicatorImageState(app, 'rd', 'on');
          await delay(1_000);
          await waitForActivityIndicatorImageState(app, 'rd', 'off');
        },
        {
          env: {
            SHELL: shell.shellPath,
          },
        }
      );
    });
  });

  it('writes VTE user input to the local shell PTY', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createInputShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath, '--test-latch-activity-indicators'],
        async (app) => {
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await pressKeyUntilSdIndicatorOn(app, 'a');
          await app.input.pressKey('Return');
          await waitForRepeatedCharacter(shell.markerPath, 'a');
        },
        {
          env: {
            SHELL: shell.shellPath,
          },
        }
      );
    });
  });

  it('applies the local terminal backspace setting', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createRawInputShellFixture(directory);
      const configPath = join(directory, 'terminal-text.ini');
      await writeFile(
        configPath,
        '[terminal]\nauto_close=false\nbackspace_code=bs\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app) => {
          await waitForFileText(shell.readyPath, 'ready');
          await focusTerminal(app);
          await app.input.pressKey('BackSpace');

          await toPass(
            async () => {
              expect(
                Array.from((await readFile(shell.markerPath)).values())
              ).toEqual([0x08]);
            },
            {
              message:
                'local shell should receive the configured ASCII BS code',
              timeoutMs: 5_000,
            }
          );
        },
        {
          env: {
            SHELL: shell.shellPath,
          },
        }
      );
    });
  });

  it('sets the local PTY size from the configured VTE grid', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createSizeShellFixture(directory);
      const configPath = join(directory, 'terminal-size.ini');
      await writeFile(
        configPath,
        '[terminal]\nwidth=81\nheight=25\nauto_close=false\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['-c', configPath],
        async () => {
          await waitForFileText(shell.markerPath, '25 81\n');
        },
        {
          env: {
            SHELL: shell.shellPath,
          },
        }
      );
    });
  });

  it('sends encoded text read-only while local output remains active', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const sourcePath = join(directory, 'send.txt');
      const receivedPath = join(directory, 'received.bin');
      const readyPath = join(directory, 'ready.txt');
      const shellPath = join(directory, 'text-send-shell.sh');
      const logPath = join(directory, 'logs', 'cooked.txt');
      const configPath = join(directory, 'text-send.ini');
      const sourceText = '日本\x1b[A0123456789';
      await writeFile(sourcePath, sourceText, 'utf8');
      await writeFile(
        shellPath,
        `#!/bin/sh\nstty raw -echo\nprintf ready > ${shellQuote(
          readyPath
        )}\ndd bs=1 count=1 of=${shellQuote(
          receivedPath
        )} 2>/dev/null\nprintf LOCAL_DURING_TEXT_SEND\ndd bs=1 count=16 of=${shellQuote(
          receivedPath
        )} oflag=append conv=notrunc 2>/dev/null\nsleep 1\nexit 0\n`,
        'utf8'
      );
      await chmod(shellPath, 0o755);
      await writeFile(
        configPath,
        `[terminal]\nauto_close=false\nencoding=SHIFT-JIS\ncursor_key_mode=adm3\n\n[transfer]\ntext_send_bytes_per_second=10\n\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=logs/cooked.txt\nmode=cooked\n`,
        'utf8'
      );

      await runGtkTest(
        context,
        [
          '-c',
          configPath,
          `--test-transfer-source-uri=${pathToFileURL(sourcePath).href}`,
        ],
        async (app) => {
          await waitForFileText(readyPath, 'ready');
          const button = await activateTextSend(app);
          await expectTextSendActive(button);
          await focusTerminal(app);
          await app.input.pressKey('x');
          await expectTextSendFinished(app, button);

          await toPass(async () => {
            expect(Array.from((await readFile(receivedPath)).values())).toEqual(
              [
                0x93, 0xfa, 0x96, 0x7b, 0x1b, 0x5b, 0x41, 0x30, 0x31, 0x32,
                0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
              ]
            );
            expect(await readFile(logPath, 'utf8')).toContain(
              'LOCAL_DURING_TEXT_SEND'
            );
          });
        },
        {
          env: {
            SHELL: shellPath,
          },
        }
      );
    });
  });
});
