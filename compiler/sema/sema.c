/* rsharp/compiler/sema/sema.c — R# Type Checker (C11) */
#include "sema.h"
#include <string.h>
#include <stdio.h>

#define NEW(ctx,T) ((T*)arena_calloc((ctx)->arena,1,sizeof(T)))
#define NEWA(ctx,T,n) ((T*)arena_calloc((ctx)->arena,(n),sizeof(T)))

/* ── Primitive type constructors ──────────────────────────────────── */
static ResolvedType *make_int(Arena *a, int w, bool s) {
    ResolvedType *t=arena_calloc(a,1,sizeof*t);
    t->kind=RTYPE_INT; t->int_ty.width=w; t->int_ty.is_signed=s; return t;
}
static ResolvedType *make_float(Arena *a, int w) {
    ResolvedType *t=arena_calloc(a,1,sizeof*t);
    t->kind=RTYPE_FLOAT; t->float_ty.width=w; return t;
}
static ResolvedType *make_simple(Arena *a, ResolvedTypeKind k) {
    ResolvedType *t=arena_calloc(a,1,sizeof*t); t->kind=k; return t;
}

SemaCtx sema_create(Arena *arena, DiagSink *diags) {
    SemaCtx ctx={0}; ctx.arena=arena; ctx.diags=diags;
    ctx.ty_i8   =make_int(arena,8,true);   ctx.ty_i16 =make_int(arena,16,true);
    ctx.ty_i32  =make_int(arena,32,true);  ctx.ty_i64 =make_int(arena,64,true);
    ctx.ty_i128 =make_int(arena,128,true);
    ctx.ty_u8   =make_int(arena,8,false);  ctx.ty_u16 =make_int(arena,16,false);
    ctx.ty_u32  =make_int(arena,32,false); ctx.ty_u64 =make_int(arena,64,false);
    ctx.ty_u128 =make_int(arena,128,false);
    ctx.ty_f32  =make_float(arena,32);     ctx.ty_f64 =make_float(arena,64);
    ctx.ty_bool =make_simple(arena,RTYPE_BOOL);
    ctx.ty_char =make_simple(arena,RTYPE_CHAR);
    ctx.ty_void =make_simple(arena,RTYPE_VOID);
    ctx.ty_str  =make_simple(arena,RTYPE_STR);
    ctx.ty_never=make_simple(arena,RTYPE_NEVER);
    /* push global scope */
    sema_push_scope(&ctx, true);
    return ctx;
}

/* ── Scope ────────────────────────────────────────────────────────── */
void sema_push_scope(SemaCtx *ctx, bool is_fn_root) {
    Scope *s = arena_calloc(ctx->arena,1,sizeof*s);
    s->cap=8; s->syms=arena_calloc(ctx->arena,8,sizeof(Symbol*));
    s->parent=ctx->scope; s->is_fn_root=is_fn_root;
    ctx->scope=s;
}
void sema_pop_scope(SemaCtx *ctx) {
    if (ctx->scope) ctx->scope=ctx->scope->parent;
}

Symbol *sema_define(SemaCtx *ctx, Ident name, SymbolKind kind, ResolvedType *ty, bool is_mut) {
    Scope *s=ctx->scope;
    if (s->count==s->cap) {
        size_t new_cap = s->cap * 2;
        Symbol **new_syms = arena_calloc(ctx->arena, new_cap, sizeof(Symbol*));
        memcpy(new_syms, s->syms, s->count * sizeof(Symbol*));
        s->syms = new_syms;
        s->cap = new_cap;
    }
    Symbol *sym=arena_calloc(ctx->arena,1,sizeof*sym);
    sym->name=name; sym->kind=kind; sym->type=ty; sym->is_mut=is_mut;
    s->syms[s->count++]=sym;
    return sym;
}

Symbol *sema_lookup(SemaCtx *ctx, Ident name) {
    for (Scope *s=ctx->scope; s; s=s->parent)
        for (size_t i=0;i<s->count;i++)
            if (s->syms[i]->name.len==name.len &&
                memcmp(s->syms[i]->name.ptr,name.ptr,name.len)==0)
                return s->syms[i];
    return NULL;
}

