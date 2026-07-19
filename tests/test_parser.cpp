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

static std::string parseErrors(const std::string& src) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    Parser parser(lexer.tokenize(), f, sink);
    parser.parseProgram();
    std::string out;
    for (const Diagnostic& d : sink.all()) {
        if (!out.empty()) out += "\n";
        out += d.message;
    }
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

#define ERROR_CONTAINS(src, needle)                                           \
    do {                                                                       \
        ++g_checks;                                                            \
        std::string errors = parseErrors(src);                                 \
        if (errors.find(needle) == std::string::npos) {                        \
            ++g_failures;                                                      \
            std::printf("  FAIL %s:%d\n    wanted error: %s\n    in:\n%s\n",  \
                        __FILE__, __LINE__, needle, errors.c_str());            \
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

    // techdesign-labeled-break-continue.md F1/F2: labeled loops + labeled
    // break/continue, all four loop kinds, dump ` label=<x>` on both the
    // loop's own line and the break/continue's line.
    CONTAINS("void f() { outer: while (true) { break outer; } }", "While (true) label=outer");
    CONTAINS("void f() { outer: while (true) { break outer; } }", "Break label=outer");
    CONTAINS("void f() { outer: do { continue outer; } while (true); }", "DoWhile (true) label=outer");
    CONTAINS("void f() { outer: do { continue outer; } while (true); }", "Continue label=outer");
    CONTAINS("void f() { outer: for (int i = 0; i < 5; i += 1) { break outer; } }", "label=outer");
    CONTAINS("void f() { outer: for (int i = 0; i < 5; i += 1) { break outer; } }", "Break label=outer");
    CONTAINS("void f() { outer: for (int x in 1..5) { continue outer; } }", "label=outer");
    CONTAINS("void f() { outer: for (int x in 1..5) { continue outer; } }", "Continue label=outer");
    // Unlabeled break/continue print no label suffix at all (regression).
    CONTAINS("void f() { while (true) { break; } }", "Break\n");
    // A parse-error case as a golden CLEAN-negative: `foo: return;` — the
    // label guard's three-token lookahead (Identifier, Colon, loop-kw)
    // requires a LOOP keyword after the colon; `return` isn't one, so this
    // claims no grammar and falls through to ordinary statement parsing,
    // where `foo` reads as an expression and the following `:` is unexpected.
    CONTAINS("void f() { foo: return; }", "<HAD ERRORS>");
    // `a: b: while` — one label per loop (problem #2): the outer guard's
    // three-token peek sees (a, :, b) — the third token is an identifier,
    // not a loop keyword, so it isn't claimed as a label either; `a` parses
    // as an expression statement and the `:` after it is a parse error.
    CONTAINS("void f() { a: b: while (true) { } }", "<HAD ERRORS>");

    // Explicit generic call arguments: the marker belongs to the Call, after
    // the complete callee. Type parsing retains qualified, function, and
    // nested `>>` forms without changing the callee tree.
    CONTAINS("void f() { Box::<int>(); }", "Box::<int>()");
    CONTAINS("void f() { N::Box::From::<Array<int>>(xs); }",
             "N::Box::From::<Array<int>>(xs)");
    CONTAINS("void f() { identity::<(User) => bool>(pred); }",
             "identity::<(User) => bool>(pred)");
    CONTAINS("void f() { obj.remap::<string>(\"x\"); }",
             "obj.remap::<string>(x)");
    CONTAINS("void f() { outer::<Array<Array<int>>>(); }",
             "outer::<Array<Array<int>>>()");
    // The deliberately ambiguous bare spelling remains comparison syntax.
    CONTAINS("void f() { identity<int>(5); }", "((identity < int) > 5)");
    // Ordinary calls and macro calls retain their established spelling.
    CONTAINS("void f() { NS::f(1); macro!(2); }", "NS::f(1)");
    CONTAINS("void f() { NS::f(1); macro!(2); }", "macro!(2)");
    // Seeing `::<` commits to the turbofish grammar (empty list still rejected).
    ERROR_CONTAINS("void f() { f::<>(); }", "expected type argument after '<'");
    // LA-32 §4.6: `::<T>` with no following `(args)` is a legal pinned generic
    // VALUE reference (`var f = identity::<int>;`), not a parse error.
    CONTAINS("void f() { f::<int>; }", "f::<int>");
    // A turbofish value reference is not macro-callable — `!(...)` after it is a
    // clean parse error (the trailing `!` is unexpected).
    ERROR_CONTAINS("void f() { f::<int>!(1); }", "expected ';'");

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
