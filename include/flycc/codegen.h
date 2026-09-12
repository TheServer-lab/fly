#pragma once
#include "flycc/ast.h"
#include <llvm/IR/Module.h>
#include <memory>
#include <string>

namespace flycc {

// LLVM IR generation for the milestone-1..4 subset (docs/architecture.md
// §3.4): scalars, tex (incl. interpolation), coll/board, and ARC
// (retain/release) insertion.
//
// Calling convention used here (a deliberate MVP simplification of the full
// design): EVERY Fly operator/builtin lowers to a runtime call — there is
// no inlined-fast-path/tag-guard split yet (that's the §3.3-point-3 /
// §3.4.1 optimization, planned but not implemented). This keeps codegen
// small while still producing real, correctly-behaving native code: num
// overflow still throws (checked in the runtime helper instead of via an
// inlined llvm.sadd.with.overflow), dynamic re-typing of a variable across
// two assignments still works, etc. Swapping in the inline fast path later
// is additive, not a rewrite. See codegen.cpp's top-of-file comment for the
// ARC ownership convention this milestone introduces.
class CodeGen {
public:
    // Lowers `prog` to an LLVM module named after `moduleName`. Fatal
    // errors (e.g. use of a not-yet-implemented builtin) print a diagnostic
    // and exit — matching the Lexer/Parser's error-handling style for this
    // milestone.
    std::unique_ptr<llvm::Module> generate(Program& prog, const std::string& moduleName);
};

} // namespace flycc
