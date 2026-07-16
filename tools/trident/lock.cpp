#include "lock.hpp"
#include "semver.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace {

// A minimal hand-rolled TOML-subset reader, mirroring manifest.cpp's
// TomlReader cursor style (deliberately duplicated — this file has no
// dependency on manifest.cpp, and the two grammars differ: this one needs
// a top-level integer `version` and `[[module]]`'s `requires` array).
struct LockReader {
    const std::string& text;
    size_t i = 0;
    std::string path;
    std::string* err;

    bool atEnd() const { return i >= text.size(); }
    char peek() const { return atEnd() ? '\0' : text[i]; }

    int line() const {
        int l = 1;
        for (size_t k = 0; k < i && k < text.size(); ++k)
            if (text[k] == '\n') ++l;
        return l;
    }

    bool fail(const std::string& msg) {
        if (err) *err = path + ":" + std::to_string(line()) + ": " + msg;
        return false;
    }

    void skipWhitespaceAndComments() {
        for (;;) {
            while (!atEnd() && (peek() == ' ' || peek() == '\t' ||
                                peek() == '\r' || peek() == '\n'))
                ++i;
            if (!atEnd() && peek() == '#') {
                while (!atEnd() && peek() != '\n') ++i;
                continue;
            }
            break;
        }
    }

    bool parseIdent(std::string& out) {
        skipWhitespaceAndComments();
        size_t start = i;
        while (!atEnd() && (std::isalnum((unsigned char)peek()) ||
                            peek() == '_' || peek() == '-'))
            ++i;
        if (i == start) return fail("expected an identifier");
        out = text.substr(start, i - start);
        return true;
    }

    bool parseString(std::string& out) {
        skipWhitespaceAndComments();
        if (peek() != '"') return fail("expected a quoted string");
        ++i;
        out.clear();
        while (!atEnd() && peek() != '"') {
            char c = text[i++];
            if (c == '\\' && !atEnd()) {
                char e = text[i++];
                switch (e) {
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    default:   out += e;    break;
                }
            } else {
                out += c;
            }
        }
        if (atEnd()) return fail("unterminated string");
        ++i;
        return true;
    }

    bool parseStringArray(std::vector<std::string>& out) {
        skipWhitespaceAndComments();
        if (peek() != '[') return fail("expected '['");
        ++i;
        skipWhitespaceAndComments();
        if (peek() == ']') { ++i; return true; }
        for (;;) {
            std::string s;
            if (!parseString(s)) return false;
            out.push_back(std::move(s));
            skipWhitespaceAndComments();
            if (peek() == ',') {
                ++i;
                skipWhitespaceAndComments();
                if (peek() == ']') { ++i; break; }
                continue;
            }
            if (peek() == ']') { ++i; break; }
            return fail("expected ',' or ']' in array");
        }
        return true;
    }

    bool parseInt(int& out) {
        skipWhitespaceAndComments();
        size_t start = i;
        if (!atEnd() && peek() == '-') ++i;
        while (!atEnd() && std::isdigit((unsigned char)peek())) ++i;
        if (i == start || (i == start + 1 && text[start] == '-'))
            return fail("expected an integer");
        out = std::atoi(text.substr(start, i - start).c_str());
        return true;
    }
};

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// §3.4's [[module]] `requires` entries: "path@MAJOR.MINOR.PATCH" — the full
// required version, not the module's own "@vN" IDENTITY suffix (a version
// fully determines its major, so this is unambiguous without one).
std::string serializeRequireEdge(const Require& r) {
    return r.mod.path + "@" + formatSemVer(r.min);
}

}  // namespace

std::string serializeModuleId(const ModuleId& mod) {
    if (mod.major <= 1) return mod.path;
    return mod.path + "@v" + std::to_string(mod.major);
}

bool parseModuleId(const std::string& text, ModuleId& out) {
    size_t at = text.rfind("@v");
    if (at != std::string::npos) {
        std::string majorStr = text.substr(at + 2);
        if (!majorStr.empty() &&
            std::all_of(majorStr.begin(), majorStr.end(),
                       [](unsigned char c) { return std::isdigit(c); })) {
            out.path = text.substr(0, at);
            out.major = std::atoi(majorStr.c_str());
            return true;
        }
    }
    out.path = text;
    out.major = 1;   // the major<=1 bucket — dependencyToRequire (mvs.cpp)
                     // never actually produces a ModuleId with major 0
    return true;
}

Lockfile lockfileFromBuildList(const std::vector<BuildListEntry>& buildList) {
    Lockfile lock;
    lock.version = 1;
    lock.modules.reserve(buildList.size());
    for (const BuildListEntry& e : buildList) {
        LockedModule m;
        m.mod = e.mod;
        m.selected = formatSemVer(e.selected);
        m.hash = "sha256:" + e.contentHash;
        for (const Require& r : e.requires_) m.requires_.push_back(serializeRequireEdge(r));
        lock.modules.push_back(std::move(m));
    }
    return lock;
}

