#!/usr/bin/env node
'use strict';
const path = require('path');
const fs = require('fs');
const { spawn, spawnSync } = require('child_process');
const readline = require('readline');
const colors = require('ansi-colors');

const root = path.join(__dirname, '..');
const dir = `${process.platform}-${process.arch}`;
const commandName = path.basename(process.argv[1] || 'assembly-repl').replace(/\.js$/, '');
const modeByCommand = {
    'assembly-repl': 'asm',
    'c-repl': 'c',
    'cpp-repl': 'cpp',
    'objc-repl': 'objc',
    'llvmir-repl': 'llvmir',
    'wasm-repl': 'wasm',
    'rust-repl': 'rust',
    'zig-repl': 'zig',
    'go-repl': 'go',
};
const mode = modeByCommand[commandName] || 'asm';
const promptBaseByMode = {
    asm: 'asm',
    c: 'c',
    cpp: 'cpp',
    objc: 'objc',
    llvmir: 'ir',
    wasm: 'wasm',
    rust: 'rust',
    zig: 'zig',
    go: 'go',
};
const binaryBase = mode === 'asm' ? 'assembly-repl' : 'language-repl';
const localBinary = path.join(root, mode === 'asm' ? 'asmrepl' : 'language-repl');
const prebuiltBinary = path.join(root, 'prebuilds', dir, binaryBase);
const binary = mode === 'wasm' ? process.execPath : (fileExists(localBinary) ? localBinary : prebuiltBinary);
const runtimeDependencies = runtimeDependenciesForMode(mode);
const options = parseWrapperArgs(process.argv.slice(2));
const noHighlight = options.noHighlight ||
    process.env.REPL_NO_HIGHLIGHT === '1' ||
    process.env.ASMREPL_NO_HIGHLIGHT === '1';
const childArgs = options.childArgs;
const effectiveChildArgs = mode === 'wasm'
    ? [path.join(root, 'bin', 'wasm-runner.js'), ...childArgs]
    : (mode === 'asm' ? childArgs : ['--mode', mode, ...childArgs]);

if (!ensureExecutable(binary)) {
    console.error(`${commandName}: no prebuilt native runner for ${dir}.`);
    if (mode === 'asm') {
        console.error('Supported prebuild targets: darwin-arm64, linux-arm64, linux-x64.');
    } else {
        console.error('Supported prebuild targets for this command: darwin-arm64, linux-arm64, linux-x64.');
    }
    console.error('Reinstall assembly-repl or use a supported platform.');
    process.exit(1);
}

if (mode === 'objc' && process.platform !== 'darwin') {
    console.error('objc-repl is only supported on macOS arm64. Objective-C snippets need the Apple Objective-C runtime and Foundation framework.');
    process.exit(1);
}

const missingDependencies = runtimeDependencies.filter((dependency) => !commandAvailable(dependency.command));
if (missingDependencies.length > 0) {
    printMissingDependencyError(commandName, missingDependencies);
    process.exit(1);
}

if (options.debuggerRequested) {
    launchDebugger(commandName, options.debuggerCommand, binary, effectiveChildArgs);
} else if (noHighlight || !process.stdin.isTTY || !process.stdout.isTTY) {
    const child = spawn(binary, effectiveChildArgs, { stdio: 'inherit' });
    child.on('exit', (code, signal) => {
        if (signal) process.kill(process.pid, signal);
        else process.exit(code ?? 0);
    });
} else {
    runHighlighted(binary, effectiveChildArgs, mode, promptBaseByMode[mode]);
}

function parseWrapperArgs(args) {
    const childArgs = [];
    let noHighlight = false;
    let debuggerRequested = false;
    let debuggerCommand = '';

    for (let i = 0; i < args.length; i++) {
        const arg = args[i];

        if (arg === '--no-highlight') {
            noHighlight = true;
            continue;
        }

        if (arg === '--debugger' || arg === '--debug') {
            debuggerRequested = true;
            const next = args[i + 1];
            if (next && !next.startsWith('-')) {
                debuggerCommand = next;
                i++;
            }
            continue;
        }

        if (arg.startsWith('--debugger=')) {
            debuggerRequested = true;
            debuggerCommand = arg.slice('--debugger='.length);
            continue;
        }

        if (arg === '--lldb') {
            debuggerRequested = true;
            debuggerCommand = 'lldb';
            continue;
        }

        if (arg === '--gdb') {
            debuggerRequested = true;
            debuggerCommand = 'gdb';
            continue;
        }

        childArgs.push(arg);
    }

    return { childArgs, noHighlight, debuggerRequested, debuggerCommand };
}

