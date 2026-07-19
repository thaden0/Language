// Unit tests for tools/trident/{semver,mvs}.cpp (techdesign-package-manager.md
// §5.1 P2.1a). Drives MVS against a fake, in-memory ModuleProvider — no I/O —
// covering: single dep, transitive chain, diamond (same major, higher min
// wins), two majors (distinct ids coexist), an unsatisfiable/missing version
// (clean error), and the external-require cycle policy. Also checks
// determinism (root-require order does not change the sorted build list) and
// the SemVer parser/comparator directly. Minimal offline test harness,
// matching tests/test_lexer.cpp.
#include "../tools/trident/manifest.hpp"
#include "../tools/trident/mvs.hpp"
#include "../tools/trident/provider.hpp"
#include "../tools/trident/semver.hpp"
#include <cstdio>
#include <map>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

namespace {

// A Vcs-kind dependency declaration, for building fake modules' manifests.
Dependency vcsDep(const std::string& path, const std::string& version,
                  const std::string& as = "") {
    Dependency d;
    d.path = path;
    d.version = version;
    d.as_ = as;
    d.kind = DepKind::Vcs;
    return d;
}

// An in-memory fake ModuleProvider (no I/O): each module@version is a canned
// list of its own Vcs deps, keyed by "path@MAJOR.MINOR.PATCH".
struct FakeProvider : ModuleProvider {
    std::map<std::string, std::vector<Dependency>> byKey;

    static std::string key(const std::string& path, const Version& v) {
        return path + "@" + formatSemVer(v);
    }

    void add(const std::string& path, const std::string& version,
            std::vector<Dependency> deps) {
        Version v;
        std::string err;
        if (!parseSemVer(version, v, err)) { std::printf("bad fixture version: %s\n", err.c_str()); return; }
        byKey[key(path, v)] = std::move(deps);
    }

    bool manifestOf(const ModuleId& mod, const Version& version, ProjectManifest& out,
                    std::string& err) override {
        auto it = byKey.find(key(mod.path, version));
        if (it == byKey.end()) {
            err = "no such version " + formatSemVer(version) + " for module '" + mod.path + "'";
            return false;
        }
        out = ProjectManifest{};
        out.deps = it->second;
        return true;
    }

    bool materialize(const ModuleId& mod, const Version&, std::string& storeDir,
                     std::string& contentHash, std::string&) override {
        storeDir = "fake:" + mod.path;
        contentHash = "sha256:fake";
        return true;
    }

    bool versions(const ModuleId&, std::vector<Version>&, std::string&) override {
        return true;
    }
};

Require req(const std::string& path, const std::string& version) {
    Dependency d = vcsDep(path, version);
    Require r;
    std::string err;
    bool ok = dependencyToRequire(d, r, err);
    if (!ok) std::printf("bad fixture require: %s\n", err.c_str());
    return r;
}

const BuildListEntry* findEntry(const MvsResult& res, const std::string& path, int major) {
    for (const BuildListEntry& e : res.buildList)
        if (e.mod.path == path && e.mod.major == major) return &e;
    return nullptr;
}

}  // namespace

static void test_semver_parse_and_compare() {
    Version v;
    std::string err;
    CHECK(parseSemVer("1.2.3", v, err) && v.major == 1 && v.minor == 2 && v.patch == 3);
    CHECK(parseSemVer("v1.2.3", v, err) && v.major == 1 && v.minor == 2 && v.patch == 3);
    CHECK(!parseSemVer("1.2", v, err));
    CHECK(!parseSemVer("abc", v, err));
    CHECK(!parseSemVer("1.2.3.4", v, err));

    Version a{1, 2, 3}, b{1, 2, 4}, c{2, 0, 0};
    CHECK(compareSemVer(a, b) < 0);
    CHECK(compareSemVer(b, a) > 0);
    CHECK(compareSemVer(a, a) == 0);
    CHECK(compareSemVer(a, c) < 0);
    CHECK(formatSemVer(Version{1, 2, 3}) == "1.2.3");
    CHECK(formatSemVerTag(Version{1, 2, 3}) == "v1.2.3");
}

static void test_single_dep() {
    FakeProvider p;
    p.add("github.com/x/a", "1.0.0", {});

    MvsResult res = selectVersions({req("github.com/x/a", "1.0.0")}, p);
    CHECK(res.ok);
    CHECK(res.buildList.size() == 1);
    const BuildListEntry* a = findEntry(res, "github.com/x/a", 1);
    CHECK(a && compareSemVer(a->selected, Version{1, 0, 0}) == 0);
}

