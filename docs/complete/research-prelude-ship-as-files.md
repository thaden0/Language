# Research: Ship the Stdlib Prelude as `.lev` Files (real `parsePrelude()` file seam + per-target selection)

**Status:** research input for a tech design. Not a design; carries no rulings.
**Feeds:** `designs/requests/request-prelude-ship-as-files.md` (owner ruling landed
`476135f` 2026-07-19, `info.md` §19 #18: "the stdlib ships as `.lev` files... per-target
selection is a packaging detail within that model").
**Date:** 2026-07-19. **Author target:** whoever writes the tech design for this ticket
(opus complexity per `docs/policies.md` — core-compiler decision-making).
**Grounding:** current tree, branch `agent0`. Every file:line citation below was read
fresh from the current source. Where the request's own text asserts something about the
code (notably its description of the runtime-archive resolution precedent), this
document checked it against the code and flags the one place it doesn't match.

This assembles everything needed to write the design: the exact current shape of
`parsePrelude()` and the eight segments (full inventory, sizes, cross-reference check),
the *actual* runtime-archive resolution precedent the request asks to mirror (corrected —
it is narrower than the request assumes), how and where the LLVM target triple is or
isn't available at the point the prelude is parsed, the build system's current total
absence of any file-shipping/embedding machinery, a full survey of every test harness
that invokes `build/leviathan` and the CWD/path assumptions each makes, the diagnostic/
span-attribution implications of the concatenation, and a menu of concrete options for
each of the four decisions the request flags as needing an explicit owner ruling.

---

## 0. Contents

1. Executive summary
2. Current state: `parsePrelude()` and the eight segments
3. The runtime-archive resolution precedent — what it actually does (corrected)
4. Target-triple availability at the `Resolver` call site
5. Per-target selection semantics: the `Dom`/`kPreludeWasm` case study
6. Build system survey: what exists today for shipping non-source-code files (nothing)
7. Test harness survey: every consumer of `build/leviathan`'s CWD assumptions
8. Diagnostics and span attribution: what the concatenation currently guarantees
9. The embedded-fallback design space
10. Blast-radius and staging considerations
11. Cross-references: Track W, `info.md`, `docs/reference.md`
12. File-level change map
13. Open questions for the design doc to rule on

---

## 1. Executive summary

The request is well-specified and the owner ruling is already recorded (`info.md`
§19 #18, quoted above) — the *shape* of the answer ("ship `.lev` files, real file-reading
seam, per-target selection as a packaging detail within that model") is settled. What
remains is mechanism, and every piece of mechanism the request asks about is answerable
from the current tree:

- **The eight segments are real, in Resolver.cpp, in the concatenation order the request
  lists**, and — critically — **there are zero cross-segment symbol dependencies out of
  `kPreludeWasm`** (grep-verified, §5): nothing outside `kPreludeWasm`'s own 119 lines
  references `Dom`. Excluding it from a native build breaks nothing.
- **Whole-program resolution genuinely makes segment order irrelevant**
  (`Resolver.cpp:11-14`'s own comment, and confirmed structurally — see §2), so per-file
  vs. concatenated-buffer parsing is a free choice, not a correctness constraint.
- **The request's assumed resolution precedent is slightly wrong.** It describes "env
  override → next-to-binary → source-tree-relative → install prefix." The actual
  runtime-archive resolver (`src/main.cpp:141-167`) is **`--runtime <path>` CLI flag
  override → next-to-binary (via `/proc/self/exe`) → (per-triple only) one source-tree-
  relative fallback (`../runtime/<triple>/`)**. There is **no environment variable** in
  the existing precedent, and **no install-prefix search** — this codebase has no
  `install()` directives anywhere and no installed-tree concept yet. A design that wants
  `LV_PRELUDE_DIR` as an env var and an install-prefix search tier would be *adding* two
  tiers beyond precedent, not mirroring it. That may still be the right call, but the
  design doc should say so explicitly rather than cite it as "the same as the runtime
  archive."
- **The target triple is already in scope where it's needed.** `main()` parses
  `--target` into a local `std::string targetTriple` (`src/main.cpp:226`) before
  constructing the `Resolver` (`src/main.cpp:446`), for every mode (`--run`, `--ir`,
  `--build-native`, etc. all share the same construction site). `Resolver`'s constructor
  currently takes only `(file, sink)` (`Resolver.hpp:21`) and has no notion of a target.
  Threading it in is a small, mechanical constructor/field change — not a "does the
  Resolver have access" open question, just an "add the parameter" task.
- **The build system has zero existing precedent for shipping or embedding a non-.cpp
  file.** No `install()` directive exists anywhere in `CMakeLists.txt`; no
  `file(GENERATE)`/`configure_file`/`xxd`/`incbin`-style embedding of external file
  content exists either (the closest relative, `lvrt.link`, is CMake-*generated* content,
  not a copied file). This means the "guarantee-by-construction that the files are always
  found" option in the request is *new* CMake plumbing, not a small addition to existing
  plumbing — the design should size it accordingly.
- **Every test harness resolves the binary via `$<TARGET_FILE:leviathan>` (an absolute
  path) or an absolute path passed as `$1`**, and the binary itself locates its siblings
  via `/proc/self/exe`, never `argv[0]` or CWD (§7). This means CWD-independence, which
  the request calls out as a risk, is **already the existing discipline** for the runtime
  archive and can be copied verbatim for the prelude directory.
- **No test pins the literal string `<prelude>`** (`preludeFile_.name`) anywhere in
  expected output (§8) — diagnostics never surface it because the Checker never runs on
  the prelude (`prelude-not-checked`, confirmed still true at `main.cpp:446-543`, the
  Checker runs only on `program`, never on `preludeProgram_`). This significantly lowers
  the risk of the "span attribution must stay coherent" concern the request raises: there
  is no committed golden output to preserve either way.

Net: this is a well-bounded, mechanically clear refactor with one genuinely new piece of
work (the build-system file-shipping mechanism, since none exists) and one genuinely open
design question with real safety weight (embedded-fallback vs. files-only, §9) — exactly
the one the request already flags as "the load-bearing safety decision."

---

## 2. Current state: `parsePrelude()` and the eight segments

`src/Resolver.cpp` lines 5913–5921, in full:

```cpp
Program Resolver::parsePrelude() {
    preludeFile_.name = "<prelude>";
    preludeFile_.text = std::string(kPreludeCore) + kPreludeStd +
                        kPreludeRest + kPreludeRegexCore + kPreludeRegexApi + kPreludeWeb +
                        kPreludeWasm + kPreludeExpr;
    DiagnosticSink dummy;  // the prelude is trusted; ignore its diagnostics
    Lexer lexer(preludeFile_, dummy);
    Parser parser(lexer.tokenize(), preludeFile_, dummy);
    return parser.parseProgram();
}
```

Called exactly once, from `Resolver::run` (`src/Resolver.cpp:7244`):

```cpp
void Resolver::run(Program& program) {
    desugarEnums(program);
    synthesizeFloatNaN(program);
    sema_.global = sema_.newScope(nullptr);
    addToScope(sema_.global, sema_.newSymbol(SymbolKind::Primitive, "void"));
    addToScope(sema_.global, sema_.newSymbol(SymbolKind::Primitive, "None"));

    preludeProgram_ = parsePrelude();
    gatherInto(preludeProgram_.items, sema_.global);
    // Boundary between prelude and program classes in `classSymbols_`: only the
    // program's classes get file-scoped below (prelude spans live in a separate
    // buffer whose offsets would otherwise collide with the program's).
    size_t preludeClassCount = classSymbols_.size();
    gatherInto(program.items, sema_.global);
    ...
```

`Resolver` itself (`src/Resolver.hpp:19-53`) is constructed as `Resolver(const SourceFile&
file, DiagnosticSink& sink)` — the file being *compiled*, not the prelude. `preludeFile_`
is a private member (`SourceFile preludeFile_;`, line 44) populated entirely inside
`parsePrelude()`. `preludeProgram()` is exposed as an accessor (line 33) for the IR
lowerer, which lowers prelude declarations into every module (see §5's note on
`LlvmGen.cpp:740`).

### 2.1 The eight segments (measured, this tree)

| Segment | Declared at | Raw-string span (approx., incl. header comment) | Role |
|---|---|---|---|
| `kPreludeCore` | `Resolver.cpp:22` | ~1,100 lines | Primitive object masks (int/string/bool/float method shapes) + collections + `Seq` + streams glue |
| `kPreludeStd` | `Resolver.cpp:1122` | ~1,666 lines | `namespace std` — sys floor, aggregates, promise/timer, sockets+HTTP, files, exception hierarchy |
| `kPreludeRest` | `Resolver.cpp:2788` | ~678 lines | Top-level `Range`/`StreamBuffer`/`Console` + `meta`/`math`/`env`/`term` |
| `kPreludeRegexCore` | `Resolver.cpp:3466` | ~688 lines | Track 10 regex engine internals (`namespace regex`) |
| `kPreludeRegexApi` | `Resolver.cpp:4154` | ~465 lines | Track 10 regex public surface (`Regex`/`Match`/`Group`/`RegexOptions`/`RegexException`) |
| `kPreludeWeb` | `Resolver.cpp:4619` | ~1,004 lines | Track 09 web foundations — encoding, digest, `DateTime`, `json` |
| `kPreludeWasm` | `Resolver.cpp:5623` | ~119 lines | Track W W-M3 JS/DOM bridge (`Dom`, `DomNode`, `DomEvent`, `std::sysHost*`) — **the per-target candidate** |
| `kPreludeExpr` | `Resolver.cpp:5742` | ~1,620 lines | LA-31 expression reification (`expr::Expr<F>`) |

(Line counts above are inter-declaration deltas, so each includes that segment's leading
banner comment; the request's "~5,860 lines" figure likely counts only in-string content.
Either way, the request's request-details table (segment start lines) matches the current
tree exactly — no drift since the request was filed.)

The file's own header comment (`Resolver.cpp:5-21`) already documents the segmentation
rationale and is worth quoting because it is the authority for "order doesn't matter":

```cpp
// Track 09 §0 (prelude-scale decision): the prelude is split into several
// adjacent raw-string segments concatenated at gather (parsePrelude below).
// This is a mechanical, zero-semantic partition — whole-program resolution
// (§12) makes segment order irrelevant, so the seams are just organizational.
```

This is confirmed structurally, not just by comment: `gatherInto` (called once over the
whole `preludeProgram_.items` at `Resolver.cpp:7245`) builds every declaration's scope
entry in a single pass before anything is resolved, and resolution itself is a second
pass over the fully-gathered symbol table. Nothing in `gatherInto`/`processImports`/type
resolution depends on which segment a declaration's text originated from, only on which
*scope* it's gathered into (all segments gather into the same `sema_.global`). This means
splitting into files and reassembling in a **different concatenation order** than today
would not change program semantics — a useful degree of freedom if, e.g., a design wants
wasm-only segments concatenated last for clarity.

---

## 3. The runtime-archive resolution precedent — what it actually does (corrected)

The request asks the design to mirror "the same way the driver already resolves the
runtime archive (env override → next-to-binary → source-tree-relative → install
prefix)." The actual code, `src/main.cpp:125-167`:

```cpp
// B-M2 (doc-2 §4 item 4): the `leviathan` executable's own directory, via
// /proc/self/exe rather than a baked-in build-dir path so a relocated install
// still finds its siblings. Linux-only (driver code, not runtime) — ported
// alongside the rest of the driver at B-M5. Returns "" on failure.
static std::string exeDir() {
    char exePath[PATH_MAX];
    ssize_t len = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len <= 0) return "";
    exePath[len] = '\0';
    std::string dir(exePath);
    size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) return "";
    return dir.substr(0, slash);
}

// Host runtime archive: liblvrt.a next to the `leviathan` executable.
static std::string findRuntimeArchive() {
    std::string dir = exeDir();
    if (dir.empty()) return "";
    std::string candidate = dir + "/liblvrt.a";
    struct stat st;
    if (::stat(candidate.c_str(), &st) == 0) return candidate;
    return "";
}

// B-M4 (doc-2 §6 item 2): per-triple runtime archive. `runtime/build-triple.sh
// <triple>` cross-compiles the runtime into `runtime/<triple>/liblvrt.a`; the
// driver resolves it "by triple". Two layouts are honored so both an installed
// tree (archives colocated next to the host one) and the in-source dev build
// (build-triple.sh's default `runtime/<triple>` output, reached from build/
// via ../runtime) work without configuration. `--runtime` overrides entirely.
static std::string findRuntimeArchiveForTriple(const std::string& triple) {
    std::string dir = exeDir();
    if (dir.empty()) return "";
    std::string candidates[] = {
        dir + "/" + triple + "/liblvrt.a",             // installed / colocated
        dir + "/../runtime/" + triple + "/liblvrt.a",  // in-source dev build
    };
    struct stat st;
    for (const std::string& c : candidates)
        if (::stat(c.c_str(), &st) == 0) return c;
    return "";
}
```

And the override, at the call sites (`src/main.cpp:655-657`, `699-702`):

```cpp
std::string runtimeLib = runtimePathOverride
    ? runtimePathOverride
    : findRuntimeArchiveForTriple(targetTriple);
```

`runtimePathOverride` is populated from the CLI flag `--runtime <path>`
(`src/main.cpp:225`, `302-303`) — **a command-line flag, not an environment variable.**
There is no `std::getenv` call anywhere in `main.cpp` related to runtime/archive/prelude
resolution (the only `getenv` in the file is for `PATH`, used by the linker-driver probe,
`main.cpp:39`).

So the actual, precise precedent is three tiers, not four, and the "env override" tier
the request names doesn't exist yet for anything in this codebase's driver:

1. **Explicit CLI flag** (`--runtime <path>`) — absolute override, wins unconditionally.
2. **Next-to-binary**, resolved via `/proc/self/exe` (not `argv[0]`, not CWD) — this is
   the one tier that's identical for host and per-triple lookups.
3. **One source-tree-relative fallback**, `../runtime/<triple>/`, and *only* for the
   per-triple lookup (the host lookup has no third tier at all — a missing
   `liblvrt.a` next to the binary is just an error).

There is no fourth "install prefix" tier because **there is no install concept in this
codebase yet** (§6 confirms no `install()` directives exist anywhere). If the design
wants an env var (`LV_PRELUDE_DIR`) and/or an install-prefix search, it should be
explicit that it is establishing a *new*, richer pattern — one the runtime-archive
resolver could later be retrofitted to match, but does not currently exemplify. This
doesn't argue against the richer pattern (an env var for prelude override is reasonable
and cheap), it just means "mirror the precedent" isn't literally achievable for all four
tiers the request lists — three of the four tiers exist to mirror; the fourth (env var)
and fifth (install prefix, if wanted) would be new.

---

## 4. Target-triple availability at the `Resolver` call site

`main()`'s argument-parsing loop populates a local:

```cpp
const char* targetTriple = "";     // --target <triple>: cross emission (B-M4)
...
else if (std::strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
    targetTriple = argv[++i];
}
```

(`src/main.cpp:226`, `305-306`.) This happens in the single argument-parsing pass at the
top of `main()`, **before** any compilation work. The `Resolver` is constructed later in
the same function, in the non-special-mode branch that handles every ordinary
compile/run/build invocation:

```cpp
Resolver resolver(file, sink);
resolver.setFileRanges(fileRanges);
resolver.run(program);
```

(`src/main.cpp:446-448`.) `targetTriple` is an ordinary local variable of `main()`, in
scope at this call site — there is no threading problem to solve at the `main.cpp` level;
`targetTriple` is simply available to pass in. The gap is entirely on the `Resolver` side:
its constructor (`Resolver.hpp:21`) takes only `(file, sink)` and has no field to receive
it. The mechanical fix is a third constructor parameter (or a setter mirroring
`setFileRanges`), stored as a private member, read inside `parsePrelude()` to decide
which per-target files to include.

One wrinkle worth the design's attention: `--target` is parsed and stored **regardless of
mode** — it's set even for `--run`/`--ir`/`--tokens`, not just `--build-native`/
`--native-obj`. Verified against actual usage: `tests/run_wasm.sh` and
`tests/run_wasm_dom.sh` both pass `--target wasm32-unknown-unknown` only alongside
`--build-native`/`--native-obj`; the `--ir` invocation used as their comparison baseline
(`run_wasm.sh:113`, `"$bin" --ir "$f"`) is deliberately called **without** `--target`, so
it parses/resolves/interprets against the *native* (Dom-excluding) prelude — consistent
with Dom being wasm-only and the DOM lane (`run_wasm_dom.sh`) not diffing against `--ir`
at all (it diffs against committed `.expected` goldens instead, per its own header
comment). This means a design that keys per-target inclusion purely off whatever
`targetTriple` happens to be at `Resolver` construction time will do the right thing for
every existing test invocation pattern with no special-casing — but the design should
still state explicitly that `--target` with `--run`/`--ir` (compile-for-one-triple,
execute-on-host) is a real, exercised combination, not just a `--build-native` detail.

`RuleEngine`/`ComptimeOptions` already carries a `targetTriple` field independently
(`copts.targetTriple = targetTriple;`, `main.cpp:466`, feeding the `target::os`/`target::arch`
comptime predicate machinery, `info.md` item Q, "Metaprog tail CLOSED" per memory). That is
a **separate plumbing path** from what `Resolver::parsePrelude()` would need — `ComptimeOptions`
is constructed later, only when `program.hasMeta` is true, and only consumed by the
`RuleEngine`, not the `Resolver`. The two should not be conflated; `Resolver` needs its own
copy of the triple (or a shared holder both read from), not a dependency on
`ComptimeOptions`.

---

## 5. Per-target selection semantics: the `Dom`/`kPreludeWasm` case study

`kPreludeWasm`'s banner comment (`Resolver.cpp:5612-5622`) is directly on point:

```cpp
// --- kPreludeWasm: Track W W-M3 the JS/DOM bridge surface -----------------
// techdesign-05-dom-bridge.md §2-§6 / hard-06-hostbridge-seam.md. The
// hand-written v1 `Dom` prelude surface (doc 05 §2): opaque JS values wrapped
// in handle classes (an `int` slot, nothing more, §2), reached through the
// generic `std::sysHost*` bridge natives (hard-06), with DOM events surfaced as
// §13 stream endpoints (§6). Shipped in the SHARED prelude for now (doc 05 §1 /
// overview §5: "ordinary prelude code until the packaging ruling" — W-M4/doc 06
// §2 makes it a per-target segment). On native targets the bridge natives raise
// (a wasm-gained capability); nothing in the shared corpus reaches them, so the
// four-lane differential is unaffected. Every symbol is namespaced (`Dom`,
// `std::sysHost*`) so it cannot collide with existing surface.
```

Two claims worth independently verifying (both confirmed):

**No cross-segment dependency out of `kPreludeWasm`.** `grep -n "Dom\b" src/Resolver.cpp`
returns matches only inside the `kPreludeWasm` segment itself (lines 5612–5730 range);
nothing in `kPreludeCore`/`Std`/`Rest`/`RegexCore`/`RegexApi`/`Web`/`Expr` references
`Dom`. This means excluding the file that becomes `kPreludeWasm` from a native build's
concatenation is a pure subtraction — no other segment needs it defined to parse or
resolve.

**Correctness is already handled independently of inclusion — this is a size/cleanliness
change, not a correctness fix.** This is the point Track W's own docs make repeatedly
(`techdesign-06-bindgen-and-ship.md` §2, `info.md` §20, `techdesign-00-overview.md` §3's
capability matrix) and it's worth the design doc restating plainly: the "doc-02 capability
gate" (`LlvmGen.cpp:174`'s `targetWasm` flag and the `wasmGatedNative` table referenced at
`LlvmGen.cpp:2662`) already produces a compile-time diagnostic if *user* code on a native
target reaches a wasm-only native, and a runtime trap-stub for prelude-only/unreachable
uses — independent of whether `Dom`'s *declaration* is present in the prelude at all.
Today, with `Dom` unconditionally concatenated into every target's prelude, this gate is
what keeps native builds correct despite `Dom` being present-but-unusable. Once per-target
selection lands and native builds simply never parse `Dom`, the gate becomes redundant
*for this specific case* (there's nothing left to gate — the symbol doesn't exist), but
the gate mechanism itself is not being removed or touched by this refactor, and it's still
needed for the OS-only natives that (per the request) may or may not be excluded from wasm
builds in v1.

This has a design-relevant consequence for `Resolver.cpp:7237-741`'s userReach comment in
`LlvmGen.cpp`:

```cpp
// Track W hard-03 tier 1: user-reachability — the SAME edges as the
// walk above, but rooted at @main only. @ginit (prelude + namespace
// global initializers) is deliberately NOT a root: a gated native
// reached only through prelude initialization is tier 2's runtime
// trap, never a compile-time brick (the whole prelude lowers into
// every module — Resolver.cpp's parsePrelude concatenation — so a
// blanket compile-time fail would brick every wasm build).
```

This comment's premise ("the whole prelude lowers into every module") is exactly what
per-target selection changes for the excluded segments — after this refactor, the whole
prelude no longer lowers into every module; only the target-applicable subset does. The
comment's *conclusion* (don't make `@ginit` a compile-time-fail root) doesn't need to
change — it's about OS-only natives reachable only via prelude init on the *wasm* side,
which is a different (still gated, still not selected-out in v1 per the request) case —
but the design doc should note this comment's premise is stale post-refactor so a future
reader doesn't trust it verbatim. Not a required code change, just a documentation
consistency item to flag (`docs/policies.md`'s "if your changes update any language
functionality, update docs/reference.md" duty extends in spirit to comments whose factual
premise the change invalidates, though this one comment is arguably fine to leave since
its reasoning about `@ginit` remains correct).

**What "per-target" needs to key on.** The existing `isWasmTriple` helper is already
exactly the right shape and is unexported (`static`, `main.cpp:85-87`):

```cpp
static bool isWasmTriple(const std::string& triple) {
    return triple.rfind("wasm32", 0) == 0;
}
```

If the `Resolver` needs the same predicate, either duplicate this one-liner (it's tiny and
`main.cpp`-local statics aren't visible to `Resolver.cpp`) or lift it to a shared header —
a call the design doc should make explicitly rather than leave implicit, since the request
flags "confirm whether OS-only exclusion is in-scope for v1" as open. If v1 stays
wasm-inclusion-only (no OS-only exclusion), a single boolean ("is this a wasm target")
threaded into `Resolver` suffices and no general triple-taxonomy is needed yet.

---

## 6. Build system survey: what exists today for shipping non-source-code files

**No `install()` directive exists anywhere in `CMakeLists.txt`** (verified: `grep -n
"^install(" CMakeLists.txt` returns nothing). There is no installed-tree concept in this
build at all today — everything runs from the build directory.

**No file-embedding mechanism exists either.** Searched for the usual C++ patterns
(`xxd`, `incbin`, `file(READ ...)` used to slurp a file into a CMake variable for
`configure_file`, any custom embed script) — none present. The nearest relative is
`file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/lvrt.link" CONTENT "${LVRT_LINK_FLAGS}\n")`
(`CMakeLists.txt:1562`), but that generates a small string CMake itself computed
(the TLS link flags), not a copy of an existing file's content — not reusable as a
pattern for turning `.lev` files into C++ constants even if the design wants an
embedded fallback.

**Where the executable and archive currently land.** `add_executable(leviathan
src/main.cpp)` (`CMakeLists.txt:78`) and `add_library(lvrt STATIC ...)`
(`CMakeLists.txt:1526`) are both declared at the top level of the single-directory
project (no `add_subdirectory`), and neither has an explicit
`RUNTIME_OUTPUT_DIRECTORY`/`ARCHIVE_OUTPUT_DIRECTORY` override (verified — no such
`set_target_properties` calls exist for either target). CMake's default behavior in a
single-directory project places both outputs directly in `CMAKE_BINARY_DIR`, which is
exactly why `findRuntimeArchive()`'s "next to the binary" (`exeDir() + "/liblvrt.a"`)
works with zero configuration: `leviathan` and `liblvrt.a` are siblings in the build
directory by CMake's default layout, not by any explicit copy step. There's also no
`add_dependencies(leviathan lvrt)` — the comment at `CMakeLists.txt:353` explains this is
deliberate ("liblvrt.a isn't next to the binary [until both are built], so ordering
against the lvrt target is not required for the test to be meaningful" — tests that need
the archive self-skip if it's absent rather than forcing a build order).

**Implication for the design.** Shipping a `prelude/` directory of `.lev` files "next to
the binary" the same way has no existing single-line precedent to copy — CMake doesn't
automatically place a source-tree directory next to a build output the way it does for
co-located build *targets*. The design needs one of:

- `file(COPY ...)` at configure time (copies once; stale if the `.lev` files change
  without a reconfigure — CMake reruns configure automatically when a file it globbed
  changes, if globbing was used with `CONFIGURE_DEPENDS`, but plain `file(COPY)` has no
  dependency tracking of its own),
- a custom target + `add_custom_command(... COMMAND ${CMAKE_COMMAND} -E copy_directory
  ...)` with proper `DEPENDS`/`OUTPUT` so it's incremental and dependency-tracked (the
  more correct option, more code), or
- **just read the files from the source tree directly** (`${CMAKE_SOURCE_DIR}/prelude/`)
  and skip copying into the build tree altogether for dev builds, reserving a real
  install-time copy for whatever "install prefix" tier the design decides to add. This
  sidesteps the copy-staleness problem entirely for the by-far-most-common case (building
  and testing in-tree) at the cost of needing a source-tree-relative resolution tier
  (which `findRuntimeArchiveForTriple` already has one instance of, `../runtime/<triple>/`,
  so there's at least a shape to imitate even though it's not literally an install-prefix
  tier).

None of these is implemented today; whichever is chosen is new CMake work, not a
one-line addition.

---

## 7. Test harness survey: every consumer of `build/leviathan`'s CWD assumptions

The request states "every test harness must find the files" and lists specific scripts.
Full survey of `tests/run_*.sh` (34 scripts) plus the `add_test(...)` call sites in
`CMakeLists.txt` (189 `add_test` invocations):

**How the binary path reaches every script.** Every `add_test` that runs `leviathan`
passes `$<TARGET_FILE:leviathan>` — a CMake generator expression that resolves to the
*absolute* path of the built binary — either directly as the test's `COMMAND`, or as
`$1` to a `tests/run_*.sh` wrapper script (e.g. `CMakeLists.txt:920-924`,
`run_wasm.sh`'s own usage line `# usage: run_wasm.sh <leviathan-binary> <file-or-dir>
...`). No test relies on `leviathan` being on `PATH`, in the CWD, or found via any
relative lookup — the absolute path is always threaded in explicitly. `ctest` itself,
when run without `-j`/other options, executes each test's `WORKING_DIRECTORY` (unset here
for basically all tests → defaults to wherever `ctest` was invoked from, typically
`CMAKE_BINARY_DIR`, but this doesn't matter because the binary path is absolute).

**How the binary finds its own siblings.** Independent of CWD or `argv[0]`, via
`exeDir()`'s `/proc/self/exe` readlink (`main.cpp:129-138`), confirmed Linux-only by its
own comment ("Linux-only (driver code, not runtime)"). This is the mechanism the request
wants the prelude directory to reuse, and it's already CWD-agnostic — a script invoked
from any directory, with any CWD, still resolves `leviathan`'s own directory correctly.
This directly answers the request's concern ("all invoke `build/leviathan` from varying
CWDs") — the *existing* archive-resolution mechanism already doesn't care about CWD, so a
prelude-directory resolver built the same way inherits that property for free.

**Scripts specifically named in the request**, each confirmed:
- `run_wasm.sh` (`tests/run_wasm.sh:36`, `bin="$1"`) — builds via `"$bin" --build-native
  ... --target wasm32-unknown-unknown "$f"`, always with an absolute `$bin`.
- `run_wasm_dom.sh` (`tests/run_wasm_dom.sh:21`, `bin="$1"; shift`) — same pattern, DOM
  corpus only (`dom_*.lev`/`dom_*.ext` globs), always `--target wasm32-unknown-unknown`.
- `run_elf.sh` — **out of scope for code changes** (frozen X64/ELF backend, per the
  request's own warning and the standing `X64Gen frozen` house rule); still worth noting
  it takes the binary path the same way, for completeness, but the prelude refactor must
  not touch it or its invocation.
- `run_sysnatives.sh` (`tests/run_sysnatives.sh:12`, `bin="$1"`) — no `--target`, always
  native/host prelude.

No script constructs its own path to a prelude directory today (none exists yet, so
there's nothing to grep for) — this is purely forward-looking: whatever resolution
`main.cpp` implements, every script inherits automatically because none of them bypass
the `leviathan` binary to reach prelude content directly.

---

## 8. Diagnostics and span attribution: what the concatenation currently guarantees

The request raises a specific risk: "if files are parsed separately, span attribution
(`<prelude>` file name) and error reporting must stay coherent."

Current mechanism: `preludeFile_.name = "<prelude>"` (`Resolver.cpp:5914`) is a single
`SourceFile` (`Source.hpp:16-19`, just `{name, text}`) whose `text` is the full
concatenation of all eight segments. `LineCol lineColAt(...)` (`Source.hpp:31`) does a
linear scan over `text` to compute a 1-based line/column for any byte offset — meaning
today, a diagnostic anchored to, say, a declaration inside `kPreludeWeb` reports a line
number that is `kPreludeWeb`'s offset *within the concatenated buffer*, not its offset
within the original `kPreludeWeb`-labeled source region. This is already somewhat
"incoherent" relative to the eventual per-file source, but it doesn't matter today because:

- **The Checker never runs on the prelude** (`prelude-not-checked`, reconfirmed by reading
  `main.cpp`'s pipeline end-to-end: `checker.run(program, ...)` at `main.cpp:542-543`
  takes only `program`, never `preludeProgram_` or `resolver.preludeProgram()`) — so no
  Checker diagnostic can ever originate from prelude text.
- **`grep -rn "<prelude>" src/*.cpp tests/`** finds exactly one hit, the assignment site
  itself (`Resolver.cpp:5914`) — nothing else in the compiler or test suite constructs,
  parses, or asserts against that literal string.
- **No corpus `.expected` file contains the word "prelude"** (checked both
  `tests/corpus/**/*.expected` and `tests/*.expected` — zero matches) — so there is no
  committed golden output whose byte content depends on prelude line/column numbers.

**Conclusion for the design:** the "stay coherent" concern is real in principle (a
Resolver-only diagnostic — e.g. a duplicate-declaration or unresolved-type error inside
prelude text, which *can* still surface if a prelude bug is introduced, since `gatherInto`/
resolution *do* run on `preludeProgram_`, only the Checker skips it) but has **zero
existing test surface pinned against it**. This means:
- The request's own suggested default ("prefer concatenating file *contents* into the one
  `preludeFile_`, minimal change, unless there's a reason to parse per-file") is safe to
  take with confidence — it exactly preserves today's `LineCol` behavior (one buffer, one
  linear scan), and nothing regresses because nothing was pinned to per-segment line
  numbers to begin with.
- If the design instead chooses one-`SourceFile`-per-`.lev`-file (nicer diagnostics,
  more invasive), the invasiveness is in `Resolver`/`Lexer`/`Parser`'s current
  single-`SourceFile`-per-parse assumption (`Lexer lexer(preludeFile_, dummy);` takes one
  `SourceFile` by reference) — every one of those would need to either loop and merge, or
  the `SourceFile`/span model would need multi-file support (which the *user program*
  side already has via `fileRanges_`/`Resolver::setFileRanges` for the bug.md #8 lexical
  `uses`-scoping fix — that machinery exists and could plausibly be reused/adapted rather
  than invented fresh, since it already solves "one buffer, multiple file-scoped offset
  ranges" for exactly this kind of multi-source-file-in-one-parse scenario).

---

## 9. The embedded-fallback design space

The request's own framing of this as "the load-bearing safety decision" is correct — a
missing/misresolved prelude directory means **every single compile on every one of the
four active worktrees fails**, immediately, with no gradual-degradation path (there's no
"compile without the prelude" mode; `int`/`string`/etc. are declared as classes *in* the
prelude — `Resolver.cpp:23`'s comment: "primitive object masks... value types that carry
a method shape" — so a missing prelude isn't a missing-library-function problem, it's a
missing-primitive-types problem, total compiler failure).

Options, grounded in what exists:

**(a) Files-only, loud failure.** Delete the `kPreludeXxx` C++ constants entirely; if the
directory can't be found, print a specific diagnostic and exit non-zero (satisfies
acceptance criterion 2's "never a silent empty prelude"). Simplest code, but per the
request's own framing, this makes correctness depend entirely on CMake/install plumbing
being right in every environment, forever, including ones this design can't fully
enumerate today (§6 established that plumbing doesn't exist yet — this option bets
everything on getting it right the first time and keeping it right).

**(b) Embedded fallback, files override.** Keep a generated (not hand-maintained)
embedded copy as a last-resort fallback; the file-reading seam is tried first, and only
falls back to the embedded copy if the directory truly can't be resolved. This preserves
today's "always works" property unconditionally while still delivering the request's
actual goal (editable, diffable, testable source *as the normal path*; the fallback is
insurance, not the intended everyday route). Cost: needs *some* embedding mechanism (§6
confirmed none exists), and needs a discipline to keep the embedded copy from silently
drifting from the `.lev` files (most naturally solved by generating the embedded copy
*from* the `.lev` files at build time, rather than hand-maintaining two copies — which
also directly reuses the "turn a file into a C++ string constant" problem §6 flagged as
needing new CMake machinery either way).

**(c) Guaranteed-by-construction, no fallback needed.** The request's own alternate
phrasing: make the files unfindable-in-practice impossible rather than defending against
it — CMake copies/generates them next to the binary at build time *and* installs them,
and every test harness is proven to resolve them (which per §7, they already would, for
free, if the resolution mirrors the archive resolver). This is really option (a) plus a
rigor commitment about build/CI coverage, not a structurally different mechanism — worth
the design calling out as the same code path as (a) with a stronger "we tested every
harness" bar rather than a separate implementation.

The request asks for an explicit ruling between these, calling out (b)/(c) as the two it
weighed. Nothing in the codebase today favors one on technical-feasibility grounds alone —
(a)/(c) are cheaper to build (no embedding mechanism needed at all), (b) is the more
defensive choice at the cost of building the embedding mechanism the codebase doesn't
have yet regardless. Given zero `install()` precedent exists (§6), **(c)'s "and installs
them" half is not a small ask** — it requires originating the project's first-ever
`install()` directives, which has scope beyond just the prelude (what else would an
"install" mean for this project — the binary? the runtime archives? trident?) that the
design doc should either bound tightly to "just enough for the prelude" or flag as
implicitly opening a larger question.

---

## 10. Blast-radius and staging considerations

The request's "Known Warnings" section is accurate against the code as found:

- **Four worktrees** (`agent0`–`agent3`) plus `master`, per the `three-branch-rule`/
  `Master push guardrail` memories — a path-resolution bug ships to `master` and is felt
  everywhere simultaneously the moment any worktree pulls it, before even considering CI.
  This argues for whatever embedded-fallback decision (§9) errs toward keeping a safety
  net during the transition, even if it's later removed once the files-only path is
  proven — a phased rollout (ship files + fallback, prove it across N builds/harnesses,
  then drop the fallback in a follow-up) is a reasonable staging shape the design could
  adopt explicitly, independent of which of (a)/(b)/(c) is the final steady state.
- **`.lev`, never `.ext`** — mechanical, no code-level risk found; just confirms the eight
  new files must use the `.lev` extension (per the standing HARD house rule), and nothing
  in the current `Lexer`/`Parser` cares about file extension at all (they operate on
  `SourceFile.text`, extension-agnostic) so this is purely a naming discipline, not a
  parser constraint.
- **Prelude-not-checked** — reconfirmed in §8; the refactor doesn't touch the Checker
  call site at all, so this invariant is preserved by construction as long as the design
  doesn't accidentally start passing `preludeProgram_` (or the newly-loaded text) through
  `checker.run(...)`.
- **X64/ELF backend** — confirmed structurally in §5: the triple-based per-target
  decision happens inside `Resolver::parsePrelude()`, upstream of every backend. The ELF
  backend (`X64Gen`) is never invoked with a non-empty `--target` (it's the frozen,
  host-only x86-64 backend; cross-target emission is exclusively the `LlvmGen` path per
  `main.cpp`'s `--target`/`isWasmTriple` branch structure at lines 628–633). So
  `isWasmTriple(targetTriple)` naturally evaluates false for every ELF-backend
  invocation with zero ELF-specific code needed — the request's "should need zero
  LlvmGen/X64Gen edits" claim holds for X64Gen trivially (it never sees a triple) and
  should also hold for `LlvmGen.cpp` itself, since `LlvmGen` consumes whatever
  `preludeProgram_`/lowered IR the `Resolver` already produced — it doesn't re-decide
  per-target inclusion itself.

---

## 11. Cross-references: Track W, `info.md`, `docs/reference.md`

- **`info.md` §19 #18** (`info.md:2208-2214`) is the ruling record; §20
  (`info.md:2218-2250`) is the wasm-target framing that names this refactor as the one
  open upstream dependency for Track W's W-M4 (`info.md:2247-2250`, "Per-target stdlib
  packaging now rides the §19 #18 ruling... this track is the consumer of that upstream
  refactor, which is not yet built").
- **`designs/wasm-frontend/techdesign-06-bindgen-and-ship.md` §2** and
  **`designs/wasm-frontend/techdesign-00-overview.md` §5** both independently describe the
  same STOP-condition history (escalate at W-M1 start, do not improvise) and both confirm,
  as of their 2026-07-19 updates, that `parsePrelude()` "still concatenates the C++
  raw-string segments... not yet built" — consistent with what this research found reading
  the code directly (no drift between doc and tree on this specific point).
- **`docs/reference.md` §7.3** (`docs/reference.md:2795-2834`) already documents the
  `wasm32` target's capability matrix and explicitly cites `kPreludeWasm`
  (`docs/reference.md:2832`, "`runtime/../Resolver.cpp` `kPreludeWasm`") as the DOM
  surface's current location — this line will need updating once the segment becomes a
  shipped file (acceptance criterion 6 already calls for a `docs/reference.md` update;
  this is the specific line).
- Per `docs/policies.md`'s Design Implementation section, once this design is implemented:
  `docs/reference.md` update is required (the line above, plus whatever new "per-target
  selection"/`--target`-and-prelude behavior is worth documenting in the target/backend
  matrix section it already lives in); and Track W's `techdesign-06-bindgen-and-ship.md`
  should be revisited by its own owner once this lands, since its §2 explicitly says it
  "consumes" this refactor to close W-M4 — that's Track W's move, not this design's, but
  worth the design doc flagging as "this unblocks X" for sequencing awareness.

---

## 12. File-level change map

Everything identified as touched by this refactor, for design-doc scoping:

| File | Change |
|---|---|
| `src/Resolver.cpp` | `parsePrelude()` rewritten to a file-reading seam; the eight `kPreludeXxx` `const char*` definitions either deleted or retained as a generated embedded-fallback source (§9 decision); header comment (`:5-21`) updated to describe the new model |
| `src/Resolver.hpp` | Constructor or setter to receive the target triple (mirrors `setFileRanges`'s existing pattern, `Resolver.hpp:27-29`); possibly a new private member for the resolved prelude directory / target |
| `src/main.cpp` | Thread `targetTriple` (already-local, `main.cpp:226`) into the new `Resolver` parameter at the construction site (`main.cpp:446`); add the prelude-directory resolver function(s), following `findRuntimeArchive`/`findRuntimeArchiveForTriple`'s shape (`main.cpp:141-167`) |
| `prelude/*.lev` (new) | Eight new files, byte-identical content to today's raw strings, named per the request's working layout (`core.lev`, `std.lev`, `rest.lev`, `regex_core.lev`, `regex_api.lev`, `web.lev`, `wasm.lev`, `expr.lev`) |
| `CMakeLists.txt` | New machinery per §6/§9's decision: some combination of `file(COPY)`/custom target/embedded-fallback-generation; no existing pattern to lift verbatim |
| `docs/reference.md` | §7.3's `kPreludeWasm` citation (`:2832`) updated; target/backend matrix gains whatever "per-target prelude selection" note is warranted |
| `info.md` | §19 #18 marked fully resolved-and-shipped (currently records the *ruling*, not the *landing*); §20 updated per Track W's own doc-06 §6 sweep duty (though that sweep is Track W's, triggered by this landing) |
| **Untouched (confirmed)** | `src/X64Gen.cpp`, ELF test infra, `src/Checker.cpp`'s call site, every `tests/run_*.sh` script's own logic (they inherit correctness for free per §7), `src/LlvmGen.cpp`'s capability-gate machinery (orthogonal per §5) |

---

## 13. Open questions for the design doc to rule on

Mirroring and sharpening the request's own list, now with the concrete option menus this
research surfaced:

1. **Embedded fallback vs. files-only vs. guaranteed-by-construction** — §9's (a)/(b)/(c).
   No technical blocker favors one; it's a risk-tolerance call weighed against the new
   CMake-embedding-mechanism cost every option except pure (a) incurs anyway if any
   fallback content needs generating. A staged rollout (start with a fallback, drop it
   later once proven, §10) is available regardless of the steady-state choice.
2. **Resolution tiers** — the actual precedent is 3 tiers (CLI flag → next-to-binary →
   one source-relative fallback for per-triple lookups only), not 4, and has no env var
   today (§3). Decide explicitly whether to introduce `LV_PRELUDE_DIR` as new pattern
   (reasonable on its own merits, e.g. useful for out-of-tree testing) and whether an
   install-prefix tier is worth adding now given no `install()` exists anywhere yet (§6) —
   or deferred until the project has an actual installed-tree story.
3. **Per-target selection mechanism, v1 scope** — wasm-inclusion is unambiguous (`Dom`,
   zero cross-segment dependents, §5). OS-only *exclusion* from wasm builds needs an
   explicit yes/no: the doc-02 capability gate already makes it a size-only question, not
   a correctness one (§5), so "no, not in v1" is a defensible scope cut the design can
   make without inventing new gating.
4. **Concatenated buffer vs. per-file parsing** — §8 found zero existing test surface
   pinned to prelude line/column numbers (Checker never sees the prelude; no `.expected`
   contains "prelude"), so the request's own suggested default (concatenate contents into
   one `preludeFile_`, matching today's `LineCol` behavior exactly) carries no discovered
   regression risk and is the cheaper option; per-file `SourceFile`s would need either a
   multi-file merge loop or extending the `fileRanges_`-style multi-offset-range machinery
   that already exists for the user-program side (`Resolver::setFileRanges`,
   `Resolver.cpp:7238` boundary comment) — available to reuse if the design wants it, not
   mandatory.
5. **Where `.lev` files ship from for dev/CWD-varying test harnesses** — §7 established
   every harness already reaches `leviathan` by absolute path and the binary already
   self-locates via `/proc/self/exe`, so whatever resolution order is chosen "just works"
   for every existing script with no per-script changes needed. Worth the design
   explicitly stating this as a design *property* (not an assumption to re-verify per
   script) so implementation doesn't over-engineer per-harness accommodations that aren't
   needed.
