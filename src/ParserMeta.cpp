#include "ParserInternal.hpp"
#include "Lexer.hpp"

// ---------------------------------------------------------------------------
//  metaprogramming grammar (§16.5) — refactor_1 session 04
//
//  rule/macro/attribute-declaration grammar, statement-position `$for`/`$if`
//  splices, and quasiquote-fragment re-parsing. `parseAttrUses` (the
//  `@Name(args)` prefix on an ordinary declaration) and `comptime`
//  var/if/expr-stmt stay in Parser.cpp — both are driven entirely from its
//  statement/declaration grammar, not from here.
// ---------------------------------------------------------------------------

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

// `attribute Name { fields }` — a struct-shaped, fields-only marker type
// (§16.5 Layer A). Internally a Class node with isAttribute set; its fields
// ARE its (positional) arguments.
StmtPtr Parser::parseAttributeDecl(Access access) {
    sawMeta_ = true;
    auto cls = parserDetail::mkStmt(StmtKind::Class, cur().span);
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
    auto r = parserDetail::mkStmt(StmtKind::Rule, cur().span);
    r->access = access;
    advance();                                       // 'rule'
    r->name = cur().text;
    advance();                                       // name
    // Two independent contextual markers on the rule header (Layer D, Phase 4;
    // `generates` added by the bindgen metaprog scope design):
    //   `rewrites body of <bind>` (§2.1) — this rule REPLACES the named bind's body,
    //                                      splicing the original back in via `$body`
    //   `generates body of <bind>`       — sibling: REPLACES the body and DISCARDS the
    //                                      original; `$body` is unavailable (M36)
    //   `reentrant` (§4)                 — this rule may re-trigger on generated code
    // `rewrites` and `generates` are mutually exclusive; either may combine with
    // `reentrant` in either order (exotic but legal).
    for (int i = 0; i < 2 && at(TokenKind::Identifier); ++i) {
        if ((cur().text == "rewrites" || cur().text == "generates") &&
            !r->ruleRewrites && !r->ruleGenerates) {
            bool generates = cur().text == "generates";
            if (generates) r->ruleGenerates = true; else r->ruleRewrites = true;
            advance();                               // 'rewrites' | 'generates'
            if (at(TokenKind::Identifier) && cur().text == "body") advance();
            else error(generates ? "'body' after 'generates'" : "'body' after 'rewrites'");
            if (at(TokenKind::Identifier) && cur().text == "of") advance();
            else error(generates ? "'of' after 'generates body'" : "'of' after 'rewrites body'");
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

    bool bodyRewriteHeader = r->ruleRewrites || r->ruleGenerates;
    while (at(TokenKind::KwInject) ||
           (at(TokenKind::Identifier) && cur().text == "replace")) {
        RuleAction a;
        parseRuleAction(a, r->ruleGenerates);
        // M30 (§2.1): a rule is additive (`inject`) XOR a rewriter (`replace`),
        // never both. `replace` demands a `rewrites`/`generates` header; `inject`
        // forbids one.
        bool isReplace = a.anchor == AnchorKind::BodyReplace ||
                          a.anchor == AnchorKind::BodyGenerate;
        if (isReplace && !bodyRewriteHeader)
            sink_.error(a.span, "'replace' is only legal in a 'rewrites' or "
                        "'generates' rule (add 'rewrites body of <bind>' or "
                        "'generates body of <bind>' to the rule header)");
        else if (!isReplace && bodyRewriteHeader)
            sink_.error(a.span, "a 'rewrites'/'generates' rule uses 'replace', not "
                        "'inject' (a rule is additive or a rewriter, never both)");
        r->ruleActions.push_back(std::move(a));
    }
    if (r->ruleActions.empty())
        error(bodyRewriteHeader ? "at least one 'replace' clause"
                                : "at least one 'inject' clause");

    expect(TokenKind::RBrace, "'}'");
    return r;
}

// Template form:   `macro Name(params) => \`expr\`;`
// Procedural form: `macro Name(string payload) comptime <body>` (F4).
// Both are namespace-level contextual declarations detached by RuleEngine.
StmtPtr Parser::parseMacroDecl(Access access) {
    sawMeta_ = true;
    auto r = parserDetail::mkStmt(StmtKind::Rule, cur().span);
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

// inject `template` at <anchor>   |   replace `template`   (Layer D, Phase 4 §2.1;
// `generates` sibling added by the bindgen metaprog scope design)
void Parser::parseRuleAction(RuleAction& out, bool generates) {
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
    // <bind>` / `generates body of <bind>`. Its template is a statement fragment
    // (`$body` splices the original body for `rewrites`; unavailable for
    // `generates`, M36). Set the anchor and parse; the header records the target
    // bind.
    if (isReplace) {
        out.anchor = generates ? AnchorKind::BodyGenerate : AnchorKind::BodyReplace;
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
    } else if (at(TokenKind::Identifier) && cur().text == "splice") {
        // `at splice Name [multi]` (named-anchor design §2.2): land at the
        // program-global `@Name();` site(s). `multi` opts into fanning out across
        // every same-named site; without it, ≥2 sites is a rule-stage error (M42).
        advance();                                       // 'splice'
        if (at(TokenKind::Identifier)) { out.target = cur().text; advance(); }
        else error("a splice-site name after 'splice'");
        if (at(TokenKind::Identifier) && cur().text == "multi") {
            out.spliceMulti = true;
            advance();
        }
        out.anchor = AnchorKind::SpliceSite;
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
