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
| P0       | — |
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

#73 closed 2026-07-21 (global `Array<T>` COW self-append leak on LLVM native).
Verification + register closure: the fix itself was already in the tree, landed
un-registered — the design docs (`examples/recon/DESIGN-2.md`,
`designs/sonar/dom/techdesign-00-dom-overview.md`,
`designs/sonar_v2/dom/techdesign-00-dom-overview.md`) still carried it as
"[P2, OPEN]" and workaround mandates ("grow a local, assign once"; "no
namespace-global growing arrays") were still being propagated.

Filed symptom: a global (file-level or namespace-level) `Array<T>` grown in a
loop by the COW self-append idiom `xs = xs.add(v);` leaked every superseded
intermediate buffer on the LLVM native backend — O(N²) live bytes in the
grow-only shape (`lvrt: heap exhausted` near N≈10k) — while the identical
idiom on a LOCAL was clean (#31/#90 territory).

Root cause: `Op::StoreGlobal` in the LLVM backend originally mirrored the
X64Gen parity path (`src/X64Gen.cpp` `Op::StoreGlobal` — store + retain the
new value only), which skips releasing the global slot's OLD reference on the
false premise that globals are write-once. Locals never hit this: a local
write-back reuses the register window (#90's consumed-receiver release), but a
global write-back goes through the dk==0 StoreGlobal slot store, which owns
its own ARC. Fix (already present): `src/backend/LlvmGenOps.cpp`
`Op::StoreGlobal` stashes the old slot value, stores + retains the new one,
then releases the old (retain-new-before-release-old keeps `xs = xs`
self-assign safe; the zero-initialized globals array makes the first store's
release a no-op).

Proven this is THE relevant code by red/green differential (2026-07-21):
temporarily removing only the release-old call and rebuilding reproduces the
filed leak exactly — churn shape (global reset + grown twice per iteration)
live-at-exit 16,736 B at N=100 → 128,736 B at N=800 (~160 B/iteration, oracle
root set constant at 14); grow-only shape goes quadratic, 122,176 B at N=100 →
7,548,224 B at N=800. With the release-old restored: flat 672 B at both N,
root set constant — and the grow-only shape is linear (the genuinely-reachable
final buffer only). Namespace-globals written from inside their own
namespace's functions (the Sonar/DOM carrier shape) measure equally flat; note
`NS::x = ...` from OUTSIDE the namespace does not lower at all on the IR
engines ("IR: not yet lowerable: name 'Store'") — a separate limitation, not a
leak. Object-element (`Array<Node>`) and in-language-callee
(`xs = xs.skip(1)`, the #90 shape with a global write-back target) variants:
flat. Columnar and `--no-columnar` layouts: both flat.

Regression floor: `tests/corpus/churn/global_array_cow_growth.lev` (file-global
+ namespace-global COW growth, reset per iteration; runs in both churn-leak
lanes). Marked `XFAIL-ELF`: frozen X64Gen's StoreGlobal still never releases
the old value, so the leak persists on ELF — P3.2 frozen-backend debt, no
X64Gen change permitted. The three design docs above now point here.

#102 fixed 2026-07-21 (method-nested lambda over-captured `this`, forming an
uncollectable refcount cycle when the closure was stored into a field of the same
object). Ruling + fix, not just a diagnosis.

Root cause: a lambda literal written inside a method captured `this`
UNCONDITIONALLY, even when its body referenced neither `this` nor any member.
Reassigning such a no-op closure into a field of the same object formed a genuine
cycle `object -> field -> closure -> this=object`; Leviathan is pure ARC with no
cycle collector, so both nodes leaked every iteration on every engine.

Ruling (capture-narrowing, three investigation questions):
1. SAFE. Capture `this` only when the lambda body actually references `this`
   (bare) or a class member. The gate uses the SAME predicate the member
   body-lowering already uses to route a bare name through the receiver
   (`classHasMember`, which covers fields AND methods since both are flattened
   shape slots, plus this class's accessors), so it can only ever REDUCE capture
   relative to the old always-capture rule — never under-capture. Verified the
   subtle cases the naive "fields only" sketch would have missed: bare `this`
   (an `ExprKind::This`, not a `Name` — now recorded as the sentinel name
   "this"), self-METHOD calls (methods ARE shape slots, so already covered), and
   set-accessor writes (covered by the accessor arm). The metaprogramming rule
   engine runs BEFORE the checker and lowering (`src/main.cpp`), so capture
   analysis always sees fully-expanded bodies — no ordering hazard. The
   capture set also feeds the spawn/cross-thread reject (`lvThreadCopy` walks
   `thisObj` unconditionally when set), so narrowing only REMOVES false
   rejections of member-born lambdas that never touch the receiver — never a new
   safety hole, since a body that doesn't reference this/members genuinely does
   not need the object across the boundary.
2. Do NOT touch `src/X64Gen.cpp` (frozen, no-touch). It turned out this was moot
   for the CYCLE: X64Gen consumes Lower's `MakeClosure`/`CaptureVar` IR ops
   (`src/X64Gen.cpp:3752-3766`), it does NOT recompute captures — so narrowing
   Lower's capture list flows through to ELF automatically. The bug entry's claim
   that the over-capture was "replicated in X64Gen" was imprecise. ELF's residual
   ~32 B/iter growth on the repro is a SEPARATE, pre-existing frozen-backend
   escaping-tier gap (a closure held in an object field is not reclaimed when the
   object drops — reproduced with a closure BORN IN A FREE FUNCTION that never
   captured `this`, before or after this fix), out of scope and unchanged.
3. No intentional rationale for always-capture — an oversight, not a deliberate
   choice (the surrounding code already computed the referenced-name set; `this`
   was just force-included).

Fix (two independent capture implementations, both narrowed identically; frozen
X64Gen untouched):
- `src/Lower.cpp` — `lwrCollectExprNames` now records bare `this`; `lowerLambda`
  gates both `this`-capture sites (the scopes_ loop and the enclosing-receiver
  add) on `needThis`. Fixes the IR interpreter AND LLVM (both consume this IR).
- `src/Eval.cpp` — the tree-walk oracle snapshots `cl->thisObj` only when
  `lambdaCapturesThis(e, thisClass_)` (new `Evaluator` helper mirroring the IR
  predicate); `cl->thisClass` stays (a type pointer, no refcount).

Verified: oracle `--mem-verify` root set flat (13 at N=100 and N=800; was
213 -> 1613); IR-interp mem-verify flat; LLVM churn live-at-exit flat (640 B at
both N). Regression floor `tests/corpus/churn/method_lambda_this_cycle.ext`
(XFAIL-ELF for the orthogonal frozen-backend closure-field gap; a real PASS on
the LLVM lane). Full closure/lambda/spawn/task/thread/meta/composition corpora
green across treewalk/ir/llvm.

#95 fixed 2026-07-20 (Atlantis routing corpus SEGFAULT on LLVM). Not a Track 06
regression and not runtime-stale: a latent value-struct ARC over-release exposed
by the 2026-07-19 base-qualified-call merge (Lower.cpp `lowerCall`). Root cause:
a METHOD call on a value-struct receiver marshaled the receiver into the CallDyn
window via a plain `Op::Move` — a BARE ALIAS, since the wrap's retain no-ops on
value classes — and that window register survived to frame exit. bug #66 cleared
the for-in loop VARIABLE for exactly this stale-alias shape, but a method call on
the loop var (`Router.finalize`'s `rec.key()`) copies the alias into a SECOND
(window) register #66's clear never saw. Once the aliased boxed element's array
died in the same frame (`this.routeList = rebuilt`), `releaseAllRegs` released
that stale window alias: it read the freed block's classId (garbage), the
value-class skip in `lv_is_counted` failed, and the "release" decremented a
freelist next-pointer word — heap corruption surfacing later inside
`lv_alloc_heap`, far from the site (the classic P0.3 shape). Boxed-only because a
flat struct is dense-inlined (no per-element free) — the nested `Array<ParamDesc>`
field forced the boxed path (#66). Fixed in `src/Lower.cpp`: after a value-struct
(`definiteValueStruct`) receiver's non-consumed method call, void the receiver
window register (`Op::LoadConst … vvoid()`), same shape as #66's loop-var clear;
consumed (COW self-append) receivers are containers whose window slot the backend
already voids. Engine-neutral (an extra dead-store on every engine); oracle/IR
never crashed but inherit the clear harmlessly. Regression floor:
`tests/corpus/composition/aggregates/green/array_struct_method_recv_alias.lev`
(oracle+IR+cpp+llvm; verified red→green — SIGSEGV on LLVM before the fix, `70`
after). Full atlantis corpus suite + composition/churn-leak/ownership ctest lanes
green on all engines.

#91 found AND fixed 2026-07-19 (same session, owner-directed): rules and
attributes declared in a NESTED namespace (`namespace Atlantis { namespace Orm
{ … } }` or `namespace Atlantis::Orm { … }`) never fired — `uses Atlantis::Orm`
+ bare `@Table` errored `no attribute 'Table' in scope`; a fully qualified
`@Atlantis::Orm::Table` resolved the attribute but the co-located rule stayed
silent with `matched no imported rule (missing 'uses Orm'?)`; `uses Orm` (the
name the warning asked for) was `unknown namespace`. Root cause: `Rules.cpp`
keyed rule/attribute namespaces by the innermost SIMPLE name (`collectRules`/
`indexDecls` recursion dropped the prefix) while `computeFileImports` produces
full paths ("Atlantis::Orm"), and `RuleEngine::namespaceScope` could only
resolve root-level names — nested declarations fell into the gap between the
two spellings, so the visibility test `effective.count(r.ns)` could never be
true. Fixed by (1) making `namespaceScope` walk `::`-separated paths, (2)
carrying the full qualified path through the `collectRules`/`indexDecls`
recursions (with the namespace symbol resolved through the parent path, not
the global scope), and (3) building def-site qualification (§10) as a chained
Member expression (`Atlantis` then `::Orm` then `::name`) instead of a single
Name token containing "::". Regression floor:
`packages/atlantis/tests/probes/orm_p1_nested_ns_rules.lev` (oracle+IR+LLVM);
full meta/rule/expand/reify ctest set green after the fix. This unblocked the
ORM Track 06 amended-C1 placement (rules/attributes subsystem-owned in
`Atlantis::Orm`) with no fallback relocation needed.

#99 fixed 2026-07-20 (a class-typed-param function with an early `return` from a
`for` over `Array<Struct>` corrupting a LATER unrelated heap call on LLVM). The
sibling of #95, same borrowed-value-struct-alias family. Root cause: a
value-struct loop variable is a BORROWED alias of the boxed array element (#66);
`StmtKind::ForIn` (`src/Lower.cpp`) voids that alias register AFTER the loop so
`releaseAllRegs` never releases it, and a `break` reaches that void too — but an
early `return` jumps straight to `Op::Ret`/`releaseAllRegs`, BYPASSING the
post-loop void. `releaseAllRegs` (`src/LlvmGen.cpp`, every `Op::Ret`/`RetVoid`/
unwind) then released the stale value-struct alias whose backing `Array` was
already freed in the same frame: it read the freed block's classId (garbage), the
value-class skip in `lv_is_counted` failed, and the "release" decremented a
freelist word — heap corruption surfacing far from the site (the bug entry's core
dump inside a later `digest::hmacSha256`; the minimal regression pin silently
drops a later `writeln` on LLVM). The class-typed parameter is required only
because it shifts the frame's register/release ordering into the corrupting shape;
the alias itself is the defect. Fixed in `src/Lower.cpp`: `LoopCtx` gains
`borrowedElem` (the borrowed value-struct loop-var register, set under the same
gate as the post-loop void), and `StmtKind::Return` voids every active loop's
`borrowedElem` — after the return value is computed, before control reaches the
frame exit — on both the plain-return and using-cleanup-chain paths. Engine-neutral
(a dead store on oracle/IR, which never crashed). Regression floor:
`tests/corpus/composition/aggregates/green/array_struct_early_return_param_alias.lev`
(oracle+IR+cpp+llvm; verified red→green — the later output was silently dropped on
LLVM before the fix, correct after). Verified: the bug entry's exact minimal repro
now prints all three lines including after `hmacSha256`; composition
(treewalk/ir/cpp/llvm/red), churn-leak (incl. columnar/tasks), map-return-ownership,
corpus (treewalk/ir/llvm/llvm_full), and meta ctest lanes all green; full atlantis
corpus suite green on oracle+IR+LLVM except the `auth` LLVM lane, which still
crashes LATER in `SessionStrategy.issue`'s `await`→store path — a DISTINCT,
still-open instance of the same family (the `Await`→`CopyVal` borrowed-alias case
named in `known_bugs_2.md` #96), documented by this bug's own entry as the broader
systemic issue and NOT part of #99's minimal for-in/early-return shape (its
accepted-red posture, same as #95's routing lane, is unchanged by this fix).

#93 resolved 2026-07-20 as a MISDIAGNOSIS — there was no runtime corruption; the
filing misread a debug-dump artifact. Claim: a punctuation-only (`"."`) or
`@`-leading (`"@Row "`) string literal inside an `inject` quasiquote template lost
its string-literal-ness during the template clone/re-lex and produced a wrong
concatenation. Investigation: reproduced the filing's exact `--ast-after-rules`
output (`(("users" + .) + "id")`, `(@Row  + …)`) on a freshly rebuilt binary AND on
the binary at the exact filing commit (`9709700`, ORM Track 06) — but the RUNTIME
output was correct in every case (`users.id`, `@Row id`, and a full sweep
`"" "@" ";" "()" "::" "[" "]"`), on the oracle (`--run`), IR (`--ir`), and emit-C++
(expand round-trip). The dump is not corruption: the `--ast-after-rules` printer
(`AstPrinter.cpp` `exprStr`, `ExprKind::StringLit → sv(e->text)`) renders a
StringLit's `text` BARE by design (pinned by `tests/test_parser.cpp:141`,
`"plain" → Expr console.writeln(plain)`), and a SOURCE string literal is stored as
an `isRawSegment` bare content slice (`Parser::parseInterpolatedString`,
`"." → text "."` i.e. bare `.`), whereas a REIFIED `$hole.name` literal keeps its
quotes (`RuleEngine::reify`, `"users"`). So the dump shows quoted holes next to
bare source literals (`("users" + .)`) for a perfectly correct program — the `.` is
a valid StringLit whose content is `.`. `cloneExpr` already carries the StringLit
kind and the `isRawSegment`/`isQuasiPayload`/`isRawString` flags through cloning
(present at the filing commit), so the re-lex path never lost fidelity. The
source-faithful lens `--expand` (`printProgramSource` → `srcString`, which re-quotes
raw segments) already prints `(("users" + ".") + "id")` and `("@Row " + "id")`
correctly. No compiler code changed (no defect to fix; the bare-dump format is
intended, tested behavior — re-quoting it is a design decision, not warranted).
Regression floor: `tests/corpus/meta/rule_punct_literal.ext` (+`.expected`), pinning
punctuation-only and `@`-leading source literals concatenated with reified holes
inside `$for` inject templates — green on `corpus_meta_treewalk` (oracle),
`corpus_meta_ir` (IR), and `corpus_meta_expand_roundtrip` (emit-C++ compile+run);
would go red if a future clone/re-lex change ever actually dropped a StringLit kind.
The ORM's `Atlantis::Orm::ctx(table, col)`/`ctxRow(col)` helpers
(`packages/atlantis/src/orm/orm.lev:240-241`, called from the `ormFromRow`/
`ormRowFromRow` templates) were adopted to dodge this non-bug; they are now
removable — inline `$t.name + "." + $f.name` / `"@Row " + $f.name` templates
produce identical correct output (verified against the exact nested
`step(this.$f = fromDb(…, ctx($t.name, $f.name)))` shape). Left in place as
optional cleanup, not required by this resolution.

#98 fixed 2026-07-21 (metaprogramming rules/attributes declared in the prelude
silently never fired). Root cause: `main.cpp` built one `RuleEngine` and called
`engine->run(program)` once, always on the USER's parsed tree; the prelude was
parsed by `Resolver::parsePrelude()` into a separate `Program` (`preludeProgram_`)
handed to the engine ONLY for `eval_.initGlobals` (comptime-value seeding).
Nothing in the rule engine — `collectRules`/`indexDecls`/`walkAttrs`/`runRules` —
ever walked the prelude's items, so a rule/attribute authored in a `prelude/*.lev`
segment was dead code by construction (exit 0, no diagnostic, the annotated
symbol ran its untouched placeholder body). Confirmed NOT a deliberate boundary:
`designs/techdesign-bindgen-metaprog-scope.md` §6's fallback explicitly asked to
"confirm-or-add rule-stage processing of the prelude segment", and its §13 spike
1 was written precisely to discover this gap — an oversight, not a decision. Fix
(`src/Rules.cpp`, `src/Rules.hpp`): the engine now stores the prelude program and,
in `run()`, also (a) `collectRules` over it (prelude rules join `rules_`), (b)
`walkAttrs` over it (prelude `@Attr` uses resolve), and (c) `indexDecls` over it
(prelude decls become matchable), so prelude-declared rules match prelude AND user
decls and user rules can match prelude-carried attributes. The prelude lives in a
SEPARATE source buffer whose offsets collide with the user tree's, so it cannot
share `fileOf`'s offset-keyed slots: its provenance is computed on its own
(`computeFileImports` over one synthetic full-range file) and appended to
`imports_` at a dedicated `preludeFileIdx_`; a `fileIdxOverride_` forces that slot
onto `indexDecls`/`processAttrs` while the prelude is walked (co-location needs no
`uses`, so a prelude rule + its subject in one segment resolve via `declaresInto`).
Second half of the fix (the subtle one — `src/main.cpp`, `src/Resolver.{hpp,cpp}`):
a rule that rewrites a PRELUDE decl mutates the pass-1 prelude tree, but pass 2's
fresh `Resolver` re-parsed a PRISTINE prelude, dropping the injection before the
backend saw it. Pass 2 now `adoptPrelude()`s the rule-processed pass-1 tree
(re-resolving the mutation like hand-written code, and never re-parsing the
detached `rule` statements back in) whenever the prelude carried meta; a meta-free
prelude — the common case — keeps the old fresh-re-parse path byte-for-byte. The
rule stage is also now gated on `program.hasMeta || preludeProgram.hasMeta`, so it
runs for a caller with no meta of its own when the prelude supplies it. Prelude
rule/attribute diagnostics ride the real user sink (no new suppression path,
matching the existing choice for parse diagnostics); dangling-attribute warnings
are still emitted over the user tree only, so the trusted prelude never nags the
caller. Regression floor: `tests/run_prelude_rules.sh` (ctest `prelude_rules`,
wired in `CMakeLists.txt` beside `prelude_select`) copies the real prelude, appends
a self-contained `generates body of` spike to a segment, and asserts a plain
no-meta caller sees the rewritten body (`12345`) on BOTH `--run` (oracle) and
`--ir`, plus a control proving the spike is absent from the shipped prelude — the
`prelude/*.lev` files are never edited. Verified: the entry's exact repro now
prints `12345` on `--run` and `--ir`; `metatests` (124 checks), `corpus_meta_{
treewalk,ir}` (55 files each), `corpus_meta_expand_roundtrip`, the meta LLVM legs
(`corpus_procedural_macros_llvm`, `corpus_target_uses_llvm`), every `rule_*` reject
row, base `corpus_{treewalk,ir,ir_verify}`, and the parser/resolver/checker/eval
unit suites all green. Unblocks `techdesign-bindgen-metaprog-scope.md`'s parked
DOM `@extern` bindgen: its "Recommended" placement (the Dom surface as a shipped
`dom.lev` prelude/stdlib segment with co-located `@extern` rules) now has real
rule-stage participation — the §6 STOP that forced the project-file fallback is
resolved. Known small edges left for that pickup, none blocking: prelude rules are
offset-ordered interleaved with user rules (arbitrary but deterministic across the
two buffers); the reentrant fixpoint and `namespace N` anchor injection still
operate over the user tree only; and cross-buffer splice sites (`@PreludeAttr()` in
user code targeting a prelude-declared attribute) are not yet indexed — all
out-of-scope for the repro and filable if a consumer needs them.

---

#103 fixed 2026-07-21 (same day it was filed; fix landed via the agent0 merge):
emit-C++ (`--build`) deferred `Process`/`Pty` with the generic construct-coverage
error instead of naming the blocking sys native — the object/closure-wrapping
opcode tripped CGen's generic `default:` first and the sink reported only that
first error, so emission never reached the prelude-ctor `CallNativeFn` that
would have named it. Fixed in `CGen::generate()` with a pre-emit BFS from the
entry over static call/ctor/closure edges (`Call`/`NewObject`/`MakeClosure`
operand b): the first uncovered native on that walk is reported up front as
`native backend: native '<sysX>'` (Process -> sysSpawn, Pty -> sysPtySpawn),
deterministically, regardless of emission order. Entry-rooted static edges keep
the named native tied to the user's construct rather than an unrelated
by-name-reachable prelude native; a native surfaced this way is genuinely
uncoverable, so no previously-compiling program is newly rejected. The
`sys_natives` lane (§10 spawn / §11 pty assertions) is green again.
