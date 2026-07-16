// Minimal offline test harness (swap for doctest/Catch2 later).
#include "Lexer.hpp"
#include <cstdio>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

// Lex `src` and return the token kinds (excluding the trailing End).
static std::vector<TokenKind> kinds(const std::string& src) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    std::vector<TokenKind> ks;
    for (const Token& t : lexer.tokenize()) {
        if (t.kind == TokenKind::End) break;
        ks.push_back(t.kind);
    }
    return ks;
}

static bool lexClean(const std::string& src) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    lexer.tokenize();
    return !sink.hasErrors();
}

static void test_punctuation_maximal_munch() {
    // Multi-char operators must beat their single-char prefixes.
    CHECK((kinds("::") == std::vector{TokenKind::ColonColon}));
    CHECK((kinds(":") == std::vector{TokenKind::Colon}));
    CHECK((kinds("=>") == std::vector{TokenKind::Arrow}));
    CHECK((kinds("==") == std::vector{TokenKind::EqEq}));
    CHECK((kinds("=") == std::vector{TokenKind::Eq}));
    CHECK((kinds("<<") == std::vector{TokenKind::LtLt}));
    CHECK((kinds(">>") == std::vector{TokenKind::GtGt}));
    CHECK((kinds("..") == std::vector{TokenKind::DotDot}));
    CHECK((kinds(".") == std::vector{TokenKind::Dot}));
    CHECK((kinds("1..5") == std::vector{TokenKind::IntLiteral, TokenKind::DotDot,
                                        TokenKind::IntLiteral}));
    CHECK((kinds("<=") == std::vector{TokenKind::Le}));
    CHECK((kinds("!=") == std::vector{TokenKind::BangEq}));
    CHECK((kinds("|") == std::vector{TokenKind::Pipe}));
    CHECK((kinds("||") == std::vector{TokenKind::PipePipe}));
    // Track 01 F1: new single-char operators (no compound forms in v1).
    CHECK((kinds("^") == std::vector{TokenKind::Caret}));
    CHECK((kinds("~") == std::vector{TokenKind::Tilde}));
}

static void test_operator_selector_is_three_tokens() {
    // '(+)' is NOT a special token — the parser recognizes the pattern.
    CHECK((kinds("(+)") ==
           std::vector{TokenKind::LParen, TokenKind::Plus, TokenKind::RParen}));
    CHECK((kinds("(==)") ==
           std::vector{TokenKind::LParen, TokenKind::EqEq, TokenKind::RParen}));
}

static void test_extract_with_empty_rhs() {
    // 'reader >> ;' lexes cleanly as three tokens.
    CHECK((kinds("reader >> ;") ==
           std::vector{TokenKind::Identifier, TokenKind::GtGt, TokenKind::Semicolon}));
}

static void test_keywords_vs_identifiers() {
    CHECK((kinds("class") == std::vector{TokenKind::KwClass}));
    CHECK((kinds("distinct") == std::vector{TokenKind::KwDistinct}));
    CHECK((kinds("bind") == std::vector{TokenKind::KwBind}));
    CHECK((kinds("inject") == std::vector{TokenKind::KwInject}));
    // Not keywords: primitive names stay identifiers (naming is unsettled, §9).
    CHECK((kinds("int") == std::vector{TokenKind::Identifier}));
    CHECK((kinds("MyClass") == std::vector{TokenKind::Identifier}));
    CHECK((kinds("className") == std::vector{TokenKind::Identifier}));  // prefix, not kw
}

static void test_literals() {
    CHECK((kinds("42") == std::vector{TokenKind::IntLiteral}));
    CHECK((kinds("3.14") == std::vector{TokenKind::FloatLiteral}));
    // A trailing dot is not part of the number: '1.' -> Int, Dot.
    CHECK((kinds("1.") == std::vector{TokenKind::IntLiteral, TokenKind::Dot}));
    CHECK((kinds("\"hello\"") == std::vector{TokenKind::StringLiteral}));
    CHECK((kinds("'hi'") == std::vector{TokenKind::StringLiteral}));
}

static void test_numeric_literal_forms() {
    // Track 01 F2: hex/binary literals + `_` digit separators (all three
    // bases), each lexing as ONE IntLiteral token, clean.
    for (const char* s : {"0xFF", "0XFF", "0xff", "0b1010", "0B1010", "0x0",
                          "0b0", "1_000_000", "0x1_FF", "0b1010_1010",
                          "0xFFFFFFFFFFFFFFFF", "0x8000000000000000"}) {
        CHECK((kinds(s) == std::vector{TokenKind::IntLiteral}));
        CHECK(lexClean(s));
    }
    // `_` in a float's fractional digits too.
    CHECK((kinds("1_000.000_5") == std::vector{TokenKind::FloatLiteral}));
    CHECK(lexClean("1_000.000_5"));
    // A hex/binary literal is never a float; `..` right after one is the
    // range operator, not a malformed fractional part.
    CHECK((kinds("0xA..0xF") == std::vector{TokenKind::IntLiteral, TokenKind::DotDot,
                                            TokenKind::IntLiteral}));
    CHECK(lexClean("0xA..0xF"));
    // Malformed forms are loud lexer errors (each still yields SOME tokens —
    // error recovery, not a crash).
    CHECK(!lexClean("0x"));
    CHECK(!lexClean("0b"));
    CHECK(!lexClean("0b2"));
    CHECK(!lexClean("0x1.5"));
    CHECK(!lexClean("0b1.5"));
    CHECK(!lexClean("1__000"));
    CHECK(!lexClean("1000_"));
    CHECK(!lexClean("0x_1F"));
    CHECK(!lexClean("0x1F_"));
}

static void test_comments_are_trivia() {
    CHECK((kinds("// line\n42") == std::vector{TokenKind::IntLiteral}));
    CHECK((kinds("/* block */ 42") == std::vector{TokenKind::IntLiteral}));
}

static void test_error_recovery() {
    CHECK(!lexClean("\"unterminated"));
    CHECK(!lexClean("/* unterminated"));
    CHECK(!lexClean("`unterminated quasiquote"));
    CHECK(!lexClean("$hole"));      // holes only exist inside quasiquotes
    // '@' lexes now (attributes, §16.5); '#' is still no one's token.
    CHECK(lexClean("@"));
    CHECK(!lexClean("#"));
    // Clean sample exercising several constructs at once.
    CHECK(lexClean("class Counter : Named { public distinct int value = 0; }"));
}

int main() {
    test_punctuation_maximal_munch();
    test_operator_selector_is_three_tokens();
    test_extract_with_empty_rhs();
    test_keywords_vs_identifiers();
    test_literals();
    test_numeric_literal_forms();
    test_comments_are_trivia();
    test_error_recovery();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
