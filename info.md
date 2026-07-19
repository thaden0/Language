# Leviathan — Language Design Working Specification

**Leviathan** is the project name and the compiler binary. The default source extension is
`.lev` (`.ext` still works — see `docs/reference.md` for exact invocations, and
`designs/complete/techdesign-toolchain.md` for the landed `trident`/`leviathan` split). Everything
below is the design of the language Leviathan implements.

A statically-typed, object-oriented language influenced by C++, C#, TypeScript, and
Haskell. The guiding philosophy throughout: **eliminate special cases by finding the
general rule they are all instances of, prefer explicit over implicit, and refuse
unprincipled syntactic sugar.** Where a feature reads as "special," the design looks for
the underlying rule that makes it ordinary.

A recurring structural pattern: **safe/managed by default, with raw power available behind
an explicit gate** — the "bazooka in a marked room." The safe surface is a guarantee, not a
convention; the dangerous surface is opt-in and visible.

---

## 0. Last Updated

**Audited against the implementation on 2026-07-19.** This section is the running log of
what has landed and what has changed; the numbered sections below carry the detail.

**Landed since the last full pass** (all cross-checked against `docs/reference.md`, the
as-implemented reference, and the git history):

- **Explicit generic type arguments at call sites** (2026-07-19) — the canonical,
  unambiguous spelling is `callee::<T, U>(args)`, after the complete callee (including
  namespace, constructor label, or receiver method). Exact arity is required; explicit
  bindings are authoritative and filter overloads before substituted value checking. The
  list is checker-only, preserved by rules/specialization/reifier clones and `--expand`, and
  adds no runtime or ABI payload. Tree-walk/IR/emit-C++/LLVM are covered; no ELF gate
  (`docs/reference.md` §2.5/§3.3).
- **Harpoon — the standard unit test library, COMPLETE** (`designs/complete/techdesign-unit-test-library.md`,
  2026-07-15) — the assertion vocabulary (`assertEqual`/`assertNotEqual`/`assertSame`/
  `assertTrue`/`assertFalse`/`assertNone`/`assertSome`/`assertThrows`/`fail`, all
  overloads incl. the generic duck-typed fallback), the `TestCase` registry, the
  `runAll`/`main` runner+reporter, AND **rule-based `@Test` auto-discovery** (zero
  manual registration — mark a method `@Test`, `uses harpoon`, done) all **landed and
  pass on oracle/IR/LLVM**, consumable as an ordinary `[[dep]]`. Constructor-as-setup,
  `IDisposable`-as-teardown, zero special-casing for `await`/`spawn` in a test; the
  rule-discovered suite is byte-identical to a hand-seeded twin; inline and
  sibling-project examples produce identical reports (location-agnostic discovery).
  `harpoon/`, `harpoon/README.md`. **This track was library-only per the design, but
  finishing discovery required fixing the compiler** (owner directive): global
  initialization was reworked so **every global auto-constructs before any initializer
  runs** (fixing the namespace-init-can't-see-globals family — was #75/#76/#79/#80,
  now retired), interface-typed `using` now dispatches `close()` on the runtime class
  (teardown was silently skipped), and the rule engine gained `$C.name`/`$m.name`
  string reification, `$for`-bound-method selector splicing (`t.$m()`), and
  type-position `$C` hygiene (reference.md §5.2/§6.9). Two bugs found and since FIXED
  (2026-07-15): #77 (struct `==` is now field-wise by default, info.md §9 / reference.md
  §equality) and #78 (a file's own top-level function now wins ties over a same-named
  bulk-`uses` import).
- **`enum`** (Track 03 §2) — closed, `int`-carried value type; `::`-member access,
  `code()`/`toString()`/`fromCode`, exhaustive `match`. Desugars to a value `struct` +
  const globals, so it is **full-coverage on all four active engines** (reference.md §4.2c).
- **`Block`** (Track 03 §3) — the gated fixed-length mutable byte buffer (§16 gate); a
  reference type with bounds-checked byte/int32/int64 access and aliasing `slice`. On
  oracle/IR/emit-C++/LLVM via the `LV_BLOCK` ABI tag (reference.md §6.10).
- **`char`** (Track 03 §1) — one Unicode scalar, unboxed, target-typed literals, no
  arithmetic; on all four active engines via `LV_CHAR` (§9, reference.md §2.2/§6.1).
- **`namespace math`** (Track 06) — `pi`/`e`, libm transcendentals, overloaded `min`/`max`;
  top-level, not `std::math` (reference.md §6.1b).
- **Named arguments + default parameter values** (§2, §9) — landed, compile-time
  normalization only, no runtime calling-convention change.
- **Method / function / constructor references** (LA-25) — a `::`-reached callable in value
  position is a first-class function value; dispatch = its eta-expansion lambda
  (reference.md §3.4).
- **Bound method references** (F3) — `obj.method` in value position captures a bare local,
  parameter, or `this` and yields the method signature minus its receiver. It is the same
  checker-only eta expansion as LA-25, with target-typed overload resolution and ordinary
  runtime-class dispatch (reference.md §3.4).
- **Generic static-shaped members** (LA-18) — callable-level `T::member` resolves per concrete
  whole-program instantiation through demand-driven monomorphization above the IR; missing members
  report both the generic use and instantiation site. Class-level/HKT/override-set v1 limits are
  explicit (reference.md §2.5, `designs/complete/techdesign-generic-static-members.md`).
- **Class-method dispatch** — an unqualified instance-method call dispatches on the
  receiver's **runtime** class (uniform for class- and interface-typed receivers),
  statically devirtualized when the candidate set is provably closed (reference.md §3.4a).
- **Regex** (Track 10, LA-13) — **landed in full**: the in-language Thompson/Pike/lazy-DFA
  engine core (§11, M1–M5, no natives, comptime-foldable `regex::compileProgram` boundary,
  reference.md §6.4.4) **and** the public `Regex`/`Match`/`Group`/`RegexOptions`/
  `RegexException` C#-shaped surface + `namespace regex` conveniences (reference.md §6.4.6,
  `designs/complete/techdesign-regex-library.md`). A checker overload-resolution bug (bug.md #34 — a
  lambda-literal argument wrongly scores as applicable to a `string` parameter) was worked
  around with a source-declaration-order fix, not a compiler fix; see the design doc's
  implementation log.
- **Threads / workers** (Track 10) — `spawn`/`Worker<T>`/`Channel<T>` on all three active
  engines; true OS threads on LLVM; copy-always boundaries (§14, reference.md §6.6.66).
- **TLS + crypto** (LA-2) — wrap-in-place TLS as an fd property, `sysRsaEncrypt`,
  crypto-grade `sysRandom`; oracle/IR/LLVM (§13, reference.md §6.6.5x).
- **comptime `import(path)`** (LA-20) — the one sanctioned build-input read inside hermetic
  comptime; trident `assets = [...]` (§16.5, reference.md §6.9).
