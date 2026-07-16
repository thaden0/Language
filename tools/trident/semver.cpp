#include "semver.hpp"
#include <cctype>
#include <cstdlib>

namespace {

bool parseUInt(const std::string& s, size_t& i, int& out) {
    size_t start = i;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    if (i == start) return false;
    out = std::atoi(s.substr(start, i - start).c_str());
    return true;
}

}  // namespace

bool parseSemVer(const std::string& text, Version& out, std::string& err) {
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;

    int major = 0, minor = 0, patch = 0;
    if (!parseUInt(text, i, major) || i >= text.size() || text[i] != '.') {
        err = "'" + text + "' is not a valid version (expected MAJOR.MINOR.PATCH, "
             "e.g. \"1.2.0\" or \"v1.2.0\")";
        return false;
    }
    ++i;
    if (!parseUInt(text, i, minor) || i >= text.size() || text[i] != '.') {
        err = "'" + text + "' is not a valid version (expected MAJOR.MINOR.PATCH, "
             "e.g. \"1.2.0\" or \"v1.2.0\")";
        return false;
    }
    ++i;
    if (!parseUInt(text, i, patch) || i != text.size()) {
        err = "'" + text + "' is not a valid version (expected MAJOR.MINOR.PATCH, "
             "e.g. \"1.2.0\" or \"v1.2.0\")";
        return false;
    }

    out = Version{major, minor, patch};
    return true;
}

std::string formatSemVer(const Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
          std::to_string(v.patch);
}

std::string formatSemVerTag(const Version& v) {
    return "v" + formatSemVer(v);
}

int compareSemVer(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}
