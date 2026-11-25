// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

import { spawn } from 'node:child_process';
import { appendFile, chmod, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startSshLatencyFixture } from '../../shared/tests/ssh-latency-fixture.mts';

const binary = process.argv[2] ?? fileURLToPath(
  new URL(
    '../../.build/elder-terms-terminal/elder-terms-terminal',
    import.meta.url
  )
);
const script = fileURLToPath(
  new URL('./terminal-docking-ssh.sh', import.meta.url)
);
const directory = await mkdtemp(join(tmpdir(), 'elder-terms-dock-ssh-'));
const peer = join(directory, 'peer.sh');
const sharedFile = join(directory, 'remote', 'shared.txt');
const quote = (value: string): string => `'${value.split("'").join("'\\''")}'`;
let fixture: Awaited<ReturnType<typeof startSshLatencyFixture>> | undefined;
let failed = false;
try {
  await writeFile(
    peer,
    `#!/bin/sh
case "$SSH_ORIGINAL_COMMAND" in
  /usr/lib/openssh/sftp-server*) exec /usr/lib/openssh/sftp-server ;;
esac
stty -echo
echo $$ > ${quote(join(directory, 'shell.pid'))}
echo SSH_DOCK_READY
while IFS= read -r line; do
  printf '%s:%s\\n' "$$" "$line" >> ${quote(join(directory, 'shell.input'))}
  printf '%s:%s\\r\\n' "$$" "$line"
  [ "$line" = quit ] && exit 0
done
`
  );
  await chmod(peer, 0o700);
  await mkdir(join(directory, 'remote'));
  await writeFile(sharedFile, 'SFTP_DOCK_SHARED\n');
  fixture = await startSshLatencyFixture(directory, [peer]);
  await appendFile(
    join(directory, 'ssh-fixture', 'sshd_config'),
    'Subsystem sftp /usr/lib/openssh/sftp-server\n'
  );
  const child = spawn(
    'bash',
    [
      script,
      binary,
      String(fixture.port),
      fixture.username,
      fixture.clientKey,
      fixture.knownHosts,
      directory,
    ],
    { stdio: 'inherit' }
  );
  const status = await new Promise<number>((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', (code, signal) => resolve(code ?? (signal ? 1 : 0)));
  });
  if (status !== 0) {
    failed = true;
    process.exitCode = status;
  }
} finally {
  if (fixture) await fixture.stop();
  if (failed || process.env.ELDER_DOCK_KEEP_ARTIFACTS)
    console.error(`Docking SSH artifacts: ${directory}`);
  else await rm(directory, { recursive: true, force: true });
}
