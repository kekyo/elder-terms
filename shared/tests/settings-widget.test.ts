import { fileURLToPath } from 'node:url';
import {
  mkdtemp,
  mkdir,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
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
  readonly bell_sound: string;
  readonly show_border: string;
  readonly border_width: string;
  readonly backspace_code: string;
  readonly background: string;
  readonly cursor_key_mode: string;
  readonly return_code: string;
  readonly scrollback_lines: string;
  readonly encoding: string;
  readonly exterior_background: string;
  readonly height: string;
  readonly font_families_explicit: string;
  readonly font_families_source: string;
  readonly log_base_directory: string;
  readonly log_enabled: string;
  readonly log_file_name_format: string;
  readonly log_mode: string;
  readonly local_command_line: string;
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
  readonly ftp_address: string;
  readonly ftp_port: string;
  readonly ftp_username: string;
  readonly ftp_data_connection_mode: string;
  readonly ftp_local_directory: string;
  readonly ftp_remote_directory: string;
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

const scrollTerminalPageToBottom = async (app: GtkApp): Promise<void> => {
  const scrollbar = expectElementKind(
    await app.getById('settings_terminal_page_scrollbar'),
    'scrollbar'
  );
  const range = await scrollbar.valueInfo();
  await scrollbar.setValue(range.maximum);
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

const showFtpPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const address = await app.getById('settings_ftp_address_entry');
    expect((await address.info()).states).toContain('showing');
  });
};

