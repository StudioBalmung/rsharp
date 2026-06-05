/* rsharp/compiler/lexer/lexer.c  —  R# 1.0 Lexer  (C11) */
#include "lexer.h"
#include "../../runtime/memory/arena.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { const char *kw; TokenKind kind; } KwEntry;
static const KwEntry KEYWORDS[] = {
    {"fn",TOK_FN},{"let",TOK_LET},{"var",TOK_VAR},{"const",TOK_CONST},
    {"struct",TOK_STRUCT},{"enum",TOK_ENUM},{"impl",TOK_IMPL},{"interface",TOK_INTERFACE},
    {"if",TOK_IF},{"else",TOK_ELSE},{"match",TOK_MATCH},{"for",TOK_FOR},
    {"while",TOK_WHILE},{"loop",TOK_LOOP},{"return",TOK_RETURN},
    {"break",TOK_BREAK},{"continue",TOK_CONTINUE},{"defer",TOK_DEFER},
    {"unsafe",TOK_UNSAFE},{"async",TOK_ASYNC},{"await",TOK_AWAIT},{"spawn",TOK_SPAWN},
    {"pub",TOK_PUB},{"extern",TOK_EXTERN},{"export",TOK_EXPORT},
    {"use",TOK_USE},{"mod",TOK_MOD},{"as",TOK_AS},
    {"comptime",TOK_COMPTIME},{"try",TOK_TRY},{"catch",TOK_CATCH},{"in",TOK_IN},
    {"self",TOK_SELF},
    {"true",TOK_BOOL_LIT},{"false",TOK_BOOL_LIT},{"null",TOK_NULL_LIT},
    {"i8",TOK_KW_I8},{"i16",TOK_KW_I16},{"i32",TOK_KW_I32},{"i64",TOK_KW_I64},
    {"i128",TOK_KW_I128},{"isize",TOK_KW_ISIZE},
    {"u8",TOK_KW_U8},{"u16",TOK_KW_U16},{"u32",TOK_KW_U32},{"u64",TOK_KW_U64},
    {"u128",TOK_KW_U128},{"usize",TOK_KW_USIZE},
    {"f32",TOK_KW_F32},{"f64",TOK_KW_F64},
    {"bool",TOK_KW_BOOL},{"char",TOK_KW_CHAR},{"void",TOK_KW_VOID},{"str",TOK_KW_STR},
};
#define KW_COUNT (sizeof(KEYWORDS)/sizeof(KEYWORDS[0]))

static inline char  peek_c(const Lexer *l)  { return l->src[l->pos]; }
static inline char  peek2_c(const Lexer *l) { return l->pos+1<l->src_len?l->src[l->pos+1]:0; }
static inline char  peek3_c(const Lexer *l) { return l->pos+2<l->src_len?l->src[l->pos+2]:0; }
static inline bool  at_end(const Lexer *l)  { return l->pos >= l->src_len; }

static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; } else { l->col++; }
    return c;
}
static bool match_c(Lexer *l, char e) {
    if (at_end(l) || l->src[l->pos]!=e) return false;
    advance(l); return true;
}
static SrcPos cur_pos(const Lexer *l) { return (SrcPos){l->line,l->col,(uint32_t)l->pos}; }
static Token make_tok(TokenKind k, SrcPos s, SrcPos e) {
    Token t={0}; t.kind=k; t.span.start=s; t.span.end=e; return t;
}
static Token error_tok(Lexer *l, SrcPos s, const char *msg) {
    Token t = make_tok(TOK_ERROR,s,cur_pos(l));
    size_t n=strlen(msg); char *b=arena_alloc(l->arena,n+1,1);
    memcpy(b,msg,n+1); t.str.ptr=b; t.str.len=n; return t;
}

static void skip_trivia(Lexer *l) {
    while (!at_end(l)) {
        char c = peek_c(l);
        if (c==' '||c=='\t'||c=='\r'||c=='\n') { advance(l); continue; }
        /* // line comment */
        if (c=='/'&&peek2_c(l)=='/') {
            while (!at_end(l)&&peek_c(l)!='\n') advance(l);
            continue;
        }
        /* -- line comment (R# style) */
        if (c=='-'&&peek2_c(l)=='-') {
            while (!at_end(l)&&peek_c(l)!='\n') advance(l);
            continue;
        }
        /* block comment */
        if (c=='/'&&peek2_c(l)=='*') {
            advance(l); advance(l);
            while (!at_end(l)) {
                if (peek_c(l)=='*'&&peek2_c(l)=='/') { advance(l);advance(l); break; }
                advance(l);
            }
            continue;
        }
        break;
    }
}

