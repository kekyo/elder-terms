import { execFile, spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { chmod, mkdir, readFile, writeFile } from 'node:fs/promises';
import { createServer, type Server, type Socket } from 'node:net';
import { join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { promisify } from 'node:util';
import type {
  GtkApp,
  GtkCapture,
  GtkToggleButtonElement,
  GtkWidgetElement,
} from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  activityIndicatorIconSize,
  expectActivityIndicatorImageState,
  waitForActivityIndicatorImageState,
} from './activity-indicator-test-helpers';
import { startClipboardTextProvider } from './clipboard-test-helpers';
import {
  activateTextSend,
  expectTextSendActive,
} from './text-send-test-helpers';
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
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const clipboardReadHelperPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/clipboard-read-helper', import.meta.url)
);
const statusBarHorizontalPadding = 8;
const statusBarVerticalPadding = 2;

const transferMenuItems = [
  ['transfer_log_enabled_item', 'Log recording'],
  ['transfer_text_send_item', 'Text (Send)'],
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

const expectTransferButtonVisibleLeftOfApplicationMenu = async (
  app: GtkApp
): Promise<GtkToggleButtonElement> => {
  const transferButton = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  const applicationMenuButton = expectElementKind(
    await app.getById('application_menu_button'),
    'toggleButton'
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
  const applicationMenuCapture = await applicationMenuButton.capture();
  expect(transferCapture.bounds.x).toBeLessThan(
    applicationMenuCapture.bounds.x
  );
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
  const transferButton =
    await expectTransferButtonVisibleLeftOfApplicationMenu(app);
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

const clickWidget = async (
  app: GtkApp,
  widget: GtkWidgetElement
): Promise<void> => {
  const { bounds } = await widget.capture();
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + bounds.width / 2),
    Math.trunc(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
};

describe.concurrent('elder-terms-vte main window', () => {
  it('opens runtime settings from the first application menu item and requests the launcher-owned About page', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const launcher = join(directory, 'elder-terms-launcher');
      const capture = join(directory, 'application-dialog-requests.txt');
      await writeFile(
        launcher,
        '#!/bin/sh\nset -eu\nprintf \'%s\\n\' "$@" >>"$ELDER_TERMS_APPLICATION_REQUEST_CAPTURE"\n'
      );
      await chmod(launcher, 0o755);

      await runGtkTest(
        context,
        ['--test-fixture'],
        async (app) => {
          await expect(app.getById('settings_button')).rejects.toThrow();
          await expect(
            app.getById('application_settings_menu_item')
          ).rejects.toThrow();

          const menu = expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          );
          await clickWidget(app, menu);
          const settingsItem = expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          );
          const aboutItem = expectElementKind(
            await app.getById('about_menu_item'),
            'menuItem'
          );
          const settingsInfo = await settingsItem.info();
          const aboutInfo = await aboutItem.info();
          expect(settingsInfo.name).toBe('Settings');
          expect(settingsInfo.states).toContain('showing');
          expect(aboutInfo.name).toBe('About elder-terms');
          expect(aboutInfo.states).toContain('showing');
          expect((await settingsItem.capture()).bounds.y).toBeLessThan(
            (await aboutItem.capture()).bounds.y
          );

          await settingsItem.click();
          await waitForResult(async () => {
            expectElementKind(await app.getById('settings_dialog'), 'window');
            expectElementKind(
              await app.getById('settings_widget_root'),
              'container'
            );
          });
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();

          await clickWidget(app, menu);
          await aboutItem.click();
          await waitForResult(async () => {
            expect(await readFile(capture, 'utf8')).toBe('--about\n');
          });
        },
        {
          env: {
            ELDER_TERMS_APPLICATION_REQUEST_CAPTURE: capture,
            ELDER_TERMS_LAUNCHER_PATH: launcher,
          },
        }
      );
    });
  });

  it('publishes a runtime icon and desktop identity for its main window', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      const mainWindow = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      const x11 = await mainWindow.x11Info();
      expect(x11.instanceName).toBe('net.kekyo.elder-terms-vte');
      expect(x11.className).toBe('Elder-terms-vte');
      const result = await execFileAsync(
        'xprop',
        ['-id', x11.windowId, '-len', '16', '32c', '_NET_WM_ICON'],
        {
          env: {
            ...process.env,
            ...(await app.environment()),
          },
        }
      );
      expect(result.stdout.toString()).toMatch(
        /_NET_WM_ICON\(CARDINAL\) = [1-9]\d*, [1-9]\d*/u
      );
    });
  });

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

      await expectTransferButtonVisibleLeftOfApplicationMenu(app);
      expect(await statusLabel.text()).toBe('local terminal');
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

      expect(statusLabelCapture.bounds.x - statusBarCapture.bounds.x).toBe(
        statusBarHorizontalPadding
      );
      expect(statusLabelCapture.bounds.y - statusBarCapture.bounds.y).toBe(
        statusBarVerticalPadding
      );
      expect(
        statusBarCapture.bounds.y +
          statusBarCapture.bounds.height -
          (statusLabelCapture.bounds.y + statusLabelCapture.bounds.height)
      ).toBe(statusBarVerticalPadding);
      expect(
        activityIndicatorBarCapture.bounds.y - statusBarCapture.bounds.y
      ).toBe(statusBarVerticalPadding);
      expect(
        statusBarCapture.bounds.y +
          statusBarCapture.bounds.height -
          (activityIndicatorBarCapture.bounds.y +
            activityIndicatorBarCapture.bounds.height)
      ).toBe(statusBarVerticalPadding);
      expect(
        statusBarCapture.bounds.x +
          statusBarCapture.bounds.width -
          (activityIndicatorBarCapture.bounds.x +
            activityIndicatorBarCapture.bounds.width)
      ).toBe(statusBarHorizontalPadding);
      expect(
        statusLabelCapture.bounds.x + statusLabelCapture.bounds.width
      ).toBeLessThanOrEqual(activityIndicatorBarCapture.bounds.x);
      expect(activityIndicatorBarCapture.bounds.x).toBeGreaterThan(
        statusLabelCapture.bounds.x
      );
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

  it('moves the window when the VTE status bar is dragged', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      const mainWindow = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      const initialBounds = await mainWindow.moveTo(100, 100);
      const statusCapture = await (await app.getById('status_bar')).capture();
      const startX = Math.trunc(
        statusCapture.bounds.x + statusCapture.bounds.width / 2
      );
      const startY = Math.trunc(
        statusCapture.bounds.y + statusCapture.bounds.height / 2
      );

      await app.input.moveMouseTo(startX, startY);
      await app.input.setMouseButton('left', true);
      await app.input.moveMouseTo(startX + 120, startY + 80);
      await app.input.setMouseButton('left', false);

      await waitForResult(
        async () => {
          const movedBounds = await mainWindow.bounds();
          expect(movedBounds.x).toBeGreaterThanOrEqual(initialBounds.x + 80);
          expect(movedBounds.y).toBeGreaterThanOrEqual(initialBounds.y + 40);
          expect(movedBounds.width).toBe(initialBounds.width);
          expect(movedBounds.height).toBe(initialBounds.height);
        },
        {
          message: 'status bar drag should move the VTE window',
          timeoutMs: 5_000,
        }
      );
    });
  });

  it('shows the transfer button for serial sessions', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      await writeFile(
        configPath,
        '[general]\nauto_close=false\ntype=serial\n\n[terminal]\n\n[serial]\ndevice=/tmp/elder-terms-missing-serial\nbaudrate=115200\n',
        'utf8'
      );

      await runGtkTest(context, ['-c', configPath], async (app) => {
        await expectTransferButtonVisibleLeftOfApplicationMenu(app);
      });
    });
  });

  it('enables Paste only when the clipboard provides text', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      const layout = await waitForResult(async () =>
        readTerminalGridLayout(app)
      );
      const { bounds } = layout.terminalCapture;

      await openTerminalContextMenu(
        app,
        bounds.x + bounds.width / 2,
        bounds.y + bounds.height / 2
      );
      const pasteItem = await waitForResult(async () => {
        const item = expectElementKind(
          await app.getById('terminal_context_paste_item'),
          'menuItem'
        );
        const info = await item.info();
        expect(info.name).toBe('Paste');
        expect(info.states).toContain('showing');
        expect(info.states).not.toContain('enabled');
        expect(info.states).not.toContain('sensitive');
        return item;
      });
      const breakItem = await waitForResult(async () => {
        const item = expectElementKind(
          await app.getById('terminal_context_break_item'),
          'menuItem'
        );
        const info = await item.info();
        expect(info.name).toBe('Send BREAK');
        expect(info.states).toContain('showing');
        expect(info.states).not.toContain('enabled');
        expect(info.states).not.toContain('sensitive');
        return item;
      });

      await app.input.pressKey('Escape');
      const provider = await startClipboardTextProvider(app, 'clipboard text');
      try {
        await openTerminalContextMenu(
          app,
          bounds.x + bounds.width / 2,
          bounds.y + bounds.height / 2
        );
        await waitForResult(async () => {
          const info = await pasteItem.info();
          expect(info.states).toContain('showing');
          expect(info.states).toContain('enabled');
          expect(info.states).toContain('sensitive');
          const breakInfo = await breakItem.info();
          expect(breakInfo.states).toContain('showing');
          expect(breakInfo.states).not.toContain('enabled');
          expect(breakInfo.states).not.toContain('sensitive');
        });
      } finally {
        await provider.close();
      }
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
      await waitForResult(async () => {
        await selectTerminalCells(app, bounds, 5, 0);
        await openTerminalContextMenu(
          app,
          bounds.x + cellWidth * 6.5,
          bounds.y + cellHeight / 2
        );
        const info = await copyItem.info();
        if (!info.states.includes('enabled')) {
          await app.input.pressKey('Escape');
        }
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
      await writeFile(
        configPath,
        '[general]\nauto_close=false\n\n[terminal]\n',
        'utf8'
      );

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

  it('keeps an SSH negotiation failure reason visible in the terminal overlay', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.resume();
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'ssh-negotiation-failure.ini');
        await writeFile(
          configPath,
          `[general]\nauto_close=true\ntype=ssh\n\n[terminal]\n\n[ssh]\naddress=127.0.0.1\nport=${port}\nusername=negotiation-user\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          const usernamePanel = await app.getById('ssh_prompt_panel');
          await waitForResult(
            async () => {
              expect((await usernamePanel.info()).states).toContain('showing');
            },
            {
              message: 'SSH user name prompt should be visible',
              timeoutMs: 5_000,
            }
          );
          expect(
            await expectElementKind(
              await app.getById('ssh_prompt_entry'),
              'entry'
            ).text()
          ).toBe('negotiation-user');
          await expectElementKind(
            await app.getById('ssh_prompt_accept_button'),
            'button'
          ).click();

          const socket = await waitForResult(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
              return acceptedSocket as Socket;
            },
            {
              message: 'SSH test server should accept a client connection',
              timeoutMs: 5_000,
            }
          );
          socket.end('not-an-ssh-server\r\n');

          const notice = await app.getById('disconnected_notice');
          const label = expectElementKind(
            await app.getById('disconnected_notice_label'),
            'label'
          );
          await waitForResult(
            async () => {
              expect(await app.getWindowCount()).toBe(1);
              expect((await notice.info()).states).toContain('showing');
              expect(await label.text()).toMatch(
                /^SSH connection failed:\nFailed to establish SSH transport:/
              );
            },
            {
              message:
                'SSH negotiation failure reason should remain in the overlay',
              timeoutMs: 5_000,
            }
          );
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('reconnects TELNET repeatedly by mouse without losing scrollback or carrying a partial macro line', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const sockets: Socket[] = [];
      const received: string[] = [];
      const server = createServer((socket) => {
        const index = sockets.length;
        sockets.push(socket);
        received.push('');
        socket.on('data', (bytes) => {
          received[index] += bytes.toString('utf8');
        });
        if (index === 0) socket.write('KEEP_SCROLLBACK\r\n');
        socket.write(`ROUND ${index + 1}\r\n`);
      });
      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'reconnect.ini');
        const logPath = join(directory, 'session.log');
        const sourcePath = join(directory, 'interrupted-send.txt');
        await writeFile(sourcePath, 'STALE'.repeat(100), 'utf8');
        await writeFile(
          configPath,
          [
            '[general]',
            'auto_close=false',
            'type=telnet',
            '',
            '[terminal]',
            'return_code=lf',
            '',
            '[telnet]',
            'address=127.0.0.1',
            `port=${port}`,
            '',
            '[log]',
            'enabled=true',
            `base_directory=${directory}`,
            'file_name_format=session.log',
            'mode=cooked',
            '',
            '[transfer]',
            'text_send_bytes_per_second=1',
            '',
            '[macro.reply]',
            'regex=^ROUND (?<round>[0-9]+)$',
            'send=ACK ${round}\\n',
            '',
          ].join('\n'),
          'utf8'
        );

        await runGtkTest(
          context,
          [
            '-c',
            configPath,
            `--test-transfer-source-uri=${pathToFileURL(sourcePath).href}`,
          ],
          async (app, evidence) => {
            const window = expectElementKind(await app.windowAt(0), 'window');
            const windowId = (await window.x11Info()).windowId;
            const screen = await app.capture();
            const screenImage = PNG.sync.read(screen.image);
            const environment = await app.environment();
            const frameSize = screenImage.width * screenImage.height * 4;
            let pendingFrame = Buffer.alloc(0);
            let latestFrame = Buffer.alloc(0);
            let frameNumber = 0;
            let recordingError: Error | undefined;
            let recordingStderr = '';
            // Save lossless video and inspect the same captured frame stream.
            // State transitions below wait for matching frames, not a delay.
            const recorder = spawn(
              'ffmpeg',
              [
                '-hide_banner',
                '-loglevel',
                'error',
                '-f',
                'x11grab',
                '-framerate',
                '30',
                '-draw_mouse',
                '0',
                '-video_size',
                `${screenImage.width}x${screenImage.height}`,
                '-i',
                environment.DISPLAY ?? '',
                '-map',
                '0:v',
                '-c:v',
                'ffv1',
                '-pix_fmt',
                'bgr0',
                '-fps_mode',
                'passthrough',
                join(evidence.directory, 'telnet-reconnect.mkv'),
                '-map',
                '0:v',
                '-c:v',
                'rawvideo',
                '-pix_fmt',
                'bgr0',
                '-fps_mode',
                'passthrough',
                '-f',
                'rawvideo',
                'pipe:1',
              ],
              { env: environment, stdio: ['pipe', 'pipe', 'pipe'] }
            );
            const recordingFinished = new Promise<number | null>((resolve) => {
              recorder.once('error', (error) => {
                recordingError = error;
                resolve(null);
              });
              recorder.once('close', (code) => resolve(code));
            });
            recorder.stderr.on('data', (bytes: Buffer) => {
              recordingStderr += bytes.toString('utf8');
            });
            recorder.stdout.on('data', (bytes: Buffer) => {
              pendingFrame = Buffer.concat([pendingFrame, bytes]);
              while (pendingFrame.length >= frameSize) {
                latestFrame = Buffer.from(pendingFrame.subarray(0, frameSize));
                pendingFrame = pendingFrame.subarray(frameSize);
                frameNumber += 1;
              }
            });
            const videoStates: {
              readonly state: string;
              readonly frame: number;
            }[] = [];
            const videoCaptures: GtkCapture[] = [];
            const expectVideoFrame = async (
              capture: GtkCapture,
              state: string
            ): Promise<void> => {
              const expected = PNG.sync.read(capture.image);
              const firstFrame = frameNumber;
              const frame = await waitForResult(
                async () => {
                  if (recordingError !== undefined) throw recordingError;
                  expect(recorder.exitCode, recordingStderr).toBeNull();
                  expect(frameNumber).toBeGreaterThan(firstFrame);
                  let difference = 0;
                  for (let y = 0; y < expected.height; y += 1) {
                    for (let x = 0; x < expected.width; x += 1) {
                      const actualOffset =
                        ((capture.bounds.y + y) * screenImage.width +
                          capture.bounds.x +
                          x) *
                        4;
                      const expectedOffset = (y * expected.width + x) * 4;
                      difference += Math.abs(
                        latestFrame[actualOffset + 2] -
                          expected.data[expectedOffset]
                      );
                      difference += Math.abs(
                        latestFrame[actualOffset + 1] -
                          expected.data[expectedOffset + 1]
                      );
                      difference += Math.abs(
                        latestFrame[actualOffset] -
                          expected.data[expectedOffset + 2]
                      );
                    }
                  }
                  expect(
                    difference / (expected.width * expected.height * 3)
                  ).toBeLessThan(2);
                  return frameNumber;
                },
                {
                  message: `recorded video should show ${state}`,
                  timeoutMs: 10_000,
                }
              );
              videoStates.push({ state, frame });
              videoCaptures.push(capture);
            };
            try {
              for (let round = 1; round <= 3; round += 1) {
                await waitForResult(async () => {
                  expect(sockets).toHaveLength(round);
                  expect(received[round - 1]).toContain(`ACK ${round}\n`);
                  if (round > 1) expect(received[round - 1]).not.toContain('S');
                });
                await waitForActivityIndicatorImageState(app, 'conn', 'on');
                await waitForResult(async () => {
                  expect(
                    (await (await app.getById('terminal_view')).info()).states
                  ).toContain('focused');
                });
                expect(
                  (await (await app.getById('disconnected_notice')).info())
                    .states
                ).not.toContain('showing');
                expect(
                  (
                    await expectElementKind(
                      await app.windowAt(0),
                      'window'
                    ).x11Info()
                  ).windowId
                ).toBe(windowId);
                await waitForResult(async () => {
                  expect(await readFile(logPath, 'utf8')).toContain(
                    `ROUND ${round}`
                  );
                });
                await expectVideoFrame(
                  (await readTerminalGridLayout(app)).terminalCapture,
                  `connected-${round}`
                );
                await waitForResult(async () => {
                  expect(
                    await expectElementKind(
                      await app.getById('status_label'),
                      'label'
                    ).text()
                  ).toBe(`telnet: 127.0.0.1:${port}`);
                });
                if (round === 3) break;

                if (round === 1) {
                  const transfer = await activateTextSend(app);
                  await expectTextSendActive(transfer);
                  await waitForResult(async () =>
                    expect(received[0]).toContain('S')
                  );
                }

                // Leave an unterminated macro input line in the old connection.
                sockets[round - 1].end('OLD-');
                await waitForActivityIndicatorImageState(app, 'conn', 'off');
                const reconnect = expectElementKind(
                  await app.getById('reconnect_button'),
                  'button'
                );
                await waitForResult(async () => {
                  const info = await reconnect.info();
                  expect(info.states).toContain('showing');
                  expect(info.states).toContain('sensitive');
                });
                await expectVideoFrame(
                  await (await app.getById('disconnected_notice')).capture(),
                  `disconnected-${round}`
                );
                // Native mouse input must reach the button through the overlay.
                await clickWidget(app, reconnect);
                for (let click = 0; click < 4; click += 1) {
                  await app.input.setMouseButton('left', true);
                  await app.input.setMouseButton('left', false);
                }
              }

              const { bounds } = (await readTerminalGridLayout(app))
                .terminalCapture;
              await selectTerminalCells(
                app,
                bounds,
                'KEEP_SCROLLBACK'.length,
                0
              );
              await openTerminalContextMenu(app, bounds.x + 20, bounds.y + 10);
              await expectElementKind(
                await app.getById('terminal_context_copy_item'),
                'menuItem'
              ).click();
              expect(await readClipboardText(app)).toBe('KEEP_SCROLLBACK');
              expect(await app.getWindowCount()).toBe(1);
              expect(videoStates.map(({ state }) => state)).toEqual([
                'connected-1',
                'disconnected-1',
                'connected-2',
                'disconnected-2',
                'connected-3',
              ]);
              for (let index = 1; index < videoStates.length; index += 1) {
                expect(videoStates[index].frame).toBeGreaterThan(
                  videoStates[index - 1].frame
                );
              }
            } finally {
              recorder.stdin.write('q\n');
              const code = await recordingFinished;
              await evidence.log('reconnect video frame assertions', {
                videoStates,
                recordingStderr,
                code,
              });
              expect(code, recordingStderr).toBe(0);
            }

            // Verify the saved video, not only the live analysis stream. Both
            // outputs pass timestamps through, so frame ordinals are identical.
            const selection = videoStates
              .map(({ frame }) => `eq(n\\,${frame - 1})`)
              .join('+');
            await execFileAsync('ffmpeg', [
              '-v',
              'error',
              '-i',
              join(evidence.directory, 'telnet-reconnect.mkv'),
              '-vf',
              `select=${selection}`,
              '-fps_mode',
              'passthrough',
              join(evidence.directory, 'reconnect-frame-%d.png'),
            ]);
            for (let index = 0; index < videoStates.length; index += 1) {
              const actual = PNG.sync.read(
                await readFile(
                  join(evidence.directory, `reconnect-frame-${index + 1}.png`)
                )
              );
              const capture = videoCaptures[index];
              const expected = PNG.sync.read(capture.image);
              let difference = 0;
              for (let y = 0; y < expected.height; y += 1) {
                for (let x = 0; x < expected.width; x += 1) {
                  const actualOffset =
                    ((capture.bounds.y + y) * actual.width +
                      capture.bounds.x +
                      x) *
                    4;
                  const expectedOffset = (y * expected.width + x) * 4;
                  for (let channel = 0; channel < 3; channel += 1) {
                    difference += Math.abs(
                      actual.data[actualOffset + channel] -
                        expected.data[expectedOffset + channel]
                    );
                  }
                }
              }
              expect(
                difference / (expected.width * expected.height * 3),
                videoStates[index].state
              ).toBeLessThan(2);
            }
          }
        );
      } finally {
        for (const socket of sockets) socket.destroy();
        await closeServer(server);
      }
    });
  });

  for (const failure of ['negotiation', 'cancel'] as const) {
    it(`allows another SSH attempt after ${failure} with manual closing`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const sockets: Socket[] = [];
        const server = createServer((socket) => {
          sockets.push(socket);
          socket.resume();
        });
        try {
          const port = await listenOnLocalhost(server);
          const configPath = join(directory, 'ssh-reconnect.ini');
          await writeFile(
            configPath,
            `[general]\nauto_close=false\ntype=ssh\n\n[terminal]\n\n[ssh]\naddress=127.0.0.1\nport=${port}\nusername=retry-user\n`,
            'utf8'
          );
          await runGtkTest(context, ['-c', configPath], async (app) => {
            for (let attempt = 1; attempt <= 2; attempt += 1) {
              await waitForResult(async () => {
                expect(
                  (await (await app.getById('ssh_prompt_panel')).info()).states
                ).toContain('showing');
              });
              await expectElementKind(
                await app.getById(
                  failure === 'cancel'
                    ? 'ssh_prompt_cancel_button'
                    : 'ssh_prompt_accept_button'
                ),
                'button'
              ).click();
              if (failure === 'negotiation') {
                await waitForResult(async () =>
                  expect(sockets).toHaveLength(attempt)
                );
                sockets[attempt - 1].end('not-an-ssh-server\r\n');
              }
              const reconnect = expectElementKind(
                await app.getById('reconnect_button'),
                'button'
              );
              await waitForResult(async () => {
                const info = await reconnect.info();
                expect(info.states).toContain('showing');
                expect(info.states).toContain('sensitive');
              });
              expect(await app.getWindowCount()).toBe(1);
              if (attempt === 1) await clickWidget(app, reconnect);
            }
            // Closing during the next authentication prompt must cancel it,
            // finish the backend work and exit without starting another try.
            await clickWidget(app, await app.getById('reconnect_button'));
            await waitForResult(async () => {
              expect(
                (await (await app.getById('ssh_prompt_panel')).info()).states
              ).toContain('showing');
            });
            const pending: GtkWidgetElement[] = [
              expectElementKind(await app.windowAt(0), 'window'),
            ];
            let closed = false;
            while (pending.length !== 0) {
              const widget = pending.shift() as GtkWidgetElement;
              if (
                widget.kind === 'button' &&
                (await widget.info()).name === 'Close'
              ) {
                await widget.click();
                closed = true;
                break;
              }
              if ('getChildCount' in widget) {
                const count = await widget.getChildCount();
                for (let index = 0; index < count; index += 1) {
                  const child = await widget.childAt(index);
                  if (child !== undefined) pending.push(child);
                }
              }
            }
            expect(closed).toBe(true);
            await waitForResult(async () =>
              expect((await app.output()).exitCode).toBe(0)
            );
            expect(sockets).toHaveLength(failure === 'cancel' ? 0 : 2);
          });
        } finally {
          for (const socket of sockets) socket.destroy();
          await closeServer(server);
        }
      });
    });
  }

  for (const autoClose of ['[general]', 'auto_close=true\n', '']) {
    it(`preserves TELNET automatic closing after connection refusal with ${autoClose === '' ? 'default' : 'explicit'} settings`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const server = createServer();
        const port = await listenOnLocalhost(server);
        await closeServer(server);
        const configPath = join(directory, 'refused-auto-close.ini');
        await writeFile(
          configPath,
          `[general]\ntype=telnet\n\n[terminal]\n${autoClose}\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
          'utf8'
        );
        await runGtkTest(context, ['-c', configPath], async (app) => {
          await waitForResult(async () => {
            const output = await app.output();
            expect(output.exitCode).toBe(0);
            expect(output.exitSignal).toBeNull();
            expect(output.stderr).toContain('Connection refused');
          });
        });
      });
    });

    it(`preserves TELNET automatic closing with ${autoClose === '' ? 'default' : 'explicit'} settings`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        let socket: Socket | undefined;
        const server = createServer((accepted) => {
          socket = accepted;
          accepted.resume();
        });
        try {
          const port = await listenOnLocalhost(server);
          const configPath = join(directory, 'auto-close.ini');
          await writeFile(
            configPath,
            `[general]\ntype=telnet\n\n[terminal]\n${autoClose}\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
            'utf8'
          );
          await runGtkTest(context, ['-c', configPath], async (app) => {
            await waitForActivityIndicatorImageState(app, 'conn', 'on');
            expect(socket).not.toBeUndefined();
            socket?.end();
            await waitForResult(async () => {
              expect((await app.output()).exitCode).toBe(0);
            });
          });
        } finally {
          socket?.destroy();
          await closeServer(server);
        }
      });
    });
  }

  it('keeps TELNET reconnection available after repeated connection refusals', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const sockets: Socket[] = [];
      const server = createServer((socket) => {
        sockets.push(socket);
        socket.resume();
      });
      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'refused-reconnect.ini');
        await writeFile(
          configPath,
          `[general]\nauto_close=false\ntype=telnet\n\n[terminal]\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
          'utf8'
        );
        await runGtkTest(context, ['-c', configPath], async (app) => {
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          sockets[0].end();
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await closeServer(server);
          const reconnect = expectElementKind(
            await app.getById('reconnect_button'),
            'button'
          );
          for (let attempt = 0; attempt < 2; attempt += 1) {
            await waitForResult(async () => {
              expect((await reconnect.info()).states).toContain('sensitive');
            });
            await clickWidget(app, reconnect);
            await waitForResult(async () => {
              expect((await reconnect.info()).states).toContain('showing');
              expect((await reconnect.info()).states).toContain('sensitive');
              expect(
                await expectElementKind(
                  await app.getById('disconnected_notice_label'),
                  'label'
                ).text()
              ).toMatch(/^TELNET connection failed:\n/);
            });
          }
          expect(sockets).toHaveLength(1);
          expect(await app.getWindowCount()).toBe(1);
        });
      } finally {
        for (const socket of sockets) socket.destroy();
        await closeServer(server);
      }
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
          `[general]\nauto_close=false\ntype=telnet\n\n[terminal]\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
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
            await expectTransferButtonVisibleLeftOfApplicationMenu(app);
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

  it('opens one SFTP window from SSH and keeps it after the terminal closes', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const localDirectory = join(directory, 'local');
      const configPath = join(directory, 'ssh.ini');
      await mkdir(localDirectory, { recursive: true });
      await writeFile(join(localDirectory, 'hello.txt'), 'hello from local\n');
      await writeFile(
        configPath,
        [
          '[general]',
          'auto_close=false',
          'name=Shared SSH fixture',
          'type=ssh',
          '',
          '[terminal]',
          '',
          '[ssh]',
          'address=fixture.example',
          '',
          '[sftp]',
          `local_directory=${localDirectory}`,
          'remote_directory=/remote',
          '',
        ].join('\n'),
        'utf8'
      );

      await runGtkTest(
        context,
        [
          '--test-fixture',
          '--test-focus-transfer-on-sftp-open',
          '-c',
          configPath,
        ],
        async (app, evidence) => {
          const mainWindow = expectElementKind(
            await app.getById('main_window'),
            'window'
          );
          const transferButton =
            await expectTransferButtonVisibleLeftOfApplicationMenu(app);
          await expectTransferButtonSensitive(transferButton);
          await transferButton.click();
          const sftpItem = await waitForResult(
            async () => {
              const item = expectElementKind(
                await app.getById('transfer_sftp_item'),
                'menuItem'
              );
              const info = await item.info();
              expect(info.name).toBe('SFTP');
              expect(info.states).toContain('showing');
              return item;
            },
            {
              message: 'SFTP transfer menu item should be visible for SSH',
              timeoutMs: 5_000,
            }
          );
          await evidence.captureEvidence('ssh-transfer-sftp-menu', async () =>
            app.capture()
          );
          await sftpItem.click();

          await waitForResult(async () => {
            expect(await app.getWindowCount()).toBe(2);
            expect(
              await expectElementKind(
                await app.getById('file_transfer_remote_path_entry'),
                'entry'
              ).text()
            ).toBe('/remote');
          });
          const terminalStatus = await (
            await app.getById('status_bar')
          ).capture();
          const fileStatus = await (
            await app.getById('file_transfer_status_bar')
          ).capture();
          expect(fileStatus.bounds.height).toBe(terminalStatus.bounds.height);
          const terminal = await app.getById('terminal_view');
          expect((await terminal.info()).states).toContain('sensitive');
          await evidence.captureEvidence(
            'ssh-and-shared-sftp-windows',
            async () => app.capture()
          );
          const sftpWindow = expectElementKind(
            await app.getById('file_transfer_window'),
            'window'
          );

          await mainWindow.activate();
          await waitForResult(async () => {
            expect((await terminal.info()).states).toContain('focused');
          });

          await transferButton.click();
          await sftpItem.click();
          await waitForResult(async () => {
            expect(await app.getWindowCount()).toBe(2);
          });

          await mainWindow.activate();
          await expectElementKind(
            await app.getByPath('main_window.0.0.3'),
            'button'
          ).click();
          await waitForResult(
            async () => {
              expect(await app.getWindowCount()).toBe(1);
              expect(
                (await (await app.getById('file_transfer_window')).info())
                  .states
              ).toContain('showing');
            },
            {
              message: 'SFTP window should remain after the SSH window closes',
              timeoutMs: 5_000,
            }
          );

          await sftpWindow.activate();
          await expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          ).click();
          await expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          ).click();
          await waitForResult(async () => {
            expectElementKind(await app.getById('settings_dialog'), 'window');
            expect((await sftpWindow.info()).states).not.toContain('sensitive');
          });
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(await app.getWindowCount()).toBe(1);
            expect((await sftpWindow.info()).states).toContain('sensitive');
          });
          await expectElementKind(
            await app.getByPath('file_transfer_window.0.0.3'),
            'button'
          ).click();
          await waitForResult(
            async () => {
              const output = await app.output();
              expect(output.exitCode).toBe(0);
              return output;
            },
            {
              message: 'app should exit after the remaining SFTP window closes',
              timeoutMs: 5_000,
            }
          );
        }
      );
    });
  });

  it('keeps local SFTP browsing available when shared SSH disconnects', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const localDirectory = join(directory, 'local');
      const configPath = join(directory, 'ssh.ini');
      await mkdir(localDirectory, { recursive: true });
      await writeFile(
        configPath,
        [
          '[general]',
          'name=Disconnected SSH fixture',
          'auto_close=false',
          'type=ssh',
          '',
          '[ssh]',
          'address=fixture.example',
          '',
          '[sftp]',
          `local_directory=${localDirectory}`,
          'remote_directory=/remote',
          '',
        ].join('\n'),
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '--test-shared-sftp-disconnected', '-c', configPath],
        async (app) => {
          const transferButton =
            await expectTransferButtonVisibleLeftOfApplicationMenu(app);
          await transferButton.click();
          await expectElementKind(
            await app.getById('transfer_sftp_item'),
            'menuItem'
          ).click();

          const localPath = expectElementKind(
            await app.getById('file_transfer_local_path_entry'),
            'entry'
          );
          const remotePath = expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          );
          const status = expectElementKind(
            await app.getById('file_transfer_status_label'),
            'label'
          );
          await waitForResult(
            async () => {
              expect((await localPath.info()).states).toContain('sensitive');
              expect((await remotePath.info()).states).not.toContain(
                'sensitive'
              );
              expect(await status.text()).toBe('Disconnected');
            },
            {
              message: 'shared disconnect should only disable remote SFTP',
              timeoutMs: 5_000,
            }
          );
        }
      );
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
          `[general]\nauto_close=false\ntype=telnet\n\n[terminal]\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
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
            await expectTransferButtonVisibleLeftOfApplicationMenu(app);
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
          `[general]\nauto_close=false\ntype=telnet\n\n[terminal]\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
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

            const mainWindow = expectElementKind(
              await app.getById('main_window'),
              'window'
            );
            await openZmodemSendDialog(app);
            await waitForTransferDialogFolderUri(
              app,
              pathToFileURL(xdg.downloads).href
            );

            expectElementKind(
              await app.getById('transfer_file_dialog'),
              'container'
            );
            const exposedDialog = await app.windowAt(1);
            if (exposedDialog === undefined) {
              throw new Error('Transfer file dialog window was not found');
            }
            const dialog = expectElementKind(exposedDialog, 'window');
            expect((await dialog.info()).states).not.toContain('modal');
            expect((await mainWindow.info()).states).not.toContain('sensitive');
            await evidence.captureEvidence(
              'zmodem-send-dialog-xdg-downloads',
              async () => dialog.capture()
            );
            await mainWindow.moveTo(40, 40);
            await dialog.moveTo(480, 280);
            const mainBounds = await mainWindow.bounds();
            await app.input.moveMouseTo(mainBounds.x + 20, mainBounds.y + 20);
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await dialog.capture();
            await app.input.pressKey('Escape');
            await waitForResult(async () => {
              expect(await app.getWindowCount()).toBe(1);
              expect((await mainWindow.info()).states).toContain('sensitive');
              expect(
                (await (await app.getById('terminal_view')).info()).states
              ).toContain('focused');
            });
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
  }, 90_000);

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
          `[general]\nauto_close=false\ntype=telnet\n\n[terminal]\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n\n[transfer]\nbase_path=${basePath}\n`,
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
  }, 90_000);

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
