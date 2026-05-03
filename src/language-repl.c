#define _GNU_SOURCE
#define _DARWIN_C_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#  define SHARED_EXT ".dylib"
#else
#  define SHARED_EXT ".so"
#endif

#define BUILD_DIR ".repl-build"
#define REPL_MAX_INPUT 4096
#define SCRATCH_SIZE 4096
#define SLOT_COUNT 16
#define OUT_SIZE 4096

typedef struct {
    uint64_t u64[SLOT_COUNT];
    double f64[SLOT_COUNT];
    uint8_t scratch[SCRATCH_SIZE];
    char out[OUT_SIZE];
    uint64_t result;
} repl_state_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} text_buffer_t;

typedef enum {
    MODE_C,
    MODE_CPP,
    MODE_OBJC,
    MODE_LLVMIR,
} repl_mode_t;

typedef struct {
    const char *topic;
    const char *aliases;
    const char *summary;
    const char *syntax;
    const char *examples;
    const char *notes;
} topic_help_t;

typedef struct {
    char *name;
    char summary[96];
} ir_instruction_info_t;

typedef struct {
    ir_instruction_info_t *items;
    size_t len;
    size_t cap;
    char source[REPL_MAX_INPUT];
} ir_instruction_list_t;

typedef void (*repl_entry_fn)(repl_state_t *);

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

static char *xstrdup(const char *text) {
    size_t len = strlen(text) + 1;
    char *copy = xmalloc(len);
    memcpy(copy, text, len);
    return copy;
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

static bool strip_repl_inline_comment(repl_mode_t mode, char *line) {
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
        if (in_single || in_double) {
            continue;
        }
        if (p[0] == '/' && p[1] == '/') {
            *p = '\0';
            rtrim_in_place(line);
            return true;
        }
        if (mode == MODE_LLVMIR && *p == ';') {
            *p = '\0';
            rtrim_in_place(line);
            return true;
        }
    }
    return false;
}

static const char *mode_name(repl_mode_t mode) {
    switch (mode) {
        case MODE_C: return "c";
        case MODE_CPP: return "cpp";
        case MODE_OBJC: return "objc";
        case MODE_LLVMIR: return "ir";
    }
    return "repl";
}

static const char *mode_title(repl_mode_t mode) {
    switch (mode) {
        case MODE_C: return "C";
        case MODE_CPP: return "C++";
        case MODE_OBJC: return "Objective-C";
        case MODE_LLVMIR: return "LLVM IR";
    }
    return "source";
}

static const char *mode_extension(repl_mode_t mode) {
    switch (mode) {
        case MODE_C: return ".c";
        case MODE_CPP: return ".cc";
        case MODE_OBJC: return ".m";
        case MODE_LLVMIR: return ".ll";
    }
    return ".txt";
}

static bool mode_is_statement_repl(repl_mode_t mode) {
    return mode == MODE_C || mode == MODE_CPP || mode == MODE_OBJC;
}

static const char *compiler_command(repl_mode_t mode) {
    switch (mode) {
        case MODE_C:
        case MODE_OBJC:
        case MODE_LLVMIR:
            return "clang";
        case MODE_CPP:
            return "clang++";
    }
    return NULL;
}

