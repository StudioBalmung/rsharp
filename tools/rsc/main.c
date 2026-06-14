/* rsharp/tools/rsc/main.c
 * R# Compiler Driver v1.1.0  (C11)
 *
 * Full pipeline:
 *   source.rsl
 *     -> Lex   (compiler/lexer)
 *     -> Parse (compiler/parser)
 *     -> Sema  (compiler/sema)      — type checking
 *     -> Own   (compiler/ownership) — borrow/move checking
 *     -> LLVM  (compiler/codegen)   — IR emission + native .o
 *     -> Link  (lld / ld)           — produce final binary
 *
 * Usage:
 *   rsc <file.rsl> [options]
 *   rsc --version
 *
 * Options:
 *   -o <out>          Output binary path      (default: ./a.out)
 *   --emit-ir         Also write <out>.ll      LLVM IR text
 *   --emit-asm        Also write <out>.s       assembly
 *   -O0 / -O1 / -O2 / -Os   Optimisation level
 *   --release         Equivalent to -O2, strip asserts
 *   --check           Type+borrow check only, no codegen
 *   --test            Compile with test harness entry point
 *   --target <t>      Cross-compile triple (x86_64-linux-gnu, etc.)
 *   --verbose / -v    Print each pipeline stage
 *   --version         Print version and exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdbool.h>
#include "../../compiler/compat/platform.h"

#include "../../compiler/lexer/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/sema/sema.h"
#include "../../compiler/ownership/ownership.h"
#include "../../compiler/ownership/ownership.h"
#include "../../compiler/error/diagnostic.h"
#include "../../runtime/memory/arena.h"

#ifdef RSHARP_WITH_LLVM
#include "../../compiler/codegen/llvm_backend.h"
#endif

#define RSC_VERSION "1.1.0"

/* ── CLI options ─────────────────────────────────────────────────── */
typedef struct {
    const char  *output;
    const char  *target;
    const char  *cpu;
    int          opt_level;     /* 0=debug 1=O1 2=O2 3=size */
    bool         emit_ir;
    bool         emit_asm;
    bool         check_only;
    bool         verbose;
    bool         release;
    bool         test_mode;
    bool         lsp_mode;
    const char **inputs;
    int          input_count;
} Options;

static void usage(void) {
    fprintf(stderr,
        "rsc v" RSC_VERSION " — R# Compiler\n\n"
        "USAGE:\n"
        "  rsc [OPTIONS] <file.rsl> [<file.rsl> ...]\n\n"
        "OPTIONS:\n"
        "  -o <path>        Output binary  (default: a.out)\n"
        "  --check          Type+borrow check only, no codegen\n"
        "  --emit-ir        Write LLVM IR  (<out>.ll)\n"
        "  --emit-asm       Write assembly (<out>.s)\n"
        "  --release        -O2 + strip debug assertions\n"
        "  -O0/-O1/-O2/-Os  Optimisation level\n"
        "  --target <t>     Cross-compile target triple\n"
        "  --cpu <cpu>      Target CPU (default: generic)\n"
        "  --verbose / -v   Verbose pipeline output\n"
        "  --version        Print version\n"
        "  -h / --help      Show this help\n\n"
#ifdef RSHARP_WITH_LLVM
        "LLVM backend: ENABLED\n"
#else
        "LLVM backend: disabled (rebuild with -DRSHARP_WITH_LLVM=ON)\n"
#endif
    );
}

