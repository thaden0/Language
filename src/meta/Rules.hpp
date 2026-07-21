#pragma once
#include "core/Ast.hpp"
#include "core/Diagnostic.hpp"
#include "runtime/Eval.hpp"
#include "driver/Project.hpp"
#include "core/Symbols.hpp"
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// One recorded rule firing — drives --expand provenance and the --rules report.
struct ExpansionRecord {
    std::string ruleName;     // "Web::registerRoutes"
    SourceSpan origin;        // the matched declaration / attribute (use site)
    SourceSpan templateSpan;  // the quasiquote (rule site)
    int fileIndex = -1;
};

// -----------------------------------------------------------------------------
// The rule stage (§16.5): author-defined compile-time metaprogramming, run
// between resolve pass 1 and the pass-2 resolve/check. Phase 1 covers Layer C
// (comptime: fold `comptime` vars/exprs/ifs to literals via the shipped
// tree-walk oracle, hermetic + step-bounded) and Layer A (attributes: resolve
// `@Name(args)` against the using file's imported namespaces, type-check the
// arguments against the attribute's fields, evaluate them to compile-time
// values). Rules (Layer B) consume the evaluated attributes in Phase 2.
//
// Scoping rides P-4: an attribute name resolves through the namespaces the
// declaring FILE imports (`FileImports::effective`), so "what metaprogramming
// can touch this file" is answered by its `uses` list (proposal §5).
// -----------------------------------------------------------------------------

class RuleEngine {
public:
    // §9 (Phase 3): the engine computes its own imports map — internally,
    // AFTER comptime folding (so `uses` inside a taken `comptime if` branch is
    // visible before scoping decisions read it). Callers no longer compute or
    // pass one in.
    RuleEngine(const std::vector<ProjectFile>& files,
               const Sema& sema, Program& prelude,
               const SourceFile& file, DiagnosticSink& sink,
               const ComptimeOptions& opts = {});

    // Fold comptime, resolve + evaluate attributes. Returns true if the tree
    // changed (caller re-resolves). Failures are sink diagnostics, never throws.
    bool run(Program& program);

    // The evaluated fields of one attribute use (Phase 2's rule input):
    // field name -> compile-time value, in the attribute's field order.
    // `provided` distinguishes an explicitly-written argument from one that
    // fell back to the field's default — item A's `argStr(i)` reads as None on
    // a defaulted arg so `attr("X")?.argStr(0) ?? fallback` works (LA-4 §2.2).
    struct AttrArgVal { std::string field; Value val; bool provided; };
    using AttrValue = std::vector<AttrArgVal>;
    const std::map<const AttrUse*, AttrValue>& attrValues() const { return attrValues_; }

    const std::vector<ExpansionRecord>& expansions() const { return expansions_; }
    std::string renderRulesReport() const;             // --rules

    // LA-20 §7/§8: wire the comptime `import()` intrinsic. `ctx.assets`/
    // `ctx.rootDir` come from main.cpp (the plan-mode asset table, or the
    // single-file root); this locates the prelude's `std::import` decl once
    // (namespace-scope lookup, the same pattern metaClassSymbol uses) and
    // installs both on the Evaluator before run() executes any comptime
    // code. Call after construction, before run().
    void setImportContext(ImportContext ctx);
    const std::vector<ImportedAsset>& importedAssets() const { return eval_.importedAssets(); }
    // LA-20 §8: span offset -> {rel, bytes} for every reified literal that
    // came from a folded import() call — --expand's elision keys off this.
    const std::map<uint32_t, std::pair<std::string, size_t>>& importLiteralSpans() const {
        return importLiteralSpans_;
    }

private:
    const std::vector<ProjectFile>& files_;
    // Owned (not a reference): recomputed by run() itself, after comptime
    // folding (§9) — the caller no longer supplies this.
    std::vector<FileImports> imports_;
    const Sema& sema_;
    const SourceFile& file_;
    DiagnosticSink& sink_;
    Evaluator eval_;             // the oracle in comptime mode (§7.1 reuse)
    bool changed_ = false;

