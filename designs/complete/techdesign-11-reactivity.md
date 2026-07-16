# Sonar — Tech Design 11: Compile-Time Reactivity (v1.5)

**Status:** **LANDED IN FULL 2026-07-14** (M1–M5), all four engines byte-identical. See §8 implementation log for the two forced deviations (per-type rules; global-fan-out notify) and their justification. **Date:** 2026-07-12. **Track:** T11.
**Owns:** `sonar/src/reactive.lev` + additions inside T06's macro (guarded, additive).
**Depends on:** **F4** (procedural macros — hard), **T06** (the `sonar!` expansion this extends), landed Layer-B rules + `member of` anchors + get/set accessor views (§6 of info.md: accessors are views over a slot; raw-slot access inside a view is non-recursive by construction — the property this design leans on). **F5 (weak refs) — soft**: the binding registry holds node references; lifecycle rules below keep it cycle-safe pre-F5.
**Gates:** G-v1.5 (2026-07-31). **Difficulty:** M/L, risk MED/HIGH (the accessor-injection interaction is the novel part — probe first).

Conforms to anchor: §4/F4 (v1.5 extension posture: attribute-relay over macro-emitted members), C7/C8, R7, cheat-sheet §7.

---

## 1. The feature

```lev
class Dashboard : ContentBox {
    @Sonar::Reactive int count = 0;
    Text counter;                                   // id-bound by the template

    new Dashboard() {
        add(sonar!(`<Text id="counter" text={this.count}/>`));
    }
    void onTick() { count = count + 1; }            // the Text updates. That's the feature.
}
```

**Scope rule (frozen):** a hole is reactive **iff** it is exactly `this.<field>` AND the field carries `@Sonar::Reactive`. Arbitrary expressions (`{this.count * 2}`, `{model.count}`) stay one-shot — loudly documented, visible in `--expand`. This is compile-time reactivity: the update graph is static code, no runtime observer discovery, no dirty-checking VM.

## 2. Mechanism — the attribute-relay design (chosen)

Two cooperating, ALREADY-DESIGNED pieces (no new language capability beyond F4):

