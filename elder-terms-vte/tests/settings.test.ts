import { createRequire } from 'node:module';
import { chmod, mkdir, readFile, writeFile } from 'node:fs/promises';
import { createServer, type Server, type Socket } from 'node:net';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { fileURLToPath } from 'node:url';
import type {
  GtkApp,
  GtkCapture,
  GtkEntryElement,
  GtkKeyboardModifier,
  GtkKeyInput,
  GtkMenuItemElement,
  GtkWidgetElement,
  GtkWindowElement,
} from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import type { PNG as PngImage } from 'pngjs';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import { expectCaptureToMatchFixture, expectElementKind } from './test-helpers';
import {
  assertTerminalTextGridMatches,
  defaultColumns,
  defaultRows,
  expectFixtureVteGridSize,
  expectMainWindowStatus,
  expectWindowCellSize,
  moveMouseToTerminalCenter,
  pressKeyWithModifiers,
  readTerminalGridLayout,
  readWindowCellLayout,
  runGtkTest,
  saveTerminalGridLayoutEvidence,
  scrollWheelWithControl,
  terminalGrid81x25ConfigPath,
  terminalInvalidValuesConfigPath,
  terminalTextGrid80x24Path,
  telnetMissingAddressConfigPath,
  terminalTextGrid80x24FontScale11Path,
  terminalTextGrid81x25Path,
  terminalZoom11ConfigPath,
  telnetLocalhostConfigPath,
  withTemporaryDirectory,
} from './gtk-test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const connectionColorsCustomFixturePath = fileURLToPath(
  new URL('./fixtures/connection-colors-custom.png', import.meta.url)
);
const connectionColorsDefaultFixturePath = fileURLToPath(
  new URL('./fixtures/connection-colors-default.png', import.meta.url)
);
const connectionColorsSettingsFixturePath = fileURLToPath(
  new URL('./fixtures/connection-colors-settings-dialog.png', import.meta.url)
);
const connectionColorsSettingsDropdownFixturePath = fileURLToPath(
  new URL('./fixtures/connection-colors-settings-dropdown.png', import.meta.url)
);
const japaneseTestEnvironment = {
  ELDER_TERMS_LOCALE_DIR: fileURLToPath(
    new URL('../../.build/po/', import.meta.url)
  ),
  LANGUAGE: 'ja',
  LC_ALL: 'ja_JP.UTF-8',
} as const;
const connectionStatusTextMaskWidth = 200;

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

const openSettingsDialog = async (app: GtkApp): Promise<void> => {
  const settingsButton = expectElementKind(
    await app.getById('settings_button'),
    'button'
  );
  await settingsButton.click();
  await toPass(async () => {
    expectElementKind(await app.getById('settings_dialog'), 'window');
    expectElementKind(await app.getById('settings_widget_root'), 'container');
  });
};

const focusEntry = async (
  app: GtkApp,
  entry: GtkEntryElement
): Promise<void> => {
  const { bounds } = await entry.capture();
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + bounds.width / 2),
    Math.trunc(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
  await toPass(async () => {
    expect((await entry.info()).states).toContain('focused');
  });
};

const captureKeyBinding = async (
  app: GtkApp,
  entry: GtkEntryElement,
  modifiers: readonly GtkKeyboardModifier[],
  key: GtkKeyInput
): Promise<void> => {
  await focusEntry(app, entry);
  for (const modifier of modifiers) {
    await app.input.setModifier(modifier, true);
  }
  try {
    await app.input.pressKey(key);
  } finally {
    for (const modifier of [...modifiers].reverse()) {
      await app.input.setModifier(modifier, false);
    }
  }
};

const expectSettingsDialogClosed = async (app: GtkApp): Promise<void> => {
  await toPass(async () => {
    let dialog: GtkWidgetElement | undefined = undefined;
    try {
      dialog = await app.getById('settings_dialog');
    } catch {
      return;
    }
    expectElementKind(dialog, 'window');
    expect((await dialog.info()).states).not.toContain('showing');
  });
};

const expectTerminalFocused = async (app: GtkApp): Promise<void> => {
  await toPass(async () => {
    expect(
      (await (await app.getById('terminal_view')).info()).states
    ).toContain('focused');
  });
};

const expectFileContent = async (
  path: string,
  expected: string
): Promise<void> => {
  await toPass(async () => {
    expect(await readFile(path, 'utf8')).toBe(expected);
  });
};

const showTerminalSettingsPage = async (app: GtkApp): Promise<void> => {
  await selectSettingsNotebookTab(
    app,
    'Terminal',
    'settings_terminal_auto_close_combo'
  );
};

const showGeneralSettingsPage = async (
  app: GtkApp
): Promise<GtkWidgetElement> =>
  selectSettingsNotebookTab(
    app,
    'General',
    'settings_general_background_mode_combo'
  );

const selectedSettingsTabName = async (app: GtkApp): Promise<string> => {
  const notebook = expectElementKind(
    await app.getById('settings_notebook'),
    'tabList'
  );
  const selected = await notebook.selectedChildAt(0);
  expect(selected).toBeDefined();
  return (await selected?.info())?.name ?? '';
};

const showLoggingSettingsPage = async (app: GtkApp): Promise<void> => {
  const notebook = expectElementKind(
    await app.getById('settings_notebook'),
    'tabList'
  );
  const childCount = await notebook.getChildCount();
  for (let index = 0; index < childCount; ++index) {
    const tab = await notebook.childAt(index);
    if (tab === undefined || (await tab.info()).name !== 'Logging') {
      continue;
    }

    await tab.click();
    await toPass(async () => {
      const enabled = await app.getById('settings_log_enabled_combo');
      expect((await enabled.info()).states).toContain('showing');
    });
    return;
  }

  throw new Error('Settings notebook tab was not found: Logging');
};

const openLogRecordingMenuItem = async (app: GtkApp) => {
  const transferButton = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  await toPass(async () => {
    const info = await transferButton.info();
    expect(info.states).toContain('showing');
    expect(info.states).toContain('sensitive');
  });
  await transferButton.click();

  return waitForResult(async () => {
    const item = expectElementKind(
      await app.getById('transfer_log_enabled_item'),
      'menuItem'
    );
    const info = await item.info();
    expect(info.name).toBe('Log recording');
    expect(info.states).toContain('showing');
    return item;
  });
};

const toggleLogRecordingMenuItem = async (
  app: GtkApp,
  item: GtkMenuItemElement
): Promise<void> => {
  await item.click();
  if ((await item.info()).states.includes('showing')) {
    await app.input.pressKey('Escape');
  }
  await toPass(async () => {
    expect((await item.info()).states).not.toContain('showing');
  });
};

const showTelnetSettingsPage = async (app: GtkApp): Promise<void> => {
  await selectSettingsNotebookTab(
    app,
    'TELNET',
    'settings_telnet_address_entry'
  );
};

const showSerialSettingsPage = async (app: GtkApp): Promise<void> => {
  await selectSettingsNotebookTab(
    app,
    'Serial',
    'settings_serial_device_entry'
  );
};

const selectSettingsNotebookTab = async (
  app: GtkApp,
  tabName: string,
  expectedVisibleId: string
): Promise<GtkWidgetElement> => {
  const notebook = expectElementKind(
    await app.getById('settings_notebook'),
    'tabList'
  );
  const childCount = await notebook.getChildCount();
  for (let index = 0; index < childCount; ++index) {
    const tab = await notebook.childAt(index);
    if (tab === undefined) {
      continue;
    }
    if ((await tab.info()).name !== tabName) {
      continue;
    }

    await tab.select();
    await toPass(async () => {
      const expected = await app.getById(expectedVisibleId);
      expect((await expected.info()).states).toContain('showing');
    });
    return tab;
  }

  throw new Error(`Settings notebook tab was not found: ${tabName}`);
};

const openComboBoxPopup = async (
  app: GtkApp,
  combo: GtkWidgetElement
): Promise<GtkWindowElement> => {
  const screenBounds = (await app.capture()).visibleBounds;
  const initialWindowCount = await app.getWindowCount();
  await expectElementKind(combo, 'comboBox').click();
  return waitForResult(async () => {
    const windowCount = await app.getWindowCount();
    expect(windowCount).toBeGreaterThan(initialWindowCount);
    for (let index = initialWindowCount; index < windowCount; ++index) {
      const popup = await app.windowAt(index);
      if (popup === undefined || popup.kind !== 'window') {
        continue;
      }
      const bounds = await popup.bounds();
      if (
        bounds.x < screenBounds.x + screenBounds.width &&
        bounds.x + bounds.width > screenBounds.x &&
        bounds.y < screenBounds.y + screenBounds.height &&
        bounds.y + bounds.height > screenBounds.y
      ) {
        return popup;
      }
    }
    throw new Error('Visible ComboBox popup window was not exposed');
  });
};

