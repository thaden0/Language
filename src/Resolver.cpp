#include "Resolver.hpp"
#include "PreludeEmbedded.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

// A minimal prelude: primitives are registered directly; these library types
// are just ordinary declarations gathered like any other source (§12). This is
// also what lets the IOStream diamond be a real, testable shape.
//
// Ship-as-files (techdesign-prelude-ship-as-files-opus.md): the prelude no
// longer lives in R"prelude(...)" string constants here. The eight segments
// ship as prelude/*.lev files (the single source of truth), with a build-
// generated embedded fallback (build/generated/PreludeEmbedded.cpp, ordered
// table declared in PreludeEmbedded.hpp) baked into langfront. parsePrelude()
// (below) reads the files when a prelude directory resolves and the embedded
// bytes otherwise; main.cpp's findPreludeDir() picks the directory (--prelude
// -> LV_PRELUDE_DIR -> next-to-binary -> source tree -> embedded fallback).
// Whole-program resolution (§12) makes segment order irrelevant, so the seams
// are purely organizational:
//   core       : primitive object masks + collections + Seq + streams glue
//   std        : `namespace std` (sys floor, aggregates, promise/timer,
//                sockets + HTTP, files, exception hierarchy)
//   rest       : top-level Range/StreamBuffer/Console + meta/math/env/term
//   regex_core : Track 10 linear-time regex engine (namespace regex internals)
//   regex_api  : Track 10 regex public surface (Regex/Match/Group/...)
//   web        : Track 09 web foundations (encoding, digest, DateTime, json)
//   wasm       : Track W W-M3 JS/DOM bridge (doc 05 §2); wasm32*-only segment
//                (R3) — `Dom` handle classes over the std::sysHost* bridge,
//                DOM events as §13 stream endpoints; on native targets the
//                bridge natives raise, and nothing in the shared corpus reaches
//                them, so the four-lane differential is unaffected.
//   expr       : LA-31 expression reification `expr::Expr<F>` node taxonomy

bool isTypeKind(SymbolKind k) {
    return k == SymbolKind::Class || k == SymbolKind::TypeParam ||
           k == SymbolKind::Primitive;
}

Symbol* findLocal(Scope* scope, std::string_view name, SymbolKind kind) {
    const std::vector<Symbol*>* v = scope->localLookup(name);
    if (!v) return nullptr;
    for (Symbol* s : *v)
        if (s->kind == kind) return s;
    return nullptr;
}

void addToScope(Scope* scope, Symbol* sym) {
    scope->names[sym->name].push_back(sym);
}

// --- 005 R2/R3: match-arm value→type reclassification helpers ---------------

// One classifiable leaf of a match-arm value pattern: a pure `::`-chain
// (namespace `path` + leaf `name`) or a bare `Name` (empty path).
struct ArmLeaf {
    std::vector<std::string_view> path;   // namespace segments (empty for bare Name)
    std::string_view name;                // final segment
    SourceSpan span;
};

// Extract a bare `Name` or a pure `::`-chain into an ArmLeaf. Returns false for
// anything else — a `.`-link anywhere (colon=false), a Call, Index, etc. — so
// only genuine qualified-name-shaped patterns are ever reclassified.
bool asChainLeaf(const Expr* e, ArmLeaf& out) {
    if (!e) return false;
    if (e->kind == ExprKind::Name) {
        out.path.clear();
        out.name = e->text;
        out.span = e->span;
        return true;
    }
    if (e->kind == ExprKind::Member && e->colon) {
        std::vector<std::string_view> segs;      // collected leaf..root
        const Expr* cur = e;
        while (cur->kind == ExprKind::Member && cur->colon) {
            segs.push_back(cur->text);
            cur = cur->a.get();
        }
        if (!cur || cur->kind != ExprKind::Name) return false;   // a `.` link ⇒ disqualify
        segs.push_back(cur->text);               // the root Name
        std::reverse(segs.begin(), segs.end());  // now root..leaf
        out.name = segs.back();
        out.path.assign(segs.begin(), segs.end() - 1);
        out.span = e->span;
        return true;
    }
    return false;
}

// Flatten a `|` (Pipe) tree on the value route into its operand leaves.
void collectPipeLeaves(const Expr* e, std::vector<const Expr*>& out) {
    if (e && e->kind == ExprKind::Binary && e->op == TokenKind::Pipe) {
        collectPipeLeaves(e->a.get(), out);
        collectPipeLeaves(e->b.get(), out);
    } else {
        out.push_back(e);
    }
}

// Classify a leaf against `scope`, navigating exactly as resolveType's Named
// case. Returns navOK (false ⇒ root not a namespace, or a middle/leaf miss on a
// qualified chain — the pattern is left untouched). On navOK, sets hasType /
// hasValue from the leaf's visible symbols (a type-kind symbol vs a Var value).
bool leafClassify(const ArmLeaf& lf, Scope* scope, bool& hasType, bool& hasValue) {
    hasType = hasValue = false;
    const std::vector<Symbol*>* cands = nullptr;
    if (lf.path.empty()) {
        for (Scope* sc = scope; sc && !cands; sc = sc->parent)
            cands = sc->localLookup(lf.name);
        if (!cands) return true;                 // unknown bare name: navOK, no symbols
    } else {
        Symbol* ns = nullptr;
        for (size_t i = 0; i < lf.path.size(); ++i) {
            std::string_view seg = lf.path[i];
            if (i == 0)
                for (Scope* sc = scope; sc && !ns; sc = sc->parent)
                    ns = findLocal(sc, seg, SymbolKind::Namespace);
            else
                ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                       : nullptr;
            if (!ns) return false;               // root not a namespace / middle miss
        }
        if (!ns || !ns->scope) return false;
        cands = ns->scope->localLookup(lf.name);
        if (!cands) return false;                // leaf absent in the namespace
    }
    for (Symbol* s : *cands) {
        if (isTypeKind(s->kind)) hasType = true;
        else if (s->kind == SymbolKind::Var) hasValue = true;
    }
    return true;
}

// A leaf is a clean type leaf iff navigation succeeded and it names a type but
// not a value (ambiguous both-cases are handled explicitly at the R2 call site).
bool leafIsType(const ArmLeaf& lf, Scope* scope) {
    bool hasType = false, hasValue = false;
    return leafClassify(lf, scope, hasType, hasValue) && hasType && !hasValue;
}

}  // namespace

// ---------------------------------------------------------------------------
//  gather
// ---------------------------------------------------------------------------

Program Resolver::parsePrelude() {
    // Per-target selection (design R3): wasm-only segments ride wasm32*
    // triples only. Predicate mirrors main.cpp's isWasmTriple (main.cpp:85,
    // file-local static there — duplicated one-liner by design ruling R3).
    bool wasm = targetTriple_.rfind("wasm32", 0) == 0;
    preludeFile_.name = "<prelude>";
    std::string text;
    for (unsigned long i = 0; i < kPreludeSegmentCount; ++i) {
        const PreludeSegment& seg = kPreludeSegments[i];
        if (seg.wasmOnly && !wasm) continue;
        if (!preludeDir_.empty()) {
            std::string path = preludeDir_ + "/" + seg.name + ".lev";
            std::ifstream in(path, std::ios::binary);   // raw bytes, no
            if (!in) {                                   // newline mangling
                std::fprintf(stderr, "leviathan: cannot read prelude file "
                             "'%s'\n", path.c_str());
                std::exit(1);   // R8: fatal-loud, never silent/mixed
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            text += ss.str();
        } else {
            text.append(reinterpret_cast<const char*>(seg.data), seg.size);
        }
    }
    preludeFile_.text = std::move(text);
    DiagnosticSink dummy;  // the prelude is trusted; ignore its diagnostics
    Lexer lexer(preludeFile_, dummy);
    Parser parser(lexer.tokenize(), preludeFile_, dummy);
    return parser.parseProgram();
}

void Resolver::gatherClass(Stmt* cls, Scope* parent) {
    if (findLocal(parent, cls->name, SymbolKind::Class))
        sink_.error(cls->span, "duplicate class '" + std::string(cls->name) + "'");

    Symbol* sym = sema_.newSymbol(SymbolKind::Class, cls->name, cls);
    sym->isValue = cls->isValue;              // `struct`: value semantics
    Scope* classScope = sema_.newScope(parent);
    sym->scope = classScope;
    addToScope(parent, sym);
    classSymbols_.push_back(sym);

    for (std::string_view g : cls->generics)
        addToScope(classScope, sema_.newSymbol(SymbolKind::TypeParam, g));
}

void Resolver::gatherInto(std::vector<StmtPtr>& items, Scope* scope, Symbol* enclosingNs) {
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        switch (s->kind) {
            case StmtKind::Namespace: {
                Symbol* ns = findLocal(scope, s->name, SymbolKind::Namespace);
                if (!ns) {                       // reopen if already present (§12)
                    ns = sema_.newSymbol(SymbolKind::Namespace, s->name, s);
                    ns->scope = sema_.newScope(scope);
                    addToScope(scope, ns);
                }
                gatherInto(s->body, ns->scope, ns);
                break;
            }
            case StmtKind::Class:
                gatherClass(s, scope);
                break;
            case StmtKind::Member:               // a free function at this level
                if (s->callable) {
                    addToScope(scope, sema_.newSymbol(SymbolKind::Function, s->name, s));
                    // bug.md #32 M2: remember which namespace (if any) directly
                    // encloses this free function, so unchecked prelude bodies can
                    // resolve a bare `Class(...)` construction against it.
                    s->enclosingNs = enclosingNs;
                }
                break;
            case StmtKind::Var:
                addToScope(scope, sema_.newSymbol(SymbolKind::Var, s->name, s));
                break;
            default:
                break;                           // exprs / binds: not names
        }
    }
}

