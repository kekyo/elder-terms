import { mkdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { GtkApp, GtkWidgetElement } from 'gestament';
import { describe, expect, it } from 'vitest';
import { toPass } from 'gestament/testing';
import {
  capturePixel,
  expectCaptureToMatchFixture,
  expectElementKind,
} from './test-helpers';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';

const connectionColorsSshPromptFixturePath = fileURLToPath(
  new URL('./fixtures/connection-colors-ssh-prompt.png', import.meta.url)
);

const expectShowing = async (element: GtkWidgetElement): Promise<void> => {
  await toPass(async () => {
    expect((await element.info()).states).toContain('showing');
  });
};

const expectHidden = async (element: GtkWidgetElement): Promise<void> => {
  await toPass(async () => {
    expect((await element.info()).states).not.toContain('showing');
  });
};

const expectPromptInsideTerminalOverlay = async (
  app: GtkApp,
  panel: GtkWidgetElement
): Promise<void> => {
  const overlay = await app.getById('terminal_overlay');
  const [panelCapture, overlayCapture] = await Promise.all([
    panel.capture(),
    overlay.capture(),
  ]);
  expect(panelCapture.bounds.x).toBeGreaterThanOrEqual(overlayCapture.bounds.x);
  expect(panelCapture.bounds.y).toBeGreaterThanOrEqual(overlayCapture.bounds.y);
  expect(panelCapture.bounds.x + panelCapture.bounds.width).toBeLessThanOrEqual(
    overlayCapture.bounds.x + overlayCapture.bounds.width
  );
  expect(
    panelCapture.bounds.y + panelCapture.bounds.height
  ).toBeLessThanOrEqual(overlayCapture.bounds.y + overlayCapture.bounds.height);
};

const expectOnlyMainWindow = async (app: GtkApp): Promise<void> => {
  await toPass(async () => {
    expect(await app.getWindowCount()).toBe(1);
  });
};

describe.concurrent('SSH prompt overlay', () => {
  it('confirms an unknown host key inside the dimmed terminal surface', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '--test-ssh-prompt=host-key'],
      async (app) => {
        const panel = expectElementKind(
          await app.getById('ssh_prompt_panel'),
          'container'
        );
        const message = expectElementKind(
          await app.getById('ssh_prompt_message_label'),
          'label'
        );
        const entry = expectElementKind(
          await app.getById('ssh_prompt_entry'),
          'entry'
        );
        const dim = await app.getById('terminal_dim_overlay');

        await expectShowing(panel);
        await expectShowing(dim);
        await expectHidden(entry);
        expect(await message.text()).toContain('SHA256:fixture-host-key');
        await expectPromptInsideTerminalOverlay(app, panel);
        await expectOnlyMainWindow(app);

        await expectElementKind(
          await app.getById('ssh_prompt_accept_button'),
          'button'
        ).click();
        await expectHidden(panel);
        await expectShowing(dim);
        await toPass(async () => {
          expect(
            await expectElementKind(
              await app.getById('status_label'),
              'label'
            ).text()
          ).toBe('SSH prompt accepted');
        });
      }
    );
  });

  it('collects a password and restores VTE focus after authentication', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const configPath = join(directory, 'colored-ssh-prompt.ini');
      const gtkConfigHome = join(directory, 'gtk-config');
      const gtkSettingsDirectory = join(gtkConfigHome, 'gtk-3.0');
      await mkdir(gtkSettingsDirectory, { recursive: true });
      await writeFile(
        join(gtkSettingsDirectory, 'settings.ini'),
        '[Settings]\ngtk-cursor-blink=false\n',
        'utf8'
      );
      const background = [0x60, 0x40, 0x20] as const;
      await writeFile(
        configPath,
        [
          '[general]',
          'type=local',
          'background=#604020',
          '',
          '[terminal]',
          'auto_close=false',
          '',
        ].join('\n'),
        'utf8'
      );

      await runGtkTest(
        context,
        ['--test-fixture', '--test-ssh-prompt=password', '-c', configPath],
        async (app, evidence) => {
          const panel = expectElementKind(
            await app.getById('ssh_prompt_panel'),
            'container'
          );
          const entry = expectElementKind(
            await app.getById('ssh_prompt_entry'),
            'entry'
          );
          const dim = await app.getById('terminal_dim_overlay');
          const terminal = await app.getById('terminal_view');

          await expectShowing(panel);
          await expectShowing(entry);
          await expectShowing(dim);
          await expectOnlyMainWindow(app);
          for (const [widgetId, horizontalRatio] of [
            ['ssh_prompt_panel', 0.05],
            ['ssh_prompt_background', 0.05],
            ['ssh_prompt_title_label', 0.95],
            ['ssh_prompt_message_label', 0.95],
            ['ssh_prompt_entry', 0.1],
            ['ssh_prompt_actions', 0.05],
            ['ssh_prompt_cancel_button', 0.15],
            ['ssh_prompt_accept_button', 0.15],
          ] as const) {
            expect(
              capturePixel(
                await (await app.getById(widgetId)).capture(),
                horizontalRatio,
                0.5
              )
            ).toEqual(background);
          }
          const promptCapture = await evidence.captureEvidence(
            'connection-colors-ssh-prompt',
            async () => panel.capture()
          );
          await expectCaptureToMatchFixture(
            promptCapture,
            'connection-colors-ssh-prompt',
            connectionColorsSshPromptFixturePath,
            evidence
          );

          await entry.setText('fixture-secret');
          await expectElementKind(
            await app.getById('ssh_prompt_accept_button'),
            'button'
          ).click();

          await expectHidden(panel);
          await expectHidden(dim);
          await toPass(async () => {
            expect(
              await expectElementKind(
                await app.getById('status_label'),
                'label'
              ).text()
            ).toBe('SSH prompt accepted');
          });
          await toPass(async () => {
            expect((await terminal.info()).states).toContain('focused');
          });
        },
        {
          env: {
            XDG_CONFIG_HOME: gtkConfigHome,
          },
        }
      );
    });
  });
});
