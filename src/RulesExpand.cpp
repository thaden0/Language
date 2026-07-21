#include "RulesInternal.hpp"
#include <algorithm>
#include <functional>

// =============================================================================
//  Layer B — rules: collect, index, match, expand (§5)
// =============================================================================
//
// refactor_1 session 04: declaration-level expansion — collect rules off the
// tree, index every matchable declaration + splice site, match rules against
// them, and expand a match into the tree (anchors, dangling-attribute
// warning, --rules report). The hygienic clone itself (cloneStmt/cloneExpr/
// cloneType and their hole-substitution machinery) lives in RulesClone.cpp;
// Phase 1 (comptime fold + attributes + meta.* reflection) stays in Rules.cpp.

namespace rulesDetail {

// Look up a class symbol by name in a namespace scope (for the :IFace check).
// Was a file-static in Rules.cpp; shared across TUs, so it lives here now —
// see RulesInternal.hpp.
Symbol* lookupClassIn(Scope* sc, std::string_view name) {
    if (!sc) return nullptr;
    if (const std::vector<Symbol*>* v = sc->localLookup(name))
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Class) return s;
    return nullptr;
}

// A short, human-readable word for a declaration's kind, for diagnostics
// (M25/M26/M27) — derived directly from the Stmt's own flags. Was a
// file-static in Rules.cpp; shared across TUs, so it lives here now — see
// RulesInternal.hpp.
const char* declKindWord(const Stmt* s) {
    if (!s) return "?";
    if (s->kind == StmtKind::Class)
        return s->isInterface ? "interface" : s->isValue ? "struct" : "class";
    if (s->kind == StmtKind::Namespace) return "namespace";
    if (s->kind == StmtKind::Member) {
        if (!s->callable) return "field";
        if (s->isCtor) return "constructor";
        if (s->isGet || s->isSet) return "accessor";
        return "method";
    }
    return "declaration";
}

}  // namespace rulesDetail

// --- 1. collection: detach rules from the tree (§5.1) ------------------------
void RuleEngine::collectRules(std::vector<StmtPtr>& items, const std::string& ns) {
    for (StmtPtr& item : items) {
        if (!item) continue;
        if (item->kind == StmtKind::Rule) {
            OwnedRule r;
            r.offset = item->span.offset;
            r.ns = ns;
            r.node = std::move(item);            // slot becomes null
            rules_.push_back(std::move(r));
        } else if (item->kind == StmtKind::Namespace) {
            // #91: keep the FULL nested path — imports_ (walkProvenance) and
            // namespaceScope both speak qualified names ("Atlantis::Orm").
            collectRules(item->body,
                         ns == "<root>" ? std::string(item->name)
                                        : ns + "::" + std::string(item->name));
        }
    }
    items.erase(std::remove(items.begin(), items.end(), nullptr), items.end());
}

// --- 2. index every matchable declaration + its ancestors --------------------
void RuleEngine::indexDecls(std::vector<StmtPtr>& items,
                            std::vector<Ancestor>& chain, const std::string& ns) {
    for (StmtPtr& item : items) {
        if (!item) continue;
        Stmt* s = item.get();
        if (s->kind == StmtKind::Namespace) {
            // #91: resolve the namespace symbol through the CURRENT path, not
            // the global scope, and recurse with the full qualified path.
            std::string full = ns == "<root>" ? std::string(s->name)
                                              : ns + "::" + std::string(s->name);
            Symbol* nsym = nullptr;
            Scope* parent = namespaceScope(ns);
            if (parent)
                if (const std::vector<Symbol*>* v = parent->localLookup(s->name))
                    for (Symbol* sy : *v)
                        if (sy->kind == SymbolKind::Namespace) { nsym = sy; break; }
            decls_.push_back({s, "namespace", fileIdxFor(s->span), chain, nsym});
            chain.push_back({"namespace", s, nsym});
            indexDecls(s->body, chain, full);
            chain.pop_back();
        } else if (s->kind == StmtKind::Class) {
            std::string_view kw = s->isAttribute ? "attribute"
                                : s->isInterface ? "interface"
                                : s->isValue     ? "struct" : "class";
            Symbol* csym = rulesDetail::lookupClassIn(namespaceScope(ns), s->name);
            decls_.push_back({s, kw, fileIdxFor(s->span), chain, csym});
            chain.push_back({kw, s, csym});
            indexDecls(s->body, chain, ns);      // members share the namespace
            chain.pop_back();
        } else if (s->kind == StmtKind::Member) {
            bool inClass = false;
            for (const Ancestor& a : chain)
                if (a.kindWord == "class" || a.kindWord == "struct" ||
                    a.kindWord == "interface") inClass = true;
            std::string_view kw = s->isCtor            ? "constructor"
                                : (s->isGet || s->isSet) ? "accessor"
                                : !s->callable         ? "field"
                                : inClass              ? "method" : "function";
            decls_.push_back({s, kw, fileIdxFor(s->span), chain});
        }
    }
}