const expectSelectedConnectionType = async (
  app: GtkApp,
  expectedName: string
): Promise<void> => {
  const combo = expectElementKind(
    await app.getById('settings_general_type_combo'),
    'comboBox'
  );
  const selected = await combo.selectedChildAt(0);
  expect(selected).toBeDefined();
  expect((await selected?.info())?.name).toBe(expectedName);
};

const expectSelectedComboValue = async (
  app: GtkApp,
  id: string,
  expectedName: string
): Promise<void> => {
  const combo = expectElementKind(await app.getById(id), 'comboBox');
  const selected = await combo.selectedChildAt(0);
  expect(selected).toBeDefined();
  expect((await selected?.info())?.name).toBe(expectedName);
};

const numericEntryValue = async (entry: GtkEntryElement): Promise<number> =>
  Number(await entry.text());

const setNumericEntryValue = async (
  entry: GtkEntryElement,
  value: number
): Promise<void> => {
  await entry.setText(String(value));
};

const waitForShellExit = async (markerPath: string): Promise<void> => {
  await toPass(
    async () => {
      expect(await readFile(markerPath, 'utf8')).toBe('exited');
    },
    {
      message: 'local shell fixture should exit',
      timeoutMs: 6_000,
    }
  );
};

const expectSettingsActionCount = async (
  app: GtkApp,
  expected: number
): Promise<void> => {
  const actionRow = expectElementKind(
    await app.getById('settings_action_row'),
    'container'
  );
  expect(await actionRow.getChildCount()).toBe(expected);
};

const expectInsensitive = async (element: GtkWidgetElement): Promise<void> => {
  const info = await element.info();
  expect(info.states).not.toContain('enabled');
  expect(info.states).not.toContain('sensitive');
};

const expectSensitive = async (element: GtkWidgetElement): Promise<void> => {
  const info = await element.info();
  expect(info.states).toContain('enabled');
  expect(info.states).toContain('sensitive');
};

type RgbPixel = readonly [red: number, green: number, blue: number];

interface WindowBackgroundPixels {
  readonly header: RgbPixel;
  readonly status: RgbPixel;
  readonly terminal: RgbPixel;
}

const capturePixel = (
  capture: GtkCapture,
  horizontalRatio: number,
  verticalRatio: number
): RgbPixel => {
  const png = PNG.sync.read(capture.image) as PngImage;
  const x = Math.min(
    png.width - 1,
    Math.max(0, Math.trunc(png.width * horizontalRatio))
  );
  const y = Math.min(
    png.height - 1,
    Math.max(0, Math.trunc(png.height * verticalRatio))
  );
  const offset = (y * png.width + x) * 4;
  return [
    png.data[offset] ?? 0,
    png.data[offset + 1] ?? 0,
    png.data[offset + 2] ?? 0,
  ];
};

const capturePixelAtScreenPosition = (
  capture: GtkCapture,
  screenX: number,
  screenY: number
): RgbPixel =>
  capturePixel(
    capture,
    (screenX - capture.bounds.x) / capture.bounds.width,
    (screenY - capture.bounds.y) / capture.bounds.height
  );

const captureVisibleScreenRegion = async (
  app: GtkApp,
  element: GtkWindowElement
): Promise<GtkCapture> => {
  const [screen, elementBounds] = await Promise.all([
    app.capture(),
    element.bounds(),
  ]);
  const left = Math.max(elementBounds.x, screen.visibleBounds.x);
  const top = Math.max(elementBounds.y, screen.visibleBounds.y);
  const right = Math.min(
    elementBounds.x + elementBounds.width,
    screen.visibleBounds.x + screen.visibleBounds.width
  );
  const bottom = Math.min(
    elementBounds.y + elementBounds.height,
    screen.visibleBounds.y + screen.visibleBounds.height
  );
  if (right <= left || bottom <= top) {
    throw new Error('Element does not intersect the captured screen');
  }

  const source = PNG.sync.read(screen.image) as PngImage;
  const width = right - left;
  const height = bottom - top;
  const cropped = new PNG({ width, height }) as PngImage;
  const sourceStartX = left - screen.visibleBounds.x;
  const sourceStartY = top - screen.visibleBounds.y;
  for (let y = 0; y < height; ++y) {
    const sourceOffset = ((sourceStartY + y) * source.width + sourceStartX) * 4;
    const targetOffset = y * width * 4;
    source.data.copy(
      cropped.data,
      targetOffset,
      sourceOffset,
      sourceOffset + width * 4
    );
  }

  const visibleBounds = { x: left, y: top, width, height };
  return {
    image: PNG.sync.write(cropped),
    bounds: elementBounds,
    visibleBounds,
    clipped:
      visibleBounds.x !== elementBounds.x ||
      visibleBounds.y !== elementBounds.y ||
      visibleBounds.width !== elementBounds.width ||
      visibleBounds.height !== elementBounds.height,
  };
};

const expectRgbNear = (
  actual: RgbPixel,
  expected: RgbPixel,
  maximumChannelDifference: number
): void => {
  expect(Math.abs(actual[0] - expected[0])).toBeLessThanOrEqual(
    maximumChannelDifference
  );
  expect(Math.abs(actual[1] - expected[1])).toBeLessThanOrEqual(
    maximumChannelDifference
  );
  expect(Math.abs(actual[2] - expected[2])).toBeLessThanOrEqual(
    maximumChannelDifference
  );
};

const maximumRgbChannelDifference = (left: RgbPixel, right: RgbPixel): number =>
  Math.max(
    Math.abs(left[0] - right[0]),
    Math.abs(left[1] - right[1]),
    Math.abs(left[2] - right[2])
  );

const expectHoverBackgroundContrast = async (
  app: GtkApp,
  element: GtkWidgetElement,
  capture: () => Promise<GtkCapture>,
  normalBackground: RgbPixel
): Promise<void> => {
  expect(capturePixel(await capture(), 0.8, 0.5)).toEqual(normalBackground);
  const bounds = (await element.capture()).bounds;
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + bounds.width * 0.8),
    Math.trunc(bounds.y + bounds.height * 0.5)
  );
  await toPass(async () => {
    expect(
      maximumRgbChannelDifference(
        capturePixel(await capture(), 0.8, 0.5),
        normalBackground
      )
    ).toBeGreaterThanOrEqual(16);
  });
};

const captureWindowBackgroundPixels = async (
  app: GtkApp
): Promise<WindowBackgroundPixels> => {
  const [header, status, terminal] = await Promise.all([
    (await app.getById('header_bar')).capture(),
    (await app.getById('status_bar')).capture(),
    (await app.getById('terminal_view')).capture(),
  ]);
  return {
    header: capturePixel(header, 0.02, 0.5),
    status: capturePixel(status, 0.45, 0.5),
    terminal: capturePixel(terminal, 0.5, 0.5),
  };
};

const expectWindowBackgroundPixels = async (
  app: GtkApp,
  expected: WindowBackgroundPixels
): Promise<void> => {
  await toPass(
    async () => {
      expect(await captureWindowBackgroundPixels(app)).toEqual(expected);
    },
    {
      message: 'window background colors should match the configured RGB',
      timeoutMs: 5_000,
    }
  );
};

const expectMainWindowTitle = async (
  app: GtkApp,
  expectedTitle: string
): Promise<void> => {
  const mainWindow = expectElementKind(
    await app.getById('main_window'),
    'window'
  );
  await toPass(async () => {
    expect((await mainWindow.x11Info()).title).toBe(expectedTitle);
  });
};

const connectionStatusTextMask = async (
  app: GtkApp,
  windowCapture: GtkCapture
): Promise<{
  readonly height: number;
  readonly width: number;
  readonly x: number;
  readonly y: number;
}> => {
  const statusCapture = await (await app.getById('status_label')).capture();
  return {
    x: statusCapture.visibleBounds.x - windowCapture.visibleBounds.x,
    y: statusCapture.visibleBounds.y - windowCapture.visibleBounds.y,
    width: Math.min(
      connectionStatusTextMaskWidth,
      statusCapture.visibleBounds.width
    ),
    height: statusCapture.visibleBounds.height,
  };
};

