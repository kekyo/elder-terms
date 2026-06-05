import { createRequire } from 'node:module';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { expect, type TestContext } from 'vitest';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkAppLauncherOptions,
  type GtkCapture,
  type GtkWidgetElement,
  type GtkWindowElement,
  type GtkWindowResizeHints,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
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
 * Launch options forwarded by runGtkTest.
 */
export interface RunGtkTestOptions {
  /** Environment overrides passed to the launched GTK app. */
  readonly env?: GtkAppLauncherOptions['env'];
}

const readPng = (capture: GtkCapture): PngImage => PNG.sync.read(capture.image);

/**
 * Foreground luminance summary for a terminal capture.
 */
export interface TerminalForegroundLuminanceStats {
  /** Mean luminance of pixels different from the capture background. */
  readonly average: number;
  /** Mean luminance distance between foreground pixels and the background. */
  readonly contrast: number;
  /** Number of pixels considered foreground. */
  readonly count: number;
}

/**
 * Measures foreground luminance relative to the terminal background.
 *
 * @param capture Captured terminal widget image.
 * @returns Foreground pixel count and average luminance.
 */
export const terminalForegroundLuminanceStats = (
  capture: GtkCapture
): TerminalForegroundLuminanceStats => {
  const image = readPng(capture);
  const backgroundRed = image.data[0];
  const backgroundGreen = image.data[1];
  const backgroundBlue = image.data[2];
  const backgroundAlpha = image.data[3];
  const backgroundLuminance =
    0.2126 * backgroundRed + 0.7152 * backgroundGreen + 0.0722 * backgroundBlue;
  let count = 0;
  let contrast = 0;
  let total = 0;

  for (let offset = 0; offset < image.data.length; offset += 4) {
    const red = image.data[offset];
    const green = image.data[offset + 1];
    const blue = image.data[offset + 2];
    const alpha = image.data[offset + 3];
    const distance =
      Math.abs(red - backgroundRed) +
      Math.abs(green - backgroundGreen) +
      Math.abs(blue - backgroundBlue) +
      Math.abs(alpha - backgroundAlpha);
    if (alpha > 0 && distance > 24) {
      const luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
      total += luminance;
      contrast += Math.abs(luminance - backgroundLuminance);
      ++count;
    }
  }

  return {
    average: count === 0 ? 0 : total / count,
    contrast: count === 0 ? 0 : contrast / count,
    count,
  };
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
 * Launches the GTK app and records evidence for one Vitest test.
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
  const evidence = createTestEvidence(context);
  const launcher = createGtkAppLauncher({
    appPath,
    env: options?.env,
    onSystemOutput: evidence.recordSystemOutputEvent,
    xvfbTrayHost: false,
  });
  const apps: GtkApp[] = [];

  try {
    await evidence.log('launching app', { appPath, args });
    const app = await launcher.launch(args, {
      onOutput: evidence.recordAppOutputEvent,
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
};

const readPngFile = async (path: string): Promise<PngImage> =>
  PNG.sync.read(await readFile(path));

/**
 * Asserts that a terminal capture exactly matches an expected fixture image.
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
  const expectedPng = await readPngFile(masterImagePath);
  const capture = await waitForResult(async () => {
    const currentCapture = await terminal.capture();
    const currentPng = readPng(currentCapture);
    expect(currentCapture.clipped).toBe(false);
    expect(currentPng.width).toBe(expectedPng.width);
    expect(currentPng.height).toBe(expectedPng.height);
    return currentCapture;
  });
  const savedCapture = await evidence.captureEvidence(
    name,
    async () => capture
  );
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
    }
  );
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
