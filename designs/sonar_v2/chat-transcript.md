Overall verdict
The core architecture is right for this language. Retained-mode is the correct call — pure arrays + ARC punish per-frame tree rebuilds, and stable object identity is what Leviathan's memory model rewards. The two-grain damage model (component dirty flags + cell-level diff), Block as the frame buffer, structs for geometry, enums for the input decoder, and DI for theming are all the language's own idioms used for what they're for. The MI-mixin composition story (§5) is genuinely the strongest section — it's the best showcase of distinct I've seen in any design doc here.

But the design as written has four places where it assumes language capabilities that don't exist, and one of them is load-bearing.

Where the design conflicts with the landed language
1. Bound method references don't exist. §7 says menuItem.onSelect(this.save) and §8.5's rule injects this.$m. Reference §3.4 v1 limits are explicit: a bound reference (receiver captured) is not in the language — only unbound Editor::save (receiver becomes the first parameter) or a lambda. Every handler-registration example in the doc is written in a form that won't compile. The framework works fine with lambdas, but the ergonomics the doc is selling depend on a feature LA-25 deliberately excluded.

2. The sonar! macro can't be built on landed metaprogramming. Shipped expression macros substitute holes into a fixed quasiquote template; Layer B rules match declaration shapes. Neither can parse a tag grammar and emit variably-shaped construction code — that's a procedural macro (comptime code that returns code), which is not in Phases 1–2 or the shipped Layer D. Same problem for §8.3 id binding and §8.6 reactivity, which additionally need a macro to inject members into the enclosing class — member of is a rule anchor fired by attribute matches, not something an expression macro can reach. §8 as a whole is speced against machinery that doesn't exist.

3. Reference cycles make every Sonar app leak by construction. The doc's open question 2 understates this. parent_ + children arrays form cycles; the language has no weak references and no cycle collector (§19 #10, and the Timer→handler cycle is already observed and uncollected). "parent_ is a raw back-reference by convention" isn't expressible — a class field holding an object is a counted reference. Every modal closed, every tab removed, every subtree replaced leaks its whole subtree in a long-running TUI. A discipline workaround exists (remove()/clear() must null parent_, breaking the cycle at detach), but it's fragile.

4. There is no terminal floor. The run loop needs raw mode (termios), terminal size query, and resize notification (SIGWINCH or polling). None of these sys* natives exist, and the language has no signal-receiving surface at all. sysWatch on fd 0 for stdin bytes plausibly works (the recv path falls back to read(2)), but without raw mode you get cooked, line-buffered input and no arrow keys — a hard blocker before any code runs.

Smaller flags: the fluent-API story in §4 has an unresolved typing hole — if IContainer requires IContainer add(IComponent) and Container declares Container add(IComponent), whether that satisfies the requirement (covariant return) is unspecified, and same-name/same-arity members differing only in return type are unresolvable at call sites. I'd trim IContainer to queries (children(), dirty()) and keep all fluent verbs class-only. Also: App::Current() needs static-side declarations that have no plumbing yet (use a namespace global); BorderStyle::None collides nominally with the language's None (rename to be safe); KeyEventBox is the framework fighting the language — just make events classes (identity + mutable handled is exactly what reference types are for); and if Theme::FromToml parses via JSON instead, bug #30 blocks it on LLVM.

