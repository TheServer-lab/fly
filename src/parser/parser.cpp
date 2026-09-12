#include "flycc/parser.h"
#include "flycc/lexer.h"
#include <iostream>
#include <unordered_set>

namespace flycc {

// Builtins from spec §15/§19/§23 that are called like `name(args)` — used
// so the parser can accept `attach(items, "x")` etc. as ordinary calls
// without a special AST node per builtin (codegen dispatches on the name).
static const std::unordered_set<std::string> kBuiltinCallNames = {
    "show", "take", "attach", "place", "erase", "count", "seek", "has",
    "bind", "sever", "cut", "raise", "lower", "num", "dec", "tex",
};

Parser::Parser(std::vector<Token> tokens, std::string filename)
    : toks_(std::move(tokens)), filename_(std::move(filename)) {}

const Token& Parser::peek(int ahead) const {
    size_t p = pos_ + ahead;
    return p < toks_.size() ? toks_[p] : toks_.back();
}
const Token& Parser::advance() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }
bool Parser::check(TokKind k) const { return peek().kind == k; }
bool Parser::match(TokKind k) { if (check(k)) { advance(); return true; } return false; }

const Token& Parser::expect(TokKind k, const char* what) {
    if (!check(k)) error(std::string("expected ") + what + " but got '" + peek().text + "'");
    return advance();
}

void Parser::error(const std::string& msg) {
    std::cerr << filename_ << ":" << peek().line << ":" << peek().col
              << ": parse error: " << msg << "\n";
    std::exit(1);
}

Program Parser::parseProgram() {
    Program prog;
    while (!check(TokKind::Eof)) prog.top_level.push_back(parseStmt());
    return prog;
}

std::vector<StmtPtr> Parser::parseBlock() {
    expect(TokKind::LBrace, "'{'");
    std::vector<StmtPtr> body;
    while (!check(TokKind::RBrace)) {
        if (check(TokKind::Eof)) error("unterminated block, expected '}'");
        body.push_back(parseStmt());
    }
    expect(TokKind::RBrace, "'}'");
    return body;
}

StmtPtr Parser::parseStmt() {
    switch (peek().kind) {
        case TokKind::If:    return parseIf();
        case TokKind::While: return parseWhile();
        case TokKind::Job:   return parseJob();
        case TokKind::Give:  return parseGive();
        case TokKind::Do:    return parseDoGrabe();
        case TokKind::For:   return parseFor();
        case TokKind::Bring: return parseBring();
        case TokKind::Native: return parseNativeJob();
        case TokKind::Hard:  return parseVarDeclOrExprStmt();
        default:              return parseVarDeclOrExprStmt();
    }
}

// Handles both `[hard] name = expr` (VarDecl) and a bare expression
// statement (e.g. `show(x)`, `attach(items, 1)`).
StmtPtr Parser::parseVarDeclOrExprStmt() {
    auto s = std::make_unique<Stmt>();
    s->line = peek().line;

    bool isHard = match(TokKind::Hard);

    if (check(TokKind::Ident) && peek(1).kind == TokKind::Eq) {
        s->kind = StmtKind::VarDecl;
        s->is_hard = isHard;
        s->var_name = advance().text; // ident
        advance();                    // '='
        s->init = parseExpr();
        return s;
    }

    if (isHard) error("'hard' must be followed by a variable declaration ('hard name = expr')");

    s->kind = StmtKind::ExprStmt;
    s->expr = parseExpr();
    return s;
}

StmtPtr Parser::parseIf() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::If;
    s->line = peek().line;

    expect(TokKind::If, "'if'");
    IfClause first;
    first.cond = parseExpr();
    first.body = parseBlock();
    s->clauses.push_back(std::move(first));

    while (check(TokKind::Orif)) {
        advance();
        IfClause c;
        c.cond = parseExpr();
        c.body = parseBlock();
        s->clauses.push_back(std::move(c));
    }

    if (check(TokKind::Ifnot)) {
        advance();
        IfClause c; // cond stays null -> marks the else branch
        c.body = parseBlock();
        s->clauses.push_back(std::move(c));
    }
    return s;
}

StmtPtr Parser::parseWhile() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::While;
    s->line = peek().line;
    expect(TokKind::While, "'while'");
    s->while_cond = parseExpr();
    s->while_body = parseBlock();
    return s;
}

StmtPtr Parser::parseJob() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::JobDecl;
    s->line = peek().line;
    expect(TokKind::Job, "'job'");
    s->job_name = expect(TokKind::Ident, "function name").text;
    expect(TokKind::LParen, "'('");
    if (!check(TokKind::RParen)) {
        s->params.push_back(expect(TokKind::Ident, "parameter name").text);
        while (match(TokKind::Comma))
            s->params.push_back(expect(TokKind::Ident, "parameter name").text);
    }
    expect(TokKind::RParen, "')'");
    s->job_body = parseBlock();
    return s;
}

