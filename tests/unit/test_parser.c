/* tests/unit/test_parser.c — R# Parser Tests */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../compiler/parser/parser.h"
#include "../../runtime/memory/arena.h"
#include "../../compiler/error/diagnostic.h"

static int ok=0, fail=0;
#define EXPECT(cond, msg) do{ if(cond){ok++;}else{fail++;fprintf(stderr,"FAIL [%d] %s\n",__LINE__,msg);} }while(0)

typedef struct { AstFile file; Arena arena; DiagSink diags; } ParseResult;

static ParseResult parse_src(const char *src) {
    ParseResult r;
    r.arena = arena_create(1<<20);
    r.diags = diag_sink_create(0);
    Lexer  l = lexer_create(src, strlen(src), "<test>", &r.arena);
    Parser p = parser_create(&l, &r.arena, &r.diags);
    r.file   = parser_parse_file(&p, "<test>");
    return r;
}
static void pr_free(ParseResult *r) {
    arena_destroy(&r->arena);
    diag_sink_destroy(&r->diags);
}

static void test_empty(void) {
    ParseResult r = parse_src("");
    EXPECT(r.file.declc == 0, "empty file => 0 decls");
    pr_free(&r);
}
static void test_fn_void(void) {
    ParseResult r = parse_src("fn foo() => void {}");
    EXPECT(r.file.declc == 1, "one fn decl");
    EXPECT(r.file.decls[0]->kind == DECL_FN, "decl is fn");
    pr_free(&r);
}
static void test_fn_params(void) {
    ParseResult r = parse_src("fn add(a: i32, b: i32) => i32 { a }");
    EXPECT(r.file.declc == 1, "one fn");
    EXPECT(r.file.decls[0]->fn.paramc == 2, "two params");
    pr_free(&r);
}
static void test_struct(void) {
    ParseResult r = parse_src("struct Point { x: f32, y: f32, }");
    EXPECT(r.file.declc == 1, "one struct");
    EXPECT(r.file.decls[0]->kind == DECL_STRUCT, "is struct");
    EXPECT(r.file.decls[0]->struc.fieldc == 2, "two fields");
    pr_free(&r);
}
static void test_enum(void) {
    ParseResult r = parse_src("enum Color { Red, Green, Blue, }");
    EXPECT(r.file.declc == 1, "one enum");
    EXPECT(r.file.decls[0]->enm.variantc == 3, "three variants");
    pr_free(&r);
}
static void test_let(void) {
    ParseResult r = parse_src("fn f() => void { let x: i32 = 42 }");
    EXPECT(!diag_had_errors(&r.diags), "no parse errors");
    if (r.file.declc>0 && r.file.decls[0]->fn.bodyc>0)
        EXPECT(r.file.decls[0]->fn.body[0]->kind == STMT_LET, "stmt is let");
    else EXPECT(0, "has body stmt");
    pr_free(&r);
}
static void test_for_loop(void) {
    ParseResult r = parse_src("fn f() => void { for i in 0..10 { } }");
    EXPECT(!diag_had_errors(&r.diags), "no parse errors");
    if (r.file.declc>0 && r.file.decls[0]->fn.bodyc>0)
        EXPECT(r.file.decls[0]->fn.body[0]->kind == STMT_FOR, "stmt is for");
    else EXPECT(0, "has body stmt");
    pr_free(&r);
}
static void test_if_expr(void) {
    ParseResult r = parse_src("fn f() => i32 { if true { 1 } else { 0 } }");
    EXPECT(!diag_had_errors(&r.diags), "no errors in if expr");
    pr_free(&r);
}
static void test_binary(void) {
    ParseResult r = parse_src("fn f() => i32 { 1 + 2 * 3 }");
    EXPECT(!diag_had_errors(&r.diags), "binary expr no errors");
    pr_free(&r);
}
static void test_call(void) {
    ParseResult r = parse_src("fn f() => void { g(1, 2, 3) }");
    EXPECT(!diag_had_errors(&r.diags), "call no errors");
    pr_free(&r);
}
static void test_pub_fn(void) {
    ParseResult r = parse_src("pub fn hello() => void {}");
    EXPECT(r.file.decls[0]->is_pub, "fn is pub");
    pr_free(&r);
}
static void test_const(void) {
    ParseResult r = parse_src("const PI: f64 = 3.14");
    EXPECT(r.file.declc == 1, "one const");
    EXPECT(r.file.decls[0]->kind == DECL_CONST, "is const");
    pr_free(&r);
}
static void test_fat_arrow(void) {
    ParseResult r = parse_src("fn f() => i32 { 0 }");
    EXPECT(!diag_had_errors(&r.diags), "fat arrow OK");
    EXPECT(r.file.decls[0]->fn.ret_ty != NULL, "has ret type");
    pr_free(&r);
}

int main(void) {
    test_empty(); test_fn_void(); test_fn_params(); test_struct();
    test_enum();  test_let();     test_for_loop();  test_if_expr();
    test_binary();test_call();    test_pub_fn();    test_const(); test_fat_arrow();

    printf("\nParser tests: %d/%d passed", ok, ok+fail);
    if (fail>0) { printf(" (%d FAILED)\n",fail); return 1; }
    printf(" — all OK\n"); return 0;
}
