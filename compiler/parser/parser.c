/* rsharp/compiler/parser/parser.c
 * R# Recursive-Descent Parser — C11
 *
 * Expression precedence (low → high):
 *  1  assignment    = += -= *= /=
 *  2  nullcoal      ??
 *  3  logical or    ||
 *  4  logical and   &&
 *  5  comparison    == != < > <= >=
 *  6  bitwise or    |
 *  7  bitwise xor   ^
 *  8  bitwise and   &
 *  9  shift         << >>
 * 10  additive      + -
 * 11  multiplicative * / %
 * 12  unary         - ! ~ & *
 * 13  postfix       call [] . await
 * 14  primary       literals, ident, (expr), block, if, match, loop
 */

#include "parser.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ─── Arena helpers ─────────────────────────────────────────────── */
#define NEW(p, T)        ((T *)arena_calloc((p)->arena, 1, sizeof(T)))
#define NEW_ARR(p, T, n) ((T *)arena_calloc((p)->arena, (n), sizeof(T)))

#define GROW_ARR(p, arr, count, cap, T) do { \
    if ((count) >= (cap)) { \
        size_t new_cap = (cap) * 2; \
        T *new_arr = NEW_ARR(p, T, new_cap); \
        memcpy(new_arr, arr, (count) * sizeof(T)); \
        arr = new_arr; \
        cap = new_cap; \
    } \
} while(0)

/* ─── Token navigation ──────────────────────────────────────────── */
static void advance_tok(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = lexer_next(p->lexer);
        if (p->current.kind != TOK_ERROR) break;
        /* Report lex error and keep going */
        diag_error(p->diags,
                   p->lexer->filepath, p->lexer->src,
                   p->current.span,
                   p->current.str.ptr ? p->current.str.ptr : "unexpected character",
                   NULL, NULL);
        p->had_error = true;
    }
}

static bool check(const Parser *p, TokenKind k) { return p->current.kind == k; }

static bool match_tok(Parser *p, TokenKind k) {
    if (!check(p, k)) return false;
    advance_tok(p);
    return true;
}

static Token consume(Parser *p, TokenKind k, const char *msg) {
    if (check(p, k)) { advance_tok(p); return p->previous; }
    if (!p->panic_mode) {
        char buf[256];
        snprintf(buf, sizeof(buf), "expected %s, found %s",
                 msg, token_kind_name(p->current.kind));
        diag_error(p->diags, p->lexer->filepath, p->lexer->src,
                   p->current.span, buf, NULL, NULL);
        p->had_error  = true;
        p->panic_mode = true;
    }
    return p->current;
}

/* Synchronise after error: skip to next statement boundary */
static void synchronise(Parser *p) {
    p->panic_mode = false;
    while (!check(p, TOK_EOF)) {
        if (p->previous.kind == TOK_SEMI) return;
        switch (p->current.kind) {
            case TOK_FN: case TOK_LET: case TOK_VAR: case TOK_STRUCT:
            case TOK_ENUM: case TOK_IMPL: case TOK_FOR: case TOK_WHILE:
            case TOK_LOOP: case TOK_RETURN: case TOK_IF: case TOK_MATCH:
                return;
            default: break;
        }
        advance_tok(p);
    }
}

static SrcSpan span_from(SrcPos start, SrcPos end) {
    return (SrcSpan){ start, end };
}

/* ─── Type parsing ──────────────────────────────────────────────── */

static Type *parse_type(Parser *p);

static PrimKind tok_to_prim(TokenKind k) {
    switch (k) {
        case TOK_KW_I8:   return PRIM_I8;   case TOK_KW_I16:  return PRIM_I16;
        case TOK_KW_I32:  return PRIM_I32;  case TOK_KW_I64:  return PRIM_I64;
        case TOK_KW_I128: return PRIM_I128; case TOK_KW_U8:   return PRIM_U8;
        case TOK_KW_U16:  return PRIM_U16;  case TOK_KW_U32:  return PRIM_U32;
        case TOK_KW_U64:  return PRIM_U64;  case TOK_KW_U128: return PRIM_U128;
        case TOK_KW_F32:  return PRIM_F32;  case TOK_KW_F64:  return PRIM_F64;
        case TOK_KW_BOOL: return PRIM_BOOL; case TOK_KW_CHAR: return PRIM_CHAR;
        case TOK_KW_VOID: return PRIM_VOID;
        default: return PRIM_VOID;
    }
}

static bool is_prim_type_tok(TokenKind k) {
    return k >= TOK_KW_I8 && k <= TOK_KW_STR;
}