// ---------------------------------------------------------------------------
//  imports (`uses NS;`)
// ---------------------------------------------------------------------------

// Resolve one `uses NS;` and dump all of NS's names into `into` (nearer
// declarations already present win, since lookup returns the front).
void Resolver::importOne(Stmt* s, Scope* into) {
    Symbol* ns = nullptr;
    for (size_t i = 0; i < s->generics.size(); ++i) {
        std::string_view seg = s->generics[i];
        if (i == 0) {
            for (Scope* sc = into; sc && !ns; sc = sc->parent)
                ns = findLocal(sc, seg, SymbolKind::Namespace);
        } else {
            ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                   : nullptr;
        }
        if (!ns) { sink_.error(s->span, "unknown namespace '" + std::string(seg) + "'"); break; }
    }
    if (ns && ns->scope)
        for (auto& [name, syms] : ns->scope->names)
            for (Symbol* sym : syms) {
                // An attribute's class symbol is not ordinary-name surface —
                // `@Name` resolution goes through the imports map + namespace
                // scopes (RuleEngine::resolveAttr), never the overlay. Dumping
                // it here would let e.g. `@Row` (Atlantis::Orm) silently
                // shadow a real class `Row` (Atlantis::Data) for every bare
                // type/value use in the importing file.
                if (sym->kind == SymbolKind::Class && sym->decl &&
                    sym->decl->isAttribute) continue;
                into->names[name].push_back(sym);
            }
}

// Resolve one selective `use Path::name (as alias);` (imports.md §3/§4) and
// bind the (possibly aliased) name into `into`. Every declaration kind
// imports uniformly: whatever symbols share the final segment's name in its
// home scope all travel together (the overload-set case for functions).
void Resolver::useOne(Stmt* s, Scope* into) {
    const std::vector<std::string_view>& path = s->generics;
    if (path.empty()) return;                 // parse already reported an error

    Symbol* ns = nullptr;
    const std::vector<Symbol*>* found = nullptr;
    if (path.size() == 1) {
        // No `::` at all: a bare name resolved through the ordinary scope
        // chain (no namespace to cross).
        for (Scope* sc = into; sc && !found; sc = sc->parent)
            found = sc->localLookup(path[0]);
    } else {
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            std::string_view seg = path[i];
            if (i == 0)
                for (Scope* sc = into; sc && !ns; sc = sc->parent)
                    ns = findLocal(sc, seg, SymbolKind::Namespace);
            else
                ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                       : nullptr;
            if (!ns) {
                sink_.error(s->span, "unknown namespace '" + std::string(seg) + "'");
                return;
            }
        }
        // A qualified name must not leak out to enclosing scopes (localLookup,
        // not lookup) — same rule as resolveType's Named-path navigation.
        found = ns->scope ? ns->scope->localLookup(path.back()) : nullptr;
    }
    if (!found || found->empty()) {
        sink_.error(s->span, "unknown name '" + std::string(path.back()) + "'");
        return;
    }
    for (Symbol* sym : *found) into->names[s->name].push_back(sym);

    // system-binds.md §5.3 (Channel 1): a qualified `use NS::T;` that resolves
    // to a class/interface also carries the (namespace, type) pair the
    // Checker consults to activate NS's exported bind for T (§1). A bare
    // `use name;` (no `::`, ns == null) has no namespace to search and never
    // activates anything.
    if (ns && (*found)[0]->kind == SymbolKind::Class) {
        s->useResolvedNs = ns;
        s->useResolvedTypeKey = (*found)[0]->name;
    }
}

bool Resolver::hasImports(const std::vector<StmtPtr>& items) const {
    for (const StmtPtr& s : items)
        if (s && (s->kind == StmtKind::UsesImport || s->kind == StmtKind::Use)) return true;
    return false;
}

// A direct-child factory `bind T => …;` (the `s->type`-present form — the same
// filter the Checker's bind stack applied). An object-install `bind expr;` (no
// type) is not a factory bind and stays on its Checker-discovered path.
bool Resolver::hasFactoryBinds(const std::vector<StmtPtr>& items) const {
    for (const StmtPtr& s : items)
        if (s && s->kind == StmtKind::Bind && s->type) return true;
    return false;
}

void Resolver::fillBinds(const std::vector<StmtPtr>& items, Scope* scope) {
    if (!scope) return;
    for (const StmtPtr& s : items) {
        if (!s || s->kind != StmtKind::Bind || !s->type) continue;   // factory form only
        // Key exactly as indexNamespaceBinds does: the resolved canonical type
        // string, or the bare name if unresolved.
        std::string key = s->type->canonical.empty()
            ? std::string(s->type->name) : s->type->canonical;
        // block-scoped-use §3.2: duplicate-in-scope detection lives here now (the
        // substrate owns registration). First-declared wins; the message text is
        // the Checker's verbatim so diagnostics stay byte-identical.
        auto [it, inserted] = scope->binds.emplace(key, s.get());
        (void)it;
        if (!inserted)
            sink_.error(s->span, "duplicate binding for '" + key + "' in this scope");
    }
}

void Resolver::fillDeclBinds(std::vector<StmtPtr>& items, Scope* scope) {
    if (!scope) return;
    fillBinds(items, scope);   // this scope's own direct factory binds
    for (StmtPtr& s : items) {
        if (!s || s->kind != StmtKind::Namespace) continue;
        if (Symbol* ns = findLocal(scope, s->name, SymbolKind::Namespace))
            fillDeclBinds(s->body, ns->scope);
    }
}

// Top-level and namespace-body `uses`/`use` (bug.md #8, imports.md — lexical
// import scoping):
//   - a TOP-LEVEL import (called with the global scope) resolves into its own
//     file's overlay scope, so a top-of-file import covers exactly that file
//     and never leaks program-wide;
//   - an import in a namespace body resolves into that (shared) namespace scope.
// Block-level imports are handled per-block in resolveStmtTypes.
//
// `use` runs before `uses` in the same scope so a selective import's symbol
// lands at the front of the scope's name vector — "specific beats bulk"
// (imports.md §4): Scope::lookup() returns .front(), so a `use` shadows a
// same-named `uses`-dumped symbol even though both add to the same table.
// (Function overloads are unaffected: functionOverloads collects the whole
// vector, not just the front, so two same-named functions still merge.)
void Resolver::processImports(std::vector<StmtPtr>& items, Scope* scope) {
    bool top = (scope == sema_.global);
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        if (s->kind == StmtKind::Use)
            useOne(s, top ? sema_.fileScopeFor(s->span.offset) : scope);
    }
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        if (s->kind == StmtKind::UsesImport) {
            importOne(s, top ? sema_.fileScopeFor(s->span.offset) : scope);
        } else if (s->kind == StmtKind::Namespace) {
            if (Symbol* nsSym = findLocal(scope, s->name, SymbolKind::Namespace))
                processImports(s->body, nsSym->scope);
        }
    }
}

// ---------------------------------------------------------------------------
//  type resolution
// ---------------------------------------------------------------------------

