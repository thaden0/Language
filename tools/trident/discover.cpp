#include "discover.hpp"
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool isExecutableFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && ::access(path.c_str(), X_OK) == 0;
}

std::string findOnPath(const std::string& name) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return "";
    std::string paths = pathEnv;
    size_t start = 0;
    while (start <= paths.size()) {
        size_t colon = paths.find(':', start);
        std::string dir = paths.substr(start, colon == std::string::npos
                                                   ? std::string::npos
                                                   : colon - start);
        if (!dir.empty() && isExecutableFile(dir + "/" + name))
            return dir + "/" + name;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

// trident's own directory, via /proc/self/exe (mirrors src/main.cpp's exeDir,
// duplicated rather than shared — the two binaries share only levsyntax and
// the frozen plan text contract, never driver-plumbing code).
std::string exeDir() {
    char exePath[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) return "";
    exePath[len] = '\0';
    std::string dir(exePath);
    size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) return "";
    return dir.substr(0, slash);
}

}  // namespace

std::string discoverManifestPath(const std::string& dir) {
    std::string base = dir.empty() ? "." : dir;
    if (base.back() != '/') base += '/';

    std::string candidate = base + "trident.toml";
    struct stat st;
    if (::stat(candidate.c_str(), &st) == 0) return candidate;
    return "";
}

std::string resolveManifestArg(const std::string& arg) {
    std::string target = arg.empty() ? "." : arg;
    struct stat st;
    if (::stat(target.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return target;
    return discoverManifestPath(target);
}

std::string findLeviathan(const std::string& flagOverride, std::string* tried) {
    auto note = [&](const std::string& s) {
        if (tried) { if (!tried->empty()) *tried += ", "; *tried += s; }
    };
    if (!flagOverride.empty()) {
        note("--leviathan " + flagOverride);
        if (isExecutableFile(flagOverride)) return flagOverride;
    }
    if (const char* env = std::getenv("LEVIATHAN")) {
        note(std::string("$LEVIATHAN=") + env);
        if (isExecutableFile(env)) return env;
    }
    std::string dir = exeDir();
    if (!dir.empty()) {
        std::string sibling = dir + "/leviathan";
        note(sibling);
        if (isExecutableFile(sibling)) return sibling;
    }
    note("PATH");
    std::string onPath = findOnPath("leviathan");
    if (!onPath.empty()) return onPath;
    return "";
}
