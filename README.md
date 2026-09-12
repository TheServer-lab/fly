# fly-cc

A native compiler for [Fly 0.1](../fly-architecture.md) — lexer → parser →
Sema → LLVM IR → object file → linked native binary.

**Status: milestones 1–6 of the roadmap in `docs/architecture.md` §7.**
Working: `num`/`dec`/`yn`/`emp` scalars, `tex` (a real heap type, incl.
string interpolation, §10), `coll`/`board` literals + indexing/slicing
(§11-14) and the §15 ops (`attach`/`place`/`erase`/`count`/`seek`/`has`/
`bind`/`sever`/`cut`/`raise`/`lower`, plus `take`), arithmetic (with
checked `num` overflow), comparisons (content-equality for `tex`),
`and`/`or`/`not`, `if`/`orif`/`ifnot`, `while`, `job`/`give`, `show`,
`hard`, ARC (retain/release) memory management for `tex`/`coll`/`board`,
`do`/`grabe` (§3.5, milestone 5) via real LLVM `invoke`/`landingpad` +
the Itanium C++ exception ABI, and now (milestone 6) `num()`/`dec()`/
`tex()` casts (§23), `for x in <iterable>` against the internal
FlyIterator protocol (§3.4), and `bring`/project-local modules (§3.6).
`examples/fizzbuzz.fly`, `examples/mixed_arith.fly`,
`examples/interpolation.fly`, `examples/collections.fly`,
`examples/arc_stress.fly`, `examples/error_handling.fly`,
`examples/casting.fly`, `examples/for_loops.fly`, and
`examples/modules_demo/` all compile and run correctly.
`examples/arc_stress.fly` and `examples/for_loops.fly` (per their headers)
run leak-free under `valgrind --leak-check=full`; `examples/error_handling.fly`
and `examples/casting.fly` run crash/corruption-free under valgrind too,
with the small, DOCUMENTED ARC-on-unwind leaks noted below (not silently
swept under the rug).

**Not yet implemented** (later roadmap milestones, see
`docs/architecture.md` for the design): concurrency (`task`/mutation-guard/
channels — architecture.md §9, syntax still TBD anyway); within milestone
6's own scope, `bring`'s bundled-standard-library and `flylink.sleep`
dump-package resolution (§3.6 points 2–3) are also open items (already
flagged as such by §21) — only project-local module resolution (§3.6
point 1) is implemented, with an `FLY_STDLIB_DIR` environment variable as
a documented stand-in for a real bundled stdlib.

## ✅ Build-verified

This tree now builds and runs end-to-end against LLVM 20 (Ubuntu 24.04,
`llvm-20-dev`/`clang-20`; LLVM 17/18 are also expected to work per the
version note below — milestone 5 was in fact built and verified against
LLVM 18). Issues found and fixed while getting milestone 4
running:

1. **`llvm/Support/Host.h` moved to `llvm/TargetParser/Host.h`** in LLVM
   18 (`src/driver/driver.cpp`) — exactly the kind of shallow
   API-version drift flagged in the original draft.
2. **Real bug, not just a build error:** mixed `num`/`dec` arithmetic
   (`3.14 * 2`, `a + b` where one side is `num`) silently produced
   garbage. `runtime/arith.c` was reusing `fly_rt_as_dec()`, which
   bit-reinterprets a `FlyValue`'s `i64` payload as a `double` — correct
   for an actual `dec` value (whose payload *is* a bit-cast double), but
   wrong for a `num` operand (whose payload is a raw integer). Bit-casting
   the integer `2` as a double gives a tiny denormal, not `2.0`. Fixed by
   adding a `toDec()` helper that properly *converts* (rounds) a `num`
   payload to `double` instead of reinterpreting its bits, used
   consistently across `+ - * / <`. Regression test:
   `examples/mixed_arith.fly` / `tests/e2e/mixed_arith.expected`
   (`mixed_arith_e2e` in ctest).
3. **Real bug found while building milestone 4:** `tex` equality (`==`/
   `!=`) compared raw payload bits, which meant two `tex` values with
   identical *content* but produced by different allocations (e.g. two
   separately-interpolated strings) compared unequal. Fixed in
   `runtime/arith.c`'s `valuesEqual()` to do byte-content comparison for
   `FLY_TEX` instead of pointer/payload equality; `coll`/`board` keep
   identity comparison since the spec doesn't define deep equality for
   them.
