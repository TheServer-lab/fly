---

name: fly-development
description: Work on, explain, debug, test, and develop the Fly programming language and its toolchain. Use this skill whenever modifying Fly compiler/runtime/toolchain code, creating Fly programs or public modules, working with flylink.sleep manifests or SLEEP, investigating Fly APIs, building/testing Fly, or answering implementation questions about Fly. Fly is a distinct language and toolchain; do not pattern-match from Python, JavaScript, C++, Rust, or other languages. Inspect the actual repository, tests, and implementation before assuming syntax, APIs, module locations, SLEEP behavior, CLI behavior, or feature availability.
---

# Fly Development Skill

## 1. Purpose

This skill provides the practical knowledge required to work on the **actual Fly repository** correctly.

Fly is:

* High-level
* Dynamically typed
* General-purpose
* Compiled to native code
* LLVM-based
* Toolchain-oriented
* Equipped with integer bitwise operators (`band`, `bor`, `bxor`, `bnot`, `shl`, `shr`)
* Designed around a small language and a large ecosystem

Fly's design principle is:

> **One language. One toolchain. One ecosystem.**

The purpose of this skill is not merely to teach Fly syntax. It is to prevent incorrect assumptions while developing the compiler, runtime, toolchain, standard/built-in functionality, public modules, tests, manifests, and documentation.

---

# 2. Absolute Rule: Do Not Guess Fly

**Never invent Fly syntax, APIs, CLI flags, SLEEP syntax, module behavior, repository paths, or runtime functionality from familiarity with another language.**

Fly intentionally resembles high-level languages in some ways, but it is **not Python-compatible** and does not use arbitrary Python/JavaScript/C++ conventions.

For every implementation task:

```text
Question
  ↓
Inspect repository
  ↓
Inspect existing implementation
  ↓
Inspect tests
  ↓
Confirm behavior
  ↓
Implement
  ↓
Build
  ↓
Test
```

Do not skip the repository inspection step when the answer can be obtained from the source.

If this skill conflicts with the actual repository implementation:

> **The actual implementation and passing tests are authoritative for implemented behavior.**

If implementation, tests, and documentation disagree, identify the disagreement explicitly instead of silently choosing one.

---

# 3. Implemented vs Planned

Always distinguish between:

```text
IMPLEMENTED
EXPERIMENTAL
PROPOSED
PLANNED
```

Never write code against a planned feature as though it already exists.

Never tell a user that a feature exists merely because it appears in a roadmap or design document.

For a requested capability:

1. Search the repository.
2. Check compiler/runtime support.
3. Check modules.
4. Check tests.
5. Determine whether the capability is actually available.

If it is missing, say so.

Then determine which layer should provide it:

```text
language
compiler
runtime
built-in module
public module
toolchain
```

Do not automatically add a compiler/runtime feature merely because a higher-level module currently cannot implement something.

---

# 4. Fly Repository

Before modifying Fly, inspect the repository's real structure.

Do not assume that an expected directory exists.

The repository may contain areas such as:

```text
src/
dump/
dump-mirror/
stdlib/
runtime/
include/
examples/
tests/
docs/
windows/
linux/
build-windows/
build-linux/
```

The `module/` directory is **per-project** (created by `fly -deps` / `fly -dump install`), NOT a repository area. Public modules' upstream source lives in `dump/public/` (with local fallback mirrors in `dump-mirror/`).

The actual checkout is authoritative.

When looking for functionality:

```text
language syntax
→ compiler source

runtime behavior
→ runtime source

CLI behavior
→ fly/toolchain source

public modules
→ dump/public/ (upstream)
→ dump-mirror/ (local fallback)
→ <project>/module/ (installed per-project)

test behavior
→ tests/

project configuration
→ flylink.sleep
```

Use actual repository paths rather than paths invented from generic project layouts.

---

# 5. Obtaining the Fly Toolchain

A coding agent must know how to obtain a usable Fly compiler before attempting to validate Fly source.

The repository's documented bootstrap/build procedure is authoritative.

