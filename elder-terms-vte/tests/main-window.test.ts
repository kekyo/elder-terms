import { execFile } from 'node:child_process';
import { chmod, mkdir, readFile, writeFile } from 'node:fs/promises';
import { createServer, type Server, type Socket } from 'node:net';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { promisify } from 'node:util';
import type { GtkApp, GtkCapture, GtkToggleButtonElement } from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  activityIndicatorIconSize,
  expectActivityIndicatorImageState,
  waitForActivityIndicatorImageState,
} from './activity-indicator-test-helpers';
import { expectElementKind } from './test-helpers';
import {
  defaultColumns,
  defaultRows,
  expectFixtureVteGridSize,
  readTerminalGridLayout,
  runGtkTest,
  withTemporaryDirectory,
} from './gtk-test-helpers';

const execFileAsync = promisify(execFile);
const clipboardReadHelperPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/clipboard-read-helper', import.meta.url)
);

const transferMenuItems = [
  ['transfer_zmodem_send_item', 'ZMODEM (send)'],
  ['transfer_ymodem_send_item', 'YMODEM (send)'],
  ['transfer_xmodem_1k_send_item', 'XMODEM 1K (send)'],
  ['transfer_xmodem_send_item', 'XMODEM (send)'],
  ['transfer_zmodem_receive_item', 'ZMODEM (receive)'],
  ['transfer_ymodem_g_receive_item', 'YMODEM-g (receive)'],
  ['transfer_ymodem_receive_item', 'YMODEM (receive)'],
  ['transfer_xmodem_crc_receive_item', 'XMODEM CRC (receive)'],
  ['transfer_xmodem_receive_item', 'XMODEM (receive)'],
] as const;

const transferDialogProbePrefix =
  'ELDER_TERMS_TRANSFER_DIALOG_CURRENT_FOLDER_URI=';

const shellQuote = (value: string): string =>
  `'${value.split("'").join("'\\''")}'`;

const listenOnLocalhost = async (server: Server): Promise<number> =>
  new Promise<number>((resolve, reject) => {
    const rejectFromError = (error: Error): void => {
      reject(error);
    };
    server.once('error', rejectFromError);
    server.listen(0, '127.0.0.1', () => {
      server.off('error', rejectFromError);
      const address = server.address();
      if (address === null || typeof address === 'string') {
        reject(new Error('Server did not expose a TCP port.'));
        return;
      }
      resolve(address.port);
    });
  });

const closeServer = async (server: Server): Promise<void> =>
  new Promise<void>((resolve, reject) => {
    if (!server.listening) {
      resolve();
      return;
    }
    server.close((error) => {
      if (error === undefined) {
        resolve();
      } else {
        reject(error);
      }
    });
  });

const writeXdgDownloadDirectory = async (
  directory: string
): Promise<{
  readonly configHome: string;
  readonly downloads: string;
  readonly home: string;
}> => {
  const home = join(directory, 'home');
  const configHome = join(directory, 'xdg-config');
  const downloads = join(home, 'XDG Downloads');
  await mkdir(downloads, { recursive: true });
  await mkdir(configHome, { recursive: true });
  await writeFile(
    join(configHome, 'user-dirs.dirs'),
    'XDG_DOWNLOAD_DIR="$HOME/XDG Downloads"\n',
    'utf8'
  );
  return { configHome, downloads, home };
};

const expectTransferButtonHidden = async (app: GtkApp): Promise<void> => {
  const transferButton = await app.getById('transfer_button');
  const info = await transferButton.info();
  expect(info.states).not.toContain('showing');
  expect(info.states).not.toContain('visible');
};

const expectTransferButtonVisibleLeftOfSettings = async (
  app: GtkApp
): Promise<GtkToggleButtonElement> => {
  const transferButton = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  const settingsButton = expectElementKind(
    await app.getById('settings_button'),
    'button'
  );
  await waitForResult(
    async () => {
      const info = await transferButton.info();
      expect(info.states).toContain('showing');
      expect(info.states).toContain('visible');
      return info;
    },
    {
      message: 'transfer button should be visible',
      timeoutMs: 5_000,
    }
  );

  const transferCapture = await transferButton.capture();
  const settingsCapture = await settingsButton.capture();
  expect(transferCapture.bounds.x).toBeLessThan(settingsCapture.bounds.x);
  return transferButton;
};

