// Rule-stage (§16.5) Phase 1 tests: attributes (Layer A) + comptime (Layer C).
// Drives the same pipeline main.cpp uses — parse, resolve pass 1, RuleEngine,
// then (when the tree changed) resolve pass 2 + check — and asserts on the
// diagnostics. The zero-cost guard checks hasMeta stays false for plain code.
#include "Checker.hpp"
#include "Lexer.hpp"
#include "Parser.hpp"
#include "Project.hpp"
#include "Resolver.hpp"
#include "Rules.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

static int g_checks = 0;
static int g_failures = 0;

struct MetaResult {
    bool hadError = false;
    bool hasMeta = false;
    bool changed = false;
    std::string firstError;
};

// `importRoot` wires LA-20's ImportContext in single-file (rootDir) mode when
// non-null (even ""), so import()-exercising sources resolve against a real
// directory instead of leaving importFn unset (which would just run
// std::import's ordinary — throwing — body, since the intercept only fires
// under comptime_ && importCtx_.importFn).
static MetaResult runMeta(const std::string& src, long long budget = 0,
                          const char* importRoot = nullptr,
                          const char* targetTriple = nullptr) {
    SourceFile f{"<test>", src};
    DiagnosticSink sink;
    Lexer lexer(f, sink);
    Parser parser(lexer.tokenize(), f, sink);
    Program prog = parser.parseProgram();
    Resolver r(f, sink);
    r.run(prog);

    MetaResult res;
    res.hasMeta = prog.hasMeta;
    if (prog.hasMeta && !sink.hasErrors()) {
        std::vector<ProjectFile> files{{f.name, 0, (uint32_t)f.text.size(), "", ""}};
        ComptimeOptions opts;
        if (budget > 0) opts.stepBudget = budget;
        if (targetTriple) opts.targetTriple = targetTriple;
        RuleEngine engine(files, r.sema(), r.preludeProgram(), f, sink, opts);
        if (importRoot) {
            ImportContext ictx;
            ictx.rootDir = importRoot;
            engine.setImportContext(ictx);
        }
        res.changed = engine.run(prog);
        if (res.changed && !sink.hasErrors()) {
            Resolver r2(f, sink);
            r2.run(prog);
            Checker c(r2.sema(), f, sink);
            c.run(prog);
        } else if (!sink.hasErrors()) {
            Checker c(r.sema(), f, sink);
            c.run(prog);
        }
    } else if (!sink.hasErrors()) {
        Checker c(r.sema(), f, sink);
        c.run(prog);
    }
    res.hadError = sink.hasErrors();
    for (const Diagnostic& d : sink.all())
        if (d.severity == Severity::Error) { res.firstError = d.message; break; }
    return res;
}

static void expect(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("  FAIL: %s\n", what); }
}
#define ERRORS(src)  expect(runMeta(src).hadError, "should error: " src)
#define CLEAN(src)   expect(!runMeta(src).hadError, "should be clean: " src)

