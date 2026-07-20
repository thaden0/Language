#include "AstPrinter.hpp"
#include "Checker.hpp"
#include "Diagnostic.hpp"
#include "Eval.hpp"
#include "IrInterp.hpp"
#include "Lower.hpp"
#include "CGen.hpp"
#include "X64Gen.hpp"
#ifdef HAVE_LLVM
#include "LlvmGen.hpp"
#endif
#include "Ownership.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Resolver.hpp"
#include "PreludeEmbedded.hpp"
#include "Rules.hpp"
#include "Project.hpp"
#include "BuildPlan.hpp"
#include "Source.hpp"
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <sstream>
#include <unistd.h>
#include <climits>

// B-M2 link driver (doc-2 §4): locate a C/C++ compiler driver to shell out to
// for linking, tried in this order. Reused by both --build (compiles CGen's
// C++ output) and --build-native (links an LLVM object against liblvrt.a).
static bool isExecutableFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && ::access(path.c_str(), X_OK) == 0;
}

static std::string findOnPath(const std::string& name) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return "";
    std::string paths = pathEnv;
    size_t start = 0;
    while (start <= paths.size()) {
        size_t colon = paths.find(':', start);
        std::string dir = paths.substr(start, colon == std::string::npos
                                                   ? std::string::npos
                                                   : colon - start);
        if (!dir.empty() && isExecutableFile(dir + "/" + name))
            return dir + "/" + name;
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return "";
}

static std::string probeLinkerDriver(std::string* tried) {
    static const char* const kCandidates[] = {"clang++", "g++", "cc"};
    for (const char* c : kCandidates) {
        if (tried) { if (!tried->empty()) *tried += ", "; *tried += c; }
        if (!findOnPath(c).empty()) return c;
    }
    return "";
}

// B-M5: the LLVM target triple and the distro/MinGW cross-toolchain's binary
// prefix are the same string for aarch64-linux-gnu, but NOT for Windows — the
// MinGW-w64 packages install as `x86_64-w64-mingw32-{gcc,g++,ar}`, never
// `x86_64-pc-windows-gnu-*` (no such package name exists). Same mapping as
// runtime/build-triple.sh's `gnu_prefix`; keep the two in sync.
static std::string gnuToolchainPrefix(const std::string& triple) {
    if (triple == "x86_64-pc-windows-gnu" ||
        (triple.rfind("x86_64-", 0) == 0 && triple.find("mingw32") != std::string::npos))
        return "x86_64-w64-mingw32";
    return triple;
}

static bool isWindowsTriple(const std::string& triple) {
    return triple.find("windows") != std::string::npos ||
           triple.find("mingw") != std::string::npos;
}

// Track W hard-02 (techdesign-02-backend-column.md §4): the wasm link lane's
// key — the triple starts `wasm32`. Wasm never goes through the native/cross
// C++-driver probe below; it links via wasm-ld directly.
static bool isWasmTriple(const std::string& triple) {
    return triple.rfind("wasm32", 0) == 0;
}

// wasm-ld probe: plain name first, then the distro-versioned one (Debian/
// Ubuntu ship `wasm-ld-18` in the `lld-18` package without the bare alias).
// Returns "" if none found; `tried` lists what was probed, in
// probeLinkerDriver's human-readable form, for the diagnostic.
static std::string probeWasmLinker(std::string* tried) {
    static const char* const kCandidates[] = {"wasm-ld", "wasm-ld-18"};
    for (const char* c : kCandidates) {
        if (tried) { if (!tried->empty()) *tried += ", "; *tried += c; }
        if (!findOnPath(c).empty()) return c;
    }
    return "";
}

// B-M4 (doc-2 §6 item 3): cross link probe. Prefer `clang++ -target <triple>`
// (one clang++ retargets to any triple), then a distro cross-gcc named
// `<prefix>-g++` / `<prefix>-gcc` (prefix == triple except on Windows, see
// gnuToolchainPrefix). Returns the full driver invocation prefix (may contain
// a `-target` flag) or "" if none found; `tried` lists what was probed, in
// `probeLinkerDriver`'s human-readable form, for the diagnostic.
static std::string probeCrossLinkerDriver(const std::string& triple,
                                          std::string* tried) {
    auto note = [&](const std::string& s) {
        if (tried) { if (!tried->empty()) *tried += ", "; *tried += s; }
    };
    std::string clangInvoke = "clang++ -target " + triple;
    note(clangInvoke);
    if (!findOnPath("clang++").empty()) return clangInvoke;
    std::string prefix = gnuToolchainPrefix(triple);
    for (const std::string& suffix : {std::string("-g++"), std::string("-gcc")}) {
        std::string cross = prefix + suffix;
        note(cross);
        if (!findOnPath(cross).empty()) return cross;
    }
    return "";
}

// B-M2 (doc-2 §4 item 4): the `leviathan` executable's own directory, via
// /proc/self/exe rather than a baked-in build-dir path so a relocated install
// still finds its siblings. Linux-only (driver code, not runtime) — ported
// alongside the rest of the driver at B-M5. Returns "" on failure.
static std::string exeDir() {
    char exePath[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) return "";
    exePath[len] = '\0';
    std::string dir(exePath);
    size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) return "";
    return dir.substr(0, slash);
}

