#pragma once
#include "flycc/ast.h"
#include <string>

namespace flycc {

// Sema v0 (docs/architecture.md §3.3): deliberately lightweight, NOT a type
// checker. Responsibilities in this milestone:
//   1. Scope resolution: every Ident must resolve to a binding.
//   2. `hard` enforcement: reassigning a hard binding is a compile-time error.
//   3. `job` call-site arity checks.
// Local type-hinting for the IRGen fast path (§3.3 point 3) and task-escape
// analysis (§9.2/§9.8) are later-milestone additions, not implemented here.
class Sema {
public:
    // Returns true if the program is well-formed; prints diagnostics and
    // returns false otherwise (driver decides whether to abort).
    bool check(Program& prog, const std::string& filename);
};

} // namespace flycc
