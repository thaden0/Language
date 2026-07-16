#include "resolve.hpp"
#include "checksum.hpp"
#include "discover.hpp"
#include "fetch.hpp"
#include "hash.hpp"
#include "lock.hpp"
#include "mvs.hpp"
#include "semver.hpp"
#include "store.hpp"
#include "Diagnostic.hpp"
#include "Lexer.hpp"
#include "Source.hpp"
#include "Token.hpp"
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fnmatch.h>
#include <fstream>
#include <glob.h>
#include <sstream>
#include <sys/stat.h>

namespace {

bool readWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::string canon(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Forward decl (defined below, near the VCS store-walk it also serves): every
// regular file under `dir`, recursively, sorted.
void listFilesRecursive(const std::string& dir, std::vector<std::string>& out);

// Expand a manifest `sources` list (literals or globs) relative to `base`, in
// manifest order and alphabetically within each glob, deduped via `seen` by
// CANONICAL path. The as-written path is appended to `out`.
bool expandSources(const std::string& base, const std::vector<std::string>& sources,
                   std::set<std::string>& seen, std::vector<std::string>& out,
                   std::string& err) {
    for (const std::string& rel : sources) {
        std::string pattern = base + rel;
        if (rel.find_first_of("*?[") == std::string::npos) {
            if (seen.insert(canon(pattern)).second) out.push_back(pattern);
            continue;
        }
        glob_t g;
        int rc = glob(pattern.c_str(), GLOB_MARK, nullptr, &g);
        if (rc == GLOB_NOMATCH) {
            globfree(&g);
            err = "source pattern '" + rel + "' matched no files";
            return false;
        }
        if (rc != 0) {
            globfree(&g);
            err = "failed to expand source pattern '" + rel + "'";
            return false;
        }
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            std::string p = g.gl_pathv[i];
            if (!p.empty() && p.back() == '/') continue;   // GLOB_MARK flags dirs
            if (seen.insert(canon(p)).second) out.push_back(p);
        }
        globfree(&g);
    }
    return true;
}

bool gatherSources(const std::string& base, const std::vector<std::string>& sources,
                   const std::string& origin, const std::string& moduleId,
                   std::set<std::string>& seen, std::vector<ResolvedSource>& gather,
                   std::string& err) {
    std::vector<std::string> paths;
    if (!expandSources(base, sources, seen, paths, err)) return false;
    for (std::string& p : paths) gather.push_back({p, moduleId, origin});
    return true;
}

// LA-20 §7: `**` recursive matching — trident-only, since sources' flat
// `*.lev` needed only plain glob(). A pattern is split on its (first) "**":
// everything before is a literal directory prefix, everything after (if any)
// is matched via fnmatch against each file's path relative to that prefix,
// so `views/**` matches every file under views/ and `views/**/*.html` (or
// the equivalent `views/**.html`) matches every .html file at any depth.
// Zero matches is a WARNING (printed, not returned as an error) — unlike a
// plain source glob, an empty `views/` during scaffolding must not fail the
// build (§7).
void expandRecursiveAssetGlob(const std::string& base, const std::string& pattern,
                              std::set<std::string>& seen, std::vector<std::string>& out) {
    size_t starstar = pattern.find("**");
    std::string prefix = pattern.substr(0, starstar);
    std::string suffix = pattern.substr(starstar + 2);
    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    if (!suffix.empty() && suffix.front() == '/') suffix.erase(suffix.begin());

    std::string rootDir = base + prefix;
    std::vector<std::string> allFiles;
    listFilesRecursive(rootDir, allFiles);
    std::sort(allFiles.begin(), allFiles.end());

    bool any = false;
    for (const std::string& f : allFiles) {
        std::string rel = f.size() > rootDir.size() + 1 ? f.substr(rootDir.size() + 1) : f;
        bool matches = suffix.empty() || ::fnmatch(suffix.c_str(), rel.c_str(), 0) == 0;
        if (matches && seen.insert(canon(f)).second) { out.push_back(f); any = true; }
    }
    if (!any)
        std::fprintf(stderr, "warning: asset pattern '%s' matched no files\n", pattern.c_str());
}

// Expand a manifest `assets` list (literals, plain globs, or `**` recursive
// globs) relative to `base`. Unlike `expandSources`, a glob matching zero
// files is a warning (§7) — only a glob-free literal naming a missing file
// is an error.
bool expandAssets(const std::string& base, const std::vector<std::string>& assets,
                  std::set<std::string>& seen, std::vector<std::string>& out,
                  std::string& err) {
    for (const std::string& rel : assets) {
        if (rel.find("**") != std::string::npos) {
            expandRecursiveAssetGlob(base, rel, seen, out);
            continue;
        }
        std::string pattern = base + rel;
        if (rel.find_first_of("*?[") == std::string::npos) {
            struct stat st;
            if (::stat(pattern.c_str(), &st) != 0) {
                err = "asset '" + rel + "' does not name an existing file";
                return false;
            }
            if (seen.insert(canon(pattern)).second) out.push_back(pattern);
            continue;
        }
        glob_t g;
        int rc = glob(pattern.c_str(), GLOB_MARK, nullptr, &g);
        if (rc != 0 && rc != GLOB_NOMATCH) {
            globfree(&g);
            err = "failed to expand asset pattern '" + rel + "'";
            return false;
        }
        if (rc == GLOB_NOMATCH || g.gl_pathc == 0)
            std::fprintf(stderr, "warning: asset pattern '%s' matched no files\n", rel.c_str());
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            std::string p = g.gl_pathv[i];
            if (!p.empty() && p.back() == '/') continue;   // GLOB_MARK flags dirs
            if (seen.insert(canon(p)).second) out.push_back(p);
        }
        globfree(&g);
    }
    return true;
}