    // AST string_views must point at storage that outlives the tree; reified
    // literals and synthesized names live here (deque: stable addresses).
    std::deque<std::string> reifiedText_;
    // F4: generated fragment buffers own every string_view in parsed Ast
    // values. RuleEngine outlives pass 2 and all later consumers in main.
    std::deque<SourceFile> generatedSources_;
    Value parseGenerated(bool expr, const std::string& text, std::string& err);

    std::map<const AttrUse*, AttrValue> attrValues_;
    std::vector<ExpansionRecord> expansions_;

    // --- file / scope plumbing ---
    int fileOf(SourceSpan span) const;                 // ProjectFile index by offset
    Scope* namespaceScope(const std::string& ns) const; // "<root>"/"std"/named -> scope
    // LA-20 §5/§6: the moduleId owning a span ("" for the root project, a
    // single file, or an offset predating every file). Feeds the Evaluator's
    // per-comptime-root import module before every evalComptimeAt call, so a
    // dependency's `import` resolves against the dependency's OWN assets.
    std::string moduleOf(SourceSpan span) const;
    // LA-20: evalComptime, but sets the Evaluator's import-context module
    // from the expression's own span first — the single choke point every
    // comptime-root evaluation in this file goes through, so `import()`
    // always sees the right module no matter which comptime construct
    // (var/if/default/attribute/where/$for) triggers it.
    Value evalComptimeAt(Expr* e, std::string& err, bool& failed);
    Value evalComptimeAt(Expr* e, const std::unordered_map<std::string, Value>& locals,
                        std::string& err, bool& failed);
    // LA-20 §8: see importLiteralSpans() above.
    std::map<uint32_t, std::pair<std::string, size_t>> importLiteralSpans_;
    void noteImportLiteral(size_t assetsBefore, const Value& v, SourceSpan span);

    // --- attributes (Layer A) ---
    void validateAttributeDecl(Stmt* cls);             // fields must be primitive
    void processAttrs(Stmt* decl);
    Symbol* resolveAttr(AttrUse& a, int fileIdx);      // M01/M02
    void evalAttrArgs(AttrUse& a, Symbol* attrClass);  // M03/M04
    // Step D (§9): a walk over the tree that does ONLY attribute resolution —
    // separated from comptime folding so imports_ can be recomputed between
    // them (a rule/attribute must scope against POST-fold `uses`).
    void walkAttrs(std::vector<StmtPtr>& items);

    // --- comptime (Layer C) ---
    // `walkItems` recurses into an ORDINARY statement list (e.g. a function
    // body's Block) — a folded comptime-if there still nests as a Block
    // (§9 step 3 leaves this path alone; splicing only matters where
    // `computeFileImports`/`processImports` look, i.e. item level).
    void walkItems(std::vector<StmtPtr>& items);
    void walkStmt(StmtPtr& slot);
    void walkExpr(ExprPtr& slot);
    // Shared structural descent (a/b/c/list/block/arms), factored out of
    // walkExpr so the C-late deferred-comptime path (§7×§9) can expand macro
    // calls hiding inside a comptime subtree without going through foldExpr.
    void walkExprChildren(Expr* e);
    void foldExpr(ExprPtr& slot);                      // comptime expr -> literal
    void foldParamDefaults(std::vector<Param>& params);
    bool reify(const Value& v, SourceSpan span, ExprPtr& out);  // M14/M28

