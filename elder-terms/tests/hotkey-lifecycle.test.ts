import { chmod, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type {
  GtkApp,
  GtkEntryElement,
  GtkKeyboardModifier,
  GtkKeyInput,
  GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const ctrlAltT = {
  key: 't',
  modifiers: ['control', 'alt'],
} as const;

const ctrlShiftY = {
  key: 'y',
  modifiers: ['control', 'shift'],
} as const;

const japaneseTestEnvironment = {
  ELDER_TERMS_LOCALE_DIR: fileURLToPath(
    new URL('../../.build/po/', import.meta.url)
  ),
  LANGUAGE: 'ja',
  LC_ALL: 'ja_JP.UTF-8',
} as const;

const writeGlobalSettings = async (
  connections: string,
  openApplication: string | undefined
): Promise<void> => {
  const hotkey =
    openApplication === undefined
      ? ''
      : `open_application=${openApplication}\n`;
  await writeFile(
    join(connections, '..', 'global.ini'),
    `[general]\nstartup_mode=tray\n${hotkey}`
  );
};

interface FakeChildContext {
  readonly capture: string;
  readonly executable: string;
  readonly release: () => Promise<void>;
}

interface ChildCapture {
  readonly activationToken: string | null;
  readonly args: readonly string[];
}

const createFakeChild = async (): Promise<FakeChildContext> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-hotkey-child-'));
  const executable = join(directory, 'fake-child.mjs');
  const capture = join(directory, 'capture.json');
  await writeFile(
    executable,
    `#!/usr/bin/env node
import { writeFile } from 'node:fs/promises';
await writeFile(
  ${JSON.stringify(capture)},
  JSON.stringify({
    activationToken: process.env.XDG_ACTIVATION_TOKEN ?? null,
    args: process.argv.slice(2),
  })
);
`
  );
  await chmod(executable, 0o755);
  return {
    capture,
    executable,
    release: async () => rm(directory, { recursive: true, force: true }),
  };
};

const waitForChildCapture = async (path: string): Promise<ChildCapture> =>
  waitForResult(
    async () => JSON.parse(await readFile(path, 'utf8')) as ChildCapture
  );

const waitForWindowCount = async (
  app: GtkApp,
  expected: number
): Promise<void> => {
  await waitForResult(async () => {
    expect(await app.getWindowCount()).toBe(expected);
  });
};

const pressShortcut = async (
  app: GtkApp,
  modifiers: readonly GtkKeyboardModifier[],
  key: GtkKeyInput
): Promise<void> => {
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

const clickWidget = async (
  app: GtkApp,
  widget: GtkWidgetElement
): Promise<void> => {
  const bounds = await waitForResult(async () => {
    expect((await widget.info()).states).toContain('showing');
    const capture = await widget.capture();
    expect(capture.bounds.width).toBeGreaterThan(0);
    expect(capture.bounds.height).toBeGreaterThan(0);
    return capture.bounds;
  });
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + bounds.width / 2),
    Math.trunc(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
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
  for (let index = 0; index < childCount; index += 1) {
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

const captureShortcut = async (
  app: GtkApp,
  entry: GtkEntryElement,
  modifiers: readonly GtkKeyboardModifier[],
  key: GtkKeyInput
): Promise<void> => {
  await clickWidget(app, entry);
  await pressShortcut(app, modifiers, key);
};

const closeWindowWithAccelerator = async (app: GtkApp): Promise<void> => {
  await pressShortcut(app, ['control'], 'w');
};

const selectConnection = async (app: GtkApp, row: number): Promise<void> => {
  const list = await app.getById('connection_list');
  if (list.kind === 'tree') {
    await list.selectChildAt(row);
    return;
  }
  if (list.kind === 'table') {
    const cell = await list.cellAt(row, 0);
    expect(cell).toBeDefined();
    const capture = await cell?.capture();
    expect(capture).toBeDefined();
    if (capture === undefined) {
      return;
    }
    await app.input.moveMouseTo(
      Math.round(capture.bounds.x + capture.bounds.width / 2),
      Math.round(capture.bounds.y + capture.bounds.height / 2)
    );
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    return;
  }
  throw new Error(`Unexpected connection list kind: ${list.kind}`);
};

const selectGeneralSettingsTab = async (
  app: GtkApp,
  notebookId: string
): Promise<void> => {
  const notebook = expectElementKind(await app.getById(notebookId), 'tabList');
  const childCount = await notebook.getChildCount();
  for (let index = 0; index < childCount; index += 1) {
    const tab = await notebook.childAt(index);
    if (tab !== undefined && (await tab.info()).name === 'General') {
      await notebook.selectChildAt(index);
      return;
    }
  }
  throw new Error('General settings tab was not found');
};

describe('elder-terms application hotkey lifecycle', () => {
  it('opens the tray-resident application with the default hotkey', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, undefined);
      },
      async ({ app }) => {
        await waitForWindowCount(app, 0);
        await pressShortcut(app, ['control', 'alt'], 't');
        await waitForWindowCount(app, 1);
      }
    );
  });

  it('uses a configured hotkey instead of the built-in default', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, 'ctrl+shift+y');
      },
      async ({ app }) => {
        await waitForWindowCount(app, 0);
        await pressShortcut(app, ['control', 'alt'], 't');
        expect(await app.getWindowCount()).toBe(0);
        await pressShortcut(app, ['control', 'shift'], 'y');
        await waitForWindowCount(app, 1);
      }
    );
  });

  it('leaves the application hotkey disabled for an explicit empty value', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, '');
      },
      async ({ app }) => {
        await waitForWindowCount(app, 0);
        await pressShortcut(app, ['control', 'alt'], 't');
        expect(await app.getWindowCount()).toBe(0);
        const tray = await app.getTrayItem({ id: 'elder-terms' });
        await tray.click();
        await waitForWindowCount(app, 1);
      }
    );
  });

  it('shows a localized dialog when the initial hotkey registration fails', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, 'ctrl+alt+t');
      },
      async ({ app }) => {
        await waitForWindowCount(app, 1);
        const dialog = expectElementKind(
          await app.getById('hotkey_registration_error_dialog'),
          'infoBar'
        );
        expect(
          await findDescendantByName(
            dialog,
            'label',
            'グローバルショートカットを利用できません'
          )
        ).toBeDefined();
      },
      {
        args: [],
        blockedHotkeys: [ctrlAltT],
        env: japaneseTestEnvironment,
      }
    );
  });

  it('does not show a registration error when every hotkey is disabled', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, '');
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[general]\nopen_connection=\n'
        );
      },
      async ({ app }) => {
        expect(
          await app.findById('hotkey_registration_error_dialog')
        ).toBeUndefined();
        const tray = await app.getTrayItem({ id: 'elder-terms' });
        await tray.click();
        await waitForWindowCount(app, 1);
        expect(
          await app.findById('hotkey_registration_error_dialog')
        ).toBeUndefined();
      },
      {
        args: [],
        blockedHotkeys: [ctrlAltT],
        env: {},
      }
    );
  });

  it('shows the registration error after the first hotkey is added to a connection', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, '');
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[general]\nopen_connection=\n'
        );
      },
      async ({ app, x11MapRecorder }) => {
        const tray = await app.getTrayItem({ id: 'elder-terms' });
        await tray.click();
        await waitForWindowCount(app, 1);
        expect(
          await app.findById('hotkey_registration_error_dialog')
        ).toBeUndefined();
        await selectConnection(app, 0);
        await selectGeneralSettingsTab(app, 'settings_notebook');

        const entry = expectElementKind(
          await app.getById('settings_general_open_connection_entry'),
          'entry'
        );
        await captureShortcut(app, entry, ['control', 'alt'], 't');
        await waitForResult(async () => {
          expect(await entry.text()).toBe('ctrl+alt+t');
        });
        expect(x11MapRecorder).toBeDefined();
        await x11MapRecorder?.grabHotkey(ctrlAltT);
        await expectElementKind(
          await app.getById('apply_button'),
          'button'
        ).click();

        await waitForWindowCount(app, 2);
        expectElementKind(
          await app.getById('hotkey_registration_error_dialog'),
          'infoBar'
        );
        await expectElementKind(
          await app.getById('hotkey_registration_error_close_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);

        await captureShortcut(app, entry, ['control', 'shift'], 'y');
        await waitForResult(async () => {
          expect(await entry.text()).toBe('ctrl+shift+y');
        });
        await x11MapRecorder?.grabHotkey(ctrlShiftY);
        await expectElementKind(
          await app.getById('apply_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(
            await app.findById('hotkey_registration_error_dialog')
          ).toBeUndefined();
        });
        expect(await app.getWindowCount()).toBe(1);
      },
      {
        args: [],
        env: {},
        recordX11Maps: true,
      }
    );
  });

  it('replaces the active hotkey immediately after application settings are saved', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, 'ctrl+alt+t');
      },
      async ({ app }) => {
        const tray = await app.getTrayItem({ id: 'elder-terms' });
        await tray.click();
        await waitForWindowCount(app, 1);
        await expectElementKind(
          await app.getById('application_menu_button'),
          'toggleButton'
        ).click();
        await expectElementKind(
          await app.getById('application_settings_menu_item'),
          'menuItem'
        ).click();
        await waitForWindowCount(app, 2);

        const entry = expectElementKind(
          await app.getById('application_settings_open_application_entry'),
          'entry'
        );
        await captureShortcut(app, entry, ['control', 'shift'], 'y');
        await waitForResult(async () => {
          expect(await entry.text()).toBe('ctrl+shift+y');
        });
        await expectElementKind(
          await app.getById('application_dialog_save_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 1);
        await closeWindowWithAccelerator(app);
        await waitForWindowCount(app, 0);

        await pressShortcut(app, ['control', 'alt'], 't');
        expect(await app.getWindowCount()).toBe(0);
        await pressShortcut(app, ['control', 'shift'], 'y');
        await waitForWindowCount(app, 1);
      }
    );
  });

  it('launches saved terminal, SFTP, and FTP connections while resident in the tray', async (context) => {
    const fakeVte = await createFakeChild();
    const fakeSftp = await createFakeChild();
    const fakeFtp = await createFakeChild();
    try {
      await runLauncherGtkTest(
        context,
        async (connections) => {
          await writeGlobalSettings(connections, '');
          await writeFile(
            join(connections, 'Alpha.ini'),
            '[general]\nopen_connection=ctrl+shift+y\n'
          );
          await writeFile(
            join(connections, 'Files.ini'),
            [
              '[general]',
              'type=sftp',
              'open_connection=ctrl+shift+u',
              '',
              '[ssh]',
              'address=files.example',
              '',
            ].join('\n')
          );
          await writeFile(
            join(connections, 'Legacy files.ini'),
            [
              '[general]',
              'type=ftp',
              'open_connection=ctrl+shift+i',
              '',
              '[ftp]',
              'address=ftp.example',
              '',
            ].join('\n')
          );
        },
        async ({ app, connections }) => {
          await waitForWindowCount(app, 0);
          await pressShortcut(app, ['control', 'shift'], 'y');
          expect(await waitForChildCapture(fakeVte.capture)).toEqual({
            activationToken: null,
            args: ['-c', join(connections, 'Alpha.ini')],
          });
          await pressShortcut(app, ['control', 'shift'], 'u');
          expect(await waitForChildCapture(fakeSftp.capture)).toEqual({
            activationToken: null,
            args: ['-c', join(connections, 'Files.ini')],
          });
          await pressShortcut(app, ['control', 'shift'], 'i');
          expect(await waitForChildCapture(fakeFtp.capture)).toEqual({
            activationToken: null,
            args: ['-c', join(connections, 'Legacy files.ini')],
          });
          expect(await app.getWindowCount()).toBe(0);
        },
        {
          args: [],
          env: {
            ELDER_TERMS_FTP_PATH: fakeFtp.executable,
            ELDER_TERMS_SFTP_PATH: fakeSftp.executable,
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await Promise.all([
        fakeFtp.release(),
        fakeSftp.release(),
        fakeVte.release(),
      ]);
    }
  });

  it('warns for duplicate connection hotkeys and launches the first profile', async (context) => {
    const fakeVte = await createFakeChild();
    try {
      await runLauncherGtkTest(
        context,
        async (connections) => {
          await writeGlobalSettings(connections, 'ctrl+alt+t');
          const duplicate = '[general]\nopen_connection=ctrl+alt+t\n';
          await writeFile(join(connections, 'Alpha.ini'), duplicate);
          await writeFile(join(connections, 'Beta.ini'), duplicate);
        },
        async ({ app, connections }) => {
          await waitForResult(async () => {
            expect((await app.output()).stderr).toContain(
              'Warning: connection hotkey ctrl+alt+t is assigned to both Alpha and Beta; using Alpha'
            );
          });
          await pressShortcut(app, ['control', 'alt'], 't');
          expect(await waitForChildCapture(fakeVte.capture)).toEqual({
            activationToken: null,
            args: ['-c', join(connections, 'Alpha.ini')],
          });
          expect(await app.getWindowCount()).toBe(0);
        },
        {
          args: [],
          env: {
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await fakeVte.release();
    }
  });

  it('replaces connection hotkeys after external changes and saves', async (context) => {
    const fakeVte = await createFakeChild();
    try {
      await runLauncherGtkTest(
        context,
        async (connections) => {
          await writeGlobalSettings(connections, '');
          await writeFile(
            join(connections, 'Alpha.ini'),
            '[general]\nopen_connection=ctrl+shift+y\n'
          );
          await writeFile(join(connections, 'Beta.ini'), '[general]\n');
        },
        async ({ app, connections }) => {
          const tray = await app.getTrayItem({ id: 'elder-terms' });
          await tray.click();
          await waitForWindowCount(app, 1);
          await selectConnection(app, 0);
          const entry = expectElementKind(
            await app.getById('settings_general_open_connection_entry'),
            'entry'
          );
          await waitForResult(async () => {
            expect(await entry.text()).toBe('ctrl+shift+y');
          });

          await writeFile(
            join(connections, 'Alpha.ini'),
            '[general]\nopen_connection=ctrl+shift+u\n'
          );
          await waitForResult(async () => {
            expect(await entry.text()).toBe('ctrl+shift+u');
          });
          await closeWindowWithAccelerator(app);
          await waitForWindowCount(app, 0);
          await pressShortcut(app, ['control', 'shift'], 'u');
          expect(await waitForChildCapture(fakeVte.capture)).toEqual({
            activationToken: null,
            args: ['-c', join(connections, 'Alpha.ini')],
          });

          await rm(fakeVte.capture, { force: true });
          await tray.click();
          await waitForWindowCount(app, 1);
          await selectConnection(app, 1);
          const alternateEntry = expectElementKind(
            await app.getById('settings_general_open_connection_entry'),
            'entry'
          );
          await waitForResult(async () => {
            expect(await alternateEntry.text()).toBe('');
          });
          await selectConnection(app, 0);
          const savedEntry = expectElementKind(
            await app.getById('settings_general_open_connection_entry'),
            'entry'
          );
          await waitForResult(async () => {
            expect(await savedEntry.text()).toBe('ctrl+shift+u');
          });
          await selectGeneralSettingsTab(app, 'settings_notebook');
          await captureShortcut(app, savedEntry, ['control', 'shift'], 'i');
          await expectElementKind(
            await app.getById('apply_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(
              await readFile(join(connections, 'Alpha.ini'), 'utf8')
            ).toContain('open_connection=ctrl+shift+i');
          });
          await closeWindowWithAccelerator(app);
          await waitForWindowCount(app, 0);
          await pressShortcut(app, ['control', 'shift'], 'i');
          expect(await waitForChildCapture(fakeVte.capture)).toEqual({
            activationToken: null,
            args: ['-c', join(connections, 'Alpha.ini')],
          });
        },
        {
          args: [],
          env: {
            ELDER_TERMS_VTE_PATH: fakeVte.executable,
          },
        }
      );
    } finally {
      await fakeVte.release();
    }
  });
});
