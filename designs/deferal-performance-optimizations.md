# Tech Design — Logged Performance Deferrals: AST Arena Allocator + JIT Execution Tier

**Status:** deferral rationale, tracked. **NOT approved for implementation** — this
document exists so the two deferrals are logged with their reasons, their resolution
paths, and the concrete triggers under which adoption becomes worthwhile. Building
either item now is out of scope and would be a design violation, not initiative.
**Authored by:** Fable-class model. **Date:** 2026-07-06.
**Convention:** per `designs/complete/techdesign-00-overview.md`, this file moves to
`designs/complete/` only if/when a deferral is *adopted and landed* — until then it
stays live as the rationale of record.

---

## 0. Read this first

### 0.1 What this document is

Two optimizations are deliberately **deferred, not rejected**:

| # | Deferral | Where the deferral is logged today | Current approach |
|---|---|---|---|
| A | **Arena allocator for the AST** | `src/Ast.hpp:16-19` — "Ownership is unique_ptr / vector (RAII). An arena is a deferred optimization for once the node set stabilizes." | `unique_ptr`/`vector` RAII, virtual destructors |
| B | **JIT execution tier** | `info.md:1359-1363` (§17) — "JIT is deferred, not adopted." | AOT (LLVM) for speed; tree-walk oracle + IR interpreter for flexibility |

Both deferrals are **legitimate and current**: the approach in place is correct, and in
both cases already fast enough for what the project needs today. This document records
*why* they wait, *what could go wrong while they wait*, *how each would be built when
its time comes*, and *the measurable trigger that starts that clock*.

### 0.2 What this document is not

- It is **not** a green light. No milestone here is scheduled. An implementation agent
  reading this doc has **nothing to build** from it.
- It is **not** about the *runtime* per-frame arena. That is a different arena (see
  §1.3) and it already **landed** at A-M6.

### 0.3 STOP protocol

Same as `docs/techdesign-portable-backend.md` §0.3. If a future milestone adopts either
deferral and the design here proves wrong in a way that requires an architectural
choice, a Sonnet-class (or smaller) agent must STOP, log, and escalate — not improvise.
Two standing STOP tripwires specific to this doc:

- Any step that would touch `src/X64.hpp/.cpp` or `src/X64Gen.hpp/.cpp` — **frozen**,
  full stop (portable-backend §0.4). Deferral B in particular must never become an
  excuse for new hand-rolled x86-64 emission.
- Any step that would change the runtime ABI contract (`docs/techdesign-portable-
  backend-2.md` §2 — normative and frozen). A JIT tier that "needs one small ABI
  addition" is a STOP event, exactly as the inline-throw-flag lever was flagged.

---

## 1. Deferral A — arena allocator for the AST

### 1.1 What exists (verified 2026-07-06)

The AST is a tagged class hierarchy (Clang-style Kind + virtual destructor), owned
node-by-node through `unique_ptr` (`src/Ast.hpp:14-33`):

- Ownership typedefs: `TypeRefPtr` / `ExprPtr` / `StmtPtr` are
  `std::unique_ptr<...>` (`Ast.hpp:31-33`).
- The deferral itself is in the header comment: *"Ownership is unique_ptr / vector
  (RAII). An arena is a deferred optimization for once the node set stabilizes"*
  (`Ast.hpp:16-19`).
- Node creation is **already mostly centralized**: the parser builds every node through
  three factory helpers — `mkExpr`/`mkStmt`/`mkType` (`src/Parser.cpp:116-130`). The
  exceptions are direct `std::make_unique` sites in `src/Rules.cpp` (~12 sites, rule
  expansion synthesizing nodes: e.g. `Rules.cpp:224, 251, 364, 1507, 1611, 1637`) and
  `src/Checker.cpp` (2 sites: a `TypeRef` clone at `:78`, an `Inject` arg at `:1101`).
- The AST already carries a **borrow lifetime**: node names are `std::string_view`
  into the source text (`Ast.hpp:48-49`), and `src/Source.hpp:14` states the rule — "a
  SourceFile must outlive" the views. So "the AST depends on a longer-lived buffer" is
  already a discipline the codebase enforces; an arena would extend an existing
  pattern, not introduce a new class of lifetime.
