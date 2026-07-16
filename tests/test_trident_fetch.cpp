// Unit tests for tools/trident/{vcs,fetch}.cpp — the GitProvider
// (techdesign-package-manager.md §5.3 P2.1c). Runs entirely against a local
// bare git repo fixture (tests/trident/store/fixture_json.git, H-7 — no test
// may reach the real network): GitProvider::versions() lists its tags,
// manifestOf() reads a tagged manifest, materialize() populates the
// content-addressed store with the expected hash and is a cache hit on
// re-fetch, and MVS (P2.1a) runs end-to-end over the GitProvider instead of
// the fake. Minimal offline test harness, matching tests/test_lexer.cpp.
#include "../tools/trident/fetch.hpp"
#include "../tools/trident/mvs.hpp"
#include "../tools/trident/semver.hpp"
#include "../tools/trident/vcs.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifndef LANG_SOURCE_DIR
#error "LANG_SOURCE_DIR must be defined (see CMakeLists.txt)"
#endif

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

std::string fixtureRepo() {
    return std::string(LANG_SOURCE_DIR) + "/tests/trident/store/fixture_json.git";
}

}  // namespace

static void test_find_git() {
    CHECK(!findGit().empty());
}

static void test_list_tags() {
    std::vector<std::string> tags;
    std::string err;
    CHECK(gitListTags(fixtureRepo(), tags, err));
    CHECK(tags.size() == 2);
    bool has1 = false, has2 = false;
    for (const std::string& t : tags) {
        if (t == "v1.0.0") has1 = true;
        if (t == "v1.1.0") has2 = true;
    }
    CHECK(has1 && has2);
}

static void test_provider_versions() {
    GitProvider provider;
    ModuleId mod{fixtureRepo(), 1};
    std::vector<Version> versions;
    std::string err;
    CHECK(provider.versions(mod, versions, err));
    CHECK(versions.size() == 2);
}

static void test_manifest_of() {
    GitProvider provider;
    ModuleId mod{fixtureRepo(), 1};
    ProjectManifest pm;
    std::string err;
    CHECK(provider.manifestOf(mod, Version{1, 0, 0}, pm, err));
    CHECK(pm.name == "fixture_json");
    CHECK(pm.sources.size() == 1 && pm.sources[0] == "json.lev");
}

static void test_manifest_of_missing_version_is_a_clean_error() {
    GitProvider provider;
    ModuleId mod{fixtureRepo(), 1};
    ProjectManifest pm;
    std::string err;
    CHECK(!provider.manifestOf(mod, Version{9, 9, 9}, pm, err));
    CHECK(!err.empty());
}

static void test_materialize_and_idempotent_refetch() {
    char tmpl[] = "/tmp/trident_fetch_test_XXXXXX";
    char* tmp = ::mkdtemp(tmpl);
    CHECK(tmp != nullptr);
    ::setenv("TRIDENT_HOME", (std::string(tmp) + "/home").c_str(), 1);

    GitProvider provider;
    ModuleId mod{fixtureRepo(), 1};

    std::string dir1, hash1, err1;
    CHECK(provider.materialize(mod, Version{1, 0, 0}, dir1, hash1, err1));
    CHECK(!hash1.empty());

    std::ifstream f(dir1 + "/json.lev");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content.find("public string encode") != std::string::npos);

    // Re-fetch (same version): idempotent cache hit, identical hash/dir.
    std::string dir2, hash2, err2;
    CHECK(provider.materialize(mod, Version{1, 0, 0}, dir2, hash2, err2));
    CHECK(dir2 == dir1 && hash2 == hash1);

    // A different tagged version materializes to a DIFFERENT hash (its
    // json.lev differs — v1.1.0 adds a version() function).
    std::string dir3, hash3, err3;
    CHECK(provider.materialize(mod, Version{1, 1, 0}, dir3, hash3, err3));
    CHECK(hash3 != hash1);

    std::string rmCmd = "rm -rf '" + std::string(tmp) + "'";
    std::system(rmCmd.c_str());
}

static void test_mvs_end_to_end_over_git_provider() {
    GitProvider provider;
    Require root;
    root.mod = ModuleId{fixtureRepo(), 1};
    root.min = Version{1, 0, 0};

    MvsResult res = selectVersions({root}, provider);
    CHECK(res.ok);
    CHECK(res.buildList.size() == 1);
    if (!res.buildList.empty()) {
        CHECK(compareSemVer(res.buildList[0].selected, Version{1, 0, 0}) == 0);
    }
}

int main() {
    test_find_git();
    test_list_tags();
    test_provider_versions();
    test_manifest_of();
    test_manifest_of_missing_version_is_a_clean_error();
    test_materialize_and_idempotent_refetch();
    test_mvs_end_to_end_over_git_provider();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
