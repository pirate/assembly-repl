#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <ctype.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)
#  define ASMREPL_APPLE_ARM64 1
#elif defined(__APPLE__) && defined(__x86_64__)
#  define ASMREPL_APPLE_X86_64 1
#elif defined(__linux__) && defined(__aarch64__)
#  define ASMREPL_LINUX_ARM64 1
#elif defined(__linux__) && defined(__x86_64__)
#  define ASMREPL_LINUX_X86_64 1
#else
#  error "asmrepl: unsupported platform; supported: macOS arm64/x86_64, Linux arm64/x86_64"
#endif

#if defined(ASMREPL_APPLE_ARM64) || defined(ASMREPL_LINUX_ARM64)
#  define ASMREPL_ARCH_ARM64 1
#endif
#if defined(ASMREPL_APPLE_X86_64) || defined(ASMREPL_LINUX_X86_64)
#  define ASMREPL_ARCH_X86_64 1
#endif

#ifdef __APPLE__
#  include <mach-o/loader.h>
#  include <libkern/OSCacheControl.h>
#  define ASMREPL_FORMAT_MACHO 1
#  define ENTRY_SYMBOL "_asmrepl_entry"
#else
#  include <elf.h>
#  define ASMREPL_FORMAT_ELF 1
#  define ENTRY_SYMBOL "asmrepl_entry"
#  ifndef MAP_ANON
#    define MAP_ANON MAP_ANONYMOUS
#  endif
#endif

#define BUILD_DIR ".repl-build"
#define SCRATCH_SIZE 4096
#define MAX_INPUT 4096

#if ASMREPL_ARCH_ARM64
#  define REG_COUNT 31
typedef struct {
    uint64_t x[REG_COUNT];
    uint64_t nzcv;
    uint64_t sp;
} reg_context_t;
#elif ASMREPL_ARCH_X86_64
typedef struct {
    /* Offsets are referenced from the JIT wrapper; do not reorder. */
    uint64_t rax;     /* 0   */
    uint64_t rcx;     /* 8   */
    uint64_t rdx;     /* 16  */
    uint64_t rbx;     /* 24  */
    uint64_t rsp;     /* 32  */
    uint64_t rbp;     /* 40  */
    uint64_t rsi;     /* 48  */
    uint64_t rdi;     /* 56  */
    uint64_t r8;      /* 64  */
    uint64_t r9;      /* 72  */
    uint64_t r10;     /* 80  */
    uint64_t r11;     /* 88  */
    uint64_t r12;     /* 96  */
    uint64_t r13;     /* 104 */
    uint64_t r14;     /* 112 */
    uint64_t r15;     /* 120 */
    uint64_t rflags;  /* 128 */
} reg_context_t;
#endif

typedef struct {
    uint8_t *data;
    size_t size;
} code_blob_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} text_buffer_t;

typedef struct {
    const char *name;
    const char *aliases;
    const char *summary;
    const char *syntax;
    const char *examples;
    const char *notes;
} instruction_help_t;

typedef void (*jit_fn_t)(reg_context_t *);

static noreturn void die(const char *message) {
    perror(message);
    exit(1);
}

static void *xmalloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        die("malloc");
    }
    return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr) {
        die("realloc");
    }
    return new_ptr;
}

static void text_buffer_init(text_buffer_t *buf) {
    buf->cap = 4096;
    buf->len = 0;
    buf->data = xmalloc(buf->cap);
    buf->data[0] = '\0';
}

static void text_buffer_clear(text_buffer_t *buf) {
    buf->len = 0;
    if (buf->data) {
        buf->data[0] = '\0';
    }
}

static void text_buffer_append(text_buffer_t *buf, const char *text) {
    size_t text_len = strlen(text);
    if (buf->len + text_len + 1 > buf->cap) {
        while (buf->len + text_len + 1 > buf->cap) {
            buf->cap *= 2;
        }
        buf->data = xrealloc(buf->data, buf->cap);
    }

    memcpy(buf->data + buf->len, text, text_len + 1);
    buf->len += text_len;
}

static void text_buffer_append_line(text_buffer_t *buf, const char *line) {
    text_buffer_append(buf, line);
    text_buffer_append(buf, "\n");
}

