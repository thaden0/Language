// Focused parser tests: assert the AST dump contains the structures that
// encode the language's key decisions.
#include "AstPrinter.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include <cstdio>
#include <string>

static int g_checks = 0;
static int g_failures = 0;

// Parse `src` and return its AST dump; fail the check if parsing had errors.
static std::string ast(const std::string& src) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    Parser parser(lexer.tokenize(), f, sink);
    std::string out = printProgram(parser.parseProgram());
    if (sink.hasErrors()) out += "\n<HAD ERRORS>";
    return out;
}

#define CONTAINS(src, needle)                                                  \
    do {                                                                       \
        ++g_checks;                                                            \
        std::string dump = ast(src);                                          \
        if (dump.find(needle) == std::string::npos) {                        \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d\n    wanted substring: %s\n    in:\n%s\n", \
                        __FILE__, __LINE__, needle, dump.c_str());            \
        }                                                                      \
    } while (0)

#define CLEAN(src)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        std::string dump = ast(src);                                          \
        if (dump.find("<HAD ERRORS>") != std::string::npos) {                \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d: unexpected parse error in: %s\n",      \
                        __FILE__, __LINE__, src);                            \
        }                                                                      \
    } while (0)

int main() {
    // Operator members: the "()" selector, not a name.
    CONTAINS("class C { C (+)(int v) => v; }", "Operator (+)(int v) : C");
    CONTAINS("class C { bool (==)(int v) => true; }", "Operator (==)(int v) : bool");

    // Body-is-one-statement, all forms.
    CONTAINS("int f() => 1;", "Method f() : int\n    Return 1");
    CONTAINS("int f() return 1;", "Method f() : int\n    Return 1");
    CONTAINS("void f() { }", "Method f() : void\n    Block");
    CONTAINS("void f() console.writeln(1);", "Expr console.writeln(1)");

    // Constructors: 'new' + label; label need not match the class.
    CONTAINS("class C { new C() { } new Other(int x) { } }", "Ctor C()");
    CONTAINS("class C { new Other(int x) { } }", "Ctor Other(int x)");

    // distinct field, union type, generics, interface.
    CONTAINS("class C { distinct int value = 0; }", "Field distinct value : int = 0");
    CONTAINS("int | string f() => 1;", ": int | string");
    CONTAINS("class W<T> : A, B { }", "Class W<T> : A, B");
    CONTAINS("interface I { int x; string name(); }", "Interface I");

    // :: qualification chains and label selection.
    CONTAINS("void f() { C::Ctor(); }", "C::Ctor()");
    CONTAINS("void f() { Type t = Type::Label(1); }", "Type::Label(1)");

    // Stream operators, including the empty-rhs extract from the demo.
    CONTAINS("void f() { w << 42; }", "(w << 42)");
    CONTAINS("void f() { int x = r >> ; }", "(r >>)");

    // Ternary, lambda, array, await, inject.
    CONTAINS("int f() => a ? b : c;", "(a ? b : c)");
    CONTAINS("void f() { var g = xs.where((el) => el % 2 == 0); }",
             "(el) => ((el % 2) == 0)");
    CONTAINS("void f() { var a = [1, 2, 3]; }", "[1, 2, 3]");
    CONTAINS("void f() { int x = await g(); }", "await g()");
    CONTAINS("void f() { greet(inject ILogger); }", "greet(inject ILogger)");

    // DI: factory bind (arrow) vs object install.
    CONTAINS("bind ILogger => ConsoleLogger();", "Bind ILogger =>");
    CONTAINS("void f() { bind b; }", "Bind (object) b");

    // Same type name in two namespaces — must both parse, no collision.
    CLEAN("namespace A { class W { } } namespace B { class W { } }");

    // Class-level sectional access, nested namespace, and a top-level call.
    CONTAINS("class C { public: int f() => 1; }", "public Method f() : int");
    CLEAN("namespace N { int f() => 1; } N::f();");

    // imports.md: selective `use` — path, `as` alias (a contextual keyword,
    // reference §1.5's precedent), nested path, and block position.
    CONTAINS("use std::read;", "Use std::read");
    CONTAINS("use std::read as readMode;", "Use std::read as readMode");
    CONTAINS("use A::B;", "Use A::B");
    CONTAINS("use A::B as C;", "Use A::B as C");
    CLEAN("void f() { use std::read; }");
    CONTAINS("void f() { use std::read as rm; }", "Use std::read as rm");
    // `uses` (bulk) is unaffected by the `use` grammar change.
    CONTAINS("uses Lcurl;", "Uses Lcurl");
    CLEAN("uses A::B;");

    // Track 01 F4: string interpolation desugars to a `+`-chain of literal
    // segments and `(hole).toString()` calls, built in the parser.
    CLEAN("void f() { console.writeln(\"code=${1}!\"); }");
    CONTAINS("void f() { console.writeln(\"code=${1}!\"); }",
             "(code= + 1.toString())");
    CONTAINS("void f() { console.writeln(\"a${x}b${y}c\"); }",
             "x.toString()");
    // A member/call expression works as a hole, not just a bare name.
    CONTAINS("void f() { console.writeln(\"${resp.status}\"); }",
             "resp.status.toString()");
    // Zero holes: unaffected (still an ordinary StringLit — no `+` at all).
    CONTAINS("void f() { console.writeln(\"plain\"); }", "Expr console.writeln(plain)");
    // `\$` escapes a literal `$`, so `\${` never starts a hole.
    CLEAN("void f() { console.writeln(\"\\${not a hole}\"); }");
    // Empty and unterminated holes are compile errors (parser-level).
    CONTAINS("void f() { console.writeln(\"${}\"); }", "<HAD ERRORS>");
    CONTAINS("void f() { console.writeln(\"${\"); }", "<HAD ERRORS>");
    // A nested string (opposite quote style) containing `}` doesn't
    // prematurely close the hole.
    CLEAN("void f() { console.writeln(\"${'a } b'}\"); }");

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
