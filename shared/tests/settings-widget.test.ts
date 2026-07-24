import { fileURLToPath } from 'node:url';
import type {
  GtkApp,
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
  readonly name: string;
  readonly auto_close: string;
  readonly backspace_code: string;
  readonly cursor_key_mode: string;
  readonly encoding: string;
  readonly height: string;
  readonly log_base_directory: string;
  readonly log_enabled: string;
  readonly log_file_name_format: string;
  readonly log_mode: string;
  readonly ssh_address: string;
  readonly ssh_identity_file: string;
  readonly ssh_port: string;
  readonly ssh_terminal_type: string;
  readonly ssh_username: string;
  readonly sftp_local_directory: string;
  readonly sftp_remote_directory: string;
  readonly telnet_address: string;
  readonly telnet_port: string;
  readonly telnet_terminal_type: string;
  readonly serial_baudrate: string;
  readonly serial_bits: string;
  readonly serial_carrier_detect: string;
  readonly serial_device: string;
  readonly serial_flow_control: string;
  readonly serial_parity: string;
  readonly serial_stop_bit: string;
  readonly transfer_base_path: string;
  readonly text_send_bytes_per_second: string;
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

const showTerminalPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const width = await app.getById('settings_terminal_width_spin');
    expect((await width.info()).states).toContain('showing');
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
    const device = await app.getById('settings_serial_device_entry');
    expect((await device.info()).states).toContain('showing');
  });
};

const showTransferPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const basePath = await app.getById('settings_transfer_base_path_entry');
    expect((await basePath.info()).states).toContain('showing');
  });
};

const showLoggingPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const enabled = await app.getById('settings_log_enabled_check');
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

const visibleSettingsTabNames = async (app: GtkApp): Promise<string[]> => {
  const notebook = expectElementKind(
    await app.getById('settings_notebook'),
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
  expectedName: string
): Promise<void> => {
  const notebook = expectElementKind(
    await app.getById('settings_notebook'),
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
  const capture = await (await app.getById(testCase.pageId)).capture();
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
  waitForResult(async () => {
    const output = await app.output();
    const line = output.stdout
      .split('\n')
      .find((candidate) => candidate.startsWith('APPLIED '));
    expect(line).toBeDefined();
    return parseAppliedStore(line as string);
  });

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
  blurTarget: GtkWidgetElement
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
  });
};

