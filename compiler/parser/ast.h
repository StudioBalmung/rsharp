/* rsharp/compiler/parser/ast.h
 * R# Abstract Syntax Tree
 * Language: C11
 */

#pragma once
#include "../lexer/lexer.h"
#include <stdint.h>
#include <stdbool.h>

/* ─── Forward declarations ─────────────────────────────────────── */
typedef struct Expr  Expr;
typedef struct Stmt  Stmt;
typedef struct Type  Type;
typedef struct ResolvedType ResolvedType;
typedef struct Decl  Decl;
typedef struct Param Param;
typedef struct Field Field;

/* ─── Interned string ───────────────────────────────────────────── */
typedef struct { const char *ptr; size_t len; } Ident;

/* ─── Types ─────────────────────────────────────────────────────── */
typedef enum TypeKind {
    TY_PRIM,        /* i32, f64, bool, etc.     */
    TY_NAMED,       /* Foo, my_module::Bar       */
    TY_PTR,         /* *T                        */
    TY_SLICE,       /* []T                       */
    TY_ARRAY,       /* [T; N]                    */
    TY_OPTIONAL,    /* ?T                        */
    TY_RESULT,      /* Result<T, E>              */
    TY_TUPLE,       /* (A, B, C)                 */
    TY_FN,          /* fn(A, B) -> C             */
    TY_INFER,       /* _ (let the compiler fill) */
} TypeKind;

typedef enum PrimKind {
    PRIM_I8, PRIM_I16, PRIM_I32, PRIM_I64, PRIM_I128,
    PRIM_U8, PRIM_U16, PRIM_U32, PRIM_U64, PRIM_U128,
    PRIM_F32, PRIM_F64,
    PRIM_BOOL, PRIM_CHAR, PRIM_VOID,
} PrimKind;

struct Type {
    TypeKind kind;
    SrcSpan  span;
    union {
        PrimKind prim;
        struct { Ident name; Type **args; size_t argc; }  named;
        struct { Type *inner; }                            ptr;
        struct { Type *elem; }                             slice;
        struct { Type *elem; Expr *count; }                array;
        struct { Type *inner; }                            optional;
        struct { Type *ok; Type *err; }                    result;
        struct { Type **elems; size_t count; }             tuple;
        struct { Type **params; size_t paramc; Type *ret; } fn;
    };
};

/* ─── Expressions ───────────────────────────────────────────────── */
typedef enum ExprKind {
    EXPR_INT_LIT,   EXPR_FLOAT_LIT, EXPR_BOOL_LIT, EXPR_CHAR_LIT,
    EXPR_STR_LIT,   EXPR_NULL,
    EXPR_IDENT,
    EXPR_UNARY,     EXPR_BINARY,
    EXPR_CALL,      EXPR_INDEX,     EXPR_FIELD,
    EXPR_IF,        EXPR_MATCH,     EXPR_LOOP,
    EXPR_BLOCK,
    EXPR_STRUCT_LIT,
    EXPR_ARRAY_LIT,
    EXPR_TUPLE_LIT,
    EXPR_CAST,
    EXPR_DEREF,     /* *expr       */
    EXPR_REF,       /* &expr       */
    EXPR_TRY,       /* try expr    */
    EXPR_CATCH,     /* expr catch |e| block */
    EXPR_RANGE,     /* a..b, a..=b */
    EXPR_COMPTIME,
    EXPR_BUILTIN,   /* @name(args) */
    EXPR_AWAIT,
    EXPR_CLOSURE,
} ExprKind;

typedef enum UnaryOp {
    UNOP_NEG, UNOP_NOT, UNOP_BITNOT, UNOP_DEREF, UNOP_ADDR
} UnaryOp;

typedef enum BinOp {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_AND, BINOP_OR,
    BINOP_BITAND, BINOP_BITOR, BINOP_BITXOR, BINOP_SHL, BINOP_SHR,
    BINOP_EQ, BINOP_NEQ, BINOP_LT, BINOP_GT, BINOP_LE, BINOP_GE,
    BINOP_ASSIGN,
    BINOP_ADD_ASSIGN, BINOP_SUB_ASSIGN, BINOP_MUL_ASSIGN, BINOP_DIV_ASSIGN,
    BINOP_NULLCOAL,    /* ?? */
} BinOp;

typedef struct MatchArm {
    Expr   *pattern;  /* pattern expression (can be literal, enum path, etc.) */
    Expr   *guard;    /* optional `if condition`                               */
    Expr   *body;
} MatchArm;

typedef struct FieldInit { Ident name; Expr *value; } FieldInit;

