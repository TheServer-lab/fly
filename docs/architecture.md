# Fly Compiler Architecture (fly-cc)

**Target:** Native compiler for Fly 0.1, written in C++, backed by LLVM.

**Runtime model:** Tagged-union values, reference-counted heap objects, no tracing GC.

**Toolchain model:** `fly.exe` is the bootstrap/launcher, `fly-cc` is the compiler, `fly-repl` is the REPL, and `libflyrt` is the native runtime linked into compiled Fly programs.

---

## 1. Pipeline Overview

```text
.fly source

   │
   ▼

[Lexer]        → token stream

   ▼

[Parser]       → AST

   ▼

[Sema]         → checked/annotated AST
                  (scopes, hard-checks, type hints)

   ▼

[IRGen]        → LLVM IR (per-module)

   ▼

[LLVM opt]     → optimization passes (-O0..-O3)

   ▼

[LLVM backend] → object file (.o)

   ▼

[Linker]       → link against libflyrt
                  + required stdlib modules

   ▼

native executable in bin/
```

`fly -build` drives this pipeline using the local project's `flylink.sleep`.

`fly -run` uses the local `flylink.sleep` to determine which executable to run, building first when necessary.

Each `.fly` file becomes one LLVM `Module`. Modules are linked at the LLVM-IR level (`llvm::Linker`) before optimization, so cross-file inlining works and the runtime only needs one final codegen pass.

---

## 2. Value Representation

### 2.1 `FlyValue` — the universal runtime type

Since Fly is dynamically typed but compiled, every variable, function argument, and return value is a `FlyValue`: a fixed-size tagged union passed **by value** in registers (not boxed on the heap for scalars).

```c
typedef enum {

    FLY_NUM,    // int64_t
    FLY_DEC,    // double
    FLY_YN,     // bool (stored in same slot as num, 0/1)
    FLY_EMP,    // no payload
    FLY_TEX,    // heap: FlyText*
    FLY_COLL,   // heap: FlyColl*
    FLY_BOARD,  // heap: FlyBoard*

} FlyTag;

typedef struct {

    FlyTag tag;

    union {

        int64_t num;
        double  dec;
        bool    yn;
        void*   obj;   // FlyText* / FlyColl* / FlyBoard*

    } as;

} FlyValue;
```

`FlyValue` is 16 bytes on 64-bit targets.

In LLVM IR this is represented as a 16-byte aggregate. The compiler uses an explicit Fly calling convention rather than relying entirely on C ABI struct-return heuristics, keeping generated call sites predictable across targets.

### 2.2 Heap objects

```c
typedef struct {

    _Atomic uint32_t refcount;

    _Atomic uint64_t writer_task;

} FlyObjHeader;

typedef struct {

    FlyObjHeader hdr;

    size_t len;
    char* data;

} FlyText;

typedef struct {

    FlyObjHeader hdr;

    size_t len, cap;

    FlyValue* items;

} FlyColl;

typedef struct {

    FlyObjHeader hdr;

    size_t len, cap;

    FlyValue* keys;
    FlyValue* vals;

} FlyBoard;
```

`FlyText` is immutable.

Literal text may be interned and safely shared.

### 2.3 Board key hashing

`num`, `dec`, `yn`, `tex`, and `emp` are hashable.

`coll` and `board` are not hashable.

Equal keys must always hash equally.

| Tag              | Hash rule                                                                                                                       |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `num`            | Hash raw `int64_t` bits directly, such as through splitmix64/FNV over the 8 bytes.                                              |
| `dec`            | Normalize `-0.0` to `0.0` before hashing. Reject `NaN` keys at runtime because `NaN != NaN`.                                    |
| `yn`             | Hash `0` or `1`.                                                                                                                |
| `tex`            | Hash UTF-8 contents. FNV-1a is acceptable; SipHash is preferred for untrusted input.                                            |
| `emp`            | Constant hash.                                                                                                                  |
| `coll` / `board` | Not hashable. Statically known invalid keys are compile-time errors; dynamic invalid keys are runtime errors caught by `grabe`. |