std::string Resolver::resolveType(TypeRef* t, Scope* scope) {
    if (!t) return "";
    switch (t->kind) {
        case TypeKind::Inferred:
            return t->canonical = "var";
        case TypeKind::Union: {
            std::string c;
            for (size_t i = 0; i < t->members.size(); ++i) {
                if (i) c += " | ";
                c += resolveType(t->members[i].get(), scope);
            }
            return t->canonical = c;
        }
        case TypeKind::Function: {
            std::string c = "(";
            for (size_t i = 0; i < t->funcParams.size(); ++i) {
                if (i) c += ", ";
                c += resolveType(t->funcParams[i].get(), scope);
            }
            c += ") => " + resolveType(t->funcRet.get(), scope);
            return t->canonical = c;
        }
        case TypeKind::Named: {
            // Build the qualified display/canonical prefix (`Room1::TheClass`).
            std::string qualName;
            for (std::string_view seg : t->path) { qualName += seg; qualName += "::"; }
            qualName += std::string(t->name);

            // Rule hygiene (bug.md #22, type position): a rule may have spliced
            // `$C` as a type and pre-resolved it to the matched class. Kept as a
            // FALLBACK only (not an override — overriding would lock in a stale
            // pass-1 symbol on re-resolution and break ordinary base-chain
            // checks): used just when by-name lookup below finds nothing or a
            // non-type, e.g. a `$C`-as-type colliding with the same-named
            // `bool $C = seed(...)` value the injection itself declares.
            Symbol* hygieneSym = (t->resolvedSymbol && isTypeKind(t->resolvedSymbol->kind))
                                 ? t->resolvedSymbol : nullptr;
            Symbol* sym = nullptr;
            bool navFailed = false;
            if (t->path.empty()) {
                sym = scope->lookup(t->name);
                // An attribute's class symbol is not an ordinary TYPE — `@Row`
                // (Atlantis::Orm) must not silently shadow a real class named
                // Row (Atlantis::Data) imported into the same file. Prefer a
                // non-attribute type when one is visible.
                if (sym && sym->decl && sym->decl->isAttribute) {
                    Symbol* nonAttr = nullptr;
                    for (Scope* sc = scope; sc && !nonAttr; sc = sc->parent)
                        if (const auto* v = sc->localLookup(t->name))
                            for (Symbol* c : *v)
                                if (isTypeKind(c->kind) &&
                                    !(c->decl && c->decl->isAttribute)) { nonAttr = c; break; }
                    if (nonAttr) sym = nonAttr;
                }
                if (hygieneSym && (!sym || !isTypeKind(sym->kind))) sym = hygieneSym;
            } else {
                // ::-qualified: walk the namespace path (§12), then find the type
                // in the final namespace's OWN scope (localLookup, not lookup —
                // a qualified name must not leak out to enclosing scopes).
                Symbol* ns = nullptr;
                for (size_t i = 0; i < t->path.size(); ++i) {
                    std::string_view seg = t->path[i];
                    if (i == 0)
                        for (Scope* sc = scope; sc && !ns; sc = sc->parent)
                            ns = findLocal(sc, seg, SymbolKind::Namespace);
                    else
                        ns = (ns && ns->scope) ? findLocal(ns->scope, seg, SymbolKind::Namespace)
                                               : nullptr;
                    if (!ns) {
                        sink_.error(t->span, "unknown namespace '" + std::string(seg) + "'");
                        navFailed = true;
                        break;
                    }
                }
                if (ns && ns->scope)
                    if (const auto* v = ns->scope->localLookup(t->name)) {
                        for (Symbol* c : *v)
                            if (isTypeKind(c->kind) &&
                                !(c->decl && c->decl->isAttribute)) { sym = c; break; }
                        if (!sym)
                            for (Symbol* c : *v) if (isTypeKind(c->kind)) { sym = c; break; }
                    }
            }

            if (navFailed) { /* namespace error already reported */ }
            else if (!sym)
                sink_.error(t->span, "unknown type '" + qualName + "'");
            else if (!isTypeKind(sym->kind))
                sink_.error(t->span, "'" + qualName + "' is not a type");
            else
                t->resolvedSymbol = sym;

            std::string c = qualName;
            if (!t->generics.empty()) {
                c += "<";
                for (size_t i = 0; i < t->generics.size(); ++i) {
                    if (i) c += ", ";
                    c += resolveType(t->generics[i].get(), scope);
                }
                c += ">";
            }
            return t->canonical = c;
        }
    }
    return t->canonical;
}

// Descend through an expression to resolve TypeRefs carried in expression
// position: explicit Call type arguments and `match` arm patterns, plus
// (recursively) any declarations in an arm body. The
// statement-level pass otherwise never reaches these: a `match` reaching a
// statement position is wrapped in an `ExprStmt` (or sits in a Var initializer /
// return / etc.), so a union-typed `Var` declared *inside* an arm block never
// had its type resolved, leaving the union's members unresolved (canonical "")
// and misfiring the inner match's exhaustiveness check. `is`/`inject` targets
// stay on the checker's existing lazy path.
void Resolver::resolveExprTypes(Expr* e, Scope* scope) {
    if (!e) return;
    for (TypeRefPtr& t : e->explicitTypeArgs) resolveType(t.get(), scope);
    if (e->kind == ExprKind::Match) {
        resolveExprTypes(e->a.get(), scope);         // subject
        for (MatchArm& arm : e->arms) {
            reclassifyMatchArm(arm, scope);          // 005 R2/R3: value → type pattern
            if (arm.type)      resolveType(arm.type.get(), scope);
            if (arm.value)     resolveExprTypes(arm.value.get(), scope);
            if (arm.bodyExpr)  resolveExprTypes(arm.bodyExpr.get(), scope);
            if (arm.bodyBlock) resolveStmtTypes(arm.bodyBlock.get(), scope);
        }
        return;
    }
    // bug #101: a lambda literal's DECLARED parameter types (`(Left a) => ...`)
    // must resolve like any declared type, so downstream generic inference — in
    // particular LA-18's specialization-set collector, which unifies a generic
    // type parameter (`(A) => R`) against the lambda's own declared param type —
    // sees a resolved symbol instead of an opaque, unresolved TypeRef. Harmless
    // for non-lambda exprs (their `params` vector is empty).
    for (Param& p : e->params)
        if (p.type) resolveType(p.type.get(), scope);
    // Generic descent: find expression-carried types nested anywhere.
    resolveExprTypes(e->a.get(), scope);
    resolveExprTypes(e->b.get(), scope);
    resolveExprTypes(e->c.get(), scope);
    for (ExprPtr& x : e->list) resolveExprTypes(x.get(), scope);
    if (e->block) resolveStmtTypes(e->block.get(), scope);   // lambda block body
}

// 005 R2/R3: `NS::Type` and `Enum::Member` share a token shape, so the parser
// leaves every `::`-qualified arm head on the neutral value route. Here — in the
// pass all four engines share, before Eval/Lower — reclassify to a type pattern
// when the leaf actually names a type. Parse neutrally, classify semantically.
void Resolver::reclassifyMatchArm(MatchArm& arm, Scope* scope) {
    if (!arm.value) return;
    Expr* v = arm.value.get();

    // R3: a `|` union on the value route (`ns::Sub | None =>`). `|` binds tighter
    // than the arm route, so it parses as a Binary(Pipe) tree. If ANY leaf is a
    // type, ALL must be — build a Union type pattern; a mixed tree is an error;
    // no type leaves ⇒ leave it (a bitwise-or of int consts stays a value).
    if (v->kind == ExprKind::Binary && v->op == TokenKind::Pipe) {
        std::vector<const Expr*> leaves;
        collectPipeLeaves(v, leaves);
        std::vector<ArmLeaf> parsed(leaves.size());
        std::vector<bool> isType(leaves.size(), false);
        int types = 0;
        for (size_t i = 0; i < leaves.size(); ++i) {
            if (asChainLeaf(leaves[i], parsed[i]) && leafIsType(parsed[i], scope)) {
                isType[i] = true;
                ++types;
            }
        }
        if (types == 0) return;                       // stays a value pattern
        if (types != (int)leaves.size()) {
            sink_.error(v->span, "mixed type/value '|' match pattern");
            return;
        }
        auto uni = std::make_unique<TypeRef>(TypeKind::Union);
        uni->span = v->span;
        for (const ArmLeaf& lf : parsed) {
            auto ref = std::make_unique<TypeRef>(TypeKind::Named);
            ref->span = lf.span;
            ref->path = lf.path;
            ref->name = lf.name;
            uni->members.push_back(std::move(ref));
        }
        resolveType(uni.get(), scope);
        arm.type = std::move(uni);
        arm.value = nullptr;
        return;
    }

    // R2: a single pure `::`-chain. A bare Name never reaches the value route
    // (the parser sends it to parseType), so require a non-empty namespace path.
    ArmLeaf lf;
    if (!asChainLeaf(v, lf) || lf.path.empty()) return;
    bool hasType = false, hasValue = false;
    if (!leafClassify(lf, scope, hasType, hasValue)) return;   // nav miss ⇒ leave
    if (hasType && hasValue) {
        std::string spelled;
        for (std::string_view seg : lf.path) { spelled += seg; spelled += "::"; }
        spelled += std::string(lf.name);
        sink_.error(v->span, "ambiguous match pattern '" + spelled +
                             "': names both a type and a value");
        return;
    }
    if (hasType) {                                   // type, no value ⇒ type pattern
        auto ref = std::make_unique<TypeRef>(TypeKind::Named);
        ref->span = v->span;
        ref->path = lf.path;
        ref->name = lf.name;
        resolveType(ref.get(), scope);
        arm.type = std::move(ref);
        arm.value = nullptr;
    }
    // value-only, or neither ⇒ leave as a value pattern (backstop/runtime stay loud).
}

