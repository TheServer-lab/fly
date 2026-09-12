#include "flyrt.h"
#include <stdlib.h>

// docs/architecture.md §3.4 ("`for` lowers against the internal
// FlyIterator protocol") / §4.1's `iter.c/h`. See flyrt.h's comment on the
// three public entry points for the MVP scope this implements: coll
// elements, board keys, tex bytes, snapshotted length at creation time.
//
// FlyIter is intentionally opaque outside this file -- codegen.cpp only
// ever holds the `void*` handle fly_rt_iter_new returns and passes it
// straight back into has_next/next/free, exactly like FlyStrBuild
// (runtime/strbuild.c) already does for string-building state.

typedef enum { ITER_COLL, ITER_BOARD, ITER_TEX } IterKind;

typedef struct {
    FlyValue container; // retained for the iterator's lifetime
    IterKind kind;
    size_t pos;
    size_t len; // snapshotted at fly_rt_iter_new time -- see flyrt.h's mutation-during-iteration note
} FlyIter;

void* fly_rt_iter_new(FlyValue container) {
    IterKind kind;
    size_t len;
    switch (container.tag) {
        case FLY_COLL:  kind = ITER_COLL;  len = (size_t)fly_rt_count(container).payload; break;
        case FLY_BOARD: kind = ITER_BOARD; len = (size_t)fly_rt_count(container).payload; break;
        case FLY_TEX:   kind = ITER_TEX;   len = fly_rt_text_len(container); break;
        default:
            fly_rt_throw("'for ... in' requires a coll, board, or tex value");
            return NULL; // unreachable
    }
    FlyIter* it = (FlyIter*)malloc(sizeof(FlyIter));
    if (!it) fly_rt_throw("out of memory allocating iterator");
    it->container = fly_rt_retain(container);
    it->kind = kind;
    it->pos = 0;
    it->len = len;
    return it;
}

int fly_rt_iter_has_next(void* itp) {
    FlyIter* it = (FlyIter*)itp;
    return it->pos < it->len;
}

// Board iteration uses fly_rt_board_key_at (runtime/board.c) -- a small,
// purpose-built positional accessor rather than reaching into FlyBoard's
// layout directly, keeping that layout private to board.c like every other
// board op already does (see flyrt.h's comment on that function).

FlyValue fly_rt_iter_next(void* itp) {
    FlyIter* it = (FlyIter*)itp;
    if (it->pos >= it->len) fly_rt_throw("internal error: iterator advanced past its end (fly-cc bug, not a Fly program error)");
    size_t i = it->pos++;
    switch (it->kind) {
        case ITER_COLL:  return fly_rt_index(it->container, fly_rt_num((int64_t)i));
        case ITER_BOARD: return fly_rt_board_key_at(it->container, i);
        case ITER_TEX:   return fly_rt_index(it->container, fly_rt_num((int64_t)i));
    }
    return fly_rt_emp(); // unreachable
}

void fly_rt_iter_free(void* itp) {
    FlyIter* it = (FlyIter*)itp;
    fly_rt_release(it->container);
    free(it);
}
