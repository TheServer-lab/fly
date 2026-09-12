#include "flycc/codegen.h"
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <iostream>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

namespace flycc {

namespace {

// Tag values MUST match runtime/flyrt.h's FlyTag enum exactly.
enum FlyTag : int8_t {
    FLY_NUM = 0, FLY_DEC = 1, FLY_YN = 2, FLY_EMP = 3,
    FLY_TEX = 4, FLY_COLL = 5, FLY_BOARD = 6,
};

// ---- ARC ownership convention used throughout this file -------------------
//
// docs/architecture.md §2.3: retain on assignment/copy into a new binding
// or container slot, release at end-of-scope for locals and when a slot is
// overwritten. This milestone's codegen keeps that simple by enforcing ONE
// invariant everywhere:
//
//   Every Value* returned by genExpr() is a "+1 owned" temporary: if it's a
//   heap value (tex/coll/board) the caller holds a reference nobody else is
//   accounting for, which the caller must either (a) transfer into a
//   persistent slot with NO extra retain (a fresh store into a variable
//   alloca, or a `give`/fallthrough return -- both just move the value,
//   they don't copy it), or (b) release exactly once after it's done using
//   it as an intermediate.
//
// fly_rt_retain/fly_rt_release are no-ops for scalar tags (checked by the
// runtime, not by codegen), so it's always SAFE to call release on a temp
// even when Sema/the grammar guarantee it's scalar-only (arithmetic,
// logical ops) -- this file still does so uniformly rather than trying to
// prove scalar-only-ness in codegen, since that provability work is
// exactly the §3.3-point-3 type-hinting optimization this milestone
// explicitly doesn't implement yet (see codegen.h's calling-convention
// note). The cost is a few extra no-op runtime calls on the scalar-only
// fast paths; correctness for heap types (e.g. `tex == tex` comparing two
// freshly-interpolated strings) is what actually matters here.
//
// Concretely, per genExpr case:
//   - Ident load:      retains the loaded value before returning it (a
//                       "copy into a new binding" per §2.3, even though the
//                       new binding here is just an expression temporary).
//   - Literals/casts:  every constructor (fly_rt_text_from_cstr,
//                       fly_rt_coll_new, fly_rt_board_new,
//                       fly_rt_strbuild_finish, ...) returns a FRESH
//                       object at refcount 1 -- that's already the "+1
//                       owned" the convention wants, no extra retain.
//   - Binary/Unary/builtin calls: operand temps are released right after
//                       the call consumes them; the call's OWN result is a
//                       fresh +1 (see above) that becomes this node's
//                       return value.
//   - Call (user job):  arguments are passed as their raw +1 temps with NO
//                       extra retain/release at the call site -- ownership
//                       transfers straight into the callee's parameter
//                       alloca (mirrors a VarDecl store), and the callee
//                       releases it at its own end-of-scope like any other
//                       local. This is the one case where a genExpr() temp
//                       is consumed WITHOUT an explicit release call here.
//
// Scope cleanup (the other half of §2.3):
//   - releaseCurrentScope(): called when a block/clause/loop-body finishes
//     WITHOUT an early `give` (no terminator yet in the current LLVM basic
//     block) -- releases just that scope's own locals so loop bodies don't
//     accumulate references across iterations.
//   - releaseAllScopes(): called right before any `ret` (explicit `give`,
//     or the implicit "fell off the end -> EMP" return) -- releases every
//     local across every still-active scope frame, since LLVM only allows
//     one terminator per basic block, so a `ret` deep inside nested blocks
//     can't rely on those blocks' own normal (non-terminated) cleanup path
//     running afterward.

class Emitter {
public:
    Emitter(LLVMContext& ctx, Module& mod, const std::string& filename)
        : ctx_(ctx), mod_(mod), builder_(ctx), filename_(filename) {
// { i64 tag, i64 payload } -- see codegen.h and runtime/flyrt.h for
        // the ABI-compatibility assumption (non-packed, matching target
        // datalayout on both the LLVM and C-compiled-runtime sides). The
        // {i64,i64} layout is what BOTH SysV x86-64 and LLVM's own Win64
        // convention want for a 16-byte sequence of two INTEGER eight-bytes
        // (returned in rax:rdx, passed by value in register pairs) -- that
        // is exactly the path POSIX uses, and the one LLVM-generated job
        // bodies use on Windows too (caller and callee agree). MinGW-GCC's
        // `-mabi=ms` is the one divergence: it lowers 16-byte INTEGER
        // aggregates to memory -- hidden sret out-pointer in RCX for the
        // result, byref copy for every by-value parameter -- which no
        // calling-convention attribute reconciles with LLVM. So on _WIN32
        // the runtime calls are re-declared and re-marshalled to that
        // memory ABI -- see RtSig/declareRt/loweredRuntimeCall below.
        i8Ty_ = Type::getInt8Ty(ctx_);
        i32Ty_ = Type::getInt32Ty(ctx_);
        i64Ty_ = Type::getInt64Ty(ctx_);
        dblTy_ = Type::getDoubleTy(ctx_);
        voidTy_ = Type::getVoidTy(ctx_);
        i8PtrTy_ = PointerType::getUnqual(ctx_);
        flyValueTy_ = StructType::create(ctx_, {i64Ty_, i64Ty_}, "FlyValue");
        // landingpad result type, per the Itanium ABI: { exception-object
        // ptr, selector index } -- see genDoGrabe / declareRuntime's
        // personality-function setup below (milestone 5, §3.5).
        lpTy_ = StructType::get(ctx_, {i8PtrTy_, Type::getInt32Ty(ctx_)});
        declareRuntime();
    }

    void run(Program& prog) {
        // Pass 0: bind every `native job` declaration to its libflyrt
        // symbol. No ordering requirement vs. jobs/statements -- native
        // declarations have no body to generate, just a callable binding
        // that genCall (via nativeFuncs_) and Sema's arity check both need
        // available before any call site is processed.
        for (auto& s : prog.top_level)
            if (s->kind == StmtKind::NativeJobDecl) declareNative(s.get());

        // Pass 1: forward-declare every job so call sites resolve
        // regardless of textual order (no ordering requirement in the
        // grammar).
        for (auto& s : prog.top_level)
            if (s->kind == StmtKind::JobDecl) declareJob(s.get());

        // Pass 2: define job bodies.
        for (auto& s : prog.top_level)
            if (s->kind == StmtKind::JobDecl) defineJob(s.get());

        // SLEEP/NET follow-up: bind qualified module-call aliases
        // (ast.h's Program::module_aliases) to whatever flat-named job or
        // native declaration they point at, so e.g. a `bring sleep` site's
        // `sleep.parse(...)` call resolves genCall's jobFuncs_/nativeFuncs_
        // lookup exactly like the flat `parse(...)` spelling would -- see
        // genCall's lookup order (builtins table, then casts, then native,
        // then jobFuncs_: this alias just adds another key into the same
        // maps, no new dispatch path).
        for (auto& [qualified, flat] : prog.module_aliases) {
            auto jf = jobFuncs_.find(flat);
            if (jf != jobFuncs_.end()) { jobFuncs_[qualified] = jf->second; continue; }
            auto nf = nativeFuncs_.find(flat);
            if (nf != nativeFuncs_.end()) {
                nativeFuncs_[qualified] = nf->second;
                nativeArity_[qualified] = nativeArity_[flat];
            }
        }

        // Pass 3: top-level statements become the body of C `main`, wrapped
        // in an IMPLICIT top-level do/grabe (spec §3.5: "unhandled errors
        // propagate to the top-level program entry and terminate with an
        // error message and nonzero exit code") -- so a Fly error that
        // unwinds past every user `grabe` still gets exactly that
        // treatment instead of falling through to std::terminate/abort.
        //
        // milestone 7, §5.1: `main` now takes (argc, argv) -- matching C's
        // `int main(int argc, char** argv)` -- purely so it can hand them
        // to fly_rt_init_args() before running any Fly code, backing
        // process.args(). Fly source itself never sees argc/argv directly;
        // it only ever observes them through fly_rt_args()'s coll.
        FunctionType* mainTy = FunctionType::get(i64Ty_, {i32Ty_, PointerType::getUnqual(i8PtrTy_)}, false);
        Function* mainFn = Function::Create(mainTy, Function::ExternalLinkage, "main", mod_);
        mainFn->setUWTableKind(UWTableKind::Async); // see declareJob's comment
        BasicBlock* entry = BasicBlock::Create(ctx_, "entry", mainFn);
        builder_.SetInsertPoint(entry);
        curFn_ = mainFn;
        scopes_.clear();
        scopes_.emplace_back();
        {
            auto argIt = mainFn->args().begin();
            Value* argcArg = &*argIt++;
            Value* argvArg = &*argIt;
            builder_.CreateCall(rt_["fly_rt_init_args"], {argcArg, argvArg}); // never throws -- plain call
        }
        ensurePersonality();
        BasicBlock* topLandingBB = BasicBlock::Create(ctx_, "toplevel.landing", mainFn);
        invokeStack_.push_back(topLandingBB);
        for (auto& s : prog.top_level)
            if (s->kind != StmtKind::JobDecl) genStmt(s.get());
        invokeStack_.pop_back();
        if (!builder_.GetInsertBlock()->getTerminator()) {
            releaseAllScopes();
            builder_.CreateRet(ConstantInt::get(i64Ty_, 0));
        }

        // Reached only via unwind (never normal control flow) when a
        // thrown FlyValue escapes every user `do`/`grabe` -- report it and
        // exit nonzero, matching §3.5 exactly (see
        // runtime/eh.cpp's fly_rt_report_uncaught).
        builder_.SetInsertPoint(topLandingBB);
        LandingPadInst* topLp = builder_.CreateLandingPad(lpTy_, 1, "toplevel.lp");
        topLp->addClause(ConstantPointerNull::get(cast<PointerType>(i8PtrTy_)));
        Value* topExcPtr = builder_.CreateExtractValue(topLp, {0});
        Value* topCaught = builder_.CreateCall(rt_["fly_rt_begin_catch"], {topExcPtr});
        Value* topErrVal = plainRuntimeCall(rt_["fly_rt_catch_extract"], {topCaught});
        builder_.CreateCall(rt_["fly_rt_end_catch"], {});
        plainRuntimeCall(rt_["fly_rt_report_uncaught"], {topErrVal}); // never returns (exit(1))
        builder_.CreateUnreachable();

        if (verifyModule(mod_, &errs())) {
            std::cerr << "internal error: generated LLVM module failed verification "
                         "(this is a fly-cc bug, not a Fly program error)\n";
            std::exit(1);
        }
    }

private:
    LLVMContext& ctx_;
    Module& mod_;
    IRBuilder<> builder_;
    std::string filename_;

