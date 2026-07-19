#include "checksum.hpp"
#include "hash.hpp"
#include "semver.hpp"
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace {

// 64 lowercase hex zeros — the chain's genesis "previous hash".
const char* kGenesisHash = "0000000000000000000000000000000000000000000000000000000000000";

struct ChecksumEntry {
    enum class Kind { Content, Yank } kind = Kind::Content;
    std::string path;
    int major = 0;
    std::string version;       // formatSemVer text (MAJOR.MINOR.PATCH)
    std::string contentHash;   // bare lowercase hex
    std::string chainHash;     // bare lowercase hex
};

// One line: "<chainHash> <prevChainHash> <path> <major> <version> <contentHash>".
// `path` (a VCS path like "github.com/x/json") never contains whitespace —
// same assumption manifest.cpp/lock.cpp already make about dependency paths.
std::string chainHashOfPayload(const std::string& prevChainHash,
                               const std::string& payload) {
    Sha256 h;
    h.update(prevChainHash);
    h.update(" ", 1);
    h.update(payload);
    return h.finalHex();
}

std::string contentPayload(const std::string& path, int major,
                           const std::string& version,
                           const std::string& contentHash) {
    return path + " " + std::to_string(major) + " " + version + " " + contentHash;
}

std::string yankPayload(const std::string& path, int major,
                        const std::string& version) {
    return "yank " + path + " " + std::to_string(major) + " " + version;
}

bool ensureDirRec(const std::string& dir) {
    if (dir.empty() || dir == "/") return true;
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && !ensureDirRec(dir.substr(0, slash))) return false;
    return ::mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST;
}

// Read every record, verifying the hash chain as it goes. `lastChainHash` is
// the genesis hash if the file is empty/absent (a missing checksum DB is not
// an error — the very first fetch anywhere creates it). A broken chain link
// (any entry whose recomputed chainHash doesn't match what's on disk) means
// the log itself was edited/truncated/reordered out from under trident.
bool loadChecksumDb(const std::string& dbPath, std::vector<ChecksumEntry>& entries,
                    std::string& lastChainHash, std::string& err) {
    lastChainHash = kGenesisHash;
    std::ifstream in(dbPath, std::ios::binary);
    if (!in) return true;   // no DB yet — empty, not an error

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.empty()) continue;
        std::istringstream ss(line);
        ChecksumEntry e;
        std::string prevChainHash, first, majorText, extra;
        if (!(ss >> e.chainHash >> prevChainHash >> first)) {
            err = dbPath + ":" + std::to_string(lineNo) + ": malformed checksum DB record";
            return false;
        }
        std::string payload;
        if (first == "yank") {
            e.kind = ChecksumEntry::Kind::Yank;
            if (!(ss >> e.path >> majorText >> e.version) || (ss >> extra)) {
                err = dbPath + ":" + std::to_string(lineNo) +
                      ": malformed checksum DB yank record";
                return false;
            }
            payload = yankPayload(e.path, std::atoi(majorText.c_str()), e.version);
        } else {
            e.path = first;
            if (!(ss >> majorText >> e.version >> e.contentHash) || (ss >> extra)) {
                err = dbPath + ":" + std::to_string(lineNo) +
                      ": malformed checksum DB content record";
                return false;
            }
            payload = contentPayload(e.path, std::atoi(majorText.c_str()), e.version,
                                     e.contentHash);
        }
        if (prevChainHash != lastChainHash) {
            err = dbPath + ":" + std::to_string(lineNo) +
                 ": checksum DB chain broken (this record's previous-hash link does not "
                 "match the prior record) — the log may have been edited, reordered, or "
                 "truncated";
            return false;
        }
        e.major = std::atoi(majorText.c_str());
        std::string recomputed = chainHashOfPayload(prevChainHash, payload);
        if (recomputed != e.chainHash) {
            err = dbPath + ":" + std::to_string(lineNo) +
                 ": checksum DB record hash does not match its own content — the log has "
                 "been tampered with";
            return false;
        }
        entries.push_back(e);
        lastChainHash = e.chainHash;
    }
    return true;
}

