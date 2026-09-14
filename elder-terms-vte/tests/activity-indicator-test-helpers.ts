import { createRequire } from 'node:module';
import type { GtkApp, GtkCapture } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect } from 'vitest';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

/**
 * Stable activity indicator ids.
 */
export type ActivityIndicatorId =
  'conn' | 'log' | 'sd' | 'rd' | 'rts' | 'cts' | 'dtr' | 'dsr' | 'cd' | 'ri';

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

/**
 * Requested RGB channels, or undefined for the original green lamp images.
 */
export type ActivityIndicatorColor = readonly [
  red: number,
  green: number,
  blue: number,
];

/**
 * Asserts visible lamp hue and brightness independently of the source PNGs.
 *
 * @param capture Captured GtkImage.
 * @param state Expected indicator state.
 * @param color Configured RGB channels; omit for the built-in green lamps.
 * @returns Promise resolved after the display assertions pass.
 */
export const expectActivityIndicatorImageState = async (
  capture: GtkCapture,
  state: ActivityIndicatorImageState,
  color: ActivityIndicatorColor | undefined = undefined
): Promise<void> => {
  const actual = PNG.sync.read(capture.image);
  expect(capture.clipped).toBe(false);
  expect(actual.width).toBe(activityIndicatorIconSize);
  expect(actual.height).toBe(activityIndicatorIconSize);
  const offset = (9 * actual.width + 9) * 4;
  const channels = Array.from(actual.data.subarray(offset, offset + 3));
  const brightness = Math.max(...channels);
  if (color === undefined) {
    if (state === 'on') {
      expect(channels[1] - channels[0]).toBeGreaterThan(35);
      expect(channels[1] - channels[2]).toBeGreaterThan(35);
    } else {
      expect(brightness - Math.min(...channels)).toBeLessThan(12);
    }
  } else {
    // A neutral reflection keeps even black lamps distinguishable. Normalize
    // brightness for the requested color. Custom inactive colors retain the
    // requested brightness instead of multiplying it by a dark OFF template.
    for (let channel = 0; channel < 3; channel += 1) {
      for (let other = 0; other < 3; other += 1) {
        if (color[channel] - color[other] >= 64) {
          expect(channels[channel] - channels[other]).toBeGreaterThan(12);
        }
      }
    }
  }
  const strength =
    color === undefined ? 1 : 0.2 + (0.8 * Math.max(...color)) / 255;
  const normalized = brightness / strength;
  if (state === 'on' || color !== undefined) {
    expect(normalized).toBeGreaterThan(130);
    expect(normalized).toBeLessThan(200);
  } else {
    expect(normalized).toBeGreaterThan(55);
    expect(normalized).toBeLessThan(115);
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
 * @param color Configured RGB channels; omit for the built-in green lamps.
 * @returns Captured GtkImage that matched the expected state.
 */
export const waitForActivityIndicatorImageState = async (
  app: GtkApp,
  indicator: ActivityIndicatorId,
  state: ActivityIndicatorImageState,
  timeoutMs = 5_000,
  color: ActivityIndicatorColor | undefined = undefined
): Promise<GtkCapture> =>
  waitForResult(
    async () => {
      const capture = await captureActivityIndicatorImage(app, indicator);
      await expectActivityIndicatorImageState(capture, state, color);
      return capture;
    },
    {
      message: `${indicator.toUpperCase()} indicator should show ${state}`,
      timeoutMs,
    }
  );
