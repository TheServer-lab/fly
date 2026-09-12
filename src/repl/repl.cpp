// fly-repl: the interactive Fly REPL (docs/architecture.md §6.1, milestone
// 7 §5-§14).
//
// Architecture, in one paragraph: this executable is deliberately thin. It
// does NOT link Sema/CodeGen/LLVM and does NOT re-implement any part of
// the language. Per §6.1 ("The REPL may use `fly-cc` and `libflyrt`
// internally"), each REPL turn is compiled and run by shelling out to the
// real `fly-cc` binary and then executing the native binary it produces --
// exactly the same Lexer -> Parser -> Sema -> CodeGen -> link pipeline
// every other Fly program goes through, with zero semantic drift. The only
// thing this file does with the actual Lexer/Parser in-process is classify
// an already-known-to-compile chunk's top-level statements into
// "persist across turns" vs "run once" (see classifyChunk below) -- that
// is bookkeeping about which *source lines* to replay next turn, not a
// second implementation of the language.
//
// Session model (docs/architecture.md §6.1's "maintaining an interactive
// session", milestone doc §8): the REPL keeps a growing `prelude_` string
// of persisted declarations (VarDecl/JobDecl/NativeJobDecl/Bring). Every
// turn compiles and runs `prelude_ + <this turn's chunk>` as one ordinary
// Fly program -- so `hard` immutability, job arity checks, scoping, etc.
// are enforced by the real Sema on the real accumulated program every
// single turn, not approximated by the REPL. Only if that run succeeds
// (exit code 0) does the turn's own decl-shaped statements get appended to
// `prelude_` for next time; a failing turn leaves `prelude_` untouched, so
// one bad line can never corrupt the session (§9's "REPL process itself
// should remain alive after recoverable input errors").
//
// Milestone 9: expression echo. A turn that is exactly ONE top-level
// expression statement (not an explicit show(...) call) is compiled as
// `show(<chunk>)`, so a bare expression entered interactively displays its
// resulting value (1 + 1 -> 2, "hello" -> hello, 10 * 4 -> 40, Yes -> Yes,
// a bare variable reference -> its current value). This is NOT a REPL-side
// evaluator: the whole turn still goes through the real Lexer/Parser/Sema/
// CodeGen and the real fly_rt_show runtime -- see evaluateChunkShape below.
// Explicit show(expr) is deliberately left unwrapped (show returns EMP, so
// wrapping it would print "EMP" instead of the value).
//
// One subtlety the forked classifier children must respect (both
// classifyChunk and evaluateChunkShape): std::exit() from inside a forked
// child (Parser::error does exactly this) runs glibc's atexit/stdio
// cleanup, and closing that child's inherited copy of the stdin FILE can
// rewind the shared input file offset -- the parent's very next getline
// would silently re-read already-consumed input. Both children therefore
// re-home fd 0 to /dev/null before parsing, and always die via _exit(0).
#include "flycc/ast.h"
#include "flycc/lexer.h"
#include "flycc/parser.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <vector>

namespace fs = std::filesystem;
using namespace flycc;

namespace {

std::string g_flyccPath;
std::string g_sessionDir;

// Returns a path INSIDE the session directory with native (backslash)
// separators. A bare `g_sessionDir + "/name"` string concatenation would
// produce a mixed-separator "Temp\fly-repl-<pid>-<n>/name" path that
// cmd.exe's program/redirection parser (runCaptured's `cmd /C ... > file`
// transport) provably rejects with "The filename, directory name, or
// volume label syntax is incorrect." -- fly-cc and ofstream tolerate the
// mix, but cmd.exe does not. fs::path does the join in native format.
std::string sessPath(const std::string& name) {
    return (fs::path(g_sessionDir) / name).string();
}

std::string selfExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::string p(buf, n);
    auto pos = p.find_last_of("\\/");
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    std::string p(buf);
    auto pos = p.find_last_of('/');
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
#endif
}