// Host runtime archive: liblvrt.a next to the `leviathan` executable.
static std::string findRuntimeArchive() {
    std::string dir = exeDir();
    if (dir.empty()) return "";
    std::string candidate = dir + "/liblvrt.a";
    struct stat st;
    if (::stat(candidate.c_str(), &st) == 0) return candidate;
    return "";
}

// B-M4 (doc-2 §6 item 2): per-triple runtime archive. `runtime/build-triple.sh
// <triple>` cross-compiles the runtime into `runtime/<triple>/liblvrt.a`; the
// driver resolves it "by triple". Two layouts are honored so both an installed
// tree (archives colocated next to the host one) and the in-source dev build
// (build-triple.sh's default `runtime/<triple>` output, reached from build/
// via ../runtime) work without configuration. `--runtime` overrides entirely.
static std::string findRuntimeArchiveForTriple(const std::string& triple) {
    std::string dir = exeDir();
    if (dir.empty()) return "";
    std::string candidates[] = {
        dir + "/" + triple + "/liblvrt.a",             // installed / colocated
        dir + "/../runtime/" + triple + "/liblvrt.a",  // in-source dev build
    };
    struct stat st;
    for (const std::string& c : candidates)
        if (::stat(c.c_str(), &st) == 0) return c;
    return "";
}

// Shipped prelude (techdesign-prelude-ship-as-files-opus.md §4.1). The eight
// prelude/*.lev segments ship as files with a build-generated embedded
// fallback; these resolve which directory (if any) parsePrelude() reads from.

// R7: a prelude dir must contain ALL segments, target-independent — selection
// happens at parse time, not ship time. Returns the first missing path or "".
static std::string preludeDirMissing(const std::string& dir) {
    struct stat st;
    for (unsigned long i = 0; i < kPreludeSegmentCount; ++i) {
        std::string f = dir + "/" + kPreludeSegments[i].name + ".lev";
        if (::stat(f.c_str(), &st) != 0) return f;
    }
    return "";
}

// Tiers (design R2): --prelude flag -> LV_PRELUDE_DIR -> exeDir()/prelude ->
// exeDir()/../prelude (in-source dev build, mirrors ../runtime/<triple>) ->
// "" meaning the generated embedded fallback. Explicit overrides and existing-
// but-incomplete dirs are fatal (R7): never silently ignore an override, never
// mask a corrupt dir by falling through.
static std::string findPreludeDir(const char* cliOverride) {
    auto fatalIfBad = [](const std::string& dir, const char* how) {
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            std::fprintf(stderr, "leviathan: prelude directory '%s' (%s) "
                         "does not exist\n", dir.c_str(), how);
            std::exit(1);
        }
        std::string missing = preludeDirMissing(dir);
        if (!missing.empty()) {
            std::fprintf(stderr, "leviathan: prelude directory '%s' (%s) is "
                         "missing '%s'\n", dir.c_str(), how, missing.c_str());
            std::exit(1);
        }
    };
    if (cliOverride) { fatalIfBad(cliOverride, "--prelude"); return cliOverride; }
    if (const char* env = std::getenv("LV_PRELUDE_DIR")) {
        fatalIfBad(env, "LV_PRELUDE_DIR"); return env;
    }
    std::string dir = exeDir();
    if (!dir.empty()) {
        for (const std::string& cand : { dir + "/prelude", dir + "/../prelude" }) {
            struct stat st;
            if (::stat(cand.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                std::string missing = preludeDirMissing(cand);
                if (!missing.empty()) {
                    std::fprintf(stderr, "leviathan: prelude directory '%s' is "
                                 "missing '%s'\n", cand.c_str(), missing.c_str());
                    std::exit(1);
                }
                return cand;
            }
        }
    }
    return "";  // embedded fallback (R1) — silent, correct by construction
}

static bool readFile(const char* path, SourceFile& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out.name = path;
    out.text = ss.str();
    return true;
}

// LA-20 §8: `--assets` introspection — one line per consumed asset (the
// debugging window for I03/I05), in the same `[mode] ...` prefixed style as
// --imports/--graph/--namespaces. Plan mode joins each asset's hash from the
// plan's own table (keyed by absolute path, Project.hpp); single-file mode
// has no hash to join (hashing is trident's, contract rule 1).
static std::string renderImportedAssets(const std::vector<ImportedAsset>& assets,
                                        const std::map<std::string, std::string>& hashes) {
    if (assets.empty()) return "[assets] no assets imported\n";
    std::string out = "[assets] consumed assets (path, bytes, hash if known, owning module)\n";
    for (const ImportedAsset& a : assets) {
        out += "[assets] " + a.rel + "  " + std::to_string(a.bytes) + " B";
        auto it = hashes.find(a.abs);
        if (it != hashes.end()) out += "  " + it->second;
        out += "  (module \"" + a.moduleId + "\")\n";
    }
    return out;
}

static void dumpTokens(const SourceFile& file, const std::vector<Token>& tokens) {
    for (const Token& t : tokens) {
        LineCol lc = lineColAt(file.text, t.span.offset);
        if (t.kind == TokenKind::End) {
            std::printf("%4u:%-3u  %s\n", lc.line, lc.col, tokenKindName(t.kind));
        } else {
            std::printf("%4u:%-3u  %-14s  %.*s\n", lc.line, lc.col,
                        tokenKindName(t.kind),
                        (int)t.text.size(), t.text.data());
        }
    }
}