- The AST is **not write-once**: rule expansion (`Rules.cpp`) rewrites and splices
  subtrees post-parse, and the checker synthesizes/clones nodes. Under `unique_ptr`,
  a replaced subtree frees immediately; that behavior changes under an arena (§1.5).

### 1.2 Why it is deferred (the rationale, in detail)

1. **RAII is correct by construction; an arena is a pure optimization.** `unique_ptr` +
   virtual destructor means the tree cannot leak and cannot double-free, with zero
   analysis burden on any pass that mutates it. An arena changes *when memory is
   reclaimed*, never *what the program means* — there is no semantic or correctness
   deficit being papered over. Optimizations with no correctness payload wait for
   measurements.
2. **The node set has not stabilized — the header's own condition is unmet.** The
   deferral comment gates on "once the node set stabilizes." As of this writing,
   Track 03 (`char`/`enum`/`Block`) and Track 07 (iteration protocol) both add
   `StmtKind`/`ExprKind` members, and the rules/metaprogramming layer (`Rules.cpp`,
   info.md §16) is still an active axis. Migrating ownership while the node set churns
   multiplies merge pain across every in-flight compiler track (the overview's §2
   merge-order rule exists precisely because `Ast.hpp` enums are append-points every
   track touches).
3. **No measured hot spot.** No profile has shown AST allocation or teardown as a
   material fraction of compile time on the corpus or on the largest real program
   (`examples/curl`). Adopting an arena before that measurement exists is optimizing
   on vibes.
4. **The C++ front end has a shrinking horizon.** Per the portable pivot
   (`docs/techdesign-portable-backend.md:18-20`), the goal is *no C++ in code we
   author* — the compiler is to be rewritten in Leviathan (roadmap Phase 3+). Deep
   allocator surgery on the C++ AST is an investment in code the roadmap intends to
   shed. This does not make the arena worthless (self-hosting is 2027-01-gated and the
   C++ front end serves until then), but it correctly raises the bar for adoption.
5. **The process-lifetime AST makes teardown nearly free anyway.** The compiler is a
   batch process; the tree lives until exit. The main RAII costs are per-node malloc at
   parse and the destructor walk at exit — both bounded, neither observed to matter.

### 1.3 Do not conflate: the A-M6 *runtime* arena (landed) vs. this *AST* arena (deferred)

The word "arena" names **two different things** in this codebase, and one of them is
no longer deferred:

- **The runtime per-frame arena tier** — scope-owned *value-struct* allocation
  reclaimed at frame exit. Its deferral was logged at A-M5 as the two `XFAIL-LLVM`
  churn markers (`returned_value_struct`, `struct_array_field`), owner-ratified, with
  closure explicitly scheduled for A-M6
  (`docs/techdesign-portable-backend.md:1082-1097`). A-M6 then **implemented it and
  closed Gate G1** (`techdesign-portable-backend.md:1149` onward; `lv_objnew_arena` /
  `lv_copyval_arena` over the frozen `lvrt_halloc(size, LV_TIER_ARENA)` primitive).
  That deferral is **resolved**; nothing in this document reopens it.
- **The AST arena (this deferral)** — compile-time allocation of parser/checker/rules
  nodes. Entirely separate memory, separate lifetime, separate code.

The A-M5→A-M6 arena is cited here for two reasons. First, as the **model of a deferral
run correctly**: logged where it bit, marked with an explicit expected-failure marker,
owner-ratified, scoped to a named future milestone, then actually closed. Deferral A
should follow the same shape when its trigger fires. Second, as a **searchability
hazard**: `grep -rn arena` now hits the landed runtime tier all over `LlvmGen.cpp`,
`lv_abi.h`, and the design logs. Any future work item for this deferral must say
"**AST arena**" explicitly in its title and commits, or an implementation agent will
land in the wrong subsystem.

### 1.4 Resolution path (when the trigger fires — not now)

