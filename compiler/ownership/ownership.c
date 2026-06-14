/* compiler/ownership/ownership.c — R# Ownership & Borrow Checker  C11 */
#include "ownership.h"
#include <string.h>
#include <stdio.h>

#define NEW(ctx,T) ((T*)arena_calloc((ctx)->arena,1,sizeof(T)))

/* ── Helpers ─────────────────────────────────────────────────────── */
static bool ident_eq(Ident a, Ident b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

bool own_type_is_copy(const ResolvedType *t) {
    if (!t) return false;
    switch (t->kind) {
        case RTYPE_INT: case RTYPE_FLOAT: case RTYPE_BOOL:
        case RTYPE_CHAR: case RTYPE_VOID: case RTYPE_NEVER:
            return true;
        case RTYPE_PTR:   /* raw pointers are Copy */
            return true;
        case RTYPE_OPTIONAL:
            return own_type_is_copy(t->opt_ty.inner);
        default:
            return false; /* structs, enums, slices, Box etc. are Move */
    }
}

/* ── Context ─────────────────────────────────────────────────────── */
OwnCtx own_ctx_create(Arena *arena, DiagSink *diags) {
    OwnCtx ctx = {0};
    ctx.arena  = arena;
    ctx.diags  = diags;
    ctx.depth  = 0;
    return ctx;
}

/* ── Scope ───────────────────────────────────────────────────────── */
void own_push_scope(OwnCtx *ctx) { ctx->depth++; }

void own_pop_scope(OwnCtx *ctx) {
    own_release_borrows_at_depth(ctx, ctx->depth);
    /* Remove variables defined at this depth */
    OwnEntry **cur = &ctx->vars;
    while (*cur) {
        if ((*cur)->scope_depth == ctx->depth) {
            *cur = (*cur)->next;
        } else {
            cur = &(*cur)->next;
        }
    }
    if (ctx->depth > 0) ctx->depth--;
}

void own_release_borrows_at_depth(OwnCtx *ctx, uint32_t depth) {
    BorrowRecord **cur = &ctx->borrows;
    while (*cur) {
        if ((*cur)->scope_depth >= depth) {
            /* Restore variable to owned state if borrow was the last one */
            OwnEntry *e = own_lookup(ctx, (*cur)->var);
            if (e && (e->state == MOVE_BORROWED || e->state == MOVE_BORROW_MUT))
                e->state = MOVE_OWNED;
            *cur = (*cur)->next;
        } else {
            cur = &(*cur)->next;
        }
    }
}

/* ── Variable registration ───────────────────────────────────────── */
OwnEntry *own_define(OwnCtx *ctx, Ident name, bool is_mut, bool is_copy) {
    OwnEntry *e = NEW(ctx, OwnEntry);
    e->name        = name;
    e->state       = MOVE_OWNED;
    e->is_mut      = is_mut;
    e->is_copy     = is_copy;
    e->scope_depth = ctx->depth;
    e->next        = ctx->vars;
    ctx->vars      = e;
    return e;
}

OwnEntry *own_lookup(OwnCtx *ctx, Ident name) {
    for (OwnEntry *e = ctx->vars; e; e = e->next)
        if (ident_eq(e->name, name)) return e;
    return NULL;
}

/* ── Move ────────────────────────────────────────────────────────── */
bool own_move(OwnCtx *ctx, Ident name, SrcSpan use_span) {
    OwnEntry *e = own_lookup(ctx, name);
    if (!e) return true; /* unknown var — sema already caught it */

    if (e->is_copy) return true; /* Copy types never move */

    if (e->state == MOVE_MOVED) {
        char buf[256];
        snprintf(buf, sizeof buf,
            "use of moved value '%.*s'", (int)name.len, name.ptr);
        char note[256];
        snprintf(note, sizeof note,
            "value was moved on line %u:%u",
            e->moved_at.start.line, e->moved_at.start.col);
        diag_error(ctx->diags, NULL, NULL, use_span, buf, note,
                   "clone the value before moving, or restructure to avoid the move");
        return false;
    }
    if (e->state == MOVE_BORROWED || e->state == MOVE_BORROW_MUT) {
        char buf[256];
        snprintf(buf, sizeof buf,
            "cannot move '%.*s' while borrowed", (int)name.len, name.ptr);
        diag_error(ctx->diags, NULL, NULL, use_span, buf,
                   "borrow must end before value can be moved", NULL);
        return false;
    }
    if (e->state == MOVE_PARTIAL) {
        char buf[256];
        snprintf(buf, sizeof buf,
            "use of partially moved value '%.*s'", (int)name.len, name.ptr);
        diag_error(ctx->diags, NULL, NULL, use_span, buf,
                   "some fields were moved out of this value", NULL);
        return false;
    }

    e->state    = MOVE_MOVED;
    e->moved_at = use_span;
    return true;
}

/* ── Borrow ──────────────────────────────────────────────────────── */
bool own_borrow(OwnCtx *ctx, Ident name, bool is_mut, SrcSpan span) {
    if (ctx->in_unsafe) return true; /* unsafe disables borrow rules */
    OwnEntry *e = own_lookup(ctx, name);
    if (!e) return true;

    if (e->state == MOVE_MOVED) {
        char buf[256];
        snprintf(buf, sizeof buf,
            "cannot borrow '%.*s' after it was moved", (int)name.len, name.ptr);
        diag_error(ctx->diags, NULL, NULL, span, buf, NULL, NULL);
        return false;
    }

    if (is_mut) {
        /* Mutable borrow: no other borrows allowed */
        if (e->state == MOVE_BORROWED || e->state == MOVE_BORROW_MUT) {
            char buf[256];
            snprintf(buf, sizeof buf,
                "cannot borrow '%.*s' as mutable: already borrowed",
                (int)name.len, name.ptr);
            diag_error(ctx->diags, NULL, NULL, span, buf,
                       "end all borrows before taking a mutable borrow", NULL);
            return false;
        }
        if (!e->is_mut) {
            char buf[256];
            snprintf(buf, sizeof buf,
                "cannot borrow '%.*s' as mutable: declared with 'let' (immutable)",
                (int)name.len, name.ptr);
            diag_error(ctx->diags, NULL, NULL, span, buf,
                       "change 'let' to 'var' to allow mutation", NULL);
            return false;
        }
        e->state = MOVE_BORROW_MUT;
    } else {
        /* Shared borrow: not allowed if mutable borrow active */
        if (e->state == MOVE_BORROW_MUT) {
            char buf[256];
            snprintf(buf, sizeof buf,
                "cannot borrow '%.*s' as shared: mutable borrow is active",
                (int)name.len, name.ptr);
            diag_error(ctx->diags, NULL, NULL, span, buf, NULL, NULL);
            return false;
        }
        e->state = MOVE_BORROWED;
    }

    BorrowRecord *br = NEW(ctx, BorrowRecord);
    br->var         = name;
    br->is_mut      = is_mut;
    br->span        = span;
    br->scope_depth = ctx->depth;
    br->next        = ctx->borrows;
    ctx->borrows    = br;
    return true;
}

/* ── Expression checker ──────────────────────────────────────────── */
bool own_check_expr(OwnCtx *ctx, Expr *e, bool is_lvalue) {
    if (!e) return true;
    switch (e->kind) {
        case EXPR_IDENT: {
            if (is_lvalue) return true; /* assignment target, not a read */
            /* Check: is it a move-out? Only if type is non-Copy */
            bool is_copy = e->type ? own_type_is_copy(e->type) : true;
            if (!is_copy) {
                return own_move(ctx, e->ident, e->span);
            }
            /* Copy type — just check not moved */
            OwnEntry *en = own_lookup(ctx, e->ident);
            if (en && en->state == MOVE_MOVED) {
                char buf[256];
                snprintf(buf, sizeof buf,
                    "use of moved value '%.*s'", (int)e->ident.len, e->ident.ptr);
                diag_error(ctx->diags, NULL, NULL, e->span, buf, NULL, NULL);
                return false;
            }
            return true;
        }
        case EXPR_UNARY:
            if (e->unary.op == UNOP_ADDR)
                return own_borrow(ctx, e->unary.operand->ident, false, e->span);
            if (e->unary.op == UNOP_DEREF)
                return own_check_expr(ctx, e->unary.operand, false);
            return own_check_expr(ctx, e->unary.operand, false);

        case EXPR_BINARY:
            if (token_is_assign_op(e->binary.op == BINOP_ASSIGN ?
                    TOK_ASSIGN : TOK_PLUS_EQ)) {
                bool ok = own_check_expr(ctx, e->binary.lhs, true);
                ok     &= own_check_expr(ctx, e->binary.rhs, false);
                /* Re-own the lhs after assignment */
                if (e->binary.lhs->kind == EXPR_IDENT) {
                    OwnEntry *en = own_lookup(ctx, e->binary.lhs->ident);
                    if (en) { en->state = MOVE_OWNED; }
                }
                return ok;
            }
            return own_check_expr(ctx, e->binary.lhs, false) &&
                   own_check_expr(ctx, e->binary.rhs, false);

        case EXPR_CALL: {
            bool ok = own_check_expr(ctx, e->call.callee, false);
            for (size_t i = 0; i < e->call.argc; i++)
                ok &= own_check_expr(ctx, e->call.args[i], false);
            return ok;
        }
        case EXPR_FIELD:
            return own_check_expr(ctx, e->field.obj, false);
        case EXPR_INDEX:
            return own_check_expr(ctx, e->index.obj, false) &&
                   own_check_expr(ctx, e->index.idx, false);
        case EXPR_IF:
            own_check_expr(ctx, e->if_expr.cond, false);
            own_push_scope(ctx);
            own_check_expr(ctx, e->if_expr.then_expr, false);
            own_pop_scope(ctx);
            if (e->if_expr.else_expr) {
                own_push_scope(ctx);
                own_check_expr(ctx, e->if_expr.else_expr, false);
                own_pop_scope(ctx);
            }
            return true;
        case EXPR_BLOCK: {
            own_push_scope(ctx);
            for (size_t i = 0; i < e->block.stmtc; i++)
                own_check_stmt(ctx, e->block.stmts[i]);
            if (e->block.tail)
                own_check_expr(ctx, e->block.tail, false);
            own_pop_scope(ctx);
            return true;
        }
        case EXPR_BUILTIN:
            for (size_t i = 0; i < e->builtin.argc; i++)
                own_check_expr(ctx, e->builtin.args[i], false);
            return true;
        default:
            return true;
    }
}

/* ── Statement checker ───────────────────────────────────────────── */
bool own_check_stmt(OwnCtx *ctx, Stmt *s) {
    if (!s) return true;
    switch (s->kind) {
        case STMT_LET: {
            bool ok = s->let.init ? own_check_expr(ctx, s->let.init, false) : true;
            bool is_copy = s->let.ty ? false : /* infer from init */ false;
            if (s->let.init && s->let.init->type)
                is_copy = own_type_is_copy(s->let.init->type);
            own_define(ctx, s->let.name, s->let.is_mut, is_copy);
            return ok;
        }
        case STMT_RETURN:
            return s->ret.value ? own_check_expr(ctx, s->ret.value, false) : true;
        case STMT_EXPR:
            return own_check_expr(ctx, s->expr_stmt, false);
        case STMT_FOR: {
            own_check_expr(ctx, s->for_s.iter, false);
            own_push_scope(ctx);
            own_define(ctx, s->for_s.bind, false, true);
            for (size_t i = 0; i < s->for_s.c; i++)
                own_check_stmt(ctx, s->for_s.body[i]);
            own_pop_scope(ctx);
            return true;
        }
        case STMT_WHILE: {
            own_check_expr(ctx, s->while_s.cond, false);
            own_push_scope(ctx);
            for (size_t i = 0; i < s->while_s.c; i++)
                own_check_stmt(ctx, s->while_s.body[i]);
            own_pop_scope(ctx);
            return true;
        }
        case STMT_UNSAFE: {
            bool old = ctx->in_unsafe;
            ctx->in_unsafe = true;
            own_push_scope(ctx);
            for (size_t i = 0; i < s->unsafe_s.count; i++)
                own_check_stmt(ctx, s->unsafe_s.body[i]);
            own_pop_scope(ctx);
            ctx->in_unsafe = old;
            return true;
        }
        case STMT_DEFER:
            return own_check_expr(ctx, (Expr*)s->defer_s.inner, false);
        default:
            return true;
    }
}

static bool ast_type_is_copy(Type *t) {
    if (!t) return false;
    switch(t->kind) {
        case TY_PRIM:
        case TY_PTR:  return true;
        default:      return false;
    }
}

/* ── Function checker ────────────────────────────────────────────── */
bool own_check_fn(OwnCtx *ctx, Decl *fn) {
    if (!fn->fn.body) return true;
    own_push_scope(ctx);
    /* Register parameters */
    for (size_t i = 0; i < fn->fn.paramc; i++) {
        bool is_copy = ast_type_is_copy(fn->fn.params[i].ty);
        own_define(ctx, fn->fn.params[i].name, false, is_copy);
    }
    bool ok = true;
    for (size_t i = 0; i < fn->fn.bodyc; i++)
        ok &= own_check_stmt(ctx, fn->fn.body[i]);
    own_pop_scope(ctx);
    return ok;
}

/* ── File checker ────────────────────────────────────────────────── */
bool own_check_file(OwnCtx *ctx, AstFile *file) {
    for (size_t i = 0; i < file->declc; i++) {
        Decl *d = file->decls[i];
        if (d->kind == DECL_FN) own_check_fn(ctx, d);
    }
    return !diag_had_errors(ctx->diags);
}
