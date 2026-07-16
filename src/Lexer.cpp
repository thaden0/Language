#include "Lexer.hpp"

static bool isIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool isIdentCont(char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}
static bool isDigit(char c) { return c >= '0' && c <= '9'; }
static bool isHexDigit(char c) {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool isBinDigit(char c) { return c == '0' || c == '1'; }

Token Lexer::make(TokenKind kind, uint32_t start) const {
    SourceSpan span{start, pos_ - start};
    return Token{kind, span, std::string_view(file_.text).substr(start, span.length)};
}

void Lexer::skipTrivia() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++pos_;
        } else if (c == '/' && peek(1) == '/') {
            pos_ += 2;
            while (!atEnd() && peek() != '\n') ++pos_;
        } else if (c == '/' && peek(1) == '*') {
            uint32_t start = pos_;
            pos_ += 2;
            while (!atEnd() && !(peek() == '*' && peek(1) == '/')) ++pos_;
            if (atEnd()) {
                sink_.error({start, 2}, "unterminated block comment");
                return;
            }
            pos_ += 2;  // consume */
        } else {
            return;
        }
    }
}

// Consume a maximal run of `pred(c) || c == '_'` starting at pos_ (F2: digit
// separators, all three bases). Validates placement over the whole consumed
// span afterward — a leading '_' (only reachable right after a 0x/0b prefix;
// decimal never starts a run on '_') a trailing one, or a doubled one, are
// each "misplaced digit separator" at the run's span. Returns the count of
// actual digits (excluding separators) so the caller can detect an empty run
// (`0x` / `0b` with nothing valid after the prefix).
uint32_t Lexer::scanDigitRun(bool (*pred)(char), const char* baseName) {
    uint32_t start = pos_;
    while (pred(peek()) || peek() == '_') ++pos_;
    std::string_view run = std::string_view(file_.text).substr(start, pos_ - start);
    uint32_t digits = 0;
    for (char c : run) if (c != '_') ++digits;
    if (!run.empty() &&
        (run.front() == '_' || run.back() == '_' || run.find("__") != std::string_view::npos))
        sink_.error({start, pos_ - start},
                    std::string("misplaced digit separator in ") + baseName + " literal");
    return digits;
}

Token Lexer::lexNumber(uint32_t start) {
    // 0x/0X hex, 0b/0B binary: int-only (no octal, no hex/binary float — a
    // dot-then-digit after one is a malformed literal, not a fractional
    // part; a dot-then-dot is the `..` range operator and is left alone).
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        pos_ += 2;
        uint32_t digits = scanDigitRun(isHexDigit, "hex");
        if (digits == 0) sink_.error({start, pos_ - start}, "hex literal has no digits");
        if (peek() == '.' && isDigit(peek(1)))
            sink_.error({start, pos_ - start + 1}, "hex literals cannot have a fractional part");
        return make(TokenKind::IntLiteral, start);
    }
    if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        pos_ += 2;
        uint32_t digits = scanDigitRun(isBinDigit, "binary");
        if (digits == 0) sink_.error({start, pos_ - start}, "binary literal has no digits");
        if (peek() == '.' && isDigit(peek(1)))
            sink_.error({start, pos_ - start + 1}, "binary literals cannot have a fractional part");
        return make(TokenKind::IntLiteral, start);
    }
    scanDigitRun(isDigit, "decimal");
    bool isFloat = false;
    if (peek() == '.' && isDigit(peek(1))) {
        isFloat = true;
        ++pos_;  // consume '.'
        scanDigitRun(isDigit, "decimal");
    }
    return make(isFloat ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start);
}

Token Lexer::lexIdentifier(uint32_t start) {
    while (isIdentCont(peek())) ++pos_;
    std::string_view word = std::string_view(file_.text).substr(start, pos_ - start);
    return make(keywordKind(word), start);
}

// A quasiquote literal `...` (§16.5): raw capture, newlines allowed, \` for a
// literal backtick. The payload is re-lexed as language source by the fragment
// parser, so no other escape processing happens here.
Token Lexer::lexQuasi(uint32_t start) {
    while (!atEnd()) {
        char c = peek();
        if (c == '\\' && peek(1) == '`') { pos_ += 2; continue; }
        ++pos_;
        if (c == '`') return make(TokenKind::QuasiLiteral, start);
    }
    sink_.error({start, pos_ - start}, "unterminated quasiquote literal");
    return make(TokenKind::Error, start);
}

Token Lexer::lexString(uint32_t start, char quote) {
    // opening quote already consumed
    while (!atEnd()) {
        char c = peek();
        if (c == '\\') { pos_ += 2; continue; }  // skip escaped char
        if (c == '\n') break;                      // unterminated
        ++pos_;
        if (c == quote) return make(TokenKind::StringLiteral, start);
    }
    sink_.error({start, pos_ - start}, "unterminated string literal");
    return make(TokenKind::Error, start);
}

