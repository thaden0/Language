// Type-checker tests. The centerpiece is the "bare read with no type context
// is an error" rule for distinct-collided members, plus resolution-by-type,
// assignability, and inference.
#include "Checker.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Resolver.hpp"
#include <cstdio>
#include <string>

static int g_checks = 0;
static int g_failures = 0;

// Returns true if checking `src` produced at least one diagnostic.
static bool checkHasError(const std::string& src) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    Parser parser(lexer.tokenize(), f, sink);
    Program prog = parser.parseProgram();
    Resolver r(f, sink);
    r.run(prog);
    Checker c(r.sema(), f, sink);
    c.run(prog, &r.preludeProgram());
    return sink.hasErrors();
}

static bool checkErrorContains(const std::string& src, const std::string& needle) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    Parser parser(lexer.tokenize(), f, sink);
    Program prog = parser.parseProgram();
    Resolver r(f, sink);
    r.run(prog);
    Checker c(r.sema(), f, sink);
    c.run(prog, &r.preludeProgram());
    for (const Diagnostic& d : sink.all())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

static void expect(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}
#define ERRORS(src)  expect(checkHasError(src), "should error: " src)
#define CLEAN(src)   expect(!checkHasError(src), "should be clean: " src)
#define ERROR_HAS(src, msg) expect(checkErrorContains(src, msg), "should contain '" msg "': " src)

