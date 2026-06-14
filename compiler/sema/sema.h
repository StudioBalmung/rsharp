/* rsharp/compiler/sema/sema.h — Type Checker & Semantic Analysis (C11) */
#pragma once
#include "../parser/ast.h"
#include "../error/diagnostic.h"
#include "../../runtime/memory/arena.h"

typedef enum ResolvedTypeKind {
    RTYPE_NEVER, RTYPE_VOID, RTYPE_INT, RTYPE_FLOAT, RTYPE_BOOL,
    RTYPE_CHAR, RTYPE_STR, RTYPE_PTR, RTYPE_SLICE, RTYPE_ARRAY,
    RTYPE_OPTIONAL, RTYPE_RESULT, RTYPE_STRUCT, RTYPE_ENUM,
    RTYPE_FN, RTYPE_TUPLE, RTYPE_TRAIT_OBJ, RTYPE_INFER,
} ResolvedTypeKind;

typedef struct ResolvedType ResolvedType;
struct ResolvedType {
    ResolvedTypeKind kind;
    union {
        struct { int width; bool is_signed; }           int_ty;
        struct { int width; }                           float_ty;
        struct { ResolvedType *inner; bool is_mut; }    ptr_ty;
        struct { ResolvedType *elem; }                  slice_ty;
        struct { ResolvedType *elem; uint64_t count; }  array_ty;
        struct { ResolvedType *inner; }                 opt_ty;
        struct { ResolvedType *ok; ResolvedType *err; } result_ty;
        struct { Decl *decl; }                          named_ty;
        struct { ResolvedType **params; size_t pc; ResolvedType *ret; } fn_ty;
        struct { ResolvedType **elems; size_t count; }  tuple_ty;
    };
};

typedef enum SymbolKind {
    SYM_LOCAL, SYM_PARAM, SYM_FN, SYM_STRUCT, SYM_ENUM,
    SYM_CONST, SYM_INTERFACE, SYM_MODULE,
} SymbolKind;

typedef struct Symbol {
    Ident        name;
    SymbolKind   kind;
    ResolvedType *type;
    Decl         *decl;
    bool          is_mut;
    bool          moved;
    bool          is_pub;
} Symbol;

typedef struct Scope {
    Symbol       **syms;
    size_t         count, cap;
    struct Scope  *parent;
    bool           is_fn_root;
} Scope;

typedef struct SemaCtx {
    Arena    *arena;
    DiagSink *diags;
    Scope    *scope;
    ResolvedType *current_ret_type;
    bool          in_loop, in_unsafe;
    ResolvedType *ty_i8,*ty_i16,*ty_i32,*ty_i64,*ty_i128;
    ResolvedType *ty_u8,*ty_u16,*ty_u32,*ty_u64,*ty_u128;
    ResolvedType *ty_f32,*ty_f64;
    ResolvedType *ty_bool,*ty_char,*ty_void,*ty_str,*ty_never;
} SemaCtx;

SemaCtx      sema_create(Arena *arena, DiagSink *diags);
bool         sema_check_file(SemaCtx *ctx, AstFile *file);
ResolvedType *sema_check_expr(SemaCtx *ctx, Expr *e);
bool          sema_check_stmt(SemaCtx *ctx, Stmt *s);
bool          sema_types_equal(const ResolvedType *a, const ResolvedType *b);
const char   *sema_type_name(const ResolvedType *t, Arena *scratch);
void          sema_push_scope(SemaCtx *ctx, bool is_fn_root);
void          sema_pop_scope(SemaCtx *ctx);
Symbol       *sema_define(SemaCtx *ctx, Ident name, SymbolKind kind, ResolvedType *ty, bool is_mut);
Symbol       *sema_lookup(SemaCtx *ctx, Ident name);
