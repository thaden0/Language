#pragma once
#include "core/Source.hpp"
#include "core/Diagnostic.hpp"
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// The frozen trident -> leviathan build-plan contract (techdesign-toolchain.md
// §3.3). trident emits it; leviathan reads it here, with a small DEDICATED
// reader — this is NOT the project{} manifest grammar (that parser, and every
// manifest/dependency concept, lives in trident, techdesign-toolchain.md §3.2).
//
// The plan is fully resolved: every `src.path` is absolute and already on
// disk. leviathan trusts it completely — no fetching, no dependency
// resolution, no path resolution beyond opening the files (contract rule 1).
//
//     plan {
//         out      = "<output path>";
//         mode     = "build-native" | "build" | "emit-llvm" | "run" | "check" | …;
//         target   = "<triple>";              // "" = host
//         optLevel = 0 | 2;
//
//         entry { kind = "script" | "file" | "function"; target = "…"; }
//
//         src  { path = "<abs>"; moduleId = ""; origin = ""; }
//         edge { from = ""; to = "lib"; }
//         asset { rel = "views/index.html"; path = "<abs>"; moduleId = "";
//                 hash = "sha256:ab12…"; }
//     }
//
// entry.kind is explicit — trident tells leviathan the entry mode; leviathan
// never sniffs a filename extension to guess it (contract rule 2). For a File
// entry, `entry.target` is the exact string that also appears as some src
// row's `path` (both absolute), so leviathan matches it by plain equality —
// no path resolution needed on the compiler side.
// -----------------------------------------------------------------------------

enum class PlanEntryKind { Script, File, Function };

struct PlanSource {
    std::string path;       // absolute, already on disk (trident guarantees it)
    std::string moduleId;   // "" = root project
    std::string origin;     // "" for the project itself; else the dependency's
                            // module path (so diagnostics can name the dep)
};

// `from` may `uses` any namespace `to` declares (moduleId, "" = root).
struct PlanEdge {
    std::string from;
    std::string to;
};

// LA-20 §7: one declared build-input asset (a comptime `import()` target),
// resolved and hashed by trident. `rel` is the exact string `import()`
// matches against; leviathan trusts `hash` completely (contract rule 1 —
// it never re-hashes).
struct PlanAsset {
    std::string rel;
    std::string path;
    std::string moduleId;
    std::string hash;
};

struct BuildPlan {
    bool ok = false;
    SourceFile planFile;      // kept so plan diagnostics can render

    std::string out;
    std::string mode;
    std::string target;
    int optLevel = 2;

    PlanEntryKind entryKind = PlanEntryKind::Script;
    std::string entryTarget;  // file path or function name; "" for Script

    std::vector<PlanSource> sources;
    std::vector<PlanEdge> edges;
    std::vector<PlanAsset> assets;   // LA-20: declared build-input assets
};

// Read and parse a build plan's text. Reports errors to `sink` (spans into the
// returned BuildPlan's `planFile`). Returns a BuildPlan with ok == false on
// any error (unreadable file, malformed grammar, missing `entry`/`src` rows).
BuildPlan readBuildPlan(const std::string& planPath, DiagnosticSink& sink);