    Type* i8Ty_; Type* i32Ty_; Type* i64Ty_; Type* dblTy_; Type* voidTy_; Type* i8PtrTy_;
    StructType* flyValueTy_;
    StructType* lpTy_; // landingpad result type (milestone 5, §3.5)

    std::unordered_map<std::string, FunctionCallee> rt_; // runtime helper functions
#ifdef _WIN32
    // Windows/MinGW-GCC runtime ABI, as OBSERVED on the compiled libflyrt:
    // `-mabi=ms` does NOT implement LLVM/Win64's register convention for
    // 16-byte {i64,i64} aggregates. A FlyValue result comes back through a
    // hidden out-pointer (occupying RCX, the first integer-arg slot) and
    // every FlyValue by-value parameter is passed as a pointer to a
    // caller-owned copy (each taking one integer-arg slot: RDX, R8, R9...).
    // No calling-convention attribute reconciles this with LLVM (verified:
    // even a Win64CC declaration of the runtime + a -mabi=ms build crashes,
    // and disassembly shows GCC reading `(%rdx)`/`(%r8)` param copies and
    // writing the FlyValue return via `mov %rax,(%rcx); mov %rdx,0x8(%rcx)`).
    // So each runtime function that touches a FlyValue is DECLARED with the
    // lowered memory-based signature, and this map records, per callee name,
    // whether it takes a hidden sret out-slot and which original by-value
    // params arrive by reference -- that is what loweredRuntimeCall()
    // consults to marshal call sites. Only names in this map get marshalled;
    // every other callee (LLVM-defined job bodies, scalar/pointer-only
    // externs) uses the ordinary by-value convention.
    struct RtSig {
        bool sretReturn = false;          // function returns via hidden out-slot prepended to args
        std::vector<unsigned char> kinds; // per original param: 0 = pass as-is, 1 = byref copy
    };
    std::unordered_map<std::string, RtSig> rtSig_;
#endif
    std::unordered_map<std::string, Function*> jobFuncs_; // user "job" -> flyjob_<n>

    // milestone 7, §5/§21: `native job` declarations. Keyed by the
    // Fly-visible name (e.g. "rt_file_exists"), bound straight to the
    // libflyrt C symbol "fly_" + name -- see declareNative(). Separate
    // from jobFuncs_ because these calls follow the runtime-builtin
    // BORROW convention (genRuntimeBuiltinCall-style: caller releases its
    // own argument temps after the call) rather than jobFuncs_'s
    // ownership-transfer convention -- see genNativeCall().
    std::unordered_map<std::string, FunctionCallee> nativeFuncs_;
    std::unordered_map<std::string, size_t> nativeArity_;

    Function* curFn_ = nullptr;
    std::vector<std::unordered_map<std::string, AllocaInst*>> scopes_;

    // ---- do/grabe (milestone 5, §3.5) ------------------------------------
    // Personality routine: libstdc++'s Itanium-ABI personality routine
    // (runtime/eh.cpp reuses the rest of that ABI too -- see its header
    // comment for why we don't hand-roll one). The symbol name differs by
    // platform: POSIX uses `__gxx_personality_v0`; Windows/MinGW SEH uses
    // `__gxx_personality_seh0`. Declared once per module, attached
    // (Function::setPersonalityFn) to any generated function that ends up
    // containing an invoke/landingpad pair -- see ensurePersonality().
    Function* personalityFn_ = nullptr;
    // Stack of "if a call made right now throws, unwind straight to this
    // landing pad" targets -- empty outside any `do` block. genDoGrabe
    // pushes/pops around do_body; emitCall() (the invoke-aware call
    // helper used for every runtime/job call that can throw) consults the
    // top of this stack. Nested do/grabe, and do blocks containing
    // if/while/nested-blocks, all fall out of this naturally: it's a plain
    // stack threaded through the existing recursive genStmt/genExpr walk,
    // the same way scopes_ already is.
    std::vector<BasicBlock*> invokeStack_;

    // Currently-open `for` loop iterators (milestone 6), innermost last.
    // Needed so an early `give` from inside a `for` body (or from inside
    // any construct nested within it) still frees every iterator it's
    // escaping through -- see releaseAllScopes() below and genFor's
    // comment. genFor pushes right after fly_rt_iter_new and pops right
    // after its own normal-exit fly_rt_iter_free at the loop's endBB; the
    // two are mutually exclusive at runtime (an early `give` never reaches
    // that endBB, and normal loop exhaustion never runs releaseAllScopes'
    // iterator cleanup), so there's no double-free either way.
    std::vector<Value*> iterStack_;

