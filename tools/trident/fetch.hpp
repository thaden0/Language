#pragma once
#include "provider.hpp"
#include <string>

// -----------------------------------------------------------------------------
// tools/trident/fetch.{hpp,cpp} — the GitProvider (techdesign-package-
// manager.md §3.6/§5.3 P2.1c): the ModuleProvider (provider.hpp, §3.3)
// backed by the system `git` (vcs.{hpp,cpp}). `ModuleId.path` (a VCS path
// like "github.com/thaden0/json", or a local filesystem path for the
// offline test fixture, H-7) resolves to a clonable remote via
// `remoteUrlFor()`.
// -----------------------------------------------------------------------------

// Resolve a `ModuleId::path` to a clonable git remote: already a URL
// (contains "://") or an absolute local path (starts with "/") is used
// as-is (the offline bare-repo test fixture is a local path); otherwise it
// is a Go-modules-style host/path and gets "https://" prepended.
std::string remoteUrlFor(const std::string& modulePath);

class GitProvider : public ModuleProvider {
public:
    bool manifestOf(const ModuleId& mod, const Version& version, ProjectManifest& out,
                    std::string& err) override;
    bool materialize(const ModuleId& mod, const Version& version, std::string& storeDir,
                     std::string& contentHash, std::string& err) override;
    bool versions(const ModuleId& mod, std::vector<Version>& out, std::string& err) override;
};
