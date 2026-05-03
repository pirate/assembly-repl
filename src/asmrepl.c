#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
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

#define BUILD_DIR ".asmrepl-build"
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

static void print_help(void) {
#if ASMREPL_APPLE_ARM64
    puts("Enter Apple ARM64 assembly.");
#elif ASMREPL_LINUX_ARM64
    puts("Enter Linux arm64 assembly.");
#elif ASMREPL_ARCH_X86_64
    puts("Enter x86_64 assembly (Intel syntax; the wrapper sets .intel_syntax noprefix).");
#endif
    puts("");
    puts("Commands:");
    puts("  :help     show this help");
    puts("  :regs     print current registers");
    puts("  :reset    zero registers and restore scratch pointers");
    puts("  :scratch  print scratch memory pointer and size");
    puts("  :defs     print persisted labels/directives/routines");
    puts("  :clear    clear persisted labels/directives/routines");
    puts("  :quit     exit");
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
    printf("%s Type :help for commands.\n", banner);
#if ASMREPL_ARCH_ARM64
    printf("scratch: x19 = 0x%016llx, x20 = %d bytes\n",
           (unsigned long long)(uintptr_t)scratch, SCRATCH_SIZE);
#elif ASMREPL_ARCH_X86_64
    printf("scratch: r15 = 0x%016llx, r14 = %d bytes\n",
           (unsigned long long)(uintptr_t)scratch, SCRATCH_SIZE);
#endif

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

        char *line = trim(input);
        if (*line == '\0') {
            if (in_block) {
                text_buffer_append_line(&block, "");
            }
            continue;
        }

        if (in_block && !starts_indented(raw_line)) {
            commit_definition_block(&definitions, &block);
            in_block = false;
        }

        if (strcmp(line, ":quit") == 0 || strcmp(line, ":q") == 0) {
            break;
        }

        if (strcmp(line, ":help") == 0 || strcmp(line, ":h") == 0) {
            print_help();
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

        if (in_block) {
            text_buffer_append_line(&block, raw_line);
            continue;
        }

        if (is_definition_start(raw_line)) {
            text_buffer_append_line(&block, raw_line);
            in_block = true;
            continue;
        }

        if (is_directive(raw_line)) {
            text_buffer_append_line(&definitions, raw_line);
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