bool appendChecksumEntry(const std::string& dbPath, const std::string& prevChainHash,
                         const std::string& path, int major, const std::string& version,
                         const std::string& contentHash, std::string& err) {
    size_t slash = dbPath.find_last_of('/');
    if (slash != std::string::npos && !ensureDirRec(dbPath.substr(0, slash))) {
        err = "cannot create directory for '" + dbPath + "'";
        return false;
    }
    std::string payload = contentPayload(path, major, version, contentHash);
    std::string chainHash = chainHashOfPayload(prevChainHash, payload);
    std::ofstream out(dbPath, std::ios::binary | std::ios::app);
    if (!out) { err = "cannot write '" + dbPath + "'"; return false; }
    out << chainHash << ' ' << prevChainHash << ' ' << path << ' ' << major << ' ' << version
        << ' ' << contentHash << '\n';
    return out.good();
}

bool appendYankEntry(const std::string& dbPath, const std::string& prevChainHash,
                     const std::string& path, int major, const std::string& version,
                     std::string& err) {
    size_t slash = dbPath.find_last_of('/');
    if (slash != std::string::npos && !ensureDirRec(dbPath.substr(0, slash))) {
        err = "cannot create directory for '" + dbPath + "'";
        return false;
    }
    std::string payload = yankPayload(path, major, version);
    std::string chainHash = chainHashOfPayload(prevChainHash, payload);
    std::ofstream out(dbPath, std::ios::binary | std::ios::app);
    if (!out) { err = "cannot write '" + dbPath + "'"; return false; }
    out << chainHash << ' ' << prevChainHash << ' ' << payload << '\n';
    return out.good();
}

}  // namespace

std::string checksumDbPath() {
    if (const char* home = std::getenv("TRIDENT_HOME")) return std::string(home) + "/checksum.db";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.trident/checksum.db";
    return "./.trident/checksum.db";
}

bool checksumDbVerifyOrRecord(const std::string& dbPath, const ModuleId& mod,
                              const Version& version, const std::string& contentHash,
                              std::string& err) {
    std::vector<ChecksumEntry> entries;
    std::string lastChainHash;
    if (!loadChecksumDb(dbPath, entries, lastChainHash, err)) return false;

    std::string versionText = formatSemVer(version);
    for (const ChecksumEntry& e : entries) {
        if (e.kind != ChecksumEntry::Kind::Content) continue;
        if (e.path != mod.path || e.major != mod.major || e.version != versionText) continue;
        if (e.contentHash == contentHash) return true;   // matches the recorded baseline
        err = "content for " + mod.path + "@" + versionText + " does not match the checksum "
             "DB (recorded sha256:" + e.contentHash + ", fetched sha256:" + contentHash +
             ") — the tag may have been moved or the content swapped after it was first "
             "recorded";
        return false;
    }

    // First time this module@version has ever been fetched: record it as
    // the trusted baseline for every future fetch.
    return appendChecksumEntry(dbPath, lastChainHash, mod.path, mod.major, versionText,
                               contentHash, err);
}

bool checksumDbIsYanked(const std::string& dbPath, const ModuleId& mod,
                        const Version& version, bool& yanked, std::string& err) {
    std::vector<ChecksumEntry> entries;
    std::string lastChainHash;
    if (!loadChecksumDb(dbPath, entries, lastChainHash, err)) return false;
    (void)lastChainHash;
    const std::string versionText = formatSemVer(version);
    yanked = false;
    for (const ChecksumEntry& e : entries) {
        if (e.kind == ChecksumEntry::Kind::Yank && e.path == mod.path &&
            e.major == mod.major && e.version == versionText)
            yanked = true;
    }
    return true;
}

bool checksumDbYank(const std::string& dbPath, const ModuleId& mod,
                    const Version& version, std::string& err) {
    std::vector<ChecksumEntry> entries;
    std::string lastChainHash;
    if (!loadChecksumDb(dbPath, entries, lastChainHash, err)) return false;

    const std::string versionText = formatSemVer(version);
    bool hasContent = false;
    bool alreadyYanked = false;
    for (const ChecksumEntry& e : entries) {
        if (e.path != mod.path || e.major != mod.major || e.version != versionText) continue;
        if (e.kind == ChecksumEntry::Kind::Content) hasContent = true;
        if (e.kind == ChecksumEntry::Kind::Yank) alreadyYanked = true;
    }
    if (!hasContent) {
        err = "cannot yank " + mod.path + "@" + versionText +
              ": no immutable content hash is recorded in the checksum DB "
              "(fetch or publish the version first)";
        return false;
    }
    if (alreadyYanked) return true;  // idempotent
    return appendYankEntry(dbPath, lastChainHash, mod.path, mod.major, versionText, err);
}