How I'd improve it
Ship v1 as the builder API only, as a trident package (not prelude segments — the prelude is unchecked and narrowing-hostile per the Track 09 lesson; a package gets full checking). The doc itself says templates expand to the builder, so the framework is fully usable without §8. Sequence: sonar-core now, sonar-templates when the macro capability exists.
An interim template path that is buildable today: comptime-parse the markup into a flat instruction array (the exact regex::compileProgram → Array<int> precedent) interpreted by a small runtime builder — but it can't carry handler refs or id binding, so I'd rather wait for real proc-macros than ship a gimped template layer.
Make detach (remove/clear/modal dismissal) formally responsible for breaking parent links, and have App.run() teardown walk the tree — documented as the cycle discipline until weak lands.
Add a hardware-cursor story (position/visibility for Input focus) — every TUI needs it and the doc never mentions it.
Language features to implement first, in order
Terminal floor natives — raw mode, size, resize signal, stdin watch. Small Track-08-shaped work; absolute prerequisite.
Weak references (weak fields) — resolves §19 #10; Sonar is the forcing function. Without it the framework's core data structure leaks.
Bound method references — the single biggest ergonomic win for handlers and the @Shortcut/template designs as written.
Procedural macro / computed-expansion capability — gates the entire template layer (§8), which is the framework's differentiator.
Block bulk ops (fill/copy/compare natives) — Surface.fill and the frame diff are per-byte interpreted loops today; cheap natives make the paint path viable on the interpreter lanes.
A covariant-return ruling for interface satisfaction — small checker decision that determines the fluent API's shape.
With 1–3 landed, this is a framework no other TUI ecosystem could replicate — the MI mixins, exhaustive-match input decoding, and DI theming are real, language-native advantages. With 4, the template layer becomes the standout. As speced today, though, §7–8 would not compile and §2's tree would leak; the design needs those revisions or the features above before implementation starts.

# Sonar — Pre-Design Research Findings

**Status:** research complete, pre-design. **Date:** 2026-07-12.
**Purpose:** ground-truth map (verified against the tree, file:line) for the 6 language
features Sonar needs and the framework's load-bearing dependencies, so the design docs can
be written against reality rather than the (often stale) design-doc headers. Produced by a
6-agent parallel code sweep + design-doc reading.