static void text_buffer_free(text_buffer_t *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static char *trim(char *line) {
    while (*line == ' ' || *line == '\t' || *line == '\n' || *line == '\r') {
        line++;
    }

    size_t len = strlen(line);
    while (len > 0) {
        char c = line[len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        line[--len] = '\0';
    }

    return line;
}

static void rtrim_in_place(char *line) {
    size_t len = strlen(line);
    while (len > 0) {
        char c = line[len - 1];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        line[--len] = '\0';
    }
}

static void strip_asm_inline_comment(char *line) {
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (char *p = line; *p; p++) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (*p == '\\' && (in_single || in_double)) {
            escaped = true;
            continue;
        }
        if (*p == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (*p == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (!in_single && !in_double && p[0] == '/' && p[1] == '/') {
            *p = '\0';
            rtrim_in_place(line);
            return;
        }
    }
}

static bool starts_indented(const char *line) {
    return line[0] == ' ' || line[0] == '\t';
}

static bool is_directive(const char *line) {
    return line[0] == '.';
}

static bool is_label_line(const char *line) {
    const char *colon = strchr(line, ':');
    if (!colon) {
        return false;
    }

    for (const char *p = line; p < colon; p++) {
        if (*p == ' ' || *p == '\t') {
            return false;
        }
    }

    return colon > line;
}

static bool is_definition_start(const char *line) {
    return !starts_indented(line) && is_label_line(line);
}

static bool resolve_executable_path(const char *name, char *out, size_t out_size) {
    if (strchr(name, '/')) {
        char resolved[MAX_INPUT];
        if (realpath(name, resolved)) {
            snprintf(out, out_size, "%s", resolved);
        } else {
            snprintf(out, out_size, "%s", name);
        }
        return access(out, X_OK) == 0;
    }

    const char *path = getenv("PATH");
    if (!path || *path == '\0') {
        path = "/usr/bin:/bin:/usr/local/bin";
    }

    char *copy = xmalloc(strlen(path) + 1);
    strcpy(copy, path);

    bool found = false;
    for (char *dir = copy; dir;) {
        char *next = strchr(dir, ':');
        if (next) {
            *next++ = '\0';
        }
        if (*dir == '\0') {
            dir = ".";
        }

        char candidate[MAX_INPUT];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (access(candidate, X_OK) == 0) {
            char resolved[MAX_INPUT];
            if (realpath(candidate, resolved)) {
                snprintf(out, out_size, "%s", resolved);
            } else {
                snprintf(out, out_size, "%s", candidate);
            }
            found = true;
            break;
        }

        dir = next;
    }

    free(copy);
    if (!found) {
        snprintf(out, out_size, "%s (not found on PATH)", name);
    }
    return found;
}

static void ensure_build_dir(void) {
    if (mkdir(BUILD_DIR, 0755) != 0 && errno != EEXIST) {
        die("mkdir " BUILD_DIR);
    }
}

static void reset_context(reg_context_t *ctx, void *scratch) {
    memset(ctx, 0, sizeof(*ctx));
#if ASMREPL_ARCH_ARM64
    ctx->x[19] = (uint64_t)(uintptr_t)scratch;
    ctx->x[20] = SCRATCH_SIZE;
#elif ASMREPL_ARCH_X86_64
    ctx->r15 = (uint64_t)(uintptr_t)scratch;
    ctx->r14 = SCRATCH_SIZE;
#endif
}

#if ASMREPL_ARCH_ARM64
static void print_flags_arm(uint64_t nzcv) {
    putchar((nzcv & (1ULL << 31)) ? 'N' : 'n');
    putchar((nzcv & (1ULL << 30)) ? 'Z' : 'z');
    putchar((nzcv & (1ULL << 29)) ? 'C' : 'c');
    putchar((nzcv & (1ULL << 28)) ? 'V' : 'v');
}
#elif ASMREPL_ARCH_X86_64
static void print_flags_x86(uint64_t rflags) {
    /* Print the common arithmetic flags. Bit numbers per Intel SDM. */
    putchar((rflags & (1ULL << 11)) ? 'O' : 'o'); /* overflow */
    putchar((rflags & (1ULL << 7))  ? 'S' : 's'); /* sign */
    putchar((rflags & (1ULL << 6))  ? 'Z' : 'z'); /* zero */
    putchar((rflags & (1ULL << 4))  ? 'A' : 'a'); /* aux carry */
    putchar((rflags & (1ULL << 2))  ? 'P' : 'p'); /* parity */
    putchar((rflags & (1ULL << 0))  ? 'C' : 'c'); /* carry */
}
#endif

static void print_regs(const reg_context_t *ctx) {
#if ASMREPL_ARCH_ARM64
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 4; col++) {
            int reg = row * 4 + col;
            if (reg >= REG_COUNT) {
                break;
            }
            printf("x%-2d 0x%016llx  ", reg, (unsigned long long)ctx->x[reg]);
        }
        putchar('\n');
    }

    printf("sp  0x%016llx  nzcv 0x%016llx [",
           (unsigned long long)ctx->sp,
           (unsigned long long)ctx->nzcv);
    print_flags_arm(ctx->nzcv);
    puts("]");
#elif ASMREPL_ARCH_X86_64
    static const struct { const char *name; size_t off; } gprs[] = {
        {"rax", offsetof(reg_context_t, rax)},
        {"rcx", offsetof(reg_context_t, rcx)},
        {"rdx", offsetof(reg_context_t, rdx)},
        {"rbx", offsetof(reg_context_t, rbx)},
        {"rsp", offsetof(reg_context_t, rsp)},
        {"rbp", offsetof(reg_context_t, rbp)},
        {"rsi", offsetof(reg_context_t, rsi)},
        {"rdi", offsetof(reg_context_t, rdi)},
        {"r8 ", offsetof(reg_context_t, r8)},
        {"r9 ", offsetof(reg_context_t, r9)},
        {"r10", offsetof(reg_context_t, r10)},
        {"r11", offsetof(reg_context_t, r11)},
        {"r12", offsetof(reg_context_t, r12)},
        {"r13", offsetof(reg_context_t, r13)},
        {"r14", offsetof(reg_context_t, r14)},
        {"r15", offsetof(reg_context_t, r15)},
    };
    const uint8_t *base = (const uint8_t *)ctx;
    for (int i = 0; i < 16; i++) {
        uint64_t v;
        memcpy(&v, base + gprs[i].off, sizeof(v));
        printf("%s 0x%016llx  ", gprs[i].name, (unsigned long long)v);
        if ((i % 4) == 3) putchar('\n');
    }
    printf("rflags 0x%016llx [", (unsigned long long)ctx->rflags);
    print_flags_x86(ctx->rflags);
    puts("]");
#endif
}

static const char *architecture_name(void) {
#if ASMREPL_ARCH_ARM64
    return "arm64";
#elif ASMREPL_ARCH_X86_64
    return "x86_64";
#endif
}

static const char *instruction_help_examples(void) {
#if ASMREPL_ARCH_ARM64
    return "mov? or ldr?";
#elif ASMREPL_ARCH_X86_64
    return "mov? or add?";
#endif
}

static const char *instruction_help_full_line_example(void) {
#if ASMREPL_ARCH_ARM64
    return "add x0, x0, #1?";
#elif ASMREPL_ARCH_X86_64
    return "add rax, 1?";
#endif
}

#if ASMREPL_ARCH_ARM64
static const instruction_help_t instruction_help[] = {
    {"adc", "adcs", "Add two registers plus the carry flag.", "adc xd, xn, xm\nadcs xd, xn, xm   ; also updates NZCV", "adc x0, x1, x2\nadcs x0, x0, x3", "Use after adds/subs/adcs/sbcs when building multi-word arithmetic."},
    {"add", "adds", "Add registers or an immediate. adds also updates NZCV flags.", "add xd, xn, xm\nadd xd, xn, #imm\nadds xd, xn, xm\nadds xd, xn, #imm", "add x0, x0, #1\nadd x2, x0, x1\nadds x0, x0, #1", "The immediate form accepts the encodings supported by the assembler, commonly a 12-bit value optionally shifted by 12."},
    {"adr", "adrp", "Form an address relative to the current program counter.", "adr xd, label\nadrp xd, label", "adr x0, message\nadrp x0, message", "adr is page-local. adrp forms a page address and is normally paired with add or a load/store relocation."},
    {"and", "ands", "Bitwise AND. ands also updates NZCV flags.", "and xd, xn, xm\nand xd, xn, #imm\nands xd, xn, xm\nands xd, xn, #imm", "and x0, x0, #0xff\nands xzr, x0, #1", "tst is an alias for ands with xzr as the destination."},
    {"asr", NULL, "Arithmetic right shift, preserving the sign bit.", "asr xd, xn, #shift\nasr xd, xn, xm", "asr x0, x0, #1", "For unsigned values, use lsr."},
    {"b", "b.eq b.ne b.cs b.hs b.cc b.lo b.mi b.pl b.vs b.vc b.hi b.ls b.ge b.lt b.gt b.le", "Branch to a label. Conditional forms branch based on NZCV flags.", "b label\nb.<cond> label", "b done\nb.eq equal_case\nb.lt smaller", "Common conditions include eq/ne, lt/le/gt/ge for signed comparisons, and lo/ls/hi/hs for unsigned comparisons."},
    {"bl", NULL, "Branch with link; calls a routine and stores the return address in x30.", "bl label", "bl square", "The called routine should normally return with ret. In this REPL, calls are not sandboxed."},
    {"blr", NULL, "Branch with link to an address stored in a register.", "blr xn", "blr x9", "The target register must contain executable code. Arbitrary targets can crash the process."},
    {"br", NULL, "Branch to an address stored in a register.", "br xn", "br x9", "Unlike blr, this does not set x30."},
    {"cbz", "cbnz", "Compare a register with zero and branch.", "cbz xt, label\ncbnz xt, label", "cbz x0, was_zero\ncbnz x1, has_value", "The compare does not update NZCV flags."},
    {"cmp", "cmn", "Compare values by subtracting or adding and setting NZCV flags.", "cmp xn, xm\ncmp xn, #imm\ncmn xn, xm\ncmn xn, #imm", "cmp x0, #42\nb.eq matched", "cmp is an alias for subs xzr, ...; cmn is an alias for adds xzr, ...."},
    {"csel", "cset cinc cinv cneg", "Conditional select and common conditional aliases.", "csel xd, xn, xm, cond\ncset xd, cond\ncinc xd, xn, cond\ncinv xd, xn, cond\ncneg xd, xn, cond", "csel x0, x1, x2, eq\ncset x0, ne", "These read the current NZCV flags, usually after cmp, cmn, adds, or subs."},
    {"eor", NULL, "Bitwise exclusive OR.", "eor xd, xn, xm\neor xd, xn, #imm", "eor x0, x0, x1\neor x0, x0, #0xff", "Use eor with the same register twice to produce zero, though mov x0, xzr is clearer."},
    {"ldp", NULL, "Load a pair of registers from memory.", "ldp xt1, xt2, [xn]\nldp xt1, xt2, [xn, #imm]\nldp xt1, xt2, [xn], #imm\nldp xt1, xt2, [xn, #imm]!", "ldp x0, x1, [x19]\nldp x29, x30, [sp], #16", "Pair loads are commonly used for stack frame restore sequences."},
    {"ldr", "ldrb ldrh ldrsw", "Load from memory into a register.", "ldr xt, [xn]\nldr xt, [xn, #imm]\nldr xt, [xn], #imm\nldr xt, [xn, #imm]!\nldrb wt, [xn]\nldrh wt, [xn]\nldrsw xt, [xn]", "ldr x0, [x19]\nldr x1, [x19, #8]\nldrb w0, [x19]", "In this REPL, x19 starts as a writable scratch page pointer."},
    {"lsl", NULL, "Logical left shift.", "lsl xd, xn, #shift\nlsl xd, xn, xm", "lsl x0, x0, #3", "Left shifting by n multiplies unsigned integers by 2^n when no useful bits are shifted out."},
    {"lsr", NULL, "Logical right shift, filling high bits with zero.", "lsr xd, xn, #shift\nlsr xd, xn, xm", "lsr x0, x0, #1", "For signed values, use asr."},
    {"madd", "msub", "Multiply and add or subtract in one instruction.", "madd xd, xn, xm, xa\nmsub xd, xn, xm, xa", "madd x0, x1, x2, x3   ; x0 = x3 + x1*x2\nmsub x0, x1, x2, x3", "mul is an alias for madd with xzr as the addend."},
    {"mov", NULL, "Move a value between registers or load an encodable immediate.", "mov xd, xn\nmov xd, #imm\nmov wd, wzr\nmov xd, xzr", "mov x0, #41\nmov x1, x0\nmov x2, xzr", "Large constants may need movz/movk. The assembler expands some mov forms to the real encodable instruction."},
    {"movk", "movz movn", "Build or modify a 16-bit chunk of a register immediate.", "movz xd, #imm16[, lsl #shift]\nmovk xd, #imm16[, lsl #shift]\nmovn xd, #imm16[, lsl #shift]", "movz x0, #0xbeef\nmovk x0, #0xfeed, lsl #16", "Use movz to start from zero, movn to start from all ones, and movk to keep other chunks unchanged."},
    {"mrs", "msr", "Read or write selected system registers.", "mrs xt, system_register\nmsr system_register, xt", "mrs x0, nzcv\nmsr nzcv, x0", "Only some system registers are accessible from user mode."},
    {"mul", NULL, "Multiply two registers and keep the low result bits.", "mul xd, xn, xm", "mul x0, x1, x2", "For multiply-add, use madd. For full-width multiply results, look at umulh/smulh."},
    {"mvn", NULL, "Bitwise NOT of a register.", "mvn xd, xm", "mvn x0, x0", "mvn is an alias for orn with xzr."},
    {"neg", "negs", "Negate a value. negs also updates NZCV flags.", "neg xd, xm\nnegs xd, xm", "neg x0, x0\nnegs x1, x2", "neg is an alias for sub from zero."},
    {"nop", NULL, "Do nothing.", "nop", "nop", "Useful as a placeholder while defining small routines."},
    {"orr", NULL, "Bitwise OR.", "orr xd, xn, xm\norr xd, xn, #imm", "orr x0, x0, #1\norr x2, x0, x1", "mov register-to-register is often encoded as orr with the zero register."},
    {"ret", NULL, "Return to the address in x30 or another register.", "ret\nret xn", "ret\nret x9", "Use ret at the end of routines entered with bl. A top-level ret can leave the REPL wrapper early."},
    {"rev", "rev16 rev32", "Reverse byte order in a register.", "rev xd, xn\nrev32 xd, xn\nrev16 xd, xn", "rev x0, x0", "Useful for endian conversions."},
    {"ror", NULL, "Rotate right.", "ror xd, xn, #shift\nror xd, xn, xm", "ror x0, x0, #8", "Unlike lsr, rotated-out low bits re-enter at the high end."},
    {"sbc", "sbcs", "Subtract with carry/borrow. sbcs also updates NZCV flags.", "sbc xd, xn, xm\nsbcs xd, xn, xm", "sbcs x0, x0, x1", "Use after subs/sbcs for multi-word subtraction."},
    {"sdiv", "udiv", "Signed or unsigned integer division.", "sdiv xd, xn, xm\nudiv xd, xn, xm", "sdiv x0, x1, x2\nudiv x0, x1, x2", "Division by zero produces zero on ARM64; it does not trap."},
    {"stp", NULL, "Store a pair of registers to memory.", "stp xt1, xt2, [xn]\nstp xt1, xt2, [xn, #imm]\nstp xt1, xt2, [xn], #imm\nstp xt1, xt2, [xn, #imm]!", "stp x0, x1, [x19]\nstp x29, x30, [sp, #-16]!", "Pair stores are commonly used for stack frame save sequences."},
    {"str", "strb strh", "Store a register to memory.", "str xt, [xn]\nstr xt, [xn, #imm]\nstr xt, [xn], #imm\nstr xt, [xn, #imm]!\nstrb wt, [xn]\nstrh wt, [xn]", "str x0, [x19]\nstr x1, [x19, #8]\nstrb w0, [x19]", "In this REPL, x19 starts as a writable scratch page pointer."},
    {"sub", "subs", "Subtract registers or an immediate. subs also updates NZCV flags.", "sub xd, xn, xm\nsub xd, xn, #imm\nsubs xd, xn, xm\nsubs xd, xn, #imm", "sub x0, x0, #1\nsubs xzr, x0, #42", "cmp is an alias for subs with xzr as the destination."},
    {"svc", NULL, "Supervisor call; normally used for system calls.", "svc #imm", "svc #0", "This transfers control to the operating system and is not sandboxed."},
    {"tbz", "tbnz", "Test a single bit and branch if it is zero or nonzero.", "tbz xt, #bit, label\ntbnz xt, #bit, label", "tbz x0, #0, even\ntbnz x1, #7, high_bit_set", "The test does not update NZCV flags."},
    {"tst", NULL, "Test bits by ANDing and setting NZCV flags.", "tst xn, xm\ntst xn, #imm", "tst x0, #1\nb.ne odd", "tst is an alias for ands with xzr as the destination."},
};
#elif ASMREPL_ARCH_X86_64
static const instruction_help_t instruction_help[] = {
    {"adc", NULL, "Add with carry.", "adc dst, src", "adc rax, rbx\nadc rax, 1", "Use after add/adc when building multi-word arithmetic. Updates status flags."},
    {"add", NULL, "Add source to destination.", "add dst, src", "add rax, 1\nadd rax, rcx\nadd qword ptr [r15], 8", "This REPL uses Intel syntax with no register prefixes. add updates status flags."},
    {"and", NULL, "Bitwise AND.", "and dst, src", "and rax, 0xff\nand rax, rcx", "Updates status flags. test is usually better when you only need flags."},
    {"call", NULL, "Call a routine by pushing a return address and branching.", "call label\ncall reg\ncall qword ptr [mem]", "call square\ncall rax", "The target should return with ret. In this REPL, calls are not sandboxed."},
    {"cmp", NULL, "Compare by subtracting source from destination and setting flags.", "cmp lhs, rhs", "cmp rax, 42\nje matched", "The result is discarded. Signed jumps use jl/jle/jg/jge; unsigned jumps use jb/jbe/ja/jae."},
    {"cmovcc", "cmove cmovne cmovz cmovnz cmova cmovae cmovb cmovbe cmovg cmovge cmovl cmovle", "Conditional move based on flags.", "cmov<cc> dst, src", "cmove rax, rcx\ncmovl rax, rdx", "Flags usually come from cmp, test, add, or sub."},
    {"dec", NULL, "Decrement destination by one.", "dec dst", "dec rax\ndec qword ptr [r15]", "Updates most arithmetic flags but not carry."},
    {"div", "idiv", "Unsigned or signed integer division.", "div src\nidiv src", "xor rdx, rdx\ndiv rcx\ncqo\nidiv rcx", "64-bit div divides rdx:rax by src. idiv usually needs cqo to sign-extend rax into rdx."},
    {"imul", NULL, "Signed multiply.", "imul src\nimul dst, src\nimul dst, src, imm", "imul rax, rcx\nimul rax, rcx, 10", "The one-operand form uses rdx:rax. The two- and three-operand forms write the named destination."},
    {"inc", NULL, "Increment destination by one.", "inc dst", "inc rax\ninc qword ptr [r15]", "Updates most arithmetic flags but not carry."},
    {"jcc", "je jne jz jnz ja jae jb jbe jg jge jl jle jc jnc jo jno js jns", "Conditional jump based on flags.", "j<cc> label", "cmp rax, 42\nje equal\njl signed_less\njb unsigned_below", "Flags usually come from cmp, test, add, or sub."},
    {"jmp", NULL, "Unconditional jump.", "jmp label\njmp reg\njmp qword ptr [mem]", "jmp done\njmp rax", "Jumping away from the generated wrapper can hang or crash the REPL."},
    {"lea", NULL, "Load an effective address without touching memory.", "lea dst, [address-expression]", "lea rax, [r15 + 8]\nlea rax, [rcx + rdx*4 + 16]", "Often used for address arithmetic and some integer arithmetic."},
    {"mov", NULL, "Copy data between registers, memory, and immediates.", "mov dst, src", "mov rax, 41\nmov rcx, rax\nmov [r15], rax\nmov rax, [r15]", "Memory-to-memory moves are not allowed; move through a register. r15 starts as a writable scratch page pointer."},
    {"movsx", "movsxd", "Move and sign-extend a smaller source.", "movsx dst, src\nmovsxd dst, src", "movsx rax, byte ptr [r15]\nmovsxd rax, ecx", "Use when the source should be interpreted as signed."},
    {"movzx", NULL, "Move and zero-extend a smaller source.", "movzx dst, src", "movzx rax, byte ptr [r15]\nmovzx eax, cl", "Use when the source should be interpreted as unsigned."},
    {"mul", NULL, "Unsigned multiply using the accumulator.", "mul src", "mov rax, 6\nmul rcx", "64-bit mul computes rdx:rax = rax * src."},
    {"neg", NULL, "Two's-complement negate.", "neg dst", "neg rax", "Updates status flags."},
    {"nop", NULL, "Do nothing.", "nop", "nop", "Useful as a placeholder while defining small routines."},
    {"not", NULL, "Bitwise NOT.", "not dst", "not rax", "Does not update status flags."},
    {"or", NULL, "Bitwise OR.", "or dst, src", "or rax, 1\nor rax, rcx", "Updates status flags."},
    {"pop", NULL, "Pop a value from the stack.", "pop dst", "pop rax", "The REPL wrapper depends on the real process rsp; unbalanced stack changes may crash."},
    {"push", NULL, "Push a value onto the stack.", "push src", "push rax\npush 42", "The REPL wrapper depends on the real process rsp; balance pushes with pops before returning."},
    {"ret", NULL, "Return from a routine by popping the return address into rip.", "ret\nret imm16", "ret", "Use ret at the end of routines entered with call. A top-level ret can leave the REPL wrapper early."},
    {"sal", "sar shl shr", "Shift bits left or right.", "shl dst, count\nsal dst, count\nshr dst, count\nsar dst, count", "shl rax, 3\nshr rax, 1\nsar rax, 1", "shl/sal shift left. shr is logical right shift; sar preserves the sign bit."},
    {"sbb", NULL, "Subtract with borrow.", "sbb dst, src", "sbb rax, rbx\nsbb rax, 0", "Use after sub/sbb when building multi-word subtraction. Updates status flags."},
    {"setcc", "sete setne setz setnz seta setae setb setbe setg setge setl setle", "Set a byte to 0 or 1 based on flags.", "set<cc> r/m8", "cmp rax, 42\nsete al\nsetl byte ptr [r15]", "Only writes one byte. Zero-extend if you need a full register result."},
    {"sub", NULL, "Subtract source from destination.", "sub dst, src", "sub rax, 1\nsub rax, rcx", "Updates status flags."},
    {"syscall", NULL, "Enter the operating system syscall handler.", "syscall", "mov rax, 39\nsyscall", "This transfers control to the operating system and is not sandboxed. Syscall numbers and arguments depend on the OS."},
    {"test", NULL, "AND operands, discard the result, and set flags.", "test lhs, rhs", "test rax, rax\njz was_zero\ntest rax, 1", "Commonly used to check zero or bit masks."},
    {"xchg", NULL, "Exchange two operands.", "xchg dst, src", "xchg rax, rcx\nxchg [r15], rax", "At most one operand may be memory."},
    {"xor", NULL, "Bitwise exclusive OR.", "xor dst, src", "xor rax, rax\nxor rax, rcx", "xor reg, reg is a common way to zero a register. Updates status flags."},
};
#endif

static size_t instruction_help_count(void) {
    return sizeof(instruction_help) / sizeof(instruction_help[0]);
}

static bool alias_matches(const char *aliases, const char *query) {
    if (!aliases) {
        return false;
    }

    size_t query_len = strlen(query);
    const char *p = aliases;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }

        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',') {
            p++;
        }

        if ((size_t)(p - start) == query_len && strncasecmp(start, query, query_len) == 0) {
            return true;
        }
    }

    return false;
}

