// Milestone 5 (`do`/`grabe`, docs/architecture.md §3.5): real error
// unwinding.
//
// Rather than hand-rolling a personality routine + LSDA (Language Specific
// Data Area) parser from scratch, this builds on the Itanium C++ exception
// ABI that libstdc++/libgcc already provide on this platform
// (__cxa_throw / __cxa_begin_catch / __cxa_end_catch, the
// __gxx_personality_v0 personality routine, libunwind underneath). This is
// the same "prove the need first" bias the rest of this codebase applies
// (see e.g. board.c's linear-scan-until-a-hash-table-is-proven-necessary
// comment) -- fly-cc's IRGen still emits genuine LLVM `invoke`/`landingpad`
// instructions per architecture.md's "LLVM exception machinery with
// invoke/landingpad and libunwind" (see codegen.cpp's genDoGrabe), this
// file just supplies the runtime half of that contract by reusing the
// existing, battle-tested ABI implementation instead of duplicating it.
//
// This file is compiled as C++ (needed for <cxxabi.h>/<typeinfo>) but
// exposes only the plain-C `flyrt.h` surface, same as every other runtime
// file -- IRGen and the rest of libflyrt never need to know this one is
// C++ under the hood.
//
// ---- Documented MVP simplification (loud, not silent, per this repo's
// established style) --------------------------------------------------
// A `grabe` clause always catches EVERY thrown Fly error via a single
// catch-all landingpad clause (`catch i8* null`, the standard Itanium-ABI
// idiom for a `catch (...)`) -- Fly 0.1's spec doesn't define multiple
// typed error kinds yet, so there's nothing to type-match against. A
// minimal FlyError RTTI object still exists below purely because
// __cxa_throw's signature requires a non-null std::type_info*; codegen.cpp
// never inspects or references it.
//
// A SEPARATE, also-documented limitation lives in codegen.cpp's
// genDoGrabe/emitCall comments: ARC cleanup-on-unwind (releasing
// tex/coll/board locals that are live in a scope an exception unwinds
// through) is not implemented in this milestone. Errors ARE caught and
// `grabe` DOES run with the right value -- only refcounts on in-flight
// locals at the unwind point aren't released, which is a real, separate
// piece of follow-up work (the "cleanup landingpad at every scope"
// mechanism a C++ compiler normally builds for local destructors).

#include "flyrt.h"
#include <cxxabi.h>
#include <typeinfo>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// A trivial std::type_info subclass so __cxa_throw has a non-null RTTI
// pointer to hand back. Fly 0.1 has exactly one kind of thrown value (any
// FlyValue, wrapped verbatim), so one global instance is all that's ever
// needed -- see the file header comment on why codegen never looks at it.
class FlyErrorTypeInfo : public std::type_info {
public:
    FlyErrorTypeInfo() : std::type_info("FlyError") {}
    ~FlyErrorTypeInfo() override = default;
};

FlyErrorTypeInfo g_fly_error_tinfo;

} // namespace

extern "C" {

void fly_rt_throw_value(FlyValue errval) {
    // __cxa_allocate_exception hands back `size` bytes of storage for the
    // thrown object, sitting right after the hidden unwind header it also
    // manages -- we just memcpy the FlyValue's bytes in verbatim (it's a
    // POD struct, so no constructor/destructor thunk is needed, hence the
    // final `nullptr` below).
    void* buf = __cxxabiv1::__cxa_allocate_exception(sizeof(FlyValue));
    std::memcpy(buf, &errval, sizeof(FlyValue));
    __cxxabiv1::__cxa_throw(buf, &g_fly_error_tinfo, nullptr);
}

void fly_rt_throw(const char* msg) {
    fly_rt_throw_value(fly_rt_text_from_cstr(msg));
}

void* fly_rt_begin_catch(void* excObj) {
    return __cxxabiv1::__cxa_begin_catch(excObj);
}

void fly_rt_end_catch(void) {
    __cxxabiv1::__cxa_end_catch();
}

FlyValue fly_rt_catch_extract(void* caught) {
    // __cxa_begin_catch returns a pointer to the (possibly-adjusted)
    // thrown object -- for our non-polymorphic, non-derived FlyValue
    // payload there's no adjustment, so this is exactly the buffer
    // fly_rt_throw_value memcpy'd into.
    FlyValue v;
    std::memcpy(&v, caught, sizeof(FlyValue));
    return v;
}

// SLEEP/NET follow-up: a general-purpose "raise a user-level Fly error
// from Fly source" primitive, reachable via `native job rt_throwv(err)`
// (codegen.cpp's declareNative binds it to this exact `fly_<name>` -> C
// symbol convention, see flyrt.h's §5 header comment). Every OTHER
// fly_rt_* error path so far is runtime-internal (a type error, an out-
// of-range index, an OS failure) -- nothing before this let ordinary Fly
// *library* code (like sleep.fly's parser, validating malformed input)
// throw its OWN catchable error with a message it composed itself. This
// is declared FlyValue-returning (not void) purely so its native-decl ABI
// matches every other native call site's calling convention (see
// codegen.cpp's declareNative: every native symbol is `FlyValue(FlyValue...)`
// today) -- the return is never actually reached, since fly_rt_throw_value
// always unwinds.
FlyValue fly_rt_throwv(FlyValue errval) {
    fly_rt_throw_value(errval);
    return fly_rt_emp(); // unreachable
}

void fly_rt_report_uncaught(FlyValue errval) {
    FlyValue asText = fly_rt_to_text(errval); // spec §10.3 conversion table, same as show()/interpolation use
    fprintf(stderr, "fly: runtime error: %.*s\n",
            (int)fly_rt_text_len(asText), fly_rt_text_data(asText));
    fly_rt_release(asText);
    exit(1);
}

} // extern "C"