// --- 2b. index every `@Name();` splice site (named-anchor design §2.3) --------
// Recursively gather every splice-site marker in a statement vector, recording
// its owning vector + node under the attribute name. Mirrors findMarkerSlot's
// recursion set (a splice site is a statement-position marker) so a site nested
// in an if/try block is found the same way `at marker` finds one.
void RuleEngine::collectSpliceSites(std::vector<StmtPtr>& vec) {
    for (size_t i = 0; i < vec.size(); ++i) {
        Stmt* s = vec[i].get();
        if (s->kind == StmtKind::Empty && s->isSpliceSite && !s->name.empty())
            spliceSites_[s->name].push_back({&vec, s});
        if (s->kind == StmtKind::Block) collectSpliceSites(s->body);
        if (s->thenBranch && s->thenBranch->kind == StmtKind::Block)
            collectSpliceSites(s->thenBranch->body);
        if (s->elseBranch && s->elseBranch->kind == StmtKind::Block)
            collectSpliceSites(s->elseBranch->body);
        for (const CatchClause& c : s->catches)
            if (c.body && c.body->kind == StmtKind::Block)
                collectSpliceSites(c.body->body);
    }
}

// Walk the program tree (namespaces/classes into their bodies, callable members
// into their statement bodies), recording every `@Name();` site and, alongside,
// the simple name of every declared `attribute`. Tree-based, not decls_-based, so
// it runs even when no rule exists.
void RuleEngine::gatherSpliceSites(std::vector<StmtPtr>& items,
                                   std::set<std::string_view>& attrNames) {
    for (StmtPtr& item : items) {
        if (!item) continue;
        Stmt* s = item.get();
        if (s->kind == StmtKind::Class && s->isAttribute) attrNames.insert(s->name);
        if (s->kind == StmtKind::Namespace || s->kind == StmtKind::Class)
            gatherSpliceSites(s->body, attrNames);
        else if (s->kind == StmtKind::Member && s->callable && s->memberBody &&
                 s->memberBody->kind == StmtKind::Block)
            collectSpliceSites(s->memberBody->body);
    }
}

// (Re)build the program-global splice-site index (design §2.3), then M43: every
// site must name a declared `attribute`, so a mistyped `@Bindz()` with no such
// attribute anywhere is a loud error — regardless of whether any rule exists (the
// typo net is a property of the site, not of a targeting rule).
void RuleEngine::indexSpliceSites(std::vector<StmtPtr>& items) {
    spliceSites_.clear();
    std::set<std::string_view> attrNames;
    gatherSpliceSites(items, attrNames);
    for (auto& [name, sites] : spliceSites_) {
        if (attrNames.count(name)) continue;
        for (const SpliceSiteRef& site : sites)
            sink_.error(site.node->span, "'@" + std::string(name) +
                        "()' is not a declared attribute — a splice site must name "
                        "'attribute " + std::string(name) + " { }'");   // M43
    }
}

// --- 3. deterministic rule order (§5.3) --------------------------------------
// Source offset is the stable total order: the whole-program gather concatenates
// files in manifest `sources` order, so offset encodes namespace-then-decl order
// deterministically. (UsesGraph namespace topo is a refinement, not needed for
// correctness — a rule only fires where its namespace is imported anyway.)
void RuleEngine::orderRules() {
    std::stable_sort(rules_.begin(), rules_.end(),
                     [](const OwnedRule& a, const OwnedRule& b) {
                         return a.offset < b.offset;
                     });
}

bool RuleEngine::declKindMatches(const Stmt* d, std::string_view kind) const {
    // `method` and `function` are the same node shape; the encloser clause
    // (`in class C`) enforces context, so accept either for callable members.
    // `type` (techdesign-splices-desugars-sonnet.md §2) is the subject-position
    // analogue of the encloser's existing `class`-also-matches-struct/interface
    // leniency — new and additive; `class`/`struct` alone stay exact.
    for (const DeclInfo& di : decls_)
        if (di.decl == d)
            return di.kindWord == kind ||
                   ((kind == "method" || kind == "function") &&
                    (di.kindWord == "method" || di.kindWord == "function")) ||
                   (kind == "type" &&
                    (di.kindWord == "class" || di.kindWord == "struct" ||
                     di.kindWord == "interface"));
    return false;
}

// Resolve a bare type name (the rule's `: IFace` constraint) to a symbol,
// searching the rule's namespace then global.
Symbol* RuleEngine::resolveTypeName(const TypeRef* t, const std::string& ns) const {
    if (!t) return nullptr;
    if (t->resolvedSymbol) return t->resolvedSymbol;   // resolver already did it
    if (Symbol* s = rulesDetail::lookupClassIn(namespaceScope(ns), t->name)) return s;
    return rulesDetail::lookupClassIn(sema_.global, t->name);
}

// Does `cls` implement/extend `iface`, directly or transitively? Reads the
// pass-1-resolved base list — bases carry resolvedSymbol from the resolver.
bool RuleEngine::implementsOrExtends(Symbol* cls, Symbol* iface) const {
    if (!cls || !iface) return false;
    if (cls == iface) return true;
    if (!cls->decl) return false;
    for (const TypeRefPtr& b : cls->decl->bases)
        if (b && implementsOrExtends(b->resolvedSymbol, iface)) return true;
    return false;
}

// Forward decl: defined near the other anchor helpers (§8.2), used by expand().
static bool findMarkerSlot(std::vector<StmtPtr>& vec, std::string_view marker,
                           std::vector<StmtPtr>** outVec, int* outIdx);

