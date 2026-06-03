/* rsharp/tools/rsc/main.c — R# Compiler Driver v1.0  (C11)
 *
 * Pipeline: source.rsl -> Lex -> Parse -> Sema -> [Ownership] -> [LLVM] -> binary
 *
 * Usage:
 *   rsc <file.rsl> [options]
 *   rsc --version
 *
 * Options:
 *   -o <out>          Output path          (default: a.out)
 *   --emit-ir         Write LLVM IR (.ll)
 *   -O0/-O1/-O2/-Os   Optimisation level
 *   --release         -O2 + strip asserts
 *   --check           Type-check only
 *   --test            Build + link test runner
 *   --target <t>      Cross-compile triple
 *   --verbose
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include "../../compiler/lexer/lexer.h"
#include "../../compiler/parser/parser.h"
#include "../../compiler/sema/sema.h"
#include "../../compiler/error/diagnostic.h"
#include "../../runtime/memory/arena.h"

#define RSC_VERSION "1.0.0"

typedef struct {
    const char *output;
    const char *target;
    int         opt_level;
    bool        emit_ir, check_only, run_after, verbose, release, test_mode;
    const char **inputs;
    int          input_count;
} Options;

static void usage(void) {
    fprintf(stderr,
        "rsc v" RSC_VERSION " — R# Compiler\n\n"
        "USAGE: rsc [OPTIONS] <file.rsl>...\n\n"
        "OPTIONS:\n"
        "  -o <out>       Output path (default: a.out)\n"
        "  --check        Type-check only\n"
        "  --emit-ir      Write LLVM IR\n"
        "  --release      -O2 + strip assertions\n"
        "  -O0/-O1/-O2/-Os  Optimisation level\n"
        "  --target <t>   Cross-compile target triple\n"
        "  --verbose      Verbose output\n"
        "  --version      Print version\n"
        "  -h/--help      Show this help\n");
}

static Options parse_args(int argc, char **argv) {
    Options o = { .output="a.out", .inputs=malloc(sizeof(char*)*argc) };
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--version"))  { printf("rsc v" RSC_VERSION "\n"); exit(0); }
        if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")) { usage(); exit(0); }
        if (!strcmp(argv[i],"--emit-ir"))  { o.emit_ir=true; continue; }
        if (!strcmp(argv[i],"--check"))    { o.check_only=true; continue; }
        if (!strcmp(argv[i],"--release"))  { o.release=true; o.opt_level=2; continue; }
        if (!strcmp(argv[i],"--verbose"))  { o.verbose=true; continue; }
        if (!strcmp(argv[i],"--test"))     { o.test_mode=true; continue; }
        if (!strcmp(argv[i],"-O0"))        { o.opt_level=0; continue; }
        if (!strcmp(argv[i],"-O1"))        { o.opt_level=1; continue; }
        if (!strcmp(argv[i],"-O2"))        { o.opt_level=2; continue; }
        if (!strcmp(argv[i],"-Os"))        { o.opt_level=3; continue; }
        if (!strcmp(argv[i],"-o")&&i+1<argc) { o.output=argv[++i]; continue; }
        if (!strcmp(argv[i],"--target")&&i+1<argc) { o.target=argv[++i]; continue; }
        if (argv[i][0]=='-') { fprintf(stderr,"rsc: unknown option '%s'\n",argv[i]); exit(1); }
        o.inputs[o.input_count++] = argv[i];
    }
    if (o.input_count==0) { fprintf(stderr,"rsc: no input files\n"); usage(); exit(1); }
    return o;
}

static char *read_file(const char *path, size_t *len, Arena *arena) {
    FILE *f = fopen(path,"rb");
    if (!f) { fprintf(stderr,"rsc: cannot open '%s'\n",path); return NULL; }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf = arena_alloc(arena,(size_t)sz+1,1);
    fread(buf,1,(size_t)sz,f); buf[sz]='\0'; fclose(f);
    if (len) *len=(size_t)sz;
    return buf;
}

int main(int argc, char **argv) {
    Options opts = parse_args(argc, argv);
    bool color = isatty(STDERR_FILENO);
    bool all_ok = true;

    for (int i=0;i<opts.input_count;i++) {
        const char *path = opts.inputs[i];
        Arena    arena = arena_create(1<<22);
        DiagSink diags = diag_sink_create(color);

        size_t src_len;
        char  *src = read_file(path, &src_len, &arena);
        if (!src) { all_ok=false; arena_destroy(&arena); diag_sink_destroy(&diags); continue; }

        if (opts.verbose) fprintf(stderr,"  [lex]   %s\n",path);
        Lexer  lex = lexer_create(src, src_len, path, &arena);

        if (opts.verbose) fprintf(stderr,"  [parse] %s\n",path);
        Parser par = parser_create(&lex, &arena, &diags);
        AstFile ast = parser_parse_file(&par, path);

        if (diag_had_errors(&diags)) { diag_print_all(&diags); all_ok=false; goto next; }

        if (opts.verbose) fprintf(stderr,"  [sema]  %s\n",path);
        SemaCtx sema = sema_create(&arena, &diags);
        sema_check_file(&sema, &ast);

        if (diag_had_errors(&diags)) { diag_print_all(&diags); all_ok=false; goto next; }

        if (opts.check_only) {
            if (opts.verbose) fprintf(stderr,"  check OK: %s\n",path);
            goto next;
        }

#ifdef RSHARP_WITH_LLVM
        if (opts.verbose) fprintf(stderr,"  [codegen] %s -> %s\n",path,opts.output);
        /* TODO: llvm_backend_compile(&ast, opts.output, opts.opt_level, opts.emit_ir) */
        fprintf(stderr,"rsc: LLVM backend not yet wired to driver (compile with RSHARP_WITH_LLVM=ON)\n");
#else
        fprintf(stderr,"rsc: native code generation requires -DRSHARP_WITH_LLVM=ON\n"
                       "     For now: rsc --check works, rsl run uses the interpreter\n");
#endif

    next:
        arena_destroy(&arena);
        diag_sink_destroy(&diags);
    }

    free((void*)opts.inputs);
    return all_ok ? 0 : 1;
}