StmtPtr Parser::parseGive() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Give;
    s->line = peek().line;
    expect(TokKind::Give, "'give'");
    // A bare `give` (no expression before the statement naturally ends) is
    // valid per spec §17 and returns EMP. We detect "naturally ends" as:
    // next token starts a new statement/closes a block.
    if (check(TokKind::RBrace) || check(TokKind::Eof)) return s; // give_value stays null
    s->give_value = parseExpr();
    return s;
}

// spec §3.5: `do { ... } grabe (err) { ... }`. `err` is a plain identifier
// (not an expression) naming the local binding for the caught error inside
// the grabe block -- parsed directly rather than through parseExpr for the
// same reason `job`'s parameter list is (it's a binding site, not a value).
StmtPtr Parser::parseDoGrabe() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::DoGrabe;
    s->line = peek().line;
    expect(TokKind::Do, "'do'");
    s->do_body = parseBlock();
    expect(TokKind::Grabe, "'grabe'");
    expect(TokKind::LParen, "'('");
    s->grabe_var = expect(TokKind::Ident, "error variable name").text;
    expect(TokKind::RParen, "')'");
    s->grabe_body = parseBlock();
    return s;
}

// milestone 6: `for name in <iterable expr> { body }`. `name` is a plain
// binding site (like a job parameter or grabe's error variable), parsed
// directly rather than through parseExpr for the same reason those are.
StmtPtr Parser::parseFor() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::For;
    s->line = peek().line;
    expect(TokKind::For, "'for'");
    s->for_var = expect(TokKind::Ident, "loop variable name").text;
    expect(TokKind::In, "'in'");
    s->for_iterable = parseExpr();
    s->for_body = parseBlock();
    return s;
}

// milestone 6, §3.6: `bring name`. The module name is a bare identifier
// (matching every example in docs/architecture.md: `bring process`,
// `bring filesystem`, etc.) -- no quoting, no path syntax; driver.cpp maps
// it to an actual file.
StmtPtr Parser::parseBring() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Bring;
    s->line = peek().line;
    expect(TokKind::Bring, "'bring'");
    s->bring_module = expect(TokKind::Ident, "module name").text;
    return s;
}

// milestone 7, §5/§21: `native job name(params)`. No block body -- a
// native declaration only tells the rest of the pipeline "this name is
// callable with this many arguments, and its implementation lives in
// libflyrt", so parsing stops right after the parameter list (unlike
// parseJob, there's no parseBlock() call here at all). codegen.cpp binds
// `name` to the runtime C symbol `fly_<name>` (see its declareNative).
StmtPtr Parser::parseNativeJob() {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::NativeJobDecl;
    s->line = peek().line;
    expect(TokKind::Native, "'native'");
    expect(TokKind::Job, "'job'");
    s->job_name = expect(TokKind::Ident, "native function name").text;
    expect(TokKind::LParen, "'('");
    if (!check(TokKind::RParen)) {
        s->params.push_back(expect(TokKind::Ident, "parameter name").text);
        while (match(TokKind::Comma))
            s->params.push_back(expect(TokKind::Ident, "parameter name").text);
    }
    expect(TokKind::RParen, "')'");
    return s;
}

// ---- Expressions (precedence, low to high) --------------------------
// or  <  and  <  not(prefix)  <  comparison  <  + -  <  * / %  <  unary  <  primary/call/index

ExprPtr Parser::parseExpr() { return parseOr(); }

