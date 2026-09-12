#include "flycc/driver.h"
#include <iostream>
#include <string>

// Minimal CLI for this milestone: `fly-cc input.fly [-o output] [--dump-ast] [--dump-ir] [-icon icon.ico]`.
// This is NOT the `fly` toolchain command itself (see docs/architecture.md
// §26-33 for `fly -build`/`-run`/`-deps`, `flyup`, `unfly`) -- fly-cc is
// the compiler binary those commands will eventually shell out to.
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: fly-cc <input.fly> [-o output] [--dump-ast] [--dump-ir] [-icon icon.ico]\n";
        return 1;
    }
    std::string input, output = "a.out", iconPath;
    bool dumpAst = false, dumpIR = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "-icon" && i + 1 < argc) iconPath = argv[++i];
        else if (a == "--dump-ast") dumpAst = true;
        else if (a == "--dump-ir") dumpIR = true;
        else if (!a.empty() && a[0] != '-') input = a;
        else { std::cerr << "fly-cc: unrecognized argument '" << a << "'\n"; return 1; }
    }
    if (input.empty()) { std::cerr << "fly-cc: no input file\n"; return 1; }
    return flycc::compileFile(input, output, dumpAst, dumpIR, iconPath);
}
