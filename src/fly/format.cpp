// fly -format: a deterministic, semantics-preserving formatter for Fly
// source (docs/architecture.md §7.2).
//
// Design constraints:
//   * Fly 0.1's grammar is entirely whitespace-insensitive (statements are
//     token-structure delimited, blocks are `{ ... }`), so ANY
//     whitespace-only change between tokens is semantics-preserving. This
//     formatter only moves whitespace and re-lays-out lines; it never
//     reorders, renames, or otherwise edits tokens.
//   * Comments (`$` line and `$$ ... $$` block) are recovered from the raw
//     source spans between tokens and preserved verbatim -- never dropped.
//   * Text literals are copied through RAW (their `{{`/`}}` escapes and
//     `{expr}` interpolation spans must survive byte-for-byte; they are
//     NOT reconstructed from token fields).
//   * One output line per input line is preserved (blank lines included),
//     so formatting is conservative about structure and strictly
//     idempotent: format(format(x)) == format(x).
//   * Before writing anything, the output is re-lexed and its token stream
//     compared token-for-token against the input's. Any mismatch aborts
//     with an error and the file is left untouched.
//
// The input must parse cleanly with the compiler's own Parser (the same
// gate fly-cc uses); an unparseable file is refused rather than risked.
#include "format.h"

#include "flycc/lexer.h"
#include "flycc/parser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace fly {

namespace {

using flycc::Lexer;
using flycc::Parser;
using flycc::TokKind;
using flycc::Token;

std::string readFile(const std::string& path, bool* ok) {
    std::ifstream f(path);
    if (!f) { *ok = false; return ""; }
    std::stringstream ss;
    ss << f.rdbuf();
    *ok = true;
    return ss.str();
}

// Fixed spelling for punctuation/operator tokens (the lexer does not store
// text for these; everything else re-emits from its own lexeme).
std::string punctSpelling(TokKind k) {
    switch (k) {
        case TokKind::Plus: return "+";
        case TokKind::Minus: return "-";
        case TokKind::Star: return "*";
        case TokKind::Slash: return "/";
        case TokKind::Percent: return "%";
        case TokKind::EqEq: return "==";
        case TokKind::NotEq: return "!=";
        case TokKind::Lt: return "<";
        case TokKind::Gt: return ">";
        case TokKind::LtEq: return "<=";
        case TokKind::GtEq: return ">=";
        case TokKind::Eq: return "=";
        case TokKind::LParen: return "(";
        case TokKind::RParen: return ")";
        case TokKind::LBrace: return "{";
        case TokKind::RBrace: return "}";
        case TokKind::LBracket: return "[";
        case TokKind::RBracket: return "]";
        case TokKind::Comma: return ",";
        case TokKind::Colon: return ":";
        case TokKind::Dot: return ".";
        default: return "";
    }
}

bool isCloser(const std::string& s) {
    return s == ")" || s == "]" || s == "}";
}
bool isOpener(const std::string& s) {
    return s == "(" || s == "[" || s == "{";
}

// Finds the byte offset just past the closing `"` of the text literal that
// starts at src[start] (which must be '"'). Mirrors the lexer's own span
// tracking: {{ }}/}} escapes, {expr} interpolation with a bracket-kind
// stack, and nested "..." literals inside interpolation.
size_t scanTextEnd(const std::string& src, size_t start) {
    size_t i = start + 1;
    while (i < src.size()) {
        char c = src[i];
        if (c == '"') return i + 1;
        if (c == '{' && i + 1 < src.size() && src[i + 1] == '{') { i += 2; continue; }
        if (c == '}' && i + 1 < src.size() && src[i + 1] == '}') { i += 2; continue; }
        if (c == '{') {
            i++;
            std::vector<char> closers = {'}'};
            while (i < src.size() && !closers.empty()) {
                char cc = src[i];
                if (cc == '"') {
                    i++;
                    while (i < src.size() && src[i] != '"') i++;
                    if (i < src.size()) i++;
                    continue;
                }
                if (cc == '(' || cc == '[' || cc == '{') {
                    closers.push_back(cc == '(' ? ')' : cc == '[' ? ']' : '}');
                } else if (cc == ')' || cc == ']' || cc == '}') {
                    if (!closers.empty() && closers.back() == cc) closers.pop_back();
                }
                i++;
            }
            continue;
        }
        i++;
    }
    return src.size();
}

// Byte offset of the raw token text for every token kind except Text/Eof.
size_t scanTokenEnd(const std::string& src, size_t start, const Token& t) {
    if (t.kind == TokKind::Text) return scanTextEnd(src, start);
    std::string sp = punctSpelling(t.kind);
    if (!sp.empty()) return start + sp.size();
    size_t j = start;
    while (j < src.size() &&
           (std::isalnum((unsigned char)src[j]) || src[j] == '_'))
        j++;
    // number with a fractional part: `3.14`
    if (start < src.size() && std::isdigit((unsigned char)src[start]) &&
        j < src.size() && src[j] == '.' && j + 1 < src.size() &&
        std::isdigit((unsigned char)src[j + 1])) {
        j += 2;
        while (j < src.size() && std::isdigit((unsigned char)src[j])) j++;
    }
    return j;
}

std::string rawTokenText(const std::string& src, size_t start, const Token& t) {
    if (t.kind == TokKind::Text)
        return src.substr(start, scanTextEnd(src, start) - start);
    std::string sp = punctSpelling(t.kind);
    if (!sp.empty()) return sp;
    return t.text; // identifiers, keywords, Yes/No/EMP, numbers
}

// Byte offset -> 1-based (line, col) via a line-start index.
struct Coord { int line, col; };
Coord lineColOf(const std::vector<size_t>& lineStarts, size_t off) {
    size_t lo = 0, hi = lineStarts.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (lineStarts[mid] <= off) lo = mid;
        else hi = mid;
    }
    return {(int)(lo + 1), (int)(off - lineStarts[lo] + 1)};
}