/* ── Type resolution ──────────────────────────────────────────────── */
static ResolvedType *resolve_type(SemaCtx *ctx, const Type *t) {
    if (!t) return ctx->ty_void;
    switch (t->kind) {
        case TY_PRIM: switch(t->prim) {
            case PRIM_I8:   return ctx->ty_i8;   case PRIM_I16:  return ctx->ty_i16;
            case PRIM_I32:  return ctx->ty_i32;   case PRIM_I64:  return ctx->ty_i64;
            case PRIM_I128: return ctx->ty_i128;
            case PRIM_U8:   return ctx->ty_u8;   case PRIM_U16:  return ctx->ty_u16;
            case PRIM_U32:  return ctx->ty_u32;   case PRIM_U64:  return ctx->ty_u64;
            case PRIM_U128: return ctx->ty_u128;
            case PRIM_F32:  return ctx->ty_f32;  case PRIM_F64:  return ctx->ty_f64;
            case PRIM_BOOL: return ctx->ty_bool; case PRIM_CHAR: return ctx->ty_char;
            case PRIM_VOID: return ctx->ty_void;
            default:        return ctx->ty_void;
        }
        case TY_OPTIONAL: {
            ResolvedType *r=NEW(ctx,ResolvedType);
            r->kind=RTYPE_OPTIONAL; r->opt_ty.inner=resolve_type(ctx,t->optional.inner);
            return r;
        }
        case TY_PTR: {
            ResolvedType *r=NEW(ctx,ResolvedType);
            r->kind=RTYPE_PTR; r->ptr_ty.inner=resolve_type(ctx,t->ptr.inner);
            return r;
        }
        case TY_SLICE: {
            ResolvedType *r=NEW(ctx,ResolvedType);
            r->kind=RTYPE_SLICE; r->slice_ty.elem=resolve_type(ctx,t->slice.elem);
            return r;
        }
        case TY_NAMED: {
            Symbol *sym = sema_lookup(ctx, t->named.name);
            if (!sym || (sym->kind != SYM_STRUCT && sym->kind != SYM_ENUM)) {
                char buf[128];
                snprintf(buf, sizeof buf, "undefined type '%.*s'", (int)t->named.name.len, t->named.name.ptr);
                diag_error(ctx->diags, NULL, NULL, t->span, buf, NULL, NULL);
                return ctx->ty_void;
            }
            return sym->type;
        }
        case TY_RESULT: {
            ResolvedType *r = NEW(ctx, ResolvedType);
            r->kind = RTYPE_RESULT;
            r->result_ty.ok = resolve_type(ctx, t->result.ok);
            r->result_ty.err = resolve_type(ctx, t->result.err);
            return r;
        }
        case TY_ARRAY: {
            ResolvedType *r = NEW(ctx, ResolvedType);
            r->kind = RTYPE_ARRAY;
            r->array_ty.elem = resolve_type(ctx, t->array.elem);
            r->array_ty.count = 0; /* Should evaluate count expression, but 0 for now */
            return r;
        }
        case TY_TUPLE: {
            ResolvedType *r = NEW(ctx, ResolvedType);
            r->kind = RTYPE_TUPLE;
            r->tuple_ty.elems = NEWA(ctx, ResolvedType*, t->tuple.count);
            r->tuple_ty.count = t->tuple.count;
            for (size_t i = 0; i < t->tuple.count; i++)
                r->tuple_ty.elems[i] = resolve_type(ctx, t->tuple.elems[i]);
            return r;
        }
        case TY_FN: {
            ResolvedType *r = NEW(ctx, ResolvedType);
            r->kind = RTYPE_FN;
            r->fn_ty.params = NEWA(ctx, ResolvedType*, t->fn.paramc);
            r->fn_ty.pc = t->fn.paramc;
            for (size_t i = 0; i < t->fn.paramc; i++)
                r->fn_ty.params[i] = resolve_type(ctx, t->fn.params[i]);
            r->fn_ty.ret = resolve_type(ctx, t->fn.ret);
            return r;
        }
        case TY_INFER:  return ctx->ty_i32; /* default integer width */
        default:        return ctx->ty_void;
    }
}