    // Step B, item-level (§9 step 3): `program.items` and namespace bodies are
    // walked with THIS pair instead — a folded comptime-if's taken Block
    // splices its statements inline (so a `uses` in it lands where the
    // recomputed imports map and pass-2 `processImports` both see it).
    void walkTopLevelItems(std::vector<StmtPtr>& items);
    void foldTopLevelItem(StmtPtr item, std::vector<StmtPtr>& out);
    // §8 pass-1 comptime pre-check: the lexical scope of the comptime root
    // currently being walked, non-null ONLY at scope-complete positions (root
    // = global, namespace = its scope; null inside class/member/block bodies,
    // which have locals a static scope can't see). Read by precheckComptime.
    Scope* comptimeScope_ = nullptr;
    // §8: when a comptime root FAILED to evaluate, run the type checker over it
    // for a precise message. Returns that message, or "" if the checker found
    // nothing (fall back to the opaque eval error). Runs only on failure and
    // only at a scope-complete position, so it never introduces a false error.
    std::string precheckComptime(const Expr* e);
    // §7: on during the SECOND walkTopLevelItems call only (after imports_ is
    // recomputed) — see run()'s step D comment for why macro-call expansion
    // cannot ride the first (comptime-fold) walk.
    bool macroExpansionEnabled_ = false;

    std::string_view own(std::string text);            // intern into reifiedText_

    // --- rules (Layer B) ---
    // A matchable declaration + its enclosing chain and owning file (built once,
    // consulted per rule). Ancestors run innermost -> outermost.
    struct Ancestor { std::string_view kindWord; Stmt* stmt; Symbol* sym; };
    struct DeclInfo {
        Stmt* decl = nullptr;
        std::string_view kindWord;
        int fileIdx = -1;
        std::vector<Ancestor> ancestors;
        Symbol* sym = nullptr;   // the decl's own symbol (class/namespace); for
                                 // a subject binding's declSym (type-hole hygiene)
    };
    // A rule detached from the tree, with its declaring namespace + order key.
    struct OwnedRule {
        StmtPtr node;             // owns the detached rule Stmt
        std::string ns;           // declaring namespace ("<root>" for top level)
        uint32_t offset = 0;      // source offset (tie-break ordering)
    };
    // One resolved match binding (§2, Phase 3): unified on Value so the SAME
    // binding serves template value-holes ($r.method) and `where`-clause plain
    // names — an attribute binds its evaluated Object directly; a decl's
    // meta.* reflection Object is built lazily (only if `where` needs it).
    struct Binding {
        Value val;                       // attribute: its Object. decl: unset
                                          // until materializeBindings() builds it.
        bool hasVal = false;
        Stmt* declStmt = nullptr;        // decl identity, for name-splicing/anchors
        Symbol* declSym = nullptr;
        std::string_view selectorText;   // method/field selector, for name holes
        const Expr* exprNode = nullptr;  // macro args (§7): the call-site argument
                                          // subtree a `$param` splices verbatim
    };
    using Bindings = std::map<std::string_view, Binding>;

    std::vector<OwnedRule> rules_;
    std::vector<DeclInfo> decls_;
    std::deque<std::string> synthNames_;     // owns gensym'd identifier text
    // Hygiene (§7.1): template-declared locals are alpha-renamed to fresh names
    // so injected code can neither capture nor be captured by use-site locals.
    std::map<std::string_view, std::string_view> renames_;
    int gensymCounter_ = 0;

