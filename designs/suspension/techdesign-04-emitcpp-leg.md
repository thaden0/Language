# LA-30 True Suspension — emit-C++ Leg: Explicit Deferral (doc 4 of 6)

**Owns:** `src/CGen.cpp` — **and changes nothing in it for v1**. This doc exists so the
deferral is a recorded decision with a specified future path, not an omission someone
"fixes" mid-track. **Status:** PROPOSED (decision doc). **Gotchas owned:** the two in §5.

---

## 1. Status quo (audited)

The emit-C++ backend has **no async surface at all** today:

- No `Op::Await` case — an `Await` op hits the clean "unsupported" default skip
  (`src/CGen.cpp:677` comment: "today's clean 'unsupported' skip"), the same lane
  discipline other unmodeled ops use (`:203`, `:522`, `:536`, `:649`).
- No `RuntimeLoop` reference, no timers, no watches, no sockets-driven dispatch.
- Exceptions are the interpreter-model pending-throw flag — but as **plain file
  statics** (`static bool g_throwing; static V g_thrown;`, `CGen.cpp:208-215`), not
  thread-local: this backend has no threads either (Track 10's file list —
  techdesign-threads-3 §"files" — names `LlvmGen.cpp`/`Eval.cpp`/`IrInterp.cpp` and
  never `CGen.cpp`).

Consequently no async/threads corpus lane runs against emit-C++ now, and LA-30 cannot
regress what does not exist.

## 2. Decision

**Defer.** LA-30 ships with no emit-C++ leg:

- `LANG_TASKS` is meaningless to emitted C++ programs (nothing reads it there).
- M5's flip does not touch this backend; its docs line in reference.md §7.3 reads
  "emit-C++: no async surface (unchanged by LA-30)".
- The corpus matrix keeps async/threads/tasks lanes excluded from emit-C++ exactly as
  they are excluded today (audit `tests/run_corpus.sh` lane lists at M5 to confirm no
  accidental inclusion — that's the acceptance check this doc owns).

Precedent: Track 10 (threads) made the same call for the same backend, and the project
has a standing pattern for honest per-engine coverage notes (reference.md's "engine
coverage" convention). This is that pattern applied forward.

## 3. Why not "just add it" (the cost honestly)

Bringing emit-C++ into async at all requires, in order: the loop (a C++ transcription or
a link against `runtime/lv_loop.c` — but emitted programs are currently self-contained
single translation units, a property worth keeping deliberate, not eroding as a side
effect); timers/watch natives; the promise/worker native floor; and only *then* tasks.
That is a Track-10-and-a-half–sized effort for the backend with the least distinctive
value per hour right now (LLVM is the production AOT; emit-C++ earns its keep as the
portability/reference lane for the sync core). If/when Atlantis or self-host needs
emitted-C++ servers, that's a separate green-light with its own track.

## 4. The future path (specified now so it's cheap later)

When the day comes, the design is a straight replay of docs 1-2 with two substitutions:

1. **Runtime linkage:** either (a) emit `#include`-style preamble embedding of
   `lv_task.c` + `lv_loop.c` equivalents (keeps single-file emission; duplicates
   maintenance), or (b) drop single-file and link `liblvrt.a` like `--build-native`
   does (one runtime, two backends — strongly preferred; the single-file property
   should be given up *deliberately* in that future doc, with the owner's sign-off).
2. **Context switch:** default to the `LV_CTX_UCONTEXT` fallback (doc 1 §5) on this
   backend — emitted C++ compiles on arbitrary toolchains where shipping `.S` files is
   friction; the ucontext syscall tax (G1) is acceptable for a portability lane. The
   asm path remains available where the toolchain matches.

Pre-requisites recorded for that future track: `g_throwing`/`g_thrown` become `LV_TLS`
(they are per-thread state the moment either threads or tasks exist); the emitted
`throwCheck` after calls (`CGen.cpp:582-593`) already has the right shape and needs no
redesign; semantics arrive **already-corrected** (C1/C2/C3 as flipped at M5) — the pump
model must never be implemented on this backend (there will be nothing to be
bug-compatible with by then).

## 5. Predictable gotchas

| trap | note |
|---|---|
| someone adds a "quick" `Op::Await` case to CGen during M1-M5 "since the runtime exists now" | forbidden — it drags the loop/native floor in behind it and lands a fifth semantics mid-flip; overview §11 lists this as a STOP condition |
| corpus lane drift | a future lane refactor accidentally pointing async corpus at emit-C++ will fail confusingly at the missing-op skip; the M5 lane audit (§2) is the guard, and the skip's diagnostic already names the op |

## 6. Acceptance

Nothing to build. M5's flip commit includes: (1) the reference.md §7.3 coverage line,
(2) the lane-audit result confirming async corpus exclusion, (3) this doc moved along
with the track per the designs-layout convention.

**STOP:** any change to `src/CGen.cpp` attributed to LA-30.