    // ---- setup ------------------------------------------------------
    void declareRuntime() {
        FunctionType* binOp = FunctionType::get(flyValueTy_, {flyValueTy_, flyValueTy_}, false);
        FunctionType* triOp = FunctionType::get(flyValueTy_, {flyValueTy_, flyValueTy_, flyValueTy_}, false);
        FunctionType* unOp  = FunctionType::get(flyValueTy_, {flyValueTy_}, false);
        FunctionType* nullaryVal = FunctionType::get(flyValueTy_, {}, false);
        FunctionType* showTy = FunctionType::get(voidTy_, {flyValueTy_}, false);
        FunctionType* releaseTy = FunctionType::get(voidTy_, {flyValueTy_}, false);
        FunctionType* throwTy = FunctionType::get(voidTy_, {i8PtrTy_}, false);
        FunctionType* textFromCstrTy = FunctionType::get(flyValueTy_, {i8PtrTy_}, false);
        FunctionType* textFromBytesTy = FunctionType::get(flyValueTy_, {i8PtrTy_, i64Ty_}, false);
        FunctionType* collNewTy = FunctionType::get(flyValueTy_, {i64Ty_}, false);
        FunctionType* collPushTy = FunctionType::get(voidTy_, {flyValueTy_, flyValueTy_}, false);
        FunctionType* boardPutTy = FunctionType::get(voidTy_, {flyValueTy_, flyValueTy_, flyValueTy_}, false);
        Type* strbuildPtrTy = i8PtrTy_; // opaque FlyStrBuild* -- codegen never looks inside it
        FunctionType* strbuildNewTy = FunctionType::get(strbuildPtrTy, {}, false);
        FunctionType* strbuildAppendLitTy = FunctionType::get(voidTy_, {strbuildPtrTy, i8PtrTy_, i64Ty_}, false);
        FunctionType* strbuildAppendValTy = FunctionType::get(voidTy_, {strbuildPtrTy, flyValueTy_}, false);
        FunctionType* strbuildFinishTy = FunctionType::get(flyValueTy_, {strbuildPtrTy}, false);

        for (auto name : {"add", "sub", "mul", "div", "mod", "eq", "neq", "lt", "gt", "le", "ge", "and", "or",
                           "attach", "seek", "has", "bind", "sever", "erase", "index", "board_get", "board_set",
                           "board_erase", "board_has"})
            rt_["fly_rt_" + std::string(name)] = declareRt("fly_rt_" + std::string(name), binOp);
        for (auto name : {"not", "neg", "count", "cut", "raise", "lower", "board_count", "retain"})
            rt_["fly_rt_" + std::string(name)] = declareRt("fly_rt_" + std::string(name), unOp);
        for (auto name : {"place", "slice"})
            rt_["fly_rt_" + std::string(name)] = declareRt("fly_rt_" + std::string(name), triOp);
        rt_["fly_rt_take"] = declareRt("fly_rt_take", nullaryVal);

        // Every remaining declaration below that touches a FlyValue is
        // routed through declareRt() (lowering it to the memory-based ABI
        // the MinGW-GCC runtime implements); the scalar/pointer-only
        // helpers -- fly_rt_throw, strbuild_new/append_lit,
        // begin_catch/end_catch, iter_has_next/iter_free, init_args --
        // stay plain mod_.getOrInsertFunction since their interface has no
        // struct to disagree about. See RtSig's comment for the full
        // rationale.

        rt_["fly_rt_to_text"] = declareRt("fly_rt_to_text", unOp);
        rt_["fly_rt_show"] = declareRt("fly_rt_show", showTy);
        rt_["fly_rt_release"] = declareRt("fly_rt_release", releaseTy);
        rt_["fly_rt_throw"] = mod_.getOrInsertFunction("fly_rt_throw", throwTy);
        rt_["fly_rt_text_from_cstr"] = declareRt("fly_rt_text_from_cstr", textFromCstrTy);
        rt_["fly_rt_text_from_bytes"] = declareRt("fly_rt_text_from_bytes", textFromBytesTy);
        rt_["fly_rt_coll_new"] = declareRt("fly_rt_coll_new", collNewTy);
        rt_["fly_rt_coll_push"] = declareRt("fly_rt_coll_push", collPushTy);
        rt_["fly_rt_board_new"] = declareRt("fly_rt_board_new", nullaryVal);
        rt_["fly_rt_board_put"] = declareRt("fly_rt_board_put", boardPutTy);
        rt_["fly_rt_strbuild_new"] = mod_.getOrInsertFunction("fly_rt_strbuild_new", strbuildNewTy);
        rt_["fly_rt_strbuild_append_lit"] = mod_.getOrInsertFunction("fly_rt_strbuild_append_lit", strbuildAppendLitTy);
        rt_["fly_rt_strbuild_append_value"] = declareRt("fly_rt_strbuild_append_value", strbuildAppendValTy);
        rt_["fly_rt_strbuild_finish"] = declareRt("fly_rt_strbuild_finish", strbuildFinishTy);

        // ---- do/grabe (milestone 5, §3.5) ---------------------------------
        FunctionType* beginCatchTy = FunctionType::get(i8PtrTy_, {i8PtrTy_}, false);
        FunctionType* endCatchTy = FunctionType::get(voidTy_, {}, false);
        FunctionType* catchExtractTy = FunctionType::get(flyValueTy_, {i8PtrTy_}, false);
        FunctionType* reportUncaughtTy = FunctionType::get(voidTy_, {flyValueTy_}, false);
        rt_["fly_rt_begin_catch"] = mod_.getOrInsertFunction("fly_rt_begin_catch", beginCatchTy);
        rt_["fly_rt_end_catch"] = mod_.getOrInsertFunction("fly_rt_end_catch", endCatchTy);
        rt_["fly_rt_catch_extract"] = declareRt("fly_rt_catch_extract", catchExtractTy);
        rt_["fly_rt_report_uncaught"] = declareRt("fly_rt_report_uncaught", reportUncaughtTy);

        // ---- casts (milestone 6, §23) --------------------------------------
        rt_["fly_rt_cast_num"] = declareRt("fly_rt_cast_num", unOp);
        rt_["fly_rt_cast_dec"] = declareRt("fly_rt_cast_dec", unOp);

        // ---- iteration (milestone 6, runtime/iter.c's FlyIterator) ---------
        // The iterator handle is an opaque native pointer (i8*), exactly
        // like FlyStrBuild* above -- codegen never looks inside it, just
        // threads it through new/has_next/next/free. fly_rt_iter_new takes
        // a FlyValue (the container) and returns a pointer, so it's declared
        // via declareRt for the byref parameter on Windows; iter_has_next
        // and iter_free only see the opaque handle and stay plain.
        FunctionType* iterNewTy = FunctionType::get(i8PtrTy_, {flyValueTy_}, false);
        FunctionType* iterHasNextTy = FunctionType::get(Type::getInt32Ty(ctx_), {i8PtrTy_}, false);
        FunctionType* iterNextTy = FunctionType::get(flyValueTy_, {i8PtrTy_}, false);
        FunctionType* iterFreeTy = FunctionType::get(voidTy_, {i8PtrTy_}, false);
        rt_["fly_rt_iter_new"] = declareRt("fly_rt_iter_new", iterNewTy);
        rt_["fly_rt_iter_has_next"] = mod_.getOrInsertFunction("fly_rt_iter_has_next", iterHasNextTy);
        rt_["fly_rt_iter_next"] = declareRt("fly_rt_iter_next", iterNextTy);
        rt_["fly_rt_iter_free"] = mod_.getOrInsertFunction("fly_rt_iter_free", iterFreeTy);

        // ---- path.* dotted-call builtins (milestone 7, §5.3) --------------
        // Compiler-known builtins, NOT native declarations (path.join is
        // variadic; there's no variadic native-declaration syntax) -- see
        // flyrt.h's §5.3 section comment. path.join itself is declared as
        // unOp here too: codegen's genCall marshals its variadic Fly-level
        // args into a single coll argument before calling it (see below).
        for (auto name : {"path_join", "path_basename", "path_dirname", "path_extension", "path_stem", "path_absolute"})
            rt_[std::string("fly_rt_") + name] = declareRt(std::string("fly_rt_") + name, unOp);
        rt_["fly_rt_path_separator"] = declareRt("fly_rt_path_separator", nullaryVal);

        // ---- process args (milestone 7, §5.1) ------------------------------
        // Called once from generated `main` (run(), below) to stash argc/
        // argv for fly_rt_args() -- NOT itself Fly-callable (no native
        // declaration for it, see flyrt.h).
        FunctionType* initArgsTy = FunctionType::get(voidTy_, {i32Ty_, PointerType::getUnqual(i8PtrTy_)}, false);
        rt_["fly_rt_init_args"] = mod_.getOrInsertFunction("fly_rt_init_args", initArgsTy);

        // ---- filesystem.*/process.*/environment.*/system.* dotted-call
        // builtins (milestone 7, §5.1/§5.2/§5.4/§5.5, §8-12) ----------------
        // Handled the SAME way path.* already is (compiler-known dotted-
        // call names bound straight to their libflyrt symbol, see the
        // `builtins` table in genCall below) rather than through a `bring`-
        // able Fly-level job wrapper. This deliberately sidesteps a real
        // problem a job-wrapper approach would hit: Fly's job namespace is
        // flat (docs/architecture.md §3.6, driver.cpp's ModuleMerger), so
        // `bring`ing filesystem/process/environment/system together and
        // giving each module its own plain-named wrapper job (e.g. both
        // filesystem and environment wanting a job called `exists`) would
        // collide. Routing every one of these straight to its `fly_rt_*`
        // symbol as a dotted builtin -- exactly like path.* -- avoids that
        // entirely, keeps the native-symbol mapping centralized in one
        // place (here + declareNative, per §21), and still lets
        // stdlib/{filesystem,process,environment,system}.fly exist purely
        // as `bring`-able documentation (see path.fly's precedent). All of
        // them take/return FlyValue, so they all go through declareRt().
        for (auto name : {"fly_rt_file_exists", "fly_rt_file_isfile", "fly_rt_file_isdir",
                           "fly_rt_file_read", "fly_rt_dir_create", "fly_rt_file_remove",
                           "fly_rt_dir_list", "fly_rt_chdir",
                           "fly_rt_environment_get", "fly_rt_environment_has"})
            rt_[name] = declareRt(name, unOp);
        for (auto name : {"fly_rt_file_write", "fly_rt_file_append", "fly_rt_environment_set",
                           "fly_rt_process_run", "fly_rt_process_spawn"})
            rt_[name] = declareRt(name, binOp);
        for (auto name : {"fly_rt_getcwd", "fly_rt_args", "fly_rt_os", "fly_rt_arch", "fly_rt_hostname"})
            rt_[name] = declareRt(name, nullaryVal);
        rt_["fly_rt_process_wait"] = declareRt("fly_rt_process_wait", unOp);
        rt_["fly_rt_process_exit"] = declareRt("fly_rt_process_exit", unOp);

        // ---- net.* dotted-call builtins (SLEEP/NET milestone, flyrt.h
        // §5.6) -- handled EXACTLY like filesystem.*/path.* above: `net`
        // is a built-in/runtime-backed module (NOT a `bring`-able Fly
        // source library -- see stdlib/net.fly's header comment), so every
        // net.* name is a compiler-known dotted-call builtin bound
        // straight to its libflyrt symbol, not a native declaration or a
        // Fly job. All FlyValue-bearing, so all via declareRt().
        for (auto name : {"fly_rt_net_resolve", "fly_rt_net_accept", "fly_rt_net_close"})
            rt_[name] = declareRt(name, unOp);
        for (auto name : {"fly_rt_net_connect", "fly_rt_net_connect_tls", "fly_rt_net_listen", "fly_rt_net_send", "fly_rt_net_receive"})
            rt_[name] = declareRt(name, binOp);

        // __gxx_personality_v0's actual C++ signature is variadic-looking
        // from an extern-"C" declaration's point of view (it's really
        // `(int, _Unwind_Action, uint64_t, _Unwind_Exception*,
        // _Unwind_Context*) -> _Unwind_Reason_Code`, but LLVM only ever
        // references a personality function by pointer/symbol -- it's
        // never actually called through this IR-level type, so `i32 (...)`
        // (an arbitrary, ABI-irrelevant declaration) is the conventional
        // choice clang itself uses for this symbol.
        FunctionType* persTy = FunctionType::get(Type::getInt32Ty(ctx_), true);
        // The personality symbol name is a platform/ABI difference, not a
        // naming choice: POSIX toolchains implement the generic Itanium-ABI
        // `__gxx_personality_v0`, while win64/MinGW's SEH-based unwinding
        // implements it as `__gxx_personality_seh0` (the only name the
        // Windows libstdc++/libgcc actually export) -- the exact same
        // difference g++ itself encodes in the modules it compiles.
        personalityFn_ = Function::Create(persTy, Function::ExternalLinkage,
#ifdef _WIN32
                                           "__gxx_personality_seh0", &mod_);
#else
                                            "__gxx_personality_v0", &mod_);
#endif
    }

