import { execFile, spawn } from 'node:child_process';
import {
  chmod,
  mkdtemp,
  mkdir,
  readFile,
  rm,
  stat,
  writeFile,
} from 'node:fs/promises';
import { createConnection } from 'node:net';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createGtkAppLauncher } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { runLauncherGtkTest } from './test-helpers';

const execute = promisify(execFile);
const ctl = fileURLToPath(
  new URL('../../.build/elder-terms/elder-termsctl', import.meta.url)
);

it('controls the resident launcher and saved connections without D-Bus or a display in the client', async (context) => {
  const runtime = await mkdtemp(join(tmpdir(), 'elder-control-'));
  const capture = join(runtime, 'capture.json');
  const child = join(runtime, 'child.mjs');
  await writeFile(
    child,
    `#!/usr/bin/env node\nimport fs from 'node:fs';\nfs.writeFileSync(${JSON.stringify(capture)}, JSON.stringify({args: process.argv.slice(2), token: process.env.XDG_ACTIVATION_TOKEN}));\n`
  );
  await chmod(child, 0o700);
  const env = {
    XDG_RUNTIME_DIR: runtime,
    ELDER_TERMS_VTE_PATH: child,
  };
  const control = async (args: string[]) =>
    await execute(ctl, args, {
      env: {
        ...process.env,
        ...env,
        DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/elder-terms-bus',
        DISPLAY: '',
        WAYLAND_DISPLAY: '',
        XDG_ACTIVATION_TOKEN: 'test-control-token',
      },
    });
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=background\nopen_application=\n'
        );
        await writeFile(
          join(connections, '日本語 connection.ini'),
          '[general]\ntype=local\n'
        );
      },
      async ({ app, connections }) => {
        expect(await app.getWindowCount()).toBe(0);
        await expect(
          control(['open-connection', 'missing'])
        ).rejects.toMatchObject({ code: 1 });
        await expect(
          control(['open-connection', '../global'])
        ).rejects.toMatchObject({ code: 1 });
        for (const request of [
          'unknown\0\0',
          'open-application\0unexpected\0',
          'x'.repeat(17000),
        ]) {
          const response = await new Promise<string>((resolve, reject) => {
            const socket = createConnection(
              join(runtime, 'elder-terms/control.sock')
            );
            let data = '';
            socket.setEncoding('utf8');
            socket.on('data', (chunk: string) => {
              data += chunk;
            });
            socket.on('error', reject);
            socket.on('end', () => resolve(data));
            socket.end(request);
          });
          expect(response).toMatch(/^ERROR /);
        }
        const idle = createConnection(
          join(runtime, 'elder-terms/control.sock')
        );
        try {
          await control(['open-application']);
        } finally {
          idle.destroy();
        }
        await waitForResult(async () =>
          expect(await app.getWindowCount()).toBe(1)
        );
        await control(['open-connection', '日本語 connection']);
        const launched = await waitForResult(
          async () =>
            JSON.parse(await readFile(capture, 'utf8')) as {
              args: string[];
              token: string;
            }
        );
        expect(launched.args).toContain(
          join(connections, '日本語 connection.ini')
        );
        expect(launched.token).toBe('test-control-token');
        expect(
          (await stat(join(runtime, 'elder-terms/control.sock'))).mode & 0o777
        ).toBe(0o600);
        await expect(control(['unknown-command'])).rejects.toMatchObject({
          code: 2,
        });
      },
      { args: [], env }
    );
    await expect(control(['open-application'])).rejects.toMatchObject({
      code: 1,
    });
  } finally {
    await rm(runtime, { recursive: true, force: true });
  }
});

it('explains usage without GTK and reports a missing resident launcher', async () => {
  const runtime = await mkdtemp(join(tmpdir(), 'elder-control-cli-'));
  const env = {
    ...process.env,
    XDG_RUNTIME_DIR: runtime,
    DISPLAY: '',
    WAYLAND_DISPLAY: '',
    DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/elder-terms-bus',
  };
  try {
    const help = await execute(ctl, ['--help'], { env });
    expect(help.stdout).toContain('open-connection');
    await expect(
      execute(ctl, ['open-connection'], { env })
    ).rejects.toMatchObject({ code: 2 });
    await expect(
      execute(ctl, ['open-application'], { env })
    ).rejects.toMatchObject({ code: 1 });
  } finally {
    await rm(runtime, { recursive: true, force: true });
  }
});

it('accepts requests when the launcher itself cannot connect to D-Bus and recovers after termination', async () => {
  const runtime = await mkdtemp(join(tmpdir(), 'elder-control-nobus-'));
  const binary = fileURLToPath(
    new URL('../../.build/elder-terms/elder-terms', import.meta.url)
  );
  const desktop = createGtkAppLauncher({
    appPath: binary,
    xvfbPool: { type: 'xvfb' },
    xvfbTrayHost: false,
  });
  const config = join(runtime, 'config');
  await mkdir(join(config, 'elder-terms/connections'), { recursive: true });
  await writeFile(
    join(config, 'elder-terms/global.ini'),
    '[general]\nstartup_mode=background\nopen_application=\n'
  );
  const capture = join(runtime, 'opened');
  const childPath = join(runtime, 'child.sh');
  await writeFile(childPath, '#!/bin/sh\nprintf opened > "' + capture + '"\n');
  await chmod(childPath, 0o700);
  await writeFile(
    join(config, 'elder-terms/connections/local.ini'),
    '[general]\ntype=local\n'
  );
  try {
    const env = {
      ...process.env,
      ...(await desktop.environment()),
      XDG_RUNTIME_DIR: runtime,
      XDG_CONFIG_HOME: config,
      DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/elder-terms-bus',
      ELDER_TERMS_VTE_PATH: childPath,
    };
    for (let attempt = 0; attempt < 2; ++attempt) {
      const launcherProcess = spawn(binary, [], { env, stdio: 'pipe' });
      let output = '';
      launcherProcess.stderr.on('data', (chunk: Buffer) => {
        output += chunk.toString();
      });
      const exited = new Promise<void>((resolve, reject) => {
        launcherProcess.once('error', reject);
        launcherProcess.once('exit', () => resolve());
      });
      try {
        await waitForResult(async () => {
          expect(launcherProcess.exitCode, output).toBeNull();
          await execute(ctl, ['open-application'], { env });
        });
        await execute(ctl, ['open-connection', 'local'], { env });
        await waitForResult(async () =>
          expect(await readFile(capture, 'utf8')).toBe('opened')
        );
        await rm(capture);
      } finally {
        launcherProcess.kill('SIGKILL');
        await exited;
      }
    }
  } finally {
    await desktop.release();
    await rm(runtime, { recursive: true, force: true });
  }
});
