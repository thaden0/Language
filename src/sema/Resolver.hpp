#pragma once
#include "core/Ast.hpp"
#include "core/Diagnostic.hpp"
#include "frontend/Lexer.hpp"
#include "frontend/Parser.hpp"
#include "core/Symbols.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Builds the semantic model from a parsed Program:
//   1. gather   — scopes + symbols for every declaration (whole-program),
//                 plus a prelude of primitives and library type stubs.
//   2. resolve  — every type reference to its declaration (or a diagnostic).
//   3. shapes   — each class's slot layout, with collision detection
//                 (distinct vs collapse) and interface-satisfaction checks.
class Resolver {
public:
    Resolver(const SourceFile& file, DiagnosticSink& sink) : file_(file), sink_(sink) {}

    // The [offset, end) ranges of each source file in the combined buffer, in
    // manifest order (bug.md #8: top-level `uses` scopes to its own file). A
    // lone source needs no call — `run` defaults to one file covering the whole
    // buffer.
    void setFileRanges(std::vector<std::pair<uint32_t, uint32_t>> ranges) {
        fileRanges_ = std::move(ranges);
    }

    // Ship-as-files prelude (techdesign-prelude-ship-as-files-opus.md §4.2).
    // Both default to "" so any Resolver constructed without these calls gets
    // the embedded native prelude and keeps working — R1's zero-config guarantee.
    void setPreludeDir(std::string dir) { preludeDir_ = std::move(dir); }
    void setTargetTriple(std::string triple) { targetTriple_ = std::move(triple); }

    // #98: adopt an already-parsed prelude tree instead of re-parsing a fresh
    // one in run(). Used only by main.cpp's pass-2 resolver when the rule stage
    // rewrote a prelude declaration — the mutated prelude AST the rule engine
    // produced must be the one re-resolved and lowered, not a pristine re-parse
    // that would drop the injection. The tree's identifier string_views still
    // point into the pass-1 resolver's prelude source buffer, which outlives
    // this resolver (both are alive through lowering in main.cpp).
    void adoptPrelude(Program&& prog) {
        adoptedPrelude_ = std::move(prog);
        haveAdoptedPrelude_ = true;
    }

    void run(Program& program);
    const Sema& sema() const { return sema_; }
    Program& preludeProgram() { return preludeProgram_; }   // for IR lowering

    // Render each class's computed shape (for --shapes and tests).
    std::string dumpShapes() const;

private:
    const SourceFile& file_;
    DiagnosticSink& sink_;
    Sema sema_;

    // Prelude source kept alive because symbols/tokens reference its text.
    SourceFile preludeFile_;
    Program preludeProgram_;
    Program adoptedPrelude_;             // #98: set by adoptPrelude(), consumed in run()
    bool haveAdoptedPrelude_ = false;
    std::vector<Symbol*> classSymbols_;   // every class, in gather order
    std::vector<std::pair<uint32_t, uint32_t>> fileRanges_;   // per-file spans (bug.md #8)
    std::string preludeDir_;    // "" => use the embedded prelude fallback
    std::string targetTriple_;  // "" => host/native target

    // gather
    void gatherInto(std::vector<StmtPtr>& items, Scope* scope, Symbol* enclosingNs = nullptr);
    void gatherClass(Stmt* cls, Scope* parent);
    Program parsePrelude();

    // Track 03 §2: lower every top-level `enum` in `program.items` to a value
    // struct + per-member mangled const globals + a `fromCode` free function,
    // splicing the results in place and recording metadata on the Program. Runs
    // before gather so the synthesized decls resolve/check like hand-written code.
    void desugarEnums(Program& program);

    // Struct-equality §5.5 (packet 02): for every user value struct with no
    // explicit `(==)`, either splice a derived field-wise `(==)` member into
    // its body (all fields comparable per the §5.2 ladder) or record why not
    // on Program::structEqSynths for the checker's use-site gate (packet 03).
    // Runs after type resolution (classification reads resolvedSymbol) and
    // before shapes (the spliced member becomes an ordinary "==" slot).
    void synthesizeStructEquality(Program& program);

    // Struct-equality §6 (packet 06): synthesize the single `float::NaN`
    // language constant as a const global through the synth channel, parking
    // its decl on Program::floatNaNGlobal for the checker. Runs before gather
    // (like desugarEnums) so the global resolves/checks/inits like hand-written
    // code; idempotent across the two resolver passes.
    void synthesizeFloatNaN(Program& program);

    // imports (`uses` / `use`) — lexically scoped (bug.md #8, imports.md).
    void processImports(std::vector<StmtPtr>& items, Scope* scope);  // top-level + namespace bodies
    void importOne(Stmt* usesStmt, Scope* into);   // resolve one `uses` into `into`
    void useOne(Stmt* useStmt, Scope* into);       // resolve one selective `use` into `into`
    bool hasImports(const std::vector<StmtPtr>& items) const;  // any `uses`/`use` directly here?
    bool hasFactoryBinds(const std::vector<StmtPtr>& items) const;  // any factory `bind` directly here?
    // Record each direct-child factory `bind` into `scope->binds`, keyed by the
    // bound type's canonical string (block-scoped-use §3.2(b)). First-declared
    // wins on a duplicate (reported here) — the same grain the Checker's bind
    // stack used.
    void fillBinds(const std::vector<StmtPtr>& items, Scope* scope);
    // Fill the declaration-scope bind tables: top-level factory binds into
    // `global` (program-wide, §4.3), each namespace body's into its own scope.
    // Recurses through namespaces only — block binds are filled per-block in the
    // Block case of resolveStmtTypes.
    void fillDeclBinds(std::vector<StmtPtr>& items, Scope* scope);

    // resolve
    void resolveTypesIn(std::vector<StmtPtr>& items, Scope* scope);
    void resolveMember(Stmt* m, Scope* classScope);
    void resolveStmtTypes(Stmt* s, Scope* scope);
    void resolveExprTypes(Expr* e, Scope* scope);
    std::string resolveType(TypeRef* t, Scope* scope);
    // 005 R2/R3: reclassify a `::`-qualified `match`-arm value pattern to a type
    // pattern when the leaf resolves to a type (not an enum member / const).
    void reclassifyMatchArm(MatchArm& arm, Scope* scope);

    // shapes
    void buildShape(Symbol* cls);
    void mergeSlot(std::vector<Slot>& slots, Slot incoming);
    Slot slotOf(const Stmt* member, Symbol* source);
    bool returnAssignable(const Slot& provided, const Slot& required) const;
};