    // Declares a runtime function against the calling convention the
    // compiled C runtime ACTUALLY implements, and returns the callee call
    // sites should use:
    //   - POSIX/SysV:  16-byte {i64,i64} is returned in rax:rdx and passed
    //                   by value in register pairs -- `declFn` is exactly
    //                   right, unchanged (this is what Linux has always
    //                   used; the declaration is byte-identical to the old
    //                   mod_.getOrInsertFunction call).
    //   - Windows/GCC: 16-byte aggregates go through memory (see RtSig's
    //                   comment) -- declFn's FlyValue result and FlyValue
    //                   by-value params are lowered to plain pointers, and
    //                   the resulting RtSig is recorded so every call site
    //                   can marshal to match (loweredRuntimeCall).
    // Only called for FlyValue-touching declarations; scalar/pointer-only
    // helpers (fly_rt_init_args, fly_rt_throw, strbuild_new/append_lit,
    // begin/end_catch, iter_has_next/iter_free, ...) intentionally stay
    // plain mod_.getOrInsertFunction -- with no struct involved there is
    // nothing for the two toolchains to disagree about.
    FunctionCallee declareRt(const std::string& name, FunctionType* declFn) {
#ifndef _WIN32
        return mod_.getOrInsertFunction(name, declFn);
#else
        RtSig sig;
        sig.sretReturn = declFn->getReturnType() == flyValueTy_;
        // Only a FlyValue result is lowered to `void` (moved into the
        // hidden out-slot); a pointer/scalar result (e.g. fly_rt_iter_new's
        // FlyIterator*) keeps its own return type -- that value comes back
        // in RAX on both toolchains.
        Type* retTy = sig.sretReturn ? static_cast<Type*>(voidTy_) : declFn->getReturnType();
        std::vector<Type*> params;
        params.reserve(declFn->getNumParams() + (sig.sretReturn ? 1u : 0u));
        if (sig.sretReturn) params.push_back(i8PtrTy_); // hidden out-slot (first integer arg slot = RCX)
        sig.kinds.reserve(declFn->getNumParams());
        for (unsigned i = 0; i < declFn->getNumParams(); i++) {
            if (declFn->getParamType(i) == flyValueTy_) {
                sig.kinds.push_back(1);             // byref: caller passes address of a copy
                params.push_back(i8PtrTy_);
            } else {
                sig.kinds.push_back(0);             // passed as-is (pointer/scalar)
                params.push_back(declFn->getParamType(i));
            }
        }
        FunctionCallee fc = mod_.getOrInsertFunction(name,
            FunctionType::get(retTy, params, declFn->isVarArg()));
        rtSig_[name] = std::move(sig);
        return fc;
#endif
    }

    // Attaches the shared personality function to curFn_ if it doesn't
    // have one yet. Must be called before emitting the first
    // invoke/landingpad in a given function (LLVM's verifier requires any
    // function containing an `invoke` to have a personality function set;
    // it's harmless -- just unused -- on functions that end up with none).
    void ensurePersonality() {
        if (!curFn_->hasPersonalityFn()) curFn_->setPersonalityFn(personalityFn_);
    }

    // Invoke-aware call helper: every runtime/job call that can throw
    // (i.e. everything except fly_rt_retain/fly_rt_release, which never
    // do) goes through this instead of a raw builder_.CreateCall, so that
    // calls made anywhere within a `do` block's dynamic/lexical extent
    // (including inside nested if/while/blocks -- see invokeStack_'s
    // comment) correctly unwind to the enclosing `grabe` instead of
    // propagating straight past it. On Windows this ALSO marshals the call
    // to the lowered MinGW-GCC ABI (see loweredRuntimeCall).
    Value* emitCall(FunctionCallee callee, ArrayRef<Value*> args) {
#ifdef _WIN32
        return loweredRuntimeCall(callee, args, /*mayThrow=*/true);
#else
        if (invokeStack_.empty()) return builder_.CreateCall(callee, args);
        BasicBlock* contBB = BasicBlock::Create(ctx_, "invoke.cont", curFn_);
        Value* result = builder_.CreateInvoke(callee, contBB, invokeStack_.back(), args);
        builder_.SetInsertPoint(contBB);
        return result;
#endif
    }

    // Runtime calls that provably never throw (retain/release/iter_next/
    // catch_extract/report_uncaught): plain CreateCall even when inside a
    // do-block's invoke extent (an invoke for a call that can't unwind is
    // pure overhead), and -- unlike emitCall -- legal inside a landing pad,
    // where invoke is forbidden. Same Windows marshalling as emitCall.
    Value* plainRuntimeCall(FunctionCallee callee, ArrayRef<Value*> args) {
#ifdef _WIN32
        return loweredRuntimeCall(callee, args, /*mayThrow=*/false);
#else
        return builder_.CreateCall(callee, args);
#endif
    }

#ifdef _WIN32
    // Allocas for the sret out-slots and byref argument copies. Placed --
    // via a temporary builder pointed at the function ENTRY block, not the
    // current insertion point -- so a runtime call inside a loop doesn't
    // grow the stack frame on every iteration.
    AllocaInst* hoistedAlloca(Type* ty, const std::string& hint) {
        BasicBlock& entry = curFn_->getEntryBlock();
        IRBuilder<> eb(&entry, entry.getFirstInsertionPt());
        return eb.CreateAlloca(ty, nullptr, hint);
    }

    // Shared Windows implementation behind emitCall()/plainRuntimeCall():
    // if `callee` was declared via declareRt (i.e. it's in rtSig_), re-lower
    // its args to the memory-based MinGW-GCC ABI --
    //   [sret-slot?, (byref copy | as-is argument)...]
    // then call/invoke, and for FlyValue results re-load the sret-slot as
    // the call's value. Callees NOT in rtSig_ (LLVM-defined job bodies,
    // scalar/pointer-only externs like fly_rt_throw/fly_rt_init_args) go
    // straight through the ordinary by-value path.
    Value* loweredRuntimeCall(FunctionCallee callee, ArrayRef<Value*> args,
                              bool mayThrow) {
        std::string name;
        if (auto* f = dyn_cast<Function>(callee.getCallee())) name = f->getName().str();
        auto sigIt = rtSig_.find(name);
        if (sigIt == rtSig_.end()) {
            if (!mayThrow || invokeStack_.empty()) return builder_.CreateCall(callee, args);
            BasicBlock* contBB = BasicBlock::Create(ctx_, "invoke.cont", curFn_);
            Value* r = builder_.CreateInvoke(callee, contBB, invokeStack_.back(), args);
            builder_.SetInsertPoint(contBB);
            return r;
        }
        const RtSig& sig = sigIt->second;
        std::vector<Value*> callArgs;
        callArgs.reserve(args.size() + 1);
        AllocaInst* sretSlot = nullptr;
        if (sig.sretReturn) {
            sretSlot = hoistedAlloca(flyValueTy_, "sret");
            callArgs.push_back(sretSlot);
        }
        for (unsigned i = 0; i < sig.kinds.size(); i++) {
            if (sig.kinds[i]) {
                AllocaInst* copy = hoistedAlloca(flyValueTy_, "byref");
                builder_.CreateStore(args[i], copy);
                callArgs.push_back(copy);
            } else {
                callArgs.push_back(args[i]);
            }
        }
        if (!mayThrow || invokeStack_.empty()) {
            Value* raw = builder_.CreateCall(callee, callArgs);
            return sretSlot ? builder_.CreateLoad(flyValueTy_, sretSlot) : raw;
        }
        BasicBlock* contBB = BasicBlock::Create(ctx_, "invoke.cont", curFn_);
        if (sretSlot) {
            builder_.CreateInvoke(callee, contBB, invokeStack_.back(), callArgs);
            builder_.SetInsertPoint(contBB);
            return builder_.CreateLoad(flyValueTy_, sretSlot);
        }
        Value* raw = builder_.CreateInvoke(callee, contBB, invokeStack_.back(), callArgs);
        builder_.SetInsertPoint(contBB);
        return raw;
    }
#endif

