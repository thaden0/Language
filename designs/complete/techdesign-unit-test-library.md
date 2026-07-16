# Tech design — Harpoon: the standard xUnit-shaped unit test library

**Status:** **COMPLETE — M0–M5 all landed 2026-07-15 (`harpoon/`, `harpoon/README.md`),
including M3 rule-based auto-discovery.** Finishing discovery required compiler fixes
(owner directive lifting the "library-only" constraint) — see the implementation log
(§13) for exactly what changed and why. Originally: ready for implementation (M0 probe
gate first — §9). **Date:** 2026-07-13.
**Source:** `designs/requests/request-unit-test-library.md` (accepted with this design; moved to
`designs/requests/accepted/`). The request's xUnit ruling is adopted verbatim: a test is a
method on an ordinary class, constructor = setup, `IDisposable` = teardown, `@Test` is a pure
marker, discovery is metaprogramming's job.
**Priority:** P2 — additive ecosystem standardization; blocks nothing in flight, wanted by
everything (Atlantis, Sonar, Trident, the future self-hosted stdlib all hand-roll the
print-vs-`.expected` corpus pattern today).
**Track:** single track, one implementer. **Library only — zero compiler work.** Every
mechanism this design uses is landed and corpus-proven (§3); if any M0 probe falsifies that,
the implementer STOPs and escalates (§9) rather than reaching into `src/`.
**Owns (files):** `harpoon/` (new package: `trident.toml`, `src/*.lev`, `tests/`,
`examples/`) — nothing else. `.lev` extension everywhere, never `.ext`.
**Does NOT touch:** `src/` (the compiler — this is a pure `.lev` package), the prelude
(deliberately: prelude code is unchecked and narrowing-hostile — the request's own warning;
Harpoon is ordinary checked project source pulled in via Trident), `tests/corpus/` (the
compiler's corpus stays the compiler's), Trident (no `[targets]`/`[workspace]` work — the
`deferal-trident-post-v1.md` §9(f) STOP condition is respected by construction, §8), and
`designs/sonar/` (the TUI framework keeps its name until the owner rules on the rename — §10).
No ELF lane anywhere; nothing here is ever gated on an ELF finding.

---

## 1. The one rule

xUnit's whole design collapses into machinery this language already has:

> **A test is an ordinary zero-arg `void` method marked `@Test` on an ordinary class.**
> Setup is the constructor (a fresh instance per test). Teardown is `IDisposable.close()`
> (via `using`, so it runs even when the test throws). Async is `await` inside the body
> (no coloring — the language's own rule). Threads are `spawn` + `await` inside the body.
> Failure is an exception (`AssertionException`), caught by contract (`IAssertionException`)
> — catch-selection-by-type applied to test scoring.

Nothing in that sentence is new mechanism. The only interesting part is **discovery** — turning
`@Test` into a populated registry with zero manual registration — and that is a two-rule
application of landed Phase 2/3 metaprogramming (§5). Everything else is a small assertion
vocabulary (§4) and a ~150-line runner (§6).

**Why `void` and not `async` variants:** a `void` test that `await`s internally parks the
runner's task at the call and resumes when done — the runner scores it *after* every internal
await completes, synchronously from the runner's point of view. So async and threaded tests
fall out of "a test is a method" with **zero** runner special-casing, exactly as the request
hoped. A *`Promise`-returning* test method has a value nobody consumes and would need runner
await plumbing; v1 rejects it loudly (§5.4) and defers the shape (§11).

---

## 2. What ships, at a glance

| Piece | Kind | Status |
|---|---|---|
| `assertEqual`/`assertTrue`/`assertFalse`/`assertNotEqual`/`assertSame`/`assertNone`/`assertSome`/`assertThrows`/`fail` | library (`namespace harpoon`) | **v1** (§4) |
| `AssertionException : Exception, IAssertionException` | library | **v1** (§4.1) |
| `@Test`, `@Skip` marker attributes | library | **v1** (§5) |
| Two discovery rules (per-class, `where`-split on `IDisposable`) | library (Phase 2/3 rules) | **v1** (§5) |
| Registry + `harpoon::runAll()` + `harpoon::main()` reporter/exit-code | library | **v1** (§6) |
| Sibling-test-project pattern (own `trident.toml`, `[[dep]] path` back) | worked example | **v1** (§8) |
| `@Skip("reason")` with a reason string | blocked on LA-4 attr-value reflection | **deferred** (§11) |
| `@Timeout(ms)`, `Promise<T>`-returning tests, parallel execution | needs promise/TaskGroup plumbing | **deferred** (§11) |
| Theories / parameterized `@Cases` | blocked on LA-4 (+ nicer with LA-16) | **deferred** (§11) |
| `trident test` subcommand | Trident's domain (PM/compiler separation) | **deferred; request filed if wanted** (§11) |
| Rename to "Sonar" | owner decision (TUI rename first) | **open, mechanical later** (§10) |

---

## 3. Grounding (what is true on this tree today)

The request was written before Phase 3 landed and warns "namespace anchors are not yet
landed." **That warning is stale.** Everything below was re-verified against this tree
(2026-07-13):

1. **`namespace N` anchors are landed and corpus-proven** — `tests/corpus/meta/rule_ns_inject.ext`
   injects one hole-named (`$C`) top-level declaration per matched class into a target
   namespace via reopen-and-merge; two matched classes produce two distinct declarations.
2. **Attribute-less structural match + `where` predicates** — `rule_where.ext`:
   `match on method m in class C : IService where m.returnType != "void"`. So a rule can fire
   on *classes that have `@Test` methods* without requiring a class-level attribute.
3. **`meta::Class.methods` exists** with `name`, `returnType`, `arity()`, `hasAttr(string)`
   (`techdesign-metaprog-phase3.md` §3). `attrs` is **names-only** — attribute *values* are
   not readable from `$for` iteration (that is LA-4, open). This is why `@Skip` is a bare
   marker in v1.
4. **`$for` array-literal splices over filtered members** — `rule_orm.ext`:
   `[ $for f in C.fields.where((x) => x.hasAttr("Column")) : $f.name ]`.
5. **Namespace-scope globals may have runtime-computed initializers that run at startup** —
   `const Array<string> args = std::sysArgs();` is the landed reference example
   (reference.md §4.3b, const.md). This is the registration vehicle (§5.2).
6. **Rules fire only in files that import the rule's namespace** (reference.md §6.9) — so
   `uses harpoon;` is simultaneously "give me the assertion vocabulary" and "arm test
   discovery for this file." One import, both facts; location-agnostic discovery (request
   item 6) follows because the trigger is the import, not the path.
7. **Block-body lambdas, `using` on every exit edge incl. throw-unwind, `env::setExitCode`,
   `std::env::args`, `spawn`/`await`/task substrate** — all landed (reference.md §3.2, §5.2,
   §6.6.53, §6.6.66–67).

Known bugs this design routes around (all filed in `bug.md`; none fixed here):

- **#53 (HARD memory rule):** lambdas never call sibling methods bare — every injected lambda
  uses an explicit receiver (`t.$m()`), never an implicit one.
- **#38:** positional auto-construction of a struct silently drops closure-typed fields —
  `TestCase` is a **class with an explicit constructor**, never a bare struct.
- **#47 (HARD memory rule):** no `Map<K, struct>` in class fields — the registry is a flat
  `Array<TestCase>`; lookups are linear scans (test counts are small).
- **#2 / #52:** a field-held closure must be copied to a local before calling
  (`var f = c.run; f();`), and an indexing result is never called directly (`cases[i].run()`
  is forbidden; bind first).
- **#51:** no nullable function types (`(()=>void)?` fails to parse) — `TestCase.run` is
  always present; skipped tests carry a no-op closure plus a flag instead.
