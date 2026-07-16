// Unit tests for tools/trident/{hash,store}.cpp (techdesign-package-
// manager.md §5.2 P2.1b). hash: sha256Hex against published NIST test
// vectors (empty string, "abc", the 448-bit multi-block vector, and the
// long "a"*1000000 vector). store: materializing the same content twice is a
// no-op, and two different orderings of the same files produce the same
// content hash (canonicalization holds). Minimal offline test harness,
// matching tests/test_lexer.cpp.
#include "../tools/trident/hash.hpp"
#include "../tools/trident/store.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
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

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

}  // namespace

static void test_sha256_nist_vectors() {
    CHECK(sha256Hex(std::string("")) ==
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex(std::string("abc")) ==
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex(std::string(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    std::string millionAs(1000000, 'a');
    CHECK(sha256Hex(millionAs) ==
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_store_idempotent_and_order_independent() {
    char tmpl[] = "/tmp/trident_store_test_XXXXXX";
    char* tmp = ::mkdtemp(tmpl);
    CHECK(tmp != nullptr);
    std::string base = tmp;
    ::setenv("TRIDENT_HOME", (base + "/home").c_str(), 1);

    std::string srcDir = base + "/src";
    ::mkdir(srcDir.c_str(), 0755);
    writeFile(srcDir + "/a.lev", "namespace A {}\n");
    writeFile(srcDir + "/b.lev", "namespace B {}\n");

    std::vector<StoreFile> files1 = {
        {"a.lev", srcDir + "/a.lev"},
        {"b.lev", srcDir + "/b.lev"},
    };
    std::vector<StoreFile> files2 = {   // same files, reversed order
        {"b.lev", srcDir + "/b.lev"},
        {"a.lev", srcDir + "/a.lev"},
    };

    std::string dir1, hash1, err1;
    bool ok1 = materializeToStore(files1, dir1, hash1, err1);
    CHECK(ok1);

    std::string dir2, hash2, err2;
    bool ok2 = materializeToStore(files2, dir2, hash2, err2);
    CHECK(ok2);

    CHECK(hash1 == hash2);              // canonicalization is order-independent
    CHECK(dir1 == dir2);
    CHECK(dir1 == storeRoot() + "/" + hash1);

    // The store entry actually holds both files with their right content.
    std::ifstream a(dir1 + "/a.lev");
    std::string aContent((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
    CHECK(aContent == "namespace A {}\n");

    // A second materialize (already present) is a no-op cache hit: same
    // hash/dir, and it must not error even though the store entry now
    // exists.
    std::string dir3, hash3, err3;
    bool ok3 = materializeToStore(files1, dir3, hash3, err3);
    CHECK(ok3 && hash3 == hash1 && dir3 == dir1);

    // Different content hashes differently.
    writeFile(srcDir + "/a.lev", "namespace A2 {}\n");
    std::vector<StoreFile> filesChanged = {
        {"a.lev", srcDir + "/a.lev"},
        {"b.lev", srcDir + "/b.lev"},
    };
    std::string dir4, hash4, err4;
    bool ok4 = materializeToStore(filesChanged, dir4, hash4, err4);
    CHECK(ok4 && hash4 != hash1);

    std::string rmCmd = "rm -rf '" + base + "'";
    std::system(rmCmd.c_str());
}

int main() {
    test_sha256_nist_vectors();
    test_store_idempotent_and_order_independent();

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
