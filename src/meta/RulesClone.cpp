#include "meta/RulesInternal.hpp"

// =============================================================================
//  RulesClone — the hygienic AST cloner (§5.5, §7.1)
// =============================================================================
//
// refactor_1 session 04: everything that clones a rule/macro template into
// use-site code — hole substitution ($C/$m/$_params/$_args/$for/$if/$body),
// hygiene (alpha-renaming template-declared locals), and definition-site
// qualification (§10). RulesExpand.cpp drives WHEN a clone happens (matching
// + anchor placement); this file is entirely about WHAT the clone produces.

// M22, static: is this token text a `$name` hole? Was a file-static in
// Rules.cpp; every caller ended up here, so it stays a plain file-static.
static bool isHole(std::string_view t) { return !t.empty() && t[0] == '$'; }
// A composite identifier carries an embedded `$` but does NOT start with one
// (`copy_$f`) — spliced at clone time, unlike a bare hole `$f` (techdesign-
// splices-positions §2.2).
static bool hasHole(std::string_view t) {
    return t.find('$') != std::string_view::npos;
}
static bool isIdentContChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
// A concatenated/synthesized name is a legal identifier: non-empty, a non-digit
// first char, ident-continue thereafter (matches the lexer's ident grammar).
static bool isLegalIdent(std::string_view s) {
    if (s.empty()) return false;
    if (s[0] >= '0' && s[0] <= '9') return false;
    for (char c : s) if (!isIdentContChar(c)) return false;
    return true;
}

// Collect names the template declares as locals (Var statements), assigning
// each a fresh gensym in renames_ — hygiene by alpha-renaming (§7.1).
void RuleEngine::collectTemplateLocals(const Stmt* s) {
    if (!s) return;
    // A composite identifier (`copy_$f`) carries a `$` but is not a bare hole —
    // it is author-chosen and spliced at clone time, so it is NOT a renameable
    // template local (techdesign-splices-positions §2.4).
    if (s->kind == StmtKind::Var && !s->name.empty() && !isHole(s->name) &&
        !hasHole(s->name) && !renames_.count(s->name)) {
        synthNames_.push_back("__r" + std::to_string(gensymCounter_++) + "_" +
                              std::string(s->name));
        renames_[s->name] = synthNames_.back();
    }
    collectTemplateLocals(s->memberBody.get());
    collectTemplateLocals(s->thenBranch.get());
    collectTemplateLocals(s->elseBranch.get());
    collectTemplateLocals(s->forInit.get());
    for (const StmtPtr& x : s->body) collectTemplateLocals(x.get());
    for (const CatchClause& c : s->catches) collectTemplateLocals(c.body.get());
}

// --- clone + hole substitution (§5.5) ----------------------------------------
std::string_view RuleEngine::declRefName(const Binding& bnd) const {
    if (bnd.declStmt) return bnd.selectorText.empty() ? bnd.declStmt->name
                                                      : bnd.selectorText;
    return {};
}

// --- splice positions: identifier synthesis (techdesign-splices-positions §2.2/§3)
bool RuleEngine::holeNameString(std::string_view hole, const Bindings& b,
                                SourceSpan span, std::string& out, bool& err) {
    auto it = b.find(hole.substr(1));
    if (it == b.end()) {
        sink_.error(span, "'" + std::string(hole) + "' is not bound by this rule");
        err = true; return false;
    }
    const Binding& bn = it->second;
    if (bn.declStmt) { out = std::string(declRefName(bn)); return true; }
    if (bn.hasVal) {
        const Value& v = bn.val;
        if (v.kind == VKind::Object && v.obj) {
            auto nit = v.obj->fields.find("name");
            if (nit != v.obj->fields.end() && nit->second.kind == VKind::String) {
                out = nit->second.s; return true;
            }
            sink_.error(span, "'" + std::string(hole) + "' has no 'name' field to "
                        "form an identifier");
            err = true; return false;
        }
        if (v.kind == VKind::String) { out = v.s; return true; }
        out = valueToString(v);   // int/bool/float loop var -> `local_$idx`
        return true;
    }
    sink_.error(span, "'" + std::string(hole) + "' is an attribute value, not "
                "usable inside an identifier");
    err = true; return false;
}

std::string_view RuleEngine::spliceCompositeName(std::string_view name,
                                                 const Bindings& b, SourceSpan span,
                                                 bool& err) {
    std::string acc;
    size_t i = 0;
    while (i < name.size()) {
        if (name[i] == '$') {
            size_t j = i + 1;
            while (j < name.size() && isIdentContChar(name[j])) ++j;
            std::string piece;
            if (!holeNameString(name.substr(i, j - i), b, span, piece, err))
                return name;
            acc += piece;
            i = j;
        } else {
            size_t j = i;
            while (j < name.size() && name[j] != '$') ++j;
            acc.append(name.data() + i, j - i);
            i = j;
        }
    }
    if (!isLegalIdent(acc)) {
        sink_.error(span, "composite name '" + std::string(name) + "' produced '" +
                    acc + "', not a legal identifier");   // M38 shape
        err = true; return name;
    }
    return own(std::move(acc));
}