// #98 (2 of 2): resolve the attribute a rule's `match @Foo` names, from the
// RULE's own point of view — i.e. the same way resolveAttr resolves the `@Foo`
// written at a use site, but keyed off the file that DECLARES the rule. A rule
// writes `@Foo` (or `@NS::Foo`) relying on its own file's `uses` list plus the
// namespaces that file opens; the attribute it names may live in a DIFFERENT
// namespace than the rule (e.g. `ruleB` in `SibB` matching `@Foo` from `SibA`).
// The old code only looked in the rule's OWN namespace + the global scope, so a
// cross-namespace attribute resolved to null and the rule silently never fired.
// The scope guard in tryMatch (rule's namespace must be visible to the DECL's
// file) still governs whether the rule is eligible at all — this only fixes
// WHICH attribute symbol `match @Foo` denotes.
Symbol* RuleEngine::matchAttrSymbol(const OwnedRule& r) const {
    const RuleMatch& m = *r.node->ruleMatch;
    // Prefer an actual attribute class (mirrors resolveAttr): a same-named real
    // class must not shadow the attribute the rule means to match.
    auto attrIn = [&](Scope* sc) -> Symbol* {
        if (!sc) return nullptr;
        if (const std::vector<Symbol*>* v = sc->localLookup(m.attrName))
            for (Symbol* s : *v)
                if (s->kind == SymbolKind::Class && s->decl && s->decl->isAttribute)
                    return s;
        return nullptr;
    };
    // A qualified `match @NS::Name` walks straight down from the global scope.
    if (!m.attrPath.empty()) {
        Scope* sc = sema_.global;
        for (std::string_view seg : m.attrPath) {
            Scope* next = nullptr;
            if (const std::vector<Symbol*>* v = sc->localLookup(seg))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Namespace && s->scope) { next = s->scope; break; }
            if (!next) return nullptr;
            sc = next;
        }
        return attrIn(sc);
    }
    // Unqualified: search the namespaces visible to the rule's own file (its
    // `uses` + opened namespaces + std), then fall back to the rule's namespace
    // and the global scope (covers prelude/co-located rules with no file slot).
    int rfi = fileIdxFor(r.node->span);
    if (rfi >= 0 && rfi < (int)imports_.size())
        for (const std::string& ns : imports_[rfi].effective)
            if (Symbol* s = attrIn(namespaceScope(ns))) return s;
    if (Symbol* s = attrIn(namespaceScope(r.ns))) return s;
    return attrIn(sema_.global);
}

// --- 4. matching (§5.4) ------------------------------------------------------
bool RuleEngine::tryMatch(const OwnedRule& r, const DeclInfo& di, Bindings& out,
                          const AttrUse** firedAttr) {
    const RuleMatch& m = *r.node->ruleMatch;
    *firedAttr = nullptr;

    // scope: the rule's namespace must be visible to the decl's file (§5.2)
    if (di.fileIdx >= 0 && di.fileIdx < (int)imports_.size())
        if (!imports_[di.fileIdx].effective.count(r.ns)) return false;

    if (!declKindMatches(di.decl, m.subject.kindWord)) return false;

    // attribute pattern: an AttrUse on the decl resolving to the rule's attr
    if (m.hasAttr) {
        Symbol* want = matchAttrSymbol(r);
        const AttrUse* found = nullptr;
        int count = 0;
        for (AttrUse& a : di.decl->attrs)
            if (a.resolved && a.resolved == want) { found = &a; ++count; }
        if (!found) return false;
        if (m.one && count > 1) {
            sink_.error(found->span, "rule '" + std::string(r.node->name) +
                        "' requires at most one @" + std::string(m.attrName) +
                        "; found " + std::to_string(count));
            return false;
        }
        *firedAttr = found;
        if (!m.attrBind.empty()) {
            auto it = attrValues_.find(found);
            if (it != attrValues_.end()) {
                // §2: the attribute binds as an Object of its own class — the
                // SAME value both `$r.method` template splicing (below) and a
                // `where` clause (§4) read.
                Binding b;
                b.hasVal = true;
                b.val.kind = VKind::Object;
                b.val.obj = std::make_shared<Object>();
                b.val.obj->cls = found->resolved;
                for (const auto& av : it->second) b.val.obj->fields[av.field] = av.val;
                out[m.attrBind] = b;
            }
        }
    }

    // subject binding (m -> the matched decl)
    Binding sb;
    sb.declStmt = di.decl;
    sb.declSym = di.sym;   // class/namespace symbol, for a `$C`-as-type splice
    sb.selectorText = di.decl->selector.text.empty() ? di.decl->name
                                                     : di.decl->selector.text;
    out[m.subject.bind] = sb;
    // Internal sentinel (§6): lets $_params/$_args resolve against "the
    // subject" specifically, regardless of what the author named it.
    out[std::string_view("$subject")] = sb;

    // enclosers: bind outward from the innermost ancestor (§5.4)
    int cursor = (int)di.ancestors.size();
    for (const RuleLevel& lv : m.enclosers) {
        int found = -1;
        for (int ai = cursor - 1; ai >= 0; --ai)
            if (di.ancestors[ai].kindWord == lv.kindWord ||
                ((lv.kindWord == "class") &&
                 (di.ancestors[ai].kindWord == "struct" ||
                  di.ancestors[ai].kindWord == "interface"))) { found = ai; break; }
        if (found < 0) return false;
        const Ancestor& anc = di.ancestors[found];
        if (lv.constraint) {
            Symbol* iface = resolveTypeName(lv.constraint.get(), r.ns);
            if (!implementsOrExtends(anc.sym, iface)) return false;
        }
        Binding b; b.declStmt = anc.stmt; b.declSym = anc.sym;
        b.selectorText = anc.stmt->name;
        out[lv.bind] = b;
        cursor = found;
    }

    // `where` (§4): an ordinary comptime predicate with the bindings in scope
    // as plain names (meta.* objects, materialized on demand). Must yield
    // bool; false silently skips this decl (that's the feature).
    if (m.where) {
        std::unordered_map<std::string, Value> locals = materializeBindings(out);
        std::string err; bool failed = false;
        Value v = evalComptimeAt(m.where.get(), locals, err, failed);
        if (failed) {
            sink_.error(m.where->span, "rule '" + std::string(r.node->name) +
                        "' where-clause evaluation failed: " + err);
            return false;
        }
        if (v.kind != VKind::Bool) {
            sink_.error(m.where->span, "rule '" + std::string(r.node->name) +
                        "' where-clause must be bool (got '" + valueToString(v) + "')");
            return false;
        }
        if (!v.b) return false;
    }
    return true;
}

