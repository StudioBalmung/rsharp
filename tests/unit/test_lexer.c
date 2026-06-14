/* rsharp/tests/unit/test_lexer.c
 * Lexer unit tests — minimal test framework (no external deps)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "../../compiler/lexer/lexer.h"
#include "../../runtime/memory/arena.h"

static int tests_run   = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b, msg)  do { \
    tests_run++; \
    if ((a) != (b)) { \
        fprintf(stderr, "FAIL [%s:%d] %s: expected %d, got %d\n", \
                __FILE__, __LINE__, msg, (int)(b), (int)(a)); \
        tests_failed++; \
    } \
} while(0)

#define ASSERT_STR(a, b, msg) do { \
    tests_run++; \
    if (strcmp((a),(b)) != 0) { \
        fprintf(stderr, "FAIL [%s:%d] %s: expected '%s', got '%s'\n", \
                __FILE__, __LINE__, msg, b, a); \
        tests_failed++; \
    } \
} while(0)

#define ASSERT_TRUE(cond, msg) ASSERT_EQ(!!(cond), 1, msg)

/* ─── Helpers ──────────────────────────────────────────────────── */
static Token lex_one(const char *src, Arena *arena) {
    Lexer l = lexer_create(src, strlen(src), "<test>", arena);
    return lexer_next(&l);
}

static Token lex_nth(const char *src, int n, Arena *arena) {
    Lexer l = lexer_create(src, strlen(src), "<test>", arena);
    Token t = {0};
    for (int i = 0; i <= n; i++) t = lexer_next(&l);
    return t;
}

/* ─── Tests ─────────────────────────────────────────────────────── */

static void test_keywords(Arena *a) {
    ASSERT_EQ(lex_one("fn",        a).kind, TOK_FN,       "fn keyword");
    ASSERT_EQ(lex_one("let",       a).kind, TOK_LET,      "let keyword");
    ASSERT_EQ(lex_one("var",       a).kind, TOK_VAR,      "var keyword");
    ASSERT_EQ(lex_one("struct",    a).kind, TOK_STRUCT,   "struct keyword");
    ASSERT_EQ(lex_one("if",        a).kind, TOK_IF,       "if keyword");
    ASSERT_EQ(lex_one("match",     a).kind, TOK_MATCH,    "match keyword");
    ASSERT_EQ(lex_one("defer",     a).kind, TOK_DEFER,    "defer keyword");
    ASSERT_EQ(lex_one("unsafe",    a).kind, TOK_UNSAFE,   "unsafe keyword");
    ASSERT_EQ(lex_one("comptime",  a).kind, TOK_COMPTIME, "comptime keyword");
    ASSERT_EQ(lex_one("interface", a).kind, TOK_INTERFACE,"interface keyword");
}

static void test_identifiers(Arena *a) {
    Token t = lex_one("my_var", a);
    ASSERT_EQ(t.kind, TOK_IDENT, "identifier kind");
    ASSERT_STR(t.str.ptr, "my_var", "identifier text");

    Token t2 = lex_one("_private", a);
    ASSERT_EQ(t2.kind, TOK_IDENT, "underscore-prefixed ident");
    ASSERT_STR(t2.str.ptr, "_private", "underscore ident text");
}

static void test_integer_literals(Arena *a) {
    ASSERT_EQ(lex_one("42",   a).int_val, 42,   "decimal int");
    ASSERT_EQ(lex_one("0xFF", a).int_val, 255,  "hex int");
    ASSERT_EQ(lex_one("0b1010",a).int_val, 10,  "binary int");
    ASSERT_EQ(lex_one("1_000_000", a).int_val, 1000000, "underscore int");
}

static void test_float_literals(Arena *a) {
    Token t = lex_one("3.14", a);
    ASSERT_EQ(t.kind, TOK_FLOAT_LIT, "float kind");
    ASSERT_TRUE(t.flt_val > 3.13 && t.flt_val < 3.15, "float value ~3.14");
}

