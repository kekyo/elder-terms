import type { GtkApp, GtkWidgetElement } from 'gestament';
import { describe, expect, it } from 'vitest';
import { toPass } from 'gestament/testing';
import { expectElementKind } from './test-helpers';
import { runGtkTest } from './gtk-test-helpers';

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

  it('collects a password in the same overlay and keeps VTE read-only afterward', async (context) => {
    await runGtkTest(
      context,
      ['--test-fixture', '--test-ssh-prompt=password'],
      async (app) => {
        const panel = expectElementKind(
          await app.getById('ssh_prompt_panel'),
          'container'
        );
        const entry = expectElementKind(
          await app.getById('ssh_prompt_entry'),
          'entry'
        );
        const dim = await app.getById('terminal_dim_overlay');

        await expectShowing(panel);
        await expectShowing(entry);
        await expectShowing(dim);
        await expectOnlyMainWindow(app);
        await entry.setText('fixture-secret');
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
});
