#!/usr/bin/env node
//
// Dev server for the authoring tool. Serves the midisynth directory so the
// tool can fetch content/ and test/golden/ alongside its own files, with
// correct MIME types for ES modules. No COOP/COEP needed -- the worklet
// imports the wasm as a plain ES6 module.
//
//   node tool/serve.js [port]     then open http://localhost:8930/tool/
//
const http = require('http');
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const port = Number(process.argv[2] || 8930);

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
}).listen(port, () => {
  console.log(`midisynth tool: http://localhost:${port}/tool/`);
});