// --- 5. run: match every rule against every decl, in order -------------------
void RuleEngine::matchAndExpandRule(const OwnedRule& r) {
    for (const DeclInfo& di : decls_) {
        // §4 fixpoint guard: never expand the same rule on the same decl twice.
        // Harmless in the single-pass majority (each rule sweeps decls once);
        // essential across reentrant rounds, where re-indexing re-surfaces
        // already-fired decls — the rule must fire only on NEW ones.
        auto key = std::make_pair((const Stmt*)r.node.get(), (const Stmt*)di.decl);
        if (firedPairs_.count(key)) continue;
        Bindings b;
        const AttrUse* fired = nullptr;
        if (tryMatch(r, di, b, &fired)) {
            if (fired) const_cast<AttrUse*>(fired)->consumed = true;
            firedPairs_.insert(key);
            expand(r, di, b, fired);
        } else if (fired) {
            // bug.md #26: the rule matched this attribute (so it IS imported and
            // reachable) but a `where`/shape filter excluded this particular
            // decl — an intentional non-match, not a forgotten `uses`. Record it
            // so warnDanglingAttrs does not flag the attribute as dangling.
            const_cast<AttrUse*>(fired)->considered = true;
        }
    }
}

void RuleEngine::runRulePasses(bool reentrantOnly) {
    // §2.4 defined order: all additive `inject` rules apply first (in rule
    // order), THEN the `rewrites`/`replace` rewriters — so a rewriter's `$body`
    // captures "the method as written plus any additive prologue/epilogue".
    // Two passes over the offset-ordered rule list keep both halves
    // deterministic regardless of a rewriter's source position.
    for (const OwnedRule& r : rules_) {
        if (!r.node->ruleMatch || r.node->ruleRewrites || r.node->ruleGenerates) continue;
        if (reentrantOnly && !r.node->ruleReentrant) continue;
        matchAndExpandRule(r);
    }
    for (const OwnedRule& r : rules_) {
        if (!r.node->ruleMatch || !(r.node->ruleRewrites || r.node->ruleGenerates)) continue;
        if (reentrantOnly && !r.node->ruleReentrant) continue;
        matchAndExpandRule(r);
    }
}

void RuleEngine::runRules() { runRulePasses(/*reentrantOnly=*/false); }

// §4: resolve+evaluate attributes on rule-injected decls (whose attributes are
// unresolved because attribute resolution — step E — ran before this code
// existed). Processes only decls carrying an unresolved attribute, so
// already-resolved (original) attributes are never re-evaluated.
void RuleEngine::resolveNewAttrs(std::vector<StmtPtr>& items) {
    for (StmtPtr& item : items) {
        if (!item) continue;
        Stmt* s = item.get();
        bool anyUnresolved = false;
        for (const AttrUse& a : s->attrs) if (!a.resolved) { anyUnresolved = true; break; }
        if (anyUnresolved) processAttrs(s);
        if (s->kind == StmtKind::Namespace || s->kind == StmtKind::Class)
            resolveNewAttrs(s->body);
    }
}

// §4: the gated reentrant fixpoint. The initial sweep (runRules) already fired
// every rule once; this re-runs ONLY `reentrant` rules on the re-indexed tree
// until a round fires nothing new (converged) or the round budget trips (M34).
void RuleEngine::runReentrantFixpoint(Program& program) {
    const OwnedRule* seed = nullptr;
    for (const OwnedRule& r : rules_)
        if (r.node->ruleReentrant) { seed = &r; break; }
    if (!seed) return;   // no reentrant rule: the safe majority path, nothing to do

    for (int round = 1; ; ++round) {
        // Fold pending `namespace N` injections into the tree so a reentrant
        // rule can match decls a prior round introduced there.
        for (StmtPtr& ns : namespaceInjections_) program.items.push_back(std::move(ns));
        namespaceInjections_.clear();

        if (round > reentrantRounds_) {
            sink_.error(seed->node->span,
                "reentrant rule expansion did not converge within " +
                std::to_string(reentrantRounds_) + " rounds — a rule is "
                "re-triggering itself; raise the bound with --reentrant-budget "
                "or break the cycle");   // M34
            return;
        }

        size_t firedBefore = firedPairs_.size();
        resolveNewAttrs(program.items);          // resolve attributes on new code
        decls_.clear();                          // re-index the expanded tree
        std::vector<Ancestor> chain;
        indexDecls(program.items, chain, "<root>");
        indexSpliceSites(program.items);         // rebuild the splice-site index too
        runRulePasses(/*reentrantOnly=*/true);   // only reentrant rules re-trigger
        if (firedPairs_.size() == firedBefore) return;   // fixpoint: nothing new fired
    }
}

// --- clone-time macro expansion (§7×§9) --------------------------------------
void RuleEngine::expandMacrosInClone(StmtPtr& s) {
    bool saved = macroExpansionEnabled_;
    macroExpansionEnabled_ = true;
    walkStmt(s);
    macroExpansionEnabled_ = saved;
}

