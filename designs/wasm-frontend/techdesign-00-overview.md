# Track W — WebAssembly Front-End Target (Leviathan in the browser) — Overview (doc 0 of 6)

**Track:** W — wasm-browser backend column + floor retarget + JS/DOM bridge.
**Status:** PROPOSED — design complete, awaiting owner green-light on W-M1+ (W-M0 the spike
needs no approval beyond this doc). **Date:** 2026-07-16 (audited against master @ working
tree; substrate map is `docs/wasm-frontend-research.md`, audited same day).
**Source:** `designs/requests/proposal-wasm-frontend.md` (the verdict) +
`docs/wasm-frontend-research.md` (the grounded substrate — the dossier). This track is the
tech design both documents point at. The dossier is the *only* research prerequisite; every
file:line cited here is anchored there unless independently verified in this audit.
**Docs in this track:** this overview + `techdesign-01-spike.md`,
`-02-backend-column.md`, `-03-floor-wasm.md`, `-04-async-jspi.md`,
`-05-dom-bridge.md`, `-06-bindgen-and-ship.md`, plus the **HARD packets**
`hard-01-tls-pin.md` … `hard-05-callclosure-seam.md` (§0).
**Owns:** a new backend column (IR → `wasm32`), `runtime/lv_plat_wasm.c` (the browser
Layer-0 floor), a wasm task realization (JSPI), the JS glue + reflective marshaler + closure
trampoline, and a rules-engine DOM bindgen. **Touches the language: nothing. Touches the
IR: nothing.** That is the central claim, inherited from the proposal and grounded by the
dossier (§2 there): everything lowers to the one 44-op IR (`src/Ir.hpp:26-89`); a backend
only diverges on emit-per-op.

---

## 0. HARD labeling convention (binding on every doc in this track)

Work items marked **[HARD]** touch the sensitive LLVM backend surface:

- `src/LlvmGen.cpp` / `src/LlvmGen.hpp` — any edit, however small;
- the generated-code ↔ runtime ABI contract (`runtime/lv_abi.h` semantics as *emitted
  against* by LlvmGen — adding/changing what generated code calls);
- the LLVM link-driving lane in `src/main.cpp:600-673`.

Rules for **[HARD]** items: minimal diffs; no drive-by refactors; every HARD change is
followed *in the same packet* by the four-lane engine differential (§6) plus the wasm lane
once it exists; a HARD change that produces any cross-engine divergence is a STOP, not a
debug-in-place. Non-HARD work (the floor `.c` file, JS glue, prelude rules, corpus, docs)
may iterate freely.

**Every HARD item lives in its own minimal packet doc** (`hard-NN-*.md` in this
directory) — the stage docs hold context and a pointer, never the edit spec. One HARD
packet = one commit.

| HARD packet | item | what it edits | status |
|---|---|---|---|
| `hard-01-tls-pin.md` | TLS-model wasm branch | `LlvmGen.cpp:3343-3355` | scheduled (W-M1) |
| `hard-02-link-lane.md` | wasm link lane | `main.cpp:600-673` | scheduled (W-M1) |
| `hard-03-capability-gate.md` | capability gate + `lvrt_unsupported` | `LlvmGen.cpp:2457ff` (CallNativeFn), pattern of `:2542,2565`; one new `lvrt_*` symbol | scheduled (W-M1) |
| `hard-04-await-routing.md` | Await → `lvrt_await` routing | `LlvmGen.cpp` Await path | CLOSED — NOT NEEDED (2026-07-17 audit) |
| `hard-05-callclosure-seam.md` | `lvrt_callclosure` ABI addition | `lv_runtime.c` / `lv_abi.h` | CONTINGENT (pre-W-M3 audit) |

Everything else in the track is not HARD: `lv_plat_wasm.c`, `lv_task_wasm.c`, JS glue,
`build-triple.sh`, prelude/bindgen rules, corpus, demos, docs.

---

## 1. Thesis

The wasm-browser target is **three artifacts and zero language changes**:

1. **A backend column** — IR → `wasm32` via the existing LLVM backend, which already
   reaches it: `emitObject(path, triple, optLevel)` takes any triple and initializes all
   targets (`LlvmGen.hpp:32-33`, `LlvmGen.cpp:3309-3333`); the installed LLVM 18 dylib has
   the `WebAssembly` target built. Codegen is nearly free; the runtime archive and link lane
   are the actual work (doc 02).
2. **A floor retarget** — one new file `runtime/lv_plat_wasm.c` implementing the same ~40
   symbols `runtime/lv_plat.h` declares, syscalls swapped for host imports. The seam is real
   and enforced ("the ONLY place the runtime touches an OS", `lv_plat.h:1-4`) (doc 03).
