#pragma once
#include "core/Source.hpp"
#include "core/Diagnostic.hpp"
#include <map>
#include <set>
#include <string>
#include <vector>

struct Program;   // Ast.hpp

// -----------------------------------------------------------------------------
// The project / file system (§12): a multi-file program is gathered into one
// logical compilation unit. This is the two-source prelude gather generalized
// from 2 (prelude + one file) to N (prelude + every project source).
//
// leviathan does not own the manifest or dependency resolution — that is
// trident's job exclusively (techdesign-toolchain.md §3.2/§3.3). leviathan's
// only project-shaped input is a resolved build plan (src/BuildPlan.hpp),
// read via loadProjectFromPlan below: every source is already on disk,
// grouped by moduleId/origin, with an explicit entry and a moduleDeps
// adjacency — trident decided all of that; leviathan just gathers and
// enforces.
// -----------------------------------------------------------------------------

// One physical source inside a loaded project, mapped to its byte range in the
// combined compilation buffer (so a diagnostic offset can name the right file).
struct ProjectFile {
    std::string path;
    uint32_t offset = 0;   // start of this file's text in `combined.text`
    uint32_t length = 0;
    std::string origin;    // "" for the project itself; else the dependency's
                           // module path (so diagnostics can name the dep)
    std::string moduleId;  // canonical module root dir ("" = root project) —
                           // stable identity for the dependency graph
};

// How the program starts (§ project system):
//   Script   - no `entry`: top-level statements run in source order.
//   File     - `entry = "main.ext"`: only that file's top-level runs; top-level
//              code in any other file is an error.
//   Function - `entry = "main"` / `"App::main"`: gather everything, call that
//              function; any top-level executable statement is an error.
enum class EntryMode { Script, File, Function };

struct LoadedProject {
    bool ok = false;
    SourceFile combined;              // prelude-style gather: all sources, one buffer
    std::vector<ProjectFile> files;   // offset map for span -> (file, line)
    SourceFile manifestFile;          // kept so plan diagnostics can render
    EntryMode entryMode = EntryMode::Script;
    int entryFileIndex = -1;          // File mode: index into `files`
    uint32_t entryCallOffset = 0;     // Function mode: offset of the synthetic
                                      // entry call (>= every file's range)
    // The human-readable entry target (a file path or function name), for
    // validateEntry's diagnostic text.
    std::string entryName;
    // The dependency graph, by canonical module dir ("" = root): each module
    // maps to the set of modules it directly declared as deps. Drives phantom-
    // dep prevention (a module may only `uses` its own / direct-dep namespaces).
    std::map<std::string, std::set<std::string>> moduleDeps;

    // LA-20 §8: declared build-input assets (comptime `import()` targets),
    // moduleId -> (rel -> abs). Filled by loadProjectFromPlan from the plan's
    // `asset` rows; empty for a bare single file (rootDir-mode resolution
    // instead, see main.cpp's ImportContext construction).
    std::map<std::string, std::map<std::string, std::string>> assets;
    // abs path -> "sha256:..." hash string, kept only for `--assets` display
    // (plan mode shows it; single-file mode has none, by design). Keyed by
    // absolute path (not rel) since the same rel name may recur across
    // modules with distinct hashes.
    std::map<std::string, std::string> assetHashes;
};

// Load a project from a resolved build plan (techdesign-toolchain.md §3.3) —
// trident's frozen hand-off to leviathan. Every plan `src` path is already
// absolute and on disk (no manifest parsing, no dependency resolution, no
// path resolution beyond opening the files). Reuses the same whole-program
// gather as loadProject; only the input shape differs. On any error, reports
// to `sink` and returns a LoadedProject with ok == false.
LoadedProject loadProjectFromPlan(const std::string& planPath, DiagnosticSink& sink);

// Render diagnostics against a concatenated project buffer, attributing each to
// its owning source file and that file's own 1-based line (not the combined
// buffer's). Mirrors DiagnosticSink::render's caret format.
void renderProjectDiagnostics(const SourceFile& combined,
                              const std::vector<ProjectFile>& files,
                              const DiagnosticSink& sink);

