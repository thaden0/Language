# Tech Design: Block Bulk Operations (`fill` / `blit` / `equals` / `mismatch`)

**Status:** **COMPLETE** — implemented 2026-07-12. **Date:** 2026-07-12. **Feature ID:** F1 (Sonar prerequisite set).
**Depends on:** none (Block landed, Track 03). **Unblocks:** Sonar T01 (Surface) + T09 (frame diff).
**Difficulty:** S. **Risk:** LOW — pure additive natives on the established Block recipe; no ABI, IR, or checker change.

Frozen surface: `designs/sonar/techdesign-00-overview.md` §4/F1. Ground truth: `designs/sonar/infodemp.md` §6 (file:line refs below are from that verified map; re-verify at implementation — lines drift).

---

## 1. Motivation

Sonar's `Surface` is one `Block` written thousands of times per frame (8 bytes/cell; a 200×50 terminal = 80,000 bytes), and its renderer diffs the frame against the previous one every present. In-language per-byte loops for fill/copy/compare run on the interpreter lanes at interpreter speed — a full-screen clear is 80k `setByte` native calls, a full diff is 80k `byteAt` pairs. Four natives replace those loops with `memset`/`memmove`/`memcmp`:

- `fill` — Surface.clear and pattern-fill substrate.
- `blit` — scrolling, prev-frame updates, pattern-doubling fills.
- `equals` — cheap "did anything change" gates.
- `mismatch` — the frame-diff primitive: the renderer scans difference runs (`i = cells.mismatch(prev, i)`) at native speed instead of comparing cells in-language.

## 2. Current state (verified map)

- Repr: tag `LV_BLOCK = 11` (`lv_abi.h:49`); native body `{parentPtr@0, off@8, len@16, dataPtr@24}` (`lv_abi.h:144`); byte address `lv_block_at(payload, i) = P8(dataPtr) + off + i` (`lv_runtime.c:1367`).
- Existing natives `lvrt_block_{new,fromstr,byteat,setbyte,slice,tostring,int32at,setint32,int64at,setint64}` at `lv_runtime.c:1380–1465`.
- Interpreter repr `BlockData { shared_ptr<vector<uint8_t>> bytes; size_t off; size_t len; }` (`RuntimeValue.hpp:28`); **`==` on Blocks is reference identity** (`RuntimeValue.hpp:237`). CGen repr `CGen.cpp:44`.
- Slices **alias the root** (`lvrt_block_slice` flattens to root and retains it, `lv_runtime.c:1411-1421`; interp shares `bytes`, `RuntimeNatives.cpp:593-599`; CGen same, `CGen.cpp:429-430`). Overlap is therefore a first-class case, not an edge case.
- Bounds discipline: check against **each view's own `len@16`**, never the underlying buffer; OOB throws via `lvrt_raise_oob` (`lv_runtime.c:2286`); `setByte` rejects values outside 0..255 with a custom message (`:1403-1407`).
- The 6-step recipe for a new Block method native (no new IR op — methods dispatch through `nativeCall`): prelude decl (`Resolver.cpp:120-132`) → C body (`lv_runtime.c` ~`:1465`) → shared oracle+IR clause (`RuntimeNatives.cpp:565` `cls == "Block"` branch — one clause covers both interpreters) → CGen clause (`CGen.cpp:419`) → LLVM triple: enum member + `fn(...)` binding (`LlvmGen.cpp:159/270-279`), `row(11, ...)` in `emitNativeRows` (`:1720-1724`), **and the name added to `kCovered[]` (`:1765-1775`)** → corpus under `tests/corpus/blocks/`.

## 3. Design

### 3.1 Surface (prelude `class Block` additions)

```lev
void fill(int off, int len, int value);
void blit(int dstOff, Block src, int srcOff, int len);
bool equals(Block other);
int  mismatch(Block other, int from);
```

### 3.2 Semantics (normative)

