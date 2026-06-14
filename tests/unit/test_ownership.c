/* tests/unit/test_ownership.c — R# Ownership Checker Tests */
#include <stdio.h>
#include <string.h>
#include "../../compiler/ownership/ownership.h"
#include "../../compiler/sema/sema.h"
#include "../../compiler/parser/parser.h"
#include "../../runtime/memory/arena.h"
#include "../../compiler/error/diagnostic.h"

static int ok=0, fail=0;
#define EXPECT(cond, msg) do{ \
    if(cond){ok++;}else{fail++; \
    fprintf(stderr,"FAIL [%d] %s\n",__LINE__,msg);} \
}while(0)

static bool run_own(const char *src) {
    Arena    a = arena_create(1<<20);
    DiagSink d = diag_sink_create(0);
    Lexer    l = lexer_create(src,strlen(src),"<t>",&a);
    Parser   p = parser_create(&l,&a,&d);
    AstFile  f = parser_parse_file(&p,"<t>");
    SemaCtx  s = sema_create(&a,&d);
    sema_check_file(&s,&f);
    OwnCtx   o = own_ctx_create(&a,&d);
    own_check_file(&o,&f);
    bool r = !diag_had_errors(&d);
    arena_destroy(&a); diag_sink_destroy(&d);
    return r;
}
static bool own_has_error(const char *src) { return !run_own(src); }

// Copy types can be used after binding
static void test_copy_int(void) {
    EXPECT(run_own("fn f() => void { let x: i32 = 1; let y = x; let z = x }"),
           "copy type: int usable twice");
}
// Basic let binding
static void test_basic_let(void) {
    EXPECT(run_own("fn f() => void { let x: i32 = 42 }"), "basic let ok");
}
// Function with params
static void test_fn_params(void) {
    EXPECT(run_own("fn add(a: i32, b: i32) => i32 { return 0 }"), "fn params ok");
}
// Empty function
static void test_empty_fn(void) {
    EXPECT(run_own("fn f() => void {}"), "empty fn ok");
}
// For loop
static void test_for_loop(void) {
    EXPECT(run_own("fn f() => void { for i in 0..10 {} }"), "for loop ok");
}
// While loop
static void test_while_loop(void) {
    EXPECT(run_own("fn f() => void { while true {} }"), "while loop ok");
}
// Unsafe block allowed
static void test_unsafe_block(void) {
    EXPECT(run_own("fn f() => void { unsafe { let x: i32 = 0 } }"), "unsafe block ok");
}
// Return statement
static void test_return(void) {
    EXPECT(run_own("fn f() => i32 { return 0 }"), "return ok");
}
// Multiple functions
static void test_multi_fn(void) {
    EXPECT(run_own("fn a() => void {} fn b() => void {} fn c() => i32 { return 1 }"),
           "multiple fns ok");
}
// Struct declaration
static void test_struct_decl(void) {
    EXPECT(run_own("struct Point { x: f32, y: f32, }"), "struct decl ok");
}
// own_type_is_copy
static void test_type_is_copy(void) {
    Arena a = arena_create(1<<20);
    DiagSink d = diag_sink_create(0);
    SemaCtx s = sema_create(&a,&d);
    EXPECT(own_type_is_copy(s.ty_i32),  "i32 is copy");
    EXPECT(own_type_is_copy(s.ty_f64),  "f64 is copy");
    EXPECT(own_type_is_copy(s.ty_bool), "bool is copy");
    EXPECT(own_type_is_copy(s.ty_char), "char is copy");
    EXPECT(own_type_is_copy(s.ty_void), "void is copy");
    EXPECT(!own_type_is_copy(s.ty_str), "str is NOT copy");
    arena_destroy(&a); diag_sink_destroy(&d);
}

int main(void) {
    test_copy_int();
    test_basic_let();
    test_fn_params();
    test_empty_fn();
    test_for_loop();
    test_while_loop();
    test_unsafe_block();
    test_return();
    test_multi_fn();
    test_struct_decl();
    test_type_is_copy();

    printf("\nOwnership tests: %d/%d passed", ok, ok+fail);
    if (fail>0) { printf(" (%d FAILED)\n",fail); return 1; }
    printf(" — all OK\n"); return 0;
}
