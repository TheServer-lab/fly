#include "flyrt.h"
#include <stdlib.h>
#include <string.h>

// Growable byte buffer backing string interpolation (docs/architecture.md
// §3.4: "allocate a growable buffer, append each literal chunk, append
// each interpolated expression's fly_rt_to_text(FlyValue) conversion,
// finalize into an ... owned FlyText"). Kept as an opaque type
// (FlyStrBuild in flyrt.h) so codegen only ever holds a pointer to it.
struct FlyStrBuild {
    char* buf;
    size_t len, cap;
};

FlyStrBuild* fly_rt_strbuild_new(void) {
    FlyStrBuild* b = (FlyStrBuild*)malloc(sizeof(FlyStrBuild));
    if (!b) fly_rt_throw("out of memory allocating string builder");
    b->cap = 64;
    b->len = 0;
    b->buf = (char*)malloc(b->cap);
    if (!b->buf) fly_rt_throw("out of memory allocating string builder buffer");
    return b;
}

static void ensureCap(FlyStrBuild* b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t newCap = b->cap ? b->cap : 64;
    while (newCap < b->len + extra) newCap *= 2;
    char* n = (char*)realloc(b->buf, newCap);
    if (!n) fly_rt_throw("out of memory growing string builder");
    b->buf = n;
    b->cap = newCap;
}

void fly_rt_strbuild_append_lit(FlyStrBuild* b, const char* s, size_t n) {
    ensureCap(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}

void fly_rt_strbuild_append_value(FlyStrBuild* b, FlyValue v) {
    FlyValue asTex = fly_rt_to_text(v); // spec §10.3 conversion table
    fly_rt_strbuild_append_lit(b, fly_rt_text_data(asTex), fly_rt_text_len(asTex));
    fly_rt_release(asTex);
}

FlyValue fly_rt_strbuild_finish(FlyStrBuild* b) {
    FlyValue out = fly_rt_text_from_bytes(b->buf, b->len);
    free(b->buf);
    free(b);
    return out;
}
