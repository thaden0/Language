#include "fetch.hpp"
#include "manifest.hpp"
#include "semver.hpp"
#include "store.hpp"
#include "vcs.hpp"
#include <cstdlib>
#include <fstream>
#include <glob.h>
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

// Mirrors resolve.cpp's expandSources (glob expansion of a manifest's
// `sources` list relative to a base dir). Deliberately duplicated rather
// than shared: that version also does whole-project file dedup, which does
// not apply to expanding one already-fetched module's own source list.
bool expandModuleSources(const std::string& base, const std::vector<std::string>& sources,
                         std::vector<std::string>& out, std::string& err) {
    for (const std::string& rel : sources) {
        std::string pattern = base + rel;
        if (rel.find_first_of("*?[") == std::string::npos) {
            out.push_back(pattern);
            continue;
        }
        glob_t g;
        int rc = glob(pattern.c_str(), GLOB_MARK, nullptr, &g);
        if (rc == GLOB_NOMATCH) {
            globfree(&g);
            err = "source pattern '" + rel + "' matched no files";
            return false;
        }
        if (rc != 0) {
            globfree(&g);
            err = "failed to expand source pattern '" + rel + "'";
            return false;
        }
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            std::string p = g.gl_pathv[i];
            if (!p.empty() && p.back() == '/') continue;   // GLOB_MARK flags dirs
            out.push_back(p);
        }
        globfree(&g);
    }
    return true;
}

std::string relTo(const std::string& base, const std::string& path) {
    if (path.compare(0, base.size(), base) == 0) return path.substr(base.size());
    return path;
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

    std::string base = checkoutDir + "/";
    std::vector<std::string> paths;
    if (!expandModuleSources(base, pm.sources, paths, err)) {
        removeTree(checkoutDir);
        return false;
    }

    std::vector<StoreFile> files;
    files.reserve(paths.size());
    for (const std::string& p : paths) files.push_back({relTo(base, p), p});

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
        if (parseSemVer(t, v, perr) && v.major == mod.major) out.push_back(v);
    }
    return true;
}
