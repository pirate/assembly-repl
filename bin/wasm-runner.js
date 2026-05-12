#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const readline = require('readline');

const root = path.join(__dirname, '..');
const buildDir = path.join(root, '.repl-build');
const scratchSize = 4096;
const outOffset = scratchSize;
const outSize = 4096;
const pageSize = 65536;

const wasmType = {
    i32: 0x7f,
    i64: 0x7e,
    f32: 0x7d,
    f64: 0x7c,
};

class ReplCompileError extends Error {
    constructor(message) {
        super(message);
        this.name = 'ReplCompileError';
    }
}

const builtinFunctions = new Map([
    ['$print_i64', { index: 0, importName: 'print_i64', typeIndex: 1, params: ['i64'], results: [], summary: 'Append an i64 value to out.', examples: 'i64.const 42\ncall $print_i64' }],
    ['$print_i32', { index: 1, importName: 'print_i32', typeIndex: 2, params: ['i32'], results: [], summary: 'Append an i32 value to out.', examples: 'i32.const 42\ncall $print_i32' }],
    ['$print_f64', { index: 2, importName: 'print_f64', typeIndex: 3, params: ['f64'], results: [], summary: 'Append an f64 value to out.', examples: 'f64.const 3.14\ncall $print_f64' }],
    ['$host_time_ms', { index: 3, importName: 'host_time_ms', typeIndex: 4, params: [], results: ['i64'], summary: 'Read the host wall clock as Unix milliseconds.', examples: 'call $host_time_ms\nglobal.set $u0' }],
]);
const mainFunctionIndex = builtinFunctions.size;

const globalSpecs = [
    { name: '$result', importName: 'result', type: 'i64', summary: 'Printed integer result.' },
];
for (let i = 0; i < 16; i++) {
    globalSpecs.push({ name: `$u${i}`, importName: `u${i}`, type: 'i64', summary: `Persistent i64 slot u${i}.` });
}
for (let i = 0; i < 16; i++) {
    globalSpecs.push({ name: `$f${i}`, importName: `f${i}`, type: 'f64', summary: `Persistent f64 slot f${i}.` });
}

const globalsByName = new Map(globalSpecs.map((spec, index) => [spec.name, { ...spec, index }]));

const topics = [
    {
        topic: 'state',
        aliases: 'slot slots global globals',
        summary: 'Persistent REPL state exposed as imported WebAssembly globals and memory.',
        syntax: 'global.get $result\n' +
            'global.get $u0 ... global.get $u15\n' +
            'global.get $f0 ... global.get $f15\n' +
            'i32.const <offset>  ;; address inside imported memory',
        examples: 'i64.const 42\n' +
            'global.set $u0\n' +
            'global.get $u0',
        notes: '$result is printed as result. $u0..$u15 and $f0..$f15 persist across snippets. Memory offset 0 starts the 4096-byte scratch area; offset 4096 starts the out buffer.',
    },
    {
        topic: 'body',
        aliases: 'source clear accumulated',
        summary: 'The accumulated WebAssembly instruction body compiled on every run.',
        syntax: ':body\n:clear',
        examples: 'i64.const 40\ni64.const 2\ni64.add\n:body',
        notes: 'Like llvmir-repl, wasm-repl recompiles and executes the whole accumulated body after each accepted line. Use :clear to start a new body.',
    },
    {
        topic: 'stack',
        aliases: 'operand operands',
        summary: 'The generated function uses normal WebAssembly operand stack validation.',
        syntax: '<producer instruction>\n<consumer instruction>',
        examples: 'i64.const 40\ni64.const 2\ni64.add',
        notes: 'If the accumulated body leaves values on the stack, wasm-repl emits a small epilogue: the top i32/i64 value updates $result, the top f32/f64 value updates $f0, and older stack values are dropped.',
    },
    {
        topic: 'memory',
        aliases: 'load store scratch out',
        summary: 'Use imported linear memory for scratch bytes and string output.',
        syntax: 'i32.const <addr>\n<type>.load [offset=N] [align=N]\n' +
            'i32.const <addr>\n<value>\n<type>.store [offset=N] [align=N]',
        examples: 'i32.const 0\ni64.const 0xfeedface\ni64.store\ni32.const 0\ni64.load',
        notes: 'The scratch area starts at offset 0. The out buffer starts at offset 4096 and is printed as a null-terminated UTF-8 string when non-empty.',
    },
    {
        topic: 'call',
        aliases: 'host print import',
        summary: 'Call built-in imported helper functions.',
        syntax: 'call $print_i64\ncall $print_i32\ncall $print_f64\ncall $host_time_ms',
        examples: 'call $host_time_ms\nglobal.set $u0',
        notes: 'Host calls are ordinary imported WebAssembly functions. The print helpers append a line to out; $host_time_ms returns the host wall clock as Unix milliseconds.',
    },
    {
        topic: 'const',
        aliases: 'constant i32.const i64.const f32.const f64.const',
        summary: 'Push a numeric constant onto the operand stack.',
        syntax: 'i32.const <s32>\ni64.const <s64>\nf32.const <number>\nf64.const <number>',
        examples: 'i64.const 42\ni64.const -1\nf64.const 3.14',
        notes: 'Integer literals may be decimal or hexadecimal. Unsigned 64-bit hexadecimal values are accepted and encoded as their two\'s-complement bit pattern.',
    },
    {
        topic: 'add',
        aliases: 'i32.add i64.add f32.add f64.add',
        summary: 'Pop two values of the same type and push their sum.',
        syntax: 'i32.add\ni64.add\nf32.add\nf64.add',
        examples: 'i64.const 40\ni64.const 2\ni64.add',
        notes: 'The operands must already be on the WebAssembly operand stack.',
    },
    {
        topic: 'global.get',
        aliases: 'global.set globals',
        summary: 'Read or write a persistent imported global.',
        syntax: 'global.get $name\nglobal.set $name',
        examples: 'i64.const 10\nglobal.set $u0\nglobal.get $u0',
        notes: 'Available globals are $result, $u0..$u15, and $f0..$f15.',
    },
];

const instructions = new Map();

function defineInstruction(name, spec) {
    instructions.set(name, {
        name,
        syntax: name,
        examples: defaultExamplesForInstruction(name, spec),
        notes: null,
        ...spec,
    });
}

function defineSimple(name, opcode, pop, push, summary) {
    defineInstruction(name, {
        kind: 'simple',
        opcode,
        pop,
        push,
        summary,
    });
}