This is implemented by:

```c
uint64_t fly_rt_hash(FlyValue value);
bool fly_rt_keys_equal(FlyValue a, FlyValue b);
```

in `runtime/board.c`.

---

## 2.4 Memory management: ARC, not GC

* Every heap `FlyValue` (`TEX`, `COLL`, `BOARD`) is reference-counted.
* IRGen inserts `fly_rt_retain()` / `fly_rt_release()` calls at well-defined points.
* `hard` does not change refcounting rules.
* Reference cycles are a documented v0.1 limitation.
* Refcounts are atomic because concurrency is required.
* Sema/IRGen may elide retain/release pairs in provably local, non-escaping cases.

---

# 3. Compiler Stages in Detail

## 3.1 Lexer

Hand-written, single-pass, no external lexer generator.

Notable rules:

* `$` → line comment.
* `$$ ... $$` → block comment.
* String interpolation is represented structurally so `{...}` expressions can be parsed correctly.
* Keywords are lowercase and case-sensitive.
* `Yes`, `No`, and `EMP` are literal tokens with exact capitalization.

## 3.2 Parser

Recursive-descent with Pratt expression parsing.

AST families include:

* `Program`
* `FunctionDecl (job)`
* `VarDecl`
* `If/Orif/Ifnot`
* `While`
* `For`
* `Return (give)`
* `ExprStmt`
* `DoGrabe`
* `Literal`
* `Ident`
* `BinaryOp`
* `UnaryOp`
* `Call`
* `Index`
* `Slice`
* `TextTemplate`
* `CollLiteral`
* `BoardLiteral`

Every AST node carries source position information.

## 3.3 Semantic Analysis (Sema)

Sema is lightweight because Fly is dynamically typed.

Responsibilities:

1. Scope resolution.
2. `hard` enforcement.
3. Local type hints for optimization.
4. `job` arity checks.
5. Basic dead-code/unreachable warnings.
6. Validation of native/runtime declarations where applicable.

Sema does not eliminate the runtime `FlyValue` tag.

## 3.4 IR Generation

IRGen emits LLVM IR against the `FlyValue` ABI.

Cheap scalar operations may use inline LLVM IR with runtime fallback.

Heap operations and dynamic operations use `libflyrt` calls.

String interpolation lowers through `fly_rt_strbuild`.

`for` lowers against the internal `FlyIterator` protocol.

### 3.4.1 `num` overflow

`num` is signed 64-bit.

**Overflow is a runtime error, never silent wrapping or promotion.**

LLVM checked arithmetic intrinsics are used for integer overflow detection.

Overflow is converted into a `NUM_OVERFLOW` Fly error.

`dec` retains IEEE-754 floating-point semantics.

## 3.5 Error handling (`do` / `grabe`)

Fly uses LLVM exception machinery with `invoke` / `landingpad` and `libunwind`.

Runtime errors call `fly_rt_throw`.

Unhandled errors propagate to the top-level program entry and terminate with an error message and nonzero exit code.

## 3.6 Modules (`bring`)

`bring` resolves modules from:

1. Project-local `src/`.
2. Bundled Fly standard library.
3. Packages declared in the local `flylink.sleep` and installed under the project's `module/`.

Compiled modules may be cached as LLVM bitcode keyed by source hash.

---

# 4. Runtime Library (`libflyrt`)

`libflyrt` is the low-level native runtime used by compiled Fly programs.

It is written in C and exposes a stable C ABI through `flyrt.h`.

The runtime is responsible for:

* `FlyValue` operations
* memory management
* text
* collections
* boards
* arithmetic helpers
* casts
* string building
* I/O
* errors
* iteration
* process interaction
* filesystem access
* environment access
* system information
* concurrency