**Phase A0 — measure (mandatory gate before any code).** Heap-profile a full compile
(parse → rules → resolve → check → lower) over the corpus and over `examples/curl`,
plus one large synthetic `.lev` file (~100k lines). Record: node count by kind, malloc
count and bytes attributable to node allocation vs. `std::vector` internals vs.
everything else, and wall-time share of allocation + exit teardown. **If node
allocation + teardown is under the adoption threshold (§1.7), stop here and re-log the
deferral with the numbers.** The measurement is a deliverable even when the answer is
"still no."

**Phase A1 — centralize creation (safe, semantics-free, could even land early).**
Route the `Rules.cpp` and `Checker.cpp` direct `make_unique` node sites through the
same factory seam the parser already uses (move `mkExpr`/`mkStmt`/`mkType` to a shared
header, e.g. alongside `Ast.hpp`). After A1 there is exactly one place each node kind
is born. This phase is pure refactoring with no allocator change and is the only part
of the path that is low-risk enough to consider opportunistically — though even it
should wait for a lull in the compiler-track merge queue (§1.2 point 2).

**Phase A2 — the arena, `std::pmr` route (recommended shape).**
- Add an `AstArena` owning a `std::pmr::monotonic_buffer_resource`; one arena per
  compilation (or per `SourceFile`, see §1.5 on incremental reparse). It lives next to
  the `SourceFile` buffers, matching the existing "must outlive the views" rule.
- The factories placement-`new` nodes from the arena. The typedefs at `Ast.hpp:31-33`
  flip from `unique_ptr` to raw non-owning pointers in **one place** — which is
  exactly why A1 precedes A2 and why the typedefs (rather than spelled-out
  `unique_ptr`) were the right original call.
- Node-internal containers (`std::vector<...>` members throughout `Ast.hpp`) become
  `std::pmr::vector` drawing from the same resource, so subtree memory is *entirely*
  arena-backed and destructors can be skipped wholesale on arena drop.
- The virtual destructor stays (the Kind+vtable design is unrelated to ownership); it
  simply stops being *called* per node.
- Rule-expansion subtree replacement changes from "free immediately" to "becomes arena
  garbage until compilation end" — accepted and documented (§1.5 problem 2).

**Phase A2-alt — nodes-only bump arena (fallback if pmr churn is too invasive).**
Arena-allocate only the node objects; leave member `std::vector`s on the global heap
and keep a per-arena destroy-list so vector destructors still run at arena drop. Less
win (vector internals are plausibly the *majority* of allocations — A0 will say), much
smaller diff. Choose based on A0's malloc breakdown, not preference.

**Phase A3 — acceptance.** Full ctest suite green; ASan/UBSan sweep over a corpus
compile (the A-M4/A-M5/A-M6-precedented pipeline); re-run the A0 profile and record
the delta in this file's implementation log. No `.expected` changes possible or
permitted — this is allocator-only.

### 1.5 Potential problems

1. **Dangling references / lifetime inversion.** Resolver annotations (`Symbol`,
   `Scope` — `Ast.hpp:10-11`) and every downstream pass hold raw pointers into the
   tree. Under RAII the compiler *forces* ownership discipline; under an arena, a
   too-early arena drop is a mass use-after-free that ASan may only partially catch
   (arena memory can look "alive"). Poison-on-drop (memset + ASan manual poisoning) is
   the mitigation.
2. **Subtree replacement becomes garbage retention.** `Rules.cpp` splices and rewrites
   trees; replaced subtrees currently free immediately. Under a monotonic arena they
   are retained until compilation end. A pathological rule-expansion workload
   (metaprogramming Phase 3/4) could grow peak memory measurably. Mitigation: accept
   and measure (rule expansion is bounded in practice); never add per-node free to a
   monotonic arena to "fix" this — that rebuilds malloc badly.
3. **Destructor skipping is only safe if nothing non-trivial hides in a node.** The
   pmr route skips dtors; a future node member owning a real resource (file handle,
   global-heap `std::string`) would silently leak. Mitigation: a `static_assert`
   trait-check at the factory seam that whitelists node member types, so the
   assumption is compiler-enforced rather than tribal.
