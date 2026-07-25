import { createRequire } from 'node:module';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { expect, type TestContext } from 'vitest';
import {
  createGtkAppLauncher,
  type GtkButtonElement,
  type GtkApp,
  type GtkAppOutputEvent,
  type GtkAppLauncherOptions,
  type GtkCapture,
  type GtkKeyboardModifier,
  type GtkKeyInput,
  type GtkWidgetElement,
  type GtkWindowElement,
  type GtkWindowResizeHints,
} from 'gestament';
import { toPass, waitForResult } from 'gestament/testing';
import type { PNG as PngImage } from 'pngjs';
import {
  createTestEvidence,
  expectElementKind,
  type TestEvidence,
} from './test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

const appPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/elder-terms-vte', import.meta.url)
);

/** Test-only synchronous XYZMODEM peer used by transfer progress notice tests. */
export const xyzmPausePeerPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/xyzm-pause-peer', import.meta.url)
);

/** Expected terminal columns for the default app launch. */
export const defaultColumns = 80;

/** Expected terminal rows for the default app launch. */
export const defaultRows = 24;

/** Number of Ctrl+wheel steps used for constrained zoom tests. */
export const constrainedFontZoomSteps = 8;

/** Number of Ctrl+wheel steps used for burst zoom tests. */
export const rapidFontZoomBurstSteps = 8;

/** Expected 80x24 terminal fixture PNG. */
export const terminalTextGrid80x24Path = fileURLToPath(
  new URL('./fixtures/terminal-text-grid-80x24.png', import.meta.url)
);

/** Expected 81x25 terminal fixture PNG. */
export const terminalTextGrid81x25Path = fileURLToPath(
  new URL('./fixtures/terminal-text-grid-81x25.png', import.meta.url)
);

/** Expected 80x24 terminal fixture PNG with 1.1 font scale. */
export const terminalTextGrid80x24FontScale11Path = fileURLToPath(
  new URL(
    './fixtures/terminal-text-grid-80x24-font-scale-1.1.png',
    import.meta.url
  )
);

/** Expected disconnected notice PNG. */
export const disconnectedNoticePath = fileURLToPath(
  new URL('./fixtures/disconnected-notice.png', import.meta.url)
);

/** Expected disconnected notice PNG rendered on the terminal surface. */
export const disconnectedNoticeOnTerminalPath = fileURLToPath(
  new URL('./fixtures/disconnected-notice-on-terminal.png', import.meta.url)
);

/** Expected dimmed local terminal PNG after disconnection. */
export const localTerminalDisconnectedDimPath = fileURLToPath(
  new URL('./fixtures/local-terminal-disconnected-dim.png', import.meta.url)
);

/** Expected dimmed terminal PNG while a transfer is active. */
export const transferTerminalDimPath = fileURLToPath(
  new URL('./fixtures/transfer-terminal-dim.png', import.meta.url)
);

/** Expected transfer progress notice PNG for XMODEM receive. */
export const transferProgressNoticeXmodemReceivePath = fileURLToPath(
  new URL(
    './fixtures/transfer-progress-notice-xmodem-receive.png',
    import.meta.url
  )
);

/** Expected transfer progress notice PNG for XMODEM send. */
export const transferProgressNoticeXmodemSendPath = fileURLToPath(
  new URL(
    './fixtures/transfer-progress-notice-xmodem-send.png',
    import.meta.url
  )
);

/** Expected transfer progress notice PNG for YMODEM receive. */
export const transferProgressNoticeYmodemReceivePath = fileURLToPath(
  new URL(
    './fixtures/transfer-progress-notice-ymodem-receive.png',
    import.meta.url
  )
);

/** Expected transfer progress notice PNG for YMODEM send. */
export const transferProgressNoticeYmodemSendPath = fileURLToPath(
  new URL(
    './fixtures/transfer-progress-notice-ymodem-send.png',
    import.meta.url
  )
);

/** Expected transfer progress notice PNG for ZMODEM receive. */
export const transferProgressNoticeZmodemReceivePath = fileURLToPath(
  new URL(
    './fixtures/transfer-progress-notice-zmodem-receive.png',
    import.meta.url
  )
);

