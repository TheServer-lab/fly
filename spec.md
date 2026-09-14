# Fly Language and Toolchain Specification

> **Status note:** This document describes Fly **as implemented** in the current repository (version 0.1.3). Where the implementation, tests, and this document disagree, the implementation and passing tests are authoritative. Anything marked *planned* is not yet shipped.

---

## 1. Overview

Fly is a high-level, dynamically typed, general-purpose programming language that compiles to native executables.

Design principle:

> **One language. One toolchain. One ecosystem.**

Fly deliberately uses its own vocabulary. It is *inspired by* high-level languages but is **not compatible** with any of them, and must not be pattern-matched from Python, JavaScript, C++, or Rust conventions.

Current development version: **0.1.3**.

Fly ships three executables:

| Executable   | Role |
|--------------|------|
| `fly`        | Toolchain launcher + project-command front-end |
| `fly-cc`     | The native compiler (lexer, parser, sema, IR, LLVM, linking) |
| `fly-repl`   | Interactive REPL |

---

## 2. Repository layout

```text
src/fly/          fly launcher, project commands (fly.cpp, project.cpp, format.cpp)
src/driver/       fly-cc front-end (driver.cpp), ModuleMerger (bring resolution)
src/lexer/        lexer implementation
src/parser/       parser implementation
src/ast/          AST representation
src/sema/         semantic analysis
src/codegen/      IR generation and builtins dispatch
src/repl/         REPL implementation
include/flycc/    compiler headers (token.h, lexer.h, parser.h, ast.h, sema.h, codegen.h, driver.h)
runtime/          libflyrt C runtime (flyrt.h + value.c, arith.c, text.c, coll.c, ...)
stdlib/           bundled stdlib name-stub .fly files (bring-able; compiler-known dotted builtins)
dump/public/      upstream source of public Fly modules (sleep.fly, http.fly, hashbox.fly)
dump-mirror/      local fallback copies of the public modules (offline installs)
examples/         example Fly programs and project demos
tests/            end-to-end test suite (CTest; no unit tests yet)
windows/          Windows installer (NSIS) and release staging
linux/            Linux release staging
docs/             architecture documentation
```

A project's installed dependencies live in `module/` under that project (created by `fly -deps` / `fly -dump install`). `module/` is **not** a repository directory.

---

## 3. Language specification

### 3.1 File extension

```text
.fly
```

### 3.2 Comments

Single-line comments begin with `$`:

```fly
$ this is a comment
name = "Rick" $ trailing comment
```

Multiline comments use `$$`:

```fly
$$
This is a
multiline comment.
$$
```

`#` and `//` are **not** Fly comments.

### 3.3 Core value types