**`fill(off, len, value)`** — writes `value` into bytes `[off, off+len)`.
- `value` outside `0..255` → throws (same message shape as `setByte`'s range error).
- Bounds: requires `len >= 0`, `off >= 0`, `off + len <= this.len`; violation throws OOB.
- `len == 0` is a no-op, but `off` must still satisfy `0 <= off <= this.len` (inclusive upper bound for the empty write — matches C++ iterator convention; an empty fill at the end is legal, one past it is not).

**`blit(dstOff, src, srcOff, len)`** — copies `len` bytes from `src[srcOff..)` into `this[dstOff..)`.
- **memmove semantics, unconditionally**: correct when `this` and `src` are views of the same root with overlapping ranges (forward AND backward overlap). Scrolling a Surface is `blit` of a region onto itself shifted — the motivating overlap case.
- Bounds: each side checks against its own view's `len` (`dstOff + len <= this.len`, `srcOff + len <= src.len`); `len == 0` no-op with the same inclusive-offset rule as fill.
- Cross-root blits are ordinary copies. Self-blit (`b.blit(2, b, 0, n)`) is the documented overlap idiom.

**`equals(other)`** — content comparison of the full views.
- Lengths differ → `false` (no throw — equals answers a question, it doesn't assert a precondition).
- Two views of **different roots** with identical bytes → `true`. Two aliasing views → compares the (same) bytes.
- **Loud documentation requirement**: `equals` is CONTENT equality; the `==` operator on Blocks remains reference identity (`RuntimeValue.hpp:237`, and Block is a reference type by design). The prelude doc comment and reference.md §6.10 addendum must state the pair explicitly — this is the one API-confusion hazard of the feature.

**`mismatch(other, from)`** — first index `i >= from` where `this[i] != other[i]`, else `-1`.
- **Requires equal lengths**; unequal → throws (unlike `equals`: mismatch is a scan over a shared index space; a length mismatch is a caller bug, not an answer).
- `from` range: `0 <= from <= len` (inclusive: `from == len` returns `-1` — lets the renderer's scan loop terminate without a pre-check).
- Intended use (normative example for the doc comment):
  ```lev
  int i = cells.mismatch(prev, 0);
  while (i >= 0) {
      int runEnd = i;
      while (runEnd < cells.length() && cells.byteAt(runEnd) != prev.byteAt(runEnd)) runEnd = runEnd + 1;
      // emit cells[i..runEnd), then:
      i = cells.mismatch(prev, runEnd);
  }
  ```
  (The run-extension inner loop stays in-language v1; if profiling demands, a `matchFrom` inverse native is the v2 addition — noted in §8.)

`length()` already exists; no other surface is added. A frame-diff helper that emits escape runs was considered and **rejected** — that logic is Sonar's (`.lev`), the native surface stays minimal primitives (per the review ruling in the research set).

## 4. Implementation plan

| M | step | detail | difficulty |
|---|---|---|---|
| M1 | Prelude decls | 4 signatures in `class Block`, `Resolver.cpp:120-132` region | S |
| M2 | C bodies | `lv_runtime.c` after `:1465`: `lvrt_block_fill` (bounds + value check → `memset(lv_block_at(p,off), value, len)`); `lvrt_block_blit` (both-side bounds → `memmove`); `lvrt_block_equals` (len compare → `memcmp == 0`); `lvrt_block_mismatch` (len-equality throw, `from` check → `memcmp` on the suffix; on nonzero, byte-scan the window to localize the index — or scan in 64-byte `memcmp` chunks then bytes; either is fine, chunked is recommended) | S |
| M3 | Oracle+IR clause | one `cls == "Block"` clause each in `RuntimeNatives.cpp:565` branch: `std::memset`/`std::memmove` on `&bytes->at(off + view.off)` etc. (shared vector: self-blit via memmove on the same buffer is well-defined); `std::memcmp` for equals/mismatch; reuse the `oob` lambda (`:573-577`) and mirror the setByte value-message (`:584-591`) | S |
| M4 | CGen clause | `CGen.cpp:419` `self.k == 11` branch, same shapes over its BlockData | S |
| M5 | LLVM triple | 4 enum members + `fn` bindings (`rtBlockFill = fn("lvrt_block_fill", voidTy, {ptrTy,i64Ty,i64Ty,i64Ty})` etc. — mismatch returns i64, equals returns the bool-carrying LvValue out-param per the existing row conventions — match how `byteat` returns); 4 rows in `emitNativeRows` (`row(11, ...)` at `:1720-1724`); **4 names into `kCovered[]` (`:1773`)** | S/M |
| M6 | Corpus | `tests/corpus/blocks/bulk_*.lev` + `.expected`, plus the `blocks_llvm` symlink convention if present in-tree (follow the existing blocks tests' layout exactly) | S |

No ELF lane (X64Gen frozen — natives fall through per the established pattern; nothing gates on it).

## 5. Edge cases & failure modes

- Zero-length ops at every boundary (`off == len`), OOB one past it.
- Backward overlap self-blit (`blit(0, self, 2, n)`) and forward (`blit(2, self, 0, n)`) — both must match a reference in-language copy-via-temp.
- Slices: fill through a slice must not touch bytes outside the slice's window; blit between two overlapping slices of one root; equals/mismatch on slices at different offsets of the same root.
- `mismatch` where the difference is at index 0, at `len-1`, nowhere, exactly at `from`, and before `from` (must be skipped).
- Value 256/-1 to `fill` → the range throw; negative `len`/`off`/`from` → OOB throw.
- Empty Blocks (`Block(0)`): equals of two empties → true; mismatch → -1; fill/blit len 0 legal.

## 6. Potential issues & mitigations

1. **`kCovered[]` drift (the known trap).** A native missing from the whitelist silently falls through to class dispatch and fails at runtime on LLVM only. Mitigation: M5's checklist pairs every row with its `kCovered` entry; the corpus runs on LLVM so the failure is caught pre-merge regardless.
2. **Interpreter self-blit aliasing.** `BlockData.bytes` is a shared `vector`; overlapping `std::copy` would be UB — use `std::memmove` on raw pointers into the vector, never iterator copies. Stated in M3.
3. **int64 truncation at the native boundary.** Offsets/lengths arrive as language ints (i64); C bodies take `int64_t` and bounds-check BEFORE any cast/pointer math (negative values must hit the OOB throw, not wrap). Same discipline as existing block natives.
4. **OOB message consistency across engines.** Differential tests compare output; the throw messages must match the existing `lvrt_raise_oob` shape on all lanes (the interpreters' `oob` lambda already mirrors it — reuse, don't restate).
5. **`equals` vs `==` confusion.** §3.2's documentation requirement; plus one corpus test that pins both behaviors side by side so the distinction is executable documentation.
6. **`mismatch` chunked-scan off-by-one.** The chunk-then-localize approach must return the FIRST differing index, not the chunk start. The corpus's "difference at every position of a 130-byte block" sweep test pins it.

## 7. Testing plan

`tests/corpus/blocks/`: `bulk_fill.lev` (whole/partial/zero-len/through-slice + range and OOB throws), `bulk_blit.lev` (cross-block, forward/backward self-overlap, slice-to-slice same root, zero-len), `bulk_equals.lev` (same/different roots, length mismatch → false, empties, the `==`-vs-`equals` pin), `bulk_mismatch.lev` (position sweep via a loop over a 130-byte block, `from` semantics incl. `from == len`, length-mismatch throw), each with `.expected`; all four run oracle/IR/emit-C++/LLVM byte-identical. Micro-benchmark note (not a test): time a 1M-byte fill+diff loop pre/post on the IR lane for the design log.

## 8. Open questions

1. `matchFrom(other, from) -> int` (first EQUAL index — the inverse, for native run-extension) — add only if Sonar's renderer profiling shows the in-language run-extension loop matters. v1.1 candidate, same recipe.

## 9. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — M1–M6 landed: the prelude, C runtime/ABI, shared oracle+IR
  native clauses, emit-C++ runtime, LLVM declarations/rows/coverage whitelist,
  reference documentation, and four focused corpus programs. Bounds checks are
  overflow-safe and precede pointer arithmetic; `blit` uses `memmove` in every
  engine; `mismatch` scans equal 64-byte chunks before byte localization.
- 2026-07-12 — the executable `equals`/`==` distinction exposed and fixed a
  pre-existing oracle/IR/emit-C++ Block-operator bug: those lanes fell through
  scalar comparison and treated distinct Blocks as equal because their unused
  integer fields were both zero. Block `==`/`!=` now compare view identity,
  matching LLVM and `RuntimeValue::keyEquals`; `equals` compares content.
- 2026-07-12 — acceptance: `corpus_blocks_treewalk`, `corpus_blocks_ir`,
  `corpus_blocks_cpp`, and `corpus_blocks_llvm` pass byte-identically (five
  Block programs per lane). The mismatch corpus checks every differing position
  in a 130-byte view, including both sides of the 64-byte chunk boundary.
  A release-build IR smoke benchmark of 200 iterations over 1,000,000 bytes
  (`fill` + `blit` + equal `mismatch`, 600 MB of bulk work) completed in 0.05 s
  wall time on the implementation host; this is a sanity datapoint, not a
  stable performance gate.
