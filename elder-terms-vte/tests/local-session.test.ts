import { execFile } from 'node:child_process';
import { chmod, readFile, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { promisify } from 'node:util';
import type { GtkApp, GtkCapture } from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import {
  activateTerminalPaste,
  startClipboardTextProvider,
} from './clipboard-test-helpers';
import {
  assertTerminalCaptureMatches,
  expectDisconnectedNoticeHidden,
  expectDisconnectedNoticeVisibleAtTerminalTopRight,
  expectMainWindowStatus,
  expectMainWindowTitle,
  localTerminalDisconnectedDimPath,
  pressKeyWithModifiers,
  runGtkTest,
  withTemporaryDirectory,
} from './gtk-test-helpers';
import {
  capturePixel,
  expectCaptureToMatchFixture,
  expectElementKind,
} from './test-helpers';
import {
  activateTextSend,
  cancelTextSend,
  expectTextSendActive,
  expectTextSendFinished,
} from './text-send-test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const execFileAsync = promisify(execFile);
const connectionColorsDisconnectedNoticeFixturePath = fileURLToPath(
  new URL(
    './fixtures/connection-colors-disconnected-notice.png',
    import.meta.url
  )
);

interface ExitingShellFixture {
  readonly markerPath: string;
  readonly shellPath: string;
}

interface ControlledExitShellFixture extends ExitingShellFixture {
  readonly releasePath: string;
}

interface TriggeredOutputShellFixture {
  readonly finishedPath: string;
  readonly shellPath: string;
  readonly triggerPath: string;
}

interface RawInputShellFixture {
  readonly markerPath: string;
  readonly readyPath: string;
  readonly shellPath: string;
}

interface KeyboardProtocolShellFixture {
  readonly activeInputPath: string;
  readonly inactiveInputPath: string;
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

const createControlledExitShellFixture = async (
  directory: string
): Promise<ControlledExitShellFixture> => {
  const markerPath = join(directory, 'delayed-shell-exited.txt');
  const releasePath = join(directory, 'delayed-shell-release');
  const shellPath = join(directory, 'delayed-exit-shell.sh');
  const output = 'LOCAL_CONN_DIM_MARKER '.repeat(60);
  await execFileAsync('/usr/bin/mkfifo', [releasePath]);
  await writeFile(
    shellPath,
    `#!/bin/sh\nprintf '%s\\n' ${shellQuote(
      output
    )}\nIFS= read -r release < ${shellQuote(
      releasePath
    )}\nprintf exited > ${shellQuote(markerPath)}\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);

  return {
    markerPath,
    releasePath,
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
  const finishedPath = join(directory, 'repeating-output-finished.txt');
  const shellPath = join(directory, 'repeating-output-shell.sh');
  await writeFile(
    shellPath,
    `#!/bin/sh\nwhile [ ! -f ${shellQuote(triggerPath)} ]; do\n  sleep 0.02\ndone\ni=0\nwhile [ "$i" -lt 50 ]; do\n  printf 'LOCAL_BLINK_MARKER %s\\n' "$i"\n  i=$((i + 1))\n  sleep 0.03\ndone\nprintf finished > ${shellQuote(
      finishedPath
    )}\nsleep 1\nexit 0\n`,
    'utf8'
  );
  await chmod(shellPath, 0o755);
  return {
    finishedPath,
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

const createKeyboardProtocolShellFixture = async (
  directory: string
): Promise<KeyboardProtocolShellFixture> => {
  const activeInputPath = join(directory, 'keyboard-protocol-active.bin');
  const inactiveInputPath = join(directory, 'keyboard-protocol-inactive.bin');
  const shellPath = join(directory, 'keyboard-protocol-shell.sh');
  await writeFile(
    shellPath,
    `#!/bin/sh
stty raw -echo
printf '\\033[>7uKEYBOARD_PROTOCOL_ACTIVE\\n'
dd bs=1 count=7 iflag=fullblock of=${shellQuote(activeInputPath)} 2>/dev/null
printf '\\033[<uKEYBOARD_PROTOCOL_INACTIVE\\n'
dd bs=1 count=1 iflag=fullblock of=${shellQuote(inactiveInputPath)} 2>/dev/null
exit 0
`,
    'utf8'
  );
  await chmod(shellPath, 0o755);

  return {
    activeInputPath,
    inactiveInputPath,
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
  it('starts the configured local process with exact arguments instead of the user shell', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const markerPath = join(directory, 'configured-process-arguments.txt');
      const fallbackMarkerPath = join(directory, 'fallback-shell-started.txt');
      const commandPath = join(directory, 'configured-local-process');
      const fallbackShellPath = join(directory, 'fallback-shell.sh');
      const configPath = join(directory, 'configured-local-process.ini');
      await writeFile(
        commandPath,
        `#!/bin/sh\nprintf '%s\\n' "$#" "$@" > ${shellQuote(
          markerPath
        )}\nexit 0\n`,
        'utf8'
      );
      await chmod(commandPath, 0o755);
      await writeFile(
        fallbackShellPath,
        `#!/bin/sh\nprintf fallback > ${shellQuote(
          fallbackMarkerPath
        )}\nexit 0\n`,
        'utf8'
      );
      await chmod(fallbackShellPath, 0o755);
      await writeFile(
        configPath,
        "[local]\ncommand_line=configured-local-process alpha 'two words' $HOME '*'\n\n[terminal]\nauto_close=false\n",
        'utf8'
      );

      await runGtkTest(
        context,
        ['-c', configPath],
        async (_app, evidence) => {
          await waitForFileText(
            markerPath,
            ['4', 'alpha', 'two words', '$HOME', '*', ''].join('\n')
          );
          await expect(
            readFile(fallbackMarkerPath, 'utf8')
          ).rejects.toMatchObject({ code: 'ENOENT' });
          await evidence.log('configured local process arguments preserved');
        },
        {
          env: {
            PATH: `${directory}:${process.env.PATH ?? ''}`,
            SHELL: fallbackShellPath,
          },
        }
      );
    });
  });

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
      const shell = await createControlledExitShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      const background = [0x60, 0x40, 0x20] as const;
      await writeFile(
        configPath,
        [
          '[general]',
          'background=#604020',
          '',
          '[terminal]',
          'auto_close=false',
          '',
        ].join('\n'),
        'utf8'
      );

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app, evidence) => {
          const connectedTitle = 'elder-terms: auto-close-disabled';
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await expectMainWindowTitle(app, connectedTitle);
          await expectMainWindowStatus(app, 'local terminal');
          await expectDisconnectedNoticeHidden(app);

          await writeFile(shell.releasePath, 'exit\n', 'utf8');
          await waitForShellExit(shell.markerPath);
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await expectMainWindowTitle(app, `${connectedTitle} (Disconnected)`);
          await expectMainWindowStatus(app, 'local terminal');
          await expectDisconnectedNoticeVisibleAtTerminalTopRight(app);
          for (const [widgetId, horizontalRatio] of [
            ['disconnected_notice', 0.05],
            ['disconnected_notice_background', 0.05],
            ['disconnected_notice_label', 0.95],
          ] as const) {
            expect(
              capturePixel(
                await (await app.getById(widgetId)).capture(),
                horizontalRatio,
                0.5
              )
            ).toEqual(background);
          }
          const noticeCapture = await evidence.captureEvidence(
            'connection-colors-disconnected-notice',
            async () => (await app.getById('disconnected_notice')).capture()
          );
          await expectCaptureToMatchFixture(
            noticeCapture,
            'connection-colors-disconnected-notice',
            connectionColorsDisconnectedNoticeFixturePath,
            evidence
          );
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
      const shell = await createControlledExitShellFixture(directory);
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app, evidence) => {
          const terminal = await app.getById('terminal_view');
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await writeFile(shell.releasePath, 'exit\n', 'utf8');
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
          await waitForFileText(shell.finishedPath, 'finished');
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

  it('applies the automatic local terminal backspace setting', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createRawInputShellFixture(directory);
      const configPath = join(directory, 'terminal-text.ini');
      await writeFile(
        configPath,
        '[terminal]\nauto_close=false\nbackspace_code=auto\n',
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
                'local shell should receive the VTE automatic Backspace code',
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

  it('sends the configured Return code from the local terminal', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      for (const testCase of [
        { expected: [0x0d], name: 'cr' },
        { expected: [0x0a], name: 'lf' },
        { expected: [0x0d, 0x0a], name: 'crlf' },
      ] as const) {
        const markerPath = join(directory, `${testCase.name}.bin`);
        const readyPath = join(directory, `${testCase.name}-ready.txt`);
        const shellPath = join(directory, `${testCase.name}-shell.sh`);
        const configPath = join(directory, `${testCase.name}.ini`);
        await writeFile(
          shellPath,
          `#!/bin/sh\nstty raw -echo\nprintf ready > ${shellQuote(
            readyPath
          )}\ndd bs=1 count=${testCase.expected.length} of=${shellQuote(
            markerPath
          )} 2>/dev/null\nexit 0\n`,
          'utf8'
        );
        await chmod(shellPath, 0o755);
        await writeFile(
          configPath,
          `[terminal]\nauto_close=false\nreturn_code=${testCase.name}\n`,
          'utf8'
        );

        await runGtkTest(
          context,
          ['-c', configPath],
          async (app) => {
            await waitForFileText(readyPath, 'ready');
            await focusTerminal(app);
            await app.input.pressKey('Return');
            await toPass(
              async () => {
                expect(
                  Array.from((await readFile(markerPath)).values())
                ).toEqual(testCase.expected);
              },
              {
                message: `local Return should send ${testCase.name}`,
                timeoutMs: 5_000,
              }
            );
          },
          {
            env: {
              SHELL: shellPath,
            },
          }
        );
      }
    });
  });

  it('encodes Ctrl+Enter only while the app requests enhanced keyboard input', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const shell = await createKeyboardProtocolShellFixture(directory);
      const logPath = join(directory, 'logs', 'cooked.txt');
      const configPath = join(directory, 'keyboard-protocol.ini');
      await writeFile(
        configPath,
        `[terminal]
