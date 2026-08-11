import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { TestContext } from 'vitest';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkAppEnvironment,
  type GtkCapture,
  type GtkElementOfKind,
  type GtkWidgetElement,
  type GtkWidgetKind,
} from 'gestament';
import {
  createGtkCaptureExpect,
  type GtkCaptureLookSimilarOptions,
} from 'gestament/testing';

const defaultAppPath = fileURLToPath(
  new URL('../../.build/elder-terms/elder-terms', import.meta.url)
);
const x11MapRecorderPath = fileURLToPath(
  new URL('../../.build/elder-terms/x11-map-recorder', import.meta.url)
);
const launcherTestResultsDirectory = fileURLToPath(
  new URL('../../test-results/launcher/', import.meta.url)
);

/** One top-level X11 window map observed during a launcher test. */
export interface X11MapEvent {
  /** X11 window identifier. */
  readonly windowId: string;
  /** Process identifier advertised by the window, or zero when unavailable. */
  readonly processId: number;
  /** Window manager title. */
  readonly name: string;
  /** Window manager instance name. */
  readonly instanceName: string;
  /** Window manager class name. */
  readonly className: string;
  /** Whether the window publishes valid _NET_WM_ICON dimensions and pixels. */
  readonly hasIcon: boolean;
}

/** Modifier accepted by the X11 hotkey test recorder. */
export type X11HotkeyModifier = 'control' | 'alt' | 'shift' | 'super';

/** One X11 key combination reserved by the test recorder. */
export interface X11Hotkey {
  /** X11 keysym name. */
  readonly key: string;
  /** Modifiers held with the key. */
  readonly modifiers: readonly X11HotkeyModifier[];
}

/** Records top-level X11 window maps from before application launch. */
export interface X11MapRecorder {
  /**
   * Flushes X11 events that precede this call into the recorder.
   *
   * @returns A promise completed after the recorder reaches the barrier.
   */
  readonly flush: () => Promise<void>;
  /**
   * Returns all map events observed so far.
   *
   * @returns A snapshot of recorded events.
   */
  readonly events: () => readonly X11MapEvent[];
  /**
   * Maps and focuses a competing top-level window.
   *
   * @returns A promise completed with the decimal X11 window identifier.
   */
  readonly focusCompetitor: () => Promise<string>;
  /**
   * Reads the top-level X11 window that currently owns input focus.
   *
   * @returns A promise completed with the decimal X11 window identifier.
   */
  readonly focusedWindow: () => Promise<string>;
  /**
   * Reserves a global key combination in the recorder process.
   *
   * @param hotkey Key combination to reserve.
   * @returns A promise completed after the X server accepts the grab.
   */
  readonly grabHotkey: (hotkey: X11Hotkey) => Promise<void>;
  /**
   * Stops the recorder.
   *
   * @returns A promise completed after the helper exits.
   */
  readonly stop: () => Promise<void>;
}

/** Context passed to a launcher GTK integration test. */
export interface LauncherGtkTestContext {
  /** Running launcher application. */
  readonly app: GtkApp;
  /** Isolated XDG configuration root. */
  readonly configHome: string;
  /** Isolated connection profile directory. */
  readonly connections: string;
  /** X11 map recorder when requested by the test. */
  readonly x11MapRecorder: X11MapRecorder | undefined;
}

/** Options for launching the GTK application under test. */
export interface LauncherGtkTestOptions {
  /** Launcher executable path. */
  readonly appPath?: string;
  /** Additional command-line arguments. */
  readonly args: readonly string[];
  /** Additional environment variables. */
  readonly env: Readonly<Record<string, string>>;
  /** Whether the isolated Xvfb session provides a StatusNotifier tray host. */
  readonly xvfbTrayHost?: boolean;
  /** Whether to record top-level X11 window maps from before launch. */
  readonly recordX11Maps?: boolean;
  /** X11 key combinations reserved before the application starts. */
  readonly blockedHotkeys?: readonly X11Hotkey[];
}

const x11ModifierMasks: Readonly<Record<X11HotkeyModifier, number>> = {
  shift: 1,
  control: 4,
  alt: 8,
  super: 64,
};

const x11ModifierMask = (modifiers: readonly X11HotkeyModifier[]): number =>
  modifiers.reduce((mask, modifier) => mask | x11ModifierMasks[modifier], 0);

