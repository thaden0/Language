// Resolver + shape tests: the collision rules are the heart of the language,
// so exercise collapse / distinct / coexist / diamond / interfaces directly.
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Resolver.hpp"
#include <cstdio>
#include <string>

static int g_checks = 0;
static int g_failures = 0;

struct Result {
    std::string shapes;
    bool hadError = false;
};

static Result resolve(const std::string& src) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    Parser parser(lexer.tokenize(), f, sink);
    Program prog = parser.parseProgram();
    Resolver r(f, sink);
    r.run(prog);
    return {r.dumpShapes(), sink.hasErrors()};
}

// The shape block for one class (between its header and the next "Shape ").
static std::string section(const std::string& dump, const std::string& cls) {
    std::string header = "Shape " + cls + "\n";
    size_t p = dump.find(header);
    if (p == std::string::npos) return "";
    size_t start = p + header.size();
    size_t next = dump.find("Shape ", start);
    return dump.substr(start, next == std::string::npos ? std::string::npos : next - start);
}

static int countOf(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = 0; (p = hay.find(needle, p)) != std::string::npos; p += needle.size()) ++n;
    return n;
}

static void report(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}
#define CHECK(cond) report((cond), #cond)

int main() {
    // Same name + same type, neither distinct -> collapse to ONE slot (later wins).
    {
        Result r = resolve("class A { int x; } class B { int x; } class C : A, B { }");
        std::string c = section(r.shapes, "C");
        CHECK(countOf(c, "x : int") == 1);
        CHECK(c.find("(from B)") != std::string::npos);   // later base wins
        CHECK(c.find("(from A)") == std::string::npos);
        CHECK(!r.hadError);
    }

    // Same name + same type, one side distinct -> keep BOTH, marked distinct.
    {
        Result r = resolve("class A { distinct int x; } class B { int x; } class C : A, B { }");
        std::string c = section(r.shapes, "C");
        CHECK(countOf(c, "x : int") == 2);
        CHECK(countOf(c, "distinct x : int") == 2);       // one degree marks the pair
        CHECK(c.find("(from A)") != std::string::npos);
        CHECK(c.find("(from B)") != std::string::npos);
    }

    // Same name, DIFFERENT type -> coexist, no collision, not distinct.
    {
        Result r = resolve("class A { int x; } class B { string x; } class C : A, B { }");
        std::string c = section(r.shapes, "C");
        CHECK(c.find("x : int") != std::string::npos);
        CHECK(c.find("x : string") != std::string::npos);
        CHECK(c.find("distinct") == std::string::npos);
        CHECK(!r.hadError);
    }

    // Diamond: shared non-distinct member collapses to ONE (IOStream/StreamBuffer case).
    {
        Result r = resolve("class Base { int s; } class L : Base { } "
                           "class R : Base { } class D : L, R { }");
        std::string d = section(r.shapes, "D");
        CHECK(countOf(d, "s : int") == 1);
        CHECK(!r.hadError);
    }

    // Method overloads (same name, different signature) coexist.
    {
        Result r = resolve("class A { int f(int a) => a; } class B { int f(string a) => 1; } "
                           "class C : A, B { }");
        std::string c = section(r.shapes, "C");
        CHECK(c.find("f : (int) -> int") != std::string::npos);
        CHECK(c.find("f : (string) -> int") != std::string::npos);
    }

    // Interface satisfaction: pass when the class declares the required members.
    {
        Result r = resolve("interface I { int x; string name(); } "
                           "class C : I { int x; string name() => x; }");
        CHECK(!r.hadError);
    }
    // F6's class-override non-goal: a narrowed class return does not override
    // or merge the base slot.  Both remain present in the derived shape.
    {
        Result r = resolve("class A { A self() => this; } class B : A { B self() => this; }");
        std::string b = section(r.shapes, "B");
        CHECK(countOf(b, "self : () -> A") == 1);
        CHECK(countOf(b, "self : () -> B") == 1);
        CHECK(!r.hadError);
    }
    // ...and fail (with a diagnostic) when a requirement is missing.
    {
        Result r = resolve("interface I { int x; } class C : I { }");
        CHECK(r.hadError);
    }
    // Two interfaces requiring the same field -> no collision (one declaring site).
    {
        Result r = resolve("interface I { int x; } interface J { int x; } "
                           "class C : I, J { int x; }");
        std::string c = section(r.shapes, "C");
        CHECK(countOf(c, "x : int") == 1);
        CHECK(!r.hadError);
    }

    // Duplicate class and unknown type are diagnosed.
    CHECK(resolve("class A { } class A { }").hadError);
    CHECK(resolve("class C { Nope x; }").hadError);

    // Same type name in two namespaces: no clash.
    CHECK(!resolve("namespace P { class W { } } namespace Q { class W { } }").hadError);

    // `uses` imports names so they resolve unqualified.
    CHECK(!resolve("namespace M { class W { } } uses M; void f() { W w = W(); }").hadError);
    CHECK(resolve("namespace M { class W { } } void f() { W w = W(); }").hadError);   // no import
    CHECK(resolve("uses Nope;").hadError);                                            // unknown namespace

    // imports.md: selective `use Path::name (as alias);` resolves a class
    // for type positions the same way `uses` does.
    CHECK(!resolve("namespace M { class W { } } use M::W; void f() { W w = W(); }").hadError);
    CHECK(!resolve("namespace M { class W { } } use M::W as X; void f() { X w = X(); }").hadError);
    CHECK(resolve("namespace M { class W { } } use M::Nope;").hadError);    // unknown member
    CHECK(resolve("use Nope::W;").hadError);                                // unknown namespace

    // Explicit call type arguments resolve in expression position, including
    // qualification and an enclosing class type parameter in a field init.
    CHECK(!resolve("namespace N { class Item { } } class Box<T> { } "
                   "void f() { Box<N::Item> b = Box::<N::Item>(); }").hadError);
    CHECK(!resolve("class Box<T> { } class Holder<T> { "
                   "Box<T> value = Box::<T>(); }").hadError);
    CHECK(resolve("T id<T>(T value) => value; "
                  "void f() { id::<Missing>(1); }").hadError);

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
