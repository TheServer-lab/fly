#include "flycc/lexer.h"
#include <cctype>
#include <iostream>
#include <unordered_map>

namespace flycc {

static const std::unordered_map<std::string, TokKind> kKeywords = {
    {"if", TokKind::If}, {"orif", TokKind::Orif}, {"ifnot", TokKind::Ifnot},
    {"while", TokKind::While}, {"for", TokKind::For}, {"in", TokKind::In},
    {"job", TokKind::Job}, {"give", TokKind::Give},
    {"take", TokKind::Take}, {"show", TokKind::Show}, {"bring", TokKind::Bring},
    {"do", TokKind::Do}, {"grabe", TokKind::Grabe}, {"hard", TokKind::Hard},
    // milestone 7, §5's "native declaration syntax" open item (§21):
    // `native job rt_name(params)` declares a Fly-callable name that's
    // implemented in libflyrt (runtime/flyrt.h's `fly_<name>` C symbol)
    // rather than a Fly job body -- see parser.cpp's parseNativeJob.
    {"native", TokKind::Native},
    {"attach", TokKind::Attach}, {"place", TokKind::Place}, {"erase", TokKind::Erase},
    {"count", TokKind::Count}, {"seek", TokKind::Seek}, {"has", TokKind::Has},
    {"bind", TokKind::Bind}, {"sever", TokKind::Sever}, {"cut", TokKind::Cut},
    {"raise", TokKind::Raise}, {"lower", TokKind::Lower},
    {"and", TokKind::And}, {"or", TokKind::Or}, {"not", TokKind::Not},
    // Yes / No / EMP are handled as exact-spelling literals below, NOT here,
    // per spec §35: "yes", "YES", "emp" etc. must lex as ordinary identifiers.
};

const char* tokKindName(TokKind k) {
    switch (k) {
        case TokKind::Num: return "num";
        case TokKind::Dec: return "dec";
        case TokKind::Text: return "text";
        case TokKind::Yes: return "Yes";
        case TokKind::No: return "No";
        case TokKind::Emp: return "EMP";
        case TokKind::Ident: return "identifier";
        case TokKind::Eof: return "eof";
        default: return "token";
    }
}

Lexer::Lexer(std::string source, std::string filename)
    : src_(std::move(source)), filename_(std::move(filename)) {}

char Lexer::peek(int ahead) const {
    size_t p = pos_ + ahead;
    return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { line_++; col_ = 1; } else { col_++; }
    return c;
}

bool Lexer::match(char expected) {
    if (peek() != expected) return false;
    advance();
    return true;
}

Token Lexer::make(TokKind k, std::string text) {
    Token t;
    t.kind = k;
    t.text = std::move(text);
    t.line = line_;
    t.col = col_;
    return t;
}

void Lexer::error(const std::string& msg) {
    std::cerr << filename_ << ":" << line_ << ":" << col_ << ": lex error: " << msg << "\n";
    std::exit(1);
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(); continue; }
        if (c == '$') {
            if (peek(1) == '$') {
                // $$ ... $$ block comment (spec §2.2)
                advance(); advance();
                while (!(peek() == '$' && peek(1) == '$')) {
                    if (peek() == '\0') error("unterminated $$ block comment");
                    advance();
                }
                advance(); advance();
                continue;
            }
            // $ line comment (spec §2.1)
            while (peek() != '\n' && peek() != '\0') advance();
            continue;
        }
        break;
    }
}

Token Lexer::lexNumber() {
    int startLine = line_, startCol = col_;
    std::string s;
    while (isdigit((unsigned char)peek())) s += advance();
    bool isDec = false;
    if (peek() == '.' && isdigit((unsigned char)peek(1))) {
        isDec = true;
        s += advance();
        while (isdigit((unsigned char)peek())) s += advance();
    }
    Token t = isDec ? make(TokKind::Dec, s) : make(TokKind::Num, s);
    t.line = startLine; t.col = startCol;
    if (isDec) t.dec_val = std::stod(s);
    else t.num_val = std::stoll(s);
    return t;
}