static Type *parse_type(Parser *p) {
    SrcPos start = p->current.span.start;
    Type  *t     = NEW(p, Type);

    /* Primitive types */
    if (is_prim_type_tok(p->current.kind)) {
        t->kind = TY_PRIM;
        t->prim = tok_to_prim(p->current.kind);
        advance_tok(p);
        t->span = span_from(start, p->previous.span.end);
        return t;
    }

    /* Optional ?T */
    if (match_tok(p, TOK_QMARK)) {
        t->kind = TY_OPTIONAL;
        t->optional.inner = parse_type(p);
        t->span = span_from(start, p->previous.span.end);
        return t;
    }

    /* Pointer *T */
    if (match_tok(p, TOK_STAR)) {
        t->kind = TY_PTR;
        t->ptr.inner = parse_type(p);
        t->span = span_from(start, p->previous.span.end);
        return t;
    }

    /* Slice []T */
    if (match_tok(p, TOK_LBRACK)) {
        if (match_tok(p, TOK_RBRACK)) {
            t->kind = TY_SLICE;
            t->slice.elem = parse_type(p);
        } else {
            /* Array [T; N] */
            t->kind = TY_ARRAY;
            t->array.elem  = parse_type(p);
            consume(p, TOK_SEMI, ";");
            t->array.count = NULL; /* expr parsed below if needed */
            consume(p, TOK_RBRACK, "]");
        }
        t->span = span_from(start, p->previous.span.end);
        return t;
    }

    /* Tuple (A, B, C) */
    if (match_tok(p, TOK_LPAREN)) {
        t->kind = TY_TUPLE;
        size_t cap = 4;
        t->tuple.elems = NEW_ARR(p, Type*, cap);
        t->tuple.count = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                GROW_ARR(p, t->tuple.elems, t->tuple.count, cap, Type*);
                t->tuple.elems[t->tuple.count++] = parse_type(p);
            } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
        }
        consume(p, TOK_RPAREN, ")");
        t->span = span_from(start, p->previous.span.end);
        return t;
    }

    /* fn(A, B) -> C */
    if (match_tok(p, TOK_FN)) {
        t->kind = TY_FN;
        consume(p, TOK_LPAREN, "(");
        size_t cap = 4;
        t->fn.params  = NEW_ARR(p, Type*, cap);
        t->fn.paramc  = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                GROW_ARR(p, t->fn.params, t->fn.paramc, cap, Type*);
                t->fn.params[t->fn.paramc++] = parse_type(p);
            } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
        }
        consume(p, TOK_RPAREN, ")");
        if (match_tok(p, TOK_ARROW)||match_tok(p, TOK_FAT_ARROW)) {
            t->fn.ret = parse_type(p);
        } else {
            Type *v = NEW(p, Type); v->kind = TY_PRIM; v->prim = PRIM_VOID;
            t->fn.ret = v;
        }
        t->span = span_from(start, p->previous.span.end);
        return t;
    }

    /* Named type: Ident or Ident::Ident */
    if (check(p, TOK_IDENT)) {
        t->kind = TY_NAMED;
        advance_tok(p);
        t->named.name = (Ident){ p->previous.str.ptr, p->previous.str.len };
        /* Generics: Foo<T, E> — simplified: Result<T, E> */
        t->named.args = NULL; t->named.argc = 0;
        if (match_tok(p, TOK_LT)) {
            size_t cap = 4;
            t->named.args = NEW_ARR(p, Type*, cap);
            do {
                GROW_ARR(p, t->named.args, t->named.argc, cap, Type*);
                t->named.args[t->named.argc++] = parse_type(p);
            } while (match_tok(p, TOK_COMMA) && !check(p, TOK_GT));
            consume(p, TOK_GT, ">");
        }
        t->span = span_from(start, p->previous.span.end);
        return t;
    }

    /* Infer _ */
    if (match_tok(p, TOK_IDENT)) { /* handled above */ }
    t->kind = TY_INFER;
    t->span = span_from(start, p->previous.span.end);
    return t;
}

/* ─── Expression parsing (Pratt / precedence climbing) ──────────── */

static Expr *parse_expr(Parser *p);
static Expr *parse_expr_prec(Parser *p, int min_prec);
static Expr *parse_primary(Parser *p);
static Expr *parse_unary(Parser *p);

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGN,     /* = += -= *= /= */
    PREC_NULLCOAL,   /* ??             */
    PREC_OR,         /* ||             */
    PREC_AND,        /* &&             */
    PREC_CMP,        /* == != < > <= >= */
    PREC_BITOR,      /* |              */
    PREC_BITXOR,     /* ^              */
    PREC_BITAND,     /* &              */
    PREC_SHIFT,      /* << >>          */
    PREC_ADD,        /* + -            */
    PREC_MUL,        /* * / %          */
    PREC_UNARY,      /* - ! ~ & *      */
    PREC_POSTFIX,    /* () [] . await  */
    PREC_PRIMARY,
} Prec;