void Resolver::resolveStmtTypes(Stmt* s, Scope* scope) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Block: {
            // designs/complete/techdesign-block-scoped-use.md §3.2: a block's own lexical
            // scope carries both its imports (`use`/`uses`) and its type-keyed
            // bind table. Materialized lazily — only when the block directly
            // contains an import or a factory `bind` — and consulted here (for
            // type refs), by the Checker (for names + binds), and the Lowerer
            // (for namespace re-derivation) via `s->blockScope`.
            //
            // (a) Unconditional reset: pass 2 (post-fold re-resolution) must
            // never inherit a pass-1 scope chain holding pass-1 symbols. Owned
            // by the substrate, not by luck (§1.2.3 / §5 P3a).
            s->blockScope = nullptr;
            Scope* inner = scope;
            if (hasImports(s->body) || hasFactoryBinds(s->body)) {
                inner = sema_.newScope(scope);
                // `use` before `uses` — same shadowing order as processImports.
                for (StmtPtr& c : s->body)
                    if (c->kind == StmtKind::Use) useOne(c.get(), inner);
                for (StmtPtr& c : s->body)
                    if (c->kind == StmtKind::UsesImport) importOne(c.get(), inner);
                s->blockScope = inner;
            }
            for (StmtPtr& c : s->body) resolveStmtTypes(c.get(), inner);
            // (b) Bind table fills AFTER the child walk: a bind's key is its
            // bound type's canonical string, which resolveStmtTypes' Bind case
            // (below) is what resolves.
            if (s->blockScope) fillBinds(s->body, s->blockScope);
            break;
        }
        case StmtKind::Var:
            if (s->type) resolveType(s->type.get(), scope);
            resolveExprTypes(s->init.get(), scope);
            break;
        case StmtKind::ExprStmt:
        case StmtKind::Return:
        case StmtKind::Throw:
            resolveExprTypes(s->expr.get(), scope);
            break;
        case StmtKind::If:
            // §16.5 Phase 3 §9: a comptime-if's branches are not unconditional
            // syntax — the rule stage folds the condition and keeps only the
            // taken branch before pass 2. Resolving both branches in pass 1
            // would type-check code that may never survive the fold (and,
            // per the "only syntax is unconditional" stance, never should).
            if (s->isComptime) break;
            resolveExprTypes(s->expr.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            resolveStmtTypes(s->elseBranch.get(), scope);
            break;
        case StmtKind::While:
        case StmtKind::DoWhile:
            resolveExprTypes(s->expr.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            break;
        case StmtKind::For:
            resolveStmtTypes(s->forInit.get(), scope);
            resolveExprTypes(s->expr.get(), scope);
            resolveExprTypes(s->forStep.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            break;
        case StmtKind::ForIn:
            if (s->type) resolveType(s->type.get(), scope);
            resolveExprTypes(s->expr.get(), scope);
            resolveStmtTypes(s->thenBranch.get(), scope);
            break;
        case StmtKind::Try:
            resolveStmtTypes(s->thenBranch.get(), scope);
            for (CatchClause& c : s->catches) {
                if (c.type) resolveType(c.type.get(), scope);
                resolveStmtTypes(c.body.get(), scope);
            }
            break;
        case StmtKind::Bind:                     // §12.5: resolve the bound type + factory body
            if (s->type) resolveType(s->type.get(), scope);
            if (s->memberBody) resolveStmtTypes(s->memberBody.get(), scope);
            break;
        default:
            break;
    }
}

void Resolver::resolveMember(Stmt* m, Scope* classScope) {
    // A generic method/function introduces its own type-param slots (same rule
    // as a generic class — just a different carrier).
    Scope* scope = classScope;
    if (!m->generics.empty()) {
        scope = sema_.newScope(classScope);
        for (std::string_view g : m->generics)
            addToScope(scope, sema_.newSymbol(SymbolKind::TypeParam, g));
    }
    if (m->type) resolveType(m->type.get(), scope);
    for (Param& p : m->params)
        if (p.type) resolveType(p.type.get(), scope);
    // Fields and other member declarations may carry initializer expressions.
    // Their embedded type syntax (including call-site `::<...>` arguments)
    // must resolve in the same generic/class scope as the declaration itself.
    resolveExprTypes(m->init.get(), scope);
    if (m->memberBody) resolveStmtTypes(m->memberBody.get(), scope);
}

void Resolver::resolveTypesIn(std::vector<StmtPtr>& items, Scope* scope) {
    // At the top level, each item's own types/exprs resolve through its file's
    // import overlay (bug.md #8) so a top-of-file `uses` is visible in that
    // file only; declarations are still *looked up* in the gather scope (global
    // at top level, the namespace scope when nested), where every name lives.
    bool top = (scope == sema_.global);
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        Scope* lex = top ? sema_.fileScopeFor(s->span.offset) : scope;
        switch (s->kind) {
            case StmtKind::Namespace: {
                Symbol* ns = findLocal(scope, s->name, SymbolKind::Namespace);
                if (ns) resolveTypesIn(s->body, ns->scope);
                break;
            }
            case StmtKind::Class: {
                Symbol* sym = findLocal(scope, s->name, SymbolKind::Class);
                // Top-level class scopes are re-parented to their file overlay
                // in run(), so `classScope` already reaches the file's imports.
                Scope* classScope = sym ? sym->scope : lex;
                for (TypeRefPtr& base : s->bases) resolveType(base.get(), classScope);
                for (StmtPtr& m : s->body) resolveMember(m.get(), classScope);
                break;
            }
            case StmtKind::Member:               // free function
                resolveMember(s, lex);
                break;
            default:
                // Var, Bind, and top-level executable statements (ExprStmt,
                // If/While/For, Return, ...) all resolve their types the same
                // way a function body does — including descending into `match`
                // arms, so a top-level nested match resolves like a nested one.
                resolveStmtTypes(s, lex);
                break;
        }
    }
}

// ---------------------------------------------------------------------------
//  shapes + collision detection
// ---------------------------------------------------------------------------

Slot Resolver::slotOf(const Stmt* member, Symbol* source) {
    Slot s;
    s.name = member->selector.text;
    s.source = source;
    s.decl = member;
    s.access = member->access;
    s.distinct = member->distinct;
    s.isConst = member->isConst;
    s.isReadonly = member->isReadonly;
    s.isWeak = member->isWeak;
    if (member->callable) {
        s.isMethod = true;
        std::string params = "(";
        for (size_t i = 0; i < member->params.size(); ++i) {
            if (i) params += ", ";
            params += member->params[i].type ? member->params[i].type->canonical : "?";
        }
        params += ")";
        s.paramsCanon = std::move(params);
        s.retCanon = member->type ? member->type->canonical : "void";
        s.canonical = s.paramsCanon + " -> " + s.retCanon;
    } else {
        s.canonical = member->type ? member->type->canonical : "?";
    }
    return s;
}

// True if class `sub` transitively inherits from class `base` (proper: sub != base).
// Walks the AST base clauses, which are resolved before shapes are merged.
static bool classInheritsFrom(Symbol* sub, Symbol* base) {
    if (!sub || !base || sub == base) return false;
    std::vector<Symbol*> stack;
    auto pushBases = [&](Symbol* c) {
        if (!c || !c->decl) return;
        for (const TypeRefPtr& b : c->decl->bases)
            if (b->resolvedSymbol && b->resolvedSymbol->kind == SymbolKind::Class)
                stack.push_back(b->resolvedSymbol);
    };
    pushBases(sub);
    int guard = 0;
    while (!stack.empty() && ++guard < 100000) {
        Symbol* cur = stack.back(); stack.pop_back();
        if (cur == base) return true;
        pushBases(cur);
    }
    return false;
}

void Resolver::mergeSlot(std::vector<Slot>& slots, Slot incoming) {
    // A collision is same name AND same canonical type (§4). Different name or
    // different type coexist (resolution by type disambiguates at the use site).
    for (Slot& s : slots) {
        if (s.name == incoming.name && s.canonical == incoming.canonical) {
            if (s.distinct || incoming.distinct) {
                // Keep both, per source; one degree of `distinct` marks the pair.
                s.distinct = true;
                incoming.distinct = true;
                slots.push_back(incoming);
            } else if (s.isConst != incoming.isConst) {
                // const.md §4: a same-name+same-type collapse whose sides disagree
                // on constness would leave the merged slot's constness ambiguous —
                // refuse to guess rather than silently pick a side.
                sink_.error(incoming.decl ? incoming.decl->span : SourceSpan{},
                            "'" + std::string(incoming.name) + " : " + incoming.canonical +
                            "' is declared both `const` and non-const across bases; "
                            "mark `distinct` or match the declarations");
            } else if (s.isReadonly != incoming.isReadonly) {
                // techdesign-readonly §4.5: same collision rule, parallel arm —
                // a merged slot's write-once-ness can't be ambiguous either.
                sink_.error(incoming.decl ? incoming.decl->span : SourceSpan{},
                            "'" + std::string(incoming.name) + " : " + incoming.canonical +
                            "' is declared both `readonly` and non-readonly across bases; "
                            "mark `distinct` or match the declarations");
            } else if (s.isWeak != incoming.isWeak) {
                sink_.error(incoming.decl ? incoming.decl->span : SourceSpan{},
                            "'" + std::string(incoming.name) + " : " + incoming.canonical +
                            "' is declared both `weak` and strong across bases; "
                            "mark `distinct` or match the declarations");
            } else if (s.source && incoming.source && s.source != incoming.source &&
                       classInheritsFrom(s.source, incoming.source)) {
                // Diamond override (bug.md #65): both bases share a common
                // ancestor that declares this member; the existing slot is a
                // more-derived OVERRIDE of it (its declaring class inherits from
                // the incoming slot's declaring class), while `incoming` is only
                // the ancestor's copy carried in through the other base. The
                // override wins regardless of base order — do NOT let the base
                // copy replace it. (Two genuinely unrelated declarations still
                // fall to "later base wins" below; an incoming override of the
                // existing base also still wins there.)
            } else {
                s = incoming;               // collapse: later/overriding wins
            }
            return;
        }
    }
    slots.push_back(incoming);
}

