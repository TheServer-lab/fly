#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// AST for the milestone-1..4 subset of Fly 0.1 (docs/architecture.md §7
// roadmap): scalars (num/dec/yn/emp), tex (incl. interpolation, §10),
// coll/board literals + indexing/slicing (§11-14), arithmetic,
// if/orif/ifnot, while, job/give, show, hard, ARC (see codegen.cpp).
//
// `do` / `grabe` (milestone 5, §3.5) is now represented (StmtKind::DoGrabe
// below) -- runtime errors thrown anywhere within a `do` block's dynamic
// extent unwind (via real LLVM invoke/landingpad + the Itanium C++
// exception ABI, see codegen.cpp's genDoGrabe) to the matching `grabe`,
// which binds the thrown FlyValue to `grabe_var`.
//
// Milestone 6 (this file's current state) adds: `for x in <iterable> { ... }`
// (StmtKind::For, against the internal FlyIterator protocol -- see
// runtime/iter.c), `num()`/`dec()`/`tex()` casts (still plain ExprKind::Call
// nodes -- codegen.cpp dispatches on the callee name, same as every other
// builtin; no new Expr node needed), and `bring <module>` (StmtKind::Bring
// -- see driver.cpp for project-local module resolution, §3.6 point 1).
//
// NOT yet represented here (post-roadmap, see architecture.md):
//   - task/spawn/channels (concurrency, §9): syntax intentionally TBD per
//     the design discussion.

namespace flycc {

struct Expr; struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class ExprKind {
    NumLit, DecLit, YnLit, EmpLit, TextLit, TextTemplate,
    Ident, Binary, Unary, Call,
    CollLit, BoardLit, Index,
};

// One segment of an interpolated text literal (mirrors token.h's TextPart,
// but with the expr part already parsed into a real sub-expression).
struct TemplatePart {
    bool is_expr = false;
    std::string literal;   // valid when !is_expr
    ExprPtr expr;          // valid when is_expr
};

enum class BinOp {
    Add, Sub, Mul, Div, Mod,
    Eq, Neq, Lt, Gt, Le, Ge,
    And, Or,
};

enum class UnOp { Neg, Not };

struct Expr {
    ExprKind kind;
    int line = 0;

    // literals
    int64_t num_val = 0;
    double dec_val = 0.0;
    bool yn_val = false;
    std::string text_val;   // TextLit contents, or Ident name

    // Binary / Unary
    BinOp binop{};
    UnOp unop{};
    ExprPtr lhs, rhs;   // rhs unused for Unary

    // Call
    std::string callee;
    std::vector<ExprPtr> args;

    // TextTemplate (spec §10): interpolated text literal, >= 1 part with
    // at least one is_expr==true (otherwise the parser emits a plain
    // TextLit instead -- see parser.cpp).
    std::vector<TemplatePart> template_parts;

    // CollLit (spec §11): `[e1, e2, ...]`
    std::vector<ExprPtr> elements;

    // BoardLit (spec §12): `{k1: v1, k2: v2, ...}` -- parallel arrays,
    // board_keys[i] pairs with board_vals[i].
    std::vector<ExprPtr> board_keys;
    std::vector<ExprPtr> board_vals;

    // Index / Slice (spec §13/§14), a postfix operator on `index_target`.
    // is_slice == false: `target[index_expr]` (index_expr non-null).
    // is_slice == true:  `target[slice_from : slice_to]`, either bound may
    // be null (omitted, spec §14's "omitted slice boundaries").
    ExprPtr index_target;
    bool is_slice = false;
    ExprPtr index_expr;
    ExprPtr slice_from, slice_to;
};

enum class StmtKind {
    VarDecl,     // [hard] name = expr
    ExprStmt,    // e.g. a bare call like show(x) or attach(...)
    If,          // if / orif* / ifnot?
    While,
    JobDecl,
    Give,
    Block,
    DoGrabe,     // do { ... } grabe (err) { ... }  -- spec §3.5, milestone 5
    For,         // for x in <iterable> { ... }     -- milestone 6
    Bring,       // bring <module>                  -- milestone 6, §3.6
    NativeJobDecl, // native job name(params)        -- milestone 7, §5/§21
};

struct IfClause {
    ExprPtr cond;              // null for the trailing `ifnot`
    std::vector<StmtPtr> body;
};

struct Stmt {
    StmtKind kind;
    int line = 0;

    // VarDecl
    std::string var_name;
    bool is_hard = false;
    ExprPtr init;

    // ExprStmt
    ExprPtr expr;

    // If: clauses[0] is the `if`, following are `orif`s, an optional final
    // clause with cond == nullptr is the `ifnot`.
    std::vector<IfClause> clauses;

    // While
    ExprPtr while_cond;
    std::vector<StmtPtr> while_body;

    // JobDecl
    std::string job_name;
    std::vector<std::string> params;
    std::vector<StmtPtr> job_body;

    // Give
    ExprPtr give_value; // may be null (bare `give` -> EMP, matches §17)

    // Block
    std::vector<StmtPtr> block_body;

    // DoGrabe (spec §3.5): `do { do_body } grabe (grabe_var) { grabe_body }`.
    // grabe_var names the local binding for the caught FlyValue error
    // object inside grabe_body (it is scoped to grabe_body only, like a
    // job parameter -- see sema.cpp).
    std::vector<StmtPtr> do_body;
    std::string grabe_var;
    std::vector<StmtPtr> grabe_body;

    // For (milestone 6): `for for_var in for_iterable { for_body }`.
    // for_var is a plain binding name (like a job parameter), scoped to
    // for_body only, rebound fresh each iteration -- see sema.cpp/codegen.cpp.
    std::string for_var;
    ExprPtr for_iterable;
    std::vector<StmtPtr> for_body;

    // Bring (milestone 6, §3.6): `bring bring_module`. Resolved to an
    // actual source file by the driver (driver.cpp) BEFORE Sema/CodeGen
    // run -- by the time CodeGen sees a Bring statement, the module's job
    // declarations have already been merged into the Program's top level,
    // so CodeGen treats StmtKind::Bring itself as a no-op marker.
    std::string bring_module;
};

struct Program {
    std::vector<StmtPtr> top_level;

    // Milestone 7 SLEEP/NET follow-up: qualified module-call aliases,
    // populated by driver.cpp's ModuleMerger from every `bring X` site.
    // Each entry is (qualifiedName, flatName) e.g. ("sleep.parse", "parse")
    // -- for a `bring sleep` that pulled in a module directly declaring
    // `job parse(...)`, this says "the call written as sleep.parse(...)
    // resolves to the SAME job as the flat name parse". This is what lets
    // a real, ordinary `.fly` module (sleep.fly) be called with
    // `sleep.parse(...)` qualified syntax at the use site WITHOUT sema/
    // codegen needing any sleep-specific special case: `bring`-able jobs
    // already have flat names (Fly's job namespace is flat, see driver.cpp's
    // ModuleMerger comment), and this table is just "also usable under this
    // qualified spelling", generically, for ANY brought module whose jobs
    // are plain JobDecl/NativeJobDecl (not the filesystem/process/etc.
    // dotted-call builtins, which are compiler-known and never populate
    // this table -- see codegen.cpp's `builtins` map, checked first).
    std::vector<std::pair<std::string, std::string>> module_aliases;
};

void dumpProgram(const Program& prog);

} // namespace flycc