int main(int argc, char** argv) {
    // techdesign-toolchain.md §5.2 B-M2: `trident --version` reports both its
    // own version and leviathan's.
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("leviathan 0.1 (techdesign-toolchain.md Phase 1)\n");
        return 0;
    }
    const char* path = nullptr;
    enum { Full, Tokens, Ast, Resolve, Run, Ir, Own, IrVerify, MemVerify, EmitCpp,
           EmitLlvm, NativeObj, Build, BuildNative, EmitElf, Imports, Graph, Expand,
           ExpandAst, Rules, Namespaces, Why, LintNs, Assets, Specializations } mode = Full;
    const char* outPath = nullptr;
    const char* planPath = nullptr;    // --plan <file>: trident's build-plan (§3.3)
    const char* whyName = nullptr;     // --why <name>: the name to explain
    const char* whyInFile = "";        // --why <name> in <file>: narrow to a file
    const char* runtimePathOverride = nullptr;
    const char* preludeDirOverride = nullptr;  // --prelude <dir>: ship-as-files override (R2)
    const char* targetTriple = "";     // --target <triple>: cross emission (B-M4)
    int optLevel = 2;                  // --opt-level 0|2 (§9 A-M6 H-12): default O2, O0 for debug
    bool noRules = false;
    // techdesign-columnar-arrays.md C-M5 flip (2026-07-12): columnar Array<struct>
    // storage is now the DEFAULT for eligible scalar structs; `--no-columnar` is the
    // escape hatch back to row-major dense (retained one cycle). `--columnar` stays
    // as an explicit no-op-on for compatibility. Value semantics make the layout
    // unobservable, so the flip changes only physical layout, never program output.
    bool columnar = true;
    long long comptimeBudget = 0;      // 0 = ComptimeOptions default
    int reentrantBudget = 0;           // 0 = ComptimeOptions default (§4, Phase 4)
    std::vector<std::string> programArgs;   // the `--` tail: the program's own argv (designs/argv.md §5.2)
    for (int i = 1; i < argc; ++i) {
        // `--` ends leviathan's flag parsing; everything after is the running
        // program's argv, handed to it verbatim via env::args() (the shell
        // already did the quoting). Only the interpreters consult this; compiled
        // binaries get argv from the OS.
        if (std::strcmp(argv[i], "--") == 0) {
            for (int j = i + 1; j < argc; ++j) programArgs.push_back(argv[j]);
            break;
        }
        if (std::strcmp(argv[i], "--plan") == 0 && i + 1 < argc) {
            planPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--assets") == 0) mode = Assets;
        else if (std::strcmp(argv[i], "--imports") == 0) mode = Imports;
        else if (std::strcmp(argv[i], "--graph") == 0) mode = Graph;
        else if (std::strcmp(argv[i], "--namespaces") == 0) mode = Namespaces;
        else if (std::strcmp(argv[i], "--specializations") == 0) mode = Specializations;
        else if (std::strcmp(argv[i], "--lint-namespaces") == 0) mode = LintNs;
        else if (std::strcmp(argv[i], "--why") == 0 && i + 1 < argc) {
            // --why <name> [in <file>]: the name, then an optional `in <file>`
            // qualifier. Both are consumed here so they never fall through to the
            // source-path argument (proposal §4.4 / §6.3).
            mode = Why;
            whyName = argv[++i];
            if (i + 2 < argc && std::strcmp(argv[i + 1], "in") == 0) {
                whyInFile = argv[i + 2];
                i += 2;
            }
        }
        // Phase 4 §6 demux: --expand is now the source-shaped re-emit (a
        // verifiable artifact — the output recompiles); --ast-after-rules keeps
        // the structural AST dump for eyeballing node shape.
        else if (std::strcmp(argv[i], "--expand") == 0) mode = Expand;
        else if (std::strcmp(argv[i], "--ast-after-rules") == 0) mode = ExpandAst;
        else if (std::strcmp(argv[i], "--rules") == 0) mode = Rules;
        else if (std::strcmp(argv[i], "--no-rules") == 0) noRules = true;
        else if (std::strcmp(argv[i], "--columnar") == 0) columnar = true;
        else if (std::strcmp(argv[i], "--no-columnar") == 0) columnar = false;
        else if (std::strcmp(argv[i], "--comptime-budget") == 0 && i + 1 < argc)
            comptimeBudget = std::atoll(argv[++i]);
        else if (std::strcmp(argv[i], "--reentrant-budget") == 0 && i + 1 < argc)
            reentrantBudget = std::atoi(argv[++i]);   // §4 M34 round cap override
        else if (std::strcmp(argv[i], "--tokens") == 0) mode = Tokens;
        else if (std::strcmp(argv[i], "--ast") == 0) mode = Ast;
        else if (std::strcmp(argv[i], "--resolve") == 0) mode = Resolve;
        else if (std::strcmp(argv[i], "--run") == 0) mode = Run;
        else if (std::strcmp(argv[i], "--ir") == 0) mode = Ir;
        else if (std::strcmp(argv[i], "--ownership") == 0) mode = Own;
        else if (std::strcmp(argv[i], "--ir-verify") == 0) mode = IrVerify;
        else if (std::strcmp(argv[i], "--mem-verify") == 0) mode = MemVerify;
        else if (std::strcmp(argv[i], "--emit-cpp") == 0) mode = EmitCpp;
        else if (std::strcmp(argv[i], "--emit-llvm") == 0) mode = EmitLlvm;
        else if (std::strcmp(argv[i], "--native-obj") == 0 && i + 1 < argc) {
            mode = NativeObj;
            outPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--build") == 0 && i + 1 < argc) {
            mode = Build;
            outPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--build-native") == 0 && i + 1 < argc) {
            mode = BuildNative;
            outPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
            runtimePathOverride = argv[++i];
        }
        else if (std::strcmp(argv[i], "--prelude") == 0 && i + 1 < argc) {
            preludeDirOverride = argv[++i];
        }
        else if (std::strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
            targetTriple = argv[++i];
        }
        else if (std::strcmp(argv[i], "--opt-level") == 0 && i + 1 < argc) {
            optLevel = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--emit-elf") == 0 && i + 1 < argc) {
            mode = EmitElf;
            outPath = argv[++i];
        }
        else path = argv[i];
    }
    // techdesign-columnar C-M5: the frozen x86-64/ELF backend has NO columnar lane
    // (X64Gen can't lower ColGet). ELF is not a project target, so force row-major
    // regardless of the (now columnar-on) default — never gate ELF on a columnar op.
    if (mode == EmitElf) columnar = false;
    if (!path && !planPath) {
        std::fprintf(stderr,
            "usage: %s [--run|--ir|--build <out>|--build-native <out>|--emit-cpp|"
            "--emit-llvm|--native-obj <out>|--runtime <path>|--prelude <dir>|--target <triple>|"
            "--opt-level <0|2>|"
            "--ownership|--ir-verify|--ast|--resolve|--tokens|--imports|--graph|"
            "--namespaces|--specializations|--why <name> [in <file>]|--lint-namespaces|--assets] "
            "(<source-file> | --plan <build-plan>) [-- <program args>]\n", argv[0]);
        return 2;
    }

    DiagnosticSink sink;

    // A project is the two-source prelude gather generalized to N: read
    // trident's already-resolved build plan (techdesign-toolchain.md §3.3),
    // concatenate every source into one compilation buffer, and hand that
    // buffer to the ordinary single-file pipeline (§12). leviathan never
    // parses a manifest itself — trident owns that exclusively (§3.2).
    bool usingProject = planPath != nullptr;
    SourceFile file;
    LoadedProject project;
    if (planPath) {
        project = loadProjectFromPlan(planPath, sink);
        if (!project.ok) {
            sink.render(project.manifestFile);
            std::fprintf(stderr, "\n%zu error(s)\n", sink.errorCount());
            return 1;
        }
        file = std::move(project.combined);
    } else if (!readFile(path, file)) {
        std::fprintf(stderr, "error: cannot read '%s'\n", path);
        return 2;
    }

    // Stash the running program's argv for the interpreters' env::args()
    // (designs/argv.md §5.2). argv[0] is the program name: the source basename
    // (minus .lev) for a single file, else the project's combined-buffer name.
    // Compiled backends (--build/--build-native/--emit-cpp) ignore this and take
    // argv from the OS when the produced binary is run.
    {
        std::string progName = "program";
        const std::string& src = path ? std::string(path) : file.name;
        if (!src.empty()) {
            size_t slash = src.find_last_of('/');
            progName = slash == std::string::npos ? src : src.substr(slash + 1);
            if (progName.size() > 4 &&
                progName.compare(progName.size() - 4, 4, ".lev") == 0)
                progName = progName.substr(0, progName.size() - 4);
        }
        std::vector<std::string> fullArgs;
        fullArgs.push_back(progName);
        for (const std::string& a : programArgs) fullArgs.push_back(a);
        setProgramArgs(std::move(fullArgs));
    }

    // Process exit status the interpreters (--run/--ir) thread out of the engine
    // (designs/exit-codes.md §4): env.exit / env.setExitCode's code, or 1 for an
    // uncaught throw. Returned at the end unless a compile error preempts it.
    int programExitCode = 0;

    Lexer lexer(file, sink);
    std::vector<Token> tokens = lexer.tokenize();

    if (mode == Tokens) {
        dumpTokens(file, tokens);
    } else {
        Parser parser(std::move(tokens), file, sink);
        Program program = parser.parseProgram();
        if (mode == Ast) {
            std::printf("%s", printProgram(program).c_str());
        } else if (mode == Imports) {
            // P-4: dump the file -> imports provenance map. For a lone source
            // file (project-of-one) synthesize a single-file offset map.
            // This runs on the PRE-fold tree (before the rule stage), so a
            // `uses` a comptime-if would splice in (§9) is not reflected here
            // — the engine recomputes its own, post-fold map internally.
            std::vector<ProjectFile> files = project.files;
            if (files.empty())
                files.push_back({file.name, 0, (uint32_t)file.text.size(), "", ""});
            std::printf("%s", renderFileImports(computeFileImports(files, program)).c_str());
        } else if (mode == Graph) {
            // P-3: dump the `uses` include graph + build order, derived from the
            // P-4 provenance. Project-of-one synthesizes a single-file map.
            std::vector<ProjectFile> files = project.files;
            if (files.empty())
                files.push_back({file.name, 0, (uint32_t)file.text.size(), "", ""});
            std::printf("%s", renderUsesGraph(
                buildUsesGraph(computeFileImports(files, program))).c_str());
        } else if (mode == Namespaces) {
            // Discoverability (§4.4): every namespace, the files that open it,
            // and its members. Pure AST + offset map, like --imports/--graph.
            std::vector<ProjectFile> files = project.files;
            if (files.empty())
                files.push_back({file.name, 0, (uint32_t)file.text.size(), "", ""});
            std::printf("%s", renderNamespaces(computeNamespaces(files, program)).c_str());
        } else if (mode == Why) {
            // Discoverability (§4.4): where a bare name resolves from, and — with
            // `in <file>` — which candidate wins for that file (or ambiguity).
            std::vector<ProjectFile> files = project.files;
            if (files.empty())
                files.push_back({file.name, 0, (uint32_t)file.text.size(), "", ""});
            std::printf("%s", renderWhy(computeWhy(files, program, whyName),
                                        whyInFile).c_str());
        } else if (mode == LintNs) {
            // Optional folder~namespace lint (§4.4, opt-in). Reports and exits
            // non-zero on any mismatch so a team can gate CI on the convention;
            // a normal build never runs it (namespaces stay path-decoupled, §12).
            std::vector<ProjectFile> files = project.files;
            if (files.empty())
                files.push_back({file.name, 0, (uint32_t)file.text.size(), "", ""});
            LayoutLint lint = lintNamespaceLayout(files, program);
            std::printf("%s", renderLayoutLint(lint).c_str());
            programExitCode = lint.mismatches > 0 ? 1 : 0;
        } else {
            if (usingProject) {
                validateEntry(project, program, sink);        // entry rule (§ project)
                checkPhantomDeps(project, program, sink);      // pnpm-style dep strictness
            }
            // Per-file span ranges for lexical import scoping (bug.md #8): a
            // top-level `uses` covers exactly its file. A lone source needs none
            // (the Resolver defaults to one file spanning the whole buffer).
            std::vector<std::pair<uint32_t, uint32_t>> fileRanges;
            for (const ProjectFile& pf : project.files)
                fileRanges.push_back({pf.offset, pf.offset + pf.length});

            // Ship-as-files prelude (R2): resolve the directory (or "" =>
            // embedded fallback) once; both Resolver construction sites use it.
            std::string preludeDir = findPreludeDir(preludeDirOverride);

            Resolver resolver(file, sink);
            resolver.setFileRanges(fileRanges);
            resolver.setPreludeDir(preludeDir);        // "" => embedded
            resolver.setTargetTriple(targetTriple);    // "" => host/native
            resolver.run(program);

            // The rule stage (§16.5): comptime folding + attributes, between
            // resolve pass 1 and pass 2. Gated on hasMeta — a program with no
            // metaprogramming surface runs exactly the old pipeline.
            std::unique_ptr<RuleEngine> engine;
            std::unique_ptr<Resolver> resolver2;
            Resolver* R = &resolver;
            if (program.hasMeta && !noRules && !sink.hasErrors()) {
                std::vector<ProjectFile> rfiles = project.files;
                if (rfiles.empty())
                    rfiles.push_back({file.name, 0, (uint32_t)file.text.size(), "", ""});
                ComptimeOptions copts;
                if (comptimeBudget > 0) copts.stepBudget = comptimeBudget;
                if (reentrantBudget > 0) copts.reentrantRounds = reentrantBudget;
                // Item Q (techdesign-target-predicate.md): `target::os` & co
                // reflect the CROSS triple when one was given — the comptime
                // fold must pick the destination's branch, not the host's.
                copts.targetTriple = targetTriple;
                // §9 (Phase 3): the engine computes its own imports map,
                // internally, AFTER comptime folding — a program-wide import
                // list computed here (pre-fold) would miss a `uses` spliced
                // in by a taken `comptime if` branch.
                engine = std::make_unique<RuleEngine>(rfiles, resolver.sema(),
                                                      resolver.preludeProgram(), file,
                                                      sink, copts);
                // LA-20 §5: wire the comptime import() intrinsic. Plan builds
                // resolve against trident's declared asset table (moduleId ->
                // rel -> abs); a bare single file resolves against its own
                // directory instead (project-of-one, no manifest to declare
                // assets in).
                ImportContext ictx;
                if (usingProject) {
                    ictx.assets = &project.assets;
                } else {
                    std::string src = path ? std::string(path) : std::string();
                    size_t slash = src.find_last_of('/');
                    ictx.rootDir = slash == std::string::npos ? std::string()
                                                              : src.substr(0, slash);
                }
                engine->setImportContext(ictx);
                bool changed = engine->run(program);
                if (changed && !sink.hasErrors()) {
                    // Pass 2: folded/injected code resolves and checks exactly
                    // like hand-written code (P1 cost-identity).
                    resolver2 = std::make_unique<Resolver>(file, sink);
                    resolver2->setFileRanges(fileRanges);
                    resolver2->setPreludeDir(preludeDir);
                    resolver2->setTargetTriple(targetTriple);
                    resolver2->run(program);
                    R = resolver2.get();
                }
            }
            if (mode == Rules) {
                std::printf("%s", engine ? engine->renderRulesReport().c_str()
                                         : "[rules] no metaprogramming in this program\n");
            } else
            if (mode == Assets) {
                static const std::vector<ImportedAsset> kNone;
                std::printf("%s", renderImportedAssets(
                    engine ? engine->importedAssets() : kNone, project.assetHashes).c_str());
            } else
            if (mode == ExpandAst) {
                std::printf("%s", printProgram(program).c_str());
            } else
            if (mode == Expand) {
                // LA-31 R1: `--expand` now runs the full Checker before printing,
                // so it shows every checker rewrite (method-ref eta-lambdas, LA-31
                // expr::Expr constructions, named-arg/default normalization). An
                // ill-typed program fails --expand with the ordinary diagnostics
                // (flushed below) and a non-zero exit, exactly like a compile;
                // --ast-after-rules stays the pre-Checker debugging hatch.
                Checker checker(R->sema(), file, sink);
                checker.run(program, &R->preludeProgram());
            }
            if (mode == Expand && !sink.hasErrors()) {
                // Phase 4 §6: source-shaped re-emit, with a `// from rule …`
                // comment above each injected block (located by whether its span
                // falls inside a firing's quasiquote template span).
                std::vector<ExpandProvenance> prov;
                if (engine) {
                    for (const ExpansionRecord& r : engine->expansions()) {
                        LineCol oc = lineColAt(file.text, r.origin.offset);
                        prov.push_back({r.templateSpan.offset, r.templateSpan.end(),
                                        "from rule " + r.ruleName + " @ " +
                                        std::to_string(oc.line) + ":" + std::to_string(oc.col)});
                    }
                }
                static const std::map<uint32_t, std::pair<std::string, size_t>> kNoLits;
                std::printf("%s", printProgramSource(
                    program, prov, engine ? engine->importLiteralSpans() : kNoLits).c_str());
            } else
            if (mode == Full || mode == Run || mode == Ir || mode == Own ||
                mode == IrVerify || mode == MemVerify || mode == EmitCpp || mode == EmitLlvm ||
                mode == NativeObj || mode == Build || mode == BuildNative || mode == EmitElf ||
                mode == Specializations) {
                Checker checker(R->sema(), file, sink);
                checker.run(program, &R->preludeProgram());
            }
            if (mode == Specializations) {
                if (!sink.hasErrors()) {
                    if (program.specializationReport.empty())
                        std::printf("[specializations] none\n");
                    else
                        for (const std::string& row : program.specializationReport)
                            std::printf("[specializations] %s\n", row.c_str());
                }
            } else if (mode == Run) {
                if (!sink.hasErrors()) {
                    Evaluator evaluator(R->sema(), sink);
                    evaluator.initGlobals(R->preludeProgram());
                    std::printf("%s", evaluator.run(program).c_str());
                    programExitCode = evaluator.exitCode();   // exit-codes.md §4
                }
            } else if (mode == Ir || mode == Own || mode == IrVerify || mode == MemVerify || mode == EmitCpp ||
                       mode == EmitLlvm || mode == NativeObj || mode == Build ||
                       mode == BuildNative || mode == EmitElf) {
                if (!sink.hasErrors()) {
                    IrModule module;
                    Lowerer lowerer(R->sema(), sink);
                    lowerer.setColumnar(columnar);   // techdesign-columnar staged flag
                    if (lowerer.lower(program, R->preludeProgram(), module)) {
                        if (mode == EmitElf) {
                            // pure backend: our own x86-64 + ELF, zero deps.
                            X64Gen gen(module, sink);
                            std::string elf = gen.emit();
                            if (!elf.empty()) {
                                std::ofstream of(outPath, std::ios::binary);
                                of.write(elf.data(), (std::streamsize)elf.size());
                                of.close();
                                ::chmod(outPath, 0755);
                                std::fprintf(stderr, "wrote %s\n", outPath);
                            }
                        } else if (mode == Build) {
                            // source -> executable in one step: generate the
                            // self-contained C++ translation unit and pipe it
                            // to the system compiler.
                            CGen cgen(module, sink);
                            std::string cpp = cgen.generate();
                            if (!cpp.empty()) {
                                std::string tried;
                                std::string cxx = probeLinkerDriver(&tried);
                                if (cxx.empty()) {
                                    std::fprintf(stderr,
                                        "error: no C++ compiler found on PATH (tried: %s)\n",
                                        tried.c_str());
                                    return 1;
                                }
                                std::string cmd = cxx + " -O2 -x c++ - -o " + outPath;
                                FILE* p = popen(cmd.c_str(), "w");
                                if (!p) {
                                    std::fprintf(stderr, "error: cannot run %s\n", cxx.c_str());
                                    return 1;
                                }
                                fwrite(cpp.data(), 1, cpp.size(), p);
                                int rc = pclose(p);
                                if (rc != 0) {
                                    std::fprintf(stderr, "error: %s failed\n", cxx.c_str());
                                    return 1;
                                }
                                std::fprintf(stderr, "built %s\n", outPath);
                            }
                        } else if (mode == EmitLlvm || mode == NativeObj || mode == BuildNative) {
#ifdef HAVE_LLVM
                            LlvmGen gen(module, sink);
                            if (mode == EmitLlvm) {
                                std::string ir = gen.emitIr();
                                if (!ir.empty()) std::printf("%s", ir.c_str());
                            } else if (mode == NativeObj) {
                                if (!gen.emitObject(outPath, targetTriple, optLevel)) {
                                    // diagnostics already reported
                                }
                            } else {
                                // --build-native (doc-2 §4 item 2): emit object,
                                // link against liblvrt.a + -lm, clean up the temp
                                // object on success. With --target this becomes a
                                // cross build (doc-2 §6 items 2-3): the object is
                                // emitted for the triple, resolved against the
                                // per-triple runtime archive, and linked via the
                                // cross linker probe. (The B-M4 gate that used to
                                // reject --build-native --target is lifted here —
                                // lifting it is the milestone's exit criterion.)
                                bool cross = targetTriple[0] != '\0';
                                bool windows = cross && isWindowsTriple(targetTriple);
                                std::string objPath = std::string(outPath) + ".o";
                                if (!gen.emitObject(objPath, targetTriple, optLevel)) {
                                    // diagnostics already reported
                                } else if (cross && isWasmTriple(targetTriple)) {
                                    // Track W hard-02 (doc 02 §4): the wasm-ld
                                    // lane — early and parallel to the native
                                    // probe below, which stays byte-identical
                                    // for non-wasm triples. `main` is the entry
                                    // symbol lv_entry.c exposes on native
                                    // (reused, not invented); on wasm the same
                                    // function is named `lv_entry_main` and
                                    // carries `export_name("main")` (doc 02
                                    // §6/§7 finding: there is no crt0/libc
                                    // `_start` pulling it in the way native's
                                    // linker always needs `main`, so wasm-ld
                                    // has nothing to force the archive pull
                                    // with unless told; `--export=lv_entry_main`
                                    // is that forcing root, and the attribute
                                    // then pins the WASM EXPORT table entry to
                                    // "main" regardless — see lv_entry.c).
                                    // `--import-undefined` resolves the
                                    // remaining imports from the JS host
                                    // (techdesign-03-floor-wasm.md §2). No -lm,
                                    // no lvrt.link flags — there is no system
                                    // linker namespace to pull from on wasm.
                                    std::string runtimeLib = runtimePathOverride
                                        ? runtimePathOverride
                                        : findRuntimeArchiveForTriple(targetTriple);
                                    if (runtimeLib.empty()) {
                                        std::fprintf(stderr,
                                            "error: cannot locate the %s runtime archive "
                                            "(looked next to 'leviathan' and in runtime/%s/); "
                                            "build it with 'runtime/build-triple.sh %s' or "
                                            "pass --runtime <path>\n",
                                            targetTriple, targetTriple, targetTriple);
                                        std::remove(objPath.c_str());
                                        return 1;
                                    }
                                    std::string tried;
                                    std::string wasmLd = probeWasmLinker(&tried);
                                    if (wasmLd.empty()) {
                                        std::fprintf(stderr,
                                            "error: no wasm linker on PATH (tried: %s); "
                                            "install LLVM's lld (package 'lld', which "
                                            "provides wasm-ld)\n", tried.c_str());
                                        std::remove(objPath.c_str());
                                        return 1;
                                    }
                                    // the output is a wasm module — name it so
                                    // (the Windows lane's .exe-append precedent).
                                    std::string finalOutPath = outPath;
                                    static const std::string kWasm = ".wasm";
                                    if (finalOutPath.size() < kWasm.size() ||
                                        finalOutPath.compare(finalOutPath.size() - kWasm.size(),
                                                             kWasm.size(), kWasm) != 0)
                                        finalOutPath += kWasm;
                                    std::string cmd = wasmLd + " \"" + objPath + "\" \"" +
                                        runtimeLib + "\" --no-entry "
                                        "--export=lv_entry_main "
                                        "--import-undefined -o \"" + finalOutPath + "\"";
                                    int rc = std::system(cmd.c_str());
                                    if (rc != 0) {
                                        std::fprintf(stderr, "error: link failed\n");
                                        std::remove(objPath.c_str());
                                        return 1;
                                    }
                                    std::remove(objPath.c_str());
                                    std::fprintf(stderr, "built %s\n", finalOutPath.c_str());
                                } else {
                                    std::string runtimeLib = runtimePathOverride
                                        ? runtimePathOverride
                                        : (cross ? findRuntimeArchiveForTriple(targetTriple)
                                                 : findRuntimeArchive());
                                    if (runtimeLib.empty()) {
                                        if (cross)
                                            std::fprintf(stderr,
                                                "error: cannot locate the %s runtime archive "
                                                "(looked next to 'leviathan' and in runtime/%s/); "
                                                "build it with 'runtime/build-triple.sh %s' or "
                                                "pass --runtime <path>\n",
                                                targetTriple, targetTriple, targetTriple);
                                        else
                                            std::fprintf(stderr,
                                                "error: cannot locate liblvrt.a next to the "
                                                "'leviathan' executable (pass --runtime <path>)\n");
                                        std::remove(objPath.c_str());
                                        return 1;
                                    }
                                    std::string tried;
                                    std::string cxx = cross
                                        ? probeCrossLinkerDriver(targetTriple, &tried)
                                        : probeLinkerDriver(&tried);
                                    if (cxx.empty()) {
                                        if (cross)
                                            std::fprintf(stderr,
                                                "error: no cross linker for %s on PATH "
                                                "(tried: %s)\n", targetTriple, tried.c_str());
                                        else
                                            std::fprintf(stderr,
                                                "error: no C/C++ compiler found on PATH to link "
                                                "(tried: %s)\n", tried.c_str());
                                        std::remove(objPath.c_str());
                                        return 1;
                                    }
                                    // Windows (B-M5, doc-2 §7): Winsock needs
                                    // -lws2_32 (lv_plat_win32.c's WSAStartup/
                                    // socket calls), and wine/Windows only run
                                    // a binary named *.exe — append it if the
                                    // caller's --build-native <out> didn't.
                                    std::string finalOutPath = outPath;
                                    if (windows) {
                                        static const std::string kExe = ".exe";
                                        if (finalOutPath.size() < kExe.size() ||
                                            finalOutPath.compare(finalOutPath.size() - kExe.size(),
                                                                 kExe.size(), kExe) != 0)
                                            finalOutPath += kExe;
                                    }
                                    // LA-2 (techdesign-tls-crypto.md §5.2): the
                                    // generic link-plumbing seam. A `lvrt.link`
                                    // text file beside liblvrt.a lists extra link
                                    // flags the archive's providers need (OpenSSL's
                                    // "-lssl -lcrypto" when the TLS provider is
                                    // lv_tls_openssl.c; empty for the none-provider
                                    // and for old archives without the file, which
                                    // keep linking unchanged). build-triple.sh
                                    // writes it per-triple; host CMake writes it too.
                                    std::string extraLink;
                                    {
                                        std::string linkFile = runtimeLib;
                                        size_t slash = linkFile.find_last_of('/');
                                        linkFile = (slash == std::string::npos ? std::string()
                                                    : linkFile.substr(0, slash + 1)) + "lvrt.link";
                                        if (FILE* lf = std::fopen(linkFile.c_str(), "r")) {
                                            char lb[512];
                                            while (std::fgets(lb, sizeof lb, lf)) {
                                                std::string ln(lb);
                                                while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r'))
                                                    ln.pop_back();
                                                if (!ln.empty()) extraLink += " " + ln;
                                            }
                                            std::fclose(lf);
                                        }
                                    }
                                    std::string cmd = cxx + " \"" + objPath + "\" \"" +
                                        runtimeLib + "\" -lm" + (windows ? " -lws2_32" : "") +
                                        extraLink + " -o \"" + finalOutPath + "\"";
                                    int rc = std::system(cmd.c_str());
                                    if (rc != 0) {
                                        std::fprintf(stderr, "error: link failed\n");
                                        std::remove(objPath.c_str());
                                        return 1;
                                    }
                                    std::remove(objPath.c_str());
                                    std::fprintf(stderr, "built %s\n", finalOutPath.c_str());
                                }
                            }
#else
                            std::fprintf(stderr,
                                "error: this build has no LLVM backend (LLVM dev not found)\n");
                            return 2;
#endif
                        } else if (mode == EmitCpp) {
                            CGen cgen(module, sink);
                            std::string cpp = cgen.generate();
                            if (!cpp.empty()) std::printf("%s", cpp.c_str());
                        } else if (mode == Own) {
                            OwnershipInfo info = analyzeOwnership(module);
                            std::printf("%s", ownershipReport(module, info).c_str());
                        } else if (mode == IrVerify) {
                            OwnershipInfo info = analyzeOwnership(module);
                            IrInterp interp(module, &info);
                            std::printf("%s", interp.run().c_str());
                            std::fprintf(stderr,
                                "[ownership] %zu scope-owned allocation(s) tracked, "
                                "%zu violation(s)\n",
                                interp.trackedAllocs(), interp.violations().size());
                            for (const std::string& v : interp.violations())
                                std::fprintf(stderr, "[ownership] VIOLATION: %s\n", v.c_str());
                            if (!interp.violations().empty()) return 1;
                        } else if (mode == MemVerify) {
                            IrInterp interp(module, nullptr, /*memVerify=*/true);
                            std::printf("%s", interp.run().c_str());
                            std::fprintf(stderr, "%s", interp.memReport().c_str());
                        } else {
                            IrInterp interp(module);
                            std::printf("%s", interp.run().c_str());
                            programExitCode = interp.exitCode();   // exit-codes.md §4
                        }
                    }
                }
            } else if (mode != Expand && mode != Rules && mode != Assets) {
                std::printf("%s", R->dumpShapes().c_str());
            }
        }
    }

    if (usingProject) renderProjectDiagnostics(file, project.files, sink);
    else sink.render(file);
    if (sink.hasErrors()) {
        std::fprintf(stderr, "\n%zu error(s)\n", sink.errorCount());
        return 1;
    }
    // exit-codes.md §4: the program's own exit status (0 unless --run/--ir set it
    // via env.exit / env.setExitCode, or an uncaught throw made it 1).
    return programExitCode;
}