    [[noreturn]] void fatal(int line, const std::string& msg) {
        std::cerr << filename_ << ":" << line << ": codegen error: " << msg << "\n";
        std::exit(1);
    }

    // ---- value construction ------------------------------------------
    Value* buildFlyValue(int64_t tag, Value* payloadI64) {
        Value* undef = UndefValue::get(flyValueTy_);
        Value* withTag = builder_.CreateInsertValue(undef, ConstantInt::get(i64Ty_, tag), {0});
        return builder_.CreateInsertValue(withTag, payloadI64, {1});
    }
    Value* numConst(int64_t n) { return buildFlyValue(FLY_NUM, ConstantInt::get(i64Ty_, n, true)); }
    Value* decConst(double d) {
        Value* dbl = ConstantFP::get(dblTy_, d);
        Value* asI64 = builder_.CreateBitCast(dbl, i64Ty_);
        return buildFlyValue(FLY_DEC, asI64);
    }
    Value* ynConst(bool b) { return buildFlyValue(FLY_YN, ConstantInt::get(i64Ty_, b ? 1 : 0)); }
    Value* empConst() { return buildFlyValue(FLY_EMP, ConstantInt::get(i64Ty_, 0)); }

    // A tex literal is now a REAL heap object (runtime/text.c's FlyText),
    // not a raw C-string payload -- milestone 3's shortcut of bit-casting a
    // global string pointer straight into the payload doesn't survive
    // once fly_rt_release/fly_rt_show/etc. all expect a FlyText* with a
    // refcount header. So this is a runtime call (fly_rt_text_from_cstr),
    // returning a fresh +1-owned value per the ARC convention above. No
    // interning yet (architecture.md §2.2 flags that as a future
    // optimization for literals specifically) -- every evaluation of the
    // same source literal allocates its own FlyText.
    Value* textConst(const std::string& s) {
        Constant* strGlobal = builder_.CreateGlobalString(s, "flytext");
        return emitCall(rt_["fly_rt_text_from_cstr"], {strGlobal});
    }

    // fly_rt_release never throws (it's a plain refcount decrement/free),
    // so this stays a plain call regardless of invokeStack_ -- see
    // emitCall's comment for which calls DO need to go through it.
    void releaseTemp(Value* v) { plainRuntimeCall(rt_["fly_rt_release"], {v}); }

    // Extracts the yn payload as i1, with a runtime check that the tag
    // really is FLY_YN (spec doesn't define what `if <non-yn>` does, so we
    // fail loudly instead of guessing). Fully consumes (releases) `v`.
    Value* asCondition(Value* v, int line) {
        Value* tag = builder_.CreateExtractValue(v, {0});
        Value* isYn = builder_.CreateICmpEQ(tag, ConstantInt::get(i64Ty_, FLY_YN));
        BasicBlock* okBB = BasicBlock::Create(ctx_, "cond.ok", curFn_);
        BasicBlock* badBB = BasicBlock::Create(ctx_, "cond.bad", curFn_);
        builder_.CreateCondBr(isYn, okBB, badBB);

        builder_.SetInsertPoint(badBB);
        Constant* msg = builder_.CreateGlobalString(
            "condition (if/while) requires a yn value", "errmsg");
        emitCall(rt_["fly_rt_throw"], {msg});
        builder_.CreateUnreachable();

        builder_.SetInsertPoint(okBB);
        Value* payload = builder_.CreateExtractValue(v, {1});
        releaseTemp(v); // scalar yn -> no-op release, kept for hygiene/consistency
        return builder_.CreateTrunc(payload, Type::getInt1Ty(ctx_));
        (void)line;
    }

    // ---- scopes -------------------------------------------------------
    AllocaInst* findVar(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }
    AllocaInst* declareVar(const std::string& name) {
        // Allocas placed in the current insertion point rather than the
        // function entry block: simpler for this milestone, at the cost of
        // relying on `mem2reg`/`-O` to clean it up rather than being
        // SSA-friendly by construction. Fine for correctness; a later pass
        // can hoist to entry-block allocas as a real optimization.
        AllocaInst* a = builder_.CreateAlloca(flyValueTy_, nullptr, name);
        scopes_.back()[name] = a;
        return a;
    }

    // Releases every local in just the innermost scope frame (used when a
    // block/clause/loop-body finishes WITHOUT an early return -- see the
    // big ARC comment above). Does NOT pop the frame; caller does that.
    void releaseCurrentScope() {
        for (auto& kv : scopes_.back()) {
            Value* cur = builder_.CreateLoad(flyValueTy_, kv.second);
            releaseTemp(cur);
        }
    }
    // Releases every local across every active scope frame (used right
    // before any `ret`, since a return deep in nested blocks bypasses
    // those blocks' own normal-exit cleanup -- LLVM allows only one
    // terminator per basic block).
    void releaseAllScopes() {
        for (auto& frame : scopes_) {
            for (auto& kv : frame) {
                Value* cur = builder_.CreateLoad(flyValueTy_, kv.second);
                releaseTemp(cur);
            }
        }
        // milestone 6: also close every `for` iterator we're escaping
        // through on this early-return path (see iterStack_'s comment) --
        // this is the ordinary-control-flow analogue of the documented
        // ARC-on-unwind gap genDoGrabe already notes for exceptions.
        for (auto* it : iterStack_) builder_.CreateCall(rt_["fly_rt_iter_free"], {it});
    }

    // ---- jobs -----------------------------------------------------------
    // milestone 7, §5/§21: `native job name(params)`. Unlike declareJob,
    // there's no matching "defineNative" -- a native declaration never
    // gets a Fly-emitted function body, just an extern declaration bound
    // to libflyrt's "fly_" + name symbol (e.g. Fly-visible "rt_file_exists"
    // -> C symbol "fly_rt_file_exists", matching every native name's
    // "rt_"-prefixed spelling in docs/architecture.md §5's examples).
    void declareNative(Stmt* s) {
        std::vector<Type*> paramTys(s->params.size(), flyValueTy_);
        FunctionType* fnTy = FunctionType::get(flyValueTy_, paramTys, false);
        // Every native job is FlyValue-in/FlyValue-out, so like the builtin
        // runtime functions it's declared through declareRt (re-declaring it
        // to the lowered memory-based ABI on Windows; byte-identical to the
        // old by-value declaration on POSIX). rtSig_ is keyed by the callee
        // symbol ("fly_" + name), so genNativeCall's emitCall(nf->second, ...)
        // automatically marshals on Windows.
        nativeFuncs_[s->job_name] = declareRt("fly_" + s->job_name, fnTy);
        nativeArity_[s->job_name] = s->params.size();
    }

    void declareJob(Stmt* s) {
        std::vector<Type*> paramTys(s->params.size(), flyValueTy_);
        FunctionType* fnTy = FunctionType::get(flyValueTy_, paramTys, false);
        Function* fn = Function::Create(fnTy, Function::ExternalLinkage,
                                          "flyjob_" + s->job_name, mod_);
        // milestone 5, §3.5: a `job` may be called from inside a `do`
        // block (possibly several frames up, transitively), and may throw
        // from several frames of its OWN plain (non-invoke) calls before
        // that. Unwind tables (.eh_frame CFI) let the ABI's unwinder walk
        // back through this frame correctly regardless of whether this
        // specific function ever emits an invoke itself -- without this,
        // some targets only guarantee correct unwinding through frames
        // that contain an invoke/landingpad, which every plain `call`-only
        // job body would not.
        fn->setUWTableKind(UWTableKind::Async);
        jobFuncs_[s->job_name] = fn;
    }
    void defineJob(Stmt* s) {
        Function* fn = jobFuncs_[s->job_name];
        BasicBlock* entry = BasicBlock::Create(ctx_, "entry", fn);
        builder_.SetInsertPoint(entry);
        curFn_ = fn;
        scopes_.clear();
        scopes_.emplace_back();
        size_t i = 0;
        for (auto& arg : fn->args()) {
            // Arguments arrive as +1-owned temps from the caller (see the
            // ARC comment's Call case) -- stored directly, no extra retain,
            // ownership transfers straight into the parameter binding.
            AllocaInst* a = declareVar(s->params[i]);
            builder_.CreateStore(&arg, a);
            i++;
        }
        for (auto& st : s->job_body) genStmt(st.get());
        if (!builder_.GetInsertBlock()->getTerminator()) {
            releaseAllScopes();
            builder_.CreateRet(empConst()); // spec §17: falls off the end -> EMP
        }
    }

