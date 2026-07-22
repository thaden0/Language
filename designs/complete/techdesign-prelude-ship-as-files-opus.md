# Tech Design: Ship the Stdlib Prelude as `.lev` Files (`parsePrelude()` file seam + per-target selection)

**Status:** IMPLEMENTED — landed on `agent0` in `6bf297a` (M1) + `38b59ce` (M2–M5); in `designs/complete/`.
**Complexity:** opus (core-compiler decision-making: driver resolution tiers, Resolver
seam, build-system plumbing; zero backend work).
**Date:** 2026-07-19.
**Sources:** `designs/requests/accepted/request-prelude-ship-as-files.md` (owner ruling
`info.md` §19 #18, commit `476135f`); `docs/research-prelude-ship-as-files.md`
(grounding — every mechanism below was verified against the tree, branch `agent0`,
2026-07-19).
**Consumer:** Track W W-M4 §2 (`designs/wasm-frontend/techdesign-06-bindgen-and-ship.md`)
— consumer, not author, of this refactor.
**Session plan:** ONE session implements this entire document (M1–M5 in order). No
parallel tracks; every touched file is owned by this design alone.
**Timeline:** land same-day as pickup. M1–M3 are the code (bulk of the session); M4
verification and M5 docs close it out. Nothing here blocks on any other in-flight track.

---

## 1. Rulings

Every decision the request flagged is ruled here. There are no deferrals; §10 is the
fixed ending state.

- **R1 — Embedded fallback, files win (request option b).** The eight segments live as
  `prelude/*.lev` files (the single source of truth). A byte-array copy is **generated
  from those files at build time** and compiled into `langfront`; `parsePrelude()` uses
  the files when a prelude directory resolves, and the embedded copy when none does.
  The fallback is **permanent**, not a transition aid: it makes drift impossible by
  construction (it is generated from the files, never hand-maintained) and it keeps
  every present and future `Resolver` construction site working with zero configuration
  — which is exactly the blast-radius insurance the request demands ("a path-resolution
  bug means every compile on all four worktrees fails").
- **R2 — Resolution tiers**, in order: (1) `--prelude <dir>` CLI flag, mirroring
  `--runtime`; (2) `LV_PRELUDE_DIR` environment variable; (3) next-to-binary
  `exeDir() + "/prelude"`; (4) in-source dev layout `exeDir() + "/../prelude"`;
  (5) embedded fallback, silently. Tier 2 is **deliberately beyond precedent** — the
  runtime-archive resolver (`src/main.cpp:141-167`) has no env tier (research §3
  corrected the request on this); we add it anyway because it lets a harness or
  out-of-tree build redirect the prelude without editing every invocation, and it costs
  one `getenv`. **No install-prefix tier — rejected**, not deferred: this project has
  zero `install()` directives and no installed-tree concept; tier 3 (next-to-binary via
  `/proc/self/exe`) *is* the relocation story, identically to `liblvrt.a`. If the
  project ever grows an install story, that effort adds its tier then.
- **R3 — Per-target selection, v1 and steady state:** `wasm.lev` is included iff the
  target triple starts with `wasm32` (same predicate as `main.cpp:85`'s
  `isWasmTriple`). **OS-only exclusion from wasm builds — rejected**, not deferred: the
  OS-only surface is interleaved through `std.lev`/`rest.lev` (not a separable
  segment), the byte-identical-extraction mandate forbids re-partitioning, the doc-02
  capability gate already guarantees correctness independent of inclusion, and the
  Track W size pass (`tests/wasm_size.sh`) is green with the full prelude — there is no
  live problem this would solve. Anyone who later needs a leaner wasm prelude files a
  new request to re-partition the segments; that is a different design.
- **R4 — One concatenated buffer, not per-file parsing.** File contents are read as raw
  bytes and concatenated, in today's exact segment order, into the single
  `preludeFile_` named `<prelude>` — byte-identical to today's concatenation for wasm
  targets, identical-minus-`wasm.lev` for native. Research §8 established zero test
  surface is pinned to prelude line numbers (the Checker never sees the prelude; no
  `.expected` contains "prelude"), so per-file `SourceFile`s buy nothing and would
  disturb the single-`SourceFile`-per-parse assumption in `Lexer`/`Parser`. Rejected.
- **R5 — The canonical segment list lives in exactly one place:** the CMake list that
  drives the embedded-fallback generator. The generated `PreludeEmbedded.cpp` exposes
  an ordered table `{name, wasmOnly, data, size}`; `Resolver::parsePrelude()` iterates
  that table for **both** file loading (names) and fallback (bytes), and
  `main.cpp`'s directory validator iterates it for completeness checks. No manifest
  file ships in `prelude/` — a manifest is a second copy of the list with its own parse
  step and failure mode; the compiler is the authority on what its prelude contains.
- **R6 — No build-tree copy of the `.lev` files.** Dev builds resolve the source tree
  via tier 4 (`build/leviathan` → `../prelude`), mirroring the existing
  `../runtime/<triple>/` shape. This eliminates the copy-staleness bug class entirely:
  editing a `.lev` file takes effect on the very next compile with no build step
  (files win over the embedded copy), and the embedded fallback regenerates on the next
  build via CMake dependency tracking.
- **R7 — Directory validation is all-eight, target-independent, and never falls
  through.** A shipped `prelude/` dir always contains all eight files (`wasm.lev`
  included — selection happens at parse time, not ship time). A tier-1/2 explicit
  override that is missing or incomplete is a **fatal error** (an explicit override
  must never be silently ignored). A tier-3/4 auto-discovered directory that *exists
  but is incomplete* is also **fatal** (a half-prelude signals corruption; falling
  through to another tier would mask it). Only a tier-3/4 directory that does not exist
  at all falls through to the next tier. The embedded fallback engages only when no
  directory exists anywhere — silently, because it is correct by construction.
- **R8 — Failure inside `parsePrelude()` is fatal-loud.** If a resolved directory's
  file cannot be read at parse time (deleted between validation and read, permissions),
  print `leviathan: cannot read prelude file '<path>'` to stderr and `exit(1)`. Never a
  silent empty prelude, never a per-segment mix of file and embedded content.

---

## 2. Current state (verified against tree)

`src/Resolver.cpp` holds eight `const char* kPreludeXxx = R"prelude(...)"prelude";`
constants, concatenated once in `Resolver::parsePrelude()` (`Resolver.cpp:5913-5921`),
called once from `Resolver::run` (`:7244`). Exact spans in the current tree:

| Segment | opens (line) | closes (line) | ships as |
|---|---|---|---|
| `kPreludeCore` | 22 | 1119 | `prelude/core.lev` |
| `kPreludeStd` | 1122 | 2785 | `prelude/std.lev` |
| `kPreludeRest` | 2788 | 3461 | `prelude/rest.lev` |
| `kPreludeRegexCore` | 3466 | 4142 | `prelude/regex_core.lev` |
| `kPreludeRegexApi` | 4154 | 4612 | `prelude/regex_api.lev` |
| `kPreludeWeb` | 4619 | 5610 | `prelude/web.lev` |
| `kPreludeWasm` | 5623 | 5733 | `prelude/wasm.lev` (wasm-only) |
| `kPreludeExpr` | 5742 | 5798 | `prelude/expr.lev` |

(Correction to the research doc: its table lists `kPreludeExpr` at "~1,620 lines"; the
segment is 5742–5798, ~56 lines. No design consequence — files are byte-identical
whatever their size — recorded here so nobody hunts for missing content.)

Other verified facts this design leans on:

- `Resolver.cpp` compiles inside `add_library(langfront ...)` (`CMakeLists.txt:26`);
  `leviathan` is `add_executable(leviathan src/main.cpp)` linking `langfront`.
- **Two** `Resolver` construction sites exist, both in `main.cpp`: the primary at
  `:446` and the post-rules re-resolve `resolver2` at `:493`. Nothing in `tools/`,
  `fuzz/`, or elsewhere constructs a `Resolver`.
- `targetTriple` is a `main()` local (`main.cpp:226`), parsed for **every** mode, in
  scope at both construction sites. `ComptimeOptions.targetTriple` (`main.cpp:466`) is
  a separate plumbing path for the RuleEngine — do not conflate; the Resolver gets its
  own copy.
- `exeDir()` (`main.cpp:129-138`) resolves via `/proc/self/exe` — CWD-agnostic; every
  test harness passes the binary as an absolute path (`$<TARGET_FILE:leviathan>` or
  `$1`), so all 34 `tests/run_*.sh` scripts inherit correct resolution with zero
  per-script changes (research §7).
- The only references to `kPrelude*` outside `Resolver.cpp` are two comments
  (`LlvmGen.cpp:1762`, `X64Gen.cpp:2155`) — no code dependency anywhere.
- The Checker runs only on the user program (`main.cpp:542-543`), never the prelude —
  this refactor does not touch that call site, preserving prelude-not-checked by
  construction.

---

## 3. New files and generated artifacts

### 3.1 `prelude/` (new, repo root) — eight `.lev` files

Byte-exact extraction: for each constant, the bytes strictly between `R"prelude(` and
`)prelude"`. Each segment currently begins with the newline immediately following the
opening `(` and ends with the newline before the closing `)` — **preserve both**; the
files each start with a blank line and that is correct (byte-identity is the mandate,
`cat` of the files in table order must equal today's concatenation exactly). All eight
files use the `.lev` extension — never `.ext` (HARD house rule). The `Lexer`/`Parser`
are extension-agnostic (they see only `SourceFile.text`), so this is pure naming
discipline.

Each file gets **no added header comment** — the raw-string banner comments
(`// --- kPreludeWasm: ...`) live *outside* the string literals today and therefore do
not move into the files. Their content is preserved by rehoming it into the segment
table comment in `CMakeLists.txt` and the rewritten `Resolver.cpp` header comment
(§4.3); adding banners inside the `.lev` files would break byte-identity.

### 3.2 `src/PreludeEmbedded.hpp` (new, hand-written, ~20 lines)

```cpp
#pragma once
#include <cstddef>

// Generated table of prelude segments (build/generated/PreludeEmbedded.cpp,
// produced by cmake/GenPreludeEmbedded.cmake from prelude/*.lev). Order is
// the canonical concatenation order. `data` is the embedded fallback copy of
// `prelude/<name>.lev`, byte-identical by construction (generated from it).
struct PreludeSegment {
    const char* name;          // segment stem: file is prelude/<name>.lev
    bool wasmOnly;             // include only for wasm32* target triples
    const unsigned char* data; // embedded fallback bytes (not NUL-terminated)
    unsigned long size;
};
extern const PreludeSegment kPreludeSegments[];
extern const unsigned long kPreludeSegmentCount;
```

### 3.3 `cmake/GenPreludeEmbedded.cmake` (new, ~40 lines)

A `-P` script invoked at build time:

```
cmake -DOUT=<build>/generated/PreludeEmbedded.cpp \
      -DPRELUDE_DIR=<src>/prelude \
      -DSEGMENTS="core;std;rest;regex_core;regex_api;web;wasm;expr" \
      -DWASM_ONLY="wasm" \
      -P cmake/GenPreludeEmbedded.cmake
```

For each segment: `file(READ "${PRELUDE_DIR}/${seg}.lev" hex HEX)` then
`string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")`, emitting

```cpp
static const unsigned char kSeg_core[] = { 0x0a, ... };
```

and finally the table + count matching `PreludeSegment`. Hex emission is
delimiter-proof (no raw-string delimiter collision to engineer around) and pure CMake
(no `xxd`/host-tool dependency). ~180 KB of prelude → a one-or-two-second regex pass
that runs only when a `.lev` file or the script changes.

### 3.4 `build/generated/PreludeEmbedded.cpp` (generated, never committed)

Wired in `CMakeLists.txt` (§6). Compiled into `langfront`.

---

## 4. Compiler changes

### 4.1 `src/main.cpp` — flag, env var, directory resolution

**New flag** `--prelude <dir>` parsed exactly like `--runtime` (`main.cpp:302-303`
pattern): `const char* preludeDirOverride = nullptr;` populated in the arg loop. If the
driver has a usage/help text listing `--runtime`, add `--prelude` beside it.

**New resolver functions**, placed next to `findRuntimeArchive` (`main.cpp:141`):

```cpp
#include "PreludeEmbedded.hpp"

// R7: a prelude dir must contain ALL segments, target-independent — selection
// happens at parse time, not ship time. Returns the first missing name or "".
static std::string preludeDirMissing(const std::string& dir) {
    struct stat st;
    for (unsigned long i = 0; i < kPreludeSegmentCount; ++i) {
        std::string f = dir + "/" + kPreludeSegments[i].name + ".lev";
        if (::stat(f.c_str(), &st) != 0) return f;
    }
    return "";
}

// Tiers (design R2): --prelude flag -> LV_PRELUDE_DIR -> exeDir()/prelude ->
// exeDir()/../prelude (in-source dev build, mirrors ../runtime/<triple>) ->
// "" meaning the generated embedded fallback. Explicit overrides and existing-
// but-incomplete dirs are fatal (R7): never silently ignore an override, never
// mask a corrupt dir by falling through.
static std::string findPreludeDir(const char* cliOverride) {
    auto fatalIfBad = [](const std::string& dir, const char* how) {
        struct stat st;
        if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            std::fprintf(stderr, "leviathan: prelude directory '%s' (%s) "
                         "does not exist\n", dir.c_str(), how);
            std::exit(1);
        }
        std::string missing = preludeDirMissing(dir);
        if (!missing.empty()) {
            std::fprintf(stderr, "leviathan: prelude directory '%s' (%s) is "
                         "missing '%s'\n", dir.c_str(), how, missing.c_str());
            std::exit(1);
        }
    };
    if (cliOverride) { fatalIfBad(cliOverride, "--prelude"); return cliOverride; }
    if (const char* env = std::getenv("LV_PRELUDE_DIR")) {
        fatalIfBad(env, "LV_PRELUDE_DIR"); return env;
    }
    std::string dir = exeDir();
    if (!dir.empty()) {
        for (const std::string& cand : { dir + "/prelude", dir + "/../prelude" }) {
            struct stat st;
            if (::stat(cand.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                std::string missing = preludeDirMissing(cand);
                if (!missing.empty()) {
                    std::fprintf(stderr, "leviathan: prelude directory '%s' is "
                                 "missing '%s'\n", cand.c_str(), missing.c_str());
                    std::exit(1);
                }
                return cand;
            }
        }
    }
    return "";  // embedded fallback (R1) — silent, correct by construction
}
```

**Both construction sites** get the same two setter calls. Resolve once, above `:446`:

```cpp
std::string preludeDir = findPreludeDir(preludeDirOverride);

Resolver resolver(file, sink);
resolver.setFileRanges(fileRanges);
resolver.setPreludeDir(preludeDir);        // "" => embedded
resolver.setTargetTriple(targetTriple);    // "" => host/native
resolver.run(program);
```

and identically on `resolver2` (`:493-495`). `preludeDir` and `targetTriple` are both
in scope at `:493` (same block); no restructuring needed.

### 4.2 `src/Resolver.hpp` — two setters, two members

Mirroring `setFileRanges` (`Resolver.hpp:27-29`):

```cpp
void setPreludeDir(std::string dir) { preludeDir_ = std::move(dir); }
void setTargetTriple(std::string triple) { targetTriple_ = std::move(triple); }
...
private:
    std::string preludeDir_;    // "" => use the embedded prelude fallback
    std::string targetTriple_;  // "" => host/native target
```

Defaults ("" / "") mean any `Resolver` constructed without these calls — including any
future tool or fuzz harness — gets the embedded native prelude and keeps working.
That is R1's zero-config guarantee.

### 4.3 `src/Resolver.cpp` — `parsePrelude()` rewrite, constants deleted

Delete all eight `kPreludeXxx` definitions (lines 22–5798's raw strings). Add
`#include "PreludeEmbedded.hpp"` plus `<fstream>`/`<sstream>` if not present. Rewrite:

```cpp
Program Resolver::parsePrelude() {
    // Per-target selection (design R3): wasm-only segments ride wasm32*
    // triples only. Predicate mirrors main.cpp's isWasmTriple (main.cpp:85,
    // file-local static there — duplicated one-liner by design ruling R3).
    bool wasm = targetTriple_.rfind("wasm32", 0) == 0;
    preludeFile_.name = "<prelude>";
    std::string text;
    for (unsigned long i = 0; i < kPreludeSegmentCount; ++i) {
        const PreludeSegment& seg = kPreludeSegments[i];
        if (seg.wasmOnly && !wasm) continue;
        if (!preludeDir_.empty()) {
            std::string path = preludeDir_ + "/" + seg.name + ".lev";
            std::ifstream in(path, std::ios::binary);   // raw bytes, no
            if (!in) {                                   // newline mangling
                std::fprintf(stderr, "leviathan: cannot read prelude file "
                             "'%s'\n", path.c_str());
                std::exit(1);   // R8: fatal-loud, never silent/mixed
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            text += ss.str();
        } else {
            text.append(reinterpret_cast<const char*>(seg.data), seg.size);
        }
    }
    preludeFile_.text = std::move(text);
    DiagnosticSink dummy;  // the prelude is trusted; ignore its diagnostics
    Lexer lexer(preludeFile_, dummy);
    Parser parser(lexer.tokenize(), preludeFile_, dummy);
    return parser.parseProgram();
}
```

Update the file's header comment (`Resolver.cpp:5-21`): the segments are now shipped
`prelude/*.lev` files with a generated embedded fallback; whole-program resolution
still makes segment order irrelevant; rehome the per-segment banner notes (especially
`kPreludeWasm`'s doc-05 provenance note) into this comment block so the provenance
survives the constants' deletion.

### 4.4 One comment edit in `src/LlvmGen.cpp` (comment only, no code)

`LlvmGen.cpp:~734-741`'s `@ginit` rationale says "the whole prelude lowers into every
module". Post-refactor the premise is "the target-selected prelude lowers into every
module"; edit that clause only. The conclusion (@ginit is not a compile-time-fail
root) is unchanged and correct. No other `LlvmGen.cpp` edit; the doc-02 capability
gate (`targetWasm` / `wasmGatedNative`) is untouched — it still guards OS-only natives
on wasm builds, which R3 deliberately keeps in the prelude.

### 4.5 Untouched — by construction

`src/X64Gen.cpp` and all ELF test infra (frozen — X64Gen never sees a triple, so the
predicate is false for every ELF invocation with zero ELF-aware code); the Checker
call site (`main.cpp:542`); every `tests/run_*.sh` script's own logic (absolute binary
path + `/proc/self/exe` resolution make them correct for free); the capability-gate
machinery; `RuleEngine`/`ComptimeOptions` plumbing.

---

## 5. CMake wiring (`CMakeLists.txt`)

Near the `langfront` definition:

```cmake
# --- shipped prelude (techdesign-prelude-ship-as-files-opus.md §3/§5) ---
# Canonical segment list, in concatenation order. This list is the single
# source of truth (design R5): the generator bakes it into the embedded-
# fallback table that Resolver::parsePrelude() and main.cpp's directory
# validator both iterate. wasm = Track W W-M3 DOM bridge, wasm32*-only (R3).
set(LV_PRELUDE_SEGMENTS core std rest regex_core regex_api web wasm expr)
set(LV_PRELUDE_WASM_ONLY wasm)
set(LV_PRELUDE_DIR ${CMAKE_SOURCE_DIR}/prelude)
set(LV_PRELUDE_EMBEDDED ${CMAKE_BINARY_DIR}/generated/PreludeEmbedded.cpp)
list(TRANSFORM LV_PRELUDE_SEGMENTS APPEND ".lev"
     OUTPUT_VARIABLE LV_PRELUDE_FILES)
list(TRANSFORM LV_PRELUDE_FILES PREPEND "${LV_PRELUDE_DIR}/"
     OUTPUT_VARIABLE LV_PRELUDE_FILES)
add_custom_command(
  OUTPUT ${LV_PRELUDE_EMBEDDED}
  COMMAND ${CMAKE_COMMAND}
          -DOUT=${LV_PRELUDE_EMBEDDED}
          -DPRELUDE_DIR=${LV_PRELUDE_DIR}
          "-DSEGMENTS=${LV_PRELUDE_SEGMENTS}"
          "-DWASM_ONLY=${LV_PRELUDE_WASM_ONLY}"
          -P ${CMAKE_SOURCE_DIR}/cmake/GenPreludeEmbedded.cmake
  DEPENDS ${LV_PRELUDE_FILES} ${CMAKE_SOURCE_DIR}/cmake/GenPreludeEmbedded.cmake
  COMMENT "Generating embedded prelude fallback")
# --- end shipped prelude ---
```

and `${LV_PRELUDE_EMBEDDED}` appended to `add_library(langfront ...)`'s source list.
Editing any `.lev` file regenerates the fallback on the next build (dependency-tracked
custom command — not `file(COPY)`, which has no tracking; research §6). There is **no**
copy of `prelude/` into the build tree (R6) and **no** `install()` directive (R2).

---

## 6. Tests

### 6.1 New script: `tests/run_prelude_select.sh`

Usage: `run_prelude_select.sh <leviathan-binary> <prelude-src-dir>`. Four cases, none
needing the wasm linker toolchain:

1. **Native excludes `Dom`:** a temp program whose body references `Dom` (e.g. a
   function mentioning `Dom.byId`, never called) compiled with `--ir` and **no**
   `--target` must exit non-zero with a resolve error naming `Dom`.
2. **wasm triple includes `Dom`:** the same program with a top-level
   `console.writeln("ok")` and the `Dom`-referencing function left uncalled, run with
   `--ir --target wasm32-unknown-unknown`, must exit 0 and print `ok`
   (compile-for-triple/execute-on-host is a real, exercised combination — research §4).
3. **Explicit override is never ignored:** `--prelude /nonexistent/dir` must exit
   non-zero with `prelude directory` + `does not exist` on stderr.
4. **Incomplete dir is fatal, complete dir works:** copy `<prelude-src-dir>` to a temp
   dir; `LV_PRELUDE_DIR=<tmp>` must compile a hello program; delete `std.lev` from the
   copy; the same invocation must exit non-zero naming the missing `std.lev`.

Registered in `CMakeLists.txt` beside the other script tests:

```cmake
add_test(NAME prelude_select
         COMMAND ${CMAKE_SOURCE_DIR}/tests/run_prelude_select.sh
                 $<TARGET_FILE:leviathan> ${CMAKE_SOURCE_DIR}/prelude)
```

### 6.2 Existing suites (unchanged, must stay green)

Full `ctest` on this box; the wasm lanes (`run_wasm.sh`, `run_wasm_dom.sh`) prove the
wasm-inclusion path against committed goldens. Zero behavioral diff is the bar: the
prelude *content* is unchanged, so all four engine lanes must be byte-identical on the
existing corpus. Do not run or mention the ELF lane (frozen).

---

## 7. Milestones (one session, in order)

- **M1 — Extract + prove byte-identity.** Add a temporary 3-line dump of
  `preludeFile_.text` to a scratch file inside today's `parsePrelude()`; build, run
  once on any program, keep the dump. Extract the eight `.lev` files (bytes strictly
  between `R"prelude(` and `)prelude"`); `cat` them in table order and `cmp` against
  the dump — must be byte-equal. Remove the temp lines. Commit the `prelude/` dir.
- **M2 — Embedded generation.** `cmake/GenPreludeEmbedded.cmake`,
  `src/PreludeEmbedded.hpp`, CMake wiring (§5). Build; confirm
  `build/generated/PreludeEmbedded.cpp` exists and `langfront` compiles.
- **M3 — The seam.** Rewrite `parsePrelude()` (§4.3), delete the constants, add the
  Resolver setters (§4.2), `findPreludeDir` + `--prelude` + both construction sites
  (§4.1), the `LlvmGen.cpp` comment edit (§4.4). Build.
- **M4 — Verification.** §6.1 script + `add_test`; full `ctest`; wasm lanes; spot-run
  one program with `LV_PRELUDE_DIR` pointing at a moved copy and one from a `cd /`
  CWD to demonstrate CWD-independence. Also verify the fallback: temporarily rename
  `prelude/` and run `build/leviathan --run` on hello — must still compile (embedded),
  then restore.
- **M5 — Docs + closure** (§9), commit, pull master, push per `docs/policies.md`.

---

## 8. STOP-and-escalate protocol (implementer)

STOP work and escalate to the owner — do not improvise — if any of these occurs:

1. The M1 `cmp` is not byte-equal and the cause is not an obvious extraction slip.
2. Any four-lane output diff appears on the existing corpus after M3.
3. The change appears to require touching `src/X64Gen.cpp`, any ELF test file, the
   Checker call site, or capability-gate code in `LlvmGen.cpp` (beyond §4.4's one
   comment clause).
4. A test harness fails to resolve the prelude in a way the tier design (§1 R2/R7)
   does not already describe — do not add per-harness special cases.
5. Any temptation to "fix" prelude code to satisfy the Checker — the prelude is
   lexed, parsed, gathered, resolved, but **never checked**; that invariant is
   load-bearing (prelude-not-checked).

---

## 9. Documentation and closure duties (M5)

- `docs/reference.md` §7.3: the `kPreludeWasm` citation (`:2832`) now points to
  `prelude/wasm.lev`; add one sentence to the target matrix: the prelude ships as
  `prelude/*.lev`, resolved `--prelude` → `LV_PRELUDE_DIR` → next-to-binary →
  source-tree, embedded fallback otherwise; `wasm.lev` included only for `wasm32*`.
- `info.md` §19 #18: append "SHIPPED" with date + this design's filename. §20: update
  the packaging paragraph — per-target stdlib packaging is now built; `Dom` is absent
  from native preludes by selection, the capability gate remains for OS-only natives
  on wasm.
- `designs/wasm-frontend/techdesign-06-bindgen-and-ship.md` §2: mark **consumed /
  landed** (W-M4 §2 closes). The doc and track stay in `designs/wasm-frontend/` —
  §1's bindgen is still structurally blocked (its own scoping doc:
  `designs/complete/techdesign-bindgen-metaprog-scope.md`); the track cannot close on this
  landing and this design does not move it.
- Move this design to `designs/complete/`; the request stays in
  `designs/requests/accepted/`; the research doc is already in `docs/complete/`.

---

## 10. Fixed ending state

1. `prelude/` at repo root holds the eight byte-identical `.lev` files; the
   `kPreludeXxx` constants no longer exist in `src/Resolver.cpp`.
2. `parsePrelude()` loads segments from the resolved directory, or from the
   build-generated embedded table when no directory exists; every failure mode in
   between is fatal-loud per R7/R8; no silent empty or mixed prelude is possible.
3. Native builds do not parse `wasm.lev`; `wasm32*` builds do — proven by
   `tests/run_prelude_select.sh` cases 1–2 and the wasm golden lanes. Track W W-M4 §2
   is closed as consumed.
4. Full `ctest` green including `prelude_select`; four-lane corpus byte-identical;
   zero edits to the frozen X64/ELF backend or its tests.
5. `docs/reference.md`, `info.md` §19 #18 + §20, and Track W doc-06 §2 updated as §9
   specifies; this design lives in `designs/complete/`.
