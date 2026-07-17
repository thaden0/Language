# Tech Design — Deterministic Step-Counter Gate for Regex Linearity

**Status:** design, ready for implementation. **Date:** 2026-07-16.
**Scope:** replace the wall-clock assertion in the `regex_pathological_linear` CTest gate with a
deterministic engine step counter, so the linearity property is proven independent of host speed
and CI load. Prelude + test change only — no backend, no ABI, no public-surface change.
**Ground truth:** `tests/run_regex_linear.sh`, `fuzz/regex_linear.lev`, `CMakeLists.txt:695-697`
+ `:713` (`TIMEOUT 10`), `src/Resolver.cpp:3434-3620` (`RegexCoreVm` — the in-language engine),
`designs/complete/techdesign-regex-library.md` (the landed Track 10 surface).

---

## 0. What and why

`regex_pathological_linear` exists to prove one algorithmic property: the Pike NFA leg (the
worst-case-guarantee path behind the lazy DFA) is **linear in input length** — `(a+)+$` against
`"a"*n + "X"` must scale ~2× when `n` doubles, never explode the way a backtracking engine would.

Today it proves that with **wall-clock milliseconds** (`std::sysMonotonic` deltas in
`fuzz/regex_linear.lev`, ratio-checked by `run_regex_linear.sh`: `t400 <= t200*3 + 50`) under a
10-second CTest `TIMEOUT`. That makes a *scaling* gate into a *throughput* gate, and it is fragile
in exactly the observed ways (2026-07-16 spawn-landing test sweep):

- On the current host the probe takes ~17 s of pure interpreted matching against the 10 s cap —
  a hard fail with a **correct** engine (the printed ratios were cleanly linear: 885→1782→3478 ms).
- Under a parallel `ctest -j$(nproc)` run, co-scheduled 600 s corpus lanes starve it further.

Measured decomposition (same probe, same engine code): oracle ~8.7 ms/byte, IR ~6.3 ms/byte,
LLVM native ~0.11 ms/byte. The cost is interpreter tax on a deliberately in-language engine plus
the Pike leg's pure/COW array churn — all *by design* (the engine is a no-natives, comptime-
foldable library; throughput belongs to the lazy-DFA path and the compiled backends). The gate
should therefore measure what the design guarantees — **work done**, not **time taken**.

**The fix:** count engine steps. Step counts are pure language semantics — deterministic for a
given (pattern, input) on every engine, immune to host speed, load, valgrind, and sanitizers.
Linearity becomes an exact arithmetic fact the gate checks in ratios.

**Non-goals:** any change to match semantics or the public `Regex`/`namespace regex` API surface;
any speedup of the Pike leg itself (worthwhile, separate); the lazy-DFA path (not what this gate
exercises); recalibrating other timed gates.

---

## 1. Design decisions

**D1 — What a "step" is: one (pc, position) examination in the Pike leg.**
A single monotonically-increasing counter field on `RegexCoreVm` (`int pikeSteps`), incremented at
exactly two sites, both inside the class:

1. `closureFlat` (`Resolver.cpp:3561`) — once per stack pop (each `(pc, caps)` popped and
   examined in the epsilon-closure walk);
2. `pikeFindRaw`'s per-byte transition loop (`Resolver.cpp:3603-3611`) — once per thread
   transition test (each `state.at(ti)` examined against the current byte).

Together these dominate the leg's work and are exactly the quantity the Thompson/Pike linearity
theorem bounds: steps ∈ O(n·m) for input length n and program size m. Any consistent monotone
count of examinations works for a ratio gate; these two sites are the canonical ones. The `seen`
dedup already caps closure pops at O(m) per position, so a superlinear regression (e.g. losing
the dedup, or re-seeding threads that should have been merged) shows up directly in the ratio.

**D2 — Surface: one new engine-internal free function, `regex::programPikeProbe`.**
```
// diagnostics/testing only: run the Pike leg from 0 and report work done.
// [0] = matched (0/1), [1] = steps (D1's counter). NOT part of the public
// Regex surface; sibling of the existing engine internals the corpus already
// calls directly (compileProgram, programIsMatchPike).
Array<int> programPikeProbe(Array<int> program, string s);
```
It constructs the `RegexCoreVm`, runs the same Pike entry `programIsMatchPike` uses, and returns
`[hit, pikeSteps]` — one array because the gate needs both facts and must not run the (expensive)
match twice. No counter is exposed through the `Regex` class, and the DFA path is untouched.

**D3 — Gate shape: self-calibrating deterministic ratio; wall-clock demoted to hang-guard.**
`fuzz/regex_linear.lev` probes n = **50/100/200** (down from 100/200/400 — with a noise-free
counter, larger inputs prove nothing more) and prints `n:steps:hit`. `run_regex_linear.sh` asserts:

