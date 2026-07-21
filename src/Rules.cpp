#include "Rules.hpp"
#include "Checker.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include <algorithm>
#include <functional>
#include <cstdio>

// -----------------------------------------------------------------------------
// Phase 1 of the rule stage: comptime folding + attribute resolution.
// Everything here runs on the pass-1 resolved tree; whatever it changes is
// re-resolved and checked by pass 2, so folded/injected code is checked
// exactly like hand-written code (§16.5 P1, cost-identical).
// -----------------------------------------------------------------------------

RuleEngine::RuleEngine(const std::vector<ProjectFile>& files,
                       const Sema& sema, Program& prelude,
                       const SourceFile& file, DiagnosticSink& sink,
                       const ComptimeOptions& opts)
    : files_(files), sema_(sema), file_(file), sink_(sink),
      eval_(sema, sink), prelude_(&prelude) {
    eval_.setComptime(opts);
    eval_.setComptimeParseHook(
        [this](bool expr, const std::string& text, SourceSpan, std::string& err) {
            return parseGenerated(expr, text, err);
        });
    reentrantRounds_ = opts.reentrantRounds;   // §4 M34 budget
    eval_.initGlobals(prelude);   // std::read etc. — pure value construction
}

Value RuleEngine::parseGenerated(bool expr, const std::string& text, std::string& err) {
    generatedSources_.push_back({"<procedural-macro>", text});
    SourceFile& src = generatedSources_.back();
    DiagnosticSink local;
    Lexer lex(src, local);
    Parser parser(lex.tokenize(), src, local);
    auto payload = std::make_shared<AstPayload>();
    payload->kind = expr ? AstFragmentKind::Expr : AstFragmentKind::Stmts;
    if (expr) payload->expr = parser.parseExpressionFragment();
    else payload->stmts = parser.parseStatementsFragment();
    if (local.hasErrors()) {
        const Diagnostic& d = local.all().front();
        LineCol lc = lineColAt(src.text, d.span.offset);
        size_t lineStart = d.span.offset;
        while (lineStart > 0 && src.text[lineStart - 1] != '\n') --lineStart;
        size_t lineEnd = src.text.find('\n', d.span.offset);
        if (lineEnd == std::string::npos) lineEnd = src.text.size();
        std::string line = src.text.substr(lineStart, lineEnd - lineStart);
        err = std::string(expr ? "meta::parseExpr" : "meta::parseStmts") +
              " failed: " + d.message + " at " + std::to_string(lc.line) + ":" +
              std::to_string(lc.col) + "\n" + line + "\n" +
              std::string(lc.col > 0 ? lc.col - 1 : 0, ' ') + "^";
        return vvoid();
    }
    Value out = vast(std::move(payload));
    out.s = expr ? "expr" : "stmts";
    return out;
}

// §9 (Phase 3): ordered passes, so a `uses` inside a taken `comptime if`
// branch is visible to scoping decisions made later in the SAME run — the
// engine recomputes its own imports map after folding, rather than trusting
// one computed on the pre-fold tree.
bool RuleEngine::run(Program& program) {
    changed_ = false;
    // A. Detach rules (and macro decls, which reuse StmtKind::Rule) from the
    // tree — pass 2 must never see either (§5.1).
    collectRules(program.items, "<root>");
    // #98: prelude-authored rules join the same rules_ list, so a rule shipped
    // in a prelude/*.lev segment fires exactly like a project-file rule.
    if (prelude_) collectRules(prelude_->items, "<root>");
    // M22 (§7): static, independent of whether/where a macro is ever called —
    // fires even for a macro nobody uses yet.
    validateMacroDecls();
    // M32/M35/M36 (§2.3; M36 added by the bindgen metaprog scope design): static
    // checks on `rewrites`/`generates` rules, likewise fire-or-not independent —
    // a malformed rewriter/generator is an error even if it never matches.
    validateRewriteRules();
    comptimeScope_ = sema_.global;   // §8: root-level comptime is scope-complete
    // B. Comptime fold walk ONLY — vars/ifs/exprs. Macro-call expansion does
    // NOT ride this pass (despite the design note that it's "the same walk
    // that folds comptime exprs") — a macro call scopes through imports_ the
    // same as an attribute/rule (§5.2), which does not exist yet here; it
    // isn't computed until step C, on the tree THIS walk produces. Interleaving
    // macro expansion into this same walk would resolve every unqualified
    // macro call against an empty imports_ (nothing visible yet, "unknown
    // macro" on everything). macroExpansionEnabled_ keeps walkExpr's hook off
    // for this call, mirroring exactly why attribute processing was already
    // split into its own post-C pass (D) rather than living in this walk too.
    walkTopLevelItems(program.items);
    // C. Recompute the imports map on the POST-fold tree — a `uses` a
    // comptime-if spliced in at item level (step B) is visible here.
    imports_ = computeFileImports(files_, program);
    // #98: the prelude is a distinct buffer with its own offset space that
    // collides with the user tree's, so it cannot share a slot in `imports_`
    // keyed by offset. Compute its provenance separately (one synthetic file
    // covering every offset) and append it at preludeFileIdx_. A prelude decl's
    // visibility test then resolves against the prelude's OWN declared
    // namespaces — co-location (a rule + its subject in one file) needs no
    // `uses`, which is exactly the prelude repro's shape.
    if (prelude_ && !prelude_->items.empty()) {
        preludeFileIdx_ = (int)imports_.size();
        std::vector<ProjectFile> pf = {{"<prelude>", 0u, UINT32_MAX, "", ""}};
        std::vector<FileImports> pi = computeFileImports(pf, *prelude_);
        imports_.push_back(std::move(pi[0]));
    }
    // D. Macro-call expansion (§7), now that imports_ reflects the post-fold
    // tree. A second walk over the same (already-folded) tree: safe to
    // re-run because every comptime-fold site either replaces itself with a
    // fresh, non-comptime-marked node (If, ExprStmt, and expr folding all
    // swap in a brand-new node) or explicitly clears `isComptime` once folded
    // (Var, below) — so nothing re-folds, refires a compile-time side effect,
    // or re-consumes step budget the second time through.
    macroExpansionEnabled_ = true;
    walkTopLevelItems(program.items);
    macroExpansionEnabled_ = false;
    // E. Attribute resolution (Layer A), scoped by the same recomputed map.
    walkAttrs(program.items);
    // #98: resolve attributes on prelude decls too (an `@GenSpike` on a
    // prelude method must resolve for the matching rule to see it). The prelude
    // buffer's offsets collide with the user tree's, so force its dedicated
    // imports slot onto processAttrs instead of routing through fileOf.
    if (prelude_ && preludeFileIdx_ >= 0) {
        fileIdxOverride_ = preludeFileIdx_;
        walkAttrs(prelude_->items);
        fileIdxOverride_ = -1;
    }
    // E2. Build the splice-site index + validate sites (M43). Always — a malformed
    // `@Name();` site must be rejected even in a rule-free program (named-anchor
    // design §2.1); `expand`'s `at splice` arm reads the index built here.
    indexSpliceSites(program.items);
    // F. Index every matchable declaration, then match + expand rules (Layer B).
    if (!rules_.empty()) {
        std::vector<Ancestor> chain;
        indexDecls(program.items, chain, "<root>");
        // #98: index prelude decls under their dedicated file slot so a rule
        // (prelude- or project-declared) can match them, and so the expansion
        // records target the right visibility set.
        if (prelude_ && preludeFileIdx_ >= 0) {
            fileIdxOverride_ = preludeFileIdx_;
            std::vector<Ancestor> pchain;
            indexDecls(prelude_->items, pchain, "<root>");
            fileIdxOverride_ = -1;
        }
        orderRules();
        runRules();
        // §4: re-run reentrant rules to a fixpoint (no-op unless a reentrant
        // rule exists). Must precede the namespaceInjections_ flush below —
        // the fixpoint folds and consumes them itself, round by round.
        runReentrantFixpoint(program);
    }
    // `namespace N` anchors (§8.3): reopen-and-merge via an ordinary top-level
    // namespace Stmt; pass 2's existing gather handles the merge.
    for (StmtPtr& ns : namespaceInjections_) program.items.push_back(std::move(ns));
    // G. Warn on attributes no in-scope rule consumed (§5.7 / M05).
    warnDanglingAttrs(program.items);
    return changed_;
}

