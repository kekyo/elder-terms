import { existsSync } from 'node:fs';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { expect, type TestContext } from 'vitest';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkCapture,
  type GtkElementOfKind,
  type GtkWidgetElement,
  type GtkWidgetKind,
} from 'gestament';
import {
  createGtkCaptureExpect,
  type GtkCaptureLookSimilarOptions,
} from 'gestament/testing';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

const appPath = fileURLToPath(
  new URL(
    '../../.build/shared/elder-terms-settings-widget-fixture',
    import.meta.url
  )
);

const maxPathSegmentLength = 120;

const formatEvidenceTimestamp = (date: Date): string => {
  const pad = (value: number, length: number): string =>
    value.toString().padStart(length, '0');

  return [
    `${pad(date.getFullYear(), 4)}${pad(date.getMonth() + 1, 2)}${pad(
      date.getDate(),
      2
    )}`,
    `${pad(date.getHours(), 2)}${pad(date.getMinutes(), 2)}${pad(
      date.getSeconds(),
      2
    )}`,
    pad(date.getMilliseconds(), 3),
  ].join('_');
};

const sanitizePathSegment = (value: string): string => {
  const normalized = value
    .normalize('NFKD')
    .replace(/[^A-Za-z0-9._-]+/g, '-')
    .replace(/^[.-]+|[.-]+$/g, '')
    .replace(/[-.]{2,}/g, '-');
  const safe = normalized.length === 0 ? 'unnamed' : normalized;
  if (safe.length <= maxPathSegmentLength) {
    return safe;
  }

  const hash = createHash('sha1').update(safe).digest('hex').slice(0, 12);
  return `${safe.slice(0, maxPathSegmentLength - hash.length - 1)}-${hash}`;
};

const runResultsDirectory = fileURLToPath(
  new URL(
    `../../test-results/${
      process.env.ELDER_TERMS_TEST_RESULT_RUN_ID ??
      formatEvidenceTimestamp(new Date())
    }/shared/`,
    import.meta.url
  )
);

/**
 * Test context for a launched shared GTK fixture.
 */
export interface SharedGtkTestContext {
  /** Running fixture application. */
  readonly app: GtkApp;
  /** Artifact directory for the current test. */
  readonly directory: string;
}

/**
 * Process options for a shared GTK fixture test.
 */
export interface SharedGtkTestOptions {
  /** Additional environment variables passed to the fixture. */
  readonly env: Readonly<Record<string, string>>;
}

/**
 * Launches the shared settings widget fixture for one test body.
 *
 * @param context Vitest test context.
 * @param args Fixture command-line arguments.
 * @param body Test body receiving the launched app.
 * @param options Additional process options.
 * @returns Promise resolved after the app and launcher are released.
 */
export const runSharedGtkTest = async (
  context: TestContext,
  args: readonly string[],
  body: (context: SharedGtkTestContext) => Promise<void>,
  options: SharedGtkTestOptions | undefined = undefined
): Promise<void> => {
  const testName = context.task.fullTestName ?? context.task.name;
  const directory = join(runResultsDirectory, sanitizePathSegment(testName));
  await mkdir(directory, { recursive: true });

  const launcher = createGtkAppLauncher({
    appPath,
    env: {
      LANGUAGE: 'C',
      LC_ALL: 'C.UTF-8',
      ...options?.env,
    },
    xvfbTrayHost: true,
  });
  const app = await launcher.launch(args);
  try {
    await body({ app, directory });
  } finally {
    await launcher.release();
  }
};

/**
 * Asserts that a GTK element exists.
 *
 * @param element Element that may be undefined.
 * @returns Resolved element.
 */
export const expectElement = (
  element: GtkWidgetElement | undefined
): GtkWidgetElement => {
  expect(element).toBeDefined();
  return element as GtkWidgetElement;
};

/**
 * Asserts that a GTK element exists and has the expected kind.
 *
 * @param element Element that may be undefined.
 * @param kind Expected widget kind.
 * @returns Resolved typed element.
 */
export const expectElementKind = <Kind extends GtkWidgetKind>(
  element: GtkWidgetElement | undefined,
  kind: Kind
): GtkElementOfKind<Kind> => {
  const resolved = expectElement(element);
  expect(resolved.kind).toBe(kind);
  return resolved as GtkElementOfKind<Kind>;
};

/**
 * Compares a capture with a committed fixture image.
 *
 * @param capture Actual GTK capture.
 * @param name Comparison name.
 * @param fixturePath Expected fixture path.
 * @param outputDirectory Directory where comparison artifacts are written.
 * @param options Pixel comparison options.
 * @returns Promise resolved after the comparison passes.
 */
export const expectCaptureToMatchFixture = async (
  capture: GtkCapture,
  name: string,
  fixturePath: string,
  outputDirectory: string,
  options?: GtkCaptureLookSimilarOptions
): Promise<void> => {
  if (process.env.ELDER_TERMS_UPDATE_WIDGET_FIXTURES === '1') {
    await mkdir(dirname(fixturePath), { recursive: true });
    await writeFile(fixturePath, capture.image);
  }
  if (!existsSync(fixturePath)) {
    throw new Error(`missing GTK capture fixture: ${fixturePath}`);
  }

  const visualExpect = createGtkCaptureExpect({
    outputResultPath: outputDirectory,
    variant: 'comparisons',
  });
  try {
    await visualExpect
      .expectCapture(capture, name)
      .toLookSimilar(fixturePath, options);
  } finally {
    await visualExpect.release();
  }
};

/**
 * Counts pixels that differ from the capture's top-left pixel.
 *
 * @param capture GTK capture.
 * @returns Number of visually different pixels.
 */
export const countNonBackgroundPixels = (capture: GtkCapture): number => {
  const png = PNG.sync.read(capture.image);
  const red = png.data[0] ?? 0;
  const green = png.data[1] ?? 0;
  const blue = png.data[2] ?? 0;
  let count = 0;
  for (let index = 0; index < png.data.length; index += 4) {
    const distance =
      Math.abs(png.data[index] - red) +
      Math.abs(png.data[index + 1] - green) +
      Math.abs(png.data[index + 2] - blue);
    if (distance > 18) {
      count += 1;
    }
  }
  return count;
};

/**
 * Counts exact pixel differences between a capture and a fixture image.
 *
 * @param capture Actual GTK capture.
 * @param fixturePath Expected fixture path.
 * @returns Number of differing pixels.
 */
export const countCaptureFixtureDiffPixels = async (
  capture: GtkCapture,
  fixturePath: string
): Promise<number> => {
  const actual = PNG.sync.read(capture.image);
  const expected = PNG.sync.read(await readFile(fixturePath));
  if (actual.width !== expected.width || actual.height !== expected.height) {
    return Math.max(
      actual.width * actual.height,
      expected.width * expected.height
    );
  }

  let count = 0;
  for (let index = 0; index < actual.data.length; index += 4) {
    if (
      actual.data[index] !== expected.data[index] ||
      actual.data[index + 1] !== expected.data[index + 1] ||
      actual.data[index + 2] !== expected.data[index + 2] ||
      actual.data[index + 3] !== expected.data[index + 3]
    ) {
      count += 1;
    }
  }
  return count;
};
