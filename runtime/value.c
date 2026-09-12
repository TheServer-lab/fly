#include "flyrt.h"
#include <string.h>

// Bit-reinterpretation helpers (dec is stored bit-cast into the i64 payload
// slot, see flyrt.h). Kept in one place so arith.c/show.c don't duplicate
// the memcpy dance.
double fly_rt_as_dec(FlyValue v) {
    double d;
    memcpy(&d, &v.payload, sizeof(double));
    return d;
}

FlyValue fly_rt_dec(double d) {
    FlyValue v; v.tag = FLY_DEC;
    memcpy(&v.payload, &d, sizeof(double));
    return v;
}

FlyValue fly_rt_num(int64_t n) {
    FlyValue v; v.tag = FLY_NUM; v.payload = n; return v;
}

FlyValue fly_rt_yn(int b) {
    FlyValue v; v.tag = FLY_YN; v.payload = b ? 1 : 0; return v;
}

FlyValue fly_rt_emp(void) {
    FlyValue v; v.tag = FLY_EMP; v.payload = 0; return v;
}

#include <stdlib.h>

// ---- ARC (docs/architecture.md §2.3) --------------------------------------

static void freeHeapObj(FlyValue v) {
    switch (v.tag) {
        case FLY_TEX: {
            FlyText* t = (FlyText*)(intptr_t)v.payload;
            free(t->data);
            free(t);
            return;
        }
        case FLY_COLL: {
            FlyColl* c = (FlyColl*)(intptr_t)v.payload;
            for (size_t i = 0; i < c->len; i++) fly_rt_release(c->items[i]);
            free(c->items);
            free(c);
            return;
        }
        case FLY_BOARD: {
            FlyBoard* b = (FlyBoard*)(intptr_t)v.payload;
            for (size_t i = 0; i < b->len; i++) {
                fly_rt_release(b->keys[i]);
                fly_rt_release(b->vals[i]);
            }
            free(b->keys);
            free(b->vals);
            free(b);
            return;
        }
        default:
            return; // scalar, nothing to free
    }
}

static FlyObjHeader* headerOf(FlyValue v) {
    // Every heap struct (FlyText/FlyColl/FlyBoard) starts with FlyObjHeader,
    // so this reinterpretation is safe regardless of which one it is.
    return (FlyObjHeader*)(intptr_t)v.payload;
}

FlyValue fly_rt_retain(FlyValue v) {
    if (v.tag == FLY_TEX || v.tag == FLY_COLL || v.tag == FLY_BOARD) {
        if (v.payload != 0) headerOf(v)->refcount++;
    }
    return v;
}

void fly_rt_release(FlyValue v) {
    if (v.tag != FLY_TEX && v.tag != FLY_COLL && v.tag != FLY_BOARD) return;
    if (v.payload == 0) return;
    FlyObjHeader* h = headerOf(v);
    if (h->refcount == 0) {
        fly_rt_throw("internal error: refcount underflow (fly-cc bug, not a Fly program error)");
        return;
    }
    h->refcount--;
    if (h->refcount == 0) freeHeapObj(v);
}

// SLEEP/NET follow-up: a minimal reflection primitive. Nothing before this
// let Fly SOURCE code ask "what kind of value is this" -- every existing
// builtin either only accepts one specific tag (arith.c's numeric ops) or
// silently coerces (show.c). That's fine for hand-written code that
// already knows its own shapes, but a *generic* library writing a value
// out recursively (SLEEP's/JSON's serializer, walking an arbitrary nested
// board/coll of unknown shape) has no way to tell "is this element a board,
// a coll, or a scalar" without one. Reachable from Fly source via
// `native job rt_typename(v)` (see codegen.cpp's declareNative convention).
// Deliberately returns a tex TAG NAME, not e.g. a num enum code -- matches
// this runtime's existing "errors/loosely-typed data as tex" style (see
// fly_rt_throw's tex-message convention) and needs no accompanying set of
// magic-number constants on the Fly side.
FlyValue fly_rt_typename(FlyValue v) {
    switch (v.tag) {
        case FLY_NUM:   return fly_rt_text_from_cstr("num");
        case FLY_DEC:   return fly_rt_text_from_cstr("dec");
        case FLY_YN:    return fly_rt_text_from_cstr("yn");
        case FLY_EMP:   return fly_rt_text_from_cstr("emp");
        case FLY_TEX:   return fly_rt_text_from_cstr("tex");
        case FLY_COLL:  return fly_rt_text_from_cstr("coll");
        case FLY_BOARD: return fly_rt_text_from_cstr("board");
        default:        return fly_rt_text_from_cstr("unknown");
    }
}
