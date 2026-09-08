import assert from 'node:assert/strict';
import { appendFile, readFile, writeFile } from 'node:fs/promises';
import { createGtkAppLauncher, type GtkApp } from 'gestament';
import { waitForResult } from 'gestament/testing';

const [address, expectedName] = process.argv.slice(2);
assert(address && expectedName);
const numericAddress = address
  .split('.')
  .reduce((result, octet) => result * 256 + Number(octet), 0);
const configPath = '/tmp/scanned-connection.ini';
const environment = {
  LANGUAGE: 'C',
  LC_ALL: 'C.UTF-8',
  XDG_CONFIG_HOME: '/tmp/scan-config',
};
const launcher = createGtkAppLauncher({
  appPath: '/workspace/.build/shared/elder-terms-settings-widget-fixture',
  env: environment,
});
let settingsApp: GtkApp | undefined;
try {
  settingsApp = await launcher.launch([
    '--page=telnet',
    '--type=telnet',
    '--telnet-port=23',
    `--ip-scan=network:${numericAddress}`,
    `--save-file=${configPath}`,
  ]);
  const button = await settingsApp.getById('settings_telnet_ip_scan_button');
  assert.equal(button.kind, 'button');
  await button.click();
  const table = await settingsApp.getById('settings_ip_scan_results');
  assert.equal(table.kind, 'table');
  await waitForResult(async () => {
    assert.equal(await table.getRowCount(), 1);
    assert.equal((await (await table.cellAt(0, 0))?.info())?.name, address);
    assert.equal(
      (await (await table.cellAt(0, 1))?.info())?.name,
      expectedName === address ? '' : expectedName
    );
    const progress = await settingsApp!.getById('settings_ip_scan_progress');
    assert.equal(progress.kind, 'progressBar');
    const value = await progress.valueInfo();
    assert.equal(value.value, value.maximum);
  });
  const bounds = (await (await table.cellAt(0, 0))?.capture())?.bounds;
  assert(bounds);
  await settingsApp.input.moveMouseTo(
    Math.round(bounds.x + bounds.width / 2),
    Math.round(bounds.y + bounds.height / 2)
  );
  for (let click = 0; click < 2; ++click) {
    await settingsApp.input.setMouseButton('left', true);
    await settingsApp.input.setMouseButton('left', false);
  }
  await waitForResult(async () => {
    assert.equal(
      await settingsApp!.findById('settings_ip_scan_dialog'),
      undefined
    );
  });
  const entry = await settingsApp.getById('settings_telnet_address_entry');
  assert.equal(entry.kind, 'entry');
  assert.equal(await entry.text(), expectedName);
  const save = await settingsApp.getById('settings_save_button');
  assert.equal(save.kind, 'button');
  await save.click();
  await waitForResult(async () => {
    const saved = await readFile(configPath, 'utf8');
    assert(saved.includes(`address=${expectedName}\n`));
    assert(saved.includes('port=23\n'));
    assert(saved.includes('type=telnet\n'));
  });
  await writeFile('/evidence/settings.png', (await entry.capture()).image);
} finally {
  if (settingsApp !== undefined) {
    await writeFile(
      '/evidence/settings-output.json',
      JSON.stringify(await settingsApp.output(), null, 2)
    );
  }
  await launcher.release();
}

// Launch the real terminal from the saved file. Its ordinary GResolver forward
// lookup must reach the container's resolved stub, without fixture injection.
// Add only a greeting-response macro to observe real bidirectional terminal I/O;
// the scanned and saved connection address and port remain untouched.
await appendFile(
  configPath,
  '\n[macro.verify]\n' +
    'regex=^MULTICAST CONNECTION VERIFIED$\n' +
    'send=CONFIRMED SCANNED NAME\\r\\n\n'
);
const terminalLauncher = createGtkAppLauncher({
  appPath: '/workspace/.build/elder-terms-vte/elder-terms-vte',
  env: environment,
});
let terminalApp: GtkApp | undefined;
try {
  terminalApp = await terminalLauncher.launch(['-c', configPath]);
  const terminal = await terminalApp.getById('terminal_view');
  await waitForResult(async () => {
    assert.equal(
      await readFile('/evidence/peer-response.txt', 'utf8'),
      'CONFIRMED SCANNED NAME\n'
    );
  });
  await writeFile('/evidence/terminal.png', (await terminal.capture()).image);
  process.stdout.write(`Verified ${address} -> ${expectedName} -> TELNET\n`);
} finally {
  if (terminalApp !== undefined) {
    await writeFile(
      '/evidence/terminal-output.json',
      JSON.stringify(await terminalApp.output(), null, 2)
    );
  }
  await terminalLauncher.release();
}
