import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { once } from 'node:events';
import { fileURLToPath } from 'node:url';
import type { GtkApp, GtkToggleButtonElement } from 'gestament';
import { expect } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind } from './test-helpers';

const clipboardWriteHelperPath = fileURLToPath(
  new URL(
    '../../.build/elder-terms-vte/clipboard-write-helper',
    import.meta.url
  )
);

/**
 * Running test clipboard provider that owns one UTF-8 text value.
 */
export interface ClipboardTextProvider {
  /** Stops the provider and releases clipboard ownership. */
  readonly close: () => Promise<void>;
}

/** X11 text selection owned by a clipboard test provider. */
export type ClipboardTextSelection = 'clipboard' | 'primary';

const closeClipboardTextProvider = async (
  child: ChildProcessWithoutNullStreams
): Promise<void> => {
  if (child.exitCode !== null || child.signalCode !== null) {
    return;
  }

  child.stdin.end('\n');
  const timeout = setTimeout(() => {
    child.kill('SIGKILL');
  }, 2_000);
  try {
    await once(child, 'exit');
  } finally {
    clearTimeout(timeout);
  }
};

/**
 * Starts a helper that provides UTF-8 text on the app's default clipboard.
 *
 * @param app Running GTK app whose display owns the clipboard.
 * @param text UTF-8 clipboard text to provide.
 * @param selection Clipboard or primary selection to own.
 * @returns Provider handle after clipboard ownership is ready.
 */
export const startClipboardTextProvider = async (
  app: GtkApp,
  text: string,
  selection: ClipboardTextSelection = 'clipboard'
): Promise<ClipboardTextProvider> => {
  const arguments_ = selection === 'primary' ? ['--primary', text] : [text];
  const child = spawn(clipboardWriteHelperPath, arguments_, {
    env: await app.environment(),
    stdio: ['pipe', 'pipe', 'pipe'],
  });

  return new Promise<ClipboardTextProvider>((resolve, reject) => {
    let resolved = false;
    let stdout = '';
    let stderr = '';
    const timeout = setTimeout(() => {
      if (!resolved) {
        child.kill('SIGKILL');
        reject(new Error(`Clipboard provider did not become ready: ${stderr}`));
      }
    }, 5_000);

    const rejectIfPending = (error: Error): void => {
      if (!resolved) {
        clearTimeout(timeout);
        reject(error);
      }
    };

    child.stderr.on('data', (chunk: Buffer) => {
      stderr += chunk.toString('utf8');
    });
    child.stdout.on('data', (chunk: Buffer) => {
      stdout += chunk.toString('utf8');
      if (!resolved && stdout.split(/\r?\n/u).includes('READY')) {
        resolved = true;
        clearTimeout(timeout);
        resolve({
          close: async (): Promise<void> => {
            await closeClipboardTextProvider(child);
          },
        });
      }
    });
    child.once('error', rejectIfPending);
    child.once('exit', (code, signal) => {
      rejectIfPending(
        new Error(
          `Clipboard provider exited before ready: code=${code} signal=${signal} stderr=${stderr}`
        )
      );
    });
  });
};

/**
 * Opens the terminal context menu at the center of the VTE widget.
 *
 * @param app Running GTK app.
 */
export const openTerminalContextMenu = async (app: GtkApp): Promise<void> => {
  const terminal = await app.getById('terminal_view');
  const capture = await terminal.capture();
  await app.input.moveMouseTo(
    Math.trunc(capture.bounds.x + capture.bounds.width / 2),
    Math.trunc(capture.bounds.y + capture.bounds.height / 2)
  );
  await app.input.setMouseButton('right', true);
  await app.input.setMouseButton('right', false);
};

/**
 * Activates the enabled Paste item from the terminal context menu.
 *
 * @param app Running GTK app.
 * @returns Transfer menu button used to observe text-send activity.
 */
export const activateTerminalPaste = async (
  app: GtkApp
): Promise<GtkToggleButtonElement> => {
  const transferButton = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  await openTerminalContextMenu(app);
  const pasteItem = await waitForResult(async () => {
    const item = expectElementKind(
      await app.getById('terminal_context_paste_item'),
      'menuItem'
    );
    const info = await item.info();
    expect(info.name).toBe('Paste');
    expect(info.states).toContain('showing');
    expect(info.states).toContain('enabled');
    expect(info.states).toContain('sensitive');
    return item;
  });
  await pasteItem.click();
  return transferButton;
};
