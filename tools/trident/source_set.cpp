#include "source_set.hpp"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <glob.h>
#include <set>
#include <sys/stat.h>

namespace {

bool unsafeRelativePath(const std::string& path) {
    if (path.empty() || path.front() == '/') return true;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos
                                                  ? std::string::npos
                                                  : slash - start);
        if (part == "..") return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

bool addRegularFile(const std::string& base, const std::string& path,
                    std::set<std::string>& seen, std::vector<StoreFile>& out,
                    std::string& err) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        err = "declared source '" + path + "' does not exist";
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        err = "declared source '" + path + "' is not a regular file "
              "(directories and symlinks are not package sources)";
        return false;
    }
    char baseReal[PATH_MAX], pathReal[PATH_MAX];
    std::string baseNoSlash = base.size() > 1 && base.back() == '/'
        ? base.substr(0, base.size() - 1) : base;
    if (!::realpath(baseNoSlash.c_str(), baseReal) || !::realpath(path.c_str(), pathReal)) {
        err = "cannot canonicalize declared source '" + path + "'";
        return false;
    }
    std::string canonicalBase = std::string(baseReal) + "/";
    std::string canonicalPath = pathReal;
    if (canonicalPath.compare(0, canonicalBase.size(), canonicalBase) != 0) {
        err = "declared source '" + path + "' escapes the package root through a symlink";
        return false;
    }
    if (path.compare(0, base.size(), base) != 0) {
        err = "declared source '" + path + "' escapes the package root";
        return false;
    }
    std::string rel = path.substr(base.size());
    if (unsafeRelativePath(rel)) {
        err = "declared source '" + rel + "' escapes the package root";
        return false;
    }
    if (seen.insert(rel).second) out.push_back({rel, path});
    return true;
}

}  // namespace

bool collectDeclaredSources(const std::string& baseDir,
                            const std::vector<std::string>& patterns,
                            std::vector<StoreFile>& files, std::string& err) {
    std::string base = baseDir;
    if (base.empty()) base = ".";
    if (base.back() != '/') base += '/';

    files.clear();
    std::set<std::string> seen;
    for (const std::string& rel : patterns) {
        if (unsafeRelativePath(rel)) {
            err = "source path '" + rel + "' must stay inside the package root";
            return false;
        }
        std::string pattern = base + rel;
        if (rel.find_first_of("*?[") == std::string::npos) {
            if (!addRegularFile(base, pattern, seen, files, err)) return false;
            continue;
        }

        glob_t g{};
        int rc = ::glob(pattern.c_str(), GLOB_MARK, nullptr, &g);
        if (rc == GLOB_NOMATCH) {
            ::globfree(&g);
            err = "source pattern '" + rel + "' matched no files";
            return false;
        }
        if (rc != 0) {
            ::globfree(&g);
            err = "failed to expand source pattern '" + rel + "'";
            return false;
        }
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            std::string path = g.gl_pathv[i];
            if (!path.empty() && path.back() == '/') continue;
            if (!addRegularFile(base, path, seen, files, err)) {
                ::globfree(&g);
                return false;
            }
        }
        ::globfree(&g);
    }
    std::sort(files.begin(), files.end(),
              [](const StoreFile& a, const StoreFile& b) { return a.relPath < b.relPath; });
    if (files.empty()) {
        err = "manifest's source set is empty";
        return false;
    }
    return true;
}