std::string_view RuleEngine::synthIdentName(const Stmt* s, Bindings& b,
                                            SourceSpan span, bool& err) {
    std::unordered_map<std::string, Value> locals = materializeBindings(b);
    std::string acc;
    for (size_t k = 0; k < s->nameSynthArgs.size(); ++k) {
        std::string evErr; bool failed = false;
        Value v = evalComptimeAt(s->nameSynthArgs[k].get(), locals, evErr, failed);
        if (failed) {
            sink_.error(s->nameSynthArgs[k]->span, "$ident argument " +
                        std::to_string(k + 1) + " failed to evaluate: " + evErr);  // M38
            err = true; return {};
        }
        if (v.kind != VKind::String) {
            sink_.error(s->nameSynthArgs[k]->span, "$ident argument " +
                        std::to_string(k + 1) + " is not a comptime string (got '" +
                        valueToString(v) + "')");   // M38
            err = true; return {};
        }
        acc += v.s;
    }
    if (!isLegalIdent(acc)) {
        sink_.error(span, "$ident produced '" + acc + "', not a legal identifier");  // M38
        err = true; return {};
    }
    return own(std::move(acc));
}

std::string RuleEngine::qualRuleName(const OwnedRule& r) const {
    return r.ns == "<root>" ? std::string(r.node->name)
                            : r.ns + "::" + std::string(r.node->name);
}

std::string RuleEngine::declNamespace(const DeclInfo& di) const {
    // ancestors run innermost -> outermost; namespaces reversed give the path.
    std::vector<std::string_view> parts;
    for (const Ancestor& a : di.ancestors)
        if (a.kindWord == "namespace") parts.push_back(a.stmt->name);
    if (parts.empty()) return "<root>";
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += "::";
        out += std::string(*it);
    }
    return out;
}

void RuleEngine::checkSynthCollision(Stmt* decl, const std::string& ns,
                                     const OwnedRule& r) {
    std::string simple(decl->name);
    std::string key = ns + "::" + simple;
    std::string rn = qualRuleName(r);
    auto report = [&](SourceSpan other) {
        LineCol lc = lineColAt(file_.text, other.offset);
        sink_.error(decl->span, "rule '" + rn + "' synthesizes declaration '" + simple +
                    "' but a declaration '" + simple + "' already exists at " +
                    std::to_string(lc.line) + ":" + std::to_string(lc.col) +
                    " — synthesized names must be unique");   // M37
    };
    auto sit = synthDeclNames_.find(key);
    if (sit != synthDeclNames_.end()) { report(sit->second); return; }
    for (const DeclInfo& di : decls_) {
        if (di.decl == decl) continue;
        if ((di.kindWord == "class" || di.kindWord == "struct" ||
             di.kindWord == "interface") &&
            di.decl->name == simple && declNamespace(di) == ns) {
            report(di.decl->span); return;
        }
    }
    synthDeclNames_[key] = decl->span;
}

// --- definition-site qualification (§10) -------------------------------------
bool RuleEngine::qualifyDefSite(std::string_view name, std::string& outNs) const {
    if (curRuleNs_ == "<root>") return false;
    Scope* sc = namespaceScope(curRuleNs_);
    if (!sc) return false;
    if (const std::vector<Symbol*>* v = sc->localLookup(name))
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Function || s->kind == SymbolKind::Class ||
                s->kind == SymbolKind::Var) {
                outNs = curRuleNs_;
                return true;
            }
    return false;
}

// --- $_params / $_args (§6) --------------------------------------------------
// Both resolve against the subject specifically, via the internal "$subject"
// sentinel binding tryMatch always sets alongside the subject's real bind name.
Stmt* RuleEngine::subjectOf(const Bindings& b) const {
    auto it = b.find(std::string_view("$subject"));
    return it != b.end() ? it->second.declStmt : nullptr;
}

void RuleEngine::cloneParamList(const std::vector<Param>& params, Bindings& b,
                                bool& err, std::vector<Param>& out) {
    for (const Param& p : params) {
        if (p.name != "$_params") { out.push_back(cloneParam(p, b, err)); continue; }
        Stmt* subj = subjectOf(b);
        if (!subj || !subj->callable) {
            sink_.error(p.span, "'$_params' needs a callable subject; '" +
                        std::string(subj ? subj->name : std::string_view("?")) +
                        "' is a " + rulesDetail::declKindWord(subj));
            err = true;
            continue;
        }
        // Types+names of the subject's OWN params, cloned fresh — the names
        // are NOT gensym-renamed (they must line up with $_args, and they land
        // in a brand-new lambda/member scope, so capture is structurally
        // impossible regardless).
        for (const Param& sp : subj->params) out.push_back(cloneParam(sp, b, err));
    }
}

