# Fly

**The programming language for people who want Python's simplicity without Python's ecosystem sprawl.**

Fly is a high-level, dynamically typed, general-purpose programming language designed for **native compilation**, practical software development, and a complete first-party developer toolchain.

Write readable code. Compile it to a native executable. Manage dependencies. Build projects. Run tests. Format code. Package software. Update the toolchain.

**One language. One toolchain. One ecosystem.**

---

## Why Fly?

Programming languages often make you choose.

You can have a friendly language, but then end up juggling a compiler, package manager, build system, environment manager, formatter, test runner, dependency files, and third-party tools.

Or you can use a powerful systems language and spend more time fighting the language than building your software.

Fly is designed around a different idea:

> **The language and the toolchain should feel like one product.**

Fly takes the approachable style of high-level scripting languages and combines it with a native compiler and first-party project tooling.

### Fly aims to give you

* A readable, high-level syntax
* Dynamic typing
* Native executable compilation
* A built-in project toolchain
* Dependency management
* A package ecosystem
* Project manifests
* Formatting
* Testing
* Cleaning build artifacts
* Running projects
* Toolchain updates
* Module support
* A native REPL
* Cross-platform compiler architecture
* A distinctive, simple vocabulary

---

# A Fly program

```fly
name = take("What is your name? ")

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

Functions are deliberately simple:

```fly
job add(a, b) {
    give a + b
}

result = add(10, 20)

show("Result: {result}")
```

And because Fly is dynamically typed:

```fly
value = 42
value = "Hello"
value = Yes
```

No type declaration ceremony is required.

---

# Native compilation

Fly is a **compiled language**.

A `.fly` source file is transformed through the Fly compiler into a native executable.

```text
.fly
  ↓
Lexer
  ↓
Parser
  ↓
Semantic analysis
  ↓
LLVM IR generation
  ↓
LLVM optimization
  ↓
Native backend
  ↓
Linker
  ↓
Executable
```

Fly does not require a language-level interpreter or virtual machine for normal execution.

The compiler is implemented in C++ and uses LLVM for native code generation.

The runtime is implemented separately as `libflyrt` with a stable C ABI.

---

# The Fly toolchain

Fly is more than a compiler.

The `fly` command is intended to be the **entry point for the entire development workflow**.

```text
fly
├── compile
├── build
├── run
├── test
├── deps
├── dump
├── init
├── format
├── clean
├── up
└── uninstall
```

## Compile

Compile one Fly source file:

```cmd
fly -compile src/main.fly
```

An icon can be supplied for Windows native executables:

```cmd
fly -compile src/main.fly -icon logo.ico
```

---

## Build

Build a Fly project from its `flylink.sleep` manifest:

```cmd
fly -build
```

The project manifest describes the source file, output and dependencies.

Example:

```sleep
project
  name "hello"
  version "0.1.0"
  source "src/main.fly"
  output "bin/hello"
  icon "assets/hello.ico"
  deps coll ["http"]
```

---

## Run

Build and run the project:

```cmd
fly -run
```

Fly can determine when a rebuild is necessary from project source and module timestamps.

---

## Test

Run the project's tests:

```cmd
fly -test
```

---

## Format

Format Fly source using the official formatter:

```cmd
fly -format
```

The goal is to avoid every project inventing its own formatting conventions.

---

## Clean

Remove generated build artifacts:

```cmd
fly -clean
```

---

## Initialize a project

Create a new Fly project:

```cmd
fly -init myapp
```

A project contains a `flylink.sleep` manifest and a source tree.

Typical layout:

```text
myapp/
├── flylink.sleep
├── src/
│   └── main.fly
├── module/
└── bin/
```

---

# One ecosystem

Fly is designed around an unusually integrated development experience.

Instead of requiring a collection of unrelated tools:

```text
language
compiler
build system
package manager
dependency file
formatter
test runner
project generator
runtime
```

Fly brings these pieces together under the same toolchain.

The goal is simple:

> **You should be able to install Fly and have everything you need to start building software.**

That does not mean Fly will prevent developers from using external tools.

It means they shouldn't be mandatory merely to get a normal project built and managed.

---

# Packages and dependencies

Fly projects use the `deps` field in `flylink.sleep`.

```sleep
project
  name "webapp"
  version "0.1.0"
  source "src/main.fly"
  output "bin/webapp"
  deps coll ["http", "sleep"]