static int infix_prec(TokenKind k) {
    switch (k) {
        case TOK_ASSIGN: case TOK_PLUS_EQ: case TOK_MINUS_EQ:
        case TOK_STAR_EQ: case TOK_SLASH_EQ:     return PREC_ASSIGN;
        case TOK_QMARKQMARK:                      return PREC_NULLCOAL;
        case TOK_LOGOR:                              return PREC_OR;
        case TOK_LOGAND:                             return PREC_AND;
        case TOK_EQ: case TOK_NEQ:
        case TOK_LT: case TOK_GT:
        case TOK_LE: case TOK_GE:                return PREC_CMP;
        case TOK_PIPE:                            return PREC_BITOR;
        case TOK_CARET:                           return PREC_BITXOR;
        case TOK_AMP:                             return PREC_BITAND;
        case TOK_SHL: case TOK_SHR:               return PREC_SHIFT;
        case TOK_PLUS: case TOK_MINUS:            return PREC_ADD;
        case TOK_STAR: case TOK_SLASH:
        case TOK_PERCENT:                         return PREC_MUL;
        case TOK_LPAREN: case TOK_LBRACK:
        case TOK_DOT: case TOK_AWAIT:             return PREC_POSTFIX;
        default: return PREC_NONE;
    }
}

static bool is_right_assoc(TokenKind k) {
    return k == TOK_ASSIGN || k == TOK_PLUS_EQ ||
           k == TOK_MINUS_EQ || k == TOK_STAR_EQ || k == TOK_SLASH_EQ;
}

static BinOp tok_to_binop(TokenKind k) {
    switch (k) {
        case TOK_PLUS:      return BINOP_ADD;  case TOK_MINUS:    return BINOP_SUB;
        case TOK_STAR:      return BINOP_MUL;  case TOK_SLASH:    return BINOP_DIV;
        case TOK_PERCENT:   return BINOP_MOD;
        case TOK_LOGAND:       return BINOP_AND;  case TOK_LOGOR:       return BINOP_OR;
        case TOK_AMP:       return BINOP_BITAND; case TOK_PIPE:   return BINOP_BITOR;
        case TOK_CARET:     return BINOP_BITXOR;
        case TOK_SHL:       return BINOP_SHL;  case TOK_SHR:      return BINOP_SHR;
        case TOK_EQ:        return BINOP_EQ;   case TOK_NEQ:      return BINOP_NEQ;
        case TOK_LT:        return BINOP_LT;   case TOK_GT:       return BINOP_GT;
        case TOK_LE:        return BINOP_LE;   case TOK_GE:       return BINOP_GE;
        case TOK_ASSIGN:    return BINOP_ASSIGN;
        case TOK_PLUS_EQ:   return BINOP_ADD_ASSIGN;
        case TOK_MINUS_EQ:  return BINOP_SUB_ASSIGN;
        case TOK_STAR_EQ:   return BINOP_MUL_ASSIGN;
        case TOK_SLASH_EQ:  return BINOP_DIV_ASSIGN;
        case TOK_QMARKQMARK: return BINOP_NULLCOAL;
        default:            return BINOP_ADD;
    }
}

/* Parse a block { stmts... tail_expr? } */
static Expr *parse_block(Parser *p);
static Stmt *parse_stmt(Parser *p);

static Expr *parse_if_expr(Parser *p) {
    SrcPos start = p->previous.span.start;
    Expr  *e     = NEW(p, Expr);
    e->kind = EXPR_IF;
    e->if_expr.cond = parse_expr_prec(p, PREC_ASSIGN + 1);
    e->if_expr.then_expr = parse_block(p);
    if (match_tok(p, TOK_ELSE)) {
        if (check(p, TOK_IF)) {
            advance_tok(p);
            e->if_expr.else_expr = parse_if_expr(p);
        } else {
            e->if_expr.else_expr = parse_block(p);
        }
    } else {
        e->if_expr.else_expr = NULL;
    }
    e->span = span_from(start, p->previous.span.end);
    return e;
}

static Expr *parse_match_expr(Parser *p) {
    SrcPos start = p->previous.span.start;
    Expr  *e     = NEW(p, Expr);
    e->kind = EXPR_MATCH;
    e->match.subject = parse_expr_prec(p, PREC_ASSIGN + 1);
    consume(p, TOK_LBRACE, "{");

    size_t     cap  = 8;
    MatchArm  *arms = NEW_ARR(p, MatchArm, cap);
    size_t     armc = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        GROW_ARR(p, arms, armc, cap, MatchArm);
        MatchArm *arm = &arms[armc++];
        arm->pattern  = parse_expr_prec(p, PREC_ASSIGN + 1);
        arm->guard    = NULL;
        if (check(p, TOK_IF)) {
            advance_tok(p);
            arm->guard = parse_expr_prec(p, PREC_ASSIGN + 1);
        }
        consume(p, TOK_FAT_ARROW, "=>");
        arm->body = check(p, TOK_LBRACE) ? parse_block(p) : parse_expr(p);
        match_tok(p, TOK_COMMA);
    }
    consume(p, TOK_RBRACE, "}");
    e->match.arms = arms;
    e->match.armc = armc;
    e->span = span_from(start, p->previous.span.end);
    return e;
}

