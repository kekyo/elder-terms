import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { describe, expect, it } from 'vitest';

const execFileAsync = promisify(execFile);

const buildArtifactPath = (relativePath: string): string =>
  fileURLToPath(new URL(`../../.build/${relativePath}`, import.meta.url));

const inspectElf = async (
  program: 'nm' | 'readelf',
  args: readonly string[]
): Promise<string> => {
  const { stdout } = await execFileAsync(program, [...args], {
    env: {
      ...process.env,
      LC_ALL: 'C',
    },
  });
  return stdout;
};

describe('elder-terms shared library', () => {
  it('provides the shared runtime dependency with only public API symbols', async () => {
    const libraryPath = buildArtifactPath('shared/libelder-terms.so');
    const launcherPath = buildArtifactPath('elder-terms/elder-terms');
    const vtePath = buildArtifactPath('elder-terms-vte/elder-terms-vte');

    const [header, dynamic, launcherDynamic, vteDynamic, symbols] =
      await Promise.all([
        inspectElf('readelf', ['--file-header', libraryPath]),
        inspectElf('readelf', ['--dynamic', libraryPath]),
        inspectElf('readelf', ['--dynamic', launcherPath]),
        inspectElf('readelf', ['--dynamic', vtePath]),
        inspectElf('nm', [
          '--dynamic',
          '--defined-only',
          '--demangle',
          libraryPath,
        ]),
      ]);

    expect(header).toMatch(/Type:\s+DYN/);
    expect(dynamic).toContain('Library soname: [libelder-terms.so]');
    expect(launcherDynamic).toContain('Shared library: [libelder-terms.so]');
    expect(vteDynamic).toContain('Shared library: [libelder-terms.so]');
    expect(symbols).toContain('elder_terms::create_settings_widget(');
    expect(symbols).toContain('elder_terms::load_settings(');
    expect(symbols).not.toContain('elder_terms::SettingsWidgetState::');
    expect(
      symbols
        .split('\n')
        .filter(
          (symbol) =>
            symbol.length > 0 && !/\s[A-Za-z]\s+elder_terms::/.test(symbol)
        )
    ).toEqual([]);
  });
});