void RuleEngine::cloneArgList(const std::vector<ExprPtr>& args, Bindings& b,
                              bool& err, std::vector<ExprPtr>& out) {
    for (const ExprPtr& x : args) {
        if (x->kind != ExprKind::Name || x->text != "$_args") {
            out.push_back(cloneExpr(x.get(), b, err));
            continue;
        }
        Stmt* subj = subjectOf(b);
        if (!subj || !subj->callable) {
            sink_.error(x->span, "'$_args' needs a callable subject; '" +
                        std::string(subj ? subj->name : std::string_view("?")) +
                        "' is a " + rulesDetail::declKindWord(subj));
            err = true;
            continue;
        }
        for (const Param& sp : subj->params) {
            auto nm = std::make_unique<Expr>(ExprKind::Name);
            nm->span = x->span;
            nm->text = sp.name;
            out.push_back(std::move(nm));
        }
    }
}

// Statement-position `$for` (item J): the statement/decl/member analogue of
// cloneArrayElements. A StmtKind::ForSplice repeats its body (thenBranch) once
// per iterated item — same evaluation contract as the array form (iterator via
// materializeBindings, M21 on a non-array, per-item extended bindings). A body
// that is a `{ }` block splices its statements flat (so several statements can
// repeat together); any other body clones as one. Non-ForSplice statements
// clone 1:1.
// B2 (techdesign-splices-conditional §3.2): a `$if` condition is an ordinary
// comptime predicate over the firing's bindings — the same env `where` and
// item-level `comptime if` evaluate against (materializeBindings + evalComptimeAt).
bool RuleEngine::evalForkCond(Expr* cond, Bindings& b, bool& taken, bool& err) {
    std::unordered_map<std::string, Value> locals = materializeBindings(b);
    std::string evErr; bool failed = false;
    Value v = evalComptimeAt(cond, locals, evErr, failed);
    if (failed) {
        // Same wording as the where-clause failure (Rules.cpp §5), keyed to $if —
        // a runaway/erroring predicate is caught by the existing step budget.
        sink_.error(cond->span, "rule '" + curRuleName_ +
                    "' $if-condition evaluation failed: " + evErr);
        err = true; return false;
    }
    if (v.kind != VKind::Bool) {
        sink_.error(cond->span, "rule '" + curRuleName_ +          // M40
                    "' $if-condition must be bool (got '" + valueToString(v) + "')");
        err = true; return false;
    }
    taken = v.b; return true;
}

void RuleEngine::cloneStmtInto(const Stmt* s, Bindings& b, bool& err,
                               std::vector<StmtPtr>& out) {
    if (!s) return;
    if (s->kind == StmtKind::ForkSplice) {
        // B2: evaluate the predicate, then splice ONLY the taken branch's
        // fragment flat into `out` — cloning it through cloneStmtInto so its own
        // holes, `$for`s, and nested `$if`s (`$else if` lands here as a
        // ForkSplice elseBranch) all expand. The untaken branch is never cloned
        // (template stays a template). A false `$if` with no `$else` emits nothing.
        bool taken = false;
        if (!evalForkCond(s->expr.get(), b, taken, err)) return;
        const Stmt* branch = taken ? s->thenBranch.get() : s->elseBranch.get();
        if (!branch) return;
        if (branch->kind == StmtKind::Block)
            for (const StmtPtr& bs : branch->body) cloneStmtInto(bs.get(), b, err, out);
        else
            cloneStmtInto(branch, b, err, out);
        return;
    }
    if (s->kind != StmtKind::ForSplice) {
        StmtPtr c = cloneStmt(s, b, err);
        if (c) out.push_back(std::move(c));
        return;
    }
    std::unordered_map<std::string, Value> locals = materializeBindings(b);
    std::string evErr; bool failed = false;
    Value iter = evalComptimeAt(s->expr.get(), locals, evErr, failed);
    if (failed) {
        sink_.error(s->expr->span, "'$for' iterator evaluation failed: " + evErr);
        err = true; return;
    }
    if (iter.kind != VKind::Array) {
        sink_.error(s->expr->span, "'$for' iterator didn't yield an array (got '" +
                    valueToString(iter) + "')");   // M21
        err = true; return;
    }
    if (!iter.arr) return;                          // empty: nothing to repeat
    for (const Value& item : *iter.arr) {
        Bindings inner = b;                          // extend, not mutate
        Binding ib; ib.hasVal = true; ib.val = item;
        inner[s->name] = ib;
        const Stmt* body = s->thenBranch.get();
        if (body && body->kind == StmtKind::Block)
            for (const StmtPtr& bs : body->body) cloneStmtInto(bs.get(), inner, err, out);
        else
            cloneStmtInto(body, inner, err, out);    // recursion also handles nested $for
    }
}

// Clone one array-literal element into `out`, folding an expression-position
// ForkSplice (`$if`) to its taken leaf (B2). Following nested ForkSplice in `c`
// resolves the whole `$else if` chain; a false tail with no `$else` contributes
// no element. A plain element clones 1:1. Shared by cloneArrayElements' top
// level and its `$for` per-item body, so `$for p : $if (…) {…} $else {…}` folds.
void RuleEngine::cloneArrayElementInto(const Expr* el, Bindings& b, bool& err,
                                       std::vector<ExprPtr>& out) {
    const Expr* cur = el;
    while (cur && cur->kind == ExprKind::ForkSplice) {
        bool taken = false;
        if (!evalForkCond(cur->a.get(), b, taken, err)) return;
        cur = taken ? cur->b.get() : cur->c.get();
    }
    if (!cur) return;
    ExprPtr cloned = cloneExpr(cur, b, err);
    if (cloned) out.push_back(std::move(cloned));
}

