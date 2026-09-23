import { execFile, spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import {
  mkdir,
  mkdtemp,
  readFile,
  readdir,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkTableElement,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';

// Use the same complete window scenarios for an installed application tree.
const ftpAppPath =
  process.env.ELDER_TERMS_TEST_FTP_APP ??
  fileURLToPath(
    new URL(
      '../../.build/elder-terms-vte/elder-terms-file-transfer',
      import.meta.url
    )
  );
const serverPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/ftp-test-server', import.meta.url)
);
const execute = promisify(execFile);
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');

for (const testCase of [
  'passive',
  'ftps-explicit',
  'ftps-implicit',
  'ftps-legacy',
  'ftps-prompt-allow',
  'ftps-prompt-implicit',
  'ftps-prompt-legacy',
  'ftps-prompt-abort',
  'ftps-prompt-ja',
  'ftps-prompt-escape',
  'ftps-prompt-enter',
  'ftps-prompt-close',
  'ftps-prompt-valid',
  'ftps-prompt-expired',
  'ftps-prompt-future',
  'ftps-prompt-name',
  'ftps-prompt-data',
  'ftps-cert-reject',
  'ftps-cancel-final',
  'ftps-close-final-upload',
  'ftps-close-final-download',
  'active',
  'login-refused',
  'cancel-final',
  'close-final',
  'close-final-download',
] as const) {
  const scenario = [
    'ftps-cancel-final',
    'ftps-close-final-upload',
    'ftps-close-final-download',
  ].includes(testCase)
    ? testCase.slice(5)
    : testCase;
  it(`uses the real FTP service through its window: ${testCase}`, async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-terms-ftp-live-'));
    const evidence = createTestEvidence(context);
    const apps: GtkApp[] = [];
    let launcher: ReturnType<typeof createGtkAppLauncher> | undefined;
    let server: ReturnType<typeof spawn> | undefined;
    let serverFinished: Promise<number | null> | undefined;
    const events: string[] = [];
    let serverError: Error | undefined;
    let serverLog = '';
    try {
      const configHome = join(directory, 'config');
      const local = join(directory, 'local');
      const remoteRoot = join(directory, 'remote');
      const remote = join(remoteRoot, 'home');
      const configPath = join(directory, 'ftp.ini');
      const ftps = testCase.startsWith('ftps-');
      const japanese = testCase === 'ftps-prompt-ja';
      const certificatePrompt = testCase.startsWith('ftps-prompt-');
      const dataCertificateFailure = testCase === 'ftps-prompt-data';
      const invalidCertificate =
        certificatePrompt && testCase !== 'ftps-prompt-valid';
      const rejectedCertificate = testCase === 'ftps-cert-reject';
      const legacyTls =
        testCase === 'ftps-legacy' || testCase === 'ftps-prompt-legacy';
      const tlsMode =
        testCase === 'ftps-prompt-implicit' ||
        testCase === 'ftps-implicit' ||
        testCase.startsWith('ftps-close-')
          ? 'implicit'
          : 'explicit';
      const certificate = join(directory, 'certificate.pem');
      const privateKey = join(directory, 'private-key.pem');
      const dataCertificate = join(directory, 'data-certificate.pem');
      const dataKey = join(directory, 'data-key.pem');
      if (ftps) {
        await execute('openssl', [
          'req',
          '-x509',
          '-newkey',
          'rsa:2048',
          '-noenc',
          '-keyout',
          privateKey,
          '-out',
          certificate,
          '-days',
          '1',
          '-subj',
          '/CN=localhost',
          '-addext',
          testCase === 'ftps-prompt-name'
            ? 'subjectAltName=DNS:wrong.invalid'
            : 'subjectAltName=IP:127.0.0.1',
        ]);
      }
      if (
        testCase === 'ftps-prompt-future' ||
        testCase === 'ftps-prompt-expired'
      ) {
        const request = join(directory, 'dated.csr'),
          config = join(directory, 'ca.cnf'),
          dated = join(directory, 'dated.pem');
        await writeFile(join(directory, 'index'), '');
        await writeFile(join(directory, 'serial'), '01\n');
        await writeFile(
          config,
          '[ca]\ndefault_ca=test\n[test]\ndatabase=' +
            join(directory, 'index') +
            '\nserial=' +
            join(directory, 'serial') +
            '\nnew_certs_dir=' +
            directory +
            '\ncertificate=' +
            certificate +
            '\nprivate_key=' +
            privateKey +
            '\ndefault_md=sha256\npolicy=policy\nx509_extensions=server\n[policy]\ncommonName=supplied\n[server]\nbasicConstraints=critical,CA:TRUE\nsubjectAltName=IP:127.0.0.1\n'
        );
        await execute('openssl', [
          'req',
          '-new',
          '-key',
          privateKey,
          '-subj',
          '/CN=localhost',
          '-out',
          request,
        ]);
        await execute('openssl', [
          'ca',
          '-batch',
          '-selfsign',
          '-config',
          config,
          '-in',
          request,
          '-out',
          dated,
          '-startdate',
          testCase === 'ftps-prompt-expired'
            ? '20000101000000Z'
            : '20990101000000Z',
          '-enddate',
          testCase === 'ftps-prompt-expired'
            ? '20010101000000Z'
            : '21000101000000Z',
        ]);
        await writeFile(certificate, await readFile(dated));
      }
      if (dataCertificateFailure)
        await execute('openssl', [
          'req',
          '-x509',
          '-newkey',
          'rsa:2048',
          '-noenc',
          '-keyout',
          dataKey,
          '-out',
          dataCertificate,
          '-days',
          '1',
          '-subj',
          '/CN=data-channel',
          '-addext',
          'subjectAltName=IP:127.0.0.1',
        ]);
      const closingDownload = scenario === 'close-final-download';
      const held =
        scenario === 'cancel-final' || scenario.startsWith('close-final');
      await Promise.all([
        mkdir(configHome),
        mkdir(local),
        mkdir(join(remote, 'archive'), { recursive: true }),
      ]);
      const upload = Buffer.alloc(2 * 1024 * 1024 + 17);
      for (let index = 0; index < upload.length; index++)
        upload[index] = index % 251;
      const download = Buffer.from('Remote binary\r\n\x00\xff\n', 'latin1');
      await Promise.all([
        writeFile(join(local, 'upload.bin'), upload),
        writeFile(join(remote, 'download.bin'), download),
        writeFile(
          join(remote, 'archive', 'nested.txt'),
          'nested remote file\n'
        ),
      ]);
      server = spawn(
        serverPath,
        [
          remoteRoot,
          '--trace',
          ...(ftps
            ? [
                `--tls=${tlsMode}`,
                `--cert=${certificate}`,
                `--key=${privateKey}`,
              ]
            : []),
          ...(dataCertificateFailure
            ? ['--data-cert=' + dataCertificate, '--data-key=' + dataKey]
            : []),
          ...(held ? ['--hold-final'] : []),
          ...(scenario === 'login-refused' ? ['--reject-login'] : []),
          ...(legacyTls ? ['--tls-version=769', '--legacy-tls'] : []),
        ],
        { stdio: ['pipe', 'pipe', 'pipe'] }
      );
      const lines = createInterface({ input: server.stdout! });
      lines.on('line', (line) => events.push(line));
      server.stderr!.on('data', (bytes: Buffer) => {
        serverLog += bytes.toString();
      });
      serverFinished = new Promise((resolve) => {
        server!.once('error', (error) => {
          serverError = error;
          resolve(null);
        });
        server!.once('close', resolve);
      });
      const ready = await waitForResult(async () => {
        if (serverError) throw serverError;
        expect(server!.exitCode, serverLog).toBeNull();
        const line = events.find((value) => value.startsWith('READY '));
        expect(line).toBeDefined();
        return line!;
      });
      await writeFile(
        configPath,
        [
          '[general]',
          'name=Live FTP',
          'type=ftp',
          'auto_close=false',
          'background=#183C58',
          '[ftp]',
          'address=127.0.0.1',
          `port=${ready.slice(6)}`,
          'username=alice',
          ...(ftps
            ? [
                `tls_mode=${tlsMode}`,
                `ca_file=${(invalidCertificate && !dataCertificateFailure && !['ftps-prompt-expired', 'ftps-prompt-future', 'ftps-prompt-name'].includes(testCase)) || rejectedCertificate ? '' : certificate}`,
              ]
            : []),
          ...(legacyTls
            ? [
                'tls_min_version=1.0',
                'tls_max_version=1.0',
                'tls_compatibility=openssl_legacy',
              ]
            : []),
          ...(certificatePrompt ? ['certificate_error_action=prompt'] : []),
          `data_connection_mode=${scenario === 'active' ? 'active' : 'passive'}`,
          `local_directory=${local}`,
          'remote_directory=/home',
          '',
        ].join('\n')
      );
      if (japanese) {
        const configurationDirectory = join(configHome, 'elder-terms');
        await mkdir(configurationDirectory, { recursive: true });
        await writeFile(
          join(configurationDirectory, 'global.ini'),
          '[general]\nui_language=ja\n'
        );
      }
      launcher = createGtkAppLauncher({
        appPath: ftpAppPath,
        // Instrumented GTK startup can exceed the driver's default ten seconds.
        timeoutMs: 60_000,
        env: {
          LANGUAGE: japanese ? 'ja' : 'en',
          LC_ALL: 'C.UTF-8',
          XDG_CONFIG_HOME: configHome,
          ELDER_TERMS_LOCALE_DIR: fileURLToPath(
            new URL('../../.build/po/', import.meta.url)
          ),
        },
        onSystemOutput: evidence.recordSystemOutputEvent,
        xvfbTrayHost: true,
      });
      const app = await launcher.launch(['-c', configPath], {
        onOutput: evidence.recordAppOutputEvent,
      });
      apps.push(app);
      await expectElementKind(
        await app.getById('file_transfer_prompt_entry'),
        'entry'
      ).setText('alice');
      await expectElementKind(
        await app.getById('file_transfer_prompt_secondary_entry'),
        'entry'
      ).setText('secret');
      await expectElementKind(
        await app.getById('file_transfer_prompt_accept_button'),
        'button'
      ).click();

      if (invalidCertificate) {
        const title = expectElementKind(
          await app.getById('file_transfer_prompt_title_label'),
          'label'
        );
        await waitForResult(async () => {
          expect(await title.text()).toBe(
            japanese
              ? 'FTPS証明書の検証に失敗しました'
              : 'FTPS certificate validation failed'
          );
        });
        if (!dataCertificateFailure) {
          expect(serverLog).not.toContain('COMMAND USER');
          expect(serverLog).not.toContain('COMMAND PASS');
        } else expect(serverLog).toContain('COMMAND USER');
        const message = await expectElementKind(
          await app.getById('file_transfer_prompt_message_label'),
          'label'
        ).text();
        const fingerprint = (
          await execute('openssl', [
            'x509',
            '-in',
            dataCertificateFailure ? dataCertificate : certificate,
            '-noout',
            '-fingerprint',
            '-sha256',
          ])
        ).stdout
          .trim()
          .split('=')[1];
        expect(message.replace(/\n/g, '')).toContain(fingerprint);
        for (const detail of [
          '127.0.0.1',
          ready.slice(6),
          ...(japanese
            ? [
                '制御接続',
                '主体:',
                '発行者:',
                '有効期間の開始:',
                '有効期間の終了:',
                '例外は保存されません。',
              ]
            : [
                dataCertificateFailure
                  ? 'Data connection'
                  : 'Control connection',
                'Subject:',
                'Issuer:',
                'Valid from:',
                'Valid until:',
                'Exceptions are not saved.',
              ]),
        ])
          expect(message).toContain(detail);
        const overlay = await app.getById('file_transfer_prompt_panel');
        await evidence.captureEvidence('certificate-pending', async () =>
          app.capture()
        );
        const cancel = expectElementKind(
          await app.getById('file_transfer_prompt_cancel_button'),
          'button'
        );
        expect((await cancel.info()).states).toContain('focused');
        if (testCase === 'ftps-prompt-close') {
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
        } else if (testCase === 'ftps-prompt-abort' || japanese)
          await cancel.click();
        else if (testCase === 'ftps-prompt-escape')
          await app.input.pressKey('Escape');
        else if (testCase === 'ftps-prompt-enter')
          await app.input.pressKey('Return');
        else if (testCase === 'ftps-prompt-allow') {
          const screen = PNG.sync.read((await app.capture()).image);
          const area = (await overlay.capture()).bounds;
          const environment = await app.environment();
          const videoPath = join(
            evidence.directory,
            'ftps-certificate-confirmation.mkv'
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
              'certificate decision pending without FTP authentication'
            );
            expect(serverLog).not.toContain('COMMAND USER');
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
            await evidence.log('FTPS certificate video states', {
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
            join(evidence.directory, 'ftps-certificate-frame-%d.png'),
          ]);
          for (let index = 0; index < states.length; index++) {
            const saved = PNG.sync.read(
              await readFile(
                join(
                  evidence.directory,
                  `ftps-certificate-frame-${index + 1}.png`
                )
              )
            );
            expect(
              difference(saved.data, saved.width, false, states[index].pixels)
            ).toBeLessThan(0.01);
          }
        } else
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
        if (
          [
            'ftps-prompt-close',
            'ftps-prompt-abort',
            'ftps-prompt-ja',
            'ftps-prompt-escape',
            'ftps-prompt-enter',
          ].includes(testCase)
        ) {
          await waitForResult(async () => {
            const output = await app.output();
            expect(output.exitCode, output.stderr).toBe(0);
            expect(output.exitSignal).toBeNull();
          });
          expect(serverLog).not.toContain('COMMAND USER');
          expect(serverLog).not.toContain('COMMAND PASS');
          return;
        }
      }
      if (dataCertificateFailure) {
        const dialog = await app.getById(
          'file_transfer_operation_error_dialog'
        );
        expect(
          serverLog
            .split('\n')
            .filter((line) => /^COMMAND (MLSD|LIST)$/.test(line))
        ).toHaveLength(1);
        const pending: GtkWidgetElement[] = [dialog];
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
              index++
            ) {
              const child = await widget.childAt(index);
              if (child) pending.push(child);
            }
        }
        expect(closed).toBe(true);
        await expectElementKind(
          await app.getById('file_transfer_remote_refresh_button'),
          'button'
        ).click();
      }
      if (rejectedCertificate) {
        await waitForResult(async () =>
          expect(
            await expectElementKind(
              await app.getById('file_transfer_prompt_title_label'),
              'label'
            ).text()
          ).toBe('Failed to start FTPS')
        );
        const close = expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        );
        expect((await close.info()).name).toBe('Close');
        expect(
          (
            await (
              await app.getById('file_transfer_prompt_cancel_button')
            ).info()
          ).states
        ).not.toContain('showing');
        expect(serverLog).not.toContain('COMMAND USER');
        expect(serverLog).not.toContain('COMMAND PASS');
        await evidence.captureEvidence('certificate-rejected', async () =>
          app.capture()
        );
        await close.click();
        await waitForResult(async () => {
          const output = await app.output();
          expect(output.exitCode, output.stderr).toBe(0);
          expect(output.exitSignal).toBeNull();
        });
        expect(serverLog).not.toContain('COMMAND USER');
        expect(serverLog).not.toContain('COMMAND PASS');
        return;
      } else if (scenario === 'login-refused') {
        await waitForResult(async () => {
          expect(
            await expectElementKind(
              await app.getById('file_transfer_prompt_title_label'),
              'label'
            ).text()
          ).toBe('Failed to start FTP');
          expect(
            await expectElementKind(
              await app.getById('file_transfer_prompt_message_label'),
              'label'
            ).text()
          ).toContain('530');
        });
      } else {
        const localTree = expectElementKind(
          await app.getById('file_transfer_local_tree'),
          'table'
        ) as GtkTableElement;
        const remoteTree = expectElementKind(
          await app.getById('file_transfer_remote_tree'),
          'table'
        ) as GtkTableElement;
        const rowFor = async (tree: GtkTableElement, name: string) =>
          await waitForResult(async () => {
            expect((await tree.info()).states).toContain('sensitive');
            for (let index = 0; index < (await tree.getRowCount()); index++) {
              if ((await (await tree.cellAt(index, 0))?.info())?.name === name)
                return index;
            }
            throw new Error(`Missing FTP row: ${name}`);
          });
        const contextMenu = async (tree: GtkTableElement, name: string) => {
          await waitForResult(async () => {
            expect((await localTree.info()).states).toContain('sensitive');
            expect((await remoteTree.info()).states).toContain('sensitive');
          });
          const row = await rowFor(tree, name);
          await tree.selectRow(row);
          const cell = await tree.cellAt(row, 0);
          expect((await cell?.info())?.name).toBe(name);
          const bounds = (await cell!.capture()).bounds;
          await app.input.moveMouseTo(
            Math.round(bounds.x + bounds.width / 2),
            Math.round(bounds.y + bounds.height / 2)
          );
          await app.input.setMouseButton('right', true);
          await app.input.setMouseButton('right', false);
        };
        await rowFor(remoteTree, 'download.bin');
        expect(
          await expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          ).text()
        ).toBe('/home');
        await contextMenu(
          closingDownload ? remoteTree : localTree,
          closingDownload ? 'download.bin' : 'upload.bin'
        );
        await expectElementKind(
          await app.getById(
            closingDownload
              ? 'file_transfer_receive_item'
              : 'file_transfer_send_item'
          ),
          'menuItem'
        ).click();
        if (held) {
          await waitForResult(async () => {
            expect(events, serverLog).toContain('FINAL_WAIT');
          });
          const overlay = await app.getById('file_transfer_overlay');
          expect((await overlay.info()).states).toContain('showing');
          const destination = closingDownload ? local : remote;
          expect(await readdir(destination)).not.toContain(
            closingDownload ? 'download.bin' : 'upload.bin'
          );
          await waitForResult(async () => {
            expect(
              (await readdir(destination)).some((name) =>
                name.includes('.elder-terms-part-')
              )
            ).toBe(true);
          });
          await evidence.captureEvidence('ftp-final-response-held', async () =>
            overlay.capture()
          );
          if (scenario === 'cancel-final') {
            const screen = PNG.sync.read((await app.capture()).image);
            const area = (await overlay.capture()).bounds;
            const environment = await app.environment();
            const videoPath = join(evidence.directory, 'ftp-cancellation.mkv');
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
                if (states.length > 0) {
                  // A disabled button alone does not show that the progress
                  // overlay has disappeared from the rendered video.
                  expect(
                    difference(
                      captured.data,
                      captured.width,
                      false,
                      states[states.length - 1].pixels
                    )
                  ).toBeGreaterThan(0.2);
                }
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
              await expectVideoState('waiting for the final FTP response');
              await expectElementKind(
                await app.getById('file_transfer_cancel_button'),
                'button'
              ).click();
              // The server still withholds 226 and waits for the client's EOF.
              // Cancellation must reach GTK and terminate the operation itself.
              server.stdin!.write('cancel\n');
              await waitForResult(async () => {
                expect((await overlay.info()).states).not.toContain('showing');
              });
              expect(await readdir(remote)).not.toContain('upload.bin');
              await expectVideoState('canceled with no committed destination');
              expect(states[1].frame).toBeGreaterThan(states[0].frame);
              expect(states[1].pixels.equals(states[0].pixels)).toBe(false);
            } finally {
              recorder.stdin.write('q\n');
              const code = await finished;
              await evidence.log('FTP cancellation video states', {
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
              join(evidence.directory, 'ftp-frame-%d.png'),
            ]);
            for (let index = 0; index < states.length; index++) {
              const saved = PNG.sync.read(
                await readFile(
                  join(evidence.directory, `ftp-frame-${index + 1}.png`)
                )
              );
              expect(
                difference(saved.data, saved.width, false, states[index].pixels)
              ).toBeLessThan(0.01);
            }
          }
        } else {
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_status_label'),
                'label'
              ).text()
            ).toBe(
              'Sent 1 item' +
                (invalidCertificate ? ' — Certificate exception active' : '')
            );
          });
          expect(await readFile(join(remote, 'upload.bin'))).toEqual(upload);
          await rowFor(remoteTree, 'upload.bin');
          await contextMenu(remoteTree, 'download.bin');
          await expectElementKind(
            await app.getById('file_transfer_receive_item'),
            'menuItem'
          ).click();
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_status_label'),
                'label'
              ).text()
            ).toBe(
              'Received 1 item' +
                (invalidCertificate ? ' — Certificate exception active' : '')
            );
          });
          expect(await readFile(join(local, 'download.bin'))).toEqual(download);
          await contextMenu(remoteTree, 'upload.bin');
          await expectElementKind(
            await app.getById('file_transfer_remote_rename_item'),
            'menuItem'
          ).click();
          await expectElementKind(
            await app.getById('file_transfer_prompt_entry'),
            'entry'
          ).setText('renamed 日本語.bin');
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
          await rowFor(remoteTree, 'renamed 日本語.bin');
          expect(await readFile(join(remote, 'renamed 日本語.bin'))).toEqual(
            upload
          );
          await contextMenu(remoteTree, 'renamed 日本語.bin');
          await expectElementKind(
            await app.getById('file_transfer_remote_delete_item'),
            'menuItem'
          ).click();
          await expectElementKind(
            await app.getById('file_transfer_prompt_accept_button'),
            'button'
          ).click();
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_status_label'),
                'label'
              ).text()
            ).toBe(
              'Deleted 1 item' +
                (invalidCertificate ? ' — Certificate exception active' : '')
            );
          });
          expect(await readdir(remote)).not.toContain('renamed 日本語.bin');
          expect(
            (await readdir(remote)).some((name) =>
              name.includes('.elder-terms-part-')
            )
          ).toBe(false);
          // Navigate a real server directory using the existing path entry.
          const path = expectElementKind(
            await app.getById('file_transfer_remote_path_entry'),
            'entry'
          );
          await waitForResult(async () => {
            expect((await path.info()).states).toContain('sensitive');
          });
          await path.setText('/home/archive');
          const pathBounds = (await path.capture()).bounds;
          await app.input.moveMouseTo(
            Math.round(pathBounds.x + pathBounds.width / 2),
            Math.round(pathBounds.y + pathBounds.height / 2)
          );
          await app.input.setMouseButton('left', true);
          await app.input.setMouseButton('left', false);
          await app.input.pressKey('Return');
          await rowFor(remoteTree, 'nested.txt');
          expect(await path.text()).toBe('/home/archive');
        }
      }
      const window = expectElementKind(
        await app.getById('file_transfer_window'),
        'window'
      );
      // Xvfb has no window manager to handle Alt+F4. Exercise the same
      // title-bar action as an ordinary close, including its GTK destroy path.
      const clickClose = async (root: GtkWidgetElement) => {
        const pending: GtkWidgetElement[] = [root];
        while (pending.length > 0) {
          const widget = pending.shift()!;
          if (
            widget.kind === 'button' &&
            (await widget.info()).name === 'Close'
          ) {
            await widget.click();
            return;
          }
          if ('getChildCount' in widget) {
            const count = await widget.getChildCount();
            for (let index = 0; index < count; index++) {
              const child = await widget.childAt(index);
              if (child !== undefined) pending.push(child);
            }
          }
        }
        throw new Error('The GTK Close button is missing');
      };
      if (scenario === 'cancel-final') {
        // Refresh reports the closed connection after an interrupted transfer.
        // Dismiss that modal notice before interacting with the title bar.
        await clickClose(
          await app.getById('file_transfer_operation_error_dialog')
        );
      }
      if (testCase === 'passive' || testCase === 'ftps-explicit') {
        server.kill('SIGTERM');
        await serverFinished;
        const refresh = expectElementKind(
          await app.getById('file_transfer_remote_refresh_button'),
          'button'
        );
        await refresh.click();
        const reconnect = expectElementKind(
          await app.getById('file_transfer_reconnect_button'),
          'button'
        );
        await waitForResult(async () => {
          expect((await reconnect.info()).states).toContain('showing');
          expect(
            (await (await app.getById('file_transfer_dim_overlay')).info())
              .states
          ).toContain('showing');
          expect(
            await expectElementKind(
              await app.getById('file_transfer_status_label'),
              'label'
            ).text()
          ).toBe('Disconnected');
        });
        await evidence.captureEvidence('ftp-disconnected', async () =>
          window.capture()
        );
        // The disconnected overlay must keep local browsing usable.
        await expectElementKind(
          await app.getById('file_transfer_local_refresh_button'),
          'button'
        ).click();
        const restartArgs = [
          ...server.spawnargs.slice(1),
          '--port=' + ready.slice(6),
        ];
        server = spawn(serverPath, restartArgs, {
          stdio: ['pipe', 'pipe', 'pipe'],
        });
        let restarted = false;
        const restartLines = createInterface({ input: server.stdout! });
        restartLines.on('line', (line) => {
          if (line.startsWith('READY ')) restarted = true;
        });
        server.stderr!.on('data', (bytes: Buffer) => {
          serverLog += bytes.toString();
        });
        serverFinished = new Promise((resolve) => {
          server!.once('error', (error) => {
            serverError = error;
            resolve(null);
          });
          server!.once('close', resolve);
        });
        await waitForResult(async () => {
          if (serverError) throw serverError;
          expect(restarted, serverLog).toBe(true);
        });
        await writeFile(
          join(remote, 'archive', 'reconnected.txt'),
          'new listing after restart'
        );
        await reconnect.click();
        await expectElementKind(
          await app.getById('file_transfer_prompt_entry'),
          'entry'
        ).setText('alice');
        await expectElementKind(
          await app.getById('file_transfer_prompt_secondary_entry'),
          'entry'
        ).setText('secret');
        await expectElementKind(
          await app.getById('file_transfer_prompt_accept_button'),
          'button'
        ).click();
        const tree = expectElementKind(
          await app.getById('file_transfer_remote_tree'),
          'table'
        ) as GtkTableElement;
        await waitForResult(async () => {
          expect((await tree.info()).states).toContain('sensitive');
          const names = [];
          for (let row = 0; row < (await tree.getRowCount()); row++)
            names.push((await (await tree.cellAt(row, 0))?.info())?.name);
          expect(names).toContain('reconnected.txt');
          expect((await reconnect.info()).states).not.toContain('showing');
          expect(
            (await (await app.getById('file_transfer_dim_overlay')).info())
              .states
          ).not.toContain('showing');
        });
      }
      if (legacyTls) {
        const channels = serverLog
          .split('\n')
          .filter(
            (line) =>
              line.startsWith('TLS CONTROL') || line.startsWith('TLS DATA')
          );
        expect(channels.length).toBeGreaterThan(1);
        expect(channels.every((line) => line.split(' ')[2] === 'TLSv1')).toBe(
          true
        );
      }
      await evidence.captureEvidence('ftp-before-window-close', async () =>
        window.capture()
      );
      await clickClose(await app.getById('file_transfer_header_bar'));
      if (scenario.startsWith('close-final')) server.stdin!.write('cancel\n');
      await waitForResult(async () => {
        const output = await app.output();
        expect(output.exitCode, output.stderr).toBe(0);
        expect(output.exitSignal).toBeNull();
      });
      if (scenario === 'close-final-download') {
        const remaining = await readdir(local);
        expect(remaining).not.toContain('download.bin');
        expect(
          remaining.some((name) => name.includes('.elder-terms-part-'))
        ).toBe(false);
      }
    } catch (error) {
      const app = apps[apps.length - 1];
      if (app) {
        try {
          await evidence.captureEvidence('ftp-failure', async () =>
            app.capture()
          );
        } catch (captureError) {
          await evidence.log('FTP failure capture unavailable', {
            error: captureError,
          });
        }
      }
      throw error;
    } finally {
      try {
        await evidence.log('FTP server events', { events, stderr: serverLog });
        await evidence.flushOutputs(apps, launcher);
      } finally {
        if (launcher) await launcher.release();
        if (server) {
          server.stdin?.end();
          server.kill('SIGTERM');
        }
        if (serverFinished) await serverFinished;
        await evidence.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  }, 120_000);
}
