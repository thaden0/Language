#include "provenance.hpp"
#include "endpoint.hpp"
#include "hash.hpp"
#include "process.hpp"
#include "semver.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::string trim(const std::string& text) {
    size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    size_t last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    return text.substr(first, last - first);
}

std::string stripComment(const std::string& line) {
    bool quoted = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quoted && escaped) { escaped = false; continue; }
        if (quoted && c == '\\') { escaped = true; continue; }
        if (c == '"') quoted = !quoted;
        else if (c == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

std::string quote(const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        if (c == '\n') out += "\\n";
        else out += c;
    }
    out += '"';
    return out;
}

bool parseQuoted(const std::string& text, std::string& out) {
    std::string value = trim(text);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return false;
    out.clear();
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char c = value[i];
        if (c == '\\' && i + 2 < value.size()) {
            char next = value[++i];
            out += next == 'n' ? '\n' : next;
        } else out += c;
    }
    return true;
}

bool parseStringArray(const std::string& text, std::vector<std::string>& out) {
    std::string value = trim(text);
    if (value.size() < 2 || value.front() != '[' || value.back() != ']') return false;
    out.clear();
    size_t i = 1;
    while (i + 1 < value.size()) {
        while (i + 1 < value.size() &&
               (std::isspace(static_cast<unsigned char>(value[i])) || value[i] == ',')) ++i;
        if (i + 1 >= value.size()) break;
        if (value[i] != '"') return false;
        size_t start = i++;
        bool escaped = false;
        while (i < value.size()) {
            if (!escaped && value[i] == '"') { ++i; break; }
            if (!escaped && value[i] == '\\') escaped = true;
            else escaped = false;
            ++i;
        }
        std::string item;
        if (!parseQuoted(value.substr(start, i - start), item)) return false;
        out.push_back(std::move(item));
        while (i + 1 < value.size() && std::isspace(static_cast<unsigned char>(value[i]))) ++i;
        if (i + 1 < value.size() && value[i] != ',') return false;
    }
    return true;
}

bool splitAssignment(const std::string& line, std::string& key, std::string& value) {
    size_t equal = line.find('=');
    if (equal == std::string::npos) return false;
    key = trim(line.substr(0, equal));
    value = trim(line.substr(equal + 1));
    return !key.empty();
}