static void test_major_0_and_1_share_one_identity() {
    // techdesign-package-manager.md §0.5: "major <= 1 => key path" — a 0.x
    // package and its 1.x successor are the SAME module identity (only
    // major 2+ needs a distinct id), so MVS must select the higher of the
    // two as one entry, not coexist as two.
    FakeProvider p;
    p.add("github.com/x/a", "0.9.0", {});
    p.add("github.com/x/a", "1.2.0", {});

    MvsResult res = selectVersions(
        {req("github.com/x/a", "0.9.0"), req("github.com/x/a", "1.2.0")}, p);
    CHECK(res.ok);
    CHECK(res.buildList.size() == 1);
    const BuildListEntry* a = findEntry(res, "github.com/x/a", 1);
    CHECK(a && compareSemVer(a->selected, Version{1, 2, 0}) == 0);
}

static void test_transitive_chain() {
    FakeProvider p;
    p.add("github.com/x/a", "1.0.0", {vcsDep("github.com/x/b", "2.0.0")});
    p.add("github.com/x/b", "2.0.0", {});

    MvsResult res = selectVersions({req("github.com/x/a", "1.0.0")}, p);
    CHECK(res.ok);
    CHECK(res.buildList.size() == 2);
    const BuildListEntry* a = findEntry(res, "github.com/x/a", 1);
    const BuildListEntry* b = findEntry(res, "github.com/x/b", 2);
    CHECK(a && compareSemVer(a->selected, Version{1, 0, 0}) == 0);
    CHECK(b && compareSemVer(b->selected, Version{2, 0, 0}) == 0);
}

static void test_diamond_same_major_higher_min_wins() {
    FakeProvider p;
    // root requires A>=1.1.0 directly, and B>=1.0.0; B itself requires
    // A>=1.5.0. MVS must select the higher of the two mins for A.
    p.add("github.com/x/a", "1.1.0", {});
    p.add("github.com/x/a", "1.5.0", {});
    p.add("github.com/x/b", "1.0.0", {vcsDep("github.com/x/a", "1.5.0")});

    MvsResult res = selectVersions(
        {req("github.com/x/a", "1.1.0"), req("github.com/x/b", "1.0.0")}, p);
    CHECK(res.ok);
    const BuildListEntry* a = findEntry(res, "github.com/x/a", 1);
    const BuildListEntry* b = findEntry(res, "github.com/x/b", 1);
    CHECK(a && compareSemVer(a->selected, Version{1, 5, 0}) == 0);
    CHECK(b && compareSemVer(b->selected, Version{1, 0, 0}) == 0);
}

static void test_two_majors_coexist() {
    FakeProvider p;
    p.add("github.com/x/json", "1.2.0", {});
    p.add("github.com/x/json", "2.0.0", {});

    MvsResult res = selectVersions(
        {req("github.com/x/json", "1.2.0"), req("github.com/x/json", "2.0.0")}, p);
    CHECK(res.ok);
    CHECK(res.buildList.size() == 2);   // two distinct ModuleIds, not a version conflict
    const BuildListEntry* v1 = findEntry(res, "github.com/x/json", 1);
    const BuildListEntry* v2 = findEntry(res, "github.com/x/json", 2);
    CHECK(v1 && compareSemVer(v1->selected, Version{1, 2, 0}) == 0);
    CHECK(v2 && compareSemVer(v2->selected, Version{2, 0, 0}) == 0);
}

static void test_missing_version_is_a_clean_error() {
    FakeProvider p;
    p.add("github.com/x/a", "1.0.0", {});   // only 1.0.0 exists

    MvsResult res = selectVersions({req("github.com/x/a", "9.9.9")}, p);
    CHECK(!res.ok);
    CHECK(!res.err.empty());
    CHECK(res.err.find("github.com/x/a") != std::string::npos);
}

static void test_deterministic_regardless_of_root_order() {
    FakeProvider p;
    p.add("github.com/x/a", "1.0.0", {});
    p.add("github.com/x/b", "1.0.0", {});
    p.add("github.com/x/c", "1.0.0", {});

    std::vector<Require> order1 = {req("github.com/x/a", "1.0.0"),
                                   req("github.com/x/b", "1.0.0"),
                                   req("github.com/x/c", "1.0.0")};
    std::vector<Require> order2 = {req("github.com/x/c", "1.0.0"),
                                   req("github.com/x/a", "1.0.0"),
                                   req("github.com/x/b", "1.0.0")};

    MvsResult r1 = selectVersions(order1, p);
    MvsResult r2 = selectVersions(order2, p);
    CHECK(r1.ok && r2.ok);
    CHECK(r1.buildList.size() == r2.buildList.size());
    for (size_t i = 0; i < r1.buildList.size() && i < r2.buildList.size(); ++i) {
        CHECK(r1.buildList[i].mod.path == r2.buildList[i].mod.path);
        CHECK(r1.buildList[i].mod.major == r2.buildList[i].mod.major);
    }
}

