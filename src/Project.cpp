#include "Project.hpp"
#include "Ast.hpp"
#include "BuildPlan.hpp"
#include "Lexer.hpp"
#include "Token.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string readWholeFile(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { ok = false; return {}; }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

// One gathered source file: its path, its origin (the dependency module path,
// or "" for the project itself), and its canonical moduleId (module root dir).
struct GatherEntry {
    std::string path;
    std::string origin;
    std::string moduleId;
};

// Concatenate every gathered file's already-read text into one compilation
// buffer, recording each file's byte range (the offset map). This is the
// whole-program gather itself (§12) — shared by the manifest path
// (loadProject) and the build-plan path (loadProjectFromPlan,
// techdesign-toolchain.md §3.3), which differ only in how `gather` and
// `aliases` were produced, never in how they're laid out.
void layoutCombinedBuffer(const std::vector<GatherEntry>& gather,
                          const std::vector<std::string>& texts,
                          const std::vector<std::pair<std::string, std::string>>& aliases,
                          LoadedProject& proj) {
    auto emit = [&](const std::string& path, const std::string& origin,
                    const std::string& moduleId, const std::string& text) {
        ProjectFile pf;
        pf.path = path;
        pf.origin = origin;
        pf.moduleId = moduleId;
        pf.offset = (uint32_t)proj.combined.text.size();
        pf.length = (uint32_t)text.size();
        proj.combined.text += text;
        proj.combined.text += '\n';
        proj.files.push_back(std::move(pf));
    };
    for (const auto& a : aliases) emit(a.first, "", "", a.second);
    for (size_t i = 0; i < gather.size(); ++i)
        emit(gather[i].path, gather[i].origin, gather[i].moduleId, texts[i]);
}

}  // namespace

LoadedProject loadProjectFromPlan(const std::string& planPath, DiagnosticSink& sink) {
    LoadedProject proj;

    BuildPlan plan = readBuildPlan(planPath, sink);
    proj.manifestFile = plan.planFile;   // reused so plan diagnostics can render too
    if (!plan.ok) return proj;

    // Every plan `src` row is already a resolved, on-disk file — trident's job
    // (globbing, dep discovery, `as` aliasing) is done. leviathan just reads
    // and gathers (§3.3 contract rule 1); no manifest concepts involved.
    std::vector<GatherEntry> gather;
    gather.reserve(plan.sources.size());
    for (const PlanSource& s : plan.sources)
        gather.push_back({s.path, s.origin, s.moduleId});

    std::vector<std::string> texts;
    texts.reserve(gather.size());
    for (const GatherEntry& e : gather) {
        bool fok = false;
        std::string text = readWholeFile(e.path, fok);
        if (!fok) {
            sink.error({0, 0}, "cannot read source '" + e.path +
                "' (the build plan claims it exists)");
            return proj;
        }
        texts.push_back(std::move(text));
    }

    // No `aliases`: trident already materializes any `as` alias as an ordinary
    // src row (§6 — "a dependency is just more source", now more plan rows).
    layoutCombinedBuffer(gather, texts, {}, proj);
    proj.combined.name = plan.out.empty() ? planPath : plan.out;

    for (const PlanEdge& e : plan.edges)
        proj.moduleDeps[e.from].insert(e.to);

    // LA-20 §8: the declared asset table, moduleId -> (rel -> abs). No
    // existence probing — trident guaranteed the files (contract rule 1);
    // a vanished file surfaces as I05 only when `import()` actually reads it.
    for (const PlanAsset& a : plan.assets) {
        proj.assets[a.moduleId][a.rel] = a.path;
        if (!a.hash.empty()) proj.assetHashes[a.path] = a.hash;
    }

    // Entry point: explicit from the plan — no extension sniffing (§3.3 rule 2).
    proj.entryName = plan.entryTarget;
    switch (plan.entryKind) {
        case PlanEntryKind::Script:
            proj.entryMode = EntryMode::Script;
            break;
        case PlanEntryKind::File: {
            proj.entryMode = EntryMode::File;
            for (size_t i = 0; i < proj.files.size(); ++i)
                if (proj.files[i].path == plan.entryTarget) { proj.entryFileIndex = (int)i; break; }
            if (proj.entryFileIndex < 0)
                sink.error({0, 0}, "entry file '" + plan.entryTarget +
                    "' is not among the plan's src rows");
            break;
        }
        case PlanEntryKind::Function: {
            proj.entryMode = EntryMode::Function;
            bool valid = !plan.entryTarget.empty();
            for (char c : plan.entryTarget)
                if (!(std::isalnum((unsigned char)c) || c == '_' || c == ':')) valid = false;
            if (!valid)
                sink.error({0, 0}, "entry '" + plan.entryTarget +
                    "' is not a valid function name (a name or NS::name)");
            else {
                proj.entryCallOffset = (uint32_t)proj.combined.text.size();
                proj.combined.text += plan.entryTarget + "();\n";
            }
            break;
        }
    }

    proj.ok = !sink.hasErrors();
    return proj;
}

