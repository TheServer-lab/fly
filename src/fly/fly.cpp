// fly: the toolchain bootstrap/launcher (docs/architecture.md §7).
//
// fly.exe is NOT the compiler, the runtime, or the REPL -- it locates and
// dispatches to the real tools (fly-cc, fly-repl) that live alongside it,
// and implements the project-level commands (docs/architecture.md §7.2)
// that coordinate flylink.sleep, fly-cc, Dump and the project's module/
// directory. The project-command bodies live in project.cpp (manifest
// reading, -build/-run/-init/-test/-clean/-deps/-dump) and format.cpp
// (the official formatter); this file only parses the command line and
// forwards to them, keeping the command surface audit-able in one place.
#include "format.h"
#include "project.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <vector>

namespace {
namespace fs = std::filesystem;

// Prefers the copy sitting next to `fly` itself (the normal install/build
// layout -- see CMakeLists.txt), falling back to PATH.
[[noreturn]] void execTool(const std::string& tool, const std::vector<std::string>& args) {
    std::string path = fly::findSibling(tool);
#ifdef _WIN32
    // Windows has no process-replacing exec(); spawn the tool as a real
    // child and FORWARD its exit code by terminating with it, which is
    // observably the same thing every call site above relies on (the
    // tool's stdout/err stream to the console and its exit code becoming
    // fly's exit code).
    std::vector<std::string> argv;
    argv.push_back(path);
    for (auto& a : args) argv.push_back(a);
    int rc = fly::runWindowsProcess(argv);
    if (rc < 0) {
        std::cerr << "fly: could not locate or launch '" << tool
                  << "' (looked next to 'fly' and on PATH)\n";
        std::exit(1);
    }
    std::exit(rc);
#else
    std::vector<char*> cargs;
    cargs.push_back(const_cast<char*>(path.c_str()));
    for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);
    execvp(path.c_str(), cargs.data());
    // execvp only returns on failure.
    std::cerr << "fly: could not locate or launch '" << tool
              << "' (looked next to 'fly' and on PATH)\n";
    std::exit(1);
#endif
}

std::string requireProjectRoot() {
    std::string root = fly::findProjectRoot();
    if (root.empty()) {
        std::cerr << "fly: no flylink.sleep found in this directory or any parent "
                     "(looked up to 8 levels up) -- run 'fly -init <name>' to create a project\n";
        std::exit(1);
    }
    return root;
}

fly::Manifest requireManifest(const std::string& root, const std::string& cmd) {
    fly::Manifest m = fly::readManifest(root);
    if (!m.ok) {
        std::cerr << "fly: " << cmd << ": " << root << "/flylink.sleep: " << m.error << "\n";
        std::exit(1);
    }
    return m;
}

bool hasOfflineFlag(const std::vector<std::string>& rest) {
    for (auto& r : rest)
        if (r == "--offline") return true;
    return false;
}