function defineConst(name, opcode, type) {
    defineInstruction(name, {
        kind: 'const',
        opcode,
        type,
        summary: `Push an ${type} constant.`,
        syntax: `${name} <value>`,
    });
}

function defineLoad(name, opcode, resultType, alignBytes, summary) {
    defineInstruction(name, {
        kind: 'load',
        opcode,
        resultType,
        alignBytes,
        summary,
        syntax: `${name} [offset=N] [align=N]`,
    });
}

function defineStore(name, opcode, valueType, alignBytes, summary) {
    defineInstruction(name, {
        kind: 'store',
        opcode,
        valueType,
        alignBytes,
        summary,
        syntax: `${name} [offset=N] [align=N]`,
    });
}

function defaultExamplesForInstruction(name, spec) {
    if (name === 'drop') return 'i64.const 1\ndrop';
    if (name === 'select') return 'i64.const 11\ni64.const 22\ni32.const 1\nselect';
    if (name === 'global.get') return 'global.get $u0';
    if (name === 'global.set') return 'i64.const 42\nglobal.set $u0';
    if (name === 'call') return 'call $host_time_ms';
    if (spec.kind === 'const') return `${name} ${spec.type.startsWith('f') ? '3.14' : '42'}`;
    if (spec.kind === 'load') return `i32.const 0\n${name}`;
    if (spec.kind === 'store') return `i32.const 0\n${spec.valueType}.const 42\n${name}`;
    if (name.endsWith('.eqz')) return `${name.slice(0, 3)}.const 0\n${name}`;
    if (spec.pop && spec.pop.length === 2) {
        const type = spec.pop[0];
        return `${type}.const 40\n${type}.const 2\n${name}`;
    }
    if (spec.pop && spec.pop.length === 1) {
        const type = spec.pop[0];
        return `${type}.const ${type.startsWith('f') ? '-3.14' : '42'}\n${name}`;
    }
    return name;
}

defineInstruction('nop', {
    kind: 'simple',
    opcode: 0x01,
    pop: [],
    push: [],
    summary: 'Do nothing.',
});
defineInstruction('drop', {
    kind: 'drop',
    opcode: 0x1a,
    summary: 'Pop and discard one operand stack value.',
});
defineInstruction('select', {
    kind: 'select',
    opcode: 0x1b,
    summary: 'Select one of two same-typed values using an i32 condition.',
    syntax: 'select',
});
defineInstruction('global.get', {
    kind: 'global.get',
    opcode: 0x23,
    summary: 'Push the value of an imported persistent global.',
    syntax: 'global.get $result|$u0..$u15|$f0..$f15',
});
defineInstruction('global.set', {
    kind: 'global.set',
    opcode: 0x24,
    summary: 'Pop a value and write it to an imported persistent global.',
    syntax: 'global.set $result|$u0..$u15|$f0..$f15',
});
defineInstruction('call', {
    kind: 'call',
    opcode: 0x10,
    summary: 'Call a built-in imported helper function.',
    syntax: 'call $print_i64|$print_i32|$print_f64',
});

defineLoad('i32.load', 0x28, 'i32', 4, 'Load four bytes from memory as i32.');
defineLoad('i64.load', 0x29, 'i64', 8, 'Load eight bytes from memory as i64.');
defineLoad('f32.load', 0x2a, 'f32', 4, 'Load four bytes from memory as f32.');
defineLoad('f64.load', 0x2b, 'f64', 8, 'Load eight bytes from memory as f64.');
defineLoad('i32.load8_s', 0x2c, 'i32', 1, 'Load one byte and sign-extend to i32.');
defineLoad('i32.load8_u', 0x2d, 'i32', 1, 'Load one byte and zero-extend to i32.');
defineLoad('i32.load16_s', 0x2e, 'i32', 2, 'Load two bytes and sign-extend to i32.');
defineLoad('i32.load16_u', 0x2f, 'i32', 2, 'Load two bytes and zero-extend to i32.');
defineLoad('i64.load8_s', 0x30, 'i64', 1, 'Load one byte and sign-extend to i64.');
defineLoad('i64.load8_u', 0x31, 'i64', 1, 'Load one byte and zero-extend to i64.');
defineLoad('i64.load16_s', 0x32, 'i64', 2, 'Load two bytes and sign-extend to i64.');
defineLoad('i64.load16_u', 0x33, 'i64', 2, 'Load two bytes and zero-extend to i64.');
defineLoad('i64.load32_s', 0x34, 'i64', 4, 'Load four bytes and sign-extend to i64.');
defineLoad('i64.load32_u', 0x35, 'i64', 4, 'Load four bytes and zero-extend to i64.');
defineStore('i32.store', 0x36, 'i32', 4, 'Store an i32 to memory.');
defineStore('i64.store', 0x37, 'i64', 8, 'Store an i64 to memory.');
defineStore('f32.store', 0x38, 'f32', 4, 'Store an f32 to memory.');
defineStore('f64.store', 0x39, 'f64', 8, 'Store an f64 to memory.');
defineStore('i32.store8', 0x3a, 'i32', 1, 'Store the low byte of an i32 to memory.');
defineStore('i32.store16', 0x3b, 'i32', 2, 'Store the low two bytes of an i32 to memory.');
defineStore('i64.store8', 0x3c, 'i64', 1, 'Store the low byte of an i64 to memory.');
defineStore('i64.store16', 0x3d, 'i64', 2, 'Store the low two bytes of an i64 to memory.');
defineStore('i64.store32', 0x3e, 'i64', 4, 'Store the low four bytes of an i64 to memory.');
defineInstruction('memory.size', {
    kind: 'memory.zero',
    opcode: 0x3f,
    push: ['i32'],
    summary: 'Push the current memory size in WebAssembly pages.',
});
defineInstruction('memory.grow', {
    kind: 'memory.zero',
    opcode: 0x40,
    pop: ['i32'],
    push: ['i32'],
    summary: 'Grow memory by a page count and push the previous page count or -1.',
});

defineConst('i32.const', 0x41, 'i32');
defineConst('i64.const', 0x42, 'i64');
defineConst('f32.const', 0x43, 'f32');
defineConst('f64.const', 0x44, 'f64');