3. **A JS/DOM bridge** — one reflective marshaler over the frozen `LvValue {tag, payload}`
   ABI (`lv_abi.h:36`), a handle table for opaque JS values, a closure trampoline for DOM
   events, and rules-engine-generated `@extern` stubs (docs 05, 06).

Plus one runtime realization that is neither a library nor a backend: **async suspension on
wasm is JSPI** — the engine-provided version of the exact stackful park/resume the landed
task substrate already does with hand-written `.S` context switches (`lv_ctx_x86_64.S:35`,
`lv_task.c:258-261`). One model, two realizations (doc 04).

### 1.1 What moved since the proposal (why this is schedulable now)

The proposal (2026-07-08) deferred the real work behind three conditions. Per the dossier
§12, **two are met**: Gate E is complete (Track 09 DONE 2026-07-10) and the §19 #5 async
decision resolved — stackful tasks are the landed default on all three engines, `await`
stays uncolored (`info.md:1642-1648`). Metaprog Phases 1–4 + F4 also landed, so the
ergonomic bindgen needs no waiting. **The one open upstream dependency is the per-target
stdlib packaging ruling** ("ship stdlib as files", deferred in
`designs/complete/techdesign-09-web-foundations.md` §0). This track's stance on it: §5.

---

## 2. Architecture (the one picture)

```
                     source .lev
                          │
        Lexer → Parser → Resolver (prelude gather, Resolver.cpp:5193) → RuleEngine → Checker
                          │
                  IrModule (src/Ir.hpp — 44 ops, THE semantic truth)
                          │
   ┌────────┬─────────┬───┴──────┬─────────────┬──────────────────────────┐
   ▼        ▼         ▼          ▼             ▼                          ▼
 Eval    IrInterp   CGen      X64Gen        LlvmGen (native)     LlvmGen --target wasm32   ◄ NEW COLUMN (doc 02)
(oracle) (oracle) (emit-C++) (ELF, FROZEN)                                │
                                                        ┌─────────────────┼───────────────────┐
                                                        ▼                 ▼                   ▼
                                                 liblvrt (wasm32)   lv_plat_wasm.c        JS glue (lv_host.js)
                                                 lv_runtime.c       floor → host imports  marshaler + handle
                                                 lv_task_wasm.c     (doc 03)              table + trampoline
                                                 (JSPI, doc 04)                           + DOM bindgen (05/06)
```

The wasm target is **another column in the engine-coverage matrix**
(`techdesign-00-overview.md:187-190` policy): it declares its covered capability subset,
implements the browser-relevant floor, and **cleanly diagnoses the rest** — the ELF-DNS
pattern (`X64Gen.cpp:3859-3860`; Windows precedent inside LlvmGen itself at
`LlvmGen.cpp:2542,2565`). Never a language subset, never a dialect (STOP condition §8).

## 3. Capability matrix (per-target; native Leviathan loses nothing)

| | wasm-browser target |
|---|---|
| **Lost** (compile-time gated, doc 02 §5) | filesystem (`File`, dirs), process spawn, raw TCP/UDP + DNS, argv/env, tty, signals, blocking sync reads, raw OS threads / shared-address `fork` |
| **Kept** (floor retargeted, docs 03/04) | entire language + every pure prelude library (JSON, DateTime, encoding, digests, regex, collections, strings, math); console; time; randomness; event loop; async/await; high-tier threads (deferred leg, doc 04 §7) |
| **Reshaped** | HTTP → `fetch`, sockets → `WebSocket`, both as §13 stream endpoints |
| **Gained** (doc 05/06) | DOM, `fetch`, `WebSocket`, WebCrypto, Canvas/WebGL, storage |

## 4. Milestones & gates

| milestone | contents | docs | gate to pass |
|---|---|---|---|
| **W-M0** | de-risking spike: LLVM→wasm32, pure compute, differential vs oracle | 01 | runs + matches on the pure-compute corpus subset. **No approval needed; throwaway branch.** |
| **W-M1** | real backend column: TLS branch, link lane, `liblvrt-wasm32.a`, minimal floor (write/time/random/exit), capability gate | 02, 03 | pure-compute + console corpus green in `wasmtime` **and** a browser loader; gated natives diagnose cleanly |
| **W-M2** | async: JSPI task realization, timers, promise settle, `fetch` as a stream endpoint; Asyncify fallback lane proven once then shelved | 04 | await/timer/fetch corpus green in Chrome (JSPI); differential vs IR interp on the async corpus |
| **W-M3** | DOM bridge: marshaler, handle table, closure trampoline, events-as-streams, `@extern` bindgen | 05, 06 | "hello DOM" demo — a Leviathan program builds a DOM tree, handles a click, round-trips strings |
| **W-M4** | ship: per-target stdlib consumption, Atlantis-client demo, docs sweep | 06 | Atlantis client demo; `info.md` §20 + `docs/reference.md` matrix row landed; track moved to `designs/complete/` |

