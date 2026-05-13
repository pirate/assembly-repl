# 🧪 `assembly-repl`, <br/>`llvmir-repl`, `cpp-repl`, `c-repl`, `objc-repl`, `wasm-repl`, `rust-repl`, `zig-repl`, `go-repl`

A small family of low-level REPLs for learning assembly, WebAssembly, LLVM IR,
and compiled systems languages.

Type assembly, run it directly on the CPU, and immediately see the register
state that came back. You can enter single instructions or define normal
assembly routines with labels and indentation, then call them later with `bl`
(arm64) or `call` (x86_64).

This is an educational toy for learning assembly, WebAssembly, LLVM IR, and
compiled snippets. It is not a sandbox, emulator, or production debugger. If you
ask it to crash, loop forever, corrupt memory, or jump into nonsense, it will
probably do exactly that. 🔥

## Included REPLs ⚙️

- `assembly-repl`: native assembly REPL
- `c-repl`: C snippet REPL
- `cpp-repl`: C++20 snippet REPL
- `objc-repl`: Objective-C snippet REPL on macOS
- `llvmir-repl`: LLVM IR snippet REPL
- `wasm-repl`: WebAssembly instruction REPL
- `rust-repl`: Rust snippet REPL
- `zig-repl`: Zig snippet REPL
- `go-repl`: Go snippet REPL

## Requirements

- `clang` at runtime for `assembly-repl`, `c-repl`, `objc-repl`, and
  `llvmir-repl`
- `clang++` at runtime for `cpp-repl`
- `rustc` at runtime for `rust-repl`
- `zig` at runtime for `zig-repl`
- `go` at runtime for `go-repl`
- `wasm-repl` uses Node's built-in WebAssembly runtime and does not require an
  external Wasm toolchain
- Objective-C snippets are supported on macOS, where Foundation and the Apple
  Objective-C runtime are available

The npm package bundles the REPL runners, but it does not install language
runtimes or compilers. If a required compiler is missing, the command exits with
install hints for that dependency.

## Install 🚀

The npm package bundles prebuilt native runners for `darwin-arm64`, `linux-x64`,
and `linux-arm64`. Installing it does not run `node-gyp`, `make`, or a
native build.

Run without installing globally:

```sh
npx assembly-repl  # Run assembly-repl without a global install.

# or for any of the other repls, e.g. llvmir-repl:
npx --package=assembly-repl llvmir-repl  # Run llvmir-repl from the same package.
npx --package=assembly-repl wasm-repl    # Run wasm-repl from the same package.
npx --package=assembly-repl rust-repl    # Run rust-repl from the same package.
npx --package=assembly-repl zig-repl     # Run zig-repl from the same package.
npx --package=assembly-repl go-repl      # Run go-repl from the same package.
```

Or install globally:

```sh
npm i -g assembly-repl  # Install every REPL command globally.
assembly-repl           # Start the native assembly REPL.
c-repl                  # Start the C snippet REPL.
cpp-repl                # Start the C++20 snippet REPL.
objc-repl               # Start the Objective-C snippet REPL.
llvmir-repl             # Start the LLVM IR snippet REPL.
wasm-repl               # Start the WebAssembly instruction REPL.
rust-repl               # Start the Rust snippet REPL.
zig-repl                # Start the Zig snippet REPL.
go-repl                 # Start the Go snippet REPL.
```

The native runners are prebuilt, but the source-language REPLs still shell out to
their compilers for the code you type. `wasm-repl` runs through Node's
WebAssembly engine instead.

## Help Lookup

Every REPL prints its `:help` text at startup. You can ask for help again or
look up a specific topic or instruction from the prompt:

```text
:help                         // Show the help screen for the current REPL.
:help <topic-or-instruction>  // Show focused help for one topic or instruction.
:instructions                 // List all available instructions.
```

You can also add `?` after an instruction or topic:

```text
asm> mov?                 // Show help for the ARM64 or x86_64 move instruction.
asm> ldr?                 // Show help for ARM64 load-register forms.
asm> add x0, x0, #1?      // Show help for the instruction at the start of the line.
c> state?                 // Show help for the persistent C REPL state.
cpp> template?            // Show help for reusable C++ template definitions.
objc> message?            // Show help for Objective-C message sends.
ir> getelementptr?        // Show help for the LLVM IR pointer instruction.
wasm> i64.add?            // Show help for a WebAssembly numeric instruction.
rust> unsafe?             // Show help for unsafe Rust and external calls.
zig> extern?              // Show help for Zig external calls.
go> syscall?              // Show help for Go syscall examples.
```

For C, C++, Objective-C, LLVM IR, Rust, Zig, and Go, top-level definitions can
be pasted directly. `go-repl` also accepts normal `package` and `import`
declarations. `:def ... :end` is only an explicit fallback when you want to force
definition mode.

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
npm i -g assembly-repl  # Install the package globally.
assembly-repl           # Start the assembly REPL.

asm> :help              // Show commands, compiler path, and instruction help syntax.
asm> mov x0, #41        // Put 41 into x0.
x0  0x0000000000000029  ...

asm> add x0, x0, #1     // Add 1 to x0.
x0  0x000000000000002a  ...

asm> cmp x0, #42        // Compare x0 with 42 and update NZCV flags.
nzcv 0x0000000060000000 [nZCv]
```

At startup, the REPL also prints the selected compiler path, scratch register,
commands, and instruction help syntax.

### `assembly-repl`: Examples

<details><summary><h4><code>assembly-repl</code>: Basics</h4></summary>

Registers persist between lines:

```text
asm> mov x0, #10        // Put 10 into x0.
asm> mov x1, #32        // Put 32 into x1.
asm> add x2, x0, x1     // Add x0 and x1, storing 42 in x2.
```

After the final line, `x2` contains `42`.

You can also inspect flags directly:

```text
asm> cmp x0, #42        // Compare x0 with 42 and update NZCV.
nzcv 0x0000000060000000 [nZCv]
```

</details>

<details><summary><h4><code>assembly-repl</code>: Making a System Call</h4></summary>

The raw syscall instruction and registers depend on the OS and architecture.
These examples call `getpid` and leave the pid in the normal return register.

ARM64 macOS:

```asm
movz x16, #20                 // Load the Darwin getpid syscall number low bits.
movk x16, #0x200, lsl #16     // Add the Unix syscall class bits.
svc #0x80                     // Enter the kernel; pid returns in x0.
```

ARM64 Linux:

```asm
mov x8, #172                  // Load the Linux arm64 getpid syscall number.
svc #0                        // Enter the kernel; pid returns in x0.
```

x86_64 Linux:

```asm
mov rax, 39                   // Load the Linux x86_64 getpid syscall number.
syscall                       // Enter the kernel; pid returns in rax.
```

</details>

<details><summary><h4><code>assembly-repl</code>: Defining a Reusable Routine</h4></summary>

Directives at column 0 are persisted immediately. Labels at column 0 start
persistent definition blocks. Indented lines belong to the current block. When
you outdent, the block is committed and future input can call it.

```text
asm> _double:                 // Start a persisted routine named _double.
asm|   add x0, x0, x0         // Double the argument in x0.
asm|   ret                    // Return to the generated REPL wrapper.
asm| mov x0, #21              // Outdent to commit the block, then put 21 in x0.
definition block committed
x0  0x0000000000000015  ...

