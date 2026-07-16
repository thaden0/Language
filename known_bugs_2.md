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
| P0       | — |
| P1       | — |
| P2       | #73 |

Every open bug also carries a row in `docs/footguns.md` (workaround + debt sites) and,
once the composition corpus lands, a red-lane repro under `tests/corpus/composition/`
(`designs/techdesign-composition-corpus.md`). Fixing #N means: fix, delete the entry here,
sweep the footguns row's debt sites, promote red→green — one commit.

---

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

## #73 [P2] Reassigning a GLOBAL `Array<T>` grown element-by-element (`xs = xs.add(...)`) leaks every intermediate COW buffer on LLVM native — O(N²) live bytes, `lvrt: heap exhausted` near N≈10k

**Priority justification:** P2.2 — below the exhaustion cliff, output is
correct on every engine but memory behavior is asymptotically wrong on an
actively-maintained engine (O(N²) live bytes for O(N) data on LLVM); at the
cliff it becomes P2.3 (fails loud — `lvrt: heap exhausted`, exit 1 — on one
actively-maintained engine while oracle/IR/emit-C++ all work). No P0/P1
marker: the failure is loud at the faulting site, never a silent wrong value.

**Symptom.** Growing a global array by repeated copy-on-write reassignment:

```lev
Array<int> xs;

void build(int n) {
    int i = 0;
    while (i < n) {
        xs = xs.add(i);
        i = i + 1;
    }
}

build(10000);
console.writeln(xs.length().toString());
```

- Oracle (`--run`), IR (`--ir`), emit-C++ (`--build`): print `10000`, exit 0.
- LLVM (`--build-native`): `lvrt: heap exhausted`, exit 1, no output.
- At N=1000 LLVM completes but the escaping-tier meter shows the leak
  directly: `[heap] escaping-tier peak=10824960 live-at-exit=10824928` —
  ~10.8 MB still live for a 1000-int array (~16 KB of real data), i.e.
  every superseded intermediate buffer of the global remains live
  (Σ k·elemSize ≈ O(N²)), and the fixed 256 MiB bump heap dies near N≈10k.
- The IDENTICAL loop on a **local** array is clean (`peak=787008
  live-at-exit=576`), so reclamation works for locals; the leak is specific
  to the global-slot store path.

**Root cause (pointer).** Not traced past the symptom. The live-at-exit ≈
peak signature says superseded global-slot array payloads are never
released on native: suspicion is the LLVM `StoreGlobal` lowering (or the
runtime store path it calls) skipping the release-old of the previous
array buffer when a global is reassigned — globals allocate in the
escaping tier (`runtime/lv_runtime.c` bump heap, 256 MiB,
`LV_HEAP_BYTES`), so anything not released is live forever. The local
variant's cleanliness exonerates `Array.add` itself and the COW copy.

**Workaround (verified).** Build in a local, assign the global once:
`Array<int> tmp; ...grow tmp...; xs = tmp;` — N=10000 then completes on
LLVM with `peak=787040`. One workaround per growth site (P1.2 does not
apply: the sites are few and the rewrite is mechanical).

**Found:** reshaping `tests/corpus/tasks/park_storm.lev` for the bug #35
fix, 2026-07-15 — its 10000-element global `Array<Promise<int>>` build
dies of exactly this on native, which is why park_storm stays
interpreter-lanes-only (see the `tests/corpus/tasks_llvm` comment block in
CMakeLists.txt); promoting it rides on this fix. Red-lane repro:
`tests/corpus/composition/aggregates/red/global_array_cow_growth.lev`
(engines: llvm).