describe.concurrent('elder-terms-vte settings', () => {
  it('uses a Japanese global UI language from a C UTF-8 environment', async (context) => {
    await runGtkTest(
      context,
      [
        '--test-fixture',
        '--test-show-transfer-progress',
        '-c',
        telnetLocalhostConfigPath,
      ],
      async (app) => {
        const settingsButton = expectElementKind(
          await app.getById('settings_button'),
          'button'
        );
        const transferButton = expectElementKind(
          await app.getById('transfer_button'),
          'toggleButton'
        );
        await waitForResult(async () => {
          expect((await settingsButton.info()).description).toBe('設定');
          const transferInfo = await transferButton.info();
          expect(transferInfo.description).toBe('転送');
          expect(transferInfo.states).toContain('showing');
          expect(transferInfo.states).toContain('sensitive');
        });
        expect(
          await expectElementKind(
            await app.getById('disconnected_notice_label'),
            'label'
          ).text()
        ).toBe('切断されました');
        const progressNotice = await app.getById('transfer_progress_notice');
        const progressLabel = expectElementKind(
          await app.getById('transfer_progress_notice_label'),
          'label'
        );
        const transferCancel = expectElementKind(
          await app.getById('transfer_cancel_button'),
          'button'
        );
        await waitForResult(async () => {
          expect(await progressLabel.text()).toBe('転送中...');
          expect((await progressNotice.info()).states).toContain('showing');
          const cancelInfo = await transferCancel.info();
          expect(cancelInfo.name).toBe('キャンセル');
          expect(cancelInfo.states).toContain('showing');
        });
        expect(
          await expectElementKind(
            await app.getById('conn_indicator_label'),
            'label'
          ).text()
        ).toBe('CONN');

        await transferButton.click();
        for (const [id, name] of [
          ['transfer_log_enabled_item', 'ログ記録'],
          ['transfer_text_send_item', 'テキスト（送信）'],
          ['transfer_zmodem_send_item', 'ZMODEM（送信）'],
          ['transfer_ymodem_send_item', 'YMODEM（送信）'],
          ['transfer_xmodem_1k_send_item', 'XMODEM 1K（送信）'],
          ['transfer_xmodem_send_item', 'XMODEM（送信）'],
          ['transfer_zmodem_receive_item', 'ZMODEM（受信）'],
          ['transfer_ymodem_g_receive_item', 'YMODEM-g（受信）'],
          ['transfer_ymodem_receive_item', 'YMODEM（受信）'],
          ['transfer_xmodem_crc_receive_item', 'XMODEM CRC（受信）'],
          ['transfer_xmodem_receive_item', 'XMODEM（受信）'],
        ] as const) {
          await waitForResult(async () => {
            const item = expectElementKind(await app.getById(id), 'menuItem');
            const info = await item.info();
            expect(info.name).toBe(name);
            expect(info.states).toContain('showing');
          });
        }
      },
      {
        env: {
          ELDER_TERMS_LOCALE_DIR:
            japaneseTestEnvironment.ELDER_TERMS_LOCALE_DIR,
        },
        globalSettings: '[general]\nui_language=ja\n',
      }
    );
  });

  it('uses an English global UI language from a Japanese environment', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture'],
      async (app) => {
        const settingsButton = expectElementKind(
          await app.getById('settings_button'),
          'button'
        );
        await waitForResult(async () => {
          expect((await settingsButton.info()).description).toBe('Settings');
        });
      },
      {
        env: japaneseTestEnvironment,
        globalSettings: '[general]\nui_language=en\n',
      }
    );
  });

  it('localizes runtime settings surfaces into Japanese', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture'],
      async (app) => {
        const settingsButton = expectElementKind(
          await app.getById('settings_button'),
          'button'
        );
        await waitForResult(async () => {
          expect((await settingsButton.info()).description).toBe('設定');
        });

        await openSettingsDialog(app);
        const dialog = expectElementKind(
          await app.getById('settings_dialog'),
          'window'
        );
        await waitForResult(async () => {
          expect((await dialog.x11Info()).title).toBe('設定');
          expect(await selectedSettingsTabName(app)).toBe('一般');
        });
        expect(
          (await (await app.getById('settings_apply_button')).info()).name
        ).toBe('適用');
        const cancel = expectElementKind(
          await app.getById('settings_cancel_button'),
          'button'
        );
        expect((await cancel.info()).name).toBe('キャンセル');
        await cancel.click();
        await expectSettingsDialogClosed(app);
      },
      {
        env: japaneseTestEnvironment,
      }
    );
  });

  it('shows an explicit connection name with the backend status', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'named.ini');
      await writeFile(
        configPath,
        '[general]\nname=Tokyo / Lab\ntype=local\n',
        'utf8'
      );
      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await expectMainWindowTitle(app, 'elder-terms: Tokyo / Lab');
          await expectMainWindowStatus(app, 'local terminal');
        }
      );
    });
  });

  it('shows the local connection name and status', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await expectMainWindowTitle(app, 'elder-terms: elder-terms');
      await expectMainWindowStatus(app, 'local terminal');
    });
  });

  it('shows the SSH connection name and endpoint in separate surfaces', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'storage.ad.kekyo.net.ini');
      await writeFile(
        configPath,
        [
          '[general]',
          'name=storage.ad.kekyo.net',
          'type=ssh',
          '',
          '[ssh]',
          'address=storage.ad.kekyo.net',
          '',
        ].join('\n'),
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await expectMainWindowTitle(app, 'elder-terms: storage.ad.kekyo.net');
          await expectMainWindowStatus(app, 'ssh: storage.ad.kekyo.net:22');
        }
      );
    });
  });

  it('restores terminal focus after closing runtime settings dialog', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await openSettingsDialog(app);
      await expectElementKind(
        await app.getById('settings_cancel_button'),
        'button'
      ).click();
      await expectSettingsDialogClosed(app);
      await expectTerminalFocused(app);
    });
  });

  it('blocks terminal input while runtime settings are open and restores it after closing', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const markerPath = join(directory, 'shell-input.txt');
      const shellPath = join(directory, 'input-shell.sh');
      const configPath = join(directory, 'auto-close-disabled.ini');
      await writeFile(
        shellPath,
        `#!/bin/sh\nIFS= read -r input\nprintf '%s' "$input" > ${shellQuote(markerPath)}\nexit 0\n`,
        'utf8'
      );
      await chmod(shellPath, 0o755);
      await writeFile(configPath, '[terminal]\nauto_close=false\n', 'utf8');

      await runGtkTest(
        context,
        ['-c', configPath],
        async (app) => {
          await openSettingsDialog(app);

          const mainWindow = expectElementKind(
            await app.getById('main_window'),
            'window'
          );
          const settingsDialog = expectElementKind(
            await app.getById('settings_dialog'),
            'window'
          );
          expect((await settingsDialog.info()).states).not.toContain('modal');
          await expectInsensitive(mainWindow);
          const nameEntry = expectElementKind(
            await app.getById('settings_general_name_entry'),
            'entry'
          );
          await focusEntry(app, nameEntry);
          await mainWindow.moveTo(40, 40);
          await settingsDialog.moveTo(480, 280);
          const mainBounds = await mainWindow.bounds();
          await app.input.moveMouseTo(mainBounds.x + 20, mainBounds.y + 20);
          await app.input.setMouseButton('left', true);
          await app.input.setMouseButton('left', false);
          await toPass(async () => {
            expect((await nameEntry.info()).states).toContain('focused');
          });
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);
          await expectSensitive(mainWindow);
          await expectTerminalFocused(app);
          await app.input.pressKey('b');
          await app.input.pressKey('Return');

          await expectFileContent(markerPath, 'b');
        },
        {
          env: {
            SHELL: shellPath,
          },
        }
      );
    });
  });

  it('uses the configured terminal grid size from an INI file', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '-c', terminalGrid81x25ConfigPath],
      async (app, evidence) => {
        const layout = await waitForResult(async () => {
          const currentLayout = await readTerminalGridLayout(app);
          expectWindowCellSize(
            currentLayout,
            defaultColumns + 1,
            defaultRows + 1
          );
          await expectFixtureVteGridSize(
            app,
            defaultColumns + 1,
            defaultRows + 1
          );
          return currentLayout;
        });
        await saveTerminalGridLayoutEvidence(
          evidence,
          layout,
          'configured-grid-layout'
        );
        await assertTerminalTextGridMatches(
          layout.terminal,
          'fixture-terminal-configured-grid',
          terminalTextGrid81x25Path,
          evidence
        );
      }
    );
  });

  it('uses the startup terminal grid size from a read-only INI file', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const startupConfigPath = join(directory, 'startup.ini');
      await writeFile(
        startupConfigPath,
        '[terminal]\nwidth=81\nheight=25\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-s', startupConfigPath],
        async (app) => {
          const layout = await waitForResult(async () => {
            const currentLayout = await readWindowCellLayout(app);
            expectWindowCellSize(
              currentLayout,
              defaultColumns + 1,
              defaultRows + 1
            );
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
            return currentLayout;
          });
          expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
        }
      );
    });
  });

  it('overlays -s after -c and saves current settings only to -c', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'config.ini');
      const startupConfigPath = join(directory, 'startup.ini');
      const startupConfig = '[terminal]\nwidth=82\nheight=26\n';
      await writeFile(
        configPath,
        '[terminal]\nwidth=81\nheight=25\nzoom=1.0\nunknown=removed\n',
        'utf8'
      );
      await writeFile(startupConfigPath, startupConfig, 'utf8');

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath, '-s', startupConfigPath],
        async (app) => {
          const layout = await waitForResult(async () => {
            const currentLayout = await readWindowCellLayout(app);
            expectWindowCellSize(
              currentLayout,
              defaultColumns + 2,
              defaultRows + 2
            );
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 2,
              defaultRows + 2
            );
            return currentLayout;
          });
          expectWindowCellSize(layout, defaultColumns + 2, defaultRows + 2);

          await openSettingsDialog(app);
          await expectSettingsActionCount(app, 3);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await toPass(async () => {
            const config = await readFile(configPath, 'utf8');
            expect(config).toContain('width=82');
            expect(config).toContain('height=26');
            expect(config).toContain('zoom=1');
            expect(config).not.toContain('unknown=');
            expect(await readFile(startupConfigPath, 'utf8')).toBe(
              startupConfig
            );
          });
        }
      );
    });
  });

  it('applies standard global defaults before -c and -s settings', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configHome = join(directory, 'xdg-config');
      const globalDirectory = join(configHome, 'elder-terms');
      const globalConfigPath = join(globalDirectory, 'global.ini');
      const configPath = join(directory, 'connection.ini');
      const startupConfigPath = join(directory, 'startup.ini');
      await mkdir(globalDirectory, { recursive: true });
      await writeFile(
        globalConfigPath,
        '[terminal]\nwidth=83\nheight=27\nzoom=1.1\nbackspace_code=bs\n',
        'utf8'
      );
      await writeFile(configPath, '[terminal]\nwidth=84\nzoom=1.2\n', 'utf8');
      await writeFile(startupConfigPath, '[terminal]\nwidth=85\n', 'utf8');

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath, '-s', startupConfigPath],
        async (app) => {
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, 85, 27);
            await expectFixtureVteGridSize(app, 85, 27);
          });

          await openSettingsDialog(app);
          await showTerminalSettingsPage(app);
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_width_entry'),
              'entry'
            ).text()
          ).toBe('85');
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_height_entry'),
              'entry'
            ).text()
          ).toBe('');
          expect(
            await numericEntryValue(
              expectElementKind(
                await app.getById('settings_terminal_zoom_entry'),
                'entry'
              )
            )
          ).toBeCloseTo(1.2);
          await expectSelectedComboValue(
            app,
            'settings_terminal_backspace_code_combo',
            'BS (global default)'
          );
        },
        {
          env: {
            XDG_CONFIG_HOME: configHome,
          },
        }
      );
    });
  });

  it('saves runtime overrides without flattening inherited global defaults', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configHome = join(directory, 'xdg-config');
      const globalDirectory = join(configHome, 'elder-terms');
      const globalConfigPath = join(globalDirectory, 'global.ini');
      const configPath = join(directory, 'connection.ini');
      const startupConfigPath = join(directory, 'startup.ini');
      const globalConfig = '[terminal]\nheight=29\nbackspace_code=bs\n';
      const startupConfig = '[terminal]\nzoom=1.1\n';
      await mkdir(globalDirectory, { recursive: true });
      await writeFile(globalConfigPath, globalConfig, 'utf8');
      await writeFile(configPath, '[terminal]\nwidth=81\n', 'utf8');
      await writeFile(startupConfigPath, startupConfig, 'utf8');

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath, '-s', startupConfigPath],
        async (app) => {
          await waitForResult(async () => {
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, 81, 29);
            await expectFixtureVteGridSize(app, 81, 29);
          });

          await openSettingsDialog(app);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await toPass(async () => {
            const savedConfig = await readFile(configPath, 'utf8');
            expect(savedConfig).toContain('width=81');
            expect(savedConfig).toContain('zoom=1.1');
            expect(savedConfig).not.toContain('height=');
            expect(savedConfig).not.toContain('backspace_code=');
            expect(await readFile(globalConfigPath, 'utf8')).toBe(globalConfig);
            expect(await readFile(startupConfigPath, 'utf8')).toBe(
              startupConfig
            );
          });
        },
        {
          env: {
            XDG_CONFIG_HOME: configHome,
          },
        }
      );
    });
  });

  it('ignores a missing standard global defaults file without warning', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      expect((await app.output()).stderr).not.toContain(
        'Warning: configuration file not found:'
      );
    });
  });

  it('uses the configured terminal zoom from an INI file', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '-c', terminalZoom11ConfigPath],
      async (app, evidence) => {
        const layout = await waitForResult(async () => {
          const currentLayout = await readTerminalGridLayout(app);
          expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
          await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
          return currentLayout;
        });
        await saveTerminalGridLayoutEvidence(
          evidence,
          layout,
          'configured-zoom-layout'
        );
        await assertTerminalTextGridMatches(
          layout.terminal,
          'fixture-terminal-configured-zoom',
          terminalTextGrid80x24FontScale11Path,
          evidence
        );
      }
    );
  });

  it('warns and uses defaults when a requested INI file is missing', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const missingConfigPath = join(directory, 'missing.ini');
      await runGtkTest(
        context,
        ['--test-fixture', '-c', missingConfigPath],
        async (app) => {
          const layout = await waitForResult(async () => {
            const currentLayout = await readTerminalGridLayout(app);
            expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            return currentLayout;
          });
          expectWindowCellSize(layout, defaultColumns, defaultRows);

          const output = await app.output();
          expect(output.stderr).toContain(
            'Warning: configuration file not found:'
          );
          expect(output.stderr).toContain(missingConfigPath);
        }
      );
    });
  });

  it('warns and falls back to defaults for invalid configured terminal values', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '-c', terminalInvalidValuesConfigPath],
      async (app) => {
        const layout = await waitForResult(async () => {
          const currentLayout = await readTerminalGridLayout(app);
          expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
          await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
          return currentLayout;
        });
        expectWindowCellSize(layout, defaultColumns, defaultRows);

        const output = await app.output();
        expect(output.stderr).toContain(
          'Warning: invalid configuration value [terminal] width:'
        );
        expect(output.stderr).toContain(
          'Warning: invalid configuration value [terminal] height:'
        );
        expect(output.stderr).toContain(
          'Warning: invalid configuration value [terminal] zoom:'
        );
        expect(output.stderr).toContain(
          'Warning: invalid configuration value [terminal] auto_close:'
        );
      }
    );
  });

  it('warns and falls back to defaults for invalid configured TELNET values', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'telnet-invalid-values.ini');
      await writeFile(
        configPath,
        '[general]\ntype=telnet\n\n[telnet]\naddress=127.0.0.1\nport=70000\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          const layout = await waitForResult(async () => {
            const currentLayout = await readTerminalGridLayout(app);
            expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            return currentLayout;
          });
          expectWindowCellSize(layout, defaultColumns, defaultRows);

          const output = await app.output();
          expect(output.stderr).toContain(
            'Warning: invalid configuration value [telnet] port:'
          );
        }
      );
    });
  });

  it('warns and leaves TELNET unconnected when the TELNET address is missing', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '-c', telnetMissingAddressConfigPath],
      async (app) => {
        const layout = await waitForResult(async () => {
          const currentLayout = await readTerminalGridLayout(app);
          expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
          await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
          return currentLayout;
        });
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectMainWindowTitle(app, 'elder-terms: telnet-missing-address');
        await expectMainWindowStatus(app, 'telnet: (unknown)');

        const output = await app.output();
        expect(output.stderr).toContain(
          'Warning: missing required configuration value [telnet] address'
        );
      }
    );
  });

  it('warns and falls back to defaults for invalid configured serial values', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial-invalid-values.ini');
      await writeFile(
        configPath,
        '[general]\ntype=serial\n\n[serial]\ndevice=/dev/ttyUSB0\nbaudrate=149\nbits=9\nparity=x\nstop_bit=3\nflow_control=invalid\ncarrier_detect=ri\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          const layout = await waitForResult(async () => {
            const currentLayout = await readTerminalGridLayout(app);
            expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            return currentLayout;
          });
          expectWindowCellSize(layout, defaultColumns, defaultRows);

          const output = await app.output();
          expect(output.stderr).toContain(
            'Warning: invalid configuration value [serial] baudrate:'
          );
          expect(output.stderr).toContain(
            'Warning: invalid configuration value [serial] bits:'
          );
          expect(output.stderr).toContain(
            'Warning: invalid configuration value [serial] parity:'
          );
          expect(output.stderr).toContain(
            'Warning: invalid configuration value [serial] stop_bit:'
          );
          expect(output.stderr).toContain(
            'Warning: invalid configuration value [serial] flow_control:'
          );
          expect(output.stderr).toContain(
            'Warning: invalid configuration value [serial] carrier_detect:'
          );
        }
      );
    });
  });

  it('warns and leaves serial unconnected when the serial device is missing', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial-missing-device.ini');
      await writeFile(configPath, '[general]\ntype=serial\n', 'utf8');

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          const layout = await waitForResult(async () => {
            const currentLayout = await readTerminalGridLayout(app);
            expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            return currentLayout;
          });
          expectWindowCellSize(layout, defaultColumns, defaultRows);
          await expectMainWindowTitle(
            app,
            'elder-terms: serial-missing-device'
          );
          await expectMainWindowStatus(app, 'serial: (unknown)');

          const output = await app.output();
          expect(output.stderr).toContain(
            'Warning: missing required configuration value [serial] device'
          );
        }
      );
    });
  });

  it('applies startup and runtime RGB backgrounds to the complete terminal window', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.write('\u001B[2J\u001B[H\u001B[?25l');
      });

      const configHome = join(directory, 'xdg-config');
      const globalDirectory = join(configHome, 'elder-terms');
      const globalConfigPath = join(globalDirectory, 'global.ini');
      const configPath = join(directory, 'connection.ini');
      const exterior = [0x20, 0x40, 0x60] as const;
      const exteriorComponentBackground = [0x24, 0x48, 0x6b] as const;
      const background = [0x60, 0x40, 0x20] as const;
      const componentBackground = [0x6b, 0x48, 0x24] as const;

      try {
        const port = await listenOnLocalhost(server);
        const initialConfig = [
          '[general]',
          'name=Connection color fixture with a stable title that hides the dynamic endpoint',
          'type=telnet',
          'exterior_background=#204060',
          'background=#604020',
          '',
          '[terminal]',
          'auto_close=false',
          '',
          '[telnet]',
          'address=127.0.0.1',
          `port=${port}`,
          '',
        ].join('\n');
        await mkdir(globalDirectory, { recursive: true });
        await writeFile(
          globalConfigPath,
          '[general]\nexterior_background=#204060\nbackground=#604020\n',
          'utf8'
        );
        await writeFile(configPath, initialConfig, 'utf8');

        await runGtkTest(
          context,
          ['-c', configPath],
          async (app, evidence) => {
            await toPass(
              async () => {
                expect(acceptedSocket).not.toBeUndefined();
              },
              {
                message: 'TELNET server should accept the color fixture',
                timeoutMs: 5_000,
              }
            );
            await waitForActivityIndicatorImageState(app, 'conn', 'on');
            await expectWindowBackgroundPixels(app, {
              header: exterior,
              status: exterior,
              terminal: background,
            });
            expect(
              capturePixel(
                await (await app.getById('header_bar')).capture(),
                0.89,
                0.5
              )
            ).toEqual(exteriorComponentBackground);

            const transferButton = await app.getById('transfer_button');
            const settingsButton = await app.getById('settings_button');
            await toPass(async () => {
              expect((await transferButton.info()).states).toContain('showing');
            });
            expect(
              capturePixel(await transferButton.capture(), 0.15, 0.5)
            ).toEqual(exteriorComponentBackground);
            expect(
              capturePixel(await settingsButton.capture(), 0.15, 0.5)
            ).toEqual(exteriorComponentBackground);

            const mainWindow = expectElementKind(
              await app.getById('main_window'),
              'window'
            );
            const startupCapture = await evidence.captureEvidence(
              'connection-colors-custom-startup',
              async () => mainWindow.capture()
            );
            // The OS-assigned TELNET port is intentionally visible in the
            // status bar and is asserted exactly outside the visual test.
            const startupStatusMask = await connectionStatusTextMask(
              app,
              startupCapture
            );

            await app.input.moveMouseTo(0, 0);
            await expectElementKind(transferButton, 'toggleButton').click();
            const transferMenuItem = await waitForResult(async () => {
              const item = expectElementKind(
                await app.getById('transfer_zmodem_send_item'),
                'menuItem'
              );
              expect((await item.info()).states).toContain('showing');
              return item;
            });
            await expectHoverBackgroundContrast(
              app,
              transferMenuItem,
              async () => transferMenuItem.capture(),
              componentBackground
            );
            await evidence.captureEvidence(
              'connection-colors-transfer-menu-hover',
              async () => transferMenuItem.capture()
            );
            await app.input.pressKey('Escape');
            await toPass(async () => {
              expect((await transferMenuItem.info()).states).not.toContain(
                'showing'
              );
            });

            await openSettingsDialog(app);
            const generalTab = await showGeneralSettingsPage(app);
            const settingsRootCapture = await (
              await app.getById('settings_widget_root')
            ).capture();
            expect(capturePixel(settingsRootCapture, 0.01, 0.5)).toEqual(
              background
            );
            await app.input.moveMouseTo(
              Math.trunc(
                settingsRootCapture.bounds.x +
                  settingsRootCapture.bounds.width / 2
              ),
              Math.trunc(
                settingsRootCapture.bounds.y +
                  settingsRootCapture.bounds.height * 0.65
              )
            );
            await toPass(async () => {
              expect(
                capturePixel(await generalTab.capture(), 0.1, 0.5)
              ).toEqual(componentBackground);
            });
            const generalTabCapture = await generalTab.capture();
            expect(capturePixel(generalTabCapture, 0.5, 0.15)).toEqual(
              componentBackground
            );
            await expectCaptureToMatchFixture(
              startupCapture,
              'connection-colors-custom-startup',
              connectionColorsCustomFixturePath,
              evidence,
              {
                masks: [startupStatusMask],
              }
            );
            const exteriorColorButton = await app.getById(
              'settings_general_exterior_background_button'
            );
            const backgroundColorButton = await app.getById(
              'settings_general_background_button'
            );
            expect(
              capturePixel(await exteriorColorButton.capture(), 0.5, 0.5)
            ).toEqual(exterior);
            expect(
              capturePixel(await backgroundColorButton.capture(), 0.5, 0.5)
            ).toEqual(background);
            const actionRowCapture = await (
              await app.getById('settings_action_row')
            ).capture();
            expect(capturePixel(actionRowCapture, 0.01, 0.5)).toEqual(exterior);
            const actionRowCenterX =
              actionRowCapture.bounds.x + actionRowCapture.bounds.width / 2;
            const actionRowCenterY =
              actionRowCapture.bounds.y + actionRowCapture.bounds.height / 2;
            const actionPanelBorderSamples = [
              capturePixelAtScreenPosition(
                settingsRootCapture,
                actionRowCapture.bounds.x - 6,
                actionRowCenterY
              ),
              capturePixelAtScreenPosition(
                settingsRootCapture,
                actionRowCapture.bounds.x + actionRowCapture.bounds.width + 6,
                actionRowCenterY
              ),
              capturePixelAtScreenPosition(
                settingsRootCapture,
                actionRowCenterX,
                actionRowCapture.bounds.y - 6
              ),
              capturePixelAtScreenPosition(
                settingsRootCapture,
                actionRowCenterX,
                actionRowCapture.bounds.y + actionRowCapture.bounds.height + 6
              ),
            ];
            expect(actionPanelBorderSamples).toEqual([
              exterior,
              exterior,
              exterior,
              exterior,
            ]);
            for (const buttonId of [
              'settings_apply_button',
              'settings_save_button',
              'settings_cancel_button',
            ]) {
              const buttonCapture = await (
                await app.getById(buttonId)
              ).capture();
              expect(capturePixel(buttonCapture, 0.15, 0.5)).toEqual(
                exteriorComponentBackground
              );
            }
            const settingsDialog = expectElementKind(
              await app.getById('settings_dialog'),
              'window'
            );
            const settingsDialogCapture = await evidence.captureEvidence(
              'connection-colors-settings-dialog',
              async () => settingsDialog.capture()
            );
            const generalTabCenterX =
              generalTabCapture.bounds.x + generalTabCapture.bounds.width / 2;
            const generalTabCenterY =
              generalTabCapture.bounds.y + generalTabCapture.bounds.height / 2;
            expect([
              capturePixelAtScreenPosition(
                settingsDialogCapture,
                generalTabCapture.bounds.x - 4,
                generalTabCenterY
              ),
              capturePixelAtScreenPosition(
                settingsDialogCapture,
                generalTabCapture.bounds.x + generalTabCapture.bounds.width + 4,
                generalTabCenterY
              ),
              capturePixelAtScreenPosition(
                settingsDialogCapture,
                generalTabCenterX,
                generalTabCapture.bounds.y - 4
              ),
            ]).toEqual([
              componentBackground,
              componentBackground,
              componentBackground,
            ]);
            await expectCaptureToMatchFixture(
              settingsDialogCapture,
              'connection-colors-settings-dialog',
              connectionColorsSettingsFixturePath,
              evidence
            );

            const settingsHeader = await app.findById(
              'settings_dialog_header_bar'
            );
            const settingsHeaderColor =
              settingsHeader === undefined
                ? undefined
                : capturePixel(await settingsHeader.capture(), 0.1, 0.5);
            const backgroundModeCombo = expectElementKind(
              await app.getById('settings_general_background_mode_combo'),
              'comboBox'
            );
            const backgroundModeComboColor = capturePixel(
              await backgroundModeCombo.capture(),
              0.5,
              0.5
            );
            await app.input.moveMouseTo(0, 0);
            const dropdown = await openComboBoxPopup(app, backgroundModeCombo);
            const dropdownCapture = await evidence.captureEvidence(
              'connection-colors-settings-dropdown',
              async () => captureVisibleScreenRegion(app, dropdown)
            );
            const dropdownColor = capturePixel(dropdownCapture, 0.8, 0.5);
            expect({
              settingsHeader: settingsHeaderColor,
              backgroundModeCombo: backgroundModeComboColor,
              dropdown: dropdownColor,
            }).toEqual({
              settingsHeader: exterior,
              backgroundModeCombo: componentBackground,
              dropdown: componentBackground,
            });
            await expectCaptureToMatchFixture(
              dropdownCapture,
              'connection-colors-settings-dropdown',
              connectionColorsSettingsDropdownFixturePath,
              evidence
            );
            expect(
              maximumRgbChannelDifference(
                capturePixel(dropdownCapture, 0.8, 0.83),
                componentBackground
              )
            ).toBeGreaterThanOrEqual(16);
            await app.input.pressKey('Escape');

            await expectElementKind(
              await app.getById(
                'settings_general_exterior_background_mode_combo'
              ),
              'comboBox'
            ).selectChildAt(1);
            await expectElementKind(
              await app.getById('settings_general_background_mode_combo'),
              'comboBox'
            ).selectChildAt(1);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await expectSettingsDialogClosed(app);
            const defaultPixels = await captureWindowBackgroundPixels(app);
            expect(defaultPixels).not.toEqual({
              header: exterior,
              status: exterior,
              terminal: background,
            });
            const defaultCapture = await evidence.captureEvidence(
              'connection-colors-default-runtime',
              async () => mainWindow.capture()
            );
            await expectCaptureToMatchFixture(
              defaultCapture,
              'connection-colors-default-runtime',
              connectionColorsDefaultFixturePath,
              evidence,
              {
                masks: [await connectionStatusTextMask(app, defaultCapture)],
              }
            );

            await openSettingsDialog(app);
            await showGeneralSettingsPage(app);
            await expectElementKind(
              await app.getById(
                'settings_general_exterior_background_mode_combo'
              ),
              'comboBox'
            ).selectChildAt(0);
            await expectElementKind(
              await app.getById('settings_general_background_mode_combo'),
              'comboBox'
            ).selectChildAt(0);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await expectSettingsDialogClosed(app);
            await expectWindowBackgroundPixels(app, {
              header: exterior,
              status: exterior,
              terminal: background,
            });
            const runtimeCapture = await evidence.captureEvidence(
              'connection-colors-custom-runtime',
              async () => mainWindow.capture()
            );
            await expectCaptureToMatchFixture(
              runtimeCapture,
              'connection-colors-custom-runtime',
              connectionColorsCustomFixturePath,
              evidence,
              {
                masks: [await connectionStatusTextMask(app, runtimeCapture)],
              }
            );
            await expectFileContent(configPath, initialConfig);
          },
          {
            env: {
              XDG_CONFIG_HOME: configHome,
            },
          }
        );
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  }, 90_000);

  it('opens the runtime settings dialog from the header bar', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await openSettingsDialog(app);
      expect(await selectedSettingsTabName(app)).toBe('General');
    });
  });

  it('shows Save only when -c provides a persistent target', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await openSettingsDialog(app);
      await expectSettingsActionCount(app, 2);
    });

    await withTemporaryDirectory(async (directory) => {
      const startupConfigPath = join(directory, 'startup.ini');
      await writeFile(startupConfigPath, '[terminal]\nwidth=81\n', 'utf8');
      await runGtkTest(
        context,
        ['--test-fixture', '-s', startupConfigPath],
        async (app) => {
          await openSettingsDialog(app);
          await expectSettingsActionCount(app, 2);
        }
      );
    });

    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'config.ini');
      await writeFile(configPath, '', 'utf8');
      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await openSettingsDialog(app);
          await expectSettingsActionCount(app, 3);
          expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          );
        }
      );
    });
  });

  it('toggles runtime logging from the transfer menu without implicitly saving it', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'runtime-log.ini');
        const logPath = join(directory, 'logs', 'runtime.txt');
        const initialConfig = `[general]\ntype=telnet\n\n[terminal]\nauto_close=false\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n\n[log]\nenabled=false\nbase_directory=${directory}\nfile_name_format=logs/runtime.txt\nmode=cooked\n`;
        await writeFile(configPath, initialConfig, 'utf8');

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
            },
            {
              message: 'TELNET server should accept a logging client',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await waitForActivityIndicatorImageState(app, 'log', 'off');

          let logItem = await openLogRecordingMenuItem(app);
          expect((await logItem.info()).states).not.toContain('checked');
          await toggleLogRecordingMenuItem(app, logItem);
          await waitForActivityIndicatorImageState(app, 'log', 'on');
          await expectFileContent(configPath, initialConfig);

          acceptedSocket?.write('MENU_LOGGED\r\n');
          await toPass(
            async () => {
              expect(await readFile(logPath, 'utf8')).toContain('MENU_LOGGED');
            },
            {
              message: 'menu-enabled logging should record TELNET output',
              timeoutMs: 5_000,
            }
          );

          await openSettingsDialog(app);
          await showLoggingSettingsPage(app);
          await expectSelectedComboValue(
            app,
            'settings_log_enabled_combo',
            'Enabled'
          );
          await expectElementKind(
            await app.getById('settings_log_enabled_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);
          await waitForActivityIndicatorImageState(app, 'log', 'off');
          const savedConfig = await readFile(configPath, 'utf8');
          expect(savedConfig).toContain('enabled=false');

          await openSettingsDialog(app);
          await showLoggingSettingsPage(app);
          await expectSelectedComboValue(
            app,
            'settings_log_enabled_combo',
            'Disabled'
          );
          await expectElementKind(
            await app.getById('settings_log_enabled_combo'),
            'comboBox'
          ).selectChildAt(1);
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);
          await waitForActivityIndicatorImageState(app, 'log', 'on');

          logItem = await openLogRecordingMenuItem(app);
          expect((await logItem.info()).states).toContain('checked');
          await toggleLogRecordingMenuItem(app, logItem);
          await waitForActivityIndicatorImageState(app, 'log', 'off');
          await expectFileContent(configPath, savedConfig);

          acceptedSocket?.end('MENU_NOT_LOGGED\r\n');
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await toPass(async () => {
            const log = await readFile(logPath, 'utf8');
            expect(log).toContain('MENU_LOGGED');
            expect(log).not.toContain('MENU_NOT_LOGGED');
          });
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('reflects the current local runtime settings in General and Terminal', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await openSettingsDialog(app);

      await expectSelectedConnectionType(app, 'Local shell (built-in default)');
      await showTerminalSettingsPage(app);
      expect(
        await expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        ).text()
      ).toBe('');
      expect(
        await expectElementKind(
          await app.getById('settings_terminal_height_entry'),
          'entry'
        ).text()
      ).toBe('');
      expect(
        await expectElementKind(
          await app.getById('settings_terminal_zoom_entry'),
          'entry'
        ).text()
      ).toBe('');
      await expectSelectedComboValue(
        app,
        'settings_terminal_auto_close_combo',
        'Enabled (built-in default)'
      );
    });
  });

  it('reflects runtime terminal grid settings after window resizing', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      const initialLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });

      await initialLayout.mainWindow.resizeTo(
        initialLayout.mainBounds.width + initialLayout.hints.widthIncrement,
        initialLayout.mainBounds.height + initialLayout.hints.heightIncrement
      );

      await toPass(async () => {
        const layout = await readWindowCellLayout(app);
        expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
        await expectFixtureVteGridSize(
          app,
          defaultColumns + 1,
          defaultRows + 1
        );
      });

      await openSettingsDialog(app);
      await showTerminalSettingsPage(app);

      await toPass(async () => {
        expect(
          await numericEntryValue(
            expectElementKind(
              await app.getById('settings_terminal_width_entry'),
              'entry'
            )
          )
        ).toBe(defaultColumns + 1);
        expect(
          await numericEntryValue(
            expectElementKind(
              await app.getById('settings_terminal_height_entry'),
              'entry'
            )
          )
        ).toBe(defaultRows + 1);
      });
    });
  });

  it('reflects Ctrl+wheel runtime zoom when opening settings', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      const initialLayout = await waitForResult(async () => {
        const layout = await readTerminalGridLayout(app);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return layout;
      });

      await moveMouseToTerminalCenter(app, initialLayout);
      await scrollWheelWithControl(app, -1);
      await toPass(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).not.toBe(
          initialLayout.hints.heightIncrement
        );
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
      });

      await openSettingsDialog(app);
      await showTerminalSettingsPage(app);
      expect(
        await numericEntryValue(
          expectElementKind(
            await app.getById('settings_terminal_zoom_entry'),
            'entry'
          )
        )
      ).toBeCloseTo(1.1);
    });
  });

  it('applies edited terminal key bindings at runtime', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      const initialLayout = await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expectWindowCellSize(layout, defaultColumns, defaultRows);
        return layout;
      });

      await openSettingsDialog(app);
      await showTerminalSettingsPage(app);
      const zoomInKey = expectElementKind(
        await app.getById('settings_terminal_zoom_in_key_entry'),
        'entry'
      );
      const zoomOutKey = expectElementKind(
        await app.getById('settings_terminal_zoom_out_key_entry'),
        'entry'
      );
      await captureKeyBinding(app, zoomInKey, ['alt'], 'Up');
      await captureKeyBinding(app, zoomOutKey, ['alt'], 'Down');
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();
      await expectSettingsDialogClosed(app);
      await expectTerminalFocused(app);

      await pressKeyWithModifiers(app, ['alt'], 'Up');
      await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.hints.widthIncrement).not.toBe(
          initialLayout.hints.widthIncrement
        );
      });

      await pressKeyWithModifiers(app, ['alt'], 'Down');
      await waitForResult(async () => {
        const layout = await readWindowCellLayout(app);
        expect(layout.hints.widthIncrement).toBe(
          initialLayout.hints.widthIncrement
        );
        expect(layout.hints.heightIncrement).toBe(
          initialLayout.hints.heightIncrement
        );
      });
    });
  });

  it('reflects the current TELNET runtime settings in General and TELNET', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const server = createServer();
      try {
        const port = await listenOnLocalhost(server);
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        const configPath = join(directory, 'telnet.ini');
        await writeFile(
          configPath,
          configTemplate.replace('${port}', String(port)),
          'utf8'
        );

        await runGtkTest(
          context,
          ['--test-fixture', '-c', configPath],
          async (app) => {
            await expectMainWindowTitle(app, 'elder-terms: telnet');
            await expectMainWindowStatus(app, `telnet: 127.0.0.1:${port}`);
            await openSettingsDialog(app);

            await expectSelectedConnectionType(app, 'TELNET');
            await showTelnetSettingsPage(app);
            expect(
              await expectElementKind(
                await app.getById('settings_telnet_address_entry'),
                'entry'
              ).text()
            ).toBe('127.0.0.1');
            expect(
              await numericEntryValue(
                expectElementKind(
                  await app.getById('settings_telnet_port_entry'),
                  'entry'
                )
              )
            ).toBe(port);
            expect((await app.output()).stderr).toBe('');
          }
        );
      } finally {
        await closeServer(server);
      }
    });
  });

  it('shows the configured serial connection name and status', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      await writeFile(
        configPath,
        '[general]\ntype=serial\n\n[serial]\ndevice=/dev/ttyUSB1\nbaudrate=115200\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await expectMainWindowTitle(app, 'elder-terms: serial');
          await expectMainWindowStatus(app, 'serial: /dev/ttyUSB1:115200:n81n');
        }
      );
    });
  });

  it('reflects the current serial runtime settings in General and Serial', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial.ini');
      await writeFile(
        configPath,
        '[general]\ntype=serial\n\n[serial]\ndevice=/dev/ttyUSB0\nbaudrate=115200\nbits=7\nparity=e\nstop_bit=2\nflow_control=xon\ncarrier_detect=dsr\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await expectMainWindowTitle(app, 'elder-terms: serial');
          await expectMainWindowStatus(app, 'serial: /dev/ttyUSB0:115200:e72x');
          await openSettingsDialog(app);

          await expectSelectedConnectionType(app, 'Serial');
          await showSerialSettingsPage(app);
          const device = expectElementKind(
            await app.getById('settings_serial_device_entry'),
            'entry'
          );
          const baudrate = expectElementKind(
            await app.getById('settings_serial_baudrate_entry'),
            'entry'
          );
          const bits = expectElementKind(
            await app.getById('settings_serial_bits_combo'),
            'comboBox'
          );
          const parity = expectElementKind(
            await app.getById('settings_serial_parity_combo'),
            'comboBox'
          );
          const stopBit = expectElementKind(
            await app.getById('settings_serial_stop_bit_combo'),
            'comboBox'
          );
          const flowControl = expectElementKind(
            await app.getById('settings_serial_flow_control_combo'),
            'comboBox'
          );
          const carrierDetect = expectElementKind(
            await app.getById('settings_serial_carrier_detect_combo'),
            'comboBox'
          );
          expect(await device.text()).toBe('/dev/ttyUSB0');
          expect(await numericEntryValue(baudrate)).toBe(115200);
          await expectSelectedComboValue(
            app,
            'settings_serial_bits_combo',
            '7'
          );
          await expectSelectedComboValue(
            app,
            'settings_serial_parity_combo',
            'Even'
          );
          await expectSelectedComboValue(
            app,
            'settings_serial_stop_bit_combo',
            '2'
          );
          await expectSelectedComboValue(
            app,
            'settings_serial_flow_control_combo',
            'XON/XOFF (software)'
          );
          await expectSelectedComboValue(
            app,
            'settings_serial_carrier_detect_combo',
            'DSR (Data Set Ready)'
          );
          await expectInsensitive(device);
          await expectSensitive(baudrate);
          await expectSensitive(bits);
          await expectSensitive(parity);
          await expectSensitive(stopBit);
          await expectSensitive(flowControl);
          await expectSensitive(carrierDetect);
          expect((await app.output()).stderr).toBe('');

          await setNumericEntryValue(baudrate, 57600);
          await bits.selectChildAt(1);
          await parity.selectChildAt(3);
          await stopBit.selectChildAt(2);
          await flowControl.selectChildAt(3);
          await carrierDetect.selectChildAt(2);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await toPass(async () => {
            const config = await readFile(configPath, 'utf8');
            expect(config).toContain('type=serial');
            expect(config).toContain('device=/dev/ttyUSB0');
            expect(config).toContain('baudrate=57600');
            expect(config).toContain('bits=5');
            expect(config).toContain('parity=o');
            expect(config).toContain('stop_bit=2');
            expect(config).toContain('flow_control=hard');
            expect(config).toContain('carrier_detect=cts');
          });
        }
      );
    });
  });

  it('updates the serial connection status when runtime settings are applied', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'serial-apply-title.ini');
      await writeFile(
        configPath,
        '[general]\ntype=serial\n\n[serial]\ndevice=/dev/ttyUSB1\nbaudrate=115200\nbits=8\nparity=n\nstop_bit=1\nflow_control=none\ncarrier_detect=cd\n',
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await expectMainWindowTitle(app, 'elder-terms: serial-apply-title');
          await expectMainWindowStatus(app, 'serial: /dev/ttyUSB1:115200:n81n');
          await openSettingsDialog(app);
          await showSerialSettingsPage(app);

          await expectElementKind(
            await app.getById('settings_serial_baudrate_entry'),
            'entry'
          ).setText('57600');
          await expectElementKind(
            await app.getById('settings_serial_bits_combo'),
            'comboBox'
          ).selectChildAt(1);
          await expectElementKind(
            await app.getById('settings_serial_parity_combo'),
            'comboBox'
          ).selectChildAt(3);
          await expectElementKind(
            await app.getById('settings_serial_stop_bit_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectElementKind(
            await app.getById('settings_serial_flow_control_combo'),
            'comboBox'
          ).selectChildAt(3);
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await expectMainWindowTitle(app, 'elder-terms: serial-apply-title');
          await expectMainWindowStatus(app, 'serial: /dev/ttyUSB1:57600:o52h');
        }
      );
    });
  });

  it('closes without applying or saving runtime terminal grid size changes when cancelled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'cancel-grid.ini');
      const initialConfig = `[terminal]\nwidth=${defaultColumns}\nheight=${defaultRows}\n`;
      await writeFile(configPath, initialConfig, 'utf8');

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await openSettingsDialog(app);
          await showTerminalSettingsPage(app);

          await expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          ).setText(String(defaultColumns + 1));
          await expectElementKind(
            await app.getById('settings_terminal_height_entry'),
            'entry'
          ).setText(String(defaultRows + 1));
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await toPass(async () => {
            await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, defaultColumns, defaultRows);
          });
          await expectFileContent(configPath, initialConfig);
        }
      );
    });
  });

  it('does not apply runtime terminal zoom changes when cancelled', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      await openSettingsDialog(app);
      await showTerminalSettingsPage(app);

      await expectElementKind(
        await app.getById('settings_terminal_zoom_entry'),
        'entry'
      ).setText('1.1');
      await expectElementKind(
        await app.getById('settings_cancel_button'),
        'button'
      ).click();
      await expectSettingsDialogClosed(app);
      await delay(300);

      const layout = await waitForResult(async () => {
        const currentLayout = await readTerminalGridLayout(app);
        expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return currentLayout;
      });
      await assertTerminalTextGridMatches(
        layout.terminal,
        'fixture-terminal-cancelled-runtime-zoom',
        terminalTextGrid80x24Path,
        evidence
      );
    });
  });

  it('does not apply runtime auto_close changes when cancelled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const markerPath = join(directory, 'shell-exited.txt');
      const shellPath = join(directory, 'delayed-exit-shell.sh');
      await writeFile(
        shellPath,
        `#!/bin/sh\nsleep 3\nprintf exited > ${shellQuote(markerPath)}\nexit 0\n`,
        'utf8'
      );
      await chmod(shellPath, 0o755);

      await runGtkTest(
        context,
        [],
        async (app) => {
          await openSettingsDialog(app);
          await showTerminalSettingsPage(app);

          await expectSelectedComboValue(
            app,
            'settings_terminal_auto_close_combo',
            'Enabled (built-in default)'
          );
          await expectElementKind(
            await app.getById('settings_terminal_auto_close_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectSelectedComboValue(
            app,
            'settings_terminal_auto_close_combo',
            'Disabled'
          );
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await waitForShellExit(markerPath);
          await delay(1_000);
          const output = await app.output();
          expect(output.exitCode).toBe(0);
          expect(output.exitSignal).toBeNull();
        },
        {
          env: {
            SHELL: shellPath,
          },
        }
      );
    });
  });

  it('closes and applies runtime terminal grid size changes without saving when applied', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'apply-grid.ini');
      const initialConfig = `[terminal]\nwidth=${defaultColumns}\nheight=${defaultRows}\n`;
      await writeFile(configPath, initialConfig, 'utf8');

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await openSettingsDialog(app);
          await showTerminalSettingsPage(app);

          const width = expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          );
          const height = expectElementKind(
            await app.getById('settings_terminal_height_entry'),
            'entry'
          );
          await setNumericEntryValue(width, defaultColumns + 1);
          await setNumericEntryValue(height, defaultRows + 1);
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await toPass(async () => {
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
          });
          await expectFileContent(configPath, initialConfig);
        }
      );
    });
  });

  it('closes, applies, and saves runtime terminal grid size changes when saved', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'save-grid.ini');
      const initialConfig = `[terminal]\nwidth=${defaultColumns}\nheight=${defaultRows}\n`;
      await writeFile(configPath, initialConfig, 'utf8');

      await runGtkTest(
        context,
        ['--test-fixture', '-c', configPath],
        async (app) => {
          await openSettingsDialog(app);
          await showTerminalSettingsPage(app);

          const width = expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          );
          const height = expectElementKind(
            await app.getById('settings_terminal_height_entry'),
            'entry'
          );
          await setNumericEntryValue(width, defaultColumns + 1);
          await setNumericEntryValue(height, defaultRows + 1);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await toPass(async () => {
            await expectFixtureVteGridSize(
              app,
              defaultColumns + 1,
              defaultRows + 1
            );
            const layout = await readWindowCellLayout(app);
            expectWindowCellSize(layout, defaultColumns + 1, defaultRows + 1);
          });
          await toPass(async () => {
            const config = await readFile(configPath, 'utf8');
            expect(config).toContain(`width=${defaultColumns + 1}`);
            expect(config).toContain(`height=${defaultRows + 1}`);
          });
        }
      );
    });
  });

  it('applies runtime terminal zoom changes', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      await openSettingsDialog(app);
      await showTerminalSettingsPage(app);

      const zoom = expectElementKind(
        await app.getById('settings_terminal_zoom_entry'),
        'entry'
      );
      await setNumericEntryValue(zoom, 1.1);
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();
      await expectSettingsDialogClosed(app);
      await delay(300);

      const layout = await waitForResult(async () => {
        const currentLayout = await readTerminalGridLayout(app);
        expectWindowCellSize(currentLayout, defaultColumns, defaultRows);
        await expectFixtureVteGridSize(app, defaultColumns, defaultRows);
        return currentLayout;
      });
      await assertTerminalTextGridMatches(
        layout.terminal,
        'fixture-terminal-runtime-zoom',
        terminalTextGrid80x24FontScale11Path,
        evidence
      );
    });
  });

  it('applies runtime auto_close changes before the local shell exits', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const markerPath = join(directory, 'shell-exited.txt');
      const shellPath = join(directory, 'delayed-exit-shell.sh');
      await writeFile(
        shellPath,
        `#!/bin/sh\nsleep 3\nprintf exited > ${shellQuote(markerPath)}\nexit 0\n`,
        'utf8'
      );
      await chmod(shellPath, 0o755);

      await runGtkTest(
        context,
        [],
        async (app, evidence) => {
          await openSettingsDialog(app);
          await showTerminalSettingsPage(app);

          await expectSelectedComboValue(
            app,
            'settings_terminal_auto_close_combo',
            'Enabled (built-in default)'
          );
          await expectElementKind(
            await app.getById('settings_terminal_auto_close_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectSelectedComboValue(
            app,
            'settings_terminal_auto_close_combo',
            'Disabled'
          );
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await waitForShellExit(markerPath);
          await delay(1_000);
          const output = await app.output();
          expect(output.exitCode).toBeNull();
          expect(output.exitSignal).toBeNull();
          await evidence.log('runtime auto_close disabled before shell exit', {
            exitCode: output.exitCode,
            exitSignal: output.exitSignal,
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
