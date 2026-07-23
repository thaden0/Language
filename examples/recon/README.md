# Recon — a terminal REST client, in Leviathan

Recon is a **Postman-in-the-terminal**: import Postman collections, build & send
HTTP/HTTPS requests with `{{variable}}` interpolation, inspect responses, run a
native declarative test/extract layer, and manage cookies/sessions/history — all
in a Moby TUI.

It is the **first end-to-end Leviathan application** and the **reference standard**
for how Leviathan apps are structured, named, tested, and packaged. The full spec
is [`DESIGN.md`](DESIGN.md); every language/framework fact it relies on is verified
in [`RESEARCH.md`](RESEARCH.md). This README is the quick-start + the normative
coding standard (DESIGN §12).

## Build, run, test

```sh
# from the repository root
build/trident check examples/recon          # type-check the whole app
build/trident run   examples/recon           # launch the TUI (needs a real terminal)
build/trident run   examples/recon collection.json   # open a collection on launch

# corpus tests — runs every test on BOTH lanes (oracle + IR), byte-identical
examples/recon/tests/run-tests.sh            # all tests
examples/recon/tests/run-tests.sh examples/recon/tests/eval   # one test
```

The **run lanes are the tree-walk oracle and the IR interpreter** (`trident run`
+ `leviathan --plan build/plan.lvplan --ir`). The compiled lanes are not v1
targets (emit-C++ has no `App.run()`; LLVM segfaults on component paint, bug #67).
Networking and the run loop are engine-clean; only Moby *paint* pins us to the
interpreters.

## Layout

```
src/
  main.lev            entry: env read, DI root, launch (top-level main())
  app.lev             AppState (the mutable hub)
  model/              domain classes (request/collection/environment/cookie/session/history/tests)
  net/                url · vars · auth · body · sender (redirect+timeout state machine)
  io/                 jsonio · importer · exporter · store (persistence)
  eval/               jsonpath · runner (native assertions/extractions)
  ui/                 sources · textarea · panels · keymap · commandbar · dialog · reconapp
fixtures/             sample Postman collection + environment
tests/                *.lev + *.expected corpus (run on oracle + IR)
themes/recon.toml     the shipped theme
```

## Architecture

Everything except `ui/` is **pure, headless, and unit-testable without a terminal**
(DESIGN §2). The UI is a thin, replaceable shell over a fully-tested core. Network
work is **callbacks + timers** on the single-threaded event loop the Moby run loop
already rides — **the UI task never `await`s** (DESIGN §2.1).

## Coding standard (DESIGN §12, normative — later apps copy this)

- **Files:** one concern per file, grouped in `src/<area>/`; 4-space indent; a
  method body is one statement (block or `=>`).
- **Namespaces:** one app namespace `Recon`; files reopen and merge it. `uses Moby;`
  goes **inside** the namespace block in UI files. Namespace globals are written
  **bare** (`x = v;`, never `NS::x = v;`).
- **`class` vs `struct`:** default to `class`; use `struct` only for small copy-
  semantics value bundles — and never one holding an `enum` field inside an `Array`
  (bug #41) nor one used as a `Map` value on a class field (bug #49).
- **No statics:** a "static factory" is a **labeled constructor** (`new Of(...)`) or
  a **free function** returning `T?` (`parseUrl(string) -> Url?`).
- **Optionals & no truthiness:** total functions return `T?` and never throw for
  expected absence; **always narrow with `if (x != None) { ... }` or `x ?? d`** —
  early-return/negative narrowing does **not** persist in Leviathan, so the positive
  block form is mandatory. Conditions are `bool`; write `if (x != None)`, never
  `if (x)`. Never declare an optional function-typed field (`((T)=>R)?`, bug #51).
- **Collections:** `m[k] = v` bracket sugar for map writes (bug #18); `a = a.add(x)`
  to append; index a `Map<K,int>` into parallel arrays instead of `Map<K,Struct>`.
- **Lambdas/handlers:** call sibling instance methods with an explicit
  `this.method()` inside any lambda (bug #53 — bare `this` segfaults on native/LLVM).
  Bind a `char` local before comparing/passing (bug #50).
- **Env:** read `env::get(...)` at the top-level `main()` and pass values down (bug #68).
- **Moby UI:** prefer composing shipped components; where a custom container is
  unavoidable, build a plain class that *owns* a framework `Container` rather than
  subclassing one (sidesteps the MI-leaf paint hazards). Custom **leaves** override
  `contentDesired` + `paintContent`, register handlers in the ctor with explicit
  `this.`, and `invalidate()` on state change.
- **Async:** UI code never `await`s — callbacks + `App.every`/`std::after` timers.
  `await`/`Promise`/`awaitTimeout` are for headless code and tests only.
- **Headless App tests:** inject `TestRenderer`/`ScriptedInput`, drive with
  `pumpOnce()`, and **always `quit()` + `stopSession()`** before returning (else the
  frame timer keeps the event loop alive and the program hangs).

## Status

**M1 — headless core (DESIGN T0–T6): complete and corpus-tested.** Models, URL/var
resolution, Postman import/export (round-trip + v1 refusal), the auth/body/redirect/
timeout send pipeline (verified against a hermetic in-language `HttpServer`), the
JSON-path assertion/extraction runner, and cookie/session/history/settings
persistence — all byte-identical on the oracle and IR lanes.

**M2 — interactive shell (T7–T9): implemented.** `AppState` hub, the `ReconApp`
component tree (top bar / sidebar `TreeView` / request panel / response panel /
bottom bar), the `TextArea` multi-line editor, source adapters, command registry,
command bar, dialogs (over Moby's now-shipped `Modal`), keymap, and the callback-
driven send flow. A headless end-to-end corpus test composes the app, loads a
collection, selects a request, feeds a mock response (tests + history), exports, and
paints frames.

**Deviations from DESIGN (both since RESEARCH.md was written):**
1. Dialogs compose Moby's now-shipped `Modal`/`alert`/`confirm` instead of a
   hand-rolled custom Container-leaf over the raw overlay stack (§9.4 assumed no
   Modal existed). Same intent, far more robust.
2. Panels are plain builder classes that own framework `Container`s rather than
   custom `Container` subclasses — avoids the MI-leaf paint bug family entirely.

**Remaining polish (T10/T11):** in-app history/env-editor dialogs, unresolved-var
flagging, and theme `import()` wiring are partial (the export command, persistence,
and history model are done; the dialog *screens* for them are the open work).
