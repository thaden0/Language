# Handover: bug #99 — LLVM value-struct corruption from a `for`-over-`Array<Struct>` with an early `return`

**Status:** OPEN, P0.3 (top open P0 in `known_bugs_1.md`, entry at the
`## #99 [P0] …` heading). LLVM native backend only; oracle (`--run`) and IR
(`--ir`) are clean. This doc is a cold-start briefing for a fresh session.

**Branch/worktree:** `agent2` (this worktree is agent2 — always work here, never
master). At handover time `agent2 == origin/master == 2ec9305` (fully synced).
Re-fetch before pushing; origin/master is shared and moves concurrently.

---

## TL;DR

#99 is the **sibling** of the just-fixed #95. Both are the same class of defect:
a **value-struct alias left in a live register at frame exit**, which the
frame-exit `releaseAllRegs` then "releases." On oracle/IR that release safely
no-ops; on LLVM's manual ARC, once the aliased element's backing `Array` has
been freed earlier in the same frame, the release reads the freed block's
classId (garbage), the value-class skip in `lv_is_counted` fails, and the
decrement lands on a **freelist next-pointer word** → heap corruption that
surfaces LATER inside `lv_alloc_heap`, far from the causing site (classic P0.3
"crash-later").

- **#95 (FIXED, commit `ffa9e6e`):** the un-cleared register was the **method-call
  receiver window** (`rec.key()` on a value-struct loop var). Fixed in
  `src/Lower.cpp` by voiding that window register after the call.
- **#99 (THIS BUG, open):** the leading hypothesis is the **early-`return`
  exit path** skipping `Lower.cpp`'s post-loop value-struct loop-var clear — but
  **this is UNCONFIRMED** and the minimal early-return repro below does NOT
  reproduce, so the real trigger has extra ingredients not yet isolated.

The fix is almost certainly in the **standard shared compiler code**
(`src/Lower.cpp`, possibly `src/LlvmGen.cpp`) — NOT the frozen `X64Gen.cpp`
assembly, and NOT a logic bug in the runtime ARC (`runtime/lv_runtime.c`). The
runtime faithfully executes a bogus release the compiler emitted; that was
exactly the shape of #95.

---

## How to reproduce (the reliable one)

The **auth corpus** segfaults on LLVM. This is the trustworthy repro right now
(master's own minimal #99 repro in the bug entry could not be re-triggered in a
stripped-down form — see "Dead ends").

```
cd /home/len/code/Language-agent2
./build/trident plan packages/atlantis/tests/corpus/auth --plan /tmp/auth.lvplan --leviathan ./build/leviathan
./build/leviathan --build-native /tmp/auth.bin --plan /tmp/auth.lvplan
setarch $(uname -m) -R /tmp/auth.bin        # SIGSEGV partway through; oracle/IR print the full auth.expected
```

Oracle/IR reference (both green, byte-identical):
```
./build/leviathan --run --plan /tmp/auth.lvplan   # matches packages/atlantis/tests/corpus/auth/auth.expected
./build/leviathan --ir  --plan /tmp/auth.lvplan
```

Verified 2026-07-20: the auth corpus still crashes on LLVM **with the #95 fix in
place**, which is the proof that #99 is a distinct un-cleared path, not something
#95 already closed. Its entry notes the crash point "moves around" depending on
unrelated prior heap activity (reorder which test runs first → crash moves, never
disappears) — the fingerprint of freelist corruption, same as #95.

`setarch … -R` disables ASLR so heap addresses are deterministic run-to-run
(essential for the watchpoint technique below).

---

## The proven debugging playbook (this is exactly how #95 was cracked)

The symptom (crash in `lv_alloc_heap`) is far from the cause. Do NOT try to read
the disassembly at the crash. Instead, catch the corrupting WRITE:

1. **Instrument the freelist for integrity** (temporary, in `runtime/lv_runtime.c`).
   Add a `lv_dbg_check_freelists(where)` that walks every `g_freelist[c]` and
   `__builtin_trap()`s at the first node that is out-of-region **or not
   16-byte-aligned** (real blocks are always 16-aligned; the alignment check
   catches the corruption one hop earlier than a region-only check). Call it at
   the top of `lv_alloc_heap` and `lv_free_raw`. Rebuild `lvrt` + `leviathan`,
   rebuild the native binary, run under `setarch -R`. It prints the **container**
   address holding the corrupt pointer — a stable, aligned freed block, e.g.
   `container=0x7fffe7402200 holds=0x7fffe74020ff` (note the corrupt value is
   `container_word - 1`: a `lvrt_release` decrement of a freelist next-pointer).

2. **Hardware-watchpoint the corrupting write.** With ASLR off the container
   address is deterministic. Under gdb: break once so the heap is mapped, then
   `watch *(unsigned long*)0x<container>` with a condition catching the unaligned
   store (`… & 0xf != 0`), `cont`, and `bt`. This lands you **inside
   `lvrt_release`**, called from the exact generated function.

3. **Map the generated frame names.** LLVM funcs are named `f<N>` = index into
   `mod.functions`. Temporarily, in `src/LlvmGen.cpp` right after the
   `fns.assign(...)` loop (~line 830), dump the map under an env gate:
   ```cpp
   if (getenv("LVDBG_FNMAP"))
       for (size_t i=0;i<mod.functions.size();++i)
           fprintf(stderr,"FNMAP f%zu = %s\n", i, mod.functions[i].name.c_str());
   ```
   Also handy: an `LVDBG_IRDUMP=<fnname>` gate that prints the Op stream of one
   function (op name + a/b/c/d/sname/sym) so you can read exactly which
   register/Op emits the doomed release. (#95's culprit was `Router.finalize`.)

4. **Life-cycle trace a specific block.** Add a `noinline,noipa` hook
   `lv_dbg_event(kind, ptr)` called from alloc / free / retain / release, and
   `break lv_dbg_event if $rsi == 0x<block>` in gdb to print alloc→free→release
   order with backtraces. (`noipa` matters: without it GCC clones the hook via
   constprop and your breakpoint sits on the dead original.)

**All of this instrumentation is temporary — `git checkout` it before committing.**
For #95 the whole chain took the crash from "SIGSEGV in the allocator" to "stale
value-struct alias in register N released by `releaseAllRegs` at frame exit" in a
few iterations.

---

## Where the fix landed for #95 (your starting map)

`src/Lower.cpp`, method-call lowering in `lowerCall` (~line 1745-1785). The
receiver was marshaled into the CallDyn window via a plain `Op::Move` — a **bare
alias**, because the wrap's retain no-ops on value classes — and that window
register survived to frame exit. The fix voids it after the call for a
`definiteValueStruct` non-consumed receiver:
```cpp
auto clearStructRecvWin = [&] {
    if (!clearRecv && callee->a->definiteValueStruct)
        emit(Op::LoadConst, recvWin, addConst(vvoid()));
};
```

The **for-in loop-var clear that #99 likely bypasses** is in the same file at
`src/Lower.cpp:991-1002` (the general-iteration branch of `StmtKind::For`):
```cpp
// bug #66: a VALUE-STRUCT loop variable ALIASES the array element … clear the
// register after the loop so the stale alias is not released at function-scope
// exit …
else if (s->type && s->type->resolvedSymbol && s->type->resolvedSymbol->isValue)
    emit(Op::LoadConst, elem, addConst(vvoid()));   // line 1002 — runs AFTER the loop
```
**Hypothesis:** an early `return` from inside the loop jumps to the `Op::Ret`
path (which runs `releaseAllRegs`) *before* this post-loop clear at line 1002
executes, so `elem` (the value-struct alias) is still live and gets released
against a possibly-freed element. Candidate fixes to explore: emit the clear on
the loop's exit/return edges too, clear `elem` at the top of each iteration
before `IterAt`, or have the checker/lowerer treat the loop-var as borrowed so
`releaseAllRegs` never touches it. **Confirm the mechanism before coding** — see
the dead end below.

Related prior fixes for context (all `bug.md #NN` commits in git log): #66
(boxed value-struct array elements deep-copied/individually freed), #90
(`Op::CallDyn` consumed-receiver release), #95 (this session).

---

## Dead ends / things already ruled out (don't repeat these)

- **My #95 fix does NOT fix #99.** Auth corpus still crashes on LLVM with it in
  place. Different un-cleared register.
- **Minimal early-return repro does NOT reproduce.** A standalone
  `struct` + `class`-typed param + early `return` from `for` over `Array<Struct>`
  + later heap churn ran clean on LLVM *even pre-#95-fix*. So the "class param +
  early return" pair from the bug entry is **necessary but not sufficient** —
  there is at least one more ingredient (candidates: the specific `Http::Context`
  class shape, `string?`/`None` return type, the exact `digest::hmacSha256`
  allocation pattern, string content/length, or interaction with the prelude).
  Reproducing #99 in a small standalone `.lev` is itself an open sub-task; until
  then, drive it through the auth corpus plan.
- **The `bind-iterable-to-a-local-first` workaround does NOT fix #99** (the entry
  says so explicitly — it fixes a superficially similar case but not this).
