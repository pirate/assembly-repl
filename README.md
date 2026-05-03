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

## Demo: Flags Explorer 🚩

Use `cmp`, `adds`, and `subs` to watch the `NZCV` flags change.

```asm
mov x0, #-1
adds x0, x0, #1
```

`adds` writes the arithmetic result to `x0` and updates flags. After adding
`-1 + 1`, `x0` is zero and the `Z` flag is set.

```asm
mov x0, #5
subs x1, x0, #10
```

This leaves a negative result in `x1`, so the `N` flag is set.

## Demo: Calling Convention Lab 🧠

Apple ARM64 passes the first integer arguments in `x0`, `x1`, `x2`, and so on.
Return values come back in `x0`.

```asm
square:
  mul x0, x0, x0
  ret

mov x0, #12
bl square
```

After the call, `x0` contains `144`.

## Demo: Manual Stack Frames 🧱

This routine uses a conventional frame pointer and return-address save/restore.

```asm
increment_with_frame:
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  add x0, x0, #1
  ldp x29, x30, [sp], #16
  ret

mov x0, #41
bl increment_with_frame
```

Watch `sp`, `x29`, and `x30` in the register dump to see the call machinery.

## Demo: Pointer Arithmetic With Live Memory 🧰

`x19` points at a writable scratch page. Use it like a tiny heap.

```asm
mov x0, #10
str x0, [x19]
mov x0, #20
str x0, [x19, #8]
ldr x1, [x19]
ldr x2, [x19, #8]
add x3, x1, x2
```

After the final line, `x3` contains `30`.

## Demo: Tiny Virtual Machine 🎛️

Store a tiny instruction stream in scratch memory, then interpret it with native
assembly.

This toy bytecode format uses pairs of 64-bit words:

- opcode `1`: add immediate
- opcode `2`: multiply immediate
- opcode `0`: halt

```asm
run_tiny_vm:
  mov x1, x19
  mov x0, #0
vm_loop:
  ldr x2, [x1], #8
  cbz x2, vm_done
  ldr x3, [x1], #8
  cmp x2, #1
  b.eq vm_add
  cmp x2, #2
  b.eq vm_mul
  b vm_done
vm_add:
  add x0, x0, x3
  b vm_loop
vm_mul:
  mul x0, x0, x3
  b vm_loop
vm_done:
  ret

mov x0, #1
str x0, [x19]
mov x0, #7
str x0, [x19, #8]
mov x0, #1
str x0, [x19, #16]
mov x0, #35
str x0, [x19, #24]
mov x0, #2
str x0, [x19, #32]
mov x0, #2
str x0, [x19, #40]
mov x0, #0
str x0, [x19, #48]
bl run_tiny_vm
```

The bytecode computes `(0 + 7 + 35) * 2`, so `x0` ends as `84`.

## Demo: Recursive Assembly 🌀

Recursion works as long as you preserve the link register and any values you
need after recursive calls.

```asm
factorial:
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  str x0, [sp, #16]
  cmp x0, #1
  b.le factorial_base
  sub x0, x0, #1
  bl factorial
  ldr x1, [sp, #16]
  mul x0, x0, x1
  b factorial_done
factorial_base:
  mov x0, #1
factorial_done:
  ldp x29, x30, [sp], #32
  ret

mov x0, #5
bl factorial
```

After the call, `x0` contains `120`.

## Demo: Conditional Branches 🛣️

Build small control-flow routines and call them with different inputs.

```asm
max:
  cmp x0, x1
  b.ge max_done
  mov x0, x1
max_done:
  ret

mov x0, #17
mov x1, #42
bl max
```

After the call, `x0` contains the larger value.

## Demo: Self-Contained Function Library 📚

Use the REPL like a live assembly notebook. Define a few reusable routines, then
compose them interactively.

```asm
add3:
  add x0, x0, x1
  add x0, x0, x2
  ret

clamp_min:
  cmp x0, x1
  b.ge clamp_min_done
  mov x0, x1
clamp_min_done:
  ret

mov x0, #5
mov x1, #10
mov x2, #20
bl add3
mov x1, #40
bl clamp_min
```

`add3` produces `35`; `clamp_min` then raises that to `40`.

## Demo: Instruction Equivalence ⚖️

Some instructions produce the same register result but differ in side effects.

```asm
mov x0, #41
add x0, x0, #1
```

Now reset and try the flag-setting form:

```asm
:reset
mov x0, #41
adds x0, x0, #1
```

Both versions leave `x0` as `42`, but only `adds` updates `NZCV`.

## Demo: Crash-As-A-Lesson Mode 💥

This REPL is intentionally unsafe. You can use that to learn why valid memory,
balanced stack changes, and correct return addresses matter.

This may crash the REPL:

```asm
ldr x0, [xzr]
```

So can this:

```asm
sub sp, sp, #16
```

Those failures are useful when you want to see what bad assembly does to a real
process instead of an emulator.
