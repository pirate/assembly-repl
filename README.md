# assembly-repl 🧪

A small family of low-level REPLs for learning assembly and LLVM IR. The main `assembly-repl`
executes CPU assembly directly, but the package also provides `c-repl`, `cpp-repl`,
`objc-repl`, and `llvmir-repl` as well.

The package ships prebuilt native runners for macOS (`arm64`) and Linux
(`x86_64`, `arm64`); `npm install assembly-repl` picks the right one for your
machine automatically.

Type assembly, run it directly on the CPU, and immediately see the register
state that came back. You can enter single instructions or define normal
assembly routines with labels and indentation, then call them later with `bl`
(arm64) or `call` (x86_64).

This is an educational toy for learning assembly and LLVM IR. It is not a sandbox, emulator,
or production debugger. If you ask it to crash, loop forever, corrupt memory, or
jump into nonsense, it will probably do exactly that. 🔥

## Included REPLs ⚙️

- `assembly-repl`: native assembly REPL
- `c-repl`: C snippet REPL
- `cpp-repl`: C++20 snippet REPL
- `objc-repl`: Objective-C snippet REPL on macOS
- `llvmir-repl`: LLVM IR snippet REPL

## Requirements

- `clang` at runtime (the REPL shells out to it for each line)
- `clang++` at runtime for `cpp-repl`
- Objective-C snippets are supported on macOS, where Foundation and the Apple
  Objective-C runtime are available

## Install 🚀

The npm package bundles prebuilt native runners for `darwin-arm64`, `linux-x64`,
and `linux-arm64`. Installing it does not run `node-gyp`, `make`, or a
native build.

Run without installing globally:

```sh
npx assembly-repl

# or for any of the other repls, e.g. llvmir-repl:
npx --package=assembly-repl llvmir-repl
```

Or install globally:

```sh
npm i -g assembly-repl
assembly-repl
c-repl
cpp-repl
objc-repl
llvmir-repl
```

The native runners are prebuilt, but `clang` is still required at runtime because
the REPLs shell out to the compiler for the code you type.

## Help Lookup

Every REPL prints its `:help` text at startup. You can ask for help again or
look up a specific topic or instruction from the prompt:

```text
:help
:help <topic-or-instruction>
```

You can also add `?` after an instruction or topic:

```text
asm> mov?
asm> ldr?
asm> add x0, x0, #1?
c> state?
cpp> template?
objc> message?
ir> getelementptr?
```

`assembly-repl` uses `:instructions` to list built-in instruction help topics
for the current architecture. `llvmir-repl` uses `:topics` for built-in help
topics and `:instructions` to discover LLVM IR instructions from the installed
LLVM/Clang toolchain.

## `assembly-repl`

- Assembles each executable input with `clang`
- Extracts the generated machine code from the object file
  (Mach-O `__TEXT,__text` on macOS, ELF `.text` on Linux)
- Maps the bytes into executable memory
- Calls the code inside the REPL process
- Persists general-purpose registers between lines
- Persists labels, directives, and routines between executions
- Prints registers and arithmetic flags after each instruction

`assembly-repl` exposes a writable scratch page in a callee-saved register so you
can use it like a tiny heap. The register depends on the architecture:

| arch   | scratch ptr | scratch size | syntax              |
|--------|-------------|--------------|---------------------|
| arm64  | `x19`       | `x20`        | ARM64               |
| x86_64 | `r15`       | `r14`        | Intel, no prefixes  |

Assembly examples below use ARM64 syntax unless the heading explicitly says `x86_64`.

### `assembly-repl`: Quickstart

```bash
npm i -g assembly-repl
assembly-repl

asm> :help
asm> mov x0, #41
x0  0x0000000000000029  ...

asm> add x0, x0, #1
x0  0x000000000000002a  ...

asm> cmp x0, #42
nzcv 0x0000000060000000 [nZCv]
```

At startup, the REPL also prints the selected compiler path, scratch register,
commands, and instruction help syntax.

### `assembly-repl`: Examples

<details><summary><h4><code>assembly-repl</code>: Basics</h4></summary>

Registers persist between lines:

```text
asm> mov x0, #10
asm> mov x1, #32
asm> add x2, x0, x1
```

After the final line, `x2` contains `42`.

You can also inspect flags directly:

