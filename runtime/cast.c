#include "flyrt.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

// docs/architecture.md §4.1's `cast.c/h`. `tex()` casts are NOT here --
// they reuse fly_rt_to_text() (runtime/text.c), which already IS the
// §10.3 "turn any FlyValue into its textual representation" conversion;
// see codegen.cpp's genCall for the num/dec/tex dispatch.
//
// Design, since the spec doesn't spell out exact cast semantics beyond
// naming the three casts: each cast accepts every scalar tag plus tex
// (parsed), and rejects EMP/coll/board (there's no sane numeric reading of
// "nothing" or a whole collection) with a thrown type error, matching the
// runtime's existing "throw on bad input" convention used throughout
// arith.c/text.c/coll.c/board.c.

// Trims ASCII whitespace from both ends of [s, s+len) -- tex sources are
// allowed leading/trailing whitespace (e.g. num(" 42 ")) the same way most
// scripting-language numeric parses are forgiving about it, but interior
// junk ("12x") is still rejected.
static void trimSpan(const char* s, size_t len, const char** outStart, size_t* outLen) {
    size_t start = 0, end = len;
    while (start < end && isspace((unsigned char)s[start])) start++;
    while (end > start && isspace((unsigned char)s[end - 1])) end--;
    *outStart = s + start;
    *outLen = end - start;
}

FlyValue fly_rt_cast_num(FlyValue v) {
    switch (v.tag) {
        case FLY_NUM:
            return v; // already num -- identity, no allocation involved either way
        case FLY_YN:
            return fly_rt_num(v.payload ? 1 : 0);
        case FLY_DEC: {
            double d = fly_rt_as_dec(v);
            if (d != d) fly_rt_throw("num(): cannot cast NaN to num"); // d != d <=> NaN
            // int64_t's representable double range is roughly
            // [-2^63, 2^63), checked explicitly rather than relying on
            // undefined-behavior-on-overflow (double-to-)int64 truncation.
            if (d < -9223372036854775808.0 || d >= 9223372036854775808.0)
                fly_rt_throw("num(): decimal value out of range for num");
            return fly_rt_num((int64_t)d); // truncates toward zero, matching C's (int64_t) cast semantics
        }
        case FLY_TEX: {
            const char* raw = fly_rt_text_data(v);
            size_t rawLen = fly_rt_text_len(v);
            const char* start; size_t len;
            trimSpan(raw, rawLen, &start, &len);
            if (len == 0) fly_rt_throw("num(): cannot cast empty/blank tex to num");
            char* buf = (char*)malloc(len + 1);
            if (!buf) fly_rt_throw("out of memory in num()");
            memcpy(buf, start, len);
            buf[len] = '\0';
            errno = 0;
            char* endp = NULL;
            long long parsed = strtoll(buf, &endp, 10);
            int bad = (endp != buf + len) || (errno == ERANGE);
            free(buf);
            if (bad) fly_rt_throw("num(): tex is not a valid whole-number literal");
            return fly_rt_num((int64_t)parsed);
        }
        default:
            fly_rt_throw("num(): cannot cast EMP, coll, or board to num");
            return fly_rt_emp(); // unreachable
    }
}

FlyValue fly_rt_cast_dec(FlyValue v) {
    switch (v.tag) {
        case FLY_DEC:
            return v; // identity
        case FLY_NUM:
            return fly_rt_dec((double)v.payload);
        case FLY_YN:
            return fly_rt_dec(v.payload ? 1.0 : 0.0);
        case FLY_TEX: {
            const char* raw = fly_rt_text_data(v);
            size_t rawLen = fly_rt_text_len(v);
            const char* start; size_t len;
            trimSpan(raw, rawLen, &start, &len);
            if (len == 0) fly_rt_throw("dec(): cannot cast empty/blank tex to dec");
            char* buf = (char*)malloc(len + 1);
            if (!buf) fly_rt_throw("out of memory in dec()");
            memcpy(buf, start, len);
            buf[len] = '\0';
            errno = 0;
            char* endp = NULL;
            double parsed = strtod(buf, &endp);
            int bad = (endp != buf + len) || (errno == ERANGE);
            free(buf);
            if (bad) fly_rt_throw("dec(): tex is not a valid decimal literal");
            return fly_rt_dec(parsed);
        }
        default:
            fly_rt_throw("dec(): cannot cast EMP, coll, or board to dec");
            return fly_rt_emp(); // unreachable
    }
}
