#pragma once
#include "manifest.hpp"
#include "provider.hpp"
#include "store.hpp"
#include <string>
#include <vector>

// A validated snapshot of the package in the current git worktree. P2.3
// publish and P2.4 attest both use this exact inspection path so tag,
// checksum, and signature can never describe different source sets.
struct PackageSnapshot {
    std::string manifestPath;
    std::string repoRoot;
    std::string modulePath;
    std::string tag;
    std::string commit;
    ProjectManifest manifest;
    Version version;
    std::vector<StoreFile> sources;
    std::string contentHash;  // bare lowercase SHA-256
};

// Resolve/parse the manifest, require it at a git repository root, validate
// name/version/source files, require a clean tree when `requireClean`, derive
// module identity from origin (unless `moduleOverride` is supplied), and
// compute the canonical source hash. `tagOverride` must name the same SemVer
// as manifest.version.
bool inspectPackage(const std::string& manifestArg, const std::string& tagOverride,
                    const std::string& moduleOverride, bool requireClean,
                    PackageSnapshot& out, std::string& err);