```

Dependencies are managed through Fly's package ecosystem.

Install a package:

```cmd
fly -dump install http
```

List installed packages:

```cmd
fly -dump list
```

Remove a package:

```cmd
fly -dump remove http
```

Update packages:

```cmd
fly -dump update
```

Offline dependency operations can be performed with:

```cmd
fly -dump install http --offline
```

The package system is designed around the Fly ecosystem rather than requiring a separate package manager.

---

# Dump

**Dump** is Fly's package index and distribution mechanism.

Public packages are published through the Fly Dump ecosystem.

This allows Fly projects to use normal module imports:

```fly
bring http
bring sleep
```

while the project toolchain handles obtaining those dependencies.

The project manifest records installed dependencies so builds remain reproducible and understandable.

---

# `flylink.sleep`

Fly uses **SLEEP** for its project manifest format.

Example:

```sleep
project
  name "myapp"
  version "0.1.0"
  source "src/main.fly"
  output "bin/myapp"
  icon "assets/myapp.ico"
  deps coll ["http"]
```

SLEEP is:

**Simple Lightweight Extensible Expression Protocol**

It is an indentation-based data/configuration format designed to be easier to write and read than heavily punctuated configuration formats.

Fly uses SLEEP because project configuration should be readable too.

---

# Project icons

Fly projects can specify an executable icon:

```sleep
project
  name "myapp"
  version "0.1.0"
  source "src/main.fly"
  output "bin/myapp"
  icon "assets/myapp.ico"
  deps coll []
```

When building a Windows executable, the configured icon can be embedded into the resulting PE executable.

For one-off compilation:

```cmd
fly -compile src/main.fly -icon myapp.ico
```

A project icon is intended to flow through the toolchain automatically:

```text
flylink.sleep
      ↓
project icon
      ↓
fly -build
      ↓
fly-cc
      ↓
native executable
      ↓
embedded Windows icon
```

---

# Fly language

## Dynamic typing

Fly is dynamically typed.

```fly
name = "Rick"
age = 18
pi = 3.14
ready = Yes
```

Variables do not normally require explicit type declarations.

---

# Values and types

Fly 0.1 defines these core types:

| Type    | Description          | Example            |
| ------- | -------------------- | ------------------ |
| `tex`   | Text                 | `"Hello"`          |
| `num`   | Integer number       | `42`               |
| `dec`   | Decimal number       | `3.14`             |
| `yn`    | Boolean              | `Yes`              |
| `coll`  | Collection           | `[1, 2, 3]`        |
| `board` | Key/value collection | `{"name": "Rick"}` |
| `emp`   | Empty value          | `EMP`              |

Types describe Fly values but normally do not appear in variable declarations.

---

# Variables

Variables are mutable by default.

```fly
a = 10
a = 20
```

Fly is dynamically typed, so the same variable can hold different types:

```fly
a = 10
a = "Hello"
a = Yes
```

---

# Immutable variables

Use `hard` for an immutable variable:

```fly
hard name = "Rick"
```

After initialization:

```fly
hard age = 18

age = 19
```

The reassignment is an error.

`hard` applies to all Fly value types.

---

# Comments

Single-line comments begin with `$`.

```fly
$ This is a comment

name = "Rick" $ Inline comment
```

Multiline comments use `$$`:

```fly
$$
This is a multiline comment.

It can span multiple lines.
$$
```

---

# Booleans

Fly uses:

```fly
Yes
No
```

Example:

```fly
ready = Yes
running = No
```

---

# Empty values

Fly uses:

```fly
EMP
```

for an empty or absent value.

```fly
value = EMP
```

Operations that cannot produce a meaningful value may return `EMP`.

For example:

```fly
position = seek(items, "Nobody")