void RuleEngine::cloneArrayElements(const std::vector<ExprPtr>& elems, Bindings& b,
                                    bool& err, std::vector<ExprPtr>& out) {
    for (const ExprPtr& el : elems) {
        if (el->kind == ExprKind::ForkSplice) {
            cloneArrayElementInto(el.get(), b, err, out);
            continue;
        }
        if (el->kind != ExprKind::ForSplice) {
            out.push_back(cloneExpr(el.get(), b, err));
            continue;
        }
        // The iterator is ordinary comptime code, evaluated with the current
        // bindings as locals (materializeBindings, same env `where` uses) —
        // full stdlib, so `$for` needs no filter clause of its own (the
        // language's own `.where(...)` is the filter, per the ORM example).
        std::unordered_map<std::string, Value> locals = materializeBindings(b);
        std::string evErr; bool failed = false;
        Value iter = evalComptimeAt(el->a.get(), locals, evErr, failed);
        if (failed) {
            sink_.error(el->a->span, "'$for' iterator evaluation failed: " + evErr);
            err = true;
            continue;
        }
        if (iter.kind != VKind::Array) {
            sink_.error(el->a->span, "'$for' iterator didn't yield an array (got '" +
                        valueToString(iter) + "')");   // M21
            err = true;
            continue;
        }
        if (!iter.arr) continue;            // empty array: no elements to splice
        for (const Value& item : *iter.arr) {
            Bindings inner = b;             // extend, not mutate — nested $for sees both
            Binding ib; ib.hasVal = true; ib.val = item;
            inner[el->text] = ib;
            cloneArrayElementInto(el->b.get(), inner, err, out);   // element may be a `$if`
        }
    }
}

// --- $body (Layer D, §2.2) ---------------------------------------------------
// The original body's single value expression, for `$body` in expression
// position: an arrow body (`=> e`, a lone Return) or a block that is exactly
// `{ return e; }`. Any other shape yields null (caller raises M31).
const Expr* RuleEngine::singleValueBodyExpr(const Stmt* body) const {
    if (!body) return nullptr;
    const Stmt* inner = body;
    if (body->kind == StmtKind::Block) {
        if (body->body.size() != 1) return nullptr;
        inner = body->body[0].get();
    }
    if (inner && inner->kind == StmtKind::Return && inner->expr) return inner->expr.get();
    return nullptr;
}

// Clone a template statement list, splicing the ORIGINAL body's statements in
// place of any bare `$body;` statement (§2.2 statement form). The spliced
// statements are cloned VERBATIM — they move as one authored unit, so their
// own locals are neither hygiene-renamed nor def-site-qualified (§2.2). Outside
// a `replace` expansion (replaceBodyOrig_ null) this is a plain per-item clone.
void RuleEngine::cloneStmtListBody(const std::vector<StmtPtr>& in, Bindings& b,
                                   bool& err, std::vector<StmtPtr>& out) {
    for (const StmtPtr& s : in) {
        if (replaceBodyOrig_ && !verbatimClone_ && s && s->kind == StmtKind::ExprStmt &&
            s->expr && s->expr->kind == ExprKind::Name && s->expr->text == "$body") {
            bool saved = verbatimClone_;
            verbatimClone_ = true;
            for (const StmtPtr& orig : replaceBodyOrig_->body) {
                StmtPtr c = cloneStmt(orig.get(), b, err);
                if (c) out.push_back(std::move(c));
            }
            verbatimClone_ = saved;
            continue;
        }
        // cloneStmtInto (not cloneStmt) so a statement-position `$for` nested in
        // this block/body — e.g. inside a generated method — expands to one clone
        // per item (item J). Plain statements still clone 1:1.
        cloneStmtInto(s.get(), b, err, out);
    }
}

