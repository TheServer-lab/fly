#include "flycc/ast.h"
#include <iostream>

namespace flycc {

static void indent(int n) { for (int i = 0; i < n; i++) std::cout << "  "; }

static void dumpExpr(const Expr* e, int d) {
    if (!e) { indent(d); std::cout << "<null>\n"; return; }
    indent(d);
    switch (e->kind) {
        case ExprKind::NumLit:  std::cout << "Num(" << e->num_val << ")\n"; return;
        case ExprKind::DecLit:  std::cout << "Dec(" << e->dec_val << ")\n"; return;
        case ExprKind::YnLit:   std::cout << "Yn(" << (e->yn_val ? "Yes" : "No") << ")\n"; return;
        case ExprKind::EmpLit:  std::cout << "EMP\n"; return;
        case ExprKind::TextLit: std::cout << "Text(\"" << e->text_val << "\")\n"; return;
        case ExprKind::TextTemplate:
            std::cout << "TextTemplate\n";
            for (auto& p : e->template_parts) {
                indent(d + 1);
                if (p.is_expr) { std::cout << "expr:\n"; dumpExpr(p.expr.get(), d + 2); }
                else            { std::cout << "lit(\"" << p.literal << "\")\n"; }
            }
            return;
        case ExprKind::Ident:   std::cout << "Ident(" << e->text_val << ")\n"; return;
        case ExprKind::CollLit:
            std::cout << "CollLit\n";
            for (auto& el : e->elements) dumpExpr(el.get(), d + 1);
            return;
        case ExprKind::BoardLit:
            std::cout << "BoardLit\n";
            for (size_t i = 0; i < e->board_keys.size(); i++) {
                indent(d + 1); std::cout << "pair:\n";
                dumpExpr(e->board_keys[i].get(), d + 2);
                dumpExpr(e->board_vals[i].get(), d + 2);
            }
            return;
        case ExprKind::Index:
            std::cout << (e->is_slice ? "Slice\n" : "Index\n");
            dumpExpr(e->index_target.get(), d + 1);
            if (e->is_slice) {
                dumpExpr(e->slice_from.get(), d + 1);
                dumpExpr(e->slice_to.get(), d + 1);
            } else {
                dumpExpr(e->index_expr.get(), d + 1);
            }
            return;
        case ExprKind::Binary:
            std::cout << "Binary(op=" << (int)e->binop << ")\n";
            dumpExpr(e->lhs.get(), d + 1);
            dumpExpr(e->rhs.get(), d + 1);
            return;
        case ExprKind::Unary:
            std::cout << "Unary(op=" << (int)e->unop << ")\n";
            dumpExpr(e->lhs.get(), d + 1);
            return;
        case ExprKind::Call:
            std::cout << "Call(" << e->callee << ")\n";
            for (auto& a : e->args) dumpExpr(a.get(), d + 1);
            return;
    }
}

static void dumpStmt(const Stmt* s, int d) {
    indent(d);
    switch (s->kind) {
        case StmtKind::VarDecl:
            std::cout << (s->is_hard ? "HardVarDecl(" : "VarDecl(") << s->var_name << ")\n";
            dumpExpr(s->init.get(), d + 1);
            return;
        case StmtKind::ExprStmt:
            std::cout << "ExprStmt\n";
            dumpExpr(s->expr.get(), d + 1);
            return;
        case StmtKind::If:
            std::cout << "If\n";
            for (auto& c : s->clauses) {
                indent(d + 1);
                std::cout << (c.cond ? "clause:\n" : "ifnot:\n");
                if (c.cond) dumpExpr(c.cond.get(), d + 2);
                for (auto& st : c.body) dumpStmt(st.get(), d + 2);
            }
            return;
        case StmtKind::While:
            std::cout << "While\n";
            dumpExpr(s->while_cond.get(), d + 1);
            for (auto& st : s->while_body) dumpStmt(st.get(), d + 1);
            return;
        case StmtKind::JobDecl:
            std::cout << "Job(" << s->job_name << ", params=" << s->params.size() << ")\n";
            for (auto& st : s->job_body) dumpStmt(st.get(), d + 1);
            return;
        case StmtKind::Give:
            std::cout << "Give\n";
            if (s->give_value) dumpExpr(s->give_value.get(), d + 1);
            return;
        case StmtKind::Block:
            std::cout << "Block\n";
            for (auto& st : s->block_body) dumpStmt(st.get(), d + 1);
            return;
        case StmtKind::DoGrabe:
            std::cout << "DoGrabe\n";
            indent(d + 1); std::cout << "do:\n";
            for (auto& st : s->do_body) dumpStmt(st.get(), d + 2);
            indent(d + 1); std::cout << "grabe(" << s->grabe_var << "):\n";
            for (auto& st : s->grabe_body) dumpStmt(st.get(), d + 2);
            return;
        case StmtKind::For:
            std::cout << "For(" << s->for_var << ")\n";
            dumpExpr(s->for_iterable.get(), d + 1);
            for (auto& st : s->for_body) dumpStmt(st.get(), d + 1);
            return;
        case StmtKind::Bring:
            std::cout << "Bring(" << s->bring_module << ")\n";
            return;
        case StmtKind::NativeJobDecl:
            std::cout << "NativeJob(" << s->job_name << ", params=" << s->params.size() << ")\n";
            return;
    }
}

void dumpProgram(const Program& prog) {
    for (auto& s : prog.top_level) dumpStmt(s.get(), 0);
}

} // namespace flycc
