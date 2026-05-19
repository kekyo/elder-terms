import { fileURLToPath } from 'node:url';
import type { GtkApp, GtkWidgetElement } from 'gestament';
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
  readonly auto_close: string;
  readonly height: string;
  readonly telnet_address: string;
  readonly telnet_port: string;
  readonly serial_baudrate: string;
  readonly serial_bits: string;
  readonly serial_carrier_detect: string;
  readonly serial_device: string;
  readonly serial_flow_control: string;
  readonly serial_parity: string;
  readonly serial_stop_bit: string;
  readonly type: string;
  readonly width: string;
  readonly zoom: string;
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

const showSerialPage = async (app: GtkApp): Promise<void> => {
  await waitForResult(async () => {
    const device = await app.getById('settings_serial_device_entry');
    expect((await device.info()).states).toContain('showing');
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

describe('shared settings widget', () => {
  it('matches General visual fixtures for connection type and runtime state', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=terminal'],
        assert: async (app) => {
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

  it('matches Terminal visual fixtures for each terminal setting value', async (context) => {
    const cases: readonly SettingVisualCase[] = [
      {
        args: ['--page=terminal'],
        assert: async (app) => {
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_width_spin'),
              'spinButton'
            ).value()
          ).toBe(80);
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
          expect(await address.text()).toBe('127.0.0.1');
          expect(await port.value()).toBe(23);
          await expectSensitive(address);
          await expectSensitive(port);
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
        args: ['--page=telnet', '--runtime', '--type=telnet'],
        assert: async (app) => {
          await expectInsensitive(
            await app.getById('settings_telnet_address_entry')
          );
          await expectInsensitive(
            await app.getById('settings_telnet_port_spin')
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
        expect(await address.text()).toBe('example.test');
        expect(await port.value()).toBe(2323);
        await expectSensitive(address);
        await expectSensitive(port);

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
        expect(await address.text()).toBe('runtime.example');
        expect(await port.value()).toBe(10023);
        await expectInsensitive(address);
        await expectInsensitive(port);
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
        expect(await width.value()).toBe(88);
        expect(await height.value()).toBe(31);
        expect(await zoom.value()).toBe(1.25);
        expect(await autoClose.isChecked()).toBe(false);

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
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();

        const store = await waitForAppliedStore(app);
        expect(store.width).toBe('81');
        expect(store.height).toBe('25');
        expect(Number(store.zoom)).toBeCloseTo(1.1);
        expect(store.auto_close).toBe('true');
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
