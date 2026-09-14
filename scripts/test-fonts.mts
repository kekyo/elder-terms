import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  mkdir,
  mkdtemp,
  readFile,
  rename,
  rm,
  writeFile,
} from 'node:fs/promises';
import { basename, join } from 'node:path';
import { promisify } from 'node:util';
import { buildCandidateVariables, runScriptOnceToText } from 'funcity';

/** A pinned download used to prepare a test font. */
export interface FontDownload {
  /** Resource URL. */
  readonly url: string;
  /** SHA-256 digest of the complete resource. */
  readonly sha256: string;
}

/** A font and its license, either standalone or in a Debian package. */
export type TestFontSource = {
  /** Exact Fontconfig family required by the tests. */
  readonly family: string;
  /** Pinned font or package download. */
  readonly download: FontDownload;
} & (
  | {
      /** A standalone font file. */
      readonly format: 'font';
      /** Filename including the font format extension. */
      readonly filename: string;
      /** Pinned license download. */
      readonly license: FontDownload;
    }
  | {
      /** A Debian package containing the font and its license. */
      readonly format: 'deb';
      /** Relative font path inside the package. */
      readonly fontPath: string;
      /** Relative copyright/license path inside the package. */
      readonly licensePath: string;
    }
);

/** Inputs for preparing a private font environment. */
export interface TestFontOptions {
  /** Absolute path to the build-owned font directory. */
  readonly directory: string;
  /** Absolute path to the existing Fontconfig configuration to include. */
  readonly baseConfig: string;
  /** Fonts to reuse or download when absent. */
  readonly sources: readonly TestFontSource[];
}

/**
 * Makes required fonts available without changing the host font store.
 * @param options Private directory, base configuration and pinned font sources.
 * @returns Path to the Fontconfig configuration for test processes.
 */
export const prepareTestFonts = async (
  options: TestFontOptions
): Promise<string> => {
  await mkdir(options.directory, { recursive: true });
  const cache = join(options.directory, 'cache');
  await mkdir(cache, { recursive: true });
  const configuration = join(options.directory, 'fonts.conf');
  const directories: string[] = [];
  await writeConfiguration(
    configuration,
    options.baseConfig,
    cache,
    directories
  );
  for (const source of options.sources) {
    if (await matchesFamily(configuration, source.family)) continue;
    const key = digest(Buffer.from(JSON.stringify(source)));
    const directory = join(options.directory, 'fonts', key);
    const filename =
      source.format === 'font' ? source.filename : basename(source.fontPath);
    if (!(await validCache(directory, filename))) {
      const staging = await mkdtemp(join(options.directory, '.download-'));
      try {
        console.log(`Preparing test font: ${source.family}`);
        const download = await downloadResource(source.download);
        let font: Buffer;
        let license: Buffer;
        if (source.format === 'font') {
          font = download;
          license = await downloadResource(source.license);
        } else {
          const archive = join(staging, 'font.deb');
          const extracted = join(staging, 'extracted');
          await writeFile(archive, download);
          // Extract package data only; no installation or maintainer scripts run.
          await execute('dpkg-deb', ['--extract', archive, extracted]);
          font = await readFile(join(extracted, source.fontPath));
          license = await readFile(join(extracted, source.licensePath));
        }
        const ready = join(staging, 'ready');
        await mkdir(ready);
        const path = join(ready, filename);
        await writeFile(path, font);
        const query = await execute('fc-query', ['-f', '%{family}\n', path]);
        if (!query.stdout.split(/[\n,]/).includes(source.family)) {
          throw new Error('Downloaded font family does not match');
        }
        await writeFile(join(ready, 'LICENSE'), license);
        await writeFile(
          join(ready, 'receipt.json'),
          JSON.stringify({
            font: digest(font),
            license: digest(license),
          })
        );
        await mkdir(join(options.directory, 'fonts'), { recursive: true });
        await rm(directory, { recursive: true, force: true });
        await rename(ready, directory);
        // Do not reuse a font scan of the replaced directory on coarse-mtime filesystems.
        await rm(cache, { recursive: true, force: true });
        await mkdir(cache, { recursive: true });
      } catch (error) {
        throw new Error(
          `${source.family}: ${error instanceof Error ? error.message : String(error)}`
        );
      } finally {
        await rm(staging, { recursive: true, force: true });
      }
    }
    directories.push(directory);
    await writeConfiguration(
      configuration,
      options.baseConfig,
      cache,
      directories
    );
    if (!(await matchesFamily(configuration, source.family))) {
      throw new Error(
        `${source.family}: prepared font family is not available`
      );
    }
  }
  return configuration;
};

const execute = promisify(execFile);
const digest = (bytes: Buffer): string =>
  createHash('sha256').update(bytes).digest('hex');
const escapeXml = (value: string): string =>
  value.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

const writeConfiguration = async (
  path: string,
  base: string,
  cache: string,
  directories: readonly string[]
): Promise<void> => {
  // The first writable cache wins, so place the private cache before the base.
  const text = await runScriptOnceToText(
    '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd"><fontconfig><cachedir>{{cache}}</cachedir><include>{{base}}</include>{{for directory directories}}<dir>{{directory}}</dir>{{end}}</fontconfig>',
    {
      sourceId: 'test-fonts.conf',
      variables: buildCandidateVariables({
        base: escapeXml(base),
        cache: escapeXml(cache),
        directories: directories.map(escapeXml),
      }),
    }
  );
  if (text === undefined)
    throw new Error('Could not generate test Fontconfig configuration');
  await writeFile(`${path}.tmp`, text);
  await rename(`${path}.tmp`, path);
};

const matchesFamily = async (
  configuration: string,
  family: string
): Promise<boolean> => {
  const match = await execute('fc-match', ['-f', '%{family}', family], {
    env: { ...process.env, FONTCONFIG_FILE: configuration },
  });
  // fc-match silently substitutes another family when the requested one is absent.
  return match.stdout.split(',').includes(family);
};

const validCache = async (
  directory: string,
  filename: string
): Promise<boolean> => {
  try {
    const receipt = JSON.parse(
      await readFile(join(directory, 'receipt.json'), 'utf8')
    ) as {
      font: string;
      license: string;
    };
    const font = await readFile(join(directory, filename));
    const license = await readFile(join(directory, 'LICENSE'));
    return digest(font) === receipt.font && digest(license) === receipt.license;
  } catch {
    return false;
  }
};

const downloadResource = async (source: FontDownload): Promise<Buffer> => {
  const response = await fetch(source.url, {
    signal: AbortSignal.timeout(240_000),
  });
  if (!response.ok) {
    await response.body?.cancel();
    throw new Error(`HTTP ${response.status} downloading ${source.url}`);
  }
  const bytes = Buffer.from(await response.arrayBuffer());
  if (digest(bytes) !== source.sha256)
    throw new Error(`SHA-256 mismatch for ${source.url}`);
  return bytes;
};
