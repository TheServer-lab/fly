#include "flyrt.h"
#include <stdlib.h>
#include <string.h>

// FlyBoard (docs/architecture.md §2.2): parallel key/value arrays, linear
// scan. The full design upgrades to a real hash table above a size
// threshold (§2.2's hashing table); not built here yet -- correctness for
// v0.1-sized programs doesn't depend on it, and it's additive later.
//
// Key hashability rule from §2.2: num/dec/yn/tex/emp are valid keys; coll
// and board are not (equality on them isn't well-defined here). Enforced in
// keysEqual()/validateKey() below via fly_rt_throw, matching the spec's
// "runtime error when the key type can't be proven until runtime" path
// (Sema doesn't do the static-type-known variant of this check yet).

static FlyBoard* asBoard(FlyValue v, const char* op) {
    (void)op;
    if (v.tag != FLY_BOARD) fly_rt_throw("type error: expected board operand");
    return (FlyBoard*)(intptr_t)v.payload;
}

static void validateKey(FlyValue k) {
    if (k.tag == FLY_COLL || k.tag == FLY_BOARD)
        fly_rt_throw("type error: coll/board values cannot be used as board keys");
}

static int keysEqual(FlyValue a, FlyValue b) {
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
        case FLY_NUM: return a.payload == b.payload;
        case FLY_DEC: {
            double da = fly_rt_as_dec(a), db = fly_rt_as_dec(b);
            if (da != da || db != db) fly_rt_throw("NaN cannot be used as a board key"); // §2.2: fail fast, matches fly_rt_board_set's NaN rejection
            return da == db;
        }
        case FLY_YN:  return a.payload == b.payload;
        case FLY_EMP: return 1;
        case FLY_TEX: {
            size_t la = fly_rt_text_len(a), lb = fly_rt_text_len(b);
            return la == lb && memcmp(fly_rt_text_data(a), fly_rt_text_data(b), la) == 0;
        }
        default: return 0; // unreachable given validateKey()
    }
}

FlyValue fly_rt_board_new(void) {
    FlyBoard* b = (FlyBoard*)malloc(sizeof(FlyBoard));
    if (!b) fly_rt_throw("out of memory allocating board");
    b->hdr.refcount = 1;
    b->len = 0;
    b->cap = 4;
    b->keys = (FlyValue*)malloc(b->cap * sizeof(FlyValue));
    b->vals = (FlyValue*)malloc(b->cap * sizeof(FlyValue));
    if (!b->keys || !b->vals) fly_rt_throw("out of memory allocating board storage");
    FlyValue v; v.tag = FLY_BOARD; v.payload = (int64_t)(intptr_t)b;
    return v;
}

static void boardEnsureCap(FlyBoard* b, size_t needed) {
    if (needed <= b->cap) return;
    size_t newCap = b->cap ? b->cap : 4;
    while (newCap < needed) newCap *= 2;
    FlyValue* nk = (FlyValue*)realloc(b->keys, newCap * sizeof(FlyValue));
    FlyValue* nv = (FlyValue*)realloc(b->vals, newCap * sizeof(FlyValue));
    if (!nk || !nv) fly_rt_throw("out of memory growing board");
    b->keys = nk; b->vals = nv; b->cap = newCap;
}

static int64_t findKey(FlyBoard* b, FlyValue k) {
    for (size_t i = 0; i < b->len; i++)
        if (keysEqual(b->keys[i], k)) return (int64_t)i;
    return -1;
}

void fly_rt_board_put(FlyValue boardV, FlyValue k, FlyValue v) {
    validateKey(k);
    FlyBoard* b = asBoard(boardV, "board literal");
    int64_t idx = findKey(b, k);
    if (idx >= 0) {
        fly_rt_release(b->vals[idx]);
        b->vals[idx] = fly_rt_retain(v);
        return; // last entry for a duplicate key wins, matching normal literal semantics
    }
    boardEnsureCap(b, b->len + 1);
    b->keys[b->len] = fly_rt_retain(k);
    b->vals[b->len] = fly_rt_retain(v);
    b->len++;
}

FlyValue fly_rt_board_get(FlyValue boardV, FlyValue k) {
    validateKey(k);
    FlyBoard* b = asBoard(boardV, "board get");
    int64_t idx = findKey(b, k);
    if (idx < 0) return fly_rt_emp();
    return fly_rt_retain(b->vals[idx]);
}

FlyValue fly_rt_board_set(FlyValue boardV, FlyValue k, FlyValue v) {
    fly_rt_board_put(boardV, k, v);
    return fly_rt_emp();
}

FlyValue fly_rt_board_erase(FlyValue boardV, FlyValue k) {
    validateKey(k);
    FlyBoard* b = asBoard(boardV, "board erase");
    int64_t idx = findKey(b, k);
    if (idx < 0) fly_rt_throw("erase() key not found in board");
    FlyValue removed = b->vals[idx];
    fly_rt_release(b->keys[idx]);
    memmove(&b->keys[idx], &b->keys[idx + 1], (b->len - idx - 1) * sizeof(FlyValue));
    memmove(&b->vals[idx], &b->vals[idx + 1], (b->len - idx - 1) * sizeof(FlyValue));
    b->len--;
    return removed; // ownership transfers to caller (was already retained)
}

FlyValue fly_rt_board_has(FlyValue boardV, FlyValue k) {
    validateKey(k);
    FlyBoard* b = asBoard(boardV, "has");
    return fly_rt_yn(findKey(b, k) >= 0);
}

FlyValue fly_rt_board_count(FlyValue boardV) {
    return fly_rt_num((int64_t)asBoard(boardV, "count")->len);
}

// Positional key accessor (milestone 6, runtime/iter.c's board iteration
// support) -- see flyrt.h's comment on why this exists instead of exposing
// FlyBoard's layout directly.
FlyValue fly_rt_board_key_at(FlyValue boardV, size_t i) {
    FlyBoard* b = asBoard(boardV, "iteration");
    if (i >= b->len) fly_rt_throw("internal error: board key index out of range (fly-cc bug, not a Fly program error)");
    return fly_rt_retain(b->keys[i]);
}
