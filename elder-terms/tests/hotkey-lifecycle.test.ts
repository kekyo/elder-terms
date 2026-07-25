import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
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

const selectGeneralSettingsTab = async (app: GtkApp): Promise<void> => {
  const notebook = expectElementKind(
    await app.getById('global_settings_notebook'),
    'tabList'
  );
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

  it('replaces the active hotkey immediately after global settings are saved', async (context) => {
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
          await app.getById('global_defaults_button'),
          'button'
        ).click();
        await waitForWindowCount(app, 2);
        await selectGeneralSettingsTab(app);

        const entry = expectElementKind(
          await app.getById('global_settings_general_open_application_entry'),
          'entry'
        );
        await captureShortcut(app, entry, ['control', 'shift'], 'y');
        await waitForResult(async () => {
          expect(await entry.text()).toBe('ctrl+shift+y');
        });
        await expectElementKind(
          await app.getById('global_defaults_save_button'),
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
});