/* ── Type equality ────────────────────────────────────────────────── */
bool sema_types_equal(const ResolvedType *a, const ResolvedType *b) {
    if (!a||!b) return a==b;
    if (a->kind!=b->kind) return false;
    switch(a->kind) {
        case RTYPE_INT:   return a->int_ty.width==b->int_ty.width && a->int_ty.is_signed==b->int_ty.is_signed;
        case RTYPE_FLOAT: return a->float_ty.width==b->float_ty.width;
        case RTYPE_PTR:   return sema_types_equal(a->ptr_ty.inner,b->ptr_ty.inner);
        case RTYPE_SLICE: return sema_types_equal(a->slice_ty.elem,b->slice_ty.elem);
        case RTYPE_OPTIONAL: return sema_types_equal(a->opt_ty.inner,b->opt_ty.inner);
        default: return true;
    }
}

const char *sema_type_name(const ResolvedType *t, Arena *scratch) {
    if (!t) return "?";
    switch(t->kind) {
        case RTYPE_VOID:  return "void";  case RTYPE_BOOL:  return "bool";
        case RTYPE_CHAR:  return "char";  case RTYPE_STR:   return "str";
        case RTYPE_NEVER: return "!";
        case RTYPE_INT: {
            char *buf=arena_alloc(scratch,8,1);
            snprintf(buf,8,"%c%d",t->int_ty.is_signed?'i':'u',t->int_ty.width);
            return buf;
        }
        case RTYPE_FLOAT: {
            char *buf=arena_alloc(scratch,8,1);
            snprintf(buf,8,"f%d",t->float_ty.width);
            return buf;
        }
        case RTYPE_OPTIONAL: {
            const char *inner=sema_type_name(t->opt_ty.inner,scratch);
            size_t l=strlen(inner)+2;
            char *buf=arena_alloc(scratch,l,1);
            snprintf(buf,l,"?%s",inner);
            return buf;
        }
        case RTYPE_PTR: {
            const char *inner=sema_type_name(t->ptr_ty.inner,scratch);
            size_t l=strlen(inner)+2;
            char *buf=arena_alloc(scratch,l,1);
            snprintf(buf,l,"*%s",inner);
            return buf;
        }
        default: return "<type>";
    }
}