bool readFile(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    text = ss.str();
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

bool writeFile(const std::string& path, const std::string& text, std::string& err) {
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !ensureDirRec(path.substr(0, slash))) {
        err = "cannot create directory for '" + path + "'";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot write '" + path + "'"; return false; }
    out << text;
    return out.good();
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

std::string resolveBeside(const std::string& ownerPath, const std::string& child) {
    if (child.empty() || child.front() == '/') return child;
    return dirOf(ownerPath) + "/" + child;
}

std::string normalizeHash(const std::string& hash) {
    return hash.compare(0, 7, "sha256:") == 0 ? hash.substr(7) : hash;
}

std::string attestationStatement(const Attestation& a) {
    auto field = [](const std::string& name, const std::string& value) {
        return name + " " + std::to_string(value.size()) + "\n" + value + "\n";
    };
    return "trident-attestation-v1\n" +
           field("path", a.path) +
           field("selected", a.selected) +
           field("hash", "sha256:" + normalizeHash(a.hash)) +
           field("commit", a.commit) +
           field("identity", a.identity) +
           field("artifact", a.artifact) +
           field("artifact_hash", a.artifactHash);
}

std::string hexEncode(const std::string& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

bool hexDecode(const std::string& text, std::string& bytes) {
    if (text.size() % 2 != 0) return false;
    auto value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    bytes.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi = value(text[i]), lo = value(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes += static_cast<char>((hi << 4) | lo);
    }
    return true;
}

bool tempDir(std::string& out, std::string& err) {
    std::string tmpl = "/tmp/trident-attestation-XXXXXX";
    std::vector<char> bytes(tmpl.begin(), tmpl.end());
    bytes.push_back('\0');
    if (::mkdtemp(bytes.data()) == nullptr) {
        err = "cannot create a temporary attestation directory";
        return false;
    }
    out = bytes.data();
    return true;
}

void removeTemp(const std::string& dir) {
    ::unlink((dir + "/statement").c_str());
    ::unlink((dir + "/signature").c_str());
    ::rmdir(dir.c_str());
}

bool writeBinary(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool verifySignature(const Attestation& att, const std::string& publicKey,
                     std::string& err) {
    if (findExecutable("openssl").empty()) {
        err = "cannot find the 'openssl' executable on $PATH (required by provenance policy)";
        return false;
    }
    std::string signature;
    if (!hexDecode(att.signature, signature)) {
        err = "attestation signature is not valid hexadecimal";
        return false;
    }
    std::string temp;
    if (!tempDir(temp, err)) return false;
    bool ok = writeBinary(temp + "/statement", attestationStatement(att)) &&
              writeBinary(temp + "/signature", signature);
    if (!ok) err = "cannot write temporary attestation verification files";
    if (ok) {
        std::string output;
        int rc = runProcess({"openssl", "dgst", "-sha256", "-verify", publicKey,
                             "-signature", temp + "/signature", temp + "/statement"},
                            output, err);
        if (rc != 0) {
            err = "signature verification failed for attestation by '" + att.identity + "'";
            ok = false;
        }
    }
    removeTemp(temp);
    return ok;
}

void listAttestations(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* ent = ::readdir(d)) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (::lstat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) listAttestations(path, out);
        else if (S_ISREG(st.st_mode) && name.size() >= 12 &&
                 name.compare(name.size() - 12, 12, ".attestation") == 0)
            out.push_back(path);
    }
    ::closedir(d);
}

bool sameSelected(const std::string& text, const Version& selected) {
    Version parsed;
    std::string err;
    return parseSemVer(text, parsed, err) && compareSemVer(parsed, selected) == 0;
}

}  // namespace

bool readAuditRecords(const std::string& path, std::vector<AuditRecord>& records,
                      std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot read audit records '" + path + "'"; return false; }
    records.clear();
    AuditRecord* current = nullptr;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(stripComment(line));
        if (line.empty()) continue;
        if (line == "[[audit]]") {
            records.push_back({});
            current = &records.back();
            continue;
        }
        std::string key, value;
        if (!splitAssignment(line, key, value)) {
            err = path + ":" + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        if (!current) {
            if (key != "version" || value != "1") {
                err = path + ":" + std::to_string(lineNo) +
                      ": only top-level 'version = 1' is supported";
                return false;
            }
            continue;
        }
        std::string parsed;
        if (!parseQuoted(value, parsed)) {
            err = path + ":" + std::to_string(lineNo) + ": expected quoted string";
            return false;
        }
        if (key == "path") current->path = parsed;
        else if (key == "selected") current->selected = parsed;
        else if (key == "hash") current->hash = parsed;
        else if (key == "auditor") current->auditor = parsed;
        else {
            err = path + ":" + std::to_string(lineNo) + ": unknown audit field '" + key + "'";
            return false;
        }
    }
    for (const AuditRecord& r : records) {
        if (r.path.empty() || r.selected.empty() || r.hash.empty() || r.auditor.empty()) {
            err = path + ": every [[audit]] requires path, selected, hash, and auditor";
            return false;
        }
    }
    return true;
}

bool appendAuditRecord(const std::string& path, const AuditRecord& record,
                       std::string& err) {
    std::vector<AuditRecord> records;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && !readAuditRecords(path, records, err)) return false;
    for (const AuditRecord& existing : records) {
        if (existing.path == record.path && existing.selected == record.selected &&
            normalizeHash(existing.hash) == normalizeHash(record.hash) &&
            existing.auditor == record.auditor)
            return true;
    }
    records.push_back(record);
    std::sort(records.begin(), records.end(), [](const AuditRecord& a, const AuditRecord& b) {
        if (a.path != b.path) return a.path < b.path;
        if (a.selected != b.selected) return a.selected < b.selected;
        return a.auditor < b.auditor;
    });
    std::ostringstream out;
    out << "version = 1\n";
    for (const AuditRecord& r : records) {
        out << "\n[[audit]]\n"
            << "path = " << quote(r.path) << "\n"
            << "selected = " << quote(r.selected) << "\n"
            << "hash = " << quote("sha256:" + normalizeHash(r.hash)) << "\n"
            << "auditor = " << quote(r.auditor) << "\n";
    }
    return writeFile(path, out.str(), err);
}

