// fly project-level commands (docs/architecture.md §7.2/§9). Implements the
// toolchain layer that coordinates flylink.sleep, fly-cc, Dump, and the
// project's module/ directory. See project.h for the interface.
#include "project.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace fly {
namespace {

// Wait status from std::system() -> a clean exit code (split by the shell).
// On Windows, std::system() already returns the child's exit code directly
// (no WIFEXITED/WEXITSTATUS encoding), so it passes through unchanged.
int sysExitCode(int raw) {
    if (raw == -1) return 1;
#ifdef _WIN32
    return raw;
#else
    if (WIFEXITED(raw)) return WEXITSTATUS(raw);
    return 1;
#endif
}

// Runs an argv invocation and returns the child's exit code (>=0) or 1 on
// spawn failure -- the toolchain's portable subprocess route. POSIX goes
// through the shell with shellQuote()-escaped args; Windows CANNOT use
// std::system() here because cmd.exe /C "-quoted-command" strips the FIRST
// and LAST quote of the command string, so a command that STARTS with a
// quoted executable path (`fly -build`/`-test` always do -- fly-cc lives
// in an arbitrarily-located sibling dir) is left as `C:\...exe" ".." ...`
// minus its final quote, which cmd rejects with "The filename, directory
// name, or volume label syntax is incorrect.". runWindowsProcess() builds
// a properly backslash-escaped CreateProcess command line instead.
std::string shellQuote(const std::string& s); // defined below; declared here for runTool's POSIX branch
int runTool(const std::vector<std::string>& argv) {
#ifdef _WIN32
    int rc = runWindowsProcess(argv);
    return rc < 0 ? 1 : rc;
#else
    std::string cmd;
    for (size_t i = 0; i < argv.size(); i++) {
        if (i) cmd += ' ';
        cmd += shellQuote(argv[i]);
    }
    return sysExitCode(std::system(cmd.c_str()));
#endif
}

std::string selfExeDir() {
#ifdef _WIN32
    // GetModuleFileNameA is the Windows analog of readlink("/proc/self/exe").
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

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

std::string readAll(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeAll(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    if (!f) return false;
    f << content;
    return f.good();
}

// The manifest tokenizer (shared by the reader and -- via the byte offsets
// on each token -- the deps-list writer that splices flylink.sleep in
// place, preserving every other byte of the file). `start` is the byte
// offset of the token's first character in the original text; comments are
// dropped, so `[`/`]` offsets still line up with the raw file.
struct MTok { std::string s; bool quoted = false; size_t start = 0; };
bool manifestTokenize(const std::string& all, std::vector<MTok>& toks, std::string& error) {
    size_t i = 0;
    const size_t n = all.size();
    while (i < n) {
        char c = all[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
        if (c == '$') {
            if (i + 1 < n && all[i + 1] == '$') {
                i += 2;
                while (i + 1 < n && !(all[i] == '$' && all[i + 1] == '$')) i++;
                i = std::min(i + 2, n);
            } else {
                while (i < n && all[i] != '\n') i++;
            }
            continue;
        }
        if (c == '"') {
            size_t start = i;
            i++;
            std::string s;
            while (i < n && all[i] != '"') { s += all[i]; i++; }
            if (i >= n) { error = "unterminated string in flylink.sleep"; return false; }
            i++; // closing "
            toks.push_back(MTok{s, true, start});
            continue;
        }
        if (c == '[' || c == ']') { toks.push_back(MTok{std::string(1, c), false, i}); i++; continue; }
        if (c == '{' || c == '}') { toks.push_back(MTok{std::string(1, c), false, i}); i++; continue; }
        if (c == ':' || c == ',') { i++; continue; } // tolerated separators
        // bare word (ident or punctuation glyph we ignore, e.g. `project`)
        size_t start = i;
        std::string w;
        while (i < n && all[i] != ' ' && all[i] != '\t' && all[i] != '\r' &&
               all[i] != '\n' && all[i] != '"' && all[i] != '[' && all[i] != ']' &&
               all[i] != '{' && all[i] != '}' && all[i] != '$' && all[i] != ':') {
            w += all[i];
            i++;
        }
        toks.push_back(MTok{w, false, start});
    }
    return true;
}

// The canonical `deps coll [ "a" "b" ... ]` package names from the manifest.
// Also used by `fly -deps` (which installs every declared package).
bool manifestParse(const std::string& manifestPath, Manifest& m) {
    m.ok = false;
    std::ifstream f(manifestPath);
    if (!f) { m.error = "cannot open flylink.sleep"; return false; }
    std::string all;
    std::string line;
    while (std::getline(f, line)) { all += line; all += '\n'; }

    std::vector<MTok> toks;
    std::string terr;
    if (!manifestTokenize(all, toks, terr)) { m.error = terr; return false; }

    // ---- parse: `[project] <key> <value> ...` where a value is either a
    // quoted string or `coll [ <quoted-string>* ]`.
    size_t k = 0;
    const size_t nt = toks.size();
    if (k < nt && toks[k].s == "project") k++;
    while (k < nt) {
        std::string key = toks[k].s;
        k++;
        if (k >= nt) { m.error = "dangling key '" + key + "' in flylink.sleep"; return false; }
        if (toks[k].s == "coll") {
            k++;
            if (k >= nt || toks[k].s != "[") { m.error = "expected '[' after 'coll' for '" + key + "'"; return false; }
            k++;
            std::vector<std::string> vals;
            while (k < nt && toks[k].s != "]") {
                vals.push_back(toks[k].s);
                k++;
            }
            if (k >= nt) { m.error = "unterminated coll list for '" + key + "'"; return false; }
            k++; // ']'
            if (key == "deps") m.deps = vals;
            else if (key == "test" && !vals.empty()) { m.hasTest = true; m.test = vals[0]; }
            // unknown coll-valued keys ignored
            continue;
        }
        // scalar value: must be a quoted string
        if (!toks[k].quoted) { m.error = "expected a quoted value for '" + key + "'"; return false; }
        std::string val = toks[k].s;
        k++;
        if (key == "name") m.name = val;
        else if (key == "version") m.version = val;
        else if (key == "source") m.source = val;
        else if (key == "output") m.output = val;
        else if (key == "test") { m.hasTest = true; m.test = val; }
        else if (key == "icon") m.icon = val;
        // unknown keys ignored
    }

    m.ok = true;
    return true;
}

// Outcome of a manifest deps edit: no rewrite needed (kUnchanged), the
// manifest was rewritten (kChanged), or the manifest could not be read,
// parsed, or written back (kError).
enum class DepEdit { kUnchanged, kChanged, kError };

// Adds (add=true) or removes (add=false) `pkg` from <root>/flylink.sleep's
// `deps coll [...]` list. The rewrite splices ONLY the `deps coll [...]`
// span, leaving every other byte of the file (the `$` header comment, the
// other fields, whitespace, layout) untouched. Existing entries keep their
// order; a new entry is appended; an already-present/freshly-absent entry
// short-circuits to kUnchanged so the file is only written when the list
// actually changes. This is the one and only place the manifest is
// mutated; the command layer calls it only AFTER a package installation
// has fully succeeded (or, for remove, reports failure if it cannot) so a
// failed download never reaches the manifest.
DepEdit manifestEditDeps(const std::string& root, const std::string& pkg, bool add) {
    std::string path = (fs::path(root) / "flylink.sleep").string();
    std::string text = readAll(path);
    if (text.empty()) return DepEdit::kError;

    std::vector<MTok> toks;
    std::string terr;
    if (!manifestTokenize(text, toks, terr)) return DepEdit::kError;

    // Locate `deps` `coll` `[` ... `]` and collect the currently-declared
    // entries (every token up to the closing bracket, mirroring the reader).
    bool found = false;
    std::vector<std::string> deps;
    size_t lb = 0, rb = 0;
    for (size_t t = 0; t + 2 < toks.size(); t++) {
        if (toks[t].s != "deps" || toks[t + 1].s != "coll") continue;
        if (toks[t + 2].s != "[") continue;
        size_t c = t + 3;
        while (c < toks.size() && toks[c].s != "]") { deps.push_back(toks[c].s); c++; }
        if (c >= toks.size()) return DepEdit::kError; // unterminated deps list
        lb = toks[t + 2].start;              // byte offset of '['
        rb = toks[c].start + 1;              // one past ']'
        found = true;
        break;
    }

    bool has = std::find(deps.begin(), deps.end(), pkg) != deps.end();
    if (add == has) return DepEdit::kUnchanged; // already in the desired state
    if (add) deps.push_back(pkg);               // preserve order, append new
    else deps.erase(std::find(deps.begin(), deps.end(), pkg));

    // Canonical form: `["a", "b"]`, or `[ ]` for an empty list (the -init
    // template's layout). The reader tolerates both.
    std::string list;
    if (deps.empty()) list = "[ ]";
    else {
        list = "[";
        for (size_t i = 0; i < deps.size(); i++) {
            if (i) list += ", ";
            list += "\"" + deps[i] + "\"";
        }
        list += "]";
    }

    std::string out;
    if (found) {
        out = text.substr(0, lb) + list + text.substr(rb);
    } else {
        // No deps list in the manifest at all: append the canonical line.
        out = text;
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "  deps coll " + list + "\n";
    }
    if (!writeAll(path, out)) return DepEdit::kError;
    return DepEdit::kChanged;
}

// HTTPS download of one package's .fly source from the Dump repository.
// Returns true iff the file was written successfully; false on any failure
// (curl absent, network error, 404). The caller decides on mirror fallback.
bool downloadFromDump(const std::string& packageName, const std::string& destPath) {
    // The live Dump repo URL, overridable for tests the same way the
    // runtime allows FLY_TLS_CA_FILE: FLY_DUMP_URL_BASE points the
    // download at a deterministic local HTTPS fixture that serves the
    // public tree (nothing here hard-codes a test host, and the mirror
    // fallback stays intact for every real failure -- refused, 404, or
    // no network).
    const char* override = getenv("FLY_DUMP_URL_BASE");
    std::string base = (override && override[0] != '\0')
        ? override
        : "https://raw.githubusercontent.com/TheServer-lab/fly-dump/refs/heads/main/public/";
    std::string url = base + packageName + ".fly";
    // cmd.exe has no /dev/null: a POSIX `2>/dev/null` makes the whole
    // redirected command fail with "The system cannot find the path
    // specified." (it tries to create a \dev\null file) before curl even
    // starts -- so every download unconditionally fell back to the mirror.
    // `2>NUL` is the Windows equivalent (POSIX keeps /dev/null).
#ifdef _WIN32
    const char* nullDev = "NUL";
#else
    const char* nullDev = "/dev/null";
#endif
    std::string cmd = "curl -sS --fail --connect-timeout 10 -o \"" + destPath + "\" \"" + url + "\" 2>" + nullDev;
    int rc = std::system(cmd.c_str());
    return rc == 0;
}

// Installs one package into <root>/module/<pkg>.fly: try HTTPS Dump first
// (unless offline), then fall back to the local mirror. Mirrors fly.cpp's
// original -deps behavior; returns 0 on success, nonzero with a message on
// failure. Prints the per-package status lines.
int installPackage(const std::string& root, const std::string& pkg, bool offline) {
    std::string moduleDir = root + "/module";
    std::error_code ec;
    fs::create_directories(moduleDir, ec);
    if (ec) {
        std::cerr << "fly: -deps: could not create " << moduleDir << ": " << ec.message() << "\n";
        return 1;
    }
    std::string dst = moduleDir + "/" + pkg + ".fly";
    bool got = false;

    if (!offline) {
        if (downloadFromDump(pkg, dst)) {
            std::cout << "fly -deps: downloaded " << pkg << " from Dump -> " << dst << "\n";
            got = true;
        } else {
            std::cout << "fly -deps: download failed for '" << pkg << "' from Dump, trying local mirror...\n";
        }
    }

    if (!got) {
#ifdef FLY_DUMP_MIRROR_DIR
        std::string mirrorDir = FLY_DUMP_MIRROR_DIR;
        std::string src = mirrorDir + "/" + pkg + ".fly";
        std::error_code ec2;
        if (fileExists(src)) {
            std::error_code ec3;
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec3);
            if (ec3) {
                std::cerr << "fly -deps: failed to install '" << pkg << "': " << ec3.message() << "\n";
                return 1;
            }
            std::cout << "fly -deps: installed " << pkg << " from mirror -> " << dst << "\n";
            got = true;
        } else {
            std::cerr << "fly -deps: package '" << pkg << "' is not available "
                         "(mirror: " << mirrorDir << ", Dump download "
                         << (offline ? "skipped (--offline)" : "failed") << ")\n";
            return 1;
        }
#else
        std::cerr << "fly: -deps: package '" << pkg << "' is not available "
                     "(no Dump mirror configured, Dump download "
                     << (offline ? "skipped (--offline)" : "failed") << ")\n";
        return 1;
#endif
    }
    return got ? 0 : 1;
}

std::string shellQuote(const std::string& s) {
    return "\"" + s + "\"";
}

// True if `path` is lexically inside `dir` (or equal to it). Both are
// canonicalized first (symlinks resolved), so it's not fooled by `..` or
// link prefixes -- used as a guardrail for destructive operations.
bool isWithin(const std::string& path, const std::string& dir) {
    fs::path p = fs::weakly_canonical(fs::path(path));
    fs::path d = fs::weakly_canonical(fs::path(dir));
    auto pi = p.begin();
    auto di = d.begin();
    for (; di != d.end(); ++di, ++pi) {
        if (pi == p.end() || *pi != *di) return false;
    }
    return true;
}

} // namespace

#ifdef _WIN32
// Spawns argv[0] (the program) with argv[1..] as its arguments and waits
// for it, mirroring what execvp()+waitpid() does for cmdRun's POSIX path:
// a full path/command line built with the same quote-escaping rules the C
// runtime applies when it parses a CreateProcess command line (backslashes
// before a quote or at the end of an argument are doubled -- see
// runtime/process.c's buildWindowsCommandLine, which uses the same rules).
int runWindowsProcess(const std::vector<std::string>& argv) {
    std::string cmdline;
    bool first = true;
    for (const std::string& a : argv) {
        if (!first) cmdline += ' ';
        first = false;
        cmdline += '"';
        size_t backslashes = 0;
        for (char c : a) {
            if (c == '\\') { backslashes++; continue; }
            if (c == '"') {
                cmdline.append(backslashes * 2 + 1, '\\');
                cmdline += '"';
                backslashes = 0;
                continue;
            }
            cmdline.append(backslashes, '\\');
            backslashes = 0;
            cmdline += c;
        }
        cmdline.append(backslashes * 2, '\\');
        cmdline += '"';
    }

    std::vector<char> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back('\0');

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;

    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#endif

// ---- public helpers -------------------------------------------------------

std::string findProjectRoot() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) return "";
    for (int i = 0; i < 8; i++) {
        std::error_code ec2;
        if (fs::exists(dir / "flylink.sleep", ec2) && !ec2) return dir.string();
        if (!dir.has_parent_path() || dir == dir.root_path()) break;
        dir = dir.parent_path();
    }
    return "";
}

Manifest readManifest(const std::string& root) {
    Manifest m;
    std::string path = (fs::path(root) / "flylink.sleep").string();
    if (!manifestParse(path, m)) return m;
    return m;
}

std::string resolvePath(const std::string& root, const std::string& rel) {
    fs::path p(rel);
    if (p.is_absolute()) return p.string();
    return (fs::path(root) / p).string();
}

// The executable the manifest's `output "bin/demo"` actually lands at. On
// Windows the toolchain's `cc -o X` produces `X.exe` (the MinGW driver
// appends the executable suffix), and CreateProcess only finds a plain `X`
// via its final-effort .exe search when NO `X` byte exists -- so any
// existence/staleness/clean logic must target `X.exe`, like CMake's own
// add_executable(). The flylink.sleep value stays platform-agnostic.
std::string outputExePath(const std::string& root, const std::string& out) {
#ifdef _WIN32
    return resolvePath(root, out) + ".exe";
#else
    return resolvePath(root, out);
#endif
}

// The effective Windows icon for a build: the manifest's `icon "..."` when
// configured (resolved against the project root), otherwise the toolchain
// default fly.ico baked in at CMake configure time (FLY_DEFAULT_ICON).
// Empty when neither exists -- compiled without an embedded icon. fly-cc
// only acts on this on Windows, so passing it on Linux is harmless.
std::string projectIconPath(const std::string& root, const Manifest& m) {
    if (!m.icon.empty()) return resolvePath(root, m.icon);
#ifdef FLY_DEFAULT_ICON
    return FLY_DEFAULT_ICON;
#else
    return "";
#endif
}

std::string findSibling(const std::string& name) {
    std::string dir = selfExeDir();
    if (!dir.empty()) {
        std::string candidate = dir + "/" + name;
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) return candidate;
    }
    return name;
}