describe.concurrent('shared settings widget', () => {
  it('orders connection settings before Terminal when available', async (context) => {
    const cases = [
      {
        args: [] as const,
        expected: ['General', 'Terminal', 'Transfer', 'Logging'],
      },
      {
        args: ['--type=telnet'] as const,
        expected: ['General', 'TELNET', 'Terminal', 'Transfer', 'Logging'],
      },
      {
        args: ['--type=serial'] as const,
        expected: ['General', 'Serial', 'Terminal', 'Transfer', 'Logging'],
      },
      {
        args: ['--type=ssh'] as const,
        expected: ['General', 'SSH', 'Terminal', 'Transfer', 'Logging'],
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

  it('exposes launcher draft state without its internal action row', async (context) => {
    await runSharedGtkTest(
      context,
      ['--hide-actions', '--page=terminal'],
      async ({ app }) => {
        await showTerminalPage(app);
        await expect(app.getById('settings_action_row')).rejects.toThrow();

        const width = expectElementKind(
          await app.getById('settings_terminal_width_spin'),
          'spinButton'
        );
        await width.setValue(91);
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
        args: ['--page=terminal'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_general_name_entry'),
              'entry'
            ).text()
          ).toBe('fixture');
          await expectSelectedConnectionType(app, 'Local');
          await expectSensitive(
            await app.getById('settings_general_type_combo')
          );
        },
        differsFrom: undefined,
        fixtureName: 'settings-widget-general-type-local-editable',
        pageId: 'settings_notebook',
        prepare: stayOnInitialPage,
      },
      {
        args: ['--page=telnet', '--type=telnet'],
        assert: async (app) => {
          await expectSelectedConnectionType(app, 'TELNET');
          await expectSensitive(
            await app.getById('settings_general_type_combo')
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
          await expectSelectedConnectionType(app, 'Local');
          await expectInsensitive(
            await app.getById('settings_general_type_combo')
          );
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
  });

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

  it('matches Terminal visual fixtures for each terminal setting value', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=terminal'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_encoding_entry'),
              'entry'
            ).text()
          ).toBe('Default (UTF-8)');
          await expectSelectedComboValue(
            app,
            'settings_terminal_backspace_code_combo',
            'Default (DEL)'
          );
          await expectSelectedComboValue(
            app,
            'settings_terminal_cursor_key_mode_combo',
            'Default (Normal)'
          );
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_width_spin'),
              'spinButton'
            ).value()
          ).toBe(80);
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_zoom_in_key_entry'),
              'entry'
            ).text()
          ).toBe('ctrl+plus');
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_zoom_out_key_entry'),
              'entry'
            ).text()
          ).toBe('ctrl+minus');
        },
        differsFrom: undefined,
        fixtureName: 'settings-widget-terminal-page-default',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--width=88'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_width_spin'),
              'spinButton'
            ).value()
          ).toBe(88);
        },
        differsFrom: 'settings-widget-terminal-page-default',
        fixtureName: 'settings-widget-terminal-width-88',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--height=31'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_height_spin'),
              'spinButton'
            ).value()
          ).toBe(31);
        },
        differsFrom: 'settings-widget-terminal-page-default',
        fixtureName: 'settings-widget-terminal-height-31',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--zoom=1.25'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_zoom_spin'),
              'spinButton'
            ).value()
          ).toBe(1.25);
        },
        differsFrom: 'settings-widget-terminal-page-default',
        fixtureName: 'settings-widget-terminal-zoom-1.25',
        pageId: 'settings_terminal_page',
        prepare: showTerminalPage,
      },
      {
        args: ['--page=terminal', '--auto-close=false'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_auto_close_check'),
              'checkbox'
            ).isChecked()
          ).toBe(false);
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

  it('matches the default Logging visual fixture', async (context) => {
    const testCase: SettingVisualCase = {
      args: ['--page=logging'],
      assert: async (app) => {
        expect(
          await expectElementKind(
            await app.getById('settings_log_enabled_check'),
            'checkbox'
          ).isChecked()
        ).toBe(false);
        expect(
          await expectElementKind(
            await app.getById('settings_log_base_directory_entry'),
            'entry'
          ).text()
        ).toBe('{documents}/logs/');
        expect(
          await expectElementKind(
            await app.getById('settings_log_file_name_format_entry'),
            'entry'
          ).text()
        ).toBe('{YYYYMMDD}_{hhmmss}_{fff}.txt');
        await expectSelectedComboValue(app, 'settings_log_mode_combo', 'Raw');
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
            await app.getById('settings_telnet_port_spin'),
            'spinButton'
          );
          const terminalType = expectElementKind(
            await app.getById('settings_telnet_terminal_type_entry'),
            'entry'
          );
          expect(await address.text()).toBe('127.0.0.1');
          expect(await port.value()).toBe(23);
          expect(await terminalType.text()).toBe('xterm-256color');
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
          expect(
            await expectElementKind(
              await app.getById('settings_telnet_port_spin'),
              'spinButton'
            ).value()
          ).toBe(2323);
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
            await app.getById('settings_telnet_port_spin')
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
          await app.getById('settings_telnet_port_spin'),
          'spinButton'
        );
        const terminalType = expectElementKind(
          await app.getById('settings_telnet_terminal_type_entry'),
          'entry'
        );
        expect(await address.text()).toBe('example.test');
        expect(await port.value()).toBe(2323);
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
          expect(await terminalType.text()).toBe('xterm-256color');
        });
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.telnet_terminal_type).toBe('xterm-256color');
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
            await app.getById('settings_ssh_port_spin'),
            'spinButton'
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
          expect(await address.text()).toBe('ssh.example.test');
          expect(await port.value()).toBe(22);
          expect(await username.text()).toBe('');
          expect(await identity.text()).toBe('');
          expect(await terminalType.text()).toBe('xterm-256color');
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
          await expectInsensitive(await app.getById('settings_ssh_port_spin'));
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
          await app.getById('settings_ssh_port_spin'),
          'spinButton'
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
        await port.setValue(2222);
        await username.setText('alice');
        await identity.setText('~/.ssh/id_test');
        await clickWidget(app, terminalType);
        await terminalType.setText('   ');
        await clickWidget(app, address);
        await waitForResult(async () => {
          expect(await terminalType.text()).toBe('xterm-256color');
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
        expect(store.backspace_code).toBe('del');
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
        '--serial-carrier-detect=dsr',
      ],
      async ({ app }) => {
        await showSerialPage(app);

        const device = expectElementKind(
          await app.getById('settings_serial_device_entry'),
          'entry'
        );
        const baudrate = expectElementKind(
          await app.getById('settings_serial_baudrate_spin'),
          'spinButton'
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

        expect(await device.text()).toBe('/dev/ttyUSB9');
        expect(await baudrate.value()).toBe(115200);
        await expectSelectedComboValue(app, 'settings_serial_bits_combo', '7');
        await expectSelectedComboValue(
          app,
          'settings_serial_parity_combo',
          'e'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_stop_bit_combo',
          '2'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_flow_control_combo',
          'xon'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_carrier_detect_combo',
          'dsr'
        );
        await expectSensitive(device);
        await expectSensitive(baudrate);
        await expectSensitive(bits);
        await expectSensitive(parity);
        await expectSensitive(stopBit);
        await expectSensitive(flowControl);
        await expectSensitive(carrierDetect);

        await device.setText('/dev/ttyUSB10');
        await baudrate.setValue(57600);
        await bits.selectChildAt(3);
        await parity.selectChildAt(2);
        await stopBit.selectChildAt(0);
        await flowControl.selectChildAt(2);
        await carrierDetect.selectChildAt(1);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.type).toBe('serial');
        expect(store.serial_device).toBe('/dev/ttyUSB10');
        expect(store.serial_baudrate).toBe('57600');
        expect(store.serial_bits).toBe('8');
        expect(store.serial_parity).toBe('o');
        expect(store.serial_stop_bit).toBe('1');
        expect(store.serial_flow_control).toBe('hard');
        expect(store.serial_carrier_detect).toBe('cts');
      }
    );
  });

  it('shows Transfer controls and applies transfer edits', async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=transfer',
        '--transfer-base-path=file:///tmp/elder-terms-transfer',
        '--text-send-bytes-per-second=4096',
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
          await app.getById('settings_transfer_text_send_rate_spin'),
          'spinButton'
        );
        expect(await basePath.text()).toBe('file:///tmp/elder-terms-transfer');
        expect(await textSendRate.value()).toBe(4096);
        await expectSelectedComboValue(
          app,
          'settings_transfer_zmodem_autostart_combo',
          'Disabled'
        );
        await expectSensitive(basePath);
        await expectSensitive(textSendRate);
        await expectSensitive(zmodemAutostart);

        await basePath.setText('file:///tmp/elder-terms-downloads');
        await textSendRate.setValue(2048);
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
          await app.getById('settings_log_enabled_check'),
          'checkbox'
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

        expect(await enabled.isChecked()).toBe(true);
        expect(await baseDirectory.text()).toBe('/tmp/elder-terms-log');
        expect(await fileNameFormat.text()).toBe(
          '{YYYY-MM-DD}/{hh:mm:ss}_{fff}.txt'
        );
        await expectSelectedComboValue(
          app,
          'settings_log_mode_combo',
          'Cooked'
        );

        await fileNameFormat.setText('../outside.log');
        await waitForChangedState(app, 'CHANGED dirty=true valid=false');
        await expectInsensitive(apply);
        await expectInsensitive(save);

        await fileNameFormat.setText(
          '{YYYY}-{MM}-{DD}/{name}_{hh}-{mm}-{ss}.txt'
        );
        await baseDirectory.setText('{documents}/elder-terms/{name}');
        await enabled.toggle();
        await mode.selectChildAt(0);
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

  it('updates General and TELNET state from the editable connection type', async (context) => {
    await runSharedGtkTest(context, [], async ({ app }) => {
      const combo = expectElementKind(
        await app.getById('settings_general_type_combo'),
        'comboBox'
      );
      await expectSensitive(combo);

      const telnetPage = await app.getById('settings_telnet_page');
      expect((await telnetPage.info()).states).not.toContain('visible');

      await combo.selectChildAt(1);
      await waitForResult(async () => {
        expect((await telnetPage.info()).states).toContain('visible');
      });
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();

      const store = await waitForAppliedStore(app);
      expect(store.type).toBe('telnet');
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

      await combo.selectChildAt(2);
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

  it('updates General and SSH state with a DEL default', async (context) => {
    await runSharedGtkTest(context, [], async ({ app }) => {
      const combo = expectElementKind(
        await app.getById('settings_general_type_combo'),
        'comboBox'
      );
      await expectSensitive(combo);

      const sshPage = await app.getById('settings_ssh_page');
      expect((await sshPage.info()).states).not.toContain('visible');

      await combo.selectChildAt(3);
      await waitForResult(async () => {
        expect((await sshPage.info()).states).toContain('visible');
      });
      await selectSettingsTab(app, 'Terminal');
      await showTerminalPage(app);
      await expectSelectedComboValue(
        app,
        'settings_terminal_backspace_code_combo',
        'Default (DEL)'
      );
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();

      const store = await waitForAppliedStore(app);
      expect(store.type).toBe('ssh');
      expect(store.backspace_code).toBe('del');
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
          await app.getById('settings_telnet_port_spin'),
          'spinButton'
        );
        const terminalType = expectElementKind(
          await app.getById('settings_telnet_terminal_type_entry'),
          'entry'
        );
        expect(await address.text()).toBe('runtime.example');
        expect(await port.value()).toBe(10023);
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
          await app.getById('settings_serial_device_entry'),
          'entry'
        );
        const baudrate = expectElementKind(
          await app.getById('settings_serial_baudrate_spin'),
          'spinButton'
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
        expect(await device.text()).toBe('/dev/ttyUSB11');
        expect(await baudrate.value()).toBe(38400);
        await expectSelectedComboValue(app, 'settings_serial_bits_combo', '6');
        await expectSelectedComboValue(
          app,
          'settings_serial_parity_combo',
          'o'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_stop_bit_combo',
          '2'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_flow_control_combo',
          'hard'
        );
        await expectSelectedComboValue(
          app,
          'settings_serial_carrier_detect_combo',
          'cts'
        );
        await expectInsensitive(device);
        await expectSensitive(baudrate);
        await expectSensitive(bits);
        await expectSensitive(parity);
        await expectSensitive(stopBit);
        await expectSensitive(flowControl);
        await expectSensitive(carrierDetect);

        await baudrate.setValue(57600);
        await bits.selectChildAt(3);
        await parity.selectChildAt(1);
        await stopBit.selectChildAt(0);
        await flowControl.selectChildAt(1);
        await carrierDetect.selectChildAt(2);
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
        '--zoom=1.25',
        '--auto-close=false',
      ],
      async ({ app, directory }) => {
        await showTerminalPage(app);

        const width = expectElementKind(
          await app.getById('settings_terminal_width_spin'),
          'spinButton'
        );
        const height = expectElementKind(
          await app.getById('settings_terminal_height_spin'),
          'spinButton'
        );
        const zoom = expectElementKind(
          await app.getById('settings_terminal_zoom_spin'),
          'spinButton'
        );
        const autoClose = expectElementKind(
          await app.getById('settings_terminal_auto_close_check'),
          'checkbox'
        );
        const zoomInKey = expectElementKind(
          await app.getById('settings_terminal_zoom_in_key_entry'),
          'entry'
        );
        const zoomOutKey = expectElementKind(
          await app.getById('settings_terminal_zoom_out_key_entry'),
          'entry'
        );
        expect(await width.value()).toBe(88);
        expect(await height.value()).toBe(31);
        expect(await zoom.value()).toBe(1.25);
        expect(await autoClose.isChecked()).toBe(false);
        expect(await zoomInKey.text()).toBe('ctrl+plus');
        expect(await zoomOutKey.text()).toBe('ctrl+minus');

        const terminalPageCapture = await (
          await app.getById('settings_terminal_page')
        ).capture();
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

        await width.setValue(81);
        await height.setValue(25);
        await zoom.setValue(1.1);
        await autoClose.toggle();
        await captureKeyBinding(app, zoomInKey, ['alt'], 'Up');
        await clearKeyBinding(app, zoomOutKey, width);
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.width).toBe('81');
        expect(store.height).toBe('25');
        expect(Number(store.zoom)).toBeCloseTo(1.1);
        expect(store.auto_close).toBe('true');
        expect(store.zoom_in_key).toBe('alt+Up');
        expect(store.zoom_out_key).toBe('');
      }
    );
  });

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
        expect(await encoding.text()).toBe('Default (UTF-8)');
        await expectSelectedComboValue(
          app,
          'settings_terminal_backspace_code_combo',
          'Default (BS)'
        );
        await expectSelectedComboValue(
          app,
          'settings_terminal_cursor_key_mode_combo',
          'Default (ADM3)'
        );

        await encoding.setText('CP932');
        await backspace.selectChildAt(2);
        await cursorKeys.selectChildAt(1);
        await waitForChangedState(app, 'CHANGED dirty=true valid=true');
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.encoding).toBe('CP932');
        expect(store.backspace_code).toBe('del');
        expect(store.cursor_key_mode).toBe('normal');
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

  it('captures terminal key bindings with live modifier state', async (context) => {
    await runSharedGtkTest(
      context,
      ['--page=terminal', '--save'],
      async ({ app }) => {
        await showTerminalPage(app);
        const zoomInKey = expectElementKind(
          await app.getById('settings_terminal_zoom_in_key_entry'),
          'entry'
        );
        const width = expectElementKind(
          await app.getById('settings_terminal_width_spin'),
          'spinButton'
        );

        expect(await zoomInKey.text()).toBe('ctrl+plus');
        expect((await zoomInKey.info()).states).not.toContain('editable');
        await clickWidget(app, zoomInKey);
        await expectEntryText(zoomInKey, '');
        await clickWidget(app, width);
        await expectEntryText(zoomInKey, 'ctrl+plus');
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
          await clickWidget(app, width);
          await expectEntryText(zoomInKey, 'BackSpace');
        } finally {
          await app.input.setModifier('control', false);
        }

        await clearKeyBinding(app, zoomInKey, width);
        await expectEntryText(zoomInKey, '');

        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        expect((await waitForAppliedStore(app)).zoom_in_key).toBe('');
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
        const apply = await app.getById('settings_apply_button');
        const save = await app.getById('settings_save_button');

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
        await expectElementKind(apply, 'button').click();
        const store = await waitForAppliedStore(app);
        expect(store.zoom_in_key).toBe('alt+F1');
        expect(store.zoom_out_key).toBe('alt+F2');
      }
    );
  });

  it('does not apply draft terminal edits when cancelled', async (context) => {
    await runSharedGtkTest(context, ['--page=terminal'], async ({ app }) => {
      await showTerminalPage(app);
      await expectElementKind(
        await app.getById('settings_terminal_width_spin'),
        'spinButton'
      ).setValue(90);
      await expectElementKind(
        await app.getById('settings_terminal_height_spin'),
        'spinButton'
      ).setValue(40);
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
