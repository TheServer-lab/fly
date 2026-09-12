#include "flyrt.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// FlyText (docs/architecture.md §2.2): heap-allocated, refcounted, and
// (per the spec) immutable -- every op here that "changes" text allocates a
// new FlyText rather than mutating one in place. No interning yet (§2.2
// mentions interning literals as a future optimization); every text value,
// including literals, gets a fresh heap allocation for this milestone.

static FlyValue makeText(const char* data, size_t len) {
    FlyText* t = (FlyText*)malloc(sizeof(FlyText));
    if (!t) fly_rt_throw("out of memory allocating tex");
    t->hdr.refcount = 1;
    t->len = len;
    t->data = (char*)malloc(len + 1);
    if (!t->data) fly_rt_throw("out of memory allocating tex data");
    if (len) memcpy(t->data, data, len);
    t->data[len] = '\0';
    FlyValue v; v.tag = FLY_TEX; v.payload = (int64_t)(intptr_t)t;
    return v;
}

FlyValue fly_rt_text_from_cstr(const char* s) { return makeText(s, strlen(s)); }
FlyValue fly_rt_text_from_bytes(const char* s, size_t n) { return makeText(s, n); }

static FlyText* asText(FlyValue v, const char* op) {
    (void)op;
    if (v.tag != FLY_TEX) fly_rt_throw("type error: expected tex operand");
    return (FlyText*)(intptr_t)v.payload;
}

size_t fly_rt_text_len(FlyValue v) { return asText(v, "count")->len; }
const char* fly_rt_text_data(FlyValue v) { return asText(v, "data")->data; }

// spec §10.3 conversion table -- this is the shared textual representation
// used by both show() (runtime/show.c) and string interpolation
// (runtime/strbuild.c). coll/board get a "standard representation": Fly
// source-literal-like syntax, with nested tex quoted so structure is
// unambiguous (top-level show()/interpolation of a bare tex is unquoted,
// matching §10.3's "tex -> Text itself").
typedef struct { char* buf; size_t len, cap; } DynBuf;
static void dbInit(DynBuf* d) { d->cap = 64; d->len = 0; d->buf = (char*)malloc(d->cap); if (!d->buf) fly_rt_throw("out of memory"); d->buf[0] = '\0'; }
static void dbAppend(DynBuf* d, const char* s, size_t n) {
    if (d->len + n + 1 > d->cap) {
        while (d->len + n + 1 > d->cap) d->cap *= 2;
        d->buf = (char*)realloc(d->buf, d->cap);
        if (!d->buf) fly_rt_throw("out of memory");
    }
    memcpy(d->buf + d->len, s, n);
    d->len += n;
    d->buf[d->len] = '\0';
}
static void dbAppendCstr(DynBuf* d, const char* s) { dbAppend(d, s, strlen(s)); }

static void appendDisplay(DynBuf* d, FlyValue v, int quoteTex);

static void appendScalarOrQuoted(DynBuf* d, FlyValue v) {
    // Used for elements NESTED inside a coll/board display, where tex
    // elements are quoted so e.g. [1, "a"] doesn't render as [1, a].
    appendDisplay(d, v, /*quoteTex=*/1);
}

static void appendDisplay(DynBuf* d, FlyValue v, int quoteTex) {
    char numbuf[64];
    switch (v.tag) {
        case FLY_NUM: snprintf(numbuf, sizeof(numbuf), "%lld", (long long)v.payload); dbAppendCstr(d, numbuf); return;
        case FLY_DEC: snprintf(numbuf, sizeof(numbuf), "%g", fly_rt_as_dec(v)); dbAppendCstr(d, numbuf); return;
        case FLY_YN:  dbAppendCstr(d, v.payload ? "Yes" : "No"); return;
        case FLY_EMP: dbAppendCstr(d, "EMP"); return;
        case FLY_TEX: {
            FlyText* t = (FlyText*)(intptr_t)v.payload;
            if (quoteTex) { dbAppend(d, "\"", 1); dbAppend(d, t->data, t->len); dbAppend(d, "\"", 1); }
            else            dbAppend(d, t->data, t->len);
            return;
        }
        case FLY_COLL: {
            FlyColl* c = (FlyColl*)(intptr_t)v.payload;
            dbAppend(d, "[", 1);
            for (size_t i = 0; i < c->len; i++) {
                if (i) dbAppendCstr(d, ", ");
                appendScalarOrQuoted(d, c->items[i]);
            }
            dbAppend(d, "]", 1);
            return;
        }
        case FLY_BOARD: {
            FlyBoard* b = (FlyBoard*)(intptr_t)v.payload;
            dbAppend(d, "{", 1);
            for (size_t i = 0; i < b->len; i++) {
                if (i) dbAppendCstr(d, ", ");
                appendScalarOrQuoted(d, b->keys[i]);
                dbAppendCstr(d, ": ");
                appendScalarOrQuoted(d, b->vals[i]);
            }
            dbAppend(d, "}", 1);
            return;
        }
        default:
            dbAppendCstr(d, "<unknown fly value>");
            return;
    }
}

