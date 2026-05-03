#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const pkg = require(path.join(root, 'package.json'));

const expectedBins = {
    'assembly-repl': './bin/repl.js',
    'c-repl': './bin/c-repl.js',
    'cpp-repl': './bin/cpp-repl.js',
    'objc-repl': './bin/objc-repl.js',
    'llvmir-repl': './bin/llvmir-repl.js',
};

const supportedTargets = ['darwin-arm64', 'linux-arm64', 'linux-x64'];
const nativeRunners = ['assembly-repl', 'language-repl'];
const forbiddenScripts = ['install', 'postinstall', 'preinstall', 'node-gyp', 'prepare'];
const requiredFiles = ['bin/', 'scripts/', 'src/', 'Makefile', '.dockerignore', 'prebuilds/', 'README.md'];
const requiredBuildFiles = ['scripts/build.js', 'scripts/Dockerfile.prebuild', '.dockerignore', 'Makefile', 'src/asmrepl.c', 'src/language-repl.c'];

let failed = false;

function fail(message) {
    failed = true;
    console.error(`package check: ${message}`);
}

for (const [name, relPath] of Object.entries(expectedBins)) {
    if (pkg.bin?.[name] !== relPath) {
        fail(`bin ${name} must point to ${relPath}`);
    }

    const absPath = path.join(root, relPath);
    if (!fs.existsSync(absPath)) {
        fail(`bin file is missing: ${relPath}`);
    }
}

for (const scriptName of forbiddenScripts) {
    if (pkg.scripts?.[scriptName]) {
        fail(`package must not define ${scriptName} script`);
    }
}

if (pkg.scripts?.build !== 'node ./scripts/build.js') {
    fail('build script must run node ./scripts/build.js');
}

for (const relPath of requiredFiles) {
    if (!pkg.files?.includes(relPath)) {
        fail(`package files must include ${relPath}`);
    }
}

for (const relPath of requiredBuildFiles) {
    if (!fs.existsSync(path.join(root, relPath))) {
        fail(`build input is missing: ${relPath}`);
    }
}

for (const target of supportedTargets) {
    for (const runner of nativeRunners) {
        const relPath = path.join('prebuilds', target, runner);
        const absPath = path.join(root, relPath);
        if (!fs.existsSync(absPath)) {
            fail(`missing prebuilt runner: ${relPath}`);
            continue;
        }

        try {
            fs.accessSync(absPath, fs.constants.X_OK);
        } catch {
            fail(`prebuilt runner is not executable: ${relPath}`);
        }
    }
}

if (failed) {
    process.exit(1);
}
