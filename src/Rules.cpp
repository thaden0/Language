#include "Rules.hpp"
#include "RulesInternal.hpp"
#include "Checker.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
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
      eval_(sema, sink) {
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
    // E2. Build the splice-site index + validate sites (M43). Always — a malformed
    // `@Name();` site must be rejected even in a rule-free program (named-anchor
    // design §2.1); `expand`'s `at splice` arm reads the index built here.
    indexSpliceSites(program.items);
    // F. Index every matchable declaration, then match + expand rules (Layer B).
    if (!rules_.empty()) {
        std::vector<Ancestor> chain;
        indexDecls(program.items, chain, "<root>");
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
    int fi = fileOf(decl->span);
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

// --- meta.* reflection + `where` (§3, §4) — Phase 1 support code for the
// `where` clause: kept here because RuleEngine::buildMetaValue (below) is the
// object the doc names as staying in Rules.cpp, and metaClassSymbol/
// materializeBindings are its immediate, single-purpose helpers.

Symbol* RuleEngine::metaClassSymbol(const char* name) const {
    return rulesDetail::lookupClassIn(namespaceScope("meta"), name);
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