void validateEntry(const LoadedProject& proj, const Program& program,
                   DiagnosticSink& sink) {
    auto isExecutable = [](StmtKind k) {
        switch (k) {
            case StmtKind::ExprStmt: case StmtKind::If: case StmtKind::While:
            case StmtKind::DoWhile:
            case StmtKind::For: case StmtKind::ForIn: case StmtKind::Try:
            case StmtKind::Throw: case StmtKind::Return: case StmtKind::Block:
                return true;
            default: return false;   // declarations, globals, uses, empty
        }
    };

    for (const StmtPtr& item : program.items) {
        if (!isExecutable(item->kind)) continue;
        uint32_t off = item->span.offset;
        int fi = -1;
        for (size_t i = 0; i < proj.files.size(); ++i)
            if (off >= proj.files[i].offset &&
                off < proj.files[i].offset + proj.files[i].length) { fi = (int)i; break; }
        if (fi < 0) continue;   // the synthesized entry call (beyond all files)

        // A dependency must be declaration-only: no library code runs at
        // top level in the consuming program. (Enforced in every entry mode.)
        if (!proj.files[fi].origin.empty()) {
            sink.error(item->span, "dependency '" + proj.files[fi].origin +
                "' has top-level code; a dependency must be declaration-only "
                "(namespaces, classes, functions)");
            continue;
        }

        // Project files: the entry rule (script mode allows anything).
        if (proj.entryMode == EntryMode::Script) continue;
        if (proj.entryMode == EntryMode::File && fi == proj.entryFileIndex) continue;
        if (proj.entryMode == EntryMode::Function)
            sink.error(item->span, "top-level code alongside entry function '" +
                proj.entryName + "' — move it into '" + proj.entryName + "'");
        else
            sink.error(item->span, "top-level code outside the entry file '" +
                proj.entryName + "' — move it into a function");
    }
}

namespace {

// The canonical module (moduleId) that owns a combined-buffer offset; nullptr if
// the offset predates every file (a synthesized node).
const std::string* moduleAt(const LoadedProject& proj, uint32_t off) {
    for (const ProjectFile& f : proj.files)
        if (off >= f.offset && off < f.offset + f.length) return &f.moduleId;
    return nullptr;
}

// Joins path[0, end) with "::" ("" if end == 0).
std::string joinPath(const std::vector<std::string_view>& path, size_t end) {
    std::string s;
    for (size_t i = 0; i < end; ++i) {
        if (i) s += "::";
        s += std::string(path[i]);
    }
    return s;
}

// Walk items (recursively through namespaces), invoking `onNs(fullPath, span)`
// for each namespace declaration and `onUses(fullPath, span)` for each `uses`
// (and, for a selective `use A::B::name`, the namespace it draws from — the
// path minus the imported name itself, imports.md §5).
template <class NsFn, class UsesFn>
void walkImports(const std::vector<StmtPtr>& items, const std::string& prefix,
                 NsFn&& onNs, UsesFn&& onUses) {
    for (const StmtPtr& item : items) {
        const Stmt* s = item.get();
        if (s->kind == StmtKind::Namespace) {
            std::string full = prefix.empty() ? std::string(s->name)
                                              : prefix + "::" + std::string(s->name);
            onNs(full, s->span);
            walkImports(s->body, full, onNs, onUses);
        } else if (s->kind == StmtKind::UsesImport) {
            std::string path = joinPath(s->generics, s->generics.size());
            if (!path.empty()) onUses(path, s->span);
        } else if (s->kind == StmtKind::Use && s->generics.size() > 1) {
            onUses(joinPath(s->generics, s->generics.size() - 1), s->span);
        }
    }
}

}  // namespace

