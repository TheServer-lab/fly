#include "flyrt.h"

// docs/architecture.md §3.4.1: num overflow is a runtime error, never
// silent wrapping or promotion. The real design lowers this to
// llvm.sadd.with.overflow.i64 etc. directly in codegen for the common
// tag-proven case; this MVP keeps ALL arithmetic going through the runtime
// (see codegen.cpp's comment on why — simplicity for the first milestone),
// so the overflow check lives here via __builtin_*_overflow instead of an
// inlined intrinsic. Swapping this for inline IR later is a pure
// optimization, not a semantics change.

static FlyValue numNumOp(FlyValue a, FlyValue b, const char* opname,
                          int (*checked)(int64_t, int64_t, int64_t*)) {
    if (a.tag != FLY_NUM || b.tag != FLY_NUM)
        fly_rt_throw("type error: arithmetic requires two num values (dec/mixed-type arithmetic not yet implemented in this milestone)");
    int64_t result;
    if (checked(a.payload, b.payload, &result)) {
        fly_rt_throw("num overflow");
    }
    return fly_rt_num(result);
}

static int addOvf(int64_t a, int64_t b, int64_t* r) { return __builtin_add_overflow(a, b, r); }
static int subOvf(int64_t a, int64_t b, int64_t* r) { return __builtin_sub_overflow(a, b, r); }
static int mulOvf(int64_t a, int64_t b, int64_t* r) { return __builtin_mul_overflow(a, b, r); }

// Converts a num-or-dec FlyValue to a double for mixed arithmetic/comparison.
// NOTE: this is NOT the same as fly_rt_as_dec(), which assumes its argument's
// payload is already a bit-cast double (i.e. tag == FLY_DEC). A FLY_NUM's
// payload is a raw int64 value, not a bit-cast double, so bit-reinterpreting
// it (as the old code did via fly_rt_as_dec on a NUM) produces garbage
// (e.g. num 2's bit pattern read as a double is a tiny denormal, not 2.0).
// This must actually convert (round) the integer to its double value.
static double toDec(FlyValue v) {
    if (v.tag == FLY_DEC) return fly_rt_as_dec(v);
    if (v.tag == FLY_NUM) return (double)v.payload;
    fly_rt_throw("type error: expected num or dec operand");
    return 0.0; // unreachable
}

FlyValue fly_rt_add(FlyValue a, FlyValue b) {
    if (a.tag == FLY_DEC || b.tag == FLY_DEC) return fly_rt_dec(toDec(a) + toDec(b));
    return numNumOp(a, b, "add", addOvf);
}
FlyValue fly_rt_sub(FlyValue a, FlyValue b) {
    if (a.tag == FLY_DEC || b.tag == FLY_DEC) return fly_rt_dec(toDec(a) - toDec(b));
    return numNumOp(a, b, "sub", subOvf);
}
FlyValue fly_rt_mul(FlyValue a, FlyValue b) {
    if (a.tag == FLY_DEC || b.tag == FLY_DEC) return fly_rt_dec(toDec(a) * toDec(b));
    return numNumOp(a, b, "mul", mulOvf);
}
FlyValue fly_rt_div(FlyValue a, FlyValue b) {
    if (a.tag == FLY_DEC || b.tag == FLY_DEC) return fly_rt_dec(toDec(a) / toDec(b));
    if (a.tag != FLY_NUM || b.tag != FLY_NUM) fly_rt_throw("type error: '/' requires num or dec operands");
    if (b.payload == 0) fly_rt_throw("division by zero");
    return fly_rt_num(a.payload / b.payload);
}
FlyValue fly_rt_mod(FlyValue a, FlyValue b) {
    if (a.tag != FLY_NUM || b.tag != FLY_NUM) fly_rt_throw("type error: '%' requires num operands");
    if (b.payload == 0) fly_rt_throw("modulo by zero");
    return fly_rt_num(a.payload % b.payload);
}

static int valuesEqual(FlyValue a, FlyValue b) {
    if (a.tag != b.tag) return 0;
    if (a.tag == FLY_DEC) return fly_rt_as_dec(a) == fly_rt_as_dec(b);
    if (a.tag == FLY_TEX) {
        // Content equality, not pointer identity (milestone 4: tex is now
        // a real heap object, and two interpolated/concatenated strings
        // with the same content are obviously meant to compare equal --
        // e.g. `name == "Rick"` shouldn't depend on which allocation
        // produced `name`).
        size_t la = fly_rt_text_len(a), lb = fly_rt_text_len(b);
        if (la != lb) return 0;
        const char* da = fly_rt_text_data(a);
        const char* db = fly_rt_text_data(b);
        for (size_t i = 0; i < la; i++) if (da[i] != db[i]) return 0;
        return 1;
    }
    // coll/board: identity comparison for this MVP -- the spec doesn't
    // define deep-equality semantics for them, so pointer equality is at
    // least sound (never falsely reports two distinct collections equal).
    return a.payload == b.payload;
}

FlyValue fly_rt_eq(FlyValue a, FlyValue b)  { return fly_rt_yn(valuesEqual(a, b)); }
FlyValue fly_rt_neq(FlyValue a, FlyValue b) { return fly_rt_yn(!valuesEqual(a, b)); }

FlyValue fly_rt_lt(FlyValue a, FlyValue b) {
    if ((a.tag == FLY_DEC || a.tag == FLY_NUM) && (b.tag == FLY_DEC || b.tag == FLY_NUM)) {
        if (a.tag == FLY_DEC || b.tag == FLY_DEC) return fly_rt_yn(toDec(a) < toDec(b));
        return fly_rt_yn(a.payload < b.payload);
    }
    fly_rt_throw("type error: '<' requires num/dec operands");
    return fly_rt_yn(0); // unreachable
}
FlyValue fly_rt_gt(FlyValue a, FlyValue b) { return fly_rt_lt(b, a); }
FlyValue fly_rt_le(FlyValue a, FlyValue b) { FlyValue g = fly_rt_gt(a, b); return fly_rt_yn(!g.payload); }
FlyValue fly_rt_ge(FlyValue a, FlyValue b) { FlyValue l = fly_rt_lt(a, b); return fly_rt_yn(!l.payload); }

FlyValue fly_rt_and(FlyValue a, FlyValue b) {
    if (a.tag != FLY_YN || b.tag != FLY_YN) fly_rt_throw("type error: 'and' requires yn operands");
    return fly_rt_yn(a.payload && b.payload);
}
FlyValue fly_rt_or(FlyValue a, FlyValue b) {
    if (a.tag != FLY_YN || b.tag != FLY_YN) fly_rt_throw("type error: 'or' requires yn operands");
    return fly_rt_yn(a.payload || b.payload);
}
FlyValue fly_rt_not(FlyValue a) {
    if (a.tag != FLY_YN) fly_rt_throw("type error: 'not' requires a yn operand");
    return fly_rt_yn(!a.payload);
}
FlyValue fly_rt_neg(FlyValue a) {
    if (a.tag == FLY_DEC) return fly_rt_dec(-fly_rt_as_dec(a));
    if (a.tag != FLY_NUM) fly_rt_throw("type error: unary '-' requires num or dec");
    if (a.payload == INT64_MIN) fly_rt_throw("num overflow");
    return fly_rt_num(-a.payload);
}
