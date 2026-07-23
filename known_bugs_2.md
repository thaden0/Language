# Known bugs — part 2 of 2 (known_bugs_2.md)

Active, unresolved bugs only. This is one half of the known-bug register;
the other half lives in `known_bugs_1.md`. The two files together hold every open
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
| P2       | agent1-101 |
| P3       | — |

Each entry's Workaround note (inline, above) carries its own debt sites — there is no
separate `docs/footguns.md` registry (retired 2026-07-19, merged into these two files).
Once the composition corpus lands, a fixed bug also gets a red-lane repro promoted to
green under `tests/corpus/composition/` (`designs/techdesign-composition-corpus.md`).
Fixing #N means: fix, delete the entry here, promote red→green — one commit.

---

#agent1-101 [P2] found 2026-07-22 (Harpoon->Sonar / Sonar->Moby tooling-rename
task, verifying the Moby TUI/DOM package's own examples still build after the
merge — build rebuilt fresh from HEAD before filing, per the rebuild-first
policy). Confirmed unrelated to the rename itself: reproduces identically
whether the package/types are named Sonar or Moby.

**Repro:** `moby/examples/editor-dom` (the Track-09/10 "flagship" DOM example)
fails to even compile on the oracle: `./main.lev:50:25: error: procedural
macro 'dom' body threw:` (message body empty) immediately followed by
`./main.lev:50:25: error: unknown function 'dom'`. The `dom!(...)` call in
question is a `<flex>` tree containing a `<menuitem id="file-menu"
hotkey="^f">Open</menuitem>` — i.e. an element using the `hotkey` attribute.

**Expected:** `dom!` expands cleanly, matching every other `dom!(...)` use in
the corpus.