// ---- commands --------------------------------------------------------------

int cmdDeps(const std::string& root, const Manifest& m, bool offline) {
    if (m.deps.empty()) {
        std::cout << "fly -deps: " << root << "/flylink.sleep declares no 'deps coll [...]' -- nothing to install.\n";
        return 0;
    }
    int installed = 0, failed = 0;
    for (auto& dep : m.deps) {
        if (installPackage(root, dep, offline) == 0) installed++;
        else failed++;
    }
    if (failed > 0) return 1;
    std::cout << "fly -deps: " << installed << " package(s) installed under " << root << "/module\n";
    return 0;
}

int cmdBuild(const std::string& root, const Manifest& m) {
    if (m.source.empty()) {
        std::cerr << "fly: -build: " << root << "/flylink.sleep sets no 'source' (add a line like  source \"src/main.fly\")\n";
        return 1;
    }
    if (m.output.empty()) {
        std::cerr << "fly: -build: " << root << "/flylink.sleep sets no 'output' (add a line like  output \"bin/my_project\")\n";
        return 1;
    }
    std::string srcPath = resolvePath(root, m.source);
    if (!fileExists(srcPath)) {
        std::cerr << "fly: -build: source file not found: " << srcPath << "\n";
        return 1;
    }
    // A manifest-configured icon is a hard requirement: if `icon "path"`
    // names a missing file the build fails with a clear error (no silent
    // fallback to the default -- omitting the field entirely IS the
    // "use the default fly.ico" signal). The default icon itself is baked
    // into the binary at configure time and always exists in a source
    // build, so it is not re-validated here (and on Linux icons are
    // ignored entirely, so a broken default can never break a build).
    std::string iconPath = projectIconPath(root, m);
    if (!m.icon.empty() && !fileExists(iconPath)) {
        std::cerr << "fly: -build: icon file not found: " << iconPath
                  << " (set by 'icon \"...\"' in flylink.sleep)\n";
        return 1;
    }
    std::string outPath = outputExePath(root, m.output);
    std::error_code ec;
    fs::create_directories(fs::path(outPath).parent_path(), ec);
    if (ec) {
        std::cerr << "fly: -build: could not create output directory for " << outPath << ": " << ec.message() << "\n";
        return 1;
    }
    std::string cc = findSibling("fly-cc");
    std::vector<std::string> toolArgs = {cc, srcPath, "-o", outPath};
    if (!iconPath.empty()) {
        toolArgs.push_back("-icon");
        toolArgs.push_back(iconPath);
    }
    int rc = runTool(toolArgs);
    if (rc != 0) {
        std::cerr << "fly: -build: compilation failed (fly-cc exited " << rc << ")\n";
        return rc;
    }
    std::cout << "fly: built " << m.source << " -> " << m.output << "\n";
    return 0;
}