/** Expected transfer progress notice PNG for ZMODEM send. */
export const transferProgressNoticeZmodemSendPath = fileURLToPath(
  new URL(
    './fixtures/transfer-progress-notice-zmodem-send.png',
    import.meta.url
  )
);

/** INI fixture that configures the terminal grid to 81x25. */
export const terminalGrid81x25ConfigPath = fileURLToPath(
  new URL('./fixtures/terminal-grid-81x25.ini', import.meta.url)
);

/** INI fixture that configures the terminal zoom to 1.1. */
export const terminalZoom11ConfigPath = fileURLToPath(
  new URL('./fixtures/terminal-zoom-1.1.ini', import.meta.url)
);

/** INI fixture that contains invalid terminal values. */
export const terminalInvalidValuesConfigPath = fileURLToPath(
  new URL('./fixtures/terminal-invalid-values.ini', import.meta.url)
);

/** INI fixture that configures TELNET without the required address. */
export const telnetMissingAddressConfigPath = fileURLToPath(
  new URL('./fixtures/telnet-missing-address.ini', import.meta.url)
);

/** INI fixture template that configures TELNET to localhost. */
export const telnetLocalhostConfigPath = fileURLToPath(
  new URL('./fixtures/telnet-localhost.ini', import.meta.url)
);

/**
 * Captured geometry and widget handles for terminal grid assertions.
 */
export interface TerminalGridLayout {
  /** Window geometry hints. */
  readonly hints: GtkWindowResizeHints;
  /** Main window element. */
  readonly mainWindow: GtkWindowElement;
  /** Main window bounds. */
  readonly mainBounds: {
    readonly height: number;
    readonly width: number;
    readonly x: number;
    readonly y: number;
  };
  /** Captured status bar image. */
  readonly statusBarCapture: GtkCapture;
  /** Terminal widget element. */
  readonly terminal: GtkWidgetElement;
  /** Captured terminal image. */
  readonly terminalCapture: GtkCapture;
  /** Captured terminal scroller image. */
  readonly terminalScrollerCapture: GtkCapture;
  /** Captured terminal scrollbar image. */
  readonly terminalScrollbarCapture: GtkCapture;
}

/**
 * Captured geometry and widget handle for window cell sizing assertions.
 */
export interface WindowCellLayout {
  /** Window geometry hints. */
  readonly hints: GtkWindowResizeHints;
  /** Main window bounds. */
  readonly mainBounds: {
    readonly height: number;
    readonly width: number;
    readonly x: number;
    readonly y: number;
  };
  /** Main window element. */
  readonly mainWindow: GtkWindowElement;
}

/**
 * Terminal grid size reported by the fixture status label.
 */
export interface TerminalGridSize {
  /** VTE column count. */
  readonly columns: number;
  /** VTE row count. */
  readonly rows: number;
}

/**
 * Current transfer progress bar value normalized from AT-SPI metadata.
 */
export interface TransferProgressBarValue {
  /** Raw current value returned by value(). */
  readonly rawValue: number;
  /** Raw current value returned by valueInfo(). */
  readonly infoValue: number;
  /** Minimum value reported by AT-SPI. */
  readonly minimum: number;
  /** Maximum value reported by AT-SPI. */
  readonly maximum: number;
  /** Current value normalized to 0.0 through 1.0. */
  readonly normalized: number;
}

/**
 * Launch options forwarded by runGtkTest.
 */
export interface RunGtkTestOptions {
  /** Environment overrides, including the isolated XDG config home. */
  readonly env?: GtkAppLauncherOptions['env'];
  /** Additional output callback invoked for launched GTK app stdout/stderr. */
  readonly onOutput?: (event: GtkAppOutputEvent) => void;
}

const readPng = (capture: GtkCapture): PngImage => PNG.sync.read(capture.image);

