import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

interface DesktopEntryExpectation {
  readonly applicationId: string;
  readonly executable: string;
  readonly hidden: boolean;
  readonly path: string;
  readonly wmClass: string;
}

const desktopEntries: readonly DesktopEntryExpectation[] = [
  {
    applicationId: 'net.kekyo.elder-terms',
    executable: 'elder-terms',
    hidden: false,
    path: fileURLToPath(
      new URL(
        '../packaging/applications/net.kekyo.elder-terms.desktop',
        import.meta.url
      )
    ),
    wmClass: 'Elder-terms',
  },
  {
    applicationId: 'net.kekyo.elder-terms-vte',
    executable: 'elder-terms-vte',
    hidden: true,
    path: fileURLToPath(
      new URL(
        '../../elder-terms-vte/packaging/applications/net.kekyo.elder-terms-vte.desktop',
        import.meta.url
      )
    ),
    wmClass: 'Elder-terms-vte',
  },
];

const readDesktopEntry = (path: string): ReadonlyMap<string, string> => {
  const values = new Map<string, string>();
  let inDesktopEntryGroup = false;
  for (const line of readFileSync(path, 'utf8').split(/\r?\n/u)) {
    if (line.startsWith('[')) {
      inDesktopEntryGroup = line === '[Desktop Entry]';
      continue;
    }
    if (!inDesktopEntryGroup) {
      continue;
    }
    const separator = line.indexOf('=');
    if (separator > 0) {
      values.set(line.slice(0, separator), line.slice(separator + 1));
    }
  }
  return values;
};

describe.each(desktopEntries)(
  '$applicationId desktop entry',
  ({ applicationId, executable, hidden, path, wmClass }) => {
    it('is valid and matches its runtime application identity', () => {
      const validation = spawnSync('desktop-file-validate', [path], {
        encoding: 'utf8',
      });
      expect(
        validation.status,
        `desktop-file-validate failed:\n${validation.stderr}`
      ).toBe(0);

      const entry = readDesktopEntry(path);
      expect(basename(path)).toBe(`${applicationId}.desktop`);
      expect(entry.get('Version')).toBe('1.5');
      expect(entry.get('Type')).toBe('Application');
      expect(entry.get('TryExec')).toBe(executable);
      expect(entry.get('Exec')).toBe(executable);
      expect(entry.get('Icon')).toBe('elder-terms');
      expect(entry.get('StartupNotify')).toBe('true');
      expect(entry.get('StartupWMClass')).toBe(wmClass);
      expect(entry.get('NoDisplay')).toBe(hidden ? 'true' : undefined);
    });
  }
);
