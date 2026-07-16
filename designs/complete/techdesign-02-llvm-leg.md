# LA-30 True Suspension — LLVM Leg (doc 2 of 6)

**Owns:** `src/LlvmGen.cpp` (the `Op::Await` case + entry emission), `runtime/lv_loop.c`,
`runtime/lv_entry.c`, `runtime/lv_thread.c`, `runtime/lv_runtime.c` (`lvrt_await`,
`lvrt_promise_state`, `[tasks]` stats), `runtime/lv_abi.h` (declarations).
**Depends on:** doc 1 (M0). **Status:** PROPOSED. **Gotchas owned:** G11, G15 (leg-local
detail), plus the state-audit table in §9.

---

## 1. Shape of the change

Today `Op::Await` lowers to an **inline pump BB graph** (`src/LlvmGen.cpp:2811-2855`):
tag-check → read `ready` via `rtGetfield` → `chkThrow` (`rtThrowing`) → `chkWork`
(`rtLoopHasWork`) → `pumpStep` (`rtLoopStep`) → … → `chkFail`/`failThrow`
(`lvrt_raise_str`, declared at `:282`) → read `value`.

Under tasks the entire graph collapses to **one runtime call plus the existing throw
check**:

```
call void @lvrt_await(%LvValue* %dst, %LvValue* %promise)
; then emitThrowCheck(pc)  — dispatch at THIS pc, oracle parity (LlvmGen.cpp:1210, :1929)
```

All pump/park logic moves into the runtime, where it can switch stacks. Generated code
gets *simpler*; the codegen diff is small and mechanical.

### History note — this reverses a recorded ruling, deliberately

The comment at `LlvmGen.cpp:2811-2814` records that an `lvrt_await` runtime call once
existed and was **removed** by the maintenance-pass-2 §8 correction in favor of the
inline pump (lineage: `X64Gen.cpp:3573-3593`). That ruling was correct *for pumping*: a
pump is just a loop over two ABI calls, and inlining it removed a call and a runtime
entry with no semantic payload. Parking is different in kind — it must run on the
scheduler's stack discipline and cannot be expressed as inline BBs at all. Re-introducing
the runtime entry is not un-learning the old lesson; the old lesson's premise (await ≡
a loop you could inline) is what LA-30 removes. Record this in the code comment so the
next maintenance pass doesn't "restore" the inline form.

## 2. New ABI surface (`runtime/lv_abi.h`)

```c
/* LA-30. Generated code calls ONLY lvrt_await; the rest is runtime-internal. */
void lvrt_await(LvValue* dst, const LvValue* promise);
```

Runtime-internal (not in `lv_abi.h`; `lv_runtime.c` / `lv_loop.c` / `lv_task.h` only):

```c
int  lvrt_promise_state(const void* obj);   /* poll hook: bit0 ready, bit1 failed */
int  lvrt_loop_step_tasks(void);            /* loop_step hook: one batch, user cbs task-spawned */
```

## 3. `lvrt_await` (runtime side, parity-exact)

> **⟦SENSITIVE-GATE — ENTER S2⟧** Model-gated (overview §12) — the `value`-read retain
> discipline and drained-await parity in this section. **STOP before writing any fenced
> code below** (through the EXIT gate at the end of this section; S2 also covers the
> interpreter parity path noted in doc 03 §2). Declare
> `SENSITIVE SECTION ENTERED: S2 — lvrt_await retain/parity. Halting for model escalation; not writing fenced code.`
> and hand back. Escalated model only.

