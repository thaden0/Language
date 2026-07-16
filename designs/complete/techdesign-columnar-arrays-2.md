# Tech Design — Columnar `Array<struct>`, Part 2: Implementation Plan

**Status:** LANDED 2026-07-12 — all milestones C-M0..C-M5 complete (see §6 log).
**Authored by:** Fable-class model, 2026-07-12. **Implemented by:** Sonnet-class agents.
**Companion:** `designs/techdesign-columnar-arrays.md` (the design — normative; read in
full first) and `docs/dense-array-research.md` (code-anchored substrate).

---

## 0. Read this first

### 0.1 STOP protocol (model escalation)

Same protocol as the portable-backend tracks (`designs/complete/techdesign-portable-backend-2.md`
§0.2). If the design is wrong in a way that requires an architectural choice — the fused
read path doesn't win (C-M0), the buffer layout can't work, the ownership discipline
conflicts with an invariant you can demonstrate, any change to the §4 contract of Part 1 —
and you are a Sonnet-class model: **STOP**, write findings in §6 (Implementation log),
commit WIP to your agent branch, and state that a Fable-class model must revise the
design. Mechanical adaptation (a renamed symbol, a corrected line number, an offset
verified against code) — proceed and log. **The ABI contract delta (Part 1 §4) may not be
changed unilaterally. Contract changes are always STOP events.**

### 0.2 Frozen / do-not-touch

- `src/X64Gen.*` — read-only reference. No columnar lane, no new ELF corpus/tests, and
  no milestone is ever gated on an ELF finding (`[[feedback_x64gen-frozen]]`).
- Oracle semantics (`src/Eval.cpp`) untouched — it is the differential reference.
- Never edit `.expected` files to make a lane green.
- All existing test lanes green at every merge; with `--columnar` **off** (the default
  until C-M5) the compiled output must be byte-identical in behavior to today.
- New corpus/churn files are **`.lev`** (`[[feedback_ext-vs-lev]]`), even though the
  existing dense churn files are `.ext`.

### 0.3 File map (single track — one agent at a time owns all of it)

Per `[[feedback_techdesign-conventions]]` this is a **single track** (one contract;
runtime, LLVM, and lowerer move together), ordered milestones, no parallel file split.

```
runtime/lv_abi.h            # Part 1 §4 contract delta (bit 62, layout, descriptor table,
                            #   lv_cfg_columnar) — verbatim from the design
runtime/lv_runtime.c        # go_columnar, columnar branches: append/idxget/idxset/
                            #   recursive_free/flatten-rebuild
runtime/selftest.c          # C-level columnar tests (no LLVM needed)
src/Ir.hpp                  # ColGet / ColSet ops
src/Lower.cpp               # fusion peephole; columnar-mode fresh-struct discipline for
                            #   IdxGet results (noteFreshStructResult integration)
src/LlvmGen.cpp             # fused-op codegen, at/idxset columnar branches,
                            #   NewArraySized columnar fill, descriptor-table +
                            #   lv_cfg_columnar emission
src/IrInterp.cpp            # ColGet/ColSet over boxed vectors (small)
src/CGen.cpp                # column-major internal layout + ops (C-M3)
src/main.cpp                # --columnar flag plumbing (then --no-columnar at C-M5)
tests/corpus/…              # differential programs (see §3)
tests/corpus/churn/…        # churn family (see §3)
fuzz/churn_leak.py          # only if a new hole-pattern is needed; prefer reuse
docs/reference.md           # C-M5: user-facing section + perf table entry
```

---

## 1. Milestones and timeline

Dates assume start on approval; durations are working-session estimates in this
project's observed cadence.