#ifdef _WIN32
// Full path to this executable -- the Windows-transport classifier child
// (see classifyChunk / evaluateChunkShape / runClassifyChild below) spawns
// a copy of fly-repl itself.
std::string selfExePath() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    return std::string(buf, n);
}

bool runClassifyChild(const char* mode, const std::string& inFile, const std::string& outFile);
int classifyChildMain(const char* mode, const char* inFile, const char* outFile);
#endif

// Looks for fly-cc next to this binary first (the normal installed/build
// layout -- see CMakeLists.txt's add_dependencies(fly-repl fly-cc)), then
// falls back to letting the shell find it on $PATH. An explicit
// FLY_CC_PATH environment variable always wins, mainly for tests that want
// to point at a specific build's binary.
std::string findFlyCC() {
    if (const char* override = std::getenv("FLY_CC_PATH")) return override;
    std::string dir = selfExeDir();
    if (!dir.empty()) {
        std::string candidate = dir + "/fly-cc";
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) return candidate;
    }
    return "fly-cc";
}

std::string shellQuote(const std::string& s) {
#ifdef _WIN32
    // cmd.exe quotes with double quotes, not the single quotes sh uses.
    // The only strings quoted here are paths we constructed ourselves (the
    // session directory and the fly-cc path), so a simple wrap-around is
    // safe -- neither can contain an embedded quote.
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
#endif
}

std::string readFileAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeFileAll(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc | std::ios::binary);
    f << content;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Lightweight bracket/quote/comment balance scanner used ONLY to decide
// whether the REPL should show a continuation prompt (§10's "support
// multi-line input"). This is NOT a Fly lexer and never rejects anything
// -- the real Lexer/Parser (via fly-cc) always gets the final say once the
// user's input looks balanced.
bool inputLooksComplete(const std::string& text) {
    int depth = 0;
    bool inStr = false;
    bool inBlockComment = false;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (inBlockComment) {
            if (c == '$' && i + 1 < text.size() && text[i + 1] == '$') { inBlockComment = false; i++; }
            continue;
        }
        if (inStr) {
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '$') {
            if (i + 1 < text.size() && text[i + 1] == '$') { inBlockComment = true; i++; continue; }
            while (i < text.size() && text[i] != '\n') i++;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') depth--;
    }
    return depth <= 0 && !inStr && !inBlockComment;
}

struct RunResult {
    int exitCode = -1;
    std::string out, err;
};

