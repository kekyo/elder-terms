// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);
const execFileAsync = promisify(execFile);

describe('boxrig input policies', () => {
  for (const scenario of [
    {
      name: 'automatic CR',
      options: ['--return', 'auto'],
      setup: '',
      keys: ['Return'],
      expected: '0d',
    },
    {
      name: 'automatic LNM',
      options: ['--return', 'auto'],
      setup: '\\033[20h',
      keys: ['Return'],
      expected: '0d0a',
    },
    {
      name: 'fixed LF with LNM',
      options: ['--return', 'lf'],
      setup: '\\033[20h',
      keys: ['Return'],
      expected: '0a',
    },
    {
      name: 'fixed CRLF',
      options: ['--return', 'crlf'],
      setup: '',
      keys: ['Return'],
      expected: '0d0a',
    },
    {
      name: 'explicit BS',
      options: ['--backspace', 'bs'],
      setup: '',
      keys: ['BackSpace'],
      expected: '08',
    },
    {
      name: 'explicit DEL',
      options: ['--backspace', 'del'],
      setup: '',
      keys: ['BackSpace'],
      expected: '7f',
    },
    {
      name: 'TRS-80 arrows',
      options: ['--trs80'],
      setup: '\\033[?1h',
      keys: ['Up', 'Down', 'Left', 'Right'],
      expected: '1e1f1d1c',
    },
    {
      name: 'Kitty Enter priority',
      options: ['--return', 'lf'],
      setup: '\\033[>8u',
      keys: ['Return'],
      expected: '1b5b313375',
    },
    {
      name: 'application keypad priority',
      options: ['--return', 'lf'],
      setup: '\\033=',
      keys: ['KP_Enter'],
      expected: '1b4f4d',
    },
  ]) {
    it(`sends ${scenario.name} through the core encoder`, async () => {
      const directory = await mkdtemp(join(tmpdir(), 'boxrig-input-'));
      const destination = join(directory, 'received');
      const expected = Buffer.from(scenario.expected, 'hex');
      const launcher = createGtkAppLauncher({
        appPath: driverPath,
        args: [
          '--local',
          ...scenario.options,
          '--',
          '/bin/sh',
          '-c',
          `stty raw -echo; printf 'KEY_READY\\r\\n${scenario.setup}'; dd bs=1 count=${expected.length} iflag=fullblock of='${destination}' 2>/dev/null`,
        ],
      });
      try {
        const app = await launcher.launch();
        await toPass(async () => {
          const raw = Buffer.concat(
            [...(await app.output()).stdout.matchAll(/^raw=([^\n]*)/gm)].map(
              (match) => Buffer.from(match[1], 'base64')
            )
          );
          expect(raw.toString()).toContain('KEY_READY');
        });
        const terminal = await app.getById('driver_terminal');
        const bounds = (await terminal.capture()).bounds;
        await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        for (const key of scenario.keys) await app.input.pressKey(key);
        await toPass(async () =>
          expect(await readFile(destination)).toEqual(expected)
        );
      } finally {
        await launcher.release();
        await rm(directory, { recursive: true, force: true });
      }
    });
  }

  it('exchanges arrow settings without replacing the display or connection', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-settings-'));
    const destination = join(directory, 'received');
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: [
        '--local',
        '--',
        '/bin/sh',
        '-c',
        `stty raw -echo; printf 'PERSISTED\\r\\n'; dd bs=1 count=7 iflag=fullblock of='${destination}' 2>/dev/null`,
      ],
    });
    try {
      const app = await launcher.launch();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain('input_pending=0')
      );
      const terminal = await app.getById('driver_terminal');
      const bounds = (await terminal.capture()).bounds;
      await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await app.input.pressKey('Up');
      for (const id of ['driver_trs80', 'driver_default_input']) {
        const button = await app.getById(id);
        if (button.kind !== 'button')
          throw new Error('Input settings button is missing');
        await button.click();
        await toPass(async () =>
          expect(
            (await app.output()).stdout.trimEnd().split('\n').pop()
          ).toContain('input_pending=0')
        );
        await app.input.pressKey('Up');
      }
      await toPass(async () =>
        expect(await readFile(destination)).toEqual(
          Buffer.from('\x1b[A\x1e\x1b[A')
        )
      );
      const lines = (await app.output()).stdout.split('\n');
      const snapshot =
        lines.filter((line) => line.includes(' text=')).pop() ?? '';
      expect(
        Buffer.from(/ text=(.*)/.exec(snapshot)?.[1] ?? '', 'base64').toString()
      ).toContain('PERSISTED');
      expect((await app.output()).stdout).not.toContain('generation=2 spawn=');
    } finally {
      await launcher.release();
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('discards input while inhibited and resumes with a fresh input generation', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-inhibit-'));
    const destination = join(directory, 'received');
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: [
        '--local',
        '--',
        '/bin/sh',
        '-c',
        `stty raw -echo; printf 'PAUSE_READY\\r\\n'; dd bs=1 count=2 iflag=fullblock of='${destination}' 2>/dev/null`,
      ],
    });
    try {
      const app = await launcher.launch();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain('connection=connected')
      );
      const pause = await app.getById('driver_pause_input');
      const resume = await app.getById('driver_resume_input');
      if (pause.kind !== 'button' || resume.kind !== 'button')
        throw new Error('Input controls are missing');
      const window = await app.getById('driver_window');
      if (window.kind !== 'window') throw new Error('Driver window is missing');
      await window.activate();
      await pause.click();
      await toPass(async () =>
        expect(
          (await app.output()).stdout.trimEnd().split('\n').pop()
        ).toContain('input=2')
      );
      const terminal = await app.getById('driver_terminal');
      const bounds = (await terminal.capture()).bounds;
      const metrics = /cell=(\d+)x(\d+)/.exec((await app.output()).stdout);
      if (!metrics) throw new Error('Cell metrics are missing');
      const width = Number(metrics[1]);
      const height = Number(metrics[2]);
      await app.input.moveMouseTo(
        bounds.x + Math.trunc(width / 2),
        bounds.y + Math.trunc(height / 2)
      );
      await app.input.setMouseButton('left', true);
      await app.input.moveMouseTo(
        Math.trunc(bounds.x + width * 10.5),
        bounds.y + Math.trunc(height / 2)
      );
      await app.input.setMouseButton('left', false);
      const copy = await app.getById('driver_copy');
      if (copy.kind !== 'button') throw new Error('Copy control is missing');
      await copy.click();
      await toPass(async () => {
        const { stdout } = await execFileAsync(
          fileURLToPath(
            new URL(
              '../../.build/elder-terms-terminal/clipboard-read-helper',
              import.meta.url
            )
          ),
          [],
          { env: await app.environment() }
        );
        expect(stdout).toBe('PAUSE_READY');
      });
      for (const key of ['a', 'Return']) await app.input.pressKey(key);
      await resume.click();
      await toPass(async () =>
        expect(
          (await app.output()).stdout.trimEnd().split('\n').pop()
        ).toContain('input=0')
      );
      for (const key of ['b', 'Return']) await app.input.pressKey(key);
      await toPass(async () =>
        expect(await readFile(destination)).toEqual(Buffer.from('b\r'))
      );
    } finally {
      await launcher.release();
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('keeps a settings exchange pending until a held key is released', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-busy-'));
    const destination = join(directory, 'received');
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: [
        '--local',
        '--',
        '/bin/sh',
        '-c',
        `stty raw -echo; printf 'BUSY_READY\\r\\n'; dd bs=1 count=1 of='${destination}' 2>/dev/null`,
      ],
    });
    try {
      const app = await launcher.launch();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain('connection=connected')
      );
      const terminal = await app.getById('driver_terminal');
      const bounds = (await terminal.capture()).bounds;
      await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await app.input.setModifier('control', true);
      await app.input.pressKey('F12');
      await toPass(async () =>
        expect(
          (await app.output()).stdout.trimEnd().split('\n').pop()
        ).toContain('input_pending=1')
      );
      expect(await readFile(destination)).toHaveLength(0);
      await app.input.setModifier('control', false);
      await toPass(async () =>
        expect(
          (await app.output()).stdout.trimEnd().split('\n').pop()
        ).toContain('input_pending=0')
      );
      await app.input.pressKey('Up');
      await toPass(async () =>
        expect(await readFile(destination)).toEqual(Buffer.from([0x1e]))
      );
    } finally {
      await launcher.release();
      await rm(directory, { recursive: true, force: true });
    }
  });
});
