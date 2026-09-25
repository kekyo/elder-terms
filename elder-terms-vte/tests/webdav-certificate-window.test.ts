import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { PNG } from 'pngjs';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';
import { createWebdavTestServer } from './webdav-test-server';

const execute = promisify(execFile);

describe('WebDAV certificate confirmation', () => {
  for (const scenario of [
    'allow',
    'abort',
    'escape',
    'enter',
    'close',
    'auth-enter',
    'name',
    'expired',
    'future',
    'valid',
    'reject',
    'missing-ca',
    'tls-1.0',
    'tls-1.1',
    'two-errors',
    'changed',
  ] as const) {
    it(`handles ${scenario} through the shared overlay and connection policy`, async (context) => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-dav-certificate-'));
      const certificate = join(directory, 'certificate.pem'),
        key = join(directory, 'key.pem');
      const wrongName = scenario === 'name' || scenario === 'two-errors';
      await execute('openssl', [
        'req',
        '-x509',
        '-newkey',
        'rsa:2048',
        '-noenc',
        '-days',
        '2',
        '-subj',
        '/CN=fixture <DAV> & server',
        '-addext',
        wrongName
          ? 'subjectAltName=DNS:other.example.test'
          : 'subjectAltName=IP:127.0.0.1',
        '-keyout',
        key,
        '-out',
        certificate,
      ]);
      if (scenario === 'expired' || scenario === 'future') {
        const index = join(directory, 'index'),
          serial = join(directory, 'serial');
        const config = join(directory, 'openssl.cnf'),
          csr = join(directory, 'request.pem');
        await writeFile(index, '');
        await writeFile(serial, '1000\n');
        await writeFile(
          config,
          [
            '[ca]',
            'default_ca=fixture',
            '[fixture]',
            `database=${index}`,
            `serial=${serial}`,
            `new_certs_dir=${directory}`,
            `certificate=${certificate}`,
            `private_key=${key}`,
            'default_md=sha256',
            'policy=policy',
            'x509_extensions=server',
            '[policy]',
            'commonName=supplied',
            '[server]',
            'basicConstraints=critical,CA:TRUE',
            'subjectAltName=IP:127.0.0.1',
            '',
          ].join('\n')
        );
        await execute('openssl', [
          'req',
          '-new',
          '-key',
          key,
          '-subj',
          '/CN=fixture <DAV> & server',
          '-out',
          csr,
        ]);
        const dated = join(directory, 'dated.pem');
        await execute('openssl', [
          'ca',
          '-batch',
          '-selfsign',
          '-config',
          config,
          '-in',
          csr,
          '-out',
          dated,
          '-startdate',
          scenario === 'expired' ? '20000101000000Z' : '20990101000000Z',
          '-enddate',
          scenario === 'expired' ? '20010101000000Z' : '21000101000000Z',
        ]);
        await writeFile(certificate, await readFile(dated));
      }
      const legacyVersion =
        scenario === 'tls-1.0'
          ? 'TLSv1'
          : scenario === 'tls-1.1'
            ? 'TLSv1.1'
            : undefined;
      const server = await createWebdavTestServer('basic', {
        cert: await readFile(certificate, 'utf8'),
        key: await readFile(key, 'utf8'),
        minVersion: legacyVersion,
        maxVersion: legacyVersion,
        ciphers:
          legacyVersion === undefined ? undefined : 'DEFAULT:@SECLEVEL=0',
      });
      const evidence = createTestEvidence(context);
      const apps: GtkApp[] = [];
      let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
      try {
        const local = join(directory, 'local');
        await mkdir(local);
        const configPath = join(directory, 'webdav.ini');
        const trusted = [
          'name',
          'expired',
          'future',
          'valid',
          'tls-1.0',
          'tls-1.1',
        ].includes(scenario);
        const settings = [
          '[general]',
          'type=webdav',
          'name=DAV certificate fixture',
          '[webdav]',
          'scheme=https',
          'address=127.0.0.1',
          `port=${server.port}`,
          'base_path=/dav/',
          'authentication=basic',
          'username=alice',
          `local_directory=${local}`,
          `ca_file=${scenario === 'missing-ca' ? join(directory, 'absent.pem') : trusted ? certificate : ''}`,
          `certificate_error_action=${scenario === 'reject' ? 'reject' : 'prompt'}`,
          '',
        ].join('\n');
        await writeFile(configPath, settings);
        launcher = createGtkAppLauncher({
          appPath:
            process.env.ELDER_TERMS_TEST_FTP_APP ??
            fileURLToPath(
              new URL(
                '../../.build/elder-terms-vte/elder-terms-file-transfer',
                import.meta.url
              )
            ),
          env: {
            LANGUAGE: 'en',
            LC_ALL: 'C.UTF-8',
            XDG_CONFIG_HOME: join(directory, 'config'),
          },
          onSystemOutput: evidence.recordSystemOutputEvent,
          xvfbTrayHost: true,
        });
        const authenticate = async (app: GtkApp) => {
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_prompt_message_label'),
                'label'
              ).text()
            ).toBe('Password:');
          });
          const entry = expectElementKind(
            await app.getById('file_transfer_prompt_entry'),
            'entry'
          );
          await entry.setText('secret');
          if (scenario === 'auth-enter') {
            const bounds = (await entry.capture()).bounds;
            await app.input.moveMouseTo(
              Math.round(bounds.x + bounds.width / 2),
              Math.round(bounds.y + bounds.height / 2)
            );
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
            await app.input.pressKey('Return');
          } else
            await expectElementKind(
              await app.getById('file_transfer_prompt_accept_button'),
              'button'
            ).click();
        };
        const app = await launcher.launch(['-c', configPath], {
          onOutput: evidence.recordAppOutputEvent,
        });
        apps.push(app);
        await authenticate(app);
        const status = expectElementKind(
          await app.getById('file_transfer_status_label'),
          'label'
        );
        const title = expectElementKind(
          await app.getById('file_transfer_prompt_title_label'),
          'label'
        );
        if (scenario === 'valid') {
          await waitForResult(async () => {
            expect(await status.text()).toBe('Ready');
          });
          expect(server.requests.some((request) => request.authenticated)).toBe(
            true
          );
          return;
        }
        if (
          scenario === 'reject' ||
          scenario === 'missing-ca' ||
          legacyVersion !== undefined
        ) {
          await waitForResult(async () => {
            expect(await title.text()).toBe('Failed to start WebDAV');
          });
          expect(server.requests).toHaveLength(0);
          return;
        }
        await waitForResult(async () => {
          expect(await title.text()).toBe(
            'HTTPS certificate validation failed'
          );
        });
        expect(server.requests).toHaveLength(0);
        const messageLabel = expectElementKind(
          await app.getById('file_transfer_prompt_message_label'),
          'label'
        );
        const message = await messageLabel.text();
        const fingerprint = (
          await execute('openssl', [
            'x509',
            '-in',
            certificate,
            '-noout',
            '-fingerprint',
            '-sha256',
          ])
        ).stdout
          .trim()
          .split('=')[1];
        expect(message.replace(/\n/g, '')).toContain(fingerprint);
        for (const text of [
          '127.0.0.1',
          String(server.port),
          'HTTPS',
          'Subject:',
          'Issuer:',
          'Valid from:',
          'Valid until:',
          'Exceptions are not saved.',
        ])
          expect(message).toContain(text);
        const subject = (
          await execute('openssl', [
            'x509',
            '-in',
            certificate,
            '-noout',
            '-subject',
            '-nameopt',
            'RFC2253',
          ])
        ).stdout
          .trim()
          .replace(/^subject=/, '');
        expect(message.replace(/\n/g, '')).toContain(subject);
        const cancel = expectElementKind(
          await app.getById('file_transfer_prompt_cancel_button'),
          'button'
        );
        expect((await cancel.info()).states).toContain('focused');
        const overlay = await app.getById('file_transfer_prompt_panel');
        await evidence.captureEvidence('webdav-certificate-pending', async () =>
          app.capture()
        );
        if (scenario === 'close') {
          const pending: GtkWidgetElement[] = [
            await app.getById('file_transfer_header_bar'),
          ];
          let closed = false;
          while (pending.length) {
            const widget = pending.shift()!;
            if (
              widget.kind === 'button' &&
              (await widget.info()).name === 'Close'
            ) {
              await widget.click();
              closed = true;
              break;
            }
            if ('getChildCount' in widget)
              for (
                let index = 0;
                index < (await widget.getChildCount());
                ++index
              ) {
                const child = await widget.childAt(index);
                if (child) pending.push(child);
              }
          }
          expect(closed).toBe(true);
        } else if (scenario === 'abort') await cancel.click();
        else if (scenario === 'escape') await app.input.pressKey('Escape');
        else if (scenario === 'enter') await app.input.pressKey('Return');
        else {
          if (scenario === 'allow') {
            const screen = PNG.sync.read((await app.capture()).image);
            const area = (await overlay.capture()).bounds;
            const environment = await app.environment();
            const videoPath = join(
              evidence.directory,
              'webdav-certificate-confirmation.mkv'
            );
            let pending = Buffer.alloc(0);
            let frame = Buffer.alloc(0);
            let frameNumber = 0;
            let recordingError: Error | undefined;
            let stderr = '';
            const frameSize = screen.width * screen.height * 4;
            const recorder = spawn(
              'ffmpeg',
              [
                '-hide_banner',
                '-loglevel',
                'error',
                '-f',
                'x11grab',
                '-framerate',
                '30',
                '-draw_mouse',
                '0',
                '-video_size',
                `${screen.width}x${screen.height}`,
                '-i',
                environment.DISPLAY ?? '',
                '-map',
                '0:v',
                '-c:v',
                'ffv1',
                '-pix_fmt',
                'bgr0',
                '-fps_mode',
                'passthrough',
                videoPath,
                '-map',
                '0:v',
                '-c:v',
                'rawvideo',
                '-pix_fmt',
                'bgr0',
                '-fps_mode',
                'passthrough',
                '-f',
                'rawvideo',
                'pipe:1',
              ],
              { env: environment, stdio: ['pipe', 'pipe', 'pipe'] }
            );
            const finished = new Promise<number | null>((resolve) => {
              recorder.once('error', (error) => {
                recordingError = error;
                resolve(null);
              });
              recorder.once('close', resolve);
            });
            recorder.stderr.on('data', (bytes: Buffer) => {
              stderr += bytes.toString();
            });
            recorder.stdout.on('data', (bytes: Buffer) => {
              pending = Buffer.concat([pending, bytes]);
              while (pending.length >= frameSize) {
                frame = Buffer.from(pending.subarray(0, frameSize));
                pending = pending.subarray(frameSize);
                frameNumber++;
              }
            });
            const states: {
              readonly frame: number;
              readonly label: string;
              readonly pixels: Buffer;
            }[] = [];
            const difference = (
              image: Buffer,
              width: number,
              bgr: boolean,
              reference: Buffer
            ) => {
              let different = 0;
              for (let y = 0; y < area.height; y++) {
                for (let x = 0; x < area.width; x++) {
                  const actual = ((area.y + y) * width + area.x + x) * 4;
                  const expected = (y * area.width + x) * 4;
                  if (
                    [0, 1, 2].some(
                      (channel) =>
                        Math.abs(
                          image[actual + (bgr ? 2 - channel : channel)] -
                            reference[expected + channel]
                        ) > 8
                    )
                  )
                    different++;
                }
              }
              return different / (area.width * area.height);
            };
            const expectVideoState = async (label: string) => {
              let previousPixels: Buffer | undefined;
              const previousFrame = frameNumber;
              const state = await waitForResult(async () => {
                if (recordingError) throw recordingError;
                expect(recorder.exitCode, stderr).toBeNull();
                const captured = PNG.sync.read((await app.capture()).image);
                const current = Buffer.alloc(area.width * area.height * 4);
                for (let y = 0; y < area.height; y++) {
                  const offset = ((area.y + y) * captured.width + area.x) * 4;
                  captured.data.copy(
                    current,
                    y * area.width * 4,
                    offset,
                    offset + area.width * 4
                  );
                }
                const previous = previousPixels;
                previousPixels = current;
                // The recorded states must match stable screenshots. The visible
                // change is also checked below; its area depends on the layout.
                expect(previous).toBeDefined();
                expect(current.equals(previous!)).toBe(true);
                // Recapture when rendering advances before the video catches up.
                // A transient stable screenshot must not become a permanent target.
                expect(frameNumber).toBeGreaterThan(previousFrame);
                expect(
                  difference(frame, screen.width, true, current)
                ).toBeLessThan(0.01);
                return { frame: frameNumber, label, pixels: current };
              });
              states.push(state);
            };
            try {
              await expectVideoState(
                'certificate decision pending without HTTP authentication'
              );
              expect(server.requests).toHaveLength(0);
              await expectElementKind(
                await app.getById('file_transfer_prompt_accept_button'),
                'button'
              ).click();

              await waitForResult(async () => {
                expect((await overlay.info()).states).not.toContain('showing');
                expect(
                  await expectElementKind(
                    await app.getById('file_transfer_status_label'),
                    'label'
                  ).text()
                ).toBe('Ready — Certificate exception active');
                expect(
                  await expectElementKind(
                    await app.getById('file_transfer_remote_tree'),
                    'table'
                  ).getRowCount()
                ).toBeGreaterThan(0);
              });
              await expectVideoState(
                'explicitly approved certificate and resumed browser'
              );
              expect(states[1].frame).toBeGreaterThan(states[0].frame);
              expect(states[1].pixels.equals(states[0].pixels)).toBe(false);
            } finally {
              recorder.stdin.write('q\n');
              const code = await finished;
              await evidence.log('WebDAV certificate video states', {
                states: states.map(({ frame, label }) => ({ frame, label })),
                code,
                stderr,
              });
              expect(code, stderr).toBe(0);
            }
            const selection = states
              .map(({ frame }) => `eq(n\\,${frame - 1})`)
              .join('+');
            await execute('ffmpeg', [
              '-v',
              'error',
              '-i',
              videoPath,
              '-vf',
              `select=${selection}`,
              '-fps_mode',
              'passthrough',
              join(evidence.directory, 'webdav-certificate-frame-%d.png'),
            ]);
            for (let index = 0; index < states.length; index++) {
              const saved = PNG.sync.read(
                await readFile(
                  join(
                    evidence.directory,
                    `webdav-certificate-frame-${index + 1}.png`
                  )
                )
              );
              expect(
                difference(saved.data, saved.width, false, states[index].pixels)
              ).toBeLessThan(0.01);
            }
          } else {
            await expectElementKind(
              await app.getById('file_transfer_prompt_accept_button'),
              'button'
            ).click();
            if (scenario === 'two-errors') {
              await waitForResult(async () => {
                expect(await title.text()).toBe(
                  'HTTPS certificate validation failed'
                );
                expect(await messageLabel.text()).not.toBe(message);
              });
              expect(server.requests).toHaveLength(0);
              expect((await cancel.info()).states).toContain('focused');
              await expectElementKind(
                await app.getById('file_transfer_prompt_accept_button'),
                'button'
              ).click();
            }
          }
          await waitForResult(async () => {
            expect(await status.text()).toBe(
              'Ready — Certificate exception active'
            );
            expect(
              await expectElementKind(
                await app.getById('file_transfer_remote_tree'),
                'table'
              ).getRowCount()
            ).toBe(4);
          });
          expect(server.requests.some((request) => request.authenticated)).toBe(
            true
          );
          expect(await readFile(configPath, 'utf8')).toBe(settings);
          if (scenario === 'changed') {
            const replacement = join(directory, 'replacement.pem');
            await execute('openssl', [
              'req',
              '-x509',
              '-key',
              key,
              '-days',
              '2',
              '-set_serial',
              '2',
              '-subj',
              '/CN=replacement fixture',
              '-addext',
              'subjectAltName=IP:127.0.0.1',
              '-out',
              replacement,
            ]);
            const changedFingerprint = (
              await execute('openssl', [
                'x509',
                '-in',
                replacement,
                '-noout',
                '-fingerprint',
                '-sha256',
              ])
            ).stdout
              .trim()
              .split('=')[1];
            expect(changedFingerprint).not.toBe(fingerprint);
            const requestCount = server.requests.length;
            server.replaceTls({
              cert: await readFile(replacement, 'utf8'),
              key: await readFile(key, 'utf8'),
            });
            await expectElementKind(
              await app.getById('file_transfer_remote_refresh_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect(await title.text()).toBe(
                'HTTPS certificate validation failed'
              );
              expect((await messageLabel.text()).replace(/\n/g, '')).toContain(
                changedFingerprint
              );
            });
            expect(server.requests).toHaveLength(requestCount);
            expect((await cancel.info()).states).toContain('focused');
            await cancel.click();
            await waitForResult(async () => {
              expect((await app.output()).exitCode).toBe(0);
            });
            return;
          }

          if (scenario === 'allow') {
            const requestCount = server.requests.length;
            const second = await launcher.launch(['-c', configPath], {
              onOutput: evidence.recordAppOutputEvent,
            });
            apps.push(second);
            await authenticate(second);
            await waitForResult(async () => {
              expect(
                await expectElementKind(
                  await second.getById('file_transfer_prompt_title_label'),
                  'label'
                ).text()
              ).toBe('HTTPS certificate validation failed');
            });
            expect(server.requests).toHaveLength(requestCount);
            await expectElementKind(
              await second.getById('file_transfer_prompt_cancel_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect((await second.output()).exitCode).toBe(0);
            });
            expect(await status.text()).toBe(
              'Ready — Certificate exception active'
            );
          }
          return;
        }
        await waitForResult(async () => {
          const output = await app.output();
          expect(output.exitCode, output.stderr).toBe(0);
          expect(output.exitSignal).toBeNull();
        });
        expect(server.requests).toHaveLength(0);
      } finally {
        try {
          await evidence.log('WebDAV certificate requests', server.requests);
          await evidence.flushOutputs(apps, launcher);
        } finally {
          if (launcher) await launcher.release();
          await server.close();
          await evidence.release();
          await rm(directory, { recursive: true, force: true });
        }
      }
    });
  }
});
