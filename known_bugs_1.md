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
| P1       | #98 |
| P2       | #102 |
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

## #102 [P2] — reassigning a closure-typed FIELD leaks the old closure on LLVM

**Renumbered from #99** (2026-07-20, agent2↔origin/master merge audit): filed as
#99 on the agent2 line, but origin/master had already assigned #99 to the
`Array<Struct>`-loop corruption P0 below (canonical, cross-referenced from
`designs/complete/techdesign-08-auth-security.md` and `known_bugs_2.md`), so this
closure-field-leak entry takes the next free number. #100/#101 are already in
`known_bugs_2.md`.

**Found:** 2026-07-20, LA-HTTP-STREAM (server-side streaming responses) ARC
churn gate. The prelude's `TcpStream`/`ChunkedSink` teardown broke the
connection<->stream<->callback cycle by overwriting the stored callback field
with a no-op closure (`onChunk = (s) => {}`), the shape the design specified.
On LLVM that overwrite never releases the previous closure (or anything it
captures), so a program that churns callback-bearing objects leaks one closure
graph per iteration while the oracle's reachability root set stays flat.
**Priority justification:** P2.2 — resource-only: output is correct on every
engine (oracle/IR/LLVM print identically) and the oracle reachability oracle
reports a CONSTANT root set in N; only the LLVM escaping-tier live-at-exit
grows with N. No wrong value, no crash — memory behavior is wrong on one
actively-maintained engine.

**Minimal repro** (`@N@` swept by `fuzz/churn_leak.py`-style magnitudes):

```
class Payload { Array<int> data; int v = 0; }
class Holder {
    (int) => void cb; bool has = false;
    void set((int) => void c) { cb = c; has = true; }
    void detach() { cb = (x) => { }; has = false; }   // reassign: leaks old cb + its Payload
}
void run(int n) {
    int sink = 0;
    for (int i in 1..n) {
        Holder h = Holder(); Payload p = Payload(); p.v = i; p.data = p.data.add(i);
        h.set((k) => { sink = p.v; });   // closure captures p
        h.detach();                       // must drop the closure -> drop p; on LLVM it does NOT
        sink = sink + p.v;
    }
    console.writeln(sink);
}
run(@N@);
```

`[heap] live-at-exit` grows ~128 B/iter on LLVM; oracle `--mem-verify` root
set is `x1` at every N. A `set`-only variant (no `detach` reassignment) is
FLAT, so the defect is specifically the field REASSIGNMENT release path, not
closure capture.

**Root-cause pointer:** LLVM lowering of a store to a closure-typed object
field — the old field value is overwritten without a release. Compare the
object-field store path (storing a class/`?` value releases the prior value
correctly) and bug #90's `Op::CallDyn` consumed-receiver release fix, both in
`src/LlvmGen.cpp`.

**Workaround (debt sites):** make the field an OPTIONAL closure and clear it to
`None` — an object-field store, which DOES release the prior value (verified
flat at N=50/800). Applied throughout the LA-HTTP-STREAM prelude teardown:
`TcpStream.onChunk`/`onClosed` and `ChunkedSink.onDone`/`userClose` are
`(... => ...)?` cleared to `None` in `close()`/`detach()`, never overwritten
with a no-op closure. NOTE: this workaround only removes the leak at sites that
adopt it; the landed base HTTP loopback path (`HttpClient` / `HttpResponseReader`
reader teardown) still churns a residual per-connection cycle under
loopback-churn and is not retired by this entry — a full net-stack cycle audit
is the real fix and is out of scope for the streaming design.

---

## #98 [P1] — metaprogramming rules/attributes declared in the prelude silently never fire

**Found:** 2026-07-19, implementing `designs/techdesign-bindgen-metaprog-scope.md`
(the DOM `@extern` bindgen's `generates body of` primitive) — running its own §13
spike 1 ("does the rule stage run over wherever the Dom surface lives").
**Priority justification:** P1.1 — ordinary, checker-accepted CALLER code (a plain
`.lev` program invoking a prelude-declared, attribute-annotated symbol) silently
gets the wrong value at exit 0, no diagnostic, because the annotating rule never
fires. Compounded by P1.2: the workaround (keep any rule/attribute/macro-driven
declaration OUT of the prelude; ship it as an ordinary project file instead) must
be independently rediscovered by every future track that wants to author prelude
surface with metaprogramming — this design walked straight into it, assuming the
ship-as-files migration (#38b59ce) would resolve it, and it doesn't.

**Repro** (prelude edit + a plain caller program):

```
$ cat >> prelude/wasm.lev <<'EOF'

namespace WasmSpike {
    attribute GenSpike { }
    rule genSpikeRule generates body of m {
        match @GenSpike on method m
        replace `return 12345;`
    }
    @GenSpike
    int __spikeProbe() => 0;
}
EOF
$ printf 'console.writeln(WasmSpike::__spikeProbe());\n' > t.ext
$ build/leviathan --target wasm32-unknown-unknown --ir t.ext
0
```

Expected (attribute/rule co-location fires on every other checked file in the
language — `tests/corpus/meta/rule_body_generate.ext` and the rest of the
`generates`/`rewrites` corpus confirm this): `12345`, the rule-generated body.
Actual: the placeholder `=> 0` body runs untouched — no error, no warning, exit 0.
(Revert the `prelude/wasm.lev` edit after reproducing; it is not a real feature.)

**Root-cause pointer:** `main.cpp` constructs exactly one `RuleEngine` and calls
`engine->run(program)` exactly once (`src/main.cpp:539`, `:557`), always on the
user's own parsed file tree. `Resolver::parsePrelude()` (`src/Resolver.cpp:153-183`)
parses the prelude into a wholly separate `Program` (`preludeProgram_`) using a
throwaway `DiagnosticSink dummy` ("the prelude is trusted; ignore its
diagnostics") and hands it to the `RuleEngine` constructor's `prelude` parameter
— which `RuleEngine` uses ONLY for `eval_.initGlobals(prelude)` (pure
comptime-value seeding, `src/Rules.cpp:16-28`). Nothing in `RuleEngine` —
`collectRules`, `indexDecls`, `runRules` — ever walks `preludeProgram_`'s items;
grep confirms no reference to `prelude` anywhere past the constructor. This
predates the ship-as-files migration (`38b59ce`, `6bf297a`) and is unaffected by
it: ship-as-files changed where the prelude's TEXT is sourced from (`prelude/*.lev`
files vs. embedded bytes), not whether its AST is ever handed to the rule engine
— confirmed by reproducing the bug against post-migration `prelude/wasm.lev`
(a real file) exactly as easily as the pre-migration embedded string.

**Workaround (debt sites):** none inside the prelude — a rule/attribute/macro
declared in any `prelude/*.lev` segment is dead code by construction. Ship the
metaprogramming-authored surface as an ordinary project `.lev` file compiled
alongside the consumer instead. This is exactly the fork
`techdesign-bindgen-metaprog-scope.md`'s DOM adoption (§5/§6) hit: its
"Recommended" placement (`dom.lev` as a prelude/stdlib segment) assumed landing
ship-as-files would make prelude rule-stage participation "the demonstrated
§4.3(e) case" — it doesn't, because §4.3(e) demonstrated rules firing on an
ordinary checked project file, and the prelude is not on the checker's or the
rule engine's path at all, file-backed or not. That design's DOM-surface rewrite
is parked on this bug (or on a project-file placement instead of prelude) rather
than worked around.

---

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
