#pragma once
#include "provider.hpp"
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// tools/trident/lock.{hpp,cpp} — the lockfile (techdesign-package-manager.md
// §3.4/§5.4 P2.1d): `trident.lock` (or `<manifest-basename>.lock` beside a
// differently-named manifest, §0.5). A HASH MANIFEST: because MVS makes
// *selection* deterministic from the manifests, the lock's job is integrity
// pinning + a cached copy of the resolved graph, not recording a solver's
// choice. Reuses `manifest.cpp`'s hand-rolled TOML-reader cursor style — no
// TOML library added.
// -----------------------------------------------------------------------------

// One locked module entry (§3.4's `[[module]]` table). `mod` is the module's
// identity (path + major, provider.hpp); `selected`/`requires_` carry full
// versions as bare "MAJOR.MINOR.PATCH" text (`formatSemVer`) — a version
// fully determines its own major, so only `mod`'s own serialized identity
// (below) needs the §0.5 "@vN" disambiguator.
struct LockedModule {
    ModuleId mod;
    std::string selected;                 // formatSemVer(selected version)
    std::string hash;                     // "sha256:<hex>"
    std::vector<std::string> requires_;   // "path@MAJOR.MINOR.PATCH" edges
};

struct Lockfile {
    int version = 1;
    std::vector<LockedModule> modules;
};

// §0.5's module-identity text: bare `path` for major <= 1, `path@vN` for
// major >= 2 (two majors of one VCS path are two coexisting modules that
// must serialize distinctly).
std::string serializeModuleId(const ModuleId& mod);
bool parseModuleId(const std::string& text, ModuleId& out);

// Build a Lockfile value from a resolved build list (already in ModuleId
// order, mvs.cpp) — the pure "what would this lock contain" step, usable
// for the consistency check without touching disk.
Lockfile lockfileFromBuildList(const std::vector<BuildListEntry>& buildList);

// Serialize `lock` to `path` in the §3.4 TOML grammar. Deterministic: does
// NOT re-sort — callers pass an already-ordered Lockfile
// (`lockfileFromBuildList`'s is ModuleId-ordered, matching the build list).
bool writeLockfile(const std::string& path, const Lockfile& lock, std::string& err);

// Parse a trident.lock file's TEXT (already read from disk; `path` is used
// only in error messages).
bool parseLockfile(const std::string& path, const std::string& text, Lockfile& out,
                   std::string& err);

// The manifest-basename-derived lockfile path beside `manifestPath` (§0.5):
// "trident.toml" -> "trident.lock", "foo.toml" -> "foo.lock".
std::string lockfilePathFor(const std::string& manifestPath);

// Consistency check (§3.4): is `lock` exactly what `buildList` (the CURRENT
// resolution) would produce? Compares the module set, each one's selected
// version, and its requires edges (not the hash — verifying fetched content
// against a recorded hash is the checksum DB's job, P2.2, not this
// structural check). A mismatch means the manifest was edited without
// re-running `trident lock` — the caller turns that into a loud error
// naming the fix, never a silent re-resolve (§3.4).
bool lockfileMatchesBuildList(const Lockfile& lock, const std::vector<BuildListEntry>& buildList,
                              std::string& err);
