import type { GtkApp, GtkToggleButtonElement } from 'gestament';
import { expect } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind } from './test-helpers';
import {
  activateTransferCancel,
  expectTransferProgressNoticeHidden,
} from './gtk-test-helpers';

/**
 * Activates the Text (Send) transfer menu item.
 *
 * @param app Running terminal application.
 * @returns Transfer menu button used by the operation.
 */
export const activateTextSend = async (
  app: GtkApp
): Promise<GtkToggleButtonElement> => {
  const button = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  await waitForResult(async () => {
    const info = await button.info();
    expect(info.states).toContain('sensitive');
  });
  await button.click();
  const item = await waitForResult(async () => {
    const menuItem = expectElementKind(
      await app.getById('transfer_text_send_item'),
      'menuItem'
    );
    const info = await menuItem.info();
    expect(info.name).toBe('Text (Send)');
    expect(info.states).toContain('showing');
    return menuItem;
  });
  await item.click();
  return button;
};

/**
 * Waits for a text send operation to enter its read-only presentation.
 *
 * @param button Transfer menu button disabled during the operation.
 */
export const expectTextSendActive = async (
  button: GtkToggleButtonElement
): Promise<void> => {
  await waitForResult(async () => {
    const info = await button.info();
    expect(info.states).not.toContain('sensitive');
  });
};

/**
 * Waits for a text send operation to finish successfully.
 *
 * @param app Running terminal application.
 * @param button Transfer menu button restored after the operation.
 */
export const expectTextSendFinished = async (
  app: GtkApp,
  button: GtkToggleButtonElement
): Promise<void> => {
  await waitForResult(async () => {
    const info = await button.info();
    const status = expectElementKind(
      await app.getById('status_label'),
      'label'
    );
    expect(info.states).toContain('sensitive');
    expect(await status.text()).toBe('Terminal');
  });
};

/**
 * Cancels an active text send and waits for the terminal presentation to
 * become available again.
 *
 * @param app Running terminal application.
 * @param button Transfer menu button restored after cancellation.
 */
export const cancelTextSend = async (
  app: GtkApp,
  button: GtkToggleButtonElement
): Promise<void> => {
  await activateTransferCancel(app);
  await expectTransferProgressNoticeHidden(app);
  await waitForResult(async () => {
    const info = await button.info();
    const status = expectElementKind(
      await app.getById('status_label'),
      'label'
    );
    expect(info.states).toContain('sensitive');
    expect(await status.text()).toBe('Text send failed');
  });
};
