// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import {
  capturePixel,
  createTestEvidence,
  expectElementKind,
} from './test-helpers';

describe.concurrent('file transfer compact title bar', () => {
  for (const { type, tlsMode, theme, scale } of [
    { type: 'ftp', tlsMode: 'none', theme: 'Adwaita', scale: 1 },
    { type: 'ftp', tlsMode: 'explicit', theme: 'Adwaita:dark', scale: 1 },
    { type: 'sftp', tlsMode: 'none', theme: 'Adwaita', scale: 2 },
    { type: 'ftp', tlsMode: 'none', theme: 'Yaru', scale: 1 },
    { type: 'sftp', tlsMode: 'none', theme: 'Yaru-dark', scale: 1 },
    { type: 'ftp', tlsMode: 'implicit', theme: 'Yaru-dark', scale: 2 },
  ]) {
    it(`shrinks at startup and restores the themed height when toggled (${type}, ${tlsMode}, ${theme}, scale ${scale})`, async (context) => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-header-'));
      const configPath = join(directory, 'connection.ini');
      const evidence = createTestEvidence(context);
      const apps: GtkApp[] = [];
      const launcher = createGtkAppLauncher({
        appPath: fileURLToPath(
          new URL(
            '../../.build/elder-terms-terminal/elder-terms-file-transfer',
            import.meta.url
          )
        ),
        env: {
          LANGUAGE: 'en',
          LC_ALL: 'C.UTF-8',
          GTK_THEME: theme,
          GDK_SCALE: String(scale),
          XDG_CONFIG_HOME: join(directory, 'config'),
          XDG_DATA_HOME: join(directory, 'data'),
        },
        xvfbScreen: '2560x1600x24',
        onSystemOutput: evidence.recordSystemOutputEvent,
      });
      try {
        const settings = [
          `name=Compact ${type}`,
          `type=${type}`,
          '[ssh]',
          'address=fixture.example',
          '[sftp]',
          `local_directory=${directory}`,
          'remote_directory=/remote',
          '[ftp]',
          'address=fixture.example',
          'username=fixture-user',
          `tls_mode=${tlsMode}`,
          `local_directory=${directory}`,
          'remote_directory=/remote',
          '',
        ].join('\n');
        let regularHeight = 0;
        let regularConnectionIconSize = 0;
        for (const compact of [false, true]) {
          await writeFile(
            configPath,
            `[general]\ncompact_mode=${compact}\nshow_border=true\nborder_width=7\n${settings}`
          );
          const app = await launcher.launch(
            ['--test-fixture', '-c', configPath],
            {
              onOutput: evidence.recordAppOutputEvent,
            }
          );
          apps.push(app);
          const header = await app.getById('file_transfer_header_bar');
          const capture = await evidence.captureEvidence(
            compact ? 'compact-header' : 'regular-header',
            async () => header.capture()
          );
          if (!compact) {
            regularHeight = capture.bounds.height;
            regularConnectionIconSize = (
              await (
                await app.getById('file_transfer_connection_icon')
              ).capture()
            ).bounds.width;
            expect(regularHeight).toBeGreaterThan(0);
            await app.release();
            continue;
          }

          const compactHeight = Math.round(regularHeight * 0.75);
          expect(capture.bounds.height).toBe(compactHeight);
          const compactConnectionIconSize = (
            await (await app.getById('file_transfer_connection_icon')).capture()
          ).bounds.width;
          expect(compactConnectionIconSize).toBeLessThan(
            regularConnectionIconSize
          );
          expect(capture.clipped).toBe(false);
          for (const id of [
            'file_transfer_connection_icon',
            'file_transfer_title_label',
            'conn_indicator_image',
            'conn_indicator_label',
            'sd_indicator_image',
            'sd_indicator_label',
            'rd_indicator_image',
            'rd_indicator_label',
          ]) {
            const child = await (await app.getById(id)).capture();
            expect(child.clipped, id).toBe(false);
            expect(child.bounds.height, id).toBeGreaterThan(0);
            expect(child.bounds.y, id).toBeGreaterThanOrEqual(capture.bounds.y);
            expect(
              child.bounds.y + child.bounds.height,
              id
            ).toBeLessThanOrEqual(capture.bounds.y + compactHeight);
          }
          const buttons = [await app.getById('application_menu_button')];
          const pending: GtkWidgetElement[] = [header];
          let hasCloseButton = false;
          while (pending.length !== 0) {
            const widget = pending.shift()!;
            if (widget.kind === 'button') {
              buttons.push(widget);
              if ((await widget.info()).name === 'Close') hasCloseButton = true;
            }
            if ('getChildCount' in widget) {
              for (
                let index = 0;
                index < (await widget.getChildCount());
                ++index
              ) {
                const child = await widget.childAt(index);
                if (child !== undefined) pending.push(child);
              }
            }
          }
          expect(hasCloseButton).toBe(true);
          for (const button of buttons) {
            const { bounds } = await button.capture();
            expect(bounds.width).toBe(bounds.height);
            expect(bounds.height).toBeGreaterThanOrEqual(24);
            expect(bounds.y - capture.bounds.y).toBeGreaterThanOrEqual(2);
            expect(
              capture.bounds.y + compactHeight - bounds.y - bounds.height
            ).toBeGreaterThanOrEqual(2);
          }

          if (type === 'ftp') {
            await expectElementKind(
              await app.getById('file_transfer_prompt_accept_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect(
                await expectElementKind(
                  await app.getById('file_transfer_prompt_message_label'),
                  'label'
                ).text()
              ).toBe('Password:');
            });
            await expectElementKind(
              await app.getById('file_transfer_prompt_entry'),
              'entry'
            ).setText('fixture-secret');
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
            ).toBe('Ready');
          });

          // Reapplying Enabled must not shrink an already compact header.
          await expectElementKind(
            await app.getById('application_menu_button'),
            'toggleButton'
          ).click();
          await expectElementKind(
            await app.getById('settings_menu_item'),
            'menuItem'
          ).click();
          for (const enabled of [true, false, true, true, false]) {
            await expectElementKind(
              await app.getById('settings_general_compact_mode_combo'),
              'comboBox'
            ).selectChildAt(enabled ? 1 : 2);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect(await app.getWindowCount()).toBe(2);
              expect((await header.capture()).bounds.height).toBe(
                enabled ? compactHeight : regularHeight
              );
              expect(
                (
                  await (
                    await app.getById('file_transfer_connection_icon')
                  ).capture()
                ).bounds.width
              ).toBe(
                enabled ? compactConnectionIconSize : regularConnectionIconSize
              );
              const status = await (
                await app.getById('file_transfer_status_bar')
              ).info();
              expect(status.states.includes('showing')).toBe(!enabled);
              const bottom = await (
                await app.getById('file_transfer_frame_bottom_border')
              ).info();
              expect(bottom.states.includes('showing')).toBe(enabled);
            });
          }
          await expectElementKind(
            await app.getById('settings_cancel_button'),
            'button'
          ).click();
        }
      } finally {
        try {
          await evidence.flushOutputs(apps, launcher);
        } finally {
          await launcher.release();
          await evidence.release();
          await rm(directory, { recursive: true, force: true });
        }
      }
    });
  }
});

