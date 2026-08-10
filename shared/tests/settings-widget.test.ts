import { fileURLToPath } from 'node:url';
import { mkdtemp, mkdir, rm, symlink, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import type {
  GtkApp,
  GtkCapture,
  GtkEntryElement,
  GtkKeyboardModifier,
  GtkKeyInput,
  GtkWidgetElement,
} from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  countCaptureFixtureDiffPixels,
  countNonBackgroundPixels,
  expectCaptureToMatchFixture,
  expectElementKind,
  runSharedGtkTest,
} from './test-helpers';

const fixturePath = (name: string): string =>
  fileURLToPath(new URL(`./fixtures/${name}.png`, import.meta.url));

const japaneseTestEnvironment = {
  ELDER_TERMS_LOCALE_DIR: fileURLToPath(
    new URL('../../.build/po/', import.meta.url)
  ),
  LANGUAGE: 'ja',
  LC_ALL: 'ja_JP.UTF-8',
} as const;

const visualComparisonOptions = {
  maxDiffPixels: 0,
  threshold: 0.01,
} as const;

const telnetEditableFixturePath = fileURLToPath(
  new URL('./fixtures/settings-widget-telnet-editable.png', import.meta.url)
);

const terminalPageFixturePath = fileURLToPath(
  new URL('./fixtures/settings-widget-terminal-page.png', import.meta.url)
);

const actionRowFixturePath = fileURLToPath(
  new URL('./fixtures/settings-widget-action-row.png', import.meta.url)
);

interface AppliedStore {
  readonly [key: string]: string;
  readonly name: string;
  readonly auto_close: string;
  readonly backspace_code: string;
  readonly background: string;
  readonly cursor_key_mode: string;
  readonly return_code: string;
  readonly scrollback_lines: string;
  readonly encoding: string;
  readonly exterior_background: string;
  readonly height: string;
  readonly font_fallback_family: string;
  readonly font_fallback_family_explicit: string;
  readonly font_fallback_family_source: string;
  readonly font_primary_family: string;
  readonly font_primary_family_explicit: string;
  readonly font_primary_family_source: string;
  readonly log_base_directory: string;
  readonly log_enabled: string;
  readonly log_file_name_format: string;
  readonly log_mode: string;
  readonly open_application: string;
  readonly open_connection: string;
  readonly ssh_address: string;
  readonly ssh_identity_file: string;
  readonly ssh_port: string;
  readonly ssh_terminal_type: string;
  readonly ssh_username: string;
  readonly startup_mode: string;
  readonly ui_language: string;
  readonly sftp_local_directory: string;
  readonly sftp_remote_directory: string;
  readonly telnet_address: string;
  readonly telnet_port: string;
  readonly telnet_terminal_type: string;
  readonly serial_baudrate: string;
  readonly serial_bits: string;
  readonly serial_carrier_detect: string;
  readonly serial_device: string;
  readonly serial_device_match_mode: string;
  readonly serial_device_usb_serial: string;
  readonly serial_flow_control: string;
  readonly serial_parity: string;
  readonly serial_stop_bit: string;
  readonly send_break_key: string;
  readonly transfer_base_path: string;
  readonly text_send_bytes_per_second: string;
  readonly text_send_follow_return_code: string;
  readonly type: string;
  readonly width: string;
  readonly zmodem_autostart: string;
  readonly zoom: string;
  readonly zoom_in_key: string;
  readonly zoom_out_key: string;
}

interface SettingVisualCase {
  readonly args: readonly string[];
  readonly assert: (app: GtkApp) => Promise<void>;
  readonly differsFrom: string | undefined;
  readonly fixtureName: string;
  readonly pageId: string;
  readonly prepare: (app: GtkApp) => Promise<void>;
}

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

const descendantAccessibleIds = async (
  root: GtkWidgetElement
): Promise<ReadonlySet<string>> => {
  const ids = new Set<string>();
  const pending = [root];
  while (pending.length > 0) {
    const element = pending.pop() as GtkWidgetElement;
    const info = await element.info();
    if (info.accessibleId.length > 0) {
      ids.add(info.accessibleId);
    }
    if (!('getChildCount' in element) || !('childAt' in element)) {
      continue;
    }
    const childCount = await element.getChildCount();
    for (let index = 0; index < childCount; ++index) {
      const child = await element.childAt(index);
      if (child !== undefined) {
        pending.push(child);
      }
    }
  }
  return ids;
};

const showTerminalPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const width = await app.getById('settings_terminal_width_entry');
    expect((await width.info()).states).toContain('showing');
  });
};

const showTerminalKeyBindings = async (app: GtkApp): Promise<void> => {
  const scrollbar = expectElementKind(
    await app.getById('settings_terminal_page_scrollbar'),
    'scrollbar'
  );
  const range = await scrollbar.valueInfo();
  await scrollbar.setValue(range.maximum);
  await waitForResult(async () => {
    const zoomIn = await app.getById('settings_terminal_zoom_in_key_entry');
    const zoomOut = await app.getById('settings_terminal_zoom_out_key_entry');
    const sendBreak = await app.getById(
      'settings_terminal_send_break_key_entry'
    );
    expect((await zoomIn.info()).states).toContain('showing');
    expect((await zoomOut.info()).states).toContain('showing');
    expect((await sendBreak.info()).states).toContain('showing');
  });
};

const showTelnetPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const address = await app.getById('settings_telnet_address_entry');
    expect((await address.info()).states).toContain('showing');
  });
};

const showSshPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const address = await app.getById('settings_ssh_address_entry');
    expect((await address.info()).states).toContain('showing');
  });
};

const showSftpPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const localDirectory = await app.getById(
      'settings_sftp_local_directory_entry'
    );
    expect((await localDirectory.info()).states).toContain('showing');
  });
};

const showSerialPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const device = await app.getById('settings_serial_device_combo');
    expect((await device.info()).states).toContain('showing');
  });
};

interface SerialDeviceFixture {
  readonly byIdRoot: string;
  readonly byPathRoot: string;
  readonly devRoot: string;
  readonly environment: Readonly<Record<string, string>>;
  readonly physicalTarget: string;
  readonly root: string;
  readonly stableTarget: string;
  readonly sysClassTtyRoot: string;
}

const createSerialDeviceFixture = async (): Promise<SerialDeviceFixture> => {
  const root = await mkdtemp(join(tmpdir(), 'elder-terms-serial-widget-'));
  const devRoot = join(root, 'dev');
  const byIdRoot = join(root, 'by-id');
  const byPathRoot = join(root, 'by-path');
  const sysClassTtyRoot = join(root, 'sys-class-tty');
  const stableTarget = join(byIdRoot, 'usb-elder-demo');
  const physicalTarget = join(byPathRoot, 'pci-demo-usb-0');
  await Promise.all([
    mkdir(devRoot, { recursive: true }),
    mkdir(byIdRoot, { recursive: true }),
    mkdir(byPathRoot, { recursive: true }),
    mkdir(join(sysClassTtyRoot, 'null', 'device'), { recursive: true }),
  ]);
  await Promise.all([
    symlink('/dev/null', stableTarget),
    symlink('/dev/null', physicalTarget),
    writeFile(
      join(sysClassTtyRoot, 'null', 'device', 'product'),
      'Elder USB Demo\n'
    ),
    writeFile(
      join(sysClassTtyRoot, 'null', 'device', 'serial'),
      'FT12345678901234\n'
    ),
  ]);
  return {
    byIdRoot,
    byPathRoot,
    devRoot,
    environment: {
      ELDER_TERMS_SERIAL_BY_ID_ROOT: byIdRoot,
      ELDER_TERMS_SERIAL_BY_PATH_ROOT: byPathRoot,
      ELDER_TERMS_SERIAL_DEV_ROOT: devRoot,
      ELDER_TERMS_SERIAL_SYS_CLASS_TTY_ROOT: sysClassTtyRoot,
    },
    physicalTarget,
    root,
    stableTarget,
    sysClassTtyRoot,
  };
};

const showTransferPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const basePath = await app.getById('settings_transfer_base_path_entry');
    expect((await basePath.info()).states).toContain('showing');
  });
};

const showLoggingPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const enabled = await app.getById('settings_log_enabled_combo');
    expect((await enabled.info()).states).toContain('showing');
  });
};

const stayOnInitialPage = async (_app: GtkApp): Promise<void> => {};

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

const comboOptionNames = async (
  app: GtkApp,
  id: string
): Promise<readonly string[]> => {
  const combo = expectElementKind(await app.getById(id), 'comboBox');
  const names: string[] = [];
  const childCount = await combo.getChildCount();
  for (let index = 0; index < childCount; index += 1) {
    const child = await combo.childAt(index);
    expect(child).toBeDefined();
    names.push((await child?.info())?.name ?? '');
  }
  return names;
};

const findDescendantByName = async (
  root: GtkWidgetElement,
  kind: GtkWidgetElement['kind'],
  name: string
): Promise<GtkWidgetElement | undefined> => {
  const info = await root.info();
  if (info.kind === kind && info.name === name) {
    return root;
  }
  if (!('getChildCount' in root) || !('childAt' in root)) {
    return undefined;
  }
  const childCount = await root.getChildCount();
  for (let index = 0; index < childCount; ++index) {
    const child = await root.childAt(index);
    if (child === undefined) {
      continue;
    }
    const match = await findDescendantByName(child, kind, name);
    if (match !== undefined) {
      return match;
    }
  }
  return undefined;
};

const expectPageLabels = async (
  app: GtkApp,
  pageId: string,
  expectedLabels: readonly string[]
): Promise<void> => {
  const page = await app.getById(pageId);
  for (const label of expectedLabels) {
    expect(
      await findDescendantByName(page, 'label', label),
      `${pageId}: ${label}`
    ).toBeDefined();
  }
};

const findWindowByName = async (
  app: GtkApp,
  name: string
): Promise<GtkWidgetElement | undefined> => {
  const windowCount = await app.getWindowCount();
  for (let index = 0; index < windowCount; ++index) {
    const window = await app.windowAt(index);
    if (window !== undefined && (await window.info()).name === name) {
      return window;
    }
  }
  return undefined;
};

const chooseNamedColor = async (
  app: GtkApp,
  pickerId: string,
  colorName: string
): Promise<void> => {
  await expectElementKind(await app.getById(pickerId), 'button').click();
  const dialog = await waitForResult(async () => {
    const window = await findWindowByName(app, 'Pick a Color');
    if (window === undefined) {
      throw new Error('color picker dialog is not open');
    }
    return window;
  });
  await expectElementKind(
    await findDescendantByName(dialog, 'radio', colorName),
    'radio'
  ).click();
  await expectElementKind(
    await findDescendantByName(dialog, 'button', 'Select'),
    'button'
  ).click();
  await waitForResult(async () => {
    expect(await findWindowByName(app, 'Pick a Color')).toBeUndefined();
  });
};

const confirmSelectedFont = async (
  app: GtkApp,
  pickerId: string,
  dialogName: string
): Promise<void> => {
  await expectElementKind(await app.getById(pickerId), 'button').click();
  const dialog = await waitForResult(async () => {
    const window = await findWindowByName(app, dialogName);
    if (window === undefined) {
      throw new Error('font chooser dialog is not open');
    }
    return window;
  });
  await expectElementKind(
    await findDescendantByName(dialog, 'button', 'Select'),
    'button'
  ).click();
  await waitForResult(async () => {
    expect(await findWindowByName(app, dialogName)).toBeUndefined();
  });
};

const visibleSettingsTabNames = async (
  app: GtkApp,
  idPrefix = 'settings'
): Promise<string[]> => {
  const notebook = expectElementKind(
    await app.getById(`${idPrefix}_notebook`),
    'tabList'
  );
  const names: string[] = [];
  const childCount = await notebook.getChildCount();
  for (let index = 0; index < childCount; ++index) {
    const tab = await notebook.childAt(index);
    if (tab === undefined) {
      continue;
    }
    const info = await tab.info();
    if (info.states.includes('showing')) {
      names.push(info.name);
    }
  }
  return names;
};

const selectSettingsTab = async (
  app: GtkApp,
  expectedName: string,
  idPrefix = 'settings'
): Promise<void> => {
  const notebook = expectElementKind(
    await app.getById(`${idPrefix}_notebook`),
    'tabList'
  );
  const childCount = await notebook.getChildCount();
  for (let index = 0; index < childCount; ++index) {
    const tab = await notebook.childAt(index);
    if (tab !== undefined && (await tab.info()).name === expectedName) {
      await notebook.selectChildAt(index);
      return;
    }
  }
  throw new Error(`settings tab was not found: ${expectedName}`);
};

const expectPageVisualFixture = async (
  app: GtkApp,
  testCase: SettingVisualCase,
  directory: string
): Promise<void> => {
  const window = expectElementKind(
    await app.getById('settings_widget_test_window'),
    'window'
  );
  const windowBounds = await window.bounds();
  await app.input.moveMouseTo(
    windowBounds.x + 5,
    windowBounds.y + windowBounds.height - 5
  );
  const capture = await captureWhenVisuallyStable(
    await app.getById(testCase.pageId),
    testCase.fixtureName
  );
  expect(capture.clipped).toBe(false);
  await expectCaptureToMatchFixture(
    capture,
    testCase.fixtureName,
    fixturePath(testCase.fixtureName),
    directory,
    visualComparisonOptions
  );
  if (testCase.differsFrom !== undefined) {
    const diffPixels = await countCaptureFixtureDiffPixels(
      capture,
      fixturePath(testCase.differsFrom)
    );
    expect(diffPixels, testCase.fixtureName).toBeGreaterThan(0);
  }
};

const captureWhenVisuallyStable = async (
  page: GtkWidgetElement,
  description: string
): Promise<GtkCapture> => {
  let previousImage: Buffer | undefined;
  let consecutiveStableCaptures = 0;
  return waitForResult<GtkCapture>(
    async () => {
      const currentCapture = await page.capture();
      if (
        previousImage !== undefined &&
        previousImage.equals(currentCapture.image)
      ) {
        consecutiveStableCaptures += 1;
      } else {
        consecutiveStableCaptures = 0;
      }
      previousImage = currentCapture.image;
      expect(consecutiveStableCaptures).toBeGreaterThanOrEqual(2);
      return currentCapture;
    },
    {
      timeoutMs: 2_000,
      intervalMs: 50,
      message: `settings page did not become visually stable: ${description}`,
    }
  );
};