defineSimple('i32.eqz', 0x45, ['i32'], ['i32'], 'Compare an i32 with zero.');
const i32CompareOps = [
    ['i32.eq', 0x46], ['i32.ne', 0x47], ['i32.lt_s', 0x48], ['i32.lt_u', 0x49],
    ['i32.gt_s', 0x4a], ['i32.gt_u', 0x4b], ['i32.le_s', 0x4c], ['i32.le_u', 0x4d],
    ['i32.ge_s', 0x4e], ['i32.ge_u', 0x4f],
];
for (const [name, opcode] of i32CompareOps) defineSimple(name, opcode, ['i32', 'i32'], ['i32'], 'Compare two i32 values and push an i32 boolean.');
defineSimple('i64.eqz', 0x50, ['i64'], ['i32'], 'Compare an i64 with zero.');
const i64CompareOps = [
    ['i64.eq', 0x51], ['i64.ne', 0x52], ['i64.lt_s', 0x53], ['i64.lt_u', 0x54],
    ['i64.gt_s', 0x55], ['i64.gt_u', 0x56], ['i64.le_s', 0x57], ['i64.le_u', 0x58],
    ['i64.ge_s', 0x59], ['i64.ge_u', 0x5a],
];
for (const [name, opcode] of i64CompareOps) defineSimple(name, opcode, ['i64', 'i64'], ['i32'], 'Compare two i64 values and push an i32 boolean.');
const f32CompareOps = [
    ['f32.eq', 0x5b], ['f32.ne', 0x5c], ['f32.lt', 0x5d], ['f32.gt', 0x5e], ['f32.le', 0x5f], ['f32.ge', 0x60],
];
for (const [name, opcode] of f32CompareOps) defineSimple(name, opcode, ['f32', 'f32'], ['i32'], 'Compare two f32 values and push an i32 boolean.');
const f64CompareOps = [
    ['f64.eq', 0x61], ['f64.ne', 0x62], ['f64.lt', 0x63], ['f64.gt', 0x64], ['f64.le', 0x65], ['f64.ge', 0x66],
];
for (const [name, opcode] of f64CompareOps) defineSimple(name, opcode, ['f64', 'f64'], ['i32'], 'Compare two f64 values and push an i32 boolean.');

const i32UnaryOps = [['i32.clz', 0x67], ['i32.ctz', 0x68], ['i32.popcnt', 0x69]];
for (const [name, opcode] of i32UnaryOps) defineSimple(name, opcode, ['i32'], ['i32'], 'Apply a unary i32 numeric instruction.');
const i32BinaryOps = [
    ['i32.add', 0x6a], ['i32.sub', 0x6b], ['i32.mul', 0x6c], ['i32.div_s', 0x6d],
    ['i32.div_u', 0x6e], ['i32.rem_s', 0x6f], ['i32.rem_u', 0x70], ['i32.and', 0x71],
    ['i32.or', 0x72], ['i32.xor', 0x73], ['i32.shl', 0x74], ['i32.shr_s', 0x75],
    ['i32.shr_u', 0x76], ['i32.rotl', 0x77], ['i32.rotr', 0x78],
];
for (const [name, opcode] of i32BinaryOps) defineSimple(name, opcode, ['i32', 'i32'], ['i32'], 'Apply a binary i32 numeric instruction.');
const i64UnaryOps = [['i64.clz', 0x79], ['i64.ctz', 0x7a], ['i64.popcnt', 0x7b]];
for (const [name, opcode] of i64UnaryOps) defineSimple(name, opcode, ['i64'], ['i64'], 'Apply a unary i64 numeric instruction.');
const i64BinaryOps = [
    ['i64.add', 0x7c], ['i64.sub', 0x7d], ['i64.mul', 0x7e], ['i64.div_s', 0x7f],
    ['i64.div_u', 0x80], ['i64.rem_s', 0x81], ['i64.rem_u', 0x82], ['i64.and', 0x83],
    ['i64.or', 0x84], ['i64.xor', 0x85], ['i64.shl', 0x86], ['i64.shr_s', 0x87],
    ['i64.shr_u', 0x88], ['i64.rotl', 0x89], ['i64.rotr', 0x8a],
];
for (const [name, opcode] of i64BinaryOps) defineSimple(name, opcode, ['i64', 'i64'], ['i64'], 'Apply a binary i64 numeric instruction.');

const f32UnaryOps = [
    ['f32.abs', 0x8b], ['f32.neg', 0x8c], ['f32.ceil', 0x8d], ['f32.floor', 0x8e],
    ['f32.trunc', 0x8f], ['f32.nearest', 0x90], ['f32.sqrt', 0x91],
];
for (const [name, opcode] of f32UnaryOps) defineSimple(name, opcode, ['f32'], ['f32'], 'Apply a unary f32 numeric instruction.');
const f32BinaryOps = [
    ['f32.add', 0x92], ['f32.sub', 0x93], ['f32.mul', 0x94], ['f32.div', 0x95],
    ['f32.min', 0x96], ['f32.max', 0x97], ['f32.copysign', 0x98],
];
for (const [name, opcode] of f32BinaryOps) defineSimple(name, opcode, ['f32', 'f32'], ['f32'], 'Apply a binary f32 numeric instruction.');
const f64UnaryOps = [
    ['f64.abs', 0x99], ['f64.neg', 0x9a], ['f64.ceil', 0x9b], ['f64.floor', 0x9c],
    ['f64.trunc', 0x9d], ['f64.nearest', 0x9e], ['f64.sqrt', 0x9f],
];
for (const [name, opcode] of f64UnaryOps) defineSimple(name, opcode, ['f64'], ['f64'], 'Apply a unary f64 numeric instruction.');
const f64BinaryOps = [
    ['f64.add', 0xa0], ['f64.sub', 0xa1], ['f64.mul', 0xa2], ['f64.div', 0xa3],
    ['f64.min', 0xa4], ['f64.max', 0xa5], ['f64.copysign', 0xa6],
];
for (const [name, opcode] of f64BinaryOps) defineSimple(name, opcode, ['f64', 'f64'], ['f64'], 'Apply a binary f64 numeric instruction.');