namespace {
// Rebuild if the output binary is missing, or if any project .fly source
// (the manifest's entry source, anything under src/, or an installed module)
// is newer than it. Module files are included so that re-installing a
// package after `fly -deps` forces relinking when the module's contents
// changed on disk.
bool outputStale(const std::string& outPath, const std::string& root, const Manifest& m) {
    if (!fileExists(outPath)) return true;
    std::error_code ec;
    auto outTime = fs::last_write_time(outPath, ec);
    if (ec) return true;

    auto newer = [&](const std::string& p) {
        std::error_code e2;
        if (!fs::exists(p, e2) || e2) return false;
        auto t = fs::last_write_time(p, e2);
        // >= (not >): the source lookup runs on stat()/st_mtime on this
        // toolchain, which truncates to whole SECONDS -- a source edited
        // within the same second the exe was linked compares EQUAL, and a
        // strict > would silently skip a real source edit. Equal counts as
        // stale; the cost is one redundant rebuild when times genuinely
        // coincide, the benefit is never missing a change.
        return !e2 && t >= outTime;
    };

    if (!m.source.empty() && newer(resolvePath(root, m.source))) return true;
    // The effective icon is a build input for `-run` too: editing the
    // configured .ico (or swapping it to a different file) must force a
    // relink, exactly like a source edit. projectIconPath() mirrors what
    // cmdBuild embedded, so the check is consistent between build/run.
    std::string iconPath = projectIconPath(root, m);
    if (!iconPath.empty() && newer(iconPath)) return true;
    for (auto& mod : std::vector<std::string>{
             (fs::path(root) / "src").string(), (fs::path(root) / "module").string()}) {
        std::error_code e2;
        if (!fs::exists(mod, e2) || e2) continue;
        for (auto& de : fs::recursive_directory_iterator(mod, e2)) {
            if (e2) break;
            if (de.is_regular_file() && de.path().extension() == ".fly" && newer(de.path().string()))
                return true;
        }
    }
    return false;
}
} // namespace