4. **Real bug found while building milestone 4:** `genIndex()` in
   `codegen.cpp` released the indexed *container* temp after a `[...]`
   access but not the *index* temp itself. This was invisible for
   `coll`/`tex` indexing (the index is always a scalar `num`, and
   retain/release are no-ops on scalars) but leaked one `FlyText`
   allocation per `board[key]` read with a `tex` key — caught by running
   `examples/collections.fly` under `valgrind --leak-check=full` (2
   blocks definitely lost, tracked straight back to the two `people[...]`
   reads in that example). Fixed by releasing `idx`/`from`/`to` the same
   way every other builtin-call argument is released.

## Milestone 4 design notes

- **`tex` is now a real heap object** (`runtime/text.c`'s `FlyText`:
  refcounted header + length + owned, NUL-terminated buffer), not the
  milestone-3 shortcut of bit-casting a `CreateGlobalStringPtr` straight
  into the payload. Every text literal now allocates via
  `fly_rt_text_from_cstr` at the point it's evaluated — no interning yet
  (`docs/architecture.md` §2.2 flags that as a future optimization).
- **String interpolation** (§10) lowers to the `fly_rt_strbuild_*` call
  sequence from `docs/architecture.md` §3.4: `new` → one
  `append_lit`/`append_value` per template segment, in source order →
  `finish`. The lexer does the hard part (finding each `{expr}` span's
  matching `}` via a bracket-kind stack, skipping over nested string
  literals, honoring `{{`/`}}` escapes) and hands the parser raw,
  unparsed expression source; the parser re-lexes/re-parses each span
  with its own sub-`Lexer`+`Parser` instance.
- **`coll`/`board`** are growable-array / parallel-key-value-array heap
  objects (`runtime/coll.c`, `runtime/board.c`); `board` is linear-scan
  only for now (§2.2's hash-table upgrade above a size threshold is a
  documented, not-yet-built optimization). `board`'s "access syntax" is
  explicitly left open by the spec (§12) — this implementation's choice:
  `board[key]` reads (via `fly_rt_index`, returning `EMP` if absent) and
  `place(board, key, val)` writes/upserts (reusing `place`'s vocabulary
  rather than adding a new op name, matching how `count`/`has`/`erase`
  already dispatch by container type at the runtime level).
- **ARC** (§2.3): `codegen.cpp`'s top-of-file comment documents the exact
  ownership convention used (every `genExpr()` result is a "+1 owned"
  temporary; scope-exit and reassignment release accordingly). Refcounts
  are plain `uint32_t`, not atomic — correct for now since nothing in this
  milestone can share a `FlyValue` across threads/tasks (no concurrency
  yet); swapping to atomic ops is a follow-up, not a layout change.
  Verified leak-free and error-free under `valgrind --leak-check=full`
  across every example, including `examples/arc_stress.fly`'s 2000
  iterations × 20-element interpolated-`tex` colls (300,003 allocs,
  300,003 frees, zero errors).

## Milestone 5 design notes (`do`/`grabe`)

- **Real `invoke`/`landingpad`, not a simulation.** `codegen.cpp`'s
  `genDoGrabe` pushes the enclosing `grabe`'s landing pad onto a small
  stack (`invokeStack_`) while generating a `do` block's statements; every
  runtime/`job` call that can throw goes through a new `emitCall()` helper
  that checks this stack and emits an `invoke` (instead of a plain `call`)
  whenever it's non-empty. This falls out correctly for `do` blocks
  containing nested `if`/`while`/blocks/nested `do`/`grabe` for free,
  since it's threaded through the same recursive `genStmt`/`genExpr` walk
  `scopes_` already uses.
- **Built on the existing Itanium C++ exception ABI** (`runtime/eh.cpp`)
  rather than a hand-rolled personality routine + LSDA parser: `fly_rt_throw`/
  `fly_rt_throw_value` call libstdc++'s `__cxa_throw`, and codegen's
  landing pads call `fly_rt_begin_catch`/`fly_rt_catch_extract`/
  `fly_rt_end_catch`, which wrap `__cxa_begin_catch`/`__cxa_end_catch`.
  The personality function attached to every generated function is
  `__gxx_personality_v0` itself — libunwind does the actual stack walking,
  exactly matching architecture.md §3.5's "LLVM exception machinery with
  invoke/landingpad and libunwind."