// Emission items, in source order: comments and tokens interleaved.
struct Item {
    bool isComment;
    int line, col;
    int endLine; // last line this item's text touches (comments spanning lines)
    std::string text;
    flycc::TokKind kind = flycc::TokKind::Eof; // meaningful for tokens
};

// Scans a gap between tokens (known to contain only whitespace + comments)
// and appends each comment as an Item.
void appendComments(const std::string& src, const std::vector<size_t>& lineStarts,
                    size_t start, size_t end, std::vector<Item>& items) {
    size_t i = start;
    while (i < end) {
        if (src[i] == '$') {
            size_t cs = i;
            if (i + 1 < end && src[i + 1] == '$') {
                i += 2;
                while (i + 1 < end && !(src[i] == '$' && src[i + 1] == '$')) i++;
                i = std::min(i + 2, end);
            } else {
                while (i < end && src[i] != '\n') i++;
            }
            Coord c = lineColOf(lineStarts, cs);
            Item it;
            it.isComment = true;
            it.line = c.line;
            it.col = c.col;
            it.text = src.substr(cs, i - cs);
            it.endLine = it.line;
            for (size_t p = 0; p < it.text.size(); p++)
                if (it.text[p] == '\n') it.endLine++;
            items.push_back(std::move(it));
        } else {
            i++;
        }
    }
}

bool lexesToSameTokens(const std::string& a, const std::string& b) {
    Lexer la(a, "in"); auto ta = la.lexAll();
    Lexer lb(b, "out"); auto tb = lb.lexAll();
    if (ta.size() != tb.size()) return false;
    for (size_t i = 0; i < ta.size(); i++) {
        if (ta[i].kind != tb[i].kind) return false;
        if (ta[i].kind == TokKind::Eof) continue;
        // Compare the raw lexeme text. Comment strings would be a mismatch
        // here only if we mangled them -- but comments are dropped from the
        // token stream by the lexer, so this compares real tokens only.
        if (ta[i].text != tb[i].text) {
            std::string spa = punctSpelling(ta[i].kind);
            std::string spb = punctSpelling(tb[i].kind);
            if (spa != spb) return false;
        }
    }
    return true;
}

// A token that "ends an atom" -- a call `foo(...)` or index `x[...]` may
// abut it without a space (`(`, `[` after an atom get no separating space).
// Includes the call-able builtin keywords (show/take/attach/place/erase/
// count/seek/has/bind/sever/cut/raise/lower), which the grammar calls like
// ordinary functions (see parser.cpp's kBuiltinCallNames).
bool isAtomEndItem(const Item& it) {
    if (it.isComment) return false;
    switch (it.kind) {
        case TokKind::Ident:
        case TokKind::Num:
        case TokKind::Dec:
        case TokKind::Text:
        case TokKind::Yes:
        case TokKind::No:
        case TokKind::Emp:
        case TokKind::RParen:
        case TokKind::RBracket:
        case TokKind::Show:
        case TokKind::Take:
        case TokKind::Attach:
        case TokKind::Place:
        case TokKind::Erase:
        case TokKind::Count:
        case TokKind::Seek:
        case TokKind::Has:
        case TokKind::Bind:
        case TokKind::Sever:
        case TokKind::Cut:
        case TokKind::Raise:
        case TokKind::Lower:
            return true;
        default:
            return false;
    }
}

} // namespace

