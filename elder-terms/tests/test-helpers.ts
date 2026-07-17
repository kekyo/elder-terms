import { mkdir, mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { TestContext } from 'vitest';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkElementOfKind,
  type GtkWidgetElement,
  type GtkWidgetKind,
} from 'gestament';

const appPath = fileURLToPath(
  new URL('../../.build/elder-terms/elder-terms', import.meta.url)
);

/** Context passed to a launcher GTK integration test. */
export interface LauncherGtkTestContext {
  /** Running launcher application. */
  readonly app: GtkApp;
  /** Isolated XDG configuration root. */
  readonly configHome: string;
  /** Isolated connection profile directory. */
  readonly connections: string;
}

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
 * Launches elder-terms with an isolated XDG connection directory.
 *
 * @param _context Vitest context reserved for evidence integration.
 * @param prepare Creates test profiles before launch.
 * @param body Test body.
 */
export const runLauncherGtkTest = async (
  _context: TestContext,
  prepare: (connections: string) => Promise<void>,
  body: (context: LauncherGtkTestContext) => Promise<void>
): Promise<void> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-gtk-'));
  const configHome = join(directory, 'config');
  const connections = join(configHome, 'elder-terms', 'connections');
  await mkdir(connections, { recursive: true });
  await prepare(connections);

  const launcher = createGtkAppLauncher({
    appPath,
    env: {
      XDG_CONFIG_HOME: configHome,
    },
    xvfbTrayHost: false,
  });
  const app = await launcher.launch([]);
  try {
    await body({ app, configHome, connections });
  } finally {
    await launcher.release();
    await rm(directory, { recursive: true, force: true });
  }
};
