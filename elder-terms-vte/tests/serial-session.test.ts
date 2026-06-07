import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { once } from 'node:events';
import { rm, symlink, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import type { GtkApp } from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import {
  activityIndicatorIconSize,
  activityIndicatorLabels,
  captureActivityIndicatorBox,
  captureActivityIndicatorImage,
  expectActivityIndicatorImageState,
  serialActivityIndicatorIds,
  waitForActivityIndicatorImageState,
} from './activity-indicator-test-helpers';
import {
  expectDisconnectedNoticeHidden,
  expectDisconnectedNoticeRenderedUndimmedAtTerminalTopRight,
  expectDisconnectedNoticeVisibleAtTerminalTopRight,
  expectMainWindowTitle,
  runGtkTest,
  withTemporaryDirectory,
} from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

const helperPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/serial-pty-helper', import.meta.url)
);

interface SerialPtyHelper {
  readonly lines: readonly string[];
  readonly slavePath: string;
  readonly writeCommand: (command: string) => void;
  readonly close: () => Promise<void>;
}

const startSerialPtyHelper = async (): Promise<SerialPtyHelper> => {
  const child = spawn(helperPath, [], {
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const lines: string[] = [];
  let stdoutBuffer = '';
  let stderr = '';

  return new Promise<SerialPtyHelper>((resolve, reject) => {
    let resolved = false;

    const rejectIfPending = (error: Error): void => {
      if (!resolved) {
        reject(error);
      }
    };

    child.stderr.on('data', (chunk: Buffer) => {
      stderr += chunk.toString('utf8');
    });
    child.stdout.on('data', (chunk: Buffer) => {
      stdoutBuffer += chunk.toString('utf8');
      let newline = stdoutBuffer.indexOf('\n');
      while (newline >= 0) {
        const line = stdoutBuffer.slice(0, newline);
        stdoutBuffer = stdoutBuffer.slice(newline + 1);
        lines.push(line);
        if (!resolved && line.startsWith('READY ')) {
          resolved = true;
          resolve({
            lines,
            slavePath: line.slice('READY '.length),
            writeCommand: (command: string): void => {
              child.stdin.write(`${command}\n`);
            },
            close: async (): Promise<void> => {
              await closeSerialPtyHelper(child);
            },
          });
        }
        newline = stdoutBuffer.indexOf('\n');
      }
    });
    child.once('error', rejectIfPending);
    child.once('exit', (code, signal) => {
      rejectIfPending(
        new Error(
          `serial pty helper exited before ready: code=${code} signal=${signal} stderr=${stderr}`
        )
      );
    });
  });
};

const closeSerialPtyHelper = async (
  child: ChildProcessWithoutNullStreams
): Promise<void> => {
  if (child.exitCode !== null || child.signalCode !== null) {
    return;
  }

  child.stdin.write('QUIT\n');
  const timeout = setTimeout(() => {
    child.kill('SIGKILL');
  }, 2_000);
  try {
    await once(child, 'exit');
  } finally {
    clearTimeout(timeout);
  }
};

const hasReceivedHex = (
  helper: SerialPtyHelper,
  expectedHex: string
): boolean =>
  helper.lines.some(
    (line) => line.startsWith('RX ') && line.includes(expectedHex)
  );

const zmodemZrqinitPreamble = (): string => `**\x18B00000000000000`;

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

const pressKeyUntilReceived = async (
  app: GtkApp,
  helper: SerialPtyHelper,
  key: string,
  expectedHex: string
): Promise<void> => {
  await focusTerminal(app);
  await toPass(
    async () => {
      await app.input.pressKey(key);
      expect(hasReceivedHex(helper, expectedHex)).toBe(true);
    },
    {
      message: `serial PTY helper should receive ${expectedHex}`,
      timeoutMs: 7_000,
    }
  );
};

const pressKeyUntilReceivedAndSdIndicatorOn = async (
  app: GtkApp,
  helper: SerialPtyHelper,
  key: string,
  expectedHex: string
): Promise<void> => {
  await focusTerminal(app);
  await toPass(
    async () => {
      await app.input.pressKey(key);
      await delay(30);
      await expectActivityIndicatorImageState(
        await captureActivityIndicatorImage(app, 'sd'),
        'on'
      );
      expect(hasReceivedHex(helper, expectedHex)).toBe(true);
    },
    {
      message: `serial PTY helper should receive ${expectedHex} and SD should light`,
      timeoutMs: 7_000,
    }
  );
};

const expectSerialActivityIndicatorsVisibleInitialState = async (
  app: GtkApp
): Promise<void> => {
  const boxCaptures = await Promise.all(
    serialActivityIndicatorIds.map((indicator) =>
      captureActivityIndicatorBox(app, indicator)
    )
  );
  for (let index = 1; index < boxCaptures.length; ++index) {
    expect(boxCaptures[index - 1].bounds.x).toBeLessThan(
      boxCaptures[index].bounds.x
    );
  }

  for (const indicator of serialActivityIndicatorIds) {
    const label = expectElementKind(
      await app.getById(`${indicator}_indicator_label`),
      'label'
    );
    expect(await label.text()).toBe(activityIndicatorLabels[indicator]);

    const imageCapture = await captureActivityIndicatorImage(app, indicator);
    expect(imageCapture.bounds.width).toBe(activityIndicatorIconSize);
    expect(imageCapture.bounds.height).toBe(activityIndicatorIconSize);
    await expectActivityIndicatorImageState(
      imageCapture,
      indicator === 'conn' ? 'on' : 'off'
    );
  }
};

describe.concurrent('elder-terms-vte serial session', () => {
  it('connects to a PTY serial device and transfers data in both directions', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const helper = await startSerialPtyHelper();
      try {
        const configPath = join(directory, 'serial.ini');
        const serialDevicePath = join(directory, 'ttyELDERTERMS0');
        await symlink(helper.slavePath, serialDevicePath);
        await writeFile(
          configPath,
          `[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=${serialDevicePath}\nbaudrate=9600\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(
            async () => {
              expect((await app.output()).stderr).toContain(
                'serial carrier detection unavailable'
              );
            },
            {
              message: 'serial session should start against the PTY',
              timeoutMs: 5_000,
            }
          );
          await expectSerialActivityIndicatorsVisibleInitialState(app);
          await toPass(
            async () => {
              helper.writeCommand('TX serial-output');
              await waitForActivityIndicatorImageState(app, 'rd', 'on', 400);
            },
            {
              message: 'RD indicator should light after serial input',
              timeoutMs: 7_000,
            }
          );
          await delay(500);
          await waitForActivityIndicatorImageState(app, 'rd', 'off');
          await pressKeyUntilReceivedAndSdIndicatorOn(app, helper, 'a', '61');
          await delay(500);
          await waitForActivityIndicatorImageState(app, 'sd', 'off');
        });
      } finally {
        await helper.close();
      }
    });
  });

  it('exits when an active serial session window is closed', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const helper = await startSerialPtyHelper();
      try {
        const configPath = join(directory, 'serial.ini');
        const serialDevicePath = join(directory, 'ttyELDERTERMS0');
        await symlink(helper.slavePath, serialDevicePath);
        await writeFile(
          configPath,
          `[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=${serialDevicePath}\nbaudrate=9600\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app, evidence) => {
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          const closeButton = expectElementKind(
            await app.getByPath('main_window.0.0.3'),
            'button'
          );
          await closeButton.click();

          const output = await waitForResult(
            async () => {
              const currentOutput = await app.output();
              expect(currentOutput.exitCode).toBe(0);
              expect(currentOutput.exitSignal).toBeNull();
              return currentOutput;
            },
            {
              message: 'app should exit after closing an active serial window',
              timeoutMs: 5_000,
            }
          );
          await evidence.log('active serial window close exited app', {
            exitCode: output.exitCode,
            exitSignal: output.exitSignal,
          });
        });
      } finally {
        await helper.close();
      }
    });
  });

  it('exits when an initially disconnected serial session window is closed', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      const serialDevicePath = join(directory, 'ttyELDERTERMS0');
      await writeFile(
        configPath,
        `[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=${serialDevicePath}\nbaudrate=9600\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n`,
        'utf8'
      );

      await runGtkTest(context, ['-c', configPath], async (app, evidence) => {
        await toPass(
          async () => {
            expect((await app.output()).stderr).toContain(
              'serial device not found'
            );
          },
          {
            message: 'serial session should report the missing device',
            timeoutMs: 5_000,
          }
        );
        await waitForActivityIndicatorImageState(app, 'conn', 'off');

        const closeButton = expectElementKind(
          await app.getByPath('main_window.0.0.3'),
          'button'
        );
        await closeButton.click();

        const output = await waitForResult(
          async () => {
            const currentOutput = await app.output();
            expect(currentOutput.exitCode).toBe(0);
            expect(currentOutput.exitSignal).toBeNull();
            return currentOutput;
          },
          {
            message:
              'app should exit after closing an initially disconnected serial window',
            timeoutMs: 5_000,
          }
        );
        await evidence.log('initially disconnected serial close exited app', {
          exitCode: output.exitCode,
          exitSignal: output.exitSignal,
        });
      });
    });
  });

  it('auto-starts ZMODEM receive from a serial preamble by default', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const helper = await startSerialPtyHelper();
      try {
        const configPath = join(directory, 'serial.ini');
        const serialDevicePath = join(directory, 'ttyELDERTERMS0');
        await symlink(helper.slavePath, serialDevicePath);
        await writeFile(
          configPath,
          `[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=${serialDevicePath}\nbaudrate=9600\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(
            async () => {
              expect((await app.output()).stderr).toContain(
                'serial carrier detection unavailable'
              );
            },
            {
              message: 'serial session should start against the PTY',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          helper.writeCommand(`TX ${zmodemZrqinitPreamble()}`);

          await toPass(
            async () => {
              expect(hasReceivedHex(helper, '2a2a18423031')).toBe(true);
            },
            {
              message:
                'serial session should answer auto-start ZRQINIT with ZRINIT',
              timeoutMs: 7_000,
            }
          );
        });
      } finally {
        await helper.close();
      }
    });
  });

  it('reconnects when a missing serial device appears with auto_close disabled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      const serialDevicePath = join(directory, 'ttyELDERTERMS0');
      let helper: SerialPtyHelper | undefined;

      try {
        await writeFile(
          configPath,
          `[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=${serialDevicePath}\nbaudrate=9600\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(
            async () => {
              expect((await app.output()).stderr).toContain(
                'serial device not found'
              );
            },
            {
              message: 'serial session should report the missing device',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'off');

          helper = await startSerialPtyHelper();
          await symlink(helper.slavePath, serialDevicePath);
          await pressKeyUntilReceived(app, helper, 'a', '61');
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
        });
      } finally {
        await helper?.close();
      }
    });
  });

  it('renders the initially disconnected serial notice above terminal dimming', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      const serialDevicePath = join(directory, 'ttyELDERTERMS0');
      await writeFile(
        configPath,
        `[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=${serialDevicePath}\nbaudrate=9600\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n`,
        'utf8'
      );

      await runGtkTest(context, ['-c', configPath], async (app, evidence) => {
        await toPass(
          async () => {
            expect((await app.output()).stderr).toContain(
              'serial device not found'
            );
          },
          {
            message: 'serial session should report the missing device',
            timeoutMs: 5_000,
          }
        );
        await waitForActivityIndicatorImageState(app, 'conn', 'off');
        await expectDisconnectedNoticeRenderedUndimmedAtTerminalTopRight(
          app,
          evidence
        );
      });
    });
  });

  it('reconnects after a serial device is lost and restored with auto_close disabled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      const serialDevicePath = join(directory, 'ttyELDERTERMS0');
      let firstHelper: SerialPtyHelper | undefined;
      let secondHelper: SerialPtyHelper | undefined;

      try {
        firstHelper = await startSerialPtyHelper();
        await symlink(firstHelper.slavePath, serialDevicePath);
        await writeFile(
          configPath,
          `[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=${serialDevicePath}\nbaudrate=9600\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app, evidence) => {
          const activeFirstHelper = firstHelper;
          if (activeFirstHelper === undefined) {
            throw new Error('first serial PTY helper is not running');
          }

          const connectedTitle = `elder-terms: serial: ${serialDevicePath}:9600:n81n`;
          await pressKeyUntilReceived(app, activeFirstHelper, 'a', '61');
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await expectMainWindowTitle(app, connectedTitle);
          await expectDisconnectedNoticeHidden(app);

          await firstHelper?.close();
          await rm(serialDevicePath, { force: true });

          await toPass(
            async () => {
              await app.input.pressKey('x');
              expect((await app.output()).stderr).toMatch(
                /serial (device not found|open failed|read failed|write failed|carrier detection failed)/
              );
            },
            {
              message: 'serial session should notice the lost device',
              timeoutMs: 7_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await expectMainWindowTitle(app, `${connectedTitle} (Disconnected)`);
          await expectDisconnectedNoticeVisibleAtTerminalTopRight(app);

          secondHelper = await startSerialPtyHelper();
          await symlink(secondHelper.slavePath, serialDevicePath);
          await pressKeyUntilReceived(app, secondHelper, 'b', '62');
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await expectMainWindowTitle(app, connectedTitle);
          await expectDisconnectedNoticeHidden(app);
          await evidence.log('serial CONN tracked device lifetime', {
            title: connectedTitle,
          });
        });
      } finally {
        await firstHelper?.close();
        await secondHelper?.close();
      }
    });
  });
});