const conversionOps = [
    ['i32.wrap_i64', 0xa7, ['i64'], ['i32']],
    ['i32.trunc_f32_s', 0xa8, ['f32'], ['i32']],
    ['i32.trunc_f32_u', 0xa9, ['f32'], ['i32']],
    ['i32.trunc_f64_s', 0xaa, ['f64'], ['i32']],
    ['i32.trunc_f64_u', 0xab, ['f64'], ['i32']],
    ['i64.extend_i32_s', 0xac, ['i32'], ['i64']],
    ['i64.extend_i32_u', 0xad, ['i32'], ['i64']],
    ['i64.trunc_f32_s', 0xae, ['f32'], ['i64']],
    ['i64.trunc_f32_u', 0xaf, ['f32'], ['i64']],
    ['i64.trunc_f64_s', 0xb0, ['f64'], ['i64']],
    ['i64.trunc_f64_u', 0xb1, ['f64'], ['i64']],
    ['f32.convert_i32_s', 0xb2, ['i32'], ['f32']],
    ['f32.convert_i32_u', 0xb3, ['i32'], ['f32']],
    ['f32.convert_i64_s', 0xb4, ['i64'], ['f32']],
    ['f32.convert_i64_u', 0xb5, ['i64'], ['f32']],
    ['f32.demote_f64', 0xb6, ['f64'], ['f32']],
    ['f64.convert_i32_s', 0xb7, ['i32'], ['f64']],
    ['f64.convert_i32_u', 0xb8, ['i32'], ['f64']],
    ['f64.convert_i64_s', 0xb9, ['i64'], ['f64']],
    ['f64.convert_i64_u', 0xba, ['i64'], ['f64']],
    ['f64.promote_f32', 0xbb, ['f32'], ['f64']],
    ['i32.reinterpret_f32', 0xbc, ['f32'], ['i32']],
    ['i64.reinterpret_f64', 0xbd, ['f64'], ['i64']],
    ['f32.reinterpret_i32', 0xbe, ['i32'], ['f32']],
    ['f64.reinterpret_i64', 0xbf, ['i64'], ['f64']],
    ['i32.extend8_s', 0xc0, ['i32'], ['i32']],
    ['i32.extend16_s', 0xc1, ['i32'], ['i32']],
    ['i64.extend8_s', 0xc2, ['i64'], ['i64']],
    ['i64.extend16_s', 0xc3, ['i64'], ['i64']],
    ['i64.extend32_s', 0xc4, ['i64'], ['i64']],
];
for (const [name, opcode, pop, push] of conversionOps) {
    defineSimple(name, opcode, pop, push, 'Convert, reinterpret, or sign-extend a numeric value.');
}

function createState() {
    return {
        result: new WebAssembly.Global({ value: 'i64', mutable: true }, 0n),
        u: Array.from({ length: 16 }, () => new WebAssembly.Global({ value: 'i64', mutable: true }, 0n)),
        f: Array.from({ length: 16 }, () => new WebAssembly.Global({ value: 'f64', mutable: true }, 0)),
        memory: new WebAssembly.Memory({ initial: 1 }),
        out: '',
    };
}

let state = createState();
let bodyLines = [];
let blockLines = [];
let inBlock = false;
let lastSource = '';
let lastWasm = '';
let serial = 1;

function resetState() {
    state = createState();
}

function prompt() {
    process.stdout.write(`wasm${inBlock ? '|' : '>'} `);
}

function printHelp() {
    console.log('WebAssembly REPL. Type flat WebAssembly text instructions and inspect persistent state.');
    console.log('Compiler: Node.js WebAssembly engine');
    console.log('');
    console.log('Commands:');
    console.log('  :help              show this help');
    console.log('  :help <topic>      show built-in help for a topic or instruction');
    console.log('  :topics            list built-in help topics');
    console.log('  :instructions      list supported WebAssembly instructions');
    console.log('  :state             print persistent REPL state');
    console.log('  :reset             reset persistent REPL state');
    console.log('  :scratch           print scratch memory size and first bytes');
    console.log('  :defs              print the current instruction block');
    console.log('  :def               start a multi-line instruction block');
    console.log('  :end               commit the current instruction block');
    console.log('  :body              print accumulated WebAssembly instructions');
    console.log('  :clear             clear the accumulated WebAssembly body');
    console.log('  :source            print the last generated .wat file path');
    console.log('  :quit              exit');
    console.log('');
    console.log('Help topics:');
    console.log('  Add ? after a topic or instruction, for example stack? or i64.add?.');
    console.log('');
    console.log('Startup flags for the public command:');
    console.log('  --debugger         launch this REPL under LLDB, or GDB if LLDB is unavailable');
    console.log('  --debugger=<name>  use lldb, lldb-gui, gdb, gdb-tui, cgdb, or pwnbg');
    console.log('  --lldb / --gdb     launch under LLDB or GDB explicitly');
    console.log('');
    console.log('Execution model:');
    console.log('  Each accepted line is appended to one generated WebAssembly function body.');
    console.log('  The whole body is compiled and executed after each accepted line.');
    console.log('  Input uses flat instruction form, for example: i64.const 40');
    console.log('  The top remaining i32/i64 stack value updates $result; f32/f64 updates $f0.');
    console.log('  Persistent globals: $result, $u0..$u15, $f0..$f15.');
    console.log('  Imported memory offset 0 is scratch; offset 4096 is the out buffer.');
    console.log('');
    console.log('Safety:');
    console.log('  WebAssembly traps are caught, but infinite loops and large memory growth can still hang or consume resources.');
}

function printTopicList() {
    console.log('Built-in help topics for wasm-repl:');
    for (const topic of topics) {
        console.log(`  ${topic.topic.padEnd(14)} ${topic.summary}`);
        if (topic.aliases) {
            console.log(`                 aliases/forms: ${topic.aliases}`);
        }
    }
    console.log('');
    console.log('Type <topic>? for details, for example state?.');
}

function printInstructionList() {
    console.log('WebAssembly instructions supported by wasm-repl:');
    for (const spec of [...instructions.values()].sort((a, b) => a.name.localeCompare(b.name))) {
        console.log(`  ${spec.name.padEnd(20)} ${spec.summary}`);
    }
    console.log('');
    console.log('This list is implemented by wasm-repl\'s WebAssembly binary emitter.');
    console.log('Use :help <instruction> for focused help on one instruction.');
}

