#pragma once
#include <string>

namespace flycc {

// Compiles a single .fly source file straight through to a native
// executable. This is a minimal stand-in for the real `fly -build` driver
// (docs/architecture.md §3.6 / §25 project-structure support): no
// fly.sleep manifest parsing, no bins/ output convention yet -- just
// `fly-cc input.fly -o output` for now. As of milestone 6, `inputPath`'s
// top-level `bring` statements ARE resolved (project-locally; see
// driver.cpp's ModuleMerger for the exact rules) and their modules' job
// declarations merged in before compilation, so multi-file `bring`-based
// programs work end to end even though there's still no full project/
// `flylink.sleep` tree.
//
// `iconPath` (optional) is a path to a Windows .ico file to embed into the
// produced executable as its icon resource (RT_GROUP_ICON). It is only
// consulted on Windows; on other platforms it is ignored so Linux builds
// proceed normally. Empty means "embed no icon". `fly` resolves/validates
// the effective icon (manifest `icon`, CLI `-icon`, or the toolchain
// default fly.ico) and passes the resolved path here; the compiler itself
// does NOT hard-code any default icon.
//
// Returns the process exit code (0 = success).
int compileFile(const std::string& inputPath, const std::string& outputPath, bool dumpAst, bool dumpIR, const std::string& iconPath = "");

} // namespace flycc