| Type     | Meaning |
|----------|---------|
| `tex`    | text (UTF-8 string) |
| `num`    | 64-bit signed integer |
| `dec`    | double-precision float |
| `yn`     | boolean |
| `coll`   | ordered collection (array) |
| `board`  | key/value map |
| `emp`    | empty value |
| `job`    | callable function value (used for GUI callbacks and `bring`'d modules) |

Type names appear as cast operations, not declaration keywords:

```fly
age = num("18")
price = dec("19.99")
text = tex(123)
```

### 3.4 Literals

Booleans use exact case:

```fly
Yes
No
```

`yes`, `YES`, `no`, `EMP` (wrong case) lex as ordinary identifiers.

The empty value:

```fly
EMP
```

Numbers and decimals use ordinary literal syntax:

```fly
42
3.14
```

Text is double-quoted (see §3.11).

### 3.5 Variables

Variables are dynamically typed and mutable by default:

```fly
value = 42
value = "Hello"
value = Yes
```

Immutable bindings use `hard`:

```fly
hard value = 42
```

Reassigning a `hard` binding is a compile error (`cannot reassign hard variable`). `hard` at top level allocates a global slot.

The empty value is spelled `EMP`.

### 3.6 Operators

Arithmetic:

```text
+   -   *   /   %
```

Comparison:

```text
==   !=   <   >   <=   >=
```

Logical:

```text
and   or   not
```

Every operator lowers to a libflyrt call (`fly_rt_add`, `fly_rt_sub`, ..., `fly_rt_and`, `fly_rt_not`). Unary minus is `fly_rt_neg`.

### 3.7 Collections

Collections are ordered and may hold mixed types:

```fly
items = [10, "hello", Yes, 3.14]
```

Indexing is zero-based:

```fly
items[0]
```

Slicing is supported (start inclusive, end exclusive):

```fly
items[1:4]    $ from index 1 to before index 4
items[:3]     $ from start to before index 3
items[2:]     $ from index 2 to end
items[:]      $ the whole copy
```

### 3.8 Boards

Boards are key/value data:

```fly
person = {
    "name": "Rick"
    "age": 18
}
```

The key expression is followed by `:` and then the value expression. Commas between entries are optional — both `{"a": 1, "b": 2}` and the multi-line newline-separated form above parse. Keys are matched by recursive value equality. Boards are linearly scanned (parallel key/value arrays); lookup is `O(n)`.

### 3.9 Functions

Functions are declared with `job`:

```fly
job add(a, b) {
    give a + b
}
```

A `job` that reaches the end without `give` produces `EMP`. A bare `give` (no value) is valid and returns `EMP`.

`native job` declares a function implemented in libflyrt rather than in Fly:

```fly
native job sha256(text)
```

The declaration binds the Fly name `sha256` to the C symbol `fly_sha256` in libflyrt. Native jobs are real, implemented functionality; they are not mere prototypes.

### 3.10 Input and output

```fly
name = take("Name: ")   $ prints the prompt and reads a line from stdin
show(name)              $ prints a value followed by a newline
```

`show` accepts any value. `take` returns the read line as `tex`.

### 3.11 Text and interpolation

```fly
message = "Hello"
show("Hello, {name}")
show("Result: {add(10, 5)}")    $ expressions evaluate when the text is created
```

Literal braces:

```fly
show("Use {{name}} literally.")   $ prints: Use {name} literally.
```

Text supports byte-safe operations (`cut`, `raise`, `lower`, `sever`, `bind`, `seek`, `has`, indexing, slicing). Text is **not** automatically binary-safe for arbitrary bytes with zero values.

### 3.12 Conditions

```fly
if condition {
    ...
}
orif other_condition {
    ...
}
ifnot {
    ...
}
```

`orif` = else-if; `ifnot` = else.

### 3.13 Loops

`while`:

```fly
while condition {
    ...
}
```

`for` over an iterable (coll, board, or tex):

```fly
for item in items {
    show(item)
}
```

Loop control:

```fly
skip       $ skip the rest of this iteration (like continue)
getout     $ exit the loop (like break)
```

`skip`/`getout` outside a loop are compile errors (`used outside of a loop`), enforced by Sema.

### 3.14 Error handling

```fly
do {
    value = num("hello")
}
grab (err) {
    show("Conversion failed.")
}
```

Errors are real runtime values thrown through the C++ exception ABI and caught by `grab`. An error not handled by any enclosing `grab` propagates outward; an unhandled error at top level terminates the program with an error message and a nonzero exit code. There is no `finally`.

### 3.15 Type casting

```fly
num("42")    $ throws a catchable error on failure
dec("3.14")
tex(123)
```

Casting failures are ordinary runtime errors, catchable with `do`/`grab`. Casting uses strict conversion rules (e.g. `num("42abc")` throws).

### 3.16 Modules

`bring` imports a module by name, resolving through the module search path (§5.1):

```fly
bring path
bring hashbox as hb
```

`bring X as Y` aliases the module's jobs so `X.name(...)` becomes `Y.name(...)`. A brought module's jobs are just flat-named jobs; the alias rebinds them under the qualified name.

### 3.17 Collection and text operations

All take the collection/text as the first argument, then the element/value:

```fly
attach(items, value)        $ append to a coll
place(items, index, value)  $ insert at index (also board set via place(board, key, value))
erase(items, index)         $ remove by index or key
count(items)                $ length (coll, board, or tex)
seek(items, value)          $ index of a value (coll or tex); EMP if absent
has(items, value)           $ membership (coll or tex)
bind(items, separator)      $ join a coll into a tex
sever(text, separator)      $ split a tex into a coll
cut(text)                   $ trim whitespace
raise(text)                 $ uppercase
lower(text)                 $ lowercase
```

`attach`, `place`, `erase`, `bind`, `sever`, `cut`, `raise`, `lower` are *code-unit* oriented (they operate on bytes/values, not Unicode codepoints).

### 3.18 Reserved keywords

Fly reserves:

```text
if      orif    ifnot
while   for     in
job     give
take    show    bring
do      grab
hard    native
skip    getout
attach  place   erase   count   seek   has
bind    sever   cut     raise   lower
and     or      not
Yes     No      EMP
```

---

## 4. Built-in modules and the compiler's builtins table

The built-in modules are compiler-known: `bring filesystem` resolves to a `stdlib/*.fly` stub whose dotted calls are lowered directly by `codegen.cpp`. They are **not** the same as public Fly modules (§5).

The complete set of compiler-recognized callable names (61 total):

### 4.1 Standalone

```text
show(value)              print value + newline
take(prompt)             read a line from stdin
num(value)               cast to number (throws on failure)
dec(value)               cast to decimal (throws on failure)
tex(value)               convert to text
```

### 4.2 Collection / text (11)

```text
attach  place  erase  count  seek  has  bind  sever  cut  raise  lower
```

### 4.3 `path.*` (7)

```text
path.basename(p)      path.dirname(p)      path.extension(p)
path.stem(p)          path.absolute(p)     path.separator()
path.join(a, b, ...)  (variadic)
```

### 4.4 `filesystem.*` (11)

```text
filesystem.exists(p)   filesystem.isfile(p)   filesystem.isdir(p)
filesystem.read(p)     filesystem.write(p, s) filesystem.append(p, s)
filesystem.mkdir(p)    filesystem.remove(p)   filesystem.list(p)
filesystem.cwd()       filesystem.chdir(p)
```

### 4.5 `process.*` (5)

```text
process.args()         process.run(cmd, args)   process.spawn(cmd, args)
process.wait(pid)      process.exit(code)
```

`process.run` returns the child's numeric exit code. `process.spawn` returns a numeric pid. `process.exit` terminates the process (never returns).

### 4.6 `environment.*` (3)

```text
environment.get(name)      environment.exists(name)    environment.set(name, value)
```

`environment.get` returns `EMP` when unset.

### 4.7 `system.*` (3)

```text
system.os()          system.arch()      system.hostname()
```

`system.os()` returns `"windows"`, `"linux"`, or `"macos"`.

### 4.8 `net.*` (8)

```text
net.resolve(host)                    DNS lookup, returns tex IP
net.connect(host, port)              TCP connect, returns numeric fd
net.connect_tls(host, port)          TLS connect (OpenSSL), returns numeric handle; throws catchable error on failure
net.listen(host, port)               TCP listen, returns numeric fd
net.accept(fd)                       blocking accept, returns numeric fd
net.send(fd, bytes)                  blocking send, returns number of bytes sent
net.receive(fd, max)                 blocking receive, returns tex ("" = EOF)
net.close(fd)                        close a connection
```

### 4.9 `gui.*` (6)

```text
gui.window(title, width, height)     returns numeric window handle
gui.label(text)                      returns numeric widget handle
gui.button(text)                     returns numeric widget handle
gui.textbox()                        returns numeric widget handle
gui.run()                            native message loop; returns EMP when the last window closes
gui.close(handle)                    close a window
```

Platform support: Win32 is automatic. GTK 4 is detected on Linux (optional); a backend-less build still compiles but `gui.*` throws a catchable runtime error. Member-call sugar `widget.add(child)`, `widget.on_click(job)` is resolved by codegen for GUI handles.

---

## 5. Module system

### 5.1 `bring` resolution order

`ModuleMerger::resolveModule` in `src/driver/driver.cpp` searches, in order, for `<name>.fly`:

1. Next to the bringing file (`<fromDir>/<name>.fly`)
2. `<fromDir>/src/<name>.fly`
3. The bundled stdlib directory (compile-time `FLY_BUILTIN_STDLIB_DIR`)
4. The `FLY_STDLIB_DIR` environment variable override
5. `<projectRoot>/module/<name>.fly` (installed packages; project root found by walking up ≤ 8 levels for `flylink.sleep`)
6. `<fromDir>/module/<name>.fly` (single-file project fallback)

Stdlib has priority over installed packages, so a project can never shadow a built-in.

### 5.2 Bundled stdlib

`stdlib/*.fly` contains the seven bring-able built-in module stubs: `filesystem`, `path`, `process`, `environment`, `system`, `net`, `gui`. They are doc/name stubs: the actual behavior is hardwired in the compiler's builtins table (§4). `bring sleep` / `bring http` / `bring hashbox` resolve **only** through the public-module path (an installed `module/<name>.fly`).

### 5.3 Public modules

Public modules are separately distributed Fly source files, installed per-project:

| Package   | Provides |
|-----------|----------|
| `sleep`   | SLEEP v1.0 library (Simple Lightweight Extensible Expression Protocol — used for the `flylink.sleep` manifest and the module's data format) |
| `http`    | HTTP/1.1 client built on `bring net` (TLS via `net.connect_tls`) |
| `hashbox` | SHA-256 hashing (`hashbox.sha256(text)`, `hashbox.file_sha256(path)`) |

Upstream source: `dump/public/*.fly` (GitHub: `TheServer-lab/fly-dump`, `public/`). Local fallback for offline use: `dump-mirror/*.fly`.

### 5.4 Installing packages

```text
fly -deps                install every package in flylink.sleep's deps into module/
fly -deps --offline      install from the local mirror only, no network
fly -dump install "pkg"  install one package and register it in flylink.sleep deps
fly -dump remove "pkg"   remove an installed package and unregister it
fly -dump list           list installed packages
fly -dump update         re-fetch every installed package
```

Download source is `https://raw.githubusercontent.com/TheServer-lab/fly-dump/refs/heads/main/public/<name>.fly` (overridable via `FLY_DUMP_URL_BASE`). With `--offline`, or if HTTPS fails, the local mirror in `dump-mirror/` is used.

On Windows with `SSL_CERT_FILE` set, installs pass `--cacert <ca> --ssl-no-revoke` to keep TLS verification on.

---

## 6. Project manifests (`flylink.sleep`)

A project is defined by `flylink.sleep` at the project root. The format is the SLEEP protocol.

```sleep
project
  name "my_project"
  version "0.1.0"
  source "src/main.fly"
  output "bin/my_project"
  deps coll [ "sleep" ]
  icon "assets/fly.ico"     optional; Windows icon resource (default: fly.ico)
```

Recognized fields: `project`, `name`, `version`, `source`, `output`, `deps`, `icon`, and an optional `test "tests/main.fly"` line. Unknown keys are ignored by the bootstrap reader. SLEEP is not JSON/TOML/YAML.

---

## 7. Toolchain CLI

### 7.1 `fly`

```text
fly                          launch the REPL (fly-repl)
fly -new <name>              scaffold a new project (fly -init <name> is an alias)
fly -compile <file>          compile a single .fly file; no project needed
fly -format <file>           deterministically format a .fly file
fly -deps [--offline]        install declared deps into module/
fly -build                   build flylink.sleep 'source' into 'output'
fly -run [args...]           build when needed, then run the project output
fly -test                    build and run the project's tests
fly -clean                   remove build artifacts (never source/deps)
fly -dump install "pkg"      install a package
fly -dump remove "pkg"       remove an installed package
fly -dump list               list installed packages
fly -dump update             re-fetch installed packages
fly -help  |  -h  |  --help     
fly -version  |  --version   
fly -up                      update the toolchain to the latest release
```

### 7.2 Compile modifiers

```text
fly -compile file.fly            compile and keep everything fly-cc produced
fly -compile file.fly -run       compile, run once, keep the artifact
fly -compile file.fly -done      compile, run once, clean transient .o/.rc artifacts, keep the binary
fly -compile file.fly -finish    compile, never run, clean transient artifacts, keep the binary (release-ready)
fly -compile file.fly -o name    output name (Windows normalizes to name.exe)
fly -compile file.fly -icon x.ico  embed a Windows icon resource (default: the toolchain fly.ico)
```

Modifier flags may appear before or after the file (the launcher scans all arguments). `-done` and `-finish` are mutually exclusive; `-run` and `-finish` are mutually exclusive.

For projects, `fly -build -finish` and `fly -run -done` apply the same transient-artifact cleanup to the project output.

### 7.3 `fly-cc`

The compiler front-end (`src/main.cpp` + `src/driver/driver.cpp`). Accepts source files plus flags including `-o <output>`, `-icon <file>`, and module resolution per §5.1. `fly` dispatches to the copy of `fly-cc` next to itself, falling back to PATH.

### 7.4 `fly-repl`

A statement-by-statement REPL over actual Fly semantics. Each input turn is compiled via `fly-cc` (it ships no LLVM backend of its own). It supports scripted stdin (used by the REPL CTest tests).

---

## 8. Runtime (`libflyrt`)

`fly-cc` emits native code that calls into `libflyrt`, a static C library (`runtime/*.c`, plus `eh.cpp` for the exception ABI). The runtime lib is found at runtime next to the executable, not baked in at fly-cc build time.

### 8.1 `FlyValue`

```c
typedef struct {
    int64_t tag;
    int64_t payload;
} FlyValue;
```

16 bytes total. `tag` selects the type; `payload` holds the integer/double-as-bits, or a pointer (cast through `intptr_t`) to a heap object.

### 8.2 Tags

| Tag        | Value | Payload |
|------------|-------|---------|
| `FLY_NUM`  | 0     | 64-bit integer value |
| `FLY_DEC`  | 1     | double, bit-cast |
| `FLY_YN`   | 2     | 0 or 1 |
| `FLY_EMP`  | 3     | unused |
| `FLY_TEX`  | 4     | `FlyText*` |
| `FLY_COLL` | 5     | `FlyColl*` |
| `FLY_BOARD`| 6     | `FlyBoard*` |
| `FLY_JOB`  | 7     | machine address of a `FlyValue(*)()` function |

Tag values must match between codegen and the runtime exactly.

### 8.3 Memory model

Fly uses **automatic reference counting (ARC)**. Each heap object begins with a `FlyObjHeader { uint32_t refcount }`. Rule: every expression temporary that is a heap value is "+1 owned"; the owner either transfers it into a binding/container slot (no extra retain) or releases it exactly once. `fly_rt_retain`/`fly_rt_release` are no-ops for scalar tags. Reference counts are non-atomic in this milestone. **Cycles are a known limitation.**

### 8.4 Runtime surface

`runtime/flyrt.h` declares the ABI seam codegen calls into. Key functions (92+ symbols in 18 source files):

- Value/ARC: `fly_rt_show`, `fly_rt_num`, `fly_rt_dec`, `fly_rt_yn`, `fly_rt_emp`, `fly_rt_retain`, `fly_rt_release`, `fly_rt_to_text`
- Arithmetic/logic: `fly_rt_add/sub/mul/div/mod/eq/neq/lt/gt/le/ge/and/or/not/neg`
- Text: `fly_rt_text_from_cstr`, `fly_rt_text_from_bytes`, `fly_rt_text_concat`, `fly_rt_char_from_code`, `fly_rt_char_code`, `fly_rt_cut/raise/lower`, `fly_rt_take`
- Interpolation: `fly_rt_strbuild_new/append_lit/append_value/finish`
- Coll/board: `fly_rt_coll_new/push`, `fly_rt_board_new/put/get/set/erase/has/count/key_at`, `fly_rt_attach/place/erase/count/seek/has/bind/sever/index/slice`
- Iteration: `fly_rt_iter_new/has_next/next/free`
- Casts: `fly_rt_cast_num`, `fly_rt_cast_dec`
- Error handling: `fly_rt_throw_value`, `fly_rt_throwv` (user `native job rt_throwv`), `fly_rt_begin_catch`, `fly_rt_end_catch`, `fly_rt_catch_extract`, `fly_rt_report_uncaught`, `fly_rt_gui_invoke_job`
- Process: `fly_rt_init_args`, `fly_rt_args`, `fly_rt_process_run/spawn/wait/exit`
- Filesystem: `fly_rt_file_exists/isfile/isdir/read/write/append/remove`, `fly_rt_dir_create/list`, `fly_rt_getcwd`, `fly_rt_chdir`
- Path: `fly_rt_path_join/basename/dirname/extension/stem/absolute/separator`
- Environment: `fly_rt_environment_get/has/set`
- System: `fly_rt_os/arch/hostname`
- Net: `fly_rt_net_resolve/connect/connect_tls/listen/accept/send/receive/close`
- GUI: `fly_rt_gui_window/label/button/textbox/add/on_click/run/close`
- Hash: `fly_sha256`, `fly_file_sha256` (OpenSSL-backed), also exposed through the `hashbox` public module

On Windows, flyrt builds with `-mabi=ms` (Microsoft ABI for `{i64,i64}` struct returns) and links `ws2_32`; TLS links OpenSSL `SSL`/`Crypto`.

---

## 9. Compiler architecture

```text
.fly
 ↓
Lexer (src/lexer)
 ↓
Parser (src/parser) — re-lexes interpolation spans via a sub-lexer
 ↓
AST (src/ast)
 ↓
Semantic analysis (src/sema) — hard reassignment, skip/getout placement, arity
 ↓
IR generation (src/codegen) — LLVM IR; builtins dispatch table; module aliases
 ↓
LLVM modules → linking → optimization → backend (x86, AArch64 targets)
 ↓
Linker (drives cc + libflyrt.a, optional icon resource)
 ↓
Native executable
```

`fly-cc` links the final executable against `libflyrt.a` and the platform runtime (e.g. on Windows: `-lgdi32 -luser32 -lcomctl32`, MinGW DLLs). Error unwinding uses the Itanium C++ ABI (`__cxa_throw`/`__cxa_begin_catch`) even on Windows (MSVC-style exceptions are not used).

---

## 10. Build system

Dependencies: CMake ≥ 3.20, a C++17 compiler (GCC/MinGW validated), LLVM via `find_package(LLVM CONFIG)` (verified with LLVM 17 through 22; set via `-DLLVM_DIR`), and OpenSSL (required).

```sh
cmake -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows
ctest --test-dir build-windows --output-on-failure
```

Targets:

| Target     | Kind         | Notes |
|------------|--------------|-------|
| `flyrt`    | static lib   | the runtime; PUBLIC links OpenSSL, ws2_32 on Windows |
| `fly-cc`   | executable   | compiler; links LLVM; depends on `flyrt` |
| `fly-repl` | executable   | REPL; depends on `fly-cc` |
| `fly`      | executable   | launcher + project commands; depends on `fly-repl` + `fly-cc` |

Build-time defines: `FLY_VERSION`, `FLY_UP_VERSION`, `FLY_UP_URL_BASE`, `FLY_UP_LATEST_URL`, `FLY_DUMP_MIRROR_DIR`, `FLY_DEFAULT_ICON`, `FLY_BUILTIN_STDLIB_DIR`, `FLY_GUI_CC_LINK_FLAGS`.

There are no committed build scripts; the repo has no CI configuration. Releases are produced manually:

- Windows: `windows/installer/` — NSIS installer `fly-installer-<ver>.exe` (per-user install to `$LOCALAPPDATA\Programs\Fly`, PATH management via generated `fly-path.ps1`, HKCU registry entries, Start Menu shortcuts, uninstaller) plus `Fly-<ver>.zip`.
- Linux: `linux/release/Fly-<ver>-linux.zip` (`fly`, `fly-cc`, `fly-repl`, `libflyrt.a`).

Platform support: **Windows** (primary; MinGW-w64 + Ninja validated) and **Linux** (secondary; GCC + Unix Makefiles validated). **macOS is not implemented yet.**

---

## 11. Testing

All 49 registered CTest tests are end-to-end. There are no unit tests yet (lexer/parser/sema golden tests are a documented TODO).

Test types:

- `add_e2e_test` (21) — compile an example `.fly`, run it, diff stdout against a `.expected` golden file (`.expected.win` variants on Windows where output differs).
- `add_negative_e2e_test` (3) — compile a fixture that MUST fail; assert an expected fragment in stderr (`cannot reassign hard variable`, `used outside of a loop`).
- Exit-code test (1) — assert a program exits with code 42.
- `add_repl_test` (11) — pipe a scripted `.input` into `fly-repl`; assert expected lines appear in order.
- Python-driven harnesses (13) — `net_tls_e2e` (local TLS fixture servers), `fresh_project_e2e`, `cli_toolchain_e2e`, `dump_install_e2e`, `icon_resources_e2e`, `take_e2e`, `release_layout_e2e`, `run_done_e2e`, `fly_up_e2e` (self-update regression vs. a local `latest.txt`), `gui_e2e`, `module_alias_diag_e2e`, `hashbox_e2e`, `compile_done_e2e`.

Run a single test: `ctest --test-dir build-windows -R fizzbuzz_e2e --output-on-failure`. List all: `ctest --test-dir build-windows -N`.

---

## 12. Updates

`fly -up` updates the Fly toolchain. The updater:

1. Reads the latest release's `latest.txt` (from `FLY_UP_LATEST_URL`, default the GitHub releases "latest/download" URL) to discover the newest version.
2. Compares it against the installed `FLY_VERSION` (`versionLess`).
3. Falls back to the baked-in `FLY_UP_VERSION` if the metadata fetch or parse fails.
4. Downloads and installs the platform-specific artifact.

`fly -up` is implemented differently per platform: on Windows it spawns a self-replacing child; on POSIX it forks a detached helper that waits for the parent to exit, then copies and chmods the replacement.

---

## 13. Known gaps and planned work

**Implemented in 0.1.3** as of writing: dynamic typing, `hard`, all core types, arithmetic/comparison/logical operators, interpolation, collections, boards, jobs, `native job`, conditions, `while`/`for`, `skip`/`getout`, `do`/`grab`, casting, `bring` + aliases, the full builtins table (§4), SLEEP manifests, `fly -deps`/`-dump` package flow, the formatter, `fly -up`, REPL, GUI (Win32 + optional GTK4), net/TLS, process/filesystem/path/environment/system APIs, SHA-256.

**Not yet implemented:**

- Binary/bitwise primitives and binary-safe byte handling for arbitrary data (zero bytes in the middle of text).
- A public ZIP module (planned to live at the public-module layer, needing DEFLATE/INFLATE). These features are **planned**, not shipped — do not build code against them.
- macOS support.
- Repository CI.
- Unit tests for lexer/parser/sema.
- A tracked `LICENSE` file: `README.md` refers to `LICENSE` (PolyForm Noncommercial License 1.0.0), but no such file is present in the repository. Until one is added, licensing terms must not be invented.