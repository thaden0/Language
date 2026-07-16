// Unit tests for tools/trident/checksum.cpp — the checksum DB (techdesign-
// package-manager.md §6 P2.2, GT4). Covers: first-fetch records a baseline,
// a repeat fetch with the same content verifies clean, a different content
// hash for an already-recorded module@version is rejected (moved tag /
// swapped content), two distinct module@versions coexist independently, and
// the on-disk hash chain detects tampering (an edited content hash, or a
// hand-edited/truncated log) even when a caller never touches those entries
// directly. Minimal offline test harness, matching test_trident_lock.cpp.
#include "../tools/trident/checksum.hpp"
#include "../tools/trident/provider.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <vector>

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

std::string mktempPath(const char* suffix) {
    std::string tmpl = std::string("/tmp/trident_checksum_test_") + suffix + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    CHECK(fd >= 0);
    if (fd >= 0) ::close(fd);
    std::string path(buf.data());
    ::remove(path.c_str());   // checksumDbVerifyOrRecord must create it fresh
    return path;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

static void test_first_fetch_records_baseline() {
    std::string db = mktempPath("first");
    ModuleId mod{"github.com/x/json", 1};
    Version v{1, 2, 0};
    std::string err;
    CHECK(checksumDbVerifyOrRecord(db, mod, v, "aaaa", err));
    CHECK(err.empty());

    std::string text = readFile(db);
    CHECK(!text.empty());
    CHECK(text.find("github.com/x/json") != std::string::npos);
    CHECK(text.find("aaaa") != std::string::npos);
    ::remove(db.c_str());
}

static void test_repeat_fetch_same_content_verifies_clean() {
    std::string db = mktempPath("repeat");
    ModuleId mod{"github.com/x/json", 1};
    Version v{1, 2, 0};
    std::string err;
    CHECK(checksumDbVerifyOrRecord(db, mod, v, "aaaa", err));
    std::string firstText = readFile(db);

    std::string err2;
    CHECK(checksumDbVerifyOrRecord(db, mod, v, "aaaa", err2));
    CHECK(err2.empty());
    // A pure verification (no new baseline) does not append anything.
    CHECK(readFile(db) == firstText);
    ::remove(db.c_str());
}

static void test_moved_tag_is_rejected() {
    std::string db = mktempPath("moved");
    ModuleId mod{"github.com/x/json", 1};
    Version v{1, 2, 0};
    std::string err;
    CHECK(checksumDbVerifyOrRecord(db, mod, v, "aaaa", err));

    std::string err2;
    CHECK(!checksumDbVerifyOrRecord(db, mod, v, "bbbb", err2));
    CHECK(err2.find("aaaa") != std::string::npos);
    CHECK(err2.find("bbbb") != std::string::npos);
    CHECK(err2.find("github.com/x/json") != std::string::npos);
    ::remove(db.c_str());
}

static void test_distinct_module_versions_coexist() {
    std::string db = mktempPath("coexist");
    std::string err;
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/json", 1}, Version{1, 2, 0},
                                   "aaaa", err));
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/json", 1}, Version{1, 3, 0},
                                   "cccc", err));
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/http", 1}, Version{0, 4, 1},
                                   "dddd", err));
    // Each is independently verifiable, unaffected by the others.
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/json", 1}, Version{1, 2, 0},
                                   "aaaa", err));
    std::string err2;
    CHECK(!checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/json", 1}, Version{1, 3, 0},
                                    "zzzz", err2));
    ::remove(db.c_str());
}

static void test_edited_content_hash_breaks_the_chain() {
    std::string db = mktempPath("edited");
    std::string err;
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/json", 1}, Version{1, 2, 0},
                                   "aaaa", err));
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/http", 1}, Version{0, 4, 1},
                                   "dddd", err));

    // Hand-edit the first record's content hash in place (same field width,
    // so the line structure otherwise still looks well-formed) — simulating
    // someone editing the log file directly rather than through this API.
    std::string text = readFile(db);
    size_t pos = text.find("aaaa");
    CHECK(pos != std::string::npos);
    if (pos != std::string::npos) text.replace(pos, 4, "eeee");
    writeFile(db, text);

    std::string err2;
    CHECK(!checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/http", 1}, Version{0, 4, 1},
                                    "dddd", err2));
    CHECK(err2.find("tamper") != std::string::npos);
    ::remove(db.c_str());
}

static void test_truncated_log_breaks_the_chain() {
    std::string db = mktempPath("truncated");
    std::string err;
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/json", 1}, Version{1, 2, 0},
                                   "aaaa", err));
    CHECK(checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/http", 1}, Version{0, 4, 1},
                                   "dddd", err));

    // Drop the first line — the second record's own prev-hash link now
    // points at an entry that no longer precedes it.
    std::string text = readFile(db);
    size_t nl = text.find('\n');
    CHECK(nl != std::string::npos);
    if (nl != std::string::npos) writeFile(db, text.substr(nl + 1));

    std::string err2;
    CHECK(!checksumDbVerifyOrRecord(db, ModuleId{"github.com/x/http", 1}, Version{0, 4, 1},
                                    "dddd", err2));
    CHECK(err2.find("chain") != std::string::npos);
    ::remove(db.c_str());
}

static void test_checksum_db_path_uses_trident_home() {
    ::setenv("TRIDENT_HOME", "/scratch/th", 1);
    CHECK(checksumDbPath() == "/scratch/th/checksum.db");
    ::unsetenv("TRIDENT_HOME");
}

int main() {
    test_first_fetch_records_baseline();
    test_repeat_fetch_same_content_verifies_clean();
    test_moved_tag_is_rejected();
    test_distinct_module_versions_coexist();
    test_edited_content_hash_breaks_the_chain();
    test_truncated_log_breaks_the_chain();
    test_checksum_db_path_uses_trident_home();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