    // ---- statements -----------------------------------------------------
    void genStmt(Stmt* s) {
        switch (s->kind) {
            case StmtKind::VarDecl: {
                Value* v = genExpr(s->init.get()); // +1 owned
                AllocaInst* a = findVar(s->var_name);
                if (a) {
                    // Reassignment: release whatever this binding held
                    // before overwriting it (§2.3: "release ... when a
                    // coll/board slot is overwritten").
                    Value* old = builder_.CreateLoad(flyValueTy_, a);
                    releaseTemp(old);
                } else {
                    a = declareVar(s->var_name); // first assignment == declaration
                }
                builder_.CreateStore(v, a); // ownership transfers into the slot, no extra retain
                return;
            }
            case StmtKind::ExprStmt: {
                Value* v = genExpr(s->expr.get()); // +1 owned, discarded
                releaseTemp(v);
                return;
            }
            case StmtKind::If:
                genIf(s);
                return;
            case StmtKind::While:
                genWhile(s);
                return;
            case StmtKind::Give: {
                Value* v = s->give_value ? genExpr(s->give_value.get()) : empConst(); // +1 owned, this IS the return value
                releaseAllScopes(); // release every OTHER live local before jumping out (see ARC comment)
                builder_.CreateRet(v);
                return;
            }
            case StmtKind::Block:
                scopes_.emplace_back();
                for (auto& st : s->block_body) genStmt(st.get());
                if (!builder_.GetInsertBlock()->getTerminator()) releaseCurrentScope();
                scopes_.pop_back();
                return;
            case StmtKind::JobDecl:
                return; // handled in the two-pass job walk in run()
            case StmtKind::DoGrabe:
                genDoGrabe(s);
                return;
            case StmtKind::For:
                genFor(s);
                return;
            case StmtKind::Bring:
                // Already resolved and merged into the Program by
                // driver.cpp before Sema/CodeGen ever ran (see ast.h's
                // Bring comment) -- nothing left to lower here.
                return;
            case StmtKind::NativeJobDecl:
                return; // handled in run()'s pass 0 (declareNative)
        }
    }

    // spec §3.5, milestone 5: `do { do_body } grabe (grabe_var) { grabe_body }`.
    //
    // Real invoke/landingpad, not a simulation: every emitCall() made
    // while do_body is on invokeStack_ (directly, or from inside a nested
    // if/while/block/job-call within do_body) becomes an `invoke`
    // targeting landingBB below instead of a plain `call`. A thrown
    // FlyValue unwinds -- via the Itanium C++ ABI machinery runtime/eh.cpp
    // hooks into (libunwind underneath, matching architecture.md §3.5
    // exactly) -- straight to landingBB, where it's unpacked back into a
    // FlyValue and bound to grabe_var for grabe_body to use.
    //
    // Documented MVP limitation (see runtime/eh.cpp's header comment for
    // the matching note): this does NOT run ARC cleanup for tex/coll/board
    // locals that are live (in do_body's scope, or in the scope of any job
    // whose call is still on the stack when it throws) at the moment of
    // unwind -- those references leak rather than being released. Doing
    // that fully requires a cleanup landingpad at every such scope (the
    // mechanism a C++ compiler builds for automatic local destructors),
    // which is real, separate follow-up work, not implemented here. What
    // IS real here: the exception is genuinely thrown/caught via
    // invoke/landingpad, `grabe` genuinely runs with the right value, and
    // normal (non-throwing) execution of a `do` block has completely
    // ordinary ARC behavior (releaseCurrentScope() below covers it).
    void genDoGrabe(Stmt* s) {
        ensurePersonality();
        BasicBlock* landingBB = BasicBlock::Create(ctx_, "grabe.landing", curFn_);
        BasicBlock* mergeBB = BasicBlock::Create(ctx_, "dograbe.end", curFn_);

        invokeStack_.push_back(landingBB);
        scopes_.emplace_back();
        for (auto& st : s->do_body) genStmt(st.get());
        bool doTerminated = builder_.GetInsertBlock()->getTerminator() != nullptr;
        if (!doTerminated) releaseCurrentScope();
        scopes_.pop_back();
        invokeStack_.pop_back();
        if (!doTerminated) builder_.CreateBr(mergeBB);

        builder_.SetInsertPoint(landingBB);
        LandingPadInst* lp = builder_.CreateLandingPad(lpTy_, 1, "lp");
        lp->addClause(ConstantPointerNull::get(cast<PointerType>(i8PtrTy_))); // catch-all, see file header comment
        Value* excPtr = builder_.CreateExtractValue(lp, {0});
        // __cxa_begin_catch/__cxa_end_catch bracket the catch handler per
        // the Itanium ABI; neither of these can themselves throw, so
        // they're plain calls even though we're inside a landing pad.
        Value* caught = builder_.CreateCall(rt_["fly_rt_begin_catch"], {excPtr});
        Value* errVal = plainRuntimeCall(rt_["fly_rt_catch_extract"], {caught});
        builder_.CreateCall(rt_["fly_rt_end_catch"], {});

        scopes_.emplace_back();
        AllocaInst* errSlot = declareVar(s->grabe_var);
        builder_.CreateStore(errVal, errSlot); // ownership transfers straight into the binding, no extra retain (matches the ARC convention elsewhere)
        for (auto& st : s->grabe_body) genStmt(st.get());
        bool grabeTerminated = builder_.GetInsertBlock()->getTerminator() != nullptr;
        if (!grabeTerminated) releaseCurrentScope();
        scopes_.pop_back();
        if (!grabeTerminated) builder_.CreateBr(mergeBB);

        builder_.SetInsertPoint(mergeBB);
    }

    void genIf(Stmt* s) {
        BasicBlock* mergeBB = BasicBlock::Create(ctx_, "if.end", curFn_);
        for (size_t i = 0; i < s->clauses.size(); i++) {
            auto& c = s->clauses[i];
            bool isLast = (i + 1 == s->clauses.size());
            if (!c.cond) {
                // `ifnot` -- unconditional trailing block.
                scopes_.emplace_back();
                for (auto& st : c.body) genStmt(st.get());
                bool terminated = builder_.GetInsertBlock()->getTerminator() != nullptr;
                if (!terminated) releaseCurrentScope();
                scopes_.pop_back();
                if (!terminated) builder_.CreateBr(mergeBB);
                continue;
            }
            Value* condV = asCondition(genExpr(c.cond.get()), s->line);
            BasicBlock* thenBB = BasicBlock::Create(ctx_, "if.then", curFn_);
            BasicBlock* elseBB = isLast ? mergeBB : BasicBlock::Create(ctx_, "if.next", curFn_);
            builder_.CreateCondBr(condV, thenBB, elseBB);

            builder_.SetInsertPoint(thenBB);
            scopes_.emplace_back();
            for (auto& st : c.body) genStmt(st.get());
            bool terminated = builder_.GetInsertBlock()->getTerminator() != nullptr;
            if (!terminated) releaseCurrentScope();
            scopes_.pop_back();
            if (!terminated) builder_.CreateBr(mergeBB);

            if (!isLast) builder_.SetInsertPoint(elseBB);
        }
        builder_.SetInsertPoint(mergeBB);
    }

    void genWhile(Stmt* s) {
        BasicBlock* condBB = BasicBlock::Create(ctx_, "while.cond", curFn_);
        BasicBlock* bodyBB = BasicBlock::Create(ctx_, "while.body", curFn_);
        BasicBlock* endBB  = BasicBlock::Create(ctx_, "while.end", curFn_);

        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        Value* condV = asCondition(genExpr(s->while_cond.get()), s->line);
        builder_.CreateCondBr(condV, bodyBB, endBB);

        builder_.SetInsertPoint(bodyBB);
        scopes_.emplace_back();
        for (auto& st : s->while_body) genStmt(st.get());
        // Release this iteration's locals before looping back, so they
        // don't accumulate references across iterations (see ARC comment).
        bool terminated = builder_.GetInsertBlock()->getTerminator() != nullptr;
        if (!terminated) releaseCurrentScope();
        scopes_.pop_back();
        if (!terminated) builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
    }