- no probe reports a match (`hit` must be 0 — the existing `:true` check, unchanged in spirit);
- `steps200 <= 2*steps100 + steps50` — doubling the input at most doubles the work, with the
  smallest probe itself as the O(1)/startup slack term (self-scaling; no magic constant like
  today's `+50 ms`).

**Engine choice (settled at implementation):** because the step count is engine-independent
(byte-identical on `--run`/`--ir`/LLVM — verified 507/1007/2007), the gate runs on the fastest
**no-build** engine, the IR interpreter (`--ir`), ~2.8× the tree-walk oracle. This is semantically
free — the same assertion on the same numbers — and is only possible *because* the redesign
removed the wall-clock dependence. (The oracle's fixed per-op interpretation cost, not the
match-step count, dominates the in-language engine's runtime — shrinking inputs alone left the
oracle at ~14 s, so the engine switch, not the size cut, is what buys the margin.)

CTest `TIMEOUT` **stays as a pure hang-guard** but is set to **30 s** (`CMakeLists.txt:713`), not
the old 10 s: with correctness now on the deterministic ratio, the timeout's only job is to catch
a genuine superlinear/exponential blow-up (which balloons far past 30 s), so it is sized for
generous margin over the ~5 s linear `--ir` run rather than as a tight throughput judge. The old
10 s was a throughput judge in disguise — the exact conflation this redesign removes.

**D4 — The counter is unconditional.** One integer field increment per examination, no flag, no
mode. Cost is a single add in a loop already doing array allocation and bounds-checked reads —
unmeasurable on every engine, and a conditional would itself cost more than the add.

**D5 — Ratios only; no absolute-count goldens.** Step counts are identical across all four
engines by the differential doctrine (ordinary language semantics), but pinning absolute values
in a `.expected` would force a regold on every legitimate engine refactor. The gate checks the
ratio property only; cross-engine agreement is already enforced wholesale by the existing
`regex_engine` corpus lanes.

**Rejected alternatives:** raising `TIMEOUT` *as the fix* (a bigger throughput budget is still a
throughput judge — it treats the symptom; note D3 raises it only as a hang-guard *behind* the
deterministic ratio, which is a different role); running the *timed* probe on the LLVM lane
(faster, but still wall-clock, and adds a native build to a gate that should stay cheap — distinct
from D3's choice to run the *deterministic* probe on `--ir`, which carries no timing dependence);
an env-gated step budget inside the engine (production machinery for a test-only concern).

---

## 2. Changes, file by file

| File | Change |
|---|---|
| `src/Resolver.cpp` (prelude, `RegexCoreVm`) | `int pikeSteps = 0;` field; `pikeSteps = pikeSteps + 1;` at the two D1 sites; `programPikeProbe` free function beside `programIsMatchPike` in `namespace regex` |
| `fuzz/regex_linear.lev` | probe sizes 50/100/200; call `programPikeProbe` once per size; print `n:steps:hit`; drop the `sysMonotonic` timing |
| `tests/run_regex_linear.sh` | run on `--ir` (engine-independent counts → fastest no-build engine); parse steps from lines 1-3; assert `hit == false` per line and `steps200 <= 2*steps100 + steps50`; failure message prints the counts |
| `CMakeLists.txt` | `TIMEOUT 10` → `30` (hang-guard only; see §1 D3) |
| `docs/reference.md` §6.4.4 | one line: `programPikeProbe` listed among the engine internals as diagnostics-only |

No `lv_abi.h`, no backend, no `Op`, no public class — nothing STOP-adjacent.

---

## 3. Testing

1. **The gate itself** — `ctest -R regex_pathological_linear` green on the slow host, standalone
   and under a full parallel `ctest -j$(nproc)` (the two conditions that failed 2026-07-16).
2. **Determinism** — run the probe twice; identical `steps` output byte-for-byte. Run on `--ir`
   once manually; identical to `--run` (D5's cross-engine claim, spot-checked).
3. **The gate still bites** — one-off manual check during landing (not committed): weaken the
   closure dedup (`seen`) locally and confirm the ratio assertion trips. This proves the counter
   measures the property, not just decorates it.
4. **No behavior drift** — the full `regex_engine` + `regex` corpus lanes stay green on all four
   engines (the counter must not perturb match results; it is write-only state).

---

## 4. Risks & mitigations

| # | Risk | Mitigation |
|---|---|---|
| R1 | Counter placement drifts as the engine evolves (a refactor moves the hot loop) | the counter is a per-examination count, not a line anchor — any faithful move keeps the ratio property; test #3 re-run on any Pike-leg refactor |
| R2 | Ratio slack too tight for some future pattern family added to the probe | slack term is the smallest probe's own count (self-scaling); a new family gets its own three sizes and the same formula |
| R3 | Someone reads `programPikeProbe` as public API | doc comment + reference.md marks it diagnostics-only, same status as the other engine internals |
| R4 | int overflow | counts are ~10⁴ per probe; language int is 64-bit — not reachable |

---

## 5. Landing order

Single gate, one commit: prelude counter + probe function → fuzz/script flip → reference.md line
→ verify §3 items 1/2/4 (item 3 is a local-only check during review). Estimated size: ~30 lines
of prelude, ~15 of test.

---

## 6. Implementation log (append-only)

- 2026-07-16 — design created, motivated by the spawn-landing test sweep (the gate failed twice
  on a correct engine: host-speed margin standalone, contention under parallel ctest). Decisions
  D1–D5 recorded.

- 2026-07-16 — LANDED. Implemented as specified; step counts came out cleanly linear
  (`10n+7`: 507/1007/2007) and byte-identical on `--run`/`--ir`. Two facts discovered at
  landing, both folded into §1 D3 / §2: (a) shrinking the probe sizes alone did NOT get the
  oracle under 10 s (~14 s — the cost is the oracle's per-op interpretation of the in-language
  engine, not the match-step count), so the gate runs on `--ir` (~5 s), which the deterministic-
  count design makes free; (b) the CTest TIMEOUT is now a pure hang-guard, raised 10→30 s. The
  §3.3 "gate still bites" check was run: an injected O(n^2) cost at a counted site produced
  1782/6057/22107 and tripped the ratio assertion, then reverted.