```text
asm> cmp x0, #42
nzcv 0x0000000060000000 [nZCv]
```

</details>

<details><summary><h4><code>assembly-repl</code>: Making a System Call</h4></summary>

The raw syscall instruction and registers depend on the OS and architecture.
These examples call `getpid` and leave the pid in the normal return register.

ARM64 macOS:

```asm
movz x16, #20
movk x16, #0x200, lsl #16
svc #0x80
```

ARM64 Linux:

```asm
mov x8, #172
svc #0
```

x86_64 Linux:

```asm
mov rax, 39
syscall
```

</details>

<details><summary><h4><code>assembly-repl</code>: Defining a Reusable Routine</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

Paste this into `assembly-repl`:

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

The result is left in `x0` as `0x54`, decimal `84`.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 To x86_64 Cheat Sheet 🧷</h4></summary>

This section is only for `assembly-repl` on x86_64. The REPL uses Intel syntax
without `%` register prefixes.

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

For a complete x86_64 demo see *`assembly-repl`: x86_64 Linux Syscalls* below,
including a working real-time scheduling switch.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Registers 🧠</h4></summary>

Registers persist between lines:

```text
asm> mov x0, #10
asm> mov x1, #32
asm> add x2, x0, x1
```

After the final line, `x2` contains `42`.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Scratch Memory 🧰</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Flags Explorer 🚩</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Calling Convention Lab 🧠</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Manual Stack Frames 🧱</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Pointer Arithmetic With Live Memory 🧰</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Tiny Virtual Machine 🎛️</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Recursive Assembly 🌀</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Conditional Branches 🛣️</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Self-Contained Function Library 📚</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Instruction Equivalence ⚖️</h4></summary>

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

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 macOS Syscalls 🧬</h4></summary>

On macOS ARM64, a Unix syscall uses this basic convention:

- `x0`, `x1`, `x2`, ... hold arguments
- `x16` holds the syscall number
- Unix syscall numbers are encoded as `0x2000000 | SYS_number`
- `svc #0x80` enters the kernel
- `x0` receives the return value
- on error, carry is set and `x0` contains `errno`

The examples below use `movz` + `movk` to build syscall numbers like
`0x2000005`, because those constants are too large for a single `mov` immediate.

#### open

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

#### mmap

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

#### fork

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

#### exit

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

#### execve

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

</details>

<details><summary><h4><code>assembly-repl</code>: x86_64 Linux Syscalls 🐧</h4></summary>

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

#### Real-time scheduling: SCHED_FIFO

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

</details>

<details><summary><h4><code>assembly-repl</code>: Crash-As-A-Lesson Mode 💥</h4></summary>

This REPL is intentionally unsafe. You can use that to learn why valid memory,
balanced stack changes, and correct return addresses matter.

This may crash the REPL:

```asm
mov x0, #0
ldr x1, [x0]
```

So can this:

```asm
sub sp, sp, #16
```

Those failures are useful when you want to see what bad assembly does to a real
process instead of an emulator.

</details>

### `assembly-repl`: Reference

#### `assembly-repl`: Commands 🕹️

- `:help` shows commands and notes
- `:help <instruction>` shows built-in help for an instruction
- `:instructions` lists instruction help topics for the current architecture
- `:regs` prints the current register context
- `:reset` zeroes registers and restores scratch pointers
- `:scratch` prints the scratch memory address and size
- `:defs` prints persisted labels, directives, and routines
- `:clear` clears persisted labels, directives, and routines
- `:quit` exits

You can also add `?` after an instruction mnemonic to show help without
executing anything:

```text
asm> mov?
asm> ldr?
asm> add x0, x0, #1?
```

Short aliases:

- `:h` for `:help`
- `:inst` or `:i` for `:instructions`
- `:r` for `:regs`
- `:q` for `:quit`

#### `assembly-repl`: Sharp Edges ⚠️

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

## `llvmir-repl`

`llvmir-repl` appends each non-command line to a generated LLVM IR function body,
then recompiles and executes the whole body. It exposes `%state`, whose type is
`%repl_state`, so you can load, store, and compute directly in LLVM IR.

### `llvmir-repl`: Quickstart

```bash
npm i -g assembly-repl
llvmir-repl

ir> :help
ir> %x = add i64 40, 2
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4
ir> store i64 %x, ptr %result
result 0x000000000000002a (42)
```

