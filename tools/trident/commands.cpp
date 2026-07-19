#include "commands.hpp"
#include "checksum.hpp"
#include "discover.hpp"
#include "fetch.hpp"
#include "index.hpp"
#include "lock.hpp"
#include "manifest.hpp"
#include "package.hpp"
#include "proxy.hpp"
#include "provenance.hpp"
#include "resolve.hpp"
#include "semver.hpp"
#include "vcs.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

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
    std::string proxyEndpoint = tridentProxyEndpoint();
    ProxyProvider proxy(proxyEndpoint);
    ModuleProvider& selectedProvider = proxyEndpoint.empty()
        ? static_cast<ModuleProvider&>(provider)
        : static_cast<ModuleProvider&>(proxy);
    std::vector<Version> versions;
    ModuleId mod{path, major};
    if (!selectedProvider.versions(mod, versions, err)) return false;
    if (versions.empty()) {
        err = "no tags found for '" + path + "'";
        return false;
    }
    bool found = false;
    for (const Version& v : versions) {
        bool yanked = false;
        if (!checksumDbIsYanked(checksumDbPath(), mod, v, yanked, err)) return false;
        if (yanked) continue;
        if (!found || compareSemVer(v, out) > 0) out = v;
        found = true;
    }
    if (!found) {
        err = "every published version of '" + path + "' in this major is yanked";
        return false;
    }
    return true;
}

bool ensureDir(const std::string& dir) {
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && !ensureDir(dir.substr(0, slash))) return false;
    return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool copyFile(const std::string& src, const std::string& dst, std::string& err) {
    std::ifstream in(src, std::ios::binary);
    if (!in) { err = "cannot read '" + src + "'"; return false; }
    size_t slash = dst.find_last_of('/');
    if (slash != std::string::npos && !ensureDir(dst.substr(0, slash))) {
        err = "cannot create directory for '" + dst + "'";
        return false;
    }
    std::ofstream out(dst, std::ios::binary);
    if (!out) { err = "cannot write '" + dst + "'"; return false; }
    out << in.rdbuf();
    return out.good();
}

// Recursive directory copy for `trident vendor` — the store dir it reads
// from (store.cpp's `materializeToStore`) holds exactly a module's declared
// sources, so this needs no filtering.
bool copyTree(const std::string& srcDir, const std::string& dstDir, std::string& err) {
    DIR* d = ::opendir(srcDir.c_str());
    if (!d) { err = "cannot open '" + srcDir + "'"; return false; }
    dirent* ent;
    bool ok = true;
    while (ok && (ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string sp = srcDir + "/" + name, dp = dstDir + "/" + name;
        struct stat st;
        if (::stat(sp.c_str(), &st) != 0) { err = "cannot stat '" + sp + "'"; ok = false; break; }
        ok = S_ISDIR(st.st_mode) ? copyTree(sp, dp, err) : copyFile(sp, dp, err);
    }
    ::closedir(d);
    return ok;
}

bool parseVersionedSpec(const std::string& spec, std::string& path, Version& version,
                        std::string& err) {
    size_t at = spec.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 == spec.size()) {
        err = "expected <path>@<MAJOR.MINOR.PATCH>, got '" + spec + "'";
        return false;
    }
    path = spec.substr(0, at);
    if (!parseSemVer(spec.substr(at + 1), version, err)) {
        err = "invalid version in '" + spec + "': " + err;
        return false;
    }
    return true;
}

