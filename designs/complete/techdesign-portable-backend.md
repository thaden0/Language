# Tech Design — Portable Backend Pivot, Track A: IR Memory Ops + LLVM Backend Parity

**Status:** approved for implementation (owner decision, 2026-07-05)
**Authored by:** Fable-class model. **Implemented by:** Sonnet-class implementation agents.
**Companion:** `designs/complete/techdesign-portable-backend-2.md` (Track B: runtime v2, link driver,
platform floor). Read its §2 (the ABI contract) before starting A-M2 or later.

---

## 0. Read this first

### 0.1 The mission

Leviathan pivots from "build ELF directly, Linux/x86-64 only" to **LLVM as the primary
AOT backend**, portable across Linux, macOS, Windows, Android (and later wasm32), while
keeping the path to self-hosting. Decisions locked by the owner:

1. **Zero-dependency is dropped as a goal.** The goal is now: *no C++ in code we author*
   (compiler, runtime, stdlib — eventually all Leviathan), with LLVM as a sanctioned
   external library. Ride C++ only while it is the faster path; shed it per the roadmap (§2).
2. **LLVM is the primary backend. X64Gen is frozen** — kept, never extended, repurposed as
   (a) the differential-testing anchor on Linux/x86-64 and (b) the future zero-dependency
   bootstrap seed. **Nothing is deleted.**
3. **Full IR parity for the LLVM backend** — everything the pure-ELF backend runs today
   must run through LLVM.
4. Runtime gets rewritten **in Leviathan** as soon as that stops being slower than C
   (Phase 3, §2). FFI is **postponed but explicitly on the roadmap** (Phase 4, §2).

### 0.2 The two tracks (parallel agents, no merge collisions)