```c
void lvrt_await(LvValue* dst, const LvValue* p) {
    if (lv_tag(p) != LV_OBJECT) { lv_copy(dst, p); return; }   /* await non-promise = id */
    if (lvrt_throwing())        { lv_set_void(dst); return; }  /* Eval.cpp:1077 parity   */
    if (!(lvrt_promise_state(p->obj) & 1)) {
        int r = lv_tasks_enabled()
              ? lv_task_park_on(p->obj)                        /* park: doc 1 §7 */
              : lv_pump_until_ready(p);                        /* LANG_TASKS=0: old path */
        if (r == LV_PARK_DRAINED && !lv_parity_mode())
            { lvrt_raise_str("await: event loop drained with promise unresolved");
              lv_set_void(dst); return; }                      /* C3 (doc 5) — M5 on   */
        /* parity mode falls through to the field reads, silent — G2 */
    }
    if (lvrt_promise_state(p->obj) & 2) {                      /* Worker failed → rethrow */
        LvValue msg; lvrt_getfield(&msg, p, "failMessage");
        lvrt_raise_lv(&msg); lv_set_void(dst); return;         /* Track 10 §3.3 parity   */
    }
    lvrt_getfield(dst, p, "value");   /* SAME retain discipline as the inline readBB —
                                         the "dk==1 wrap retains the borrowed read-out"
                                         note at LlvmGen.cpp:2820 is now THIS line's
                                         contract; the churn corpus (+0B) is the proof */
}
```

Parity notes bound to existing behavior, each an acceptance case:
- non-object passthrough (`Eval.cpp:1078` parity, and the `aw.pass` BB today);
- pending-throw on entry returns immediately (the pump's `notThrowing` gate);
- `failed` read *before* `value` (the `chkFail` ordering — a rejected Worker must never
  yield its default `value`);
- `ready` re-check after park is a *state* read, not trust in the wake (a drain wake
  arrives with `ready` still false — that is the point).

`lvrt_promise_state` reads the two fields through the same internal getfield the pump's
`aw.cond` BB used, but **only** `ready`/`failed` — both `bool` immediates, so no retain
happens anywhere on the poll path (G15). If profiling ever shows the name-based field
lookup hot at high parked counts, the recorded optimization is caching the slot offsets
once per class via the shape registry (declared prelude fields have fixed offsets — §7 of
info.md) — an optimization, not a semantic change, and not built until measured (G10).

> **⟦SENSITIVE-GATE — EXIT S2⟧** On landing `lvrt_await`'s `value`-read (retain discipline
> exact) and the drained-parity path, and passing S2's bar (`+0B` churn corpus exact;
> both-mode corpus byte-identical), **STOP.** Declare
> `SENSITIVE SECTION COMPLETE: S2 — lvrt_await retain/parity. <what landed + how verified>. Halting for review.`
> and hand back. The codegen diff (§4) below is NOT fenced — it is mechanical (emit a call
> + throw-check) and resumes as ordinary work.

## 4. Codegen diff (`src/LlvmGen.cpp`)

1. Declare `rtAwait = fn("lvrt_await", voidTy, {ptrTy, ptrTy})` beside the §2.9 loop fns
   (`:348` block).