static Expr *parse_block(Parser *p) {
    SrcPos start = p->current.span.start;
    consume(p, TOK_LBRACE, "{");
    Expr *e = NEW(p, Expr);
    e->kind = EXPR_BLOCK;

    size_t cap   = 16;
    Stmt **stmts = NEW_ARR(p, Stmt*, cap);
    size_t stmtc = 0;
    Expr  *tail  = NULL;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        /* If the last thing before } is an expression without ;, it's the tail */
        GROW_ARR(p, stmts, stmtc, cap, Stmt*);
        Stmt *s = parse_stmt(p);
        if (s == NULL) break;
        stmts[stmtc++] = s;
    }
    /* TODO: extract tail expression from last expr-stmt if no semicolon */
    (void)tail;

    consume(p, TOK_RBRACE, "}");
    e->block.stmts = stmts;
    e->block.stmtc = stmtc;
    e->block.tail  = tail;
    e->span = span_from(start, p->previous.span.end);
    return e;
}

static Expr *parse_primary(Parser *p) {
    SrcPos start = p->current.span.start;
    Expr  *e     = NEW(p, Expr);

    /* Literals */
    if (match_tok(p, TOK_INT_LIT)) {
        e->kind    = EXPR_INT_LIT;
        e->int_val = p->previous.int_val;
        e->span    = p->previous.span;
        return e;
    }
    if (match_tok(p, TOK_FLOAT_LIT)) {
        e->kind    = EXPR_FLOAT_LIT;
        e->flt_val = p->previous.flt_val;
        e->span    = p->previous.span;
        return e;
    }
    if (match_tok(p, TOK_BOOL_LIT)) {
        e->kind     = EXPR_BOOL_LIT;
        e->bool_val = (bool)p->previous.int_val;
        e->span     = p->previous.span;
        return e;
    }
    if (match_tok(p, TOK_STR_LIT)) {
        e->kind        = EXPR_STR_LIT;
        e->str_val.ptr = p->previous.str.ptr;
        e->str_val.len = p->previous.str.len;
        e->span        = p->previous.span;
        return e;
    }
    if (match_tok(p, TOK_NULL_LIT)) {
        e->kind = EXPR_NULL;
        e->span = p->previous.span;
        return e;
    }

    /* Builtin @name(...) */
    if (match_tok(p, TOK_AT)) {
        e->kind = EXPR_BUILTIN;
        Token name_tok = consume(p, TOK_IDENT, "builtin name");
        e->builtin.name = (Ident){ name_tok.str.ptr, name_tok.str.len };
        consume(p, TOK_LPAREN, "(");
        size_t cap = 4;
        Expr **args = NEW_ARR(p, Expr*, cap);
        size_t argc = 0;
        if (!check(p, TOK_RPAREN)) {
            do {
                GROW_ARR(p, args, argc, cap, Expr*);
                args[argc++] = parse_expr(p);
            } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
        }
        consume(p, TOK_RPAREN, ")");
        e->builtin.args = args;
        e->builtin.argc = argc;
        e->span = span_from(start, p->previous.span.end);
        return e;
    }

    /* Identifier */
    if (match_tok(p, TOK_IDENT)) {
        e->kind  = EXPR_IDENT;
        e->ident = (Ident){ p->previous.str.ptr, p->previous.str.len };
        e->span  = p->previous.span;
        return e;
    }

    /* Grouped expression or tuple */
    if (match_tok(p, TOK_LPAREN)) {
        Expr *inner = parse_expr(p);
        consume(p, TOK_RPAREN, ")");
        inner->span = span_from(start, p->previous.span.end);
        return inner;
    }

    /* Block */
    if (check(p, TOK_LBRACE)) return parse_block(p);

    /* if expression */
    if (match_tok(p, TOK_IF)) return parse_if_expr(p);

    /* match expression */
    if (match_tok(p, TOK_MATCH)) return parse_match_expr(p);

    /* loop */
    if (match_tok(p, TOK_LOOP)) {
        e->kind = EXPR_LOOP;
        Expr *body = parse_block(p);
        e->block = body->block;
        e->span = span_from(start, p->previous.span.end);
        return e;
    }

    /* Comptime */
    if (match_tok(p, TOK_COMPTIME)) {
        e->kind = EXPR_COMPTIME;
        e->comptime.inner = check(p, TOK_LBRACE) ? parse_block(p) : parse_expr(p);
        e->span = span_from(start, p->previous.span.end);
        return e;
    }

    /* try expr */
    if (match_tok(p, TOK_TRY)) {
        e->kind = EXPR_TRY;
        /* re-use unary.operand for try's inner */
        e->unary.operand = parse_expr_prec(p, PREC_UNARY);
        e->span = span_from(start, p->previous.span.end);
        return e;
    }

    /* await expr */
    if (match_tok(p, TOK_AWAIT)) {
        e->kind = EXPR_AWAIT;
        e->await_expr.inner = parse_expr_prec(p, PREC_UNARY);
        e->span = span_from(start, p->previous.span.end);
        return e;
    }

    /* Error fallthrough */
    if (!p->panic_mode) {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected expression, found %s",
                 token_kind_name(p->current.kind));
        diag_error(p->diags, p->lexer->filepath, p->lexer->src,
                   p->current.span, buf, NULL, "start an expression here");
        p->had_error = p->panic_mode = true;
    }
    e->kind = EXPR_INT_LIT; e->int_val = 0;
    return e;
}

