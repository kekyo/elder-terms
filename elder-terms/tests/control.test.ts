import { execFile, spawn } from 'node:child_process';
import {
  chmod,
  copyFile,
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
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkEntryElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const execute = promisify(execFile);
const ctl = fileURLToPath(
  new URL('../../.build/elder-terms/etctl', import.meta.url)
);

const clickShortcutEntry = async (
  app: GtkApp,
  entry: GtkEntryElement,
  clearIcon: boolean
): Promise<void> => {
  await expectElementKind(
    await app.getById('application_dialog'),
    'window'
  ).activate();
  const { bounds } = await entry.capture();
  await app.input.moveMouseTo(
    Math.trunc(
      clearIcon ? bounds.x + bounds.width - 14 : bounds.x + bounds.width / 2
    ),
    Math.trunc(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
  await waitForResult(async () => {
    expect((await entry.info()).states).toContain('focused');
  });
};

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
          '[general]\nstartup_mode=background\nopen_application=Ctrl+Shift+Y\n'
        );
        await writeFile(
          join(connections, '日本語 connection.ini'),
          '[general]\ntype=local\n'
        );
      },
      async ({ app, connections }) => {
        expect(await app.getWindowCount()).toBe(0);
        expect((await control(['setup'])).stdout).toContain(
          'X11 hotkeys are registered'
        );
        await expect(
          control(['open-connection', 'missing'])
        ).rejects.toMatchObject({ code: 1 });
        await expect(
          control(['open-connection', '../global'])
        ).rejects.toMatchObject({ code: 1 });
        for (const request of [
          'unknown\0\0',
          'open-application\0unexpected\0',
          'open-application\0\0' + 'x'.repeat(17000),
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
    expect(help.stdout).toContain('setup');
    expect(help.stdout).toContain('unsetup');
    await expect(
      execute(ctl, ['open-connection'], { env })
    ).rejects.toMatchObject({ code: 2 });
    await expect(
      execute(ctl, ['open-application'], { env })
    ).rejects.toMatchObject({ code: 1 });
    await expect(execute(ctl, ['setup'], { env })).rejects.toMatchObject({
      code: 1,
    });
    const configHome = join(runtime, 'config');
    await mkdir(join(configHome, 'sway'), { recursive: true });
    await writeFile(
      join(configHome, 'sway/config'),
      "bindsym Mod4+Return exec terminal\n# elder-terms setup begin\nbindsym Ctrl+F2 exec 'etctl' 'open-application'\n# elder-terms setup end\n"
    );
    const unsetup = await execute(ctl, ['unsetup'], {
      env: {
        ...env,
        HOME: runtime,
        XDG_CONFIG_HOME: configHome,
        SWAYSOCK: '',
        LABWC_PID: '',
      },
    });
    expect(unsetup.stdout).toContain('removed');
    expect(await readFile(join(configHome, 'sway/config'), 'utf8')).toBe(
      'bindsym Mod4+Return exec terminal\n'
    );
    expect(
      (
        await execute(ctl, ['unsetup'], {
          env: {
            ...env,
            HOME: runtime,
            XDG_CONFIG_HOME: configHome,
            SWAYSOCK: '',
            LABWC_PID: '',
          },
        })
      ).stdout
    ).toContain('No setup-managed hotkeys');
  } finally {
    await rm(runtime, { recursive: true, force: true });
  }
});

it('starts the launcher when setup is run in an active X11 session', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-control-setup-'));
  const desktop = createGtkAppLauncher({
    appPath: fileURLToPath(
      new URL('../../.build/elder-terms/elder-terms', import.meta.url)
    ),
    xvfbPool: { type: 'xvfb' },
    xvfbTrayHost: false,
  });
  const bin = join(directory, 'bin');
  const config = join(directory, 'config');
  const runtime = join(directory, 'runtime');
  const pidFile = join(directory, 'launcher.pid');
  const launcherBinary = fileURLToPath(
    new URL('../../.build/elder-terms/elder-terms', import.meta.url)
  );
  let launcherPid: number | undefined;
  try {
    await mkdir(bin);
    await mkdir(runtime, { mode: 0o700 });
    await mkdir(join(config, 'elder-terms/connections'), { recursive: true });
    await writeFile(
      join(config, 'elder-terms/global.ini'),
      '[general]\nstartup_mode=background\nopen_application=Ctrl+Shift+Y\n'
    );
    await copyFile(ctl, join(bin, 'etctl'));
    await writeFile(
      join(bin, 'elder-terms'),
      `#!/bin/sh\nprintf '%s' "$$" > '${pidFile}'\nexec '${launcherBinary}' "$@"\n`
    );
    await chmod(join(bin, 'elder-terms'), 0o700);
    const env = {
      ...process.env,
      ...(await desktop.environment()),
      XDG_CONFIG_HOME: config,
      XDG_RUNTIME_DIR: runtime,
      DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/elder-terms-bus',
    };
    const setup = await execute(join(bin, 'etctl'), ['setup'], { env });
    expect(setup.stdout).toContain('X11 hotkeys are registered');
    launcherPid = Number(await readFile(pidFile, 'utf8'));
    expect(Number.isInteger(launcherPid)).toBe(true);
    await execute(join(bin, 'etctl'), ['open-application'], { env });
  } finally {
    if (launcherPid !== undefined) {
      try {
        process.kill(launcherPid);
      } catch {
        // The launcher may have exited during cleanup.
      }
    }
    await desktop.release();
    await rm(directory, { recursive: true, force: true });
  }
});

