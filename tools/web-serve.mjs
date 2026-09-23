#!/usr/bin/env node
/*
 * web-serve.mjs — serve the web build locally.
 *
 *   node tools/web-serve.mjs [--site build_web/site] [--port 8080] [--rom path/to/merged.zip]
 *
 * A static server for build_web/site with the MIME types a browser insists on
 * (.wasm must be application/wasm to be compiled while it streams).
 *
 * --rom exposes ONE local zip at /dev-rom.zip, so the page's development
 * parameter can boot the game with no file dialog:
 *
 *   http://localhost:8080/?rom=/dev-rom.zip
 *
 * That is what lets a headless browser test the build. The zip is served from
 * where it already is and is never copied into the site directory, so it cannot
 * end up in a deployed site by accident.
 *
 * Binds to localhost only.
 */
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
};
const site = path.resolve(opt('--site', 'build_web/site'));
const port = Number(opt('--port', '8080'));
const rom = opt('--rom', null);

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.wasm': 'application/wasm',
  '.woff2': 'font/woff2',
  '.json': 'application/json',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.zip': 'application/zip',
};

function send(res, file) {
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404).end('not found'); return; }
    res.writeHead(200, {
      'Content-Type': TYPES[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Content-Length': st.size,
      'Cache-Control': 'no-store',
    });
    fs.createReadStream(file).pipe(res);
  });
}

http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  if (rom && url.pathname === '/dev-rom.zip') { send(res, path.resolve(rom)); return; }
  let rel = decodeURIComponent(url.pathname);
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.join(site, rel);
  if (!file.startsWith(site)) { res.writeHead(403).end('forbidden'); return; }   /* no ../ out of the site */
  send(res, file);
}).listen(port, '127.0.0.1', () => {
  console.log(`serving ${site}`);
  console.log(`  http://localhost:${port}/` + (rom ? `?rom=/dev-rom.zip   (${rom})` : ''));
});