// --- file / scope plumbing ---------------------------------------------------

int RuleEngine::fileOf(SourceSpan span) const {
    for (int i = 0; i < (int)files_.size(); ++i)
        if (span.offset >= files_[i].offset &&
            span.offset < files_[i].offset + files_[i].length)
            return i;
    return files_.empty() ? -1 : 0;
}

Scope* RuleEngine::namespaceScope(const std::string& ns) const {
    if (ns == "<root>") return sema_.global;
    // #91: a namespace key may be a nested path ("Atlantis::Orm") — walk each
    // segment down from the global scope.
    Scope* sc = sema_.global;
    size_t pos = 0;
    while (pos <= ns.size()) {
        size_t sep = ns.find("::", pos);
        std::string seg = ns.substr(pos, sep == std::string::npos ? std::string::npos
                                                                  : sep - pos);
        Scope* next = nullptr;
        if (const std::vector<Symbol*>* v = sc->localLookup(seg))
            for (Symbol* s : *v)
                if (s->kind == SymbolKind::Namespace && s->scope) { next = s->scope; break; }
        if (!next) return nullptr;
        sc = next;
        if (sep == std::string::npos) break;
        pos = sep + 2;
    }
    return sc;
}

std::string RuleEngine::moduleOf(SourceSpan span) const {
    int fi = fileOf(span);
    return (fi >= 0 && fi < (int)files_.size()) ? files_[fi].moduleId : std::string();
}

Value RuleEngine::evalComptimeAt(Expr* e, std::string& err, bool& failed) {
    if (e) eval_.setImportModule(moduleOf(e->span));
    return eval_.evalComptime(e, err, failed);
}

Value RuleEngine::evalComptimeAt(Expr* e, const std::unordered_map<std::string, Value>& locals,
                                 std::string& err, bool& failed) {
    if (e) eval_.setImportModule(moduleOf(e->span));
    return eval_.evalComptime(e, locals, err, failed);
}

namespace {
// LA-20: look up a FUNCTION symbol by name in a namespace scope (mirrors
// lookupClassIn below, for the same reason — locating the prelude's
// std::import declaration once, by identity).
Symbol* lookupFunctionIn(Scope* sc, std::string_view name) {
    if (!sc) return nullptr;
    if (const std::vector<Symbol*>* v = sc->localLookup(name))
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Function) return s;
    return nullptr;
}
}  // namespace

void RuleEngine::setImportContext(ImportContext ctx) {
    ctx.importFn = nullptr;
    if (Symbol* fn = lookupFunctionIn(namespaceScope("std"), "import"))
        ctx.importFn = fn->decl;
    eval_.setImportContext(std::move(ctx));
}

// LA-20 §8: record which reified literal spans came from a folded import()
// call, for --expand's large-literal elision. There is no AST-level identity
// check available here (the checker — which runs after this stage — is what
// sets Call::resolved; §6's dynamic-dispatch intercept is the only thing
// that actually knows a call reached std::import). So this observes the
// Evaluator's own import bookkeeping instead: exactly one new ImportedAsset
// appeared during this fold, and the folded string is byte-for-byte its
// content — strong enough in practice (an import() site's value is either
// exactly the read content or, once combined with other expressions, no
// longer matches the byte count exactly, and is correctly left un-elided).
void RuleEngine::noteImportLiteral(size_t assetsBefore, const Value& v, SourceSpan span) {
    const std::vector<ImportedAsset>& assets = eval_.importedAssets();
    if (v.kind != VKind::String || assets.size() != assetsBefore + 1) return;
    if (assets.back().bytes != v.s.size()) return;
    importLiteralSpans_[span.offset] = {assets.back().rel, assets.back().bytes};
}

std::string_view RuleEngine::own(std::string text) {
    reifiedText_.push_back(std::move(text));
    return reifiedText_.back();
}

// §8: type-check a comptime root that FAILED to evaluate, for a precise message.
// Runs only at a scope-complete position (comptimeScope_ non-null), into a
// throwaway sink, so it can never introduce a false error — worst case it finds
// nothing and the caller keeps the opaque eval message.
std::string RuleEngine::precheckComptime(const Expr* e) {
    if (!comptimeScope_ || !e) return "";
    DiagnosticSink scratch;
    Checker checker(sema_, file_, scratch);
    checker.checkComptimeRoot(e, comptimeScope_);
    for (const Diagnostic& d : scratch.all())
        if (d.severity == Severity::Error) return d.message;
    return "";
}

// --- the walk (Layer C — comptime folding + macro expansion only) -----------
//
// Attribute processing (Layer A) is a SEPARATE walk, `walkAttrs`, run after
// this one — §9 splits what used to be one interleaved pass so imports_ can
// be recomputed between them (folding may splice a `uses` into the tree that
// scoping decisions need to see).

// Forward decl: defined near the other macro helpers (§7), used below by the
// comptime C-early/C-late deferral (a comptime site containing an unexpanded
// macro call can't fold on the pre-imports walk — see walkStmt's Var/If/
// ExprStmt cases and walkExpr).
static const Expr* findMacroCall(const Expr* e);

static bool defaultReferencesParam(const Expr* e,
                                   const std::vector<Param>& params) {
    if (!e) return false;
    if (e->kind == ExprKind::This) return true;
    if (e->kind == ExprKind::Name)
        for (const Param& p : params)
            if (e->text == p.name) return true;
    if (defaultReferencesParam(e->a.get(), params) ||
        defaultReferencesParam(e->b.get(), params) ||
        defaultReferencesParam(e->c.get(), params)) return true;
    for (const ExprPtr& x : e->list)
        if (defaultReferencesParam(x.get(), params)) return true;
    return false;
}

void RuleEngine::foldParamDefaults(std::vector<Param>& params) {
    for (Param& p : params) {
        if (!p.defaultValue || p.defaultFolded) continue;
        if (defaultReferencesParam(p.defaultValue.get(), params)) {
            sink_.error(p.defaultValue->span, "default parameter '" +
                        std::string(p.name) +
                        "' cannot reference another parameter or 'this'");
            p.defaultValue.reset();
            p.defaultFolded = true;
            continue;
        }
        std::string err; bool failed = false;
        Value v = evalComptimeAt(p.defaultValue.get(), err, failed);
        ExprPtr literal;
        if (failed || !reify(v, p.defaultValue->span, literal)) {
            sink_.error(p.defaultValue->span,
                        "a default value must be a compile-time constant");
            p.defaultValue.reset();
            p.defaultFolded = true;
            continue;
        }
        p.defaultValue = std::move(literal);
        p.defaultFolded = true;
        changed_ = true;
    }
}

void RuleEngine::walkItems(std::vector<StmtPtr>& items) {
    for (StmtPtr& item : items) walkStmt(item);
}