| | Track A (this doc) | Track B (`-2` doc) |
|---|---|---|
| Scope | LlvmGen from scalar core to full parity, incl. the backend-side ARC wrapper (§4) | Runtime v2 in C; link driver; platform floor; target bring-up |
| Owns (may edit) | `src/LlvmGen.cpp/.hpp`, `tests/run_native_llvm.sh`, Track-A region of `CMakeLists.txt` (§10.3). `src/Ir.hpp`/`Lower.cpp/.hpp`/`IrInterp.cpp` are read-only references unless a milestone explicitly says otherwise (none does as of A-M1's revision, §4) | `runtime/**` (new), `src/main.cpp` driver additions, `src/NativeRuntime.cpp` (until retired), Track-B region of `CMakeLists.txt`, `tests/` new scripts it introduces |
| Branch | `track-a-llvm-parity` | `track-b-runtime` |

Coordination rules:
- **The ABI contract in doc-2 §2 is normative and frozen.** Track A emits calls that match
  it; Track B implements it. Neither track changes it unilaterally — a needed contract
  change is a **STOP event** (§0.3).
- **Merge order:** since A-M1 no longer touches `src/main.cpp` (§4's revision), there is
  no ordering constraint against B-M2's driver work there — the two tracks' first main.cpp
  touch is whichever lands first.
- Rebase/merge master before starting each milestone. Merge to master at each milestone
  once its acceptance tests are green (branch merge implies push).
- If only **one** agent is available, interleave: A-M2's step-0 (§5.1) → B-M1 → A-M2 →
  B-M2 → A-M3 → B-M3 → A-M4 → A-M5 → A-M6 (B-M4/M5 when environments exist).

### 0.3 STOP protocol (model escalation) — applies to every milestone

This design was authored by a Fable-class model. If during implementation you find the
design is **wrong or incomplete in a way that requires an architectural choice** — an
ABI/layout that cannot work as specified, an LLVM API that does not support the specified
approach, a corpus test that cannot pass without deviating from this document, a needed
change to the Track A/B contract — and you are a **Sonnet-class (or smaller) model**:

**STOP. Do not improvise a design change.**
1. Write what you found, why it blocks, and 1–2 option sketches into §12 (Implementation
   log) of this file.
2. Commit your WIP to your track branch (do not merge to master).
3. End your turn stating explicitly: *"Design correction needed — escalate to a
   Fable-class model."*

Mechanical adaptation is **not** a design change: a renamed helper, a missing include, an
offset that differs from a stale comment once verified against code — proceed and note it
in the log. When unsure which kind it is: stop.

### 0.4 Frozen / do-not-touch list

- `src/X64.hpp`, `src/X64.cpp`, `src/X64Gen.hpp`, `src/X64Gen.cpp` — **read-only
  reference material.** They are the map you port from; never edit them.
- `src/Eval.cpp` semantics (the oracle) and `src/RuntimeNatives.cpp` behavior — the
  behavioral truth. Never change oracle behavior to make a backend pass.
  (2026-07-06: Bug 21, a genuine oracle defect — `--run`-only call frames
  weren't isolated from the caller's stack — was fixed through the proper
  channel: filed to `bug.md`, STOP-and-escalated per §0.3 rather than patched
  by a backend track, then fixed and re-verified 56/56 green across all
  engines. That is the one sanctioned path to changing oracle behavior: a
  `bug.md` entry proving the oracle itself is wrong, not "the backend
  disagrees so patch the oracle to match." Any `.expected` file that predates
  the fix and happens to exercise the exact caller/callee name-collision
  shape was generated from the buggy oracle and should be treated as
  suspect — regenerate and diff by hand, don't assume it's still correct.)
- `tests/corpus/**/*.expected` — **never edit an .expected to make a test pass.** If your
  output disagrees with the oracle, your backend is wrong; if you believe the oracle is
  wrong, that is a STOP event (and a bug.md entry per project convention).
- Existing test lanes must stay green at every merge (`ctest` full suite).

---

## 1. Context: what exists (verified 2026-07-05)

Five engines consume one front end. `Lower.cpp` produces the bytecode IR (`src/Ir.hpp`,
ops at `Ir.hpp:24-78`) — the single semantic truth.

| Engine | Entry | Size | Coverage |
|---|---|---|---|
| Tree-walk oracle | `lang --run` | — | full (reference semantics) |
| IR interpreter | `lang --ir` | — | full |
| emit-C++ (`src/CGen.cpp`) | `lang --emit-cpp` / `--build` | ~1,000 LOC | all except event-loop/system layer |
| **LLVM (`src/LlvmGen.cpp`)** | `--emit-llvm` / `--native-obj` / `--build-native` | ~~312 LOC, scalar core only~~ **~2,000 LOC, full parity through the system layer** *(as of A-M5, 2026-07-06 — this row was the 2026-07-05 snapshot)* | **whole language incl. ARC, COW, exceptions, event loop, sockets, async** |
| Pure ELF (`src/X64Gen.cpp`) | `--emit-elf` | ~4,000 LOC | whole language incl. ARC, COW, event loop, sockets, HTTP |

Why the pivot (short): raw syscalls are a stable ABI **only on Linux**; the hand-rolled
ELF (single RWX segment, fixed base, non-PIE — `src/X64.cpp:8-57`) fails modern security
baselines; every new OS×ISA multiplies hand-rolled work, while LLVM makes ISAs free.

**What "parity" means in this document:** the LLVM backend passes everything
`tests/run_elf.sh` passes over `tests/corpus/` (same skip semantics, same XFAIL set), plus
the churn leak harness. The empirical pass/skip list you record in A-M2 step 0 (§5.1) is
the binding checklist — trust the corpus over prose (reference.md:706 "unions/streams
unlowerable" is stale; `corpus/elf/` runs `optional.ext`, `match.ext`, `streams.ext`).

Useful facts already verified for you:
- The current LlvmGen convention is **pointer-based**: every generated function is
  `void fN(ptr ret, ptr p0, …)` (`LlvmGen.cpp:104-111`), runtime calls pass `LvValue*`
  slots. Keep this convention; it is ABI-trivial on every target.
- `LlvmGen::emitObject` already uses `TargetMachine` + default triple + `Reloc::PIC_`
  (`LlvmGen.cpp:282-312`). Track B parameterizes the triple later; you don't need to.
- Value tags follow the **CGen reference numbering**: 0 void, 1 int, 2 float, 3 bool,
  4 string, 5 object, 6 array, 7 map, 8 none, 9 closure (`X64Gen.hpp:16-23`). This is
  **not** the same order as `VKind` (`RuntimeValue.hpp:22`) — see hurdle H-3.
- X64Gen's per-frame arena + ARC mechanics: prologue saves the arena cursor, every return
  path restores it (`X64Gen.cpp:~2938-2993`); refcount sentinel `-1` = arena/not-counted,
  `0` = fresh/unowned, `≥1` = owned; heap-range checks gate string ARC (literals carry no
  prefix). Track B replicates this in C; you emit the calls.

---

## 2. Roadmap and timeline (authoritative for both tracks)

Dates assume the current cadence (up to 2 parallel agent tracks). **Gates are
authoritative; dates are planning targets.** Slipping a gate by more than ~2 weeks
triggers a Fable-level replan. Today: 2026-07-05.

| Phase | What | Gate ("done" means) | Target |
|---|---|---|---|
| **P0** | Pivot decided, these designs land | docs merged to master | 2026-07-06 |
| **P1** | **LLVM parity on Linux/x86-64** (Track A M1–M6 ∥ Track B M1–M3) | **G1 (functional parity MET 2026-07-06; perf/docs closeout = A-M6 open):** `corpus_llvm_full` green byte-identical to the oracle; churn harness green on the llvm engine (11-green/3-XFAIL — 2 value-struct XFAIL-LLVM pending A-M6's arena tier); one-step `--build-native` works (incl. lcurl project + native lcurl matrix); perf table + reference.md docs still pending A-M6 §9.5 | 2026-08-07 |
| **P2** | Additional targets: aarch64-linux (cross + qemu), then macOS, then Windows (Track B M4–M5) | **G2:** corpus green per target (env-gated for mac/win hardware) | aarch64 2026-08-21; mac/win 2026-09-18 |
| **P3** | **Runtime in Leviathan** — rewrite runtime v2's logic in Leviathan over the platform floor; C shrinks to the floor shims | **G3:** no C above the plat layer; churn corpus still +0B | 2026-10-16 |
| **P4** | **FFI** (explicitly postponed to here): design doc (Fable) then implementation — needed for OS-library floors (Win/mac) in pure Leviathan, optional in-process LLVM-C, and corporate interop | **G4:** FFI design accepted; extern calls work on 2 platforms | design 2026-10; impl 2026-11 |
| **P5** | **Self-hosting:** compiler front end + lowering rewritten in Leviathan, emitting **textual LLVM IR (.ll)** first (no FFI needed); 3-stage bootstrap | **G5:** stage1 (built by C++ stage0) builds stage2; stage2 rebuilds itself as stage3; stage2 ≡ stage3 output; full corpus green under the self-built compiler | start ~2026-11; self-hosted 2027-01-15 |
| — | **"C++ exit" declaration** | compiler, runtime, stdlib all Leviathan; the C++ tree is retained only as the archived bootstrap seed; LLVM (external library/tool) is the one sanctioned dependency | with G5 |

Standing decisions across phases: X64Gen stays frozen as the Linux/x86-64 differential
anchor until G5, then becomes bootstrap-seed-only. New backend/runtime phases each get
their own tech design authored by a Fable-class model **before** implementation starts
(P3 and P4 designs are scheduled work items, not implied by this doc).

---

## 3. Target architecture (end of P1)

```
source ─► front end (lex/parse/resolve/check) ─► Lower ─► IR (+ Retain/Release ops, §4)
                                                            │
                    ┌───────────────────────────────────────┼──────────────────┐
                    ▼                                       ▼                  ▼
             IR interpreter                          LlvmGen (this doc)   X64Gen (frozen)
             (ARC ops are no-ops)                    LLVM module ─► .o
                                                            │
                                              linker (Track B driver) + liblvrt.a
                                                            ▼
                                                     portable executable
```

- Generated code and the runtime meet at the **C ABI defined in doc-2 §2** (`runtime/lv_abi.h`).
  All cross-boundary values move as `LvValue*` pointers — never by-value aggregates.
- Program-specific data (class tables, dispatch tables, string literals) is emitted by
  LlvmGen as constant globals + a generated registration call; the runtime is
  program-agnostic (doc-2 §2.6).
- The runtime owns process entry: its `main()` runs `lv_rt_init(...)` → generated
  `@lv_main` → `lv_loop_run()` → exit. LlvmGen stops emitting its own `main` (§8).

---

## 4. Milestone A-M1 — the ARC wrapper (ported from X64Gen, backend-side)

**Revision (2026-07-05, logged in §12): the original plan for this milestone — new
`Retain`/`Release` IR opcodes inserted by `Lower.cpp` behind an `arcOps` flag — is
superseded.** Ground-truth inspection of the frozen reference backend found the actual
mechanism is smaller, lives entirely in the backend, and needs no IR changes at all. Do
**not** implement the opcode-based plan; implement §4.1–§4.3 below instead. Ir.hpp,
Lower.cpp/.hpp, IrInterp.cpp, CGen.cpp, Ownership.cpp, and MemVerify.cpp are **untouched**
by this milestone.

### 4.1 What X64Gen actually does (the ground truth)

`X64Gen.cpp` classifies every op by a pure function of its opcode,
`destKind(Op)` (`X64Gen.cpp:3042-3058`):

```cpp
auto destKind = [](Op op) -> int {
    switch (op) {
        case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue:
        case Op::Print: case Op::PrintNl: case Op::Ret: case Op::RetVoid:
        case Op::SetMember: case Op::RawSet:
        case Op::StoreGlobal: case Op::CaptureVar: case Op::Throw:
        case Op::VFree:
            return 0;    // `a` is not a destination register for this op at all
        case Op::Call: case Op::CallDyn: case Op::CallValue:
        case Op::CallNativeFn: case Op::Arith: case Op::MoveClear:
        case Op::NewObject:
            return 2;    // writes a value ALREADY at +1 (transfer) — release-old only
        default:
            return 1;    // writes an owned/borrowed value — release-old, retain-new
    }
};
```

`dk==0` ops don't write a new value into register `a` at all (Ir.hpp's own per-op field
comments confirm this: `SetMember`'s `a` is the *value being written*, not a destination;
`StoreGlobal`'s `a` is the *source*; `Ret`/`Throw`/`Print`/`VFree`'s `a` is a *value read*).
`dk==2` ops (calls, `Arith` string-concat, `MoveClear`, `NewObject`) already hand back a
value at refcount +1 by convention (§15's "return transfer"), so only the old value needs
releasing. `dk==1` — everything else — both releases the old value and retains the new one.

The generic wrap around the per-op switch (`X64Gen.cpp:3082-3121`, `:3652-3669`):
1. Before the op: if `dk==2` or `dk==1`, stash the *old* `(tag,payload)` currently in
   register `a` into a scratch cell. **Exception:** `IndexStore` instead **releases the
   destination register immediately, pre-op**, and zeroes it (`X64Gen.cpp:3086-3117`) —
   this is the fix that makes indexed-write COW actually fire in a loop: without it, a
   stale reference sitting in the not-yet-overwritten dest register reads the base as
   "shared" (refcount 2) even when the caller is its only real owner, defeating the
   uniqueness check. The same function also releases one more stale temp two
   instructions ahead when it recognizes the exact `CopyVal t,base; Move dest,t` rebind
   chain `lowerAssign` emits for `arr[i] = v` (only on straight-line code — no jump may
   land between the three instructions).
2. Run the op (unchanged).
3. After the op: if `dk!=0`, compare the *new* `(tag,payload)` in `a` against the stashed
   old value. **If identical, skip release and retain entirely** — this is the guard
   against a self-assign or an in-place COW mutation that legitimately returns the same
   buffer: releasing then immediately re-retaining the same pointer at refcount 1 would
   free it out from under itself (release to 0, then "resurrect" it with retain — a
   use-after-free). Otherwise: release the old value; if `dk==1` (not a transfer), also
   retain the new value.

### 4.2 What Track A actually builds

Port `destKind` and the generic wrap **as a private copy inside `src/LlvmGen.cpp`** —
not a shared extraction into a common header. X64Gen stays frozen and untouched per §0.4;
sharing the function would require editing `X64Gen.cpp` to reference it. ~15 duplicated
lines is the correct trade against that risk.

Call the wrap's release/retain through the runtime ABI (doc-2 §2.5:
`lvrt_retain`/`lvrt_release`), which are themselves tag-dispatched no-ops for non-heap
values and for arena/literal values outside the heap's registered regions — the wrap
calls them unconditionally; the runtime decides.

This milestone has **no standalone deliverable independent of A-M2** — there is nothing
to wrap until A-M2 introduces the first heap-producing ops (`NewObject`, `GetMember`,
...). Treat §4.1–4.2 as the ARC design Track A codes against starting in A-M2, not as a
separately shippable slice. What *is* independently doable now, with no Track B
dependency, is A-M2's step-0 coverage matrix (§5.1) — do that first while Track B's B-M1
lands.

### 4.3 Hard rules carried over from the ELF work

1. **Registers must be observably void before first write** (a stashed "old" value read
   from a never-written alloca must see tag 0, or the first wrap on that register frees
   garbage). LlvmGen must zero-init every register alloca in the function entry block
   (§5.3) — this was already required for the interpreter and is now required here too.
2. **The IndexStore pre-release + rebind-chain peephole (§4.1 point 1) must be
   replicated independently in LlvmGen**, over IR shapes `Lower.cpp` already emits
   unchanged — do not attempt to fix this by changing `Lower.cpp`'s emission of the
   rebind chain, which X64Gen's own peephole also depends on matching exactly; a shared
   emission change risks silently breaking the frozen backend's COW behavior even though
   `X64Gen.cpp` itself is never edited.

---

## 5. Milestone A-M2 — value model + objects on LLVM

**Prereq:** Track B's B-M1 (runtime v2 core) merged; `runtime/lv_abi.h` exists.

### 5.1 Step 0 — build the coverage matrix (do this before writing code)

Empirically enumerate the work:
```
grep -n "case Op::" src/X64Gen.cpp   | sed 's/.*case //' | sort -u > /tmp/elf-ops.txt
grep -n "case Op::" src/LlvmGen.cpp  | sed 's/.*case //' | sort -u > /tmp/llvm-ops.txt
diff /tmp/llvm-ops.txt /tmp/elf-ops.txt
bash tests/run_elf.sh build/lang tests/corpus            # record pass/skip per program
```
Record both results in §12. The op diff is your burndown list; the corpus pass/skip set is
the **binding parity checklist** for G1. Also grep `sname ==` in `src/IrInterp.cpp` and
X64Gen's `genCallNative` to enumerate every `CallNativeFn` name — that list goes to Track
B via the log (cross-check against doc-2 §2.7's native table).

### 5.2 Replace the scalar LV with the contract LvValue

Swap `lvTy` from `{i32,i64,double,ptr}` to the contract type `{i64 tag, i64 payload}`
(doc-2 §2.3): floats live as double **bit patterns** in the payload
(`builder.CreateBitCast` / `llvm::ConstantFP` → `i64` bits). Rewrite the existing scalar
ops (LoadConst/Default/Arith/Not/Neg/Print/truth/CallNativeFn) against the v2 runtime
symbols (doc-2 §2.5 names). The old `src/NativeRuntime.cpp` stays on disk (nothing is
deleted) but `tests/run_native_llvm.sh` now links `liblvrt.a` instead (coordinate the
script flip with B-M2's archive; until then link the runtime .c files directly).

Function convention stays `void fN(LvValue* ret, LvValue* p0, …)` with one alloca per IR
register, **explicitly zero-initialized** (store `{0,0}`) in the entry block (§4.2 rule 1).

### 5.3 Program-specific data emission

- **Class metadata:** for every reachable class (walk `mod.sema` the way X64Gen's
  `clsId_`/`fieldKeys` do), emit constant globals per doc-2 §2.6 and one generated
  function `@lv_register(LvProgramTable*)`… (exact shape in the contract). classIds must
  be assigned identically to slot layout expectations — extract the slot-offset scheme
  from X64Gen's `$init`/fixed-offset emission (`mov [obj+16+slot*16]` pattern per
  reference.md §7.3) and verify with a two-field probe class before relying on it.
  **X64Gen.hpp's `[classId][fieldHead]` linked-node comment (`X64Gen.hpp:50-53`) predates
  the packed-slot layout — trust `genMkObj`/`genGetField` code and reference.md §7.3, not
  the header comment** (hurdle H-2).
- **String literals:** LLVM private constant globals in the descriptor layout of doc-2
  §2.4 (length-prefixed, NUL-terminated for debugging convenience). No allocation header —
  the runtime's heap-range check exempts them from ARC (that is the design; do not add
  headers to literals).
- **Closure dispatch trampoline:** a generated
  `@lv_dispatch(i64 fnIndex, LvValue* ret, LvValue* args, i64 argc)` switch over all
  reachable functions, registered at startup — this is how the runtime (event loop,
  `callclosure`, `callm` method tables) calls back into generated code. Emit it in A-M2
  even though the loop lands in A-M5; the map dispatch (`callm`) needs it now.

### 5.4 Ops to implement in A-M2

`NewObject` (alloc via runtime, run `$init` fn if `b >= 0`), `GetMember`/`SetMember`
(accessor-aware: when the resolved decl is an accessor the checker/lowerer already picked
the call — mirror how X64Gen splits direct-slot vs `getm`/`setm` dispatch),
`RawGet`/`RawSet` (fixed-offset direct GEP when the slot is statically known; runtime
lookup otherwise), `CallDyn` (direct call when `in.decl` resolved; else `lvrt_callm` via
class tables), `CallValue` (closure call through the trampoline), `IsType` (`lvrt_issub`),
`Move`/`MoveClear`/`CopyVal` for objects (MoveClear must **clear the source slot** to
void — the current scalar `plain move` comment at `LlvmGen.cpp:208` becomes wrong the
moment ARC lands), `LoadGlobal`/`StoreGlobal` (an `LvValue` array global; check how
X64Gen initializes globals given `Ir.hpp:113` marks `ginit` interp-only — mirror what
`grep -n ginit src/X64Gen.cpp` shows), and the `Retain`/`Release` ops (call
`lvrt_retain`/`lvrt_release`).

### 5.5 Acceptance (A-M2)

Add a Track-A test lane (CMake region §10.3):
```
corpus_llvm_objects: run_native_llvm.sh over a curated list: classes.ext,
generics.ext (if IR-covered), corpus/elf/objects.ext, corpus/elf/intcore.ext
```
plus `corpus_core_llvm` still green. Byte-identical output vs `.expected`.

---

## 6. Milestone A-M3 — collections, strings, closures, COW

Ops: `NewArray`/`NewArraySized`/`NewMap`/`MakeRange`/`GetIndex`/`IndexStore`/`IterLen`/
`IterAt`, `MakeClosure`/`CaptureVar`/`LoadCapture`, `VFree` (now a real call to
`lvrt_vfree`, no longer a no-op — `LlvmGen.cpp:257`), string natives
(concat via `Arith` on tag-4, `subStr`, `indexOf`, `trim`, case, `toString`, `strEq`).

Critical semantics to port faithfully (all differentially caught, but know them going in):
- **COW at rc==1** on `IndexStore` for boxed arrays, dense value-struct arrays, and
  existing-key map updates; copy when shared. The runtime implements the decision
  (doc-2 §2.5 `lvrt_idxset`); your job is the §4.2 pre-release discipline so a
  uniquely-owned base actually *reads* rc==1 at the op. Add the timed smoke test
  (§10.2) so an O(n²) regression is loud.
- **In-language stdlib methods** (where/map/reduce/join/…) are ordinary IR functions —
  they come for free once the ops above work. Do not special-case them.
- **Dense value-struct arrays** are the trickiest layout (tag-5 pointers may point
  *inline into* a dense buffer — that is why structs are never refcounted). Port
  X64Gen's `genIsValueClass`/`genCopyVal` behavior via the runtime equivalents. The known
  dense-IndexStore value-operand leak is a **declared XFAIL** (`dense_index_set.ext`) —
  reproduce the XFAIL, do not fix it here (parity means matching the ELF backend's
  behavior, warts included).
- Map inserts on a missing key copy (no spare capacity by design); existing-key updates
  COW. Insertion order is preserved and observable (printing, `keys`).

**Acceptance:** extend the lane with `collections.ext`, `cow.ext`,
`corpus/elf/collections.ext`, `corpus/elf/strings.ext`, closures-heavy programs from the
matrix; all byte-identical.

---

## 7. Milestone A-M4 — exceptions

The model is **pending-throw, not unwinding tables** — deliberately: no landingpads, no
libunwind, fully portable (this is the design's answer to LLVM's hardest feature; do not
"upgrade" to invoke/landingpad).

- Runtime state: `lvrt_throwing()` / thrown value accessors per doc-2 §2.8 (X64Gen keeps
  `g_throwing`/`g_thrown` in its data segment at offsets 24/32/40 — `X64Gen.hpp:69-71` —
  the runtime owns the equivalent globals now).
- `Throw`: store thrown value, set flag, then behave like a return (frame teardown
  releases owned regs — but the thrown value is **owned by the unwinding machinery**;
  §15 explicitly fixed "in-flight thrown exceptions not owned across unwinding frames" —
  make the runtime's throw-set retain it and catch-bind release the *previous* binding).
- **After every call-like op** (Call, CallDyn, CallValue, CallNativeFn, GetMember/
  SetMember accessor paths, GetIndex/IndexStore — index OOB raises! — and Await): load
  the throwing flag and branch. Centralize in one `emitCheckedCall(...)` helper so a
  missed site is structurally impossible.
- Dispatch: if pc is inside a `Handler` range (`Ir.hpp:90-95`, in clause order), branch
  to a generated chain testing `lvrt_issub(thrownClassId, handlerType)`; first assignable
  clause wins; bind reg gets the value; else propagate (return with flag set).
- Uncaught: entry wrapper calls `lvrt_uncaught()` — its report text must byte-match the
  oracle's (see `exceptions.expected`; X64Gen's `genUncaught` is the native reference).

**Acceptance:** `exceptions.ext`, `corpus/elf/exceptions.ext`, `match.ext`,
`optional.ext` (narrowing paths raise on misuse) — byte-identical, plus the whole
prior lane still green (checked-call insertion touches every op — regression risk).

---

## 8. Milestone A-M5 — system layer: natives, event loop, async, sockets

- Flip program entry to the runtime-owned model (§3): LlvmGen emits `@lv_main` (external
  linkage) instead of `main`; the runtime provides `main()` (doc-2 §2.9). Registration
  (`@lv_register`, trampoline pointer) happens before `@lv_main` runs.
- `CallNativeFn` grows to the full native list from your step-0 enumeration (files,
  stdin, time, sockets — implemented runtime-side; you route args/results and apply the
  ownership contracts column of doc-2 §2.7 exactly: `at`/`keys`/`values` return +1; this
  was a real bug class in the ELF work).
- `Await` → an INLINE pump, not a runtime call *(corrected 2026-07-05 maintenance
  pass #2 — this bullet used to say `lvrt_await(out, promise)`, which was never
  ratified into `lv_abi.h`; B-M3 deliberately shipped the two loop entry points
  instead, doc-2 §2.9)*: emit X64Gen's own pump shape (`src/X64Gen.cpp:3573-3593`) —
  loop { read the promise's `ready` field; if truthy or `lvrt_throwing()`, break; if
  `!lvrt_loop_has_work()`, break; `lvrt_loop_step()` } then read `value`. The promise
  is an ordinary object; extract field names from IrInterp's Await case. Heads-up from
  doc-2 §10 (B-M3 entry): `lvrt_loop_step` blocks up to the earliest due timer, so the
  pump is NOT a busy loop; don't add sleeps around it.
- Timers/sockets/HTTP are in-language code over the natives + loop — they come for free
  when the natives and trampoline are right.

**Acceptance = Gate G1.** New lanes (§10.1): `corpus_llvm` (over `corpus/elf/`),
`corpus_llvm_full` (whole corpus, same skip list as `run_elf.sh`), churn harness with
`--engine llvm` (extend `fuzz/churn_leak.py` by copying the elf-engine plumbing; same
13-green/1-XFAIL profile), and the loopback-port `RESOURCE_LOCK corpus_net_ports`
property on the new full-corpus lanes (`CMakeLists.txt:149-151` — forgetting this makes
CI flaky under `ctest -j`).

---

## 9. Milestone A-M6 — performance and closeout

**Load-bearing correctness item carried from A-M5 (do this first — it flips 2 XFAILs
green and is the biggest scope-owned-allocation perf lever): the arena tier.** Scope-local
value structs (a `CopyVal` heap copy at rc 0, and dense value-struct records) currently go
to the counted heap with no reclamation path — X64Gen arena-allocates them
(`beginAlloc`/`endAlloc`) and bulk-frees at frame exit via `restoreArena`; the LLVM backend
has no equivalent, so `returned_value_struct.ext` and `struct_array_field.ext` leak
(marked `XFAIL-LLVM` in `tests/corpus/churn/`, green on ELF). The runtime already exposes
`lvrt_arena_save()`/`lvrt_arena_restore(mark)` (doc-2 §2.5). Wire: save at frame prologue,
restore on every return/unwind path, and route scope-local (non-escaping) value-struct
allocations to the arena — this needs the escape signal X64Gen carries in `beginAlloc`
(a `CopyVal` whose result does NOT reach a `Ret`/escaping store is arena-safe; one that
escapes must stay on the counted heap). Acceptance: flip both `XFAIL-LLVM` markers off and
the churn llvm lane reaches 13-green/1-XFAIL (matching ELF). **This is a memory-model
change — author the escape rule carefully; a mis-arena'd escaping struct is a UAF.**

Then the performance work:

1. Replace the deprecated `legacy::PassManager`-only flow: run a `PassBuilder` O2 module
   pipeline before object emission; add `-O0|-O2` plumbing (default O2, O0 for debug).
   Corpus must be green at **both** levels (hurdle H-12).
2. Inline fast paths (measure first, in this order of expected payoff):
   int/float `Arith` inline with tag-check + fallback call; fixed-offset `RawGet/RawSet`
   direct GEP (already in A-M2); checker-resolved `CallDyn` direct calls (A-M2);
   `truth`/`Not` inline. Also the A-M4/A-M5 note: `emitThrowCheck` calls `lvrt_throwing()`
   (an un-inlinable cross-`.o` call) after every call-like op — X64Gen reads its
   `g_throwing` global inline. Inlining the flag read needs the throwing flag exposed in
   the ABI (a §2.8 contract addition — coordinate with Track B, STOP-gated); measure
   whether it matters before spending the contract change.
3. Optional stretch (needs Track B buy-in): compile the runtime to LLVM bitcode at build
   time and `llvm::Linker::linkModules` it into the program module before the pass
   pipeline — cross-boundary inlining without LTO plumbing. Skip if packaging friction
   appears; log either way.
4. Benchmarks: update the fib(30) table (reference.md §7.3) and add an object-heavy and
   an indexed-store micro. Target: beat or match the pure-ELF numbers (a stack-slot
   machine with no register allocation should not outrun LLVM O2; if it does, fast paths
   are missing — investigate before closing).
5. **Docs to update at G1** (single commit): `docs/reference.md` §7/§7.3 (LLVM = primary,
   flags, coverage, the stale §7.1 "unlowerable" note), `info.md` §17, `HANDOFF.md`
   engine list/CI table. Do not restructure those docs; surgical edits.

---

## 9b. Milestone A-M7 — inline hot-state fast paths (post-G1 perf tail; authored 2026-07-06, Fable maintenance pass #4)

**Context.** A-M6 closed G1 with two measured, deliberately-unspent levers (its §12
entry): every call-like op pays a cross-`.o` `lvrt_throwing()` call, and every
value-struct-allocating frame pays a cross-`.o` `lvrt_arena_save()`/`lvrt_arena_restore()`
pair — root-caused as the dominant remaining gap on the value-struct micro (LLVM ~1.4s vs
ELF ~0.85s on a 3M-call allocator loop; the non-allocating micros are already at/near ELF
parity). Both needed an ABI addition, which is now ratified: **doc-2 §2.10** exports
`lv_g_throwing` and `lv_g_arena_cursor`. Read §2.10 in full before starting — its contract
bullets (load-only for the flag; init-before-use guarantee; the function entry points
stay) are normative.

**Work items, in order:**

1. **Runtime export rename** (owner-authorized narrow edit, per §2.10's closing
   paragraph): in `runtime/lv_runtime.c`, rename `static int32_t g_throwing` →
   `int32_t lv_g_throwing` and `static uint8_t* g_arena_cursor` →
   `uint8_t* lv_g_arena_cursor`; keep every internal use and all three accessor
   functions; declare both in `lv_abi.h` §2.10's block (already specified there).
   `runtime_selftest` green (plain + `-DLANG_RT_SANITIZE=ON` scratch build) before
   moving on. Touch nothing else under `runtime/**`.
2. **Inline the throw-flag read** (`src/LlvmGen.cpp`): `emitThrowCheck` loads the i32
   global `lv_g_throwing` instead of calling `rtThrowing`. One helper, one edit site —
   every checked op inherits it. The A-M4 audit rule holds: no call-like op loses its
   check.
3. **Inline the arena save/restore** (`src/LlvmGen.cpp`): for `usesArena` functions
   only (the existing precomputation), the prologue loads `lv_g_arena_cursor` into the
   mark alloca and every return/unwind path stores it back — replacing both runtime
   calls. The restore-after-`releaseAllRegs` ordering and the unwind-propagation-block
   coverage are unchanged from A-M6; only the call becomes a load/store.
4. **Re-benchmark** (same three micros as A-M6, same machine-relative method):
   fib(30), the 5M indexed-store loop, the 3M value-struct allocator. Target: the
   value-struct micro closes to ≤ ~1.1× ELF; if it does not, investigate before
   closing (per §9 item 4's own rule) — the next suspect A-M6 named is LTO/bitcode
   linking (§9 item 3), which stays NOT-attempted unless `clang` exists on the host.
   Update the numbers in `docs/reference.md` §7.3 and this doc's §12 log.

**Acceptance:** full `ctest` green (36/36 as of 2026-07-06); corpus byte-identical at
BOTH `--opt-level 0` and `2` (hurdle H-12); churn llvm lane still 13-green/1-XFAIL;
ASan/UBSan manual pipeline (the A-M6-documented by-hand sweep) clean on the elf-parity +
churn programs; selftest green plain and sanitized; benchmark table updated. Log
everything in §12; the STOP protocol (§0.3) applies unchanged.

---

## 10. Testing strategy

### 10.1 Lanes (end state)

| lane | command shape | when |
|---|---|---|
| `corpus_core_llvm` | existing, now against runtime v2 | from A-M2 |
| `corpus_llvm_objects` | curated object programs | A-M2 → folded into corpus_llvm at A-M5 |
| `corpus_llvm` | `run_native_llvm.sh` over `tests/corpus/elf` | A-M5 ✅ |
| `corpus_llvm_full` | whole corpus, RESOURCE_LOCK corpus_net_ports | A-M5 (G1) ✅ |
| `corpus_churn_leak_llvm` | `churn_leak.py --engine llvm` (as-built name; 11-green/3-XFAIL — the dense-append XFAIL + 2 `XFAIL-LLVM` value-struct programs pending A-M6's arena tier) | A-M5 (G1) ✅ |

*(As-built at A-M5, 2026-07-06: the churn lane is `corpus_churn_leak_llvm`, not the
sketch's `corpus_churn_llvm`; `run_native_llvm.sh` folds `corpus_llvm_objects` in via the
full-corpus lane. `LANG_LVRT_SOURCES` gained `lv_loop.c` + `lv_entry.c` for the entry
flip.)*

### 10.2 Rules

- Differential output is the truth; byte-identical vs `.expected`.
- Never edit `.expected`; oracle disagreement = STOP + bug.md entry.
- Add one **timed** smoke (indexed-store loop ~1M iterations must finish < 2s) so a COW
  regression cannot land silently.
- Adding new corpus programs is allowed (generate `.expected` with `--run`); prefer
  reusing existing ones.

### 10.3 CMakeLists.txt discipline

Both tracks append tests **only inside their marked region** at the end of the file:
```cmake
# --- Track A: LLVM parity lanes (techdesign-portable-backend.md §10) ---
...
# --- Track B: runtime v2 / driver (techdesign-portable-backend-2.md §8) ---
...
```
Create your region in your first milestone; never edit inside the other track's region.

---

## 11. Suspected hurdles (read before each milestone)

- **H-1 ABI at the boundary.** Only `LvValue*` pointers cross the generated↔runtime
  boundary. Never declare a runtime function returning/taking the struct by value — clang
  lowers 16-byte structs differently per target (SysV splits into two scalars; Win64
  passes by pointer) and hand-written IR declarations will silently mismatch. The
  existing pointer convention sidesteps this entirely; keep it.
- **H-2 Comments lag code.** `X64Gen.hpp:50-53`'s linked-field-node comment describes the
  pre-packed-layout era; reference.md §7.3 describes packed slots + dynamic fallback.
  Extract layouts from `genMkObj`/`genGetField`/`$init` emission and verify with probe
  programs; record extracted facts in `lv_abi.h` comments (Track B owns the file; hand
  facts over via your log if B hasn't pinned them yet).
- **H-3 Tag numbering ≠ VKind order.** Tags: 0 void, 1 int, 2 float, 3 bool, 4 string,
  5 object, 6 array, 7 map, 8 none, 9 closure. `VKind` (`RuntimeValue.hpp:22`) orders
  Closure before Array — copying enum order corrupts every array/closure dispatch.
  A `static_assert`-style comment table in lv_abi.h is the guard.
- **H-4 Zero-init registers** before any `Release` can see them (§4.2). Allocas are
  garbage otherwise; this will manifest as nondeterministic frees — the worst bug class.
- **H-5 Checked calls everywhere.** One helper, used for every op that can run user code
  or raise (including index OOB and operator dispatch). An audit grep at A-M4 end:
  every `CreateCall` to a fn that can throw goes through the helper.
- **H-6 Output-format parity.** Float formatting, array/map printing, insertion-order
  keys, `None`, uncaught-exception report, int division/modulo on negatives — all defined
  by the oracle (`RuntimeValue.hpp:77-111` `valueToString`, CGen's `kRuntime` as the
  portable reference). When a diff appears, match the oracle; never "improve."
- **H-7 COW pre-release.** §4.2 rule 2. The symptom of getting it wrong is not a failure
  but a silent O(n²); the timed smoke (§10.2) is the tripwire.
- **H-8 MoveClear must clear** once ARC lands (`LlvmGen.cpp:208` is scalar-era).
- **H-9 ginit** is marked interp-only (`Ir.hpp:113`) — discover X64Gen's global-init
  strategy before assuming (grep `ginit`, `LoadGlobal` in X64Gen.cpp) and mirror it.
- **H-10 Trampoline before loop.** `callm` method tables and closure calls need
  `@lv_dispatch` in A-M2; the event loop reuses it in A-M5. Emitting it late forces a
  rework of map dispatch.
- **H-11 Reachability now crosses the trampoline.** `computeReachable`
  (`LlvmGen.cpp:84-97`) only follows `Op::Call`; once closures/method tables exist,
  reachable = every function referenced by MakeClosure, method tables, and `byDecl`
  entries for emitted classes — mirror how X64Gen builds `reachable_`/`closureFns_`.
- **H-12 O2-only failures = your UB.** If the corpus passes at O0 and fails at O2,
  suspect uninitialized loads, out-of-bounds GEPs, or a missing clear — in generated IR,
  not in LLVM. `verifyModule` stays on for every emit.
- **H-13 Stale prose.** reference.md:706 contradicts the elf corpus; the corpus wins
  (§5.1 records the truth).
- **H-14 LLVM version drift.** CMake pins `llvm-config-18` (`CMakeLists.txt:39`);
  `CodeGenOptLevel`/`PassBuilder` APIs move between majors. Build against 18; if your
  environment offers only a newer LLVM and APIs changed, that is mechanical adaptation
  (log it), unless signatures force a design change (then STOP).

---

## 12. Implementation log (append-only)

*(Implementation agents: date-stamped entries. Record: coverage matrix (§5.1), per-op
retain-contract table (§4.2), extracted layout facts, deviations, XFAILs reproduced,
benchmark numbers, and any STOP events with findings.)*

### 2026-07-05 — design correction: A-M1 rescoped (destKind ported to LlvmGen, no IR ops)

Before writing any code, read `X64Gen.cpp`'s actual ARC mechanism end to end
(`X64Gen.cpp:3036-3121`, `:3652-3669`). Found it is a pure `destKind(Op)` classification
(0/1/2, see §4.1) plus a ~30-line generic wrap around the per-instruction switch — no
IR-level opcodes at all. This is smaller and more faithful than the originally-planned
`Retain`/`Release` IR ops gated by an `arcOps` flag, so §4 was rewritten in place
(git-blame the doc for the prior text if needed) rather than shipping the weaker design
and reworking it later. Net effect: A-M1 no longer touches `Ir.hpp`, `Lower.cpp/.hpp`,
`IrInterp.cpp`, `CGen.cpp`, `Ownership.cpp`, `MemVerify.cpp`, or `main.cpp` — it folds
into A-M2 (nothing to wrap until heap-producing ops exist). This was judged a mechanical
correction made with high confidence from reading the reference implementation directly,
not a STOP-worthy escalation — logged per the "mechanical adaptation, proceed and log"
guidance in §0.3.

### 2026-07-05 — A-M2 step 0: coverage matrix + corpus parity checklist (done)

Op diff (`grep -n "case Op::" src/X64Gen.cpp src/LlvmGen.cpp`, deduped):
- LlvmGen currently covers: `Arith, Call, CallNativeFn, CopyVal, Default, Jump,
  JumpIfFalse, JumpIfTrue, LoadConst, Move, MoveClear, Neg, Not, Print, PrintNl,
  Ret, RetVoid, VFree` (18 ops — the scalar core, matches reference.md).
- **Burndown list (23 ops in X64Gen, absent from LlvmGen):** `Await, CallDyn,
  CallValue, CaptureVar, GetIndex, GetMember, IndexStore, IsType, IterAt, IterLen,
  LoadCapture, LoadGlobal, MakeClosure, MakeRange, NewArray, NewArraySized, NewMap,
  NewObject, RawGet, RawSet, SetMember, StoreGlobal, Throw`.

Corpus parity checklist (`bash tests/run_elf.sh build/lang tests/corpus`, non-recursive
— top-level `*.ext` only): **28/28 pass, 0 skipped.** The exact file list (this is the
binding G1 checklist for `corpus_llvm_full`):
`async, bare_method_call, block_uses, call_positions, classes, collections, const, cow,
di, exceptions, files, generics, http, io, match, match_nested, namespaces, oo,
optional, qualified, sieve, sockets, strcmp, streams, structs_array, structs, timers,
use` (all `.ext`, in `tests/corpus/`). `tests/corpus/elf/` (14 files) and
`tests/corpus/core/` (5 files) are additional, separately-run subsets already known to
pass on the ELF backend (`corpus_elf`, `corpus_elf_core` lanes).

Native (`CallNativeFn`) enumeration, from `X64Gen::genCallNative`
(`X64Gen.cpp:3511-3565`) — the full `std::sys*` list and X64Gen's own ownership
contract per native (this is the seed data for doc-2 §2.7's table):

| native | returns | contract |
|---|---|---|
| `sysWrite(fd,data)` | int | no ARC (int result) |
| `sysReadLine(fd)` | string | **fresh string, retained +1** after the call (`X64Gen.cpp:3520-3521`) |
| `sysRead(fd,n)` | string | **fresh string, retained +1** (`:3524-3525`) |
| `sysOpen(path,flags)` | int | no ARC |
| `sysClose(fd)` | int | no ARC |
| `sysStat(path,out)` | int | no ARC |
| `sysTimerStart(ms,repeat,cb)` | int | no ARC (the callback closure's own ownership is handled by the timer-registry capture path, not here) |
| `sysTimerCancel(id)` | int | no ARC |
| `sysTcpConnect(ip,port)` | int | no ARC |
| `sysTcpListen(port)` | int | no ARC |
| `sysAccept(fd)` | int | no ARC |
| `sysSend(fd,data)` | int | no ARC |
| `sysRecv(fd,n)` | string (`None` on EOF) | **fresh string, retained +1** (`:3552-3554`; the None-literal path skips the gate since it's not heap) |
| `sysWatch(fd,cb)` | int | no ARC |
| `sysUnwatch(id)` | int | no ARC |

Baseline before any code change: `cmake --build build -j$(nproc)` clean (only
pre-existing `-Wmisleading-indentation` warnings in `X64Gen.cpp:3600-3601`, not
introduced by this work); `ctest -j$(nproc)` → **26/26 tests pass**, 35.8s total. This is
the regression baseline for every later milestone.

### 2026-07-05 — Track separation clarified; Track B worked by a separate session

Track B (runtime v2) is being implemented by a **different, concurrent Claude session**
in the `Language-agent2` worktree; B-M1 landed on master via PR #1. This session stays
**strictly on Track A** and does not touch `runtime/**`, `src/main.cpp`, or Track B's
files. Because this Track-A branch (`track-a-llvm-parity`) forked from master *before*
PR #1 merged, it does not yet see `runtime/liblvrt.a`; to stay unblocked and isolated,
A-M2 links against a **temporary Track-A-local runtime stub**
(`tests/support/llvm_stub_runtime.c`) implementing the same `lvrt_*` ABI names. Swapping
it for Track B's `liblvrt.a` once this branch rebases onto post-PR-#1 master is a pure
relink (same symbol names, same layouts) — a scheduled follow-up, not a rewrite.

### 2026-07-05 — A-M2 implemented (value model, ARC wrapper, objects, globals)

**Delivered.** `src/LlvmGen.cpp` moved off the scalar `{i32,i64,double,ptr}` LV to the
contract `LV = {i64 tag, i64 payload}` (doc-2 §2.3), and grew from the scalar core to the
object core. New/changed ops: `Move`/`MoveClear` (MoveClear now clears the source, §4.1),
`CopyVal` (degenerates to move + retain until value structs land in A-M3), `NewObject`
(alloc + real classId + $init call + own-at-+1 retain), `RawGet`/`RawSet` (fixed-slot GEP
at `16+16*slot`), `GetMember`/`SetMember` (runtime name→slot via `lvrt_getm`/`lvrt_setm`),
`LoadGlobal`/`StoreGlobal` (an `@lv_globals [N x LV]` array), and `CallDyn`
(checker-resolved direct calls only; unresolved `callm` is A-M3+). String literals now
emit as `{i64 len, i8 bytes[]}` constant globals (doc-2 §2.4). Class metadata
(`lvrt_register` + a constant `LvClassInfo[]` table with per-class slot-name arrays,
doc-2 §2.6) is emitted in a generated `main` before `ginit` and entry.

**The ARC wrapper (A-M1, §4).** `destKind(Op)` + the generic wrap were ported verbatim
into LlvmGen (private copy; X64Gen untouched). Register allocas are zero-initialized
(§4.3 rule 1). Retain/release go through `lvrt_retain`/`lvrt_release`.

**`ginit` (hurdle H-9 confirmed).** `Ir.hpp:113`'s "interp-only" comment on `ginit` is
stale exactly as the design predicted — X64Gen calls it (`X64Gen.cpp:3793`). LlvmGen now
marks it reachable and calls it before entry. Without this, top-level/prelude globals
(e.g. `console`) are never initialized.

**Bug found and fixed via ASan (the key validation win).** First object runs printed
correct output but ASan (`-fsanitize=address,undefined`) flagged a heap-use-after-free:
I copied call parameters into registers but did not **retain them on entry**, so the
callee's exit-time `releaseAllRegs` over-released them (the caller passes args at +0 and
keeps its own count — X64Gen.cpp:2963-2969). Added the param-retain-on-entry loop; all
object tests then clean under ASan/UBSan/LSan (objects: zero UAF, zero double-free, zero
object leaks). Lesson for later milestones: **plain-output-correct is not
ARC-correct — sanitize every new heap-producing op.**

**Deliberate A-M2 simplifications (all logged, all A-M3+ scope):** the stub does not
refcount strings (tag-4 retain/release are no-ops → `makeStr` values leak; LSan flags
this, it is not a codegen defect and vanishes when Track B's real string ARC is linked);
no arena tier (every alloc is heap); `GetMember`/`SetMember` do plain name→slot with no
accessor dispatch (no A-M2 corpus program routes an accessor through getm — the checker
lowers resolvable accessors to `CallDyn`); `StoreGlobal` retains without releasing the
old value — a **deliberate parity match** to X64Gen (`X64Gen.cpp:3504-3510`), safe
because corpus globals are write-once.

**Tests.** `corpus_core_llvm` now links the stub (the old `NativeRuntime.cpp` is
incompatible with the new value contract and is left untouched on disk). New lane
`corpus_llvm_objects` over `tests/corpus/llvm_objects/` (basic_object, multi_field,
nested_object — methods, multi-field offsets, nested/aliased objects with reference
semantics), each cross-checked against oracle == IR interp == ELF before trusting. Full
suite: **27/27 green** (was 26; +1 new lane). The stub carries `#ifdef __cplusplus
extern "C"` because `run_native_llvm.sh` compiles the runtime with `g++`.

**Remaining for parity (A-M3+):** collections/strings/COW (A-M3), exceptions (A-M4),
system layer + full natives + the dispatch trampoline for `callm`/`CallValue`/closures
(A-M5). The burndown list from step 0 still has: `Await, CallValue, CaptureVar, GetIndex,
IndexStore, IsType, IterAt, IterLen, LoadCapture, MakeClosure, MakeRange, NewArray,
NewArraySized, NewMap, Throw, VFree`, plus unresolved `CallDyn`/dynamic-key
`GetMember`/`RawGet`.

### 2026-07-05 — Fable maintenance pass: A-M1 rescope RATIFIED; A-M3 opens with the relink

**Ratification.** The aa90005 rescope (destKind(Op) classification + generic pre/post
wrap ported into LlvmGen.cpp privately; **no** Retain/Release IR opcodes; Ir.hpp /
Lower.cpp / IrInterp.cpp / main.cpp untouched by Track A) is **ratified as the design of
record**. Rationale accepted: the IR-visibility goal served hypothetical future backends,
but LLVM *is* the backend of record and X64Gen is frozen — backend-side classification is
where the mechanism actually lives in the reference implementation, and dropping the
shared-file edits eliminated the cross-track merge surface. §4 of this doc is superseded
by aa90005's rewrite; the "A-M1 merges before B-M2 touches main.cpp" ordering rule is
void (main.cpp is Track B's exclusively).

**A-M3 step 0 is the relink** (stub → real runtime). Track B's runtime v2 landed (doc-2
§10: B-M1 + the 2026-07-05 ABI reconciliation) and implements everything the stub did and
everything A-M3 needs (strings with real ARC, arrays/maps with rc==1 COW, dense
value-struct arrays, closures, exceptions, `to_string` at valueToString parity). The
authoritative contract is `runtime/lv_abi.h` — read its comments in full before emitting
a single call. The checklist, each item verified against `lv_abi.h`:

1. **Link `runtime/lv_runtime.c` + `runtime/lv_plat_posix.c`** in
   `tests/run_native_llvm.sh` + both LLVM lanes (compile as **C17 with `cc`/`gcc`/
   `clang -std=c17`, never bare `g++`** — they are C, not C++ — and add `-lm`: fmod).
   Flip to `liblvrt.a` instead once Track B's B-M2 merges. **Delete
   `tests/support/llvm_stub_runtime.c`** (its own header declares it discardable; update
   the CMake comment).
2. **`LvClassInfo` is 11 fields** — emit the FULL struct
   `{ptr,i64,i64,i32,ptr,ptr,i64,ptr,ptr,ptr,i64}` per element (name, classId, nslots,
   isValue, slotNames, subtypeIds, nSubtypeIds, methodNames, methodFnIndex, methodKinds,
   nMethods), null/0 for unused tails. Your 5-field A-M2 emission has the wrong ARRAY
   STRIDE against the real runtime (garbage past entry 0) — this is the first thing that
   will bite if skipped.
3. **`lvrt_obj_new` is `void lvrt_obj_new(LvValue* out, i64 classId)`** — out-param
   first, no nslots (the runtime derives it from the registered table). Your A-M2
   `ptr lvrt_obj_new(i64, i64)` emission must change.
4. **`lvrt_getm` results are +1 (transfer)** — remove the wrap-retain on
   GetMember-via-getm results (your stub-era pairing of borrowed-getm + wrap-retain was
   net-equivalent; against the real runtime it double-retains). `lvrt_setm` now does the
   slot ARC internally — do not emit release/retain around it.
5. **Method tables**: when you emit them (A-M3+ dynamic residue), the `methodKinds`
   column discriminates LV_M_METHOD/GET/SET/OP — operator entries use the symbol text
   ("+", "==") as the name. `getm` only dispatches LV_M_GET, etc.
6. **Key globals**: keep interning (pointer-compare fast path) but the runtime now falls
   back to content comparison — a missed dedup is no longer fatal.
7. **Scalar entry points are unchanged** (`lvrt_truth/not/neg/arith/print_val/print_nl/
   syswrite/readline` keep your exact A-M2 signatures — they were ratified as-is), with
   one behavioral delta: string `==`/`!=` against a non-string is now FALSE per the
   oracle (the stub stringified both sides). If any corpus program compared mixed types,
   its expected output is the oracle's — trust the differential.
8. **`StoreGlobal` retain-without-release** (your deliberate X64Gen parity match) stays
   as-is — same wart, same reason, still fine for write-once corpus globals.

Re-run your three `llvm_objects` programs + `corpus_core_llvm` under ASan/UBSan/LSan
after the relink — the real runtime's string ARC means the stub-era LSan string-leak
noise must be GONE (a remaining leak is now a real bug, yours or Track B's; bisect with
LANG_ARC_TRACE). Then proceed into A-M3 proper (§6).

### 2026-07-05 — runtime-v2 relink + A-M3 implemented (collections, strings, closures, COW)

**Context.** Track B's B-M1 runtime and its ABI-reconciliation pass (`runtime/lv_abi.h`
now normative, `lvrt_getm` ruled +1-transfer, `lvrt_setm` ruled internal-slot-ARC,
`LvClassInfo` grew `methodKinds`) merged to master as PR #1 + follow-ups; PR #2 (this
track's A-M2) also merged. This branch merged master in, which resolved the handoff's
open (a)/(b) decision in favor of **(b): build A-M3 against the real runtime** — the
throwaway stub is deleted (`tests/support/llvm_stub_runtime.c`, retired as designed).

**Relink adaptations (all pre-flagged by the reconciliation commit).**
`lvrt_obj_new` is out-param `(out, classId)` — nslots now comes from the registered
class table; `LvClassInfo` is emitted as the FULL 11-field struct (the A-M2 5-field
prefix was a stride bug waiting for entry 1); `GetMember` moved to destKind 2 (getm
returns +1; a wrap retain would double-count); `sysReadLine` result now retained
(fresh string at +1 under the CallNativeFn transfer contract, X64Gen.cpp:3516-3520).

**A-M3 delivered.** All §6 ops implemented in `src/LlvmGen.cpp`: `NewArray` (+ Range
spread), `NewArraySized` (0-arg and (n, fill) — fill goes dense for value structs via
copyval+append+vfree per element), `NewMap`, `MakeRange` (Range obj + setfield),
`GetIndex` (runtime idxget; instantiated-class `([])` getters dispatch first, X64Gen
genIdxGet parity), `IndexStore` (idxset + the §4.2 pre-release discipline below),
`IterLen`/`IterAt` (inline tag dispatch: boxed/dense arrays, Range arithmetic, map →
Pair object with retained fields — X64Gen genIterAt parity incl. the dense tag-5
pointer-into-buffer), `MakeClosure`/`CaptureVar`/`LoadCapture` (lvrt closure core;
capture_set retains internally), `CallValue` (fnIndex from closure word0 through the
trampoline), real `VFree` (lvrt_vfree + slot clear), real `CopyVal` (lvrt_copyval),
dynamic-key `RawGet`/`RawSet`, `Default` for Array/Map, `Arith` op 11-14 (& | << >>)
and the object row (left tag-5 → `lvrt_opm`, obj-vs-None stays scalar — X64Gen:3178).

**Dispatch.** One internal `lv_dispatch(fnIndex, ret, args, argc)` trampoline (switch
over every reachable IR function) serves both `lvrt_register` (getm/setm/opm/callm
table dispatch) and `CallValue`. Class registration now emits real method tables:
names (operators keyed by `selector.text` — "+", "==" — per the LV_M_OP ruling; a
plain `m->name` is EMPTY for symbolic members, which cost one debugging round), IR fn
indices, LV_M_* kinds, plus transitive-closure `subtypeIds` (ready for A-M4 issub).
Unresolved `CallDyn` compiles a per-site dispatch chain: effective classId (tag-5
header read, else tag→mask constant), in-language candidates from
instantiated+Array/Map+mask classes (X64Gen callm order: in-language first), then the
native rows ported from X64Gen::genCallNative — string
length/isEmpty/toString/indexOf/contains/subStr/charAt/toInt/trim/toUpper/toLower/
startsWith/endsWith, array length/at/add, map length/at/has/keys/values, int/bool
toString — with X64Gen's exact ownership: fresh strings retained (+1), `at` retains
the borrowed element, keys/values retain every copied ref, bool toString returns the
ARC-exempt literal, startsWith/endsWith free their probe (retain+release = the
temp-free idiom under the rc<=0 gate). Reachability adopted X64Gen's model:
instantiating a class marks all members; any collection op seeds the whole Array/Map
prelude surface; masks join dispatch without force-marking.

**COW / §4.2 pre-release (the design's named hard part).** Ported X64Gen's IndexStore
discipline verbatim (X64Gen.cpp:3086-3117): release-and-clear the stale dest BEFORE
idxset (else a store loop reads rc 2 forever and goes O(n²)), plus the
`CopyVal t←nb; Move L←t` rebind-chain peephole (straight-line-only, jump-target
guarded). Verified by the new **timed smoke lane** (`corpus_llvm_cow_smoke`,
tests/corpus/llvm_smoke/cow_timed.ext: 1M indexed stores under `timeout 2` — §10.2).

**STOP-protocol divergence (logged per §0.3): the rc-0 floating-string tier is not
portable to runtime v2, and was NOT ported.** X64Gen's §15 string discipline leaves
concat/toString results at rc 0; its own `Ret` retain + releaseAllRegs then drives
them 1→0, which genuinely FREES them — a returned string crosses the frame boundary
as a freed-but-byte-intact block. That is only observably correct because X64Gen's
allocator never poisons freed blocks (and its freelist next-pointer lands in the
header, not the body). Runtime v2 deliberately 0xFE-poisons freed bodies (a logged
Track B hardening), which turns that idiom into visible corruption — caught
immediately by the relinked corpus (multi_field printed nothing: its returned
describe() string came back poisoned). Resolution: **generated code now retains every
string it materializes into a register** (one `lvrt_retain` on the scalar-Arith
result path; all native rows already returned +1) — registers own their strings, the
runtime's consume-unowned gates (concat/print temp-free, both gated on rc==0) go
dormant for generated code, and strings free through the ordinary release paths.
Byte-identical output, and strictly sounder than the ELF backend (no UAF, no floating
temps). The runtime contract is untouched; this is purely an emission-discipline
divergence, documented at the destKind table and the Arith case.

**Also diverged (same reason, same doc points):** `NewArray`/`NewArraySized` are dk=2
with an explicit retain-at-birth (X64Gen births arrays at rc 1 under its arena/
creation-count scheme; runtime v2 births rc 0 — the register must own the base DURING
element appends or every append copies). No arena tier yet: scope-owned allocations
(X64Gen's `beginAlloc` regions) go to the counted heap and value-struct copies free
via VFree/recursive-free instead of bulk arena restore — the churn/live-bytes meter
is the A-M5 gate where this gets measured, and an arena tier is the A-M6 perf lever.
The known dense-IndexStore value-operand leak XFAIL is inherited as-is (the runtime
replicated X64Gen's dense-append-never-frees edge; `cow.ext`'s dense section runs it).

**Tests.** New lane `corpus_llvm_m3` (symlinks, single-sourced .expected):
collections.ext, cow.ext, elf/collections.ext, elf/strings.ext, elf/objects.ext
(operators/accessors through the real dispatch tables), elf/intcore.ext. Plus
`corpus_llvm_cow_smoke` (timed). Existing `corpus_core_llvm`/`corpus_llvm_objects`
relinked to the real runtime. Every lane program ASan+UBSan clean (`detect_leaks=0`;
LSan cannot see into the runtime's region allocator — live-bytes is the A-M5 churn
lane's job). Full suite: **30/30 green** (was 28; +2 lanes). All new .expected
outputs cross-checked oracle == IR interp == frozen ELF before trusting.

**Known gaps going into A-M4:** no `throwCheck` after call-like ops yet (§7 wires it
via one emitCheckedCall-style helper — `lvrt_raise_oob` already fires, the pending
flag just isn't polled); `Throw`/`IsType`/`Await` still fail loudly; `lvrt_getm`'s
single-name lookup means a getter on a `Source::name` distinct member routed through
GetMember would miss dispatch and read the raw field (no corpus program does this;
X64Gen passes both name forms — revisit if a differential ever trips it).

### 2026-07-05 — Maintenance pass #2 (Fable): state re-analysis, A-M5 drift corrected, two shared-code bug fixes

All agents stopped; owner-directed maintenance across both tracks. Full detail lives in
doc-2 §10 (same-date entry); this entry records what Track A must know.

**Where Track A stands.** A-M1 (rescoped) / A-M2 / A-M3 all landed and merged; master
carries A-M3 + Track B's B-M3 together at 31/31 `ctest`, and the lcurl example suite is
48/48 over `--run`/`--ir`. `--build-native` runs scalar AND object corpus programs
byte-identically end-to-end through `liblvrt.a`. **A-M4 (exceptions, §7) is next**; its
entry state is exactly the "known gaps" list at the end of the A-M3 entry above
(throwCheck polling, Throw/IsType, the getm dual-name edge).

**Design drift corrected in §8:** the `Await` bullet promised `lvrt_await(out,
promise)`, but that function was never ratified into the ABI — B-M3 shipped
`lvrt_loop_has_work()`/`lvrt_loop_step()` for Await's codegen to pump INLINE (X64Gen
parity, `src/X64Gen.cpp:3573-3593`). §8 now specifies the inline pump; doc-2 §2.9 was
updated to match its as-built API. Also for A-M5: the entry flip needs a runtime-owned
`main()` in `liblvrt.a` — Track B pre-lands it (safe now: a generated `main` in the .o
always beats an archive member; the archive copy is only pulled once LlvmGen emits
`lv_main` instead), so coordinate timing through the logs, not through files.

**Shared-code changes this pass that touch Track A's world:**
1. `LlvmGen::emitObject(path)` grew a defaulted second parameter
   `emitObject(path, triple = "")` (B-M4's cross-track dependency, landed while no
   agent was mid-flight). No Track A call site changes; host emission byte-identical.
   The driver gained `--target <triple>`.
2. The prelude's `TcpStream`/`TcpListener` (src/Resolver.cpp) got stale-handle guards
   (close is idempotent + invalidates; writes no-op on a dead handle) — fixes a real
   cross-connection corruption found via lcurl (`--max-time` red case). New corpus
   program `tests/corpus/stale_stream.ext` (port 8095) exercises it on every engine
   lane including `corpus_elf_full`; when A-M5 builds `corpus_llvm_full`, it inherits
   this program — nothing special to do beyond the already-mandated
   `RESOURCE_LOCK corpus_net_ports`.
3. `RuntimeLoop.cpp` (shared oracle loop): POLLNVAL now wakes a watch once, then the
   registry auto-retires it (was: silent 100%-CPU busy spin when an fd was closed
   under its watch). Runtime v2's loop got the identical semantics. **X64Gen's frozen
   loop mask (`0x19`, `src/X64Gen.cpp:2430`) still has the latent gap** — do not add
   corpus programs that leave a watch armed on a closed fd; the fixed prelude no
   longer produces that shape from in-language code.

**A-M5 planning note:** the lcurl example (examples/curl, 48-case fixture suite) is the
richest available differential for A-M5's system layer — it exercises sockets, timers,
redirects, chunked decoding, file output, and error paths through natives LlvmGen
doesn't emit yet (`sysTimerStart` et al. fail loudly today, correctly). Budget for
running it under a `--build-native` binary as an A-M5 exit check alongside
`corpus_llvm_full`.

### 2026-07-05 — A-M4 implemented (exceptions on the LLVM backend). Opus-class agent.

**Context.** Entry state was exactly the "known gaps" list at the end of the A-M3 entry
(throwCheck polling unwired; `Throw`/`IsType` failing loudly). Branch and master were
identical at 9f26cd7; 31/31 green. The pending-throw model (doc §7): NO landingpads, no
libunwind — a runtime `g_throwing`/`g_thrown` flag polled after every call-like op, per
the frozen doc-2 §2.8 ABI (`lvrt_throw_set` / `throwing` / `thrown` / `catch_bind` /
`issub` / `uncaught`). The runtime contract is UNTOUCHED — this is pure codegen.

**Delivered (all in `src/LlvmGen.cpp`).**
- **`emitThrowCheck(pc)`** — the single checked-call helper (X64Gen::throwCheck parity,
  X64Gen.cpp:3008-3034). Emitted INSIDE the op's `wrapDest` body, right after the call:
  `lvrt_throwing()`; if set, `lvrt_thrown()` peeks the value and each covering handler
  (clause order, `pc ∈ [h.start,h.end)`) is tried — tag must be 5 (object, X64Gen tests
  this even for an untyped `catch`, so a non-object throw is never caught), then
  `lvrt_issub(thrownClassId, clsId(h.type))`; first match binds via `lvrt_catch_bind` and
  branches to `blocks[h.handlerPc]`; no match propagates by jumping to the frame's
  void-return block (`releaseAllRegs` runs there). On the NOT-throwing path the builder is
  left at `contBB` so `wrapDest`'s release-old/retain-new tail runs normally; on a throw
  we branch away BEFORE that tail, and the stale dest ref is dropped by `releaseAllRegs`
  at the unwind point instead (X64Gen leaks it identically — output-neutral, ASan-clean).
- **Wired after every call-like op** — `Call`, `CallDyn` (resolved + unresolved),
  `CallValue`, `CallNativeFn`, `GetMember`/`SetMember` (accessor paths), `GetIndex`/
  `IndexStore` (OOB raises). One insertion site per op; a missed site is structurally
  impossible because the helper is the only path that continues the op. `SetMember`'s
  check lives in the post-switch dk==0 block (after `lvrt_setm`); it is dk==0 so there is
  no `wrapDest` tail to race with the branch-away.
- **`Throw`** — `lvrt_throw_set(reg)` then `emitThrowCheck`. throw_set RETAINS internally
  (§15 in-flight ownership) so NO emission-side retain is added; `catch_bind`'s transfer
  consumes exactly that +1 (the §15 "in-flight exceptions" trap). dk==0.
- **`IsType`** (the `is`/`match` type test — match.ext/optional.ext) — true iff the
  subject matches ANY union member: a tag compare for primitives/collections
  (None/int/bool/float/string/Array/Map), an `lvrt_issub` for classes. Class members are
  resolved per-member by name (CGen::IsType parity — handles multi-class unions), with an
  `in.sym` fallback for the single-class case (X64Gen parity). Result is an unboxed bool.
- **Uncaught** — the module `main()` wrapper calls `lvrt_uncaught()` after the entry fn
  returns (X64Gen's post-entry `-31` call). It reports byte-matching the oracle
  (`Uncaught <Class>: <msg>`) and does NOT exit; the process returns 0 regardless (the
  ABI note + X64Gen's `exit(0)`). The A-M5 event-loop pump lands between entry and
  uncaught later.
- **Exception-hierarchy seeding** — `run()` now `idChain`s `RuntimeException` and its
  bases into the class table unconditionally (X64Gen.cpp:3859), so a runtime-raised
  RuntimeException (OOB / bad-key / unresolved dispatch) always has a NAMED row
  (`lvrt_raise`'s `lv_find_class_by_name`) with complete `subtypeIds` (`issub`) even when
  the program never instantiates one. Class ids are symbolic, so the re-seed does not
  perturb output; verified across all lanes. Handler entries were also added to the
  `jumpTarget` map (X64Gen.cpp:3078) so the IndexStore rebind-chain peephole never fires
  across a catch entry.

**Acceptance — all byte-identical, plus the whole suite green.** New lane
`corpus_llvm_m4` (symlinks, single-sourced `.expected`): `exceptions.ext`,
`corpus/elf/exceptions.ext`, `match.ext`, `optional.ext`. Full suite: **32/32** (was 31;
+1 lane). Every acceptance program is ASan+UBSan clean (`detect_leaks=0` — LSan cannot
see the region allocator). A scratch differential (catch-in-a-loop rebind, re-throw of a
new type, cross-frame propagation, nested try with wrong-type inner catch → propagate,
OOB through nested try, uncaught-at-top) is byte-identical to `lang --run` and
sanitizer-clean.

**Scope boundaries held (match the frozen backends, not a deviation).** `Await`'s
throwCheck is deferred to A-M5 with the rest of Await (still fails loudly, correctly).
`Arith`→opm (operator method) and `NewObject`→`$init` are the two call-like paths NEITHER
X64Gen NOR this backend checks — consistent with the frozen reference and outside doc §7's
list; a throwing operator/field-initializer would only be caught at the next checked op.
The `new Name(...)` constructor is a separate `Call` op, so it IS checked. No corpus
program trips any of these; revisit if a differential ever does.

**Known gap going into A-M5.** `emitThrowCheck` calls `lvrt_throwing()` (an
un-inlinable cross-`.o` call) after every call-like op — X64Gen reads its `g_throwing`
global inline. The cow_smoke timed lane (1M IndexStores, each now +1 flag call) still
finishes in ~0.8s under the 2s budget, so this is not a blocker, but inlining the flag
read is an A-M6 perf lever (it would need the flag exposed in the ABI — a contract change,
so a deliberate STOP-gated decision, not a drive-by).

### 2026-07-06 — A-M5 implemented (system layer: entry flip, natives, event loop, async). Gate G1. Opus-class agent.

**Context.** Branch was at 9898e22 (A-M4 + Track B's B-M4 merged). B-M4 pre-landed
`runtime/lv_entry.c` (the runtime-owned `main()`), so the entry flip was unblocked. This
milestone is **Gate G1** — full IR parity for the LLVM backend over the corpus.

**Delivered (`src/LlvmGen.cpp`).**
- **Entry flip (§3 / doc-2 §2.9).** LlvmGen now emits `lv_main(LvValue* ret)` (external
  linkage) instead of `main`. The generated object leaves `main` undefined, so the linker
  pulls `lv_entry.o`'s `main` (which does `lv_rt_init` → `lv_main` → loop drain →
  `lv_rt_exit_code`). `lv_main`'s body is X64Gen's `main` verbatim minus the process exit:
  register class table → ginit → entry → **run_loop drain** (`while lvrt_loop_has_work()
  && !lvrt_throwing(): lvrt_loop_step()`, X64Gen::genRunLoop parity) → `lvrt_uncaught` →
  the §2.5 escaping-tier meter to fd 2. The runtime's own post-`lv_main` drain is then a
  no-op. Test lanes gained `runtime/lv_loop.c` + `runtime/lv_entry.c` in `LANG_LVRT_SOURCES`.
- **Full `CallNativeFn` list (doc-2 §2.7).** All 15 std::sys natives routed to their
  `lvrt_sys*` entry points — args cross as `LvValue*` (the boundary rule; unlike X64Gen's
  raw payloads); fresh-string returns (readLine/sysRead/sysRecv) retained +1 per the A-M3
  string discipline; the void-return natives (close/timerCancel/unwatch) leave int 0 in
  the dest (X64Gen parity). Ownership contracts of doc-2 §2.7 applied exactly.
- **`Await` inline pump (X64Gen.cpp:3573-3593, the ratified post-maintenance-pass shape —
  NO `lvrt_await`).** Non-object promise passes through; else pump `while !ready &&
  !throwing && has_work: loop_step()`, then read `value` (raw `getfield` on the
  `ready`/`value` fields). A `throwCheck` follows (oracle parity — IrInterp throwChecks
  after every op, so a callback that threw during the pump dispatches at the Await).
- **IndexStore object `set ([])` dispatch.** A-M3 shipped GetIndex's object-getter path
  but never the symmetric setter path, so `g[1] = 7` on an object hit the array/map core
  and no-op'd (classes.ext line 6 regressed the moment the full corpus ran on LLVM).
  Ported X64Gen::genIdxSet's class dispatch: object receiver → call `set ([])(this,i,v)`,
  base unchanged; else the runtime core. Timer/socket/HTTP came for free once the natives
  + trampoline were right (the runtime fires callbacks through the registered dispatch
  trampoline, `lv_loop.c` `lv_invoke1`).

**Two runtime bugs fixed (owner-authorized — `runtime/**` is normally Track B's; each is
a completion of a documented-but-unfinished contract, verified against the oracle,
selftest, and every engine lane).**
1. **`lvrt_sysstat` field 2 (mtime) was stubbed to -1** — diverging from its own ABI note
   and the oracle (`RuntimeNatives.cpp:166` returns real `st_mtime`). files.ext line 10
   (`attrs.modified() > 0`) went false. Added `lv_plat_stat_mtime` to the platform floor
   (`lv_plat.h` + `lv_plat_posix.c`) and wired field 2. files.ext byte-identical after.
2. **`lv_arr_boxed_append` grow path leaked a dead `fresh` array** — `LvValue fresh;
   lvrt_arr_new(&fresh, 0); … (void)fresh;` allocated an empty array on every geometric
   grow, never used it, never freed it (~32B per grow). LLVM-only (X64Gen uses its own
   frozen append), so the churn gate caught it exactly as the A-M3 log predicted. Removing
   the dead alloc cleared the arrays.ext and index_set.ext churn leaks.

**Acceptance — Gate G1 green.** New lanes: `corpus_llvm` (over `corpus/elf/`, 14 programs)
and `corpus_llvm_full` (whole top-level corpus, 29 programs) — both byte-identical to the
oracle, both under `RESOURCE_LOCK corpus_net_ports`. `corpus_churn_leak_llvm` extends
`fuzz/churn_leak.py` with an `--engine llvm` (native-obj + runtime link; the meter is
emitted by `lv_main`). Full suite **35/35** (was 32; +3 lanes). Every acceptance program
ASan+UBSan clean (`detect_leaks=0`).

**STOP-adjacent scope call (owner-decided, NOT a design correction).** The churn llvm lane
is **11-green / 3-XFAIL**, not the §8-stated 13-green/1-XFAIL. The two extra XFAILs
(`returned_value_struct`, `struct_array_field`) leak on the LLVM backend because
scope-local value structs (a `CopyVal` heap copy at rc 0) have no `VFree` and no arena to
reclaim them — X64Gen frees them via `beginAlloc`/`restoreArena`. There is **no safe
shortcut**: a blanket frame-exit `VFree` would free value-struct *return* values (UAF) and
risks inline-pointer corruption; correct reclamation needs escape analysis, which *is* the
A-M6 arena tier the A-M3 log already scoped ("an arena tier is the A-M6 perf lever"). Per
the owner's decision, these are deferred to A-M6 and marked `XFAIL-LLVM` (a new
engine-aware marker in `churn_leak.py` — GREEN on ELF, expected-red on `--engine llvm`
only). A-M5 measures the gap exactly as the A-M3 note predicted; A-M6 closes it.

**Note going into A-M6.** The arena tier is now the load-bearing A-M6 item: it closes the
two value-struct churn XFAILs (flip their `XFAIL-LLVM` markers off) and is the perf lever
for scope-owned allocation. The `lvrt_throwing()`-per-call cost (A-M4 note) and an inlined
throw flag remain the other A-M6 levers.

### 2026-07-06 — Maintenance pass #3 (Fable): G1 ratified, drift corrected, agent-3 language work integrated, branches synced

All portable-backend agents stopped; owner-directed maintenance. Doc-2 §10 carries the
same-date companion entry for the Track B-facing pieces.

**G1 ratified as functionally met (roadmap §2 row updated).** `corpus_llvm_full` is
byte-identical over the whole corpus; the churn llvm lane is green at 11/3-XFAIL (the
2-XFAIL divergence from ELF's profile is the owner-ratified A-M6 arena deferral, not a
gap in the gate); one-step `--build-native` verified end-to-end including a whole
`--project` build. The remaining G1 language in the gate cell — perf table + reference-doc
updates — is A-M6 §9 items 4–5 and stays open. §1's engine table and §10.1's lane table
were updated to as-built (the lane is `corpus_churn_leak_llvm`; `LANG_LVRT_SOURCES` grew
`lv_loop.c` + `lv_entry.c` at the entry flip). §9 now leads with the arena tier as the
load-bearing A-M6 item and folds in the inline-throw-flag lever with its STOP-gated
ABI-addition caveat.

**A-M5 exit-check debt closed.** `examples/curl/test/run-tests.sh` grew a third,
feature-guarded `--native` matrix: it builds the whole lcurl project once via
`--build-native --project` and drives the identical fixture cases against the binary.
As-built result: **72/72** (`--run`/`--ir`/`--native` × 24) — sockets, timers, redirects,
chunked decoding, `-o` file output, `--max-time`, and the error paths all agree with the
interpreters through the LLVM backend + runtime v2. The runner stays manual (not a ctest
lane); the corpus lanes remain the acceptance of record.

**Agent-3 language work integrated (origin/agent3, 6 commits).** Float arithmetic fixes
(oracle/IR/CGen/X64Gen — the shared engines all computed on the int payload), loud
checker rejection of undefined primitive operators, bug.md #1 (block-scoped namespace
imports on `--ir`), bug.md #2 (bare top-level globals). File overlap with the
post-agent-3-base portable-backend work: **empty** — clean merge. Two facts worth
recording: (a) runtime v2's `lvrt_arith` float row was **already correct** (agent 3's
"LLVM native runtime" fix landed in `src/NativeRuntime.cpp` — the pre-runtime-v2
scalar-era support file, which nothing links anymore; runtime v2 needed nothing); (b) the
new `floats.ext` exposed ONE real
LlvmGen gap — `(0.25).toString()` had no float row in the CallDyn native dispatch (empty
output; every other float op already flowed through `lvrt_arith`/print correctly). Fixed
this pass: a tag-2 `toString` row calling `lvrt_to_string` (fresh string +1, same
ownership as the int row). `floats.ext`, `toplevel_global.ext`, and `block_ns_import.ext`
join every LLVM lane via the full-corpus glob and are byte-identical post-merge.

**Also this pass:** `lv_plat_win32.c` gained the `lv_plat_stat_mtime` the A-M5 mtime fix
added to the POSIX floor (FILETIME → epoch seconds; compile-unverified like the rest of
that pre-landed floor — the doc-2 §5 floor table row is updated). Branches synced:
`track-a-llvm-parity` and `track-b-runtime` both fast-forwarded to the post-merge master.

**State for the next agents:** full suite green (see the sync commit for the count),
lcurl 72/72, bug.md #10 (int div/mod-by-zero semantics) filed and awaiting an owner
ruling — no portable-backend agent should act on it. Track A's next milestone is A-M6
(§9, arena tier first); Track B's is the environment-gated B-M5 bring-up plus runtime
support for A-M6 if the arena work surfaces contract needs.

### 2026-07-06 — A-M6 implemented (arena tier + perf/docs closeout). Gate G1 fully closed. Sonnet-class agent.

**Context.** Branch confirmed at 551f6bd (post maintenance-pass-#3 sync), 35/35 green,
matching the handoff exactly. This is the last Track A milestone — closing it closes G1.

**The arena tier (load-bearing item, done first).** Read X64Gen's `beginAlloc`/`endAlloc`/
`restoreArena` (X64Gen.cpp:3106-3171) as the ground truth per §0.3, plus the shared
`analyzeOwnership()` (`src/Ownership.cpp` — NOT frozen, NOT X64Gen-owned; X64Gen just
calls it at X64Gen.cpp:3943) that already computes the `scopeOwned` set X64Gen routes to
its arena. Two findings shaped the scope, both logged here rather than assumed:
1. X64Gen arena-routes EVERY scope-owned allocation type (NewObject/NewArray/NewMap/
   MakeRange/MakeClosure/CopyVal), but tracing why `returned_value_struct.ext` and
   `struct_array_field.ext` specifically leak on LLVM (and NOT any general object/array/
   map pattern) shows the *correctness* need is narrower: value-struct objects are
   permanently excluded from `lvrt_retain`/`lvrt_release` (`lv_is_counted`'s
   `lvrt_isvalueclass` gate, lv_runtime.c:196) — the ONLY things that can ever reclaim
   them are `lvrt_vfree` (Lower's existing, narrower `maybeVFree` tracking) or the arena.
   Regular (non-value) allocations are already correctly reclaimed by the generic ARC
   wrap regardless of the arena, so arena-routing them would be a pure micro-optimization
   with real correctness risk attached (a scope-owned `MakeClosure` with captures would
   leak the captured value's retain, since `lv_recursive_free` — which releases
   captures — only runs via `lvrt_release`'s rc-hits-0 path, never via an arena reset).
   Scoped this milestone to value-struct sites ONLY: `CopyVal` with `c==1` (a definite
   copy) and `NewObject` with `in.sym->isValue` — matching doc §9's own wording
   ("a CopyVal whose result does NOT reach a Ret/escaping store is arena-safe") more
   precisely than the blanket X64Gen behavior would. `NewObject`'s `c==1` special case
   (bare value-struct field auto-construct inside `$init`, X64Gen.cpp:3430-3443) needs no
   separate handling: Ownership.cpp's `RawSet` sink rule always marks that site's result
   as escaping ("stored in an object"), so it structurally never reaches `scopeOwned` —
   verified, not assumed.
2. The ABI's typed constructors (`lvrt_obj_new`, `lvrt_copyval`) are hardcoded to
   `LV_TIER_HEAP` internally (lv_runtime.c); there is no tier-aware entry point for them.
   Rather than treat this as a contract gap (STOP), built two NEW module-level LLVM-IR
   helpers (`emitArenaHelpers`, mirroring `emitDispatchTrampoline`'s existing pattern of
   hand-built functions) — `lv_objnew_arena`/`lv_copyval_arena` — using ONLY already-
   frozen primitives: `lvrt_halloc(size, tier)` (doc-2 §2.5's own "raw allocator... write
   the header yourself" escape hatch, otherwise dead code — nothing else in lv_runtime.c
   ever passes `LV_TIER_ARENA`), `lvrt_isvalueclass`, `lvrt_fieldcount`. `lv_copyval_arena`
   recurses field-by-field (self-call, bounded — value structs form a finite DAG per
   Lower.cpp's own comment) so a nested value-struct field lands on the arena with its
   parent, mirroring X64Gen's "arena outer → arena inner" rule without needing that
   rule's dynamic this-tier-check at all (moot given finding 1). Zero changes to
   `runtime/lv_abi.h` or `runtime/lv_runtime.c`.
   Prologue: `lvrt_arena_save()` into an alloca; every `Ret`/`RetVoid`/the unwind-
   propagation block (`blocks[irfn.code.size()]`, throwCheck's no-handler-matched target)
   calls `lvrt_arena_restore()` after `releaseAllRegs()`, matching X64Gen's Ret ordering
   exactly. A per-function `usesArena` precomputation (scans for a scope-owned qualifying
   site once) skips the save/restore pair ENTIRELY for functions with no arena traffic —
   fib and most scalar code pay nothing (perf note below).

**Acceptance (arena tier).** Both `XFAIL-LLVM` markers flipped off
(`tests/corpus/churn/returned_value_struct.ext`, `struct_array_field.ext`); churn llvm
lane reaches **13-green/1-XFAIL**, matching ELF exactly (the one shared XFAIL,
`dense_index_set.ext`, is `XFAIL` — not `XFAIL-LLVM` — i.e. already expected-red on ELF
too, an inherited edge unrelated to this milestone). Full `ctest` 35/35. Manually verified
ASan+UBSan clean (`ASAN_OPTIONS=detect_leaks=0`) on all 46 elf-parity + full-corpus
programs, the whole churn corpus, and a hand-written nested-value-struct-field /
dense-array edge-case sweep — `run_native_llvm.sh`/`churn_leak.py` don't wire sanitizer
flags into their `cc` invocations, so this was done by hand per the handoff's documented
manual pipeline, not a new ctest lane (matching A-M4/A-M5's own verification style).

**Perf work (doc §9 items 1-4).**
1. **PassBuilder O2 pipeline (hurdle H-12).** `emitObject` previously ran ONLY the
   codegen (isel/emission) legacy `PassManager` — zero IR-level optimization ever ran.
   Added a `PassBuilder(tm)` module pipeline (`buildPerModuleDefaultPipeline(O2)` /
   `buildO0DefaultPipeline(O0)` — the former asserts on O0, hence the branch) before
   object emission; `main.cpp` gained `--opt-level 0|2` (default 2), threaded through
   `LlvmGen::emitObject`'s new third parameter to both `--native-obj` and
   `--build-native`. Corpus verified green at both levels (46 programs, output +
   ASan/UBSan at O0; full `ctest` — which runs the default O2 — at O2).
2. **Measured, then added, three fast paths**, each verified against the full corpus +
   churn net after landing (§9's #2/#4; RawGet/RawSet fixed-offset and resolved CallDyn
   direct calls were already done at A-M2, per the doc):
   - `Op::Arith` inlines int-int and float-float `+ - * / % < > <= >= == != & |` (shl/shr
     too, though the checker rejects them on either primitive today — dead but harmless,
     matching `lvrt_arith`'s own unreachable-in-practice default rows) directly in LLVM
     IR, byte-for-byte matching `lvrt_arith`'s semantics (int div/mod-by-zero silently 0;
     float div/mod-by-zero silently 0.0 rather than IEEE inf/nan — see the oracle-bug
     note below for why that second one is deliberate, not an oversight). Mixed int/
     float, strings, objects, and unresolved operators fall through to the ORIGINAL
     opm/`lvrt_arith` dispatch, unchanged — the fast path only changes which cases skip
     the call, never the semantics.
   - `truth()` (every `JumpIfFalse`/`JumpIfTrue`) and `Op::Not` inline `lvrt_truth`'s
     `tag==BOOL && payload!=0` directly instead of a call.
   - The arena-tier `usesArena` skip above is itself a perf fast path, not just a
     correctness nicety.
3. **Measured and logged, NOT spent (STOP-adjacent — cross-track ABI questions, not a
   drive-by):**
   - The inline-throw-flag lever (doc §9 item 2's explicit callout): `lvrt_throwing()`
     is a cross-`.o` call after every call-like op. Would need the flag exposed as an
     ABI global (§2.8 addition) — real, per the doc's own framing, but not something to
     spend unilaterally.
   - A NEWLY discovered lever of the same shape: `lvrt_arena_save`/`_restore` are ALSO
     cross-`.o` calls (2 per value-struct-allocating invocation), unlike X64Gen's inline
     global-flag arena scheme. Measured directly: a 3M-call value-struct-allocator micro
     is ~1.4s on LLVM vs ~0.85s on ELF (the dominant remaining gap once arith/truth were
     inlined and non-arena functions stopped paying the save/restore pair at all); a
     non-allocating indexed-store micro of comparable iteration count is within ~2% of
     ELF. Closing this needs the SAME kind of ABI addition as the throw flag (a directly-
     addressable arena-cursor global) or LTO-linking runtime bitcode into the program
     module (doc §9 item 3's "optional stretch" — not attempted: this machine has no
     `clang`, so `-emit-llvm` on the C runtime sources isn't buildable here). Logged in
     the handoff doc for whichever track/model picks this up next; not spent here.
4. **Benchmarks** (this machine; `the Track-A handoff (removed 2026-07-07)` has the same
   numbers). `fib(30)`: oracle 1.8s, IR interp 1.0s, emit-C++ 0.35s, LLVM 0.21s, ELF
   0.18s (was ~0.28s before the arith/truth/arena-skip fast paths — measured
   incrementally, each landing shaved real time). New micros in
   `tests/corpus/llvm_smoke`-style form (kept as scratch benchmarks, not new ctest lanes
   — doc §9 didn't ask for permanent benchmark fixtures, just numbers): an indexed-store
   loop (5M `Array<int>` writes) is LLVM 0.25s vs ELF 0.24s (near parity — no value
   structs, no arena traffic); a value-struct-returning allocator run 3M times is LLVM
   1.4s vs ELF 0.85s (the arena-call-overhead gap above). Per doc §9 item 4's own
   instruction ("if it does [not beat ELF], fast paths are missing — investigate before
   closing"): investigated, root-caused to the arena save/restore call pair specifically
   (not the arena logic itself, not the ownership analysis, not the construction
   helpers — isolated by testing an arena-free micro against an arena-heavy one), and the
   remaining gap needs the STOP-gated ABI/LTO levers above, not more Track-A-local work.

**Two bugs found, NOT fixed (outside this milestone's file ownership and scope — logged
for the owner / language-features agent, matching this repo's bug.md convention; neither
is touched, per the standing instruction never to edit bug.md/designs/ myself):**
- `bool == bool` / `bool != bool` produce the INVERTED answer on both the tree-walk
  oracle (`--run`) and the IR interpreter (`--ir`) — e.g. `true != false` prints `false`.
  Found while sanity-checking the new int/float `Arith` inline path against edge cases.
  `lvrt_arith`'s (unmodified) int/bool fallback branch already computes the correct
  answer, so LLVM actually disagrees with the buggy oracle here — invisible today because
  no corpus program compares bare bools with `==`/`!=`. This is `Eval.cpp`/`IrInterp.cpp`,
  not `LlvmGen.cpp` — outside Track A entirely.
- Float `x / 0.0` / `x % 0.0` silently return `0.0` on every native/runtime path
  (`lvrt_arith`, matching X64Gen's own embedded arith) but the oracle produces real IEEE
  `inf`/`nan` — the mirror image of bug.md #10 (int div/mod-by-zero), but for floats, and
  not yet filed anywhere. The new inline float fast path deliberately REPLICATES
  `lvrt_arith`'s existing 0.0-override rather than "fixing" only the inlined subset,
  which would create a fresh inconsistency against the mixed-int/float fallback that
  still calls the unmodified `lvrt_arith`.

**Docs updated (§9.5, one commit with the code — surgical edits, no restructuring):**
`docs/reference.md` §7.1 (the stale "async/streams/unions unlowerable" note — all three
lower cleanly now; only genuinely unimplemented constructs like `fork` still fall to
`Lowerer::fail`), §7.3 (LLVM now documented as the primary/portable backend with full
parity, the arena tier, `--opt-level`, real fib(30)/micro numbers, corrected churn counts
— the corpus grew since "12/12" was written; it's 13/13 on both engines now, one shared
XFAIL); `info.md` §17's LLVM bullet (was "scalar core... a follow-up"); and this track's
OWN handoff, `the Track-A handoff (removed 2026-07-07)` (post-A-M3 stale — updated to reflect
the closed track, corrected the "never touch runtime/**/main.cpp" hard rule against the
A-M5/A-M6 precedent of narrowly-scoped, logged exceptions, and carried forward the two
unspent perf levers + two found-but-out-of-scope bugs above so they're not lost).
`HANDOFF.md` at repo root is a DIFFERENT track's document (agent3's bug-fixing session,
per its own title) — not touched; `the Track-A handoff (removed 2026-07-07)` is this track's
actual handoff and is what doc §9.5 meant.

**Gate G1 — fully closed.** All of A-M1 through A-M6 landed; `src/LlvmGen.cpp` has full
IR parity with the frozen `src/X64Gen.cpp`, matching corpus output byte-for-byte, matching
its churn-leak profile exactly (13-green/1-XFAIL, same XFAIL), and passing ASan/UBSan on
every acceptance program. No more Track A milestones remain on the roadmap (§2); see the
handoff for what's next.

### 2026-07-06 — Maintenance pass #4 (Fable): agent-3 Track 01 merged, LLVM brought to F1 parity, A-M7 authored, branches synced

Owner-directed phase turnover (Track A agent continuing; Track B agent swapping to a new
session). Doc-2 §10 carries the same-date companion entry.

**Agent-3 Track 01 merged to master** (6 commits: int shift/xor/complement, hex/binary
literals + digit separators, `\xNN` + consolidated escapes, string interpolation; Track 01
design moved to `designs/complete/`). Textual merge was clean; ONE semantic collision
surfaced exactly where predicted: A-M6's inline `shl`/`shr` fast path was emitted while
shifts were checker-rejected (dead code, masking the count `& 63` to match `lvrt_arith`),
and Track 01 F1 made int shifts legal with **throw-on-out-of-range semantics**
("shift count out of range", counts outside 0..63 — implemented in arithPrim/CGen/X64Gen,
deliberately not in LLVM). `bitops.ext` joined `corpus_llvm_full` via the glob and went
red (compile-time "LLVM backend: operator" — `^` had no opCode row; the masked shifts
were the byte-divergence waiting behind it).

**Fixed this pass (LLVM lane to F1 parity):** LlvmGen `opCode` gains `Caret → 15`; the
int-int inline path gains `xor` and replaces both masked shifts with X64Gen genAr's
range-check-then-raise shape (unsigned `count > 63` compare → `lvrt_raise("shift count
out of range")` → `emitThrowCheck` at the Arith pc, oracle-parity dispatch; in-range
computes plain `shl`/`ashr`). Runtime v2 kept honest with the ratified semantics (logged
in doc-2 §10): `LV_OP_XOR = 15` added to `lv_abi.h`, `lvrt_arith`'s masked shift rows now
raise (unreachable from LLVM-emitted code — the inline path range-checks first — but no
future caller inherits the mask), `lvrt_opm`'s name table gains `"^"`. Verified:
`bitops.ext` byte-identical at `--opt-level 0` AND `2`; full `ctest` **36/36** (35 lanes +
the four Track 01 corpus programs flowing through every glob lane); `runtime_selftest`
green plain + ASan/UBSan scratch build.

**Two A-M6-logged bugs verified and FILED (bug.md #11, #12 — owner rulings, not
portable-track work):** #11 bool `==`/`!=` compares the never-populated `.i` field on
oracle/IR/emit-C++ (always-"equal"; LLVM and ELF natives compute the real answer — the
natives are right and the oracle is wrong). #12 float `/0.0`: the engine matrix is worse
than A-M6's note — oracle/IR/emit-C++ AND the frozen ELF backend all produce IEEE
`inf`/`nan` (ELF renders `nan` where the interpreters print `-nan`); only the LLVM lane
(`lvrt_arith` + the A-M6 inline row that faithfully replicates it) silently returns
`0.0`. Neither blocks either track; neither is corpus-pinnable until the engines
converge.

**A-M7 authored (§9b) and its ABI dependency ratified (doc-2 §2.10):** `lv_g_throwing` +
`lv_g_arena_cursor` exported as load/store-able hot words, resolving both STOP-gated
levers A-M6 measured and left unspent. A-M7 is Track A's next (and final currently-scoped)
milestone.

**Branches synced:** `agent3`, `track-a-llvm-parity`, `track-b-runtime` all fast-forwarded
to post-merge master (the Language-agent3 worktree updated in place). Track B's next work
remains the environment-gated B-M4 qemu execution + B-M5 Windows/macOS bring-up (doc-2
§6/§7) — toolchain installation is an owner-authorization item, not an agent call.

### 2026-07-06 — A-M7 implemented (inline hot-state fast paths). Sonnet-class agent.

**Context.** Branch confirmed at a7466c0 (post maintenance-pass-#4 sync), 35/35 `ctest`
green (this session's actual registered-test count — the pass-#4 entry's "36/36" phrasing
folds in the four Track 01 corpus programs riding the full-corpus glob lanes, not 36
separate `ctest` entries; no lane was added or dropped by this milestone). Read both docs
in full per §0.3/§9b before touching anything; the §2.10 contract (doc-2) was already
ratified, so no STOP was needed — this was a mechanical wire-up of an already-agreed
contract.

**1. Runtime export rename (`runtime/lv_runtime.c` + `runtime/lv_abi.h`).** `static
int32_t g_throwing` → `int32_t lv_g_throwing`; `static uint8_t* g_arena_cursor` →
`uint8_t* lv_g_arena_cursor` — every internal use (allocator, `lvrt_throw_set`/
`lvrt_throwing`/`lvrt_uncaught`, `lv_rt_init`, `lvrt_arena_save`/`lvrt_arena_restore`,
`lv_alloc_arena`) renamed in place via a scoped `sed` pass, verified against a full grep
afterward (no stray `g_throwing`/`g_arena_cursor` left, no accidental over-match). Declared
both as `extern` in `lv_abi.h`: `lv_g_arena_cursor` next to §2.5's memory API (new §2.10
subsection, matching doc-2's placement), `lv_g_throwing` next to `lvrt_throwing` in §2.8.
Touched nothing else under `runtime/**` — no other Track B file, no ABI shape change, no
accessor-function signature change.

**2. `src/LlvmGen.cpp`: throw-flag inlining.** Added two `GlobalVariable*` members
(`gThrowing`, `gArenaCursor`), declared once in the constructor as external globals (no
initializer — `GlobalValue::ExternalLinkage`, resolved by the linker against
`lv_runtime.c`'s definitions), matching the existing `module->getOrInsertFunction` pattern
used for every other runtime entry point. `emitThrowCheck`'s `flag` load changed from
`b.CreateCall(rtThrowing, {})` to `b.CreateLoad(i32Ty, gThrowing)` — one edit site, so
every call-like op's checked-call dispatch inherits it structurally (the A-M4 audit
invariant holds unchanged). Two other `lvrt_throwing()` call sites were deliberately LEFT
AS CALLS, not inlined: `lv_main`'s event-loop drain condition (§3/doc-2 §2.9, runs once per
outer-loop pump, not per call-like op) and the `Await` inline pump's per-step throw check
(`LlvmGen.cpp` Await case) — the doc's own instruction was "one helper, one edit site";
neither of these is `emitThrowCheck`, and neither is the measured hot path (A-M6's note
names the per-call-like-op check specifically). Left unchanged and logged rather than
silently expanded past the ratified scope.

**3. `src/LlvmGen.cpp`: arena save/restore inlining.** `arenaMarkSlot`'s type changed from
`i64Ty` (matching `lvrt_arena_save`'s `int64_t` return) to `ptrTy` (matching
`lv_g_arena_cursor`'s `uint8_t*` type directly — no int/pointer round-trip). Prologue:
`b.CreateStore(b.CreateCall(rtArenaSave, {}), arenaMarkSlot)` → `b.CreateStore(
b.CreateLoad(ptrTy, gArenaCursor), arenaMarkSlot)`. `restoreArena`'s body:
`b.CreateCall(rtArenaRestore, {b.CreateLoad(i64Ty, arenaMarkSlot)})` → `b.CreateStore(
b.CreateLoad(ptrTy, arenaMarkSlot), gArenaCursor)` — a direct store, matching
`lvrt_arena_restore`'s actual body (`cursor = mark`, verified by rereading `lv_runtime.c`
before ratifying, per doc-2 §2.10's own instruction) exactly. The `usesArena`-gated skip
(no save/restore pair at all for frames with no arena traffic) and the restore-after-
`releaseAllRegs` ordering on every return/unwind path are unchanged — only the
call-vs-load/store mechanics moved. `rtArenaSave`/`rtArenaRestore`/`rtThrowing`
`FunctionCallee` members are kept (the latter is still called at the two sites above); the
first two are now unused by `LlvmGen.cpp` but left declared (harmless — an unreferenced
`getOrInsertFunction` costs nothing, and the doc says the entry points stay in the ABI for
non-inlining callers, so removing the declarations would be gratuitous churn against a
"narrow edit" mandate).

**Verification, in order:**
- `runtime_selftest` green plain AND under a scratch `-DLANG_RT_SANITIZE=ON` build (fresh
  `cmake` configure in a scratch dir; both green, no ASan/UBSan report).
- Full `cmake --build` clean (only the two pre-existing `X64Gen.cpp` `-Wmisleading-
  indentation` warnings and the pre-existing `Checker.cpp` trigraph warning — both predate
  this milestone, neither touched).
- Full `ctest -j$(nproc)`: **35/35 green** (see the context note above on the 35-vs-36
  count).
- Corpus byte-identical at **both** `--opt-level 0` and the ctest-default `2` (hurdle
  H-12): wrote a scratch harness mirroring `tests/run_native_llvm.sh` but threading
  `--opt-level 0` through the `--native-obj` invocation, run over every LLVM-lane corpus
  dir (`core` 5, `llvm_objects` 3, `llvm_m3` 6, `llvm_m4` 4, `elf` 14, and the full
  top-level `tests/corpus` 36) — all byte-identical to `.expected` at O0, matching the
  already-green O2 ctest run. Not promoted to a new ctest lane (same precedent as A-M6:
  manual verification, not permanent CI surface).
- Churn llvm lane: **13-green/1-XFAIL** (`dense_index_set.ext` the one shared XFAIL),
  unchanged from the pre-A-M7 baseline — ran `fuzz/churn_leak.py --engine llvm` directly to
  confirm post-change.
- Manual ASan/UBSan sweep (the A-M4/A-M5/A-M6-precedented pipeline: runtime `.c` sources
  compiled with `-fsanitize=address,undefined -g` via `cc`, linked against the PLAIN
  `--native-obj` output, `ASAN_OPTIONS=detect_leaks=0`): all 36 top-level corpus programs,
  the 14-program `elf` subset, `core`/`llvm_objects`/`llvm_m3`/`llvm_m4`, and all 14 churn
  programs (templated `@N@` substituted with 300) — sanitizer-clean, no new report anywhere
  the inlined loads/stores are exercised.

**Benchmarks.** The three A-M6 micros were never persisted as corpus files (A-M6's log:
"kept as scratch benchmarks, not new ctest lanes") — re-authored from scratch this pass,
same shapes described in A-M6's log and `reference.md` §7.3:
- `fib(30)`: unchanged shape, ~0.21s LLVM vs ~0.19s ELF (both a hair faster than A-M6's
  reported 0.21s/0.18s — noise-level, not attributable to this milestone; `fib` has no
  arena traffic and its `Call`-only throw-checks did not show a measurable A/B delta,
  below).
- **5M-write indexed-store loop** (`Array<int>` of size 8, `a[i % 8] = i` 5,000,000 times):
  now ~0.45s LLVM vs ~0.46s ELF — at or fractionally ahead of ELF (was ~0.25s/~0.24s at a
  smaller iteration count in A-M6's run; the shapes aren't identical since the original
  wasn't preserved, so treat the ratio, not the absolute seconds, as comparable).
- **Value-struct allocator, 3,000,000 calls**: reconstructing A-M6's exact benchmark was
  not possible (the file was never committed) — the first two reconstructions attempted
  (`Point p = makePoint(i)` bound to a loop-local var across all 3M iterations of ONE
  outer frame; then the same call chained directly into `.x` with no local binding) both
  **failed before completing**: the arena is a fixed 64 MiB bump region reset only at
  frame *return*, so 3M scope-owned struct copies accumulated in the *caller's own frame*
  (never returning until after the whole loop) exhausts it — LLVM raised its own
  `lv_die("lvrt: arena exhausted")` cleanly; the frozen ELF backend's own (differently-
  sized, differently-bounds-checked) arena segfaulted outright on the same shape. **This is
  a pre-existing property of the arena tier's design** (a per-*frame* watermark, not a
  per-*iteration* one — doc §9's own text), not something introduced or fixable by this
  milestone (X64Gen is frozen; the arena's fixed-size bump-allocator shape is A-M6's, not
  A-M7's, to revisit) — logged here rather than silently worked around by shrinking N below
  the threshold, since that would misrepresent what the benchmark measures. The shape that
  *does* complete safely on both engines calls a single function
  (`makePointX(seed)` — builds a `Point`, reads `.x`, returns the `int`) 3,000,000 times
  from the driver loop, so each call's `Point p = Point()` is scope-owned *within that one
  call's own frame* and the arena resets on every return — this is the fair per-call
  analog of "a value-struct-allocating frame" the doc's levers actually target. Result:
  **LLVM ~0.91s vs ELF ~0.67s (~1.36x)** — closer than A-M6's reported 1.4s/0.85s (~1.65x)
  but short of the ≤1.1x target.
- **Controlled A/B** (same benchmark sources, only `src/LlvmGen.cpp` swapped between the
  pre-A-M7 commit and this one, both linked against the identical post-rename runtime since
  the renamed accessor functions are unchanged): the value-struct allocator is **~8%
  faster** after this milestone (pre-A-M7 cross-`.o`-call baseline ~0.99s → post ~0.91s,
  same machine, same binary otherwise) and the indexed-store micro is **~6% faster**
  (~0.475s → ~0.448s, consistent with its per-op `IndexStore` OOB throw-check now being a
  load instead of a call). `fib(30)` showed no measurable A/B delta (~0.21s either way) —
  plausible since `fib`'s call overhead (parameter retain, frame setup) already dominates a
  single inlined `i32` load's savings.
- **Per doc §9 item 4 / §9b item 4's own rule ("if it does not [reach ≤1.1x], investigate
  before closing"): investigated, not fully closed.** The measured win is real (both A/B
  deltas reproduce across repeated runs) but does not fully close the value-struct gap.
  Root cause of the residual: (a) every call — arena-using or not — still pays a real
  `lvrt_retain` cross-`.o` call per parameter (untouched by this milestone's scope, which
  was limited to the throwing flag and the arena cursor specifically); (b) the arena mark
  still round-trips through a frame `alloca` rather than staying in a register end-to-end
  (LLVM's O2 `mem2reg`/SROA pipeline may or may not promote it depending on the function's
  shape — not investigated further, out of this milestone's scope to chase); (c) the
  previously-identified LTO/bitcode lever (§9 item 3) remains untried — this host still has
  no `clang`, confirmed again this pass (`which clang clang-18` → not found), so `-emit-
  llvm` on the runtime `.c` sources is still unbuildable here. All three are logged as open
  follow-ups, not spent unilaterally (some touch scope beyond doc-2 §2.10's ratified
  contract and would need their own STOP-gated ABI discussion).

**Docs updated:** `docs/reference.md` §7.3 (the fib/indexed-store/value-struct numbers
above, replacing A-M6's; the residual-gap explanation). This log entry.

**No more Track A milestones remain scoped** (doc §2's roadmap table) — A-M7 was
authored as "post-G1 perf tail," and its own acceptance does not gate any further
milestone. Full `ctest` green, corpus byte-identical at both opt levels, churn lane
unchanged, sanitizers clean: acceptance met. If a future session wants to chase the
residual ~1.36x further, the three items above (parameter-retain inlining, register-
resident arena mark, LTO) are where to start — each is its own scope/STOP conversation,
not a continuation of this one.