int cmdRun(const std::string& root, const Manifest& m, const std::vector<std::string>& args) {
    if (m.output.empty()) {
        std::cerr << "fly: -run: " << root << "/flylink.sleep sets no 'output'\n";
        return 1;
    }
    std::string outPath = outputExePath(root, m.output);
    if (!fileExists(outPath) || outputStale(outPath, root, m)) {
        int rc = cmdBuild(root, m);
        if (rc != 0) return rc;
    }
    if (!fileExists(outPath)) {
        std::cerr << "fly: -run: " << outPath << " was not produced\n";
        return 1;
    }
    std::error_code ec;
    fs::current_path(root, ec); // program runs from the project root
#ifdef _WIN32
    std::vector<std::string> argv;
    argv.push_back(outPath);
    for (auto& a : args) argv.push_back(a);
    int rc = runWindowsProcess(argv);
    if (rc < 0) {
        std::cerr << "fly: -run: could not execute '" << outPath << "'\n";
        return 1;
    }
    return rc;
#else
    std::vector<char*> cargs;
    cargs.push_back(const_cast<char*>(outPath.c_str()));
    for (auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
    cargs.push_back(nullptr);
    execvp(outPath.c_str(), cargs.data());
    std::cerr << "fly: -run: could not execute '" << outPath << "'\n";
    return 1;
#endif
}

int cmdInit(const std::string& name) {
    if (name.empty() || name == "." || name == "..") {
        std::cerr << "fly: -init: provide a project name (e.g.  fly -init my_project)\n";
        return 1;
    }
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (!ok || (i == 0 && (c >= '0' && c <= '9'))) {
            std::cerr << "fly: -init: '" << name << "' is not a valid project name (use letters, digits, '_'; not starting with a digit)\n";
            return 1;
        }
    }
    std::error_code ec;
    fs::path dir = fs::current_path(ec) / name;
    if (fs::exists(dir, ec) && !ec) {
        std::cerr << "fly: -init: '" << dir.string() << "' already exists\n";
        return 1;
    }
    fs::create_directories(dir / "src", ec);
    if (ec) {
        std::cerr << "fly: -init: could not create project directory: " << ec.message() << "\n";
        return 1;
    }

    std::string manifestContent =
        "$ " + name + " project manifest (docs/architecture.md §9)\n"
        "project\n"
        "  name \"" + name + "\"\n"
        "  version \"0.1.0\"\n"
        "  source \"src/main.fly\"\n"
        "  output \"bin/" + name + "\"\n"
        "  deps coll [ ]\n";
    std::string mainContent = "show(\"Hello from " + name + "!\")\n";

    if (!writeAll((dir / "flylink.sleep").string(), manifestContent)) {
        std::cerr << "fly: -init: could not write flylink.sleep\n";
        return 1;
    }
    if (!writeAll((dir / "src" / "main.fly").string(), mainContent)) {
        std::cerr << "fly: -init: could not write src/main.fly\n";
        return 1;
    }

    std::cout << "fly: created project '" << name << "' at " << dir.string() << "\n";
    std::cout << "  flylink.sleep\n";
    std::cout << "  src/main.fly\n";
    std::cout << "next:  cd " << name << "  &&  fly -build  &&  fly -run\n";
    return 0;
}

