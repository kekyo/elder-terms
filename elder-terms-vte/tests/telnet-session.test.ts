import { execFile } from 'node:child_process';
import { readFile, writeFile } from 'node:fs/promises';
import { createServer, type Server, type Socket } from 'node:net';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { promisify } from 'node:util';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import {
  expectMainWindowStatus,
  runGtkTest,
  telnetLocalhostConfigPath,
  withTemporaryDirectory,
} from './gtk-test-helpers';
import { openTerminalContextMenu } from './clipboard-test-helpers';
import { expectElementKind } from './test-helpers';
import {
  activateTextSend,
  expectTextSendActive,
  expectTextSendFinished,
} from './text-send-test-helpers';

const telnetSe = 240;
const telnetBrk = 243;
const telnetSb = 250;
const telnetWill = 251;
const telnetWont = 252;
const telnetDo = 253;
const telnetDont = 254;
const telnetIac = 255;
const telnetBinary = 0;
const telnetTerminalType = 24;
const telnetNaws = 31;
const telnetTerminalTypeIs = 0;
const telnetTerminalTypeSend = 1;
const asciiBs = 8;
const asciiDel = 127;
const xtermDeleteSequence = [0x1b, 0x5b, 0x33, 0x7e];
const execFileAsync = promisify(execFile);

const listenOnLocalhost = async (server: Server): Promise<number> =>
  new Promise<number>((resolve, reject) => {
    const rejectFromError = (error: Error): void => {
      reject(error);
    };
    server.once('error', rejectFromError);
    server.listen(0, '127.0.0.1', () => {
      server.off('error', rejectFromError);
      const address = server.address();
      if (address === null || typeof address === 'string') {
        reject(new Error('Server did not expose a TCP port.'));
        return;
      }
      resolve(address.port);
    });
  });

const closeServer = async (server: Server): Promise<void> =>
  new Promise<void>((resolve, reject) => {
    if (!server.listening) {
      resolve();
      return;
    }
    server.close((error) => {
      if (error === undefined) {
        resolve();
      } else {
        reject(error);
      }
    });
  });

const receivedBytes = (chunks: readonly Buffer[]): readonly number[] =>
  Array.from(Buffer.concat(chunks).values());

const hasSubsequence = (
  bytes: readonly number[],
  sequence: readonly number[]
): boolean => {
  for (let index = 0; index <= bytes.length - sequence.length; ++index) {
    const matched = sequence.every(
      (value, offset) => bytes[index + offset] === value
    );
    if (matched) {
      return true;
    }
  }
  return false;
};

const countSubsequence = (
  bytes: readonly number[],
  sequence: readonly number[]
): number => {
  let count = 0;
  for (let index = 0; index <= bytes.length - sequence.length; ++index) {
    if (sequence.every((value, offset) => bytes[index + offset] === value)) {
      ++count;
    }
  }
  return count;
};

