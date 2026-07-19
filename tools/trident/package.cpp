#include "package.hpp"
#include "discover.hpp"
#include "semver.hpp"
#include "source_set.hpp"
#include "store.hpp"
#include "vcs.hpp"
#include <climits>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace {

bool readFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

std::string canonical(const std::string& path) {
    char buffer[PATH_MAX];
    return ::realpath(path.c_str(), buffer) ? std::string(buffer) : path;
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool inspectPackage(const std::string& manifestArg, const std::string& tagOverride,
                    const std::string& moduleOverride, bool requireClean,
                    PackageSnapshot& out, std::string& err) {
    out = PackageSnapshot{};
    out.manifestPath = resolveManifestArg(manifestArg);
    if (out.manifestPath.empty()) {
        err = "no manifest found (looked for trident.toml in '" +
              (manifestArg.empty() ? std::string(".") : manifestArg) + "')";
        return false;
    }
    out.manifestPath = canonical(out.manifestPath);
    if (baseName(out.manifestPath) != "trident.toml") {
        err = "a published VCS package must use trident.toml at its repository root "
              "(GitProvider discovers that filename at a tag)";
        return false;
    }

    std::string text;
    if (!readFile(out.manifestPath, text)) {
        err = "cannot read manifest '" + out.manifestPath + "'";
        return false;
    }
    if (!parseManifest(out.manifestPath, text, out.manifest, err)) return false;
    if (out.manifest.name.empty()) {
        err = "a published package manifest requires a non-empty 'name'";
        return false;
    }
    if (out.manifest.version.empty()) {
        err = "a published package manifest requires 'version = \"MAJOR.MINOR.PATCH\"'";
        return false;
    }
    if (!parseSemVer(out.manifest.version, out.version, err)) {
        err = "manifest version: " + err;
        return false;
    }
    out.tag = tagOverride.empty() ? formatSemVerTag(out.version) : tagOverride;
    Version tagVersion;
    std::string tagErr;
    if (!parseSemVer(out.tag, tagVersion, tagErr)) {
        err = "publish tag: " + tagErr;
        return false;
    }
    if (compareSemVer(tagVersion, out.version) != 0) {
        err = "publish tag '" + out.tag + "' does not match manifest version '" +
              formatSemVer(out.version) + "'";
        return false;
    }
    out.tag = formatSemVerTag(tagVersion);  // canonical lowercase-v spelling

    std::string manifestDir = canonical(dirOf(out.manifestPath));
    if (!gitRepoRoot(manifestDir, out.repoRoot, err)) return false;
    out.repoRoot = canonical(out.repoRoot);
    if (manifestDir != out.repoRoot) {
        err = "published trident.toml must sit at the git repository root ('" +
              out.repoRoot + "')";
        return false;
    }
    if (requireClean) {
        bool clean = false;
        std::string details;
        if (!gitWorkingTreeClean(out.repoRoot, clean, details, err)) return false;
        if (!clean) {
            err = "git working tree is not clean; commit or remove these changes before "
                  "publishing:\n" + details;
            return false;
        }
    }
    if (!gitHeadCommit(out.repoRoot, out.commit, err)) return false;

    if (!moduleOverride.empty()) out.modulePath = moduleOverride;
    else {
        std::string remote;
        if (!gitOriginUrl(out.repoRoot, remote, err)) {
            err += " (or pass --path <vcs-path>)";
            return false;
        }
        out.modulePath = modulePathFromRemote(remote);
    }
    if (out.modulePath.empty()) {
        err = "cannot derive a module VCS path (pass --path <vcs-path>)";
        return false;
    }

    if (!collectDeclaredSources(out.repoRoot, out.manifest.sources, out.sources, err))
        return false;
    for (const StoreFile& source : out.sources) {
        bool tracked = false;
        if (!gitPathTracked(out.repoRoot, source.relPath, tracked, err)) return false;
        if (!tracked) {
            err = "declared package source '" + source.relPath +
                  "' is not tracked by git and would be absent from the published tag";
            return false;
        }
    }
    return canonicalContentHash(out.sources, out.contentHash, err);
}