void RuleEngine::expandMacrosInClone(std::vector<StmtPtr>& stmts) {
    bool saved = macroExpansionEnabled_;
    macroExpansionEnabled_ = true;
    walkItems(stmts);
    macroExpansionEnabled_ = saved;
}

// --- 6. expansion: clone the template, substitute holes, place at anchor -----
void RuleEngine::expand(const OwnedRule& r, const DeclInfo& di, Bindings& b,
                        const AttrUse* firedAttr) {
    curRuleNs_ = r.ns;   // §10: def-site qualification during this firing's clones
    curRuleName_ = r.node->name;   // B2: for `$if` diagnostics (M40)
    for (const RuleAction& act : r.node->ruleActions) {
        // Hygiene (§7.1): alpha-rename any local this template declares, fresh
        // per firing, so it cannot collide with use-site names.
        renames_.clear();
        if (act.tmplMember) collectTemplateLocals(act.tmplMember.get());
        for (const StmtPtr& t : act.tmplStmts) collectTemplateLocals(t.get());

        bool err = false;
        if (act.anchor == AnchorKind::MemberOf) {
            if (!act.tmplMember) continue;
            // The template is one member, or a `$for m ... : <member>` that
            // repeats a member per iteration (item J, member position).
            std::vector<StmtPtr> members;
            cloneStmtInto(act.tmplMember.get(), b, err, members);
            if (err) continue;
            expandMacrosInClone(members);
            Stmt* cls = boundClass(b, act.target);
            if (!cls) { sink_.error(act.span, "'" + std::string(act.target) +
                                    "' is not a bound class"); continue; }
            for (StmtPtr& member : members)
                if (member) injectMember(cls, std::move(member), r);

        } else if (act.anchor == AnchorKind::CtorTop || act.anchor == AnchorKind::CtorBottom) {
            std::vector<StmtPtr> stmts;
            for (const StmtPtr& t : act.tmplStmts) cloneStmtInto(t.get(), b, err, stmts);
            if (err) continue;
            expandMacrosInClone(stmts);
            Stmt* cls = boundClass(b, act.target);
            if (!cls) { sink_.error(act.span, "'" + std::string(act.target) +
                                    "' is not a bound class"); continue; }
            injectIntoCtors(cls, stmts, act.anchor == AnchorKind::CtorTop);

        } else if (act.anchor == AnchorKind::BodyTop || act.anchor == AnchorKind::BodyBottom) {
            // §8.1: target is implicit — always the subject (the grammar has
            // no identifier here).
            Stmt* subj = subjectOf(b);
            const char* which = act.anchor == AnchorKind::BodyTop ? "top of body"
                                                                  : "bottom of body";
            if (!subj || !subj->callable || !subj->memberBody) {
                sink_.error(act.span, "'" + std::string(which) +
                            "' needs a callable subject; '" +
                            std::string(subj ? subj->name : std::string_view("?")) +
                            "' is a " + rulesDetail::declKindWord(subj));
                continue;
            }
            normalizeMemberBody(subj);
            std::vector<StmtPtr>& body = subj->memberBody->body;
            std::vector<StmtPtr> cloned;
            for (const StmtPtr& t : act.tmplStmts) cloneStmtInto(t.get(), b, err, cloned);
            if (err) continue;
            expandMacrosInClone(cloned);
            if (act.anchor == AnchorKind::BodyTop) {
                body.insert(body.begin(), std::make_move_iterator(cloned.begin()),
                            std::make_move_iterator(cloned.end()));
            } else {
                // An arrow body normalizes to `{ return expr; }` — appending
                // after a value return is unreachable (M25); additive rules
                // needing "after the result" semantics want a Layer-D wrap.
                if (!body.empty() && body.back()->kind == StmtKind::Return) {
                    sink_.error(act.span, "'bottom of body' would inject after a "
                                "value return in '" + std::string(subj->name) +
                                "' (its body is an arrow expression) — unreachable; "
                                "use 'top of body' or a Layer-D wrap instead");
                    continue;
                }
                for (StmtPtr& s : cloned) body.push_back(std::move(s));
            }

        } else if (act.anchor == AnchorKind::Marker) {
            // §8.2: also subject-implicit; searched within its normalized body.
            Stmt* subj = subjectOf(b);
            if (!subj || !subj->memberBody) {
                sink_.error(act.span, "marker " + std::string(act.markerName) +
                            " not found in '" +
                            std::string(subj ? subj->name : std::string_view("?")) + "'");
                continue;
            }
            normalizeMemberBody(subj);
            std::vector<StmtPtr>* vecPtr = nullptr;
            int idx = -1;
            if (!findMarkerSlot(subj->memberBody->body, act.markerName, &vecPtr, &idx)) {
                sink_.error(act.span, "marker " + std::string(act.markerName) +
                            " not found in '" + std::string(subj->name) + "'");
                continue;
            }
            std::vector<StmtPtr> cloned;
            for (const StmtPtr& t : act.tmplStmts) cloned.push_back(cloneStmt(t.get(), b, err));
            if (err) continue;
            expandMacrosInClone(cloned);
            Stmt* markerNode = (*vecPtr)[idx].get();
            int insertAt = idx + 1 + markerInsertCount_[markerNode];
            int n = (int)cloned.size();
            vecPtr->insert(vecPtr->begin() + insertAt,
                           std::make_move_iterator(cloned.begin()),
                           std::make_move_iterator(cloned.end()));
            markerInsertCount_[markerNode] += n;

        } else if (act.anchor == AnchorKind::SpliceSite) {
            // Named-anchor design §2.3: land at the program-global `@Name();`
            // site(s), NOT the matched subject's own body — the entire delta over
            // subject-local `at marker`. The site's index was built in
            // indexSpliceSites(); the spliced statements inherit the site body's
            // lexical scope by placement (no plumbing — design §2.4).
            auto it = spliceSites_.find(act.target);
            size_t nsites = it == spliceSites_.end() ? 0 : it->second.size();
            if (nsites == 0) {
                sink_.error(act.span, "rule '" + std::string(r.node->name) +
                            "' injects at splice '" + std::string(act.target) +
                            "' but no '@" + std::string(act.target) +
                            "()' splice site exists");   // M42 (zero sites)
                continue;
            }
            // Default single-site keeps the common case honest; ≥2 sites without
            // `multi` is an error naming the sites, so a stray second `@Name()`
            // can't silently double the injection (design §2.3 point 3).
            if (nsites > 1 && !act.spliceMulti) {
                std::string locs;
                for (size_t i = 0; i < it->second.size(); ++i) {
                    LineCol lc = lineColAt(file_.text, it->second[i].node->span.offset);
                    if (i) locs += ", ";
                    locs += std::to_string(lc.line) + ":" + std::to_string(lc.col);
                }
                sink_.error(act.span, "rule '" + std::string(r.node->name) +
                            "' injects at splice '" + std::string(act.target) +
                            "' but " + std::to_string(nsites) + " '@" +
                            std::string(act.target) + "()' sites exist at " + locs +
                            " — add 'multi' to fan out or keep one site");   // M42 (multi)
                continue;
            }
            // Clone the template per matched site and insert AFTER the marker,
            // accumulating in rule order via markerInsertCount_ (reused wholesale
            // from the `at marker` arm — several rules stacking on one site stay
            // deterministic). The marker node stays in the tree, so `--expand`
            // shows the site with the injected statements around it.
            for (SpliceSiteRef& site : it->second) {
                std::vector<StmtPtr> cloned;
                for (const StmtPtr& t : act.tmplStmts)
                    cloned.push_back(cloneStmt(t.get(), b, err));
                if (err) break;
                expandMacrosInClone(cloned);
                std::vector<StmtPtr>& v = *site.vec;
                int idx = -1;
                for (int i = 0; i < (int)v.size(); ++i)
                    if (v[i].get() == site.node) { idx = i; break; }
                if (idx < 0) continue;   // marker vanished (defensive)
                int insertAt = idx + 1 + markerInsertCount_[site.node];
                int n = (int)cloned.size();
                v.insert(v.begin() + insertAt,
                         std::make_move_iterator(cloned.begin()),
                         std::make_move_iterator(cloned.end()));
                markerInsertCount_[site.node] += n;
            }
            if (err) continue;

        } else if (act.anchor == AnchorKind::BodyReplace ||
                   act.anchor == AnchorKind::BodyGenerate) {
            // Layer D (§2.3) / bindgen metaprog scope: overwrite the `rewrites
            // body of <bind>` / `generates body of <bind>` target's body. A
            // `rewrites` action splices the original back in wherever `$body`
            // appears; a `generates` action discards it outright (M36 already
            // rejected any `$body` reference in this template at validate time).
            bool isGenerate = act.anchor == AnchorKind::BodyGenerate;
            std::string_view tgt = r.node->rewritesTarget;
            Stmt* subj = nullptr;
            auto bit = b.find(tgt);
            if (bit != b.end()) subj = bit->second.declStmt;
            if (!subj || !subj->callable || !subj->memberBody) {
                sink_.error(act.span, "rule '" + std::string(r.node->name) +
                            "' " + (isGenerate ? "generates" : "rewrites") +
                            " body of '" + std::string(tgt) +
                            "', but it has no body to replace");   // M35 (expansion)
                continue;
            }
            std::string qualName = (r.ns == "<root>" ? std::string(r.node->name)
                                                     : r.ns + "::" + std::string(r.node->name));
            // §3 confluence: only one replace/generate may own a body (no order
            // composes two independent whole-body rewrites). The two rules are
            // named in stable source-offset order (orderRules), so the report is
            // reproducible: the earlier owner owns the mark, the later trips it.
            auto prev = replacedBy_.find(subj);
            if (prev != replacedBy_.end()) {
                sink_.error(act.span, "rule '" + prev->second + "' and rule '" +
                            qualName + "' both replace the body of '" +
                            std::string(subj->name) +
                            "' — two whole-body replacements do not compose");   // M33
                continue;
            }
            replacedBy_[subj] = qualName;
            // `rewrites`: capture the original body under `$body` (normalized to
            // a Block so the statement form has a vector), clone the template —
            // which splices it — then overwrite. The clone reads replaceBodyOrig_
            // before the overwrite frees the old body. `generates` leaves
            // replaceBodyOrig_ null: there is nothing to splice, by design.
            normalizeMemberBody(subj);
            if (!isGenerate) replaceBodyOrig_ = subj->memberBody.get();
            std::vector<StmtPtr> cloned;
            cloneStmtListBody(act.tmplStmts, b, err, cloned);
            replaceBodyOrig_ = nullptr;
            if (err) continue;
            expandMacrosInClone(cloned);
            auto newBlock = std::make_unique<Stmt>(StmtKind::Block);
            newBlock->span = subj->memberBody->span;
            newBlock->body = std::move(cloned);
            subj->memberBody = std::move(newBlock);

        } else {   // NamespaceScope (§8.3): target is a literal namespace name,
                   // not a binding — reopen-and-merge happens in pass 2 (§12).
            auto ns = std::make_unique<Stmt>(StmtKind::Namespace);
            ns->span = act.span;
            ns->name = act.target;
            for (const StmtPtr& t : act.tmplStmts) cloneStmtInto(t.get(), b, err, ns->body);
            if (err) continue;
            expandMacrosInClone(ns->body);
            namespaceInjections_.push_back(std::move(ns));
        }

        ExpansionRecord rec;
        rec.ruleName = (r.ns == "<root>" ? std::string(r.node->name)
                                         : r.ns + "::" + std::string(r.node->name));
        rec.origin = firedAttr ? firedAttr->span : di.decl->span;
        rec.templateSpan = act.quasiSpan;
        rec.fileIndex = di.fileIdx;
        expansions_.push_back(std::move(rec));
        changed_ = true;
        // #98: a rewrite that landed on a prelude decl must survive to the
        // backend — main.cpp's pass-2 resolver re-resolves this mutated prelude
        // tree (adoptPrelude) rather than re-parsing a fresh, un-rewritten one.
        if (preludeFileIdx_ >= 0 && di.fileIdx == preludeFileIdx_)
            preludeMutated_ = true;
    }
}

