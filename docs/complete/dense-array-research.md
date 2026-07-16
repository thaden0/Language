# Columnar / dense `Array<struct>` — technical research dossier

**Status:** pre-design research. Not a tech design. This is the complete technical
substrate a tech design for the columnar-storage track (`designs/requests/request-columnar-dense-array-struct.md`)
must build on: what exists today, exactly how it is represented on every engine, what
machinery it touches, the hard problems columnar introduces, and the questions the design
has to answer. Every claim below is anchored to code (`file:line`) verified against the
current tree, not to prose in `info.md`/`reference.md`.

Audience: whoever writes the tech design (a Fable-class author per
`[[feedback_techdesign-conventions]]`), and the Sonnet-class implementers after them.

---

## 0. The one-paragraph orientation

"Dense `Array<struct>`" already exists and is **row-major** (array-of-structs / AoS): each
element is a contiguous inline record, laid out one after another in a single heap buffer,
no per-element boxing or pointer chase. **Columnar** (struct-of-arrays / SoA) is a *different
physical layout* that does not exist yet: each *field* gets its own contiguous run, so
`Array<Point>` becomes physically "all the `x`s, then all the `y`s" instead of
"`{x,y}`, `{x,y}`, …". The payoff is column-selective locality — `sum(pts.map(p => p.x))`
touches only the `x` run — which is the analytical/bulk-data workload the language's
`struct` + dense-array + `mmap`-via-`Block` story (info.md §9/§13/§17) is explicitly betting
on as its differentiating data feature. The row-major dense layout is implemented in the
**two native backends only** (LLVM primary, emit-C++; plus the frozen X64Gen reference);
the tree-walk oracle and IR interpreter never went dense — they represent value-struct
arrays as ordinary vectors of boxed objects and rely on differential testing to stay honest.
That asymmetry is the single most important architectural fact for scoping this work.

---

## 1. Terminology, pinned

| term | meaning here |
|---|---|
| **row-major / AoS** | today's dense array: one buffer, element `i` at `base + 16 + i*recBytes`, each record a full inline object body. |
| **columnar / SoA** | proposed: field `f` stored in its own contiguous run; element `i`'s `f` at `columnBase(f) + i*stride(f)`. |
| **boxed array** | the non-dense representation: `[len][LvValue elems…]`, each element a 16-byte tagged value (a pointer for objects). What `Array<int>`, `Array<string>`, `Array<SomeClass>` use. |
| **record** | one element's inline bytes in a dense buffer: `[classId(8)][dyn(8)][slot0(16)]…[slotN(16)]`. |
| **slot** | one field's storage inside a record: a 16-byte `{tag, payload}` `LvValue`, regardless of field type. |
| **value struct** | a `struct` (info.md §9): copied on bind/pass/return/store, no identity, final. `Symbol::isValue == true`. The *only* element kind that goes dense. |
| **the record-pointer trick** | reading `arr[i]` on a dense array returns a tag-5 `LvValue` whose payload points **into** the buffer — no materialization. Field access then adds a fixed slot offset. This is exactly what columnar breaks. |

---

## 2. The engine architecture and where "dense" lives

The compiler front-end (lexer → Pratt parser → resolver/shapes → checker) lowers to one
register-machine bytecode IR, the single semantic truth. **Five** consumers, **four active**:

| # | engine | file | value-struct array representation |
|---|---|---|---|
| 1 | tree-walk oracle | `src/Eval.cpp` + `src/RuntimeValue.hpp` | **boxed**: `std::vector<Value>`, each element a `VKind::Object` with a `fields` map. No dense. |
| 2 | IR interpreter | `src/IrInterp.cpp` (shares `RuntimeValue.hpp`) | **boxed**: same `std::vector<Value>`. No dense. |
| 3 | emit-C++ | `src/CGen.cpp` | **dense (flat)**: one `std::vector<V>` of `fieldCount(cls)` slots per element; `aelem` = classId. |
| 4 | LLVM (primary AOT) | `src/LlvmGen.cpp` + `runtime/lv_runtime.c` + `runtime/lv_abi.h` | **dense (ABI)**: the `[lenWithBit][recBytes][records…]` buffer. |
| 5 | pure x86-64/ELF | `src/X64Gen.cpp` | **dense** — **FROZEN, reference-only.** Never extended. Not a target. |

**Consequences that drive the whole design:**

- **The interpreters (oracle, IR) never need columnar.** They store value-struct arrays as
  vectors of boxed objects and get correct observable behavior for free. Whatever columnar
  layout the native backends adopt is validated *against* the oracle's boxed semantics via
  the differential corpus. So "columnar across oracle/IR/emit-C++/LLVM" (the ticket's
  phrasing) really means: **new codegen in LlvmGen + CGen + runtime; zero change to the
  interpreters except that their output must still match.** This is the same shape the
  row-major dense work took — `grep -rl dense src/` hits `Ownership/Lower/X64Gen/LlvmGen/CGen/Project`
  but **never** `Eval.cpp` or `IrInterp.cpp`.
- **X64Gen is frozen** (info.md §0/§17, `[[feedback_x64gen-frozen]]`). No columnar lane on
  ELF. No new ELF corpus. No design gated on an ELF finding. The frozen row-major code stays
  as-is; the design references it only as the reference implementation.
- **The real work is two backends + one runtime contract.** LLVM (`LlvmGen.cpp`,
  `lv_runtime.c`, `lv_abi.h`) and emit-C++ (`CGen.cpp`, self-contained).