## 4.1 Runtime layout

```text
runtime/

├── flyrt.h

├── value.c/h
├── text.c/h
├── coll.c/h
├── board.c/h
├── numeric.c/h
├── cast.c/h
├── strbuild.c/h
├── io.c/h
├── error.c/h
├── iter.c/h
├── entry.c

├── process.c/h
├── filesystem.c/h
├── path.c/h
├── environment.c/h
├── system.c/h

├── sched.c/h
├── task.c/h
├── guard.c/h
├── chan.c/h
└── sync.c/h
```

---

# 5. Native System Interface

Fly's high-level standard library needs controlled access to the operating system.

The initial implementation provides native runtime functions through `libflyrt`.

The public Fly modules are:

```text
stdlib/

├── process.fly
├── filesystem.fly
├── path.fly
├── environment.fly
└── system.fly
```

These modules may be thin Fly wrappers around native `libflyrt` functions.

The native runtime remains the implementation boundary.

---

## 5.1 Process API

The runtime exposes operations equivalent to:

```c
FlyValue fly_rt_args(void);

FlyValue fly_rt_process_run(
    FlyValue program,
    FlyValue args
);

FlyValue fly_rt_process_spawn(
    FlyValue program,
    FlyValue args
);

FlyValue fly_rt_process_wait(
    FlyValue child
);

FlyValue fly_rt_process_exit(
    FlyValue code
);
```

The corresponding Fly module exposes:

```fly
bring process

job args() {
    give rt_args()
}

job run(program, args) {
    give rt_process_run(program, args)
}

job spawn(program, args) {
    give rt_process_spawn(program, args)
}

job wait(child) {
    give rt_process_wait(child)
}

job exit(code) {
    rt_process_exit(code)
}
```

The exact native declaration syntax is implementation-defined until the compiler's native-function mechanism is finalized.

---

## 5.2 Filesystem API

The runtime exposes:

```c
FlyValue fly_rt_file_exists(FlyValue path);
FlyValue fly_rt_file_isfile(FlyValue path);
FlyValue fly_rt_file_isdir(FlyValue path);

FlyValue fly_rt_file_read(FlyValue path);
FlyValue fly_rt_file_write(FlyValue path, FlyValue text);
FlyValue fly_rt_file_append(FlyValue path, FlyValue text);

FlyValue fly_rt_dir_create(FlyValue path);
FlyValue fly_rt_file_remove(FlyValue path);
FlyValue fly_rt_dir_list(FlyValue path);

FlyValue fly_rt_getcwd(void);
FlyValue fly_rt_chdir(FlyValue path);
```

The public Fly module provides:

```fly
bring filesystem

job exists(path) {
    give rt_file_exists(path)
}

job isfile(path) {
    give rt_file_isfile(path)
}

job isdir(path) {
    give rt_file_isdir(path)
}

job read(path) {
    give rt_file_read(path)
}

job write(path, text) {
    give rt_file_write(path, text)
}

job append(path, text) {
    give rt_file_append(path, text)
}

job mkdir(path) {
    give rt_dir_create(path)
}

job remove(path) {
    give rt_file_remove(path)
}

job list(path) {
    give rt_dir_list(path)
}

job cwd() {
    give rt_getcwd()
}

job chdir(path) {
    give rt_chdir(path)
}
```

---

## 5.3 Path API

Path manipulation is exposed through the `path` standard-library module.

The implementation must use platform-appropriate path rules.

Core operations include:

```text
path.join(...)
path.basename(...)
path.dirname(...)
path.extension(...)
path.stem(...)
path.absolute(...)
path.separator()
```

Path manipulation should not require programs to hard-code platform separators.

---

## 5.4 Environment API

The runtime exposes:

```c
FlyValue fly_rt_environment_get(FlyValue name);
FlyValue fly_rt_environment_has(FlyValue name);
FlyValue fly_rt_environment_set(
    FlyValue name,
    FlyValue value
);
```

