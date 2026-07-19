#pragma once
#include "provider.hpp"
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// tools/trident/mvs.{hpp,cpp} — Minimal Version Selection (techdesign-
// package-manager.md §3.2/§5.1 P2.1a, proposal §2.2/§8.3): "selected = max
// over requirers of (min version)". Deterministic, unique, no backtracking —
// no ranges, no SAT, no "newest available" (§0.3e: replacing this algorithm
// is a STOP event). The resolver reaches source ONLY through a
// ModuleProvider (provider.hpp, §3.3 rule 1), so it is unit-testable offline
// against a fake in-memory provider before any git/network code exists.
// -----------------------------------------------------------------------------

// Convert one Vcs-kind manifest Dependency into a Require edge (ModuleId +
// minimum Version). Call only for deps already known to be Vcs-kind — a
// Local-kind dep carries no semver identity and does not participate in MVS
// (it resolves in place via the existing loadDepsRec path, resolve.cpp, when
// its owning module builds). The only failure mode here is an unparseable
// `version`.
bool dependencyToRequire(const Dependency& d, Require& out, std::string& err);

struct MvsResult {
    bool ok = false;
    std::vector<BuildListEntry> buildList;   // sorted by ModuleId — deterministic
    std::string err;
};

// Return "" when `entries`' recorded require graph is acyclic; otherwise
// return the first cycle as a rendered chain such as
// "path@1.2.0 -> other@1.0.0 -> path@1.2.0". Deterministic for a given
// build list: entries are visited in their existing (ModuleId-sorted) order
// and requires in recorded order. An edge whose ModuleId has no entry is a
// leaf, not a cycle.
std::string findRequireCycle(const std::vector<BuildListEntry>& entries);

// Run MVS over the require graph rooted at `rootRequires`, reading each
// visited module's own `requires` via `provider.manifestOf()` (never
// `materialize()` — H-2: fetch just the manifest to walk the graph; only the
// finally-selected build list is ever materialized, by a later, separate
// step). A manifest reached this way is, by construction, already inside
// VCS-dependency territory — kind inference ("does this path exist as a
// local directory") cannot run on it (there is no local base dir before
// materialize()) — so every dep it declares is treated as a further Vcs
// requirement unconditionally. (The root project's own manifest is the one
// place Local and Vcs deps legitimately mix; splitting that is the caller's
// job — resolve.cpp's P2.1e integration — not this function's.)
//
// Deterministic: the same `rootRequires` + provider responses always produce
// a byte-identical `buildList` (sorted by ModuleId), regardless of graph-walk
// order. `BuildListEntry.contentHash`/`storeDir` are left empty — selection
// only; a later step (P2.1e) materializes the finally-selected versions.
MvsResult selectVersions(const std::vector<Require>& rootRequires, ModuleProvider& provider);
