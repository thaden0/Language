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
| P1       | #83 |
| P2       | #90 |
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

## #83 [P1] — implementing a dependency's interface requires bare (uses-imported) member types, and `uses` is package-global via source concatenation

**Markers:** P1.2 (the only workaround is per-use: every future driver/consumer
track — Track 06 ORM, `atlantis-postgres`, any package implementing a C3/atlantis
interface — must independently know and re-apply the pattern).

Two linked resolver behaviors surface when a package (B) implements an interface
declared in a dependency (A):

1. **Alias-qualified member types fail interface satisfaction.** If B spells a
   method's return/param type as `A::Data::Foo`, the checker reports
   `Promise<A::Data::Foo>` is *not assignable to* the interface's required
   `Promise<Foo>` — even for a non-generic `A::Data::Foo` vs `Foo`. The
   alias-qualified type and the interface's in-package bare name are treated as
   distinct identities. **Workaround:** `uses A::Data;` and spell member types
   **bare** (`Foo`); the interface-inheritance clause itself may stay qualified
   (`class Impl : A::Data::IThing`).

2. **`uses` behaves package-global.** Source files in a package are concatenated
   (the "file boundaries dissolve" invariant), so a bare name resolves only if
   **every** file that contributes to the namespace carries the `uses` — a single
   sibling file lacking `uses A::Data;` makes the bare names in *other* files read
   as `unknown type`. **Workaround:** put the `uses` line in *every* `.lev` file of
   the package, even files that do not themselves reference the imported types.

Minimal repro: pkgA `namespace A { namespace Data { class Foo{…} interface IThing{ Promise<Foo> make(); } } }`;
pkgB `uses A::Data; namespace B { class Impl : A::Data::IThing { Promise<A::Data::Foo> make(){…} } }`
→ "not assignable to required `Promise<Foo>`". Changing `A::Data::Foo` → `Foo`
(bare) compiles. Adding a second pkgB file that declares `namespace B { … }`
without its own `uses A::Data;` reintroduces `unknown type Foo` in the first file.

**Root cause pointer:** (1) type-identity comparison keys on the alias-qualified
symbol path rather than the canonical class; (2) `uses` scope is applied per
translation-unit-fragment but bare-name lookup runs over the merged namespace.
Both are worked around throughout `packages/atlantis-mysql/` (bare C3 types +
`uses Atlantis::Data;` in all eight src files); a fix would let drivers name C3
types either way. **Debt sites:** every file in `packages/atlantis-mysql/src/`
(8 files) + `packages/atlantis-mysql/tests/{loopback,pool}/main.lev`.

---

#86, #88, #89 fixed — see git history (commits prefixed `bug.md #N`) for
their resolutions.

---

## #90 [P2] — LLVM leaks ~128B per iteration when a class field `Array<T>` is mutated (`.add()`/`.skip()`) by two *separate* method calls, even though the same mutation inline or through one method does not leak

**Markers:** P2.2 (output is byte-identical and correct on every engine —
oracle/IR/LLVM all agree, `--mem-verify`'s root-set stays constant in N — only
LLVM's escaping-tier live-at-exit grows linearly with N; not P0.3/P1.1 since
nothing is silently wrong *value*-wise, only resource accounting).

Found via `fuzz/task_churn/` while implementing
`designs/complete/techdesign-http-and-streams-maturity.md` (D-B): a churn program that
calls `StreamBuffer.push()` then `StreamBuffer.pull()`/`pullRaw()` in a loop
(the ordinary shape — a producer method and a consumer method mutating the
same buffered `Array` field across two calls) leaked ~384B/iteration on
`--engine llvm`. Bisected with `git stash` to confirm it predates this
branch's changes entirely, then reduced to a repro with no relation to
streams at all:

```
class Box {
    Array<int> items;
    void push(int v) { items = items.add(v); }
    int pop() { int v = items.first(); items = items.skip(1); return v; }
}
void iterate(int seed) {
    Box b = Box();
    b.push(seed);
    int v = b.pop();
}
void run(int n) { for (int i in 1..n) iterate(i); }
run(@N@);
```

`fuzz/churn_leak.py --engine llvm` on this shape: `live-at-exit` grows
~128B/iteration (N=1 → 768B, N=20 → 3200B) while the `--mem-verify` oracle
root set stays constant — a leak, not genuine retention. Two negative
controls isolate the trigger precisely:

- **Same field, same two methods, `.add()`+`.first()`+`.skip()` all inlined
  into ONE function body instead of two methods (`Box`'s field accessed
  directly as `b.items = b.items.add(...)` etc. inside `iterate()` itself,
  no `push()`/`pop()` methods)** — flat, no leak.
- **A bare local `Array<int>` variable (not a field) mutated by
  `.add()`/`.first()`/`.skip()` inline in one function** — DOES leak, same
  rate — so the trigger is not "crossing a method boundary" alone; a bare
  local reassigned through the COW `add`/`skip` pair also leaks, while the
  *same operations threaded through an object field, done inline in one
  function*, does not.

The common factor across the leaking shapes (two-methods-on-a-field,
bare-local-inline) versus the one that doesn't (field-inline-in-one-function)
points at the LLVM backend's refcount/release codegen for a value
reassigned via `arr = arr.add(x)` / `arr = arr.skip(n)` (copy-on-write Array
ops returning a *new* backing store) — the old backing store's release
appears to be dropped on some but not all of these paths. Not isolated
further (`src/LlvmGen.cpp`'s Array COW lowering / `runtime/lv_runtime.c`'s
array release path is the likely region; out of this design's scope to dig
into further).

**Root-cause pointer:** not confirmed; likely `src/LlvmGen.cpp`'s codegen
for `Op::Call` on `Array.add`/`Array.skip` (or the COW release path in
`runtime/lv_runtime.c`) failing to emit/hit the old-backing-store release on
some call shapes. `--mem-verify`'s reachability oracle staying flat while
the escaping-tier meter grows is the standard leak signature this project's
churn nets already use to attribute (`fuzz/churn_leak.py`'s own module
docstring).

**Workaround:** none needed at the language level — output is correct on
every engine; this is resource-only. `fuzz/task_churn/park_inside_callback.lev`
(added by D-B, exercises exactly the `push()`-then-`pull()`-across-a-park
shape that surfaced this) is marked `XFAIL` citing this entry rather than
gated out of the corpus, so it converges to a guarded PASS automatically
once #90 is fixed (`fuzz/churn_leak.py`'s XFAIL/XPASS convention).