function printTopicHelp(query) {
    const normalized = query.trim();
    const exactTopic = findExactTopic(normalized);
    if (exactTopic) {
        printHelpRecord(exactTopic.topic, exactTopic);
        return;
    }

    const instruction = instructions.get(normalized);
    if (instruction) {
        printHelpRecord(instruction.name, instruction);
        return;
    }

    const builtin = builtinFunctions.get(normalized);
    if (builtin) {
        printHelpRecord(normalized, {
            summary: builtin.summary,
            syntax: `call ${normalized}`,
            examples: builtin.examples,
            notes: 'Built-in helper imported from the host REPL.',
        });
        return;
    }

    const global = globalsByName.get(normalized);
    if (global) {
        printHelpRecord(normalized, {
            summary: global.summary,
            syntax: `global.get ${normalized}\nglobal.set ${normalized}`,
            examples: `${global.type}.const ${global.type === 'f64' ? '3.14' : '42'}\nglobal.set ${normalized}\nglobal.get ${normalized}`,
            notes: `Imported mutable ${global.type} global.`,
        });
        return;
    }

    const topic = findTopic(normalized);
    if (topic) {
        printHelpRecord(topic.topic, topic);
        return;
    }

    console.log(`No built-in help for '${query}' in wasm-repl.`);
    console.log('Use :topics to list available help topics.');
}

function printHelpRecord(name, record) {
    console.log(`${name} - ${record.summary}`);
    if (record.aliases) {
        console.log(`Aliases/forms: ${record.aliases}`);
    }
    console.log('');
    console.log('Syntax:');
    console.log(record.syntax || name);
    console.log('');
    console.log('Examples:');
    console.log(record.examples || name);
    if (record.notes) {
        console.log('');
        console.log('Notes:');
        console.log(record.notes);
    }
}

function findTopic(query) {
    for (const topic of topics) {
        if (topic.topic.toLowerCase() === query.toLowerCase() || aliasMatches(topic.aliases, query)) {
            return topic;
        }
    }
    return null;
}

function findExactTopic(query) {
    for (const topic of topics) {
        if (topic.topic.toLowerCase() === query.toLowerCase()) {
            return topic;
        }
    }
    return null;
}

function aliasMatches(aliases, query) {
    if (!aliases) return false;
    return aliases.split(/[\s,]+/).some((alias) => alias.toLowerCase() === query.toLowerCase());
}

function printState() {
    const result = unsigned64(state.result.value);
    console.log(`result 0x${hex64(result)} (${result})`);
    for (let i = 0; i < 8; i++) {
        process.stdout.write(`u${String(i).padEnd(2)} 0x${hex64(unsigned64(state.u[i].value))}  `);
        if (i % 4 === 3) process.stdout.write('\n');
    }
    console.log(`f0 ${formatFloat(state.f[0].value)}  f1 ${formatFloat(state.f[1].value)}  f2 ${formatFloat(state.f[2].value)}  f3 ${formatFloat(state.f[3].value)}`);

    const memory = new Uint8Array(state.memory.buffer);
    const scratch = memory.slice(0, 32);
    if (scratch.some((value) => value !== 0)) {
        console.log(`scratch[0..31] ${[...scratch].map((value) => value.toString(16).padStart(2, '0')).join(' ')}`);
    }

    const memoryOut = readNullTerminated(memory, outOffset, outSize);
    const output = memoryOut || state.out;
    if (output.length > 0) {
        console.log('out:');
        process.stdout.write(output);
        if (!output.endsWith('\n')) process.stdout.write('\n');
    }
}

function printScratch() {
    const memory = new Uint8Array(state.memory.buffer);
    console.log(`scratch size: ${scratchSize} bytes`);
    console.log(`scratch[0..31] ${[...memory.slice(0, 32)].map((value) => value.toString(16).padStart(2, '0')).join(' ')}`);
}

function printBody() {
    if (bodyLines.length === 0) {
        console.log('(empty body)');
        return;
    }
    process.stdout.write(bodyLines.join('\n'));
    process.stdout.write('\n');
}

function printDefs() {
    if (!inBlock || blockLines.length === 0) {
        console.log('(no current instruction block)');
        return;
    }
    process.stdout.write(blockLines.join('\n'));
    process.stdout.write('\n');
}

function runCandidate(lines) {
    let compiled;
    try {
        compiled = compileBody(lines);
    } catch (error) {
        if (error instanceof ReplCompileError) {
            console.error(`wasm validation failed: ${error.message}`);
            return false;
        }
        throw error;
    }

    writeArtifacts(compiled, lines);

    let module;
    try {
        module = new WebAssembly.Module(compiled.bytes);
    } catch (error) {
        console.error(`WebAssembly compilation failed: ${error.message}`);
        return false;
    }

    const snapshot = snapshotState();
    try {
        clearOut();
        const instance = new WebAssembly.Instance(module, importsForState());
        instance.exports.repl_entry();
    } catch (error) {
        restoreState(snapshot);
        console.error(`WebAssembly trap: ${error.message}`);
        return false;
    }

    return true;
}

function compileBody(lines) {
    const context = {
        bytes: [],
        stack: [],
    };

    for (const line of lines) {
        const tokens = tokenize(line);
        for (let index = 0; index < tokens.length;) {
            index = compileInstruction(tokens, index, context);
        }
    }

    const stackBeforeEpilogue = [...context.stack];
    emitEpilogue(context);
    context.bytes.push(0x0b);
    return {
        bytes: buildModule(context.bytes),
        stack: stackBeforeEpilogue,
    };
}