const cropPng = (
  image: PngImage,
  rect: {
    readonly height: number;
    readonly width: number;
    readonly x: number;
    readonly y: number;
  }
): PngImage => {
  const startX = Math.max(0, Math.floor(rect.x));
  const startY = Math.max(0, Math.floor(rect.y));
  const width = Math.min(image.width - startX, Math.ceil(rect.width));
  const height = Math.min(image.height - startY, Math.ceil(rect.height));
  const cropped = new PNG({ width, height }) as PngImage;

  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const sourceOffset = ((startY + y) * image.width + startX + x) * 4;
      const targetOffset = (y * width + x) * 4;
      image.data.copy(
        cropped.data,
        targetOffset,
        sourceOffset,
        sourceOffset + 4
      );
    }
  }

  return cropped;
};

/**
 * Creates and deletes a temporary directory for a test body.
 *
 * @param body Test body receiving the directory path.
 * @returns Promise resolved after cleanup.
 */
export const withTemporaryDirectory = async (
  body: (directory: string) => Promise<void>
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-vte-config-'));
  try {
    await body(directory);
  } finally {
    await rm(directory, { force: true, recursive: true });
  }
};

/**
 * Asserts the X11 title of the main application window.
 *
 * @param app Running GTK app.
 * @param expectedTitle Expected window title.
 */
export const expectMainWindowTitle = async (
  app: GtkApp,
  expectedTitle: string
): Promise<void> => {
  const mainWindow = expectElementKind(
    await app.getById('main_window'),
    'window'
  );
  await toPass(
    async () => {
      expect((await mainWindow.x11Info()).title).toBe(expectedTitle);
    },
    {
      message: `main window title should be ${expectedTitle}`,
      timeoutMs: 5_000,
    }
  );
};

/**
 * Asserts the terminal-surface disconnected notice is hidden.
 *
 * @param app Running GTK app.
 */
export const expectDisconnectedNoticeHidden = async (
  app: GtkApp
): Promise<void> => {
  const notice = await app.getById('disconnected_notice');
  await waitForResult(
    async () => {
      const info = await notice.info();
      expect(info.states).not.toContain('showing');
      expect(info.states).not.toContain('visible');
    },
    {
      message: 'disconnected notice should be hidden',
      timeoutMs: 5_000,
    }
  );
};

/**
 * Asserts the disconnected notice is visible at the terminal top-right edge.
 *
 * @param app Running GTK app.
 */
export const expectDisconnectedNoticeVisibleAtTerminalTopRight = async (
  app: GtkApp
): Promise<void> => {
  const terminal = await app.getById('terminal_view');
  const notice = await app.getById('disconnected_notice');
  const label = expectElementKind(
    await app.getById('disconnected_notice_label'),
    'label'
  );

  expect(await label.text()).toBe('Disconnected');

  const { noticeCapture, terminalCapture } = await waitForResult(
    async () => {
      const info = await notice.info();
      expect(info.states).toContain('showing');
      expect(info.states).toContain('visible');
      return {
        noticeCapture: await notice.capture(),
        terminalCapture: await terminal.capture(),
      };
    },
    {
      message: 'disconnected notice should be visible',
      timeoutMs: 5_000,
    }
  );

  const terminalRight = terminalCapture.bounds.x + terminalCapture.bounds.width;
  const noticeRight = noticeCapture.bounds.x + noticeCapture.bounds.width;
  const rightGap = terminalRight - noticeRight;
  const topGap = noticeCapture.bounds.y - terminalCapture.bounds.y;

  expect(noticeCapture.bounds.x).toBeGreaterThanOrEqual(
    terminalCapture.bounds.x
  );
  expect(noticeCapture.bounds.y).toBeGreaterThanOrEqual(
    terminalCapture.bounds.y
  );
  expect(noticeRight).toBeLessThanOrEqual(terminalRight);
  expect(
    noticeCapture.bounds.y + noticeCapture.bounds.height
  ).toBeLessThanOrEqual(
    terminalCapture.bounds.y + terminalCapture.bounds.height
  );
  expect(rightGap).toBeGreaterThanOrEqual(0);
  expect(rightGap).toBeLessThanOrEqual(16);
  expect(topGap).toBeLessThanOrEqual(16);
};

/**
 * Asserts the terminal-surface transfer progress notice is hidden.
 *
 * @param app Running GTK app.
 */
