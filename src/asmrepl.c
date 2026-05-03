#define _DARWIN_C_SOURCE

#include <errno.h>
#include <mach-o/loader.h>
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

#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#endif

#define BUILD_DIR ".asmrepl-build"
#define SCRATCH_SIZE 4096
#define MAX_INPUT 4096
#define REG_COUNT 31

typedef struct {
    uint64_t x[REG_COUNT];
    uint64_t nzcv;
    uint64_t sp;
} reg_context_t;

typedef struct {
    uint8_t *data;
    size_t size;
} code_blob_t;

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

static void ensure_build_dir(void) {
    if (mkdir(BUILD_DIR, 0755) != 0 && errno != EEXIST) {
        die("mkdir " BUILD_DIR);
    }
}

static void reset_context(reg_context_t *ctx, void *scratch) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->x[19] = (uint64_t)(uintptr_t)scratch;
    ctx->x[20] = SCRATCH_SIZE;
}

static void print_flags(uint64_t nzcv) {
    putchar((nzcv & (1ULL << 31)) ? 'N' : 'n');
    putchar((nzcv & (1ULL << 30)) ? 'Z' : 'z');
    putchar((nzcv & (1ULL << 29)) ? 'C' : 'c');
    putchar((nzcv & (1ULL << 28)) ? 'V' : 'v');
}

static void print_regs(const reg_context_t *ctx) {
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
    print_flags(ctx->nzcv);
    puts("]");
}

static void print_help(void) {
    puts("Enter one Apple ARM64 assembly instruction per line.");
    puts("");
    puts("Commands:");
    puts("  :help     show this help");
    puts("  :regs     print current registers");
    puts("  :reset    zero registers and restore scratch pointers");
    puts("  :scratch  print scratch memory pointer and size");
    puts("  :quit     exit");
    puts("");
    puts("Notes:");
    puts("  x19 starts as a writable scratch page pointer.");
    puts("  x20 starts as the scratch page size.");
    puts("  The wrapper depends on the real process sp; unbalanced sp changes may crash.");
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

static bool write_assembly_file(const char *path, const char *line) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return false;
    }

    fputs(".text\n", fp);
    fputs(".globl _asmrepl_entry\n", fp);
    fputs(".p2align 2\n", fp);
    fputs("_asmrepl_entry:\n", fp);
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

static bool assemble_line(const char *line, unsigned long serial, code_blob_t *blob) {
    char asm_path[256];
    char obj_path[256];
    snprintf(asm_path, sizeof(asm_path), BUILD_DIR "/line-%lu.s", serial);
    snprintf(obj_path, sizeof(obj_path), BUILD_DIR "/line-%lu.o", serial);

    if (!write_assembly_file(asm_path, line)) {
        return false;
    }

    char *const argv[] = {
        "clang",
        "-c",
        "-arch",
        "arm64",
        asm_path,
        "-o",
        obj_path,
        NULL,
    };

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

#ifdef __APPLE__
    sys_icache_invalidate(mem, blob->size);
#endif

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

static bool run_line(const char *line, unsigned long serial, reg_context_t *ctx) {
    code_blob_t blob = {0};
    if (!assemble_line(line, serial, &blob)) {
        return false;
    }

    bool ok = execute_blob(&blob, ctx);
    free(blob.data);
    return ok;
}

int main(void) {
#if !defined(__APPLE__) || !defined(__aarch64__)
    fprintf(stderr, "asmrepl currently supports Apple Silicon macOS only.\n");
    return 1;
#endif

    ensure_build_dir();

    void *scratch = mmap(NULL, SCRATCH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (scratch == MAP_FAILED) {
        die("mmap scratch");
    }

    reg_context_t ctx;
    reset_context(&ctx, scratch);

    puts("arm64 native assembly REPL. Type :help for commands.");
    printf("scratch: x19 = 0x%016llx, x20 = %d bytes\n",
           (unsigned long long)(uintptr_t)scratch,
           SCRATCH_SIZE);

    char input[MAX_INPUT];
    unsigned long serial = 1;
    for (;;) {
        fputs("asm> ", stdout);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            putchar('\n');
            break;
        }

        char *line = trim(input);
        if (*line == '\0') {
            continue;
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

        if (run_line(line, serial++, &ctx)) {
            print_regs(&ctx);
        }
    }

    munmap(scratch, SCRATCH_SIZE);
    return 0;
}
