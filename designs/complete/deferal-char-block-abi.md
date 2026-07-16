# Deferral — `char` / `Block` native-backend ABI (`LV_CHAR` / `LV_BLOCK`)

**Status:** **LLVM legs + M4 I/O overloads LANDED 2026-07-10** (M1 char, M3
Block, and the `sys*`/`File` `Block` I/O overloads on oracle + IR + LLVM); only
the **ELF leg** of M4 remains deferred behind the X64Gen freeze. **Date:**
2026-07-08 (ratified), 2026-07-10 (LLVM legs + I/O overloads). **Owning track:**
Track B (runtime v2 ABI). **Still blocks:** the **ELF leg only** (`Block` heap
tag + ARC on the frozen `src/X64Gen.cpp`) — the I/O overloads no longer wait on
it, as they need only `LV_BLOCK` (landed) plus natives in the shared runtime.
**Source of record:** `designs/complete/techdesign-03-core-types.md` §9 (implementation log,
2026-07-06 currency-check RULING and 2026-07-07 STOP-resolved entry);
STOP-gating rule: `designs/complete/techdesign-portable-backend-2.md` §0.2
("contract changes are always STOP events").

## 2026-07-10 landing (LLVM legs — the addendum shipped)

`LV_CHAR = 10` and `LV_BLOCK = 11` now exist in `runtime/lv_abi.h` (the closed
0–9 set is extended by exactly two co-landed tags) with their full matching
implementation in `runtime/lv_runtime.c` and native emission in
`src/LlvmGen.cpp` — a contract change shipped whole, not half (doc-2 §0.2):