const doubleClickTableRow = async (
  app: GtkApp,
  table: GtkWidgetElement,
  row: number
): Promise<void> => {
  if (table.kind !== 'table') {
    throw new Error(`Expected a GTK table, received ${table.kind}`);
  }
  const cell = await table.cellAt(row, 0);
  const bounds = (await cell?.capture())?.bounds;
  expect(bounds).toBeDefined();
  if (bounds === undefined) {
    return;
  }
  await app.input.moveMouseTo(
    Math.round(bounds.x + bounds.width / 2),
    Math.round(bounds.y + bounds.height / 2)
  );
  for (let click = 0; click < 2; click += 1) {
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
  }
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

const showLocalPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const commandLine = await app.getById('settings_local_command_line_entry');
    expect((await commandLine.info()).states).toContain('showing');
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
  const labels = new Set<string>();
  const pending = [page];
  while (pending.length > 0) {
    const element = pending.pop() as GtkWidgetElement;
    const info = await element.info();
    if (info.kind === 'label') {
      labels.add(info.name);
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

  for (const label of expectedLabels) {
    expect(labels.has(label), `${pageId}: ${label}`).toBe(true);
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
  app: Pick<GtkApp, 'output'>,
  prefix: 'APPLIED' | 'REBASED' | 'SAVED'
): Promise<AppliedStore> =>
  waitForResult(async () => {
    const output = await app.output();
    const lines = output.stdout.split('\n');
    // A pipe read can end mid-record. Do not return a truncated result or
    // an older result while the latest matching record is still arriving.
    const pending = lines.pop() ?? '';
    expect(pending.startsWith(`${prefix} `)).toBe(false);
    const line = lines
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
  it('waits for the complete latest settings record when output arrives in chunks', async () => {
    for (const prefix of ['APPLIED', 'REBASED', 'SAVED'] as const) {
      for (const previous of [
        '',
        `${prefix} serial_parity_source=override serial_parity_explicit=true\n`,
      ]) {
        let reads = 0;
        const store = await waitForPrintedStore(
          {
            output: async () => ({
              stdout:
                previous +
                (++reads === 1
                  ? `${prefix} serial_parity_source=global serial_parity_exp`
                  : `${prefix} serial_parity_source=global serial_parity_explicit=false\n`),
              stderr: '',
              exitCode: null,
              exitSignal: null,
              stdoutTruncated: false,
              stderrTruncated: false,
            }),
          },
          prefix
        );
        expect(store).toMatchObject({
          serial_parity_source: 'global',
          serial_parity_explicit: 'false',
        });
      }
    }
  });

  it('orders connection settings before Terminal when available', async (context) => {
    const cases = [
      {
        args: [] as const,
        expected: [
          'General',
          'Local',
          'Terminal',
          'Transfer',
          'Logging',
          'Macro',
          'Links',
        ],
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
          'Links',
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
          'Links',
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
          'Links',
        ],
      },
      {
        args: ['--type=sftp'] as const,
        expected: ['General', 'SSH', 'SFTP'],
      },
      {
        args: ['--type=ftp'] as const,
        expected: ['General', 'FTP'],
      },
      {
        args: ['--type=webdav'] as const,
        expected: ['General', 'WebDAV'],
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
              'Title and status bar background',
              'Content background',
            ],
          },
          {
            id: 'global_settings_local_page',
            labels: ['Startup command'],
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
            id: 'global_settings_ftp_page',
            labels: [
              'Address',
              'Port',
              'User name',
              'Data connection mode',
              'Local directory',
              'Remote directory',
            ],
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
              'Active indicator color',
              'Inactive indicator color',
              'Font families',
              'Close window when session ends',
              'Show window side borders',
              'Window side border width (px)',
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
          'ローカル',
          'TELNET',
          'シリアル',
          'SSH',
          'SFTP',
          'FTP',
          'WebDAV',
          '端末',
          '転送',
          'ログ',
        ]);
        const pages = [
          {
            id: 'global_settings_general_page',
            labels: [
              '接続方式',
              'タイトル／ステータスバーの背景',
              'コンテンツの背景',
            ],
          },
          {
            id: 'global_settings_local_page',
            labels: ['起動コマンド'],
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
            id: 'global_settings_ftp_page',
            labels: [
              'アドレス',
              'ポート',
              'ユーザー名',
              'データ接続方式',
              'ローカルディレクトリ',
              'リモートディレクトリ',
            ],
          },
          {
            id: 'global_settings_webdav_page',
            labels: [
              '接続の暗号化',
              'アドレス',
              'ポート',
              '公開パス',
              '認証方式',
              'ユーザー名',
              'ローカルディレクトリ',
              'リモートディレクトリ',
              'CA証明書',
              '証明書検証の失敗時',
              '接続タイムアウト（秒）',
              '無通信タイムアウト（秒）',
            ],
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
              'フォントファミリー',
              'セッション終了時にウィンドウを閉じる',
              'ウィンドウの左右にボーダーを表示する',
              'ウィンドウ左右のボーダー幅（px）',
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
        await expectSelectedComboValue(
          app,
          'global_settings_terminal_fonts_mode_combo',
          'アプリ標準'
        );
        expect(
          await comboOptionNames(
            app,
            'global_settings_terminal_fonts_mode_combo'
          )
        ).toEqual(['アプリ標準', '指定する']);
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

  it('matches Local visual fixtures for default, configured, and runtime settings', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=local'],
        assert: async (app) => {
          const commandLine = expectElementKind(
            await app.getById('settings_local_command_line_entry'),
            'entry'
          );
          expect(await commandLine.text()).toBe('');
          await waitForEntryPlaceholder(
            app,
            'settings_local_command_line_entry',
            'Built-in default'
          );
          await expectSensitive(commandLine);
        },
        differsFrom: undefined,
        fixtureName: 'settings-widget-local-page-default-editable',
        pageId: 'settings_local_page',
        prepare: showLocalPage,
      },
      {
        args: ['--page=local', '--local-command-line=/bin/bash --noprofile'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_local_command_line_entry'),
              'entry'
            ).text()
          ).toBe('/bin/bash --noprofile');
        },
        differsFrom: 'settings-widget-local-page-default-editable',
        fixtureName: 'settings-widget-local-command-bash',
        pageId: 'settings_local_page',
        prepare: showLocalPage,
      },
      {
        args: [
          '--page=local',
          '--runtime',
          '--local-command-line=/bin/bash --noprofile',
        ],
        assert: async (app) => {
          await expectInsensitive(
            await app.getById('settings_local_command_line_entry')
          );
        },
        differsFrom: 'settings-widget-local-command-bash',
        fixtureName: 'settings-widget-local-page-runtime',
        pageId: 'settings_local_page',
        prepare: showLocalPage,
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

  it('edits, validates, and restores inheritance for the Local startup command', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=local', '--global=local.command_line=global-process', '--save'],
      async ({ app }) => {
        await showLocalPage(app);
        const commandLine = expectElementKind(
          await app.getById('settings_local_command_line_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');
        await expectInheritedEntry(
          app,
          'settings_local_command_line_entry',
          'global-process (global default)'
        );

        await commandLine.setText("broken '");
        await waitForChangedState(app, 'CHANGED dirty=true valid=false');
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await commandLine.setText('connection-process');
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectSensitive(apply);
        await expectSensitive(save);
        await expectElementKind(apply, 'button').click();
        const explicit = await waitForAppliedStore(app);
        expect(explicit.local_command_line).toBe('connection-process');
        expect(explicit.local_command_line_source).toBe('override');
        expect(explicit.local_command_line_explicit).toBe('true');

        await commandLine.setText('');
        await waitForEntryPlaceholder(
          app,
          'settings_local_command_line_entry',
          'global-process (global default)'
        );
        await expectElementKind(apply, 'button').click();
        await waitForResult(async () => {
          const inherited = await waitForAppliedStore(app);
          expect(inherited.local_command_line).toBe('global-process');
          expect(inherited.local_command_line_source).toBe('global');
          expect(inherited.local_command_line_explicit).toBe('false');
        });
      }
    );
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
          await waitForEntryPlaceholder(
            app,
            'settings_terminal_border_width_entry',
            '4 (built-in default)'
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

    const visualErrors: string[] = [];
    for (const testCase of cases) {
      await runSharedGtkTest(
        context,
        testCase.args,
        async ({ app, directory }) => {
          await testCase.prepare(app);
          await testCase.assert(app);
          // Keep every setting's capture when a shared page layout changes.
          try {
            await expectPageVisualFixture(app, testCase, directory);
          } catch (error) {
            visualErrors.push(`${testCase.fixtureName}: ${String(error)}`);
          }
        }
      );
    }
    expect(visualErrors).toEqual([]);
  }, 60_000);

  it('identifies the font inheritance source in a single option', async (context) => {
    for (const global of [false, true]) {
      await runSharedGtkTest(
        context,
        [
          '--page=terminal',
          '--save',
          ...(global ? ['--global=terminal.font_families=Global Font;'] : []),
          '--rebase-global=terminal.font_families=Rebased Font;',
        ],
        async ({ app }) => {
          await showTerminalPage(app);
          const id = 'settings_terminal_fonts_mode_combo';
          expect(await comboOptionNames(app, id)).toEqual([
            global
              ? 'Inherited from: global settings'
              : 'Inherited from: app defaults',
            'Use app defaults',
            'Specify for this connection',
          ]);
          await expectSelectedComboValue(
            app,
            id,
            global
              ? 'Inherited from: global settings'
              : 'Inherited from: app defaults'
          );
          await expectElementKind(
            await app.getById('rebase_fallbacks_button'),
            'button'
          ).click();
          await expectSelectedComboValue(
            app,
            id,
            'Inherited from: global settings'
          );
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_font_family_0'),
              'entry'
            ).text()
          ).toBe('Rebased Font');
        }
      );
    }
  });

  it('saves global fonts through two choices and restores app defaults', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-global-font-'));
    const path = join(directory, 'global.ini');
    try {
      await runSharedGtkTest(
        context,
        ['--global-mode', '--page=terminal', `--save-file=${path}`],
        async ({ app }) => {
          const id = 'global_settings_terminal_fonts_mode_combo';
          expect(await comboOptionNames(app, id)).toEqual([
            'App defaults',
            'Specify fonts',
          ]);
          const mode = expectElementKind(await app.getById(id), 'comboBox');
          await mode.selectChildAt(1);
          await expectElementKind(
            await app.getById('global_settings_terminal_font_family_0'),
            'entry'
          ).setText('Global Font');
          await expectElementKind(
            await app.getById('global_settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(await readFile(path, 'utf8')).toContain(
              'font_families=Global Font;Monospace;'
            )
          );
          await mode.selectChildAt(0);
          await expectElementKind(
            await app.getById('global_settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(await readFile(path, 'utf8')).not.toContain('font_families=')
          );
        }
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('aligns the font label with the top and places every candidate in the page', async (context) => {
    await runSharedGtkTest(context, ['--page=terminal'], async ({ app }) => {
      await showTerminalPage(app);
      await scrollTerminalPageToBottom(app);
      const window = expectElementKind(await app.windowAt(0), 'window');
      const original = (await window.capture()).bounds;
      const label = await findDescendantByName(
        await app.getById('settings_terminal_page'),
        'label',
        'Font families'
      );
      expect(label).toBeDefined();
      const mode = await app.getById('settings_terminal_fonts_mode_combo');
      const labelBounds = (await label!.capture()).bounds;
      expect(labelBounds.height).toBeLessThanOrEqual(
        (await mode.capture()).bounds.height
      );
      expect(
        Math.abs(labelBounds.y - (await mode.capture()).bounds.y)
      ).toBeLessThan(12);
      const add = expectElementKind(
        await app.getById('settings_terminal_fonts_add_button'),
        'button'
      );
      await add.click();
      const third = expectElementKind(
        await app.getById('settings_terminal_font_family_2'),
        'entry'
      );
      await waitForResult(async () => {
        expect((await third.info()).states).toContain('showing');
        expect((await third.info()).states).toContain('focused');
      });
      await third.setText('Third Candidate');
      const outer = expectElementKind(
        await app.getById('settings_terminal_page_scrollbar'),
        'scrollbar'
      );
      await outer.setValue((await outer.valueInfo()).maximum);
      const first = (
        await (await app.getById('settings_terminal_font_family_0')).capture()
      ).bounds;
      const last = (await third.capture()).bounds;
      expect(last.y - first.y).toBeGreaterThan(first.height * 2);
      expect((await window.capture()).bounds.height).toBe(original.height);
    });
  });

  it('saves and resets the inactive color independently of the active color', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-off-color-'));
    const path = join(directory, 'connection.ini');
    try {
      await runSharedGtkTest(
        context,
        [
          '--page=terminal',
          '--indicator-color=#FF0000',
          '--global=terminal.indicator_off_color=#112233',
          `--save-file=${path}`,
        ],
        async ({ app }) => {
          await showTerminalPage(app);
          await scrollTerminalPageToBottom(app);
          const mode = expectElementKind(
            await app.getById(
              'settings_terminal_indicator_off_color_mode_combo'
            ),
            'comboBox'
          );
          await chooseNamedColor(
            app,
            'settings_terminal_indicator_off_color_button',
            'Blue'
          );
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            const content = await readFile(path, 'utf8');
            expect(content).toContain('indicator_color=#FF0000\n');
            expect(content).toContain('indicator_off_color=#3584E4\n');
          });
          await chooseNamedColor(
            app,
            'settings_terminal_indicator_off_color_button',
            'Red'
          );
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(await readFile(path, 'utf8')).toContain(
              'indicator_off_color=#3584E4\n'
            )
          );
          await mode.selectChildAt(1);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(await readFile(path, 'utf8')).toContain(
              'indicator_off_color=default\n'
            )
          );
          await mode.selectChildAt(0);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(await readFile(path, 'utf8')).not.toContain(
              'indicator_off_color='
            )
          );
        }
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('saves an ordered font list containing three families', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-font-list-'));
    const savedPath = join(directory, 'connection.ini');
    try {
      await runSharedGtkTest(
        context,
        ['--page=terminal', `--save-file=${savedPath}`],
        async ({ app }) => {
          await showTerminalPage(app);
          await scrollTerminalPageToBottom(app);
          await expectElementKind(
            await app.getById('settings_terminal_fonts_mode_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectElementKind(
            await app.getById('settings_terminal_fonts_add_button'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_terminal_font_family_2'),
            'entry'
          ).setText('IPAGothic');
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForPrintedStore(app, 'SAVED');
          expect(await readFile(savedPath, 'utf8')).toContain(
            'font_families=Noto Sans Mono;Monospace;IPAGothic;'
          );
        }
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('reorders and removes font candidates as one applied and saved list', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-font-order-'));
    const path = join(directory, 'connection.ini');
    try {
      await runSharedGtkTest(
        context,
        [
          '--page=terminal',
          '--connection=terminal.font_families=Monospace;IPAGothic;DejaVu Sans Mono;',
          `--save-file=${path}`,
        ],
        async ({ app }) => {
          await showTerminalPage(app);
          await scrollTerminalPageToBottom(app);
          await expectElementKind(
            await app.getById('settings_terminal_font_up_2'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_terminal_font_down_0'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_terminal_font_remove_2'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect((await app.output()).stdout).toContain(
              'APPLIED_FONTS ["DejaVu Sans Mono","Monospace"]'
            );
          });
          await expectElementKind(
            await app.getById('settings_terminal_font_family_0'),
            'entry'
          ).setText('Unapplied Font');
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForPrintedStore(app, 'SAVED');
          expect(await readFile(path, 'utf8')).toContain(
            'font_families=DejaVu Sans Mono;Monospace;'
          );
        }
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('preserves invalid font drafts across rebase and resets the whole list', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.font_families=Global One;Global Two;',
        '--rebase-global=terminal.font_families=Replacement One;Replacement Two;Replacement Three;',
        '--save',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        await scrollTerminalPageToBottom(app);
        const mode = expectElementKind(
          await app.getById('settings_terminal_fonts_mode_combo'),
          'comboBox'
        );
        await mode.selectChildAt(2);
        const entry = expectElementKind(
          await app.getById('settings_terminal_font_family_0'),
          'entry'
        );
        for (const invalid of ['', 'Global Two', 'Invalid,Family']) {
          await entry.setText(invalid);
          await waitForChangedState(app, 'CHANGED dirty=true valid=false');
          expect(
            (await (await app.getById('settings_apply_button')).info()).states
          ).not.toContain('sensitive');
        }
        await expectElementKind(
          await app.getById('rebase_fallbacks_button'),
          'button'
        ).click();
        expect(
          await expectElementKind(
            await app.getById('settings_terminal_font_family_0'),
            'entry'
          ).text()
        ).toBe('Invalid,Family');
        expect(
          (await (await app.getById('settings_save_button')).info()).states
        ).not.toContain('sensitive');
        await mode.selectChildAt(1);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect((await app.output()).stdout).toContain(
            'APPLIED_FONTS ["Noto Sans Mono","Monospace"]'
          );
        });
        await mode.selectChildAt(0);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect((await app.output()).stdout).toContain(
            'APPLIED_FONTS ["Replacement One","Replacement Two","Replacement Three"]'
          );
        });
      }
    );
  });

  it('edits long ordered font lists and shows the list controls in Japanese', async (context) => {
    const names = Array.from(
      { length: 12 },
      (_, index) => `Candidate ${index + 1}`
    );
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        `--connection=terminal.font_families=${names.join(';')};`,
      ],
      async ({ app, directory }) => {
        await showTerminalPage(app);
        await scrollTerminalPageToBottom(app);
        const scrollbar = expectElementKind(
          await app.getById('settings_terminal_page_scrollbar'),
          'scrollbar'
        );
        const range = await scrollbar.valueInfo();
        expect(range.maximum).toBeGreaterThan(range.minimum);
        await scrollbar.setValue(range.maximum);
        const last = expectElementKind(
          await app.getById('settings_terminal_font_family_11'),
          'entry'
        );
        await waitForResult(async () =>
          expect((await last.info()).states).toContain('showing')
        );
        expect(await last.text()).toBe('Candidate 12');
        await last.setText('Last Font');
        const window = expectElementKind(await app.windowAt(0), 'window');
        const originalHeight = (await window.capture()).bounds.height;
        await expectElementKind(
          await app.getById('settings_terminal_fonts_add_button'),
          'button'
        ).click();
        const added = expectElementKind(
          await app.getById('settings_terminal_font_family_12'),
          'entry'
        );
        await waitForResult(async () => {
          expect((await added.info()).states).toContain('showing');
          expect((await added.info()).states).toContain('focused');
        });
        await added.setText('Added Font');
        expect((await window.capture()).bounds.height).toBe(originalHeight);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        await waitForResult(async () =>
          expect((await app.output()).stdout).toContain(
            `APPLIED_FONTS ${JSON.stringify([...names.slice(0, -1), 'Last Font', 'Added Font'])}`
          )
        );
        const page = await app.getById('settings_terminal_page');
        await writeFile(
          join(directory, 'long-font-list.png'),
          (await page.capture()).image
        );
      }
    );
    await runSharedGtkTest(
      context,
      ['--page=terminal'],
      async ({ app, directory }) => {
        await showTerminalPage(app);
        await scrollTerminalPageToBottom(app);
        await expectPageLabels(app, 'settings_terminal_page', [
          'フォントファミリー',
        ]);
        const add = await app.getById('settings_terminal_fonts_add_button');
        expect((await add.info()).name).toBe('追加');
        expect(
          await comboOptionNames(app, 'settings_terminal_fonts_mode_combo')
        ).toEqual(['継承元：アプリ標準', 'アプリ標準に固定', 'この接続で指定']);
        await writeFile(
          join(directory, 'font-list-ja.png'),
          (await (await app.getById('settings_terminal_page')).capture()).image
        );
      },
      { env: japaneseTestEnvironment }
    );
  });

  it('applies, cancels, saves, and resets the common terminal indicator color', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-indicator-'));
    const savedPath = join(directory, 'connection.ini');
    try {
      await runSharedGtkTest(
        context,
        [
          '--page=terminal',
          '--global=terminal.indicator_color=#112233',
          `--save-file=${savedPath}`,
        ],
        async ({ app }) => {
          await showTerminalPage(app);
          await scrollTerminalPageToBottom(app);
          await expectSelectedComboValue(
            app,
            'settings_terminal_indicator_color_mode_combo',
            'Custom color (global default)'
          );
          const mode = expectElementKind(
            await app.getById('settings_terminal_indicator_color_mode_combo'),
            'comboBox'
          );
          await chooseNamedColor(
            app,
            'settings_terminal_indicator_color_button',
            'Red'
          );
          await expectSelectedComboValue(
            app,
            'settings_terminal_indicator_color_mode_combo',
            'Custom color'
          );
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            const store = await waitForAppliedStore(app);
            expect(store.indicator_color).toBe('#E01B24');
            expect(store.indicator_color_source).toBe('override');
            expect(store.indicator_color_explicit).toBe('true');
          });
          await chooseNamedColor(
            app,
            'settings_terminal_indicator_color_button',
            'Blue'
          );
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(await readFile(savedPath, 'utf8')).toContain(
              'indicator_color=#E01B24\n'
            );
          });
          await mode.selectChildAt(1);
          await expectSelectedComboValue(
            app,
            'settings_terminal_indicator_color_mode_combo',
            'Default color'
          );
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(await readFile(savedPath, 'utf8')).toContain(
              'indicator_color=default\n'
            );
          });
          await mode.selectChildAt(0);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(await readFile(savedPath, 'utf8')).not.toContain(
              'indicator_color='
            );
          });
          await expectSelectedComboValue(
            app,
            'settings_terminal_indicator_color_mode_combo',
            'Custom color (global default)'
          );
        }
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('edits the global indicator color and keeps an explicit default over an inherited color', async (context) => {
    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=terminal'],
      async ({ app }) => {
        await selectSettingsTab(app, 'Terminal', 'global_settings');
        const scrollbar = expectElementKind(
          await app.getById('global_settings_terminal_page_scrollbar'),
          'scrollbar'
        );
        await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
        await expectSelectedComboValue(
          app,
          'global_settings_terminal_indicator_color_mode_combo',
          'Default color (built-in default)'
        );
        await chooseNamedColor(
          app,
          'global_settings_terminal_indicator_color_button',
          'Blue'
        );
        await expectElementKind(
          await app.getById('global_settings_apply_button'),
          'button'
        ).click();
        expect((await waitForAppliedStore(app)).indicator_color).toBe(
          '#3584E4'
        );
      }
    );
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.indicator_color=#112233',
        '--indicator-color=default',
        '--rebase-global=terminal.indicator_color=#445566',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        await expectSelectedComboValue(
          app,
          'settings_terminal_indicator_color_mode_combo',
          'Default color'
        );
        await scrollTerminalPageToBottom(app);
        await expectElementKind(
          await app.getById('rebase_fallbacks_button'),
          'button'
        ).click();
        await expectSelectedComboValue(
          app,
          'settings_terminal_indicator_color_mode_combo',
          'Default color'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        expect((await waitForAppliedStore(app)).indicator_color).toBe(
          'default'
        );
        await expectElementKind(
          await app.getById('settings_terminal_indicator_color_mode_combo'),
          'comboBox'
        ).selectChildAt(0);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect((await waitForAppliedStore(app)).indicator_color).toBe(
            '#445566'
          );
        });
      }
    );
  });

  it('warns about invalid indicator colors and uses a valid inherited fallback', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--global=terminal.indicator_color=#123abc',
        '--indicator-color=green',
        '--allow-invalid-connection-values',
      ],
      async ({ app }) => {
        await showTerminalPage(app);
        await scrollTerminalPageToBottom(app);
        await expectSelectedComboValue(
          app,
          'settings_terminal_indicator_color_mode_combo',
          'Custom color (global default)'
        );
        expect((await app.output()).stderr).toContain(
          'invalid configuration value [terminal] indicator_color'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.indicator_color).toBe('#123abc');
        expect(store.indicator_color_explicit).toBe('false');
      }
    );
  });

  it('shows the common indicator color setting in Japanese', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal'],
      async ({ app, directory }) => {
        await showTerminalPage(app);
        await scrollTerminalPageToBottom(app);
        await expectPageLabels(app, 'settings_terminal_page', [
          '点灯時の色',
          '消灯時の色',
        ]);
        await expectSelectedComboValue(
          app,
          'settings_terminal_indicator_color_mode_combo',
          '既定の色（組み込み既定値）'
        );
        await writeFile(
          join(directory, 'indicator-color-ja.png'),
          (await (await app.getById('settings_terminal_page')).capture()).image
        );
      },
      { env: japaneseTestEnvironment }
    );
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
  }, 60_000);

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

  it('edits WebDAV settings with inherited ports and independent HTTPS policy', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=webdav', '--type=webdav'],
      async ({ app, directory }) => {
        await selectSettingsTab(app, 'WebDAV');
        await writeFile(
          join(directory, 'webdav-settings-top.png'),
          (await app.capture()).image
        );
        const scheme = expectElementKind(
          await app.getById('settings_webdav_scheme_combo'),
          'comboBox'
        );
        const port = expectElementKind(
          await app.getById('settings_webdav_port_entry'),
          'entry'
        );
        await expectInheritedEntry(
          app,
          'settings_webdav_port_entry',
          '443 (built-in default)'
        );
        await scheme.selectChildAt(2);
        await expectInheritedEntry(
          app,
          'settings_webdav_port_entry',
          '80 (built-in default)'
        );
        await port.setText('8080');
        await scheme.selectChildAt(1);
        expect(await port.text()).toBe('8080');
        await expectElementKind(
          await app.getById('settings_webdav_base_path_entry'),
          'entry'
        ).setText('/dav/files/');
        await port.setText('invalid');
        await expectInsensitive(await app.getById('settings_apply_button'));
        await port.setText('8443');
        const scroll = expectElementKind(
          await app.getById('settings_webdav_page_scrollbar'),
          'scrollbar'
        );
        await scroll.setValue((await scroll.valueInfo()).maximum);
        await expectElementKind(
          await app.getById('settings_webdav_ca_file_combo'),
          'comboBox'
        ).selectChildAt(2);
        await expectInsensitive(await app.getById('settings_apply_button'));
        await expectElementKind(
          await app.getById('settings_webdav_ca_file_entry'),
          'entry'
        ).setText('/tmp/dav-ca.pem');
        await expectElementKind(
          await app.getById('settings_webdav_certificate_error_action_combo'),
          'comboBox'
        ).selectChildAt(2);
        await expectSensitive(await app.getById('settings_apply_button'));
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.type).toBe('webdav');
        expect(store.webdav_scheme).toBe('https');
        expect(store.webdav_port).toBe('8443');
        expect(store.webdav_base_path).toBe('/dav/files/');
        expect(store.webdav_ca_file).toBe('/tmp/dav-ca.pem');
        expect(store.webdav_certificate_action).toBe('prompt');
      }
    );
  });

  it('preserves unfinished WebDAV edits when global defaults change', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=webdav', '--type=webdav', '--rebase-global=webdav.port=8080'],
      async ({ app }) => {
        await selectSettingsTab(app, 'WebDAV');
        const port = expectElementKind(
          await app.getById('settings_webdav_port_entry'),
          'entry'
        );
        await port.setText('unfinished');
        await expectElementKind(
          await app.getById('rebase_fallbacks_button'),
          'button'
        ).click();
        expect(await port.text()).toBe('unfinished');
        await expectInsensitive(await app.getById('settings_apply_button'));
        await port.setText('8081');
        await expectSensitive(await app.getById('settings_apply_button'));
      }
    );
  });

  it('applies FTPS modes, protocol ranges and independent certificate policy', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=ftp', '--type=ftp'],
      async ({ app }) => {
        await showFtpPage(app);
        const mode = expectElementKind(
          await app.getById('settings_ftp_tls_mode_combo'),
          'comboBox'
        );
        const port = expectElementKind(
          await app.getById('settings_ftp_port_entry'),
          'entry'
        );
        await mode.selectChildAt(3);
        await expectInheritedEntry(
          app,
          'settings_ftp_port_entry',
          '990 (built-in default)'
        );
        const auth = await app.getById('settings_ftp_tls_auth_order_combo');
        await expectInsensitive(auth);
        await mode.selectChildAt(2);
        await expectInheritedEntry(
          app,
          'settings_ftp_port_entry',
          '21 (built-in default)'
        );
        await port.setText('2121');
        await mode.selectChildAt(3);
        expect(await port.text()).toBe('2121');
        await port.setText('');
        await expectInheritedEntry(
          app,
          'settings_ftp_port_entry',
          '990 (built-in default)'
        );
        await mode.selectChildAt(2);
        for (const [name, index] of [
          ['tls_min_version', 1],
          ['tls_max_version', 2],
          ['tls_auth_order', 2],
          ['tls_compatibility', 2],
          ['certificate_error_action', 2],
        ] as const) {
          await expectElementKind(
            await app.getById(`settings_ftp_${name}_combo`),
            'comboBox'
          ).selectChildAt(index);
        }
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.ftp_tls_mode).toBe('explicit');
        expect(store.ftp_tls_min_version).toBe('1.0');
        expect(store.ftp_tls_max_version).toBe('1.0');
        expect(store.ftp_tls_auth_order).toBe('ssl');
        expect(store.ftp_tls_compatibility).toBe('openssl_legacy');
        expect(store.ftp_certificate_error_action).toBe('prompt');
      }
    );
  });

  it('requires valid custom CA and TLS ranges before applying settings', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=ftp', '--type=ftp', '--connection=ftp.tls_mode=explicit'],
      async ({ app }) => {
        await showFtpPage(app);
        const apply = await app.getById('settings_apply_button');
        const minimum = expectElementKind(
          await app.getById('settings_ftp_tls_min_version_combo'),
          'comboBox'
        );
        const maximum = expectElementKind(
          await app.getById('settings_ftp_tls_max_version_combo'),
          'comboBox'
        );
        await maximum.selectChildAt(2);
        await expectInsensitive(apply);
        await minimum.selectChildAt(1);
        await expectSensitive(apply);
        await expectElementKind(
          await app.getById('settings_ftp_ca_file_mode_combo'),
          'comboBox'
        ).selectChildAt(2);
        await expectInsensitive(apply);
        const ca = expectElementKind(
          await app.getById('settings_ftp_ca_file_entry'),
          'entry'
        );
        await ca.setText('relative.pem');
        await expectInsensitive(apply);
        await ca.setText('/tmp/private-ca.pem');
        await expectSensitive(apply);
        await expectElementKind(apply, 'button').click();
        expect((await waitForAppliedStore(app)).ftp_ca_file).toBe(
          '/tmp/private-ca.pem'
        );
      }
    );
  });

  it('preserves an explicit system CA override through save and reload', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-ftps-settings-'));
    try {
      const path = join(directory, 'connection.ini');
      await runSharedGtkTest(
        context,
        [
          '--page=ftp',
          '--type=ftp',
          '--global=ftp.tls_mode=implicit',
          '--global=ftp.ca_file=/tmp/global-ca.pem',
          `--save-file=${path}`,
        ],
        async ({ app }) => {
          await showFtpPage(app);
          const ca = expectElementKind(
            await app.getById('settings_ftp_ca_file_mode_combo'),
            'comboBox'
          );
          await ca.selectChildAt(1);
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect((await app.output()).stdout).toContain('SAVED ')
          );
          const saved = await readFile(path, 'utf8');
          expect(saved).toContain('ca_file=');
          expect(saved).not.toContain('global-ca.pem');
          const restored = await waitForPrintedStore(app, 'SAVED');
          expect(restored.ftp_ca_file).toBe('');
          expect(restored.ftp_ca_file_explicit).toBe('true');
        }
      );
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('keeps FTPS connection settings read-only in a running window', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ftp',
        '--type=ftp',
        '--runtime',
        '--connection=ftp.tls_mode=explicit',
        '--connection=ftp.certificate_error_action=prompt',
      ],
      async ({ app }) => {
        await showFtpPage(app);
        for (const name of [
          'tls_mode',
          'tls_min_version',
          'tls_max_version',
          'tls_auth_order',
          'tls_compatibility',
          'certificate_error_action',
        ])
          await expectInsensitive(
            await app.getById(`settings_ftp_${name}_combo`)
          );
        for (const name of [
          'ca_file',
          'tls_cipher_list',
          'tls13_cipher_list',
        ]) {
          await expectInsensitive(
            await app.getById(`settings_ftp_${name}_mode_combo`)
          );
          await expectInsensitive(
            await app.getById(`settings_ftp_${name}_entry`)
          );
        }
      }
    );
  });

  it('retains an invalid custom CA edit while global defaults are reloaded', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ftp',
        '--type=ftp',
        '--connection=ftp.tls_mode=explicit',
        '--rebase-global=ftp.ca_file=/tmp/rebased.pem',
      ],
      async ({ app }) => {
        await showFtpPage(app);
        await expectElementKind(
          await app.getById('settings_ftp_ca_file_mode_combo'),
          'comboBox'
        ).selectChildAt(2);
        const ca = expectElementKind(
          await app.getById('settings_ftp_ca_file_entry'),
          'entry'
        );
        await ca.setText('unfinished');
        await expectElementKind(
          await app.getById('rebase_fallbacks_button'),
          'button'
        ).click();
        expect(await ca.text()).toBe('unfinished');
        await expectInsensitive(await app.getById('settings_apply_button'));
        await ca.setText('/tmp/corrected.pem');
        await expectSensitive(await app.getById('settings_apply_button'));
      }
    );
  });

  it('edits FTPS global defaults and exposes the complete scrollable form', async (context) => {
    await runSharedGtkTest(
      context,
      ['--global-mode', '--page=ftp'],
      async ({ app, directory }) => {
        const mode = expectElementKind(
          await app.getById('global_settings_ftp_tls_mode_combo'),
          'comboBox'
        );
        await mode.selectChildAt(3);
        await expectElementKind(
          await app.getById(
            'global_settings_ftp_certificate_error_action_combo'
          ),
          'comboBox'
        ).selectChildAt(2);
        const ca = expectElementKind(
          await app.getById('global_settings_ftp_ca_file_mode_combo'),
          'comboBox'
        );
        await expectSelectedComboValue(
          app,
          'global_settings_ftp_ca_file_mode_combo',
          'System CA certificates'
        );
        await ca.selectChildAt(1);
        await expectElementKind(
          await app.getById('global_settings_ftp_ca_file_entry'),
          'entry'
        ).setText('/tmp/global-ca.pem');
        await captureWhenVisuallyStable(
          await app.getById('global_settings_ftp_page'),
          'ftps-global-top.png'
        );
        await writeFile(
          join(directory, 'ftps-global-top.png'),
          (await app.capture()).image
        );
        const scrollbar = expectElementKind(
          await app.getById('global_settings_ftp_page_scrollbar'),
          'scrollbar'
        );
        await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
        const cipher = expectElementKind(
          await app.getById('global_settings_ftp_tls13_cipher_list_mode_combo'),
          'comboBox'
        );
        await waitForResult(async () =>
          expect((await cipher.info()).states).toContain('showing')
        );
        await cipher.selectChildAt(2);
        await expectElementKind(
          await app.getById('global_settings_ftp_tls13_cipher_list_entry'),
          'entry'
        ).setText('TLS_AES_128_GCM_SHA256');
        await captureWhenVisuallyStable(
          await app.getById('global_settings_ftp_page'),
          'ftps-global-details.png'
        );
        await writeFile(
          join(directory, 'ftps-global-details.png'),
          (await app.capture()).image
        );
        await expectElementKind(
          await app.getById('global_settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.ftp_tls_mode).toBe('implicit');
        expect(store.ftp_ca_file).toBe('/tmp/global-ca.pem');
        expect(store.ftp_certificate_error_action).toBe('prompt');
        expect(store.ftp_tls13_cipher_list).toBe('TLS_AES_128_GCM_SHA256');
      }
    );
  });

  it('shows Japanese FTPS choices and keeps TLS options when changing modes', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ftp',
        '--type=ftp',
        '--connection=ftp.tls_mode=explicit',
        '--connection=ftp.tls_min_version=1.0',
        '--connection=ftp.tls_compatibility=openssl_legacy',
      ],
      async ({ app, directory }) => {
        await showFtpPage(app);
        await expectSelectedComboValue(
          app,
          'settings_ftp_tls_mode_combo',
          'FTPS（明示的TLS）'
        );
        await expectSelectedComboValue(
          app,
          'settings_ftp_tls_compatibility_combo',
          '旧TLS互換（暗号・署名の制約を緩和）'
        );
        const mode = expectElementKind(
          await app.getById('settings_ftp_tls_mode_combo'),
          'comboBox'
        );
        await mode.selectChildAt(1);
        await expectInsensitive(
          await app.getById('settings_ftp_tls_min_version_combo')
        );
        await mode.selectChildAt(2);
        await expectSensitive(
          await app.getById('settings_ftp_tls_min_version_combo')
        );
        await expectSelectedComboValue(
          app,
          'settings_ftp_tls_min_version_combo',
          '1.0'
        );
        const scrollbar = expectElementKind(
          await app.getById('settings_ftp_page_scrollbar'),
          'scrollbar'
        );
        await scrollbar.setValue((await scrollbar.valueInfo()).maximum);
        await captureWhenVisuallyStable(
          await app.getById('settings_ftp_page'),
          'ftps-japanese-details.png'
        );
        await writeFile(
          join(directory, 'ftps-japanese-details.png'),
          (await app.capture()).image
        );
      },
      { env: japaneseTestEnvironment }
    );
  });

  it('allows correction of a retained invalid FTPS mode before applying', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ftp',
        '--type=ftp',
        '--allow-invalid-connection-values',
        '--connection=ftp.tls_mode=explict',
      ],
      async ({ app }) => {
        await showFtpPage(app);
        await expectInsensitive(await app.getById('settings_apply_button'));
        await expectSelectedComboValue(
          app,
          'settings_ftp_tls_mode_combo',
          'Invalid value: explict'
        );
        await expectElementKind(
          await app.getById('settings_ftp_tls_mode_combo'),
          'comboBox'
        ).selectChildAt(2);
        await expectSensitive(await app.getById('settings_apply_button'));
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        expect((await waitForAppliedStore(app)).ftp_tls_mode).toBe('explicit');
      }
    );
  });

  it('selects a CA file through a cancellable chooser', async (context) => {
    const root = await mkdtemp(join(tmpdir(), 'elder-ftps-ca-'));
    try {
      const file = join(root, 'selected-ca.pem');
      await writeFile(file, 'test CA selection');
      await runSharedGtkTest(
        context,
        [
          '--page=ftp',
          '--type=ftp',
          '--connection=ftp.tls_mode=explicit',
          `--ftp-ca-dialog-file=${file}`,
        ],
        async ({ app }) => {
          await showFtpPage(app);
          await expectElementKind(
            await app.getById('settings_ftp_ca_file_mode_combo'),
            'comboBox'
          ).selectChildAt(2);
          const scroll = expectElementKind(
            await app.getById('settings_ftp_page_scrollbar'),
            'scrollbar'
          );
          await scroll.setValue((await scroll.valueInfo()).maximum);
          const browse = expectElementKind(
            await app.getById('settings_ftp_ca_browse_button'),
            'button'
          );
          const entry = expectElementKind(
            await app.getById('settings_ftp_ca_file_entry'),
            'entry'
          );
          await browse.click();
          await waitForResult(async () =>
            expect(await app.getWindowCount()).toBe(2)
          );
          await app.input.pressKey('Escape');
          await waitForResult(async () =>
            expect(await app.getWindowCount()).toBe(1)
          );
          expect(await entry.text()).toBe('');
          await browse.click();
          await expectElementKind(
            await app.getById('settings_ftp_ca_open_button'),
            'button'
          ).click();
          await waitForResult(async () =>
            expect(await entry.text()).toBe(file)
          );
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          expect((await waitForAppliedStore(app)).ftp_ca_file).toBe(file);
        }
      );
    } finally {
      await rm(root, { recursive: true, force: true });
    }
  });

  it('cancels FTPS edits and restores inherited cipher settings', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ftp',
        '--save',
        '--type=ftp',
        '--global=ftp.tls_mode=explicit',
        '--global=ftp.port=2021',
        '--global=ftp.tls_cipher_list=AES128-SHA',
      ],
      async ({ app }) => {
        await showFtpPage(app);
        const mode = expectElementKind(
          await app.getById('settings_ftp_tls_mode_combo'),
          'comboBox'
        );
        const cipher = expectElementKind(
          await app.getById('settings_ftp_tls_cipher_list_mode_combo'),
          'comboBox'
        );
        await mode.selectChildAt(3);
        await expectInheritedEntry(
          app,
          'settings_ftp_port_entry',
          '2021 (global default)'
        );
        await cipher.selectChildAt(1);
        await expectElementKind(
          await app.getById('settings_cancel_button'),
          'button'
        ).click();
        // Cancel restores the draft; the real caller closes the editor.
        // Check the subsequently saved values, as the other cancel tests do.
        await expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        ).click();
        const cancelled = await waitForPrintedStore(app, 'SAVED');
        expect(cancelled.ftp_tls_mode).toBe('explicit');
        expect(cancelled.ftp_tls_mode_explicit).toBe('false');
        expect(cancelled.ftp_tls_cipher_list).toBe('AES128-SHA');
        expect(cancelled.ftp_tls_cipher_list_explicit).toBe('false');
        await cipher.selectChildAt(0);
        await cipher.selectChildAt(1);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        expect((await waitForAppliedStore(app)).ftp_tls_cipher_list).toBe('');
        expect(
          (await waitForAppliedStore(app)).ftp_tls_cipher_list_explicit
        ).toBe('true');
        await cipher.selectChildAt(0);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        await waitForResult(async () =>
          expect((await waitForAppliedStore(app)).ftp_tls_cipher_list).toBe(
            'AES128-SHA'
          )
        );
        expect(
          (await waitForAppliedStore(app)).ftp_tls_cipher_list_explicit
        ).toBe('false');
      }
    );
  });

  it('shows FTP connection controls and applies FTP edits', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ftp',
        '--type=ftp',
        '--ftp-address=ftp.example.test',
        '--ftp-port=2121',
        '--ftp-username=alice',
        '--ftp-data-connection-mode=active',
        '--ftp-local-directory=/home/alice/uploads',
        '--ftp-remote-directory=/srv/incoming',
      ],
      async ({ app }) => {
        await showFtpPage(app);
        const address = expectElementKind(
          await app.getById('settings_ftp_address_entry'),
          'entry'
        );
        const port = expectElementKind(
          await app.getById('settings_ftp_port_entry'),
          'entry'
        );
        const username = expectElementKind(
          await app.getById('settings_ftp_username_entry'),
          'entry'
        );
        const dataConnectionMode = expectElementKind(
          await app.getById('settings_ftp_data_connection_mode_combo'),
          'comboBox'
        );
        const localDirectory = expectElementKind(
          await app.getById('settings_ftp_local_directory_entry'),
          'entry'
        );
        const remoteDirectory = expectElementKind(
          await app.getById('settings_ftp_remote_directory_entry'),
          'entry'
        );

        expect(await address.text()).toBe('ftp.example.test');
        expect(await port.text()).toBe('2121');
        expect(await username.text()).toBe('alice');
        await expectSelectedComboValue(
          app,
          'settings_ftp_data_connection_mode_combo',
          'Active'
        );
        expect(await localDirectory.text()).toBe('/home/alice/uploads');
        expect(await remoteDirectory.text()).toBe('/srv/incoming');

        await address.setText('after.example.test');
        await setNumericEntryValue(port, 2021);
        await username.setText('bob');
        await dataConnectionMode.selectChildAt(1);
        await localDirectory.setText('/home/bob/outgoing');
        await remoteDirectory.setText('/opt/drop');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.type).toBe('ftp');
        expect(store.ftp_address).toBe('after.example.test');
        expect(store.ftp_port).toBe('2021');
        expect(store.ftp_username).toBe('bob');
        expect(store.ftp_data_connection_mode).toBe('passive');
        expect(store.ftp_local_directory).toBe('/home/bob/outgoing');
        expect(store.ftp_remote_directory).toBe('/opt/drop');
      }
    );
  });

  it('offers IP scan beside every network address setting', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=ssh', '--type=sftp'],
      async ({ app }) => {
        await showSshPage(app);
        for (const id of [
          'settings_telnet_ip_scan_button',
          'settings_ssh_ip_scan_button',
          'settings_ftp_ip_scan_button',
        ]) {
          const button = expectElementKind(await app.getById(id), 'button');
          expect((await button.info()).name).toBe('IP scan');
          await expectSensitive(button);
        }
      }
    );
  });

  it('shows completed IP scan results without changing the address on cancel', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ftp',
        '--type=ftp',
        '--ftp-address=before.example.test',
        '--ip-scan=complete',
      ],
      async ({ app, directory }) => {
        await showFtpPage(app);
        const parent = expectElementKind(
          await app.getById('settings_widget_test_window'),
          'window'
        );
        await expectSensitive(parent);
        await expectElementKind(
          await app.getById('settings_ftp_ip_scan_button'),
          'button'
        ).click();

        const dialog = expectElementKind(
          await app.getById('settings_ip_scan_dialog'),
          'window'
        );
        expect((await dialog.info()).states).not.toContain('modal');
        await expectInsensitive(parent);
        const results = expectElementKind(
          await app.getById('settings_ip_scan_results'),
          'table'
        );
        await waitForResult(async () => {
          expect(await results.getRowCount()).toBe(1);
        });
        expect(await results.getColumnCount()).toBe(5);
        for (const heading of ['SSH/SFTP(22)', 'TELNET(23)', 'FTP(21)']) {
          expect(await dialog.findText(heading)).toBeDefined();
        }
        const expectedCells = [
          '192.0.2.25',
          'router.example.test',
          '',
          '✓',
          '✓',
        ];
        for (let column = 0; column < expectedCells.length; column += 1) {
          const cell = await results.cellAt(0, column);
          expect((await cell?.info())?.name).toBe(expectedCells[column]);
        }
        const progress = expectElementKind(
          await app.getById('settings_ip_scan_progress'),
          'progressBar'
        );
        await waitForResult(async () => {
          const value = await progress.valueInfo();
          expect(value.value).toBe(value.maximum);
        });
        const dialogCapture = await dialog.capture();
        expect(dialogCapture.clipped).toBe(false);
        await expectCaptureToMatchFixture(
          dialogCapture,
          'settings-widget-ip-scan-dialog',
          fixturePath('settings-widget-ip-scan-dialog'),
          directory,
          visualComparisonOptions
        );

        await expectElementKind(
          await app.getById('settings_ip_scan_cancel_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await app.findById('settings_ip_scan_dialog')).toBeUndefined();
        });
        await expectSensitive(parent);
        expect(
          await expectElementKind(
            await app.getById('settings_ftp_address_entry'),
            'entry'
          ).text()
        ).toBe('before.example.test');
      }
    );
  });

  it('keeps a wide active IP scan bounded and responsive while resizing', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=telnet', '--type=telnet', '--ip-scan=burst'],
      async ({ app }) => {
        await showTelnetPage(app);
        await expectElementKind(
          await app.getById('settings_telnet_ip_scan_button'),
          'button'
        ).click();
        const dialog = expectElementKind(
          await app.getById('settings_ip_scan_dialog'),
          'window'
        );
        const progress = expectElementKind(
          await app.getById('settings_ip_scan_progress'),
          'progressBar'
        );
        const initialValue = await progress.valueInfo();
        expect(initialValue.value).toBeLessThan(initialValue.maximum);

        const initialBounds = await dialog.bounds();
        await dialog.resizeTo(
          initialBounds.width + 120,
          initialBounds.height + 80
        );
        await waitForResult(async () => {
          const resizedBounds = await dialog.bounds();
          expect(resizedBounds.width).toBeGreaterThanOrEqual(
            initialBounds.width + 100
          );
          expect(resizedBounds.height).toBeGreaterThanOrEqual(
            initialBounds.height + 60
          );
        });

        await waitForResult(async () => {
          const value = await progress.valueInfo();
          expect(value.value).toBe(value.maximum);
        });
        const results = expectElementKind(
          await app.getById('settings_ip_scan_results'),
          'table'
        );
        expect(await results.getRowCount()).toBe(256);
        for (const [column, expected] of ['✓', '', ''].entries()) {
          const cell = await results.cellAt(0, column + 2);
          expect((await cell?.info())?.name).toBe(expected);
        }
        await expectElementKind(
          await app.getById('settings_ip_scan_cancel_button'),
          'button'
        ).click();
      }
    );
  });

  it('cancels an active IP scan without changing the address', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=telnet',
        '--type=telnet',
        '--telnet-address=before.example.test',
        '--ip-scan=pending',
      ],
      async ({ app }) => {
        await showTelnetPage(app);
        await expectElementKind(
          await app.getById('settings_telnet_ip_scan_button'),
          'button'
        ).click();
        const progress = expectElementKind(
          await app.getById('settings_ip_scan_progress'),
          'progressBar'
        );
        await waitForResult(async () => {
          const value = await progress.valueInfo();
          expect(value.value).toBeLessThan(value.maximum);
        });
        await expectElementKind(
          await app.getById('settings_ip_scan_cancel_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await app.findById('settings_ip_scan_dialog')).toBeUndefined();
        });
        expect(
          await expectElementKind(
            await app.getById('settings_telnet_address_entry'),
            'entry'
          ).text()
        ).toBe('before.example.test');
      }
    );
  });

  for (const type of ['ssh', 'sftp', 'telnet', 'ftp']) {
    for (const mode of ['complete', 'no-name', 'pending', 'mdns', 'llmnr']) {
      it(`selects and persists the ${mode} IP scan result for ${type}`, async (context) => {
        const section = type === 'sftp' ? 'ssh' : type;
        const expectedAddress =
          mode === 'complete'
            ? 'router.example.test'
            : mode === 'mdns'
              ? 'router.local'
              : mode === 'llmnr'
                ? 'router'
                : '192.0.2.25';
        const directory = await mkdtemp(join(tmpdir(), 'elder-terms-scan-'));
        try {
          await runSharedGtkTest(
            context,
            [
              `--page=${section}`,
              `--type=${type}`,
              `--${section}-address=before.example.test`,
              `--${section}-port=2222`,
              `--ip-scan=${mode}`,
              `--save-file=${join(directory, 'connection.ini')}`,
            ],
            async ({ app }) => {
              if (section === 'ssh') {
                await showSshPage(app);
              } else if (section === 'telnet') {
                await showTelnetPage(app);
              } else {
                await showFtpPage(app);
              }
              await expectElementKind(
                await app.getById(`settings_${section}_ip_scan_button`),
                'button'
              ).click();
              const results = expectElementKind(
                await app.getById('settings_ip_scan_results'),
                'table'
              );
              await waitForResult(async () => {
                expect(await results.getRowCount()).toBe(1);
                if (expectedAddress !== '192.0.2.25') {
                  expect(
                    (await (await results.cellAt(0, 1))?.info())?.name
                  ).toBe(expectedAddress);
                }
              });
              await doubleClickTableRow(app, results, 0);
              await waitForResult(async () => {
                expect(
                  await app.findById('settings_ip_scan_dialog')
                ).toBeUndefined();
              });
              expect(
                await expectElementKind(
                  await app.getById(`settings_${section}_address_entry`),
                  'entry'
                ).text()
              ).toBe(expectedAddress);
              await expectElementKind(
                await app.getById('settings_apply_button'),
                'button'
              ).click();
              const applied = await waitForAppliedStore(app);
              expect(applied[`${section}_address`]).toBe(expectedAddress);
              expect(applied[`${section}_port`]).toBe('2222');
              expect(applied.type).toBe(type);
              expect(applied.name).toBe('fixture');
              await expectElementKind(
                await app.getById('settings_save_button'),
                'button'
              ).click();
              const saved = await waitForPrintedStore(app, 'SAVED');
              expect(saved[`${section}_address`]).toBe(expectedAddress);
              expect(saved[`${section}_port`]).toBe('2222');
              expect(saved.type).toBe(type);
              expect(saved.name).toBe('fixture');
            }
          );
        } finally {
          await rm(directory, { recursive: true, force: true });
        }
      });
    }
  }

  it('uses a discovered IP from an active scan for SFTP', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=ssh',
        '--type=sftp',
        '--ssh-address=before.example.test',
        '--ip-scan=pending',
      ],
      async ({ app }) => {
        await showSshPage(app);
        await expectElementKind(
          await app.getById('settings_ssh_ip_scan_button'),
          'button'
        ).click();
        const results = expectElementKind(
          await app.getById('settings_ip_scan_results'),
          'table'
        );
        await waitForResult(async () => {
          expect(await results.getRowCount()).toBe(1);
        });
        await doubleClickTableRow(app, results, 0);
        await waitForResult(async () => {
          expect(await app.findById('settings_ip_scan_dialog')).toBeUndefined();
        });
        expect(
          await expectElementKind(
            await app.getById('settings_ssh_address_entry'),
            'entry'
          ).text()
        ).toBe('192.0.2.25');

        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        const store = await waitForAppliedStore(app);
        expect(store.type).toBe('sftp');
        expect(store.ssh_address).toBe('192.0.2.25');
        expect(store.ssh_port).toBe('22');
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
          // Publish the device only after its metadata is complete. Creating
          // the link first can notify the watcher before the product exists.
          await writeFile(
            join(fixture.sysClassTtyRoot, 'zero', 'device', 'product'),
            'Other USB\n'
          );
          await symlink('/dev/zero', join(fixture.byIdRoot, 'usb-other'));
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
        '--log-file-name-format=${YYYY-MM-DD}/${hh:mm:ss}_${fff}.txt',
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
          '${YYYY-MM-DD}/${hh:mm:ss}_${fff}.txt'
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
          '${YYYY}-${MM}-${DD}/${name}_${hh}-${mm}-${ss}.txt'
        );
        await baseDirectory.setText('${documents}/elder-terms/${name}');
        await enabled.selectChildAt(2);
        await mode.selectChildAt(1);
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectSensitive(apply);
        await expectSensitive(save);
        await expectElementKind(apply, 'button').click();

        const store = await waitForAppliedStore(app);
        expect(store.log_enabled).toBe('false');
        expect(store.log_base_directory).toBe(
          '${documents}/elder-terms/${name}'
        );
        expect(store.log_file_name_format).toBe(
          '${YYYY}-${MM}-${DD}/${name}_${hh}-${mm}-${ss}.txt'
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
        '--show-border=true',
        '--border-width=7',
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
        const showBorder = expectElementKind(
          await app.getById('settings_terminal_show_border_combo'),
          'comboBox'
        );
        const borderWidth = expectElementKind(
          await app.getById('settings_terminal_border_width_entry'),
          'entry'
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
        await expectSelectedComboValue(
          app,
          'settings_terminal_show_border_combo',
          'Enabled'
        );
        await expectNumericEntryValue(borderWidth, 7);
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
        await showBorder.selectChildAt(2);
        await setNumericEntryValue(borderWidth, 6);
        await scrollTerminalPageToBottom(app);
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
        expect(store.show_border).toBe('false');
        expect(store.border_width).toBe('6');
        expect(store.zoom_in_key).toBe('alt+Up');
        expect(store.zoom_out_key).toBe('');
        expect(store.send_break_key).toBe('shift+F12');
      }
    );
  });

  it('uses whole-list font modes and selects a family without changing size', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        '--save',
        '--global=terminal.font_families=Monospace;IPAGothic;',
      ],
      async ({ app, directory }) => {
        await showTerminalPage(app);
        await scrollTerminalPageToBottom(app);
        await expectSelectedComboValue(
          app,
          'settings_terminal_fonts_mode_combo',
          'Inherited from: global settings'
        );
        const page = await app.getById('settings_terminal_page');
        const capture = await captureWhenVisuallyStable(
          page,
          'settings-widget-terminal-font-families'
        );
        let visualError: unknown;
        try {
          await expectCaptureToMatchFixture(
            capture,
            'settings-widget-terminal-font-families',
            fixturePath('settings-widget-terminal-font-families'),
            directory,
            visualComparisonOptions
          );
        } catch (error) {
          visualError = error;
        }
        await confirmSelectedFont(
          app,
          'settings_terminal_font_choose_1',
          'Select Terminal Font'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_fonts_mode_combo',
          'Specify for this connection'
        );
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        let applied = await waitForAppliedStore(app);
        expect(applied.font_families_explicit).toBe('true');
        expect(applied.zoom).toBe('1');
        await waitForResult(async () =>
          expect((await app.output()).stdout).toContain(
            'APPLIED_FONTS ["Monospace","IPAGothic"]'
          )
        );
        const mode = expectElementKind(
          await app.getById('settings_terminal_fonts_mode_combo'),
          'comboBox'
        );
        await mode.selectChildAt(1);
        await expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        ).click();
        expect(
          (await waitForPrintedStore(app, 'SAVED')).font_families_explicit
        ).toBe('true');
        await waitForResult(async () =>
          expect((await app.output()).stdout).toContain(
            'SAVED_FONTS ["Noto Sans Mono","Monospace"]'
          )
        );
        await mode.selectChildAt(0);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          applied = await waitForAppliedStore(app);
          expect(applied.font_families_explicit).toBe('false');
          expect(applied.font_families_source).toBe('global');
        });
        expect(visualError).toBeUndefined();
      }
    );
  }, 60_000);

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

  it('validates and applies a custom terminal BEL sound file', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app, directory }) => {
        await showTerminalPage(app);
        const bellSound = expectElementKind(
          await app.getById('settings_terminal_bell_sound_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        expect(await bellSound.text()).toBe('');
        await waitForEntryPlaceholder(
          app,
          'settings_terminal_bell_sound_entry',
          'default (built-in default)'
        );

        await bellSound.setText('relative.wav');
        await waitForChangedState(app, 'CHANGED dirty=true valid=false');
        await expectInsensitive(apply);
        await expectInsensitive(save);

        const soundPath = join(directory, 'bell.wav');
        await writeFile(soundPath, 'test');
        await bellSound.setText(soundPath);
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectSensitive(apply);
        await expectSensitive(save);
        await expectElementKind(apply, 'button').click();
        expect((await waitForAppliedStore(app)).bell_sound).toBe(soundPath);
      }
    );
  });

  it('selects a terminal BEL sound file through a cancellable chooser', async (context) => {
    const root = await mkdtemp(join(tmpdir(), 'elder-terms-bell-sound-'));
    const soundPath = join(root, 'selected-bell.ogg');
    try {
      await writeFile(soundPath, 'test');
      await runSharedGtkTest(
        context,
        ['--page=terminal', '--save', `--bell-sound-dialog-file=${soundPath}`],
        async ({ app }) => {
          await showTerminalPage(app);
          const bellSound = expectElementKind(
            await app.getById('settings_terminal_bell_sound_entry'),
            'entry'
          );
          const scrollbar = expectElementKind(
            await app.getById('settings_terminal_page_scrollbar'),
            'scrollbar'
          );
          const range = await scrollbar.valueInfo();
          await scrollbar.setValue(range.maximum);

          const browse = expectElementKind(
            await app.getById('settings_terminal_bell_sound_browse_button'),
            'button'
          );
          await waitForResult(async () => {
            expect((await browse.info()).states).toContain('showing');
          });

          await browse.click();
          await waitForResult(async () => {
            expect(await app.getWindowCount()).toBe(2);
          });
          expectElementKind(
            await app.getById('settings_terminal_bell_sound_dialog'),
            'container'
          );
          await app.input.pressKey('Escape');
          await waitForResult(async () => {
            expect(await app.getWindowCount()).toBe(1);
            expect(await bellSound.text()).toBe('');
          });

          await browse.click();
          await expectElementKind(
            await app.getById('settings_terminal_bell_sound_open_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(await app.getWindowCount()).toBe(1);
            expect(await bellSound.text()).toBe(soundPath);
          });
          await waitForChangedState(app, 'CHANGED dirty=true valid=true');
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          expect((await waitForAppliedStore(app)).bell_sound).toBe(soundPath);
        }
      );
    } finally {
      await rm(root, { force: true, recursive: true });
    }
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

  it('accepts only terminal border widths from 1 through 1000 pixels', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        const borderWidth = expectElementKind(
          await app.getById('settings_terminal_border_width_entry'),
          'entry'
        );
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

        await waitForEntryPlaceholder(
          app,
          'settings_terminal_border_width_entry',
          '4 (built-in default)'
        );
        await borderWidth.setText('0');
        await waitForEntryIconTooltip(
          app,
          'settings_terminal_border_width_entry',
          'Value must be between 1 and 1000'
        );
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await borderWidth.setText('1001');
        await waitForEntryIconTooltip(
          app,
          'settings_terminal_border_width_entry',
          'Value must be between 1 and 1000'
        );
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await borderWidth.setText('1000');
        await waitForResult(async () => {
          await expectSensitive(apply);
          await expectSensitive(save);
        });
        await expectElementKind(apply, 'button').click();
        expect((await waitForAppliedStore(app)).border_width).toBe('1000');
      }
    );
  });

  it('captures terminal key bindings with live modifier state', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        await scrollTerminalPageToBottom(app);
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

        await scrollTerminalPageToBottom(app);
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
      '--global=terminal.show_border=true',
      '--global=terminal.border_width=9',
      '--global=terminal.encoding=CP932',
      '--global=terminal.backspace_code=del',
      '--global=terminal.cursor_key_mode=normal',
      '--global=terminal.zoom_in_key=alt+plus',
      '--global=terminal.zoom_out_key=alt+minus',
      '--global=terminal.send_break_key=alt+F12',
      '--global=local.command_line=global-process',
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
      '--global=log.file_name_format=${name}.global.log',
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
        'show_border',
        'border_width',
        'encoding',
        'backspace_code',
        'cursor_key_mode',
        'zoom_in_key',
        'zoom_out_key',
        'send_break_key',
        'local_command_line',
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
      await expectSelectedComboValue(
        app,
        'settings_terminal_show_border_combo',
        'Enabled (global default)'
      );
      await expectInheritedEntry(
        app,
        'settings_terminal_border_width_entry',
        '9 (global default)'
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

      await expectInheritedEntry(
        app,
        'settings_local_command_line_entry',
        'global-process (global default)'
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
        '${name}.global.log (global default)'
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
        await scrollTerminalPageToBottom(app);
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

        await scrollTerminalPageToBottom(app);
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
          'Local',
          'TELNET',
          'Serial',
          'SSH',
          'SFTP',
          'FTP',
          'WebDAV',
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
        expect(
          generalIds.has('global_settings_general_ui_language_combo')
        ).toBe(false);
        expect(
          generalIds.has('global_settings_general_startup_mode_combo')
        ).toBe(false);
        expect(
          generalIds.has('global_settings_general_open_application_entry')
        ).toBe(false);
        await expect(app.getById('settings_notebook')).rejects.toThrow();
        expectElementKind(
          await app.getById('global_settings_general_type_combo'),
          'comboBox'
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
        '--global=log.file_name_format=${name}.log',
        '--rebase-global=terminal.width=100',
        '--rebase-global=terminal.encoding=UTF-8',
        '--rebase-global=log.file_name_format=${YYYY}.log',
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
        expect(rebased.log_file_name_format).toBe('${YYYY}.log');
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
