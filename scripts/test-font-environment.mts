import { isAbsolute, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

/** Absolute build-owned directory containing prepared fonts and Fontconfig caches. */
export const testFontDirectory = fileURLToPath(
  new URL('../.build/test-fonts/', import.meta.url)
);

/**
 * Supplies the prepared Fontconfig configuration to tests and their child builds.
 * @returns Environment entries preserving the original configuration separately.
 */
export const getTestFontEnvironment = (): Record<string, string> => {
  const configuration = join(testFontDirectory, 'fonts.conf');
  const original =
    process.env.ELDER_TERMS_TEST_FONTCONFIG_BASE ??
    process.env.FONTCONFIG_FILE ??
    '/etc/fonts/fonts.conf';
  // Fontconfig resolves a bare filename relative to FONTCONFIG_PATH, not cwd.
  const base = isAbsolute(original)
    ? original
    : resolve(
        process.env.FONTCONFIG_PATH?.split(':')[0] ?? '/etc/fonts',
        original
      );
  if (base === configuration) {
    throw new Error(
      'The prepared Fontconfig configuration cannot include itself; preserve ELDER_TERMS_TEST_FONTCONFIG_BASE'
    );
  }
  return {
    FONTCONFIG_FILE: configuration,
    ELDER_TERMS_TEST_FONTCONFIG_BASE: base,
  };
};
