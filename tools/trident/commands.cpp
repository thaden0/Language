#include "commands.hpp"
#include "discover.hpp"
#include "fetch.hpp"
#include "lock.hpp"
#include "manifest.hpp"
#include "resolve.hpp"
#include "semver.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Load + kind-infer a manifest for a CLI command — the same steps
// resolveProject takes before its load-deps recursion, but these commands
// only need the manifest + inferred kinds, not a full resolve -> plan pass.
bool loadManifestForCommand(const std::string& manifestArg, std::string& manifestPath,
                            ProjectManifest& pm, std::string& err) {
    manifestPath = resolveManifestArg(manifestArg);
    if (manifestPath.empty()) {
        err = "no manifest found (looked for trident.toml in '" +
             (manifestArg.empty() ? std::string(".") : manifestArg) + "')";
        return false;
    }
    std::string text;
    if (!readWholeFile(manifestPath, text)) {
        err = "cannot read manifest '" + manifestPath + "'";
        return false;
    }
    if (!parseManifest(manifestPath, text, pm, err)) return false;
    return inferAndValidateDependencyKinds(pm.deps, dirOf(manifestPath), err);
}

// Re-resolve `pm`'s Vcs deps FRESH — never off an existing lock, since every
// caller here (add/remove/update/lock/fetch) is explicitly about
// recomputing it — and rewrite trident.lock. Returns the fresh build list
// for the caller's own reporting (cmdAdd/cmdFetch).
bool regenerateLock(const std::string& manifestPath, const ProjectManifest& pm,
                    std::vector<BuildListEntry>& buildList, std::string& err) {
    VcsResolution vr = resolveVcsDeps(pm, /*includeDevDeps=*/true, /*lockPath=*/"");
    if (!vr.ok) {
        err = vr.err;
        return false;
    }
    buildList = vr.buildList;
    Lockfile lock = lockfileFromBuildList(buildList);
    return writeLockfile(lockfilePathFor(manifestPath), lock, err);
}

// The highest available tag for `path` at identity major `major` (§0.5's
// major<=1 bucket, unless the caller already knows a higher one).
bool highestVersion(const std::string& path, int major, Version& out, std::string& err) {
    GitProvider provider;
    std::vector<Version> versions;
    if (!provider.versions(ModuleId{path, major}, versions, err)) return false;
    if (versions.empty()) {
        err = "no tags found for '" + path + "'";
        return false;
    }
    out = versions[0];
    for (const Version& v : versions)
        if (compareSemVer(v, out) > 0) out = v;
    return true;
}

}  // namespace