RunResult runCaptured(const std::string& cmd) {
    std::string outPath = sessPath(".out");
    std::string errPath = sessPath(".err");
#ifdef _WIN32
    // Windows transport: spawn via CreateProcess with stdout/stderr
    // redirected to per-run capture files, NOT via std::system()/cmd.exe.
    // cmd.exe's `> "path"` redirection is provably unreliable here: the
    // documented `/C "..."` rule strips the first and last quote of a
    // command string, so a run command that STARTS with a quoted executable
    // path (as the session_exe run passes -- the whole command is
    // shellQuote(exePath)) has its trailing `"` ripped off and a bare
    // `2> "....err` is left behind, which cmd rejects with "The filename,
    // directory name, or volume label syntax is incorrect." Switching to
    // CreateProcess also removes cmd as a dependency, so its whole quoting
    // minefield (spaces, mixed separators, quote-stripping) never applies.
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE; // the std handles below store to real files, so they MUST be inheritable
    HANDLE outH = CreateFileA(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE errH = CreateFileA(errPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    RunResult r;
    if (outH == INVALID_HANDLE_VALUE || errH == INVALID_HANDLE_VALUE) {
        if (outH != INVALID_HANDLE_VALUE) CloseHandle(outH);
        if (errH != INVALID_HANDLE_VALUE) CloseHandle(errH);
        r.exitCode = 1;
        r.err = "runCaptured: could not open stdout/stderr capture files";
        return r;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outH;
    si.hStdError = errH;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');
    if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(outH);
        CloseHandle(errH);
        r.exitCode = 1;
        r.err = "runCaptured: CreateProcessA failed";
        return r;
    }
    CloseHandle(outH);
    CloseHandle(errH);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    r.exitCode = (int)code;
    r.out = readFileAll(outPath);
    r.err = readFileAll(errPath);
    return r;
#else
    std::string full = cmd + " > " + shellQuote(outPath) + " 2> " + shellQuote(errPath);
    int rc = std::system(full.c_str());
    RunResult r;
    r.out = readFileAll(outPath);
    r.err = readFileAll(errPath);
    r.exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
    return r;
#endif
}

struct DeclRange { int startLine, endLine; };

// Parses `chunk` (already proven to compile as part of `prelude_ + chunk`)
// purely to find the [startLine, endLine] source-line span of every
// top-level VarDecl/JobDecl/NativeJobDecl/Bring statement, so the caller
// can slice those exact lines back out of `chunk` and append them to the
// persisted prelude. Runs in a forked child so that if this ever DID hit a
// Lexer/Parser error (Lexer::error/Parser::error call std::exit(1) -- see
// their headers), only the child dies; the REPL itself is unaffected. This
// should never actually trigger in practice (see this file's top comment)
// -- it's defense in depth, not the primary error path.
bool classifyChunk(const std::string& chunk, std::vector<DeclRange>& out) {
#ifdef _WIN32
    // Windows has no fork(): the same child-isolation guarantee comes from
    // spawning a copy of THIS executable in a hidden --classify-ranges mode
    // (see main()). The chunk travels as a file (command lines are length-
    // limited on Windows and quoting arbitrary source is error-prone), and
    // the child writes the SAME "startLine endLine" payload (per line) the
    // POSIX path produces -- so a Lexer/Parser std::exit(1) inside the
    // child still cannot kill the REPL.
    std::string inFile = sessPath("classify-in.txt");
    std::string outFile = sessPath("classify-ranges.txt");
    writeFileAll(inFile, chunk);
    if (!runClassifyChild("--classify-ranges", inFile, outFile)) return false;
    std::string data = readFileAll(outFile);
    std::error_code ec;
    fs::remove(inFile, ec);
    fs::remove(outFile, ec);

    std::istringstream iss(data);
    int a, b;
    while (iss >> a >> b) out.push_back({a, b});
    return true;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }

    if (pid == 0) {
        // Same stdin re-homing as evaluateChunkShape's child: if the Lexer/
        // Parser ever std::exit(1)s in here, glibc's exit-time fclose(stdin)
        // would rewind the SHARED input file offset and silently re-read
        // input in the parent (fork+stdio footgun). Detach fd 0 first so
        // this classifier child's exit cannot disturb the session stream.
        close(pipefd[0]);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); if (nullfd != STDIN_FILENO) close(nullfd); }
        if (!std::freopen("/dev/null", "w", stderr)) {
            // Non-fatal: worst case a stray diagnostic reaches the
            // terminal from this short-lived classification child.
        }
        Lexer lexer(chunk, "<repl>");
        auto toks = lexer.lexAll();
        Parser parser(std::move(toks), "<repl>");
        Program prog = parser.parseProgram();

        std::string payload;
        for (size_t k = 0; k < prog.top_level.size(); k++) {
            Stmt* s = prog.top_level[k].get();
            bool persist = s->kind == StmtKind::VarDecl || s->kind == StmtKind::JobDecl ||
                           s->kind == StmtKind::NativeJobDecl || s->kind == StmtKind::Bring;
            if (!persist) continue;
            int endLine = (k + 1 < prog.top_level.size()) ? prog.top_level[k + 1]->line - 1 : INT_MAX;
            payload += std::to_string(s->line) + " " + std::to_string(endLine) + "\n";
        }
        ssize_t written = write(pipefd[1], payload.data(), payload.size());
        (void)written;
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    std::string data;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) data.append(buf, n);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;

    std::istringstream iss(data);
    int a, b;
    while (iss >> a >> b) out.push_back({a, b});
    return true;
#endif
}