if position == EMP {
    show("Not found.")
}
```

---

# Operators

## Arithmetic

```text
+
-
*
/
%
```

Example:

```fly
a = 10 + 5
b = 20 * 4
```

## Comparison

```text
==
!=
<
>
<=
>=
```

## Logical

```text
and
or
not
```

Example:

```fly
ready = Yes and running == No
```

---

# Text

Text values use double quotes:

```fly
name = "Rick"
message = "Hello!"
```

---

# String interpolation

Fly supports expression interpolation directly inside text:

```fly
age = 17

show("Next year you will be {age + 1}")
```

Expressions may be arbitrary Fly expressions:

```fly
job add(a, b) {
    give a + b
}

show("Result: {add(10, 5)}")
```

Direct values also work:

```fly
show("Value: {age}")
```

Expressions are evaluated when the text value is created.

---

# Escaped braces

A literal `{` is written as:

```text
{{
```

A literal `}` is written as:

```text
}}
```

Example:

```fly
show("Use {{Name}} as an example.")
```

produces:

```text
Use {Name} as an example.
```

---

# Collections

Collections are ordered sequences.

```fly
items = [10, "Hello", Yes, 3.14]
```

Collections can contain mixed types.

Indexing starts at zero:

```fly
items = ["A", "B", "C"]

show(items[0])
```

Output:

```text
A
```

---

# Indexing

Collections and text support square-bracket indexing:

```fly
items[0]
name[1]
```

Text indexing produces a single text element.

---

# Slicing

Collections and text support slicing:

```fly
items[1:4]
name[0:3]
```

The starting index is inclusive.

The ending index is exclusive.

Example:

```fly
name = "RICK"

show(name[1:4])
```

Output:

```text
ICK
```

Omitted boundaries are supported:

```fly
name[:3]
name[2:]
name[:]
```

---

# Collection and text operations

Fly uses readable command-oriented names for common operations.

## `attach`

Append a value:

```fly
attach(items, "New")
```

## `place`

Insert at an index:

```fly
place(items, 1, "Inserted")
```

## `erase`

Remove an element:

```fly
erase(items, 2)
```

## `count`

Get the number of elements:

```fly
count(items)
```

Objects that expose a length API also support:

```fly
items.length()
name.length()
```

## `seek`

Search for a value or substring:

```fly
seek(items, "Alex")
seek("Hello World", "World")
```

For collections, `seek` returns the matching index.

For text, `seek` returns the matching substring position.

When no match exists:

```text
EMP
```

## `has`

Check for a value or substring:

```fly
has(items, "Alex")
has("Hello World", "World")
```

The result is:

```fly
Yes
No
```

## `bind`

Join collection values into text:

```fly
items = ["A", "B", "C"]

result = bind(items, ", ")
```

Result:

```text
A, B, C
```

## `sever`

Split text into a collection:

```fly
sever("A,B,C", ",")
```

Result:

```fly
["A", "B", "C"]
```

## `cut`

Remove surrounding whitespace:

```fly
cut(text)
```

## `raise`

Convert text to uppercase:

```fly
raise(text)
```

## `lower`

Convert text to lowercase:

```fly
lower(text)
```

---

# Boards

A `board` stores key/value pairs.

Example:

```fly
people = {
    "Rick": 18
    "Bob": 25
}
```

Boards may use non-text keys.

The exact board literal grammar and access semantics are part of the Fly grammar/toolchain specification.

---

# Functions

Functions are declared with `job`:

```fly
job add(a, b) {
    give a + b
}
```

Call them normally:

```fly
result = add(10, 20)
```

Functions do not require parameter or return-type declarations.

---

# Returning values

Use `give`:

```fly
job square(x) {
    give x * x
}
```

A function that finishes without giving a value produces:

```fly
EMP
```

---

# Input and output

Read input with `take`:

```fly
name = take("What is your name: ")
```

Display values with `show`:

```fly
show("Hello")
show(name)
show(10 + 5)
```

---

# Conditions

Fly uses:

```text
if
orif
ifnot
```

Example:

```fly
name = take("What is your name: ")

