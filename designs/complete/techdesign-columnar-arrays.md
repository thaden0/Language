# Tech Design — Columnar `Array<struct>` (struct-of-arrays dense storage)

**Status:** LANDED 2026-07-12 — columnar is the default layout for eligible scalar structs
(`--no-columnar` escape hatch); full suite green under the flip. See the implementation log
in the companion doc (`techdesign-columnar-arrays-2.md` §6).
**Authored by:** Fable-class model, 2026-07-12. **Implemented by:** Opus-class agent.
**Request:** `designs/requests/accepted/request-columnar-dense-array-struct.md`
**Research substrate:** `docs/dense-array-research.md` — read it first; every `file:line`
fact this design relies on is anchored there and is not re-derived here.
**Companion:** `designs/techdesign-columnar-arrays-2.md` (implementation plan: milestones,
file ownership, STOP checkpoints, test/bench matrix, timeline).

---

## 0. Decision summary (the answers the request demands)

The request names four open questions. This design's answers, up front:

| question | decision |
|---|---|
| **ABI representation** | A **third array representation** behind the existing `LV_ARR` tag: one refcounted allocation, header + per-field column sections, discriminated by **bit 62** of the first body word (bit 63 = dense stays; bit 63+62 = columnar). Columns are **tag-free 8-byte payload words**; tags are synthesized from a codegen-emitted per-class **column descriptor table**. No new `LvValue` tag, no new value representation crosses the ABI. |
| **Field-access codegen** | **Lower-time fusion**: a new IR op pair `ColGet`/`ColSet` fuses `IdxGet → RawGet` when the array's element type is statically columnar-eligible, computing `columnBase(f) + i*8` directly — this is where the locality win is delivered. Any *unfused* element read (`arr[i]` escaping to a bind, argument, return, capture) **gathers** into a fresh standalone value struct. There is **no row-view/cursor value at runtime** — the "row reference" exists only transiently inside the lowerer. |
| **COW / mutation** | **Whole-buffer COW on the refcount**, same granularity and same discipline as today's dense-row `idxset` (in place at rc==1, full-buffer copy when shared). Column-selective COW is explicitly deferred (§10). `arr[i] = v` scatters the operand's fields across the columns. |
| **Default vs opt-in** | **Columnar becomes the default representation for eligible structs — no new user-facing type, no annotation** — but **staged behind a compile-time flag** (`--columnar`, default off) until the corpus/churn gates are green, then flipped to default-on with `--no-columnar` retained one cycle as the escape hatch. This is the LA-30 flip pattern applied to a layout. Ineligible structs (any non-scalar field) keep today's row-major dense layout permanently. |

Everything below justifies and specifies these.

---

## 1. Goals and non-goals

### Goals (v1)

1. Column-selective locality: a scan of one field of a wide `Array<struct>` touches only
   that field's memory. Concretely: `for (int i in 0..n-1) s = s + arr[i].hot;` on a
   struct with 8 int fields reads ~n*8 bytes, not n*(16 + 8*16) bytes.
2. The tag-free memory win that falls out of static field types: 8 bytes/field/row
   instead of 16, and no per-row `classId`/`dyn` — a 2×+ footprint reduction for scalar
   structs (research §6.5).
3. Zero observable behavior change. Value semantics make layout unobservable; the
   differential corpus proves it (oracle/IR stay boxed, identical output required).