FlyValue fly_rt_to_text(FlyValue v) {
    if (v.tag == FLY_TEX) return fly_rt_retain(v); // already tex -- no-op, matches "tex itself"
    DynBuf d; dbInit(&d);
    appendDisplay(&d, v, /*quoteTex=*/0); // top-level tex/interpolation: unquoted
    FlyValue out = makeText(d.buf, d.len);
    free(d.buf);
    return out;
}

FlyValue fly_rt_cut(FlyValue text) {
    FlyText* t = asText(text, "cut");
    size_t start = 0, end = t->len;
    while (start < end && isspace((unsigned char)t->data[start])) start++;
    while (end > start && isspace((unsigned char)t->data[end - 1])) end--;
    return makeText(t->data + start, end - start);
}
FlyValue fly_rt_raise(FlyValue text) {
    FlyText* t = asText(text, "raise");
    FlyValue out = makeText(t->data, t->len);
    FlyText* o = (FlyText*)(intptr_t)out.payload;
    for (size_t i = 0; i < o->len; i++) o->data[i] = (char)toupper((unsigned char)o->data[i]);
    return out;
}
FlyValue fly_rt_lower(FlyValue text) {
    FlyText* t = asText(text, "lower");
    FlyValue out = makeText(t->data, t->len);
    FlyText* o = (FlyText*)(intptr_t)out.payload;
    for (size_t i = 0; i < o->len; i++) o->data[i] = (char)tolower((unsigned char)o->data[i]);
    return out;
}

// SLEEP/NET follow-up: Fly 0.1's text literal syntax (lexer.cpp's lexText)
// has no backslash-escape mechanism at all -- only the `{{`/`}}` brace
// escapes and `{expr}` interpolation exist. That means there is currently
// NO way to write a literal `"` (or any other "awkward" byte like a raw
// control character) inside Fly SOURCE at all: it would either terminate
// the literal early or just isn't reachable by any existing syntax. Any
// Fly library that needs to construct or recognize such characters at
// runtime (a text-format parser/serializer needing to emit `"`, for
// instance -- exactly SLEEP's and JSON's situation) has no way to do it
// from source literals alone.
//
// These two are the minimal general-purpose primitives that close that
// gap, symmetric with each other, reachable via `native job
// rt_char_from_code(code)` / `native job rt_char_code(ch)`. Not a SLEEP-
// specific workaround -- any Fly library hand-building text a byte at a
// time hits the same wall.
FlyValue fly_rt_char_from_code(FlyValue codeV) {
    if (codeV.tag != FLY_NUM) fly_rt_throw("type error: char_from_code() expects a num");
    int64_t code = codeV.payload;
    if (code < 0 || code > 255) fly_rt_throw("char_from_code(): code must be between 0 and 255 (byte-level, not a Unicode codepoint -- see runtime/text.c)");
    char c = (char)(unsigned char)code;
    return makeText(&c, 1);
}

FlyValue fly_rt_char_code(FlyValue chV) {
    FlyText* t = asText(chV, "char_code");
    if (t->len != 1) fly_rt_throw("char_code(): expects exactly one character of tex");
    return fly_rt_num((int64_t)(unsigned char)t->data[0]);
}

FlyValue fly_rt_take(void) {
    DynBuf d; dbInit(&d);
    int c;
    int any = 0;
    while ((c = fgetc(stdin)) != EOF && c != '\n') { char ch = (char)c; dbAppend(&d, &ch, 1); any = 1; }
    if (!any && c == EOF) { free(d.buf); return fly_rt_emp(); } // spec doesn't cover EOF explicitly; EMP is the closest existing "nothing" value
    FlyValue out = makeText(d.buf, d.len);
    free(d.buf);
    return out;
}