const parseAppliedStore = (line: string): AppliedStore => {
  const values = Object.fromEntries(
    line
      .trim()
      .split(/\s+/)
      .slice(1)
      .map((entry) => {
        const separator = entry.indexOf('=');
        return [entry.slice(0, separator), entry.slice(separator + 1)];
      })
  );
  return values as unknown as AppliedStore;
};

const waitForAppliedStore = async (app: GtkApp): Promise<AppliedStore> =>
  waitForPrintedStore(app, 'APPLIED');

const waitForPrintedStore = async (
  app: GtkApp,
  prefix: 'APPLIED' | 'REBASED' | 'SAVED'
): Promise<AppliedStore> =>
  waitForResult(async () => {
    const output = await app.output();
    const line = output.stdout
      .split('\n')
      .reverse()
      .find((candidate) => candidate.startsWith(`${prefix} `));
    expect(line).toBeDefined();
    return parseAppliedStore(line as string);
  });

const waitForEntryPlaceholder = async (
  app: GtkApp,
  id: string,
  expected: string
): Promise<void> => {
  await waitForResult(async () => {
    const prefix = `PLACEHOLDER ${id}=`;
    const line = (await app.output()).stdout
      .split('\n')
      .reverse()
      .find((candidate) => candidate.startsWith(prefix));
    expect(line?.slice(prefix.length)).toBe(expected);
  });
};

const waitForEntryIconTooltip = async (
  app: GtkApp,
  id: string,
  expected: string
): Promise<void> => {
  await waitForResult(async () => {
    const prefix = `ICON_TOOLTIP ${id}=`;
    const line = (await app.output()).stdout
      .split('\n')
      .reverse()
      .find((candidate) => candidate.startsWith(prefix));
    expect(line?.slice(prefix.length)).toBe(expected);
  });
};

const waitForChangedState = async (
  app: GtkApp,
  expected: string
): Promise<void> => {
  await waitForResult(async () => {
    expect((await app.output()).stdout).toContain(expected);
  });
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
  await waitForResult(async () => {
    expect((await widget.info()).states).toContain('focused');
  });
};

const expectEntryText = async (
  entry: GtkEntryElement,
  expected: string
): Promise<void> => {
  await waitForResult(async () => {
    expect(await entry.text()).toBe(expected);
  });
};

const expectNumericEntryValue = async (
  entry: GtkEntryElement,
  expected: number
): Promise<void> => {
  await waitForResult(async () => {
    expect(Number(await entry.text())).toBeCloseTo(expected);
  });
};

const setNumericEntryValue = async (
  entry: GtkEntryElement,
  value: number
): Promise<void> => {
  await entry.setText(String(value));
};

const expectInheritedEntry = async (
  app: GtkApp,
  id: string,
  placeholder: string
): Promise<GtkEntryElement> => {
  const entry = expectElementKind(await app.getById(id), 'entry');
  expect(await entry.text()).toBe('');
  await waitForEntryPlaceholder(app, id, placeholder);
  return entry;
};