static Expr *parse_unary(Parser *p) {
    SrcPos start = p->current.span.start;
    if (p->current.kind == TOK_MINUS || p->current.kind == TOK_BANG ||
        p->current.kind == TOK_TILDE || p->current.kind == TOK_STAR ||
        p->current.kind == TOK_AMP) {
        advance_tok(p);
        UnaryOp op;
        switch (p->previous.kind) {
            case TOK_MINUS: op = UNOP_NEG;    break;
            case TOK_BANG:  op = UNOP_NOT;    break;
            case TOK_TILDE: op = UNOP_BITNOT; break;
            case TOK_STAR:  op = UNOP_DEREF;  break;
            case TOK_AMP:   op = UNOP_ADDR;   break;
            default:        op = UNOP_NEG;    break;
        }
        Expr *e = NEW(p, Expr);
        e->kind           = EXPR_UNARY;
        e->unary.op       = op;
        e->unary.operand  = parse_unary(p);
        e->span           = span_from(start, p->previous.span.end);
        return e;
    }
    return parse_primary(p);
}

/* Parse postfix: call, index, field, range */
static Expr *parse_postfix(Parser *p, Expr *left) {
    for (;;) {
        SrcPos start = p->current.span.start;

        /* Function call: expr(args) */
        if (match_tok(p, TOK_LPAREN)) {
            Expr  *e    = NEW(p, Expr);
            e->kind     = EXPR_CALL;
            e->call.callee = left;
            size_t cap  = 4;
            Expr **args = NEW_ARR(p, Expr*, cap);
            size_t argc = 0;
            if (!check(p, TOK_RPAREN)) {
                do {
                    GROW_ARR(p, args, argc, cap, Expr*);
                    args[argc++] = parse_expr(p);
                } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
            }
            consume(p, TOK_RPAREN, ")");
            e->call.args = args;
            e->call.argc = argc;
            e->span = span_from(left->span.start, p->previous.span.end);
            left = e;
            continue;
        }

        /* Index: expr[idx] */
        if (match_tok(p, TOK_LBRACK)) {
            Expr *e = NEW(p, Expr);
            e->kind = EXPR_INDEX;
            e->index.obj = left;
            e->index.idx = parse_expr(p);
            consume(p, TOK_RBRACK, "]");
            e->span = span_from(left->span.start, p->previous.span.end);
            left = e;
            continue;
        }

        /* Field access: expr.field */
        if (match_tok(p, TOK_DOT)) {
            Token field_tok = consume(p, TOK_IDENT, "field name");
            Expr *e = NEW(p, Expr);
            e->kind       = EXPR_FIELD;
            e->field.obj  = left;
            e->field.field = (Ident){ field_tok.str.ptr, field_tok.str.len };
            e->span = span_from(left->span.start, p->previous.span.end);
            left = e;
            continue;
        }

        /* Range: expr..expr or expr..=expr */
        if (check(p, TOK_DOTDOT) || check(p, TOK_DOTDOTEQ)) {
            bool inclusive = (p->current.kind == TOK_DOTDOTEQ);
            advance_tok(p);
            Expr *e = NEW(p, Expr);
            e->kind           = EXPR_RANGE;
            e->range.start    = left;
            e->range.end      = parse_expr_prec(p, PREC_ADD);
            e->range.inclusive = inclusive;
            e->span = span_from(left->span.start, p->previous.span.end);
            left = e;
            break;
        }

        /* catch |e| { ... } */
        if (match_tok(p, TOK_CATCH)) {
            Expr *e = NEW(p, Expr);
            e->kind = EXPR_CATCH;
            e->catch_expr.expr = left;
            consume(p, TOK_PIPE, "|");
            Token bind = consume(p, TOK_IDENT, "error binding");
            e->catch_expr.err_bind = (Ident){ bind.str.ptr, bind.str.len };
            consume(p, TOK_PIPE, "|");
            e->catch_expr.handler = parse_block(p);
            e->span = span_from(left->span.start, p->previous.span.end);
            left = e;
            continue;
        }

        (void)start;
        break;
    }
    return left;
}

