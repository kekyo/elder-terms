// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFileSync, spawn } from 'node:child_process';
import { cp, mkdir, rm } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const outputArgument = process.argv[2];
if (outputArgument === undefined)
  throw new Error('Usage: prepare-package-tools.mts <output-directory>');
const output = resolve(outputArgument);
const upstream = join(root, 'deps/boxrig');

const run = async (
  command: string,
  args: string[],
  cwd: string
): Promise<void> => {
  const child = spawn(command, args, { cwd, stdio: 'inherit' });
  const code = await new Promise<number | null>((resolveExit, reject) => {
    child.once('error', reject);
    child.once('exit', resolveExit);
  });
  if (code !== 0) throw new Error(`${command} exited with ${code}`);
};

// Containers use their distribution's Node.js without loading host native addons.
await mkdir(output, { recursive: true });
await run(
  process.execPath,
  [
    join(root, 'node_modules/typescript/bin/tsc'),
    join(root, 'scripts/prepare-boxrig.mts'),
    join(root, 'scripts/prepare-jemalloc.mts'),
    '--module',
    'NodeNext',
    '--target',
    'ES2022',
    '--strict',
    '--skipLibCheck',
    '--noEmitOnError',
    '--types',
    'node',
    '--outDir',
    output,
  ],
  root
);
await run('npm', ['ci', '--prefix', 'tools/codegen'], upstream);
await run(
  'make',
  [
    '-f',
    'tools/codegen/rules.mk',
    'package-codegen',
    `BUILD_DIR=${join(output, 'boxrig')}`,
    'BOXRIG_VERSION=0.10.0',
  ],
  upstream
);
const includeDirectory = execFileSync(
  join(root, 'node_modules/.bin/gestament-config'),
  ['--includedir'],
  { encoding: 'utf8' }
).trim();
await cp(includeDirectory, join(output, 'include'), { recursive: true });

const renderedIcons = join(output, 'material-presets-rendered');
await run(
  process.execPath,
  [
    join(root, 'scripts/render-material-connection-icons.mts'),
    join(root, 'data/connection-icons/material-svg'),
    join(root, 'shared/connection-icon-material.gresource.xml'),
    join(output, 'elder-terms-material-presets.c'),
    renderedIcons,
    'convert',
    'glib-compile-resources',
    'gdk-pixbuf-thumbnailer',
  ],
  root
);
await rm(renderedIcons, { recursive: true, force: true });