struct Expr {
    ExprKind kind;
    SrcSpan  span;
    ResolvedType *type; /* filled in by sema pass                             */
    union {
        int64_t     int_val;
        double      flt_val;
        bool        bool_val;
        uint32_t    char_val;
        struct { const char *ptr; size_t len; } str_val;

        Ident       ident;

        struct { UnaryOp op; Expr *operand; }                   unary;
        struct { BinOp   op; Expr *lhs; Expr *rhs; }           binary;
        struct { Expr *callee; Expr **args; size_t argc; }      call;
        struct { Expr *obj; Expr *idx; }                        index;
        struct { Expr *obj; Ident field; }                      field;

        struct { Expr *cond; Expr *then_expr; Expr *else_expr; } if_expr;
        struct { Expr *subject; MatchArm *arms; size_t armc; }  match;
        struct { Stmt **stmts; size_t stmtc; Expr *tail; }      block;

        struct { Type *ty; FieldInit *fields; size_t fieldc; }  struct_lit;
        struct { Expr **elems; size_t count; }                   array_lit;

        struct { Type *target; Expr *value; }                   cast;

        struct { Expr *expr; Expr *handler; Ident err_bind; }   catch_expr;
        struct { Expr *start; Expr *end; bool inclusive; }      range;

        struct { Ident name; Expr **args; size_t argc; }        builtin;
        struct { Expr *inner; }                                  comptime;
        struct { Expr *inner; }                                  await_expr;

        /* Closure: |a: T, b: T| -> T { body } */
        struct {
            Param *params; size_t paramc;
            Type  *ret;
            Expr  *body;
        } closure;
    };
};

/* ─── Statements ────────────────────────────────────────────────── */
typedef enum StmtKind {
    STMT_LET,       /* let x: T = expr              */
    STMT_VAR,       /* var x: T = expr (mutable)    */
    STMT_EXPR,      /* expression used as statement */
    STMT_RETURN,    /* return expr                  */
    STMT_BREAK,     /* break expr?                  */
    STMT_CONTINUE,
    STMT_DEFER,     /* defer stmt                   */
    STMT_UNSAFE,    /* unsafe { stmts }             */
    STMT_FOR,       /* for x in iter { }            */
    STMT_WHILE,     /* while cond { }               */
    STMT_ASSIGN,    /* target = value               */
} StmtKind;

struct Stmt {
    StmtKind kind;
    SrcSpan  span;
    union {
        struct { Ident name; Type *ty; Expr *init; bool is_mut; } let;
        struct { Expr *value; }                                    ret;
        struct { Expr *value; }                                    break_s;
        struct { Stmt *inner; }                                    defer_s;
        struct { Stmt **body; size_t count; }                      unsafe_s;
        struct { Ident bind; Expr *iter; Stmt **body; size_t c; } for_s;
        struct { Expr *cond; Stmt **body; size_t c; }              while_s;
        struct { Expr *lhs; BinOp op; Expr *rhs; }                assign;
        Expr  *expr_stmt;
    };
};

/* ─── Declarations ──────────────────────────────────────────────── */
typedef enum DeclKind {
    DECL_FN,
    DECL_STRUCT,
    DECL_ENUM,
    DECL_IMPL,
    DECL_INTERFACE,
    DECL_CONST,
    DECL_MODULE,
} DeclKind;

struct Param {
    Ident name;
    Type *ty;
    bool  is_comptime;
};

struct Field {
    Ident   name;
    Type   *ty;
    Expr   *default_val;
    bool    is_pub;
};

typedef struct EnumVariant {
    Ident   name;
    Field  *fields;
    size_t  fieldc;
} EnumVariant;

struct Decl {
    DeclKind kind;
    SrcSpan  span;
    bool     is_pub;
    union {
        struct {
            Ident   name;
            Param  *params;
            size_t  paramc;
            Type   *ret_ty;
            Stmt  **body;
            size_t  bodyc;
            bool    is_async;
            bool    is_extern;
        } fn;

        struct {
            Ident   name;
            Field  *fields;
            size_t  fieldc;
        } struc;

        struct {
            Ident        name;
            EnumVariant *variants;
            size_t       variantc;
        } enm;

        struct {
            Type  *self_ty;
            Type  *iface;   /* optional: impl Foo for Bar */
            Decl **methods;
            size_t methodc;
        } impl;

        struct {
            Ident   name;
            Decl  **methods; /* fn signatures only, no body */
            size_t  methodc;
        } iface;

        struct {
            Ident name;
            Type *ty;
            Expr *value;
        } cnst;

        struct {
            Ident   name;
            Decl  **decls;
            size_t  declc;
        } module;
    };
};

/* ─── Top-level compilation unit ────────────────────────────────── */
typedef struct AstFile {
    const char *path;
    Decl      **decls;
    size_t      declc;
} AstFile;
