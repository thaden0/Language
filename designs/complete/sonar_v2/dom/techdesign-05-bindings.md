# Sonar DOM — Tech Design 05: Bindings (`{{…}}`)

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D05.
**Owns:** `sonar/src/dom/binding.lev` (+ the `Document` binding/exposure/store members D01 declared
for it).
**Depends on:** D01 (Document, sweep hook in `SonarApp.renderFrame`), D02 (hole registration from
both tiers), T11 as the adjacent landed system (bridged, not modified). Probes: D-P3, D-P6.
**Gates:** G-D3. **Difficulty:** M. **Risk:** MED — the sweep-scheduling contract is the subtle
part; it is frozen here so tests can pin it.

Implements anchor D-C8. The headline: `<span>{{filename}}</span>` + a bare global write
`filename = await openDialog.show();` updates the span — no observer registration, no store write,
no signal type.

---

## 1. Why pull, not push (the analysis, recorded)

Push-based reactivity needs a write hook. The landed rule engine cannot provide one for globals
(rules match class members only — the indexing pass never registers a top-level `var`; constraints
dossier §D.3), T11's set-view route is scalar **fields** with global fan-out, and accessor views
over namespace globals are undocumented/unsupported. Filing a "global set-views" language request
would gate the target feel on a compiler change — rejected (anchor §9). A **pull sweep** — 
re-evaluate registered getters, diff, apply on change — needs zero language surface, works for ANY
expression a closure can read (globals, fields, calls), is deterministic under `pumpOnce`, and its
cost is proportional to binding count, not writes. UI binding counts are small (10²); a string
compare per binding per frame is noise next to one damaged-leaf repaint. Pull wins on every axis
that matters here.

## 2. The binding record & table

```lev
class Binding {                     // class, not struct (closures + #74 discipline)
    IComponent node;                // the Text (or attr host) the binding drives
    string sourceText;              // "{{filename}}" — serializer/inspector channel (D01 §5.1)
    () => string getter;            // evaluate current value
    (string) => void apply;         // write into the component (its setter invalidates)
    string last;                    // diff cache
    bool dead;                      // tombstone (node detached)
}
```

`Document` holds `Array<Binding> bindings_` (instance field — #73 discipline; compaction shares
D-P10's threshold). Registration:

```lev
void bindText(Document doc, IComponent textNode, string sourceText, () => string getter);
// generic form for future attr bindings:
void bind(Document doc, IComponent node, string sourceText, () => string getter, (string) => void apply);
```

`bindText`'s apply is `(v) => { t.text(v); }` over the narrowed `Text` (explicit local capture —
the #53 discipline). Registration **runs the getter once immediately** (initial render = update
path, the T11 `__sonarBind` precedent). Detach: bindings are validated lazily — a sweep hitting a
`dead`/detached node (parent-walk fails; the D01 validation helper) tombstones it; `Document`
compaction reclaims.

## 3. The two tiers' holes

- **`dom!` tier (full fidelity):** the emitter (D02 §4) wraps every `{{expr}}` as
  `Sonar::Dom::bindText(__doc, __sonar_n, "{{expr}}", () => Sonar::Dom::text(expr));` — the closure
  captures the global/field/call **directly**; the sweep re-reads it each pass. This is what makes
  the target sketch literal: no `expose`, no store, just the write.
- **Runtime tier:** `{{key}}` must be a bare identifier (E-D9 otherwise). Resolution at sweep time,
  checked in order: (1) an `expose(key, getter)` registration — the app-supplied closure route;
  (2) the Document store (`doc.set(key, value)` / `get`) — the dynamic route (`set` marks the sweep
  dirty and schedules a frame); (3) neither → renders `""` once + one `Sonar::log` line naming the
  key (quiet-but-logged degradation; the T11 non-reactive-hole stance).

### 3.1 `Sonar::Dom::text` — the hole renderer (D-P3)

