#include "../tools/trident/index.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static int checks = 0;
static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (!(condition)) {                                                 \
            ++failures;                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                   \
    } while (0)

int main() {
    char tmpl[] = "/tmp/trident_index_test_XXXXXX";
    char* root = ::mkdtemp(tmpl);
    CHECK(root != nullptr);
    if (!root) return 1;

    std::string endpoint = root;
    std::string err;
    CHECK(indexRegisterName(endpoint, "json", "github.com/acme/json", err));

    std::string path;
    bool found = false;
    CHECK(indexResolveName(endpoint, "json", path, found, err));
    CHECK(found);
    CHECK(path == "github.com/acme/json");

    // Same registration is retry-safe; a different owner cannot replace it.
    CHECK(indexRegisterName(endpoint, "json", "github.com/acme/json", err));
    std::string conflictErr;
    CHECK(!indexRegisterName(endpoint, "json", "github.com/evil/json", conflictErr));
    CHECK(conflictErr.find("already registered") != std::string::npos);
    CHECK(indexResolveName(endpoint, "json", path, found, err));
    CHECK(path == "github.com/acme/json");

    CHECK(isFriendlyPackageName("json-core"));
    CHECK(!isFriendlyPackageName("github.com/acme/json"));

    std::string cleanup = "rm -rf '" + endpoint + "'";
    std::system(cleanup.c_str());
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
