# Deferral Resolution — Transcendental Math on the Native Backends

**Status:** ready. **Date:** 2026-07-06. **Depends on:** nothing open — Track 06
(math) is complete (`designs/complete/techdesign-06-stdlib-math.md` §8), the
LLVM backend is at parity for everything else in that track, and the link
plumbing this design needs (`-lm`) already exists on every LLVM link line.
**Source:** the deliberately-logged Track 06 deferral (techdesign-06 §2's
coverage table, "deferred with diagnostic"); portable-backend pivot
(`docs/techdesign-portable-backend.md` §0.4 freeze).
**Owns (regions):** `src/LlvmGen.cpp` — the `emitNativeRows` lambda
(LlvmGen.cpp:1174) and the `Op::CallNativeFn` case (LlvmGen.cpp:1792–1850),
plus the per-callsite `pow` check at LlvmGen.cpp:1739–1742 and the extern
declaration block at LlvmGen.cpp:195–289; the `src/LlvmGen.hpp:18` link-line
comment; the `corpus_math_transcendental_*` region of `CMakeLists.txt`
(lines 205–221 + a new LLVM lane in the HAVE_LLVM block); `docs/reference.md`
coverage notes (§6.1 note at reference.md:510–517, §7.3 example at
reference.md:1017–1024); `designs/complete/techdesign-00-overview.md` §5 roadmap list
(overview:214–218).
**Explicitly NOT owned (do not touch):** `src/X64Gen.cpp` / `src/X64.hpp`
(frozen — `docs/techdesign-portable-backend.md` §0.4: "read-only reference
material... never edit them"), `runtime/**` (no ABI change is needed and
`lv_abi.h` contract changes are STOP events per
`docs/techdesign-portable-backend-2.md` §0.2), `src/RuntimeNatives.cpp`,
`src/CGen.cpp`, `src/Resolver.cpp` (all already correct for this group), and
`tests/corpus/**/*.expected` (never edited to make a test pass, doc-2 rule).

---

## 1. Summary — the deferral as logged

The transcendental group — `float.pow(float)` and the `math::log` / `log2` /
`exp` / `sin` / `cos` / `tan` / `atan2` free functions — is fully implemented
on the oracle, IR interpreter, and emit-C++ engines via libm, and
**deliberately not emitted on either native backend**. Each native backend
fails loudly at compile time instead of silently misbehaving:

| engine | state today | evidence |
|---|---|---|
| oracle / IR (`RuntimeNatives.cpp`) | live — `std::pow/log/log2/exp/sin/cos/tan/atan2` straight through | RuntimeNatives.cpp:156 (`pow`), 180–190 (free functions) |
| emit-C++ (`CGen.cpp`) | live — libm via the `<cmath>` preamble | CGen.cpp:11, 718–734 |
| **LLVM** (`LlvmGen.cpp`) | **deferred, loud** — `pow` per-callsite check; free functions hit the `CallNativeFn` floor | LlvmGen.cpp:1739–1742 (`"float native 'pow' not yet emitted (LLVM: transcendental math deferred)"`); LlvmGen.cpp:1846 (`"native floor function '<n>'"`) |
| ELF (`X64Gen.cpp`, frozen) | deferred, loud — same split | X64Gen.cpp:3626–3640 (`pow`), 3854–3856 (free-function floor); `fail()` prefixes `"native-elf backend: "` (X64Gen.cpp:5–8), which `tests/run_elf.sh:10` treats as a clean SKIP |

The deferral is documented in three places that this design will update:
techdesign-06 §2's coverage table (row 2: "**deferred with diagnostic**"),
`docs/reference.md:510–517` (the §6.1 engine-coverage note) and
`reference.md:1017–1024` (the §7.3 out-of-coverage example), and the
`CMakeLists.txt:205–212` comment excluding the `math_transcendental` corpus
from both native lanes.

**This design resolves the LLVM half and re-ratifies the ELF half.** The LLVM
lane becomes fully covered (intrinsics + libm, §3); the ELF lane stays a
standing, explicitly-logged deferral with a named future path and a named
trigger (§4). After this lands, the transcendental group runs on four of five
engines and the only gap is the frozen zero-dep backend — exactly the shape
`strings_native` and `set_map_keys` already have (CMakeLists.txt:192–232).

---

## 2. Why it was deferred (and why half of it no longer applies)