ExprPtr RuleEngine::cloneExpr(const Expr* e, Bindings& b, bool& err) {
    if (!e) return nullptr;

    // $body in expression position (Layer D, §2.2): the original body's single
    // value expression, cloned verbatim. Valid only when the body IS a single
    // value expression; otherwise M31 (a statement block can't be an expr).
    if (replaceBodyOrig_ && !verbatimClone_ &&
        e->kind == ExprKind::Name && e->text == "$body") {
        const Expr* val = singleValueBodyExpr(replaceBodyOrig_);
        if (!val) {
            sink_.error(e->span, "'$body' is used as an expression, but the "
                        "method's body is a statement block, not a single value "
                        "expression — capture it in statement position, or give "
                        "the method an arrow body");   // M31
            err = true;
            return nullptr;
        }
        bool saved = verbatimClone_;
        verbatimClone_ = true;
        ExprPtr out = cloneExpr(val, b, err);
        verbatimClone_ = saved;
        return out;
    }

    // Value-hole with a field access: `$r.method` — read the attr field, reify.
    if (e->kind == ExprKind::Member && e->a && e->a->kind == ExprKind::Name &&
        isHole(e->a->text)) {
        auto it = b.find(e->a->text.substr(1));
        // `$C.name` / `$m.name` on a DECL binding (a matched class/method/...,
        // not a `$for`-bound meta value): reify the decl's simple name as a
        // string literal. The ergonomic way to name a matched declaration in
        // generated code (e.g. a registry keyed by class/method name) without
        // the `[ $for x in [C] : $x.name ][0]` re-binding dance.
        if (it != b.end() && it->second.declStmt && !it->second.hasVal &&
            e->text == "name") {
            ExprPtr lit;
            if (reify(vstr(std::string(declRefName(it->second))), e->span, lit))
                return lit;
        }
        if (it != b.end() && it->second.hasVal &&
            it->second.val.kind == VKind::Object && it->second.val.obj) {
            auto fit = it->second.val.obj->fields.find(std::string(e->text));
            if (fit != it->second.val.obj->fields.end()) {
                ExprPtr lit;
                if (reify(fit->second, e->span, lit)) return lit;
                sink_.error(e->span, "attribute field '" + std::string(e->text) +
                            "' is not reifiable"); err = true; return nullptr;
            }
            sink_.error(e->span, "no field '" + std::string(e->text) +
                        "' on this attribute"); err = true; return nullptr;
        }
    }

    // Macro-parameter splice (§7): bare `$param` -> a full clone of the
    // argument subtree bound at the call site. Exclusively an exprNode-shaped
    // Binding (never set alongside declStmt/hasVal, so this can't misfire on
    // an ordinary rule/attribute hole). Checked before the generic Name-hole
    // path below so `$e.foo` also works: this fires when cloning `e->a`
    // (the bare `$e`), and the Member wrapping it is built normally around
    // whatever comes back.
    if (e->kind == ExprKind::Name && isHole(e->text)) {
        auto it = b.find(e->text.substr(1));
        if (it != b.end() && it->second.exprNode) return cloneExpr(it->second.exprNode, b, err);
    }

    auto out = std::make_unique<Expr>(e->kind);
    out->span = e->span;
    out->text = e->text;
    out->op = e->op; out->colon = e->colon; out->optChain = e->optChain;
    out->isMacroCall = e->isMacroCall;
    out->isComptime = e->isComptime;
    out->argLabel = e->argLabel;
    // F4: a StringLit's isRawSegment flag says whether `text` is bare content
    // (no quotes — an interpolation segment) or a full raw token (needs
    // quote-stripping). A quasiquote template's own nested string literals
    // (e.g. `` `log = log + "one;";` ``) go through the SAME parser as any
    // other source, so they can carry this flag too — losing it here made a
    // cloned segment's already-bare text run through decodeStringLiteral's
    // quote-stripping a SECOND time, silently eating its first/last byte
    // (confirmed: "one;" -> "ne", reproduced via rule_marker.ext).
    out->isRawSegment = e->isRawSegment;
    out->isQuasiPayload = e->isQuasiPayload;
    out->isRawString = e->isRawString;
    out->singleQuoted = e->singleQuoted;
    out->charLit = e->charLit;

    // Decl-hole in member-name position: `this.$m` -> splice the selector.
    if (e->kind == ExprKind::Member && isHole(e->text)) {
        auto it = b.find(e->text.substr(1));
        if (it == b.end()) {
            sink_.error(e->span, "'$" + std::string(e->text.substr(1)) +
                        "' is not bound by this rule"); err = true;
        } else if (it->second.declStmt) {
            out->text = declRefName(it->second);
        } else if (it->second.hasVal && it->second.val.kind == VKind::Object &&
                   it->second.val.obj) {
            // A `$for`-bound `meta.*` value (a `meta::Method`/`meta::Field`
            // object, e.g. `$for m in C.methods : ... t.$m() ...`) splices its
            // `name` field as the member selector — `t.$m()` calls the method
            // that `m` names. Without this, a $for loop could name a member's
            // NAME as a string (`$m.name`) but never USE it as a selector, so a
            // rule that both discovers and invokes members (a test runner's
            // per-method dispatch) was impossible. The name is interned so the
            // spliced string_view outlives this binding.
            auto nit = it->second.val.obj->fields.find("name");
            if (nit != it->second.val.obj->fields.end() &&
                nit->second.kind == VKind::String) {
                out->text = own(nit->second.s);
            } else {
                sink_.error(e->span, "'$" + std::string(e->text.substr(1)) +
                            "' is not a member name (no reifiable 'name' field)");
                err = true;
            }
        } else {
            sink_.error(e->span, "'$" + std::string(e->text.substr(1)) +
                        "' is an attribute value, not a member name"); err = true;
        }
    }

    // Name-hole: `$C` / bare `$m` -> splice the decl's name. Class holes also
    // retain declaration identity for pass-2 call resolution: source spelling
    // alone cannot qualify a root class away from a same-named generated
    // function (`C C() => C()`), but hygiene still requires the hole to denote
    // the matched class rather than capture the new function (bug.md #22).
    // `$for`'s loop var (§5) also lands here for a bare reference: a
    // primitive/array item reifies directly; an Object item (e.g. a `$for`
    // over meta.* fields) still needs a field splice, same as an attribute.
    if (e->kind == ExprKind::Name && isHole(e->text)) {
        auto it = b.find(e->text.substr(1));
        if (it == b.end()) {
            sink_.error(e->span, "'$" + std::string(e->text.substr(1)) +
                        "' is not bound by this rule"); err = true;
        } else if (it->second.declStmt) {
            out->text = declRefName(it->second);
            if (it->second.declStmt->kind == StmtKind::Class)
                out->hygienicDecl = it->second.declStmt;
        } else if (it->second.hasVal && it->second.val.kind != VKind::Object) {
            ExprPtr lit;
            if (reify(it->second.val, e->span, lit)) return lit;
            sink_.error(e->span, "'$" + std::string(e->text.substr(1)) +
                        "' is not reifiable (got '" +
                        valueToString(it->second.val) + "')"); err = true;
        } else {
            sink_.error(e->span, "'$" + std::string(e->text.substr(1)) +
                        "' is an attribute value; splice a field (e.g. $" +
                        std::string(e->text.substr(1)) + ".name)"); err = true;
        }
    } else if (e->kind == ExprKind::Name && hasHole(e->text)) {
        // B1: a REFERENCE to a composite-named local (`copy_$f`) — resolve it the
        // same way its declaration was (techdesign-splices-positions §2.2), so the
        // two agree. Author-chosen, so rename-exempt (never routed through
        // renames_/def-site qualification).
        out->text = spliceCompositeName(e->text, b, e->span, err);
    } else if (e->kind == ExprKind::Name && !verbatimClone_) {
        // Hygiene: a reference to a template-declared local -> its fresh name.
        // Skipped under verbatimClone_ (§2.2): the original body being spliced
        // back by `$body` keeps its own names — no rename, no def-site qualify.
        auto rit = renames_.find(e->text);
        if (rit != renames_.end()) {
            out->text = rit->second;
        } else {
            // §10: a free name (not a hole, not a template-local, and — since
            // `this` is ExprKind::This, a structurally different node kind —
            // never `this`) that resolves in the rule's/macro's OWN declaring
            // namespace qualifies to NS::name. A name that doesn't resolve
            // there stays bare and resolves at the injection site instead
            // (the deliberate channel for `this`-relative members and
            // prelude names). A bare Name has no children, so qualifying
            // rewrites `out` into a Member in place and returns immediately.
            std::string qualNs;
            if (qualifyDefSite(e->text, qualNs)) {
                // #91: qualNs may be a nested path ("Atlantis::Orm") — build
                // the left side as a chained Member (Atlantis then ::Orm …),
                // never a single Name token containing "::".
                ExprPtr left;
                size_t pos = 0;
                while (pos <= qualNs.size()) {
                    size_t sep = qualNs.find("::", pos);
                    std::string seg = qualNs.substr(
                        pos, sep == std::string::npos ? std::string::npos : sep - pos);
                    if (!left) {
                        left = std::make_unique<Expr>(ExprKind::Name);
                        left->span = e->span;
                        left->text = own(seg);
                    } else {
                        auto m = std::make_unique<Expr>(ExprKind::Member);
                        m->span = e->span;
                        m->colon = true;
                        m->text = own(seg);
                        m->a = std::move(left);
                        left = std::move(m);
                    }
                    if (sep == std::string::npos) break;
                    pos = sep + 2;
                }
                out->kind = ExprKind::Member;
                out->colon = true;
                out->text = e->text;
                out->a = std::move(left);
                return out;
            }
        }
    }

    out->a = cloneExpr(e->a.get(), b, err);
    out->b = cloneExpr(e->b.get(), b, err);
    out->c = cloneExpr(e->c.get(), b, err);
    for (const TypeRefPtr& t : e->explicitTypeArgs)
        out->explicitTypeArgs.push_back(cloneType(t.get(), b, err));
    // $_args (§6) is legal only as a call-argument element; anywhere else a
    // literal "$_args" falls through to the generic unbound-hole error below.
    if (e->kind == ExprKind::Call) cloneArgList(e->list, b, err, out->list);
    else if (e->kind == ExprKind::Array) cloneArrayElements(e->list, b, err, out->list);
    else for (const ExprPtr& x : e->list) out->list.push_back(cloneExpr(x.get(), b, err));
    cloneParamList(e->params, b, err, out->params);   // $_params (§6), lambda side
    if (e->block) out->block = cloneStmt(e->block.get(), b, err);
    out->type = cloneType(e->type.get(), b, err);

    // Make constructor intent survive `--expand` round-trips, which cannot
    // serialize the in-memory hygienicDecl pointer. `$C()` becomes the ordinary
    // explicit constructor-label spelling `C::C()`: the first C names the
    // matched class and the second selects its default label. This remains
    // unambiguous even inside a generated function also named C.
    if (out->kind == ExprKind::Call && out->a &&
        out->a->kind == ExprKind::Name && out->a->hygienicDecl &&
        out->a->hygienicDecl->kind == StmtKind::Class) {
        auto qualified = std::make_unique<Expr>(ExprKind::Member);
        qualified->span = out->a->span;
        qualified->a = std::move(out->a);
        qualified->text = qualified->a->text;
        qualified->colon = true;
        out->a = std::move(qualified);
    }
    for (const MatchArm& arm : e->arms) {
        MatchArm a2;
        a2.isElse = arm.isElse; a2.span = arm.span;
        a2.type = cloneType(arm.type.get(), b, err);
        a2.value = cloneExpr(arm.value.get(), b, err);
        a2.bodyExpr = cloneExpr(arm.bodyExpr.get(), b, err);
        a2.bodyBlock = cloneStmt(arm.bodyBlock.get(), b, err);
        out->arms.push_back(std::move(a2));
    }
    return out;
}