int cmdAdd(const std::string& manifestArg, const std::string& depSpec, const std::string& asName,
          bool dev) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::string path = depSpec, version;
    size_t at = depSpec.rfind('@');
    if (at != std::string::npos) {
        path = depSpec.substr(0, at);
        version = depSpec.substr(at + 1);
    }
    if (version.empty()) {
        // Major unknown without a version string — query the major<=1
        // identity bucket (§0.5). A `trident add path@v2` (major, no
        // minor.patch) selector for "latest of major 2" is a follow-on.
        Version best;
        if (!highestVersion(path, 1, best, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        version = formatSemVer(best);
    }

    Dependency* existing = nullptr;
    for (Dependency& d : pm.deps)
        if (d.path == path) { existing = &d; break; }
    if (existing) {
        existing->version = version;
        if (!asName.empty()) existing->as_ = asName;
        existing->dev = dev;
    } else {
        Dependency d;
        d.path = path;
        d.version = version;
        d.as_ = asName;
        d.dev = dev;
        pm.deps.push_back(d);
    }

    if (!writeManifest(manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    // Re-load: the just-added dep's `kind` was never inferred above (`pm`
    // was edited by hand, not by inferAndValidateDependencyKinds).
    ProjectManifest fresh;
    if (!loadManifestForCommand(manifestArg, manifestPath, fresh, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::vector<BuildListEntry> buildList;
    if (!regenerateLock(manifestPath, fresh, buildList, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::printf("added %s@%s", path.c_str(), version.c_str());
    if (!asName.empty()) std::printf(" as %s", asName.c_str());
    std::printf("\n");
    return 0;
}

int cmdRemove(const std::string& manifestArg, const std::string& path) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    auto it = std::find_if(pm.deps.begin(), pm.deps.end(),
                           [&](const Dependency& d) { return d.path == path; });
    if (it == pm.deps.end()) {
        std::fprintf(stderr, "error: no dependency '%s' in the manifest\n", path.c_str());
        return 1;
    }
    pm.deps.erase(it);

    if (!writeManifest(manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::vector<BuildListEntry> buildList;
    if (!regenerateLock(manifestPath, pm, buildList, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("removed %s\n", path.c_str());
    return 0;
}

int cmdUpdate(const std::string& manifestArg, const std::string& path) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    bool any = false;
    for (Dependency& d : pm.deps) {
        if (d.kind != DepKind::Vcs) continue;
        if (!path.empty() && d.path != path) continue;
        any = true;

        Version cur;
        std::string perr;
        int major = parseSemVer(d.version, cur, perr) ? (cur.major <= 1 ? 1 : cur.major) : 1;
        Version best;
        if (!highestVersion(d.path, major, best, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        d.version = formatSemVer(best);
    }
    if (!any) {
        std::fprintf(stderr, "error: %s\n",
                    path.empty() ? "no VCS dependencies to update"
                                : ("no VCS dependency '" + path + "'").c_str());
        return 1;
    }

    if (!writeManifest(manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::vector<BuildListEntry> buildList;
    if (!regenerateLock(manifestPath, pm, buildList, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("updated\n");
    return 0;
}

int cmdLock(const std::string& manifestArg) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::vector<BuildListEntry> buildList;
    if (!regenerateLock(manifestPath, pm, buildList, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", lockfilePathFor(manifestPath).c_str());
    return 0;
}

int cmdFetch(const std::string& manifestArg) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::vector<BuildListEntry> buildList;
    if (!regenerateLock(manifestPath, pm, buildList, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("fetched %zu module(s)\n", buildList.size());
    return 0;
}

int cmdWhy(const std::string& manifestArg, const std::string& path) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    // Reads the CURRENT resolution — the lock if present/consistent (fast,
    // offline), else a fresh MVS run — the same rule build/check use.
    VcsResolution vr = resolveVcsDeps(pm, /*includeDevDeps=*/true, lockfilePathFor(manifestPath));
    if (!vr.ok) {
        std::fprintf(stderr, "error: %s\n", vr.err.c_str());
        return 1;
    }

    const BuildListEntry* target = nullptr;
    for (const BuildListEntry& e : vr.buildList)
        if (e.mod.path == path) { target = &e; break; }
    if (!target) {
        std::fprintf(stderr, "error: '%s' is not a selected dependency\n", path.c_str());
        return 1;
    }

    std::printf("%s@%s\n", target->mod.path.c_str(), formatSemVer(target->selected).c_str());

    bool any = false;
    for (const Dependency& d : pm.deps) {
        if (d.kind == DepKind::Vcs && d.path == path) {
            std::printf("  required by root (>= %s)\n", d.version.c_str());
            any = true;
        }
    }
    for (const BuildListEntry& e : vr.buildList) {
        for (const Require& r : e.requires_) {
            if (r.mod.path == path) {
                std::printf("  required by %s@%s (>= %s)\n", e.mod.path.c_str(),
                           formatSemVer(e.selected).c_str(), formatSemVer(r.min).c_str());
                any = true;
            }
        }
    }
    if (!any)
        std::printf("  (not directly required by name — pulled in transitively; "
                    "see the other selected modules' own requires)\n");
    return 0;
}
