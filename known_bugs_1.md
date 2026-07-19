# Known bugs — part 1 of 2 (known_bugs_1.md)

Active, unresolved bugs only. This is one half of the known-bug register;
the other half lives in `known_bugs_2.md`. The two files together hold every open
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
| P0       | #87 |
| P1       | — |
| P2       | — |
| P3       | — |

Each entry's Workaround note (inline, above) carries its own debt sites — there is no
separate `docs/footguns.md` registry (retired 2026-07-19, merged into these two files).
Once the composition corpus lands, a fixed bug also gets a red-lane repro promoted to
green under `tests/corpus/composition/` (`designs/techdesign-composition-corpus.md`).
Fixing #N means: fix, delete the entry here, promote red→green — one commit.

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

## #87 [P0] — relational comparison (`<`/`<=`/`>`/`>=`) of a `None`-valued optional against a literal produces a `bool` whose `.toString()` prints empty

**Markers:** P0.1 (all four actively-maintained engines — oracle, IR, emit-C++,
LLVM — unanimously print the wrong output for ordinary, checker-accepted user
code; wrong per the language reference regardless of cross-engine agreement,
since `bool.toString()` must yield `"true"`/`"false"`, never empty).

Found implementing `designs/expr-reification/techdesign-03-verification.md`
§4 (the differential corpus wants a None-operand `<` row, per doc 01 §3.1's
pinned semantics that any `None` operand makes a relational comparison
`false`). Repro (no `expr::` surface involved — ordinary optional-typed field
comparison):
```
class Rec { int? maybeAge; new Rec(int? a) { maybeAge = a; } }
void probe() {
    Rec r1 = Rec(None);
    bool a = r1.maybeAge < 10;
    bool b = r1.maybeAge <= 10;
    bool c = r1.maybeAge > 10;
    bool d = r1.maybeAge >= 10;
    bool e = r1.maybeAge == 10;
    console.writeln("lt=" + a.toString() + " le=" + b.toString() + " gt=" +
                     c.toString() + " ge=" + d.toString() + " eq=" + e.toString());
}
probe();
```
Prints `lt= le= gt= ge= eq=false` on `--run`, `--ir`, `--emit-cpp` (g++), and
`--native-obj`/LLVM alike — the `<`/`<=`/`>`/`>=` results are empty strings,
while the sibling `==` on the same optional prints correctly. Control flow
over the same value is unaffected: `if (r1.maybeAge < 10) { … } else { … }`
correctly takes the else branch — only the *stringified* value is wrong, and
only for the four ordering operators (not `==`/`!=`). This is a symptom
surfacing away from a different expression than the value's origin (the
comparison computes a real, correctly-branching bool, but something about its
runtime representation confuses `.toString()`'s dispatch) — consistent with
the "operand mis-tagged/mis-encoded on the None-arm of an optional relational
compare" family, not a simple wrong-value bug.

**Root cause pointer:** not yet isolated to a file/line (out of this stage's
`src/`-touching scope — verification-only, per
`designs/expr-reification/techdesign-03-verification.md`'s header rule). Likely
in the relational-operator codegen/eval path for `T?`-typed operands (`Checker.cpp`'s
comparison typing or `Eval.cpp`/`Lower.cpp`'s runtime union comparison lowering) —
the four ordering operators share a code path distinct from `==`/`!=`'s
`dbEq`-style union comparison, and only that shared path is affected.
**Workaround:** avoid relational (`<`/`<=`/`>`/`>=`) comparisons directly on an
optional-typed operand; narrow first (e.g. via `match`) before comparing. LA-31
Stage 3's differential corpus therefore pins only the `==`/`!=` None-operand
rows (doc 01 §3.1's semantics for those two operators, which are unaffected)
and excludes a `<`-against-None row pending this fix
(`designs/expr-reification/techdesign-03-verification.md` §10 implementation log).