export const expectTransferProgressNoticeHidden = async (
  app: GtkApp
): Promise<void> => {
  const notice = await app.getById('transfer_progress_notice');
  await waitForResult(
    async () => {
      const info = await notice.info();
      expect(info.states).not.toContain('showing');
      expect(info.states).not.toContain('visible');
    },
    {
      message: 'transfer progress notice should be hidden',
      timeoutMs: 5_000,
    }
  );
};

/**
 * Asserts the transfer progress notice is visible at the terminal top-right edge.
 *
 * @param app Running GTK app.
 */
export const expectTransferProgressNoticeVisibleAtTerminalTopRight = async (
  app: GtkApp
): Promise<void> => {
  const terminal = await app.getById('terminal_view');
  const notice = await app.getById('transfer_progress_notice');
  const label = expectElementKind(
    await app.getById('transfer_progress_notice_label'),
    'label'
  );
  expectElementKind(await app.getById('transfer_progress_bar'), 'progressBar');

  expect(await label.text()).toBe('Transferring...');

  const { noticeCapture, terminalCapture } = await waitForResult(
    async () => {
      const noticeInfo = await notice.info();
      expect(noticeInfo.states).toContain('showing');
      expect(noticeInfo.states).toContain('visible');
      return {
        noticeCapture: await notice.capture(),
        terminalCapture: await terminal.capture(),
      };
    },
    {
      message: 'transfer progress notice should be visible',
      timeoutMs: 5_000,
    }
  );

  const terminalRight = terminalCapture.bounds.x + terminalCapture.bounds.width;
  const noticeRight = noticeCapture.bounds.x + noticeCapture.bounds.width;
  const rightGap = terminalRight - noticeRight;
  const topGap = noticeCapture.bounds.y - terminalCapture.bounds.y;

  expect(noticeCapture.bounds.x).toBeGreaterThanOrEqual(
    terminalCapture.bounds.x
  );
  expect(noticeCapture.bounds.y).toBeGreaterThanOrEqual(
    terminalCapture.bounds.y
  );
  expect(noticeRight).toBeLessThanOrEqual(terminalRight);
  expect(
    noticeCapture.bounds.y + noticeCapture.bounds.height
  ).toBeLessThanOrEqual(
    terminalCapture.bounds.y + terminalCapture.bounds.height
  );
  expect(rightGap).toBeGreaterThanOrEqual(0);
  expect(rightGap).toBeLessThanOrEqual(16);
  expect(topGap).toBeLessThanOrEqual(16);
};

/**
 * Waits for the cancel action exposed by the visible transfer overlay.
 *
 * @param app Running GTK app.
 * @returns Visible and actionable cancel button.
 */
export const expectTransferCancelVisible = async (
  app: GtkApp
): Promise<GtkButtonElement> => {
  const cancelButton = expectElementKind(
    await app.getById('transfer_cancel_button'),
    'button'
  );
  await waitForResult(
    async () => {
      const info = await cancelButton.info();
      expect(info.name).toBe('Cancel');
      expect(info.states).toContain('sensitive');
      expect(info.states).toContain('showing');
    },
    {
      message: 'transfer cancel button should be actionable',
      timeoutMs: 5_000,
    }
  );
  return cancelButton;
};

/**
 * Activates the cancel action exposed by the visible transfer overlay.
 *
 * @param app Running GTK app.
 */
export const activateTransferCancel = async (app: GtkApp): Promise<void> => {
  const cancelButton = await expectTransferCancelVisible(app);
  await cancelButton.click();
};

/**
 * Reads the transfer progress bar value normalized to 0.0 through 1.0.
 *
 * @param app Running GTK app.
 * @returns Progress bar value details.
 */
export const readTransferProgressBarValue = async (
  app: GtkApp
): Promise<TransferProgressBarValue> => {
  const progress = expectElementKind(
    await app.getById('transfer_progress_bar'),
    'progressBar'
  );
  const rawValue = await progress.value();
  const info = await progress.valueInfo();
  const range = info.maximum - info.minimum;
  const normalized =
    range <= 0
      ? 0
      : Math.min(1, Math.max(0, (info.value - info.minimum) / range));

  return {
    rawValue,
    infoValue: info.value,
    minimum: info.minimum,
    maximum: info.maximum,
    normalized,
  };
};

