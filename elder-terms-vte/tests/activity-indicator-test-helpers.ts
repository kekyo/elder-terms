import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import type { GtkApp, GtkCapture } from 'gestament';
import { waitForResult } from 'gestament/testing';
import type { PNG as PngImage } from 'pngjs';
import { expect } from 'vitest';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

/**
 * Stable activity indicator ids.
 */
export type ActivityIndicatorId =
  | 'conn'
  | 'log'
  | 'sd'
  | 'rd'
  | 'rts'
  | 'cts'
  | 'dtr'
  | 'dsr'
  | 'cd'
  | 'ri';

/**
 * Ordered activity indicators shown for non-serial sessions.
 */
export const nonSerialActivityIndicatorIds = [
  'conn',
  'log',
  'sd',
  'rd',
] as const satisfies readonly ActivityIndicatorId[];

/**
 * Ordered activity indicators shown for serial sessions.
 */
export const serialActivityIndicatorIds = [
  'conn',
  'log',
  'sd',
  'rd',
  'rts',
  'cts',
  'dtr',
  'dsr',
  'cd',
  'ri',
] as const satisfies readonly ActivityIndicatorId[];

/**
 * Visible labels for activity indicators.
 */
export const activityIndicatorLabels: Record<ActivityIndicatorId, string> = {
  conn: 'CONN',
  log: 'LOG',
  sd: 'SD',
  rd: 'RD',
  rts: 'RTS',
  cts: 'CTS',
  dtr: 'DTR',
  dsr: 'DSR',
  cd: 'CD',
  ri: 'RI',
};

/**
 * Expected activity indicator image state.
 */
export type ActivityIndicatorImageState = 'off' | 'on';

/**
 * Pixel size used by the activity indicator icons.
 */
export const activityIndicatorIconSize = 18;

const indicatorIconPaths: Record<ActivityIndicatorImageState, string> = {
  off: fileURLToPath(
    new URL('../src/indicators/green-off.png', import.meta.url)
  ),
  on: fileURLToPath(new URL('../src/indicators/green-on.png', import.meta.url)),
};

interface RgbaPixel {
  readonly alpha: number;
  readonly blue: number;
  readonly green: number;
  readonly red: number;
}

const readPng = (capture: GtkCapture): PngImage => PNG.sync.read(capture.image);

const pixelOffset = (png: PngImage, x: number, y: number): number =>
  (y * png.width + x) * 4;

const pixelAt = (png: PngImage, x: number, y: number): RgbaPixel => {
  const offset = pixelOffset(png, x, y);
  return {
    red: png.data[offset],
    green: png.data[offset + 1],
    blue: png.data[offset + 2],
    alpha: png.data[offset + 3],
  };
};

const rgbDistance = (left: RgbaPixel, right: RgbaPixel): number =>
  Math.abs(left.red - right.red) +
  Math.abs(left.green - right.green) +
  Math.abs(left.blue - right.blue);

const scalePngNearest = (
  source: PngImage,
  width: number,
  height: number
): PngImage => {
  const scaled = new PNG({ width, height }) as PngImage;
  for (let y = 0; y < height; ++y) {
    const sourceY = Math.min(
      source.height - 1,
      Math.floor(((y + 0.5) * source.height) / height)
    );
    for (let x = 0; x < width; ++x) {
      const sourceX = Math.min(
        source.width - 1,
        Math.floor(((x + 0.5) * source.width) / width)
      );
      const sourceOffset = pixelOffset(source, sourceX, sourceY);
      const scaledOffset = pixelOffset(scaled, x, y);
      scaled.data[scaledOffset] = source.data[sourceOffset];
      scaled.data[scaledOffset + 1] = source.data[sourceOffset + 1];
      scaled.data[scaledOffset + 2] = source.data[sourceOffset + 2];
      scaled.data[scaledOffset + 3] = source.data[sourceOffset + 3];
    }
  }
  return scaled;
};

const countNonBackgroundPixels = (png: PngImage): number => {
  const background = pixelAt(png, 0, 0);
  let count = 0;
  for (let y = 0; y < png.height; ++y) {
    for (let x = 0; x < png.width; ++x) {
      const pixel = pixelAt(png, x, y);
      const distance =
        rgbDistance(pixel, background) +
        Math.abs(pixel.alpha - background.alpha);
      if (distance > 24) {
        ++count;
      }
    }
  }
  return count;
};