// Milestone 9 expression echo: parse `chunk` in a forked child (same
// child-isolation rationale as classifyChunk -- Lexer/Parser errors call
// std::exit) to decide whether this turn is a SINGLE top-level expression
// statement whose root is not an explicit show(...) call. When it is, the
// caller compiles `show(<chunk>)` so the value is displayed; any other
// turn (a declaration, a show(...), multiple statements, a parse error)
// is compiled exactly as typed. echo=false on any shape that should not
// (or, for a parse failure, cannot) be echoed.
struct ChunkShape { bool parseOk = false; bool echo = false; };

bool evaluateChunkShape(const std::string& chunk, ChunkShape& out) {
#ifdef _WIN32
    // Windows transport of the same isolated-classification pattern as
    // classifyChunk above (see its _WIN32 branch for the rationale).
    std::string inFile = sessPath("classify-in.txt");
    std::string outFile = sessPath("classify-shape.txt");
    writeFileAll(inFile, chunk);
    if (!runClassifyChild("--classify-shape", inFile, outFile)) return false;
    std::string data = readFileAll(outFile);
    std::error_code ec;
    fs::remove(inFile, ec);
    fs::remove(outFile, ec);

    out.parseOk = true;
    out.echo = !data.empty() && data[0] == '1';
    return true;
#else
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }

    if (pid == 0) {
        // Detach this child from the parent's shared stdio BEFORE any
        // parsing: Parser::error() (and other Lexer/Parser internals) end
        // unloved input with std::exit(1), and glibc's exit-time cleanup on
        // the inherited FILE objects corrupts the parent's buffered stdin
        // (well-known fork+stdio footgun -- forked children must die with
        // _exit()). Re-homing fd 0 to /dev/null means the child's exit
        // cannot lseek/close/reset anything the parent is still reading.
        close(pipefd[0]);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); if (nullfd != STDIN_FILENO) close(nullfd); }
        if (!std::freopen("/dev/null", "w", stderr)) {
            // Non-fatal: worst case a stray diagnostic reaches the
            // terminal from this short-lived classification child.
        }
        Lexer lexer(chunk, "<repl>");
        auto toks = lexer.lexAll();
        Parser parser(std::move(toks), "<repl>");
        Program prog = parser.parseProgram();

        std::string payload = "0\n";
        if (prog.top_level.size() == 1 && prog.top_level[0]->kind == StmtKind::ExprStmt) {
            Expr* e = prog.top_level[0]->expr.get();
            bool isShow = e != nullptr && e->kind == ExprKind::Call && e->callee == "show";
            if (!isShow) payload = "1\n";
        }
        ssize_t written = write(pipefd[1], payload.data(), payload.size());
        (void)written;
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    std::string data;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) data.append(buf, n);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;

    out.parseOk = true;
    out.echo = !data.empty() && data[0] == '1';
    return true;
#endif
}