// Gather one module's declared assets, hashing each (sha256Hex, hash.hpp) so
// leviathan never hashes anything itself (contract rule 1). `rel` is the
// gathered path with `base` stripped — the exact string a plan-mode
// `import()` matches against.
bool gatherAssets(const std::string& base, const std::vector<std::string>& assetPatterns,
                  const std::string& moduleId, std::vector<ResolvedAsset>& out,
                  std::string& err) {
    std::set<std::string> seen;
    std::vector<std::string> paths;
    if (!expandAssets(base, assetPatterns, seen, paths, err)) return false;
    for (const std::string& p : paths) {
        std::string content;
        if (!readWholeFile(p, content)) {
            err = "cannot read asset '" + p + "'";
            return false;
        }
        ResolvedAsset a;
        a.rel = p.size() > base.size() ? p.substr(base.size()) : p;
        a.path = p;
        a.moduleId = moduleId;
        a.hash = "sha256:" + sha256Hex(content);
        out.push_back(std::move(a));
    }
    return true;
}

// The top-level (brace-depth 0) namespaces a source file declares — a
// dependency's exported namespaces, for `as` aliasing. Lexical, no full parse.
std::set<std::string> scanExportedNamespaces(const std::string& text) {
    SourceFile sf;
    sf.name = "<dep-scan>";
    sf.text = text;
    DiagnosticSink dummy;
    Lexer lex(sf, dummy);
    std::vector<Token> toks = lex.tokenize();
    std::set<std::string> out;
    int depth = 0;
    for (size_t i = 0; i < toks.size(); ++i) {
        TokenKind k = toks[i].kind;
        if (k == TokenKind::LBrace) ++depth;
        else if (k == TokenKind::RBrace) --depth;
        else if (k == TokenKind::KwNamespace && depth == 0 &&
                 i + 1 < toks.size() && toks[i + 1].kind == TokenKind::Identifier)
            out.insert(std::string(toks[i + 1].text));
    }
    return out;
}