- **#34:** overload-resolution scoring can wrongly admit a lambda literal against a `string`
  parameter — where a name has both lambda-taking and string-taking overloads, the
  lambda-taking ones are declared first (relevant to `assertThrows` message variants).

---

## 4. The assertion vocabulary (`harpoon/src/assert.lev`)

### 4.1 The failure carrier

```lev
interface IAssertionException : IException { }
class AssertionException : Exception, IAssertionException {
    new AssertionException(string msg) { message = msg; }
}
```

The runner's scoring is catch-by-contract, the language's own idiom:
`catch (IAssertionException e)` → **FAIL** (an assertion said so);
`catch (IException e)` → **ERROR** (the test blew up some other way). Any user class can
implement `IAssertionException` to have its throw scored as a failure — multiple inheritance
earning its keep, no registration with Harpoon needed.

### 4.2 The functions

All free functions in `namespace harpoon`; `uses harpoon;` gives the bare names the request
shows. Every failure message is labeled with the assertion name, expected, and actual, via
string interpolation.

| function | overloads | notes |
|---|---|---|
| `assertEqual(expected, actual)` | `(int,int)`, `(string,string)`, `(bool,bool)`, `(char,char)`, `(float,float,float eps = 0.000001)` | scalar ladder; float always via eps (the default makes 2-arg float calls work) |
| `assertEqual(expected, actual)` | `(Array<int>,Array<int>)`, `(Array<string>,Array<string>)`, `(Array<float>,Array<float>)`, `(Array<bool>,Array<bool>)` | length then element-wise; failure names the first differing index |
| `assertEqual<T>(T, T)` | generic, **declared last** (concrete overloads are most-specific and first-declared) | duck-typed `==` at the instantiation site — covers user structs with field-wise/defined `(==)` and enums (carrier compare). Probe P7 pins it; if erased-generic `==` misbehaves on any engine, the generic overload is cut and the doc says "use `assertTrue(a == b, msg)`" (loud, no silent wrongness) |
| `assertNotEqual(...)` | same scalar ladder | negation of the above |
| `assertSame(object a, object b)` / `assertNotSame` | class references | reference identity (`==` on class refs); structs/primitives deliberately excluded — identity is not a value-type concept |
| `assertTrue(bool cond, string msg = "")` / `assertFalse` | | the escape hatch everything else reduces to |
| `assertNone<T>(T? v, string msg = "")` / `assertSome<T>(T? v, ...)` | | `v == None` test — Harpoon is checked user code, so narrowing works (this is exactly why it is NOT prelude code) |
| `IException assertThrows((() => void) body)` | + `(body, string mustContain)` variant | runs `body` in `try/catch (IException e) { return e; }`; falling through throws `AssertionException("assertThrows: nothing was thrown")`. Caller type-checks the result with `is` (`assertTrue(e is FileException, ...)`) — generic `assertThrows<T>` is impossible with erased generics and is not pretended at. Lambda-first declaration order per bug #34 |
| `fail(string msg)` | | unconditional |