auto_close=false

[log]
enabled=true
base_directory=${directory}
file_name_format=logs/cooked.txt
mode=cooked
`,
        'utf8'
      );

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app) => {
          await toPass(
            async () => {
              expect(await readFile(logPath, 'utf8')).toContain(
                'KEYBOARD_PROTOCOL_ACTIVE'
              );
            },
            {
              message: 'local shell keyboard mode push should be observed',
              timeoutMs: 5_000,
            }
          );
          await focusTerminal(app);
          await pressKeyWithModifiers(app, ['control'], 'Return');
          await toPass(
            async () => {
              expect(
                Array.from((await readFile(shell.activeInputPath)).values())
              ).toEqual([0x1b, 0x5b, 0x31, 0x33, 0x3b, 0x35, 0x75]);
            },
            {
              message: 'Ctrl+Enter should be encoded as CSI 13;5u',
              timeoutMs: 5_000,
            }
          );

          await toPass(
            async () => {
              expect(await readFile(logPath, 'utf8')).toContain(
                'KEYBOARD_PROTOCOL_INACTIVE'
              );
            },
            {
              message: 'local shell keyboard mode pop should be observed',
              timeoutMs: 5_000,
            }
          );
          await pressKeyWithModifiers(app, ['control'], 'Return');
          await toPass(
            async () => {
              expect(
                Array.from((await readFile(shell.inactiveInputPath)).values())
              ).toEqual([0x0d]);
            },
            {
              message: 'Ctrl+Enter should return to legacy CR after pop',
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
        `[terminal]\nauto_close=false\nencoding=SHIFT-JIS\ncursor_key_mode=trs80\n\n[transfer]\ntext_send_bytes_per_second=10\n\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=logs/cooked.txt\nmode=cooked\n`,
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
          await expectTextSendFinished(app, button, 'local terminal');

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

  it('applies the text-send Return-code setting to file line endings', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const sourceText = 'a\r\nb\rc\nd';
      for (const testCase of [
        { expected: 'a\nb\nc\nd', follow: true, name: 'follow' },
        { expected: sourceText, follow: false, name: 'preserve' },
      ] as const) {
        const sourcePath = join(directory, `${testCase.name}-send.txt`);
        const receivedPath = join(directory, `${testCase.name}-received.bin`);
        const receivedReadyPath = join(
          directory,
          `${testCase.name}-received-ready.txt`
        );
        const readyPath = join(directory, `${testCase.name}-ready.txt`);
        const releasePath = join(directory, `${testCase.name}-release`);
        const shellPath = join(directory, `${testCase.name}-shell.sh`);
        const configPath = join(directory, `${testCase.name}.ini`);
        await execFileAsync('/usr/bin/mkfifo', [releasePath]);
        await writeFile(sourcePath, sourceText, 'utf8');
        await writeFile(
          shellPath,
          `#!/bin/sh\nstty raw -echo\nprintf ready > ${shellQuote(
            readyPath
          )}\ndd bs=1 count=${Buffer.byteLength(
            testCase.expected
          )} of=${shellQuote(receivedPath)} 2>/dev/null\nprintf received > ${shellQuote(
            receivedReadyPath
          )}\nIFS= read -r release < ${shellQuote(releasePath)}\nexit 0\n`,
          'utf8'
        );
        await chmod(shellPath, 0o755);
        await writeFile(
          configPath,
          `[terminal]\nauto_close=false\nreturn_code=lf\n\n[transfer]\ntext_send_bytes_per_second=8000000\ntext_send_follow_return_code=${String(
            testCase.follow
          )}\n`,
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
            let shellWaitingForRelease = false;
            try {
              const button = await activateTextSend(app);
              await waitForFileText(receivedReadyPath, 'received');
              shellWaitingForRelease = true;
              await expectTextSendFinished(app, button, 'local terminal');
              expect(await readFile(receivedPath, 'utf8')).toBe(
                testCase.expected
              );
            } finally {
              if (shellWaitingForRelease) {
                await writeFile(releasePath, 'release\n', 'utf8');
              }
            }
          },
          {
            env: {
              SHELL: shellPath,
            },
          }
        );
      }
    });
  });

  it('cancels an active text send and restores terminal input', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const sourcePath = join(directory, 'send.txt');
      const receivedPath = join(directory, 'received.bin');
      const readyPath = join(directory, 'ready.txt');
      const shellPath = join(directory, 'text-send-cancel-shell.sh');
      const configPath = join(directory, 'text-send-cancel.ini');
      await writeFile(sourcePath, '0123456789', 'utf8');
      await writeFile(
        shellPath,
        `#!/bin/sh\nstty raw -echo\nprintf ready > ${shellQuote(
          readyPath
        )}\ndd bs=1 count=2 of=${shellQuote(
          receivedPath
        )} 2>/dev/null\nexit 0\n`,
        'utf8'
      );
      await chmod(shellPath, 0o755);
      await writeFile(
        configPath,
        '[terminal]\nauto_close=false\n\n[transfer]\ntext_send_bytes_per_second=1\n',
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
          await toPass(async () => {
            expect((await readFile(receivedPath)).length).toBe(1);
          });

          await cancelTextSend(app, button);
          await focusTerminal(app);
          await app.input.pressKey('x');

          await toPass(async () => {
            expect(await readFile(receivedPath, 'utf8')).toBe('0x');
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

  it('pastes encoded clipboard text read-only at the configured rate', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const receivedPath = join(directory, 'pasted.bin');
      const readyPath = join(directory, 'paste-ready.txt');
      const shellPath = join(directory, 'paste-shell.sh');
      const logPath = join(directory, 'logs', 'cooked.txt');
      const configPath = join(directory, 'paste.ini');
      const clipboardText = '日本😀0123456789';
      await writeFile(
        shellPath,
        `#!/bin/sh\nstty raw -echo\nprintf ready > ${shellQuote(
          readyPath
        )}\ndd bs=1 count=1 of=${shellQuote(
          receivedPath
        )} 2>/dev/null\nprintf LOCAL_DURING_PASTE\ndd bs=1 count=14 of=${shellQuote(
          receivedPath
        )} oflag=append conv=notrunc 2>/dev/null\nsleep 1\nexit 0\n`,
        'utf8'
      );
      await chmod(shellPath, 0o755);
      await writeFile(
        configPath,
        `[terminal]\nauto_close=false\nencoding=SHIFT-JIS\n\n[transfer]\ntext_send_bytes_per_second=10\n\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=logs/cooked.txt\nmode=cooked\n`,
        'utf8'
      );

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app) => {
          await waitForFileText(readyPath, 'ready');
          const provider = await startClipboardTextProvider(app, clipboardText);
          try {
            const button = await activateTerminalPaste(app);
            await expectTextSendActive(button);
            await focusTerminal(app);
            await app.input.pressKey('x');
            await expectTextSendFinished(app, button, 'local terminal');
          } finally {
            await provider.close();
          }

          await toPass(async () => {
            expect(Array.from((await readFile(receivedPath)).values())).toEqual(
              [
                0x93, 0xfa, 0x96, 0x7b, 0x3f, 0x30, 0x31, 0x32, 0x33, 0x34,
                0x35, 0x36, 0x37, 0x38, 0x39,
              ]
            );
            expect(await readFile(logPath, 'utf8')).toContain(
              'LOCAL_DURING_PASTE'
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

  it('normalizes line endings for every terminal paste route', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const pastedText = 'a\r\nb\rc\nd';
      const expectedText = 'a\nb\nc\nd';
      const routes = [
        { name: 'context', selection: 'clipboard' },
        { name: 'shift-insert', selection: 'clipboard' },
        { name: 'middle', selection: 'primary' },
      ] as const;

      for (const route of routes) {
        const receivedPath = join(directory, `${route.name}-pasted.bin`);
        const receivedReadyPath = join(
          directory,
          `${route.name}-received-ready.txt`
        );
        const readyPath = join(directory, `${route.name}-ready.txt`);
        const releasePath = join(directory, `${route.name}-release`);
        const shellPath = join(directory, `${route.name}-shell.sh`);
        const configPath = join(directory, `${route.name}.ini`);
        await execFileAsync('/usr/bin/mkfifo', [releasePath]);
        await writeFile(
          shellPath,
          `#!/bin/sh\nstty raw -echo\nprintf ready > ${shellQuote(
            readyPath
          )}\ndd bs=1 count=${Buffer.byteLength(
            expectedText
          )} of=${shellQuote(receivedPath)} 2>/dev/null\nprintf received > ${shellQuote(
            receivedReadyPath
          )}\nIFS= read -r release < ${shellQuote(releasePath)}\nexit 0\n`,
          'utf8'
        );
        await chmod(shellPath, 0o755);
        await writeFile(
          configPath,
          '[terminal]\nauto_close=false\nreturn_code=lf\n\n' +
            '[transfer]\ntext_send_bytes_per_second=10\n' +
            'text_send_follow_return_code=true\n',
          'utf8'
        );

        await runGtkTest(
          context,
          ['-c', configPath],
          async (app) => {
            await waitForFileText(readyPath, 'ready');
            await focusTerminal(app);
            const provider = await startClipboardTextProvider(
              app,
              pastedText,
              route.selection
            );
            let shellWaitingForRelease = false;
            try {
              const button = expectElementKind(
                await app.getById('transfer_button'),
                'toggleButton'
              );
              if (route.name === 'context') {
                await activateTerminalPaste(app);
              } else if (route.name === 'shift-insert') {
                await pressKeyWithModifiers(app, ['shift'], 'Insert');
              } else {
                const terminalCapture = await (
                  await app.getById('terminal_view')
                ).capture();
                await app.input.moveMouseTo(
                  Math.trunc(
                    terminalCapture.bounds.x + terminalCapture.bounds.width / 2
                  ),
                  Math.trunc(
                    terminalCapture.bounds.y + terminalCapture.bounds.height / 2
                  )
                );
                await app.input.setMouseButton('middle', true);
                await app.input.setMouseButton('middle', false);
              }
              await expectTextSendActive(button);
              await waitForFileText(receivedReadyPath, 'received');
              shellWaitingForRelease = true;
              await expectTextSendFinished(app, button, 'local terminal');
              expect(await readFile(receivedPath, 'utf8')).toBe(expectedText);
            } finally {
              if (shellWaitingForRelease) {
                await writeFile(releasePath, 'release\n', 'utf8');
              }
              await provider.close();
            }
          },
          {
            env: {
              SHELL: shellPath,
            },
          }
        );
      }
    });
  });
});
