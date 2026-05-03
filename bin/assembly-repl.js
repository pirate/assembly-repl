#!/usr/bin/env node
'use strict';
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

const platform = process.platform;
const arch = process.arch;
const dir = `${platform}-${arch}`;
const binary = path.join(__dirname, '..', 'prebuilds', dir, 'assembly-repl');

if (!fs.existsSync(binary)) {
    console.error(`assembly-repl: no prebuilt binary for ${dir}.`);
    console.error('Supported: darwin-arm64, linux-x64. Build from source with `make`.');
    process.exit(1);
}

const child = spawn(binary, process.argv.slice(2), { stdio: 'inherit' });
child.on('exit', (code, signal) => {
    if (signal) process.kill(process.pid, signal);
    else process.exit(code ?? 0);
});
