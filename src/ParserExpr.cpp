#include "ParserInternal.hpp"
#include "Lexer.hpp"

// ---------------------------------------------------------------------------
//  expressions (Pratt) — refactor_1 session 04
//
//  Everything reachable from Parser::parseExpr: the Pratt core itself,
//  unary/postfix/primary, typed-lambda-parameter disambiguation,
//  array-literal elements (incl. their own `$for`/`$if` splice forms), and
//  string interpolation. Statement/declaration grammar stays in Parser.cpp;
//  the rule/macro/quasiquote-fragment grammar is in ParserMeta.cpp.
// ---------------------------------------------------------------------------

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

static ExprPtr mkExpr(ExprKind k, SourceSpan sp) {
    auto e = std::make_unique<Expr>(k);
    e->span = sp;
    return e;
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
            if (at(TokenKind::Identifier) || parserDetail::isContextualName(cur().kind)) {
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
        if (k == TokenKind::GtGt && !parserDetail::canStartExpr(cur().kind)) {
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
