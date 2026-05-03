# assembly-repl 🧪

A native assembly REPL. The package ships prebuilt binaries for macOS
(`arm64`) and Linux (`x86_64`, `arm64`); `npm install assembly-repl` picks the right
one for your machine automatically.

Type assembly, run it directly on the CPU, and immediately see the register
state that came back. You can enter single instructions or define normal
assembly routines with labels and indentation, then call them later with `bl`
(arm64) or `call` (x86_64).

This is an educational toy for learning assembly. It is not a sandbox, emulator,
or production debugger. If you ask it to crash, loop forever, corrupt memory, or
jump into nonsense, it will probably do exactly that. 🔥

## What It Does ⚙️

- Assembles each executable input with `clang`
- Extracts the generated machine code from the object file
  (Mach-O `__TEXT,__text` on macOS, ELF `.text` on Linux)
- Maps the bytes into executable memory
- Calls the code inside the REPL process
- Persists general-purpose registers between lines
- Persists labels, directives, and routines between executions
- Prints registers and arithmetic flags after each instruction

The REPL exposes a writable scratch page in a callee-saved register so you
can use it like a tiny heap. The register depends on the architecture:

| arch   | scratch ptr | scratch size | syntax         |
|--------|-------------|--------------|----------------|
| arm64  | `x19`       | `x20`        | ARM64 (AT&T)   |
| x86_64 | `r15`       | `r14`        | Intel (no-prefix) |

## Requirements

- `clang` at runtime (the REPL shells out to it for each line)
- `make` only if building from source on a platform without a prebuild

## Install 🚀

The npm package bundles prebuilt binaries for `darwin-arm64`, `linux-x64`,
and `linux-arm64`. Installing it does not run `node-gyp`, `make`, or a
native build.

Run without installing globally:

```sh
npx assembly-repl
```

Or install globally:

```sh
npm i -g assembly-repl
assembly-repl
```

The binary is prebuilt, but `clang` is still required at runtime because the REPL
uses it to assemble the code you type.

## Build From Source 🛠️

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

The remainder of the README uses ARM64 syntax. The same ideas apply to
x86_64 — the equivalents are listed once in the *x86_64 Cheat Sheet*
section near the bottom; everything else is shared.

```text
arm64 native assembly REPL (macOS). Type :help for commands.
scratch: x19 = 0x0000000100abc000, x20 = 4096 bytes
asm> mov x0, #41
x0  0x0000000000000029  ...

asm> add x0, x0, #1
x0  0x000000000000002a  ...

asm> cmp x0, #42
nzcv 0x0000000060000000 [nZCv]
```

## x86_64 Cheat Sheet 🧷

The rest of this README uses ARM64. The mapping for x86_64 is small enough
to live in one place — once you know it, every other example translates
mechanically.

| concept                | arm64                             | x86_64 (Intel syntax)            |
|------------------------|-----------------------------------|----------------------------------|
| immediate move         | `mov x0, #41`                     | `mov rax, 41`                    |
| add                    | `add x0, x0, #1`                  | `add rax, 1`                     |
| compare                | `cmp x0, #42`                     | `cmp rax, 42`                    |
| store / load (scratch) | `str x0, [x19]` / `ldr x1, [x19]` | `mov [r15], rax` / `mov rcx, [r15]` |
| call routine           | `bl square`                       | `call square`                    |
| return                 | `ret`                             | `ret`                            |
| flags shown            | `NZCV`                            | `OSZAPC`                         |
| scratch ptr / size     | `x19` / `x20`                     | `r15` / `r14`                    |

Two short x86_64 examples — register persistence and a routine call:

```text
asm> mov rax, 10
asm> mov rcx, 32
asm> add rax, rcx        # rax = 42

asm> square:
asm|   imul rdi, rdi
asm|   mov rax, rdi
asm|   ret
asm> mov rdi, 12
asm> call square         # rax = 144
```

For a complete x86_64 demo see *Demo: Linux Syscalls* below, including a
working real-time scheduling switch.

## Example: Registers 🧠 (arm64)

Registers persist between lines:

```text
asm> mov x0, #10
asm> mov x1, #32
asm> add x2, x0, x1
```

After the final line, `x2` contains `42`.

## Example: Scratch Memory 🧰 (arm64)

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

## Demo: macOS Syscalls 🧬

On macOS ARM64, a Unix syscall uses this basic convention:

- `x0`, `x1`, `x2`, ... hold arguments
- `x16` holds the syscall number
- Unix syscall numbers are encoded as `0x2000000 | SYS_number`
- `svc #0x80` enters the kernel
- `x0` receives the return value
- on error, carry is set and `x0` contains `errno`

The examples below use `movz` + `movk` to build syscall numbers like
`0x2000005`, because those constants are too large for a single `mov` immediate.

### open

This calls `open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)`. The returned file
descriptor is left in `x0`.