void checkPhantomDeps(const LoadedProject& proj, const Program& program,
                      DiagnosticSink& sink) {
    if (proj.files.empty()) return;   // single-file / no project

    // providedBy: namespace -> the set of modules that declare into it.
    std::map<std::string, std::set<std::string>> providedBy;
    walkImports(program.items, "",
        [&](const std::string& ns, SourceSpan sp) {
            if (const std::string* m = moduleAt(proj, sp.offset))
                providedBy[ns].insert(*m);
        },
        [](const std::string&, SourceSpan) {});

    // Each `uses NS` is legal only if NS is provided by the importing module,
    // one of its directly-declared deps, or std.
    walkImports(program.items, "",
        [](const std::string&, SourceSpan) {},
        [&](const std::string& ns, SourceSpan sp) {
            if (ns == "std") return;
            const std::string* m = moduleAt(proj, sp.offset);
            if (!m) return;
            auto pit = providedBy.find(ns);
            if (pit == providedBy.end()) return;   // unknown ns: the resolver reports it

            std::set<std::string> allowed = {*m};
            auto dit = proj.moduleDeps.find(*m);
            if (dit != proj.moduleDeps.end())
                allowed.insert(dit->second.begin(), dit->second.end());

            for (const std::string& provider : pit->second)
                if (allowed.count(provider)) return;   // provided by an allowed module

            // Phantom: NS exists in the gather but only via an undeclared module.
            std::string who = "this project";
            if (!m->empty())
                for (const ProjectFile& f : proj.files)
                    if (f.moduleId == *m) { who = "dependency '" + f.origin + "'"; break; }
            sink.error(sp, "namespace '" + ns + "' comes from an indirect "
                "dependency not declared by " + who + "; declare it as a direct "
                "dependency to use it");
        });
}

namespace {

const char* severityWord(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
    }
    return "?";
}

// Attribute a combined-buffer offset to its owning source and that file's own
// 1-based line/col. Returns false for offsets outside every file (e.g. the
// synthesized {0,0} span of a manifest-level diagnostic).
bool locate(const std::vector<ProjectFile>& files, const std::string& text,
            uint32_t offset, const ProjectFile*& owner, uint32_t& line, uint32_t& col) {
    for (const ProjectFile& f : files) {
        if (offset >= f.offset && offset < f.offset + f.length) {
            // Line/col within the file's own slice.
            uint32_t l = 1, c = 1;
            for (uint32_t i = f.offset; i < offset && i < text.size(); ++i) {
                if (text[i] == '\n') { ++l; c = 1; } else ++c;
            }
            owner = &f; line = l; col = c;
            return true;
        }
    }
    return false;
}

}  // namespace

void renderProjectDiagnostics(const SourceFile& combined,
                              const std::vector<ProjectFile>& files,
                              const DiagnosticSink& sink) {
    const std::string& text = combined.text;
    for (const Diagnostic& d : sink.all()) {
        const ProjectFile* owner = nullptr;
        uint32_t line = 0, col = 0;
        if (!locate(files, text, d.span.offset, owner, line, col)) {
            std::fprintf(stderr, "%s: %s: %s\n", combined.name.c_str(),
                         severityWord(d.severity), d.message.c_str());
            continue;
        }
        std::fprintf(stderr, "%s:%u:%u: %s: %s\n", owner->path.c_str(), line, col,
                     severityWord(d.severity), d.message.c_str());
        // Print the offending line (from the combined buffer; contents are the
        // file's) and a caret under the span.
        uint32_t start = d.span.offset;
        while (start > 0 && text[start - 1] != '\n') --start;
        uint32_t end = d.span.offset;
        while (end < text.size() && text[end] != '\n') ++end;
        std::fprintf(stderr, "  %s\n", text.substr(start, end - start).c_str());
        std::string caret(col - 1 + 2, ' ');
        uint32_t width = d.span.length ? d.span.length : 1;
        caret.push_back('^');
        for (uint32_t i = 1; i < width; ++i) caret.push_back('~');
        std::fprintf(stderr, "%s\n", caret.c_str());
    }
}

// -----------------------------------------------------------------------------
// P-4: file -> imports provenance
// -----------------------------------------------------------------------------