// Whole-token replace of `from` with `to` inside a canonical type-text string
// (never a substring match inside a longer identifier — "T" must not touch
// "T2" or "MyT").
static std::string substituteGenericToken(const std::string& text, std::string_view from,
                                          const std::string& to) {
    std::string out;
    out.reserve(text.size());
    auto isIdentChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };
    size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, from.size(), from) == 0 &&
            (i == 0 || !isIdentChar(text[i - 1])) &&
            (i + from.size() == text.size() || !isIdentChar(text[i + from.size()]))) {
            out += to;
            i += from.size();
        } else {
            out += text[i];
            ++i;
        }
    }
    return out;
}

static Slot substituteSlotGenerics(Slot s,
                                   const std::vector<std::pair<std::string_view, std::string>>& subst) {
    for (const auto& [from, to] : subst) {
        s.canonical = substituteGenericToken(s.canonical, from, to);
        s.paramsCanon = substituteGenericToken(s.paramsCanon, from, to);
        s.retCanon = substituteGenericToken(s.retCanon, from, to);
    }
    return s;
}

// `canonical` text is built from the REFERENCE SITE's spelling
// (Resolver::resolveType bakes in `path`/`name` as written, not the resolved
// symbol's own identity), so a dependency's class named through an alias
// (`A::Data::Foo`) and the same class named bare from inside its own package
// (`Foo`, as an interface declared in that package spells it) produce
// different canonical strings for the identical Symbol. Recurses through
// generic arguments so `Promise<A::Data::Foo>` also matches a required
// `Promise<Foo>`, not just the bare non-generic case.
static bool namedTypeSameSymbol(const TypeRef* a, const TypeRef* b) {
    if (!a || !b || a->kind != TypeKind::Named || b->kind != TypeKind::Named)
        return false;
    if (!a->resolvedSymbol || a->resolvedSymbol != b->resolvedSymbol) return false;
    if (a->generics.size() != b->generics.size()) return false;
    for (size_t i = 0; i < a->generics.size(); ++i)
        if (!namedTypeSameSymbol(a->generics[i].get(), b->generics[i].get()))
            return false;
    return true;
}

// F6: covariant return satisfaction is intentionally Resolver-local and
// intentionally narrow.  The declaration's resolved symbol proves that the
// provided return is a declared class/interface type; the canonical strings
// are then compared exactly while walking its bases.  No union/function/type-
// expression parsing is involved.  Generic substitutions are carried along
// each base edge so C : R<int> can satisfy a requirement returning R<int>.
bool Resolver::returnAssignable(const Slot& provided, const Slot& required) const {
    if (provided.retCanon == required.retCanon) return true;
    if (!provided.isMethod || !required.isMethod || !provided.decl ||
        !provided.decl->type || provided.decl->type->kind != TypeKind::Named)
        return false;

    Symbol* providedSym = provided.decl->type->resolvedSymbol;
    if (!providedSym || providedSym->kind != SymbolKind::Class) return false;

    // Same declared type under a different spelling: satisfied regardless of
    // the (possibly stale/unrelated) covariant-base walk below, which exists
    // for a genuine subclass return, not spelling variance of one class.
    if (required.decl && namedTypeSameSymbol(provided.decl->type.get(), required.decl->type.get()))
        return true;

    std::string requiredCanon = required.retCanon;
    constexpr std::string_view optionalSuffix = " | None";
    if (requiredCanon.size() > optionalSuffix.size() &&
        requiredCanon.compare(requiredCanon.size() - optionalSuffix.size(),
                              optionalSuffix.size(), optionalSuffix) == 0)
        requiredCanon.resize(requiredCanon.size() - optionalSuffix.size());
    else if (provided.retCanon.size() > optionalSuffix.size() &&
             provided.retCanon.compare(provided.retCanon.size() - optionalSuffix.size(),
                                       optionalSuffix.size(), optionalSuffix) == 0)
        return false; // an optional value cannot satisfy a required non-optional return

    struct Work {
        Symbol* sym;
        std::vector<std::pair<std::string_view, std::string>> subst;
    };
    std::vector<Work> work;
    Work initial{providedSym, {}};
    if (providedSym->decl)
        for (size_t i = 0;
             i < providedSym->decl->generics.size() &&
             i < provided.decl->type->generics.size(); ++i)
            initial.subst.emplace_back(providedSym->decl->generics[i],
                                       provided.decl->type->generics[i]->canonical);
    work.push_back(std::move(initial));
    std::vector<Work> seen;

    while (!work.empty()) {
        Work cur = std::move(work.back());
        work.pop_back();
        if (!cur.sym || !cur.sym->decl) continue;
        bool visited = std::any_of(seen.begin(), seen.end(), [&](const Work& old) {
            if (old.sym != cur.sym || old.subst.size() != cur.subst.size()) return false;
            for (size_t i = 0; i < old.subst.size(); ++i)
                if (old.subst[i].first != cur.subst[i].first ||
                    old.subst[i].second != cur.subst[i].second)
                    return false;
            return true;
        });
        if (visited) continue;
        seen.push_back(cur);

        for (const TypeRefPtr& base : cur.sym->decl->bases) {
            std::string baseCanon = base->canonical;
            for (const auto& [from, to] : cur.subst)
                baseCanon = substituteGenericToken(baseCanon, from, to);
            if (baseCanon == requiredCanon) return true;

            Symbol* baseSym = base->resolvedSymbol;
            if (!baseSym || baseSym->kind != SymbolKind::Class) continue;
            std::vector<std::pair<std::string_view, std::string>> next;
            if (baseSym->decl) {
                for (size_t i = 0;
                     i < baseSym->decl->generics.size() && i < base->generics.size(); ++i) {
                    std::string actual = base->generics[i]->canonical;
                    for (const auto& [from, to] : cur.subst)
                        actual = substituteGenericToken(actual, from, to);
                    next.emplace_back(baseSym->decl->generics[i], std::move(actual));
                }
            }
            work.push_back({baseSym, std::move(next)});
        }
    }
    return false;
}

// Same idea as returnAssignable's identity fallback, but for the PARAMETER
// list that gates interface-satisfaction candidacy in the first place: a
// provided method whose params are spelled through an alias (`A::Data::Foo`)
// must still be recognized as matching an interface requirement spelled bare
// (`Foo`, as written inside its own package) — a plain paramsCanon string
// compare treats the two spellings as different types.
static bool paramsAssignable(const Slot& provided, const Slot& required) {
    if (provided.paramsCanon == required.paramsCanon) return true;
    if (!provided.decl || !required.decl ||
        provided.decl->params.size() != required.decl->params.size())
        return false;
    for (size_t i = 0; i < provided.decl->params.size(); ++i) {
        const TypeRefPtr& pt = provided.decl->params[i].type;
        const TypeRefPtr& rt = required.decl->params[i].type;
        if (!pt || !rt) return false;
        if (pt->canonical == rt->canonical) continue;
        if (!namedTypeSameSymbol(pt.get(), rt.get())) return false;
    }
    return true;
}

