# fuzz/ — the ARC verification net

Two complementary tools guard the copy-on-write / reference-counting paths:

- **`alias_fuzz.py`** — catches *wrong output* (differential across three engines).
- **`churn_leak.py`** — catches *leaks* (the escaping-tier ARC must reclaim
  dropped objects). See [the churn-leak section](#churn_leak--the-leak-half-of-the-net) below.

---

# alias_fuzz — aliasing-heavy differential fuzzer

Generates random `.ext` programs that stress the copy-on-write / reference-counting
paths under aliasing shapes the fixed corpus does not cover, then runs each program
through three engines and flags any disagreement:

| engine | command | role |
|---|---|---|
| tree-walk | `leviathan --run` | oracle (reference semantics) |
| pure x86-64/ELF | `leviathan --emit-elf` | zero-dep native backend |
| emit-C++ | `leviathan --emit-cpp` + `g++ -O2` | C++ backend |

## What it generates

Random sequences over a few `Array<int>` and `Holder` (object) locals:

- `let` — fresh array/object locals, empty or with literal elements
- alias — `a1 = a0;`, `Holder o2 = o1;`, alias-at-declaration
- append — `a = a.add(v);` (the COW hot path), also inside `for` loops
- store-in-field / load-from-field — `o.f = a;`, `a = o.f;`, `o.f = o.f.add(v);`
- pass-to-function — randomly generated helpers that append to their parameter,
  alias it, or mutate a passed object's field
- drop — `a = [];`, `o = Holder();`, and scoped aliases (`if (true) { ... }`)
  whose reference dies at block exit, re-arming the unique/in-place path

Programs are fully deterministic; interleaved and final `console.writeln`
observations pin the values down, so any COW bug (a write leaking through an
alias, or a copy losing an in-place append) shows up as an output diff.

## Usage

```sh
cmake --build build -j            # the fuzzer needs build/leviathan
fuzz/alias_fuzz.py                # 100 programs, seeds 1..100, 40 ops each
fuzz/alias_fuzz.py --count 500 --ops 80
fuzz/alias_fuzz.py --seed 42 --count 1 --print-program   # inspect a program
```

Exit code is nonzero if any seed disagrees. Generation is deterministic per
seed (independent of `--jobs`), so every failure is reproducible:

```sh
fuzz/alias_fuzz.py --seed <N> --count 1
```

## Failures

Each failing seed gets a full repro under `fuzz/failures/seed-<N>/`:
`prog.ext`, per-engine stdout (`run.out`, `elf.out`, `cpp.out`), and
`report.txt` with the first differing line. Emit errors, crashes, exit-code
divergence, and timeouts are flagged the same way as output mismatches.

---

# churn_leak — the leak half of the net

The aliasing fuzzer proves the engines *agree*; it cannot prove the pure
x86-64/ELF backend *frees* what it drops. A program can print the right answer
while leaking every escaping object it allocated. `churn_leak.py` closes that gap.

Each program in `tests/corpus/churn/` allocates and drops **N escaping values
per iteration**, parameterized by a template hole `@N@`. The harness compiles
each at two magnitudes of N via `--emit-elf`, runs the binary, and reads the
escaping-tier accounting meter the backend prints to stderr at exit (ARC net #1):

```
[heap] escaping-tier peak=<P> live-at-exit=<L> bytes
```

**The spec:** `live-at-exit` must **not grow with N**. The dropped values are
unreachable; the escaping-tier ARC (§15) must reclaim them, leaving only the
N-independent root set live.

### Coverage: flat and nested/recursive graphs

- **Flat churn** (`objects`, `arrays`, `closures`) — int-typed contents; the
  baseline single-level free.
- **Nested / recursive graphs** — exercise the **recursive-release** path an
  ARC needs once it walks object interiors:
  - `nested_obj_array_obj` — a Bag holding an Array of Leaf objects (object →
    array → objects); release must recurse two levels.
  - `recursive_list` — a None-terminated linked list; release must walk the
    `next` chain to its end.
  - `map_shared_values` — a Map whose heap values include the **same object
    stored under two keys**; the owned-vs-borrowed trap — release it *once*, not
    per slot (a per-slot free is a double-free).
  - `struct_array_field` — a Bag holding a **dense `Array<Point>`** of value
    structs (§9 B1, inline). The **exclusion path**: free the array buffer but
    do **not** follow the inline struct bytes as pointers.

### Two failure modes it catches

1. **Missed release → leak.** `live-at-exit` grows with N. Cross-checked against
   `--mem-verify`'s root set (`<R> reachable at exit`), which is *constant* in N:
   when live scales with N while R stays flat, it is unambiguously an ARC failure,
   not genuinely-reachable memory, and the message says so.
2. **Over-release → double-free / use-after-free.** Surfaces two ways, both
   flagged with attribution: the binary is **killed by a signal** (reported as a
   likely over-release), or a freed-then-reused block **corrupts live data** so
   the ELF output diverges from the tree-walk oracle (the harness runs both and
   diffs). Nested and shared-value graphs are what make this reachable once
   recursive release is wired.

```sh
fuzz/churn_leak.py                       # default sweep (N in {100, 800})
fuzz/churn_leak.py --small 100 --large 800
```

Registered as the `corpus_churn_leak` ctest. **It is expected-red today** — the
escaping-tier free is landing on master in steps, and this is the target it must
hit. Today all programs fail with the leak signal (recursive release is not yet
wired) and ELF output still matches the oracle (no over-release yet). As
recursive release lands, `live-at-exit` goes flat per shape; a bug in it trips
the over-release detection instead of silently passing. A no-escaping-allocation
control already exercises the PASS path.