2. `case Op::Await:` — emit the call + `emitThrowCheck((int)pc)` (the same
   dispatch-at-THIS-pc rule the pump's fail path uses today, `:1929` pattern). Delete the
   BB graph **at M6**, not before: through M5 the case emits *either* form keyed off
   `lv_tasks_enabled()`? **No — flag is runtime-read, codegen is static.** The call form
   is emitted unconditionally from M1; `lvrt_await` itself contains the `LANG_TASKS=0`
   pump fallback (`lv_pump_until_ready` — a C transcription of today's BB loop, same two
   ABI calls). This keeps ONE emitted shape and puts the mode switch where the mode
   lives: the runtime. The inline BB graph is deleted in the same commit (its C
   transcription replaces it verbatim — behavior, not code, is what M1's double-mode
   corpus run pins).
3. Entry emission: unchanged — `lv_main` already the A-M5 shape (`lv_entry.c:12-15`).

## 5. Entry and drain (`runtime/lv_entry.c`)

Today (`:36-37`): `lv_main(&ret)` runs to completion on the OS stack, then
`while (lvrt_loop_has_work()) lvrt_loop_step();` drains.

Tasks mode:

```c
lv_rt_init(argc, argv);
if (lv_tasks_enabled()) {
    lv_sched_init();
    lv_sched_hooks(lvrt_promise_state, lvrt_loop_step_tasks);
    lv_sched_throw_probe(lvrt_throwing_flag);         /* G11 probe = gThrowing read */
    lv_sched_run(lv_main_task, &ret, NULL);           /* lv_main as task 0 */
} else {
    lv_main(&ret);
    while (lvrt_loop_has_work()) lvrt_loop_step();    /* verbatim today — until M6 */
}
lvrt_term_shutdown();
return lv_rt_exit_code();
```

`lvrt_term_shutdown` (raw-mode restore) stays outside the scheduler — it must run even
when the scheduler exits via the drain path, same guarantee as today (`lv_entry.c:39-42`).

## 6. Dispatch-on-task (`runtime/lv_loop.c`)

`lvrt_loop_step_tasks` = today's step with one change at the **user-callback invocation
sites** (timer fire, fd/eventfd watch fire): instead of invoking the closure inline on
the scheduler stack, wrap in `lv_task_spawn(run_closure_thunk, closure, arg)`. Everything
else — poll-set construction, TLS wants/pending integration (RuntimeLoop.hpp:27-37
guarantee), POLLNVAL auto-cancel (`lv_loop.c:215-233`), `lvrt_loop_cancel_fd`
(`lv_thread.h:44`) — is untouched; the loop stays dumb.

Runtime-internal handlers (the join reaper inside `sysThreadResult`'s watch, POLLNVAL
cancels) are *language-level* closures registered by the prelude, so they ride the same
task path — that is correct and required: `sysThreadResult` rebuilds into this thread's
heap and may run prelude code that awaits in the future. **Rule: every closure the loop
fires becomes a task; the loop itself never runs language code** (under tasks mode).

Batch semantics note: today `nextBatch`-equivalents dispatch a *batch* then re-poll.
Task-spawning a batch preserves order (spawn order = FIFO runq order = old inline order)
but interleaves differently once callbacks park — which is C1, the designed change. The
loop must **not** wait for the batch's tasks to finish before returning from
`lvrt_loop_step_tasks` (that would rebuild the pump one level up — the scheduler owns
sequencing now).

## 7. Threads (`runtime/lv_thread.c`)

> **⟦SENSITIVE-GATE — ENTER S4⟧** Model-gated (overview §12) — this section adds to
> `lv_thread.c`, which owns Track 10's already-closed eventfd join race ("never leaves the
> awaiter parked forever"). **STOP before writing any fenced code below** (through the EXIT
> gate at the end of this section). Declare
> `SENSITIVE SECTION ENTERED: S4 — lv_thread.c scheduler bootstrap. Halting for model escalation; not writing fenced code.`
> and hand back. Escalated model only. The changes are additive — do not refactor the
> landed spawn/trampoline/join/reap path.

The worker trampoline (M3c) currently: `lv_thread_ctx_init` → run flattened body →
flatten result → signal eventfd. Tasks mode adds, symmetrically with §5:

```c
lv_thread_ctx_init(&heapBase, &arenaBase);
if (lv_tasks_enabled()) {
    lv_sched_init(); lv_sched_hooks(...); lv_sched_throw_probe(...);
    lv_sched_run(worker_body_task, ctx, NULL);   /* body = task 0 of THIS thread */
} else { /* today's direct run */ }
/* flatten result / reject, signal eventfd — unchanged, runs after sched_run returns */
```

A worker whose body parks forever: its scheduler quiesces → drain wake → (M5 mode) the
C3 throw → lands in `spawn`'s prelude `catch (IException e) → w.reject(e)` leg →
rethrows at the *spawner's* await with the drained message. No new code beyond the
drain-wake — the routing falls out of the landed reject seam (`Resolver.cpp:1146ff`).
The eventfd is still always signaled (the threads design's "never leaves the awaiter
parked forever" invariant, techdesign-threads.md risk #3/#212 — preserved verbatim).

Sched TLS init rides `lv_thread_ctx_init`'s existing contract (`lv_thread.h:32-36`:
"initialize ONLY this thread's TLS runtime state") — the scheduler struct is exactly such
state. Reap: `lv_sched_run` returning guarantees no live tasks (DONE or drain-woken and
completed), so `lv_thread_ctx_unmap` needs no change; task stacks are munmap'd by the
scheduler before return (pool teardown), keeping the reap-time `+0B` accounting exact.

> **⟦SENSITIVE-GATE — EXIT S4⟧** On landing the worker scheduler bootstrap and passing
> S4's bar (thread-leak harness `+0B`, no leaked pthread, join-race corpus green), **STOP.**
> Declare
> `SENSITIVE SECTION COMPLETE: S4 — lv_thread.c scheduler bootstrap. <what landed + how verified>. Halting for review.`
> and hand back before resuming non-fenced work.