int cmdTest(const std::string& root, const Manifest& m) {
    std::vector<std::string> files;
    if (m.hasTest) {
        files.push_back(resolvePath(root, m.test));
    } else {
        std::string td = (fs::path(root) / "tests").string();
        std::error_code ec;
        if (fs::exists(td, ec) && !ec) {
            for (auto& de : fs::directory_iterator(td, ec)) {
                if (ec) break;
                if (de.is_regular_file() && de.path().extension() == ".fly")
                    files.push_back(de.path().string());
            }
            std::sort(files.begin(), files.end());
        }
    }
    if (files.empty()) {
        std::cout << "fly: no tests found for project at " << root
                  << " (add a tests/ directory with .fly files, or a test \"path.fly\" line in flylink.sleep)\n";
        return 0;
    }

    std::string buildDir = (fs::path(root) / "build-tests").string();
    std::error_code ec;
    fs::create_directories(buildDir, ec);
    if (ec) {
        std::cerr << "fly: -test: could not create " << buildDir << ": " << ec.message() << "\n";
        return 1;
    }
    std::string cc = findSibling("fly-cc");

    int passed = 0;
    for (auto& f : files) {
        std::string stem = f.find_last_of('/') == std::string::npos
                               ? f.substr(0, f.size() - 4)
                               : f.substr(f.find_last_of('/') + 1);
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".fly") == 0)
            stem = stem.substr(0, stem.size() - 4);
        std::string bin = (fs::path(buildDir) / stem).string();

        int rc = runTool({cc, f, "-o", bin});
        if (rc != 0) {
            std::cerr << "fly: -test: '" << f << "' failed to build (fly-cc exited " << rc << ")\n";
            return rc;
        }
        int rrc = runTool({bin});
        if (rrc != 0) {
            std::cerr << "fly: -test: '" << f << "' failed (exited " << rrc << ")\n";
            return rrc;
        }
        std::cout << "fly: test '" << f << "' passed\n";
        passed++;
    }
    std::cout << "fly: " << passed << " test(s) passed\n";
    return 0;
}