The Fly module provides:

```fly
bring environment

job get(name) {
    give rt_environment_get(name)
}

job has(name) {
    give rt_environment_has(name)
}

job set(name, value) {
    give rt_environment_set(name, value)
}
```

---

## 5.5 System API

The runtime exposes:

```c
FlyValue fly_rt_os(void);
FlyValue fly_rt_arch(void);
FlyValue fly_rt_hostname(void);
```

The Fly module provides:

```fly
bring system

job os() {
    give rt_os()
}

job arch() {
    give rt_arch()
}

job hostname() {
    give rt_hostname()
}
```

---

# 6. Fly REPL

The Fly REPL is a **separate executable** from `fly.exe`.

Initial name:

```text
fly-repl.exe
```

The REPL is not implemented by `fly.exe`.

Its responsibility is to provide an interactive Fly programming environment.

Example:

```text
fly
```

causes `fly.exe` to locate and launch:

```text
fly-repl.exe
```

The REPL may eventually be implemented in Fly itself, but the initial implementation may be written in C++.

## 6.1 REPL responsibilities

The REPL handles:

* interactive source input
* parsing expressions/statements
* compiling submitted code
* evaluating or executing submitted code
* displaying results
* reporting errors
* maintaining an interactive session

The REPL may use `fly-cc` and `libflyrt` internally.

Conceptually:

```text
fly.exe
   │
   ▼
fly-repl.exe
   │
   ├── parser/compiler services
   │
   └── libflyrt
```

---

# 7. Fly Bootstrap (`fly.exe`)

`fly.exe` is the **bootstrap and toolchain launcher**.

It is installed into the system `PATH`.

`fly.exe` is **not**:

* the compiler
* the runtime
* the REPL
* the standard library
* the package registry

It is the stable command-line entry point for the Fly toolchain.

## 7.1 Responsibilities

`fly.exe` is responsible for:

1. locating the installed Fly toolchain
2. locating `fly-cc`
3. locating `fly-repl`
4. dispatching commands
5. forwarding arguments
6. handling toolchain-level commands
7. reporting missing or broken components

## 7.2 Command behavior

### REPL

```text
fly
```

Launches:

```text
fly-repl.exe
```

### Help

```text
fly -help
```

Displays Fly command information directly from `fly.exe`.

### Version

```text
fly -version
```

Displays installed Fly toolchain information.

### Single-file compilation

```text
fly -compile path/to/file.fly [-icon icon.ico]
```

Invokes `fly-cc` to compile one source file.

`-compile` does not require `flylink.sleep`.

The optional `-icon <path>` embeds the given Windows `.ico` as the
executable's icon resource, overriding both the project `flylink.sleep`
`icon` field and the toolchain default. Relative paths resolve against the
current directory (paths with spaces are supported). Without `-icon`, the
toolchain default `fly.ico` is embedded, matching `fly -build`. A missing
icon file fails with a clear diagnostic.

### Source checking

```text
fly -check path/to/file.fly
```

Runs parsing and semantic analysis without producing a final executable.

### Project building

```text
fly -build
```

Reads the local `flylink.sleep` and invokes the compiler using that
configuration.

On Windows the executable gets an embedded icon resource: the manifest's
`icon "assets/x.ico"` when set (a missing configured icon fails the build
with a clear error -- no silent fallback), otherwise the toolchain default
`fly.ico`. Changing the configured icon invalidates the output the same way
a source edit does, so `fly -run` relinks.

### Project execution

```text
fly -run
```

Reads the local `flylink.sleep`, builds the project when necessary, and executes the configured output.

### Dependency resolution

```text
fly -deps
```

Reads dependencies from the local `flylink.sleep` and ensures the required packages exist in `module/`.

### Dump

```text
fly -dump install "package"
fly -dump remove "package"
fly -dump list
fly -dump update
```

Performs package operations through Dump.

