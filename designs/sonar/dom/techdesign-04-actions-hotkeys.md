# Sonar DOM — Tech Design 04: Actions & Hotkeys

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D04.
**Owns:** `sonar/src/dom/actions.lev`.
**Depends on:** D01 (Document meta rows host per-node registries), D02 (attrs `action`/`hotkey`),
T03 Keymap (chord grammar, duplicate-throw), T07's pending-shortcut seam
(`__sonarBindShortcut` — pending until attach, unbound at detach), T05 MenuItem/Menu/BarMenu.
Probes: D-P4.
**Gates:** G-D2. **Difficulty:** M. **Risk:** LOW/MED (the responder-chain semantics must be
frozen precisely or every app invents its own).

Implements anchor D-C7. This is the command layer: **one named verb, many triggers** (menu item,
button, hotkey, `fire()` from code), with enablement in one place.

---

## 1. `ActionRegistry`

```lev
class ActionRegistry {
    // parallel columns — closures are reference values (D-P4); never Array<struct>
    private Array<string> names_;  private Array<() => void> handlers_;
    private Array<bool> enabled_;

    ActionRegistry add(string name, () => void handler);   // duplicate name in ONE registry throws
    void remove(string name);
    bool has(string name);
    bool isEnabled(string name);                            // false when absent
    void setEnabled(string name, bool on);                  // → re-grays bound items (§4)
    bool fire(string name);                                 // run if present+enabled; true = handled
}
```

`fire` binds the handler to a local before the call (`var h = handlers_[i]; h();` — the #52 shape).
Handler exceptions are caught at the fire site, `Sonar::log`ged + counted (the T03 dispatch
containment ruling — a broken handler must not kill the key that triggered it). Handlers are
ordinary lambdas; `await` inside is landed task machinery (the sketch's
`filename = await openDialog.show()` runs as a parked task continuation — D05 §4 covers the UI
refresh that follows).

## 2. Where registries live; the responder chain

Every node CAN own a registry (lazily created in the Document meta row — `DomNode.actions()`), and
the DOM containers expose one as a real field (`FlexContainer.actions` — the sketch's
`fileMenu.actions.add(…)` is a field read, no method call). `SonarApp` owns the root registry.

**Resolution (frozen):** `Sonar::Dom::fireAction(IComponent from, string name) -> bool` walks
`from` → parent chain → the app root; at each node with a registry: if `has(name)`:
- enabled → run it, return true (**first-holder-wins**, even if an ancestor also has it);
- disabled → **stop and return false** (a disabled nearer binding deliberately shadows an enabled
  outer one — "Save is off here" must not fall through to a different Save).

Not found anywhere → return false (callers decide loudness; menu/button wiring logs once). This is
the catch-clause/most-specific-first shape the language uses everywhere, applied to commands.

## 3. The `hotkey` attribute

**DOM hotkey grammar** (friendlier than the frozen chord grammar; normalized, never a second
runtime path): `normalizeHotkey(spec) -> string` produces `parseChord`-legal text, then everything
rides the landed Keymap.

| spec | normalizes to | meaning |
|---|---|---|
| `^s` | `C-s` | Ctrl+S (case-folded by parseChord) |
| `^+s` | `C-S-s` | Ctrl+Shift+S — **the sketch's `^+s`** |
| `!x` / `M-x` | `M-x` | Alt+X |
| `^!t` | `C-M-t` | Ctrl+Alt+T |
| `F5`, `Enter`, `Tab` | verbatim | named keys |
| `+F5` | `S-F5` | Shift+F5 |

Rules: leading `^` → `C-`, `!` → `M-`, `+` → `S-` (any order, each once); remainder must be one
printable char or a KeyCode name (parseChord's own validation catches the rest — errors at bind
time, loud). Anchor-logged as the one sanctioned divergence from raw chord specs in markup.

**Binding lifecycle:** the builder/emitter (D02) wires `hotkey` via the landed pending seam —
`node.__sonarBindShortcut(normalized, () => { Sonar::Dom::fireActivate(node); })` — so hotkeys
follow R7 exactly: pending at build, bound into `App.keymap()` at attach, unbound at detach. A
hidden-but-attached menu's hotkeys are LIVE (the sketch depends on this: `^o` works while the File
menu is `hidden`); a popped overlay's are not. Duplicate chords across two mounted components throw
at attach naming the chord (landed Keymap rule — wanted, loud).