    // milestone 6: `for for_var in for_iterable { for_body }`, lowered
    // against runtime/iter.c's FlyIterator protocol -- structurally the
    // same invoke-free while-loop shape as genWhile above, plus a fresh
    // per-iteration binding for for_var (mirrors how genDoGrabe binds
    // grabe_var: ownership transfers straight into the slot, no extra
    // retain, and releaseCurrentScope() at the bottom of the loop body
    // cleans it up like any other local each iteration).
    //
    // fly_rt_iter_new can throw (a non-coll/board/tex iterable), so it goes
    // through emitCall; has_next/next/free never throw (see flyrt.h), so
    // they're plain calls, matching fly_rt_retain/fly_rt_release's
    // precedent elsewhere in this file.
    void genFor(Stmt* s) {
        Value* container = genExpr(s->for_iterable.get()); // +1 owned
        Value* iter = emitCall(rt_["fly_rt_iter_new"], {container});
        releaseTemp(container); // fly_rt_iter_new retained its own copy inside the iterator
        iterStack_.push_back(iter); // see iterStack_'s comment: covers early-`give` cleanup

        BasicBlock* condBB = BasicBlock::Create(ctx_, "for.cond", curFn_);
        BasicBlock* bodyBB = BasicBlock::Create(ctx_, "for.body", curFn_);
        BasicBlock* endBB  = BasicBlock::Create(ctx_, "for.end", curFn_);

        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        Value* hasNext = builder_.CreateCall(rt_["fly_rt_iter_has_next"], {iter});
        Value* hasNextBit = builder_.CreateICmpNE(hasNext, ConstantInt::get(Type::getInt32Ty(ctx_), 0));
        builder_.CreateCondBr(hasNextBit, bodyBB, endBB);

        builder_.SetInsertPoint(bodyBB);
        scopes_.emplace_back();
        AllocaInst* loopVar = declareVar(s->for_var);
        Value* item = plainRuntimeCall(rt_["fly_rt_iter_next"], {iter}); // +1 owned
        builder_.CreateStore(item, loopVar); // ownership transfers into the fresh per-iteration binding, no extra retain
        for (auto& st : s->for_body) genStmt(st.get());
        bool terminated = builder_.GetInsertBlock()->getTerminator() != nullptr;
        if (!terminated) releaseCurrentScope(); // releases loopVar (and any other for_body locals) before looping back
        scopes_.pop_back();
        if (!terminated) builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        builder_.CreateCall(rt_["fly_rt_iter_free"], {iter}); // releases the container fly_rt_iter_new retained
        iterStack_.pop_back();
    }

    // ---- expressions ------------------------------------------------
    Value* genExpr(Expr* e) {
        switch (e->kind) {
            case ExprKind::NumLit:  return numConst(e->num_val);
            case ExprKind::DecLit:  return decConst(e->dec_val);
            case ExprKind::YnLit:   return ynConst(e->yn_val);
            case ExprKind::EmpLit:  return empConst();
            case ExprKind::TextLit: return textConst(e->text_val);
            case ExprKind::TextTemplate: return genTextTemplate(e);
            case ExprKind::CollLit: return genCollLit(e);
            case ExprKind::BoardLit: return genBoardLit(e);
            case ExprKind::Index: return genIndex(e);
            case ExprKind::Ident: {
                AllocaInst* a = findVar(e->text_val);
                if (!a) fatal(e->line, "undefined identifier '" + e->text_val + "' (should have been caught by Sema)");
                Value* loaded = builder_.CreateLoad(flyValueTy_, a);
                return plainRuntimeCall(rt_["fly_rt_retain"], {loaded}); // never throws -- plain call regardless of invokeStack_; "copy into a new binding" -> +1 owned temp
            }
            case ExprKind::Unary: {
                Value* v = genExpr(e->lhs.get());
                const char* fn = (e->unop == UnOp::Neg) ? "fly_rt_neg" : "fly_rt_not";
                Value* result = emitCall(rt_[fn], {v});
                releaseTemp(v);
                return result;
            }
            case ExprKind::Binary: {
                Value* l = genExpr(e->lhs.get());
                Value* r = genExpr(e->rhs.get());
                static const std::unordered_map<BinOp, const char*> table = {
                    {BinOp::Add, "fly_rt_add"}, {BinOp::Sub, "fly_rt_sub"}, {BinOp::Mul, "fly_rt_mul"},
                    {BinOp::Div, "fly_rt_div"}, {BinOp::Mod, "fly_rt_mod"},
                    {BinOp::Eq, "fly_rt_eq"}, {BinOp::Neq, "fly_rt_neq"},
                    {BinOp::Lt, "fly_rt_lt"}, {BinOp::Gt, "fly_rt_gt"},
                    {BinOp::Le, "fly_rt_le"}, {BinOp::Ge, "fly_rt_ge"},
                    {BinOp::And, "fly_rt_and"}, {BinOp::Or, "fly_rt_or"},
                };
                Value* result = emitCall(rt_[table.at(e->binop)], {l, r});
                releaseTemp(l);
                releaseTemp(r);
                return result;
            }
            case ExprKind::Call:
                return genCall(e);
        }
        fatal(e->line, "unhandled expression kind (internal error)");
    }

    // spec §11: `[e1, e2, ...]`. fly_rt_coll_new returns a fresh +1-owned
    // coll; fly_rt_coll_push internally retains each element it stores, so
    // the per-element temp from genExpr must be released right after.
    Value* genCollLit(Expr* e) {
        Value* coll = emitCall(rt_["fly_rt_coll_new"], {ConstantInt::get(i64Ty_, e->elements.size())});
        for (auto& el : e->elements) {
            Value* v = genExpr(el.get());
            emitCall(rt_["fly_rt_coll_push"], {coll, v});
            releaseTemp(v);
        }
        return coll;
    }

    // spec §12: `{k1: v1, ...}`. Same shape as genCollLit -- fly_rt_board_put
    // internally retains both key and value.
    Value* genBoardLit(Expr* e) {
        Value* board = emitCall(rt_["fly_rt_board_new"], {});
        for (size_t i = 0; i < e->board_keys.size(); i++) {
            Value* k = genExpr(e->board_keys[i].get());
            Value* v = genExpr(e->board_vals[i].get());
            emitCall(rt_["fly_rt_board_put"], {board, k, v});
            releaseTemp(k);
            releaseTemp(v);
        }
        return board;
    }

    // spec §10: string interpolation, lowered to the strbuild call
    // sequence from architecture.md §3.4: new -> (append_lit | append_value)*
    // -> finish. Literal chunks become global string constants (their
    // length is compile-time known, so no strlen() at runtime).
    Value* genTextTemplate(Expr* e) {
        Value* sb = emitCall(rt_["fly_rt_strbuild_new"], {});
        for (auto& part : e->template_parts) {
            if (part.is_expr) {
                Value* v = genExpr(part.expr.get());
                emitCall(rt_["fly_rt_strbuild_append_value"], {sb, v});
                releaseTemp(v);
            } else {
                Constant* g = builder_.CreateGlobalString(part.literal, "flytpl");
                emitCall(rt_["fly_rt_strbuild_append_lit"],
                    {sb, g, ConstantInt::get(i64Ty_, part.literal.size())});
            }
        }
        return emitCall(rt_["fly_rt_strbuild_finish"], {sb});
    }

    // spec §13/§14: `target[i]` / `target[a:b]` (either bound omittable).
    Value* genIndex(Expr* e) {
        Value* target = genExpr(e->index_target.get());
        Value* result;
        if (e->is_slice) {
            Value* from = e->slice_from ? genExpr(e->slice_from.get()) : empConst();
            Value* to = e->slice_to ? genExpr(e->slice_to.get()) : empConst();
            result = emitCall(rt_["fly_rt_slice"], {target, from, to});
            releaseTemp(from);
            releaseTemp(to);
        } else {
            // idx isn't always a scalar num -- board access (`board[key]`,
            // this milestone's choice for §12's spec-left-open access
            // syntax) allows any hashable key, including tex, so this temp
            // must be released same as any other builtin-call argument
            // (fly_rt_index/fly_rt_board_get only read it, never take
            // ownership of the caller's copy).
            Value* idx = genExpr(e->index_expr.get());
            result = emitCall(rt_["fly_rt_index"], {target, idx});
            releaseTemp(idx);
        }
        releaseTemp(target);
        return result;
    }

    // Generic "call one runtime builtin, release every argument temp
    // afterward" helper -- covers the §15 ops uniformly. `argc` is checked
    // against e->args.size() first for a clean diagnostic.
    Value* genRuntimeBuiltinCall(Expr* e, const char* rtName, size_t argc) {
        if (e->args.size() != argc)
            fatal(e->line, "'" + e->callee + "' takes exactly " + std::to_string(argc) +
                  " argument(s), got " + std::to_string(e->args.size()));
        std::vector<Value*> args;
        args.reserve(argc);
        for (auto& a : e->args) args.push_back(genExpr(a.get()));
        Value* result = emitCall(rt_[rtName], args);
        for (auto* v : args) releaseTemp(v);
        return result;
    }

