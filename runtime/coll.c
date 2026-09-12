#include "flyrt.h"
#include <stdlib.h>
#include <string.h>

// FlyColl (docs/architecture.md §2.2/§4): growable array of FlyValue.
// ARC note: every slot holds a RETAINED value; pushing into the coll
// retains, popping/removing/overwriting a slot releases. The coll itself
// starts life at refcount 1 (owned by whoever created it -- a literal's
// codegen, or an intermediate builtin result); IRGen releases it same as
// any other heap value at end-of-scope.

static FlyColl* asColl(FlyValue v, const char* op) {
    (void)op;
    if (v.tag != FLY_COLL) fly_rt_throw("type error: expected coll operand");
    return (FlyColl*)(intptr_t)v.payload;
}

FlyValue fly_rt_coll_new(size_t cap_hint) {
    FlyColl* c = (FlyColl*)malloc(sizeof(FlyColl));
    if (!c) fly_rt_throw("out of memory allocating coll");
    c->hdr.refcount = 1;
    c->len = 0;
    c->cap = cap_hint > 0 ? cap_hint : 4;
    c->items = (FlyValue*)malloc(c->cap * sizeof(FlyValue));
    if (!c->items) fly_rt_throw("out of memory allocating coll storage");
    FlyValue v; v.tag = FLY_COLL; v.payload = (int64_t)(intptr_t)c;
    return v;
}

static void collEnsureCap(FlyColl* c, size_t needed) {
    if (needed <= c->cap) return;
    size_t newCap = c->cap ? c->cap : 4;
    while (newCap < needed) newCap *= 2;
    FlyValue* n = (FlyValue*)realloc(c->items, newCap * sizeof(FlyValue));
    if (!n) fly_rt_throw("out of memory growing coll");
    c->items = n;
    c->cap = newCap;
}

void fly_rt_coll_push(FlyValue collV, FlyValue v) {
    FlyColl* c = asColl(collV, "coll literal");
    collEnsureCap(c, c->len + 1);
    c->items[c->len++] = fly_rt_retain(v);
}

static int64_t checkedIndex(FlyValue idxV, size_t len, const char* what) {
    if (idxV.tag != FLY_NUM) fly_rt_throw("type error: index must be a num");
    int64_t idx = idxV.payload;
    if (idx < 0 || (size_t)idx >= len) fly_rt_throw(what);
    return idx;
}

FlyValue fly_rt_attach(FlyValue collV, FlyValue v) {
    FlyColl* c = asColl(collV, "attach");
    collEnsureCap(c, c->len + 1);
    c->items[c->len++] = fly_rt_retain(v);
    return fly_rt_emp();
}

FlyValue fly_rt_place(FlyValue container, FlyValue idxOrKey, FlyValue v) {
    if (container.tag == FLY_BOARD) return fly_rt_board_set(container, idxOrKey, v); // board's natural "insert-or-overwrite", §12's access syntax is left open by the spec so this reuses place()'s vocabulary rather than adding a new op name
    FlyColl* c = asColl(container, "place");
    FlyValue idxV = idxOrKey;
    if (idxV.tag != FLY_NUM) fly_rt_throw("type error: place() index must be a num");
    int64_t idx = idxV.payload;
    if (idx < 0 || (size_t)idx > c->len) fly_rt_throw("place() index out of range");
    collEnsureCap(c, c->len + 1);
    memmove(&c->items[idx + 1], &c->items[idx], (c->len - idx) * sizeof(FlyValue));
    c->items[idx] = fly_rt_retain(v);
    c->len++;
    return fly_rt_emp();
}

FlyValue fly_rt_erase(FlyValue container, FlyValue idxV) {
    if (container.tag == FLY_BOARD) return fly_rt_board_erase(container, idxV);
    FlyColl* c = asColl(container, "erase");
    int64_t idx = checkedIndex(idxV, c->len, "erase() index out of range");
    FlyValue removed = c->items[idx];
    memmove(&c->items[idx], &c->items[idx + 1], (c->len - idx - 1) * sizeof(FlyValue));
    c->len--;
    return removed; // ownership transfers to the caller (was already retained in the slot)
}