## 8. Pending-throw routing (G11 + C2 mechanism)

`gThrowing` is thread-local (`LlvmGen.cpp:367-370`) — correct granularity already, since
tasks never migrate. The obligations:

- **Park sites:** `lvrt_await` parks only after the `lvrt_throwing()` early-out, so the
  flag is provably clear at park (the probe assert is belt-and-braces).
- **Task exit with flag set** (a callback threw uncaught): the task trampoline checks the
  flag before returning to the scheduler. Parity through M4 is **impossible to reproduce
  exactly** (the pump delivered it to whichever await pumped innermost — C2's misroute);
  the *achievable* parity is the no-await-pumping case, which today exits the drain loop
  (`:705-706` `notThrowing` gate) and reports uncaught. So: trampoline leaves the flag
  set and returns; the scheduler sees it, stops dispatching (same gate), and the standard
  uncaught path (`rtUncaught`, exit 1 — the exit-codes design) fires. Through M4, corpus
  material that exercised the misroute is quarantined into the C2 divergence list (doc 5
  §4) rather than "made to pass".
- **Scheduler never runs language code**, so the flag can't flip between tasks except
  through a task — no save/restore per task is needed (contrast the eh_globals dance a
  C++-EH runtime would owe; we owe nothing — overview §2).

## 9. State-audit table (thread-local & global runtime state vs. parked tasks)

Anything a task can observe across a park must be either (a) per-task by construction
(on its stack), (b) thread-level and *interleaving-safe under the pump already* (tasks
add no new exposure), or (c) fixed. The audit:

| state | where | verdict |
|---|---|---|
| TLS heap/arena/freelist/accounting | `lv_thread_ctx_init` | (b) thread-pinned tasks; allocator was already reentrant under nested pumps |
| `gThrowing`/thrown slot | `LlvmGen.cpp:367`, runtime | (b) + §8 invariants; assert via throw probe |
| transfer counters | `lv_thread.h:28-30`, C11 atomics | (c) atomics, diagnostics only |
| TLS session table (fd-keyed) | `RuntimeNatives.cpp` / `lv_tls_openssl.c` | (b) fd-confined; pump already interleaved handshakes across awaits |
| terminal raw-mode | `lvrt_term_*` | (c) process-global, restore-on-exit unchanged (§5) |
| `LANG_ARC_TRACE` flag | `lv_runtime.c:187` | (c) read-once |
| argv/class registry | `lv_rt_init`, `lvrt_register` (`lv_abi.h:238`) | (c) immutable after init |
| loop registries (timers/watches) | `lv_loop.c` | (b) mutated only from this thread; callbacks now tasks — same mutation points |

Any implementer touching a runtime global not in this table: add it to the table with a
verdict **before** merging (that's the review rule this section exists for).

## 10. Corpus and lanes

- M1 gate: `tests/run_corpus.sh` LLVM lanes + `run_native_llvm.sh`, `run_sysnatives.sh`,
  `run_tls.sh`, sockets/http/threads corpus — **green under `LANG_TASKS=0` and `=1`**,
  byte-identical to each other except the enumerated C-divergences (which stay
  quarantined until M5, doc 5 §4).
- churn/leak: `tests/corpus/churn` and the thread-leak harness with `[tasks]` stats on —
  `+0B`, `transfers_out=0`, `stacks mapped==pooled+live` at exit.
- New `tests/corpus/tasks/` (doc 5 §4) runs on this leg from M1.

## 11. Predictable gotchas (leg-local)

| trap | note |
|---|---|
| re-inlining `lvrt_await` at a future maintenance pass | §1's history comment goes IN the code |
| `lvrt_getfield` retain discipline on `value` | replicate the `dk==1` wrap exactly; churn corpus is the proof |
| task-spawning the batch then *waiting* for it | forbidden — rebuilds the pump; the scheduler owns sequencing (§6) |
| worker eventfd signaled before spawner's watch registered | unchanged protocol; do not "simplify" registration order (threads doc risk #3) |
| `lvrt_term_shutdown` inside the scheduler | keep it outside `lv_sched_run` (§5) or a parked-at-exit program skips terminal restore |
| polling `value` instead of `ready`/`failed` | G15 — ARC churn + drain leak; poll bools only |

## 12. Acceptance (M1)

Corpus per §10; perf: no lane slower than pump by >2% wall-clock (`run_llvm_timed.sh`);
`[tasks]` stats line present and sane under `LANG_TASK_STATS=1`; `drained_wakes=0`
across the entire existing corpus (any nonzero = a latent G2 case — file it, don't ship
it silent).

**STOP:** any corpus divergence outside C1/C2/C3 shapes; churn regression; any need to
modify `src/Lower.cpp`, the Checker, or the prelude to make this leg pass (the design
says none is needed — a needed change means the design missed something; escalate).

---

## Implementation log (2026-07-12 — IMPLEMENTED, M1 bar met)

S2 and S4 authored under the model gate (escalated model, owner-directed); §4–§6
glue as ordinary work. Deviations/refinements, each verified:

1. **Throw-path `dst` discipline tightens the §3 sketch.** The sketch's
   `lv_set_void(dst)` on the failed-rethrow and C3 paths would leak the dest
   register's old ref: emitThrowCheck branches away before the dk==1 wrap's
   release/retain tail, so unwind's releaseAllRegs must find the OLD value in
   the register (the pump's failThrow wrote only arcScratch for the same
   reason). `lvrt_await` leaves `*dst` untouched on every throwing exit.
2. **The raw `lvrt_loop_step` keeps inline dispatch in BOTH modes**; only the
   scheduler's hook (`lvrt_loop_step_tasks`) task-spawns. Reason: lv_main's
   emitted tail drain (LlvmGen.cpp:693-716) pumps the raw step from inside
   task 0 — task-spawning there would run callbacks after lv_main's
   uncaught/meter epilogue and break byte-identity. §4.3 "entry emission
   unchanged" holds precisely because of this split.
3. **Doc-1 trampoline completion assert relaxed to §8's rule.** The spawn
   try/catch lives on the SPAWNER side (Resolver.cpp watch callback), so a
   worker body's uncaught throw legitimately completes task 0 with the flag
   set — that is the uncaught carrier §8 describes. The scheduler now gates
   (stops dispatching, returns flag-intact); park-with-flag stays asserted.
   lv_entry reports cross-task uncaught via a task-0-completion probe
   (`lv_sched_main_completed`) to avoid double-reporting.
4. **`lv_sched_teardown`** (lv_task.c/h — the teardown doc 1's pool-return
   comment delegated here): workers munmap pooled stacks + free scheduler
   allocs before the eventfd publish; §8-gate-abandoned parked tasks stay
   pinned (G12; B2 unwinds).
5. **Cross lanes**: `build-triple.sh` + `LANG_LVRT_SOURCES` carry the task
   substrate; lv_task.c's POSIX includes moved below the `_WIN32` split so the
   MinGW stub compiles. qemu(aarch64) + wine(win32) lanes green.

Verified: full ctest 147/148 (the one fail is `regex_pathological_linear`, a
pre-existing host-speed timeout — 15.6s pre-change vs 15.4s post on a 10s
budget, proven on the stashed tree); full 55-program LLVM corpus + threads/
regex/tls/json/etc. lanes green under `LANG_TASKS=0` **and** `=1`,
byte-identical incl. [heap] meters; churn corpus `+0B` both modes; thread-leak
harness `+0B`/reaps==spawns/fd-flat under tasks; valgrind clean (0 errors,
0 lost) on spawn_join + reject_rethrow under tasks; `[tasks]` stats sane
(`mapped==pooled+live`), `drained_wakes=0` corpus-wide. Perf: corpus lanes at
parity (timed lane green; channel microbench +17% under tasks); worker-spawn
microchurn pays ~24µs/worker for its scheduler bootstrap+teardown (spawn_join
−31% iters/sec) — inherent to §7's per-thread-scheduler choice, logged for the
M4 perf pass, not speculatively optimized (G10).

Not in this leg (other docs own them): `tests/corpus/tasks/` authoring (doc 5
§4), interpreter legs (doc 3), the M5 flip/S5 (doc 5 — `lv_parity_mode()` in
lv_runtime.c is the hook it flips), pump deletion (M6).