`assertEqual` element-compare for `Map` is deferred to the generic overload
(`assertEqual<T>` with `Map`'s duck `==` is unproven) — v1 documents the idiom: compare
`m.keys()` and `m.values()` (typed arrays) or `entries()` projections. Honest over magical.

### 4.3 Maps to request item 3

Primitives ✓ (scalar ladder), structs ✓ (generic overload / P7), arrays ✓ (typed ladder),
maps ✓ (documented idiom, §4.2), identity ✓ (`assertSame`), throws ✓, labeled
expected/actual output ✓.

---

## 5. Discovery (`harpoon/src/discover.lev`) — the metaprogramming piece

### 5.1 The attributes

```lev
namespace harpoon {
    attribute Test { }     // pure marker — the only thing the author writes
    attribute Skip { }     // bare marker in v1; reason string arrives with LA-4 (§11)
}
```

### 5.2 The mechanism — one registration global per test class

A rule fires **once per class that has at least one `@Test` method** (attribute-less
structural match + `where` over `meta::Class.methods` — grounded facts §3.2/§3.3), and injects
**one hole-named namespace-scope global** into `namespace HarpoonReg` (grounded §3.1). The
global's initializer runs at startup (grounded §3.5) and seeds the registry:

```lev
rule discoverTests {
    match on class C
    where C.methods.any((x) => x.hasAttr("Test")) && !C.hasBase("IDisposable")
    inject `bool $C = harpoon::__seed("$C",
        [ $for m in C.methods.where((x) => x.hasAttr("Test") && x.returnType == "void" && x.arity() == 0 && !x.hasAttr("Skip"))
            : harpoon::__entry("$m", () => { $C t = $C(); t.$m(); }) ],
        [ $for m in C.methods.where((x) => x.hasAttr("Test") && x.hasAttr("Skip")) : "$m" ],
        [ $for m in C.methods.where((x) => x.hasAttr("Test") && (x.returnType != "void" || x.arity() != 0)) : "$m" ]);`
    at namespace HarpoonReg
}
```

The three `$for` arrays per class: **runnable entries** (name + a closure that constructs a
fresh instance and calls the method — explicit receiver `t.$m()`, per bug #53), **skipped
names** (strings only — the ORM-corpus-proven shape), and **misdeclared names** (a `@Test` on
a non-`void` or parameterized method is *reported as an ERROR by the runner*, never silently
dropped — the silent-distant-footgun ban applied to test discovery; §5.4).

A second rule, identical but `where ... && C.hasBase("IDisposable")`, differs only in the
entry template — teardown via `using`, which closes on **every** exit edge including
throw-unwind, so teardown runs even when the test fails:

```lev
    : harpoon::__entry("$m", () => { using $C t = $C(); t.$m(); })
```

That is the request's constructor-as-setup / `IDisposable`-as-teardown lifecycle, verbatim,
with zero new lifecycle vocabulary.

### 5.3 Why this satisfies "zero manual registration"

- The author writes `@Test` and (in that file) `uses harpoon;` — which they need anyway for
  `assertEqual`. No list of tests or test classes exists anywhere by hand (request
  acceptance 1). The one honest boundary: a file that never imports `harpoon` is invisible to
  the rule — that is the language's own rule-scoping law (§3.6), it is greppable, and it is
  the same deal `@Route` controllers already accept.
- Location-agnostic (request item 6): inline-next-to-the-code and dedicated-`tests/`-project
  classes are discovered identically, because the trigger is the import graph, not the path.
  Both worked examples ship (§8) and M4 tests them for identical output.
- Fresh instance per test: the closure constructs `$C` itself — construction happens at
  *run* time per entry, not at seed time, so instance N of a 5-test class never sees
  instance N−1's state.

### 5.4 Loudness rules

- `@Test` on a non-`void`/non-nullary method → runner prints
  `ERROR <C>::<m> — @Test requires 'void m()' (got a return value or parameters)` and the run
  exits non-zero. Never silently skipped.
- Two same-named test **classes in different namespaces** collide on the injected `$C` global
  name in `HarpoonReg` → a **compile-time** duplicate-declaration error (pass-2's ordinary
  machinery). v1 documents the limit ("test class simple names must be program-unique");
  loud-at-compile beats any silent merge. Revisit only if it bites in practice.
- A `@Test` class with no nullary constructor fails to compile at the injected `$C()` call —
  the error names generated code, so the docs give the reading ("a test class must be
  bare-constructable; the constructor IS the setup").

### 5.5 The registry (`harpoon/src/registry.lev`)

```lev
namespace harpoon {
    class TestCase {
        string className; string methodName;
        (() => void) run;
        bool skipped; bool misdeclared;
        new TestCase(string c, string m, (() => void) r, bool s, bool bad) { ... }  // explicit ctor — bug #38
    }
    Array<TestCase> __cases;          // bare-declared: auto-constructs to [], no init-order dependency
    // __seed / __entry append via BARE writes (__cases = __cases.add(...)) — the
    // ns-global-write lowering gotcha; never `harpoon::__cases = ...`.
}
```

`__entry(name, fn)` returns a partial record; `__seed(className, entries, skips, bad)` stamps
the class name on and appends — called once per class from the injected globals' initializers,
in whatever order globals initialize (order-independent by construction; the report sorts).

---

## 6. Runner + reporter (`harpoon/src/runner.lev`)

```lev
namespace harpoon {
    int runAll() { ... }   // runs everything, prints report, returns failed+errored+misdeclared count
    void main()  { env::setExitCode(runAll() > 0 ? 1 : 0); }
}
```

- Sequential, single task, insertion order grouped by class (sorted by class name for
  determinism against global-init order).
- Per test: copy the closure to a local (`var f = c.run; f();` — bugs #2/#52), wrap in
  `try/catch (IAssertionException a) → FAIL / catch (IException e) → ERROR`. A test that
  `await`s or `spawn`s inside parks/resumes the runner task transparently — scoring happens
  after the body truly completes (request acceptance 4 & 5, no special casing).
- CLI filter: `std::env::args` scanned for `--filter <substring>` matched against
  `Class::method` (skipped/misdeclared entries still reported when matched).
- `env::setExitCode` (not `env::exit`) so the implicit loop drains — no async work is
  guillotined mid-teardown.
- Report shape (stable, corpus-pinnable):

```
harpoon: 12 tests, 3 classes
  MathTests::addsNumbers ......... ok
  MathTests::resolvesAsync ....... ok
  UserTests::rejectsEmptyName .... FAIL
      assertEqual: expected "" but was "bob"
  UserTests::flaky ............... skip
12 run: 10 ok, 1 FAIL, 0 ERROR, 1 skip
```

The interpreters buffer stdout (prelude-gotchas memory) — the report prints nothing
mid-test-dependent on timing; it is all ordinary sequential writes, so byte-identical
cross-engine output is achievable (M4 pins it).

---

## 7. Engine coverage

Assertions + registry + runner + discovery: **oracle, IR, emit-C++, LLVM** — all ordinary
in-language code and landed metaprogramming (rules run in the front-end; all four engines see
the same post-rule tree). Tests that `await`/`spawn`: **oracle, IR, LLVM** (emit-C++ has no
async surface — its lane simply doesn't run the async/threaded example programs, the standing
deferral). Frozen ELF: not a target, no lane, nothing gated on it.

---

## 8. Project shape and the Trident story

`harpoon/` is an ordinary Trident package:

```
harpoon/
  trident.toml            # name = "harpoon", sources = ["src/*.lev"]
  src/{assert,discover,registry,runner}.lev
  tests/                  # Harpoon's own self-tests (dogfooding: Harpoon tests Harpoon)
    trident.toml          # [[dep]] path = ".."
    main.lev              # uses harpoon;  test classes;  harpoon::main();
    run.sh                # runs main via oracle/IR/LLVM, diffs the three outputs (acceptance 7)
  examples/
    inline/               # @Test classes in the same files as the code they test
    sibling/              # app/ + tests/ as two trident.toml projects, [[dep]] path = "../app"
```

Consumers add `[[dep]] path = "harpoon"` (with `dev = true` where supported by their flow) and
a two-line test entry file: `uses harpoon;` … `harpoon::main();`. The sibling-project pattern
(request's own recommendation) gives fast local iteration — rebuilding the test project does
not rebuild the app binary — with **zero new Trident machinery**, which is exactly what
`deferal-trident-post-v1.md` §9(f) demands: no `[targets]`, no workspaces, no owner-ruling
trigger. A `trident test` convenience subcommand is Trident's call, later, via a
`designs/requests/` ticket if wanted (§11) — the PM/compiler separation keeps it out of scope
here.

---

## 9. Milestones, probes, acceptance

Single implementer. Per project convention the implementer **STOPs and escalates** (rather
than improvising design changes) when a milestone's stated assumption fails; each probe below
names its fallback so the escalation carries a decision, not a shrug.

**M0 — mechanism probes (2026-07-13).** One throwaway probe project (kept as
`harpoon/tests/probes/` once green). Gate for everything else:

| # | probe | fallback if it fails |
|---|---|---|
| P1 | a `$for` array element containing a **block lambda with `$C`/`$m` holes** (`harpoon::__entry("$m", () => { $C t = $C(); t.$m(); })`) compiles, fires per method, closures run correctly | STOP + escalate; likely shape: file a `designs/requests/` ticket for lambda-bearing `$for` elements, interim = per-method `member of C` wrappers only if the owner rules for it |
| P2 | an injected `namespace HarpoonReg` global `bool $C = harpoon::__seed(...);` initializer executes at startup on oracle/IR/LLVM, and appends to the bare-declared `__cases` survive regardless of init order | STOP + escalate (this is the registration vehicle; no code-level fallback is credible without an owner ruling) |
| P3 | `using $C t = $C(); t.$m();` inside an injected block lambda compiles; `close()` runs on assertion-throw unwind | drop the `using` template; teardown becomes `t.$m(); t.close();` (no teardown on failure) + a documented limit, escalate for priority |
| P4 | `C.hasBase("IDisposable")` matches the prelude interface's canonical spelling | adjust the `where` string to the observed canonical form (probe exists to learn the spelling) |
| P5 | two rules with complementary `where` predicates both fire correctly across a mixed program | merge into one rule + always-`using`… only if P3 also showed `using` tolerates it; else escalate |
| P6 | `uses harpoon;` arms the rules for a file, selective `use harpoon::assertEqual;` also arms (namespace-grain opt-in, per the attr-scope corpus), and a file with neither stays dark | document whichever import forms actually arm discovery; no code change |
| P7 | generic `assertEqual<T>` with duck `==` behaves identically on all four engines for a user struct and an enum | cut the generic overload; keep the concrete ladder + documented `assertTrue(a == b)` idiom |

**M1 — assertions (07-13/14).** `assert.lev` + `AssertionException`; direct-call self-tests
(no discovery yet) green on all four engines.

**M2 — registry + runner (07-14).** Hand-seeded (`__seed` called explicitly) twin program
proves the runner/report/exit-code end-to-end before any rule exists — the corpus twin
discipline applied to the library itself.

**M3 — discovery (07-14/15).** The two rules + `@Test`/`@Skip`; `--expand` output inspected
and kept as a checked-in artifact; M2's hand-seeded twin must produce **byte-identical
reports** to the rule-discovered equivalent (the rule_orm twin pattern). Misdeclared-`@Test`
loudness cases.

**M4 — the request's acceptance matrix (07-15).** Async test (deliberately-delayed failing
assertion IS caught — acceptance 4); threaded test (`spawn`+join+assert — acceptance 5);
lifecycle test (IDisposable teardown observed after pass AND after fail — acceptance 2);
`inline/` vs `sibling/` examples produce identical reports (acceptance 6, 8); `run.sh`
three-engine diff green (acceptance 7).

**M5 — close-out (07-15).** `harpoon/README.md` (usage, the import-arms-discovery rule, the
gotcha list); `info.md` gets a one-paragraph §0 entry; this design moves to
`designs/complete/`; the request moves to `designs/requests/accepted/` (done with this
commit).

Acceptance = the request's 8 criteria, mapped: 1→§5.3, 2→§5.2/M4, 3→§4.3, 4/5→§6/M4,
6→§5.3/M4, 7→§8 run.sh/M4, 8→§8/M4.

---

## 10. Naming

Working name **Harpoon** (nautical register: Leviathan, Trident, Atlantis — a harpoon is the
thing you hunt a leviathan's bugs with). The request's stated intent — reuse **Sonar** once
the TUI framework is renamed (Fathom recommended there) — is an **owner decision this design
does not make**. Cost of a later rename is one namespace identifier + one directory + docs
(mechanical grep); nothing here bakes the name into compiler or manifest machinery.

---

## 11. Deferred (named, so they aren't re-litigated ad hoc)

- **`@Skip("reason")` reasons, theories/`@Cases(...)` parameterized tests** — both need
  attribute-value reflection in `$for` iteration (**LA-4**, `request-metaprog-attr-values.md`,
  already P1 for Atlantis's ORM; theories also read nicer with statement-position `$for`,
  LA-16). Harpoon becomes the second named consumer — noted on that request, not re-filed.
- **`Promise<T>`-returning test methods and `@Timeout(ms)`** — both want the
  wrap-in-a-`Promise` + `awaitTimeout`/`TaskGroup` runner plumbing; v1's void-await shape
  covers the acceptance criteria without it.
- **Parallel test execution** (`TaskGroup` per class / `spawn` per suite) — sequential v1
  keeps output deterministic; revisit when a consumer's suite is slow.
- **`trident test`** — Trident's domain; file a request ticket when the ergonomics itch.
- **Sonar-harness convergence** — once both exist, Sonar's `SonarTest` UI-snapshot helpers
  could sit on Harpoon's assertions (the request's own convergence note).
- **`assertEqual` for `Map`/`Set` directly** — after P7's verdict on duck-`==` generics.

## 12. Risks

| risk | mitigation |
|---|---|
| P1/P2 falsified (template or startup-init gap) | M0 gates all build work; STOP-and-escalate with probe evidence; requests filed rather than compiler hacks |
| Global-initializer ordering differs across engines | registry is order-independent (bare-declared `[]`, append-only, report sorts); M4's three-engine diff would catch any residue |
| Duck-typed generic `assertEqual<T>` diverges on an engine | P7 pins before it ships; cut-don't-fudge fallback |
| Same-named test classes across namespaces | loud compile-time collision, documented v1 limit (§5.4) |
| Known closure/field bugs (#2, #38, #47, #51, #52, #53) | each has an explicit routing rule in §3; they are design inputs, not discoveries to be made mid-build |
| Report byte-divergence from interpreter stdout buffering | sequential writes only; no timing-dependent output |

---

## 13. Implementation log (2026-07-15) — COMPLETE

Landed in full: assertions, registry, runner, **and** rule-based `@Test` discovery, on
oracle/IR/LLVM, consumable as an ordinary `[[dep]]`. M0's probing surfaced a family of
compiler defects that blocked the design's registration vehicle; the owner directed that
they be fixed (lifting this doc's original "library-only, zero compiler work" constraint),
so the log below records both the library work and the compiler changes it required.

**M0 (probes).** The mechanism survey found the discovery template's assumptions half-true
and half-blocked. What was a mechanical spelling issue: a hole inside a **string literal**
(`"$C"`) never substitutes (a string is one opaque token) — reify a name with `.name`
instead. What was genuinely blocked (and is now fixed in the compiler, below): P2 — the
registration vehicle (`namespace HarpoonReg { bool $C = seed(...); }`) silently lost its
seeding because a namespace-scoped global's initializer ran before top-level globals
existed. P1/P4/P5/P6/P7 (the `$for`/`hasAttr`/`hasBase`/import-scoping/duck-`==`
machinery) all passed as designed and needed no change.

**Compiler fixes (the reason discovery could land).**
1. **Global initialization order** (`Eval.cpp`, `Lower.cpp`) — reworked so **every global,
   top-level and namespace-scoped, auto-constructs to its §3 default before any initializer
   or top-level statement runs**. A namespace initializer (the registration vehicle) then
   sees a real slot for every global and its mutations persist; a bare namespace global is
   constructed at all (previously never); source-file order and `[[dep]]` package
   boundaries no longer change the result. Retired bugs #75/#76/#79/#80 (reference.md §4.3b
   documents the resulting order; the one residual — a namespace initializer sees a
   *top-level* global's default, not its explicit value — is now a defined, documented
   ordering, `docs/footguns.md` "by design").
2. **Interface-typed `using` dispatch** (`Eval.cpp`, `Lower.cpp`) — `close()` now dispatches
   on the binding's **runtime class**, so `using IDisposable d = ...` runs the concrete
   override instead of the interface's empty requirement (teardown was silently skipped).
   This is what lets the IDisposable discovery rule route teardown through a `__dispose(d,
   body)` helper (`using IDisposable` inside it) rather than needing `using` in the injected
   quasiquote (a separate fragment-parser gap, still open but no longer on the path).
3. **Rule-engine hole capabilities** (`Rules.cpp`, `Resolver.cpp`) — three additions the
   discovery template needs: `$C.name`/`$m.name` reifies a matched decl's simple name as a
   string literal; a `$for`-bound `meta::Method` used in member-selector position (`t.$m()`)
   splices its name as the selector (so a rule can discover *and invoke* members); and `$C`
   in type position (`$C t = $C()`) carries the class's resolved symbol through pass-2 so it
   wins over a same-named value the injection itself declares (the type-position analogue of
   bug.md #22). reference.md §6.9.

**M1 — assertions,** four engines (`harpoon/src/assert.lev`, `tests/assertions/`). One
correction to §4.2: `assertSame`/`assertNotSame` are generic (`<T>`), not `object`-typed
(no universal reference supertype). Found #77 (a `struct` without an explicit `(==)` is not
field-wise) — kept open (a semantics ruling), documented on `assertEqual<T>`.

**M2 — registry + runner, hand-seeded,** four engines (`harpoon/src/{registry,runner}.lev`,
`tests/registry_runner/`). This is now the **byte-identical twin** of the discovered suite
(M3), not a stand-in for it.

**M3 — discovery, LANDED** (`harpoon/src/discover.lev`, `tests/discovery/`). Two rules
(`discoverTests` / `discoverDisposableTests`, `where`-split on `hasBase("IDisposable")`)
inject one `namespace HarpoonReg { bool $C = harpoon::__seed(...); }` startup global per
`@Test` class, exactly as §5.2 specifies — the template works verbatim now that the compiler
supports it (`$C.name`, `$for … : harpoon::__entry($m.name, () => { $C t = $C(); t.$m(); })`,
the disposable variant routing through `__dispose`). The rule-discovered report is
**byte-identical** to M2's hand-seeded twin (the rule_orm twin discipline), verified across
oracle/IR/LLVM. Misdeclared-`@Test` loudness is exercised (a non-`void` method → ERROR).

**M4 — acceptance matrix.** Lifecycle teardown after pass AND fail (acceptance 2 — the
IDisposable `LifecycleTests` in the discovery suite); async + threaded tests (acceptance
4/5 — `tests/concurrency/`, `await`/`spawn` inside `@Test void` methods, discovered);
inline-vs-sibling equivalence (acceptance 6/8 — `examples/inline/` and `examples/sibling/`
produce byte-identical reports); three-engine agreement throughout.

**M5 — close-out.** `harpoon/README.md` rewritten for the shipped surface; `info.md` §0
entry; this doc moved to `designs/complete/`; the fixed bugs deleted from
`known_bugs_{1,2}.md` and `docs/footguns.md`; reference.md updated (§4.3b init order, §5.2
interface `using`, §6.9 hole capabilities).

**Bugs found this track:** FIXED and retired — #75, #76, #79, #80 (the global-init family)
plus the interface-`using`-close defect (never filed; found and fixed in one pass). Found
and KEPT OPEN — #77 (struct `==` not field-wise; a semantics ruling) and #78 (a bulk `uses`
import's function shadows a same-named local top-level function; worked around by not naming
a test entry point `main`). Still deferred (unchanged): `@Skip("reason")` reasons and
parameterized theories (LA-4), the `using`-inside-a-quasiquote-lambda fragment-parser gap
(routed around via `__dispose`).