asm> bl _double               // Call the persisted routine.
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
calc_add:                     // x0 = x0 + x1.
  add x0, x0, x1              // Add the two input registers.
  ret                         // Return with the sum in x0.

calc_mul:                     // x0 = x0 * x1.
  mul x0, x0, x1              // Multiply the two input registers.
  ret                         // Return with the product in x0.

calculator_demo:              // Compute (7 + 35) * 2.
  stp x29, x30, [sp, #-16]!   // Save frame pointer and link register.
  mov x29, sp                 // Establish a frame pointer.
  mov x0, #7                  // First add input.
  mov x1, #35                 // Second add input.
  bl calc_add                 // x0 becomes 42.
  mov x1, #2                  // Set the multiply input.
  bl calc_mul                 // x0 becomes 84.
  ldp x29, x30, [sp], #16     // Restore frame pointer and link register.
  ret                         // Return to the caller.

bl calculator_demo            // Run the calculator demo.
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
asm> mov rax, 10         // Put 10 into rax.
asm> mov rcx, 32         // Put 32 into rcx.
asm> add rax, rcx        // Add rcx into rax, leaving 42 in rax.

asm> square:             // Start a persisted routine named square.
asm|   imul rdi, rdi     // Square the input argument in rdi.
asm|   mov rax, rdi      // Move the return value into rax.
asm|   ret               // Return to the generated REPL wrapper.
asm> mov rdi, 12         // Put the argument 12 in rdi.
asm> call square         // Call square, leaving 144 in rax.
```

For a complete x86_64 demo see *`assembly-repl`: x86_64 Linux Syscalls* below,
including a working real-time scheduling switch.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Registers 🧠</h4></summary>

Registers persist between lines:

```text
asm> mov x0, #10        // Put 10 into x0.
asm> mov x1, #32        // Put 32 into x1.
asm> add x2, x0, x1     // Add x0 and x1, storing 42 in x2.
```

After the final line, `x2` contains `42`.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Scratch Memory 🧰</h4></summary>

`x19` points at a writable scratch page:

```text
asm> mov x0, #123       // Put 123 into x0.
asm> str x0, [x19]      // Store x0 at the start of scratch memory.
asm> ldr x1, [x19]      // Load that scratch value into x1.
```

After the final line, `x1` contains `123`.

You can use offsets too:

```text
asm> mov x0, #7         // Put 7 into x0.
asm> str x0, [x19, #8]  // Store x0 eight bytes into scratch memory.
asm> ldr x2, [x19, #8]  // Load that offset value into x2.
```

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Flags Explorer 🚩</h4></summary>

Use `cmp`, `adds`, and `subs` to watch the `NZCV` flags change.

```asm
mov x0, #-1             // Put -1 into x0.
adds x0, x0, #1         // Add 1 and update NZCV flags.
```

`adds` writes the arithmetic result to `x0` and updates flags. After adding
`-1 + 1`, `x0` is zero and the `Z` flag is set.

```asm
mov x0, #5              // Put 5 into x0.
subs x1, x0, #10        // Subtract 10, store -5 in x1, and update flags.
```

This leaves a negative result in `x1`, so the `N` flag is set.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Calling Convention Lab 🧠</h4></summary>

Apple ARM64 passes the first integer arguments in `x0`, `x1`, `x2`, and so on.
Return values come back in `x0`.

```asm
square:                 // Define a routine that squares x0.
  mul x0, x0, x0        // Multiply x0 by itself.
  ret                   // Return with the result in x0.

mov x0, #12             // Put the argument 12 in x0.
bl square               // Call square.
```

After the call, `x0` contains `144`.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Manual Stack Frames 🧱</h4></summary>

This routine uses a conventional frame pointer and return-address save/restore.

```asm
increment_with_frame:        // Define a routine that increments x0.
  stp x29, x30, [sp, #-16]!  // Save frame pointer and link register.
  mov x29, sp                // Establish a frame pointer.
  add x0, x0, #1             // Increment x0.
  ldp x29, x30, [sp], #16    // Restore frame pointer and link register.
  ret                        // Return with the incremented value.

mov x0, #41                  // Put the argument 41 in x0.
bl increment_with_frame      // Call the routine.
```

Watch `sp`, `x29`, and `x30` in the register dump to see the call machinery.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Pointer Arithmetic With Live Memory 🧰</h4></summary>

`x19` points at a writable scratch page. Use it like a tiny heap.

```asm
mov x0, #10             // Put 10 into x0.
str x0, [x19]           // Store 10 at scratch[0].
mov x0, #20             // Put 20 into x0.
str x0, [x19, #8]       // Store 20 at scratch[8].
ldr x1, [x19]           // Load scratch[0] into x1.
ldr x2, [x19, #8]       // Load scratch[8] into x2.
add x3, x1, x2          // Add both loaded values into x3.
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
run_tiny_vm:            // Interpret opcode/value pairs from scratch memory.
  mov x1, x19           // Point x1 at the scratch bytecode stream.
  mov x0, #0            // Start the accumulator at 0.
vm_loop:                // Begin the interpreter loop.
  ldr x2, [x1], #8      // Load the next opcode and advance the stream.
  cbz x2, vm_done       // Opcode 0 halts.
  ldr x3, [x1], #8      // Load the opcode operand.
  cmp x2, #1            // Check for add-immediate.
  b.eq vm_add           // Branch to add handler.
  cmp x2, #2            // Check for multiply-immediate.
  b.eq vm_mul           // Branch to multiply handler.
  b vm_done             // Unknown opcode halts.
vm_add:                 // Add handler.
  add x0, x0, x3        // Add operand into accumulator.
  b vm_loop             // Continue interpreting.
vm_mul:                 // Multiply handler.
  mul x0, x0, x3        // Multiply accumulator by operand.
  b vm_loop             // Continue interpreting.
vm_done:                // Halt handler.
  ret                   // Return with accumulator in x0.

mov x0, #1              // Write opcode 1: add.
str x0, [x19]           // Store opcode at scratch[0].
mov x0, #7              // Write operand 7.
str x0, [x19, #8]       // Store operand at scratch[8].
mov x0, #1              // Write opcode 1: add.
str x0, [x19, #16]      // Store opcode at scratch[16].
mov x0, #35             // Write operand 35.
str x0, [x19, #24]      // Store operand at scratch[24].
mov x0, #2              // Write opcode 2: multiply.
str x0, [x19, #32]      // Store opcode at scratch[32].
mov x0, #2              // Write operand 2.
str x0, [x19, #40]      // Store operand at scratch[40].
mov x0, #0              // Write opcode 0: halt.
str x0, [x19, #48]      // Store halt opcode at scratch[48].
bl run_tiny_vm          // Run the interpreter.
```

The bytecode computes `(0 + 7 + 35) * 2`, so `x0` ends as `84`.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Recursive Assembly 🌀</h4></summary>

Recursion works as long as you preserve the link register and any values you
need after recursive calls.

```asm
factorial:                    // Define recursive factorial(x0).
  stp x29, x30, [sp, #-32]!   // Save frame pointer and link register.
  mov x29, sp                 // Establish a frame pointer.
  str x0, [sp, #16]           // Save the current n.
  cmp x0, #1                  // Check whether n <= 1.
  b.le factorial_base         // Use the base case for n <= 1.
  sub x0, x0, #1              // Prepare n - 1 for the recursive call.
  bl factorial                // Compute factorial(n - 1).
  ldr x1, [sp, #16]           // Reload n.
  mul x0, x0, x1              // Multiply factorial(n - 1) by n.
  b factorial_done            // Skip the base-case assignment.
factorial_base:               // Base case.
  mov x0, #1                  // Return 1.
factorial_done:               // Shared function epilogue.
  ldp x29, x30, [sp], #32     // Restore frame pointer and link register.
  ret                         // Return with factorial result in x0.

mov x0, #5                    // Put the input 5 in x0.
bl factorial                  // Compute factorial(5).
```

After the call, `x0` contains `120`.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Conditional Branches 🛣️</h4></summary>

Build small control-flow routines and call them with different inputs.

```asm
max:                    // Define max(x0, x1).
  cmp x0, x1            // Compare the two inputs.
  b.ge max_done         // Keep x0 when it is already >= x1.
  mov x0, x1            // Otherwise copy x1 into the return register.
max_done:               // Shared return point.
  ret                   // Return with the larger value in x0.

mov x0, #17             // First input.
mov x1, #42             // Second input.
bl max                  // Compute the larger value.
```

After the call, `x0` contains the larger value.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Self-Contained Function Library 📚</h4></summary>

Use the REPL like a live assembly notebook. Define a few reusable routines, then
compose them interactively.

```asm
add3:                   // Define add3(x0, x1, x2).
  add x0, x0, x1        // Add x1 into x0.
  add x0, x0, x2        // Add x2 into x0.
  ret                   // Return with the sum in x0.

clamp_min:              // Define clamp_min(value=x0, minimum=x1).
  cmp x0, x1            // Compare value with minimum.
  b.ge clamp_min_done   // Keep value if it is already high enough.
  mov x0, x1            // Otherwise return the minimum.
clamp_min_done:         // Shared return point.
  ret                   // Return with the clamped value in x0.

mov x0, #5              // First add input.
mov x1, #10             // Second add input.
mov x2, #20             // Third add input.
bl add3                 // x0 becomes 35.
mov x1, #40             // Set the minimum to 40.
bl clamp_min            // Raise x0 to 40.
```

`add3` produces `35`; `clamp_min` then raises that to `40`.

</details>

<details><summary><h4><code>assembly-repl</code>: ARM64 Instruction Equivalence ⚖️</h4></summary>

Some instructions produce the same register result but differ in side effects.

```asm
mov x0, #41             // Put 41 into x0.
add x0, x0, #1          // Add 1 without updating NZCV.
```

Now reset and try the flag-setting form:

```asm
:reset                  // Reset registers and flags.
mov x0, #41             // Put 41 into x0 again.
adds x0, x0, #1         // Add 1 and update NZCV.
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
open_demo:                         // Define a routine that calls open(...).
  adr x0, open_path                // x0 points at the path string.
  mov x1, #0x601                   // x1 = O_WRONLY | O_CREAT | O_TRUNC.
  mov x2, #420                     // x2 = 0644 file mode.
  movz x16, #5                     // Load SYS_open low bits.
  movk x16, #0x200, lsl #16        // Add Darwin Unix syscall class bits.
  svc #0x80                        // Enter the kernel.
  ret                              // Return with fd or errno in x0.

open_path:                         // Store the file path beside the code.
  .asciz ".asmrepl-open-demo.txt"   // Null-terminated path string.

bl open_demo                       // Call the open demo.
```

The flags are `O_WRONLY` (`0x1`), `O_CREAT` (`0x200`), and `O_TRUNC` (`0x400`).

#### mmap

This calls `mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
-1, 0)`, writes `42` into the returned mapping, and loads it back into `x2`.

```asm
mmap_demo:                    // Define a routine that calls mmap(...).
  mov x0, #0                  // addr = NULL.
  mov x1, #4096               // length = 4096.
  mov x2, #3                  // prot = PROT_READ | PROT_WRITE.
  mov x3, #0x1002             // flags = MAP_PRIVATE | MAP_ANON.
  mov x4, #-1                 // fd = -1.
  mov x5, #0                  // offset = 0.
  movz x16, #197              // Load SYS_mmap low bits.
  movk x16, #0x200, lsl #16   // Add Darwin Unix syscall class bits.
  svc #0x80                   // Enter the kernel.
  mov x21, x0                 // Save the mapped address.
  mov x1, #42                 // Prepare a test value.
  str x1, [x21]               // Store 42 into the mapping.
  ldr x2, [x21]               // Load the value back into x2.
  ret                         // Return to the REPL wrapper.

bl mmap_demo                  // Call the mmap demo.
```

After the call, `x21` contains the mapped address and `x2` contains `42`.

#### fork

This calls `fork()`. On Darwin, the parent returns with the child pid in `x0`
and `x1 = 0`; the child returns with `x1 = 1`. The child immediately calls
`exit(0)` so it does not become a second REPL reading from the same terminal.

```asm
fork_demo:                    // Define a routine that calls fork().
  movz x16, #2                // Load SYS_fork low bits.
  movk x16, #0x200, lsl #16   // Add Darwin Unix syscall class bits.
  svc #0x80                   // Enter the kernel.
  cbnz x1, fork_child         // Child returns with x1 = 1.
  ret                         // Parent returns to the REPL.

fork_child:                   // Child-process path.
  mov x0, #0                  // Exit status 0.
  movz x16, #1                // Load SYS_exit low bits.
  movk x16, #0x200, lsl #16   // Add Darwin Unix syscall class bits.
  svc #0x80                   // Terminate the child process.
  ret                         // Unreached unless exit fails.

bl fork_demo                  // Call the fork demo.
```

#### exit

This terminates the REPL process with exit status `42`.

```asm
exit_demo:                    // Define a routine that calls exit(42).
  mov x0, #42                 // Exit status.
  movz x16, #1                // Load SYS_exit low bits.
  movk x16, #0x200, lsl #16   // Add Darwin Unix syscall class bits.
  svc #0x80                   // Terminate the process.
  ret                         // Unreached unless exit fails.

bl exit_demo                  // Call exit_demo; this ends the REPL.
```

Run this one last. It does exactly what it says.

#### execve

This calls `execve("/bin/bash", argv, NULL)` and replaces the REPL process with
Bash. The `argv` array is built in scratch memory at `x19`.

```asm
exec_bash_demo:                         // Define a routine that calls execve(...).
  adr x0, bash_path                     // x0 points at "/bin/bash".

  adr x3, bash_path                     // Load argv[0] address.
  str x3, [x19]                         // Store argv[0] in scratch.
  adr x3, bash_arg_c                    // Load argv[1] address.
  str x3, [x19, #8]                     // Store argv[1] in scratch.
  adr x3, bash_script                   // Load argv[2] address.
  str x3, [x19, #16]                    // Store argv[2] in scratch.
  str xzr, [x19, #24]                   // Store the terminating NULL pointer.

  mov x1, x19                           // x1 points at argv.
  mov x2, #0                            // x2 = envp NULL.
  movz x16, #59                         // Load SYS_execve low bits.
  movk x16, #0x200, lsl #16             // Add Darwin Unix syscall class bits.
  svc #0x80                             // Replace this process with bash.
  ret                                   // Unreached unless execve fails.

bash_path:                              // Store argv[0] string.
  .asciz "/bin/bash"                    // Null-terminated bash path.

bash_arg_c:                             // Store argv[1] string.
  .asciz "-c"                           // Ask bash to run a command string.

bash_script:                            // Store argv[2] string.
  .asciz "echo hello from assembly exec; uname -m"  // Command run by bash.

bl exec_bash_demo                       // Call execve; this replaces the REPL.
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
mov rax, 39                   // Load the Linux x86_64 getpid syscall number.
syscall                       // Enter the kernel; pid returns in rax.
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
mov rax, 50       // Use priority 50 for struct sched_param.sched_priority.
mov [r15], rax    // Store the priority at the start of scratch memory.

mov rdi, 0        // pid = 0 means the current process.
mov rsi, 1        // policy = SCHED_FIFO.
mov rdx, r15      // param points at scratch memory.
mov rax, 144      // syscall number = sched_setscheduler.
syscall           // Enter the kernel.
```

`rax` should be `0`. A non-zero negative value (e.g. `-1` = `-EPERM`) means
the process lacked `CAP_SYS_NICE`.

Read it back with `sched_getscheduler(0)` (syscall `145`):

```asm
mov rdi, 0        // pid = 0 means the current process.
mov rax, 145      // syscall number = sched_getscheduler.
syscall           // Enter the kernel; policy returns in rax.
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
mov x0, #0        // Put a null pointer in x0.
ldr x1, [x0]      // Try to read through it, usually crashing.
```

So can this:

```asm
sub sp, sp, #16   // Move the stack pointer without restoring it.
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
asm> mov?                 // Show help for move instructions.
asm> ldr?                 // Show help for ARM64 loads.
asm> add x0, x0, #1?      // Show help for the instruction at the start of the line.
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
npm i -g assembly-repl  # Install the package globally.
llvmir-repl             # Start the LLVM IR REPL.

ir> :help                                                       ; Show commands and LLVM IR help topics.
ir> %x = add i64 40, 2                                         ; Compute 40 + 2.
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4 ; Point at state->result.
ir> store i64 %x, ptr %result                                  ; Store 42 as the printed result.
result 0x000000000000002a (42)
```

### `llvmir-repl`: Examples

<details><summary><h4><code>llvmir-repl</code>: Basics</h4></summary>

Do inline integer arithmetic, then store into field 4 of `%repl_state` to update
the printed result:

```text
ir> %x = add i64 40, 2                                         ; Compute 40 + 2.
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4 ; Point at state->result.
ir> store i64 %x, ptr %result                                  ; Store 42 as the printed result.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>llvmir-repl</code>: Making a System Call</h4></summary>

This calls the platform C library's `getpid` entry point, avoiding OS-specific raw
syscall numbers in the IR:

```text
ir> declare i32 @getpid()                                      ; Declare the C library getpid function.
definition block committed
ir> %pid32 = call i32 @getpid()                                ; Call getpid and receive an i32 pid.
ir> %pid = zext i32 %pid32 to i64                              ; Widen the pid to i64.
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4 ; Point at state->result.
ir> store i64 %pid, ptr %result                                ; Store the pid as the printed result.
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>llvmir-repl</code>: Defining a Reusable Function</h4></summary>

```text
ir> define i64 @twice(i64 %x) {                                ; Define twice(x).
ir| entry:                                                     ; Start the function entry block.
ir|   %r = mul i64 %x, 2                                       ; Multiply the argument by 2.
ir|   ret i64 %r                                               ; Return the doubled value.
ir| }                                                          ; End the function definition.
definition block committed
ir> %v = call i64 @twice(i64 21)                               ; Call twice(21).
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4 ; Point at state->result.
ir> store i64 %v, ptr %result                                  ; Store 42 as the printed result.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>llvmir-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
ir> define i64 @calc_add(i64 %a, i64 %b) {                     ; Define calc_add(a, b).
ir| entry:                                                     ; Start calc_add's entry block.
ir|   %r = add i64 %a, %b                                      ; Add the two arguments.
ir|   ret i64 %r                                               ; Return the sum.
ir| }                                                          ; End calc_add.
definition block committed
ir> define i64 @calc_mul(i64 %a, i64 %b) {                     ; Define calc_mul(a, b).
ir| entry:                                                     ; Start calc_mul's entry block.
ir|   %r = mul i64 %a, %b                                      ; Multiply the two arguments.
ir|   ret i64 %r                                               ; Return the product.
ir| }                                                          ; End calc_mul.
definition block committed
ir> %sum = call i64 @calc_add(i64 7, i64 35)                   ; Compute 7 + 35.
ir> %product = call i64 @calc_mul(i64 %sum, i64 2)             ; Multiply the sum by 2.
ir> %result = getelementptr %repl_state, ptr %state, i32 0, i32 4 ; Point at state->result.
ir> store i64 %product, ptr %result                            ; Store 84 as the printed result.
result 0x0000000000000054 (84)
```

</details>

### `llvmir-repl`: Reference

Persistent state:

```llvm
%repl_state = type { [16 x i64], [16 x double], [4096 x i8], [4096 x i8], i64 } ; Persistent REPL state layout.
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
- `:def` starts an explicit persisted definition block
- `:end` commits the explicit definition block
- `:clear` clears definitions and LLVM IR body
- `:source` prints the last generated IR file path
- `:quit` exits

Top-level declarations and definitions are persisted automatically. Body
instructions are appended to `repl_entry` and recompiled after each accepted
line.

## `c-repl`

`c-repl` compiles each input as C inside:

```c
void repl_entry(repl_state_t *state) {  /* Generated entry point for one C snippet. */
    /* your snippet */  /* Each c-repl line runs inside this function. */
}  /* Returning hands control back to the REPL. */
```

Use it for C expressions, pointer experiments, small helper functions, and
shared-library-level behavior while keeping persistent state between snippets.

### `c-repl`: Quickstart

```bash
npm i -g assembly-repl  # Install the package globally.
c-repl                  # Start the C REPL.

c> :help                                      // Show commands and C help topics.
c> U(0) = 40 + 2; state->result = U(0);       // Compute 42 and store it as the printed result.
result 0x000000000000002a (42)
```

### `c-repl`: Examples

<details><summary><h4><code>c-repl</code>: Basics</h4></summary>

```text
c> U(0) = 41;                                 // Store 41 in persistent integer slot 0.
c> U(0) += 1; state->result = U(0);           // Increment the slot and publish the result.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>c-repl</code>: Making a System Call</h4></summary>

```text
c> #include <unistd.h>                        // Persist the getpid declaration.
directive persisted
c> state->result = (uint64_t)getpid();        // Call getpid and publish the pid.
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>c-repl</code>: Defining a Reusable Function</h4></summary>

Top-level function definitions are persisted after the closing brace:

```text
c> static uint64_t twice(uint64_t x) {        // Start a persisted helper function.
c|   return x * 2;                            // Return double the input.
c| }                                          // Close and commit the function.
definition block committed
c> state->result = twice(21);                 // Call the helper and publish 42.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>c-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
c> static uint64_t calc_add(uint64_t a, uint64_t b) {  // Start a reusable add helper.
c|   return a + b;                                     // Return a + b.
c| }                                                   // Close and commit calc_add.
definition block committed
c> static uint64_t calc_mul(uint64_t a, uint64_t b) {  // Start a reusable multiply helper.
c|   return a * b;                                     // Return a * b.
c| }                                                   // Close and commit calc_mul.
definition block committed
c> state->result = calc_mul(calc_add(7, 35), 2);       // Compute (7 + 35) * 2.
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
U(n), F(n), SCRATCH(n)                         /* Shorthand for persistent slots and scratch bytes. */
print("value=%llu\n", (unsigned long long)U(0)) /* Append formatted text to state->out. */
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:instructions` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts an explicit persisted definition block
- `:end` commits the explicit definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until the compiler accepts it. Top-level
definitions are persisted automatically; accepted statements run inside
`repl_entry`. Press Enter on an empty continuation line to force diagnostics.

## `cpp-repl`

`cpp-repl` compiles snippets as C++20 with `clang++`. Use it for templates,
lambdas, overloads, classes, and standard C++ experiments.

### `cpp-repl`: Quickstart

```bash
npm i -g assembly-repl  # Install the package globally.
cpp-repl                # Start the C++ REPL.

cpp> :help                                    // Show commands and C++ help topics.
cpp> U(0) = 40 + 2; state->result = U(0);     // Compute 42 and store it as the printed result.
result 0x000000000000002a (42)
```

### `cpp-repl`: Examples

<details><summary><h4><code>cpp-repl</code>: Basics</h4></summary>

```text
cpp> U(0) = 40 + 2; state->result = U(0);     // Compute 42 and publish it.
result 0x000000000000002a (42)
```

Local lambdas work too:

```text
cpp> auto sq = [](uint64_t x) { return x * x; }; state->result = sq(12);  // Define a local lambda and publish 12 squared.
result 0x0000000000000090 (144)
```

</details>

<details><summary><h4><code>cpp-repl</code>: Making a System Call</h4></summary>

```text
cpp> #include <unistd.h>                      // Persist the getpid declaration.
directive persisted
cpp> state->result = static_cast<uint64_t>(::getpid());  // Call getpid and publish the pid.
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>cpp-repl</code>: Defining a Reusable Function</h4></summary>

Top-level definitions work the same way:

```text
cpp> template <typename T>                    // Start a reusable template definition.
cpp| T triple(T x) {                          // Define triple(x).
cpp|   return x * 3;                          // Return three times the input.
cpp| }                                        // Close and commit the template.
definition block committed
cpp> state->result = triple<uint64_t>(14);    // Instantiate the template and publish 42.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>cpp-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
cpp> static uint64_t calc_add(uint64_t a, uint64_t b) {  // Start a reusable add helper.
cpp|   return a + b;                                     // Return a + b.
cpp| }                                                   // Close and commit calc_add.
definition block committed
cpp> static uint64_t calc_mul(uint64_t a, uint64_t b) {  // Start a reusable multiply helper.
cpp|   return a * b;                                     // Return a * b.
cpp| }                                                   // Close and commit calc_mul.
definition block committed
cpp> auto value = calc_mul(calc_add(7, 35), 2); state->result = value;  // Compute (7 + 35) * 2 and publish it.
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
U(n), F(n), SCRATCH(n)                         /* Shorthand for persistent slots and scratch bytes. */
print("value=%llu\n", (unsigned long long)U(0)) /* Append formatted text to state->out. */
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:instructions` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts an explicit persisted definition block
- `:end` commits the explicit definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until the compiler accepts it. Top-level
definitions are persisted automatically; accepted statements run inside
`repl_entry`. Press Enter on an empty continuation line to force diagnostics.

## `objc-repl`

`objc-repl` compiles Objective-C snippets on macOS with Foundation available.
Use it for message sends, Objective-C classes, ARC behavior, and small Cocoa or
Foundation experiments. Foundation is imported by the generated wrapper.

### `objc-repl`: Quickstart

```bash
npm i -g assembly-repl  # Install the package globally.
objc-repl               # Start the Objective-C REPL.

objc> :help                                   // Show commands and Objective-C help topics.
objc> U(0) = 40 + 2; state->result = U(0);    // Compute 42 and store it as the printed result.
result 0x000000000000002a (42)
```

### `objc-repl`: Examples

<details><summary><h4><code>objc-repl</code>: Basics</h4></summary>

```text
objc> U(0) = 40 + 2; state->result = U(0);    // Compute 42 and publish it.
result 0x000000000000002a (42)
```

Foundation values work too:

```text
objc> NSString *s = @"hello"; state->result = [s length];  // Create a string and publish its length.
result 0x0000000000000005 (5)
```

</details>

<details><summary><h4><code>objc-repl</code>: Making a System Call</h4></summary>

```text
objc> #include <unistd.h>                     // Persist the getpid declaration.
directive persisted
objc> state->result = (uint64_t)getpid();     // Call getpid and publish the pid.
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>objc-repl</code>: Defining a Reusable Class</h4></summary>

You can persist Objective-C classes by entering interface and implementation
blocks:

```text
objc> @interface Counter : NSObject           // Start the Counter class interface.
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b;  // Declare an add method.
objc| @end                                    // Close and commit the interface.
definition block committed
objc> @implementation Counter                 // Start the Counter implementation.
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b { return a + b; }  // Implement add.
objc| @end                                    // Close and commit the implementation.
definition block committed
objc> Counter *c = [Counter new]; state->result = [c add:40 to:2];  // Create a Counter and publish 42.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>objc-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
objc> @interface Calculator : NSObject        // Start the Calculator interface.
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b;       // Declare add.
objc| - (uint64_t)multiply:(uint64_t)a by:(uint64_t)b;  // Declare multiply.
objc| @end                                    // Close and commit the interface.
definition block committed
objc> @implementation Calculator              // Start the Calculator implementation.
objc| - (uint64_t)add:(uint64_t)a to:(uint64_t)b { return a + b; }       // Implement add.
objc| - (uint64_t)multiply:(uint64_t)a by:(uint64_t)b { return a * b; }  // Implement multiply.
objc| @end                                    // Close and commit the implementation.
definition block committed
objc> Calculator *calc = [Calculator new]; state->result = [calc multiply:[calc add:7 to:35] by:2];  // Compute (7 + 35) * 2.
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
U(n), F(n), SCRATCH(n)                         /* Shorthand for persistent slots and scratch bytes. */
print("value=%llu\n", (unsigned long long)U(0)) /* Append formatted text to state->out. */
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:instructions` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts an explicit persisted definition block
- `:end` commits the explicit definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until the compiler accepts it. Top-level
definitions are persisted automatically; accepted statements run inside
`repl_entry`. Press Enter on an empty continuation line to force diagnostics.

## `wasm-repl`

`wasm-repl` accepts flat WebAssembly text instructions, emits a small Wasm
module in memory, and executes it through Node's built-in `WebAssembly` API.
It is useful for learning the Wasm operand stack, numeric instructions,
globals, and linear memory without installing `wat2wasm` or a separate runtime.

Each accepted line is appended to a generated `repl_entry` function body. The
whole body is recompiled and executed after each accepted line. If the body
leaves values on the operand stack, the REPL-generated epilogue stores the top
`i32`/`i64` value into `$result`, stores the top `f32`/`f64` value into `$f0`,
and drops older stack values so the generated function validates.

### `wasm-repl`: Quickstart

```bash
npm i -g assembly-repl  # Install the package globally.
wasm-repl               # Start the WebAssembly REPL.

wasm> :help             ;; Show commands and WebAssembly help topics.
wasm> i64.const 40      ;; Push an i64 constant.
result 0x0000000000000028 (40)
wasm> i64.const 2       ;; Push another i64 constant.
result 0x0000000000000002 (2)
wasm> i64.add           ;; Add the two constants from the accumulated body.
result 0x000000000000002a (42)
```

### `wasm-repl`: Examples

<details><summary><h4><code>wasm-repl</code>: Persistent Globals</h4></summary>

Store a value in an imported mutable global and read it back:

```text
wasm> i64.const 42
result 0x000000000000002a (42)
wasm> global.set $u0
result 0x000000000000002a (42)
u0  0x000000000000002a
wasm> global.get $u0
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>wasm-repl</code>: Linear Memory</h4></summary>

Use the imported memory as scratch storage:

```text
wasm> i32.const 0
wasm> i64.const 0xfeedface
wasm> i64.store
scratch[0..31] ce fa ed fe 00 00 00 00 ...
wasm> i32.const 0
wasm> i64.load
result 0x00000000feedface (4277009102)
```

</details>

<details><summary><h4><code>wasm-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
wasm> i64.const 7
wasm> i64.const 35
wasm> i64.add
wasm> i64.const 2
wasm> i64.mul
result 0x0000000000000054 (84)
```

</details>

<details><summary><h4><code>wasm-repl</code>: Host Call Timer</h4></summary>

`$host_time_ms` is an imported host function that returns the host wall clock as
Unix milliseconds. This stores one timestamp in `$u0`, then computes elapsed
time with a second host call:

```text
wasm> call $host_time_ms
wasm> global.set $u0
u0  0x0000019ad5f1d2a0
wasm> call $host_time_ms
wasm> global.get $u0
wasm> i64.sub
result 0x0000000000000037 (55)
```

The exact timestamp and elapsed millisecond value will be different on your
machine.

</details>

### `wasm-repl`: Reference

Persistent imports:

```wat
(import "repl" "host_time_ms" (func $host_time_ms (result i64)))
(import "repl" "memory" (memory 1))
(import "repl" "result" (global $result (mut i64)))
(import "repl" "u0"     (global $u0     (mut i64)))  ;; through $u15
(import "repl" "f0"     (global $f0     (mut f64)))  ;; through $f15
```

Built-in host calls:

```wat
call $print_i64
call $print_i32
call $print_f64
call $host_time_ms
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic-or-instruction>` shows built-in help
- `:topics` lists built-in topic help
- `:instructions` lists supported WebAssembly instructions
- `:state` prints persistent globals, result, scratch, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints the current instruction block
- `:def` starts an optional multi-line instruction block
- `:end` commits the optional instruction block
- `:body` prints accumulated WebAssembly instructions
- `:clear` clears the accumulated WebAssembly body
- `:source` prints the last generated `.wat` file path
- `:wasm` prints the last generated `.wasm` file path
- `:quit` exits

Instruction help:

```text
wasm> i64.add?          ;; Show help for one instruction.
wasm> :instructions     ;; List supported instructions.
wasm> :help memory      ;; Show memory/load/store notes.
```

Each accepted instruction line is appended and run immediately. `:def ... :end`
is only needed when you want to batch several instructions before running them.

## `rust-repl`

`rust-repl` compiles snippets with `rustc` as Rust 2021 shared libraries and
loads them into the REPL process. Each executable snippet runs inside:

```rust
pub unsafe extern "C" fn repl_entry(state: *mut ReplState) {
    let state = unsafe { &mut *state };
    /* your snippet */
}
```

Use it for small Rust experiments, unsafe code, FFI calls, and helper functions
while keeping persistent state between snippets.

### `rust-repl`: Quickstart

```bash
npm i -g assembly-repl  # Install the package globally.
rust-repl               # Start the Rust REPL.

rust> :help                            // Show commands and Rust help topics.
rust> state.result = 40 + 2;           // Compute 42 and store it as the printed result.
result 0x000000000000002a (42)
```

### `rust-repl`: Examples

<details><summary><h4><code>rust-repl</code>: Basics</h4></summary>

```text
rust> state.u64[0] = 41;               // Store 41 in persistent integer slot 0.
rust> state.u64[0] += 1; state.result = state.u64[0];  // Increment and publish the result.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>rust-repl</code>: Making a System Call</h4></summary>

```text
rust> unsafe extern "C" { fn getpid() -> i32; }
definition block committed
rust> state.result = unsafe { getpid() as u64 };
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>rust-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
rust> fn calc_add(a: u64, b: u64) -> u64 { a + b }
definition block committed
rust> fn calc_mul(a: u64, b: u64) -> u64 { a * b }
definition block committed
rust> state.result = calc_mul(calc_add(7, 35), 2);
result 0x0000000000000054 (84)
```

</details>

### `rust-repl`: Reference

Persistent state:

```rust
state.result       /* u64 result value printed after each run */
state.u64[n]       /* 16 persistent integer slots */
state.f64[n]       /* 16 persistent f64 slots */
state.scratch[n]   /* 4096 bytes of persistent scratch memory */
state.out          /* 4096-byte output buffer used by repl_print!(...) */
```

Convenience helper:

```rust
repl_print!(state, "value={}\n", state.u64[0]);  /* Append formatted text to state.out. */
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:instructions` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts an explicit persisted definition block
- `:end` commits the explicit definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until `rustc` accepts it. Top-level definitions
are persisted automatically; accepted statements run inside `repl_entry`. Press
Enter on an empty continuation line to force diagnostics.

## `zig-repl`

`zig-repl` compiles snippets with `zig` as dynamic libraries and loads them into
the REPL process. Each executable snippet runs inside:

```zig
export fn repl_entry(state: *ReplState) callconv(.c) void {
    /* your snippet */
}
```

Use it for Zig expressions, pointer experiments, C ABI calls, and small helper
functions with persistent state between snippets.

### `zig-repl`: Quickstart

```bash
npm i -g assembly-repl  # Install the package globally.
zig-repl                # Start the Zig REPL.

zig> :help                             // Show commands and Zig help topics.
zig> state.result = 40 + 2;            // Compute 42 and store it as the printed result.
result 0x000000000000002a (42)
```

### `zig-repl`: Examples

<details><summary><h4><code>zig-repl</code>: Basics</h4></summary>

```text
zig> state.u[0] = 41;                  // Store 41 in persistent integer slot 0.
zig> state.u[0] += 1; state.result = state.u[0];  // Increment and publish the result.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>zig-repl</code>: Making a System Call</h4></summary>

```text
zig> extern fn getpid() c_int;
definition block committed
zig> state.result = @as(u64, @intCast(getpid()));
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>zig-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
zig> fn calcAdd(a: u64, b: u64) u64 { return a + b; }
definition block committed
zig> fn calcMul(a: u64, b: u64) u64 { return a * b; }
definition block committed
zig> state.result = calcMul(calcAdd(7, 35), 2);
result 0x0000000000000054 (84)
```

</details>

### `zig-repl`: Reference

Persistent state:

```zig
state.result       /* u64 result value printed after each run */
state.u[n]         /* 16 persistent integer slots */
state.f[n]         /* 16 persistent f64 slots */
state.scratch[n]   /* 4096 bytes of persistent scratch memory */
state.out          /* 4096-byte output buffer used by print(...) */
```

Convenience helper:

```zig
print(state, "value={}\n", .{state.u[0]});  /* Append formatted text to state.out. */
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:instructions` lists built-in topic help
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted definitions
- `:def` starts an explicit persisted definition block
- `:end` commits the explicit definition block
- `:clear` clears definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until `zig` accepts it. Top-level definitions are
persisted automatically; accepted statements run inside `repl_entry`. Press
Enter on an empty continuation line to force diagnostics.

## `go-repl`

`go-repl` uses a gore-style build/run loop: each accepted snippet is emitted as
a temporary Go program, built with `go build`, and run as a child process. The
REPL serializes persistent state before and after each run.

Use it for small Go experiments, helper functions, and low-level package calls
without keeping a full source file open. Normal `package main` and `import`
declarations are accepted so snippets can stay close to source-file shape.

### `go-repl`: Quickstart

```bash
npm i -g assembly-repl  # Install the package globally.
go-repl                 # Start the Go REPL.

go> :help                              // Show commands and Go help topics.
go> state.Result = 40 + 2              // Compute 42 and store it as the printed result.
result 0x000000000000002a (42)
```

### `go-repl`: Examples

<details><summary><h4><code>go-repl</code>: Basics</h4></summary>

```text
go> state.U[0] = 41                    // Store 41 in persistent integer slot 0.
go> state.U[0] += 1; state.Result = state.U[0]  // Increment and publish the result.
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>go-repl</code>: Package Imports</h4></summary>

```text
go> package main
go> import "math"
import persisted
go> state.Result = uint64(math.Abs(-42))
result 0x000000000000002a (42)
```

Multi-line import blocks work too:

```text
go> import (
go|     m "math"
go| )
import block persisted
go> state.Result = uint64(m.Abs(-42))
result 0x000000000000002a (42)
```

</details>

<details><summary><h4><code>go-repl</code>: Making a System Call</h4></summary>

```text
go> package main
go> import "syscall"
import persisted
go> pid, _, errno := syscall.RawSyscall(syscall.SYS_GETPID, 0, 0, 0)
go| if errno == 0 { state.Result = uint64(pid) }
result 0x0000000000001234 (4660)
```

The exact process id will be different on your machine.

</details>

<details><summary><h4><code>go-repl</code>: Full Calculator</h4></summary>

This computes:

```text
(7 + 35) * 2 = 84
```

```text
go> func calcAdd(a, b uint64) uint64 { return a + b }
definition block committed
go> func calcMul(a, b uint64) uint64 { return a * b }
definition block committed
go> state.Result = calcMul(calcAdd(7, 35), 2)
result 0x0000000000000054 (84)
```

</details>

### `go-repl`: Reference

Persistent state:

```go
state.Result      /* uint64 result value printed after each run */
state.U[n]        /* 16 persistent integer slots */
state.F[n]        /* 16 persistent float64 slots */
state.Scratch[n]  /* 4096 bytes of persistent scratch memory */
state.Out         /* 4096-byte output buffer used by Print(...) */
```

Convenience helper:

```go
Print(state, "value=%d\n", state.U[0])  /* Append formatted text to state.Out. */
```

Commands:

- `:help` shows commands and execution notes
- `:help <topic>` shows built-in help for a topic
- `:topics` lists built-in topic help
- `:instructions` lists built-in topic help
- `:import <package>` is a shortcut for a Go import declaration
- `:state` prints persistent slots, result, and output
- `:reset` resets persistent state
- `:scratch` prints scratch memory details
- `:defs` prints persisted imports and definitions
- `:def` starts an explicit persisted definition block
- `:end` commits the explicit definition block
- `:clear` clears imports and definitions
- `:source` prints the last generated source file path
- `:quit` exits

Multi-line input is collected until `go build` accepts it. Package declarations
are accepted as file boilerplate and ignored. Import declarations and top-level
definitions are persisted automatically; accepted statements run inside
`replEntry`. Press Enter on an empty continuation line to force diagnostics.

## Runtime Internals 🛠️

### `assembly-repl`: How It Works

For each executable input, the REPL writes a tiny wrapper assembly file into
`.repl-build/`, like this conceptually:

```asm
_asmrepl_entry:                                   // Generated wrapper entry point.
  ; save host registers the C ABI cares about     // Preserve the host process state.
  ; load persisted user registers from reg_context_t // Restore the REPL register state.

  <your instruction here>                         // The instruction or branch you typed.

  ; store user registers and NZCV flags back into reg_context_t // Persist the result.
  ; restore host registers                         // Put the host ABI state back.
  ret                                             // Return to the C runner.

  ; persisted labels/directives/routines live down here // User definitions are appended.
  _some_routine:                                  // Example persisted routine.
    ret                                          // Return to its caller.
```

Then it runs:

```sh
clang -c -arch arm64 .repl-build/line-N.s -o .repl-build/line-N.o  # Assemble one generated snippet.
```

The C code extracts the `__TEXT,__text` bytes from that object file, maps them
with `mmap`, flips the mapping to executable with `mprotect`, clears the
instruction cache, and calls the resulting function pointer.

### Source-Language, LLVM IR, And WebAssembly REPLs

The source-language REPLs share one native runner, `language-repl`. The public
entrypoints (`c-repl`, `cpp-repl`, `objc-repl`, `llvmir-repl`, `rust-repl`,
`zig-repl`, and `go-repl`) are Node wrappers that choose a language mode and
launch that native runner.

For C, C++, Objective-C, LLVM IR, Rust, and Zig, each accepted snippet is written
into `.repl-build/`, compiled into a shared library, loaded into the REPL
process with `dlopen`, and called through a common `repl_entry` function. State
lives in a persistent `repl_state_t` struct that is passed to each snippet.

`go-repl` writes a temporary Go program instead of a shared library. The native
runner serializes the REPL state to a `.bin` file, runs the compiled Go child
process, then reads the state file back after the child exits.

`wasm-repl` is implemented as a Node runner instead of a native runner. It emits
WebAssembly binaries directly, instantiates them with Node's `WebAssembly` API,
and writes the generated `.wat` and `.wasm` artifacts into `.repl-build/`.

## Debugger Flag 🔎

Pass `--debugger` to any REPL command to open a debugger with the right target
selected. With no value, the wrapper tries LLDB first, then GDB. You can also
use `--lldb`, `--gdb`, or choose an explicit debugger:

```sh
assembly-repl --debugger
wasm-repl --debugger=lldb-gui
```

Allowed debugger names:

- `lldb`
- `lldb-gui` 🌈
- `gdb`
- `gdb-tui` 🌈
- `cgdb` 🌈
- `pwnbg` 🌈
- `pwndbg` 🌈

If the selected debugger is missing, the wrapper prints install hints for that
dependency. On macOS, `pwnbg`/`pwndbg` uses `pwndbg-lldb` attach mode.

## Development 🛠️

This section is for working on `assembly-repl` itself. Normal users should only
need the install, runtime requirements, commands, examples, runtime internals,
and debugging notes above.

### Development Requirements

- `make` only if building local native runners from source
- Docker buildx if refreshing all packaged prebuilds with `pnpm build`

### Refresh Packaged Prebuilds

```sh
pnpm build  # Rebuild and vendor packaged native prebuilds.
```

This rebuilds the macOS arm64 native runners locally, rebuilds the Linux x64 and
Linux arm64 native runners with Docker buildx, and vendors all of them into
`prebuilds/`.

### Local Native Build

For a local-only native build:

```sh
make       # Build local native runners.
./asmrepl  # Run the local assembly runner directly.
```

Or:

```sh
make run   # Build and run the local assembly runner.
```

Clean generated files:

```sh
make clean # Remove generated native build outputs.
```

## Further Reading

- https://imtomt.github.io/ymawky/
- https://mariokartwii.com/arm64/
- https://hrishim.github.io/llvl_prog1_book/starting_assembly.html
- https://github.com/pirate/polyglot-agent