Track 06 §2 drew the line on purpose: *"an implementer must not silently link
libm into the pure backend (zero-dep is the whole point of that engine)"*
(techdesign-06:70–73), and its §7 lists "any pressure to link libm into the
ELF backend" as a STOP condition. At design time (2026-07-05) the LLVM lane
was still "best-effort / uncovered-report" (techdesign-06:8–9), so both
native backends got the same diagnostic treatment.

Two things have changed since:

1. **The portable-backend pivot made LLVM the primary backend** — mandatory
   green ctest lanes (`corpus_llvm`, `corpus_llvm_full`), runtime v2, cross
   targets. "Best-effort" no longer describes it; a primary backend with a
   hole in `sin()` is a language gap, not a coverage note.
2. **The LLVM lane already links libm everywhere.** The zero-dep rationale
   never applied to LLVM — it links `liblvrt.a` through a C driver by design:
   - `--build-native` link line appends `-lm` unconditionally
     (src/main.cpp:485–487, comment at main.cpp:422–423 "links an LLVM object
     against liblvrt.a + -lm");
   - `tests/run_native_llvm.sh:24` and `tests/run_llvm_timed.sh:16` both link
     `-lm`;
   - the runtime archive note at CMakeLists.txt:423 says the same.

   The transcendental group was deferred on LLVM only because Track 06
   predated LLVM parity — the boundary argument was ELF's, inherited by
   adjacency. There is no dependency being *added* by this design; the
   dependency is already on every link line and merely unused.

The ELF rationale, by contrast, is intact and now doubly so: zero-dep is that
backend's identity (reference.md:1005–1012 — "talks to the kernel through the
`syscall` instruction only... the eventual zero-dependency bootstrap seed"),
and the backend is frozen regardless (§0.4). §4 keeps that deferral, properly
logged.

---

## 3. Resolution design — the LLVM lane

### 3.1 Shape: intrinsics + extern-C libm, no runtime-ABI change

The eight natives lower inside generated code, following the exact precedent
of the already-landed Track 06 rows (`floor`/`ceil`/`round`/`trunc` at
LlvmGen.cpp:1279–1292, `sqrt` at 1293–1298): load the receiver/arg payload,
bitcast `i64 → f64`, call, bitcast back, store tag 2. Floats are immediates —
no retain, no ARC, no arena interaction.

Crucially this needs **zero `lvrt_*` additions**: `lv_abi.h` is a STOP-gated
contract (doc-2 §0.2, and techdesign-03's currency check learned this the
hard way for char/Block tags). LLVM f64 intrinsics lower to plain libm
libcalls on x86-64/AArch64 (no hardware instruction for these), and libm is
already on the link line (§2 item 2). `tan`/`atan2` are declared as ordinary
extern-C functions resolved from libm at link time — also not an ABI
addition, for the same reason `-lm` isn't a new dependency.

### 3.2 Per-native lowering table

LLVM here is 18 (`/usr/lib/llvm-18`, CMakeLists.txt:49–57 probes
`llvm-config`). `llvm.tan.*` first appears in LLVM 19 and `llvm.atan2.*` in
LLVM 20 — hence the intrinsic/extern split below. Do not version-detect to
"upgrade" tan/atan2 to intrinsics later; one code shape, revisit only on a
measured need.

| surface | IR op reaching LlvmGen | lowering | notes |
|---|---|---|---|
| `float.pow(float)` | `Op::CallDyn`, `sname == "pow"`, unresolved path (LlvmGen.cpp:1699+) | `llvm.pow.f64(self, arg0)` | new `row(2, ...)` in `emitNativeRows`, beside the `sqrt` row (1293); arg0 = `bitcast(loadPay(arg(0)), f64)` |
| `math::log(float)` | `Op::CallNativeFn`, `sname == "log"`, d=1 (Lower.cpp:1069/1106/1162/1190 emit sites) | `llvm.log.f64` | new branch in the CallNativeFn chain (insert before the `fail` floor at 1845) |
| `math::log2(float)` | CallNativeFn `"log2"`, d=1 | `llvm.log2.f64` | |
| `math::exp(float)` | CallNativeFn `"exp"`, d=1 | `llvm.exp.f64` | |
| `math::sin(float)` | CallNativeFn `"sin"`, d=1 | `llvm.sin.f64` | |
| `math::cos(float)` | CallNativeFn `"cos"`, d=1 | `llvm.cos.f64` | |
| `math::tan(float)` | CallNativeFn `"tan"`, d=1 | extern-C `double tan(double)` | no LLVM-18 intrinsic; `getOrInsertFunction("tan", f64(f64))` |
| `math::atan2(float, float)` | CallNativeFn `"atan2"`, d=2 | extern-C `double atan2(double, double)` | no LLVM-18/19 intrinsic |

Intrinsic declarations use the landed pattern
`Intrinsic::getDeclaration(module.get(), Intrinsic::pow, {f64Ty})`
(cf. LlvmGen.cpp:1290, 1296). The extern-C pair uses the `fn` lambda shape at
LlvmGen.cpp:195–198 but with a real f64 signature
(`FunctionType::get(f64Ty, {f64Ty}, false)`), NOT the `ptrTy` out-param
boundary shape — that boundary rule (LlvmGen.cpp:259–261, "all args cross as
LvValue*") governs `lvrt_*` runtime helpers; pure libm calls follow the
intrinsic precedent instead (raw f64 in/out, like the sqrt row).

### 3.3 Exact code changes (all in `src/LlvmGen.cpp` + one comment in `.hpp`)

1. **`emitNativeRows` (LlvmGen.cpp:1174):** add
   `else if (n == "pow") { row(2, ...llvm.pow.f64...); }` following the sqrt
   row's body shape (1293–1298). Two operands: receiver payload and
   `arg(0)` payload, both bitcast to f64.
2. **Delete the per-callsite `pow` check (LlvmGen.cpp:1725–1742).** The long
   comment block and the `fail(...)` exist only because no row existed and
   the shared dispatch falls through to silent void (bug.md #18). Once the
   row exists the check is dead code — remove it, comment included. The
   X64Gen twin (X64Gen.cpp:3626–3640) **stays untouched**: the ELF lane
   remains deferred and the file is frozen.
3. **`Op::CallNativeFn` (LlvmGen.cpp:1792):** seven new `else if` branches
   before the `fail("native floor function ...")` floor at 1845–1847,
   mirroring CGen's branch list (CGen.cpp:720–734) name-for-name and
   arity-for-arity. Each: payload → f64 → call → `storeTP(dst, tag 2, ...)`.
   No `retainDst()` (immediates). The existing unconditional
   `emitThrowCheck` at 1848 stays — these natives never raise (§3.4), the
   check is harmless and keeps the case's single exit shape.
4. **Extern declarations:** add `rtLibmTan` / `rtLibmAtan2` (names "tan",
   "atan2") near the declaration block, with a comment noting they are libm
   symbols, not `lv_abi.h` members — the distinction matters for the doc-2
   §2 ABI freeze audit.
5. **`src/LlvmGen.hpp:18` comment:** the documented manual link line
   (`cc out.o tests/support/llvm_stub_runtime.c -o prog`) gains `-lm`, so
   `--native-obj` users linking by hand pick up libm the same way
   `--build-native` (main.cpp:486) and the test runners already do.

### 3.4 Semantics to pin (oracle parity contract)

The oracle calls libm and wraps the result — it never inspects `errno` and
never raises for domain errors (RuntimeNatives.cpp:180–190: `err` is never
set for this group; CGen.cpp:720–734 is a straight passthrough). The LLVM
lane must pin the identical contract:

- `log(0.0)` → `-inf`; `log(x<0)` → NaN; same for `log2`.
- `exp` overflow → `+inf`, underflow → `0.0`.
- `sin/cos/tan(±inf)` → NaN.
- `atan2` follows IEEE special cases (`atan2(1.0, 1.0)` = pi/4, the existing
  corpus line).
- `pow` follows IEEE 754 `pow` special cases (`pow(x, 0) == 1.0` for all x).
- **Never a throw, never an errno read.** LLVM's f64 intrinsics are specified
  errno-free (that is what permits their constant folding), which matches
  "nothing reads errno" exactly.

This is math convention, consistent with the already-landed
`sqrt(negative) → NaN, not a throw` ruling (techdesign-06 §1.2,
RuntimeNatives.cpp:155).

### 3.5 Test lanes

- **New ctest lane** `corpus_math_transcendental_llvm` inside the HAVE_LLVM
  block, modeled byte-for-byte on `corpus_llvm_objects`
  (CMakeLists.txt:335–338): `run_native_llvm.sh` over
  `tests/corpus/math_transcendental` with `${LANG_LVRT_SOURCES}`.
- **Reuse the existing fixture** `tests/corpus/math_transcendental/trig.ext`
  + `trig.expected` unchanged. Its values are parity-safe by construction
  (§5 problem 4): `pow(2,10)=1024`, `log(e)=1`, `log2(8)=3`, `sin(0)=0`,
  `cos(0)=1`, `tan(0)=0`, `atan2(1,1)=pi/4` — exact or far from 6-decimal
  rounding boundaries. Do NOT rename it to `.lev` (corpus `.ext` fixtures are
  grandfathered by owner directive, CMakeLists.txt:159) and do not create any
  NEW `.ext` file — any additional programs are `.lev`, which requires the
  runner-glob extension flagged in §5 problem 7 (prefer reuse; only extend if
  a new program is genuinely needed).
- **Update the CMakeLists comment block** (CMakeLists.txt:205–212): LLVM is
  no longer excluded; the dir stays out of the shared corpus only because of
  the ELF lane, same reasoning as `strings_native` (CMakeLists.txt:192–196).
  The stale header comment inside `trig.ext` itself ("excluded from those two
  engines") is left byte-identical (fixture-untouched directive); the
  reference-doc update (§3.6) is the authoritative record.
- The three existing lanes (`corpus_math_transcendental_treewalk`/`_ir`/
  `_cpp`, CMakeLists.txt:213–221) are untouched and must stay green.

### 3.6 Reference-doc duty

- `docs/reference.md:510–517` (§6.1 engine-coverage note): rewrite — the
  transcendental group is live on oracle/IR/emit-C++/LLVM; deferred only on
  the frozen ELF backend.
- `docs/reference.md:1017–1024` (§7.3 concrete out-of-coverage example):
  rewrite to be ELF-only, and note the LLVM resolution date + this design.
- `designs/complete/techdesign-00-overview.md:214–218` (§5 deferred-items roadmap):
  add the ELF transcendental project explicitly (§4 below) so the remaining
  half of the deferral is on the roadmap by name, not implied.
- `designs/complete/techdesign-06-stdlib-math.md` is **not edited**
  (completed docs are the historical record; this doc supersedes its §2 LLVM
  column and says so here).
- info.md: no change (no design-shape change; a coverage completion).

---

## 4. The ELF lane — standing deferral, re-ratified

**Decision: the ELF/X64Gen lane stays deferred indefinitely.** This is not a
gap that drifted — it is the intersection of two owner-level rulings:

1. **Zero-dep identity:** the pure backend's contract is "statically-linked,
   syscall-only, not a dynamic executable" (reference.md:1005–1012). Linking
   libm breaks the property that is the entire point of the engine;
   techdesign-06 §7 lists exactly that pressure as a STOP condition. Still
   binding.
2. **The freeze:** `src/X64Gen.cpp`/`src/X64.hpp` are read-only reference
   material (portable-backend §0.4; owner ruling logged in techdesign-06 §8:
   "do no further X64Gen work going forward; LLVM is the real backend now").
   Even a libm-free emission would be new X64Gen work, which is barred
   independent of the dependency question.

What stays in place, verified as of this doc: the per-callsite `pow`
diagnostic (X64Gen.cpp:3626–3640), the free-function floor
(X64Gen.cpp:3854–3856), the `"native-elf backend: "` prefix (X64Gen.cpp:5–8)
that `tests/run_elf.sh:10` greps into a clean `SKIP (beyond ELF coverage)`,
and the corpus-dir exclusion. Nothing about the ELF lane changes in this
design's implementation — by design, not by omission.

### 4.1 What the future project would be (for the roadmap entry)

A libm-free transcendental kernel set is a genuinely separate project, weeks
not days, and none of it is "add rows to a switch":

- **Kernels:** minimax-polynomial (Remez-derived) f64 approximations per
  function, with argument range reduction — Cody–Waite splitting for moderate
  trig arguments and Payne–Hanek for large ones; `exp` via `k*ln2` splitting;
  `log`/`log2` via exponent/mantissa bit extraction; `pow` composed as
  `exp2(y * log2(x))` carried in extended precision to survive the
  cancellation. Target 1–2 ulp (correctly-rounded is out of scope), plus the
  full IEEE special-case table from §3.4.
- **Where it lives:** two libm-free routes exist and both have prerequisites.
  (a) *In-language prelude implementations* (pure Leviathan float arithmetic
  — runs on every engine automatically) — blocked on a `floatToBits`/
  `bitsToFloat` native pair for range reduction, whose ELF emission is itself
  frozen-file work today. (b) *Emission into the pure backend's generated
  runtime* — squarely X64Gen work. Either way the freeze is the gate, which
  is why this is a trigger-based deferral and not a milestone.
- **Verification:** differential test against the oracle (same-host libm)
  over millions of random inputs + the special-value table, with an explicit
  ulp budget — a new harness, not the byte-diff corpus.

**Trigger to start (named, so the roadmap entry is actionable):** the
zero-dep bootstrap seed (reference.md:1010–1012) reaching a stage that
actually calls transcendentals — realistically the self-host gate window
(G5, ~2027-01 per the portable-pivot plan) — or an explicit owner ruling
that re-opens X64Gen scope. Until then, `run_elf.sh`'s SKIP line keeps the
gap visible on every test run, which is the deferral working as intended.

---

## 5. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Windows/msvcrt libm** — cross `--build-native --target x86_64-pc-windows-gnu` (main.cpp:431–487) links `-lm`, which on MinGW is a stub archive; the real math comes from the UCRT plus mingw-w64's own implementations (historically msvcrt lacked C99 `log2`; mingw-w64 ships one). Symbols resolve, but last-ulp results can differ from glibc, so glibc-generated `.expected` files are not portable for this group. | Keep `math_transcendental` OUT of `tests/run_wine_cross.sh`'s scope — its own header already scopes the lane to "core + objects corpus subsets" (run_wine_cross.sh:9–13), so this is the status quo, now stated as policy. If a per-triple lane is ever wanted, regenerate per-triple expected files; never widen the host files' tolerance. Same policy for the qemu/AArch64 lane. |
| 2 | **errno / domain-error semantics drift** — glibc libm sets errno; LLVM intrinsics are errno-free; a future reader might "fix" the mismatch by adding throws. | Pin §3.4 in reference.md §6.1b as the contract: NaN/±inf returns, never a throw, errno never read — matching RuntimeNatives.cpp:180–190 exactly. The intrinsic errno-freedom is then a feature (it is what makes constant folding legal), not a divergence. |
| 3 | **Fast-math flags would break NaN honesty** — any pass-pipeline change that sets `nnan`/`ninf` FMF on these calls silently voids §3.4. | `emitObject`'s pipeline (LlvmGen.cpp:2392+, default O2 per main.cpp:187) sets no fast-math flags today; add a one-line code comment at the new branches stating FMF must stay off for this group, and a STOP condition (§8) if pipeline work ever proposes it. |
| 4 | **Oracle parity / determinism across libm implementations** — transcendentals are not correctly rounded; glibc vs musl vs UCRT differ in the last ulp. Two sub-risks: (a) on-host divergence between engines; (b) compile-time constant folding of intrinsic calls (LLVM folds via the *compiler host's* libm) vs runtime target libm under cross-compilation. | (a) is a non-risk on-host: oracle, IR, emit-C++, and the LLVM-emitted binary all call the SAME host libm at runtime, and the compiler host == run host, so folded constants match too — byte-identical 6-decimal `toString` output (the P4-pinned format, techdesign-06 §8). (b) only bites cross-target with constant args; cross lanes exclude this dir (problem 1). Additionally keep corpus values exact-or-far-from-boundary (the existing `trig.ext` set already is; keep that discipline for any new program). If on-host parity ever fails, that is a STOP (§8), not a tolerance widen. |
| 5 | **LLVM 18 has no `llvm.tan`/`llvm.atan2`** (they arrive in LLVM 19/20); an implementer following the "intrinsics where available" instruction too literally would hit a build error. | §3.2's split is normative: six intrinsics + two extern-C libm calls. One code shape; no version detection. |
| 6 | **Payload-typing hazard** — the `bitcast(loadPay(...), f64)` shape assumes tag 2. The checker types `math::` params as `float` (Resolver.cpp:1270–1276), but prelude checking has known holes (bug.md #13); an int argument would be reinterpreted as garbage bits. | Parity note: the oracle reads the `.f` union field the same blind way (RuntimeNatives.cpp:180–190), so behavior cannot *diverge* — but probe it (P1). If `math::sin(1)` is silently garbage everywhere, file a bug.md entry (checker coercion gap) — do not fix it inside this design; if it is checker-rejected or coerced, no action. |
| 7 | **`pow` joins the bug.md #18 hole** — deleting the per-callsite check (§3.3 item 2) means a non-float receiver dispatching `"pow"` now falls through `emitNativeRows` to silent void, like `string.byteAt` et al. today (bug.md #18, LlvmGen.cpp:1727–1733's own comment). | Acceptable and strictly no worse: the check existed only because there was no row at all. Add one line to the bug.md #18 entry noting `pow` now has a tag-2 row and shares the general unmatched-tag hole; the real fix stays with #18's owner ruling. |
| 8 | **Runner globs are `.ext`-only** (`run_native_llvm.sh:17`, `run_corpus.sh:5`, `run_native.sh:9`) but new test files must be `.lev` (hard rule). | M1 needs no new program (reuse `trig.ext`). If M2's acceptance genuinely needs another program (e.g. a NaN/inf special-case program per §3.4), extend the globs to `*.ext *.lev` (nullglob-safe) in the runners as a separate, explicitly-logged edit — never create a new `.ext`. |

---

## 6. P-probes (run before M1's first commit, log results in §9)

- **P1 (arg typing):** `math::sin(1)` (int literal) on all four live engines —
  checker error, coercion, or garbage? Decides problem 6's disposition.
- **P2 (IR shape):** `--emit-llvm` on `trig.ext` variantized with *variable*
  (not constant) arguments — confirm `llvm.pow.f64`/`llvm.sin.f64`/... calls
  and `tan`/`atan2` extern declarations appear; confirm no fast-math flags on
  the calls.
- **P3 (link):** `--build-native` of trig.ext runs and diffs clean;
  `nm` the `--native-obj` object and confirm the undefined symbol set
  (`pow`, `sin`, ..., `tan`, `atan2`) resolves against `-lm` on the
  documented manual link line (post §3.3 item 5).
- **P4 (special cases):** a scratch program covering §3.4's table —
  `log(0.0)`, `log(0.0 - 1.0)`, `pow` specials, `atan2` quadrants — diffed
  oracle vs LLVM binary. Source negative constants by subtraction, NOT
  division (bug.md #12's LLVM `0.0/0.0` divergence is open, awaiting its own
  ruling, and must not contaminate this probe).

---

## 7. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | P1–P4 logged; §3.3 items 1–5 (the eight lowerings, pow-check deletion, extern decls, .hpp comment) | `trig.ext` output byte-identical oracle vs `--build-native` binary on host; existing `corpus_llvm`/`corpus_llvm_full` lanes still green (no regression from the CallNativeFn floor change) |
| M2 | `corpus_math_transcendental_llvm` ctest lane + CMakeLists comment update; reference.md §6.1/§7.3 rewrites; overview §5 roadmap entry for the ELF future project (§4.1 wording); bug.md #18 pow note | full `ctest` green including the new lane; `run_elf.sh` still SKIPs the dir cleanly (nothing ELF-side changed) |

Target: **Jul 7 – Jul 8** (M1 1d, M2 0.5d). No dependency on any open track;
touches no file another open track owns (Track 03 is STOP-parked on statics;
Tracks 07/08/09 do not own LlvmGen's math regions).

---

## 8. STOP conditions

- Any pressure to emit, link, or work around the transcendental group on the
  **ELF backend** — including "just a tiny movq for floatToBits" — is the
  frozen-file STOP (portable-backend §0.4) and techdesign-06 §7's libm STOP,
  both still binding. Escalate; do not touch X64Gen.cpp/X64.hpp.
- Implementation discovers a genuine need for a **new `lvrt_*` symbol or any
  `lv_abi.h` change** (this design believes there is none) — doc-2 §0.2:
  contract changes are always STOP events.
- **On-host parity failure** between the oracle and the LLVM binary on
  trig.expected — this design's model (§5 problem 4) says that cannot happen
  with one host libm; if it does, something is folding or flagging
  nonstandardly. STOP and escalate with the IR diff; never widen the
  expected file.
- Any proposal to set fast-math flags on these calls (§5 problem 3).
- P1 reveals **cross-engine divergence** (coerced on some engines, garbage on
  others) for int-typed args — file the bug and STOP for a ruling only if it
  blocks the corpus; otherwise file and proceed.

---

## 9. Implementation log

*(empty — filled by the implementing agent: probe results, deviations,
new bugs filed, per the house convention.)*