### `llvmir-repl`: Examples

<details><summary><h4><code>llvmir-repl</code>: Basics</h4></summary>

Do inline integer arithmetic, then store into field 4 of `%repl_state` to update
the printed result:

```text
ir> %x = add i64 40, 2
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4
ir> store i64 %x, ptr %result
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>llvmir-repl</code>: Making a System Call</h4></summary>

This calls the platform C library's `getpid` entry point, avoiding OS-specific raw
syscall numbers in the IR:

```text
ir> :def
ir| declare i32 @getpid()
ir| :end
definition block committed
ir> %pid32 = call i32 @getpid()
ir> %pid = zext i32 %pid32 to i64
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4
ir> store i64 %pid, ptr %result
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>llvmir-repl</code>: Defining a Reusable Function</h4></summary>

```text
ir> :def
ir| define i64 @twice(i64 %x) {
ir| entry:
ir|   %r = mul i64 %x, 2
ir|   ret i64 %r
ir| }
ir| :end
definition block committed
ir> %v = call i64 @twice(i64 21)
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4
ir> store i64 %v, ptr %result
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>llvmir-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
ir> :def
ir| define i64 @calc_add(i64 %a, i64 %b) {
ir| entry:
ir|   %r = add i64 %a, %b
ir|   ret i64 %r
ir| }
ir| define i64 @calc_mul(i64 %a, i64 %b) {
ir| entry:
ir|   %r = mul i64 %a, %b
ir|   ret i64 %r
ir| }
ir| :end
definition block committed
ir> %sum = call i64 @calc_add(i64 7, i64 35)
ir> %product = call i64 @calc_mul(i64 %sum, i64 2)
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4
ir> store i64 %product, ptr %result
result 0x0000000000000054 (84)
```

</details>

### `llvmir-repl`: Reference

Persistent state:

```llvm
%repl_state = type { [16 x i64], [16 x double], [4096 x i8], [4096 x i8], i64 }
```

Field 4 of `%repl_state` updates the printed result.

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:instructions` discovers LLVM IR instructions from the installed LLVM/Clang
  toolchain and prints small generated summaries
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts a persisted definition block
- `:end` commits the current definition block
- `:clear` clears definitions and LLVM IR body
- `:source` prints the last generated IR file path
- `:quit` exits

## `c-repl`

`c-repl` compiles each input as C inside:

```c
void repl_entry(repl_state_t *state) {
    /* your snippet */
}
```

Use it for C expressions, pointer experiments, small helper functions, and
shared-library-level behavior while keeping persistent state between snippets.

### `c-repl`: Quickstart

```bash
npm i -g assembly-repl
c-repl

c> :help
c> U(0) = 40 + 2; state->result = U(0);
result 0x000000000000002a (42)
```

### `c-repl`: Examples

<details><summary><h4><code>c-repl</code>: Basics</h4></summary>

```text
c> U(0) = 41;
c> U(0) += 1; state->result = U(0);
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>c-repl</code>: Making a System Call</h4></summary>

```text
c> #include <unistd.h>
directive persisted
c> state->result = (uint64_t)getpid();
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>c-repl</code>: Defining a Reusable Function</h4></summary>

Top-level function definitions are persisted after the closing brace:

```text
c> static uint64_t twice(uint64_t x) {
c|   return x * 2;
c| }
definition block committed
c> state->result = twice(21);
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>c-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
c> static uint64_t calc_add(uint64_t a, uint64_t b) {
c|   return a + b;
c| }
definition block committed
c> static uint64_t calc_mul(uint64_t a, uint64_t b) {
c|   return a * b;
c| }
definition block committed
c> state->result = calc_mul(calc_add(7, 35), 2);
result 0x0000000000000054 (84)
```

</details>

### `c-repl`: Reference

Persistent state:

```c
state->result        /* uint64_t result value printed after each run */
state->u64[n]        /* 16 persistent integer slots */
state->f64[n]        /* 16 persistent double slots */
state->scratch[n]    /* 4096 bytes of persistent scratch memory */
state->out           /* 4096-byte output buffer used by print(...) */
```

Convenience helpers:

```c
U(n), F(n), SCRATCH(n)
print("value=%llu\n", (unsigned long long)U(0))
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts a persisted definition block
- `:end` commits the current definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until the compiler accepts it. Accepted top-level
definitions are persisted; accepted statements run inside `repl_entry`. Press
Enter on an empty continuation line to force diagnostics.