const expectTransferButtonSensitive = async (
  button: GtkToggleButtonElement
): Promise<void> => {
  await waitForResult(
    async () => {
      const info = await button.info();
      expect(info.states).toContain('enabled');
      expect(info.states).toContain('sensitive');
      return info;
    },
    {
      message: 'transfer button should be sensitive',
      timeoutMs: 5_000,
    }
  );
};

const expectTransferButtonInsensitive = async (
  button: GtkToggleButtonElement
): Promise<void> => {
  await waitForResult(
    async () => {
      const info = await button.info();
      expect(info.states).not.toContain('enabled');
      expect(info.states).not.toContain('sensitive');
      return info;
    },
    {
      message: 'transfer button should be insensitive',
      timeoutMs: 5_000,
    }
  );
};

const openZmodemSendDialog = async (app: GtkApp): Promise<void> => {
  const transferButton = await expectTransferButtonVisibleLeftOfSettings(app);
  await expectTransferButtonSensitive(transferButton);
  await transferButton.click();

  const item = await waitForResult(
    async () => {
      const menuItem = expectElementKind(
        await app.getById('transfer_zmodem_send_item'),
        'menuItem'
      );
      expect((await menuItem.info()).states).toContain('showing');
      return menuItem;
    },
    {
      message: 'ZMODEM send menu item should be visible',
      timeoutMs: 5_000,
    }
  );
  await item.click();
};

const waitForTransferDialogFolderUri = async (
  app: GtkApp,
  expectedUri: string
): Promise<string> =>
  waitForResult(
    async () => {
      const output = await app.output();
      const line = output.stdout
        .split(/\r?\n/u)
        .find((candidate) => candidate.startsWith(transferDialogProbePrefix));
      const expectedLine = `${transferDialogProbePrefix}${expectedUri}`;
      if (line !== expectedLine) {
        throw new Error(
          `Expected transfer dialog folder probe [${expectedLine}], actual [${line ?? ''}]`
        );
      }
      return line;
    },
    {
      message: `transfer dialog should open at ${expectedUri}`,
      timeoutMs: 5_000,
    }
  );

const openTerminalContextMenu = async (
  app: GtkApp,
  x: number,
  y: number
): Promise<void> => {
  await app.input.moveMouseTo(Math.trunc(x), Math.trunc(y));
  await app.input.setMouseButton('right', true);
  await app.input.setMouseButton('right', false);
};

const selectTerminalCells = async (
  app: GtkApp,
  bounds: GtkCapture['bounds'],
  cellCount: number,
  row: number
): Promise<void> => {
  const cellWidth = bounds.width / defaultColumns;
  const cellHeight = bounds.height / defaultRows;
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + cellWidth / 4),
    Math.trunc(bounds.y + cellHeight * (row + 0.5))
  );
  await app.input.setMouseButton('left', true);
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + cellWidth * (cellCount - 0.25)),
    Math.trunc(bounds.y + cellHeight * (row + 0.5))
  );
  await app.input.setMouseButton('left', false);
};

const readClipboardText = async (app: GtkApp): Promise<string> => {
  const result = await execFileAsync(clipboardReadHelperPath, [], {
    encoding: 'utf8',
    env: await app.environment(),
  });
  return result.stdout.toString();
};

