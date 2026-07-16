#pragma once
#include "manifest.hpp"
#include "provider.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// tools/trident/resolve.{hpp,cpp} — dependency resolution (techdesign-toolchain.md
// §3.2: loadDepsRec, dep-dir discovery, `as` aliasing; ported from the
// compiler's former src/Project.cpp at the Track A <-> Track B code cut,
// H-1). Local-path deps (kind == Local) resolve via loadDepsRec below,
// unchanged since P2.0. VCS deps (kind == Vcs) resolve via MVS + the
// GitProvider (resolveVcsDeps, P2.1e, techdesign-package-manager.md §5.5).
// -----------------------------------------------------------------------------

// The reference ModuleProvider (provider.hpp, §3.3) for local-path deps —
// wraps the same manifest-discovery/parse helpers loadDepsRec uses below.
// Local deps carry no VCS version semantics, so ModuleId.path is the dep's
// canonicalized absolute directory and major is always 0; `versions()` is
// empty (there is exactly one "version": whatever is on disk). Not yet
// wired into resolveProject's live path (P2.1e does that, dispatching per
// dep kind) — this class exists to prove the ModuleProvider seam is real
// before the git provider (P2.1c) is written against it.
class LocalProvider : public ModuleProvider {
public:
    bool manifestOf(const ModuleId& mod, const Version& version,
                    ProjectManifest& out, std::string& err) override;
    bool materialize(const ModuleId& mod, const Version& version,
                     std::string& storeDir, std::string& contentHash,
                     std::string& err) override;
    bool versions(const ModuleId& mod, std::vector<Version>& out,
                 std::string& err) override;
};

// One resolved source: its absolute path, its canonical moduleId ("" = root
// project), and its origin (the dependency's declared path, "" for the root).
struct ResolvedSource {
    std::string path;
    std::string moduleId;
    std::string origin;
};

// LA-20 §7: one resolved, hashed build-input asset (a comptime `import()`
// target). `rel` uses '/' separators, relative to the owning module's own
// manifest directory — the exact string a plan-mode `import()` matches
// against. `hash` is `sha256Hex` of the file's content, computed once here
// so leviathan (contract rule 1) never hashes anything itself.
struct ResolvedAsset {
    std::string rel;
    std::string path;
    std::string moduleId;
    std::string hash;
};

struct ResolvedProject {
    bool ok = false;
    ProjectManifest manifest;

    // Aliases first (materialized as real files under the plan's alias dir —
    // the plan format only carries on-disk paths, §3.3 rule 1), then every
    // real gathered file (root sources, then each dep's, recursively).
    std::vector<ResolvedSource> sources;

    // The dependency graph, by canonical module dir ("" = root): each module
    // maps to the set of modules it directly declared as deps. Becomes the
    // plan's `edge` rows (§3.3) — drives leviathan's phantom-dep enforcement.
    std::map<std::string, std::set<std::string>> moduleDeps;

    // LA-20 §7: declared build-input assets, gathered per module exactly like
    // `sources` (the root's own `assets =`, then each LOCAL dep's own,
    // recursively — VCS-dep assets are an open call, §15/§16 of the design;
    // not resolved here yet). Becomes the plan's `asset` rows.
    std::vector<ResolvedAsset> assets;

    // The entry, classified explicitly (no sniffing on leviathan's side
    // anymore, §3.3 rule 2): "script" | "file" | "function". For "file", the
    // target is the exact absolute path also present in `sources` above (so
    // leviathan matches it by plain string equality, no path resolution).
    std::string entryKind = "script";
    std::string entryTarget;
};

// Resolve a project rooted at `manifestPath`: parse the manifest, recursively
// load local-path deps, resolve VCS deps (MVS + fetch, below), expand source
// globs, classify the entry, and synthesize `as` aliases as materialized
// source files written under `aliasDir`. `includeDevDeps` controls whether
// dev-flagged VCS deps are resolved (production builds pass false; `check`
// and `plan` pass true, §5.5 — local-kind dev deps are not filtered, matching
// loadDepsRec's pre-P2 behavior; no fixture needs that yet). A non-empty
// `vendorDir` switches every VCS dep to the hermetic, network-free path
// (P2.2 GT4, `trident vendor`/`--vendor`, below) — see resolveVcsDeps for
// what that requires. Prints diagnostics to stderr. Returns a ResolvedProject
// with ok == false on any error.
ResolvedProject resolveProject(const std::string& manifestPath, const std::string& aliasDir,
                               bool includeDevDeps = true, const std::string& vendorDir = "");

// The result of resolving just the root manifest's Vcs-kind deps: MVS
// selection + materialization (contentHash/storeDir filled in on every
// entry — unlike mvs.cpp's own selectVersions(), which leaves them empty).
// Exposed so the CLI commands (add/lock/why, commands.cpp) can run this
// without going through the whole resolveProject → plan pipeline.
struct VcsResolution {
    bool ok = false;
    std::vector<BuildListEntry> buildList;
    std::string err;
};

// Resolve `manifest`'s own Vcs-kind deps (Local-kind deps never reach this —
// they resolve entirely via loadDepsRec/LocalProvider). A manifest with no
// Vcs-kind deps trivially succeeds with an empty build list — no GitProvider
// call, no git prerequisite. `includeDevDeps` excludes dev-flagged deps when
// false (production builds, §5.5). A non-empty `lockPath` opts into §3.4's
// "use the lock verbatim when present and consistent" — no MVS graph walk,
// no manifestOf() network calls, only an idempotent (often no-op)
// materialize() per selected entry. Pass "" to force a fresh MVS run
// regardless of any existing lock (`add`/`lock`/`update`/`fetch`,
// commands.cpp — recomputing the lock is their whole point).
//
// Every materialized entry is verified (P2.2 GT4): its freshly computed
// content hash is checked against the checksum DB (recording it there if
// this is the first time this module@version has ever been fetched) and,
// when the lock was used verbatim, against the lock's own pinned hash too —
// either mismatch is a loud `vr.err` (tag moved / content swapped), never a
// silent acceptance.
//
// A non-empty `vendorDir` switches to the hermetic path (`trident vendor`/
// `--vendor`, P2.2 GT4): every module is read straight from
// `vendorDir/<serializeModuleId>/` (no git, no network, no $TRIDENT_HOME
// store) instead of fetched. This REQUIRES a present, consistent `lockPath`
// (a vendored build has nothing else to pin versions to — a stale/missing
// lock is a loud error naming `trident lock` then `trident vendor`) and only
// checks the vendored content against the lock's hash — the checksum DB is
// deliberately not consulted, so a vendor directory copied to a machine with
// no `$TRIDENT_HOME` history still builds.
VcsResolution resolveVcsDeps(const ProjectManifest& manifest, bool includeDevDeps,
                             const std::string& lockPath, const std::string& vendorDir = "");

// Infer each dep's `kind` (P2.0 §4: a `path` resolving to an existing
// directory relative to `base` is Local, otherwise Vcs, which requires
// `version`). `resolveProject` calls this internally; it is exposed for
// callers (commands.cpp: add/update/lock/fetch/why, P2.1e) that parse a
// manifest directly without running the whole resolveProject pipeline.
bool inferAndValidateDependencyKinds(std::vector<Dependency>& deps, const std::string& base,
                                     std::string& err);