void RuleEngine::walkStmt(StmtPtr& slot) {
    if (!slot) return;
    Stmt* s = slot.get();

    switch (s->kind) {
        case StmtKind::Namespace: {
            // Namespace bodies are item-level too (§9 step 3): a `uses`
            // spliced in from a comptime-if here must be as visible as one
            // at the true top level. §8: a namespace-level comptime root is
            // scope-complete against the namespace's scope.
            Scope* saved = comptimeScope_;
            Scope* ns = namespaceScope(std::string(s->name));
            comptimeScope_ = ns ? ns : saved;
            walkTopLevelItems(s->body);
            comptimeScope_ = saved;
            return;
        }
        case StmtKind::Class: {
            Scope* saved = comptimeScope_;
            comptimeScope_ = nullptr;     // §8: member/method bodies carry locals
            walkItems(s->body);           // comptime folding inside members
            comptimeScope_ = saved;
            return;
        }
        case StmtKind::Member: {
            Scope* saved = comptimeScope_;
            comptimeScope_ = nullptr;     // §8: a body has locals
            foldParamDefaults(s->params);
            if (s->memberBody) walkStmt(s->memberBody);
            comptimeScope_ = saved;
            return;
        }
        case StmtKind::Block: {
            Scope* saved = comptimeScope_;
            comptimeScope_ = nullptr;     // §8: a block introduces locals
            walkItems(s->body);
            comptimeScope_ = saved;
            return;
        }
        case StmtKind::Var:
            if (s->isComptime && s->init) {
                // §7×§9 deferral: a macro call inside this init can't fold on
                // the pre-imports (C-early) walk — the oracle has no notion
                // of macros. Leave it untouched (isComptime stays true) until
                // the post-imports (C-late) walk, which expands macros in it
                // first (ordinary structural recursion) and then falls
                // through to fold here, same as any other comptime var.
                if (findMacroCall(s->init.get())) {
                    if (!macroExpansionEnabled_) return;
                    Expr* e = s->init.get();
                    walkExprChildren(e);
                    if (e->kind == ExprKind::Call && e->isMacroCall)
                        expandMacroCall(s->init);
                }
                std::string err; bool failed = false;
                size_t assetsBefore = eval_.importedAssets().size();   // LA-20 §8
                Value v = evalComptimeAt(s->init.get(), err, failed);
                if (failed) {
                    std::string better = precheckComptime(s->init.get());   // §8
                    sink_.error(s->init->span, better.empty()
                                ? "comptime evaluation failed: " + err : better);
                    return;
                }
                ExprPtr lit;
                if (!reify(v, s->init->span, lit)) {
                    sink_.error(s->init->span,
                                "comptime result is not reifiable (got '" +
                                valueToString(v) +
                                "'; folds primitives, strings, arrays, None, and value-"
                                "structs with a constructor matching their fields 1:1 by "
                                "name and order — M28)");
                    return;
                }
                noteImportLiteral(assetsBefore, v, lit->span);   // LA-20 §8
                s->init = std::move(lit);
                changed_ = true;
                // Later comptime sites see the folded value by name.
                eval_.defineGlobal(std::string(s->name), v);
                // Cleared so the §7 macro-expansion re-walk (which reuses this
                // same walk, after Pass C) sees an ordinary Var and doesn't
                // re-evaluate the (now-literal) init a second time — folding
                // a comptime var with a side effect (a compile-time log) must
                // fire exactly once.
                s->isComptime = false;
                return;
            }
            if (s->init) walkExpr(s->init);
            return;
        case StmtKind::If:
            if (s->isComptime) {
                // M29 (§7×§9): a macro call in the CONDITION can never defer
                // like Var/ExprStmt above — the condition feeds the imports
                // map macro resolution itself needs (§9), so deferring it
                // would be exactly the fixpoint this stage forbids. Reported
                // once, during C-early; C-late revisits the same untouched
                // node (still isComptime, since the branch below never runs)
                // and must not re-report the same error a second time.
                if (findMacroCall(s->expr.get())) {
                    if (!macroExpansionEnabled_)
                        sink_.error(s->expr->span,
                            "a macro call is not allowed in a 'comptime if' "
                            "condition (M29): the condition feeds the imports "
                            "map macro resolution itself needs, so deferring "
                            "it would be exactly the fixpoint this stage "
                            "forbids — evaluate the macro call outside the "
                            "condition (e.g. a plain comptime var) instead");
                    return;
                }
                // §16.5 Layer C: the untaken branch is not compiled — the
                // principled replacement for a silent source delete. `uses`
                // inside a branch is fine here (Phase 3 §9 lifted the M15
                // restriction); this NESTED path (inside a body, not item
                // level) keeps the old replace-with-Block shape — a `uses`
                // here is semantically inert either way, same as one inside
                // any ordinary (non-comptime) nested block.
                std::string err; bool failed = false;
                Value cond = evalComptimeAt(s->expr.get(), err, failed);
                if (failed) {
                    std::string better = precheckComptime(s->expr.get());   // §8
                    if (!better.empty()) { sink_.error(s->expr->span, better); return; }
                    sink_.error(s->expr->span, "comptime condition failed: " + err);
                    return;
                }
                if (cond.kind != VKind::Bool) {
                    sink_.error(s->expr->span,
                                "a comptime if condition must be bool (got '" +
                                valueToString(cond) + "')");
                    return;
                }
                StmtPtr taken = cond.b ? std::move(slot->thenBranch)
                                       : std::move(slot->elseBranch);
                SourceSpan sp = s->span;
                if (!taken) {
                    taken = std::make_unique<Stmt>(StmtKind::Empty);
                    taken->span = sp;
                }
                slot = std::move(taken);
                changed_ = true;
                walkStmt(slot);            // the taken branch may nest comptime
                return;
            }
            walkExpr(s->expr);
            walkStmt(s->thenBranch);
            walkStmt(s->elseBranch);
            return;
        case StmtKind::ExprStmt:
            if (s->expr && s->expr->isComptime) {
                // §7×§9 deferral (same shape as the Var case above).
                if (findMacroCall(s->expr.get())) {
                    if (!macroExpansionEnabled_) return;
                    Expr* e = s->expr.get();
                    walkExprChildren(e);
                    if (e->kind == ExprKind::Call && e->isMacroCall)
                        expandMacroCall(s->expr);
                }
                // Evaluate for compile-time effect (console logs), discard.
                std::string err; bool failed = false;
                evalComptimeAt(s->expr.get(), err, failed);
                if (failed) {
                    std::string better = precheckComptime(s->expr.get());   // §8
                    sink_.error(s->expr->span, better.empty()
                                ? "comptime evaluation failed: " + err : better);
                }
                SourceSpan sp = s->span;
                slot = std::make_unique<Stmt>(StmtKind::Empty);
                slot->span = sp;
                changed_ = true;
                return;
            }
            walkExpr(s->expr);
            return;
        case StmtKind::Return: case StmtKind::Throw:
            walkExpr(s->expr);
            return;
        case StmtKind::While:
        case StmtKind::DoWhile:
            walkExpr(s->expr);
            walkStmt(s->thenBranch);
            return;
        case StmtKind::For:
            walkStmt(s->forInit);
            walkExpr(s->expr);
            walkExpr(s->forStep);
            walkStmt(s->thenBranch);
            return;
        case StmtKind::ForIn:
            walkExpr(s->expr);
            walkStmt(s->thenBranch);
            return;
        case StmtKind::Try:
            walkStmt(s->thenBranch);
            for (CatchClause& c : s->catches) walkStmt(c.body);
            return;
        case StmtKind::Bind:
            if (s->memberBody) walkStmt(s->memberBody);
            walkExpr(s->init);
            return;
        default:
            return;
    }
}

// Shared structural descent (a/b/c/list/block/arms) used by walkExpr's
// ordinary path AND the C-late deferred-comptime path (which needs the SAME
// descent, without going through foldExpr, to expand macro calls hiding
// inside a comptime subtree before folding it).
void RuleEngine::walkExprChildren(Expr* e) {
    walkExpr(e->a);
    walkExpr(e->b);
    walkExpr(e->c);
    for (ExprPtr& x : e->list) walkExpr(x);
    if (e->block) walkStmt(e->block);
    for (MatchArm& arm : e->arms) {
        walkExpr(arm.value);
        walkExpr(arm.bodyExpr);
        if (arm.bodyBlock) walkStmt(arm.bodyBlock);
    }
}