bool readAuditPolicy(const std::string& path, AuditPolicy& policy, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot read audit policy '" + path + "'"; return false; }
    policy = AuditPolicy{};
    TrustedKey* current = nullptr;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(stripComment(line));
        if (line.empty()) continue;
        if (line == "[[key]]") {
            policy.keys.push_back({});
            current = &policy.keys.back();
            continue;
        }
        std::string key, value;
        if (!splitAssignment(line, key, value)) {
            err = path + ":" + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        if (current) {
            std::string parsed;
            if (!parseQuoted(value, parsed)) {
                err = path + ":" + std::to_string(lineNo) + ": expected quoted string";
                return false;
            }
            if (key == "identity") current->identity = parsed;
            else if (key == "public_key") current->publicKey = parsed;
            else {
                err = path + ":" + std::to_string(lineNo) + ": unknown key field '" + key + "'";
                return false;
            }
            continue;
        }
        if (key == "version") {
            if (value != "1") {
                err = path + ":" + std::to_string(lineNo) + ": only policy version 1 is supported";
                return false;
            }
        } else if (key == "trusted_auditors") {
            if (!parseStringArray(value, policy.trustedAuditors)) {
                err = path + ":" + std::to_string(lineNo) + ": expected a string array";
                return false;
            }
        } else if (key == "audit_files") {
            if (!parseStringArray(value, policy.auditFiles)) {
                err = path + ":" + std::to_string(lineNo) + ": expected a string array";
                return false;
            }
        } else if (key == "attestation_dirs") {
            if (!parseStringArray(value, policy.attestationDirs)) {
                err = path + ":" + std::to_string(lineNo) + ": expected a string array";
                return false;
            }
        } else if (key == "require_attestations") {
            if (value == "true") policy.requireAttestations = true;
            else if (value == "false") policy.requireAttestations = false;
            else {
                err = path + ":" + std::to_string(lineNo) + ": expected true or false";
                return false;
            }
        } else {
            err = path + ":" + std::to_string(lineNo) + ": unknown policy field '" + key + "'";
            return false;
        }
    }
    for (const TrustedKey& key : policy.keys) {
        if (key.identity.empty() || key.publicKey.empty()) {
            err = path + ": every [[key]] requires identity and public_key";
            return false;
        }
    }
    if (!policy.trustedAuditors.empty() && policy.auditFiles.empty())
        policy.auditFiles.push_back("trident.audits.toml");
    if (policy.requireAttestations && policy.attestationDirs.empty())
        policy.attestationDirs.push_back("attestations");
    if (policy.requireAttestations && policy.keys.empty()) {
        err = path + ": require_attestations = true needs at least one [[key]]";
        return false;
    }
    return true;
}

bool readAttestation(const std::string& path, Attestation& out, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot read attestation '" + path + "'"; return false; }
    out = Attestation{};
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(stripComment(line));
        if (line.empty()) continue;
        std::string key, value;
        if (!splitAssignment(line, key, value)) {
            err = path + ":" + std::to_string(lineNo) + ": expected key = value";
            return false;
        }
        if (key == "version") {
            if (value != "1") { err = path + ": unsupported attestation version"; return false; }
            continue;
        }
        std::string parsed;
        if (!parseQuoted(value, parsed)) {
            err = path + ":" + std::to_string(lineNo) + ": expected quoted string";
            return false;
        }
        if (key == "path") out.path = parsed;
        else if (key == "selected") out.selected = parsed;
        else if (key == "hash") out.hash = parsed;
        else if (key == "commit") out.commit = parsed;
        else if (key == "identity") out.identity = parsed;
        else if (key == "artifact") out.artifact = parsed;
        else if (key == "artifact_hash") out.artifactHash = parsed;
        else if (key == "signature") out.signature = parsed;
        else { err = path + ": unknown attestation field '" + key + "'"; return false; }
    }
    if (out.path.empty() || out.selected.empty() || out.hash.empty() || out.commit.empty() ||
        out.identity.empty() || out.signature.empty()) {
        err = path + ": attestation requires path, selected, hash, commit, identity, and signature";
        return false;
    }
    if ((!out.artifact.empty() && out.artifactHash.empty()) ||
        (out.artifact.empty() && !out.artifactHash.empty())) {
        err = path + ": artifact and artifact_hash must either both be present or both be empty";
        return false;
    }
    return true;
}

bool writeSignedAttestation(const PackageSnapshot& package, const std::string& identity,
                            const std::string& privateKey, const std::string& artifact,
                            const std::string& outputPath, std::string& err) {
    if (identity.empty()) { err = "attestation identity must not be empty"; return false; }
    if (findExecutable("openssl").empty()) {
        err = "cannot find the 'openssl' executable on $PATH (required to sign provenance)";
        return false;
    }
    Attestation att;
    att.path = package.modulePath;
    att.selected = formatSemVer(package.version);
    att.hash = "sha256:" + package.contentHash;
    att.commit = package.commit;
    att.identity = identity;
    att.artifact = artifact;
    if (!artifact.empty()) {
        std::string bytes;
        if (!readFile(artifact, bytes)) { err = "cannot read artifact '" + artifact + "'"; return false; }
        att.artifactHash = "sha256:" + sha256Hex(bytes);
    }

    std::string temp;
    if (!tempDir(temp, err)) return false;
    bool ok = writeBinary(temp + "/statement", attestationStatement(att));
    if (!ok) err = "cannot write temporary attestation statement";
    if (ok) {
        std::string output;
        int rc = runProcess({"openssl", "dgst", "-sha256", "-sign", privateKey,
                             "-out", temp + "/signature", temp + "/statement"}, output, err);
        if (rc != 0) { err = "openssl could not sign the provenance statement"; ok = false; }
    }
    std::string signatureBytes;
    if (ok && !readFile(temp + "/signature", signatureBytes)) {
        err = "cannot read generated provenance signature";
        ok = false;
    }
    if (ok) att.signature = hexEncode(signatureBytes);
    removeTemp(temp);
    if (!ok) return false;

    std::ostringstream out;
    out << "version = 1\n"
        << "path = " << quote(att.path) << "\n"
        << "selected = " << quote(att.selected) << "\n"
        << "hash = " << quote(att.hash) << "\n"
        << "commit = " << quote(att.commit) << "\n"
        << "identity = " << quote(att.identity) << "\n"
        << "artifact = " << quote(att.artifact) << "\n"
        << "artifact_hash = " << quote(att.artifactHash) << "\n"
        << "signature = " << quote(att.signature) << "\n";
    return writeFile(outputPath, out.str(), err);
}