- **The accumulator workaround was already applied** to
  `packages/atlantis/src/auth/cookie.lev` (`cookieValue`) and the auth corpus
  STILL crashes — so either the workaround site isn't the real trigger, or there
  are multiple trigger sites. Treat the auth crash as possibly-broader than the
  single cookie shape.
- **Standard non-runtime layers to focus on:** oracle/IR are clean, so the Op
  stream is correct and the bug is in **LLVM lowering** — either a shared
  `Lower.cpp` emission that only bites LLVM's manual ARC (most likely, like #95),
  or a `LlvmGen.cpp` Op-lowering divergence from `IrInterp.cpp`. Compare the two
  for whatever Op the watchpoint implicates.

---

## Verification bar for the eventual fix (match #95's discipline)

1. Auth corpus green on **oracle + IR + LLVM** (`packages/atlantis/tests/runtests.sh`).
2. Full atlantis suite green on all three engines (that script runs every corpus dir).
3. Targeted ctest lanes green — at minimum: `corpus_treewalk`, `corpus_ir`,
   `corpus_llvm`, `corpus_llvm_full`, `corpus_churn_leak_llvm`,
   `corpus_map_return_ownership_llvm`, `composition_llvm`, `metatests`.
4. **Red→green regression pin** under
   `tests/corpus/composition/aggregates/green/` with a `.lev` + `.expected`,
   proven to SIGSEGV on LLVM before the fix and pass after (revert the fix,
   rebuild, confirm the crash, restore). This is the known-bugs workflow: fix +
   delete the #99 entry from `known_bugs_1.md` + update the priority table +
   promote red→green, all in one `bug.md #99`-prefixed commit.
   (#95's pin — `array_struct_method_recv_alias.lev` — is a good template.)
5. Do NOT commit any of the temporary runtime/LlvmGen instrumentation.

## Fast facts

- Fix #95 commit: `ffa9e6e`. Merge that synced master: `2ec9305`.
- Corrupting write signature: `lvrt_release` decrementing a freelist next-pointer
  (`container_word - 1`), caught by a freelist-integrity trap, back-traced via a
  hardware watchpoint.
- Runtime ARC entry points (all correct; they're the messenger): `lvrt_release`,
  `lv_is_counted`, `lvrt_vfree`, `lv_free_raw`, `lv_alloc_heap` in
  `runtime/lv_runtime.c`.
- Frame exit that fires the bad release: `releaseAllRegs()` in
  `src/LlvmGen.cpp` (called from every `Op::Ret`/`Op::RetVoid`/unwind).