### Project creation

```text
fly -init "project_name"
```

Creates a new Fly project structure and initial `flylink.sleep`.

### Formatting

```text
fly -format path/to/file.fly
```

Formats Fly source using the official formatter.

### Testing

```text
fly -test
```

Runs project tests.

### Cleaning

```text
fly -clean
```

Removes project build artifacts.

### Toolchain update

```text
fly -up
```

Updates the installed Fly toolchain.

### Toolchain uninstall

```text
fly -uninstall
```

Uninstalls the Fly toolchain.

---

# 8. Bootstrap Architecture

The installed toolchain conceptually looks like:

```text
Fly/
├── fly.exe
│
├── compiler/
│   └── fly-cc.exe
│
├── repl/
│   └── fly-repl.exe
│
├── runtime/
│   └── libflyrt.*
│
├── stdlib/
│   ├── process.fly
│   ├── filesystem.fly
│   ├── path.fly
│   ├── environment.fly
│   └── system.fly
│
└── tools/
```

The user only needs:

```text
fly
```

on the command line.

`fly.exe` finds the internal components.

---

# 9. `flylink.sleep`

Every buildable Fly project has a local:

```text
flylink.sleep
```

This is the project's central configuration file.

It combines:

* project metadata
* source configuration
* output configuration
* dependency declarations

Example:

```sleep
$ Project configuration

project
  name "my_project"
  version "0.1.0"
  source "src/main.fly"
  output "bin/my_project.exe"
  deps coll [
    "http"
    "json"
  ]
  icon "assets/fly.ico"
```

The `deps` field is a `coll` containing package names, not repository URLs.

The `icon` field names a Windows `.ico` file (project-relative) to embed as
the executable's icon resource (RT_GROUP_ICON) when `fly -build` or `fly
-compile` runs. Omitting the field uses the toolchain default `fly.ico`
automatically. On Linux builds the icon is accepted and validated but never
embedded (there are no PE resources on Linux).

---

# 10. Compiler Project Layout

The **Fly compiler source repository** is separate from normal Fly project repositories.

```text
fly-cc/

├── CMakeLists.txt

├── include/flycc/
│   ├── lexer.h
│   ├── parser.h
│   ├── ast.h
│   ├── sema.h
│   ├── irgen.h
│   └── diagnostics.h

├── src/
│   ├── lexer/
│   ├── parser/
│   ├── ast/
│   ├── sema/
│   ├── irgen/
│   ├── driver/
│   └── main.cpp

├── runtime/
│
├── stdlib/
│
├── repl/
│   ├── repl.cpp
│   └── ...
│
├── bootstrap/
│   ├── fly.cpp
│   └── ...
│
├── tests/
│
└── docs/
```

The compiler repository therefore contains the initial implementations of:

```text
fly-cc
fly-repl
fly.exe
libflyrt
stdlib
```

plus the repository `fly.ico` (the toolchain's default icon resource).

On Windows, every executable the repository CMake build produces -- `fly-cc`,
`fly-repl`, and `fly.exe` -- embeds `fly.ico` as a PE icon resource (a per-
target `.rc` compiled by the platform resource compiler via CMake's RC
language support). Executables that `fly -build` / `fly -compile` produce
embed their icon the same way through `fly-cc`'s `-icon` option (see §9 and
§11); on Linux the option is accepted and ignored and no PE resources are
ever written.

while ordinary Fly project repositories do not.

---

# 11. `fly -build`

```text
fly -build
```

operates on the local `flylink.sleep`.

Example:

```text
source "src/main.fly"
output "bin/my_project.exe"
```

The build flow is:

```text
fly.exe
   │
   ▼
flylink.sleep
   │
   ▼
fly-cc
   │
   ▼
LLVM
   │
   ▼
link libflyrt + stdlib
   │
   ▼