- **`LV_CHAR` (char, M1 LLVM leg):** pure immediate, `payload` = the Unicode
  scalar. `LoadConst`/`std::charFromCode`/`char.code()` retag inline in
  LlvmGen; `char.toString()`/`string.at`/`string.chars`/`to_string`/`print` go
  through `lvrt_char_to_string`/`lvrt_str_at`/`lvrt_str_chars` (UTF-8 en/decode
  byte-mirroring the oracle's `utf8Encode`/`utf8DecodeAt`). retain/release
  no-op on it, exactly like `int`. The LlvmGen `char`-class reachability skip is
  removed; a `corpus_chars_llvm` ctest lane diffs byte-identical vs the oracle.
- **`LV_BLOCK` (Block, M3 LLVM leg):** heap type, body
  `{parentPtr, off, len, dataPtr}`. A root owns its byte buffer (`dataPtr`); a
  slice sets `parentPtr` to the **root** and retains it, so a slice outliving
  its parent keeps the bytes alive (verified). `lv_recursive_free` releases
  `parentPtr` (slice) or frees `dataPtr` (root); `to_string` = `Block(len=N)`.
  Constructors/accessors are `lvrt_block_*` in `lv_runtime.c`, dispatched by
  tag-11 rows in `emitNativeRows`. A `corpus_blocks_llvm` lane diffs
  byte-identical vs the oracle; a block-churn probe holds live-at-exit flat
  (peak 896 B, live 288 B) from N=100 to N=10000 (no per-iteration leak).

Only **M4** (below, item 3) is still open, and only because `src/X64Gen.cpp`
is frozen under the portable-backend pivot — the ELF leg is not this landing's
to make.

---

## Why this is deferred, not done

`char` and `Block` are the only two Track 03 features that introduce a **new
runtime value kind**. On the interpreter engines (oracle, IR) and the emit-C++
engine, a new `VKind` is a local change and shipped in M1/M3 (landed 2026-07-08).
On the native backends the value kind is part of a **closed, contract-governed
ABI**:

- **LLVM** (`runtime/lv_abi.h`) — the `LV_*` tag enum is a **closed 0–9 set**.
  Adding `LV_CHAR`/`LV_BLOCK` extends that contract.
- **ELF** (`src/X64Gen.cpp`) — **frozen** under the portable-backend pivot
  (Track B's reference/bootstrap anchor; never extended). `Block` additionally
  needs retain/release/recursive-free + a `to_string` row in
  `runtime/lv_runtime.c` (the slice-retains-parent edge touches the allocator
  prefix contract).

Both `runtime/lv_abi.h` and `runtime/lv_runtime.c` are **Track B-owned**, and §2
ABI changes are STOP-gated: a contract change is never made unilaterally by a
language track. Shipping a half-implemented tag into a closed set that this track
cannot fully land + differentially verify in `lv_runtime.c` this session would be
worse than a logged deferral. So the ABI **shape is ratified** here for Track B to
implement; the implementation is deferred to a Track-B co-landed pass.

## Ratified ABI shape (for Track B to implement)

- **`LV_CHAR` = tag 10 — pure immediate.** `payload` = the Unicode scalar; no
  heap, no ARC (retain/release are no-ops). `lv_runtime.c` `to_string` row =
  UTF-8 encode the scalar. Trivial, but still extends the closed 0–9 set → must
  land in a Track-B pass, not here.
- **`LV_BLOCK` = tag 11 — heap type.** Header `{rc, meta}` + body
  `{parentPtr, off, len, dataPtr}`. A **slice sets `parentPtr` to the root and
  retains it**; `recursiveFree` releases `parentPtr`; `to_string` row =
  `Block(len=N)`. This touches the allocator prefix contract
  (`techdesign-portable-backend-2.md` §8 STOP condition 3) → genuinely
  Track-B-sized.

## What unblocks when the addendum lands

1. **M1 char LLVM leg** — ✅ **LANDED 2026-07-10.** `char` natives
   (`code`/`toString`/`charFromCode`, `string.at`/`chars`) on the LLVM backend;
   the LlvmGen `char`-class reachability skip is removed; `corpus_chars_llvm`
   ctest lane added (byte-identical vs oracle).
2. **M3 Block LLVM leg** — ✅ **LANDED 2026-07-10.** `Block` natives on LLVM
   (tag-11 rows in `emitNativeRows`, `lvrt_block_*` in `lv_runtime.c`);
   `corpus_blocks_llvm` lane added.
3. **M4 I/O overloads** — ✅ **LANDED 2026-07-10** (oracle + IR + LLVM). The
   `sys*`/`File` `Block` I/O overloads (reference §6.6.5) — `sysRead(fd, Block,
   max)`, `sysWrite(fd, Block, off, len)`, `sysRecv(fd, Block, max) -> int?`,
   `sysSend(fd, Block, off, len)`, and `File.read`/`File.write` `Block`
   overloads. `lvrt_sys*_block` in `lv_runtime.c`, arity-selected in LlvmGen
   (the `Block` forms differ in argument count from the string forms, so no
   runtime type-sniffing); interpreter branches by `VKind::Block`. Acceptance:
   `tests/corpus/blocks_io/` (deterministic — stdout + `/dev/zero`; bounds
   throw), on `corpus_blocks_io_{treewalk,ir,llvm}`. This half needed only
   `LV_BLOCK` (landed), so it never depended on the ELF leg despite the original
   plan bundling them.
4. **M4 ELF leg** — **STILL DEFERRED** (X64Gen frozen). `Block` on ELF (heap tag
   + ARC via `genRetain`/`genRelease`/`recursiveFree`, the slice-retains-parent
   edge). Not started — the ELF backend is the frozen bootstrap anchor and is
   never extended. This is the sole remaining piece of the deferral.

## Current state (what actually ships)

- **`char`**, `std::charFromCode`, `string.at`/`chars` — oracle + IR + emit-C++
  **+ LLVM** (2026-07-10).
- **`Block`** (full C4 API, aliasing slices, bounds-checked throws) — oracle + IR
  + emit-C++ **+ LLVM** (2026-07-10). ELF still deferred (M4 ELF leg).
- **`Block` I/O overloads** (`sys*`/`File`, zero-copy) — oracle + IR **+ LLVM**
  (2026-07-10). emit-C++ defers the file/socket layer as it does for the string
  forms; ELF frozen.
- **`enum`** — full-coverage including LLVM (desugars to struct + int + globals;
  no tag, not gated by this deferral).

Corpus: `tests/corpus/chars/`, `tests/corpus/blocks/` run on treewalk/ir/cpp
**and LLVM** (`corpus_chars_llvm`/`corpus_blocks_llvm`); `tests/corpus/blocks_io/`
runs on treewalk/ir **and LLVM** (`corpus_blocks_io_llvm`; no cpp lane — the
probe uses the file layer emit-C++ defers). No ELF lane by design (X64Gen frozen).
`tests/corpus/enums/` runs on all four including `corpus_enums_llvm`.
