# Harpoon — the standard Leviathan unit test library

Status: **v1, complete.** Design: `designs/complete/techdesign-unit-test-library.md`.
Assertions, the test-case registry, the runner/reporter, and **automatic
`@Test` discovery** (zero manual registration) all ship and pass on the
tree-walk oracle, the IR interpreter, and LLVM native (emit-C++ compiles the
non-async surface). Consume it as an ordinary Trident dependency.

## Usage

Add Harpoon as a dependency and write a two-line entry file:

```toml
# your-tests/trident.toml
name = "my-tests"
sources = ["my_tests.lev"]

[[dep]]
path = "path/to/harpoon"
as = "harpoon"
```

```lev
// my_tests.lev
uses harpoon;

class MathTests {
    @Test void addsNumbers()      { harpoon::assertEqual(4, 2 + 2); }
    @Test void subtracts()        { harpoon::assertEqual(1, 4 - 3); }
    @Test @Skip void notYet()     { }                 // reported as "skip"
}

harpoon::main();   // runs every discovered @Test, prints the report, sets the exit code
```

That is the whole surface. Mark a method `@Test`, `uses harpoon;` (which you
need anyway for the assertions), and the tests are discovered and run — no
registration list anywhere.

- **A test is a zero-arg `void` method marked `@Test`** on an ordinary class.
  Setup is the constructor (a fresh instance per test). Teardown is
  `IDisposable.close()` — implement `IDisposable` and `close()` runs after
  every test of that class, including one that fails (via `using`). Async and
  threaded tests are just `await`/`spawn` inside the body — the runner needs no
  special casing (it parks and resumes on the test's own suspension points).
- **`@Skip`** on a `@Test` marks it skipped (reported, not run). A `@Test` on a
  non-`void` or parameterized method is reported as an **ERROR** (misdeclared),
  never silently dropped.
- Failure is an exception: `harpoon::assertEqual`/`assertTrue`/… throw an
  `AssertionException` (scored **FAIL**); any other throw is scored **ERROR**.
  Any class implementing `IAssertionException` is scored as a failure too.
- `harpoon::main()` runs everything, prints the report, and sets the process
  exit code (0 if every test is `ok`/`skip`, 1 if anything is `FAIL`/`ERROR`).
  Filter with `--filter <substring>` matched against `Class::method`.

Discovery is **location-agnostic**: `@Test` classes inline with the code they
test and classes in a dedicated `tests/` project are found identically — the
trigger is `uses harpoon`, not the file's path. Both shapes ship as worked
examples (`harpoon/examples/inline/`, `harpoon/examples/sibling/`) and produce
byte-identical reports.

## Assertion vocabulary (`harpoon/src/assert.lev`)

`assertEqual` (scalars, typed arrays, and a generic duck-`==` fallback for
structs/enums), `assertNotEqual`, `assertSame`/`assertNotSame` (reference
identity), `assertTrue`/`assertFalse`, `assertNone`/`assertSome`,
`assertThrows` (returns the caught `IException`; a `mustContain` variant checks
the message), and `fail`. See `harpoon/tests/assertions/main.lev` for every
overload exercised on its pass and fail path.

## Layout

```
harpoon/
  trident.toml
  src/{assert,registry,discover,runner}.lev
  tests/
    assertions/       # direct assertion-vocabulary tests
    registry_runner/  # hand-seeded registry+runner — the byte-identical TWIN of…
    discovery/        # …the @Test-discovered suite (rule_orm twin discipline)
    concurrency/      # async + threaded @Test acceptance (await/spawn in a test)
    probes/           # M0 mechanism probes (kept green)
  examples/
    inline/           # @Test classes in the same file as the code under test
    sibling/          # app/ + tests/ as two projects ([[dep]] on app + harpoon)
```

## Remaining gotchas (see `docs/footguns.md` for the cross-project registry)

- **Give your test-suite entry point any name but `main`** if the file also
  `uses harpoon;` — a bulk `uses` import's `main` silently wins over a
  same-named local `main`, no diagnostic (known_bugs_1.md #78). Harpoon's own
  self-tests call `harpoon::main()` directly (they have no local `main`);
  `tests/assertions/main.lev` names its driver `runSelfTest()`.
- **A `struct` with no explicit `(==)` is not field-wise by default**
  (known_bugs_2.md #77) — `assertEqual`'s generic fallback needs one defined,
  or compare with `assertTrue(a == b, msg)` / field-by-field instead.
- **`@Skip("reason")` reasons and parameterized theories** are deferred — they
  need attribute-value reflection (LA-4). v1's `@Skip` is a bare marker.
