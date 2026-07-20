#include "Parser.hpp"
#include "Lexer.hpp"
#include <string>

// ---------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------

// A keyword that may also serve as a member name after '.' / '::'. The keyword
// sense only binds at statement/declaration position, so member access can use
// these freely (client.get(...), resp.is, x.in).
static bool isContextualName(TokenKind k) {
    switch (k) {
        case TokenKind::KwGet: case TokenKind::KwSet: case TokenKind::KwIs:
        case TokenKind::KwIn: case TokenKind::KwUse: case TokenKind::KwUses:
        case TokenKind::KwNew: case TokenKind::KwBind: case TokenKind::KwInject:
            return true;
        default:
            return false;
    }
}

static bool canStartExpr(TokenKind k) {
    switch (k) {
        case TokenKind::Identifier:
        case TokenKind::IntLiteral:
        case TokenKind::FloatLiteral:
        case TokenKind::StringLiteral:
        case TokenKind::RawStringLiteral:
        case TokenKind::LParen:
        case TokenKind::LBracket:
        case TokenKind::Bang:
        case TokenKind::Minus:
        case TokenKind::Tilde:
        case TokenKind::KwThis:
        case TokenKind::KwAwait:
        case TokenKind::KwInject:
        case TokenKind::KwMatch:
        case TokenKind::KwTrue:
        case TokenKind::KwFalse:
            return true;
        default:
            return false;
    }
}

// Binding power of an infix operator (0 == not infix). Ternary (2) and the
// assignment right-assoc case are handled directly in parseExpr.
static int infixBP(TokenKind k) {
    switch (k) {
        case TokenKind::Eq:
        case TokenKind::PlusEq:
        case TokenKind::MinusEq:
        case TokenKind::StarEq:
        case TokenKind::SlashEq:
        case TokenKind::PercentEq: return 1;
        case TokenKind::PipePipe:  return 3;
        case TokenKind::QuestionQuestion: return 3;
        case TokenKind::AmpAmp:    return 4;
        case TokenKind::Pipe:      return 5;
        case TokenKind::Amp:       return 5;
        case TokenKind::Caret:     return 5;
        case TokenKind::EqEq:
        case TokenKind::BangEq:    return 6;
        case TokenKind::Lt:
        case TokenKind::Gt:
        case TokenKind::Le:
        case TokenKind::Ge:        return 7;
        case TokenKind::LtLt:
        case TokenKind::GtGt:      return 8;
        case TokenKind::Plus:
        case TokenKind::Minus:     return 9;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:   return 10;
        default:                   return 0;
    }
}

// Skip a balanced `< ... >` group starting at index i (tokens[i] == Lt).
// Treats `>>` as two closers so single-level generics parse; nested generics
// that bottom out in `>>` are approximated (good enough for scans).
static size_t skipAngles(const std::vector<Token>& t, size_t i) {
    int depth = 0;
    for (; i < t.size(); ++i) {
        TokenKind k = t[i].kind;
        if (k == TokenKind::Lt) { ++depth; }
        else if (k == TokenKind::Gt) { if (--depth == 0) return i + 1; }
        else if (k == TokenKind::GtGt) { depth -= 2; if (depth <= 0) return i + 1; }
        else if (k == TokenKind::End) return i;
    }
    return i;
}

// Skip a type's generic-arg and union suffix, returning the index just past it.
static size_t skipTypeSuffix(const std::vector<Token>& t, size_t i) {
    // ::-qualified head segments: A::B::C (§12 qualified type names).
    while (i + 1 < t.size() && t[i].kind == TokenKind::ColonColon &&
           t[i + 1].kind == TokenKind::Identifier)
        i += 2;
    if (i < t.size() && t[i].kind == TokenKind::Lt) i = skipAngles(t, i);
    if (i < t.size() && t[i].kind == TokenKind::Question) ++i;
    while (i < t.size() && t[i].kind == TokenKind::Pipe) {
        ++i;                                                  // '|'
        if (i < t.size() && t[i].kind == TokenKind::Identifier) ++i;
        else break;
        while (i + 1 < t.size() && t[i].kind == TokenKind::ColonColon &&   // | A::B
               t[i + 1].kind == TokenKind::Identifier)
            i += 2;
        if (i < t.size() && t[i].kind == TokenKind::Lt) i = skipAngles(t, i);
        if (i < t.size() && t[i].kind == TokenKind::Question) ++i;
    }
    return i;
}

// bug #44: skip a full type starting at i, INCLUDING a leading lambda/function
// type `(T, ...) => R` (whose R may itself be a lambda type). Returns the index
// just past the type, or `i` unchanged if the tokens at i do not begin a type.
// Used by the namespace/top-level and statement-level decl-vs-expression
// lookahead so `(Foo) => Foo makeHandler() {…}` and `(Foo) => Foo h = …;` are
// recognized as declarations the same way the class-member grammar already does.
static size_t skipTypeSuffix(const std::vector<Token>& t, size_t i);
static size_t skipTypeFull(const std::vector<Token>& t, size_t i) {
    if (i >= t.size()) return i;
    if (t[i].kind == TokenKind::LParen) {
        int depth = 0; size_t j = i;
        for (; j < t.size(); ++j) {
            if (t[j].kind == TokenKind::LParen) ++depth;
            else if (t[j].kind == TokenKind::RParen) { if (--depth == 0) { ++j; break; } }
            else if (t[j].kind == TokenKind::End) return i;   // unbalanced -> not a type
        }
        if (j < t.size() && t[j].kind == TokenKind::Arrow)    // `(...) =>` -> lambda type
            return skipTypeFull(t, j + 1);                    // return type (may be lambda)
        return i;                                             // a plain parenthesized expr
    }
    if (t[i].kind == TokenKind::Identifier) return skipTypeSuffix(t, i + 1);
    return i;
}

static ExprPtr mkExpr(ExprKind k, SourceSpan sp) {
    auto e = std::make_unique<Expr>(k);
    e->span = sp;
    return e;
}
static StmtPtr mkStmt(StmtKind k, SourceSpan sp) {
    auto s = std::make_unique<Stmt>(k);
    s->span = sp;
    return s;
}
static TypeRefPtr mkType(TypeKind k, SourceSpan sp) {
    auto t = std::make_unique<TypeRef>(k);
    t->span = sp;
    return t;
}

// ---------------------------------------------------------------------------
//  token cursor
// ---------------------------------------------------------------------------

const Token& Parser::peek(size_t ahead) const {
    size_t i = pos_ + ahead;
    if (i >= tokens_.size()) return tokens_.back();  // always the End token
    return tokens_[i];
}

const Token& Parser::advance() {
    const Token& t = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) ++pos_;
    return t;
}

bool Parser::accept(TokenKind k) {
    if (at(k)) { advance(); return true; }
    return false;
}

bool Parser::expect(TokenKind k, const char* what) {
    if (at(k)) { advance(); return true; }
    error(what);
    return false;
}

// Close a generic argument list. A `>>` (right-shift) that appears where a
// single `>` is expected is split: consume one `>`, leave the other in place.
bool Parser::expectGt() {
    if (at(TokenKind::Gt)) { advance(); return true; }
    if (at(TokenKind::GtGt)) {
        Token& t = tokens_[pos_];
        t.kind = TokenKind::Gt;                 // the remaining single '>'
        t.span.offset += 1; t.span.length = 1;
        t.text = std::string_view(file_.text).substr(t.span.offset, 1);
        return true;
    }
    error("'>'");
    return false;
}

void Parser::error(const char* message) {
    sink_.error(cur().span, std::string("expected ") + message);
}

void Parser::synchronize() {
    while (!atEnd()) {
        if (accept(TokenKind::Semicolon)) return;
        switch (cur().kind) {
            case TokenKind::RBrace:
            case TokenKind::KwClass:
            case TokenKind::KwStruct:
            case TokenKind::KwInterface:
            case TokenKind::KwEnum:
            case TokenKind::KwNamespace:
            case TokenKind::KwPublic:
            case TokenKind::KwPrivate:
            case TokenKind::KwNew:
                return;
            default:
                advance();
        }
    }
}

// ---------------------------------------------------------------------------
//  types
// ---------------------------------------------------------------------------

