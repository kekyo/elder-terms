// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
import { createServer as createHttpServer } from 'node:http';
import { createServer as createHttpsServer } from 'node:https';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher, type GtkTableElement } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers.ts';

for (const scheme of ['http', 'https'])
  it(`browses ${scheme} lazily and receives HTML or a cyclic subtree through jobs`, async (context) => {
    const directory = await mkdtemp(join(tmpdir(), 'elder-http-ui-'));
    const local = join(directory, 'local');
    await mkdir(local);
    const certificate = join(directory, 'ca.pem');
    const key = join(directory, 'key.pem');
    if (scheme === 'https') {
      const generating = spawn(
        'openssl',
        [
          'req',
          '-x509',
          '-newkey',
          'rsa:2048',
          '-noenc',
          '-keyout',
          key,
          '-out',
          certificate,
          '-days',
          '1',
          '-subj',
          '/CN=localhost',
          '-addext',
          'subjectAltName=IP:127.0.0.1',
        ],
        { stdio: 'ignore' }
      );
      expect((await once(generating, 'close'))[0]).toBe(0);
    }
    const requests: string[] = [];
    const source =
      '<a href="tree/">tree</a><a href="document.html">document</a><a href="untouched/">untouched</a>';
    const server =
      scheme === 'https'
        ? createHttpsServer({
            key: await readFile(key),
            cert: await readFile(certificate),
          })
        : createHttpServer();
    server.on('request', (request, response) => {
      requests.push(`${request.method} ${request.url}`);
      response.setHeader('Content-Type', 'text/html');
      if (request.url === '/pub/') response.end(source);
      else if (request.url === '/pub/document.html')
        response.end('<p>Original HTML</p>');
      else if (request.url === '/pub/tree/')
        response.end(
          '<a href="file.bin">file</a><a href="../">parent</a><a href="#self">self</a>'
        );
      else if (request.url === '/pub/tree/file.bin') {
        response.setHeader('Content-Type', 'application/octet-stream');
        response.end('download bytes');
      } else {
        response.statusCode = 404;
        response.end('missing');
      }
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const address = server.address();
    if (!address || typeof address === 'string')
      throw new Error('Expected TCP server');
    const base = `${scheme}://127.0.0.1:${address.port}/pub/`;
    const evidence = createTestEvidence(context);
    const launcher = createGtkAppLauncher({
      appPath: fileURLToPath(
        new URL(
          '../../.build/elder-terms-terminal/elder-terms-file-transfer',
          import.meta.url
        )
      ),
      env: {
        LANGUAGE: 'en',
        LC_ALL: 'C.UTF-8',
        XDG_CONFIG_HOME: join(directory, 'config'),
        XDG_DATA_HOME: join(directory, 'data'),
      },
      onSystemOutput: evidence.recordSystemOutputEvent,
      xvfbTrayHost: true,
    });
    try {
      const config = join(directory, 'http.ini');
      await writeFile(
        config,
        `[general]\ntype=http\nname=HTTP files\nauto_close=false\n[http]\nbase_url=${base}\nlocal_directory=${local}\nca_file=${scheme === 'https' ? certificate : ''}\n`
      );
      const app = await launcher.launch(['-c', config], {
        onOutput: evidence.recordAppOutputEvent,
      });
      const tree = expectElementKind(
        await app.getById('file_transfer_remote_tree'),
        'table'
      ) as GtkTableElement;
      const ready = async () =>
        await waitForResult(async () => {
          expect((await tree.info()).states).toContain('sensitive');
        });
      await ready();
      await waitForResult(async () => {
        expect(await tree.getRowCount()).toBe(3);
      });
      expect(requests).toEqual(['GET /pub/']);
      const rowFor = async (name: string) =>
        await waitForResult(async () => {
          for (let index = 0; index < (await tree.getRowCount()); ++index)
            if ((await (await tree.cellAt(index, 0))?.info())?.name === name)
              return index;
          throw new Error(`Missing HTTP row ${name}`);
        });
      const clickRow = async (name: string, button: 'left' | 'right') => {
        for (const row of await tree.selectedRows())
          await tree.deselectRow(row);
        const row = await rowFor(name);
        await tree.selectRow(row);
        const bounds = (await (await tree.cellAt(row, 0))!.capture()).bounds;
        await app.input.moveMouseTo(
          Math.round(bounds.x + bounds.width / 2),
          Math.round(bounds.y + bounds.height / 2)
        );
        await app.input.setMouseButton(button, true);
        await app.input.setMouseButton(button, false);
      };
      await clickRow('tree', 'left');
      await evidence.captureEvidence('http-unresolved-links', async () =>
        app.capture()
      );
      expect(requests).toEqual(['GET /pub/']);
      const toggleTree = async () => {
        const row = await rowFor('tree');
        const cell = (await (await tree.cellAt(row, 0))!.capture()).bounds;
        const table = (await tree.capture()).bounds;
        await app.input.moveMouseTo(
          table.x + 10,
          Math.round(cell.y + cell.height / 2)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
      };
      await toggleTree();
      await ready();
      await evidence.log('Requests after expansion', requests);
      await evidence.captureEvidence('http-after-expansion', async () =>
        app.capture()
      );
      await rowFor('file.bin');
      expect(requests).toEqual(['GET /pub/', 'GET /pub/tree/']);
      await toggleTree();
      await toggleTree();
      await ready();
      await rowFor('file.bin');
      expect(requests).toEqual(['GET /pub/', 'GET /pub/tree/']);
      await clickRow('document.html', 'right');
      for (const id of ['new_directory', 'rename', 'delete'])
        expect(
          (await (await app.getById(`file_transfer_remote_${id}_item`)).info())
            .states
        ).not.toContain('sensitive');
      await expectElementKind(
        await app.getById('file_transfer_receive_file_item'),
        'menuItem'
      ).click();
      await waitForResult(async () => {
        expect(await readFile(join(local, 'document.html'), 'utf8')).toBe(
          '<p>Original HTML</p>'
        );
      });
      await ready();
      await clickRow('tree', 'right');
      await expectElementKind(
        await app.getById('file_transfer_receive_item'),
        'menuItem'
      ).click();
      await waitForResult(async () => {
        expect(await readFile(join(local, 'tree', 'file.bin'), 'utf8')).toBe(
          'download bytes'
        );
      });
      await waitForResult(async () => {
        expect(
          await expectElementKind(
            await app.getById('file_transfer_jobs_summary'),
            'label'
          ).text()
        ).toContain('2 finished');
      });
      expect(
        requests.filter((request) => request === 'GET /pub/tree/')
      ).toHaveLength(1);
      expect(
        requests.every(
          (request) =>
            request.startsWith('GET ') && !request.includes('untouched')
        )
      ).toBe(true);
      await evidence.captureEvidence('http-completed-jobs', async () =>
        app.capture()
      );
      expect(
        (await (await app.getById('file_transfer_job_2_row')).info()).name
      ).toContain('tree');
      await expectElementKind(
        await app.getById('file_transfer_remote_refresh_button'),
        'button'
      ).click();
      await ready();
      await waitForResult(async () => {
        expect(
          requests.filter((request) => request === 'GET /pub/')
        ).toHaveLength(2);
      });
      const path = expectElementKind(
        await app.getById('file_transfer_remote_path_entry'),
        'entry'
      );
      const bounds = (await path.capture()).bounds;
      await app.input.moveMouseTo(bounds.x + 20, bounds.y + bounds.height / 2);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await path.setText(base + 'tree/');
      await app.input.pressKey('Return');
      await ready();
      await rowFor('file.bin');
      await expectElementKind(
        await app.getById('file_transfer_remote_up_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await path.text()).toBe(base);
        expect(await tree.getRowCount()).toBe(3);
      });
      expect(
        requests.filter((request) => request === 'GET /pub/tree/')
      ).toHaveLength(1);
      await evidence.flushOutputs([app], launcher);
    } finally {
      await launcher.release();
      await evidence.release();
      server.closeAllConnections();
      server.close();
      await once(server, 'close');
      await rm(directory, { recursive: true, force: true });
    }
  });