4. **Incremental reparse / future tooling.** A whole-compilation arena is hostile to
   an LSP-style "reparse one file" future: partial invalidation frees nothing. If
   tooling lands first, the arena granularity must be per-`SourceFile` from day one —
   this is a design fork A0/A2 must decide with the then-current roadmap in hand.
5. **Migration window.** A half-migrated tree (some nodes arena, some `unique_ptr`)
   is a double-free/UAF factory. The typedef flip makes migration all-or-nothing per
   build, which is the point — but it also means A2 is one large, serialized landing
   that must respect the overview §2 compiler-file merge-order rule.
6. **The "arena" name collision** (§1.3). Low-tech but real: the A-M6 runtime arena is
   now the dominant grep result. Naming (`AstArena`, "AST arena" in every commit) is
   the whole mitigation.

### 1.6 Recommended solutions (summary)

Sequenced phases with a mandatory measurement gate (A0); a single creation seam before
any allocator change (A1); pmr-backed monotonic arena with skipped dtors as the
end-state, nodes-only bump arena as the measured fallback (A2/A2-alt); poison-on-drop +
trait-check static_asserts for the two silent-failure modes; per-SourceFile granularity
decided at A0 against the tooling roadmap; explicit "AST arena" naming throughout.

### 1.7 Adoption triggers and STOP conditions

**Adopt (start Phase A0→A2) when ANY of:**
- **T-A1 (measured cost):** a profile shows AST allocation + teardown ≥ **10%** of
  end-to-end wall time compiling the corpus or a real project (`examples/curl` or
  larger). Below that, the win cannot repay the migration risk.
- **T-A2 (node-set stability):** the `Ast.hpp` condition is met — no compiler track
  in flight touches `ExprKind`/`StmtKind`/`TypeKind` for a full milestone cycle —
  **and** compile time has become a felt cost in the dev loop (subjective but must be
  logged with at least one timing).
- **T-A3 (self-hosting design input):** the Phase-3 compiler-in-Leviathan design work
  begins and wants the C++ front end's allocation story as its reference model — at
  which point building the arena teaches the self-hosted design something. (Weakest
  trigger; on its own it justifies A0+A1 only.)

**Do NOT adopt merely because:** the header comment says "deferred" (deferred ≠ queued);
an agent is idle; a micro-benchmark of malloc in isolation looks bad.

**STOP conditions during adoption:** A0 measurement can't be made reliable; A2
requires touching any frozen file; any ctest lane or the ASan sweep regresses and the
fix isn't obvious mechanical adaptation; peak-memory growth from problem 2 exceeds 2×
on any corpus program. Each of these is a log-and-escalate, not a workaround.

---

## 2. Deferral B — JIT execution tier

### 2.1 What exists (verified 2026-07-06)

`info.md` §17 defines **two user-facing variants with non-overlapping jobs**
(`info.md:1350-1357`): **AOT compiled** — "the real, optimized path… the answer for
'I need speed'" — and **interpreted** — "flexible, fast-iteration, REPL/scripting. It
does **not** need to be fast, because AOT already is." The deferral is then explicit
(`info.md:1359-1363`):

> **JIT is deferred, not adopted.** It is fast (and the shape/inline-cache system is
> already JIT-ready), but it is the highest-complexity option (deopt, tiering, runtime
> codegen) and, with AOT already present, it buys speed AOT largely already provides.
> It can be added as a later tier on the interpreter if a population that needs
> interpreter-flexibility-plus-throughput-without-AOT proves to exist.

Supporting facts on the ground:
- **One bytecode IR is the single semantic truth** (`info.md:1365-1371`); five engines
  consume it (`info.md:1378-1403`): oracle, IR interpreter, emit-C++, LLVM (full
  parity, Gate G1 closed), frozen ELF.
- The **IR interpreter** (`src/IrInterp.cpp`, 608 LOC) is a switch-dispatch register
  machine (`IrInterp.cpp:142,406`) — small, correct, differential-tested.