---

## 3. The current row-major dense array — complete specification

### 3.1 The ABI contract (`runtime/lv_abi.h`)

The normative generated↔runtime boundary. Both the LLVM codegen (Track A) and the C runtime
(Track B) code against this file; **neither may change it unilaterally — contract changes are
STOP events** (`techdesign-portable-backend-2.md` §0.2). Columnar is a contract change, so
by construction it is a Fable-authored, STOP-gated design.

**Value cell** (`lv_abi.h:36`): `typedef struct LvValue { int64_t tag; int64_t payload; } LvValue;` — 16 bytes.
Tags (`lv_abi.h:38-53`): `VOID=0 INT=1 FLOAT=2 BOOL=3 STR=4 OBJ=5 ARR=6 MAP=7 NONE=8 CLO=9 CHAR=10 BLOCK=11 WEAK_PROXY=12`.

**Array — two representations, bit 63 of the first body word discriminates** (`lv_abi.h:109-129`):

```
BOXED:  payload -> { int64_t len; LvValue elems[capacity]; }
        header meta = capacity (slot count). idxset COW: in place at rc==1, else copy.

DENSE:  payload -> { int64_t lenWithBit; int64_t recBytes;
                     uint8_t records[cleanLen * recBytes]; }
        lenWithBit = cleanLen | (1<<63).
        Each record is a full inline object body (classId, dyn=0, declared slots).
        Reading an element yields a tag-5 LvValue whose payload POINTS INTO the buffer.
        Dense arrays only ever hold value-struct elements (never refcounted), so
        recursiveFree does no per-element work — just frees the buffer.
```

**Object record shape** (`lv_abi.h:95-107`): `{ int64_t classId; int64_t dyn; LvValue slots[nslots]; }`,
slot `i` at byte offset `16 + 16*i`. So one record = `16 + 16*nslots` bytes; a dense element
carries the classId and the `dyn` head **per row** even though both are invariant across the
array, plus a full 16-byte tagged slot per field even for an `int` (8 bytes of which is a
redundant tag). This redundancy is exactly the memory the columnar layout can reclaim (§6.5).

**Tag-5 aliasing caveat** (`lv_abi.h:61-63`): "Tag-5 payloads may point INLINE into a dense
array buffer … never assume a tag-5 pointer owns an allocation header; always isvalueclass()-gate
before touching `[payload-16]`." This is the load-bearing invariant that lets the record-pointer
trick work without ARC corrupting the buffer.

### 3.2 Construction — how an array becomes dense

Denseness is **discovered dynamically, not declared**: an empty boxed array flips to dense on
the first value-struct append.

`lvrt_arr_append` (`lv_runtime.c:1701`):
```c
void lvrt_arr_append(LvValue* out, const LvValue* arr, const LvValue* val) {
    int64_t rawlen = lv_ld_i64(arr->payload, 0);
    if (rawlen < 0) { lv_arr_dense_append(out, arr, val); return; }        // already dense
    if (rawlen == 0 && val->tag == LV_OBJ && lvrt_isvalueclass(lv_ld_i64(val->payload,0))) {
        lv_arr_go_dense(out, val);  return;                                 // first struct -> go dense
    }
    lv_arr_boxed_append(out, arr, val);                                     // stays boxed
}
```

`lv_arr_go_dense` (`lv_runtime.c:1633`): reads `classId` from the first element, computes
`recBytes = 16 + 16*fieldcount(classId)`, allocates `16 + recBytes`, sets `lenWithBit = 1|(1<<63)`,
`recBytes` in body word 1, `memcpy`s the element's record in, stamps header rc=1 (owned, +1
transfer contract).

`lv_arr_dense_append` (`lv_runtime.c:1650`): **always copies + grows** — allocates
`16 + (cleanLen+1)*recBytes`, memcpy old records, memcpy new record. **Never frees the old
buffer** — deliberately replicating X64Gen's `genArrAppend` DENSE branch for differential
parity; this is a known leak edge (see §5, the `dense_index_set` XFAIL).

`NewArraySized` (the `Array(n, fill)` path, `LlvmGen.cpp:2616-2628`): loops `n` times doing
`Copyval(fill)` then `arr_append`; the struct copy is memcpy'd into the dense record and then
`Vfree`d (dead standalone copy).

emit-C++ equivalent (`CGen.cpp:179-186`): `arrPush` — if the array is empty and the pushed
value is a value class, set `aelem = classId`; thereafter store `fieldCount(aelem)` flat slots
per element into one `std::vector<V>`.

### 3.3 Element read — the record-pointer trick

`lvrt_idxget` dense branch (`lv_runtime.c:1759-1765`):
```c
if (rawlen < 0) {   /* dense */
    int64_t i = idx->payload;
    /* bounds-check against cleanLen */
    int64_t recBytes = lv_ld_i64(base->payload, 8);
    out->tag = LV_OBJ;
    out->payload = I64(P8(base->payload) + 16 + i * recBytes);   // POINTER INTO the buffer
    return;
}
```

