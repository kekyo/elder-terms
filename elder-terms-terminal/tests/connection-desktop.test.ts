// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { expect, it } from 'vitest';
import { PNG } from 'pngjs';
import { createGtkAppLauncher } from 'gestament';
import type { GtkApp, GtkWidgetElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { runGtkTest } from './gtk-test-helpers';
import {
  withTemporaryDirectory,
  expectElementKind,
} from '../../shared/tests/connection-test-helpers.ts';

const clickWidget = async (
  app: GtkApp,
  widget: GtkWidgetElement
): Promise<void> => {
  const { bounds } = await widget.capture();
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + bounds.width / 2),
    Math.trunc(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
};

it('uses a saved connection identity and prepares its desktop entry before showing the window', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const data = join(directory, 'data');
    const config = join(directory, 'connection with spaces % $ ".ini');
    const id = '180acb56-2e23-47c4-a827-d2c29ed73ba5';
    const appId = `net.kekyo.elder-terms.connection.${id}`;
    await mkdir(data);
    await writeFile(
      config,
      `[general]\nname=Desktop icon test\nconnection_id=${id}\nicon=material:white:action/home\n`
    );
    await runGtkTest(
      context,
      ['--test-fixture', '-c', config],
      async (app) => {
        const window = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        const native = await window.x11Info();
        expect(native.className).toBe(appId);
        expect(native.instanceName).toBe(appId);
        const entry = await readFile(
          join(data, 'applications', `${appId}.desktop`),
          'utf8'
        );
        expect(entry).toContain('Name=Desktop icon test');
        expect(entry).toContain('NoDisplay=true');
        expect(entry).toContain(`StartupWMClass=${appId}`);
        const iconName = entry.match(/^Icon=(.+)$/mu)?.[1];
        expect(iconName).toBe(appId);
        const iconPath = join(
          data,
          'icons',
          'hicolor',
          '256x256',
          'apps',
          `${iconName}.png`
        );
        const firstImage = await readFile(iconPath);
        expect(firstImage.subarray(1, 4).toString()).toBe('PNG');
        const exported = PNG.sync.read(firstImage);
        expect([exported.width, exported.height]).toEqual([256, 256]);
        const center = (128 * exported.width + 128) * 4;
        expect([...exported.data.subarray(center, center + 4)]).toEqual([
          255, 255, 255, 255,
        ]);
        const outline = (190 * exported.width + 90) * 4;
        expect([...exported.data.subarray(outline, outline + 4)]).toEqual([
          0, 0, 0, 255,
        ]);
        expect(exported.data[(128 * exported.width + 32) * 4 + 3]).toBe(0);

        await clickWidget(app, await app.getById('application_menu_button'));
        await expectElementKind(
          await app.getById('settings_menu_item'),
          'menuItem'
        ).click();
        await expectElementKind(
          await app.getById('settings_general_icon_reset'),
          'button'
        ).click();
        await expectElementKind(
          await app.getById('settings_apply_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          const updated = await readFile(
            join(data, 'applications', `${appId}.desktop`),
            'utf8'
          );
          expect(updated.match(/^Icon=(.+)$/mu)?.[1]).toBe(iconName);
          expect((await readFile(iconPath)).equals(firstImage)).toBe(false);
        });
      },
      { env: { XDG_DATA_HOME: data, XDG_CONFIG_HOME: directory } }
    );
  });
});

it('uses the same connection identity for the SFTP file-transfer window', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-desktop-sftp-'));
  const data = join(directory, 'data');
  const config = join(directory, 'sftp.ini');
  const id = '9d738f4c-5d58-41bc-bc9c-9e68e6db35ad';
  const appId = `net.kekyo.elder-terms.connection.${id}`;
  await mkdir(data);
  await writeFile(
    config,
    `[general]\nname=SFTP desktop\ntype=sftp\nconnection_id=${id}\nicon=noto:emoji_u1f44d\n` +
      `[ssh]\naddress=fixture.example\n[sftp]\nlocal_directory=${directory}\n`
  );
  const launcher = createGtkAppLauncher({
    appPath: fileURLToPath(
      new URL(
        '../../.build/elder-terms-file-transfer/elder-terms-file-transfer',
        import.meta.url
      )
    ),
    env: {
      XDG_DATA_HOME: data,
      XDG_CONFIG_HOME: directory,
      LANGUAGE: 'en',
      LC_ALL: 'C.UTF-8',
    },
    xvfbTrayHost: true,
  });
  try {
    const app = await launcher.launch(['--test-fixture', '-c', config]);
    const window = expectElementKind(
      await app.getById('file_transfer_window'),
      'window'
    );
    const native = await window.x11Info();
    expect(native.className).toBe(appId);
    expect(native.instanceName).toBe(appId);
    const entry = await readFile(
      join(data, 'applications', `${appId}.desktop`),
      'utf8'
    );
    expect(entry).toContain('Name=SFTP desktop');
    expect(entry).toContain(`StartupWMClass=${appId}`);
  } finally {
    await launcher.release();
    await rm(directory, { recursive: true, force: true });
  }
});

it('keeps the saved ID when a startup draft is saved over its source', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const data = join(directory, 'data');
    const config = join(directory, 'saved.ini');
    const startup = join(directory, 'draft.ini');
    const savedId = '63630528-737d-47a6-a66f-bcc5c65ecaa4';
    await mkdir(data);
    await writeFile(
      config,
      `[general]\nname=Saved\nconnection_id=${savedId}\nicon=material:action/home\n`
    );
    await writeFile(startup, '[general]\nname=Draft\n');
    await runGtkTest(
      context,
      ['--test-fixture', '-c', config, '-s', startup],
      async (app) => {
        const window = expectElementKind(
          await app.getById('main_window'),
          'window'
        );
        const draftClass = (await window.x11Info()).className;
        expect(draftClass).toMatch(
          /^net\.kekyo\.elder-terms\.connection\.[0-9a-f-]{36}$/u
        );
        expect(draftClass).not.toBe(
          `net.kekyo.elder-terms.connection.${savedId}`
        );
        const desktop = await readFile(
          join(data, 'applications', `${draftClass}.desktop`),
          'utf8'
        );
        expect(desktop).toContain(
          `Exec="${fileURLToPath(new URL('../../.build/elder-terms/elder-terms', import.meta.url))}"`
        );
        await clickWidget(app, await app.getById('application_menu_button'));
        await expectElementKind(
          await app.getById('settings_menu_item'),
          'menuItem'
        ).click();
        await expectElementKind(
          await app.getById('settings_save_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await readFile(config, 'utf8')).toContain(
            `connection_id=${savedId}`
          );
        });
      },
      { env: { XDG_DATA_HOME: data, XDG_CONFIG_HOME: directory } }
    );
  });
});
