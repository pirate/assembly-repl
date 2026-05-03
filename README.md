# assembly-repl 🧪

An intentionally unsafe native ARM64 assembly REPL for Apple Silicon macOS.

Type one line of assembly, run it directly on the CPU, and immediately see the
register state that came back.

This is an educational toy for learning assembly. It is not a sandbox, emulator,
or production debugger. If you ask it to crash, loop forever, corrupt memory, or
jump into nonsense, it will probably do exactly that. 🔥

## What It Does ⚙️

- Assembles each input line with `clang`
- Extracts the generated ARM64 machine code from the Mach-O object file
- Maps the bytes into executable memory
- Calls the code inside the REPL process
- Persists general-purpose registers between lines
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
- `:quit` exits

Short aliases:

- `:h` for `:help`
- `:r` for `:regs`
- `:q` for `:quit`

## How It Works 🛠️

For each input line, the REPL writes a tiny wrapper assembly file into
`.asmrepl-build/`, like this conceptually:

```asm
_asmrepl_entry:
  ; save host registers the C ABI cares about
  ; load persisted user registers from reg_context_t

  <your instruction here>

  ; store user registers and NZCV flags back into reg_context_t
  ; restore host registers
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

## Pure Assembly: Addition-Only Calculator ➕

Here is the smallest version inside the REPL: use `x0` as the running total and
`x1` as the next number to add.

```text
asm> mov x0, #0        ; total = 0
asm> mov x1, #7        ; enter 7
asm> add x0, x0, x1    ; total = total + 7
asm> mov x1, #35       ; enter 35
asm> add x0, x0, x1    ; total = total + 35
```

After the final line, `x0` contains `42`.

The same idea as a standalone pure ARM64 assembly function:

```asm
// add_only_calculator.s
// Apple arm64 calling convention:
//   x0 = pointer to uint64_t values
//   x1 = number of values
// returns:
//   x0 = sum

.text
.globl _add_only_calculator
.p2align 2

_add_only_calculator:
  mov x2, x0        // x2 = values pointer
  mov x3, x1        // x3 = remaining count
  mov x0, #0        // x0 = running total

loop:
  cbz x3, done      // if remaining == 0, return total
  ldr x4, [x2], #8  // load next uint64_t and advance pointer
  add x0, x0, x4    // total += value
  sub x3, x3, #1    // remaining--
  b loop

done:
  ret
```

That is an addition-only calculator in the literal sense: it keeps a running
total, accepts one integer at a time, and the only arithmetic operation it uses
for the result is `add`.