```asm
open_demo:
  adr x0, open_path
  mov x1, #0x601
  mov x2, #420
  movz x16, #5
  movk x16, #0x200, lsl #16
  svc #0x80
  ret

open_path:
  .asciz ".asmrepl-open-demo.txt"

bl open_demo
```

The flags are `O_WRONLY` (`0x1`), `O_CREAT` (`0x200`), and `O_TRUNC` (`0x400`).

### mmap

This calls `mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
-1, 0)`, writes `42` into the returned mapping, and loads it back into `x2`.

```asm
mmap_demo:
  mov x0, #0
  mov x1, #4096
  mov x2, #3
  mov x3, #0x1002
  mov x4, #-1
  mov x5, #0
  movz x16, #197
  movk x16, #0x200, lsl #16
  svc #0x80
  mov x21, x0
  mov x1, #42
  str x1, [x21]
  ldr x2, [x21]
  ret

bl mmap_demo
```

After the call, `x21` contains the mapped address and `x2` contains `42`.

### fork

This calls `fork()`. On Darwin, the parent returns with the child pid in `x0`
and `x1 = 0`; the child returns with `x1 = 1`. The child immediately calls
`exit(0)` so it does not become a second REPL reading from the same terminal.

```asm
fork_demo:
  movz x16, #2
  movk x16, #0x200, lsl #16
  svc #0x80
  cbnz x1, fork_child
  ret

fork_child:
  mov x0, #0
  movz x16, #1
  movk x16, #0x200, lsl #16
  svc #0x80
  ret

bl fork_demo
```

### exit

This terminates the REPL process with exit status `42`.

```asm
exit_demo:
  mov x0, #42
  movz x16, #1
  movk x16, #0x200, lsl #16
  svc #0x80
  ret

bl exit_demo
```

Run this one last. It does exactly what it says.

### execve

This calls `execve("/bin/bash", argv, NULL)` and replaces the REPL process with
Bash. The `argv` array is built in scratch memory at `x19`.

```asm
exec_bash_demo:
  adr x0, bash_path

  adr x3, bash_path
  str x3, [x19]
  adr x3, bash_arg_c
  str x3, [x19, #8]
  adr x3, bash_script
  str x3, [x19, #16]
  str xzr, [x19, #24]

  mov x1, x19
  mov x2, #0
  movz x16, #59
  movk x16, #0x200, lsl #16
  svc #0x80
  ret

bash_path:
  .asciz "/bin/bash"

bash_arg_c:
  .asciz "-c"

bash_script:
  .asciz "echo hello from assembly exec; uname -m"

bl exec_bash_demo
```

Expected output:

```text
hello from assembly exec
arm64
```

Like `exit`, this replaces the REPL process. Run it last.

## Demo: Linux Syscalls 🐧

On Linux x86_64 the syscall convention is:

- `rax` holds the syscall number
- `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` hold the first six arguments
- `syscall` enters the kernel
- `rax` receives the return value (negative `errno` on error)
- `rcx` and `r11` are clobbered (the kernel uses them for return state)

A quick `getpid` looks like this:

```asm
mov rax, 39
syscall
```

After the call, `rax` contains the REPL's pid.

### Real-time scheduling: SCHED_FIFO

Linux lets you switch a process to real-time scheduling with one syscall:
`sched_setscheduler(pid, policy, &param)` (syscall `144`). With `policy =
SCHED_FIFO (1)` and a non-zero priority, the task runs ahead of every normal
`SCHED_OTHER` task on its CPU and is never preempted by them.

This is the same mechanism JACK, PipeWire, and other audio stacks use to keep
their callback threads from being interrupted by the rest of the system.

Sharp edge: a real-time `SCHED_FIFO` task with a tight `while (1)` and no
`sched_yield` can starve normal tasks on its CPU and make the system feel
frozen. Linux's RT bandwidth throttle (see `/proc/sys/kernel/sched_rt_*`)
limits this to ~95% of CPU time per second by default, but it is still rude.
Requires `CAP_SYS_NICE` (or root).

```asm
# struct sched_param has one field: int sched_priority. We write it as a
# 64-bit store at the start of the scratch page; the upper 32 bits land in
# whatever padding the kernel ignores.
mov rax, 50
mov [r15], rax

# sched_setscheduler(pid=0, policy=SCHED_FIFO, &param)
mov rdi, 0
mov rsi, 1
mov rdx, r15
mov rax, 144
syscall
```

`rax` should be `0`. A non-zero negative value (e.g. `-1` = `-EPERM`) means
the process lacked `CAP_SYS_NICE`.

Read it back with `sched_getscheduler(0)` (syscall `145`):

```asm
mov rdi, 0
mov rax, 145
syscall
```

`rax` is now `1`, which is `SCHED_FIFO`. From outside the REPL you can confirm
with `chrt -p <pid>`:

```text
pid 9228's current scheduling policy: SCHED_FIFO
pid 9228's current scheduling priority: 50
```

To go back to normal scheduling, repeat the call with `policy = 0`
(`SCHED_OTHER`) and `priority = 0`.

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