// Infer each dep's `kind` (techdesign-package-manager.md §4 P2.0-1): a
// Recursively load local-path dependencies. Each Local dep's `path` is a
// local directory holding its own manifest; its sources gather into the same
// project (deps are just more source, §1/§6). Vcs deps (§4 P2.0-1) are
// recognized here but skipped — P2.1 resolves them via MVS + the git
// provider (docs/techdesign-package-manager.md §5). `visited` guards
// cycles/dups among Local deps.
bool loadDepsRec(const std::vector<Dependency>& deps, const std::string& base,
                 const std::string& parentModule, std::set<std::string>& visited,
                 std::set<std::string>& seenFiles, std::vector<ResolvedSource>& gather,
                 std::vector<ResolvedAsset>& assetGather,
                 std::map<std::string, std::set<std::string>>& moduleDeps,
                 std::string& err) {
    for (const Dependency& d : deps) {
        if (d.kind == DepKind::Vcs) continue;   // not yet resolved — P2.1

        std::string depDir = base + d.path;
        if (depDir.empty() || depDir.back() != '/') depDir += '/';
        std::string depModule = canon(depDir);
        moduleDeps[parentModule].insert(depModule);   // the dep-graph edge

        std::string depManifest = discoverManifestPath(depDir);
        if (depManifest.empty()) {
            err = "cannot find a manifest in dependency directory '" + depDir +
                 "' (looked for trident.toml)";
            return false;
        }
        if (!visited.insert(canon(depManifest)).second) continue;   // cycle/dup guard

        std::string mtext;
        if (!readWholeFile(depManifest, mtext)) {
            err = "cannot read dependency manifest '" + depManifest + "'";
            return false;
        }
        ProjectManifest dm;
        std::string perr;
        if (!parseManifest(depManifest, mtext, dm, perr)) {
            err = "dependency '" + d.path + "' has manifest errors: " + perr;
            return false;
        }
        if (!gatherSources(depDir, dm.sources, d.path, depModule, seenFiles, gather, err))
            return false;
        // LA-20 §7: a local dep's own `assets =`, keyed by ITS moduleId — a
        // dependency's `import()` resolves against its own declared assets,
        // mirroring phantom-dep discipline (§5): an app cannot reach into a
        // dep's assets, or vice versa, without declaring them itself.
        std::vector<ResolvedAsset> depAssets;
        if (!gatherAssets(depDir, dm.assets, depModule, depAssets, err)) return false;
        for (ResolvedAsset& a : depAssets) assetGather.push_back(std::move(a));
        if (!inferAndValidateDependencyKinds(dm.deps, depDir, err)) return false;
        if (!loadDepsRec(dm.deps, depDir, depModule, visited, seenFiles, gather,
                         assetGather, moduleDeps, err))
            return false;
    }
    return true;
}

// Every regular file under `dir`, recursively, sorted (so the resulting plan
// rows are deterministic regardless of directory-entry order). A fetched
// module's store directory (store.hpp) contains EXACTLY its declared
// sources, nothing else, so this is a correct — and interface-change-free —
// way to recover a materialized module's file list (P2.1e, §5.5).
void listFilesRecursive(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = ::readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string path = dir + "/" + name;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) listFilesRecursive(path, out);
        else out.push_back(path);
    }
    ::closedir(d);
}

// The canonical "module directory" for one of the root's direct deps,
// regardless of kind — a Local dep's own directory, or a Vcs dep's
// materialized store directory (looked up by the ModuleId `resolveVcsDeps`
// resolved it to). Returns false if a Vcs dep was excluded (dev-filtered)
// or otherwise never made it into `storeDirByModule` — callers treat that
// as "nothing to alias/edge for this dep," not an error, since dev
// filtering is an intentional, expected omission.
bool depModuleFor(const Dependency& d, const std::string& base,
                  const std::map<ModuleId, std::string>& storeDirByModule,
                  std::string& out) {
    if (d.kind == DepKind::Local) {
        std::string depDir = base + d.path;
        if (depDir.empty() || depDir.back() != '/') depDir += '/';
        out = canon(depDir);
        return true;
    }
    Require r;
    std::string err;
    if (!dependencyToRequire(d, r, err)) return false;
    auto it = storeDirByModule.find(r.mod);
    if (it == storeDirByModule.end()) return false;
    out = it->second;
    return true;
}

}  // namespace