namespace {

// Which file owns a combined-buffer offset (-1 if outside every file).
int fileIndexOf(const std::vector<ProjectFile>& files, uint32_t offset) {
    for (size_t i = 0; i < files.size(); ++i)
        if (offset >= files[i].offset && offset < files[i].offset + files[i].length)
            return (int)i;
    return -1;
}

const char* kRoot = "<root>";

void walkProvenance(const std::vector<StmtPtr>& items, const std::string& nsPrefix,
                    const std::vector<ProjectFile>& files,
                    std::vector<FileImports>& out) {
    for (const StmtPtr& item : items) {
        const Stmt* s = item.get();
        int fi = fileIndexOf(files, s->span.offset);
        if (fi < 0) continue;
        switch (s->kind) {
            case StmtKind::Namespace: {
                std::string full = nsPrefix.empty()
                    ? std::string(s->name)
                    : nsPrefix + "::" + std::string(s->name);
                out[fi].declaresInto.insert(full);
                walkProvenance(s->body, full, files, out);   // nested ns / uses
                break;
            }
            case StmtKind::UsesImport: {
                std::string path = joinPath(s->generics, s->generics.size());
                if (!path.empty()) out[fi].importsExplicit.insert(path);
                break;
            }
            case StmtKind::Use: {
                if (s->generics.size() > 1)
                    out[fi].importsExplicit.insert(joinPath(s->generics, s->generics.size() - 1));
                std::string full = joinPath(s->generics, s->generics.size());
                if (!full.empty()) {
                    std::string row = full;
                    if (s->name != s->generics.back()) row += " as " + std::string(s->name);
                    out[fi].useNames.insert(row);
                }
                break;
            }
            case StmtKind::Class:
            case StmtKind::Member:
            case StmtKind::Bind:
            case StmtKind::Var:
                out[fi].declaresInto.insert(nsPrefix.empty() ? kRoot : nsPrefix);
                break;
            default:
                break;   // executable statements declare nothing
        }
    }
}

}  // namespace

std::vector<FileImports> computeFileImports(const std::vector<ProjectFile>& files,
                                            const Program& program) {
    std::vector<FileImports> out(files.size());
    for (size_t i = 0; i < files.size(); ++i) out[i].path = files[i].path;

    walkProvenance(program.items, "", files, out);

    // effective = declaresInto ∪ importsExplicit ∪ {std}  (std is implicitly used)
    for (FileImports& fi : out) {
        fi.effective = fi.declaresInto;
        fi.effective.insert(fi.importsExplicit.begin(), fi.importsExplicit.end());
        fi.effective.insert("std");
    }
    return out;
}

std::string renderFileImports(const std::vector<FileImports>& imports) {
    auto join = [](const std::set<std::string>& s) -> std::string {
        if (s.empty()) return "(none)";
        std::string r;
        for (const std::string& x : s) { if (!r.empty()) r += ", "; r += x; }
        return r;
    };
    std::string out = "[imports] file -> imports provenance "
                      "(declares / uses / effective visible namespaces)\n";
    for (const FileImports& fi : imports) {
        out += "[imports] " + fi.path + "\n";
        out += "[imports]   declares: " + join(fi.declaresInto) + "\n";
        out += "[imports]   uses:     " + join(fi.importsExplicit) + "\n";
        if (!fi.useNames.empty())
            out += "[imports]   use:      " + join(fi.useNames) + "\n";
        out += "[imports]   visible:  " + join(fi.effective) + "\n";
    }
    return out;
}

// -----------------------------------------------------------------------------
// P-3: the `uses` include graph + deterministic build order
// -----------------------------------------------------------------------------

namespace {

// Tarjan's SCC, iterated over start nodes in ascending index and neighbours in
// ascending index, so the emitted component order is deterministic. Tarjan
// finishes each SCC in reverse-topological order of the condensation: for an
// edge A -> B (A depends on B), B's component is emitted before A's — exactly a
// dependencies-first build order. Recursion depth is bounded by the file count.
struct Tarjan {
    const std::vector<std::set<int>>& deps;
    std::vector<int> index, low, stack;
    std::vector<char> onStack;
    std::vector<std::vector<int>> comps;   // in emit (reverse-topo) order
    int counter = 0;

    explicit Tarjan(const std::vector<std::set<int>>& d)
        : deps(d), index(d.size(), -1), low(d.size(), 0), onStack(d.size(), 0) {}

