/* rsharp/compiler/error/diagnostic.h + diagnostic.c
 * R# Diagnostic System
 *
 * Design goals:
 *  - Rust-quality errors: show the source span, suggest a fix
 *  - Errors are values, not longjmp/exceptions
 *  - All messages go through a single sink (suppresses after N errors)
 *  - Colors via ANSI (auto-disabled when not a TTY)
 */

#pragma once
#include "../lexer/lexer.h"
#include <stdbool.h>

#define MAX_ERRORS 20   /* stop after this many errors */

typedef enum DiagLevel {
    DIAG_NOTE,
    DIAG_WARN,
    DIAG_ERROR,
    DIAG_FATAL,   /* always printed, triggers immediate abort */
} DiagLevel;

typedef struct DiagLabel {
    SrcSpan     span;
    const char *message;  /* short label under the caret */
    bool        is_primary;
} DiagLabel;

typedef struct Diagnostic {
    DiagLevel    level;
    const char  *filepath;
    const char  *source;      /* full source text, for context printing */
    SrcSpan      span;
    const char  *message;     /* main message                           */
    const char  *note;        /* optional "note: ..." line              */
    const char  *suggestion;  /* optional "help: ..." with fix hint     */
    DiagLabel   *labels;
    size_t       label_count;
} Diagnostic;

/* Sink collects diagnostics; caller decides when to flush/print */
typedef struct DiagSink {
    Diagnostic  *diags;
    size_t       count;
    size_t       capacity;
    size_t       error_count;
    size_t       warn_count;
    bool         use_color;
    bool         suppressing;  /* true after MAX_ERRORS */
} DiagSink;

DiagSink   diag_sink_create(bool use_color);
void       diag_sink_destroy(DiagSink *s);
void       diag_emit(DiagSink *s, Diagnostic d);
void       diag_print_all(const DiagSink *s);
bool       diag_had_errors(const DiagSink *s);

/* Convenience builders */
void diag_error(DiagSink *s, const char *filepath, const char *src,
                SrcSpan span, const char *msg, const char *note, const char *help);

void diag_warn(DiagSink *s, const char *filepath, const char *src,
               SrcSpan span, const char *msg);