function compileInstruction(tokens, index, context) {
    const name = tokens[index++];
    const spec = instructions.get(name);
    if (!spec) {
        throw new ReplCompileError(`unknown instruction '${name}'`);
    }

    switch (spec.kind) {
        case 'simple':
            popExpected(context, spec.pop || [], name);
            context.bytes.push(spec.opcode);
            pushTypes(context, spec.push || []);
            return index;
        case 'drop':
            popAny(context, name);
            context.bytes.push(spec.opcode);
            return index;
        case 'select': {
            popExpected(context, ['i32'], name);
            const rhs = popAny(context, name);
            const lhs = popAny(context, name);
            if (lhs !== rhs) {
                throw new ReplCompileError(`${name} requires two same-typed values before the condition, got ${lhs} and ${rhs}`);
            }
            context.bytes.push(spec.opcode);
            context.stack.push(lhs);
            return index;
        }
        case 'const': {
            if (index >= tokens.length) {
                throw new ReplCompileError(`${name} requires a value`);
            }
            const value = tokens[index++];
            context.bytes.push(spec.opcode);
            if (spec.type === 'i32') context.bytes.push(...encodeS32(parseI32(value, name)));
            else if (spec.type === 'i64') context.bytes.push(...encodeS64(parseI64(value, name)));
            else if (spec.type === 'f32') context.bytes.push(...encodeF32(parseFloatLiteral(value, name)));
            else if (spec.type === 'f64') context.bytes.push(...encodeF64(parseFloatLiteral(value, name)));
            context.stack.push(spec.type);
            return index;
        }
        case 'global.get': {
            const operand = requiredOperand(tokens, index, name);
            index++;
            const global = resolveGlobal(operand, name);
            context.bytes.push(spec.opcode, ...encodeU32(global.index));
            context.stack.push(global.type);
            return index;
        }
        case 'global.set': {
            const operand = requiredOperand(tokens, index, name);
            index++;
            const global = resolveGlobal(operand, name);
            popExpected(context, [global.type], name);
            context.bytes.push(spec.opcode, ...encodeU32(global.index));
            return index;
        }
        case 'load': {
            const memarg = parseMemarg(tokens, index, spec.alignBytes);
            index = memarg.next;
            popExpected(context, ['i32'], name);
            context.bytes.push(spec.opcode, ...encodeU32(memarg.align), ...encodeU32(memarg.offset));
            context.stack.push(spec.resultType);
            return index;
        }
        case 'store': {
            const memarg = parseMemarg(tokens, index, spec.alignBytes);
            index = memarg.next;
            popExpected(context, [spec.valueType, 'i32'], name);
            context.bytes.push(spec.opcode, ...encodeU32(memarg.align), ...encodeU32(memarg.offset));
            return index;
        }
        case 'memory.zero':
            popExpected(context, spec.pop || [], name);
            context.bytes.push(spec.opcode, 0x00);
            pushTypes(context, spec.push || []);
            return index;
        case 'call': {
            const operand = requiredOperand(tokens, index, name);
            index++;
            const fn = builtinFunctions.get(operand);
            if (!fn) {
                throw new ReplCompileError(`${name} only supports built-in functions: ${[...builtinFunctions.keys()].join(', ')}`);
            }
            popExpected(context, [...fn.params].reverse(), name);
            context.bytes.push(spec.opcode, ...encodeU32(fn.index));
            pushTypes(context, fn.results);
            return index;
        }
        default:
            throw new ReplCompileError(`instruction '${name}' is not implemented`);
    }
}

function emitEpilogue(context) {
    if (context.stack.length === 0) {
        return;
    }

    const top = context.stack.pop();
    if (top === 'i64') {
        context.bytes.push(0x24, ...encodeU32(globalsByName.get('$result').index));
    } else if (top === 'i32') {
        context.bytes.push(0xad, 0x24, ...encodeU32(globalsByName.get('$result').index));
    } else if (top === 'f64') {
        context.bytes.push(0x24, ...encodeU32(globalsByName.get('$f0').index));
    } else if (top === 'f32') {
        context.bytes.push(0xbb, 0x24, ...encodeU32(globalsByName.get('$f0').index));
    }

    while (context.stack.length > 0) {
        context.stack.pop();
        context.bytes.push(0x1a);
    }
}

function popExpected(context, expectedTopFirst, instructionName) {
    for (const expected of expectedTopFirst) {
        const actual = popAny(context, instructionName);
        if (actual !== expected) {
            throw new ReplCompileError(`${instructionName} expected ${expected} on top of the stack, got ${actual}`);
        }
    }
}

function popAny(context, instructionName) {
    const actual = context.stack.pop();
    if (!actual) {
        throw new ReplCompileError(`${instructionName} needs more operands`);
    }
    return actual;
}

function pushTypes(context, types) {
    for (const type of types) {
        context.stack.push(type);
    }
}

function requiredOperand(tokens, index, instructionName) {
    if (index >= tokens.length) {
        throw new ReplCompileError(`${instructionName} requires an operand`);
    }
    return tokens[index];
}

function resolveGlobal(name, instructionName) {
    const global = globalsByName.get(name);
    if (!global) {
        throw new ReplCompileError(`${instructionName} unknown global '${name}'`);
    }
    return global;
}

function parseMemarg(tokens, start, defaultAlignBytes) {
    let offset = 0;
    let alignBytes = defaultAlignBytes;
    let index = start;

    while (index < tokens.length) {
        const match = tokens[index].match(/^(offset|align)=(.+)$/);
        if (!match) break;
        const value = parseU32(match[2], tokens[index]);
        if (match[1] === 'offset') {
            offset = value;
        } else {
            alignBytes = value;
        }
        index++;
    }

    return {
        align: alignBytesToExponent(alignBytes),
        offset,
        next: index,
    };
}

function alignBytesToExponent(bytes) {
    if (bytes <= 0 || (bytes & (bytes - 1)) !== 0) {
        throw new ReplCompileError(`align must be a power-of-two byte count, got ${bytes}`);
    }
    return Math.log2(bytes);
}

function tokenize(line) {
    const withoutComments = stripWasmComment(line);
    const normalized = withoutComments.replace(/[()]/g, ' ').trim();
    if (normalized.length === 0) return [];
    return normalized.split(/\s+/);
}

function stripWasmComment(line) {
    const blockStripped = line.replace(/\(;.*?;\)/g, '');
    const commentStarts = [';;', '//']
        .map((marker) => blockStripped.indexOf(marker))
        .filter((index) => index >= 0);
    if (commentStarts.length === 0) return blockStripped;
    return blockStripped.slice(0, Math.min(...commentStarts));
}

function parseI32(text, context) {
    const value = parseBigInteger(text, context);
    const min = -(1n << 31n);
    const signedMax = (1n << 31n) - 1n;
    const unsignedMax = (1n << 32n) - 1n;
    if (value >= min && value <= signedMax) return Number(value);
    if (value >= 0n && value <= unsignedMax) return Number(BigInt.asIntN(32, value));
    throw new ReplCompileError(`${context} i32 literal out of range: ${text}`);
}

function parseI64(text, context) {
    const value = parseBigInteger(text, context);
    const min = -(1n << 63n);
    const signedMax = (1n << 63n) - 1n;
    const unsignedMax = (1n << 64n) - 1n;
    if (value >= min && value <= signedMax) return value;
    if (value >= 0n && value <= unsignedMax) return BigInt.asIntN(64, value);
    throw new ReplCompileError(`${context} i64 literal out of range: ${text}`);
}

function parseU32(text, context) {
    const value = parseBigInteger(text, context);
    const max = (1n << 32n) - 1n;
    if (value < 0n || value > max) {
        throw new ReplCompileError(`${context} u32 literal out of range: ${text}`);
    }
    return Number(value);
}

