#!/usr/bin/env node
//
// Dev server for the authoring tool. Serves the DigidreamSynth (midisynth/) directory so the
// tool can fetch content/ and test/golden/ alongside its own files, with
// correct MIME types for ES modules. No COOP/COEP needed -- the worklet
// imports the wasm as a plain ES6 module.
//
//   node tool/serve.js [port] [--host 0.0.0.0] [--root /path/to/dist-web]
//   then open http://localhost:8930/tool/
//
// The same server runs a packaged dist-web bundle on a VPS (--root), but for
// anything beyond localhost put it behind HTTPS: browsers only allow
// AudioWorklet (and Web MIDI) in a secure context.
//
const http = require('http');
const fs = require('fs');
const path = require('path');

let root = path.join(__dirname, '..');
let host = '127.0.0.1';
let port = 8930;
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--host') host = args[++i];
  else if (args[i] === '--root') root = path.resolve(args[++i]);
  else if (/^\d+$/.test(args[i])) port = Number(args[i]);
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.json': 'application/json',
  '.mid': 'audio/midi',
  '.wasm': 'application/wasm',
  '.css': 'text/css',
};

http.createServer((req, res) => {
  let urlPath = decodeURIComponent(req.url.split('?')[0]);
  if (urlPath.endsWith('/')) urlPath += 'index.html';
  const file = path.normalize(path.join(root, urlPath));
  if (!file.startsWith(root)) {
    res.writeHead(403).end();
    return;
  }
  fs.readFile(file, (err, data) => {
    if (err) {
      res.writeHead(404).end('not found: ' + urlPath);
      return;
    }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file)] || 'application/octet-stream',
      'Cache-Control': 'no-store',
    });
    res.end(data);
  });
}).listen(port, host, () => {
  console.log(`DigidreamSynth: http://${host}:${port}/tool/ (root ${root})`);
});
