#!/usr/bin/env node
'use strict';
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

const root = path.join(__dirname, '..');
const dir = `${process.platform}-${process.arch}`;
const binary = path.join(root, 'prebuilds', dir, 'assembly-repl');

try {
    fs.accessSync(binary, fs.constants.X_OK);
} catch {
    console.error(`assembly-repl: no prebuilt binary for ${dir}.`);
    console.error('Supported: darwin-arm64, linux-arm64, linux-x64.');
    process.exit(1);
}

const child = spawn(binary, process.argv.slice(2), { stdio: 'inherit' });
child.on('exit', (code, signal) => {
    if (signal) process.kill(process.pid, signal);
    else process.exit(code ?? 0);
});