int cmdClean(const std::string& root, const Manifest& m) {
    if (m.output.empty()) {
        std::cerr << "fly: -clean: " << root << "/flylink.sleep sets no 'output'\n";
        return 1;
    }
    std::string outPath = outputExePath(root, m.output);

    // Guardrails: never touch anything outside the project, and never even
    // look at src/ or module/ (source and dependencies are not build
    // artifacts).
    if (!isWithin(outPath, root)) {
        std::cerr << "fly: -clean: refusing to remove '" << outPath << "': outside the project root\n";
        return 1;
    }
    std::string srcDir = (fs::path(root) / "src").string();
    std::string modDir = (fs::path(root) / "module").string();
    if (isWithin(outPath, srcDir) || isWithin(outPath, modDir)) {
        std::cerr << "fly: -clean: refusing to remove '" << outPath << "': looks like source, not a build artifact\n";
        return 1;
    }

    int removed = 0;
    auto removeIfPresent = [&](const std::string& p, const char* what) {
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) {
            if (fs::remove(p, ec) && !ec) removed++;
            std::cout << "fly: -clean: removed " << p << "\n";
        }
    };
    removeIfPresent(outPath, "output");
    removeIfPresent(outPath + ".o", "object"); // fly-cc's transient .o, if any survived

    std::string bt = (fs::path(root) / "build-tests").string();
    std::error_code btEc;
    if (fs::exists(bt, btEc) && !btEc && fs::is_directory(bt, btEc)) {
        fs::remove_all(bt, btEc);
        if (!btEc) removed++;
        std::cout << "fly: -clean: removed directory " << bt << "\n";
    }

    if (removed == 0) std::cout << "fly: nothing to clean in " << root << "\n";
    return 0;
}