// --- anchors (§5.6, §8) -------------------------------------------------------
Stmt* RuleEngine::boundClass(const Bindings& b, std::string_view target) const {
    auto it = b.find(target);
    if (it == b.end() || !it->second.declStmt) return nullptr;
    Stmt* s = it->second.declStmt;
    return s->kind == StmtKind::Class ? s : nullptr;
}

// Ensures `member->memberBody` is a Block (wrapping a single-statement body
// in place if not), so callers have a statement vector to splice into.
void RuleEngine::normalizeMemberBody(Stmt* member) {
    if (member->memberBody && member->memberBody->kind == StmtKind::Block) return;
    auto block = std::make_unique<Stmt>(StmtKind::Block);
    block->span = member->span;
    if (member->memberBody) block->body.push_back(std::move(member->memberBody));
    member->memberBody = std::move(block);
}

// Recursively finds an Empty stmt named `marker` inside `vec` (or a nested
// Block reachable from it — §8.2's "normalized body, walked recursively").
// On success, *outVec/*outIdx locate its CURRENT position (re-scanned fresh
// each call, so it stays correct across earlier insertions elsewhere).
static bool findMarkerSlot(std::vector<StmtPtr>& vec, std::string_view marker,
                           std::vector<StmtPtr>** outVec, int* outIdx) {
    for (size_t i = 0; i < vec.size(); ++i) {
        Stmt* s = vec[i].get();
        if (s->kind == StmtKind::Empty && !s->name.empty() && s->name == marker) {
            *outVec = &vec; *outIdx = (int)i; return true;
        }
        if (s->kind == StmtKind::Block && findMarkerSlot(s->body, marker, outVec, outIdx))
            return true;
        if (s->thenBranch && s->thenBranch->kind == StmtKind::Block &&
            findMarkerSlot(s->thenBranch->body, marker, outVec, outIdx)) return true;
        if (s->elseBranch && s->elseBranch->kind == StmtKind::Block &&
            findMarkerSlot(s->elseBranch->body, marker, outVec, outIdx)) return true;
        for (const CatchClause& c : s->catches)
            if (c.body && c.body->kind == StmtKind::Block &&
                findMarkerSlot(c.body->body, marker, outVec, outIdx)) return true;
    }
    return false;
}