bool readLockForCommand(const std::string& manifestPath, Lockfile& lock, std::string& err) {
    std::string path = lockfilePathFor(manifestPath);
    std::string text;
    if (!readWholeFile(path, text)) {
        err = "no lock found at '" + path + "' — run `trident lock` first";
        return false;
    }
    return parseLockfile(path, text, lock, err);
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
    // P2.3's optional thin index resolves only friendly names. Any explicit
    // path bypasses it, preventing dependency-confusion shadowing.
    std::string index = tridentIndexEndpoint();
    if (!index.empty() && isFriendlyPackageName(path)) {
        std::string resolved;
        bool found = false;
        if (!indexResolveName(index, path, resolved, found, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (!found) {
            std::fprintf(stderr, "error: package name '%s' is not registered in TRIDENT_INDEX\n",
                         path.c_str());
            return 1;
        }
        path = resolved;
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

int cmdAudit(const std::string& manifestArg, const std::string& policyPath) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::string lockPath = lockfilePathFor(manifestPath);
    std::string lockText;
    if (!readWholeFile(lockPath, lockText)) {
        std::fprintf(stderr, "error: no lock found at '%s' — run `trident lock` first\n",
                    lockPath.c_str());
        return 1;
    }

    // Re-resolving with the lock path re-verifies every module's freshly
    // fetched content against BOTH the checksum DB and the lock's own
    // pinned hash (resolve.cpp's resolveVcsDeps, P2.2 GT4) — a mismatch
    // comes back as a loud vr.err naming the module and both hashes.
    VcsResolution vr = resolveVcsDeps(pm, /*includeDevDeps=*/true, lockPath);
    if (!vr.ok) {
        std::fprintf(stderr, "audit FAILED: %s\n", vr.err.c_str());
        return 1;
    }

    for (const BuildListEntry& e : vr.buildList) {
        std::printf("OK   %s@%s  sha256:%s\n", e.mod.path.c_str(),
                    formatSemVer(e.selected).c_str(), e.contentHash.c_str());
    }

    std::string effectivePolicy = policyPath;
    if (effectivePolicy.empty()) {
        std::string companion = dirOf(manifestPath) + "trident.audit.toml";
        struct stat st;
        if (::stat(companion.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            effectivePolicy = companion;
    }
    if (!effectivePolicy.empty()) {
        std::vector<std::string> policyReport;
        if (!enforceAuditPolicy(effectivePolicy, vr.buildList, policyReport, err)) {
            std::fprintf(stderr, "audit FAILED: %s\n", err.c_str());
            return 1;
        }
        for (const std::string& line : policyReport) std::printf("%s\n", line.c_str());
        std::printf("policy passed: %s\n", effectivePolicy.c_str());
    }
    std::printf("audit passed: %zu module(s) verified\n", vr.buildList.size());
    return 0;
}

int cmdVendor(const std::string& manifestArg) {
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    std::string lockPath = lockfilePathFor(manifestPath);
    std::string lockText;
    if (!readWholeFile(lockPath, lockText)) {
        std::fprintf(stderr, "error: no lock found at '%s' — run `trident lock` first\n",
                    lockPath.c_str());
        return 1;
    }

    // A normal (possibly-networked) resolution, verified exactly like any
    // other (checksum DB + lock-hash cross-check) — vendoring never trusts
    // content it hasn't already verified once.
    VcsResolution vr = resolveVcsDeps(pm, /*includeDevDeps=*/true, lockPath);
    if (!vr.ok) {
        std::fprintf(stderr, "error: %s\n", vr.err.c_str());
        return 1;
    }

    std::string vendorDir = dirOf(manifestPath) + "vendor";
    for (const BuildListEntry& e : vr.buildList) {
        std::string dest = vendorDir + "/" + serializeModuleId(e.mod);
        if (!copyTree(e.storeDir, dest, err)) {
            std::fprintf(stderr, "error: vendoring %s: %s\n", e.mod.path.c_str(), err.c_str());
            return 1;
        }
    }
    std::printf("vendored %zu module(s) into %s\n", vr.buildList.size(), vendorDir.c_str());
    return 0;
}

int cmdPublish(const std::string& manifestArg, const std::string& tag,
               const std::string& modulePath, const std::string& signKey,
               const std::string& identity, const std::string& artifact,
               const std::string& attestationOut) {
    if (!signKey.empty() && identity.empty()) {
        std::fprintf(stderr, "error: --sign-key requires --identity <publisher>\n");
        return 2;
    }
    if (signKey.empty() &&
        (!identity.empty() || !artifact.empty() || !attestationOut.empty())) {
        std::fprintf(stderr,
            "error: --identity/--artifact/--attestation-out require --sign-key <private.pem>\n");
        return 2;
    }
    PackageSnapshot package;
    std::string err;
    if (!inspectPackage(manifestArg, tag, modulePath, /*requireClean=*/true, package, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    bool tagExists = false;
    std::string taggedCommit;
    if (!gitTagCommit(package.repoRoot, package.tag, tagExists, taggedCommit, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    bool createdTag = false;
    if (tagExists) {
        if (taggedCommit != package.commit) {
            std::fprintf(stderr,
                "error: immutable tag '%s' already points at %s, not current commit %s\n",
                package.tag.c_str(), taggedCommit.c_str(), package.commit.c_str());
            return 1;
        }
    } else {
        if (!gitCreateTag(package.repoRoot, package.tag, package.commit, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        createdTag = true;
    }

    int identityMajor = package.version.major <= 1 ? 1 : package.version.major;
    ModuleId mod{package.modulePath, identityMajor};
    if (!checksumDbVerifyOrRecord(checksumDbPath(), mod, package.version,
                                  package.contentHash, err)) {
        if (createdTag) {
            std::string rollbackErr;
            gitDeleteTag(package.repoRoot, package.tag, rollbackErr);
        }
        std::fprintf(stderr, "error: publish integrity check failed: %s\n", err.c_str());
        return 1;
    }

    std::string index = tridentIndexEndpoint();
    if (!index.empty() &&
        !indexRegisterName(index, package.manifest.name, package.modulePath, err)) {
        std::fprintf(stderr,
            "error: package tag and checksum were published, but optional index registration "
            "failed: %s\n", err.c_str());
        return 1;
    }

    std::string writtenAttestation;
    if (!signKey.empty()) {
        writtenAttestation = attestationOut.empty()
            ? defaultAttestationPath(package, identity) : attestationOut;
        if (!writeSignedAttestation(package, identity, signKey, artifact,
                                    writtenAttestation, err)) {
            std::fprintf(stderr,
                "error: package tag and checksum were published, but attestation signing "
                "failed: %s\n", err.c_str());
            return 1;
        }
    }

    std::printf("published %s@%s\n", package.modulePath.c_str(),
                formatSemVer(package.version).c_str());
    std::printf("  tag: %s (%s)\n", package.tag.c_str(), package.commit.c_str());
    std::printf("  hash: sha256:%s\n", package.contentHash.c_str());
    if (!index.empty())
        std::printf("  index: %s -> %s\n", package.manifest.name.c_str(),
                    package.modulePath.c_str());
    if (!writtenAttestation.empty())
        std::printf("  attestation: %s\n", writtenAttestation.c_str());
    return 0;
}

int cmdYank(const std::string& moduleSpec) {
    std::string path, err;
    Version version;
    if (!parseVersionedSpec(moduleSpec, path, version, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    std::string index = tridentIndexEndpoint();
    if (!index.empty() && isFriendlyPackageName(path)) {
        std::string resolved;
        bool found = false;
        if (!indexResolveName(index, path, resolved, found, err) || !found) {
            if (err.empty()) err = "package name '" + path + "' is not registered";
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        path = resolved;
    }
    ModuleId mod{path, version.major <= 1 ? 1 : version.major};
    if (!checksumDbYank(checksumDbPath(), mod, version, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("yanked %s@%s (existing locks remain valid)\n", path.c_str(),
                formatSemVer(version).c_str());
    return 0;
}

int cmdAttest(const std::string& manifestArg, const std::string& modulePath,
              const std::string& privateKey, const std::string& identity,
              const std::string& artifact, const std::string& outputPath) {
    if (privateKey.empty() || identity.empty()) {
        std::fprintf(stderr, "error: attest requires --key <private.pem> and --identity <name>\n");
        return 2;
    }
    PackageSnapshot package;
    std::string err;
    if (!inspectPackage(manifestArg, "", modulePath, /*requireClean=*/true, package, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::string destination = outputPath.empty()
        ? defaultAttestationPath(package, identity) : outputPath;
    if (!writeSignedAttestation(package, identity, privateKey, artifact, destination, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("%s\n", destination.c_str());
    return 0;
}

int cmdAuditRecord(const std::string& manifestArg, const std::string& moduleSpec,
                   const std::string& auditor, const std::string& outputPath) {
    if (auditor.empty()) {
        std::fprintf(stderr, "error: audit-record requires --auditor <name>\n");
        return 2;
    }
    std::string manifestPath, err;
    ProjectManifest pm;
    if (!loadManifestForCommand(manifestArg, manifestPath, pm, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::string path;
    Version requested;
    if (!parseVersionedSpec(moduleSpec, path, requested, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }
    Lockfile lock;
    if (!readLockForCommand(manifestPath, lock, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    const LockedModule* selected = nullptr;
    for (const LockedModule& module : lock.modules) {
        Version version;
        std::string parseErr;
        if (module.mod.path == path && parseSemVer(module.selected, version, parseErr) &&
            compareSemVer(version, requested) == 0) {
            selected = &module;
            break;
        }
    }
    if (!selected) {
        std::fprintf(stderr, "error: %s@%s is not present in %s\n", path.c_str(),
                     formatSemVer(requested).c_str(), lockfilePathFor(manifestPath).c_str());
        return 1;
    }
    AuditRecord record{path, formatSemVer(requested), selected->hash, auditor};
    std::string destination = outputPath.empty()
        ? dirOf(manifestPath) + "trident.audits.toml" : outputPath;
    if (!appendAuditRecord(destination, record, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("recorded audit by %s for %s@%s in %s\n", auditor.c_str(), path.c_str(),
                formatSemVer(requested).c_str(), destination.c_str());
    return 0;
}