void RuleEngine::walkExpr(ExprPtr& slot) {
    if (!slot) return;
    if (slot->isComptime) {
        // §7×§9 deferral (§7's "§7×§9 interaction"): a comptime subtree
        // containing an unexpanded macro call can't fold — the oracle has no
        // notion of macros. C-early (pre-imports) leaves it fully untouched;
        // C-late (macroExpansionEnabled_, post-imports) expands macros in it
        // first via the same structural descent ordinary code gets, then
        // folds. (A macro-free comptime site is unaffected either way.)
        if (findMacroCall(slot.get())) {
            if (!macroExpansionEnabled_) return;
            Expr* e = slot.get();
            walkExprChildren(e);
            // The comptime node itself may BE the macro call (not just
            // contain one), e.g. `comptime string s = safeStrip!(x);` — the
            // descent above only reaches its ARGUMENTS, so check the node
            // too, same as the ordinary (non-comptime) path below does.
            if (e->kind == ExprKind::Call && e->isMacroCall) expandMacroCall(slot);
        }
        foldExpr(slot);
        return;
    }
    walkExprChildren(slot.get());
    // Expression macros (§7): expand AFTER children, so a macro call nested in
    // an argument (`outer!(inner!(x))`) expands bottom-up in one pass — no
    // separate re-entry handling needed for that shape; M24 only guards
    // against the OTHER shape (a macro's own template calling a macro).
    // Gated on macroExpansionEnabled_ — see run()'s step D comment.
    if (macroExpansionEnabled_ && slot->kind == ExprKind::Call && slot->isMacroCall)
        expandMacroCall(slot);
}

// `comptime <expr>` -> evaluate on the oracle, reify the value as a literal in
// place. The literal carries the original expression's span, so pass-2 type
// errors point at the comptime site the author wrote.
void RuleEngine::foldExpr(ExprPtr& slot) {
    std::string err;
    bool failed = false;
    size_t assetsBefore = eval_.importedAssets().size();   // LA-20 §8
    Value v = evalComptimeAt(slot.get(), err, failed);
    if (failed) {
        std::string better = precheckComptime(slot.get());   // §8
        sink_.error(slot->span, better.empty()
                    ? "comptime evaluation failed: " + err : better);
        return;
    }
    ExprPtr lit;
    if (!reify(v, slot->span, lit)) {
        sink_.error(slot->span,
                    "comptime result is not reifiable (got '" + valueToString(v) +
                    "'; folds primitives, strings, arrays, None, and value-structs "
                    "with a constructor matching their fields 1:1 by name and "
                    "order — M28)");
        return;
    }
    noteImportLiteral(assetsBefore, v, lit->span);   // LA-20 §8
    slot = std::move(lit);
    changed_ = true;
}

bool RuleEngine::reify(const Value& v, SourceSpan span, ExprPtr& out) {
    auto mk = [&](ExprKind k) {
        auto e = std::make_unique<Expr>(k);
        e->span = span;
        return e;
    };
    switch (v.kind) {
        case VKind::Int: {
            out = mk(ExprKind::IntLit);
            out->text = own(std::to_string(v.i));
            return true;
        }
        case VKind::Float: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.17g", v.f);
            std::string t(buf);
            // FloatLit must look like one to the reader and the lexer.
            if (t.find('.') == std::string::npos &&
                t.find('e') == std::string::npos) t += ".0";
            out = mk(ExprKind::FloatLit);
            out->text = own(t);
            return true;
        }
        case VKind::Bool: {
            out = mk(ExprKind::BoolLit);
            out->text = v.b ? "true" : "false";
            return true;
        }
        case VKind::String: {
            std::string q = "\"";
            for (char c : v.s) {
                switch (c) {
                    case '"':  q += "\\\""; break;
                    case '\\': q += "\\\\"; break;
                    case '\n': q += "\\n";  break;
                    case '\t': q += "\\t";  break;
                    case '\r': q += "\\r";  break;
                    default:   q += c;      break;
                }
            }
            q += "\"";
            out = mk(ExprKind::StringLit);
            out->text = own(std::move(q));
            return true;
        }
        case VKind::None: {
            out = mk(ExprKind::Name);
            out->text = "None";
            return true;
        }
        case VKind::Array: {
            out = mk(ExprKind::Array);
            if (v.arr)
                for (const Value& el : *v.arr) {
                    ExprPtr le;
                    if (!reify(el, span, le)) return false;
                    out->list.push_back(std::move(le));
                }
            return true;
        }
        case VKind::Object: {
            // §11 (stretch): a value struct reifies as a constructor call —
            // there is no object-literal syntax, so `ClassName(field₁, …)` is
            // the only source shape that means "this value". Reference
            // classes stay non-reifiable permanently (identity has no
            // compile-time meaning); a value struct without a constructor
            // whose params match its fields 1:1 by name and order is M28 —
            // give it one, or return primitives/arrays/other structs instead.
            if (!v.obj || !v.obj->cls || !v.obj->cls->isValue || !v.obj->cls->decl)
                return false;
            Symbol* cls = v.obj->cls;
            std::vector<std::string_view> fieldNames;
            for (const StmtPtr& m : cls->decl->body)
                if (m->kind == StmtKind::Member && !m->callable) fieldNames.push_back(m->name);
            const Stmt* ctor = nullptr;
            for (const StmtPtr& m : cls->decl->body) {
                if (m->kind != StmtKind::Member || !m->isCtor) continue;
                if (m->params.size() != fieldNames.size()) continue;
                bool matches = true;
                for (size_t i = 0; i < fieldNames.size(); ++i)
                    if (m->params[i].name != fieldNames[i]) { matches = false; break; }
                if (matches) { ctor = m.get(); break; }
            }
            if (!ctor) return false;
            out = mk(ExprKind::Call);
            auto callee = mk(ExprKind::Name);
            callee->text = own(std::string(cls->name));
            out->a = std::move(callee);
            for (std::string_view fname : fieldNames) {
                auto fit = v.obj->fields.find(std::string(fname));
                Value fv = fit != v.obj->fields.end() ? fit->second : vvoid();
                ExprPtr fe;
                if (!reify(fv, span, fe)) return false;   // nested struct/M28 recurses
                out->list.push_back(std::move(fe));
            }
            return true;
        }
        default:
            return false;   // closures / maps: not reifiable (no source form)
    }
}

// --- expression macros (§7) --------------------------------------------------

// M22, static: count every bare-Name reference to `$param` inside an expr
// tree. Mirrors cloneExpr's/cloneStmt's own descent (a/b/c/list/arms/block)
// so it flags exactly what the splice mechanism could actually double-splice.
static void countHoleRefsStmt(const Stmt* s, std::string_view hole, int& n);
static void countHoleRefsExpr(const Expr* e, std::string_view hole, int& n) {
    if (!e) return;
    if (e->kind == ExprKind::Name && e->text == hole) ++n;
    countHoleRefsExpr(e->a.get(), hole, n);
    countHoleRefsExpr(e->b.get(), hole, n);
    countHoleRefsExpr(e->c.get(), hole, n);
    for (const ExprPtr& x : e->list) countHoleRefsExpr(x.get(), hole, n);
    if (e->block) countHoleRefsStmt(e->block.get(), hole, n);
    for (const MatchArm& arm : e->arms) {
        countHoleRefsExpr(arm.value.get(), hole, n);
        countHoleRefsExpr(arm.bodyExpr.get(), hole, n);
        countHoleRefsStmt(arm.bodyBlock.get(), hole, n);
    }
}
static void countHoleRefsStmt(const Stmt* s, std::string_view hole, int& n) {
    if (!s) return;
    countHoleRefsExpr(s->expr.get(), hole, n);
    countHoleRefsExpr(s->init.get(), hole, n);
    countHoleRefsExpr(s->forStep.get(), hole, n);
    countHoleRefsStmt(s->memberBody.get(), hole, n);
    countHoleRefsStmt(s->thenBranch.get(), hole, n);
    countHoleRefsStmt(s->elseBranch.get(), hole, n);
    countHoleRefsStmt(s->forInit.get(), hole, n);
    for (const StmtPtr& x : s->body) countHoleRefsStmt(x.get(), hole, n);
    for (const CatchClause& c : s->catches) countHoleRefsStmt(c.body.get(), hole, n);
}