LLVM inlines the same in the `at`/indexer fast path — `at.dense` block (`LlvmGen.cpp:1563-1572`):
computes `recAddr = payAddr(recv, 16 + idx*recBytes)`, stores `{tag=5, payload=recAddr}`, then
`retain` (a borrowed-element +1 transfer — but retain no-ops on a tag-5 pointer into a dense
buffer because it isn't a counted header). The returned value **aliases the buffer**; it is not
a copy.

`CGen.cpp:173-177` (`arrGet`): materializes a **fresh** `mkobj(aelem)` and copies the `F` flat
slots into it — the C++ backend does gather-on-read (a fresh object per element access), a
subtly different choice from the ABI backend's pointer-aliasing. Both are observably correct
because value semantics mean the caller can't tell.

### 3.4 Field access — the fixed-offset fast path (the part columnar breaks)

Field reads/writes lower to `RawGet`/`RawSet` with a **compile-time packed slot index** when
the slot is provably stable across all subclasses. Since structs are final (no subclasses), a
struct field is always a stable slot.

- `fieldSlotOf(cls, key)` (`Lower.cpp:61-70`): walks `cls->shape.slots`, counts non-method
  non-`distinct` slots, returns the field's index (declaration order within the shape).
- `packedSlot(cls, key, requireNoAccessor)` (`Lower.cpp:78-90`): returns `slot+1` iff no
  subclass shifts the slot or intercepts it with an accessor; else `0` (fall back to dynamic
  name dispatch). For a struct, always `slot+1`.
- Lowered read (`Lower.cpp:1585-1590`): `RawGet` with `in.d = slot+1`.
- LLVM `RawGet` (`LlvmGen.cpp:2547-2555`):
  ```cpp
  if (in.d > 0) {  // compile-time packed slot
      llvm::Value* slotPtr = payAddr(recv, 16 + (in.d-1)*16);
      copyLV(dst, slotPtr);
  }
  ```

So `arr[i].x` today = `idxget` (record pointer `p = base + 16 + i*recBytes`) then `RawGet`
(`p + 16 + slot_x*16`). **One base pointer, two fixed offsets, both compile-time constant.**
This composes only because the record is contiguous. Columnar has no contiguous record, so the
`idxget → RawGet` composition cannot survive unchanged — this is the central codegen problem
(§6.1).

### 3.5 Mutation — `idxset` + COW-on-refcount

`lvrt_idxset` dense branch (`lv_runtime.c:1835-1852`):
```c
if (rawlen < 0) {   /* dense: value-struct records, no element refs */
    int64_t recBytes = lv_ld_i64(base->payload, 8);
    int64_t rc = HDR(base->payload)[0];
    if (rc == 1) {                                   // uniquely owned: mutate in place
        memcpy(P8(base->payload)+16 + i*recBytes, P8(val->payload), recBytes);
        *out = *base;  return;
    }
    /* shared: copy whole buffer, then write the record into the copy */
    int64_t payload = lv_halloc_prefixed(16 + cleanLen*recBytes);
    /* memcpy all records, then memcpy the one new record over index i */
}
```

This is info.md §11's COW-on-refcount: `arr[i] = v` mutates in place when the base's refcount
is exactly 1, copies the whole buffer when shared. Note the granularity: **the whole array
buffer** is the COW unit. LLVM inlines this at `ia.dense` (`LlvmGen.cpp:2809-2861`).

### 3.6 Free / ARC — dense records carry no per-element refs

`lv_recursive_free` LV_ARR branch (`lv_runtime.c:331-345`):
```c
} else if (tag == LV_ARR) {
    int64_t rawlen = lv_ld_i64(payload, 0);
    if (rawlen < 0) {                       // dense: value-struct records only, never refcounted
        lv_free_raw(P8(payload)-16, HDR(payload)[1]);   // just free the buffer
    } else {                                // boxed: release each element, then free
        for (i in 0..rawlen) { release(elem); vfree(elem); }
        lv_free_raw(...);
    }
}
```

The dense buffer is one allocation with one ARC header (`rc` at `payload-16`); freeing it is a
single `lv_free_raw`, no per-element walk, because value-struct records are copied bytes, never
separately-allocated heap pointers. `lv_recursive_free`'s `LV_OBJ`/`isvalueclass` gate
(`lv_runtime.c:214`) returns 0 for a value struct so a stray tag-5 record pointer never gets
walked as a heap object.

### 3.7 The X64Gen reference (frozen — for reading only)

`src/X64Gen.cpp` carries the original hand-emitted version of all of the above:
`genArrAppend` dense branch (`X64Gen.cpp:1038-1076`), dense `genIdxGet` (`:1659-1690`), dense
`genIdxSet` with COW (`:1743-1799`), dense `genRecursiveFree` (`:755-775`), `genMkArrFill`
dense (`:1187-1196`). The runtime's C code was extracted from these and is documented to match
them "wart for wart" (`lv_runtime.c:1645-1649`). The design should treat X64Gen as an
executable spec of the row-major semantics, **not** as something to modify.

---

## 4. The value-struct machinery (what dense storage sits on)

Columnar inherits all of this; the design must preserve every invariant.

- **`struct` parsing** (`Parser.cpp:1757`): `struct` → `parseClass(..., isValue=true)`; sets
  `ClassDecl::isValue` (`Ast.hpp:332`) → `Symbol::isValue` (`Symbols.hpp:67`).
- **Value types are final & flat** (`Resolver.cpp:5452-5459`): a struct may implement
  interfaces but not inherit implementation, and nothing inherits from a struct. This is why a
  dense/columnar element type has a *fixed, closed* field set — no subtype can widen a record.
  (info.md §9: "value types are final, so there is no slicing and no dense-array subtype
  ambiguity; mixed variants use a closed union instead.")
- **Field layout** = `cls->shape.slots` in declaration order, non-method non-`distinct`
  (`Lower.cpp:61-70`). Each `Slot` (`Symbols.hpp:35-52`) carries `name`, `canonical` (the
  field's **static type as text** — `"int"`, `"string"`, `"Point"`), `isConst`, `isReadonly`,
  `isWeak`. **The static field type is available at compile time** — this is what makes
  tag-free, per-type-sized columns *possible* (§6.5).
- **Copy semantics** (`RuntimeValue.hpp:114-119` oracle; `CGen.cpp:130-132`; runtime `Copyval`):
  binding/passing/returning/storing a struct deep-copies it (nested struct fields copy;
  reference-class fields are shared). `Op::CopyVal` (`Ir.hpp:31`) "deep-copy iff src is a value
  struct (else move)."
- **`Op::VFree`** (`Ir.hpp:79`): free a **dead standalone** value-struct copy. Value structs
  can't be refcounted (a tag-5 pointer may point inline into a dense buffer, `X64Gen.cpp:645`),
  so the escaping tier frees returned/copied-out structs by **uniqueness/liveness** instead:
  the lowerer tracks call-result registers holding a fresh standalone struct copy and emits
  `VFree` at the consuming site (`Lower.cpp:885-901` `noteFreshStructResult`, `:870-882`
  `emitArgCopies`). GC'd engines no-op `VFree`.
- **Ownership analysis** (`Ownership.cpp:92-105`, `:293-297`): a value-struct argument copied
  by value into a container (a dense array inlines the record's bytes) means the **object does
  not alias the result** — it's dead after the call, routed to the arena, not the heap. The
  analysis special-cases "every source is provably a value-struct alloc" to skip the
  args-to-result alias.

---

## 5. The known leak edge the design inherits (`dense_index_set` XFAIL)

The one declared-red target in the churn corpus. `tests/corpus/churn/dense_index_set.ext`
(first line `// XFAIL: …`):

```
dense IndexStore leaks its VALUE operand — the struct written into a dense array is
heap-allocated (it "escapes" per Ownership's stored-by-index rule), its record is memcpy'd
into the buffer, but the standalone copy is never freed (~64B/iteration). idxset cannot free
it (the source local may still be read); the fix needs Lower-side liveness (a VFree at the
consuming site, like returned value structs) or arena routing for operands that are provably
copied in. Pre-existing — present before the §11 COW work.
```

The program: build `Array<Pt>` of 4 elements, then in a loop `q.x = i; pts[j] = q;` — each `q`
is a fresh heap struct whose bytes get memcpy'd into the dense buffer, but the standalone `q`
copy is never reclaimed. Present on both ELF and LLVM (info.md §15/§17). **The columnar design
must decide whether it re-solves this or inherits it** — the request's Known Warning #2 flags
exactly this: "any new storage layout needs its own churn-corpus coverage before it's trusted."
A columnar `idxset` scatters the operand's fields into N columns; the same "who frees the
standalone operand copy" question recurs, possibly N times.

The green companion is `tests/corpus/churn/struct_array_field.ext` — the value-struct-exclusion
path that must stay flat in N: a `Bag` holding a dense `Array<Point>`, built and dropped per
iteration; reclaiming must free the buffer but must NOT recurse into inline struct bytes as if
heap pointers (that would be over-release/UAF). This is the exact test shape a columnar variant
needs a parallel of.

The churn harness (`fuzz/churn_leak.py`): each program has an `@N@` hole, compiled at several
magnitudes, run, and its stderr `[heap] escaping-tier peak=… live-at-exit=…` meter read;
`live-at-exit` must not grow with N, cross-checked against `--mem-verify`'s reachability oracle
(constant root set). XFAIL = expected-red; XPASS = "flip it green."

---

## 6. The hard problems columnar introduces

This is the core of what the design must resolve. Each is a real, code-level fork.

### 6.1 Field access when fields are not adjacent — THE central problem

Today `arr[i].x` = `base + 16 + i*recBytes` (record pointer) `+ 16 + slot_x*16` (field slot).
Both offsets compile-time-constant, composed via one intermediate tag-5 pointer.

Columnar has no record. Field `x` of element `i` lives at `columnBase(x) + i*stride(x)`, where
`columnBase(x)` and `columnBase(y)` are in different regions. So:

- `idxget` **cannot** return a single "record pointer," because there is no contiguous record
  to point at. The record-pointer trick (§3.3) — the thing that makes today's field access a
  fixed offset — is exactly what breaks.
- `RawGet`'s `recv + 16 + slot*16` assumption (`LlvmGen.cpp:2549`) is invalid for a columnar
  element reference.

Three candidate resolutions (the design must pick / combine):

1. **Materialize-on-read (gather).** `arr[i]` gathers all columns into a fresh boxed/standalone
   struct. Simple, preserves the tag-5 record-pointer downstream, requires **zero** field-access
   changes. But it **defeats the entire purpose**: every element read touches every column, so
   `sum(map(p => p.x))` is *worse* than row-major (a full gather + a boxed alloc per element).
   Only acceptable as a correctness fallback for escape/binding, never for projection.
2. **Row-view / cursor references.** `arr[i]` yields a *lightweight reference* carrying
   `(arrayHandle, rowIndex)` instead of a pointer into a record. Field access `.x` on such a
   reference computes `columnBase(x) + rowIndex*stride(x)` and touches **only** the x column —
   the actual win. Requires: a new value representation (a "columnar row ref" — possibly a new
   tag, or a reuse of tag-5 with a discriminated payload), and field-access codegen
   (`RawGet`/`RawSet`) that recognizes it and does the column computation. This is the
   representationally-honest approach and the one that delivers the locality claim, but it is
   the deepest change.
3. **Pattern fusion.** Recognize column-projection shapes (`map(p => p.field)`,
   `reduce`/`sum` over a projected field, `where(p => p.field …)`) at the IR level and lower
   them to direct column scans that never materialize a row at all. Highest performance ceiling,
   but the stdlib methods are written in the language over `at(i)` (§7), so fusion has to see
   through closures and the `for..in` protocol — a substantial optimizer, and it only helps the
   patterns it recognizes.

Realistic answer is probably **2 + 3**: row-view refs for correctness and general field access,
plus fusion for the hot aggregate patterns. The design must define the row-view value
representation precisely (it crosses the ABI boundary, so it is a `lv_abi.h` contract change)
and the escape rule below.

### 6.2 The escape / materialization boundary

Value semantics say `Point p = arr[i];` binds a **copy** — `p` is independent of the array.
With a row-view ref, the copy must **gather** the scattered columns into a standalone,
contiguous value struct at the moment the ref escapes (is bound to a variable, passed, returned,
stored, or captured), because:

- the backing array may be freed or COW-copied out from under it, and
- downstream code (including other backends and the oracle) expects a normal struct.

So the design needs a crisp rule: **immediate field projection stays column-local; any escape
gathers.** That rule maps onto machinery that already exists — `mayBeValueStruct` /
`definiteValueStruct` (`Ast.hpp:198-199`), `CopyVal`, `noteFreshStructResult`/`emitArgCopies`
(`Lower.cpp:870-901`). The gather is essentially "materialize row `i` into a fresh record, then
it's an ordinary standalone struct." Getting the boundary exactly right is the correctness crux
and the place the `dense_index_set`-style leak questions (§5) resurface.

### 6.3 Mutation & COW granularity

Row-major COW (§3.5) copies the **whole buffer** at rc>1. Columnar has a choice:

- **One buffer, column sections** — keep info.md's "one refcounted array = one allocation":
  a header + all columns in a single alloc. COW copies the whole thing (same granularity as
  today). Simplest ARC (one header, one free), cheapest to reason about, matches the existing
  `lv_recursive_free` single-`lv_free_raw` discipline. Downside: a write to one column still
  copies every column when shared.
- **N independent column arrays behind a small header** — each column its own allocation +
  refcount. Enables *column-selective COW* (writing `x` when only `x` is aliased copies just the
  x column) and is closer to the "hand-rolled struct-of-arrays" the request names as today's
  manual fallback. Downside: multiplies the ARC/free surface by field count, complicates the
  "one array = one counted object" model, and makes `idxset` of a full struct touch N headers.

`idxset` of a full struct (`arr[i] = someStruct`) scatters the operand across N columns — the
"who frees the standalone operand copy" leak (§5) can recur per column. `arr[i].x = v` (single
field) is a new, cheaper path columnar enables that row-major doesn't special-case today (it
rewrites the whole record).

### 6.4 Default-vs-opt-in and the type surface

The request calls this out as a required decision (Acceptance Criterion #2). Options:

- **Columnar becomes the default `Array<struct>`.** A behavior change for all existing dense-array
  code; every value-struct array silently relayouts. Maximizes the win's reach; maximizes blast
  radius on the ARC/churn discipline and on any code that (even implicitly, via `at(i)` aliasing)
  depends on row-major record contiguity. Requires re-greening the entire churn corpus.
- **Opt-in via a distinct type** (e.g. `Columnar<Point>` / `Table<Point>`) — coexists with
  row-major dense `Array<Point>`. Localizes risk and codegen; needs a new type with its own
  method surface (or an interface both share). Cleaner story, less automatic reach.
- **Opt-in via annotation** on the array or the struct (`@columnar struct Point` /
  `Array<Point> @columnar`). Keeps one `Array` type; needs an annotation channel that survives
  to codegen and a rule for how it interacts with generics/inference.

This decision cascades into nearly every other one: whether interpreters can stay entirely
untouched (a distinct type is easier to keep semantically boxed), how much of the stdlib surface
must be duplicated or generalized, and how big the churn re-verification is.

### 6.5 Column typing, sizing, and the tag-free win

Each field's static type is known (`Slot.canonical`, §4). Today a dense record spends a full
16-byte `{tag,payload}` slot per field even for an `int` (8 wasted tag bytes) and repeats
`classId`+`dyn` (16 bytes) per row. A columnar layout can:

- **Drop the per-row `classId`/`dyn`** (invariant across the array — store once in the header).
- **Drop per-element tags** — a whole `int` column is statically `int`, so store 8-byte payloads
  and *synthesize* the tag from the field type when materializing a row-view read. For a 2×`int`
  `Point` this is 32 bytes/row → 16 bytes/row (2×); for a wide scalar struct the ratio approaches
  the tag overhead (up to ~2×) plus the 16-byte classId/dyn savings.

This is a real, separate memory win the request doesn't mention but the layout exposes. It adds
codegen work: `RawGet`/`RawSet` on a column must reconstruct/deconstruct the tag per the field's
static type, and mixed-type structs need per-column strides and alignment. **Alignment**: a
column of `int`/`float`/`char` wants natural alignment; the header + column offsets must be laid
out so each column is aligned (padding between columns, not within). This is new; row-major
records are uniformly 16-byte-slotted and never faced per-field alignment.

### 6.6 Mixed and non-trivial field types

Row-major records handle these by storing a normal 16-byte `LvValue` slot. Columnar must decide
per column:

- **`string` fields** — a string is a handle (pointer+len, 16-byte value). A string column is a
  column of handles; **the handles are refcounted heap pointers**, so a string column is NOT
  "value-struct records, no per-element refs" — freeing it must release each handle. This breaks
  the row-major invariant that dense buffers carry no per-element refs (`lv_abi.h:121-123`,
  `lv_runtime.c:334`). Any struct with a string (or array/map/closure/block/reference-class)
  field has **refcounted columns**, and `recursiveFree` must walk those columns. This is a
  significant departure and must be designed explicitly.
- **Reference-class fields** — shared, refcounted (copy shares the wrapper). Same as strings:
  refcounted column, per-element release on free.
- **Nested `struct` fields** — deep-copied. A nested struct column is itself a mini-columnar or
  inline-record question (columnarize recursively, or store the nested struct's bytes inline in
  the outer column?).
- **`char`/`bool`/`float`/`int`** — pure immediates, tag-free packable (§6.5).
- **`weak T?` fields** (F5, `lv_abi.h:50-52`, tag 12) — a weak slot owns an `LV_WEAK_PROXY`
  cell. A weak column of proxies has its own liveness/rebuild semantics; thread-copy boundaries
  rebuild weak fields as `None` (info.md). Almost certainly out of scope for v1 — flag it.
- **union / `T?` fields** — value size is the largest member (info.md §9); a column of unions is
  a column of that fixed size with a tag. Likely v1-deferrable.

A clean v1 scoping is probably: **columnarize structs whose fields are all pure scalars**
(int/float/bool/char), and fall back to row-major (or gather) for structs with string/reference/
nested/weak/union fields — with the design stating the boundary and the roadmap for widening it.

### 6.7 Static vs dynamic denseness

Today denseness is discovered at runtime (first value-struct append flips the bit, §3.2). An
empty `Array<Point>` starts boxed and becomes dense on first append. Columnar likely wants
**static** knowledge at the allocation site (to lay out columns, size the header, choose the
representation up front) — the checker knows `Array<Point>`'s element type is a value struct at
every allocation. The design must decide whether columnar arrays are statically shaped from
construction (cleaner, enables tag-free columns, but needs the element type at every `NewArray`)
or retain the dynamic-flip model (harder to columnarize, since an empty array has no columns
yet). The whole-program, element-type-known-at-checker-time property (info.md §7, §12) favors
static.

### 6.8 The stdlib method surface and fusion (why storage alone isn't the win)

Critical and easy to miss: **changing storage does not by itself yield the columnar win**,
because every bulk method is written in the language over `at(i)`:

`Array<T>.map` (`Resolver.cpp:394-398`):
```
Array<U> map<U>((T) => U fn) { Array<U> r = []; for (T x in this) r = r.add(fn(x)); return r; }
```
`where`/`reduce`/`any`/`all`/`count`/`forEach`/`sort`/… all follow the same `for (T x in this)`
shape (`Resolver.cpp:388-620`), and `for..in` over an array uses the `IterLen`/`IterAt` native
fast path = `at(i)`.

So `pts.map(p => p.x)`: `for` pulls `at(i)` (a row), the closure reads `.x`. If `at(i)` on a
columnar array *materializes a row* (gather, §6.1 option 1), the map touches every column and the
columnar layout buys nothing (worse — it adds a gather). The win requires either row-view refs
(so `.x` after `at(i)` stays column-local, §6.1 option 2) **or** fusion that rewrites the whole
`map(p => p.x)` into a direct x-column scan (§6.1 option 3). The design cannot treat "storage"
and "the aggregate methods" as separable — the method-flow is where the locality is won or lost.
This also interacts with `Seq<T>` (the lazy pipeline, `reference.md` §6.4.9): a lazy
column-projection could be the natural fusion surface.

---

## 7. Constraints, interactions, and non-negotiables

- **X64Gen frozen** (`[[feedback_x64gen-frozen]]`, info.md §0): no ELF columnar lane, no new
  ELF corpus, never gate completion on an ELF finding. Row-major ELF code stays untouched.
- **ABI = STOP-gated contract** (`techdesign-portable-backend-2.md` §0.2): any `lv_abi.h`
  change (a new tag, a row-view representation, a columnar buffer layout) is a contract change —
  a Fable-authored design decision, never a Sonnet improvisation. The design must specify the
  contract delta in full so Track A (LlvmGen) and Track B (runtime) implement against a frozen
  spec.
- **Interpreter parity** (`reference.md` §7.1): the entire differential corpus must produce
  identical output on all active engines. The oracle/IR stay boxed; columnar is validated
  against them. Any observable difference is a bug by definition.
- **ARC/churn discipline** (info.md §15, request Known Warning #2): new storage needs its own
  churn-corpus coverage (`tests/corpus/churn/`, `fuzz/churn_leak.py`) — `live-at-exit` flat in
  N, `--mem-verify` root-set cross-check — before it's trusted. Expect a
  `columnar_index_set`/`columnar_field_set`/`columnar_string_col` family of churn programs.
  Refcounted columns (§6.6) are the highest-risk case.
- **Thread copy-boundary** (info.md §14): every value crossing a thread boundary deep-copies
  (`CGen.cpp:130-160` `threadtransfer`, runtime flatten/rebuild). A columnar array crossing a
  `Channel` must flatten/rebuild correctly — another representation the copy engine must learn.
- **`[[feedback_techdesign-conventions]]`**: default to a SINGLE track with ordered milestones
  and STOP-and-escalate checkpoints unless there's genuine disjoint-file parallelism. This work
  is tightly coupled (one contract, LlvmGen + runtime move together, then CGen) — lean single
  track. Explicit gates with done-criteria and dates.
- **`.lev` not `.ext`** (`[[feedback_ext-vs-lev]]`): any new corpus/churn programs are `.lev`,
  even though existing dense churn files are `.ext`.

### Adjacent requests that interact

- **`request-narrow-integer-types.md`** (`int8/int16/int32/uint`, `byte`): columnar packing is
  explicitly cited there as a motivation — fixed-width typed columns are far more compelling with
  real narrow types (an `int8` column is 1 byte/elem, not 8). Not a blocker, but the column-sizing
  design (§6.5) should be forward-compatible with narrow types landing later.
- **`Block` / `mmap`** (info.md §13/§17): the "maps directly onto `mmap` and columnar forms"
  story ties columnar to `Block`-backed memory. A columnar column is morally a typed `Block`;
  whether columns can be `mmap`-backed (zero-copy load of a columnar file) is the long-game the
  request's §13/§17 framing points at. Likely out of v1 scope but the layout should not preclude it.

---

## 8. Acceptance criteria (from the request) + how to satisfy them

From `request-columnar-dense-array-struct.md` §Acceptance Criteria:

1. **A tech design** proposing ABI representation, field-access codegen, and COW/mutation story,
   reviewed before implementation. → §3 (what exists), §6.1/§6.3/§6.5 (the forks).
2. **A default-vs-opt-in decision.** → §6.4.
3. **If implemented:** a benchmark showing the cache-locality win on a column-selective aggregate
   (e.g. `sum` over one field of a large struct array) vs today's row-major dense array, **plus
   full churn-corpus coverage on every active engine.**

**Benchmark methodology** (to make Criterion #3 concrete):
- Corpus program: a large `Array<Wide>` where `Wide` has one hot field + several cold fields;
  `std::sum(arr.map(w => w.hot))` (or a `reduce`). Compare wall-clock row-major dense vs columnar
  on LLVM (the primary backend), at `-O2`. The win should scale with struct width (more cold
  fields = more row-major cache waste columnar avoids). Reference the existing perf-measurement
  style in `reference.md` §7.3 (indicative wall-clock table, controlled A/B rebuilds).
- Locality proof beyond wall-clock: the design may want a bytes-touched or cache-miss argument,
  since wall-clock on a small machine can be noisy. State the struct width at which columnar
  wins (narrow structs may not).
- **Churn**: mirror `struct_array_field.ext` (green, flat-in-N) and `dense_index_set.ext` (the
  leak edge) for the columnar representation, on oracle + IR + emit-C++ + LLVM (no ELF).
  Refcounted-column programs (string field) need their own churn target.

---

## 9. File & line index (where everything lives)

| concern | location |
|---|---|
| ABI contract (value cell, tags, array layouts, object record) | `runtime/lv_abi.h:34-180` |
| dense go-dense / append (runtime) | `runtime/lv_runtime.c:1633-1709` |
| dense idxget (record-pointer) | `runtime/lv_runtime.c:1756-1782` |
| dense idxset + COW | `runtime/lv_runtime.c:1832-1852` |
| dense free / ARC gate | `runtime/lv_runtime.c:214, 331-345, 736-806` (flatten) |
| key equality (struct field-wise) | `runtime/lv_runtime.c:1738-1754` |
| LLVM dense `at`/idxget | `src/LlvmGen.cpp:1544-1585` |
| LLVM dense idxset/COW | `src/LlvmGen.cpp:2795-2861` |
| LLVM `NewArraySized` (dense fill) | `src/LlvmGen.cpp:2616-2628` |
| LLVM field access (RawGet/RawSet packed slot) | `src/LlvmGen.cpp:2547-2563` |
| emit-C++ dense (aelem, arrGet/Push/Set/Cow) | `src/CGen.cpp:37-38, 164-194, 238` |
| emit-C++ fieldCount/isValueClass emit | `src/CGen.cpp:1313-1345` |
| X64Gen dense (FROZEN reference) | `src/X64Gen.cpp:755-775, 1038-1076, 1187-1196, 1659-1799` |
| field slot index | `src/Lower.cpp:61-90` |
| value-struct copy/vfree lowering | `src/Lower.cpp:870-901` |
| ownership value-struct rule | `src/Ownership.cpp:92-105, 293-297` |
| Slot struct (field type text) | `src/Symbols.hpp:35-58` |
| shape building (field layout, struct finality) | `src/Resolver.cpp:5437-5524` |
| oracle value-struct array (boxed) | `src/RuntimeValue.hpp:40-119` |
| IR interp value-struct array (boxed) | `src/IrInterp.cpp:644-674, 723-735` |
| stdlib Array methods (map/where/reduce…) | `src/Resolver.cpp:358-620` |
| CopyVal / VFree IR ops | `src/Ir.hpp:31, 79` |
| churn corpus (leak net) | `tests/corpus/churn/{struct_array_field,dense_index_set}.ext`, `fuzz/churn_leak.py` |
| STOP / contract-change protocol | `designs/complete/techdesign-portable-backend-2.md:28-58` |
| the request ticket | `designs/requests/request-columnar-dense-array-struct.md` |
| the "why" (data thrust, DB, differentiator) | `info.md` §9, §13, §17; `designs/proposal-web-framework.md:270-275, 645-655` |

---

## 10. Open design questions to resolve (the decision list)

1. **Representation** (§6.1, §6.3): one-buffer-with-column-sections vs N-independent-column-arrays.
   Drives COW granularity, ARC surface, and the "one array = one counted object" model.
2. **Element-read representation** (§6.1, §6.2): row-view/cursor ref vs gather-on-read. If
   row-view: its exact `LvValue` encoding (new tag vs discriminated tag-5) — an ABI contract delta.
3. **The escape/materialization boundary** (§6.2): the precise rule for when a row-view gathers
   into a standalone struct, and where the gather's freeing obligation lives (the
   `dense_index_set` leak, generalized).
4. **Field-access codegen** (§6.1): how `RawGet`/`RawSet` recognize a columnar element and
   compute `columnBase(f) + i*stride(f)`; how `.x = v` single-field writes lower.
5. **Column typing / tag-free packing / alignment** (§6.5): tagged 16-byte columns (simple) vs
   typed tag-free columns (2× memory win, tag synth on read, per-column alignment).
6. **Field-type scope for v1** (§6.6): scalars-only (fall back to row-major/gather for
   string/reference/nested/weak/union fields) vs full refcounted-column support. Recommend
   scalars-first with an explicit widening roadmap.
7. **Default vs opt-in** (§6.4): default `Array<struct>` relayout vs a distinct `Columnar<T>`
   type vs an annotation. Recommend a distinct type or annotation to bound blast radius and keep
   interpreters trivially boxed — but this is the owner's call.
8. **Static vs dynamic denseness** (§6.7): lay out columns at the checker-known allocation site
   vs the current runtime-flip model.
9. **The method/fusion story** (§6.8): row-view refs (general, moderate win) vs IR-level fusion
   of projection/aggregate patterns (hot-path, maximal win) vs both. How `Seq<T>` lazy pipelines
   fit.
10. **Track shape & milestones** (§7): single track (recommended — tightly coupled contract +
    LlvmGen + runtime, then CGen) with ordered gates; STOP-and-escalate checkpoints; explicit
    dates.

---

## 11. Further research to do before/while designing

Things not yet nailed down that the design author should resolve, ideally before committing the
contract:

1. **Prototype the row-view field-access path on LLVM in isolation** — confirm that
   `columnBase(f) + i*stride(f)` field access actually beats row-major on a wide struct (the
   whole premise). Cheapest way to de-risk §6.1 before designing the full surface.
2. **Measure the actual win threshold** — at what struct width / access selectivity does columnar
   beat row-major? A 2-field `Point` may never win (both fields share a cache line row-major).
   The design should state the regime where it's recommended, not oversell it.
3. **Decide the row-view escape gather cost** — benchmark gather-on-escape; if binding a struct
   out of a columnar array is common, the gather may dominate and argue for a different default.
4. **Refcounted-column ARC design** (§6.6) — the string/reference-field case is where the "dense
   buffers have no per-element refs" invariant dies. Prototype `recursiveFree` over a mixed
   columnar array + a churn program before trusting it. This is the highest-risk correctness area.
5. **Interpreter strategy confirmation** — verify the oracle/IR can stay 100% boxed (no columnar
   representation at all) and still pass a columnar corpus differentially. Near-certain given the
   row-major precedent, but confirm there's no observable (`toString`, identity, iteration-order)
   leak of the layout. If a distinct `Columnar<T>` type is chosen, confirm the interpreters can
   back it with the same boxed vector they use for `Array<struct>`.
6. **Generics & inference interaction** — how `Columnar<T>` (or an annotation) flows through
   inference, `map<U>`'s result type, HKT, and the raw-form compatibility rule (info.md §9). A
   distinct type needs a method surface story (share via an interface? duplicate?).
7. **Fusion feasibility scan** (§6.8) — how hard is it to fuse `map(p => p.f)` /
   `reduce`/`std::sum` over a projection through the in-language method bodies and closures? If
   fusion is the only path to the win (i.e. if row-view refs alone don't beat row-major), that
   raises the design's cost and risk materially. Determine this early.
8. **`Block`/`mmap` forward-compat** (§7) — confirm the chosen column layout doesn't preclude a
   future `mmap`-backed / `Block`-viewed column (the long-game). Don't design it in v1, but don't
   design it out.
9. **Narrow-int forward-compat** (§7) — ensure the column-sizing scheme (§6.5) can later host
   1/2/4-byte columns without a re-layout, so `request-narrow-integer-types` composes cleanly if
   it lands.
10. **Churn-target enumeration** — list the exact churn programs needed (columnar field-set,
    full-struct-set/leak edge, refcounted-column, thread-transfer of a columnar array) so
    Acceptance Criterion #3 is a checklist, not a judgment call.