static Token lex_number(Lexer *l, SrcPos start) {
    /* hex */
    if (peek_c(l)=='0'&&(peek2_c(l)=='x'||peek2_c(l)=='X')) {
        advance(l);advance(l);
        uint64_t v=0;
        while (!at_end(l)&&(isxdigit(peek_c(l))||peek_c(l)=='_')) {
            if (peek_c(l)!='_') v=v*16+(isdigit(peek_c(l))?peek_c(l)-'0':tolower(peek_c(l))-'a'+10);
            advance(l);
        }
        Token t=make_tok(TOK_INT_LIT,start,cur_pos(l)); t.int_val=(int64_t)v; return t;
    }
    /* binary */
    if (peek_c(l)=='0'&&(peek2_c(l)=='b'||peek2_c(l)=='B')) {
        advance(l);advance(l);
        uint64_t v=0;
        while (!at_end(l)&&(peek_c(l)=='0'||peek_c(l)=='1'||peek_c(l)=='_')) {
            if (peek_c(l)!='_') v=v*2+(peek_c(l)-'0');
            advance(l);
        }
        Token t=make_tok(TOK_INT_LIT,start,cur_pos(l)); t.int_val=(int64_t)v; return t;
    }
    /* octal */
    if (peek_c(l)=='0'&&(peek2_c(l)=='o'||peek2_c(l)=='O')) {
        advance(l);advance(l);
        uint64_t v=0;
        while (!at_end(l)&&((peek_c(l)>='0'&&peek_c(l)<='7')||peek_c(l)=='_')) {
            if (peek_c(l)!='_') v=v*8+(peek_c(l)-'0');
            advance(l);
        }
        Token t=make_tok(TOK_INT_LIT,start,cur_pos(l)); t.int_val=(int64_t)v; return t;
    }
    bool is_float=false;
    while (!at_end(l)&&(isdigit(peek_c(l))||peek_c(l)=='_')) advance(l);
    if (!at_end(l)&&peek_c(l)=='.'&&isdigit(peek2_c(l))) {
        is_float=true; advance(l);
        while (!at_end(l)&&(isdigit(peek_c(l))||peek_c(l)=='_')) advance(l);
    }
    if (!at_end(l)&&(peek_c(l)=='e'||peek_c(l)=='E')) {
        is_float=true; advance(l);
        if (!at_end(l)&&(peek_c(l)=='+'||peek_c(l)=='-')) advance(l);
        while (!at_end(l)&&isdigit(peek_c(l))) advance(l);
    }
    size_t raw=l->pos-start.offset;
    char *buf=arena_alloc(l->arena,raw+1,1);
    size_t bi=0;
    for (size_t i=0;i<raw;i++) if (l->src[start.offset+i]!='_') buf[bi++]=l->src[start.offset+i];
    buf[bi]='\0';
    if (is_float) {
        Token t=make_tok(TOK_FLOAT_LIT,start,cur_pos(l)); t.flt_val=strtod(buf,NULL); return t;
    }
    Token t=make_tok(TOK_INT_LIT,start,cur_pos(l)); t.int_val=(int64_t)strtoll(buf,NULL,10); return t;
}

static Token lex_string(Lexer *l, SrcPos start) {
    char *buf=arena_alloc(l->arena,4096,1); size_t bi=0;
    while (!at_end(l)) {
        char c=advance(l);
        if (c=='"') break;
        if (c!='\\') { buf[bi++]=c; continue; }
        char esc=advance(l);
        switch(esc) {
            case 'n':  buf[bi++]='\n'; break; case 't': buf[bi++]='\t'; break;
            case 'r':  buf[bi++]='\r'; break; case '\\':buf[bi++]='\\'; break;
            case '"':  buf[bi++]='"';  break; case '0': buf[bi++]='\0'; break;
            case '{':  buf[bi++]='{';  break;
            default: return error_tok(l,start,"unknown escape sequence in string");
        }
    }
    buf[bi]='\0';
    Token t=make_tok(TOK_STR_LIT,start,cur_pos(l)); t.str.ptr=buf; t.str.len=bi; return t;
}

