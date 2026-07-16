# Bug 30 — LLVM: `Map.with` returns an UNOWNED map under a +1-transfer convention (freed at the first `return`)

**Status:** LANDED (2026-07-10) — M1/M2/M3/M4 all implemented and verified; see §7
Implementation log. **Date:** 2026-07-10. **Source:** `bug.md` #30 [P1]; Track 09 M4's JSON LLVM-lane
exclusion (`leviathan-track09-landed`). **Depends on:** nothing. Runs parallel to
`techdesign-bug29-prelude-self-call-capture.md` (disjoint files — that track owns
an `src/Eval.cpp` region; this one never touches it).
**Owns:** `runtime/lv_runtime.c` (one new entry point next to `lv_map_upsert`),
`runtime/lv_abi.h` (declaration + two ownership-table rows), `runtime/selftest.c`
(contract rows), `src/LlvmGen.cpp` `with`/`without` native rows (`:1480-1535`) and
the matching `rtMapWith` binding in the fn-table region (`:152/:228` area), new
corpus cases, and the `tests/corpus/json` LLVM-lane re-enable.
**Does NOT touch:** `lvrt_idxset`/`lv_map_upsert` semantics for `IndexStore` sites
(kept byte-identical — see §5 for why), `X64Gen.cpp`/ELF (frozen), `CGen.cpp`,
`RuntimeNatives.cpp`, `Lower.cpp`, the IR.

---

## 1. Re-bounding: the report's triggers are all red herrings

`bug.md` #30 says the trigger is "a `Map` whose value type is a class that
transitively contains that same `Map`", surfacing in class methods, with
`Map<string,int>` unaffected. Measured today (tree `a3dd932`), every one of those
qualifiers falls away:

| probe | shape (all: build then read `length()`/`at()`) | LLVM `--build-native` | `--run`/`--ir`/`--build` |
|---|---|---|---|
| p3 | with-chain, used in the SAME function (no return) | `2` ✓ | `2` |
| r1 | `return m` where `m` is a never-`with`ed empty map | `0` ✓ | `0` |
| r2 | **`return m.with("a", 1)` — minimal trigger** | `-72340172838076674` ✗ | `1` |
| q1 | `Map<string,int>`, free function, chain + return | poison ✗ | `2` |
| p1 | plain (non-recursive) class values | poison ✗ | `2` |
| p2 | recursive class values (the report's repro) | poison ✗ | `2` |
| r3 | class method, `int` values | poison ✗ | `2` |
| s3 | update-of-existing-key path, then return | *(void)* ✗ | `7` |
| t1 | `m["a"] = 1; m["b"] = 2; return m;` (IndexStore statement form) | `2` ✓ | `2` |
| s1 | shared ARRAY, `b[0] = 9`, return (IndexStore) | `3` ✓ | `3` |
| q4 | `Array` `add`-chain + return | `2` ✓ | `2` |

True statement of the bug: **on LLVM, any map that has passed through `Map.with`
is freed at the first `return` that carries it across a frame; the caller reads
freelist-poisoned memory** (`0xFEFEFEFEFEFEFEFE` = `lv_free_raw`'s poison,
`runtime/lv_runtime.c:147` — hence the report's `-72340172838076674` length and
the segfault once entries are dereferenced). Value type, recursion, class-method
context, entry count — all irrelevant. The JSON symptoms match exactly: a
poisoned length reads negative → render loop sees no entries → one-entry object
renders `{}`; entry reads on reused memory → SIGSEGV.

The report's bounding was confounded: its `Map<string,int>` and `Array<Node>`
"correct" probes read the container in-frame (p3-shaped) or used `add`
(q4-shaped) — never `with`-then-return.

## 2. Background: the two ownership conventions in play

Runtime v2's ARC model (`runtime/lv_abi.h` §2.4): every counted heap payload has
a header `[rc, meta]`; `rc == 0` is *fresh/unowned*; `lvrt_release` on `rc <= 0`
is a documented no-op; a block is freed only by the release that takes a counted
rc from 1 to 0 (`lv_runtime.c:213-221`).

`src/LlvmGen.cpp` assigns each IR op a dest-write kind (`destKind`, `:60-98`):

- **dk==1** (default; includes `IndexStore`, `NewMap`): codegen retains the
  value it writes — the native may return unowned (+0).
- **dk==2** (`Call`/`CallDyn`/...): the value written is ALREADY at +1 — a
  return-transfer; codegen releases the old dest only, retains nothing.

So a `CallDyn` native row must deliver its result at +1. Frame exit is built on
that: `Op::Ret` (`:1963-1969`) retains the return register, releases ALL
registers, and copies out — net +0 across the boundary for an owned value, but
for an UNOWNED (+0) value the sequence is retain(0→1), release(1→0) → the value
is **recursively freed at the return site** and a dangling payload is copied to
the caller.

Every other container row honors the contract:

- `array.add` → `lvrt_arr_append` stamps `HDR[0] = 1` on all four paths, with the
  comment "§15: append returns owned (+1), a transfer contract"
  (`lv_runtime.c:759, 776, 802, 815`), and its rc==1 grow path even frees the
  consumed base (`:801`).
- `map.without`, `map.keys`, `map.values` are inline rows that end in
  `rtRetain` on the fresh container — "+1 transfer" (`LlvmGen.cpp:1477, 1533`).

## 3. Root cause (proven)

The `with` row (`src/LlvmGen.cpp:1480-1487`) is the one row that delegates to
**`lvrt_idxset`** — a helper written for the OTHER convention (`IndexStore`,
dk==1) — and adds nothing:

```cpp
} else if (n == "with") {
    // ... "so no extra retain here — same as IndexStore's own rtIdxSet call
    //      above, which doesn't either."            <-- the faulty equivalence
    row(7, [&] { b.CreateCall(rtIdxSet, {dst, recv, arg(0), arg(1)}); });
```

`lvrt_idxset`'s map branch `lv_map_upsert` (`lv_runtime.c:902-931`) returns:
- **in-place path** (`rc==1 && key found`): `*out = *base` — fine under `with`'s
  consumed-receiver protocol (the MoveClear'd +1 rides through; see below);
- **fresh path** (everything else): a `lvrt_map_new` copy with entries retained
  but the map itself left at **rc = 0** — correct for IndexStore (dk==1 retains
  it), WRONG under CallDyn dk==2, where nobody ever adds the +1.

Direct proof (LANG_ARC_TRACE on r2): the with-produced map's full lifetime is
`R→1, r→0` — exactly one retain (at `Op::Ret`) and one release
(`releaseAllRegs`), freed at the boundary while the caller's copy dangles. An
owned result would read `R→1` (row), `R→2` (Ret), `r→1` (teardown).

Four consequences, all measured:

1. **Return across a frame → poison/segfault** (r2/q1/p1/p2/r3) — the filed bug.
2. **In-frame `with`-maps never free** (release on rc 0 no-ops) — a silent leak;
   visible as inflated `live-at-exit` in the heap meter.
3. **The rc==1 in-place COW path is unreachable** — a with-chained map is rc 0,
   so `with` copies every time; bug #19's whole point is silently defeated on
   LLVM (s3 additionally shows the update shape corrupting on return).
4. **Consumed-base leak:** `m = m.with(...)` lowers with `in.b = 1` — "the
   callee owns the receiver buffer's fate" (`Lower.cpp:1321-1324`), and the
   CallDyn tail clears the window slot WITHOUT releasing (`LlvmGen.cpp:1940-1943`)
   — but `lv_map_upsert`'s fresh path never takes that fate (contrast
   `lv_arr_boxed_append`'s grow, which frees the consumed base, `:801`). The old
   map leaks even once (2) is fixed. `without` (`:1488-1535`) has the same
   consumed-base leak: fresh result retained, consumed receiver never released.

`--run`/`--ir`/emit-C++ are immune (shared_ptr cores: `RuntimeNatives.cpp`,
`CGen.cpp:465`); the ownership half of CGen's `with` semantics simply never got
ported when the row was written. `lv_abi.h`'s ownership table (§ around `:385`)
lists `map.length/.has/.at/.keys/.values` — **`with`/`without` were never given a
row**; the contract that was violated was never written down. Fix that too.

## 4. The fix

### 4.1 Runtime: `lvrt_map_with` (result ownership)

New entry point in `runtime/lv_runtime.c`, immediately after `lv_map_upsert`;
`lvrt_idxset` keeps calling `lv_map_upsert` unchanged:

```c
/* map.with — CallDyn (+1 transfer) convention, mirroring lvrt_arr_append's
 * stamped contract (§15): in-place COW result carries the consumed receiver's
 * own +1 through; a FRESH copy leaves lv_map_upsert at rc 0 and is retained
 * here so every path crosses the native boundary owned. The consumed
 * receiver's fate stays with the CALL SITE (LlvmGen's with row, which alone
 * knows in.b) — see the row comment. */
void lvrt_map_with(LvValue* out, const LvValue* base,
                   const LvValue* idx, const LvValue* val) {
    lv_map_upsert(out, base, idx, val);
    if (out->payload != base->payload) lvrt_retain(out);   /* fresh: 0 -> 1 */
}
```

Declare in `lv_abi.h` §natives and add the missing ownership-table rows:

```
map.with      | in-place (COW, uniquely-owned receiver): returns the receiver,
              | whose consumed +1 rides through; else returns a FRESH map at +1
              | (entries retained). Never takes the base's fate — the with row
              | releases the consumed receiver ref itself (in.b-gated).
map.without   | borrows args; returns a FRESH map at +1, entries retained; the
              | row releases the consumed receiver ref itself (in.b-gated).
```

### 4.2 Codegen: the `with` row + the consumed-receiver release

In `src/LlvmGen.cpp`: bind `rtMapWith` (fn-table region `:152/:228`), and rewrite
the `with` row (replace the faulty comment — cite this doc):

```cpp
} else if (n == "with") {
    row(7, [&] {
        b.CreateCall(rtMapWith, {dst, recv, arg(0), arg(1)});
        if (in.b) {
            // consumed receiver (m = m.with(...)): the callee owns its fate.
            // In-place returned the same payload — the consumed +1 IS the
            // result's count, nothing to do. Fresh copy: release exactly the
            // one consumed ref (entries survive; the fresh map retained them).
            llvm::Value* same = b.CreateICmpEQ(loadPay(b, regs[in.a]),
                                               loadPay(b, recv));
            /* emit: if (!same) rtRelease(recv) — recv register is still intact
               here; the CallDyn tail's storeVoid runs after the row. */
        }
    });
```

`without` rider (same family, `:1488-1535`): after the existing
`rtRetain(lvA); copyLV(dst, lvA);`, emit `if (in.b) rtRelease(recv)`
unconditionally-on-fresh (the result is always fresh, never the receiver).

Why the split between 4.1 and 4.2 — this is the load-bearing design decision:

- **The consumed-base release cannot live in the runtime.** `rc==1` does NOT
  imply "consumed" at every call site: `IndexStore` sites deliberately engineer
  `rc==1` with the base register still live (the stale-+1 release discipline at
  `LlvmGen.cpp:1570-1595` exists precisely so `arr[i]=v` loops see rc 1 and hit
  COW-in-place). Only the row knows `in.b`, which is compile-time. A runtime
  that freed "rc==1 non-found" bases would double-free at IndexStore sites.
- **The +1 stamp cannot live in `lv_map_upsert`/`lvrt_idxset`.** IndexStore is
  dk==1 — codegen retains on top; stamping inside the shared helper would make
  `m[k]=v` / `b[0]=9` results rc 2 (leak + permanently defeat COW: the s1/t1
  rows that are green today would regress to bug-15-shaped behavior).
- **An unconditional `retainDst()` in the row is wrong too:** it would take the
  in-place path's result to rc 2 (the consumed +1 plus the retain) — same leak,
  and the next `with` sees rc 2 → copies → COW dead again.

Post-fix, the in-place path becomes REACHABLE on LLVM for the first time
(`m = m.with(k, v)` self-append on a uniquely-owned map): review
`lv_map_upsert:909-916` (in-place old-value release/store/retain) as part of M2
— it has never run in production.

Out of scope, noted for the record: `array.add` with a CONSUMED but SHARED
(rc≥2, e.g. field-held) receiver leaks the consumed ref the same way
(`lv_arr_boxed_append`'s shared path never takes the fate, and the add row emits
no in.b-gated release). File it as its own bug.md entry during M4 rather than
widening this fix — its interaction with `:801`'s internal free (release-after-
internal-free reads a poisoned header) needs its own careful row design.

## 5. Milestones & acceptance

- **M1 — runtime.** 4.1 + `selftest.c` rows: (a) fresh-insert via
  `lvrt_map_with` → result `HDR[0]==1`, base untouched; (b) rc-1 found-key →
  same payload out, rc unchanged; (c) entries' counts balanced after a
  chained insert (build 2-entry, release result once → entry strings freed —
  assert via the 0xFE poison pattern like the existing `:728` block).
- **M2 — codegen.** 4.2 both rows. Acceptance: the §1 probe table goes all-✓ on
  LLVM with `--run`/`--ir`/`--build` unchanged; `LANG_ARC_TRACE` on r2 shows the
  owned lifetime (`R1` row, `R2` Ret, `r1` teardown, `r0` in the CALLER's
  frame); heap-meter `live-at-exit` for p1/r4-shaped programs drops vs. today
  (leak (2) and (4) retired).
- **M3 — corpus.** Land r2/q1/p1/p2/s3/t1 as corpus programs (all four engines;
  `.expected` from the oracle, which was always correct here). Re-run the full
  corpus — zero diffs expected outside the new cases.
- **M4 — unblock Track 09.** Re-enable the `tests/corpus/json` LLVM lane
  (bug.md #30's workaround) and confirm JSON object round-trip on
  `--build-native`; file the `array.add` consumed-shared-receiver bug (§4 tail)
  in `bug.md`; update the #30 entry (fixed) per the tracker's convention.

**Timeline:** one day, one agent. M1 ~2h, M2 ~3h, M3 ~2h, M4 ~1h.

## 6. STOP conditions

1. M3's corpus re-run diffs outside the new cases — something depended on rc-0
   maps (e.g. a hidden double-release the no-op was masking). STOP with the
   engine × case matrix and the ARC trace of the smallest diff.
2. The newly-reachable in-place path (M2 acceptance) shows a defect inside
   `lv_map_upsert:909-916` under LANG_ARC_TRACE (entry over/under-release).
   File it in `bug.md`; STOP only if it blocks the probe table from going
   green (a disabled in-place path — always-copy — is a valid fallback ONLY by
   owner ruling; do not silently ship it).
3. Any temptation to "fix" `lvrt_idxset` itself, change `destKind`, or touch
   the IndexStore stale-+1 discipline: STOP; re-read §4's three bullets. The
   green rows (s1/t1/q4) are the regression floor.
4. X64Gen/ELF questions (its `with` handling predates this contract): frozen
   backend, out of scope — note findings in the log, change nothing.

## 7. Implementation log

**2026-07-10 — all four milestones landed (agent1).** Implemented exactly as
specced in §4; no STOP condition (§6) triggered.

- **M1 — runtime.** Added `lvrt_map_with` in `runtime/lv_runtime.c` immediately
  after `lv_map_upsert` (verbatim §4.1): delegates to `lv_map_upsert`, then
  `lvrt_retain(out)` iff `out->payload != base->payload` (fresh copy → 0→1;
  in-place COW returns base's same payload and rides its consumed +1 through).
  `lvrt_idxset`/`lv_map_upsert` untouched. Declared it in `runtime/lv_abi.h`
  §2.7 natives and added the `map.with`/`map.without` ownership-table rows (§4.1).
  New `test_map_with` in `runtime/selftest.c` covers (a) fresh insert → result
  `HDR[0]==1`, base rc untouched, key owned by exactly the map; (b) rc-1
  found-key → same payload, rc unchanged; (c) chained 2-entry insert → per-map
  entry ownership balances, and a full release round-trip returns `live_bytes`
  to baseline (a stronger leak check than the poison read the spec suggested).
  `runtime_selftest` green; a standalone ASan+UBSan build of the runtime +
  selftest is clean.

- **M2 — codegen.** `src/LlvmGen.cpp`: bound `rtMapWith`
  (`lvrt_map_with`, `{ptrTy,ptrTy,ptrTy,ptrTy}`) in the fn-table region and added
  it to the `FunctionCallee` list. The `with` row now calls `rtMapWith` and, when
  the receiver is consumed, emits `if (dst.payload != recv.payload) rtRelease(recv)`
  (the in-place path leaves them equal and does nothing). The `without` row
  releases the consumed receiver unconditionally-on-fresh after its
  `rtRetain(lvA); copyLV(dst, lvA)`. **Implementation note (deviation from the
  §4.2 sketch's literal `in.b`):** `emitNativeRows` is defined *outside* the
  per-instruction loop, so it cannot capture the loop's `in`. Threaded the flag
  through instead — a new `bool consumed` parameter, passed `in.b != 0` at the
  single call site — and used the existing `dst`/`recv` params (which already
  equal `regs[in.a]`/`regs[in.c]`). Keeping the release INSIDE the `row(7,…)`
  (LV_MAP-tag-guarded) block is load-bearing: a call-site version keyed on
  `in.sname` would fire even when the runtime receiver isn't a map. Acceptance:
  the §1 probe table goes all-✓ on `--build-native` with `--run`/`--ir`/`--build`
  unchanged; `live-at-exit` for a returned-with-map matches the known-good
  in-frame baseline (288B — pre-existing top-level teardown, not a #30 leak).

- **M3 — corpus.** New `tests/corpus/map_return_ownership/` (one program covering
  r2/q1/s3/t1/p1/p2 — every §1 return-across-frame shape; the existing
  `map_with_without` corpus only ever exercised in-frame use, which is why the
  bug escaped). `.expected` generated from the oracle; oracle/IR/emit-C++/LLVM
  agree byte-for-byte. Registered treewalk/ir/cpp/llvm lanes in `CMakeLists.txt`.
  Full `ctest` re-run: zero diffs outside the new cases; the churn corpus
  (`corpus_churn_leak{,_llvm}`) stays green at +0 bytes (regression floor held).

- **M4 — unblock Track 09.** Re-enabled the `tests/corpus/json` LLVM lane
  (`corpus_json_llvm`) — JSON object round-trip on `--build-native` is now
  byte-identical to `.expected`. Filed the `array.add` consumed-shared-receiver
  leak (§4 tail) as `bug.md` #31 [P2]. Removed the (now-fixed) `bug.md` #30 entry
  per the tracker convention (fixed bugs live in git history) and updated the
  priority standings table.
