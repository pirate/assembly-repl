#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.join(__dirname, '..');
const hostTarget = `${process.platform}-${process.arch}`;
const releaseCflags = '-std=c11 -Wall -Wextra -Wpedantic -O2';
const dockerTargets = [
    { platform: 'linux/amd64', prebuild: 'linux-x64' },
    { platform: 'linux/arm64', prebuild: 'linux-arm64' },
];
const runners = [
    { built: 'asmrepl', vendored: 'assembly-repl' },
    { built: 'language-repl', vendored: 'language-repl' },
];
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
const requiredFiles = ['bin/', 'src/', 'Makefile', '.dockerignore', 'Dockerfile', 'prebuilds/', 'README.md'];
const requiredBuildFiles = ['bin/build.js', 'Dockerfile', '.dockerignore', 'Makefile', 'src/asmrepl.c', 'src/language-repl.c'];

if (hostTarget !== 'darwin-arm64') {
    console.error(`build: unsupported release host ${hostTarget}`);
    console.error('build: run pnpm build on macOS arm64 so darwin-arm64 can be rebuilt locally.');
    console.error('build: linux-x64 and linux-arm64 are rebuilt from that host with docker buildx.');
    process.exit(1);
}

function run(command, args) {
    const result = spawnSync(command, args, {
        cwd: root,
        stdio: 'inherit',
        env: process.env,
    });

    if (result.error) {
        console.error(`build: failed to run ${command}: ${result.error.message}`);
        process.exit(1);
    }
    if (result.status !== 0) {
        process.exit(result.status ?? 1);
    }
}

function vendorRunner(source, target, runner) {
    const targetDir = path.join(root, 'prebuilds', target);
    const destination = path.join(targetDir, runner.vendored);

    const stat = fs.statSync(source);
    if (!stat.isFile() || stat.size === 0) {
        console.error(`build: ${runner.built} was not rebuilt correctly`);
        process.exit(1);
    }

    fs.mkdirSync(targetDir, { recursive: true });
    fs.copyFileSync(source, destination);
    fs.chmodSync(destination, 0o755);
    console.log(`vendored ${path.relative(root, source)} -> prebuilds/${target}/${runner.vendored}`);
}

function vendorHostRunners() {
    run('make', ['clean']);
    run('make', ['CC=clang', `CFLAGS=${releaseCflags}`, 'all']);

    for (const runner of runners) {
        vendorRunner(path.join(root, runner.built), 'darwin-arm64', runner);
    }
}

function vendorLinuxRunners(target) {
    const outputDir = path.join(root, '.repl-build', `docker-${target.prebuild}`);
    fs.rmSync(outputDir, { recursive: true, force: true });
    fs.mkdirSync(outputDir, { recursive: true });

    run('docker', [
        'buildx',
        'build',
        '--platform',
        target.platform,
        '--output',
        `type=local,dest=${outputDir}`,
        '-f',
        'Dockerfile',
        '.',
    ]);

    for (const runner of runners) {
        vendorRunner(path.join(outputDir, runner.vendored), target.prebuild, runner);
    }
}

function validatePackage() {
    const pkg = require(path.join(root, 'package.json'));
    let failed = false;

    function fail(message) {
        failed = true;
        console.error(`build: package check: ${message}`);
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

    if (pkg.scripts?.build !== 'node ./bin/build.js') {
        fail('build script must run node ./bin/build.js');
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
}

run('docker', ['buildx', 'version']);

vendorHostRunners();
for (const target of dockerTargets) {
    vendorLinuxRunners(target);
}

validatePackage();
run('pnpm', ['pack', '--pack-destination', '/tmp']);