Overload family: `text(string)`, `text(string | None)` (None → `""`), `text(int)`, `text(float)`,
`text(bool)`, `text(char)`. The union overload is exactly what `{{filename}}` with
`string | None filename` needs — probe D-P3 pins that overload resolution picks it; **fallback if
red:** the family drops the union member and a union-typed hole is a compile error in the expansion
whose fix is spelled in the error text (`{{filename ?? ""}}`) — fidelity delta anchor-logged. Any
other hole type fails to resolve at the expansion's re-check with the call-site span (honest,
compile-time).

## 4. The sweep (frozen contract)

```
sweep():  for each live binding: v = getter()          // exceptions: catch, log, count, skip (T03 rule)
          if v != last: last = v; apply(v)             // apply → setter → invalidate → damage
```

**When it runs:**
1. **Every frame, first phase** — `SonarApp.renderFrame` calls `doc.__sweepBindings()` before the
   qualified base frame (D01 §3). Every event/timer/resize already schedules a frame, so any state
   change caused inside a dispatched handler is visible the same frame.
2. **Async resolutions** — DOM-owned promise surfaces (D07 dialogs; `Document.set`) call
   `doc.__scheduleSweepFrame()` (= `host().requestFrame()`) after resolving, so a parked
   continuation's writes (the sketch's post-`await` global write) are picked up by the frame that
   resolution schedules. Probe D-P6 pins same-frame visibility for the button-resolves →
   continuation-writes → frame-sweeps sequence on all engines; **if red:** the sweep additionally
   arms one trailing frame whenever a sweep changed anything (convergence within one extra frame,
   ~16ms at the default cap — documented as the worst case either way).
3. **Manually** — `doc.refresh()` (public): sweep now + schedule a frame if anything changed. The
   escape hatch for exotic mutation paths (worker results marshalled in, etc.).

**Idle guarantee preserved:** no timer, no polling — an app with no events runs no frames and no
sweeps (the T09 zero-CPU idle rule stands). The trade, stated plainly: a global mutated by NOTHING
that schedules a frame (impossible from UI; possible from… nothing in the single-threaded loop
except a timer the app armed — which schedules frames) stays stale until the next frame; `refresh()`
covers the theoretical residue.

**Determinism:** under `pumpOnce` the sweep runs exactly once per pump at a fixed phase —
scripted tests assert exact before/after snapshots with zero timing sensitivity.

## 5. Interplay with T11 (`@Sonar::Reactive`)