#ifdef _WIN32
// Spawns this same executable in one of its hidden --classify-* modes (see
// main()), feeding it `inFile` and expecting it to write its payload to
// `outFile`, then waits to completion and reports whether the child exited
// 0 (success). The child's stdio is attached to NUL so a stray
// Lexer/Parser diagnostic can't garble the REPL's prompt -- its results
// travel exclusively via `outFile`.
bool runClassifyChild(const char* mode, const std::string& inFile, const std::string& outFile) {
    std::string self = selfExePath();
    if (self.empty()) return false;
    std::string cmdline = "\"" + self + "\" " + mode + " \"" + inFile + "\" \"" + outFile + "\"";

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
    if (nul != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul;
        si.hStdOutput = nul;
        si.hStdError = nul;
    }

    std::vector<char> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return false;
    }
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Hidden child-entry mode used by the Windows transport of
// classifyChunk/evaluateChunkShape above (there is no fork(): the parent
// REPL spawns a copy of itself in one of these modes to get the same "a
// Lexer/Parser error cannot kill the REPL" isolation the POSIX path gets
// from a forked child). Reads the chunk to classify from `inFile`, runs
// exactly the same classification logic the POSIX children run, writes the
// SAME payload format to `outFile`, and returns 0. If Parser::error()
// fires it will std::exit(1) -- which is precisely why this runs in its
// own child process, never in the REPL itself.
int classifyChildMain(const char* mode, const char* inFile, const char* outFile) {
    std::string chunk = readFileAll(inFile);

    if (std::strcmp(mode, "--classify-shape") == 0) {
        Lexer lexer(chunk, "<repl>");
        auto toks = lexer.lexAll();
        Parser parser(std::move(toks), "<repl>");
        Program prog = parser.parseProgram();

        std::string payload = "0\n";
        if (prog.top_level.size() == 1 && prog.top_level[0]->kind == StmtKind::ExprStmt) {
            Expr* e = prog.top_level[0]->expr.get();
            bool isShow = e != nullptr && e->kind == ExprKind::Call && e->callee == "show";
            if (!isShow) payload = "1\n";
        }
        writeFileAll(outFile, payload);
        return 0;
    }

    if (std::strcmp(mode, "--classify-ranges") == 0) {
        Lexer lexer(chunk, "<repl>");
        auto toks = lexer.lexAll();
        Parser parser(std::move(toks), "<repl>");
        Program prog = parser.parseProgram();

        std::string payload;
        for (size_t k = 0; k < prog.top_level.size(); k++) {
            Stmt* s = prog.top_level[k].get();
            bool persist = s->kind == StmtKind::VarDecl || s->kind == StmtKind::JobDecl ||
                           s->kind == StmtKind::NativeJobDecl || s->kind == StmtKind::Bring;
            if (!persist) continue;
            int endLine = (k + 1 < prog.top_level.size()) ? prog.top_level[k + 1]->line - 1 : INT_MAX;
            payload += std::to_string(s->line) + " " + std::to_string(endLine) + "\n";
        }
        writeFileAll(outFile, payload);
        return 0;
    }

    return 2; // unknown mode -- never reached by the callers above
}
#endif

void printBanner() {
    std::cout << "Fly REPL 0.1.0\n";
    std::cout << "Type :help for help, :quit to exit.\n\n";
}