function parseBigInteger(text, context) {
    const normalized = text.replace(/_/g, '');
    const sign = normalized.startsWith('-') ? -1n : 1n;
    const unsigned = normalized.replace(/^[+-]/, '');
    try {
        if (/^0x[0-9a-f]+$/i.test(unsigned)) {
            return sign * BigInt(unsigned);
        }
        if (/^\d+$/.test(unsigned)) {
            return sign * BigInt(unsigned);
        }
    } catch {
        // Fall through to the common error below.
    }
    throw new ReplCompileError(`${context} invalid integer literal: ${text}`);
}

function parseFloatLiteral(text, context) {
    const normalized = text.replace(/_/g, '').toLowerCase();
    if (normalized === 'inf' || normalized === '+inf') return Infinity;
    if (normalized === '-inf') return -Infinity;
    if (normalized === 'nan' || normalized === '+nan' || normalized === '-nan') return NaN;
    const value = Number(normalized);
    if (!Number.isNaN(value) || /^[-+]?nan$/.test(normalized)) return value;
    throw new ReplCompileError(`${context} invalid float literal: ${text}`);
}

function encodeU32(value) {
    const bytes = [];
    let current = value >>> 0;
    do {
        let byte = current & 0x7f;
        current >>>= 7;
        if (current !== 0) byte |= 0x80;
        bytes.push(byte);
    } while (current !== 0);
    return bytes;
}

function encodeS32(value) {
    return encodeS64(BigInt(value));
}

function encodeS64(value) {
    const bytes = [];
    let current = BigInt(value);
    for (;;) {
        let byte = Number(current & 0x7fn);
        current >>= 7n;
        const signBitSet = (byte & 0x40) !== 0;
        const done = (current === 0n && !signBitSet) || (current === -1n && signBitSet);
        if (!done) byte |= 0x80;
        bytes.push(byte);
        if (done) break;
    }
    return bytes;
}

function encodeF32(value) {
    const buffer = new ArrayBuffer(4);
    new DataView(buffer).setFloat32(0, value, true);
    return [...new Uint8Array(buffer)];
}

function encodeF64(value) {
    const buffer = new ArrayBuffer(8);
    new DataView(buffer).setFloat64(0, value, true);
    return [...new Uint8Array(buffer)];
}

function encodeName(name) {
    const bytes = Buffer.from(name, 'utf8');
    return [...encodeU32(bytes.length), ...bytes];
}

function buildModule(functionBodyBytes) {
    const bytes = [
        0x00, 0x61, 0x73, 0x6d,
        0x01, 0x00, 0x00, 0x00,
    ];

    bytes.push(...section(1, typeSection()));
    bytes.push(...section(2, importSection()));
    bytes.push(...section(3, [...encodeU32(1), ...encodeU32(0)]));
    bytes.push(...section(7, exportSection()));

    const body = [0x00, ...functionBodyBytes];
    const codePayload = [...encodeU32(1), ...encodeU32(body.length), ...body];
    bytes.push(...section(10, codePayload));
    return Uint8Array.from(bytes);
}

function typeSection() {
    const typeEntries = [
        funcType([], []),
        funcType(['i64'], []),
        funcType(['i32'], []),
        funcType(['f64'], []),
        funcType([], ['i64']),
    ];
    return [...encodeU32(typeEntries.length), ...typeEntries.flat()];
}

function funcType(params, results) {
    return [
        0x60,
        ...encodeU32(params.length),
        ...params.map((type) => wasmType[type]),
        ...encodeU32(results.length),
        ...results.map((type) => wasmType[type]),
    ];
}

function importSection() {
    const imports = [];
    for (const fn of builtinFunctions.values()) {
        imports.push([...encodeName('repl'), ...encodeName(fn.importName), 0x00, ...encodeU32(fn.typeIndex)]);
    }
    imports.push([...encodeName('repl'), ...encodeName('memory'), 0x02, 0x00, ...encodeU32(1)]);
    for (const global of globalSpecs) {
        imports.push([...encodeName('repl'), ...encodeName(global.importName), 0x03, wasmType[global.type], 0x01]);
    }
    return [...encodeU32(imports.length), ...imports.flat()];
}

function exportSection() {
    return [
        ...encodeU32(1),
        ...encodeName('repl_entry'),
        0x00,
        ...encodeU32(mainFunctionIndex),
    ];
}

function section(id, payload) {
    return [id, ...encodeU32(payload.length), ...payload];
}

function importsForState() {
    const imports = {
        print_i64(value) {
            appendOut(`${unsigned64(value)}\n`);
        },
        print_i32(value) {
            appendOut(`${value >>> 0}\n`);
        },
        print_f64(value) {
            appendOut(`${formatFloat(value)}\n`);
        },
        host_time_ms() {
            return BigInt(Date.now());
        },
        memory: state.memory,
        result: state.result,
    };
    for (let i = 0; i < 16; i++) imports[`u${i}`] = state.u[i];
    for (let i = 0; i < 16; i++) imports[`f${i}`] = state.f[i];
    return { repl: imports };
}

function appendOut(text) {
    state.out += text;
    const memory = new Uint8Array(state.memory.buffer);
    const encoder = new TextEncoder();
    const encoded = encoder.encode(state.out);
    const len = Math.min(encoded.length, outSize - 1);
    memory.fill(0, outOffset, outOffset + outSize);
    memory.set(encoded.slice(0, len), outOffset);
}

function clearOut() {
    state.out = '';
    const memory = new Uint8Array(state.memory.buffer);
    if (memory.length >= outOffset + outSize) {
        memory[outOffset] = 0;
    }
}

function snapshotState() {
    return {
        result: state.result.value,
        u: state.u.map((global) => global.value),
        f: state.f.map((global) => global.value),
        memory: new Uint8Array(state.memory.buffer).slice(),
        out: state.out,
    };
}

function restoreState(snapshot) {
    state.result.value = snapshot.result;
    for (let i = 0; i < 16; i++) state.u[i].value = snapshot.u[i];
    for (let i = 0; i < 16; i++) state.f[i].value = snapshot.f[i];
    new Uint8Array(state.memory.buffer).set(snapshot.memory);
    state.out = snapshot.out;
}