/**
 * Asserts the transfer progress notice visual matches its fixture image.
 *
 * @param app Running GTK app.
 * @param evidence Test evidence writer.
 * @param name Evidence and comparison name.
 * @param masterImagePath Expected PNG path.
 * @param options Pixel comparison options.
 */
export const assertTransferProgressNoticeMatches = async (
  app: GtkApp,
  evidence: TestEvidence,
  name: string,
  masterImagePath: string,
  options?: Parameters<TestEvidence['expectCaptureToLookSimilar']>[3]
): Promise<void> => {
  await expectTransferProgressNoticeVisibleAtTerminalTopRight(app);

  const notice = await app.getById('transfer_progress_notice');
  const capture = await evidence.captureEvidence(name, async () =>
    notice.capture()
  );
  await evidence.expectCaptureToLookSimilar(
    capture,
    name,
    masterImagePath,
    options
  );
};

/**
 * Asserts the disconnected notice is rendered above terminal dimming.
 *
 * @param app Running GTK app.
 */
export const expectDisconnectedNoticeRenderedUndimmedAtTerminalTopRight =
  async (app: GtkApp, evidence: TestEvidence): Promise<void> => {
    await expectDisconnectedNoticeVisibleAtTerminalTopRight(app);

    const terminalOverlay = await app.getById('terminal_overlay');
    const notice = await app.getById('disconnected_notice');
    const { noticeCapture, terminalOverlayCapture } = await waitForResult(
      async () => {
        const info = await notice.info();
        expect(info.states).toContain('showing');
        expect(info.states).toContain('visible');
        return {
          noticeCapture: await notice.capture(),
          terminalOverlayCapture: await terminalOverlay.capture(),
        };
      },
      {
        message: 'disconnected notice should be rendered on terminal surface',
        timeoutMs: 5_000,
      }
    );

    const renderedNoticePng = cropPng(readPng(terminalOverlayCapture), {
      x: noticeCapture.bounds.x - terminalOverlayCapture.bounds.x,
      y: noticeCapture.bounds.y - terminalOverlayCapture.bounds.y,
      width: noticeCapture.bounds.width,
      height: noticeCapture.bounds.height,
    });
    const renderedNoticeCapture: GtkCapture = {
      ...noticeCapture,
      image: PNG.sync.write(renderedNoticePng),
    };
    const savedCapture = await evidence.captureEvidence(
      'disconnected-notice-rendered-on-terminal',
      async () => renderedNoticeCapture
    );

    await evidence.expectCaptureToLookSimilar(
      savedCapture,
      'disconnected-notice-rendered-on-terminal',
      disconnectedNoticeOnTerminalPath,
      {
        maxDiffPixels: 0,
        threshold: 0.01,
      }
    );
  };

/**
 * Asserts the disconnected notice visual matches its fixture image.
 *
 * @param app Running GTK app.
 * @param evidence Test evidence writer.
 */
export const assertDisconnectedNoticeMatches = async (
  app: GtkApp,
  evidence: TestEvidence
): Promise<void> => {
  await expectDisconnectedNoticeVisibleAtTerminalTopRight(app);

  const notice = await app.getById('disconnected_notice');
  const capture = await evidence.captureEvidence(
    'disconnected-notice',
    async () => notice.capture()
  );
  await evidence.expectCaptureToLookSimilar(
    capture,
    'disconnected-notice',
    disconnectedNoticePath,
    {
      maxDiffPixels: 0,
      threshold: 0.01,
    }
  );
};

/**
 * Launches the GTK app and records evidence for one Vitest test.
 *
 * @remarks A temporary XDG config home isolates each launch unless overridden
 * through `options.env`.
 *
 * @param context Vitest test context.
 * @param args Application arguments.
 * @param body Test body receiving the app and evidence writer.
 * @param options Additional app launch options.
 * @returns Promise resolved after app and evidence cleanup.
 */
