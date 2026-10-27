// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFileSync, spawn } from 'node:child_process';
import { closeSync, openSync } from 'node:fs';
import { access, mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [peer, input, driver] = process.argv.slice(2);
if (!peer || !input || !driver)
  throw new Error('Expected peer, input and driver paths');
const captureMode = process.argv[5] ?? 'video';
const loadMode = process.argv[6] ?? 'idle';
const initialHistory = process.argv[7] ?? '0';
const stopAfter = process.argv[8] ?? '0';
const directory = await mkdtemp(join(tmpdir(), 'elder-latency-gui-'));
const report = join(directory, 'child.tsv');
const log = openSync(join(directory, 'driver.log'), 'w');
const app = spawn(
  driver,
  ['--local', '--', peer, report, initialHistory, 'plain', loadMode, '10', '4'],
  {
    env: { ...process.env, GDK_GL: 'disable', NO_AT_BRIDGE: '1' },
    stdio: ['ignore', log, log],
  }
);
closeSync(log);
const pause = (time) => new Promise((resolve) => setTimeout(resolve, time));
const run = async (program, args) => {
  const child = spawn(program, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  let stdout = '';
  let stderr = '';
  child.stdout.on('data', (bytes) => {
    stdout += bytes.toString();
  });
  child.stderr.on('data', (bytes) => {
    stderr += bytes.toString();
  });
  const result = await new Promise((resolve) => child.on('close', resolve));
  if (result !== 0) throw new Error(`${program} exited ${result}: ${stderr}`);
  return stdout;
};

try {
  let window = '';
  for (let attempt = 0; attempt < 200; attempt++) {
    if (app.exitCode !== null) throw new Error(`Driver exited ${app.exitCode}`);
    try {
      const result = await run('xdotool', [
        'search',
        '--onlyvisible',
        '--name',
        'elder-latency:ready',
      ]);
      window = result.trim().split('\n').at(-1) ?? '';
      if (window) break;
    } catch {
      /* The title appears after the PTY history has been parsed. */
    }
    await pause(50);
  }
  if (!window) throw new Error('The ready title did not appear');
  await run(input, [window, directory, '3', stopAfter, captureMode, 'window']);
  for (let attempt = 0; attempt < 200; attempt++) {
    try {
      await access(report);
      break;
    } catch {
      await pause(50);
    }
  }
  const child = (await readFile(report, 'utf8')).trim().split('\n');
  const events = (await readFile(join(directory, 'events.tsv'), 'utf8'))
    .trim()
    .split('\n');
  const frames = (await readFile(join(directory, 'frames.tsv'), 'utf8'))
    .trim()
    .split('\n')
    .slice(1);
  if (loadMode === 'transcript') {
    const geometry = await run('xdotool', [
      'getwindowgeometry',
      '--shell',
      window,
    ]);
    const height = Number(/^HEIGHT=(\d+)$/m.exec(geometry)?.[1]);
    const markerY = Number(frames[0]?.split('\t')[5]);
    if (!Number.isFinite(height) || !(markerY > height / 2))
      throw new Error(
        `Transcript marker was not kept in the footer: ${markerY}/${height}`
      );
  }
  if (child.length !== 4 || events.length !== 4 || frames.length < 3)
    throw new Error('Missing measured input or display frames');
  for (let index = 1; index <= 3; index++) {
    const [, received, written] = child[index].split('\t').map(Number);
    const [, sent, visible] = events[index].split('\t').map(Number);
    if (!(sent <= received && received <= written && written <= visible))
      throw new Error(
        `Invalid sequence ${index}: ${events[index]} / ${child[index]}`
      );
  }
  await run('ffmpeg', [
    '-nostdin',
    '-v',
    'error',
    '-y',
    '-f',
    'concat',
    '-safe',
    '0',
    '-i',
    join(directory, 'video.ffconcat'),
    '-c:v',
    'ffv1',
    '-pix_fmt',
    'bgr0',
    '-fps_mode',
    'vfr',
    '-enc_time_base',
    'demux',
    join(directory, 'video.mkv'),
  ]);
  const stream = await run('ffprobe', [
    '-v',
    'error',
    '-select_streams',
    'v:0',
    '-count_frames',
    '-show_entries',
    'stream=nb_read_frames',
    '-of',
    'default=nw=1:nk=1',
    join(directory, 'video.mkv'),
  ]);
  if (Number(stream.trim()) < frames.length - 1)
    throw new Error(
      `Video omitted observed frames: ${stream.trim()} vs ${frames.length}`
    );
  const firstImage = await readFile(join(directory, 'frame-00000.ppm'));
  const header = /^P6\n(\d+) (\d+)\n255\n/.exec(
    firstImage.toString('latin1', 0, 64)
  );
  if (!header) throw new Error('Invalid recorded frame');
  const width = Number(header[1]);
  const height = Number(header[2]);
  const decoded = execFileSync(
    'ffmpeg',
    [
      '-nostdin',
      '-v',
      'error',
      '-i',
      join(directory, 'video.mkv'),
      '-fps_mode',
      'passthrough',
      '-f',
      'rawvideo',
      '-pix_fmt',
      'rgb24',
      'pipe:1',
    ],
    { maxBuffer: 64 * 1024 * 1024 }
  );
  const frameSize = width * height * 3;
  const decodedFrames = decoded.length / frameSize;
  if (!Number.isInteger(decodedFrames) || decodedFrames < frames.length - 1)
    throw new Error(
      `Decoded video omitted recorded frames: ${decodedFrames} vs ${frames.length}`
    );
  const seenColors = [];
  for (let index = 0; index < Math.min(frames.length, decodedFrames); index++) {
    const expected = parseInt(frames[index].split('\t')[3], 16);
    const cells = frames[index].split('\t');
    const sampleX = Number(cells[6] ?? Math.floor(width / 2));
    const sampleY = Number(cells[7] ?? Math.floor(height / 2));
    const pixel = index * frameSize + (sampleY * width + sampleX) * 3;
    const actual =
      (decoded[pixel] << 16) | (decoded[pixel + 1] << 8) | decoded[pixel + 2];
    if (actual !== expected)
      throw new Error(
        `Video pixel mismatch at frame ${index}: ${actual.toString(16)} vs ${expected.toString(16)}`
      );
    if (seenColors.at(-1) !== actual) seenColors.push(actual);
  }
  if (seenColors[0] !== 0x204060) throw new Error('Video omitted ready marker');
  let previousIndex = 0;
  for (let sequence = 1; sequence <= 3; sequence++) {
    const expected = ((64 + sequence) << 16) | (80 << 8) | 80;
    const found = seenColors.indexOf(expected);
    if (found <= previousIndex)
      throw new Error(`Video did not show key ${sequence} in order`);
    previousIndex = found;
  }
  console.log('Latency input and video protocol passed');
} finally {
  if (app.exitCode === null && app.signalCode === null) {
    app.kill('SIGTERM');
    await new Promise((resolve) => app.once('close', resolve));
  }
  await rm(directory, { recursive: true, force: true });
}