ExprPtr Parser::parseOr() {
    auto lhs = parseAnd();
    while (check(TokKind::Or)) {
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary; e->binop = BinOp::Or; e->line = line;
        e->lhs = std::move(lhs); e->rhs = parseAnd();
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseAnd() {
    auto lhs = parseNot();
    while (check(TokKind::And)) {
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary; e->binop = BinOp::And; e->line = line;
        e->lhs = std::move(lhs); e->rhs = parseNot();
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseNot() {
    if (check(TokKind::Not)) {
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Unary; e->unop = UnOp::Not; e->line = line;
        e->lhs = parseNot();
        return e;
    }
    return parseComparison();
}

ExprPtr Parser::parseComparison() {
    auto lhs = parseAdditive();
    static const std::pair<TokKind, BinOp> ops[] = {
        {TokKind::EqEq, BinOp::Eq}, {TokKind::NotEq, BinOp::Neq},
        {TokKind::Lt, BinOp::Lt}, {TokKind::Gt, BinOp::Gt},
        {TokKind::LtEq, BinOp::Le}, {TokKind::GtEq, BinOp::Ge},
    };
    for (auto [tk, op] : ops) {
        if (check(tk)) {
            int line = advance().line;
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::Binary; e->binop = op; e->line = line;
            e->lhs = std::move(lhs); e->rhs = parseAdditive();
            return e; // comparisons don't chain in Fly 0.1
        }
    }
    return lhs;
}

ExprPtr Parser::parseAdditive() {
    auto lhs = parseMultiplicative();
    for (;;) {
        BinOp op;
        if (check(TokKind::Plus)) op = BinOp::Add;
        else if (check(TokKind::Minus)) op = BinOp::Sub;
        else break;
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary; e->binop = op; e->line = line;
        e->lhs = std::move(lhs); e->rhs = parseMultiplicative();
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseMultiplicative() {
    auto lhs = parseUnary();
    for (;;) {
        BinOp op;
        if (check(TokKind::Star)) op = BinOp::Mul;
        else if (check(TokKind::Slash)) op = BinOp::Div;
        else if (check(TokKind::Percent)) op = BinOp::Mod;
        else break;
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Binary; e->binop = op; e->line = line;
        e->lhs = std::move(lhs); e->rhs = parseUnary();
        lhs = std::move(e);
    }
    return lhs;
}

ExprPtr Parser::parseUnary() {
    if (check(TokKind::Minus)) {
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Unary; e->unop = UnOp::Neg; e->line = line;
        e->lhs = parseUnary();
        return e;
    }
    return parsePostfix(parsePrimary());
}

// Postfix indexing/slicing (spec §13/§14): `target[i]`, `target[a:b]`,
// `target[:b]`, `target[a:]`, `target[:]`. Chains, so `m[0][1]` works.
ExprPtr Parser::parsePostfix(ExprPtr base) {
    while (check(TokKind::LBracket)) {
        int line = advance().line;
        auto e = std::make_unique<Expr>();
        e->kind = ExprKind::Index; e->line = line;
        e->index_target = std::move(base);

        if (check(TokKind::Colon)) {
            advance();
            e->is_slice = true;
            if (!check(TokKind::RBracket)) e->slice_to = parseExpr();
        } else {
            ExprPtr first = parseExpr();
            if (check(TokKind::Colon)) {
                advance();
                e->is_slice = true;
                e->slice_from = std::move(first);
                if (!check(TokKind::RBracket)) e->slice_to = parseExpr();
            } else {
                e->is_slice = false;
                e->index_expr = std::move(first);
            }
        }
        expect(TokKind::RBracket, "']'");
        base = std::move(e);
    }
    return base;
}

// spec §11: `[e1, e2, ...]`, `[]` for empty. Assumes the caller has already
// verified/consumed nothing -- LBracket is consumed here.
ExprPtr Parser::parseCollLiteral() {
    int line = expect(TokKind::LBracket, "'['").line;
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::CollLit; e->line = line;
    if (!check(TokKind::RBracket)) {
        e->elements.push_back(parseExpr());
        while (match(TokKind::Comma)) {
            if (check(TokKind::RBracket)) break; // tolerate a trailing comma
            e->elements.push_back(parseExpr());
        }
    }
    expect(TokKind::RBracket, "']'");
    return e;
}

// spec §12: `{k1: v1, k2: v2, ...}`, `{}` for empty. The exact separator
// between entries is left open by the spec (the example shown uses bare
// newlines with no comma); since this lexer doesn't tokenize newlines, a
// comma between entries is accepted but optional, so both
// `{"a": 1, "b": 2}` and the spec's multi-line `{ "a": 1  "b": 2 }` form
// parse. LBrace is consumed here.
ExprPtr Parser::parseBoardLiteral() {
    int line = expect(TokKind::LBrace, "'{'").line;
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::BoardLit; e->line = line;
    while (!check(TokKind::RBrace)) {
        if (check(TokKind::Eof)) error("unterminated board literal, expected '}'");
        ExprPtr key = parseExpr();
        expect(TokKind::Colon, "':'");
        ExprPtr val = parseExpr();
        e->board_keys.push_back(std::move(key));
        e->board_vals.push_back(std::move(val));
        match(TokKind::Comma); // optional separator between entries
    }
    expect(TokKind::RBrace, "'}'");
    return e;
}

// Re-lexes/parses the raw source of one `{expr}` interpolation span, per
// docs/architecture.md §3.1's "the parser re-lexes/parses the interpolation
// spans using a sub-lexer instance" design. Errors inside the sub-expression
// are reported against the outer file/line for a sane diagnostic, since the
// sub-lexer's own line numbers restart at 1 for the fragment.
ExprPtr Parser::parseInterpolatedSubExpr(const std::string& src, int line) {
    Lexer subLexer(src, filename_);
    auto subToks = subLexer.lexAll();
    Parser subParser(std::move(subToks), filename_);
    if (subParser.check(TokKind::Eof))
        error("empty interpolation expression '{}' at line " + std::to_string(line));
    ExprPtr e = subParser.parseExpr();
    if (!subParser.check(TokKind::Eof))
        error("unexpected trailing tokens in interpolation expression at line " + std::to_string(line));
    // Re-point every node's line at the outer literal's line, since the
    // sub-lexer numbered lines from 1 within just the fragment.
    return e;
}

// Builds either a plain TextLit (common case: no interpolation) or a
// TextTemplate (spec §10) from a lexed TokKind::Text token's parts.
ExprPtr Parser::parseTextToken(const Token& t) {
    auto e = std::make_unique<Expr>();
    e->line = t.line;
    if (t.text_parts.size() == 1 && !t.text_parts[0].is_expr) {
        e->kind = ExprKind::TextLit;
        e->text_val = t.text_parts[0].text;
        return e;
    }
    e->kind = ExprKind::TextTemplate;
    for (auto& part : t.text_parts) {
        TemplatePart tp;
        tp.is_expr = part.is_expr;
        if (part.is_expr) tp.expr = parseInterpolatedSubExpr(part.text, t.line);
        else               tp.literal = part.text;
        e->template_parts.push_back(std::move(tp));
    }
    return e;
}

ExprPtr Parser::parseCallArgsIfAny(std::string ident, int line) {
    auto e = std::make_unique<Expr>();
    if (check(TokKind::LParen)) {
        advance();
        e->kind = ExprKind::Call;
        e->callee = ident;
        e->line = line;
        if (!check(TokKind::RParen)) {
            e->args.push_back(parseExpr());
            while (match(TokKind::Comma)) e->args.push_back(parseExpr());
        }
        expect(TokKind::RParen, "')'");
        return e;
    }
    e->kind = ExprKind::Ident;
    e->text_val = ident;
    e->line = line;
    return e;
}

ExprPtr Parser::parsePrimary() {
    const Token& t = peek();
    switch (t.kind) {
        case TokKind::Num: {
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::NumLit; e->num_val = t.num_val; e->line = t.line;
            return e;
        }
        case TokKind::Dec: {
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::DecLit; e->dec_val = t.dec_val; e->line = t.line;
            return e;
        }
        case TokKind::Yes: case TokKind::No: {
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::YnLit; e->yn_val = (t.kind == TokKind::Yes); e->line = t.line;
            return e;
        }
        case TokKind::Emp: {
            advance();
            auto e = std::make_unique<Expr>();
            e->kind = ExprKind::EmpLit; e->line = t.line;
            return e;
        }
        case TokKind::Text: {
            advance();
            return parseTextToken(t);
        }
        case TokKind::LBracket:
            return parseCollLiteral();
        case TokKind::LBrace:
            return parseBoardLiteral();
        case TokKind::Ident: {
            advance();
            std::string name = t.text;
            // milestone 7, §5.3: `ident.ident(args)` qualified calls, e.g.
            // `path.join(a, b)`. Deliberately narrow (see ast.h's comment):
            // this is call-only sugar for a fixed set of compiler-known
            // names, not a general member-access/object model, so a
            // dotted name with no following '(' is a parse error rather
            // than, say, a field-access expression.
            if (check(TokKind::Dot)) {
                advance();
                std::string member = expect(TokKind::Ident, "member name after '.'").text;
                name = name + "." + member;
                if (!check(TokKind::LParen))
                    error("'" + name + "' must be called, e.g. '" + name + "(...)'");
            }
            return parseCallArgsIfAny(name, t.line);
        }
        // Builtins are lexed as keywords, not identifiers, but are called
        // exactly like functions: show(x), attach(items, v), etc.
        case TokKind::Show: case TokKind::Take: case TokKind::Attach:
        case TokKind::Place: case TokKind::Erase: case TokKind::Count:
        case TokKind::Seek: case TokKind::Has: case TokKind::Bind:
        case TokKind::Sever: case TokKind::Cut: case TokKind::Raise:
        case TokKind::Lower: {
            std::string name = t.text.empty() ? std::string(tokKindName(t.kind)) : t.text;
            advance();
            return parseCallArgsIfAny(name, t.line);
        }
        case TokKind::LParen: {
            advance();
            auto e = parseExpr();
            expect(TokKind::RParen, "')'");
            return e;
        }
        default:
            error("unexpected token '" + t.text + "' while parsing an expression");
    }
}

} // namespace flycc