4. Full churn-corpus coverage of the new layout on every active engine before the flip
   (request Acceptance Criterion #3).
5. A benchmark demonstrating the locality win vs row-major on LLVM (Criterion #3), with
   the honest statement of the regime where columnar wins (§9).

### Non-goals (v1) — each with its forward hook

- **Stdlib pipeline fusion** (`pts.map(p => p.x)` as a direct column scan). The closure
  call boundary copies the element (research §6.8), so map-shaped pipelines gather per
  element in v1 and do **not** get the locality win; direct-index loops do. Fusion (or a
  `Seq`-based column projection) is the designated follow-up (§10.1). The v1 benchmark
  uses the index-loop form.
- **Column-selective COW** and geometric growth/capacity slack. v1 append replicates the
  dense-row copy-and-grow behavior for parity (§6).
- **Refcounted columns** — structs containing `string`/reference-class/array/map/
  closure/`Block`/`weak`/union/nested-struct fields are **ineligible** and stay
  row-major. The eligibility line is static and crisp (§3). Widening it is §10.2.
- **Narrow column strides** (`int8` → 1-byte column). The header carries per-column
  stride so narrow-int support (`request-narrow-integer-types.md`) can land later
  without a re-layout; v1 always writes stride 8 (§4.3).
- **`mmap`/`Block`-backed columns.** The layout deliberately keeps each column a plain
  contiguous typed run so a future zero-copy `Block` view is possible (§10.3); nothing
  in v1 implements it.
- **ELF / X64Gen** — frozen, not a target, no columnar lane, no new ELF corpus, and no
  gate of this work on any ELF finding (`[[feedback_x64gen-frozen]]`). The ELF backend
  emits its own inline codegen and does not link `lv_runtime.c`, so it is structurally
  unaffected.

---

## 2. Where the work lands (engine map)

Same shape as the row-major dense work (research §2): the interpreters stay boxed and act
as the semantic oracle; the native backends and the C runtime carry the layout.

| engine | change |
|---|---|
| tree-walk oracle (`Eval.cpp`) | **zero** — pre-IR, boxed, the semantic reference |
| IR interpreter (`IrInterp.cpp`) | **minimal** — implement the two new IR ops (`ColGet`/`ColSet`) over its existing boxed vectors (element read + field read; no layout) |
| emit-C++ (`CGen.cpp`) | columnar storage order in its self-contained flat-vector representation + the two ops. Note CGen already gathers on read (fresh object per `arrGet`), so its semantic delta is small |
| LLVM (`LlvmGen.cpp` + `runtime/lv_runtime.c` + `runtime/lv_abi.h`) | the real work: contract delta, runtime columnar core, fused-op codegen |
| X64Gen / ELF | **frozen — untouched** |

The `lv_abi.h` delta (§4) is a **contract change and therefore a STOP-gated,
Fable-authored decision** (`techdesign-portable-backend-2.md` §0.2). It is specified in
full here so implementation never has to improvise it.

---

## 3. Eligibility — which structs go columnar

A class `C` is **columnar-eligible** iff all of:

1. `C` is a value struct (`Symbol::isValue`), and
2. every field slot of `C` (non-method, non-`distinct`, per `fieldSlotOf` /
   `Lower.cpp:61-70` order) has a static type in the scalar set
   **{`int`, `float`, `bool`, `char`}**, and
3. `C` has at least one field (a zero-field struct stays on the existing path — nothing
   to columnarize), and
4. no field is `weak`, optional (`T?`), a union, or a nested struct.

Eligibility is decided **statically** from `Slot.canonical` (`Symbols.hpp:35-58`) — the
checker and lowerer both know it, and codegen emits it into the per-class descriptor
table (§4.2) so the runtime agrees. Ineligible structs keep the row-major dense layout
**unchanged, forever** (row-major is not deprecated; it is the general case, columnar is
the scalar-struct fast case).

The dynamic go-dense model is retained (§5.1): an empty array is boxed and flips on the
first value-struct append, exactly as today — the flip just picks columnar instead of
row-major when the class is eligible and the columnar mode is on. This avoids any new
checker→allocation-site plumbing (research §6.7's "static" option is not needed because
the descriptor table gives the runtime the same information at flip time).

**Determinism invariant:** within one compiled program, an `Array<C>` for eligible `C`
is only ever boxed-empty or columnar — never row-major. This is what lets the lowerer
statically apply the gather/ownership discipline (§5.3) without a runtime mode check.

---

## 4. The representation (the `lv_abi.h` contract delta — normative)

### 4.1 Discriminator

Today (`lv_abi.h:109-129`): first body word ≥ 0 → BOXED; bit 63 set → DENSE (row-major).
New: bit 62 **in addition to** bit 63 → COLUMNAR.

```
BOXED:    word0 = len                      (>= 0)
DENSE:    word0 = cleanLen | (1<<63)                      — unchanged
COLUMNAR: word0 = cleanLen | (1<<63) | (1<<62)            — new
          cleanLen = word0 & ~(3<<62)
```

Every existing `rawlen < 0` check still correctly routes "not boxed"; dense-path code
gains one sub-branch on bit 62. Existing dense-row layouts are bit-for-bit unchanged.

### 4.2 Columnar buffer layout

One allocation, one ARC header (rc at `payload-16`, same as every heap value):

```
payload -> { int64_t lenWithBits;        // word 0: cleanLen | (1<<63) | (1<<62)
             int64_t classId;            // word 1 (dense-row stores recBytes here;
                                         //         columnar stores classId — bit 62 disambiguates)
             uint8_t columns[fieldCount * 8 * cleanLen]; }

columnBase(k) = payload + 16 + k * 8 * cleanLen      // column k, 8-byte stride, v1
field k of element i lives at columnBase(k) + 8*i    // payload word only, no tag
```

- **No per-row `classId`/`dyn`, no per-slot tags** — that is the 2×+ memory win
  (research §6.5). For v1 all columns have stride 8 and are naturally 8-aligned because
  the header is 16 bytes and every section length is a multiple of 8.
- Like dense-row, the buffer records **no capacity slack**; length == capacity, append
  reallocates (§6.3).

### 4.3 The column descriptor table (new emitted contract item)

Codegen already emits per-class tables the runtime reads (`fieldcount`,
`isvalueclass`). Two additions:

```c
/* per class: 1 iff columnar-eligible per §3 (and compiled in columnar mode) */
int32_t lv_col_eligible(int64_t classId);
/* per class, per field slot k: the field's scalar typecode */
int32_t lv_col_typecode(int64_t classId, int64_t k);   /* 1=INT 2=FLOAT 3=BOOL 10=CHAR
                                                          (values == the LvValue tags) */
```

Typecodes deliberately equal the `LvValue` tags so tag synthesis on gather/read is a
copy, not a mapping. The table is how the runtime lays out columns at flip time and how
`recursiveFree`/thread-transfer know a columnar buffer carries **no heap references**
(eligibility guarantees it). A future stride table (`lv_col_stride`) is reserved for the
narrow-int follow-up; v1 hardcodes 8 and MUST compute offsets via a helper so that
constant lives in exactly one place per artifact (one in `lv_runtime.c`, one in
`LlvmGen`'s address computation — noted as a deliberate, documented duplication of the
kind the contract file itself carries).

### 4.4 Mode agreement

Columnar is a **compile-time** mode (`--columnar` / after the flip `--no-columnar`).
Generated code and the shared runtime archive must agree, so codegen emits one config
symbol the runtime reads at flip time:

```c
extern const int32_t lv_cfg_columnar;   /* 1 iff this program was compiled columnar */
```

`lv_arr_go_dense` consults `lv_cfg_columnar && lv_col_eligible(classId)` to choose the
layout. A **runtime** env-var gate is explicitly rejected: the lowerer's ownership
discipline for element reads differs between modes (§5.3), so mode must be baked at
compile time or codegen and runtime could disagree (alias vs fresh-copy), which is a
memory-safety bug, not a perf choice.

### 4.5 Tag-5 invariant, restated for columnar

The existing tag-5 aliasing caveat (`lv_abi.h:61-63` — a tag-5 payload may point inline
into a dense buffer) is **not extended**: a columnar array **never** hands out a pointer
into its buffer. Every element value that leaves a columnar array is a **fresh standalone
record** (gather, §5.2) with the ordinary alloc-header shape. This *shrinks* the aliasing
surface relative to row-major and is the reason no new tag is needed.

---

## 5. Element access semantics

### 5.1 Construction and append

- `lvrt_arr_append` empty→first-struct branch (`lv_runtime.c:1701`): flip picks columnar
  per §4.4, else the existing `lv_arr_go_dense` row path. `lv_arr_go_columnar` allocates
  `16 + fieldCount*8`, sets bits, stores classId, scatters the first element's slot
  payloads into the (length-1) columns.
- Columnar append: allocate `16 + fieldCount*8*(n+1)`, copy each column region (they
  move — column bases depend on length), scatter the new element. Copy-and-grow with no
  capacity slack, matching dense-row's deliberate wart (`lv_runtime.c:1650`); the old
  buffer is released per the same discipline dense-row uses today. O(n²) build-by-append
  is a known, shared property of both dense forms; sized construction is the fast path.
- `NewArraySized` (`Array(n, fill)`, `LlvmGen.cpp:2616-2628`): new columnar fast path —
  allocate once at final size, then per column `k` fill `n` copies of the fill value's
  slot-`k` payload (a tight per-column loop; for a zeroed fill this is a memset-shaped
  loop). This is the constructor the analytic workload uses.

### 5.2 Element read — the gather rule

`arr[i]` on a columnar array (runtime `lvrt_idxget` bit-62 branch, and the LLVM inline
`at` fast path) **materializes a fresh standalone struct**: allocate one record
(`16 + 16*fieldCount`, ordinary rc=1 header), stamp `classId`, `dyn=0`, and for each
field `k` write `{tag = lv_col_typecode(classId,k), payload = load(columnBase(k)+8*i)}`.

This is exactly the semantics value binding required anyway (`Point p = arr[i]` must be
a copy); columnar just performs the copy at read time instead of handing out an alias
and copying later. Precedent: emit-C++ has **always** gathered on read
(`CGen.cpp:173-177`) and is differentially indistinguishable.

### 5.3 Ownership of the gathered value

The gathered record is a **fresh standalone value-struct copy** — precisely the thing
the existing `VFree`-by-liveness discipline manages (`Ir.hpp:79`,
`Lower.cpp:870-901` `noteFreshStructResult`/`emitArgCopies`). The lowerer, in columnar
mode, classifies `IdxGet` results whose static element type is columnar-eligible as
fresh struct results (like a call returning a struct), so consuming sites free them.
GC'd engines no-op `VFree` as today.

This is the design's answer to the escape/materialization boundary (research §6.2): there
is no boundary to police at runtime because **nothing un-materialized ever escapes**. The
only aliasing that exists is inside a single fused op (§5.4), whose array operand is a
live register for the op's duration.

### 5.4 Fused field access — `ColGet` (the win)

New IR op (in `Ir.hpp`, implemented by IrInterp/CGen/LlvmGen; the oracle is pre-IR):

```
ColGet dst, arr, idx, slotK     // dst = element idx's field k of arr
```

**Lowering rule:** when lowering a field read whose receiver is *syntactically* an index
read of an array whose static element type is columnar-eligible — `arr[i].x`, including
via the `([])` indexer and the `at(i)` prelude fast path feeding an immediate `RawGet` —
emit one `ColGet` instead of `IdxGet`+`RawGet`. No materialization, no copy, no `VFree`.

**LLVM codegen:** `payload = load(arrPay + 16 + (k*8*cleanLen) + 8*idx)`; store
`{tag = typecode_k, payload}` to `dst`. `cleanLen` is one load+mask from word 0; the tag
is a compile-time constant (static field type). Bounds-check against cleanLen exactly as
the `at.dense` path does today. The op must also carry the **boxed fallback** branch
(array still empty/boxed → ordinary boxed element field read) because denseness is
dynamic; the fallback is statically cold.

**`ColSet`** (`arr[i].x = v` single-field write) is specified as the symmetric fused
store **with the COW check first** (§6) — but its lowering is gated on C-M0 verifying
what `arr[i].x = v` currently lowers to (the pure-array rebind sugar may lower it as a
read-modify-write of the whole element; if the source shape doesn't exist today, `ColSet`
ships as codegen for that pattern only if the pattern exists — STOP item, see plan doc
§2.1). `ColGet` alone carries the v1 win; `ColSet` is an optimization.

**What does NOT fuse (v1):** an element flowing into a closure, a method call, a bind, a
return, a container, a thread channel — all gather per §5.2. In particular the stdlib
`map`/`where`/`reduce` bodies (`for (T x in this)` + closure call, research §6.8) gather
per element; pipelines are correct but win nothing in v1 (§1 non-goal, §10.1 follow-up).

### 5.5 Iteration

`for (T x in arr)` uses the `IterLen`/`IterAt` fast path = `at(i)` = gather per element
(correct; each `x` is an owned fresh struct, freed by the existing loop-variable
discipline). A direct-index loop over a projected field is the idiomatic fast form and
what the benchmark measures. `length()` reads cleanLen from word 0 — unchanged.

---

## 6. Mutation, COW, ARC

### 6.1 `idxset` (`arr[i] = v`) — whole-buffer COW

Bit-62 branch in `lvrt_idxset` (and the LLVM `ia.dense` inline), mirroring
`lv_runtime.c:1835-1852`:

- rc == 1: **scatter in place** — for each field `k`, store `v`'s slot-`k` payload to
  `columnBase(k) + 8*i`. `*out = *base`.
- rc > 1: allocate a same-size columnar buffer, memcpy the whole column region once,
  scatter `v` into the copy, release the original per the existing shared-path
  discipline. The COW unit is the whole buffer — identical granularity to dense-row.

The **standalone-operand leak** (`dense_index_set` XFAIL, research §5) recurs here
unchanged in kind: `idxset` cannot free `v`'s standalone copy (the source local may
still be read). v1 **inherits** the XFAIL as a declared edge with a columnar mirror
churn program (`columnar_index_set.lev`, XFAIL, same wording); fixing it via Lower-side
liveness for **both** layouts is a tracked stretch goal, not a gate (plan doc §5).

### 6.2 Free / recursiveFree

Eligibility guarantees a columnar buffer holds **only immediate scalars — no heap
references of any kind**. `lv_recursive_free`'s `LV_ARR` dense branch
(`lv_runtime.c:331-345`) is already "just free the buffer"; the columnar sub-case is the
same single `lv_free_raw`. No per-element walk, no new ARC surface. (This is the payoff
of drawing the eligibility line at scalars: research §6.6's refcounted-column problem is
excluded from v1 by construction, not solved incorrectly.)

### 6.3 Append/self-append

`a = a.add(x)`: the `MoveClear` self-append discipline applies as today; the columnar
append path (§5.1) is copy-and-grow. No capacity-slack optimization in v1 (parity with
dense-row; a shared geometric-growth follow-up would benefit both layouts and is out of
scope).

### 6.4 Thread transfer

The flatten/rebuild copy engine (`lv_runtime.c:736-806`, `CGen.cpp:130-160`) learns the
bit-62 case: a columnar buffer is scalars-only, so flatten = one length-prefixed byte
blob (like dense-row), rebuild = one allocation + memcpy + header stamp. A dedicated
churn/differential program (`columnar_thread_transfer.lev`) gates this (plan doc §3).

---

## 7. Engine-by-engine specification

- **Oracle** (`Eval.cpp`): zero change. Boxed `std::vector<Value>`; the differential
  reference columnar is validated against.
- **IR interpreter** (`IrInterp.cpp`): implement `ColGet` (= boxed element's field read)
  and `ColSet` (= boxed read-modify-write / in-place per its existing COW discipline)
  over its boxed vectors. ~dozens of lines; no layout, no other change.
- **emit-C++** (`CGen.cpp`): its dense representation is self-contained (one
  `std::vector<V>` of flat slots, `aelem` = classId). Columnar mode = store that vector
  **column-major** (`slot k of element i` at `k*len + i`) and implement
  `ColGet`/`ColSet` as direct index math; `arrGet` keeps gathering (it already does).
  Because CGen's buffers are internal, this leg carries no ABI risk and lands third,
  after the LLVM leg proves the design (plan doc §2).
- **LLVM + runtime**: §§4–6. The runtime core (`lv_arr_go_columnar`, columnar branches
  in append/idxget/idxset/free/flatten, descriptor-table readers) is plain C testable by
  `runtime/selftest.c` **before any codegen exists** — the same B-M1-first shape the
  portable-backend pivot used.
- **X64Gen/ELF**: untouched, frozen, not consulted except as the executable spec of
  row-major semantics.

---

## 8. Default-vs-opt-in — the decision and its rationale (Acceptance Criterion #2)

**Decision: columnar is the default representation for eligible structs, staged behind a
compile-time flag until proven, with no new user-facing type and no annotation.**

- A distinct `Columnar<T>` type was **rejected**: it forks the entire stdlib method
  surface (or demands an interface-sharing story), adds a generics/inference/raw-form
  question (research §11.6), and permanently makes the language's flagship data layout
  the thing you have to know to ask for. The language's philosophy is one general rule
  over special cases; "the same pure `Array<T>` value, laid out better when the element
  type allows it" is the general rule. Value semantics make the layout unobservable, and
  the differential corpus is the proof machine for exactly this kind of claim.
- An annotation (`@columnar`) was **rejected** for the same reason plus the annotation-
  to-codegen plumbing cost; it also invites the worst outcome (two layouts both in
  common use, doubling the churn surface forever).
- The **blast-radius concern** that motivated opt-in in the research (§6.4) is handled
  by *staging*, not by surface area: v1 lands entirely behind `--columnar` (default
  off — zero delta to any existing program), the full differential + churn matrix runs
  in **both modes** in CI during the track, and the final milestone flips the default
  and retains `--no-columnar` for one cycle. This is the LA-30 pump-flip pattern, which
  the project has already executed successfully once.

Consequence: interpreters stay boxed permanently (they never had a dense layout either);
row-major dense remains the permanent representation for ineligible structs; and no
existing source changes meaning at any point — only physical layout, under a flag, then
by default.

---

## 9. The performance claim, stated honestly

- **Wins**: column-selective access over wide scalar structs. The model: row-major
  reading one 8-byte field per element touches one cache line per `recBytes` stride
  (a 8-field struct = 144-byte records → ~2.25 lines/element for 8 useful bytes);
  columnar touches 8 contiguous bytes/element (8 elements per line). Expected ≥3× on
  the benchmark shape (plan doc §4), growing with struct width. Plus the flat ~2×+
  footprint reduction (§4.2) for **all** access patterns, which also helps full-struct
  scans.
- **Does not win (v1)**: closure pipelines (`map`/`where`/`reduce` — gather per element,
  §5.4); whole-struct consumption of narrow structs. A 2-field `Point` fully consumed
  per element may be marginally *slower* columnar (two column touches vs one record
  line); the benchmark reports this regime rather than hiding it (research §11.2 — the
  design does not oversell).
- The C-M0 milestone **prototypes the fused read path and measures before the track
  commits** (research §11.1) — if the index-loop win doesn't materialize on LLVM at
  `-O2`, that is a STOP with the numbers in the implementation log.

---

## 10. Deferred work, with hooks kept warm

1. **Pipeline fusion / `Seq` column projection** — the follow-up that extends the win to
   idiomatic `map`-shaped code: either IR-level recognition of projection+aggregate
   shapes, or a lazy `Seq` stage that carries "column k of arr" as a fused source. The
   `ColGet` op is deliberately shaped as the primitive such a fusion pass would emit.
2. **Refcounted columns** (string/reference fields): requires per-column release walks
   and breaks the "dense buffers carry no refs" invariant — needs its own design; the
   descriptor table's typecode channel is where a heap-typecode would go.
3. **`Block`/`mmap`-backed columns**: a column is a contiguous typed run starting at an
   8-aligned offset — nothing in the layout precludes an external-backing variant later
   (a different bit / a views table). Not designed here; just not designed out.
4. **Narrow strides** (`int8`/`int16`/`int32`): reserved `lv_col_stride` channel (§4.3);
   re-layout is not required to adopt it later.
5. **Column-selective COW / N-buffer representation**: rejected for v1 (multiplies the
   ARC surface by field count; breaks one-array-one-allocation). Revisit only with
   workload evidence that shared-then-mutate-one-column is common.
6. **Geometric growth** for both dense forms.
7. **The standalone-operand `idxset` leak**, both layouts — Lower-side liveness or arena
   routing (research §5); tracked as a stretch item in the plan doc.

---

## 11. Risks (summary — full register in the plan doc)

| risk | mitigation |
|---|---|
| The win doesn't materialize (fused loop no faster) | C-M0 prototype-and-measure gate before any surface work; STOP with numbers |
| Ownership discipline mismatch (alias vs fresh) between modes | compile-time mode only (§4.4); `lv_cfg_columnar` emitted symbol; no runtime env gate |
| Churn regressions in the new layout | dedicated churn family in both modes, XFAIL only where row-major already XFAILs, `--mem-verify` cross-check |
| `arr[i].x = v` source shape assumption wrong | C-M0 verification item; `ColSet` scope shrinks, `ColGet` unaffected |
| Differential drift (gather vs alias observable) | it isn't, by value semantics — and the corpus in both modes is the proof; any diff is a bug by definition |