if name == "admin" {
    show("Welcome back!")
}
orif name == "RICK" {
    show("Hi rick!")
}
ifnot {
    show("Hello, {name}!")
}
```

---

# Loops

Fly provides `while` and `for`.

## `while`

```fly
while condition {
    ...
}
```

## `for`

```fly
for item in items {
    show(item)
}
```

Iteration semantics depend on the iterable value.

---

# Error handling

Fly provides structured error handling through `do` and `grabe`.

```fly
do {
    ...
}
grabe (err) {
    ...
}
```

Example:

```fly
do {
    value = num("hello")
}
grabe (err) {
    show("The conversion failed.")
}
```

An error that is not handled by a surrounding `grabe` block propagates outward.

An unhandled error terminates the current operation/program and reports the error.

---

# Type casting

Fly is dynamically typed, but values can be explicitly converted between compatible types.

The target type acts as the conversion function:

```fly
age = num("18")
price = dec("19.99")
text = tex(123)
```

More examples:

```fly
a = num(10.8)
b = dec(10)
c = tex(10)
```

Invalid conversions produce runtime errors.

```fly
do {
    number = num("hello")
}
grabe (err) {
    show("Invalid number.")
}
```

Casting does not mutate another variable's original value.

---

# Modules

Fly supports modules using `bring`.

```fly
bring math
bring filesystem
```

Imported modules provide functionality to the program.

Public modules can also be installed through Fly's package ecosystem.

Example:

```fly
bring http

response = http.get_tls("https://example.com")
```

---

# Built-in modules

Fly's toolchain defines compiler/runtime-backed modules including:

```text
net
filesystem
process
environment
system
path
```

These expose standard platform functionality through Fly's API.

Examples include operations conceptually such as:

```fly
path.join(...)
filesystem.read(...)
process.run(...)
environment.get(...)
system.os(...)
net.connect_tls(...)
```

The compiler recognizes the built-in API surface and routes these operations to the Fly runtime.

---

# Public modules

Public ecosystem modules are installed into the project's `module/` directory.

For example:

```text
module/
└── http.fly
```

Then a Fly program can use:

```fly
bring http
```

and call its public API.

This keeps the language itself small while allowing the ecosystem to grow independently.

---

# HTTP and networking

Fly's standard ecosystem supports native networking facilities.

The toolchain/runtime provides:

* TCP sockets
* HTTPS/TLS
* DNS
* Platform networking APIs
* HTTP functionality through public modules

Example:

```fly
bring http

response = http.get_tls("https://google.com")

show(response["status"])
```

Native TLS support uses real TLS rather than silently falling back to plain HTTP.

Certificate and hostname verification are part of the secure HTTPS path.

---

# Memory management

Fly uses **automatic reference counting (ARC)**.

Fly does not use a tracing garbage collector as its language runtime memory model.

The current runtime uses tagged `FlyValue` values with reference counting.

Reference counts are atomic where required by concurrent execution.

Cycles are a known limitation of the current model.

The goal is predictable native memory management while keeping the language high-level.

---

# `FlyValue`

The runtime represents Fly values with a tagged value structure.

Conceptually:

```text
FlyValue
├── NUM
├── DEC
├── YN
├── EMP
├── TEX
├── COLL
└── BOARD
```

The runtime uses a compact tagged representation and heap-backed structures for complex values such as text, collections and boards.

---

# Runtime architecture

The Fly runtime is separated from the compiler.

```text
fly-cc
   │
   └── native program
          │
          └── libflyrt
```

`libflyrt` is implemented in C and exposes a stable C ABI to generated code.

This separation allows the compiler and runtime to evolve independently.

---

# Compiler architecture

The current compiler architecture is:

```text
                  ┌───────────────┐
                  │   .fly file   │
                  └───────┬───────┘
                          │
                       Lexer
                          │
                       Parser
                          │
                        Sema
                          │
                       IRGen
                          │
                    LLVM modules
                          │
                    LLVM linking
                          │
                     LLVM opt
                          │
                    LLVM backend
                          │
                      Linker
                          │
                   Native binary