FlyValue fly_rt_count(FlyValue container) {
    if (container.tag == FLY_COLL)  return fly_rt_num((int64_t)asColl(container, "count")->len);
    if (container.tag == FLY_TEX)   return fly_rt_num((int64_t)fly_rt_text_len(container));
    if (container.tag == FLY_BOARD) return fly_rt_board_count(container);
    fly_rt_throw("type error: count() requires coll, board, or tex");
    return fly_rt_emp();
}

static int valuesEqualForSearch(FlyValue a, FlyValue b) {
    if (a.tag != b.tag) return 0;
    if (a.tag == FLY_TEX) {
        size_t la = fly_rt_text_len(a), lb = fly_rt_text_len(b);
        return la == lb && memcmp(fly_rt_text_data(a), fly_rt_text_data(b), la) == 0;
    }
    if (a.tag == FLY_DEC) return fly_rt_as_dec(a) == fly_rt_as_dec(b);
    // coll/board aren't valid seek() needles for element-equality purposes
    // in this MVP (would need deep-equality, not defined by the spec); fall
    // back to identity (pointer) comparison, which is at least sound.
    return a.payload == b.payload;
}

FlyValue fly_rt_seek(FlyValue container, FlyValue needle) {
    if (container.tag == FLY_TEX) {
        if (needle.tag != FLY_TEX) fly_rt_throw("type error: seek() on tex requires a tex needle");
        const char* hay = fly_rt_text_data(container);
        size_t hayLen = fly_rt_text_len(container);
        const char* pat = fly_rt_text_data(needle);
        size_t patLen = fly_rt_text_len(needle);
        if (patLen == 0) return fly_rt_num(0);
        if (patLen > hayLen) return fly_rt_emp();
        for (size_t i = 0; i + patLen <= hayLen; i++)
            if (memcmp(hay + i, pat, patLen) == 0) return fly_rt_num((int64_t)i);
        return fly_rt_emp();
    }
    FlyColl* c = asColl(container, "seek");
    for (size_t i = 0; i < c->len; i++)
        if (valuesEqualForSearch(c->items[i], needle)) return fly_rt_num((int64_t)i);
    return fly_rt_emp();
}

FlyValue fly_rt_has(FlyValue container, FlyValue needle) {
    if (container.tag == FLY_BOARD) return fly_rt_board_has(container, needle); // key-existence, §15.6 extended naturally to boards
    FlyValue pos = fly_rt_seek(container, needle);
    return fly_rt_yn(pos.tag != FLY_EMP);
}

FlyValue fly_rt_bind(FlyValue collV, FlyValue sepV) {
    FlyColl* c = asColl(collV, "bind");
    if (sepV.tag != FLY_TEX) fly_rt_throw("type error: bind() separator must be tex");
    const char* sep = fly_rt_text_data(sepV);
    size_t sepLen = fly_rt_text_len(sepV);
    size_t total = 0;
    for (size_t i = 0; i < c->len; i++) {
        FlyValue asTex = fly_rt_to_text(c->items[i]);
        total += fly_rt_text_len(asTex);
        if (i + 1 < c->len) total += sepLen;
        fly_rt_release(asTex);
    }
    char* buf = (char*)malloc(total + 1);
    if (!buf && total > 0) fly_rt_throw("out of memory in bind()");
    size_t pos = 0;
    for (size_t i = 0; i < c->len; i++) {
        FlyValue asTex = fly_rt_to_text(c->items[i]);
        size_t n = fly_rt_text_len(asTex);
        if (n) memcpy(buf + pos, fly_rt_text_data(asTex), n);
        pos += n;
        fly_rt_release(asTex);
        if (i + 1 < c->len) { memcpy(buf + pos, sep, sepLen); pos += sepLen; }
    }
    FlyValue out = fly_rt_text_from_bytes(buf, pos);
    free(buf);
    return out;
}