static bool resolve_executable_path(const char *name, char *out, size_t out_size) {
    if (!name) {
        snprintf(out, out_size, "(none; this REPL is currently a stub)");
        return false;
    }

    if (strchr(name, '/')) {
        char resolved[REPL_MAX_INPUT];
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

        char candidate[REPL_MAX_INPUT];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (access(candidate, X_OK) == 0) {
            char resolved[REPL_MAX_INPUT];
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

static void reset_state(repl_state_t *state) {
    memset(state, 0, sizeof(*state));
}

static int run_command_with_stdio(char *const argv[], bool quiet) {
    pid_t pid = fork();
    if (pid < 0) {
        die("fork");
    }

    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
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
        if (quiet) {
            return 128 + WTERMSIG(status);
        }
        fprintf(stderr, "%s terminated by signal %d\n", argv[0], WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }

    return 1;
}

static bool capture_command_line(const char *command, char *out, size_t out_size) {
    FILE *fp = popen(command, "r");
    if (!fp) {
        return false;
    }

    bool ok = fgets(out, (int)out_size, fp) != NULL;
    int status = pclose(fp);
    if (!ok || status != 0) {
        if (out_size > 0) {
            out[0] = '\0';
        }
        return false;
    }

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
    return len > 0;
}

static char *read_text_file(const char *path, size_t *size_out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *data = xmalloc((size_t)size + 1);
    if (size > 0 && fread(data, 1, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return NULL;
    }

    data[size] = '\0';
    fclose(fp);
    if (size_out) {
        *size_out = (size_t)size;
    }
    return data;
}

static bool path_is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void print_state(const repl_state_t *state) {
    printf("result 0x%016llx (%llu)\n",
           (unsigned long long)state->result,
           (unsigned long long)state->result);

    for (int i = 0; i < 8; i++) {
        printf("u%-2d 0x%016llx  ", i, (unsigned long long)state->u64[i]);
        if ((i % 4) == 3) {
            putchar('\n');
        }
    }

    printf("f0 %.6g  f1 %.6g  f2 %.6g  f3 %.6g\n",
           state->f64[0], state->f64[1], state->f64[2], state->f64[3]);

    bool scratch_nonzero = false;
    for (int i = 0; i < 32; i++) {
        if (state->scratch[i] != 0) {
            scratch_nonzero = true;
            break;
        }
    }
    if (scratch_nonzero) {
        fputs("scratch[0..31]", stdout);
        for (int i = 0; i < 32; i++) {
            printf(" %02x", state->scratch[i]);
        }
        putchar('\n');
    }

    if (state->out[0] != '\0') {
        fputs("out:\n", stdout);
        fputs(state->out, stdout);
        size_t len = strlen(state->out);
        if (len == 0 || state->out[len - 1] != '\n') {
            putchar('\n');
        }
    }
}

static const topic_help_t c_topics[] = {
    {"state", "slot slots", "Persistent REPL state shared by every compiled snippet.", "state->result\nstate->u64[n]\nstate->f64[n]\nstate->scratch[n]", "state->result = 42;\nU(0) = U(0) + 1;\nSCRATCH(0) = 0xaa;", "The helper macros U(n), F(n), and SCRATCH(n) expand to fields on the state pointer."},
    {"print", "printf out", "Append formatted text to the REPL output buffer.", "print(\"format\", ...)", "print(\"answer=%llu\\n\", (unsigned long long)U(0));", "This writes into state->out, then the host prints it after the snippet returns."},
    {"include", "import header", "Persist a preprocessor include for future snippets.", "#include <header>", "#include <math.h>\n:defs", "Lines beginning with # are persisted immediately."},
    {"function", "def definition", "Persist helper functions, globals, structs, and typedefs.", "int square(int x) {\n    return x * x;\n}", "static uint64_t twice(uint64_t x) {\n    return x * 2;\n}\nstate->result = twice(21);", "Multi-line input is collected until the compiler accepts it. Top-level definitions are persisted; statements run inside repl_entry."},
    {"if", NULL, "Branch inside the generated entry function.", "if (condition) statement\nif (condition) { ... } else { ... }", "if (U(0) == 0) U(0) = 1; else U(0) *= 2;", "State persists between snippets; local variables do not."},
    {"for", "while loop", "Loop inside the generated entry function.", "for (init; condition; step) { ... }\nwhile (condition) { ... }", "for (int i = 0; i < 8; i++) U(0) += i;", "Infinite loops will hang the REPL process."},
    {"pointer", "memory scratch", "Use the scratch buffer as persistent writable memory.", "uint8_t *p = state->scratch;", "uint64_t *p = (uint64_t *)state->scratch;\np[0] = 0xfeedface;", "The scratch buffer is 4096 bytes and lives in host memory."},
};

static const topic_help_t cpp_topics[] = {
    {"state", "slot slots", "Persistent REPL state shared by every compiled snippet.", "state->result\nstate->u64[n]\nstate->f64[n]\nstate->scratch[n]", "state->result = 42;\nU(0) += 1;\nF(0) = 3.14;", "The C helper macros are available in C++ too."},
    {"print", "printf out", "Append formatted text to the REPL output buffer.", "print(\"format\", ...)", "print(\"u0=%llu\\n\", (unsigned long long)U(0));", "For iostream experiments, include the header in a :def block and write to state explicitly."},
    {"include", "using header", "Persist includes and using declarations for future snippets.", "#include <vector>\nusing std::vector;", "#include <vector>\n:defs", "Lines beginning with # are persisted immediately. Other top-level declarations belong in :def blocks."},
    {"function", "lambda def", "Persist helper functions or run local lambdas.", "auto helper_name(args) -> type { ... }", "auto sq = [](uint64_t x) { return x * x; };\nstate->result = sq(12);", "A lambda typed at the prompt is local to that one execution."},
    {"class", "struct", "Persist C++ classes and structs.", "struct Point {\n    int x;\n    int y;\n};", "struct Counter {\n    uint64_t n = 0;\n    void inc() { n++; }\n};", "Multi-line input is collected until the compiler accepts it."},
    {"template", NULL, "Persist function or class templates.", "template <class T>\nT twice(T x) {\n    return x + x;\n}", "state->result = twice<uint64_t>(21);", "Templates are compiled each time a new snippet is built."},
    {"for", "while loop", "Loop inside the generated entry function.", "for (init; condition; step) { ... }\nwhile (condition) { ... }", "for (auto i = 0; i < 8; ++i) U(0) += i;", "Infinite loops will hang the REPL process."},
};

static const topic_help_t objc_topics[] = {
    {"state", "slot slots", "Persistent REPL state shared by every compiled snippet.", "state->result\nstate->u64[n]\nstate->f64[n]\nstate->scratch[n]", "state->result = 42;\nU(0) += 1;", "Snippets run inside an @autoreleasepool on macOS."},
    {"print", "printf nslog out", "Append formatted text to the REPL output buffer.", "print(\"format\", ...)", "NSString *s = @\"hello\";\nprint(\"%s\\n\", [s UTF8String]);", "print writes into state->out. NSLog writes to stderr and is not captured."},
    {"import", "include foundation", "Persist Objective-C imports.", "#import <Foundation/Foundation.h>", "#import <Foundation/Foundation.h>", "Foundation is imported by default on macOS."},
    {"message", "selector", "Send Objective-C messages.", "[receiver selector]\n[receiver selector:arg]", "NSString *s = [@\"gpu\" uppercaseString];\nprint(\"%s\\n\", [s UTF8String]);", "Objective-C snippets are compiled with ARC on macOS."},
    {"class", "interface implementation def", "Persist Objective-C interfaces and implementations.", "@interface Box : NSObject\n@property NSInteger value;\n@end\n@implementation Box\n@end", "@interface Greeter : NSObject\n- (NSString *)hello;\n@end\n@implementation Greeter\n- (NSString *)hello { return @\"hi\"; }\n@end", "Multi-line input is collected until the compiler accepts it."},
};

static const topic_help_t ir_topics[] = {
    {"state", "slot slots", "The %state pointer references the persistent REPL state struct.", "%repl_state = type { [16 x i64], [16 x double], [4096 x i8], [4096 x i8], i64 }", "%r = getelementptr %repl_state, ptr %state, i32 0, i32 4\nstore i64 42, ptr %r", "Field 0 is u64 slots, field 1 is f64 slots, field 2 is scratch, field 3 is out, field 4 is result."},
    {"add", "sub mul udiv sdiv", "Integer arithmetic instructions.", "%name = add i64 %a, %b", "%x = add i64 40, 2\n%r = getelementptr %repl_state, ptr %state, i32 0, i32 4\nstore i64 %x, ptr %r", "LLVM IR input is appended to the current function body, then the whole body is recompiled."},
    {"load", NULL, "Load a typed value from memory.", "%value = load type, ptr %address", "%p = getelementptr %repl_state, ptr %state, i32 0, i32 0, i32 0\n%v = load i64, ptr %p", "Use getelementptr to compute addresses inside the state struct."},
    {"store", NULL, "Store a typed value to memory.", "store type %value, ptr %address", "%p = getelementptr %repl_state, ptr %state, i32 0, i32 0, i32 0\nstore i64 123, ptr %p", "Stores to %state persist after the function returns."},
    {"getelementptr", "gep", "Compute an address inside an aggregate without loading memory.", "%p = getelementptr type, ptr %base, indices...", "%result = getelementptr %repl_state, ptr %state, i32 0, i32 4", "The state struct type is already declared as %repl_state."},
    {"icmp", "fcmp", "Compare values and produce an i1.", "%c = icmp eq i64 %a, %b", "%c = icmp ult i64 %v, 10", "Pair with select or br."},
    {"br", "phi", "Branch to labels and join values with phi.", "br label %name\nbr i1 %cond, label %yes, label %no\nname:", "br label %done\ndone:", "A terminator ends the current basic block; malformed control flow will be rejected by LLVM."},
    {"call", "declare", "Call a declared function.", "%x = call i64 @fn(i64 %arg)", ":def\ndeclare i32 @puts(ptr)\n:end", "External calls from a shared library depend on platform dynamic linking behavior."},
    {"ret", NULL, "Return from the generated entry function.", "ret void", "ret void", "If you type ret void, later appended instructions will be unreachable or invalid until :clear."},
};

static void ir_instruction_list_init(ir_instruction_list_t *list) {
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
    list->source[0] = '\0';
}

static void ir_instruction_list_free(ir_instruction_list_t *list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i].name);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static bool ir_instruction_exists(const ir_instruction_list_t *list, const char *name) {
    for (size_t i = 0; i < list->len; i++) {
        if (strcmp(list->items[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool string_is_one_of(const char *text, const char *const *items, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(text, items[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool string_has_suffix(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

static const char *ir_instruction_summary_from_name(const char *opcode) {
    static const char *const terminators[] = {
        "ret", "br", "switch", "indirectbr", "invoke", "callbr",
        "resume", "catchswitch", "catchret", "cleanupret", "unreachable",
    };
    static const char *const integer_binary[] = {
        "add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
        "shl", "lshr", "ashr", "and", "or", "xor",
    };
    static const char *const floating_binary[] = {
        "fadd", "fsub", "fmul", "fdiv", "frem",
    };

    if (string_is_one_of(opcode, terminators, sizeof(terminators) / sizeof(terminators[0]))) {
        return "Terminator/control-flow instruction.";
    }
    if (string_is_one_of(opcode, integer_binary, sizeof(integer_binary) / sizeof(integer_binary[0]))) {
        return "Integer arithmetic or bitwise instruction.";
    }
    if (string_is_one_of(opcode, floating_binary, sizeof(floating_binary) / sizeof(floating_binary[0])) ||
        strcmp(opcode, "fneg") == 0) {
        return "Floating-point arithmetic instruction.";
    }
    if (strcmp(opcode, "icmp") == 0) {
        return "Integer/pointer comparison instruction.";
    }
    if (strcmp(opcode, "fcmp") == 0) {
        return "Floating-point comparison instruction.";
    }
    if (strstr(opcode, "load") || strstr(opcode, "store") || strstr(opcode, "atomic") ||
        strcmp(opcode, "alloca") == 0 || strcmp(opcode, "getelementptr") == 0 ||
        strcmp(opcode, "fence") == 0 || strcmp(opcode, "cmpxchg") == 0) {
        return "Memory or atomic memory instruction.";
    }
    if (strcmp(opcode, "phi") == 0) {
        return "SSA value merge instruction.";
    }
    if (strcmp(opcode, "select") == 0) {
        return "Conditional value selection instruction.";
    }
    if (strcmp(opcode, "call") == 0 || strcmp(opcode, "va_arg") == 0) {
        return "Call or variable-argument instruction.";
    }
    if (strstr(opcode, "value") || strstr(opcode, "element") || strstr(opcode, "vector")) {
        return "Aggregate or vector manipulation instruction.";
    }
    if (strcmp(opcode, "trunc") == 0 || string_has_suffix(opcode, "trunc") ||
        string_has_suffix(opcode, "ext") || strncmp(opcode, "fpto", 4) == 0 ||
        string_has_suffix(opcode, "tofp") || strncmp(opcode, "ptrto", 5) == 0 ||
        strncmp(opcode, "intto", 5) == 0 || strcmp(opcode, "bitcast") == 0 ||
        strcmp(opcode, "addrspacecast") == 0) {
        return "Conversion/cast instruction.";
    }
    if (strstr(opcode, "pad") || strstr(opcode, "catch") || strstr(opcode, "cleanup") ||
        strcmp(opcode, "landingpad") == 0) {
        return "Exception handling pad instruction.";
    }
    if (strcmp(opcode, "malloc") == 0 || strcmp(opcode, "free") == 0) {
        return "Legacy memory allocation/deallocation instruction.";
    }
    if (strcmp(opcode, "getresult") == 0) {
        return "Legacy result extraction instruction.";
    }
    if (strcmp(opcode, "unwind") == 0) {
        return "Legacy exception control-flow instruction.";
    }
    return "LLVM IR instruction.";
}

static const char *ir_instruction_summary_from_macro(const char *macro_name, const char *opcode) {
    if (strstr(macro_name, "TERM")) {
        return "Terminator/control-flow instruction.";
    }
    if (strstr(macro_name, "MEMORY")) {
        return "Memory or atomic memory instruction.";
    }
    if (strstr(macro_name, "CAST")) {
        return "Conversion/cast instruction.";
    }
    if (strstr(macro_name, "BINARY")) {
        return "Binary arithmetic or bitwise instruction.";
    }
    if (strstr(macro_name, "UNARY")) {
        return "Unary arithmetic instruction.";
    }
    if (strstr(macro_name, "FUNCLET")) {
        return "Exception handling pad instruction.";
    }
    return ir_instruction_summary_from_name(opcode);
}

static void ir_instruction_list_add(ir_instruction_list_t *list, const char *name, const char *summary) {
    if (!name || !summary || !isalpha((unsigned char)name[0])) {
        return;
    }
    if (ir_instruction_exists(list, name)) {
        return;
    }

    if (list->len == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 64;
        list->items = xrealloc(list->items, list->cap * sizeof(list->items[0]));
    }

    list->items[list->len].name = xstrdup(name);
    snprintf(list->items[list->len].summary, sizeof(list->items[list->len].summary), "%s", summary);
    list->len++;
}

static int compare_ir_instruction_info(const void *a, const void *b) {
    const ir_instruction_info_t *ia = a;
    const ir_instruction_info_t *ib = b;
    return strcmp(ia->name, ib->name);
}

static void sort_ir_instruction_list(ir_instruction_list_t *list) {
    qsort(list->items, list->len, sizeof(list->items[0]), compare_ir_instruction_info);
}

static char *trim_span(char *start, char *end) {
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return start;
}

static bool normalize_ir_opcode(const char *opcode, char *out, size_t out_size) {
    size_t len = 0;
    for (const char *p = opcode; *p && !isspace((unsigned char)*p); p++) {
        if (!isalnum((unsigned char)*p)) {
            return false;
        }
        if (len + 1 >= out_size) {
            return false;
        }
        out[len++] = (char)tolower((unsigned char)*p);
    }
    out[len] = '\0';
    return len > 0;
}

static bool parse_instruction_def_file(const char *path, ir_instruction_list_t *list) {
    size_t size = 0;
    char *data = read_text_file(path, &size);
    if (!data) {
        return false;
    }

    char *line = data;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }

        char *trimmed_line = trim_span(line, line + strlen(line));
        if (strncmp(trimmed_line, "HANDLE_", 7) != 0) {
            line = next;
            continue;
        }

        char *macro = trimmed_line;
        char *open = macro ? strchr(macro, '(') : NULL;
        if (!macro || !open || !strstr(macro, "_INST")) {
            line = next;
            continue;
        }

        char macro_name[64];
        size_t macro_len = (size_t)(open - macro);
        if (macro_len >= sizeof(macro_name)) {
            macro_len = sizeof(macro_name) - 1;
        }
        memcpy(macro_name, macro, macro_len);
        macro_name[macro_len] = '\0';

        char *first_comma = strchr(open + 1, ',');
        char *second_comma = first_comma ? strchr(first_comma + 1, ',') : NULL;
        if (!first_comma || !second_comma) {
            line = next;
            continue;
        }

        char *opcode = trim_span(first_comma + 1, second_comma);
        char normalized[64];
        if (normalize_ir_opcode(opcode, normalized, sizeof(normalized))) {
            ir_instruction_list_add(list, normalized, ir_instruction_summary_from_macro(macro_name, normalized));
        }

        line = next;
    }

    if (list->len > 0) {
        snprintf(list->source, sizeof(list->source), "%s", path);
    }
    free(data);
    return list->len > 0;
}

static bool add_instruction_def_candidate(const char *path, ir_instruction_list_t *list) {
    return path && path[0] != '\0' && path_is_file(path) && parse_instruction_def_file(path, list);
}

static bool parse_highlight_js_file(const char *path, ir_instruction_list_t *list) {
    size_t size = 0;
    char *data = read_text_file(path, &size);
    if (!data) {
        return false;
    }

    char *start = strstr(data, " add fadd sub");
    char *end = start ? strstr(start, " argmemonly") : NULL;
    if (!start || !end) {
        free(data);
        return false;
    }

    for (char *p = start; p < end;) {
        while (p < end && !isalpha((unsigned char)*p)) {
            p++;
        }
        char *token_start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
            p++;
        }
        if (p > token_start) {
            char saved = *p;
            *p = '\0';
            ir_instruction_list_add(list, token_start, ir_instruction_summary_from_name(token_start));
            *p = saved;
        }
    }

    if (list->len > 0) {
        snprintf(list->source, sizeof(list->source), "%s", path);
    }
    free(data);
    return list->len > 0;
}

static bool parse_highlight_js_dir(const char *dir_path, ir_instruction_list_t *list) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return false;
    }

    struct dirent *entry;
    bool ok = false;
    while ((entry = readdir(dir)) != NULL) {
        if (!strstr(entry->d_name, "highlight-js-llvm-js") || !strstr(entry->d_name, ".js")) {
            continue;
        }

        char path[REPL_MAX_INPUT];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        if (parse_highlight_js_file(path, list)) {
            ok = true;
            break;
        }
    }

    closedir(dir);
    return ok;
}

static bool load_ir_instruction_list(ir_instruction_list_t *list) {
    char path[REPL_MAX_INPUT];
    char llvm_include[REPL_MAX_INPUT];

    if (capture_command_line("llvm-config --includedir 2>/dev/null", llvm_include, sizeof(llvm_include))) {
        snprintf(path, sizeof(path), "%s/llvm/IR/Instruction.def", llvm_include);
        if (add_instruction_def_candidate(path, list)) {
            sort_ir_instruction_list(list);
            return true;
        }
    }

    char resource_dir[REPL_MAX_INPUT];
    if (capture_command_line("clang -print-resource-dir 2>/dev/null", resource_dir, sizeof(resource_dir))) {
        snprintf(path, sizeof(path), "%s/include/llvm/IR/Instruction.def", resource_dir);
        if (add_instruction_def_candidate(path, list)) {
            sort_ir_instruction_list(list);
            return true;
        }

        snprintf(path, sizeof(path), "%s/../../include/llvm/IR/Instruction.def", resource_dir);
        if (add_instruction_def_candidate(path, list)) {
            sort_ir_instruction_list(list);
            return true;
        }

        char resource_prefix[REPL_MAX_INPUT];
        snprintf(resource_prefix, sizeof(resource_prefix), "%s", resource_dir);
        char *lib_clang = strstr(resource_prefix, "/lib/clang/");
        if (lib_clang) {
            *lib_clang = '\0';
            snprintf(path, sizeof(path), "%s/include/llvm/IR/Instruction.def", resource_prefix);
            if (add_instruction_def_candidate(path, list)) {
                sort_ir_instruction_list(list);
                return true;
            }

            snprintf(path, sizeof(path), "%s/share/docc/render/js", resource_prefix);
            if (parse_highlight_js_dir(path, list)) {
                sort_ir_instruction_list(list);
                return true;
            }
        }
    }

    char compiler_path[REPL_MAX_INPUT];
    if (resolve_executable_path("clang", compiler_path, sizeof(compiler_path))) {
        char *bin = strstr(compiler_path, "/bin/clang");
        if (bin) {
            *bin = '\0';
            snprintf(path, sizeof(path), "%s/include/llvm/IR/Instruction.def", compiler_path);
            if (add_instruction_def_candidate(path, list)) {
                sort_ir_instruction_list(list);
                return true;
            }

            snprintf(path, sizeof(path), "%s/share/docc/render/js", compiler_path);
            if (parse_highlight_js_dir(path, list)) {
                sort_ir_instruction_list(list);
                return true;
            }
        }
    }

    return false;
}

static const ir_instruction_info_t *find_ir_instruction_info(const ir_instruction_list_t *list, const char *name) {
    for (size_t i = 0; i < list->len; i++) {
        if (strcmp(list->items[i].name, name) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}

static void print_ir_instruction_reference(const char *instruction) {
    printf("https://llvm.org/docs/LangRef.html#%s-instruction\n", instruction);
}

static void print_ir_instruction_generated_help(const ir_instruction_info_t *info, const char *source) {
    printf("%s - %s\n", info->name, info->summary);
    puts("");
    puts("Reference:");
    print_ir_instruction_reference(info->name);
    puts("");
    puts("Notes:");
    printf("Discovered from %s.\n", source);
}

static void print_ir_instruction_list(void) {
    ir_instruction_list_t list;
    ir_instruction_list_init(&list);

    if (!load_ir_instruction_list(&list)) {
        puts("Could not discover LLVM IR instructions from the installed LLVM/Clang toolchain.");
        puts("Tried llvm-config include paths, clang resource include paths, and bundled LLVM syntax assets.");
        ir_instruction_list_free(&list);
        return;
    }

    printf("LLVM IR instructions discovered from %s:\n", list.source);
    for (size_t i = 0; i < list.len; i++) {
        printf("  %-18s %-48s https://llvm.org/docs/LangRef.html#%s-instruction\n",
               list.items[i].name, list.items[i].summary, list.items[i].name);
    }
    puts("");
    puts("This list is derived from the installed compiler/toolchain, not a checked-in opcode table.");
    puts("Use :help <instruction> for focused help on one instruction.");

    ir_instruction_list_free(&list);
}

static const topic_help_t *topics_for_mode(repl_mode_t mode, size_t *count) {
    switch (mode) {
        case MODE_C:
            *count = sizeof(c_topics) / sizeof(c_topics[0]);
            return c_topics;
        case MODE_CPP:
            *count = sizeof(cpp_topics) / sizeof(cpp_topics[0]);
            return cpp_topics;
        case MODE_OBJC:
            *count = sizeof(objc_topics) / sizeof(objc_topics[0]);
            return objc_topics;
        case MODE_LLVMIR:
            *count = sizeof(ir_topics) / sizeof(ir_topics[0]);
            return ir_topics;
    }
    *count = 0;
    return NULL;
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

static const topic_help_t *find_topic(repl_mode_t mode, const char *query) {
    size_t count = 0;
    const topic_help_t *topics = topics_for_mode(mode, &count);
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(topics[i].topic, query) == 0 ||
            alias_matches(topics[i].aliases, query)) {
            return &topics[i];
        }
    }
    return NULL;
}

static const topic_help_t *find_exact_topic(repl_mode_t mode, const char *query) {
    size_t count = 0;
    const topic_help_t *topics = topics_for_mode(mode, &count);
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(topics[i].topic, query) == 0) {
            return &topics[i];
        }
    }
    return NULL;
}

static bool extract_question_query(const char *line, char *query, size_t query_size) {
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
    while (p < end && !isspace((unsigned char)*p) && *p != '(' && *p != '<' && *p != '?') {
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

static void print_topic_help(repl_mode_t mode, const char *query) {
    const topic_help_t *help = find_exact_topic(mode, query);
    if (!help && mode == MODE_LLVMIR) {
        ir_instruction_list_t list;
        ir_instruction_list_init(&list);
        if (load_ir_instruction_list(&list)) {
            const ir_instruction_info_t *info = find_ir_instruction_info(&list, query);
            if (info) {
                print_ir_instruction_generated_help(info, list.source);
                ir_instruction_list_free(&list);
                return;
            }
        }
        ir_instruction_list_free(&list);
    }

    if (!help) {
        help = find_topic(mode, query);
    }
    if (!help) {
        if (mode == MODE_LLVMIR) {
            ir_instruction_list_t list;
            ir_instruction_list_init(&list);
            if (load_ir_instruction_list(&list)) {
                const ir_instruction_info_t *info = find_ir_instruction_info(&list, query);
                if (info) {
                    print_ir_instruction_generated_help(info, list.source);
                    ir_instruction_list_free(&list);
                    return;
                }
            }
            ir_instruction_list_free(&list);
        }
        printf("No built-in help for '%s' in %s-repl.\n", query, mode_name(mode));
        puts("Use :topics to list available help topics.");
        return;
    }

    printf("%s - %s\n", help->topic, help->summary);
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

    if (mode == MODE_LLVMIR) {
        ir_instruction_list_t list;
        ir_instruction_list_init(&list);
        if (load_ir_instruction_list(&list)) {
            const ir_instruction_info_t *info = find_ir_instruction_info(&list, query);
            if (!info) {
                info = find_ir_instruction_info(&list, help->topic);
            }
            if (info) {
                puts("");
                puts("Reference:");
                print_ir_instruction_reference(info->name);
            }
        }
        ir_instruction_list_free(&list);
    }
}

static void print_topic_list(repl_mode_t mode) {
    size_t count = 0;
    const topic_help_t *topics = topics_for_mode(mode, &count);
    printf("Built-in help topics for %s-repl:\n", mode_name(mode));
    for (size_t i = 0; i < count; i++) {
        printf("  %-14s %s\n", topics[i].topic, topics[i].summary);
        if (topics[i].aliases) {
            printf("                 aliases/forms: %s\n", topics[i].aliases);
        }
    }
    puts("");
    puts("Type <topic>? for details, for example state?.");
}

static void print_help(repl_mode_t mode) {
    char compiler_path[REPL_MAX_INPUT];
    resolve_executable_path(compiler_command(mode), compiler_path, sizeof(compiler_path));

    printf("%s REPL. Type snippets and inspect persistent state.\n", mode_title(mode));
    printf("Compiler: %s\n", compiler_path);
    puts("");
    puts("Commands:");
    puts("  :help              show this help");
    puts("  :help <topic>      show built-in help for a topic");
    puts("  :topics            list built-in help topics");
    if (mode == MODE_LLVMIR) {
        puts("  :instructions      discover LLVM IR instructions from the toolchain");
    }
    puts("  :state             print persistent REPL state");
    puts("  :reset             reset persistent REPL state");
    puts("  :scratch           print scratch memory size and first bytes");
    puts("  :defs              print persisted definitions");
    puts("  :def               start a persisted definition block");
    puts("  :end               commit the current definition block");
    puts("  :clear             clear definitions and LLVM IR body");
    puts("  :source            print the last generated source/IR file path");
    puts("  :quit              exit");
    puts("");
    puts("Help topics:");
    puts("  Add ? after a topic, for example state? or store?.");
    puts("");
    if (mode_is_statement_repl(mode)) {
        puts("Execution model:");
        puts("  Normal input is compiled inside void repl_entry(repl_state_t *state).");
        puts("  Multi-line input continues until the compiler accepts it.");
        puts("  Press Enter on an empty continuation line to force diagnostics.");
        puts("  Accepted top-level definitions are persisted; accepted statements run.");
        puts("  Lines beginning with # are persisted as preprocessor directives.");
        puts("  :def ... :end is still available when you want explicit definition mode.");
        puts("  Persistent helpers: U(n), F(n), SCRATCH(n), print(...), state->result.");
    } else if (mode == MODE_LLVMIR) {
        puts("Execution model:");
        puts("  Each non-command line is appended to the current LLVM IR function body.");
        puts("  The whole body is recompiled and executed after each appended line.");
        puts("  The entry block receives ptr %state, whose type is %repl_state.");
    }
    puts("");
    puts("Safety:");
    puts("  Generated code runs in this process through a shared library.");
    puts("  Crashes, infinite loops, invalid memory writes, and unsafe calls are not sandboxed.");
}

static bool write_common_c_header(FILE *fp, repl_mode_t mode) {
    if (mode == MODE_CPP) {
        fputs("#include <cstdarg>\n#include <cstdint>\n#include <cstdio>\n#include <cstring>\n#include <cstdlib>\n\n", fp);
        fputs("extern \"C\" {\n", fp);
    } else if (mode == MODE_OBJC) {
        fputs("#import <Foundation/Foundation.h>\n", fp);
        fputs("#include <stdarg.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n#include <stdlib.h>\n\n", fp);
    } else {
        fputs("#include <stdarg.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n#include <stdlib.h>\n\n", fp);
    }

    fputs("#define REPL_SLOT_COUNT 16\n", fp);
    fputs("#define REPL_SCRATCH_SIZE 4096\n", fp);
    fputs("#define REPL_OUT_SIZE 4096\n", fp);
    fputs("typedef struct {\n", fp);
    fputs("    uint64_t u64[REPL_SLOT_COUNT];\n", fp);
    fputs("    double f64[REPL_SLOT_COUNT];\n", fp);
    fputs("    unsigned char scratch[REPL_SCRATCH_SIZE];\n", fp);
    fputs("    char out[REPL_OUT_SIZE];\n", fp);
    fputs("    uint64_t result;\n", fp);
    fputs("} repl_state_t;\n", fp);
    if (mode == MODE_CPP) {
        fputs("}\n\n", fp);
    }

    fputs("#define U(n) (state->u64[(n)])\n", fp);
    fputs("#define F(n) (state->f64[(n)])\n", fp);
    fputs("#define SCRATCH(n) (state->scratch[(n)])\n\n", fp);
    fputs("static void __attribute__((unused)) repl_print(repl_state_t *state, const char *fmt, ...) {\n", fp);
    fputs("    size_t used = strlen(state->out);\n", fp);
    fputs("    if (used >= REPL_OUT_SIZE - 1) return;\n", fp);
    fputs("    va_list ap;\n", fp);
    fputs("    va_start(ap, fmt);\n", fp);
    fputs("    vsnprintf(state->out + used, REPL_OUT_SIZE - used, fmt, ap);\n", fp);
    fputs("    va_end(ap);\n", fp);
    fputs("}\n", fp);
    fputs("#define print(...) repl_print(state, __VA_ARGS__)\n\n", fp);
    return ferror(fp) == 0;
}

static bool write_statement_source(const char *path, repl_mode_t mode, const char *definitions, const char *line) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return false;
    }

    write_common_c_header(fp, mode);
    if (definitions && definitions[0] != '\0') {
        fputs("\n/* persisted REPL definitions */\n", fp);
        fputs(definitions, fp);
        if (definitions[strlen(definitions) - 1] != '\n') {
            fputc('\n', fp);
        }
    }

    if (mode == MODE_CPP) {
        fputs("\nextern \"C\" void repl_entry(repl_state_t *state) {\n", fp);
        fputs("    state->out[0] = '\\0';\n", fp);
        fprintf(fp, "    %s\n", line);
        fputs("}\n", fp);
    } else if (mode == MODE_OBJC) {
        fputs("\nvoid repl_entry(repl_state_t *state) {\n", fp);
        fputs("    state->out[0] = '\\0';\n", fp);
        fputs("    @autoreleasepool {\n", fp);
        fprintf(fp, "        %s\n", line);
        fputs("    }\n", fp);
        fputs("}\n", fp);
    } else {
        fputs("\nvoid repl_entry(repl_state_t *state) {\n", fp);
        fputs("    state->out[0] = '\\0';\n", fp);
        fprintf(fp, "    %s\n", line);
        fputs("}\n", fp);
    }

    if (fclose(fp) != 0) {
        perror(path);
        return false;
    }
    return true;
}

static bool write_definition_probe_source(const char *path, repl_mode_t mode,
                                          const char *definitions, const char *candidate) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return false;
    }

    write_common_c_header(fp, mode);
    if (definitions && definitions[0] != '\0') {
        fputs("\n/* persisted REPL definitions */\n", fp);
        fputs(definitions, fp);
        if (definitions[strlen(definitions) - 1] != '\n') {
            fputc('\n', fp);
        }
    }

    fputs("\n/* candidate REPL definition */\n", fp);
    fputs(candidate, fp);
    size_t candidate_len = strlen(candidate);
    if (candidate_len == 0 || candidate[candidate_len - 1] != '\n') {
        fputc('\n', fp);
    }

    if (mode == MODE_CPP) {
        fputs("\nextern \"C\" void repl_entry(repl_state_t *state) {\n", fp);
        fputs("    state->out[0] = '\\0';\n", fp);
        fputs("    (void)state;\n", fp);
        fputs("}\n", fp);
    } else if (mode == MODE_OBJC) {
        fputs("\nvoid repl_entry(repl_state_t *state) {\n", fp);
        fputs("    state->out[0] = '\\0';\n", fp);
        fputs("    (void)state;\n", fp);
        fputs("}\n", fp);
    } else {
        fputs("\nvoid repl_entry(repl_state_t *state) {\n", fp);
        fputs("    state->out[0] = '\\0';\n", fp);
        fputs("    (void)state;\n", fp);
        fputs("}\n", fp);
    }

    if (fclose(fp) != 0) {
        perror(path);
        return false;
    }
    return true;
}

static bool write_llvm_source(const char *path, const char *definitions, const char *body) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return false;
    }

    fputs("; generated by llvmir-repl\n", fp);
    fputs("%repl_state = type { [16 x i64], [16 x double], [4096 x i8], [4096 x i8], i64 }\n\n", fp);
    if (definitions && definitions[0] != '\0') {
        fputs("; persisted REPL definitions\n", fp);
        fputs(definitions, fp);
        if (definitions[strlen(definitions) - 1] != '\n') {
            fputc('\n', fp);
        }
        fputc('\n', fp);
    }
    fputs("define void @repl_entry(ptr %state) {\n", fp);
    fputs("entry:\n", fp);
    if (body && body[0] != '\0') {
        fputs(body, fp);
        if (body[strlen(body) - 1] != '\n') {
            fputc('\n', fp);
        }
    }
    fputs("  ret void\n", fp);
    fputs("}\n", fp);

    if (fclose(fp) != 0) {
        perror(path);
        return false;
    }
    return true;
}

static bool llvm_opaque_pointer_flag_supported(void) {
    static int supported = -1;
    if (supported >= 0) {
        return supported == 1;
    }

    ensure_build_dir();

    char source_path[REPL_MAX_INPUT];
    char object_path[REPL_MAX_INPUT];
    snprintf(source_path, sizeof(source_path), "%s/llvm-opaque-probe-%ld.ll", BUILD_DIR, (long)getpid());
    snprintf(object_path, sizeof(object_path), "%s/llvm-opaque-probe-%ld.o", BUILD_DIR, (long)getpid());

    FILE *fp = fopen(source_path, "w");
    if (!fp) {
        supported = 0;
        return false;
    }
    fputs("define void @f(ptr %p) {\n  ret void\n}\n", fp);
    if (fclose(fp) != 0) {
        unlink(source_path);
        supported = 0;
        return false;
    }

    char *const argv[] = {
        "clang", "-Wno-override-module", "-mllvm", "-opaque-pointers",
        "-c", source_path, "-o", object_path, NULL,
    };
    supported = run_command_with_stdio(argv, true) == 0 ? 1 : 0;
    unlink(source_path);
    unlink(object_path);
    return supported == 1;
}

static bool compile_shared(repl_mode_t mode, const char *source_path, const char *library_path, bool quiet) {
#ifdef __APPLE__
    const char *shared_flag = "-dynamiclib";
#else
    const char *shared_flag = "-shared";
#endif

    if (mode == MODE_C) {
        char *const argv[] = {
            "clang", "-std=c17", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-fPIC", (char *)shared_flag, (char *)source_path, "-o", (char *)library_path, NULL,
        };
        int status = run_command_with_stdio(argv, quiet);
        if (status != 0) {
            if (!quiet) {
                fprintf(stderr, "c compilation failed with exit code %d\n", status);
            }
            return false;
        }
        return true;
    }

    if (mode == MODE_CPP) {
        char *const argv[] = {
            "clang++", "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-fPIC", (char *)shared_flag, (char *)source_path, "-o", (char *)library_path, NULL,
        };
        int status = run_command_with_stdio(argv, quiet);
        if (status != 0) {
            if (!quiet) {
                fprintf(stderr, "c++ compilation failed with exit code %d\n", status);
            }
            return false;
        }
        return true;
    }

    if (mode == MODE_OBJC) {
#ifndef __APPLE__
        if (!quiet) {
            fprintf(stderr, "objc-repl is only supported on macOS in this build.\n");
        }
        return false;
#else
        char *const argv[] = {
            "clang", "-x", "objective-c", "-fobjc-arc", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-fPIC", (char *)shared_flag,
            (char *)source_path, "-framework", "Foundation", "-o", (char *)library_path, NULL,
        };
        int status = run_command_with_stdio(argv, quiet);
        if (status != 0) {
            if (!quiet) {
                fprintf(stderr, "objective-c compilation failed with exit code %d\n", status);
            }
            return false;
        }
        return true;
#endif
    }

    if (mode == MODE_LLVMIR) {
        int status = 0;
        if (llvm_opaque_pointer_flag_supported()) {
            char *const argv[] = {
                "clang", "-Wno-override-module", "-mllvm", "-opaque-pointers",
                "-fPIC", (char *)shared_flag, (char *)source_path, "-o", (char *)library_path, NULL,
            };
            status = run_command_with_stdio(argv, quiet);
        } else {
            char *const argv[] = {
                "clang", "-Wno-override-module", "-fPIC", (char *)shared_flag,
                (char *)source_path, "-o", (char *)library_path, NULL,
            };
            status = run_command_with_stdio(argv, quiet);
        }
        if (status != 0) {
            if (!quiet) {
                fprintf(stderr, "llvm ir compilation failed with exit code %d\n", status);
            }
            return false;
        }
        return true;
    }

    return false;
}

static bool load_and_run(const char *library_path, repl_state_t *state) {
    void *handle = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return false;
    }

    dlerror();
    repl_entry_fn entry = (repl_entry_fn)dlsym(handle, "repl_entry");
    const char *error = dlerror();
    if (error) {
        fprintf(stderr, "dlsym: %s\n", error);
        dlclose(handle);
        return false;
    }

    entry(state);
    dlclose(handle);
    return true;
}

static bool build_and_run_statement(repl_mode_t mode, const char *definitions, const char *line,
                                    unsigned long serial, repl_state_t *state,
                                    char *last_source, size_t last_source_size) {
    char source_path[256];
    char library_path[256];
    snprintf(source_path, sizeof(source_path), BUILD_DIR "/%s-%ld-%lu%s",
             mode_name(mode), (long)getpid(), serial, mode_extension(mode));
    snprintf(library_path, sizeof(library_path), BUILD_DIR "/%s-%ld-%lu%s",
             mode_name(mode), (long)getpid(), serial, SHARED_EXT);

    if (!write_statement_source(source_path, mode, definitions, line)) {
        return false;
    }
    snprintf(last_source, last_source_size, "%s", source_path);

    if (!compile_shared(mode, source_path, library_path, false)) {
        return false;
    }

    return load_and_run(library_path, state);
}

static bool probe_statement_compile(repl_mode_t mode, const char *definitions,
                                    const char *line, unsigned long serial) {
    char source_path[256];
    char library_path[256];
    snprintf(source_path, sizeof(source_path), BUILD_DIR "/probe-stmt-%s-%ld-%lu%s",
             mode_name(mode), (long)getpid(), serial, mode_extension(mode));
    snprintf(library_path, sizeof(library_path), BUILD_DIR "/probe-stmt-%s-%ld-%lu%s",
             mode_name(mode), (long)getpid(), serial, SHARED_EXT);

    if (!write_statement_source(source_path, mode, definitions, line)) {
        return false;
    }
    return compile_shared(mode, source_path, library_path, true);
}

static bool probe_definition_compile(repl_mode_t mode, const char *definitions,
                                     const char *candidate, unsigned long serial) {
    char source_path[256];
    char library_path[256];
    snprintf(source_path, sizeof(source_path), BUILD_DIR "/probe-def-%s-%ld-%lu%s",
             mode_name(mode), (long)getpid(), serial, mode_extension(mode));
    snprintf(library_path, sizeof(library_path), BUILD_DIR "/probe-def-%s-%ld-%lu%s",
             mode_name(mode), (long)getpid(), serial, SHARED_EXT);

    if (!write_definition_probe_source(source_path, mode, definitions, candidate)) {
        return false;
    }
    return compile_shared(mode, source_path, library_path, true);
}

static void commit_definition_text(text_buffer_t *definitions, const char *text) {
    if (definitions->len > 0 && definitions->data[definitions->len - 1] != '\n') {
        text_buffer_append(definitions, "\n");
    }
    text_buffer_append(definitions, text);
    if (definitions->len > 0 && definitions->data[definitions->len - 1] != '\n') {
        text_buffer_append(definitions, "\n");
    }
}

static bool finish_pending_statement(repl_mode_t mode, text_buffer_t *definitions,
                                     text_buffer_t *pending, unsigned long *serial,
                                     repl_state_t *state, char *last_source,
                                     size_t last_source_size, bool force_diagnostics) {
    if (pending->len == 0) {
        return true;
    }

    if (probe_definition_compile(mode, definitions->data, pending->data, *serial)) {
        commit_definition_text(definitions, pending->data);
        text_buffer_clear(pending);
        puts("definition block committed");
        return true;
    }

    if (probe_statement_compile(mode, definitions->data, pending->data, *serial)) {
        if (build_and_run_statement(mode, definitions->data, pending->data, (*serial)++,
                                    state, last_source, last_source_size)) {
            print_state(state);
        }
        text_buffer_clear(pending);
        return true;
    }

    if (force_diagnostics) {
        (void)build_and_run_statement(mode, definitions->data, pending->data, (*serial)++,
                                      state, last_source, last_source_size);
        text_buffer_clear(pending);
        return true;
    }

    return false;
}

static bool build_and_run_llvm(const char *definitions, const char *body, unsigned long serial,
                               repl_state_t *state, char *last_source, size_t last_source_size) {
    char source_path[256];
    char library_path[256];
    snprintf(source_path, sizeof(source_path), BUILD_DIR "/ir-%ld-%lu.ll", (long)getpid(), serial);
    snprintf(library_path, sizeof(library_path), BUILD_DIR "/ir-%ld-%lu%s", (long)getpid(), serial, SHARED_EXT);

    if (!write_llvm_source(source_path, definitions, body)) {
        return false;
    }
    snprintf(last_source, last_source_size, "%s", source_path);

    if (!compile_shared(MODE_LLVMIR, source_path, library_path, false)) {
        return false;
    }

    return load_and_run(library_path, state);
}

static bool starts_with_preprocessor_directive(const char *line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return *line == '#';
}

static repl_mode_t parse_mode(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0) {
            if (strcmp(argv[i + 1], "c") == 0) return MODE_C;
            if (strcmp(argv[i + 1], "cpp") == 0) return MODE_CPP;
            if (strcmp(argv[i + 1], "objc") == 0) return MODE_OBJC;
            if (strcmp(argv[i + 1], "llvmir") == 0 || strcmp(argv[i + 1], "ir") == 0) return MODE_LLVMIR;
        }
    }
    return MODE_C;
}

