import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
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
});
