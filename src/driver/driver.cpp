#include "flycc/driver.h"
#include "flycc/lexer.h"
#include "flycc/parser.h"
#include "flycc/ast.h"
#include "flycc/sema.h"
#include "flycc/codegen.h"

#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace flycc {

namespace fs = std::filesystem;

static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "fly-cc: cannot open '" << path << "'\n";
        std::exit(1);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string dirName(const std::string& path) {
    // Both separators: Windows absolute paths use '\' (a pure-backslash
    // path has no '/' at all, so split-on-'/'-only would degrade the
    // module search directory to "." and make every `bring` resolve
    // cwd-relatively -- failing for modules that live NEXT TO the source
    // file or in the project's src/, i.e. exactly the files compile_fly
    // harnesses (net_tls's bunded http.fly, fresh-project's module/)
    // feed it).
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

// ---- `bring` module resolution & merging (milestone 6, §3.6) --------------
//
// `bring name` is resolved and merged BEFORE Sema/CodeGen ever see the
// Program: this class walks every top-level `bring` (transitively, through
// whatever the brought modules themselves `bring`), resolves each module
// name to a real `.fly` file, and flattens every module's job declarations
// into one list that gets prepended to the entry file's top level. By the
// time Sema/CodeGen run, `bring` is just a no-op marker (see ast.h's Bring
// comment) and every brought job is an ordinary top-level JobDecl -- Fly's
// job namespace is already flat/unqualified (no `module.job()` syntax
// exists in the grammar), so this is a faithful, non-hacky way to give
// `bring` real, working semantics without inventing new call syntax.
//
// Resolution order, matching docs/architecture.md §3.6:
//   1. Project-local: next to the file doing the `bring`, or that file's
//      sibling `src/` directory.
//   2. Bundled Fly standard library: no stdlib `.fly` sources ship with
//      fly-cc yet, so this milestone adds an FLY_STDLIB_DIR environment
//      variable as a stand-in -- the resolution ORDER §3.6 specifies is
//      real, even though the specific "bundled" directory doesn't exist
//      yet. A real bundled stdlib is future work.
//   3. Packages declared in `flylink.sleep` / installed under `module/`:
//      `flylink.sleep` itself doesn't exist yet (see §21's open items,
//      already documented there) -- not implemented, and this stays a
//      documented, not silently-swallowed limitation (same style as the
//      rest of this codebase's MVP notes).
//
// A module file that gets `bring`-ed may ONLY contain job declarations
// (and its own `bring`s) at the top level -- there's no defined semantics
// yet for "running a module's top-level statements at import time", and
// rather than invent one, this simplification is enforced with a clear
// compile error. Only the file fly-cc is invoked on directly gets to have
// top-level executable statements (they become `main`'s body).
class ModuleMerger {
public:
    std::vector<StmtPtr> collectedJobs; // flattened job decls from every merged module, in first-seen order

    // qualified-call aliases (see ast.h's Program::module_aliases comment):
    // filled in as every `bring X` site is processed below.
    std::vector<std::pair<std::string, std::string>> moduleAliases;

    void process(Program& prog, const std::string& filePath) {
        markVisited(filePath);
        std::string dir = dirName(filePath);

        for (auto& s : prog.top_level)
            if (s->kind == StmtKind::JobDecl || s->kind == StmtKind::NativeJobDecl)
                registerJob(s->job_name, filePath, s->line, filePath,
                            s->kind == StmtKind::NativeJobDecl, (int)s->params.size());

        for (auto& s : prog.top_level) {
            if (s->kind != StmtKind::Bring) continue;

            std::vector<std::string> tried;
            std::string resolved = resolveModule(s->bring_module, dir, tried);
            if (resolved.empty()) {
                std::cerr << filePath << ":" << s->line << ": error: cannot resolve module '"
                          << s->bring_module << "' (tried: ";
                for (size_t i = 0; i < tried.size(); i++) {
                    if (i) std::cerr << ", ";
                    std::cerr << "'" << tried[i] << "'";
                }
                std::cerr << "). Set FLY_STDLIB_DIR to a directory of .fly modules for an ad hoc override, "
                             "or (for public packages like 'sleep') run 'fly -deps' in a project with a "
                             "flylink.sleep that lists it, which installs it under module/.\n";
                std::exit(1);
            }

            std::string canon = canonicalize(resolved);

            if (!alreadyVisited(resolved)) {
                std::string modSrc = readFile(resolved);
                Lexer modLexer(modSrc, resolved);
                auto modToks = modLexer.lexAll();
                Parser modParser(std::move(modToks), resolved);
                Program modProg = modParser.parseProgram();

                for (auto& ms : modProg.top_level) {
                    if (ms->kind != StmtKind::JobDecl && ms->kind != StmtKind::NativeJobDecl && ms->kind != StmtKind::Bring) {
                        std::cerr << resolved << ":" << ms->line << ": error: a module brought in via 'bring' "
                                     "may only contain job declarations, native declarations (and its own "
                                     "'bring's) at the top level -- only the file passed directly to fly-cc may "
                                     "have top-level executable statements.\n";
                        std::exit(1);
                    }
                }

                // SLEEP/NET follow-up: remember exactly which job/native
                // names THIS module (resolved) declares directly at its
                // own top level -- BEFORE recursing, since recursion
                // flattens nested `bring`s' jobs into modProg too and we
                // only want this module's own names for qualified-call
                // aliasing below (ast.h's Program::module_aliases).
                std::vector<std::string> directNames;
                for (auto& ms : modProg.top_level)
                    if (ms->kind == StmtKind::JobDecl || ms->kind == StmtKind::NativeJobDecl)
                        directNames.push_back(ms->job_name);
                moduleDirectJobs_[canon] = std::move(directNames);

                process(modProg, resolved); // recurse first: transitive deps land before this module's own jobs (cosmetic ordering only -- call resolution doesn't depend on it, see codegen.cpp's two-pass job walk)

                for (auto& ms : modProg.top_level) {
                    if (ms->kind != StmtKind::JobDecl && ms->kind != StmtKind::NativeJobDecl) continue;
                    bool keep = registerJob(ms->job_name, resolved, ms->line, resolved,
                                ms->kind == StmtKind::NativeJobDecl, (int)ms->params.size());
                    if (keep) collectedJobs.push_back(std::move(ms));
                }
            } // else: diamond `bring`, or a `bring` cycle -- module content already processed (or in progress), but THIS bring site's qualified aliases (below) still need registering.

            // Qualified-call aliasing (ast.h's Program::module_aliases):
            // `bring sleep` at THIS site means "sleep.<name>" should reach
            // every job sleep.fly declares directly, regardless of how
            // many other files also bring the same module (diamond-safe:
            // moduleDirectJobs_ is keyed by the module's own canonical
            // path, populated once, read here on every bring site).
            auto dj = moduleDirectJobs_.find(canon);
            if (dj != moduleDirectJobs_.end())
                for (auto& name : dj->second)
                    moduleAliases.push_back({s->bring_module + "." + name, name});
        }
    }

private:
    std::unordered_map<std::string, std::string> jobOwner_;      // job name -> owning file (main file or a module path)
    std::unordered_map<std::string, int> nativeArity_;           // native job name -> its declared arity (for the identical-redeclaration check in registerJob)
    std::unordered_map<std::string, bool> visited_;              // canonical path -> already processed (or in progress)
    std::unordered_map<std::string, std::vector<std::string>> moduleDirectJobs_; // canonical module path -> job/native names declared directly at that module's own top level

    static std::string canonicalize(const std::string& path) {
        std::error_code ec;
        fs::path c = fs::canonical(path, ec);
        return ec ? path : c.string();
    }
    void markVisited(const std::string& path) { visited_[canonicalize(path)] = true; }
    bool alreadyVisited(const std::string& path) const {
        auto it = visited_.find(canonicalize(path));
        return it != visited_.end() && it->second;
    }

    // Returns true if this declaration should be kept (pushed into the
    // flattened program); false for a recognized-safe duplicate native
    // redeclaration (see below) that must be DROPPED rather than kept --
    // otherwise the flattened program would still contain two AST nodes
    // for the same name and Sema's own (separate, single-file-oriented)
    // duplicate-declaration check would reject it anyway.
    bool registerJob(const std::string& name, const std::string& owner, int line, const std::string& srcFileForErr,
                      bool isNative = false, int arity = -1) {
        auto it = jobOwner_.find(name);
        if (it == jobOwner_.end()) {
            // First time this name has been seen anywhere -- register it.
            jobOwner_[name] = owner;
            if (isNative) nativeArity_[name] = arity;
            return true;
        }
        if (it->second == owner) {
            // The SAME file's own declaration being registered again (this
            // happens harmlessly: a module's own top-level pass inside its
            // recursive process() call registers its jobs for nested-bring
            // collision bookkeeping, and the caller that brought it in
            // registers them again right before adding them to
            // collectedJobs) -- not a collision, keep as before.
            return true;
        }
        // Different owners, same name: normally a real collision -- EXCEPT
        // two DIFFERENT modules independently declaring the identical
        // `native job foo(...)` (same name, same arity), which isn't a
        // real conflict at all: there's no body to disagree over, and
        // both resolve to the exact same libflyrt symbol (fly_<name>)
        // either way. This matters once more than one public module wants
        // to share a runtime primitive like `rt_typename`/`rt_throwv`
        // (runtime/value.c, runtime/eh.cpp) and each declares it locally
        // rather than one exporting it to the other -- Fly 0.1 has no
        // re-export/visibility mechanism for that yet, so redeclaring is
        // the only option, and it should just work. Caller must NOT keep
        // (push into collectedJobs) this second AST node -- only the
        // first owner's copy is kept, so Sema's own (separate) duplicate-
        // declaration check never sees two nodes for the same name.
        if (isNative) {
            auto nit = nativeArity_.find(name);
            if (nit != nativeArity_.end() && nit->second == arity) return false;
        }
        std::cerr << srcFileForErr << ":" << line << ": error: job '" << name
                  << "' is declared both in '" << it->second << "' and '" << owner
                  << "' -- Fly 0.1's job namespace is flat/unqualified, so brought-in job names must "
                     "not collide with each other or with the entry file's own jobs.\n";
        std::exit(1);
    }

    // Walks upward from `fromDir` looking for a `flylink.sleep` project
    // manifest (docs/architecture.md §9/§3.6 point 3), the same way most
    // package managers locate "the project root" -- returns the directory
    // containing it, or "" if none is found within a few levels (an
    // unbounded walk to the filesystem root would risk resolving a
    // completely unrelated ancestor `module/` directory; a handful of
    // levels comfortably covers any real project layout).
    static std::string findProjectRoot(const std::string& fromDir) {
        fs::path dir = fs::path(fromDir);
        std::error_code ec;
        dir = fs::absolute(dir, ec);
        if (ec) return "";
        for (int i = 0; i < 8; i++) {
            std::error_code ec2;
            if (fs::exists(dir / "flylink.sleep", ec2) && !ec2) return dir.string();
            if (!dir.has_parent_path() || dir == dir.root_path()) break;
            dir = dir.parent_path();
        }
        return "";
    }

    std::string resolveModule(const std::string& name, const std::string& fromDir, std::vector<std::string>& tried) {
        std::vector<std::string> candidates = {
            fromDir + "/" + name + ".fly",       // §3.6 point 1a: project-local, next to the bringing file
            fromDir + "/src/" + name + ".fly",   // §3.6 point 1b: project-local src/
        };
        // §3.6 point 2: bundled Fly standard library. Milestone 7 (§5) adds
        // the first real bundled modules (process/filesystem/environment/
        // system -- see repo-root stdlib/), built and installed alongside
        // fly-cc itself via the FLY_BUILTIN_STDLIB_DIR compile definition
        // (see CMakeLists.txt), so `bring process` etc. work out of the box
        // with no environment setup. FLY_STDLIB_DIR stays supported as an
        // explicit override/dev-time replacement for that bundled copy.
        //
        // Deliberately NOT extended to cover public modules like `sleep`
        // (SLEEP/NET follow-up milestone, brief §1-2): those are resolved
        // ONLY through point 3 below (the project's installed module/), by
        // design -- a public module is meant to come from the package
        // system, not be silently bundled with the compiler the way the
        // built-in-flavored filesystem/process/etc. doc-only .fly files
        // are (see stdlib/*.fly's header comments: those files exist only
        // so `bring` resolves at all, since their actual behavior is
        // compiler-known dotted-call builtins in codegen.cpp -- sleep.fly
        // is the opposite: real Fly logic, resolved like any other
        // installed dependency).
#ifdef FLY_BUILTIN_STDLIB_DIR
        candidates.push_back(std::string(FLY_BUILTIN_STDLIB_DIR) + "/" + name + ".fly");
#endif
        if (const char* stdlibDir = std::getenv("FLY_STDLIB_DIR"))
            candidates.push_back(std::string(stdlibDir) + "/" + name + ".fly");

        // §3.6 point 3 / milestone brief §7 "public module resolution":
        // packages declared in `flylink.sleep` and installed under the
        // project's `module/` (by `fly -deps`/`fly -dump install`, see
        // fly.cpp). Tried last -- deliberately LOWER priority than the
        // built-in stdlib fallback above, so a project can never
        // accidentally shadow e.g. `bring filesystem` with a same-named
        // installed package; this only matters for names that AREN'T one
        // of the compiler-known built-ins anyway (sleep, json, mathlib, ...).
        std::string root = findProjectRoot(fromDir);
        if (!root.empty()) candidates.push_back(root + "/module/" + name + ".fly");
        // Even with no flylink.sleep found (e.g. `fly -compile` on a
        // loose file, matching this milestone's other test fixtures),
        // still check for a sibling `module/` next to the file itself --
        // covers the common case of a single-file project with a
        // `module/` dir but no manifest yet.
        candidates.push_back(fromDir + "/module/" + name + ".fly");

        for (auto& c : candidates) {
            tried.push_back(c);
            std::error_code ec;
            if (fs::exists(c, ec) && !ec) return c;
        }
        return "";
    }
};

int compileFile(const std::string& inputPath, const std::string& outputPath, bool dumpAst, bool dumpIR, const std::string& iconPath) {
    std::string source = readFile(inputPath);

    Lexer lexer(source, inputPath);
    auto tokens = lexer.lexAll();

    Parser parser(std::move(tokens), inputPath);
    Program prog = parser.parseProgram();

    // milestone 6, §3.6: resolve & merge every `bring`-ed module's job
    // declarations into prog's top level BEFORE Sema/CodeGen run -- see
    // ModuleMerger's header comment.
    {
        ModuleMerger merger;
        merger.process(prog, inputPath);
        prog.top_level.insert(prog.top_level.begin(),
            std::make_move_iterator(merger.collectedJobs.begin()),
            std::make_move_iterator(merger.collectedJobs.end()));
        prog.module_aliases = std::move(merger.moduleAliases);
    }

    if (dumpAst) dumpProgram(prog);

    Sema sema;
    if (!sema.check(prog, inputPath)) {
        std::cerr << "fly-cc: compilation aborted due to the above error(s)\n";
        return 1;
    }

    // ---- target setup (must happen before codegen so IR uses correct ABI) ----
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    auto targetTripleStr = llvm::sys::getDefaultTargetTriple();
    llvm::Triple targetTriple(targetTripleStr);

    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(targetTripleStr, err);
    if (!target) {
        std::cerr << "fly-cc: " << err << "\n";
        return 1;
    }

    // Generate IR first (module created internally by CodeGen)
    CodeGen codegen;
    auto module = codegen.generate(prog, inputPath);

    // Now configure the module for the target (target triple + data layout)
    // This must happen AFTER IR generation but BEFORE the backend runs.
    module->setTargetTriple(targetTriple);
    llvm::TargetOptions opts;
    auto* machine = target->createTargetMachine(targetTriple, "generic", "", opts, llvm::Reloc::PIC_);
    module->setDataLayout(machine->createDataLayout());

    if (dumpIR) module->print(llvm::outs(), nullptr);

    std::string objPath = outputPath + ".o";
    std::error_code ec;
    llvm::raw_fd_ostream dest(objPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "fly-cc: could not open object output file: " << ec.message() << "\n";
        return 1;
    }
    llvm::legacy::PassManager pass;
    if (machine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        std::cerr << "fly-cc: target machine can't emit an object file\n";
        return 1;
    }
    pass.run(*module);
    dest.flush();

    // ---- link against libflyrt + libm/libc via the system compiler ----
    // A real `fly -build` (architecture.md §3.6) would drive `lld`/the
    // platform linker directly instead of shelling out to `cc`; shelling
    // out is the pragmatic MVP choice so we don't have to hand-roll linker
    // invocation flags for every platform right now.
    //
    // -lstdc++ (milestone 5, §3.5): `do`/`grabe` is built on the Itanium
    // C++ exception ABI (runtime/eh.cpp: __cxa_throw/__cxa_begin_catch/
    // __cxa_end_catch, the __gxx_personality_v0 personality routine, and
    // libunwind underneath) -- those symbols live in libstdc++, and since
    // we still link the final binary with `cc` (a C compiler driver) that
    // library isn't pulled in automatically the way `c++`/`clang++` would.
    // -lssl -lcrypto (TLS/HTTPS follow-up milestone, runtime/net.c): NET's
    // TLS primitives are built on OpenSSL's libssl/libcrypto -- those
    // symbols live outside libflyrt.a itself (a system library, not
    // something we vendor/statically link into libflyrt), so every final
    // Fly binary needs them here too, same reasoning as -lstdc++ above.
    // -lws2_32 (Windows portability pass): the Winsock calls in runtime/
    // net.c and runtime/system.c live in ws2_32.dll, not in libc, so on a
    // native Windows/MinGW build the final link needs it the same way it
    // needs -lstdc++/libm.
#ifndef FLYRT_LIB_PATH
#error "FLYRT_LIB_PATH must be defined by CMake (see CMakeLists.txt)"
#endif
    std::string linkCmd = "cc -o \"" + outputPath + "\" \"" + objPath + "\" \"" FLYRT_LIB_PATH "\" -lstdc++ -lm -lssl -lcrypto";
#ifdef _WIN32
    // -lws2_32 (Windows portability pass): the Winsock calls in runtime/
    // net.c and runtime/system.c live in ws2_32.dll -- see the comment
    // block above this line for the full -lstdc++/-lssl rationale that
    // applies here identically.
    linkCmd += " -lws2_32";
#endif
    // ---- optional Windows icon resource (the `-icon` CLI/project option) --
    // Only consulted on Windows; Linux builds ignore iconPath and the final
    // binary simply has no embedded icon. The icon is embedded as a PE
    // resource, NOT shipped as a sidecar file: we write a one-line .rc
    // (`1 ICON "<path>"`), let the MinGW cc driver compile it with windres
    // (gcc recognizes .rc inputs) and link the resulting object into the
    // executable. windres registers the RT_GROUP_ICON + RT_ICON resources
    // from the .ico, so Explorer/the taskbar display it. The generated .rc
    // lives next to the output object and is removed after the link.
#ifdef _WIN32
    std::string rcPath = outputPath + ".rc";
    if (!iconPath.empty()) {
        std::ifstream icoFile(iconPath, std::ios::binary);
        char magic[4] = {0, 0, 0, 0};
        if (!icoFile || !icoFile.read(magic, 4) || magic[0] != 0 || magic[1] != 0 ||
            magic[2] != 1 || magic[3] != 0) {
            std::cerr << "fly-cc: -icon '" << iconPath << "' is not a Windows .ico file"
                         " (expected the 4-byte ICO magic 00 00 01 00)\n";
            return 1;
        }
        std::string fwd = iconPath;
        for (auto& ch : fwd) if (ch == '\\') ch = '/';
        std::ofstream rc(rcPath);
        if (!rc) {
            std::cerr << "fly-cc: could not write icon resource script: " << rcPath << "\n";
            return 1;
        }
        rc << "1 ICON \"" << fwd << "\"\n";
        rc.close();
        linkCmd = "cc -o \"" + outputPath + "\" \"" + objPath + "\" \"" + rcPath + "\" \"" FLYRT_LIB_PATH "\" -lstdc++ -lm -lssl -lcrypto -lws2_32";
    }
#endif
    int rc = std::system(linkCmd.c_str());
    llvm::sys::fs::remove(objPath);
#ifdef _WIN32
    if (!iconPath.empty()) llvm::sys::fs::remove(rcPath);
#endif
    if (rc != 0) {
        std::cerr << "fly-cc: link step failed (command: " << linkCmd << ")\n";
        return 1;
    }

    return 0;
}

} // namespace flycc