// -----------------------------------------------------------------------------
// P-4: the retained `file -> imports` provenance map (§12).
//
// The multi-file gather (P-2/P-3) dissolves file boundaries into one tree; this
// records which file each namespace-open and each `uses`/`use` import came
// from, so a downstream consumer (e.g. a namespace-scoped compile-time rule,
// §16.5) can ask "which namespaces are visible to declarations in this file"
// as a lookup rather than re-deriving it. For each file:
//   declaresInto    - namespaces this file contributes declarations to
//                     ("<root>" = top-level, outside any namespace)
//   importsExplicit - namespaces this file draws from, via `uses NS` or a
//                     selective `use NS::name` (imports.md §5 — both count
//                     the same way for phantom-dep purposes)
//   useNames        - selective `use path::name (as alias)` rows, for
//                     display only (`--imports`, imports.md §5)
//   effective       - declaresInto ∪ importsExplicit ∪ {std}  (std is implicit)
// -----------------------------------------------------------------------------
struct FileImports {
    std::string path;
    std::set<std::string> declaresInto;
    std::set<std::string> importsExplicit;
    std::set<std::string> useNames;
    std::set<std::string> effective;
};

// Validate the entry rule against the parsed program: with an explicit entry
// (File or Function), a top-level executable statement in a disallowed place is
// an error (reported to `sink`). Script mode accepts anything. Call after parse,
// before resolution.
void validateEntry(const LoadedProject& proj, const Program& program,
                   DiagnosticSink& sink);

// Phantom-dependency prevention (§ package manager, §6.2): a `uses NS` is legal
// only if NS is provided by the importing file's own module, a module it
// directly declared as a dep, or std. Using a namespace that lives only in an
// undeclared (indirect / transitive) dependency is an error. Call after parse.
void checkPhantomDeps(const LoadedProject& proj, const Program& program,
                      DiagnosticSink& sink);

// Compute the provenance map. Each top-level item is attributed to its owning
// file by span offset (the concatenation offset map), keyed positionally to
// `files` (FileId == index).
std::vector<FileImports> computeFileImports(const std::vector<ProjectFile>& files,
                                            const Program& program);

// Render the provenance map for the `--imports` introspection mode.
std::string renderFileImports(const std::vector<FileImports>& imports);

// -----------------------------------------------------------------------------
// P-3: the `uses` include graph + a deterministic build/processing order (§12).
//
// Derived (not authored) from the P-4 provenance map: nodes are project files
// and the namespaces they open/import. From the per-file (declaresInto,
// importsExplicit) sets we recover
//   declaredBy[NS]  - the files that open namespace NS      ("which files declare into NS?")
//   importedBy[NS]  - the files that `uses NS`              ("which file imported NS?")
//   fileDeps[i]     - files file i depends on (i uses a namespace they declare)
// and a build order. Because the gather is whole-program (§17), resolution is
// cycle-immune and needs no order; the order matters only for the rule stage and
// deterministic diagnostics (proposal §5.3). Cycles are therefore *allowed*:
// namespaces can be re-opened and mutually `uses` one another. We condense each
// strongly-connected component (a `uses` cycle) and emit components in
// reverse-topological order (every dependency before its dependents), starting
// DFS from the lowest file index so ties break deterministically ascending.
// File indices are positional == FileId == index into `files`.
//
// `std` and top-level `<root>` never produce edges: no project file declares
// into `std`, and no file can `uses <root>` — so both drop out of the graph.
// -----------------------------------------------------------------------------
struct UsesGraph {
    std::vector<std::string>                   paths;       // file index -> path
    std::map<std::string, std::vector<int>>    declaredBy;  // NS -> files opening it
    std::map<std::string, std::vector<int>>    importedBy;  // NS -> files that `uses` it
    std::vector<std::set<int>>                 fileDeps;    // file -> files it depends on
    std::vector<int>                           order;       // deterministic build order
    std::vector<std::vector<int>>              cycles;      // SCCs with >1 file (the `uses` cycles)
};