int main() {
    // --- Zero-cost guard: plain programs carry no meta flag -----------------
    expect(!runMeta("int f() => 1; console.writeln(f());").hasMeta,
           "plain program must not set hasMeta (rule stage stays off)");
    expect(runMeta("comptime int n = 1;").hasMeta, "comptime sets hasMeta");
    expect(runMeta("attribute T { int n; } @T(1) void f() {}").hasMeta,
           "attributes set hasMeta");

    // --- Attributes: resolution (M01/M02) -----------------------------------
    ERRORS("@Nope(1) void f() {}");                       // unknown attribute
    CLEAN("attribute T { int n; } @T(1) void f() {}");
    CLEAN("namespace A { attribute Tag { int n; } } uses A; @Tag(1) void f() {}");
    CLEAN("namespace A { attribute Tag { int n; } } uses A; @A::Tag(1) void f() {}");
    ERRORS("namespace A { attribute Tag { int n; } } "     // ambiguous across imports
           "namespace B { attribute Tag { int n; } } "
           "uses A; uses B; @Tag(1) void f() {}");
    CLEAN("namespace A { attribute Tag { int n; } } "      // qualification resolves it
          "namespace B { attribute Tag { int n; } } "
          "uses A; uses B; @A::Tag(1) void f() {}");
    // A file's OWN namespaces are always in scope (§5.1: imports(F) includes
    // the namespaces F opens) — single-file programs see their attributes
    // without a `uses`. The cross-file not-imported negative lives in
    // tests/corpus/project/attr_scope_err (needs two physical files).
    CLEAN("namespace A { attribute Tag { int n; } } @Tag(1) void f() {}");

    // --- Attributes: arguments (M03/M04) -------------------------------------
    ERRORS("attribute T { string a; } @T(42) void f() {}");        // type mismatch
    ERRORS("attribute T { string a; } @T(\"x\", \"y\") void f() {}"); // arity over
    ERRORS("attribute T { string a; } @T void f() {}");            // missing, no default
    CLEAN("attribute T { string a = \"d\"; } @T void f() {}");     // default fills
    CLEAN("attribute T { int n; bool b = true; } @T(3) void f() {}");
    ERRORS("attribute T { int n; } int rt = 5; @T(rt) void f() {}"); // not comptime-const

    // --- Attribute declarations (M17 + field types) --------------------------
    ERRORS("attribute T { int n; void m() {} }");          // methods forbidden
    ERRORS("attribute T { Array<int> xs; }");              // non-primitive field
    ERRORS("@T(1)\nint x = 3;\nconsole.writeln(x);\n@T(2);"); // attr before non-decl

    // --- Comptime folding ----------------------------------------------------
    CLEAN("comptime int n = 2 + 3; console.writeln(n);");
    CLEAN("comptime int a = 4; comptime int b = a * 2; console.writeln(b);");
    CLEAN("int y = 1 + comptime 2 * 3; console.writeln(y);");
    CLEAN("comptime Array<int> xs = [1,2,3].map((v) => v + 1); console.writeln(xs);");
    ERRORS("comptime int n;");                             // needs an initializer
    // Type mismatch surfaces in pass-2 check on the folded literal
    ERRORS("comptime int n = \"oops\"; console.writeln(n);");

    // --- comptime if ----------------------------------------------------------
    CLEAN("comptime int n = 1; comptime if (n == 1) console.writeln(\"a\"); "
          "else console.writeln(\"b\");");
    ERRORS("comptime if (3) console.writeln(\"a\");");     // condition must be bool

    // §9 (Phase 3): the M15 restriction lifts — `uses` inside a comptime-if
    // is legal. A top-level comptime-if's taken Block splices its statements
    // inline (not nested), so the `uses` lands where scoping can see it and
    // helper() resolves through it.
    CLEAN("namespace A { int helper() => 1; } "
          "comptime if (true) { uses A; console.writeln(helper()); }");
    CLEAN("namespace A { int helper() => 1; } "            // untaken branch: fine too
          "comptime if (false) { uses A; } "
          "else { console.writeln(\"no helper needed\"); }");

    // block-scoped-use §3.2(a)/§5 P3(b): a comptime-if spliced into a nested
    // BLOCK carries its `uses` into that block's own scope — pass 2 re-resolves
    // the folded tree and re-materializes the block scope (the unconditional
    // reset guarantees no stale pass-1 scope survives), so an in-block call
    // resolves through the spliced import...
    CLEAN("namespace A { int helper() => 1; } "
          "void f() { comptime if (true) { uses A; console.writeln(helper()); } }");
    // ...and the import stays confined: a call outside that block errors
    // (the spliced block scope does not leak — confinement falls out of pop).
    ERRORS("namespace A { int helper() => 1; } "
           "void f() { { comptime if (true) { uses A; } } int x = helper(1); }");

    // --- Hermeticity (M12) and budget (M13) ----------------------------------
    ERRORS("comptime string s = std::sysReadLine(0);");
    ERRORS("comptime int n = std::sysOpen(\"/etc/passwd\", 1);");
    CLEAN("comptime int n = std::sysWrite(1, \"\");");     // stdout is the allowance
    {
        MetaResult r = runMeta(
            "int spin() { int i = 0; while (true) i = i + 1; return i; } "
            "comptime int n = spin();", /*budget=*/20000);
        expect(r.hadError, "runaway comptime loop must exhaust the step budget");
        expect(r.firstError.find("budget") != std::string::npos,
               "budget exhaustion names the budget");
    }
    {   // budget exhaustion is NOT catchable by comptime try/catch
        MetaResult r = runMeta(
            "int spin() { try { while (true) { } } catch (IException e) return 1; "
            "return 2; } comptime int n = spin();", /*budget=*/20000);
        expect(r.hadError, "budget exhaustion must not be try-swallowed");
    }

    // --- Rules (Layer B): the happy path and its guards ----------------------
    // A rule that fires and injects a real call must leave a checkable tree.
    CLEAN("namespace W { interface I {} attribute R { string p; } "
          "rule reg { match @R(a) on method m in class C : I "
          "inject `log.hit($a.p)` at bottom of C.constructor } "
          "class Log { string s = \"\"; Log hit(string p) => this; } } "
          "uses W; class Ctl : I { Log log; new Ctl(Log l){log=l;} "
          "@R(\"/x\") int f() => 1; } Log g = Log(); Ctl c = Ctl(g);");

    // Unbound hole: the template references $q, which the match never binds.
    ERRORS("namespace W { attribute R { string p; } "
           "rule reg { match @R(a) on class C "
           "inject `int y = $q;` at member of C } } "
           "uses W; @R(\"z\") class T { }");

    // Whole-attribute splice ($a with no field) is an error — splice a field.
    ERRORS("namespace W { attribute R { string p; } "
           "rule reg { match @R(a) on class C "
           "inject `int y = $a;` at member of C } } "
           "uses W; @R(\"z\") class T { }");

    // :IFace constraint gates the match — a class NOT implementing I: no fire,
    // so the injected reference never appears and the program stays clean.
    CLEAN("namespace W { interface I {} attribute R { string p; } "
          "rule reg { match @R(a) on method m in class C : I "
          "inject `nope.hit()` at bottom of C.constructor } } "
          "uses W; class Plain { @R(\"/x\") int f() => 1; }");

    // `member of` conflict: injected member collides with an existing same-typed
    // member of the same name.
    ERRORS("namespace W { attribute R { string p; } "
           "rule reg { match @R(a) on class C "
           "inject `string describe() => $a.p;` at member of C } } "
           "uses W; @R(\"z\") class T { string describe() => \"x\"; }");

    // --- Layer D (Phase 4 §2): body-replacing rules (rewrites/replace/$body) --
    // The happy path: a `rewrites body of m` rule with one `replace` that
    // splices `$body` exactly once resolves + checks clean (output proofs live
    // in tests/corpus/meta/rule_timed|rule_memoize|rule_body_replace_arrow).
    CLEAN("namespace W { attribute Timed {} rule t rewrites body of m { "
          "match @Timed on method m replace `var r = $body; return r;` } } "
          "uses W; class K { @Timed int f(int n) => n * 2; } K k = K(); k.f(3);");
    // M35: `rewrites body of` a bind that is not a callable (a field here).
    ERRORS("namespace W { attribute R {} rule reg rewrites body of f { "
           "match @R on field f replace `return $body;` } }");
    // M35: `rewrites body of` a name the match clause never binds.
    ERRORS("namespace W { attribute R {} rule reg rewrites body of zzz { "
           "match @R on method m replace `return $body;` } }");
    // M32: a `rewrites` rule that never references `$body` (silent obliteration).
    ERRORS("namespace W { attribute R {} rule reg rewrites body of m { "
           "match @R on method m replace `return 0;` } }");
    // M32: `$body` spliced more than once (duplicated side effects).
    ERRORS("namespace W { attribute R {} rule reg rewrites body of m { "
           "match @R on method m replace `var a = $body; var b = $body; return a+b;` } }");
    // M30: `replace` in a non-`rewrites` rule (a rule is additive XOR a rewriter).
    ERRORS("namespace W { attribute R {} rule reg { "
           "match @R on method m replace `return $body;` } }");
    // M30: `inject` in a `rewrites` rule.
    ERRORS("namespace W { attribute R {} rule reg rewrites body of m { "
           "match @R on method m inject `log = 1;` at top of body } }");
    // M31: `$body` in expression position but the body is a statement block.
    ERRORS("namespace W { attribute R {} rule reg rewrites body of m { "
           "match @R on method m replace `var x = $body; return x;` } } "
           "uses W; class K { @R int f(int n) { int a = n; return a + 1; } } K k = K();");
    // M33: two `replace`s on one method body do not compose.
    ERRORS("namespace W { attribute A {} attribute B {} "
           "rule ra rewrites body of m { match @A on method m replace `return $body + 1;` } "
           "rule rb rewrites body of m { match @B on method m replace `return $body + 2;` } } "
           "uses W; class K { @A @B int f() => 1; } K k = K();");
    // M33 (§3 class 3): two `member of` injections of the same name + type into
    // one class collide — folded into the unified M33 family (names both rules).
    ERRORS("namespace W { attribute A {} attribute B {} "
           "rule ra { match @A on class C inject `int tag() => 1;` at member of C } "
           "rule rb { match @B on class C inject `int tag() => 2;` at member of C } } "
           "uses W; @A @B class K { int base = 0; } K k = K();");
    // §4.3 distinct escape: one degree of `distinct` keeps the same-name+type
    // pair as separate slots, so no conflict.
    CLEAN("namespace W { attribute A {} attribute B {} "
          "rule ra { match @A on class C inject `distinct int tag() => 1;` at member of C } "
          "rule rb { match @B on class C inject `distinct int tag() => 2;` at member of C } } "
          "uses W; @A @B class K { int base = 0; } K k = K();");
    // Different return type, same name: coexist (resolution by type), no conflict.
    CLEAN("namespace W { attribute A {} attribute B {} "
          "rule ra { match @A on class C inject `int tag() => 1;` at member of C } "
          "rule rb { match @B on class C inject `string tag() => \"x\";` at member of C } } "
          "uses W; @A @B class K { int base = 0; } K k = K();");

    // --- Layer D (Phase 4 §4): reentrant rules -----------------------------
    // The gated fixpoint: a non-reentrant `seed` injects a @Grown method, and a
    // `reentrant` rule re-triggers on that generated attribute. Converges (the
    // reentrant rule's output carries no @Grown), so it is clean. Output proof
    // lives in tests/corpus/meta/rule_reentrant.ext.
    CLEAN("namespace G { attribute Seed {} attribute Grown {} "
          "rule seed { match @Seed on class C inject `@Grown int g() => 1;` at member of C } "
          "rule grow reentrant { match @Grown on method m in class C "
          "inject `int d() => 2;` at member of C } } "
          "uses G; @Seed class W { int base = 0; } W w = W();");
    // M34: a reentrant rule that regenerates its own trigger never converges.
    ERRORS("namespace L { attribute Ping {} "
           "rule spin reentrant { match @Ping on function f "
           "inject `@Ping void spawn() {}` at namespace L } } "
           "uses L; @Ping void start() {}");
    // The safe majority: without any reentrant rule the fixpoint never runs, so
    // an ordinary self-shaped rule still sees the tree exactly once (clean).
    CLEAN("namespace G { attribute Seed {} attribute Grown {} "
          "rule seed { match @Seed on class C inject `@Grown int g() => 1;` at member of C } "
          "rule grow { match @Grown on method m in class C "
          "inject `int d() => 2;` at member of C } } "
          "uses G; @Seed class W { int base = 0; } W w = W();");

    // Hygiene: a template-declared local must not be captured by a use-site
    // field of the same name (renamed to a fresh symbol) — stays clean.
    CLEAN("namespace W { interface I {} attribute R {} "
          "rule reg { match @R on method m in class C : I "
          "inject `int tmp = 1; this.n = this.n + tmp;` at bottom of C.constructor } } "
          "uses W; class K : I { int n = 0; int tmp = 5; new K(){ n = tmp; } "
          "@R int f() => 1; } K k = K();");

    // --- `where` (§4, Phase 3): M19/M20 ---------------------------------------
    CLEAN("namespace W { interface I {} rule reg { match on method m in class C : I "
          "where m.returnType != \"void\" inject `int y = 1;` at member of C } } "
          "uses W; class K : I { int f() => 1; }");   // baseline: clean where-gated rule

    // M19: where must yield bool (an int doesn't).
    ERRORS("namespace W { interface I {} rule reg { match on method m in class C : I "
           "where 1 + 1 inject `int y = 1;` at member of C } } "
           "uses W; class K : I { int f() => 1; }");

    // M20: where evaluation failing (hermeticity violation) is reported, not
    // silently swallowed or crashed on.
    ERRORS("namespace W { interface I {} rule reg { match on method m in class C : I "
           "where std::sysReadLine(0) == \"\" inject `int y = 1;` at member of C } } "
           "uses W; class K : I { int f() => 1; }");

    // `where` correctly gates: a false predicate means no firing at all, so a
    // template that would otherwise error (unbound subject binding aside)
    // never even runs — confirms short-circuit, not just "doesn't crash".
    CLEAN("namespace W { interface I {} rule reg { match on method m in class C : I "
          "where m.name == \"nope\" inject `int y = 1;` at member of C } } "
          "uses W; class K : I { int f() => 1; }");

    // --- body/marker anchors (§8, Phase 3): M25/M26/M27 -----------------------

    // M25: `bottom of body` after a value return (arrow body) is unreachable.
    ERRORS("namespace W { attribute T {} rule reg { match @T on method m "
           "inject `int y = 1;` at bottom of body } } "
           "uses W; @T int f() => 1;");

    // `top of body` has no such restriction — the SAME arrow-bodied method is
    // fine as a top-of-body subject.
    CLEAN("namespace W { attribute T {} rule reg { match @T on method m "
          "inject `int y = 1;` at top of body } } "
          "uses W; @T int f() => 1;");

    // M26: the named marker isn't present in the subject's body.
    ERRORS("namespace W { attribute T {} rule reg { match @T on method m "
           "inject `int y = 1;` at marker \"nope\" } } "
           "uses W; @T void f() { int x = 1; }");

    // Marker present -> clean (and the marker itself is untouched/stays).
    CLEAN("namespace W { attribute T {} rule reg { match @T on method m "
          "inject `int y = 1;` at marker \"here\" } } "
          "uses W; @T void f() { @anchor(\"here\"); }");

    // M27 (partial): a body anchor on a non-callable (field) subject.
    ERRORS("namespace W { attribute T {} rule reg { match @T on field f "
           "inject `int y = 1;` at top of body } } "
           "uses W; class K { @T int x = 1; }");

    // M27 (partial): $_params on a non-callable (field) subject, via `member
    // of` (the subject here is the field; C is a separate encloser bind).
    ERRORS("namespace W { attribute T {} rule reg { "
           "match @T on field f in class C "
           "inject `int g($_params) => 1;` at member of C } } "
           "uses W; class K { @T int x = 1; }");

    // $_params/$_args on a callable subject: clean, and forwards correctly
    // (checked functionally by tests/corpus/meta/rule_forward_args.ext).
    CLEAN("namespace W { interface IC {} attribute Route { string p; } "
          "rule reg { match @Route(r) on method m in class C : IC "
          "inject `router.record($r.p, ($_params) => this.$m($_args))` "
          "at bottom of C.constructor } } "
          "uses W; class Router { string log = \"\"; "
          "Router record(string p, (int) => int h) { return this; } } "
          "class Ctl : IC { Router router; new Ctl(Router r) { router = r; } "
          "@Route(\"/x\") int f(int n) => n; }");

    // --- $for list splices (§5, Phase 3): M21 -----------------------------
    CLEAN("namespace W { attribute Col {} rule reg { match on class C "
          "inject `Array<string> names() => "
          "[ $for f in C.fields.where((x) => x.hasAttr(\"Col\")) : $f.name ];` "
          "at member of C } } "
          "uses W; class K { @Col int a; int b; }");

    // M21: the iterator must yield an array.
    ERRORS("namespace W { rule reg { match on class C "
           "inject `Array<string> names() => [ $for f in 42 : $f ];` "
           "at member of C } } "
           "uses W; class K { int a; }");

    // --- expression macros (§7, Phase 3): M22/M23/M24 ---------------------
    CLEAN("namespace T { macro safeStrip(e) => `($e ?? \"\").trim()`; } "
          "uses T; string? s = \"x\"; console.writeln(safeStrip!(s));");

    // M22: a macro parameter spliced more than once, static (nobody calls it).
    ERRORS("namespace T { macro dup(e) => `$e + $e`; }");

    // M23: unknown macro.
    ERRORS("console.writeln(nope!(1));");

    // M23: ambiguous macro across two imports.
    ERRORS("namespace A { macro dbl(x) => `$x * 2`; } "
           "namespace B { macro dbl(x) => `$x + 10`; } "
           "uses A; uses B; console.writeln(dbl!(5));");

    // Qualified NS::name! resolves the ambiguity above.
    CLEAN("namespace A { macro dbl(x) => `$x * 2`; } "
          "namespace B { macro dbl(x) => `$x + 10`; } "
          "uses A; uses B; console.writeln(A::dbl!(5));");

    // A macro call nested in call-site ARGUMENTS expands bottom-up in one
    // pass (dbl!(inc!(3)) -> (3+1)*2) — not M24 (that's a different shape).
    CLEAN("namespace T { macro inc(x) => `$x + 1`; macro dbl(x) => `$x * 2`; } "
          "uses T; console.writeln(dbl!(inc!(3)));");

    // M24: a macro's own TEMPLATE calling another macro (re-entry) is rejected.
    ERRORS("namespace T { macro inner(x) => `$x + 1`; "
           "macro outer(y) => `inner!($y) * 2`; } "
           "uses T; console.writeln(outer!(3));");

    // --- F4 procedural macros: comptime string -> Ast ---------------------
    CLEAN("macro p(string s) comptime { return meta::parseExpr(s); } "
          "int x = p!(`1 + 2`); console.writeln(x);");
    CLEAN("macro p(string s) comptime { if (s.length() == 1) "
          "return meta::parseExpr(s); return p(s.subStr(1, s.length() - 1)); } "
          "int x = p!(\"xx7\"); console.writeln(x);");
    ERRORS("macro p(string s) comptime { return 1; } int x = p!(\"x\");");
    ERRORS("macro p(string s) comptime { throw RuntimeException(\"boom\"); } "
           "int x = p!(\"x\");");
    ERRORS("macro p(string s) comptime { return meta::parseExpr(\"1 +\"); } "
           "int x = p!(\"x\");");
    ERRORS("macro p(string s) comptime { return meta::parseStmts(\"int x = 1;\"); } "
           "int x = p!(\"x\");");
    ERRORS("macro inner(x) => `$x`; "
           "macro p(string s) comptime { return meta::parseExpr(\"inner!(1)\"); } "
           "int x = p!(\"x\");");
    // The quasiliteral grammar change is deliberately scoped to macro args.
    ERRORS("string s = `raw`; console.writeln(s);");
    {
        MetaResult bad = runMeta(
            "macro broken(string s) comptime { return meta::parseExpr(\"1 + )\"); } "
            "int x = broken!(\"x\");");
        expect(bad.firstError.find("meta::parseExpr failed") != std::string::npos &&
                   bad.firstError.find("1 + )\n    ^") != std::string::npos,
               "procedural parse error includes fragment text and exact caret");
    }
    {
        MetaResult exhausted = runMeta(
            "macro runaway(string s) comptime { while (true) { } "
            "return meta::parseExpr(\"1\"); } int x = runaway!(\"x\");", 50);
        expect(exhausted.firstError.find("procedural macro 'runaway'") != std::string::npos &&
                   exhausted.firstError.find("comptime step budget exceeded") != std::string::npos,
               "procedural budget diagnostic names the macro");
    }

    // --- §7x§9 interaction: deferred comptime folding + M29 ----------------
    // A macro call under a `comptime` fold expands (post-imports) before the
    // oracle folds the result — the deferred C-early/C-late split. Functional
    // proof (the value is genuinely baked in) is
    // tests/corpus/meta/macro_safestrip.ext; here just confirms it compiles.
    CLEAN("namespace T { macro safeStrip(e) => `($e ?? \"\").trim()`; } "
          "uses T; comptime string s = safeStrip!(\"  x  \"); console.writeln(s);");

    // M29: a macro call in a comptime-if CONDITION is a hard error — the
    // condition feeds the imports map macro resolution itself needs, so
    // deferring it (like the comptime var above) would be the one true
    // fixpoint this stage forbids. Item-level position.
    ERRORS("namespace M { macro yes(x) => `$x == 1`; } uses M; "
           "comptime if (yes!(1)) { console.writeln(\"a\"); } "
           "else { console.writeln(\"b\"); }");

    // M29 applies identically to a NESTED comptime-if (inside a function body).
    ERRORS("namespace M { macro yes(x) => `$x == 1`; } uses M; "
           "void f() { comptime if (yes!(1)) { console.writeln(\"a\"); } "
           "else { console.writeln(\"b\"); } } f();");

    // A macro call inside a rule/anchor TEMPLATE (5c: clone-time expansion,
    // def-site scoped) — functional proof (the use site never `uses` the
    // macro's namespace, only the rule's) is
    // tests/corpus/meta/macro_in_rule_template.ext; here just confirms clean.
    CLEAN("namespace T { macro tag(e) => `\"<\" + $e + \">\"`; } "
          "namespace W { uses T; attribute Mark {} "
          "rule reg { match @Mark on method m in class C "
          "inject `log = log + tag!(\"x\");` at bottom of C.constructor } } "
          "uses W; class K { string log = \"\"; @Mark int f() => 1; } K k = K();");

    // --- definition-site qualification (§10, Phase 3) ----------------------
    // Web's own `fmt` must win over the use-site's same-named `fmt` — output
    // proof lives in tests/corpus/meta/rule_defsite_qual.ext; clean here too.
    CLEAN("namespace Web { string fmt(int n) => \"web:\" + n.toString(); "
          "attribute Route {} rule reg { match @Route on method m in class C "
          "inject `log = log + fmt(1);` at bottom of C.constructor } } "
          "uses Web; string fmt(int n) => \"userland:\" + n.toString(); "
          "class Ctl { string log = \"\"; @Route int handle() => 1; } "
          "Ctl c = Ctl();");

    // --- Layer D (Phase 4 §8): pass-1 comptime-root pre-checking -----------
    // The pre-check is a safe message upgrade: it runs the type checker over a
    // FAILED comptime root at a scope-complete position, never introducing a
    // false error. These assert the safety invariant — valid comptime stays
    // clean, and a nested comptime referencing locals (not scope-complete, so
    // the pre-check is skipped) is unaffected.
    CLEAN("comptime int n = 2 + 3; console.writeln(n);");
    CLEAN("int f() { int local = 3; comptime int c = local + 1; return c; } "
          "console.writeln(f());");
    CLEAN("namespace M { int base() => 10; } uses M; "
          "comptime int n = M::base() + 1; console.writeln(n);");

    // --- struct reification (§11, Phase 3, stretch): M28 -------------------
    CLEAN("struct Point { int x; int y; "
          "new Point(int x, int y) { this.x = x; this.y = y; } } "
          "Point mk() => Point(1, 2); "
          "comptime Point p = mk(); console.writeln(p.x);");

    // M28: a reference class (not `struct`) is never reifiable — identity has
    // no compile-time meaning.
    ERRORS("class Widget { int id; new Widget(int id) { this.id = id; } } "
           "Widget mk() => Widget(1); comptime Widget w = mk();");

    // M28: a struct without a constructor matching its fields 1:1.
    ERRORS("struct Couple { int a; int b; "
           "new Couple(int onlyA) { a = onlyA; b = 0; } } "
           "Couple mk() => Couple(1); comptime Couple c = mk();");

    // --- bug.md #17: cloneStmt must preserve isConst through rule injection -
    // A `const` declaration injected via a template and reassigned within
    // that same template must still be rejected by the checker post-clone,
    // exactly as a hand-written const/reassign pair would be (see the
    // plain-source case a few lines above this one in test_checker.cpp).
    // Before cloneStmt copied isConst, the cloned decl silently became a
    // mutable var and this went CLEAN instead of ERRORS.
    ERRORS("namespace RC { attribute InjectConst {} "
           "rule addConst { match @InjectConst on method m "
           "inject `const int y = 1; y = 2;` at top of body } } "
           "uses RC; class K { @InjectConst void f() { } } K k = K(); k.f();");

    // --- LA-20: comptime import() -------------------------------------------
    // Single-file (rootDir) mode against a real scratch directory — plan-mode
    // (I03/I05, per-module asset tables) is exercised by the project corpus
    // (tests/corpus/project/asset_ok, asset_module) instead, since it needs a
    // real trident-resolved plan.
    {
        char tmpl[] = "/tmp/lev_meta_import_XXXXXX";
        char* tmpDir = ::mkdtemp(tmpl);
        std::string root = tmpDir ? tmpDir : ".";
        {
            std::ofstream out(root + "/fixture.txt", std::ios::binary);
            out << "hello import";
        }

        // Happy path: folds to the file's content, byte for byte.
        expect(!runMeta("comptime string s = import(\"fixture.txt\"); "
                        "console.writeln(s);", 0, root.c_str()).hadError,
               "import(): a declared, readable file folds cleanly");

        // I01: the argument must be a string.
        expect(runMeta("comptime string s = import(42);", 0, root.c_str()).hadError,
               "import(): I01 non-string argument");

        // I02: escape-shaped paths are refused lexically (leading '/', '..', '.').
        expect(runMeta("comptime string s = import(\"../etc/passwd\");", 0,
                       root.c_str()).hadError,
               "import(): I02 '..' segment");
        expect(runMeta("comptime string s = import(\"/etc/passwd\");", 0,
                       root.c_str()).hadError,
               "import(): I02 leading '/'");
        expect(runMeta("comptime string s = import(\"./fixture.txt\");", 0,
                       root.c_str()).hadError,
               "import(): I02 '.' segment");
        expect(runMeta("comptime string s = import(\"\");", 0, root.c_str()).hadError,
               "import(): I02 empty path");

        // I04: single-file mode, a well-formed but missing/unreadable path.
        expect(runMeta("comptime string s = import(\"nope.txt\");", 0,
                       root.c_str()).hadError,
               "import(): I04 file not found under rootDir");

        // Runtime misuse: the prelude body's own throw, uninterrupted by the
        // comptime intercept (comptime_ is false at runtime).
        expect(runMeta("string s = import(\"fixture.txt\"); console.writeln(s);", 0,
                       root.c_str()).hadError == false,
               "import() at runtime is not a compile error (it throws at run time)");

        // Symbol-identity guard: a user's OWN `import` (a different decl, in
        // the global namespace, not std's) is never hijacked by the comptime
        // intercept — it just runs normally, like any other comptime call.
        expect(!runMeta("string import(string path) => \"shadowed:\" + path; "
                       "comptime string s = import(\"x\"); console.writeln(s);", 0,
                       root.c_str()).hadError,
               "import(): a user-declared import() shadows std's, even at comptime");

        std::remove((root + "/fixture.txt").c_str());
        ::rmdir(root.c_str());
    }

    // --- Item Q (techdesign-target-predicate.md): the target predicate -------
    {
        // The uses-selection program: only the taken branch's namespace is
        // imported, so calling the other branch's marker is the observable.
        // A taken TrueNS branch makes tmark() resolvable; a taken FalseNS
        // branch does not — so `tmark()` is clean iff the predicate held.
        auto sel = [](const std::string& pred) {
            return "namespace TrueNS { public int tmark() => 1; } "
                   "namespace FalseNS { public int fmark() => 2; } "
                   "comptime if (" + pred + ") { uses TrueNS; } "
                   "else { uses FalseNS; } "
                   "console.writeln(tmark());";
        };
        // Target-not-host: a windows cross triple folds the windows branch on
        // this (non-windows) build host — the predicate reads the TARGET.
        expect(!runMeta(sel("target::os == \"windows\""), 0, nullptr,
                        "x86_64-pc-windows-gnu").hadError,
               "target:: cross triple folds the windows uses branch");
        // Host build (no triple): the windows predicate must NOT hold.
        expect(runMeta(sel("target::os == \"windows\"")).hadError,
               "target:: host build must not fold the windows branch");
        // Arch normalization: arm64 spelling normalizes to aarch64.
        expect(!runMeta(sel("target::arch == \"aarch64\""), 0, nullptr,
                        "arm64-apple-darwin").hadError,
               "target::arch normalizes arm64 -> aarch64");
        // Wasm triples report os "wasm".
        expect(!runMeta(sel("target::os == \"wasm\""), 0, nullptr,
                        "wasm32-wasi").hadError,
               "target::os is wasm for wasm32 triples");
        // target::triple is the exact cross string.
        expect(!runMeta(sel("target::triple == \"aarch64-linux-gnu\""), 0, nullptr,
                        "aarch64-linux-gnu").hadError,
               "target::triple carries the exact --target string");
        // Unknown member is loud and names the three constants.
        MetaResult bogus = runMeta("comptime if (target::bogus == \"x\") { }");
        expect(bogus.hadError, "target::bogus is an error");
        expect(bogus.firstError.find("target::os") != std::string::npos,
               "the target::bogus error names the known constants");
        // Runtime position is untouched: no comptime context, no intercept —
        // and a plain program without meta surface never sees `target` at all.
        expect(runMeta("console.writeln(target::os);").hadError,
               "runtime target::os stays an ordinary unresolved name");
    }

    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
