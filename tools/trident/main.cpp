// trident — the package manager / build driver (techdesign-toolchain.md).
// B-M2: discover manifest -> resolve (parse + deps + aliasing) -> write plan
// -> locate leviathan (§3.4) -> invoke `leviathan --plan ...` with the mode
// mapped from the subcommand -> relay its diagnostics + exit code.
#include "commands.hpp"
#include "discover.hpp"
#include "plan.hpp"
#include "resolve.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

bool ensureDir(const std::string& dir) {
    struct stat st;
    if (::stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(dir.c_str(), 0755) == 0;
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Run leviathan and relay its exit code. Uses fork/execv (not system()) so
// paths need no shell quoting.
int runLeviathan(const std::string& bin, const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(bin.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) { std::perror("trident: fork"); return 1; }
    if (pid == 0) {
        ::execv(bin.c_str(), argv.data());
        std::perror("trident: execv");
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

struct Options {
    std::string manifestArg;
    std::string outOverride;
    std::string targetTriple;
    std::string planOverride;
    std::string leviathanOverride;
    int optLevel = 2;
    bool useVendor = false;   // P2.2 GT4: --vendor, resolve VCS deps from
                              // ./vendor instead of git (no network access).
};

int dispatch(const std::string& subcommand, const Options& o) {
    std::string manifestPath = resolveManifestArg(o.manifestArg);
    if (manifestPath.empty()) {
        std::fprintf(stderr,
            "error: no manifest found (looked for trident.toml in '%s')\n",
            o.manifestArg.empty() ? "." : o.manifestArg.c_str());
        return 1;
    }

    // The build dir also hosts materialized `as`-alias sources (§3.3 rule 1 —
    // the plan carries only on-disk paths). Default to ./build; an explicit
    // --plan lives wherever the caller wants it (e.g. a test's own tmpdir),
    // so alias materialization follows it there instead of littering ./build.
    std::string buildDir, planPath;
    if (!o.planOverride.empty()) {
        planPath = o.planOverride;
        size_t slash = planPath.find_last_of('/');
        buildDir = slash == std::string::npos ? "." : planPath.substr(0, slash);
    } else {
        buildDir = "build";
        planPath = buildDir + "/plan.lvplan";
    }
    if (!ensureDir(buildDir)) {
        std::fprintf(stderr, "error: cannot create build directory '%s'\n", buildDir.c_str());
        return 1;
    }

    // Resolve: manifest parse + dependency graph + `as` aliasing (§3.2). This
    // is the whole of trident's package-manager job; leviathan never sees it.
    // dev-flagged VCS deps are excluded from the three modes that produce a
    // shippable artifact; `check` and the mode-agnostic `plan` (used to drive
    // several leviathan modes against one resolved plan, e.g.
    // tests/run_project.sh) include them (§5.5).
    bool includeDevDeps = (subcommand != "build" && subcommand != "run" &&
                          subcommand != "emit-llvm");
    // P2.2 GT4: --vendor reads VCS deps from `<manifest-dir>/vendor/` (laid
    // down earlier by `trident vendor`) instead of git — a hermetic,
    // network-free build. resolveVcsDeps enforces that this only works off
    // a valid, consistent trident.lock (resolve.cpp).
    std::string vendorDir = o.useVendor ? dirOf(manifestPath) + "vendor" : "";
    ResolvedProject rp = resolveProject(manifestPath, buildDir, includeDevDeps, vendorDir);
    if (!rp.ok) return 1;

    WritePlanOptions opts;
    opts.target = o.targetTriple;
    opts.optLevel = o.optLevel;
    std::string defaultOut = !rp.manifest.out.empty()  ? rp.manifest.out
                            : !rp.manifest.name.empty() ? rp.manifest.name
                                                        : "a.out";
    opts.out = o.outOverride.empty() ? defaultOut : o.outOverride;

    if (subcommand == "plan") {
        // Resolve + write only — no leviathan invocation. Useful for scripts/
        // tests that want to drive leviathan themselves across several modes
        // against the same resolved plan (e.g. tests/run_project.sh).
        opts.mode = "";
        if (!writeBuildPlan(planPath, rp, opts)) return 1;
        std::printf("%s\n", planPath.c_str());
        return 0;
    }

    std::vector<std::string> leviathanArgs = {"--plan", planPath};
    if (subcommand == "build") {
        opts.mode = "build-native";
        leviathanArgs.push_back("--build-native");
        leviathanArgs.push_back(opts.out);
        if (!o.targetTriple.empty()) {
            leviathanArgs.push_back("--target");
            leviathanArgs.push_back(o.targetTriple);
        }
        leviathanArgs.push_back("--opt-level");
        leviathanArgs.push_back(std::to_string(o.optLevel));
    } else if (subcommand == "run") {
        opts.mode = "run";
        leviathanArgs.push_back("--run");
    } else if (subcommand == "check") {
        opts.mode = "check";
        // No extra flag: leviathan's default (Full) mode resolves, runs the
        // rule stage, and type-checks — no execution.
    } else if (subcommand == "emit-llvm") {
        opts.mode = "emit-llvm";
        leviathanArgs.push_back("--emit-llvm");
    } else {
        std::fprintf(stderr, "error: unknown subcommand '%s'\n", subcommand.c_str());
        return 2;
    }

    if (!writeBuildPlan(planPath, rp, opts)) return 1;

    std::string tried;
    std::string leviathanBin = findLeviathan(o.leviathanOverride, &tried);
    if (leviathanBin.empty()) {
        std::fprintf(stderr,
            "error: cannot locate the 'leviathan' executable "
            "(tried: %s)\n", tried.c_str());
        return 1;
    }

    return runLeviathan(leviathanBin, leviathanArgs);
}

void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s <build|run|check|emit-llvm|plan> [manifest-or-dir] [--out <path>] "
        "[--target <triple>] [--opt-level <0|2>] [--plan <path>] "
        "[--leviathan <path>] [--vendor]\n"
        "       %s add <path>[@version] [--as <name>] [--dev] [manifest-or-dir]\n"
        "       %s remove <path> [manifest-or-dir]\n"
        "       %s update [<path>] [manifest-or-dir]\n"
        "       %s lock [manifest-or-dir]\n"
        "       %s fetch [manifest-or-dir]\n"
        "       %s why <path> [manifest-or-dir]\n"
        "       %s audit [manifest-or-dir]\n"
        "       %s vendor [manifest-or-dir]\n"
        "       %s --version\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { printUsage(argv[0]); return 2; }

    std::string sub = argv[1];
    if (sub == "--version") {
        std::printf("trident 0.1 (techdesign-toolchain.md Phase 1)\n");
        std::fflush(stdout);   // ordered before the forked child's own writes
        std::string tried;
        std::string bin = findLeviathan("", &tried);
        if (bin.empty()) {
            std::printf("leviathan: not found (tried: %s)\n", tried.c_str());
            return 1;
        }
        return runLeviathan(bin, {"--version"});
    }
    // P2.1e dependency-management CLI (techdesign-package-manager.md §5.5):
    // none of these invoke leviathan. `add`/`remove`/`update`/`why` take a
    // dep-spec/path as their first positional, an optional manifest-or-dir
    // as their second; `lock`/`fetch` take only the (optional)
    // manifest-or-dir.
    if (sub == "add" || sub == "remove" || sub == "update" || sub == "lock" ||
        sub == "fetch" || sub == "why" || sub == "audit" || sub == "vendor") {
        std::string posArgs[2];
        int posCount = 0;
        std::string asName;
        bool dev = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--as" && i + 1 < argc) asName = argv[++i];
            else if (a == "--dev") dev = true;
            else if (!a.empty() && a[0] != '-') {
                if (posCount < 2) posArgs[posCount++] = a;
                else { std::fprintf(stderr, "error: too many arguments\n"); return 2; }
            } else { std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str()); return 2; }
        }

        if (sub == "add") {
            if (posCount < 1) {
                std::fprintf(stderr,
                    "usage: %s add <path>[@version] [--as <name>] [--dev] "
                    "[manifest-or-dir]\n", argv[0]);
                return 2;
            }
            return cmdAdd(posArgs[1], posArgs[0], asName, dev);
        }
        if (sub == "remove") {
            if (posCount < 1) {
                std::fprintf(stderr, "usage: %s remove <path> [manifest-or-dir]\n", argv[0]);
                return 2;
            }
            return cmdRemove(posArgs[1], posArgs[0]);
        }
        if (sub == "update") return cmdUpdate(posArgs[1], posArgs[0]);
        if (sub == "lock") return cmdLock(posArgs[0]);
        if (sub == "fetch") return cmdFetch(posArgs[0]);
        if (sub == "why") {
            if (posCount < 1) {
                std::fprintf(stderr, "usage: %s why <path> [manifest-or-dir]\n", argv[0]);
                return 2;
            }
            return cmdWhy(posArgs[1], posArgs[0]);
        }
        if (sub == "audit") return cmdAudit(posArgs[0]);
        if (sub == "vendor") return cmdVendor(posArgs[0]);
    }

    if (sub != "build" && sub != "run" && sub != "check" && sub != "emit-llvm" &&
        sub != "plan") {
        printUsage(argv[0]);
        return 2;
    }

    Options o;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) o.outOverride = argv[++i];
        else if (a == "--target" && i + 1 < argc) o.targetTriple = argv[++i];
        else if (a == "--opt-level" && i + 1 < argc) o.optLevel = std::atoi(argv[++i]);
        else if (a == "--plan" && i + 1 < argc) o.planOverride = argv[++i];
        else if (a == "--leviathan" && i + 1 < argc) o.leviathanOverride = argv[++i];
        else if (a == "--release") o.optLevel = 2;
        else if (a == "--vendor") o.useVendor = true;
        else if (!a.empty() && a[0] != '-') o.manifestArg = a;
        else { std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str()); return 2; }
    }

    return dispatch(sub, o);
}