describe.concurrent('elder-terms-vte TELNET session', () => {
  it('sends BREAK from its shortcut once per press and from the context menu', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const receivedChunks: Buffer[] = [];
      let acceptedSocket: Socket | undefined;
      let binaryAccepted = false;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.on('data', (chunk) => {
          receivedChunks.push(Buffer.from(chunk));
          const data = receivedBytes(receivedChunks);
          if (
            !binaryAccepted &&
            hasSubsequence(data, [telnetIac, telnetWill, telnetBinary]) &&
            hasSubsequence(data, [telnetIac, telnetDo, telnetBinary])
          ) {
            binaryAccepted = true;
            socket.write(
              Buffer.from([
                telnetIac,
                telnetDo,
                telnetBinary,
                telnetIac,
                telnetWill,
                telnetBinary,
              ])
            );
          }
        });
        socket.write('connected\r\n');
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet-break.ini');
        await writeFile(
          configPath,
          `[general]\ntype=telnet\nname=break\n\n[terminal]\nauto_close=false\nsend_break_key=F12\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(async () => {
            expect(acceptedSocket).not.toBeUndefined();
            expect(binaryAccepted).toBe(true);
          });
          await expectMainWindowStatus(app, `telnet: 127.0.0.1:${port}`);

          const baseline = receivedBytes(receivedChunks).length;
          const mainWindow = expectElementKind(
            await app.getById('main_window'),
            'window'
          );
          await mainWindow.activate();
          const environment = await app.environment();
          try {
            await execFileAsync('/usr/bin/xdotool', ['keydown', 'F12'], {
              env: environment,
            });
            await execFileAsync('/usr/bin/xdotool', ['keydown', 'F12'], {
              env: environment,
            });
          } finally {
            await execFileAsync('/usr/bin/xdotool', ['keyup', 'F12'], {
              env: environment,
            });
          }

          await expectMainWindowStatus(app, 'BREAK sent');
          await toPass(
            async () => {
              expect(
                countSubsequence(
                  receivedBytes(receivedChunks).slice(baseline),
                  [telnetIac, telnetBrk]
                )
              ).toBe(1);
            },
            {
              message: 'F12 should send one TELNET BREAK command',
              timeoutMs: 5_000,
            }
          );

          const contextBaseline = receivedBytes(receivedChunks).length;
          await openTerminalContextMenu(app);
          const breakItem = await waitForResult(async () => {
            const item = expectElementKind(
              await app.getById('terminal_context_break_item'),
              'menuItem'
            );
            const info = await item.info();
            expect(info.name).toBe('Send BREAK');
            expect(info.states).toContain('showing');
            expect(info.states).toContain('enabled');
            expect(info.states).toContain('sensitive');
            return item;
          });
          await breakItem.click();
          await expectMainWindowStatus(app, 'BREAK sent');
          await toPass(async () => {
            expect(
              countSubsequence(
                receivedBytes(receivedChunks).slice(contextBaseline),
                [telnetIac, telnetBrk]
              )
            ).toBe(1);
          });
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('negotiates BINARY and maximized NAWS after connecting, tolerates BINARY disable, and sends user input', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const receivedChunks: Buffer[] = [];
      let acceptedSocket: Socket | undefined;
      let binaryAccepted = false;
      let negotiationInterval: ReturnType<typeof setInterval> | undefined;
      const stopNegotiationInterval = (): void => {
        if (negotiationInterval !== undefined) {
          clearInterval(negotiationInterval);
          negotiationInterval = undefined;
        }
      };
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.on('data', (chunk) => {
          receivedChunks.push(
            typeof chunk === 'string' ? Buffer.from(chunk) : Buffer.from(chunk)
          );
          const data = receivedBytes(receivedChunks);
          if (
            !binaryAccepted &&
            hasSubsequence(data, [telnetIac, telnetWill, telnetBinary]) &&
            hasSubsequence(data, [telnetIac, telnetDo, telnetBinary])
          ) {
            binaryAccepted = true;
            socket.write(
              Buffer.from([
                telnetIac,
                telnetDo,
                telnetBinary,
                telnetIac,
                telnetWill,
                telnetBinary,
              ])
            );
          }
        });
        socket.on('close', stopNegotiationInterval);
        const writeNawsRequest = (): void => {
          socket.write(Buffer.from([telnetIac, telnetDo, telnetNaws]));
        };
        writeNawsRequest();
        negotiationInterval = setInterval(writeNawsRequest, 60);
        socket.write('connected\r\n');
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        await writeFile(
          configPath,
          configTemplate.replace('${port}', String(port)),
          'utf8'
        );

        await runGtkTest(
          context,
          [
            '--test-maximize-window',
            '--test-latch-activity-indicators',
            '-c',
            configPath,
          ],
          async (app) => {
            await toPass(
              async () => {
                expect(acceptedSocket).not.toBeUndefined();
              },
              {
                message: 'TELNET server should accept a client connection',
                timeoutMs: 5_000,
              }
            );

            await toPass(
              async () => {
                const data = receivedBytes(receivedChunks);
                expect(
                  hasSubsequence(data, [telnetIac, telnetWill, telnetBinary])
                ).toBe(true);
                expect(
                  hasSubsequence(data, [telnetIac, telnetDo, telnetBinary])
                ).toBe(true);
                expect(binaryAccepted).toBe(true);
                expect(
                  hasSubsequence(data, [telnetIac, telnetWill, telnetNaws])
                ).toBe(true);
                expect(
                  hasSubsequence(data, [
                    telnetIac,
                    telnetSb,
                    telnetNaws,
                    0,
                    82,
                    0,
                    26,
                    telnetIac,
                    telnetSe,
                  ])
                ).toBe(true);
              },
              {
                message: 'TELNET client should negotiate BINARY and NAWS',
                timeoutMs: 5_000,
              }
            );

            await toPass(
              async () => {
                expect((await app.output()).stderr).toContain(
                  'Info: TELNET BINARY negotiation succeeded after connection'
                );
              },
              {
                message:
                  'successful initial BINARY negotiation should be logged',
                timeoutMs: 5_000,
              }
            );

            await waitForActivityIndicatorImageState(app, 'rd', 'on');
            await waitForActivityIndicatorImageState(app, 'sd', 'on');
            stopNegotiationInterval();

            const disableBaselineLength = receivedBytes(receivedChunks).length;
            acceptedSocket?.write(
              Buffer.from([
                telnetIac,
                telnetDont,
                telnetBinary,
                telnetIac,
                telnetWont,
                telnetBinary,
              ])
            );
            await toPass(
              async () => {
                const newData = receivedBytes(receivedChunks).slice(
                  disableBaselineLength
                );
                expect(
                  hasSubsequence(newData, [telnetIac, telnetWont, telnetBinary])
                ).toBe(true);
                expect(
                  hasSubsequence(newData, [telnetIac, telnetDont, telnetBinary])
                ).toBe(true);
              },
              {
                message: 'TELNET client should tolerate BINARY disable',
                timeoutMs: 5_000,
              }
            );

            await app.input.pressKey('a');
            await toPass(
              async () => {
                const data = receivedBytes(receivedChunks);
                expect(data).toContain('a'.charCodeAt(0));
              },
              {
                message: 'TELNET client should send VTE user input',
                timeoutMs: 5_000,
              }
            );
          }
        );
      } finally {
        stopNegotiationInterval();
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('continues the session without an error when initial BINARY negotiation is rejected', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const receivedChunks: Buffer[] = [];
      let acceptedSocket: Socket | undefined;
      let binaryRejected = false;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.on('data', (chunk) => {
          receivedChunks.push(
            typeof chunk === 'string' ? Buffer.from(chunk) : Buffer.from(chunk)
          );
          const data = receivedBytes(receivedChunks);
          if (
            !binaryRejected &&
            hasSubsequence(data, [telnetIac, telnetWill, telnetBinary]) &&
            hasSubsequence(data, [telnetIac, telnetDo, telnetBinary])
          ) {
            binaryRejected = true;
            socket.write(
              Buffer.from([
                telnetIac,
                telnetDont,
                telnetBinary,
                telnetIac,
                telnetWont,
                telnetBinary,
              ])
            );
          }
        });
        socket.write('connected\r\n');
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        await writeFile(
          configPath,
          configTemplate.replace('${port}', String(port)),
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(
            async () => {
              expect(binaryRejected).toBe(true);
            },
            {
              message: 'TELNET server should reject initial BINARY negotiation',
              timeoutMs: 5_000,
            }
          );

          const baselineLength = receivedBytes(receivedChunks).length;
          await app.input.pressKey('b');
          await toPass(
            async () => {
              const newData =
                receivedBytes(receivedChunks).slice(baselineLength);
              expect(newData).toContain('b'.charCodeAt(0));
            },
            {
              message: 'TELNET client should continue after BINARY rejection',
              timeoutMs: 5_000,
            }
          );
          expect((await app.output()).stderr).not.toContain('Warning: TELNET');
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('reports xterm when an RGB background selects the built-in terminal type', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const receivedChunks: Buffer[] = [];
      let acceptedSocket: Socket | undefined;
      let terminalTypeRequested = false;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.on('data', (chunk) => {
          receivedChunks.push(Buffer.from(chunk));
          const data = receivedBytes(receivedChunks);
          if (
            !terminalTypeRequested &&
            hasSubsequence(data, [telnetIac, telnetWill, telnetTerminalType])
          ) {
            terminalTypeRequested = true;
            socket.write(
              Buffer.from([
                telnetIac,
                telnetSb,
                telnetTerminalType,
                telnetTerminalTypeSend,
                telnetIac,
                telnetSe,
              ])
            );
          }
        });
        socket.write(Buffer.from([telnetIac, telnetDo, telnetTerminalType]));
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        await writeFile(
          configPath,
          configTemplate
            .replace('[general]\n', '[general]\nbackground=#604020\n')
            .replace('${port}', String(port)),
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async () => {
          await toPass(
            async () => {
              const data = receivedBytes(receivedChunks);
              expect(terminalTypeRequested).toBe(true);
              expect(
                hasSubsequence(data, [
                  telnetIac,
                  telnetSb,
                  telnetTerminalType,
                  telnetTerminalTypeIs,
                  'x'.charCodeAt(0),
                  't'.charCodeAt(0),
                  'e'.charCodeAt(0),
                  'r'.charCodeAt(0),
                  'm'.charCodeAt(0),
                  telnetIac,
                  telnetSe,
                ])
              ).toBe(true);
            },
            {
              message:
                'TELNET client should report the background-dependent ' +
                'built-in terminal type',
              timeoutMs: 5_000,
            }
          );
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('uses the VTE automatic Backspace and Delete bindings by default', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const receivedChunks: Buffer[] = [];
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.on('data', (chunk) => {
          receivedChunks.push(
            typeof chunk === 'string' ? Buffer.from(chunk) : Buffer.from(chunk)
          );
        });
        socket.write('connected\r\n');
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        await writeFile(
          configPath,
          configTemplate.replace('${port}', String(port)),
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
            },
            {
              message: 'TELNET server should accept a client connection',
              timeoutMs: 5_000,
            }
          );

          const backspaceBaselineLength = receivedBytes(receivedChunks).length;
          await app.input.pressKey('BackSpace');
          await toPass(
            async () => {
              const newData = receivedBytes(receivedChunks).slice(
                backspaceBaselineLength
              );
              expect(newData).toContain(asciiBs);
            },
            {
              message:
                'TELNET client should let VTE send Backspace as ASCII BS',
              timeoutMs: 5_000,
            }
          );

          const baselineLength = receivedBytes(receivedChunks).length;
          await app.input.pressKey('Delete');
          await toPass(
            async () => {
              const newData =
                receivedBytes(receivedChunks).slice(baselineLength);
              expect(hasSubsequence(newData, xtermDeleteSequence)).toBe(true);
              expect(newData).not.toContain(asciiDel);
            },
            {
              message:
                'TELNET client should let VTE generate the Delete sequence',
              timeoutMs: 5_000,
            }
          );
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('exits when the TELNET server closes the connection and terminal auto_close is enabled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.end();
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        await writeFile(
          configPath,
          configTemplate.replace('${port}', String(port)),
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app, evidence) => {
          await toPass(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
            },
            {
              message: 'TELNET server should accept a client connection',
              timeoutMs: 5_000,
            }
          );

          const output = await waitForResult(
            async () => {
              const currentOutput = await app.output();
              expect(currentOutput.exitCode).toBe(0);
              expect(currentOutput.exitSignal).toBeNull();
              return currentOutput;
            },
            {
              message: 'app should exit after the TELNET connection closes',
              timeoutMs: 5_000,
            }
          );
          await evidence.log('TELNET close auto closed app', {
            exitCode: output.exitCode,
            exitSignal: output.exitSignal,
          });
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('keeps running when the TELNET server closes the connection and terminal auto_close is disabled', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      let acceptedSocketClosed = false;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.resume();
        socket.on('close', () => {
          acceptedSocketClosed = true;
        });
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        await writeFile(
          configPath,
          `${configTemplate.replace(
            '${port}',
            String(port)
          )}\n[terminal]\nauto_close=false\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app, evidence) => {
          await toPass(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
            },
            {
              message: 'TELNET server should accept a client connection',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          acceptedSocket?.end();
          await toPass(
            async () => {
              expect(acceptedSocketClosed).toBe(true);
            },
            {
              message: 'TELNET server socket should close',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'off');

          const output = await app.output();
          expect(output.exitCode).toBeNull();
          expect(output.exitSignal).toBeNull();
          await evidence.log('TELNET close left app running', {
            exitCode: output.exitCode,
            exitSignal: output.exitSignal,
          });
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('records cooked terminal output only while the TELNET connection is active', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      let acceptedSocket: Socket | undefined;
      const terminalText = 'terminal log text';
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.write(`${terminalText}\r\n`);
      });

      try {
        const port = await listenOnLocalhost(server);
        const configPath = join(directory, 'telnet.ini');
        const logPath = join(directory, 'logs', 'cooked.txt');
        const configTemplate = await readFile(
          telnetLocalhostConfigPath,
          'utf8'
        );
        await writeFile(
          configPath,
          `${configTemplate.replace(
            '${port}',
            String(port)
          )}\n[terminal]\nauto_close=false\n\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=logs/cooked.txt\nmode=cooked\n`,
          'utf8'
        );

        await runGtkTest(context, ['-c', configPath], async (app) => {
          await toPass(
            async () => {
              expect(acceptedSocket).not.toBeUndefined();
            },
            {
              message: 'TELNET server should accept a logging client',
              timeoutMs: 5_000,
            }
          );
          await waitForActivityIndicatorImageState(app, 'conn', 'on');
          await waitForActivityIndicatorImageState(app, 'log', 'on');

          acceptedSocket?.end();
          await waitForActivityIndicatorImageState(app, 'conn', 'off');
          await waitForActivityIndicatorImageState(app, 'log', 'off');
          await toPass(
            async () => {
              expect(await readFile(logPath, 'utf8')).toContain(terminalText);
            },
            {
              message: 'closed cooked log should contain terminal output',
              timeoutMs: 5_000,
            }
          );
        });
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });

  it('sends encoded text read-only while TELNET output remains active', async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const receivedChunks: Buffer[] = [];
      let acceptedSocket: Socket | undefined;
      const server = createServer((socket) => {
        acceptedSocket = socket;
        socket.on('data', (chunk) => {
          receivedChunks.push(Buffer.from(chunk));
        });
        socket.write('connected\r\n');
      });

      try {
        const port = await listenOnLocalhost(server);
        const sourcePath = join(directory, 'send.txt');
        const configPath = join(directory, 'telnet.ini');
        const logPath = join(directory, 'logs', 'cooked.txt');
        await writeFile(sourcePath, 'AÿB\r\nC\rD\n', 'utf8');
        await writeFile(
          configPath,
          `[general]\ntype=telnet\n\n[terminal]\nauto_close=false\nencoding=ISO-8859-1\n\n[telnet]\naddress=127.0.0.1\nport=${port}\n\n[transfer]\ntext_send_bytes_per_second=10\n\n[log]\nenabled=true\nbase_directory=${directory}\nfile_name_format=logs/cooked.txt\nmode=cooked\n`,
          'utf8'
        );

        await runGtkTest(
          context,
          [
            '-c',
            configPath,
            `--test-transfer-source-uri=${pathToFileURL(sourcePath).href}`,
          ],
          async (app) => {
            await toPass(async () => {
              expect(acceptedSocket).not.toBeUndefined();
            });
            await delay(200);
            const baseline = Buffer.concat(receivedChunks).length;
            const button = await activateTextSend(app);
            await expectTextSendActive(button);
            acceptedSocket?.write('TELNET_DURING_TEXT_SEND');
            await app.input.pressKey('x');
            await expectTextSendFinished(
              app,
              button,
              `telnet: 127.0.0.1:${port}`
            );

            await toPass(async () => {
              const sent = Array.from(
                Buffer.concat(receivedChunks).subarray(baseline).values()
              );
              expect(
                hasSubsequence(
                  sent,
                  [
                    // Auto emits CR for each logical newline, which TELNET NVT
                    // frames as CR NUL.
                    0x41, 0xff, 0xff, 0x42, 0x0d, 0x00, 0x43, 0x0d, 0x00, 0x44,
                    0x0d, 0x00,
                  ]
                )
              ).toBe(true);
              expect(sent).not.toContain(0x78);
              expect(await readFile(logPath, 'utf8')).toContain(
                'TELNET_DURING_TEXT_SEND'
              );
            });
          }
        );
      } finally {
        acceptedSocket?.destroy();
        await closeServer(server);
      }
    });
  });
});