static Expr *parse_expr_prec(Parser *p, int min_prec) {
    Expr *left = parse_unary(p);
    left       = parse_postfix(p, left);

    while (true) {
        int prec = infix_prec(p->current.kind);
        if (prec < min_prec) break;
        if (p->current.kind == TOK_DOTDOT || p->current.kind == TOK_DOTDOTEQ) break;

        TokenKind op_tok = p->current.kind;
        advance_tok(p);
        int       next   = is_right_assoc(op_tok) ? prec : prec + 1;
        Expr     *right  = parse_expr_prec(p, next);
        right            = parse_postfix(p, right);

        Expr *e = NEW(p, Expr);
        e->kind       = EXPR_BINARY;
        e->binary.op  = tok_to_binop(op_tok);
        e->binary.lhs = left;
        e->binary.rhs = right;
        e->span       = span_from(left->span.start, right->span.end);
        left          = e;
    }
    return left;
}

static Expr *parse_expr(Parser *p) {
    return parse_expr_prec(p, PREC_ASSIGN);
}

/* ─── Statement parsing ─────────────────────────────────────────── */
static Stmt *parse_stmt(Parser *p) {
    SrcPos start = p->current.span.start;
    Stmt  *s     = NEW(p, Stmt);

    /* let / var */
    if (check(p, TOK_LET) || check(p, TOK_VAR)) {
        bool is_mut = (p->current.kind == TOK_VAR);
        advance_tok(p);
        s->kind = STMT_LET;
        Token name = consume(p, TOK_IDENT, "variable name");
        s->let.name   = (Ident){ name.str.ptr, name.str.len };
        s->let.is_mut = is_mut;
        s->let.ty     = NULL;
        if (match_tok(p, TOK_COLON)) s->let.ty = parse_type(p);
        s->let.init = NULL;
        if (match_tok(p, TOK_ASSIGN)) s->let.init = parse_expr(p);
        match_tok(p, TOK_SEMI);
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* return */
    if (match_tok(p, TOK_RETURN)) {
        s->kind = STMT_RETURN;
        s->ret.value = (!check(p, TOK_RBRACE) && !check(p, TOK_SEMI) && !check(p, TOK_EOF))
                       ? parse_expr(p) : NULL;
        match_tok(p, TOK_SEMI);
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* break */
    if (match_tok(p, TOK_BREAK)) {
        s->kind = STMT_BREAK;
        s->break_s.value = (!check(p, TOK_RBRACE) && !check(p, TOK_SEMI))
                           ? parse_expr(p) : NULL;
        match_tok(p, TOK_SEMI);
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* continue */
    if (match_tok(p, TOK_CONTINUE)) {
        s->kind = STMT_CONTINUE;
        match_tok(p, TOK_SEMI);
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* defer */
    if (match_tok(p, TOK_DEFER)) {
        s->kind = STMT_DEFER;
        s->defer_s.inner = (Expr*)parse_stmt(p);
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* unsafe { } */
    if (match_tok(p, TOK_UNSAFE)) {
        s->kind = STMT_UNSAFE;
        consume(p, TOK_LBRACE, "{");
        size_t cap = 8;
        Stmt **body = NEW_ARR(p, Stmt*, cap);
        size_t c    = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            GROW_ARR(p, body, c, cap, Stmt*);
            body[c++] = parse_stmt(p);
        }
        consume(p, TOK_RBRACE, "}");
        s->unsafe_s.body  = body;
        s->unsafe_s.count = c;
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* for bind in iter { } */
    if (match_tok(p, TOK_FOR)) {
        s->kind = STMT_FOR;
        /* support: for i in 0..10 or for item in list */
        Token bind_tok = consume(p, TOK_IDENT, "loop variable");
        s->for_s.bind = (Ident){ bind_tok.str.ptr, bind_tok.str.len };
        consume(p, TOK_IN, "in");
        s->for_s.iter = parse_expr(p);
        consume(p, TOK_LBRACE, "{");
        size_t cap = 16;
        Stmt **body = NEW_ARR(p, Stmt*, cap);
        size_t c    = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            GROW_ARR(p, body, c, cap, Stmt*);
            body[c++] = parse_stmt(p);
        }
        consume(p, TOK_RBRACE, "}");
        s->for_s.body = body;
        s->for_s.c    = c;
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* while cond { } */
    if (match_tok(p, TOK_WHILE)) {
        s->kind = STMT_WHILE;
        s->while_s.cond = parse_expr_prec(p, PREC_ASSIGN + 1);
        consume(p, TOK_LBRACE, "{");
        size_t cap = 16;
        Stmt **body = NEW_ARR(p, Stmt*, cap);
        size_t c    = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            GROW_ARR(p, body, c, cap, Stmt*);
            body[c++] = parse_stmt(p);
        }
        consume(p, TOK_RBRACE, "}");
        s->while_s.body = body;
        s->while_s.c    = c;
        s->span = span_from(start, p->previous.span.end);
        return s;
    }

    /* Expression statement */
    s->kind      = STMT_EXPR;
    s->expr_stmt = parse_expr(p);
    match_tok(p, TOK_SEMI);
    if (p->panic_mode) synchronise(p);
    s->span = span_from(start, p->previous.span.end);
    return s;
}

/* ─── Declaration parsing ───────────────────────────────────────── */

static Param parse_param(Parser *p) {
    Param pm = {0};
    if (match_tok(p, TOK_COMPTIME)) pm.is_comptime = true;
    Token name = consume(p, TOK_IDENT, "parameter name");
    pm.name = (Ident){ name.str.ptr, name.str.len };
    consume(p, TOK_COLON, ":");
    pm.ty = parse_type(p);
    return pm;
}

static Decl *parse_fn(Parser *p, bool is_pub) {
    SrcPos start = p->previous.span.start;
    Decl  *d     = NEW(p, Decl);
    d->kind   = DECL_FN;
    d->is_pub = is_pub;

    Token name = consume(p, TOK_IDENT, "function name");
    d->fn.name = (Ident){ name.str.ptr, name.str.len };

    consume(p, TOK_LPAREN, "(");
    size_t  cap    = 4;
    Param  *params = NEW_ARR(p, Param, cap);
    size_t  paramc = 0;

    if (!check(p, TOK_RPAREN)) {
        do {
            GROW_ARR(p, params, paramc, cap, Param);
            /* self parameter */
            if (check(p, TOK_IDENT) &&
                p->current.str.ptr && strncmp(p->current.str.ptr, "self", 4) == 0
                && p->current.str.len == 4) {
                advance_tok(p);
                params[paramc].name = (Ident){"self", 4};
                params[paramc].ty   = NULL; /* filled by sema */
                params[paramc].is_comptime = false;
                paramc++;
            } else {
                params[paramc++] = parse_param(p);
            }
        } while (match_tok(p, TOK_COMMA) && !check(p, TOK_RPAREN));
    }
    consume(p, TOK_RPAREN, ")");

    d->fn.params = params;
    d->fn.paramc = paramc;

    /* Return type */
    d->fn.ret_ty = NULL;
    if (match_tok(p, TOK_FAT_ARROW)) d->fn.ret_ty = parse_type(p);

    /* Body or extern (no body) */
    if (match_tok(p, TOK_LBRACE)) {
        /* parse body as stmts until } */
        size_t  bcap  = 16;
        Stmt  **body  = NEW_ARR(p, Stmt*, bcap);
        size_t  bodyc = 0;
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            GROW_ARR(p, body, bodyc, bcap, Stmt*);
            body[bodyc++] = parse_stmt(p);
        }
        consume(p, TOK_RBRACE, "}");
        d->fn.body  = body;
        d->fn.bodyc = bodyc;
    } else {
        match_tok(p, TOK_SEMI);
        d->fn.body  = NULL;
        d->fn.bodyc = 0;
    }

    d->span = span_from(start, p->previous.span.end);
    return d;
}

static Decl *parse_struct(Parser *p, bool is_pub) {
    SrcPos start = p->previous.span.start;
    Decl  *d     = NEW(p, Decl);
    d->kind   = DECL_STRUCT;
    d->is_pub = is_pub;

    Token name = consume(p, TOK_IDENT, "struct name");
    d->struc.name = (Ident){ name.str.ptr, name.str.len };
    consume(p, TOK_LBRACE, "{");

    size_t cap    = 8;
    Field *fields = NEW_ARR(p, Field, cap);
    size_t fieldc = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        GROW_ARR(p, fields, fieldc, cap, Field);
        bool field_pub = match_tok(p, TOK_PUB);
        Token fname = consume(p, TOK_IDENT, "field name");
        consume(p, TOK_COLON, ":");
        Type *ftype = parse_type(p);
        fields[fieldc++] = (Field){ {fname.str.ptr, fname.str.len}, ftype, NULL, field_pub };
        match_tok(p, TOK_COMMA);
    }
    consume(p, TOK_RBRACE, "}");

    d->struc.fields = fields;
    d->struc.fieldc = fieldc;
    d->span = span_from(start, p->previous.span.end);
    return d;
}

static Decl *parse_enum(Parser *p, bool is_pub) {
    SrcPos start = p->previous.span.start;
    Decl  *d     = NEW(p, Decl);
    d->kind   = DECL_ENUM;
    d->is_pub = is_pub;

    Token name = consume(p, TOK_IDENT, "enum name");
    d->enm.name = (Ident){ name.str.ptr, name.str.len };
    consume(p, TOK_LBRACE, "{");

    size_t       cap      = 8;
    EnumVariant *variants = NEW_ARR(p, EnumVariant, cap);
    size_t       variantc = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        GROW_ARR(p, variants, variantc, cap, EnumVariant);
        Token vname = consume(p, TOK_IDENT, "variant name");
        EnumVariant *v = &variants[variantc++];
        v->name   = (Ident){ vname.str.ptr, vname.str.len };
        v->fields = NULL;
        v->fieldc = 0;
        if (match_tok(p, TOK_LBRACE)) {
            size_t fcap    = 4;
            Field *vfields = NEW_ARR(p, Field, fcap);
            size_t vfieldc = 0;
            while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                GROW_ARR(p, vfields, vfieldc, fcap, Field);
                Token fn2 = consume(p, TOK_IDENT, "field name");
                consume(p, TOK_COLON, ":");
                Type *ft = parse_type(p);
                vfields[vfieldc++] = (Field){ {fn2.str.ptr, fn2.str.len}, ft, NULL, false };
                match_tok(p, TOK_COMMA);
            }
            consume(p, TOK_RBRACE, "}");
            v->fields = vfields;
            v->fieldc = vfieldc;
        }
        match_tok(p, TOK_COMMA);
    }
    consume(p, TOK_RBRACE, "}");

    d->enm.variants = variants;
    d->enm.variantc = variantc;
    d->span = span_from(start, p->previous.span.end);
    return d;
}