export const runGtkTest = async (
  context: TestContext,
  args: readonly string[],
  body: (app: GtkApp, evidence: TestEvidence) => Promise<void>,
  options?: RunGtkTestOptions
): Promise<void> => {
  const configHome = await mkdtemp(
    join(tmpdir(), 'elder-terms-vte-xdg-config-')
  );
  try {
    const evidence = createTestEvidence(context);
    const launcher = createGtkAppLauncher({
      appPath,
      env: {
        XDG_CONFIG_HOME: configHome,
        ...options?.env,
      },
      onSystemOutput: evidence.recordSystemOutputEvent,
      xvfbPool: {
        maxIdlePerKey: 8,
        maxIdleTotal: 8,
        type: 'xvfb',
      },
      xvfbTrayHost: true,
    });
    const apps: GtkApp[] = [];

    try {
      await evidence.log('launching app', { appPath, args });
      const app = await launcher.launch(args, {
        onOutput: (event) => {
          evidence.recordAppOutputEvent(event);
          options?.onOutput?.(event);
        },
      });
      apps.push(app);
      await body(app, evidence);
    } catch (error) {
      await evidence.log('test error', { error });
      throw error;
    } finally {
      try {
        await evidence.flushOutputs(apps, launcher);
      } finally {
        try {
          await launcher.release();
        } finally {
          await evidence.flushOutputs([], launcher);
          await evidence.release();
        }
      }
    }
  } finally {
    await rm(configHome, { force: true, recursive: true });
  }
};

const readPngFile = async (path: string): Promise<PngImage> =>
  PNG.sync.read(await readFile(path));

/**
 * Asserts that a terminal capture matches an expected fixture image.
 *
 * @param terminal Terminal widget to capture.
 * @param name Evidence and comparison name.
 * @param masterImagePath Expected PNG path.
 * @param evidence Test evidence writer.
 * @param options Pixel comparison options.
 * @returns Promise resolved after the comparison passes.
 */
export const assertTerminalCaptureMatches = async (
  terminal: GtkWidgetElement,
  name: string,
  masterImagePath: string,
  evidence: TestEvidence,
  options?: Parameters<TestEvidence['expectCaptureToLookSimilar']>[3]
): Promise<void> => {
  const capture = await waitForResult(async () => {
    const currentCapture = await terminal.capture();
    expect(currentCapture.clipped).toBe(false);
    return currentCapture;
  });
  const savedCapture = await evidence.captureEvidence(
    name,
    async () => capture
  );
  const expectedPng = await readPngFile(masterImagePath);
  const savedPng = readPng(savedCapture);

  expect(savedCapture.clipped).toBe(false);
  expect(savedPng.width).toBe(expectedPng.width);
  expect(savedPng.height).toBe(expectedPng.height);

  await evidence.expectCaptureToLookSimilar(
    savedCapture,
    name,
    masterImagePath,
    {
      maxDiffPixels: 0,
      threshold: 0.01,
      ...options,
    }
  );
};

/**
 * Asserts that a terminal text-grid capture exactly matches its fixture image.
 *
 * @param terminal Terminal widget to capture.
 * @param name Evidence and comparison name.
 * @param masterImagePath Expected PNG path.
 * @param evidence Test evidence writer.
 * @returns Promise resolved after the comparison passes.
 */
export const assertTerminalTextGridMatches = async (
  terminal: GtkWidgetElement,
  name: string,
  masterImagePath: string,
  evidence: TestEvidence
): Promise<void> => {
  await assertTerminalCaptureMatches(terminal, name, masterImagePath, evidence);
};

/**
 * Asserts that a window size maps to the expected VTE cell count.
 *
 * @param layout Layout with window bounds and resize hints.
 * @param columns Expected columns.
 * @param rows Expected rows.
 */
export const expectWindowCellSize = (
  layout: TerminalGridLayout | WindowCellLayout,
  columns: number,
  rows: number
): void => {
  expect(
    (layout.mainBounds.width - layout.hints.baseWidth) /
      layout.hints.widthIncrement
  ).toBe(columns);
  expect(
    (layout.mainBounds.height - layout.hints.baseHeight) /
      layout.hints.heightIncrement
  ).toBe(rows);
};

