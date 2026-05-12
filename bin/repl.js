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
};
const mode = modeByCommand[commandName] || 'asm';
const promptBaseByMode = {
    asm: 'asm',
    c: 'c',
    cpp: 'cpp',
    objc: 'objc',
    llvmir: 'ir',
    wasm: 'wasm',
};
const binaryBase = mode === 'asm' ? 'assembly-repl' : 'language-repl';
const localBinary = path.join(root, mode === 'asm' ? 'asmrepl' : 'language-repl');
const prebuiltBinary = path.join(root, 'prebuilds', dir, binaryBase);
const binary = mode === 'wasm' ? process.execPath : (fileExists(localBinary) ? localBinary : prebuiltBinary);
const runtimeDependencies = runtimeDependenciesForMode(mode);
const args = process.argv.slice(2);
const noHighlight = args.includes('--no-highlight') ||
    process.env.REPL_NO_HIGHLIGHT === '1' ||
    process.env.ASMREPL_NO_HIGHLIGHT === '1';
const childArgs = args.filter((arg) => arg !== '--no-highlight');
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

if (noHighlight || !process.stdin.isTTY || !process.stdout.isTTY) {
    const child = spawn(binary, effectiveChildArgs, { stdio: 'inherit' });
    child.on('exit', (code, signal) => {
        if (signal) process.kill(process.pid, signal);
        else process.exit(code ?? 0);
    });
} else {
    runHighlighted(binary, effectiveChildArgs, mode, promptBaseByMode[mode]);
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

    if (/^(?:asm|c|cpp|objc|ir|wasm)[>|] $/.test(line)) {
        state.mode = 'normal';
        return highlightPrompt(line);
    }

    const promptMatch = line.match(/^((?:asm|c|cpp|objc|ir|wasm)[>|] )(.*)$/);
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

    if (/native assembly REPL/.test(line) || /^(?:C|C\+\+|Objective-C|LLVM IR|WebAssembly) REPL/.test(line)) {
        state.mode = 'normal';
        return colors.bold(line);
    }

    if (/^Compiler:/.test(line)) {
        return line.replace(/^Compiler:/, colors.bold('Compiler:'))
            .replace(/(\/[^\s]+)/, (compilerPath) => colors.cyan(compilerPath));
    }

    if (/^(Commands|Instruction help|Block mode|Notes|Help topics|Execution model|Safety):$/.test(line)) {
        state.mode = line.slice(0, -1).toLowerCase().replace(/\s+/g, '-');
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

    if (/^definition block committed$|^definition block started|^instruction block committed$|^instruction block started|^directive persisted$|^state reset$|^register context reset$|^definitions cleared$|^definitions and (?:LLVM IR|WebAssembly) body cleared$/.test(line)) {
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
        state.mode === 'execution-model' || state.mode === 'safety') {
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

    addRegexSpans(code, spans, cKeywords, colors.green.bold);
    if (modeName === 'cpp') {
        addRegexSpans(code, spans, cppKeywords, colors.green.bold);
    } else if (modeName === 'objc') {
        addRegexSpans(code, spans, objcKeywords, colors.green.bold);
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
    const result = spawnSync(command, ['--version'], { stdio: 'ignore' });
    return !result.error && result.status === 0;
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
    console.error('macOS:        xcode-select --install');
    console.error('Debian/Ubuntu: sudo apt install clang');
    console.error('Fedora:       sudo dnf install clang');
    console.error('Arch:         sudo pacman -S clang');
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
