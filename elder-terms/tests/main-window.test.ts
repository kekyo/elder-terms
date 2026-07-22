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
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

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

const prepareProfiles = async (connections: string): Promise<void> => {
  await writeFile(join(connections, 'Alpha.ini'), '[terminal]\nwidth=88\n');
  await writeFile(join(connections, 'Beta.ini'), '[terminal]\nwidth=99\n');
};

interface FakeVteContext {
  readonly executable: string;
  readonly capture: string;
  readonly release: () => Promise<void>;
}

const createFakeVte = async (): Promise<FakeVteContext> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-fake-vte-'));
  const executable = join(directory, 'fake-vte.mjs');
  const capture = join(directory, 'capture.json');
  await writeFile(
    executable,
    `#!/usr/bin/env node
import { readFile, writeFile } from 'node:fs/promises';
const args = process.argv.slice(2);
const startupIndex = args.indexOf('-s');
const startupContent = startupIndex < 0 ? null : await readFile(args[startupIndex + 1], 'utf8');
await writeFile(process.env.ELDER_TERMS_TEST_CAPTURE, JSON.stringify({ args, startupContent }));
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
  it('starts unselected in a resizable split layout', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      expect(await app.getWindowCount()).toBe(1);
      const window = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      const left = await app.getById('connection_scroller');
      const right = await app.getById('details_stack');
      const list = await app.getById('connection_list');
      const apply = await app.getById('apply_button');
      const connect = await app.getById('connect_button');

      expect(['table', 'tree']).toContain(list.kind);
      expect((await connect.info()).name).toBe('Launch');
      await expectInsensitive(apply);
      await expectInsensitive(connect);
      const before = await window.bounds();
      await window.resizeTo(before.width + 120, before.height + 80);
      const after = await window.bounds();
      expect(after.width).toBeGreaterThan(before.width);
      expect(after.height).toBeGreaterThan(before.height);
      expect((await left.capture()).bounds.x).toBeLessThan(
        (await right.capture()).bounds.x
      );
    });
  });

  it('loads, edits, and applies a selected connection', async (context) => {
    await runLauncherGtkTest(
      context,
      prepareProfiles,
      async ({ app, connections }) => {
        const list = await app.getById('connection_list');
        await selectConnectionRow(app, list, 0);
        const width = expectElementKind(
          await app.getById('settings_terminal_width_spin'),
          'spinButton'
        );
        await waitForResult(async () => {
          expect(await width.value()).toBe(88);
        });

        const apply = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        await expectInsensitive(apply);
        await width.setValue(91);
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
          await app.getById('settings_terminal_width_spin'),
          'spinButton'
        );
        await width.setValue(92);
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
        expect(await width.value()).toBe(92);

        await newButton.click();
        await expectElementKind(
          await app.getById('discard_changes_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await width.value()).toBe(80);
        });
      }
    );
  });

  it('confirms before changing selection or closing with unsaved edits', async (context) => {
    await runLauncherGtkTest(context, prepareProfiles, async ({ app }) => {
      const list = await app.getById('connection_list');
      await selectConnectionRow(app, list, 0);
      const width = expectElementKind(
        await app.getById('settings_terminal_width_spin'),
        'spinButton'
      );
      await waitForResult(async () => {
        expect(await width.value()).toBe(88);
      });
      const apply = expectElementKind(
        await app.getById('apply_button'),
        'button'
      );
      await width.setValue(96);
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
      expect(await width.value()).toBe(96);

      await selectConnectionRow(app, list, 1);
      await expectElementKind(
        await app.getById('discard_changes_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await width.value()).toBe(99);
      });
      await width.setValue(97);
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
      expect(await width.value()).toBe(97);
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
          await app.getById('settings_terminal_width_spin'),
          'spinButton'
        );
        const apply = expectElementKind(
          await app.getById('apply_button'),
          'button'
        );
        await waitForResult(async () => {
          expect(await width.value()).toBe(88);
        });

        await width.setValue(91);
        await expectSensitive(apply);
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[terminal]\nwidth=95\n'
        );
        await waitForResult(async () => {
          expect(await width.value()).toBe(95);
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
          expect(await width.value()).toBe(95);
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
            await app.getById('settings_terminal_width_spin'),
            'spinButton'
          );
          await waitForResult(async () => {
            expect(await width.value()).toBe(88);
          });
          await width.setValue(93);
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
            await app.getById('settings_terminal_width_spin'),
            'spinButton'
          );
          await width.setValue(94);
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