First inspect:

* build scripts
* CMake configuration
* README/build documentation
* platform-specific instructions
* bootstrap scripts
* existing compiler targets

The important executables are:

```text
fly
fly-cc
fly-repl
```

Do not assume they are already installed globally.

If:

```text
which fly
which fly-cc
```

returns nothing, that does **not** mean Fly cannot be built. It means the agent must locate the repository's build procedure and build the toolchain.

On Windows, inspect the repository's Windows build configuration and generated build directory.

On Linux/WSL, inspect the repository's Linux build configuration.

Do not fabricate package-manager commands or installation steps.

---

# 6. Building Fly

Fly is built from the repository.

The primary developer-facing tool is:

```text
fly
```

The native compiler is:

```text
fly-cc
```

The REPL is:

```text
fly-repl
```

Project commands include:

```text
fly -build
fly -test
fly -run
fly -format
fly -clean
```

File compilation includes:

```text
fly -compile file.fly
```

The exact accepted flag order and behavior must be verified from the current implementation/tests.

Do not assume command-line argument ordering from another CLI framework.

For example, the project intentionally supports:

```text
fly -compile file.fly -run
```

and:

```text
fly -compile -run file.fly
```

where supported by the current implementation.

---

# 7. Testing Fly

Do not claim tests pass unless tests were actually executed.

The repository contains unit/integration/end-to-end tests.

Typical project testing uses:

```text
fly -test
```

Repository-level testing may additionally use CMake/CTest.

For example:

```text
cmake --build ...
ctest --output-on-failure
```

but use the repository's actual build directories and commands.

When reporting results, distinguish:

```text
Executed test
```

from:

```text
Manually reasoned / traced test
```

Manual reasoning is **not** equivalent to a passing test.

---

# 8. Fly Syntax: Core Vocabulary

Fly intentionally uses its own vocabulary.

The following are important:

| Familiar concept    | Fly      |
| ------------------- | -------- |
| function definition | `job`    |
| return              | `give`   |
| print/output        | `show`   |
| input               | `take`   |
| else-if             | `orif`   |
| else                | `ifnot`  |
| continue            | `skip`   |
| break               | `getout` |
| try                 | `do`     |
| catch               | `grab`   |
| import              | `bring`  |
| true                | `Yes`    |
| false               | `No`     |
| null/none           | `EMP`    |

Do not replace Fly words with familiar equivalents.

For example:

```fly
skip
```

is correct Fly loop-control syntax.

Do **not** generate:

```fly
continue
```

Likewise:

```fly
getout
```

is Fly's loop exit operation, not `break`.

---

# 9. Fly Blocks

Fly uses braces for blocks:

```fly
if ready {
    show("Ready")
}
```

Indentation improves readability but is not a replacement for block delimiters.

Do not generate Python-style indentation-only blocks.

---

# 10. Comments

Single-line comments begin with `$`:

```fly
$ comment
name = "Rick" $ inline comment
```

Multiline comments use `$$`:

```fly
$$
This is a multiline comment.
$$
```

Do not assume `#` or `//` are Fly comments.

---

# 11. Variables

Fly is dynamically typed.

Variables do not require explicit type declarations:

```fly
value = 42
value = "Hello"
value = Yes
```

Variables are mutable by default.

Immutable variables use:

```fly
hard value = 42
```

Reassignment of a `hard` binding is an error.

---

# 12. Core Fly Types

The core value model includes:

```text
tex
num
dec
yn
coll
board
emp
```

Boolean literals:

```text
Yes
No
```

Empty value:

```text
EMP
```

Type names normally appear as conversion operations rather than declaration keywords.

Example:

```fly
age = num("18")
price = dec("19.99")
text = tex(123)
```

---

# 13. Strings and Interpolation

Text uses double quotes:

```fly
message = "Hello"
```

Interpolation uses:

```fly
show("Hello, {name}")
```

The expression inside `{}` is evaluated when the text is created.

Function calls are valid interpolation expressions:

```fly
show("Result: {add(10, 5)}")
```

Literal braces use:

```text
{{
}}
```

Example:

```fly
show("Use {{name}} literally.")
```

produces:

```text
Use {name} literally.
```

Do not import string interpolation syntax from another language.

---

# 14. Collections

Collections are ordered and may contain mixed types:

```fly
items = [10, "hello", Yes, 3.14]
```

Indexing is zero-based:

```fly
items[0]
```

Slicing is supported:

```fly
items[1:4]
items[:3]
items[2:]
items[:]
```

The start is inclusive and the end is exclusive.

---

# 15. Collection APIs

Collection operations are command-oriented functions:

```fly
attach(items, value)
place(items, index, value)
erase(items, index)
count(items)
seek(items, value)
has(items, value)
bind(items, separator)
sever(text, separator)
cut(text)
raise(text)
lower(text)
```

Examples:

```fly
attach(items, "New")
place(items, 1, "Inserted")
erase(items, 2)
count(items)
seek(items, "Alex")
has(items, "Alex")
```

Text also supports:

```fly
seek("Hello World", "World")
```

No match may produce:

```text
EMP
```

Objects supporting the length API may also expose:

```fly
items.length()
name.length()
```

Do not replace these with assumed APIs such as:

```text
len(items)
items.count()
```

unless those APIs are actually implemented.

---

# 16. Boards

Boards represent key/value data:

```fly
person = {
    "name": "Rick"
    "age": 18
}
```

The exact board grammar and key semantics are defined by the compiler/runtime implementation.

When making board-related changes, inspect parser, semantic analysis, runtime, and tests rather than assuming dictionary syntax from another language.

---

# 17. Functions

Fly functions are declared with `job`:

```fly
job add(a, b) {
    give a + b
}
```

Call them normally:

```fly
result = add(10, 20)
```

Functions do not require explicit parameter or return-type declarations.

A `job` that reaches the end without `give` produces `EMP`.

---

# 18. Conditions

Fly conditionals use:

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

Interpret them as:

```text
if
else if
else
```

but use the actual Fly vocabulary when writing Fly code.

---

# 19. Loops

`while`:

```fly
while condition {
    ...
}
```

`for`:

```fly
for item in items {
    show(item)
}
```

Iteration semantics depend on the iterable value.

Loop control:

```fly
skip
getout
```

`skip` skips the current iteration.

`getout` exits the current loop.

---

# 20. Error Handling

Fly uses:

```fly
do {
    ...
}
grab (err) {
    ...
}
```

Runtime errors can propagate outward.

Example:

```fly
do {
    value = num("hello")
}
grab (err) {
    show("Conversion failed.")
}
```

Do not silently convert errors into `EMP` unless the actual API specifies that behavior.

Do not invent a `finally` construct.

---

# 21. Modules: Two Different Categories

Fly has two important module categories:

```text
Built-in/runtime-backed modules
Public Fly modules
```

These are **not the same thing**.

---

# 22. Built-in Modules

Built-in modules are supported by Fly's compiler/runtime/toolchain.

Current built-in functionality includes areas such as:

```text
net
filesystem
process
environment
system
path
gui
```

The compiler's builtins table (`src/codegen/codegen.cpp`) also includes a broader set of runtime-backed dotted calls:

```text
path.basename        path.dirname        path.extension     path.stem
path.absolute        path.separator      path.join

filesystem.exists    filesystem.isfile   filesystem.isdir   filesystem.read
filesystem.write     filesystem.append   filesystem.mkdir   filesystem.remove
filesystem.list      filesystem.cwd      filesystem.chdir

process.args         process.run         process.spawn      process.wait
process.exit

environment.get      environment.exists  environment.set

system.os            system.arch         system.hostname

net.resolve          net.connect         net.connect_tls    net.listen
net.accept           net.send            net.receive        net.close

gui.window           gui.label           gui.button         gui.textbox
gui.run              gui.close
```

Examples include:

```fly
filesystem.read(path)
filesystem.write(path, text)
filesystem.list(path)

filesystem.isfile(path)
filesystem.isdir(path)

path.join(a, b)

process.run(command)

environment.get(name)
```

These names must be verified against the actual implementation before adding new code.

Do not rename APIs merely to make them look conventional.

For example:

```text
filesystem.isfile
filesystem.isdir
```

are different from:

```text
filesystem.is_file
filesystem.is_dir
```

Only use whichever the implementation actually provides.

---

# 23. Public Fly Modules

A public module is **separately distributed Fly source code**.

A public module is not automatically runtime functionality.

A project may contain a local/public module tree such as:

```text
module/
```

with Fly source files.

Example:

```text
module/
└── http.fly
```

A program can import it:

```fly
bring http
```

Public modules are part of the Fly ecosystem and may be installed through the package system.

---

# 24. `/module` Rules

When modifying or creating a public module:

1. Inspect the actual repository's `dump/public/` structure (the upstream public modules) and `dump-mirror/` (the local fallback copies).
2. Inspect how `bring` resolves module names (the resolve order in `src/driver/driver.cpp`): project-local `<name>.fly`, then `<dir>/src/`, then the bundled stdlib, then `FLY_STDLIB_DIR`, then `<projectRoot>/module/`.
3. Inspect dependency handling (`fly -deps`, `fly -dump` in `src/fly/project.cpp`).
4. Inspect existing public modules in `dump/public/` (`sleep.fly`, `http.fly`, `hashbox.fly`).
5. Follow existing conventions.

Do not assume that every module is necessarily one file.

Do not assume that every module is necessarily a directory.

Do not invent a package layout without checking the implementation.

The repository's current module resolver is authoritative.

---

# 25. Built-in vs Public Module Decision

Use this principle:

```text
Fundamental/runtime/platform primitive
        ↓
runtime or built-in API

Higher-level reusable functionality
        ↓
public Fly module
```

Examples of functionality that naturally fit public modules include:

* HTTP libraries
* archive libraries
* data-format libraries
* utility libraries
* application-level helpers

Examples that may require runtime/compiler support include:

* primitive binary I/O
* low-level process/platform facilities
* fundamental networking primitives
* operations that cannot be expressed efficiently or safely in Fly

Do not embed an entire high-level library into the runtime merely because one application needs it.

---

# 26. SLEEP

SLEEP means:

**Simple Lightweight Extensible Expression Protocol**

Fly uses SLEEP for project manifests, especially:

```text
flylink.sleep
```

A typical manifest is:

```sleep
project
name "myapp"
version "0.1.0"
source "src/main.fly"
output "bin/myapp"
icon "assets/myapp.ico"
deps coll ["http"]
```

SLEEP is not JSON.

SLEEP is not TOML.

SLEEP is not YAML.

Never invent SLEEP syntax by pattern-matching to those formats.

---

# 27. `flylink.sleep`

The project manifest describes project configuration.

Fields used by Fly include concepts such as:

```text
project
name
version
source
output
icon
deps
```

The exact accepted field types, grammar, and validation rules must be checked against the SLEEP parser and project loader.

When modifying `flylink.sleep`, use existing repository manifests as canonical examples.

---

# 28. SLEEP Development Rule

Before creating or modifying a SLEEP file:

```text
Inspect existing .sleep files
        ↓
Inspect SLEEP parser
        ↓
Inspect project loader
        ↓
Confirm syntax
        ↓
Write manifest
        ↓
Run the actual build
```

Never invent constructs merely because they look reasonable.

For example, do not assume arbitrary JSON-style objects, YAML lists, `key: value` syntax, or punctuation that is not supported by the actual parser.

---

# 29. CLI Roles

Fly separates responsibilities among several executables.

## `fly`

Main developer-facing tool.

Typical commands (verify each against `src/fly/fly.cpp` / `fly -help` before relying on it):