it('automatically configures the detected Sway session without OS labels', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-control-sway-'));
  const launcherBinary = fileURLToPath(
    new URL('../../.build/elder-terms/elder-terms', import.meta.url)
  );
  const wrapper = join(directory, 'launcher');
  const compositor = join(directory, 'swaymsg');
  const capture = join(directory, 'sway-calls');
  await writeFile(
    wrapper,
    `#!/bin/sh\nexport GDK_BACKEND=x11\nexport XDG_SESSION_TYPE=wayland\nexec '${launcherBinary}' "$@"\n`
  );
  await chmod(wrapper, 0o700);
  await writeFile(
    compositor,
    `#!/bin/sh\nprintf '%s\\n' "$*" >> '${capture}'\nif [ "$*" = '-t get_config' ]; then cat "$XDG_CONFIG_HOME/sway/config"; fi\nexit 0\n`
  );
  await chmod(compositor, 0o700);
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=background\nopen_application=Ctrl+Shift+Y\n'
        );
      },
      async ({ app, configHome }) => {
        const env = {
          ...process.env,
          ...(await app.environment()),
          XDG_RUNTIME_DIR: join(configHome, '..', 'runtime'),
          DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/elder-terms-bus',
        };
        await waitForResult(async () =>
          expect(
            (
              await stat(join(env.XDG_RUNTIME_DIR, 'elder-terms/control.sock'))
            ).isSocket()
          ).toBe(true)
        );
        await waitForResult(
          async () => {
            expect(
              await readFile(join(configHome, 'sway/config'), 'utf8')
            ).toContain('bindsym Ctrl+Shift+y');
            expect(await readFile(capture, 'utf8')).toBe(
              '-t get_version\nreload\n-t get_config\n'
            );
          },
          { timeoutMs: 45_000 }
        );
        expect(
          await app.findById('hotkey_registration_error_dialog')
        ).toBeUndefined();
        const first = await execute(ctl, ['setup'], { env });
        const second = await execute(ctl, ['setup'], { env });
        expect(first.stdout).toContain('Sway hotkeys configured and reloaded');
        expect(second.stdout).toContain('Sway hotkeys configured and reloaded');
        const configuration = await readFile(
          join(configHome, 'sway/config'),
          'utf8'
        );
        expect(configuration.match(/bindsym Ctrl\+Shift\+y/g)).toHaveLength(1);
        expect(await readFile(capture, 'utf8')).toBe(
          '-t get_version\nreload\n-t get_config\n-t get_version\nreload\n-t get_config\n-t get_version\nreload\n-t get_config\n'
        );

        const profile = join(configHome, 'elder-terms/connections/Alpha.ini');
        await writeFile(profile, '[general]\nopen_connection=ctrl+alt+t\n');
        await waitForResult(async () => {
          const updated = await readFile(
            join(configHome, 'sway/config'),
            'utf8'
          );
          expect(updated).toContain('bindsym Ctrl+Mod1+t');
          expect(updated).toContain("'open-connection' 'Alpha'");
          expect(
            (await readFile(capture, 'utf8')).match(/reload\n/g)
          ).toHaveLength(4);
        });
        await writeFile(profile, '[general]\nopen_connection=ctrl+alt+y\n');
        await waitForResult(async () => {
          const updated = await readFile(
            join(configHome, 'sway/config'),
            'utf8'
          );
          expect(updated).toContain('bindsym Ctrl+Mod1+y');
          expect(updated).not.toContain('bindsym Ctrl+Mod1+t');
          expect(
            (await readFile(capture, 'utf8')).match(/reload\n/g)
          ).toHaveLength(5);
        });
      },
      {
        appPath: wrapper,
        args: [],
        env: {
          SWAYSOCK: join(directory, 'session.sock'),
          PATH: directory + ':' + (process.env.PATH ?? '/usr/bin:/bin'),
        },
        xvfbTrayHost: false,
      }
    );
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}, 90_000);

