#pragma once
#include "core/Ast.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Render the AST as an indented tree, for eyeballing structure and for
// round-trip snapshot tests. This is a structural dump, not a source re-emit.
// Backs `--ast` and `--ast-after-rules`.
std::string printProgram(const Program& program);

// One provenance annotation for the source-shaped printer: any printed
// declaration whose span falls inside [tmplStart, tmplEnd) was injected by the
// rule whose firing produced this record, so `comment` is emitted above it.
// (Decoupled from Rules.hpp's ExpansionRecord so the printer stays lightweight;
// main.cpp builds these from the engine's expansions.)
struct ExpandProvenance {
    uint32_t tmplStart = 0;
    uint32_t tmplEnd = 0;
    std::string comment;   // e.g. "from rule Web::reg @ users.lev:8"
};

// Render the post-rules AST as compilable Leviathan source (metaprog Phase 4
// §6): real class/method/=>/{} forms, not `Class …`/`Method …` labels. The
// output, saved as a `.lev` file, compiles and runs identically to the original
// (the round-trip acceptance). Backs `--expand`. Injected declarations carry a
// `// from rule …` provenance comment when `prov` locates them.
//
// `importLits` (LA-20 §8): span offset -> {rel, bytes} for literals folded
// from an `import()` call (RuleEngine::importLiteralSpans). A literal over
// ~200 bytes elides to a provenance comment instead of dumping raw content —
// large templates would otherwise drown the dump. Small ones print verbatim
// (so the round-trip fixture stays honest); elided programs are not byte-
// round-trippable and are expected to carry `@no-roundtrip`.
std::string printProgramSource(const Program& program,
                               const std::vector<ExpandProvenance>& prov = {},
                               const std::map<uint32_t, std::pair<std::string, size_t>>& importLits = {});
