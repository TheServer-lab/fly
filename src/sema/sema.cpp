#include "flycc/sema.h"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flycc {

namespace {

struct Binding { bool is_hard; };

// One scope frame per block. Chained via a stack for simple lexical lookup;
// this is intentionally the simplest structure that satisfies §3.3 point 1 —
// no need for anything fancier until closures/task-escape analysis land.
class Scopes {
public:
    void push() { frames_.emplace_back(); }
    void pop() { frames_.pop_back(); }

    void declare(const std::string& name, bool is_hard) {
        frames_.back()[name] = Binding{is_hard};
    }

    Binding* lookup(const std::string& name) {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

private:
    std::vector<std::unordered_map<std::string, Binding>> frames_;
};

class Checker {
public:
    Checker(const std::string& filename) : filename_(filename) {}

    bool ok = true;

    void err(int line, const std::string& msg) {
        std::cerr << filename_ << ":" << line << ": error: " << msg << "\n";
        ok = false;
    }

    void checkExpr(Expr* e, Scopes& sc) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Ident:
                if (!sc.lookup(e->text_val))
                    err(e->line, "undefined identifier '" + e->text_val + "'");
                return;
            case ExprKind::Binary:
                checkExpr(e->lhs.get(), sc); checkExpr(e->rhs.get(), sc); return;
            case ExprKind::Unary:
                checkExpr(e->lhs.get(), sc); return;
            case ExprKind::TextTemplate:
                for (auto& p : e->template_parts)
                    if (p.is_expr) checkExpr(p.expr.get(), sc);
                return;
            case ExprKind::CollLit:
                for (auto& el : e->elements) checkExpr(el.get(), sc);
                return;
            case ExprKind::BoardLit:
                for (size_t i = 0; i < e->board_keys.size(); i++) {
                    checkExpr(e->board_keys[i].get(), sc);
                    checkExpr(e->board_vals[i].get(), sc);
                }
                return;
            case ExprKind::Index:
                checkExpr(e->index_target.get(), sc);
                if (e->is_slice) {
                    checkExpr(e->slice_from.get(), sc);
                    checkExpr(e->slice_to.get(), sc);
                } else {
                    checkExpr(e->index_expr.get(), sc);
                }
                return;
            case ExprKind::Call: {
                auto arity = jobArity_.find(e->callee);
                if (arity != jobArity_.end() && arity->second != (int)e->args.size()) {
                    err(e->line, "'" + e->callee + "' called with " +
                        std::to_string(e->args.size()) + " argument(s), expected " +
                        std::to_string(arity->second));
                }
                for (auto& a : e->args) checkExpr(a.get(), sc);
                return;
            }
            default: return; // literals
        }
    }

    void checkStmt(Stmt* s, Scopes& sc) {
        switch (s->kind) {
            case StmtKind::VarDecl: {
                checkExpr(s->init.get(), sc);
                if (auto* b = sc.lookup(s->var_name)) {
                    // Re-declaration in the SAME binding = reassignment.
                    if (b->is_hard)
                        err(s->line, "cannot reassign hard variable '" + s->var_name + "'");
                }
                sc.declare(s->var_name, s->is_hard);
                return;
            }
            case StmtKind::ExprStmt:
                checkExpr(s->expr.get(), sc); return;
            case StmtKind::If:
                for (auto& c : s->clauses) {
                    checkExpr(c.cond.get(), sc);
                    sc.push();
                    for (auto& st : c.body) checkStmt(st.get(), sc);
                    sc.pop();
                }
                return;
            case StmtKind::While:
                checkExpr(s->while_cond.get(), sc);
                sc.push();
                for (auto& st : s->while_body) checkStmt(st.get(), sc);
                sc.pop();
                return;
            case StmtKind::JobDecl: {
                jobArity_[s->job_name] = (int)s->params.size();
                sc.push();
                for (auto& p : s->params) sc.declare(p, /*is_hard=*/false);
                for (auto& st : s->job_body) checkStmt(st.get(), sc);
                sc.pop();
                return;
            }
            case StmtKind::Give:
                checkExpr(s->give_value.get(), sc); return;
            case StmtKind::Block:
                sc.push();
                for (auto& st : s->block_body) checkStmt(st.get(), sc);
                sc.pop();
                return;
            case StmtKind::DoGrabe:
                sc.push();
                for (auto& st : s->do_body) checkStmt(st.get(), sc);
                sc.pop();
                sc.push();
                sc.declare(s->grabe_var, /*is_hard=*/false); // scoped to grabe_body only, like a job parameter
                for (auto& st : s->grabe_body) checkStmt(st.get(), sc);
                sc.pop();
                return;
            case StmtKind::For:
                checkExpr(s->for_iterable.get(), sc);
                sc.push();
                sc.declare(s->for_var, /*is_hard=*/false); // scoped to for_body only, rebound each iteration
                for (auto& st : s->for_body) checkStmt(st.get(), sc);
                sc.pop();
                return;
            case StmtKind::Bring:
                // Module resolution/merging happens in driver.cpp BEFORE
                // Sema runs (see ast.h's Bring comment) -- by the time
                // Sema sees this node, any jobs the module exports are
                // already in jobArity_ via collectJobs(). Nothing left to
                // check here.
                return;
            case StmtKind::NativeJobDecl:
                // Registered into jobArity_ by collectJobs() below, same
                // as a JobDecl -- a native declaration has no body to walk.
                return;
        }
    }

