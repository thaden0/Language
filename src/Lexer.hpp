#pragma once
#include "Diagnostic.hpp"
#include "Source.hpp"
#include "Token.hpp"
#include <vector>

// Turns a SourceFile into a token stream. Whitespace and comments are
// discarded. The final token is always TokenKind::End. Invalid characters and
// unterminated strings/comments produce an Error token plus a diagnostic, and
// lexing continues (no exceptions).
class Lexer {
public:
    // `allowHoles`: quasiquote fragment mode (§16.5) — `$name` lexes as one
    // Identifier token whose text includes the '$'. Off for ordinary source,
    // where '$' stays an invalid character.
    Lexer(const SourceFile& file, DiagnosticSink& sink, bool allowHoles = false)
        : file_(file), sink_(sink), allowHoles_(allowHoles) {}

    std::vector<Token> tokenize();

    // Lex a sub-range of the file (a quasiquote's payload): tokens carry real
    // spans into the same buffer, so template diagnostics render normally.
    std::vector<Token> tokenizeRange(uint32_t begin, uint32_t end);

private:
    const SourceFile& file_;
    DiagnosticSink& sink_;
    bool allowHoles_ = false;
    uint32_t pos_ = 0;
    uint32_t limit_ = 0;   // 0 = whole file; else exclusive end for range mode

    bool atEnd() const {
        return pos_ >= (limit_ ? limit_ : (uint32_t)file_.text.size());
    }
    char peek(uint32_t ahead = 0) const {
        uint32_t i = pos_ + ahead;
        uint32_t end = limit_ ? limit_ : (uint32_t)file_.text.size();
        return i < end ? file_.text[i] : '\0';
    }
    char advance() { return file_.text[pos_++]; }
    bool match(char c) {
        if (peek() == c) { ++pos_; return true; }
        return false;
    }

    void skipTrivia();                     // whitespace + comments
    Token make(TokenKind kind, uint32_t start) const;
    // F2: consume a run of `pred(c) || '_'`, validating separator placement
    // (no leading/trailing/doubled '_'); returns the digit count (excluding
    // separators) so an empty run (bare `0x`/`0b`) is detectable.
    uint32_t scanDigitRun(bool (*pred)(char), const char* baseName);
    Token lexNumber(uint32_t start);
    Token lexIdentifier(uint32_t start);
    Token lexString(uint32_t start, char quote);
    // request-string-literal-tail: r"..."/r'...' — 'r' and the opening quote
    // already consumed; no escape processing, single-line only.
    Token lexRawString(uint32_t start, char quote);
    // request-string-literal-tail: """..."""/'''...''' — the three opening
    // quote chars already consumed; ordinary escapes + interpolation still
    // apply later (same content rules as lexString), only raw newlines and
    // the wider delimiter differ.
    Token lexTripleString(uint32_t start, char quote);
    Token lexQuasi(uint32_t start);
};
