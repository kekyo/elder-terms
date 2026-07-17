import { spawn, type ChildProcess } from 'node:child_process';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { constants } from 'node:fs';
import { access, chmod, mkdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import type { GtkApp, GtkToggleButtonElement } from 'gestament';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import { expectElementKind, type TestEvidence } from './test-helpers';
import {
  assertTerminalCaptureMatches,
  assertTransferProgressNoticeMatches,
  expectDisconnectedNoticeHidden,
  expectTransferProgressNoticeHidden,
  expectTransferProgressNoticeVisibleAtTerminalTopRight,
  readTransferProgressBarValue,
  runGtkTest,
  transferProgressNoticeXmodemReceivePath,
  transferProgressNoticeXmodemSendPath,
  transferProgressNoticeYmodemReceivePath,
  transferProgressNoticeYmodemSendPath,
  transferProgressNoticeZmodemReceivePath,
  transferProgressNoticeZmodemSendPath,
  transferTerminalDimPath,
  withTemporaryDirectory,
  xyzmPausePeerPath,
  type TransferProgressBarValue,
} from './gtk-test-helpers';

type TransferProtocol = 'xmodem' | 'ymodem' | 'zmodem';
type TransferDirection = 'send' | 'receive';

interface TransferFixture {
  readonly command: readonly string[];
  readonly configPath: string;
  readonly expectedPath: string;
  readonly loginScriptPath: string;
  readonly markerPath: string;
  readonly payload: Buffer;
  readonly payloadPath: string;
  readonly peerReleasePath?: string;
  readonly peerStatusPath?: string;
  readonly peerStderrPath?: string;
  readonly sourceUri: string | undefined;
  readonly transferBasePath: string;
}

interface TransferProgressPeerFixture extends TransferFixture {
  readonly peerStderrPath: string;
  readonly pauseRequestPath: string;
  readonly pausedPath: string;
  readonly resumePath: string;
}

interface BatchTransferFileFixture {
  readonly expectedPath: string;
  readonly name: string;
  readonly payload: Buffer;
  readonly payloadPath: string;
  readonly sourceUri: string | undefined;
}

interface BatchTransferFixture {
  readonly command: readonly string[];
  readonly configPath: string;
  readonly files: readonly BatchTransferFileFixture[];
  readonly loginScriptPath: string;
  readonly markerPath: string;
  readonly sourceUris: readonly string[];
  readonly transferBasePath: string;
}

interface ZmodemResumeFixture extends TransferFixture {
  readonly partialPath: string | undefined;
  readonly resumeOffset: number;
}

interface YmodemPostRbFixture extends TransferFixture {
  readonly postRbCaptureMarkerPath: string;
  readonly postRbInputPath: string;
}

interface TransferSizeCase {
  readonly byteLength: number;
  readonly label: string;
  readonly timeoutMs: number;
}

interface TransferConnectionStartOptions {
  readonly loginScriptPath: string;
}

interface TransferConnection {
  readonly close: () => Promise<void>;
  readonly port: number;
}

interface TransferConnectionCase {
  readonly name: string;
  readonly start: (
    options: TransferConnectionStartOptions
  ) => Promise<TransferConnection>;
}

interface TransferMenuCase {
  readonly direction: TransferDirection;
  readonly label: string;
  readonly menuItemId: string;
  readonly protocol: TransferProtocol;
}

interface TransferProgressNoticeCase {
  readonly fixturePath: string;
  readonly maxDiffPixels: number;
  readonly sizeCase: TransferSizeCase;
  readonly transferCase: TransferMenuCase;
}

const startDelaysMs = [250, 1000, 3000] as const;
const batchTransferStartDelayMs = 1_000;
const batchTransferTimeoutMs = 90_000;
const maxXmodemSendAdjustedDurationMs = 20_000;
const transferProgressNoticeStartDelayMs = 250;
const transferProgressNoticeLinkPaceMs = 1;
const ymodemPostRbStartDelayMs = 12_000;
const ymodemPostRbTimeoutMs = 150_000;
const transferDimTimeoutMs = 60_000;
const zmodemResumeOffset = 4 * 1024;
const zmodemResumeStartDelayMs = 1_000;
const zmodemResumeSizeCase: TransferSizeCase = {
  byteLength: 10 * 1024,
  label: '10KB',
  timeoutMs: 60_000,
};
const batchTransferFileCases = [
  {
    byteLength: 257,
    name: 'batch-a.bin',
  },
  {
    byteLength: 4097,
    name: 'batch-b.bin',
  },
  {
    byteLength: 32771,
    name: 'batch-c.bin',
  },
] as const;
const transferSizeCasesByProtocol: Record<
  TransferProtocol,
  readonly TransferSizeCase[]
> = {
  xmodem: [
    {
      byteLength: 160,
      label: '160B',
      timeoutMs: 60_000,
    },
    {
      byteLength: 16 * 1024,
      label: '16KB',
      timeoutMs: 90_000,
    },
    {
      byteLength: 480 * 1024,
      label: '480KB',
      timeoutMs: 120_000,
    },
  ],
  ymodem: [
    {
      byteLength: 320,
      label: '320B',
      timeoutMs: 60_000,
    },
    {
      byteLength: 32 * 1024,
      label: '32KB',
      timeoutMs: 90_000,
    },
    {
      byteLength: 960 * 1024,
      label: '960KB',
      timeoutMs: 120_000,
    },
  ],
  zmodem: [
    {
      byteLength: 10 * 1024,
      label: '10KB',
      timeoutMs: 60_000,
    },
    {
      byteLength: 1024 * 1024,
      label: '1MB',
      timeoutMs: 120_000,
    },
    {
      byteLength: 30 * 1024 * 1024,
      label: '30MB',
      timeoutMs: 360_000,
    },
  ],
};

const transferProgressNoticeDeterminateSizeCase: TransferSizeCase = {
  byteLength: 4 * 1024 * 1024,
  label: '4MB',
  timeoutMs: 120_000,
};

const transferProgressNoticeXmodemSizeCase: TransferSizeCase = {
  byteLength: 512 * 1024,
  label: '512KB',
  timeoutMs: 120_000,
};

const requiredCommands = [
  '/usr/bin/socat',
  '/usr/sbin/telnetd',
  '/usr/bin/rz',
  '/usr/bin/rb',
  '/usr/bin/rx',
  '/usr/bin/sz',
  '/usr/bin/sb',
  '/usr/bin/sx',
] as const;

const shellQuote = (value: string): string =>
  `'${value.split("'").join("'\\''")}'`;

const transferMenuCases: readonly TransferMenuCase[] = [
  {
    direction: 'send',
    label: 'zmodem send',
    menuItemId: 'transfer_zmodem_send_item',
    protocol: 'zmodem',
  },
  {
    direction: 'send',
    label: 'ymodem send',
    menuItemId: 'transfer_ymodem_send_item',
    protocol: 'ymodem',
  },
  {
    direction: 'send',
    label: 'xmodem 1k send',
    menuItemId: 'transfer_xmodem_1k_send_item',
    protocol: 'xmodem',
  },
  {
    direction: 'send',
    label: 'xmodem send',
    menuItemId: 'transfer_xmodem_send_item',
    protocol: 'xmodem',
  },
  {
    direction: 'receive',
    label: 'zmodem receive',
    menuItemId: 'transfer_zmodem_receive_item',
    protocol: 'zmodem',
  },
  {
    direction: 'receive',
    label: 'ymodem-g receive',
    menuItemId: 'transfer_ymodem_g_receive_item',
    protocol: 'ymodem',
  },
  {
    direction: 'receive',
    label: 'ymodem receive',
    menuItemId: 'transfer_ymodem_receive_item',
    protocol: 'ymodem',
  },
  {
    direction: 'receive',
    label: 'xmodem crc receive',
    menuItemId: 'transfer_xmodem_crc_receive_item',
    protocol: 'xmodem',
  },
  {
    direction: 'receive',
    label: 'xmodem receive',
    menuItemId: 'transfer_xmodem_receive_item',
    protocol: 'xmodem',
  },
];

const batchTransferMenuCases: readonly TransferMenuCase[] = [
  {
    direction: 'send',
    label: 'zmodem send batch',
    menuItemId: 'transfer_zmodem_send_item',
    protocol: 'zmodem',
  },
  {
    direction: 'send',
    label: 'ymodem send batch',
    menuItemId: 'transfer_ymodem_send_item',
    protocol: 'ymodem',
  },
  {
    direction: 'receive',
    label: 'zmodem receive batch',
    menuItemId: 'transfer_zmodem_receive_item',
    protocol: 'zmodem',
  },
  {
    direction: 'receive',
    label: 'ymodem receive batch',
    menuItemId: 'transfer_ymodem_receive_item',
    protocol: 'ymodem',
  },
  {
    direction: 'receive',
    label: 'ymodem-g receive batch',
    menuItemId: 'transfer_ymodem_g_receive_item',
    protocol: 'ymodem',
  },
];

const zmodemAutoStartSendCase: TransferMenuCase = {
  direction: 'send',
  label: 'zmodem auto-start send',
  menuItemId: 'transfer_zmodem_send_item',
  protocol: 'zmodem',
};

const zmodemAutoStartReceiveCase: TransferMenuCase = {
  direction: 'receive',
  label: 'zmodem auto-start receive',
  menuItemId: 'transfer_zmodem_receive_item',
  protocol: 'zmodem',
};

const transferDimCase: TransferMenuCase = {
  direction: 'receive',
  label: 'zmodem receive',
  menuItemId: 'transfer_zmodem_receive_item',
  protocol: 'zmodem',
};

const transferProgressNoticeCases: readonly TransferProgressNoticeCase[] = [
  {
    fixturePath: transferProgressNoticeZmodemSendPath,
    maxDiffPixels: 320,
    sizeCase: transferProgressNoticeDeterminateSizeCase,
    transferCase: {
      direction: 'send',
      label: 'zmodem send progress notice',
      menuItemId: 'transfer_zmodem_send_item',
      protocol: 'zmodem',
    },
  },
  {
    fixturePath: transferProgressNoticeZmodemReceivePath,
    maxDiffPixels: 320,
    sizeCase: transferProgressNoticeDeterminateSizeCase,
    transferCase: {
      direction: 'receive',
      label: 'zmodem receive progress notice',
      menuItemId: 'transfer_zmodem_receive_item',
      protocol: 'zmodem',
    },
  },
  {
    fixturePath: transferProgressNoticeYmodemSendPath,
    maxDiffPixels: 320,
    sizeCase: transferProgressNoticeDeterminateSizeCase,
    transferCase: {
      direction: 'send',
      label: 'ymodem send progress notice',
      menuItemId: 'transfer_ymodem_send_item',
      protocol: 'ymodem',
    },
  },
  {
    fixturePath: transferProgressNoticeYmodemReceivePath,
    maxDiffPixels: 320,
    sizeCase: transferProgressNoticeDeterminateSizeCase,
    transferCase: {
      direction: 'receive',
      label: 'ymodem receive progress notice',
      menuItemId: 'transfer_ymodem_receive_item',
      protocol: 'ymodem',
    },
  },
  {
    fixturePath: transferProgressNoticeXmodemSendPath,
    maxDiffPixels: 2_000,
    sizeCase: transferProgressNoticeXmodemSizeCase,
    transferCase: {
      direction: 'send',
      label: 'xmodem send progress notice',
      menuItemId: 'transfer_xmodem_1k_send_item',
      protocol: 'xmodem',
    },
  },
  {
    fixturePath: transferProgressNoticeXmodemReceivePath,
    maxDiffPixels: 2_000,
    sizeCase: transferProgressNoticeXmodemSizeCase,
    transferCase: {
      direction: 'receive',
      label: 'xmodem receive progress notice',
      menuItemId: 'transfer_xmodem_crc_receive_item',
      protocol: 'xmodem',
    },
  },
];

const zmodemResumeMenuCases: readonly TransferMenuCase[] = [
  {
    direction: 'send',
    label: 'zmodem resume send',
    menuItemId: 'transfer_zmodem_send_item',
    protocol: 'zmodem',
  },
  {
    direction: 'receive',
    label: 'zmodem resume receive',
    menuItemId: 'transfer_zmodem_receive_item',
    protocol: 'zmodem',
  },
];

const makeTransferPayload = (byteLength: number): Buffer =>
  randomBytes(byteLength);

const transferProgressPeerLinkPaceArgs = (
  transferCase: TransferMenuCase
): readonly string[] =>
  transferCase.protocol === 'xmodem' ||
  (transferCase.protocol === 'zmodem' && transferCase.direction === 'send')
    ? []
    : [
        '--link-pace-ms',
        String(transferProgressNoticeLinkPaceMs),
        '--link-pace-every',
        '4',
      ];

const corruptPrefix = (payload: Buffer, prefixLength: number): Buffer => {
  const corrupted = Buffer.from(payload);
  const limit = Math.min(prefixLength, corrupted.length);
  for (let index = 0; index < limit; index += 1) {
    corrupted[index] ^= 0xff;
  }
  return corrupted;
};

const transferSourceUriArgs = (
  sourceUris: readonly string[]
): readonly string[] =>
  sourceUris.map((sourceUri) => `--test-transfer-source-uri=${sourceUri}`);

const expectRequiredCommands = async (): Promise<void> => {
  await Promise.all(
    requiredCommands.map(async (command) => {
      await access(command, constants.X_OK);
    })
  );
};

const expectTransferProgressPeerAvailable = async (): Promise<void> => {
  await Promise.all([
    access(xyzmPausePeerPath, constants.X_OK),
    access('/usr/bin/mkfifo', constants.X_OK),
  ]);
};

const createFifo = async (path: string): Promise<void> => {
  const child = spawn('/usr/bin/mkfifo', [path], {
    stdio: 'ignore',
  });
  const [code, signal] = (await once(child, 'exit')) as [
    number | null,
    string | null,
  ];
  if (code !== 0) {
    throw new Error(`mkfifo failed for ${path}: code=${code} signal=${signal}`);
  }
};

const socatListeningPort = (stderr: string): number | undefined => {
  const match = /\blistening on\b.*\b127\.0\.0\.1:([0-9]+)\b/iu.exec(stderr);
  if (match === null) {
    return undefined;
  }

  return Number(match[1]);
};

const waitForSocatListening = async (
  child: ChildProcess,
  stderr: () => string
): Promise<number> =>
  waitForResult(
    async () => {
      if (child.exitCode !== null || child.signalCode !== null) {
        throw new Error(`socat exited before listening: ${stderr()}`);
      }

      const port = socatListeningPort(stderr());
      if (port === undefined) {
        throw new Error(`socat has not exposed a listening port: ${stderr()}`);
      }

      return port;
    },
    {
      message: 'socat should start listening for telnetd connections',
      timeoutMs: 5_000,
    }
  );

const stopProcessGroup = async (child: ChildProcess): Promise<void> => {
  if (child.pid === undefined) {
    return;
  }
  if (child.exitCode !== null || child.signalCode !== null) {
    return;
  }
  const pid = child.pid;

  try {
    process.kill(-pid, 'SIGTERM');
  } catch {
    return;
  }

  const timeout = setTimeout(() => {
    try {
      process.kill(-pid, 'SIGKILL');
    } catch {
      // The process group may have exited between the timeout and kill.
    }
  }, 2_000);
  try {
    await once(child, 'exit');
  } finally {
    clearTimeout(timeout);
  }
};

const startSocatGnuTelnetd = async (
  options: TransferConnectionStartOptions
): Promise<TransferConnection> => {
  const telnetdCommand = `/usr/sbin/telnetd -h -E ${shellQuote(
    options.loginScriptPath
  )}`;
  const child = spawn(
    '/usr/bin/socat',
    [
      '-d',
      '-d',
      'TCP-LISTEN:0,bind=127.0.0.1,reuseaddr,fork',
      `EXEC:${telnetdCommand}`,
    ],
    {
      detached: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    }
  );
  let stderr = '';

  child.stderr?.on('data', (chunk: Buffer) => {
    stderr += chunk.toString('utf8');
  });
  const port = await waitForSocatListening(child, () => stderr);

  return {
    close: async (): Promise<void> => {
      await stopProcessGroup(child);
    },
    port,
  };
};

const connectionCases: readonly TransferConnectionCase[] = [
  {
    name: 'telnetd',
    start: startSocatGnuTelnetd,
  },
];

const lrzszCommand = (
  transferCase: TransferMenuCase,
  fileName: string
): readonly string[] => {
  if (transferCase.direction === 'send') {
    if (transferCase.protocol === 'zmodem') {
      return ['/usr/bin/rz'];
    }
    if (transferCase.protocol === 'ymodem') {
      return ['/usr/bin/rb'];
    }
    return ['/usr/bin/rx', fileName];
  }

  if (transferCase.protocol === 'zmodem') {
    return ['/usr/bin/sz', fileName];
  }
  if (transferCase.protocol === 'ymodem') {
    if (transferCase.menuItemId === 'transfer_ymodem_g_receive_item') {
      return [
        '/bin/sh',
        '-c',
        `/usr/bin/sb --ymodem -b -q ${shellQuote(fileName)}; sleep 1`,
      ];
    }
    return ['/usr/bin/sb', '--ymodem', '-b', '-q', fileName];
  }
  return ['/usr/bin/sx', fileName];
};

const lrzszBatchCommand = (
  transferCase: TransferMenuCase,
  fileNames: readonly string[]
): readonly string[] => {
  if (transferCase.direction === 'send') {
    if (transferCase.protocol === 'zmodem') {
      return ['/usr/bin/rz'];
    }
    if (transferCase.protocol === 'ymodem') {
      return ['/usr/bin/rb'];
    }
    throw new Error(
      `unsupported batch send protocol: ${transferCase.protocol}`
    );
  }

  if (transferCase.protocol === 'zmodem') {
    return ['/usr/bin/sz', ...fileNames];
  }
  if (transferCase.protocol !== 'ymodem') {
    throw new Error(
      `unsupported batch receive protocol: ${transferCase.protocol}`
    );
  }
  if (transferCase.menuItemId === 'transfer_ymodem_g_receive_item') {
    return [
      '/bin/sh',
      '-c',
      `/usr/bin/sb --ymodem -b -q ${fileNames
        .map(shellQuote)
        .join(' ')}; sleep 1`,
    ];
  }
  return ['/usr/bin/sb', '--ymodem', '-b', '-q', ...fileNames];
};

const writeLoginScript = async (
  path: string,
  remoteDirectory: string,
  markerPath: string,
  command: readonly string[]
): Promise<void> => {
  const releasePath = `${markerPath}.release`;
  await createFifo(markerPath);
  await createFifo(releasePath);
  await writeFile(
    path,
    [
      '#!/bin/sh',
      `cd ${shellQuote(remoteDirectory)} || exit 1`,
      'stty raw -echo -ixon -ixoff -icanon min 1 time 0 2>/dev/null || true',
      `cat ${shellQuote(markerPath)} >/dev/null || exit 1`,
      `${command.map(shellQuote).join(' ')} 2>lrzsz.stderr`,
      'status=$?',
      `printf '%s\\n' "$status" >lrzsz.status`,
      `if [ "$status" -eq 0 ]; then cat ${shellQuote(
        releasePath
      )} >/dev/null || exit 1; fi`,
      'exit "$status"',
      '',
    ].join('\n'),
    'utf8'
  );
  await chmod(path, 0o755);
};

const writeTransferProgressPeerLoginScript = async (
  path: string,
  remoteDirectory: string,
  markerPath: string,
  command: readonly string[]
): Promise<void> => {
  await writeFile(
    path,
    [
      '#!/bin/sh',
      `cd ${shellQuote(remoteDirectory)} || exit 1`,
      'stty raw -echo -ixon -ixoff -icanon min 1 time 0 2>/dev/null || true',
      `cat ${shellQuote(markerPath)} >/dev/null || exit 1`,
      `exec ${command.map(shellQuote).join(' ')} 2>xyzm-pause-peer.stderr`,
      '',
    ].join('\n'),
    'utf8'
  );
  await chmod(path, 0o755);
};

const writeTransferDimLoginScript = async (
  path: string,
  remoteDirectory: string,
  markerPath: string,
  command: readonly string[]
): Promise<void> => {
  const markerText = 'TRANSFER_DIM_MARKER '.repeat(60);
  await createFifo(markerPath);
  await writeFile(
    path,
    [
      '#!/bin/sh',
      `cd ${shellQuote(remoteDirectory)} || exit 1`,
      `printf '%s\\n' ${shellQuote(markerText)}`,
      'stty raw -echo -ixon -ixoff -icanon min 1 time 0 2>/dev/null || true',
      `cat ${shellQuote(markerPath)} >/dev/null || exit 1`,
      `exec ${command.map(shellQuote).join(' ')} 2>lrzsz.stderr`,
      '',
    ].join('\n'),
    'utf8'
  );
  await chmod(path, 0o755);
};

const writeYmodemPostRbLoginScript = async (
  path: string,
  remoteDirectory: string,
  markerPath: string
): Promise<void> => {
  await createFifo(markerPath);
  await writeFile(
    path,
    [
      '#!/bin/sh',
      `cd ${shellQuote(remoteDirectory)} || exit 1`,
      'rm -f post-rb-input.bin post-rb-capture.marker lrzsz.stderr',
      'stty raw -echo -ixon -ixoff -icanon min 1 time 0 2>/dev/null || true',
      `cat ${shellQuote(markerPath)} >/dev/null || exit 1`,
      '/usr/bin/rb --ymodem -b -q -y 2>lrzsz.stderr',
      'stty raw -echo -ixon -ixoff -icanon min 0 time 5 2>/dev/null || true',
      'dd bs=1 count=256 of=post-rb-input.bin 2>/dev/null || :',
      ': > post-rb-capture.marker',
      'sleep 1',
      '',
    ].join('\n'),
    'utf8'
  );
  await chmod(path, 0o755);
};

const writeTelnetConfig = async (
  path: string,
  port: number,
  transferBasePath: string,
  zmodemAutostart: boolean | undefined
): Promise<void> => {
  const transferLines = [`base_path=${transferBasePath}`];
  if (zmodemAutostart !== undefined) {
    transferLines.push(
      `zmodem_autostart=${zmodemAutostart ? 'true' : 'false'}`
    );
  }
  await writeFile(
    path,
    [
      '[general]',
      'type=telnet',
      '',
      '[terminal]',
      'auto_close=false',
      '',
      '[telnet]',
      'address=127.0.0.1',
      `port=${port}`,
      '',
      '[transfer]',
      ...transferLines,
      '',
    ].join('\n'),
    'utf8'
  );
};

const createTransferFixture = async (
  directory: string,
  transferCase: TransferMenuCase,
  sizeCase: TransferSizeCase
): Promise<TransferFixture> => {
  const localDirectory = join(directory, 'local');
  const remoteDirectory = join(directory, 'remote');
  const receiveDirectory = join(directory, 'receive');
  await Promise.all([
    mkdir(localDirectory, { recursive: true }),
    mkdir(remoteDirectory, { recursive: true }),
    mkdir(receiveDirectory, { recursive: true }),
  ]);

  const payload = makeTransferPayload(sizeCase.byteLength);
  const configPath = join(directory, 'telnet.ini');
  const markerPath = join(directory, 'start-transfer.marker');
  const loginScriptPath = join(directory, 'login.sh');
  const peerStatusPath = join(remoteDirectory, 'lrzsz.status');
  const peerStderrPath = join(remoteDirectory, 'lrzsz.stderr');
  const peerReleasePath = `${markerPath}.release`;

  if (transferCase.direction === 'send') {
    const sourcePath = join(localDirectory, 'payload.bin');
    const remoteName =
      transferCase.protocol === 'xmodem' ? 'received.bin' : 'payload.bin';
    const command = lrzszCommand(transferCase, remoteName);
    await writeFile(sourcePath, payload);
    await writeLoginScript(
      loginScriptPath,
      remoteDirectory,
      markerPath,
      command
    );
    return {
      command,
      configPath,
      expectedPath: join(remoteDirectory, remoteName),
      loginScriptPath,
      markerPath,
      payload,
      payloadPath: sourcePath,
      peerReleasePath,
      peerStatusPath,
      peerStderrPath,
      sourceUri: pathToFileURL(sourcePath).href,
      transferBasePath: receiveDirectory,
    };
  }

  const remoteName = 'remote.bin';
  const remotePayloadPath = join(remoteDirectory, remoteName);
  const command = lrzszCommand(transferCase, remoteName);
  await writeFile(remotePayloadPath, payload);
  await writeLoginScript(loginScriptPath, remoteDirectory, markerPath, command);
  return {
    command,
    configPath,
    expectedPath: join(
      receiveDirectory,
      transferCase.protocol === 'xmodem' ? 'received.bin' : remoteName
    ),
    loginScriptPath,
    markerPath,
    payload,
    payloadPath: remotePayloadPath,
    peerReleasePath,
    peerStatusPath,
    peerStderrPath,
    sourceUri: undefined,
    transferBasePath: receiveDirectory,
  };
};

const createTransferProgressPeerFixture = async (
  directory: string,
  transferCase: TransferMenuCase,
  sizeCase: TransferSizeCase
): Promise<TransferProgressPeerFixture> => {
  const localDirectory = join(directory, 'local');
  const remoteDirectory = join(directory, 'remote');
  const receiveDirectory = join(directory, 'receive');
  await Promise.all([
    mkdir(localDirectory, { recursive: true }),
    mkdir(remoteDirectory, { recursive: true }),
    mkdir(receiveDirectory, { recursive: true }),
  ]);

  const payload = makeTransferPayload(sizeCase.byteLength);
  const configPath = join(directory, 'telnet.ini');
  const markerPath = join(directory, 'start-transfer.marker');
  const loginScriptPath = join(directory, 'login.sh');
  const pauseRequestPath = join(directory, 'pause-request.marker');
  const pausedPath = join(directory, 'paused.marker');
  const peerStderrPath = join(remoteDirectory, 'xyzm-pause-peer.stderr');
  const resumePath = join(directory, 'resume.marker');
  await createFifo(markerPath);

  if (transferCase.direction === 'send') {
    const sourcePath = join(localDirectory, 'payload.bin');
    const remoteName =
      transferCase.protocol === 'xmodem' ? 'received.bin' : 'payload.bin';
    const command = [
      xyzmPausePeerPath,
      '--protocol',
      transferCase.protocol,
      '--direction',
      'receive',
      '--output-dir',
      remoteDirectory,
      '--fallback-name',
      remoteName,
      '--pause-request-file',
      pauseRequestPath,
      '--paused-file',
      pausedPath,
      '--resume-file',
      resumePath,
      '--max-link-chunk',
      '1024',
      ...transferProgressPeerLinkPaceArgs(transferCase),
    ] as const;
    await writeFile(sourcePath, payload);
    await writeTransferProgressPeerLoginScript(
      loginScriptPath,
      remoteDirectory,
      markerPath,
      command
    );
    return {
      command,
      configPath,
      expectedPath: join(remoteDirectory, remoteName),
      loginScriptPath,
      markerPath,
      payload,
      payloadPath: sourcePath,
      peerStderrPath,
      sourceUri: pathToFileURL(sourcePath).href,
      transferBasePath: receiveDirectory,
      pauseRequestPath,
      pausedPath,
      resumePath,
    };
  }

  const remoteName = 'remote.bin';
  const remotePayloadPath = join(remoteDirectory, remoteName);
  const command = [
    xyzmPausePeerPath,
    '--protocol',
    transferCase.protocol,
    '--direction',
    'send',
    '--source',
    remotePayloadPath,
    '--pause-request-file',
    pauseRequestPath,
    '--paused-file',
    pausedPath,
    '--resume-file',
    resumePath,
    '--max-link-chunk',
    '1024',
    ...(transferCase.protocol === 'zmodem'
      ? ['--pause-after-source-bytes', String(Math.floor(payload.length / 2))]
      : []),
    ...transferProgressPeerLinkPaceArgs(transferCase),
  ] as const;
  await writeFile(remotePayloadPath, payload);
  await writeTransferProgressPeerLoginScript(
    loginScriptPath,
    remoteDirectory,
    markerPath,
    command
  );
  return {
    command,
    configPath,
    expectedPath: join(
      receiveDirectory,
      transferCase.protocol === 'xmodem' ? 'received.bin' : remoteName
    ),
    loginScriptPath,
    markerPath,
    payload,
    payloadPath: remotePayloadPath,
    peerStderrPath,
    sourceUri: undefined,
    transferBasePath: receiveDirectory,
    pauseRequestPath,
    pausedPath,
    resumePath,
  };
};

const createTransferDimFixture = async (
  directory: string
): Promise<TransferFixture> => {
  const remoteDirectory = join(directory, 'remote');
  const receiveDirectory = join(directory, 'receive');
  await Promise.all([
    mkdir(remoteDirectory, { recursive: true }),
    mkdir(receiveDirectory, { recursive: true }),
  ]);

  const payload = makeTransferPayload(10 * 1024);
  const configPath = join(directory, 'telnet.ini');
  const markerPath = join(directory, 'start-transfer.marker');
  const loginScriptPath = join(directory, 'login.sh');
  const remoteName = 'remote.bin';
  const remotePayloadPath = join(remoteDirectory, remoteName);
  const command = lrzszCommand(transferDimCase, remoteName);
  await writeFile(remotePayloadPath, payload);
  await writeTransferDimLoginScript(
    loginScriptPath,
    remoteDirectory,
    markerPath,
    command
  );

  return {
    command,
    configPath,
    expectedPath: join(receiveDirectory, remoteName),
    loginScriptPath,
    markerPath,
    payload,
    payloadPath: remotePayloadPath,
    sourceUri: undefined,
    transferBasePath: receiveDirectory,
  };
};

const createYmodemPostRbFixture = async (
  directory: string
): Promise<YmodemPostRbFixture> => {
  const localDirectory = join(directory, 'local');
  const remoteDirectory = join(directory, 'remote');
  const receiveDirectory = join(directory, 'receive');
  await Promise.all([
    mkdir(localDirectory, { recursive: true }),
    mkdir(remoteDirectory, { recursive: true }),
    mkdir(receiveDirectory, { recursive: true }),
  ]);

  const payload = makeTransferPayload(960 * 1024);
  const sourcePath = join(localDirectory, 'payload.bin');
  const configPath = join(directory, 'telnet.ini');
  const markerPath = join(directory, 'start-transfer.marker');
  const loginScriptPath = join(directory, 'login.sh');
  await writeFile(sourcePath, payload);
  await writeYmodemPostRbLoginScript(
    loginScriptPath,
    remoteDirectory,
    markerPath
  );

  return {
    command: ['/usr/bin/rb', '--ymodem', '-b', '-q', '-y'],
    configPath,
    expectedPath: join(remoteDirectory, 'payload.bin'),
    loginScriptPath,
    markerPath,
    payload,
    payloadPath: sourcePath,
    postRbCaptureMarkerPath: join(remoteDirectory, 'post-rb-capture.marker'),
    postRbInputPath: join(remoteDirectory, 'post-rb-input.bin'),
    sourceUri: pathToFileURL(sourcePath).href,
    transferBasePath: receiveDirectory,
  };
};

const createBatchTransferFixture = async (
  directory: string,
  transferCase: TransferMenuCase
): Promise<BatchTransferFixture> => {
  const localDirectory = join(directory, 'local');
  const remoteDirectory = join(directory, 'remote');
  const receiveDirectory = join(directory, 'receive');
  await Promise.all([
    mkdir(localDirectory, { recursive: true }),
    mkdir(remoteDirectory, { recursive: true }),
    mkdir(receiveDirectory, { recursive: true }),
  ]);

  const configPath = join(directory, 'telnet.ini');
  const markerPath = join(directory, 'start-transfer.marker');
  const loginScriptPath = join(directory, 'login.sh');
  const fileNames = batchTransferFileCases.map((fileCase) => fileCase.name);
  const command = lrzszBatchCommand(transferCase, fileNames);

  if (transferCase.direction === 'send') {
    const files = await Promise.all(
      batchTransferFileCases.map(async (fileCase) => {
        const payload = makeTransferPayload(fileCase.byteLength);
        const payloadPath = join(localDirectory, fileCase.name);
        await writeFile(payloadPath, payload);
        return {
          expectedPath: join(remoteDirectory, fileCase.name),
          name: fileCase.name,
          payload,
          payloadPath,
          sourceUri: pathToFileURL(payloadPath).href,
        };
      })
    );
    await writeLoginScript(
      loginScriptPath,
      remoteDirectory,
      markerPath,
      command
    );
    return {
      command,
      configPath,
      files,
      loginScriptPath,
      markerPath,
      sourceUris: files.map((file) => file.sourceUri),
      transferBasePath: receiveDirectory,
    };
  }

  const files = await Promise.all(
    batchTransferFileCases.map(async (fileCase) => {
      const payload = makeTransferPayload(fileCase.byteLength);
      const payloadPath = join(remoteDirectory, fileCase.name);
      await writeFile(payloadPath, payload);
      return {
        expectedPath: join(receiveDirectory, fileCase.name),
        name: fileCase.name,
        payload,
        payloadPath,
        sourceUri: undefined,
      };
    })
  );
  await writeLoginScript(loginScriptPath, remoteDirectory, markerPath, command);
  return {
    command,
    configPath,
    files,
    loginScriptPath,
    markerPath,
    sourceUris: [],
    transferBasePath: receiveDirectory,
  };
};

const createZmodemResumeFixture = async (
  directory: string,
  transferCase: TransferMenuCase
): Promise<ZmodemResumeFixture> => {
  const localDirectory = join(directory, 'local');
  const remoteDirectory = join(directory, 'remote');
  const receiveDirectory = join(directory, 'receive');
  await Promise.all([
    mkdir(localDirectory, { recursive: true }),
    mkdir(remoteDirectory, { recursive: true }),
    mkdir(receiveDirectory, { recursive: true }),
  ]);

  const payload = makeTransferPayload(zmodemResumeSizeCase.byteLength);
  const configPath = join(directory, 'telnet.ini');
  const markerPath = join(directory, 'start-transfer.marker');
  const loginScriptPath = join(directory, 'login.sh');

  if (transferCase.direction === 'send') {
    const sourcePath = join(localDirectory, 'payload.bin');
    const remoteName = 'payload.bin';
    const command = ['/usr/bin/rz', '--resume'] as const;
    await writeFile(sourcePath, payload);
    await writeFile(
      join(remoteDirectory, remoteName),
      payload.subarray(0, zmodemResumeOffset)
    );
    await writeLoginScript(
      loginScriptPath,
      remoteDirectory,
      markerPath,
      command
    );
    return {
      command,
      configPath,
      expectedPath: join(remoteDirectory, remoteName),
      loginScriptPath,
      markerPath,
      partialPath: undefined,
      payload,
      payloadPath: sourcePath,
      resumeOffset: zmodemResumeOffset,
      sourceUri: pathToFileURL(sourcePath).href,
      transferBasePath: receiveDirectory,
    };
  }

  const remoteName = 'remote.bin';
  const remotePayloadPath = join(remoteDirectory, remoteName);
  const partialPath = join(receiveDirectory, `${remoteName}.partial`);
  const command = ['/usr/bin/sz', '--resume', remoteName] as const;
  await writeFile(
    remotePayloadPath,
    corruptPrefix(payload, zmodemResumeOffset)
  );
  await writeFile(partialPath, payload.subarray(0, zmodemResumeOffset));
  await writeLoginScript(loginScriptPath, remoteDirectory, markerPath, command);
  return {
    command,
    configPath,
    expectedPath: join(receiveDirectory, remoteName),
    loginScriptPath,
    markerPath,
    partialPath,
    payload,
    payloadPath: remotePayloadPath,
    resumeOffset: zmodemResumeOffset,
    sourceUri: undefined,
    transferBasePath: receiveDirectory,
  };
};

const startConnection = async (
  connectionCase: TransferConnectionCase,
  fixture: Pick<TransferFixture | BatchTransferFixture, 'loginScriptPath'>
): Promise<TransferConnection> =>
  connectionCase.start({
    loginScriptPath: fixture.loginScriptPath,
  });

const expectTransferButtonSensitive = async (
  button: GtkToggleButtonElement
): Promise<void> => {
  await waitForResult(
    async () => {
      const info = await button.info();
      expect(info.states).toContain('enabled');
      expect(info.states).toContain('sensitive');
    },
    {
      message: 'transfer button should be sensitive',
      timeoutMs: 5_000,
    }
  );
};

const expectTransferButtonInsensitive = async (app: GtkApp): Promise<void> => {
  const transferButton = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  await waitForResult(
    async () => {
      const info = await transferButton.info();
      expect(info.states).not.toContain('sensitive');
    },
    {
      message: 'transfer button should be insensitive',
      timeoutMs: 5_000,
    }
  );
};

const activateTransfer = async (
  app: GtkApp,
  transferCase: TransferMenuCase
): Promise<void> => {
  const transferButton = expectElementKind(
    await app.getById('transfer_button'),
    'toggleButton'
  );
  await expectTransferButtonSensitive(transferButton);
  await transferButton.click();

  const item = await waitForResult(
    async () => {
      const menuItem = expectElementKind(
        await app.getById(transferCase.menuItemId),
        'menuItem'
      );
      expect((await menuItem.info()).states).toContain('showing');
      return menuItem;
    },
    {
      message: `${transferCase.label} menu item should be visible`,
      timeoutMs: 5_000,
    }
  );
  await item.click();
};

const waitForFileBytes = async (
  path: string,
  expected: Buffer,
  protocol: TransferProtocol,
  timeoutMs: number
): Promise<void> => {
  await waitForResult(
    async () => {
      const actual = await readFile(path);
      const comparable =
        protocol === 'xmodem' ? actual.subarray(0, expected.length) : actual;
      expect(comparable.length).toBe(expected.length);
      expect(Buffer.compare(comparable, expected)).toBe(0);
    },
    {
      message: `transfer result should match ${path}`,
      timeoutMs,
    }
  );
};

const waitForBatchFileBytes = async (
  files: readonly BatchTransferFileFixture[],
  protocol: TransferProtocol,
  timeoutMs: number
): Promise<void> => {
  await Promise.all(
    files.map((file) =>
      waitForFileBytes(file.expectedPath, file.payload, protocol, timeoutMs)
    )
  );
};

const expectFileMissing = async (path: string): Promise<void> => {
  await expect(access(path, constants.F_OK)).rejects.toThrow();
};

const waitForFileExists = async (
  path: string,
  timeoutMs: number
): Promise<void> => {
  await waitForResult(
    async () => {
      await access(path, constants.F_OK);
    },
    {
      message: `file should exist: ${path}`,
      timeoutMs,
    }
  );
};

const waitForTransferProgressRange = async (
  app: GtkApp,
  minimum: number,
  maximum: number,
  timeoutMs: number
): Promise<TransferProgressBarValue> =>
  waitForResult(
    async () => {
      const progress = await readTransferProgressBarValue(app);
      expect(progress.normalized).toBeGreaterThanOrEqual(minimum);
      expect(progress.normalized).toBeLessThanOrEqual(maximum);
      return progress;
    },
    {
      message: `transfer progress should be ${minimum}-${maximum}`,
      intervalMs: 1,
      timeoutMs,
    }
  );

const transferEtaStatusPattern = /\bETA [0-9]{2,}:[0-9]{2}\b/;

const readStatusBarText = async (app: GtkApp): Promise<string> => {
  const statusLabel = expectElementKind(
    await app.getById('status_label'),
    'label'
  );
  return statusLabel.text();
};

const expectTransferEtaStatus = async (
  app: GtkApp,
  transferCase: TransferMenuCase
): Promise<void> => {
  if (transferCase.protocol === 'xmodem') {
    expect(await readStatusBarText(app)).not.toMatch(transferEtaStatusPattern);
    return;
  }

  await waitForResult(
    async () => {
      expect(await readStatusBarText(app)).toMatch(transferEtaStatusPattern);
    },
    {
      message: `${transferCase.label} status should show ETA`,
      intervalMs: 10,
      timeoutMs: 15_000,
    }
  );
};

const requestTransferProgressPeerPause = async (
  fixture: TransferProgressPeerFixture
): Promise<void> => {
  await writeFile(fixture.pauseRequestPath, 'pause', 'utf8');
};

const resumeTransferProgressPeer = async (
  fixture: TransferProgressPeerFixture
): Promise<void> => {
  await writeFile(fixture.resumePath, 'resume', 'utf8');
};

const readOptionalTextFile = async (path: string): Promise<string> => {
  try {
    return await readFile(path, 'utf8');
  } catch (error) {
    return `failed to read ${path}: ${String(error)}`;
  }
};

const saveTransferPeerFailureEvidence = async (
  evidence: TestEvidence,
  fixture: TransferFixture,
  error: unknown
): Promise<void> => {
  const stderr =
    fixture.peerStderrPath === undefined
      ? 'peer stderr path unavailable'
      : await readOptionalTextFile(fixture.peerStderrPath);
  const status =
    fixture.peerStatusPath === undefined
      ? 'peer status path unavailable'
      : await readOptionalTextFile(fixture.peerStatusPath);
  await evidence.saveBinaryEvidence(
    'transfer-peer-stderr',
    Buffer.from(stderr, 'utf8'),
    'log'
  );
  await evidence.log('XYZMODEM peer failure evidence', {
    error,
    status,
  });
};

const releaseTransferPeer = async (fixture: TransferFixture): Promise<void> => {
  if (fixture.peerReleasePath !== undefined) {
    await writeFile(fixture.peerReleasePath, 'release', 'utf8');
  }
};

const pauseTransferAtProgressForCapture = async (
  app: GtkApp,
  evidence: TestEvidence,
  noticeCase: TransferProgressNoticeCase,
  fixture: TransferProgressPeerFixture
): Promise<void> => {
  const { transferCase } = noticeCase;
  if (
    transferCase.protocol === 'zmodem' &&
    transferCase.direction === 'receive'
  ) {
    let peerPaused = false;
    try {
      await expectTransferProgressNoticeVisibleAtTerminalTopRight(app);
      await waitForFileExists(fixture.pausedPath, 10_000);
      peerPaused = true;
      const pausedProgress = await waitForTransferProgressRange(
        app,
        0.45,
        0.55,
        5_000
      );
      await evidence.log('transfer progress automatic pause value', {
        pausedProgress,
        transferCase: transferCase.label,
      });
      await expectTransferEtaStatus(app, transferCase);
      await assertTransferProgressNoticeMatches(
        app,
        evidence,
        `transfer-progress-notice-${transferCase.protocol}-${transferCase.direction}`,
        noticeCase.fixturePath,
        {
          maxDiffPixels: noticeCase.maxDiffPixels,
          threshold: 0.03,
        }
      );
    } finally {
      if (peerPaused) {
        await resumeTransferProgressPeer(fixture);
      }
    }
    return;
  }

  if (transferCase.protocol === 'zmodem' && transferCase.direction === 'send') {
    await expectTransferProgressNoticeVisibleAtTerminalTopRight(app);
    const liveProgress = await waitForTransferProgressRange(
      app,
      0.45,
      0.55,
      noticeCase.sizeCase.timeoutMs
    );
    await evidence.log('transfer progress live capture value', {
      liveProgress,
      transferCase: transferCase.label,
    });
    await assertTransferProgressNoticeMatches(
      app,
      evidence,
      `transfer-progress-notice-${transferCase.protocol}-${transferCase.direction}`,
      noticeCase.fixturePath,
      {
        maxDiffPixels: noticeCase.maxDiffPixels,
        maxDiffRatio: 0.02,
        threshold: 0.03,
      }
    );
    return;
  }

  if (transferCase.protocol === 'xmodem') {
    let pauseRequested = false;
    try {
      await expectTransferProgressNoticeVisibleAtTerminalTopRight(app);
      await requestTransferProgressPeerPause(fixture);
      pauseRequested = true;
      await waitForFileExists(fixture.pausedPath, 10_000);
      await delay(100);
      await expectTransferEtaStatus(app, transferCase);
      await assertTransferProgressNoticeMatches(
        app,
        evidence,
        `transfer-progress-notice-${transferCase.protocol}-${transferCase.direction}`,
        noticeCase.fixturePath,
        {
          maxDiffPixels: noticeCase.maxDiffPixels,
          maxDiffRatio: 0.03,
          threshold: 0.08,
        }
      );
    } finally {
      if (pauseRequested) {
        await resumeTransferProgressPeer(fixture);
      }
    }
    return;
  }

  await expectTransferProgressNoticeVisibleAtTerminalTopRight(app);
  const progressBeforePause = await waitForTransferProgressRange(
    app,
    0.45,
    0.55,
    60_000
  );
  await evidence.log('transfer progress pause request value', {
    progressBeforePause,
    transferCase: transferCase.label,
  });
  await expectTransferEtaStatus(app, transferCase);
  let pauseRequested = false;
  try {
    await requestTransferProgressPeerPause(fixture);
    pauseRequested = true;
    await waitForFileExists(fixture.pausedPath, 10_000);
    const pausedProgress = await waitForTransferProgressRange(
      app,
      0.45,
      0.55,
      5_000
    );
    await evidence.log('transfer progress capture value', {
      pausedProgress,
      progressBeforePause,
      transferCase: transferCase.label,
    });
    await assertTransferProgressNoticeMatches(
      app,
      evidence,
      `transfer-progress-notice-${transferCase.protocol}-${transferCase.direction}`,
      noticeCase.fixturePath,
      {
        maxDiffPixels: noticeCase.maxDiffPixels,
        threshold: 0.03,
      }
    );
  } finally {
    if (pauseRequested) {
      await resumeTransferProgressPeer(fixture);
    }
  }
};

describe.concurrent('elder-terms-vte XYZMODEM transfer e2e', () => {
  it(
    'dims the terminal image while a transfer is active',
    async (context) => {
      await expectRequiredCommands();
      await withTemporaryDirectory(async (directory) => {
        const fixture = await createTransferDimFixture(directory);
        const connection = await startConnection(connectionCases[0], fixture);
        try {
          await writeTelnetConfig(
            fixture.configPath,
            connection.port,
            fixture.transferBasePath,
            undefined
          );
          await runGtkTest(
            context,
            ['-c', fixture.configPath],
            async (app, evidence) => {
              const terminal = await app.getById('terminal_view');
              await waitForActivityIndicatorImageState(app, 'conn', 'on');
              await expectDisconnectedNoticeHidden(app);
              await delay(300);

              await activateTransfer(app, transferDimCase);
              await expectTransferButtonInsensitive(app);
              await expectDisconnectedNoticeHidden(app);
              await expectTransferProgressNoticeVisibleAtTerminalTopRight(app);
              await assertTerminalCaptureMatches(
                terminal,
                'transfer-terminal-dim',
                transferTerminalDimPath,
                evidence,
                {
                  maxDiffPixels: 2_000,
                  maxDiffRatio: 0.03,
                  threshold: 0.08,
                }
              );

              await writeFile(fixture.markerPath, 'start', 'utf8');
              await waitForFileBytes(
                fixture.expectedPath,
                fixture.payload,
                transferDimCase.protocol,
                transferDimTimeoutMs
              );
            }
          );
        } finally {
          await connection.close();
        }
      });
    },
    transferDimTimeoutMs + 30_000
  );

  for (const connectionCase of connectionCases) {
    for (const transferCase of transferMenuCases) {
      for (const startDelayMs of startDelaysMs) {
        for (const sizeCase of transferSizeCasesByProtocol[
          transferCase.protocol
        ]) {
          it(
            `${connectionCase.name} ${transferCase.label} ${sizeCase.label} starts after ${startDelayMs}ms`,
            async (context) => {
              await expectRequiredCommands();
              await withTemporaryDirectory(async (directory) => {
                const fixture = await createTransferFixture(
                  directory,
                  transferCase,
                  sizeCase
                );
                const connection = await startConnection(
                  connectionCase,
                  fixture
                );
                try {
                  await writeTelnetConfig(
                    fixture.configPath,
                    connection.port,
                    fixture.transferBasePath,
                    undefined
                  );
                  const args = [
                    '-c',
                    fixture.configPath,
                    ...transferSourceUriArgs(
                      fixture.sourceUri === undefined ? [] : [fixture.sourceUri]
                    ),
                  ];
                  await runGtkTest(context, args, async (app, evidence) => {
                    const payloadBytes = await readFile(fixture.payloadPath);
                    const payloadEvidencePath =
                      await evidence.saveBinaryEvidence(
                        'transfer-payload',
                        payloadBytes,
                        'bin'
                      );
                    expect(
                      Buffer.compare(
                        await readFile(payloadEvidencePath),
                        payloadBytes
                      )
                    ).toBe(0);
                    await evidence.log('XYZMODEM e2e case', {
                      command: fixture.command,
                      connection: connectionCase.name,
                      direction: transferCase.direction,
                      payloadEvidencePath,
                      protocol: transferCase.protocol,
                      transferCase: transferCase.label,
                      size: sizeCase.label,
                      sizeBytes: sizeCase.byteLength,
                      startDelayMs,
                    });
                    await waitForActivityIndicatorImageState(app, 'conn', 'on');

                    const startedAtMs = performance.now();
                    if (
                      transferCase.menuItemId ===
                      'transfer_ymodem_g_receive_item'
                    ) {
                      await writeFile(fixture.markerPath, 'start', 'utf8');
                      await delay(startDelayMs);
                      await activateTransfer(app, transferCase);
                    } else if (transferCase.direction === 'send') {
                      await writeFile(fixture.markerPath, 'start', 'utf8');
                      await delay(startDelayMs);
                      await activateTransfer(app, transferCase);
                    } else {
                      await activateTransfer(app, transferCase);
                      await expectTransferButtonInsensitive(app);
                      await delay(startDelayMs);
                      await writeFile(fixture.markerPath, 'start', 'utf8');
                    }

                    try {
                      await waitForFileBytes(
                        fixture.expectedPath,
                        fixture.payload,
                        transferCase.protocol,
                        sizeCase.timeoutMs
                      );
                    } catch (error) {
                      await saveTransferPeerFailureEvidence(
                        evidence,
                        fixture,
                        error
                      );
                      throw error;
                    }
                    await releaseTransferPeer(fixture);
                    const elapsedMs = performance.now() - startedAtMs;
                    const adjustedElapsedMs = Math.max(
                      0,
                      elapsedMs - startDelayMs
                    );
                    await evidence.log('XYZMODEM e2e result', {
                      adjustedElapsedMs,
                      elapsedMs,
                    });
                    if (
                      transferCase.menuItemId === 'transfer_xmodem_1k_send_item'
                    ) {
                      expect(adjustedElapsedMs).toBeLessThan(
                        maxXmodemSendAdjustedDurationMs
                      );
                    }
                  });
                } finally {
                  await connection.close();
                }
              });
            },
            sizeCase.timeoutMs + 30_000
          );
        }
      }
    }
  }

  for (const connectionCase of connectionCases) {
    for (const transferCase of zmodemResumeMenuCases) {
      it(
        `${connectionCase.name} ${transferCase.label} resumes from ${zmodemResumeOffset} bytes`,
        async (context) => {
          await expectRequiredCommands();
          await withTemporaryDirectory(async (directory) => {
            const fixture = await createZmodemResumeFixture(
              directory,
              transferCase
            );
            const connection = await startConnection(connectionCase, fixture);
            try {
              await writeTelnetConfig(
                fixture.configPath,
                connection.port,
                fixture.transferBasePath,
                undefined
              );
              const args = [
                '-c',
                fixture.configPath,
                ...transferSourceUriArgs(
                  fixture.sourceUri === undefined ? [] : [fixture.sourceUri]
                ),
              ];
              await runGtkTest(context, args, async (app, evidence) => {
                const payloadEvidencePath = await evidence.saveBinaryEvidence(
                  'zmodem-resume-expected-payload',
                  fixture.payload,
                  'bin'
                );
                await evidence.log('ZMODEM resume e2e case', {
                  command: fixture.command,
                  connection: connectionCase.name,
                  direction: transferCase.direction,
                  payloadEvidencePath,
                  resumeOffset: fixture.resumeOffset,
                  transferCase: transferCase.label,
                });
                await waitForActivityIndicatorImageState(app, 'conn', 'on');

                if (transferCase.direction === 'send') {
                  await writeFile(fixture.markerPath, 'start', 'utf8');
                  await delay(zmodemResumeStartDelayMs);
                  await activateTransfer(app, transferCase);
                } else {
                  await activateTransfer(app, transferCase);
                  await delay(zmodemResumeStartDelayMs);
                  await writeFile(fixture.markerPath, 'start', 'utf8');
                }

                await waitForFileBytes(
                  fixture.expectedPath,
                  fixture.payload,
                  'zmodem',
                  zmodemResumeSizeCase.timeoutMs
                );
                if (fixture.partialPath !== undefined) {
                  await expectFileMissing(fixture.partialPath);
                }
              });
            } finally {
              await connection.close();
            }
          });
        },
        zmodemResumeSizeCase.timeoutMs + 30_000
      );
    }
  }

  for (const connectionCase of connectionCases) {
    for (const transferCase of batchTransferMenuCases) {
      it(
        `${connectionCase.name} ${transferCase.label} transfers 3 files starts after ${batchTransferStartDelayMs}ms`,
        async (context) => {
          await expectRequiredCommands();
          await withTemporaryDirectory(async (directory) => {
            const fixture = await createBatchTransferFixture(
              directory,
              transferCase
            );
            const connection = await startConnection(connectionCase, fixture);
            try {
              await writeTelnetConfig(
                fixture.configPath,
                connection.port,
                fixture.transferBasePath,
                undefined
              );
              const args = [
                '-c',
                fixture.configPath,
                ...transferSourceUriArgs(fixture.sourceUris),
              ];
              await runGtkTest(context, args, async (app, evidence) => {
                const payloadEvidencePaths = await Promise.all(
                  fixture.files.map(async (file) =>
                    evidence.saveBinaryEvidence(
                      `transfer-batch-payload-${file.name}`,
                      await readFile(file.payloadPath),
                      'bin'
                    )
                  )
                );
                await evidence.log('XYZMODEM batch e2e case', {
                  command: fixture.command,
                  connection: connectionCase.name,
                  direction: transferCase.direction,
                  files: fixture.files.map((file, index) => ({
                    name: file.name,
                    payloadEvidencePath: payloadEvidencePaths[index],
                    sizeBytes: file.payload.length,
                  })),
                  protocol: transferCase.protocol,
                  startDelayMs: batchTransferStartDelayMs,
                  transferCase: transferCase.label,
                });
                await waitForActivityIndicatorImageState(app, 'conn', 'on');

                const startedAtMs = performance.now();
                if (
                  transferCase.direction === 'send' ||
                  transferCase.menuItemId === 'transfer_ymodem_g_receive_item'
                ) {
                  await writeFile(fixture.markerPath, 'start', 'utf8');
                  await delay(batchTransferStartDelayMs);
                  await activateTransfer(app, transferCase);
                } else {
                  await activateTransfer(app, transferCase);
                  await delay(batchTransferStartDelayMs);
                  await writeFile(fixture.markerPath, 'start', 'utf8');
                }

                await waitForBatchFileBytes(
                  fixture.files,
                  transferCase.protocol,
                  batchTransferTimeoutMs
                );
                const elapsedMs = performance.now() - startedAtMs;
                await evidence.log('XYZMODEM batch e2e result', {
                  adjustedElapsedMs: Math.max(
                    0,
                    elapsedMs - batchTransferStartDelayMs
                  ),
                  elapsedMs,
                });
              });
            } finally {
              await connection.close();
            }
          });
        },
        batchTransferTimeoutMs + 30_000
      );
    }
  }

  it(
    'telnetd ymodem send leaves no terminal header bytes after rb exits',
    async (context) => {
      await expectRequiredCommands();
      await withTemporaryDirectory(async (directory) => {
        const fixture = await createYmodemPostRbFixture(directory);
        const connection = await startConnection(connectionCases[0], fixture);
        try {
          await writeTelnetConfig(
            fixture.configPath,
            connection.port,
            fixture.transferBasePath,
            undefined
          );
          const args = [
            '-c',
            fixture.configPath,
            ...transferSourceUriArgs(
              fixture.sourceUri === undefined ? [] : [fixture.sourceUri]
            ),
          ];
          await runGtkTest(context, args, async (app, evidence) => {
            await evidence.log('YMODEM post-rb residual e2e case', {
              command: fixture.command,
              payloadSizeBytes: fixture.payload.length,
              startDelayMs: ymodemPostRbStartDelayMs,
            });
            await waitForActivityIndicatorImageState(app, 'conn', 'on');

            await writeFile(fixture.markerPath, 'start', 'utf8');
            await delay(ymodemPostRbStartDelayMs);
            await activateTransfer(app, {
              direction: 'send',
              label: 'ymodem send',
              menuItemId: 'transfer_ymodem_send_item',
              protocol: 'ymodem',
            });

            await waitForFileBytes(
              fixture.expectedPath,
              fixture.payload,
              'ymodem',
              ymodemPostRbTimeoutMs
            );
            await waitForFileExists(fixture.postRbCaptureMarkerPath, 30_000);

            const residual = await readFile(fixture.postRbInputPath);
            await evidence.saveBinaryEvidence(
              'ymodem-post-rb-input',
              residual,
              'bin'
            );
            const terminalHeaderPrefix = Buffer.from([0x01, 0x00, 0xff]);
            const hasTerminalHeaderPrefix =
              residual.length >= terminalHeaderPrefix.length &&
              Buffer.compare(
                residual.subarray(0, terminalHeaderPrefix.length),
                terminalHeaderPrefix
              ) === 0;
            expect(hasTerminalHeaderPrefix).toBe(false);
            expect(residual.length).toBe(0);
          });
        } finally {
          await connection.close();
        }
      });
    },
    ymodemPostRbTimeoutMs + ymodemPostRbStartDelayMs + 30_000
  );

  it(
    'telnetd zmodem auto-start receives without manual transfer activation',
    async (context) => {
      await expectRequiredCommands();
      await withTemporaryDirectory(async (directory) => {
        const fixture = await createTransferFixture(
          directory,
          zmodemAutoStartReceiveCase,
          transferSizeCasesByProtocol.zmodem[0]
        );
        const connection = await startConnection(connectionCases[0], fixture);
        try {
          await writeTelnetConfig(
            fixture.configPath,
            connection.port,
            fixture.transferBasePath,
            true
          );
          await runGtkTest(context, ['-c', fixture.configPath], async (app) => {
            await waitForActivityIndicatorImageState(app, 'conn', 'on');
            await writeFile(fixture.markerPath, 'start', 'utf8');
            await waitForFileBytes(
              fixture.expectedPath,
              fixture.payload,
              'zmodem',
              transferSizeCasesByProtocol.zmodem[0].timeoutMs
            );
          });
        } finally {
          await connection.close();
        }
      });
    },
    transferSizeCasesByProtocol.zmodem[0].timeoutMs + 30_000
  );

  it(
    'telnetd zmodem auto-start sends with configured test source URI',
    async (context) => {
      await expectRequiredCommands();
      await withTemporaryDirectory(async (directory) => {
        const fixture = await createTransferFixture(
          directory,
          zmodemAutoStartSendCase,
          transferSizeCasesByProtocol.zmodem[0]
        );
        const connection = await startConnection(connectionCases[0], fixture);
        try {
          await writeTelnetConfig(
            fixture.configPath,
            connection.port,
            fixture.transferBasePath,
            true
          );
          const args = [
            '-c',
            fixture.configPath,
            ...transferSourceUriArgs(
              fixture.sourceUri === undefined ? [] : [fixture.sourceUri]
            ),
          ];
          await runGtkTest(context, args, async (app) => {
            await waitForActivityIndicatorImageState(app, 'conn', 'on');
            await writeFile(fixture.markerPath, 'start', 'utf8');
            await waitForFileBytes(
              fixture.expectedPath,
              fixture.payload,
              'zmodem',
              transferSizeCasesByProtocol.zmodem[0].timeoutMs
            );
          });
        } finally {
          await connection.close();
        }
      });
    },
    transferSizeCasesByProtocol.zmodem[0].timeoutMs + 30_000
  );
});

describe('elder-terms-vte XYZMODEM transfer progress notice e2e', () => {
  for (const noticeCase of transferProgressNoticeCases) {
    const { transferCase, sizeCase } = noticeCase;
    it(
      `shows transfer progress notice during ${transferCase.label}`,
      async (context) => {
        await expectTransferProgressPeerAvailable();
        await withTemporaryDirectory(async (directory) => {
          const fixture = await createTransferProgressPeerFixture(
            directory,
            transferCase,
            sizeCase
          );
          const connection = await startConnection(connectionCases[0], fixture);
          try {
            await writeTelnetConfig(
              fixture.configPath,
              connection.port,
              fixture.transferBasePath,
              undefined
            );
            const args = [
              '-c',
              fixture.configPath,
              ...transferSourceUriArgs(
                fixture.sourceUri === undefined ? [] : [fixture.sourceUri]
              ),
            ];
            await runGtkTest(context, args, async (app, evidence) => {
              await evidence.log('XYZMODEM progress notice e2e case', {
                command: fixture.command,
                direction: transferCase.direction,
                protocol: transferCase.protocol,
                size: sizeCase.label,
                sizeBytes: sizeCase.byteLength,
              });
              await waitForActivityIndicatorImageState(app, 'conn', 'on');
              await expectDisconnectedNoticeHidden(app);
              await expectTransferProgressNoticeHidden(app);

              if (transferCase.direction === 'send') {
                await activateTransfer(app, transferCase);
                await expectTransferButtonInsensitive(app);
                await expectTransferProgressNoticeVisibleAtTerminalTopRight(
                  app
                );
                await delay(transferProgressNoticeStartDelayMs);
                if (transferCase.protocol === 'xmodem') {
                  await requestTransferProgressPeerPause(fixture);
                }
                await writeFile(fixture.markerPath, 'start', 'utf8');
              } else {
                await activateTransfer(app, transferCase);
                await expectTransferButtonInsensitive(app);
                await expectTransferProgressNoticeVisibleAtTerminalTopRight(
                  app
                );
                await delay(transferProgressNoticeStartDelayMs);
                if (transferCase.protocol === 'xmodem') {
                  await requestTransferProgressPeerPause(fixture);
                }
                await writeFile(fixture.markerPath, 'start', 'utf8');
              }

              await pauseTransferAtProgressForCapture(
                app,
                evidence,
                noticeCase,
                fixture
              );
              try {
                await waitForFileBytes(
                  fixture.expectedPath,
                  fixture.payload,
                  transferCase.protocol,
                  sizeCase.timeoutMs
                );
              } catch (error) {
                await evidence.log('xyzm pause peer stderr', {
                  error,
                  stderr: await readOptionalTextFile(fixture.peerStderrPath),
                });
                throw error;
              }
              await expectTransferProgressNoticeHidden(app);
            });
          } finally {
            await connection.close();
          }
        });
      },
      sizeCase.timeoutMs + 30_000
    );
  }
});