/* ── Expression type-checking ─────────────────────────────────────── */
ResolvedType *sema_check_expr(SemaCtx *ctx, Expr *e) {
    if (!e) return ctx->ty_void;
    switch(e->kind) {
        case EXPR_INT_LIT:   e->type=ctx->ty_i32; break;
        case EXPR_FLOAT_LIT: e->type=ctx->ty_f64; break;
        case EXPR_BOOL_LIT:  e->type=ctx->ty_bool; break;
        case EXPR_STR_LIT:   e->type=ctx->ty_str; break;
        case EXPR_NULL:      {
            ResolvedType *r=NEW(ctx,ResolvedType);
            r->kind=RTYPE_OPTIONAL; r->opt_ty.inner=ctx->ty_void;
            e->type=r; break;
        }
        case EXPR_IDENT: {
            Symbol *sym=sema_lookup(ctx,e->ident);
            if (!sym) {
                char buf[128];
                snprintf(buf,sizeof buf,"undefined symbol '%.*s'",(int)e->ident.len,e->ident.ptr);
                diag_error(ctx->diags,NULL,NULL,e->span,buf,NULL,
                           "declare it with 'let' or 'var' before use");
                e->type=ctx->ty_never; break;
            }
            if (sym->moved) {
                char buf[128];
                snprintf(buf,sizeof buf,"use of moved value '%.*s'",(int)e->ident.len,e->ident.ptr);
                diag_error(ctx->diags,NULL,NULL,e->span,buf,
                           "value was moved earlier","add '.clone()' to copy, or restructure to avoid the move");
            }
            e->type=sym->type; break;
        }
        case EXPR_UNARY: {
            ResolvedType *inner=sema_check_expr(ctx,e->unary.operand);
            if (e->unary.op == UNOP_NOT) {
                e->type = ctx->ty_bool;
            } else if (e->unary.op == UNOP_DEREF) {
                e->type = (inner->kind == RTYPE_PTR) ? inner->ptr_ty.inner : ctx->ty_void;
            } else if (e->unary.op == UNOP_ADDR) {
                ResolvedType *r = NEW(ctx, ResolvedType);
                r->kind = RTYPE_PTR;
                r->ptr_ty.inner = inner;
                r->ptr_ty.is_mut = false;
                e->type = r;
            } else {
                e->type = inner;
            }
            break;
        }
        case EXPR_BINARY: {
            ResolvedType *lhs=sema_check_expr(ctx,e->binary.lhs);
            ResolvedType *rhs=sema_check_expr(ctx,e->binary.rhs);
            BinOp op=e->binary.op;
            /* Comparison ops return bool */
            if (op==BINOP_EQ||op==BINOP_NEQ||op==BINOP_LT||op==BINOP_GT||
                op==BINOP_LE||op==BINOP_GE||op==BINOP_AND||op==BINOP_OR)
                e->type=ctx->ty_bool;
            else if (!sema_types_equal(lhs,rhs)) {
                Arena *scratch=ctx->arena;
                char buf[128];
                snprintf(buf,sizeof buf,"type mismatch: %s vs %s",
                         sema_type_name(lhs,scratch),sema_type_name(rhs,scratch));
                diag_error(ctx->diags,NULL,NULL,e->span,buf,NULL,
                           "coerce with @cast(T, value) if intentional");
                e->type=lhs;
            } else { e->type=lhs; }
            break;
        }
        case EXPR_CALL: {
            /* Simplified: look up function, return its ret type */
            ResolvedType *callee=sema_check_expr(ctx,e->call.callee);
            for (size_t i=0;i<e->call.argc;i++) sema_check_expr(ctx,e->call.args[i]);
            e->type = (callee->kind==RTYPE_FN) ? callee->fn_ty.ret : ctx->ty_void;
            break;
        }
        case EXPR_IF: {
            ResolvedType *cond=sema_check_expr(ctx,e->if_expr.cond);
            if (cond->kind!=RTYPE_BOOL)
                diag_error(ctx->diags,NULL,NULL,e->span,
                           "if condition must be bool",NULL,NULL);
            sema_push_scope(ctx,false);
            ResolvedType *then_ty=sema_check_expr(ctx,e->if_expr.then_expr);
            sema_pop_scope(ctx);
            if (e->if_expr.else_expr) {
                sema_push_scope(ctx,false);
                sema_check_expr(ctx,e->if_expr.else_expr);
                sema_pop_scope(ctx);
            }
            e->type=then_ty; break;
        }
        case EXPR_BLOCK: {
            sema_push_scope(ctx,false);
            for (size_t i=0;i<e->block.stmtc;i++) sema_check_stmt(ctx,e->block.stmts[i]);
            ResolvedType *tail=e->block.tail ? sema_check_expr(ctx,e->block.tail) : ctx->ty_void;
            sema_pop_scope(ctx);
            e->type=tail; break;
        }
        case EXPR_BUILTIN: {
            for (size_t i=0;i<e->builtin.argc;i++) sema_check_expr(ctx,e->builtin.args[i]);
            Ident name = e->builtin.name;
            if (name.len == 4 && memcmp(name.ptr, "sqrt", 4) == 0) e->type = ctx->ty_f64;
            else if (name.len == 3 && memcmp(name.ptr, "len", 3) == 0) e->type = ctx->ty_i64;
            else e->type = ctx->ty_void;
            break;
        }
        case EXPR_CHAR_LIT:  e->type=ctx->ty_char; break;
        case EXPR_STRUCT_LIT: {
            ResolvedType *ty = resolve_type(ctx, e->struct_lit.ty);
            for (size_t i = 0; i < e->struct_lit.fieldc; i++)
                sema_check_expr(ctx, e->struct_lit.fields[i].value);
            e->type = ty;
            break;
        }
        case EXPR_ARRAY_LIT: {
            ResolvedType *elem_ty = ctx->ty_void;
            if (e->array_lit.count > 0) {
                elem_ty = sema_check_expr(ctx, e->array_lit.elems[0]);
                for (size_t i = 1; i < e->array_lit.count; i++)
                    sema_check_expr(ctx, e->array_lit.elems[i]);
            }
            ResolvedType *r = NEW(ctx, ResolvedType);
            r->kind = RTYPE_ARRAY;
            r->array_ty.elem = elem_ty;
            r->array_ty.count = e->array_lit.count;
            e->type = r;
            break;
        }
        case EXPR_TUPLE_LIT: {
            ResolvedType *r = NEW(ctx, ResolvedType);
            r->kind = RTYPE_TUPLE;
            r->tuple_ty.count = e->array_lit.count;
            r->tuple_ty.elems = NEWA(ctx, ResolvedType*, e->array_lit.count);
            for (size_t i = 0; i < e->array_lit.count; i++)
                r->tuple_ty.elems[i] = sema_check_expr(ctx, e->array_lit.elems[i]);
            e->type = r;
            break;
        }
        case EXPR_INDEX: {
            ResolvedType *obj = sema_check_expr(ctx, e->index.obj);
            sema_check_expr(ctx, e->index.idx);
            if (obj->kind == RTYPE_ARRAY) e->type = obj->array_ty.elem;
            else if (obj->kind == RTYPE_SLICE) e->type = obj->slice_ty.elem;
            else e->type = ctx->ty_void;
            break;
        }
        case EXPR_FIELD: {
            ResolvedType *obj = sema_check_expr(ctx, e->field.obj);
            e->type = ctx->ty_void; 
            if (obj && obj->kind == RTYPE_STRUCT) {
                Decl *d = obj->named_ty.decl;
                for (size_t i = 0; i < d->struc.fieldc; i++) {
                    if (d->struc.fields[i].name.len == e->field.field.len &&
                        memcmp(d->struc.fields[i].name.ptr, e->field.field.ptr, e->field.field.len) == 0) {
                        e->type = resolve_type(ctx, d->struc.fields[i].ty);
                        break;
                    }
                }
                if (e->type == ctx->ty_void) {
                    diag_error(ctx->diags, NULL, NULL, e->span, "struct has no such field", NULL, NULL);
                }
            } else if (obj && obj->kind == RTYPE_ENUM) {
                /* Enum variant access like State.Idle handled elsewhere or as static, but if instances have it... */
            }
            break;
        }
        case EXPR_MATCH: {
            sema_check_expr(ctx, e->match.subject);
            ResolvedType *arm_ty = ctx->ty_void;
            for (size_t i = 0; i < e->match.armc; i++) {
                sema_push_scope(ctx, false);
                sema_check_expr(ctx, e->match.arms[i].pattern);
                if (e->match.arms[i].guard) sema_check_expr(ctx, e->match.arms[i].guard);
                ResolvedType *t = sema_check_expr(ctx, e->match.arms[i].body);
                if (i == 0) {
                    arm_ty = t;
                } else if (!sema_types_equal(arm_ty, t) && t != ctx->ty_never) {
                    diag_error(ctx->diags, NULL, NULL, e->match.arms[i].body->span, "match arms have incompatible types", NULL, NULL);
                }
                sema_pop_scope(ctx);
            }
            e->type = arm_ty;
            break;
        }
        case EXPR_LOOP: {
            sema_push_scope(ctx, false);
            bool old = ctx->in_loop; ctx->in_loop = true;
            for (size_t i = 0; i < e->block.stmtc; i++) sema_check_stmt(ctx, e->block.stmts[i]);
            ctx->in_loop = old;
            sema_pop_scope(ctx);
            e->type = ctx->ty_void;
            break;
        }
        case EXPR_CAST: {
            sema_check_expr(ctx, e->cast.value);
            e->type = resolve_type(ctx, e->cast.target);
            break;
        }
        case EXPR_TRY: {
            ResolvedType *inner = sema_check_expr(ctx, e->unary.operand);
            if (inner->kind == RTYPE_RESULT) e->type = inner->result_ty.ok;
            else if (inner->kind == RTYPE_OPTIONAL) e->type = inner->opt_ty.inner;
            else e->type = inner;
            break;
        }
        case EXPR_CATCH: {
            ResolvedType *inner = sema_check_expr(ctx, e->catch_expr.expr);
            sema_push_scope(ctx, false);
            ResolvedType *err_ty = ctx->ty_void;
            if (inner->kind == RTYPE_RESULT) err_ty = inner->result_ty.err;
            sema_define(ctx, e->catch_expr.err_bind, SYM_LOCAL, err_ty, false);
            ResolvedType *handler_ty = sema_check_expr(ctx, e->catch_expr.handler);
            sema_pop_scope(ctx);
            if (inner->kind == RTYPE_RESULT) e->type = inner->result_ty.ok;
            else e->type = handler_ty;
            break;
        }
        case EXPR_RANGE: {
            sema_check_expr(ctx, e->range.start);
            sema_check_expr(ctx, e->range.end);
            e->type = ctx->ty_void;
            break;
        }
        case EXPR_COMPTIME: {
            e->type = sema_check_expr(ctx, e->comptime.inner);
            break;
        }
        case EXPR_AWAIT: {
            e->type = sema_check_expr(ctx, e->await_expr.inner);
            break;
        }
        case EXPR_CLOSURE: {
            ResolvedType *r = NEW(ctx, ResolvedType);
            r->kind = RTYPE_FN;
            r->fn_ty.params = NEWA(ctx, ResolvedType*, e->closure.paramc);
            r->fn_ty.pc = e->closure.paramc;
            sema_push_scope(ctx, false);
            for (size_t i = 0; i < e->closure.paramc; i++) {
                ResolvedType *pt = resolve_type(ctx, e->closure.params[i].ty);
                r->fn_ty.params[i] = pt;
                sema_define(ctx, e->closure.params[i].name, SYM_PARAM, pt, false);
            }
            r->fn_ty.ret = resolve_type(ctx, e->closure.ret);
            sema_check_expr(ctx, e->closure.body);
            sema_pop_scope(ctx);
            e->type = r;
            break;
        }
        default: e->type=ctx->ty_void; break;
    }
    return e->type;
}

