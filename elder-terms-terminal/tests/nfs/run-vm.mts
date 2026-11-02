// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createNfsWireObserver } from './nfs-wire-observer.mts';

const directory = fileURLToPath(new URL('.', import.meta.url));
const root = resolve(directory, '../../..');
const vmDirectory = resolve(root, '.build/nfs-vm');
const evidence = resolve(root, '.build/nfs-evidence');
const driver =
  process.env.ELDER_NFS_PROBE ?? resolve(root, '.build/nfs/nfs-probe');
const clientDriver = resolve(dirname(driver), 'nfs-client-test');
const run = async (
  command: string,
  args: string[],
  timeout = 60000
): Promise<string> => {
  const child = spawn(command, args, { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', (bytes) => {
    output += bytes;
  });
  child.stderr.on('data', (bytes) => {
    output += bytes;
  });
  const watchdog = setTimeout(() => child.kill('SIGKILL'), timeout);
  try {
    const [status] = await once(child, 'exit');
    assert.equal(status, 0, `${command}: ${output}`);
    return output;
  } finally {
    clearTimeout(watchdog);
  }
};

if (!process.argv.includes('--isolated')) {
  await run('bash', [resolve(directory, 'prepare-vm.sh')], 600000);
  // A private rootless network namespace permits standard PORTMAP port 111
  // without privileged host ports or changing the host's networking settings.
  process.stdout.write(
    await run(
      'podman',
      [
        'unshare',
        'unshare',
        '--net',
        process.execPath,
        fileURLToPath(import.meta.url),
        '--isolated',
      ],
      240000
    )
  );
} else {
  await run('ip', ['link', 'set', 'lo', 'up']);
  await mkdir(evidence, { recursive: true });
  const args = [
    '-enable-kvm',
    '-m',
    '768',
    '-smp',
    '2',
    '-kernel',
    resolve(vmDirectory, 'kernel'),
    '-initrd',
    resolve(vmDirectory, 'initrd'),
    '-append',
    'root=/dev/vda rw console=ttyS0 init=/nfs-test-init net.ifnames=0',
    '-drive',
    `file=${resolve(vmDirectory, 'base.ext4')},format=raw,if=virtio,snapshot=on`,
    '-netdev',
    'user,id=nfs,hostfwd=tcp:127.0.0.1:111-:111,hostfwd=tcp:127.0.0.1:22049-:2049,hostfwd=tcp:127.0.0.1:22048-:20048,hostfwd=tcp:127.0.0.1:22022-:22',
    '-device',
    'virtio-net-pci,netdev=nfs',
    '-display',
    'none',
    '-serial',
    'stdio',
    '-no-reboot',
  ];
  const observer = await createNfsWireObserver();
  const vm = spawn('qemu-system-x86_64', args, {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  const ended = once(vm, 'exit');
  let consoleOutput = '';
  const ready = new Promise<void>((resolveReady, reject) => {
    const collect = (bytes: Buffer) => {
      consoleOutput += bytes.toString();
      if (consoleOutput.includes('NFS_TEST_READY')) resolveReady();
      if (consoleOutput.includes('Kernel panic'))
        reject(new Error(consoleOutput));
    };
    vm.stdout.on('data', collect);
    vm.stderr.on('data', collect);
    vm.once('error', reject);
    vm.once('exit', () =>
      reject(new Error(`VM stopped before readiness: ${consoleOutput}`))
    );
  });
  const watchdog = setTimeout(() => vm.kill('SIGKILL'), 180000);
  const ssh = async (command: string): Promise<string> =>
    await run('ssh', [
      '-F',
      '/dev/null',
      '-i',
      resolve(vmDirectory, 'id_ed25519'),
      '-p',
      '22022',
      '-o',
      'StrictHostKeyChecking=no',
      '-o',
      'UserKnownHostsFile=/dev/null',
      '-o',
      'LogLevel=ERROR',
      'root@127.0.0.1',
      `set -e; ${command}`,
    ]);
  try {
    await ready;
    for (const version of [2, 3, 4]) {
      const options =
        version === 2
          ? '-V 2 -N 3 -N 4'
          : version === 3
            ? '-N 2 -V 3 -N 4'
            : '-N 2 -N 3 -V 4 -N 4.1 -N 4.2';
      const state = await ssh(
        `rpc.nfsd 0; rpc.nfsd ${options} -U -G 10 -L 10 8; cat /proc/fs/nfsd/versions`
      );
      assert.match(state, new RegExp(`\\+${version}(?: |\\n)`));
      for (const other of [2, 3, 4].filter((value) => value !== version))
        assert.ok(
          !state.includes(`+${other}`),
          `Guest must serve only version ${version}: ${state}`
        );
      if (version === 4) {
        await ssh(
          'pkill -x rpc.mountd; rpc.mountd -N 2 -N 3; pkill -x rpcbind'
        );
        const services = await ssh('cat /proc/fs/nfsd/v4_end_grace; ss -lntu');
        assert.ok(
          !/:111\s|:20048\s/.test(services),
          `v4-only guest must not serve MOUNT/PORTMAP: ${services}`
        );
        // Test fixture has no clients to reclaim. The server exposes readiness,
        // so no fixed sleep or automatic replay of a failed OPEN is needed.
        await ssh(
          'test "$(cat /proc/fs/nfsd/v4_end_grace)" = Y || echo Y > /proc/fs/nfsd/v4_end_grace'
        );
      }
      await writeFile(resolve(evidence, `server-v${version}.txt`), state);
      for (const encoding of ['UTF-8', 'CP932', 'SHIFT-JIS', 'EUC-JP']) {
        for (const fixed of version === 4 ? [false] : [false, true]) {
          observer.calls.length = 0;
          const output = await run(driver, [
            String(version),
            '127.0.0.1',
            version === 4 ? '/public' : '/srv/public',
            fixed ? '2049' : '0',
            fixed ? '20048' : '0',
            encoding,
            'fixture',
          ]);
          const label = `v${version}-${encoding}-${fixed ? 'fixed' : 'discovery'}`;
          await writeFile(resolve(evidence, `${label}.log`), output);
          await writeFile(
            resolve(evidence, `${label}-rpc.json`),
            JSON.stringify(observer.calls, null, 2)
          );
          assert.deepEqual(observer.errors, []);
          const nfsCalls = observer.calls.filter(
            (call) => call.program === 100003
          );
          const mountCalls = observer.calls.filter(
            (call) => call.program === 100005
          );
          assert.ok(nfsCalls.length > 0, 'A real NFS request must be observed');
          assert.deepEqual(
            [...new Set(nfsCalls.map((call) => call.version))],
            [version]
          );
          assert.deepEqual(
            [...new Set(mountCalls.map((call) => call.version))],
            version === 4 ? [] : [version === 2 ? 1 : 3]
          );
          if (version === 4)
            assert.deepEqual(
              [
                ...new Set(
                  nfsCalls
                    .filter((call) => call.procedure === 1)
                    .map((call) => call.minor)
                ),
              ],
              [0]
            );
          assert.match(output, new RegExp(`PASS NFS ${version} ${encoding}`));
          console.log(`PASS ${label}`);
        }
      }
      if (version === 3) {
        for (const encoding of ['UTF-8', 'CP932', 'SHIFT-JIS', 'EUC-JP']) {
          observer.calls.length = 0;
          const output = await run(clientDriver, [encoding]);
          await writeFile(
            resolve(evidence, `client-v3-${encoding}.log`),
            output
          );
          assert.deepEqual(observer.errors, []);
          assert.deepEqual(
            [
              ...new Set(
                observer.calls
                  .filter((call) => call.program === 100003)
                  .map((call) => call.version)
              ),
            ],
            [3]
          );
          assert.match(output, /PASS NFS client 3/);
          console.log(`PASS client-v3-${encoding}`);
        }
        await ssh(
          "sed -i '/^\\/srv\\/public /s/,insecure,/,secure,/' /etc/exports; exportfs -ra"
        );
        const parameters = [
          '3',
          '127.0.0.1',
          '/srv/public',
          '22049',
          '22048',
          'UTF-8',
          'fixture',
        ];
        const secureOutput = await run(driver, parameters);
        await writeFile(resolve(evidence, 'reserved-port.log'), secureOutput);
        await assert.rejects(
          async () =>
            await run('setpriv', [
              '--bounding-set=-net_bind_service',
              driver,
              ...parameters,
            ]),
          /denied|ACCES/i
        );
        await ssh(
          "sed -i '/^\\/srv\\/public /s/,secure,/,insecure,/' /etc/exports; exportfs -ra"
        );
        console.log(
          'PASS reserved source port with capability; denied without capability'
        );
      }
    }
  } finally {
    await observer.close();
    clearTimeout(watchdog);
    vm.kill('SIGTERM');
    await ended;
    await writeFile(resolve(evidence, 'console.log'), consoleOutput);
  }
}