    void collectRules(std::vector<StmtPtr>& items, const std::string& ns);
    void indexDecls(std::vector<StmtPtr>& items, std::vector<Ancestor>& chain,
                    const std::string& ns);
    void orderRules();
    void runRules();
    // The two-pass rule sweep (additive `inject` rules first, then
    // `rewrites`/`replace` rewriters — §2.4 defined order). `reentrantOnly`
    // restricts the sweep to `reentrant` rules, used by the fixpoint below.
    void runRulePasses(bool reentrantOnly);
    // The per-rule inner loop (match every indexed decl, expand on a hit).
    void matchAndExpandRule(const OwnedRule& r);
    // §4: after the initial sweep, re-run ONLY `reentrant` rules on the
    // re-indexed post-expansion tree until a fixpoint (a round fires nothing
    // new) or the round budget is exhausted (M34). Non-`reentrant` rules see
    // the tree exactly once — the fired-pair guard keeps the safe majority safe.
    void runReentrantFixpoint(Program& program);
    // §4: resolve+evaluate attributes on decls carrying a not-yet-resolved
    // attribute (freshly rule-injected code), so a reentrant rule can match an
    // attribute a prior round emitted. Idempotent on already-resolved attrs.
    void resolveNewAttrs(std::vector<StmtPtr>& items);
    int reentrantRounds_ = 8;                 // §4 re-trigger cap (M34)
    // §4: (rule, decl) pairs already expanded, so a re-indexed decl a reentrant
    // round re-encounters never fires twice — the fixpoint fires only on NEW
    // decls, which is what makes it converge (or hit M34 if new decls never stop).
    std::set<std::pair<const Stmt*, const Stmt*>> firedPairs_;
    // Static validation of `rewrites` rules at collection time (Layer D, §2.3):
    // M35 (target bind is a callable match bind) and M32 ($body referenced
    // exactly once across the rule's replace templates).
    void validateRewriteRules();
    bool declKindMatches(const Stmt* d, std::string_view kind) const;
    bool tryMatch(const OwnedRule& r, const DeclInfo& di, Bindings& out,
                  const AttrUse** firedAttr);
    Symbol* resolveTypeName(const TypeRef* t, const std::string& ns) const;
    bool implementsOrExtends(Symbol* cls, Symbol* iface) const;
    void expand(const OwnedRule& r, const DeclInfo& di, Bindings& b,
                const AttrUse* firedAttr);
    void warnDanglingAttrs(std::vector<StmtPtr>& items);

    // --- expression macros (§7) --------------------------------------------
    // M22, static: a macro parameter spliced more than once in its own
    // template — checked once per macro decl, right after collection, so it
    // fires even for a macro nobody ever calls.
    void validateMacroDecls();
    // §7×§9, clone-time expansion: a rule/anchor template can call a macro
    // (`inject \`log = tag!(x)\`` …). The template clone runs during rule
    // matching (Layer B), long after the dedicated macro walk (Layer C, step
    // D) has finished, so it never sees the injected code — left alone, the
    // clone would carry an unexpanded macro-call node into pass 2. Called
    // right after cloning, on the clone only: reuses walkStmt/walkItems (with
    // macroExpansionEnabled_ forced on for the call, since Layer B always
    // runs after it's been turned back off) rather than a bespoke walker.
    // Definition-site scoping (§10) falls out for free — the template's
    // spans still point into the RULE's file, so file-based macro scoping
    // (fileOf(call->span)) resolves against the rule file's imports, exactly
    // as if the macro call had been written there by hand.
    void expandMacrosInClone(StmtPtr& s);
    void expandMacrosInClone(std::vector<StmtPtr>& stmts);

    // Resolves and expands one `name!(args)` call site in place, during the
    // SECOND walkTopLevelItems pass (after imports_ is recomputed — a macro
    // call scopes through imports_ exactly like an attribute/rule, so it
    // can't resolve correctly during the first, pre-imports fold walk; see
    // run()'s step D comment). Nesting order still matches a single walk:
    // children (incl. any macro call in an argument) are visited first, so
    // `outer!(inner!(x))` expands bottom-up. M23 unknown/ambiguous; M24 no
    // re-entry (checked after cloning).
    void expandMacroCall(ExprPtr& slot);

    // --- meta.* reflection + `where` (§3, §4, Phase 3) ---
    // Lazily-built meta.{Class,Method,Field} Object per declaration, cached for
    // the stage's lifetime (most rules never use `where`, so this stays empty).
    std::map<const Stmt*, Value> metaCache_;
    Value buildMetaValue(Stmt* decl);
    Symbol* metaClassSymbol(const char* name) const;   // namespaceScope("meta") lookup
    // name -> comptime value for every binding, for the `where` expression's env
    // (attribute bindings use their already-evaluated .val; decl bindings build
    // their meta.* object on demand). Excludes the internal "$subject" sentinel.
    std::unordered_map<std::string, Value> materializeBindings(Bindings& out);