static Options parse_args(int argc, char **argv) {
    Options o = {
        .output      = "a.out",
        .cpu         = "generic",
        .inputs      = malloc(sizeof(char *) * (size_t)(argc + 1)),
    };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
            printf("rsc v" RSC_VERSION
#ifdef RSHARP_WITH_LLVM
                   " (LLVM backend enabled)"
#endif
                   "\n");
            exit(0);
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))  { usage(); exit(0); }
        if (!strcmp(a, "--check"))    { o.check_only = true;  continue; }
        if (!strcmp(a, "--emit-ir"))  { o.emit_ir    = true;  continue; }
        if (!strcmp(a, "--emit-asm")) { o.emit_asm   = true;  continue; }
        if (!strcmp(a, "--release"))  { o.release    = true; o.opt_level = 2; continue; }
        if (!strcmp(a, "--verbose") || !strcmp(a, "-v")) { o.verbose = true; continue; }
        if (!strcmp(a, "--test"))     { o.test_mode  = true;  continue; }
        if (!strcmp(a, "--lsp"))      { o.lsp_mode   = true;  continue; }
        if (!strcmp(a, "-O0"))        { o.opt_level  = 0;     continue; }
        if (!strcmp(a, "-O1"))        { o.opt_level  = 1;     continue; }
        if (!strcmp(a, "-O2"))        { o.opt_level  = 2;     continue; }
        if (!strcmp(a, "-Os"))        { o.opt_level  = 3;     continue; }
        if (!strcmp(a, "-o")       && i+1 < argc) { o.output = argv[++i]; continue; }
        if (!strcmp(a, "--target") && i+1 < argc) { o.target = argv[++i]; continue; }
        if (!strcmp(a, "--cpu")    && i+1 < argc) { o.cpu    = argv[++i]; continue; }
        if (a[0] == '-') {
            fprintf(stderr, "rsc: unknown option '%s'\n", a);
            exit(1);
        }
        o.inputs[o.input_count++] = a;
    }
    if (!o.check_only && !o.lsp_mode && o.input_count == 0) {
        fprintf(stderr, "rsc: no input files\n");
        usage(); exit(1);
    }
    return o;
}

/* ── File reader ─────────────────────────────────────────────────── */
static char *read_file(const char *path, size_t *out_len, Arena *arena) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "rsc: cannot open '%s': no such file\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = arena_alloc(arena, (size_t)sz + 1, 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* ── Stage helpers ───────────────────────────────────────────────── */
static void stage(const Options *o, const char *name, const char *file) {
    if (o->verbose)
        fprintf(stderr, "  \033[36m[%s]\033[0m  %s\n", name, file);
}