static const instruction_help_t *find_instruction_help(const char *query) {
    size_t count = instruction_help_count();
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(instruction_help[i].name, query) == 0 ||
            alias_matches(instruction_help[i].aliases, query)) {
            return &instruction_help[i];
        }
    }
    return NULL;
}

static bool extract_instruction_help_query(const char *line, char *query, size_t query_size) {
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        len--;
    }
    if (len == 0 || line[len - 1] != '?') {
        return false;
    }

    const char *p = line;
    const char *end = line + len - 1;
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    if (p == end || *p == ':') {
        return false;
    }

    const char *start = p;
    while (p < end && !isspace((unsigned char)*p) && *p != ',' && *p != '?') {
        p++;
    }

    size_t query_len = (size_t)(p - start);
    if (query_len == 0 || query_len >= query_size) {
        return false;
    }

    memcpy(query, start, query_len);
    query[query_len] = '\0';
    return true;
}

static void print_instruction_help(const char *query) {
    const instruction_help_t *help = find_instruction_help(query);
    if (!help) {
        printf("No built-in help for '%s' on %s.\n", query, architecture_name());
        puts("Use :instructions to list available help topics.");
        puts("You can still try assembling the instruction normally without the trailing '?'.");
        return;
    }

    printf("%s - %s\n", help->name, help->summary);
    if (help->aliases) {
        printf("Aliases/forms: %s\n", help->aliases);
    }
    puts("");
    puts("Syntax:");
    puts(help->syntax);
    puts("");
    puts("Examples:");
    puts(help->examples);
    if (help->notes) {
        puts("");
        puts("Notes:");
        puts(help->notes);
    }
}