void RuleEngine::validateMacroDecls() {
    for (const OwnedRule& r : rules_) {
        if (!r.node->isMacroDecl) continue;
        if (r.node->isProceduralMacro) continue;
        if (r.node->ruleActions.empty() || !r.node->ruleActions[0].tmplExpr) continue;
        const Expr* tmpl = r.node->ruleActions[0].tmplExpr.get();
        for (std::string_view param : r.node->generics) {
            std::string hole = "$" + std::string(param);
            int n = 0;
            countHoleRefsExpr(tmpl, hole, n);
            if (n > 1)
                sink_.error(r.node->span, "macro '" + std::string(r.node->name) +
                            "' splices parameter '" + std::string(param) +
                            "' more than once in its template (" +
                            std::to_string(n) + " uses) — bind it to a local "
                            "instead of splicing the argument expression twice");
        }
    }
}

// Layer D (§2.3): static checks on `rewrites`/`generates` rules at collection
// time, run once per rule independent of whether it ever fires — M35 (the
// target bind names a callable match bind), M32 (a `rewrites` rule's `$body`
// is referenced exactly once across its replace templates: zero drops the
// original body silently, more than once duplicates its side effects), and
// M36 (a `generates` rule's replace templates never reference `$body` — there
// is no original to splice; bindgen metaprog scope design).
void RuleEngine::validateRewriteRules() {
    for (const OwnedRule& r : rules_) {
        bool generates = r.node->ruleGenerates;
        if ((!r.node->ruleRewrites && !generates) || !r.node->ruleMatch) continue;
        const RuleMatch& m = *r.node->ruleMatch;
        const char* verb = generates ? "generates" : "rewrites";

        // M35: the target bind must name a callable match bind (subject or an
        // encloser of method/function kind). A field/class/struct bind has no
        // body to replace.
        std::string_view tgt = r.node->rewritesTarget;
        auto isCallableKind = [](std::string_view kw) {
            return kw == "method" || kw == "function";
        };
        bool found = false, callable = false;
        if (tgt == m.subject.bind) { found = true; callable = isCallableKind(m.subject.kindWord); }
        for (const RuleLevel& lv : m.enclosers)
            if (tgt == lv.bind) { found = true; callable = isCallableKind(lv.kindWord); }
        if (!found)
            sink_.error(r.node->span, "rule '" + std::string(r.node->name) +
                        "' " + verb + " body of '" + std::string(tgt) +
                        "', which is not a name bound by its match clause");
        else if (!callable)
            sink_.error(r.node->span, "rule '" + std::string(r.node->name) +
                        "' " + verb + " body of '" + std::string(tgt) +
                        "', which is not a callable (only a method/function has a "
                        "body to replace)");   // M35

        // M32 (`rewrites`): exactly one `$body` across every replace template.
        // M36 (`generates`): `$body` must never appear — there is no original.
        int n = 0;
        for (const RuleAction& act : r.node->ruleActions) {
            if (act.anchor != AnchorKind::BodyReplace && act.anchor != AnchorKind::BodyGenerate)
                continue;
            for (const StmtPtr& s : act.tmplStmts) countHoleRefsStmt(s.get(), "$body", n);
        }
        if (generates) {
            if (n > 0)
                sink_.error(r.node->span, "rule '" + std::string(r.node->name) +
                            "' is a 'generates' rule — the original body is "
                            "discarded, so '$body' is unavailable; use 'rewrites' "
                            "to keep it");   // M36
        } else if (n == 0) {
            sink_.error(r.node->span, "rule '" + std::string(r.node->name) +
                        "' never references '$body' — a 'replace' that drops the "
                        "original body is silent obliteration; splice '$body' to "
                        "keep it");   // M32
        } else if (n > 1) {
            sink_.error(r.node->span, "rule '" + std::string(r.node->name) +
                        "' references '$body' " + std::to_string(n) + " times — a "
                        "'rewrites' rule may splice the original body at most once "
                        "(splicing it twice duplicates its side effects)");   // M32
        }
    }
}

// M24: does `e` still contain a macro-call Call node anywhere? A nested call
// in the ORIGINAL call-site arguments is already expanded bottom-up by the
// walk before expandMacroCall ever runs (walkExpr recurses into children
// first) — this only fires for the other re-entry shape, a macro's own
// template invoking a macro.
static const Expr* findMacroCall(const Expr* e);
static const Expr* findMacroCallStmt(const Stmt* s) {
    if (!s) return nullptr;
    if (const Expr* f = findMacroCall(s->expr.get())) return f;
    if (const Expr* f = findMacroCall(s->init.get())) return f;
    if (const Expr* f = findMacroCall(s->forStep.get())) return f;
    if (const Expr* f = findMacroCallStmt(s->memberBody.get())) return f;
    if (const Expr* f = findMacroCallStmt(s->thenBranch.get())) return f;
    if (const Expr* f = findMacroCallStmt(s->elseBranch.get())) return f;
    if (const Expr* f = findMacroCallStmt(s->forInit.get())) return f;
    for (const StmtPtr& x : s->body)
        if (const Expr* f = findMacroCallStmt(x.get())) return f;
    for (const CatchClause& c : s->catches)
        if (const Expr* f = findMacroCallStmt(c.body.get())) return f;
    return nullptr;
}
static const Expr* findMacroCall(const Expr* e) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::Call && e->isMacroCall) return e;
    if (const Expr* f = findMacroCall(e->a.get())) return f;
    if (const Expr* f = findMacroCall(e->b.get())) return f;
    if (const Expr* f = findMacroCall(e->c.get())) return f;
    for (const ExprPtr& x : e->list) if (const Expr* f = findMacroCall(x.get())) return f;
    if (const Expr* f = findMacroCallStmt(e->block.get())) return f;
    for (const MatchArm& arm : e->arms) {
        if (const Expr* f = findMacroCall(arm.value.get())) return f;
        if (const Expr* f = findMacroCall(arm.bodyExpr.get())) return f;
    }
    return nullptr;
}

static void restampStmt(Stmt* s, SourceSpan span);
static void restampExpr(Expr* e, SourceSpan span) {
    if (!e) return;
    e->span = span;
    restampExpr(e->a.get(), span); restampExpr(e->b.get(), span); restampExpr(e->c.get(), span);
    for (ExprPtr& x : e->list) restampExpr(x.get(), span);
    restampStmt(e->block.get(), span);
    for (Param& p : e->params) { p.span = span; restampExpr(p.defaultValue.get(), span); }
    for (MatchArm& arm : e->arms) {
        arm.span = span; restampExpr(arm.value.get(), span);
        restampExpr(arm.bodyExpr.get(), span); restampStmt(arm.bodyBlock.get(), span);
    }
}
static void restampStmt(Stmt* s, SourceSpan span) {
    if (!s) return;
    s->span = span;
    restampExpr(s->expr.get(), span); restampExpr(s->init.get(), span);
    restampExpr(s->forStep.get(), span);
    restampStmt(s->memberBody.get(), span); restampStmt(s->thenBranch.get(), span);
    restampStmt(s->elseBranch.get(), span); restampStmt(s->forInit.get(), span);
    for (StmtPtr& x : s->body) restampStmt(x.get(), span);
    for (Param& p : s->params) { p.span = span; restampExpr(p.defaultValue.get(), span); }
    for (CatchClause& c : s->catches) restampStmt(c.body.get(), span);
    for (AttrUse& a : s->attrs) {
        a.span = span; for (ExprPtr& x : a.args) restampExpr(x.get(), span);
    }
}

