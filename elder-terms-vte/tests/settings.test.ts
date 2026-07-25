import { chmod, readFile, writeFile } from 'node:fs/promises';
import { createServer, type Server, type Socket } from 'node:net';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import type {
  GtkApp,
  GtkEntryElement,
  GtkKeyboardModifier,
  GtkKeyInput,
  GtkMenuItemElement,
  GtkWidgetElement,
} from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import { expectElementKind } from './test-helpers';
import {
  assertTerminalTextGridMatches,
  defaultColumns,
  defaultRows,
  expectFixtureVteGridSize,
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
    'settings_terminal_auto_close_check'
  );
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
      const enabled = await app.getById('settings_log_enabled_check');
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
): Promise<void> => {
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
    return;
  }

  throw new Error(`Settings notebook tab was not found: ${tabName}`);
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

describe.concurrent('elder-terms-vte settings', () => {
  it('shows an explicit connection name with the backend title', async (context) => {
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
          await expectMainWindowTitle(
            app,
            'elder-terms: Tokyo / Lab (local terminal)'
          );
        }
      );
    });
  });

  it('shows the local terminal connection in the main window title', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await expectMainWindowTitle(
        app,
        'elder-terms: elder-terms (local terminal)'
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
          await mainWindow.activate();
          await app.input.pressKey('x');
          await app.input.pressKey('Return');

          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);
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
        await expectMainWindowTitle(
          app,
          'elder-terms: telnet-missing-address (telnet: (unknown))'
        );

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
            'elder-terms: serial-missing-device (serial: (unknown))'
          );

          const output = await app.output();
          expect(output.stderr).toContain(
            'Warning: missing required configuration value [serial] device'
          );
        }
      );
    });
  });

  it('opens the runtime settings dialog from the header bar', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app) => {
      await openSettingsDialog(app);
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
          const logEnabled = expectElementKind(
            await app.getById('settings_log_enabled_check'),
            'checkbox'
          );
          expect(await logEnabled.isChecked()).toBe(true);
          await logEnabled.toggle();
          await expectElementKind(
            await app.getById('settings_save_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);
          await waitForActivityIndicatorImageState(app, 'log', 'off');
          const savedConfig = await readFile(configPath, 'utf8');
          expect(savedConfig).not.toContain('enabled=true');

          await openSettingsDialog(app);
          await showLoggingSettingsPage(app);
          const appliedLogEnabled = expectElementKind(
            await app.getById('settings_log_enabled_check'),
            'checkbox'
          );
          expect(await appliedLogEnabled.isChecked()).toBe(false);
          await appliedLogEnabled.toggle();
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

      await expectSelectedConnectionType(app, 'Local');
      await showTerminalSettingsPage(app);
      expect(
        await expectElementKind(
          await app.getById('settings_terminal_width_spin'),
          'spinButton'
        ).value()
      ).toBe(defaultColumns);
      expect(
        await expectElementKind(
          await app.getById('settings_terminal_height_spin'),
          'spinButton'
        ).value()
      ).toBe(defaultRows);
      expect(
        await expectElementKind(
          await app.getById('settings_terminal_zoom_spin'),
          'spinButton'
        ).value()
      ).toBeCloseTo(1.0);
      expect(
        await expectElementKind(
          await app.getById('settings_terminal_auto_close_check'),
          'checkbox'
        ).isChecked()
      ).toBe(true);
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
          await expectElementKind(
            await app.getById('settings_terminal_width_spin'),
            'spinButton'
          ).value()
        ).toBe(defaultColumns + 1);
        expect(
          await expectElementKind(
            await app.getById('settings_terminal_height_spin'),
            'spinButton'
          ).value()
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
        await expectElementKind(
          await app.getById('settings_terminal_zoom_spin'),
          'spinButton'
        ).value()
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
            await expectMainWindowTitle(
              app,
              `elder-terms: telnet (telnet: 127.0.0.1:${port})`
            );
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
              await expectElementKind(
                await app.getById('settings_telnet_port_spin'),
                'spinButton'
              ).value()
            ).toBe(port);
            expect((await app.output()).stderr).toBe('');
          }
        );
      } finally {
        await closeServer(server);
      }
    });
  });

  it('shows the configured serial connection in the main window title', async (context) => {
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
          await expectMainWindowTitle(
            app,
            'elder-terms: serial (serial: /dev/ttyUSB1:115200:n81n)'
          );
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
          await expectMainWindowTitle(
            app,
            'elder-terms: serial (serial: /dev/ttyUSB0:115200:e72x)'
          );
          await openSettingsDialog(app);

          await expectSelectedConnectionType(app, 'Serial');
          await showSerialSettingsPage(app);
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
          expect(await device.text()).toBe('/dev/ttyUSB0');
          expect(await baudrate.value()).toBe(115200);
          await expectSelectedComboValue(
            app,
            'settings_serial_bits_combo',
            '7'
          );
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
          await expectInsensitive(device);
          await expectSensitive(baudrate);
          await expectSensitive(bits);
          await expectSensitive(parity);
          await expectSensitive(stopBit);
          await expectSensitive(flowControl);
          await expectSensitive(carrierDetect);
          expect((await app.output()).stderr).toBe('');

          await baudrate.setValue(57600);
          await bits.selectChildAt(0);
          await parity.selectChildAt(2);
          await stopBit.selectChildAt(1);
          await flowControl.selectChildAt(2);
          await carrierDetect.selectChildAt(1);
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

  it('updates the serial connection title when runtime settings are applied', async (context) => {
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
          await expectMainWindowTitle(
            app,
            'elder-terms: serial-apply-title (serial: /dev/ttyUSB1:115200:n81n)'
          );
          await openSettingsDialog(app);
          await showSerialSettingsPage(app);

          await expectElementKind(
            await app.getById('settings_serial_baudrate_spin'),
            'spinButton'
          ).setValue(57600);
          await expectElementKind(
            await app.getById('settings_serial_bits_combo'),
            'comboBox'
          ).selectChildAt(0);
          await expectElementKind(
            await app.getById('settings_serial_parity_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectElementKind(
            await app.getById('settings_serial_stop_bit_combo'),
            'comboBox'
          ).selectChildAt(1);
          await expectElementKind(
            await app.getById('settings_serial_flow_control_combo'),
            'comboBox'
          ).selectChildAt(2);
          await expectElementKind(
            await app.getById('settings_apply_button'),
            'button'
          ).click();
          await expectSettingsDialogClosed(app);

          await expectMainWindowTitle(
            app,
            'elder-terms: serial-apply-title (serial: /dev/ttyUSB1:57600:o52h)'
          );
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
            await app.getById('settings_terminal_width_spin'),
            'spinButton'
          ).setValue(defaultColumns + 1);
          await expectElementKind(
            await app.getById('settings_terminal_height_spin'),
            'spinButton'
          ).setValue(defaultRows + 1);
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
        await app.getById('settings_terminal_zoom_spin'),
        'spinButton'
      ).setValue(1.1);
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

          const autoClose = expectElementKind(
            await app.getById('settings_terminal_auto_close_check'),
            'checkbox'
          );
          expect(await autoClose.isChecked()).toBe(true);
          await autoClose.toggle();
          expect(await autoClose.isChecked()).toBe(false);
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
            await app.getById('settings_terminal_width_spin'),
            'spinButton'
          );
          const height = expectElementKind(
            await app.getById('settings_terminal_height_spin'),
            'spinButton'
          );
          await width.setValue(defaultColumns + 1);
          await height.setValue(defaultRows + 1);
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
            await app.getById('settings_terminal_width_spin'),
            'spinButton'
          );
          const height = expectElementKind(
            await app.getById('settings_terminal_height_spin'),
            'spinButton'
          );
          await width.setValue(defaultColumns + 1);
          await height.setValue(defaultRows + 1);
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
        await app.getById('settings_terminal_zoom_spin'),
        'spinButton'
      );
      await zoom.setValue(1.1);
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

          const autoClose = expectElementKind(
            await app.getById('settings_terminal_auto_close_check'),
            'checkbox'
          );
          expect(await autoClose.isChecked()).toBe(true);
          await autoClose.toggle();
          expect(await autoClose.isChecked()).toBe(false);
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