static void print_instruction_list(void) {
    printf("Built-in instruction help topics for %s:\n", architecture_name());

    size_t count = instruction_help_count();
    for (size_t i = 0; i < count; i++) {
        printf("  %-8s %s\n", instruction_help[i].name, instruction_help[i].summary);
        if (instruction_help[i].aliases) {
            printf("           aliases/forms: %s\n", instruction_help[i].aliases);
        }
    }

    puts("");
    printf("Type <instruction>? for details, for example %s.\n", instruction_help_examples());
    puts("You can also use :help <instruction>.");
}

static void print_help(void) {
    char compiler_path[MAX_INPUT];
    resolve_executable_path("clang", compiler_path, sizeof(compiler_path));
#if ASMREPL_APPLE_ARM64
    puts("Enter Apple ARM64 assembly.");
#elif ASMREPL_LINUX_ARM64
    puts("Enter Linux arm64 assembly.");
#elif ASMREPL_ARCH_X86_64
    puts("Enter x86_64 assembly (Intel syntax; the wrapper sets .intel_syntax noprefix).");
#endif
    printf("Compiler: %s\n", compiler_path);
    puts("");
    puts("Commands:");
    puts("  :help              show this help");
    puts("  :help <inst>       show help for an instruction");
    puts("  :instructions      list built-in instruction help topics");
    puts("  :regs              print current registers");
    puts("  :reset             zero registers and restore scratch pointers");
    puts("  :scratch           print scratch memory pointer and size");
    puts("  :defs              print persisted labels/directives/routines");
    puts("  :clear             clear persisted labels/directives/routines");
    puts("  :quit              exit");
    puts("");
    puts("Instruction help:");
    printf("  Add ? after an instruction mnemonic, for example %s.\n", instruction_help_examples());
    printf("  A full line ending in ? also works, for example %s.\n", instruction_help_full_line_example());
    puts("");
    puts("Startup flags for the public command:");
    puts("  --debugger         launch this REPL under LLDB, or GDB if LLDB is unavailable");
    puts("  --debugger=<name>  use lldb, lldb-gui, gdb, gdb-tui, cgdb, or pwnbg");
    puts("  --lldb / --gdb     launch under LLDB or GDB explicitly");
    puts("");
    puts("Block mode:");
    puts("  A directive at column 0 is persisted immediately.");
    puts("  A label at column 0 starts a persistent definition block.");
    puts("  Indented lines are added to that block.");
    puts("  The block is committed when you outdent.");
    puts("");
    puts("Notes:");
#if ASMREPL_ARCH_ARM64
    puts("  x19 starts as a writable scratch page pointer.");
    puts("  x20 starts as the scratch page size.");
    puts("  The wrapper depends on the real process sp; unbalanced sp changes may crash.");
#elif ASMREPL_ARCH_X86_64
    puts("  r15 starts as a writable scratch page pointer.");
    puts("  r14 starts as the scratch page size.");
    puts("  The wrapper depends on the real process rsp; unbalanced rsp changes may crash.");
#endif
    puts("  Branches, calls, traps, syscalls, and memory corruption are intentionally not sandboxed.");
}

