import { execFile } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import type { GtkApp, GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const writeGlobalSettings = async (
  connections: string,
  startupMode: 'tray' | 'window_and_tray'
): Promise<void> => {
  await writeFile(
    join(connections, '..', 'global.ini'),
    `[general]\nstartup_mode=${startupMode}\nopen_application=\n`
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

const closeWindowWithAccelerator = async (app: GtkApp): Promise<void> => {
  await app.input.setModifier('control', true);
  try {
    await app.input.pressKey('w');
  } finally {
    await app.input.setModifier('control', false);
  }
};

const selectFirstConnection = async (
  app: GtkApp,
  connectionList: GtkWidgetElement
): Promise<void> => {
  if (connectionList.kind === 'tree') {
    await connectionList.selectChildAt(0);
    return;
  }
  if (connectionList.kind !== 'table') {
    throw new Error(`Unexpected connection list kind: ${connectionList.kind}`);
  }
  const cell = await connectionList.cellAt(0, 0);
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
};

const callSessionBus = async (
  app: GtkApp,
  args: readonly string[]
): Promise<string> => {
  const environment = await app.environment();
  return new Promise<string>((resolve, reject) => {
    execFile(
      'gdbus',
      ['call', '--session', ...args],
      {
        encoding: 'utf8',
        env: { ...process.env, ...environment },
      },
      (error, stdout, stderr) => {
        if (error !== null) {
          reject(new Error(`gdbus call failed: ${stderr.trim()}: ${error}`));
          return;
        }
        resolve(stdout);
      }
    );
  });
};

const statusNotifierBusName = async (app: GtkApp): Promise<string> => {
  const registeredItems = await callSessionBus(app, [
    '--dest',
    'org.kde.StatusNotifierWatcher',
    '--object-path',
    '/StatusNotifierWatcher',
    '--method',
    'org.freedesktop.DBus.Properties.Get',
    'org.kde.StatusNotifierWatcher',
    'RegisteredStatusNotifierItems',
  ]);
  const item = registeredItems.match(
    /['"]([^'"]+)\/StatusNotifierItem['"]/u
  )?.[1];
  if (item === undefined) {
    throw new Error(
      `StatusNotifier item was not registered: ${registeredItems}`
    );
  }
  return item;
};

describe('elder-terms tray lifecycle', () => {
  it('starts hidden and opens from both the tray icon and menu', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, 'tray');
      },
      async ({ app, x11MapRecorder }) => {
        await waitForWindowCount(app, 0);
        const tray = await app.getTrayItem({ id: 'elder-terms' });
        expect(await tray.metadata()).toMatchObject({
          id: 'elder-terms',
          title: 'elder-terms',
          status: 'Active',
          iconName: 'elder-terms',
          backend: 'status-notifier',
        });
        expect(x11MapRecorder).toBeDefined();
        await x11MapRecorder?.flush();
        expect(
          x11MapRecorder
            ?.events()
            .filter(
              (event) =>
                event.name === 'elder-terms' ||
                event.instanceName === 'elder-terms'
            )
        ).toEqual([]);

        await tray.click();
        await waitForWindowCount(app, 1);
        await x11MapRecorder?.flush();
        expect(
          x11MapRecorder
            ?.events()
            .some(
              (event) =>
                event.name === 'elder-terms' ||
                event.instanceName === 'elder-terms'
            )
        ).toBe(true);
        await closeWindowWithAccelerator(app);
        await waitForWindowCount(app, 0);

        const busName = await statusNotifierBusName(app);
        const layout = await callSessionBus(app, [
          '--dest',
          busName,
          '--object-path',
          '/StatusNotifierMenu',
          '--method',
          'com.canonical.dbusmenu.GetLayout',
          '0',
          '1',
          '[]',
        ]);
        expect(layout).toContain('Open elder-terms');
        await callSessionBus(app, [
          '--dest',
          busName,
          '--object-path',
          '/StatusNotifierMenu',
          '--method',
          'com.canonical.dbusmenu.Event',
          '1',
          'clicked',
          '<0>',
          '0',
        ]);
        await waitForWindowCount(app, 1);
      },
      {
        args: [],
        env: {},
        recordX11Maps: true,
      }
    );
  });

  it('keeps unsaved editor state while the tray window is hidden', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, 'Alpha.ini'),
          '[terminal]\nwidth=88\n'
        );
        await writeGlobalSettings(connections, 'tray');
      },
      async ({ app }) => {
        const tray = await app.getTrayItem({ id: 'elder-terms' });
        await tray.click();
        await waitForWindowCount(app, 1);
        await selectFirstConnection(app, await app.getById('connection_list'));
        const width = expectElementKind(
          await app.getById('settings_terminal_width_entry'),
          'entry'
        );
        await width.setText('101');

        await closeWindowWithAccelerator(app);
        await waitForWindowCount(app, 0);

        await tray.click();
        await waitForWindowCount(app, 1);
        expect(
          Number(
            await expectElementKind(
              await app.getById('settings_terminal_width_entry'),
              'entry'
            ).text()
          )
        ).toBe(101);
      }
    );
  });

  it('starts with both the window and tray when configured', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, 'window_and_tray');
      },
      async ({ app }) => {
        await waitForWindowCount(app, 1);
        expect(await app.getTrayItem({ id: 'elder-terms' })).toBeDefined();
      }
    );
  });

  it('shows the window when no tray host can retain the application', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, 'tray');
      },
      async ({ app }) => {
        await waitForWindowCount(app, 1);
      },
      {
        args: [],
        env: {},
        xvfbTrayHost: false,
      }
    );
  });
});
