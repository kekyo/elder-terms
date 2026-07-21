import { readFile, writeFile } from 'node:fs/promises';
import { createServer, type Server, type Socket } from 'node:net';
import { join } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { describe, expect, it } from 'vitest';
import { waitForResult, toPass } from 'gestament/testing';
import { waitForActivityIndicatorImageState } from './activity-indicator-test-helpers';
import {
  runGtkTest,
  telnetLocalhostConfigPath,
  withTemporaryDirectory,
} from './gtk-test-helpers';

const telnetSe = 240;
const telnetSb = 250;
const telnetWill = 251;
const telnetWont = 252;
const telnetDo = 253;
const telnetDont = 254;
const telnetIac = 255;
const telnetBinary = 0;
const telnetNaws = 31;
const asciiBs = 8;
const asciiDel = 127;
const xtermDeleteSequence = [0x1b, 0x5b, 0x33, 0x7e];

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

describe.concurrent('elder-terms-vte TELNET session', () => {
  it('negotiates BINARY and NAWS after connecting, tolerates BINARY disable, and sends user input', async (context) => {
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
                  80,
                  0,
                  24,
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
              message: 'successful initial BINARY negotiation should be logged',
              timeoutMs: 5_000,
            }
          );

          await waitForActivityIndicatorImageState(app, 'rd', 'on');
          await waitForActivityIndicatorImageState(app, 'sd', 'on');
          stopNegotiationInterval();
          await delay(500);
          await waitForActivityIndicatorImageState(app, 'rd', 'off');
          await waitForActivityIndicatorImageState(app, 'sd', 'off');

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
        });
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

  it('sends the default Backspace as BS and Delete as ASCII DEL', async (context) => {
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
              message: 'TELNET client should send Backspace as ASCII BS',
              timeoutMs: 5_000,
            }
          );

          const baselineLength = receivedBytes(receivedChunks).length;
          await app.input.pressKey('Delete');
          await toPass(
            async () => {
              const newData =
                receivedBytes(receivedChunks).slice(baselineLength);
              expect(newData).toContain(asciiDel);
              expect(hasSubsequence(newData, xtermDeleteSequence)).toBe(false);
            },
            {
              message: 'TELNET client should send Delete as ASCII DEL',
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
});
