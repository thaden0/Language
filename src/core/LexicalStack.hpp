#pragma once
#include "core/Symbols.hpp"
#include <string>
#include <unordered_map>
#include <vector>

// designs/complete/techdesign-block-scoped-use.md §3.2 — the traversal side of the
// per-block lexical-scope substrate. One stack, pushed/popped per open block
// (and, in the Checker, per namespace body plus once program-wide), retiring
// the Checker's old private bind stack and the Lowerer's private block-import
// stack in favor of one shared object over the substrate's scope tables.
//
// Each frame points at a `Scope` whose `names`/`binds` tables the Resolver
// already filled (registration is the substrate's job now, not each consumer's).
// The frame's own `activated` overlay carries the system-binds.md §5.3 Channel-1
// binds that a `use NS::T;` activates at check time — kept out of the shared
// `Scope` so a bulk `uses` (which copies only `names`) can never pull them in,
// preserving the owner ruling that binds never enter the shared name tables.
struct LexicalFrame {
    Scope* scope = nullptr;                                     // nullptr = a block with no scope
    std::unordered_map<std::string, const Stmt*> activated;     // Channel-1 (§5.3)
};

struct LexicalStack {
    std::vector<LexicalFrame> frames;

    void pushScope(Scope* s) { frames.push_back(LexicalFrame{s, {}}); }
    void pop() { if (!frames.empty()) frames.pop_back(); }
    bool empty() const { return frames.empty(); }

    // Nearest-wins bind lookup. Within a frame a textual (Resolver-registered)
    // bind beats an activated one; across frames the innermost wins — the exact
    // order the old per-scope bind maps produced (each held its textual binds
    // first, activation filled only the gaps).
    const Stmt* lookupBind(const std::string& canonical) const {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            if (it->scope)
                if (const Stmt* b = it->scope->localBind(canonical)) return b;
            auto a = it->activated.find(canonical);
            if (a != it->activated.end()) return a->second;
        }
        return nullptr;
    }

    // The namespace `name` binds to across the open block frames, innermost
    // first. Returns true when some frame mentions `name` (setting `out` to the
    // namespace symbol, or null if that binding is shadowed by a non-namespace)
    // — the same break-on-first-hit rule the Lowerer's namespaceSym used, so the
    // caller stops and does NOT fall through to the file overlay. Returns false
    // when no frame mentions it (fall through).
    bool namespaceInFrames(std::string_view name, Symbol*& out) const {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it) {
            if (!it->scope) continue;
            if (const std::vector<Symbol*>* v = it->scope->localLookup(name)) {
                out = nullptr;
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Namespace) { out = s; break; }
                return true;
            }
        }
        return false;
    }
};