const captureKeyBinding = async (
  app: GtkApp,
  entry: GtkEntryElement,
  modifiers: readonly GtkKeyboardModifier[],
  key: GtkKeyInput
): Promise<void> => {
  await clickWidget(app, entry);
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

const clearKeyBinding = async (
  app: GtkApp,
  entry: GtkEntryElement,
  blurTarget: GtkWidgetElement,
  resetButtonId: string
): Promise<void> => {
  await waitForResult(async () => {
    await clickWidget(app, entry);
    const { bounds } = await entry.capture();
    await app.input.moveMouseTo(
      Math.trunc(bounds.x + bounds.width - 18),
      Math.trunc(bounds.y + bounds.height / 2)
    );
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    await clickWidget(app, blurTarget);
    expect(await entry.text()).toBe('');
    await expectSensitive(
      expectElementKind(await app.getById(resetButtonId), 'button')
    );
  });
};

describe.concurrent('shared settings widget', () => {
  it('orders connection settings before Terminal when available', async (context) => {
    const cases = [
      {
        args: [] as const,
        expected: ['General', 'Terminal', 'Transfer', 'Logging', 'Macro'],
      },
      {
        args: ['--type=telnet'] as const,
        expected: [
          'General',
          'TELNET',
          'Terminal',
          'Transfer',
          'Logging',
          'Macro',
        ],
      },
      {
        args: ['--type=serial'] as const,
        expected: [
          'General',
          'Serial',
          'Terminal',
          'Transfer',
          'Logging',
          'Macro',
        ],
      },
      {
        args: ['--type=ssh'] as const,
        expected: [
          'General',
          'SSH',
          'Terminal',
          'Transfer',
          'Logging',
          'Macro',
        ],
      },
      {
        args: ['--type=sftp'] as const,
        expected: ['General', 'SSH', 'SFTP'],
      },
    ] as const;

    for (const testCase of cases) {
      await runSharedGtkTest(context, testCase.args, async ({ app }) => {
        expect(await visibleSettingsTabNames(app)).toEqual(testCase.expected);
      });
    }
  });

  it('adds, validates, edits, and reorders connection macros', async (context) => {
    await runSharedGtkTest(context, ['--page=macro'], async ({ app }) => {
      await selectSettingsTab(app, 'Macro');
      const add = expectElementKind(
        await app.getById('settings_macro_add_button'),
        'button'
      );
      const apply = expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      );

      await add.click();
      await expectInsensitive(apply);
      const id = expectElementKind(
        await app.getById('settings_macro_id_entry'),
        'entry'
      );
      const regex = expectElementKind(
        await app.getById('settings_macro_regex_entry'),
        'entry'
      );
      expect(await id.text()).toBe('rule1');
      await id.setText('first');
      await regex.setText('^READY (?<value>.+)$');
      const action = expectElementKind(
        await app.getById('settings_macro_action_combo'),
        'comboBox'
      );
      await action.selectChildAt(1);
      const command = expectElementKind(
        await app.getById('settings_macro_command_entry'),
        'entry'
      );
      await command.setText('notify-send');
      await expectSensitive(apply);

      const addArgument = expectElementKind(
        await app.getById('settings_macro_argument_add_button'),
        'button'
      );
      await addArgument.click();
      const argument = await waitForResult(async () =>
        expectElementKind(
          await app.getById('settings_macro_argument_0_entry'),
          'entry'
        )
      );
      await argument.setText('${value}');

      await add.click();
      await expectInsensitive(apply);
      await regex.setText('SECOND');
      await action.selectChildAt(1);
      await command.setText('true');
      await expectSensitive(apply);
      await expectElementKind(
        await app.getById('settings_macro_move_up_button'),
        'button'
      ).click();
      await apply.click();

      const store = await waitForAppliedStore(app);
      expect(store.macro_count).toBe('2');
      expect(store.macro_ids).toBe('rule2,first');
    });
  });

  it('presents every settings key with a user-facing label', async (context) => {
    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=general'],
      async ({ app }) => {
        const pages = [
          {
            id: 'global_settings_general_page',
            labels: [
              'Connection type',
              'Display language',
              'Startup mode',
              'Open application shortcut',
              'Title and status bar background',
              'Content background',
            ],
          },
          {
            id: 'global_settings_telnet_page',
            labels: ['Address', 'Port', 'Terminal type'],
          },
          {
            id: 'global_settings_ssh_page',
            labels: [
              'Address',
              'Port',
              'User name',
              'Identity file',
              'Terminal type',
            ],
          },
          {
            id: 'global_settings_sftp_page',
            labels: ['Local directory', 'Remote directory'],
          },
          {
            id: 'global_settings_serial_page',
            labels: [
              'Device identification',
              'Device',
              'Stable ID',
              'USB serial number',
              'Current device node',
              'Baud rate',
              'Data bits',
              'Parity',
              'Stop bits',
              'Flow control',
              'Connection monitoring signal',
            ],
          },
          {
            id: 'global_settings_terminal_page',
            labels: [
              'Character encoding',
              'Backspace code',
              'Cursor key mode',
              'Columns',
              'Rows',
              'Scrollback lines',
              'Zoom factor',
              'Primary font family',
              'Secondary font family',
              'Close window when session ends',
              'Zoom in shortcut',
              'Zoom out shortcut',
              'Send BREAK shortcut',
            ],
          },
          {
            id: 'global_settings_transfer_page',
            labels: [
              'Transfer base directory',
              'Text send rate (bytes/s)',
              'Follow Enter/Return code for text send',
              'Automatically start ZMODEM transfers',
            ],
          },
          {
            id: 'global_settings_logging_page',
            labels: [
              'Enable logging',
              'Log directory',
              'File name format',
              'Log content',
            ],
          },
        ] as const;

        for (const page of pages) {
          await expectPageLabels(app, page.id, page.labels);
        }
      }
    );

    await runSharedGtkTest(context, ['--page=general'], async ({ app }) => {
      await expectPageLabels(app, 'settings_general_page', [
        'Connection name',
        'Connection type',
        'Open connection shortcut',
      ]);
    });
  }, 90_000);

  it('localizes all settings presentation text into Japanese', async (context) => {
    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=general'],
      async ({ app }) => {
        expect(await visibleSettingsTabNames(app, 'global_settings')).toEqual([
          '一般',
          'TELNET',
          'シリアル',
          'SSH',
          'SFTP',
          '端末',
          '転送',
          'ログ',
        ]);
        const pages = [
          {
            id: 'global_settings_general_page',
            labels: [
              '接続方式',
              '表示言語',
              '起動モード',
              'アプリケーションを開くショートカット',
              'タイトル／ステータスバーの背景',
              'コンテンツの背景',
            ],
          },
          {
            id: 'global_settings_telnet_page',
            labels: ['アドレス', 'ポート', '端末種別'],
          },
          {
            id: 'global_settings_ssh_page',
            labels: [
              'アドレス',
              'ポート',
              'ユーザー名',
              '秘密鍵ファイル',
              '端末種別',
            ],
          },
          {
            id: 'global_settings_sftp_page',
            labels: ['ローカルディレクトリ', 'リモートディレクトリ'],
          },
          {
            id: 'global_settings_serial_page',
            labels: [
              'デバイス識別方式',
              'デバイス',
              '安定ID',
              'USBシリアル番号',
              '現在のデバイスノード',
              'ボーレート',
              'データビット',
              'パリティ',
              'ストップビット',
              'フロー制御',
              '接続監視信号',
            ],
          },
          {
            id: 'global_settings_terminal_page',
            labels: [
              '文字エンコーディング',
              'Backspaceコード',
              'カーソルキーモード',
              'Enter/Returnコード',
              '列数',
              '行数',
              'スクロールバッファ行数',
              '拡大率',
              'プライマリフォントファミリー',
              'セカンダリフォントファミリー',
              'セッション終了時にウィンドウを閉じる',
              '拡大ショートカット',
              '縮小ショートカット',
              'BREAK送信ショートカット',
            ],
          },
          {
            id: 'global_settings_transfer_page',
            labels: [
              '転送ベースディレクトリ',
              'テキスト送信速度（バイト/秒）',
              'テキスト送信時にEnter/Returnコードに従う',
              'ZMODEM転送を自動開始する',
            ],
          },
          {
            id: 'global_settings_logging_page',
            labels: [
              'ログを有効にする',
              'ログディレクトリ',
              'ファイル名形式',
              'ログ内容',
            ],
          },
        ] as const;

        for (const page of pages) {
          await expectPageLabels(app, page.id, page.labels);
        }
        const primaryFontMode = expectElementKind(
          await app.getById('global_settings_terminal_font_primary_mode_combo'),
          'comboBox'
        );
        const fallbackFontMode = expectElementKind(
          await app.getById(
            'global_settings_terminal_font_fallback_mode_combo'
          ),
          'comboBox'
        );
        expect(primaryFontMode.kind).toBe('comboBox');
        expect(fallbackFontMode.kind).toBe('comboBox');
        await expectSelectedComboValue(
          app,
          'global_settings_terminal_font_primary_mode_combo',
          '組み込み既定値'
        );
        await expectSelectedComboValue(
          app,
          'global_settings_terminal_font_fallback_mode_combo',
          '組み込み既定値'
        );
        expect(
          await comboOptionNames(
            app,
            'global_settings_terminal_font_primary_mode_combo'
          )
        ).toEqual([
          '組み込み既定値',
          '組み込み既定値を使用',
          'カスタムフォント',
        ]);
        expect(
          (await (await app.getById('global_settings_apply_button')).info())
            .name
        ).toBe('適用');
        expect(
          (await (await app.getById('global_settings_cancel_button')).info())
            .name
        ).toBe('キャンセル');
      },
      { env: japaneseTestEnvironment }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=serial',
        '--type=serial',
        '--serial-parity=e',
        '--serial-flow-control=xon',
        '--serial-carrier-detect=ignore',
      ],
      async ({ app }) => {
        await showSerialPage(app);
        await expectSelectedComboValue(
          app,
          'settings_serial_parity_combo',
          '偶数'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_flow_control_combo',
          'XON/XOFF（ソフトウェア）'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_carrier_detect_combo',
          '監視しない'
        );

        await selectSettingsTab(app, '端末');
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_width_entry',
          '80（組み込み既定値）'
        );
        await width.setText('invalid');
        await waitForEntryIconTooltip(
          app,
          'settings_terminal_width_entry',
          '整数を入力してください'
        );
        expect(
          (
            await (
              await app.getById('settings_terminal_zoom_in_key_reset_button')
            ).info()
          ).name
        ).toBe('リセット');
      },
      { env: japaneseTestEnvironment }
    );
  }, 90_000);

  it('exposes launcher draft state without its internal action row', async (context) => {
    await runSharedGtkTest(
      context,
      ['--hide-actions', '--page=terminal'],
      async ({ app }) => {
        await showTerminalPage(app);
        await expect(app.getById('settings_action_row')).rejects.toThrow();

        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await setNumericEntryValue(width, 91);
        await waitForChangedState(
          app,
          'CHANGED dirty=true valid=true width=91'
        );
      }
    );
  });

  it('matches General visual fixtures for connection type and runtime state', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=general'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_general_name_entry'),
              'entry'
            ).text()
          ).toBe('fixture');
          await expectSelectedConnectionType(
            app,
            'Local shell (built-in default)'
          );
          await expectSensitive(
            await app.getById('settings_general_type_combo')
          );
          await expectInheritedEntry(
            app,
            'settings_general_open_connection_entry',
            'Disabled (built-in default)'
          );
        },
        differsFrom: undefined,
        fixtureName: 'settings-widget-general-type-local-editable',
        pageId: 'settings_notebook',
        prepare: stayOnInitialPage,
      },
      {
        args: ['--page=general', '--type=telnet'],
        assert: async (app) => {
          await expectSelectedConnectionType(app, 'TELNET');
          await expectSensitive(
            await app.getById('settings_general_type_combo')
          );
          expectElementKind(
            await app.getById('settings_general_open_connection_entry'),
            'entry'
          );
        },
        differsFrom: 'settings-widget-general-type-local-editable',
        fixtureName: 'settings-widget-general-type-telnet-editable',
        pageId: 'settings_notebook',
        prepare: stayOnInitialPage,
      },
      {
        args: ['--runtime'],
        assert: async (app) => {
          await expectSelectedConnectionType(
            app,
            'Local shell (built-in default)'
          );
          await expectInsensitive(
            await app.getById('settings_general_type_combo')
          );
          await expect(
            app.getById('settings_general_open_connection_entry')
          ).rejects.toThrow();
        },
        differsFrom: 'settings-widget-general-type-local-editable',
        fixtureName: 'settings-widget-general-type-local-runtime',
        pageId: 'settings_notebook',
        prepare: stayOnInitialPage,
      },
      {
        args: ['--runtime', '--type=telnet'],
        assert: async (app) => {
          await expectSelectedConnectionType(app, 'TELNET');
          await expectInsensitive(
            await app.getById('settings_general_type_combo')
          );
          await expect(
            app.getById('settings_general_open_connection_entry')
          ).rejects.toThrow();
        },
        differsFrom: 'settings-widget-general-type-telnet-editable',
        fixtureName: 'settings-widget-general-type-telnet-runtime',
        pageId: 'settings_notebook',
        prepare: stayOnInitialPage,
      },
    ];

    for (const testCase of cases) {
      await runSharedGtkTest(
        context,
        testCase.args,
        async ({ app, directory }) => {
          await testCase.prepare(app);
          await testCase.assert(app);
          await expectPageVisualFixture(app, testCase, directory);
        }
      );
    }
  }, 60_000);

  it('edits the explicit connection name shown on General', async (context) => {
    await runSharedGtkTest(context, [], async ({ app }) => {
      const name = expectElementKind(
        await app.getById('settings_general_name_entry'),
        'entry'
      );
      expect(await name.text()).toBe('fixture');
      await name.setText('Tokyo/Lab');
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();
      expect((await waitForAppliedStore(app)).name).toBe('Tokyo/Lab');
    });
  });

  it('edits, validates, and resets the connection launch hotkey', async (context) => {
    await runSharedGtkTest(context, ['--page=general'], async ({ app }) => {
      const hotkey = expectElementKind(
        await app.getById('settings_general_open_connection_entry'),
        'entry'
      );
      await captureKeyBinding(app, hotkey, [], 'y');
      await expectEntryText(hotkey, 'y');
      await expectInsensitive(await app.getById('settings_apply_button'));

      await captureKeyBinding(app, hotkey, ['control', 'shift'], 'y');
      await expectEntryText(hotkey, 'ctrl+shift+y');
      await expectSensitive(await app.getById('settings_apply_button'));
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();
      const configured = await waitForAppliedStore(app);
      expect(configured.open_connection).toBe('ctrl+shift+y');
      expect(configured.open_connection_explicit).toBe('true');
    });

    await runSharedGtkTest(
      context,
      ['--page=general', '--open-connection=ctrl+shift+x'],
      async ({ app }) => {
        const hotkey = expectElementKind(
          await app.getById('settings_general_open_connection_entry'),
          'entry'
        );
        expect(await hotkey.text()).toBe('ctrl+shift+x');
        await expectElementKind(
          await app.getById('settings_general_open_connection_reset_button'),
          'button'
        ).click();
        await waitForEntryPlaceholder(
          app,
          'settings_general_open_connection_entry',
          'Disabled (built-in default)'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const reset = await waitForAppliedStore(app);
        expect(reset.open_connection).toBe('');
        expect(reset.open_connection_explicit).toBe('false');
      }
    );
  });

  it('matches Terminal visual fixtures for each terminal setting value', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=terminal'],
        assert: async (app) => {
          await expectInheritedEntry(
            app,
            'settings_terminal_encoding_entry',
            'UTF-8 (built-in default)'
          );
          await expectSelectedComboValue(
            app,
            'settings_terminal_backspace_code_combo',
            'Auto (built-in default)'
          );
          await expectSelectedComboValue(
            app,
            'settings_terminal_cursor_key_mode_combo',
            'Normal (built-in default)'
          );
          await expectSelectedComboValue(
            app,
            'settings_terminal_return_code_combo',
            'Auto (built-in default)'
          );
          const width = expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          );
          expect(await width.text()).toBe('');
          await waitForEntryPlaceholder(
            app,
            'settings_terminal_width_entry',
            '80 (built-in default)'
          );
          await waitForEntryPlaceholder(
            app,
            'settings_terminal_scrollback_lines_entry',
            '10000 (built-in default)'
          );
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_zoom_in_key_entry'),
              'entry'
            ).text()
          ).toBe('');
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_zoom_out_key_entry'),
              'entry'
            ).text()
          ).toBe('');
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_send_break_key_entry'),
              'entry'
            ).text()
          ).toBe('');
          await waitForEntryPlaceholder(
            app,
            'settings_terminal_send_break_key_entry',
            'Disabled (built-in default)'
          );
        },
        differsFrom: undefined,
        fixtureName: 'settings-widget-terminal-page-default',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--width=88'],
        assert: async (app) => {
          await expectNumericEntryValue(
            expectElementKind(
              await app.getById('settings_terminal_width_entry'),
              'entry'
            ),
            88
          );
        },
        differsFrom: 'settings-widget-terminal-page-default',
        fixtureName: 'settings-widget-terminal-width-88',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--height=31'],
        assert: async (app) => {
          await expectNumericEntryValue(
            expectElementKind(
              await app.getById('settings_terminal_height_entry'),
              'entry'
            ),
            31
          );
        },
        differsFrom: 'settings-widget-terminal-page-default',
        fixtureName: 'settings-widget-terminal-height-31',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--zoom=1.25'],
        assert: async (app) => {
          await expectNumericEntryValue(
            expectElementKind(
              await app.getById('settings_terminal_zoom_entry'),
              'entry'
            ),
            1.25
          );
        },
        differsFrom: 'settings-widget-terminal-page-default',
        fixtureName: 'settings-widget-terminal-zoom-1.25',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--auto-close=false'],
        assert: async (app) => {
          await expectSelectedComboValue(
            app,
            'settings_terminal_auto_close_combo',
            'Disabled'
          );
        },
        differsFrom: 'settings-widget-terminal-page-default',
        fixtureName: 'settings-widget-terminal-auto-close-false',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
    ];

    for (const testCase of cases) {
      await runSharedGtkTest(
        context,
        testCase.args,
        async ({ app, directory }) => {
          await testCase.prepare(app);
          await testCase.assert(app);
          await expectPageVisualFixture(app, testCase, directory);
        }
      );
    }
  });

  it('edits inherited, uncolored, and custom General backgrounds with RGB pickers', async (context) => {
    await runSharedGtkTest(context, ['--page=general'], async ({ app }) => {
      const generalPage = await app.getById('settings_general_page');
      const terminalPage = await app.getById('settings_terminal_page');
      const generalIds = await descendantAccessibleIds(generalPage);
      const terminalIds = await descendantAccessibleIds(terminalPage);
      expect(generalIds).toContain(
        'settings_general_exterior_background_mode_combo'
      );
      expect(generalIds).toContain('settings_general_background_mode_combo');
      expect(terminalIds).not.toContain(
        'settings_general_exterior_background_mode_combo'
      );
      expect(terminalIds).not.toContain(
        'settings_general_background_mode_combo'
      );

      const exteriorPicker = await app.getById(
        'settings_general_exterior_background_button'
      );
      const backgroundPicker = await app.getById(
        'settings_general_background_button'
      );
      await expectSelectedComboValue(
        app,
        'settings_general_exterior_background_mode_combo',
        'No color (built-in default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_general_background_mode_combo',
        'No color (built-in default)'
      );
      await waitForEntryPlaceholder(
        app,
        'settings_telnet_terminal_type_entry',
        'xterm-256color (built-in default)'
      );
      await waitForEntryPlaceholder(
        app,
        'settings_ssh_terminal_type_entry',
        'xterm-256color (built-in default)'
      );
      await expectSensitive(exteriorPicker);
      await expectSensitive(backgroundPicker);
      await waitForResult(async () => {
        expect((await app.output()).stdout).toContain(
          'COLOR_PICKERS exterior_use_alpha=false background_use_alpha=false'
        );
      });

      await chooseNamedColor(
        app,
        'settings_general_exterior_background_button',
        'Red'
      );
      await chooseNamedColor(app, 'settings_general_background_button', 'Blue');
      await expectSelectedComboValue(
        app,
        'settings_general_exterior_background_mode_combo',
        'Custom color'
      );
      await expectSelectedComboValue(
        app,
        'settings_general_background_mode_combo',
        'Custom color'
      );
      await waitForEntryPlaceholder(
        app,
        'settings_telnet_terminal_type_entry',
        'xterm (built-in default)'
      );
      await waitForEntryPlaceholder(
        app,
        'settings_ssh_terminal_type_entry',
        'xterm (built-in default)'
      );

      const backgroundMode = expectElementKind(
        await app.getById('settings_general_background_mode_combo'),
        'comboBox'
      );
      await backgroundMode.selectChildAt(1);
      await waitForEntryPlaceholder(
        app,
        'settings_telnet_terminal_type_entry',
        'xterm-256color (built-in default)'
      );
      await waitForEntryPlaceholder(
        app,
        'settings_ssh_terminal_type_entry',
        'xterm-256color (built-in default)'
      );
      await backgroundMode.selectChildAt(2);
      await waitForEntryPlaceholder(
        app,
        'settings_telnet_terminal_type_entry',
        'xterm (built-in default)'
      );
      await waitForEntryPlaceholder(
        app,
        'settings_ssh_terminal_type_entry',
        'xterm (built-in default)'
      );
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();

      const store = await waitForAppliedStore(app);
      expect(store.exterior_background).toBe('#E01B24');
      expect(store.exterior_background_source).toBe('override');
      expect(store.exterior_background_explicit).toBe('true');
      expect(store.background).toBe('#3584E4');
      expect(store.background_source).toBe('override');
      expect(store.background_explicit).toBe('true');
    });

    await runSharedGtkTest(
      context,
      [
        '--page=general',
        '--global=general.exterior_background=#112233',
        '--global=general.background=#445566',
        '--global=telnet.terminal_type=vt220',
        '--global=ssh.terminal_type=ansi',
        '--exterior-background=none',
        '--background=#778899',
      ],
      async ({ app }) => {
        const exteriorMode = expectElementKind(
          await app.getById('settings_general_exterior_background_mode_combo'),
          'comboBox'
        );
        const backgroundMode = expectElementKind(
          await app.getById('settings_general_background_mode_combo'),
          'comboBox'
        );
        await expectSelectedComboValue(
          app,
          'settings_general_exterior_background_mode_combo',
          'No color'
        );
        await expectSelectedComboValue(
          app,
          'settings_general_background_mode_combo',
          'Custom color'
        );
        await expectSensitive(
          await app.getById('settings_general_exterior_background_button')
        );
        await expectSensitive(
          await app.getById('settings_general_background_button')
        );
        await waitForEntryPlaceholder(
          app,
          'settings_telnet_terminal_type_entry',
          'vt220 (global default)'
        );
        await waitForEntryPlaceholder(
          app,
          'settings_ssh_terminal_type_entry',
          'ansi (global default)'
        );

        await exteriorMode.selectChildAt(0);
        await backgroundMode.selectChildAt(0);
        await expectSelectedComboValue(
          app,
          'settings_general_exterior_background_mode_combo',
          'Custom color (global default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_general_background_mode_combo',
          'Custom color (global default)'
        );
        await expectSensitive(
          await app.getById('settings_general_exterior_background_button')
        );
        await expectSensitive(
          await app.getById('settings_general_background_button')
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.exterior_background).toBe('#112233');
        expect(store.exterior_background_source).toBe('global');
        expect(store.exterior_background_explicit).toBe('false');
        expect(store.background).toBe('#445566');
        expect(store.background_source).toBe('global');
        expect(store.background_explicit).toBe('false');
        expect(store.telnet_terminal_type).toBe('vt220');
        expect(store.telnet_terminal_type_explicit).toBe('false');
        expect(store.ssh_terminal_type).toBe('ansi');
        expect(store.ssh_terminal_type_explicit).toBe('false');
      }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=general',
        '--telnet-terminal-type=screen',
        '--ssh-terminal-type=vt100',
      ],
      async ({ app }) => {
        await chooseNamedColor(
          app,
          'settings_general_background_button',
          'Blue'
        );
        expect(
          await expectElementKind(
            await app.getById('settings_telnet_terminal_type_entry'),
            'entry'
          ).text()
        ).toBe('screen');
        expect(
          await expectElementKind(
            await app.getById('settings_ssh_terminal_type_entry'),
            'entry'
          ).text()
        ).toBe('vt100');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.telnet_terminal_type).toBe('screen');
        expect(store.telnet_terminal_type_explicit).toBe('true');
        expect(store.ssh_terminal_type).toBe('vt100');
        expect(store.ssh_terminal_type_explicit).toBe('true');
      }
    );
  }, 90_000);

  it('matches the default Logging visual fixture', async (context) => {
    const testCase: SettingVisualCase = {
      args: ['--page=logging'],
      assert: async (app) => {
        await expectSelectedComboValue(
          app,
          'settings_log_enabled_combo',
          'Disabled (built-in default)'
        );
        expect(
          await expectElementKind(
            await app.getById('settings_log_base_directory_entry'),
            'entry'
          ).text()
        ).toBe('');
        expect(
          await expectElementKind(
            await app.getById('settings_log_file_name_format_entry'),
            'entry'
          ).text()
        ).toBe('');
        await expectSelectedComboValue(
          app,
          'settings_log_mode_combo',
          'Raw bytes (before character conversion) (built-in default)'
        );
      },
      differsFrom: undefined,
      fixtureName: 'settings-widget-logging-page-default',
      pageId: 'settings_logging_page',
      prepare: showLoggingPage,
    };

    await runSharedGtkTest(
      context,
      testCase.args,
      async ({ app, directory }) => {
        await testCase.prepare(app);
        await testCase.assert(app);
        await expectPageVisualFixture(app, testCase, directory);
      }
    );
  });

  it('matches TELNET visual fixtures for each TELNET setting and runtime state', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=telnet', '--type=telnet'],
        assert: async (app) => {
          const address = expectElementKind(
            await app.getById('settings_telnet_address_entry'),
            'entry'
          );
          const port = expectElementKind(
            await app.getById('settings_telnet_port_entry'),
            'entry'
          );
          const terminalType = expectElementKind(
            await app.getById('settings_telnet_terminal_type_entry'),
            'entry'
          );
          expect(await address.text()).toBe('');
          expect(await port.text()).toBe('');
          expect(await terminalType.text()).toBe('');
          await waitForEntryPlaceholder(
            app,
            'settings_telnet_address_entry',
            'Built-in default'
          );
          await waitForEntryPlaceholder(
            app,
            'settings_telnet_port_entry',
            '23 (built-in default)'
          );
          await waitForEntryPlaceholder(
            app,
            'settings_telnet_terminal_type_entry',
            'xterm-256color (built-in default)'
          );
          await expectSensitive(address);
          await expectSensitive(port);
          await expectSensitive(terminalType);
        },
        differsFrom: undefined,
        fixtureName: 'settings-widget-telnet-page-default-editable',
        pageId: 'settings_telnet_page',
        prepare: showTelnetPage,
      },
      {
        args: [
          '--page=telnet',
          '--type=telnet',
          '--telnet-address=address.example',
        ],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_telnet_address_entry'),
              'entry'
            ).text()
          ).toBe('address.example');
        },
        differsFrom: 'settings-widget-telnet-page-default-editable',
        fixtureName: 'settings-widget-telnet-address-address.example',
        pageId: 'settings_telnet_page',
        prepare: showTelnetPage,
      },
      {
        args: ['--page=telnet', '--type=telnet', '--telnet-port=2323'],
        assert: async (app) => {
          await expectNumericEntryValue(
            expectElementKind(
              await app.getById('settings_telnet_port_entry'),
              'entry'
            ),
            2323
          );
        },
        differsFrom: 'settings-widget-telnet-page-default-editable',
        fixtureName: 'settings-widget-telnet-port-2323',
        pageId: 'settings_telnet_page',
        prepare: showTelnetPage,
      },
      {
        args: [
          '--page=telnet',
          '--type=telnet',
          '--telnet-terminal-type=vt220',
        ],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_telnet_terminal_type_entry'),
              'entry'
            ).text()
          ).toBe('vt220');
        },
        differsFrom: 'settings-widget-telnet-page-default-editable',
        fixtureName: 'settings-widget-telnet-terminal-type-vt220',
        pageId: 'settings_telnet_page',
        prepare: showTelnetPage,
      },
      {
        args: ['--page=telnet', '--runtime', '--type=telnet'],
        assert: async (app) => {
          await expectInsensitive(
            await app.getById('settings_telnet_address_entry')
          );
          await expectInsensitive(
            await app.getById('settings_telnet_port_entry')
          );
          await expectInsensitive(
            await app.getById('settings_telnet_terminal_type_entry')
          );
        },
        differsFrom: 'settings-widget-telnet-page-default-editable',
        fixtureName: 'settings-widget-telnet-page-runtime',
        pageId: 'settings_telnet_page',
        prepare: showTelnetPage,
      },
    ];

    for (const testCase of cases) {
      await runSharedGtkTest(
        context,
        testCase.args,
        async ({ app, directory }) => {
          await testCase.prepare(app);
          await testCase.assert(app);
          await expectPageVisualFixture(app, testCase, directory);
        }
      );
    }
  });

  it('shows TELNET controls in editable mode and matches the visual fixture', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=telnet',
        '--type=telnet',
        '--telnet-address=example.test',
        '--telnet-port=2323',
        '--telnet-terminal-type=vt220',
      ],
      async ({ app, directory }) => {
        await showTelnetPage(app);

        const address = expectElementKind(
          await app.getById('settings_telnet_address_entry'),
          'entry'
        );
        const port = expectElementKind(
          await app.getById('settings_telnet_port_entry'),
          'entry'
        );
        const terminalType = expectElementKind(
          await app.getById('settings_telnet_terminal_type_entry'),
          'entry'
        );
        expect(await address.text()).toBe('example.test');
        await expectNumericEntryValue(port, 2323);
        expect(await terminalType.text()).toBe('vt220');
        await expectSensitive(address);
        await expectSensitive(port);
        await expectSensitive(terminalType);

        await terminalType.setText('ansi');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.telnet_terminal_type).toBe('ansi');

        const capture = await (
          await app.getById('settings_telnet_page')
        ).capture();
        expect(capture.clipped).toBe(false);
        expect(countNonBackgroundPixels(capture)).toBeGreaterThan(1000);
        await expectCaptureToMatchFixture(
          capture,
          'settings-widget-telnet-editable',
          telnetEditableFixturePath,
          directory,
          { maxDiffPixels: 0, threshold: 0.01 }
        );
      }
    );
  });

  it('resets a blank TELNET terminal type to the built-in default', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=telnet', '--type=telnet', '--telnet-terminal-type=vt220'],
      async ({ app }) => {
        await showTelnetPage(app);
        const terminalType = expectElementKind(
          await app.getById('settings_telnet_terminal_type_entry'),
          'entry'
        );
        await clickWidget(app, terminalType);
        await terminalType.setText('   ');
        await clickWidget(
          app,
          await app.getById('settings_telnet_address_entry')
        );
        await waitForResult(async () => {
          expect(await terminalType.text()).toBe('');
        });
        await waitForEntryPlaceholder(
          app,
          'settings_telnet_terminal_type_entry',
          'xterm-256color (built-in default)'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.telnet_terminal_type).toBe('xterm-256color');
        expect(store.telnet_terminal_type_explicit).toBe('false');
      }
    );
  });

  it('matches SSH visual fixtures for editable and runtime settings', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=ssh', '--type=ssh'],
        assert: async (app) => {
          const address = expectElementKind(
            await app.getById('settings_ssh_address_entry'),
            'entry'
          );
          const port = expectElementKind(
            await app.getById('settings_ssh_port_entry'),
            'entry'
          );
          const username = expectElementKind(
            await app.getById('settings_ssh_username_entry'),
            'entry'
          );
          const identity = expectElementKind(
            await app.getById('settings_ssh_identity_file_entry'),
            'entry'
          );
          const terminalType = expectElementKind(
            await app.getById('settings_ssh_terminal_type_entry'),
            'entry'
          );
          expect(await address.text()).toBe('');
          expect(await port.text()).toBe('');
          expect(await username.text()).toBe('');
          expect(await identity.text()).toBe('');
          expect(await terminalType.text()).toBe('');
          await waitForEntryPlaceholder(
            app,
            'settings_ssh_port_entry',
            '22 (built-in default)'
          );
          await waitForEntryPlaceholder(
            app,
            'settings_ssh_terminal_type_entry',
            'xterm-256color (built-in default)'
          );
          await expectSensitive(address);
          await expectSensitive(port);
          await expectSensitive(username);
          await expectSensitive(identity);
          await expectSensitive(terminalType);
        },
        differsFrom: undefined,
        fixtureName: 'settings-widget-ssh-page-default-editable',
        pageId: 'settings_ssh_page',
        prepare: showSshPage,
      },
      {
        args: ['--page=ssh', '--runtime', '--type=ssh'],
        assert: async (app) => {
          await expectInsensitive(
            await app.getById('settings_ssh_address_entry')
          );
          await expectInsensitive(await app.getById('settings_ssh_port_entry'));
          await expectInsensitive(
            await app.getById('settings_ssh_username_entry')
          );
          await expectInsensitive(
            await app.getById('settings_ssh_identity_file_entry')
          );
          await expectInsensitive(
            await app.getById('settings_ssh_terminal_type_entry')
          );
        },
        differsFrom: 'settings-widget-ssh-page-default-editable',
        fixtureName: 'settings-widget-ssh-page-runtime',
        pageId: 'settings_ssh_page',
        prepare: showSshPage,
      },
    ];

    for (const testCase of cases) {
      await runSharedGtkTest(
        context,
        testCase.args,
        async ({ app, directory }) => {
          await testCase.prepare(app);
          await testCase.assert(app);
          await expectPageVisualFixture(app, testCase, directory);
        }
      );
    }
  });

  it('applies SSH edits and resets a blank terminal type', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ssh',
        '--type=ssh',
        '--ssh-address=before.example',
        '--ssh-port=22',
        '--ssh-terminal-type=vt220',
      ],
      async ({ app }) => {
        await showSshPage(app);
        const address = expectElementKind(
          await app.getById('settings_ssh_address_entry'),
          'entry'
        );
        const port = expectElementKind(
          await app.getById('settings_ssh_port_entry'),
          'entry'
        );
        const username = expectElementKind(
          await app.getById('settings_ssh_username_entry'),
          'entry'
        );
        const identity = expectElementKind(
          await app.getById('settings_ssh_identity_file_entry'),
          'entry'
        );
        const terminalType = expectElementKind(
          await app.getById('settings_ssh_terminal_type_entry'),
          'entry'
        );

        await address.setText('after.example');
        await setNumericEntryValue(port, 2222);
        await username.setText('alice');
        await identity.setText('~/.ssh/id_test');
        await clickWidget(app, terminalType);
        await terminalType.setText('   ');
        await clickWidget(app, address);
        await waitForResult(async () => {
          expect(await terminalType.text()).toBe('');
        });
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.type).toBe('ssh');
        expect(store.ssh_address).toBe('after.example');
        expect(store.ssh_port).toBe('2222');
        expect(store.ssh_username).toBe('alice');
        expect(store.ssh_identity_file).toBe('~/.ssh/id_test');
        expect(store.ssh_terminal_type).toBe('xterm-256color');
        expect(store.backspace_code).toBe('auto');
      }
    );
  });

  it('shows shared SSH endpoint and applies SFTP directory edits', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=sftp',
        '--type=sftp',
        '--sftp-local-directory=/home/alice/uploads',
        '--sftp-remote-directory=/srv/incoming',
      ],
      async ({ app }) => {
        await showSftpPage(app);
        const localDirectory = expectElementKind(
          await app.getById('settings_sftp_local_directory_entry'),
          'entry'
        );
        const remoteDirectory = expectElementKind(
          await app.getById('settings_sftp_remote_directory_entry'),
          'entry'
        );
        expect(await localDirectory.text()).toBe('/home/alice/uploads');
        expect(await remoteDirectory.text()).toBe('/srv/incoming');
        await expectSensitive(localDirectory);
        await expectSensitive(remoteDirectory);

        await selectSettingsTab(app, 'SSH');
        await showSshPage(app);
        expect(
          (await (await app.getById('settings_ssh_terminal_type_entry')).info())
            .states
        ).not.toContain('showing');

        await selectSettingsTab(app, 'SFTP');
        await localDirectory.setText('/home/alice/outgoing');
        await remoteDirectory.setText('/opt/drop');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.type).toBe('sftp');
        expect(store.sftp_local_directory).toBe('/home/alice/outgoing');
        expect(store.sftp_remote_directory).toBe('/opt/drop');
      }
    );
  });

  it('shows Serial controls in editable mode and applies serial edits', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=serial',
        '--type=serial',
        '--serial-device=/dev/ttyUSB9',
        '--serial-baudrate=115200',
        '--serial-bits=7',
        '--serial-parity=e',
        '--serial-stop-bit=2',
        '--serial-flow-control=xon',
        '--serial-carrier-detect=ignore',
      ],
      async ({ app }) => {
        await showSerialPage(app);

        const device = expectElementKind(
          await app.getById('settings_serial_device_combo'),
          'comboBox'
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

        await expectSelectedComboValue(
          app,
          'settings_serial_device_combo',
          '/dev/ttyUSB9'
        );
        await expectNumericEntryValue(baudrate, 115200);
        await expectSelectedComboValue(app, 'settings_serial_bits_combo', '7');
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
          'Ignore (do not monitor)'
        );
        await expectSensitive(device);
        await expectSensitive(baudrate);
        await expectSensitive(bits);
        await expectSensitive(parity);
        await expectSensitive(stopBit);
        await expectSensitive(flowControl);
        await expectSensitive(carrierDetect);

        await setNumericEntryValue(baudrate, 57600);
        await bits.selectChildAt(4);
        await parity.selectChildAt(3);
        await stopBit.selectChildAt(1);
        await flowControl.selectChildAt(3);
        await carrierDetect.selectChildAt(2);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.type).toBe('serial');
        expect(store.serial_device).toBe('/dev/ttyUSB9');
        expect(store.serial_baudrate).toBe('57600');
        expect(store.serial_bits).toBe('8');
        expect(store.serial_parity).toBe('o');
        expect(store.serial_stop_bit).toBe('1');
        expect(store.serial_flow_control).toBe('hard');
        expect(store.serial_carrier_detect).toBe('cts');
      }
    );
  });

  it('enumerates serial devices, maps identification modes, and refreshes after hotplug', async (context) => {
    const fixture = await createSerialDeviceFixture();
    try {
      await runSharedGtkTest(
        context,
        ['--page=serial', '--type=serial', '--serial-match-mode=by-id'],
        async ({ app }) => {
          await showSerialPage(app);
          await expect(
            app.getById('settings_serial_device_entry')
          ).rejects.toThrow();

          const matchMode = expectElementKind(
            await app.getById('settings_serial_device_match_mode_combo'),
            'comboBox'
          );
          expect(
            await comboOptionNames(
              app,
              'settings_serial_device_match_mode_combo'
            )
          ).toEqual([
            'Stable device identity (built-in default)',
            'Device path',
            'Stable device identity',
            'Physical USB port',
          ]);

          const device = expectElementKind(
            await app.getById('settings_serial_device_combo'),
            'comboBox'
          );
          expect(
            await comboOptionNames(app, 'settings_serial_device_combo')
          ).toEqual([
            'No device (built-in default)',
            'Elder USB Demo [SN:FT12...1234]',
          ]);
          await device.selectChildAt(1);
          await expectSelectedComboValue(
            app,
            'settings_serial_device_combo',
            'Elder USB Demo [SN:FT12...1234]'
          );
          expect(
            (
              await (
                await app.getById('settings_serial_stable_id_value')
              ).info()
            ).name
          ).toBe(fixture.stableTarget);
          expect(
            (
              await (
                await app.getById('settings_serial_usb_serial_value')
              ).info()
            ).name
          ).toBe('FT12345678901234');
          expect(
            (
              await (
                await app.getById('settings_serial_current_node_value')
              ).info()
            ).name
          ).toBe('/dev/null');

          await mkdir(join(fixture.sysClassTtyRoot, 'zero', 'device'), {
            recursive: true,
          });
          await Promise.all([
            writeFile(
              join(fixture.sysClassTtyRoot, 'zero', 'device', 'product'),
              'Other USB\n'
            ),
            symlink('/dev/zero', join(fixture.byIdRoot, 'usb-other')),
          ]);
          await waitForResult(async () => {
            expect(
              await comboOptionNames(app, 'settings_serial_device_combo')
            ).toEqual([
              'No device (built-in default)',
              'Elder USB Demo [SN:FT12...1234]',
              'Other USB',
            ]);
          });

          await matchMode.selectChildAt(3);
          await expectSelectedComboValue(
            app,
            'settings_serial_device_match_mode_combo',
            'Physical USB port'
          );
          await expectSelectedComboValue(
            app,
            'settings_serial_device_combo',
            'pci-demo-usb-0'
          );
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          const store = await waitForAppliedStore(app);
          expect(store.serial_device_match_mode).toBe('by-path');
          expect(store.serial_device).toBe(fixture.physicalTarget);
          expect(store.serial_device_usb_serial).toBe('FT12345678901234');
        },
        { env: fixture.environment }
      );
    } finally {
      await rm(fixture.root, { force: true, recursive: true });
    }
  }, 60_000);

  it('shows Transfer controls and applies transfer edits', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=transfer',
        '--transfer-base-path=file:///tmp/elder-terms-transfer',
        '--text-send-bytes-per-second=4096',
        '--text-send-follow-return-code=disabled',
        '--zmodem-autostart=disabled',
      ],
      async ({ app }) => {
        await showTransferPage(app);

        const basePath = expectElementKind(
          await app.getById('settings_transfer_base_path_entry'),
          'entry'
        );
        const zmodemAutostart = expectElementKind(
          await app.getById('settings_transfer_zmodem_autostart_combo'),
          'comboBox'
        );
        const textSendRate = expectElementKind(
          await app.getById('settings_transfer_text_send_rate_entry'),
          'entry'
        );
        const followReturnCode = expectElementKind(
          await app.getById(
            'settings_transfer_text_send_follow_return_code_combo'
          ),
          'comboBox'
        );
        expect(await basePath.text()).toBe('file:///tmp/elder-terms-transfer');
        await expectNumericEntryValue(textSendRate, 4096);
        await expectSelectedComboValue(
          app,
          'settings_transfer_text_send_follow_return_code_combo',
          'Disabled'
        );
        await expectSelectedComboValue(
          app,
          'settings_transfer_zmodem_autostart_combo',
          'Disabled'
        );
        await expectSensitive(basePath);
        await expectSensitive(textSendRate);
        await expectSensitive(followReturnCode);
        await expectSensitive(zmodemAutostart);

        await basePath.setText('file:///tmp/elder-terms-downloads');
        await setNumericEntryValue(textSendRate, 2048);
        await followReturnCode.selectChildAt(1);
        await zmodemAutostart.selectChildAt(1);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.transfer_base_path).toBe(
          'file:///tmp/elder-terms-downloads'
        );
        expect(store.text_send_bytes_per_second).toBe('2048');
        expect(store.text_send_follow_return_code).toBe('enabled');
        expect(store.zmodem_autostart).toBe('enabled');
      }
    );
  });

  it('shows Logging controls, validates the format, and applies edits', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=logging',
        '--log-enabled=true',
        '--log-base-directory=/tmp/elder-terms-log',
        '--log-file-name-format={YYYY-MM-DD}/{hh:mm:ss}_{fff}.txt',
        '--log-mode=cooked',
        '--save',
      ],
      async ({ app }) => {
        await showLoggingPage(app);

        const enabled = expectElementKind(
          await app.getById('settings_log_enabled_combo'),
          'comboBox'
        );
        const baseDirectory = expectElementKind(
          await app.getById('settings_log_base_directory_entry'),
          'entry'
        );
        const fileNameFormat = expectElementKind(
          await app.getById('settings_log_file_name_format_entry'),
          'entry'
        );
        const mode = expectElementKind(
          await app.getById('settings_log_mode_combo'),
          'comboBox'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        await expectSelectedComboValue(
          app,
          'settings_log_enabled_combo',
          'Enabled'
        );
        expect(await baseDirectory.text()).toBe('/tmp/elder-terms-log');
        expect(await fileNameFormat.text()).toBe(
          '{YYYY-MM-DD}/{hh:mm:ss}_{fff}.txt'
        );
        await expectSelectedComboValue(
          app,
          'settings_log_mode_combo',
          'UTF-8 text (after character conversion)'
        );

        await fileNameFormat.setText('../outside.log');
        await waitForChangedState(app, 'CHANGED dirty=true valid=false');
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await fileNameFormat.setText(
          '{YYYY}-{MM}-{DD}/{name}_{hh}-{mm}-{ss}.txt'
        );
        await baseDirectory.setText('{documents}/elder-terms/{name}');
        await enabled.selectChildAt(2);
        await mode.selectChildAt(1);
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectSensitive(apply);
        await expectSensitive(save);
        await expectElementKind(apply, 'button').click();

        const store = await waitForAppliedStore(app);
        expect(store.log_enabled).toBe('false');
        expect(store.log_base_directory).toBe('{documents}/elder-terms/{name}');
        expect(store.log_file_name_format).toBe(
          '{YYYY}-{MM}-{DD}/{name}_{hh}-{mm}-{ss}.txt'
        );
        expect(store.log_mode).toBe('raw');
      }
    );
  });

  it('updates General and TELNET state with an Auto default', async (context) => {
    await runSharedGtkTest(context, [], async ({ app }) => {
      const combo = expectElementKind(
        await app.getById('settings_general_type_combo'),
        'comboBox'
      );
      await expectSensitive(combo);

      const telnetPage = await app.getById('settings_telnet_page');
      expect((await telnetPage.info()).states).not.toContain('visible');

      await combo.selectChildAt(2);
      await waitForResult(async () => {
        expect((await telnetPage.info()).states).toContain('visible');
      });
      await selectSettingsTab(app, 'Terminal');
      await showTerminalPage(app);
      await expectSelectedComboValue(
        app,
        'settings_terminal_backspace_code_combo',
        'Auto (built-in default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_terminal_return_code_combo',
        'Auto (built-in default)'
      );
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();

      const store = await waitForAppliedStore(app);
      expect(store.type).toBe('telnet');
      expect(store.backspace_code).toBe('auto');
      expect(store.return_code).toBe('auto');
    });
  });

  it('updates General and Serial state from the editable connection type', async (context) => {
    await runSharedGtkTest(context, [], async ({ app }) => {
      const combo = expectElementKind(
        await app.getById('settings_general_type_combo'),
        'comboBox'
      );
      await expectSensitive(combo);

      const serialPage = await app.getById('settings_serial_page');
      expect((await serialPage.info()).states).not.toContain('visible');

      await combo.selectChildAt(3);
      await waitForResult(async () => {
        expect((await serialPage.info()).states).toContain('visible');
      });
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();

      const store = await waitForAppliedStore(app);
      expect(store.type).toBe('serial');
    });
  });

  it('updates General and SSH state with an Auto default', async (context) => {
    await runSharedGtkTest(context, [], async ({ app }) => {
      const combo = expectElementKind(
        await app.getById('settings_general_type_combo'),
        'comboBox'
      );
      await expectSensitive(combo);

      const sshPage = await app.getById('settings_ssh_page');
      expect((await sshPage.info()).states).not.toContain('visible');

      await combo.selectChildAt(4);
      await waitForResult(async () => {
        expect((await sshPage.info()).states).toContain('visible');
      });
      await selectSettingsTab(app, 'Terminal');
      await showTerminalPage(app);
      await expectSelectedComboValue(
        app,
        'settings_terminal_backspace_code_combo',
        'Auto (built-in default)'
      );
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();

      const store = await waitForAppliedStore(app);
      expect(store.type).toBe('ssh');
      expect(store.backspace_code).toBe('auto');
    });
  });

  it('applies runtime sensitivity rules to General and TELNET controls', async (context) => {
    await runSharedGtkTest(context, ['--runtime'], async ({ app }) => {
      await expectInsensitive(await app.getById('settings_general_type_combo'));
      const telnetPage = await app.getById('settings_telnet_page');
      expect((await telnetPage.info()).states).not.toContain('visible');
    });

    await runSharedGtkTest(
      context,
      [
        '--page=telnet',
        '--runtime',
        '--type=telnet',
        '--telnet-address=runtime.example',
        '--telnet-port=10023',
        '--telnet-terminal-type=vt220',
      ],
      async ({ app }) => {
        await expectInsensitive(
          await app.getById('settings_general_type_combo')
        );
        await showTelnetPage(app);

        const address = expectElementKind(
          await app.getById('settings_telnet_address_entry'),
          'entry'
        );
        const port = expectElementKind(
          await app.getById('settings_telnet_port_entry'),
          'entry'
        );
        const terminalType = expectElementKind(
          await app.getById('settings_telnet_terminal_type_entry'),
          'entry'
        );
        expect(await address.text()).toBe('runtime.example');
        await expectNumericEntryValue(port, 10023);
        expect(await terminalType.text()).toBe('vt220');
        await expectInsensitive(address);
        await expectInsensitive(port);
        await expectInsensitive(terminalType);
      }
    );
  });

  it('applies runtime sensitivity rules to General and Serial controls', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=serial',
        '--runtime',
        '--type=serial',
        '--serial-device=/dev/ttyUSB11',
        '--serial-baudrate=38400',
        '--serial-bits=6',
        '--serial-parity=o',
        '--serial-stop-bit=2',
        '--serial-flow-control=hard',
        '--serial-carrier-detect=cts',
      ],
      async ({ app }) => {
        await expectInsensitive(
          await app.getById('settings_general_type_combo')
        );
        await showSerialPage(app);

        const device = expectElementKind(
          await app.getById('settings_serial_device_combo'),
          'comboBox'
        );
        const matchMode = expectElementKind(
          await app.getById('settings_serial_device_match_mode_combo'),
          'comboBox'
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
        await expectSelectedComboValue(
          app,
          'settings_serial_device_combo',
          '/dev/ttyUSB11'
        );
        await expectNumericEntryValue(baudrate, 38400);
        await expectSelectedComboValue(app, 'settings_serial_bits_combo', '6');
        await expectSelectedComboValue(
          app,
          'settings_serial_parity_combo',
          'Odd'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_stop_bit_combo',
          '2'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_flow_control_combo',
          'RTS/CTS (hardware)'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_carrier_detect_combo',
          'CTS (Clear to Send)'
        );
        await expectInsensitive(device);
        await expectInsensitive(matchMode);
        await expectSensitive(baudrate);
        await expectSensitive(bits);
        await expectSensitive(parity);
        await expectSensitive(stopBit);
        await expectSensitive(flowControl);
        await expectSensitive(carrierDetect);

        await setNumericEntryValue(baudrate, 57600);
        await bits.selectChildAt(4);
        await parity.selectChildAt(2);
        await stopBit.selectChildAt(1);
        await flowControl.selectChildAt(2);
        await carrierDetect.selectChildAt(3);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.serial_device).toBe('/dev/ttyUSB11');
        expect(store.serial_baudrate).toBe('57600');
        expect(store.serial_bits).toBe('8');
        expect(store.serial_parity).toBe('e');
        expect(store.serial_stop_bit).toBe('1');
        expect(store.serial_flow_control).toBe('xon');
        expect(store.serial_carrier_detect).toBe('dsr');
      }
    );
  });

  it('shows Terminal values, centers actions, and applies terminal edits', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--width=88',
        '--height=31',
        '--scrollback-lines=20000',
        '--zoom=1.25',
        '--auto-close=false',
        '--send-break-key=shift+F11',
      ],
      async ({ app, directory }) => {
        await showTerminalPage(app);

        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        const height = expectElementKind(
          await app.getById('settings_terminal_height_entry'),
          'entry'
        );
        const zoom = expectElementKind(
          await app.getById('settings_terminal_zoom_entry'),
          'entry'
        );
        const scrollbackLines = expectElementKind(
          await app.getById('settings_terminal_scrollback_lines_entry'),
          'entry'
        );
        const autoClose = expectElementKind(
          await app.getById('settings_terminal_auto_close_combo'),
          'comboBox'
        );
        const zoomInKey = expectElementKind(
          await app.getById('settings_terminal_zoom_in_key_entry'),
          'entry'
        );
        const zoomOutKey = expectElementKind(
          await app.getById('settings_terminal_zoom_out_key_entry'),
          'entry'
        );
        const sendBreakKey = expectElementKind(
          await app.getById('settings_terminal_send_break_key_entry'),
          'entry'
        );
        await expectNumericEntryValue(width, 88);
        await expectNumericEntryValue(height, 31);
        await expectNumericEntryValue(scrollbackLines, 20000);
        await expectNumericEntryValue(zoom, 1.25);
        await expectSelectedComboValue(
          app,
          'settings_terminal_auto_close_combo',
          'Disabled'
        );
        expect(await zoomInKey.text()).toBe('');
        expect(await zoomOutKey.text()).toBe('');
        expect(await sendBreakKey.text()).toBe('shift+F11');

        const window = expectElementKind(
          await app.getById('settings_widget_test_window'),
          'window'
        );
        const windowBounds = await window.bounds();
        await app.input.moveMouseTo(
          windowBounds.x + 5,
          windowBounds.y + windowBounds.height - 5
        );
        const terminalPageCapture = await captureWhenVisuallyStable(
          await app.getById('settings_terminal_page'),
          'settings-widget-terminal-page'
        );
        expect(terminalPageCapture.clipped).toBe(false);
        await expectCaptureToMatchFixture(
          terminalPageCapture,
          'settings-widget-terminal-page',
          terminalPageFixturePath,
          directory,
          { maxDiffPixels: 0, threshold: 0.01 }
        );

        const actionRowCapture = await app
          .getById('settings_action_row')
          .then((row) => row.capture());
        const applyCapture = await app
          .getById('settings_apply_button')
          .then((button) => button.capture());
        const cancelCapture = await app
          .getById('settings_cancel_button')
          .then((button) => button.capture());
        expect(actionRowCapture.clipped).toBe(false);
        await expectCaptureToMatchFixture(
          actionRowCapture,
          'settings-widget-action-row',
          actionRowFixturePath,
          directory,
          { maxDiffPixels: 0, threshold: 0.01 }
        );
        const rowCenter =
          actionRowCapture.bounds.y + actionRowCapture.bounds.height / 2;
        expect(
          Math.abs(
            applyCapture.bounds.y + applyCapture.bounds.height / 2 - rowCenter
          )
        ).toBeLessThanOrEqual(2);
        expect(
          Math.abs(
            cancelCapture.bounds.y + cancelCapture.bounds.height / 2 - rowCenter
          )
        ).toBeLessThanOrEqual(2);

        await setNumericEntryValue(width, 81);
        await setNumericEntryValue(height, 25);
        await setNumericEntryValue(scrollbackLines, 50000);
        await setNumericEntryValue(zoom, 1.1);
        await autoClose.selectChildAt(1);
        await showTerminalKeyBindings(app);
        await captureKeyBinding(app, zoomInKey, ['alt'], 'Up');
        await captureKeyBinding(app, sendBreakKey, ['shift'], 'F12');
        await clearKeyBinding(
          app,
          zoomOutKey,
          zoomInKey,
          'settings_terminal_zoom_out_key_reset_button'
        );
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_zoom_out_key_entry',
          'Disabled'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.width).toBe('81');
        expect(store.height).toBe('25');
        expect(store.scrollback_lines).toBe('50000');
        expect(Number(store.zoom)).toBeCloseTo(1.1);
        expect(store.auto_close).toBe('true');
        expect(store.zoom_in_key).toBe('alt+Up');
        expect(store.zoom_out_key).toBe('');
        expect(store.send_break_key).toBe('shift+F12');
      }
    );
  });

  it('uses three-state terminal font modes and applies, saves, and cancels overrides', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--save',
        '--global=terminal.font_primary_family=Monospace',
        '--global=terminal.font_fallback_family=IPAGothic',
      ],
      async ({ app, directory }) => {
        await showTerminalPage(app);
        const primaryMode = expectElementKind(
          await app.getById('settings_terminal_font_primary_mode_combo'),
          'comboBox'
        );
        const fallbackMode = expectElementKind(
          await app.getById('settings_terminal_font_fallback_mode_combo'),
          'comboBox'
        );
        const primaryButton = expectElementKind(
          await app.getById('settings_terminal_font_primary_button'),
          'button'
        );
        const fallbackButton = expectElementKind(
          await app.getById('settings_terminal_font_fallback_button'),
          'button'
        );

        await expectSelectedComboValue(
          app,
          'settings_terminal_font_primary_mode_combo',
          'Custom font (global default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_font_fallback_mode_combo',
          'Custom font (global default)'
        );
        expect(
          await comboOptionNames(
            app,
            'settings_terminal_font_primary_mode_combo'
          )
        ).toEqual([
          'Custom font (global default)',
          'Use built-in default',
          'Custom font',
        ]);
        expect(
          await comboOptionNames(
            app,
            'settings_terminal_font_fallback_mode_combo'
          )
        ).toEqual([
          'Custom font (global default)',
          'Use built-in default',
          'Custom font',
        ]);
        await expectSensitive(primaryButton);
        await expectSensitive(fallbackButton);
        await waitForResult(async () => {
          const output = (await app.output()).stdout;
          expect(output).toContain('primary_present=true');
          expect(output).toContain('primary_level=0');
          expect(output).toContain('primary_use_size=false');
          expect(output).toContain('primary_show_size=false');
          expect(output).toContain('primary_show_style=false');
          expect(output).toContain('fallback_present=true');
          expect(output).toContain('fallback_level=0');
          expect(output).toContain('fallback_use_size=false');
          expect(output).toContain('fallback_show_size=false');
          expect(output).toContain('fallback_show_style=false');
        });

        const scrollbar = expectElementKind(
          await app.getById('settings_terminal_page_scrollbar'),
          'scrollbar'
        );
        const scrollbarValue = await scrollbar.valueInfo();
        expect(scrollbarValue.maximum).toBeGreaterThan(scrollbarValue.minimum);
        const window = expectElementKind(
          await app.getById('settings_widget_test_window'),
          'window'
        );
        const windowBounds = await window.bounds();
        await app.input.moveMouseTo(
          windowBounds.x + 5,
          windowBounds.y + windowBounds.height - 5
        );
        await scrollbar.setValue(scrollbarValue.maximum);
        await waitForResult(async () => {
          expect((await primaryMode.info()).states).toContain('showing');
          expect((await fallbackMode.info()).states).toContain('showing');
        });
        const terminalPage = await app.getById('settings_terminal_page');
        const terminalPageCapture = await captureWhenVisuallyStable(
          terminalPage,
          'settings-widget-terminal-font-families'
        );
        expect(terminalPageCapture.clipped).toBe(false);
        await expectCaptureToMatchFixture(
          terminalPageCapture,
          'settings-widget-terminal-font-families',
          fixturePath('settings-widget-terminal-font-families'),
          directory,
          visualComparisonOptions
        );

        await confirmSelectedFont(
          app,
          'settings_terminal_font_primary_button',
          'Select Primary Terminal Font'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_font_primary_mode_combo',
          'Custom font'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_font_fallback_mode_combo',
          'Custom font (global default)'
        );
        await confirmSelectedFont(
          app,
          'settings_terminal_font_fallback_button',
          'Select Secondary Terminal Font'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_font_primary_mode_combo',
          'Custom font'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_font_fallback_mode_combo',
          'Custom font'
        );
        await expectSensitive(primaryButton);
        await expectSensitive(fallbackButton);
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const applied = await waitForAppliedStore(app);
        expect(applied.font_primary_family).toBe('Monospace');
        expect(applied.font_primary_family_source).toBe('override');
        expect(applied.font_primary_family_explicit).toBe('true');
        expect(applied.font_fallback_family).toBe('IPAGothic');
        expect(applied.font_fallback_family_source).toBe('override');
        expect(applied.font_fallback_family_explicit).toBe('true');
      }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.font_primary_family=Monospace',
        '--global=terminal.font_fallback_family=IPAGothic',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        const primaryMode = expectElementKind(
          await app.getById('settings_terminal_font_primary_mode_combo'),
          'comboBox'
        );
        const fallbackMode = expectElementKind(
          await app.getById('settings_terminal_font_fallback_mode_combo'),
          'comboBox'
        );
        await primaryMode.selectChildAt(1);
        await fallbackMode.selectChildAt(1);
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const applied = await waitForAppliedStore(app);
        expect((await app.output()).stdout).toContain(
          'font_primary_family=Noto Sans Mono font_fallback_family=Monospace'
        );
        expect(applied.font_primary_family_source).toBe('override');
        expect(applied.font_primary_family_explicit).toBe('true');
        expect(applied.font_fallback_family).toBe('Monospace');
        expect(applied.font_fallback_family_source).toBe('override');
        expect(applied.font_fallback_family_explicit).toBe('true');
      }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--save',
        '--font-primary-family=Monospace',
        '--font-fallback-family=IPAGothic',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        const primaryMode = expectElementKind(
          await app.getById('settings_terminal_font_primary_mode_combo'),
          'comboBox'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_font_primary_mode_combo',
          'Custom font'
        );
        await primaryMode.selectChildAt(0);
        await expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        ).click();

        const saved = await waitForPrintedStore(app, 'SAVED');
        expect((await app.output()).stdout).toContain(
          'font_primary_family=Noto Sans Mono font_fallback_family=IPAGothic'
        );
        expect(saved.font_primary_family_explicit).toBe('false');
        expect(saved.font_fallback_family).toBe('IPAGothic');
        expect(saved.font_fallback_family_explicit).toBe('true');
      }
    );

    await runSharedGtkTest(
      context,
      ['--page=terminal', '--global=terminal.font_primary_family=Monospace'],
      async ({ app }) => {
        await showTerminalPage(app);
        const primaryMode = expectElementKind(
          await app.getById('settings_terminal_font_primary_mode_combo'),
          'comboBox'
        );
        await primaryMode.selectChildAt(2);
        await expectElementKind(
          await app.getById('settings_cancel_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          const output = (await app.output()).stdout;
          expect(output).toContain('CANCELLED');
          expect(output).not.toContain('APPLIED');
          expect(output).not.toContain('SAVED');
        });
      }
    );
  }, 120_000);

  it('applies terminal encoding and special-code selections', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--type=serial', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);

        const encoding = expectElementKind(
          await app.getById('settings_terminal_encoding_entry'),
          'entry'
        );
        const backspace = expectElementKind(
          await app.getById('settings_terminal_backspace_code_combo'),
          'comboBox'
        );
        const cursorKeys = expectElementKind(
          await app.getById('settings_terminal_cursor_key_mode_combo'),
          'comboBox'
        );
        const returnCode = expectElementKind(
          await app.getById('settings_terminal_return_code_combo'),
          'comboBox'
        );
        expect(await encoding.text()).toBe('');
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_encoding_entry',
          'UTF-8 (built-in default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_backspace_code_combo',
          'BS (built-in default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_cursor_key_mode_combo',
          'TRS80 (built-in default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_return_code_combo',
          'CR (built-in default)'
        );

        await encoding.setText('CP932');
        await backspace.selectChildAt(1);
        await expectSelectedComboValue(
          app,
          'settings_terminal_backspace_code_combo',
          'Auto'
        );
        await cursorKeys.selectChildAt(1);
        await returnCode.selectChildAt(3);
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.encoding).toBe('CP932');
        expect(store.backspace_code).toBe('auto');
        expect(store.cursor_key_mode).toBe('normal');
        expect(store.return_code).toBe('lf');
      }
    );
  });

  it('blocks applying an invalid terminal encoding', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        const encoding = expectElementKind(
          await app.getById('settings_terminal_encoding_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        await encoding.setText('elder-terms-invalid-encoding');
        await waitForChangedState(app, 'CHANGED dirty=true valid=false');
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await encoding.setText('CP932');
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectSensitive(apply);
        await expectSensitive(save);
      }
    );
  });

  it('accepts only terminal scrollback sizes from 1000 through 100000', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        const scrollbackLines = expectElementKind(
          await app.getById('settings_terminal_scrollback_lines_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        await waitForEntryPlaceholder(
          app,
          'settings_terminal_scrollback_lines_entry',
          '10000 (built-in default)'
        );
        await scrollbackLines.setText('999');
        await waitForEntryIconTooltip(
          app,
          'settings_terminal_scrollback_lines_entry',
          'Value must be between 1000 and 100000'
        );
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await scrollbackLines.setText('100001');
        await waitForEntryIconTooltip(
          app,
          'settings_terminal_scrollback_lines_entry',
          'Value must be between 1000 and 100000'
        );
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await scrollbackLines.setText('100000');
        await waitForResult(async () => {
          await expectSensitive(apply);
          await expectSensitive(save);
        });
        await expectElementKind(apply, 'button').click();
        expect((await waitForAppliedStore(app)).scrollback_lines).toBe(
          '100000'
        );
      }
    );
  });

  it('captures terminal key bindings with live modifier state', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        await showTerminalKeyBindings(app);
        const zoomInKey = expectElementKind(
          await app.getById('settings_terminal_zoom_in_key_entry'),
          'entry'
        );
        const zoomOutKey = expectElementKind(
          await app.getById('settings_terminal_zoom_out_key_entry'),
          'entry'
        );

        expect(await zoomInKey.text()).toBe('');
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_zoom_in_key_entry',
          'ctrl+equal (built-in default)'
        );
        expect((await zoomInKey.info()).states).not.toContain('editable');
        await clickWidget(app, zoomInKey);
        await expectEntryText(zoomInKey, '');
        await clickWidget(app, zoomOutKey);
        await expectEntryText(zoomInKey, '');
        expect((await app.output()).stdout).not.toContain('CHANGED');

        await clickWidget(app, zoomInKey);
        await app.input.setModifier('control', true);
        try {
          await expectEntryText(zoomInKey, 'ctrl');
          await app.input.setModifier('shift', true);
          try {
            await expectEntryText(zoomInKey, 'ctrl+shift');
          } finally {
            await app.input.setModifier('shift', false);
          }
          await expectEntryText(zoomInKey, 'ctrl');
        } finally {
          await app.input.setModifier('control', false);
        }
        await expectEntryText(zoomInKey, '');
        expect((await app.output()).stdout).not.toContain('CHANGED');

        await app.input.pressKey('x');
        await expectEntryText(zoomInKey, 'x');
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');

        await app.input.setModifier('control', true);
        try {
          await app.input.setModifier('shift', true);
          try {
            await app.input.pressKey('x');
            await expectEntryText(zoomInKey, 'ctrl+shift+x');
          } finally {
            await app.input.setModifier('shift', false);
          }
          await expectEntryText(zoomInKey, 'ctrl+shift+x');
        } finally {
          await app.input.setModifier('control', false);
        }
        await expectEntryText(zoomInKey, 'ctrl+shift+x');
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');

        await app.input.pressKey('Tab');
        await expectEntryText(zoomInKey, 'Tab');
        await app.input.pressKey('Escape');
        await expectEntryText(zoomInKey, 'Escape');
        await app.input.pressKey('BackSpace');
        await expectEntryText(zoomInKey, 'BackSpace');

        await app.input.setModifier('control', true);
        try {
          await expectEntryText(zoomInKey, 'ctrl');
          await clickWidget(app, zoomOutKey);
          await expectEntryText(zoomInKey, 'BackSpace');
        } finally {
          await app.input.setModifier('control', false);
        }

        await clearKeyBinding(
          app,
          zoomInKey,
          zoomOutKey,
          'settings_terminal_zoom_in_key_reset_button'
        );
        await expectEntryText(zoomInKey, '');
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_zoom_in_key_entry',
          'Disabled'
        );

        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.zoom_in_key).toBe('');
        expect(store.zoom_in_key_explicit).toBe('true');
      }
    );
  });

  it('blocks applying conflicting terminal key bindings', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        const zoomInKey = expectElementKind(
          await app.getById('settings_terminal_zoom_in_key_entry'),
          'entry'
        );
        const zoomOutKey = expectElementKind(
          await app.getById('settings_terminal_zoom_out_key_entry'),
          'entry'
        );
        const sendBreakKey = expectElementKind(
          await app.getById('settings_terminal_send_break_key_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        await showTerminalKeyBindings(app);
        await captureKeyBinding(app, zoomInKey, ['alt'], 'F1');
        await expectEntryText(zoomInKey, 'alt+F1');
        await captureKeyBinding(app, zoomOutKey, ['alt'], 'F1');
        await expectEntryText(zoomOutKey, 'alt+F1');
        await waitForResult(async () => {
          await expectInsensitive(apply);
          await expectInsensitive(save);
        });

        await captureKeyBinding(app, zoomOutKey, ['alt'], 'F2');
        await expectEntryText(zoomOutKey, 'alt+F2');
        await waitForResult(async () => {
          await expectSensitive(apply);
          await expectSensitive(save);
        });

        await captureKeyBinding(app, sendBreakKey, ['alt'], 'F1');
        await expectEntryText(sendBreakKey, 'alt+F1');
        await waitForResult(async () => {
          await expectInsensitive(apply);
          await expectInsensitive(save);
        });

        await captureKeyBinding(app, sendBreakKey, ['alt'], 'F3');
        await expectEntryText(sendBreakKey, 'alt+F3');
        await waitForResult(async () => {
          await expectSensitive(apply);
          await expectSensitive(save);
        });
        await expectElementKind(apply, 'button').click();
        const store = await waitForAppliedStore(app);
        expect(store.zoom_in_key).toBe('alt+F1');
        expect(store.zoom_out_key).toBe('alt+F2');
        expect(store.send_break_key).toBe('alt+F3');
      }
    );
  });

  it('shows global source labels and preserves inheritance for every global-capable setting', async (context) => {
    const args = [
      '--page=terminal',
      '--global=general.type=serial',
      '--global=terminal.width=96',
      '--global=terminal.height=32',
      '--global=terminal.scrollback_lines=20000',
      '--global=terminal.zoom=1.25',
      '--global=terminal.auto_close=true',
      '--global=terminal.encoding=CP932',
      '--global=terminal.backspace_code=del',
      '--global=terminal.cursor_key_mode=normal',
      '--global=terminal.zoom_in_key=alt+plus',
      '--global=terminal.zoom_out_key=alt+minus',
      '--global=terminal.send_break_key=alt+F12',
      '--global=telnet.address=global.telnet.test',
      '--global=telnet.port=2323',
      '--global=telnet.terminal_type=vt220',
      '--global=ssh.address=global.ssh.test',
      '--global=ssh.port=2222',
      '--global=ssh.username=',
      '--global=ssh.identity_file=/tmp/id_global',
      '--global=ssh.terminal_type=ansi',
      '--global=sftp.local_directory=/tmp/local',
      '--global=sftp.remote_directory=/srv/global',
      '--global=serial.device=/dev/ttyGLOBAL',
      '--global=serial.baudrate=57600',
      '--global=serial.bits=7',
      '--global=serial.parity=e',
      '--global=serial.stop_bit=2',
      '--global=serial.flow_control=xon',
      '--global=serial.carrier_detect=dsr',
      '--global=transfer.base_path=file:///tmp/global-transfer',
      '--global=transfer.text_send_bytes_per_second=4096',
      '--global=transfer.text_send_follow_return_code=false',
      '--global=transfer.zmodem_autostart=false',
      '--global=log.enabled=true',
      '--global=log.base_directory=/tmp/global-log',
      '--global=log.file_name_format={name}.global.log',
      '--global=log.mode=cooked',
    ] as const;

    await runSharedGtkTest(context, args, async ({ app }) => {
      await expectSelectedConnectionType(app, 'Serial (global default)');
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();
      const inherited = await waitForAppliedStore(app);
      const globallyInheritedKeys = [
        'type',
        'width',
        'height',
        'scrollback_lines',
        'zoom',
        'auto_close',
        'encoding',
        'backspace_code',
        'cursor_key_mode',
        'zoom_in_key',
        'zoom_out_key',
        'send_break_key',
        'telnet_address',
        'telnet_port',
        'telnet_terminal_type',
        'ssh_address',
        'ssh_port',
        'ssh_username',
        'ssh_identity_file',
        'ssh_terminal_type',
        'sftp_local_directory',
        'sftp_remote_directory',
        'serial_device',
        'serial_baudrate',
        'serial_bits',
        'serial_parity',
        'serial_stop_bit',
        'serial_flow_control',
        'serial_carrier_detect',
        'transfer_base_path',
        'text_send_bytes_per_second',
        'text_send_follow_return_code',
        'zmodem_autostart',
        'log_enabled',
        'log_base_directory',
        'log_file_name_format',
        'log_mode',
      ] as const;
      for (const key of globallyInheritedKeys) {
        expect(inherited[`${key}_source`], key).toBe('global');
        expect(inherited[`${key}_explicit`], key).toBe('false');
      }

      await selectSettingsTab(app, 'Terminal');
      await expectInheritedEntry(
        app,
        'settings_terminal_width_entry',
        '96 (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_height_entry',
        '32 (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_scrollback_lines_entry',
        '20000 (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_zoom_entry',
        '1.25 (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_terminal_auto_close_combo',
        'Enabled (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_encoding_entry',
        'CP932 (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_terminal_backspace_code_combo',
        'DEL (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_terminal_cursor_key_mode_combo',
        'Normal (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_zoom_in_key_entry',
        'alt+plus (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_zoom_out_key_entry',
        'alt+minus (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_send_break_key_entry',
        'alt+F12 (global default)'
      );
      expectElementKind(
        await app.getById('settings_terminal_zoom_in_key_reset_button'),
        'button'
      );
      expectElementKind(
        await app.getById('settings_terminal_zoom_out_key_reset_button'),
        'button'
      );
      expectElementKind(
        await app.getById('settings_terminal_send_break_key_reset_button'),
        'button'
      );

      await selectSettingsTab(app, 'Serial');
      await expectSelectedComboValue(
        app,
        'settings_serial_device_combo',
        '/dev/ttyGLOBAL (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_serial_baudrate_entry',
        '57600 (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_serial_bits_combo',
        '7 (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_serial_parity_combo',
        'Even (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_serial_stop_bit_combo',
        '2 (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_serial_flow_control_combo',
        'XON/XOFF (software) (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_serial_carrier_detect_combo',
        'DSR (Data Set Ready) (global default)'
      );

      await selectSettingsTab(app, 'Transfer');
      await expectInheritedEntry(
        app,
        'settings_transfer_base_path_entry',
        'file:///tmp/global-transfer (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_transfer_text_send_rate_entry',
        '4096 (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_transfer_text_send_follow_return_code_combo',
        'Disabled (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_transfer_zmodem_autostart_combo',
        'Disabled (global default)'
      );

      await selectSettingsTab(app, 'Logging');
      await expectSelectedComboValue(
        app,
        'settings_log_enabled_combo',
        'Enabled (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_log_base_directory_entry',
        '/tmp/global-log (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_log_file_name_format_entry',
        '{name}.global.log (global default)'
      );
      await expectSelectedComboValue(
        app,
        'settings_log_mode_combo',
        'UTF-8 text (after character conversion) (global default)'
      );

      await expectInheritedEntry(
        app,
        'settings_telnet_address_entry',
        'global.telnet.test (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_telnet_port_entry',
        '2323 (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_telnet_terminal_type_entry',
        'vt220 (global default)'
      );

      await expectInheritedEntry(
        app,
        'settings_ssh_address_entry',
        'global.ssh.test (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_ssh_port_entry',
        '2222 (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_ssh_username_entry',
        'Global default'
      );
      await expectInheritedEntry(
        app,
        'settings_ssh_identity_file_entry',
        '/tmp/id_global (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_ssh_terminal_type_entry',
        'ansi (global default)'
      );

      await expectInheritedEntry(
        app,
        'settings_sftp_local_directory_entry',
        '/tmp/local (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_sftp_remote_directory_entry',
        '/srv/global (global default)'
      );
    });
  });

  it('keeps a same-value numeric override and clears it only from a blank entry', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--global=terminal.width=96'],
      async ({ app }) => {
        await showTerminalPage(app);
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await expectInheritedEntry(
          app,
          'settings_terminal_width_entry',
          '96 (global default)'
        );
        await width.setText('96');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const explicit = await waitForAppliedStore(app);
        expect(explicit.width).toBe('96');
        expect(explicit.width_source).toBe('override');
        expect(explicit.width_explicit).toBe('true');
      }
    );

    await runSharedGtkTest(
      context,
      ['--page=terminal', '--global=terminal.width=96', '--width=96'],
      async ({ app }) => {
        await showTerminalPage(app);
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await width.setText('');
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_width_entry',
          '96 (global default)'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const inherited = await waitForAppliedStore(app);
        expect(inherited.width).toBe('96');
        expect(inherited.width_source).toBe('global');
        expect(inherited.width_explicit).toBe('false');
      }
    );
  });

  it('shows a same-value combo override without a qualifier and clears it from the inherit row', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=serial', '--type=serial', '--global=serial.parity=e'],
      async ({ app }) => {
        await showSerialPage(app);
        const parity = expectElementKind(
          await app.getById('settings_serial_parity_combo'),
          'comboBox'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_parity_combo',
          'Even (global default)'
        );
        await parity.selectChildAt(2);
        await expectSelectedComboValue(
          app,
          'settings_serial_parity_combo',
          'Even'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const explicit = await waitForAppliedStore(app);
        expect(explicit.serial_parity).toBe('e');
        expect(explicit.serial_parity_source).toBe('override');
        expect(explicit.serial_parity_explicit).toBe('true');
      }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=serial',
        '--type=serial',
        '--global=serial.parity=e',
        '--serial-parity=e',
      ],
      async ({ app }) => {
        await showSerialPage(app);
        const parity = expectElementKind(
          await app.getById('settings_serial_parity_combo'),
          'comboBox'
        );
        await parity.selectChildAt(0);
        await expectSelectedComboValue(
          app,
          'settings_serial_parity_combo',
          'Even (global default)'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const inherited = await waitForAppliedStore(app);
        expect(inherited.serial_parity).toBe('e');
        expect(inherited.serial_parity_source).toBe('global');
        expect(inherited.serial_parity_explicit).toBe('false');
      }
    );
  });

  it('creates same-value text, editable, boolean, and key-binding overrides from inherited controls', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=general.type=telnet',
        '--global=terminal.encoding=CP932',
        '--global=terminal.auto_close=true',
        '--global=terminal.zoom_in_key=alt+F1',
        '--global=telnet.terminal_type=vt220',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        const encoding = await expectInheritedEntry(
          app,
          'settings_terminal_encoding_entry',
          'CP932 (global default)'
        );
        const autoClose = expectElementKind(
          await app.getById('settings_terminal_auto_close_combo'),
          'comboBox'
        );
        const zoomIn = await expectInheritedEntry(
          app,
          'settings_terminal_zoom_in_key_entry',
          'alt+F1 (global default)'
        );

        await encoding.setText('CP932');
        await autoClose.selectChildAt(1);
        await showTerminalKeyBindings(app);
        await captureKeyBinding(app, zoomIn, ['alt'], 'F1');
        await selectSettingsTab(app, 'TELNET');
        const terminalType = await expectInheritedEntry(
          app,
          'settings_telnet_terminal_type_entry',
          'vt220 (global default)'
        );
        await terminalType.setText('vt220');

        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const explicit = await waitForAppliedStore(app);
        for (const key of [
          'encoding',
          'auto_close',
          'zoom_in_key',
          'telnet_terminal_type',
        ] as const) {
          expect(explicit[`${key}_source`], key).toBe('override');
          expect(explicit[`${key}_explicit`], key).toBe('true');
        }
        expect(explicit.encoding).toBe('CP932');
        expect(explicit.auto_close).toBe('true');
        expect(explicit.zoom_in_key).toBe('alt+F1');
        expect(explicit.zoom_out_key_source).toBe('built-in');
        expect(explicit.zoom_out_key_explicit).toBe('false');
        expect(explicit.telnet_terminal_type).toBe('vt220');
      }
    );
  });

  it('restores a plain entry fallback presentation when an override is cleared', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ssh',
        '--type=ssh',
        '--global=ssh.username=alice',
        '--ssh-username=bob',
      ],
      async ({ app }) => {
        await showSshPage(app);
        const username = expectElementKind(
          await app.getById('settings_ssh_username_entry'),
          'entry'
        );
        expect(await username.text()).toBe('bob');
        await username.setText('');
        await waitForEntryPlaceholder(
          app,
          'settings_ssh_username_entry',
          'alice (global default)'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const inherited = await waitForAppliedStore(app);
        expect(inherited.ssh_username).toBe('alice');
        expect(inherited.ssh_username_source).toBe('global');
        expect(inherited.ssh_username_explicit).toBe('false');
      }
    );
  });

  it('validates numeric text before applying and accepts blank inheritance', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--global=terminal.width=96', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        await width.setText('not-a-number');
        await waitForChangedState(app, 'CHANGED dirty=true valid=false');
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await width.setText('0');
        await waitForResult(async () => {
          await expectInsensitive(apply);
          await expectInsensitive(save);
        });

        await width.setText('');
        await waitForChangedState(app, 'CHANGED dirty=false valid=true');
        await expectSensitive(apply);
        await expectSensitive(save);
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_width_entry',
          '96 (global default)'
        );
      }
    );
  });

  it('distinguishes an explicitly disabled key binding from inheritance and resets it', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.zoom_in_key=alt+plus',
        '--zoom-in-key=',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        const zoomIn = expectElementKind(
          await app.getById('settings_terminal_zoom_in_key_entry'),
          'entry'
        );
        expect(await zoomIn.text()).toBe('');
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_zoom_in_key_entry',
          'Disabled'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const disabled = await waitForAppliedStore(app);
        expect(disabled.zoom_in_key).toBe('');
        expect(disabled.zoom_in_key_source).toBe('override');
        expect(disabled.zoom_in_key_explicit).toBe('true');
      }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.zoom_in_key=alt+plus',
        '--zoom-in-key=',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        await expectElementKind(
          await app.getById('settings_terminal_zoom_in_key_reset_button'),
          'button'
        ).click();
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const reset = await waitForAppliedStore(app);
        expect(reset.zoom_in_key).toBe('alt+plus');
        expect(reset.zoom_in_key_source).toBe('global');
        expect(reset.zoom_in_key_explicit).toBe('false');
      }
    );
  });

  it('rejects explicit key bindings that conflict with inherited effective values', async (context) => {
    const cases = [
      {
        args: [
          '--page=terminal',
          '--save',
          '--zoom-out-key=ctrl+equal',
        ] as const,
      },
      {
        args: [
          '--page=terminal',
          '--save',
          '--global=terminal.zoom_in_key=alt+F1',
          '--zoom-out-key=alt+F1',
        ] as const,
      },
    ];

    for (const testCase of cases) {
      await runSharedGtkTest(context, testCase.args, async ({ app }) => {
        await showTerminalPage(app);
        const zoomOut = expectElementKind(
          await app.getById('settings_terminal_zoom_out_key_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        await waitForResult(async () => {
          await expectInsensitive(apply);
          await expectInsensitive(save);
        });

        await showTerminalKeyBindings(app);
        await captureKeyBinding(app, zoomOut, ['alt'], 'F2');
        await waitForResult(async () => {
          await expectSensitive(apply);
          await expectSensitive(save);
        });
      });
    }
  });

  it('labels dynamic terminal and ZMODEM fallbacks with their actual source', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--type=serial'],
      async ({ app }) => {
        await showTerminalPage(app);
        await expectSelectedComboValue(
          app,
          'settings_terminal_backspace_code_combo',
          'BS (built-in default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_cursor_key_mode_combo',
          'TRS80 (built-in default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_return_code_combo',
          'CR (built-in default)'
        );
        await selectSettingsTab(app, 'Transfer');
        await expectSelectedComboValue(
          app,
          'settings_transfer_zmodem_autostart_combo',
          'Enabled (built-in default)'
        );

        await selectSettingsTab(app, 'General');
        const type = expectElementKind(
          await app.getById('settings_general_type_combo'),
          'comboBox'
        );
        await type.selectChildAt(4);
        await selectSettingsTab(app, 'SSH');
        await showSshPage(app);
        await selectSettingsTab(app, 'Terminal');
        await expectSelectedComboValue(
          app,
          'settings_terminal_backspace_code_combo',
          'Auto (built-in default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_cursor_key_mode_combo',
          'Normal (built-in default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_return_code_combo',
          'Auto (built-in default)'
        );
        await selectSettingsTab(app, 'Transfer');
        await expectSelectedComboValue(
          app,
          'settings_transfer_zmodem_autostart_combo',
          'Disabled (built-in default)'
        );
      }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--type=serial',
        '--global=terminal.backspace_code=del',
        '--global=terminal.cursor_key_mode=normal',
        '--global=terminal.return_code=lf',
        '--global=transfer.zmodem_autostart=false',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        await expectSelectedComboValue(
          app,
          'settings_terminal_backspace_code_combo',
          'DEL (global default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_cursor_key_mode_combo',
          'Normal (global default)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_return_code_combo',
          'LF (global default)'
        );
        await selectSettingsTab(app, 'Transfer');
        await expectSelectedComboValue(
          app,
          'settings_transfer_zmodem_autostart_combo',
          'Disabled (global default)'
        );
      }
    );
  });

  it('renders the global editor with its own IDs, no name, and every backend tab', async (context) => {
    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=terminal', '--global=terminal.width=97'],
      async ({ app }) => {
        expect(await visibleSettingsTabNames(app, 'global_settings')).toEqual([
          'General',
          'TELNET',
          'Serial',
          'SSH',
          'SFTP',
          'Terminal',
          'Transfer',
          'Logging',
        ]);
        await expect(
          app.getById('global_settings_general_name_entry')
        ).rejects.toThrow();
        const generalIds = await descendantAccessibleIds(
          await app.getById('global_settings_general_page')
        );
        expect(
          generalIds.has('global_settings_general_open_connection_entry')
        ).toBe(false);
        await expect(app.getById('settings_notebook')).rejects.toThrow();
        expectElementKind(
          await app.getById('global_settings_general_type_combo'),
          'comboBox'
        );
        await expectSelectedComboValue(
          app,
          'global_settings_general_ui_language_combo',
          'System default'
        );
        await expectSelectedComboValue(
          app,
          'global_settings_general_startup_mode_combo',
          'Simple startup (built-in default)'
        );
        const openApplication = expectElementKind(
          await app.getById('global_settings_general_open_application_entry'),
          'entry'
        );
        expect(await openApplication.text()).toBe('');
        await waitForEntryPlaceholder(
          app,
          'global_settings_general_open_application_entry',
          'ctrl+alt+t (built-in default)'
        );
        await selectSettingsTab(app, 'SSH', 'global_settings');
        await waitForResult(async () => {
          expect(
            (
              await (
                await app.getById('global_settings_ssh_terminal_type_entry')
              ).info()
            ).states
          ).toContain('showing');
        });
        await selectSettingsTab(app, 'Terminal', 'global_settings');
        const width = expectElementKind(
          await app.getById('global_settings_terminal_width_entry'),
          'entry'
        );
        await expectNumericEntryValue(width, 97);
        await expectInheritedEntry(
          app,
          'global_settings_terminal_height_entry',
          '24 (built-in default)'
        );
        await expectElementKind(
          await app.getById('global_settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.width).toBe('97');
        expect(store.width_source).toBe('override');
        expect(store.width_explicit).toBe('true');
        expect(store.height_source).toBe('built-in');
        expect(store.height_explicit).toBe('false');
      }
    );
  }, 60_000);

  it('edits global-only UI language, startup, and application hotkey settings', async (context) => {
    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=general'],
      async ({ app }) => {
        await expect(
          app.getById('settings_general_startup_mode_combo')
        ).rejects.toThrow();
        await expect(
          app.getById('settings_general_ui_language_combo')
        ).rejects.toThrow();
        const uiLanguage = expectElementKind(
          await app.getById('global_settings_general_ui_language_combo'),
          'comboBox'
        );
        expect(
          await comboOptionNames(
            app,
            'global_settings_general_ui_language_combo'
          )
        ).toEqual([
          'System default',
          'English',
          'العربية',
          'Español',
          'Français',
          'हिन्दी',
          '日本語',
          '한국어',
          'Português',
          'Русский',
          '中文',
        ]);
        await uiLanguage.selectChildAt(6);
        await expectSelectedComboValue(
          app,
          'global_settings_general_ui_language_combo',
          '日本語'
        );
        const startupMode = expectElementKind(
          await app.getById('global_settings_general_startup_mode_combo'),
          'comboBox'
        );
        expect(
          await comboOptionNames(
            app,
            'global_settings_general_startup_mode_combo'
          )
        ).toEqual([
          'Simple startup (built-in default)',
          'Simple startup',
          'Background only',
          'System tray only',
          'System tray and main window',
        ]);
        await startupMode.selectChildAt(2);
        await expectSelectedComboValue(
          app,
          'global_settings_general_startup_mode_combo',
          'Background only'
        );

        const openApplication = expectElementKind(
          await app.getById('global_settings_general_open_application_entry'),
          'entry'
        );
        await captureKeyBinding(
          app,
          openApplication,
          ['control', 'shift'],
          'y'
        );
        await expectEntryText(openApplication, 'ctrl+shift+y');
        await expectElementKind(
          await app.getById('global_settings_apply_button'),
          'button'
        ).click();
        const configured = await waitForAppliedStore(app);
        expect(configured.ui_language).toBe('ja');
        expect(configured.startup_mode).toBe('background');
        expect(configured.open_application).toBe('ctrl+shift+y');
        expect(configured.ui_language_explicit).toBe('true');
        expect(configured.startup_mode_explicit).toBe('true');
        expect(configured.open_application_explicit).toBe('true');
      }
    );

    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=general', '--global=general.ui_language=ja'],
      async ({ app }) => {
        const uiLanguage = expectElementKind(
          await app.getById('global_settings_general_ui_language_combo'),
          'comboBox'
        );
        await uiLanguage.selectChildAt(0);
        await expectSelectedComboValue(
          app,
          'global_settings_general_ui_language_combo',
          'System default'
        );
        await expectElementKind(
          await app.getById('global_settings_apply_button'),
          'button'
        ).click();
        const system = await waitForAppliedStore(app);
        expect(system.ui_language).toBe('system');
        expect(system.ui_language_explicit).toBe('false');
      }
    );

    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=general', '--global=general.open_application='],
      async ({ app }) => {
        const openApplication = expectElementKind(
          await app.getById('global_settings_general_open_application_entry'),
          'entry'
        );
        await waitForEntryPlaceholder(
          app,
          'global_settings_general_open_application_entry',
          'Disabled'
        );
        await expectElementKind(
          await app.getById(
            'global_settings_general_open_application_reset_button'
          ),
          'button'
        ).click();
        await waitForEntryPlaceholder(
          app,
          'global_settings_general_open_application_entry',
          'ctrl+alt+t (built-in default)'
        );
        await expectElementKind(
          await app.getById('global_settings_apply_button'),
          'button'
        ).click();
        const reset = await waitForAppliedStore(app);
        expect(reset.open_application).toBe('ctrl+alt+t');
        expect(reset.open_application_explicit).toBe('false');
      }
    );
  }, 60_000);

  it('rebases inherited fields without changing dirty overrides or the connection name', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.width=90',
        '--global=terminal.height=30',
        '--width=90',
        '--rebase-global=terminal.width=100',
        '--rebase-global=terminal.height=40',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        const name = expectElementKind(
          await app.getById('settings_general_name_entry'),
          'entry'
        );
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await name.setText('DraftName');
        await width.setText('95');
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');

        await expectElementKind(
          await app.getById('rebase_fallbacks_button'),
          'button'
        ).click();
        const rebased = await waitForPrintedStore(app, 'REBASED');
        expect(rebased.dirty).toBe('true');
        expect(rebased.name).toBe('DraftName');
        expect(rebased.width).toBe('95');
        expect(rebased.width_source).toBe('override');
        expect(rebased.width_explicit).toBe('true');
        expect(rebased.height).toBe('40');
        expect(rebased.height_source).toBe('global');
        expect(rebased.height_explicit).toBe('false');

        await expectNumericEntryValue(
          expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          ),
          95
        );
        await expectInheritedEntry(
          app,
          'settings_terminal_height_entry',
          '40 (global default)'
        );
        expect(await name.text()).toBe('DraftName');
      }
    );

    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.height=30',
        '--rebase-global=terminal.height=40',
      ],
      async ({ app }) => {
        await expectElementKind(
          await app.getById('rebase_fallbacks_button'),
          'button'
        ).click();
        const rebased = await waitForPrintedStore(app, 'REBASED');
        expect(rebased.dirty).toBe('false');
        expect(rebased.height).toBe('40');
        expect(rebased.height_source).toBe('global');
      }
    );
  });

  it('preserves invalid raw input while rebasing inherited fields', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.width=90',
        '--global=terminal.encoding=CP932',
        '--global=log.file_name_format={name}.log',
        '--rebase-global=terminal.width=100',
        '--rebase-global=terminal.encoding=UTF-8',
        '--rebase-global=log.file_name_format={YYYY}.log',
        '--save',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        const encoding = expectElementKind(
          await app.getById('settings_terminal_encoding_entry'),
          'entry'
        );
        await width.setText('not-a-number');
        await encoding.setText('elder-terms-invalid-encoding');
        await selectSettingsTab(app, 'Logging');
        await showLoggingPage(app);
        const fileNameFormat = expectElementKind(
          await app.getById('settings_log_file_name_format_entry'),
          'entry'
        );
        await fileNameFormat.setText('../outside.log');
        await waitForChangedState(app, 'CHANGED dirty=true valid=false');

        await expectElementKind(
          await app.getById('rebase_fallbacks_button'),
          'button'
        ).click();
        const rebased = await waitForPrintedStore(app, 'REBASED');
        expect(rebased.width).toBe('100');
        expect(rebased.width_source).toBe('global');
        expect(rebased.encoding).toBe('UTF-8');
        expect(rebased.encoding_source).toBe('global');
        expect(rebased.log_file_name_format).toBe('{YYYY}.log');
        expect(rebased.log_file_name_format_source).toBe('global');
        expect(await fileNameFormat.text()).toBe('../outside.log');

        await selectSettingsTab(app, 'Terminal');
        await showTerminalPage(app);
        expect(await width.text()).toBe('not-a-number');
        expect(await encoding.text()).toBe('elder-terms-invalid-encoding');
        await expectInsensitive(await app.getById('settings_apply_button'));
        await expectInsensitive(await app.getById('settings_save_button'));
      }
    );
  });

  it('does not apply draft terminal edits when cancelled', async (context) => {
    await runSharedGtkTest(context, ['--page=terminal'], async ({ app }) => {
      await showTerminalPage(app);
      await setNumericEntryValue(
        expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        ),
        90
      );
      await setNumericEntryValue(
        expectElementKind(
          await app.getById('settings_terminal_height_entry'),
          'entry'
        ),
        40
      );
      await expectElementKind(
        await app.getById('settings_cancel_button'),
        'button'
      ).click();

      await waitForResult(async () => {
        const output = await app.output();
        expect(output.stdout).toContain('CANCELLED');
        expect(output.stdout).not.toContain('APPLIED');
      });
    });
  });
});
