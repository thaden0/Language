#pragma once
#include "manifest.hpp"
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// tools/trident/provider.hpp — the internal ModuleProvider seam (techdesign-
// package-manager.md §3.3). Decouples the MVS resolver (P2.1a) from WHERE a
// module's source comes from, so MVS can be written and unit-tested offline
// against a fake in-memory provider before any git/network code (P2.1c)
// exists, and so the local-path provider (LocalProvider, resolve.hpp) and the
// git provider (GitProvider, P2.1c) are interchangeable. This is an internal
// design seam, not a cross-team contract — but its shape is settled here
// (P2.0) and should change only deliberately, since every later milestone
// codes against it.
// -----------------------------------------------------------------------------

// A module's identity: (VCS path, major). major <= 1 serializes as "path";
// major >= 2 serializes as "path@vN" (techdesign-package-manager.md §0.5,
// §3.5) — two majors are two modules, so json@1.x and json@2.x coexist as
// distinct ModuleIds in one build list.
struct ModuleId {
    std::string path;
    int major = 0;

    bool operator==(const ModuleId& o) const {
        return major == o.major && path == o.path;
    }
    bool operator<(const ModuleId& o) const {
        return path != o.path ? path < o.path : major < o.major;
    }
};

// A semantic version, parsed from a git tag "vMAJOR.MINOR.PATCH". Parsing and
// comparison live in semver.{hpp,cpp} (P2.1a) — this is just the value shape.
struct Version {
    int major = 0, minor = 0, patch = 0;
};

// One declared dependency edge: a requirer needs `mod` at least at `min`.
struct Require {
    ModuleId mod;
    Version min;
    std::string as_;
    bool dev = false;
};

// The MVS resolver's output for one selected module (P2.1a). `requires_`
// avoids the C++20 `requires` keyword (the design's §3.3 prose spells this
// field `requires`; renamed with the trailing underscore already used
// elsewhere in this schema, e.g. Dependency::as_ / Require::as_).
struct BuildListEntry {
    ModuleId mod;
    Version selected;
    std::string contentHash;
    std::string storeDir;
    std::vector<Require> requires_;
};

// The seam. The MVS resolver depends ONLY on this abstract interface, never
// on git/fs directly — this is what makes MVS unit-testable offline against a
// fake in-memory provider, and what keeps the local-path and git paths
// interchangeable.
struct ModuleProvider {
    virtual ~ModuleProvider() = default;

    // Read module@version's OWN manifest (to get its `requires`) without
    // gathering its full source. The git provider (P2.1c) fetches just the
    // tag's trident.toml (or reads it from the proxy).
    virtual bool manifestOf(const ModuleId& mod, const Version& version,
                            ProjectManifest& out, std::string& err) = 0;

    // Materialize module@version's declared sources into the content-
    // addressed store. Returns the store dir and the content hash. Idempotent:
    // a present hash skips the fetch.
    virtual bool materialize(const ModuleId& mod, const Version& version,
                             std::string& storeDir, std::string& contentHash,
                             std::string& err) = 0;

    // Enumerate published versions (tags) — for `update` / latest queries.
    // May be empty for local deps (there is exactly one "version": whatever
    // is on disk).
    virtual bool versions(const ModuleId& mod, std::vector<Version>& out,
                          std::string& err) = 0;
};