static void test_direct_cycle_is_a_hard_error() {
    FakeProvider p;
    p.add("github.com/x/a", "1.2.0", {vcsDep("github.com/x/b", "1.0.0")});
    p.add("github.com/x/b", "1.0.0", {vcsDep("github.com/x/a", "1.0.0")});

    MvsResult res = selectVersions({req("github.com/x/a", "1.2.0")}, p);
    CHECK(!res.ok);
    CHECK(res.err ==
          "require cycle detected: github.com/x/a@1.2.0 -> github.com/x/b@1.0.0 -> "
          "github.com/x/a@1.2.0 — the external dependency graph must be acyclic; break "
          "the cycle by removing one of these modules' requires on the other");
}

static void test_self_require_is_a_hard_error() {
    FakeProvider sameMajor;
    sameMajor.add("github.com/x/a", "1.2.0", {vcsDep("github.com/x/a", "1.1.0")});

    MvsResult direct = selectVersions({req("github.com/x/a", "1.2.0")}, sameMajor);
    CHECK(!direct.ok);
    CHECK(direct.err.find("github.com/x/a@1.2.0 -> github.com/x/a@1.2.0") !=
          std::string::npos);

    // 0.x and 1.x deliberately share one ModuleId. Once 1.1.0 wins, its
    // require on 0.9.0 is therefore a one-node cycle too.
    FakeProvider sharedIdentity;
    sharedIdentity.add("github.com/x/a", "0.9.0", {vcsDep("github.com/x/a", "1.1.0")});
    sharedIdentity.add("github.com/x/a", "1.1.0", {vcsDep("github.com/x/a", "0.9.0")});

    MvsResult bucketed = selectVersions({req("github.com/x/a", "0.9.0")}, sharedIdentity);
    CHECK(!bucketed.ok);
    CHECK(bucketed.err.find("github.com/x/a@1.1.0 -> github.com/x/a@1.1.0") !=
          std::string::npos);
}

static void test_one_way_cross_major_is_legal() {
    FakeProvider p;
    p.add("github.com/x/a", "1.5.0", {});
    p.add("github.com/x/a", "2.0.0", {vcsDep("github.com/x/a", "1.5.0")});

    MvsResult res = selectVersions({req("github.com/x/a", "2.0.0")}, p);
    CHECK(res.ok);
    CHECK(res.buildList.size() == 2);
    CHECK(findEntry(res, "github.com/x/a", 1) != nullptr);
    CHECK(findEntry(res, "github.com/x/a", 2) != nullptr);
}

static void test_mutual_cross_major_is_a_hard_error() {
    FakeProvider p;
    p.add("github.com/x/a", "1.5.0", {vcsDep("github.com/x/a", "2.0.0")});
    p.add("github.com/x/a", "2.0.0", {vcsDep("github.com/x/a", "1.5.0")});

    MvsResult res = selectVersions({req("github.com/x/a", "1.5.0")}, p);
    CHECK(!res.ok);
    CHECK(res.err.find("github.com/x/a@1.5.0 -> github.com/x/a@2.0.0 -> "
                       "github.com/x/a@1.5.0") != std::string::npos);
}

static void test_dangling_edge_is_a_leaf_not_a_cycle() {
    BuildListEntry a;
    a.mod = ModuleId{"github.com/x/a", 1};
    a.selected = Version{1, 0, 0};
    a.requires_.push_back(req("github.com/x/missing", "1.0.0"));

    CHECK(findRequireCycle({a}).empty());
}

static void test_cycle_through_dev_edge_is_a_hard_error() {
    FakeProvider p;
    Dependency devEdge = vcsDep("github.com/x/b", "1.0.0");
    devEdge.dev = true;
    p.add("github.com/x/a", "1.0.0", {devEdge});
    p.add("github.com/x/b", "1.0.0", {vcsDep("github.com/x/a", "1.0.0")});

    MvsResult res = selectVersions({req("github.com/x/a", "1.0.0")}, p);
    CHECK(!res.ok);
    CHECK(res.err.find("require cycle detected") != std::string::npos);
    CHECK(res.err.find("github.com/x/a@1.0.0 -> github.com/x/b@1.0.0 -> "
                       "github.com/x/a@1.0.0") != std::string::npos);
}

int main() {
    test_semver_parse_and_compare();
    test_single_dep();
    test_major_0_and_1_share_one_identity();
    test_transitive_chain();
    test_diamond_same_major_higher_min_wins();
    test_two_majors_coexist();
    test_missing_version_is_a_clean_error();
    test_deterministic_regardless_of_root_order();
    test_direct_cycle_is_a_hard_error();
    test_self_require_is_a_hard_error();
    test_one_way_cross_major_is_legal();
    test_mutual_cross_major_is_a_hard_error();
    test_dangling_edge_is_a_leaf_not_a_cycle();
    test_cycle_through_dev_edge_is_a_hard_error();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
