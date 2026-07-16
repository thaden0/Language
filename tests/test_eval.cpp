// Evaluator tests: run programs and assert their console output. This is the
// semantics oracle confirming the runtime model behaves as designed.
#include "Checker.hpp"
#include "Eval.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Resolver.hpp"
#include "RuntimeLoop.hpp"
#include <cstdio>
#include <string>
#include <unistd.h>

static int g_checks = 0;
static int g_failures = 0;

// Type-check then run `src`; return captured output (or "<errors>" if it didn't check).
static std::string run(const std::string& src) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    Parser parser(lexer.tokenize(), f, sink);
    Program prog = parser.parseProgram();
    Resolver r(f, sink);
    r.run(prog);
    Checker c(r.sema(), f, sink);
    c.run(prog);
    if (sink.hasErrors()) return "<errors>";
    Evaluator e(r.sema(), sink);
    e.initGlobals(r.preludeProgram());
    return e.run(prog);
}

static void expectEq(const std::string& got, const std::string& want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("  FAIL %s\n    want: %s\n    got:  %s\n", what, want.c_str(), got.c_str());
    }
}
#define OUT(src, want) expectEq(run(src), want, src)

int main() {
    // §3: bare object declaration AUTO-CONSTRUCTS (no null/unbound state).
    OUT("class C { int n = 0; new C() { n = 42; } int v() => n; } "
        "void run() { C c; console.writeln(c.v()); } run();", "42\n");
    // Compound assignment on an object dispatches its (+) operator (not int math).
    OUT("class N { int n = 0; new N() { n = 10; } N mk(int v) { N x = N(); x.n = v; return x; } "
        "N (+)(int d) => mk(n + d); int v() => n; } "
        "void run() { N a = N(); a += 5; console.writeln(a.v()); } run();", "15\n");

    // Construction + field default + method + console.
    OUT("class C { int x = 0; new C() { x = 5; } int read() => x; } "
        "void run() { C c = C(); console.writeln(c.read()); } run();", "5\n");

    // Constructor overload selection by label and by arity.
    OUT("class C { int x = 0; new C() { x = 1; } new Seed(int v) { x = v; } int g() => x; } "
        "void run() { console.writeln(C().g()); console.writeln(C::Seed(9).g()); } run();",
        "1\n9\n");

    // Operators are methods; a fresh object is returned, then read via a method.
    OUT("class N { int n = 0; new W(int v) { n = v; } "
        "N (+)(int d) => N::W(n + d); int v() => n; } "
        "void run() { console.writeln((N::W(3) + 4).v()); } run();", "7\n");

    // get/set as views over a slot: set doubles on write, get reads raw.
    OUT("class Box { int v = 0; get v() => v; set v(int x) v = x * 2; } "
        "void run() { Box b = Box(); b.v = 10; console.writeln(b.v); } run();", "20\n");

    // distinct across multiple inheritance, reached by :: qualification.
    OUT("class A { distinct int v = 1; } class B { distinct int v = 2; } "
        "class C : A, B { int sum() => this.A::v + this.B::v; } "
        "void run() { console.writeln(C().sum()); } run();", "3\n");

    // Non-distinct, different-named members via inheritance; bare access is fine.
    OUT("class A { int a = 10; } class B { int b = 20; } "
        "class C : A, B { int sum() => a + b; } "
        "void run() { console.writeln(C().sum()); } run();", "30\n");

    // Control flow, return, ternary.
    OUT("int sign(int k) { if (k < 0) return 0; return 1; } "
        "void run() { console.writeln(sign(0 - 3)); console.writeln(sign(3)); } run();", "0\n1\n");
    OUT("int pick(bool t) => t ? 1 : 2; "
        "void run() { console.writeln(pick(true)); console.writeln(pick(false)); } run();", "1\n2\n");

    // Reference semantics: two names share one object; a mutation is visible via both.
    OUT("class Cell { int v = 0; set v(int x) v = x; get v() => v; } "
        "void run() { Cell a = Cell(); Cell b = a; a.v = 42; console.writeln(b.v); } run();",
        "42\n");

    // Loops: for and while.
    OUT("int sumTo(int n) { int t = 0; for (int i = 1; i <= n; i = i + 1) t = t + i; return t; } "
        "void run() { console.writeln(sumTo(5)); } run();", "15\n");
    OUT("int f(int n) { int s = 0; while (n > 0) { n = n - 1; s = s + 1; } return s; } "
        "void run() { console.writeln(f(4)); } run();", "4\n");

    // Higher-order function + function-typed parameter + closure capture.
    OUT("int apply((int) => int f, int x) => f(x); "
        "int run2() { int k = 10; return apply((n) => n + k, 5); } "
        "void run() { console.writeln(run2()); } run();", "15\n");   // closure captures k
    OUT("int twice((int) => int f, int x) => f(f(x)); "
        "void run() { console.writeln(twice((n) => n * 2, 3)); } run();", "12\n");

    // Ranges + for-in (inclusive range).
    OUT("void run() { int t = 0; for (int i in 1..5) t = t + i; console.writeln(t); } run();",
        "15\n");
    OUT("void run() { for (int j in 1..3) console.writeln(j); } run();", "1\n2\n3\n");

    // Object mask over primitives: methods on unboxed values (this = the value).
    OUT("void run() { int n = 0 - 7; console.writeln(n.abs()); } run();", "7\n");   // in-language
    OUT("void run() { console.writeln((3).max(9)); } run();", "9\n");
    OUT("void run() { console.writeln(\"Hello\".length()); } run();", "5\n");        // native
    OUT("void run() { console.writeln(\"Hello\".toUpper()); } run();", "HELLO\n");
    OUT("void run() { console.writeln(\"Hello\".subStr(0, 2)); } run();", "He\n");
    OUT("void run() { console.writeln(\"Hello\".contains(\"ell\")); } run();", "true\n");
    OUT("void run() { console.writeln((42).toString() + \"!\"); } run();", "42!\n");

    // Arrays: LINQ/JS methods written in-language over the native core.
    OUT("void run() { Array a = [1,2,3]; console.writeln(a.length()); } run();", "3\n");
    OUT("void run() { Array a = [1,2,3,4]; console.writeln(a.where((n) => n % 2 == 0).joinToString(\",\")); } run();",
        "2,4\n");
    OUT("void run() { Array a = [1,2,3]; console.writeln(a.map((n) => n * 10).joinToString(\",\")); } run();",
        "10,20,30\n");
    OUT("void run() { Array a = [1,2,3,4]; console.writeln(a.reduce(0, (x, y) => x + y)); } run();",
        "10\n");
    OUT("void run() { Array a = [1,2,3]; console.writeln(a.reverse().joinToString(\",\")); } run();", "3,2,1\n");
    OUT("void run() { Array a = [1,2,3,4,5]; console.writeln(a.take(2).joinToString(\",\")); "
        "console.writeln(a.skip(3).joinToString(\",\")); } run();", "1,2\n4,5\n");
    OUT("void run() { Array a = [1,2,3]; console.writeln(a.contains(2)); console.writeln(a.indexOf(3)); } run();",
        "true\n2\n");
    OUT("void run() { Array a = [1,2,3]; console.writeln(a.concat([4,5]).joinToString(\",\")); } run();",
        "1,2,3,4,5\n");
    OUT("void run() { Array a = [1,2,3,4]; console.writeln(a.any((n) => n > 3)); "
        "console.writeln(a.all((n) => n > 0)); console.writeln(a.count((n) => n % 2 == 0)); } run();",
        "true\ntrue\n2\n");

    // Relational joins (return pairs; U inferred from the other array).
    OUT("void run() { Array a = [1,2,3]; Array b = [2,3,4]; "
        "Array j = a.join(b, (x, y) => x == y); console.writeln(j.length()); "
        "for (Pair p in j) console.writeln(p.first); } run();", "2\n2\n3\n");
    OUT("void run() { Array a = [1,2,3]; Array b = [2,3,4]; "
        "Array g = a.groupJoin(b, (x, y) => x == y); "
        "for (Pair p in g) console.writeln(p.second.length()); } run();", "0\n1\n1\n");

    // Method-level generics run: identity + container remap.
    OUT("R identity<R>(R x) => x; void run() { console.writeln(identity(42)); } run();", "42\n");

    // `uses` imports a namespace's names into scope (alt to Full::Name).
    OUT("namespace M { public class W { int val() => 7; } int helper(int x) => x + 1; } "
        "uses M; "
        "void run() { W w = W(); console.writeln(w.val()); console.writeln(helper(4)); "
        "console.writeln(M::helper(9)); } run();", "7\n5\n10\n");

    // Indexers via ([]) accessor. Array read + pure-array a[i]=v rebind.
    OUT("void run() { Array a = [10,20,30]; console.writeln(a[0]); console.writeln(a[2]); } run();",
        "10\n30\n");
    OUT("void run() { Array a = [1,2,3]; a[1] = 99; console.writeln(a.joinToString(\",\")); } run();",
        "1,99,3\n");
    // User class defining get/set ([]) (mutable wrapper); Array<int> field auto-inits to [].
    OUT("class Grid { Array<int> cells; new Sized(int n) { for (int i in 1..n) cells = cells.add(0); } "
        "get ([])(int i) => cells[i]; set ([])(int i, int v) cells[i] = v; } "
        "void run() { Grid g = Grid::Sized(3); g[1] = 7; console.writeln(g[1]); console.writeln(g[0]); } run();",
        "7\n0\n");

    // Overload resolution by argument type (methods, free functions, operators, ctors).
    OUT("class C { string d(int n) => \"int\"; string d(string s) => \"str\"; } "
        "void run() { C c = C(); console.writeln(c.d(5)); console.writeln(c.d(\"x\")); } run();",
        "int\nstr\n");
    OUT("string k(int x) => \"i\"; string k(string x) => \"s\"; "
        "void run() { console.writeln(k(3)); console.writeln(k(\"y\")); } run();", "i\ns\n");
    OUT("class N { int n = 0; new Of(int v){n=v;} int v() => n; "
        "N (+)(int d) => N::Of(n+d); N (+)(N o) => N::Of(n+o.v()); } "
        "void run() { N a = N::Of(2); console.writeln((a + 3).v()); console.writeln((a + a).v()); } run();",
        "5\n4\n");
    OUT("class B { int tag; new One(int a){tag=1;} new Two(int a, int b){tag=2;} int t()=>tag; } "
        "void run() { console.writeln(B::One(0).t()); console.writeln(B::Two(0,0).t()); } run();",
        "1\n2\n");

    // Array construction (sized/filled), [1..n] range->array, range printing, spread.
    OUT("void run() { Array<bool> a = Array(3, false); console.writeln(a.joinToString(\",\")); } run();",
        "false,false,false\n");
    OUT("void run() { Array<int> a = [1..5]; console.writeln(a.joinToString(\",\")); } run();",
        "1,2,3,4,5\n");
    OUT("void run() { console.writeln(1..10); } run();", "1..10\n");
    OUT("void run() { Array<int> a = [1..3, 7, 9..10]; console.writeln(a.joinToString(\",\")); } run();",
        "1,2,3,7,9,10\n");

    // Compound assignment.
    OUT("void run() { int x = 5; x += 3; x *= 2; console.writeln(x); } run();", "16\n");
    OUT("void run() { string s = \"a\"; s += \"b\"; s += \"c\"; console.writeln(s); } run();", "abc\n");

    // Sieve of Eratosthenes (sized array + += + for-in ranges + indexer).
    OUT("void sieve() { int n = 20; Array<bool> c = Array(n + 1, false); "
        "for (int i in 2..n) { if (!c[i]) { for (int j = i*i; j <= n; j += i) c[j] = true; } } "
        "for (int i in 2..n) if (!c[i]) console.writeln(i); } sieve();",
        "2\n3\n5\n7\n11\n13\n17\n19\n");

    // Map<K,V>: pure associative value; m[k]=v rebinds; iterates as Pair.
    OUT("void run() { Map<string, int> m; m[\"a\"] = 1; m[\"b\"] = 2; m[\"a\"] = 3; "
        "console.writeln(m.length()); console.writeln(m[\"a\"]); console.writeln(m.has(\"b\")); } run();",
        "2\n3\ntrue\n");
    OUT("void run() { Map<string, int> m; m[\"x\"] = 1; m[\"y\"] = 2; "
        "console.writeln(m.keys().joinToString(\",\")); console.writeln(m); } run();",
        "x,y\n{x: 1, y: 2}\n");
    OUT("void run() { Map<string, int> m; m[\"a\"] = 10; m[\"b\"] = 20; "
        "Map<string, int> f = m.without(\"a\"); "
        "console.writeln(f.length()); console.writeln(m.length()); } run();", "1\n2\n");   // purity
    OUT("void run() { Map<string, int> m; m[\"a\"] = 30; m[\"b\"] = 26; int t = 0; "
        "for (Pair e in m) t += e.second; console.writeln(t); } run();", "56\n");

    // HKT: container-preserving generic functions (F bound to the constructor head).
    OUT("F<B> mapIt<F, A, B>(F<A> c, (A) => B fn) => c.map(fn); "
        "void run() { Array<int> a = [1,2,3]; Array<int> d = mapIt(a, (n) => n * 2); "
        "console.writeln(d.joinToString(\",\")); } run();", "2,4,6\n");

    // Exceptions: catch by exact type / base class / interface; binding reads message.
    OUT("void run() { try { throw RuntimeException(\"boom\"); } "
        "catch (RuntimeException e) { console.writeln(\"c: \" + e.message); } } run();",
        "c: boom\n");
    OUT("void run() { try { throw LogicException(\"bad\"); } "
        "catch (IException e) { console.writeln(e.message); } } run();", "bad\n");
    OUT("void run() { try { throw RuntimeException(\"rt\"); } "
        "catch (Exception e) { console.writeln(e.message); } } run();", "rt\n");
    // First matching clause; propagation through a call; continue after catch.
    OUT("int div(int a, int b) { if (b == 0) throw RuntimeException(\"dz\"); return a / b; } "
        "void run() { try { int x = div(1, 0); console.writeln(\"no\"); } "
        "catch (ILogicException e) { console.writeln(\"wrong\"); } "
        "catch (IRuntimeException e) { console.writeln(\"p: \" + e.message); } "
        "console.writeln(div(10, 2)); } run();", "p: dz\n5\n");
    // Uncaught: reported, execution stops.
    OUT("void run() { throw LogicException(\"oops\"); } run(); console.writeln(\"never\");",
        "Uncaught LogicException: oops\n");

    // Loud runtime failures: thrown as catchable RuntimeExceptions.
    OUT("void run() { Array a = [1,2,3]; "
        "try { int x = a[9]; } catch (RuntimeException e) { console.writeln(e.message); } } run();",
        "index 9 out of bounds (length 3)\n");
    // bug.md #10: integer div/mod by zero throws a catchable RuntimeException
    // (not a silent 0). Floats keep IEEE inf/nan (bug.md #12), so no throw there.
    OUT("void run() { try { int x = 5 / 0; console.writeln(x); } "
        "catch (RuntimeException e) { console.writeln(e.message); } } run();",
        "division by zero\n");
    OUT("void run() { try { int x = 5 % 0; console.writeln(x); } "
        "catch (RuntimeException e) { console.writeln(e.message); } } run();",
        "modulo by zero\n");
    // A throw INSIDE `return <expr>;` must propagate to an enclosing try/catch,
    // not be masked as a normal void return (was an oracle-only miss — the IR /
    // emit-C++ / LLVM engines already propagated; found via #10's catch test).
    OUT("int sd(int a, int b) { try { return a / b; } "
        "catch (RuntimeException e) { return -1; } } "
        "void run() { console.writeln(sd(10, 2)); console.writeln(sd(10, 0)); } run();",
        "5\n-1\n");
    OUT("void run() { Map<string, int> m; m[\"a\"] = 1; "
        "try { int x = m.at(\"zz\"); } catch (RuntimeException e) { console.writeln(\"missing\"); } } run();",
        "missing\n");
    // (!=) derived from (==) at runtime.
    OUT("class C { int n = 5; bool (==)(int v) => n == v; } "
        "void run() { C c = C(); console.writeln(c != 5); console.writeln(c != 9); } run();",
        "false\ntrue\n");

    // Streams: queue substrate, typed views, extract, subscribe.
    OUT("void run() { StreamBuffer<int> b = StreamBuffer(); OutStream<int> w = OutStream(b); "
        "InStream<int> r = InStream(b); w << 4 << 2; console.writeln(r >> ); "
        "console.writeln(r.pull()); } run();", "4\n2\n");
    OUT("void run() { StreamBuffer<int> b = StreamBuffer(); IOStream<int> d = IOStream(b); "
        "d << 9; int x = d >> ; console.writeln(x); } run();", "9\n");
    OUT("void run() { StreamBuffer<string> b = StreamBuffer(); OutStream<string> w = OutStream(b); "
        "InStream<string> r = InStream(b); w << \"a\"; "
        "r.subscribe((s) => console.writeln(\"got \" + s)); w << \"b\"; } run();",
        "got a\ngot b\n");

    // String concatenation and mixed console output.
    OUT("void run() { console.writeln(\"x=\" + 5); } run();", "x=5\n");

    // Regression (2026-07-05 maintenance): a watch whose fd is closed under
    // it must wake once (POLLNVAL counts as ready) and then be auto-retired
    // by the registry — before the fix, nextBatch() ignored POLLNVAL, so
    // poll() returned instantly with an event nobody consumed and the loop
    // busy-spun forever. Driven directly against RuntimeLoop: no sockets, a
    // closed pipe end is enough.
    {
        ++g_checks;
        RuntimeLoop& loop = RuntimeLoop::instance();
        loop.reset();
        int pfd[2];
        if (::pipe(pfd) == 0) {
            int fires = 0;
            loop.addWatch(pfd[0], vint(0));   // callback payload unused here
            ::close(pfd[0]);                   // fd dies under the watch
            ::close(pfd[1]);
            // pre-fix this loop never terminates: hasWork() stays true and
            // nextBatch() returns empty batches forever.
            for (int i = 0; i < 100 && loop.hasWork(); ++i)
                fires += (int)loop.nextBatch().size();
            if (loop.hasWork() || fires != 1) {
                ++g_failures;
                std::printf("  FAIL dead-fd watch retire: hasWork=%d fires=%d\n",
                            loop.hasWork() ? 1 : 0, fires);
            }
        }
        loop.reset();
    }

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