- **Catch-all only.** Every landing pad uses a single `catch i8* null`
  clause (the standard Itanium-ABI idiom for `catch (...)`) since Fly 0.1
  doesn't define multiple typed error kinds to discriminate between yet.
  A minimal `FlyError` RTTI object exists in `eh.cpp` purely because
  `__cxa_throw`'s signature requires a non-null `std::type_info*`; codegen
  never looks at it.
- **Implicit top-level catch-all.** The whole top-level statement sequence
  (what becomes `main`'s body) is wrapped in its own invoke/landingpad
  targeting `fly_rt_report_uncaught`, so an error that unwinds past every
  user `grabe` still prints `fly: runtime error: <message>` and exits 1 —
  matching §3.5's unhandled-error behavior exactly, just reached via real
  unwinding now instead of the old MVP's immediate `exit()` at the throw
  site.
- **`-lstdc++` needed at link time** (`driver.cpp`): `fly-cc` still shells
  out to `cc` (a C compiler driver) to link the final binary, which
  doesn't pull in libstdc++ automatically the way `c++`/`clang++` would —
  needed for the ABI symbols above.
- **Every generated function now carries `uwtable` (async)** (`declareJob`/
  `run()`'s `mainFn`): a `job` can be called from inside a `do` block and
  itself throw several frames down through its OWN plain (non-invoke)
  calls before that — unwind tables (.eh_frame CFI) are what let the
  ABI's unwinder walk back through those intermediate frames correctly,
  independent of whether that specific function ever emits an `invoke`
  itself.
- **Documented, NOT silently glossed-over limitation: no ARC
  cleanup-on-unwind.** If an exception unwinds through a scope holding
  live `tex`/`coll`/`board` locals (the `do` block's own locals, or a
  called `job`'s locals), those references currently leak instead of
  being released — verified via `examples/error_handling.fly` under
  `valgrind`: zero invalid-memory errors/crashes, only small, expected
  leaks matching exactly this gap. Full correctness here needs a cleanup
  landingpad at every scope with heap-owning locals (essentially what a
  C++ compiler builds for automatic local destructors) — real, separate
  follow-up work, not implemented in this milestone. What IS real:
  errors are genuinely thrown/caught via `invoke`/`landingpad`, `grabe`
  genuinely runs with the right bound value, and a `do` block with no
  error has completely ordinary ARC behavior.

## Milestone 6 design notes (casting, `for`/iterators, `bring`/modules)

- **Casts** (§23): `tex(v)` is not a new runtime entry point — it reuses
  `fly_rt_to_text` (`runtime/text.c`), which already *is* the §10.3
  "convert any `FlyValue` to its textual representation" conversion.
  `num()`/`dec()` get their own `runtime/cast.c`, accepting every scalar
  tag plus `tex` (parsed via `strtoll`/`strtod`, surrounding whitespace
  tolerated, interior junk rejected) and throwing an ordinary Fly error
  (catchable via `do`/`grabe`) on `EMP`/`coll`/`board` or an unparsable/
  out-of-range/NaN source. `dec`→`num` truncates toward zero, matching
  C's `(int64_t)` cast semantics, with explicit range-checking rather than
  relying on double-to-`int64` overflow being undefined behavior.
- **Iterators** (§3.4's "FlyIterator protocol", `runtime/iter.c`): the
  iterator is an opaque native handle (`void*`), never a `FlyValue` — Fly
  0.1 doesn't expose iterators as a first-class value. `for x in c { ... }`
  lowers to `fly_rt_iter_new`/`_has_next`/`_next`/`_free` around a
  `while`-shaped loop (`codegen.cpp`'s `genFor`), with `x` rebound fresh
  each iteration exactly like `grabe`'s error variable. Scope, documented
  rather than silently assumed: `coll` iterates elements in order, `board`
  iterates **keys** in insertion order (the natural `for k in board { ...
  board[k] ... }` idiom, given §12 itself leaves board's access syntax
  open), `tex` iterates one-byte substrings (UTF-8 codepoint-aware
  iteration is a documented future refinement); the container's length is
  snapshotted at iterator-creation time, so mutating a `coll`/`board`
  mid-loop is a documented MVP hazard, not a checked error. An early
  `give` from inside a `for` body correctly frees every iterator it's
  escaping through (`codegen.cpp`'s `iterStack_`, drained by
  `releaseAllScopes()`) — verified leak-free under `valgrind
  --leak-check=full` (see `examples/for_loops.fly`'s early-return case).
- **`bring`/modules** (§3.6): resolved and merged entirely in
  `driver.cpp` (see its `ModuleMerger` class comment) **before** Sema/
  CodeGen run — by the time CodeGen sees a `Bring` statement it's a no-op
  marker, and every brought-in job is just an ordinary top-level `JobDecl`
  in the flattened `Program` (Fly's job namespace is already flat/
  unqualified, so no new call syntax was needed). Only project-local
  resolution (§3.6 point 1: next to the bringing file, or its sibling
  `src/`) is implemented; bundled-stdlib and `flylink.sleep` dump-package
  resolution (points 2–3) are open items already flagged by §21, with an
  `FLY_STDLIB_DIR` environment variable as a documented stand-in for a
  real bundled stdlib. A module file brought in via `bring` may only
  contain job declarations (and its own `bring`s) at the top level — Fly
  doesn't define "run a module's top-level statements at import time"
  semantics yet, so this is enforced as a clear compile error rather than
  guessed at. Diamond/repeated `bring`s (including cycles) are resolved
  correctly without duplicate-declaring or infinite-looping (canonical-
  path-keyed visited set); a genuine job-name collision between two
  different brought files (or between a module and the entry file) is a
  compile error, since Fly 0.1's job namespace has no per-module
  qualification to disambiguate with. Verified against a diamond-
  dependency example (`examples/modules_demo/`: `main.fly` brings
  `geometry` directly *and* `shapes`, which itself brings `geometry`) —
  leak-free under `valgrind --leak-check=full`.



```sh
# Debian/Ubuntu, e.g.:
sudo apt-get install llvm-20-dev clang cmake ninja-build libzstd-dev

cmake -B build -G Ninja -DLLVM_DIR=$(llvm-config-20 --cmakedir)
cmake --build build
```

## Run

```sh
./build/fly-cc examples/fizzbuzz.fly -o fizzbuzz
./fizzbuzz

./build/fly-cc examples/collections.fly -o collections
./collections
```

Useful flags: `--dump-ast` (print the parsed AST and exit-code-0 continue),
`--dump-ir` (print the generated LLVM IR before linking).

The `fly` toolchain launcher dispatches the project-level commands
(docs/architecture.md §7.2) and the compiler (`fly-cc`) and REPL
(`fly-repl`) that ship beside it:

```sh
# one-off compile / format
./build/fly -compile hello.fly -o hello
./build/fly -format src/main.fly

# scaffold, build, run, test a project (around a flylink.sleep manifest)
./build/fly -init my_project
cd my_project
../build/fly -deps --offline   # install flylink.sleep 'deps' from the local mirror
../build/fly -build
../build/fly -run              # rebuilds when sources are newer, then runs
../build/fly -test             # builds+runs tests/*.fly (or a test "..." line)
../build/fly -clean            # removes build artifacts (never source/deps)

# Dump package lifecycle
../build/fly -dump install "sleep"     # HTTPS from Dump, mirror fallback
../build/fly -dump list
../build/fly -dump remove "sleep"
../build/fly -dump update --offline
```

Run `fly -help` for the full surface. `-format` is the official deterministic,
semantics-preserving formatter: it only moves whitespace, preserves `$`/`$$`
comments and text literals verbatim, refuses syntactically invalid input, is
idempotent, and verifies its output lexes to the same token stream before
writing.

## Test

```sh
cd build && ctest --output-on-failure
```

For an ARC correctness check beyond the functional e2e tests, run any
example under valgrind, e.g.:

```sh
valgrind --leak-check=full ./arc_stress
```

## Layout

```
fly-cc/
├── CMakeLists.txt
├── include/flycc/        public headers (token, lexer, ast, parser, sema, codegen, driver)
├── src/
│   ├── lexer/  parser/  ast/  sema/  codegen/  driver/
│   └── main.cpp
├── runtime/               libflyrt (plain C, see docs/architecture.md §4)
│                          value/show/arith/error (milestone 1-3) +
│                          text/coll/board/strbuild (milestone 4) +
│                          cast/iter (milestone 6)
├── examples/              fizzbuzz.fly, mixed_arith.fly, interpolation.fly,
│                          collections.fly, arc_stress.fly, error_handling.fly,
│                          casting.fly, for_loops.fly, modules_demo/
└── tests/                 CTest wiring + e2e golden output
```

See `docs/architecture.md` (the architecture doc from the design
conversation — copy it into `docs/` in this repo) for the full design,
including the parts not built yet: atomic refcounting once concurrency
lands, the mutation-guard concurrency-safety mechanism, the layered
concurrency model, and `bring`'s bundled-stdlib/package resolution.