const averageForegroundGreenDominance = (png: PngImage): number => {
  const background = pixelAt(png, 0, 0);
  let dominance = 0;
  let count = 0;
  for (let y = 0; y < png.height; ++y) {
    for (let x = 0; x < png.width; ++x) {
      const pixel = pixelAt(png, x, y);
      const distance =
        rgbDistance(pixel, background) +
        Math.abs(pixel.alpha - background.alpha);
      if (distance <= 24) {
        continue;
      }
      dominance += Math.max(0, pixel.green - Math.max(pixel.red, pixel.blue));
      ++count;
    }
  }
  return count === 0 ? 0 : dominance / count;
};

const countCloseForegroundPixels = (
  actual: PngImage,
  expected: PngImage
): number => {
  let count = 0;
  for (let y = 0; y < expected.height; ++y) {
    for (let x = 0; x < expected.width; ++x) {
      const expectedPixel = pixelAt(expected, x, y);
      if (expectedPixel.alpha <= 16) {
        continue;
      }
      if (rgbDistance(pixelAt(actual, x, y), expectedPixel) <= 70) {
        ++count;
      }
    }
  }
  return count;
};

const readExpectedIndicatorPng = async (
  state: ActivityIndicatorImageState
): Promise<PngImage> =>
  scalePngNearest(
    PNG.sync.read(await readFile(indicatorIconPaths[state])),
    activityIndicatorIconSize,
    activityIndicatorIconSize
  );

/**
 * Asserts that a captured indicator image matches the expected on/off asset.
 *
 * @param capture Captured GtkImage.
 * @param state Expected indicator image state.
 * @returns Promise resolved after the image assertion passes.
 */
export const expectActivityIndicatorImageState = async (
  capture: GtkCapture,
  state: ActivityIndicatorImageState
): Promise<void> => {
  const actual = readPng(capture);
  const expected = await readExpectedIndicatorPng(state);
  const expectedForegroundCount = countNonBackgroundPixels(expected);
  const actualForegroundCount = countNonBackgroundPixels(actual);
  const center = Math.floor(activityIndicatorIconSize / 2);
  const greenDominance = averageForegroundGreenDominance(actual);

  expect(capture.clipped).toBe(false);
  expect(actual.width).toBe(activityIndicatorIconSize);
  expect(actual.height).toBe(activityIndicatorIconSize);
  expect(actualForegroundCount).toBeGreaterThan(
    Math.floor(expectedForegroundCount * 0.65)
  );
  expect(actualForegroundCount).toBeLessThan(
    Math.ceil(expectedForegroundCount * 1.35)
  );
  expect(
    rgbDistance(
      pixelAt(actual, center, center),
      pixelAt(expected, center, center)
    )
  ).toBeLessThanOrEqual(45);
  expect(countCloseForegroundPixels(actual, expected)).toBeGreaterThan(
    Math.floor(expectedForegroundCount * 0.55)
  );
  if (state === 'off') {
    expect(greenDominance).toBeLessThan(12);
  } else {
    expect(greenDominance).toBeGreaterThan(35);
  }
};

/**
 * Captures one activity indicator image widget.
 *
 * @param app Running GTK app.
 * @param indicator Indicator id.
 * @returns Captured GtkImage.
 */
export const captureActivityIndicatorImage = async (
  app: GtkApp,
  indicator: ActivityIndicatorId
): Promise<GtkCapture> => {
  const image = await app.getById(`${indicator}_indicator_image`);
  return image.capture();
};

/**
 * Captures one activity indicator box widget.
 *
 * @param app Running GTK app.
 * @param indicator Indicator id.
 * @returns Captured GtkBox.
 */
export const captureActivityIndicatorBox = async (
  app: GtkApp,
  indicator: ActivityIndicatorId
): Promise<GtkCapture> => {
  const box = await app.getById(`${indicator}_indicator_box`);
  return box.capture();
};

/**
 * Waits until one activity indicator image reaches the expected state.
 *
 * @param app Running GTK app.
 * @param indicator Indicator id.
 * @param state Expected indicator image state.
 * @param timeoutMs Timeout in milliseconds.
 * @returns Captured GtkImage that matched the expected state.
 */
export const waitForActivityIndicatorImageState = async (
  app: GtkApp,
  indicator: ActivityIndicatorId,
  state: ActivityIndicatorImageState,
  timeoutMs = 5_000
): Promise<GtkCapture> =>
  waitForResult(
    async () => {
      const capture = await captureActivityIndicatorImage(app, indicator);
      await expectActivityIndicatorImageState(capture, state);
      return capture;
    },
    {
      message: `${indicator.toUpperCase()} indicator should show ${state}`,
      timeoutMs,
    }
  );