function launchDebugger(displayCommand, requestedDebugger, target, targetArgs) {
    const debuggerConfig = resolveDebugger(requestedDebugger, target, targetArgs);
    if (!debuggerConfig) {
        printDebuggerInstallHelp(displayCommand, requestedDebugger);
        process.exit(1);
    }

    let attachedTarget = null;
    let attachedTargetExited = false;
    const cleanupAttachedTarget = () => {
        if (attachedTarget && !attachedTargetExited) {
            try {
                attachedTarget.kill('SIGKILL');
            } catch {
                // The attached process may already be gone by the time the debugger exits.
            }
        }
    };

    if (debuggerConfig.attachTarget) {
        attachedTarget = spawn(debuggerConfig.attachTarget.command, debuggerConfig.attachTarget.args, { stdio: 'inherit' });
        attachedTarget.on('exit', () => {
            attachedTargetExited = true;
        });
        attachedTarget.on('error', (error) => {
            console.error(`${displayCommand}: failed to launch target for debugger attach: ${error.message}`);
            process.exit(1);
        });
        if (!attachedTarget.pid) {
            console.error(`${displayCommand}: failed to launch target for debugger attach.`);
            process.exit(1);
        }
        try {
            if (!attachedTarget.kill('SIGSTOP')) {
                throw new Error('process did not accept SIGSTOP');
            }
        } catch (error) {
            console.error(`${displayCommand}: failed to stop target before debugger attach: ${error.message}`);
            process.exit(1);
        }
        debuggerConfig.args = debuggerConfig.args(attachedTarget.pid);
    }

    if (debuggerConfig.note) {
        console.error(`${displayCommand}: ${debuggerConfig.note}`);
    }
    console.error(`${displayCommand}: launching ${debuggerConfig.label} for ${targetLabel(target, targetArgs)}`);
    const child = spawn(debuggerConfig.command, debuggerConfig.args, { stdio: 'inherit' });
    child.on('exit', (code, signal) => {
        cleanupAttachedTarget();
        if (signal) process.kill(process.pid, signal);
        else process.exit(code ?? 0);
    });
    child.on('error', (error) => {
        cleanupAttachedTarget();
        console.error(`${displayCommand}: failed to launch ${debuggerConfig.command}: ${error.message}`);
        process.exit(1);
    });
}

function resolveDebugger(requestedDebugger, target, targetArgs) {
    const requested = normalizeDebuggerName(requestedDebugger);
    if (!requested) {
        for (const candidate of ['lldb', 'gdb']) {
            const config = resolveDebugger(candidate, target, targetArgs);
            if (config) return config;
        }
        return null;
    }

    const preset = debuggerPreset(requested);
    if (preset) {
        return commandExists(preset.command) ? preset.config(target, targetArgs) : null;
    }

    return commandExists(requested)
        ? {
            command: requested,
            args: debuggerArgs(requested, target, targetArgs),
            label: path.basename(requested),
        }
        : null;
}

function normalizeDebuggerName(value) {
    return (value || '').trim().toLowerCase();
}

function debuggerPreset(name) {
    const presets = {
        lldb: {
            command: 'lldb',
            config: (target, targetArgs) => ({
                command: 'lldb',
                args: ['--', target, ...targetArgs],
                label: 'lldb',
                note: "target is loaded but not started; type 'run' in LLDB to start it",
            }),
        },
        'lldb-gui': {
            command: 'lldb',
            config: (target, targetArgs) => ({
                command: 'lldb',
                args: ['-o', 'process launch --stop-at-entry', '--', target, ...targetArgs],
                label: 'lldb-gui',
                note: "target will stop at entry; type 'gui' at the (lldb) prompt to enter LLDB's curses UI",
            }),
        },
        gdb: {
            command: 'gdb',
            config: (target, targetArgs) => ({
                command: 'gdb',
                args: ['-q', '-ex', 'set pagination off', '--args', target, ...targetArgs],
                label: 'gdb',
                note: compactNotes(["target is loaded but not started; type 'run' in GDB to start it", gdbMacNote()]),
            }),
        },
        'gdb-tui': {
            command: 'gdb',
            config: (target, targetArgs) => ({
                command: 'gdb',
                args: ['-q', '-tui', '-ex', 'set pagination off', '--args', target, ...targetArgs],
                label: 'gdb-tui',
                note: compactNotes(["target is loaded but not started; type 'run' in GDB to start it", gdbMacNote()]),
            }),
        },
        cgdb: {
            command: 'cgdb',
            config: (target, targetArgs) => ({
                command: 'cgdb',
                args: ['--args', target, ...targetArgs],
                label: 'cgdb',
                note: "target is loaded but not started; type 'run' in the GDB command window to start it",
            }),
        },
        pwnbg: {
            command: process.platform === 'darwin' ? 'pwndbg-lldb' : 'pwndbg',
            config: pwndbgConfig,
        },
        pwndbg: {
            command: process.platform === 'darwin' ? 'pwndbg-lldb' : 'pwndbg',
            config: pwndbgConfig,
        },
    };
    return presets[name] || null;
}

function pwndbgConfig(target, targetArgs) {
    if (process.platform === 'darwin') {
        return {
            command: 'pwndbg-lldb',
            args: (pid) => ['-p', String(pid)],
            label: 'pwndbg-lldb',
            note: 'starting the target paused and attaching with pwndbg-lldb',
            attachTarget: {
                command: target,
                args: targetArgs,
            },
        };
    }

    return {
        command: 'pwndbg',
        args: ['-q', '--args', target, ...targetArgs],
        label: 'pwndbg',
        note: "target is loaded but not started; type 'run' in Pwndbg to start it",
    };
}

function compactNotes(notes) {
    return notes.filter(Boolean).join('; ');
}

function gdbMacNote() {
    return process.platform === 'darwin'
        ? 'GDB on macOS may need codesigning before it can run or attach to processes'
        : '';
}