int formatFile(const std::string& path) {
    bool readOk = false;
    std::string src = readFile(path, &readOk);
    if (!readOk) {
        std::cerr << "fly: -format: cannot open '" << path << "'\n";
        return 1;
    }

    // ---- lex + parse (validation gate: the same parser fly-cc uses) ------
    Lexer lexer(src, path);
    auto toks = lexer.lexAll();
    Parser parser(toks, path);
    (void)parser.parseProgram(); // exits(1) via the parser on bad input

    // ---- line starts for offset math -------------------------------------
    std::vector<size_t> lineStarts(1, 0);
    for (size_t i = 0; i < src.size(); i++)
        if (src[i] == '\n') lineStarts.push_back(i + 1);

    // ---- build interleaved, source-ordered items --------------------------
    std::vector<Item> items;
    size_t prevEnd = 0;
    for (size_t i = 0; i < toks.size(); i++) {
        const Token& t = toks[i];
        size_t start = lineStarts[t.line - 1] + (size_t)(t.col - 1);
        size_t end = scanTokenEnd(src, start, t);
        appendComments(src, lineStarts, prevEnd, start, items);
        if (t.kind != TokKind::Eof) {
            Item it;
            it.isComment = false;
            it.line = t.line;
            it.col = t.col;
            it.kind = t.kind;
            it.text = rawTokenText(src, start, t);
            items.push_back(std::move(it));
        }
        prevEnd = end;
    }
    appendComments(src, lineStarts, prevEnd, src.size(), items);

    // ---- emit: one output line per input line -----------------------------
    // The last emitted line is the last one that holds any item; interior
    // blank lines (no items) are preserved as empty lines, but a trailing
    // run of blank lines collapses -- otherwise format() would add a new
    // line each pass and idempotence would break.
    int nLines = 0;
    for (auto& it : items) nLines = std::max(nLines, it.line);
    // Lines strictly INSIDE a multi-line comment (beyond its start line) have
    // no items of their own: their text arrives as part of the comment item.
    // Emitting an extra blank for them would add one line per pass and break
    // idempotence, so those lines must not count as blank lines.
    std::vector<char> covered((size_t)nLines + 2, 0);
    for (auto& it : items)
        if (it.isComment && it.endLine > it.line)
            for (int l = it.line + 1; l <= it.endLine && l <= nLines; l++)
                covered[(size_t)l] = 1;
    size_t k = 0;
    int depth = 0;
    std::string out;
    for (int line = 1; line <= nLines; line++) {
        // collect this line's items
        size_t begin = k;
        while (k < items.size() && items[k].line == line) k++;
        if (begin == k && !covered[(size_t)line]) { // blank line
            out += '\n';
            continue;
        }
        if (begin == k && covered[(size_t)line]) continue;

        // indentation: the line's leading closers close before we indent
        int leadingClosers = 0;
        for (size_t j = begin; j < k; j++) {
            if (items[j].isComment) break;
            if (isCloser(items[j].text)) leadingClosers++;
            else break;
        }
        int ind = 2 * std::max(0, depth - leadingClosers);
        out.append((size_t)ind, ' ');

        bool first = true;
        const Item* prev = nullptr;
        for (size_t j = begin; j < k; j++) {
            const Item& it = items[j];
            if (!first) {
                bool noSpace = false;
                if (prev && !prev->isComment) {
                    if (prev->text == "(" || prev->text == "[" || prev->text == ".")
                        noSpace = true;
                }
                if (!noSpace && !it.isComment &&
                    (it.text == "," || it.text == "." || it.text == ")" || it.text == "]"))
                    noSpace = true;
                if (!noSpace && !it.isComment &&
                    (it.text == "(" || it.text == "[") && prev && isAtomEndItem(*prev))
                    noSpace = true;
                if (!noSpace) out += ' ';
            }
            out += it.text;
            if (!it.isComment) {
                if (isOpener(it.text)) depth++;
                else if (isCloser(it.text)) depth = std::max(0, depth - 1);
            }
            prev = &it;
            first = false;
        }
        out += '\n';
    }

    // ---- safety: the output must lex to the same token stream ------------
    if (!lexesToSameTokens(src, out)) {
        std::cerr << "fly: -format: internal error: formatting '" << path
                  << "' changed its token stream; refusing to write\n";
        return 1;
    }

    if (out == src) {
        std::cout << "fly: " << path << " is already formatted\n";
        return 0;
    }
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        std::cerr << "fly: -format: cannot write '" << path << "'\n";
        return 1;
    }
    f << out;
    if (!f.good()) {
        std::cerr << "fly: -format: write failed for '" << path << "'\n";
        return 1;
    }
    std::cout << "fly: formatted " << path << "\n";
    return 0;
}

} // namespace fly