const parseX11MapEvent = (line: string): X11MapEvent | undefined => {
  const [kind, windowId, processId, name, instanceName, className, hasIcon] =
    line.split('\t');
  if (
    kind !== 'map' ||
    windowId === undefined ||
    processId === undefined ||
    name === undefined ||
    instanceName === undefined ||
    className === undefined ||
    (hasIcon !== '0' && hasIcon !== '1')
  ) {
    return undefined;
  }
  return {
    windowId,
    processId: Number(processId),
    name,
    instanceName,
    className,
    hasIcon: hasIcon === '1',
  };
};

const startX11MapRecorder = async (
  environment: GtkAppEnvironment
): Promise<X11MapRecorder> => {
  const child = spawn(x11MapRecorderPath, [], {
    env: {
      ...process.env,
      ...environment,
    },
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  const recordedEvents: X11MapEvent[] = [];
  const barriers = new Map<
    string,
    {
      readonly resolve: () => void;
      readonly reject: (error: Error) => void;
    }
  >();
  const requests = new Map<
    string,
    {
      readonly resolve: (value: string) => void;
      readonly reject: (error: Error) => void;
    }
  >();
  let nextBarrierId = 1;
  let nextRequestId = 1;
  let outputBuffer = '';
  let errorOutput = '';
  let ready = false;
  let resolveReady: (() => void) | undefined;
  let rejectReady: ((error: Error) => void) | undefined;
  const readyPromise = new Promise<void>((resolve, reject) => {
    resolveReady = resolve;
    rejectReady = reject;
  });

  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (chunk: string) => {
    outputBuffer += chunk;
    while (true) {
      const newline = outputBuffer.indexOf('\n');
      if (newline < 0) {
        break;
      }
      const line = outputBuffer.slice(0, newline).trimEnd();
      outputBuffer = outputBuffer.slice(newline + 1);
      if (line === 'ready') {
        ready = true;
        resolveReady?.();
        continue;
      }
      const event = parseX11MapEvent(line);
      if (event !== undefined) {
        recordedEvents.push(event);
        continue;
      }
      if (line.startsWith('barrier ')) {
        barriers.get(line)?.resolve();
        barriers.delete(line);
        continue;
      }
      const responseSeparator = line.indexOf('\t');
      if (responseSeparator >= 0) {
        const command = line.slice(0, responseSeparator);
        const request = requests.get(command);
        if (request !== undefined) {
          request.resolve(line.slice(responseSeparator + 1));
          requests.delete(command);
        }
      }
    }
  });
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', (chunk: string) => {
    errorOutput += chunk;
  });
  child.on('error', (error) => {
    if (!ready) {
      rejectReady?.(error);
    }
    for (const barrier of barriers.values()) {
      barrier.reject(error);
    }
    barriers.clear();
    for (const request of requests.values()) {
      request.reject(error);
    }
    requests.clear();
  });
  child.on('exit', (code, signal) => {
    const error = new Error(
      `X11 map recorder exited unexpectedly: code=${String(code)}, signal=${String(signal)}, stderr=${errorOutput.trim()}`
    );
    if (!ready) {
      rejectReady?.(error);
    }
    for (const barrier of barriers.values()) {
      barrier.reject(error);
    }
    barriers.clear();
    for (const request of requests.values()) {
      request.reject(error);
    }
    requests.clear();
  });

  await readyPromise;
  const request = async (name: string): Promise<string> => {
    const command = `${name} ${nextRequestId}`;
    nextRequestId += 1;
    const completed = new Promise<string>((resolve, reject) => {
      requests.set(command, { resolve, reject });
    });
    child.stdin.write(`${command}\n`);
    return completed;
  };
  return {
    flush: async () => {
      const command = `barrier ${nextBarrierId}`;
      nextBarrierId += 1;
      const completed = new Promise<void>((resolve, reject) => {
        barriers.set(command, { resolve, reject });
      });
      child.stdin.write(`${command}\n`);
      await completed;
    },
    events: () => [...recordedEvents],
    focusCompetitor: async () => request('focus-competitor'),
    focusedWindow: async () => request('active-window'),
    grabHotkey: async (hotkey) => {
      const result = await request(
        `grab-hotkey ${hotkey.key} ${x11ModifierMask(hotkey.modifiers)}`
      );
      if (result !== 'ok') {
        throw new Error(
          `Failed to reserve X11 hotkey ${hotkey.modifiers.join('+')}+${hotkey.key}`
        );
      }
    },
    stop: async () => {
      if (child.exitCode !== null || child.signalCode !== null) {
        return;
      }
      const exited = new Promise<void>((resolve, reject) => {
        child.once('error', reject);
        child.once('exit', (code, signal) => {
          if (code === 0) {
            resolve();
            return;
          }
          reject(
            new Error(
              `X11 map recorder failed to stop: code=${String(code)}, signal=${String(signal)}, stderr=${errorOutput.trim()}`
            )
          );
        });
      });
      child.stdin.write('quit\n');
      await exited;
    },
  };
};

