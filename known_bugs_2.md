# Known bugs — part 2 of 2 (known_bugs_2.md)

Active, unresolved bugs only. This is one half of the known-bug register;
the other half lives in `known_bugs_1.md`. The two files together hold every open
entry, with high- and low-priority bugs dispersed evenly across both so
neither file skews toward one tier. Bug numbers are the stable identity —
a `#N` cross-reference may point at an entry in the companion file.

Each entry has a minimal repro, expected vs. actual behavior, and a
root-cause pointer. Fixed bugs are not tracked here — see git history
(commits prefixed `bug.md #N`) for their resolutions.

Every entry carries a priority tag (`[P0]`–`[P3]`) in its heading, assigned
by the marker checklist in **Priority system** below, plus a one-line
justification citing the exact marker(s) so the assignment is auditable.

Current standings for this file (within a tier, ordered by bug number):

| Priority | Bugs |
|----------|---------------|
| P0       | #94 |
| P1       | #96 |
| P2       | — |
| P3       | — |

Each entry's Workaround note (inline, above) carries its own debt sites — there is no
separate `docs/footguns.md` registry (retired 2026-07-19, merged into these two files).
Once the composition corpus lands, a fixed bug also gets a red-lane repro promoted to
green under `tests/corpus/composition/` (`designs/techdesign-composition-corpus.md`).
Fixing #N means: fix, delete the entry here, promote red→green — one commit.

---

## #94 [P0] — calling a function-typed FIELD via dot-call silently no-ops on LLVM

**Found:** 2026-07-19, ORM Track 06 M1 (boot validation never ran on LLVM
while printing success). Related family: the 2026-07-11 "field-closure
dot-call" finding and #53's this-receiver lambda rule — this entry is the
checked-code, LLVM-specific shape with a live repro.
**Priority justification:** P0.3 — an actively-maintained engine silently
drops an operation for checker-accepted ordinary code; the symptom (missing
side effects) surfaces away from the causing site. Oracle and IR run the call
correctly; LLVM returns without executing the closure body.

**Repro shape (from the ORM):**

```
class RepoHandle {
    () => Promise<void> validate;      // field holding a closure
    ...
}
await h.validate();                    // oracle/IR: runs the closure; LLVM: silent no-op
```

Observed concretely: `Db.validate()` printed its success line on LLVM while
the closure's `DESCRIBE` never executed (the corpus caught it as a missing
`[sql]` log line). Copying to a local first is reliable on all engines:

```
var f = h.validate;
await f();
```

**Root-cause pointer:** LLVM lowering of a dynamic call whose callee is a
field read (member access → call in one expression); the local-copy form
takes the ordinary closure-value call path.