void RuleEngine::injectIntoCtors(Stmt* cls, std::vector<StmtPtr>& stmts, bool top) {
    // Collect the class's constructors; synthesize a nullary one if none exist
    // (the language guarantees the implicit nullary path — §3).
    std::vector<Stmt*> ctors;
    for (const StmtPtr& m : cls->body)
        if (m->kind == StmtKind::Member && m->isCtor) ctors.push_back(m.get());
    if (ctors.empty()) {
        auto ctor = std::make_unique<Stmt>(StmtKind::Member);
        ctor->span = cls->span;
        ctor->isCtor = true; ctor->callable = true; ctor->isMutating = true;
        ctor->access = Access::Public;
        ctor->name = cls->name;
        ctor->selector.text = cls->name;
        auto body = std::make_unique<Stmt>(StmtKind::Block);
        body->span = cls->span;
        ctor->memberBody = std::move(body);
        ctors.push_back(ctor.get());
        cls->body.push_back(std::move(ctor));
    }
    for (Stmt* ctor : ctors) {
        normalizeMemberBody(ctor);   // ensure a Block so we have a vector to splice
        std::vector<StmtPtr>& body = ctor->memberBody->body;
        // Deep-clone the statements per ctor (each ctor owns its own copy).
        std::vector<StmtPtr> copy;
        bool err = false;
        Bindings empty;
        for (const StmtPtr& s : stmts) copy.push_back(cloneStmt(s.get(), empty, err));
        if (top) {
            // Insert after the leading run of base-ctor calls (Base::Ctor(...)).
            size_t pos = 0;
            while (pos < body.size()) {
                Stmt* st = body[pos].get();
                bool baseCall = st->kind == StmtKind::ExprStmt && st->expr &&
                    st->expr->kind == ExprKind::Call && st->expr->a &&
                    st->expr->a->kind == ExprKind::Member && st->expr->a->colon;
                if (!baseCall) break;
                ++pos;
            }
            body.insert(body.begin() + pos,
                        std::make_move_iterator(copy.begin()),
                        std::make_move_iterator(copy.end()));
        } else {
            for (StmtPtr& s : copy) body.push_back(std::move(s));
        }
    }
}

