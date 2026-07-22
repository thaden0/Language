#include "frontend/Parser.hpp"
#include "frontend/ParserInternal.hpp"
#include <string>

namespace parserDetail {

// ---------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------

// A keyword that may also serve as a member name after '.' / '::'. The keyword
// sense only binds at statement/declaration position, so member access can use
// these freely (client.get(...), resp.is, x.in).
bool isContextualName(TokenKind k) {
    switch (k) {
        case TokenKind::KwGet: case TokenKind::KwSet: case TokenKind::KwIs:
        case TokenKind::KwIn: case TokenKind::KwUse: case TokenKind::KwUses:
        case TokenKind::KwNew: case TokenKind::KwBind: case TokenKind::KwInject:
            return true;
        default:
            return false;
    }
}

bool canStartExpr(TokenKind k) {
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

StmtPtr mkStmt(StmtKind k, SourceSpan sp) {
    auto s = std::make_unique<Stmt>(k);
    s->span = sp;
    return s;
}

}  // namespace parserDetail

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
    if (t[i].kind == TokenKind::Identifier) {
        // B1 dotted-hole type (techdesign-splices-positions §2): `$f.type` /
        // `$p.name` is one type token in fragment mode. Consume the `.field` so
        // the decl-vs-expression lookahead sees the var name that follows it.
        size_t j = i + 1;
        if (!t[i].text.empty() && t[i].text[0] == '$' && j + 1 < t.size() &&
            t[j].kind == TokenKind::Dot && t[j + 1].kind == TokenKind::Identifier)
            j += 2;
        return skipTypeSuffix(t, j);
    }
    return i;
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

