import { existsSync } from 'node:fs';
import { appendFile, mkdir, readFile, writeFile } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { expect, type TestContext } from 'vitest';
import {
  type GtkApp,
  type GtkAppLauncher,
  type GtkAppOutputEvent,
  type GtkCapture,
  type GtkElementOfKind,
  type GtkSystemOutputEvent,
  type GtkWidgetElement,
  type GtkWidgetKind,
} from 'gestament';
import {
  createGtkCaptureExpect,
  type GtkCaptureExpectedImage,
  type GtkCaptureLookSimilarOptions,
  type GtkCaptureLookSimilarResult,
} from 'gestament/testing';

const maxPathSegmentLength = 120;

/**
 * Converts a date into the test result timestamp directory format.
 *
 * @param date Source date.
 * @returns Timestamp formatted as YYYYMMDD_HHmmss_fff.
 */
export const formatEvidenceTimestamp = (date: Date): string => {
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

/**
 * Converts arbitrary text into a stable filesystem-safe path segment.
 *
 * @param value Source text.
 * @returns Safe non-empty path segment.
 */
export const sanitizePathSegment = (value: string): string => {
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
    }/`,
    import.meta.url
  )
);
const reservedTestDirectoryNames = new Map<string, number>();

/**
 * Shared evidence writer for one Vitest test.
 */
export interface TestEvidence {
  /** Root directory for this test's artifacts. */
  readonly directory: string;
  /**
   * Saves a GTK capture and returns it.
   *
   * @param label Artifact label.
   * @param capture Capture operation.
   * @returns Captured GTK image.
   */
  readonly captureEvidence: (
    label: string,
    capture: () => Promise<GtkCapture>
  ) => Promise<GtkCapture>;
  /**
   * Flushes retained application and launcher outputs to disk.
   *
   * @param apps Application handles to snapshot before release.
   * @param launcher Launcher handle to snapshot system output from.
   * @returns Promise resolved after outputs are written.
   */
  readonly flushOutputs: (
    apps: readonly GtkApp[],
    launcher: GtkAppLauncher | undefined
  ) => Promise<void>;
  /**
   * Appends a diagnostic line to test.log.
   *
   * @param message Log message.
   * @param details Optional structured details.
   * @returns Promise resolved after the line is appended.
   */
  readonly log: (message: string, details?: unknown) => Promise<void>;
  /**
   * Compares a capture with an expected image and saves comparison evidence.
   *
   * @param capture Actual capture.
   * @param name Comparison name.
   * @param expectedImage Expected PNG source.
   * @param options Pixel comparison options.
   * @returns gestament comparison result.
   */
  readonly expectCaptureToLookSimilar: (
    capture: GtkCapture,
    name: string,
    expectedImage: GtkCaptureExpectedImage,
    options?: GtkCaptureLookSimilarOptions
  ) => Promise<GtkCaptureLookSimilarResult>;
  /**
   * Records one application stdout/stderr callback event.
   *
   * @param event Output event.
   */
  readonly recordAppOutputEvent: (event: GtkAppOutputEvent) => void;
  /**
   * Records one launcher infrastructure stdout/stderr callback event.
   *
   * @param event Output event.
   */
  readonly recordSystemOutputEvent: (event: GtkSystemOutputEvent) => void;
  /**
   * Releases evidence helper resources.
   *
   * @returns Promise resolved after release.
   */
  readonly release: () => Promise<void>;
}

const appendByStream = <
  Event extends { readonly stream: 'stdout' | 'stderr'; readonly text: string },
>(
  output: Record<'stdout' | 'stderr', string>,
  event: Event
): void => {
  output[event.stream] += event.text;
};

const createUniqueName = (baseName: string, directory: string): string => {
  let index = reservedTestDirectoryNames.get(baseName) ?? 0;
  let name = index === 0 ? baseName : `${baseName}-${index + 1}`;
  while (existsSync(join(directory, name))) {
    index += 1;
    name = `${baseName}-${index + 1}`;
  }
  reservedTestDirectoryNames.set(baseName, index + 1);
  return name;
};

const createPerEvidenceUniqueName = (
  labels: Map<string, number>,
  label: string
): string => {
  const safeLabel = sanitizePathSegment(label);
  const index = labels.get(safeLabel) ?? 0;
  labels.set(safeLabel, index + 1);
  return index === 0 ? safeLabel : `${safeLabel}-${index + 1}`;
};

const errorToJson = (error: unknown): unknown => {
  if (error instanceof Error) {
    return {
      message: error.message,
      name: error.name,
      stack: error.stack,
    };
  }
  return error;
};

const jsonReplacer = (_key: string, value: unknown): unknown => {
  if (value instanceof Error) {
    return errorToJson(value);
  }
  if (typeof value === 'bigint') {
    return value.toString();
  }
  if (Buffer.isBuffer(value)) {
    return {
      byteLength: value.length,
      type: 'Buffer',
    };
  }
  return value;
};

const toJsonLine = (value: unknown): string =>
  `${JSON.stringify(value, jsonReplacer)}\n`;

const toPrettyJson = (value: unknown): string =>
  `${JSON.stringify(value, jsonReplacer, 2)}\n`;

const writeJson = async (path: string, value: unknown): Promise<void> => {
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, toPrettyJson(value), 'utf8');
};

const writeText = async (path: string, value: string): Promise<void> => {
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, value, 'utf8');
};

const loadExpectedImage = async (
  expectedImage: GtkCaptureExpectedImage
): Promise<Buffer> => {
  if (Buffer.isBuffer(expectedImage)) {
    return expectedImage;
  }
  if (typeof expectedImage === 'string') {
    return readFile(resolve(expectedImage));
  }
  return readFile(fileURLToPath(expectedImage));
};

const createTestDirectory = (context: TestContext): string => {
  const testName = context.task.fullTestName ?? context.task.name;
  const safeName = sanitizePathSegment(testName);
  return join(
    runResultsDirectory,
    createUniqueName(safeName, runResultsDirectory)
  );
};

const captureMetadata = (
  label: string,
  capture: GtkCapture,
  pngPath: string
): object => ({
  bounds: capture.bounds,
  clipped: capture.clipped,
  imageBytes: capture.image.length,
  label,
  pngPath,
  savedAt: new Date().toISOString(),
  visibleBounds: capture.visibleBounds,
});

const writeJsonLines = async (
  path: string,
  values: readonly unknown[]
): Promise<void> => {
  const text =
    values.length === 0
      ? ''
      : values.map((value) => toJsonLine(value)).join('');
  await writeText(path, text);
};

const writeSystemEventLogs = async (
  directory: string,
  events: readonly GtkSystemOutputEvent[]
): Promise<void> => {
  const grouped = new Map<string, string>();
  for (const event of events) {
    const key = `${sanitizePathSegment(event.source)}.${event.stream}`;
    grouped.set(key, `${grouped.get(key) ?? ''}${event.text}`);
  }
  for (const [key, text] of grouped) {
    await writeText(join(directory, `system-output.${key}.log`), text);
  }
};

/**
 * Creates the evidence writer for a Vitest test context.
 *
 * @param context Current Vitest test context.
 * @returns Evidence writer.
 */
export const createTestEvidence = (context: TestContext): TestEvidence => {
  const directory = createTestDirectory(context);
  const capturesDirectory = join(directory, 'captures');
  const outputsDirectory = join(directory, 'outputs');
  const testLogPath = join(directory, 'test.log');
  const visualExpect = createGtkCaptureExpect({
    outputResultPath: directory,
    variant: 'comparisons',
  });
  const artifactLabels = new Map<string, number>();
  const appOutputEvents: GtkAppOutputEvent[] = [];
  const appOutputText: Record<'stdout' | 'stderr', string> = {
    stderr: '',
    stdout: '',
  };
  const systemOutputEvents: GtkSystemOutputEvent[] = [];
  const systemOutputText = new Map<string, string>();
  const ready = mkdir(outputsDirectory, { recursive: true }).then(async () => {
    await Promise.all([
      mkdir(capturesDirectory, { recursive: true }),
      mkdir(join(directory, 'comparisons'), { recursive: true }),
    ]);
    await appendFile(
      testLogPath,
      toJsonLine({
        event: 'test-start',
        testName: context.task.fullTestName ?? context.task.name,
        timestamp: new Date().toISOString(),
      }),
      'utf8'
    );
  });

  const log = async (message: string, details?: unknown): Promise<void> => {
    await ready;
    await appendFile(
      testLogPath,
      toJsonLine({
        details,
        message,
        timestamp: new Date().toISOString(),
      }),
      'utf8'
    );
  };

  const captureEvidence = async (
    label: string,
    capture: () => Promise<GtkCapture>
  ): Promise<GtkCapture> => {
    await ready;
    const resolvedCapture = await capture();
    const safeLabel = createPerEvidenceUniqueName(artifactLabels, label);
    const pngPath = join(capturesDirectory, `${safeLabel}.png`);
    await writeFile(pngPath, resolvedCapture.image);
    await writeJson(
      join(capturesDirectory, `${safeLabel}.json`),
      captureMetadata(label, resolvedCapture, pngPath)
    );
    await log('capture saved', {
      bounds: resolvedCapture.bounds,
      clipped: resolvedCapture.clipped,
      label,
      pngPath,
      visibleBounds: resolvedCapture.visibleBounds,
    });
    return resolvedCapture;
  };

  const writeComparisonResult = async (
    name: string,
    expectedImage: GtkCaptureExpectedImage,
    result: GtkCaptureLookSimilarResult
  ): Promise<void> => {
    const outputDirectory =
      result.outputResultPath ??
      join(directory, 'comparisons', sanitizePathSegment(name));
    await mkdir(outputDirectory, { recursive: true });
    await writeFile(
      join(outputDirectory, 'expected.png'),
      await loadExpectedImage(expectedImage)
    );
    await writeJson(join(outputDirectory, 'result.json'), {
      name,
      result,
      savedAt: new Date().toISOString(),
    });
  };

  const expectCaptureToLookSimilar = async (
    capture: GtkCapture,
    name: string,
    expectedImage: GtkCaptureExpectedImage,
    options?: GtkCaptureLookSimilarOptions
  ): Promise<GtkCaptureLookSimilarResult> => {
    await ready;
    try {
      const result = await visualExpect
        .expectCapture(capture, name)
        .toLookSimilar(expectedImage, options);
      await writeComparisonResult(name, expectedImage, result);
      await log('comparison passed', { name, result });
      return result;
    } catch (error) {
      const maybeResult = (
        error as { readonly result?: GtkCaptureLookSimilarResult }
      ).result;
      if (maybeResult !== undefined) {
        await writeComparisonResult(name, expectedImage, maybeResult);
        await log('comparison failed', {
          error: errorToJson(error),
          name,
          result: maybeResult,
        });
      }
      throw error;
    }
  };

  const recordAppOutputEvent = (event: GtkAppOutputEvent): void => {
    appOutputEvents.push(event);
    appendByStream(appOutputText, event);
  };

  const recordSystemOutputEvent = (event: GtkSystemOutputEvent): void => {
    systemOutputEvents.push(event);
    const key = `${sanitizePathSegment(event.source)}.${event.stream}`;
    systemOutputText.set(
      key,
      `${systemOutputText.get(key) ?? ''}${event.text}`
    );
  };

  const flushOutputs = async (
    apps: readonly GtkApp[],
    launcher: GtkAppLauncher | undefined
  ): Promise<void> => {
    await ready;
    await writeJsonLines(
      join(outputsDirectory, 'app-output-events.jsonl'),
      appOutputEvents
    );
    await writeText(
      join(outputsDirectory, 'app-output.stdout.log'),
      appOutputText.stdout
    );
    await writeText(
      join(outputsDirectory, 'app-output.stderr.log'),
      appOutputText.stderr
    );

    for (const [index, app] of apps.entries()) {
      const prefix = index === 0 ? 'app-output' : `app-output-${index + 1}`;
      try {
        const output = await app.output();
        await writeJson(join(outputsDirectory, `${prefix}.json`), output);
        await writeText(
          join(outputsDirectory, `${prefix}.stdout.log`),
          output.stdout
        );
        await writeText(
          join(outputsDirectory, `${prefix}.stderr.log`),
          output.stderr
        );
      } catch (error) {
        await writeJson(
          join(outputsDirectory, `${prefix}.error.json`),
          errorToJson(error)
        );
      }
    }

    await writeJsonLines(
      join(outputsDirectory, 'system-output-events.jsonl'),
      systemOutputEvents
    );
    for (const [key, text] of systemOutputText) {
      await writeText(join(outputsDirectory, `system-output.${key}.log`), text);
    }
    await writeSystemEventLogs(outputsDirectory, systemOutputEvents);

    if (launcher !== undefined) {
      try {
        const systemOutput = await launcher.systemOutput();
        await writeJson(
          join(outputsDirectory, 'system-output.json'),
          systemOutput
        );
        for (const source of systemOutput.sources) {
          const safeSource = sanitizePathSegment(source.source);
          await writeText(
            join(
              outputsDirectory,
              `system-output.${safeSource}.stdout.snapshot.log`
            ),
            source.stdout
          );
          await writeText(
            join(
              outputsDirectory,
              `system-output.${safeSource}.stderr.snapshot.log`
            ),
            source.stderr
          );
        }
      } catch (error) {
        await writeJson(
          join(outputsDirectory, 'system-output.error.json'),
          errorToJson(error)
        );
      }
    }
  };

  const release = async (): Promise<void> => {
    await ready;
    await log('test-finish');
    await visualExpect.release();
  };

  return {
    captureEvidence,
    directory,
    expectCaptureToLookSimilar,
    flushOutputs,
    log,
    recordAppOutputEvent,
    recordSystemOutputEvent,
    release,
  };
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