bool sema_check_stmt(SemaCtx *ctx, Stmt *s) {
    if (!s) return true;
    switch(s->kind) {
        case STMT_LET: {
            ResolvedType *init_ty = s->let.init ? sema_check_expr(ctx,s->let.init) : ctx->ty_void;
            ResolvedType *ann_ty  = s->let.ty   ? resolve_type(ctx,s->let.ty)      : init_ty;
            if (s->let.init && s->let.ty && !sema_types_equal(init_ty,ann_ty)) {
                Arena *sc=ctx->arena;
                char buf[128];
                snprintf(buf,sizeof buf,"type mismatch: annotated %s, but init is %s",
                         sema_type_name(ann_ty,sc),sema_type_name(init_ty,sc));
                diag_error(ctx->diags,NULL,NULL,s->span,buf,NULL,NULL);
            }
            sema_define(ctx,s->let.name,SYM_LOCAL,ann_ty,s->let.is_mut);
            break;
        }
        case STMT_RETURN: {
            ResolvedType *ret=s->ret.value ? sema_check_expr(ctx,s->ret.value) : ctx->ty_void;
            if (ctx->current_ret_type && !sema_types_equal(ret,ctx->current_ret_type)) {
                Arena *sc=ctx->arena;
                char buf[128];
                snprintf(buf,sizeof buf,"return type mismatch: expected %s, got %s",
                         sema_type_name(ctx->current_ret_type,sc),sema_type_name(ret,sc));
                diag_error(ctx->diags,NULL,NULL,s->span,buf,NULL,NULL);
            }
            break;
        }
        case STMT_EXPR: sema_check_expr(ctx,s->expr_stmt); break;
        case STMT_FOR: {
            sema_check_expr(ctx,s->for_s.iter);
            sema_push_scope(ctx,false);
            bool old=ctx->in_loop; ctx->in_loop=true;
            sema_define(ctx,s->for_s.bind,SYM_LOCAL,ctx->ty_i32,true);
            for (size_t i=0;i<s->for_s.c;i++) sema_check_stmt(ctx,s->for_s.body[i]);
            ctx->in_loop=old;
            sema_pop_scope(ctx);
            break;
        }
        case STMT_WHILE: {
            sema_check_expr(ctx,s->while_s.cond);
            sema_push_scope(ctx,false);
            bool old=ctx->in_loop; ctx->in_loop=true;
            for (size_t i=0;i<s->while_s.c;i++) sema_check_stmt(ctx,s->while_s.body[i]);
            ctx->in_loop=old;
            sema_pop_scope(ctx);
            break;
        }
        case STMT_BREAK:
            if (!ctx->in_loop)
                diag_error(ctx->diags,NULL,NULL,s->span,"'break' outside of loop",NULL,NULL);
            break;
        case STMT_CONTINUE:
            if (!ctx->in_loop)
                diag_error(ctx->diags,NULL,NULL,s->span,"'continue' outside of loop",NULL,NULL);
            break;
        default: break;
    }
    return !diag_had_errors(ctx->diags);
}

