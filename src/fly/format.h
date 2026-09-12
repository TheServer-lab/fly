#pragma once
// fly -format: deterministic, semantics-preserving Fly source formatter
// (docs/architecture.md §7.2). See format.cpp for the design.
#include <string>

namespace fly {

// Formats the .fly file at `path` in place iff its normalized form differs.
// Returns the process exit code (0 success). The file must be syntactically
// valid Fly (the compiler's own parser is the gate); failing that, fly-cc's
// parse diagnostics are printed and the file is left untouched.
int formatFile(const std::string& path);

} // namespace fly