int main() {
    // --- The crown jewel: bare read of a distinct-collided member is an error ---
    ERRORS("class A { distinct int v = 0; } class B { distinct int v = 1; } "
           "class C : A, B { void f() { int x = v; } }");
    // Qualified by base-view narrowing is fine.
    CLEAN("class A { distinct int v = 0; } class B { distinct int v = 1; } "
          "class C : A, B { void f() { int x = this.A::v; } }");

    // Different-typed same-name members do NOT error on their own (coexist);
    // resolution-by-type at the use site is deferred, so this stays lenient.
    CLEAN("class A { int v = 0; } class B { string v; } "
          "class C : A, B { void f() { var x = this.A::v; } }");

    // --- Assignability ---
    ERRORS("void f() { int x = \"hello\"; }");             // string -> int
    CLEAN("void f() { int x = 3; }");
    CLEAN("void f() { string s = \"hi\"; }");
    ERRORS("class A { } class B { } void f() { A a = B(); }");   // unrelated classes
    CLEAN("class A { } class B : A { } void f() { A a = B(); }"); // subclass -> base
    CLEAN("class I { } class A : I { } void f() { I x = A(); }"); // interface-ish base

    // --- Inference: var takes the initializer's type ---
    CLEAN("class A { int describe() => 1; } void f() { A a = A(); int n = a.describe(); }");
    ERRORS("class A { int describe() => 1; } "
           "void f() { A a = A(); string s = a.describe(); }");   // int -> string

    // --- Operators are methods: return type flows through ---
    CLEAN("class C { C add(int v) => this; C (+)(int v) => this; } "
          "void f() { C c = C(); C r = c + 1; }");
    ERRORS("class C { C (+)(int v) => this; } "
           "void f() { C c = C(); int r = c + 1; }");            // C -> int

    // --- Return checks ---
    ERRORS("int f() => \"nope\";");                       // string returned as int
    CLEAN("int f() => 3;");

    // --- Generic inference (§9) ---
    // From constructor arguments: Box(5) infers Box<int>.
    CLEAN("class Box<T> { new Box(T v) { } } void f() { Box<int> b = Box(5); }");
    ERRORS("class Box<T> { new Box(T v) { } } void f() { Box<string> b = Box(5); }");
    // From the target type: Box<int> b = Box() fills T from the target.
    CLEAN("class Box<T> { } void f() { Box<int> b = Box(); }");
    // Same target-typed inference at a bare TOP-LEVEL var decl (no enclosing
    // function): Checker::walk's own StmtKind::Var case used to call typeOf()
    // directly instead of typeInitExpr(), so the target type never reached
    // inferConstruction() here even though the identical declaration inside
    // a function (the case right above) already worked.
    CLEAN("class Box<T> { } Box<int> b = Box();");
    // Generics are invariant: Box<int> is not assignable to Box<string>.
    ERRORS("class Box<T> { new Box(T v) { } } "
           "void f() { Box<int> a = Box(5); Box<string> b = a; }");
    // The motivating case: a Promise-returning function must return a Promise<T>.
    CLEAN("Promise<int> f(int n) => Promise(n);");
    ERRORS("Promise<string> f(int n) => Promise(n);");    // Promise<int> vs Promise<string>
    ERRORS("Promise<int> f(int n) => n;");                // bare int, no implicit wrap (§14)

    // --- Method-level generics (same mechanism as class generics) ---
    CLEAN("R identity<R>(R x) => x; void f() { int a = identity(5); string b = identity(\"x\"); }");
    ERRORS("R identity<R>(R x) => x; void f() { string s = identity(5); }");   // R=int -> string
    // Inferred from a container argument, substituted into the return type.
    CLEAN("class Box<T> { T unwrap() => at; T at; U remap<U>(U v) => v; } "
          "void f() { Box<int> b = Box(); string s = b.remap(\"y\"); }");

    // --- Object mask: primitives carry a method shape, checked like any type ---
    CLEAN("void f() { int n = (5).abs(); }");             // int method -> int
    ERRORS("void f() { string s = (5).abs(); }");         // int -> string
    CLEAN("void f() { int n = \"hi\".length(); }");       // string method -> int
    ERRORS("void f() { int n = \"hi\".toUpper(); }");     // string -> int
    CLEAN("void f() { string s = \"hi\".toUpper(); }");

    // --- Overload resolution by argument type ---
    CLEAN("class C { int f(int a) => a; string f(string a) => a; } "
          "void g() { C c = C(); int a = c.f(1); string b = c.f(\"x\"); }");
    ERRORS("class C { int f(int a) => a; string f(string a) => a; } "
           "void g() { C c = C(); int a = c.f(1); int b = c.f(\"x\"); }");  // f(string)->string, not int
    ERRORS("class C { int f(int a) => a; } void g() { C c = C(); int a = c.f(\"x\"); }"); // no overload
    CLEAN("int f(int a) => a; string f(string a) => a; void g() { int a = f(1); string b = f(\"x\"); }");

    // --- Named arguments + default values: one mapped overload story ---
    CLEAN("void f(int a = 1, string s = \"x\", bool b = false) { } "
          "void g() { f(); f(9); f(b: true); f(9, b: true); f(s: \"y\", a: 3); }");
    CLEAN("void f(int a = 1, int b) { } void g() { f(b: 2); }");
    CLEAN("int f(int a) => 1; int f(int a, int b = 0) => 2; "
          "void g() { int x = f(1); }");
    CLEAN("class C { new C(int n = 1, string s = \"x\") { } "
          "void m(int n = 1) { } } void g() { C c = C(s: \"y\"); c.m(n: 2); }");
    ERROR_HAS("void f(int a, int b) { } void g() { f(a: 1, 2); }",
              "positional argument after named argument");
    ERROR_HAS("void f(int a) { } void g() { f(nope: 1); }",
              "no parameter named 'nope'");
    ERROR_HAS("void f(int a) { } void g() { f(1, a: 2); }",
              "parameter 'a' is bound both positionally and by name");
    ERROR_HAS("void f(int a, int b) { } void g() { f(1); }",
              "missing required argument 'b'");
    ERROR_HAS("int runtime() => 1; void f(int a = runtime()) { }",
              "a default value must be a compile-time constant");
    ERROR_HAS("void f(int a, int b = a) { }",
              "default parameter 'b' cannot reference another parameter or 'this'");

    // --- Strictness: unknown names/functions are errors (console/System whitelisted) ---
    ERRORS("void f() { int x = missingVar; }");
    ERRORS("void f() { missingFn(1); }");
    CLEAN("void f() { console.writeln(1); }");
    // bug.md #28: an unknown member after `::`/`.` on a class-used-as-a-value
    // used to pass through as the class type and silently evaluate to void.
    ERRORS("class E { public const int A = 1; } void f() { int x = E::doesNotExist; }");
    ERRORS("class E { public const int A = 1; } void f() { var x = E::nope.alsoFake; }");
    // ...but real ctor labels, member names and base-qualified access stay clean.
    CLEAN("class B2 { string s; new fromString(string x) { s = x; } } "
          "void f() { B2 b = B2::fromString(\"hi\"); }");
    CLEAN("class A { distinct int v = 1; } class B { distinct int v = 2; } "
          "class C : A, B { int sum() => this.A::v + this.B::v; }");
    // §9 'required when not': a generic ctor with no inference source errors.
    ERRORS("class Box<T> { } void f() { var b = Box(); }");
    CLEAN("class Box<T> { } void f() { Box<int> b = Box(); }");     // target fills T
    // Namespaced function overloads resolve by argument types.
    CLEAN("namespace N { int f(int a) => a; string f(string a) => a; } "
          "void g() { int a = N::f(1); string b = N::f(\"x\"); }");
    ERRORS("namespace N { int f(int a) => a; string f(string a) => a; } "
           "void g() { int a = N::f(\"x\"); }");                    // string overload -> string
    // (!=) derives from (==) on classes (returns bool).
    CLEAN("class C { bool (==)(int v) => true; } "
          "void f() { C c = C(); bool b = c != 5; }");

    // --- async/await: await unwraps Promise<T> -> T (no function coloring) ---
    CLEAN("Promise<int> f() => Promise(3); void run() { int x = await f(); }");
    ERRORS("Promise<int> f() => Promise(3); void run() { string x = await f(); }");  // T=int
    CLEAN("void run() { HttpClient c = HttpClient(); "
          "HttpResponse r = await c.fetch(\"h\", 80, \"/\"); int s = r.status; }");

    // --- Block-body lambdas + keyword member names (framework ergonomics) ---
    CLEAN("void run() { Timer t = std::after(1); t.subscribe((n) => { int x = n + 1; }); }");
    CLEAN("void run() { HttpClient c = HttpClient(); "
          "c.get(\"127.0.0.1\", 80, \"/\", (r) => { int s = r.status; }); }");  // .get keyword-name
    CLEAN("class K { int get(int a) => a; } void f() { K k = K(); int x = k.get(3); }");

    // --- match: exhaustiveness + per-arm narrowing (shares is/catch machinery) ---
    CLEAN("string f(int | string x) => match (x) { int => \"n\"; string => \"s\"; };");   // exhaustive union
    ERRORS("string f(int | string x) => match (x) { int => \"n\"; };");                 // missing string, no else
    CLEAN("string f(int | string x) => match (x) { int => \"n\"; else => \"o\"; };");    // else covers rest
    ERRORS("string f(int n) => match (n) { 0 => \"z\"; 1 => \"o\"; };");                 // int not exhaustive w/o else
    CLEAN("class A{} class B:A{} string f(A a) => match (a) { B => \"b\"; else => \"a\"; };");

    // --- Optionality: T? = T | None; narrowing required and enforced ---
    ERRORS("void f() { string? s = None; int n = s.length(); }");   // must narrow first
    CLEAN("void f() { string? s = None; if (s != None) { int n = s.length(); } }");
    CLEAN("void f() { string? s = None; if (s is string) { int n = s.length(); } }");
    CLEAN("void f() { string? s = \"x\"; if (s != None && s.length() > 0) { } }");  // && chains
    ERRORS("void f() { string? s = None; if (s == None) { int n = s.length(); } }"); // wrong branch
    CLEAN("void f() { string? s = None; string t = s ?? \"d\"; }");
    ERRORS("void f() { string? s = None; string t = s ?? 5; }");    // ?? default type mismatch
    CLEAN("void f() { int? n = 7; int x = (n is int) ? 0 : 1; }");
    CLEAN("class R { string? host; } void f(R r) { if (r.host != None) { int n = r.host.length(); } }");
    ERRORS("class R { string? host; } void f(R r) { int n = r.host.length(); }");   // field unnarrowed
    ERRORS("void f() { string? s = \"a\"; if (s != None) { s = None; int n = s.length(); } }"); // invalidated
    CLEAN("void f() { int? n = None; }");                            // None assignable into T?
    ERRORS("void f() { string s = None; }");                         // but not into plain T

    // --- Track 04 M2: toInt/toFloat are optional-returning; must narrow ---
    ERRORS("void f() { int n = \"5\".toInt(); }");                    // int? -> int: unnarrowed
    CLEAN("void f() { int? p = \"5\".toInt(); if (p != None) { int n = p; } }");
    CLEAN("void f() { int p = \"5\".toInt() ?? 0; }");
    ERRORS("void f() { float x = \"5.0\".toFloat(); }");              // float? -> float: unnarrowed
    CLEAN("void f() { float? p = \"5.0\".toFloat(); if (p != None) { float x = p; } }");

    // --- Files/OpenMode: flags are operator methods; NS::var typed ---
    CLEAN("void f() { OpenMode m = std::read | std::write; bool b = m.has(std::read); }");
    ERRORS("void f() { int m = std::read | std::write; }");   // OpenMode, not int
    CLEAN("void f() { File x = File(\"/tmp/t\", std::write); x.writeln(\"a\"); x.close(); }");
    ERRORS("void f() { File x = File(\"/tmp/t\", 3); }");    // int is not OpenMode

    // --- Exceptions: throwable = implements IException; catch binds typed ---
    CLEAN("void f() { try { throw RuntimeException(\"x\"); } "
          "catch (RuntimeException e) { string m = e.message; } }");
    ERRORS("void f() { throw 5; }");                            // int is not IException
    ERRORS("class NotEx { } void f() { throw NotEx(); }");      // class w/o IException
    ERRORS("void f() { try { throw RuntimeException(\"x\"); } "
           "catch (RuntimeException e) { int m = e.message; } }");   // message: string

    // --- Map typing: m[k] -> V; ops via generic substitution ---
    CLEAN("void f() { Map<string, int> m; int v = m[\"k\"]; }");
    ERRORS("void f() { Map<string, int> m; string v = m[\"k\"]; }");   // V=int, not string
    CLEAN("void f() { Map<string, int> m; Map<string, int> n = m.with(\"a\", 1); bool b = m.has(\"a\"); }");

    // --- HKT: F is a constructor variable; head + args both bind ---
    CLEAN("F<A> keepIt<F, A>(F<A> c) => c; "
          "void f() { Array<int> a = [1]; Array<int> b = keepIt(a); }");
    ERRORS("F<A> keepIt<F, A>(F<A> c) => c; "
           "void f() { Array<int> a = [1]; Pair<int, int> p = keepIt(a); }");   // wrong head
    ERRORS("F<A> keepIt<F, A>(F<A> c) => c; "
           "void f() { Array<int> a = [1]; Array<string> s = keepIt(a); }");    // wrong arg

    // --- Indexers: a[i] typed via the ([]) get accessor, with generic substitution ---
    CLEAN("void f() { Array<int> a = [1,2,3]; int x = a[0]; }");    // Array<int>[i] -> int
    ERRORS("void f() { Array<int> a = [1,2,3]; string x = a[0]; }"); // -> int, not string
    CLEAN("class M { int at2(int i) => i; get ([])(int i) => at2(i); } "
          "void f() { M m = M(); int x = m[3]; }");

    // --- Member access resolves via the shape; missing members stay lenient ---
    CLEAN("class A { int x; } void f() { A a = A(); int n = a.x; }");
    CLEAN("void f() { console.writeln(1); }");            // unmodeled global: no error

    // --- Dependency injection: bind / inject (§12.5, bug.md #9) ---
    // A block-level bind is NOT the enclosing function's return (was mis-checked).
    CLEAN("interface IG { string h(); } class H : IG { string h() => \"x\"; } "
          "string speak(IG g) => g.h(); "
          "void run() { bind IG => H(); console.writeln(speak()); }");
    // Implicit fill: an unfilled param whose type has a bind in scope resolves.
    CLEAN("interface IG { string h(); } class H : IG { string h() => \"x\"; } "
          "bind IG => H(); string speak(IG g) => g.h(); void f() { speak(); }");
    // bug.md #24: implicit fill also reaches CONSTRUCTOR params (reference §4.7),
    // both at a direct call and at an initializer/target-typed site.
    CLEAN("interface IG { string h(); } class H : IG { string h() => \"x\"; } "
          "bind IG => H(); class S { new S(IG g) { } } void f() { S s = S(); }");
    CLEAN("interface IG { string h(); } class H : IG { string h() => \"x\"; } "
          "bind IG => H(); class S { new S(IG g) { } string ready() => \"ok\"; } "
          "void f() { console.writeln(S().ready()); }");
    // No bind in scope: the unfilled ctor parameter cannot be resolved.
    ERRORS("interface IG { string h(); } class S { new S(IG g) { } } "
           "void f() { S s = S(); }");
    // No bind in scope: the unfilled parameter cannot be resolved.
    ERRORS("interface IG { string h(); } string speak(IG g) => g.h(); "
           "void f() { speak(); }");
    // Explicit `inject Type` selector needs a bind in scope.
    CLEAN("interface IG { string h(); } class H : IG { string h() => \"x\"; } "
          "bind IG => H(); string speak(IG g) => g.h(); void f() { speak(inject IG); }");
    ERRORS("interface IG { string h(); } string speak(IG g) => g.h(); "
           "void f() { speak(inject IG); }");                // no binding for IG
    // Duplicate binding for one type in one scope is a hard error.
    ERRORS("interface IG { } class A : IG { } class B : IG { } "
           "bind IG => A(); bind IG => B();");
    // bug.md #23: binding a VALUE type (struct/primitive) is rejected — injection
    // cannot carry a value through (it silently arrived default-constructed).
    ERRORS("struct Cfg { int port; } bind Cfg => Cfg(8080);");
    ERRORS("bind int => 5;");
    ERRORS("struct Cfg { int port; } void f() { Cfg c = Cfg(80); bind Cfg => c; }");
    // The SAME two binds in DIFFERENT (nested) scopes are fine (nearest-wins).
    CLEAN("interface IG { string h(); } class A : IG { string h() => \"a\"; } "
          "class B : IG { string h() => \"b\"; } bind IG => A(); "
          "string speak(IG g) => g.h(); void f() { bind IG => B(); speak(); }");
    // A factory whose body's type does not match the bound type is an error.
    ERRORS("interface IG { } class A : IG { } class NotIG { } bind IG => NotIG();");
    // `uses NS` must NOT activate NS's binds (owner ruling, system-binds.md §5).
    ERRORS("interface IG { string h(); } "
           "namespace M { class H : IG { string h() => \"x\"; } bind IG => H(); } "
           "uses M; string speak(IG g) => g.h(); void f() { speak(); }");

    // --- Bug 7 (generalized by const.md): a namespace global is only
    // rejected for reassignment when it's marked `const`; a plain (var)
    // namespace global stays legitimately mutable, same as any other var. ---
    CLEAN("namespace N { int CODE = 7; } void f() { N::CODE = 9; }");
    CLEAN("namespace N { int CODE = 7; } void f() { N::CODE += 1; }");   // compound too
    ERRORS("namespace N { const int CODE = 7; } void f() { N::CODE = 9; }");
    ERRORS("namespace N { const int CODE = 7; } void f() { N::CODE += 1; }");
    // Reading a namespace global is fine; only writing a const one is rejected.
    CLEAN("namespace N { int CODE = 7; } void f() { int x = N::CODE; }");
    // A namespaced function/class member is not a value global — unaffected.
    CLEAN("namespace N { int helper(int a) => a; } void f() { int x = N::helper(1); }");
    // Top-level (un-namespaced) user globals stay legitimately reassignable.
    CLEAN("string t = \"a\"; void f() { t = \"b\"; }");

    // --- OQ2 (deferal-const-system-extensions.md §3): sectional `const:` ---
    // A `const:` section makes each following member const — reassigning one is
    // the same write-window error a per-member `const` produces.
    ERRORS("class C { const: int p = 1; new C() {} void f() { p = 2; } }");
    // The section is sticky across a later access label (orthogonal axes): a
    // plain member under `public:` that follows `const:` is still const.
    ERRORS("class C { const: int a = 1; public: int b = 2; new C() {} void f() { b = 3; } }");
    // A per-member `var` overrides the section, restoring mutability.
    CLEAN("class C { const: int p = 1; var int hits = 0; new C() {} void f() { hits = hits + 1; } }");
    // `var` outside any const section is just an explicit-mutable field (no-op).
    CLEAN("class C { var int hits = 0; new C() {} void f() { hits = 5; } }");
    // A per-member `const` inside a normal (non-const) class still freezes it.
    ERRORS("class C { const int p = 1; new C() {} void f() { p = 2; } }");

    // --- Bug 8: `uses` is lexically scoped (block-level works, and is confined) ---
    // A block-level `uses` imports for exactly that block (was silently inert).
    CLEAN("namespace M { int helper(int x) => x + 100; } "
          "void f() { uses M; int y = helper(5); }");
    // ...and a block-level `uses` of a class type resolves in that block.
    CLEAN("namespace M { class Box { int n; new Box(int v) { n = v; } } } "
          "void f() { uses M; Box b = Box(9); int n = b.n; }");
    // The import does NOT leak out of its block (nearest-wins, lexical).
    ERRORS("namespace M { int helper(int x) => x + 100; } "
           "void f() { { uses M; int a = helper(1); } int b = helper(2); }");
    // An unknown namespace in a block `uses` is a loud error, not a silent no-op.
    ERRORS("void f() { uses Nope; }");
    // A top-level `uses` still makes the namespace's names visible file-wide.
    CLEAN("namespace M { int helper(int x) => x + 100; } uses M; "
          "void f() { int y = helper(5); }");

    // bug.md #8 follow-up: calls under `!` and inside lambda bodies are now
    // type-checked (they were skipped, so an unknown call slipped past).
    ERRORS("void f() { bool b = !nonesuch(); }");            // call under `!`
    ERRORS("void f() { var g = () => nonesuch(); }");        // call in a lambda body
    // ...and a resolvable namespaced call in those positions stays clean.
    CLEAN("namespace M { bool ok(int x) => x > 0; } uses M; "
          "void f() { if (!ok(1)) { } }");
    CLEAN("namespace M { int inc(int x) => x + 1; } uses M; "
          "void f() { var g = () => inc(2); }");

    // --- imports.md: selective `use Path::name (as alias);` ---
    // Every declaration kind imports uniformly: value, function, class,
    // nested namespace.
    CLEAN("namespace M { int V = 3; } use M::V; void f() { int x = V; }");
    CLEAN("namespace M { int inc(int x) => x + 1; } use M::inc; "
          "void f() { int x = inc(2); }");
    CLEAN("namespace M { class Box { int n; new Box(int v) { n = v; } } } "
          "use M::Box; void f() { Box b = Box(5); int n = b.n; }");
    CLEAN("namespace A { namespace B { int f() => 1; } } use A::B; "
          "void f() { int x = B::f(); }");
    // `as` renames (collision-proof); the alias is a plain declaration, not
    // an expression — no error from a dangling-expression check.
    CLEAN("namespace M { int inc(int x) => x + 1; } use M::inc as bump; "
          "void f() { int x = bump(2); }");
    // An unknown path segment or unknown final member is a loud error.
    ERRORS("use Nope::x;");
    ERRORS("namespace M { } use M::nope;");
    // File-level scoping (bug.md #8's substrate): a `use` at top level is
    // visible file-wide (position-independent), same as `uses`.
    CLEAN("void f() { int x = V; } namespace M { int V = 3; } use M::V;");
    // Block-scoped `use` (bug.md #8's per-block importScope, reused here):
    // visible for exactly that block, and confined — it does not leak out.
    CLEAN("namespace M { int inc(int x) => x + 1; } "
          "void f() { use M::inc; int x = inc(2); }");
    ERRORS("namespace M { int inc(int x) => x + 1; } "
           "void f() { { use M::inc; int a = inc(1); } int b = inc(2); }");
    // Shadowing: `use` beats a bulk `uses` dump of the SAME name in the same
    // scope ("specific beats bulk") — a same-signature clash resolves to the
    // selectively-imported one, not whichever the bulk dump happened to add.
    CLEAN("namespace Lib { int bump(int x) => x + 100; } "
          "namespace Dup { int bump(int x) => x - 1; } "
          "uses Dup; use Lib::bump; "
          "void f() { if (bump(5) != 105) { } }");
    // Construction through an ALIASED class must find the real constructor
    // (the ctor label is the class's own name, not the call-site alias text).
    CLEAN("namespace M { class Box { int n; new Box(int v) { n = v; } } } "
          "use M::Box as Bx; void f() { Bx b = Bx(5); int n = b.n; }");
    // Writing through a bare name that reached a namespace global via `use`
    // follows the same const rule as any other access path (imports.md §4 +
    // const.md — "the alias names the same slot"): a plain (non-const)
    // global stays reassignable; a const one is rejected, same as the
    // qualified form.
    CLEAN("namespace N { int CODE = 7; } use N::CODE; "
          "void f() { CODE = 9; }");
    CLEAN("namespace N { int CODE = 7; } use N::CODE as C; "
          "void f() { C = 9; }");
    ERRORS("namespace N { const int CODE = 7; } use N::CODE; "
           "void f() { CODE = 9; }");
    ERRORS("namespace N { const int CODE = 7; } use N::CODE as C; "
           "void f() { C = 9; }");
    // A plain top-level user global (no namespace, no import) stays
    // legitimately reassignable — unaffected by the extension above.
    CLEAN("string t = \"a\"; void f() { t = \"b\"; }");

    // --- const.md: a slot's write view scopes to its initialization window ---
    // Locals: fine with an initializer; a bare `const T x;` is provably useless.
    CLEAN("void f() { const int x = 1; }");
    ERRORS("void f() { const int x; }");
    ERRORS("void f() { const int x = 1; x = 2; }");
    ERRORS("void f() { const int x = 1; x += 2; }");
    CLEAN("void f() { const var x = 5; }");
    // `let` == `const var` (§5): single-assignment, else identical to `var`.
    CLEAN("void f() { let x = 1; console.writeln(x); }");
    ERRORS("void f() { let x = 1; x = 2; }");
    // Params: reassignment in the body is an error; reading stays fine.
    CLEAN("void f(const int x) { console.writeln(x); }");
    ERRORS("void f(const int x) { x = 2; }");
    // for-in: each iteration is a fresh binding; reassigning it is an error.
    CLEAN("void f(Array<int> xs) { for (const int x in xs) { console.writeln(x); } }");
    ERRORS("void f(Array<int> xs) { for (const int x in xs) { x = 5; } }");
    // Namespace/top-level globals: only the initializer's window (generalizes
    // Bug 7's ad hoc "no namespace-global writes at all" ban — see above).
    CLEAN("const int G = 1; void f() { console.writeln(G); }");
    ERRORS("const int G = 1; void f() { G = 2; }");
    // Fields: techdesign-readonly §4.2 narrows `const` to a compile-time-
    // constant initializer, and forbids ANY constructor write (even the
    // former ctor-assigned-runtime-value idiom, which now belongs to
    // `readonly` — see the readonly suite below). Only the constant-init form
    // stays valid on `const`.
    CLEAN("class S { const int seq = 0; }");
    // A ctor-assigned const field (the pre-LA-28 idiom) is now an error — the
    // runtime-value-via-ctor capability moved to `readonly` (§1.1 migration).
    ERRORS("class S { const int seq; new S(int s) { seq = s; } }");
    // A const field needs a compile-time-constant initializer; "no
    // initializer at all" is now flagged too (§4.2), unlike the old
    // deferred-definite-assignment const rule.
    ERRORS("class S { const int seq; }");
    ERRORS("class S { const int v = 1; void bump() { v = 2; } }");      // non-ctor method
    ERRORS("class A { const int v = 1; } class B { void f(A a) { a.v = 2; } }");  // other class
    // Base/derived ctor-write cases migrate to `readonly` (the construction-
    // time-fixed field) — see the readonly suite below for their equivalents.
    ERRORS("class A { const int v = 1; set v(int x) { } }");            // set over const field
    CLEAN("class A { const int v = 1; get v() => v; }");                // get is fine
    // MI collision: same name + type across bases disagreeing on constness
    // is ambiguous — refuse to guess (mirrors the `distinct` collision family).
    ERRORS("class A { const int v = 1; } class B { int v = 2; } class C : A, B { }");
    CLEAN("class A { distinct const int v = 1; } class B { distinct int v = 2; } "
          "class C : A, B { }");                       // `distinct` keeps both, no ambiguity
    // Mutating methods write `this` — calling one through a const binding
    // needs a write view that doesn't exist.
    ERRORS("struct P { int x; mutating void bump() { x = x + 1; } } "
           "void f() { const P p = P(); p.bump(); }");
    CLEAN("struct P { int x; int get() => x; } "
          "void f() { const P p = P(); int y = p.get(); }");   // non-mutating: fine
    // Indexer sugar: `a[i] = v` on a pure array is a rebind of the slot itself.
    ERRORS("void f() { const Array<int> a = Array(3, 0); a[0] = 5; }");
    CLEAN("void f() { const Array<int> a = Array(3, 0); int y = a[0]; }");   // reads are fine
    // Not transitive: a const binding's own value assigns freely; aliasing
    // into a var binding and mutating the ALIAS's own slot is unaffected.
    CLEAN("class Box { int n = 0; } "
          "void f() { const Box b = Box(); Box alias = b; alias.n = 5; }");
    // Bug 7's probe case, generalized: the prelude's `console`/OpenMode flags
    // are marked const (Resolver.cpp) — covered end-to-end via corpus/const.ext.

    // --- techdesign-readonly (LA-28): `readonly` — construction-time-fixed
    // instance fields. Reuses the const write-window machinery (§4.3) plus a
    // new definite-assignment / write-once pass (§4.4). ---

    // The DI idiom (request's motivating case): ctor-assigned runtime value,
    // exactly once. This is the migrated equivalent of old test_checker.cpp:415.
    CLEAN("class S { readonly int seq; new S(int s) { seq = s; } }");
    // Initializer form is also fine (the value is just known sooner).
    CLEAN("class S { readonly int seq = 0; }");
    // Write from a non-ctor method / from another class / after construction:
    // all "outside the window" (constBlockedWrite reused verbatim, §4.3).
    ERRORS("class S { readonly int v; new S() { v = 1; } void bump() { v = 2; } }");
    ERRORS("class A { readonly int v; new A() { v = 1; } } "
           "class B { void f(A a) { a.v = 2; } }");
    // Base/derived: migrated from old test_checker.cpp:423-426 (the same
    // ctor-only-window shape, now under `readonly`).
    ERRORS("class Base { readonly int seq; new Base(int s) { seq = s; } } "
           "class Derived : Base { new Derived(int s) { seq = s; } }");  // derived-ctor direct write
    CLEAN("class Base { readonly int seq; new Base(int s) { seq = s; } } "
          "class Derived : Base { new Derived(int s) { Base::Base(s); } }");  // via Base::Ctor
    // Definite assignment (§4.4): unassigned by a declared ctor is an error.
    ERRORS("class S { readonly int v; new S() { } }");
    // Zero constructors + no initializer: never assigned at all.
    ERRORS("class S { readonly int v; }");
    // Double-assigned in one ctor body (write-once, top-level statements).
    ERRORS("class S { readonly int v; new S() { v = 1; v = 2; } }");
    // Initializer form + a ctor also assigning it: second write, rejected.
    ERRORS("class S { readonly int v = 0; new S(int x) { v = x; } }");
    // set over a readonly field is rejected (a write view outliving the
    // window); get is fine.
    ERRORS("class A { readonly int v; new A() { v = 1; } set v(int x) { } }");
    CLEAN("class A { readonly int v; new A() { v = 1; } get v() => v; }");
    // MI collision: same name + type disagreeing on readonly-ness is
    // ambiguous, mirroring the const collision family; `distinct` keeps both.
    ERRORS("class A { readonly int v; new A() { v = 1; } } "
           "class B { int v = 2; } class C : A, B { }");
    CLEAN("class A { distinct readonly int v; new A() { v = 1; } } "
          "class B { distinct int v = 2; } class C : A, B { }");
    // Non-transitive (request §2, matching const): the field's own binding is
    // fixed, not the referenced object's fields.
    CLEAN("class Box { int n = 0; } "
          "class C { readonly Box b; new C() { b = Box(); } "
          "void f() { b.n = 5; } }");
    // A field can't be both `const` and `readonly` (§4.5) — two competing
    // write rules on one slot.
    ERRORS("class S { const readonly int v = 0; }");
    // §4.4 v1's DOCUMENTED false-reject: an exhaustive if/else ctor assignment
    // is sound-not-complete (top-level-statements-only) and is rejected in
    // v1, pinned here as a known regression per §8 OQ-2 — a later CFG-aware
    // pass relaxes this deliberately, at which point this line should flip to
    // CLEAN and the comment updated.
    ERRORS("class S { readonly int v; "
           "new S(bool c) { if (c) { v = 1; } else { v = 2; } } }");

    // --- §3.7 loudness: undefined primitive operators are compile errors ---
    // (used to type as the operand and silently produce a runtime void — the
    // `int x = 1 << 4;` silent-void bug, curl-design §2.8#1)
    ERRORS("void f() { var x = 1.5 % 2.0; }");
    ERRORS("void f() { var x = 1.5 & 2.0; }");
    ERRORS("void f() { var s = \"a\" - \"b\"; }");
    ERRORS("void f() { var b = true + false; }");
    ERRORS("void f() { var s = -\"abc\"; }");
    ERRORS("void f() { float g = 1.0; g %= 2.0; }");
    CLEAN("void f() { int x = 1 + 2 * 3 % 4; int y = x & 7; int z = y | 1; }");
    CLEAN("void f() { float g = 1.5 * 2.0 - 0.5 / 2.0; var h = -g; }");
    CLEAN("void f() { string s = \"a\" + \"b\"; bool c = s < \"z\"; }");
    CLEAN("void f() { int x = 5; x %= 2; x -= 1; }");
    // Object transfer operators are methods — untouched by the whitelist.
    CLEAN("class W { W (<<)(string s) => this; } void f() { W w = W(); w << \"x\"; }");

    // --- Track 01 F1: real int shift/xor/complement ---
    CLEAN("void f() { int x = 1 << 4; int y = 256 >> 4; int z = 5 ^ 3; int w = ~x; }");
    ERRORS("void f() { float g = 1.0; var x = g << 1; }");   // no shifts on float
    ERRORS("void f() { var x = 1.0 ^ 2.0; }");                // no xor on float
    ERRORS("void f() { var x = true ^ false; }");             // no xor on bool (use !=)
    ERRORS("void f() { var x = ~1.5; }");                     // ~ is int-only
    ERRORS("void f() { var x = ~\"a\"; }");
    CLEAN("void f() { int x = 5; int y = x << 2 >> 1 ^ 3 & 1 | 2; }");   // precedence smoke test
    // `<<`/`>>` on an object left operand still dispatch the (<<) method —
    // untouched by the int-shift addition (same resolution-by-type rule as +).
    CLEAN("class W { W (<<)(int v) => this; } void f() { W w = W(); w << 5; }");

    // --- Track 02 F1: break/continue loop-depth ---
    ERRORS("void f() { break; }");
    ERRORS("void f() { continue; }");
    CLEAN("void f() { while (true) { break; } }");
    CLEAN("void f() { while (true) { continue; } }");
    CLEAN("void f() { for (int i = 0; i < 5; i += 1) { break; } }");
    CLEAN("void f() { for (int x in 1..5) { continue; } }");
    // A lambda body is its own loop-nesting scope: a bare break inside a
    // lambda that sits inside an enclosing loop must still error (P4).
    ERRORS("void f() { while (true) { var g = () => { break; }; } }");
    // ...but a loop INSIDE the lambda body is fine.
    CLEAN("void f() { var g = () => { while (true) { break; } }; }");
    // match is not a loop-nesting boundary: break in an arm targets the
    // enclosing loop, not an error.
    CLEAN("void f() { while (true) { match (1) { 1 => { break; } else => { continue; } } } }");
    ERRORS("void f() { match (1) { 1 => { break; } else => { continue; } } }");

    // --- Track 02 F2: do-while ---
    CLEAN("void f() { do { console.writeln(1); } while (true); }");
    CLEAN("void f() { do { break; } while (true); }");
    CLEAN("void f() { do { } while (\"not a bool\"); }");   // still typed, not enforced strictly (matches While)

    // --- Track 02 F3: using ---
    CLEAN("void f() { using File f = File(\"x\", std::read); }");
    ERRORS("void f() { while (true) using File f = File(\"x\", std::read); }");  // not a direct block statement
    ERRORS("void f() { using int x = 1; }");    // int does not implement IDisposable
    ERRORS("void f() { using File f = File(\"x\", std::read); f = File(\"y\", std::read); }");  // reassignment
    ERRORS("void f() { using File f; }");        // needs an initializer (rides const's rule)

    // --- LA-25: method references as values (techdesign-method-references.md) ---
    // A single-candidate reference needs no target type: clean everywhere.
    CLEAN("namespace M { int dbl(int x) => x * 2; } "
          "class B { (int)=>int f; } void g() { B b = B(); b.f = M::dbl; }");
    CLEAN("class C { int m(int x) => x; } "
          "class B { (C, int)=>int f; } void g() { B b = B(); b.f = C::m; }");
    // Labeled-constructor reference.
    CLEAN("class U { string n; new U() { } new FromName(string s) { n = s; } } "
          "class B { (string)=>U f; } void g() { B b = B(); b.f = U::FromName; }");
    // Missing member: error at the reference site (request acceptance #2).
    ERROR_HAS("class C { int m(int x) => x; } "
              "class B { (C, int)=>int f; } void g() { B b = B(); b.f = C::nope; }",
              "no member 'nope'");
    // Overloaded + no target type: refuse to guess (§8.2).
    ERROR_HAS("class C { int m(int x) => x; string m(string s) => s; } "
              "void g() { var h = C::m; }",
              "ambiguous method reference");
    // Overloaded + a target type that selects exactly one candidate: clean —
    // by assignment, and in constructor-argument position (the R10 shape).
    CLEAN("class C { int m(int x) => x; string m(string s) => s; } "
          "class B { (C, int)=>int f; } void g() { B b = B(); b.f = C::m; }");
    CLEAN("class C { int m(int x) => x; string m(string s) => s; } "
          "class R { (C, string)=>string h; new R((C, string)=>string x) { h = x; } } "
          "void g() { R r = R(C::m); }");
    // Generic callable: unbound type parameters in value position (§8.6).
    ERROR_HAS("namespace M { R identity<R>(R x) => x; } "
              "class B { (int)=>int f; } void g() { B b = B(); b.f = M::identity; }",
              "cannot reference generic function");
    ERROR_HAS("class C { U m<U>() => 1; } "
              "class B { (C)=>int f; } void g() { B b = B(); b.f = C::m; }",
              "cannot reference generic function");

    // --- F3: bound method references (techdesign-bound-method-references.md) ---
    CLEAN("class C { int m(int x) => x + 1; } "
          "void g(C p) { C c = p; var a = c.m; var b = p.m; }");
    CLEAN("class C { (int)=>int f; new C() { f = this.m; } int m(int x) => x; }");
    // Target typing chooses a bound overload, including the double-overload
    // call shape; the string overload must not accept this deferred ref.
    CLEAN("class C { int m(int x) => x; string m(string s) => s; } "
          "class H { (string)=>string f; } "
          "void g() { C c = C(); H h = H(); h.f = c.m; }");
    CLEAN("class C { int m(int x) => x; string m(string s) => s; } "
          "void g() { C c = C(); Array<(int)=>int> fs = [c.m]; }");
    CLEAN("class C { int m(int x) => x; string m(string s) => s; } "
          "void take(string s) { } void take((int)=>int f) { } "
          "void g() { C c = C(); take(c.m); }");
    // Arbitrary receiver expressions would be re-evaluated by the synthesized
    // lambda, so v1 requires an explicit local snapshot and shows that fix.
    ERROR_HAS("class C { int m() => 1; } class H { C c; } "
              "void g() { H h = H(); var f = h.c.m; }",
              "bind the receiver to a local first");
    ERROR_HAS("class C { int m() => 1; } C make() => C(); "
              "void g() { var f = make().m; }",
              "fix: var r = make(); then use r.m");
    ERROR_HAS("class C { U m<U>() => 1; } "
              "class H { (int)=>int f; } "
              "void g() { C c = C(); H h = H(); h.f = c.m; }",
              "cannot reference generic function");
    ERROR_HAS("class C { int m() => 1; } "
              "void g() { C? c = C(); var f = c.m; }",
              "narrow the union before member access");
    // A closure-valued data field wins over a same-named method; the read is
    // not rewritten. A bound ref also composes with spawn like any lambda.
    CLEAN("class C { (int)=>int m; int m(int x) => x; } "
          "void g() { C c = C(); c.m = (x) => x; var f = c.m; int y = f(1); }");
    CLEAN("class C { int read() => 1; } "
          "void g() { C c = C(); Worker<int> w = std::spawn(c.read); }");

    // --- designs/complete/techdesign-class-method-dispatch.md ---
    // §2/S1: a class-typed receiver now dispatches on the runtime object,
    // uniformly with an interface-typed one — no override, no error.
    CLEAN("class Animal { string speak() => \"...\"; } "
          "class Dog : Animal { string speak() => \"Woof\"; } "
          "void g() { Animal a = Dog(); a.speak(); }");
    // §5.4/M3: an overridden method that shares its (name, arity) with
    // another overload can't be disambiguated by the runtime name+arity
    // lookup — loud compile error at the call site.
    ERROR_HAS("class Animal { string speak(string s) => \"a-str\"; "
              "string speak(int n) => \"a-int\"; } "
              "class Dog : Animal { string speak(int n) => \"d-int\"; } "
              "void g() { Animal a = Dog(); a.speak(5); }",
              "shares its arity with another overload");
    // §5.2: the safe case — an overridden overload set with DISTINCT arities
    // per overload stays clean; the diagnostic must not over-fire.
    CLEAN("class Animal { string speak() => \"a\"; string speak(int n) => \"a-int\"; } "
          "class Dog : Animal { string speak() => \"WOOF\"; } "
          "void g() { Animal a = Dog(); a.speak(); a.speak(5); }");
    // S2: the same hazard reached through a bare this-call.
    ERROR_HAS("class Animal { string speak(string s) => \"a-str\"; "
              "string speak(int n) => \"a-int\"; string tag() => speak(1); } "
              "class Dog : Animal { string speak(int n) => \"d-int\"; }",
              "shares its arity with another overload");
    // S3: the same hazard reached through a method reference.
    ERROR_HAS("class Animal { string speak(string s) => \"a-str\"; "
              "string speak(int n) => \"a-int\"; } "
              "class Dog : Animal { string speak(int n) => \"d-int\"; } "
              "class B { (Animal, int)=>string f; } "
              "void g() { B b = B(); b.f = Animal::speak; }",
              "shares its arity with another overload");
    // Interfaces are unaffected by the M3 diagnostic (pre-existing name+arity
    // limitation there is out of scope — roadmapped, §10.4).
    CLEAN("interface IAnimal { string speak(string s); string speak(int n); } "
          "class Cat : IAnimal { string speak(string s) => \"c-str\"; "
          "string speak(int n) => \"c-int\"; } "
          "void g() { IAnimal a = Cat(); a.speak(5); }");

    // --- F6: covariant-return interface satisfaction ---
    CLEAN("interface IComponent { } interface IContainer { IContainer add(IComponent x); } "
          "class Component : IComponent { } "
          "class Container : IContainer { Container add(IComponent x) => this; } "
          "void g() { Container c = Container(); Component x = Component(); "
          "Container chained = c.add(x).add(x); IContainer view = c; view.add(x); }");
    CLEAN("interface IValue { } interface IMaybe { IValue? value(); } "
          "class Value : IValue { } class Maybe : IMaybe { Value value() => Value(); }");
    CLEAN("interface R<T> { R<T> self(); } class C : R<int> { C self() => this; }");
    CLEAN("interface IBase { IBase self(); } interface IRefined : IBase { IRefined self(); } "
          "class C : IRefined { C self() => this; }");
    CLEAN("interface I { I self(); } class C : I { distinct C self() => this; }");
    ERROR_HAS("interface I { I make(int n); } class Wrong { } "
              "class C : I { Wrong make(int n) => Wrong(); }",
              "return type 'Wrong' is not assignable to required 'I'");
    ERROR_HAS("interface I { I make(int n); } class C : I { C make(string n) => this; }",
              "missing 'make : (int) -> I'");
    ERROR_HAS("interface I { I | string maybe(); } class C : I { C maybe() => this; }",
              "return type 'C' is not assignable to required 'I | string'");
    // Class overrides are deliberately unchanged: a narrowed return coexists
    // as a second slot rather than overriding the base declaration.
    CLEAN("class A { A self() => this; } class B : A { B self() => this; }");

    // --- F5 weak fields ---
    CLEAN("class N { weak N? parent = None; weak readonly N? root = None; } "
          "void g() { N a = N(); N b = N(); a.parent = b; N? p = a.parent; a.parent = None; }");
    CLEAN("class N { } class A { distinct weak N? link = None; } "
          "class B { distinct weak N? link = None; } class C : A, B { }");
    ERROR_HAS("class N { weak N parent; }", "weak field must have optional class/interface type");
    ERROR_HAS("class N { weak string? text = None; }", "weak field must have optional class/interface type");
    ERROR_HAS("class N { weak Array<N>? xs = None; }", "weak field must have optional class/interface type");
    ERROR_HAS("struct N { weak N? parent = None; }", "weak fields are allowed on classes only");
    ERROR_HAS("class N { weak const N? parent = None; }", "weak const is meaningless");
    ERROR_HAS("class N { } void g() { weak N? p = None; }", "weak applies to instance fields only");
    ERROR_HAS("class N { weak int f() => 1; }", "weak applies to instance fields only");
    // A weak field is deliberately not a stable flow path: copy it first.
    ERROR_HAS("class N { weak N? parent = None; int n = 1; "
              "int bad() { if (parent != None) return parent.n; return 0; } }",
              "narrow the union before member access");
    CLEAN("class N { weak N? parent = None; int n = 1; "
          "int good() { N? p = parent; if (p != None) return p.n; return 0; } }");

    // --- LA-18: generic static-shaped member resolution ---
    CLEAN("class N { new N() { } new FromInt(int n) { } } "
          "A make<A>(A witness, int n) => A::FromInt(n); "
          "void g() { N n = N(); N x = make(n, 1); }");
    CLEAN("struct N { new N() { } new FromInt(int n) { } } "
          "A make<A>(A witness, int n) => A::FromInt(n); "
          "void g() { N n = N(); N x = make(n, 1); }");
    ERROR_HAS("class N { new N() { } } "
              "A make<A>(A witness, int n) => A::FromInt(n); "
              "void g() { N n = N(); make(n, 1); }",
              "type 'N' (instantiating 'A' of 'make<A>') has no labeled constructor 'FromInt'");
    ERROR_HAS("class C<T> { T bad() => T::Missing(); }",
              "class-level type parameter");
    ERROR_HAS("F<A> bad<F, A>(F<A> witness) => F::Missing();",
              "type-constructor parameter 'F'");
    CLEAN("class N { new N() { } new FromInt(int n) { } } "
          "A inner<A>(A witness, int n) => A::FromInt(n); "
          "A outer<A>(A witness, int n) => inner(witness, n); "
          "void g() { N n = N(); N x = outer(n, 1); }");
    ERROR_HAS("class N { new N() { } new FromInt(int n) { } } "
              "class B { A make<A>(A w, int n) => A::FromInt(n); } "
              "class D : B { A make<A>(A w, int n) => A::FromInt(n); }",
              "overrides or is overridden");

    // --- bug #54: per-instantiation overload resolution in generic bodies ---
    // Positive: a generic body's overloaded call on a type-parameter value
    // re-resolves cleanly per concrete instantiation (int and string both apply).
    CLEAN("namespace P { string enc(int v) => \"I\"; string enc(string v) => \"S\"; "
          "Array<string> arr<T>(Array<T> v) { Array<string> o = []; "
          "for (T x in v) { o = o.add(P::enc(x)); } return o; } } "
          "void g() { Array<int> a = [1]; Array<string> b = [\"y\"]; "
          "P::arr(a); P::arr(b); }");
    // Missing overload at instantiation: a `T` with no applicable overload gives
    // the two-span diagnostic (use site + instantiation note), naming the generic.
    ERROR_HAS("namespace P { string enc(int v) => \"I\"; string enc(string v) => \"S\"; "
              "Array<string> arr<T>(Array<T> v) { Array<string> o = []; "
              "for (T x in v) { o = o.add(P::enc(x)); } return o; } } "
              "class W { int q = 1; } "
              "void g() { Array<W> ws = [W()]; P::arr(ws); }",
              "in specialization of 'arr<T>'");

    // --- system-binds.md §5: Channel 1 — `use NS::T;` activates T's bind ---
    // Negative acceptance (§9 M1): IEnv is visible everywhere (the implicit
    // `uses std;`) but its root bind is inert without an explicit `use` — no
    // textual bind, no activation, so injection fails.
    ERROR_HAS("string f(IEnv e) => \"ok\"; void g() { f(); }",
              "missing required argument");
    // §5.1 rule 2: a file-level `use std::IEnv;` activates the root bind,
    // filling the injection with no textual bind anywhere (Atlantis probe A9).
    CLEAN("use std::IEnv; string f(IEnv e) => \"ok\"; void g() { f(); }");
    // §5.1: an alias changes the name only; activation is identical under `as`.
    CLEAN("use std::IEnv as E; string f(E e) => \"ok\"; void g() { f(); }");
    // §5.1 rule 3: `uses std;` (bulk import) never activates, aliased or not —
    // only a selective `use` of the type does.
    ERROR_HAS("uses std; string f(IEnv e) => \"ok\"; void g() { f(); }",
              "missing required argument");
    // §5.1: block-level `use` activates only within its own block; a sibling
    // function with no `use` of its own still fails at ITS call site (the
    // bind must enclose the call site, §6.1) even though both call the same
    // `f`.
    ERROR_HAS("string f(IEnv e) => \"ok\"; "
              "void g() { use std::IEnv; f(); } "
              "void h() { f(); }",
              "missing required argument");
    CLEAN("string f(IEnv e) => \"ok\"; void g() { use std::IEnv; f(); }");
    // §5.1 rule 4: textual beats activated, silently, in the same scope — no
    // duplicate-bind error even though a `use` and a `bind` both claim IEnv.
    CLEAN("use std::IEnv; "
          "class FakeEnv : IEnv { "
          "  Array<string> args() => []; "
          "  string? variable(string n) => None; "
          "} "
          "bind IEnv => FakeEnv(); "
          "string f(IEnv e) => \"ok\"; void g() { f(); }");
    // The request's own test idiom (§6.2): `use` for the name + system bind,
    // then a block-scoped fake shadows it (nearest-wins, §5.1 rule 6) without
    // needing its own `use` (the outer one already brought the name in).
    CLEAN("use std::IEnv; "
          "class FakeEnv : IEnv { "
          "  Array<string> args() => []; "
          "  string? variable(string n) => None; "
          "} "
          "string f(IEnv e) => \"ok\"; "
          "void g() { bind IEnv => FakeEnv(); f(); } "
          "void h() { f(); }");
    // A duplicate `use` of the same type in one scope dedupes to the one
    // namespace bind (§5.1 rule 5) rather than erroring.
    CLEAN("use std::IEnv; use std::IEnv as IEnv2; "
          "string f(IEnv e) => \"ok\"; void g() { f(); }");
    // A user-namespace bind, activated cross-file (here: cross-scope, same
    // buffer) by `use` — Channel 1 is not prelude-only.
    CLEAN("namespace App { interface IWidget { } class Widget : IWidget { } "
          "bind IWidget => Widget(); } "
          "use App::IWidget; "
          "string f(IWidget w) => \"ok\"; void g() { f(); }");

    // --- Struct-equality packet 03: the loud comparability gate at ==/!= ---
    // A value struct with a non-comparable field has no synthesized (==); the
    // gate fires at the USE site, naming the first bad field (§5.1). Operands
    // are parameters so the story is purely about the (==) gate.
    ERROR_HAS("struct Job { Array<int> xs; } void f(Job a, Job b) { bool c = a == b; }",
              "field 'xs'");
    // A function-value field is non-comparable ("a function value").
    ERROR_HAS("struct Job { (int) => int fn; } void f(Job a, Job b) { bool c = a == b; }",
              "not comparable");
    // A Map field is non-comparable; the message points at the opt-in escape.
    ERROR_HAS("struct Job { Map<int, int> m; } void f(Job a, Job b) { bool c = a == b; }",
              "define an explicit");
    // !=/gate parity: the derived (!=) path also hits the gate.
    ERROR_HAS("struct Job { Array<int> xs; } void f(Job a, Job b) { bool c = a != b; }",
              "has no '(==)'");
    // The red-corpus equivalent lives here (compile errors don't run engines,
    // so the composition red lane — for wrong-OUTPUT bugs — is not the home).
    // POSITIVE: an all-comparable value struct still type-checks under ==.
    CLEAN("struct Point { int x; int y; } void f(Point a, Point b) { bool c = a == b; }");

    // --- Struct-equality packet 06: the always-false `== float::NaN` error ---
    // An OPERATOR compare against the NaN constant is statically always-false
    // (`==`) / always-true (`!=`) under IEEE (§4), so it is a compile error
    // with a fixit — never a silent constant result.
    ERROR_HAS("void f(float x) { bool b = x == float::NaN; }", "use x.isNaN()");
    // Mirror shape for `!=` (and operand order): the constant on either side.
    ERROR_HAS("void f(float x) { bool b = float::NaN != x; }", "use x.isNaN()");
    // The documented escape hatch: an aliased read through a variable is NOT
    // the constant node (the match is on the resolved decl, not the spelling) —
    // honestly IEEE-false at runtime, so it stays legal.
    CLEAN("void f(float x) { float n = float::NaN; bool b = x == n; }");

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
