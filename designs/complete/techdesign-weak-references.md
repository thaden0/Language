# Tech Design: Weak References (`weak` fields)

**Status:** implemented. **Date:** 2026-07-12. **Feature ID:** F5 (Sonar prerequisite; resolves info.md §19 #10).
**Depends on:** none. **Unblocks:** Sonar's leak-free retained tree (G-S5); fixes the observed Timer→handler cycle class.
**Difficulty:** L/XL. **Risk:** HIGH — the only feature of the set touching ARC runtime internals. Recommended landing order: LAST of F1–F6 (Sonar's R7 detach discipline is the stated interim).

Frozen surface: anchor §4/F5. Ground truth: infodemp §2 (the complete ARC map — the implementation's required reading; all file:line refs from it, re-verify at implementation).

---

## 1. Motivation

Refcounting never frees cycles. The cycle class is observed in practice (Timer → buffer → handler → Timer, info.md §15), and Sonar's retained tree is a cycle **by construction**: `parent_` up-links + `children` arrays. Every dismissed Modal / removed tab / replaced subtree leaks its whole subtree in a long-running TUI. `weak IComponent? parent_` breaks the cycle at its root: back-edges become non-owning.

## 2. Current state (the constraints that shape the design)

- **Header:** `LvHeader { int64_t rc; int64_t meta; }`, 16 bytes at `payload-16` (`lv_abi.h:63-76`). **`meta` is OCCUPIED — it stores the total allocation size** (written by `lv_halloc_prefixed`, `lv_runtime.c:167-174`; consumed by the free-list size-class logic). There is no free header word.
- **Death is immediate and destructive:** rc→0 ⇒ `lv_recursive_free` (`:252-332`) walks and frees children, then `lv_free_raw` (`:151-160`) memset-poisons `0xFE` and returns the block to a power-of-two free list — **freed memory is immediately reusable**. A naive weak read after free touches reused memory. New liveness state is unavoidable.
- ARC is implicit in op semantics, classified by `destKind(Op)` (`LlvmGen.cpp:81-98`); the store paths a weak slot must bypass retain on: `RawSet :2919`, `SetMember/lvrt_setm`, `CaptureVar`; read paths that must yield None on a dead referent: `RawGet :2484`, `GetMember/lvrt_getm`.
- Interpreters (oracle + IR) share `RuntimeValue.hpp` and use host `shared_ptr` — a weak slot is naturally `std::weak_ptr` + `.lock()`.
- Threads: **objects never cross threads** (copy-always; Channel is the portal) — weak machinery can be per-thread, lock-free.
- Allocation addresses are 16-aligned, but M1 verified that `meta` stores the
  **unrounded requested size**. Its low bits are therefore occupied and cannot
  be stolen; the implemented table-only fallback is described in §3.2.

## 3. Design

### 3.1 Language surface (frozen)

```lev
class Component {
    weak IComponent? parent_ = None;
}
```

- `weak` is **field-only** (the `readonly` precedent: it names a slot lifecycle property). Not on locals/params/globals in v1.
- Declared type MUST be `T?` with `T` a **class or interface type** — a weak read can observe absence, so the type admits `None` honestly. Not allowed: strings/arrays/maps/Blocks/closures/structs as `T` (weak = non-owning back-edge to an *entity*).
- Store does not retain; reads yield `T?` (`None` once the referent's strong count hit zero); writes accept `T`, `T?`, or `None`. Ordinary flow narrowing applies to the READ RESULT (a local copy) — narrowing a weak FIELD directly across statements is unsound (the referent can die between them) and the checker treats weak fields as non-narrowable member paths (assignments-invalidate machinery reused: a weak field read is always fresh). **This is the one checker-semantics subtlety; pinned by test.**
- Modifier matrix: `weak readonly` allowed (write-once non-owning); `weak const` rejected (a compile-time-fixed weak slot is meaningless); `distinct weak` allowed (orthogonal); access modifiers orthogonal.
- **Copy boundaries (spawn/Channel):** the flatten/rebuild engine copies weak fields as `None` on the far side — the referent wasn't copied; a copy is a different object. (Flattening the referent instead would silently convert non-owning to owning — rejected.)
- **The slot rule (design invariant, stated everywhere):** *weakness is a property of the SLOT, never of the value.* A weak read that finds a live referent yields an ordinary +1 owned value in a register; only stores into the slot skip the retain.

### 3.2 Runtime model — per-thread weak table + proxy cells (implemented; no header widening)

M1 refuted the proposed `meta` flag: allocation addresses are aligned, but
`meta` stores the unrounded requested size, whose low bits are live data. The
implementation therefore uses the table-only fallback from §8.1. Header layout
and every existing `meta` reader remain unchanged.

- **Proxy cell:** a small refcounted heap record `{ LvValue target }` (tag+payload of the referent, NOT counted). Weak SLOTS hold a strong ref to the proxy (proxies are ordinary counted objects with a dedicated tag or classId).
- **Weak table:** per-thread open-addressed hash, `payload address → proxy pointer`. Lives beside the per-thread heap state (`LV_TLS`).
- **Weak store** (`lvrt_weak_store(slotAddr, value)`): release the old proxy ref in the slot; if value is None, store None; else get-or-create the target's proxy via the table and retain it into the slot.
- **Weak read** (`lvrt_weak_load(slotAddr) -> LvValue`): load proxy; if None or `proxy.target` is None → None; else retain the target and return it (+1 transfer, dk2-shaped).
- **Object death:** in the rc→0 path, BEFORE `lv_recursive_free`'s child walk and `lv_free_raw`'s poison/reuse: if the per-thread table is non-empty, look up the payload, null `proxy.target`, and remove the entry. **Ordering is the correctness crux: null-the-proxy strictly before poison/reuse.**
- **Proxy lifetime ruling:** once created, a proxy lives until its refcount drops (slots releasing it) — the table holds a NON-counted pointer, removed at object death; after death the orphaned proxy (target=None) is kept alive only by remaining weak slots and dies with the last of them. No churn pathology: one proxy per weakly-referenced object, ever.
- **Header metadata is untouched.** No reader masking or ABI/layout change is required.

**Alternatives considered:** (a) header weak-count + tombstone (Swift): requires a third header word ⇒ ABI break across every `payload-16` assumption (HDR users, allocator, flatten, frozen-X64Gen parity) — rejected on blast radius; (c) zeroing table of weak SLOT addresses: slot addresses live in objects/copies whose lifetimes the table would then have to track — strictly more bookkeeping than (b). Implemented cost: when no weak proxies exist, a free pays one table-count branch; while the table is non-empty, a free performs a target lookup. Weak store/read retain the proxy/locked target as described above.

### 3.3 Compiler plumbing

- **Resolver/Checker:** parse `weak` in field-modifier position; `Slot` gains a weak flag (beside `distinct`/`const`/`readonly`, `Symbols.hpp:35-46`); type rules + the modifier matrix + diagnostics ("weak field must have optional class/interface type", "weak is field-only", "weak const is meaningless").
- **IR:** two new ops `RawGetWeak` / `RawSetWeak` (new ops over flag operands — `destKind` stays a pure function of the op; RawSetWeak is dk0-style own-memory, RawGetWeak dk2 +1-if-live). Lower selects them from the slot flag (statically known). Dynamic-fallback fields cannot be weak (declared-slot feature; `SetMember`'s dyn path untouched).
- **LLVM:** bind `lvrt_weak_store/lvrt_weak_load`; wire the two ops; `wrapDest`/`releaseAllRegs` untouched (the slot rule: register values are normal).
- **Oracle+IR interpreters:** new `VKind::WeakRef` wrapping `std::weak_ptr<Object>` (+ class Symbol for typing); weak store converts shared→weak; read `.lock()` → Object value or None. One shared change (both interpreters ride `RuntimeValue`/shared paths).
- **emit-C++ (CGen):** investigate its object representation at implementation; if `shared_ptr`-backed, mirror the interpreter approach; if its scalar-core model can't express it, `weak` diagnoses as unsupported on that lane (precedent: loop-bound features). Honest lane ruling deferred to the first implementation step, both paths specced.
- **ELF/X64Gen:** frozen; no lane; never a gate.

### 3.4 Engine-agreement argument

Interpreters observe None exactly when the last `shared_ptr` drops; LLVM when rc hits 0. These coincide for corpus programs because ARC emission mirrors ownership at the same source points — the identical argument the churn corpus already validates for frees (13/13 green at +0B). Weak tests are therefore differential like everything else.

## 4. Implementation plan

| M | step | difficulty |
|---|---|---|
| M1 | Verify the meta-bit premise (alignment assert + reader enumeration/masking) | S — **gate** |
| M2 | Resolver/Checker: modifier, slot flag, type rules, diagnostics, non-narrowable-member rule | M |
| M3 | Interpreters: VKind::WeakRef + store/read paths (oracle + IR) | M |
| M4 | Runtime: proxy cell + per-thread table + store/load/death hooks + masking | L |
| M5 | IR ops + Lower selection + LLVM wiring | M |
| M6 | Flatten-to-None at spawn/Channel boundaries (the flatten walk's field loop) | S/M |
| M7 | CGen lane ruling + implementation-or-diagnostic | S/M |
| M8 | Corpus + churn programs | M |

## 5. Edge cases (decided)

- **Self-weak** (`obj.weakSelf = obj`): legal; dies with obj; read-after-death impossible (no reader survives).
- **Weak store of a fresh (+0, rc=0) temp as its ONLY use:** the temp's register release at statement end drops it to death immediately ⇒ subsequent reads yield None. Correct and loud-ish (the object was never owned); documented with an example — a weak slot never keeps anything alive, including fresh temps.
- **Arena (rc=-1) / data-segment targets:** `lv_is_counted` gates — such targets never die; their proxies never null. Fine; the store path must still create proxies for them (or store the value raw with an immortal marker — simpler: proxy with target never cleared; chosen for uniformity).
- **No user code runs during `lv_recursive_free`** (no destructors in the language) — verified assumption; re-entrancy into the table during a free is therefore only the runtime's own hook. Stated as a load-bearing invariant.
- **Weak inside value structs:** banned v1 (structs copy; a copied weak slot is coherent but structs-as-fields complicate the flag story) — class fields only.
- **Captures:** a closure capturing a weak READ captures the strong value (the slot rule); "capturing weakly" is not in v1.

## 6. Potential issues & mitigations

1. **Meta-bit collision — resolved by M1.** Low bits are occupied by unrounded sizes, so the implementation uses no header bit.
2. **Table growth/deletion pathology** — open addressing with backward-shift deletion (no tombstone accumulation); per-thread sizing amortized against weakly-referenced-object count (small for Sonar: ~component count).
3. **Interpreter Value size growth** (`weak_ptr` is 2 words) — carry it boxed (`shared_ptr<WeakBox>`) if the union would grow; measure first.
4. **Differential divergence risk** (§3.4's argument fails somewhere) — every weak corpus program runs all lanes; any divergence is a P1 stop-the-line, not a "note".
5. **Missed store path** (a weak slot written through a path that retains — e.g. `$init` field defaults, CaptureVar into weak? captures can't target fields — enumerate ALL slot-write emission sites in Lower against the flag) — M5's checklist is the enumeration; the corpus's `weak readonly` + ctor-write test covers `$init`.
6. **ASAN validation** — run the weak corpus under the sanitizer build (in-tree precedent) to catch any read-after-free the design missed. Mandatory before merge.

## 7. Testing plan

Corpus: basic store/read; None-after-death (scope-driven and refcount-driven); the parent↔children cycle churn program green at +0B under `fuzz/churn_leak.py`'s conventions (the headline test — today it leaks); the info.md Timer-cycle program fixed via a weak back-ref; `weak readonly` write-once; `distinct weak`; modifier-error diagnostics; fresh-temp-only-weak → None; narrowing of the READ value + the non-narrowable-field rule; flatten-to-None across spawn and Channel; arena/global-literal targets; differential all lanes + ASAN pass.

## 8. Open questions / escalations

1. **Resolved:** M1 refuted the premise; the table-only design is implemented (every free probes only while the per-thread table is non-empty).
2. Weak locals/params (v2) — wanted eventually for cache patterns; field-only ships first.

## 9. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — implemented across parser/resolver/checker, oracle, IR interpreter,
  emit-C++, and LLVM. M1 refuted the low-meta-bit premise because `meta` stores
  unrounded requested sizes; the specified table-only fallback landed instead.
  LLVM uses per-thread target-to-proxy tables and invalidates proxies before the
  recursive free/poison path. Copy boundaries rebuild weak fields as `None`.
  Focused four-engine corpus, source round-trip, diagnostics, ASAN/UBSAN, Valgrind,
  and weak parent/children churn (`100 -> 800`, `+0B`) pass. Frozen ELF has no lane.