static Decl *parse_impl(Parser *p) {
    SrcPos start = p->previous.span.start;
    Decl  *d     = NEW(p, Decl);
    d->kind = DECL_IMPL;

    /* impl [Interface for] Type { methods } */
    Type *first = parse_type(p);
    if (check(p, TOK_FN) || check(p, TOK_LBRACE)) {
        d->impl.self_ty = first;
        d->impl.iface   = NULL;
    } else {
        /* impl Trait for Type */
        consume(p, TOK_IDENT, "for"); /* consume 'for' keyword (parsed as ident) */
        d->impl.iface   = first;
        d->impl.self_ty = parse_type(p);
    }
    consume(p, TOK_LBRACE, "{");

    size_t  mcap    = 8;
    Decl  **methods = NEW_ARR(p, Decl*, mcap);
    size_t  methodc = 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        GROW_ARR(p, methods, methodc, mcap, Decl*);
        bool mpub = match_tok(p, TOK_PUB);
        bool async_fn = match_tok(p, TOK_ASYNC);
        (void)async_fn;
        if (match_tok(p, TOK_FN)) {
            methods[methodc++] = parse_fn(p, mpub);
        } else {
            advance_tok(p); /* skip unknown, error recovery */
        }
    }
    consume(p, TOK_RBRACE, "}");

    d->impl.methods = methods;
    d->impl.methodc = methodc;
    d->span = span_from(start, p->previous.span.end);
    return d;
}