void RuleEngine::expandMacroCall(ExprPtr& slot) {
    Expr* call = slot.get();
    Expr* callee = call->a.get();
    std::string_view name;
    std::string qualNs;
    bool qualified = false;
    if (callee->kind == ExprKind::Name) {
        name = callee->text;
    } else if (callee->kind == ExprKind::Member && callee->colon && callee->a &&
               callee->a->kind == ExprKind::Name) {
        // `NS::name!(args)` — an explicit qualifier pins the namespace directly.
        name = callee->text;
        qualNs = std::string(callee->a->text);
        qualified = true;
    } else {
        sink_.error(call->span, "a macro call target must be a name or NS::name");
        return;
    }

    // M23: unknown / ambiguous, same scoping and message shape as attributes'
    // M01/M02 (a call site sees macros of namespaces its file imports).
    std::vector<const OwnedRule*> hits;
    if (qualified) {
        for (const OwnedRule& r : rules_)
            if (r.node->isMacroDecl && r.node->name == name && r.ns == qualNs)
                hits.push_back(&r);
    } else {
        int fi = fileOf(call->span);
        for (const OwnedRule& r : rules_) {
            if (!r.node->isMacroDecl || r.node->name != name) continue;
            bool visible = (fi >= 0 && fi < (int)imports_.size())
                                ? imports_[fi].effective.count(r.ns) > 0
                                : r.ns == "<root>";
            if (visible) hits.push_back(&r);
        }
    }
    if (hits.empty()) {
        sink_.error(call->span, "unknown macro '" + std::string(name) +
                    "!' (is a 'uses' missing?)");
        return;
    }
    if (hits.size() > 1) {
        std::string list;
        for (const OwnedRule* r : hits) {
            if (!list.empty()) list += ", ";
            list += (r->ns == "<root>" ? std::string(name)
                                       : r->ns + "::" + std::string(name));
        }
        sink_.error(call->span, "macro '" + std::string(name) +
                    "!' is ambiguous (" + list + ") — qualify it");
        return;
    }

    const OwnedRule& macro = *hits.front();
    const std::vector<std::string_view>& params = macro.node->generics;
    if (call->list.size() != params.size()) {
        sink_.error(call->span, "macro '" + std::string(name) + "!' takes " +
                    std::to_string(params.size()) + " argument(s); got " +
                    std::to_string(call->list.size()));
        return;
    }

    if (macro.node->isProceduralMacro) {
        if (params.size() != 1 || !macro.node->memberBody) return;
        std::string evalErr;
        bool failed = false;
        Value arg = evalComptimeAt(call->list[0].get(), evalErr, failed);
        if (failed) {
            sink_.error(call->span, "procedural macro '" + std::string(name) +
                        "' argument is not comptime-evaluable: " + evalErr);
            return;
        }
        if (arg.kind != VKind::String) {
            sink_.error(call->span, "procedural macro '" + std::string(name) +
                        "' argument must evaluate to string (got '" +
                        valueToString(arg) + "')");
            return;
        }
        std::unordered_map<std::string, Value> locals;
        locals[std::string(params[0])] = arg;
        eval_.setImportModule(moduleOf(call->span));
        Value result = eval_.evalComptimeBody(name, params[0], macro.node->memberBody.get(), locals,
                                              evalErr, failed);
        if (failed) {
            bool direct = evalErr.find("comptime step budget exceeded") != std::string::npos ||
                          evalErr.rfind("meta::parse", 0) == 0;
            std::string prefix = direct
                                     ? "procedural macro '" + std::string(name) + "': "
                                     : "procedural macro '" + std::string(name) +
                                           "' body threw: ";
            sink_.error(call->span, prefix + evalErr);
            return;
        }
        if (result.kind != VKind::Ast || !result.ast) {
            SourceSpan retSpan = eval_.comptimeReturnSpan();
            if (retSpan.length == 0) retSpan = macro.node->memberBody->span;
            sink_.error(retSpan,
                        "procedural macro '" + std::string(name) +
                        "' must return Ast (got '" + valueToString(result) + "')");
            return;
        }
        if (result.ast->kind != AstFragmentKind::Expr || !result.ast->expr) {
            sink_.error(call->span, "procedural macro '" + std::string(name) +
                        "' returned Ast(stmts) where an expression is required");
            return;
        }

        Bindings empty;
        std::string savedNs = curRuleNs_;
        curRuleNs_ = macro.ns;
        bool cloneErr = false;
        ExprPtr expanded = cloneExpr(result.ast->expr.get(), empty, cloneErr);
        curRuleNs_ = savedNs;
        if (cloneErr || !expanded) return;
        restampExpr(expanded.get(), call->span);
        if (findMacroCall(expanded.get())) {
            sink_.error(call->span, "macro call in generated code — procedural macro '" +
                        std::string(name) + "' may not re-enter macro expansion (M24)");
            return;
        }
        changed_ = true;
        slot = std::move(expanded);
        return;
    }
    if (macro.node->ruleActions.empty() || !macro.node->ruleActions[0].tmplExpr) return;

    // Each argument binds as an expression subtree (not an evaluated value) —
    // spliced verbatim where `$param` appears, via cloneExpr's exprNode path.
    Bindings mb;
    for (size_t i = 0; i < params.size(); ++i) {
        Binding pb;
        pb.exprNode = call->list[i].get();
        mb[params[i]] = pb;
    }

    std::string savedNs = curRuleNs_;
    curRuleNs_ = macro.ns;   // §10: def-site qualification during this clone
    bool err = false;
    ExprPtr expanded = cloneExpr(macro.node->ruleActions[0].tmplExpr.get(), mb, err);
    curRuleNs_ = savedNs;
    if (err || !expanded) return;

    if (const Expr* inner = findMacroCall(expanded.get())) {
        LineCol oc = lineColAt(file_.text, call->span.offset);
        sink_.error(inner->span, "macro '" + std::string(name) +
                    "' expands to another macro call — no re-entry (M24); "
                    "outer call at " + std::to_string(oc.line) + ":" +
                    std::to_string(oc.col));
        return;
    }

    changed_ = true;
    slot = std::move(expanded);
}

// --- item-level fold (§9 step 3): splice, don't nest --------------------------
//
// `program.items` and namespace bodies are walked with this pair instead of
// plain walkItems/walkStmt: a folded comptime-if's taken Block splices its
// statements straight into the parent vector (recursively, so a `uses` or a
// nested comptime-if inside it still lands where computeFileImports and
// pass-2's processImports look) rather than surviving as a nested Block.

void RuleEngine::walkTopLevelItems(std::vector<StmtPtr>& items) {
    std::vector<StmtPtr> out;
    out.reserve(items.size());
    for (StmtPtr& item : items) foldTopLevelItem(std::move(item), out);
    items = std::move(out);
}

void RuleEngine::foldTopLevelItem(StmtPtr item, std::vector<StmtPtr>& out) {
    if (!item) return;
    if (item->kind == StmtKind::If && item->isComptime) {
        Stmt* s = item.get();
        // M29 (§7×§9): same hard-error stance as walkStmt's nested If case —
        // an item-level comptime-if's condition cannot contain a macro call
        // either (it feeds the imports map macro resolution needs). The
        // `!macroExpansionEnabled_` guard mirrors the nested case for safety,
        // though here it's not load-bearing against a double-report: on
        // error the item is dropped (not pushed to `out`), so it never
        // survives into the rebuilt tree the C-late walk sees.
        if (findMacroCall(s->expr.get())) {
            if (!macroExpansionEnabled_)
                sink_.error(s->expr->span,
                    "a macro call is not allowed in a 'comptime if' condition "
                    "(M29): the condition feeds the imports map macro "
                    "resolution itself needs, so deferring it would be "
                    "exactly the fixpoint this stage forbids — evaluate the "
                    "macro call outside the condition (e.g. a plain comptime "
                    "var) instead");
            return;
        }
        std::string err; bool failed = false;
        Value cond = evalComptimeAt(s->expr.get(), err, failed);
        if (failed) {
            std::string better = precheckComptime(s->expr.get());   // §8
            sink_.error(s->expr->span, better.empty()
                        ? "comptime condition failed: " + err : better);
            return;
        }
        if (cond.kind != VKind::Bool) {
            sink_.error(s->expr->span, "a comptime if condition must be bool (got '" +
                        valueToString(cond) + "')");
            return;
        }
        changed_ = true;
        StmtPtr taken = cond.b ? std::move(s->thenBranch) : std::move(s->elseBranch);
        if (!taken) return;                      // untaken, no else: contributes nothing
        if (taken->kind == StmtKind::Block) {
            // Splice each statement individually — re-processed, so a nested
            // comptime-if (or `comptime` var) inside it folds too.
            for (StmtPtr& t : taken->body) foldTopLevelItem(std::move(t), out);
        } else {
            foldTopLevelItem(std::move(taken), out);
        }
        return;
    }
    walkStmt(item);            // ordinary comptime folding (Vars, nested Ifs, ...)
    out.push_back(std::move(item));
}