Token Lexer::lexText() {
    int startLine = line_, startCol = col_;
    advance(); // consume opening "
    std::vector<TextPart> parts;
    std::string curLiteral;

    auto flushLiteral = [&]() {
        if (!curLiteral.empty()) {
            parts.push_back(TextPart{false, curLiteral});
            curLiteral.clear();
        }
    };

    while (peek() != '"') {
        if (peek() == '\0') error("unterminated text literal");
        char c = peek();
        if (c == '{' && peek(1) == '{') { advance(); advance(); curLiteral += '{'; continue; } // {{ escape, §10.2
        if (c == '}' && peek(1) == '}') { advance(); advance(); curLiteral += '}'; continue; } // }} escape
        if (c == '}') error("unmatched '}' in text literal (use '}}' for a literal '}')");
        if (c == '{') {
            // Real interpolation span, spec §10. Scan the raw source of the
            // expression up to its matching '}', tracking a bracket-kind
            // stack (so nested (), [], {} inside the expression -- e.g. a
            // nested call or board/coll literal -- don't confuse us into
            // stopping early) and skipping over any nested "..." text
            // literal's contents verbatim (their own interpolation, if any,
            // is out of scope for this MVP nesting depth). This is exactly
            // the lexer-level brace tracking described in
            // docs/architecture.md §3.1.
            flushLiteral();
            advance(); // consume '{'
            std::string exprSrc;
            std::vector<char> closers = {'}'};
            while (!closers.empty()) {
                if (peek() == '\0') error("unterminated interpolation expression, missing '}'");
                char cc = peek();
                if (cc == '"') {
                    exprSrc += advance(); // opening quote of the nested literal
                    while (peek() != '"') {
                        if (peek() == '\0') error("unterminated text literal inside interpolation expression");
                        exprSrc += advance();
                    }
                    exprSrc += advance(); // closing quote
                    continue;
                }
                if (cc == '(' || cc == '[' || cc == '{') {
                    closers.push_back(cc == '(' ? ')' : cc == '[' ? ']' : '}');
                    exprSrc += advance();
                    continue;
                }
                if (cc == ')' || cc == ']' || cc == '}') {
                    if (closers.back() != cc)
                        error(std::string("mismatched '") + cc + "' inside interpolation expression");
                    closers.pop_back();
                    if (closers.empty()) { advance(); break; } // matching '}' for the { that opened this span
                    exprSrc += advance();
                    continue;
                }
                exprSrc += advance();
            }
            parts.push_back(TextPart{true, exprSrc});
            continue;
        }
        curLiteral += advance();
    }
    advance(); // consume closing "
    flushLiteral();
    if (parts.empty()) parts.push_back(TextPart{false, ""}); // "" -> one empty literal part

    Token t = make(TokKind::Text, "");
    t.line = startLine; t.col = startCol;
    t.text_parts = std::move(parts);
    return t;
}

Token Lexer::lexIdentOrKeyword() {
    int startLine = line_, startCol = col_;
    std::string s;
    while (isalnum((unsigned char)peek()) || peek() == '_') s += advance();

    Token t;
    t.line = startLine; t.col = startCol;

    // Exact-spelling literals (spec §35): only these exact spellings.
    if (s == "Yes") { t.kind = TokKind::Yes; t.text = s; return t; }
    if (s == "No")  { t.kind = TokKind::No;  t.text = s; return t; }
    if (s == "EMP") { t.kind = TokKind::Emp; t.text = s; return t; }

    auto it = kKeywords.find(s);
    if (it != kKeywords.end()) { t.kind = it->second; t.text = s; return t; }

    t.kind = TokKind::Ident;
    t.text = s;
    return t;
}

Token Lexer::lexOne() {
    skipWhitespaceAndComments();
    if (peek() == '\0') return make(TokKind::Eof);

    char c = peek();
    if (isdigit((unsigned char)c)) return lexNumber();
    if (c == '"') return lexText();
    if (isalpha((unsigned char)c) || c == '_') return lexIdentOrKeyword();

    int startLine = line_, startCol = col_;
    advance();
    Token t;
    switch (c) {
        case '+': t = make(TokKind::Plus); break;
        case '-': t = make(TokKind::Minus); break;
        case '*': t = make(TokKind::Star); break;
        case '/': t = make(TokKind::Slash); break;
        case '%': t = make(TokKind::Percent); break;
        case '(': t = make(TokKind::LParen); break;
        case ')': t = make(TokKind::RParen); break;
        case '{': t = make(TokKind::LBrace); break;
        case '}': t = make(TokKind::RBrace); break;
        case '[': t = make(TokKind::LBracket); break;
        case ']': t = make(TokKind::RBracket); break;
        case ',': t = make(TokKind::Comma); break;
        case ':': t = make(TokKind::Colon); break;
        // milestone 7, §5.3: `.` for namespaced calls like `path.join(...)`.
        // Bare `.` never appears elsewhere in the grammar (a leading-digit
        // decimal point is consumed inside lexNumber before this switch is
        // ever reached), so this is unambiguous.
        case '.': t = make(TokKind::Dot); break;
        case '=': t = match('=') ? make(TokKind::EqEq) : make(TokKind::Eq); break;
        case '!': if (match('=')) { t = make(TokKind::NotEq); break; } error("unexpected '!'");
        case '<': t = match('=') ? make(TokKind::LtEq) : make(TokKind::Lt); break;
        case '>': t = match('=') ? make(TokKind::GtEq) : make(TokKind::Gt); break;
        default:
            error(std::string("unexpected character '") + c + "'");
    }
    t.line = startLine; t.col = startCol;
    return t;
}

std::vector<Token> Lexer::lexAll() {
    std::vector<Token> out;
    for (;;) {
        Token t = lexOne();
        out.push_back(t);
        if (t.kind == TokKind::Eof) break;
    }
    return out;
}

} // namespace flycc