    void strongconnect(int v) {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        onStack[v] = 1;
        for (int w : deps[v]) {              // std::set iterates ascending
            if (index[w] < 0) {
                strongconnect(w);
                low[v] = std::min(low[v], low[w]);
            } else if (onStack[w]) {
                low[v] = std::min(low[v], index[w]);
            }
        }
        if (low[v] == index[v]) {
            std::vector<int> comp;
            for (;;) {
                int w = stack.back();
                stack.pop_back();
                onStack[w] = 0;
                comp.push_back(w);
                if (w == v) break;
            }
            std::sort(comp.begin(), comp.end());   // stable membership order
            comps.push_back(std::move(comp));
        }
    }

    void run() {
        for (int v = 0; v < (int)deps.size(); ++v)
            if (index[v] < 0) strongconnect(v);
    }
};

}  // namespace

UsesGraph buildUsesGraph(const std::vector<FileImports>& imports) {
    UsesGraph g;
    int n = (int)imports.size();
    g.paths.resize(n);
    g.fileDeps.assign(n, {});

    // Namespace -> files. declaresInto/importsExplicit are std::set (ascending),
    // and we walk files 0..n-1, so every index list is ascending by construction.
    for (int i = 0; i < n; ++i) {
        g.paths[i] = imports[i].path;
        for (const std::string& ns : imports[i].declaresInto)
            if (ns != kRoot) g.declaredBy[ns].push_back(i);
        for (const std::string& ns : imports[i].importsExplicit)
            g.importedBy[ns].push_back(i);
    }

    // File i depends on file j when i `uses` a namespace j declares into. A
    // namespace opened only by i (or only in the prelude, e.g. std) yields no
    // edge; a self-open is not a dependency (i != j).
    for (int i = 0; i < n; ++i) {
        for (const std::string& ns : imports[i].importsExplicit) {
            auto it = g.declaredBy.find(ns);
            if (it == g.declaredBy.end()) continue;
            for (int j : it->second)
                if (j != i) g.fileDeps[i].insert(j);
        }
    }

    // Deterministic dependencies-first order via SCC condensation.
    Tarjan t(g.fileDeps);
    t.run();
    for (const std::vector<int>& comp : t.comps) {
        for (int v : comp) g.order.push_back(v);
        if (comp.size() > 1) g.cycles.push_back(comp);
    }
    return g;
}

std::string renderUsesGraph(const UsesGraph& g) {
    auto names = [&](const std::vector<int>& idx) -> std::string {
        if (idx.empty()) return "(none)";
        std::string r;
        for (int i : idx) { if (!r.empty()) r += ", "; r += g.paths[i]; }
        return r;
    };
    auto depNames = [&](const std::set<int>& idx) -> std::string {
        if (idx.empty()) return "(none)";
        std::string r;
        for (int i : idx) { if (!r.empty()) r += ", "; r += g.paths[i]; }
        return r;
    };

    std::string out = "[graph] uses include graph "
                      "(file -> file dependencies via `uses`) + build order\n";
    out += "[graph] namespaces (declared-in / imported-by):\n";
    // Union of every namespace that is declared or imported, sorted (std::map).
    std::set<std::string> all;
    for (const auto& [ns, _] : g.declaredBy) all.insert(ns);
    for (const auto& [ns, _] : g.importedBy) all.insert(ns);
    if (all.empty()) out += "[graph]   (none)\n";
    for (const std::string& ns : all) {
        auto d = g.declaredBy.find(ns);
        auto u = g.importedBy.find(ns);
        static const std::vector<int> empty;
        out += "[graph]   " + ns + "\n";
        out += "[graph]     declared-in: " +
               names(d == g.declaredBy.end() ? empty : d->second) + "\n";
        out += "[graph]     imported-by: " +
               names(u == g.importedBy.end() ? empty : u->second) + "\n";
    }
    out += "[graph] file dependencies (A -> B: A `uses` a namespace B declares):\n";
    bool anyDep = false;
    for (size_t i = 0; i < g.fileDeps.size(); ++i) {
        if (g.fileDeps[i].empty()) continue;
        anyDep = true;
        out += "[graph]   " + g.paths[i] + " -> " + depNames(g.fileDeps[i]) + "\n";
    }
    if (!anyDep) out += "[graph]   (none)\n";
    out += "[graph] cycles: ";
    if (g.cycles.empty()) {
        out += "(none)\n";
    } else {
        out += "\n";
        for (const std::vector<int>& c : g.cycles)
            out += "[graph]   { " + names(c) + " }\n";
    }
    out += "[graph] build order: " + names(g.order) + "\n";
    return out;
}