static Decl *parse_decl(Parser *p) {
    bool is_pub = match_tok(p, TOK_PUB);
    bool is_async = match_tok(p, TOK_ASYNC);
    (void)is_async;

    if (match_tok(p, TOK_FN))        return parse_fn(p, is_pub);
    if (match_tok(p, TOK_STRUCT))    return parse_struct(p, is_pub);
    if (match_tok(p, TOK_ENUM))      return parse_enum(p, is_pub);
    if (match_tok(p, TOK_IMPL))      return parse_impl(p);

    /* const */
    if (match_tok(p, TOK_CONST)) {
        Decl *d = NEW(p, Decl);
        d->kind   = DECL_CONST;
        d->is_pub = is_pub;
        Token cn = consume(p, TOK_IDENT, "const name");
        d->cnst.name = (Ident){ cn.str.ptr, cn.str.len };
        d->cnst.ty   = NULL;
        if (match_tok(p, TOK_COLON)) d->cnst.ty = parse_type(p);
        consume(p, TOK_ASSIGN, "=");
        d->cnst.value = parse_expr(p);
        match_tok(p, TOK_SEMI);
        return d;
    }

    /* Skip unknown at file level */
    if (!p->panic_mode) {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected declaration, found %s",
                 token_kind_name(p->current.kind));
        diag_error(p->diags, p->lexer->filepath, p->lexer->src,
                   p->current.span, buf, NULL, "expected fn, struct, enum, impl, or const");
        p->had_error = p->panic_mode = true;
    }
    advance_tok(p);
    synchronise(p);
    return NULL;
}

/* ─── Public API ────────────────────────────────────────────────── */

Parser parser_create(Lexer *l, Arena *arena, DiagSink *diags) {
    Parser p = { .lexer = l, .arena = arena, .diags = diags };
    advance_tok(&p); /* prime the lookahead */
    return p;
}

AstFile parser_parse_file(Parser *p, const char *filepath) {
    AstFile file = { .path = filepath };
    size_t  cap  = 32;
    file.decls   = NEW_ARR(p, Decl*, cap);
    file.declc   = 0;

    while (!check(p, TOK_EOF)) {
        GROW_ARR(p, file.decls, file.declc, cap, Decl*);
        Decl *d = parse_decl(p);
        if (d) file.decls[file.declc++] = d;
    }
    return file;
}

Expr *parser_parse_expr(Parser *p) { return parse_expr(p); }
Stmt *parser_parse_stmt(Parser *p) { return parse_stmt(p); }
Type *parser_parse_type(Parser *p) { return parse_type(p); }