**Isolation done:** the two other files in the tree that use `dom!(` —
`moby/tests/dom-drift/drift.lev` and `moby/tests/dom-bindings/bindings.lev` —
both expand fine on oracle (they fail later, on `--ir`, for the unrelated
reason in [[#105]] above). Neither uses a `hotkey=` attribute in its markup;
editor-dom's is the only `dom!` template in the tree that does, which is the
leading suspect. Removing `uses Moby::Dialogs;` (the only import editor-dom
has that the two passing files don't) does NOT clear the `dom!` error — it
only adds the expected new "unknown type FileDialog/PromptDialog" errors for
the now-unimported types — so the Dialogs import is not the trigger; scope
this to the `hotkey` attribute (`attributes.lev`'s
`__sonarBindShortcut`-injection path, now `__mobyBindShortcut`) until proven
otherwise.

**Root-cause pointer (not investigated further — procedural-macro/comptime
internals, Opus-tier, out of scope for a Sonnet-tier rename task):** the
macro body "threw" with an empty message, meaning whatever exception the
`hotkey`-attribute rule-injection raises during `dom!`'s comptime expansion is
being swallowed before it reaches the top-level diagnostic. Whoever picks this
up should start by getting the real exception text out of the procedural-
macro-body execution path (`Rules.hpp`/`RulesExpand.cpp`) rather than the
generic "unknown function" fallback.

**Priority:** P2 — blocks one example program from compiling, no workaround
needed by consumers (it doesn't block `helm` or any golden-corpus test), but
it does mean editor-dom cannot currently serve as a working flagship demo.

---

#92 fixed 2026-07-19 (found+fixed in-session, ORM Track 06): an ATTRIBUTE's
class symbol shadowed a real same-named class for ordinary bare-name
resolution — with `uses Atlantis::Orm; uses Atlantis::Data;`, a bare `Row`
resolved to the `@Row` attribute (declared first in import order) instead of
the `Atlantis::Data::Row` class, making every member read on the value
silently void on the oracle (and `User::FromRow(r)` fail overload matching).
Fixed at source in three places: `Resolver::importOne` no longer dumps
attribute-class symbols into `uses` overlay scopes at all (attribute
resolution runs through the imports map + namespace scopes, never the
overlay), and both `Resolver::resolveType` (bare + qualified branches) and
`Checker::visibleClass` prefer a non-attribute type when both are visible.
Regression floor: `packages/atlantis/tests/probes/miniorm/` (cross-package
Row construction + FromRow through the full ORM rule set, oracle green) and
the whole atlantis corpus suite (`packages/atlantis/tests/runtests.sh`).

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

---

#86, #88, #89 fixed — see git history for their resolutions.

#83 fixed 2026-07-19 (item 1 of 2): `Resolver::returnAssignable`/a new
`paramsAssignable` now compare a method's return/param types by resolved
Symbol identity (recursing through generic arguments) before falling back to
the covariant-base walk, so an alias-qualified member type (`A::Data::Foo`)
and the interface's in-package bare name (`Foo`) satisfy each other — no
`uses` + bare-spelling workaround needed anymore.

Item 2 ("`uses` behaves package-global") turned out not to be a defect on
inspection: per-file `uses` scoping is deliberate, existing design (each
source file's top-level imports overlay only that file, `Sema::fileScopeFor`
in `Symbols.hpp`), and two existing regression tests
(`tests/corpus/project/{uses_leak_err,use_leak_err}`) assert exactly this
non-leaking behavior as required — reverting it to package-global would
regress that prior fix. The documented workaround (put `uses` in every file
that needs it) is simply correct usage, not a standing defect.

#90 fixed 2026-07-19: root cause was `src/LlvmGen.cpp`'s `Op::CallDyn`
codegen for a `consumed` (COW self-append, `x = x.method(...)`) receiver —
it voided the caller's window slot without releasing it, on the assumption
the callee takes the receiver's fate. True for a hand-written native row
(e.g. "add", which explicitly frees/reuses a shared receiver itself); false
for a call to an ordinary IN-LANGUAGE function (e.g. the prelude's
`Array<T>.skip`), which retains/releases its own copy of the parameter
independently and never touches the caller's reference — leaking exactly one
reference per COW self-append call to a real function. Fixed by releasing
the receiver explicitly at both in-language call sites (direct call and
by-name dynamic dispatch) in `Op::CallDyn`; native rows are unaffected (they
already handle their own consumed contract). Verified flat at N=1/20/100 on
the original repro, `fuzz/task_churn/park_inside_callback.lev` (promoted from
XFAIL-LLVM to a plain regression floor), and a new
`tests/corpus/churn/field_cow_across_methods.ext` churn-leak floor.

#94 fixed 2026-07-20: calling a function-typed FIELD via a fused dot-call
(`h.validate()`, where `validate` is a `() => ...` FIELD, not a method)
silently no-op'd on the LLVM native backend (exit 0, no diagnostic, no side
effect) — the P0.3 dropped-operation shape from ORM Track 06's boot
`Db.validate()`. Root cause was `src/LlvmGen.cpp`'s `Op::CallDyn` by-name
path: the checker leaves a field-closure dot-call's `resolved` unset exactly
as it does a true method call (Checker's `typeOfCallInner` doesn't distinguish
the two), so lowering builds a candidate chain over every in-language method
of that name (`callmCandidates`) and compares the receiver's effective classId
against each; on no match it fell through — and the field-closure fallback
(read `sname` as a field on the receiver, and if it holds a closure dispatch
through the CallValue trampoline) was gated on `cands.empty()`. So whenever a
same-name, same-arity method happened to exist ANYWHERE else in the reachable
program (`Db.validate()`, an unrelated `Other.compute(int)`), the candidate
set was non-empty, the fallback was skipped, and the no-match fallthrough
dropped straight into the void tail: the call returned without ever reading
the field or running the closure. This is why the defect only surfaced in a
large program (some collision existed) and vanished in every minimal repro
that lacked one — NOT a receiver-liveness or async/await issue (both are
incidental; the collision is the whole defect). Fix: the field-closure
fallback now fires on ANY fallthrough where no native method covers the name,
not only when the candidate set is empty — a fallthrough already proves the
receiver is none of the candidate classes, so a field-closure on this
receiver (or a genuine unresolvable-name raise) is the only remaining
possibility. Engine-neutral (oracle/IR/cpp were already correct). Regression
floor: `tests/corpus/composition/fnvalues/green/field_closure_shadowed_by_method.lev`
(the minimal collision shape — a field-closure dot-call with an argument,
result consumed and nested in a larger expression, plus the `var f = h.field;`
workaround, alongside an unrelated same-name+arity method) — verified red→green
on the LLVM lane (blank/0 before, 42/12/100 after) and green on treewalk/ir/cpp.
The `packages/atlantis/src/orm/db.lev` "field-closure dot-call" copy-to-local
workarounds are now unnecessary but left in place (harmless).

#96 fixed 2026-07-20: the third and final instance of the borrowed
value-struct-alias family (siblings #95 and #99, both in `src/Lower.cpp`). The
original filed symptom was a SIGSEGV when a real terminal answered
`term::size()`'s CPR fallback in a compiled-LLVM `examples/helm` — a
"surfaces-away-from-the-causing-site" P0.3 heap corruption that went latent as
unrelated commits shifted heap layout, which is why the pty-matrix repro stopped
firing. The live, reliably-reproducing manifestation was the Atlantis auth corpus
(`packages/atlantis/tests/corpus/auth`): the LLVM native binary core-dumped
partway through the session-strategy tests, inside `lv_alloc_heap`
(`g_freelist[c] = *(void**)p`, reading a corrupted freelist next-pointer). A
freelist-integrity trap in `lvrt_release` proved the FIRST stale release was not
in `SessionStrategy.issue` at all (where it surfaced) but eight test-functions
earlier, in `testPbkdf2`'s `VerifyResult okRes = await verifyPassword(...)` — the
`Await`→`CopyVal` shape the entry's own root-cause pointer had named.

Root cause: `Op::Await` reads the promise's `value` field BORROWED
(`lvrt_await`'s `getfield`, `runtime/lv_runtime.c`), and the dk==1 wrap's retain
NO-OPS on a value class (`lv_is_counted` skips value structs) — so a value-struct
await result register (`VerifyResult`, a `struct` boxed by its `string?` field per
#66) holds a bare borrowed alias of the promise's stored struct, never gaining a
real count. The consuming `CopyVal` makes an independent copy into the local, but
the borrowed alias survives to frame exit; once the awaited Promise (and the
struct it owns) is freed in the same frame, `releaseAllRegs` (`src/LlvmGen.cpp`)
releases the stale alias — reads the freed block's classId (garbage), the
value-class skip fails, and the "release" decrements a freed block's freelist
next-pointer word. Corruption surfaces at the next allocation that pulls that
node. Exactly the unifying statement from the #99 phase-1 analysis: *any lowering
that leaves a borrowed value-struct alias in a register must void that register
before control can reach `releaseAllRegs`* — #95 was the method-receiver window,
#99 the for-in loop-var early return, #96 the `await` suspend/resume boundary.

Fix (`src/Lower.cpp` + `src/Lower.hpp`): a value-struct `await` result register
(`e->definiteValueStruct`) is recorded in `borrowedAwaitStructs_` (per-function,
saved/restored around nested lambda/$init lowering exactly like
`freshStructRegs_`), and every frame-exit path voids it (`Op::LoadConst … vvoid()`)
before the `Ret`/`RetVoid` reaches `releaseAllRegs` — folded into the same
`clearBorrowedElems` lambda #99 added at `StmtKind::Return` (covering the plain,
value-struct, void, and using-cleanup-chain return paths), plus the two fall-off
`RetVoid` sites (`lowerFunction` end, lambda-block end). Reference-typed await
results carry a real +1 and are deliberately NOT voided (still correctly released
at exit). Engine-neutral: a dead store on oracle/IR, which never crashed.

Regression floor:
`tests/corpus/composition/aggregates/green/await_value_struct_alias.lev` — five
`await`-into-value-struct-local sites (the testPbkdf2 shape) followed by a
same-size-class churn loop that pulls the corrupted freelist node; verified
red→green (SIGSEGV in `lv_alloc_heap` on LLVM before the fix, `churn acc=250`
after) on oracle+IR+cpp+llvm. Verified: the full Atlantis auth corpus now passes
oracle+IR+LLVM (was the only failing lane); routing (#95's pin) and the
composition/#99 floors remain green. The original CPR/helm segfault is thereby
explained and closed — it was this same corruption planted on every Helm run and
merely landing somewhere harmless once heap layout shifted; with the borrowed
`await` (and #95/#99) aliases voided, no stale value-struct alias reaches
`releaseAllRegs` on any of the three violating shapes.

#97 fixed 2026-07-21: sockets/`Process`/`Pty` classes could not be compiled for
a Windows target (`--target x86_64-pc-windows-gnu`) — a program with no task
feature in it at all (`TcpListener l = TcpListener(9099);`) failed with
`tasks: unsupported on Windows (v1) — 'sysTaskCancel' has no Windows lowering`.

Root cause (the two contributors the entry named): (1) the prelude's arity-blind
by-name over-marking pulls `TaskGroup::close`/`Channel::send`/`Channel::close`
into every build via `TcpStream`/`File`'s own `close()`/`send()` (the emission
over-approximation the reachable walk needs for `nativeMethodCovered`), and their
bodies reach the task/channel natives; (2) the Windows reject in
`src/LlvmGen.cpp`'s `CallNativeFn` lowering was **emission**-gated — it fired the
moment a marked row was emitted, reachable from user code or not.

Fix (narrowed contributor 2, the emission gate — the smaller, better-precedented
change the request doc recommended; contributor 1's over-marking was left intact,
it is load-bearing for `nativeMethodCovered`): the task, thread/channel, and
`sysSpawn` Windows rejects in `src/LlvmGen.cpp` now take the **two-tier** shape of
the wasm gate immediately above them (Track W hard-03). Tier 1 — the native sits
in a **user-reachable** function (`userReach[index]`, rooted at `@main`, NOT
`@ginit`, computed by `computeReachable`): the user genuinely wrote
`spawn`/`Channel`/`TaskGroup`/`Process`, so today's frozen compile-time diagnostic
fires unchanged (LA-30 G5 stands — win32 tasks still need the Fiber API). Tier 2 —
the native was dragged in only by the prelude over-marking and is never
user-reachable: it lowers to the `lvrt_unsupported` trap (never returns; an int-0
store keeps the IR well-formed, the `sysExit` precedent), so the build **succeeds**
and the program can only fail if it actually reaches the construct at runtime,
which by construction it cannot. `userReach` deliberately does no by-name
CallDyn fallback (its existing comment), so the over-marked task methods on
never-constructed classes are correctly tier-2.

**`sysSpawn`/`sysPidfdOpen` experiment outcome (the doc's open question):** yes,
the over-approximation drags the process floor in too — with the tasks arm
neutralized, the repro fell through to `threads: unsupported … 'sysChannelSend'`
(TcpStream's `send()` marking `Channel::send`), then to
`process spawn: unsupported … 'sysPidfdOpen'` (the `Pty` class's own reap method
genuinely calls `std::sysPidfdOpen`). Resolution split by native honesty:
`sysPidfdOpen` now simply **lowers** on Windows like `sysReap`/`sysKill` — its
win32 floor returns the frozen `-1` sentinel by ruling (`lv_plat_win32.c:252`,
D-W2) and the `Pty` prelude's poll-reap fallback already handles that, so the
reject on it was over-broad and is removed (it is user-reachable from `Pty`, so a
tier-2 trap would be wrong — the code must actually call it and get `-1`).
`sysSpawn` (pipes-spawn, no win32 floor, D4) keeps the two-tier reject, so a
genuine `Process` construction still fails at compile time naming spawn, while a
prelude-only drag-in traps. The thread/channel arm got the same two-tier gate
because `Channel` is a task feature reached by the same over-marking.

Regression coverage: `tests/run_sysnatives.sh` §12 — `sysPidfdOpen` moved from
`win_reject` to `win_lowers`; new `win_lowers` assertions that `TcpListener`,
`TcpStream`, `Pty`, and a plain non-task program all compile for the Windows
triple; new `win_reject_msg` assertions that a genuine `Process`, `spawn`,
`Channel`, and `TaskGroup` still reject with the frozen native-naming diagnostic.
Verified: the three classes emit valid COFF/PE objects for
`x86_64-pc-windows-gnu`; no POSIX regression (`corpus_tasks_llvm`,
`corpus_threads_llvm`, `corpus_sys_spawn_llvm`, `corpus_sys_pty_llvm`,
`corpus_core_llvm`/`_native`, and the treewalk/IR siblings all green — this only
touches the Windows-target code path); the LA-30 ruling and
`runtime/lv_task.c`'s `lv_tasks_enabled()==0` on win32 are untouched. (Note: two
pre-existing `emit-cpp` deferral assertions in the same script — Process/Pty
under `--build` — were already red on the pre-fix binary, unrelated to this fix;
a separate CGen coverage-ordering matter.) Unblocks Helm H10/G-H6 and
sockets-on-Windows (Trident fetch, Atlantis) broadly.

#98 fixed 2026-07-21: a rule declared in one namespace could not match an
attribute declared in a DIFFERENT namespace, even with both namespaces
imported (`uses SibA; uses SibB;` + `@Foo` from `SibA` + `ruleB` in `SibB`
matching `@Foo`) — the rule silently never fired, a spurious `attribute '@Foo'
matched no imported rule (missing 'uses SibB'?)` warning fired even though the
`uses` WAS present, and the injected member failed at runtime (`cannot resolve
call target '__fromB'`). Same-namespace matching (bug #91's now-fixed floor,
nested arbitrarily deep) worked; only cross-namespace rule→attribute matching
was broken, in either direction and at any nesting relationship.

Root cause: `RuleEngine::tryMatch` (`src/Rules.cpp`) resolved the attribute a
rule's `match @Foo` names by looking it up ONLY in the rule's own namespace
scope (`namespaceScope(r.ns)`) and the global scope. #91's fix keyed rules by
their full qualified namespace path, so a rule and attribute co-located in the
same (arbitrarily deep) namespace resolved correctly — but when the attribute
lived in a *different* namespace than the rule, that two-scope lookup returned
null, so `want` was null and the `a.resolved == want` attribute-pattern test
never matched any decl. It ignored the rule's own file imports entirely (and
the match's `attrPath` qualifier segments), even though the rule author writes
`@Foo` relying on that file's `uses` list — exactly how `resolveAttr` resolves
a `@Foo` at a use site.

Fix: new `RuleEngine::matchAttrSymbol` resolves the match attribute the same
way `resolveAttr` resolves a use-site attribute, but keyed off the file that
DECLARES the rule: a qualified `match @NS::Name` walks straight down from
global; an unqualified `match @Foo` searches the namespaces visible to the
rule's own file (its `uses` + opened namespaces + std), then falls back to the
rule's namespace and global (covers prelude / co-located rules with no file
slot). It prefers an actual `attribute` class (mirrors `resolveAttr`) so a
same-named real class cannot shadow it. `tryMatch` now calls it in place of the
two-scope lookup. The visibility invariant is UNCHANGED and un-weakened: the
existing scope guard at the top of `tryMatch` — the rule's namespace must be in
the DECL file's `effective` import set — still governs whether a rule is
eligible at all, so an un-imported rule's namespace stays correctly ineffective
(verified below); this fix only corrects WHICH attribute symbol `match @Foo`
denotes once the rule is already eligible.

Fix location: `src/Rules.cpp` (`matchAttrSymbol` added, `tryMatch` attribute-
pattern block), `src/Rules.hpp` (declaration).

Regression floor: `tests/corpus/meta/rule_cross_ns.ext` (new, green on treewalk
+ IR) — three cross-namespace shapes in one file: plain siblings, a rule nested
inside the attribute's outer namespace, and a rule outside a nested namespace
matching an attribute declared in it. Same-namespace matching floor preserved:
`packages/atlantis/tests/probes/orm_p1_nested_ns_rules.lev` (#91) and the T7-P6
probe `packages/atlantis/tests/probes/mcp_p6_two_rules_stack.lev` (whose
`ruleInner`, in `ProbeP6::Inner`, matches `@SerialProbe` from the outer
`ProbeP6` — a cross-namespace case that was silently failing and now prints
`outer,inner`). Verified: the entry's exact sibling repro prints `B`; both
nested variants and a qualified `match @SibA::Foo` cross-namespace case work; a
two-physical-file negative (rule in `SibB` in a lib file, consumer file does
`uses SibA` but NOT `uses SibB`) correctly keeps the rule inert and re-emits
the missing-`uses` warning — the visibility guard is intact. Full suites green:
`metatests`, `corpus_meta_treewalk`/`_ir`, `corpus_meta_expand_roundtrip`, the
`rule_*` reject gates, `prelude_rules`, `corpus_project`, `corpus_treewalk`/
`_ir`, `resolvertests`/`checkertests`/`evaltests`. The
`packages/atlantis/src/openapi/schema.lev` `serializableSchema` workaround (rule
forced into flat `Atlantis` instead of nested `Atlantis::OpenApi` to dodge this
bug) is now safe to revert to its intended nested placement — left in place as
optional cleanup for the Track 07 owner.

#101 fixed 2026-07-21 (full fix for the documented repro shape; see scope note
below): LA-18's specialization-set collector (`Checker::recordSpecialization`,
fed by `Checker::genericReturn` in `src/Checker.cpp`) could not determine a
concrete type tuple for a `specializationRequired` generic (one that calls a
static-shaped labeled ctor `A::Zero()` on a callable type parameter) when the
ONLY evidence for a type parameter was a lambda-literal argument's own declared
parameter type — e.g. `Probe::wrap("left", (Left a) => a)` for
`() => string wrap<A, R>(string tag, (A) => R fn)`. Loud error at the call site:
`cannot determine a concrete type tuple for generic 'wrap' at this call site`.

Root cause — TWO gaps, both in the shared front end (engine-independent):
1. `Resolver::resolveExprTypes` (`src/Resolver.cpp`) descended into a lambda's
   body/args but never resolved the lambda's OWN declared parameter types
   (`(Left a)` → the `Left` TypeRef kept a null `resolvedSymbol`). So even where
   downstream inference tried to read that type it got an opaque, unresolved
   ref (`fromTypeRef` returns `unknown()`/empty-canonical for an unresolved
   Named TypeRef).
2. `Checker::genericReturn`'s lambda-walk bound the RETURN type var (`R`, from
   the checked body) but never unified the function parameter's declared param
   types (`(A) => R`) against the lambda's own declared param types, so `A` was
   never bound. A lambda argument's VALUE type is deferred to `unknown()` during
   argument collection (`typeOfCallInner`), so the ordinary value-type unify saw
   no evidence for `A` either. Ordinary (non-`::`) generics tolerated this
   because a `specializationRequired` callable's return type here (`() => string`)
   doesn't mention `A`; LA-18's collector needs the full `A`/`R` tuple, so it
   alone failed. This is the reuse `designs/complete/techdesign-generic-static-members.md`
   §4.1 point 4 already specifies — the LA-18 M1 implementation simply omitted it
   (a gap, not a deliberate limitation; no staging conflict was found — the
   lambda's DECLARED param type is available before the body is checked, so it
   can be unified eagerly ahead of the `recordSpecialization` call).

Fix: (1) `Resolver::resolveExprTypes` now resolves each `e->params[i].type`
(harmless for non-lambda exprs — their `params` vector is empty). (2)
`Checker::genericReturn`'s lambda-walk, before building `ptypes`/checking the
body, unifies `pt->funcParams[j]` against `fromTypeRef(a->params[j].type)` for
each lambda-declared param, so `A` is bound from `(Left a)` and the tuple is
concrete when `recordSpecialization` runs.

Scope: FULL fix for the documented repro shape and the general single-hop case —
a lambda argument whose declared parameter type directly is (or contains, via
`unify`'s existing higher-kinded/generic recursion) the generic parameter. Both
documented workarounds still pass (witness-value arg; explicit `::<T1,T2>`). Not
exhaustively swept: deeply nested/higher-order shapes where a type var appears
only inside a lambda parameter that is itself a function type — those ride the
same `unify` path but have no corpus coverage yet.

Regression floor: `tests/corpus/generic_static_members/bug101_lambda_typed_specialization.ext`
(new, green on all five lanes: treewalk, IR, emit-C++, ELF, LLVM) — two distinct
element types (`Left`/`Right`) inferred purely from lambda-literal declared param
types, forcing two distinct specializations, prints `left:L` / `right:R`.
Verified: the entry's exact repro prints `left:L`; both workarounds
(witness-value, `::<Left, Left>`) still compile and run on oracle+IR; the whole
`generic_static_members` corpus + `explicit_generic_call_args` + `composition`
suites + `checkertests` (390 checks) + `corpus_treewalk`/`_ir` all green; the
Atlantis probe suite (`packages/atlantis/tests/runtests.sh`) shows no new
failures (its only red, `static (llvm)`, is the pre-existing Router/Context
LLVM-lane bug — oracle+IR pass, and this front-end fix is engine-independent).
Track 07 unblock note: this fix removes the compiler obstacle that pushed the
`@Tool` rule to its §3.5 fallback ladder rung 3 (hand-written adapters) — a
fully-generic `makeTool<A,R>(..., (A) => R fn)` call can now infer `A` from a
lambda argument's declared param type. The separate rule-template limitation
(a rule has no bound handle for a matched method's parameter TYPE to spell an
explicit `::<...>`, T7-P5) is unaffected; whoever revisits the generic approach
should confirm the tool call is written with a real lambda literal (not spliced
purely from string `$p.type` values). Design doc unchanged (the fix restores
its §4.1 point 4 intent); left in `designs/complete/` as the entry already
resides there.
#104 fixed 2026-07-20 (found+fixed in-session, refactor_1 session 02 — the
Eval.cpp/IrInterp.cpp divergence audit that drove the RuntimeCore extraction):
the oracle (`Evaluator::combine`)'s hand-maintained operator-symbol map omitted
`|` and `&`. In unchecked/prelude code (where `Expr::resolved` is null) the
oracle reached the object-operator dispatch path, but its `opSymbol` table
stopped at `<<`/`>>` and mapped `|`/`&` to `"?"` — so applying `|` or `&` to an
object whose class defines that operator method looked up a nonexistent `"?"`
method and raised the wrong error (`no operator '?' on 'X'`), while `IrInterp`'s
`objectArith` dispatched the resolved `|`/`&` method correctly. Expected: both
engines dispatch the resolved operator method, or raise `"no operator '|' on
'X'"` if the class lacks it. Same root-cause family as bug.md #13 (hand-copied
operator/method-name tables drifting between the two engines). Fixed
structurally by the RuntimeCore unification (`043c0d5`): the new shared
`rtOpSymbol` table in `src/RuntimeCore.cpp` includes `|` and `&` (matching
IrInterp and the checker's authoritative table — the correct side), and both
engines now consume that single table, so this class of drift cannot recur.
Filed for the historical record per the refactor_1 doc's finding-disposition
rule (b) — no further action needed.

#108 fixed 2026-07-21 (found+fixed in-session, top-level-parsing sweep;
renumbered from #105 on the agent0↔master merge to avoid colliding with
master's independent open `known_bugs_1.md` #105 "`expr` identifier"): a
statement-form control-flow construct written in the TOP-LEVEL (script) body —
`if`, `while`, `for`, `for-in`, `do`-while, or a labeled loop — was silently
inert. Minimal repro (`for (int i = 0; i < 3; i = i + 1) console.writeln(i.toString());`
as the whole program): on `--run` the tree-walk oracle printed nothing and ran
no later statement; on the compiled parse path (`--ir`/`--build`/`--build-native`)
the SAME source failed to parse at all with `error: expected expression` at the
`for`. Expected: the loop runs and prints `0 1 2`, exactly as it does verbatim
inside any function body, and exactly as `try`/`throw`/a bare `match` (an
expression) already ran at the top level. Two independent, hand-maintained
"what's allowed at the top level" lists had drifted out of sync with each other
AND with the IR engine (same drift family as #104/#13 — parallel per-engine
tables of statement kinds):

  1. Parser (`src/frontend/Parser.cpp`, `parseTopLevelItemInner`): the top-level
     item switch routed ONLY `throw`/`try` to `parseStatement()`; every other
     statement keyword fell through to the bare-expression path
     (`parseExpr(0)`), and `if`/`while`/`for`/`do` are not expression starters,
     so parsing died with "expected expression". (A bare `match` slipped through
     because it IS an expression — bug #84's top-level-`match` fix — which is why
     `match` worked at the top level but `for` did not.)
  2. Oracle (`src/runtime/Eval.cpp`, `execTopLevel`): the top-level body executor
     had an `exec()` whitelist of `ExprStmt | Var | Try | Throw`, so even once a
     top-level loop *did* parse it was never executed — the node was skipped and
     the loop body (plus, in the oracle's single walk, its slot mutations) simply
     vanished. The IR lowerer (`src/ir/Lower.cpp`) already lowered every
     statement kind at the top level generically, which is why `--ir` diverged
     from `--run` the instant the parser was opened up.
  3. Checker (`src/sema/Checker.cpp`, `walk` over `program.items`): a THIRD such
     list — the top-level type-check switch handled `Namespace`/`Class`/`Member`/
     `Var`/`ExprStmt` and hit `default: break;` for everything else, so it never
     descended into top-level control-flow bodies. Two consequences: (a) a type
     error nested in a top-level `for`/`while`/`try` body slipped through
     completely unchecked; (b) more seriously, a top-level LABELED loop's `break
     <label>`/`continue <label>` was left with its label never bound (label
     binding is a check-time step, `pushLoopLabel`/`bindLoopLabel`), so at runtime
     the labeled break silently fell through to the INNERMOST loop instead of its
     named target — a silent-wrong result, not a diagnostic. (`try`/`throw`, the
     only control-flow forms that could reach the top level before this fix, were
     likewise unchecked there — a latent pre-existing gap this closes.)

Fix: (1) `parseTopLevelItemInner` now also routes `if`/`while`/`for`/`do` (and a
`label: for/while/do` labeled-loop, via the same three-token lookahead
`parseStatement` itself uses) to `parseStatement()`, alongside the existing
`try`/`throw`; (2) `execTopLevel`'s whitelist gains `If | While | For | ForIn |
DoWhile`, exactly matching the statement kinds the parser now admits; (3) the
checker's top-level `walk` gains matching cases for `If`/`While`/`For`/`ForIn`/
`DoWhile`/`Try`/`Throw` that set up a fresh function-body-like context (an env
frame for body locals, no enclosing return target, clean `loopDepth_`/
`labelStack_`) and delegate to the shared `check()` — so top-level loop bodies
are type-checked AND labeled break/continue bind to the right loop, byte-identical
to the same code inside a function. All three edits are minimal and mutually
matched — no statement kind reaches the top level without both an executor and a
checker case, and vice versa. Deliberately NOT admitted: `break`/`continue`/
`return` as a *direct* top-level statement (no enclosing loop/function — they stay
rejected; nested inside a top-level loop body they are fine). The IR/emit-C++/LLVM
lowering legs needed no change (they already handled every kind), so ARC and
codegen are untouched.

Regression floor: `tests/corpus/toplevel_control_flow.lev` (+ `.expected`) —
for-in accumulating into a top-level global (proving the body runs and its
script-body slot mutation persists), C-style `for`, `while`, `if`/`else` (both
arms), `do`-while, and a labeled `break outer` out of a nested top-level loop
(whose expected output — `pair 0,0` / `pair 0,1` / `done`, byte-identical to the
same loop written inside a function — is precisely what pins component (3): before
the checker fix the top-level `break outer` wrongly printed `pair 1,0` too). It
rides the whole-corpus `corpus_treewalk` + `corpus_ir` lanes automatically (the
exact divergent pair) and is additionally pinned on emit-C++ and LLVM by the new
`corpus_toplevel_control_flow_cpp` / `corpus_toplevel_control_flow_llvm`
single-file lanes — byte-identical output on all four active engines. Verified:
the four new/covering lanes green; `corpus_treewalk`/`_ir`/`_meta_*`,
`parsertests`/`resolvertests`/`checkertests`/`evaltests`/`metatests`, `layering`,
and `clone-ratchet` all still green (no pre-existing top-level program regressed —
the change only ADDS reachable statement kinds); a negative probe (`int y =
"s";` nested in a top-level `for`) now correctly reports `cannot initialize`.
`docs/reference.md` §4.3 top-level-statements paragraph updated to state that the
script body admits the full statement grammar except `break`/`continue`/`return`.

#106 [P2.2] fixed 2026-07-21 (quadratic `--mem-verify` sweep made the
http_streaming churn gate time out and misread as a leak): ctest
`http_streaming_integration` failed with `FAIL churn root set scales with N
(N=40: 18 sites, N=400: 0 sites)` while every corpus sub-case printed ok. The
"0 sites" was NOT a real root-set collapse (nor a leak, nor P0.3 corruption):
the N=400 `--run --mem-verify` sweep blew the script's internal `timeout 120`,
producing no `[mem]` report at all, and `grep -c` then counted the empty
output as 0 — a false leak verdict fabricated by the harness's failure mode.
Controlled unloaded reruns confirmed both halves: whenever the run completes,
the root set is exactly 18 lines at N=40 AND N=400 (the invariant holds; no
teardown leak), and the N=400 run took 58-189s on an IDLE machine — the
timeout was not merely parallel-ctest load flake.

Root cause (the real defect, engine-tooling side): `MemVerifier::sweep()`
(`src/ir/MemVerify.hpp`) is called after EVERY interpreted op
(`src/backend/IrInterp.cpp` memOn_ block) and iterated the ENTIRE `objs` map —
every object ever allocated, dead entries included (they stay for the final
report and were only skipped by a `continue`). Total verifier cost was
O(ops x totalAllocs); in a churn loop both factors are linear in N, so the
sweep was quadratic: measured tree-walk times 0.47s / 6.0s / 30.4s / 189s at
N = 40 / 100 / 200 / 400 (16,819 allocations at N=400, peak live only 51).
Output was correct on every engine whenever the run finished, so this is
P2.2 (performance/resource-only, asymptotic complexity wrong) — explicitly
NOT P0.3: no reference or resource was dropped; the roots were always there,
the verifier was just too slow to report them inside the harness budget.

Fix (two sites):
1. `src/ir/MemVerify.hpp` — added a live index (`std::vector<const void*>
   live_`): `onAlloc` pushes the key, `sweep()` walks ONLY the live index
   (swap-and-pop on expiry) instead of all of `objs`. Per-op cost drops to
   O(liveNow) (bounded, 51 here), total verifier cost linear in the op count.
   Dead entries still stay in `objs`, so `report()` is byte-identical
   (diff-verified at N=40 and N=400 pre/post fix).
2. `tests/run_http_streaming.sh` — `rootset_at` can no longer launder a dead
   run into a count: the run writes to a file, a nonzero exit (timeout/crash)
   returns the sentinel `RUNFAIL`, and the gate reports `FAIL churn
   --mem-verify run died or timed out (...)` distinctly from a genuine
   root-set scaling failure.

Regression floor: the churn gate's per-run timeout is tightened 120s -> 60s
(matching `fuzz/churn_leak.py`'s `TIMEOUT_S = 60` convention). Post-fix the
sweep runs in 0.34s / 0.54s / 0.76s / 1.34s at N = 40 / 100 / 200 / 400
(~45x headroom at N=400); the old quadratic sweep needed 58s+ at N=400 even
idle, so a complexity regression trips the floor and now reports as the
distinct RUNFAIL message, not a phantom leak.

Verified: `bash tests/run_http_streaming.sh build/leviathan .` fully green
(all 10 corpus programs on oracle/IR/LLVM + `ok churn root set constant (18
sites at N=40 and N=400)`); root-set count 18 = 18 across N with the fixed
binary; `run_memverify.sh` lane (`corpus_mem_verify`) green — the only other
consumer of the sweep — plus `corpus_treewalk`/`_ir` unaffected (memOn_ off).
Side observation while reproducing: a SIBLING checkout running the same lane
concurrently (another agent's worktree) holds the same loopback ports
(18110 ...), which ctest's RESOURCE_LOCK cannot see across repos — that
produced one spurious 120s hang at N=40 here. The new RUNFAIL message makes
that failure mode diagnosable too, instead of reading as "0 sites".