void printHelp() {
    std::cout <<
        "Fly toolchain launcher\n"
        "\n"
        "Usage:\n"
        "  fly                     launch the interactive REPL (fly-repl)\n"
        "  fly -compile <file>     compile a single .fly file (via fly-cc); no project needed\n"
        "                             optional:  -icon icon.ico   embed a Windows icon resource\n"
        "                             (default: the toolchain fly.ico)\n"
        "  fly -format <file>      deterministically format a .fly file (official formatter)\n"
        "\n"
        "Project commands (run from inside a project, i.e. near its flylink.sleep):\n"
        "  fly -deps [--offline]   install the project's flylink.sleep 'deps' into module/\n"
        "  fly -build              build the project's 'source' into its 'output'\n"
        "  fly -run [args...]      build when needed, then run the project's output\n"
        "  fly -test               build and run the project's tests (tests/*.fly or a\n"
        "                          'test \"path.fly\"' line in flylink.sleep)\n"
        "  fly -clean              remove the project's build artifacts (never source/deps)\n"
        "  fly -init <name>        scaffold a new project directory with a flylink.sleep\n"
        "\n"
        "Dump package integration:\n"
        "  fly -dump install \"pkg\"   fetch a package into module/ (HTTPS from Dump,\n"
        "                            local mirror fallback)\n"
        "  fly -dump remove \"pkg\"    remove an installed package\n"
        "  fly -dump list            list installed packages\n"
        "  fly -dump update          re-fetch every installed package\n"
        "\n"
        "Options:\n"
        "  --offline    skip HTTPS download from Dump, use only the local mirror\n"
        "\n"
        "Other:\n"
        "  fly -help               show this message\n"
        "  fly -version            show toolchain version info\n"
        "\n"
        "flylink.sleep shape (docs/architecture.md \xC2\xA7\x39):\n"
        "  project\n"
        "    name \"my_project\"\n"
        "    version \"0.1.0\"\n"
        "    source \"src/main.fly\"\n"
        "    output \"bin/my_project\"\n"
        "    deps coll [ \"sleep\" ]\n"
        "    icon \"assets/fly.ico\"     optional: Windows icon resource (default: fly.ico)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) execTool("fly-repl", {});

    std::string cmd = argv[1];
    std::vector<std::string> rest(argv + 2, argv + argc);

    if (cmd == "-help" || cmd == "--help" || cmd == "-h") { printHelp(); return 0; }
    if (cmd == "-version" || cmd == "--version") {
        std::cout << "Fly toolchain 0.1.0 (fly-cc + fly-repl + libflyrt)\n";
        return 0;
    }
    if (cmd == "-compile") {
        if (rest.empty()) { std::cerr << "fly: -compile requires a source file\n"; return 1; }
        // `-icon <path>`: resolve the path to absolute (relative paths are
        // relative to the current working directory), verify the file
        // exists with a clear diagnostic when it does not, and forward the
        // resolved path to fly-cc (which embeds it on Windows, delves with
        // paths containing spaces via runWindowsProcess's quoting). Without
        // an explicit -icon, inject the toolchain default fly.ico so a
        // plain `fly -compile src/main.fly` matches `fly -build`'s
        // default-icon behavior -- but the compiler itself never hard-codes
        // an icon; it embeds exactly what it is told to.
        std::vector<std::string> toolArgs;
        bool iconGiven = false;
        for (size_t i = 0; i < rest.size(); i++) {
            if (rest[i] != "-icon") { toolArgs.push_back(rest[i]); continue; }
            if (i + 1 >= rest.size()) {
                std::cerr << "fly: -compile: -icon requires a path\n";
                return 1;
            }
            std::string icon = rest[++i];
            fs::path p(icon);
            if (!p.is_absolute()) {
                std::error_code ec;
                p = fs::current_path(ec) / p;
                if (ec) { std::cerr << "fly: -compile: could not resolve -icon path '" << icon << "'\n"; return 1; }
            }
            if (!fs::exists(p)) {
                std::cerr << "fly: -compile: -icon file not found: " << p.string() << "\n";
                return 1;
            }
            iconGiven = true;
            toolArgs.push_back("-icon");
            toolArgs.push_back(p.string());
        }
        if (!iconGiven) {
#ifdef FLY_DEFAULT_ICON
            toolArgs.push_back("-icon");
            toolArgs.push_back(FLY_DEFAULT_ICON);
#endif
        }
        execTool("fly-cc", toolArgs);
    }
    if (cmd == "-format") {
        if (rest.empty()) { std::cerr << "fly: -format requires a source file\n"; return 1; }
        return fly::formatFile(rest[0]);
    }
    if (cmd == "-deps") {
        std::string root = requireProjectRoot();
        fly::Manifest m = requireManifest(root, "-deps");
        return fly::cmdDeps(root, m, hasOfflineFlag(rest));
    }
    if (cmd == "-build") {
        std::string root = requireProjectRoot();
        fly::Manifest m = requireManifest(root, "-build");
        return fly::cmdBuild(root, m);
    }
    if (cmd == "-run") {
        std::string root = requireProjectRoot();
        fly::Manifest m = requireManifest(root, "-run");
        return fly::cmdRun(root, m, rest);
    }
    if (cmd == "-init") {
        if (rest.empty()) { std::cerr << "fly: -init requires a project name\n"; return 1; }
        return fly::cmdInit(rest[0]);
    }
    if (cmd == "-test") {
        std::string root = requireProjectRoot();
        fly::Manifest m = requireManifest(root, "-test");
        return fly::cmdTest(root, m);
    }
    if (cmd == "-clean") {
        std::string root = requireProjectRoot();
        fly::Manifest m = requireManifest(root, "-clean");
        return fly::cmdClean(root, m);
    }
    if (cmd == "-dump") {
        std::string root = requireProjectRoot();
        fly::Manifest m = requireManifest(root, "-dump");
        (void)m;
        return fly::cmdDump(root, rest);
    }

    std::cerr << "fly: '" << cmd << "' is not a fly command.\n"
                 "Try 'fly' (REPL), 'fly -help', or one of: -compile -format -build -run "
                 "-init -test -clean -deps -dump.\n";
    return 1;
}