function printDebuggerInstallHelp(displayCommand, requestedDebugger) {
    const requested = normalizeDebuggerName(requestedDebugger);
    const label = requested || 'lldb or gdb';
    console.error(`${displayCommand}: could not find debugger '${label}'.`);
    console.error('');

    if (!requested) {
        console.error('Install LLDB or GDB, then run this command again.');
        console.error('macOS:         xcode-select --install');
        console.error('Debian/Ubuntu: sudo apt install lldb gdb');
        console.error('Fedora:        sudo dnf install lldb gdb');
        console.error('Arch:          sudo pacman -S lldb gdb');
        return;
    }

    if (requested === 'lldb' || requested === 'lldb-gui') {
        console.error(`${requested} needs the 'lldb' command on PATH.`);
        console.error('macOS:         xcode-select --install');
        console.error('Homebrew:      brew install llvm');
        console.error('Debian/Ubuntu: sudo apt install lldb');
        console.error('Fedora:        sudo dnf install lldb');
        console.error('Arch:          sudo pacman -S lldb');
        if (requested === 'lldb-gui') {
            console.error('');
            console.error("If 'lldb' exists but 'gui' fails, install an LLDB build with curses GUI support.");
        }
        return;
    }

    if (requested === 'gdb' || requested === 'gdb-tui') {
        console.error(`${requested} needs the 'gdb' command on PATH.`);
        console.error('Homebrew:      brew install gdb');
        console.error('Debian/Ubuntu: sudo apt install gdb');
        console.error('Fedora:        sudo dnf install gdb');
        console.error('Arch:          sudo pacman -S gdb');
        return;
    }

    if (requested === 'cgdb') {
        console.error("cgdb needs the 'cgdb' command on PATH.");
        console.error('Homebrew:      brew install cgdb');
        console.error('Debian/Ubuntu: sudo apt install cgdb');
        console.error('Fedora:        sudo dnf install cgdb');
        console.error('Arch:          sudo pacman -S cgdb');
        return;
    }

    if (requested === 'pwnbg' || requested === 'pwndbg') {
        const command = process.platform === 'darwin' ? 'pwndbg-lldb' : 'pwndbg';
        console.error(`pwnbg needs the '${command}' command on PATH.`);
        if (process.platform === 'darwin') {
            console.error('Homebrew:      brew install --cask pwndbg-lldb');
        } else {
            console.error('Install Pwndbg, then make sure its launcher is on PATH.');
        }
        console.error('Docs:          https://github.com/pwndbg/pwndbg');
        return;
    }

    console.error(`Install '${requested}' or pass one of: lldb, lldb-gui, gdb, gdb-tui, cgdb, pwnbg.`);
}

function debuggerArgs(debuggerCommand, target, targetArgs) {
    if (isGdb(debuggerCommand)) {
        return ['-q', '-ex', 'set pagination off', '--args', target, ...targetArgs];
    }
    return ['--', target, ...targetArgs];
}

function isGdb(debuggerCommand) {
    return path.basename(debuggerCommand).toLowerCase().includes('gdb');
}

function targetLabel(target, targetArgs) {
    return [target, ...targetArgs].map(shellQuote).join(' ');
}

function shellQuote(value) {
    if (/^[A-Za-z0-9_./:=+-]+$/.test(value)) {
        return value;
    }
    return `'${value.replace(/'/g, "'\\''")}'`;
}

function runHighlighted(command, commandArgs, modeName, promptBase) {
    colors.enabled = !process.env.NO_COLOR;

    const child = spawn(command, commandArgs, {
        stdio: ['pipe', 'pipe', 'pipe'],
    });

    let prompt = '';
    let acceptingInput = false;
    let input = '';
    let cursor = 0;
    let outputTail = '';
    const outputState = { mode: 'normal' };
    let restored = false;

    readline.emitKeypressEvents(process.stdin);
    process.stdin.setRawMode(true);
    process.stdin.resume();

    function restoreTerminal() {
        if (restored) return;
        restored = true;
        if (process.stdin.isTTY) {
            process.stdin.setRawMode(false);
        }
        process.stdin.pause();
    }

    function renderInput() {
        if (!acceptingInput) return;

        process.stdout.write('\r\x1b[2K');
        process.stdout.write(highlightPrompt(prompt));
        process.stdout.write(highlightInput(input, modeName));

        const charsRight = input.length - cursor;
        if (charsRight > 0) {
            process.stdout.write(`\x1b[${charsRight}D`);
        }
    }

    function submitInput() {
        process.stdout.write('\n');
        child.stdin.write(`${input}\n`);
        input = '';
        cursor = 0;
        acceptingInput = false;
    }

    function insertText(text) {
        input = input.slice(0, cursor) + text + input.slice(cursor);
        cursor += text.length;
        renderInput();
    }

    child.stdout.on('data', (chunk) => {
        const text = chunk.toString('utf8');
        process.stdout.write(highlightOutput(text, outputState, modeName));

        outputTail = (outputTail + text).slice(-32);
        const escapedPrompt = promptBase.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        const match = outputTail.match(new RegExp(`(${escapedPrompt}[>|] )$`));
        if (match) {
            prompt = match[1];
            acceptingInput = true;
            if (input.length > 0) {
                renderInput();
            }
        }
    });

    child.stderr.on('data', (chunk) => {
        process.stderr.write(highlightErrorOutput(chunk.toString('utf8')));
    });

    child.on('exit', (code, signal) => {
        restoreTerminal();
        if (signal) process.kill(process.pid, signal);
        else process.exit(code ?? 0);
    });

    child.on('error', (error) => {
        restoreTerminal();
        console.error(error.message);
        process.exit(1);
    });

    process.stdin.on('keypress', (str, key = {}) => {
        if (key.ctrl && key.name === 'c') {
            restoreTerminal();
            child.kill('SIGINT');
            process.exit(130);
        }

        if (key.ctrl && key.name === 'd') {
            if (input.length === 0) {
                child.stdin.end();
                return;
            }
            input = input.slice(0, cursor) + input.slice(cursor + 1);
            renderInput();
            return;
        }

        if (str && /[\r\n]/.test(str)) {
            for (const char of str) {
                if (char === '\r' || char === '\n') {
                    submitInput();
                } else if (char >= ' ') {
                    insertText(char);
                }
            }
            return;
        }

        if (key.name === 'return') {
            submitInput();
            return;
        }

        if (key.name === 'backspace') {
            if (cursor > 0) {
                input = input.slice(0, cursor - 1) + input.slice(cursor);
                cursor--;
                renderInput();
            }
            return;
        }

        if (key.name === 'delete') {
            if (cursor < input.length) {
                input = input.slice(0, cursor) + input.slice(cursor + 1);
                renderInput();
            }
            return;
        }

        if (key.name === 'left') {
            if (cursor > 0) {
                cursor--;
                renderInput();
            }
            return;
        }

        if (key.name === 'right') {
            if (cursor < input.length) {
                cursor++;
                renderInput();
            }
            return;
        }

        if (key.name === 'home') {
            cursor = 0;
            renderInput();
            return;
        }

        if (key.name === 'end') {
            cursor = input.length;
            renderInput();
            return;
        }

        if (str && !key.ctrl && !key.meta) {
            insertText(str);
        }
    });

    process.on('exit', restoreTerminal);
    process.on('SIGTERM', () => {
        restoreTerminal();
        child.kill('SIGTERM');
        process.exit(143);
    });
}