    // --- body-replacing rules (Layer D, §2) --------------------------------
    // The original body being spliced back by `$body` during a `replace`
    // expansion (null outside one). Set to the subject's normalized Block just
    // before the replace template is cloned; read by cloneExpr ($body as an
    // expression) and cloneStmtBody ($body as a statement).
    const Stmt* replaceBodyOrig_ = nullptr;
    // While true, cloneStmt/cloneExpr copy names VERBATIM — no hygiene rename,
    // no def-site qualification, no $body reinterpretation. Used to move the
    // ORIGINAL body as one authored unit (§2.2: its own locals aren't renamed).
    bool verbatimClone_ = false;
    // §3 confluence: which rule already replaced a given method's body, so a
    // second replacer on the same body trips M33 (keyed on the subject Stmt).
    std::map<const Stmt*, std::string> replacedBy_;
    // §3 confluence: which rule injected a given class member, so a same-name+
    // type `member of` collision between two injections names both rules in the
    // unified M33 family (keyed on the injected member Stmt).
    std::map<const Stmt*, std::string> injectedMemberBy_;
    // The original body's single value expression (arrow `=> e` or a block that
    // is exactly `{ return e; }`), for `$body` in expression position; null if
    // the body isn't a single value expression (caller raises M31).
    const Expr* singleValueBodyExpr(const Stmt* body) const;
    // Clone a template statement list, splicing the original body's statements
    // in place wherever a bare `$body;` statement appears (§2.2 statement form).
    void cloneStmtListBody(const std::vector<StmtPtr>& in, Bindings& b, bool& err,
                           std::vector<StmtPtr>& out);

    // clone + hole substitution (§5.5)
    void collectTemplateLocals(const Stmt* s);   // populate renames_ (hygiene)
    StmtPtr cloneStmt(const Stmt* s, Bindings& b, bool& err);
    // Clone one template statement/decl/member into `out`, expanding a
    // statement-position `$for` (StmtKind::ForSplice) into one clone per
    // iterated item — the statement-list analogue of cloneArrayElements (item J).
    void cloneStmtInto(const Stmt* s, Bindings& b, bool& err, std::vector<StmtPtr>& out);
    ExprPtr cloneExpr(const Expr* e, Bindings& b, bool& err);
    TypeRefPtr cloneType(const TypeRef* t, Bindings& b, bool& err);
    Param cloneParam(const Param& p, Bindings& b, bool& err);
    std::string_view declRefName(const Binding& bnd) const;

    // §10: the rule/macro currently being cloned, for definition-site
    // qualification — set by expand()/expandMacroCall() before any
    // cloneStmt/cloneExpr call, read by cloneExpr's free-Name handling.
    std::string curRuleNs_ = "<root>";
    // The name of the rule currently being cloned — set by expand() alongside
    // curRuleNs_, read only for B2 `$if` diagnostics (M40) so their message
    // matches the `where`-clause shape (`rule '<r>' …`).
    std::string curRuleName_ = "<root>";
    // B2 (techdesign-splices-conditional §3.2): evaluate a `$if` condition with
    // the firing's bindings as comptime locals — the identical env `where` uses.
    // Sets `taken` to the bool result and returns true; on a failed/non-bool
    // condition it raises the diagnostic (M40 for non-bool), sets `err`, and
    // returns false. Shared by the statement (cloneStmtInto) and expression
    // (cloneArrayElements) fold sites.
    bool evalForkCond(Expr* cond, Bindings& b, bool& taken, bool& err);
    // A free Name (not a hole, not a renamed template-local) that resolves in
    // the rule's/macro's OWN declaring namespace qualifies to NS::name, so
    // injected code always reaches the rule-author's helpers regardless of
    // where it fires — the second hygiene half (capture avoidance runs both
    // ways: §7.1 protects the use site from the template, this protects the
    // template from the use site).
    //
    // Scope note: the design doc's "one subtlety" — if curRuleNs_ itself only
    // re-exported the name via ITS OWN `uses` (so the name's true home is a
    // third namespace), qualify to the true home instead — is not
    // implemented. Distinguishing "genuinely declared in curRuleNs_" from
    // "present in curRuleNs_'s scope because uses-copying put it there" is
    // NOT answerable from Scope data alone (uses-copying makes both look
    // identical: a Symbol* entry in a `names` map); resolveAttr's identity-
    // dedupe (which the doc suggests reusing) only disambiguates root-vs-
    // named, not named-vs-named, so it doesn't actually resolve this case
    // either. A correct fix needs a Stmt*->declaring-namespace map (which
    // namespace's body textually contains the symbol's declaration) that
    // nothing currently builds. Left as bare-qualify-to-curRuleNs_ (still
    // correct whenever a namespace doesn't re-export borrowed names under
    // its own name, which is the common case and the doc's own acceptance
    // test).
    bool qualifyDefSite(std::string_view name, std::string& outNs) const;