// -----------------------------------------------------------------------------
// Discoverability queries: --namespaces (the symbol index) and --why (name
// provenance). Both walk the same AST + offset map as the P-3/P-4 queries above.
// -----------------------------------------------------------------------------

namespace {

// The name a namespace-level declaration introduces, or "" if it introduces no
// looked-up name (e.g. an anonymous DI `bind expr;`). Mirrors the decl kinds
// walkProvenance attributes to a namespace (Class/Member/Bind/Var/Enum).
std::string memberName(const Stmt* s) {
    switch (s->kind) {
        case StmtKind::Class:
        case StmtKind::Enum:
        case StmtKind::Var:
        case StmtKind::Member:   // free function / typed member: `name` is its label
        case StmtKind::Bind:     // DI binding: named by its bound type when present
            return std::string(s->name);
        default:
            return std::string();   // executable statements / imports declare nothing
    }
}

// Per namespace path ("<root>" = top level): the files that open it and the
// members declared directly inside it (member name -> the files declaring it).
struct NsCollect {
    std::set<int> openedIn;
    std::map<std::string, std::set<int>> members;
};

void walkDecls(const std::vector<StmtPtr>& items, const std::string& nsPrefix,
               const std::vector<ProjectFile>& files,
               std::map<std::string, NsCollect>& out) {
    for (const StmtPtr& item : items) {
        const Stmt* s = item.get();
        int fi = fileIndexOf(files, s->span.offset);
        if (fi < 0) continue;
        if (s->kind == StmtKind::Namespace) {
            std::string full = nsPrefix.empty()
                ? std::string(s->name)
                : nsPrefix + "::" + std::string(s->name);
            out[full].openedIn.insert(fi);
            walkDecls(s->body, full, files, out);   // nested ns / members
        } else {
            std::string nm = memberName(s);
            if (nm.empty()) continue;
            out[nsPrefix.empty() ? std::string(kRoot) : nsPrefix].members[nm].insert(fi);
        }
    }
}

// True when `full` (an absolute path) is named by `query` — an exact match, or a
// trailing path segment (a basename or path tail). Lets `--why X in main.lev`
// name a file without spelling its whole absolute path.
bool pathMatches(const std::string& full, const std::string& query) {
    if (full == query) return true;
    return full.size() > query.size() &&
           full.compare(full.size() - query.size(), query.size(), query) == 0 &&
           full[full.size() - query.size() - 1] == '/';
}

}  // namespace

std::vector<NamespaceInfo> computeNamespaces(const std::vector<ProjectFile>& files,
                                             const Program& program) {
    std::map<std::string, NsCollect> collected;
    walkDecls(program.items, "", files, collected);

    std::vector<NamespaceInfo> out;
    for (auto& [ns, c] : collected) {   // std::map -> namespaces ascending
        NamespaceInfo info;
        info.name = ns;
        std::set<int> opened = c.openedIn;
        if (ns == kRoot)   // <root> is "opened" wherever a top-level decl lives
            for (auto& [_, fis] : c.members) opened.insert(fis.begin(), fis.end());
        for (int fi : opened) info.openedIn.push_back(files[fi].path);
        for (auto& [nm, _] : c.members) info.members.push_back(nm);   // ascending
        out.push_back(std::move(info));
    }
    return out;
}

std::string renderNamespaces(const std::vector<NamespaceInfo>& nss) {
    auto join = [](const std::vector<std::string>& v) -> std::string {
        if (v.empty()) return "(none)";
        std::string r;
        for (const std::string& x : v) { if (!r.empty()) r += ", "; r += x; }
        return r;
    };
    std::string out = "[namespaces] every namespace, the files that open it, "
                      "and its members\n";
    if (nss.empty()) out += "[namespaces]   (none)\n";
    for (const NamespaceInfo& n : nss) {
        out += "[namespaces] " + n.name + "\n";
        out += "[namespaces]   opened-in: " + join(n.openedIn) + "\n";
        out += "[namespaces]   members:   " + join(n.members) + "\n";
    }
    return out;
}