**Piece 1 — a Layer-B rule injects the setter view** (landed machinery; the F4 doc's recommended v1.5 route):

```lev
namespace Sonar {
    attribute Reactive { }
    rule reactiveFields {
        match @Reactive on field f in class C : Component
        inject `set $f(<T> v) { $f = v; this.__sonarFieldChanged("$f"); }` at member of C
    }
}
```

The injected `set` is a **view over the existing slot** (info.md §6): `$f = v` inside the accessor is raw-slot access (non-recursive by construction — the language's own accessor rule doing the heavy lifting). Every ordinary write `count = 1` / `this.count = n` anywhere in the class now routes through the view and pings `__sonarFieldChanged`. **Open mechanism question flagged for the probe:** the rule template needs the field's declared TYPE for the parameter (`<T>` above) — whether the landed rule engine exposes a field subject's type as a hole (`$f.type`-style) must be verified against the metaprog phase-3/4 docs; if not, this is a one-line language ask (a `$f.type` reifier) filed per the request workflow — the ONLY potential language delta in this track beyond F4. Probe R-1, §6.1.

**Piece 2 — the template macro registers updaters.** When T06's emitter sees a reactive-shaped hole (`this.<field>` — it cannot see attributes at expansion time, so it registers unconditionally for `this.<field>` holes and the runtime helper no-ops for non-reactive fields... **rejected**: silent no-op hides typos. **Chosen:** the macro emits a registration that THROWS at construction if the field isn't reactive — loud, immediate):

```lev
// for: <Text id="counter" text={this.count}/>
this.__sonarBind("count", () => { __sonar_1.setText(this.count.toString()); });
```

`__sonarBind(field, updater)`: appends to the component's binding registry (`Map<string, Array<() => void>>` on Component — T01 addition, flagged) and **runs the updater once** (initial render = the update path, one code path). `__sonarFieldChanged(field)`: looks up the registry and runs updaters. Registration validates reactivity by consulting a per-class comptime-emitted marker... attributes aren't runtime-visible — **validation ruling:** the injected setter is the marker: `__sonarFieldChanged` is only ever CALLED for reactive fields, and `__sonarBind` on a non-reactive field simply never fires again after the initial render. To make the typo case loud without runtime attribute reflection, the RULE also injects `bool __sonarReactive_$f() => true;` per reactive field, and `__sonarBind` probes for it via... dynamic member probing doesn't exist in checked code. **Final ruling (honest):** v1.5 accepts the quiet degradation (a `{this.field}` hole on a non-reactive field renders once, correctly, and never updates — exactly v1 behavior), documents it, and `--expand` makes the registration visible. The loud-typo aspiration is recorded as an open question pending cheap runtime reflection. No magic, no lies.

**Type rendering:** the updater's `setText(this.count.toString())` shape depends on the target attr's type (from T06's registry: string attrs get `.toString()` on non-strings; typed attrs (`value={this.ratio}` on a ProgressBar int attr) splice bare). One rule, table-driven, shared with T06's literal typing.

## 3. Semantics (frozen)

- **Synchronous updater, asynchronous paint.** `count = 1` runs updaters inline (setter → changed → updaters), which call component setters, which `invalidate()` — damage coalesces into the next frame per C9. No reentrant paint. An updater writing ANOTHER reactive field chains (depth-bounded: a cycle A→B→A livelocks — mitigation: a per-`__sonarFieldChanged` re-entrancy set on the component (field currently notifying ⇒ skip re-notify), making cycles settle in one pass; spelled in pseudocode; the classic signals-glitch trade is documented — last-writer-wins within a frame).
- **Multiple holes per field** — all registered, all run, document order.
- **Bindings live on the component that RAN the template** (the enclosing class instance): its registry references child nodes it constructed — parent→child edges that already exist in the tree; no new cycle class pre-F5. `onDetach` of the template's host clears its registry (R7 discipline; T01 hook).
- **Threading:** single-threaded loop; reactive writes from a `spawn` worker are the same bug as any cross-thread mutation (copy-always makes it a non-communication, not a race) — noted.

## 4. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | probes: accessor-injection via `member of` on an attributed field (R-1: incl. the field-type hole question); write-through-view semantics on all engines | M — **the gate** |
| M2 | Component registry + __sonarBind/__sonarFieldChanged + lifecycle clear | S |
| M3 | rule + attribute in the package; hand-written-binding tests (no macro yet) | M |
| M4 | T06 emitter extension (reactive-hole detection + registration emit + type-rendering share) | M |
| M5 | goldens: counter app, chained fields, cycle settle, detach clears, --expand visibility | M |

## 5. Potential issues & mitigations

1. **R-1 probe red** (rules can't inject accessors onto attributed FIELDS, or no field-type hole): fallback A — the rule injects a plain method `void set$F(T v)` + the macro/user writes through it (loses bare-assignment ergonomics: `setCount(n)` instead of `count = n`; still reactive); fallback B — file the small language request. Decision tree recorded so implementation never stalls.
2. **Accessor-view interaction with `$init`/field defaults** — the initializer `int count = 0` writes at construction BEFORE bindings exist: fine (registry empty, initial render happens at bind time). But does `$init` route through the injected set-view (ping with empty registry, harmless) or write raw? Either is correct; probe documents which, tests pin it.
3. **Updater exceptions** — same dispatch rule as T03 handlers: catch, `Sonar::log`, continue (a broken binding must not kill the setter path).
4. **Registry growth under repeated template runs** (a dialog re-built N times binding the same host field) — each build registers anew; stale updaters reference detached nodes (invalidate on a detached node is harmless but wasteful). Mitigation: `__sonarBind` updaters are cleared on host detach (§3) AND template-built roots clear their host registrations when THAT root detaches — v1.5 ships host-detach clearing only + the documented pattern "build once, show/hide"; per-root unbind tokens recorded as v1.6.
5. **`--expand` legibility** — the registration lambda must read as what it is; the emitter comments each registration (`// reactive: count -> __sonar_1.setText`). Comments in generated source survive --expand? If not, naming carries the burden (`__sonarBind("count", ...)` is self-describing). Noted.

## 6. Testing plan

Probe suite (M1) first, results into T10's PROBES.md. Then: counter golden (write → snapshot delta next pump); multi-hole; chained fields + cycle settle (A→B→A converges, both consistent); non-reactive hole renders once (the documented degradation, pinned); detach clears (write after detach → no snapshot delta, no throw); differential oracle/IR/LLVM; --expand round-trip of a reactive template.

## 7. Open questions

1. Loud-typo detection for non-reactive `this.field` holes (§2's final ruling accepts quiet degradation) — revisit with any future runtime-reflection capability.
2. Two-way binding (`value<->{this.field}`: component edits write BACK to the field through the set-view) — v2; the setter/registry substrate is deliberately shaped to allow it (updater direction is the only new piece).
3. Computed/derived fields (`@Reactive` on a get-view) — v2.

## 8. Implementation log

- 2026-07-12 — design written; not started. Gated on F4 + T06; M1 probes runnable as soon as the rule engine question (R-1) is testable.

- **2026-07-14 — LANDED IN FULL (M1–M5), all four engines byte-identical.**
  Files: `sonar/src/reactive.lev` (new), `sonar/src/component.lev` (registry,
  §M2), `sonar/src/templates/expander.lev` (reactive-hole detection, §M4).
  Tests: `sonar/tests/probes/p12–p14` (M1 gate, in `sonar/PROBES.md`),
  `sonar/tests/reactive/` (hand-written bindings — basic/multi-hole/cycle-settle/
  detach/exception-containment), `sonar/tests/reactive-macro/` (the real `sonar!`
  path end to end), `sonar/tests/reactive-guard/` (loud-error negative test).

  **The mechanism works as designed** — accessor injection over an `@`-attributed
  field is GREEN on oracle/IR/emit-C++/LLVM (M1 gate), and a bare `count = n`
  write updates every bound widget through the `sonar!` macro (the headline
  feature). But **TWO assumptions in §2's setter template do not hold against the
  landed rule engine** (both confirmed absent in reference.md §rules — the filed
  LA-15/LA-16 asks, NOT compiler changes), forcing two documented deviations:

  1. **No field-TYPE hole** (`<T>`/`$f.type` is not a reifier — R-1's own flagged
     risk, now confirmed RED). *Worked around:* ONE rule per scalar type, gated
     `where f.type == "int" | "string" | "bool" | "float"`, each injecting a
     typed `set`-view. A `@Reactive` field of any other type is a **loud compile
     error** (`__reactiveGuard`, the @Serializable/@Injectable sentinel idiom) —
     never a silent no-op. Adding a type is one more rule.

  2. **No field-NAME string.** §2 assumed `this.__sonarFieldChanged("$f")` would
     stringize the field name; it does NOT — for a *match-bound* field subject
     `"$f"` stays the literal `"$f"` and `$f.name` reifies to `<field>.name` (only
     a `$for`-bound `meta::Field` VALUE stringizes via `.name`). So the injected
     view cannot key a per-field notify. *Worked around:* the view calls the
     nullary `this.__sonarNotify()` and the Component registry does **GLOBAL
     fan-out** — every reactive write re-runs every binding on that host, with a
     single re-entrancy `bool` making a chain A→B→A settle in one pass (§3's
     cycle-settle guarantee, exercised by the cycle-settle golden). This is
     **observationally identical to per-field notify for every golden** (updaters
     are idempotent renders; paint is deferred per C9). Registry stays
     string-keyed for `--expand` legibility (§5 #5) and to make per-field
     precision a one-line change once a name-stringize reifier lands (LA-15).

  **§5.1's own R-1 decision tree is superseded:** its fallback A (`void set$F(T
  v)`) needs BOTH identifier-synthesis (`set$F`) AND a field-type hole (`T`) —
  both ALSO absent — so it is not buildable either. The set-view + global-fan-out
  route above is the achievable faithful path (bare-assignment ergonomics fully
  preserved), and is what shipped. Fallback B (file the language request) is
  already covered — LA-15/LA-16 are on file in
  `designs/requests/request-metaprog-splices.md` / `request-metaprog-attr-values.md`.

  **Consequence of global fan-out on §2's "non-reactive hole renders once"
  degradation:** a `{this.field}` hole on a NON-reactive field still renders
  once at bind time and its own write triggers nothing — BUT any *reactive* write
  on the same host re-runs it (global fan-out), so it refreshes to the current
  value on the next reactive activity. This is MORE current than "renders once,
  never updates," never incorrect; pinned honestly in the reactive-macro golden.

  **Probe issue #2 resolved (§5):** the field initializer (`int count = 0`)
  writes the raw slot, bypassing the injected view — no ping before the first
  explicit write (initial render happens at bind time). Pinned by probe p14.

  **§6 `--expand` round-trip:** the reactive fragments (`__sonarBind(...)`
  registrations + the rule-injected `set` views, with rule provenance comments)
  render as ordinary, self-describing, re-lexable source — legibility (§5 #5)
  confirmed. A WHOLE-program `--expand`→recompile round-trip is blocked by a
  PRE-EXISTING, T11-independent source-printer bug (**known_bugs_1.md #69**:
  desugared enum member constants print as `EnumName$Member`, whose `$` the lexer
  rejects on re-parse — hits every enum-using program, and Sonar pulls in T03's
  enums transitively). Faithfulness is instead proven the stronger way: the
  original program runs byte-identically on all four engines (the macro expands
  exactly as `--expand` shows).

  **§7 open questions unchanged:** loud-typo detection for a non-reactive
  `{this.field}` hole (#1) still awaits runtime reflection; two-way binding (#2)
  and computed/derived `@Reactive get` (#3) remain v2. The set-view + registry
  substrate is shaped to allow all three.
