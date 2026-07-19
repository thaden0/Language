#include "store.hpp"
#include "hash.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool readWholeFile(const std::string& path, std::string& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot read '" + path + "'"; return false; }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool ensureDirRec(const std::string& dir) {
    if (dir.empty() || dir == "/") return true;
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && !ensureDirRec(dir.substr(0, slash))) return false;
    return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool writeWholeFile(const std::string& path, const std::string& data, std::string& err) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !ensureDirRec(path.substr(0, slash))) {
        err = "cannot create directory for '" + path + "'";
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) { err = "cannot write '" + path + "'"; return false; }
    out << data;
    return out.good();
}

bool collectStoreFiles(const std::string& root, const std::string& dir,
                       std::vector<StoreFile>& files, std::string& err) {
    DIR* handle = ::opendir(dir.c_str());
    if (!handle) { err = "cannot open store entry '" + dir + "'"; return false; }
    bool ok = true;
    while (ok) {
        dirent* ent = ::readdir(handle);
        if (!ent) break;
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0) {
            err = "cannot inspect store path '" + path + "'";
            ok = false;
        } else if (S_ISDIR(st.st_mode)) {
            ok = collectStoreFiles(root, path, files, err);
        } else if (S_ISREG(st.st_mode)) {
            files.push_back({path.substr(root.size() + 1), path});
        } else {
            err = "store entry contains a symlink or special file at '" +
                  path.substr(root.size() + 1) + "'";
            ok = false;
        }
    }
    ::closedir(handle);
    return ok;
}

}  // namespace

std::string storeRoot() {
    if (const char* home = std::getenv("TRIDENT_HOME")) return std::string(home) + "/store";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.trident/store";
    return "./.trident/store";
}

bool canonicalContentHash(std::vector<StoreFile> files, std::string& hash, std::string& err) {
    std::sort(files.begin(), files.end(),
             [](const StoreFile& a, const StoreFile& b) { return a.relPath < b.relPath; });

    Sha256 h;
    for (const StoreFile& f : files) {
        std::string data;
        if (!readWholeFile(f.absPath, data, err)) return false;
        h.update(f.relPath);
        unsigned char sep = 0x00;
        h.update(&sep, 1);
        h.update(data);
    }
    hash = h.finalHex();
    return true;
}

bool verifyStoreEntry(const std::string& storeDir, const std::string& expectedHash,
                      std::string& err) {
    std::vector<StoreFile> stored;
    if (!collectStoreFiles(storeDir, storeDir, stored, err)) return false;
    std::string actual;
    if (!canonicalContentHash(stored, actual, err)) return false;
    if (actual != expectedHash) {
        err = "content-addressed store entry '" + storeDir +
              "' is corrupt (directory key sha256:" + expectedHash +
              ", actual sha256:" + actual + ")";
        return false;
    }
    return true;
}

bool materializeToStore(std::vector<StoreFile> files, std::string& storeDir,
                        std::string& contentHash, std::string& err) {
    if (!canonicalContentHash(files, contentHash, err)) return false;

    storeDir = storeRoot() + "/" + contentHash;
    struct stat st;
    if (::stat(storeDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        // A cache hit is trusted only after hashing the stored bytes too.
        // Otherwise an injected file would be gathered into the build even
        // though the provider's freshly-computed hash still matched the lock.
        return verifyStoreEntry(storeDir, contentHash, err);
    }

    std::string tmpDir = storeDir + ".tmp." + std::to_string(::getpid());
    for (const StoreFile& f : files) {
        std::string data;
        if (!readWholeFile(f.absPath, data, err)) return false;
        if (!writeWholeFile(tmpDir + "/" + f.relPath, data, err)) return false;
    }
    if (::rename(tmpDir.c_str(), storeDir.c_str()) != 0) {
        err = "cannot finalize store entry '" + storeDir + "'";
        return false;
    }
    return true;
}