```

Each source file can be represented as an LLVM module.

Modules can be linked at the LLVM IR level before optimization, allowing cross-file optimization opportunities such as inlining.

---

# The Fly executables

The Fly toolchain is divided into clear roles.

## `fly.exe`

The main developer-facing tool.

It handles project and toolchain commands such as:

```text
fly
fly -compile
fly -build
fly -run
fly -test
fly -deps
fly -dump
fly -init
fly -format
fly -clean
fly -up
fly -uninstall
```

With no command-line flags, Fly launches the REPL.

---

## `fly-cc.exe`

The native Fly compiler.

It handles:

```text
lexer
parser
semantic analysis
IR generation
LLVM
linking
native executable generation
```

---

## `fly-repl.exe`

The interactive Fly REPL.

The REPL uses the real Fly compiler rather than maintaining a completely separate interpreter implementation.

Conceptually:

```text
REPL input
    ↓
Fly compiler
    ↓
native executable
    ↓
execute
```

This keeps REPL behavior close to actual compiled-program behavior.

---

# The REPL

Example:

```text
>>> 1 + 1
2

>>> a = 10

>>> a
10

>>> "hello"
hello

>>> 10 * 4
40

>>> Yes
Yes
```

Explicit output calls are not duplicated:

```text
>>> show(1 + 1)
2
```

Multiline expressions can continue across input lines:

```text
>>> (1 +
... 1)
2
```

The REPL preserves successful session state while failed turns do not corrupt previously established state.

---

# Toolchain updates

Fly is designed to update itself through the same toolchain.

The intended command is:

```cmd
fly -up
```

The updater can query the latest Fly GitHub Release, determine its release tag, download the appropriate release artifact, verify it and safely replace the current installation.

This allows users to maintain the language and toolchain without manually reinstalling every release.

---

# Windows installation

The Windows distribution is intended to provide a normal installer experience:

```text
fly-setup.exe
      ↓
Fly installer
      ↓
Fly installed
      ↓
PATH configured
      ↓
fly
```

The Windows installation includes the native Fly executables and required runtime components.

The installer can embed and use the Fly project icon as part of the Windows application experience.

---

# Native Windows executables

Fly can produce native Windows executables.

The Windows distribution includes the compiler, runtime components and required runtime DLLs.

Executable icons can be embedded into the PE executable rather than merely copied beside it.

This means a built Fly application can appear as a normal Windows application in Explorer and shortcuts.

---

# Portability

Fly's compiler architecture is intended to be portable.

The high-level architecture separates:

```text
language frontend
       ↓
LLVM IR
       ↓
platform backend/linker
       ↓
native executable
```

Platform-specific runtime functionality is isolated where necessary.

Windows and POSIX platforms may use different underlying system APIs while exposing the same Fly-level concepts.

---

# Project structure

A typical Fly project looks like:

```text
myapp/
├── flylink.sleep
├── src/
│   ├── main.fly
│   └── utils.fly
├── module/
│   └── http.fly
├── assets/
│   └── myapp.ico
└── bin/
    └── myapp.exe
```

A minimal manifest:

```sleep
project
  name "myapp"
  version "0.1.0"
  source "src/main.fly"
  output "bin/myapp"
  deps coll []
```

With an icon:

```sleep
project
  name "myapp"
  version "0.1.0"
  source "src/main.fly"
  output "bin/myapp"
  icon "assets/myapp.ico"
  deps coll []
```

---

# Complete development workflow

The intended Fly workflow is:

```text
Install Fly
    ↓
fly -init myapp
    ↓
Write .fly files
    ↓
fly -dump install ...
    ↓
fly -build
    ↓
fly -run
    ↓
fly -test
    ↓
fly -format
    ↓