// --- attributes (Layer A) — a SEPARATE walk from comptime folding (§9) -------

void RuleEngine::walkAttrs(std::vector<StmtPtr>& items) {
    for (StmtPtr& item : items) {
        if (!item) continue;
        Stmt* s = item.get();
        if (!s->attrs.empty()) processAttrs(s);
        if (s->kind == StmtKind::Namespace) {
            walkAttrs(s->body);
        } else if (s->kind == StmtKind::Class) {
            if (s->isAttribute) validateAttributeDecl(s);
            walkAttrs(s->body);      // members can carry attributes too
        }
        // Members/vars/etc. have no nested declarations that can carry
        // attributes (attributes are declaration-position only, and bodies
        // don't declare further attributable members) — no deeper recursion.
    }
}

// --- attributes (Layer A), continued ------------------------------------------

// v1: attribute fields must be primitives — they are the typed argument
// surface, and primitives are what reify (§13 open q.6 taken conservatively).
void RuleEngine::validateAttributeDecl(Stmt* cls) {
    for (const StmtPtr& m : cls->body) {
        if (m->kind != StmtKind::Member || m->callable) continue;
        const std::string& canon = m->type ? m->type->canonical : std::string();
        if (canon != "int" && canon != "string" && canon != "bool" && canon != "float")
            sink_.error(m->span, "attribute field '" + std::string(m->name) +
                                 "' must be int, float, bool, or string (got '" +
                                 (canon.empty() ? "?" : canon) + "')");
    }
}

void RuleEngine::processAttrs(Stmt* decl) {
    int fi = fileIdxFor(decl->span);
    for (AttrUse& a : decl->attrs) {
        Symbol* cls = resolveAttr(a, fi);
        if (!cls) continue;
        a.resolved = cls;
        evalAttrArgs(a, cls);
    }
}

// M01/M02: an unqualified attribute name resolves through the namespaces the
// declaring file imports (its `uses` list + own namespaces + std, P-4);
// a qualified `@NS::Name` goes straight to that namespace.
Symbol* RuleEngine::resolveAttr(AttrUse& a, int fileIdx) {
    auto attrIn = [&](Scope* sc) -> Symbol* {
        if (!sc) return nullptr;
        if (const std::vector<Symbol*>* v = sc->localLookup(a.name))
            for (Symbol* s : *v)
                if (s->kind == SymbolKind::Class && s->decl && s->decl->isAttribute)
                    return s;
        return nullptr;
    };

    if (!a.path.empty()) {
        Scope* sc = sema_.global;
        for (std::string_view seg : a.path) {
            Symbol* ns = nullptr;
            if (const std::vector<Symbol*>* v = sc->localLookup(seg))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Namespace) { ns = s; break; }
            if (!ns || !ns->scope) {
                sink_.error(a.span, "unknown namespace '" + std::string(seg) +
                                    "' in attribute qualifier");
                return nullptr;
            }
            sc = ns->scope;
        }
        if (Symbol* s = attrIn(sc)) return s;
        sink_.error(a.span, "no attribute '" + std::string(a.name) +
                            "' in that namespace");
        return nullptr;
    }

    // Dedupe by symbol identity: `uses` copies imported names into the using
    // scope, so one attribute is reachable both as <root>::X and NS::X — that
    // is one attribute, not an ambiguity.
    std::vector<std::pair<std::string, Symbol*>> hits;
    auto addHit = [&](const std::string& ns, Symbol* s) {
        if (!s) return;
        for (auto& [n, prev] : hits)
            if (prev == s) {
                if (n == "<root>" && ns != "<root>") n = ns;  // prefer the real home
                return;
            }
        hits.push_back({ns, s});
    };
    if (fileIdx >= 0 && fileIdx < (int)imports_.size()) {
        for (const std::string& ns : imports_[fileIdx].effective)
            addHit(ns, attrIn(namespaceScope(ns)));
    } else {
        addHit("<root>", attrIn(sema_.global));
    }

    if (hits.empty()) {
        sink_.error(a.span, "no attribute '" + std::string(a.name) +
                            "' in scope (is a 'uses' missing?)");
        return nullptr;
    }
    if (hits.size() > 1) {
        std::string list;
        for (auto& [ns, s] : hits) {
            if (!list.empty()) list += ", ";
            list += (ns == "<root>" ? std::string(a.name)
                                    : ns + "::" + std::string(a.name));
        }
        sink_.error(a.span, "attribute '@" + std::string(a.name) +
                            "' is ambiguous (" + list + ") — qualify it");
        return nullptr;
    }
    return hits.front().second;
}

// M03/M04: positional args against the attribute's fields (declaration order);
// trailing fields with initializers are defaultable. Values are evaluated on
// the hermetic oracle and kept for the Phase-2 rules.
void RuleEngine::evalAttrArgs(AttrUse& a, Symbol* attrClass) {
    std::vector<const Stmt*> fields;
    for (const StmtPtr& m : attrClass->decl->body)
        if (m->kind == StmtKind::Member && !m->callable) fields.push_back(m.get());

    if (a.args.size() > fields.size()) {
        sink_.error(a.span, "'@" + std::string(a.name) + "' takes at most " +
                            std::to_string(fields.size()) + " argument(s); got " +
                            std::to_string(a.args.size()));
        return;
    }

    std::vector<Expr*> mapped(fields.size(), nullptr);
    size_t positional = 0;
    for (const ExprPtr& arg : a.args) {
        size_t fi = fields.size();
        if (arg->argLabel.empty()) {
            fi = positional++;
            if (fi >= fields.size()) return;
        } else {
            for (size_t i = 0; i < fields.size(); ++i)
                if (fields[i]->name == arg->argLabel) { fi = i; break; }
            if (fi == fields.size()) {
                sink_.error(arg->span, "no parameter named '" +
                            std::string(arg->argLabel) + "'");
                return;
            }
            if (mapped[fi]) {
                sink_.error(arg->span, "parameter '" +
                            std::string(arg->argLabel) +
                            "' is bound both positionally and by name");
                return;
            }
        }
        mapped[fi] = arg.get();
    }

    AttrValue values;
    for (size_t i = 0; i < fields.size(); ++i) {
        const Stmt* f = fields[i];
        Value v;
        if (mapped[i]) {
            std::string err; bool failed = false;
            v = evalComptimeAt(mapped[i], err, failed);
            if (failed) {
                sink_.error(mapped[i]->span,
                            "attribute argument is not comptime-evaluable: " + err);
                return;
            }
            const std::string& canon = f->type ? f->type->canonical : std::string();
            bool okType = (canon == "int"    && v.kind == VKind::Int) ||
                          (canon == "float"  && v.kind == VKind::Float) ||
                          (canon == "bool"   && v.kind == VKind::Bool) ||
                          (canon == "string" && v.kind == VKind::String);
            if (!okType) {
                sink_.error(mapped[i]->span,
                            "'@" + std::string(a.name) + "' field '" +
                            std::string(f->name) + "' is " +
                            (canon.empty() ? "?" : canon) + "; got '" +
                            valueToString(v) + "'");
                return;
            }
        } else if (f->init) {
            std::string err; bool failed = false;
            v = evalComptimeAt(f->init.get(), err, failed);
            if (failed) v = vvoid();
        } else {
            sink_.error(a.span, "'@" + std::string(a.name) + "' is missing '" +
                                std::string(f->name) + "' (field " +
                                std::to_string(i + 1) + ", no default)");
            return;
        }
        values.push_back({std::string(f->name), v, mapped[i] != nullptr});
    }
    attrValues_[&a] = std::move(values);
}