static int run_command(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        die("waitpid");
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s terminated by signal %d\n", argv[0], WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }

    return 1;
}

#if ASMREPL_ARCH_ARM64
static void emit_load_registers(FILE *fp) {
    for (int reg = 1; reg <= 30; reg++) {
        fprintf(fp, "  ldr x%d, [x0, #%d]\n", reg, reg * 8);
    }
    fprintf(fp, "  ldr x0, [x0, #0]\n");
}

static void emit_store_registers(FILE *fp) {
    fputs("  str x28, [sp, #112]\n", fp);
    fputs("  ldr x28, [sp, #0]\n", fp);

    for (int reg = 0; reg <= 27; reg++) {
        fprintf(fp, "  str x%d, [x28, #%d]\n", reg, reg * 8);
    }

    fputs("  ldr x27, [sp, #112]\n", fp);
    fputs("  str x27, [x28, #224]\n", fp);
    fputs("  str x29, [x28, #232]\n", fp);
    fputs("  str x30, [x28, #240]\n", fp);
    fputs("  mrs x27, nzcv\n", fp);
    fputs("  str x27, [x28, #248]\n", fp);
    fputs("  mov x27, sp\n", fp);
    fputs("  str x27, [x28, #256]\n", fp);
}

static void emit_wrapper(FILE *fp, const char *line) {
    fputs(".text\n", fp);
    fputs(".globl " ENTRY_SYMBOL "\n", fp);
    fputs(".p2align 2\n", fp);
    fputs(ENTRY_SYMBOL ":\n", fp);
    fputs("  sub sp, sp, #128\n", fp);
    fputs("  str x0, [sp, #0]\n", fp);
    fputs("  str x30, [sp, #8]\n", fp);
    fputs("  stp x19, x20, [sp, #16]\n", fp);
    fputs("  stp x21, x22, [sp, #32]\n", fp);
    fputs("  stp x23, x24, [sp, #48]\n", fp);
    fputs("  stp x25, x26, [sp, #64]\n", fp);
    fputs("  stp x27, x28, [sp, #80]\n", fp);
    fputs("  str x29, [sp, #96]\n", fp);
    emit_load_registers(fp);
    fprintf(fp, "  %s\n", line);
    emit_store_registers(fp);
    fputs("  ldp x19, x20, [sp, #16]\n", fp);
    fputs("  ldp x21, x22, [sp, #32]\n", fp);
    fputs("  ldp x23, x24, [sp, #48]\n", fp);
    fputs("  ldp x25, x26, [sp, #64]\n", fp);
    fputs("  ldp x27, x28, [sp, #80]\n", fp);
    fputs("  ldr x29, [sp, #96]\n", fp);
    fputs("  ldr x30, [sp, #8]\n", fp);
    fputs("  add sp, sp, #128\n", fp);
    fputs("  ret\n", fp);
}
#elif ASMREPL_ARCH_X86_64
/*
 * Wrapper for x86_64 (System V ABI; Linux and Intel macOS share this).
 *
 * On entry, System V ABI puts the reg_context_t* in rdi. We:
 *   1. Save callee-saved registers (rbx, rbp, r12-r15) and the ctx pointer.
 *   2. Restore the user's saved register state from the context.
 *   3. Run the user's instruction.
 *   4. Spill user state back into the context (using xchg with [rsp] to
 *      recover the ctx pointer without disturbing rflags).
 *   5. Restore host callee-saved registers and return.
 */