TypeRefPtr RuleEngine::cloneType(const TypeRef* t, Bindings& b, bool& err) {
    if (!t) return nullptr;
    auto out = std::make_unique<TypeRef>(t->kind);
    out->span = t->span;
    out->path = t->path;
    out->name = t->name;
    // B1 dotted-hole type (techdesign-splices-positions §2): `$f.type` / `$p.name`
    // in type position. Read the bound meta value's canonical-string field
    // (`type` or `name`) and splice it as this type's name — rename-exempt
    // (cloneType never renames), holeBind/holeField deliberately NOT carried onto
    // the clone so it prints and resolves as an ordinary named type. Only
    // `.type`/`.name` are legal.
    if (!t->holeField.empty()) {
        std::string_view field = t->holeField;
        auto it = b.find(t->holeBind.substr(1));
        if ((field == "type" || field == "name") && it != b.end() &&
            it->second.hasVal && it->second.val.kind == VKind::Object &&
            it->second.val.obj) {
            auto fit = it->second.val.obj->fields.find(std::string(field));
            if (fit != it->second.val.obj->fields.end() &&
                fit->second.kind == VKind::String && !fit->second.s.empty()) {
                out->name = own(fit->second.s);
                return out;
            }
        }
        sink_.error(t->span, "'" + std::string(t->holeBind) + "." +
                    std::string(field) + "' has no reifiable type/name field "
                    "usable as a type");   // M39
        err = true;
        return out;
    }
    // Type-position hole: `$C` as a type name -> splice the bound decl's name,
    // AND retain its resolved symbol so pass-2 resolves to the class directly.
    // Without the symbol, a `$C t = ...` type inside an injection whose own
    // anchor global is ALSO named `$C` (the `bool $C = seed(...)` registration
    // idiom) would re-resolve `MathTests` by name to that bool variable and
    // report "not a type" — the type-position analogue of bug.md #22's
    // call-position hygiene.
    if (isHole(t->name)) {
        auto it = b.find(t->name.substr(1));
        if (it != b.end() && it->second.declStmt) {
            out->name = declRefName(it->second);
            if (it->second.declSym) out->resolvedSymbol = it->second.declSym;
        }
    }
    for (const TypeRefPtr& g : t->generics) out->generics.push_back(cloneType(g.get(), b, err));
    for (const TypeRefPtr& mm : t->members) out->members.push_back(cloneType(mm.get(), b, err));
    for (const TypeRefPtr& fp : t->funcParams) out->funcParams.push_back(cloneType(fp.get(), b, err));
    out->funcRet = cloneType(t->funcRet.get(), b, err);
    return out;
}