    Value* genCall(Expr* e) {
        if (e->callee == "show") {
            if (e->args.size() != 1) fatal(e->line, "show() takes exactly one argument");
            Value* v = genExpr(e->args[0].get());
            emitCall(rt_["fly_rt_show"], {v});
            releaseTemp(v);
            return empConst();
        }
        if (e->callee == "take") {
            if (!e->args.empty()) fatal(e->line, "take() takes no arguments");
            return emitCall(rt_["fly_rt_take"], {});
        }

        // milestone 7, §5.3: `path.join(a, b, ...)`. Variadic, so it's
        // handled here rather than through the fixed-arity `builtins`
        // map/genRuntimeBuiltinCall below: codegen marshals however many
        // Fly-level args were given into a single coll, matching how a
        // CollLit literal is built (genCollLit), then calls the ONE-arg
        // fly_rt_path_join with that coll.
        if (e->callee == "path.join") {
            if (e->args.empty()) fatal(e->line, "path.join() requires at least one argument");
            Value* coll = emitCall(rt_["fly_rt_coll_new"], {ConstantInt::get(i64Ty_, e->args.size())});
            for (auto& a : e->args) {
                Value* v = genExpr(a.get());
                emitCall(rt_["fly_rt_coll_push"], {coll, v});
                releaseTemp(v);
            }
            Value* result = emitCall(rt_["fly_rt_path_join"], {coll});
            releaseTemp(coll);
            return result;
        }

        // §15 collection/text ops -- coll `erase`/`has`/`count`/`seek` also
        // work over board/tex where the spec says so; runtime/*.c does the
        // per-type dispatch (e.g. fly_rt_erase checks for FLY_BOARD and
        // forwards to fly_rt_board_erase), so codegen just needs to route
        // the right arity to the right entry point.
        //
        // The `path.*` entries (milestone 7, §5.3) are the non-variadic
        // half of the dotted-call builtins -- fixed arity, so they fit
        // this same table/genRuntimeBuiltinCall path as everything else
        // here (path.join above is the one exception).
        static const std::unordered_map<std::string, std::pair<const char*, size_t>> builtins = {
            {"attach", {"fly_rt_attach", 2}}, {"place", {"fly_rt_place", 3}},
            {"erase", {"fly_rt_erase", 2}}, {"count", {"fly_rt_count", 1}},
            {"seek", {"fly_rt_seek", 2}}, {"has", {"fly_rt_has", 2}},
            {"bind", {"fly_rt_bind", 2}}, {"sever", {"fly_rt_sever", 2}},
            {"cut", {"fly_rt_cut", 1}}, {"raise", {"fly_rt_raise", 1}}, {"lower", {"fly_rt_lower", 1}},
            {"path.basename", {"fly_rt_path_basename", 1}}, {"path.dirname", {"fly_rt_path_dirname", 1}},
            {"path.extension", {"fly_rt_path_extension", 1}}, {"path.stem", {"fly_rt_path_stem", 1}},
            {"path.absolute", {"fly_rt_path_absolute", 1}}, {"path.separator", {"fly_rt_path_separator", 0}},

            // filesystem.* (§5.2/§10)
            {"filesystem.exists", {"fly_rt_file_exists", 1}}, {"filesystem.isfile", {"fly_rt_file_isfile", 1}},
            {"filesystem.isdir", {"fly_rt_file_isdir", 1}}, {"filesystem.read", {"fly_rt_file_read", 1}},
            {"filesystem.write", {"fly_rt_file_write", 2}}, {"filesystem.append", {"fly_rt_file_append", 2}},
            {"filesystem.mkdir", {"fly_rt_dir_create", 1}}, {"filesystem.remove", {"fly_rt_file_remove", 1}},
            {"filesystem.list", {"fly_rt_dir_list", 1}}, {"filesystem.cwd", {"fly_rt_getcwd", 0}},
            {"filesystem.chdir", {"fly_rt_chdir", 1}},

            // process.* (§5.1/§11)
            {"process.args", {"fly_rt_args", 0}}, {"process.run", {"fly_rt_process_run", 2}},
            {"process.spawn", {"fly_rt_process_spawn", 2}}, {"process.wait", {"fly_rt_process_wait", 1}},
            {"process.exit", {"fly_rt_process_exit", 1}},

            // environment.* (§5.4/§9) -- `has` is a reserved keyword, so the
            // Fly-visible name is `exists`, mapped to the fly_rt_environment_has symbol.
            {"environment.get", {"fly_rt_environment_get", 1}}, {"environment.exists", {"fly_rt_environment_has", 1}},
            {"environment.set", {"fly_rt_environment_set", 2}},

            // system.* (§5.5/§12)
            {"system.os", {"fly_rt_os", 0}}, {"system.arch", {"fly_rt_arch", 0}},
            {"system.hostname", {"fly_rt_hostname", 0}},

            // net.* (SLEEP/NET milestone, flyrt.h §5.6)
            {"net.resolve", {"fly_rt_net_resolve", 1}}, {"net.connect", {"fly_rt_net_connect", 2}},
            {"net.connect_tls", {"fly_rt_net_connect_tls", 2}},
            {"net.listen", {"fly_rt_net_listen", 2}}, {"net.accept", {"fly_rt_net_accept", 1}},
            {"net.send", {"fly_rt_net_send", 2}}, {"net.receive", {"fly_rt_net_receive", 2}},
            {"net.close", {"fly_rt_net_close", 1}},
        };
        auto bIt = builtins.find(e->callee);
        if (bIt != builtins.end())
            return genRuntimeBuiltinCall(e, bIt->second.first, bIt->second.second);

        // milestone 6, §23: num()/dec()/tex() casts. tex() routes to the
        // existing fly_rt_to_text (already the §10.3 "any value -> its
        // textual representation" conversion, reused rather than
        // duplicated -- see runtime/cast.c's header comment); num()/dec()
        // route to their own dedicated runtime/cast.c entry points.
        static const std::unordered_map<std::string, const char*> casts = {
            {"num", "fly_rt_cast_num"}, {"dec", "fly_rt_cast_dec"}, {"tex", "fly_rt_to_text"},
        };
        auto cIt = casts.find(e->callee);
        if (cIt != casts.end())
            return genRuntimeBuiltinCall(e, cIt->second, 1);

        // milestone 7, §5/§21: a `native job` call. Same BORROW-and-release
        // argument convention as the builtins/casts above (genRuntimeBuiltinCall
        // captures it), just resolved against nativeFuncs_/nativeArity_
        // instead of a fixed compile-time name/arity, since those come from
        // whatever `native job` declarations happen to be in scope.
        auto nf = nativeFuncs_.find(e->callee);
        if (nf != nativeFuncs_.end()) {
            size_t argc = nativeArity_[e->callee];
            if (e->args.size() != argc)
                fatal(e->line, "'" + e->callee + "' takes exactly " + std::to_string(argc) +
                      " argument(s), got " + std::to_string(e->args.size()));
            std::vector<Value*> args;
            args.reserve(argc);
            for (auto& a : e->args) args.push_back(genExpr(a.get()));
            Value* result = emitCall(nf->second, args);
            for (auto* v : args) releaseTemp(v);
            return result;
        }

        auto jf = jobFuncs_.find(e->callee);
        if (jf == jobFuncs_.end()) {
            // milestone 7 §17: a dotted callee (contains '.') that didn't
            // match the `builtins` table above, a native job, or a plain
            // job is specifically an unrecognized dotted call (e.g. a typo
            // like `filesystem.raed(...)` or a module name Fly doesn't
            // know), not an ordinary undefined-job call -- give that its
            // own clearer, source-located diagnostic instead of lumping it
            // in with "call to undefined job".
            if (e->callee.find('.') != std::string::npos)
                fatal(e->line, "unknown dotted call '" + e->callee + "'");
            fatal(e->line, "call to undefined job '" + e->callee + "'");
        }
        std::vector<Value*> args;
        for (auto& a : e->args) args.push_back(genExpr(a.get())); // +1 owned each, transferred straight into the callee's params
        return emitCall(jf->second, args);
    }
};

} // namespace

std::unique_ptr<llvm::Module> CodeGen::generate(Program& prog, const std::string& moduleName) {
    // Deliberate leak: fly-cc is a single-shot CLI process, so we skip
    // LLVMContext teardown ordering entirely by just never freeing it.
    // A long-lived compiler (e.g. a language server) would need real
    // lifetime management here instead.
    auto* ctx = new LLVMContext();
    auto mod = std::make_unique<Module>(moduleName, *ctx);
    Emitter em(*ctx, *mod, moduleName);
    em.run(prog);
    return mod;
}

} // namespace flycc