function highlightOutput(text, state = { mode: 'normal' }, modeName = 'asm') {
    if (!colors.enabled || text.length === 0) {
        return text;
    }

    return text
        .split(/(\r?\n)/)
        .map((part) => (/^\r?\n$/.test(part) ? part : highlightOutputLine(part, state, modeName)))
        .join('');
}

function highlightOutputLine(line, state, modeName = 'asm') {
    if (line.length === 0) {
        state.mode = 'normal';
        return line;
    }

    if (/^(?:asm|c|cpp|objc|ir|wasm|rust|zig|go)[>|] $/.test(line)) {
        state.mode = 'normal';
        return highlightPrompt(line);
    }

    const promptMatch = line.match(/^((?:asm|c|cpp|objc|ir|wasm|rust|zig|go)[>|] )(.*)$/);
    if (promptMatch) {
        state.mode = 'normal';
        return highlightPrompt(promptMatch[1]) + highlightOutputLine(promptMatch[2], state, modeName);
    }

    if (/^Syntax:$/.test(line)) {
        state.mode = 'syntax';
        return colors.bold.underline(line);
    }

    if (/^Examples:$/.test(line)) {
        state.mode = 'examples';
        return colors.bold.underline(line);
    }

    if (/^Aliases\/forms:/.test(line)) {
        return colorAliasesLine(line);
    }

    if (/native assembly REPL/.test(line) || /^(?:C|C\+\+|Objective-C|LLVM IR|WebAssembly|Rust|Zig|Go) REPL/.test(line)) {
        state.mode = 'normal';
        return colors.bold(line);
    }

    if (/^Compiler:/.test(line)) {
        return line.replace(/^Compiler:/, colors.bold('Compiler:'))
            .replace(/(\/[^\s]+)/, (compilerPath) => colors.cyan(compilerPath));
    }

    if (/^(Commands|Instruction help|Block mode|Notes|Help topics|Startup flags(?: for the public command)?|Execution model|Safety):$/.test(line)) {
        state.mode = /^Startup flags/.test(line)
            ? 'startup-flags'
            : line.slice(0, -1).toLowerCase().replace(/\s+/g, '-');
        return colors.bold.underline(line);
    }

    if (/^Built-in instruction help topics for /.test(line) ||
        /^Built-in help topics for /.test(line) ||
        /^LLVM IR instructions discovered from /.test(line) ||
        /^WebAssembly instructions supported by /.test(line)) {
        state.mode = 'instruction-list';
        return colorOutputTokens(line, { numbers: false });
    }

    if (/^No built-in help for /.test(line) || /^Could not discover LLVM IR instructions/.test(line) ||
        /^wasm validation failed:/.test(line) || /^WebAssembly (?:compilation failed|trap):/.test(line)) {
        return colors.yellow(line);
    }

    if (/^definition block committed$|^definition block started|^instruction block committed$|^instruction block started|^directive persisted$|^import (?:persisted|already persisted)$|^state reset$|^register context reset$|^(?:imports and definitions|definitions) cleared$|^definitions and (?:LLVM IR|WebAssembly) body cleared$/.test(line)) {
        return colors.green(line);
    }

    if (state.mode === 'commands') {
        return colorCommandHelpLine(line);
    }

    if (state.mode === 'instruction-list') {
        return colorInstructionListLine(line);
    }

    if (state.mode === 'syntax' || state.mode === 'examples') {
        return highlightInput(line, modeName);
    }

    if (state.mode === 'instruction-help') {
        return colorInstructionHelpLine(line);
    }

    if (/^[A-Za-z.][\w.]* - /.test(line)) {
        return colorInstructionSummaryLine(line);
    }

    if (state.mode === 'block-mode' || state.mode === 'notes' || state.mode === 'help-topics' ||
        state.mode === 'startup-flags' || state.mode === 'execution-model' || state.mode === 'safety') {
        return colorOutputTokens(line, { commands: true });
    }

    if (isRegisterOutputLine(line) || isSourceStateOutputLine(line) || /^scratch:/.test(line) || /^scratch size:/.test(line)) {
        return colorOutputTokens(line, { commands: false });
    }

    if (/^  :/.test(line)) {
        return colorCommandHelpLine(line);
    }

    if (/^  Add \? after/.test(line) || /^  A full line ending/.test(line)) {
        return colorInstructionHelpLine(line);
    }

    return colorOutputTokens(line, { commands: true });
}