it('automatically configures labwc while retaining existing user shortcuts', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-control-labwc-'));
  const launcherBinary = fileURLToPath(
    new URL('../../.build/elder-terms/elder-terms', import.meta.url)
  );
  const wrapper = join(directory, 'launcher');
  const compositor = join(directory, 'labwc');
  const capture = join(directory, 'labwc-calls');
  await writeFile(
    wrapper,
    `#!/bin/sh\nunset GDK_BACKEND\nexport XDG_SESSION_TYPE=wayland\nexec '${launcherBinary}' "$@"\n`
  );
  await chmod(wrapper, 0o700);
  await writeFile(
    compositor,
    `#!/bin/sh\nprintf '%s\\n' "$*" >> '${capture}'\nexit 0\n`
  );
  await chmod(compositor, 0o700);
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=background\nopen_application=Ctrl+Shift+Y\n'
        );
        const labwc = join(connections, '..', '..', 'labwc');
        await mkdir(labwc);
        await writeFile(
          join(labwc, 'rc.xml'),
          '<labwc_config><keyboard><keybind key="W-Return"><action name="Execute" command="terminal"/></keybind></keyboard></labwc_config>'
        );
      },
      async ({ app, configHome }) => {
        const env = {
          ...process.env,
          ...(await app.environment()),
          XDG_RUNTIME_DIR: join(configHome, '..', 'runtime'),
          DBUS_SESSION_BUS_ADDRESS: 'unix:path=/nonexistent/elder-terms-bus',
        };
        await waitForResult(async () =>
          expect(
            (
              await stat(join(env.XDG_RUNTIME_DIR, 'elder-terms/control.sock'))
            ).isSocket()
          ).toBe(true)
        );
        await waitForResult(
          async () => {
            expect(
              await readFile(join(configHome, 'labwc/rc.xml'), 'utf8')
            ).toContain('key="C-S-y"');
            expect(await readFile(capture, 'utf8')).toBe('--reconfigure\n');
          },
          { timeoutMs: 45_000 }
        );
        expect(
          await app.findById('hotkey_registration_error_dialog')
        ).toBeUndefined();
        expect((await execute(ctl, ['setup'], { env })).stdout).toContain(
          'labwc hotkeys configured and reloaded'
        );
        expect((await execute(ctl, ['setup'], { env })).stdout).toContain(
          'labwc hotkeys configured and reloaded'
        );
        const configuration = await readFile(
          join(configHome, 'labwc/rc.xml'),
          'utf8'
        );
        expect(configuration.match(/key="W-Return"/g)).toHaveLength(1);
        expect(configuration.match(/key="C-S-y"/g)).toHaveLength(1);
        expect(await readFile(capture, 'utf8')).toBe(
          '--reconfigure\n--reconfigure\n--reconfigure\n'
        );

        await execute(ctl, ['open-application'], { env });
        await waitForResult(async () => {
          expect(await app.getWindowCount()).toBe(1);
        });
        await expectElementKind(
          await app.getById('application_menu_button'),
          'toggleButton'
        ).click();
        await expectElementKind(
          await app.getById('application_settings_menu_item'),
          'menuItem'
        ).click();
        const shortcut = expectElementKind(
          await app.getById('application_settings_open_application_entry'),
          'entry'
        );
        await clickShortcutEntry(app, shortcut, false);
        await app.input.setModifier('control', true);
        await app.input.setModifier('alt', true);
        await app.input.pressKey('t');
        await app.input.setModifier('alt', false);
        await app.input.setModifier('control', false);
        await waitForResult(async () => {
          expect(await shortcut.text()).toBe('ctrl+alt+t');
        });
        await expectElementKind(
          await app.getById('application_dialog_save_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await app.getWindowCount()).toBe(1);
          const updated = await readFile(
            join(configHome, 'labwc/rc.xml'),
            'utf8'
          );
          expect(updated).toContain('key="C-A-t"');
          expect(updated).not.toContain('key="C-S-y"');
          expect(await readFile(capture, 'utf8')).toBe(
            '--reconfigure\n--reconfigure\n--reconfigure\n--reconfigure\n'
          );
        });

        await expectElementKind(
          await app.getById('application_menu_button'),
          'toggleButton'
        ).click();
        await expectElementKind(
          await app.getById('application_settings_menu_item'),
          'menuItem'
        ).click();
        const disabledShortcut = expectElementKind(
          await app.getById('application_settings_open_application_entry'),
          'entry'
        );
        await clickShortcutEntry(app, disabledShortcut, false);
        await clickShortcutEntry(app, disabledShortcut, true);
        await expectElementKind(
          await app.getById('application_dialog_save_button'),
          'button'
        ).click();
        await waitForResult(async () => {
          expect(await app.getWindowCount()).toBe(1);
          const updated = await readFile(
            join(configHome, 'labwc/rc.xml'),
            'utf8'
          );
          expect(updated).toContain('key="W-Return"');
          expect(updated).not.toContain('elder-terms setup');
          expect(updated).not.toContain('key="C-A-t"');
          expect(await readFile(capture, 'utf8')).toBe(
            '--reconfigure\n--reconfigure\n--reconfigure\n--reconfigure\n--reconfigure\n'
          );
        });
      },
      {
        appPath: wrapper,
        args: [],
        env: {
          LABWC_PID: '12345',
          PATH: directory + ':' + (process.env.PATH ?? '/usr/bin:/bin'),
        },
        xvfbTrayHost: false,
      }
    );
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}, 90_000);

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