`fireActivate(node)`: MenuItem → `activate()`; Button → press path; CheckBox → toggle; anything
else → fire the node's `action` meta if present, else focus it. (The `hotkey` attr means "trigger
this control", not "run this closure" — closures belong to actions.)

## 4. Wiring markup to actions

At build, a `MenuItem`/`Button` with an `action="name"` attr — or, absent that, a **slug of its
label** — gets: `item.onSelect(() => { Sonar::Dom::fireAction(item, key); })` (explicit local
capture; the #53 discipline). `slug(label)`: lowercase, runs of space/`_` → `-`, strip everything
but `[a-z0-9-]` (`"Save As"` → `save-as`, `"Open…"` → `open`). The slug is recorded in the node's
meta (`action` column) so the inspector and tests can see the effective key. An explicit
`action=""` opts out (pure-handler items).

**Enablement → graying:** `fireAction`-participating registries keep a bound-items column
(`Array<IComponent>` + parallel `Array<string>` keys, registered at build/`bindItem`). `setEnabled`
walks its bound items for that key: MenuItem/Button `enable(on)` + invalidate — menus gray out the
moment the command does, the classic desktop contract with zero per-frame polling. Cross-registry
note: an item binds to the registry that RESOLVES its key at bind time (walk once at attach; a
later nearer `add` re-resolves on next attach — documented simplification, v1).

`SonarApp` pre-registers: `quit` → `quitWith(0)`. (`"open"`/`"save"` etc. are app domain — the
framework ships exactly the one verb every app has.)

## 5. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | ActionRegistry + fireAction chain + containment + tests | S/M |
| M2 | normalizeHotkey + pending-seam wiring + fireActivate ladder | M |
| M3 | label slugs, action meta, bound-item graying | M |
| M4 | SonarApp root registry + quit; integration corpus with D02 markup | S |

## 6. Potential issues & mitigations

1. **Chain surprises** (disabled-shadows-enabled) — frozen in §2 with a rationale; the corpus pins
   both directions (nearer-disabled blocks; nearer-enabled wins over outer).
2. **Slug collisions** (`"Save"` twice in one menu) — two items firing one action is legal and often
   wanted; two `add`s of one name in ONE registry throws (the duplicate rule). Documented.
3. **Hotkey attr on a node that never attaches** — pending registrations idle harmlessly (landed
   seam semantics); nothing leaks (R7).
4. **Graying staleness** (bound-items re-resolve only at attach) — `setEnabled` on a registry the
   item didn't bind to won't gray it; the fix is the v1.1 re-resolve-on-fire note; corpus documents
   current behavior.
5. **`Array<() => void>` churn on LLVM native (D-P4)** — probe first; red ⇒ STOP-and-escalate (no
   local fallback exists for closure storage — but T01's handler arrays are the same shape and
   landed green, so expectation is green).

## 7. Testing plan

Registry unit corpus (add/remove/enabled/fire incl. #52-shape invocation, duplicate throw,
containment); chain matrix over a 3-deep tree (nearer-wins, disabled-stops, miss-returns-false);
normalizeHotkey table (every row of §3 + error cases through parseChord); attach/detach hotkey
lifecycle via harness scripts (`key("^o")` fires while hidden, stops after remove); slug table;
graying script (setEnabled → snapshot shows dim item); the target-feel menu trio end-to-end.
Differential oracle/IR/LLVM.

## 8. Open questions

1. Action arguments (`fire(name, payload)`) — v1.1; today closures capture what they need.
2. Checkable/radio menu items reflecting action state — v1.1 (wants an action-state channel).
3. Re-resolve bound registry at fire time (fixes §6.4) — v1.1, one walk per fire.

## 9. Implementation log

- 2026-07-15 — design written; not started.
