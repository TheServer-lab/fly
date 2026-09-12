#pragma once
// fly project-level commands (docs/architecture.md §7.2/§9): the shared
// pieces of `fly -build`/-run/-deps/-dump/-init/-test/-clean. `fly` and
// `fly-cc` are deliberately separate binaries that share no library (see
// fly.cpp's header comment / docs/architecture.md §7), so each of these
// helpers is an independent re-implementation of the small, stable things
// both need (project-root discovery, manifest reading, sibling tool
// location) rather than a shared library -- matching the existing design.
#include <string>
#include <vector>

namespace fly {

// A parsed flylink.sleep project manifest (docs/architecture.md §9).
// Only the documented fields are read; unknown keys are ignored (this is a
// bootstrap reader for exactly the documented schema, not a competing
// implementation of SLEEP -- see fly.cpp's reader comment for the full
// rationale).
struct Manifest {
    bool ok = false;                 // true iff the file parsed cleanly
    std::string error;               // set when ok == false
    std::string name;
    std::string version;
    std::string source;              // entry source file, relative to root
    std::string output;              // output binary path, relative to root
    bool hasTest = false;
    std::string test;                // optional `test "path.fly"` entry file
    std::vector<std::string> deps;   // `deps coll [ "a" "b" ... ]` package names
    std::string icon;                // optional `icon "assets/app.ico"` (project-relative Windows icon; when omitted fly uses the toolchain default fly.ico)
};

// Walks up at most 8 levels from the current directory looking for a
// flylink.sleep manifest (the same bounded search fly-cc's driver does for
// module resolution). Returns the containing directory, or "".
std::string findProjectRoot();

// Reads and parses <root>/flylink.sleep. Returns Manifest with ok=false
// (and error set) if the file is missing or malformed.
Manifest readManifest(const std::string& root);

// Resolves a manifest value against the project root. Absolute paths pass
// through; relative paths are joined onto root.
std::string resolvePath(const std::string& root, const std::string& rel);

// Directories: prefers the copy of `name` sitting next to the `fly`
// executable (the normal install/build layout), falling back to PATH.
std::string findSibling(const std::string& name);

// Windows portability pass: there is no process-replacing exec() on
// Windows, so both the fly launcher's execTool (src/fly/fly.cpp) and
// cmdRun need a real "spawn and wait for the child" helper that builds a
// properly-quoted CreateProcessA command line. argv[0] is the program
// path/image name, argv[1..] its arguments. Returns the child's exit
// code, or -1 if the child could not be spawned.
#ifdef _WIN32
int runWindowsProcess(const std::vector<std::string>& argv);
#endif

// ---- command implementations (return the process exit code) -------------

int cmdDeps(const std::string& root, const Manifest& m, bool offline);
int cmdBuild(const std::string& root, const Manifest& m);
int cmdRun(const std::string& root, const Manifest& m,
           const std::vector<std::string>& args);
int cmdInit(const std::string& name);
int cmdTest(const std::string& root, const Manifest& m);
int cmdClean(const std::string& root, const Manifest& m);
int cmdDump(const std::string& root, const std::vector<std::string>& args);

} // namespace fly