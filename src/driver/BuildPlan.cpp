#include "driver/BuildPlan.hpp"
#include "frontend/Lexer.hpp"
#include "core/Token.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>

// -----------------------------------------------------------------------------
// The build-plan reader (techdesign-toolchain.md §3.3). A small, dedicated
// grammar over the same Lexer/Token machinery the rest of the compiler uses —
// deliberately NOT the project{} manifest parser (that lives in trident). The
// plan is machine-generated, never hand-authored, so this reader favors a
// simple, uniform "block or field" grammar over manifest-style ergonomics.
// -----------------------------------------------------------------------------

namespace {

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// A string token's text still carries its surrounding quotes; strip them.
std::string unquote(std::string_view tok) {
    if (tok.size() >= 2 && (tok.front() == '"' || tok.front() == '\''))
        return std::string(tok.substr(1, tok.size() - 2));
    return std::string(tok);
}

// A cursor over the plan's tokens. Shaped like Project.cpp's manifest reader,
// but a distinct grammar: `plan { (field = value;)* (entry|src|edge { ... })* }`.
struct PlanReader {
    const std::vector<Token>& toks;
    DiagnosticSink& sink;
    size_t i = 0;

    const Token& cur() const { return toks[i]; }
    bool at(TokenKind k) const { return toks[i].kind == k; }
    void advance() { if (toks[i].kind != TokenKind::End) ++i; }

    bool expect(TokenKind k, const char* what) {
        if (!at(k)) {
            sink.error(cur().span, std::string("expected ") + what);
            return false;
        }
        advance();
        return true;
    }

    bool expectIdent(const char* text) {
        if (!(at(TokenKind::Identifier) && cur().text == text)) {
            sink.error(cur().span, std::string("expected '") + text + "'");
            return false;
        }
        advance();
        return true;
    }

    // A scalar field value: a string, or a bare integer (optLevel).
    bool scalar(std::string& out) {
        if (at(TokenKind::StringLiteral)) { out = unquote(cur().text); advance(); return true; }
        if (at(TokenKind::IntLiteral))    { out = std::string(cur().text); advance(); return true; }
        sink.error(cur().span, "expected a string or a number");
        return false;
    }

    // Generic `{ (field = value;)* }` walker: calls `onField(name, value)` for
    // every field. Used by entry/src/edge, whose fields are all scalars.
    template <class OnField>
    bool fieldBlock(OnField&& onField) {
        if (!expect(TokenKind::LBrace, "'{'")) return false;
        while (!at(TokenKind::RBrace) && !at(TokenKind::End)) {
            if (!at(TokenKind::Identifier)) {
                sink.error(cur().span, "expected a field name");
                return false;
            }
            std::string field(cur().text);
            advance();
            if (!expect(TokenKind::Eq, "'='")) return false;
            std::string value;
            if (!scalar(value)) return false;
            onField(field, value);
            if (at(TokenKind::Semicolon)) advance();
        }
        return expect(TokenKind::RBrace, "'}'");
    }
};

}  // namespace

BuildPlan readBuildPlan(const std::string& planPath, DiagnosticSink& sink) {
    BuildPlan plan;
    plan.planFile.name = planPath;
    if (!readWholeFile(planPath, plan.planFile.text)) {
        sink.error({0, 0}, "cannot read build plan '" + planPath + "'");
        return plan;
    }

    Lexer lexer(plan.planFile, sink);
    std::vector<Token> toks = lexer.tokenize();
    PlanReader r{toks, sink};

    if (!r.expectIdent("plan")) return plan;
    if (!r.expect(TokenKind::LBrace, "'{'")) return plan;

    bool sawEntry = false;
    while (!r.at(TokenKind::RBrace) && !r.at(TokenKind::End)) {
        if (!r.at(TokenKind::Identifier)) {
            sink.error(r.cur().span, "expected a field or block name");
            return plan;
        }
        std::string name(r.cur().text);
        r.advance();

        if (name == "entry") {
            std::string kind, target;
            if (!r.fieldBlock([&](const std::string& f, const std::string& v) {
                    if (f == "kind") kind = v;
                    else if (f == "target") target = v;
                    else sink.error(r.cur().span, "unknown 'entry' field '" + f + "'");
                }))
                return plan;
            if (kind == "script")        plan.entryKind = PlanEntryKind::Script;
            else if (kind == "file")     plan.entryKind = PlanEntryKind::File;
            else if (kind == "function") plan.entryKind = PlanEntryKind::Function;
            else { sink.error(r.cur().span, "entry.kind must be 'script', 'file', or 'function'"); return plan; }
            plan.entryTarget = target;
            sawEntry = true;
        } else if (name == "src") {
            PlanSource s;
            if (!r.fieldBlock([&](const std::string& f, const std::string& v) {
                    if (f == "path") s.path = v;
                    else if (f == "moduleId") s.moduleId = v;
                    else if (f == "origin") s.origin = v;
                    else sink.error(r.cur().span, "unknown 'src' field '" + f + "'");
                }))
                return plan;
            if (s.path.empty()) { sink.error(r.cur().span, "'src' is missing 'path'"); return plan; }
            plan.sources.push_back(std::move(s));
        } else if (name == "edge") {
            PlanEdge e;
            if (!r.fieldBlock([&](const std::string& f, const std::string& v) {
                    if (f == "from") e.from = v;
                    else if (f == "to") e.to = v;
                    else sink.error(r.cur().span, "unknown 'edge' field '" + f + "'");
                }))
                return plan;
            plan.edges.push_back(std::move(e));
        } else if (name == "asset") {
            PlanAsset a;
            if (!r.fieldBlock([&](const std::string& f, const std::string& v) {
                    if (f == "rel") a.rel = v;
                    else if (f == "path") a.path = v;
                    else if (f == "moduleId") a.moduleId = v;
                    else if (f == "hash") a.hash = v;
                    else sink.error(r.cur().span, "unknown 'asset' field '" + f + "'");
                }))
                return plan;
            if (a.rel.empty()) { sink.error(r.cur().span, "'asset' is missing 'rel'"); return plan; }
            if (a.path.empty()) { sink.error(r.cur().span, "'asset' is missing 'path'"); return plan; }
            plan.assets.push_back(std::move(a));
        } else if (name == "out" || name == "mode" || name == "target" || name == "optLevel") {
            if (!r.expect(TokenKind::Eq, "'='")) return plan;
            std::string v;
            if (!r.scalar(v)) return plan;
            if (name == "out")           plan.out = v;
            else if (name == "mode")     plan.mode = v;
            else if (name == "target")   plan.target = v;
            else                          plan.optLevel = std::atoi(v.c_str());
            if (r.at(TokenKind::Semicolon)) r.advance();
        } else {
            sink.error(r.cur().span, "unknown plan field or block '" + name + "'");
            return plan;
        }
    }
    if (!r.expect(TokenKind::RBrace, "'}'")) return plan;

    if (plan.sources.empty())
        sink.error({0, 0}, "build plan lists no 'src' rows — nothing to compile");
    if (!sawEntry)
        sink.error({0, 0}, "build plan is missing an 'entry { ... }' block");

    plan.ok = !sink.hasErrors();
    return plan;
}