static Token lex_raw_string(Lexer *l, SrcPos start) {
    /* r"..." or r#"..."# */
    int hashes=0;
    while (!at_end(l)&&peek_c(l)=='#') { hashes++; advance(l); }
    if (!match_c(l,'"')) return error_tok(l,start,"expected '\"' after r#");
    char *buf=arena_alloc(l->arena,8192,1); size_t bi=0;
    while (!at_end(l)) {
        char c=advance(l);
        if (c=='"') {
            int closing=0;
            while (!at_end(l)&&peek_c(l)=='#'&&closing<hashes) { advance(l); closing++; }
            if (closing==hashes) break;
            buf[bi++]='"';
            for (int i=0;i<closing;i++) buf[bi++]='#';
        } else { buf[bi++]=c; }
    }
    buf[bi]='\0';
    Token t=make_tok(TOK_STR_LIT,start,cur_pos(l)); t.str.ptr=buf; t.str.len=bi; return t;
}

static TokenKind ident_or_kw(const char *s, size_t len) {
    for (size_t i=0;i<KW_COUNT;i++)
        if (strlen(KEYWORDS[i].kw)==len&&memcmp(KEYWORDS[i].kw,s,len)==0)
            return KEYWORDS[i].kind;
    return TOK_IDENT;
}

Lexer lexer_create(const char *src,size_t len,const char *fp,void *arena) {
    return (Lexer){.src=src,.src_len=len,.pos=0,.line=1,.col=1,.filepath=fp,.arena=arena};
}

Token lexer_next(Lexer *l) {
    skip_trivia(l);
    if (at_end(l)) return make_tok(TOK_EOF,cur_pos(l),cur_pos(l));
    SrcPos start=cur_pos(l);
    char   c=advance(l);

    /* raw string r"..." */
    if (c=='r'&&(peek_c(l)=='"'||peek_c(l)=='#')) return lex_raw_string(l,start);

    if (isalpha(c)||c=='_') {
        while (!at_end(l)&&(isalnum(peek_c(l))||peek_c(l)=='_')) advance(l);
        size_t len=l->pos-start.offset;
        const char *s=l->src+start.offset;
        TokenKind kind=ident_or_kw(s,len);
        Token t=make_tok(kind,start,cur_pos(l));
        if (kind==TOK_IDENT) {
            char *in=arena_alloc(l->arena,len+1,1);
            memcpy(in,s,len); in[len]='\0';
            t.str.ptr=in; t.str.len=len;
        } else if (kind==TOK_BOOL_LIT) { t.int_val=(s[0]=='t')?1:0; }
        return t;
    }
    if (isdigit(c)) { l->pos--; l->col--; return lex_number(l,start); }
    if (c=='"') return lex_string(l,start);
    if (c=='\'') {
        /* char literal */
        Token t=make_tok(TOK_CHAR_LIT,start,cur_pos(l));
        char ch=advance(l);
        if (ch=='\\') { char e=advance(l); switch(e){ case 'n':ch='\n';break; case 't':ch='\t';break; default:ch=e; } }
        t.char_val=(uint32_t)ch;
        match_c(l,'\'');
        t.span.end=cur_pos(l); return t;
    }

    switch(c) {
        case '+': return make_tok(match_c(l,'=')?TOK_PLUS_EQ:TOK_PLUS,start,cur_pos(l));
        case '-':
            if (match_c(l,'>')) return make_tok(TOK_ARROW,start,cur_pos(l));
            return make_tok(match_c(l,'=')?TOK_MINUS_EQ:TOK_MINUS,start,cur_pos(l));
        case '*': return make_tok(match_c(l,'=')?TOK_STAR_EQ:TOK_STAR,start,cur_pos(l));
        case '/': return make_tok(match_c(l,'=')?TOK_SLASH_EQ:TOK_SLASH,start,cur_pos(l));
        case '%': return make_tok(match_c(l,'=')?TOK_PERCENT_EQ:TOK_PERCENT,start,cur_pos(l));
        case '&':
            if (match_c(l,'&')) return make_tok(TOK_LOGAND,start,cur_pos(l));
            return make_tok(match_c(l,'=')?TOK_AMP_EQ:TOK_AMP,start,cur_pos(l));
        case '|':
            if (match_c(l,'|')) return make_tok(TOK_LOGOR,start,cur_pos(l));
            return make_tok(match_c(l,'=')?TOK_PIPE_EQ:TOK_PIPE,start,cur_pos(l));
        case '^': return make_tok(match_c(l,'=')?TOK_CARET_EQ:TOK_CARET,start,cur_pos(l));
        case '~': return make_tok(TOK_TILDE,start,cur_pos(l));
        case '!': return make_tok(match_c(l,'=')?TOK_NEQ:TOK_BANG,start,cur_pos(l));
        case '<':
            if (match_c(l,'<')) return make_tok(TOK_SHL,start,cur_pos(l));
            return make_tok(match_c(l,'=')?TOK_LE:TOK_LT,start,cur_pos(l));
        case '>':
            if (match_c(l,'>')) return make_tok(TOK_SHR,start,cur_pos(l));
            return make_tok(match_c(l,'=')?TOK_GE:TOK_GT,start,cur_pos(l));
        case '=':
            if (match_c(l,'>')) return make_tok(TOK_FAT_ARROW,start,cur_pos(l));
            return make_tok(match_c(l,'=')?TOK_EQ:TOK_ASSIGN,start,cur_pos(l));
        case ':': return make_tok(match_c(l,':')?TOK_COLONCOLON:TOK_COLON,start,cur_pos(l));
        case '.':
            if (peek_c(l)=='.'&&peek2_c(l)=='.') { advance(l);advance(l); return make_tok(TOK_DOTDOTDOT,start,cur_pos(l)); }
            if (match_c(l,'.')) return make_tok(match_c(l,'=')?TOK_DOTDOTEQ:TOK_DOTDOT,start,cur_pos(l));
            return make_tok(TOK_DOT,start,cur_pos(l));
        case ',': return make_tok(TOK_COMMA,start,cur_pos(l));
        case ';': return make_tok(TOK_SEMI,start,cur_pos(l));
        case '?': return make_tok(match_c(l,'?')?TOK_QMARKQMARK:TOK_QMARK,start,cur_pos(l));
        case '@': return make_tok(TOK_AT,start,cur_pos(l));
        case '#': return make_tok(TOK_HASH,start,cur_pos(l));
        case '(': return make_tok(TOK_LPAREN,start,cur_pos(l));
        case ')': return make_tok(TOK_RPAREN,start,cur_pos(l));
        case '{': return make_tok(TOK_LBRACE,start,cur_pos(l));
        case '}': return make_tok(TOK_RBRACE,start,cur_pos(l));
        case '[': return make_tok(TOK_LBRACK,start,cur_pos(l));
        case ']': return make_tok(TOK_RBRACK,start,cur_pos(l));
        default: {
            char msg[64];
            snprintf(msg,sizeof(msg),"unexpected character '%c' (0x%02X)",c,(unsigned char)c);
            return error_tok(l,start,msg);
        }
    }
}