TypeRefPtr Parser::parseTypePrimary() {
    if (at(TokenKind::KwVar) || at(TokenKind::KwLet)) {
        auto t = mkType(TypeKind::Inferred, cur().span);
        advance();
        return t;
    }
    if (at(TokenKind::LParen)) {
        // `( ... )` is either a function type's PARAMETER LIST (`(T, U) => R`)
        // or a PARENTHESIZED TYPE GROUP (`(T)`), disambiguated by whether `=>`
        // follows the close paren. The group form lets a `?` bind to the whole
        // group — `((string) => bool)?` is a nullable function type, distinct
        // from `(string) => bool?` where `?` binds the return (bug.md #51).
        SourceSpan lp = cur().span;
        advance();
        std::vector<TypeRefPtr> parts;
        if (!at(TokenKind::RParen)) {
            do { parts.push_back(parseType()); } while (accept(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");
        if (accept(TokenKind::Arrow)) {          // function type: (T, U) => R
            auto t = mkType(TypeKind::Function, lp);
            t->funcParams = std::move(parts);
            t->funcRet = parseType();
            return wrapNullable(std::move(t));
        }
        // no arrow: a parenthesized type group `(T)` — exactly one member.
        if (parts.size() == 1) return wrapNullable(std::move(parts[0]));
        error("'=>' after a parenthesized parameter list");
        return mkType(TypeKind::Named, lp);
    }
    auto t = mkType(TypeKind::Named, cur().span);
    if (at(TokenKind::Identifier)) { t->name = cur().text; advance(); }
    else error("type name");

    // ::-qualified type name: A::B::C -> path=[A, B], name=C. The qualifier
    // names the namespace(s) the type lives in (§12); generics bind the final
    // segment.
    while (at(TokenKind::ColonColon)) {
        advance();
        t->path.push_back(t->name);
        if (at(TokenKind::Identifier)) { t->name = cur().text; advance(); }
        else { error("type name after '::'"); break; }
    }

    if (accept(TokenKind::Lt)) {
        if (!at(TokenKind::Gt) && !at(TokenKind::GtGt)) {
            do { t->generics.push_back(parseType()); } while (accept(TokenKind::Comma));
        }
        expectGt();
    }
    return wrapNullable(std::move(t));           // T? == T | None (pure sugar)
}

// If a `?` follows, wrap `t` as the union `t | None`; otherwise return `t`. The
// suffix applies to a named type, a generic instantiation, a function type, or a
// parenthesized group (bug.md #51) — every parseTypePrimary form routes here.
TypeRefPtr Parser::wrapNullable(TypeRefPtr t) {
    if (accept(TokenKind::Question)) {
        auto u = mkType(TypeKind::Union, t->span);
        u->members.push_back(std::move(t));
        auto none = mkType(TypeKind::Named, u->span);
        none->name = std::string_view("None");
        u->members.push_back(std::move(none));
        return u;
    }
    return t;
}

TypeRefPtr Parser::parseType() {
    TypeRefPtr first = parseTypePrimary();
    if (!at(TokenKind::Pipe)) return first;

    auto u = mkType(TypeKind::Union, first->span);
    u->members.push_back(std::move(first));
    while (accept(TokenKind::Pipe)) u->members.push_back(parseTypePrimary());
    return u;
}

// ---------------------------------------------------------------------------
//  expressions (Pratt)
// ---------------------------------------------------------------------------

std::vector<ExprPtr> Parser::parseArgs() {
    std::vector<ExprPtr> args;
    bool sawNamed = false;
    expect(TokenKind::LParen, "'('");
    if (!at(TokenKind::RParen)) {
        do {
            if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
                std::string_view label = cur().text;
                advance();
                advance();                 // ':'
                ExprPtr arg = parseExpr(0);
                arg->argLabel = label;
                args.push_back(std::move(arg));
                sawNamed = true;
            } else {
                if (sawNamed)
                    sink_.error(cur().span, "positional argument after named argument");
                args.push_back(parseExpr(0));
            }
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "')'");
    return args;
}

// Parse the unambiguous call-site generic marker `::<T, U>`. The caller has
// already established the ColonColon+Lt pair, so seeing it commits to this
// grammar and never falls back to the ordinary comparison operators.
std::vector<TypeRefPtr> Parser::parseExplicitTypeArgs() {
    std::vector<TypeRefPtr> args;
    advance();                                      // '::'
    advance();                                      // '<'
    if (at(TokenKind::Gt) || at(TokenKind::GtGt)) {
        error("type argument after '<'");
    } else {
        do { args.push_back(parseType()); } while (accept(TokenKind::Comma));
    }
    expectGt();                                     // retains nested `>>` splitting
    return args;
}

std::vector<ExprPtr> Parser::parseMacroArgs() {
    std::vector<ExprPtr> args;
    bool sawNamed = false;
    expect(TokenKind::LParen, "'('");
    if (!at(TokenKind::RParen)) {
        do {
            std::string_view label;
            if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
                label = cur().text; advance(); advance();
                sawNamed = true;
            } else if (sawNamed) {
                sink_.error(cur().span, "positional argument after named argument");
            }
            ExprPtr arg;
            if (at(TokenKind::QuasiLiteral)) {
                const Token& t = cur();
                arg = mkExpr(ExprKind::StringLit, t.span);
                arg->isQuasiPayload = true;
                if (t.span.length >= 2)
                    arg->text = std::string_view(file_.text).substr(
                        t.span.offset + 1, t.span.length - 2);
                advance();
            } else {
                arg = parseExpr(0);
            }
            if (arg) arg->argLabel = label;
            args.push_back(std::move(arg));
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "')'");
    return args;
}

// Lookahead: advance `i` past a type token sequence at tokens_[i], returning
// false if it does not start one. Handles a bare/`::`-qualified name, a `<...>`
// generic instantiation (angle depth; `>>` closes two), a trailing `?`, and a
// parenthesized function type `(...) => ret`. Used only to disambiguate a TYPED
// lambda parameter (`(int x) => ...`, bug.md #39) from an untyped one.
bool Parser::skipTypeLA(size_t& i) const {
    if (i < tokens_.size() && tokens_[i].kind == TokenKind::LParen) {
        int depth = 0;
        do {
            if (tokens_[i].kind == TokenKind::LParen) ++depth;
            else if (tokens_[i].kind == TokenKind::RParen) --depth;
            ++i;
        } while (i < tokens_.size() && depth > 0);
        if (i >= tokens_.size() || tokens_[i].kind != TokenKind::Arrow) return false;
        ++i;                                   // =>
        return skipTypeLA(i);                  // return type
    }
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::Identifier) return false;
    ++i;
    while (i < tokens_.size() && tokens_[i].kind == TokenKind::ColonColon) {
        ++i;
        if (i >= tokens_.size() || tokens_[i].kind != TokenKind::Identifier) return false;
        ++i;
    }
    if (i < tokens_.size() && tokens_[i].kind == TokenKind::Lt) {
        int depth = 0;
        while (i < tokens_.size()) {
            TokenKind k = tokens_[i].kind;
            if (k == TokenKind::Lt) ++depth;
            else if (k == TokenKind::Gt) --depth;
            else if (k == TokenKind::GtGt) depth -= 2;
            ++i;
            if (depth <= 0) break;
        }
    }
    if (i < tokens_.size() && tokens_[i].kind == TokenKind::Question) ++i;
    return true;
}

// From the current position, do the tokens read as `type name` (a typed lambda
// parameter) rather than a bare `name`? True iff a type is followed by an
// identifier (the parameter name).
bool Parser::typedParamAhead() const {
    size_t i = pos_;
    return skipTypeLA(i) && i < tokens_.size() && tokens_[i].kind == TokenKind::Identifier;
}

bool Parser::looksLikeLambda() const {
    size_t i = pos_;
    if (peek(0).kind != TokenKind::LParen) return false;
    ++i;
    if (i < tokens_.size() && tokens_[i].kind == TokenKind::RParen) {
        ++i;
    } else {
        // one or more parameters, each `name` (untyped) or `type name` (typed).
        while (true) {
            size_t save = i;
            if (skipTypeLA(i) && i < tokens_.size() && tokens_[i].kind == TokenKind::Identifier) {
                ++i;                           // typed param: consumed type + name
            } else {
                i = save;                      // untyped param: a single identifier name
                if (i >= tokens_.size() || tokens_[i].kind != TokenKind::Identifier) return false;
                ++i;
            }
            if (i < tokens_.size() && tokens_[i].kind == TokenKind::Comma) { ++i; continue; }
            break;
        }
        if (i >= tokens_.size() || tokens_[i].kind != TokenKind::RParen) return false;
        ++i;
    }
    return i < tokens_.size() && tokens_[i].kind == TokenKind::Arrow;
}

// match (subject) { Pattern => body; ... else => body; }
//   Pattern: a type (Identifier / generic / T?), a value or range (literal-led
//   expression), or `else`. Body: `=> expr;` or `=> { block }`. First-match-wins.
ExprPtr Parser::parseMatch() {
    auto e = mkExpr(ExprKind::Match, cur().span);
    advance();                                   // 'match'
    expect(TokenKind::LParen, "'('");
    e->a = parseExpr(0);                         // the subject
    expect(TokenKind::RParen, "')'");
    expect(TokenKind::LBrace, "'{'");
    // R1 (005): a `::`-qualified arm head immediately followed by `<` after the
    // whole chain is a generic TYPE pattern (`k1::Box<int> =>`) — enum members
    // and consts never take generic arguments, so this is the one parse-time-
    // provable case. Every other qualified head stays on the neutral value route
    // and is classified semantically in the resolver (parse neutrally, classify
    // semantically). Precondition at call: `cur()` is `Identifier ::`.
    auto qualifiedHeadIsGeneric = [&]() -> bool {
        size_t j = 0;
        while (peek(j).kind == TokenKind::Identifier &&
               peek(j + 1).kind == TokenKind::ColonColon)
            j += 2;
        return peek(j).kind == TokenKind::Identifier &&
               peek(j + 1).kind == TokenKind::Lt;
    };
    while (!at(TokenKind::RBrace) && !atEnd()) {
        MatchArm arm;
        arm.span = cur().span;
        if (accept(TokenKind::KwElse)) {
            arm.isElse = true;
        } else if (at(TokenKind::IntLiteral) || at(TokenKind::FloatLiteral) ||
                   at(TokenKind::StringLiteral) || at(TokenKind::RawStringLiteral) ||
                   at(TokenKind::KwTrue) ||
                   at(TokenKind::KwFalse) || at(TokenKind::Minus) ||
                   (at(TokenKind::Identifier) &&
                    peek(1).kind == TokenKind::ColonColon &&
                    !qualifiedHeadIsGeneric())) {
            // Value/range pattern (range = bp 2). A `::`-qualified name in arm
            // position is parsed neutrally as a value pattern — this is how
            // enum-member arms `Method::GET =>` parse (Track 03 §2, problem #4);
            // a namespace-qualified TYPE with the same token shape is
            // reclassified later in the resolver (R2/R3).
            arm.value = parseExpr(2);
        } else {
            arm.type = parseType();              // type pattern
        }
        expect(TokenKind::Arrow, "'=>'");
        if (at(TokenKind::LBrace)) arm.bodyBlock = parseBlock();
        else arm.bodyExpr = parseExpr(0);
        accept(TokenKind::Semicolon);            // optional separator
        accept(TokenKind::Comma);
        e->arms.push_back(std::move(arm));
    }
    expect(TokenKind::RBrace, "'}'");
    return e;
}

ExprPtr Parser::parseParenOrLambda() {
    if (looksLikeLambda()) {
        auto lam = mkExpr(ExprKind::Lambda, cur().span);
        advance();  // (
        if (!at(TokenKind::RParen)) {
            do {
                Param p;
                p.span = cur().span;
                if (typedParamAhead()) p.type = parseType();   // typed param (bug.md #39)
                if (at(TokenKind::Identifier)) { p.name = cur().text; advance(); }
                lam->params.push_back(std::move(p));
            } while (accept(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')'");
        expect(TokenKind::Arrow, "'=>'");
        if (at(TokenKind::LBrace)) lam->block = parseBlock();   // { statements }
        else lam->a = parseExpr(0);                              // expression body
        return lam;
    }
    advance();  // (
    ExprPtr e = parseExpr(0);
    expect(TokenKind::RParen, "')'");
    return e;
}

ExprPtr Parser::parsePrimary() {
    const Token t = cur();
    switch (t.kind) {
        case TokenKind::IntLiteral: {
            advance(); auto e = mkExpr(ExprKind::IntLit, t.span); e->text = t.text; return e;
        }
        case TokenKind::FloatLiteral: {
            advance(); auto e = mkExpr(ExprKind::FloatLit, t.span); e->text = t.text; return e;
        }
        case TokenKind::StringLiteral: {
            advance();
            return parseInterpolatedString(t);
        }
        case TokenKind::RawStringLiteral: {
            // request-string-literal-tail: `r"..."` — text is `r` + quote +
            // content + quote; strip the 2-char prefix and the 1-char
            // closing quote. No interpolation scan, no escape decoding
            // (Eval/Lower read isRawString and use the text byte-exact).
            advance();
            auto e = mkExpr(ExprKind::StringLit, t.span);
            e->text = t.text.substr(2, t.text.size() - 3);
            e->isRawString = true;
            return e;
        }
        case TokenKind::KwTrue:
        case TokenKind::KwFalse: {
            advance(); auto e = mkExpr(ExprKind::BoolLit, t.span); e->text = t.text; return e;
        }
        case TokenKind::KwThis: {
            advance(); return mkExpr(ExprKind::This, t.span);
        }
        case TokenKind::Identifier: {
            advance(); auto e = mkExpr(ExprKind::Name, t.span); e->text = t.text; return e;
        }
        case TokenKind::KwAwait: {
            advance(); auto e = mkExpr(ExprKind::Await, t.span); e->a = parseUnary(); return e;
        }
        case TokenKind::KwMatch:
            return parseMatch();
        case TokenKind::KwInject: {
            advance(); auto e = mkExpr(ExprKind::Inject, t.span); e->type = parseType(); return e;
        }
        case TokenKind::LParen:
            return parseParenOrLambda();
        case TokenKind::LBracket: {
            auto arr = mkExpr(ExprKind::Array, t.span);
            advance();
            if (!at(TokenKind::RBracket)) {
                do { arr->list.push_back(parseArrayElement()); } while (accept(TokenKind::Comma));
            }
            expect(TokenKind::RBracket, "']'");
            return arr;
        }
        default: {
            error("expression");
            auto e = mkExpr(ExprKind::Name, t.span);   // placeholder to keep going
            if (!atEnd()) advance();
            return e;
        }
    }
}

// An array-literal element: ordinarily just an expression, but inside a
// quasiquote fragment (the only place `$for` can lex at all — see the `$`
// hole comment in Lexer.cpp) it may be a splice: `$for ident in expr : elem`.
// The iterator expr is ordinary comptime code, parsed up to `:` (Colon has no
// infix binding power, so parseExpr(0) stops there naturally — same property
// the ternary's `? then :` relies on).
ExprPtr Parser::parseArrayElement() {
    if (at(TokenKind::Identifier) && cur().text == "$for") {
        SourceSpan sp = cur().span;
        advance();                                   // '$for'
        auto fs = mkExpr(ExprKind::ForSplice, sp);
        if (at(TokenKind::Identifier)) { fs->text = cur().text; advance(); }
        else error("a loop variable name");
        expect(TokenKind::KwIn, "'in'");
        fs->a = parseExpr(0);
        expect(TokenKind::Colon, "':'");
        // The per-iteration element may itself be a `$if` (the techdesign §4
        // `$for p : $if (…) {…} $else {…}` binding case in expression position).
        if (at(TokenKind::Identifier) && cur().text == "$if") {
            SourceSpan isp = cur().span;
            advance();
            fs->b = parseForkSpliceExpr(isp);
        } else {
            fs->b = parseExpr(0);
        }
        return fs;
    }
    if (at(TokenKind::Identifier) && cur().text == "$if") {
        // B2 expression position: `$if (c) { e } $else { e }` selects one array
        // element at expansion time (techdesign-splices-conditional §2).
        SourceSpan sp = cur().span;
        advance();                                   // '$if'
        return parseForkSpliceExpr(sp);
    }
    if (at(TokenKind::Identifier) && cur().text == "$else") {
        // M41: `$else` with no `$if` (techdesign-splices-conditional §3.3).
        sink_.error(cur().span, "$else without a preceding $if");
        advance();
    }
    return parseExpr(0);
}

// B2 expression position (techdesign-splices-conditional §2). The leading `$if`
// is already consumed. Each branch is a single brace-delimited expression; the
// `$else if` chain desugars to nested ForkSplice in `c`.
ExprPtr Parser::parseForkSpliceExpr(SourceSpan sp) {
    auto node = mkExpr(ExprKind::ForkSplice, sp);
    expect(TokenKind::LParen, "'('");
    node->a = parseExpr(0);
    expect(TokenKind::RParen, "')'");
    node->b = parseForkBranchExpr();
    if (at(TokenKind::Identifier) && cur().text == "$else") {
        SourceSpan esp = cur().span;
        advance();                                   // '$else'
        if (at(TokenKind::KwIf)) {
            advance();                               // 'if' of `$else if`
            node->c = parseForkSpliceExpr(esp);      // nested fork (desugar)
        } else {
            node->c = parseForkBranchExpr();
        }
    }
    return node;
}

// A `{ <expr> }` branch of an expression-position `$if`. Braces delimit a single
// expression (which may itself be a nested `$if`).
ExprPtr Parser::parseForkBranchExpr() {
    expect(TokenKind::LBrace, "'{'");
    ExprPtr e;
    if (at(TokenKind::Identifier) && cur().text == "$if") {
        SourceSpan sp = cur().span;
        advance();
        e = parseForkSpliceExpr(sp);
    } else {
        e = parseExpr(0);
    }
    expect(TokenKind::RBrace, "'}'");
    return e;
}

// Statement/decl/member-position `$for` splice (LA-4 item J + the item-position
// generalization the DOM bindgen's `at namespace`/`at member` templates need):
// `$for <id> in <iter> : <body>`, where <body> is ONE statement, top-level item,
// or class member depending on the fragment. Only reachable inside a quasiquote
// (holes on), so `$for` can never lex in ordinary code. Bounded on purpose: the
// body is a single element (a `{ }` block groups several) — no `$if`/`$while`
// (P4 §9.2's "keep it bounded" line).
StmtPtr Parser::parseForSpliceStmt(SpliceBody body) {
    SourceSpan sp = cur().span;
    advance();                                   // '$for'
    auto fs = std::make_unique<Stmt>(StmtKind::ForSplice);
    fs->span = sp;
    if (at(TokenKind::Identifier)) { fs->name = cur().text; advance(); }
    else error("a loop variable name");
    expect(TokenKind::KwIn, "'in'");
    fs->expr = parseExpr(0);
    expect(TokenKind::Colon, "':'");
    // The body is a single fragment element — an ordinary statement/item/member,
    // or a reserved-head splice (`$for`, `$if`), so `$for p : $if (…) {…} $else
    // {…}` composes (the techdesign §4 binding case).
    fs->thenBranch = parseFragmentStmt(body);
    return fs;
}

// One element of a statement/member/item fragment: dispatch reserved hole-heads
// to their splice productions, else the ordinary parser for `body`. Shared by
// the fragment loops (parseStmtsFragment et al.), by parseForSpliceStmt's body,
// and by parseForkBranch's branch bodies — so every splice nests in every other.
StmtPtr Parser::parseFragmentStmt(SpliceBody body) {
    if (at(TokenKind::Identifier) && cur().text == "$for")
        return parseForSpliceStmt(body);
    if (at(TokenKind::Identifier) && cur().text == "$if")
        return parseForkSpliceStmt(body);
    if (at(TokenKind::Identifier) && cur().text == "$else") {
        // M41: `$else` with no preceding `$if` (techdesign-splices-conditional
        // §3.3). Consume it so the fragment loop makes progress.
        sink_.error(cur().span, "$else without a preceding $if");
        advance();
        return nullptr;
    }
    switch (body) {
        case SpliceBody::Stmt:   return parseStatement();
        case SpliceBody::Item:   return parseTopLevelItem();
        case SpliceBody::Member: return parseClassMember(Access::Public);
    }
    return nullptr;   // unreachable; silences -Wreturn-type
}

// B2 statement/member/item position (techdesign-splices-conditional §2):
// `$if (cond) <frag> ( $else if (cond) <frag> )* ( $else <frag> )?`. Consumes
// the leading `$if`, then defers to parseForkTail.
StmtPtr Parser::parseForkSpliceStmt(SpliceBody body) {
    SourceSpan sp = cur().span;
    advance();                                   // '$if'
    return parseForkTail(body, sp);
}

// Parses `(cond) <then-frag> [ $else <else-frag> | $else if … ]` — entered once
// for `$if` and once per desugared `$else if`. Each `$else if` becomes a nested
// ForkSplice in `elseBranch`, so the whole chain is a right-leaning tree the
// fold (cloneStmtInto) walks by ordinary recursion.
StmtPtr Parser::parseForkTail(SpliceBody body, SourceSpan sp) {
    auto node = std::make_unique<Stmt>(StmtKind::ForkSplice);
    node->span = sp;
    expect(TokenKind::LParen, "'('");
    node->expr = parseExpr(0);
    expect(TokenKind::RParen, "')'");
    node->thenBranch = parseForkBranch(body);
    if (at(TokenKind::Identifier) && cur().text == "$else") {
        SourceSpan esp = cur().span;
        advance();                               // '$else'
        if (at(TokenKind::KwIf)) {
            advance();                           // 'if' of `$else if`
            node->elseBranch = parseForkTail(body, esp);   // nested fork (desugar)
        } else {
            node->elseBranch = parseForkBranch(body);
        }
    }
    return node;
}

// A `{ <frag> }` branch of a statement-position `$if`: a brace group of the same
// fragment kind as the enclosing template, collected into a Block so the fold
// splices its members flat (the same shape ForSplice's `{ }` body uses). The
// kind is fixed by `body`, so both branches are structurally the same kind —
// parse-time half of M41 (techdesign-splices-conditional §3.3).
StmtPtr Parser::parseForkBranch(SpliceBody body) {
    SourceSpan sp = cur().span;
    expect(TokenKind::LBrace, "'{'");
    auto blk = std::make_unique<Stmt>(StmtKind::Block);
    blk->span = sp;
    while (!at(TokenKind::RBrace) && !atEnd()) {
        size_t before = pos_;
        StmtPtr s = parseFragmentStmt(body);
        if (s && s->kind != StmtKind::Empty) blk->body.push_back(std::move(s));
        if (pos_ == before) { error("a fragment in $if branch"); break; }
    }
    expect(TokenKind::RBrace, "'}'");
    return blk;
}

ExprPtr Parser::parsePostfix(ExprPtr base) {
    for (;;) {
        if (at(TokenKind::ColonColon) && peek(1).kind == TokenKind::Lt) {
            std::vector<TypeRefPtr> targs = parseExplicitTypeArgs();
            if (at(TokenKind::LParen)) {
                auto c = mkExpr(ExprKind::Call, base->span);
                c->a = std::move(base);
                c->explicitTypeArgs = std::move(targs);
                c->list = parseArgs();
                base = std::move(c);
            } else {
                // LA-32 §4.6: a turbofish with NO following `(args)` is a pinned
                // generic VALUE reference (`var f = identity::<int>;`). The type
                // tuple LA-25 §8.6 needs to build the eta-expansion is supplied
                // here, so the reference is well-defined. The args ride on the
                // callee node itself (a Name/Member); the checker resolves it as
                // a concrete closure. An UNPINNED reference (`identity`) stays the
                // LA-25 §8.6 error, upgraded to suggest the turbofish.
                base->explicitTypeArgs = std::move(targs);
            }
        } else if (at(TokenKind::Dot) || at(TokenKind::ColonColon) ||
            at(TokenKind::QuestionDot)) {
            bool colon = at(TokenKind::ColonColon);
            bool opt = at(TokenKind::QuestionDot);
            SourceSpan sp = base->span;
            advance();
            auto m = mkExpr(ExprKind::Member, sp);
            m->colon = colon;
            m->optChain = opt;
            m->a = std::move(base);
            // A member name after '.'/'::' may be an identifier or a keyword used
            // contextually (get/set/is/in/...): `client.get(...)`, `x.in`. The
            // keyword meaning only applies at statement/declaration position.
            if (at(TokenKind::Identifier) || isContextualName(cur().kind)) {
                m->text = cur().text; advance();
            } else {
                error("member name");
            }
            base = std::move(m);
        } else if (at(TokenKind::LParen)) {
            auto c = mkExpr(ExprKind::Call, base->span);
            c->a = std::move(base);
            c->list = parseArgs();
            base = std::move(c);
        } else if (at(TokenKind::Bang) && peek(1).kind == TokenKind::LParen &&
                   (base->kind == ExprKind::Name || base->kind == ExprKind::Member) &&
                   base->explicitTypeArgs.empty()) {   // LA-32: a turbofish value ref is not macro-callable
            // `name!(args)` / `NS::name!(args)` — an expression macro call
            // (Phase 3 §7). `Bang` here was a parse error before this feature
            // (postfix `!` doesn't otherwise exist), so the grammar slot is
            // free with no adjacency check needed.
            sawMeta_ = true;
            advance();                                // '!'
            auto c = mkExpr(ExprKind::Call, base->span);
            c->isMacroCall = true;
            c->a = std::move(base);
            c->list = parseMacroArgs();
            base = std::move(c);
        } else if (at(TokenKind::LBracket)) {
            auto ix = mkExpr(ExprKind::Index, base->span);
            advance();
            ix->a = std::move(base);
            ix->b = parseExpr(0);
            expect(TokenKind::RBracket, "']'");
            base = std::move(ix);
        } else {
            return base;
        }
    }
}

ExprPtr Parser::parseUnary() {
    if (at(TokenKind::Bang) || at(TokenKind::Minus) || at(TokenKind::Tilde)) {
        TokenKind op = cur().kind;
        SourceSpan sp = cur().span;
        advance();
        auto e = mkExpr(ExprKind::Unary, sp);
        e->op = op;
        e->a = parseUnary();
        return e;
    }
    // `comptime <expr>` in expression position (§16.5 Layer C): everything to
    // the expression's end folds at compile time. Contextual — `comptime`
    // followed by a non-expression token is still an ordinary name.
    if (at(TokenKind::Identifier) && cur().text == "comptime" && nextStartsExpr(1)) {
        sawMeta_ = true;
        advance();
        ExprPtr e = parseExpr(0);
        if (e) e->isComptime = true;
        return e;
    }
    return parsePostfix(parsePrimary());
}

ExprPtr Parser::parseExpr(int minBP) {
    ExprPtr left = parseUnary();
    for (;;) {
        TokenKind k = cur().kind;

        if (k == TokenKind::Question) {              // ternary (bp 2, right-assoc)
            if (2 < minBP) break;
            SourceSpan sp = left->span;
            advance();
            auto t = mkExpr(ExprKind::Ternary, sp);
            t->a = std::move(left);
            t->b = parseExpr(0);
            expect(TokenKind::Colon, "':'");
            t->c = parseExpr(2);
            left = std::move(t);
            continue;
        }

        if (k == TokenKind::KwIs) {                   // expr is Type  (bp ~6)
            if (6 < minBP) break;
            SourceSpan sp = left->span;
            advance();
            auto is = mkExpr(ExprKind::Is, sp);
            is->a = std::move(left);
            is->type = parseType();
            left = std::move(is);
            continue;
        }

        if (k == TokenKind::DotDot) {                 // range  a .. b  (bp 2)
            if (2 < minBP) break;
            SourceSpan sp = left->span;
            advance();
            auto rng = mkExpr(ExprKind::Range, sp);
            rng->a = std::move(left);
            rng->b = parseExpr(9);                    // bound is an arithmetic expr
            left = std::move(rng);
            continue;
        }

        int bp = infixBP(k);
        if (bp == 0 || bp < minBP) break;
        SourceSpan sp = left->span;
        advance();

        // `stream >>` with nothing parseable after it: extract, no rhs (§13 demo).
        if (k == TokenKind::GtGt && !canStartExpr(cur().kind)) {
            auto ex = mkExpr(ExprKind::Extract, sp);
            ex->a = std::move(left);
            left = std::move(ex);
            continue;
        }

        bool rightAssoc = (k == TokenKind::Eq || k == TokenKind::PlusEq ||
                           k == TokenKind::MinusEq || k == TokenKind::StarEq ||
                           k == TokenKind::SlashEq || k == TokenKind::PercentEq);
        auto bin = mkExpr(ExprKind::Binary, sp);
        bin->op = k;
        bin->a = std::move(left);
        bin->b = parseExpr(rightAssoc ? bp : bp + 1);
        left = std::move(bin);
    }
    return left;
}

// ---------------------------------------------------------------------------
//  statements  (parseStatement is also the "body is one statement" routine)
// ---------------------------------------------------------------------------

void Parser::parseTypeParams(std::vector<std::string_view>& out) {
    if (!accept(TokenKind::Lt)) return;
    if (!at(TokenKind::Gt)) {
        do {
            if (at(TokenKind::Identifier)) { out.push_back(cur().text); advance(); }
            else error("type parameter");
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::Gt, "'>'");
}

std::vector<Param> Parser::parseParamList() {
    std::vector<Param> ps;
    expect(TokenKind::LParen, "'('");
    if (!at(TokenKind::RParen)) {
        do {
            Param p;
            p.span = cur().span;
            // $_params (§16.5 Phase 3 §6): a well-known hole standing in for
            // the matched subject's whole parameter list, in a `member of`
            // template — no type prefix (only lexes inside a fragment, so
            // this can never fire on ordinary source: '$' isn't a token there).
            if (at(TokenKind::Identifier) && cur().text == "$_params") {
                p.name = cur().text; advance();
            } else {
                p.isConst = accept(TokenKind::KwConst);   // const.md §3.4
                p.type = parseType();
                if (at(TokenKind::Identifier)) { p.name = cur().text; advance(); }
                else error("parameter name");
                if (accept(TokenKind::Eq)) {
                    p.defaultValue = parseExpr(0);
                    // Default folding uses the existing hermetic rule-stage
                    // machinery when available; the checker still validates
                    // literal-only v1 defaults for non-meta test pipelines.
                    sawMeta_ = true;
                }
            }
            ps.push_back(std::move(p));
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "')'");
    return ps;
}

bool Parser::looksLikeVarDecl() const {
    // bug #44: a local's declared type may be a lambda type `(T) => R h = …;`,
    // not only an identifier type. skipTypeFull skips either; a leading '(' that
    // is really a parenthesized/IIFE expression leaves the index unmoved (no
    // `=>` follows its balanced parens) and is rejected below.
    size_t i;
    if (peek(0).kind == TokenKind::Identifier) i = skipTypeSuffix(tokens_, pos_ + 1);
    else if (peek(0).kind == TokenKind::LParen) {
        i = skipTypeFull(tokens_, pos_);
        if (i == pos_) return false;                 // not a lambda type -> expression
    } else return false;
    if (i >= tokens_.size() || tokens_[i].kind != TokenKind::Identifier) return false;  // var name
    ++i;
    if (i >= tokens_.size()) return false;
    return tokens_[i].kind == TokenKind::Eq || tokens_[i].kind == TokenKind::Semicolon;
}

StmtPtr Parser::parseVarDecl() {
    auto v = mkStmt(StmtKind::Var, cur().span);
    if (accept(TokenKind::KwConst)) v->isConst = true;   // const.md §3
    if (at(TokenKind::KwVar) || at(TokenKind::KwLet)) {
        v->inferred = true;
        if (at(TokenKind::KwLet)) v->isConst = true;      // `let` == `const var` (const.md §5)
        advance();
    } else v->type = parseType();
    if (at(TokenKind::Identifier)) { v->name = cur().text; advance(); }
    else error("variable name");
    if (accept(TokenKind::Eq)) v->init = parseExpr(0);
    expect(TokenKind::Semicolon, "';'");
    return v;
}

StmtPtr Parser::parseBlock() {
    auto b = mkStmt(StmtKind::Block, cur().span);
    expect(TokenKind::LBrace, "'{'");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        size_t before = pos_;
        StmtPtr s = parseStatement();
        if (s) b->body.push_back(std::move(s));
        if (pos_ == before) { error("statement"); advance(); synchronize(); }
    }
    expect(TokenKind::RBrace, "'}'");
    return b;
}

StmtPtr Parser::parseBind() {
    auto b = mkStmt(StmtKind::Bind, cur().span);
    advance();  // 'bind'

    // Factory form iff `bind Type =>` or `bind Type {`; otherwise object install.
    bool factory = false;
    if (peek(0).kind == TokenKind::Identifier) {
        size_t j = skipTypeSuffix(tokens_, pos_ + 1);
        if (j < tokens_.size() &&
            (tokens_[j].kind == TokenKind::Arrow || tokens_[j].kind == TokenKind::LBrace))
            factory = true;
    }
    if (factory) {
        b->type = parseType();
        b->memberBody = parseStatement();   // '=> expr;' or '{ ... }' — body is one statement
    } else {
        b->init = parseExpr(0);             // bind <object>;
        expect(TokenKind::Semicolon, "';'");
    }
    return b;
}

// A `::`-separated path of identifiers, shared by `uses` and selective `use`.
void Parser::parsePath(std::vector<std::string_view>& out) {
    do {
        if (at(TokenKind::Identifier)) { out.push_back(cur().text); advance(); }
        else { error("path segment"); break; }
    } while (accept(TokenKind::ColonColon));
}

// `uses A::B;` — import a namespace's names into the enclosing scope.
// Path segments are stored in `generics` (a vector<string_view>).
StmtPtr Parser::parseUses() {
    auto s = mkStmt(StmtKind::UsesImport, cur().span);
    advance();  // 'uses'
    parsePath(s->generics);
    if (!s->generics.empty()) s->name = s->generics.back();
    expect(TokenKind::Semicolon, "';'");
    return s;
}

// `use A::B::name (as alias)?;` (imports.md) — bind ONE name (a value,
// function, class, or nested namespace — any declaration kind imports
// uniformly, §4) into the enclosing scope. Path segments share `generics`
// with `uses`; `name` holds the bound name (the alias if given, else the
// path's last segment), so the Resolver finds both keywords' imports the
// same way. `as` is contextual (an Identifier peek), the attribute/comptime/
// rule/macro precedent (reference §1.5).
StmtPtr Parser::parseUse() {
    auto s = mkStmt(StmtKind::Use, cur().span);
    advance();  // 'use'
    parsePath(s->generics);
    s->name = s->generics.empty() ? std::string_view() : s->generics.back();
    if (at(TokenKind::Identifier) && cur().text == "as") {
        advance();  // 'as'
        if (at(TokenKind::Identifier)) { s->name = cur().text; advance(); }
        else error("alias name after 'as'");
    }
    expect(TokenKind::Semicolon, "';'");
    return s;
}

StmtPtr Parser::parseStatement() {
    // Statement-position `$for` splice (item J). `$for` only lexes with holes
    // enabled (inside a quasiquote fragment), so this can never fire in ordinary
    // code; it catches a `$for` anywhere a statement is legal — including nested
    // in a generated method/ctor body, which the fragment-loop checks miss.
    if (at(TokenKind::Identifier) && cur().text == "$for")
        return parseForSpliceStmt(SpliceBody::Stmt);
    // Statement-position `$if` conditional splice (B2). Same reasoning as `$for`:
    // it lexes only inside a quasiquote, and a generated method/ctor body is
    // parsed by this ordinary statement routine, not the fragment loop — so the
    // recognition must live here to catch a `$if` nested in a body.
    if (at(TokenKind::Identifier) && cur().text == "$if")
        return parseForkSpliceStmt(SpliceBody::Stmt);
    if (at(TokenKind::Identifier) && cur().text == "$else") {
        sink_.error(cur().span, "$else without a preceding $if");   // M41
        advance();
        return nullptr;
    }
    // Labeled loop (techdesign-labeled-break-continue.md F1): `identifier :`
    // immediately before a loop keyword. `Colon` has no infix binding power
    // (parseExpr(0) stops at it), so `ident :` never begins an expression
    // statement today — this claims otherwise-dead grammar. The three-token
    // lookahead (peek(2) must be a loop keyword) keeps the recursion total:
    // the recursive parseStatement() call is guaranteed to land in the
    // While/For/DoWhile case (For covers ForIn).
    if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon &&
        (peek(2).kind == TokenKind::KwWhile || peek(2).kind == TokenKind::KwFor ||
         peek(2).kind == TokenKind::KwDo)) {
        std::string_view lbl = cur().text;
        advance(); advance();                       // ident, ':'
        StmtPtr s = parseStatement();                // hits the loop case
        if (s) s->label = lbl;
        return s;
    }
    switch (cur().kind) {
        case TokenKind::LBrace:
            return parseBlock();
        case TokenKind::KwReturn: {
            auto r = mkStmt(StmtKind::Return, cur().span);
            advance();
            if (!at(TokenKind::Semicolon)) r->expr = parseExpr(0);
            expect(TokenKind::Semicolon, "';'");
            return r;
        }
        case TokenKind::Arrow: {                 // '=> expr;'  (return sugar)
            auto r = mkStmt(StmtKind::Return, cur().span);
            advance();
            r->expr = parseExpr(0);
            expect(TokenKind::Semicolon, "';'");
            return r;
        }
        case TokenKind::At: {
            // `@anchor("name");` (§16.5 Phase 3 §8.2) — an author-placed marker
            // a rule can `inject ... at marker "name"`. Parsed to a StmtKind::
            // Empty carrying the (raw, quoted) marker name in `name`, so every
            // pass that already ignores Empty needs no new handling.
            SourceSpan sp = cur().span;
            if (peek(1).kind == TokenKind::Identifier && peek(1).text == "anchor" &&
                peek(2).kind == TokenKind::LParen) {
                advance();                     // '@'
                advance();                     // 'anchor'
                advance();                     // '('
                std::string_view marker;
                if (at(TokenKind::StringLiteral)) { marker = cur().text; advance(); }
                else error("a marker name string");
                expect(TokenKind::RParen, "')'");
                expect(TokenKind::Semicolon, "';'");
                sawMeta_ = true;
                auto s = mkStmt(StmtKind::Empty, sp);
                s->name = marker;
                return s;
            }
            error("'anchor(\"name\")' after '@' (attributes are declaration-position only)");
            advance();
            return mkStmt(StmtKind::Empty, sp);
        }
        case TokenKind::Semicolon:
            return (advance(), mkStmt(StmtKind::Empty, cur().span));
        case TokenKind::KwIf: {
            auto s = mkStmt(StmtKind::If, cur().span);
            advance();
            expect(TokenKind::LParen, "'('");
            s->expr = parseExpr(0);
            expect(TokenKind::RParen, "')'");
            s->thenBranch = parseStatement();
            if (accept(TokenKind::KwElse)) s->elseBranch = parseStatement();
            return s;
        }
        case TokenKind::KwThrow: {
            auto s = mkStmt(StmtKind::Throw, cur().span);
            advance();
            s->expr = parseExpr(0);
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        case TokenKind::KwTry: {
            auto s = mkStmt(StmtKind::Try, cur().span);
            advance();
            s->thenBranch = parseStatement();          // body is one statement
            while (accept(TokenKind::KwCatch)) {
                CatchClause c;
                expect(TokenKind::LParen, "'('");
                c.type = parseType();
                if (at(TokenKind::Identifier)) { c.name = cur().text; advance(); }
                expect(TokenKind::RParen, "')'");
                c.body = parseStatement();
                s->catches.push_back(std::move(c));
            }
            if (s->catches.empty()) error("'catch' clause");
            return s;
        }
        case TokenKind::KwWhile: {
            auto s = mkStmt(StmtKind::While, cur().span);
            advance();
            expect(TokenKind::LParen, "'('");
            s->expr = parseExpr(0);
            expect(TokenKind::RParen, "')'");
            s->thenBranch = parseStatement();
            return s;
        }
        case TokenKind::KwFor: {
            SourceSpan sp = cur().span;
            advance();
            expect(TokenKind::LParen, "'('");

            // Distinguish `for (T x in iter)` from `for (init; cond; step)` by
            // scanning for 'in' vs ';' at paren depth 0.
            bool forIn = false;
            for (size_t i = pos_, depth = 0; i < tokens_.size(); ++i) {
                TokenKind k = tokens_[i].kind;
                if (k == TokenKind::LParen) ++depth;
                else if (k == TokenKind::RParen) { if (depth == 0) break; --depth; }
                else if (depth == 0 && k == TokenKind::Semicolon) break;
                else if (depth == 0 && k == TokenKind::KwIn) { forIn = true; break; }
                else if (k == TokenKind::End) break;
            }

            if (forIn) {
                auto s = mkStmt(StmtKind::ForIn, sp);
                if (accept(TokenKind::KwConst)) s->isConst = true;   // const.md §3.5
                if (at(TokenKind::KwVar) || at(TokenKind::KwLet)) {
                    s->inferred = true;
                    if (at(TokenKind::KwLet)) s->isConst = true;
                    advance();
                } else s->type = parseType();
                if (at(TokenKind::Identifier)) { s->name = cur().text; advance(); }
                else error("loop variable name");
                expect(TokenKind::KwIn, "'in'");
                s->expr = parseExpr(0);               // the iterable
                expect(TokenKind::RParen, "')'");
                s->thenBranch = parseStatement();
                return s;
            }

            auto s = mkStmt(StmtKind::For, sp);
            if (!accept(TokenKind::Semicolon))            // init (consumes its own ';')
                s->forInit = parseStatement();
            if (!at(TokenKind::Semicolon)) s->expr = parseExpr(0);   // cond
            expect(TokenKind::Semicolon, "';'");
            if (!at(TokenKind::RParen)) s->forStep = parseExpr(0);   // step
            expect(TokenKind::RParen, "')'");
            s->thenBranch = parseStatement();
            return s;
        }
        case TokenKind::KwBind:
            return parseBind();
        case TokenKind::KwUses:
            return parseUses();
        case TokenKind::KwUse:
            return parseUse();
        case TokenKind::KwVar:
        case TokenKind::KwLet:
        case TokenKind::KwConst:
            return parseVarDecl();
        case TokenKind::KwReadonly:
            // techdesign-readonly §4.1: `readonly` is an instance-field-only
            // modifier (applied in parseClassMemberInner only) — seeing it
            // here means a local declaration wrote it, which is not the
            // field-only slot it exists for.
            sink_.error(cur().span, "readonly applies to instance fields only; "
                        "use 'const' for a write-once local/global/parameter");
            advance();
            return parseStatement();
        case TokenKind::KwWeak:
            sink_.error(cur().span, "weak applies to instance fields only");
            advance();
            return parseStatement();
        case TokenKind::KwUsing: {
            // techdesign-02 F3: `using` (+ optional `const`) + the ordinary
            // var-decl tail; statement-position only (v1: locals only).
            advance();  // 'using'
            StmtPtr v = parseVarDecl();
            if (v) { v->isUsing = true; v->isConst = true; }
            return v;
        }
        case TokenKind::KwBreak: {
            auto s = mkStmt(StmtKind::Break, cur().span);
            advance();
            if (at(TokenKind::Identifier)) { s->label = cur().text; advance(); }
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        case TokenKind::KwContinue: {
            auto s = mkStmt(StmtKind::Continue, cur().span);
            advance();
            if (at(TokenKind::Identifier)) { s->label = cur().text; advance(); }
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        case TokenKind::KwDo: {
            auto s = mkStmt(StmtKind::DoWhile, cur().span);
            advance();
            s->thenBranch = parseStatement();          // body is one statement
            expect(TokenKind::KwWhile, "'while'");
            expect(TokenKind::LParen, "'('");
            s->expr = parseExpr(0);
            expect(TokenKind::RParen, "')'");
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        default:
            if (StmtPtr ct = tryParseComptime()) return ct;   // §16.5 Layer C
            if (looksLikeVarDecl()) return parseVarDecl();
            auto s = mkStmt(StmtKind::ExprStmt, cur().span);
            s->expr = parseExpr(0);
            // A block-terminated expression statement (a bare `match`) needs no
            // trailing ';', like `if`/`while`/`{...}`.
            if (s->expr && s->expr->kind == ExprKind::Match) accept(TokenKind::Semicolon);
            else expect(TokenKind::Semicolon, "';'");
            return s;
    }
}

// ---------------------------------------------------------------------------
//  declarations
// ---------------------------------------------------------------------------

// Attribute wrapper for members: `@Column public int id;` etc.
StmtPtr Parser::parseClassMember(Access sectionAccess, bool sectionConst) {
    std::vector<AttrUse> attrs = parseAttrUses();
    StmtPtr m = parseClassMemberInner(sectionAccess, sectionConst);
    if (!attrs.empty()) {
        if (m && m->kind == StmtKind::Member) m->attrs = std::move(attrs);
        else sink_.error(attrs.front().span,
                         "an attribute must precede a member declaration");
    }
    return m;
}

StmtPtr Parser::parseClassMemberInner(Access sectionAccess, bool sectionConst) {
    Access acc = sectionAccess;
    if (accept(TokenKind::KwPublic)) acc = Access::Public;
    else if (accept(TokenKind::KwPrivate)) acc = Access::Private;

    // A `const:` section (OQ2, deferal-const-system-extensions.md §3.2) seeds
    // `isConst` for members that follow it — the same `isConst` bit a per-member
    // `const` sets ([Ast.hpp] isConst). The const section and the access section
    // are orthogonal sticky axes; a per-member modifier overrides its section
    // (problem #5): `const` forces const on, `var` forces it off.
    bool mut = false, dist = false, isConst = sectionConst, isReadonly = false, isWeak = false;
    // Slot modifiers are orthogonal and may be written in any order.
    for (;;) {
        if (accept(TokenKind::KwMutating)) { mut = true; continue; }
        if (accept(TokenKind::KwDistinct)) { dist = true; continue; }
        if (accept(TokenKind::KwConst)) { isConst = true; continue; }
        if (accept(TokenKind::KwVar)) { isConst = false; continue; }   // per-member override of a `const:` section
        if (accept(TokenKind::KwReadonly)) { isReadonly = true; continue; }
        if (accept(TokenKind::KwWeak)) { isWeak = true; continue; }
        break;
    }

    // constructor: 'new' Label ( params ) body
    if (accept(TokenKind::KwNew)) {
        if (isWeak) sink_.error(cur().span, "weak applies to instance fields only");
        auto m = mkStmt(StmtKind::Member, cur().span);
        m->access = acc; m->isCtor = true; m->callable = true; m->isMutating = true;
        if (at(TokenKind::Identifier)) { m->name = cur().text; m->selector.text = cur().text; advance(); }
        else error("constructor label");
        m->params = parseParamList();
        m->memberBody = parseStatement();
        return m;
    }

    // accessor: get/set Name ( params ) body   OR   get/set (SELECTOR) ( params ) body
    if (at(TokenKind::KwGet) || at(TokenKind::KwSet)) {
        if (isWeak) sink_.error(cur().span, "weak applies to instance fields only");
        bool isGet = at(TokenKind::KwGet);
        auto m = mkStmt(StmtKind::Member, cur().span);
        advance();
        m->access = acc; m->isGet = isGet; m->isSet = !isGet; m->callable = true;
        m->isMutating = mut || !isGet;      // a set accessor writes `this`
        if (at(TokenKind::Identifier)) {
            m->name = cur().text; m->selector.text = cur().text; advance();
        } else if (at(TokenKind::LParen)) {          // symbolic selector, e.g. ([])
            m->selector.symbolic = true;
            advance();
            uint32_t s = cur().span.offset, e = s;
            while (!at(TokenKind::RParen) && !atEnd()) { e = cur().span.end(); advance(); }
            m->selector.text = std::string_view(file_.text).substr(s, e - s);
            m->name = m->selector.text;
            expect(TokenKind::RParen, "')'");
        } else {
            error("accessor name or selector");
        }
        m->params = parseParamList();
        m->memberBody = parseStatement();
        return m;
    }

    // otherwise a typed member: Type ...
    auto m = mkStmt(StmtKind::Member, cur().span);
    m->access = acc; m->distinct = dist; m->isMutating = mut; m->isConst = isConst;
    m->isReadonly = isReadonly;
    m->isWeak = isWeak;
    m->type = parseType();

    // symbolic "()" selector (operator member): Type ( <selector> ) ( params ) body
    if (at(TokenKind::LParen)) {
        m->callable = true;
        m->selector.symbolic = true;
        advance();  // '('
        uint32_t s = cur().span.offset, e = s;
        while (!at(TokenKind::RParen) && !atEnd()) { e = cur().span.end(); advance(); }
        m->selector.text = std::string_view(file_.text).substr(s, e - s);
        expect(TokenKind::RParen, "')'");
        m->params = parseParamList();
        m->memberBody = parseStatement();
        return m;
    }

    // named member. A return type has already been consumed, so get/set/etc.
    // here are the member NAME, not the accessor keyword (`void get(...)`).
    if (at(TokenKind::Identifier) || isContextualName(cur().kind)) {
        m->name = cur().text; m->selector.text = cur().text; advance();
    } else {
        error("member name");
    }

    parseTypeParams(m->generics);                 // method type params: name<U, R>(...)

    if (at(TokenKind::LParen)) {                  // method
        if (isWeak) sink_.error(m->span, "weak applies to instance fields only");
        m->callable = true;
        m->params = parseParamList();
        m->memberBody = parseStatement();
        return m;
    }

    if (accept(TokenKind::Eq)) m->init = parseExpr(0);   // field with initializer
    expect(TokenKind::Semicolon, "';'");
    return m;
}

StmtPtr Parser::parseClass(Access access, bool isInterface, bool isValue) {
    auto c = mkStmt(StmtKind::Class, cur().span);
    c->access = access; c->isInterface = isInterface; c->isValue = isValue;
    advance();  // 'class' / 'struct' / 'interface'
    if (at(TokenKind::Identifier)) { c->name = cur().text; advance(); }
    else error("class name");

    parseTypeParams(c->generics);                 // generic params <T, U>

    if (accept(TokenKind::Colon)) {               // bases
        do { c->bases.push_back(parseType()); } while (accept(TokenKind::Comma));
    }

    if (accept(TokenKind::Semicolon)) return c;   // empty-body form (e.g. IOStream)

    expect(TokenKind::LBrace, "'{'");
    Access section = Access::Default;
    bool constSection = false;                    // OQ2: orthogonal `const:` section axis
    while (!at(TokenKind::RBrace) && !atEnd()) {
        if ((at(TokenKind::KwPublic) || at(TokenKind::KwPrivate)) &&
            peek(1).kind == TokenKind::Colon) {
            section = at(TokenKind::KwPublic) ? Access::Public : Access::Private;
            advance(); advance();                 // access label leaves the const section untouched (orthogonal axes)
            continue;
        }
        if (at(TokenKind::KwConst) && peek(1).kind == TokenKind::Colon) {
            constSection = true;                  // `const:` — sticky until class end; leaves access untouched
            advance(); advance();
            continue;
        }
        size_t before = pos_;
        StmtPtr m = parseClassMember(section, constSection);
        if (m) c->body.push_back(std::move(m));
        if (pos_ == before) { error("member"); advance(); synchronize(); }
    }
    expect(TokenKind::RBrace, "'}'");
    return c;
}

// `enum Name : carrier { M1, M2 = v, ... }` (Track 03 §2). Parse-only: the
// Resolver desugars this to a value struct + per-member mangled const globals +
// a `fromCode` free function. We keep the node dumb — `type` is the carrier
// TypeRef (null => int in v1), `body` is one Member per enum member with `init`
// set to an explicit carrier-value expression (null => auto-assigned).
StmtPtr Parser::parseEnum(Access access) {
    auto e = mkStmt(StmtKind::Enum, cur().span);
    e->access = access;
    advance();                                    // 'enum'
    if (at(TokenKind::Identifier)) { e->name = cur().text; advance(); }
    else error("enum name");

    if (accept(TokenKind::Colon))                 // explicit carrier: `enum E : int`
        e->type = parseType();

    expect(TokenKind::LBrace, "'{'");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        auto m = mkStmt(StmtKind::Member, cur().span);
        if (at(TokenKind::Identifier)) {
            m->name = cur().text; m->selector.text = cur().text; advance();
        } else { error("enum member name"); break; }
        if (accept(TokenKind::Eq)) m->init = parseExpr(0);   // explicit carrier value
        e->body.push_back(std::move(m));
        if (!accept(TokenKind::Comma)) break;     // comma-separated, optional trailing comma
    }
    expect(TokenKind::RBrace, "'}'");
    return e;
}

// ---------------------------------------------------------------------------
//  metaprogramming surface (§16.5): attributes + comptime
// ---------------------------------------------------------------------------

// `@Name(args)` / `@NS::Name(args)`, zero or more, before a declaration.
std::vector<AttrUse> Parser::parseAttrUses() {
    std::vector<AttrUse> out;
    while (at(TokenKind::At)) {
        AttrUse a;
        a.span = cur().span;
        advance();                                   // '@'
        if (!at(TokenKind::Identifier)) { error("attribute name"); break; }
        a.name = cur().text;
        advance();
        while (at(TokenKind::ColonColon)) {          // NS::Name — last segment wins
            advance();
            if (!at(TokenKind::Identifier)) { error("attribute name"); break; }
            a.path.push_back(a.name);
            a.name = cur().text;
            advance();
        }
        if (at(TokenKind::LParen)) a.args = parseArgs();
        if (pos_ > 0) a.span.length = tokens_[pos_ - 1].span.end() - a.span.offset;
        sawMeta_ = true;
        out.push_back(std::move(a));
    }
    return out;
}

// `attribute Name { fields }` — a struct-shaped, fields-only marker type
// (§16.5 Layer A). Internally a Class node with isAttribute set; its fields
// ARE its (positional) arguments.
StmtPtr Parser::parseAttributeDecl(Access access) {
    sawMeta_ = true;
    auto cls = mkStmt(StmtKind::Class, cur().span);
    cls->access = access;
    cls->isAttribute = true;
    advance();                                       // 'attribute'
    cls->name = cur().text;
    advance();                                       // name
    expect(TokenKind::LBrace, "'{'");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        size_t before = pos_;
        StmtPtr m = parseClassMember(Access::Public);
        if (m) {
            bool plainField = m->kind == StmtKind::Member && !m->callable;
            if (plainField)
                cls->body.push_back(std::move(m));
            else
                sink_.error(m->span, "an attribute declares fields only; methods, "
                                     "constructors, and accessors are not allowed");
        }
        if (pos_ == before) { error("attribute field"); advance(); synchronize(); }
    }
    expect(TokenKind::RBrace, "'}'");
    return cls;
}

// A token that can name a declaration kind after `on`/`in` in a match clause.
// Some kinds are keywords (class/struct/interface/namespace); others are plain
// identifiers (method/function/field/constructor) — read the text either way.
static bool isDeclKindTok(const Token& t) {
    switch (t.kind) {
        case TokenKind::Identifier:
        case TokenKind::KwClass:
        case TokenKind::KwStruct:
        case TokenKind::KwInterface:
        case TokenKind::KwNamespace:
            return true;
        default:
            return false;
    }
}

// `rule Name { match ... inject ... at anchor }` (§16.5 Layer B).
StmtPtr Parser::parseRule(Access access) {
    sawMeta_ = true;
    auto r = mkStmt(StmtKind::Rule, cur().span);
    r->access = access;
    advance();                                       // 'rule'
    r->name = cur().text;
    advance();                                       // name
    // Two independent contextual markers on the rule header (Layer D, Phase 4):
    //   `rewrites body of <bind>` (§2.1) — this rule REPLACES the named bind's body
    //   `reentrant` (§4)                 — this rule may re-trigger on generated code
    // Either order; a rule may carry both (exotic but legal).
    for (int i = 0; i < 2 && at(TokenKind::Identifier); ++i) {
        if (cur().text == "rewrites" && !r->ruleRewrites) {
            r->ruleRewrites = true;
            advance();                               // 'rewrites'
            if (at(TokenKind::Identifier) && cur().text == "body") advance();
            else error("'body' after 'rewrites'");
            if (at(TokenKind::Identifier) && cur().text == "of") advance();
            else error("'of' after 'rewrites body'");
            if (at(TokenKind::Identifier)) { r->rewritesTarget = cur().text; advance(); }
            else error("the bound name whose body is replaced");
        } else if (cur().text == "reentrant" && !r->ruleReentrant) {
            r->ruleReentrant = true;
            advance();                               // 'reentrant'
        } else break;
    }
    expect(TokenKind::LBrace, "'{'");

    r->ruleMatch = std::make_unique<RuleMatch>();
    if (at(TokenKind::KwMatch)) parseRuleMatch(*r->ruleMatch);
    else error("'match' clause");

    while (at(TokenKind::KwInject) ||
           (at(TokenKind::Identifier) && cur().text == "replace")) {
        RuleAction a;
        parseRuleAction(a);
        // M30 (§2.1): a rule is additive (`inject`) XOR a rewriter (`replace`),
        // never both. `replace` demands a `rewrites` header; `inject` forbids one.
        bool isReplace = a.anchor == AnchorKind::BodyReplace;
        if (isReplace && !r->ruleRewrites)
            sink_.error(a.span, "'replace' is only legal in a 'rewrites' rule "
                        "(add 'rewrites body of <bind>' to the rule header)");
        else if (!isReplace && r->ruleRewrites)
            sink_.error(a.span, "a 'rewrites' rule uses 'replace', not 'inject' "
                        "(a rule is additive or a rewriter, never both)");
        r->ruleActions.push_back(std::move(a));
    }
    if (r->ruleActions.empty())
        error(r->ruleRewrites ? "at least one 'replace' clause"
                              : "at least one 'inject' clause");

    expect(TokenKind::RBrace, "'}'");
    return r;
}

// Template form:   `macro Name(params) => \`expr\`;`
// Procedural form: `macro Name(string payload) comptime <body>` (F4).
// Both are namespace-level contextual declarations detached by RuleEngine.
StmtPtr Parser::parseMacroDecl(Access access) {
    sawMeta_ = true;
    auto r = mkStmt(StmtKind::Rule, cur().span);
    r->isMacroDecl = true;
    r->access = access;
    advance();                                       // 'macro'
    r->name = cur().text;
    advance();                                       // name
    expect(TokenKind::LParen, "'('");
    // The only typed macro parameter in v1 is the procedural form's one
    // string. Adjacent identifiers were invalid in the template grammar, so
    // this lookahead does not steal an existing spelling.
    if (at(TokenKind::Identifier) && cur().text == "string" &&
        peek(1).kind == TokenKind::Identifier) {
        r->isProceduralMacro = true;
        advance();                                   // string
        r->generics.push_back(cur().text); advance(); // payload
        if (accept(TokenKind::Comma)) {
            sink_.error(cur().span, "a procedural macro takes exactly one string parameter in v1");
            while (!at(TokenKind::RParen) && !atEnd()) advance();
        }
    } else if (!at(TokenKind::RParen)) {
        do {
            if (at(TokenKind::Identifier)) { r->generics.push_back(cur().text); advance(); }
            else error("a macro parameter name");
        } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "')'");

    if (r->isProceduralMacro) {
        if (at(TokenKind::Identifier) && cur().text == "comptime") advance();
        else error("'comptime' after a procedural macro parameter list");
        r->memberBody = parseStatement();             // body-is-one-statement
        return r;
    }
    expect(TokenKind::Arrow, "'=>'");

    RuleAction a;
    a.span = cur().span;
    if (at(TokenKind::QuasiLiteral)) {
        a.quasiSpan = cur().span;
        advance();
        a.tmplExpr = parseExprFragment(a.quasiSpan);
    } else {
        error("a `...` template after '=>'");
    }
    r->ruleActions.push_back(std::move(a));
    expect(TokenKind::Semicolon, "';'");
    return r;
}

// match one? (@Attr(bind))? on DeclKind bind (in DeclKind bind (: Type)?)* (where expr)?
void Parser::parseRuleMatch(RuleMatch& out) {
    out.span = cur().span;
    advance();                                       // 'match'
    if (at(TokenKind::Identifier) && cur().text == "one") { out.one = true; advance(); }

    if (at(TokenKind::At)) {                          // optional @Attr(bind)
        out.hasAttr = true;
        advance();                                   // '@'
        if (at(TokenKind::Identifier)) { out.attrName = cur().text; advance(); }
        else error("attribute name");
        while (at(TokenKind::ColonColon)) {
            advance();
            out.attrPath.push_back(out.attrName);
            if (at(TokenKind::Identifier)) { out.attrName = cur().text; advance(); }
            else { error("attribute name"); break; }
        }
        if (accept(TokenKind::LParen)) {
            if (at(TokenKind::Identifier)) { out.attrBind = cur().text; advance(); }
            expect(TokenKind::RParen, "')'");
        }
    }

    // subject: on DeclKind bind
    if (at(TokenKind::Identifier) && cur().text == "on") advance();
    else error("'on'");
    if (isDeclKindTok(cur())) { out.subject.span = cur().span;
                                out.subject.kindWord = cur().text; advance(); }
    else error("declaration kind (method / class / field / ...)");
    if (at(TokenKind::Identifier)) { out.subject.bind = cur().text; advance(); }
    else error("binding name");

    // enclosers: in DeclKind bind (: Type)?
    while (at(TokenKind::KwIn)) {
        advance();
        RuleLevel lv;
        lv.span = cur().span;
        if (isDeclKindTok(cur())) { lv.kindWord = cur().text; advance(); }
        else error("declaration kind after 'in'");
        if (at(TokenKind::Identifier)) { lv.bind = cur().text; advance(); }
        else error("binding name");
        if (accept(TokenKind::Colon)) lv.constraint = parseType();
        out.enclosers.push_back(std::move(lv));
    }

    // where <expr> (Phase 3 — parsed, currently rejected by the engine)
    if (at(TokenKind::Identifier) && cur().text == "where") {
        advance();
        out.where = parseExpr(0);
    }
}

// inject `template` at <anchor>   |   replace `template`   (Layer D, Phase 4 §2.1)
void Parser::parseRuleAction(RuleAction& out) {
    out.span = cur().span;
    bool isReplace = at(TokenKind::Identifier) && cur().text == "replace";
    advance();                                       // 'inject' | 'replace'

    if (!at(TokenKind::QuasiLiteral)) {
        error(isReplace ? "a `...` template after 'replace'"
                        : "a `...` template after 'inject'");
        return;
    }
    out.quasiSpan = cur().span;
    advance();

    // `replace` has no `at <anchor>` — the target is the rule's `rewrites body of
    // <bind>`. Its template is a statement fragment (`$body` splices the original
    // body, §2.2). Set the anchor and parse; the header records the target bind.
    if (isReplace) {
        out.anchor = AnchorKind::BodyReplace;
        out.tmplStmts = parseStmtsFragment(out.quasiSpan);
        return;
    }

    if (at(TokenKind::Identifier) && cur().text == "at") advance();
    else error("'at' <anchor>");

    // Anchor: (top|bottom) of Ident . constructor | member of Ident
    //         | (top|bottom) of body | marker "n" | namespace Ident   (Phase 3)
    if (at(TokenKind::Identifier) && (cur().text == "top" || cur().text == "bottom")) {
        bool top = cur().text == "top";
        advance();
        if (at(TokenKind::Identifier) && cur().text == "of") advance();
        else error("'of'");
        if (at(TokenKind::Identifier) && cur().text == "body") {
            advance();
            out.anchor = top ? AnchorKind::BodyTop : AnchorKind::BodyBottom;
        } else {
            if (at(TokenKind::Identifier)) { out.target = cur().text; advance(); }
            else error("the bound class name");
            expect(TokenKind::Dot, "'.'");
            if (at(TokenKind::Identifier) && cur().text == "constructor") advance();
            else error("'constructor'");
            out.anchor = top ? AnchorKind::CtorTop : AnchorKind::CtorBottom;
        }
    } else if (at(TokenKind::Identifier) && cur().text == "member") {
        advance();
        if (at(TokenKind::Identifier) && cur().text == "of") advance();
        else error("'of'");
        if (at(TokenKind::Identifier)) { out.target = cur().text; advance(); }
        else error("the bound class name");
        out.anchor = AnchorKind::MemberOf;
    } else if (at(TokenKind::Identifier) && cur().text == "marker") {
        advance();
        if (at(TokenKind::StringLiteral)) { out.markerName = cur().text; advance(); }
        else error("a marker name string");
        out.anchor = AnchorKind::Marker;
    } else if (at(TokenKind::KwNamespace)) {
        advance();
        if (at(TokenKind::Identifier)) { out.target = cur().text; advance(); }
        else error("a namespace name");
        out.anchor = AnchorKind::NamespaceScope;
    } else {
        error("an anchor (top/bottom of C.constructor, or member of C)");
        return;
    }

    // Parse the saved template with the fragment parser the anchor selects.
    if (out.anchor == AnchorKind::MemberOf)
        out.tmplMember = parseMemberFragment(out.quasiSpan);
    else if (out.anchor == AnchorKind::NamespaceScope)
        out.tmplStmts = parseItemsFragment(out.quasiSpan);   // decls, not statements
    else
        out.tmplStmts = parseStmtsFragment(out.quasiSpan);
}

// --- quasiquote fragment parsing ---------------------------------------------
// A quasiquote's payload is the slice between the backticks. Re-lex it (with
// holes enabled) into fresh tokens carrying real spans into the same buffer,
// then parse with a sub-parser — so template diagnostics render like any other.

std::vector<StmtPtr> Parser::parseStmtsFragment(SourceSpan quasi) {
    std::vector<StmtPtr> out;
    if (quasi.length < 2) return out;
    Lexer lex(file_, sink_, /*allowHoles=*/true);
    std::vector<Token> toks = lex.tokenizeRange(quasi.offset + 1, quasi.end() - 1);
    // Templates are commonly a single expression with no trailing ';' (the
    // design's examples: `this.router.add(...)`). Synthesize one so the ordinary
    // statement parser accepts it; a spurious trailing ';' just parses as Empty.
    if (toks.size() >= 2) {
        const Token& last = toks[toks.size() - 2];   // before End
        if (last.kind != TokenKind::Semicolon && last.kind != TokenKind::RBrace) {
            Token semi;
            semi.kind = TokenKind::Semicolon;
            semi.span = {quasi.end() - 1, 0};
            toks.insert(toks.end() - 1, semi);
        }
    }
    Parser sub(std::move(toks), file_, sink_);
    while (!sub.atEnd()) {
        size_t before = sub.pos_;
        StmtPtr s = sub.parseFragmentStmt(SpliceBody::Stmt);
        if (s && s->kind != StmtKind::Empty) out.push_back(std::move(s));
        if (sub.pos_ == before) { sub.error("statement in template"); break; }
    }
    return out;
}

StmtPtr Parser::parseMemberFragment(SourceSpan quasi) {
    if (quasi.length < 2) return nullptr;
    Lexer lex(file_, sink_, /*allowHoles=*/true);
    Parser sub(lex.tokenizeRange(quasi.offset + 1, quasi.end() - 1), file_, sink_);
    if (sub.atEnd()) { sub.error("a member in template"); return nullptr; }
    return sub.parseFragmentStmt(SpliceBody::Member);   // member, `$for`, or `$if`
}

ExprPtr Parser::parseExprFragment(SourceSpan quasi) {
    if (quasi.length < 2) return nullptr;
    Lexer lex(file_, sink_, /*allowHoles=*/true);
    Parser sub(lex.tokenizeRange(quasi.offset + 1, quasi.end() - 1), file_, sink_);
    if (sub.atEnd()) { sub.error("an expression in template"); return nullptr; }
    return sub.parseExpr(0);
}

ExprPtr Parser::parseExpressionFragment() {
    if (atEnd()) { error("an expression"); return nullptr; }
    ExprPtr out = parseExpr(0);
    if (!atEnd()) error("end of generated expression");
    return out;
}

std::vector<StmtPtr> Parser::parseStatementsFragment() {
    std::vector<StmtPtr> out;
    while (!atEnd()) {
        size_t before = pos_;
        StmtPtr s = parseStatement();
        if (s && s->kind != StmtKind::Empty) out.push_back(std::move(s));
        if (pos_ == before) { error("statement in generated code"); break; }
    }
    return out;
}

// F4: string interpolation. `t` is the whole StringLiteral token (raw text,
// quotes included, escapes NOT yet decoded — decoding happens once, later,
// uniformly for every literal segment via decodeStringLiteral). Builds a
// left-associative `+`-chain of literal-segment StringLit nodes and
// `(hole).toString()` Call nodes; zero holes reconstructs a single StringLit
// whose text is byte-identical to `t.text` (a substr copy, not a special
// case — see the header comment).
//
// request-string-literal-tail: `t` may also be a `"""`/`'''`-delimited
// multiline literal — same escape/interpolation rules, just a 3-char quote
// on each side instead of 1 (raw strings never reach here; they're their
// own TokenKind and skip this function entirely, see parsePrimary).
ExprPtr Parser::parseInterpolatedString(const Token& t) {
    std::string_view raw = t.text;                 // includes the quotes
    size_t quoteLen = (raw.size() >= 6 && raw[0] == raw[1] && raw[1] == raw[2] &&
                        (raw[0] == '"' || raw[0] == '\''))
                          ? 3 : 1;
    size_t contentEnd = raw.size() - quoteLen;      // exclusive; raw[contentEnd..] is the closing quote(s)
    std::vector<ExprPtr> pieces;

    auto pushSegment = [&](size_t segStart, size_t segEnd) {
        if (segEnd <= segStart) return;             // skip empty segments (no `"" + ...` noise)
        auto seg = mkExpr(ExprKind::StringLit, t.span);
        // A bare content slice — a real view into the source buffer (raw IS
        // t.text, itself a view into file_.text), never synthesized text, so
        // this stays stable with no pool/lifetime machinery. isRawSegment
        // tells Eval/Lower to decode it directly, with no quotes to strip.
        seg->text = raw.substr(segStart, segEnd - segStart);
        seg->isRawSegment = true;
        pieces.push_back(std::move(seg));
    };

    size_t segStart = quoteLen;
    size_t i = quoteLen;
    while (i < contentEnd) {
        char c = raw[i];
        if (c == '\\' && i + 1 < contentEnd) { i += 2; continue; }   // an escape pair: not a hole start
        if (c == '$' && i + 1 < contentEnd && raw[i + 1] == '{') {
            pushSegment(segStart, i);
            size_t holeStart = i + 2;
            size_t j = holeStart;
            int depth = 1;
            char inStr = '\0';
            while (j < contentEnd && depth > 0) {
                char cj = raw[j];
                if (inStr) {
                    if (cj == '\\' && j + 1 < contentEnd) { j += 2; continue; }
                    if (cj == inStr) inStr = '\0';
                    ++j;
                    continue;
                }
                if (cj == '"' || cj == '\'') { inStr = cj; ++j; continue; }
                if (cj == '{') { ++depth; ++j; continue; }
                if (cj == '}') { --depth; if (depth == 0) break; ++j; continue; }
                ++j;
            }
            if (depth != 0) {
                sink_.error({t.span.offset + (uint32_t)i, (uint32_t)(contentEnd - i)},
                            "unterminated interpolation hole");
                segStart = contentEnd;
                break;
            }
            if (j == holeStart) {
                sink_.error({t.span.offset + (uint32_t)i, (uint32_t)(j + 1 - i)},
                            "empty interpolation hole '${}'");
            } else {
                uint32_t absStart = t.span.offset + (uint32_t)holeStart;
                uint32_t absEnd = t.span.offset + (uint32_t)j;
                Lexer lex(file_, sink_, /*allowHoles=*/false);
                Parser sub(lex.tokenizeRange(absStart, absEnd), file_, sink_);
                ExprPtr holeExpr = sub.atEnd() ? nullptr : sub.parseExpr(0);
                if (holeExpr) {
                    auto ts = mkExpr(ExprKind::Member, holeExpr->span);
                    ts->text = "toString";
                    ts->a = std::move(holeExpr);
                    auto call = mkExpr(ExprKind::Call, ts->span);
                    call->a = std::move(ts);
                    pieces.push_back(std::move(call));
                } else {
                    sink_.error({absStart, absEnd - absStart}, "an expression in interpolation hole");
                }
            }
            i = j + 1;
            segStart = i;
            continue;
        }
        ++i;
    }
    pushSegment(segStart, contentEnd);

    if (pieces.empty()) {
        // Only reachable via the empty-hole/unterminated-hole error paths
        // (a hole that contributed no piece and no surrounding text) —
        // give the checker/evaluator SOMETHING typed, the error already ran.
        auto e = mkExpr(ExprKind::StringLit, t.span);
        e->text = "\"\"";
        return e;
    }
    ExprPtr result = std::move(pieces[0]);
    for (size_t k = 1; k < pieces.size(); ++k) {
        auto bin = mkExpr(ExprKind::Binary, result->span);
        bin->op = TokenKind::Plus;
        bin->a = std::move(result);
        bin->b = std::move(pieces[k]);
        result = std::move(bin);
    }
    // Track 03 §1: a simple (single-segment, no interpolation) single-quoted
    // literal is the only shape that can become `char`. Record the quote style
    // now — it is unrecoverable from the quote-stripped raw segment later.
    // A triple-quoted literal (quoteLen == 3) never qualifies, even if it
    // happens to decode to one scalar — multiline syntax is not char syntax.
    if (pieces.size() == 1 && result->kind == ExprKind::StringLit && quoteLen == 1 &&
        !raw.empty() && raw.front() == '\'')
        result->singleQuoted = true;
    return result;
}

// `namespace N` templates (§8.3) hold declarations (classes/functions), which
// statement fragments can't produce — a dedicated top-level-item loop.
std::vector<StmtPtr> Parser::parseItemsFragment(SourceSpan quasi) {
    std::vector<StmtPtr> out;
    if (quasi.length < 2) return out;
    Lexer lex(file_, sink_, /*allowHoles=*/true);
    Parser sub(lex.tokenizeRange(quasi.offset + 1, quasi.end() - 1), file_, sink_);
    while (!sub.atEnd()) {
        size_t before = sub.pos_;
        StmtPtr s = sub.parseFragmentStmt(SpliceBody::Item);
        if (s) out.push_back(std::move(s));
        if (sub.pos_ == before) { sub.error("declaration in template"); break; }
    }
    return out;
}

// `comptime <var-decl>` | `comptime if (...)` | `comptime <expr>;` at
// statement/declaration position. Contextual: `comptime` stays an ordinary
// name unless what follows can begin a declaration, an `if`, or an expression
// (so `comptime + 1` still reads a variable named comptime).
StmtPtr Parser::tryParseComptime() {
    if (!(at(TokenKind::Identifier) && cur().text == "comptime")) return nullptr;
    TokenKind n = peek(1).kind;
    bool marker = n == TokenKind::KwIf || n == TokenKind::KwVar ||
                  n == TokenKind::KwLet || canStartExpr(n);
    if (n == TokenKind::LParen) marker = true;       // reserved-in-practice
    if (!marker) return nullptr;
    sawMeta_ = true;
    SourceSpan sp = cur().span;
    advance();                                       // 'comptime'
    if (at(TokenKind::KwIf)) {
        StmtPtr s = parseStatement();
        if (s) s->isComptime = true;
        return s;
    }
    if (at(TokenKind::KwVar) || at(TokenKind::KwLet) || looksLikeVarDecl()) {
        StmtPtr v = parseVarDecl();
        if (v) {
            v->isComptime = true;
            if (!v->init)
                sink_.error(v->span, "a comptime declaration needs an initializer");
        }
        return v;
    }
    auto s = mkStmt(StmtKind::ExprStmt, sp);
    s->expr = parseExpr(0);
    if (s->expr) s->expr->isComptime = true;
    expect(TokenKind::Semicolon, "';'");
    return s;
}

bool Parser::nextStartsExpr(size_t ahead) const {
    return canStartExpr(peek(ahead).kind);
}

StmtPtr Parser::parseNamespace() {
    SourceSpan sp = cur().span;
    advance();  // 'namespace'
    // bug #37: a qualified namespace declaration `namespace A::B { ... }` is
    // sugar for nested `namespace A { namespace B { ... } }`. The owner's own
    // example app (designs/atlantis/example/) is written `namespace App::Models
    // { ... }`, so the qualified spelling must desugar directly. Collect every
    // `Ident` segment; the innermost holds the body, the rest wrap it.
    // string_view into the (stable) source — NOT a copied std::string, whose
    // storage would die with this function and leave ns->name dangling.
    std::vector<std::string_view> names;
    if (at(TokenKind::Identifier)) { names.push_back(cur().text); advance(); }
    else error("namespace name");
    while (accept(TokenKind::ColonColon)) {
        if (at(TokenKind::Identifier)) { names.push_back(cur().text); advance(); }
        else { error("namespace name after '::'"); break; }
    }
    if (names.empty()) names.push_back(std::string_view{});
    expect(TokenKind::LBrace, "'{'");
    auto ns = mkStmt(StmtKind::Namespace, sp);
    ns->name = names.back();
    while (!at(TokenKind::RBrace) && !atEnd()) {
        size_t before = pos_;
        StmtPtr item = parseTopLevelItem();
        if (item) ns->body.push_back(std::move(item));
        if (pos_ == before) { error("declaration"); advance(); synchronize(); }
    }
    expect(TokenKind::RBrace, "'}'");
    for (int i = (int)names.size() - 2; i >= 0; --i) {
        auto outer = mkStmt(StmtKind::Namespace, sp);
        outer->name = names[(size_t)i];
        outer->body.push_back(std::move(ns));
        ns = std::move(outer);
    }
    return ns;
}

// Attributes bind to the declaration that follows them (§16.5 Layer A). The
// wrapper keeps the existing item grammar untouched and just attaches.
StmtPtr Parser::parseTopLevelItem() {
    std::vector<AttrUse> attrs = parseAttrUses();
    StmtPtr item = parseTopLevelItemInner();
    if (!attrs.empty()) {
        bool declLike = item && (item->kind == StmtKind::Class ||
                                 item->kind == StmtKind::Namespace ||
                                 item->kind == StmtKind::Var ||
                                 (item->kind == StmtKind::Member && item->callable));
        if (declLike) item->attrs = std::move(attrs);
        else sink_.error(attrs.front().span,
                         "an attribute must precede a declaration "
                         "(class, struct, function, field, or namespace)");
    }
    return item;
}

StmtPtr Parser::parseTopLevelItemInner() {
    // comptime var / comptime if / comptime expression-statement (§16.5 Layer C)
    if (StmtPtr ct = tryParseComptime()) return ct;

    Access acc = Access::Default;
    if (accept(TokenKind::KwPublic)) acc = Access::Public;
    else if (accept(TokenKind::KwPrivate)) acc = Access::Private;

    // const.md §3.3: namespace/top-level global, fixed at the initializer only.
    // (Global `var`/`let` inference isn't a surface this parser has today —
    // globals always carry an explicit type — so `const` composes with that
    // same explicit-type shape, not with `var`/`let`.)
    if (accept(TokenKind::KwConst)) {
        TypeRefPtr ty = parseType();
        auto v = mkStmt(StmtKind::Var, ty->span);
        v->access = acc; v->isConst = true;
        v->type = std::move(ty);
        if (at(TokenKind::Identifier)) { v->name = cur().text; advance(); }
        else error("variable name");
        if (accept(TokenKind::Eq)) v->init = parseExpr(0);
        expect(TokenKind::Semicolon, "';'");
        return v;
    }

    // `attribute Name { fields }` — contextual keyword, declaration position
    if (at(TokenKind::Identifier) && cur().text == "attribute" &&
        peek(1).kind == TokenKind::Identifier && peek(2).kind == TokenKind::LBrace)
        return parseAttributeDecl(acc);

    // `rule Name { ... }` / `rule Name rewrites … { ... }` / `rule Name reentrant …`
    // — contextual keyword. Recognized by the triple: Identifier("rule") Identifier
    // then '{' or a header marker ("rewrites" | "reentrant", Layer D Phase 4).
    if (at(TokenKind::Identifier) && cur().text == "rule" &&
        peek(1).kind == TokenKind::Identifier &&
        (peek(2).kind == TokenKind::LBrace ||
         (peek(2).kind == TokenKind::Identifier &&
          (peek(2).text == "rewrites" || peek(2).text == "reentrant"))))
        return parseRule(acc);

    // `macro Name(params) => \`expr\`;` — contextual keyword (Phase 3 §7).
    // Recognized by the triple: Identifier("macro") Identifier LParen.
    if (at(TokenKind::Identifier) && cur().text == "macro" &&
        peek(1).kind == TokenKind::Identifier && peek(2).kind == TokenKind::LParen)
        return parseMacroDecl(acc);

    if (at(TokenKind::KwNamespace)) return parseNamespace();
    if (at(TokenKind::KwClass)) return parseClass(acc, false);
    if (at(TokenKind::KwStruct)) return parseClass(acc, false, /*isValue=*/true);
    if (at(TokenKind::KwInterface)) return parseClass(acc, true);
    if (at(TokenKind::KwEnum)) return parseEnum(acc);
    if (at(TokenKind::KwBind)) return parseBind();
    if (at(TokenKind::KwUses)) return parseUses();
    if (at(TokenKind::KwUse)) return parseUse();
    if (at(TokenKind::KwThrow) || at(TokenKind::KwTry)) return parseStatement();

    // function / global var:  Type Name ( ... )   |   Type Name ( = init )? ;
    // The Name may be a contextual keyword (`get`/`set`/`is`/…) exactly as a
    // class member's name may (parseMember §979, isContextualName): the accessor
    // sense of get/set only binds inside a class, so `string? get(...)` at
    // namespace/top level is an ordinary function named `get`.
    bool declOrFunc = false;
    // bug #44: a return/declared type may be a lambda type starting with '(' —
    // `(Foo) => Foo makeHandler()` / `(Foo) => Foo h = …;` — not just an
    // identifier type. skipTypeFull skips either form; an ordinary
    // parenthesized expression leaves the index unmoved and falls through.
    size_t typeEnd = pos_;
    if (peek(0).kind == TokenKind::Identifier)
        typeEnd = skipTypeSuffix(tokens_, pos_ + 1);
    else if (peek(0).kind == TokenKind::LParen)
        typeEnd = skipTypeFull(tokens_, pos_);
    if (typeEnd > pos_) {
        size_t i = typeEnd;
        if (i < tokens_.size() && (tokens_[i].kind == TokenKind::Identifier ||
                                   isContextualName(tokens_[i].kind))) {
            size_t j = i + 1;
            if (j < tokens_.size() && tokens_[j].kind == TokenKind::Lt)
                j = skipAngles(tokens_, j);       // skip method/function type params
            TokenKind after = (j < tokens_.size()) ? tokens_[j].kind : TokenKind::End;
            declOrFunc = (after == TokenKind::LParen || after == TokenKind::Eq ||
                          after == TokenKind::Semicolon);
        }
    }

    if (declOrFunc) {
        TypeRefPtr ty = parseType();
        std::string_view nm;
        if (at(TokenKind::Identifier) || isContextualName(cur().kind)) {
            nm = cur().text; advance();
        } else error("name");

        std::vector<std::string_view> tps;
        parseTypeParams(tps);                     // function type params

        if (at(TokenKind::LParen)) {              // function
            auto fn = mkStmt(StmtKind::Member, ty->span);
            fn->access = acc; fn->callable = true;
            fn->type = std::move(ty);
            fn->name = nm; fn->selector.text = nm;
            fn->generics = std::move(tps);
            fn->params = parseParamList();
            fn->memberBody = parseStatement();
            return fn;
        }
        auto v = mkStmt(StmtKind::Var, ty->span);  // global var
        v->access = acc;
        v->type = std::move(ty);
        v->name = nm;
        if (accept(TokenKind::Eq)) v->init = parseExpr(0);
        expect(TokenKind::Semicolon, "';'");
        return v;
    }

    // bare expression statement (e.g. `Demo::run();`)
    auto s = mkStmt(StmtKind::ExprStmt, cur().span);
    s->expr = parseExpr(0);
    // R6 (005, bug #84): a block-terminated bare `match` needs no trailing ';',
    // mirroring parseStatement's default case — otherwise a top-level `match`
    // followed by another statement fails with "expected ';'" at the *next* one.
    if (s->expr && s->expr->kind == ExprKind::Match) accept(TokenKind::Semicolon);
    else expect(TokenKind::Semicolon, "';'");
    return s;
}

Program Parser::parseProgram() {
    Program prog;
    while (!atEnd()) {
        size_t before = pos_;
        StmtPtr item = parseTopLevelItem();
        if (item) prog.items.push_back(std::move(item));
        if (pos_ == before) { error("declaration"); advance(); synchronize(); }
    }
    prog.hasMeta = sawMeta_;   // gates the rule stage; false = today's pipeline
    return prog;
}