/**
 * Asserts that an element has the expected gestament kind.
 *
 * @param element GTK element.
 * @param kind Expected kind.
 * @returns Typed GTK element.
 */
export const expectElementKind = <Kind extends GtkWidgetKind>(
  element: GtkWidgetElement,
  kind: Kind
): GtkElementOfKind<Kind> => {
  expect(element.kind).toBe(kind);
  return element as GtkElementOfKind<Kind>;
};

/**
 * Compares a launcher capture with a committed fixture image.
 *
 * @remarks Set `ELDER_TERMS_UPDATE_LAUNCHER_FIXTURES=1` to replace the
 * fixture with the current capture before comparing it.
 *
 * @param capture Actual launcher capture.
 * @param name Comparison and evidence name.
 * @param fixturePath Expected PNG path.
 * @param options Optional visual comparison tolerances.
 * @returns Promise resolved after the visual comparison passes.
 */
export const expectCaptureToMatchFixture = async (
  capture: GtkCapture,
  name: string,
  fixturePath: string,
  options?: GtkCaptureLookSimilarOptions
): Promise<void> => {
  if (process.env.ELDER_TERMS_UPDATE_LAUNCHER_FIXTURES === '1') {
    await mkdir(dirname(fixturePath), { recursive: true });
    await writeFile(fixturePath, capture.image);
  }
  if (!existsSync(fixturePath)) {
    throw new Error(`missing launcher capture fixture: ${fixturePath}`);
  }

  const visualExpect = createGtkCaptureExpect({
    outputResultPath: launcherTestResultsDirectory,
    variant: 'comparisons',
  });
  try {
    await visualExpect.expectCapture(capture, name).toLookSimilar(fixturePath, {
      maxDiffPixels: 0,
      threshold: 0.01,
      ...options,
    });
  } finally {
    await visualExpect.release();
  }
};

/**
 * Launches elder-terms with an isolated XDG connection directory.
 *
 * @param _context Vitest context reserved for evidence integration.
 * @param prepare Creates test profiles before launch.
 * @param body Test body.
 * @param options Additional process options.
 */
export const runLauncherGtkTest = async (
  _context: TestContext,
  prepare: (connections: string) => Promise<void>,
  body: (context: LauncherGtkTestContext) => Promise<void>,
  options: LauncherGtkTestOptions | undefined = undefined
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-gtk-'));
  const configHome = join(directory, 'config');
  const connections = join(configHome, 'elder-terms', 'connections');
  await mkdir(connections, { recursive: true });
  await prepare(connections);

  const launcher = createGtkAppLauncher({
    appPath: options?.appPath ?? defaultAppPath,
    env: {
      LANGUAGE: 'C',
      LC_ALL: 'C.UTF-8',
      XDG_CONFIG_HOME: configHome,
      ...options?.env,
    },
    xvfbPool: {
      type: 'xvfb',
    },
    xvfbTrayHost: options?.xvfbTrayHost ?? true,
  });
  let x11MapRecorder: X11MapRecorder | undefined;
  try {
    if (
      options?.recordX11Maps === true ||
      (options?.blockedHotkeys?.length ?? 0) > 0
    ) {
      x11MapRecorder = await startX11MapRecorder(await launcher.environment());
    }
    for (const hotkey of options?.blockedHotkeys ?? []) {
      await x11MapRecorder?.grabHotkey(hotkey);
    }
    const app = await launcher.launch([...(options?.args ?? [])]);
    await body({ app, configHome, connections, x11MapRecorder });
  } finally {
    await x11MapRecorder?.stop();
    await launcher.release();
    await rm(directory, { recursive: true, force: true });
  }
};
