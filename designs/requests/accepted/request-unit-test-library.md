# Summary: A standard unit test library — xUnit-style test classes, assertions, and first-class async + thread test support

Leviathan and its ecosystem projects (the compiler's own corpus, Trident, Atlantis, Sonar) all
currently test themselves with the house corpus pattern: a program whose stdout matches an
`.expected` file, exit code 0. That pattern works but gives every project its own ad hoc
assertion vocabulary and no shared way to organize cases into named groups, report pass/fail
counts, or run async/threaded test bodies uniformly. This request is for a small, standard unit
test **library** (not a framework on the scale of Atlantis or Sonar) that every Leviathan
project — including this compiler's own future self-hosted stdlib — can depend on via Trident,
the same way .NET/Java projects reach for xUnit/NUnit/JUnit.

This does not block any in-flight design; it is additive tooling meant to standardize testing
across the ecosystem going forward, most usefully once Trident supports pulling it in as an
ordinary dependency.

## Request Details

The shape should be **xUnit-style, class-based** — a test is a method on an ordinary class, not
a callback registered through a lambda-taking function. (An earlier draft of this request sketched
a `describe`/`it` callback API; that was carried over from a different library's convention by
mistake and is **not** what's being asked for — scrap it.)

```lev
class MathTests {
    int fixtureValue;

    new MathTests() {                  // constructor = setup; a fresh instance per @Test method,
        fixtureValue = 10;             // the xUnit.net convention — no separate @Setup attribute
    }                                  // needed, reusing a lifecycle the language already has.

    @Test
    void addsNumbers() {
        assertEqual(fixtureValue + 2, 12);
    }

    @Test
    async void resolvesFromTheNetwork() {
        var v = await fetchThing();
        assertEqual(v, 42);
    }

    @Test
    void runsOnAWorker() {
        var h = spawn(() => heavyCompute());
        assertEqual(await h, 42);
    }
}
```

A class needs no special base type or marker to be a test class — `@Test` on a method is the
only thing the runner looks for. Each `@Test` method runs against its own fresh instance
(constructor = setup); if the class implements `IDisposable`, the runner disposes that instance
after the method runs (teardown), which is the language's existing `using`/`IDisposable` rule
(reference.md, cited in `designs/complete/techdesign-metaprogramming.md` §16.5) rather than a
new `@Teardown` attribute — one lifecycle mechanism instead of two. This mirrors xUnit.net's
actual design (constructor/`Dispose` as setup/teardown, no `[TestFixture]`/`[SetUp]` attributes)
more than NUnit/MSTest's, and fits this language's "one rule over many special cases" instinct
(info.md §1) better than inventing a parallel lifecycle-attribute vocabulary.

Test bodies need to cover every effect this language can produce, not just synchronous value
checks — the two `@Test` methods above besides `addsNumbers` aren't decorative:

- **Async** — a `@Test` method may `await` (LA-30's task/Promise substrate, landed 2026-07-12);
  the runner treats the method as a task and awaits its completion before scoring pass/fail.
- **Threads** — a `@Test` method may `spawn` real OS workers (Track 10 threads, landed
  2026-07-11), join them, and assert on the outcome. A unit test library that can't exercise
  `spawn`/`Promise` join semantics can't test a meaningful fraction of what this language is
  being built to do.

Neither needs special-casing in the runner: `@Test` methods are ordinary methods, and
`await`/`spawn` are already ordinary expressions, so async and threaded tests fall out of "a
test is a method" for free — no `@AsyncTest`/`@ThreadedTest` variant attribute.

### Where the metaprogramming opportunity is

Discovery is the interesting part, and it's a good match for landed capability rather than a
place to hand-roll a manual suite list. Phase 1 (`attribute`/`@Name(args)`) and Phase 2 (rules:
`match`/`inject`/quasiquote) are both landed (info.md §16.5); the `@Route` → route-registration
example there is exactly this problem's shape ("the annotation is written and the wiring
appears"). The ask: whichever mechanism the design lands on, `@Test` should be a **pure
marker** the author writes once, with zero manual "register this test/class with the suite"
boilerplate anywhere — the same ergonomics `@Route` already has for controllers. See Known
Warnings for the one open question this raises (whether the landed rule anchors are sufficient,
or whether this needs Phase 3's namespace-scope anchors, or the landed procedural-macro `Ast`
capability instead).

### On the name

The current working name for this is a placeholder — the intent is to reuse the name **Sonar**
for it, since "sonar" (a detection system) is a better metaphorical fit for a bug/regression
*detector* than for the TUI framework `designs/sonar/` currently uses it for. That TUI framework
would need a new name first, freeing "Sonar" up for this library. See "Other" below for naming
candidates for the TUI framework; this is flagged as an open decision, not something this
request resolves on its own.

## Requested Specific Feature

1. **`@Test` as a pure marker attribute on ordinary methods** — no required base class, no
   decorator-registered lambda. Discovery is metaprogramming's job (rules/attributes, §
   "Where the metaprogramming opportunity is" above), not the test author's.
2. **Constructor-as-setup, `IDisposable`-as-teardown**, one fresh instance per `@Test` method —
   reuses landed lifecycle mechanisms instead of inventing `@Setup`/`@Teardown` attributes.
3. **An assertion vocabulary** — `assertEqual`, `assertTrue`/`assertFalse`, `assertThrows`,
   `assertNull`/`assertSome`, etc. — covering value equality across primitives, structs, arrays,
   and maps, plus identity where relevant, with labeled failure output naming the assertion,
   expected, and actual (in the spirit of Sonar's own `SonarTest::eq` "prints ok/FAIL + diff").
4. **Async test methods** — a `@Test` method may internally `await`; the runner treats the
   method as a task and awaits its completion before scoring pass/fail. No separate
   `@AsyncTest` — one `@Test` shape, per the "one rule" instinct above.
5. **Threaded test methods** — the same `@Test` method may `spawn` workers and `await` the
   join; no special casing needed beyond what already flows from `spawn`/join being
   `Promise`-shaped (LA-1, reference.md §14).
6. **Location-agnostic discovery** — a `@Test`-attributed method is found whether its class
   lives in a dedicated `tests/` directory/file or inline in the same file as the code it's
   testing, with no difference in behavior between the two. Since discovery is attribute/rule
   driven (whole-program, `uses`-graph scoped — info.md §16.5) rather than path- or
   naming-convention-driven, this should fall out for free once discovery is built, and is
   called out here only so it's tested, not assumed.
7. **A runner + reporter** — discovers every `@Test` method in the program, runs each against
   a fresh instance, prints a report (class name, method name, pass/fail, a summary count),
   exits non-zero on any failure; usable both as `trident test`-style tooling and as a plain
   `.lev` program (`trident run`) for direct iteration while writing tests.
8. **Fast local iteration, not a monolith rebuild.** See the Trident/multi-app note below —
   whatever shape this takes should let a developer re-run just the test binary they're
   currently writing without the full application also rebuilding.

### Trident / multi-app note

Trident today builds one `trident.toml` → one binary. The natural shape for "a unit test app
alongside a main app" is the same pattern Sonar's own examples already use
(`designs/sonar/techdesign-10-testing-delivery.md` §6): a `tests/` (or per-suite) directory with
its **own** `trident.toml` declaring `[[dep]] path = "../.."` back to the app/library under
test. Each test suite is then an ordinary small standalone project, builds and re-runs
independently, and nothing about "multiple apps in one repo" needs new Trident machinery —
recommended as the default answer here (see Known Warnings for why this matters).

## Known Warnings

- **This intersects the Trident post-v1 deferral tracker's D-A item (workspaces/`[targets]`)** —
  `designs/deferal-trident-post-v1.md` §3 deliberately defers multi-target/workspace
  orchestration until "the first real multi-target consumer" exists, with an explicit STOP
  condition (§9(f)) against starting it early without an owner ruling. A naive reading of "Trident
  needs to know how to handle multiple apps" for this request could look like exactly that
  trigger firing. It is **not**, if built via the local-path multi-project pattern above (already
  proven by Sonar's `examples/`) — that needs zero new Trident features. Flagging so whoever
  picks this up doesn't reach for `[targets]`/`[workspace]` without checking that tracker first.
- **Discovery may outrun the landed rule anchors.** Phase 2's landed injection anchors are
  `top`/`bottom of C.constructor` and `member of C` (info.md §16.5) — both scoped to a single
  matched class. A zero-boilerplate, whole-program `@Test` registry (so the runner doesn't need
  a hand-maintained list of test classes) most naturally wants a namespace/program-scope anchor
  ("append this entry to a global list"), which is explicitly **not yet landed** — info.md §16.5
  lists "namespace anchors" under "Remaining for Phase 3+." Two ways around this without waiting
  on Phase 3, for the design to weigh: (a) per-class injection at the constructor that
  self-registers each instance the first time it's constructed, if the runner can be made to
  construct one instance of every `@Test`-bearing class it's linked against some other way; or
  (b) the landed procedural-macro `Ast`/`meta::parseExpr` capability
  (`designs/complete/techdesign-procedural-macros.md`, comptime code→code, no ABI/backend
  change) driving a single macro call that emits the whole registration table. Neither is
  prescribed here — this is a design-time decision, flagged so it isn't discovered mid-implementation.
- **Overlaps with Sonar's own test harness** (`designs/sonar/techdesign-10-testing-delivery.md`)
  — Sonar already ships a `SonarTest` namespace (`eq`, `snap`, `harness`, `reset`) purpose-built
  for TUI snapshot testing (TestRenderer/ScriptedInput). That is UI-specific and narrower in
  scope than what's requested here. Worth a later look at whether Sonar's harness could sit on
  top of this library's assertion vocabulary instead of duplicating one — not a blocker, just a
  convergence point once both exist.
- **Prelude-not-checked** (memory: `leviathan-prelude-not-checked`) — if the assertion functions
  or the `@Test` discovery machinery end up living in prelude/native surface rather than plain
  stdlib `.lev`, they can't rely on checker-derived type annotations. Prefer implementing as
  ordinary stdlib `.lev` if at all possible.

## Acceptance Criteria

1. A `@Test`-attributed method is discovered and run with zero manual registration — no file
   anywhere lists "the tests to run" by hand.
2. A test class needs no special base type; a class implementing `IDisposable` gets disposed
   after each `@Test` method runs on the fresh instance constructed for it (constructor = setup,
   `dispose()` = teardown); a class not implementing it works fine without teardown.
3. The assertion vocabulary covers primitives, structs, arrays, and maps for equality, plus
   `assertThrows` for exception cases, with a labeled expected/actual failure message.
4. A `@Test` method that `await`s inside it is awaited by the runner before the result is scored
   (a corpus case proving a deliberately-delayed async assertion is still caught if it fails).
5. A `@Test` method that `spawn`s a worker, joins it, and asserts on the joined result
   passes/fails correctly (a corpus case proving cross-thread test coverage, not just
   single-loop async).
6. Two worked examples both discover and run identically: (a) a project with all `@Test` classes
   collected under one `tests/` directory, and (b) a project with `@Test` classes declared
   inline in the same files as the code they test — same runner, same output shape, no
   configuration difference between the two.
7. The library's own self-tests run green and byte-identical across oracle/IR/LLVM (the
   ecosystem's standing differential-testing rule).
8. A worked example shows a small app + a sibling test suite as two independent `trident.toml`
   projects (local-path dep pattern); editing only the test suite and re-running it does not
   require the app project to rebuild.

## Intrim Fallback

No work is blocked or parked by this request — every project needing tests today keeps using
the house corpus (print + `.expected`) pattern, including Sonar's own `SonarTest` harness for
its UI-specific needs. This request is additive standardization, not a prerequisite for
anything currently in flight.

## Other

**Naming candidates for the TUI framework currently at `designs/sonar/`**, to free up "Sonar"
for this library (per the requester's note that "Drown" reads too dark) — staying in the
established nautical/dark-fantasy register (Leviathan, Trident, Atlantis):

- **Fathom** (recommended) — a unit of ocean depth, and "to fathom" also means to understand/
  grasp something fully, which doubles nicely as a name for a framework whose whole job is
  rendering dense information onto a small terminal surface for a human to make sense of.
- **Helm** — a ship's steering station; apt for an interactive control-panel TUI, though it
  collides with the well-known Kubernetes package manager of the same name outside this
  ecosystem, which may cause search/branding confusion.
- **Beacon** — a lighthouse signal; nautical, guiding-light connotation fits a UI/dashboard
  framework, reads a shade lighter/less dark-fantasy than the others.

This is a naming proposal only — actually renaming `designs/sonar/` and moving its files is a
separate follow-up action, to be done once a name is chosen and approved, not performed as part
of filing this request.