/* ── Linker invocation ───────────────────────────────────────────── */
static int link_object(const char *obj_path, const char *out_path,
                       const char *target, bool verbose) {
    char cmd[2048];
    /* Try lld first (LLVM's fast linker), fall back to cc */
    const char *linker =
        (system("which ld.lld > /dev/null 2>&1") == 0) ? "ld.lld" :
        (system("which clang > /dev/null 2>&1")  == 0) ? "clang"  :
        "cc";

    if (target && strlen(target) > 0 && strcmp(target, "native") != 0) {
        /* Cross-link */
        snprintf(cmd, sizeof cmd,
            "%s --target=%s -o '%s' '%s' 2>&1",
            linker, target, out_path, obj_path);
    } else {
        /* Native link — also link runtime helpers */
        snprintf(cmd, sizeof cmd,
            "%s -o '%s' '%s' -lm 2>&1",
            linker, out_path, obj_path);
    }

    if (verbose) fprintf(stderr, "  \033[36m[link]\033[0m %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "rsc: link failed (exit %d)\n", rc);
    return rc;
}

/* ── Runtime helper source (compiled alongside user code) ─────────── */
/* Provides rsharp_print, rsharp_assert, rsharp_panic */
static const char *RUNTIME_C =
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <stdint.h>\n"
    "#include <string.h>\n"
    "void rsharp_print(const char *ptr, int64_t len, ...) {\n"
    "    fwrite(ptr, 1, (size_t)len, stdout); fputc('\\n', stdout); }\n"
    "void rsharp_assert(int cond, const char *msg) {\n"
    "    if (!cond) { fprintf(stderr, \"assertion failed: %s\\n\", msg ? msg : \"\");\n"
    "                 exit(1); } }\n"
    "void rsharp_panic(const char *msg) {\n"
    "    fprintf(stderr, \"panic: %s\\n\", msg ? msg : \"explicit panic\");\n"
    "    exit(101); }\n";

static int compile_runtime(const char *obj_out) {
    /* Write temp C file and compile it */
    char tmp_c[256], cmd[1024];
    snprintf(tmp_c, sizeof tmp_c, "/tmp/rsharp_rt_%d.c", (int)PLATFORM_GETPID());
    FILE *f = fopen(tmp_c, "w");
    if (!f) return 1;
    fputs(RUNTIME_C, f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "cc -O2 -c '%s' -o '%s' 2>/dev/null", tmp_c, obj_out);
    int rc = system(cmd);
    remove(tmp_c);
    return rc;
}

/* ── Main compiler driver ────────────────────────────────────────── */
int main(int argc, char **argv) {
    Options opts = parse_args(argc, argv);
    bool    color = PLATFORM_ISATTY(STDERR_FILENO);
    bool    all_ok = true;

    /* Collect all .o files for multi-file linking */
    char **obj_files  = malloc(sizeof(char *) * (size_t)(opts.input_count + 2));
    int    obj_count  = 0;

    for (int i = 0; i < opts.input_count; i++) {
        const char *path = opts.inputs[i];

        Arena    arena = arena_create(4 << 20);  /* 4 MiB per file */
        DiagSink diags = diag_sink_create(color);

        /* ── Stage 1: Read ──────────────────────────────────────── */
        size_t src_len;
        char  *src = read_file(path, &src_len, &arena);
        if (!src) { all_ok = false; goto done_file; }

        /* ── Stage 2: Lex ───────────────────────────────────────── */
        stage(&opts, "lex  ", path);
        Lexer lex = lexer_create(src, src_len, path, &arena);

        /* ── Stage 3: Parse ─────────────────────────────────────── */
        stage(&opts, "parse", path);
        Parser  par = parser_create(&lex, &arena, &diags);
        AstFile ast = parser_parse_file(&par, path);
        if (diag_had_errors(&diags)) { diag_print_all(&diags); all_ok = false; goto done_file; }

        /* ── Stage 4: Type check (sema) ─────────────────────────── */
        stage(&opts, "sema ", path);
        SemaCtx sema = sema_create(&arena, &diags);
        sema_check_file(&sema, &ast);
        if (diag_had_errors(&diags)) { diag_print_all(&diags); all_ok = false; goto done_file; }

        /* ── Stage 5: Ownership / borrow check ──────────────────── */
        stage(&opts, "own  ", path);
        OwnCtx own = own_ctx_create(&arena, &diags);
        own_check_file(&own, &ast);
        if (diag_had_errors(&diags)) { diag_print_all(&diags); all_ok = false; goto done_file; }

        if (opts.check_only) {
            if (opts.verbose)
                fprintf(stderr, "  \033[32mcheck OK\033[0m  %s\n", path);
            goto done_file;
        }

        /* ── Stage 6: LLVM codegen ──────────────────────────────── */
#ifdef RSHARP_WITH_LLVM
        stage(&opts, "llvm ", path);
        void *backend = llvm_backend_create(
            &diags,
            opts.opt_level,
            opts.target ? opts.target : "",
            opts.cpu,
            opts.emit_ir ? 1 : 0
        );

        /* Determine output .o path */
        char obj_path[512];
        if (opts.input_count == 1) {
            snprintf(obj_path, sizeof obj_path, "/tmp/rsc_%d_0.o", (int)PLATFORM_GETPID());
        } else {
            snprintf(obj_path, sizeof obj_path, "/tmp/rsc_%d_%d.o", (int)PLATFORM_GETPID(), i);
        }

        int cg_rc = llvm_backend_compile(backend, &ast, obj_path);
        llvm_backend_destroy(backend);

        if (cg_rc != 0 || diag_had_errors(&diags)) {
            diag_print_all(&diags);
            all_ok = false;
            goto done_file;
        }

        /* If --emit-ir, also print IR path */
        if (opts.emit_ir && opts.verbose) {
            char ir_path[512];
            snprintf(ir_path, sizeof ir_path, "%s.ll", obj_path);
            fprintf(stderr, "  \033[36m[ir]  \033[0m  %s\n", ir_path);
        }

        obj_files[obj_count] = malloc(strlen(obj_path) + 1);
        strcpy(obj_files[obj_count], obj_path);
        obj_count++;

#else
        /* No LLVM — inform user */
        fprintf(stderr,
            "\033[1m\033[33mwarning\033[0m: rsc was built without LLVM backend.\n"
            "  Type-checking and ownership analysis passed ✓\n"
            "  To compile to native binary: rebuild with -DRSHARP_WITH_LLVM=ON\n"
            "  See INSTALL.md for instructions.\n"
            "  For now, use 'rsrun %s' to run via interpreter.\n",
            path);
#endif

    done_file:
        arena_destroy(&arena);
        diag_sink_destroy(&diags);
    }

    if (!all_ok || opts.check_only || opts.input_count == 0)
        goto finish;

#ifdef RSHARP_WITH_LLVM
    if (obj_count == 0) goto finish;

    /* ── Stage 7: Compile runtime helpers ───────────────────────── */
    char rt_obj[256];
    snprintf(rt_obj, sizeof rt_obj, "/tmp/rsc_%d_rt.o", (int)PLATFORM_GETPID());
    compile_runtime(rt_obj);

    /* ── Stage 8: Link all .o files → output binary ─────────────── */
    stage(&opts, "link ", opts.output);

    /* Build combined link command */
    char link_cmd[4096] = {0};
    const char *linker =
        (system("which ld.lld > /dev/null 2>&1") == 0) ? "ld.lld" :
        (system("which clang > /dev/null 2>&1")  == 0) ? "clang"  : "cc";

    /* Linker flags */
    if (opts.target && strlen(opts.target) > 0)
        snprintf(link_cmd, sizeof link_cmd,
            "%s --target=%s -o '%s'", linker, opts.target, opts.output);
    else
        snprintf(link_cmd, sizeof link_cmd,
            "%s -o '%s'", linker, opts.output);

    /* Append object files */
    for (int i = 0; i < obj_count; i++) {
        strncat(link_cmd, " '", sizeof link_cmd - strlen(link_cmd) - 1);
        strncat(link_cmd, obj_files[i], sizeof link_cmd - strlen(link_cmd) - 1);
        strncat(link_cmd, "'", sizeof link_cmd - strlen(link_cmd) - 1);
    }
    /* Runtime object + libc math */
    strncat(link_cmd, " '", sizeof link_cmd - strlen(link_cmd) - 1);
    strncat(link_cmd, rt_obj, sizeof link_cmd - strlen(link_cmd) - 1);
    strncat(link_cmd, "' -lm 2>&1", sizeof link_cmd - strlen(link_cmd) - 1);

    if (opts.verbose) fprintf(stderr, "  \033[36m[link]\033[0m %s\n", link_cmd);
    int link_rc = system(link_cmd);
    if (link_rc != 0) {
        fprintf(stderr, "rsc: link failed\n");
        all_ok = false;
    } else {
        /* Make output executable */
        PLATFORM_CHMOD(opts.output, 0755);
        if (opts.verbose)
            fprintf(stderr, "  \033[32mfinished\033[0m  %s\n", opts.output);
    }

    /* Clean up temp objects */
    for (int i = 0; i < obj_count; i++) { remove(obj_files[i]); free(obj_files[i]); }
    remove(rt_obj);
#endif

finish:
    free(obj_files);
    free((void *)opts.inputs);
    return all_ok ? 0 : 1;
}
