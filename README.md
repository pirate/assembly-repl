# assembly-repl 🧪

An intentionally unsafe native ARM64 assembly REPL for Apple Silicon macOS.

Type assembly, run it directly on the CPU, and immediately see the register
state that came back. You can enter single instructions or define normal
assembly routines with labels and indentation, then call them later with `bl`.

This is an educational toy for learning assembly. It is not a sandbox, emulator,
or production debugger. If you ask it to crash, loop forever, corrupt memory, or
jump into nonsense, it will probably do exactly that. 🔥

## What It Does ⚙️

- Assembles each executable input with `clang`
- Extracts the generated ARM64 machine code from the Mach-O object file
- Maps the bytes into executable memory
- Calls the code inside the REPL process
- Persists general-purpose registers between lines
- Persists labels, directives, and routines between executions
- Prints registers and `NZCV` flags after each instruction

The REPL starts with `x19` pointing at a writable scratch page and `x20`
containing the scratch page size.

## Requirements 🍎

- Apple Silicon Mac
- `clang`
- `make`

This version targets Apple `arm64` Mach-O only.

## Build 🚀

```sh
make
./asmrepl
```

Or:

```sh
make run
```

Clean generated files:

```sh
make clean
```

## Quick Start ✨

```text
arm64 native assembly REPL. Type :help for commands.
scratch: x19 = 0x0000000100abc000, x20 = 4096 bytes
asm> mov x0, #41
x0  0x0000000000000029  ...

asm> add x0, x0, #1
x0  0x000000000000002a  ...

asm> cmp x0, #42
nzcv 0x0000000060000000 [nZCv]
```

## Example: Registers 🧠

Registers persist between lines:

```text
asm> mov x0, #10
asm> mov x1, #32
asm> add x2, x0, x1
```

After the final line, `x2` contains `42`.

## Example: Scratch Memory 🧰

`x19` points at a writable scratch page:

```text
asm> mov x0, #123
asm> str x0, [x19]
asm> ldr x1, [x19]
```

After the final line, `x1` contains `123`.

You can use offsets too:

```text
asm> mov x0, #7
asm> str x0, [x19, #8]
asm> ldr x2, [x19, #8]
```

## Commands 🕹️

- `:help` shows commands and notes
- `:regs` prints the current register context
- `:reset` zeroes registers and restores scratch pointers
- `:scratch` prints the scratch memory address and size
- `:defs` prints persisted labels, directives, and routines
- `:clear` clears persisted labels, directives, and routines
- `:quit` exits

Short aliases:

- `:h` for `:help`
- `:r` for `:regs`
- `:q` for `:quit`

## Example: Live Routines 🧩

Directives at column 0 are persisted immediately. Labels at column 0 start
persistent definition blocks. Indented lines belong to the current block. When
you outdent, the block is committed and future input can call it.

```text
asm> _double:
asm|   add x0, x0, x0
asm|   ret
asm| mov x0, #21
definition block committed
x0  0x0000000000000015  ...

asm> bl _double
x0  0x000000000000002a  ...
```

That is normal assembly shape: label at column 0, body indented, `ret` to return
to the generated REPL wrapper.

## How It Works 🛠️

For each executable input, the REPL writes a tiny wrapper assembly file into
`.asmrepl-build/`, like this conceptually:

```asm
_asmrepl_entry:
  ; save host registers the C ABI cares about
  ; load persisted user registers from reg_context_t

  <your instruction here>

  ; store user registers and NZCV flags back into reg_context_t
  ; restore host registers
  ret

  ; persisted labels/directives/routines live down here
  _some_routine:
    ret
```

Then it runs:

```sh
clang -c -arch arm64 .asmrepl-build/line-N.s -o .asmrepl-build/line-N.o
```

The C code extracts the `__TEXT,__text` bytes from that object file, maps them
with `mmap`, flips the mapping to executable with `mprotect`, clears the
instruction cache, and calls the resulting function pointer.

## Important Sharp Edges ⚠️

This program runs native instructions in the current process.

Things that may crash or hang the REPL:

- Unbalanced changes to `sp`
- Branching away from the generated wrapper
- Calling arbitrary addresses
- Infinite loops
- Invalid loads or stores
- Trap instructions
- Overwriting process memory

That is intentional. The goal is to keep the tool small, direct, and useful for
learning what instructions actually do.

## Debugging With LLDB 🔎

You can run the REPL under LLDB if you want a real debugger around the process:

```sh
lldb ./asmrepl
(lldb) run
```

Once stopped at a crash or breakpoint:

```text
(lldb) register read
(lldb) bt
(lldb) disassemble --pc
```

The built-in register dump is usually enough for simple instruction-level
learning, but LLDB is useful when you intentionally try dangerous instructions.

## Pure Assembly: Addition + Multiplication Calculator ➕✖️

Here is a tiny calculator written live in the REPL. Each operation is a
dedicated callable routine:

- `_calc_add`: adds `x0 + x1`
- `_calc_mul`: multiplies `x0 * x1`
- `_calculator_demo`: calls both routines with `bl`

The example computes:

```text
(7 + 35) * 2 = 84
```

Paste this into the REPL:

```asm
.globl _calc_add
.globl _calc_mul
.globl _calculator_demo
.p2align 2

_calc_add:
  add x0, x0, x1
  ret

_calc_mul:
  mul x0, x0, x1
  ret

_calculator_demo:
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  mov x0, #7
  mov x1, #35
  bl _calc_add
  mov x1, #2
  bl _calc_mul
  ldp x29, x30, [sp], #16
  ret

bl _calculator_demo
```

The final outdented `bl _calculator_demo` commits the `_calculator_demo` block,
executes the call, and leaves the result in `x0`:

```text
x0  0x0000000000000054  ...
```

The important pattern is that each operation follows the same small calling
contract: put inputs in `x0` and `x1`, call the routine with `bl`, and read the
result back from `x0`. The final `x0` value is `0x54`, which is decimal `84`.