bool sema_check_file(SemaCtx *ctx, AstFile *file) {
    /* First pass: register all top-level declarations (for forward references) */
    for (size_t i=0;i<file->declc;i++) {
        Decl *d=file->decls[i];
        if (d->kind==DECL_FN) {
            ResolvedType *ft=NEW(ctx,ResolvedType);
            ft->kind=RTYPE_FN;
            ft->fn_ty.ret=resolve_type(ctx,d->fn.ret_ty);
            sema_define(ctx,(Ident){d->fn.name.ptr,d->fn.name.len},SYM_FN,ft,false);
        } else if (d->kind==DECL_STRUCT) {
            ResolvedType *st=NEW(ctx,ResolvedType);
            st->kind=RTYPE_STRUCT; st->named_ty.decl=d;
            sema_define(ctx,(Ident){d->struc.name.ptr,d->struc.name.len},SYM_STRUCT,st,false);
        } else if (d->kind==DECL_ENUM) {
            ResolvedType *et=NEW(ctx,ResolvedType);
            et->kind=RTYPE_ENUM; et->named_ty.decl=d;
            sema_define(ctx,(Ident){d->enm.name.ptr,d->enm.name.len},SYM_ENUM,et,false);
        }
    }
    /* Second pass: type-check bodies */
    for (size_t i=0;i<file->declc;i++) {
        Decl *d=file->decls[i];
        if (d->kind==DECL_FN && d->fn.body) {
            sema_push_scope(ctx,true);
            ResolvedType *old=ctx->current_ret_type;
            ctx->current_ret_type=resolve_type(ctx,d->fn.ret_ty);
            for (size_t j=0;j<d->fn.paramc;j++) {
                ResolvedType *pty=resolve_type(ctx,d->fn.params[j].ty);
                sema_define(ctx,d->fn.params[j].name,SYM_PARAM,pty,false);
            }
            for (size_t j=0;j<d->fn.bodyc;j++) sema_check_stmt(ctx,d->fn.body[j]);
            ctx->current_ret_type=old;
            sema_pop_scope(ctx);
        }
    }
    return !diag_had_errors(ctx->diags);
}
