// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import { mkdtemp, rm, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const projectRoot = fileURLToPath(new URL('../..', import.meta.url));

it('compiles the SMB settings editor with release optimizations and warnings as errors', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-smb-editor-build-'));
  try {
    const { stdout } = await execute('pkg-config', ['--cflags', 'gtk+-3.0']);
    const object = join(directory, 'smb-settings-editor.o');
    await execute(process.env.CXX ?? 'c++', [
      '-std=c++20',
      '-O3',
      '-Wall',
      '-Wextra',
      '-Wpedantic',
      '-Werror',
      '-fPIC',
      `-I${join(projectRoot, 'shared/include')}`,
      ...stdout.trim().split(/\s+/),
      '-c',
      join(projectRoot, 'shared/src/settings-widget/smb-settings-editor.cpp'),
      '-o',
      object,
    ]);
    expect((await stat(object)).size).toBeGreaterThan(0);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
