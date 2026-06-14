/* compiler/ownership/ownership.h — R# Ownership & Borrow Checker (C11)
 *
 * Implements Rust-style single-ownership + borrow rules:
 *  - Every value has exactly one owner
 *  - Moving a value invalidates the source
 *  - Any number of shared borrows (&T) OR one mutable borrow (*T) at a time
 *  - Borrows cannot outlive the owner
 *
 * This pass runs after sema (types are resolved) and before codegen.
 */
#pragma once
#include "../parser/ast.h"
#include "../sema/sema.h"
#include "../error/diagnostic.h"
#include "../../runtime/memory/arena.h"

/* ── Move state ───────────────────────────────────────────────────── */
typedef enum MoveState {
    MOVE_OWNED,      /* normal, fully owned                         */
    MOVE_MOVED,      /* value was moved — use is an error           */
    MOVE_BORROWED,   /* currently borrowed (shared)                 */
    MOVE_BORROW_MUT, /* currently mutably borrowed                  */
    MOVE_PARTIAL,    /* struct with some fields moved out           */
    MOVE_COPY,       /* trivially-copyable type — moves are copies  */
} MoveState;

/* ── Borrow record ────────────────────────────────────────────────── */
typedef struct BorrowRecord {
    Ident            var;
    bool             is_mut;
    SrcSpan          span;       /* where the borrow was created    */
    uint32_t         scope_depth;
    struct BorrowRecord *next;
} BorrowRecord;

/* ── Variable ownership entry ─────────────────────────────────────── */
typedef struct OwnEntry {
    Ident        name;
    MoveState    state;
    SrcSpan      moved_at;   /* valid when state == MOVE_MOVED      */
    bool         is_mut;
    bool         is_copy;    /* i32, f64, bool, char, etc.          */
    uint32_t     scope_depth;
    struct OwnEntry *next;
} OwnEntry;

/* ── Ownership context ────────────────────────────────────────────── */
typedef struct OwnCtx {
    Arena          *arena;
    DiagSink       *diags;
    OwnEntry       *vars;        /* linked list, current + parent scopes */
    BorrowRecord   *borrows;     /* active borrows                       */
    uint32_t        depth;       /* current scope depth                  */
    bool            in_unsafe;   /* unsafe block disables borrow checks  */
} OwnCtx;

/* ── API ──────────────────────────────────────────────────────────── */
OwnCtx own_ctx_create(Arena *arena, DiagSink *diags);
bool   own_check_file(OwnCtx *ctx, AstFile *file);
bool   own_check_fn(OwnCtx *ctx, Decl *fn);
bool   own_check_stmts(OwnCtx *ctx, Stmt **stmts, size_t count);
bool   own_check_stmt(OwnCtx *ctx, Stmt *s);
bool   own_check_expr(OwnCtx *ctx, Expr *e, bool is_lvalue);

/* Scope management */
void own_push_scope(OwnCtx *ctx);
void own_pop_scope(OwnCtx *ctx);

/* Variable registration */
OwnEntry *own_define(OwnCtx *ctx, Ident name, bool is_mut, bool is_copy);
OwnEntry *own_lookup(OwnCtx *ctx, Ident name);

/* Move / borrow operations */
bool own_move(OwnCtx *ctx, Ident name, SrcSpan use_span);
bool own_borrow(OwnCtx *ctx, Ident name, bool is_mut, SrcSpan span);
void own_release_borrows_at_depth(OwnCtx *ctx, uint32_t depth);

/* Type helpers */
bool own_type_is_copy(const ResolvedType *t);