void RuleEngine::injectMember(Stmt* cls, StmtPtr member, const OwnedRule& r) {
    // §3 confluence (M33 family): a member of the same name + same type already
    // exists — a collision at the `member of` anchor. The injected member isn't
    // resolved yet, so compare the *syntactic* return type (TypeRef::name),
    // populated by the parser. Different-typed same-name members coexist
    // (resolution by type). One degree of `distinct` on either side keeps the
    // same-name+type pair as separate slots (§4.3 escape), so no conflict.
    auto typeName = [](const Stmt* m) -> std::string_view {
        return m->type ? m->type->name : std::string_view{};
    };
    std::string rn = (r.ns == "<root>" ? std::string(r.node->name)
                                       : r.ns + "::" + std::string(r.node->name));
    for (const StmtPtr& m : cls->body) {
        if (m->kind != StmtKind::Member) continue;
        if (m->name != member->name) continue;
        if (typeName(m.get()) != typeName(member.get())) continue;   // resolution by type
        if (member->distinct || m->distinct) continue;               // §4.3 distinct escape
        // Name both rules when the existing member was itself rule-injected —
        // the consistent "rule X and rule Y conflict at <anchor>" M33 shape.
        auto other = injectedMemberBy_.find(m.get());
        if (other != injectedMemberBy_.end())
            sink_.error(member->span, "rule '" + other->second + "' and rule '" + rn +
                        "' both inject a member '" + std::string(member->name) +
                        "' of the same type into class '" + std::string(cls->name) +
                        "' — mark one 'distinct' to keep them separate");   // M33
        else
            sink_.error(member->span, "rule '" + rn + "' injects a member '" +
                        std::string(member->name) + "' that collides with an "
                        "existing member of the same type in class '" +
                        std::string(cls->name) +
                        "' — mark one 'distinct' to keep them separate");   // M33 family
        return;
    }
    injectedMemberBy_[member.get()] = rn;   // record BEFORE the move (raw ptr stays valid)
    cls->body.push_back(std::move(member));
}

// --- 7. dangling-attribute warning (§5.7 / M05) ------------------------------
void RuleEngine::warnDanglingAttrs(std::vector<StmtPtr>& rootItems) {
    // An attribute that resolved but no imported rule consumed usually means a
    // missing `uses`. Suggest the namespace of a rule that reads that attr.
    std::function<void(std::vector<StmtPtr>&)> walk = [&](std::vector<StmtPtr>& items) {
        for (StmtPtr& item : items) {
            if (!item) continue;
            for (const AttrUse& a : item->attrs) {
                if (!a.resolved || a.consumed || a.considered) continue;   // #26
                // Only warn when a rule reading this attribute EXISTS somewhere —
                // that is the actionable "you forgot a `uses`" case. An attribute
                // no rule anywhere consumes is legitimate inert data (§4.1), not
                // a mistake, so it stays silent.
                // techdesign-splices-desugars-sonnet.md §2.3(3): a rule for this
                // attribute may exist but never fire because its subject kind
                // (e.g. `on class`) can't match this declaration's kind (e.g. a
                // `struct`) — that is a DIFFERENT mistake than a missing `uses`,
                // and blaming `uses` sends the author down the wrong trail.
                std::string hint;
                bool ruleExists = false;
                bool kindCompatibleRuleExists = false;
                std::string mismatchRuleName, mismatchKind;
                for (const OwnedRule& r : rules_) {
                    const RuleMatch& m = *r.node->ruleMatch;
                    if (m.hasAttr && m.attrName == a.name) {
                        ruleExists = true;
                        if (declKindMatches(item.get(), m.subject.kindWord)) {
                            kindCompatibleRuleExists = true;
                            if (r.ns != "<root>") hint = r.ns;
                        } else if (mismatchRuleName.empty()) {
                            mismatchRuleName = r.ns == "<root>" ? std::string(r.node->name)
                                                                : r.ns + "::" + std::string(r.node->name);
                            mismatchKind = m.subject.kindWord;
                        }
                    }
                }
                if (!ruleExists) continue;
                std::string msg;
                if (!kindCompatibleRuleExists && !mismatchRuleName.empty()) {
                    msg = "attribute '@" + std::string(a.name) + "' has a rule '" +
                        mismatchRuleName + "' that matches '" + mismatchKind +
                        "' subjects, but this is a '" + rulesDetail::declKindWord(item.get()) +
                        "' — did you mean 'on type' / 'on " + rulesDetail::declKindWord(item.get()) + "'?";
                } else {
                    msg = "attribute '@" + std::string(a.name) +
                        "' matched no imported rule";
                    if (!hint.empty()) msg += " (missing 'uses " + hint + "'?)";
                }
                sink_.warning(a.span, msg);
            }
            if (item->kind == StmtKind::Namespace || item->kind == StmtKind::Class)
                walk(item->body);
        }
    };
    walk(rootItems);
}

// --- --rules report ----------------------------------------------------------
std::string RuleEngine::renderRulesReport() const {
    std::string out = "[rules] " + std::to_string(rules_.size()) +
                      " rule(s), " + std::to_string(expansions_.size()) +
                      " expansion(s)\n";
    for (const ExpansionRecord& e : expansions_) {
        LineCol oc = lineColAt(file_.text, e.origin.offset);
        out += "[rules]   " + e.ruleName + " fired at " +
               std::to_string(oc.line) + ":" + std::to_string(oc.col) + "\n";
    }
    return out;
}
