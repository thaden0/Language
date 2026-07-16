#include "store.hpp"
#include "hash.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
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

bool materializeToStore(std::vector<StoreFile> files, std::string& storeDir,
                        std::string& contentHash, std::string& err) {
    if (!canonicalContentHash(files, contentHash, err)) return false;

    storeDir = storeRoot() + "/" + contentHash;
    struct stat st;
    if (::stat(storeDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;   // idempotent: already materialized, skip the copy
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
