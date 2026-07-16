// Unit tests for tools/trident/lock.cpp — the lockfile (techdesign-package-
// manager.md §5.4 P2.1d). Covers: round-trip (write -> read -> identical
// build list), a stale lock (manifest dep bumped, lock not regenerated) is
// rejected with the fix message, a fresh `trident lock` regenerates
// deterministically (byte-identical on repeat), and the §0.5 module-identity
// text (bare path for major<=1, "path@vN" for major>=2). Minimal offline
// test harness, matching tests/test_lexer.cpp.
#include "../tools/trident/lock.hpp"
#include "../tools/trident/provider.hpp"
#include "../tools/trident/semver.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>

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

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<BuildListEntry> sampleBuildList() {
    BuildListEntry json;
    json.mod = ModuleId{"github.com/thaden0/json", 1};
    json.selected = Version{1, 2, 0};
    json.contentHash = "9f3c1a";
    json.storeDir = "/fake/store/9f3c1a";

    BuildListEntry http;
    http.mod = ModuleId{"github.com/acme/http", 1};
    http.selected = Version{0, 4, 1};
    http.contentHash = "be21d0";
    http.storeDir = "/fake/store/be21d0";
    Require req;
    req.mod = json.mod;
    req.min = Version{1, 1, 0};   // http needs 1.1.0; root needs 1.2.0 -> MVS picked 1.2.0
    http.requires_.push_back(req);

    BuildListEntry jsonV2;
    jsonV2.mod = ModuleId{"github.com/thaden0/json", 2};
    jsonV2.selected = Version{2, 0, 0};
    jsonV2.contentHash = "aabbcc";
    jsonV2.storeDir = "/fake/store/aabbcc";

    return {json, http, jsonV2};   // already in ModuleId order, as MVS would emit
}

}  // namespace

static void test_module_id_text() {
    CHECK(serializeModuleId(ModuleId{"github.com/x/a", 1}) == "github.com/x/a");
    CHECK(serializeModuleId(ModuleId{"github.com/x/a", 2}) == "github.com/x/a@v2");

    ModuleId a, b;
    CHECK(parseModuleId("github.com/x/a", a) && a.path == "github.com/x/a" && a.major == 1);
    CHECK(parseModuleId("github.com/x/a@v2", b) && b.path == "github.com/x/a" && b.major == 2);
}

static void test_lockfile_path_for() {
    CHECK(lockfilePathFor("trident.toml") == "trident.lock");
    CHECK(lockfilePathFor("/a/b/foo.toml") == "/a/b/foo.lock");
    CHECK(lockfilePathFor("proj/trident.toml") == "proj/trident.lock");
}

static void test_round_trip() {
    std::vector<BuildListEntry> buildList = sampleBuildList();
    Lockfile lock = lockfileFromBuildList(buildList);
    CHECK(lock.version == 1);
    CHECK(lock.modules.size() == 3);

    char tmpl[] = "/tmp/trident_lock_test_XXXXXX";
    int fd = ::mkstemp(tmpl);
    CHECK(fd >= 0);
    if (fd >= 0) ::close(fd);
    std::string path = tmpl;

    std::string err;
    CHECK(writeLockfile(path, lock, err));

    std::string text = readFile(path);
    Lockfile reread;
    CHECK(parseLockfile(path, text, reread, err));
    CHECK(reread.version == lock.version);
    CHECK(reread.modules.size() == lock.modules.size());
    for (size_t i = 0; i < lock.modules.size() && i < reread.modules.size(); ++i) {
        CHECK(reread.modules[i].mod.path == lock.modules[i].mod.path);
        CHECK(reread.modules[i].mod.major == lock.modules[i].mod.major);
        CHECK(reread.modules[i].selected == lock.modules[i].selected);
        CHECK(reread.modules[i].hash == lock.modules[i].hash);
        CHECK(reread.modules[i].requires_ == lock.modules[i].requires_);
    }

    // The re-parsed lock matches the ORIGINAL build list it came from.
    CHECK(lockfileMatchesBuildList(reread, buildList, err));

    ::remove(path.c_str());
}

static void test_deterministic_regeneration() {
    std::vector<BuildListEntry> buildList = sampleBuildList();
    Lockfile lock1 = lockfileFromBuildList(buildList);
    Lockfile lock2 = lockfileFromBuildList(buildList);

    char t1[] = "/tmp/trident_lock_test_a_XXXXXX";
    char t2[] = "/tmp/trident_lock_test_b_XXXXXX";
    int fd1 = ::mkstemp(t1), fd2 = ::mkstemp(t2);
    if (fd1 >= 0) ::close(fd1);
    if (fd2 >= 0) ::close(fd2);

    std::string err;
    CHECK(writeLockfile(t1, lock1, err));
    CHECK(writeLockfile(t2, lock2, err));
    CHECK(readFile(t1) == readFile(t2));

    ::remove(t1);
    ::remove(t2);
}

static void test_stale_lock_is_a_clean_error_naming_the_fix() {
    std::vector<BuildListEntry> buildList = sampleBuildList();
    Lockfile lock = lockfileFromBuildList(buildList);

    // Simulate the manifest being edited (json bumped to 1.3.0) without
    // re-running `trident lock`.
    std::vector<BuildListEntry> bumped = buildList;
    bumped[0].selected = Version{1, 3, 0};

    std::string err;
    CHECK(!lockfileMatchesBuildList(lock, bumped, err));
    CHECK(err.find("trident lock") != std::string::npos);

    // A module added/removed is also stale.
    std::vector<BuildListEntry> truncated = buildList;
    truncated.pop_back();
    std::string err2;
    CHECK(!lockfileMatchesBuildList(lock, truncated, err2));
    CHECK(err2.find("trident lock") != std::string::npos);

    // An unchanged build list matches.
    std::string err3;
    CHECK(lockfileMatchesBuildList(lock, buildList, err3));
}

int main() {
    test_module_id_text();
    test_lockfile_path_for();
    test_round_trip();
    test_deterministic_regeneration();
    test_stale_lock_is_a_clean_error_naming_the_fix();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