Param RuleEngine::cloneParam(const Param& p, Bindings& b, bool& err) {
    Param out;
    out.name = p.name;
    out.span = p.span;
    out.isConst = p.isConst;
    out.defaultFolded = p.defaultFolded;
    out.type = cloneType(p.type.get(), b, err);
    out.defaultValue = cloneExpr(p.defaultValue.get(), b, err);
    return out;
}

StmtPtr RuleEngine::cloneStmt(const Stmt* s, Bindings& b, bool& err) {
    if (!s) return nullptr;
    auto out = std::make_unique<Stmt>(s->kind);
    out->span = s->span;
    out->access = s->access;
    out->name = s->name;
    out->isInterface = s->isInterface; out->isValue = s->isValue;
    out->isAttribute = s->isAttribute;
    out->isMacroDecl = s->isMacroDecl;
    out->isProceduralMacro = s->isProceduralMacro;
    // §4: carry attributes onto injected declarations (fresh, resolved/consumed
    // reset) so a reentrant rule can match an attribute a prior round emitted —
    // resolveNewAttrs resolves them next round. A no-op for the common template
    // with no attributes; ordinary (non-reentrant) injections are unaffected
    // because the rule stage only resolves such attrs inside the fixpoint.
    for (const AttrUse& a : s->attrs) {
        AttrUse a2;
        a2.path = a.path; a2.name = a.name; a2.span = a.span;
        for (const ExprPtr& arg : a.args) a2.args.push_back(cloneExpr(arg.get(), b, err));
        out->attrs.push_back(std::move(a2));
    }
    out->generics = s->generics;
    out->isCtor = s->isCtor; out->isGet = s->isGet; out->isSet = s->isSet;
    out->isMutating = s->isMutating; out->distinct = s->distinct;
    out->callable = s->callable; out->selector = s->selector;
    out->inferred = s->inferred; out->isComptime = s->isComptime;
    out->isConst = s->isConst;
    // techdesign-02 F3: without this, a `using` Var cloned through rule
    // injection silently drops its cleanup obligation — same class of bug as
    // isRawSegment (Track 01's log) this doc's own overview flagged for us.
    out->isUsing = s->isUsing; out->usingClose = s->usingClose;
    // techdesign-labeled-break-continue.md F2: copy the label so a rule
    // template's labeled loop/labeled break survives injection. Deliberately
    // do NOT copy labelTarget — it is always null here (the checker runs
    // after rule splicing) and a future "copy every field" sweep must not
    // "fix" this into copying a cross-node pointer that would dangle into
    // the template tree.
    out->label = s->label;
    // C: `$ident(a, …)` decl name — synthesize now (techdesign-splices-positions
    // §3). out->hasNameSynth stays set on the resolved clone so the
    // namespace-scope collision check (M37) can find it; it is cleared once
    // checked. Members funnel their M37 through injectMember's existing same-name
    // collision.
    if (s->hasNameSynth) {
        out->name = synthIdentName(s, b, out->span, err);
        out->selector.text = out->name;
        out->hasNameSynth = true;
    }
    // Member/field/method name may itself be a hole (`member of` templates).
    else if (isHole(out->name)) {
        auto it = b.find(out->name.substr(1));
        if (it != b.end() && it->second.declStmt) out->name = declRefName(it->second);
        // A `$for`-bound meta.* value (member/namespace-position `$for` that
        // GENERATES a decl named `$m`) splices its `name` field as the decl's
        // name — the item-position analogue of cloneExpr's `this.$m` selector
        // splice. Interned so the string_view outlives this binding.
        else if (it != b.end() && it->second.hasVal &&
                 it->second.val.kind == VKind::Object && it->second.val.obj) {
            auto nit = it->second.val.obj->fields.find("name");
            if (nit != it->second.val.obj->fields.end() &&
                nit->second.kind == VKind::String)
                out->name = own(nit->second.s);
        }
    }
    // B1: composite identifier `copy_$f` — author-chosen, rename-exempt.
    else if (hasHole(out->name)) {
        out->name = spliceCompositeName(out->name, b, out->span, err);
    } else if (s->kind == StmtKind::Var && !verbatimClone_) {
        // Hygiene: rename a template-declared local at its declaration site.
        // Skipped under verbatimClone_ (§2.2): the original body keeps its own
        // locals so `$body` moves as one authored unit.
        auto rit = renames_.find(out->name);
        if (rit != renames_.end()) out->name = rit->second;
    }
    if (isHole(out->selector.text)) {
        auto it = b.find(out->selector.text.substr(1));
        if (it != b.end() && it->second.declStmt) out->selector.text = declRefName(it->second);
        else if (it != b.end() && it->second.hasVal &&
                 it->second.val.kind == VKind::Object && it->second.val.obj) {
            auto nit = it->second.val.obj->fields.find("name");
            if (nit != it->second.val.obj->fields.end() &&
                nit->second.kind == VKind::String)
                out->selector.text = own(nit->second.s);
        }
    } else if (hasHole(out->selector.text) && !s->hasNameSynth) {
        out->selector.text = spliceCompositeName(out->selector.text, b, out->span, err);
    }
    out->type = cloneType(s->type.get(), b, err);
    cloneParamList(s->params, b, err, out->params);   // $_params (§6), member side
    out->memberBody = cloneStmt(s->memberBody.get(), b, err);
    out->init = cloneExpr(s->init.get(), b, err);
    out->expr = cloneExpr(s->expr.get(), b, err);
    // §2.2: a nested `$body;` statement inside a template block splices the
    // original body here too (cloneStmtListBody is a plain clone outside a
    // `replace` expansion, so ordinary rules/macros are unaffected).
    cloneStmtListBody(s->body, b, err, out->body);
    for (const TypeRefPtr& base : s->bases) out->bases.push_back(cloneType(base.get(), b, err));
    out->thenBranch = cloneStmt(s->thenBranch.get(), b, err);
    out->elseBranch = cloneStmt(s->elseBranch.get(), b, err);
    out->forInit = cloneStmt(s->forInit.get(), b, err);
    out->forStep = cloneExpr(s->forStep.get(), b, err);
    for (const CatchClause& c : s->catches) {
        CatchClause c2;
        c2.type = cloneType(c.type.get(), b, err);
        c2.name = c.name;
        c2.body = cloneStmt(c.body.get(), b, err);
        out->catches.push_back(std::move(c2));
    }
    return out;
}