- The **AOT path is genuinely fast**: LlvmGen runs a `PassBuilder` O2 pipeline and
  inlines the measured fast paths (int/float `Arith`, `truth`/`Not`, fixed-offset
  field access, checker-resolved dynamic calls — `info.md:1394-1396`); A-M7 benchmarks
  put LLVM within ~1.36× of the frozen ELF backend's best
  (`docs/techdesign-portable-backend.md:1436-1463`).
- The **shape/inline-cache design** (`info.md:323-356`, §7): shape (hidden class) +
  packed typed slots, call-site inline caches, shape transitions, declared-slots-win.
  This is the classic JIT-enabling object model, hence "already JIT-ready" — *as a
  design*. Note the realization gap in §2.3, point 1.

### 2.2 Why it is deferred (the rationale, in detail)

1. **The speed job is filled.** A JIT's payoff is closing the interpreter→native gap.
   AOT already closes it, at full language parity, byte-identical to the oracle over
   the whole corpus. A JIT would buy speed the toolchain already sells.
2. **It is the highest-complexity feature class in compilers.** Deopt (materializing
   interpreter state mid-function), tiering policy, on-stack replacement, and runtime
   code emission each carry deep correctness tails; together they routinely dominate
   VM engineering budgets. Taking that on *without a proven customer* inverts the
   project's measurement-first discipline.
3. **The interpreter's contract is flexibility, not throughput** (`info.md:1356-1357`).
   Making the interpreter fast is not a stated goal; making it *correct* is — it is
   lane #2 of the differential harness that keeps every backend honest.
4. **Runtime codegen fights the portable pivot.** W^X policies, macOS `MAP_JIT` +
   per-thread write protection, hardened-runtime entitlements, Android's restrictions,
   and wasm32's *total* prohibition of runtime codegen are exactly the class of
   per-platform work the pivot exists to avoid — and wasm32 is on the roadmap, so the
   interpreter must remain the no-codegen answer there regardless. A JIT can never be
   the floor; it can only ever be a bonus tier.
5. **A sixth engine is a permanent tax.** Five execution paths already agree by
   construction via the corpus. Each additional engine multiplies differential-lane
   maintenance and the "every feature lands N times" cost that the single-IR design
   works hard to contain.

### 2.3 Potential problems (both while deferred and if adopted)

1. **The "JIT-ready" claim is about the design, not the interpreter as built.** The
   interpreter represents object fields as name-keyed hash maps
   (`IrInterp.cpp:250-260, 450-463` — `obj->fields.find(...)`), not the §7 shape +
   packed-slot layout; `src/Ir.hpp:14` is explicit that shape offsets are a
   *backend* lowering. A JIT tier that wants the shape/IC fast path therefore has a
   hidden prerequisite: **unify the interpreter's heap/value model with runtime v2's**
   (LvValue + shaped objects), so interpreted frames and jitted frames can share
   objects mid-tier-switch. This is the single largest unbudgeted cost and the main
   reason "just bolt on a JIT" underestimates by a lot.
2. **Deopt correctness.** Any speculation (shape guards, IC-monomorphic fast paths)
   needs side exits that reconstruct interpreter state exactly. Bugs here are
   heisenbugs by nature (only manifest at tier boundaries under specific warm-up).
3. **Tiering thresholds.** Wrong thresholds either burn compile time on cold code or
   never fire; they interact with workload shape and need empirical tuning
   infrastructure the project doesn't have yet.
4. **Security/portability of emitted code** (§2.2 point 4). Also: pulling LLVM into
   the *interpreter's runtime* footprint (binary size, startup latency) if the ORC
   route (§2.4) is taken — acceptable for a dev tool, needs a stated call.
5. **Second (sixth) codegen path maintenance.** Every future language feature must
   land in the JIT too, or tier-up silently changes behavior — the exact failure mode
   the differential corpus exists to catch, so the corpus must grow a forced-tier-up
   lane on day one.