describe('file transfer window border', () => {
  for (const type of ['sftp', 'webdav']) {
    it(`respects the initial border setting for ${type}`, async (context) => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-border-'));
      const configPath = join(directory, 'connection.ini');
      const evidence = createTestEvidence(context);
      const apps: GtkApp[] = [];
      const launcher = createGtkAppLauncher({
        appPath: fileURLToPath(
          new URL(
            '../../.build/elder-terms-terminal/elder-terms-file-transfer',
            import.meta.url
          )
        ),
        env: {
          LANGUAGE: 'en',
          LC_ALL: 'C.UTF-8',
          XDG_CONFIG_HOME: join(directory, 'config'),
          XDG_DATA_HOME: join(directory, 'data'),
        },
        xvfbScreen: '1600x1000x24',
        onSystemOutput: evidence.recordSystemOutputEvent,
      });
      try {
        const protocol =
          type === 'sftp'
            ? [
                '[ssh]',
                'address=fixture.example',
                '[sftp]',
                `local_directory=${directory}`,
                'remote_directory=/remote',
              ]
            : [
                '[webdav]',
                'scheme=http',
                'authentication=none',
                'address=fixture.example',
                'base_path=/dav/',
                `local_directory=${directory}`,
                'remote_directory=/',
              ];
        let plainX = 0;
        for (const enabled of [false, true]) {
          await writeFile(
            configPath,
            [
              '[general]',
              `type=${type}`,
              `show_border=${enabled}`,
              'border_width=7',
              ...protocol,
              '',
            ].join('\n')
          );
          const app = await launcher.launch(
            ['--test-fixture', '-c', configPath],
            { onOutput: evidence.recordAppOutputEvent }
          );
          apps.push(app);
          const bounds = (
            await (await app.getById('file_transfer_root_paned')).capture()
          ).bounds;
          if (enabled) {
            expect(bounds.x - plainX).toBe(7);
          } else {
            plainX = bounds.x;
          }
          await app.release();
        }
      } finally {
        try {
          await evidence.flushOutputs(apps, launcher);
        } finally {
          await launcher.release();
          await evidence.release();
          await rm(directory, { recursive: true, force: true });
        }
      }
    });
  }

  it('uses the General border setting and updates its width when applied', async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-border-'));
    const configPath = join(directory, 'connection.ini');
    const evidence = createTestEvidence(context);
    const apps: GtkApp[] = [];
    const launcher = createGtkAppLauncher({
      appPath: fileURLToPath(
        new URL(
          '../../.build/elder-terms-terminal/elder-terms-file-transfer',
          import.meta.url
        )
      ),
      env: {
        LANGUAGE: 'en',
        LC_ALL: 'C.UTF-8',
        XDG_CONFIG_HOME: join(directory, 'config'),
        XDG_DATA_HOME: join(directory, 'data'),
      },
      xvfbScreen: '1600x1000x24',
      onSystemOutput: evidence.recordSystemOutputEvent,
    });
    try {
      const connection = [
        'name=Border FTP',
        'type=ftp',
        '[ftp]',
        'address=fixture.example',
        'username=fixture-user',
        'tls_mode=none',
        `local_directory=${directory}`,
        'remote_directory=/remote',
        '',
      ].join('\n');
      const launchWithBorder = async (enabled: boolean) => {
        await writeFile(
          configPath,
          `[general]\nshow_border=${enabled}\nborder_width=7\nexterior_background=#204060\n${connection}`
        );
        const app = await launcher.launch(
          ['--test-fixture', '-c', configPath],
          {
            onOutput: evidence.recordAppOutputEvent,
          }
        );
        apps.push(app);
        return app;
      };
      const withoutBorder = await launchWithBorder(false);
      const plainPane = await (
        await withoutBorder.getById('file_transfer_root_paned')
      ).capture();
      await withoutBorder.release();

      const withBorder = await launchWithBorder(true);
      await expectElementKind(
        await withBorder.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await withBorder.getById('file_transfer_prompt_message_label'),
            'label'
          ).text()
        ).toBe('Password:');
      });
      await expectElementKind(
        await withBorder.getById('file_transfer_prompt_entry'),
        'entry'
      ).setText('fixture-secret');
      await expectElementKind(
        await withBorder.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await withBorder.getById('file_transfer_status_label'),
            'label'
          ).text()
        ).toBe('Ready');
      });
      const pane = await withBorder.getById('file_transfer_root_paned');
      const borderedPane = await pane.capture();
      expect(borderedPane.bounds.x - plainPane.bounds.x).toBe(7);
      expect(borderedPane.bounds.width).toBe(plainPane.bounds.width - 14);
      expect(
        capturePixel(
          await (
            await withBorder.getById('file_transfer_frame_start_border')
          ).capture(),
          0.5,
          0.5
        )
      ).toEqual([32, 64, 96]);

      await expectElementKind(
        await withBorder.getById('application_menu_button'),
        'toggleButton'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_menu_item'),
        'menuItem'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_general_border_width_entry'),
        'entry'
      ).setText('12');
      await expectElementKind(
        await withBorder.getById('settings_apply_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        const resizedPane = await pane.capture();
        expect(resizedPane.bounds.x - plainPane.bounds.x).toBe(12);
        expect(resizedPane.bounds.width).toBe(plainPane.bounds.width - 24);
      });
      await waitForResult(async () => {
        expect(await withBorder.getWindowCount()).toBe(2);
      });
      await expectElementKind(
        await withBorder.getById('settings_cancel_button'),
        'button'
      ).click();

      await expectElementKind(
        await withBorder.getById('application_menu_button'),
        'toggleButton'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_menu_item'),
        'menuItem'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_general_compact_mode_combo'),
        'comboBox'
      ).selectChildAt(1);
      await expectElementKind(
        await withBorder.getById('settings_apply_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        const bottom = await withBorder.getById(
          'file_transfer_frame_bottom_border'
        );
        expect((await bottom.info()).states).toContain('showing');
        expect((await bottom.capture()).bounds.height).toBe(12);
      });
      await waitForResult(async () => {
        expect(await withBorder.getWindowCount()).toBe(2);
      });
      await expectElementKind(
        await withBorder.getById('settings_cancel_button'),
        'button'
      ).click();

      await expectElementKind(
        await withBorder.getById('application_menu_button'),
        'toggleButton'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_menu_item'),
        'menuItem'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_general_show_border_combo'),
        'comboBox'
      ).selectChildAt(2);
      await expectElementKind(
        await withBorder.getById('settings_cancel_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await withBorder.getWindowCount()).toBe(1);
        expect((await pane.capture()).bounds.x - plainPane.bounds.x).toBe(12);
      });

      await expectElementKind(
        await withBorder.getById('application_menu_button'),
        'toggleButton'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_menu_item'),
        'menuItem'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_general_show_border_combo'),
        'comboBox'
      ).selectChildAt(2);
      await expectElementKind(
        await withBorder.getById('settings_apply_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect((await pane.capture()).bounds.x).toBe(plainPane.bounds.x);
        expect(
          (
            await (
              await withBorder.getById('file_transfer_frame_bottom_border')
            ).info()
          ).states
        ).not.toContain('showing');
      });
      await waitForResult(async () => {
        expect(await withBorder.getWindowCount()).toBe(2);
      });
      await expectElementKind(
        await withBorder.getById('settings_cancel_button'),
        'button'
      ).click();

      await expectElementKind(
        await withBorder.getById('application_menu_button'),
        'toggleButton'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_menu_item'),
        'menuItem'
      ).click();
      await expectElementKind(
        await withBorder.getById('settings_general_show_border_combo'),
        'comboBox'
      ).selectChildAt(1);
      await expectElementKind(
        await withBorder.getById('settings_general_border_width_entry'),
        'entry'
      ).setText('9');
      expect(
        await expectElementKind(
          await withBorder.getById('settings_general_border_width_entry'),
          'entry'
        ).text()
      ).toBe('9');
      expect(
        (await (await withBorder.getById('settings_save_button')).info()).states
      ).toContain('sensitive');
      await expectElementKind(
        await withBorder.getById('settings_save_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await withBorder.getWindowCount()).toBe(1);
      });
      const saved = await readFile(configPath, 'utf8');
      expect(saved).toContain('show_border=true');
      expect(saved).toContain('border_width=9');
      await waitForResult(async () => {
        expect(
          (
            await (
              await withBorder.getById('file_transfer_frame_start_border')
            ).capture()
          ).bounds.width
        ).toBe(9);
        expect((await pane.capture()).bounds.x - plainPane.bounds.x).toBe(9);
        expect(
          (
            await (
              await withBorder.getById('file_transfer_frame_bottom_border')
            ).capture()
          ).bounds.height
        ).toBe(9);
      });
      await withBorder.release();
      const restored = await launcher.launch(
        ['--test-fixture', '-c', configPath],
        { onOutput: evidence.recordAppOutputEvent }
      );
      apps.push(restored);
      expect(
        (await (await restored.getById('file_transfer_root_paned')).capture())
          .bounds.x - plainPane.bounds.x
      ).toBe(9);
    } finally {
      try {
        await evidence.flushOutputs(apps, launcher);
      } finally {
        await launcher.release();
        await evidence.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  });
});