Token lexer_peek(Lexer *l) { Lexer s=*l; Token t=lexer_next(l); *l=s; return t; }

const char *token_kind_name(TokenKind k) {
    switch(k) {
        case TOK_INT_LIT:   return "integer literal";
        case TOK_FLOAT_LIT: return "float literal";
        case TOK_STR_LIT:   return "string literal";
        case TOK_BOOL_LIT:  return "bool literal";
        case TOK_IDENT:     return "identifier";
        case TOK_FN:        return "fn";      case TOK_LET:   return "let";
        case TOK_VAR:       return "var";     case TOK_STRUCT:return "struct";
        case TOK_ENUM:      return "enum";    case TOK_IF:    return "if";
        case TOK_ELSE:      return "else";    case TOK_FOR:   return "for";
        case TOK_WHILE:     return "while";   case TOK_LOOP:  return "loop";
        case TOK_RETURN:    return "return";  case TOK_MATCH: return "match";
        case TOK_DEFER:     return "defer";   case TOK_UNSAFE:return "unsafe";
        case TOK_USE:       return "use";     case TOK_MOD:   return "mod";
        case TOK_IN:        return "in";      case TOK_SELF:  return "self";
        case TOK_ARROW:     return "=>";
        case TOK_FAT_ARROW: return "=>";
        case TOK_ASSIGN:    return "=";
        case TOK_EOF:       return "<eof>";
        case TOK_ERROR:     return "<error>";
        default:            return "<token>";
    }
}
bool token_is_assign_op(TokenKind k) {
    return k==TOK_ASSIGN||k==TOK_PLUS_EQ||k==TOK_MINUS_EQ||
           k==TOK_STAR_EQ||k==TOK_SLASH_EQ||k==TOK_PERCENT_EQ||
           k==TOK_AMP_EQ||k==TOK_PIPE_EQ||k==TOK_CARET_EQ;
}