// Build the include graph from the provenance map (both keyed positionally to
// the same `files`, so FileImports[i] and UsesGraph file index i agree).
UsesGraph buildUsesGraph(const std::vector<FileImports>& imports);

// Render the include graph + build order for the `--graph` introspection mode.
std::string renderUsesGraph(const UsesGraph& g);

// -----------------------------------------------------------------------------
// Discoverability queries (proposal-project-system.md §4.4 / §6.3): buy back the
// by-name + path-decoupled "where does this name come from?" tax (the C#
// quadrant, §3.1) with compiler introspection instead of directory rules. Both
// derive from the same AST + offset map as `--imports` / `--graph` (no resolver
// run), so they answer purely structural questions and stay consistent with
// leviathan's existing `--ast` / `--resolve` introspection surface.
// -----------------------------------------------------------------------------

// `--namespaces`: every namespace, the files that open it, and the members it
// declares directly — the "symbol index" (§4.4). `<root>` is the top-level
// (no-namespace) scope; its "opened-in" files are those with top-level
// declarations.
struct NamespaceInfo {
    std::string name;                       // full ::-path, or "<root>" for top level
    std::vector<std::string> openedIn;      // files that open it (ascending file order)
    std::vector<std::string> members;       // member names declared directly in it (sorted)
};
std::vector<NamespaceInfo> computeNamespaces(const std::vector<ProjectFile>& files,
                                             const Program& program);
std::string renderNamespaces(const std::vector<NamespaceInfo>& nss);

// `--why <name> [in <file>]`: every namespace a bare `name` could resolve to and,
// for a given file, which candidates are visible and whether the choice is
// ambiguous — the provenance query by-name import otherwise erases (§4.4). Type-
// based disambiguation of a same-name/same-scope clash remains the Checker's job;
// this reports at the granularity of candidate namespaces.
struct WhyCandidate {
    std::string ns;                          // namespace declaring `name` ("<root>" = top level)
    std::vector<std::string> declaredIn;     // files declaring `name` in ns (ascending file order)
    std::vector<std::string> visibleTo;      // files whose effective import set makes ns visible
};
struct WhyResult {
    std::string name;
    std::vector<std::string> allFiles;       // every project file's path (for `in <file>` validation)
    std::vector<WhyCandidate> candidates;    // sorted by namespace name
};
WhyResult computeWhy(const std::vector<ProjectFile>& files, const Program& program,
                     const std::string& name);
// `inFile` empty => a project-wide candidate report; non-empty (a path or a
// trailing basename) => narrow to that file's effective import set and report the
// winner / ambiguity.
std::string renderWhy(const WhyResult& why, const std::string& inFile);

// -----------------------------------------------------------------------------
// `--lint-namespaces` (proposal-project-system.md §4.4, Phase 0.4): the optional,
// opt-in folder≈namespace convention check. OFF by default — you run this mode
// explicitly; a normal build never applies it (§12 keeps namespaces decoupled
// from paths). It buys the Go/Java directory tidiness for teams who want it
// without imposing it: a root-project file that declares into a namespace should
// sit in a directory whose path (relative to the project root) is a case-
// insensitive suffix of that namespace's ::-segments (so `models/link.lev`
// opening `App::Models` matches — "models" ~ "Models"). Dependency files
// (origin != "") are never linted; a file that declares into no namespace (a
// pure top-level composition root) is exempt.
// -----------------------------------------------------------------------------
struct LayoutFinding {
    std::string path;                        // the linted file
    std::string relDir;                      // its directory relative to the project root ("" = root)
    std::vector<std::string> declaresInto;   // the namespaces it opens (sorted)
    bool matches = false;                    // did one of them match the folder?
};
struct LayoutLint {
    std::vector<LayoutFinding> findings;     // one per linted file, in file order
    int mismatches = 0;
};
LayoutLint lintNamespaceLayout(const std::vector<ProjectFile>& files, const Program& program);
std::string renderLayoutLint(const LayoutLint& lint);