function writeArtifacts(compiled, lines) {
    fs.mkdirSync(buildDir, { recursive: true });
    const base = `wasm-${process.pid}-${serial++}`;
    const watPath = path.join(buildDir, `${base}.wat`);
    const wasmPath = path.join(buildDir, `${base}.wasm`);
    fs.writeFileSync(watPath, generateWat(lines, compiled.stack, wasmPath));
    fs.writeFileSync(wasmPath, Buffer.from(compiled.bytes));
    lastSource = watPath;
    lastWasm = wasmPath;
}

function generateWat(lines, stack, wasmPath) {
    const out = [];
    out.push(';; generated by wasm-repl');
    out.push('(module');
    out.push('  (import "repl" "print_i64" (func $print_i64 (param i64)))');
    out.push('  (import "repl" "print_i32" (func $print_i32 (param i32)))');
    out.push('  (import "repl" "print_f64" (func $print_f64 (param f64)))');
    out.push('  (import "repl" "host_time_ms" (func $host_time_ms (result i64)))');
    out.push('  (import "repl" "memory" (memory 1))');
    for (const global of globalSpecs) {
        out.push(`  (import "repl" "${global.importName}" (global ${global.name} (mut ${global.type})))`);
    }
    out.push('  (func $repl_entry');
    if (lines.length > 0) {
        out.push('    ;; accumulated REPL body');
        for (const line of lines) {
            out.push(`    ${line}`);
        }
    }
    const epilogue = epilogueWat(stack);
    if (epilogue.length > 0) {
        out.push('    ;; generated REPL epilogue');
        for (const line of epilogue) out.push(`    ${line}`);
    }
    out.push('  )');
    out.push('  (export "repl_entry" (func $repl_entry))');
    out.push(')');
    out.push(`;; binary: ${wasmPath}`);
    return `${out.join('\n')}\n`;
}

function epilogueWat(stack) {
    const working = [...stack];
    if (working.length === 0) return [];
    const lines = [];
    const top = working.pop();
    if (top === 'i64') {
        lines.push('global.set $result');
    } else if (top === 'i32') {
        lines.push('i64.extend_i32_u');
        lines.push('global.set $result');
    } else if (top === 'f64') {
        lines.push('global.set $f0');
    } else if (top === 'f32') {
        lines.push('f64.promote_f32');
        lines.push('global.set $f0');
    }
    while (working.length > 0) {
        working.pop();
        lines.push('drop');
    }
    return lines;
}

function unsigned64(value) {
    return BigInt.asUintN(64, BigInt(value));
}

function hex64(value) {
    return unsigned64(value).toString(16).padStart(16, '0');
}

function formatFloat(value) {
    if (Number.isNaN(value)) return 'nan';
    if (value === Infinity) return 'inf';
    if (value === -Infinity) return '-inf';
    return Number(value)
        .toPrecision(6)
        .replace(/(\.\d*?[1-9])0+(e.*)?$/i, '$1$2')
        .replace(/\.0+(e.*)?$/i, '$1');
}

function readNullTerminated(memory, offset, maxLength) {
    let end = offset;
    const limit = Math.min(memory.length, offset + maxLength);
    while (end < limit && memory[end] !== 0) end++;
    if (end === offset) return '';
    return new TextDecoder('utf8').decode(memory.slice(offset, end));
}

function extractQuestionQuery(line) {
    const trimmed = line.trim();
    if (!trimmed.endsWith('?') || trimmed.startsWith(':')) return null;
    const withoutQuestion = trimmed.slice(0, -1).trim();
    const match = withoutQuestion.match(/^([$\w.]+)/);
    return match ? match[1] : null;
}

function handleLine(rawLine, rl) {
    const codeLine = stripWasmComment(rawLine).trimEnd();
    const line = codeLine.trim();

    if (line.length === 0) {
        if (inBlock) blockLines.push('');
        return;
    }

    if (line === ':quit' || line === ':q') {
        rl.close();
        return;
    }

    let helpArg = null;
    if (line.startsWith(':help') && (line.length === 5 || /\s/.test(line[5]))) {
        helpArg = line.slice(5).trim();
    } else if (line.startsWith(':h') && (line.length === 2 || /\s/.test(line[2]))) {
        helpArg = line.slice(2).trim();
    }
    if (helpArg !== null) {
        if (helpArg.length === 0) printHelp();
        else if (helpArg === 'instructions' || helpArg === 'inst') printInstructionList();
        else printTopicHelp(helpArg);
        return;
    }

    if (line === ':topics' || line === ':topic') {
        printTopicList();
        return;
    }

    if (line === ':instructions' || line === ':inst' || line === ':i') {
        printInstructionList();
        return;
    }

    if (line === ':state' || line === ':s') {
        printState();
        return;
    }

    if (line === ':reset') {
        resetState();
        console.log('state reset');
        printState();
        return;
    }

    if (line === ':scratch') {
        printScratch();
        return;
    }

    if (line === ':defs') {
        printDefs();
        return;
    }

    if (line === ':body') {
        printBody();
        return;
    }

    if (line === ':source' || line === ':src') {
        console.log(lastSource || '(no generated source yet)');
        return;
    }

    if (line === ':wasm') {
        console.log(lastWasm || '(no generated wasm yet)');
        return;
    }

    if (line === ':clear') {
        bodyLines = [];
        blockLines = [];
        inBlock = false;
        console.log('definitions and WebAssembly body cleared');
        return;
    }

    if (line === ':def') {
        blockLines = [];
        inBlock = true;
        console.log('instruction block started; finish with :end');
        return;
    }

    if (line === ':end' || line === '.end') {
        if (!inBlock) {
            console.log('not in an instruction block');
            return;
        }
        const candidate = [...bodyLines, ...blockLines];
        if (runCandidate(candidate)) {
            bodyLines = candidate;
            blockLines = [];
            inBlock = false;
            console.log('instruction block committed');
            printState();
        }
        return;
    }

    const questionQuery = extractQuestionQuery(line);
    if (questionQuery) {
        printTopicHelp(questionQuery);
        return;
    }

    if (inBlock) {
        blockLines.push(codeLine);
        return;
    }

    const candidate = [...bodyLines, codeLine];
    if (runCandidate(candidate)) {
        bodyLines = candidate;
        printState();
    }
}

printHelp();
const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    terminal: false,
});

prompt();
rl.on('line', (line) => {
    handleLine(line, rl);
    if (!rl.closed) prompt();
});
rl.on('close', () => {
    process.stdout.write('\n');
});
