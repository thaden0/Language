#include "plan.hpp"
#include <cstdio>
#include <fstream>

namespace {

// Escape a value for the plan's string-literal grammar (BuildPlan.cpp's
// reader lexes it with the ordinary Lexer, so `"`/`\` must be escaped exactly
// as the language's own string literals are).
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

bool writeBuildPlan(const std::string& planPath, const ResolvedProject& rp,
                    const WritePlanOptions& opts) {
    std::ofstream out(planPath, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "error: cannot write build plan '%s'\n", planPath.c_str());
        return false;
    }

    out << "plan {\n";
    out << "    out = " << quote(opts.out) << ";\n";
    out << "    mode = " << quote(opts.mode) << ";\n";
    out << "    target = " << quote(opts.target) << ";\n";
    out << "    optLevel = " << opts.optLevel << ";\n";
    out << "    entry { kind = " << quote(rp.entryKind) << "; target = "
       << quote(rp.entryTarget) << "; }\n";
    for (const ResolvedSource& s : rp.sources) {
        out << "    src { path = " << quote(s.path) << "; moduleId = "
           << quote(s.moduleId) << "; origin = " << quote(s.origin) << "; }\n";
    }
    for (const auto& [from, tos] : rp.moduleDeps)
        for (const std::string& to : tos)
            out << "    edge { from = " << quote(from) << "; to = " << quote(to) << "; }\n";
    // LA-20 §7: declared build-input assets, already resolved + hashed by
    // resolveProject (gatherAssets) — the plan writer just prints the rows.
    for (const ResolvedAsset& a : rp.assets) {
        out << "    asset { rel = " << quote(a.rel) << "; path = " << quote(a.path)
           << "; moduleId = " << quote(a.moduleId) << "; hash = " << quote(a.hash) << "; }\n";
    }
    out << "}\n";

    return out.good();
}