void Resolver::buildShape(Symbol* cls) {
    if (cls->shape.built) return;
    if (cls->shape.building) {
        sink_.error(cls->decl->span,
                    "inheritance cycle involving '" + std::string(cls->name) + "'");
        return;
    }
    cls->shape.building = true;

    std::vector<Slot> slots;
    std::vector<Slot> interfaceReqs;

    for (const TypeRefPtr& base : cls->decl->bases) {
        Symbol* baseSym = base->resolvedSymbol;
        if (!baseSym || baseSym->kind != SymbolKind::Class) continue;
        buildShape(baseSym);
        // Value types are flat (§9): a struct may implement interfaces but not
        // inherit implementation, and nothing may inherit from a struct.
        if (baseSym->isValue)
            sink_.error(cls->decl->span, "cannot inherit from struct '" +
                        std::string(baseSym->name) + "'; value types are final");
        else if (cls->isValue && !baseSym->isInterface())
            sink_.error(cls->decl->span, "struct '" + std::string(cls->name) +
                        "' cannot inherit implementation; it may only implement interfaces");
        // A base/interface's own slots are collected in ITS OWN generic-
        // parameter names (e.g. `class Seq<T> { IIterator<T> iterator(); }`
        // stores `iterator : () -> IIterator<T>`; `interface IIterator<T> {
        // T next(); }` stores `next : () -> T`). A subclass implementing a
        // *parameterized* instantiation under a differently-named parameter
        // (e.g. `class MapSeq<T, U> : Seq<U> { IIterator<U> iterator() ...
        // }`, or `class MapIterator<T, U> : IIterator<U> { U next() ... }`)
        // is correctly overriding/satisfying it — substitute the base's
        // declared generic names with THIS base clause's actual type-
        // argument text before comparing, or a same-shape method spuriously
        // fails to collapse (mergeSlot) / satisfy (the loop below) purely
        // because the two sides spell their type variable differently.
        std::vector<std::pair<std::string_view, std::string>> subst;
        if (baseSym->decl)
            for (size_t i = 0;
                 i < baseSym->decl->generics.size() && i < base->generics.size(); ++i)
                subst.emplace_back(baseSym->decl->generics[i], base->generics[i]->canonical);
        if (baseSym->isInterface() && !cls->isInterface()) {
            // a class implementing an interface: requirements to satisfy.
            for (const Slot& s : baseSym->shape.slots)
                interfaceReqs.push_back(subst.empty() ? s : substituteSlotGenerics(s, subst));
        } else {
            // class base — or an interface extending an interface, which
            // absorbs the base's requirements as its own
            for (const Slot& s : baseSym->shape.slots)
                mergeSlot(slots, subst.empty() ? s : substituteSlotGenerics(s, subst));
        }
    }

    for (const StmtPtr& m : cls->decl->body) {
        if (m->kind != StmtKind::Member) continue;
        if (m->isCtor || m->isGet || m->isSet) continue;   // not shape slots
        mergeSlot(slots, slotOf(m.get(), cls));
    }

    // Interface satisfaction: the class must declare each required member.
    for (const Slot& req : interfaceReqs) {
        bool ok = false;
        const Slot* nearMiss = nullptr;
        for (const Slot& s : slots) {
            if (s.name != req.name) continue;
            if (s.isMethod && req.isMethod && paramsAssignable(s, req)) {
                if (returnAssignable(s, req)) { ok = true; break; }
                if (!nearMiss) nearMiss = &s;
            } else if (s.canonical == req.canonical) {
                ok = true;
                break;
            }
        }
        if (!ok && nearMiss)
            sink_.error(cls->decl->span,
                        "'" + std::string(cls->name) + "' does not satisfy interface: '" +
                        std::string(nearMiss->name) + " : " + nearMiss->canonical +
                        "' found, but return type '" + nearMiss->retCanon +
                        "' is not assignable to required '" + req.retCanon + "'");
        else if (!ok)
            sink_.error(cls->decl->span,
                        "'" + std::string(cls->name) + "' does not satisfy interface: "
                        "missing '" + std::string(req.name) + " : " + req.canonical + "'");
    }

    cls->shape.slots = std::move(slots);
    cls->shape.built = true;
    cls->shape.building = false;
}

// ---------------------------------------------------------------------------
//  driver
// ---------------------------------------------------------------------------

// Extract a constant integer carrier from an enum member's explicit-value expr.
// v1 accepts an integer literal, optionally negated. Anything else -> false.
static bool enumConstInt(const Expr* e, long long& out) {
    if (!e) return false;
    if (e->kind == ExprKind::IntLit) { out = parseIntLiteral(e->text); return true; }
    if (e->kind == ExprKind::Unary && e->op == TokenKind::Minus && e->a &&
        e->a->kind == ExprKind::IntLit) {
        out = -parseIntLiteral(e->a->text); return true;
    }
    return false;
}

// Track 03 §2: lower each top-level or namespace-local `enum` to a value struct + per-member
// mangled const globals + a `fromCode` free function (Option B, §2.2). We emit
// ordinary source text and parse it with the real front-end (zero fragile
// hand-built AST), then rename the member globals + `fromCode` to `$`-mangled
// interned names — the `$` is excluded from user identifiers (Lexer), so those
// names are unreachable from user code and cannot collide. The struct field
// stays lexable (`_ord`); only the *global* names' `$` is load-bearing.
void Resolver::desugarEnums(Program& program) {
    auto lower = [&](auto&& self, std::vector<StmtPtr>& items) -> void {
    std::vector<StmtPtr> out;
    out.reserve(items.size());
    for (StmtPtr& item : items) {
        if (!item || item->kind != StmtKind::Enum) {
            if (item && item->kind == StmtKind::Namespace)
                self(self, item->body);
            out.push_back(std::move(item));
            continue;
        }
        Stmt* en = item.get();

        // carrier (v1: int only; string carriers deferred — deferal-track03-type-surface.md)
        if (en->type && !(en->type->kind == TypeKind::Named && en->type->path.empty() &&
                          en->type->name == "int"))
            sink_.error(en->type->span,
                        "enum carrier must be 'int' in v1 (string carriers are deferred)");

        // resolve carriers + duplicate-value check
        struct M { std::string_view name; long long carrier; };
        std::vector<M> members;
        long long next = 0;
        for (const StmtPtr& mem : en->body) {
            long long c = next;
            if (mem->init && !enumConstInt(mem->init.get(), c)) {
                sink_.error(mem->init->span, "enum carrier value must be an integer literal");
                c = next;
            }
            for (const M& prev : members)
                if (prev.carrier == c)
                    sink_.error(mem->span,
                                "duplicate enum carrier value " + std::to_string(c) +
                                " (members '" + std::string(prev.name) + "' and '" +
                                std::string(mem->name) + "')");
            members.push_back({mem->name, c});
            next = c + 1;
        }
        if (members.empty()) {
            sink_.error(en->span, "enum '" + std::string(en->name) + "' has no members");
            continue;   // drop the empty enum entirely
        }

        std::string N(en->name);
        auto lit = [](long long v) { return std::to_string(v); };

        // --- generate desugar source (lexable placeholder names) ---
        std::string src;
        src += "struct " + N + " {\n";
        src += "    int _ord = " + lit(members[0].carrier) + ";\n";      // default = first member
        src += "    new " + N + "(int c) { _ord = c; }\n";
        src += "    int code() => _ord;\n";
        src += "    string toString() => match (_ord) {\n";
        for (const M& m : members)
            src += "        " + lit(m.carrier) + " => \"" + std::string(m.name) + "\";\n";
        src += "        else => \"\";\n    };\n";
        src += "    bool (==)(" + N + " o) => _ord == o._ord;\n";
        src += "}\n";
        for (size_t i = 0; i < members.size(); ++i)
            src += N + " __enumg_" + N + "_" + lit((long long)i) + " = " +
                   N + "(" + lit(members[i].carrier) + ");\n";
        src += N + "? __enumfc_" + N + "(int c) => match (c) {\n";
        for (const M& m : members)
            src += "    " + lit(m.carrier) + " => " + N + "(" + lit(m.carrier) + ");\n";
        src += "    else => None;\n};\n";

        // --- parse with the real front-end ---
        program.synthFiles.push_back(SourceFile{"<enum " + N + ">", std::move(src)});
        SourceFile& sf = program.synthFiles.back();
        DiagnosticSink dummy;
        Lexer lexer(sf, dummy);
        Parser parser(lexer.tokenize(), sf, dummy);
        Program sub = parser.parseProgram();
        if (dummy.hasErrors())
            sink_.error(en->span, "internal: enum '" + N + "' desugar failed to parse");

        // --- rename mangled decls, record metadata, splice in place ---
        EnumDesugar meta;
        meta.name = en->name;
        size_t gi = 0;
        for (StmtPtr& si : sub.items) {
            if (si->kind == StmtKind::Var) {                 // a per-member global
                program.synthNames.push_back(N + "$" + std::string(members[gi].name));
                si->name = program.synthNames.back();
                si->isConst = true;
                meta.members.push_back({members[gi].name, members[gi].carrier, si.get()});
                gi++;
            } else if (si->kind == StmtKind::Member && si->callable) {   // fromCode
                program.synthNames.push_back(N + "$fromCode");
                si->name = program.synthNames.back();
                si->selector.text = si->name;
                meta.fromCode = si.get();
            } else if (si->kind == StmtKind::Class) {
                si->access = en->access;                     // the enum's struct
            }
            out.push_back(std::move(si));
        }
        program.enumDesugars.push_back(std::move(meta));
    }
    items = std::move(out);
    };
    lower(lower, program.items);
}

// ---------------------------------------------------------------------------
//  derived struct (==) synthesis (struct-equality §5.5, packet 02)
// ---------------------------------------------------------------------------

// An explicit symbolic `(==)` member — the author's, a rule-injected one, or
// the one desugarEnums generates. Synthesized members never count (they are
// stripped and regenerated each pass).
static bool hasExplicitEq(const Stmt* cls) {
    for (const StmtPtr& m : cls->body)
        if (m->kind == StmtKind::Member && m->callable && !m->isSynthEq &&
            m->selector.symbolic && m->selector.text == "==")
            return true;
    return false;
}

// A struct data field: a non-callable member. Ctors/accessors are callable,
// so the extra guards are belt-and-braces.
static bool isDataField(const Stmt* m) {
    return m->kind == StmtKind::Member && !m->callable && !m->isCtor &&
           !m->isGet && !m->isSet;
}