Orthogonal and composable: T11 is push (field write → set-view → `__sonarNotify` → that component's
updaters re-run, same instant); D05 is pull (anything → next frame). A `dom!` hole over a reactive
FIELD gets both behaviors harmlessly (the sweep's diff makes the second application a no-op).
Guidance (docs): component-internal state → `@Reactive` fields; app/global state → `{{…}}` holes.
No code bridges are required — the emitter does NOT special-case reactive fields (uniformity beats
cleverness; the diff absorbs the overlap).

## 6. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | Binding/table/registration + initial-render + tombstoning + compaction | S/M |
| M2 | `text` overload family (D-P3 first) + runtime key resolution (expose/store/miss) | S/M |
| M3 | sweep + the three scheduling points + `refresh()` + SonarApp hook integration | M |
| M4 | D-P6 probe + async-resolution corpus (dialog-shaped) + determinism pins | M |

## 7. Potential issues & mitigations

1. **Getter cost creep** (a hole calling something expensive) — per-sweep evaluation is the
   documented contract ("holes are read every frame; keep them cheap"); the inspector surfaces
   per-sweep timing when it lands (D08).
2. **Exception in a getter** — contained per the T03 rule (catch/log/count/skip); a throwing binding
   renders its last good value — pinned by test.
3. **Binding to a detached-but-reattachable subtree** (a hidden template re-added later) —
   tombstoning only triggers on sweep-while-detached; re-adding after tombstone requires re-binding
   (rebuild or `refresh` of the subtree). Documented; the markup tiers rebuild naturally.
4. **Two bindings driving one node** (two holes in one span) — v1 rule: one Text per hole (the D02
   text-run builder splits runs at holes), so the case cannot arise; multi-hole runs concatenate
   getters into ONE binding at build (`"a {{x}} b {{y}}"` → one getter concatenating both) —
   simpler than positional patching, spec'd.
5. **Store typing** (store is string-valued v1) — non-string app state belongs in the dom! tier or
   an exposed getter; widening the store is an open question.

## 8. Testing plan

Sweep-visibility scripts: (a) key handler writes a global → same-pump snapshot delta; (b) dialog
resolve → continuation write → snapshot delta (the D-P6 corpus, both outcomes documented);
(c) `doc.set` → delta; (d) miss → logged + empty; (e) exception containment; (f) diff no-op leaves
zero damage (frame stats assert 0 dirty). Overload family matrix incl. `string | None` both states.
Multi-hole concat. Tombstone/compaction churn. T11-overlap double-application no-op. Differential
oracle/IR/LLVM; determinism via pumpOnce only (no timers in any test).

## 9. Open questions

1. Attr bindings (`value="{{x}}"`) — the generic `bind()` exists; wiring attr appliers to it is
   v1.1 (text-position covers the target feel).
2. Two-way binding (`value<->{{x}}`) — v2, shared with T06's deferral; needs a write-back channel.
3. Typed store / non-string exposure — v1.1 if demanded; the dom! tier already covers typed state.

## 10. Implementation log

- 2026-07-15 — design written; not started.
- 2026-07-18 — implemented. `binding.lev` filled in: the `Binding` record, the
  `text` overload family (incl. `string | None` — **D-P3 GREEN**: `text(maybeName)`
  over a `string | None` resolves to the union member, None→`""`, verified on all
  three run engines), the generic registration (named `bindNode` — `bind` is a
  reserved word), `bindText`, and the runtime tier's `bindRuntimeText`/`bindLeafText`
  (key resolution expose→store→miss, log-once). `document.lev`: the `bindings_`
  table, `__sweepBindings` (frozen §4 contract — diff/apply, lazy tombstoning via
  `reachesRoot`, T03 exception containment), `__sweepDirty`, `__compactBindings`
  (D-P10 threshold: 25% dead AND ≥64 dead), `__resolveKey`, `__scheduleSweepFrame`,
  `refresh()`, and `set()` now schedules a sweep frame. `builder.lev`: the runtime
  string tier registers live bindings instead of one-shot `doc.get`.
- **Tier asymmetry recorded (a landed D02 fact, not a D05 change):** the dom!
  emitter binds only holes that are *fragment children of a container* (each
  becomes its own bound `span` Text). A hole that is the whole text CONTENT of a
  leaf element (`<span>{{x}}</span>`, span→Text) is one-shot at the dom! tier
  (`emitLeafText`, "D05 binds Text nodes only"). The runtime tier DOES bind leaf
  content (`bindLeafText`). So the headline sketch's live update comes from the
  fragment-text form (`<flex>{{filename}}</flex>` / a bare `{{filename}}` run in a
  container); the corpus pins both tiers.
- **D-P6 (async same-frame visibility):** the scheduling contract is in place —
  `set()`/async resolutions call `__scheduleSweepFrame()` (= `host().requestFrame()`),
  a no-op before the session starts so headless build-time seeding arms nothing.
  The dialog-shaped async probe is deferred to D07 (dialogs unlanded); the
  determinism corpus drives `__sweepBindings()`/`refresh()` directly (the exact
  calls `renderFrame` makes), pumpOnce-free, no timers.
- Corpus: `sonar/tests/dom-bindings/` — dom!-global write, typed holes, overload
  matrix, runtime store/expose/miss, leaf concat, exception containment, refresh,
  tombstone+compaction churn, diff no-op idempotence (the T11-overlap mechanism).
  Passes oracle+IR+LLVM byte-identical; emit-C++ SKIP (async/native gap).