std::vector<Token> Lexer::tokenizeRange(uint32_t begin, uint32_t end) {
    pos_ = begin;
    limit_ = end;
    return tokenize();
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        skipTrivia();
        uint32_t start = pos_;
        if (atEnd()) {
            tokens.push_back(make(TokenKind::End, start));
            return tokens;
        }

        char c = advance();
        switch (c) {
            case '(': tokens.push_back(make(TokenKind::LParen, start)); break;
            case ')': tokens.push_back(make(TokenKind::RParen, start)); break;
            case '{': tokens.push_back(make(TokenKind::LBrace, start)); break;
            case '}': tokens.push_back(make(TokenKind::RBrace, start)); break;
            case '[': tokens.push_back(make(TokenKind::LBracket, start)); break;
            case ']': tokens.push_back(make(TokenKind::RBracket, start)); break;
            case ',': tokens.push_back(make(TokenKind::Comma, start)); break;
            case '.': tokens.push_back(make(match('.') ? TokenKind::DotDot
                                                       : TokenKind::Dot, start)); break;
            case ';': tokens.push_back(make(TokenKind::Semicolon, start)); break;
            case '?': tokens.push_back(make(match('?') ? TokenKind::QuestionQuestion
                                          : match('.') ? TokenKind::QuestionDot
                                                       : TokenKind::Question, start)); break;
            case '+': tokens.push_back(make(match('=') ? TokenKind::PlusEq
                                                       : TokenKind::Plus, start)); break;
            case '-': tokens.push_back(make(match('=') ? TokenKind::MinusEq
                                                       : TokenKind::Minus, start)); break;
            case '*': tokens.push_back(make(match('=') ? TokenKind::StarEq
                                                       : TokenKind::Star, start)); break;
            case '/': tokens.push_back(make(match('=') ? TokenKind::SlashEq
                                                       : TokenKind::Slash, start)); break;
            case '%': tokens.push_back(make(match('=') ? TokenKind::PercentEq
                                                       : TokenKind::Percent, start)); break;

            case ':': tokens.push_back(make(match(':') ? TokenKind::ColonColon
                                                       : TokenKind::Colon, start)); break;
            case '=': tokens.push_back(make(match('>') ? TokenKind::Arrow
                                          : match('=') ? TokenKind::EqEq
                                                       : TokenKind::Eq, start)); break;
            case '!': tokens.push_back(make(match('=') ? TokenKind::BangEq
                                                       : TokenKind::Bang, start)); break;
            case '<': tokens.push_back(make(match('<') ? TokenKind::LtLt
                                          : match('=') ? TokenKind::Le
                                                       : TokenKind::Lt, start)); break;
            case '>': tokens.push_back(make(match('>') ? TokenKind::GtGt
                                          : match('=') ? TokenKind::Ge
                                                       : TokenKind::Gt, start)); break;
            case '&': tokens.push_back(make(match('&') ? TokenKind::AmpAmp
                                                       : TokenKind::Amp, start)); break;
            case '|': tokens.push_back(make(match('|') ? TokenKind::PipePipe
                                                       : TokenKind::Pipe, start)); break;
            case '^': tokens.push_back(make(TokenKind::Caret, start)); break;
            case '~': tokens.push_back(make(TokenKind::Tilde, start)); break;

            case '"':
            case '\'':
                tokens.push_back(lexString(start, c));
                break;

            case '@': tokens.push_back(make(TokenKind::At, start)); break;
            case '`': tokens.push_back(lexQuasi(start)); break;

            case '$':
                // Fragment mode only: `$name` is one Identifier (text includes
                // the '$') — a quasiquote hole, resolved at expansion time.
                if (allowHoles_ && isIdentStart(peek())) {
                    while (isIdentCont(peek())) ++pos_;
                    tokens.push_back(make(TokenKind::Identifier, start));
                } else {
                    sink_.error({start, 1},
                                allowHoles_
                                    ? std::string("expected a name after '$'")
                                    : std::string("unexpected character '$' "
                                                  "(holes only exist inside quasiquotes)"));
                    tokens.push_back(make(TokenKind::Error, start));
                }
                break;

            default:
                if (isDigit(c)) {
                    pos_ = start;
                    tokens.push_back(lexNumber(start));
                } else if (isIdentStart(c)) {
                    pos_ = start;
                    tokens.push_back(lexIdentifier(start));
                } else {
                    sink_.error({start, 1},
                                std::string("unexpected character '") + c + "'");
                    tokens.push_back(make(TokenKind::Error, start));
                }
                break;
        }
    }
}