6. **Deferral-period rot risk (the problem with waiting):** interpreter-internal
   choices (the map-based heap, value representation) can drift further from
   runtime v2's model, raising the §2.3.1 unification cost over time. This is a real
   cost of deferring — accepted, because the trigger population remains hypothetical.

### 2.4 Resolution path (when the trigger fires — not now)

The path that respects every standing constraint (X64Gen frozen; LLVM is the
sanctioned dependency; one IR; ABI contract frozen):

**Phase B0 — prove the population and the gap (mandatory gate).** Identify the real
workload class per `info.md:1361-1363`: interpreter-flexibility + throughput, where
AOT is *structurally* unusable (REPL sessions, rule-heavy dev loops, embedded
scripting) — not merely unconfigured. Measure it: interpreter vs. `--build-native` on
the same programs. **Both** halves of trigger T-B1 (§2.6) must hold, with numbers
logged here, before any code.

**Phase B1 — heap/value unification (the real prerequisite).** Move `IrInterp` onto
runtime v2's LvValue model and heap (calling the same `lvrt_*` entry points LlvmGen
emits calls to), retiring its private map-based objects. This is independently
valuable (deletes a parallel object model; tightens differential testing) and is the
*only* phase worth considering ahead of the trigger — and even then only with an
owner decision, since it touches lane #2 of the harness. Oracle (`Eval.cpp`) is
untouched — it remains the reference semantics and never tiers.

**Phase B2 — profiling hooks.** Per-function invocation counter + loop back-edge
counter in the `IrInterp` dispatch loop, compiled out by default. Dispatch already
flows through a function table; add a per-function `code` pointer, null = interpret.

**Phase B3 — Tier-1 baseline JIT via ORC LLJIT (no new lowering code).** Reuse
`LlvmGen`'s existing IR→LLVM-IR lowering (the ~2,000-LOC full-parity backend) and hand
the module to ORC `LLJIT` instead of `TargetMachine::emitObject`. Because Tier 1 is
the *same lowering* as AOT — same runtime calls, same ABI, `-O0` or cheap passes —
there is **no speculation and therefore no deopt** in Tier 1: a jitted function runs
to completion with the same semantics; tier-up happens at call boundaries (next call
of a hot function dispatches to the code pointer). No OSR in Tier 1 — a long-running
cold loop stays interpreted until its function is re-entered (accepted; OSR is Tier-2
scope). This choice is what makes the JIT a *thin* feature: feature work continues to
land in LlvmGen once, and both AOT and JIT inherit it.
- **Rejected alternative:** a hand-rolled template/splat JIT. It would be new
  per-ISA machine-code emission — the exact work class the pivot dropped and the
  X64Gen freeze forbids re-growing.

**Phase B4 — Tier-2 (speculative) — deferred even inside the deferral.** Only sketch:
shape-guarded IC fast paths with side exits at IR-op boundaries; the register-machine
IR makes frame materialization tractable (the interpreter's register file *is* the
canonical frame — a side exit writes live LLVM values back to `regs[]` and jumps to
the interpreter at the op index). Adopt B4 only if B3 lands and measurably still
misses the B0 workload's bar. Do not design B4 further until then.

**Phase B5 — acceptance.** New differential lane: the full corpus under
`threshold=0` (every function tier-ups immediately), byte-identical to the oracle —
the same standard every other engine met. ASan/UBSan sweep. W^X/`MAP_JIT` handling
comes from ORC, verified per-platform as targets come up; wasm32 documented as
interpreter-only forever.

### 2.5 Recommended solutions (summary)

Population-and-gap measurement gate (B0); heap unification as the honest prerequisite,
owner-gated (B1); counters behind a compile-time flag (B2); Tier-1 = LlvmGen + ORC
LLJIT with **no speculation and no deopt**, tier-switch at call boundaries only (B3);
speculation/OSR explicitly out of scope until Tier 1 proves insufficient (B4);
forced-tier-up differential lane from day one (B5). The oracle never tiers and never
changes.

### 2.6 Adoption triggers and STOP conditions