Ship native executable
```

No separate project generator, package manager, formatter or build-system configuration is required for the basic workflow.

---

# Language philosophy

Fly does not attempt to reproduce another programming language.

Its design priorities are:

1. **Readability**
2. **Simplicity**
3. **Consistency**
4. **Expressiveness**
5. **Practical native compilation**
6. **A distinctive Fly vocabulary**

The goal is not to make every feature configurable.

The goal is to make the common path pleasant.

---

# Python-inspired, not Python-compatible

Fly is intentionally comfortable for programmers familiar with high-level languages.

However, Fly is **not Python syntax with a few renamed keywords**.

Fly has its own:

* Syntax
* Types
* Runtime
* Compiler
* Module system
* Error handling model
* Package manager
* Project manifest
* Build system
* CLI
* Ecosystem

The point is to provide a similar level of accessibility while building a different language from the ground up.

---

# Fly 0.1 language specification

This section defines the current Fly 0.1 language design.

## File extension

Fly source files use:

```text
.fly
```

---

## Comments

Single-line:

```fly
$ comment
```

Multiline:

```fly
$$
comment
comment
$$
```

---

## Core values

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

```fly
Yes
No
```

Empty literal:

```fly
EMP
```

---

## Variables

```fly
a = 10
```

Variables are mutable unless declared with `hard`:

```fly
hard a = 10
```

---

## Operators

Arithmetic:

```text
+
-
*
/
%
```

Comparison:

```text
==
!=
<
>
<=
>=
```

Logical:

```text
and
or
not
```

---

## Strings

```fly
message = "Hello"
```

Interpolation:

```fly
show("Hello {name}")
```

Escaped braces:

```fly
show("Use {{name}} literally.")
```

---

## Collections

```fly
items = [1, 2, 3]
```

Index:

```fly
items[0]
```

Slice:

```fly
items[1:3]
items[:3]
items[2:]
items[:]
```

---

## Boards

Boards represent key/value data:

```fly
person = {
    "name": "Rick"
    "age": 18
}
```

The board grammar defines the exact literal and access syntax.

---

## Functions

```fly
job add(a, b) {
    give a + b
}
```

A function call:

```fly
result = add(10, 20)
```

No explicit parameter or return type is required.

---

## Input

```fly
name = take("Name: ")
```

---

## Output

```fly
show("Hello")
```

---

## Conditions

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

---

## Loops

```fly
while condition {
    ...
}
```

and:

```fly
for item in items {
    show(item)
}
```

---

## Errors

```fly
do {
    ...
}
grabe (err) {
    ...
}
```

Errors can propagate to an enclosing error handler.

---

## Casting

```fly
num("42")
dec("3.14")
tex(123)
```

Invalid conversions raise runtime errors.

---

## Modules

```fly
bring math
```

Public modules can be installed into the project's module tree.

---

# Reserved keywords

Fly 0.1 reserves:

```text
if
orif
ifnot

while
for

job
give

take
show
bring

do
grabe

hard

attach
place
erase
count
seek
has
bind
sever
cut
raise
lower

Yes
No
EMP
```

Compatibility aliases or legacy vocabulary may exist in development versions, but new code should use the current Fly 0.1 names.

---

# Fly 0.1 status

**Fly 0.1 — Language design and toolchain milestone**

Current design/implementation areas include:

* Dynamic typing
* Mutable variables
* `hard` immutable variables
* `tex`
* `num`
* `dec`
* `yn`
* `coll`
* `board`
* `emp`
* Arithmetic
* Comparison
* Logical operators
* Indexing
* Slicing
* String interpolation
* Functions
* Conditions
* `while`
* `for`
* Input/output
* Collection operations
* Error handling
* Type casting
* Modules
* Native compilation
* LLVM backend
* Native runtime
* Project manifests
* Dependency management
* Package management
* Formatting
* Testing
* Project initialization
* Build/run/clean workflows
* Native Windows executables
* Windows installation tooling
* Executable icons
* Native REPL
* Toolchain update infrastructure

Some advanced APIs and platform capabilities continue to evolve as Fly approaches a stable release.

---

# Example: a complete small program

```fly
job greet(name) {
    if name == EMP {
        give "Nobody"
    }

    give name
}

name = take("What is your name? ")

message = greet(name)

show("Hello, {message}!")
```

---

# Example: collections

```fly
items = ["Fly", "SLEEP", "Dump"]

