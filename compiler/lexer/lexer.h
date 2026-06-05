/* rsharp/compiler/lexer/lexer.h  —  R# Lexer  (C11)
 * Tokens for R# 1.0
 * Key syntax: fn name(params) -> RetType { body }
 *             fn name(params) -> expr        (short form)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum TokenKind {
    /* Literals */
    TOK_INT_LIT, TOK_FLOAT_LIT, TOK_CHAR_LIT, TOK_STR_LIT,
    TOK_BOOL_LIT, TOK_NULL_LIT,
    /* Identifiers */
    TOK_IDENT,
    /* Keywords */
    TOK_FN, TOK_LET, TOK_VAR, TOK_CONST,
    TOK_STRUCT, TOK_ENUM, TOK_IMPL, TOK_INTERFACE,
    TOK_IF, TOK_ELSE, TOK_MATCH, TOK_FOR, TOK_WHILE, TOK_LOOP,
    TOK_RETURN, TOK_BREAK, TOK_CONTINUE,
    TOK_DEFER, TOK_UNSAFE, TOK_ASYNC, TOK_AWAIT, TOK_SPAWN,
    TOK_PUB, TOK_EXTERN, TOK_EXPORT, TOK_USE, TOK_MOD, TOK_AS,
    TOK_COMPTIME, TOK_TRY, TOK_CATCH, TOK_IN, TOK_SELF,
    /* Primitive types */
    TOK_KW_I8, TOK_KW_I16, TOK_KW_I32, TOK_KW_I64, TOK_KW_I128, TOK_KW_ISIZE,
    TOK_KW_U8, TOK_KW_U16, TOK_KW_U32, TOK_KW_U64, TOK_KW_U128, TOK_KW_USIZE,
    TOK_KW_F32, TOK_KW_F64,
    TOK_KW_BOOL, TOK_KW_CHAR, TOK_KW_VOID, TOK_KW_STR,
    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_BANG,
    TOK_SHL, TOK_SHR,
    TOK_EQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_LOGAND, TOK_LOGOR,
    TOK_ASSIGN,
    TOK_PLUS_EQ, TOK_MINUS_EQ, TOK_STAR_EQ, TOK_SLASH_EQ, TOK_PERCENT_EQ,
    TOK_AMP_EQ, TOK_PIPE_EQ, TOK_CARET_EQ,
    TOK_ARROW,       /* ->  (type-level, e.g. fn ptr)  */
    TOK_FAT_ARROW,   /* ->  (return type + match arms)  */
    TOK_COLON, TOK_COLONCOLON,
    TOK_DOT, TOK_DOTDOT, TOK_DOTDOTEQ, TOK_DOTDOTDOT,
    TOK_COMMA, TOK_SEMI, TOK_QMARK, TOK_QMARKQMARK,
    TOK_AT, TOK_HASH,
    /* Delimiters */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACK, TOK_RBRACK,
    /* Specials */
    TOK_EOF, TOK_ERROR,
} TokenKind;

typedef struct SrcPos  { uint32_t line, col, offset; } SrcPos;
typedef struct SrcSpan { SrcPos start, end; }          SrcSpan;

typedef struct Token {
    TokenKind kind;
    SrcSpan   span;
    union {
        int64_t  int_val;
        double   flt_val;
        uint32_t char_val;
        struct { const char *ptr; size_t len; } str;
    };
} Token;

typedef struct Lexer {
    const char *src;
    size_t      src_len;
    size_t      pos;
    uint32_t    line, col;
    const char *filepath;
    void       *arena;
} Lexer;

Lexer       lexer_create(const char *src, size_t len, const char *filepath, void *arena);
Token       lexer_next(Lexer *l);
Token       lexer_peek(Lexer *l);
const char *token_kind_name(TokenKind k);
bool        token_is_assign_op(TokenKind k);