**Execute strictly in order. One milestone = one review point.** W-M0 may start immediately.
W-M1 starts on owner green-light of this track. W-M4 is additionally gated on the stdlib
packaging ruling (§5).

## 5. The stdlib-packaging stance (the one open upstream dependency)

Verified in this audit: `Resolver::parsePrelude()` (`src/Resolver.cpp:5193-5201`)
concatenates **all** segments (`kPreludeCore + Std + Rest + RegexCore + RegexApi + Web`)
unconditionally — segmentation landed (Track 09 §0), per-target selection and file-shipping
did not.

This track's position, honoring the proposal's STOP condition ("consumer, not author, of
that refactor"):

- **W-M1–W-M3 do not need the ruling.** The whole prelude lowers into every IrModule
  already; browser-absent capabilities are handled by the doc-02 capability gate
  (compile-time diagnostic when *user code* reaches a gated native; trap-stub for
  unreachable prelude bodies). Dev builds ride the existing in-binary concat.
- **W-M4 (shipping) consumes the ruling.** Escalate the "ship stdlib as files / per-target
  segment selection" ruling to the owner **at W-M1 start**, so it has W-M1–W-M3's lead time
  to resolve. This track must not improvise a one-off seam in `parsePrelude()`; if the
  ruling is still open at W-M3 complete, W-M4 blocks and we escalate again — we do not ship
  a hacked seam.

## 6. House rules (binding on every doc)

- **Engine-differential at every step.** After each packet: build, run oracle
  (`--run`), IR (`--ir`), emit-C++, LLVM-native lanes on the touched corpus plus the full
  pre-existing suite — byte-identical or STOP. Once the wasm lane exists (W-M1), it joins
  the differential for its declared subset: same program, same bytes on stdout (via the
  floor's write), modulo the documented capability subset.
- **[HARD] discipline** per §0. The frozen X64/ELF backend gets **zero** edits, ever.
- **Corpus pins land in the same commit as the code they pin.** New corpus lives under
  `tests/corpus/wasm/` for wasm-lane-only pins; portable pins go in the normal clusters and
  simply gain the wasm lane.
- **Emscripten/Asyncify containment:** bring-up lanes only, clearly labeled, never
  load-bearing, never in CI as a required lane (dossier §13 #4).
- **The marshaler footgun rule** (doc 05): never dereference `payload-16` without the
  `isvalueclass()` / region-registry gate — literal strings and inline dense records carry
  no header (`lv_abi.h:61-63, 85-93`).
- **No new floor natives outside `lv_plat.h`'s enumerated set**; portable library code goes
  through prelude wrappers only (the standing discipline, proposal §8 — already house style).

## 7. Effort & sequencing summary

- W-M0: 1–2 days (dossier §5: "nearly mechanical" — `liblvrt.a` is real, no stub).
- W-M1: small–medium. The HARD edits are ~3 small, surgical branches; the bulk is
  `lv_plat_wasm.c` (~350–500 lines by POSIX/Win32 analogy) + `build-triple.sh` + a browser
  loader page.
- W-M2: medium — the pacing item, but it *rides* the landed task substrate rather than
  inventing suspension (the proposal's medium-large estimate shrank; dossier §14).
- W-M3: medium — marshaler is one table-driven routine; bindgen dependencies all landed.
- W-M4: small + the external ruling.

## 8. STOP conditions (inherited, still binding)

1. The spike reveals `wasm32` emission needs runtime-ABI work that belongs to the
   portable-backend owner — **coordinate, never fork the runtime.**
2. Shipping (W-M4) while the stdlib packaging ruling is open — **escalate, do not improvise
   a seam** (§5).
3. Any drift toward a *language* subset or browser dialect — the subset is
   platform-capability only, expressed through the coverage matrix.
4. Emscripten or Asyncify becoming load-bearing — bring-up only.
5. Any cross-engine divergence introduced by a **[HARD]** edit — stop and escalate; do not
   pin around it.

## 9. Reference-doc duty (W-M4, doc 06 §6)

`info.md`: new §20 (wasm target — capability-subset framing, floor-as-imports, JSPI as the
browser realization of §14's stackful tasks); §19: note the packaging ruling as the (then
hopefully closed) wasm gate. `docs/reference.md`: backend/target matrix row for `wasm32`;
the `@extern` binding surface. Cross-link `designs/atlantis/techdesign-09-views.md` as the
consumer.
