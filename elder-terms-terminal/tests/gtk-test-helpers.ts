// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { createRequire } from 'node:module';
import {
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { basename, dirname, extname, join, resolve } from 'node:path';
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
} from '../../shared/tests/connection-test-helpers.ts';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

const appPath = fileURLToPath(
  new URL(
    '../../.build/elder-terms-terminal/elder-terms-terminal',
    import.meta.url
  )
);
const fixtureDirectory = fileURLToPath(new URL('./fixtures/', import.meta.url));

/** Test-only synchronous XYZMODEM peer used by transfer progress notice tests. */
export const xyzmPausePeerPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/xyzm-pause-peer', import.meta.url)
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

/** Expected 80x24 terminal fixture PNG with a direct 14.5 DIP font size. */
export const terminalTextGrid80x24Font145Path = fileURLToPath(
  new URL('./fixtures/terminal-text-grid-80x24-font-14.5.png', import.meta.url)
);

/** Expected grid after one native visual zoom step from the default size. */
export const terminalTextGrid80x24VisualStepPath = fileURLToPath(
  new URL(
    './fixtures/terminal-text-grid-80x24-visual-step.png',
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

/** INI fixture that disables link decorations for glyph layout assertions. */
export const terminalPlainTextGridConfigPath = fileURLToPath(
  new URL('./fixtures/terminal-plain-text-grid.ini', import.meta.url)
);

/** INI fixture that configures the terminal grid to 81x25. */
export const terminalGrid81x25ConfigPath = fileURLToPath(
  new URL('./fixtures/terminal-grid-81x25.ini', import.meta.url)
);

/** INI fixture that configures a direct font size of 14.5 DIP. */
export const terminalFont145ConfigPath = fileURLToPath(
  new URL('./fixtures/terminal-font-14.5.ini', import.meta.url)
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
  /** Effective cell width in GTK logical pixels. */
  readonly cellWidth: number;
  /** Effective cell height in GTK logical pixels. */
  readonly cellHeight: number;
  /** Actual non-terminal width of the window. */
  readonly chromeWidth: number;
  /** Actual non-terminal height of the window. */
  readonly chromeHeight: number;
  /** Committed terminal columns. */
  readonly columns: number;
  /** Committed terminal rows. */
  readonly rows: number;
  /** Requested font size in device-independent pixels. */
  readonly fontSizeDip: number;
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
  /** Effective cell width in GTK logical pixels. */
  readonly cellWidth: number;
  /** Effective cell height in GTK logical pixels. */
  readonly cellHeight: number;
  /** Actual non-terminal width of the window. */
  readonly chromeWidth: number;
  /** Actual non-terminal height of the window. */
  readonly chromeHeight: number;
  /** Committed terminal columns. */
  readonly columns: number;
  /** Committed terminal rows. */
  readonly rows: number;
  /** Requested font size in device-independent pixels. */
  readonly fontSizeDip: number;
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
 * Terminal grid size reported by the hidden fixture measurement label.
 */
export interface TerminalGridSize {
  /** Terminal column count. */
  readonly columns: number;
  /** Terminal row count. */
  readonly rows: number;
}

/**
 * Launch options forwarded by runGtkTest.
 */
export interface RunGtkTestOptions {
  /** Whether this test needs the visible Xvfb tray host. Defaults to true. */
  readonly trayHost?: boolean;
  /** Use a disposable display when testing a window manager or compositor. */
  readonly isolatedDisplay?: boolean;
  /** Environment overrides, including the isolated XDG config home. */
  readonly env?: GtkAppLauncherOptions['env'];
  /** Optional global.ini contents written before the application starts. */
  readonly globalSettings?: string;
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
 * Asserts the text shown in the main-window status bar.
 *
 * @param app Running GTK app.
 * @param expectedStatus Expected status text.
 */
export const expectMainWindowStatus = async (
  app: GtkApp,
  expectedStatus: string
): Promise<void> => {
  const statusLabel = expectElementKind(
    await app.getById('status_label'),
    'label'
  );
  await toPass(
    async () => {
      expect(await statusLabel.text()).toBe(expectedStatus);
    },
    {
      message: `main window status should be ${expectedStatus}`,
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
 * @param expectedMessage Expected reason, or the generic fixture text by default.
 */
export const expectDisconnectedNoticeVisibleAtTerminalTopRight = async (
  app: GtkApp,
  expectedMessage: string | RegExp = 'Disconnected'
): Promise<void> => {
  const terminal = await app.getById('terminal_view');
  const notice = await app.getById('disconnected_notice');
  const label = expectElementKind(
    await app.getById('disconnected_notice_label'),
    'label'
  );

  const { noticeCapture, terminalCapture } = await waitForResult(
    async () => {
      const info = await notice.info();
      expect(info.states).toContain('showing');
      expect(info.states).toContain('visible');
      if (typeof expectedMessage === 'string')
        expect(await label.text()).toBe(expectedMessage);
      else expect(await label.text()).toMatch(expectedMessage);
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
  const cancel = await app.getById('transfer_cancel_button');
  const cancelBounds = (await cancel.capture()).bounds;
  expect(cancelBounds.x + cancelBounds.width).toBe(noticeRight);
  expect(cancelBounds.y).toBeGreaterThanOrEqual(
    noticeCapture.bounds.y + noticeCapture.bounds.height
  );
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
 * Asserts the disconnected notice is rendered above terminal dimming.
 *
 * @param app Running GTK app.
 * @param evidence Writer receiving the rendered notice capture.
 * @param expectedMessage Expected explanation displayed above the dimming layer.
 */
export const expectDisconnectedNoticeRenderedUndimmedAtTerminalTopRight =
  async (
    app: GtkApp,
    evidence: TestEvidence,
    expectedMessage: string | RegExp
  ): Promise<void> => {
    await expectDisconnectedNoticeVisibleAtTerminalTopRight(
      app,
      expectedMessage
    );

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
    await evidence.captureEvidence(
      'disconnected-notice-rendered-on-terminal',
      async () => renderedNoticeCapture
    );

    // Dynamic device paths change the notice dimensions. Its background and
    // bright foreground must still be painted above the terminal dimming layer.
    const reference = PNG.sync.read(
      await readFile(disconnectedNoticeOnTerminalPath)
    );
    const backgroundOffset = (2 * reference.width + 2) * 4;
    const renderedOffset = (2 * renderedNoticePng.width + 2) * 4;
    expect(
      renderedNoticePng.data.subarray(renderedOffset, renderedOffset + 4)
    ).toEqual(reference.data.subarray(backgroundOffset, backgroundOffset + 4));
    let brightTextPixels = 0;
    for (let i = 0; i < renderedNoticePng.data.length; i += 4)
      if (
        renderedNoticePng.data[i] === 255 &&
        renderedNoticePng.data[i + 1] === 255 &&
        renderedNoticePng.data[i + 2] === 0
      )
        ++brightTextPixels;
    expect(brightTextPixels).toBeGreaterThan(10);
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
 * @remarks Temporary XDG config and data homes isolate each launch unless overridden
 * through `options.env`. Repository INI fixtures passed with `-c` are copied
 * into the temporary directory before launch so startup writes leave sources intact.
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
    join(tmpdir(), 'elder-terms-terminal-xdg-config-')
  );
  try {
    const launchArgs = [...args];
    const configArgIndex = launchArgs.indexOf('-c');
    if (configArgIndex >= 0 && configArgIndex + 1 < launchArgs.length) {
      const configPath = launchArgs[configArgIndex + 1];
      if (
        dirname(resolve(configPath)) === resolve(fixtureDirectory) &&
        extname(configPath) === '.ini'
      ) {
        const copiedPath = join(configHome, basename(configPath));
        await copyFile(configPath, copiedPath);
        launchArgs[configArgIndex + 1] = copiedPath;
      }
    }
    if (options?.globalSettings !== undefined) {
      const globalDirectory = join(configHome, 'elder-terms');
      await mkdir(globalDirectory, { recursive: true });
      await writeFile(
        join(globalDirectory, 'global.ini'),
        options.globalSettings,
        'utf8'
      );
    }
    const evidence = createTestEvidence(context);
    const launcher = createGtkAppLauncher({
      appPath,
      env: {
        LANGUAGE: 'C',
        LC_ALL: 'C.UTF-8',
        XDG_CONFIG_HOME: configHome,
        XDG_DATA_HOME: join(configHome, 'data'),
        ...options?.env,
      },
      onSystemOutput: evidence.recordSystemOutputEvent,
      xvfbPool: options?.isolatedDisplay
        ? undefined
        : {
            maxIdlePerKey: 8,
            maxIdleTotal: 8,
            type: 'xvfb',
          },
      xvfbTrayHost: options?.trayHost ?? true,
    });
    const apps: GtkApp[] = [];

    try {
      await evidence.log('launching app', { appPath, args: launchArgs });
      const app = await launcher.launch(launchArgs, {
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
 * @remarks Set `ELDER_TERMS_UPDATE_terminal_FIXTURES=1` to replace the fixture
 * with the current capture before comparing it.
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
  if (process.env.ELDER_TERMS_UPDATE_terminal_FIXTURES === '1') {
    await mkdir(dirname(masterImagePath), { recursive: true });
    await writeFile(masterImagePath, savedCapture.image);
  }
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
 * Asserts that a window size maps to the expected terminal cell count.
 *
 * @param layout Layout with window bounds and effective metrics.
 * @param columns Expected columns.
 * @param rows Expected rows.
 */
export const expectWindowCellSize = (
  layout: TerminalGridLayout | WindowCellLayout,
  columns: number,
  rows: number
): void => {
  expect(layout.columns).toBe(columns);
  expect(layout.rows).toBe(rows);
  const remainderWidth =
    layout.mainBounds.width - layout.chromeWidth - columns * layout.cellWidth;
  const remainderHeight =
    layout.mainBounds.height - layout.chromeHeight - rows * layout.cellHeight;
  expect(remainderWidth).toBeGreaterThanOrEqual(0);
  expect(remainderWidth).toBeLessThan(layout.cellWidth);
  expect(remainderHeight).toBeGreaterThanOrEqual(0);
  expect(remainderHeight).toBeLessThan(layout.cellHeight);
};

/**
 * Reads the terminal grid size exposed by the hidden fixture measurement label.
 *
 * @param app Running GTK app.
 * @returns Parsed terminal grid size.
 */
export const readFixtureTerminalGridSize = async (
  app: GtkApp
): Promise<TerminalGridSize> => {
  const gridSizeLabel = expectElementKind(
    await app.getById('fixture_grid_size_label'),
    'label'
  );
  const text = await gridSizeLabel.text();
  const match = /^([0-9]+)x([0-9]+)$/.exec(text);
  expect(match).not.toBeNull();

  return {
    columns: Number(match?.[1]),
    rows: Number(match?.[2]),
  };
};

/**
 * Asserts the terminal grid size exposed by the hidden fixture measurement
 * label.
 *
 * @param app Running GTK app.
 * @param columns Expected columns.
 * @param rows Expected rows.
 * @returns Parsed terminal grid size.
 */
export const expectFixtureTerminalGridSize = async (
  app: GtkApp,
  columns: number,
  rows: number
): Promise<TerminalGridSize> => {
  const gridSize = await readFixtureTerminalGridSize(app);
  expect(gridSize).toStrictEqual({ columns, rows });
  return gridSize;
};

/**
 * Reads the scrollback size reported by terminal through the hidden fixture label.
 *
 * @param app Running GTK app.
 * @returns Current terminal scrollback size in lines.
 */
export const readFixtureTerminalScrollbackLines = async (
  app: GtkApp
): Promise<number> => {
  const scrollbackLabel = expectElementKind(
    await app.getById('fixture_scrollback_lines_label'),
    'label'
  );
  const lines = Number(await scrollbackLabel.text());
  expect(Number.isInteger(lines)).toBe(true);
  return lines;
};

/**
 * Reads main window geometry and effective metrics.
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
  const status = expectElementKind(
    await app.getById('fixture_terminal_state_label'),
    'label'
  );
  const report = await status.text();
  expect(report).toContain('ready=1');
  const grid = /grid=(\d+)x(\d+)/.exec(report);
  const metrics = /cell=(\d+)x(\d+)/.exec(report);
  if (!grid || !metrics) throw new Error('Missing native terminal metrics');
  const scale = Number(/scale=(\d+)/.exec(report)?.[1] ?? 1);
  const cellWidth = Number(metrics[1]) / scale;
  const cellHeight = Number(metrics[2]) / scale;
  expect(cellWidth).toBeGreaterThan(0);
  expect(cellHeight).toBeGreaterThan(0);
  const mainBounds = await mainWindow.bounds();
  const terminalBounds = (await (await app.getById('terminal_view')).capture())
    .bounds;
  return {
    mainBounds,
    mainWindow,
    cellWidth,
    cellHeight,
    chromeWidth: mainBounds.width - terminalBounds.width,
    chromeHeight: mainBounds.height - terminalBounds.height,
    columns: Number(grid[1]),
    rows: Number(grid[2]),
    fontSizeDip: Number(/font=([\d.]+)/.exec(report)?.[1]),
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
  const geometry = await readWindowCellLayout(app);
  const terminalScroller = await app.getById('terminal_scroller');
  const terminal = await app.getById('terminal_view');
  const terminalScrollbar = await app.getById('terminal_scrollbar');
  const statusBar = await app.getById('status_bar');

  const [
    terminalScrollerCapture,
    terminalCapture,
    terminalScrollbarCapture,
    statusBarCapture,
  ] = await Promise.all([
    terminalScroller.capture(),
    terminal.capture(),
    terminalScrollbar.capture(),
    statusBar.capture(),
  ]);

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
    ...geometry,
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
    cell: [layout.cellWidth, layout.cellHeight],
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
