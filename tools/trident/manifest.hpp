#pragma once
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// trident/manifest.{hpp,cpp} — the project manifest format (techdesign-
// toolchain.md §3.2/§5.2). TOML, default filename `trident.toml` (a fixed,
// Cargo-style name — not name-parameterized). trident owns this format
// exclusively; leviathan never parses it (it only reads the resolved build
// plan, src/BuildPlan.hpp).
//
// A small hand-rolled reader covers exactly the subset the manifest needs —
// no external TOML dependency (trident stays dependency-free, matching the
// rest of the toolchain):
//
//     name    = "hello"
//     entry   = "main.lev"              # optional metadata
//     sources = ["main.lev", "util.lev"]
//     assets  = ["views/**", "schema/*.sql", "openapi.json"]   # LA-20, optional
//     version = "0.1.0"                  # optional
//     out     = "hello"                  # optional
//
//     [[dep]]
//     path = "jsonlib"
//     as   = "Json"
//     version = "1.0.0"
//     dev  = false
// -----------------------------------------------------------------------------

// A dependency's `path` is either a local directory or a VCS module path
// (e.g. "github.com/thaden0/json") — distinguished by `kind` (techdesign-
// package-manager.md §4 P2.0-1). `kind` is INFERRED, never parsed from TOML
// (no manifest ceremony added): a `path` that resolves to an existing local
// directory, relative to the manifest's own base dir, is Local; otherwise it
// is Vcs and `version` is required. Inference needs the base dir, which
// parseManifest() does not have (it only sees manifest text), so `kind`
// defaults to Local here and is set for real by resolve.cpp once the base
// dir is known.
enum class DepKind { Local, Vcs };

// A declared dependency. Local-path deps (kind == Local) resolve today via
// LocalProvider (resolve.{hpp,cpp}): `path` names a local directory whose own
// manifest lists the sources that gather into this build. VCS deps (kind ==
// Vcs) are recognized and schema-validated as of P2.0 but not yet resolved —
// fetch/MVS/lockfile land in P2.1 (docs/techdesign-package-manager.md).
struct Dependency {
    std::string path;
    std::string version;
    std::string as_;
    bool dev = false;
    DepKind kind = DepKind::Local;
};

struct ProjectManifest {
    std::string name;
    std::string entry;
    std::vector<std::string> sources;
    // LA-20 §7: declared build-input assets (comptime `import()` targets) —
    // literals or globs, same shape as `sources`, plus `**` recursive
    // matching (resolve.cpp; assets are trees like `views/`, where sources'
    // flat `*.lev` needed only the plain glob). Absent key = empty = no
    // assets (a plan-mode `import()` then fails I03 with the "add it to
    // assets = [...]" hint).
    std::vector<std::string> assets;
    std::vector<Dependency> deps;
    std::string version;
    std::string out;
};

// Parse a manifest's TOML text (already read from disk; `path` is used only
// in error messages). Returns false and sets `err` on any grammar error.
bool parseManifest(const std::string& path, const std::string& text,
                   ProjectManifest& out, std::string& err);

// Serialize `m` back to `path` in the grammar above (P2.1e, techdesign-
// package-manager.md §5.5: `trident add`/`remove`/`update` edit the
// manifest). Rewrites the WHOLE file from the parsed structure — trident's
// hand-rolled writer does not preserve the original file's comments or
// formatting (a known, accepted trade-off of not carrying a real TOML
// editor; matches how `lock.cpp` regenerates `trident.lock` wholesale too).
// `kind` is never written — it is always re-inferred at load time (P2.0).
bool writeManifest(const std::string& path, const ProjectManifest& m, std::string& err);
