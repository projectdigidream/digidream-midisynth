#!/usr/bin/env node
//
// `npm start` helper. A terminal inside VS Code (or any other Electron-based
// editor) exports ELECTRON_RUN_AS_NODE=1, which makes the Electron binary
// boot as plain Node -- main.js then fails on its first electron API call.
// Launch with that variable cleared, whatever shell started us.
//
const { spawn } = require('node:child_process');
const electron = require('electron');

const env = { ...process.env };
delete env.ELECTRON_RUN_AS_NODE;

const child = spawn(electron, ['.', ...process.argv.slice(2)], {
  cwd: __dirname, env, stdio: 'inherit',
});
child.on('close', (code) => process.exit(code ?? 0));