WhyResult computeWhy(const std::vector<ProjectFile>& files, const Program& program,
                     const std::string& name) {
    std::map<std::string, NsCollect> collected;
    walkDecls(program.items, "", files, collected);
    std::vector<FileImports> imports = computeFileImports(files, program);

    WhyResult why;
    why.name = name;
    for (const ProjectFile& f : files) why.allFiles.push_back(f.path);

    for (auto& [ns, c] : collected) {   // std::map -> candidates sorted by ns
        auto mit = c.members.find(name);
        if (mit == c.members.end()) continue;
        WhyCandidate cand;
        cand.ns = ns;
        for (int fi : mit->second) cand.declaredIn.push_back(files[fi].path);
        // Visible to file i when i declares into ns, imports it, or ns is always
        // on: `<root>` (whole-program global scope, seen everywhere) and `std`
        // (the implicit prelude import). effective already folds in std for every
        // file; `<root>` is special-cased since no file "imports" the top level.
        for (size_t i = 0; i < files.size(); ++i) {
            bool visible = ns == kRoot || ns == "std" ||
                           imports[i].effective.count(ns) > 0;
            if (visible) cand.visibleTo.push_back(files[i].path);
        }
        why.candidates.push_back(std::move(cand));
    }
    return why;
}

std::string renderWhy(const WhyResult& why, const std::string& inFile) {
    auto join = [](const std::vector<std::string>& v) -> std::string {
        if (v.empty()) return "(none)";
        std::string r;
        for (const std::string& x : v) { if (!r.empty()) r += ", "; r += x; }
        return r;
    };
    // A namespace's name as it should read in the "resolves to" line: a top-level
    // name has no qualifier, so `<root>` becomes plain "top-level".
    auto qualified = [&](const std::string& ns) -> std::string {
        return ns == kRoot ? "top-level '" + why.name + "'"
                           : ns + "::" + why.name;
    };

    if (inFile.empty()) {
        std::string out = "[why] name '" + why.name + "' — candidate namespaces\n";
        if (why.candidates.empty()) {
            out += "[why]   no declaration of '" + why.name +
                   "' in any project namespace\n";
            return out;
        }
        for (const WhyCandidate& c : why.candidates) {
            out += "[why]   " + c.ns + "\n";
            out += "[why]     declared-in: " + join(c.declaredIn) + "\n";
            out += "[why]     visible-to:  " + join(c.visibleTo) + "\n";
        }
        size_t n = why.candidates.size();
        out += "[why] " + std::to_string(n) + " candidate" + (n == 1 ? "" : "s") +
               (n <= 1
                    ? ", no ambiguity\n"
                    : " — a file that imports more than one must qualify with `::` "
                      "(or rely on type to disambiguate)\n");
        return out;
    }

    // Narrow to one file: which candidates does its effective import set admit?
    std::string out = "[why] name '" + why.name + "' in " + inFile + "\n";
    bool known = false;
    for (const std::string& p : why.allFiles)
        if (pathMatches(p, inFile) || p == inFile) { known = true; break; }
    if (!known) {
        out += "[why]   no such file '" + inFile + "' in this project\n";
        return out;
    }
    std::vector<std::string> visible, allNs;
    for (const WhyCandidate& c : why.candidates) {
        allNs.push_back(c.ns);
        for (const std::string& p : c.visibleTo)
            if (pathMatches(p, inFile)) { visible.push_back(c.ns); break; }
    }
    if (why.candidates.empty()) {
        out += "[why]   no declaration of '" + why.name + "' in any project namespace\n";
    } else if (visible.empty()) {
        out += "[why]   '" + why.name + "' is declared in " + join(allNs) +
               " but none is visible in " + inFile + " — add a `uses` for one\n";
    } else if (visible.size() == 1) {
        out += "[why]   resolves to " + qualified(visible[0]) +
               " (1 visible candidate, no ambiguity)\n";
    } else {
        out += "[why]   ambiguous — visible via " + join(visible) +
               "; qualify with `::` (or rely on type to disambiguate)\n";
    }
    return out;
}

// -----------------------------------------------------------------------------
// --lint-namespaces: the optional folder≈namespace convention check.
// -----------------------------------------------------------------------------