static void emit_wrapper(FILE *fp, const char *line) {
    fputs(".intel_syntax noprefix\n", fp);
    fputs(".text\n", fp);
    fputs(".globl " ENTRY_SYMBOL "\n", fp);
    fputs(".p2align 4\n", fp);
    fputs(ENTRY_SYMBOL ":\n", fp);
    /* save host callee-saved registers */
    fputs("  push rbx\n", fp);
    fputs("  push rbp\n", fp);
    fputs("  push r12\n", fp);
    fputs("  push r13\n", fp);
    fputs("  push r14\n", fp);
    fputs("  push r15\n", fp);
    /* save ctx pointer at [rsp] */
    fputs("  push rdi\n", fp);
    /* load user state from ctx */
    fputs("  mov rax, [rdi + 0]\n", fp);
    fputs("  mov rcx, [rdi + 8]\n", fp);
    fputs("  mov rdx, [rdi + 16]\n", fp);
    fputs("  mov rbx, [rdi + 24]\n", fp);
    fputs("  mov rbp, [rdi + 40]\n", fp);
    fputs("  mov rsi, [rdi + 48]\n", fp);
    fputs("  mov r8,  [rdi + 64]\n", fp);
    fputs("  mov r9,  [rdi + 72]\n", fp);
    fputs("  mov r10, [rdi + 80]\n", fp);
    fputs("  mov r11, [rdi + 88]\n", fp);
    fputs("  mov r12, [rdi + 96]\n", fp);
    fputs("  mov r13, [rdi + 104]\n", fp);
    fputs("  mov r14, [rdi + 112]\n", fp);
    fputs("  mov r15, [rdi + 120]\n", fp);
    fputs("  push qword ptr [rdi + 128]\n", fp);
    fputs("  popfq\n", fp);
    fputs("  mov rdi, [rdi + 56]\n", fp);
    /* user instruction */
    fprintf(fp, "  %s\n", line);
    /* recover ctx pointer via xchg (does not touch rflags) */
    fputs("  xchg rdi, [rsp]\n", fp);
    /* save user GPRs */
    fputs("  mov [rdi + 0], rax\n", fp);
    fputs("  mov [rdi + 8], rcx\n", fp);
    fputs("  mov [rdi + 16], rdx\n", fp);
    fputs("  mov [rdi + 24], rbx\n", fp);
    fputs("  mov [rdi + 40], rbp\n", fp);
    fputs("  mov [rdi + 48], rsi\n", fp);
    fputs("  mov [rdi + 64], r8\n", fp);
    fputs("  mov [rdi + 72], r9\n", fp);
    fputs("  mov [rdi + 80], r10\n", fp);
    fputs("  mov [rdi + 88], r11\n", fp);
    fputs("  mov [rdi + 96], r12\n", fp);
    fputs("  mov [rdi + 104], r13\n", fp);
    fputs("  mov [rdi + 112], r14\n", fp);
    fputs("  mov [rdi + 120], r15\n", fp);
    /* save user rflags before any flag-modifying op */
    fputs("  pushfq\n", fp);
    fputs("  pop rax\n", fp);
    fputs("  mov [rdi + 128], rax\n", fp);
    /* save user rdi (sitting on stack) */
    fputs("  mov rax, [rsp]\n", fp);
    fputs("  mov [rdi + 56], rax\n", fp);
    /* save user rsp (host slot just below user_rdi) */
    fputs("  lea rax, [rsp + 8]\n", fp);
    fputs("  mov [rdi + 32], rax\n", fp);
    /* discard saved user_rdi slot; restore host callee-saved */
    fputs("  add rsp, 8\n", fp);
    fputs("  pop r15\n", fp);
    fputs("  pop r14\n", fp);
    fputs("  pop r13\n", fp);
    fputs("  pop r12\n", fp);
    fputs("  pop rbp\n", fp);
    fputs("  pop rbx\n", fp);
    fputs("  ret\n", fp);
}
#endif

static bool write_assembly_file(const char *path, const char *line, const char *definitions) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return false;
    }

    emit_wrapper(fp, line);

    if (definitions && definitions[0] != '\0') {
        fputs("\n// persisted REPL definitions\n", fp);
        fputs(definitions, fp);
        if (definitions[strlen(definitions) - 1] != '\n') {
            fputc('\n', fp);
        }
    }

    if (fclose(fp) != 0) {
        perror(path);
        return false;
    }

    return true;
}

static uint8_t *read_file(const char *path, size_t *size_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        perror(path);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(fp);
        return NULL;
    }

    long size = ftell(fp);
    if (size < 0) {
        perror("ftell");
        fclose(fp);
        return NULL;
    }

    rewind(fp);

    uint8_t *data = xmalloc((size_t)size);
    if (size > 0 && fread(data, 1, (size_t)size, fp) != (size_t)size) {
        perror("fread");
        free(data);
        fclose(fp);
        return NULL;
    }

    fclose(fp);
    *size_out = (size_t)size;
    return data;
}

static bool checked_range(size_t offset, size_t size, size_t total) {
    return offset <= total && size <= total - offset;
}

