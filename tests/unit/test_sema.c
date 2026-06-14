/* tests/unit/test_sema.c — R# Sema Tests */
#include <stdio.h>
#include <string.h>
#include "../../compiler/sema/sema.h"
#include "../../compiler/parser/parser.h"
#include "../../runtime/memory/arena.h"
#include "../../compiler/error/diagnostic.h"

static int ok=0, fail=0;
#define EXPECT(cond, msg) do{ if(cond){ok++;}else{fail++;fprintf(stderr,"FAIL [%d] %s\n",__LINE__,msg);} }while(0)

static bool check_src(const char *src) {
    Arena    a = arena_create(1<<20);
    DiagSink d = diag_sink_create(0);
    Lexer    l = lexer_create(src, strlen(src), "<t>", &a);
    Parser   p = parser_create(&l, &a, &d);
    AstFile  f = parser_parse_file(&p, "<t>");
    SemaCtx  s = sema_create(&a, &d);
    bool     r = sema_check_file(&s, &f) && !diag_had_errors(&d);
    arena_destroy(&a); diag_sink_destroy(&d);
    return r;
}
static bool has_error(const char *src) {
    Arena    a = arena_create(1<<20);
    DiagSink d = diag_sink_create(0);
    Lexer    l = lexer_create(src, strlen(src), "<t>", &a);
    Parser   p = parser_create(&l, &a, &d);
    AstFile  f = parser_parse_file(&p, "<t>");
    SemaCtx  s = sema_create(&a, &d);
    sema_check_file(&s, &f);
    bool r = diag_had_errors(&d);
    arena_destroy(&a); diag_sink_destroy(&d);
    return r;
}

static void test_ok_fn(void)    { EXPECT(check_src("fn f() => void {}"), "empty fn"); }
static void test_ok_let(void)   { EXPECT(check_src("fn f() => void { let x: i32 = 1 }"), "let"); }
static void test_ok_ret(void)   { EXPECT(check_src("fn f() => i32 { return 0 }"), "return"); }
static void test_ok_for(void)   { EXPECT(check_src("fn f() => void { for i in 0..10 {} }"), "for"); }
static void test_ok_struct(void){ EXPECT(check_src("struct P { x: f32, y: f32, }"), "struct"); }
static void test_ok_enum(void)  { EXPECT(check_src("enum E { A, B, C, }"), "enum"); }
static void test_undef(void)    { EXPECT(has_error("fn f() => void { let _ = undef_var }"), "undef=>error"); }
static void test_bad_break(void){ EXPECT(has_error("fn f() => void { break }"), "break outside loop"); }
static void test_ok_while(void) { EXPECT(check_src("fn f() => void { while true {} }"), "while"); }
static void test_type_names(void) {
    Arena    a = arena_create(1<<20);
    DiagSink d = diag_sink_create(0);
    SemaCtx  s = sema_create(&a, &d);
    EXPECT(strcmp(sema_type_name(s.ty_i32, &a),"i32")==0, "i32");
    EXPECT(strcmp(sema_type_name(s.ty_f64, &a),"f64")==0, "f64");
    EXPECT(strcmp(sema_type_name(s.ty_bool,&a),"bool")==0,"bool");
    EXPECT(strcmp(sema_type_name(s.ty_void,&a),"void")==0,"void");
    EXPECT(strcmp(sema_type_name(s.ty_never,&a),"!")==0,  "never");
    arena_destroy(&a); diag_sink_destroy(&d);
}
static void test_ok_const(void) { EXPECT(check_src("const N: i32 = 42"), "const"); }
static void test_two_fns(void)  { EXPECT(check_src("fn f() => void {} fn g() => void {}"), "two fns"); }

int main(void) {
    test_ok_fn(); test_ok_let(); test_ok_ret(); test_ok_for();
    test_ok_struct(); test_ok_enum(); test_undef(); test_bad_break();
    test_ok_while(); test_type_names(); test_ok_const(); test_two_fns();

    printf("\nSema tests: %d/%d passed", ok, ok+fail);
    if (fail>0) { printf(" (%d FAILED)\n",fail); return 1; }
    printf(" — all OK\n"); return 0;
}