namespace {
// The §5.2 comparability ladder over resolved field TypeRefs. classify()
// returns "" for comparable, else the parenthetical for the checker's §5.1
// message ("a function value", "an Array", ...). Struct verdicts memoize per
// symbol; the in-progress state fails closed on re-entry (value structs
// cannot cycle by value — infinite size is already rejected — so this only
// guards error-recovery states).
struct EqLadder {
    const std::unordered_map<const Symbol*, const char*>& banned;
    enum V { kInProgress = 1, kYes, kNo };
    std::unordered_map<const Symbol*, int> verdicts;

    std::string classify(const TypeRef* t) {
        if (!t) return "an untyped field";
        switch (t->kind) {
            case TypeKind::Inferred: return "an inferred type";
            case TypeKind::Function: return "a function value";
            case TypeKind::Union:
                // Comparable iff every non-None member is; the None leg is a
                // tag compare that already works end-to-end.
                for (const TypeRefPtr& m : t->members) {
                    if (m->kind == TypeKind::Named && m->path.empty() && m->name == "None")
                        continue;
                    if (std::string bad = classify(m.get()); !bad.empty()) return bad;
                }
                return "";
            case TypeKind::Named: {
                if (t->path.empty() && t->name == "None") return "";
                const Symbol* sym = t->resolvedSymbol;
                if (!sym) return "an unresolvable type";   // fail closed, never silently
                if (sym->kind == SymbolKind::TypeParam) return "a type parameter";
                if (sym->kind != SymbolKind::Class) return "a non-comparable type";
                if (auto b = banned.find(sym); b != banned.end()) return b->second;
                if (sym->isPrimitive) return "";           // int/bool/char/string/float
                if (sym->isValue)
                    return structComparable(sym)
                               ? ""
                               : "a non-comparable struct ('" + std::string(sym->name) + "')";
                return "";   // reference class / interface: identity compare
            }
        }
        return "an unresolvable type";
    }

    bool structComparable(const Symbol* sym) {
        if (auto it = verdicts.find(sym); it != verdicts.end()) return it->second == kYes;
        if (!sym->decl) return false;
        if (hasExplicitEq(sym->decl)) { verdicts[sym] = kYes; return true; }
        if (!sym->decl->generics.empty()) { verdicts[sym] = kNo; return false; }  // v1 restriction
        verdicts[sym] = kInProgress;
        bool ok = true;
        for (const StmtPtr& m : sym->decl->body)
            if (isDataField(m.get()) && !classify(m->type.get()).empty()) { ok = false; break; }
        verdicts[sym] = ok ? kYes : kNo;
        return ok;
    }
};
}  // namespace

void Resolver::synthesizeStructEquality(Program& program) {
    // Erase-then-regenerate (idempotency & the two-pass resolver): rules can
    // inject fields or an explicit (==) between the passes, so every prior
    // synthesized member is dropped FIRST — everywhere, before any
    // classification reads a struct body — then the truth is rebuilt from the
    // current field lists.
    program.structEqSynths.clear();
    auto strip = [](auto&& self, std::vector<StmtPtr>& items) -> void {
        for (StmtPtr& it : items) {
            if (!it) continue;
            if (it->kind == StmtKind::Namespace) { self(self, it->body); continue; }
            if (it->kind == StmtKind::Class)
                it->body.erase(std::remove_if(it->body.begin(), it->body.end(),
                                              [](const StmtPtr& m) { return m->isSynthEq; }),
                               it->body.end());
        }
    };
    strip(strip, program.items);

    // §5.2's named exclusions, resolved to their prelude symbols — a user's
    // own namespaced `Array` is an ordinary reference class and stays
    // identity-comparable. Function types are TypeKind::Function, not a name.
    const std::pair<const char*, const char*> kBanned[] = {
        {"Array", "an Array"},     {"Map", "a Map"},         {"Block", "a Block"},
        {"Ast", "an Ast"},         {"Promise", "a Promise"}, {"Channel", "a Channel"}};
    std::unordered_map<const Symbol*, const char*> banned;
    for (const auto& [n, note] : kBanned)
        if (Symbol* s = findLocal(sema_.global, n, SymbolKind::Class)) banned[s] = note;

    EqLadder ladder{banned};

    auto walk = [&](auto&& self, std::vector<StmtPtr>& items, Scope* scope) -> void {
        for (StmtPtr& it : items) {
            if (!it) continue;
            if (it->kind == StmtKind::Namespace) {
                if (Symbol* ns = findLocal(scope, it->name, SymbolKind::Namespace);
                    ns && ns->scope)
                    self(self, it->body, ns->scope);
                continue;
            }
            if (it->kind != StmtKind::Class || !it->isValue || it->isAttribute) continue;
            Stmt* cls = it.get();
            if (hasExplicitEq(cls)) continue;   // §5.4: the author's relation wins
                                                // (covers desugared enums too)

            program.synthNames.push_back(std::string(cls->name));
            StructEqSynth rec;
            rec.structName = program.synthNames.back();

            // v1: no derived (==) for generic structs (the self-type spelling
            // with type params is deferred; no existing corpus exercises it).
            if (!cls->generics.empty()) {
                rec.badKindNote = "a generic struct (derived '(==)' is not synthesized in v1)";
                program.structEqSynths.push_back(std::move(rec));
                continue;
            }

            // Classify every field; the first non-comparable one gates the
            // struct. No diagnostic here — the gate fires at a use site
            // (packet 03), not at the declaration.
            std::string note;
            const Stmt* bad = nullptr;
            for (const StmtPtr& m : cls->body) {
                if (!isDataField(m.get())) continue;
                note = ladder.classify(m->type.get());
                if (!note.empty()) { bad = m.get(); break; }
            }
            if (bad) {
                rec.badField = std::string(bad->selector.text);
                rec.badKindNote = std::move(note);
                program.structEqSynths.push_back(std::move(rec));
                continue;
            }

            // Generate + parse through the desugarEnums channel (the wrapper
            // struct gives the parser a member context), then lift the member
            // out and splice it into the real struct.
            std::string N(cls->name);
            std::string body;
            for (const StmtPtr& m : cls->body) {
                if (!isDataField(m.get())) continue;
                std::string f(m->selector.text);
                if (!body.empty()) body += " && ";
                // struct-equality design §5.2: float fields compare through the
                // canonical relation (§3), so a struct holding NaN is equal to
                // itself and ±0.0 agree — via float.canonEq (each engine's ONE
                // canon). Every other field kind keeps the scalar `==`.
                const TypeRef* t = m->type.get();
                bool isFloat = t && t->kind == TypeKind::Named && t->path.empty() &&
                               t->name == "float" &&
                               (!t->resolvedSymbol || t->resolvedSymbol->isPrimitive);
                if (isFloat)
                    body += "this." + f + ".canonEq(other." + f + ")";
                else
                    body += "this." + f + " == other." + f;
            }
            if (body.empty()) body = "true";   // zero-field struct: reflexively equal
            std::string src = "struct __eq_" + N + " {\n"
                              "    bool (==)(" + N + " other) => " + body + ";\n"
                              "}\n";

            program.synthFiles.push_back(SourceFile{"<eq " + N + ">", std::move(src)});
            SourceFile& sf = program.synthFiles.back();
            DiagnosticSink dummy;
            Lexer lexer(sf, dummy);
            Parser parser(lexer.tokenize(), sf, dummy);
            Program sub = parser.parseProgram();
            Stmt* wrapper = (sub.items.size() == 1 && sub.items[0]->kind == StmtKind::Class)
                                ? sub.items[0].get() : nullptr;
            if (dummy.hasErrors() || !wrapper || wrapper->body.size() != 1) {
                sink_.error(cls->span,
                            "internal: struct '" + N + "' (==) synthesis failed to parse");
                rec.badKindNote = "an internal synthesis failure";
                program.structEqSynths.push_back(std::move(rec));
                continue;
            }
            StmtPtr method = std::move(wrapper->body[0]);
            method->isSynthEq = true;
            // This pass runs after resolveTypesIn, so resolve the new member's
            // types by hand against the real class scope (param `N`, ret bool).
            if (Symbol* sym = findLocal(scope, cls->name, SymbolKind::Class);
                sym && sym->scope)
                resolveMember(method.get(), sym->scope);
            cls->body.push_back(std::move(method));
            rec.synthesized = true;
            program.structEqSynths.push_back(std::move(rec));
        }
    };
    walk(walk, program.items, sema_.global);
}