attach(items, "LLVM")

for item in items {
    show(item)
}

show("Items: {count(items)}")
```

---

# Example: error handling

```fly
input = take("Enter a number: ")

do {
    number = num(input)
    show("You entered {number}.")
}
grabe (err) {
    show("That wasn't a valid number.")
}
```

---

# Example: module usage

```fly
bring http

response = http.get_tls("https://example.com")

show(response["status"])
```

---

# Example: project

`flylink.sleep`:

```sleep
project
  name "webapp"
  version "0.1.0"
  source "src/main.fly"
  output "bin/webapp"
  icon "assets/webapp.ico"
  deps coll ["http"]
```

`src/main.fly`:

```fly
bring http

response = http.get_tls("https://example.com")

show("HTTP status: {response["status"]}")
```

Build:

```cmd
fly -build
```

Run:

```cmd
fly -run
```

---

# Design principle: batteries included

Fly's philosophy is not that every feature must live in the language itself.

Instead:

> **The language should stay small while the ecosystem stays complete.**

That means:

* language syntax stays readable
* standard capabilities live in modules/runtime APIs
* third-party functionality lives in packages
* the toolchain provides a consistent way to obtain and use both

The result is intended to feel like one ecosystem rather than a pile of unrelated utilities.

---

# Roadmap

Fly 0.1 establishes the core language and toolchain.

Future work can expand:

* Standard libraries
* More public modules
* More platform targets
* Better diagnostics
* Faster builds
* Better optimization
* More packaging capabilities
* Broader testing infrastructure
* Stable module APIs
* More complete developer tooling
* A mature package ecosystem

The exact roadmap may change as implementation progresses.

---

# Contributing

Fly is a language and toolchain project.

Contributions can involve:

* The language frontend
* Compiler/code generation
* LLVM integration
* Runtime
* Standard modules
* Package infrastructure
* Toolchain commands
* Documentation
* Tests
* Developer experience

When modifying Fly, preserve the distinction between:

```text
language
compiler
runtime
toolchain
ecosystem
```

A feature should live in the layer where it naturally belongs.

---

# Licensing

Fly uses a **dual-licensing model**.

## Free for noncommercial use

The Fly source code is available under the **PolyForm Noncommercial License 1.0.0**.

You may use, study, modify, compile, and distribute Fly for permitted noncommercial purposes, including personal projects, hobby projects, research, experimentation, education, and other uses covered by the license.

The complete license is provided in [`LICENSE`](LICENSE).

## Commercial use

Commercial use of Fly is available under a separate **Fly Commercial License**.

A commercial license is intended for organizations and individuals who want to use Fly for commercial software, commercial products, internal business operations, or other uses that are not permitted by the free noncommercial license.

The commercial license may provide additional rights such as:

* Commercial use
* Proprietary modifications
* Distribution of commercial software built with or based on Fly
* Embedding Fly into commercial products
* Commercial redistribution
* Additional support and business terms

See [`LICENSE-COMMERCIAL.md`](LICENSE-COMMERCIAL.md) for the commercial licensing terms.

### Why dual licensing?

Fly is source-available because we want people to be able to learn from it, experiment with it, improve it, and build noncommercial projects without paying for a license.

At the same time, commercial licensing allows Fly to support a sustainable ecosystem and continued development of the compiler, runtime, standard libraries, package infrastructure, and developer tooling.

**You do not need a commercial license for permitted noncommercial use.**

**Commercial use requires a Fly Commercial License unless another license or written permission from the copyright holder explicitly grants the required rights.**


---

# Fly

**Write it simply. Compile it natively. Build everything with one toolchain.**

```text
                    Fly
                     │
       ┌─────────────┼─────────────┐
       │             │             │
    Language      Compiler      Runtime
       │             │             │
       └─────────────┼─────────────┘
                     │
                  Toolchain
                     │
        ┌────────────┼────────────┐
        │            │            │
      Build        Packages      Run
        │            │            │
        └────────────┼────────────┘
                     │
                  Ecosystem
                     │
                  Software
```

**One language. One toolchain. One ecosystem.**