bin/my_project.exe
```

---

# 12. `fly -run`

```text
fly -run
```

reads the local project's `flylink.sleep`, determines the configured executable, builds when necessary, and launches it.

Example:

```text
output "bin/my_project.exe"
```

produces:

```text
fly -run
      │
      ▼
bin/my_project.exe
```

---

# 13. `fly -compile`

```text
fly -compile path/to/file.fly
```

compiles a single Fly source file without requiring a project manifest.

The basic flow is:

```text
file.fly
   │
   ▼
fly.exe
   │
   ▼
fly-cc
   │
   ▼
native executable
```

This mode is intended for quick programs, experiments, scripts, and simple standalone source files.

---

# 14. Dump Package Repository

**Dump** is Fly's package repository.

The conceptual relationship is:

```text
Dump
 │
 │ package lookup
 ▼
fly -dump
 │
 ├── install
 ├── remove
 ├── list
 └── update
 │
 ▼
flylink.sleep
 │
 ▼
module/
```

The exact Dump registry layout, package metadata format, version resolution, archive format, and release mechanism remain separate design questions.

---

# 15. Standard Library and Runtime Boundary

The architecture intentionally separates:

```text
high-level standard library
```

from:

```text
low-level native runtime
```

For example:

```text
filesystem.fly
      │
      ▼
libflyrt filesystem API
      │
      ▼
Operating system
```

and:

```text
process.fly
      │
      ▼
libflyrt process API
      │
      ▼
Operating system
```

This keeps platform-specific implementation details out of ordinary Fly source.

---

# 16. Self-Hosting

The long-term Fly toolchain is intended to become self-hosting.

The initial bootstrap is:

```text
C++ fly.exe
C++ fly-cc
C++ fly-repl
C libflyrt
Fly stdlib
```

As Fly becomes more capable, high-level toolchain components may be rewritten in Fly.

A major early target is `fly.exe`.

The required relationship is:

```text
Fly source
   │
   ├── process module
   ├── filesystem module
   ├── path module
   ├── environment module
   └── system module
            │
            ▼
         libflyrt
            │
            ▼
             OS
```

This provides enough operating-system functionality for a Fly implementation of the bootstrap to:

* inspect command-line arguments
* locate toolchain components
* read `flylink.sleep`
* inspect project directories
* launch `fly-cc`
* launch `fly-repl`
* launch built executables
* return process exit codes
* manipulate files and directories

The initial `fly.exe` may therefore be replaced by a Fly implementation once the compiler and standard library are sufficiently mature.

---

# 17. Self-Hosting Transition

The intended progression is:

```text
Stage 1

C++ fly-cc
    │
    ▼
Fly programs
```

then:

```text
Stage 2

C++ fly-cc
    │
    ├── compiles stdlib
    ├── compiles fly.exe written in Fly
    └── compiles applications
```

then eventually:

```text
Stage 3

Fly source
    │
    ├── compiler
    ├── bootstrap
    ├── REPL
    └── standard library
```

The C/C++ runtime boundary can remain in place for low-level operating-system and machine functionality.

---

# 18. Calling Convention Summary

| Aspect        | Decision                                                |
| ------------- | ------------------------------------------------------- |
| Value passing | `FlyValue` (16-byte struct) by value                    |
| Return values | `FlyValue` by value                                     |
| Errors        | LLVM `invoke` / `landingpad`, libunwind-based           |
| Heap objects  | Refcounted, ARC-style                                   |
| Scalars       | Never heap-allocated                                    |
| Runtime ABI   | Stable C ABI through `flyrt.h`                          |
| OS access     | Native runtime functions exposed through stdlib modules |
| REPL          | Separate executable                                     |
| Bootstrap     | Separate executable                                     |
| Compiler      | Separate executable                                     |

---

# 19. Concurrency Model

Confirmed as a required feature: high-level concurrent execution with structured tasks.

The architecture is layered:

```text
Core language
    ↓
Tasks / concurrency primitives
    ↓