bool writeLockfile(const std::string& path, const Lockfile& lock, std::string& err) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        err = "cannot write lockfile '" + path + "'";
        return false;
    }

    out << "version = " << lock.version << "\n";
    for (const LockedModule& m : lock.modules) {
        out << "\n[[module]]\n";
        out << "path = " << quote(serializeModuleId(m.mod)) << "\n";
        out << "selected = " << quote(m.selected) << "\n";
        out << "hash = " << quote(m.hash) << "\n";
        if (!m.requires_.empty()) {
            out << "requires = [";
            for (size_t i = 0; i < m.requires_.size(); ++i) {
                if (i) out << ", ";
                out << quote(m.requires_[i]);
            }
            out << "]\n";
        }
    }
    return out.good();
}

bool parseLockfile(const std::string& path, const std::string& text, Lockfile& out,
                   std::string& err) {
    LockReader r{text, 0, path, &err};
    LockedModule* cur = nullptr;
    bool sawVersion = false;

    for (;;) {
        r.skipWhitespaceAndComments();
        if (r.atEnd()) break;

        if (r.peek() == '[') {
            ++r.i;
            if (r.peek() != '[') return r.fail("only '[[module]]' tables are supported");
            ++r.i;
            std::string name;
            if (!r.parseIdent(name)) return false;
            if (r.peek() != ']') return r.fail("expected ']]'");
            ++r.i;
            if (r.peek() != ']') return r.fail("expected ']]'");
            ++r.i;
            if (name != "module") return r.fail("unknown table '[[" + name + "]]' (only [[module]])");
            out.modules.push_back(LockedModule{});
            cur = &out.modules.back();
            continue;
        }

        std::string key;
        if (!r.parseIdent(key)) return false;
        r.skipWhitespaceAndComments();
        if (r.peek() != '=') return r.fail("expected '=' after '" + key + "'");
        ++r.i;
        r.skipWhitespaceAndComments();

        if (cur) {
            if (key == "path") {
                std::string text2;
                if (!r.parseString(text2)) return false;
                if (!parseModuleId(text2, cur->mod)) return r.fail("bad module path '" + text2 + "'");
            } else if (key == "selected") {
                if (!r.parseString(cur->selected)) return false;
            } else if (key == "hash") {
                if (!r.parseString(cur->hash)) return false;
            } else if (key == "requires") {
                if (!r.parseStringArray(cur->requires_)) return false;
            } else {
                return r.fail("unknown [[module]] field '" + key + "'");
            }
        } else {
            if (key == "version") {
                if (!r.parseInt(out.version)) return false;
                sawVersion = true;
            } else {
                return r.fail("unknown lockfile field '" + key + "'");
            }
        }
    }

    if (!sawVersion) return r.fail("lockfile is missing 'version'");
    for (const LockedModule& m : out.modules)
        if (m.mod.path.empty()) return r.fail("a [[module]] is missing 'path'");
    return true;
}

std::string lockfilePathFor(const std::string& manifestPath) {
    size_t slash = manifestPath.find_last_of('/');
    std::string dir = slash == std::string::npos ? "" : manifestPath.substr(0, slash + 1);
    std::string base = slash == std::string::npos ? manifestPath : manifestPath.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
    return dir + stem + ".lock";
}

bool lockfileMatchesBuildList(const Lockfile& lock, const std::vector<BuildListEntry>& buildList,
                              std::string& err) {
    if (lock.modules.size() != buildList.size()) {
        err = "trident.lock is stale (module count differs from the current resolution) — "
             "run `trident lock`";
        return false;
    }
    // Build lists come out of MVS already sorted by ModuleId (mvs.cpp); a
    // freshly-written lock preserves that order (lockfileFromBuildList does
    // not re-sort) — compare positionally.
    for (size_t idx = 0; idx < buildList.size(); ++idx) {
        const BuildListEntry& e = buildList[idx];
        const LockedModule& m = lock.modules[idx];
        if (!(m.mod == e.mod)) {
            err = "trident.lock is stale (module set differs from the current resolution) — "
                 "run `trident lock`";
            return false;
        }
        if (m.selected != formatSemVer(e.selected)) {
            err = "trident.lock is stale (" + e.mod.path + " selected version differs from "
                 "the current resolution) — run `trident lock`";
            return false;
        }
        std::vector<std::string> curRequires;
        for (const Require& r : e.requires_) curRequires.push_back(serializeRequireEdge(r));
        if (m.requires_ != curRequires) {
            err = "trident.lock is stale (" + e.mod.path + "'s requires differ from the "
                 "current resolution) — run `trident lock`";
            return false;
        }
    }
    return true;
}
