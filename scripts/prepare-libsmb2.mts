// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { spawn } from 'node:child_process';
import { mkdir, writeFile } from 'node:fs/promises';
import { availableParallelism } from 'node:os';
import { join, resolve } from 'node:path';

const [rootArgument, buildArgument] = process.argv.slice(2);
if (rootArgument === undefined || buildArgument === undefined)
  throw new Error('Usage: prepare-libsmb2 <source-directory> <build-directory>');
const root = resolve(rootArgument);
const output = resolve(buildArgument);
const build = join(output, 'libsmb2/build');
const sdk = join(output, 'libsmb2/sdk');
const run = async (command: string, args: string[]): Promise<void> => {
  const child = spawn(command, args, { stdio: 'inherit' });
  const code = await new Promise<number | null>((resolveExit, reject) => {
    child.once('error', reject);
    child.once('exit', resolveExit);
  });
  if (code !== 0) throw new Error(`${command} exited with ${code}`);
};

// Build the pinned submodule without changing its source. Headers and runtime
// must come from this same SDK; distribution-provided versions are not mixed in.
await mkdir(build, { recursive: true });
await run('cmake', [
  '-S', join(root, 'deps/libsmb2'), '-B', build,
  `-DCMAKE_INSTALL_PREFIX=${sdk}`, '-DCMAKE_INSTALL_LIBDIR=lib',
  '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON',
  '-DENABLE_LIBKRB5=OFF', '-DENABLE_GSSAPI=OFF', '-DENABLE_LIBDCERPC=OFF',
  '-DENABLE_LIBQUIC=OFF', '-DENABLE_RDMA=OFF', '-DENABLE_UTILS=OFF',
  '-DENABLE_EXAMPLES=OFF',
]);
await run('cmake', ['--build', build, '--parallel', String(Math.min(availableParallelism(), 8))]);
await run('cmake', ['--install', build]);
await writeFile(join(output, 'libsmb2-build.stamp'), 'ready\n');