FlyValue fly_rt_sever(FlyValue textV, FlyValue sepV) {
    if (textV.tag != FLY_TEX || sepV.tag != FLY_TEX)
        fly_rt_throw("type error: sever() requires two tex operands");
    const char* s = fly_rt_text_data(textV);
    size_t sLen = fly_rt_text_len(textV);
    const char* sep = fly_rt_text_data(sepV);
    size_t sepLen = fly_rt_text_len(sepV);
    FlyValue out = fly_rt_coll_new(4);

    if (sepLen == 0) {
        // Degenerate case (empty separator): split into individual bytes,
        // the least-surprising behavior when there's no delimiter to match.
        for (size_t i = 0; i < sLen; i++) {
            FlyValue piece = fly_rt_text_from_bytes(s + i, 1);
            fly_rt_coll_push(out, piece);
            fly_rt_release(piece);
        }
        return out;
    }

    size_t start = 0;
    for (size_t i = 0; i + sepLen <= sLen; ) {
        if (memcmp(s + i, sep, sepLen) == 0) {
            FlyValue piece = fly_rt_text_from_bytes(s + start, i - start);
            fly_rt_coll_push(out, piece);
            fly_rt_release(piece);
            i += sepLen;
            start = i;
        } else {
            i++;
        }
    }
    FlyValue piece = fly_rt_text_from_bytes(s + start, sLen - start);
    fly_rt_coll_push(out, piece);
    fly_rt_release(piece);
    return out;
}

// ---- indexing / slicing (spec §13/§14), dispatched over coll/tex ----------

FlyValue fly_rt_index(FlyValue container, FlyValue idxV) {
    if (container.tag == FLY_TEX) {
        size_t len = fly_rt_text_len(container);
        int64_t idx = checkedIndex(idxV, len, "text index out of range");
        return fly_rt_text_from_bytes(fly_rt_text_data(container) + idx, 1);
    }
    if (container.tag == FLY_COLL) {
        FlyColl* c = asColl(container, "index");
        int64_t idx = checkedIndex(idxV, c->len, "coll index out of range");
        return fly_rt_retain(c->items[idx]);
    }
    if (container.tag == FLY_BOARD) {
        // board's "access syntax" is left open by spec §12; `board[key]`
        // reads as the natural extension of coll/tex indexing, returning
        // EMP for a missing key (consistent with seek()'s not-found EMP).
        return fly_rt_board_get(container, idxV);
    }
    fly_rt_throw("type error: '[...]' indexing requires coll, tex, or board");
    return fly_rt_emp();
}

// Clamps and resolves an (possibly-EMP, meaning "omitted") slice bound to a
// concrete [0, len] value, per spec §14's inclusive-start/exclusive-end,
// omittable-boundary semantics.
static size_t resolveBound(FlyValue boundV, size_t len, size_t defaultVal) {
    if (boundV.tag == FLY_EMP) return defaultVal;
    if (boundV.tag != FLY_NUM) fly_rt_throw("type error: slice bound must be a num");
    int64_t b = boundV.payload;
    if (b < 0) b = 0; // clamp rather than throw -- keeps slicing forgiving, matches most scripting languages
    if ((size_t)b > len) b = (int64_t)len;
    return (size_t)b;
}

FlyValue fly_rt_slice(FlyValue container, FlyValue fromV, FlyValue toV) {
    if (container.tag == FLY_TEX) {
        size_t len = fly_rt_text_len(container);
        size_t from = resolveBound(fromV, len, 0);
        size_t to = resolveBound(toV, len, len);
        if (to < from) to = from;
        return fly_rt_text_from_bytes(fly_rt_text_data(container) + from, to - from);
    }
    if (container.tag == FLY_COLL) {
        FlyColl* c = asColl(container, "slice");
        size_t from = resolveBound(fromV, c->len, 0);
        size_t to = resolveBound(toV, c->len, c->len);
        if (to < from) to = from;
        FlyValue out = fly_rt_coll_new(to - from);
        for (size_t i = from; i < to; i++) fly_rt_coll_push(out, c->items[i]);
        return out;
    }
    fly_rt_throw("type error: '[a:b]' slicing requires coll or tex");
    return fly_rt_emp();
}
