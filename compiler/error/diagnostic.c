/* rsharp/compiler/error/diagnostic.c
 * Beautiful error rendering — Rust-inspired source spans
 */

#include "diagnostic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ─── ANSI colors ───────────────────────────────────────────────── */
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_RED     "\033[31m"
#define C_YELLOW  "\033[33m"
#define C_CYAN    "\033[36m"
#define C_BLUE    "\033[34m"
#define C_GRAY    "\033[90m"
#define C_GREEN   "\033[32m"

static const char *level_color(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR: return C_RED;
        case DIAG_WARN:  return C_YELLOW;
        case DIAG_NOTE:  return C_CYAN;
        case DIAG_FATAL: return C_RED;
    }
    return C_RESET;
}

static const char *level_name(DiagLevel l) {
    switch (l) {
        case DIAG_ERROR: return "error";
        case DIAG_WARN:  return "warning";
        case DIAG_NOTE:  return "note";
        case DIAG_FATAL: return "fatal";
    }
    return "?";
}

/* ─── Extract a line from source by 1-based line number ─────────── */
static bool get_line(const char *src, uint32_t lineno,
                     const char **out, size_t *out_len) {
    uint32_t cur = 1;
    const char *p = src;
    while (*p) {
        if (cur == lineno) {
            const char *start = p;
            while (*p && *p != '\n') p++;
            *out     = start;
            *out_len = (size_t)(p - start);
            return true;
        }
        if (*p == '\n') cur++;
        p++;
    }
    return false;
}

/* ─── Render one diagnostic to stderr ──────────────────────────── */
static void render(const Diagnostic *d, bool color) {
#define CLR(c)  (color ? (c) : "")
#define NOCLR   CLR(C_RESET)

    /* Header: error[E0001]: message */
    fprintf(stderr, "%s%s%s%s: %s%s%s\n",
            CLR(C_BOLD), CLR(level_color(d->level)),
            level_name(d->level), CLR(C_RESET),
            CLR(C_BOLD), d->message, NOCLR);

    /* Location: --> file.rss:line:col */
    if (d->filepath) {
        fprintf(stderr, "%s  -=> %s:%u:%u%s\n",
                CLR(C_BLUE), d->filepath,
                d->span.start.line, d->span.start.col, NOCLR);
    }

    /* Source snippet */
    if (d->source && d->span.start.line > 0) {
        const char *line_text;
        size_t      line_len;

        /* Line number gutter width */
        char line_num[16];
        snprintf(line_num, sizeof(line_num), "%u", d->span.start.line);
        int gutter = (int)strlen(line_num) + 1;

        /* Empty gutter line */
        fprintf(stderr, "%s%*s |%s\n", CLR(C_BLUE), gutter, "", NOCLR);

        if (get_line(d->source, d->span.start.line, &line_text, &line_len)) {
            fprintf(stderr, "%s%*u |%s %.*s\n",
                    CLR(C_BLUE), gutter, d->span.start.line, NOCLR,
                    (int)line_len, line_text);

            /* Caret line */
            uint32_t col   = d->span.start.col > 0 ? d->span.start.col - 1 : 0;
            uint32_t endcol = d->span.end.line == d->span.start.line
                              ? d->span.end.col
                              : (uint32_t)line_len + 1;
            uint32_t caret_len = endcol > col ? endcol - col : 1;

            fprintf(stderr, "%s%*s |%s %*s",
                    CLR(C_BLUE), gutter, "", NOCLR, (int)col, "");
            fprintf(stderr, "%s%s", CLR(level_color(d->level)),
                    d->level == DIAG_WARN ? CLR(C_YELLOW) : CLR(C_RED));
            for (uint32_t i = 0; i < caret_len; i++) fputc('^', stderr);
            fprintf(stderr, "%s\n", NOCLR);
        }

        /* Empty gutter line */
        fprintf(stderr, "%s%*s |%s\n", CLR(C_BLUE), gutter, "", NOCLR);
    }

    /* Note */
    if (d->note) {
        fprintf(stderr, "%s  = note:%s %s\n", CLR(C_BOLD), NOCLR, d->note);
    }

    /* Help / suggestion */
    if (d->suggestion) {
        fprintf(stderr, "%s  = help:%s %s\n", CLR(C_GREEN), NOCLR, d->suggestion);
    }

    fputc('\n', stderr);
#undef CLR
#undef NOCLR
}

/* ─── Public API ────────────────────────────────────────────────── */

DiagSink diag_sink_create(bool use_color) {
    DiagSink s = {0};
    s.use_color = use_color;
    s.capacity  = 32;
    s.diags     = malloc(sizeof(Diagnostic) * s.capacity);
    return s;
}

void diag_sink_destroy(DiagSink *s) {
    free(s->diags);
    s->diags = NULL;
}

void diag_emit(DiagSink *s, Diagnostic d) {
    if (s->suppressing) return;

    if (d.level == DIAG_ERROR || d.level == DIAG_FATAL) s->error_count++;
    if (d.level == DIAG_WARN)                           s->warn_count++;

    if (s->count == s->capacity) {
        s->capacity *= 2;
        s->diags = realloc(s->diags, sizeof(Diagnostic) * s->capacity);
    }
    s->diags[s->count++] = d;

    if (d.level == DIAG_FATAL) {
        render(&d, s->use_color);
        exit(1);
    }

    if (s->error_count >= MAX_ERRORS) {
        /* Print what we have, then a truncation notice */
        diag_print_all(s);
        fprintf(stderr, "aborting due to %zu previous errors\n", s->error_count);
        exit(1);
    }
}

void diag_print_all(const DiagSink *s) {
    for (size_t i = 0; i < s->count; i++)
        render(&s->diags[i], s->use_color);

    if (s->error_count > 0)
        fprintf(stderr, "found %zu error(s)%s\n",
                s->error_count,
                s->warn_count > 0 ? ", see warnings above" : "");
}

bool diag_had_errors(const DiagSink *s) {
    return s->error_count > 0;
}

void diag_error(DiagSink *s, const char *fp, const char *src,
                SrcSpan span, const char *msg, const char *note, const char *help) {
    diag_emit(s, (Diagnostic){
        .level      = DIAG_ERROR,
        .filepath   = fp,
        .source     = src,
        .span       = span,
        .message    = msg,
        .note       = note,
        .suggestion = help,
    });
}

void diag_warn(DiagSink *s, const char *fp, const char *src,
               SrcSpan span, const char *msg) {
    diag_emit(s, (Diagnostic){
        .level    = DIAG_WARN,
        .filepath = fp,
        .source   = src,
        .span     = span,
        .message  = msg,
    });
}
