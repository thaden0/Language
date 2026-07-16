#pragma once
#include "Source.hpp"
#include <string_view>

enum class TokenKind {
    // Structural
    End,        // end of input
    Error,      // an unrecognized/invalid token (a diagnostic was emitted)

    // Literals & names
    Identifier,
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    QuasiLiteral,   // `...` quasiquote template (payload re-lexed by fragments)

    // Keywords
    KwNamespace,
    KwClass,
    KwStruct,
    KwInterface,
    KwPublic,
    KwPrivate,
    KwNew,
    KwMutating,
    KwDistinct,
    KwConst,
    KwReadonly,
    KwWeak,
    KwGet,
    KwSet,
    KwReturn,
    KwVar,
    KwLet,
    KwAwait,
    KwBind,
    KwInject,
    KwUse,
    KwUses,
    KwIf,
    KwElse,
    KwTry,
    KwCatch,
    KwThrow,
    KwWhile,
    KwFor,
    KwIn,
    KwIs,
    KwMatch,
    KwThis,
    KwTrue,
    KwFalse,
    KwBreak,
    KwContinue,
    KwDo,
    KwUsing,
    KwEnum,

    // Punctuation / operators
    ColonColon,   // ::
    Colon,        // :
    Semicolon,    // ;
    Comma,        // ,
    Dot,          // .
    DotDot,       // ..
    LParen,       // (
    RParen,       // )
    LBrace,       // {
    RBrace,       // }
    LBracket,     // [
    RBracket,     // ]
    Arrow,        // =>
    Eq,           // =
    EqEq,         // ==
    BangEq,       // !=
    Bang,         // !
    Lt,           // <
    Gt,           // >
    Le,           // <=
    Ge,           // >=
    Plus,         // +
    Minus,        // -
    Star,         // *
    Slash,        // /
    Percent,      // %
    PlusEq,       // +=
    MinusEq,      // -=
    StarEq,       // *=
    SlashEq,      // /=
    PercentEq,    // %=
    LtLt,         // <<
    GtGt,         // >>
    AmpAmp,       // &&
    PipePipe,     // ||
    Amp,          // &
    Pipe,         // |
    Question,     // ?
    QuestionQuestion, // ??
    QuestionDot,  // ?.
    At,           // @  (attributes, §16.5)
    Caret,        // ^  (int xor)
    Tilde,        // ~  (int complement, prefix)
};

struct Token {
    TokenKind kind = TokenKind::End;
    SourceSpan span;
    std::string_view text;   // the exact source slice for this token
};

// Human-readable name of a token kind (for dumps and test output).
const char* tokenKindName(TokenKind k);

// If `word` is a language keyword, return its TokenKind; otherwise
// TokenKind::Identifier.
TokenKind keywordKind(std::string_view word);

// Shared int-literal text -> value conversion (F2): the ONE place decimal/
// hex/binary/underscore-separated IntLiteral text becomes a long long,
// called from both Eval.cpp (the oracle, evaluating AST text directly) and
// Lower.cpp (building the IR const pool every other engine consumes) — the
// lexer already validated placement/emptiness, so this just parses. Hex/
// binary reinterpret their full 64-bit pattern via unsigned parsing (so
// e.g. `0x8000000000000000` yields the same bit pattern int64 `1 << 63`
// does); decimal stays plain signed parsing, unchanged.
long long parseIntLiteral(std::string_view text);

// Same idea for FloatLiteral text: the lexer accepts `_` separators in both
// the integer and fractional digit runs (F2), so the value conversion must
// strip them too — `atof`/`strtod` stop at the first `_` otherwise, silently
// truncating (`1_000.000_5` parsed as `1.0` before this existed).
double parseFloatLiteral(std::string_view text);

// F3: apply string-literal escapes to a raw CONTENT slice (no surrounding
// quotes — the caller has already stripped them, or there were none to
// begin with, e.g. an F4 interpolation segment). Escapes: `\n \t \r \0` and
// `\xNN` (exactly two hex digits -> that byte, strings being byte-clean
// through the floor); any other `\<c>` passes `c` through literally (compat
// — `\"`, `\\`, and an unrecognized escape all keep working exactly as
// before this existed). A malformed `\x` (not followed by two hex digits)
// is likewise left alone: `x` passes through and the following characters
// are read as ordinary content, not consumed.
std::string decodeEscapes(std::string_view content);

// Strip a string-literal TOKEN's surrounding quotes, then decodeEscapes —
// the ONE decoder both engines share for an ordinary string literal
// (Eval.cpp evaluates AST text directly; Lower.cpp builds the IR const pool
// every other engine consumes). An F4 interpolation segment (Expr::text is
// already bare content, Expr::isRawSegment set) calls decodeEscapes
// directly instead — see each call site.
std::string decodeStringLiteral(std::string_view raw);