    // $_params / $_args (§6, Phase 3): both resolve against the subject bound
    // under the internal "$subject" key (set alongside the subject's real bind
    // name in tryMatch), never an arbitrary $x.
    Stmt* subjectOf(const Bindings& b) const;
    void cloneParamList(const std::vector<Param>& params, Bindings& b, bool& err,
                        std::vector<Param>& out);
    void cloneArgList(const std::vector<ExprPtr>& args, Bindings& b, bool& err,
                      std::vector<ExprPtr>& out);

    // $for list splices (§5, Phase 3): array-literal elements only. A
    // ForSplice element evaluates its iterator with the CURRENT bindings as
    // comptime locals, requires an array result (M21), and clones its element
    // template once per item with the loop var bound fresh — extending, not
    // mutating, the caller's Bindings, so nested $for sees the outer var too.
    void cloneArrayElements(const std::vector<ExprPtr>& elems, Bindings& b, bool& err,
                            std::vector<ExprPtr>& out);
    // Clone one array element, folding an expression-position `$if` (ForkSplice,
    // B2) to its taken leaf; a plain element clones 1:1.
    void cloneArrayElementInto(const Expr* el, Bindings& b, bool& err,
                               std::vector<ExprPtr>& out);

    // anchors (§5.6, §8)
    Stmt* boundClass(const Bindings& b, std::string_view target) const;
    void injectIntoCtors(Stmt* cls, std::vector<StmtPtr>& stmts, bool top);
    void injectMember(Stmt* cls, StmtPtr member, const OwnedRule& r);
    void normalizeMemberBody(Stmt* member);      // memberBody -> a Block, in place
    // Marker anchors (§8.2): how many statements already inserted after a given
    // marker Empty-stmt (keyed by its stable node identity), so several rules
    // stacking on one marker accumulate in rule order rather than reversing.
    std::map<const Stmt*, int> markerInsertCount_;
    std::vector<StmtPtr> namespaceInjections_;   // `namespace N` anchor (§8.3) output

    // Named-anchor design §2.3: the program-global splice-site index. Built by
    // indexSpliceSites() in the same phase decls_ is indexed (and rebuilt each
    // reentrant round), it maps a splice attribute's simple name to every
    // `@Name();` site in the program — the owning statement vector plus the
    // marker Empty node itself. `expand`'s SpliceSite arm looks a rule's target
    // up here, so a rule matched on one declaration can land statements at a
    // site in a DIFFERENT declaration's body (the delta over subject-local
    // `at marker`), inheriting that body's lexical scope by placement.
    struct SpliceSiteRef {
        std::vector<StmtPtr>* vec;   // the body vector the marker lives in (stable)
        Stmt* node;                  // the `@Name();` Empty marker node
    };
    std::map<std::string_view, std::vector<SpliceSiteRef>> spliceSites_;
    // (Re)build spliceSites_ by walking the program tree directly (NOT decls_, so
    // it runs even for a rule-free program — M43 is a property of the site, not of
    // any rule). Also emits M43 for a site whose name is not a declared attribute.
    void indexSpliceSites(std::vector<StmtPtr>& items);
    void gatherSpliceSites(std::vector<StmtPtr>& items,
                           std::set<std::string_view>& attrNames);
    void collectSpliceSites(std::vector<StmtPtr>& vec);   // recursive site gather
};