**Adopt (start B0, then B1→B3) when ALL of T-B1 hold, or T-B2:**
- **T-B1 (the info.md criterion, quantified):** (a) a *real, named* workload exists
  (users or the project's own dev loop) that must run under the interpreter for
  structural reasons — REPL, runtime rule loading, embedding — **and** (b) that
  workload measures ≥ **5×** slower interpreted than the same logic AOT-compiled,
  **and** (c) the workload's owner confirms AOT/`--build-native` genuinely cannot
  serve it (not merely "wasn't tried"). All three, logged with numbers.
- **T-B2 (strategic):** an owner decision adopts a product direction (e.g. a hosted
  playground or plugin ecosystem) where tiered execution is a stated requirement.
  Owner decisions supersede measurement gates by definition — but B0's measurements
  still get taken first to size the work.

**Do NOT adopt merely because:** the interpreter loses a benchmark to AOT (it is
*supposed* to — `info.md:1356-1357`); ORC makes Tier 1 look cheap (the cost center is
B1 and the permanent sixth-engine tax, not B3); another language has one.

**STOP conditions during adoption:** B1 turns out to require oracle (`Eval.cpp`)
semantic changes — that is bug.md + escalation territory, never a tier-work side
effect; any needed runtime ABI addition (doc-2 §2 is frozen — STOP, as with the
inline-throw-flag precedent); any temptation to reference or extend X64Gen for
emission; the forced-tier-up lane cannot reach byte-identical and the divergence
isn't a mechanical Tier-1 bug.

---

## 3. Dependencies

| Item | Depends on | Blocked by |
|---|---|---|
| A — Phase A0 (measure) | nothing (profiling only) | — |
| A — Phase A1 (factory seam) | a lull in compiler-track merges (overview §2 order rule) | active `Ast.hpp` enum churn (Tracks 03/07) |
| A — Phase A2 (arena) | A0 numbers over threshold, A1 landed, node-set stability (T-A2) | trigger not fired |
| B — Phase B0 (measure) | a candidate population to measure | trigger not fired |
| B — Phase B1 (heap unification) | runtime v2 (landed); **owner decision** (touches harness lane #2) | B0 |
| B — Phase B3 (Tier-1 JIT) | B1, B2; ORC LLJIT in the sanctioned LLVM dep; LlvmGen parity (landed, G1) | B0/B1 |
| Both | three-branch rule (work on existing branches only); `.lev` sources for any new test programs | — |

Neither deferral depends on the other; they may resolve years apart or never.

---

## 4. Closing statement

Both deferrals are the **correct current state**, not debt: RAII is the right AST
ownership until the node set stabilizes and a profile says otherwise, and AOT + a
correct interpreter is the right execution story until a real population needs a
middle tier. The failure mode this document guards against is not "we never built
them" — it is (a) building them without their triggers, and (b) losing the rationale
and rebuilding the analysis from scratch when a trigger *does* fire. When one fires,
the adopting milestone starts from this file's path (§1.4 / §2.4), logs its
measurements here, and follows the A-M5→A-M6 arena precedent: mark, ratify, schedule,
close.

---

## 5. Implementation log

### 5.1 Phase A0 measurement — 2026-07-20 (Deferral A, mandatory gate)

**Verdict: trigger T-A1 NOT fired. AST-node allocation + teardown ≈ 1.84% of
instruction cost, vs. the ≥ 10% adoption threshold — short by ~5×. Per §1.4, the
deferral is re-logged with numbers and no code was written. This file stays live.**

**Method.** `perf` was unavailable (`kernel.perf_event_paranoid=4`, no root), so
attribution was taken with `valgrind --tool=callgrind` (deterministic instruction
counts, a sound proxy for the wall-time share T-A1 asks about) plus `/usr/bin/time`
for wall/RSS. Compiler rebuilt at merge `76794f0` (today's master). `examples/curl`
named in §1.4 no longer exists in the tree; the largest real programs are now
`examples/helm` and `examples/recon` (≤ 489 LOC/file). A synthetic
`~99k-line` `.lev` (1,500-class stress file: fields + methods + loops + branches)
stood in for the "~100k-line" input; a `~13k-line` variant was used for the callgrind
run (instruction *share* is input-size-independent).

**Wall time / memory (99k-line synthetic).**
| Pipeline | Wall | Peak RSS |
|---|---|---|
| parse → resolve → check (`leviathan <file>`) | 0.50 s | 311 MB |
| parse → check → lower to IR (`--ir`) | 1.68 s | 351 MB |

**Instruction-share attribution (callgrind, 553.9M I-refs total).**
| Bucket | Share | Addressed by an AST arena? |
|---|---|---|
| AST-node object allocation (`make_unique<Expr/Stmt/TypeRef>` paths) | **0.73%** | yes (A2/A2-alt) |
| AST-node teardown (`~Expr`/`~Stmt`/`~TypeRef` + vtable delete) | **1.11%** | yes (A2 skips dtors) |
| **AST-node alloc + teardown (T-A1 numerator)** | **≈ 1.84%** | — |
| *All* malloc/free-family (incl. Token/IR-`Inst` vectors, hashtables, strings) | 13.09% | mostly **no** — non-AST vector/hashtable/string churn a node arena does not own |

**Why the full alloc-family 13% is not the arena's win.** The arena only removes
per-AST-node malloc and the AST destructor walk (the 1.84%). The bulk of the 13% is
`std::vector<Token>` growth, the IR `std::vector<Inst>` output, symbol-table
hashtables, and `std::string` — none AST-node-owned. Even the full `std::pmr` route
(A2) captures only node-*member* vectors, a minority of that 13%; the nodes-only
fallback (A2-alt) captures essentially just the 1.84%. So no arena variant approaches
the 10% bar.

**Incidental finding (not this deferral's scope).** The genuine hot spots at `--ir`
are algorithmic, in `src/Lower.cpp`, and untouched by any arena work:
`Lowerer::isBaseOrSelf` **≈ 25%** (23.50% + 1.41% second instantiation) and
`Lowerer::packedSlot` **11.0%** — together ~36% of instruction cost, both symbol-graph
walks. If compile time ever becomes a felt cost, *these* are the targets, not
allocation. Logged here as an observation only; no ticket filed and no change made
(out of scope for this document, which covers the AST-arena and JIT deferrals).

**Node-set stability check (T-A2).** Also unmet: the node-creation surface has *grown*
since 2026-07-06 (Checker.cpp direct `make_unique` node sites went from 2 to ~50+;
Rules.cpp from ~12 to ~15) and `ExprKind`/`StmtKind`/`TypeKind` remain active
append-points across in-flight tracks (metaprog splices merged today at `76794f0`).
Phase A1's "wait for a merge-queue lull" precondition is therefore also not satisfied.

**Outcome.** Stop at A0. No A1/A2 code written. Deferral A remains the correct current
state; re-measure when either T-A1 (a real project profiles ≥ 10%) or T-A2
(node-set freezes for a full cycle *and* compile time is felt) fires.

### 5.2 Phase B0 — 2026-07-20 (Deferral B, JIT)

**Verdict: no candidate population exists to measure; T-B1 and T-B2 both unmet;
nothing autonomously buildable. Deferral B remains deferred.**

B0 requires a *real, named* workload that must run under the interpreter for
structural reasons (REPL, runtime rule loading, embedding) **and** whose owner
confirms AOT/`--build-native` structurally cannot serve it (T-B1a/c) — an owner input,
not a profiler run. None is on record. Every buildable B-phase is gated upstream: B1
(heap/value unification) explicitly needs an **owner decision** (touches differential
harness lane #2), and B3 (Tier-1 ORC JIT) depends on B1. T-B2 (a strategic owner
decision adopting tiered execution as a product requirement) is likewise not present.
No code written; no way to proceed without owner direction.

### 5.3 Document disposition

Per §0.1 / §4, this file is *not* moved to `designs/complete/` — that move happens
only when a deferral is **adopted and landed**. Neither trigger fired, so the file
stays live as the rationale of record, now carrying the 2026-07-20 measurements.
