# Tech Design: The target predicate — `target::os` / `target::arch` / `target::triple`

**Status:** IMPLEMENTED 2026-07-17 (same change as this doc). One-pager, commissioned by
`designs/complete/techdesign-metaprogramming-tail.md` §5/§8.2
(item Q — the residual half of platform-conditional `uses`). The *mechanism* (`uses` inside
a taken `comptime if` branch, folded before the imports map is computed) landed with
metaprog Phase 3 §9; this doc supplies the missing *key*: a comptime-visible constant that
reflects the compilation target. Implemented in the same change that lands this doc.

---

## 1. Decision: compiler-provided comptime string constants, not a `Platform` enum

The tracker offered two shapes: `Platform::current` as an enum with a static-side constant
(the proposal §4.3 sketch), or a compiler-provided comptime constant. **This design takes
the constants**, as the primary shape rather than a stopgap:

- `Platform::current`'s *value* is compile-configuration data — the compiler must inject
  it either way. The enum wrapper adds a prelude type, a comptime enum-comparison surface,
  and a closed variant list, for zero additional predicate power.
- A closed enum ossifies: the proposal's 2026 sketch had no `wasm` row; strings extend
  (os/arch/triple today; `abi`/`env` later, if ever needed) without a variant migration.
- Zero runtime carrier: the values exist only inside the comptime oracle and fold away
  before pass 2 — consistent with the hermetic-oracle stance and with `meta::` (the
  existing reserved comptime namespace precedent, F4). Nothing new reaches any backend.
- The Track 03 statics dependency **dissolves instead of being paid**. If the owner later
  wants the `Platform::Linux` aesthetic, a prelude enum can be layered over these
  constants as sugar; nothing here blocks it.

## 2. Surface

Three constants in the reserved comptime namespace `target` (family with `meta`):

| constant | type | value |
|---|---|---|
| `target::os` | string | `"linux"`, `"windows"`, `"macos"`, `"wasm"`, or `"unknown"` |
| `target::arch` | string | first triple component, normalized (`"x86_64"`, `"aarch64"`, `"wasm32"`, …) |
| `target::triple` | string | the exact `--target` string; host builds get the canonical host spelling |

The motivating use — platform-conditional `uses` — works the day the key exists, because
the P3 §9 fold already runs before the imports map is computed:

```
comptime if (target::os == "windows") {
    uses App::WinConsole;
} else {
    uses App::PosixConsole;
}
```

## 3. Semantics

- **Target, not host** (the portable-pivot requirement): the values derive from the
  `--target <triple>` cross flag (portable backend B-M4) when present, else from the host
  the compiler was built for. Cross-compiling `--target x86_64-pc-windows-gnu` folds the
  `"windows"` branch even on a Linux host.
- **Normalization:** `os` — a triple whose arch component starts `wasm` → `"wasm"`;
  containing `windows`/`mingw` → `"windows"`; `darwin`/`macos`/an `apple` vendor →
  `"macos"`; `linux` → `"linux"`; else `"unknown"`. `arch` — the first component, with
  `arm64` normalized to `aarch64`. `triple` is not normalized (it is the escape hatch —
  predicates that care about libc/abi match on it directly).
- **Comptime-only:** the reads are intercepted by the comptime oracle (same seam as
  `meta::parseExpr`). Everywhere the oracle evaluates — `comptime if` conditions,
  `comptime` var/expr inits, rule `where` clauses, macro bodies — the constants are live.
  In runtime position no `target` symbol exists; the ordinary unresolved-name behavior is
  unchanged.
- **Reserved, shadowable by locals:** in comptime member position, an unshadowed base name
  `target` means the compiler's namespace (exactly `meta`'s rule). A *local* named
  `target` shadows it (consistent with the oracle's other name-guarded fast paths). A user
  `namespace target` is not an error but cannot be read through `target::…` in comptime
  code — same stance as `meta`.
- **Unknown member is loud:** `target::<anything else>` in comptime code is an eval error
  naming the three constants. (Grounded: before this change, `target::os` in a `comptime
  if` silently evaluated to void and the condition folded false — a silent-no-fire trap;
  the reserved-namespace error kills it.)

## 4. Implementation map (single small commit)

| file | change |
|---|---|
| `src/Eval.hpp` | `ComptimeOptions.targetTriple` (empty = host); `Evaluator` stores derived `targetOs_/targetArch_/targetTriple_` |
| `src/Eval.cpp` | `setComptime` derives the three via a triple-parsing helper (host fallback from predefined macros); `ExprKind::Member` comptime intercept |
| `src/main.cpp` | thread the existing `--target` flag value into `ComptimeOptions` |
| `docs/reference.md` | §6.9 addition: the `target::` constants |
| tests | corpus `meta/target_uses.lev` (+`.expected`, oracle==IR==LLVM, roundtrip-swept); metatests: cross-triple folds the windows branch (target-not-host proof), unknown-member error, local-shadow |

## 5. Acceptance

1. `comptime if (target::os == …)` selects a `uses` branch; corpus green on oracle, IR
   interpreter, and LLVM native; the program round-trips through `--expand`.
2. A metatest compiling with `targetTriple = "x86_64-pc-windows-gnu"` on a Linux host
   folds the windows branch — the predicate provably reads the **target**.
3. `target::bogus` errors, naming `os`/`arch`/`triple`.
4. Zero-cost guard unchanged: programs without metaprogramming never construct the oracle;
   nothing new is reachable at runtime.

---

*Closes item Q of `designs/complete/techdesign-metaprogramming-tail.md`. Companions:
`designs/complete/techdesign-metaprog-phase3.md` §9 (the fold-before-imports mechanism),
`designs/complete/techdesign-portable-backend.md` (B-M4 `--target`, the cross-emission
flag this predicate sources).*
