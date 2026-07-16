# Sonar — Tech Design 07: Attribute Rules (`@Shortcut`, `@Timer`, `@Validator`)

**Status:** design, pre-implementation. **Date:** 2026-07-12. **Track:** T07.
**Owns:** `sonar/src/attributes.lev`.
**Depends on:** T01 (Component lifecycle), T09 (`App.keymap()`, `App.every`); **NO unlanded language features** — Layer-B rules, attributes, ctor anchors are all landed (metaprog Phases 1–4). F3 (bound refs) upgrades the injected code's spelling; lambda spellings work today.
**Gates:** none — **buildable immediately**; ships with G-S3. **Difficulty:** S/M, risk LOW.

Conforms to anchor: C1 (namespace), C9 (Keymap/App surfaces), R7 (lifecycle discipline), cheat-sheet §7 (attribute args are positional comptime-const int/float/bool/string; rules fire only in files importing the rule's namespace).

---

## 1. Design position

The attribute layer is Sonar's declarative wiring, built ENTIRELY on landed metaprogramming — it is deliberately independent of the F4-gated template layer, so enterprise apps get `@Shortcut`-grade ergonomics at G-S3, not G-S4. Everything here is additive, hygienic, and visible under `--rules` / `--expand`.

```lev
class Editor : ContentBox {
    @Sonar::Shortcut("^S") void save() { ... }
    @Sonar::Timer(250)     void blink() { ... }
}
```

Rules fire only in files that import `Sonar` (`uses Sonar;` / selective `use`) — the landed scoping rule; stated in user docs so a "why didn't my shortcut bind" has a first answer.

## 2. The attributes

```lev
namespace Sonar {
    attribute Shortcut  { string chord; }          // methods of Component subclasses
    attribute Timer     { int intervalMs; }        // methods of Component subclasses
    attribute Validator { string message; }        // methods: bool m() on IValidatable classes
}
```

Attribute args are positional comptime constants (landed limits): `@Sonar::Timer(250)` — the sketch's `"250ms"` duration string is REJECTED in favor of a plain int (no parse ambiguity, no runtime parsing; a `Timer("250ms")` misuse fails at the attribute type check). Deviation from the sketch, deliberate.

## 3. The rules (normative injected shapes)

### 3.1 `@Shortcut`

```lev
rule bindShortcuts {
    match @Shortcut(s) on method m in class C : Component
    inject `this.__sonarBindShortcut($s.chord, () => this.$m());`
    at bottom of C.constructor
}
```

The injected call goes through a Component-level helper rather than splicing keymap logic inline — one-line injections keep `--expand` readable and centralize the semantics:

```lev
// on Component (T01 hosts the field; this track specs it):
void __sonarBindShortcut(string chord, () => void action) {
    pendingShortcuts_ = pendingShortcuts_.add(ShortcutReg(chord, action));
}
```

**Binding lifecycle (the design decision):** the constructor runs before the component is attached or any app exists, so binding directly at ctor time is wrong. Registrations are **pending on the component**; `onAttach` flushes them into `Sonar::app().keymap()` (tokens recorded); `onDetach` unbinds. This makes shortcuts scope to *mounted* UI — a detached editor's `^S` stops firing — which is the correct enterprise semantics (and the R7 discipline applied to keymaps). Component's attach/detach participation: T01's hooks are open methods; this track's helpers ride them via the base implementation (Component owns `pendingShortcuts_`/`boundTokens_` fields and the flush/unbind logic — a T01 contract addition flagged in §6.1).

Duplicate chords: `Keymap.bind` throws on duplicates within one keymap (anchor C9) — two mounted components binding `^S` is therefore LOUD at attach time, with the exception naming the chord. Apps wanting contextual reuse mount/unmount (the lifecycle scoping above makes that natural).

With F3 landed, the injected template's lambda `() => this.$m()` may become `this.$m` — a rule-template one-line change; both spellings are semantically identical (eta-expansion ruling). Not worth gating on.

### 3.2 `@Timer`

```lev
rule bindTimers {
    match @Timer(t) on method m in class C : Component
    inject `this.__sonarRegisterTimer($t.intervalMs, () => this.$m());`
    at bottom of C.constructor
}
```

Same pending/attach/detach lifecycle: `onAttach` → `token = Sonar::app().every(intervalMs, action)`; `onDetach` → `cancelEvery(token)`. This kills the classic leak (a detached spinner ticking forever) by construction — the exact Timer→handler cycle info.md §15 observes, defused by lifecycle rather than by waiting for F5. Interval must be ≥ 1 (attach-time throw otherwise).

### 3.3 `@Validator`

```lev
rule bindValidators {
    match @Validator(v) on method m in class C : Component
    inject `this.__sonarAddValidator(() => this.$m(), $v.message);`
    at bottom of C.constructor
}
```

`bool m()` methods become entries in the component's validator list; `IValidatable.validate()` (T04 components implement it) runs field-intrinsic checks (Input's own validator, RadioGroup's requireSelection) THEN the attribute-registered list; first failure wins and `validationMessage()` returns its message. A form-level `Sonar::validateTree(IComponent root) -> Array<ValidationFailure>` namespace helper walks the subtree collecting failures (`struct ValidationFailure { string message; }` + the component ref as a class — walk yields components via `is IValidatable` narrowing).

## 4. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | attributes + the three rules + Component pending/flush plumbing (with T01) | M |
| M2 | validateTree + ValidationFailure walk | S |
| M3 | `--rules`/`--expand` verification corpus + duplicate-chord/interval error tests | S |

## 5. Potential issues & mitigations

1. **T01 contract addition** (pending-registration fields + flush in attach/detach) — flagged to the anchor log; additive to C7, no signature changes (private fields + one helper trio). Coordinate before T01 implementation freezes.
2. **Rule fires in classes with no constructor** — the landed engine synthesizes one (phase-2 behavior); relied upon, tested.
3. **Multiple attributes on one method** (`@Shortcut("^S") @Timer(1000) void poll()`) — both rules fire independently; legal; tested.
4. **Inheritance**: a base class's `@Shortcut` method inherited by a derived class — the rule matched the BASE declaration; the injected ctor line lives in the base ctor, which derived ctors invoke explicitly (language rule). Derived overrides of the method: the pending registration captured `() => this.$m()` — dispatch follows the landed runtime-class rule, so the OVERRIDE runs. Correct by construction; documented + tested.
5. **`Sonar::app()` absent at attach** (component attached before `run()` — e.g. building the tree, then `app.run()`): `app()` throws only when no App EXISTS; App's constructor sets `currentApp` (T09) precisely so pre-run attach works. Pinned as a T09 ordering requirement.
6. **Hygiene**: `__sonar`-prefixed helper names avoid user collisions; rule-injected code is qualified per landed def-site scoping. Standard machinery, no new risk.

## 6. Testing plan

Corpus programs (runnable at G-S3, zero terminal, via TestRenderer + pumpOnce): shortcut fires on scripted chord; shortcut stops after detach; duplicate chord throws at attach naming the chord; timer ticks N times then detach stops it (count assertion); validator ordering + message surfaces; inherited-shortcut override dispatch; `--rules` firing report and `--expand` round-trip of a representative file. Differential oracle/IR/LLVM.

## 7. Open questions

1. Chord *display* helper (menu items showing a shortcut sourced from the attribute) — needs attribute reflection at runtime, which doesn't exist; v1 duplicates the string in `MenuItem.chord`; a comptime mirror table is a v2 idea.
2. `@Shortcut` on namespace functions (app-global, no component lifecycle) — v1.1; needs an app-attach story for non-components.

## 8. Implementation log

- 2026-07-12 — design written; not started. No language gates — first implementable track after T01–T03.
- 2026-07-14 — **LANDED IN FULL** (`sonar/src/attributes.lev` + the Component
  plumbing addition). All three attributes (`Shortcut`/`Timer`/`Validator`) and
  their three Layer-B rules ship exactly as §3 spells them; `--rules` confirms
  each fires at every annotated site (verified: 8 expansions across the test
  corpus — 3 shortcuts, 2 timers, 3 validators). `validateTree` +
  `ValidationFailure` (§3.3) walk a subtree running intrinsic
  `IValidatable.validate()` first, then the attribute-registered list,
  first-failure-per-component-wins. **Verified byte-identical on oracle, IR,
  and LLVM** (`sonar/tests/attributes/`): pending-until-attach, flush on attach,
  shortcut/timer fire → method runs, detach unbind/cancel, `@Validator` message
  surfacing + ordering, **multiple attributes on one method** (§5.3), and
  **inherited-`@Shortcut` override dispatch** (§5.4 — the captured
  `() => this.act()` runs the derived override, confirmed).

  Deviations, all deliberate, none a language change:
  - **§6.1 T01 contract addition** — the pending/flush plumbing for shortcuts
    and timers (`pendingShortcuts_`/`__sonarBindShortcut`/`__sonarRegisterTimer`
    + the attach/detach flush) was **already landed by T04** exactly as this
    doc specifies, so only the `@Validator` half was new. `__sonarAddValidator`
    + its two parallel carrier arrays + `__sonarRunAttrValidators` were added to
    `component.lev` alongside this track (the sanctioned additive C7 addition).
    Validators register directly at construction (no attach/detach flush — a
    validator closure holds no App-side resource), consulted by `validateTree`.
  - **Validator storage** is two parallel arrays (`attrValidators_` +
    `attrValidatorMessages_`), not a Map/`Array<struct>`, dodging the
    collection footguns (#40/#41/#47), matching `theme.lev`'s precedent.
  - **`ValidationFailure`** carries `message` + the failing `IComponent`
    ("the component ref as a class", §3.3); a value struct with a
    reference-class field (no enum/nested-struct field, so clear of #41).
  - One checker wart worked around at the CALL site only (no compiler change):
    an `Array<ValidationFailure>` return does not initialize an identically-
    typed but differently-*qualified* annotation
    (`Array<Sonar::ValidationFailure>`); consumers bind the result to `var`.
    Filed as a low-priority observation, not gated on.