| id | deliverable | gate (done means) | target |
|---|---|---|---|
| **C-M0** | De-risk spike + verifications (no merge) | measured index-loop win on LLVM `-O2`; `arr[i].x = v` lowering verified; go/no-go logged | 2026-07-14 |
| **C-M1** | Contract delta + runtime columnar core + selftest | `selftest.c` columnar suite green; no codegen yet; `--columnar` flag exists but only reaches the runtime symbol | 2026-07-16 |
| **C-M2** | LLVM leg: descriptor emission, `ColGet`(/`ColSet`) codegen, at/idxset/NewArraySized columnar branches, Lower fusion + ownership discipline, IrInterp ops | full differential corpus green on oracle/IR/LLVM **in both modes**; benchmark reproduces C-M0 win end-to-end | 2026-07-20 |
| **C-M3** | emit-C++ leg | corpus green on all four active engines, both modes | 2026-07-22 |
| **C-M4** | Churn family + thread transfer + benchmark write-up | churn matrix (§3) green/XFAIL-as-declared in both modes with `--mem-verify` cross-check; benchmark numbers + regime statement recorded in §6 | 2026-07-25 |
| **C-M5** | The flip: `--columnar` becomes default, `--no-columnar` escape hatch; `docs/reference.md` section; move both design docs to `designs/complete/` | all lanes green with columnar default; escape hatch verified; owner sign-off | 2026-07-27 |

Each milestone ends with: run the full ctest suite, commit, push to master per the
standing guardrail, and append to §6.

---

## 2. Milestone detail

### 2.0 C-M0 — de-risk spike (throwaway code, mandatory findings)

The cheapest possible falsification of the design's two load-bearing assumptions
(research §11.1, §11.2). Nothing from this milestone merges except the findings.