// Struct-equality §6 (packet 06): the `float::NaN` language constant. One
// definition, one place — synthesized through the same synth channel the enum
// member globals use (desugarEnums above), so it flows through gather / type
// resolution / check / global-init exactly like hand-written source and every
// engine reads it with zero per-engine work. Decision 1 (ratified): the bit
// pattern is a LANGUAGE constant — 0x7FF8000000000000, the canonical positive
// quiet NaN (§3.1) — no per-target configuration, no `#ifdef`.
void Resolver::synthesizeFloatNaN(Program& program) {
    if (program.floatNaNGlobal) return;   // idempotent: the two-pass resolver
                                          // re-runs on the SAME Program.
    // Materialize the global ONLY when the program actually references
    // `float::NaN` — exactly the enum precedent (enum globals appear only when
    // an `enum` is written). Unconditional injection would push a
    // `float::fromBits(...)` initializer into every program, including the
    // churn/expand corpora whose FROZEN ELF lane cannot lower that native and
    // whose expand-roundtrip re-parses the printed source. `file_.text` is the
    // whole combined source buffer (every user file concatenated); a token scan
    // for the `float :: NaN` sequence is spacing-robust and skips comments and
    // string bodies (the lexer already did). The one language constant, but
    // paid for only where used.
    DiagnosticSink scanSink;
    std::vector<Token> toks = Lexer(file_, scanSink).tokenize();
    bool used = false;
    for (size_t i = 0; i + 2 < toks.size(); ++i)
        if (toks[i].text == "float" && toks[i + 1].kind == TokenKind::ColonColon &&
            toks[i + 2].text == "NaN") { used = true; break; }
    if (!used) return;

    // The `$` in the final mangled name is unlexable in user identifiers, so
    // parse a lexable placeholder first, then rename to `float$NaN` (backed by
    // synthNames for string_view stability) — exactly the desugarEnums move.
    // 9221120237041090560 == 0x7FF8000000000000 (fits a signed int64).
    std::string src = "float __floatnan = float::fromBits(9221120237041090560);\n";
    program.synthFiles.push_back(SourceFile{"<float::NaN>", std::move(src)});
    SourceFile& sf = program.synthFiles.back();
    DiagnosticSink dummy;
    Lexer lexer(sf, dummy);
    Parser parser(lexer.tokenize(), sf, dummy);
    Program sub = parser.parseProgram();
    if (dummy.hasErrors() || sub.items.size() != 1 ||
        sub.items[0]->kind != StmtKind::Var) {
        sink_.error(SourceSpan{}, "internal: float::NaN constant synthesis failed to parse");
        return;
    }
    StmtPtr g = std::move(sub.items[0]);
    program.synthNames.push_back("float$NaN");
    g->name = program.synthNames.back();
    g->isConst = true;                    // follows the enum member globals
    program.floatNaNGlobal = g.get();
    // Prepend, not append: top-level global initializers run in source order,
    // so the constant must be initialized BEFORE any user top-level statement
    // (e.g. `main();`) that reads `float::NaN`. Enum member globals sit at their
    // enum's source position (ahead of the call site); this one has no natural
    // site, so it leads.
    program.items.insert(program.items.begin(), std::move(g));
}

void Resolver::run(Program& program) {
    desugarEnums(program);                    // Track 03 §2: enum -> struct + globals + fromCode
    synthesizeFloatNaN(program);              // struct-equality §6: the float::NaN constant
    sema_.global = sema_.newScope(nullptr);
    // `void` is the only pure primitive with no method surface; int/string/bool/
    // float are declared as value-type classes in the prelude (the object mask).
    addToScope(sema_.global, sema_.newSymbol(SymbolKind::Primitive, "void"));
    addToScope(sema_.global, sema_.newSymbol(SymbolKind::Primitive, "None"));

    // #98: pass 2 adopts the rule-mutated prelude tree (its spans point into the
    // pass-1 resolver's still-alive prelude buffer); otherwise parse fresh.
    preludeProgram_ = haveAdoptedPrelude_ ? std::move(adoptedPrelude_) : parsePrelude();
    gatherInto(preludeProgram_.items, sema_.global);
    // Boundary between prelude and program classes in `classSymbols_`: only the
    // program's classes get file-scoped below (prelude spans live in a separate
    // buffer whose offsets would otherwise collide with the program's).
    size_t preludeClassCount = classSymbols_.size();
    gatherInto(program.items, sema_.global);

    for (const char* p : {"int", "string", "bool", "float", "char"})
        if (Symbol* s = findLocal(sema_.global, p, SymbolKind::Class)) s->isPrimitive = true;

    // Implicit `uses std;` — the standard namespace is always imported. It lives
    // in `global` (visible in every file), not in any one file's overlay.
    if (Symbol* stdNs = findLocal(sema_.global, "std", SymbolKind::Namespace))
        for (auto& [name, syms] : stdNs->scope->names)
            for (Symbol* sym : syms)
                sema_.global->names[name].push_back(sym);

    // Per-file import overlays (bug.md #8). One scope per source file (a child
    // of `global`); a lone source becomes one file covering the whole buffer.
    if (fileRanges_.empty())
        fileRanges_.push_back({0u, (uint32_t)file_.text.size()});
    sema_.fileRanges = fileRanges_;
    sema_.fileScopes.clear();
    for (size_t i = 0; i < fileRanges_.size(); ++i)
        sema_.fileScopes.push_back(sema_.newScope(sema_.global));

    // Re-parent each TOP-LEVEL program class scope (parent == global) to its
    // file's overlay, so file-level `uses` are visible inside the class and a
    // class in one file never sees another file's top-level imports. Namespaced
    // classes (parent == a namespace scope) keep resolving through their
    // namespace; prelude classes are left alone (they live in `global`).
    for (size_t i = preludeClassCount; i < classSymbols_.size(); ++i) {
        Symbol* cls = classSymbols_[i];
        if (cls->scope && cls->scope->parent == sema_.global && cls->decl)
            cls->scope->parent = sema_.fileScopeFor(cls->decl->span.offset);
    }

    // Likewise make each TOP-LEVEL namespace scope see its files' overlays, so a
    // top-of-file `uses B` is visible to code inside a reopened `namespace A {
    // ... }` block — the namespace scope chain otherwise runs straight to global,
    // bypassing the overlay where `uses` names live (bug.md #45).
    //
    // A namespace reopened across files (`namespace Helm` in both proc.lev and
    // command.lev) has ONE shared scope but N blocks, each in its own file with
    // its own top-of-file `uses`. Parenting that single scope to just the FIRST
    // block's overlay made the whole namespace see only that one file's imports:
    // if the first block's file lacked a `uses Sonar` that a later block relies
    // on, every unqualified cross-dependency type in the namespace failed to
    // resolve, order-dependently (regression floor:
    // tests/corpus/project/reopen_ns_uses_order). So instead give each reopened
    // namespace its OWN aggregate overlay (parent == global) and fold in the
    // file overlays of EVERY block, making import visibility order-independent.
    // The parent-== global guard makes one aggregate per shared namespace scope;
    // nested namespaces keep resolving through their encloser.
    std::unordered_map<Symbol*, Scope*> nsAgg;
    std::unordered_map<Symbol*, std::vector<Scope*>> nsBlockOverlays;
    for (StmtPtr& item : program.items) {
        if (item->kind != StmtKind::Namespace) continue;
        Symbol* ns = findLocal(sema_.global, item->name, SymbolKind::Namespace);
        if (!ns || !ns->scope) continue;
        Scope* fileOv = sema_.fileScopeFor(item->span.offset);
        std::vector<Scope*>& ovs = nsBlockOverlays[ns];
        if (std::find(ovs.begin(), ovs.end(), fileOv) == ovs.end()) ovs.push_back(fileOv);
        if (ns->scope->parent == sema_.global) {
            Scope* agg = sema_.newScope(sema_.global);
            ns->scope->parent = agg;
            nsAgg[ns] = agg;
        }
    }

    processImports(program.items, sema_.global);   // resolve `uses` before type resolution

    // Populate each namespace's aggregate overlay from ALL its blocks' file
    // overlays (which processImports has now filled). File overlays hold only
    // imported names, so this copies imports and nothing else.
    for (auto& [ns, agg] : nsAgg)
        for (Scope* ov : nsBlockOverlays[ns])
            for (auto& [name, syms] : ov->names)
                for (Symbol* sym : syms)
                    agg->names[name].push_back(sym);

    resolveTypesIn(preludeProgram_.items, sema_.global);
    resolveTypesIn(program.items, sema_.global);

    // block-scoped-use §3.2(b): register top-level (program-wide) and
    // namespace-body factory binds into their scopes' bind tables, now that
    // their bound types are resolved (canonical keys). Program only — prelude
    // binds reach the checker via the Channel-1 index (namespaceBinds_), never
    // the top-level frame, exactly as before. Block binds were filled per-block.
    fillDeclBinds(program.items, sema_.global);

    // Struct equality §5.5 (packet 02): after types resolve (field
    // classification reads resolvedSymbol), before shapes (the spliced member
    // becomes an ordinary "==" slot).
    synthesizeStructEquality(program);

    for (Symbol* cls : classSymbols_) buildShape(cls);
}

std::string Resolver::dumpShapes() const {
    std::string out;
    for (const std::unique_ptr<Symbol>& sp : sema_.symbols) {
        const Symbol* cls = sp.get();
        if (cls->kind != SymbolKind::Class || !cls->decl) continue;
        out += "Shape " + std::string(cls->name);
        if (cls->isInterface()) out += " (interface)";
        out += "\n";
        for (const Slot& s : cls->shape.slots) {
            out += "  ";
            if (s.distinct) out += "distinct ";
            out += std::string(s.name) + " : " + s.canonical;
            if (s.source && s.source != cls) out += "  (from " + std::string(s.source->name) + ")";
            if (s.isMethod) out += "  [method]";
            out += "\n";
        }
    }
    return out;
}
