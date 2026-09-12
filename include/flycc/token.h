#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace flycc {

enum class TokKind {
    // literals
    Num, Dec, Text, Yes, No, Emp, Ident,

    // keywords (spec §35)
    If, Orif, Ifnot, While, For, Job, Give, Take, Show, Bring,
    Do, Grabe, Hard, Native,
    Attach, Place, Erase, Count, Seek, Has, Bind, Sever, Cut, Raise, Lower,
    And, Or, Not, In,

    // punctuation / operators
    Plus, Minus, Star, Slash, Percent,
    EqEq, NotEq, Lt, Gt, LtEq, GtEq, Eq,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Colon, Dot,

    Eof, Invalid,
};

// One segment of a (possibly-interpolated) text literal (spec §10, milestone
// 4). `is_expr == false` -> `text` is literal text to embed verbatim (after
// {{ }} un-escaping). `is_expr == true` -> `text` is the RAW, unparsed Fly
// source of an interpolated expression (the substring between the { and
// its matching }); the parser re-lexes/parses this itself (see
// docs/architecture.md §3.1: "the parser re-lexes/parses the interpolation
// spans using a sub-lexer instance").
struct TextPart {
    bool is_expr = false;
    std::string text;
};

struct Token {
    TokKind kind;
    std::string text;   // raw lexeme (identifier name, keyword spelling, etc.)
    int64_t num_val = 0;
    double dec_val = 0.0;
    int line = 0, col = 0;

    // Populated only for TokKind::Text. Always has >= 1 part. When it is
    // exactly one non-expr part, the token represents an ordinary
    // (non-interpolated) text literal and the parser takes the fast path
    // straight to a plain TextLit AST node.
    std::vector<TextPart> text_parts;
};

const char* tokKindName(TokKind k);

} // namespace flycc
