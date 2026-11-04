// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cp, mkdir, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';

const [sourceArgument, buildArgument] = process.argv.slice(2);
if (sourceArgument === undefined || buildArgument === undefined)
  throw new Error('Usage: prepare-libdsm <source-directory> <build-directory>');
const root = resolve(sourceArgument);
const output = resolve(buildArgument);
const build = join(output, 'libdsm/build');
const sdk = join(output, 'libdsm/sdk');
const source = join(output, 'libdsm/source');
const run = async (command: string, args: string[], cwd: string): Promise<void> => {
  const child = spawn(command, args, { cwd, stdio: 'inherit' });
  const code = await new Promise<number | null>((resolveExit, reject) => {
    child.once('error', reject);
    child.once('exit', resolveExit);
  });
  if (code !== 0) throw new Error(`${command} exited with ${code}`);
};

// The approved libdsm fixes live in a reviewable patch. Apply them only to a
// build-local copy; package the exact corresponding source, including fixes.
const patch = join(root, 'patches/libdsm-correctness.patch');
const hash = createHash('sha256').update(await readFile(patch));
const hashSource = async (directory: string): Promise<void> => {
  for (const entry of (await readdir(directory, { withFileTypes: true }))
    .sort((a, b) => a.name.localeCompare(b.name))) {
    if (entry.name === '.git') continue;
    hash.update(entry.name).update('\0');
    const path = join(directory, entry.name);
    if (entry.isDirectory()) await hashSource(path);
    else hash.update(await readFile(path));
  }
};
await hashSource(join(root, 'deps/libdsm'));
const signature = hash.digest('hex');
const stamp = join(output, 'libdsm/source.stamp');
let previous = '';
try { previous = await readFile(stamp, 'utf8'); }
catch (error) { if ((error as NodeJS.ErrnoException).code !== 'ENOENT') throw error; }
if (previous !== signature) {
  await rm(build, { recursive: true, force: true });
  await rm(source, { recursive: true, force: true });
  await cp(join(root, 'deps/libdsm'), source, {
    recursive: true, filter: (path) => !path.endsWith('/.git'),
  });
  await run('patch', ['-p1', '--batch', '--forward', '-i', patch], source);
  await writeFile(stamp, signature);
}
await mkdir(build, { recursive: true });
await run('meson', ['setup', '--reconfigure', build, source,
  `--prefix=${sdk}`, '--libdir=lib', '-Ddefault_library=shared', '-Dbinaries=false', '-Dwerror=true'], root);
await run('meson', ['compile', '-C', build, 'dsm'], root);
await run('meson', ['install', '-C', build, '--no-rebuild', '--tags=runtime,devel'], root);
await run('tar', ['--exclude=.git', '--sort=name', '--mtime=@0', '--owner=0',
  '--group=0', '--numeric-owner', '-czf', join(sdk, 'libdsm-source.tar.gz'),
  '-C', join(output, 'libdsm'), 'source'], root);
await writeFile(join(output, 'libdsm-build.stamp'), 'ready\n');