#if ASMREPL_FORMAT_MACHO
static bool extract_text_section(const char *object_path, code_blob_t *blob) {
    size_t file_size = 0;
    uint8_t *file = read_file(object_path, &file_size);
    if (!file) {
        return false;
    }

    if (file_size < sizeof(struct mach_header_64)) {
        fprintf(stderr, "object file is too small\n");
        free(file);
        return false;
    }

    const struct mach_header_64 *header = (const struct mach_header_64 *)file;
    if (header->magic != MH_MAGIC_64) {
        fprintf(stderr, "object file is not 64-bit Mach-O\n");
        free(file);
        return false;
    }

    size_t cursor = sizeof(*header);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (!checked_range(cursor, sizeof(struct load_command), file_size)) {
            fprintf(stderr, "truncated Mach-O load command\n");
            free(file);
            return false;
        }

        const struct load_command *lc = (const struct load_command *)(file + cursor);
        if (lc->cmdsize < sizeof(*lc) || !checked_range(cursor, lc->cmdsize, file_size)) {
            fprintf(stderr, "invalid Mach-O load command size\n");
            free(file);
            return false;
        }

        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)lc;
            size_t section_cursor = cursor + sizeof(*seg);

            for (uint32_t sec_index = 0; sec_index < seg->nsects; sec_index++) {
                if (!checked_range(section_cursor, sizeof(struct section_64), file_size)) {
                    fprintf(stderr, "truncated Mach-O section\n");
                    free(file);
                    return false;
                }

                const struct section_64 *sec = (const struct section_64 *)(file + section_cursor);
                if (strncmp(sec->segname, "__TEXT", sizeof(sec->segname)) == 0 &&
                    strncmp(sec->sectname, "__text", sizeof(sec->sectname)) == 0) {
                    if (sec->nreloc != 0) {
                        fprintf(stderr, "assembly produced relocations; labels/external references are not supported yet\n");
                        free(file);
                        return false;
                    }

                    if (sec->size > SIZE_MAX || !checked_range((size_t)sec->offset, (size_t)sec->size, file_size)) {
                        fprintf(stderr, "invalid __text section range\n");
                        free(file);
                        return false;
                    }

                    blob->size = (size_t)sec->size;
                    blob->data = xmalloc(blob->size);
                    memcpy(blob->data, file + sec->offset, blob->size);
                    free(file);
                    return true;
                }

                section_cursor += sizeof(struct section_64);
            }
        }

        cursor += lc->cmdsize;
    }

    fprintf(stderr, "could not find __TEXT,__text in object\n");
    free(file);
    return false;
}
#elif ASMREPL_FORMAT_ELF
static bool extract_text_section(const char *object_path, code_blob_t *blob) {
    size_t file_size = 0;
    uint8_t *file = read_file(object_path, &file_size);
    if (!file) {
        return false;
    }

    if (file_size < sizeof(Elf64_Ehdr)) {
        fprintf(stderr, "object file is too small for ELF64\n");
        free(file);
        return false;
    }

    const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)file;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "object file is not ELF\n");
        free(file);
        return false;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        fprintf(stderr, "object file is not ELF64\n");
        free(file);
        return false;
    }
#if ASMREPL_ARCH_X86_64
    if (ehdr->e_machine != EM_X86_64) {
        fprintf(stderr, "object file is not x86_64\n");
        free(file);
        return false;
#elif ASMREPL_ARCH_ARM64
    if (ehdr->e_machine != EM_AARCH64) {
        fprintf(stderr, "object file is not aarch64\n");
        free(file);
        return false;
#endif
    }
    if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr) || ehdr->e_shnum == 0) {
        fprintf(stderr, "ELF object has no section headers\n");
        free(file);
        return false;
    }

    size_t shtab_bytes = (size_t)ehdr->e_shentsize * ehdr->e_shnum;
    if (!checked_range((size_t)ehdr->e_shoff, shtab_bytes, file_size)) {
        fprintf(stderr, "ELF section headers out of range\n");
        free(file);
        return false;
    }

    const Elf64_Shdr *sections = (const Elf64_Shdr *)(file + ehdr->e_shoff);

    if (ehdr->e_shstrndx == SHN_UNDEF || ehdr->e_shstrndx >= ehdr->e_shnum) {
        fprintf(stderr, "ELF section name string table missing\n");
        free(file);
        return false;
    }
    const Elf64_Shdr *strtab = &sections[ehdr->e_shstrndx];
    if (!checked_range((size_t)strtab->sh_offset, (size_t)strtab->sh_size, file_size)) {
        fprintf(stderr, "ELF string table out of range\n");
        free(file);
        return false;
    }
    const char *strtab_data = (const char *)(file + strtab->sh_offset);

    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        const Elf64_Shdr *sh = &sections[i];
        if (sh->sh_name >= strtab->sh_size) {
            continue;
        }
        const char *name = strtab_data + sh->sh_name;
        if (strcmp(name, ".text") != 0) {
            continue;
        }

        /* Reject if any relocation section refers to this section. */
        for (uint16_t j = 0; j < ehdr->e_shnum; j++) {
            const Elf64_Shdr *rel = &sections[j];
            if ((rel->sh_type == SHT_REL || rel->sh_type == SHT_RELA) &&
                rel->sh_info == i && rel->sh_size > 0) {
                fprintf(stderr, "assembly produced relocations; labels/external references are not supported yet\n");
                free(file);
                return false;
            }
        }

        if (!checked_range((size_t)sh->sh_offset, (size_t)sh->sh_size, file_size)) {
            fprintf(stderr, "invalid .text section range\n");
            free(file);
            return false;
        }
        blob->size = (size_t)sh->sh_size;
        blob->data = xmalloc(blob->size);
        memcpy(blob->data, file + sh->sh_offset, blob->size);
        free(file);
        return true;
    }

    fprintf(stderr, "could not find .text in object\n");
    free(file);
    return false;
}
#endif

static bool assemble_line(const char *line, const char *definitions, unsigned long serial, code_blob_t *blob) {
    char asm_path[256];
    char obj_path[256];
    snprintf(asm_path, sizeof(asm_path), BUILD_DIR "/line-%ld-%lu.s", (long)getpid(), serial);
    snprintf(obj_path, sizeof(obj_path), BUILD_DIR "/line-%ld-%lu.o", (long)getpid(), serial);

    if (!write_assembly_file(asm_path, line, definitions)) {
        return false;
    }

#if ASMREPL_APPLE_ARM64
    char *const argv[] = {
        "clang", "-c", "-arch", "arm64",
        asm_path, "-o", obj_path, NULL,
    };
#elif ASMREPL_APPLE_X86_64
    char *const argv[] = {
        "clang", "-c", "-arch", "x86_64",
        asm_path, "-o", obj_path, NULL,
    };
#else /* Linux: native object format, no -arch needed */
    char *const argv[] = {
        "clang", "-c",
        asm_path, "-o", obj_path, NULL,
    };
#endif

    int status = run_command(argv);
    if (status != 0) {
        fprintf(stderr, "assembly failed with exit code %d\n", status);
        return false;
    }

    return extract_text_section(obj_path, blob);
}

