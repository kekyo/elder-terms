import { execFile } from 'node:child_process';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';

const execute = promisify(execFile);
const project = fileURLToPath(new URL('../..', import.meta.url));
const fixtures = join(project, 'shared/tests/network');
const podman = async (args: string[]): Promise<string> => {
  const result = await execute('podman', args, {
    timeout: 120_000,
    maxBuffer: 4 * 1024 * 1024,
  });
  return result.stdout.trim();
};

describe('IP scan on an isolated multicast network', () => {
  for (const protocol of ['mdns', 'llmnr', 'unavailable']) {
    it(`selects, saves and connects with ${protocol}`, async () => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-multicast-'));
      const suffix = directory.slice(directory.lastIndexOf('-') + 1);
      const network = `elder-terms-scan-${suffix}`;
      const containers: string[] = [];
      let networkCreated = false;
      const evidence = join(
        project,
        'test-results',
        process.env.ELDER_TERMS_TEST_RESULT_RUN_ID!,
        'shared',
        `network-${protocol}`
      );
      await mkdir(evidence, { recursive: true });
      try {
        // Use a cached image and host-installed binaries: no package or image
        // downloads, and no host resolver/network configuration changes.
        await podman(['image', 'exists', 'docker.io/amd64/ubuntu:24.04']);
        await podman([
          'network',
          'create',
          '--internal',
          '--disable-dns',
          network,
        ]);
        networkCreated = true;
        const client = `elder-terms-client-${suffix}`;
        const peer = `elder-terms-peer-${suffix}`;
        const emptyServices = join(directory, 'empty-services');
        await mkdir(emptyServices);
        // Keep the host's default syscall restrictions, adding only the three
        // io_uring operations used by the real terminal's networking backend.
        const seccomp = JSON.parse(
          await readFile('/usr/share/containers/seccomp.json', 'utf8')
        );
        seccomp.syscalls.push({
          names: ['io_uring_setup', 'io_uring_enter', 'io_uring_register'],
          action: 'SCMP_ACT_ALLOW',
        });
        const seccompPath = join(directory, 'terminal-seccomp.json');
        await writeFile(seccompPath, JSON.stringify(seccomp));
        for (const role of ['peer', 'client']) {
          const name = role === 'peer' ? peer : client;
          const config = join(directory, `${role}-resolved.conf`);
          await writeFile(
            config,
            [
              '[Resolve]',
              'DNS=',
              'FallbackDNS=',
              `LLMNR=${protocol === 'mdns' ? 'no' : 'yes'}`,
              `MulticastDNS=${protocol === 'llmnr' ? 'no' : 'yes'}`,
              'DNSSEC=no',
              'DNSOverTLS=no',
              'ReadEtcHosts=no',
              '',
            ].join('\n')
          );
          const services =
            role === 'client' && protocol === 'unavailable'
              ? emptyServices
              : join(fixtures, 'dbus-services');
          await podman([
            'run',
            '--detach',
            '--pull=never',
            // The container has no GPU devices. Restrict EGL discovery to Mesa
            // so host-mounted NVIDIA drivers cannot crash its headless Xvfb.
            // https://github.com/NVIDIA/libglvnd/blob/master/src/EGL/icd_enumeration.md
            '--env',
            '__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json',
            '--name',
            name,
            '--hostname',
            role === 'peer' ? 'scan-peer' : 'scan-client',
            '--network',
            network,
            '--cap-add',
            'NET_RAW',
            ...(role === 'client'
              ? ['--security-opt', `seccomp=${seccompPath}`]
              : []),
            '-v',
            '/usr:/usr:ro',
            '-v',
            '/etc/passwd:/etc/passwd:ro',
            '-v',
            '/etc/group:/etc/group:ro',
            '-v',
            '/etc/fonts:/etc/fonts:ro',
            '-v',
            `${process.execPath}:/test-node:ro`,
            '-v',
            `${project}:/workspace:ro`,
            '-v',
            `${evidence}:/evidence:rw`,
            '-v',
            `${services}:/resolver-services:ro`,
            '-v',
            // Distribution drop-ins override resolved.conf. Keep this fixture's
            // explicit mDNS/LLMNR selection above those defaults.
            `${config}:/etc/systemd/resolved.conf.d/99-elder-terms-test.conf:ro`,
            '-v',
            `${join(fixtures, 'resolv.conf')}:/etc/resolv.conf:ro`,
            '--entrypoint',
            '/bin/bash',
            'docker.io/amd64/ubuntu:24.04',
            '/workspace/shared/tests/network/start-container.sh',
            role,
          ]);
          containers.push(name);
        }
        await waitForResult(async () => {
          await podman([
            'exec',
            peer,
            'test',
            '-f',
            '/run/ip-scan-server-ready',
          ]);
        });
        await podman([
          'exec',
          peer,
          'gdbus',
          'call',
          '--system',
          '--dest',
          'org.freedesktop.DBus',
          '--object-path',
          '/org/freedesktop/DBus',
          '--method',
          'org.freedesktop.DBus.StartServiceByName',
          'org.freedesktop.resolve1',
          '0',
        ]);
        // Older resolved versions keep per-link defaults independent of the
        // global protocol setting. The unavailable case must not start a client
        // resolver, so only its peer receives per-link configuration.
        for (const name of protocol === 'unavailable'
          ? [peer]
          : [peer, client]) {
          await podman([
            'exec',
            name,
            'resolvectl',
            'mdns',
            'eth0',
            protocol === 'llmnr' ? 'no' : 'yes',
          ]);
          await podman([
            'exec',
            name,
            'resolvectl',
            'llmnr',
            'eth0',
            protocol === 'mdns' ? 'no' : 'yes',
          ]);
        }
        const inspection = JSON.parse(await podman(['inspect', peer]));
        const address = inspection[0].NetworkSettings.Networks[network]
          .IPAddress as string;
        expect(address).toMatch(/^\d+\.\d+\.\d+\.\d+$/);
        const expected =
          protocol === 'mdns'
            ? 'scan-peer.local'
            : protocol === 'llmnr'
              ? 'scan-peer'
              : address;
        const output = await podman([
          'exec',
          client,
          '/test-node',
          '/workspace/shared/tests/network/scan-and-connect.ts',
          address,
          expected,
        ]);
        expect(output).toContain(
          `Verified ${address} -> ${expected} -> TELNET`
        );
        expect(output).toContain(
          'Verified scan -> XDG edit -> font settings -> save -> reconnect -> restart'
        );
        await writeFile(join(evidence, 'result.txt'), output);
      } finally {
        // Record diagnostics before removing only resources created by this test.
        const cleanupErrors: unknown[] = [];
        for (const name of containers.reverse()) {
          try {
            const logs = await execute('podman', ['logs', name]);
            await writeFile(
              join(evidence, `${name}.log`),
              logs.stdout + logs.stderr
            );
          } catch (error) {
            cleanupErrors.push(error);
          }
          try {
            await podman(['rm', '--force', name]);
          } catch (error) {
            cleanupErrors.push(error);
          }
        }
        if (networkCreated) {
          try {
            await podman(['network', 'rm', network]);
          } catch (error) {
            cleanupErrors.push(error);
          }
        }
        await rm(directory, { recursive: true, force: true });
        expect(cleanupErrors, 'test resources must be released').toEqual([]);
      }
    }, 180_000);
  }
});