/**
 * Reads the terminal grid size exposed by the fixture status label.
 *
 * @param app Running GTK app.
 * @returns Parsed terminal grid size.
 */
export const readFixtureVteGridSize = async (
  app: GtkApp
): Promise<TerminalGridSize> => {
  const statusLabel = expectElementKind(
    await app.getById('status_label'),
    'label'
  );
  const text = await statusLabel.text();
  const match = /^([0-9]+)x([0-9]+)$/.exec(text);
  expect(match).not.toBeNull();

  return {
    columns: Number(match?.[1]),
    rows: Number(match?.[2]),
  };
};

/**
 * Asserts the terminal grid size exposed by the fixture status label.
 *
 * @param app Running GTK app.
 * @param columns Expected columns.
 * @param rows Expected rows.
 * @returns Parsed terminal grid size.
 */
export const expectFixtureVteGridSize = async (
  app: GtkApp,
  columns: number,
  rows: number
): Promise<TerminalGridSize> => {
  const gridSize = await readFixtureVteGridSize(app);
  expect(gridSize).toStrictEqual({ columns, rows });
  return gridSize;
};

/**
 * Reads main window geometry and resize hints.
 *
 * @param app Running GTK app.
 * @returns Window cell layout.
 */
export const readWindowCellLayout = async (
  app: GtkApp
): Promise<WindowCellLayout> => {
  const mainWindow = expectElementKind(
    await app.getById('main_window'),
    'window'
  );
  const [hints, mainBounds] = await Promise.all([
    mainWindow.resizeHints(),
    mainWindow.bounds(),
  ]);

  expect(hints.widthIncrement).toBeGreaterThan(0);
  expect(hints.heightIncrement).toBeGreaterThan(0);
  expect(hints.baseWidth).toBeGreaterThan(0);
  expect(hints.baseHeight).toBeGreaterThan(0);
  expect((mainBounds.width - hints.baseWidth) % hints.widthIncrement).toBe(0);
  expect((mainBounds.height - hints.baseHeight) % hints.heightIncrement).toBe(
    0
  );

  return {
    hints,
    mainBounds,
    mainWindow,
  };
};

/**
 * Reads window, terminal, scrollbar, and status bar layout state.
 *
 * @param app Running GTK app.
 * @returns Terminal grid layout.
 */
export const readTerminalGridLayout = async (
  app: GtkApp
): Promise<TerminalGridLayout> => {
  const mainWindow = expectElementKind(
    await app.getById('main_window'),
    'window'
  );
  const terminalScroller = await app.getById('terminal_scroller');
  const terminal = await app.getById('terminal_view');
  const terminalScrollbar = await app.getById('terminal_scrollbar');
  const statusBar = await app.getById('status_bar');

  const [
    hints,
    mainBounds,
    terminalScrollerCapture,
    terminalCapture,
    terminalScrollbarCapture,
    statusBarCapture,
  ] = await Promise.all([
    mainWindow.resizeHints(),
    mainWindow.bounds(),
    terminalScroller.capture(),
    terminal.capture(),
    terminalScrollbar.capture(),
    statusBar.capture(),
  ]);

  expect(hints.widthIncrement).toBeGreaterThan(0);
  expect(hints.heightIncrement).toBeGreaterThan(0);
  expect(hints.baseWidth).toBeGreaterThan(0);
  expect(hints.baseHeight).toBeGreaterThan(0);
  expect(hints.minWidth).toBeGreaterThanOrEqual(
    hints.baseWidth + hints.widthIncrement * 4
  );
  expect(hints.minHeight).toBeGreaterThanOrEqual(
    hints.baseHeight + hints.heightIncrement
  );
  expect((mainBounds.width - hints.baseWidth) % hints.widthIncrement).toBe(0);
  expect((mainBounds.height - hints.baseHeight) % hints.heightIncrement).toBe(
    0
  );

  expect(terminalCapture.bounds.x).toBe(terminalScrollerCapture.bounds.x);
  expect(terminalCapture.bounds.y).toBe(terminalScrollerCapture.bounds.y);
  expect(terminalScrollbarCapture.bounds.y).toBe(
    terminalScrollerCapture.bounds.y
  );
  expect(terminalScrollbarCapture.bounds.height).toBe(
    terminalCapture.bounds.height
  );
  expect(terminalCapture.bounds.x + terminalCapture.bounds.width).toBe(
    terminalScrollbarCapture.bounds.x
  );
  expect(
    terminalScrollbarCapture.bounds.x + terminalScrollbarCapture.bounds.width
  ).toBe(
    terminalScrollerCapture.bounds.x + terminalScrollerCapture.bounds.width
  );
  expect(terminalCapture.bounds.y + terminalCapture.bounds.height).toBe(
    statusBarCapture.bounds.y
  );

  return {
    hints,
    mainWindow,
    mainBounds,
    statusBarCapture,
    terminal,
    terminalCapture,
    terminalScrollerCapture,
    terminalScrollbarCapture,
  };
};