```text
fly                    launch the REPL (fly-repl) when no command is given
fly -new <name>        scaffold a new project (fly -init <name> is the alias)
fly -compile <file>    compile a single .fly file, no project needed
fly -format <file>     deterministically format a .fly file
fly -deps [--offline]  install flylink.sleep 'deps' into module/
fly -build             build the project (flylink.sleep 'source' -> 'output')
fly -run [args...]     build when needed, then run the project output
fly -test              build and run the project's tests
fly -clean             remove the project's build artifacts
fly -dump install pkg  fetch a package into module/
fly -dump remove pkg   remove an installed package
fly -dump list         list installed packages
fly -dump update       re-fetch every installed package
fly -help              show help
fly -version           show toolchain version info
fly -up                update the Fly toolchain to the latest release
```

There is no `fly -uninstall` command; uninstalling Fly is handled by the Windows NSIS installer's own uninstaller.

Modifiers accepted where supported: `-done` (compile+run+cleanup) and `-finish` (compile+cleanup, no run) for `-compile`/`-run`/`-build`; also `-compile ... -o <name>`, `-compile ... -icon <file>`, and `--offline` for `-deps`/`-dump`.

## `fly-cc`

Native Fly compiler.

Handles:

```text
lexer
parser
semantic analysis
IR generation
LLVM
native code generation
linking
```

## `fly-repl`

Interactive Fly REPL.

The REPL is designed around actual Fly semantics rather than maintaining a permanently unrelated second language.

---

# 30. CLI Argument Rules

Do not assume flags must appear in one exact order.

The current CLI deliberately supports practical argument permutations where implemented.

For example, compilation may support:

```text
fly -compile file.fly -run
```

as well as:

```text
fly -compile -run file.fly
```

Special lifecycle flags include:

```text
-done
-finish
```

These have distinct semantics and are mutually exclusive where required by the implementation.

Do not forward `fly`-only flags blindly to `fly-cc`.

Always inspect the current argument parser when changing CLI behavior.

---

# 31. Build / Run Lifecycle Flags

Where implemented:

```text
-done
```

means compile, execute exactly once, clean disposable build artifacts, and preserve the final executable.

```text
-finish
```

means compile and clean disposable artifacts without executing.

They must not be combined if the implementation defines them as mutually exclusive.

Never infer their semantics from flag names alone; verify the current implementation.

---

# 32. Dependency Commands

Fly provides dependency/package management through the toolchain.

Relevant commands include:

```text
fly -deps
fly -dump install <package>
fly -dump list
fly -dump remove <package>
fly -dump update
```

Offline behavior may be supported.

The actual package index, dependency resolution, and installation paths must be inspected before modifying them.

---

# 33. Toolchain Updates

The toolchain can update itself through:

```text
fly -up
```

The current updater architecture uses release metadata to discover newer releases rather than permanently relying only on the version baked into the executable.

When working on the updater, verify:

* current installed version
* latest-version discovery
* release artifact selection
* checksum/integrity verification
* temporary files
* replacement behavior
* fallback behavior
* Windows/Linux differences

Do not hardcode a specific release version as a permanent solution.

---

# 34. Runtime and `FlyValue`

Fly uses a tagged runtime value representation.

Core concepts include:

```text
NUM
DEC
YN
EMP
TEX
COLL
BOARD
```

Complex values are heap-backed.

Fly uses **automatic reference counting (ARC)** rather than tracing garbage collection as its primary memory management model.

Reference counts may be atomic where concurrent execution requires it.

Cycles are a known limitation.

When modifying the runtime, preserve existing ownership/reference-counting rules.

---

# 35. Runtime ABI

The Fly runtime is separated from the compiler.

Conceptually:

```text
fly-cc
    ↓
generated native program
    ↓
libflyrt
```

`libflyrt` exposes a C ABI to generated native programs.

Runtime changes therefore affect more than one layer.

Before changing a runtime function:

1. inspect its declaration
2. inspect its implementation
3. inspect call sites
4. inspect generated code
5. inspect relevant tests

Do not change a runtime signature casually.

---

# 36. Compiler Architecture

The compiler broadly follows:

```text
.fly
 ↓
Lexer
 ↓
Parser
 ↓
Semantic analysis
 ↓
IR generation
 ↓
LLVM modules
 ↓
LLVM linking
 ↓
LLVM optimization
 ↓
LLVM backend
 ↓
Linker
 ↓
Native executable
```

When a language feature is added, determine which compiler layers require modification.

A complete feature may need changes to:

```text
lexer
parser
AST
semantic analysis
IR/codegen
runtime
diagnostics
tests
```

Do not stop after parser support.

---

# 37. Adding Operators

Operators are not merely parser tokens.

When adding a new operator, determine:

* lexer token
* precedence
* associativity
* parser behavior
* AST representation
* type checking
* code generation
* runtime semantics
* diagnostics
* tests

Do not add an operator only to the lexer and claim the feature is complete.

---

# 38. Adding Runtime/Binary Features

Binary functionality is especially important for systems-oriented libraries.

Current text-oriented APIs are not automatically suitable for arbitrary binary data.

Before adding binary features, determine:

```text
How bytes are represented
How bytes are stored
How bytes are indexed
How files accept bytes
How network operations accept bytes
How binary values interact with existing collections
```

Do not pass arbitrary binary data through text APIs merely because the type system happens to accept the value.

Preserve zero bytes and values `0..255` exactly.

---

# 39. Public ZIP Module

ZIP support is expected to belong primarily in a **public Fly module**, not as a large runtime feature.

A proper ZIP module may eventually provide:

```text
ZIP parsing
ZIP listing
ZIP extraction
ZIP creation
STORED
DEFLATE
INFLATE
```

For real software-distribution use, DEFLATE support is particularly important.

Do not create a fake ZIP implementation around imaginary binary APIs.

If the public module requires a missing fundamental capability:

```text
stop
↓
identify missing primitive
↓
add it at the correct Fly layer
↓
test it
↓
continue the public module
```

Do not hide missing runtime capabilities behind giant arithmetic workarounds.

---

# 40. Duster as an External Consumer

Duster is a separate project written in Fly.

Its purpose is software distribution/installation.

Conceptually:

```text
Duster
 ↓
registry
 ↓
release artifact
 ↓
download
 ↓
archive extraction
 ↓
installer execution
```

Duster is useful as a real-world consumer of:

```text
filesystem
net
process
path
binary support
archive support
public modules
```

Do not distort Fly's architecture purely to make Duster work.

---

# 41. Public Module Design Example

A higher-level public library should normally look conceptually like:

```text
<project>/module/
└── zip.fly
```

and be used as:

```fly
bring zip
```

The actual module layout must follow the repository's real public-module mechanism (a single `.fly` file per package, installed into the project's `module/` directory by `fly -deps` / `fly -dump install`).

A public module should:

* have a clear API
* avoid side effects during import
* use only documented/implemented Fly capabilities
* document dependencies
* have tests
* avoid embedding executable-specific assumptions
* remain independently reusable

---

# 42. Error Handling in Libraries

Libraries should not invent undocumented failure behavior.

Choose behavior deliberately:

```text
return EMP
return Yes/No
return a board result
raise a runtime error
```

based on existing Fly API conventions.

For domain-level operations, explicit result boards may be appropriate.

For genuine runtime/I/O failures, Fly's actual `do` / `grab` mechanism may be appropriate.

Do not casually mix incompatible error models.

---

# 43. File and Process APIs

When writing Fly code, use the actual API names.

For filesystem functionality, current examples include:

```fly
filesystem.exists(path)
filesystem.isfile(path)
filesystem.isdir(path)
filesystem.read(path)
filesystem.write(path, text)
filesystem.append(path, text)
filesystem.mkdir(path)
filesystem.remove(path)
filesystem.list(path)
filesystem.cwd()
filesystem.chdir(path)
```

The exact accepted argument and return semantics must be verified against the implementation.

Process functionality is similarly implementation-defined.

Never invent APIs such as:

```text
filesystem.is_file
filesystem.is_dir
filesystem.write_bytes
```