int main(int argc, char **argv) {
    repl_mode_t mode = parse_mode(argc, argv);

#ifndef __APPLE__
    if (mode == MODE_OBJC) {
        fputs("objc-repl is only supported on macOS arm64. Objective-C snippets need the Apple Objective-C runtime and Foundation framework.\n", stderr);
        return 1;
    }
#endif

    ensure_build_dir();

    repl_state_t state;
    reset_state(&state);

    text_buffer_t definitions;
    text_buffer_t block;
    text_buffer_t pending_statement;
    text_buffer_t ir_body;
    text_buffer_init(&definitions);
    text_buffer_init(&block);
    text_buffer_init(&pending_statement);
    text_buffer_init(&ir_body);
    bool in_def_block = false;

    char last_source[256] = "";
    unsigned long serial = 1;

    printf("%s REPL. Type :help for commands.\n", mode_title(mode));
    print_help(mode);

    char input[REPL_MAX_INPUT];
    for (;;) {
        bool in_multiline_statement = mode_is_statement_repl(mode) && pending_statement.len > 0;
        printf("%s%c ", mode_name(mode), (in_def_block || in_multiline_statement) ? '|' : '>');
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) {
            putchar('\n');
            break;
        }

        char raw_line[REPL_MAX_INPUT];
        snprintf(raw_line, sizeof(raw_line), "%s", input);
        size_t raw_len = strlen(raw_line);
        while (raw_len > 0 && (raw_line[raw_len - 1] == '\n' || raw_line[raw_len - 1] == '\r')) {
            raw_line[--raw_len] = '\0';
        }

        char code_line[REPL_MAX_INPUT];
        snprintf(code_line, sizeof(code_line), "%s", raw_line);
        bool stripped_comment = strip_repl_inline_comment(mode, code_line);

        char *line = trim(code_line);
        if (*line == '\0') {
            if (in_def_block) {
                text_buffer_append_line(&block, "");
            } else if (stripped_comment && mode_is_statement_repl(mode) && pending_statement.len > 0) {
                continue;
            } else if (mode_is_statement_repl(mode) && pending_statement.len > 0) {
                finish_pending_statement(mode, &definitions, &pending_statement, &serial,
                                         &state, last_source, sizeof(last_source), true);
            }
            continue;
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
                print_help(mode);
            } else {
                print_topic_help(mode, help_arg);
            }
            continue;
        }

        if (strcmp(line, ":topics") == 0 || strcmp(line, ":topic") == 0 || strcmp(line, ":instructions") == 0) {
            if (strcmp(line, ":instructions") == 0 && mode == MODE_LLVMIR) {
                print_ir_instruction_list();
            } else {
                print_topic_list(mode);
            }
            continue;
        }

        if (strcmp(line, ":state") == 0 || strcmp(line, ":s") == 0) {
            print_state(&state);
            continue;
        }

        if (strcmp(line, ":reset") == 0) {
            reset_state(&state);
            puts("state reset");
            print_state(&state);
            continue;
        }

        if (strcmp(line, ":scratch") == 0) {
            printf("scratch size: %d bytes\n", SCRATCH_SIZE);
            fputs("scratch[0..31]", stdout);
            for (int i = 0; i < 32; i++) {
                printf(" %02x", state.scratch[i]);
            }
            putchar('\n');
            continue;
        }

        if (strcmp(line, ":defs") == 0) {
            if (definitions.len == 0 && block.len == 0 && pending_statement.len == 0) {
                puts("(no definitions)");
            } else {
                if (definitions.len > 0) {
                    fputs(definitions.data, stdout);
                }
                if (block.len > 0) {
                    fputs(block.data, stdout);
                }
                if (pending_statement.len > 0) {
                    fputs(pending_statement.data, stdout);
                }
            }
            continue;
        }

        if (strcmp(line, ":body") == 0) {
            if (ir_body.len == 0) {
                puts("(empty body)");
            } else {
                fputs(ir_body.data, stdout);
            }
            continue;
        }

        if (strcmp(line, ":source") == 0 || strcmp(line, ":src") == 0) {
            puts(last_source[0] ? last_source : "(no generated source yet)");
            continue;
        }

        if (strcmp(line, ":clear") == 0) {
            text_buffer_clear(&definitions);
            text_buffer_clear(&block);
            text_buffer_clear(&pending_statement);
            text_buffer_clear(&ir_body);
            in_def_block = false;
            puts("definitions and LLVM IR body cleared");
            continue;
        }

        if (strcmp(line, ":def") == 0) {
            text_buffer_clear(&block);
            in_def_block = true;
            puts("definition block started; finish with :end");
            continue;
        }

        if (strcmp(line, ":end") == 0 || strcmp(line, ".end") == 0) {
            if (!in_def_block) {
                puts("not in a definition block");
                continue;
            }
            if (definitions.len > 0 && definitions.data[definitions.len - 1] != '\n') {
                text_buffer_append(&definitions, "\n");
            }
            text_buffer_append(&definitions, block.data);
            if (definitions.len > 0 && definitions.data[definitions.len - 1] != '\n') {
                text_buffer_append(&definitions, "\n");
            }
            text_buffer_clear(&block);
            in_def_block = false;
            puts("definition block committed");
            continue;
        }

        char query[64];
        if (extract_question_query(line, query, sizeof(query))) {
            print_topic_help(mode, query);
            continue;
        }

        if (in_def_block) {
            text_buffer_append_line(&block, code_line);
            continue;
        }

        if (starts_with_preprocessor_directive(code_line) && mode_is_statement_repl(mode) &&
            pending_statement.len == 0) {
            text_buffer_append_line(&definitions, code_line);
            puts("directive persisted");
            continue;
        }

        if (mode_is_statement_repl(mode)) {
            text_buffer_append_line(&pending_statement, code_line);
            finish_pending_statement(mode, &definitions, &pending_statement, &serial,
                                     &state, last_source, sizeof(last_source), false);
        } else if (mode == MODE_LLVMIR) {
            text_buffer_append_line(&ir_body, code_line);
            if (build_and_run_llvm(definitions.data, ir_body.data, serial++, &state,
                                   last_source, sizeof(last_source))) {
                print_state(&state);
            } else {
                size_t code_line_len = strlen(code_line);
                if (ir_body.len >= code_line_len + 1) {
                    ir_body.len -= code_line_len + 1;
                    ir_body.data[ir_body.len] = '\0';
                }
            }
        }
    }

    text_buffer_free(&definitions);
    text_buffer_free(&block);
    text_buffer_free(&pending_statement);
    text_buffer_free(&ir_body);
    return 0;
}