/**
 * Saves terminal layout captures and records a layout evidence log entry.
 *
 * @param evidence Test evidence writer.
 * @param layout Terminal grid layout to save.
 * @param name Evidence name prefix.
 * @returns Promise resolved after captures are saved.
 */
export const saveTerminalGridLayoutEvidence = async (
  evidence: TestEvidence,
  layout: TerminalGridLayout,
  name: string
): Promise<void> => {
  await Promise.all([
    evidence.captureEvidence(
      `${name}-terminal-scroller`,
      async () => layout.terminalScrollerCapture
    ),
    evidence.captureEvidence(
      `${name}-terminal`,
      async () => layout.terminalCapture
    ),
    evidence.captureEvidence(
      `${name}-terminal-scrollbar`,
      async () => layout.terminalScrollbarCapture
    ),
    evidence.captureEvidence(
      `${name}-status-bar`,
      async () => layout.statusBarCapture
    ),
  ]);
  await evidence.log('terminal grid layout verified', {
    hints: layout.hints,
    mainBounds: layout.mainBounds,
    name,
  });
};

/**
 * Moves the pointer to the center of the captured terminal area.
 *
 * @param app Running GTK app.
 * @param layout Terminal layout with terminal bounds.
 * @returns Promise resolved after the pointer is moved.
 */
export const moveMouseToTerminalCenter = async (
  app: GtkApp,
  layout: TerminalGridLayout
): Promise<void> => {
  await app.input.moveMouseTo(
    Math.trunc(
      layout.terminalCapture.bounds.x + layout.terminalCapture.bounds.width / 2
    ),
    Math.trunc(
      layout.terminalCapture.bounds.y + layout.terminalCapture.bounds.height / 2
    )
  );
};

/**
 * Sends one Ctrl+wheel event to the running app.
 *
 * @param app Running GTK app.
 * @param ySteps Vertical wheel steps.
 * @returns Promise resolved after the wheel event is sent.
 */
export const scrollWheelWithControl = async (
  app: GtkApp,
  ySteps: number
): Promise<void> => {
  await app.input.setModifier('control', true);
  try {
    await app.input.scrollWheel(0, ySteps);
  } finally {
    await app.input.setModifier('control', false);
  }
};

/**
 * Sends multiple Ctrl+wheel events while holding Control.
 *
 * @param app Running GTK app.
 * @param yStepList Vertical wheel step sequence.
 * @returns Promise resolved after all wheel events are sent.
 */
export const scrollWheelBurstWithControl = async (
  app: GtkApp,
  yStepList: readonly number[]
): Promise<void> => {
  await app.input.setModifier('control', true);
  try {
    for (const ySteps of yStepList) {
      await app.input.scrollWheel(0, ySteps);
    }
  } finally {
    await app.input.setModifier('control', false);
  }
};

/**
 * Sends one key while holding an exact set of keyboard modifiers.
 *
 * @param app Running GTK app.
 * @param modifiers Modifiers held for the key event.
 * @param key X11 keysym sent to the app.
 * @returns Promise resolved after the key event and modifier releases.
 */
export const pressKeyWithModifiers = async (
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