void printHelp() {
    std::cout <<
        "Fly REPL commands:\n"
        "  :help          show this message\n"
        "  :quit, :exit   leave the REPL\n"
        "\n"
        "Anything else is compiled and run as Fly code. Variables, hard\n"
        "constants, and job/native job declarations persist between\n"
        "commands. A bare expression's value is displayed; explicit\n"
        "show(...) and control-flow statements run once and are not\n"
        "replayed on later turns.\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Hidden child-entry used only by this binary's own Windows-transport
    // classifier (classifyChunk/evaluateChunkShape): fly-repl --classify-*
    // <inFile> <outFile> runs the isolated classification and returns 0.
    // No other caller has any reason to use these modes -- they are the
    // fork() replacement, not a CLI feature.
    if (argc == 4 &&
        (std::strcmp(argv[1], "--classify-ranges") == 0 ||
         std::strcmp(argv[1], "--classify-shape") == 0))
        return classifyChildMain(argv[1], argv[2], argv[3]);
#endif
    (void)argc; (void)argv;

    g_flyccPath = findFlyCC();

#ifdef _WIN32
    // mkdtemp() is POSIX-only. Build a unique session dir under %TEMP%
    // from the PID + a monotonically increasing counter, retrying past any
    // (practically impossible) collision. Must be joined with fs::path, NOT
    // string concatenation: temp_directory_path() returns a trailing
    // separator here (e.g. "C:\Users\...\Temp\"), so a "/" concatenation
    // would produce an invalid mixed "Temp\/fly-repl-..." path that cmd.exe
    // PROVABLY rejects in `> "path"` redirection ("The filename, directory
    // name, or volume label syntax is incorrect.") -- which is exactly the
    // REPL's runCaptured() transport. project.cpp shares this helper.
    for (int attempt = 0; attempt < 100; attempt++) {
        std::error_code ec;
        fs::path dir = fs::temp_directory_path();
        dir /= "fly-repl-" + std::to_string((long long)GetCurrentProcessId()) + "-" + std::to_string(attempt);
        if (fs::create_directories(dir, ec) && !ec) { g_sessionDir = dir.string(); break; }
    }
    if (g_sessionDir.empty()) {
        std::cerr << "fly-repl: could not create a temp session directory\n";
        return 1;
    }
#else
    std::string tmplPath = fs::temp_directory_path().string() + "/fly-repl-XXXXXX";
    std::vector<char> tmpl(tmplPath.begin(), tmplPath.end());
    tmpl.push_back('\0');
    char* d = mkdtemp(tmpl.data());
    if (!d) {
        std::cerr << "fly-repl: could not create a temp session directory\n";
        return 1;
    }
    g_sessionDir = d;
#endif

    printBanner();

    std::string prelude; // persisted decls, replayed at the top of every turn
    std::string chunk;
    bool continuing = false;
    int turnNo = 0; // one exe per turn; see the turn code for why it must never be reused

    for (;;) {
        std::cout << (continuing ? "... " : ">>> ");
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break; // EOF (Ctrl-D)
        }

        if (!continuing) chunk.clear();
        else chunk += "\n";
        chunk += line;

        if (!inputLooksComplete(chunk)) { continuing = true; continue; }
        continuing = false;

        std::string trimmed = trim(chunk);
        if (trimmed.empty()) continue;
        if (trimmed == ":help") { printHelp(); continue; }
        if (trimmed == ":quit" || trimmed == ":exit") { std::cout << "Goodbye!\n"; break; }

        // Milestone 9 expression echo: a turn that is exactly one top-level
        // expression statement (and not an explicit show(...)) is compiled as
        // `show(<chunk>)`, so its value is displayed via the real compiler
        // and runtime. Everything else compiles exactly as typed.
        std::string candidate;
        {
            ChunkShape shape;
            bool ok = evaluateChunkShape(chunk, shape);
            if (ok && shape.echo)
                candidate = prelude + "show(" + chunk + ")\n";
            else
                candidate = prelude + chunk + "\n";
        }

        std::string srcPath = sessPath("session.fly");
        // Unique exe per turn: Windows keeps the IMAGE section of a
        // just-executed binary mapped (section-object cache / AV short
        // locks), so an immediate relink OVER the same path deterministically
        // fails with "ld: cannot open output file ... Permission denied".
        // Linking to a fresh name every turn sidesteps that on both OSes and
        // costs nothing (the whole session dir is removed at exit).
        std::string exePath = sessPath("session_exe_" + std::to_string(turnNo));
#ifdef _WIN32
        exePath += ".exe"; // Windows naming convention for the linked image produced below
#endif
        turnNo++;
        writeFileAll(srcPath, candidate);

        RunResult comp = runCaptured(g_flyccPath + " " + shellQuote(srcPath) + " -o " + shellQuote(exePath));
        if (comp.exitCode != 0) {
            std::string e = trim(comp.err.empty() ? comp.out : comp.err);
            std::cout << (e.empty() ? "Syntax error: compilation failed." : e) << "\n";
            continue; // §9: stay alive, prelude untouched
        }

        RunResult run = runCaptured(shellQuote(exePath));
        if (!run.out.empty()) std::cout << run.out;
        if (run.exitCode != 0) {
            std::string e = trim(run.err);
            std::cout << (e.empty() ? ("Fly error: process exited with code " + std::to_string(run.exitCode))
                                     : e)
                      << "\n";
            continue; // §9: stay alive, prelude untouched (failed turn is never persisted)
        }

        std::vector<DeclRange> ranges;
        if (classifyChunk(chunk, ranges)) {
            std::vector<std::string> lines = splitLines(chunk);
            for (auto& r : ranges) {
                int startLine = std::max(1, r.startLine);
                int endLine = std::min((int)lines.size(), r.endLine);
                if (endLine < startLine) endLine = startLine;
                for (int ln = startLine; ln <= endLine; ln++) {
                    prelude += lines[ln - 1];
                    prelude += "\n";
                }
            }
        }
    }

    std::error_code ec;
    fs::remove_all(g_sessionDir, ec);
    return 0;
}