std::string defaultAttestationPath(const PackageSnapshot& package,
                                   const std::string& identity) {
    std::string root;
    if (const char* home = std::getenv("TRIDENT_HOME")) root = home;
    else if (const char* home = std::getenv("HOME")) root = std::string(home) + "/.trident";
    else root = "./.trident";
    return root + "/attestations/" + percentEncodeSegment(package.modulePath) + "-" +
           formatSemVer(package.version) + "-" + percentEncodeSegment(identity) + ".attestation";
}

bool enforceAuditPolicy(const std::string& policyPath,
                        const std::vector<BuildListEntry>& buildList,
                        std::vector<std::string>& report, std::string& err) {
    AuditPolicy policy;
    if (!readAuditPolicy(policyPath, policy, err)) return false;

    std::vector<AuditRecord> audits;
    for (const std::string& file : policy.auditFiles) {
        std::vector<AuditRecord> part;
        if (!readAuditRecords(resolveBeside(policyPath, file), part, err)) return false;
        audits.insert(audits.end(), part.begin(), part.end());
    }

    std::vector<std::pair<std::string, Attestation>> attestations;
    if (policy.requireAttestations) {
        std::vector<std::string> files;
        for (const std::string& dir : policy.attestationDirs)
            listAttestations(resolveBeside(policyPath, dir), files);
        std::sort(files.begin(), files.end());
        for (const std::string& file : files) {
            Attestation att;
            std::string parseErr;
            if (!readAttestation(file, att, parseErr)) {
                err = parseErr;
                return false;
            }
            attestations.push_back({file, std::move(att)});
        }
    }

    std::set<std::string> trustedAuditors(policy.trustedAuditors.begin(),
                                          policy.trustedAuditors.end());
    report.clear();
    for (const BuildListEntry& module : buildList) {
        std::string version = formatSemVer(module.selected);
        if (!trustedAuditors.empty()) {
            const AuditRecord* accepted = nullptr;
            for (const AuditRecord& record : audits) {
                if (record.path == module.mod.path && sameSelected(record.selected, module.selected) &&
                    normalizeHash(record.hash) == module.contentHash &&
                    trustedAuditors.count(record.auditor)) {
                    accepted = &record;
                    break;
                }
            }
            if (!accepted) {
                err = "policy requires a hash-matching audit for " + module.mod.path + "@" +
                      version + " from one of its trusted auditors";
                return false;
            }
            report.push_back("AUDIT " + module.mod.path + "@" + version + " by " +
                             accepted->auditor);
        }

        if (policy.requireAttestations) {
            bool accepted = false;
            std::string lastError;
            for (const auto& [file, att] : attestations) {
                if (att.path != module.mod.path || !sameSelected(att.selected, module.selected) ||
                    normalizeHash(att.hash) != module.contentHash)
                    continue;
                auto key = std::find_if(policy.keys.begin(), policy.keys.end(),
                    [&](const TrustedKey& candidate) { return candidate.identity == att.identity; });
                if (key == policy.keys.end()) continue;
                std::string verifyErr;
                if (verifySignature(att, resolveBeside(policyPath, key->publicKey), verifyErr)) {
                    report.push_back("ATTEST " + module.mod.path + "@" + version + " by " +
                                     att.identity + " (" + file + ")");
                    accepted = true;
                    break;
                }
                lastError = verifyErr;
            }
            if (!accepted) {
                err = "policy requires a valid trusted attestation for " + module.mod.path +
                      "@" + version;
                if (!lastError.empty()) err += ": " + lastError;
                return false;
            }
        }
    }
    return true;
}