function highlightErrorOutput(text) {
    if (!colors.enabled || text.length === 0) {
        return text;
    }

    return text
        .split(/(\r?\n)/)
        .map((part) => (/^\r?\n$/.test(part) ? part : colors.red(part)))
        .join('');
}

function colorCommandHelpLine(line) {
    const spans = [];
    addRegexSpans(line, spans, /:[A-Za-z][\w-]*/g, colors.magenta);
    addRegexSpans(line, spans, /<[^>]+>/g, colors.cyan);
    return renderSpans(line, spans);
}

function colorAliasesLine(line) {
    const spans = [];
    addRegexSpans(line, spans, /^Aliases\/forms:/, colors.bold);
    addRegexSpans(line, spans, /\b[A-Za-z.][\w.]*\b/g, colors.green);
    return renderSpans(line, spans);
}

function colorInstructionListLine(line) {
    if (/^\s+aliases\/forms:/.test(line)) {
        const spans = [];
        addRegexSpans(line, spans, /aliases\/forms:/, colors.dim);
        addRegexSpans(line, spans, /\b[A-Za-z.][\w.]*\b/g, colors.green);
        return renderSpans(line, spans);
    }

    const spans = [];
    addRegexSpans(line, spans, /^  [A-Za-z.][\w.]*/, colors.green.bold);
    return renderSpans(line, spans);
}

function colorInstructionSummaryLine(line) {
    const spans = [];
    addRegexSpans(line, spans, /^[A-Za-z.][\w.]*/, colors.green.bold);
    addRegexSpans(line, spans, / - /, colors.dim);
    return renderSpans(line, spans);
}