    // Pre-pass so forward-referenced jobs (called before their textual
    // declaration) still get arity-checked correctly. Milestone 7:
    // NativeJobDecl is arity-checked identically to JobDecl -- from a
    // call site's point of view a native declaration and a Fly job are
    // the same kind of callable, they just differ in how codegen lowers
    // the call (see codegen.cpp's declareNative vs. declareJob/defineJob).
    // milestone 7 §16: a second `job`/`native job` with a name already
    // declared at this same top level used to be silently accepted (the
    // last declaration's arity/body would just overwrite the first one's
    // entry in jobArity_/codegen's jobFuncs_/nativeFuncs_ maps, with
    // whichever definition happened to win depending on codegen's pass
    // order) -- a real correctness gap, not just a missing nicety, since
    // it let a typo'd re-declaration shadow a real one with no diagnostic
    // at all. Now a duplicate top-level job name of either kind is a
    // clear, source-located compile error, same style as driver.cpp's
    // ModuleMerger cross-file version of this same check.
    void collectJobs(Program& prog) {
        std::unordered_map<std::string, int> declaredAtLine;
        for (auto& s : prog.top_level) {
            if (s->kind != StmtKind::JobDecl && s->kind != StmtKind::NativeJobDecl) continue;
            auto it = declaredAtLine.find(s->job_name);
            if (it != declaredAtLine.end()) {
                err(s->line, "job '" + s->job_name + "' is already declared (see line " +
                    std::to_string(it->second) + ") -- Fly 0.1's job namespace is flat/unqualified, "
                    "so a job name may only be declared once per file");
                continue;
            }
            declaredAtLine[s->job_name] = s->line;
            jobArity_[s->job_name] = (int)s->params.size();
        }

        // SLEEP/NET follow-up: qualified module-call aliases (ast.h's
        // Program::module_aliases), e.g. "sleep.parse" -> "parse" for a
        // `bring sleep` that pulled in a real Fly job named `parse`. Adding
        // these to jobArity_ under their QUALIFIED spelling means a call
        // written as `sleep.parse(x)` gets the exact same arity-checking
        // treatment as any other job call, with no special-casing in
        // checkExpr's Call handling above.
        for (auto& [qualified, flat] : prog.module_aliases) {
            auto it = jobArity_.find(flat);
            if (it != jobArity_.end()) jobArity_[qualified] = it->second;
        }
    }

private:
    std::string filename_;
    std::unordered_map<std::string, int> jobArity_;
};

} // namespace

bool Sema::check(Program& prog, const std::string& filename) {
    Checker c(filename);
    c.collectJobs(prog);
    Scopes sc;
    sc.push();
    for (auto& s : prog.top_level) c.checkStmt(s.get(), sc);
    sc.pop();
    return c.ok;
}

} // namespace flycc