int cmdDump(const std::string& root, const std::vector<std::string>& args) {
    bool offline = false;
    std::vector<std::string> rest;
    for (auto& a : args) {
        if (a == "--offline") offline = true;
        else rest.push_back(a);
    }
    if (rest.empty()) {
        std::cerr << "fly: -dump: expected one of: install \"pkg\", remove \"pkg\", list, update [--offline]\n";
        return 1;
    }
    const std::string& sub = rest[0];
    std::string moduleDir = root + "/module";

    if (sub == "list") {
        std::error_code ec;
        std::vector<std::string> pkgs;
        if (fs::exists(moduleDir, ec) && !ec) {
            for (auto& de : fs::directory_iterator(moduleDir, ec)) {
                if (ec) break;
                if (de.is_regular_file() && de.path().extension() == ".fly")
                    pkgs.push_back(de.path().stem().string());
            }
            std::sort(pkgs.begin(), pkgs.end());
        }
        if (pkgs.empty()) {
            std::cout << "fly -dump: no packages installed under " << moduleDir << "\n";
            return 0;
        }
        std::cout << "fly -dump: installed under " << moduleDir << ":\n";
        for (auto& p : pkgs) std::cout << "  " << p << "\n";
        return 0;
    }

    if (sub == "install") {
        if (rest.size() < 2) {
            std::cerr << "fly: -dump install: provide a package name (e.g.  fly -dump install \"sleep\")\n";
            return 1;
        }
        const std::string& pkg = rest[1];
        // Package installation and manifest registration are one coherent
        // operation: install first (so a failed download never touches the
        // manifest), then register the dependency. A registration failure
        // after a successful install is reported honestly -- the module
        // file IS on disk, so claiming total success would be a lie.
        int rc = installPackage(root, pkg, offline);
        if (rc != 0) return rc;
        DepEdit dr = manifestEditDeps(root, pkg, /*add=*/true);
        if (dr == DepEdit::kError) {
            std::cerr << "fly -dump: installed " << pkg
                      << " but could not register it in flylink.sleep deps\n";
            return 1;
        }
        if (dr == DepEdit::kChanged)
            std::cout << "fly -deps: registered " << pkg << " in flylink.sleep deps\n";
        return 0;
    }

    if (sub == "remove") {
        if (rest.size() < 2) {
            std::cerr << "fly: -dump remove: provide a package name (e.g.  fly -dump remove \"sleep\")\n";
            return 1;
        }
        const std::string& pkg = rest[1];
        std::string path = moduleDir + "/" + pkg + ".fly";
        if (!isWithin(path, root)) {
            std::cerr << "fly: -dump remove: refusing to remove '" << path << "': outside the project root\n";
            return 1;
        }
        std::error_code ec;
        if (!fs::exists(path, ec) || ec) {
            std::cerr << "fly: -dump remove: '" << pkg << "' is not installed (no " << path << ")\n";
            return 1;
        }
        if (!fs::remove(path, ec) || ec) {
            std::cerr << "fly: -dump remove: failed to remove " << path << ": " << ec.message() << "\n";
            return 1;
        }
        std::cout << "fly -dump: removed " << pkg << " -> " << path << "\n";
        // Removal unregisters the dependency, mirroring install's
        // registration -- the package manager treats the deps list as the
        // source of truth, so leaving a removed package declared would make
        // the next `fly -deps` silently reinstall it.
        DepEdit dr = manifestEditDeps(root, pkg, /*add=*/false);
        if (dr == DepEdit::kError) {
            std::cerr << "fly -dump: removed " << pkg
                      << " but could not update flylink.sleep deps\n";
            return 1;
        }
        if (dr == DepEdit::kChanged)
            std::cout << "fly -deps: unregistered '" << pkg << "' from flylink.sleep deps\n";
        return 0;
    }

    if (sub == "update") {
        std::error_code ec;
        std::vector<std::string> pkgs;
        if (fs::exists(moduleDir, ec) && !ec) {
            for (auto& de : fs::directory_iterator(moduleDir, ec)) {
                if (ec) break;
                if (de.is_regular_file() && de.path().extension() == ".fly")
                    pkgs.push_back(de.path().stem().string());
            }
            std::sort(pkgs.begin(), pkgs.end());
        }
        if (pkgs.empty()) {
            std::cout << "fly -dump: nothing to update (no packages installed under " << moduleDir << ")\n";
            return 0;
        }
        int ok = 0, failed = 0;
        for (auto& p : pkgs) {
            if (installPackage(root, p, offline) == 0) ok++;
            else failed++;
        }
        if (failed > 0) return 1;
        std::cout << "fly -dump: updated " << ok << " package(s) under " << moduleDir << "\n";
        return 0;
    }

    std::cerr << "fly: -dump: unknown subcommand '" << sub << "' (expected install/remove/list/update)\n";
    return 1;
}

} // namespace fly