describe.concurrent('elder-terms-vte main window', () => {
  it('shows a terminal layout constrained to whole VTE cells', async (context) => {
    await runGtkTest(context, [], async (app, evidence) => {
      expect(await app.getWindowCount()).toBe(1);

      const mainWindow = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      await app.getById('header_bar');
      await app.getById('root_box');
      const terminalScroller = await app.getById('terminal_scroller');
      const terminal = await app.getById('terminal_view');
      const terminalScrollbar = await app.getById('terminal_scrollbar');
      const statusBar = await app.getById('status_bar');
      const statusLabel = expectElementKind(
        await app.getById('status_label'),
        'label'
      );
      const activityIndicatorBar = await app.getById('activity_indicator_bar');
      const connIndicatorBox = await app.getById('conn_indicator_box');
      const logIndicatorBox = await app.getById('log_indicator_box');
      const sdIndicatorBox = await app.getById('sd_indicator_box');
      const rdIndicatorBox = await app.getById('rd_indicator_box');
      const connIndicatorImage = await app.getById('conn_indicator_image');
      const logIndicatorImage = await app.getById('log_indicator_image');
      const sdIndicatorImage = await app.getById('sd_indicator_image');
      const rdIndicatorImage = await app.getById('rd_indicator_image');
      const connIndicatorLabel = expectElementKind(
        await app.getById('conn_indicator_label'),
        'label'
      );
      const logIndicatorLabel = expectElementKind(
        await app.getById('log_indicator_label'),
        'label'
      );
      const sdIndicatorLabel = expectElementKind(
        await app.getById('sd_indicator_label'),
        'label'
      );
      const rdIndicatorLabel = expectElementKind(
        await app.getById('rd_indicator_label'),
        'label'
      );

      await expectTransferButtonHidden(app);
      expect(await statusLabel.text()).toBe('Terminal');
      expect(await connIndicatorLabel.text()).toBe('CONN');
      expect(await logIndicatorLabel.text()).toBe('LOG');
      expect(await sdIndicatorLabel.text()).toBe('SD');
      expect(await rdIndicatorLabel.text()).toBe('RD');

      const [
        mainBounds,
        terminalScrollerCapture,
        terminalCapture,
        terminalScrollbarCapture,
        statusBarCapture,
        statusLabelCapture,
        activityIndicatorBarCapture,
        connIndicatorBoxCapture,
        logIndicatorBoxCapture,
        sdIndicatorBoxCapture,
        rdIndicatorBoxCapture,
        connIndicatorImageCapture,
        logIndicatorImageCapture,
        sdIndicatorImageCapture,
        rdIndicatorImageCapture,
        connIndicatorLabelCapture,
        logIndicatorLabelCapture,
        sdIndicatorLabelCapture,
        rdIndicatorLabelCapture,
        hints,
      ] = await Promise.all([
        mainWindow.bounds(),
        evidence.captureEvidence('terminal-scroller', async () =>
          terminalScroller.capture()
        ),
        evidence.captureEvidence('terminal', async () => terminal.capture()),
        evidence.captureEvidence('terminal-scrollbar', async () =>
          terminalScrollbar.capture()
        ),
        evidence.captureEvidence('status-bar', async () => statusBar.capture()),
        evidence.captureEvidence('status-label', async () =>
          statusLabel.capture()
        ),
        evidence.captureEvidence('activity-indicator-bar', async () =>
          activityIndicatorBar.capture()
        ),
        evidence.captureEvidence('conn-indicator-box', async () =>
          connIndicatorBox.capture()
        ),
        evidence.captureEvidence('log-indicator-box', async () =>
          logIndicatorBox.capture()
        ),
        evidence.captureEvidence('sd-indicator-box', async () =>
          sdIndicatorBox.capture()
        ),
        evidence.captureEvidence('rd-indicator-box', async () =>
          rdIndicatorBox.capture()
        ),
        evidence.captureEvidence('conn-indicator-image', async () =>
          connIndicatorImage.capture()
        ),
        evidence.captureEvidence('log-indicator-image', async () =>
          logIndicatorImage.capture()
        ),
        evidence.captureEvidence('sd-indicator-image', async () =>
          sdIndicatorImage.capture()
        ),
        evidence.captureEvidence('rd-indicator-image', async () =>
          rdIndicatorImage.capture()
        ),
        evidence.captureEvidence('conn-indicator-label', async () =>
          connIndicatorLabel.capture()
        ),
        evidence.captureEvidence('log-indicator-label', async () =>
          logIndicatorLabel.capture()
        ),
        evidence.captureEvidence('sd-indicator-label', async () =>
          sdIndicatorLabel.capture()
        ),
        evidence.captureEvidence('rd-indicator-label', async () =>
          rdIndicatorLabel.capture()
        ),
        mainWindow.resizeHints(),
      ]);

      expect(terminalScrollerCapture.bounds.y).toBeLessThan(
        statusBarCapture.bounds.y
      );
      expect(terminalCapture.bounds.y).toBeLessThan(statusBarCapture.bounds.y);
      expect(terminalCapture.bounds.y + terminalCapture.bounds.height).toBe(
        statusBarCapture.bounds.y
      );
      expect(terminalCapture.bounds.x).toBe(terminalScrollerCapture.bounds.x);
      expect(terminalCapture.bounds.y).toBe(terminalScrollerCapture.bounds.y);
      expect(terminalScrollbarCapture.bounds.y).toBe(terminalCapture.bounds.y);
      expect(terminalScrollbarCapture.bounds.height).toBe(
        terminalCapture.bounds.height
      );
      expect(terminalCapture.bounds.x + terminalCapture.bounds.width).toBe(
        terminalScrollbarCapture.bounds.x
      );
      expect(
        terminalScrollbarCapture.bounds.x +
          terminalScrollbarCapture.bounds.width
      ).toBe(
        terminalScrollerCapture.bounds.x + terminalScrollerCapture.bounds.width
      );
      expect((mainBounds.width - hints.baseWidth) % hints.widthIncrement).toBe(
        0
      );
      expect(
        (mainBounds.height - hints.baseHeight) % hints.heightIncrement
      ).toBe(0);
      expect((mainBounds.width - hints.baseWidth) / hints.widthIncrement).toBe(
        defaultColumns
      );
      expect(
        (mainBounds.height - hints.baseHeight) / hints.heightIncrement
      ).toBe(defaultRows);
      expect(statusBarCapture.bounds.y).toBeGreaterThan(mainBounds.y);
      expect(
        statusBarCapture.bounds.y + statusBarCapture.bounds.height
      ).toBeLessThanOrEqual(mainBounds.y + mainBounds.height);

      expect(statusLabelCapture.bounds.x).toBeGreaterThanOrEqual(
        statusBarCapture.bounds.x
      );
      expect(
        statusLabelCapture.bounds.x - statusBarCapture.bounds.x
      ).toBeLessThanOrEqual(16);
      expect(statusLabelCapture.bounds.y).toBeGreaterThanOrEqual(
        statusBarCapture.bounds.y
      );
      expect(
        statusLabelCapture.bounds.y + statusLabelCapture.bounds.height
      ).toBeLessThanOrEqual(
        statusBarCapture.bounds.y + statusBarCapture.bounds.height
      );
      expect(
        statusLabelCapture.bounds.x + statusLabelCapture.bounds.width
      ).toBeLessThanOrEqual(activityIndicatorBarCapture.bounds.x);
      expect(activityIndicatorBarCapture.bounds.x).toBeGreaterThan(
        statusLabelCapture.bounds.x
      );
      expect(
        activityIndicatorBarCapture.bounds.x +
          activityIndicatorBarCapture.bounds.width
      ).toBeLessThanOrEqual(
        statusBarCapture.bounds.x + statusBarCapture.bounds.width
      );
      expect(
        statusBarCapture.bounds.x +
          statusBarCapture.bounds.width -
          (activityIndicatorBarCapture.bounds.x +
            activityIndicatorBarCapture.bounds.width)
      ).toBeLessThanOrEqual(8);
      expect(connIndicatorBoxCapture.bounds.x).toBeLessThan(
        logIndicatorBoxCapture.bounds.x
      );
      expect(logIndicatorBoxCapture.bounds.x).toBeLessThan(
        sdIndicatorBoxCapture.bounds.x
      );
      expect(sdIndicatorBoxCapture.bounds.x).toBeLessThan(
        rdIndicatorBoxCapture.bounds.x
      );
      expect(
        connIndicatorBoxCapture.bounds.x + connIndicatorBoxCapture.bounds.width
      ).toBeLessThanOrEqual(logIndicatorBoxCapture.bounds.x);
      expect(
        logIndicatorBoxCapture.bounds.x + logIndicatorBoxCapture.bounds.width
      ).toBeLessThanOrEqual(sdIndicatorBoxCapture.bounds.x);
      expect(
        sdIndicatorBoxCapture.bounds.x + sdIndicatorBoxCapture.bounds.width
      ).toBeLessThanOrEqual(rdIndicatorBoxCapture.bounds.x);
      expect(connIndicatorImageCapture.bounds.width).toBe(
        activityIndicatorIconSize
      );
      expect(connIndicatorImageCapture.bounds.height).toBe(
        activityIndicatorIconSize
      );
      expect(logIndicatorImageCapture.bounds.width).toBe(
        activityIndicatorIconSize
      );
      expect(logIndicatorImageCapture.bounds.height).toBe(
        activityIndicatorIconSize
      );
      expect(sdIndicatorImageCapture.bounds.width).toBe(
        activityIndicatorIconSize
      );
      expect(sdIndicatorImageCapture.bounds.height).toBe(
        activityIndicatorIconSize
      );
      expect(rdIndicatorImageCapture.bounds.width).toBe(
        activityIndicatorIconSize
      );
      expect(rdIndicatorImageCapture.bounds.height).toBe(
        activityIndicatorIconSize
      );
      expect(connIndicatorImageCapture.bounds.y).toBeLessThan(
        connIndicatorLabelCapture.bounds.y
      );
      expect(logIndicatorImageCapture.bounds.y).toBeLessThan(
        logIndicatorLabelCapture.bounds.y
      );
      expect(sdIndicatorImageCapture.bounds.y).toBeLessThan(
        sdIndicatorLabelCapture.bounds.y
      );
      expect(rdIndicatorImageCapture.bounds.y).toBeLessThan(
        rdIndicatorLabelCapture.bounds.y
      );
      await expectActivityIndicatorImageState(connIndicatorImageCapture, 'on');
      await expectActivityIndicatorImageState(logIndicatorImageCapture, 'off');
      await expectActivityIndicatorImageState(sdIndicatorImageCapture, 'off');
      await expectActivityIndicatorImageState(rdIndicatorImageCapture, 'off');
      await evidence.log('terminal layout verified', {
        hints,
        mainBounds,
      });
    });
  });

  it('shows the transfer button for serial sessions', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      await writeFile(
        configPath,
        '[general]\ntype=serial\n\n[terminal]\nauto_close=false\n\n[serial]\ndevice=/tmp/elder-terms-missing-serial\nbaudrate=115200\n',
        'utf8'
      );

      await runGtkTest(context, ['-c', configPath], async (app) => {
        await expectTransferButtonVisibleLeftOfSettings(app);
      });
    });
  });

  it('copies selected terminal text from the right-click menu', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      const layout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      await waitForResult(async () =>
        expectFixtureVteGridSize(app, defaultColumns, defaultRows)
      );
      const { bounds } = layout.terminalCapture;
      const cellWidth = bounds.width / defaultColumns;
      const cellHeight = bounds.height / defaultRows;

      await openTerminalContextMenu(
        app,
        bounds.x + cellWidth / 2,
        bounds.y + cellHeight / 2
      );
      const copyItem = await waitForResult(async () => {
        const item = expectElementKind(
          await app.getById('terminal_context_copy_item'),
          'menuItem'
        );
        const info = await item.info();
        expect(info.name).toBe('Copy');
        expect(info.states).toContain('showing');
        expect(info.states).not.toContain('enabled');
        expect(info.states).not.toContain('sensitive');
        return item;
      });

      await app.input.pressKey('Escape');
      await waitForResult(async () => {
        expect((await copyItem.info()).states).not.toContain('showing');
      });
      await selectTerminalCells(app, bounds, 5, 0);

      await openTerminalContextMenu(
        app,
        bounds.x + cellWidth * 6.5,
        bounds.y + cellHeight / 2
      );
      await waitForResult(async () => {
        const info = await copyItem.info();
        expect(info.states).toContain('showing');
        expect(info.states).toContain('enabled');
        expect(info.states).toContain('sensitive');
      });
      await copyItem.click();

      expect(await readClipboardText(app)).toBe('!\"#$%');
    });
  });

  it('copies selected terminal text while the VTE is read-only', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const readyPath = join(directory, 'read-only-shell-ready.txt');
      const markerPath = join(directory, 'read-only-shell-exited.txt');
      const releasePath = join(directory, 'read-only-shell-release');
      const shellPath = join(directory, 'read-only-shell.sh');
      const configPath = join(directory, 'read-only-terminal.ini');
      await execFileAsync('mkfifo', [releasePath]);
      await writeFile(
        shellPath,
        `#!/bin/sh\nline=0\nwhile [ "$line" -lt 40 ]; do\n  printf 'SCROLL_LINE_%02d\\n' "$line"\n  line=$((line + 1))\ndone\nprintf READ_ONLY_COPY\nprintf ready > ${shellQuote(readyPath)}\nIFS= read -r release < ${shellQuote(releasePath)}\nprintf exited > ${shellQuote(markerPath)}\nexit 0\n`,
        'utf8'
      );
      await chmod(shellPath, 0o755);
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app) => {
          await waitForResult(async () => {
            expect(await readFile(readyPath, 'utf8')).toBe('ready');
          });
          await waitForActivityIndicatorImageState(app, 'conn', 'on');

          const terminalScrollbar = expectElementKind(
            await app.getById('terminal_scrollbar'),
            'scrollbar'
          );
          await waitForResult(async () => {
            expect(await terminalScrollbar.value()).toBeGreaterThan(0);
          });
          const layout = await waitForResult(async () =>
            readTerminalGridLayout(app)
          );
          const { bounds } = layout.terminalCapture;

          await writeFile(releasePath, 'exit\n', 'utf8');
          await waitForResult(async () => {
            expect(await readFile(markerPath, 'utf8')).toBe('exited');
          });
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await waitForResult(async () => {
            expect(
              (await (await app.getById('disconnected_notice')).info()).states
            ).toContain('showing');
          });

          await selectTerminalCells(
            app,
            bounds,
            'READ_ONLY_COPY'.length,
            defaultRows - 1
          );
          await openTerminalContextMenu(
            app,
            bounds.x + (bounds.width / defaultColumns) * 15.5,
            bounds.y + (bounds.height / defaultRows) * (defaultRows - 0.5)
          );
          const copyItem = await waitForResult(async () => {
            const item = expectElementKind(
              await app.getById('terminal_context_copy_item'),
              'menuItem'
            );
            const info = await item.info();
            expect(info.states).toContain('showing');
            expect(info.states).toContain('enabled');
            expect(info.states).toContain('sensitive');
            return item;
          });
          await copyItem.click();
          expect(await readClipboardText(app)).toBe('READ_ONLY_COPY');

          const initialScrollValue = await terminalScrollbar.value();
          await app.input.moveMouseTo(
            Math.trunc(bounds.x + bounds.width / 2),
            Math.trunc(bounds.y + bounds.height / 2)
          );
          await app.input.scrollWheel(0, -5);
          await waitForResult(async () => {
            const scrollValue = await terminalScrollbar.value();
            expect(scrollValue).toBeLessThan(initialScrollValue);
            return scrollValue;
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

  it('shows the transfer menu for TELNET sessions', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.write('connected\r\n');
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        await writeFile(
          configPath,
          `[general]\ntype=telnet\n\n[terminal]\nauto_close=false\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await waitForResult(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
              return acceptedSocket;
            },
            {
              message: 'TELNET server should accept a client connection',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'on');

          const transferButton =
            await expectTransferButtonVisibleLeftOfSettings(app);
          await expectTransferButtonSensitive(transferButton);
          await transferButton.click();

          const menuCaptures: GtkCapture[] = [];
          for (const [id, label] of transferMenuItems) {
            const item = await waitForResult(
              async () => {
                const item = expectElementKind(
                  await app.getById(id),
                  'menuItem'
                );
                const info = await item.info();
                expect(info.name).toBe(label);
                expect(info.states).toContain('showing');
                expect(info.states).toContain('visible');
                return item;
              },
              {
                message: `transfer menu item should be visible: ${label}`,
                timeoutMs: 5_000,
              }
            );
            menuCaptures.push(await item.capture());
          }

          for (let index = 1; index < menuCaptures.length; index += 1) {
            expect(menuCaptures[index].bounds.y).toBeGreaterThan(
              menuCaptures[index - 1].bounds.y
            );
          }
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('disables the transfer menu button after TELNET disconnects', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      let acceptedSocketClosed = false;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.resume();
        socket.on('close', () => {
          acceptedSocketClosed = true;
        });
        socket.write('connected\r\n');
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        await writeFile(
          configPath,
          `[general]\ntype=telnet\n\n[terminal]\nauto_close=false\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await waitForResult(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
              return acceptedSocket;
            },
            {
              message: 'TELNET server should accept a client connection',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'on');

          const transferButton =
            await expectTransferButtonVisibleLeftOfSettings(app);
          await expectTransferButtonSensitive(transferButton);

          acceptedSocket?.end();
          await waitForResult(
            async () => {
              expect(acceptedSocketClosed).toBe(true);
            },
            {
              message: 'TELNET server socket should close',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await expectTransferButtonInsensitive(transferButton);

          await expect(transferButton.click()).rejects.toThrow();
          const item = expectElementKind(
            await app.getById('transfer_zmodem_send_item'),
            'menuItem'
          );
          expect((await item.info()).states).not.toContain('showing');
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('opens the ZMODEM send dialog at the XDG Downloads directory', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.write('connected\r\n');
      });

      try {
        const xdg = await writeXdgDownloadDirectory(directory);
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        await writeFile(
          configPath,
          `[general]\ntype=telnet\n\n[terminal]\nauto_close=false\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
          'utf8'
        );

        await runGtkTest(
          context,
          ['--test-transfer-dialog-probe', '-c', configPath],
          async (app, evidence) => {
            await waitForResult(
              async () => {
                expect(acceptedSocket).not.toBeUndefined();
                return acceptedSocket;
              },
              {
                message: 'TELNET server should accept a client connection',
                timeoutMs: 5_000,
              }
            );
            await waitForActivityIndicatorImageState(app, 'conn', 'on');

            await openZmodemSendDialog(app);
            await waitForTransferDialogFolderUri(
              app,
              pathToFileURL(xdg.downloads).href
            );

            const dialog = await app.windowAt(1);
            if (dialog !== undefined) {
              await evidence.captureEvidence(
                'zmodem-send-dialog-xdg-downloads',
                async () => dialog.capture()
              );
            }
            await app.input.pressKey('Escape');
          },
          {
            env: {
              HOME: xdg.home,
              XDG_CONFIG_HOME: xdg.configHome,
            },
          }
        );
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('opens the ZMODEM send dialog at the configured transfer base path', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.write('connected\r\n');
      });

      try {
        const xdg = await writeXdgDownloadDirectory(directory);
        const basePath = join(directory, 'transfer-base');
        await mkdir(basePath, { recursive: true });
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        await writeFile(
          configPath,
          `[general]\ntype=telnet\n\n[terminal]\nauto_close=false\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n\n[transfer]\nbase_path=${basePath}\n`,
          'utf8'
        );

        await runGtkTest(
          context,
          ['--test-transfer-dialog-probe', '-c', configPath],
          async (app, evidence) => {
            await waitForResult(
              async () => {
                expect(acceptedSocket).not.toBeUndefined();
                return acceptedSocket;
              },
              {
                message: 'TELNET server should accept a client connection',
                timeoutMs: 5_000,
              }
            );
            await waitForActivityIndicatorImageState(app, 'conn', 'on');

            await openZmodemSendDialog(app);
            await waitForTransferDialogFolderUri(
              app,
              pathToFileURL(basePath).href
            );

            const dialog = await app.windowAt(1);
            if (dialog !== undefined) {
              await evidence.captureEvidence(
                'zmodem-send-dialog-transfer-base-path',
                async () => dialog.capture()
              );
            }
            await app.input.pressKey('Escape');
          },
          {
            env: {
              HOME: xdg.home,
              XDG_CONFIG_HOME: xdg.configHome,
            },
          }
        );
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('exits when the main window is closed', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
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
          message: 'app should exit after closing the main window',
          timeoutMs: 5_000,
        }
      );
      await evidence.log('main window close exited app', {
        exitCode: output.exitCode,
        exitSignal: output.exitSignal,
      });
      expect(output.stderr).not.toContain('Gtk-CRITICAL');
      expect(output.stderr).not.toContain('GLib-GObject-CRITICAL');
      expect(output.stderr).not.toContain('gtk_widget_get_visible');
      expect(output.stderr).not.toContain('gtk_image_set_from_pixbuf');
    });
  });
});
