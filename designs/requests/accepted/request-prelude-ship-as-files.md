# Summary: Ship the stdlib prelude as `.lev` files with a real `parsePrelude()` file-reading seam + per-target segment selection

The owner has ruled (info.md §19 #18, landed commit `476135f` 2026-07-19) that **the
stdlib ships as `.lev` files, not per-target in-binary raw-string segments**: "Moving
the prelude to shipped source files is the goal — `parsePrelude()` gains a real
file-reading seam; per-target selection (e.g. a wasm-only `kPreludeWasm`) is a packaging
detail *within* that model." This request is to build that model.

**Who needs it / what it unblocks.** The immediate consumer is **Track W, W-M4 §2**
(`designs/wasm-frontend/techdesign-06-bindgen-and-ship.md` §2): the wasm target wants the
browser-only prelude surface (`Dom`, and eventually `fetch`/`WebSocket`) to be a
per-target segment included *only* for wasm builds, instead of `kPreludeWasm` being
concatenated unconditionally into every target's prelude. That track is explicitly the
**consumer, not author** of this refactor (overview §5) — W-M4 cannot close until this
lands. Wider benefit: the prelude (~5,860 lines across 8 raw-string constants) becomes
editable, diffable, testable source instead of C++ string literals; per-target selection
becomes a first-class concept (wasm-only, OS-only, future targets); and the compiler gains
a clean seam for shipping/overriding stdlib source — the foundation for eventual
stdlib-as-dependency packaging via Trident.

## Request Details

Today the entire prelude is eight `const char* kPreludeXxx = R"prelude(...)"` constants
compiled into the `leviathan` binary and concatenated at one point:

```cpp
// src/Resolver.cpp:5881-5889
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

Segment map (all `src/Resolver.cpp`, verified 2026-07-19):

| segment | starts | role |
|---|---|---|
| `kPreludeCore` | :22 | primitive object masks + collections + Seq + streams glue |
| `kPreludeStd` | :1122 | `namespace std` (sys floor, aggregates, promise/timer) |
| `kPreludeRest` | :2756 | top-level Range/StreamBuffer/Console + meta/math/env/term |
| `kPreludeRegexCore` | :3434 | Track 10 regex engine internals |
| `kPreludeRegexApi` | :4122 | Track 10 regex public surface |
| `kPreludeWeb` | :4587 | Track 09 encoding/digest/DateTime/json |
| `kPreludeWasm` | :5591 | **Track W DOM bridge — the per-target candidate** |
| `kPreludeExpr` | :5710 | LA-31 expression reification |

The target architecture:

1. Each segment becomes a shipped `.lev` file (byte-identical content). Working layout:
   a `prelude/` directory of themed files (`core.lev`, `std.lev`, `rest.lev`,
   `regex_core.lev`, `regex_api.lev`, `web.lev`, `wasm.lev`, `expr.lev`) — order preserved.
2. `parsePrelude()` reads those files from a resolved directory instead of concatenating
   C++ constants. The directory is located the same way the driver already resolves the
   **runtime archive** (env override → next-to-binary → source-tree-relative → install
   prefix) — mirror that existing, proven resolution logic rather than invent a new one.
3. **Per-target selection** is the payoff: `wasm.lev` is included only when the compile
   target is `wasm32*`; the OS-only surface can conversely be excluded from wasm builds
   (a size/cleanliness win — the doc-02 capability gate already handles correctness, so
   nothing OS-only needs *excluding* to be correct). The Resolver must learn the target
   triple to make this selection (today it may not have it — see Warnings).

**Language-ideology fit.** This is pure plumbing, but it advances "the stdlib is ordinary
Leviathan code" — the prelude stops being an opaque C++ artifact and becomes source that
obeys the same rules as user code, editable and per-target-composable, which is the honest
expression of the capability-subset framing (info.md §20): one language, per-target
*packaging*, never a dialect.

## Requested Specific Feature

Preferences / concerns to weigh in design (each needs an owner ruling — no deferrals):

- **Embedded fallback vs. files-only.** Removing the C++ constants entirely means a
  missing/unfound prelude directory breaks *every* compilation. Strongly prefer a design
  that either (a) keeps the generated constants as an embedded fallback the file seam
  overrides, or (b) guarantees-by-construction that the files are always found (CMake
  copies them next to the binary at build time + installs them, and every test harness
  resolves them). Please rule explicitly; this is the load-bearing safety decision.
- **File resolution** should reuse the runtime-archive resolver's search order and env
  override style (an `LV_PRELUDE_DIR`-shaped knob), for consistency and testability.
- **Per-target selection mechanism.** Keyed on the target triple (`wasm32*` → include
  `wasm.lev`). Confirm whether OS-only exclusion is in-scope for v1 or a follow-up.
- **Single concatenated file vs. one-parse-per-file.** `parsePrelude()` currently lexes
  one `preludeFile_`; if files are parsed separately, span attribution (`<prelude>` file
  name) and error reporting must stay coherent. Prefer concatenating file *contents* into
  the one `preludeFile_` (minimal change) unless there's a reason to parse per-file.

## Known Warnings

- **Catastrophic blast radius.** A path-resolution bug means the compiler cannot find the
  prelude → *every* compile on *all four* worktrees (agent0–agent3 + master) fails. Stage
  carefully; keep a fallback until the file path is proven across every harness.
- **`.lev`, never `.ext`** (HARD house rule) — the shipped files are `.lev`.
- **The Checker never runs on the prelude** (prelude-not-checked): moving to files must not
  change that — it's still lexed+parsed+gathered but never checked. Don't "fix up" prelude
  bodies to satisfy the checker.
- **Every test harness must find the files**: `ctest`, and the `tests/run_*.sh` scripts
  (`run_wasm.sh`, `run_wasm_dom.sh`, `run_elf.sh`, `run_sysnatives.sh`, …) all invoke
  `build/leviathan` from varying CWDs. The `--runtime`-style override precedent shows the
  harnesses expect archives next-to-binary; the prelude dir should land there too.
- **Do NOT touch the frozen X64/ELF backend or ELF test infra** — this change is
  backend-agnostic (it's front-end prelude loading); it should need zero LlvmGen/X64Gen edits.
- **Resolver target-awareness**: confirm early whether `Resolver::parsePrelude()` has access
  to the `--target` triple; per-target selection depends on it. If not, threading it in is
  part of the work.
- **Complexity marking.** Prelude loading is core-compiler decision-making — **opus/fable
  complexity**; write the tech design before implementing (per `docs/policies.md`).

## Acceptance Criteria

1. The 8 prelude segments live as byte-identical `.lev` files in a shipped `prelude/` dir;
   the `kPreludeXxx` C++ constants are gone OR retained only as an explicit embedded
   fallback (per the ruling in "Requested Specific Feature").
2. `parsePrelude()` loads the prelude from files via a resolver mirroring the runtime-archive
   search (env override → next-to-binary → source-relative → install), with a loud, specific
   diagnostic if the prelude cannot be located (never a silent empty prelude).
3. **Per-target selection works**: a `wasm32*` build includes `wasm.lev`; a native build does
   not (verified — e.g. a native program referencing `Dom` fails to resolve, a wasm one
   succeeds). This closes Track W W-M4 §2.
4. Full `ctest` green on this box; all four engine lanes byte-identical on the existing
   corpus (the prelude content is unchanged, so zero behavioral diff is the bar).
5. CMake ships/installs the prelude files so `build/leviathan` and every `tests/run_*.sh`
   harness find them from their normal CWDs.
6. Zero edits to the frozen X64/ELF backend. `docs/reference.md` + `info.md` §20 updated to
   describe the file-shipped prelude + per-target selection; Track W doc-06 §2 marked
   consumed and the track closed to `designs/complete/` if this is its last open item.

## Interim Fallback

Track W doc-06 §3 (size pass, `tests/wasm_size.sh`) and §4 (the Atlantis-client demo,
`examples/wasm-client/`) are **already implemented and pushed** against the hand-written
`Dom` prelude stubs, which remain the as-built binding surface. Dev/wasm builds ride the
existing in-binary concat (the doc-02 capability gate handles browser-absent natives, so
this is a size/cleanliness win, not a correctness blocker). No lost work to park — this
request is purely the packaging model that lets W-M4 §2 close and the track move to
`designs/complete/`. Doc 06 was deliberately **not** moved to complete pending this.

## Other

The `@extern` rules-engine bindgen (doc 06 §1) is a **separate, still-blocked** item (it
targeted an abandoned per-method `__import` seam and needs metaprog scope beyond the bounded
P4 roadmap) — out of scope for this request; do not conflate the two.