Sync + channels
    ↓
Standard library
```

## 19.1 Tasks

* M:N cooperative scheduler.
* Pool of OS worker threads.
* Lightweight Fly tasks with growable stacks.
* Structured task lifetime.
* Parent implicitly waits for children.

## 19.2 Mutation guard

Unsynchronized concurrent mutation of a shared `coll` / `board` is a **runtime error**, never undefined behavior.

Objects carry an atomic `writer_task` field.

Mutating operations use:

```c
fly_rt_mutate_enter(...)
fly_rt_mutate_exit(...)
```

A conflicting mutation raises a catchable concurrent-mutation error.

Read/write races are currently outside this guard's scope.

## 19.3 Cancellation

Cancellation propagates from parent to children.

Children stop at defined cancellation points, including:

* task scheduling points
* channel operations
* loop boundaries

The parent's structured join waits for its children to stop.

## 19.4 Error propagation

Child-task errors are captured by the child task and re-thrown at the structured join point in the parent.

## 19.5 Sync + channels

Channels and synchronization primitives remain standard-library types rather than dedicated grammar.

A future `concurrency` module may expose:

```text
Channel
Lock
```

with operations implemented as ordinary Fly `job`s.

## 19.6 Runtime components

```text
runtime/

├── sched.c/h
├── task.c/h
├── guard.c/h
├── chan.c/h
├── sync.c/h
└── error.c
```

## 19.7 Codegen consequences

* Refcounts are atomic.
* Heap objects carry mutation ownership state.
* Text interning must become concurrency-safe.
* Cross-task exceptions use explicit error capture and rethrow rather than cross-stack unwinding.

## 19.8 Still open

* Task-spawn syntax.
* Structured-join syntax.
* Exact cancellation points.
* Whether channels or locks ever receive dedicated syntax.

---

# 20. Phased Roadmap

1. **Bootstrap:** lexer + parser + AST pretty-printer.
2. **Sema v0:** scopes, `hard`, undefined identifiers.
3. **Codegen v0:** scalar types, arithmetic, control flow, functions, `show`.
4. **Heap types:** `tex`, interpolation, `coll`, `board`, ARC.
5. **Error handling:** `do` / `grabe`.
6. **Iterators, casting, and modules.**
7. **Runtime system APIs:** process, filesystem, path, environment, system.
8. **REPL:** `fly-repl`.
9. **Toolchain UX:** `flylink.sleep`, `fly -build`, `fly -run`, `fly -compile`, `fly -deps`.
10. **Dump integration:** `fly -dump install`, `remove`, `list`, `update`.
11. **Optimization passes.**
12. **Toolchain management:** `fly -up`, `fly -uninstall`.
13. **Standard library/package ecosystem.**
14. **Rewrite `fly.exe` in Fly.**
15. **Further self-hosting:** progressively move more toolchain components into Fly.

---

# 21. Open Design Questions

* **Cycle collection:** deferred until real Fly programs demonstrate a need.
* **Read/write race guarding:** currently not covered by the mutation guard.
* **Task-escape analysis:** whether provable races can eventually become compile-time errors.
* **Cancellation point granularity.**
* **`Channel` / `Lock` syntax vs stdlib types.**
* **Exact `flylink.sleep` schema:** additional project/build fields may be added.
* **Dump package format:** exact registry layout, package metadata, versions, and archive format remain open.
* **Dump version resolution:** exact rules for selecting package versions are not yet frozen.
* **Native declaration syntax:** exact Fly syntax for exposing `libflyrt` functions to stdlib source.
* **REPL implementation:** exact C++ architecture and whether/when it is rewritten in Fly.
* **Bootstrap implementation:** initial C++ implementation and exact transition to a Fly implementation.
* **Toolchain component discovery:** exact installation-directory and environment-variable rules.
* **Cross-platform runtime layer:** exact Windows/Linux/macOS system-call abstractions.