**Decision this session:** Sonar will **aggressively pursue the missing language features**
(no "hold for v1" fallbacks). Each of the 6 features gets its own language-side tech design;
the framework designs assume they land. This mirrors the Atlantis co-development model (that
framework's `request-<slug>.md` tickets + "design assuming the ask lands"), except we build
the features rather than ticket-and-wait.

---

## 0. Headline corrections to the earlier Sonar review

The first review of `overview-sonar.md` was written before reading the code. Ground truth
changes four of its claims:

1. **Terminal floor is NOT missing — raw mode is fully built.** `lv_plat_term_raw`/`_restore`
   ship on all four active backends with crash-safe restore-on-exit
   (`lv_plat_posix.c:85/99`, `lv_runtime.c:2197/2211`, `lv_entry.c:68`; surface
   `term::enableRaw()` at `Resolver.cpp:2558`). Only **winsize** and **signals (SIGWINCH)**
   remain. The `designs/complete/terminal-raw-mode.md` header ("nothing implemented") is
   **stale**.
2. **DI works.** bug.md #9 is gone; implicit injection resolves and lexical nearest-wins
   binds work (`Checker.cpp:1680/1712/1735/1856`, `Eval.cpp:1034`). `system-binds.md`'s
   "parse-only" note is stale. Only the `Bindings`-object layer is unbuilt
   (`reference.md:646`). **Value-type structs are rejected as binds** (`Checker.cpp:1684`) —
   matters for how a `Theme` is bound (bind an interface/reference class, not a struct).
3. **Field-stored closures are callable.** The old "field-closure dot-call" defect is fixed
   (bug.md now lists only #3/#35, P1 empty) — `box.onKey(handler)` lists and `obj.field(args)`
   handler dispatch are viable. (Confirm with a probe before relying on the *direct*
   `obj.field(args)` spelling; the LA-25 log used a `var h = obj.field; h(args)` workaround
   that may now be unnecessary.)
4. **Metaprogramming is Phases 1–4 complete**, not 1–2. Layer D rewriters, expression macros,
   `$for`, `meta.*`, 6 anchor kinds all landed. The `sonar!` gap is narrow and specific
   (§4 below), not "the whole template layer is unbuildable."

Two review claims **stand, validated**: the reference-cycle leak (parent↔children) is real
and unsolved (weak refs are the fix), and bound method references genuinely don't exist.

---

## 1. Feature: Weak references — HARDEST (ABI change)

**Why Sonar needs it:** the retained-mode tree is `parent_` (up) + `children` array (down) =
a reference cycle under ARC. info.md §15/§19#10 confirms cycles are observed-but-not-collected
(the Timer→handler cycle already leaks). Every closed modal / removed tab / replaced subtree
would leak its whole subtree in a long-running TUI. `weak parent_` is the fix.

**Current state (file:line):**
- Object header is `LvHeader = {int64_t rc; int64_t meta}`, 16 bytes at `payload-16`
  (`lv_abi.h:63-76`). **A strong count only — no weak count, no side table.**
- retain/release: `lvrt_retain`/`lvrt_release` (`lv_runtime.c:220/228`); at rc→0,
  `lv_recursive_free` (`:252-332`) frees children then `lv_free_raw` (which memset-poisons
  `0xFE` and returns the block to a free list, `:151-160`).
- Counted-kind gate: `lv_is_counted(tag,payload)` (`:200-216`) + heap-address-range check
  `lv_in_region` (`:92-97`) — the latter is info.md §15's mechanism that skips data-segment
  literals / arena strings.
- ARC is **implicit in op semantics**, classified by `destKind(Op)` (`LlvmGen.cpp:81-98`);
  stores that must skip retain for a weak slot: `RawSet` (`:2919`), `SetMember`/`lvrt_setm`,
  `StoreGlobal` (`:2940`), `CaptureVar`/`lvrt_capture_set`. Reads that must return None on a
  dead referent: `RawGet` (`:2484`), `GetMember`/`lvrt_getm`, `LoadCapture`.
- Interpreters (oracle+IR) use host `std::shared_ptr` (`RuntimeValue.hpp:39-56`) — **no manual
  ARC**; a weak slot is naturally a `std::weak_ptr` + `.lock()`→None on expiry.
- **No weak/cycle infra exists.** Every "weak" token in-tree is `std::weak_ptr` used by the
  ownership *liveness probe* (`MemVerify.cpp:14`), unrelated to a language `weak`.

**The core problem:** the header can't answer "is this referent still alive?" once freed —
the block is immediately reclaimed and poisoned. A weak read after free touches freed/reused
memory. So weak refs require **new liveness state on LLVM/native**: either
(a) a **weak count** in the header's `meta` word (or a 3rd word) so the block's *memory* is
retained until strong AND weak both hit 0, with the payload **tombstoned** (a dead flag) when
strong hits 0 so weak reads see "dead"; or (b) a **zeroing-on-free side table** keyed by
object id. Option (a) is the classic Swift/Rust `Weak` model and the recommended shape (the
`meta` word already exists; the free-list reuse in `lv_free_raw` must consult the weak count).

**Engine deltas:**
- **LLVM/runtime:** header layout (`lv_abi.h:63`), allocator/free (`lv_runtime.c:151/167`),
  retain/release/recursive_free (`:220-332`), new `lvrt_weak_load`/`lvrt_weak_store`,
  destKind wiring for weak slots (`LlvmGen.cpp:81-98`, store/read paths above).
- **Oracle+IR:** carry weak-ness in `Value` (new kind/flag, not the field map);
  weak slot holds `weak_ptr`, read = `.lock()`→`None`. "Just works" via host `weak_ptr`.
- **ELF/X64Gen:** frozen — feature doesn't ship there. Do not touch (`X64Gen.cpp:648-722`).

**Design decisions to make:** surface syntax (`weak T? field;` — must be optional, since a
weak read can yield absence); whether `weak` is field-only (like `readonly`) or also
locals/params; the tombstone-vs-sidetable choice; interaction with `distinct`/`const`/
`readonly` modifiers; whether reading a weak slot auto-narrows (it yields `T?`).
**Risk: HIGH** — touches the ABI header + allocator; the one feature that isn't checker-only
or additive-native.

---

## 2. Feature: Terminal floor (winsize + signals) — LOW (raw mode already done)

**Why Sonar needs it:** `App` must size its root (winsize) and redraw on resize (SIGWINCH).
Raw mode + byte-clean stdin + `sysWatch` fd-readiness are **already shipping** — the run loop
substrate exists.

**Already built (do not redesign):** raw mode all 4 backends w/ crash-safe restore; byte-clean
`sysRead(fd,max)->string` and `sysRead(fd,Block,max)->int` (`lv_runtime.c:2068/2092`, escape
bytes + NULs survive); event loop with `sysWatch`/`sysWatchWrite` (`Resolver.cpp:1310`,
`RuntimeLoop.cpp:32`), `sysTimerStart`, Promise/await, and **keep-alive while any watch/timer
is pending** (`lv_entry.c:59` `while(lvrt_loop_has_work())`; interp mirrors `Eval.cpp:1558`,
`IrInterp.cpp:736`).

**Missing, both fully pre-designed (proposals ready to become build tickets):**
- **Winsize** — `designs/terminal-winsize.md`. `lv_plat_term_size(fd,*rows,*cols)` via
  `ioctl(TIOCGWINSZ)` + native `sysWinSize(fd,field)` + `term::size()` with the `\x1b[6n`
  cursor-report fallback (in-language). Interpreters + LLVM + emit-C++; ELF defers.
- **Signals** — `designs/signals.md`. Signal-as-system-stream via `signalfd`/self-pipe (never
  a language-level handler): `sysSignalOpen/Next/Close` + `signal::on(sig) -> InStream<int>`,
  riding the existing `sysWatch`. Also the raw-mode SIGTERM/HUP terminal-restore safety.
  Note: raw mode already clears `ISIG`, so Ctrl-C arrives as byte `0x03` (input handled),
  but **resize detection (SIGWINCH) genuinely needs this**.

**Design work:** mostly promote the two existing proposals to accepted tech designs + wire
per the raw-mode precedent; refresh the stale raw-mode doc header to "landed." **Risk: LOW.**
Additive floor natives, established pattern.

---

## 3. Feature: Bound method references — LOW/MEDIUM (checker-only, one real decision)

**Why Sonar needs it:** ergonomic handlers — `menuItem.onSelect(this.save)`,
`box.onKey(this.onCursorMoved)`. LA-25 shipped *unbound* `Class::method`; bound `obj.method`
was explicitly deferred (§8.4).

**Current state (file:line):** LA-25 is **checker-only** — a value-position `::`-callable is
rewritten *in place* into an eta-expansion `Lambda` (`Checker.cpp:989` `rewriteAsMethodRef`,
gated `!e->colon return` at `:1064`), which every engine's existing lambda path runs. No
Lower/Eval/backend code. Closures capture visible locals **by name** (`Lower.cpp:1380`),
including `this` under the literal name `"this"` (`:1389`).

**The delta (bound = eta-expansion minus the receiver param, plus the receiver captured):**
- Parser: **none** — `obj.method` already parses to `Member{colon:false}` (`Parser.cpp:443`).
- Checker type hook: **`Checker.cpp:915`** — instance-`.`-method currently returns lenient
  `wrap(unknown())` (so `var f = obj.method` silently types `unknown` today, no error). Replace
  with the bound function type = method signature **without** receiver
  (`methodRefCanonical(m, /*recvCanon=*/"", ret)`, `:938`).
- Checker rewrite: a `rewriteAsMethodRef` variant that omits the `$mr0` receiver param and uses
  **`e->a` (the receiver expr) as the call's callee base** — the capture-by-name sweep then
  snapshots the receiver local for free (the free-function branch `:1002` already reuses `e->a`
  verbatim as a callee base).
- Gates to add the `!e->colon` instance case: `tryResolveMethodRef:1064`, `typeOfMember:829`,
  `typeOfBinary:1470`, `isDeferredMethodRefArg:1145`.
- Dispatch, all 5 backends, deferral machinery: **no new code** (reuses eta-expansion).

**The one real design decision (§8.4's capture/lifetime question):** for a **simple lvalue
receiver** (a local `obj`, or `this`), capture-by-name snapshots it correctly with zero new
lowering. For an **arbitrary-expression receiver** (`getFoo().method`, `a.b.c.method`), leaving
`e->a` in the lambda body **re-evaluates it per call** instead of snapshotting once — wrong.
v1 options: (a) restrict bound refs to simple-lvalue receivers (local/`this`/field-path with no
calls), error otherwise; or (b) hoist the receiver to a fresh local before the lambda (needs
statement context the in-place checker rewrite lacks — bigger change).

**Caveat inherited from §8.8 (matters for Sonar's class-typed tree):** dispatch on the captured
object's runtime class holds **only when the receiver's static type is an interface** (or the
method is overridden-below). A **concrete-class-typed** receiver binds *statically*
(`Checker.cpp:282` `resolveDispatch`). So `this.save` where `this` is a concrete controller
binds to that class's own `save`. Since Sonar components are class-typed with overridden
`paint`/`measure`, this interacts with virtual dispatch — see §7. **Risk: LOW/MEDIUM.**

---

## 4. Feature: Procedural macros — MEDIUM (three existing pieces, need wiring through comptime)

**Why Sonar needs it:** `sonar!(\`<App>…</App>\`)` must parse a **custom tag grammar** at
compile time and emit variably-shaped construction code. Today's expression macros are
**fixed-template substitution only** — a macro cannot inspect its argument and return different
code (`Rules.cpp:737` `expandMacroCall`; body is a parse-time-fixed `tmplExpr`,
`Parser.cpp:1247`; M22 single-splice `:649`, M24 no-re-entry `:816`).

**The three pieces that exist but aren't connected through comptime:**
1. **Raw payload exists** as an addressable byte range: a backtick lexes to one raw
   `QuasiLiteral` token (`Lexer.cpp:99`), payload = `file_.text[span+1..end-1]`. **But
   `QuasiLiteral` is not a primary expression** — accepted only as a macro-decl body
   (`Parser.cpp:1244`) or rule action (`:1314`). So `sonar!(\`…\`)` **won't parse today**.
2. **A string→AST engine exists** — the fragment parsers `parseStmtsFragment`/`Member`/`Expr`/
   `Items` (`Parser.cpp:1388-1552`) re-lex a `SourceSpan` and run a sub-`Parser`. **Not
   reachable from comptime** — `Eval.cpp` has no `Parser` handle, no `parse-as-code` native.
3. **The splice substrate is solved** — `Binding::exprNode` (`Rules.hpp:181`) splices an
   arbitrary pre-built subtree (`Rules.cpp:1914`); a computed tree can be assigned to `slot`
   at `Rules.cpp:826`, bypassing `cloneExpr`-of-fixed-template.

**The missing wire (the minimal new mechanism):**
- **New `VKind::Ast`** (or `Fragment`) in `RuntimeValue.hpp:35` to carry a parsed tree as a
  first-class comptime value (the *single largest missing piece* — `reify` (`Rules.cpp:514`)
  today emits **literals only**: int/float/bool/string/None/array/value-struct-ctor, never
  arbitrary control flow / calls / statements).
- **A `parse-as-code` comptime native** exposing the fragment parsers to `Eval.cpp` (parse a
  runtime-built string, not just a source slice — the parsers already take an arbitrary span).
- **A non-template macro body form** — a comptime function that receives the payload (raw
  string and/or `Ast`) and *returns* constructed code — plus **`QuasiLiteral` as a primary
  expression** yielding the raw payload string.
- **A `reify` sibling case**: an `Ast`-valued result splices its carried tree directly.
- Reconsider M22/M24 (fixed hole-count guards) for computed output.

**Design decisions:** the macro-body surface (`macro sonar!(payload) comptime { … return quote(...) }`
vs a dedicated `procmacro`); whether comptime code builds AST via the fragment parser only
(parse strings it assembles) or also via a quasiquote-with-holes builder API; hygiene/def-site
scoping for computed output (the existing clone-time passes apply); the `--expand` story (must
still round-trip). **Risk: MEDIUM.** Bounded and additive (no ABI change; comptime + parser +
one VKind), but it's genuinely new capability, not a wiring-only job. This is the framework's
differentiator, so it's worth doing well.

**Note:** much of Sonar's template layer can also be expressed with the *already-built* Layer B
rules + `@Shortcut`/`@Timer`/`@Validator` attributes + `$for` (the §8.5 examples). The `sonar!`
tag macro (§8.1–8.4) is the part that needs procedural macros; the attribute-rule wiring does
not.

---

## 5. Feature: Block bulk ops (fill/blit/equals) — LOWEST (additive natives)

**Why Sonar needs it:** `Surface` is a `Block` cell buffer written thousands of times/frame,
and `present()` diffs the frame against the previous. Per-byte interpreted loops for
fill/copy/compare would be too slow; native `fill`/`blit`/`equals` fix it.

**Current state (file:line):** Block = tag 11, body `{parentPtr@0, off@8, len@16, dataPtr@24}`
(`lv_abi.h:144`), natives `lvrt_block_*` (`lv_runtime.c:1360-1465`), interpreter `BlockData`
(`RuntimeValue.hpp:28`), CGen `BlockData` (`CGen.cpp:44`). Slice **aliases** the root
(`lv_runtime.c:1411`, retains root); effective base = `dataPtr+off` (native) / `bytes[off+i]`
(interp). Bounds checks throw via `lvrt_raise_oob` (`:2286`).

**Recipe for a new bulk native (methods → no new IR op):**
1. Signature in `class Block` at `Resolver.cpp:120-132`.
2. C body in `lv_runtime.c` near `:1465` (bounds-check against each view's own `len@16`,
   `lv_block_at` addressing; **`blit` uses `memmove`** for overlapping slices of one root;
   `equals` returns `LV_BOOL`).
3. Clause in `RuntimeNatives.cpp:565` (covers oracle **and** IR at once).
4. Clause in `CGen.cpp:419`.
5. LLVM triple: enum+`fn(...)` (`LlvmGen.cpp:159/270`), `else if` row in `emitNativeRows`
   (`~:1748`), **and add the name to `kCovered[]` (`:1773`)** (must stay in sync or the call
   falls through to class dispatch and fails).
6. Corpus under `tests/corpus/blocks/`.

**Design decisions:** exact surface (`fill(off,len,byte)`, `blit(dstOff, src, srcOff, len)`,
`equals(other)` / `equals(off, other, otherOff, len)`); overlap semantics (memmove); whether a
frame-diff helper (emit escape bytes for changed cells) belongs in the language or in Sonar's
`.lev` (recommend Sonar — keep the native surface minimal, just the primitives). **Risk:
LOWEST.** Pure additive natives, established pattern, no ABI/IR/checker change.

---

## 6. Feature: Covariant-return interface satisfaction — LOW checker change, SUBTLE interaction

**Why Sonar needs it:** fluent builder APIs — `IContainer add(IComponent)` required by the
interface, but `Container add(IComponent)` on the class so chains stay concrete-typed and don't
decay to `IComponent` mid-expression (`box.add(a).add(b)` returns `Container`).

**Current state (file:line):** `Container add()` satisfying `IContainer add()` is a **hard
compile error today**. Interface satisfaction matches on the **full canonical signature string
including the return type**, by exact `==` (`Resolver.cpp:5277-5285`, esp. `:5280`); a method
slot's canonical embeds `) -> <return>` (`:5138-5144`). Covariant return → different string →
`"'Container' does not satisfy interface: missing 'add : (IComponent) -> IContainer'"`. The
needed subtype predicate **already exists** — `Checker::assignable`/`isSubclass`
(`Checker.cpp:296-318`, `212-219`, and `isSubclass(Container, IContainer)` is already true) —
but it lives on `Checker` and takes `Type`/`Symbol`, while satisfaction lives on `Resolver` and
compares canonical **strings**; `Slot` (`Symbols.hpp:35`) carries only the canonical string, no
structured return `Type`.

**The ruling is one line (`Resolver.cpp:5280`)** — replace whole-string equality with a
structural match: same name, same param canonicals, **return assignable** (provided return
assignable to required return) — BUT it must reconcile with three constraints:
1. **`mergeSlot` (`Resolver.cpp:5152`) is unchanged** — the covariant `(IComponent)->Container`
   and the inherited requirement `(IComponent)->IContainer` have different canonicals, so they'd
   coexist as **two `add` slots of the same arity**...
2. ...which then trips the **§3.4a "shares its arity" runtime-dispatch compile error**
   (`Checker.cpp:269-291`) at any call site where `add` is overridden below. So the ruling must
   decide whether the covariant method **replaces/collapses** the interface requirement slot
   (the clean answer: an interface requirement allocates nothing — §8 — so it should not
   produce a second runtime slot at all) or coexists.
3. **Runtime dispatch is name+arity only** (`IrInterp.cpp:27-47`), so return type can never
   disambiguate at runtime — consistent with why §3.4a rejects same-arity siblings; the fix is
   to ensure only **one** `add` slot survives.

**Design decision:** the cleanest framing — since interfaces allocate nothing (info.md §8), an
interface requirement should be **satisfied-and-consumed** by an assignable-return class method,
contributing no separate slot. That keeps one `add` slot (the class's), dispatch unambiguous,
and covariance a pure checker relaxation. **Risk: LOW** on the checker change itself; the
subtlety is entirely in the mergeSlot/dispatch reconciliation, which the design must state
explicitly. **Alternative for Sonar to sidestep entirely:** trim `IContainer` to queries only
(`children()`, `dirty()`) and keep all fluent verbs class-only (the review's original
suggestion) — then no covariant return is needed. The design should weigh "make the language
covariance-capable" vs "shape the framework interfaces to not need it." Given the aggressive-
pursuit directive, do the language feature, but note the framework can ship without it.

---

## 7. Framework-dependency findings (verified, affect the Sonar designs)

- **DI (theming/renderer):** works. Bind an **interface or reference class**, never a value
  struct (`Checker.cpp:1684` rejects struct binds). So `ITheme`/`IRenderer`/`IFocusPolicy` binds
  are fine; a `Style` **struct** cannot be bound (pass it, don't bind it). Lexical nearest-wins
  gives the scoped-rebinding a `Modal` subtree wants (§9) for free. `Bindings`-object install
  (`bind someBindings;`) is **unbuilt** — Sonar must use lexical factory binds.
- **Event loop / run loop:** `App.run()` is viable today — `sysWatch(0, cb)` for stdin,
  `sysTimerStart` for spinner/blink/`@Timer`, keep-alive while watches pending. **No SIGWINCH
  yet** (feature #1's signals half) — until it lands, resize is undetectable except by polling
  `term::size()` on a timer (ugly interim; the design should assume signals land).
- **Field-stored handler lists:** `Array<() => void>` fields + calling them works (bug fixed).
  Confirm the *direct* `obj.field(args)` spelling vs the `var h = obj.field; h(args)` binding
  with a probe before finalizing the events design.
- **Concrete-class dynamic dispatch (the big one for a retained tree):** the language dispatches
  concrete-class-typed receiver calls **statically** (only interface-typed receivers dispatch on
  the runtime object — `Checker.cpp:282`, reference §3.4a, LA-25 §8.8). Sonar's tree holds
  children as `IComponent` (interface) → **`c.paint(s)` on an `IComponent` dispatches to the
  runtime component's override correctly.** This is why the type inventory's interfaces
  (`IComponent`/`IContainer`) are load-bearing, not decorative: storing children as the concrete
  class would statically bind and break virtual paint/measure. The design MUST hold children and
  traverse via interface types. (`proposal-class-method-dispatch.md` — if it lands — would make
  concrete-class calls dynamic-with-devirtualization and remove this constraint, but Sonar
  should not gate on it.)
- **`bug.md #35` (P2):** a bare top-level `Promise` awaited inside a `spawn` body misbehaves on
  LLVM. Not a blocker for a single-threaded TUI, but if Sonar ever offloads work to a `Worker`,
  keep promises out of the global-across-spawn shape.
- **`Map.with/.without` by-name (bug #18):** missing on LLVM/ELF — use `m[k]=v` bracket sugar
  in Sonar (already the documented idiom). Affects `Theme`'s `Map<string,Style>`.
- **enum on all engines incl. LLVM** (no ABI tag) — `Color`/`KeyCode`/`BorderStyle` etc. are
  fine; exhaustive `match` for the input decoder works.

---

## 8. Sonar overview (`overview-sonar.md`) changes to make (validated against ground truth)

1. **Rewrite all handler examples** — `menuItem.onSelect(this.save)` etc. depend on **bound**
   method refs (feature #3), not the shipped unbound form. Keep them, but note they gate on the
   bound-ref feature (which we're building). Where a receiver is non-trivial, use a lambda.
2. **`sonar!` template layer (§8)** — reframe as gating on **procedural macros** (feature #4);
   the `@Shortcut`/`@Timer`/`@Validator` rules (§8.5) and `$for`/`id`-binding can partly use the
   *already-built* Layer B/D machinery; the tag-grammar macro (§8.1–8.4) needs feature #4.
3. **Reference cycles (§13 Q2)** — upgrade from "open question" to "solved by `weak parent_`
   (feature #2)"; state the detach discipline as the interim until weak lands.
4. **Terminal floor** — add a "run loop substrate" note: raw mode + stdin watch + loop already
   ship; only winsize + SIGWINCH are new (feature #1). Add a **hardware cursor** story
   (position/visibility for `Input`/`TextBox` focus) — the overview never mentions it and every
   TUI needs it (likely just `\x1b[` writes, no new native).
5. **Fluent API typing (§4)** — either adopt covariant return (feature #6) OR trim `IContainer`
   to queries + class-only verbs. Recommend documenting the covariant-return dependency and the
   fallback.
6. **Events as classes, not `KeyEventBox`** — make `KeyEvent`/`MouseEvent` **classes** (identity
   + mutable `handled` is exactly what reference types are for); drop the struct+box contortion.
   (Struct events would copy and lose `handled` propagation.)
7. **`BorderStyle::None`** collides nominally with the language `None` unit — rename (e.g.
   `BorderStyle::NoBorder`) to be safe.
8. **`App::Current()`** static-side call has no plumbing (no `static` keyword) — use a namespace
   global / injected `App` instead.
9. **`Theme::FromToml`** — if it parses via JSON, JSON is LLVM-blocked (bug #30); parse TOML
   in-language or defer. Also `Theme`'s `Map<string,Style>` — `Style` is a struct **value** in a
   Map, which is fine (Map holds values); just use bracket-sugar writes.
10. **`bind ITheme => Theme::FromToml(...)`** binds a **reference class** `Theme` returned by a
    factory — fine. Do NOT `bind` a `Style` struct.

---

## 9. Design conventions to follow (the Atlantis template)

`designs/atlantis/techdesign-00-overview.md` is the model for a framework design-set here:
- **00-overview anchor** + numbered track docs with **disjoint ownership**.
- **Frozen cross-track contracts** (C1…Cn) — namespaces, the core shapes (component/container/
  layout interfaces), the event/dispatch model — changed only by escalation.
- **Gates + roadmap** with dated milestones.
- **STOP protocol** — framework tracks are pure `.lev`, never touch `src/**`/`runtime/**`; a
  language gap becomes a request ticket. **For Sonar, the 6 features ARE the language asks —
  but we're building them, so they get real language-side tech designs (in `designs/`, not
  `designs/sonar/`), and the framework docs assume them.**
- **Verified-syntax cheat sheet** for design authors.
- **Implementation log (append-only)** per doc.
- Files are `.lev`; distributed via **Trident** as a package; `designs/sonar/` for framework
  docs, completed → `designs/sonar/complete/`.

**Proposed deliverable structure (to confirm before writing):**
- **Language side** (`designs/` + `designs/request-*.md` tickets): 6 tech designs —
  `techdesign-weak-references.md`, `techdesign-terminal-winsize.md` (+ promote `signals.md`),
  `techdesign-bound-method-references.md`, `techdesign-procedural-macros.md`,
  `techdesign-block-bulk-ops.md`, `techdesign-covariant-return.md`.
- **Framework side** (`designs/sonar/`): revise `overview-sonar.md` per §8; then a numbered set
  — 00 overview/contracts/gates, 01 core (Component/Container/damage/Surface), 02 layout,
  03 events/focus/input, 04 the component library, 05 the `sonar!` template layer + rules,
  06 DI/theming/renderers, 07 the run loop + terminal integration, 08 testing (TestRenderer
  snapshots), plus advanced features (compile-time reactivity §8.6).

---

*Research complete. No design written yet — awaiting go-ahead to produce the 6 language tech
designs and the full Sonar framework design set.*
