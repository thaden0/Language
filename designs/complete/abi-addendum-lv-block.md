# ABI Addendum — `LV_CHAR` (tag 10) / `LV_BLOCK` (tag 11)

**Status:** **LANDED** on oracle + IR + LLVM (2026-07-10). M1 char leg, M3 Block leg
(commit `bb580cd`), and the M4 `sys*`/`File` `Block` I/O overloads (§6, this pass) are
all implemented and corpus-verified. **Only the ELF leg** (Block ARC on the frozen
`src/X64Gen.cpp`) remains deferred — see `designs/deferal-char-block-abi.md`.
**Date:** 2026-07-10. **Owning track:** Track B (runtime v2 ABI).
**Promotes:** the "Ratified ABI shape" section of `designs/deferal-char-block-abi.md`
into a complete, normative contract.
**Normative home:** `runtime/lv_abi.h` (§2.4 heap-layout block, tag enum) +
`runtime/lv_runtime.c` (ARC + the four `lvrt_sys*_block` natives).

---

## 1. Scope and the two governing constraints

This addendum extends the **closed** `LV_*` tag enum in
[runtime/lv_abi.h:26](../runtime/lv_abi.h#L26) (`LV_VOID=0 … LV_CLO=9`) with two new
tags. It is the contract for **Track 03 M1's char LLVM leg**, **M3's Block LLVM leg**,
and the **LLVM-scoped half of M4** (the `sys*`/`File` `Block` I/O overloads).

**① LLVM-only. The ELF leg of M4 is dropped.** The original M4
(`techdesign-03-core-types.md` §6, "Block on **ELF** + ARC + I/O overloads") predates
the portable-backend pivot. `src/X64Gen.cpp` (ELF) is **frozen** — never extended —
so "Block on the native backends" resolves to **LLVM only**. ELF keeps its clean
coverage-deferral for `Block`/`char`; this addendum does not touch it.

**② Atomic, STOP-gated Track B change.** Adding tag 10/11 to a closed set and adding
`LV_BLOCK` to the shared ARC in `lv_runtime.c` is a **contract change** — an automatic
STOP event (`techdesign-portable-backend-2.md` §0.2), and it touches the allocator-prefix
contract (§8 STOP condition 3). Per the deferral doc's own warning, a **half-implemented
tag in the closed set is worse than a deferral**. Therefore:

> The enum extension (`lv_abi.h`) and the ARC/native implementation (`lv_runtime.c`,
> `LlvmGen.cpp`) land **together, atomically**, in one Track B co-landed pass. The
> closed set is never in a "declared but unimplemented" state. This document is the
> ratified target for that pass; writing it does not itself add the tags.

Everything in §63 of the deferral doc's "Current v1 state" still holds: `char` and
`Block` already ship **fully on oracle + IR + emit-C++ (CGen)**. This addendum adds
only the **LLVM legs**; the interpreter/CGen implementations are the differential
reference the LLVM legs are checked against and are **not** re-touched.

---

## 2. `LV_CHAR` = tag 10 — pure immediate

```
char (LV_CHAR): payload = the Unicode scalar value (a code point, 0 .. 0x10FFFF).
                No heap allocation. No ARC. payload is the value itself, like
                LV_INT/LV_BOOL — never a pointer.
```

- **ARC:** none. `char` is **not** a counted tag. `lv_is_counted`
  ([lv_runtime.c:6](../runtime/lv_runtime.c#L6)) already excludes it *by omission*
  (10 is not in the `{LV_STR, LV_OBJ, LV_ARR, LV_MAP, LV_CLO}` whitelist), so
  `lvrt_retain`/`lvrt_release` are correct no-ops with **no edit**. Track B must **not**
  add `LV_CHAR` to that whitelist.
- **`to_string` row:** UTF-8-encode the single scalar into a fresh `LV_STR` (rc=0),
  1–4 bytes. `console.write('A')` → `A`.
- **No arithmetic.** `char` carries no operators (info.md §9); comparisons are by scalar
  value. This is a codegen fact (the checker already enforces it), not an ABI field.

`char` is trivial precisely because it is an immediate — it is the low-risk warm-up leg;
`Block` (§3) is the real work.

---

## 3. `LV_BLOCK` = tag 11 — heap byte buffer

The native realization of CGen's `BlockData { shared_ptr<vector<uchar>> bytes; off; len; }`
(`src/CGen.cpp:44`), expressed in ARC. A **root** owns a byte buffer; a **slice** is an
aliasing view that shares the root's bytes and keeps the root alive by refcount.

### 3.1 Record layout (normative)

`payload` points at the record **body** (the standard 16-byte `LvHeader {rc, meta}` sits
at `payload-16`, reached by `HDR(payload)`, exactly as for every other heap tag). The body
is **four `int64` fields, 32 bytes, fixed size for both roots and slices**:

```
block (LV_BLOCK): payload -> { int64_t parentPtr;   // offset  0
                               int64_t off;         // offset  8  — absolute, from dataPtr base
                               int64_t len;          // offset 16  — this view's byte length
                               int64_t dataPtr; }    // offset 24  — base of the ROOT byte buffer
   meta (HDR[1]) = total raw allocation bytes of the RECORD (48: 16 header + 32 body).
```

Byte `i` of any block (root or slice) is `*(uint8_t*)(dataPtr + off + i)`, for `0 <= i < len`.
`dataPtr` **always points at the root's buffer base**; `off` is the absolute offset of this
view within that buffer. Slice-of-slice never chains (see §3.3).

### 3.2 The byte buffer

The actual bytes live in a **separate raw allocation** owned by the root — `lvrt_halloc(size,
LV_TIER_HEAP)` with **no ARC header** (like the 32-byte dyn-nodes, §2.4). It is freed by the
root's `recursiveFree` via `lv_free_raw(dataPtr, rootLen)` (§4.2). *Implementation latitude:*
Track B may instead inline the bytes into the root record (making the root record
variable-size and `dataPtr = payload + 32`) provided the observable contract in §3.1/§3.3
and the ARC invariant in §4.3 are preserved bit-for-bit; the separate-buffer form is the
normative reference because it keeps records fixed-size for the size-classed allocator.

### 3.3 Construction

- **`Block(int size)`** (root, zeroed): allocate a 48-byte record + a `size`-byte zeroed raw
  buffer. `parentPtr = 0` (the root marker), `off = 0`, `len = size`, `dataPtr = buffer base`.
  Record `rc` starts at 0 (fresh/unowned), like every fresh heap value.
- **`Block::fromString(string s)`** (root): as `Block(s.len)`, but the buffer is initialized by
  copying `s`'s bytes. `parentPtr = 0`, `off = 0`, `len = s.len`.
- **`b.slice(int o, int l)`** (aliasing view): bounds-check `o >= 0 && l >= 0 && o + l <= b.len`
  (else throw, §3.4). Allocate a new 48-byte record. **`parentPtr = root(b)`** — the root, never
  `b` itself when `b` is already a slice (flatten: `root(b) = b.parentPtr ? b.parentPtr : b`).
  **Retain the root** (`lvrt_retain` on `{LV_BLOCK, root}`) — this is the edge that keeps the
  bytes alive while the view exists. `off = b.off + o`, `len = l`, `dataPtr = b.dataPtr`
  (= root's buffer base).

### 3.4 Byte / integer natives (bounds-checked, LOUD)

Semantics are **identical** to the CGen reference at [src/CGen.cpp:419](../src/CGen.cpp#L419)
and the reference-doc table (`docs/reference.md` §6.10). Every access is bounds-checked and
throws a real `RuntimeException` on out-of-range offset/length (§3.7 loudness), never masks:

| native | contract |
|---|---|
| `length() -> int` | `len` |
| `byteAt(int i) -> int` | `dataPtr[off+i]` as `0..255`; `i` in `0..len-1` else throw |
| `setByte(int i, int v)` | store; `i` in range **and** `v` in `0..255` else throw (not masked) |
| `slice(int o, int l) -> Block` | §3.3 aliasing view |
| `toString(int o, int l) -> string` | copy `l` bytes from `off+o` into a fresh `LV_STR` |
| `int32At(int i)` / `setInt32(int i, int v)` | little-endian 4-byte; read **sign-extends** `(int32_t)`; needs `i+4 <= len` |
| `int64At(int i)` / `setInt64(int i, int v)` | little-endian 8-byte; needs `i+8 <= len` |

Writes through a slice mutate the shared buffer and are visible in the parent and in every
other view (zero-copy, by design).

---

## 4. ARC contract — the exact `lv_runtime.c` edits

Two edits, both in the shared runtime. This is the delicate part (the design's §5 risk row 5:
"a bug here corrupts live views").

### 4.1 `lv_is_counted` — count `LV_BLOCK`

[lv_runtime.c:8](../runtime/lv_runtime.c#L8) — extend the counted-tag whitelist so the region
check and rc transitions engage for blocks:

```c
if (tag != LV_STR && tag != LV_OBJ && tag != LV_ARR && tag != LV_MAP
    && tag != LV_CLO && tag != LV_BLOCK)   /* <-- add LV_BLOCK */
    return 0;
```

The mandatory region check (`lv_in_region`) and the fresh/arena `rc` gates in
`lvrt_retain`/`lvrt_release` then apply to blocks unchanged. `LV_CHAR` stays **out** of this
list (§2).

### 4.2 `lv_recursive_free` — the `LV_BLOCK` case

[lv_runtime.c:237](../runtime/lv_runtime.c#L237) — add a branch. It releases the parent for a
slice, or frees the owned buffer for a root, then frees the record:

```c
} else if (tag == LV_BLOCK) {
    int64_t parentPtr = lv_ld_i64(payload, 0);
    if (parentPtr) {                              /* slice: give the root back its refcount */
        LvValue parent = { LV_BLOCK, parentPtr };
        lvrt_release(&parent);                    /* may cascade to free the root + buffer */
    } else {                                      /* root: free the owned raw byte buffer */
        int64_t dataPtr = lv_ld_i64(payload, 24);
        int64_t len     = lv_ld_i64(payload, 16); /* root off==0, so len == buffer size */
        if (dataPtr) lv_free_raw(dataPtr, len);
    }
    lv_free_raw(P8(payload) - 16, HDR(payload)[1]);   /* the 48-byte record */
}
```

A slice must **not** free `dataPtr` (it belongs to the root); a root's `len` is its buffer size
because a root's `off` is always 0 and `Block` is fixed-length (slices only ever create new
records, never mutate a root's `off`/`len`).

### 4.3 The keep-alive invariant (normative)

> Every live slice holds exactly **one** `+1` on its root's record. The root's record — and
> therefore its byte buffer — is reachable (rc > 0, unpoisoned) for as long as any slice or
> direct binding of the root is live. The buffer is poisoned (`0xFE` fill) and freed **only**
> when the root's rc reaches 0, i.e. after the last slice and the last root binding release.

This is what makes "parent freed while a view is outstanding" **impossible**, resolving the
allocator-prefix / hfree-poisoning STOP condition (deferral §"Why deferred"; design §5 risk 5,
§8 condition on the prefix contract). The `block_churn` acceptance test (§9) is the differential
proof: a slice deliberately outlives its parent binding and the bytes stay valid, at **+0B** net.

---

## 5. `to_string` / print rows

Extend `lvrt_to_string` / `lvrt_print_val` ([lv_abi.h:308](../runtime/lv_abi.h#L308)) — one row
each, matching the interpreter/CGen reference exactly:

- **`LV_CHAR`** → UTF-8 encoding of the scalar (§2).
- **`LV_BLOCK`** → the literal string `Block(len=N)` where `N = len` (the **view's** length, so a
  slice prints its own length), matching `src/CGen.cpp:156`.

---

## 6. `sys*` / `File` `Block` I/O overloads (the LLVM-scoped M4 half)

`reference.md` §6.6.5 reserves four zero-copy `Block` overloads; overload resolution by argument
type already keeps the existing string forms untouched when these land. They are **new native
seam entries** alongside the string forms in `lv_abi.h` (§lvrt_sys* block), and they **borrow**
the `Block` (the "sys* borrow args" contract row) — no ARC transfer. Offsets/lengths are
bounds-checked against `block.len`; out-of-range throws `RuntimeException` (§3.7), matching the
Block natives' discipline.

| language overload | native | direction / return |
|---|---|---|
| `std::sysRead(fd, Block b, int max) -> int` | `lvrt_sysread_block(out, fd, b, max)` | reads up to `min(max, b.len)` bytes into `b` at `off 0`; returns bytes read (short reads legal, `0` at EOF) |
| `std::sysWrite(fd, Block b, int off, int len) -> int` | `lvrt_syswrite_block(out, fd, b, off, len)` | writes `b[off .. off+len)` to `fd`; returns byte count |
| `std::sysRecv(fd, Block b, int max) -> int?` | `lvrt_sysrecv_block(out, fd, b, max)` | recv into `b` at `off 0`; **three-way**: `LV_INT` bytes / `LV_INT 0` / `LV_NONE` on peer-close (mirrors the string `lvrt_sysrecv` narrowing, [lv_abi.h:428](../runtime/lv_abi.h#L428)) |
| `std::sysSend(fd, Block b, int off, int len) -> int` | `lvrt_syssend_block(out, fd, b, off, len)` | sends `b[off .. off+len)`; raw byte count (may be negative on error/would-block, like `lvrt_syssend`) |

`File.read` / `File.write` gain `Block` overloads **in-language** over this floor (no new native
beyond the four above). The string-carried forms (`sysRandom`, etc.) are unchanged.

---

## 7. Codegen (`LlvmGen.cpp`) work list

Not code here — the enumerated target for the Track B pass:

1. **Drop the `char` exclusion** at [LlvmGen.cpp:438](../src/LlvmGen.cpp#L438) (`if (cls &&
   cls->name == "char") continue;`) and the parallel Block gap, so the prelude `char`/`Block`
   classes enter the LLVM lane.
2. **Construct** `LV_CHAR` immediates (scalar in `payload`) and `LV_BLOCK` roots/slices
   (`Op::NewBlock`, `Op::NewBlockStr`, `slice`) per §3, threading the **slice-retains-parent**
   retain through the existing LLVM ARC scaffolding (the arena/`copyValArena`/retain-release
   machinery already mirrors `lv_runtime.c`).
3. **Dispatch** the Block byte/integer natives (§3.4) and the four `sys*` `Block` overloads (§6),
   emitting the same bounds-check-and-throw shape LlvmGen already uses for `Array` OOB.

The runtime-side ARC (§4) is shared C in `lv_runtime.c`, written once and linked by both native
backends — LlvmGen emits `call`s to `lvrt_retain`/`lvrt_release`/the new sys natives; it does not
reimplement ARC.

---

## 8. The atomic Track B change set (files & anchors)

| file | edit |
|---|---|
| `runtime/lv_abi.h` | add `LV_CHAR = 10, LV_BLOCK = 11` to the enum ([:26](../runtime/lv_abi.h#L26)); add the §2/§3.1 layout prose to the §2.4 block; declare the four `lvrt_sys*_block` natives |
| `runtime/lv_runtime.c` | §4.1 `lv_is_counted` whitelist; §4.2 `lv_recursive_free` `LV_BLOCK` case; §5 `to_string` rows; implement the four `lvrt_sys*_block` natives |
| `src/LlvmGen.cpp` | §7 (drop exclusion, construct, dispatch) |
| `runtime/selftest.c` (if present in the ABI selftest set) | rc-transition assertions for the slice-retains-parent edge |

All in **one commit series**; the closed set is never partially extended.

---

## 9. Acceptance criteria & corpus lanes

Files are `.lev` (never `.ext`). The interpreter/CGen `tests/corpus/blocks/` and
`tests/corpus/chars/` corpora are the **differential oracle** — LLVM output must match them.

1. **`corpus_chars_llvm`** — the existing `chars/` corpus runs on the LLVM backend
   (`code`/`toString`/`charFromCode`, `string.at`/`chars`), matching the oracle. (M1 char leg.)
2. **`corpus_blocks_llvm`** — the existing `blocks/` corpus on LLVM: bounds throws, slices &
   aliasing (write-through visible in parent), int32/64 LE round-trip, `fromString`/`toString`.
   (M3 Block leg.)
3. **`block_churn.lev` at +0B** — alloc/slice/drop in a loop, **with a slice deliberately
   outliving its parent binding**, run under `--mem-verify` and the LLVM heap meter. Proves §4.3.
4. **`sysReadBlock`/`sysRecvBlock` probe** — a program exercising the §6 overloads (e.g. read a
   file into a `Block`, `toString` it back; the three-way `sysRecv` narrowing). (M4 I/O half.)

---

## 10. Non-goals

- **ELF/X64Gen** — frozen; `Block`/`char` keep their clean coverage-deferral there. Not in scope.
- **Interpreter / CGen** — already shipped (deferral §63); untouched except as the differential
  reference.
- **`char` arithmetic**, `Block` growth/resize, or a `Block`-backed `sysStat` — out of scope
  (info.md §9; reference §6.6.5 "interim shape").