// =============================================================================
//  Layer B — rules: collect, index, match, expand (§5)
// =============================================================================

static bool isHole(std::string_view t) { return !t.empty() && t[0] == '$'; }

// Look up a class symbol by name in a namespace scope (for the :IFace check).
static Symbol* lookupClassIn(Scope* sc, std::string_view name) {
    if (!sc) return nullptr;
    if (const std::vector<Symbol*>* v = sc->localLookup(name))
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Class) return s;
    return nullptr;
}

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
            Symbol* csym = lookupClassIn(namespaceScope(ns), s->name);
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
    if (Symbol* s = lookupClassIn(namespaceScope(ns), t->name)) return s;
    return lookupClassIn(sema_.global, t->name);
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

// A short, human-readable word for a declaration's kind, for diagnostics
// (M25/M26/M27) — derived directly from the Stmt's own flags.
static const char* declKindWord(const Stmt* s) {
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

// --- meta.* reflection + `where` (§3, §4) ------------------------------------

Symbol* RuleEngine::metaClassSymbol(const char* name) const {
    return lookupClassIn(namespaceScope("meta"), name);
}

// Builds the meta.{Class,Method,Field} reflection Object for a declaration,
// lazily and cached (most rules never use `where`, so this is usually never
// called at all). Namespaces and anything else meta.* doesn't model reify to
// void — there is no Decl supertype until demanded (§3).
Value RuleEngine::buildMetaValue(Stmt* decl) {
    if (!decl) return vvoid();
    auto cached = metaCache_.find(decl);
    if (cached != metaCache_.end()) return cached->second;

    Value v;
    if (decl->kind == StmtKind::Class) {
        Symbol* cls = metaClassSymbol("Class");
        if (!cls) return vvoid();
        v.kind = VKind::Object;
        v.obj = std::make_shared<Object>();
        v.obj->cls = cls;
        v.obj->fields["name"] = vstr(std::string(decl->name));

        Value bases; bases.kind = VKind::Array;
        bases.arr = std::make_shared<std::vector<Value>>();
        for (const TypeRefPtr& base : decl->bases)
            if (base) bases.arr->push_back(vstr(base->canonical));
        v.obj->fields["bases"] = bases;

        Value fields; fields.kind = VKind::Array;
        fields.arr = std::make_shared<std::vector<Value>>();
        Value methods; methods.kind = VKind::Array;
        methods.arr = std::make_shared<std::vector<Value>>();
        for (const StmtPtr& m : decl->body) {
            if (m->kind != StmtKind::Member) continue;
            if (m->callable) methods.arr->push_back(buildMetaValue(m.get()));
            else fields.arr->push_back(buildMetaValue(m.get()));
        }
        v.obj->fields["fields"] = fields;
        v.obj->fields["methods"] = methods;
    } else if (decl->kind == StmtKind::Member) {
        // Every callable member (method/function/ctor/accessor/operator)
        // reflects uniformly as a meta::Method; every non-callable member as
        // a meta::Field — the same "member is a typed slot, some executable"
        // rule the rest of the language already follows (info.md §6.5).
        Symbol* cls = metaClassSymbol(decl->callable ? "Method" : "Field");
        if (!cls) return vvoid();
        v.kind = VKind::Object;
        v.obj = std::make_shared<Object>();
        v.obj->cls = cls;
        v.obj->fields["name"] = vstr(std::string(decl->name));

        Value attrs; attrs.kind = VKind::Array;
        attrs.arr = std::make_shared<std::vector<Value>>();
        for (const AttrUse& a : decl->attrs)
            if (a.resolved) attrs.arr->push_back(vstr(std::string(a.resolved->name)));
        v.obj->fields["attrs"] = attrs;

        // Item A (LA-4, P4-I): the same attributes, but carrying their reified
        // argument values (attrValues_, filled in walkAttrs before we run) — so
        // `$for m in C.methods` can read `m.attr("op").argStr(0)`, not just the
        // name. Positional, in attribute-field declaration order. Each AttrArg
        // fills every primitive slot; the typed accessor picks the right one.
        Symbol* attrCls = metaClassSymbol("Attr");
        Symbol* argCls  = metaClassSymbol("AttrArg");
        Value attributes; attributes.kind = VKind::Array;
        attributes.arr = std::make_shared<std::vector<Value>>();
        for (const AttrUse& a : decl->attrs) {
            if (!a.resolved || !attrCls || !argCls) continue;
            Value av; av.kind = VKind::Object;
            av.obj = std::make_shared<Object>();
            av.obj->cls = attrCls;
            av.obj->fields["name"] = vstr(std::string(a.resolved->name));
            Value args; args.kind = VKind::Array;
            args.arr = std::make_shared<std::vector<Value>>();
            auto it = attrValues_.find(&a);
            if (it != attrValues_.end())
                for (const auto& av : it->second) {
                    const Value& fval = av.val;
                    Value arg; arg.kind = VKind::Object;
                    arg.obj = std::make_shared<Object>();
                    arg.obj->cls = argCls;
                    arg.obj->fields["s"] = vstr(fval.kind == VKind::String ? fval.s : std::string());
                    arg.obj->fields["i"] = vint (fval.kind == VKind::Int   ? fval.i : 0);
                    arg.obj->fields["b"] = vbool(fval.kind == VKind::Bool  ? fval.b : false);
                    arg.obj->fields["f"] = vfloat(fval.kind == VKind::Float ? fval.f : 0.0);
                    arg.obj->fields["present"] = vbool(av.provided);
                    args.arr->push_back(arg);
                }
            av.obj->fields["args"] = args;
            attributes.arr->push_back(av);
        }
        v.obj->fields["attributes"] = attributes;

        if (decl->callable) {
            v.obj->fields["returnType"] =
                vstr(decl->type ? decl->type->canonical : std::string());
            Symbol* paramCls = metaClassSymbol("Param");
            Value params; params.kind = VKind::Array;
            params.arr = std::make_shared<std::vector<Value>>();
            for (const Param& p : decl->params) {
                Value pv; pv.kind = VKind::Object;
                pv.obj = std::make_shared<Object>();
                pv.obj->cls = paramCls;
                pv.obj->fields["name"] = vstr(std::string(p.name));
                pv.obj->fields["type"] = vstr(p.type ? p.type->canonical : std::string());
                params.arr->push_back(pv);
            }
            v.obj->fields["params"] = params;
        } else {
            v.obj->fields["type"] = vstr(decl->type ? decl->type->canonical : std::string());
        }
    } else {
        v = vvoid();
    }
    metaCache_[decl] = v;
    return v;
}

std::unordered_map<std::string, Value> RuleEngine::materializeBindings(Bindings& out) {
    std::unordered_map<std::string, Value> locals;
    for (auto& [name, bnd] : out) {
        if (name == "$subject") continue;   // internal plumbing, not user-visible
        Value v;
        if (bnd.hasVal) v = bnd.val;
        else if (bnd.declStmt) v = buildMetaValue(bnd.declStmt);
        else v = vvoid();
        locals[std::string(name)] = v;
    }
    return locals;
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
        Symbol* want = lookupClassIn(namespaceScope(r.ns), m.attrName);
        if (!want) want = lookupClassIn(sema_.global, m.attrName);
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
                            "' is a " + declKindWord(subj));
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

// Collect names the template declares as locals (Var statements), assigning
// each a fresh gensym in renames_ — hygiene by alpha-renaming (§7.1).
void RuleEngine::collectTemplateLocals(const Stmt* s) {
    if (!s) return;
    if (s->kind == StmtKind::Var && !s->name.empty() && !isHole(s->name) &&
        !renames_.count(s->name)) {
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
                        "' is a " + declKindWord(subj));
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
                        "' is a " + declKindWord(subj));
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
    // Member/field/method name may itself be a hole (`member of` templates).
    if (isHole(out->name)) {
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
                        "' subjects, but this is a '" + declKindWord(item.get()) +
                        "' — did you mean 'on type' / 'on " + declKindWord(item.get()) + "'?";
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