**Workaround (debt sites):** copy the field to a local, then call — applied
throughout `packages/atlantis/src/orm/db.lev` (search "field-closure
dot-call"); `packages/atlantis-mysql/src/pool.lev` (`var f = fn;`) already
used the same idiom.

---

#92 fixed 2026-07-19 (found+fixed in-session, ORM Track 06): an ATTRIBUTE's
class symbol shadowed a real same-named class for ordinary bare-name
resolution — with `uses Atlantis::Orm; uses Atlantis::Data;`, a bare `Row`
resolved to the `@Row` attribute (declared first in import order) instead of
the `Atlantis::Data::Row` class, making every member read on the value
silently void on the oracle (and `User::FromRow(r)` fail overload matching).
Fixed at source in three places: `Resolver::importOne` no longer dumps
attribute-class symbols into `uses` overlay scopes at all (attribute
resolution runs through the imports map + namespace scopes, never the
overlay), and both `Resolver::resolveType` (bare + qualified branches) and
`Checker::visibleClass` prefer a non-attribute type when both are visible.
Regression floor: `packages/atlantis/tests/probes/miniorm/` (cross-package
Row construction + FromRow through the full ORM rule set, oracle green) and
the whole atlantis corpus suite (`packages/atlantis/tests/runtests.sh`).

## #96 [P1] — sockets/process/pty classes cannot be compiled for a Windows target: prelude over-marking drags the task natives in

**Found:** 2026-07-19, implementing the pty floor's Windows lane
(`designs/complete/techdesign-03-pty-windows-conpty.md` S3/G-PTY3).
**Priority justification:** P1.2 — the only workaround is per-use: every track
that wants sockets, a child process, or a pty on a Windows target must
independently discover this and hand-roll the floor natives at each site; no
single workaround retires it. (P2.3 also matches — a documented feature fails
loud on one lane while working on the others — but P1 is evaluated first and
P1.2 fits the workaround shape exactly.)

**NOT the bug:** that `spawn`/`Channel`/`TaskGroup` are unsupported on a
Windows target. That is a deliberate ruling (LA-30 G5: win32 needs the Fiber
API, so tasks stay pump-pinned — `runtime/lv_task.c:27-57`), documented in
`docs/reference.md`. The bug is its **blast radius**.

**Repro** — no task feature anywhere in the program:

```
$ printf 'TcpListener l = TcpListener(9099);\n' > t.lev
$ leviathan --native-obj t.o --target x86_64-pc-windows-gnu t.lev
error: LLVM backend: tasks: unsupported on Windows (v1) — 'sysTaskCancel'
       has no Windows lowering
```

Identical failure for `TcpStream`, `Process`, and `Pty`. Expected: these
compile for a Windows triple (nothing in any design rejects them, and
`docs/reference.md` documents sockets/`Process`/`Pty` without a Windows
carve-out — only threads/`spawn` carry one). Actual: a whole capability family
is unbuildable for the target, and the diagnostic names a native the program
never mentions, so the message mis-attributes the cause.

**Root-cause pointer:** two independent contributors; narrowing **either** one
fixes it.
1. Prelude over-marking (`src/Resolver.cpp:3232` documents this exact
   mechanism breaking `--build` once before): marking is arity-blind and
   by-name, so `TcpStream`/`File`'s `close()` also marks `TaskGroup::close()`,
   whose body reaches `std::sysTaskCancel` (`src/Resolver.cpp:1414`).
2. The reject is **emission**-gated, not reachability-gated
   (`src/LlvmGen.cpp:2752-2765`): it fires when the row is emitted, reachable
   or not. The wasm gate immediately above it already has the two-tier shape
   (reachable → diagnostic; prelude-only → `lvrt_unsupported` trap) that would
   answer this.

**Owner ruling needed before a fix lands** (which of the two to narrow is a
gate question, not a pty question): recorded as
`designs/requests/request-windows-task-gate.md`.

**Workaround (debt sites):** drive the **floor natives** directly instead of
the prelude class — `std::sysPtySpawn`/`sysRecv`/`sysSend`/`sysPtyResize`/
`sysKill`/`sysReap` compile and run on Windows today. Applied in
`tests/pty_win_driver.lev` (the G-PTY3 behavioral lane, which is why it does
not use `Pty`). `tests/run_wine_cross.sh` avoids the area entirely — its scope
note excludes net corpus.

## Priority system

Priorities are derived mechanically from the markers below so that different
agents assign the same tier to the same facts. Evaluate tiers top-down
(P0 → P3) and assign the first tier with at least one matching marker. Two
overrides, applied in this order:

1. **Explicit owner ruling wins.** If the entry records an owner ruling that
   names a priority ("low priority for now", "treat as P0", ...), use that
   priority regardless of markers, and cite the ruling.
2. **Semantics-ruling cap.** If the intended *observable behavior* is still
   undecided — the owner must choose what programs should see before any fix
   can be written — cap the priority at P2 unless a P0 marker applies. A
   pending ruling that concerns only fix *shape or ownership* (the intended
   behavior is undisputed) does not cap.

Definitions used by the markers: the **oracle** is `--run` (`Eval.cpp`), the
ground truth `.expected` files are generated from
(`designs/complete/techdesign-portable-backend.md` §0.4). **Actively-maintained engines**
are `--ir`, emit-C++ (`--build`), and LLVM (`--build-native`); LLVM is the
primary backend (portable pivot, 2026-07-05). **Frozen** means
`X64Gen.cpp`/ELF (`--emit-elf`). **Ordinary user code** means expressible in
a plain `.lev` source file without editing the compiler or the prelude.

### P0 — critical
- **P0.1** The oracle prints wrong output for ordinary user code — wrong per
  the language reference, or unanimously contradicted by the
  actively-maintained engines. (Risk: the wrong output gets baked into
  `.expected` files and the correct engines then read as regressed.)
- **P0.2** A track is blocked right now and no workaround lets it proceed.
- **P0.3** An actively-maintained engine exhibits **silent state corruption**
  for ordinary, checker-accepted code: memory corruption, data going stale
  after unrelated activity, or an operation silently dropped — any failure
  whose symptom surfaces *away from the causing site*. Distinguished from
  P1.1 (a wrong value observed at the faulting expression itself): a P0.3
  defect's blast radius is unbounded, it mis-attributes downstream debugging
  time, and a crash-later variant counts even though the exit is nonzero.
  (Owner policy 2026-07-13, stop-the-line: these head the fix queue, and no
  new consumer-track code is architected on the affected construct while one
  is open — see `designs/techdesign-composition-corpus.md` §1.)

### P1 — high
- **P1.1** An actively-maintained engine silently produces a wrong value —
  exit 0, no diagnostic — for code the checker accepts, and the entry does
  not dispute which behavior is correct.
- **P1.2** The only workaround is per-use: every future track touching the
  area must independently know about it and re-apply it (naming conventions,
  per-callsite guards, ...), rather than one workaround retiring the risk.

### P2 — medium
- **P2.1** Engines diverge and a semantics ruling must pick the intended
  behavior before any fix is valid (see also the cap in override 2).
- **P2.2** Performance/resource-only: output is correct on every engine, but
  asymptotic complexity or memory behavior is wrong on an
  actively-maintained engine.
- **P2.3** A documented language feature fails loud (compile-time or runtime
  error) on one actively-maintained engine while working on the others.
- **P2.4** Missing diagnostic with a correct happy path: an unsupported
  construct should error but doesn't, and no supported construct misbehaves.

### P3 — low
- **P3.1** The owner explicitly ruled it low priority (override 1).
- **P3.2** Only frozen-backend (`X64Gen`/ELF) behavior is affected.
- **P3.3** The fix already landed; only regression-test coverage is missing.
- **P3.4** Cosmetic only (formatting/spelling of output), no value or
  control-flow difference.

---

#86, #88, #89 fixed — see git history for their resolutions.

#83 fixed 2026-07-19 (item 1 of 2): `Resolver::returnAssignable`/a new
`paramsAssignable` now compare a method's return/param types by resolved
Symbol identity (recursing through generic arguments) before falling back to
the covariant-base walk, so an alias-qualified member type (`A::Data::Foo`)
and the interface's in-package bare name (`Foo`) satisfy each other — no
`uses` + bare-spelling workaround needed anymore.

Item 2 ("`uses` behaves package-global") turned out not to be a defect on
inspection: per-file `uses` scoping is deliberate, existing design (each
source file's top-level imports overlay only that file, `Sema::fileScopeFor`
in `Symbols.hpp`), and two existing regression tests
(`tests/corpus/project/{uses_leak_err,use_leak_err}`) assert exactly this
non-leaking behavior as required — reverting it to package-global would
regress that prior fix. The documented workaround (put `uses` in every file
that needs it) is simply correct usage, not a standing defect.

#90 fixed 2026-07-19: root cause was `src/LlvmGen.cpp`'s `Op::CallDyn`
codegen for a `consumed` (COW self-append, `x = x.method(...)`) receiver —
it voided the caller's window slot without releasing it, on the assumption
the callee takes the receiver's fate. True for a hand-written native row
(e.g. "add", which explicitly frees/reuses a shared receiver itself); false
for a call to an ordinary IN-LANGUAGE function (e.g. the prelude's
`Array<T>.skip`), which retains/releases its own copy of the parameter
independently and never touches the caller's reference — leaking exactly one
reference per COW self-append call to a real function. Fixed by releasing
the receiver explicitly at both in-language call sites (direct call and
by-name dynamic dispatch) in `Op::CallDyn`; native rows are unaffected (they
already handle their own consumed contract). Verified flat at N=1/20/100 on
the original repro, `fuzz/task_churn/park_inside_callback.lev` (promoted from
XFAIL-LLVM to a plain regression floor), and a new
`tests/corpus/churn/field_cow_across_methods.ext` churn-leak floor.