namespace {

// The directory portion of a path (everything before the last '/'; "" if none).
std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Split a '/'-separated path into its non-empty segments.
std::vector<std::string> splitDir(const std::string& dir) {
    std::vector<std::string> segs;
    std::string cur;
    for (char c : dir) {
        if (c == '/') { if (!cur.empty()) { segs.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) segs.push_back(cur);
    return segs;
}

// Split a namespace "A::B::C" into ["A", "B", "C"].
std::vector<std::string> splitNs(const std::string& ns) {
    std::vector<std::string> segs;
    size_t i = 0;
    while (i <= ns.size()) {
        size_t c = ns.find("::", i);
        if (c == std::string::npos) { segs.push_back(ns.substr(i)); break; }
        segs.push_back(ns.substr(i, c - i));
        i = c + 2;
    }
    return segs;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Is `tail` a case-insensitive segment-wise suffix of `full`? (An empty tail — a
// file at the project root — is a suffix of every namespace, so root files pass.)
bool isSegSuffix(const std::vector<std::string>& full, const std::vector<std::string>& tail) {
    if (tail.size() > full.size()) return false;
    size_t base = full.size() - tail.size();
    for (size_t i = 0; i < tail.size(); ++i)
        if (lower(full[base + i]) != lower(tail[i])) return false;
    return true;
}

}  // namespace

LayoutLint lintNamespaceLayout(const std::vector<ProjectFile>& files,
                               const Program& program) {
    LayoutLint out;

    // Only the root project is ours to lint — a dependency's layout is its own.
    std::vector<int> rootFiles;
    for (int i = 0; i < (int)files.size(); ++i)
        if (files[i].origin.empty()) rootFiles.push_back(i);
    if (rootFiles.empty()) return out;

    // Project root = the common directory prefix of every root file's path,
    // trimmed to a directory boundary. (Single file => its own directory.)
    std::string root = files[rootFiles[0]].path;
    for (int i : rootFiles) {
        const std::string& p = files[i].path;
        size_t k = 0;
        while (k < root.size() && k < p.size() && root[k] == p[k]) ++k;
        root.resize(k);
    }
    if (size_t slash = root.find_last_of('/'); slash != std::string::npos)
        root = root.substr(0, slash);
    else
        root.clear();

    std::vector<FileImports> imports = computeFileImports(files, program);

    for (int i : rootFiles) {
        std::vector<std::string> declared;
        for (const std::string& ns : imports[i].declaresInto)
            if (ns != kRoot) declared.push_back(ns);   // std::set -> sorted
        if (declared.empty()) continue;                // no namespace to place: exempt

        std::string dir = dirOf(files[i].path);
        std::string rel = dir;
        if (!root.empty() && dir.size() >= root.size() &&
            dir.compare(0, root.size(), root) == 0)
            rel = dir.substr(root.size());
        while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
        std::vector<std::string> relSegs = splitDir(rel);

        LayoutFinding f;
        f.path = files[i].path;
        f.relDir = rel;
        f.declaresInto = declared;
        for (const std::string& ns : declared)
            if (isSegSuffix(splitNs(ns), relSegs)) { f.matches = true; break; }
        if (!f.matches) ++out.mismatches;
        out.findings.push_back(std::move(f));
    }
    return out;
}

std::string renderLayoutLint(const LayoutLint& lint) {
    auto join = [](const std::vector<std::string>& v) -> std::string {
        std::string r;
        for (const std::string& x : v) { if (!r.empty()) r += ", "; r += x; }
        return r;
    };
    std::string out = "[lint] folder~namespace convention (opt-in; namespaces are "
                      "path-decoupled by default, §12)\n";
    if (lint.findings.empty()) {
        out += "[lint]   (no namespace-declaring project files to check)\n";
        return out;
    }
    for (const LayoutFinding& f : lint.findings) {
        std::string where = f.relDir.empty() ? "<project root>" : f.relDir;
        if (f.matches)
            out += "[lint]   ok:   " + f.path + "  (" + where + " ~ " +
                   join(f.declaresInto) + ")\n";
        else
            out += "[lint]   WARN: " + f.path + "  declares " + join(f.declaresInto) +
                   " but sits in " + where + " — folder is not a suffix of the namespace\n";
    }
    out += "[lint] " + std::to_string(lint.mismatches) + " mismatch" +
           (lint.mismatches == 1 ? "" : "es") + " of " +
           std::to_string(lint.findings.size()) + " file(s) checked\n";
    return out;
}