static void test_string_literal(Arena *a) {
    Token t = lex_one("\"hello\"", a);
    ASSERT_EQ(t.kind, TOK_STR_LIT, "string kind");
    ASSERT_STR(t.str.ptr, "hello", "string content");
    ASSERT_EQ((int)t.str.len, 5, "string length");
}

static void test_bool_literals(Arena *a) {
    Token t = lex_one("true",  a); ASSERT_EQ(t.kind, TOK_BOOL_LIT, "bool true kind");  ASSERT_EQ(t.int_val, 1, "true value");
    Token f = lex_one("false", a); ASSERT_EQ(f.kind, TOK_BOOL_LIT, "bool false kind"); ASSERT_EQ(f.int_val, 0, "false value");
}

static void test_operators(Arena *a) {
    ASSERT_EQ(lex_one("->",  a).kind, TOK_ARROW,     "arrow ->");
    ASSERT_EQ(lex_one("=>",  a).kind, TOK_FAT_ARROW, "fat arrow =>");
    ASSERT_EQ(lex_one("::",  a).kind, TOK_COLONCOLON,"double colon");
    ASSERT_EQ(lex_one("..",  a).kind, TOK_DOTDOT,    "range ..");
    ASSERT_EQ(lex_one("..=", a).kind, TOK_DOTDOTEQ,  "inclusive range ..=");
    ASSERT_EQ(lex_one("??",  a).kind, TOK_QMARKQMARK,"null-coalescing ??");
    ASSERT_EQ(lex_one("+=",  a).kind, TOK_PLUS_EQ,   "plus-assign +=");
    ASSERT_EQ(lex_one("!=",  a).kind, TOK_NEQ,       "not-equal !=");
}

static void test_comments_skipped(Arena *a) {
    /* Line comment should be fully ignored */
    Token t = lex_nth("-- this is a comment\nlet", 0, a);
    ASSERT_EQ(t.kind, TOK_LET, "line comment skipped");

    Token t2 = lex_one("/* block */ fn", a);
    ASSERT_EQ(t2.kind, TOK_FN, "block comment skipped");
}

static void test_source_positions(Arena *a) {
    const char *src = "let\nx = 1";
    Lexer l = lexer_create(src, strlen(src), "<test>", a);
    Token t1 = lexer_next(&l);
    ASSERT_EQ(t1.span.start.line, 1, "line of 'let'");
    ASSERT_EQ(t1.span.start.col,  1, "col  of 'let'");

    Token t2 = lexer_next(&l);  /* x */
    ASSERT_EQ(t2.span.start.line, 2, "line of 'x'");
    ASSERT_EQ(t2.span.start.col,  1, "col  of 'x'");
}

static void test_eof(Arena *a) {
    Token t = lex_one("", a);
    ASSERT_EQ(t.kind, TOK_EOF, "empty source → EOF");
}

static void test_error_token(Arena *a) {
    Token t = lex_one("$$$", a);
    ASSERT_EQ(t.kind, TOK_ERROR, "unknown char → error token");
    ASSERT_TRUE(t.str.ptr != NULL, "error token has message");
}

/* ─── Runner ─────────────────────────────────────────────────────── */
int main(void) {
    Arena arena = arena_create(1 << 20);

    test_keywords(&arena);
    test_identifiers(&arena);
    test_integer_literals(&arena);
    test_float_literals(&arena);
    test_string_literal(&arena);
    test_bool_literals(&arena);
    test_operators(&arena);
    test_comments_skipped(&arena);
    test_source_positions(&arena);
    test_eof(&arena);
    test_error_token(&arena);

    arena_destroy(&arena);

    printf("\nLexer tests: %d/%d passed", tests_run - tests_failed, tests_run);
    if (tests_failed > 0) {
        printf(" (%d FAILED)\n", tests_failed);
        return 1;
    }
    printf(" — all OK\n");
    return 0;
}
