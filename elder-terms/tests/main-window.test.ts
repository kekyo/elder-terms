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
    const cell = await element.cellAt(row, 0);
    expect(cell).toBeDefined();
    const bounds = (await cell?.capture())?.bounds;
    expect(bounds).toBeDefined();
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
        ).toBe('グローバル既定値');
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
        ).toBe('Global defaults');
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
      async () => {},
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
        ).toBe('グローバル既定値');
        expect((await (await app.getById('apply_button')).info()).name).toBe(
          '保存'
        );

        const dialog = await openGlobalDefaults(app);
        await waitForResult(async () => {
          expect((await dialog.x11Info()).title).toBe('グローバル既定値');
          expect(await visibleSettingsTabNames(app, 'global_settings')).toEqual(
            [
              '一般',
              'TELNET',
              'シリアル',
              'SSH',
              'SFTP',
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
        await openGlobalDefaults(app);
        const language = expectElementKind(
          await app.getById('global_settings_general_ui_language_combo'),
          'comboBox'
        );
        await language.selectChildAt(6);
        await expectElementKind(
          await app.getById('global_defaults_save_button'),
          'button'
        ).click();

        await waitForWindowCount(app, 2);
        const restartDialog = expectElementKind(
          await app.getById('ui_language_restart_dialog'),
          'infoBar'
        );
        expect((await restartDialog.info()).states).toContain('modal');
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
        ).toBe('Global defaults');
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
        ).toBe('グローバル既定値');
        await openGlobalDefaults(app);
        const language = expectElementKind(
          await app.getById('global_settings_general_ui_language_combo'),
          'comboBox'
        );
        await language.selectChildAt(0);
        await expectElementKind(
          await app.getById('global_defaults_save_button'),
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
          ).toBe('Global defaults');
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

      await openGlobalDefaults(app);
      await expectElementKind(
        await app.getById('global_settings_general_ui_language_combo'),
        'comboBox'
      ).selectChildAt(6);
      await expectElementKind(
        await app.getById('global_defaults_save_button'),
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
        expect((await globalButton.info()).name).toBe('Global defaults');

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
              'TELNET',
              'Serial',
              'SSH',
              'SFTP',
              'Terminal',
              'Transfer',
              'Logging',
            ]
          );
        });
        expect(
          await app.findById('global_settings_general_name_entry')
        ).toBeUndefined();

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
        const language = expectElementKind(
          await app.getById('global_settings_general_ui_language_combo'),
          'comboBox'
        );
        await language.selectChildAt(6);
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
        expect((await deleteDialog.info()).states).toContain('modal');
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
        expect((await dialog.info()).states).toContain('modal');
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

  it('confirms before changing selection or closing with unsaved edits', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
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
      await width.setText('96');
      await expectSensitive(apply);

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

      await selectConnectionRow(app, list, 1);
      await waitForResult(
        async () => {
          expect(await app.getWindowCount()).toBe(2);
        },
        { message: 'selection change should show discard confirmation' }
      );
      const discardChanges = await waitForResult(async () =>
        expectElementKind(await app.getById('discard_changes_button'), 'button')
      );
      await discardChanges.click();
      await waitForResult(async () => {
        expect(Number(await width.text())).toBe(99);
      });
      await width.setText('97');
      await expectSensitive(apply);

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

        await width.setText('91');
        await expectSensitive(apply);
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

  it('routes an SFTP profile to the dedicated SFTP application', async (context) => {
    const fakeVte = await createFakeVte();
    const fakeSftp = await createFakeVte(true);
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
            const capture = await readLaunchCapture(fakeSftp.capture);
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
            ELDER_TERMS_SFTP_PATH: fakeSftp.executable,
            ELDER_TERMS_TEST_CAPTURE: fakeSftp.capture,
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await Promise.all([fakeSftp.release(), fakeVte.release()]);
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
