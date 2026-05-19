import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import type { GtkCapture } from 'gestament';
import type { PNG as PngImage } from 'pngjs';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind } from './test-helpers';
import { defaultColumns, defaultRows, runGtkTest } from './gtk-test-helpers';

const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

const indicatorIconSize = 18;
const indicatorOffIconPath = fileURLToPath(
  new URL('../src/indicators/green-off.png', import.meta.url)
);

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

const expectIndicatorImageMatchesOffAsset = async (
  capture: GtkCapture
): Promise<void> => {
  const actual = readPng(capture);
  const source = PNG.sync.read(await readFile(indicatorOffIconPath));
  const expected = scalePngNearest(
    source,
    indicatorIconSize,
    indicatorIconSize
  );
  const expectedForegroundCount = countNonBackgroundPixels(expected);
  const actualForegroundCount = countNonBackgroundPixels(actual);
  const center = Math.floor(indicatorIconSize / 2);

  expect(actual.width).toBe(indicatorIconSize);
  expect(actual.height).toBe(indicatorIconSize);
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
  expect(averageForegroundGreenDominance(actual)).toBeLessThan(12);
};

describe.concurrent('elder-terms-vte main window', () => {
  it('shows a terminal layout constrained to whole VTE cells', async (context) => {
    await runGtkTest(context, [], async (app, evidence) => {
      expect(await app.getWindowCount()).toBe(1);

      const mainWindow = expectElementKind(
        await app.getById('main_window'),
        'window'
      );
      await app.getById('header_bar');
      await app.getById('root_box');
      const terminalScroller = await app.getById('terminal_scroller');
      const terminal = await app.getById('terminal_view');
      const terminalScrollbar = await app.getById('terminal_scrollbar');
      const statusBar = await app.getById('status_bar');
      const statusLabel = expectElementKind(
        await app.getById('status_label'),
        'label'
      );
      const activityIndicatorBar = await app.getById('activity_indicator_bar');
      const sdIndicatorBox = await app.getById('sd_indicator_box');
      const rdIndicatorBox = await app.getById('rd_indicator_box');
      const sdIndicatorImage = await app.getById('sd_indicator_image');
      const rdIndicatorImage = await app.getById('rd_indicator_image');
      const sdIndicatorLabel = expectElementKind(
        await app.getById('sd_indicator_label'),
        'label'
      );
      const rdIndicatorLabel = expectElementKind(
        await app.getById('rd_indicator_label'),
        'label'
      );

      expect(await statusLabel.text()).toBe('Terminal');
      expect(await sdIndicatorLabel.text()).toBe('SD');
      expect(await rdIndicatorLabel.text()).toBe('RD');

      const [
        mainBounds,
        terminalScrollerCapture,
        terminalCapture,
        terminalScrollbarCapture,
        statusBarCapture,
        statusLabelCapture,
        activityIndicatorBarCapture,
        sdIndicatorBoxCapture,
        rdIndicatorBoxCapture,
        sdIndicatorImageCapture,
        rdIndicatorImageCapture,
        sdIndicatorLabelCapture,
        rdIndicatorLabelCapture,
        hints,
      ] = await Promise.all([
        mainWindow.bounds(),
        evidence.captureEvidence('terminal-scroller', async () =>
          terminalScroller.capture()
        ),
        evidence.captureEvidence('terminal', async () => terminal.capture()),
        evidence.captureEvidence('terminal-scrollbar', async () =>
          terminalScrollbar.capture()
        ),
        evidence.captureEvidence('status-bar', async () => statusBar.capture()),
        evidence.captureEvidence('status-label', async () =>
          statusLabel.capture()
        ),
        evidence.captureEvidence('activity-indicator-bar', async () =>
          activityIndicatorBar.capture()
        ),
        evidence.captureEvidence('sd-indicator-box', async () =>
          sdIndicatorBox.capture()
        ),
        evidence.captureEvidence('rd-indicator-box', async () =>
          rdIndicatorBox.capture()
        ),
        evidence.captureEvidence('sd-indicator-image', async () =>
          sdIndicatorImage.capture()
        ),
        evidence.captureEvidence('rd-indicator-image', async () =>
          rdIndicatorImage.capture()
        ),
        evidence.captureEvidence('sd-indicator-label', async () =>
          sdIndicatorLabel.capture()
        ),
        evidence.captureEvidence('rd-indicator-label', async () =>
          rdIndicatorLabel.capture()
        ),
        mainWindow.resizeHints(),
      ]);

      expect(terminalScrollerCapture.bounds.y).toBeLessThan(
        statusBarCapture.bounds.y
      );
      expect(terminalCapture.bounds.y).toBeLessThan(statusBarCapture.bounds.y);
      expect(terminalCapture.bounds.y + terminalCapture.bounds.height).toBe(
        statusBarCapture.bounds.y
      );
      expect(terminalCapture.bounds.x).toBe(terminalScrollerCapture.bounds.x);
      expect(terminalCapture.bounds.y).toBe(terminalScrollerCapture.bounds.y);
      expect(terminalScrollbarCapture.bounds.y).toBe(terminalCapture.bounds.y);
      expect(terminalScrollbarCapture.bounds.height).toBe(
        terminalCapture.bounds.height
      );
      expect(terminalCapture.bounds.x + terminalCapture.bounds.width).toBe(
        terminalScrollbarCapture.bounds.x
      );
      expect(
        terminalScrollbarCapture.bounds.x +
          terminalScrollbarCapture.bounds.width
      ).toBe(
        terminalScrollerCapture.bounds.x + terminalScrollerCapture.bounds.width
      );
      expect((mainBounds.width - hints.baseWidth) % hints.widthIncrement).toBe(
        0
      );
      expect(
        (mainBounds.height - hints.baseHeight) % hints.heightIncrement
      ).toBe(0);
      expect((mainBounds.width - hints.baseWidth) / hints.widthIncrement).toBe(
        defaultColumns
      );
      expect(
        (mainBounds.height - hints.baseHeight) / hints.heightIncrement
      ).toBe(defaultRows);
      expect(statusBarCapture.bounds.y).toBeGreaterThan(mainBounds.y);
      expect(
        statusBarCapture.bounds.y + statusBarCapture.bounds.height
      ).toBeLessThanOrEqual(mainBounds.y + mainBounds.height);

      expect(statusLabelCapture.bounds.x).toBeGreaterThanOrEqual(
        statusBarCapture.bounds.x
      );
      expect(
        statusLabelCapture.bounds.x - statusBarCapture.bounds.x
      ).toBeLessThanOrEqual(16);
      expect(statusLabelCapture.bounds.y).toBeGreaterThanOrEqual(
        statusBarCapture.bounds.y
      );
      expect(
        statusLabelCapture.bounds.y + statusLabelCapture.bounds.height
      ).toBeLessThanOrEqual(
        statusBarCapture.bounds.y + statusBarCapture.bounds.height
      );
      expect(
        statusLabelCapture.bounds.x + statusLabelCapture.bounds.width
      ).toBeLessThanOrEqual(activityIndicatorBarCapture.bounds.x);
      expect(activityIndicatorBarCapture.bounds.x).toBeGreaterThan(
        statusLabelCapture.bounds.x
      );
      expect(
        activityIndicatorBarCapture.bounds.x +
          activityIndicatorBarCapture.bounds.width
      ).toBeLessThanOrEqual(
        statusBarCapture.bounds.x + statusBarCapture.bounds.width
      );
      expect(
        statusBarCapture.bounds.x +
          statusBarCapture.bounds.width -
          (activityIndicatorBarCapture.bounds.x +
            activityIndicatorBarCapture.bounds.width)
      ).toBeLessThanOrEqual(8);
      expect(sdIndicatorBoxCapture.bounds.x).toBeLessThan(
        rdIndicatorBoxCapture.bounds.x
      );
      expect(
        sdIndicatorBoxCapture.bounds.x + sdIndicatorBoxCapture.bounds.width
      ).toBeLessThanOrEqual(rdIndicatorBoxCapture.bounds.x);
      expect(sdIndicatorImageCapture.bounds.width).toBe(indicatorIconSize);
      expect(sdIndicatorImageCapture.bounds.height).toBe(indicatorIconSize);
      expect(rdIndicatorImageCapture.bounds.width).toBe(indicatorIconSize);
      expect(rdIndicatorImageCapture.bounds.height).toBe(indicatorIconSize);
      expect(sdIndicatorImageCapture.bounds.y).toBeLessThan(
        sdIndicatorLabelCapture.bounds.y
      );
      expect(rdIndicatorImageCapture.bounds.y).toBeLessThan(
        rdIndicatorLabelCapture.bounds.y
      );
      await expectIndicatorImageMatchesOffAsset(sdIndicatorImageCapture);
      await expectIndicatorImageMatchesOffAsset(rdIndicatorImageCapture);
      await evidence.log('terminal layout verified', {
        hints,
        mainBounds,
      });
    });
  });

  it('exits when the main window is closed', async (context) => {
    await runGtkTest(context, ['--test-fixture'], async (app, evidence) => {
      const closeButton = expectElementKind(
        await app.getByPath('main_window.0.0.3'),
        'button'
      );
      await closeButton.click();

      const output = await waitForResult(
        async () => {
          const currentOutput = await app.output();
          expect(currentOutput.exitCode).toBe(0);
          expect(currentOutput.exitSignal).toBeNull();
          return currentOutput;
        },
        {
          message: 'app should exit after closing the main window',
          timeoutMs: 5_000,
        }
      );
      await evidence.log('main window close exited app', {
        exitCode: output.exitCode,
        exitSignal: output.exitSignal,
      });
    });
  });
});