- **True suspension** (LA-30, flipped 2026-07-12) — `await` **parks a stackful task** on all
  three active engines (per-task mmap'd stacks, per-thread FIFO scheduler); the v1 pump is
  retired to a `LANG_PUMP=1` escape hatch pending deletion. Three deliberate corrections
  shipped with the flip: completion-ordered resumption (C1), failure routing (an uncaught
  callback throw is program-uncaught, never delivered to an unrelated `await` — C2), and
  drained-loop `await` throws instead of fabricating a default (C3). Surface unchanged — no
  coloring (§14, reference.md §6.6.67).
- **Cancellation, timeouts, structured concurrency** (LA-30 B2, landed 2026-07-12) —
  `CancelledException` (an ordinary catchable `IException`, delivered **only** at park
  points — a running task is never preempted), `TaskGroup` (structured concurrency over the
  existing `using`/`IDisposable` rule, zero new syntax — `run`/`cancelAll`/`close`, with a
  shield rule so a group's own `close()` unwind can't be re-cancelled into a livelock), and
  `awaitTimeout<T>(Promise<T>, ms) -> T?` (timeout as a `None` outcome, not a failure; v1
  stops waiting but does not reach back to cancel the producer task — compose with
  `cancelAll()` for that). Extends the doc-1 task substrate (multi-key park, per-task shield
  mask, a thread-local id→task registry — ids, never handles, cross the native boundary);
  no new syntax, no ELF lane (§14, reference.md §6.6.68).
- **Metaprogramming Phases 1–4 + procedural macros (F4)** — attributes + rules
  (match/inject/quasiquote), expression macros, Layer-D body-`rewrites`, and
  `macro name(string payload) comptime` returning opaque `Ast` through
  `meta::parseExpr`/`parseStmts` (§16.5, reference.md §6.9).
- **Weak fields (F5)** — `weak T?` class/interface fields are non-owning back-edges;
  dead reads yield `None`, live reads become ordinary owned values, and thread-copy
  boundaries rebuild them as `None`. All four active engines; no frozen-ELF lane
  (reference.md §4.3d).
- **Covariant-return interface satisfaction (F6)** — an interface method requirement may
  be implemented with the same name and invariant parameter types but a more specific
  declared class/interface return (including `T` for required `T?`). The requirement
  remains slotless, so fluent concrete return types and interface dispatch share the
  implementing class's one method slot (reference.md §4.2).
- **Sonar T01 core** — the retained terminal UI foundation now ships as the `sonar/`
  package: geometry/style values, frozen 8-byte cell surfaces with wide-glyph healing,
  component damage/lifecycle/handlers, collapse-based mixins, containers, and damage
  sweeps. Its P1–P11 architecture probes and focused core suite pass on oracle, IR,
  emit-C++, and LLVM (`designs/complete/techdesign-01-core.md`).
- **Sonar T02 layout** — `sonar/src/layout/{flex,grid,dock,stack}.lev`:
  `FlexLayout`/`GridLayout`/`DockLayout`/`StackLayout` implementing `ILayoutStrategy`,
  a shared L-to-R flex-distribution/clamp-fixpoint helper, and the width-then-height
  wrap re-measure protocol; `Container`'s default layout is now `FlexLayout(Axis::Vertical)`
  (T01's bootstrap `CoreVerticalLayout` retired). Table-driven test suite passes on
  oracle, IR, emit-C++, and LLVM (`designs/complete/techdesign-02-layout.md`). Found and
  worked around bug.md #36 (emit-C++ silently fails an interface→class `is`-narrow for
  a `Component`-shaped class; worked around by promoting `dock`/grid fields to
  `IComponent` field requirements instead of narrowing).
- **Columnar `Array<struct>`** (landed, flipped to default 2026-07-12) — an `Array<T>`
  whose element is a **scalar-only value struct** (all fields `int`/`float`/`bool`/`char`)
  is stored **struct-of-arrays** (one allocation, tag-free 8-byte columns, no per-row
  classId/dyn) instead of row-major on the native backends. **No user-facing surface** — no
  type, annotation, or syntax; value semantics make the layout unobservable (differential
  corpus proves oracle/IR/emit-C++/LLVM identical in both layouts). A direct-index field
  scan `arr[i].hot` **fuses to one `ColGet`** (zero-gather column read) — the locality win
  (~4.5× on a wide struct, ~2× footprint); whole-struct/`for-in`/pipeline access **gathers**
  a fresh record per element (no win in v1). Ineligible structs keep row-major forever.
  Default-on with a `--no-columnar` escape hatch; **no ELF lane** (X64Gen frozen). The
  LA-30 pump-flip pattern, applied to a layout (reference.md §7.4,
  `designs/complete/techdesign-columnar-arrays{,-2}.md`).
- **Sonar T03 events/input** — `sonar/src/{events,focus,input,keymap}.lev`: event
  classes + capture/target/bubble dispatch, the ANSI/UTF-8 input decoder
  (Ground/C0/CSI/SS3/mouse/paste/Alt-prefix), `FocusRing` traversal + overlay trap, and
  the `Chord`/`Keymap` parser and dispatch tiers. Byte-identical on oracle, IR, emit-C++,
  and LLVM (`designs/complete/techdesign-03-events-input.md`). Found and worked around
  bug.md #40 (`Map<string, EnumType>` segfaults past one entry on emit-C++/LLVM) and #41
  (`Array<struct>` with an enum field goes stale after unrelated heap activity — `Chord`
  is a `class`, not a `struct`, because of it).
- **Sonar T04 basic components** — `sonar/src/components/{text,contentbar,input,button,
  checkbox,radio,progress,spinner}.lev`: `Text` (word-wrap/hard-break, shared
  measure/paint), `ContentBar` (tight-width drop order), `Input` (scalar-cursor/cell-scroll
  editing, mask, paste, validation), `Button`/`CheckBox`/`RadioGroup` (focus + validation),
  `ProgressBar`/`Spinner` (T01's `ILifecycleHost`/`__sonarRegisterTimer` pending-timer seam,
  not a direct T09 `App` dependency despite the design doc's header). Also declared C5's
  `ISingleLine`/`IMultiLine`/`IValidatable` interfaces, which no earlier track had needed.
  Byte-identical on oracle, IR, emit-C++, and LLVM (`designs/complete/techdesign-04-components-basic.md`).
  Found and worked around four new bugs (bug.md #50–#53): a char literal not retyping in
  call-argument position; a nullable function type (`((T)=>R)?`) failing to parse; calling
  the result of an array-indexing expression directly failing to lower on IR; and — the
  significant one — a lambda calling an instance method with a bare (implicit-`this`)
  receiver silently losing the call on IR and **segfaulting on native/LLVM** once stored
  and invoked from a different call frame (fixed by spelling every such receiver
  explicitly, `this.method()`, in every constructor-registered handler in this track).
  Also fixed T01's `TimerReg`/`ShortcutReg` structs (`component.lev`) to use explicit
  constructors instead of positional auto-construction, which bug.md #38 documents as
  silently dropping a closure-typed field — undiscovered until this track's timer seam
  was actually exercised.
- **Sonar T06 templates** — the `sonar!` compile-time template layer:
  `sonar/src/templates/{expander,macro}.lev`. `expandSonar(string) -> string`
  parses the `.sonar` tag grammar and returns the exact construction source a
  user would hand-write (nodes/attrs/`id`/`on:`/text+holes/`$for`/`$if`, the C11
  + T04 attribute registry, error catalog E1–E9); the one-line
  `macro sonar(string) comptime` (F4) splices it via `meta::parseExpr`. The
  whole engine is a pure string→string library, so its M0 goldens run it at
  runtime exactly as the macro runs it at comptime; a post-F4 round-trip builds
  a live T04 component tree (`id`/`on:`/`$for`/`$if` all exercised) and an
  external `.sonar` via `import()`+`assets`. Two forced deviations, both
  filed: the expansion routes through a `Sonar::__sonarBuild` thunk rather than
  a bare `(() => {...})()` IIFE (bug.md #57 — imported symbols don't resolve
  inside an IIFE body), and every character test uses `.code()`/a two-char
  compare rather than a char literal (bug.md #58 — char literals don't retype
  at comptime). T05 composite tags pass through as unknown tags until T05 lands
  (`designs/complete/techdesign-06-templates.md`).
- **Sonar T08 theming & DI** — `sonar/src/{theme,toml}.lev` + `sonar/themes/*.toml`:
  a `Theme : ITheme` bound via landed `bind`/`inject`, the full most-specific-first
  `Styleable.resolve` fold (instance override → exact → key-minus-state →
  component base → `"default"` → `Style()`, each layered via `Style.over`), a
  dependency-free TOML subset parser (dotted headers, `fg`/`bg`/`attrs`,
  `#` comments, mandatory `line:col` error taxonomy), four built-in themes as
  comptime-`import()`ed TOML assets (Default/Dark/Light/HighContrast) with an
  a11y rule (every state key carries a non-color `attrs` channel), and in-place
  runtime theme switching. Acceptance corpus byte-identical on oracle/IR/emit-C++/
  LLVM (`designs/complete/techdesign-08-theming-di.md`). **Two forced deviations,
  both filed:** theme storage is a `Map<string,int>` index into parallel
  `Array<int>` carrier columns, *not* the design's `Map<string, Style>` — dodging
  **both** bug #49 (`Map<K,Struct>` class field corrupts on LLVM at 3+ entries)
  and bug #41 (`Array<struct>` with an enum field goes stale; `Style` has `Color`
  fields), with debt rows added to `docs/footguns.md`; and built-in assets load
  via a top-level `comptime string = import(...)` global rather than inline
  `loadToml(import(...))`, since `import()` only folds in a comptime context
  (a runtime ctor call throws — LA-20). The T04 injected-ctor retrofit and the M4
  tracing-theme drift test are out of T08 (the latter gated on the App paint loop).
- **Sonar T09 app / run loop / terminal** — `sonar/src/{app,runloop,terminal,ansi_renderer,
  cursor,log}.lev`: `App : Container, ILifecycleHost` (the frozen C9 frame phases, damage-
  coalesced scheduler, decoder + ESC-timeout wiring, T03 key/mouse/paste dispatch, Tab focus,
  `^C`/external-INT quit, `every`/`cancelEvery` timers, R13 overlay stack, signal/poll resize,
  `pumpOnce`, the R3 `currentApp`/`app()` globals, unconditional `using`-guaranteed teardown);
  `AnsiRenderer : IRenderer` (R8: full-repaint + mismatch/blit row diff with run coalescing, the
  SGR minimizer, one `sysWrite` per frame); `TerminalSession`, `StdinSource`, the escape
  vocabulary, and `FrameStats`. Run lanes oracle/IR/**LLVM** are byte-identical for the loop,
  input/focus/keymap, timers, teardown, and the `AnsiRenderer` emitter over a directly-built
  Surface; the full frame pipeline over *drawing* components is oracle/IR-only. emit-C++ compiles
  the non-loop widget surface; `App.run()` is unavailable there (§10 lane matrix). Design in
  `designs/complete/techdesign-09-runloop-terminal.md`. **Found two LLVM backend bugs (bug.md
  #67/#68):** #67 [P0] — a component that *draws* (`Surface.fill`/`put`/`writeText`) segfaults on
  LLVM when its `paint()` is reached via interface dispatch (`Container.paint`→child); a nested
  `Size.w` read miscompiles to a dynamic member fetch. No prior test painted a drawing component
  through the container hierarchy, so T09 is the first to hit it; it blocks only the LLVM
  component-paint step (loop/renderer/dispatch are all LLVM-clean). #68 [P2] — `env::get`
  (`sysEnv`) fails LLVM codegen as a non-inlined package call, so the dumb-render/no-color/
  log-stderr toggles are setters, not env reads.
- **Sonar T11 compile-time reactivity** — `sonar/src/reactive.lev` +
  `component.lev`/`templates/expander.lev` additions: `@Sonar::Reactive` on a component
  field makes a bare write (`count = count + 1`) update every widget bound to it in a
  `sonar!` template — no runtime observer graph, the update path is static code. A Layer-B
  rule injects a **`set`-view** over the field (`$f = v` is raw-slot access, §6, so the
  write routes through it and pings the host); the T06 emitter detects a `{this.<field>}`
  hole and emits a `__sonarBind` registration. **Landed in full on oracle/IR/emit-C++/LLVM**
  (`designs/complete/techdesign-11-reactivity.md`). **Two forced deviations** (both design
  §2 assumptions the landed rule engine doesn't meet — reference.md §rules, the filed
  LA-15/LA-16 asks, no compiler change): (1) **no field-type hole** → one `set`-view rule
  per scalar type gated `where f.type == "int"|"string"|"bool"|"float"`, with a loud guard
  (the `@Serializable`/`@Injectable` sentinel idiom) rejecting any other type; (2) **no
  field-name-string reifier** for a match-bound field subject (`"$f"` stays literal,
  `$f.name`→`<field>.name`) → the view calls a nullary `__sonarNotify()` and the registry
  does **global fan-out** (every reactive write re-runs every binding on the host; a single
  re-entrancy bool settles a chain A→B→A in one pass — §3's guarantee, observationally
  identical to per-field notify for every golden). §5.1's own fallback A is superseded (it
  also needs the two absent capabilities); the set-view + global-fan-out route is the
  achievable faithful path and preserves bare-assignment ergonomics. Registry stays
  string-keyed for `--expand` legibility. **Found bug.md #69 [P2]** (pre-existing, not T11):
  `--expand` prints desugared enum member constants as `EnumName$Member`, whose `$` the
  lexer rejects on re-parse, so a whole-program `--expand`→recompile round-trip is impossible
  for any enum-using program (Sonar pulls in T03's enums transitively); reactive-expansion
  faithfulness is instead proven by four-engine byte-identical runs.
- **System-interface binds (`designs/complete/techdesign-system-binds.md`), landed in full
  2026-07-14** — the capability-injection layer over the existing `bind`/`inject` DI core
  (§12.5): five root-bound capability interfaces (`IEnv`/`IConsole`/`IClock`/`IFileSystem`/
  `INet`, `namespace std`, thin shims over the ambient globals) plus **Channel 1** — `use
  NS::T;` activates T's namespace-exported bind, welding the name-import and value-by-type DI
  systems at one point (§12.5). No new backend work: activation is a Checker-only, check-time
  scope-table fact; the existing `bind`-inlining/reachability machinery needed zero changes.
  Channel 2 (`Bindings` builder objects) stays deliberately deferred. **Found and fixed bug #70
  [was P1]** (`known_bugs_1.md`, pre-existing, not a system-binds defect): a local/parameter
  named identically to an in-scope namespace (`env`, `math`, ...), dynamically dispatched and
  calling a method that namespace also declares with the same arity, silently resolved to the
  NAMESPACE function on IR/native instead of the receiver. Fixed 2026-07-14 in `Lower.cpp`'s
  `lowerCall` NS::fn fallback (require the base name not resolve to a local/parameter before
  taking the namespace path, the same precedent as the existing `console` shadow guard); the
  `sysEnv`-not-`env` naming workaround is retired, and `tests/corpus/
  namespace_shadow_dispatch.lev` is the regression corpus.
- **Sonar T05 composite/data components — COMPLETE** (M5/M6 landed 2026-07-14 now that T09
  exists): `OverlayHost`/`Modal`/`Sonar::alert`/`Sonar::confirm` (`components/modal.lev`),
  `BarMenu`/`Menu`/`MenuItem`/`MenuSeparator` (`components/menu.lev`), and `DebugOverlay`
  (`components/debugoverlay.lev`) join the M1–M4 set. They ride T09's overlay stack, extended
  in place (additive, default-param, flagged in the anchor log) with the R13 surface T05
  always needed: `pushOverlay(c, dismissOnOutsidePress, inputTransparent, group)`,
  `popOverlayGroup`/`popOverlayComponent`, `topInputOverlay`, `newOverlayGroup`, focus
  save/restore, and `__sonarDetachTree` (R7 recursive teardown — a 1000× open/close soak
  proves zero leaks). `confirm` returns a `Promise<bool>` the buttons resolve (the async
  dialog showcase). Byte-identical on oracle/IR/**LLVM** (paint bodies take no `pushClip`, so
  direct paint is #64-clean). Design in `designs/complete/`. Deepened bug #65: **every
  `Container` override of a `Component` method** (`__sonarChildren`, `contentDesired`,
  `arrange`, `paint`, `__sonarContentRect`) misresolves for a multi-mixin leaf — so tree-walks
  see no children — each `Container` leaf redeclares the set via the working `children()`
  accessor. (This whole #65 family — plus #64/#59/#61/#41/#40/#49/#36 — was **fixed at the
  source on `origin/master` the same day**, `054e159` et al.; the package workarounds are now
  redundant but retained, re-verified byte-identical on the rebuilt compiler. Confirmed later
  the same day: this also retired the one remaining Sonar LLVM gap — `ContentBox`/`GridBox`/
  `SplitBox`/`Tabs` (M1–M4) now pass on LLVM too, via `904bcbd`'s `#41` runtime fix; see
  `designs/complete/techdesign-05-components-composite.md`'s 2026-07-14 log entries.)
- **Sonar T07 attribute rules — COMPLETE** (landed 2026-07-14): `@Sonar::Shortcut`/`@Timer`/
  `@Validator` attributes + their three Layer-B rules + `validateTree`/`ValidationFailure`
  (`sonar/src/attributes.lev`), on the landed metaprogramming substrate and T04's already-
  landed Component pending/flush plumbing (only `@Validator`'s `__sonarAddValidator` was new).
  A method's `@Shortcut`/`@Timer` registration is injected at the bottom of its class ctor,
  held pending, and flushed into the keymap/timer host at attach (unbound at detach) — the
  R7 lifecycle scoping. Byte-identical on oracle/IR/LLVM; `--rules` confirms every firing.
  Design in `designs/complete/`.
- **Sonar T10 testing/examples/delivery — LANDED IN FULL** (M1–M6, 2026-07-14):
  `sonar/src/test_renderer.lev` (`TestRenderer : IRenderer`, shipped/DI-selected, the frozen
  two-channel `snapshot()`/`textOnly()` format); `sonar/tests/harness/` (`ScriptedInput : IInputSource`
  one-chunk-per-pump + a chord encoder that reverses keymap/decoder tables — encode→decode
  round-trips; the `SonarTest` vocabulary `eq`/`snap`/`snapText`/`harness`/`pump`/`reset`, whose
  `reset()` stopSession()s first so a never-`run()` scripted test doesn't hang on a live poll timer);
  `sonar/tests/runtests.sh` (the differential engine matrix — oracle+IR+LLVM vs one `.expected`,
  emit-C++ compile-only, LLVM stdout compared alone since its `[heap]` meter is stderr) + `regen.sh`;
  five `sonar/examples/` apps (hello/form-wizard/file-manager/log-viewer/dashboard), each runnable via
  `trident run` and, under `SONAR_SCRIPT=1`, also a byte-identical differential test; `README.md` +
  `CHANGELOG.md` (0.1.0). **Found+filed `designs/sonar/sonar-bugs.md #4`** (Sonar framework, NOT
  fixed per the T10-doesn't-patch-`src/**` rule): a leaf's in-place update to a SHORTER string leaves
  stale glyphs under the default/empty theme (`paintBackground` fills only for a themed non-default
  bg) — worked around with fixed-width fields in file-manager, footguns debt row added. Shipping
  `Sonar::TestRenderer` collided with the local `TestRenderer` that pre-existing `frame`/`loop`/`runloop`
  tests hand-rolled (under `uses Sonar` the import wins); renamed those to `RecordRenderer` (goldens
  unchanged). Design in `designs/complete/`.
- **Stream unsubscribe / dispose (SU-1) — LANDED 2026-07-15** (§13): the stream substrate gains
  deterministic dispose — `InStream<T> : IDisposable` (a `using`-releasable subscription, with an
  optional producer-attached teardown closure), `StreamBuffer.close()` (push silent-drops, pull
  throws the distinct `"stream is closed"`), and the `signal::off` free function whose last-out path
  `sysUnwatch`+`sysSignalClose`s the fd (signal back to default disposition) so a one-shot signal
  program now **exits by loop drain** instead of the watch pinning the loop. Prelude-only, no native/
  IR/ABI change; oracle/IR/LLVM full, emit-C++ for the in-memory surface. Landing it surfaced and
  fixed a latent **emit-C++ reachability defect**: a decl-less by-name method `CallDyn` (the prelude
  is never checked, so `buf.close()` inside `InStream.close` dispatches by name) over-marked *every*
  class's same-named method — provably-dead code for a non-instantiated reference class, but a hard
  compile error once it dragged in `TaskGroup::close`→`sysTaskCancel` (loop-bound, unlowerable on
  emit-C++). `CGen`'s by-name marking now skips non-instantiated **reference** classes (value types
  always kept — they reach `callm` via `byNameClasses_`), with a fixpoint as instantiation is
  discovered. The M4 thread-boundary gap (a disposable `InStream` crossing `spawn`, briefly
  filed as #81) was then **fixed** with a per-object `hasDispose` gate in both flatten walks
  (`lvThreadCopy`/`lv_flatten`) — a disposable stream rejects naming the type, a plain
  in-memory one still crosses; pins in `tests/corpus/tasks/spawn_{disposable,plain}_stream_*`.
  Design + `techdesign-terminal-floor.md` §8-q1 (closed) in `designs/complete/`.
- **`InStream<T> : IIterable<T>` (D-B) — LANDED 2026-07-19** (§13,
  `techdesign-http-and-streams-maturity.md`): the stream substrate's last v1
  gap closes — `for (T x in stream)` now works directly, and `stream.asSeq()`
  joins the same lazy `Seq<T>` pipeline arrays get. `hasNext()` genuinely
  **parks** on an internal waiter `Promise<bool>` that the next `push`/`close`
  resolves — the same suspension surface `await` uses (no new native, no new
  IR op), so it needs true suspension and is oracle/IR/LLVM only. Also lands
  `pullOrNone()` (the honest non-blocking pull SU-1's hand-off deferred) and
  fixes `pull`/`pullOrNone`/iteration to drain whatever is already buffered
  before recognizing end-of-stream (SU-1's `pull()` itself is UNCHANGED —
  closed still wins unconditionally there, pinned by `unsub_inmem.lev` — the
  drain rule is new surface, not a retcon of that contract). Prelude-only, no
  native/IR/ABI change, **except** one small `CGen.cpp` reachability fix:
  emit-C++'s by-name method index over-marked `InStream`/`StreamSeq`'s
  `iterator()` (and, transitively, `StreamIterator.hasNext()`'s `await`,
  which this backend cannot lower) reachable in ANY program that merely
  constructs a stream, even one that never iterates — the SU-1
  `TaskGroup::close`/`sysTaskCancel` lesson recurring on a different
  reachability path (the eager "every instantiated class pulls in all its
  members" walk, not the by-name-dispatch one SU-1 gated); fixed by excluding
  the stream-iteration-protocol method names from both marking paths, relying
  on the by-name/instantiation gate SU-1 already built for everything that
  genuinely needs it. D-A (HTTP client pooling) and D-C's in-language DNS
  resolver remain deferred — neither trigger has fired (`techdesign-http-and-
  streams-maturity.md` §0/§4). **Found+filed `known_bugs_2.md` #90** (P2, NOT
  fixed — pre-existing, unrelated, out of scope): LLVM leaks ~128B/iteration
  when a class-field `Array<T>` is mutated via `.add()`/`.skip()` across two
  separate method calls (e.g. `StreamBuffer.push()` then `.pull()`); bisected
  against the pre-D-B tree and reduced to a repro with no stream involvement
  at all. `fuzz/task_churn/park_inside_callback.lev` is XFAIL-LLVM against it.


**Backend status — read this before citing a backend.** The **pure x86-64 / ELF backend
(`--emit-elf`, `src/X64Gen.cpp`) is FROZEN and reference-only** as of the portable-backend
pivot (2026-07-05). **ELF is not a project target.** It is kept solely as the
differential-testing anchor and the eventual zero-dependency bootstrap seed; it is **never
extended**, no new feature is required to reach it, and **no design or task is ever gated on
an ELF finding**. Where §§13–17 below describe memory/COW/socket work "on the pure ELF
backend," that is finished pre-freeze work left in place, not an active target. The
**primary AOT backend is LLVM**; the active engines are the tree-walk oracle, the IR
interpreter, emit-C++, and LLVM.

---

## 1. Core philosophy in one page

- **One rule over many special cases.** A constructor is not a special shape; it is a method
  marked by a keyword. An operator is not special syntax; it is a method whose name is a
  symbol. A property view is not a new construct; it is a typed layer over a slot.
- **Explicit over implicit**, but with inference where the types are recoverable from context.
- **Honesty over hidden magic.** A type annotation must not secretly mean something else. This
  killed implicit-promise-wrapping and drove several other decisions.
- **Resolution is by type, everywhere.** Overloads, field access, operators, and assignment
  targets all resolve on type. A bare read with no type in context is the one thing that
  cannot resolve, and is therefore an error rather than a guess.
- Regular-expression matching is designed as a byte-oriented, linear-time data operator:
  Thompson NFA/lazy DFA only, with no backtracking, backreferences, or lookaround.
-  A member is a member. Fields/attributes and methods are not two kinds of thing — a member is a typed slot bound to a label. Some of those types are executable (methods) and some are not (fields/properties). There is no field-vs-method dichotomy; "method" just means "the member's type is callable." Everything about members — access, inheritance, distinct collision, interface requirements, get/set views — follows from this one rule.
- **Gate the dangerous, guarantee the safe.** Raw memory, interception, and anything that can
  void a guarantee lives behind an explicit marker so the safe majority keeps its guarantee.

Mutation control is one such general rule, not three special cases: three orthogonal axes,
each already principled, with `const`/`readonly` (reference §4.3b/§4.3c, techdesign-readonly
LA-28) completing the set.

| axis | question | mechanism |
|---|---|---|
| **slot** | when may the binding be written? | `const` / `readonly` |
| **value** | does the value alias or copy? | `struct` / pure `Array`/`Map` (§9, §11) |
| **view** | which access views are exposed? | `get`-only accessors (§6) |

The **slot** axis itself has three points, distinguished by *when the fixed value becomes
known*: `var` (never fixed) — `const` (fixed at **compile time**: a local/global/param/for-in's
initializer may be any runtime expression, but a **field's** `const` initializer must be a
compile-time constant — a named constant, reference §4.3b) — `readonly` (fixed at
**construction time**: instance fields only, written by the initializer or any declaring-class
constructor, exactly once, reference §4.3c). `readonly` is field-only because "construction
time" is a lifecycle only instance fields have; everywhere else `const` already covers the
one write-once slot a binding can have.

There is deliberately no fourth axis (no type-qualifier const): `const`/`readonly` scope a
slot's write view to its window and are never part of the type.
---

## 2. Classes, access, constructors, methods

Access modifiers may be applied **inline** (per member) or **sectionally** (a `public:` /
`private:` region that applies until the next one).

A **constructor is marked by the `new` keyword.** The constructor's *name* is only a label —
it carries no meaning beyond selection. No `new` means it is not a constructor, regardless of
what the method is named.

```
class MyClass {

    // Inline access modifiers
    public string myString;
    public distinct int myNumber   = 10;
    public distinct int myVariable = 15;

    // Sectional access modifier
    public:
        string myString2 = "Test String";

        // Constructor: marked by 'new'. Name is a label only.
        new MyClass() {
            myString = "Good Bye";
        }

        // Overload: same marker, different parameters.
        new MyClass(string value) {
        }

        // The label need not match the class name.
        new DifferentName(string value) {
        }

        void processString(string str) {
        }

    private:
        // A method body is the next statement in method scope. Usually a block { },
        // but it does not have to be.
        int PrivateMethod(int a, int b) return a + b;

        // '=>' is sugar that shortens 'return' to an arrow.
        int PrivateMethod(int a, int b) => a + b;
}
```

### Construction and selection

Construction has **no `new` at the call site.** The call runs the matching constructor and
returns the instance. Bare declaration auto-constructs (see §3). When multiple constructors
match the same parameter shape, **first-declared wins**, and the label is the explicit
tiebreaker.

```
MyClass a = MyClass();                       // matches the nullary constructor
MyClass b = MyClass::DifferentName("x");     // explicit selection by label
```

Calls may bind parameters explicitly by name (`f(port: 8080, host: "localhost")`), and
parameters may declare compile-time-constant defaults (`int port = 80`). Positional arguments
must come first. Omitted parameters are filled by their declared defaults before ambient
`bind` injection is considered; the checker then normalizes the call to a full positional list.

The label is decorative *until* two constructors collide on parameters; then it is the only
way to reach the shadowed one. This is the same shape as field qualification — an explicit
selector needed only when the default resolution does not pick the intended target.

---

## 3. Construction, defaults, and bare declaration

Declaring a variable **auto-constructs** it — there is no null/unbound state on declaration.
This is the same decision applied uniformly to every type: `string s;` yields `""`,
`MyClass m;` yields a default-constructed instance.

```
MyClass myClass;                 // auto-constructed, usable immediately
myClass.str = "hello";           // no explicit constructor call needed
```

Because bare declaration auto-constructs, **every type has a nullary construction path** —
either an explicit zero-arg constructor or the implicit field-default one. The interaction
with generics (what is the default of an arbitrary `T`?) is an open constraint question:
bare-constructing `MyClass<T, U>` requires `T` and `U` to themselves be default-constructable.

---

4. Multiple inheritance, collision detection, and distinctThis is a defining feature of the language. Multiple inheritance is a first-class capability, and the machinery around it — how collisions are detected, how distinct resolves them, and how constructors and members are selected — is one of the things that most distinguishes the design.A class may inherit from several bases. Bases, their constructors, and their members are all reached with :: — the same qualifier used for constructor-label selection (§2) and namespaces (§12). One qualifier, three uses: reach into a base, reach a labeled constructor, reach into a namespace.The diamond and the collision questionWhen a class inherits two bases that carry a same-named, same-typed member, the language must decide: one shared slot, or two separate slots? Rather than picking a fixed answer (C++ forces the choice through virtual inheritance; most languages forbid the situation), the language makes it an explicit, per-member decision through distinct.

Default — collapse. Same-named, same-typed inherited members merge into one slot. The surviving value follows the collapse rule (the later/overriding base's value).

distinct — keep separate. A member marked distinct stays as separate, per-source slots, reachable only by :: qualification. A bare, unqualified read of such a member has no type signal to choose between the slots and is therefore invalid (the same rule as any type-ambiguous read).
distinct replaced an earlier misuse of virtual. virtual everywhere else connotes dynamic dispatch, which this feature does not involve — it is entirely static. distinct names the actual behavior: keep the slots distinct. It is one degree of distinct on either inheriting side that keeps the pair separate.Collision detection is resolution-by-typeCollisions are detected and resolved by the same rule that governs all member access: resolution is by type.

Different-typed same-named members never collide — the type at the use site selects. myVariable = 5 and myVariable = "Hello" resolve to the int slot and the string slot respectively, with no ambiguity and no qualification needed.
Same-typed members are the only real collision, because type cannot disambiguate them. These are exactly the members that need distinct to coexist, and are then reachable only by :: qualification. A bare read of a same-typed collided member is invalid — there is no type in context to choose, so the language refuses to guess rather than picking silently.

This is the whole detection rule: a collision exists precisely when two members share a name and a type. Name alone is not a collision (types disambiguate); type alone is not (names differ). The distinct/collapse decision only ever applies to the name-and-type overlap.

class Counter {
    public string label;
    public distinct int value = 0;     // distinct on this side
}

class Tag {
    public int    value = 99;          // same name + type as Counter.value
    public string note  = "tag-note";
}

class Widget<T> : Counter, Tag {

    public T payload;

    new Widget() {
        Counter::Counter();            // a 'new' call applies to "this"
        Tag::Tag();

        // 'value' is int on both sides -> a real collision.
        // 'distinct' keeps the slots separate:
        //   this.Counter::value == 0
        //   this.Tag::value     == 99
        this.Counter::value = 5;       // qualified — required
        this.Tag::value     = 7;

        // 'note' exists only on Tag -> no collision, bare access is fine.
        note = "widget";

        // console.write(value)        // invalid: two int 'value' slots, no type context
    }

    // A second constructor, selected by its label.
    new Blank() {
        Counter::Counter();
        Tag::Tag();
    }
}Constructor selection under inheritanceBecause a constructor is marked by new and named by a label (§2), inheritance composes cleanly:
A derived constructor invokes each base's constructor explicitly by qualification — Counter::Counter(), Tag::Tag() — and each applies to this (the mutate-this construction model). The derived class controls the order.
Base constructors are selected the same way any constructor is: by parameter match, first-declared winning ties, with the label as the explicit tiebreaker. Base::SomeLabel(args) reaches a specific base constructor by label exactly as Type::Label(args) does at the top level.
This is the same selection mechanism everywhere — there is no special "base constructor call" syntax; it is ordinary constructor selection with a :: qualifier naming which base.
Shared bases and the collapse choice in practiceWhen two bases themselves derive from a common base, the same rule applies to the shared member: collapse to one by default, or keep distinct per path. Some designs want the collapse — a bidirectional stream (IOStream : InStream, OutStream, §13) must have both ends on one shared StreamBuffer, so that member collapses (not distinct); two separate buffers would break it. Other designs want the separation — two independently-tracked counters that happen to share a name. The point is that the language does not pick for you: collapse is the default, distinct is the opt-out, and which is correct is a per-member fact the author states.

---

## 5. Operators as methods

An operator is **a member whose selector is a symbol.** No `operator` keyword, no privileged
operator set — `(+)`, `(-)`, `(==)` are ordinary members with symbolic selectors, and the return
type sits in the normal slot. The parentheses are **delimiters marking a symbolic selector**,
not part of the operation — the same disambiguation as `(+)` vs infix `+`. The
comparison-vs-arithmetic distinction falls out of the return type (`bool` vs the type), with no
special-casing. An object operator returns **its own type** — the value that lands in the
left-hand side of the enclosing assignment.

### The symbolic-selector family

The selector's shape decides the use-site syntax; this is one family, not separate features:

| selector | use site |
|---|---|
| a name | `a.name` |
| `(+)`, `(==)`, … | infix `a + b` |
| `([])` | indexing `a[i]` (§6.7) |
| `(())` (future) | call `a(...)` |

### Transfer operators and streams

`<<` and `>>` are **transfer operators**: *the arrowhead points at the destination (the operand
that is written); the tail is the source (read).* `writer << value` writes into the stream;
`reader >> target` fills `target`. The stream always owns the operator (left operand), so no
primitive ever carries a stream operator. `reader >> target` requires by-reference parameters —
which the language has: **objects are references**.

```
class MyClass3 : MyClass, MyClass2 {
    MyClass3 (+)(int val)  => myInt + val;
    MyClass3 (-)(int val)  => myInt - val;
    MyClass3 (*)(int val)  => myInt * val;
    MyClass3 (/)(int val)  => myInt / val;
    bool     (==)(int val) => myInt == val;
}
```

### Equality derivation

`(==)` must return `bool`. `(!=)` derives automatically as `!(==)` — defining `==` gives the
pair. Both may be overridden, but overriding both means the language no longer guarantees they
agree ("flying without a net"). The stance: **no paternalism toward the person who picks up the
weapon** — but this only applies cleanly where the failure is *loud and local*. Where a footgun's
damage surfaces far from its cause, the design prefers to at least make it announce itself.

### Custom / new operators

Overloading the **known** operator set is broadly valuable and cheap. Defining **new** operator
symbols is high-value only in narrow domains (data/relational notation people already read) and
a readability liability elsewhere (symbol soup, unknown precedence). The intended use is
data/relational operators — `in`, joins, set operations — where the notation is already in the
reader's vocabulary. The guard against misuse: **design the operation rigorously first, then
name it readably — never shape the operation to fit an English-looking sentence.** If a symbol's
meaning is uniform across every shape of data, the readable name is earned; if it only reads
right for the one sentence it was built around, that is a special case hiding behind prose.

---

## 6. Properties: get / set as views over a slot

`get` and `set` are **typed access layers mapped over the same address as the field that
allocates the storage.** The field declaration is what allocates; the accessors are read/write
views over that one slot. There is no separate backing store and no self-recursion, because the
accessor is not a function that re-enters — it is an overlay on the slot's address.

Consequences that fall out for free:
- **Read-only / write-only** are just "which views exist" — declare only `get` → read-only; only
  `set` → write-only. No special rule.
- Because a field access may now run code, **field access is no longer guaranteed inert** — this
  is the accepted cost of erasing the field/method distinction (a member is a typed, possibly
  executable slot).

A `get`/`set` with **no backing field** is discarded (nothing to view) — or, in strict mode,
raises. **This discard rule applies only to parameterless accessors** (views of a slot); see
§6.7 for parameterized accessors, which are computed and need no backing slot.

Inside an accessor body, access to the owning slot is **raw-slot access** (not view-dispatched):
the accessor is a rule attached to the slot, one level below the surface it presents — the same
way the `i` in `i = i + 1` names the current value, not a re-entry. This is what makes accessors
non-recursive by construction.

the accepted cost of the one-member rule (§1, §6.5): a member is a typed slot, and some slot types are executable.

6.5 Members are typed slots; some are executable
There is one member concept, not two. A member is a label bound to a typed slot. A field is a slot holding a value; a method is a slot holding a value of a callable type; a property is a slot with get/set views layered over it. The only axis that distinguishes them is whether the member's type is executable.
This is why the same machinery governs all of them uniformly: overload/override resolution, distinct collision, :: qualification, interface requirements, and the shape/offset layout apply to every member the same way, because there is nothing member-specific to special-case — there is only "a typed slot at a label." An interface requiring a field and an interface requiring a method are the same kind of requirement; a class satisfying either does so by declaring the slot.

### 6.6 Methods vs functions; the static side

A **function** is a callable member with no instance receiver (no `this`); a **method** is a
callable member with `this`. Both are the same kind of member — the only difference is whether
an instance exists to bind `this` to, which follows from one question: *is the containing
entity instantiated?*

- A **namespace** is a singleton (never instantiated) → its callables are **functions**.
- A **class instance** side (reached by `.`) → **methods**, per-instance fields.
- A **class static side** (reached by `::`) is a singleton, namespace-like scope → its
  callables are **functions** ("static method" is not a method), and its data is
  concretely-typed only.

The corollary that replaces C#/Java's ad-hoc rule: a singleton scope has no instantiation, so it
**cannot hold `T`-typed state** — neither a namespace nor a class's static side may declare a
slot typed by a generic parameter. Signature-only generics are fine anywhere (bound per call);
parameterized **state** needs an instantiation site (a class instance). A generic namespace
(`namespace<T>`) is therefore legal with `T` in member signatures (each member is independently
generic, inferred per call) but not with `T`-typed namespace variables — allowing the latter
per-call would be runtime-retyped storage, i.e. `any`, which is rejected.

### 6.7 Indexers: parameterized accessors with the `([])` selector

`get`/`set` accept a symbolic selector, so an indexer is just *an accessor whose selector is
`[]`*:

```
class Grid {
    Array<int> cells;
    get ([])(int i) => cells[i];
    set ([])(int i, int v) cells[i] = v;    // value parameter last
}
g[1] = 7;      // dispatches to set ([])
int x = g[1];  // dispatches to get ([])
```

A **parameterless** accessor is a *view over a slot* (needs a backing field, §6). A
**parameterized** accessor is a *computed accessor* — no backing slot required, the discard
rule does not apply. Read-only/write-only, multi-dimensional (`get ([])(int r, int c)`), and
overload-by-key-type (`a[5]` vs `a["k"]`) all fall out of the existing rules.

Assignment through an indexer follows one rule: `a[i] = v` ≡ *"`a` becomes `a` with `[i]` set
to `v`."* On a mutable object the set-view mutates in place (rebind is a no-op); on a pure
value (arrays, §11) it produces a new value and rebinds. One rule, both behaviors.

### 6.8 Method references — a callable slot read as a value (LA-25, landed)

The one-member rule taken to its conclusion: a `::`-reached **callable** member *not
immediately called* is a first-class function value, the same way a `::`-reached field read
yields the field's value (a member is a typed slot; some slot types are executable). A
namespace function reference (`NS::fn`) is its signature directly; an **instance** method
reference (`Controller::Login`) is **unbound** — the receiver becomes the first parameter
(`(Controller, Req) => Resp`); a labeled constructor (`User::FromName`) is `(string) => User`.
The reference's type spells as an ordinary function type, so it is assignable to fields,
parameters, and containers (`Array<(A,B)=>R>`), and **resolves against the target function
type** (overloaded-with-no-target is a compile error). **Dispatch is the ordinary
receiver-dispatch rule** because the reference *is* its eta-expansion lambda
`(C c, ...) => c.m(...)`. v1 limits: no reference to a **generic** callable (type params
unbound in value position); each evaluation is a **fresh** value (not an identity token); a
**bound** reference is not in the language (use a lambda). Full rules: reference.md §3.4.

### 6.9 Method dispatch: runtime slot, statically devirtualized (landed)

An unqualified instance-method call dispatches on the receiver's **runtime** class —
uniformly for interface-typed *and* class-typed receivers (a `Dog : Animal` overriding
`speak()` runs `Dog::speak()` through any `Animal`-typed binding). This is resolution-by-type
applied to the call target. The compiler **devirtualizes to a direct call** whenever the
candidate set is provably closed (nothing in the whole-program gather overrides the resolved
method below the receiver's static type) — a whole-program optimization that changes only
*how fast* the call runs, never *what* runs. Qualified access (`this.Base::m()`), operators,
constructor selection, and static/namespace functions stay statically resolved.

**Known limit — overridden overload sets sharing an arity.** Runtime dispatch is by
**name + arity** (no type disambiguation). An overridden method sharing its `(name, arity)`
with another overload on the same receiver static type cannot be picked correctly at runtime
and is a **compile error** at the call site (give distinct arities/names, or qualify).
Different-arity overridden sets — the common case — dispatch correctly. Signature-aware
runtime dispatch is roadmapped (reference.md §3.4a).

---

## 7. Memory model for members: shapes and slots

Objects are laid out as a **shape (hidden class) + a packed array of typed slots.** The
name→offset table lives in the shared *shape*, not in each object, so:

- **Declared property access compiles to a fixed offset** — a direct load, as fast as a struct
  field. This is the "point right at it" fast path.
- **Runtime-added properties** get real typed slots (tagged by name), reached via a shape lookup
  that is **cached at the call site** (inline cache) — slow only on the cold path, near-direct
  once warm.
- Adding a property transitions the object to a new shape; objects that grow the same way share
  the new shape, amortizing the cost.

This is why `any` is rejected (see §9) and why dynamic properties do **not** require a catch-all:
per-name typed slots cover them with real memory layout.

### First-wins and direct pointers

When resolution is decidable at build time (**first-wins**, closed candidate set), the compiler
points directly at the slot. When the candidate set is open at runtime, access goes through a
cached pointer/lookup. The size of the "otherwise" (indirect) bucket is set by whether dynamic
properties may **shadow** declared ones:

- **Last-resort dynamism (chosen):** declared slots always win; dynamic names fill only genuine
  gaps. The fast path survives even on dynamic-capable objects. Shadowing is **not** worth the
  fast-path forfeit for this language.

### Interception (the catch-all)

A true open-ended interceptor — for names with **no slot at all** (proxy / ORM-reflection cases)
— is the one thing per-name slots cannot do. It is available only behind an **explicit compiler
directive**, marked on the class, because the mere possibility of interception defeats static
resolution for that object. The directive partitions objects into statically-shaped-fast and
interceptable-slow, and the partition is visible.

---

## 8. Interfaces

An **interface is a contract that allocates nothing.** It declares required members (fields
included — an explicit, deliberate exception, per the one-member rule (§6.5): the field requirement and a method requirement are the same kind of requirement. The implementing class's declaration is the **first (and only)
instance** — the thing that actually allocates.

Because interfaces allocate nothing, two interfaces requiring the same field create **no
collision** — the class satisfies both requirements with its single declared slot. The
field-collision machinery never engages, because there was only ever one declaring site.

Method requirements compare names and parameter types exactly. Their return is covariant:
an implementation may return the required declared class/interface type or a subtype of it;
it may also return `T` where the requirement returns `T?`. This relaxation is confined to
interface satisfaction and to declared-type returns—parameters remain invariant, richer
union/function/primitive returns remain exact, and a derived class method with a narrower
return does not thereby override a base-class method. The interface requirement is consumed
as a contract and contributes no second slot, so runtime dispatch still uses the one concrete
implementation.

```
public interface MyInterface {
    int myNumber;
    string myString;
}

class MyInterfacedClass : MyInterface {
    int myNumber;    // the declaration that allocates
    string myString;
}
```

---

## 9. Types

### Primitives and naming

Consistent naming is preferred over historical abbreviation. The open question (`bool` vs
`boolean`, `int` vs `integer`) reduces to one rule: **either all primitives are full words or
all are abbreviated — the middle (some shortened, some not, by no rule) is the anti-pattern.**
The design's trajectory (full-and-regular over short-and-special) points toward full words.

### The object mask: primitives carry methods, unboxed

Primitives are **value types that carry a method shape** — no boxed `Int`/`String` wrapper, no
value-vs-object dichotomy. `int`, `string`, `bool`, `float`, `char` are declared as value-type
classes; literals type through them; member access resolves through the same shape machinery as
any class; storage stays unboxed and operators stay built-in. Inside a primitive's method, `this`
**is** the raw value:

`char` (Track 03) is the object mask applied to a **single Unicode scalar** — unboxed, methods
(`code()`, `toString()`, ASCII classification/case) dispatch through the same machinery, `this`
is the scalar. It deliberately carries **no arithmetic** (use `code()`), so it never drags in
C's integer-promotion special cases; comparisons are by scalar value. Its one wrinkle is the
literal: single-quoted literals are strings by default and **re-type to `char` by expected
type** (the same resolution-by-type rule as everywhere else), rather than adding a second
literal syntax. Strings stay **byte-indexed by design** (the primary, O(1) index world);
`s.chars() -> Array<char>` is the explicit scalar door — a strict RFC 3629 decode (ill-formed
bytes → U+FFFD) that unblocks scalar iteration, scalar counting, and UTF-8-correct
`s.reverse()`.

```
(-7).abs()            // 7   — int method, this = the value
"Hello".length()      // 5
"Hello".toUpper()     // "HELLO"
(42).toString() + "!" // "42!"
```

Method bodies are either **native intrinsics** (declared with empty bodies in the prelude:
`length`, `subStr`, …) or **written in the language** (`abs`, `max`, `min`). The standard
library is language-defined over a minimal intrinsic core.

Internal word boundaries in identifiers get a capital (`subString`, not `substring`) — the rule
is mechanical (capitalize each stem after the first), which removes the unprincipled "is this
compound one word yet?" judgment.

### Value types: `struct` (the object mask, generalized)

The object mask is not special to primitives — it is the general rule for **value types**, and a
user declares one with `struct` instead of `class`:

```
struct Point {
    int x;
    int y;
    int dot() { return x * x + y * y; }
    mutating void translate(int dx, int dy) { x = x + dx; y = y + dy; }
}
```

A `struct` differs from a reference `class` on exactly the axes that make it a *value*:

- **Copied, not aliased.** Binding, passing, returning, or storing a struct copies it (deep — a
  struct field copies too; a reference-class field is shared). Two variables never observe each
  other's mutations. A `class` keeps reference identity.
- **No identity.** A struct is its fields; there is no object to be `==` by reference. Equality
  is **field-wise by default via a synthesized `(==)` method** (each field compared recursively —
  a struct field field-wise, a reference-class field by identity, a float field *canonically*),
  and a hand-written `(==)` overrides that. The synthesis is real, visible source: `--expand`
  prints the generated `bool (==)(T other) => ...`. A struct with a field that has no comparison
  is a **compile error** at the comparison site — never a silent `false`. This is the same
  comparison a struct uses as a `Map` key (§keys). A `class` with no `(==)`, by contrast, is
  reference identity.
- **`mutating` methods.** Because the receiver is a value, a method that writes `this` must be
  marked `mutating`; a plain method that tries to assign a field is a compile error. Constructors
  and `set` accessors are mutating by definition.
- **Flat.** A struct may implement interfaces but not inherit implementation, and nothing inherits
  from it — value types are final (so there is no slicing and no dense-array subtype ambiguity;
  mixed variants use a closed **union** instead).

This is what unlocks dense storage: an `Array<Point>` of a value type is a flat run of `Point`
records — no per-element boxing or pointer chase — and the same layout maps directly onto `mmap`
and columnar forms (the data-processing direction). Reference `class` is unchanged; reach for
`struct` when a type is data (a row, a coordinate, a small immutable-ish bundle) rather than an
entity with identity.

### Enums: a closed value type (Track 03, landed)

An `enum` is the value-type rule with a **closed** member set carried by `int` — not a new
kind of thing, but a `struct` (a single `int $code`) plus one const global per member and an
`Enum$fromCode` free function, synthesized in the resolver. Members live on the static side,
reached by `::` (`Method::GET`), with `code()` (carrier), `toString()` (member name), and
`fromCode(int) -> Enum?`. `==`/`!=`/`<` compare by carrier; a bare declaration yields the
first member. `match` over an enum is **exhaustive over the closed set** (the same
closed-vs-open exhaustiveness rule as unions, §12.65). Because it lowers to struct+int+globals
with no new ABI tag, it is **full-coverage on all four active engines including LLVM** (still
no ELF lane — X64Gen frozen). Syntax and carrier rules: reference.md §4.2c.

### `Block`: the gated fixed-length byte buffer (Track 03, landed)

`Block` is the language's **gated mutable byte buffer** — the §16 "bazooka in a marked room"
where the gate *is the type itself* (nothing implicit converts to or from it). Unlike the
pure/immutable `Array`/`Map`, it is a **reference type** (honestly mutable, shared by
reference like any `class`): a fixed length chosen at construction, zeroed, with
bounds-checked `byteAt`/`setByte`, little-endian `int32At`/`int64At` (read sign-extends),
`fromString`/`toString`, and an **aliasing** `slice` (writes through the view are visible in
the parent — the zero-copy point). Bulk `fill` and overlap-safe `blit` provide native
mutation; `equals` compares full-view byte content and `mismatch` finds the first differing
index, while the ordinary `==` operator remains reference identity. Every out-of-range
access throws (§12.6 loudness), and
`setByte` rejects a value outside `0..255` rather than masking. It is the byte substrate the
stream/`sys*` layer (§13) fills in place: `sysRead`/`sysWrite`/`sysRecv`/`sysSend` and
`File.read`/`write` have arity-distinct `Block` overloads. On oracle/IR/emit-C++/LLVM via the
`LV_BLOCK` ABI tag (no ELF lane — X64Gen frozen). Full surface: reference.md §6.10.

### `var` / `let` vs `any`

- **`var` / `let`** are **inference markers, not types** — the value has a fixed static type,
  only its spelling is inferred. Free, safe. Kept.
- **`any`** is a real "no static type" type: per-operation runtime type-check (an extra hop to
  "current type"), boxing, realloc on type change, lost safety — *and* it **spreads by
  assignment** into code that never opted in. Its one unique payoff (gradual migration) does not
  apply to a new language. **Rejected.** The typed replacement is a **union/`Variant`**.

### Unions vs `any`

A union `int | string` shares only the *tag* with `any`; on every other axis it differs because
it is **closed**:

| | Union `int \| string` | `any` |
|---|---|---|
| Storage realloc on type change | no (pre-sized to largest) | yes |
| Dispatch | bounded jump table, optimizable | open runtime lookup |
| Boxing | often none | usually boxed |
| Safety | forced at compile time | skipped, fails at runtime |
| Contagion | contained (narrowing forced) | spreads by assignment |

Union member size is **value size, not content size** — every heap-backed type (string, array)
has a small value size regardless of content, so unions over them are cheap.

### Optionality: `T?`, `None`, and no truthiness (locked)

There is **no null**. Absence is a value: `None`, a unit type that inhabits nothing but
itself. **`T?` is pure sugar for `T | None`** — optionality is the union rule applied, not a
parallel nullability system. Where null inhabits every reference type invisibly, `None`
exists only where a signature admits it, and the checker forces its elimination.

- **Defaults (§3):** an optional field auto-constructs to `None` — defaulting to `""`/`0`
  would fabricate data the caller never sent ("omitted" and "sent empty" are different
  facts). General unions without `None` default to their first member's default
  (first-declared-wins).
- **`None` never equals a present value**: `None == ""` is false — different tags (and
  decidable at compile time). Absent / present-empty / present-with-content are three
  distinguishable states; conflating any two is the disease `None` exists to cure.
- **No truthiness — not even presence-only.** `if (host)` / `if (!host)` do not test
  `None`-ness: `bool?` makes the negation ambiguous ("is None" vs "is false"), and
  JS-shaped code that behaves un-JS-ly is a silent-distant footgun. Conditions require
  `bool`.
- **The ergonomic package instead** (lands with union narrowing):
  `host != None` / `== None` — ordinary comparisons that **narrow** (Kotlin-style flow
  typing; `host : string` in the then-branch); `is` tests narrow likewise;
  **`?.`** optional chaining (`a?.m()` — `None` short-circuits); **`??`** coalescing
  (`a ?? d` — `d` when `None`). All three are sugar over narrowing; narrowing itself is
  the core rule — *resolution by type* — applied to control flow.

**Union narrowing is implemented**: flow typing through `if`/`else`, `while`, ternary arms,
and `&&` chains; member paths (`req.host`) narrow; assignments invalidate; union member
access without narrowing is a compile error, as is member access on a `None`-narrowed value.
`T?`, `None`, `??`, and `?.` all ship with it. (Remaining: re-typing `sysReadLine` to
`string?`, `Result` ergonomics on this substrate.)

### Generics — one rule for every entity

A type parameter is **a slot whose value is a type**, declared in an entity's own scope and
bound at instantiation-or-call time. Any scope-opening entity may carry type-param slots —
**class, free function, or method** — with the same declare/infer/substitute machinery for all:

```
class Box<T> { T item; }                    // class-level: T fixed per instance
R identity<R>(R x) => x;                    // function-level: R bound per call
class Array<T> {
    Array<Pair<T, U>> join<U>(Array<U> other, (T, U) => bool match) { ... }
}                                           // method-level: U bound per call, T from receiver
```

Type arguments are **inferred when recoverable** (from the target type, from constructor
arguments, from argument types — including through containers, `Array<U>` from `Array<Tag>`)
and **required when not**. Type positions use `Name<T, U>`; call positions use the distinct
call-only spelling `callee::<T, U>(args)`. The explicit tuple is exact-arity and authoritative:
it binds class parameters for construction, callable parameters for functions/methods, and
filters overloads before substituted value checking. Generics are **invariant**
(`Array<int>` is not `Array<string>`); the raw (unparameterized) form is compatible with any
instantiation.

```
MyClass<int, string> myClass = MyClass();   // inferred from the target type
Promise<int> p = Promise(n * n);            // T inferred from the constructor argument
string s = identity("hi");                  // R inferred from the call argument
var empty = Box::<int>();                    // T explicitly pinned at construction
string t = identity::<string>("hi");         // R explicitly pinned for this call
```

### Higher-kinded types (implemented, gated)

The rule taken one level up: a type parameter may itself be a **type constructor** (`F` of kind
`* -> *`), so `F<A>` applies a type variable — `F<B> map<F, A, B>(F<A> c, (A) => B fn)` types
"any F you can map over" while **preserving the container** (`Array`→`Array`, `Promise`→
`Promise`). Inference binds the constructor head by unification (`F<A>` vs `Array<int>` →
`F = Array, A = int`) and the head flows into the return type, so mis-assigning the result to a
different container (or different type argument) is a compile error. Bodies are duck-typed at
instantiation (the C++-template model). HKT is a **gated, advanced feature** (§16 pattern), not
the default idiom — methods and interface bounds cover the common cases. (Constraint bounds on
the constructor, `F: Mappable`, remain open.)

### Overload resolution — resolution by type, realized

All overload selection resolves on **argument types**: arity filter, applicability (argument
assignable to parameter; a parameter mentioning a type variable unifies), **most-specific wins**
(exact beats widening), **first-declared breaks ties** (§2). This applies uniformly to methods,
free/namespaced functions, operators (by right-operand type), and constructors (label + argument
types). The checker's choice is recorded and is what executes — static resolution, not runtime
re-dispatch. No applicable overload is a compile error.

For ordinary calls, applicability is evaluated after mapping positional and named arguments to
parameters and filling omitted parameters from constant defaults or lexical injection. Defaults
win over injection. Equal type scores prefer the candidate requiring fewer fills, then retain the
first-declared tie-break. Named/defaulted calls are compile-time normalization only and add no
runtime calling convention.

---

## 10. Strings

A string is a **handle**: a fixed-size value (pointer + length, with small-string optimization
as a transparent implementation detail) referring to heap-backed character data. The decision to
use a handle (rather than a pure-inline primitive) is what gives strings a **fixed value size**,
which is required by unions, by fixed field offsets, and by uniform array stride.

The cost — one indirection to reach the characters — falls **only on content access** (indexing,
iteration, concatenation), not on moving, storing, passing, or length checks, which touch only
the handle. Since programs move strings far more than they scan them, the trade favors the handle.
Small-string optimization can recover pointer-free access for the short common case later, behind
the same interface.

```
string str = "Hello";
int    len = str.length();
string sub = str.subStr(0, 2);   // "He"
```

Open concern flagged for I/O: **encoding** (bytes vs characters — what does `subStr` count?) must
be decided deliberately at the byte/text boundary.

---

## 11. Collections

Arrays are the general sequence type, **akin to modern JS arrays but dense** — the ergonomics and
method surface, without JS's sparse/holey object-in-disguise substrate (which is the one piece
that is a special-case generator). An array is a **pure, immutable value**: methods return new
collections; to "change" a variable you rebind it. `arr[i] = v` is rebind-sugar
(`arr = arr with [i] set to v`), so `[]=` reads naturally without breaking purity. The
efficiency story is **copy-on-write on the refcount** (§15): mutate in place when uniquely owned
(refcount 1), copy only when shared — pure surface, in-place implementation (the Swift model).
Purity also means arrays are race-free and share freely across threads (§14). Genuine shared
in-place mutation would be a separate, gated `MutableList` (§16).

*COW status:* **implemented on the indexed-write path** across the executing engines (IR
interpreter, emit-C++, pure ELF): `arr[i] = v` and `m[k] = v` mutate in place when the base is
uniquely owned — including dense value-struct arrays and existing-key map updates — and copy
when aliased, so a set-in-a-loop is O(n), not O(n²) (measured: 1M indexed stores in ~0.07s on
the ELF backend; ~6.5s for 30k before). The self-append path (`a = a.add(x)`) had this already
via `MoveClear`. Two deliberate asymmetries: the tree-walk oracle stays copy-always (it is the
semantic reference the COW engines are differentially checked against), and a missing-key map
insert still copies (the allocator prefix records the requested size, so there is no spare
capacity to append into — geometric map growth is a possible follow-up).

```
Array arr  = [1, 2, 3, 4];
Array arr2 = arr.where((el) => el % 2 == 0);   // [2, 4]
arr = arr.concat(arr2);                        // [1, 2, 3, 4, 2, 4]
int num = arr.first();                          // 1
```

### Construction and ranges

```
Array<bool> flags = Array(limit + 1, false);   // sized + filled (T inferred from the fill)
Array<int>  a;                                 // bare declaration -> [] (§3)
Array<int>  r = [1..5];                        // range literal spreads -> [1,2,3,4,5]
Array<int>  m = [1..3, 7, 9..10];              // spread mixes with scalars
```

A **range** `a..b` (inclusive) is a first-class value — printable (`console.write(1..5)` →
`1..5`), iterable (`for (int i in 1..limit)`), and spreadable in array literals. Ranges are the
standardized iterator shortcut.

Arrays index through the same `([])` indexer any class can define (§6.7):
`Array<T>` declares `get ([])(int i) => at(i)`. The stdlib method surface (where/filter, map,
reduce, any/all/count, contains/indexOf, reverse, take/skip, concat, joinToString, first/last,
plus Track 05's flatMap/forEach/unique/groupBy/zip/takeWhile/skipWhile/indexWhere/withIndex/
insertAt/removeAt/with/slice/sort/sortBy/minBy/maxBy/find/firstOrNone/lastOrNone and the
`std::sum`/`min`/`max`/`average` free functions)
is **written in the language** over a three-intrinsic native core (`length`, `at`, `add`).
`map`/`select`/`reduce` are **method-level generic** (`map<U>`, `reduce<A>`) — the result type is
inferred from the transform, not pinned to `T` — via a checker fix (lambda-last generic
inference: value arguments bind first, then each lambda's parameter types are substituted in
and its body's inferred return type binds the rest; reference.md §6.3 and
`designs/complete/techdesign-05-stdlib-collections.md` §1 have the mechanism).

**Key equality (contract C3)** governs `Map` lookups and the `Set<T>` built over them:
primitives by value, `struct` keys field-wise recursive (a struct IS its fields, §9), classes by
identity. `Set<T>` (Track 05 §4.1) is Map-backed, zero natives, same pure-value model as
Array/Map.

### Data / relational operators and joins

Arrays get the **join family** (a place sequence-thinking tends to forget). Distinctions kept
crisp so one word never means two things:

Regular expressions join this data-operator family as a byte-oriented, compile-once operator:
the in-language Thompson/Pike engine and its bounded lazy-DFA cache guarantee linear-time
matching, with no backtracking, backreferences, or lookaround. *Status (Track 10, LA-13):*
**landed in full** — the all-in-prelude Thompson NFA / ordered Pike VM /
byte-equivalence-class lazy DFA engine (zero natives, comptime-foldable through
`regex::compileProgram(pattern, flags) -> Array<int>`, reference.md §6.4.4), plus the
public C#-shaped `Regex`/`Match`/`Group`/`RegexOptions`/`RegexException` surface and
`namespace regex` conveniences (reference.md §6.4.6, `designs/complete/techdesign-regex-library.md`).

- `join` = **by key**; `zip` = **by position** — never blurred. The string-building form is
  therefore named `joinToString` (the name `join` is reserved for the relational operation).
- **inner / left / group / cross** joins are distinct operations; **group join** (each element of
  A with all its matching B) is included because it is the one most often hand-rolled.
- `join` **builds an internal index** so it is O(n+m), not O(n·m) — arrays carry an index. The
  relational machinery stays hidden inside the operator; arrays remain arrays (sequences), not a
  promoted relation type. **Current state (Track 05):** `join`/`groupJoin` ship as the predicate
  (nested-loop, O(n·m)) form only — the by-key hidden index this promise describes needs a
  `Dictionary` type, which doesn't exist yet (scheduled after Gate C,
  `designs/complete/techdesign-05-stdlib-collections.md` §6). `groupBy` (new) is the same interim
  trade-off: correct now (O(n²) scan-map), fast later once `Dictionary` lands.

Current shape (honestly typed via method-level generics; returns pairs so only `U` needs
inference — from `other`, a real typed value):

```
Array<Pair<T, U>>        join<U>(Array<U> other, (T, U) => bool match)
Array<Pair<T, Array<U>>> groupJoin<U>(Array<U> other, (T, U) => bool match)
```

These are the predicate (nested-loop) forms; the by-key indexed O(n+m) forms arrive with the
Dictionary type.

Two decisions that determine whether data operators are *fast*, not just pretty:
**eager vs lazy execution** and **collection-vs-relation data model** (kept as
collection + hidden index).

The eager/lazy question (§19 #4) is **resolved** (Track 07): **arrays are eager**
(in-memory, bounded — every `map`/`where` materializes a new array) and **`Seq<T>`
is the opt-in lazy form**. `array.asSeq()` bridges into a lazy pipeline whose
combinators run nothing until a terminal pulls, run each function at most once per
pulled element, and let `firstOrNone` short-circuit. Both surfaces share the
iterator protocol (`IIterable<T>`/`IIterator<T>`) that also makes any user type
usable in `for..in`. See `docs/reference.md` §6.4.8–§6.4.9.

---

## 12. Namespaces, modules, and file layout

Namespaces are **declaration-based, not directory-based** — the `namespace` declaration in the
file determines the namespace; disk layout is irrelevant to the compiler. This is the
*unopinionated* model (C#-style), deliberately chosen over the opinionated directory-as-namespace
model (Java/Rust/Go). Imports are **by name, not by path**, so source never references the
filesystem.

The project/build layer gathers all source into **one logical compilation unit** in memory. File
boundaries dissolve at gather time; **namespace boundaries persist** (they are semantic structure
that partitions names). So the unit is "one tree, many named scopes," which is what makes
same-named types in different namespaces coexist, and what gives the whole-program visibility the
shape optimizer and first-wins resolution rely on.

```
namespace Room1 {
    class TheClass { public void welcome() { console.writeln("Welcome"); } }
    void start() { TheClass t = TheClass(); t.welcome(); }
}

namespace Room2 {
    class TheClass { public void welcome() { console.writeln("Hello"); } }
    void start() { TheClass t = TheClass(); t.welcome(); }
}

Room1::start();   // Welcome
Room2::start();   // Hello
```

### `uses` — importing a namespace

`uses NS;` (or `uses A::B;`) imports a namespace's names into the enclosing scope as an
alternative to `NS::Name` qualification (which always remains available). Nearer declarations
shadow imports. This is the ordinary-code counterpart of `::`-qualification — the same
"explicit selector only when needed" shape as everywhere else.

```
namespace M { class Widget { ... } int helper(int x) => x + 100; }
uses M;
Widget w = Widget();     // unqualified via the import
M::helper(1);            // qualified still works
```

### `use` — importing one name, selectively (imports.md)

`use Path::name;` (optionally `as alias`) pulls a **single** declaration — a value, a
function, a class, or a nested namespace — out of a namespace into the enclosing scope,
instead of `uses`'s whole-namespace dump:

```
use std::env::args;               // a function: bare args() now works here
use Lcurl::Header as HttpHeader;  // a class, renamed (collision-proof)
use A::B;                          // a nested namespace itself: B::f() then works
```

**The lexical model (owner ruling, 2026-07-04).** The naive framing — "imports are
file-scoped" — gets the ergonomics right but the mechanism wrong for this language: files
dissolve at gather time by design (namespace boundaries persist, file boundaries don't;
above), so making imports a *file-scoped primitive* would reintroduce the file as a
semantic wall the language deliberately tore down. Instead:

> An import (`use` or `uses`) is a declaration in its enclosing **lexical scope** — the
> same rule that already governs `bind` (§12.5: block-scoped, lexically resolved,
> nearest-wins — "a namespace is just one kind of block"). The top level of a file is a
> lexical scope. A top-of-file import therefore covers exactly that file, as the
> *consequence* of one rule, not a special file-wide mechanism — and the identical
> import placed inside a function or block scopes to exactly that block instead.

Visibility is **hoisted within its scope** (position-independent, like every other
declaration — an import anywhere in a file/block is visible throughout it) but does
**not** cross into an enclosing or sibling scope: a block-level import is confined to
that block; a top-of-file import does not leak into other files (the two defects bug.md
#8 found and fixed — a shared program-global dump was leaking imports across files and
silently dropping block-level ones — before this scoping model existed).

**An alias names the same slot, not a copy.** `use NS::x as y;` — reading or calling `y`
reaches the identical global `x` does; there is no runtime copy, no separate
initialization. Writing through the alias is therefore a write to the global, rejected at
compile time exactly like a qualified `NS::x = ...` (§3.7's ban on silently-discarded
writes, bug.md #7's ruling). Two imports contributing the same name from different
namespaces merge as an overload set for functions (resolution by argument types
disambiguates, same as two `uses` dumps would); for a non-function name, `use` shadows a
same-named `uses`-dumped name in the same scope ("specific beats bulk" — the
most-specific-wins instinct applied to imports).

### Modules as values (open)

Whether to add **modules as first-class values** (bind a unit to a variable, select an
implementation at runtime, functors as functions over module-values) depends on one question:
**is choosing a unit ever a runtime decision, or always compile-time?** Runtime →
modules-as-values, gated, with the indirection contained to calls through the module variable.
Compile-time only → namespaces suffice and "in a variable" is a compile-time alias. The design
leans namespaces-default + modules-as-opt-in-for-runtime-selection.

---

## 12.5 Dependency injection: `bind` / `inject`

DI is **compile-time, not a runtime container**. A binding is a **type-keyed factory member**:
a body that runs when that type is injected, returning the value to inject. The body rule is
identical to methods (one statement; `=>` is `return`):

```
bind ILogger { return ConsoleLogger(); }   // full body
bind ILogger => ConsoleLogger();           // arrow-sugar (fresh per injection)
bind ILogger => shared;                    // shared instance (what the body returns decides)
```

- **Scope:** a `bind` is **block-scoped and lexically resolved**, nearest-wins shadowing (a
  namespace is just one kind of block). Put a bind at the outermost block and it propagates
  project-wide. A binding is compile-time ambient context — never passed at runtime; the value
  it produces flows through ordinary parameters.
- **Injection is implicit when unambiguous.** An unfilled parameter with a binding in scope is
  filled. `inject <Type>` is the explicit selector, required only on collision — the same rule
  as `::` and constructor labels (explicit only when default resolution can't choose): when
  multiple injectable overloads exist, or an exact-arity overload shadows the injecting one.
- **Duplicate binding for the same type in one scope: hard error** (which implementation the
  whole program gets is exactly the silent-distant footgun §16 makes loud).
- **Channel 1 — `use` activates a namespace's bind (system-binds.md, landed).** The two
  lexical-provision systems (`use` provides names, `bind` provides values by type) weld at one
  point: a factory bind at the **top level of a namespace body** is that namespace's *exported*
  bind for its type, and `use NS::T;` (or `use NS::T as A;`), when `T` resolves to a
  class/interface, installs NS's bind for T into the `use`'s own scope — as if `bind T => ...;`
  (or `bind A => ...;` under the alias) were textually present there. `uses NS;` (bulk import,
  including the implicit `uses std;`) never activates — only a selective `use` of the type
  does, so nothing is ambiently bound just by being visible. A textual bind in the same scope
  as the `use`-activation wins silently (not the duplicate-bind error, which is reserved for
  two *textual* claims) — this is what makes "`use` the name, then `bind` a fake in the same
  scope" (the test idiom) frictionless. Ships with five root-bound capability interfaces —
  `IEnv`/`IConsole`/`IClock`/`IFileSystem`/`INet` in `namespace std`, thin shims over the
  ambient globals (`env::*`/`console`/`std::sysNow`/`File`/`TcpStream`+`TcpListener`) — the
  disciplined, cheaper-than-plumbing alternative to those globals, not an enforced sandbox
  (reference.md §4.7, §6.6.6a). **[planned] Binder objects (Channel 2):** a future `Bindings`
  value packaging type-keyed factories for explicit installation with `bind someBindings;`
  remains undesigned and deliberately deferred (the aggregation use case is owned by the
  metaprogramming splice mechanism instead); use lexical factory binds (plain or
  `use`-activated) today.
- **Tiering is discovered, not declared** (§7 rule): a constant body → compile-time direct
  call; a body that branches on runtime state → resolved through the binding at runtime.

Per-call-chain overrides (a caller redirecting a callee's internal injection) are deliberately
out of scope — that is hidden-parameter threading, the same contagion shape the language
rejects elsewhere.

## 12.6 Exceptions

**Exceptions are the primary error mechanism; unions provide the Result path** for expected
domain outcomes (`int | string classify(...)` is Result-shaped and already works). Rationale:
(a) **catch selection is resolution-by-type** — the language's core rule applied to the failure
path; (b) **anti-contagion** — `Result<T,E>` colors every signature up the call chain (the same
contagion shape as `any` and async-coloring, both rejected); exceptions keep signatures clean;
(c) multiple inheritance + interfaces give a **catch-by-capability** system the C++/C#/Java
family cannot express cleanly.

```
try {
    throw RuntimeException("Server error");
} catch (IRuntimeException e) {          // catch by CONTRACT (interface)
    console.writeln(e.message);
} catch (IException e) { ... }           // first assignable clause wins
```

- `throw expr;` is a statement shaped like `return` (a control transfer carrying a value).
- **A thrown value must implement `IException`** — catchability is a contract, not
  "throw anything."
- Catch clauses select by the thrown value's **dynamic type**: first clause whose type it is
  assignable to (subclass → base → implemented interface). The binding name is optional.
- `try`/`catch` bodies follow body-is-one-statement.
- **No `finally`**: §15's scope-based cleanup is RAII-shaped and covers most `finally` uses;
  the resource-lifetime question (§19 #8) resolved as `using` (techdesign-02 F3) — deterministic
  `close()` on every block-exit edge, without a general `finally`.
- Uncaught exceptions terminate with a report.

### The standard hierarchy (`namespace std`, implicitly imported)

**Interfaces define catchability; classes provide the standard payload.** The doubling is the
point: any class can make itself catchable-as-X by implementing the interface, without
inheriting the concrete class — multiple inheritance earning its keep.

```
interface IException        { string message; string toString(); }
class Exception             : IException
interface IRuntimeException : IException
class RuntimeException      : Exception, IRuntimeException
interface ILogicException       : IException
class LogicException            : Exception, ILogicException
```

The stdlib gains a **`std` namespace with an implicit `uses std;`** — bare and `std::`-
qualified names both work; user declarations shadow std (nearest-wins). Naming: **uniform
`Exception` suffix** (`LogicException`, not `LogicError`) — everything catchable is an
Exception; `Error` is reserved in case uncatchable panics ever exist. (Open: qualified type
names in type position, e.g. `catch (std::RuntimeException e)`, are not yet parsed; rethrow.)

### Runtime failures are loud

Built-in runtime failures **throw real, catchable `RuntimeException`s** rather than producing
silent voids: array index out of bounds, missing Map key (`at` on an absent key — use `has` to
test), unresolvable call targets, and applying an operator a class doesn't define. This is §16
applied to the runtime: no silent-and-distant failures.

## 12.65 Pattern matching: `match`

`match` is **resolution by type/value surfaced as control flow** — the same rule that governs
overloads, member access, `catch`, and `is`, given a statement/expression form. It is *not* a
new dispatch mechanism: match arms lower to the **same `is`/`IsType` type test** that `catch`
and `is` use, so there is one type-dispatch path across the language.

```
string describe(IShape sh) => match (sh) {
    Circle => "circle";      // TYPE pattern — narrows sh to Circle in the arm
    Square => "square";
    else   => "shape";
};

string sign(int n) => match (n) {
    0    => "zero";          // VALUE pattern
    1..9 => "small";         // RANGE pattern
    else => "big";
};
```

- **Patterns** are a type (`Type => …`), a value/range expression (`0 => …`, `1..9 => …`), or
  the catch-all `else`. First-match-wins (base types last), the same ordering as catch clauses.
- **The subject narrows per arm** (reusing the flow-narrowing engine): inside `Circle => …`,
  `sh` *is* a `Circle` — no new binding, unlike `catch`.
- **Exhaustiveness ties to the closed/open line.** Over a **closed union** the compiler checks
  every member is covered — no `else` needed, forgetting one is an error. Over an **open
  hierarchy** (unknown future implementors), an `else` is required. This is the same
  closed-vs-open distinction that governs dense variant layout: `A | B` is closed/exhaustive,
  a `class` hierarchy is open.
- **Expression or statement** — each arm yields a value (like the ternary), or a block.
- **`catch` is the kin**: a `match` on the thrown value's type wired into unwinding. The two
  stay distinct constructs but share one type-test core.

## 12.7 Statements and control flow (accumulated)

- `if`/`else`; `while`; `do`-`while` (post-test — body runs before the first check;
  techdesign-02 F2); C-style `for (init; cond; step)`.
- **`for (T x in iterable)`** — iterates ranges, arrays, (later) any iterable.
- **Ranges** `a..b` — inclusive, first-class, printable, spreadable (§11).
- **Compound assignment** `+= -= *= /= %=` — `a op= b` ≡ `a = a op b`.
- **`break;` / `continue;`** (techdesign-02 F1) — unlabeled, target the innermost enclosing
  loop; a compile error outside one. A lambda body is its own loop-nesting scope (a bare
  `break`/`continue` never escapes into an enclosing function's loop); `match` is not a
  loop-nesting boundary (an arm's `break`/`continue` targets the enclosing loop).
- **Labeled `break label;` / `continue label;`** (techdesign-labeled-break-continue.md) — a
  loop labeled `label: while (...)`/`for (...)`/`do ... while (...)` can be targeted from
  arbitrary nesting depth. One label per loop; sibling loops may reuse a label; an enclosing
  loop's label cannot be reused (duplicate = compile error); labels are a separate namespace
  from values. Same lambda/match boundary rules as the unlabeled forms. A labeled exit crossing
  one or more `using`s closes exactly the resources declared inside the *target* loop, reverse
  declaration order — see reference.md §5.2.
- **`using Type name = expr;`** (techdesign-02 F3) — deterministic resource cleanup; see §12.6
  and reference.md §5.2/§6.6.65.
- Body-is-one-statement applies everywhere a body appears (methods, accessors, binds, loops):
  a block `{ }`, `=> expr;` (arrow is `return`), a bare statement, or `;`.

## 12.8 Projects, multi-file builds, and the package manager — **[built]**

*Status:* implemented, then **split into two binaries** (`designs/complete/proposal-project-system.md`,
`designs/complete/techdesign-toolchain.md`). A project is **the two-source prelude gather (§12)
generalized from 2 to N**: a manifest lists every source file (and every dependency), all of it
concatenates into one compilation buffer, and that buffer runs through the ordinary single-file
pipeline unchanged — namespace boundaries and whole-program resolution (§12) apply exactly as
they do today. There is no separate "multi-file mode."

**Two tools (the toolchain split, HARD requirement).** The package manager and the compiler are
**separate binaries**, cargo/rustc–style: **`trident`** owns the manifest, resolves the
dependency graph, and emits a resolved **build plan**; **`leviathan`** is a pure compiler that
consumes a plan (`leviathan --plan build/plan.lvplan`) and never parses a manifest or knows a
registry exists. See reference.md §8.

**The manifest is `trident.toml` (TOML), superseding the earlier `project.mf`.** The original
design's `project.mf` "data in the language's own literal syntax" form **no longer exists** — the
landed toolchain uses a fixed-name `trident.toml` in TOML, read by a small hand-rolled parser so
the toolchain stays dependency-free (§17). The default source extension is `.lev` (`.ext` still
accepted):

```toml
name    = "app"
entry   = "main"                # a function name, or a file ("run.lev")
sources = ["*.lev"]             # globs expand alphabetically; explicit lists too
assets  = ["views/**", "schema.sql"]   # comptime import() targets (§16.5 LA-20); optional
version = "0.1.0"               # optional
out     = "app"                 # optional

[[dep]]                         # repeated array-of-tables; omit if no deps
path    = "jsonlib"
as      = "Json"
version = "1.0.0"
dev     = false
```

- **`entry`** is either a **function** (gather everything, call it) or a **file** (that file's
  top-level statements drive the program; other sources are declaration-only) — the same
  function-vs-file distinction as anywhere a body is one callable unit (§6.6). trident classifies
  which and records it in the plan; the compiler never sniffs the extension.
- **`sources`** supports glob expansion (`"*.lev"`); files re-opening the same namespace (§12)
  merge as usual.
- **`assets`** declares the build inputs a comptime `import()` (§16.5, LA-20) may read —
  literals, globs, or `**` recursive globs — hashed by trident and carried in the plan.
- **`[[dep]]`** accepts local paths and VCS modules. A local `path` names a directory holding its
  own `trident.toml`; a non-directory path such as `github.com/acme/json` requires `version` and
  resolves by MVS. VCS sources are pinned by `trident.lock`, verified against the tamper-evident
  checksum log, and deduplicated in `$TRIDENT_HOME/store/<sha256>/`. `trident vendor` + `--vendor`
  is the network-free path; `$TRIDENT_PROXY` is an optional static cache and `$TRIDENT_INDEX` an
  optional first-wins name→VCS-path map. Neither service is mandatory. `dev = true` is a
  development-only dep, excluded from shippable-artifact modes.
- **`as`** aliases a dependency's exported namespaces into a synthesized local namespace
  (`uses Client;` reaches a dep declared `as = "Client"`), so a consumer never has to know the
  dependency's internal namespace name.
- **Phantom-dependency prevention (pnpm-style strictness):** the dependency graph is tracked
  per-module; a file may only `uses` a namespace that belongs to the project itself or to one of
  its **direct** `[[dep]]` entries. Reaching a transitive dependency's namespace without declaring
  it directly is a compile error (`namespace 'X' comes from an indirect dependency`) — the same
  discipline pnpm/Cargo apply. trident decides the adjacency (plan `edge` rows); leviathan enforces it.

```
trident build [dir]                      # resolve trident.toml -> plan -> leviathan --plan --build-native
trident run   [dir]                      # ...compile and execute (tree-walk oracle)
trident add/remove/update/lock/fetch/why # edit, pin, fetch, and explain the MVS graph
trident vendor | audit                   # hermetic copy; hash + optional trust-policy verification
trident publish [--tag vX.Y.Z]           # immutable git tag + checksum + optional index/attestation
trident yank <path>@<version>            # block new selection; existing locks remain valid
leviathan --plan build/plan.lvplan       # compile a whole project from trident's resolved plan
leviathan --imports file.ext             # dump the file -> imports provenance map (P-4)
leviathan --graph file.ext               # dump the `uses` include graph + build order (P-3)
leviathan --namespaces file.ext          # symbol index: every namespace, its files, its members
leviathan --why <name> [in <file>] file.ext  # where a bare name resolves from (which candidate wins)
leviathan --lint-namespaces file.ext     # opt-in folder≈namespace convention check (off by default)
leviathan --assets file.ext              # list every comptime import()-consumed asset (LA-20 §16.5)
```

A bare single file (no `--plan`) is a **project of one** — `--imports`/`--graph`/`--namespaces`/`--why`
synthesize a one-file map so the same tooling works uniformly whether or not a manifest is present.
`--namespaces` and `--why` pay back the by-name + path-decoupled discoverability cost (§12) with a
compiler query instead of a directory rule: the symbol index, and per-name provenance. `--lint-namespaces`
is the opt-in inverse — for teams who want Go/Java folder-tidiness, it flags files whose directory is
not a suffix of a namespace they open (exiting non-zero on a mismatch) without the language imposing it.

---

## 13. Streams

**Streams are THE system boundary: nothing crosses the process boundary except through a
stream.** There is no separate file/console/socket/timer API — those are stream endpoints.
The layering:

- **Layer 0 — the syscall floor** (the §16 bazooka-room bottom shelf): the ONLY privileged
  code. Declared in the language (`std::sysWrite(fd, data)`, `std::sysReadLine(fd)` —
  interim string-carried shims that collapse to `syscall(nr, ...)` + `Block` when the gated
  raw-memory features land; C++ stands *behind* these declarations temporarily, never beside
  them). Self-hosting bottoms out here: the floor becomes language-authored unsafe code over
  the literal syscall instruction.
- **Layer 1 — the substrate**: `StreamBuffer<T>` is a **queue** — single consumer, each
  element consumed exactly once, with an overflow policy. Written in the language (Array
  storage today; the lock-free ring over raw memory is the later implementation of the same
  interface). Typed views `InStream`/`OutStream` expose capability structurally.
- **Layer 2 — reshapings** (pure library, zero new privilege): `Promise` = a one-shot
  stream; `EventEmitter` = fan-out. **Broadcast is never a substrate property**: an emitter
  consumes a stream once and re-emits to N listeners, each with its own buffer — the
  copy point is explicit, and a slow listener is visibly that listener's problem.

**`subscribe(cb)` is a standing pull, not broadcast**: it *claims* the stream's single
consumer end (drains what is queued, then receives pushes directly); a subsequent `pull` on
a claimed stream is a loud error. Queue vs broadcast is the streams-vs-events distinction
kept honest (§11: one word never means two things).

Extraction: `reader >>` (the value-returning extract) is `reader.pull()`; pull on an empty
stream is a loud error — a caller that wants to wait instead of throwing has two options
(D-B, `techdesign-http-and-streams-maturity.md`): drive the stream through `for..in`/
`IIterable<T>` (`hasNext()` parks on the event loop until data arrives or the stream
closes — real suspension, LA-30, not a spin), or poll explicitly with `pullOrNone()`
(`None` = nothing right now, no park). `sysReadLine` returning `""` signals end of input —
an interim convention that a Result-shaped signal replaces.

Four types over one substrate:

- **`StreamBuffer`** — type-agnostic raw conduit; knows nothing about `T`.
- **`InStream<T>`** — typed read view; carries the extract operator.
- **`OutStream<T>`** — typed write view; carries the insert operator.
- **`IOStream<T>`** — the composition of both, adding nothing of its own.

```
public class IOStream<T> : InStream<T>, OutStream<T>;
```

Direction lives in the type, and **capability is which operators the type exposes** — an
`InStream` physically lacks the write operator, so read-only/write-only safety is structural, not
a runtime check (the same shape as get/set views).

Under the diamond (`In` and `Out` sharing a stream base), the shared `StreamBuffer` **collapses
to one** (not `distinct`) — a bidirectional stream must have both ends on the same conduit.

### The event loop (implemented for timers)

The runtime owns a work registry; after top-level completes, the engine keeps dispatching
while live work remains and the program exits when none does (the implicit-loop lifetime
rule). Dispatch is **single-threaded** — callbacks never race, preserving §14's isolation
story before threads exist. A `Timer` is the first live system stream: the loop invokes a
closure that pushes ticks into an ordinary `StreamBuffer`, so subscription/delivery is the
same in-language machinery as any stream. Sockets ride this same loop next; `await` later
parks on it (a promise is a one-shot stream).

### async / await (implemented — stackful tasks since LA-30)

A `Promise<T>` is a **one-shot stream**: a value that arrives later. `await p` **parks the
current task** until the promise is `ready`, then yields its `value` — so await and
stream-pull are the same suspension, two surfaces (§14). There is **no function coloring**:
any function may `await`, and a function is "async" simply by returning a `Promise<T>` (no
`async` keyword). `await` unwraps `Promise<T>` to `T` at the type level. Callback APIs
reshape into promises trivially (`HttpClient.fetch` = `get` + a resolve callback), turning
callback pyramids into linear code over the same loop.

The v1 mechanism was a **pump**: `await` re-entered loop dispatch from inside the awaiting
frame, so every callback ran nested on the same stack until the awaited promise was ready.
That bought zero new runtime at the cost of stack-ordered resumption, nothing killable, and
misrouted failures. LA-30 replaced it with **stackful tasks** (the Loom-shaped swap the
uncolored surface made a pure runtime change): each loop-dispatched callback — and the
program top-level — runs on its own small mmap'd stack; `await` parks the task and switches
to a per-thread scheduler; completion moves parked tasks to a FIFO run queue. Resumption is
**completion-ordered**, a drained loop with a pending promise **throws** (never fabricates
the default value), and an uncaught callback throw is program-uncaught (never teleported
into an unrelated `await`). Normative semantics: reference.md §6.6.67; design:
`designs/complete/techdesign-0*-{task-substrate,llvm-leg,interpreter-legs,emitcpp-leg,semantics-and-flip}.md`.

### Sockets and HTTP (implemented)

Sockets ride the same event loop as timers: a watched fd fires a callback when
read-ready, and the in-language code does the accept/recv and pushes into a stream. The
loop stays dumb ("this fd is ready, run that"); all protocol logic is in the language.

- **`TcpStream`** — a connected socket wearing the stream surface: `(<<)`/`send` (write),
  `onData((string) => void)` (a read-watch that recvs and delivers chunks), `onClose`,
  `close`. `sysRecv` returns `string?` — `None` = peer closed — so the three states
  (data / nothing-now / EOF) are the ones union narrowing was built for.
- **`TcpListener`** — a listening socket presented as a **stream of connections**:
  `connections((TcpStream) => void)` accepts each client as a new stream.
- **HTTP** is a pure Layer-2 reshaping over TCP streams, written entirely in the language,
  now **framework-grade** (Track 09 F4, zero new natives): an ordered case-insensitive
  **`HeaderMap`** (of `Header` structs; duplicates + order preserved for `Set-Cookie`),
  **chunked transfer both directions** (a fragmentation-proof `ChunkedDecoder` +
  `chunkEncode`), incremental request/response `parse(feed)` state machines,
  **server-side keep-alive** (re-arm per connection, bounded at 100), a **500 error path**
  (an uncaught handler throw becomes `500` + `Connection: close`, loop survives), and a real
  **`HttpClient`** (`request`/`get`/`post` + await-able `fetch`). Text bodies until `Block`.
  One-process loopback tests (server + client in one event loop) stay the hermetic corpus,
  identical across engines; the existing programs remain green on the frozen ELF backend too.
  Deferred: client redirects, URL-string parsing, request timeout, pipelining, client-side
  chunk-send, connection pooling.
- **TLS is an fd property under the stream boundary** (LA-2,
  `designs/complete/techdesign-tls-crypto.md`): transport security lives in the fd, direction
  in the type. Arming TLS over a connected/accepted socket wraps it **in place** (the fd
  number never changes meaning); `sysSend`/`sysRecv`/`sysClose` then route through the session
  and `TcpStream`/`HttpServer`/`HttpClient` gain TLS with **zero API change** — `HttpServer(port,
  cert, key)` and `HttpClient.getTls/…` are the whole surface. Real cert verification is ON by
  default (chain + RFC 6125 hostname), TLS 1.2 floor + 1.3. **OpenSSL joins LLVM as a
  sanctioned, feature-detected dependency** — "C++ stands behind these declarations
  temporarily, never beside them": the ~12-function provider seam keeps an in-language TLS 1.3
  over `Block` the principled self-host-era destination, and a build without OpenSSL still
  compiles (plaintext binaries, clean not-built diagnostics). The event loop absorbs the two
  classic TLS/loop hazards (want-direction inversion, buffered-plaintext stall) with a poll
  augmentation that is **bit-identical to the plaintext loop whenever no session is live**.
  Standalone crypto: `sysRsaEncrypt` (auth key-transport) and the crypto-grade `sysRandom`.

Two general features fell out of this work: **block-body lambdas** (`(x) => { ...stmts... }`,
not just expression bodies) and **keyword member names** (`client.get(...)`, `void get(...)`
— `get`/`set`/`is`/… are only keywords at statement/declaration start, names elsewhere).

### System events as streams

System events (e.g. raw network events) are surfaced as **system streams** rooted on a
mother/registry object. The data is already in memory; a system stream is a typed view onto it.
Activation is **demand-driven** — the system wires up event routing only for streams that are
actually used, as an optimization layer. Because streams already have **sub/pub hooks**, this
laziness is a library pattern (subscription = activation), not a language feature.

Memory blocks can be assigned to streams as the push→pull rate-matching region (a single-producer
single-consumer ring stays lock-free via atomic head/tail). This is also the zero-copy point: the
typed in-stream reinterprets the buffer in place. The one forced pairing: **block size comes with
an overflow policy** (block / drop / grow / error), and lossless vs lossy-tolerant streams need
different policies.

---

## 14. Concurrency and async

> **Status (Track 10, 2026-07-11): pure isolation v1 with copy-always boundaries
> — LANDED IN FULL, true OS threads live on LLVM.** `spawn`/`Worker<T>`/join and
> `Channel<T>` run on all three active engines byte-identically. On the tree-walk
> oracle and IR interpreter a worker is a **cooperative loop task**; on the
> **LLVM backend it is a true OS thread** — its own per-worker TLS heap/arena/
> event loop, real `pthread`s, an `eventfd` join, and reap-time `munmap` of the
> worker's regions, with the result rebuilt back on the *spawner's* thread (so a
> promise continuation is only ever run by the thread that owns it). Every value
> that crosses a thread boundary crosses by **deep copy** (a C flatten/rebuild
> engine with a seen-map for shared substructure and cycles) — a counted value
> lives on exactly one unit, so the non-atomic refcount fast path (§15) is
> preserved verbatim; a `Worker`/`Promise` handle may NOT cross (A-1), `Channel`
> is the one portal. The two-tier "immutable data shares freely" of this section
> is **deferred to v2** (an atomic-count immortal tier); v1 copies. Threads are
> POSIX-only in v1: a Windows target rejects `spawn`/`Channel` at compile time
> (`LV_TLS` is non-TLS there), as does the frozen ELF backend. Design + status:
> `designs/complete/techdesign-threads-2.md` (D1′ copy-always model, refuting
> `techdesign-threads.md`'s move model) and `-3.md` (the M3/M6 true-thread
> execution leg); leak/reap/fd verified by `fuzz/thread_leak.py`.

### Execution units (two tiers, shared memory)

- **Low tier — `spawn`:** start a raw worker (a real OS thread that shows up as a running
  unit). Returns a handle to join/wait on. Fewer guardrails. (An earlier `fork(expr)` keyword
  for this tier was dead grammar — never acquired a runtime — and was removed at LA-30 M5;
  `std::spawn` is the landed reality, Track 10.)
- **High tier — promises on their own threads:** built over the low tier; a promise is the typed
  handle to a future result.

Both share the address space, so the isolation/race rules below govern both.

### Safety model

**Isolation by default:** a worker captures its inputs by copy or ownership, not shared mutable
reference; it runs, returns a result. **Immutable data shares freely** (cannot race). **Shared
mutable access is the gated, marked exception.** This keeps the shape system safe (no cross-thread
slot mutation on the default path) and reinforces the purity bias of the collection methods.

The sharpest interaction to respect: **threaded workers + dynamic shape mutation** race on the
*shape itself*, so shared objects across worker threads should not be the dynamic kind (or shape
changes must be confined / synchronized).

### `async` / `await`

`async` / `await` is the locked-in surface (it is the popular, expected one). **`Promise<T>` is a
plain object library** — only `await` is privileged. The honesty rule from §1 applies: an async
function's return type must not secretly become a wrapper. The consistent spelling is
`Promise<T>` as an ordinary typed return with `await` as an operation on that value, avoiding the
function-coloring contagion (the same contagion shape as `any`).

`await` is where "easy object library" stops being only a library: it is a **suspension point**,
which is compiler+runtime machinery, and it interacts with the two runtimes (below) and the thread
model. Since LA-30 that machinery is **stackful tasks** on all three active engines: the awaiting
task genuinely suspends (its own stack parks; a per-thread scheduler runs whatever else is
runnable) and resumes in completion order — see §13's async/await entry and reference.md §6.6.67
for the observable contract, including the two loud failure modes (drained-loop await throws;
an uncaught callback throw is program-uncaught, never delivered to an unrelated await).

**Cancellation** (LA-30 B2, landed 2026-07-12) resolved the concern above without threading
anything through signatures: it rides the existing suspension points instead of the type
system. **Cancellation is an exception delivered at `await`, and only there** — a running
task is never preempted, its mark takes effect at its next park — so no async/stream
signature changed. `CancelledException` is an ordinary catchable `IException`; `TaskGroup`
(structured concurrency over the existing `using`/`IDisposable` rule, zero new syntax) owns
a set of same-thread child tasks and joins/cancels them on every scope-exit edge;
`awaitTimeout<T>(Promise<T>, ms) -> T?` turns a timeout into an outcome (`None`), not a
failure. Full contract: reference.md §6.6.68; design: `designs/complete/techdesign-06-b2-cancellation.md`.

Still open: **errors across async/stream boundaries** (a `Result<T, E>` union is
the consistent choice — errors as values, composing with `Promise<Result<T,E>>` and streams of
`Result`).

### Lower-level primitives

Spawning a worker is the raw layer, and `std::spawn` is it (Track 10). Note: an
OS-`fork`-that-clones-an-address-space would collide badly with threads (inherited locks held by
vanished threads) and is platform-specific; **spawn** is the clean cross-platform primitive, with
any raw clone-style primitive gated like the other low-level tools.

---

## 15. Memory management

The model: **ownership/scope-based cleanup for the common case, reference counting for the
escaping/shared case, raw allocation gated behind `unsafe`.** This is the Rust/Swift-shaped hybrid,
and it dissolves "is a GC mandatory."

### The scope vs escape distinction

- **Scope-shaped lifetime** (created and used within a scope): the compiler knows statically where
  the scope ends and **emits the free at that point.** No runtime collector, no per-value metadata,
  deterministic. This is what people often mistake a GC for — and it is cheaper than a GC because
  it is compile-time.
- **Escaping / shared lifetime** (returned, stored, shared, passed to something that outlives the
  caller): the last user is a **runtime** fact, so this is the only part needing runtime help —
  **reference counting**, deterministic release when the last reference drops, cost paid only on the
  shared minority.

At runtime, scope-owned values carry **no scope tag and no scope pointer** — the scope association
is compile-time analysis in the compiler, compiled *away* into free-instructions placed at fixed
code locations. Only reference-counted (shared) values carry runtime bookkeeping (the count).

*Status:* the escape/ownership analysis is **built and validated on the IR** — every allocation
site is classified scope-owned / ownership-transferred-by-return / refcount-tier, and an
`--ir-verify` mode confirms empirically (weak-reference liveness at frame exit) that scope-owned
allocations are dead when their frame exits, with **zero violations** across the corpus and
**85–90% of allocations deterministic** (the §15 bet, measured). Both tiers are now wired into
the pure x86-64/ELF backend and **the heap is bounded, not just accounted**: the **scope-owned
tier** frees through a per-frame arena (reset at every return path; provably-value-struct copies
route here too, not just object/array allocations); the **escaping tier** frees through real
retain/release — every heap-allocated object, boxed array, closure (its captures), and map (its
entries) carries a refcount prefix, release-old-then-retain-new runs at every slot write (locals,
globals, fields, container elements/entries), a return transfers ownership at +1 to the caller,
and the allocator itself does **free-list reuse** (power-of-two size classes; a freed block
returns to its class's free list instead of the bump pointer growing forever). A differential
**churn corpus** (`tests/corpus/churn/`, `fuzz/churn_leak.py`) pins this down: it churns each
heap-graph shape (arrays, objects, closures, maps, nested containers, recursive/linked
structures, dense value-struct arrays, returned value structs, heap strings, caught exceptions,
timer registrations, shared native reads, COW indexed writes) at two sizes and asserts
`live-at-exit` stays flat, cross-checked against the IR interpreter's reachability oracle
(`--mem-verify`) so growth is unambiguously a leak, not genuine liveness — **all thirteen
guarded programs are green at exactly +0B** (plus one declared expected-red XFAIL target,
below), and a
200k-iteration nested-object stress run holds at ~1KB escaping-tier peak (was ~120MB of
unreclaimed bump growth before free-list reuse). Real reuse also exposed and fixed several latent
"owner doesn't count" bugs along the way: boxed-array buffers not retaining their elements,
in-flight thrown exceptions not owned across unwinding frames, a catch bind leaking the
previously bound exception on loop rebind, event-loop timer/socket-watch registries not owning
their captured callbacks, and native `at`/`keys`/`values` not honoring the +1 transfer contract —
all now fixed and corpus-covered (`http.ext`/`sockets.ext` are green for the first time). The
last two gate-excluded reference kinds have since been brought under the discipline, completing
it: **value structs returned by value** (structs cannot be refcounted — a tag-5 struct pointer
may point inline into a dense buffer — so the fix leans on uniqueness instead: the lowerer tracks
call-result registers holding a fresh standalone struct copy and emits `Op::VFree` at the
consuming site, since once the caller copies the result out the returned tree is provably dead;
GC'd engines no-op the op), and **heap-allocated strings** (every heap-string allocation site —
concat, `subStr`, `toString`, readLine/recv, case conversion — now routes through the prefixed
allocator, and retain/release un-gates the string tag with a heap-address-range check so
data-segment literals and arena strings, which carry no refcount prefix, are skipped before any
prefix read). En route, bare value-struct-typed fields were found to never construct at all
(`$init` lowered them to a class-less default); they now auto-construct recursively in the
owner's memory tier (structs form a finite DAG, so the recursion terminates). **A later
fix (2026-07-14) extended the same rule to bare _reference-class_ fields**, which had kept
a void default and so read empty on the interpreters and *segfaulted* on emit-C++: a bare
constructable-class field now auto-constructs whether it is a value struct or a reference
class (a reference field lowers to the same `NewObject`+nullary-ctor+`RawSet` an explicit
`Field f = Field();` initializer emits, so ARC ownership is unchanged). The one carve-out is
a **construction cycle** — a reference field whose type can reach itself through non-optional
constructable fields (`class Node { Node next; }`, mutual `A{B b} B{A a}`) has no finite
default and keeps the void default; an optional back-edge (`Node? next`, → `None`) is the
intended cycle-breaker. The decision is a single static predicate
(`Symbols.hpp::bareFieldAutoConstructs`) shared by the oracle and the lowerer, so every
engine's field defaults stay byte-identical.

On this completed substrate, **§11's COW-on-refcount landed for indexed writes**: `idxset`
mutates in place when the base's refcount is exactly 1 (boxed arrays, dense value-struct
arrays, and existing-key map updates), and the backend releases the IndexStore dest temp's
stale reference — plus the rebind chain's (`CopyVal t; Move L`) — *before* the op instead of
after, since those written-once temps otherwise make a uniquely-owned base read as shared
forever, silently defeating COW (the loop was O(n²); the release is safe because any live op
input holds its own counted ref). One known gap, pinned by the corpus's one XFAIL target
(`dense_index_set.ext`): **the dense IndexStore value operand leaks** (~64B/iteration) — the
struct written into a dense array is heap-allocated as escaping, its record is memcpy'd in,
but the standalone copy is never freed; the fix needs Lower-side liveness (a `VFree` at the
consuming site, like returned value structs) or arena routing for provably-copied-in operands.
This leak predates the COW work — the COW templates merely made it visible.

### Return and the copy/delete reality

There is no "move" at the hardware level — the bits are **copied** to the destination and the
source is **deleted**/abandoned. On return, the returned value is copied out to the caller and the
function's locals are deleted at scope end; the copied-out value's cleanup obligation belongs to the
caller's scope.

For a **runtime-dependent return** (which local is returned depends on a runtime value), the
compiler cannot statically decide which local leaves and which is deleted, so it uses a small
**stack boolean per candidate** and deletes each local only if it was not the one returned:

```
MyClass myFunc(bool b) {
    MyClass myClass1 = MyClass();
    MyClass myClass2 = MyClass();
    return b ? myClass1 : myClass2;
}
// The returned object is copied to the caller (no third copy location).
// At scope end, each local is deleted only if it was not the one returned,
// guarded by a stack boolean — no double-delete, no leak.
```

Statically-decidable cases cost nothing (the compiler just omits the delete for the one that
leaves). Only the runtime-dependent case pays the small stack-boolean guard.

### Reference-counting tail

Reference counting cannot free an all-strong **cycle** (A holds B, B holds A → counts never reach
zero). The escaping/shared tier now provides programmer-declared **weak fields** for explicit
non-owning back-edges. This resolves retained-tree and Timer-handler cycle shapes when their
back-edge is declared weak; arbitrary all-strong cycles still require explicit detachment or a
future supplementary collector. This is the one asterisk on "no tracing GC."
(Deep acyclic chains — a linked-list-shaped recursive structure — release correctly today,
recursively and without a distinct collector; the asterisk is specifically true reference cycles,
which must still be broken explicitly when no edge is declared weak.)

---

## 16. The gate pattern (recurring)

Several features share one structure — **safe/managed default, raw power behind an explicit,
greppable marker:**

| Safe default | Gated escape hatch |
|---|---|
| Shapes, typed slots, direct offsets | Raw pointers / `getMem` (behind a directive; may be cut) |
| Per-name typed slots (dynamic properties) | Catch-all interceptor (compiler directive, slotless names) |
| Union / `Variant` | (`any` rejected outright) |
| Ownership + reference counting | Raw allocation (`unsafe`) |
| Isolation-by-default concurrency | Shared mutable across threads (marked) |
| `spawn` a process/worker | Raw clone-style `fork` (gated, Unix-specific) |

The principle for the gate: it is not paternalism toward the person who opts in — it is protecting
the safe majority's **guarantee**, especially where a footgun's blast lands on code that never
opted in (raw memory corrupting a bystander; `any` spreading by assignment). Where the fall is loud
and local, no net is fine; where it is silent and distant, the design makes it announce itself.

---

## 16.5 Compile-time rules (reflectors / macros) — **[Phases 1–4 + F4 built]**

*Status:* design accepted (`designs/proposal-metaprogramming.md`), tech design at
`designs/complete/techdesign-metaprogramming.md`. **Phases 1–4 and F4 procedural
macros are implemented.**

Phase 1: attributes (`attribute Name { fields }`, `@Name(args)` — typed,
per-file-scoped via the P-4 imports map), and comptime (`comptime` var/if/expr
— folded on the hermetic, step-bounded oracle between resolve pass 1 and a full
pass-2 re-resolve/check, so folds are cost-identical to hand-written literals on
all four active engines). `--expand`/`--no-rules`/`--comptime-budget` shipped.
Alongside it, `console` became a real prelude object (class `Console`, generic
`write<T>`/`writeln<T>`, `(<<)`, aliasable) — comptime console output emits
during compilation.

**LA-20: `import(path)`** is the one sanctioned build-input read inside
hermetic comptime — `std::import` is an ordinary prelude function whose
comptime evaluation is intercepted (the `sys*` gate inverted: denied at
comptime, allowed at runtime for everything else; `import` is allowed at
comptime and its runtime body just throws) and folds the named file's
content to a comptime string. The file becomes a declared build input: a
plan build resolves it against the importing module's own `assets =
[...]` (trident-hashed, per-module — a dep's `import()` never reaches an
app's assets or vice versa); a bare single file resolves it against its own
directory. `--assets` lists what a build consumed. Landed 2026-07-11,
`designs/complete/techdesign-comptime-import.md`.

Phase 2: **rules (Layer B)** — `rule N { match <shape> inject `template` at
<anchor> }`. The RuleEngine detaches rules, indexes every matchable
declaration, and fires each rule where its namespace is imported (§5 scoping on
the `uses` graph). Matching binds attributes (`@Route(r)`), the subject
(`on method m`), and enclosers with `:IFace` constraints read from the resolved
base chain. Injection clones the quasiquote template and substitutes holes
(`$r.method` reifies an attribute field; `$m`/`$C` splice selectors/names) at
`top`/`bottom of C.constructor` (synthesizing a ctor if none) or `member of C`
(with same-name+type conflict detection). Template-declared locals are
alpha-renamed (hygiene). Injected code goes through pass-2 resolve/check and the
backends — the `@Route`→`router.record(...)` route example runs byte-identical
to its hand-written twin on the active engines. `--rules` reports firings.
Phases 3–4 add `where` predicates, `meta.*` reflection, `$for` splices,
expression macros, body/marker/namespace anchors, and `rewrites` (Layer D);
all are shipped.

**F4 procedural macros (landed 2026-07-12):**
`macro name(string payload) comptime <body>` evaluates its one string argument
and body on the hermetic, step-bounded oracle and returns opaque `Ast` through
compile-time-only `meta::parseExpr` / `meta::parseStmts`. Backticks are raw
strings only in macro-call argument position. Expression splices require
`Ast(expr)`, clone on every use, carry the call-site span, receive full pass-2
resolve/check, reject generated macro-call re-entry (M24), and round-trip through
`--expand`. The corpus pins branching, variable-size construction, recursion,
import-fed payloads, diagnostics, and all four active engine lanes. See
`designs/complete/techdesign-procedural-macros.md`.

A compile-time layer where **you** author the source-to-source transforms, instead of the compiler
hardcoding them. The motivating case: a reflector like `[Route("/hello")]` above a controller method
should morph into the actual route-registration wiring — but the *rule* for that morph is written by
the author, in a rule layer, scoped to a namespace, not baked into the compiler. "Syntax-shaped
regex": you match over parsed structure, not a character stream.

**Shape — matcher + action.** A rule is a **matcher** (a syntactic shape that fires on a condition —
an annotation in a declaration position, its enclosing namespace/domain, a predicate on the class's
interfaces/bases) plus an **action** (inject code at a named anchor — a call at the top or bottom of
a constructor, statements at namespace scope). Sketch (illustrative, not final syntax):

```
rule addRoutes in Controllers {
    match  [Route(path)] on method m in class C : IController
    inject `this.app.addRoute(path, m)` at end of C.new
}
```

**The rule language is not a second language.** The matcher is a small declarative *shape* language;
the action half is **the base language itself, quasiquoted, with holes** (`path`, `m`, `C`) bound by
the match. The author writes rules in the language they already know — no syntax-tree API, no
Turing-complete macro sublanguage. This is the deliberate weight-limiting choice (the fork the design
turned on): keep the action side ordinary code-as-template.

**Layer and ordering.** Rules run **AST→AST, before lowering**, so injected code goes through the
*same front-end as hand-written code* — it inherits whole-program resolution (§12) and the §7
fixed-offset fast path. A rule-wired `addRoute(...)` compiles to the identical machine code a
hand-written one would: the magic is not merely reflection-free at runtime, it is **cost-identical**
to writing it by hand. The robust pass order is *resolve → match (predicates read resolved facts) →
inject → re-resolve/check the augmented tree → lower*; a syntactic-only matcher (direct base names)
can run in a single resolve to start. The constructor-insertion anchor already exists internally —
`$init` synthesis (§7.1) inserts field-init code into constructors; rules expose that same insertion
point to authors.

**Scope.** Namespace-scoped. §12's boundaries persist into the whole-program tree, so "fires only on
declarations in `Controllers`" keys off structure the compiler already maintains — no new scoping
mechanism.

**Transform-in-memory — "keep the magic."** Rules morph the in-memory compilation unit only; the
source on disk is unchanged and the injected code exists for the compile. The controller author
writes the annotation and the wiring appears (ASP.NET-attribute ergonomics), with no generated file
to read or check in — the same category as compiler-internal `$init`.

**Legibility (the gate treatment, §1/§16).** The magic is invisible at the *use* site but the *rule*
is explicit and greppable at the rule layer: explicitness moves **up a level**, from use-site to
rule-site, not away. A rule/transform layer left ambient is exactly the kind of thing that erodes the
static legibility the rest of the language protects, so the morph is opt-in and locatable. A
**`--ast-after-rules` expansion dump is a required window** — for debugging rules and for keeping the
"what does this mean" answer available even though the expansion never appears in source.

**Relation to `bind` / `inject` (§12.5).** Those keywords are reserved for DI-style wiring and are the
adjacent space; a routing/reflector rule system is plausibly what `inject` was reaching for, so
whether this *is* that feature or a sibling is an open call.

**Known costs (not blockers).** Diagnostic **span attribution** for synthesized nodes (already a soft
spot for `$init`/prelude-origin nodes) gets more visible; the matcher is a small but real
**mini-language** with its own parse/error story; and **matcher timing** (purely syntactic vs.
post-resolve semantic predicates) trades simplicity against reach. This is a distinct axis from the
data/DBMS thrust — an ergonomics/framework capability — so it is sequenced as a deliberate phase, not
drift. The substrate it rests on (§12 scoping, `$init` anchors, `--ast`) is stable, so the design
does not rot while it waits.

---

## 17. Implementation: two variants

Two user-facing variants with **non-overlapping jobs:**

- **AOT compiled** — the real, optimized path. Whole-program compilation gives the shape system
  its closed candidate sets and direct offsets. This is the answer for "I need speed."
- **Interpreted** — flexible, fast-iteration, REPL/scripting. It does **not** need to be fast,
  because AOT already is; it needs to be correct and flexible.

**JIT is deferred, not adopted.** It is fast (and the shape/inline-cache system is already
JIT-ready), but it is the highest-complexity option (deopt, tiering, runtime codegen) and, with AOT
already present, it buys speed AOT largely already provides. It can be added as a later tier on the
interpreter if a population that needs interpreter-flexibility-plus-throughput-without-AOT proves to
exist.

### Avoiding doing things twice

Both variants share a **single front-end and semantic-lowering layer down to one bytecode IR** —
parsing, resolution, type checking, shape/first-wins resolution all happen once, above the IR. The
interpreter executes the IR; the AOT backend lowers it to native (and is *allowed and expected* to
optimize freely below the IR, so "compiled" is not a faked interpreter-in-native). The IR is the
single source of semantic truth; only "execute an op" vs "emit native for an op" diverges.

The tradeoff accepted: whole-program assembly gives the optimizer full visibility but makes
incremental/separate compilation harder. Real OS threads in the interpreted variant require the
interpreter to commit to true parallelism (the place interpreted-mode parallelism gets expensive,
analogous to where JIT got expensive).

### Implementation status (what actually exists)

The C++ compiler front-end (lexer → Pratt parser → resolver/shapes → checker) lowers to one
register-machine **bytecode IR**, the single semantic truth. **Five execution paths** consume
it, differential-tested against a shared corpus so they agree by construction — **four
active** (oracle, IR, emit-C++, LLVM) plus the **frozen, reference-only ELF backend** (#5):

1. **Tree-walk oracle** — the reference semantics (walks the AST directly).
2. **Bytecode IR interpreter** — executes the IR.
3. **emit-C++ backend** — lowers the IR to freestanding C++, compiled by the system compiler;
   covers the whole language except the event-loop/system layer.
4. **LLVM backend** — **the primary AOT backend** — direct native object emission via the LLVM C++ API (`TargetMachine`,
   no external `llc`), portable across whatever triples LLVM targets. Reached full IR
   parity with the pure backend at Gate G1 (2026-07-06): objects, collections, closures,
   exceptions, the event loop, natives, async/await, sockets, HTTP, and — as of A-M6 — the
   same per-frame arena tier for scope-owned value-struct allocations (the two remaining
   value-struct churn leaks that distinguished it from the pure backend are closed). Process
   spawn (`sysSpawn`/`sysPidfdOpen`/`sysReap`/`sysKill` — the G-LANG-2 process half) landed
   2026-07-16 via `runtime/lv_proc.c` + the `lv_plat_*` process floor
   (`designs/complete/techdesign-spawn-llvm.md`); Windows targets reject it at compile time. A
   `PassBuilder` O2 module pipeline runs before object emission (`-O0`/`-O2` selectable);
   measured fast paths inline int/float `Arith`, `truth`/`Not`, fixed-offset field access,
   and checker-resolved dynamic calls rather than crossing into the runtime `.o` for each.
5. **Pure x86-64 / ELF backend — FROZEN, reference-only (`--emit-elf`, `src/X64Gen.cpp`).**
   The self-hosting-grade path: **our own machine-code emitter and ELF writer, no g++, no
   assembler, no linker, no libc.** It compiles the *whole language* — objects, collections,
   closures, exceptions, files, streams, the event loop, timers, async/await, sockets, HTTP —
   to a statically-linked binary that talks to the kernel through the `syscall` instruction
   only ("not a dynamic executable"). The runtime is emitted as machine code; values are
   tagged 16-byte pairs `[tag][payload]`; objects are heap records with the dispatchers
   (member access, operators, dynamic dispatch, `issub`) emitted as code. **As of the
   portable-backend pivot (2026-07-05) it is FROZEN and never extended** — see §0.

**ELF is not a project target.** The frozen backend is kept solely as the differential-testing
anchor and the eventual zero-dependency bootstrap seed; every feature that landed after the
pivot (`char`, `Block`, `enum`, `math`, threads, TLS, named args, method references, regex)
deliberately has **no ELF lane**, and **no design or task is ever gated on an ELF finding**.
The finished pre-freeze work described below (the §15 memory/COW story, the socket floor)
stays in place; it is history, not an active target. The `syscall` floor (§16 taken to its
terminus) is what makes the produced binaries libc-free — the property the backend was built
to demonstrate, now demonstrated.

Realized since: **§7's packed object layout** (fixed classId + typed slots, dynamic keys on a
fallback list) is built in both native backends, and **declared field access compiles to a
fixed offset** — a direct `mov [obj+16+slot*16]` in `$init` and in methods where the slot is
provably stable across the receiver's subclasses (the "as fast as a struct field" fast path,
~2.8× over the runtime lookup). The pure backend's **full §15 memory story is now wired into
codegen and the heap is bounded, not just accounted**: the scope-owned tier frees through a
per-frame arena reset at return; the escaping tier frees through retain/release on objects,
boxed arrays, closures, maps, value-struct arrays, and heap strings, plus `Op::VFree` for
provably-dead returned value-struct copies, backed by a real free-list allocator (power-of-two
size classes, reused rather than bump-only growth). **§11's COW-on-refcount is wired into the
indexed-write path** (in-place at refcount 1 for boxed arrays, dense arrays, and existing-key
map updates; pure copy when shared), with the stale-temp pre-release that makes it actually
fire in loops (§15). The differential churn corpus is **13/13 guarded programs green at +0
bytes** (one declared XFAIL: the dense IndexStore value-operand leak, §15) — every reference
kind is under the ARC discipline; the one remaining asterisk is true reference cycles
(§15, §19 #10). Still boxed: `Array<T>`
elements generally — the dense/columnar layout (§9 value
types) beyond arrays of plain structs is the next major work. The project/file system (§12.8)
and full Trident package manager (local + VCS deps, MVS/lock/store/integrity, vendoring,
publish/yank, optional proxy/index, and provenance policy) are also implemented and share this
same front end.

---

## 18. Decisions settled since the original list

- **Objects are references** (assignment/passing shares the handle); primitives are values.
  Lifetime management (§15) is the Rust-*mechanism* (scope-free + refcount + `unsafe`), not the
  Rust-*discipline* (no borrow checker); compile-time frees are best-effort escape analysis with
  refcount as the safety net.
- **Error handling: both.** Exceptions are the primary/common path (implemented: try/catch/throw,
  resolution by type through bases + interfaces, on all engines); `Result<T, E>` via unions is
  also supported by the type system.
- **Arrays are pure values** with COW-on-refcount **implemented** on the indexed-write and
  self-append paths (§11); `a[i] = v` is rebind-sugar that mutates in place when uniquely owned.
- **HKT committed** as a gated advanced feature (§9).
- **Method/function-level generics** — the uniform "type-param slots on any scope-opening
  entity" rule (§9). Singleton scopes (namespaces, class static sides) cannot hold `T`-typed
  state (§6.6).
- **Overload resolution by argument types** everywhere (§9).
- **`uses` imports**; **`bind`/`inject` DI** (§12.5).
- **Indexers** via `([])` accessors (§6.7); the symbolic-selector family (§5).
- **Primitive object mask**; stdlib written in the language over minimal intrinsics (§9, §11).
- **Escaping-tier ARC is complete in the pure backend** (§15): retain/release for objects,
  boxed arrays, closures, maps, value-struct arrays, and heap strings, plus `Op::VFree` for
  returned value-struct copies, backed by a real free-list allocator — the differential churn
  corpus is 13/13 guarded programs green at +0 bytes (one declared XFAIL, §15). The one
  remaining asterisk is true reference cycles (observed via timer-callback capture; not yet
  collected — §19 #10).
- **Project/file system + package manager** (§12.8): manifests, glob sources, function- or
  file-shaped entry points, local/VCS deps, MVS, lock/store/integrity, publish/yank, optional
  services, `as` aliasing, and phantom-dependency prevention — split across **`trident`**
  (package manager, owns `trident.toml`) and **`leviathan`** (pure compiler, consumes a build
  plan); the old language-literal `project.mf` is gone.
- **`char`, `Block`, `enum` value/reference types** (Track 03, §9): the object mask applied to a
  Unicode scalar, the gated fixed-length byte buffer, and the closed int-carried value type — all
  on the four active engines (no ELF lane; X64Gen frozen).
- **`namespace math`** (Track 06): `pi`/`e` + libm transcendentals + overloaded `min`/`max`,
  top-level (not `std::math`).
- **Named arguments + default parameter values** (§2, §9): compile-time normalization to a
  positional list; no runtime calling-convention change.
- **Method / function / constructor references** (§6.8, LA-25) and **runtime-slot method dispatch
  with static devirtualization** (§6.9) — both landed.
- **Threads / workers** (§14, Track 10): `spawn`/`Worker<T>`/`Channel<T>` with copy-always
  boundaries; true OS threads on LLVM.
- **TLS + crypto** (§13, LA-2): wrap-in-place TLS as an fd property, `sysRsaEncrypt`, crypto-grade
  `sysRandom` (oracle + IR + LLVM).
- **Metaprogramming Phases 1–2** (§16.5): attributes + rules (match/inject/quasiquote), expression
  macros, Layer-D body-`rewrites`, and comptime `import()` (LA-20).
- **Regex landed in full** (§11, Track 10): the in-prelude Thompson/Pike/lazy-DFA engine plus
  the public `Regex`/`Match`/`Group`/`RegexOptions`/`RegexException` C#-shaped surface and
  `namespace regex` conveniences.
- **Backend pivot settled** (§0, §17): LLVM is the primary AOT backend; the pure x86-64/ELF
  backend is **frozen, reference-only, and not a project target**.

## 19. Open decisions (carried forward)

1. **Primitive naming** — full words vs abbreviations (design leans full words).
2. **Generic defaults** — does every `T` have a default (for bare auto-construction)? A constraint
   question (`where T: default`-style).
3. **Modules as values** — only if unit selection is ever a runtime decision (planned
   `Bindings` objects, §12.5, would be a first instance of the module-as-value shape).
4. ~~**Eager vs lazy** data-operator execution (arrays lean eager).~~ **Resolved
   (Track 07):** arrays are eager, `Seq<T>` (via `array.asSeq()`) is the opt-in
   lazy form over the iterator protocol. See §11 and `docs/reference.md` §6.4.8–9.
5. **`async` coloring** — return-type-carried (`Promise<T>`, no coloring keyword) vs a signature
   `async` keyword (the contagious form). The anti-contagion trajectory favors the former.
6. **Cancellation** — cross-cutting through async/streams; design early.
7. **Exception design** — syntax/semantics of throw/try/catch; how exceptions and `Result`
   coexist; what `await` does with a rejected promise.
8. **Resource lifetime** — **resolved** (techdesign-02 F3, landed): `using Type name = expr;`
   (§5.2/§12.7 of reference.md, `IDisposable`/§6.6.65) gives deterministic scope-based release
   for sockets/files/streams, separate from plain-memory ARC — `close()` runs on every
   block-exit edge (fallthrough, return, throw-unwind, break/continue), reverse declaration
   order for multiple resources in one block. `File` is the prelude's first conformer.
9. **String encoding** — bytes vs characters at the I/O boundary; what `subStr` counts.
   **Partial answer (Track 03):** the byte/scalar boundary is now explicit rather than
   conflated. `length()`/`charAt`/`subStr`/`indexOf`/`byteAt` stay **byte-counted** (unchanged);
   the scalar view is opt-in through `char` — `string.at(i)` decodes the scalar *starting at
   byte offset `i`* (mid-sequence offset throws), and `string.chars()` full-decodes to
   `Array<char>` (invalid bytes → U+FFFD, never a throw). So "what does `subStr` count?" is
   answered "bytes, deliberately," with `chars()`/`at()` as the scalar-correct path. Still open:
   grapheme clusters, normalization, and a scalar-indexed `subStr` variant.
10. **Reference-cycle handling — resolved for declared back-edges (F5).** `weak T?` fields ship
    on all four active engines and break retained-tree / Timer-handler cycles without tracing.
    A supplementary collector for arbitrary all-strong cycles remains optional future work,
    not a blocker for the explicit ownership model.
11. **Interpreter parallelism** — true parallel threads vs a global lock.
12. **Raw pointers** — kept behind a directive, or cut once safe byte/buffer types cover the real
    uses; decide last, after the memory model and a bounded buffer type exist.
13. **Range variants** — exclusive ranges (`until`), stepped ranges, slice-assignment
    (`arr[a..b] = v`).
14. **Accessor return types** — indexer `get` currently has no return-type syntax; inference vs
    annotation.
15. **Compile-time rules (§16.5)** — the reflector/macro rule layer: exact matcher DSL surface,
    whether it subsumes `bind`/`inject`, syntactic-only vs post-resolve matcher timing, and the
    span-attribution story for synthesized nodes. Design converged (§16.5); unbuilt.
16. **JSON parse error detail** (Track 09 F1) — `json::parse` is total (malformed → `None`),
    which loses position/reason ("line 3: unexpected `,`"). A `json::parseVerbose ->
    JsonParseResult` (value? + error string + offset) is the roadmap answer; do **not** switch
    malformed-data to exceptions (expected-outcome rule, §12.6). Float rendering also wants the
    global cleanup (§6.11 renders `float.toString()`'s `42.000000` form for now).
17. **HTTP client maturity** (Track 09 F4) — connection pooling / keep-alive reuse on the client
    side, URL-string parsing, redirect following, and request timeouts are deferred to the
    framework era; v1 is one-connection-per-request with explicit host/port/path. Pipelining on
    the server is also unbuilt (keep-alive is buffered-but-serial).
18. ~~**Ship stdlib as files** — the prelude roughly doubled with Track 09 and is now four
    concatenated raw-string segments (`kPreludeCore/Std/Rest/Web`, Resolver.cpp).~~ **Resolved
    (owner ruling):** the stdlib ships as `.lev` files, not per-target in-binary segments.
    Moving the prelude to shipped source files is the goal — `parsePrelude()` gains a real
    file-reading seam; per-target selection (e.g. a wasm-only `kPreludeWasm`) is a packaging
    detail *within* that model (which files get shipped/loaded per target), not an alternative
    to it. See §18.

---

*This document reflects the design as worked out so far. It is a living specification; the open
decisions in §19 are the active edges. The syntax reference lives in `docs/reference.md`.*