## `cpp-repl`

`cpp-repl` compiles snippets as C++20 with `clang++`. Use it for templates,
lambdas, overloads, classes, and standard C++ experiments.

### `cpp-repl`: Quickstart

```bash
npm i -g assembly-repl
cpp-repl

cpp> :help
cpp> U(0) = 40 + 2; state->result = U(0);
result 0x000000000000002a (42)
```

### `cpp-repl`: Examples

<details><summary><h4><code>cpp-repl</code>: Basics</h4></summary>

```text
cpp> U(0) = 40 + 2; state->result = U(0);
result 0x000000000000002a (42)
```

Local lambdas work too:

```text
cpp> auto sq = [](uint64_t x) { return x * x; }; state->result = sq(12);
result 0x0000000000000090 (144)
```

</details>

<details><summary><h4><code>cpp-repl</code>: Making a System Call</h4></summary>

```text
cpp> #include <unistd.h>
directive persisted
cpp> state->result = static_cast<uint64_t>(::getpid());
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>cpp-repl</code>: Defining a Reusable Function</h4></summary>

Top-level definitions work the same way:

```text
cpp> template <typename T>
cpp| T triple(T x) {
cpp|   return x * 3;
cpp| }
definition block committed
cpp> state->result = triple<uint64_t>(14);
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>cpp-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
cpp> static uint64_t calc_add(uint64_t a, uint64_t b) {
cpp|   return a + b;
cpp| }
definition block committed
cpp> static uint64_t calc_mul(uint64_t a, uint64_t b) {
cpp|   return a * b;
cpp| }
definition block committed
cpp> auto value = calc_mul(calc_add(7, 35), 2); state->result = value;
result 0x0000000000000054 (84)
```

</details>

### `cpp-repl`: Reference

Persistent state:

```c
state->result        /* uint64_t result value printed after each run */
state->u64[n]        /* 16 persistent integer slots */
state->f64[n]        /* 16 persistent double slots */
state->scratch[n]    /* 4096 bytes of persistent scratch memory */
state->out           /* 4096-byte output buffer used by print(...) */
```

Convenience helpers:

```c
U(n), F(n), SCRATCH(n)
print("value=%llu\n", (unsigned long long)U(0))
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts a persisted definition block
- `:end` commits the current definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until the compiler accepts it. Accepted top-level
definitions are persisted; accepted statements run inside `repl_entry`. Press
Enter on an empty continuation line to force diagnostics.

## `objc-repl`

`objc-repl` compiles Objective-C snippets on macOS with Foundation available.
Use it for message sends, Objective-C classes, ARC behavior, and small Cocoa or
Foundation experiments. Foundation is imported by the generated wrapper.

### `objc-repl`: Quickstart

```bash
npm i -g assembly-repl
objc-repl

objc> :help
objc> U(0) = 40 + 2; state->result = U(0);
result 0x000000000000002a (42)
```

### `objc-repl`: Examples

<details><summary><h4><code>objc-repl</code>: Basics</h4></summary>

```text
objc> U(0) = 40 + 2; state->result = U(0);
result 0x000000000000002a (42)
```

Foundation values work too:

```text
objc> NSString *s = @"hello"; state->result = [s length];
result 0x0000000000000005 (5)
```

</details>

<details><summary><h4><code>objc-repl</code>: Making a System Call</h4></summary>