    // B1 dotted-hole type (techdesign-splices-positions §2): `$f.type` /
    // `$p.name` — a bound meta value's canonical-string field standing in for a
    // type name. Only after a `$hole` head; the field is resolved at clone time
    // (cloneType), not here. `.field` is consumed so the type ends before it.
    if (!t->name.empty() && t->name[0] == '$' && at(TokenKind::Dot) &&
        peek(1).kind == TokenKind::Identifier) {
        t->holeBind = t->name;                       // "$f"
        advance();                                   // '.'
        t->holeField = cur().text;                   // "type" | "name"
        advance();
        return wrapNullable(std::move(t));
    }

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
    if (peek(0).kind == TokenKind::Identifier) {
        size_t after = pos_ + 1;
        // B1 dotted-hole type `$f.type` (fragment): consume `.field` so the var
        // name after it is seen, exactly as skipTypeFull does for the same shape.
        if (!peek(0).text.empty() && peek(0).text[0] == '$' &&
            pos_ + 2 < tokens_.size() && tokens_[pos_ + 1].kind == TokenKind::Dot &&
            tokens_[pos_ + 2].kind == TokenKind::Identifier)
            after = pos_ + 3;
        i = skipTypeSuffix(tokens_, after);
    }
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
    auto v = parserDetail::mkStmt(StmtKind::Var, cur().span);
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
    auto b = parserDetail::mkStmt(StmtKind::Block, cur().span);
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
    auto b = parserDetail::mkStmt(StmtKind::Bind, cur().span);
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
    auto s = parserDetail::mkStmt(StmtKind::UsesImport, cur().span);
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
    auto s = parserDetail::mkStmt(StmtKind::Use, cur().span);
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
            auto r = parserDetail::mkStmt(StmtKind::Return, cur().span);
            advance();
            if (!at(TokenKind::Semicolon)) r->expr = parseExpr(0);
            expect(TokenKind::Semicolon, "';'");
            return r;
        }
        case TokenKind::Arrow: {                 // '=> expr;'  (return sugar)
            auto r = parserDetail::mkStmt(StmtKind::Return, cur().span);
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
                auto s = parserDetail::mkStmt(StmtKind::Empty, sp);
                s->name = marker;
                return s;
            }
            // `@Name();` (named-anchor design §2.1) — a statement-position, named
            // splice site. `Name` must name a declared `attribute` (checked at the
            // rule stage, M43); the trailing `()` + `;` in statement position is
            // what distinguishes a splice site from a decl-position decorator.
            // Produces the same StmtKind::Empty marker node as `@anchor`, but with
            // the bare attribute name and isSpliceSite set.
            if (peek(1).kind == TokenKind::Identifier &&
                peek(2).kind == TokenKind::LParen) {
                advance();                     // '@'
                std::string_view spliceName = cur().text;
                advance();                     // Name
                advance();                     // '('
                if (!at(TokenKind::RParen))
                    error("a splice site takes no arguments — '@Name()'");
                expect(TokenKind::RParen, "')'");
                expect(TokenKind::Semicolon, "';'");
                sawMeta_ = true;
                auto s = parserDetail::mkStmt(StmtKind::Empty, sp);
                s->name = spliceName;
                s->isSpliceSite = true;
                return s;
            }
            error("'anchor(\"name\")' or a splice site '@Name();' after '@' "
                  "(decorations are declaration-position only)");
            advance();
            return parserDetail::mkStmt(StmtKind::Empty, sp);
        }
        case TokenKind::Semicolon:
            return (advance(), parserDetail::mkStmt(StmtKind::Empty, cur().span));
        case TokenKind::KwIf: {
            auto s = parserDetail::mkStmt(StmtKind::If, cur().span);
            advance();
            expect(TokenKind::LParen, "'('");
            s->expr = parseExpr(0);
            expect(TokenKind::RParen, "')'");
            s->thenBranch = parseStatement();
            if (accept(TokenKind::KwElse)) s->elseBranch = parseStatement();
            return s;
        }
        case TokenKind::KwThrow: {
            auto s = parserDetail::mkStmt(StmtKind::Throw, cur().span);
            advance();
            s->expr = parseExpr(0);
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        case TokenKind::KwTry: {
            auto s = parserDetail::mkStmt(StmtKind::Try, cur().span);
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
            auto s = parserDetail::mkStmt(StmtKind::While, cur().span);
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
                auto s = parserDetail::mkStmt(StmtKind::ForIn, sp);
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

            auto s = parserDetail::mkStmt(StmtKind::For, sp);
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
            auto s = parserDetail::mkStmt(StmtKind::Break, cur().span);
            advance();
            if (at(TokenKind::Identifier)) { s->label = cur().text; advance(); }
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        case TokenKind::KwContinue: {
            auto s = parserDetail::mkStmt(StmtKind::Continue, cur().span);
            advance();
            if (at(TokenKind::Identifier)) { s->label = cur().text; advance(); }
            expect(TokenKind::Semicolon, "';'");
            return s;
        }
        case TokenKind::KwDo: {
            auto s = parserDetail::mkStmt(StmtKind::DoWhile, cur().span);
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
            auto s = parserDetail::mkStmt(StmtKind::ExprStmt, cur().span);
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
    bool lastWasGroup = false;
    std::vector<AttrUse> attrs = parseAttrUses(&lastWasGroup);
    StmtPtr m = parseClassMemberInner(sectionAccess, sectionConst);
    if (!attrs.empty()) {
        if (m && m->kind == StmtKind::Member) m->attrs = std::move(attrs);
        else if (lastWasGroup) sink_.error(attrs.front().span,
                         "@attr(...) must precede a declaration");
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
        auto m = parserDetail::mkStmt(StmtKind::Member, cur().span);
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
        auto m = parserDetail::mkStmt(StmtKind::Member, cur().span);
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
    auto m = parserDetail::mkStmt(StmtKind::Member, cur().span);
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
    if (tryParseNameSynth(m.get())) {
        /* C: `int $ident(C.name,"Get")()` — name synthesized at expansion */
    } else if (at(TokenKind::Identifier) || parserDetail::isContextualName(cur().kind)) {
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

// C name synthesis (techdesign-splices-positions §3): `$ident(a, b, …)`. `$ident`
// lexes as a hole only in fragment mode, so this branch is dead in ordinary
// source. The args are ordinary comptime expressions (a `$hole.field`, a bare
// `$hole`, or a string literal); the rule engine evaluates + concatenates them
// to the actual name at expansion (M37/M38).
bool Parser::tryParseNameSynth(Stmt* into) {
    if (!(at(TokenKind::Identifier) && cur().text == "$ident" &&
          peek(1).kind == TokenKind::LParen))
        return false;
    advance();                                       // '$ident'
    into->nameSynthArgs = parseArgs();               // ( a, b, … )
    into->hasNameSynth = true;
    if (into->nameSynthArgs.empty())
        error("at least one comptime-string argument to $ident");
    return true;
}

StmtPtr Parser::parseClass(Access access, bool isInterface, bool isValue) {
    auto c = parserDetail::mkStmt(StmtKind::Class, cur().span);
    c->access = access; c->isInterface = isInterface; c->isValue = isValue;
    advance();  // 'class' / 'struct' / 'interface'
    if (tryParseNameSynth(c.get())) { /* name synthesized at expansion */ }
    else if (at(TokenKind::Identifier)) { c->name = cur().text; advance(); }
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
        // Member-position `$for`/`$if` splices inside a quasiquoted class body
        // (`class $ident(…) { $for f … : … }`, techdesign-splices-positions §3.5).
        // These heads lex only in fragment mode, so this dispatch is dead in
        // ordinary source — the same reasoning the statement grammar uses for a
        // body-nested `$for`.
        StmtPtr m;
        if (at(TokenKind::Identifier) &&
            (cur().text == "$for" || cur().text == "$if" || cur().text == "$else"))
            m = parseFragmentStmt(SpliceBody::Member);
        else
            m = parseClassMember(section, constSection);
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
    auto e = parserDetail::mkStmt(StmtKind::Enum, cur().span);
    e->access = access;
    advance();                                    // 'enum'
    if (at(TokenKind::Identifier)) { e->name = cur().text; advance(); }
    else error("enum name");

    if (accept(TokenKind::Colon))                 // explicit carrier: `enum E : int`
        e->type = parseType();

    expect(TokenKind::LBrace, "'{'");
    while (!at(TokenKind::RBrace) && !atEnd()) {
        auto m = parserDetail::mkStmt(StmtKind::Member, cur().span);
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
std::vector<AttrUse> Parser::parseAttrUses(bool* lastWasGroup) {
    std::vector<AttrUse> out;
    if (lastWasGroup) *lastWasGroup = false;
    while (at(TokenKind::At)) {
        // `@attr(Name1, Name2, …);` (techdesign-splices-desugars-sonnet.md §1) —
        // statement/member-position sugar for `@Name1 @Name2 …` on the next
        // declaration. `attr` is a contextual keyword only in this exact shape
        // (`@` `attr` `(` at a decorator-prefix position, a shape no ordinary
        // decorator use — with or without an attribute literally named `attr`
        // — collides with, since a real `@attr(...)` decorator never continues
        // with a bare `;` right after its ')').
        if (peek(1).kind == TokenKind::Identifier && peek(1).text == "attr" &&
            peek(2).kind == TokenKind::LParen) {
            advance();                               // '@'
            advance();                               // 'attr'
            advance();                               // '('
            do {
                AttrUse a;
                a.span = cur().span;
                if (!at(TokenKind::Identifier)) { error("attribute name"); break; }
                a.name = cur().text;
                advance();
                while (at(TokenKind::ColonColon)) {
                    advance();
                    if (!at(TokenKind::Identifier)) { error("attribute name"); break; }
                    a.path.push_back(a.name);
                    a.name = cur().text;
                    advance();
                }
                if (pos_ > 0) a.span.length = tokens_[pos_ - 1].span.end() - a.span.offset;
                out.push_back(std::move(a));
            } while (accept(TokenKind::Comma));
            expect(TokenKind::RParen, "')'");
            expect(TokenKind::Semicolon, "';'");
            sawMeta_ = true;
            if (lastWasGroup) *lastWasGroup = true;
            // A body's declaration loop always attempts to parse a member/item
            // even at its closing brace (there is no "nothing left" sentinel
            // return), so it cascades unrelated errors rather than surfacing
            // this specific one — check directly, here, where the dangling
            // group is actually detected (design point F.4).
            if (at(TokenKind::RBrace) || atEnd())
                sink_.error(cur().span, "@attr(...) must precede a declaration");
            continue;
        }
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
        if (lastWasGroup) *lastWasGroup = false;
        out.push_back(std::move(a));
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
                  n == TokenKind::KwLet || parserDetail::canStartExpr(n);
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
    auto s = parserDetail::mkStmt(StmtKind::ExprStmt, sp);
    s->expr = parseExpr(0);
    if (s->expr) s->expr->isComptime = true;
    expect(TokenKind::Semicolon, "';'");
    return s;
}

bool Parser::nextStartsExpr(size_t ahead) const {
    return parserDetail::canStartExpr(peek(ahead).kind);
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
    auto ns = parserDetail::mkStmt(StmtKind::Namespace, sp);
    ns->name = names.back();
    while (!at(TokenKind::RBrace) && !atEnd()) {
        size_t before = pos_;
        StmtPtr item = parseTopLevelItem();
        if (item) ns->body.push_back(std::move(item));
        if (pos_ == before) { error("declaration"); advance(); synchronize(); }
    }
    expect(TokenKind::RBrace, "'}'");
    for (int i = (int)names.size() - 2; i >= 0; --i) {
        auto outer = parserDetail::mkStmt(StmtKind::Namespace, sp);
        outer->name = names[(size_t)i];
        outer->body.push_back(std::move(ns));
        ns = std::move(outer);
    }
    return ns;
}

// Attributes bind to the declaration that follows them (§16.5 Layer A). The
// wrapper keeps the existing item grammar untouched and just attaches.
StmtPtr Parser::parseTopLevelItem() {
    bool lastWasGroup = false;
    std::vector<AttrUse> attrs = parseAttrUses(&lastWasGroup);
    StmtPtr item = parseTopLevelItemInner();
    if (!attrs.empty()) {
        bool declLike = item && (item->kind == StmtKind::Class ||
                                 item->kind == StmtKind::Namespace ||
                                 item->kind == StmtKind::Var ||
                                 (item->kind == StmtKind::Member && item->callable));
        if (declLike) item->attrs = std::move(attrs);
        else if (lastWasGroup) sink_.error(attrs.front().span,
                         "@attr(...) must precede a declaration");
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
        auto v = parserDetail::mkStmt(StmtKind::Var, ty->span);
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

    // `rule Name { ... }` / `rule Name rewrites … { ... }` / `rule Name generates …`
    // / `rule Name reentrant …` — contextual keyword. Recognized by the triple:
    // Identifier("rule") Identifier then '{' or a header marker ("rewrites" |
    // "generates" | "reentrant", Layer D Phase 4 / bindgen metaprog scope).
    if (at(TokenKind::Identifier) && cur().text == "rule" &&
        peek(1).kind == TokenKind::Identifier &&
        (peek(2).kind == TokenKind::LBrace ||
         (peek(2).kind == TokenKind::Identifier &&
          (peek(2).text == "rewrites" || peek(2).text == "generates" ||
           peek(2).text == "reentrant"))))
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
    // Statement-form control flow in the top-level (script) body. parseStatement
    // already handles every one of these verbatim; without this route they fall
    // through to the bare-expression path below and fail with "expected
    // expression" — even though `try`/`throw` (routed here) and a bare `match`
    // (an expression, handled below) already work at the top level, and all of
    // if/while/for/do work identically inside any function body. Top-level
    // statements ARE the program body (reference.md §4.3 "top-level statements
    // run in source order"), so a script's `for`/`if`/`while`/`do` belongs here.
    // `break`/`continue`/`return` are intentionally NOT routed: they have no
    // enclosing loop/function at the top level and stay rejected.
    if (at(TokenKind::KwThrow) || at(TokenKind::KwTry) ||
        at(TokenKind::KwIf) || at(TokenKind::KwWhile) ||
        at(TokenKind::KwFor) || at(TokenKind::KwDo))
        return parseStatement();
    // Labeled top-level loop: `label: for/while/do …` — same three-token
    // lookahead parseStatement uses; the recursive call lands in the loop case.
    if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon &&
        (peek(2).kind == TokenKind::KwWhile || peek(2).kind == TokenKind::KwFor ||
         peek(2).kind == TokenKind::KwDo))
        return parseStatement();

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
                                   parserDetail::isContextualName(tokens_[i].kind))) {
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
        if (at(TokenKind::Identifier) || parserDetail::isContextualName(cur().kind)) {
            nm = cur().text; advance();
        } else error("name");

        std::vector<std::string_view> tps;
        parseTypeParams(tps);                     // function type params

        if (at(TokenKind::LParen)) {              // function
            auto fn = parserDetail::mkStmt(StmtKind::Member, ty->span);
            fn->access = acc; fn->callable = true;
            fn->type = std::move(ty);
            fn->name = nm; fn->selector.text = nm;
            fn->generics = std::move(tps);
            fn->params = parseParamList();
            fn->memberBody = parseStatement();
            return fn;
        }
        auto v = parserDetail::mkStmt(StmtKind::Var, ty->span);  // global var
        v->access = acc;
        v->type = std::move(ty);
        v->name = nm;
        if (accept(TokenKind::Eq)) v->init = parseExpr(0);
        expect(TokenKind::Semicolon, "';'");
        return v;
    }

    // bare expression statement (e.g. `Demo::run();`)
    auto s = parserDetail::mkStmt(StmtKind::ExprStmt, cur().span);
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
