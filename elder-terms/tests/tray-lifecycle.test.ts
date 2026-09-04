import { execFile, spawnSync } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { GtkApp, GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const writeGlobalSettings = async (
  connections: string,
  startupMode: 'background' | 'tray' | 'window_and_tray'
): Promise<void> => {
  await writeFile(
    join(connections, '..', 'global.ini'),
    `[general]\nstartup_mode=${startupMode}\nopen_application=\n`
  );
};

const launcherPath = fileURLToPath(
  new URL('../../.build/elder-terms/elder-terms', import.meta.url)
);

const activateRunningApplication = async (app: GtkApp): Promise<void> => {
  const environment = await app.environment();
  await new Promise<void>((resolve, reject) => {
    execFile(
      launcherPath,
      [],
      {
        env: { ...process.env, ...environment },
      },
      (error) => {
        if (error !== null) {
          reject(error);
          return;
        }
        resolve();
      }
    );
  });
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
  it('starts in the background without a window or tray until activated again', async (context) => {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeGlobalSettings(connections, 'background');
      },
      async ({ app }) => {
        await waitForWindowCount(app, 0);

        await activateRunningApplication(app);

        await waitForWindowCount(app, 1);
        expect(await app.getTrayItemCount()).toBe(0);
      }
    );
  });

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

        const competitorWindowId = await x11MapRecorder?.focusCompetitor();
        expect(competitorWindowId).toBeDefined();
        await waitForResult(async () => {
          expect(await x11MapRecorder?.focusedWindow()).toBe(
            competitorWindowId
          );
        });
        await tray.click();
        await tray.click();
        await waitForWindowCount(app, 1);
        const mainWindow = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        const mainWindowId = String(
          Number.parseInt((await mainWindow.x11Info()).windowId, 16)
        );
        await waitForResult(async () => {
          expect(await x11MapRecorder?.focusedWindow()).toBe(mainWindowId);
        });
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
        expect(layout).toContain('Application settings');
        expect(layout).toContain('About elder-terms');
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

        await closeWindowWithAccelerator(app);
        await waitForWindowCount(app, 0);
        await callSessionBus(app, [
          '--dest',
          busName,
          '--object-path',
          '/StatusNotifierMenu',
          '--method',
          'com.canonical.dbusmenu.Event',
          '4',
          'clicked',
          '<0>',
          '0',
        ]);
        await waitForWindowCount(app, 1);
        expectElementKind(await app.getById('application_dialog'), 'window');
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
        expect(
          await expectElementKind(
            await app.getById('application_about_version_label'),
            'label'
          ).text()
        ).toBe(`Version ${version.stdout.trim()}`);
      },
      {
        args: [],
        env: {},
        recordX11Maps: true,
      }
    );
  }, 60_000);

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