```text
objc> #include <unistd.h>
directive persisted
objc> state->result = (uint64_t)getpid();
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>objc-repl</code>: Defining a Reusable Class</h4></summary>

You can persist Objective-C classes by entering interface and implementation
blocks:

```text
objc> @interface Counter : NSObject
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b;
objc| @end
definition block committed
objc> @implementation Counter
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b { return a + b; }
objc| @end
definition block committed
objc> Counter *c = [Counter new]; state->result = [c add:40 to:2];
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>objc-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
objc> @interface Calculator : NSObject
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b;
objc| - (uint64_t)multiply:(uint64_t)a by:(uint64_t)b;
objc| @end
definition block committed
objc> @implementation Calculator
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b { return a + b; }
objc| - (uint64_t)multiply:(uint64_t)a by:(uint64_t)b { return a * b; }
objc| @end
definition block committed
objc> Calculator *calc = [Calculator new]; state->result = [calc multiply:[calc add:7 to:35] by:2];
result 0x0000000000000054 (84)
```

</details>

### `objc-repl`: Reference

Persistent state:

```c
state->result        /* uint64_t result value printed after each run */
state->u64[n]        /* 16 persistent integer slots */
state->f64[n]        /* 16 persistent double slots */
state->scratch[n]    /* 4096 bytes of persistent scratch memory */
state->out           /* 4096-byte output buffer used by print(...) */
```

Convenience helpers:

```c
U(n), F(n), SCRATCH(n)
print("value=%llu\n", (unsigned long long)U(0))
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts a persisted definition block
- `:end` commits the current definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until the compiler accepts it. Accepted top-level
definitions are persisted; accepted statements run inside `repl_entry`. Press
Enter on an empty continuation line to force diagnostics.

## Runtime Internals 🛠️

### `assembly-repl`: How It Works

For each executable input, the REPL writes a tiny wrapper assembly file into
`.repl-build/`, like this conceptually:

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
clang -c -arch arm64 .repl-build/line-N.s -o .repl-build/line-N.o
```

The C code extracts the `__TEXT,__text` bytes from that object file, maps them
with `mmap`, flips the mapping to executable with `mprotect`, clears the
instruction cache, and calls the resulting function pointer.

### C, C++, Objective-C, And LLVM IR REPLs

The source-language REPLs share one native runner, `language-repl`. The public
entrypoints (`c-repl`, `cpp-repl`, `objc-repl`, and `llvmir-repl`) are Node
wrappers that choose a language mode and launch that native runner.

Each accepted snippet is written into `.repl-build/`, compiled into a shared
library with `clang` or `clang++`, loaded into the REPL process with `dlopen`,
and called through a common `repl_entry` function. State lives in a persistent
`repl_state_t` struct that is passed to each snippet.

## Debugging With LLDB 🔎

The built-in register and state dumps are usually enough for simple learning,
but LLDB is useful when you intentionally try dangerous code or want to inspect
the native runner process.

The npm entrypoints are Node wrapper scripts. For native debugging, point LLDB at
the native runner directly. From a source checkout after `make` or `pnpm build`:

```sh
lldb -- ./asmrepl
lldb -- ./language-repl --mode c
lldb -- ./language-repl --mode cpp
lldb -- ./language-repl --mode objc
lldb -- ./language-repl --mode llvmir
```

These correspond to:

| public command  | native LLDB target                  |
|-----------------|-------------------------------------|
| `assembly-repl` | `./asmrepl`                         |
| `c-repl`        | `./language-repl --mode c`          |
| `cpp-repl`      | `./language-repl --mode cpp`        |
| `objc-repl`     | `./language-repl --mode objc`       |
| `llvmir-repl`   | `./language-repl --mode llvmir`     |

Inside LLDB:

```text
(lldb) run
(lldb) register read
(lldb) bt
(lldb) disassemble --pc
```

To debug an installed npm package, point LLDB at the selected prebuilt native
runner:

```sh
pkg="$(npm root -g)/assembly-repl"
target="$(node -p '`${process.platform}-${process.arch}`')"

lldb -- "$pkg/prebuilds/$target/assembly-repl"
lldb -- "$pkg/prebuilds/$target/language-repl" --mode c
lldb -- "$pkg/prebuilds/$target/language-repl" --mode cpp
lldb -- "$pkg/prebuilds/$target/language-repl" --mode objc
lldb -- "$pkg/prebuilds/$target/language-repl" --mode llvmir
```

On Linux, use `gdb` or `lldb` if installed; the native runner arguments are the
same.

## Development 🛠️

This section is for working on `assembly-repl` itself. Normal users should only
need the install, runtime requirements, commands, examples, runtime internals,
and debugging notes above.

### Development Requirements

- `make` only if building local native runners from source
- Docker buildx if refreshing all packaged prebuilds with `pnpm build`

### Refresh Packaged Prebuilds

```sh
pnpm build
```

This rebuilds the macOS arm64 native runners locally, rebuilds the Linux x64 and
Linux arm64 native runners with Docker buildx, and vendors all of them into
`prebuilds/`.

### Local Native Build

For a local-only native build:

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
