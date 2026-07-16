#include "manifest.hpp"
#include <cctype>
#include <fstream>

// -----------------------------------------------------------------------------
// A minimal hand-rolled TOML reader — just the subset a manifest needs:
// top-level `key = value` (string, string array, or bool), plus repeated
// `[[dep]]` array-of-tables. No inline tables, no nested arrays, no numeric
// types beyond what `dev` needs, no multi-line strings. Deliberately small:
// trident carries no external TOML dependency.
// -----------------------------------------------------------------------------

namespace {

struct TomlReader {
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
        ++i;   // closing quote
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

    bool parseBool(bool& out) {
        skipWhitespaceAndComments();
        if (text.compare(i, 4, "true") == 0)  { out = true;  i += 4; return true; }
        if (text.compare(i, 5, "false") == 0) { out = false; i += 5; return true; }
        return fail("expected 'true' or 'false'");
    }
};

}  // namespace

bool parseManifest(const std::string& path, const std::string& text,
                   ProjectManifest& out, std::string& err) {
    TomlReader r{text, 0, path, &err};
    Dependency* curDep = nullptr;

    for (;;) {
        r.skipWhitespaceAndComments();
        if (r.atEnd()) break;

        if (r.peek() == '[') {
            ++r.i;
            if (r.peek() != '[') return r.fail("only '[[dep]]' tables are supported");
            ++r.i;
            std::string name;
            if (!r.parseIdent(name)) return false;
            if (r.peek() != ']') return r.fail("expected ']]'");
            ++r.i;
            if (r.peek() != ']') return r.fail("expected ']]'");
            ++r.i;
            if (name != "dep") return r.fail("unknown table '[[" + name + "]]' (only [[dep]])");
            out.deps.push_back(Dependency{});
            curDep = &out.deps.back();
            continue;
        }

        std::string key;
        if (!r.parseIdent(key)) return false;
        r.skipWhitespaceAndComments();
        if (r.peek() != '=') return r.fail("expected '=' after '" + key + "'");
        ++r.i;
        r.skipWhitespaceAndComments();

        if (curDep) {
            if (key == "path")            { if (!r.parseString(curDep->path)) return false; }
            else if (key == "version")    { if (!r.parseString(curDep->version)) return false; }
            else if (key == "as")         { if (!r.parseString(curDep->as_)) return false; }
            else if (key == "dev")        { if (!r.parseBool(curDep->dev)) return false; }
            else return r.fail("unknown dep field '" + key + "'");
        } else {
            if (key == "name")            { if (!r.parseString(out.name)) return false; }
            else if (key == "entry")      { if (!r.parseString(out.entry)) return false; }
            else if (key == "version")    { if (!r.parseString(out.version)) return false; }
            else if (key == "out")        { if (!r.parseString(out.out)) return false; }
            else if (key == "sources")    { if (!r.parseStringArray(out.sources)) return false; }
            else if (key == "assets")     { if (!r.parseStringArray(out.assets)) return false; }
            else return r.fail("unknown manifest field '" + key + "'");
        }
    }

    for (const Dependency& d : out.deps)
        if (d.path.empty()) return r.fail("a [[dep]] is missing 'path'");

    if (out.sources.empty())
        return r.fail("manifest lists no 'sources' — a project needs at least one file");
    return true;
}

namespace {
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}
}  // namespace

bool writeManifest(const std::string& path, const ProjectManifest& m, std::string& err) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        err = "cannot write manifest '" + path + "'";
        return false;
    }

    if (!m.name.empty()) out << "name = " << quote(m.name) << "\n";
    if (!m.entry.empty()) out << "entry = " << quote(m.entry) << "\n";
    if (!m.version.empty()) out << "version = " << quote(m.version) << "\n";
    if (!m.out.empty()) out << "out = " << quote(m.out) << "\n";
    if (!m.sources.empty()) {
        out << "sources = [";
        for (size_t i = 0; i < m.sources.size(); ++i) {
            if (i) out << ", ";
            out << quote(m.sources[i]);
        }
        out << "]\n";
    }
    if (!m.assets.empty()) {
        out << "assets = [";
        for (size_t i = 0; i < m.assets.size(); ++i) {
            if (i) out << ", ";
            out << quote(m.assets[i]);
        }
        out << "]\n";
    }
    for (const Dependency& d : m.deps) {
        out << "\n[[dep]]\n";
        out << "path = " << quote(d.path) << "\n";
        if (!d.version.empty()) out << "version = " << quote(d.version) << "\n";
        if (!d.as_.empty()) out << "as = " << quote(d.as_) << "\n";
        if (d.dev) out << "dev = true\n";
    }
    return out.good();
}