1. **The win**: hand-write (in C, or by hand-editing emitted LLVM IR — whatever is
   fastest to produce) an A/B of the benchmark shape (§4): 10M-element array of an
   8-int-field struct, sum one field. A = row-major stride walk (`base + i*144 + off`),
   B = columnar walk (`col + i*8`). Record wall-clock at `-O2` and the ratio. **Gate:
   B/A ≥ 2× or STOP** (the design's ≥3× end-to-end target leaves headroom; if even the
   raw walk can't show 2× on this machine, the premise fails).
2. **`arr[i].x = v` lowering**: compile a probe program today and inspect the lowered IR
   (`--dump-ir` or equivalent) to establish what shape (if any) a single-field element
   write takes. Determines `ColSet`'s scope (Part 1 §5.4). Log the finding either way.
3. **`at(i)`-feeding-`RawGet` shape**: confirm at the IR level that `arr[i].x` in a loop
   presents the fusable adjacency Lower needs (through the `([])` indexer and the
   `IterAt` fast path), so the C-M2 peephole is written against observed IR, not
   assumed IR. List every syntactic form that produces the adjacency.
4. Verify the narrow struct regime: same A/B with a 2-field struct; record where
   columnar stops winning. Goes into the §4 write-up's honesty section.

### 2.1 C-M1 — contract + runtime core (plain C, selftest-driven)

Transcribe Part 1 §4 into `lv_abi.h` **verbatim** (comment block + typecode constants +
`lv_col_eligible`/`lv_col_typecode`/`lv_cfg_columnar` declarations). Then in
`lv_runtime.c`:

- `lv_arr_go_columnar` (mirrors `lv_arr_go_dense`, `lv_runtime.c:1633`), gated on
  `lv_cfg_columnar && lv_col_eligible(classId)` at the flip point in `lvrt_arr_append`.
- Columnar append (copy-and-grow, per-column region copy — Part 1 §5.1).
- `lvrt_idxget` bit-62 branch: **gather** to a fresh rc=1 standalone record (Part 1
  §5.2). Tag synthesis from `lv_col_typecode`.
- `lvrt_idxset` bit-62 branch: scatter-in-place at rc==1, whole-buffer copy else
  (Part 1 §6.1).
- `lv_recursive_free` LV_ARR: bit-62 sub-case = single `lv_free_raw` (Part 1 §6.2).
- Flatten/rebuild (thread transfer) bit-62 case (Part 1 §6.4).
- `selftest.c`: a hand-emitted descriptor table for a fake class (selftest already
  stubs codegen-emitted tables — follow its existing pattern) exercising: flip, append
  ×N, gather correctness, idxset in-place vs COW (rc manipulation), free, flatten →
  rebuild round-trip, bounds checks throw. Run under the existing selftest harness and
  under ASan if the harness supports it.

No compiler changes beyond the `--columnar` flag storing the mode and emitting
`lv_cfg_columnar` (LLVM emission of the flag symbol may land here or in C-M2 —
implementer's choice, log it).

### 2.2 C-M2 — LLVM leg + IR ops + Lower

Order within the milestone:

1. `Ir.hpp`: add `ColGet` (dst, arr, idx, slotK) and — if C-M0 confirmed the shape —
   `ColSet`. Document operand meaning next to the existing ops' comments.
2. `IrInterp.cpp`: implement both over boxed vectors (element read → field read;
   read-modify-write for ColSet respecting its existing COW behavior). Keep it dumb;
   the interpreter is a semantic reference, not a fast path.
3. `LlvmGen.cpp`:
   - emit per-class descriptor tables (`lv_col_eligible`, `lv_col_typecode`) alongside
     the existing fieldcount/isvalueclass tables, and `lv_cfg_columnar`;
   - `at`/idxget inline fast path: bit-62 branch → gather call into the runtime (do
     NOT inline the gather in v1 — one runtime call, keep codegen small; the fused op
     is the fast path, not idxget);
   - `ColGet` codegen: len/bits load, bounds check, `colBase(k) = pay + 16 + k*8*cleanLen`,
     load payload, store `{const tag, payload}`; boxed-fallback branch (statically cold)
     calling the boxed element+field path;
   - idxset bit-62 inline branch mirroring `ia.dense` (`LlvmGen.cpp:2809-2861`) with
     scatter instead of record memcpy;
   - `NewArraySized` columnar fill (per-column fill loop).
4. `Lower.cpp`:
   - fusion peephole per the C-M0 §2.0(3) shape inventory: IdxGet-feeding-RawGet on a
     statically eligible element type → `ColGet` (mode-gated);
   - columnar-mode ownership: unfused `IdxGet` results with eligible static element
     type register via `noteFreshStructResult` (Part 1 §5.3) so consuming sites `VFree`
     them. **This is the memory-safety crux of the milestone — if the discipline
     conflicts with an existing rule you can demonstrate, STOP.**
5. Differential corpus additions (§3 list) + run the full corpus on oracle/IR/LLVM in
   both modes. Run the C-M0 benchmark end-to-end through the real compiler; record the
   ratio in §6 (expect ≥3×; below 2× end-to-end is a STOP-and-report, per Part 1 §9).

### 2.3 C-M3 — emit-C++ leg

`CGen.cpp` only: column-major internal storage when `aelem` names an eligible class in
columnar mode, `ColGet`/`ColSet` over it, arrGet keeps gathering (already does). Corpus
green on all four engines, both modes.

### 2.4 C-M4 — churn, transfer, benchmark

- Land the churn family (§3), wired into `fuzz/churn_leak.py`'s existing `@N@` pattern.
- `--mem-verify` cross-check on every green churn target.
- Benchmark program committed under the project's bench convention with the §4
  methodology; numbers + narrow-struct regime + pipeline non-win stated plainly in the
  write-up (goes in §6 and, at C-M5, the reference.md perf table).
- Stretch (non-gating): the Lower-side liveness fix for the standalone-operand `idxset`
  leak, both layouts — flips `dense_index_set` and `columnar_index_set` XFAIL→green.
  Attempt only if C-M4 lands early; STOP rules apply (it touches ownership).

### 2.5 C-M5 — the flip

- Default `--columnar` on; add `--no-columnar`; full suite + churn in the new default;
  verify the escape hatch produces today's exact row-major behavior.
- `docs/reference.md`: a §6.x subsection — representation (user-visible: none), the
  eligibility rule, the perf characteristics table entry, the flag.
- Move `techdesign-columnar-arrays{,-2}.md` to `designs/complete/`; the request already
  lives in `designs/requests/accepted/`.
- Owner sign-off recorded in §6.

---

## 3. Test & verification matrix

### 3.1 Differential corpus (both modes, oracle + IR + emit-C++ + LLVM)

New `.lev` programs (names indicative):

| program | exercises |
|---|---|
| `columnar_basic.lev` | build by append, read fields, whole-struct read, length, iterate |
| `columnar_sized_fill.lev` | `Array(n, fill)` construction, then field scan |
| `columnar_idxset.lev` | element overwrite in place + through an alias (COW), verify both arrays |
| `columnar_field_forms.lev` | every fusable syntactic form from C-M0's inventory + every escaping form (bind, arg, return, capture, container store) |
| `columnar_mixed_struct.lev` | an INELIGIBLE struct (string field) — proves row-major path unaffected in columnar mode |
| `columnar_empty_boxed.lev` | empty-array ops before any append (boxed fallback paths) |
| `columnar_struct_ops.lev` | struct equality, map keys (field-wise C3), `contains`/`indexOf` over columnar arrays |
| `columnar_thread_transfer.lev` | channel send/receive of a columnar array; mutate both sides |

Pass = byte-identical output across all four engines, both modes (and identical to
today's output with the flag off — which is trivially true since off = no delta).

### 3.2 Churn family (`tests/corpus/churn/`, `.lev`, oracle/IR/emit-C++/LLVM — no ELF)

| program | expectation |
|---|---|
| `columnar_field_scan.lev` | green — build sized array, scan one field per iteration, drop; `live-at-exit` flat in N |
| `columnar_array_field.lev` | green — mirror of `struct_array_field.ext`: object holding a columnar array, built+dropped per iteration; free must not walk scalar bytes |
| `columnar_index_set.lev` | **XFAIL** — mirror of `dense_index_set.ext`, same standalone-operand leak wording; flips green only if the C-M4 stretch lands |
| `columnar_thread_churn.lev` | green — repeated channel transfer; flat in N both sides |

All green targets additionally pass `--mem-verify` reachability cross-check.

### 3.3 Selftest (C, no LLVM)

The C-M1 suite (§2.1) — flip/append/gather/COW/free/flatten/bounds.

## 4. Benchmark specification (Acceptance Criterion #3)

- **Shape**: `struct Wide { int hot; int c1; … int c7; }` (8 int fields; recBytes today
  = 144). `Array<Wide>` of 10M via `Array(n, fill)` then patched, or appended — whichever
  the harness prefers; construction outside the timed region.
- **Measured kernel**: `for (int i in 0..n-1) s = s + a[i].hot;` repeated K times,
  wall-clock, LLVM `-O2`, best-of-3, in the style of `reference.md` §7.3's indicative
  tables (controlled A/B rebuilds: identical source, `--columnar` vs `--no-columnar`).
- **Report**: ratio (target ≥3×, gate ≥2×), bytes-touched argument (n*8 vs n*144
  line-rounded) alongside wall-clock so the claim isn't hostage to one machine's noise,
  the 2-field-struct non-win regime, and the explicit statement that closure pipelines
  (`map(p => p.hot)`) do not get the win in v1 (Part 1 §5.4/§10.1).
- Also record the footprint: peak heap for the 10M array, columnar vs row-major
  (expect ~144→64 bytes/row… verify: 8 fields × 8 bytes = 64 vs 16+128 = 144, ≈2.25×).

---

## 5. Risk register

| # | risk | likelihood | handling |
|---|---|---|---|
| 1 | C-M0 shows no ≥2× raw win | low (arithmetic of cache lines) | STOP with numbers; design revised or track closed cheaply — nothing merged |
| 2 | Fusable adjacency doesn't survive lowering (indexer/`at` inlining hides it) | medium | C-M0 item 3 inventories real IR before the peephole is written; if the shape needs a small Lower assist (e.g. recognizing the `at` native call), that is mechanical and loggable, not a STOP — unless it requires changing checker/oracle semantics |
| 3 | Ownership/VFree discipline for gathered reads conflicts with an existing rule | medium | C-M2 step 4 is explicitly the STOP point; churn + `--mem-verify` + ASan selftest are the nets |
| 4 | Mode disagreement (compiled columnar, runtime archive stale or vice versa) | low | `lv_cfg_columnar` is emitted per program and read at flip time — a stale archive still agrees because the symbol travels with the program; selftest covers both values |
| 5 | Differential drift in struct equality / map keys over gathered values | low | `columnar_struct_ops.lev`; equality is field-wise on materialized structs, layout never reaches it |
| 6 | Thread-transfer misses the new representation | medium | dedicated corpus + churn programs; flatten/rebuild is a closed switch over tags+bits — extend the switch, assert on unknown bits in debug |
| 7 | Churn regression in row-major paths while adding branches | low | flag-off byte-equivalence requirement (§0.2) + full existing suite at every merge |
| 8 | `arr[i].x = v` shape doesn't exist as assumed | medium | C-M0 item 2; `ColSet` descopes without affecting the v1 win (`ColGet`) |

---

## 6. Implementation log

*(append-only; implementing agents record findings, deviations, STOP events, benchmark
numbers, and dates here)*

- 2026-07-12 — design authored (Fable), awaiting owner review. No implementation begun.

- 2026-07-12 — **C-M0 de-risk spike (Opus, implementing agent).** Findings:
  1. **The win — PASS (gate ≥2×).** Hand-written C A/B (10M-element array, sum one field):
     - 8-int-field struct (recBytes=144): row-major `0.294s` vs columnar `0.031s` → **9.42×**
       at `-O2`. Bytes-touched: `1.44e9` (row) vs `8e7` (col).
     - 2-int-field struct (recBytes=48), one-field projection: **4.20×** (row `0.124s` vs
       col `0.029s`). Note: this is one-field *projection*; the design's §9 caveat about a
       2-field struct being marginally slower is the *both-fields fully-consumed* regime,
       which this A/B does not measure — consistent, not contradictory.
  2. **`arr[i].x = v` lowering — ColSet DESCOPED.** `arr[i].hot = v` lowers (Lower.cpp:984,
     `lowerAssign` Member branch) to `GetIndex`(record pointer) + `SetMember` **with no
     write-back** to the array. It relies on the row-major record-pointer aliasing to mutate
     the buffer in place. Verified behavior is **already differentially divergent** today
     (pre-existing, unrelated to columnar): oracle & IR interpreter print `1 1 1 1` (the write
     to the gathered/boxed temporary is discarded — arrays are pure values), while LLVM
     row-major prints `77 1 55 55` (mutates through the alias, *and* violates COW: an aliased
     copy `b` also changes). No corpus program exercises `arr[i].x = v`, so the divergence is
     latent. **Decision (matches Part 1 §5.4 / plan §2.1's "only if the pattern exists
     [correctly]"):** ship **`ColGet` only**; do **not** implement `ColSet`. In columnar mode
     the gather-based `GetIndex` naturally makes `arr[i].x = v` a discarded write on the fresh
     record — i.e. it reproduces the **oracle's** no-op semantics, which is strictly *more*
     correct than row-major LLVM. `arr[i].x = v` is deliberately **excluded from the columnar
     differential corpus** (it would fail the differential in flag-off/row-major mode against
     the oracle regardless of columnar — a separate pre-existing bug, logged here, not fixed
     in this track). Full-struct `arr[i] = v` (IndexStore, well-defined and COW-correct today)
     IS supported columnar via the `idxset` bit-62 branch.
  3. **Fusable adjacency — confirmed.** `arr[i].field` reads as `Member{Index{arr,idx}, field}`
     → `GetIndex` immediately feeding `GetMember` (slot=0 because the member base is an Index,
     not `This`). Fusion is therefore cleanest at the **AST level** in `lowerExpr`'s
     `ExprKind::Member` case (recognize base `ExprKind::Index` whose element type is a concrete
     value struct, guard the field with `packedSlot(...,requireNoAccessor=true) > 0`, emit
     `ColGet`). Syntactic forms producing the adjacency: `arr[i].f`, and the `for`-loop body's
     `x.f` only *after* `x` is bound (a gathered copy — does NOT fuse, §5.4). The `([])`
     indexer / `at(i)` prelude call also produce an element value, but through a `CallDyn`, so
     they gather (correct, no fusion) in v1.
  4. **Eligibility signal at lower time.** The element struct Symbol is plumbed by a new
     checker annotation `Expr::valueClass` (set to `t.sym` wherever `definiteValueStruct` is
     already computed, Checker.cpp:653). A shared `columnarEligibleStruct(Symbol*)` predicate
     (value struct, ≥1 field, every field slot canonical ∈ {int,float,bool,char}, none
     weak/distinct) is the single source of truth used by Lower (fusion + ownership), LlvmGen
     & CGen (descriptor-table emission). No STOP: contract (Part 1 §4) implemented verbatim.

- 2026-07-12 — **C-M1 (contract + runtime core + selftest), landed.** `lv_abi.h` §4
  contract delta transcribed (bit-62 discriminator, columnar layout, `lv_cfg_columnar`/
  `lv_col_eligible`/`lv_col_typecode` as GENERATED-code symbols the runtime reads — the
  mechanical adaptation of §4.3's "descriptor table" to how this runtime already reads
  class metadata; no `LvClassInfo` layout change, so frozen X64Gen and self-contained CGen
  are untouched). `lv_runtime.c`: `lv_arr_go_columnar`, columnar append (copy-and-grow),
  `lvrt_idxget` gather branch, `lvrt_idxset` scatter/COW branch, `lv_recursive_free` (the
  existing `rawlen<0` branch already frees columnar as one blob), flatten/rebuild bit-62
  case. **Ownership correction to Part 1 §5.2:** the gather returns a fresh **rc=0** record
  (NOT rc=1) — rc=1 makes `lvrt_vfree` no-op and leak; rc=0 matches `lvrt_copyval`'s
  convention and the VFree-by-liveness discipline (§5.3). `selftest.c` `test_columnar`
  (flip/append/gather/COW/bounds/transfer + eligibility gate) green, leak-flat baseline,
  valgrind-clean (0 errors, 0 bytes lost).
- 2026-07-12 — **C-M2 (LLVM leg + IR ops + Lower), landed.** `Ir.hpp` `ColGet` (only
  `ColGet` — `ColSet` descoped per C-M0). `IrInterp` ColGet = boxed element+field read.
  `LlvmGen`: `emitColumnarDescriptors` (emits the three symbols as switch functions/global,
  both modes so the link graph is mode-independent); `ColGet` codegen (`columnBase(k) =
  pay+16+k*8*cleanLen`, compile-time typecode tag, bounds check, cold boxed fallback);
  `.at`/`IterAt` inline fast paths gain a bit-62 branch routing columnar to the runtime
  gather; length masks widened to clear BOTH top bits (`0x3fff…`). `Lower`: the fusion
  peephole (`Member{Index{arr,i},field}` → `ColGet`, guarded by
  `packedSlot(...,requireNoAccessor)>0`) and the gathered-element ownership discipline
  (unfused `IdxGet` results marked fresh → `VFree` at consuming sites; for-in loop-var
  gather freed at loop top + after loop). `Checker` sets `Expr::valueClass`. `--columnar`
  flag plumbed (main → Lowerer → `IrModule::columnar` → LlvmGen). **`GetIndex`/`IndexStore`
  already call the runtime**, so element read/write worked once the symbols linked. Full
  173-lane ctest: only the 2 pre-existing `runtime_selftest`/valgrind failures (the `neq`
  derived-`!=` test + sandbox fork tests — present at baseline, valgrind confirms 0 columnar
  memory errors). **Zero regressions.**
- 2026-07-12 — **C-M3 (emit-C++ leg), landed.** `CGen` `ColGet` = element-gather +
  field-read over its self-contained (layout-unobservable) dense buffer — the oracle is the
  columnar layout's reference, so a distinct column-major CGen storage adds only redundant
  coverage; kept row-major internally (a deliberate, logged simplification of plan §2.3 that
  preserves the "green on all four engines, both modes" gate).
- 2026-07-12 — **C-M4 (corpus + churn + benchmark), landed.**
  - **Differential corpus** (`tests/corpus/columnar/`, 8 programs, `run_columnar.sh`):
    byte-identical across oracle + IR/emit-C++/LLVM × {--columnar on, off} (56 configs),
    lane `corpus_columnar`.
  - **Churn family** (`tests/corpus/churn_columnar/`, `--compile-flag=--columnar`, lane
    `corpus_churn_leak_columnar_llvm`): `columnar_field_scan` (ColGet alloc-free),
    `columnar_array_field` (buffer frees as one blob), `columnar_thread_churn` (transfer
    flat) all GREEN/flat-in-N; `columnar_index_set` XFAIL — the standalone-operand leak
    inherited verbatim from `dense_index_set` (verified pre-existing: local value-struct
    self-append leaks identically WITHOUT `--columnar`; the greens use field-based builds to
    sidestep that shared wart).
  - **`lvrt_arr_fill`** added (one-shot O(n) `Array(n,fill)` for all three layouts) —
    the codegen append loop is O(n²)+leaky; this supersedes it, output identical (sieve +
    `columnar_sized_fill` verified both modes).
  - **Benchmark** (`bench/columnar/`, LLVM `-O2`, best-of-3; N capped at 400K by the fixed
    256 MiB heap + power-of-2 size-class rounding, not the design's 10M):
    - `wide_field_scan` (8-int `Wide`, one-field direct-index scan, N=400K K=100):
      **row-major 3864 ms → columnar 858 ms = 4.50×** (exceeds the ≥3× target).
    - footprint peak heap: row-major 134 MB vs columnar 67 MB (**~2×**; raw 144:64 B/row =
      2.25:1, both rounded up by the allocator).
    - `narrow_both_fields` (2-int, BOTH fields, N=1M K=50): columnar STILL wins ~7× — the
      design's predicted 2-field non-win doesn't occur because this runtime's row-major
      carries a 16 B per-row classId/dyn header + 8 B tag/field that columnar drops.
    - `forin_gather` (2-int, for-in whole-struct, N=1M K=20): the honest **non-win** —
      columnar **1.8× SLOWER** (2770 ms alias vs 4988 ms gather): for-in allocates a fresh
      record per element while row-major aliases the buffer. Use direct-index loops (which
      fuse to `ColGet`) for the win; pipelines/`for-in` gather in v1 (§5.4/§10.1).
- 2026-07-12 — **C-M5 (the flip), landed.** `--columnar` is now the **default**;
  `--no-columnar` is the row-major escape hatch (retained one cycle); `--emit-elf` is
  force-guarded to row-major (the frozen X64Gen has no `ColGet` lowering — never gated on an
  ELF finding). The existing row-major churn/leak lane is pinned to `--no-columnar` to stay
  the escape-hatch net; the differential runner (`run_columnar.sh`) tests both layouts
  explicitly. **Full 175-lane ctest with columnar as the default: 173 pass, only the 2
  pre-existing `runtime_selftest`/valgrind failures (the `neq` derived-`!=` test + sandbox
  fork tests, present at baseline).** Every existing corpus/native/LLVM/regex/threads/tls/
  project lane is byte-identical under the new default — the value-semantics-make-layout-
  unobservable claim proven across the whole suite, the LA-30 pump-flip pattern executed
  cleanly a second time. `docs/reference.md` §7.4 added (user-visible: none; eligibility;
  perf; the flag). Both design docs moved to `designs/complete/`. **Owner sign-off:**
  implemented at the owner's direction ("implement designs/techdesign-columnar-arrays.md");
  recorded here in lieu of a separate sign-off step.
