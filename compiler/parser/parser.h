/* rsharp/compiler/parser/parser.h
 * R# Recursive-Descent Parser
 * Language: C11
 *
 * Grammar notes:
 *  - Pratt-style expression parsing (precedence climbing)
 *  - Error recovery via synchronisation at statement boundaries
 *  - All nodes allocated into a caller-supplied Arena
 */

#pragma once
#include "ast.h"
#include "../lexer/lexer.h"
#include "../error/diagnostic.h"
#include "../../runtime/memory/arena.h"

typedef struct Parser {
    Lexer    *lexer;
    Token     current;   /* lookahead token          */
    Token     previous;  /* last consumed token      */
    Arena    *arena;
    DiagSink *diags;
    bool      had_error;
    bool      panic_mode; /* suppress cascade errors */
} Parser;

/* ── API ──────────────────────────────────────────────────────────── */
Parser  parser_create(Lexer *l, Arena *arena, DiagSink *diags);
AstFile parser_parse_file(Parser *p, const char *filepath);

/* Exposed for testing */
Expr   *parser_parse_expr(Parser *p);
Stmt   *parser_parse_stmt(Parser *p);
Type   *parser_parse_type(Parser *p);