static bool execute_blob(const code_blob_t *blob, reg_context_t *ctx) {
    if (blob->size == 0) {
        fprintf(stderr, "empty code blob\n");
        return false;
    }

    size_t page_size = (size_t)getpagesize();
    size_t map_size = (blob->size + page_size - 1) & ~(page_size - 1);
    void *mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        return false;
    }

    memcpy(mem, blob->data, blob->size);

#if ASMREPL_APPLE_ARM64
    sys_icache_invalidate(mem, blob->size);
#elif ASMREPL_LINUX_ARM64
    __builtin___clear_cache((char *)mem, (char *)mem + blob->size);
#endif
    /* On x86_64 the icache is coherent with stores, no flush needed. */

    if (mprotect(mem, map_size, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        munmap(mem, map_size);
        return false;
    }

    ((jit_fn_t)mem)(ctx);

    if (munmap(mem, map_size) != 0) {
        perror("munmap");
        return false;
    }

    return true;
}

static bool run_line(const char *line, const char *definitions, unsigned long serial, reg_context_t *ctx) {
    code_blob_t blob = {0};
    if (!assemble_line(line, definitions, serial, &blob)) {
        return false;
    }

    bool ok = execute_blob(&blob, ctx);
    free(blob.data);
    return ok;
}

static void commit_definition_block(text_buffer_t *definitions, text_buffer_t *block) {
    if (block->len == 0) {
        return;
    }

    if (definitions->len > 0 && definitions->data[definitions->len - 1] != '\n') {
        text_buffer_append(definitions, "\n");
    }
    text_buffer_append(definitions, block->data);
    if (definitions->len > 0 && definitions->data[definitions->len - 1] != '\n') {
        text_buffer_append(definitions, "\n");
    }
    text_buffer_clear(block);
    puts("definition block committed");
}

int main(void) {
    ensure_build_dir();

    void *scratch = mmap(NULL, SCRATCH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (scratch == MAP_FAILED) {
        die("mmap scratch");
    }

    reg_context_t ctx;
    reset_context(&ctx, scratch);

    text_buffer_t definitions;
    text_buffer_t block;
    text_buffer_init(&definitions);
    text_buffer_init(&block);
    bool in_block = false;

#if ASMREPL_APPLE_ARM64
    const char *banner = "arm64 native assembly REPL (macOS).";
#elif ASMREPL_APPLE_X86_64
    const char *banner = "x86_64 native assembly REPL (macOS). Intel syntax.";
#elif ASMREPL_LINUX_ARM64
    const char *banner = "arm64 native assembly REPL (Linux).";
#elif ASMREPL_LINUX_X86_64
    const char *banner = "x86_64 native assembly REPL (Linux). Intel syntax.";
#endif
    printf("%s\n", banner);
#if ASMREPL_ARCH_ARM64
    printf("scratch: x19 = 0x%016llx, x20 = %d bytes\n",
           (unsigned long long)(uintptr_t)scratch, SCRATCH_SIZE);
#elif ASMREPL_ARCH_X86_64
    printf("scratch: r15 = 0x%016llx, r14 = %d bytes\n",
           (unsigned long long)(uintptr_t)scratch, SCRATCH_SIZE);
#endif
    puts("");
    print_help();

    char input[MAX_INPUT];
    unsigned long serial = 1;
    for (;;) {
        fputs(in_block ? "asm| " : "asm> ", stdout);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            putchar('\n');
            break;
        }

        char raw_line[MAX_INPUT];
        snprintf(raw_line, sizeof(raw_line), "%s", input);
        size_t raw_len = strlen(raw_line);
        while (raw_len > 0 && (raw_line[raw_len - 1] == '\n' || raw_line[raw_len - 1] == '\r')) {
            raw_line[--raw_len] = '\0';
        }

        char code_line[MAX_INPUT];
        snprintf(code_line, sizeof(code_line), "%s", raw_line);
        strip_asm_inline_comment(code_line);

        char *line = trim(code_line);
        if (*line == '\0') {
            if (in_block) {
                text_buffer_append_line(&block, "");
            }
            continue;
        }

        if (in_block && !starts_indented(code_line)) {
            commit_definition_block(&definitions, &block);
            in_block = false;
        }

        if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) {
            break;
        }

        char *help_arg = NULL;
        if (strncmp(line, ":help", 5) == 0 && (line[5] == '\0' || isspace((unsigned char)line[5]))) {
            help_arg = trim(line + 5);
        } else if (strncmp(line, ":h", 2) == 0 && (line[2] == '\0' || isspace((unsigned char)line[2]))) {
            help_arg = trim(line + 2);
        }
        if (help_arg) {
            if (*help_arg == '\0') {
                print_help();
            } else if (strcmp(help_arg, "instructions") == 0 || strcmp(help_arg, "inst") == 0) {
                print_instruction_list();
            } else {
                print_instruction_help(help_arg);
            }
            continue;
        }

        if (strcmp(line, ":instructions") == 0 || strcmp(line, ":inst") == 0 || strcmp(line, ":i") == 0) {
            print_instruction_list();
            continue;
        }

        if (strcmp(line, ":regs") == 0 || strcmp(line, ":r") == 0) {
            print_regs(&ctx);
            continue;
        }

        if (strcmp(line, ":reset") == 0) {
            reset_context(&ctx, scratch);
            puts("register context reset");
            print_regs(&ctx);
            continue;
        }

        if (strcmp(line, ":scratch") == 0) {
            printf("scratch pointer: 0x%016llx\n", (unsigned long long)(uintptr_t)scratch);
            printf("scratch size:    %d bytes\n", SCRATCH_SIZE);
            continue;
        }

        if (strcmp(line, ":defs") == 0) {
            if (definitions.len == 0 && block.len == 0) {
                puts("(no definitions)");
            } else {
                if (definitions.len > 0) {
                    fputs(definitions.data, stdout);
                }
                if (block.len > 0) {
                    fputs(block.data, stdout);
                }
            }
            continue;
        }

        if (strcmp(line, ":clear") == 0) {
            text_buffer_clear(&definitions);
            text_buffer_clear(&block);
            in_block = false;
            puts("definitions cleared");
            continue;
        }

        char instruction_query[64];
        if (extract_instruction_help_query(line, instruction_query, sizeof(instruction_query))) {
            print_instruction_help(instruction_query);
            continue;
        }

        if (in_block) {
            text_buffer_append_line(&block, code_line);
            continue;
        }

        if (is_definition_start(code_line)) {
            text_buffer_append_line(&block, code_line);
            in_block = true;
            continue;
        }

        if (is_directive(code_line)) {
            text_buffer_append_line(&definitions, code_line);
            continue;
        }

        if (run_line(line, definitions.data, serial++, &ctx)) {
            print_regs(&ctx);
        }
    }

    if (in_block) {
        commit_definition_block(&definitions, &block);
    }

    text_buffer_free(&definitions);
    text_buffer_free(&block);
    munmap(scratch, SCRATCH_SIZE);
    return 0;
}
