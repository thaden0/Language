# LA-30 True Suspension — Semantics, Corpus, and the Flip (doc 5 of 6)

**Owns:** `docs/reference.md` + `info.md` patches, `tests/corpus/tasks/*`, the Atlantis
kernel handoff list, and (coordinated) the `fork` grammar removal in
`src/{Token,Ast,Parser}.*`. **Depends on:** M1-M4 (flip is M5). **Status:** PROPOSED.
**Gotchas owned:** G2, G3, G13.

---

## 1. The observable contract (normative — this text lands in reference.md)

Replacement for the `await expr` row in §3.2 (delete `[runtime: planned]`), plus a new
§6.6.67 "Await and tasks":

> **`await expr`** — suspension point. If `expr` is not a promise-shaped object, `await`
> yields it unchanged. If the promise is already settled, `await` proceeds **without
> suspending** (no scheduling point — deterministic and free; the C# completed-task
> rule, not JS's always-defer rule). Otherwise the current **task** suspends: other
> tasks on the same thread — including new work the event loop dispatches — run while it
> waits, and the task resumes after the promise settles, **in completion order** among
> this thread's runnable tasks. Resumption order is FIFO per thread; it is **not** the
> reverse-of-suspension (stack) order the pre-LA-30 pump implied, and programs must not
> depend on either beyond what this paragraph states.
>
> A task is the unit of suspension: the program top-level is a task, and every callback
> the event loop dispatches (timer, watch, worker-join) runs as a task. Tasks are pinned
> to their thread; `await` never moves work across threads (`Channel<T>`/`Worker<T>`
> remain the only crossings — §6.6.66). Across any `await`, state on the current thread
> may be observed and mutated by other tasks that ran meanwhile — single-threaded
> interleaving is real even without data races; `await` marks exactly the points where
> it can happen.
>
> **Failure at an await.** A `Worker` that rejected rethrows its failure at the `await`
> (catchable — §6.6.66, unchanged). An `await` whose promise can no longer be settled —
> the thread's event loop has fully drained with the promise still pending — throws a
> catchable `RuntimeException`: `await: event loop drained with promise unresolved`.
> An uncaught throw inside a loop-dispatched callback terminates the program through
> the standard uncaught path; it is never delivered to an unrelated `await`.

And the coverage note (§7.3 style): oracle, IR, LLVM — full; emit-C++ — no async surface
(doc 4); ELF — frozen, not a target.

## 2. The three corrections, with repros

### C1 — completion-ordered resumption (was: innermost-pump order)

```lev
// tasks/order_two_awaits.lev
var slow = Promise<int>();
var fast = Promise<int>();
std::sysTimerStart(30, 0, (n) => { slow.resolve(1); });
std::sysTimerStart(10, 0, (n) => { fast.resolve(2); });
spawn(() => { console.writeln("A" + (await slow).toString()); return 0; });
spawn(() => { console.writeln("B" + (await fast).toString()); return 0; });
```

Pump (interpreters; cooperative spawn order): A's await pumps; inside it B starts,
awaits `fast`, which resolves first — but B sits **below** A on the pump stack only if A
pumped it… and A cannot resume until B's nested pump unwinds. Output hinges on stack
shape: `B2` then `A1` here, but flip the timer durations across a nesting boundary and
the *later*-completing promise can print first. Tasks: strictly `B2` (fast, 10ms) then
`A1` (slow, 30ms) — completion order, always. The corpus asserts the tasks order; the
pump's output for this file is recorded in the M1-M4 divergence list, not asserted.

### C2 — failure routing (was: rethrow at whichever await pumps innermost)

```lev
// tasks/throw_uncaught_callback.lev
var p = Promise<int>();                      // nothing ever resolves p
std::sysTimerStart(5, 0, (n) => { throw RuntimeException("boom"); });
try { int x = await p; } catch (IException e) {
    console.writeln("caught: " + e.message);  // PUMP: reaches here (!) — spooky
}
```

Under the pump, the timer callback runs inside `await p`'s pump loop; its throw breaks
the pump (`Eval.cpp:1084-1088`'s `if (throwing_) break`) and unwinds into a `catch`
that has nothing to do with the timer — an exception teleported into an unrelated
handler by stack accident. Under tasks the callback is its own task: the throw is
program-uncaught (`Uncaught RuntimeException: boom`, exit 1 — exit-codes contract), and
`await p` subsequently drains (C3). The corpus asserts the tasks behavior; this repro
goes verbatim into the reference.md §6.6.67 text as the canonical "why".

### C3 — drained-loop await (was: silently yield the default value)

```lev
// tasks/drained_await_throws.lev
var p = Promise<int>();
int x = await p;                 // loop is empty; nothing can ever resolve p
console.writeln(x);
```

Pump today: `hasWork()` is false → break → read `value` → prints `0` — **fabricated
data**, the exact disease `None` exists to cure (info.md §9), silently. Tasks (M5):
catchable `RuntimeException: await: event loop drained with promise unresolved`;
uncaught here → exit 1 with the standard banner. Worker variant
(`tasks/drained_worker_rejects.lev`): a spawned body that awaits a dead promise gets the
same throw *inside* the body → `spawn`'s existing `catch` → `reject` → rethrows at the
spawner's `await w` (doc 2 §7 — no new plumbing). Both shapes asserted.

The bug-for-bug equivalence claim used through M4 (parity mode): pump's per-iteration
`hasWork` check ↔ one drain-wake per park; a program that loops re-awaiting a dead
promise never terminated under the pump either. Divergence is therefore *only* the M5
throw vs. the silent default — enumerated, deliberate, and corpus-pinned.

### C4 (documented, not a change) — ready-await never yields

`await` on a settled promise does not enter the scheduler (doc 1 §6). Consequence spelled
out in reference.md: a loop of ready-awaits is compute, not yielding — fairness among
runnables starts at the first real suspension. (Same as the pump; stated because C1's
"FIFO" sentence would otherwise imply a yield that doesn't exist.) Corpus:
`tasks/ready_no_yield.lev` asserts strict sequential completion with zero switches
(`[tasks] switches` delta = 0 for the ready-chain segment).

## 3. Unchanged-behavior inventory (asserted through M4, kept at M5)

`await` non-promise = identity • comptime `await` = compile error (wording unchanged) •
`then()` runs synchronously in the resolver's context • Worker reject carrier =
`failed`+`failMessage` (LA-19 untouched — §7) • loop dispatch order (fds in fd order,
then timers in due/creation order) • cooperative-worker spawn order on the interpreters
(`threads_serial/` byte-identical) • Node-style implicit drain (program ends when no
work AND no runnable/parked-settleable task remains) • exit codes • `+0B` churn.

## 4. Corpus plan (`tests/corpus/tasks/` — new files are `.lev`, per the hard rule)

| file | asserts |
|---|---|
| `order_two_awaits.lev` | C1 completion order (repro above) |
| `order_fifo_three.lev` | 3 promises settling out of park order; FIFO resumption |
| `throw_uncaught_callback.lev` | C2 (expected: uncaught banner + exit 1; runner checks both) |
| `drained_await_throws.lev` / `drained_worker_rejects.lev` | C3 both shapes |
| `ready_no_yield.lev` | C4; `[tasks] switches` delta 0 |
| `interleave_channel.lev` | producer/consumer over `Channel<T>` interleaving at awaits only |
| `nested_spawn_join.lev` | worker inside worker; joins rethrow correctly at each level |
| `recursion_depth.lev` | doc 3 §5 depth probe (interpreter stack sizing) |
| `park_storm.lev` | 10k parked tasks settle in waves; order + `+0B` + stats sanity |

Rules: **G13** — corpus asserts contract-level facts only (never incidental
interleavings; if an assertion needs to know pump internals to predict, it is wrong).
Every file runs three-engine differential (oracle=IR=LLVM byte-identical) from M3.
Existing corpus: full run in both modes at every milestone; divergences must be exactly
the C1-C3 files quarantined in `tasks/DIVERGENCES.md` (a tracked list with per-file
pump-output vs tasks-output), which M5 deletes when the corpus flips to tasks-expected.

## 5. Flip mechanics

> **⟦SENSITIVE-GATE — ENTER S5⟧** Model-gated (overview §12) — the M5 flip is the one
> milestone that changes observable behavior on purpose, and a silently-diverged cross
> lane poisons differential testing indefinitely. **STOP before performing the flip**
> (through the EXIT gate at the end of this section). Declare
> `SENSITIVE SECTION ENTERED: S5 — M5 flip. Halting for model escalation; not writing fenced code.`
> and hand back. Escalated model only. M1–M4 bring-up below this track is ordinary work
> under §11; only the M5 flip step itself is fenced.

- **M1-M4:** `LANG_TASKS=1` opt-in; parity mode ON in tasks path (C3 silent, C2
  quarantined); both modes in CI lanes.
- **M5:** default inverts; `LANG_PUMP=1` escape hatch (maps to the old path verbatim);
  parity mode OFF (C3 throws); `tasks/DIVERGENCES.md` resolved into flipped expectations;
  reference.md/info.md patches land (this doc §1 + §6); cross lanes (qemu/wine) either
  run tasks or are pinned to `LANG_PUMP` **explicitly in the lane script with a comment
  naming the port milestone** (never silently divergent — doc 1 §5).
- **M6 (after soak):** delete the pump everywhere — `lv_pump_until_ready` (doc 2 §3),
  `lv_entry.c`'s raw drain, `Eval.cpp`/`IrInterp.cpp` pump loops and drain loops,
  `LANG_PUMP`, parity branches. Acceptance: `grep -rn "pump" src/ runtime/` hits only
  history comments; corpus green with the flag code gone; `drained_wakes` stat retained
  (it now counts C3 throws — still a useful smell).

> **⟦SENSITIVE-GATE — EXIT S5⟧** On completing the M5 flip and passing S5's bar (flip
> commit is semantics-only; every lane green-on-tasks or explicitly `LANG_PUMP`-pinned
> with a port-milestone comment; perf table §9 met; §11 acceptance), **STOP.** Declare
> `SENSITIVE SECTION COMPLETE: S5 — M5 flip. <what landed + how verified>. Halting for review.`
> and hand back. M6 (pump deletion) is a separate, non-fenced deletion-only milestone —
> do not fold it into the flip.

## 6. Docs and downstream handoff

**reference.md:** §3.2 row + §6.6.67 (text in §1); §6.6.66 threads cross-ref sentence;
§7.3 coverage line (doc 4). **info.md:** §14's async paragraphs rewritten around tasks
(pump described in past tense as the v1 mechanism); §0 log line. **bug.md:** nothing —
C1-C3 are design corrections with a design doc, not bugs.

**Atlantis kernel handoff** (kernel doc owned by the Atlantis track — file this list,
don't edit their doc; disjoint-ownership convention): §8.1's "MANDATORY `await
nextTick()` between chunks" relaxes to latency hygiene (starvation now delays only
same-thread tasks, and B2 adds the kill switch); §7/§621's "wall-clock kill switch is
impossible until true workers" gains its real dependency (LA-30 B2, doc 6); §8.4's
"no coloring" claim gets a cite to reference.md §6.6.67; the `awaitTimeout` §5.4 utility
becomes implementable-with-teeth after B2 (until then it stops waiting, still can't
stop the work — unchanged text, new footnote).

## 7. LA-19 (promise rejection) — adjacent, not absorbed

`designs/requests/request-promise-rejection.md` asks for `reject(e: IException)` with
the exception object (not just `failMessage`) rethrown at the await, `then` semantics on
rejected promises, and a never-observed-rejection drain report. LA-30 gives it precise
routing (C2/C3) and the drain machinery for free; the remaining work is the prelude
carrier (store the `IException` value, not `e.message` — note the request's own warning
about bug.md #1's optional-field crash) and the drain report line. Recommendation:
close LA-19 as a one-milestone follow-up immediately after M5, on this substrate. Its
P6 probe ("does a callback throw propagate to the awaiter?") is answered by C2:
**it must not** — the probe's YES branch (docs-only close) is hereby ruled out; build
the `reject` path.

## 8. `fork(expr)` removal (surface hygiene riding M5)

`fork` is dead grammar: `Token.hpp:35 KwFork`, keyword-table entry, `Ast.hpp:120 Fork`,
`Parser.cpp:36` (expression-start set) and `:399-400` (primary), engines' "out of scope"
stubs, the reference.md §3.2 row (`fork(expr) [runtime: planned]`) and §1.3 keyword list.
Track 10's `spawn` is the landed reality and `fork` never acquired a runtime. Remove all
of it at M5 (grep-verify no corpus/example/demo uses `fork(` first — expected: none).
Coordination note: these are front-end files recently touched by LA-28 (`readonly`) —
rebase against master before the removal commit; the change is mechanical either way.

## 9. Performance acceptance (measured, both at M4 and re-run at M5)

| gate | target | method |
|---|---|---|
| context switch | ≥ 20M/s x86-64 asm | selftest bench (doc 1 §9.2) |
| spawn/complete | ≥ 2M/s pooled | selftest #3 variant |
| existing corpus wall-clock | within ±2% of pump per lane | `run_llvm_timed.sh` A/B |
| 10k parked tasks | ≤ 100 MiB RSS over baseline | `park_storm.lev` + `/proc/self/status` VmHWM; stats cross-check |
| http echo (sockets corpus server) | p50 within ±5%; p99 under concurrent nested awaits strictly better than pump | A/B harness, 3 runs, report medians |

The p99 line is the point of the whole track; if it does not improve, STOP and find out
why before flipping.

## 10. Predictable gotchas (owned here)

| trap | note |
|---|---|
| G3: someone "fixes" C2's repro to keep the pump's catch reachable | the misroute is the bug; the reference text in §1 is normative — corpus asserts uncaught |
| G2: parity mode leaking past M5 | `lv_parity_mode()` keys off the same flag flip; M6 deletes it; `drained_wakes` stat audits the interim |
| G13: order-asserting corpus | review rule: any expected-output that would change under a legal FIFO schedule is a corpus bug |
| flip commit mixes semantics + refactors | M5 is semantics-only (flag default, C-behaviors, docs, corpus); M6 is deletion-only; never merge the two |
| cross lanes silently on old semantics | §5's "explicit pin with port-milestone comment" rule |

## 11. Acceptance (M5)

Both docs patched; corpus flipped with `DIVERGENCES.md` deleted; all lanes green on
default-tasks (or explicitly pinned); perf table §9 met; kernel handoff filed;
LA-19 follow-up scheduled; `fork` gone.

**STOP:** any divergence outside the enumerated C-set at flip time; p99 regression (§9);
any pressure to flip before qemu/wine lanes are either ported or explicitly pinned.
