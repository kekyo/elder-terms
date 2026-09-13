import { spawn, spawnSync } from 'node:child_process';
import { createInterface } from 'node:readline';
import {
  chmod,
  mkdir,
  mkdtemp,
  readFile,
  rename,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { GtkApp, GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import {
  expectCaptureToMatchFixture,
  expectElementKind,
  runLauncherGtkTest,
} from './test-helpers';

const terminalEntriesFixturePath = fileURLToPath(
  new URL('./fixtures/launcher-terminal-entries.png', import.meta.url)
);

const japaneseTestEnvironment = {
  ELDER_TERMS_LOCALE_DIR: fileURLToPath(
    new URL('../../.build/po/', import.meta.url)
  ),
  LANGUAGE: 'ja',
  LC_ALL: 'ja_JP.UTF-8',
} as const;

const selectConnectionRow = async (
  app: GtkApp,
  element: GtkWidgetElement,
  row: number
): Promise<void> => {
  if (element.kind === 'table') {
    // Saving a profile replaces its model rows. Resolve the current cell after
    // AT-SPI has observed that update, before sending any mouse input.
    const bounds = await waitForResult(async () => {
      const cell = await element.cellAt(row, 0);
      expect(cell).toBeDefined();
      const current = (await cell?.capture())?.bounds;
      expect(current).toBeDefined();
      return current;
    });
    if (bounds === undefined) {
      return;
    }
    await app.input.moveMouseTo(
      Math.round(bounds.x + bounds.width / 2),
      Math.round(bounds.y + bounds.height / 2)
    );
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    return;
  }
  if (element.kind === 'tree') {
    await element.selectChildAt(row);
    return;
  }
  throw new Error(`Connection list has unexpected kind ${element.kind}`);
};

const doubleClickConnectionRow = async (
  app: GtkApp,
  element: GtkWidgetElement,
  row: number
): Promise<void> => {
  await selectConnectionRow(app, element, row);
  if (element.kind !== 'table') {
    throw new Error('Double-click test requires a GTK table connection list');
  }
  const cell = await element.cellAt(row, 0);
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

const rightClickConnectionRow = async (
  app: GtkApp,
  element: GtkWidgetElement,
  row: number
): Promise<void> => {
  if (element.kind !== 'table') {
    throw new Error('Context-menu test requires a GTK table connection list');
  }
  const cell = await element.cellAt(row, 0);
  const bounds = (await cell?.capture())?.bounds;
  expect(bounds).toBeDefined();
  if (bounds === undefined) {
    return;
  }
  await app.input.moveMouseTo(
    Math.round(bounds.x + bounds.width / 2),
    Math.round(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('right', true);
  await app.input.setMouseButton('right', false);
};

const replaceFocusedText = async (app: GtkApp, text: string): Promise<void> => {
  await app.input.setModifier('control', true);
  try {
    await app.input.pressKey('a');
  } finally {
    await app.input.setModifier('control', false);
  }
  for (const character of text) {
    await app.input.pressKey(character === ' ' ? 'space' : character);
  }
};

const connectionRowCount = async (
  element: GtkWidgetElement
): Promise<number> => {
  if (element.kind === 'table') {
    return element.getRowCount();
  }
  if (element.kind === 'tree') {
    return element.getChildCount();
  }
  throw new Error(`Connection list has unexpected kind ${element.kind}`);
};

const expectSensitive = async (element: GtkWidgetElement): Promise<void> => {
  expect((await element.info()).states).toContain('sensitive');
};

const expectInsensitive = async (element: GtkWidgetElement): Promise<void> => {
  expect((await element.info()).states).not.toContain('sensitive');
};

const waitForWindowCount = async (
  app: GtkApp,
  expected: number
): Promise<void> => {
  await waitForResult(async () => {
    expect(await app.getWindowCount()).toBe(expected);
  });
};

const openGlobalDefaults = async (app: GtkApp) => {
  await expectElementKind(
    await app.getById('global_defaults_button'),
    'button'
  ).click();
  await waitForWindowCount(app, 2);
  const dialog = expectElementKind(
    await app.getById('global_defaults_dialog'),
    'window'
  );
  return dialog;
};

const openApplicationDialogPage = async (
  app: GtkApp,
  itemId: 'application_settings_menu_item' | 'about_menu_item'
): Promise<void> => {
  await expectElementKind(
    await app.getById('application_menu_button'),
    'toggleButton'
  ).click();
  const item = await waitForResult(async () => {
    const candidate = expectElementKind(await app.getById(itemId), 'menuItem');
    expect((await candidate.info()).states).toContain('showing');
    return candidate;
  });
  await item.click();
  await waitForWindowCount(app, 2);
};

const selectedSettingsTabName = async (
  app: GtkApp,
  idPrefix: string
): Promise<string> => {
  const notebook = expectElementKind(
    await app.getById(`${idPrefix}_notebook`),
    'tabList'
  );
  const selected = await notebook.selectedChildAt(0);
  expect(selected).toBeDefined();
  return (await selected?.info())?.name ?? '';
};

const visibleSettingsTabNames = async (
  app: GtkApp,
  idPrefix: string
): Promise<string[]> => {
  const notebook = expectElementKind(
    await app.getById(`${idPrefix}_notebook`),
    'tabList'
  );
  const names: string[] = [];
  const childCount = await notebook.getChildCount();
  for (let index = 0; index < childCount; index += 1) {
    const tab = await notebook.childAt(index);
    if (tab !== undefined) {
      const info = await tab.info();
      if (info.states.includes('showing')) {
        names.push(info.name);
      }
    }
  }
  return names;
};

const selectSettingsTab = async (
  app: GtkApp,
  idPrefix: string,
  expectedName: string
): Promise<void> => {
  const notebook = expectElementKind(
    await app.getById(`${idPrefix}_notebook`),
    'tabList'
  );
  const childCount = await notebook.getChildCount();
  for (let index = 0; index < childCount; index += 1) {
    const tab = await notebook.childAt(index);
    if (tab !== undefined && (await tab.info()).name === expectedName) {
      await notebook.selectChildAt(index);
      return;
    }
  }
  throw new Error(`Settings tab was not found: ${expectedName}`);
};

const expectSelectedComboValue = async (
  app: GtkApp,
  id: string,
  expectedName: string
): Promise<void> => {
  await waitForResult(async () => {
    const combo = expectElementKind(await app.getById(id), 'comboBox');
    const selected = await combo.selectedChildAt(0);
    expect(selected).toBeDefined();
    expect((await selected?.info())?.name).toBe(expectedName);
  });
};

const iniValueLines = (content: string): string[] =>
  content
    .split('\n')
    .filter((line) => line.length > 0 && !line.startsWith('['));

const prepareProfiles = async (connections: string): Promise<void> => {
  await writeFile(join(connections, 'Alpha.ini'), '[terminal]\nwidth=88\n');
  await writeFile(join(connections, 'Beta.ini'), '[terminal]\nwidth=99\n');
};

const beginDirtyConnectionEdit = async (app: GtkApp, value: number) => {
  const list = await app.getById('connection_list');
  await selectConnectionRow(app, list, 0);
  const width = expectElementKind(
    await app.getById('settings_terminal_width_entry'),
    'entry'
  );
  await waitForResult(async () => {
    expect(Number(await width.text())).toBe(88);
  });
  const apply = expectElementKind(await app.getById('apply_button'), 'button');
  await width.setText(String(value));
  await expectSensitive(apply);
  return { list, width };
};

interface FakeVteContext {
  readonly executable: string;
  readonly capture: string;
  readonly release: () => Promise<void>;
}

const createFakeVte = async (
  hardcodedCapture = false
): Promise<FakeVteContext> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-fake-vte-'));
  const executable = join(directory, 'fake-vte.mjs');
  const capture = join(directory, 'capture.json');
  const captureExpression = hardcodedCapture
    ? JSON.stringify(capture)
    : 'process.env.ELDER_TERMS_TEST_CAPTURE';
  await writeFile(
    executable,
    `#!/usr/bin/env node
import { readFile, writeFile } from 'node:fs/promises';
const args = process.argv.slice(2);
const startupIndex = args.indexOf('-s');
const startupContent = startupIndex < 0 ? null : await readFile(args[startupIndex + 1], 'utf8');
await writeFile(${captureExpression}, JSON.stringify({ args, startupContent }));
`
  );
  await chmod(executable, 0o755);
  return {
    executable,
    capture,
    release: async () => rm(directory, { recursive: true, force: true }),
  };
};

interface SiblingVteLayout extends FakeVteContext {
  readonly launcher: string;
}

const createSiblingVteLayout = async (): Promise<SiblingVteLayout> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-sibling-vte-'));
  const launcherDirectory = join(directory, 'elder-terms');
  const vteDirectory = join(directory, 'elder-terms-vte');
  await Promise.all([
    mkdir(launcherDirectory, { recursive: true }),
    mkdir(vteDirectory, { recursive: true }),
  ]);
  await symlink(
    fileURLToPath(
      new URL('../../.build/elder-terms/elder-terms', import.meta.url)
    ),
    join(launcherDirectory, 'elder-terms')
  );
  const executable = join(vteDirectory, 'elder-terms-vte');
  const capture = join(directory, 'capture.json');
  await writeFile(
    executable,
    `#!/usr/bin/env node
import { writeFile } from 'node:fs/promises';
await writeFile(process.env.ELDER_TERMS_TEST_CAPTURE, JSON.stringify({ args: process.argv.slice(2), startupContent: null }));
`
  );
  await chmod(executable, 0o755);
  return {
    launcher: `${launcherDirectory}/./elder-terms`,
    executable,
    capture,
    release: async () => rm(directory, { recursive: true, force: true }),
  };
};

interface LaunchCapture {
  readonly args: readonly string[];
  readonly startupContent: string | null;
}

const readLaunchCapture = async (path: string): Promise<LaunchCapture> =>
  JSON.parse(await readFile(path, 'utf8')) as LaunchCapture;

describe('elder-terms main window', () => {
  it('creates a default local terminal on the first launch', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await rm(connections, { recursive: true });
      },
      async ({ app }) => {
        const list = await app.getById('connection_list');
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(1);
        });
        await selectConnectionRow(app, list, 0);

        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('settings_general_name_entry'),
              'entry'
            ).text()
          ).toBe('Local terminal');
        });
        await expectSelectedComboValue(
          app,
          'settings_general_type_combo',
          'Local shell (built-in default)'
        );
        expect(
          await expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          ).text()
        ).toBe('');
        await expectInsensitive(
          expectElementKind(await app.getById('apply_button'), 'button')
        );
      }
    );
  });

  it('opens application settings and version information in one dialog', async (context) => {
    const version = spawnSync(
      'npx',
      ['--no-install', 'screw-up', 'format', '-e', '{version}', '-f'],
      {
        cwd: fileURLToPath(new URL('../..', import.meta.url)),
        encoding: 'utf8',
      }
    );
    expect(version.status, version.stderr).toBe(0);
    expect(version.stdout.trim()).not.toBe('');
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      await openApplicationDialogPage(app, 'application_settings_menu_item');
      const dialog = expectElementKind(
        await app.getById('application_dialog'),
        'window'
      );
      expect(await selectedSettingsTabName(app, 'application_dialog')).toBe(
        'Application'
      );
      expectElementKind(
        await app.getById('application_settings_ui_language_combo'),
        'comboBox'
      );
      expectElementKind(
        await app.getById('application_settings_startup_mode_combo'),
        'comboBox'
      );
      expectElementKind(
        await app.getById('application_settings_open_application_entry'),
        'entry'
      );
      expectElementKind(
        await app.getById('application_settings_general_page'),
        'container'
      );
      // Required controls are already present. Inspect the completed dialog
      // tree directly; a missing-control assertion must not wait for a timeout.
      const ids: string[] = [];
      const pending: GtkWidgetElement[] = [dialog];
      while (pending.length > 0) {
        const widget = pending.shift()!;
        ids.push((await widget.info()).accessibleId);
        if ('getChildCount' in widget) {
          const count = await widget.getChildCount();
          for (let index = 0; index < count; index++) {
            const child = await widget.childAt(index);
            if (child !== undefined) pending.push(child);
          }
        }
      }
      expect(ids).not.toContain('application_settings_notebook');
      expect(ids).not.toContain('application_settings_link_add_button');
      await expectElementKind(
        await app.getById('application_dialog_cancel_button'),
        'button'
      ).click();
      await waitForWindowCount(app, 1);

      await openApplicationDialogPage(app, 'about_menu_item');
      expect(await selectedSettingsTabName(app, 'application_dialog')).toBe(
        'About'
      );
      expect(
        await expectElementKind(
          await app.getById('application_about_version_label'),
          'label'
        ).text()
      ).toBe(`Version ${version.stdout.trim()}`);
    });
  });

  for (const { language, env, tabNames } of [
    { language: 'English', env: {}, tabNames: ['Application', 'About'] },
    {
      language: 'Japanese',
      env: japaneseTestEnvironment,
      tabNames: ['アプリケーション', '情報'],
    },
  ]) {
    it(`opens a compact application dialog in ${language}`, async (context) => {
      await runLauncherGtkTest(
        context,
        prepareProfiles,
        async ({ app }) => {
          for (const itemId of [
            'application_settings_menu_item',
            'about_menu_item',
          ] as const) {
            await openApplicationDialogPage(app, itemId);
            const dialog = expectElementKind(
              await app.getById('application_dialog'),
              'window'
            );
            const bounds = await dialog.bounds();
            expect(bounds.width).toBeGreaterThanOrEqual(600);
            expect(bounds.width).toBeLessThanOrEqual(660);
            expect(bounds.height).toBeGreaterThanOrEqual(340);
            expect(bounds.height).toBeLessThanOrEqual(420);

            const tabs = expectElementKind(
              await app.getById('application_dialog_notebook'),
              'tabList'
            );
            for (const [page, tabName] of tabNames.entries()) {
              await tabs.selectChildAt(page);
              expect(
                await selectedSettingsTabName(app, 'application_dialog')
              ).toBe(tabName);
              const pageBounds = await dialog.bounds();
              expect(pageBounds.width).toBe(bounds.width);
              expect(pageBounds.height).toBe(bounds.height);
              expect(
                (
                  await (
                    await app.getById('application_dialog_cancel_button')
                  ).info()
                ).states
              ).toContain('showing');
            }
            await expectElementKind(
              await app.getById('application_dialog_cancel_button'),
              'button'
            ).click();
            await waitForWindowCount(app, 1);
          }
        },
        { args: [], env }
      );
    });
  }

  it('saves application settings without replacing connection defaults', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=window\nopen_application=\n\n[terminal]\nwidth=91\n'
        );
      },
      async ({ app, configHome }) => {
        expect(
          (await (await app.getById('global_defaults_button')).info()).name
        ).toBe('Connection defaults');
        await openApplicationDialogPage(app, 'application_settings_menu_item');
        await expectElementKind(
          await app.getById('application_settings_startup_mode_combo'),
          'comboBox'
        ).selectChildAt(2);
        await expectElementKind(
          await app.getById('application_dialog_save_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);

        const saved = await readFile(
          join(configHome, 'elder-terms', 'global.ini'),
          'utf8'
        );
        expect(saved).toContain('startup_mode=background');
        expect(saved).toContain('width=91');
      }
    );
  });

  it('edits, disables, saves, and resets terminal link rules per connection', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[terminal]\nwidth=91\n',
          'utf8'
        );
      },
      async ({ app, configHome, connections }) => {
        const globalConfigPath = join(configHome, 'elder-terms', 'global.ini');
        const connectionConfigPath = join(connections, 'Alpha.ini');
        const list = await app.getById('connection_list');
        await selectConnectionRow(app, list, 0);
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('settings_general_name_entry'),
              'entry'
            ).text()
          ).toBe('Alpha');
        });
        await selectSettingsTab(app, 'settings', 'Links');

        const save = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        await expectElementKind(
          await app.getById('settings_link_add_button'),
          'button'
        ).click();
        await expectInsensitive(save);

        const id = expectElementKind(
          await app.getById('settings_link_id_entry'),
          'entry'
        );
        const source = expectElementKind(
          await app.getById('settings_link_source_combo'),
          'comboBox'
        );
        const regex = expectElementKind(
          await app.getById('settings_link_regex_entry'),
          'entry'
        );
        const command = expectElementKind(
          await app.getById('settings_link_command_entry'),
          'entry'
        );
        expect(await id.text()).toBe('rule1');
        await id.setText('local-file');
        await source.selectChildAt(1);
        await regex.setText('^file:(?<path>/[^ ]+)$');
        await command.setText('xdg-open');
        await expectElementKind(
          await app.getById('settings_link_argument_add_button'),
          'button'
        ).click();
        await expectElementKind(
          await app.getById('settings_link_argument_0_entry'),
          'entry'
        ).setText('${path}');
        await expectElementKind(
          await app.getById('settings_link_validation_combo'),
          'comboBox'
        ).selectChildAt(1);
        await expectElementKind(
          await app.getById('settings_link_path_entry'),
          'entry'
        ).setText('${path}');
        await expectElementKind(
          await app.getById('settings_link_enabled_combo'),
          'comboBox'
        ).selectChildAt(1);
        await expectSensitive(save);
        await save.click();

        let configured = '';
        await waitForResult(async () => {
          configured = await readFile(connectionConfigPath, 'utf8');
          expect(configured).toContain('[hyperlink]\nenabled=false');
        });
        expect(configured).toContain('[hyperlink.local-file]');
        expect(configured).not.toContain('[hyperlink.http-url-osc8]');
        expect(configured).toContain('source=terminal-text');
        expect(configured).toContain('command=xdg-open');
        expect(configured).toContain('path_validation=existing-local-path');
        expect(configured).toContain('path=${path}');
        expect(configured).toContain('width=88');
        const globalConfigured = await readFile(globalConfigPath, 'utf8');
        expect(globalConfigured).not.toContain('[hyperlink');
        expect(globalConfigured).toContain('width=91');

        await selectSettingsTab(app, 'settings', 'Links');
        await expectElementKind(
          await app.getById('settings_link_reset_button'),
          'button'
        ).click();
        await expectElementKind(
          await app.getById('apply_button'),
          'button'
        ).click();

        let reset = '';
        await waitForResult(async () => {
          reset = await readFile(connectionConfigPath, 'utf8');
          expect(reset).not.toContain('[hyperlink');
        });
        expect(reset).toContain('width=88');
      }
    );
  });

  it('opens the About page directly from a forwarded command line', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app }) => {
        await waitForWindowCount(app, 1);
        expectElementKind(await app.getById('application_dialog'), 'window');
        expect(await selectedSettingsTabName(app, 'application_dialog')).toBe(
          'About'
        );
      },
      {
        args: ['--about'],
        env: {},
      }
    );
  });

  it('publishes a runtime icon and desktop identity for its main window', async (context) => {
    await runLauncherGtkTest(
      context,
      async () => {},
      async ({ app, x11MapRecorder }) => {
        if (x11MapRecorder === undefined) {
          throw new Error('X11 map recorder was not started');
        }
        const mainWindow = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        const windowId = String(
          Number.parseInt((await mainWindow.x11Info()).windowId, 16)
        );
        await x11MapRecorder.flush();
        const mapEvent = x11MapRecorder
          .events()
          .find((event) => event.windowId === windowId);
        expect(mapEvent).toBeDefined();
        expect(mapEvent?.hasIcon).toBe(true);
        expect(mapEvent?.instanceName).toBe('net.kekyo.elder-terms');
        expect(mapEvent?.className).toBe('Elder-terms');
      },
      {
        args: [],
        env: {},
        recordX11Maps: true,
      }
    );
  });

  it('uses a Japanese global UI language from a C UTF-8 build-tree environment', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nui_language=ja\n'
        );
      },
      async ({ app }) => {
        expect(
          (await (await app.getById('global_defaults_button')).info()).name
        ).toBe('接続の既定値');
      }
    );
  });

  it('uses an English global UI language from a Japanese environment', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nui_language=en\n'
        );
      },
      async ({ app }) => {
        expect(
          (await (await app.getById('global_defaults_button')).info()).name
        ).toBe('Connection defaults');
      },
      {
        args: [],
        env: japaneseTestEnvironment,
      }
    );
  });

  it('localizes launcher settings surfaces into Japanese', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app }) => {
        expect((await (await app.getById('new_button')).info()).name).toBe(
          '新規'
        );
        expect((await (await app.getById('connect_button')).info()).name).toBe(
          '起動'
        );
        expect(
          await expectElementKind(
            await app.getById('empty_details_label'),
            'label'
          ).text()
        ).toBe('接続を選択するか、新しい接続を作成してください。');
        expect(
          (await (await app.getById('global_defaults_button')).info()).name
        ).toBe('接続の既定値');
        expect((await (await app.getById('apply_button')).info()).name).toBe(
          '保存'
        );

        const list = await app.getById('connection_list');
        await rightClickConnectionRow(app, list, 0);
        const duplicateItem = await waitForResult(async () => {
          const item = expectElementKind(
            await app.getById('duplicate_connection_menu_item'),
            'menuItem'
          );
          expect((await item.info()).states).toContain('showing');
          return item;
        });
        expect((await duplicateItem.info()).name).toBe('複製');
        await app.input.pressKey('Escape');

        const dialog = await openGlobalDefaults(app);
        await waitForResult(async () => {
          expect((await dialog.x11Info()).title).toBe('接続の既定値');
          expect(await visibleSettingsTabNames(app, 'global_settings')).toEqual(
            [
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
            ]
          );
        });
        expect(
          (await (await app.getById('global_defaults_save_button')).info()).name
        ).toBe('保存');
        const cancel = expectElementKind(
          await app.getById('global_defaults_cancel_button'),
          'button'
        );
        expect((await cancel.info()).name).toBe('キャンセル');
        await cancel.click();
        await waitForWindowCount(app, 1);
      },
      {
        args: [],
        env: japaneseTestEnvironment,
      }
    );
  });

  it('saves a display language change and allows restarting later', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, configHome }) => {
        await openApplicationDialogPage(app, 'application_settings_menu_item');
        const language = expectElementKind(
          await app.getById('application_settings_ui_language_combo'),
          'comboBox'
        );
        await language.selectChildAt(6);
        await expectElementKind(
          await app.getById('application_dialog_save_button'),
          'button'
        ).click();

        await waitForWindowCount(app, 2);
        const restartDialog = expectElementKind(
          await app.getById('ui_language_restart_dialog'),
          'infoBar'
        );
        expect((await restartDialog.info()).states).not.toContain('modal');
        await expectInsensitive(await app.getById('main_window'));
        expect(
          (await (await app.getById('ui_language_restart_now_button')).info())
            .name
        ).toBe('Restart now');
        await expectElementKind(
          await app.getById('ui_language_restart_later_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);

        expect(
          await readFile(join(configHome, 'elder-terms', 'global.ini'), 'utf8')
        ).toContain('ui_language=ja');
        expect(
          (await (await app.getById('global_defaults_button')).info()).name
        ).toBe('Connection defaults');
      }
    );
  });

  it('restarts with the saved display language and restores the inherited environment', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nui_language=ja\n'
        );
      },
      async ({ app }) => {
        expect(
          (await (await app.getById('global_defaults_button')).info()).name
        ).toBe('接続の既定値');
        await openApplicationDialogPage(app, 'application_settings_menu_item');
        const language = expectElementKind(
          await app.getById('application_settings_ui_language_combo'),
          'comboBox'
        );
        await language.selectChildAt(0);
        await expectElementKind(
          await app.getById('application_dialog_save_button'),
          'button'
        ).click();

        await waitForWindowCount(app, 2);
        expect(
          (await (await app.getById('ui_language_restart_later_button')).info())
            .name
        ).toBe('後で');
        const restart = expectElementKind(
          await app.getById('ui_language_restart_now_button'),
          'button'
        );
        expect((await restart.info()).name).toBe('今すぐ再起動');
        await restart.click();

        await waitForResult(async () => {
          expect(
            (await (await app.getById('global_defaults_button')).info()).name
          ).toBe('Connection defaults');
        });
      },
      {
        args: [],
        env: {
          ELDER_TERMS_LOCALE_DIR:
            japaneseTestEnvironment.ELDER_TERMS_LOCALE_DIR,
        },
      }
    );
  });

  it('confirms before restarting with dirty connection edits', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      await selectConnectionRow(app, await app.getById('connection_list'), 0);
      const width = expectElementKind(
        await app.getById('settings_terminal_width_entry'),
        'entry'
      );
      await width.setText('99');

      await openApplicationDialogPage(app, 'application_settings_menu_item');
      await expectElementKind(
        await app.getById('application_settings_ui_language_combo'),
        'comboBox'
      ).selectChildAt(6);
      await expectElementKind(
        await app.getById('application_dialog_save_button'),
        'button'
      ).click();
      await waitForWindowCount(app, 2);
      await expectElementKind(
        await app.getById('ui_language_restart_now_button'),
        'button'
      ).click();

      await waitForWindowCount(app, 2);
      expectElementKind(await app.getById('discard_changes_dialog'), 'infoBar');
      await expectElementKind(
        await app.getById('cancel_discard_button'),
        'button'
      ).click();
      await waitForWindowCount(app, 1);
      expect(await width.text()).toBe('99');
    });
  });

  it('starts unselected in a resizable split layout', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      expect(await app.getWindowCount()).toBe(1);
      const window = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      const splitPane = await app.getById('split_pane');
      const terminalEntriesLabel = await app.getById('terminal_entries_label');
      const left = await app.getById('connection_scroller');
      const right = await app.getById('details_stack');
      const list = await app.getById('connection_list');
      const actionRow = await app.getById('action_row');
      const apply = await app.getById('apply_button');
      const connect = await app.getById('connect_button');

      expect(['table', 'tree']).toContain(list.kind);
      expect((await terminalEntriesLabel.info()).name).toBe('Terminal entries');
      expect((await apply.info()).name).toBe('Save');
      expect((await connect.info()).name).toBe('Launch');
      await expectInsensitive(apply);
      await expectInsensitive(connect);
      const terminalEntriesCapture = await terminalEntriesLabel.capture();
      const leftCapture = await left.capture();
      expect(terminalEntriesCapture.bounds.y).toBeLessThan(
        leftCapture.bounds.y
      );
      expect(leftCapture.bounds.x).toBeLessThan(
        (await right.capture()).bounds.x
      );

      const splitPaneCapture = await splitPane.capture();
      expect(splitPaneCapture.clipped).toBe(false);
      await expectCaptureToMatchFixture(
        splitPaneCapture,
        'launcher-terminal-entries',
        terminalEntriesFixturePath
      );

      const initialBounds = await window.moveTo(100, 100);
      const actionCapture = await actionRow.capture();
      const startX = Math.trunc(
        actionCapture.bounds.x + actionCapture.bounds.width / 2
      );
      const startY = Math.trunc(
        actionCapture.bounds.y + actionCapture.bounds.height / 2
      );
      await app.input.moveMouseTo(startX, startY);
      await app.input.setMouseButton('left', true);
      await app.input.moveMouseTo(startX + 120, startY + 80);
      await app.input.setMouseButton('left', false);

      await waitForResult(async () => {
        const movedBounds = await window.bounds();
        expect(movedBounds.x).toBeGreaterThanOrEqual(initialBounds.x + 80);
        expect(movedBounds.y).toBeGreaterThanOrEqual(initialBounds.y + 30);
        expect(movedBounds.width).toBe(initialBounds.width);
        expect(movedBounds.height).toBe(initialBounds.height);
      });

      const before = await window.bounds();
      await window.resizeTo(before.width + 120, before.height + 80);
      const after = await window.bounds();
      expect(after.width).toBeGreaterThan(before.width);
      expect(after.height).toBeGreaterThan(before.height);
    });
  });

  it('keeps the launcher blocked until a nested CA chooser and settings both close', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, x11MapRecorder }) => {
        if (x11MapRecorder === undefined)
          throw new Error('X11 focus recorder was not started');
        const main = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        await main.moveTo(40, 40);
        const settings = await openGlobalDefaults(app);
        await settings.moveTo(480, 280);
        await selectSettingsTab(app, 'global_settings', 'WebDAV');
        const scroll = expectElementKind(
          await app.getById('global_settings_webdav_page_scrollbar'),
          'scrollbar'
        );
        await scroll.setValue((await scroll.valueInfo()).maximum);
        await expectElementKind(
          await app.getById('global_settings_webdav_ca_file_combo'),
          'comboBox'
        ).selectChildAt(2);
        await expectElementKind(
          await app.getById('global_settings_webdav_ca_browse_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 3);
        const chooserWindow = await app.windowAt(2);
        if (chooserWindow === undefined) {
          throw new Error('CA chooser window was not created');
        }
        const chooser = expectElementKind(chooserWindow, 'window');
        await chooser.moveTo(740, 400);
        await expectInsensitive(main);
        await expectInsensitive(settings);
        const chooserId = String(
          Number.parseInt((await chooser.x11Info()).windowId, 16)
        );
        const mainBounds = await main.bounds();
        await app.input.moveMouseTo(mainBounds.x + 20, mainBounds.y + 20);
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await waitForResult(async () =>
          expect(await x11MapRecorder.focusedWindow()).toBe(chooserId)
        );
        await app.input.pressKey('Escape');
        await waitForWindowCount(app, 2);
        await expectSensitive(settings);
        await expectInsensitive(main);
        await expectElementKind(
          await app.getById('global_defaults_cancel_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);
        await expectSensitive(main);
      },
      { args: [], env: {}, recordX11Maps: true }
    );
  });

  it('opens global defaults independently and disables its parent until closed', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, x11MapRecorder }) => {
        if (x11MapRecorder === undefined) {
          throw new Error('X11 focus recorder was not started');
        }
        const mainWindow = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        await mainWindow.moveTo(40, 40);
        const dialog = await openGlobalDefaults(app);
        await dialog.moveTo(480, 280);

        expect((await dialog.info()).states).not.toContain('modal');
        await expectInsensitive(mainWindow);
        expect(await selectedSettingsTabName(app, 'global_settings')).toBe(
          'General'
        );

        const mainWindowId = String(
          Number.parseInt((await mainWindow.x11Info()).windowId, 16)
        );
        const dialogWindowId = String(
          Number.parseInt((await dialog.x11Info()).windowId, 16)
        );
        const mainBounds = await mainWindow.bounds();
        await app.input.moveMouseTo(mainBounds.x + 20, mainBounds.y + 20);
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await waitForResult(async () => {
          expect(await x11MapRecorder.focusedWindow()).toBe(dialogWindowId);
        });
        await expectElementKind(
          await app.getById('global_defaults_cancel_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);
        await expectSensitive(mainWindow);
        await waitForResult(async () => {
          expect(await x11MapRecorder.focusedWindow()).toBe(mainWindowId);
        });
      },
      {
        args: [],
        env: {},
        recordX11Maps: true,
      }
    );
  });

  it('pads the global defaults action row and moves the dialog when dragged', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      const dialog = await openGlobalDefaults(app);
      const actionRow = expectElementKind(
        await app.getById('global_defaults_action_row'),
        'container'
      );
      const cancel = expectElementKind(
        await app.getById('global_defaults_cancel_button'),
        'button'
      );
      const save = expectElementKind(
        await app.getById('global_defaults_save_button'),
        'button'
      );
      const initialBounds = await dialog.moveTo(200, 160);
      const [actionCapture, cancelCapture, saveCapture] = await Promise.all([
        actionRow.capture(),
        cancel.capture(),
        save.capture(),
      ]);
      const actionBounds = actionCapture.bounds;
      const cancelBounds = cancelCapture.bounds;
      const saveBounds = saveCapture.bounds;

      expect(cancelBounds.y - actionBounds.y).toBeGreaterThanOrEqual(10);
      expect(
        actionBounds.y +
          actionBounds.height -
          (cancelBounds.y + cancelBounds.height)
      ).toBeGreaterThanOrEqual(10);
      expect(
        actionBounds.x + actionBounds.width - (saveBounds.x + saveBounds.width)
      ).toBeGreaterThanOrEqual(12);

      const startX = Math.trunc(actionBounds.x + actionBounds.width / 4);
      const startY = Math.trunc(actionBounds.y + actionBounds.height / 2);
      await app.input.moveMouseTo(startX, startY);
      await app.input.setMouseButton('left', true);
      await app.input.moveMouseTo(startX + 120, startY + 80);
      await app.input.setMouseButton('left', false);

      await waitForResult(async () => {
        const movedBounds = await dialog.bounds();
        expect(movedBounds.x).toBeGreaterThanOrEqual(initialBounds.x + 80);
        expect(movedBounds.y).toBeGreaterThanOrEqual(initialBounds.y + 40);
        expect(movedBounds.width).toBe(initialBounds.width);
        expect(movedBounds.height).toBe(initialBounds.height);
      });

      await cancel.click();
      await waitForWindowCount(app, 1);
    });
  });

  it('opens an all-backend global defaults editor and cancels without saving', async (context) => {
    const originalGlobal =
      '# keep this file unchanged on cancel\n[terminal]\nwidth=91\n';
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await writeFile(join(connections, '..', 'global.ini'), originalGlobal);
      },
      async ({ app, configHome }) => {
        const newButton = expectElementKind(
          await app.getById('new_button'),
          'button'
        );
        const globalButton = expectElementKind(
          await app.getById('global_defaults_button'),
          'button'
        );
        const applyButton = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        const launchButton = expectElementKind(
          await app.getById('connect_button'),
          'button'
        );
        expect((await globalButton.info()).name).toBe('Connection defaults');

        await waitForResult(async () => {
          const [newCapture, globalCapture, applyCapture, launchCapture] =
            await Promise.all([
              newButton.capture(),
              globalButton.capture(),
              applyButton.capture(),
              launchButton.capture(),
            ]);
          const newBounds = newCapture.bounds;
          const globalBounds = globalCapture.bounds;
          const applyBounds = applyCapture.bounds;
          const launchBounds = launchCapture.bounds;
          const newToGlobalGap =
            globalBounds.x - (newBounds.x + newBounds.width);
          const globalToApplyGap =
            applyBounds.x - (globalBounds.x + globalBounds.width);
          expect(newBounds.x + newBounds.width).toBeLessThanOrEqual(
            globalBounds.x
          );
          expect(globalBounds.x + globalBounds.width).toBeLessThan(
            applyBounds.x
          );
          expect(applyBounds.x + applyBounds.width).toBeLessThanOrEqual(
            launchBounds.x
          );
          expect(globalToApplyGap).toBeGreaterThan(newToGlobalGap);
        });

        await openGlobalDefaults(app);
        expectElementKind(
          await app.getById('global_settings_widget_root'),
          'container'
        );
        await waitForResult(async () => {
          expect(await visibleSettingsTabNames(app, 'global_settings')).toEqual(
            [
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
            ]
          );
        });
        expect(
          await app.findById('global_settings_general_name_entry')
        ).toBeUndefined();

        await selectSettingsTab(app, 'global_settings', 'WebDAV');
        await expectElementKind(
          await app.getById('global_settings_webdav_address_entry'),
          'entry'
        ).setText('dav.example.test');
        await expectElementKind(
          await app.getById('global_settings_webdav_base_path_entry'),
          'entry'
        ).setText('/dav/root%20folder/');
        await waitForResult(async () => {
          const preview = expectElementKind(
            await app.getById('global_settings_webdav_url_label'),
            'label'
          );
          expect(await preview.text()).toBe(
            'https://dav.example.test:443/dav/root%20folder/'
          );
        });
        await expectElementKind(
          await app.getById('global_settings_webdav_scheme_combo'),
          'comboBox'
        ).selectChildAt(2);
        await expectElementKind(
          await app.getById('global_settings_webdav_address_entry'),
          'entry'
        ).setText('::1');
        await expectElementKind(
          await app.getById('global_settings_webdav_remote_directory_entry'),
          'entry'
        ).setText('/Documents #');
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('global_settings_webdav_url_label'),
              'label'
            ).text()
          ).toBe('http://[::1]:80/dav/root%20folder/Documents%20%23');
        });

        await selectSettingsTab(app, 'global_settings', 'Terminal');
        const width = expectElementKind(
          await app.getById('global_settings_terminal_width_entry'),
          'entry'
        );
        expect(Number(await width.text())).toBe(91);
        await width.setText('92');
        await expectElementKind(
          await app.getById('global_defaults_cancel_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);

        const globalPath = join(configHome, 'elder-terms', 'global.ini');
        expect(await readFile(globalPath, 'utf8')).toBe(originalGlobal);

        await openGlobalDefaults(app);
        await selectSettingsTab(app, 'global_settings', 'Terminal');
        const reopenedWidth = expectElementKind(
          await app.getById('global_settings_terminal_width_entry'),
          'entry'
        );
        expect(Number(await reopenedWidth.text())).toBe(91);
        await expectElementKind(
          await app.getById('global_defaults_cancel_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);
      }
    );
  });

  it('validates, normalizes, reopens, and fully clears global defaults', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await writeFile(
          join(connections, '..', 'global.ini'),
          [
            '# obsolete comment',
            '[terminal]',
            'width=90',
            'unknown=obsolete',
            '',
            '[transfer]',
            'base_path=file:///tmp/original',
            '',
          ].join('\n')
        );
      },
      async ({ app, configHome }) => {
        const globalPath = join(configHome, 'elder-terms', 'global.ini');
        await openGlobalDefaults(app);
        await selectSettingsTab(app, 'global_settings', 'Terminal');
        const width = expectElementKind(
          await app.getById('global_settings_terminal_width_entry'),
          'entry'
        );
        const save = expectElementKind(
          await app.getById('global_defaults_save_button'),
          'button'
        );
        expect(Number(await width.text())).toBe(90);

        await width.setText('0');
        await waitForResult(async () => {
          await expectInsensitive(save);
        });
        await width.setText('96');
        await waitForResult(async () => {
          await expectSensitive(save);
        });
        await save.click();
        await waitForWindowCount(app, 1);

        const normalized = await waitForResult(async () => {
          const content = await readFile(globalPath, 'utf8');
          expect(iniValueLines(content)).toEqual([
            'width=96',
            'base_path=file:///tmp/original',
          ]);
          return content;
        });
        expect(normalized).not.toContain('# obsolete comment');
        expect(normalized).not.toContain('unknown=obsolete');

        await openGlobalDefaults(app);
        await selectSettingsTab(app, 'global_settings', 'Terminal');
        const reopenedWidth = expectElementKind(
          await app.getById('global_settings_terminal_width_entry'),
          'entry'
        );
        expect(Number(await reopenedWidth.text())).toBe(96);
        await reopenedWidth.setText('');

        await selectSettingsTab(app, 'global_settings', 'Transfer');
        const basePath = expectElementKind(
          await app.getById('global_settings_transfer_base_path_entry'),
          'entry'
        );
        expect(await basePath.text()).toBe('file:///tmp/original');
        await basePath.setText('');
        const clearSave = expectElementKind(
          await app.getById('global_defaults_save_button'),
          'button'
        );
        await waitForResult(async () => {
          await expectSensitive(clearSave);
        });
        await clearSave.click();
        await waitForWindowCount(app, 1);
        await waitForResult(async () => {
          expect(await readFile(globalPath, 'utf8')).toBe('');
        });
      }
    );
  });

  it('keeps the global defaults draft open when saving fails', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await prepareProfiles(connections);
        await mkdir(join(connections, '..', 'global.ini'));
      },
      async ({ app }) => {
        await openGlobalDefaults(app);
        await selectSettingsTab(app, 'global_settings', 'Terminal');
        const width = expectElementKind(
          await app.getById('global_settings_terminal_width_entry'),
          'entry'
        );
        const save = expectElementKind(
          await app.getById('global_defaults_save_button'),
          'button'
        );
        await width.setText('95');
        await waitForResult(async () => {
          await expectSensitive(save);
        });
        await save.click();

        await waitForWindowCount(app, 3);
        expectElementKind(
          await app.getById('operation_error_dialog'),
          'infoBar'
        );
        expectElementKind(
          await app.getById('global_defaults_dialog'),
          'window'
        );
        expect(
          await app.findById('ui_language_restart_dialog')
        ).toBeUndefined();
        expect(await width.text()).toBe('95');
        await expectInsensitive(save);
        await app.input.pressKey('Escape');
        await waitForWindowCount(app, 2);
        expect(await width.text()).toBe('95');
        await expectSensitive(save);
      }
    );
  });

  it('rebases inherited settings without losing dirty connection overrides or flattening defaults', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[general]\nname=Alpha\n\n[terminal]\nwidth=88\n'
        );
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[terminal]\nwidth=80\nauto_close=false\n'
        );
      },
      async ({ app, configHome, connections }) => {
        const list = await app.getById('connection_list');
        await selectConnectionRow(app, list, 0);
        const name = expectElementKind(
          await app.getById('settings_general_name_entry'),
          'entry'
        );
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        const apply = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        await waitForResult(async () => {
          expect(await name.text()).toBe('Alpha');
          expect(Number(await width.text())).toBe(88);
        });
        await expectSelectedComboValue(
          app,
          'settings_terminal_auto_close_combo',
          'Disabled (global default)'
        );

        await name.setText('Dirty Alpha');
        await width.setText('93');
        await waitForResult(async () => {
          await expectSensitive(apply);
        });

        await openGlobalDefaults(app);
        await selectSettingsTab(app, 'global_settings', 'Terminal');
        const globalWidth = expectElementKind(
          await app.getById('global_settings_terminal_width_entry'),
          'entry'
        );
        const globalAutoClose = expectElementKind(
          await app.getById('global_settings_terminal_auto_close_combo'),
          'comboBox'
        );
        expect(Number(await globalWidth.text())).toBe(80);
        await expectSelectedComboValue(
          app,
          'global_settings_terminal_auto_close_combo',
          'Disabled'
        );
        await globalWidth.setText('120');
        await globalAutoClose.selectChildAt(1);
        const globalSave = expectElementKind(
          await app.getById('global_defaults_save_button'),
          'button'
        );
        await waitForResult(async () => {
          await expectSensitive(globalSave);
        });
        await globalSave.click();
        await waitForWindowCount(app, 1);

        expect(await name.text()).toBe('Dirty Alpha');
        expect(Number(await width.text())).toBe(93);
        await expectSelectedComboValue(
          app,
          'settings_terminal_auto_close_combo',
          'Enabled (global default)'
        );
        await expectSensitive(apply);

        await apply.click();
        const alphaContent = await waitForResult(async () => {
          const content = await readFile(
            join(connections, 'Alpha.ini'),
            'utf8'
          );
          expect(content).toContain('name=Dirty Alpha');
          expect(content).toContain('width=93');
          return content;
        });
        expect(alphaContent).not.toContain('width=120');
        expect(alphaContent).not.toContain('auto_close=');
        const globalContent = await readFile(
          join(configHome, 'elder-terms', 'global.ini'),
          'utf8'
        );
        expect(globalContent).toContain('width=120');
        expect(globalContent).toContain('auto_close=true');

        await expectElementKind(
          await app.getById('new_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await width.text()).toBe('');
        });
        await expectSelectedComboValue(
          app,
          'settings_terminal_auto_close_combo',
          'Enabled (global default)'
        );
        await expectSensitive(apply);
        await apply.click();
        await waitForResult(async () => {
          expect(
            await readFile(join(connections, 'New connection.ini'), 'utf8')
          ).toBe('');
        });
      }
    );
  });

  it('loads, edits, and applies a selected connection', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const list = await app.getById('connection_list');
        await selectConnectionRow(app, list, 0);
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await waitForResult(async () => {
          expect(Number(await width.text())).toBe(88);
        });

        const apply = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        await expectInsensitive(apply);
        await width.setText('91');
        await expectSensitive(apply);
        await apply.click();

        await waitForResult(async () => {
          expect(
            await readFile(join(connections, 'Alpha.ini'), 'utf8')
          ).toContain('width=91');
        });
        await expectInsensitive(apply);
      }
    );
  });

  it('duplicates a saved connection from its context menu', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const list = await app.getById('connection_list');
        await rightClickConnectionRow(app, list, 0);
        const duplicateItem = await waitForResult(async () => {
          const item = expectElementKind(
            await app.getById('duplicate_connection_menu_item'),
            'menuItem'
          );
          expect((await item.info()).states).toContain('showing');
          return item;
        });
        expect((await duplicateItem.info()).name).toBe('Duplicate');

        await duplicateItem.click();
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(3);
          expect(
            await readFile(join(connections, 'Alpha (2).ini'), 'utf8')
          ).toContain('width=88');
        });
        expect(
          await readFile(join(connections, 'Alpha.ini'), 'utf8')
        ).toContain('width=88');
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('settings_terminal_width_entry'),
              'entry'
            ).text()
          ).toBe('88');
        });

        await rightClickConnectionRow(app, list, 0);
        await waitForResult(async () => {
          expect((await duplicateItem.info()).states).toContain('showing');
        });
        await duplicateItem.click();
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(4);
          expect(
            await readFile(join(connections, 'Alpha (3).ini'), 'utf8')
          ).toContain('width=88');
        });
      }
    );
  });

  it('renames and deletes a saved connection from its context menu', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const list = await app.getById('connection_list');
        await rightClickConnectionRow(app, list, 0);
        const renameItem = await waitForResult(async () => {
          const item = expectElementKind(
            await app.getById('rename_connection_menu_item'),
            'menuItem'
          );
          expect((await item.info()).states).toContain('showing');
          return item;
        });
        const deleteItem = expectElementKind(
          await app.getById('delete_connection_menu_item'),
          'menuItem'
        );
        expect((await renameItem.info()).name).toBe('Rename');
        expect((await deleteItem.info()).name).toBe('Delete');

        await renameItem.click();
        await replaceFocusedText(app, 'renamed alpha');
        await app.input.pressKey('Return');
        await waitForResult(async () => {
          expect(
            await readFile(join(connections, 'renamed alpha.ini'), 'utf8')
          ).toContain('width=88');
        });
        await expect(
          readFile(join(connections, 'Alpha.ini'), 'utf8')
        ).rejects.toMatchObject({ code: 'ENOENT' });

        await rightClickConnectionRow(app, list, 1);
        await waitForResult(async () => {
          expect((await deleteItem.info()).states).toContain('showing');
        });
        await deleteItem.click();
        const deleteDialog = expectElementKind(
          await app.getById('delete_connection_dialog'),
          'infoBar'
        );
        expect((await deleteDialog.info()).states).not.toContain('modal');
        await expectInsensitive(await app.getById('main_window'));
        await expectElementKind(
          await app.getById('cancel_delete_connection_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);
        expect(
          await readFile(join(connections, 'renamed alpha.ini'), 'utf8')
        ).toContain('width=88');

        await rightClickConnectionRow(app, list, 1);
        await waitForResult(async () => {
          expect((await deleteItem.info()).states).toContain('showing');
        });
        await deleteItem.click();
        await expectElementKind(
          await app.getById('delete_connection_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(1);
        });
        await expect(
          readFile(join(connections, 'renamed alpha.ini'), 'utf8')
        ).rejects.toMatchObject({ code: 'ENOENT' });
      }
    );
  });

  it('does not open another connection context menu while edits are unsaved', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      const list = await app.getById('connection_list');
      await selectConnectionRow(app, list, 0);
      const width = expectElementKind(
        await app.getById('settings_terminal_width_entry'),
        'entry'
      );
      await width.setText('97');

      await rightClickConnectionRow(app, list, 1);
      await expect(app.getById('rename_connection_menu_item')).rejects.toThrow(
        /not found/iu
      );
      await expect(app.getById('delete_connection_menu_item')).rejects.toThrow(
        /not found/iu
      );
      expect(await app.getWindowCount()).toBe(1);
      expect(await width.text()).toBe('97');
    });
  });

  it('creates a new profile and confirms before discarding edits', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const newButton = expectElementKind(
          await app.getById('new_button'),
          'button'
        );
        await newButton.click();
        const apply = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        await expectSensitive(apply);
        await apply.click();
        await waitForResult(async () => {
          expect(
            await readFile(join(connections, 'New connection.ini'), 'utf8')
          ).toBe('');
        });

        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await width.setText('92');
        await newButton.click();
        const dialog = expectElementKind(
          await app.getById('discard_changes_dialog'),
          'infoBar'
        );
        expect((await dialog.info()).states).not.toContain('modal');
        await expectInsensitive(await app.getById('main_window'));
        await expectElementKind(
          await app.getById('cancel_discard_button'),
          'button'
        ).click();
        expect(Number(await width.text())).toBe(92);

        await newButton.click();
        await expectElementKind(
          await app.getById('discard_changes_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await width.text()).toBe('');
        });
      }
    );
  });

  it('keeps unsaved edits when selection change confirmation is canceled', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      const { list, width } = await beginDirtyConnectionEdit(app, 96);
      await selectConnectionRow(app, list, 1);
      await waitForResult(
        async () => {
          expect(await app.getWindowCount()).toBe(2);
        },
        { message: 'selection change should show discard confirmation' }
      );
      await expectElementKind(
        await app.getById('cancel_discard_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await app.getWindowCount()).toBe(1);
      });
      expect(Number(await width.text())).toBe(96);
    });
  });

  it('changes selection after discarding unsaved edits', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      const { list, width } = await beginDirtyConnectionEdit(app, 96);
      await selectConnectionRow(app, list, 1);
      await waitForResult(
        async () => {
          expect(await app.getWindowCount()).toBe(2);
        },
        { message: 'selection change should show discard confirmation' }
      );
      await expectElementKind(
        await app.getById('discard_changes_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(Number(await width.text())).toBe(99);
      });
    });
  });

  it('keeps unsaved edits when window close confirmation is canceled', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      const { width } = await beginDirtyConnectionEdit(app, 97);
      const window = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      await window.activate();
      await app.input.setModifier('control', true);
      try {
        await app.input.pressKey('w');
      } finally {
        await app.input.setModifier('control', false);
      }
      await waitForResult(
        async () => {
          expect(await app.getWindowCount()).toBe(2);
        },
        { message: 'window close should show discard confirmation' }
      );
      await expectElementKind(
        await app.getById('cancel_discard_button'),
        'button'
      ).click();
      expect(Number(await width.text())).toBe(97);
    });
  });

  it('reloads the selected profile and connection list after external changes', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const list = await app.getById('connection_list');
        await selectConnectionRow(app, list, 0);
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        const apply = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        await waitForResult(async () => {
          expect(Number(await width.text())).toBe(88);
        });

        await expectInsensitive(apply);
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[terminal]\nwidth=95\n'
        );
        await waitForResult(async () => {
          expect(Number(await width.text())).toBe(95);
          await expectInsensitive(apply);
        });

        await writeFile(
          join(connections, 'Gamma.ini'),
          '[terminal]\nwidth=101\n'
        );
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(3);
        });
        await rm(join(connections, 'Beta.ini'));
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(2);
        });
        await rename(
          join(connections, 'Alpha.ini'),
          join(connections, 'Renamed.ini')
        );
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(2);
          expect(Number(await width.text())).toBe(95);
        });
      }
    );
  });

  for (const mode of [
    'clean',
    'new',
    'new-invalid',
    'new-save-failure',
    'save',
    'save-conflict',
    'discard',
    'cancel',
    'save-failure',
    'launch-failure',
    'no-editor',
    'missing-file',
  ]) {
    it(`opens a connection in the XDG text editor: ${mode}`, async (context) => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-editor-'));
      const applications = join(directory, 'applications');
      const executable = join(directory, 'editor.mjs');
      const capturePath = join(directory, 'capture.json');
      const isNew = mode.startsWith('new');
      const profileName = isNew
        ? 'New connection.ini'
        : "Alpha 日本語 ; $' (test).ini";
      await mkdir(applications);
      // Keep MIME recognition available without exposing host applications.
      await symlink('/usr/share/mime', join(directory, 'mime'));
      await writeFile(
        executable,
        `#!${process.execPath}
import { readFile, writeFile } from 'node:fs/promises';
const args = process.argv.slice(2);
const content = await readFile(args[0], 'utf8');
await writeFile(${JSON.stringify(capturePath)}, JSON.stringify({ args, content }));
${mode === 'clean' ? "await writeFile(args[0], '[terminal]\\nwidth=95\\n');" : ''}
`
      );
      await chmod(executable, 0o755);
      if (mode !== 'no-editor') {
        await writeFile(
          join(applications, 'fixture-editor.desktop'),
          '[Desktop Entry]\nType=Application\nName=Fixture text editor\n' +
            `Exec="${executable}" %f\nMimeType=text/plain;\nTerminal=false\n` +
            // Fail the launch request itself, before gio-launch-desktop takes
            // over. Errors inside the started editor are not returned by GIO.
            (mode === 'launch-failure'
              ? `Path=${join(directory, 'missing-working-directory')}\n`
              : '')
        );
      }
      try {
        await runLauncherGtkTest(
          context,
          async (connections) => {
            if (!isNew && mode === 'missing-file') {
              // Removing the target outside the monitored directory makes
              // the file disappear specifically between selection and launch.
              const target = join(directory, 'profile.ini');
              await writeFile(target, '[terminal]\nwidth=88\n');
              await symlink(target, join(connections, profileName));
            } else if (!isNew) {
              await writeFile(
                join(connections, profileName),
                '[terminal]\nwidth=88\n'
              );
            }
            await writeFile(
              join(connections, '..', '..', 'mimeapps.list'),
              '[Default Applications]\ntext/plain=fixture-editor.desktop;\n'
            );
          },
          async ({ app, connections }) => {
            const list = await app.getById('connection_list');
            if (isNew) {
              await expectElementKind(
                await app.getById('new_button'),
                'button'
              ).click();
              await app.input.pressKey('Escape');
            } else {
              await selectConnectionRow(app, list, 0);
            }
            const width = expectElementKind(
              await app.getById('settings_terminal_width_entry'),
              'entry'
            );
            if (isNew) {
              await selectSettingsTab(app, 'settings', 'Terminal');
              await width.setText(mode === 'new-invalid' ? '0' : '91');
            } else {
              await waitForResult(async () =>
                expect(await width.text()).toBe('88')
              );
            }
            const dirty = [
              'save',
              'save-conflict',
              'discard',
              'cancel',
              'save-failure',
            ].includes(mode);
            if (dirty) {
              await width.setText('91');
            }
            if (mode === 'save-conflict') {
              await writeFile(
                join(connections, profileName),
                '[terminal]\nwidth=95\n'
              );
              await expectElementKind(
                await app.getById('external_keep_button'),
                'button'
              ).click();
              expect(await width.text()).toBe('91');
            }
            if (mode === 'save-failure' || mode === 'new-save-failure') {
              await chmod(connections, 0o500);
            }
            if (mode === 'missing-file') {
              await rm(join(directory, 'profile.ini'));
            }
            try {
              await rightClickConnectionRow(app, list, 0);
              expect(
                (await (await app.getById('edit_connection_menu_item')).info())
                  .name
              ).toBe(
                isNew ? 'Save and open in text editor' : 'Edit in text editor'
              );
              await expectElementKind(
                await app.getById('edit_connection_menu_item'),
                'menuItem'
              ).click();
              if (dirty) {
                const response =
                  mode === 'discard'
                    ? 'discard'
                    : mode === 'cancel'
                      ? 'cancel'
                      : 'save';
                await expectElementKind(
                  await app.getById(`editor_${response}_open_button`),
                  'button'
                ).click();
                if (mode === 'save-conflict') {
                  const overwrite = expectElementKind(
                    await app.getById('external_overwrite_button'),
                    'button'
                  );
                  expect(
                    await readFile(join(connections, profileName), 'utf8')
                  ).toContain('width=95');
                  await expect(
                    readFile(capturePath, 'utf8')
                  ).rejects.toMatchObject({ code: 'ENOENT' });
                  await overwrite.click();
                }
              }
              if (mode === 'cancel') {
                await waitForWindowCount(app, 1);
                expect(await width.text()).toBe('91');
                await expect(
                  readFile(capturePath, 'utf8')
                ).rejects.toMatchObject({ code: 'ENOENT' });
                expect(
                  await readFile(join(connections, profileName), 'utf8')
                ).toContain('width=88');
              } else if (
                [
                  'save-failure',
                  'new-invalid',
                  'new-save-failure',
                  'launch-failure',
                  'no-editor',
                  'missing-file',
                ].includes(mode)
              ) {
                const error = expectElementKind(
                  await app.getById('operation_error_dialog'),
                  'infoBar'
                );
                expect(await width.text()).toBe(
                  mode === 'new-invalid' ? '0' : isNew || dirty ? '91' : '88'
                );
                if (mode === 'new-invalid') {
                  const labels: string[] = [];
                  const pending: GtkWidgetElement[] = [error];
                  while (pending.length > 0) {
                    const widget = pending.pop()!;
                    if (widget.kind === 'label')
                      labels.push((await widget.info()).name ?? '');
                    if ('getChildCount' in widget && 'childAt' in widget) {
                      for (
                        let index = 0;
                        index < (await widget.getChildCount());
                        index++
                      ) {
                        const child = await widget.childAt(index);
                        if (child !== undefined) pending.push(child);
                      }
                    }
                  }
                  expect(labels.join('\n')).toMatch(/invalid input/i);
                }
                await expect(
                  readFile(capturePath, 'utf8')
                ).rejects.toMatchObject({ code: 'ENOENT' });
                if (isNew) {
                  await expect(
                    readFile(join(connections, profileName), 'utf8')
                  ).rejects.toMatchObject({ code: 'ENOENT' });
                  await app.input.pressKey('Escape');
                  await waitForWindowCount(app, 1);
                  expect(await connectionRowCount(list)).toBe(1);
                  await rightClickConnectionRow(app, list, 0);
                  expect(
                    (
                      await (
                        await app.getById('save_new_connection_menu_item')
                      ).info()
                    ).states
                  ).toContain('showing');
                } else if (mode !== 'missing-file') {
                  expect(
                    await readFile(join(connections, profileName), 'utf8')
                  ).toContain('width=88');
                }
              } else {
                await waitForResult(async () => {
                  const capture = JSON.parse(
                    await readFile(capturePath, 'utf8')
                  );
                  expect(capture.args).toEqual([
                    join(connections, profileName),
                  ]);
                  expect(capture.content).toContain(
                    isNew || mode === 'save' || mode === 'save-conflict'
                      ? 'width=91'
                      : 'width=88'
                  );
                });
                await waitForResult(async () => {
                  expect(await width.text()).toBe(
                    mode === 'clean'
                      ? '95'
                      : isNew || mode === 'save' || mode === 'save-conflict'
                        ? '91'
                        : '88'
                  );
                  await expectInsensitive(await app.getById('apply_button'));
                });
              }
            } finally {
              await chmod(connections, 0o700);
            }
          },
          {
            args: [],
            env: {
              XDG_DATA_HOME: directory,
              XDG_DATA_DIRS: directory,
              XDG_CONFIG_DIRS: directory,
            },
          }
        );
      } finally {
        await rm(directory, { recursive: true, force: true });
      }
    });
  }

  for (const saving of ['normal', 'replacement']) {
    for (const decision of ['reload', 'keep']) {
      it(`protects dirty edits from ${saving} saves and lets the user ${decision}`, async (context) => {
        await runLauncherGtkTest(
          context,
          prepareProfiles,
          async ({ app, connections }) => {
            const { width } = await beginDirtyConnectionEdit(app, 91);
            const path = join(connections, 'Alpha.ini');
            if (saving === 'replacement') {
              const replacement = join(connections, 'replacement.tmp');
              await writeFile(replacement, '[terminal]\nwidth=95\n');
              await rename(replacement, path);
            } else {
              await writeFile(path, '[terminal]\nwidth=95\n');
            }
            await app.getById('external_change_dialog');
            expect(await width.text()).toBe('91');
            await expectElementKind(
              await app.getById(`external_${decision}_button`),
              'button'
            ).click();
            if (decision === 'reload') {
              await waitForResult(async () => {
                expect(await width.text()).toBe('95');
                await expectInsensitive(await app.getById('apply_button'));
              });
            } else {
              expect(await width.text()).toBe('91');
              const apply = expectElementKind(
                await app.getById('apply_button'),
                'button'
              );
              await apply.click();
              await expectElementKind(
                await app.getById('external_overwrite_cancel_button'),
                'button'
              ).click();
              expect(await readFile(path, 'utf8')).toContain('width=95');
              expect(await width.text()).toBe('91');
              await apply.click();
              await expectElementKind(
                await app.getById('external_overwrite_button'),
                'button'
              ).click();
              await waitForResult(async () => {
                expect(await readFile(path, 'utf8')).toContain('width=91');
                await expectInsensitive(apply);
              });
              await waitForWindowCount(app, 1);
            }
          }
        );
      });
    }
  }

  it('retains the last good editor state after a malformed external save', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const list = await app.getById('connection_list');
        await selectConnectionRow(app, list, 0);
        await selectSettingsTab(app, 'settings', 'Terminal');
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await waitForResult(async () => expect(await width.text()).toBe('88'));
        expect((await width.info()).states).toContain('showing');
        await writeFile(join(connections, 'Alpha.ini'), '[broken');
        await app.getById('operation_error_dialog');
        expect((await width.info()).states).toContain('showing');
        expect(await width.text()).toBe('88');
        await app.input.pressKey('Escape');
        await waitForWindowCount(app, 1);
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[terminal]\nwidth=95\n'
        );
        await waitForResult(async () => expect(await width.text()).toBe('95'));
      }
    );
  });

  it('preserves dirty edits when the selected file is removed externally', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const { width } = await beginDirtyConnectionEdit(app, 91);
        await selectSettingsTab(app, 'settings', 'Terminal');
        await waitForResult(async () =>
          expect((await width.info()).states).toContain('showing')
        );
        const path = join(connections, 'Alpha.ini');
        await rm(path);
        await expectElementKind(
          await app.getById('external_keep_button'),
          'button'
        ).click();
        expect((await width.info()).states).toContain('showing');
        expect(await width.text()).toBe('91');
        await expectElementKind(
          await app.getById('apply_button'),
          'button'
        ).click();
        await expectElementKind(
          await app.getById('external_overwrite_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await readFile(path, 'utf8')).toContain('width=91');
          await expectInsensitive(await app.getById('apply_button'));
        });
      }
    );
  });

  it('opens General when displaying settings for existing and new connections', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      const list = await app.getById('connection_list');
      await selectConnectionRow(app, list, 0);
      await selectSettingsTab(app, 'settings', 'Terminal');
      await selectConnectionRow(app, list, 1);
      const width = await app.getById('settings_terminal_width_entry');
      await waitForResult(async () => {
        expect(
          (await (await app.getById('settings_general_name_entry')).info())
            .states
        ).toContain('showing');
        expect((await width.info()).states).not.toContain('showing');
      });
      await selectSettingsTab(app, 'settings', 'Terminal');
      await expectElementKind(
        await app.getById('new_button'),
        'button'
      ).click();
      await app.input.pressKey('Escape');
      await waitForResult(async () => {
        expect(
          (await (await app.getById('settings_general_name_entry')).info())
            .states
        ).toContain('showing');
        expect((await width.info()).states).not.toContain('showing');
      });
      await openGlobalDefaults(app);
      expect(await selectedSettingsTabName(app, 'global_settings')).toBe(
        'General'
      );
      await selectSettingsTab(app, 'global_settings', 'Terminal');
      await expectElementKind(
        await app.getById('global_defaults_cancel_button'),
        'button'
      ).click();
      await waitForWindowCount(app, 1);
      await openGlobalDefaults(app);
      expect(await selectedSettingsTabName(app, 'global_settings')).toBe(
        'General'
      );
    });
  });

  for (const japanese of [false, true]) {
    it(
      'offers localized actions for a new unsaved entry: Japanese ' + japanese,
      async (context) => {
        await runLauncherGtkTest(
          context,
          async () => {},
          async ({ app }) => {
            await expectElementKind(
              await app.getById('new_button'),
              'button'
            ).click();
            await app.input.pressKey('Escape');
            await rightClickConnectionRow(
              app,
              await app.getById('connection_list'),
              0
            );
            for (const [id, name] of [
              ['save_new_connection_menu_item', japanese ? '保存' : 'Save'],
              [
                'edit_connection_menu_item',
                japanese
                  ? '保存してテキストエディタで開く'
                  : 'Save and open in text editor',
              ],
              [
                'rename_connection_menu_item',
                japanese ? '名前の変更' : 'Rename',
              ],
              [
                'cancel_new_connection_menu_item',
                japanese ? '新規作成を取り消す' : 'Cancel new connection',
              ],
            ]) {
              const item = await app.getById(id);
              await waitForResult(async () =>
                expect((await item.info()).states).toContain('showing')
              );
              expect((await item.info()).name).toBe(name);
              await expectSensitive(item);
            }
            for (const id of [
              'duplicate_connection_menu_item',
              'delete_connection_menu_item',
            ]) {
              const item = await app.findById(id);
              if (item !== undefined)
                expect((await item.info()).states).not.toContain('showing');
            }
            const directory = fileURLToPath(
              new URL('../../test-results/launcher/new-entry/', import.meta.url)
            );
            await mkdir(directory, { recursive: true });
            await writeFile(
              join(directory, japanese ? 'menu-ja.png' : 'menu-en.png'),
              (await (await app.getById('connection_context_menu')).capture())
                .image
            );
          },
          { args: [], env: japanese ? japaneseTestEnvironment : {} }
        );
      }
    );
  }

  it('renames a draft without creating a file and saves it from the context menu', async (context) => {
    await runLauncherGtkTest(
      context,
      async () => {},
      async ({ app, connections }) => {
        await expectElementKind(
          await app.getById('new_button'),
          'button'
        ).click();
        await app.input.pressKey('Escape');
        const list = await app.getById('connection_list');
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await selectSettingsTab(app, 'settings', 'Terminal');
        await width.setText('91');
        await rightClickConnectionRow(app, list, 0);
        await expectElementKind(
          await app.getById('rename_connection_menu_item'),
          'menuItem'
        ).click();
        await replaceFocusedText(app, 'draft renamed');
        await app.input.pressKey('Return');
        for (const name of ['New connection.ini', 'draft renamed.ini']) {
          await expect(
            readFile(join(connections, name), 'utf8')
          ).rejects.toMatchObject({ code: 'ENOENT' });
        }
        await rightClickConnectionRow(app, list, 0);
        await expectElementKind(
          await app.getById('save_new_connection_menu_item'),
          'menuItem'
        ).click();
        await waitForResult(async () => {
          expect(
            await readFile(join(connections, 'draft renamed.ini'), 'utf8')
          ).toContain('width=91');
          await expectInsensitive(await app.getById('apply_button'));
        });
        expect(await connectionRowCount(list)).toBe(1);
        expect(await selectedSettingsTabName(app, 'settings')).toBe('Terminal');
        await rightClickConnectionRow(app, list, 0);
        expect(
          (await (await app.getById('edit_connection_menu_item')).info()).name
        ).toBe('Edit in text editor');
        for (const id of [
          'duplicate_connection_menu_item',
          'delete_connection_menu_item',
        ]) {
          expect((await (await app.getById(id)).info()).states).toContain(
            'showing'
          );
        }
        for (const id of [
          'save_new_connection_menu_item',
          'cancel_new_connection_menu_item',
        ]) {
          const item = await app.findById(id);
          if (item !== undefined)
            expect((await item.info()).states).not.toContain('showing');
        }
      }
    );
  });

  it('confirms cancelling a new connection and removes only the discarded draft', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        await expectElementKind(
          await app.getById('new_button'),
          'button'
        ).click();
        await replaceFocusedText(app, 'unsaved draft');
        await app.input.pressKey('Return');
        const list = await app.getById('connection_list');
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await selectSettingsTab(app, 'settings', 'Terminal');
        await width.setText('91');
        await rightClickConnectionRow(app, list, 2);
        await expectElementKind(
          await app.getById('cancel_new_connection_menu_item'),
          'menuItem'
        ).click();
        await expectElementKind(
          await app.getById('cancel_discard_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);
        expect(await connectionRowCount(list)).toBe(3);
        expect(await width.text()).toBe('91');
        if (list.kind === 'table')
          expect((await (await list.cellAt(2, 0))?.info())?.name).toBe(
            'unsaved draft'
          );
        await rightClickConnectionRow(app, list, 2);
        await expectElementKind(
          await app.getById('cancel_new_connection_menu_item'),
          'menuItem'
        ).click();
        await expectElementKind(
          await app.getById('discard_changes_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await connectionRowCount(list)).toBe(2);
          expect(
            (await (await app.getById('empty_details_label')).info()).states
          ).toContain('showing');
        });
        expect(await readFile(join(connections, 'Alpha.ini'), 'utf8')).toBe(
          '[terminal]\nwidth=88\n'
        );
        expect(await readFile(join(connections, 'Beta.ini'), 'utf8')).toBe(
          '[terminal]\nwidth=99\n'
        );
        await expect(
          readFile(join(connections, 'unsaved draft.ini'), 'utf8')
        ).rejects.toMatchObject({ code: 'ENOENT' });
      }
    );
  });

  it('explains an invalid draft name without saving or losing the draft', async (context) => {
    await runLauncherGtkTest(
      context,
      async () => {},
      async ({ app, connections }) => {
        await expectElementKind(
          await app.getById('new_button'),
          'button'
        ).click();
        await replaceFocusedText(app, '');
        await app.input.pressKey('BackSpace');
        await app.input.pressKey('Return');
        const list = await app.getById('connection_list');
        await rightClickConnectionRow(app, list, 0);
        await expectElementKind(
          await app.getById('save_new_connection_menu_item'),
          'menuItem'
        ).click();
        const error = await app.getById('operation_error_dialog');
        expect((await error.info()).states).toContain('showing');
        const labels: string[] = [];
        const pending = [error];
        while (pending.length > 0) {
          const widget = pending.pop()!;
          if (widget.kind === 'label')
            labels.push((await widget.info()).name ?? '');
          if ('getChildCount' in widget && 'childAt' in widget) {
            for (
              let index = 0;
              index < (await widget.getChildCount());
              index++
            ) {
              const child = await widget.childAt(index);
              if (child !== undefined) pending.push(child);
            }
          }
        }
        expect(labels.join('\n')).toMatch(/name/i);
        await app.input.pressKey('Escape');
        await waitForWindowCount(app, 1);
        expect(await connectionRowCount(list)).toBe(1);
        await expect(
          readFile(join(connections, 'New connection.ini'), 'utf8')
        ).rejects.toMatchObject({ code: 'ENOENT' });
        await rightClickConnectionRow(app, list, 0);
        expect(
          (await (await app.getById('save_new_connection_menu_item')).info())
            .states
        ).toContain('showing');
      }
    );
  });

  it('connects a saved profile by double-clicking its row', async (context) => {
    const fakeVte = await createFakeVte();
    try {
      await runLauncherGtkTest(
        context,
        prepareProfiles,
        async ({ app, connections }) => {
          const list = await app.getById('connection_list');
          await doubleClickConnectionRow(app, list, 0);
          await waitForResult(async () => {
            const capture = await readLaunchCapture(fakeVte.capture);
            expect(capture.args).toEqual([
              '-c',
              join(connections, 'Alpha.ini'),
            ]);
            expect(capture.startupContent).toBeNull();
          });
        },
        {
          args: [],
          env: {
            ELDER_TERMS_TEST_CAPTURE: fakeVte.capture,
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await fakeVte.release();
    }
  });

  it('routes an SFTP profile to the file transfer application', async (context) => {
    const fakeVte = await createFakeVte();
    const fakeFileTransfer = await createFakeVte(true);
    try {
      await runLauncherGtkTest(
        context,
        async (connections) => {
          await writeFile(
            join(connections, 'Files.ini'),
            [
              '[general]',
              'type=sftp',
              '',
              '[ssh]',
              'address=files.example',
              '',
            ].join('\n')
          );
        },
        async ({ app, connections }) => {
          const list = await app.getById('connection_list');
          await doubleClickConnectionRow(app, list, 0);
          await waitForResult(async () => {
            const capture = await readLaunchCapture(fakeFileTransfer.capture);
            expect(capture.args).toEqual([
              '-c',
              join(connections, 'Files.ini'),
            ]);
            expect(capture.startupContent).toBeNull();
          });
          await expect(readFile(fakeVte.capture, 'utf8')).rejects.toThrow();
        },
        {
          args: [],
          env: {
            ELDER_TERMS_FILE_TRANSFER_PATH: fakeFileTransfer.executable,
            ELDER_TERMS_TEST_CAPTURE: fakeFileTransfer.capture,
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await Promise.all([fakeFileTransfer.release(), fakeVte.release()]);
    }
  });

  for (const mode of ['explicit', 'implicit'] as const)
    for (const legacyPrompt of [false, true]) {
      it(`saves FTPS settings and transfers from the launcher: ${mode}-${legacyPrompt ? 'legacy-prompt' : 'verified'}`, async (context) => {
        const directory = await mkdtemp(
          join(tmpdir(), 'elder-terms-launcher-ftps-')
        );
        const local = join(directory, 'local');
        const root = join(directory, 'remote');
        const remote = join(root, 'home');
        const certificate = join(directory, 'certificate.pem');
        const key = join(directory, 'key.pem');
        let server: ReturnType<typeof spawn> | undefined;
        let serverFinished: Promise<number | null> | undefined;
        const events: string[] = [];
        let serverLog = '';
        let phase = 'create fixture';
        try {
          await mkdir(local);
          await mkdir(remote, { recursive: true });
          await writeFile(
            join(local, 'from-launcher.txt'),
            'FTPS launcher upload\n'
          );
          await writeFile(
            join(remote, 'from-server.txt'),
            'FTPS server contents\n'
          );
          const generated = spawnSync(
            'openssl',
            [
              'req',
              '-x509',
              '-newkey',
              'rsa:2048',
              '-noenc',
              '-keyout',
              key,
              '-out',
              certificate,
              '-days',
              '1',
              '-subj',
              '/CN=localhost',
              '-addext',
              'subjectAltName=IP:127.0.0.1',
            ],
            { encoding: 'utf8' }
          );
          expect(generated.status, generated.stderr).toBe(0);
          server = spawn(
            fileURLToPath(
              new URL(
                '../../.build/elder-terms-vte/ftp-test-server',
                import.meta.url
              )
            ),
            [
              root,
              '--trace',
              `--tls=${mode}`,
              `--cert=${certificate}`,
              `--key=${key}`,
              ...(legacyPrompt ? ['--tls-version=769', '--legacy-tls'] : []),
            ],
            { stdio: ['pipe', 'pipe', 'pipe'] }
          );
          createInterface({ input: server.stdout! }).on('line', (line) =>
            events.push(line)
          );
          server.stderr!.on('data', (bytes: Buffer) => {
            serverLog += bytes.toString();
          });
          let serverError: Error | undefined;
          serverFinished = new Promise((resolve) => {
            server!.once('error', (error) => {
              serverError = error;
              resolve(null);
            });
            server!.once('close', resolve);
          });
          const ready = await waitForResult(async () => {
            if (serverError) throw serverError;
            const line = events.find((value) => value.startsWith('READY '));
            expect(line, serverLog).toBeDefined();
            return line!;
          });
          await runLauncherGtkTest(
            context,
            async (connections) => {
              await writeFile(
                join(connections, 'Other.ini'),
                '[general]\ntype=ftp\n'
              );
            },
            async ({ app, connections }) => {
              phase = 'configure profile';
              const list = await app.getById('connection_list');
              await expectElementKind(
                await app.getById('new_button'),
                'button'
              ).click();
              await app.input.pressKey('Escape');
              await selectSettingsTab(app, 'settings', 'General');
              const type = expectElementKind(
                await app.getById('settings_general_type_combo'),
                'comboBox'
              );
              await type.selectChildAt(6);
              await expectSelectedComboValue(
                app,
                'settings_general_type_combo',
                'FTP'
              );
              await selectSettingsTab(app, 'settings', 'FTP');
              for (const [name, value] of [
                ['address', '127.0.0.1'],
                ['port', ready.slice(6)],
                ['username', 'alice'],
                ['local_directory', local],
                ['remote_directory', '/home'],
              ])
                await expectElementKind(
                  await app.getById('settings_ftp_' + name + '_entry'),
                  'entry'
                ).setText(value);
              await expectElementKind(
                await app.getById('settings_ftp_tls_mode_combo'),
                'comboBox'
              ).selectChildAt(mode === 'explicit' ? 2 : 3);
              await expectElementKind(
                await app.getById('settings_ftp_ca_file_mode_combo'),
                'comboBox'
              ).selectChildAt(legacyPrompt ? 1 : 2);
              if (!legacyPrompt)
                await expectElementKind(
                  await app.getById('settings_ftp_ca_file_entry'),
                  'entry'
                ).setText(certificate);
              await expectElementKind(
                await app.getById('settings_ftp_tls_min_version_combo'),
                'comboBox'
              ).selectChildAt(legacyPrompt ? 1 : 3);
              await expectElementKind(
                await app.getById('settings_ftp_tls_max_version_combo'),
                'comboBox'
              ).selectChildAt(legacyPrompt ? 2 : 4);
              if (legacyPrompt) {
                await expectElementKind(
                  await app.getById('settings_ftp_tls_compatibility_combo'),
                  'comboBox'
                ).selectChildAt(2);
                await expectElementKind(
                  await app.getById(
                    'settings_ftp_certificate_error_action_combo'
                  ),
                  'comboBox'
                ).selectChildAt(2);
              }
              phase = 'save profile';
              await expectSensitive(await app.getById('apply_button'));
              await expectElementKind(
                await app.getById('apply_button'),
                'button'
              ).click();
              await waitForResult(async () => {
                const saved = await readFile(
                  join(connections, 'New connection.ini'),
                  'utf8'
                );
                for (const value of [
                  `tls_mode=${mode}`,
                  `ca_file=${legacyPrompt ? '' : certificate}`,
                  `tls_min_version=${legacyPrompt ? '1.0' : '1.2'}`,
                  `tls_max_version=${legacyPrompt ? '1.0' : '1.2'}`,
                ])
                  expect(saved).toContain(value);
              });
              phase = 'reselect saved profile';
              await selectConnectionRow(app, list, 1);
              await waitForResult(async () => {
                expect(
                  await expectElementKind(
                    await app.getById('settings_ftp_username_entry'),
                    'entry'
                  ).text()
                ).toBe('');
              });
              await selectConnectionRow(app, list, 0);
              await waitForResult(async () => {
                expect(
                  await expectElementKind(
                    await app.getById('settings_ftp_username_entry'),
                    'entry'
                  ).text()
                ).toBe('alice');
              });
              await selectSettingsTab(app, 'settings', 'FTP');
              expect(
                await expectElementKind(
                  await app.getById('settings_ftp_ca_file_entry'),
                  'entry'
                ).text()
              ).toBe(legacyPrompt ? '' : certificate);
              phase = 'open transfer window';
              await expectElementKind(
                await app.getById('connect_button'),
                'button'
              ).click();
              await expectElementKind(
                await app.getById('file_transfer_prompt_entry'),
                'entry'
              ).setText('alice');
              await expectElementKind(
                await app.getById('file_transfer_prompt_secondary_entry'),
                'entry'
              ).setText('secret');
              await expectElementKind(
                await app.getById('file_transfer_prompt_accept_button'),
                'button'
              ).click();
              if (legacyPrompt) {
                await waitForResult(async () =>
                  expect(
                    await expectElementKind(
                      await app.getById('file_transfer_prompt_title_label'),
                      'label'
                    ).text()
                  ).toBe('FTPS certificate validation failed')
                );
                expect(serverLog).not.toContain('COMMAND USER');
                await expectElementKind(
                  await app.getById('file_transfer_prompt_accept_button'),
                  'button'
                ).click();
              }
              await waitForResult(async () => {
                expect(
                  await expectElementKind(
                    await app.getById('file_transfer_status_label'),
                    'label'
                  ).text()
                ).toBe(
                  'Ready' +
                    (legacyPrompt ? ' — Certificate exception active' : '')
                );
                expect(
                  await expectElementKind(
                    await app.getById('file_transfer_local_path_entry'),
                    'entry'
                  ).text()
                ).toBe(local);
                expect(
                  await expectElementKind(
                    await app.getById('file_transfer_remote_path_entry'),
                    'entry'
                  ).text()
                ).toBe('/home');
              });
              phase = 'read transfer listing';
              const localTree = expectElementKind(
                await app.getById('file_transfer_local_tree'),
                'table'
              );
              const remoteTree = expectElementKind(
                await app.getById('file_transfer_remote_tree'),
                'table'
              );
              await waitForResult(async () => {
                expect((await remoteTree.info()).states).toContain('sensitive');
                expect(await remoteTree.getRowCount()).toBeGreaterThan(0);
              });
              const row = await waitForResult(async () => {
                for (
                  let index = 0;
                  index < (await localTree.getRowCount());
                  index++
                )
                  if (
                    (await (await localTree.cellAt(index, 0))?.info())?.name ===
                    'from-launcher.txt'
                  )
                    return index;
                throw Error('Local file missing');
              });
              await localTree.selectRow(row);
              const bounds = await waitForResult(async () => {
                const cell = await localTree.cellAt(row, 0);
                expect((await cell?.info())?.name).toBe('from-launcher.txt');
                return (await cell!.capture()).bounds;
              });
              await app.input.moveMouseTo(
                Math.round(bounds.x + bounds.width / 2),
                Math.round(bounds.y + bounds.height / 2)
              );
              phase = 'open local file menu';
              await app.input.setMouseButton('right', true);
              await app.input.setMouseButton('right', false);
              await expectElementKind(
                await app.getById('file_transfer_send_item'),
                'menuItem'
              ).click();
              phase = 'upload local file';
              try {
                await waitForResult(async () =>
                  expect(
                    await readFile(join(remote, 'from-launcher.txt'), 'utf8')
                  ).toBe('FTPS launcher upload\n')
                );
              } catch (error) {
                const evidence = fileURLToPath(
                  new URL('../../test-results/launcher/', import.meta.url)
                );
                await mkdir(evidence, { recursive: true });
                await writeFile(
                  join(
                    evidence,
                    `launcher-ftps-${mode}-${legacyPrompt}-failed.png`
                  ),
                  (await app.capture()).image
                );
                throw error;
              }
              await waitForResult(async () =>
                expect(
                  await expectElementKind(
                    await app.getById('file_transfer_status_label'),
                    'label'
                  ).text()
                ).toBe(
                  'Sent 1 item' +
                    (legacyPrompt ? ' — Certificate exception active' : '')
                )
              );
              const negotiated = serverLog
                .split('\n')
                .filter(
                  (line) =>
                    line.startsWith('TLS CONTROL') ||
                    line.startsWith('TLS DATA')
                );
              expect(negotiated.length).toBeGreaterThan(1);
              expect(
                negotiated.every(
                  (line) =>
                    line.split(' ')[2] === (legacyPrompt ? 'TLSv1' : 'TLSv1.2')
                )
              ).toBe(true);
              const evidence = fileURLToPath(
                new URL('../../test-results/launcher/', import.meta.url)
              );
              await mkdir(evidence, { recursive: true });
              await writeFile(
                join(
                  evidence,
                  `launcher-ftps-${mode}-${legacyPrompt ? 'legacy-prompt' : 'verified'}.png`
                ),
                (await app.capture()).image
              );
              phase = 'close transfer window';
              // Close the transfer window through its own header, leaving the launcher available.
              const pending: GtkWidgetElement[] = [
                await app.getById('file_transfer_header_bar'),
              ];
              let closed = false;
              while (pending.length) {
                const widget = pending.shift()!;
                if (
                  widget.kind === 'button' &&
                  (await widget.info()).name === 'Close'
                ) {
                  await widget.click();
                  closed = true;
                  break;
                }
                if ('getChildCount' in widget)
                  for (
                    let index = 0;
                    index < (await widget.getChildCount());
                    index++
                  ) {
                    const child = await widget.childAt(index);
                    if (child) pending.push(child);
                  }
              }
              expect(closed).toBe(true);
              await waitForResult(async () =>
                expect(await app.getWindowCount()).toBe(1)
              );
            },
            {
              args: [],
              env: {
                ELDER_TERMS_FILE_TRANSFER_PATH:
                  process.env.ELDER_TERMS_TEST_FTP_APP ??
                  fileURLToPath(
                    new URL(
                      '../../.build/elder-terms-vte/elder-terms-file-transfer',
                      import.meta.url
                    )
                  ),
              },
            }
          );
        } catch (error) {
          throw new Error(
            'FTPS launcher failed during ' +
              phase +
              ': ' +
              String(error) +
              '\nFTP fixture trace:\n' +
              serverLog
          );
        } finally {
          server?.kill('SIGTERM');
          if (serverFinished) await serverFinished;
          await rm(directory, { recursive: true, force: true });
        }
      }, 120_000);
    }

  it('routes an FTP profile to the file transfer application', async (context) => {
    const fakeVte = await createFakeVte();
    const fakeFileTransfer = await createFakeVte(true);
    try {
      await runLauncherGtkTest(
        context,
        async (connections) => {
          await writeFile(
            join(connections, 'Legacy files.ini'),
            [
              '[general]',
              'type=ftp',
              '',
              '[ftp]',
              'address=ftp.example',
              '',
            ].join('\n')
          );
        },
        async ({ app, connections }) => {
          const list = await app.getById('connection_list');
          await doubleClickConnectionRow(app, list, 0);
          await waitForResult(async () => {
            const capture = await readLaunchCapture(fakeFileTransfer.capture);
            expect(capture.args).toEqual([
              '-c',
              join(connections, 'Legacy files.ini'),
            ]);
            expect(capture.startupContent).toBeNull();
          });
          await expect(readFile(fakeVte.capture, 'utf8')).rejects.toThrow();
        },
        {
          args: [],
          env: {
            ELDER_TERMS_FILE_TRANSFER_PATH: fakeFileTransfer.executable,
            ELDER_TERMS_TEST_CAPTURE: fakeFileTransfer.capture,
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await Promise.all([fakeFileTransfer.release(), fakeVte.release()]);
    }
  });

  it('finds the sibling VTE when launched through a dot path', async (context) => {
    const layout = await createSiblingVteLayout();
    try {
      await runLauncherGtkTest(
        context,
        prepareProfiles,
        async ({ app, connections }) => {
          const list = await app.getById('connection_list');
          await selectConnectionRow(app, list, 0);
          await expectElementKind(
            await app.getById('connect_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            const capture = await readLaunchCapture(layout.capture);
            expect(capture.args).toEqual([
              '-c',
              join(connections, 'Alpha.ini'),
            ]);
          });
        },
        {
          appPath: layout.launcher,
          args: [],
          env: {
            ELDER_TERMS_TEST_CAPTURE: layout.capture,
          },
        }
      );
    } finally {
      await layout.release();
    }
  });

  it('connects with dirty settings without saving the profile', async (context) => {
    const fakeVte = await createFakeVte();
    try {
      await runLauncherGtkTest(
        context,
        prepareProfiles,
        async ({ app, connections }) => {
          const list = await app.getById('connection_list');
          await selectConnectionRow(app, list, 0);
          const width = expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          );
          await waitForResult(async () => {
            expect(Number(await width.text())).toBe(88);
          });
          await width.setText('93');
          await expectElementKind(
            await app.getById('connect_button'),
            'button'
          ).click();

          await waitForResult(async () => {
            const capture = await readLaunchCapture(fakeVte.capture);
            expect(capture.args[0]).toBe('-c');
            expect(capture.args[1]).toBe(join(connections, 'Alpha.ini'));
            expect(capture.args[2]).toBe('-s');
            expect(capture.args[3]).toMatch(/elder-terms-startup-/u);
            expect(capture.startupContent).toContain('width=93');
          });
          expect(
            await readFile(join(connections, 'Alpha.ini'), 'utf8')
          ).toContain('width=88');
        },
        {
          args: [],
          env: {
            ELDER_TERMS_TEST_CAPTURE: fakeVte.capture,
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await fakeVte.release();
    }
  });

  it('connects a new draft using only a temporary startup profile', async (context) => {
    const fakeVte = await createFakeVte();
    try {
      await runLauncherGtkTest(
        context,
        prepareProfiles,
        async ({ app }) => {
          await expectElementKind(
            await app.getById('new_button'),
            'button'
          ).click();
          const width = expectElementKind(
            await app.getById('settings_terminal_width_entry'),
            'entry'
          );
          await width.setText('94');
          await expectElementKind(
            await app.getById('connect_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            const capture = await readLaunchCapture(fakeVte.capture);
            expect(capture.args[0]).toBe('-s');
            expect(capture.args).toHaveLength(2);
            expect(capture.startupContent).toContain(
              '[general]\nname=New connection'
            );
            expect(capture.startupContent).toContain('width=94');
          });
        },
        {
          args: [],
          env: {
            ELDER_TERMS_TEST_CAPTURE: fakeVte.capture,
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await fakeVte.release();
    }
  });
});