function colorInstructionHelpLine(line) {
    const spans = [];
    addRegexSpans(line, spans, /[A-Za-z.][\w.]*\?/g, (value) => highlightAssembly(value));
    addRegexSpans(line, spans, /\b(?:adc|adcs|add|adds|adr|adrp|and|ands|asr|b|bl|blr|br|cbz|cbnz|cmp|cmn|csel|eor|ldp|ldr|ldrb|ldrh|ldrsw|lsl|lsr|madd|mov|movk|movz|movn|mrs|msr|mul|mvn|neg|nop|orr|ret|str|sub|svc|tbz|tst|call|jmp|lea|push|pop|syscall|test|xor)\b/g, colors.green.bold);
    addRegexSpans(line, spans, registerPattern(), colors.cyan);
    addRegexSpans(line, spans, /#?-?(?:0x[0-9a-f]+|\b\d+\b)/gi, colors.yellow);
    addRegexSpans(line, spans, /:[A-Za-z][\w-]*/g, colors.magenta);
    addRegexSpans(line, spans, /<[^>]+>/g, colors.cyan);
    return renderSpans(line, spans);
}

function colorOutputTokens(line, options = {}) {
    const spans = [];
    const includeCommands = options.commands !== false;
    const includeNumbers = options.numbers !== false;

    if (includeCommands) {
        addRegexSpans(line, spans, /:[A-Za-z][\w-]*/g, colors.magenta);
    }

    addRegexSpans(line, spans, /\[[A-Za-z]+\]/g, colors.green);
    addRegexSpans(line, spans, registerPattern(), colors.cyan);
    addRegexSpans(line, spans, /\b(?:result|u\d+|f\d+|state|scratch|out)\b/g, colors.cyan);
    if (includeNumbers) {
        addRegexSpans(line, spans, /#?-?(?:0x[0-9a-f]+|\b\d+\b)/gi, colors.yellow);
    }
    addRegexSpans(line, spans, /\b(NZCV|OSZAPC)\b/g, colors.green);

    return renderSpans(line, spans);
}

function isRegisterOutputLine(line) {
    return /\b(?:x\d+|sp|nzcv|rflags|r(?:ax|bx|cx|dx|bp|sp|si|di|1[0-5]|[0-9]))\b/.test(line) &&
        /0x[0-9a-f]+/i.test(line);
}

function isSourceStateOutputLine(line) {
    return /^(?:result|u\d+|f\d+|scratch\[|out:)/.test(line) || /\bu\d+\s+0x[0-9a-f]+/i.test(line);
}

function highlightPrompt(value) {
    if (!colors.enabled) {
        return value;
    }
    return colors.gray.bold(value);
}

function highlightInput(line, modeName) {
    if (modeName === 'asm') {
        return highlightAssembly(line);
    }
    if (modeName === 'llvmir') {
        return highlightLLVMIR(line);
    }
    if (modeName === 'wasm') {
        return highlightWasm(line);
    }
    return highlightSource(line, modeName);
}

function highlightSource(line, modeName) {
    if (!colors.enabled || line.length === 0) {
        return line;
    }

    const commentIndex = findSourceCommentIndex(line);
    const code = commentIndex >= 0 ? line.slice(0, commentIndex) : line;
    const comment = commentIndex >= 0 ? line.slice(commentIndex) : '';

    if (/^\s*:/.test(code)) {
        return colors.magenta(code) + colors.dim(comment);
    }

    const spans = [];
    addRegexSpans(code, spans, /^\s*#\s*[A-Za-z_]\w*/, colors.cyan.bold);
    addRegexSpans(code, spans, /@"(?:\\.|[^"\\])*"|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'/g, colors.yellow);
    addRegexSpans(code, spans, /\b(?:0x[0-9a-f]+|\d+(?:\.\d+)?)\b/gi, colors.yellow);
    addRegexSpans(code, spans, /\b(?:state|U|F|SCRATCH|print|repl_state_t|uint64_t|uint32_t|uint16_t|uint8_t|int64_t|int32_t|size_t|bool|true|false|nullptr|NULL)\b/g, colors.cyan);

    const cKeywords = /\b(?:auto|break|case|char|const|continue|default|do|double|else|enum|extern|float|for|goto|if|inline|int|long|register|return|short|signed|sizeof|static|struct|switch|typedef|union|unsigned|void|volatile|while|_Bool|_Atomic)\b/g;
    const cppKeywords = /\b(?:alignas|alignof|and|and_eq|asm|bitand|bitor|catch|class|concept|const_cast|constexpr|decltype|delete|dynamic_cast|explicit|export|friend|mutable|namespace|new|noexcept|not|operator|or|private|protected|public|reinterpret_cast|requires|static_assert|static_cast|template|this|throw|try|typename|using|virtual|xor)\b/g;
    const objcKeywords = /@(?:autoreleasepool|class|defs|dynamic|encode|end|implementation|interface|private|property|protected|protocol|public|selector|synthesize|try|catch|finally|throw)\b|\b(?:id|SEL|BOOL|YES|NO|nil|self|super|NSInteger|NSUInteger|NSString|NSArray|NSDictionary|NSObject)\b/g;
    const rustKeywords = /\b(?:as|async|await|break|const|continue|crate|dyn|else|enum|extern|false|fn|for|if|impl|in|let|loop|match|mod|move|mut|pub|ref|return|self|Self|static|struct|super|trait|true|type|unsafe|use|where|while|u8|u16|u32|u64|usize|i8|i16|i32|i64|isize|f32|f64|bool|str)\b/g;
    const zigKeywords = /\b(?:addrspace|align|allowzero|and|anyframe|anytype|asm|async|await|break|callconv|catch|comptime|const|continue|defer|else|enum|errdefer|error|export|extern|false|fn|for|if|inline|noalias|null|opaque|or|orelse|packed|pub|resume|return|struct|suspend|switch|test|threadlocal|true|try|undefined|union|unreachable|usingnamespace|var|volatile|while|u8|u16|u32|u64|usize|i8|i16|i32|i64|isize|f32|f64|bool|void)\b/g;
    const goKeywords = /\b(?:break|case|chan|const|continue|default|defer|else|fallthrough|for|func|go|goto|if|import|interface|map|package|range|return|select|struct|switch|type|var|true|false|nil|uint8|uint16|uint32|uint64|uint|uintptr|int8|int16|int32|int64|int|float32|float64|bool|string|byte|rune|any)\b/g;

    addRegexSpans(code, spans, cKeywords, colors.green.bold);
    if (modeName === 'cpp') {
        addRegexSpans(code, spans, cppKeywords, colors.green.bold);
    } else if (modeName === 'objc') {
        addRegexSpans(code, spans, objcKeywords, colors.green.bold);
    } else if (modeName === 'rust') {
        addRegexSpans(code, spans, rustKeywords, colors.green.bold);
        addRegexSpans(code, spans, /\b[A-Za-z_]\w*!/g, colors.blue.bold);
    } else if (modeName === 'zig') {
        addRegexSpans(code, spans, zigKeywords, colors.green.bold);
        addRegexSpans(code, spans, /@\w+/g, colors.blue.bold);
    } else if (modeName === 'go') {
        addRegexSpans(code, spans, goKeywords, colors.green.bold);
    }

    addRegexSpans(code, spans, /[{}()[\],.;*+\-/=%!<>?:&|]/g, colors.magenta);
    return renderSpans(code, spans) + colors.dim(comment);
}

function highlightLLVMIR(line) {
    if (!colors.enabled || line.length === 0) {
        return line;
    }

    const commentIndex = line.indexOf(';');
    const code = commentIndex >= 0 ? line.slice(0, commentIndex) : line;
    const comment = commentIndex >= 0 ? line.slice(commentIndex) : '';

    if (/^\s*:/.test(code)) {
        return colors.magenta(code) + colors.dim(comment);
    }

    const spans = [];
    addRegexSpans(code, spans, /%[-A-Za-z$._0-9]+|@[-A-Za-z$._0-9]+/g, colors.cyan);
    addRegexSpans(code, spans, /\b(?:define|declare|global|constant|private|internal|external|dso_local|ret|br|switch|indirectbr|invoke|call|tail|musttail|notail|alloca|load|store|getelementptr|icmp|fcmp|phi|select|add|fadd|sub|fsub|mul|fmul|udiv|sdiv|fdiv|urem|srem|and|or|xor|shl|lshr|ashr|trunc|zext|sext|fptrunc|fpext|ptrtoint|inttoptr|bitcast|addrspacecast|insertvalue|extractvalue|landingpad|resume|unreachable|void|ptr|label|metadata)\b/g, colors.green.bold);
    addRegexSpans(code, spans, /\b(?:i1|i8|i16|i32|i64|float|double|half)\b/g, colors.blue.bold);
    addRegexSpans(code, spans, /\b(?:0x[0-9a-f]+|-?\d+(?:\.\d+)?)\b/gi, colors.yellow);
    addRegexSpans(code, spans, /[{}()[\],=*]/g, colors.magenta);
    return renderSpans(code, spans) + colors.dim(comment);
}

function highlightWasm(line) {
    if (!colors.enabled || line.length === 0) {
        return line;
    }

    const commentIndex = findWasmCommentIndex(line);
    const code = commentIndex >= 0 ? line.slice(0, commentIndex) : line;
    const comment = commentIndex >= 0 ? line.slice(commentIndex) : '';

    if (/^\s*:/.test(code)) {
        return colors.magenta(code) + colors.dim(comment);
    }

    const spans = [];
    addRegexSpans(code, spans, /\$[-A-Za-z$._0-9]+/g, colors.cyan);
    addRegexSpans(code, spans, /\b(?:i32|i64|f32|f64)\.(?:const|load|load8_s|load8_u|load16_s|load16_u|load32_s|load32_u|store|store8|store16|store32|eqz|eq|ne|lt_s|lt_u|lt|gt_s|gt_u|gt|le_s|le_u|le|ge_s|ge_u|ge|clz|ctz|popcnt|add|sub|mul|div_s|div_u|div|rem_s|rem_u|and|or|xor|shl|shr_s|shr_u|rotl|rotr|abs|neg|ceil|floor|trunc|nearest|sqrt|min|max|copysign|wrap_i64|trunc_f32_s|trunc_f32_u|trunc_f64_s|trunc_f64_u|extend_i32_s|extend_i32_u|convert_i32_s|convert_i32_u|convert_i64_s|convert_i64_u|demote_f64|promote_f32|reinterpret_f32|reinterpret_f64|reinterpret_i32|reinterpret_i64|extend8_s|extend16_s|extend32_s)\b/g, colors.green.bold);
    addRegexSpans(code, spans, /\b(?:global\.get|global\.set|memory\.size|memory\.grow|call|drop|select|nop)\b/g, colors.green.bold);
    addRegexSpans(code, spans, /\b(?:i32|i64|f32|f64)\b/g, colors.blue.bold);
    addRegexSpans(code, spans, /\b(?:offset|align)=#?-?(?:0x[0-9a-f]+|\d+)\b/gi, colors.yellow);
    addRegexSpans(code, spans, /#?-?(?:0x[0-9a-f]+|\b\d+(?:\.\d+)?\b|inf|nan)/gi, colors.yellow);
    addRegexSpans(code, spans, /[()]/g, colors.magenta);
    return renderSpans(code, spans) + colors.dim(comment);
}

function highlightAssembly(line) {
    if (!colors.enabled || line.length === 0) {
        return line;
    }

    const commentIndex = findCommentIndex(line);
    const code = commentIndex >= 0 ? line.slice(0, commentIndex) : line;
    const comment = commentIndex >= 0 ? line.slice(commentIndex) : '';

    if (/^\s*:/.test(code)) {
        return colors.magenta(code) + colors.dim(comment);
    }

    if (/^\s*\./.test(code)) {
        return colors.cyan(code) + colors.dim(comment);
    }

    const spans = [];
    let offset = 0;
    const labelMatch = code.match(/^(\s*)([A-Za-z_.$][\w.$]*:)/);
    if (labelMatch) {
        const start = labelMatch[1].length;
        const end = start + labelMatch[2].length;
        spans.push({ start, end, color: colors.blue.bold });
        offset = end;
    }

    const mnemonicMatch = code.slice(offset).match(/^(\s*)([A-Za-z.][\w.]*)/);
    if (mnemonicMatch) {
        const start = offset + mnemonicMatch[1].length;
        const end = start + mnemonicMatch[2].length;
        spans.push({ start, end, color: colors.green.bold });
    }

    const tokenPattern = /\b(?:[wx](?:[0-2]?\d|3[01]|zr|sp)|[xw][admnts]?|sp|nzcv|r(?:1[0-5]|[0-9]|ax|bx|cx|dx|bp|sp|si|di|flags)|e(?:ax|bx|cx|dx|bp|sp|si|di)|[abcd][lh]|dst|src|lhs|rhs|label|reg|mem|cond|imm\d*|system_register|address-expression)\b|r\/m8|#?-?(?:0x[0-9a-f]+|\d+)|[\[\]]/gi;
    for (const match of code.matchAll(tokenPattern)) {
        const start = match.index;
        const end = start + match[0].length;
        if (spans.some((span) => rangesOverlap(start, end, span.start, span.end))) {
            continue;
        }

        let color = colors.yellow;
        if (match[0] === '[' || match[0] === ']') {
            color = colors.magenta;
        } else if (/^[A-Za-z]/.test(match[0])) {
            color = colors.cyan;
        }
        spans.push({ start, end, color });
    }

    return renderSpans(code, spans) + colors.dim(comment);
}

function findCommentIndex(line) {
    const slash = line.indexOf('//');
    const semi = line.indexOf(';');

    if (slash === -1) return semi;
    if (semi === -1) return slash;
    return Math.min(slash, semi);
}

function findSourceCommentIndex(line) {
    const slash = line.indexOf('//');
    const block = line.indexOf('/*');
    if (slash === -1) return block;
    if (block === -1) return slash;
    return Math.min(slash, block);
}

function findWasmCommentIndex(line) {
    const wat = line.indexOf(';;');
    const slash = line.indexOf('//');
    if (wat === -1) return slash;
    if (slash === -1) return wat;
    return Math.min(wat, slash);
}

function executable(file) {
    try {
        fs.accessSync(file, fs.constants.X_OK);
        return true;
    } catch {
        return false;
    }
}

function fileExists(file) {
    try {
        return fs.statSync(file).isFile();
    } catch {
        return false;
    }
}

function ensureExecutable(file) {
    if (!fileExists(file)) {
        return false;
    }

    if (executable(file)) {
        return true;
    }

    try {
        const mode = fs.statSync(file).mode;
        fs.chmodSync(file, mode | 0o755);
    } catch {
        return false;
    }

    return executable(file);
}

function commandAvailable(command) {
    const args = command === 'go' || command === 'zig' ? ['version'] : ['--version'];
    const result = spawnSync(command, args, { stdio: 'ignore' });
    return !result.error && result.status === 0;
}

function commandExists(command) {
    if (command.includes(path.sep)) {
        return executable(command);
    }

    const pathEnv = process.env.PATH || '';
    for (const dir of pathEnv.split(path.delimiter)) {
        if (!dir) continue;
        if (executable(path.join(dir, command))) {
            return true;
        }
    }
    return false;
}

function runtimeDependenciesForMode(modeName) {
    if (modeName === 'wasm') {
        return [];
    }

    if (modeName === 'cpp') {
        return [
            {
                command: 'clang++',
                reason: 'cpp-repl compiles each snippet with clang++ at runtime.',
            },
        ];
    }

    if (modeName === 'rust') {
        return [
            {
                command: 'rustc',
                reason: 'rust-repl compiles each snippet with rustc at runtime.',
            },
        ];
    }

    if (modeName === 'zig') {
        return [
            {
                command: 'zig',
                reason: 'zig-repl compiles each snippet with zig at runtime.',
            },
        ];
    }

    if (modeName === 'go') {
        return [
            {
                command: 'go',
                reason: 'go-repl builds and runs each snippet with the Go toolchain at runtime.',
            },
        ];
    }

    return [
        {
            command: 'clang',
            reason: `${modeName === 'asm' ? 'assembly-repl assembles' : `${commandName} compiles`} each snippet with clang at runtime.`,
        },
    ];
}

function printMissingDependencyError(command, dependencies) {
    const names = dependencies.map((dependency) => dependency.command).join(', ');
    console.error(`${command}: missing runtime dependency${dependencies.length === 1 ? '' : 'ies'}: ${names}`);
    console.error('');
    console.error('The npm package installed successfully and does not build native code during install.');
    console.error('These tools are still required on PATH when you run the REPL:');
    for (const dependency of dependencies) {
        console.error(`  - ${dependency.command}: ${dependency.reason}`);
    }
    console.error('');
    console.error(`Install ${dependencies.length === 1 ? dependencies[0].command : 'the missing tools'}, then run ${command} again.`);
    for (const dependency of dependencies) {
        printDependencyInstallHints(dependency.command);
    }
}

function printDependencyInstallHints(command) {
    const hints = {
        clang: [
            'macOS:        xcode-select --install',
            'Debian/Ubuntu: sudo apt install clang',
            'Fedora:       sudo dnf install clang',
            'Arch:         sudo pacman -S clang',
        ],
        'clang++': [
            'macOS:        xcode-select --install',
            'Debian/Ubuntu: sudo apt install clang',
            'Fedora:       sudo dnf install clang',
            'Arch:         sudo pacman -S clang',
        ],
        rustc: [
            'macOS:        brew install rust',
            'Debian/Ubuntu: sudo apt install rustc',
            'Fedora:       sudo dnf install rust',
            'Arch:         sudo pacman -S rust',
        ],
        zig: [
            'macOS:        brew install zig',
            'Debian/Ubuntu: install Zig from https://ziglang.org/download/',
            'Fedora:       sudo dnf install zig',
            'Arch:         sudo pacman -S zig',
        ],
        go: [
            'macOS:        brew install go',
            'Debian/Ubuntu: sudo apt install golang-go',
            'Fedora:       sudo dnf install golang',
            'Arch:         sudo pacman -S go',
        ],
    };
    const lines = hints[command] || [`Install ${command} and make sure it is on PATH.`];
    for (const line of lines) {
        console.error(line);
    }
}

function rangesOverlap(aStart, aEnd, bStart, bEnd) {
    return aStart < bEnd && bStart < aEnd;
}

function registerPattern() {
    return /\b(?:[wx](?:[0-2]?\d|3[01]|zr|sp)|sp|nzcv|r(?:1[0-5]|[0-9]|ax|bx|cx|dx|bp|sp|si|di|flags)|e(?:ax|bx|cx|dx|bp|sp|si|di)|[abcd][lh])\b/gi;
}

function addRegexSpans(text, spans, regex, color) {
    const globalRegex = regex.global ? regex : new RegExp(regex.source, `${regex.flags}g`);
    for (const match of text.matchAll(globalRegex)) {
        const start = match.index;
        const end = start + match[0].length;
        if (spans.some((span) => rangesOverlap(start, end, span.start, span.end))) {
            continue;
        }
        spans.push({ start, end, color });
    }
}

function renderSpans(text, spans) {
    let result = '';
    let position = 0;

    spans.sort((a, b) => a.start - b.start || b.end - a.end);
    for (const span of spans) {
        if (span.start < position) {
            continue;
        }
        result += text.slice(position, span.start);
        result += span.color(text.slice(span.start, span.end));
        position = span.end;
    }

    return result + text.slice(position);
}