unless they actually exist.

If a new API is needed, implement and document it properly rather than pretending it already exists.

---

# 44. Network APIs

When working with networking:

1. inspect the actual `net` implementation
2. inspect TLS behavior
3. inspect DNS behavior
4. inspect existing public HTTP modules
5. inspect network tests

Never assume a text API is binary-safe.

Never silently downgrade secure communication to plaintext.

Never invent an HTTP API based on Python/JavaScript conventions.

---

# 45. Testing Public Modules

Public Fly modules should have deterministic tests.

Prefer:

```text
local fixtures
local files
local test servers
fixed test data
```

over live third-party services when testing core logic.

A live network dependency can be used for explicit end-to-end coverage, but it should not be the only proof that a library works.

---

# 46. Test-Driven Change Discipline

When fixing a bug:

1. Reproduce the failure.
2. Identify the actual layer responsible.
3. Add or update a regression test.
4. Implement the smallest correct fix.
5. Run the targeted test.
6. Run the full relevant suite.
7. Report exact results.

Do not "fix" a test by changing the expected behavior unless the specification itself is intentionally changing.

---

# 47. Documentation Discipline

There are two useful kinds of Fly documentation:

```text
spec.md
    ↓
what Fly is and how Fly works

skill.md
    ↓
how an agent/developer should work on Fly correctly
```

`spec.md` should be public-facing specification/project documentation.

`skill.md` should be operational and defensive.

Do not turn `skill.md` into marketing copy.

Do not turn `spec.md` into a private agent instruction manual.

---

# 48. Source of Truth

When determining whether something is real:

Priority should generally be:

```text
1. actual compiler/runtime/toolchain implementation
2. passing repository tests
3. canonical project documentation
4. design specifications
5. roadmap/planning documents
6. memory/assumption
```

The lower levels must not override the higher levels for implementation facts.

If the docs say an API exists but the compiler rejects it, investigate the discrepancy.

Do not simply teach the documented API as though it is definitely implemented.

---

# 49. Common Model Failure Modes

Correct these immediately when encountered.

### Wrong language keywords

```text
continue → skip
break → getout
def → job
return → give
else if → orif
else → ifnot
try/catch → do/grab
true/false → Yes/No
null → EMP
```

### Wrong filesystem names

Do not transform:

```text
filesystem.isfile
filesystem.isdir
```

into:

```text
filesystem.is_file
filesystem.is_dir
```

### Wrong collection style

Do not transform:

```fly
count(items)
attach(items, x)
seek(items, x)
```

into assumed methods.

### Wrong comments

Do not use `#` or `//` when writing Fly comments.

### Wrong module assumptions

Do not assume every useful library is built into the runtime.

Do not assume every module is a public package.

### Wrong SLEEP assumptions

Do not write YAML/JSON/TOML syntax into SLEEP.

### Wrong API assumptions

Do not create a function merely because a similar language has one.

---

# 50. Working With Missing Functionality

When a requested program needs a capability Fly does not currently provide:

Do not immediately write a workaround.

First identify the capability.

Example:

```text
Duster needs ZIP extraction
        ↓
ZIP extraction needs binary input
        ↓
binary input needs binary-safe file I/O
        ↓
binary implementation may need bitwise primitives
```

Then decide:

```text
language?
compiler?
runtime?
built-in module?
public module?
```

Implement the lowest-level missing primitive at the correct layer.

Then build the higher-level library on top of it.

---

# 51. Public Module Rule

A public module should be genuinely implementable in Fly.

If an implementation requires a capability that does not yet exist:

> **Do not fake the missing capability with undocumented syntax.**

Instead:

1. identify the missing primitive
2. report it
3. implement it in Fly if it belongs in Fly
4. test it
5. then implement the public module

This prevents modules from accumulating imaginary APIs.

---

# 52. No Unverified Claims

When reporting work:

Good:

```text
Implemented X.
Targeted test passed.
Full Windows suite: 49/49.
```

Also good:

```text
Implemented X, but I could not execute the tests because fly-cc
was unavailable in this environment.
```

Bad:

```text
Tests should pass.
```

Bad:

```text
I manually verified it, so it passes.
```

unless explicitly describing it as a manual trace rather than a test result.

---

# 53. Release / Distribution Work

Release tooling should be deterministic and reproducible.

For checksum generation:

* use the actual Fly filesystem APIs
* do not hash the generated checksum file itself
* use correct relative release paths
* use the real `hashbox` API if available
* verify generated artifacts using actual tools

When working with release metadata:

* do not hardcode a version where metadata discovery is intended
* verify artifact names
* verify checksums
* verify both Windows and Linux release paths where applicable

---

# 54. Legal / Licensing Documentation

Fly currently uses:

**PolyForm Noncommercial License 1.0.0**

The actual license file is authoritative.

Do not claim that Fly is MIT-licensed unless the repository actually says so.

Do not invent commercial-license permissions.

If documentation and the actual license disagree, flag the contradiction.

Note: there is no `LICENSE` file in the repository at this time, even though `README.md` links to one. Until a license file is added, do not invent licensing terms; state that the license file is absent when asked.

---

# 55. When Adding a New Public Module

Before adding a new public module:

```text
Inspect module system
        ↓
Inspect existing modules
        ↓
Inspect dependency handling
        ↓
Choose module API
        ↓
Implement in Fly
        ↓
Add tests
        ↓
Document usage
        ↓
Build with actual Fly
```

A public module should not secretly depend on:

* Python
* PowerShell
* shell commands
* undocumented runtime functions
* imaginary compiler features

unless the module's explicitly documented purpose is to invoke an existing external tool and the architecture actually permits it.

---

# 56. When Adding a New Built-in Module

Only add functionality to the runtime/built-in layer when there is a real reason.

Appropriate reasons may include:

* platform functionality
* fundamental native capability
* efficient low-level operation
* ABI/runtime interaction
* functionality impossible or impractical to implement correctly in Fly

A high-level library should normally remain a public module.

When adding a built-in module, inspect:

```text
runtime
compiler API recognition
generated code
linking
tests
documentation
```

and update all affected layers.

---

# 57. Agent Workflow for Fly Tasks

For any non-trivial task:

### Step 1 — Understand the request

Identify whether it concerns:

```text
language
compiler
runtime
toolchain
built-in module
public module
documentation
tests
```

### Step 2 — Inspect reality

Search source/tests/docs.

### Step 3 — Determine the smallest correct layer

Do not modify the runtime when a public module is sufficient.

Do not write a public-module workaround when a fundamental runtime primitive is actually missing.

### Step 4 — Implement

Follow existing conventions.

### Step 5 — Test

Run the actual toolchain.

### Step 6 — Report

State:

* files changed
* architectural changes
* new APIs
* new public modules
* dependencies
* exact test commands
* exact results
* anything that remains unverified

---

# 58. Minimal Examples

## Program

```fly
name = take("Name: ")

if name == "Rick" {
    show("Welcome back, {name}!")
}
orif name == "admin" {
    show("Administrator access granted.")
}
ifnot {
    show("Hello, {name}!")
}
```

## Function

```fly
job add(a, b) {
    give a + b
}

result = add(10, 20)
show(result)
```

## Loop

```fly
for item in items {
    if item == "bad" {
        skip
    }

    if item == "stop" {
        getout
    }

    show(item)
}
```

## Error handling

```fly
do {
    number = num("hello")
}
grab (err) {
    show("Invalid number.")
}
```

## Module

```fly
bring filesystem

text = filesystem.read("hello.txt")
show(text)
```

---

# 59. Final Rule

When working on Fly, prefer:

```text
inspect
→ understand
→ implement
→ compile
→ test
→ report
```

over:

```text
assume
→ imitate another language
→ invent API
→ declare success
```

The most important rule in this entire skill is:

> **Fly is its own language, its own toolchain, and its own ecosystem. The repository is the source of truth. Never guess what Fly does.**
