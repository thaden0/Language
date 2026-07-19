#include "proxy.hpp"
#include "endpoint.hpp"
#include "lock.hpp"
#include "manifest.hpp"
#include "process.hpp"
#include "semver.hpp"
#include "store.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool ensureDir(const std::string& path, std::string& err) {
    if (::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
    err = "cannot create temporary proxy directory '" + path + "'";
    return false;
}

bool safeArchivePath(std::string path) {
    if (path == "." || path == "./") return true;
    while (path.compare(0, 2, "./") == 0) path.erase(0, 2);
    if (path.empty() || path.front() == '/') return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos
                                                  ? std::string::npos
                                                  : slash - start);
        if (part == "." || part == ".." || (part.empty() && slash != std::string::npos))
            return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool collectExtractedFiles(const std::string& root, const std::string& dir,
                           std::vector<StoreFile>& out, std::string& err) {
    DIR* handle = ::opendir(dir.c_str());
    if (!handle) { err = "cannot open extracted proxy directory '" + dir + "'"; return false; }
    bool ok = true;
    while (ok) {
        dirent* ent = ::readdir(handle);
        if (!ent) break;
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0) {
            err = "cannot inspect extracted proxy path '" + path + "'";
            ok = false;
        } else if (S_ISDIR(st.st_mode)) {
            ok = collectExtractedFiles(root, path, out, err);
        } else if (S_ISREG(st.st_mode)) {
            out.push_back({path.substr(root.size() + 1), path});
        } else {
            err = "proxy archive contains a symlink or special file at '" +
                  path.substr(root.size() + 1) + "'";
            ok = false;
        }
    }
    ::closedir(handle);
    return ok;
}

void removeTree(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { ::unlink(path.c_str()); return; }
    DIR* d = ::opendir(path.c_str());
    if (d) {
        while (dirent* ent = ::readdir(d)) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            removeTree(path + "/" + name);
        }
        ::closedir(d);
    }
    ::rmdir(path.c_str());
}

std::string versionPath(const ModuleId& mod, const Version& version,
                        const std::string& extension) {
    return proxyModulePrefix(mod) + "/@v/" + formatSemVerTag(version) + extension;
}

bool versionBelongsTo(const ModuleId& mod, const Version& version) {
    return mod.major <= 1 ? version.major <= 1 : version.major == mod.major;
}

}  // namespace

std::string tridentProxyEndpoint() {
    const char* value = std::getenv("TRIDENT_PROXY");
    return value ? std::string(value) : std::string();
}

std::string proxyModulePrefix(const ModuleId& mod) {
    return "modules/" + percentEncodeSegment(serializeModuleId(mod));
}

bool ProxyProvider::manifestOf(const ModuleId& mod, const Version& version,
                               ProjectManifest& out, std::string& err) {
    bool missing = false;
    std::string text;
    std::string path = versionPath(mod, version, ".toml");
    if (!endpointReadText(endpoint_, path, text, missing, err)) {
        if (missing)
            err = "proxy has no manifest for " + mod.path + "@" + formatSemVer(version);
        return false;
    }
    return parseManifest(endpoint_ + "/" + path, text, out, err);
}

bool ProxyProvider::materialize(const ModuleId& mod, const Version& version,
                                std::string& storeDir, std::string& contentHash,
                                std::string& err) {
    std::string tmpl = "/tmp/trident-proxy-XXXXXX";
    std::vector<char> bytes(tmpl.begin(), tmpl.end());
    bytes.push_back('\0');
    if (::mkdtemp(bytes.data()) == nullptr) {
        err = "cannot create a temporary directory for proxy extraction";
        return false;
    }
    std::string temp = bytes.data();
    std::string archive = temp + "/module.tar";
    std::string extracted = temp + "/src";
    bool ok = ensureDir(extracted, err);

    bool missing = false;
    if (ok && !endpointDownload(endpoint_, versionPath(mod, version, ".tar"), archive,
                                missing, err)) {
        if (missing)
            err = "proxy has no source archive for " + mod.path + "@" + formatSemVer(version);
        ok = false;
    }

    if (ok && findExecutable("tar").empty()) {
        err = "cannot find the 'tar' executable on $PATH (required for TRIDENT_PROXY archives)";
        ok = false;
    }
    if (ok) {
        std::string listing;
        int rc = runProcess({"tar", "--list", "--file", archive}, listing, err);
        if (rc != 0) {
            err = "cannot list proxy archive for " + mod.path + "@" + formatSemVer(version);
            ok = false;
        } else {
            std::istringstream lines(listing);
            std::string path;
            while (std::getline(lines, path)) {
                if (!safeArchivePath(path)) {
                    err = "proxy archive contains an unsafe path '" + path + "'";
                    ok = false;
                    break;
                }
            }
        }
    }
    if (ok) {
        std::string output;
        int rc = runProcess({"tar", "--extract", "--file", archive, "--directory", extracted,
                             "--no-same-owner", "--no-same-permissions"}, output, err);
        if (rc != 0) {
            err = "cannot extract proxy archive for " + mod.path + "@" + formatSemVer(version);
            ok = false;
        }
    }

    std::vector<StoreFile> files;
    if (ok && !collectExtractedFiles(extracted, extracted, files, err)) ok = false;
    if (ok && files.empty()) {
        err = "proxy archive for " + mod.path + "@" + formatSemVer(version) + " is empty";
        ok = false;
    }
    if (ok) ok = materializeToStore(files, storeDir, contentHash, err);
    removeTree(temp);
    return ok;
}

bool ProxyProvider::versions(const ModuleId& mod, std::vector<Version>& out,
                             std::string& err) {
    bool missing = false;
    std::string text;
    std::string path = proxyModulePrefix(mod) + "/@v/list";
    if (!endpointReadText(endpoint_, path, text, missing, err)) {
        if (missing) err = "proxy has no version list for '" + serializeModuleId(mod) + "'";
        return false;
    }
    out.clear();
    std::istringstream lines(text);
    std::string tag;
    while (std::getline(lines, tag)) {
        if (!tag.empty() && tag.back() == '\r') tag.pop_back();
        Version version;
        std::string parseErr;
        if (parseSemVer(tag, version, parseErr) && versionBelongsTo(mod, version))
            out.push_back(version);
    }
    std::sort(out.begin(), out.end(),
              [](const Version& a, const Version& b) { return compareSemVer(a, b) < 0; });
    return true;
}
