#include "fetch.hpp"
#include "manifest.hpp"
#include "semver.hpp"
#include "source_set.hpp"
#include "store.hpp"
#include "vcs.hpp"
#include <cstdlib>
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

// `checkoutDir` is our own mkdtemp() output (vcs.cpp) — never user input.
void removeTree(const std::string& dir) {
    std::string cmd = "rm -rf -- '" + dir + "'";
    std::system(cmd.c_str());
}

// Shallow-clone `remote`@`tag` and parse its trident.toml. On success, the
// caller owns removing `checkoutDir`; on failure, this cleans up itself.
bool cloneTagAndReadManifest(const std::string& remote, const std::string& tag,
                             std::string& checkoutDir, ProjectManifest& out,
                             std::string& err) {
    if (!gitCloneTag(remote, tag, "", checkoutDir, err)) return false;

    std::string manifestPath = checkoutDir + "/trident.toml";
    std::string text;
    if (!readWholeFile(manifestPath, text)) {
        err = "fetched tag '" + tag + "' of '" + remote + "' has no trident.toml";
        removeTree(checkoutDir);
        return false;
    }
    if (!parseManifest(manifestPath, text, out, err)) {
        removeTree(checkoutDir);
        return false;
    }
    return true;
}

}  // namespace

std::string remoteUrlFor(const std::string& modulePath) {
    if (modulePath.find("://") != std::string::npos) return modulePath;
    if (!modulePath.empty() && modulePath[0] == '/') return modulePath;
    return "https://" + modulePath;
}

bool GitProvider::manifestOf(const ModuleId& mod, const Version& version, ProjectManifest& out,
                             std::string& err) {
    std::string remote = remoteUrlFor(mod.path);
    std::string tag = formatSemVerTag(version);
    std::string checkoutDir;
    if (!cloneTagAndReadManifest(remote, tag, checkoutDir, out, err)) return false;
    removeTree(checkoutDir);
    return true;
}

bool GitProvider::materialize(const ModuleId& mod, const Version& version,
                              std::string& storeDir, std::string& contentHash,
                              std::string& err) {
    std::string remote = remoteUrlFor(mod.path);
    std::string tag = formatSemVerTag(version);
    std::string checkoutDir;
    ProjectManifest pm;
    if (!cloneTagAndReadManifest(remote, tag, checkoutDir, pm, err)) return false;

    std::vector<StoreFile> files;
    if (!collectDeclaredSources(checkoutDir, pm.sources, files, err)) {
        removeTree(checkoutDir);
        return false;
    }

    bool ok = materializeToStore(files, storeDir, contentHash, err);
    removeTree(checkoutDir);
    return ok;
}

bool GitProvider::versions(const ModuleId& mod, std::vector<Version>& out, std::string& err) {
    std::string remote = remoteUrlFor(mod.path);
    std::vector<std::string> tags;
    if (!gitListTags(remote, tags, err)) return false;

    out.clear();
    for (const std::string& t : tags) {
        Version v;
        std::string perr;
        if (!parseSemVer(t, v, perr)) continue;
        bool sameIdentity = mod.major <= 1 ? v.major <= 1 : v.major == mod.major;
        if (sameIdentity) out.push_back(v);
    }
    return true;
}
