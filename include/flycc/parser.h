#pragma once
#include "flycc/token.h"
#include "flycc/ast.h"
#include <vector>

namespace flycc {

// Recursive-descent parser with Pratt-style precedence climbing for
// expressions (see docs/architecture.md §3.2).
class Parser {
public:
    Parser(std::vector<Token> tokens, std::string filename);
    Program parseProgram();

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;
    std::string filename_;

    const Token& peek(int ahead = 0) const;
    const Token& advance();
    bool check(TokKind k) const;
    bool match(TokKind k);
    const Token& expect(TokKind k, const char* what);
    [[noreturn]] void error(const std::string& msg);

    StmtPtr parseStmt();
    StmtPtr parseVarDeclOrExprStmt(); // disambiguates `ident = expr` vs a bare call
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseJob();
    StmtPtr parseGive();
    StmtPtr parseDoGrabe();
    StmtPtr parseFor();
    StmtPtr parseBring();
    StmtPtr parseNativeJob(); // native job name(params)  -- milestone 7, §5/§21
    std::vector<StmtPtr> parseBlock(); // consumes { ... }

    ExprPtr parseExpr();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseNot();
    ExprPtr parseComparison();
    ExprPtr parseAdditive();
    ExprPtr parseMultiplicative();
    ExprPtr parseUnary();
    ExprPtr parsePostfix(ExprPtr base);
    ExprPtr parsePrimary();
    ExprPtr parseCallArgsIfAny(std::string ident, int line);
    ExprPtr parseCollLiteral();
    ExprPtr parseBoardLiteral();
    ExprPtr parseTextToken(const Token& t);   // builds TextLit or TextTemplate
    ExprPtr parseInterpolatedSubExpr(const std::string& src, int line); // re-lex/parse a {expr} span
};

} // namespace flycc