// Infer each dep's `kind` (techdesign-package-manager.md §4 P2.0-1): a
// `path` that resolves to an existing directory, relative to `base`, is
// Local; otherwise it is Vcs, which requires `version`. Inference needs
// `base` (the manifest's own directory), which parseManifest() does not
// have — this runs once per manifest, right after it is parsed, before its
// deps are walked. Mutates `deps` in place; deliberately never revisited
// once set (re-running this on the same list would be a no-op anyway, since
// the check is purely a function of path + base). Exported (not file-local,
// unlike P2.0) so commands.cpp (P2.1e: add/update/lock/fetch/why) can
// correctly classify a freshly-parsed manifest's deps without going through
// the whole resolveProject/plan pipeline.
bool inferAndValidateDependencyKinds(std::vector<Dependency>& deps,
                                     const std::string& base, std::string& err) {
    for (Dependency& d : deps) {
        std::string depDir = base + d.path;
        if (depDir.empty() || depDir.back() != '/') depDir += '/';
        struct stat st;
        bool isLocalDir = ::stat(depDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        d.kind = isLocalDir ? DepKind::Local : DepKind::Vcs;
        if (d.kind == DepKind::Vcs && d.version.empty()) {
            err = "dependency '" + d.path + "' is not a local directory, so it is "
                 "treated as a VCS dependency, which requires 'version' "
                 "(e.g. version = \"1.2.0\")";
            return false;
        }
    }
    return true;
}

namespace {

// Do `rootRequires` all appear in `lock` at a version >= their minimum
// (§3.4's consistency check)? A lighter check than full MVS-equivalence —
// it trusts the lock's own transitive `requires` edges were correct when
// written (by a real `trident lock` run) rather than re-verifying them, but
// it does catch the common "bumped a direct dep, forgot to re-lock" case,
// with zero network access.
bool lockSatisfiesRootRequires(const Lockfile& lock, const std::vector<Require>& rootRequires) {
    for (const Require& r : rootRequires) {
        bool found = false;
        for (const LockedModule& m : lock.modules) {
            if (!(m.mod == r.mod)) continue;
            Version sel;
            std::string perr;
            if (parseSemVer(m.selected, sel, perr) && compareSemVer(sel, r.min) >= 0) found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

// The inverse of lock.cpp's serializeRequireEdge: "path@MAJOR.MINOR.PATCH"
// -> a Require (mod.major re-derived from the version, same clamp
// dependencyToRequire applies — mvs.cpp's §0.5 major<=1 collapse). `as_`/
// `dev` are not recorded in a lock's `requires` list (irrelevant to
// rebuilding moduleDeps edges, the only reason this parse exists) and are
// left default.
bool parseRequireEdge(const std::string& text, Require& out) {
    size_t at = text.rfind('@');
    if (at == std::string::npos) return false;
    Version v;
    std::string err;
    if (!parseSemVer(text.substr(at + 1), v, err)) return false;
    out.mod = ModuleId{text.substr(0, at), v.major <= 1 ? 1 : v.major};
    out.min = v;
    return true;
}

// The hermetic, network-free provider for `trident vendor`/`--vendor`
// (techdesign-package-manager.md §6 P2.2, GT4): every module's sources are
// read straight from `vendorDir/<serializeModuleId>/` — no git, no
// $TRIDENT_HOME store. Only `materialize()` is ever actually reached: vendor
// mode requires a consistent lock (resolveVcsDeps enforces this below before
// constructing one), so MVS's `manifestOf()`/`versions()` are never called.
class VendorProvider : public ModuleProvider {
public:
    explicit VendorProvider(std::string vendorDir) : vendorDir_(std::move(vendorDir)) {}

    bool manifestOf(const ModuleId&, const Version&, ProjectManifest&, std::string& err) override {
        err = "internal error: vendor mode must resolve entirely from the lock — "
             "manifestOf() should never be called";
        return false;
    }

    bool materialize(const ModuleId& mod, const Version&, std::string& storeDir,
                     std::string& contentHash, std::string& err) override {
        storeDir = vendorDir_ + "/" + serializeModuleId(mod);
        struct stat st;
        if (::stat(storeDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            err = "module '" + serializeModuleId(mod) + "' is not vendored under '" +
                 vendorDir_ + "' (run `trident vendor` first)";
            return false;
        }
        std::vector<std::string> files;
        listFilesRecursive(storeDir, files);
        std::string base = storeDir + "/";
        std::vector<StoreFile> sf;
        sf.reserve(files.size());
        for (const std::string& f : files) {
            std::string rel = f.compare(0, base.size(), base) == 0 ? f.substr(base.size()) : f;
            sf.push_back({rel, f});
        }
        return canonicalContentHash(sf, contentHash, err);
    }

    bool versions(const ModuleId&, std::vector<Version>&, std::string& err) override {
        err = "internal error: vendor mode must resolve entirely from the lock — "
             "versions() should never be called";
        return false;
    }

private:
    std::string vendorDir_;
};

}  // namespace

VcsResolution resolveVcsDeps(const ProjectManifest& manifest, bool includeDevDeps,
                             const std::string& lockPath, const std::string& vendorDir) {
    VcsResolution vr;

    std::vector<Require> rootRequires;
    for (const Dependency& d : manifest.deps) {
        if (d.kind != DepKind::Vcs) continue;
        if (d.dev && !includeDevDeps) continue;
        Require r;
        if (!dependencyToRequire(d, r, vr.err)) return vr;
        rootRequires.push_back(r);
    }
    if (rootRequires.empty()) {
        vr.ok = true;   // no Vcs deps (or all dev-filtered) — nothing to do,
        return vr;       // and no git prerequisite for a project with none
    }

    if (!vendorDir.empty() && lockPath.empty()) {
        vr.err = "internal error: vendor mode requires a lock path";
        return vr;
    }

    // No dispatching provider is needed for MVS itself (contrast the
    // design's "a provider that dispatches per dep kind"): rootRequires
    // above already contains ONLY Vcs-kind deps (Local-kind ones stay
    // entirely on the loadDepsRec path, never touching MVS), and P2.1a's
    // mvs.cpp treats every dep reached via manifestOf() as Vcs
    // unconditionally (logged there) — so every ModuleId this MVS run ever
    // visits is Git-backed. LocalProvider (P2.0) remains a
    // proven-but-unwired-into-MVS seam. `fetchProvider` (P2.2) is the
    // separate choice of WHERE the final materialize() step below reads
    // from — GitProvider normally, or VendorProvider under `--vendor`.
    GitProvider gitProvider;
    VendorProvider vendorProvider(vendorDir);
    ModuleProvider& fetchProvider = vendorDir.empty()
        ? static_cast<ModuleProvider&>(gitProvider) : static_cast<ModuleProvider&>(vendorProvider);

    // §3.4: "trident build/run/check use the lock verbatim when it is
    // present and consistent... never a silent re-resolve." A non-empty
    // `lockPath` (resolveProject's callers) opts into this: if the lock
    // parses and its top-level entries satisfy the CURRENT manifest's own
    // requires, skip the MVS graph walk entirely (no manifestOf() network
    // calls) and use the lock's selection as-is. `add`/`lock`/`update`/
    // `fetch` (commands.cpp) pass "" instead — their whole point is
    // recomputing the lock, so they must never short-circuit off a
    // possibly-stale one. `lockHashByModule` caches each module's lock-
    // pinned hash for the P2.2 integrity check in the materialize loop below.
    std::vector<BuildListEntry> selected;
    bool usedLock = false;
    std::map<ModuleId, std::string> lockHashByModule;
    if (!lockPath.empty()) {
        std::string lockText;
        Lockfile lock;
        std::string lerr;
        if (readWholeFile(lockPath, lockText) && parseLockfile(lockPath, lockText, lock, lerr) &&
            lockSatisfiesRootRequires(lock, rootRequires)) {
            for (const LockedModule& m : lock.modules) {
                BuildListEntry e;
                e.mod = m.mod;
                std::string perr;
                if (!parseSemVer(m.selected, e.selected, perr)) { usedLock = false; selected.clear(); break; }
                std::string hexHash = m.hash.compare(0, 7, "sha256:") == 0 ? m.hash.substr(7) : m.hash;
                e.contentHash = hexHash;
                e.storeDir = storeRoot() + "/" + hexHash;
                lockHashByModule[m.mod] = hexHash;
                for (const std::string& reqText : m.requires_) {
                    Require r;
                    if (parseRequireEdge(reqText, r)) e.requires_.push_back(r);
                }
                selected.push_back(std::move(e));
            }
            usedLock = !selected.empty() || lock.modules.empty();
        }
    }

    if (!usedLock) {
        if (!vendorDir.empty()) {
            vr.err = "vendor mode requires a valid, up-to-date trident.lock — run `trident "
                    "lock` (with network) first, then `trident vendor`, then rebuild with "
                    "--vendor";
            return vr;
        }
        MvsResult mvs = selectVersions(rootRequires, gitProvider);
        if (!mvs.ok) {
            vr.err = mvs.err;
            return vr;
        }
        selected.assign(mvs.buildList.begin(), mvs.buildList.end());
    }

    // Materialize every selected entry — idempotent (store.cpp): a cache
    // hit when `usedLock` found it already fetched, a real (network) fetch
    // only on a genuinely cold cache, matching ordinary package-manager UX
    // (a present lock pins WHAT to fetch; it does not forbid fetching it).
    // P2.2 GT4: every fetch is then verified — against the checksum DB
    // (tamper-evident across time/machines; skipped in vendor mode, which by
    // design depends on nothing outside `vendorDir`) and, when the lock was
    // used verbatim, against the lock's own pinned hash — either mismatch is
    // a loud error, never a silent acceptance.
    for (BuildListEntry e : selected) {
        std::string storeDir, contentHash, merr;
        if (!fetchProvider.materialize(e.mod, e.selected, storeDir, contentHash, merr)) {
            vr.err = "materializing " + e.mod.path + "@" + formatSemVer(e.selected) + ": " + merr;
            return vr;
        }
        e.storeDir = storeDir;
        e.contentHash = contentHash;

        if (vendorDir.empty()) {
            std::string cerr;
            if (!checksumDbVerifyOrRecord(checksumDbPath(), e.mod, e.selected, contentHash, cerr)) {
                vr.err = "checksum verification failed for " + e.mod.path + "@" +
                        formatSemVer(e.selected) + ": " + cerr;
                return vr;
            }
        }

        if (usedLock) {
            auto it = lockHashByModule.find(e.mod);
            if (it != lockHashByModule.end() && it->second != contentHash) {
                vr.err = "content for " + e.mod.path + "@" + formatSemVer(e.selected) +
                        " does not match trident.lock (expected sha256:" + it->second +
                        ", got sha256:" + contentHash + ") — possible tampering; if this is "
                        "an intentional dependency change, run `trident lock`";
                return vr;
            }
        }

        vr.buildList.push_back(std::move(e));
    }
    vr.ok = true;
    return vr;
}

ResolvedProject resolveProject(const std::string& manifestPath, const std::string& aliasDir,
                               bool includeDevDeps, const std::string& vendorDir) {
    ResolvedProject rp;

    std::string mtext;
    if (!readWholeFile(manifestPath, mtext)) {
        std::fprintf(stderr, "error: cannot read manifest '%s'\n", manifestPath.c_str());
        return rp;
    }
    std::string perr;
    if (!parseManifest(manifestPath, mtext, rp.manifest, perr)) {
        std::fprintf(stderr, "error: %s\n", perr.c_str());
        return rp;
    }

    std::string base = dirOf(manifestPath);
    std::set<std::string> seenFiles;
    std::vector<ResolvedSource> gather;
    std::string err;
    if (!gatherSources(base, rp.manifest.sources, "", "", seenFiles, gather, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return rp;
    }
    // LA-20 §7: the root's own declared assets, keyed under moduleId "".
    if (!gatherAssets(base, rp.manifest.assets, "", rp.assets, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return rp;
    }
    if (!inferAndValidateDependencyKinds(rp.manifest.deps, base, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return rp;
    }
    std::set<std::string> visited;
    visited.insert(canon(manifestPath));   // the root itself — a dep cycling back
                                           // to it must not re-gather it
    if (!loadDepsRec(rp.manifest.deps, base, "", visited, seenFiles, gather,
                     rp.assets, rp.moduleDeps, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return rp;
    }

    // VCS deps (P2.1e, techdesign-package-manager.md §5.5): the root's own
    // Vcs-kind deps feed MVS + fetch (resolveVcsDeps) — Local-kind deps
    // never reach here, already fully handled above via the unchanged
    // loadDepsRec path. Each materialized module becomes more `gather`
    // entries (moduleId = its store directory, origin = its VCS path) and a
    // moduleDeps edge, exactly the shape loadDepsRec already produces for
    // local deps (§3.3 rule 4) — leviathan's phantom-dep enforcement and the
    // plan writer need no change to carry them.
    std::map<ModuleId, std::string> storeDirByModule;
    VcsResolution vr = resolveVcsDeps(rp.manifest, includeDevDeps, lockfilePathFor(manifestPath),
                                      vendorDir);
    if (!vr.ok) {
        std::fprintf(stderr, "error: %s\n", vr.err.c_str());
        return rp;
    }
    for (const BuildListEntry& e : vr.buildList) storeDirByModule[e.mod] = e.storeDir;
    for (const BuildListEntry& e : vr.buildList) {
        std::vector<std::string> files;
        listFilesRecursive(e.storeDir, files);
        std::sort(files.begin(), files.end());
        for (const std::string& f : files) gather.push_back({f, e.storeDir, e.mod.path});
        // This module's OWN direct requires (a cached copy of the edges,
        // provider.hpp) become edges FROM it — mirrors loadDepsRec adding
        // `moduleDeps[parentModule].insert(depModule)` once per recursion
        // level, never flattening the whole transitive closure onto one node.
        for (const Require& req : e.requires_) {
            auto it = storeDirByModule.find(req.mod);
            if (it != storeDirByModule.end()) rp.moduleDeps[e.storeDir].insert(it->second);
        }
    }
    // The root may `uses` only what it DIRECTLY declared (not everything MVS
    // transitively pulled in) — same phantom-dep rule loadDepsRec already
    // enforces for local deps.
    for (const Dependency& d : rp.manifest.deps) {
        if (d.kind != DepKind::Vcs) continue;
        std::string depModule;
        if (depModuleFor(d, base, storeDirByModule, depModule))
            rp.moduleDeps[""].insert(depModule);
    }

    // `as` binding (§3.5): for each of the root's direct deps that requests a
    // local alias, synthesize `namespace <as> { uses <exported>; }` so `uses
    // <as>` and `<as>::member` reach the dependency's namespace(s) under the
    // chosen name — same semantics the compiler used to apply in-memory.
    // Unlike the compiler, trident must materialize this as a real on-disk
    // file: the plan format carries only resolved paths (§3.3 rule 1); an
    // alias is "just more source" too (§6), now literally another plan row.
    // `depModuleFor` handles both dep kinds uniformly (a Local dep's own
    // directory, or a Vcs dep's materialized store directory).
    std::map<std::string, std::set<std::string>> exportsByModule;
    for (const Dependency& d : rp.manifest.deps) {
        if (d.as_.empty()) continue;
        std::string depModule;
        if (!depModuleFor(d, base, storeDirByModule, depModule)) continue;
        if (exportsByModule.count(depModule)) continue;
        std::set<std::string> ns;
        for (const ResolvedSource& e : gather)
            if (e.moduleId == depModule) {
                std::string text;
                if (readWholeFile(e.path, text)) {
                    std::set<std::string> fileNs = scanExportedNamespaces(text);
                    ns.insert(fileNs.begin(), fileNs.end());
                }
            }
        exportsByModule[depModule] = std::move(ns);
    }

    std::vector<ResolvedSource> aliasSources;
    int aliasIndex = 0;
    for (const Dependency& d : rp.manifest.deps) {
        if (d.as_.empty()) continue;
        std::string depModule;
        if (!depModuleFor(d, base, storeDirByModule, depModule)) continue;
        auto it = exportsByModule.find(depModule);
        std::string body;
        if (it != exportsByModule.end())
            for (const std::string& ns : it->second)
                if (ns != d.as_) body += " uses " + ns + ";";   // no self-alias
        if (body.empty()) continue;   // nothing to alias (or the name is verbatim)
        std::string aliasText = "namespace " + d.as_ + " {" + body + " }\n";
        std::string aliasPath = aliasDir + "/trident-alias-" +
                                std::to_string(aliasIndex++) + "-" + d.as_ + ".lev";
        std::ofstream out(aliasPath, std::ios::binary);
        out << aliasText;
        out.close();
        aliasSources.push_back({canon(aliasPath), "", ""});
    }

    rp.sources.reserve(aliasSources.size() + gather.size());
    for (auto& a : aliasSources) rp.sources.push_back(std::move(a));
    for (auto& g : gather) rp.sources.push_back(std::move(g));

    // Entry point, classified explicitly here (once, on trident's side) so
    // leviathan never sniffs a filename (§3.3 rule 2). P0-2's transitional
    // rule carries over verbatim: a `.lev`/`.ext` suffix means File.
    const std::string& entry = rp.manifest.entry;
    if (!entry.empty()) {
        bool isFile = (entry.size() >= 4 && entry.compare(entry.size() - 4, 4, ".ext") == 0) ||
                     (entry.size() >= 4 && entry.compare(entry.size() - 4, 4, ".lev") == 0);
        if (isFile) {
            rp.entryKind = "file";
            rp.entryTarget = base + entry;   // matches a gathered literal source's path
        } else {
            rp.entryKind = "function";
            rp.entryTarget = entry;
        }
    }

    rp.ok = true;
    return rp;
}

// --- LocalProvider (resolve.hpp) — the reference ModuleProvider for local
// deps (techdesign-package-manager.md §0.2/§3.3). Reuses the same
// discoverManifestPath/readWholeFile/parseManifest helpers loadDepsRec uses
// above; not yet called from resolveProject's live path (P2.1e wires
// per-dep-kind provider dispatch in).

bool LocalProvider::manifestOf(const ModuleId& mod, const Version&,
                               ProjectManifest& out, std::string& err) {
    std::string manifestPath = discoverManifestPath(mod.path);
    if (manifestPath.empty()) {
        err = "cannot find a manifest in dependency directory '" + mod.path +
             "' (looked for trident.toml)";
        return false;
    }
    std::string text;
    if (!readWholeFile(manifestPath, text)) {
        err = "cannot read dependency manifest '" + manifestPath + "'";
        return false;
    }
    return parseManifest(manifestPath, text, out, err);
}

bool LocalProvider::materialize(const ModuleId& mod, const Version&,
                                std::string& storeDir, std::string& contentHash,
                                std::string& err) {
    // A local dep is already materialized on disk — the plan references its
    // gathered sources directly (§3.3 rule 1); there is no store copy and no
    // integrity hash to compute.
    storeDir = mod.path;
    contentHash.clear();
    (void)err;
    return true;
}

bool LocalProvider::versions(const ModuleId&, std::vector<Version>& out, std::string&) {
    out.clear();   // a local dep has exactly one "version": whatever is on disk
    return true;
}
