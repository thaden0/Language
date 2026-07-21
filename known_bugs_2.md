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
| P0       | #94, #96 |
| P1       | #97, #98, #101 |
| P2       | #102 |
| P3       | — |

Each entry's Workaround note (inline, above) carries its own debt sites — there is no
separate `docs/footguns.md` registry (retired 2026-07-19, merged into these two files).
Once the composition corpus lands, a fixed bug also gets a red-lane repro promoted to
green under `tests/corpus/composition/` (`designs/techdesign-composition-corpus.md`).
Fixing #N means: fix, delete the entry here, promote red→green — one commit.

---

## #94 [P0] — calling a function-typed FIELD via dot-call silently no-ops on LLVM

**Found:** 2026-07-19, ORM Track 06 M1 (boot validation never ran on LLVM
while printing success). Related family: the 2026-07-11 "field-closure
dot-call" finding and #53's this-receiver lambda rule — this entry is the
checked-code, LLVM-specific shape with a live repro.
**Priority justification:** P0.3 — an actively-maintained engine silently
drops an operation for checker-accepted ordinary code; the symptom (missing
side effects) surfaces away from the causing site. Oracle and IR run the call
correctly; LLVM returns without executing the closure body.

**Repro shape (from the ORM):**

```
class RepoHandle {
    () => Promise<void> validate;      // field holding a closure
    ...
}
await h.validate();                    // oracle/IR: runs the closure; LLVM: silent no-op
```

Observed concretely: `Db.validate()` printed its success line on LLVM while
the closure's `DESCRIBE` never executed (the corpus caught it as a missing
`[sql]` log line). Copying to a local first is reliable on all engines:

```
var f = h.validate;
await f();
```

**Root-cause pointer:** LLVM lowering of a dynamic call whose callee is a
field read (member access → call in one expression); the local-copy form
takes the ordinary closure-value call path.

**Workaround (debt sites):** copy the field to a local, then call — applied
throughout `packages/atlantis/src/orm/db.lev` (search "field-closure
dot-call"); `packages/atlantis-mysql/src/pool.lev` (`var f = fn;`) already
used the same idiom.

---

## #96 [P0] — a real terminal answering `term::size()`'s CPR fallback segfaults a compiled-LLVM app

**Found:** 2026-07-19, first-ever live (`App.run()`) exercise of `examples/helm`
compiled with `trident build --out` (H10/H12 landed; this is the first Sonar-based
app in the repo to be run interactively as a native binary in a real terminal —
every prior golden test drives the event loop headlessly via `TestRenderer`/
`pumpOnce`, never a live pty). Not Helm- or Sonar-library-specific: the crash is
inside the language runtime's own `term::size()` cursor-position-report (CPR)
fallback (`runtime/lv_loop.c` / `runtime/lv_plat.h`, prelude decl in
`src/Resolver.cpp`), so any compiled app on any actively-maintained engine that
reaches this path in a real terminal is exposed.
**Priority justification:** P0.3 — an actively-maintained engine (LLVM, the
primary backend) segfaults for ordinary, checker-accepted code; the fault
manifests several closure-call frames downstream of the triggering input (the
CPR response bytes), the classic "surfaces away from the causing site" shape.

**Repro** (needs a real pty — a plain pipe/redirect never reaches the crash,
see Verification below):

```
cd examples/helm
build/trident build . --out /tmp/helm-bin
python3 - <<'PYEOF'
import pty, os, time, select, signal
pid, fd = pty.fork()
if pid == 0:
    os.environ["HELM_RUN"] = "1"
    os.execvp("/tmp/helm-bin", ["/tmp/helm-bin", "."])
else:
    out = b""; responded = False
    for _ in range(20):
        r, _, _ = select.select([fd], [], [], 0.2)
        if fd in r:
            chunk = os.read(fd, 65536)
            if not chunk: break
            out += chunk
            if not responded and b"\x1b[6n" in out:
                os.write(fd, b"\x1b[24;80R")   # a real terminal's CPR answer
                responded = True
    os.kill(pid, signal.SIGKILL)
    _, status = os.waitpid(pid, 0)
    print("signaled:", os.WIFSIGNALED(status), os.WTERMSIG(status) if os.WIFSIGNALED(status) else None)
PYEOF
# -> signaled: True 11   (SIGSEGV)
```

Also reproduces directly from an interactive shell: `HELM_RUN=1 /tmp/helm-bin .`
run from a real terminal segfaults immediately after startup
("Helm 0.1.0-dev …" then "Segmentation fault (core dumped)").

**Expected:** the app prints its startup line, enters raw mode, queries the
terminal size via `\x1b[999C\x1b[999B\x1b[6n` (move-to-bottom-right + CPR) when
`TIOCGWINSZ` isn't trusted, receives the `\x1b[<row>;<col>R` reply, and proceeds
into the frame loop.
**Actual:** SIGSEGV shortly after the CPR reply is read, inside the input
callback chain (`lv_task_trampoline` → `lv_run_closure_thunk` → four opaque
LLVM-generated frames with no symbol names in this build). Backtrace has no
line-level info — this build isn't compiled with debug symbols, and the
generated function names (`f1287`, `f2198`, …) don't map back to `.lev` source
without a symbol table, so the root cause is unisolated beyond "somewhere in
the CPR-response parse/dispatch path."

**2026-07-20 update — does NOT currently reproduce; treat as LATENT, not fixed.**
`examples/helm` rebuilt at `b397656` (which includes #95's fix, `ffa9e6e`) and
driven through a 24-case pty matrix — the repro script above verbatim, plus
split/delayed/oversized/garbage/absent CPR replies, focus and bracketed-paste
bytes around the reply, keystrokes, and SIGWINCH storms that re-enter
`onWinch()` → `r.size()` — completes and renders correctly every run, with the
CPR fallback firing 3-4× per run and parsing. Identical results before and after
the #95 merge, so `ffa9e6e` is not what changed it.

**Kept open at P0 anyway**, because non-reproduction is weak evidence for this
defect class: see the root-cause pointer below. Whoever lands #99's fix should
re-run the matrix — that is the cheap check that decides whether this entry
dies or was merely masked.

**Root-cause pointer (revised 2026-07-20, supersedes "somewhere in the
CPR-response parse/dispatch path"):** most likely a member of the LLVM
**borrowed value-struct alias** family, not anything CPR-specific. The unifying
statement (Agent2, #99 phase 1): *any lowering that leaves a borrowed
value-struct alias in a register must void that register before control can
reach `releaseAllRegs`* — currently violated on (a) loop early-return edges and
(b) the `Await`→`CopyVal` sequence. #95 (`ffa9e6e`) was the value-struct method
receiver instance of the same statement; #99 (`known_bugs_1.md`) is the open one.
The shapes line up: `Sonar::Size` is a value struct (`sonar/src/geometry.lev:23`),
`App.onWinch()` → `r.size()` runs inside the await-driven callback chain, and
this entry's own backtrace (`lv_task_trampoline` → `lv_run_closure_thunk` → opaque
frames, faulting away from the causing site) is the crash-later signature all
three share. Mechanism, if so (#95's, proven): `releaseAllRegs()`
(`src/LlvmGen.cpp`, run at every `Op::Ret`/`RetVoid`/unwind) releases the stale
alias; the release reads the freed block's classId as garbage, `lv_is_counted`'s
value-class skip fails, and the decrement lands on a **freelist next-pointer
word**, surfacing later inside `lv_alloc_heap`. Note `ffa9e6e` (#95's fix) does
NOT close #99 — the auth corpus still crashes with it in place — so the matrix
above being unchanged across that merge is expected and settles nothing.

**Decisive check, NOT yet run:** if this entry is that family, the corruption is
still occurring on every Helm run and merely landing somewhere harmless — so a
freelist-integrity trap (`bug99.md`'s step 1: walk every `g_freelist[c]` at the
top of `lv_alloc_heap`/`lv_free_raw`, trap on the first node that is out-of-region
or not 16-byte-aligned), or an `LANG_RT_SANITIZE` lvrt linked into the Helm
binary, would fire on runs the matrix above scored "alive". That turns this
entry's status from "cannot reproduce" into a real yes/no. Corruption of this
shape appears and disappears as unrelated code
shifts heap layout — `a302c23` ("Convert Pair to a value struct") landed between
this entry being filed and the matrix above being run, and does exactly that —
which is why "it stopped crashing" must not be read as "it was fixed."

**Verification gap CLOSED (was gap 1):** "oracle/IR produced zero observable
output at all" was NOT stdio buffering masking a crash — it was a DEADLOCK, and
the buffering was its cause. `sysWrite(1, …)` on the interpreters appends to the
engine's capture buffer (`Evaluator::out_`, printed once the program ends)
instead of the descriptor, so `term::size()`'s probe bytes never reached the tty,
nothing answered, and `readCursorReport()`'s `sysRead(0, 16)` blocked forever
under raw mode's `VMIN=1`. LLVM writes fd 1 directly and was unaffected — the
engines diverged because one of them captures. Fixed 2026-07-20: raw mode now
unbuffers the capture (`interpEmitStdout()`, `src/RuntimeNatives.cpp`, declared
in `src/RuntimeValue.hpp`), routed through by `sysWrite`,
`Evaluator::emitConsole`, and `IrInterp`'s `Op::Print`/`PrintNl` so a raw-mode
program's console and `sysWrite` output keep their relative order. Non-tty runs
never enter raw mode, so every `.expected` golden is untouched.

**Regression floor (new, 2026-07-20):** `tests/corpus/floor/winsize_cpr.lev` +
the `cpr` mode of `tests/floor_pty.py`, wired into `run_terminal_floor.sh` on
oracle/IR/LLVM. The CPR probe had NO automated coverage at all before this —
every other winsize lane either has a real `TIOCGWINSZ` answer (no fallback) or
no tty (fallback guarded off by `isRaw()`), which is why it was hand-run only
and why this bug went unseen for a full release of the floor.

**Remaining gap:** (2) still open — no workaround, if it returns.

**Separate finding, NOT filed as its own entry pending a ruling:** a CPR reply of
≥999 rows (`\x1b[999;999R` and up) kills a compiled Sonar app with
`lvrt: heap exhausted`, exit 1 — loud, not corruption. Reproduces identically on
every tree tested. Left alone deliberately: the probe is `\x1b[999C\x1b[999B`, so
a well-behaved terminal can never answer larger than 999, and clamping inside
`term::size()` would mean reporting a size the terminal did not give. Owner ruling
wanted on whether the floor should clamp, the heap should grow, or neither.

---

## #97 [P1] — sockets/process/pty classes cannot be compiled for a Windows target: prelude over-marking drags the task natives in

**Found:** 2026-07-19, implementing the pty floor's Windows lane
(`designs/complete/techdesign-03-pty-windows-conpty.md` S3/G-PTY3).
**Priority justification:** P1.2 — the only workaround is per-use: every track
that wants sockets, a child process, or a pty on a Windows target must
independently discover this and hand-roll the floor natives at each site; no
single workaround retires it. (P2.3 also matches — a documented feature fails
loud on one lane while working on the others — but P1 is evaluated first and
P1.2 fits the workaround shape exactly.)

**NOT the bug:** that `spawn`/`Channel`/`TaskGroup` are unsupported on a
Windows target. That is a deliberate ruling (LA-30 G5: win32 needs the Fiber
API, so tasks stay pump-pinned — `runtime/lv_task.c:27-57`), documented in
`docs/reference.md`. The bug is its **blast radius**.

**Repro** — no task feature anywhere in the program:

```
$ printf 'TcpListener l = TcpListener(9099);\n' > t.lev
$ leviathan --native-obj t.o --target x86_64-pc-windows-gnu t.lev
error: LLVM backend: tasks: unsupported on Windows (v1) — 'sysTaskCancel'
       has no Windows lowering
```

Identical failure for `TcpStream`, `Process`, and `Pty`. Expected: these
compile for a Windows triple (nothing in any design rejects them, and
`docs/reference.md` documents sockets/`Process`/`Pty` without a Windows
carve-out — only threads/`spawn` carry one). Actual: a whole capability family
is unbuildable for the target, and the diagnostic names a native the program
never mentions, so the message mis-attributes the cause.

**Root-cause pointer:** two independent contributors; narrowing **either** one
fixes it.
1. Prelude over-marking (`src/Resolver.cpp:3232` documents this exact
   mechanism breaking `--build` once before): marking is arity-blind and
   by-name, so `TcpStream`/`File`'s `close()` also marks `TaskGroup::close()`,
   whose body reaches `std::sysTaskCancel` (`src/Resolver.cpp:1414`).
2. The reject is **emission**-gated, not reachability-gated
   (`src/LlvmGen.cpp:2752-2765`): it fires when the row is emitted, reachable
   or not. The wasm gate immediately above it already has the two-tier shape
   (reachable → diagnostic; prelude-only → `lvrt_unsupported` trap) that would
   answer this.

**Owner ruling needed before a fix lands** (which of the two to narrow is a
gate question, not a pty question): recorded as
`designs/requests/request-windows-task-gate.md`.

**Workaround (debt sites):** drive the **floor natives** directly instead of
the prelude class — `std::sysPtySpawn`/`sysRecv`/`sysSend`/`sysPtyResize`/
`sysKill`/`sysReap` compile and run on Windows today. Applied in
`tests/pty_win_driver.lev` (the G-PTY3 behavioral lane, which is why it does
not use `Pty`). `tests/run_wine_cross.sh` avoids the area entirely — its scope
note excludes net corpus.

## #98 [P1] — a rule cannot match an attribute declared in a different namespace than the rule itself

**Renumbered from #96** (2026-07-19, same day) to resolve a cross-branch bug-
number collision on merge — origin/master independently filed a DIFFERENT #96
(the `term::size()` CPR segfault above) and #97 (the Windows task-gate entry
above) the same day. This entry's content is otherwise unchanged; any prior
reference to "known_bugs_2.md #96" for the cross-namespace rule-matching
finding now means #98.

**Independently re-confirmed** 2026-07-20 by a second, concurrent agent working the same
Atlantis Track 07 doc from a stale worktree copy (its own T7-P6 probe, minimal
sibling-namespace repro, identical result); folded in here rather than kept as a
duplicate entry — for THIS defect, one entry is correct. That session also isolated a
separate, independently-reproducible LA-18 defect (the specialization-tuple collector
can't infer a generic's type when the only evidence in the call is a lambda-literal
argument's declared parameter type), which it filed as #99. That finding is NOT a
duplicate of this entry — it reproduces with no rules or attributes involved at all and
points at a different subsystem — and the original merge resolution's decision to fold
it into this paragraph misfiled a distinct compiler bug. Restored as its own entry,
**#101** below (renumbered from #99, which origin/master had already assigned).

**Found:** 2026-07-19, Atlantis Track 07 M0 probe T7-P6
(`packages/atlantis/tests/probes/mcp_p6_two_rules_stack.lev`), while
validating the C1 2026-07-18 amendment's premise (rules/attributes live in
the subsystem namespace that *consumes* them, e.g. Track 07's `schemaJson`
rule in `Atlantis::OpenApi` matching `@Serializable`, declared in `Atlantis`).
**Priority justification:** P1.2 — no single fix retires the risk; every
future subsystem whose rule needs to match another subsystem's attribute
(exactly the shape C1's own "multi-consumer" tiebreak anticipates as normal)
must independently know to co-locate the rule in the attribute's namespace
instead. Not P0: a workaround exists (same-namespace co-location) and no
track is unconditionally blocked.

**Repro (minimal, no nesting relationship — plain siblings):**

```
namespace SibA {
    attribute Foo {}
}
namespace SibB {
    rule ruleB {
        match @Foo on struct S
        inject `string __fromB() => "B";` at member of S
    }
}
uses SibA;
uses SibB;

@Foo
struct Thing { int x; }

void main() {
    Thing t = Thing();
    console.writeln(t.__fromB());   // expected "B"
}
main();
```

**Expected:** `ruleB` matches `@Foo` (visible, imported via `uses SibA;`) and
injects `__fromB` into `Thing`, printing `B`.
**Actual:** a diagnostic (`warning: attribute '@Foo' matched no imported rule
(missing 'uses SibB'?)`) fires even though `uses SibB;` IS present, `ruleB`
never fires, and the call `t.__fromB()` fails at runtime: `Uncaught
RuntimeException: cannot resolve call target '__fromB'`. Confirmed the same
result whether the two namespaces are unrelated siblings (above), or one is
nested one level inside the other in either direction (attribute in the
outer namespace + rule in a nested inner namespace, matching Track 07's
actual C1-amended layout) — the failure is not about nesting depth or
direction, only about the rule and the attribute being declared in different
namespace blocks at all. When both are declared in the SAME namespace block
(regardless of how deeply that namespace is itself nested — e.g. Track 06's
`Atlantis::Orm` rules matching `Atlantis::Orm`'s own attributes, bug #91's
now-fixed regression floor), matching works correctly — confirmed by this
same probe file's `ruleOuter`/`__fromOuter` case, which passes.

**Root-cause pointer:** not investigated (framework agents don't debug
compiler internals per the Atlantis overview §0.4(b)/(h)); likely the same
family as #91 (rule/attribute namespace keying in `Rules.cpp`), but #91's fix
(keying by full qualified path) evidently did not extend to cross-namespace
*matching* — only to same-namespace rules/attributes nested arbitrarily deep.

**Workaround (debt site):** co-locate a rule with the attribute it matches,
in the attribute's OWN namespace, even when C1's placement guidance would
otherwise put the rule in a different (more specific) subsystem namespace.
Applied in `packages/atlantis/src/openapi/schema.lev` (Track 07): the
`serializableSchema` rule matches `@Serializable`, declared in flat
`Atlantis` by Track 03's (still-unmigrated) `src/json/serializable.lev` — so
it is declared inside `namespace Atlantis { ... }` directly, not nested
`Atlantis::OpenApi`, with a comment pointing here. Revisit when either this
bug is fixed, or Track 03 completes its own C1 migration (at which point the
rule should move to `Atlantis::Json` instead, once cross-namespace matching
works or `@Serializable` lands there).

## #101 [P1] — LA-18 specialization can't determine a type tuple from a lambda-typed argument alone

**Renumbered from #99** (2026-07-20, merge audit): filed as #99 in the agent0
session before its merge with origin/master, which had independently assigned
#99 (`known_bugs_1.md`, the `Array<Struct>`-loop corruption P0) and #100
(already fixed — see commits prefixed `bug.md #100`). The original merge
resolution folded this entry into #98 as a note; it is a distinct defect
(reproduces with no rules/attributes involved; different root-cause site), so
it is restored here under the next free number.

**Found:** 2026-07-20, implementing Atlantis Track 07 (`designs/atlantis/techdesign-07-mcp-openapi.md`,
probe T7-P1) — the `makeTool<A,R>(string, string, Array<ParamSpec>, (A) => R fn)` adapter shape.
**Priority justification:** P1.2 — the only workaround is per-use (a plain witness value of type `A`
in the call, or explicit `fn::<A, R>(...)` type arguments); every future LA-18 consumer that infers a
`specializationRequired` generic's type purely from a lambda-literal argument's declared parameter
type must independently discover one of those two workarounds. Not P0: it is a loud compile error
(`cannot determine a concrete type tuple`), never silent, and a per-call fix exists.

**NOT the bug:** `A::member` itself. Calling a static-shaped labeled ctor on a function-level generic
type parameter works exactly as `designs/complete/techdesign-generic-static-members.md` (LA-18)
describes — confirmed directly (`packages/atlantis/tests/probes/mcp_p1_generic_labeled_ctor_type_param.lev`
compiles and runs once the `A::Zero()` call is removed, and a companion witness-argument repro below
compiles and runs with it present). The bug is narrower: **which call shapes let the specialization
pass *see* the concrete type**.

**Repro:**

```
namespace Probe {
    () => string wrap<A, R>(string tag, (A) => R fn) {
        string eager = A::Zero().tag();      // <- specializationRequired because of this
        return () => tag + ":" + eager;
    }
}
uses Probe;
class Left { new Zero() { } string tag() => "L"; }
var l = Probe::wrap("left", (Left a) => a);   // ERROR
console.writeln(l());
```

```
error: cannot determine a concrete type tuple for generic 'wrap' at this call site
```

Deleting the `A::Zero()` line (making `wrap` an ordinary, non-`::`-using generic) makes the identical
call compile and run — plain generic checking infers `A`/`R` from the lambda argument's declared
type just fine (`inferConstruction`/`genericReturn`, the same machinery LA-18 §4.1 point 4 says its
own tuple collector reuses). Only the **specialization-set collection** step added by LA-18 fails to
pick up the tuple here.

**Confirmed workarounds (both compile and run correctly):**
1. A plain witness *value* argument of type `A` alongside the lambda:
   `A witness<A>(A w) => A::Zero().tag(); apply6(Left())` — succeeds (no lambda involved at all).
2. Explicit generic type arguments at the call site, `::<...>` syntax:
   `Probe::apply3::<Left, Left>("hi", (Left a) => a)` — succeeds; note the call-site spelling is
   `name::<T1,T2>(...)`, not `name<T1,T2>(...)` (the latter parses as a chained comparison and a
   separate, pre-existing diagnostic — "cannot reference generic function ... in value position" —
   points at the `::<...>` spelling).

**Root-cause pointer:** not yet narrowed inside `Checker.cpp`'s specialization-set collector (the
LA-18 M1 code path, `designs/complete/techdesign-generic-static-members.md` §4.1 point 4); the
collector evidently keys off argument-VALUE type inference and does not additionally consult a
lambda-LITERAL argument's own declared parameter type the way ordinary (non-specialization) generic
checking does.

**Workaround (debt sites):** Atlantis Track 07's `@Tool` rule cannot emit a fully-generic,
lambda-argument-inferred `makeTool<A,R>(...)` call from a rule template (the rule has no bound
declaration for a matched method's parameter TYPE to spell an explicit `::<AddArgs, int>` — only
`$p.name`/`$p.type` as string VALUES, confirmed separately, see T7-P5). Track 07 implements its
design's own pre-authorized §3.5 fallback ladder rung 3 instead: the rule generates tool metadata
(name/description/param descriptors) only; typed dispatch adapters are hand-written in the app's
composition root, where the concrete DTO type is already spelled by the author. Debt site:
`packages/atlantis/src/mcp/**` once implemented.

---

## #102 [P2] — emit-C++ (CGen `--build`) spawn/pty coverage deferral no longer names the specific `sys::` native

**Found:** 2026-07-20, refactor_1 session 01 (build-scaffold validation) — the
`sys_natives` ctest case (`tests/run_sysnatives.sh` steps 10/11) went red while
the underlying compiler behavior (correctly refusing to lower `Process`/`Pty` on
the native C++ backend) was unchanged.
**Priority justification:** P2.4-adjacent — the construct is correctly unsupported
and still errors *loudly* (nonzero exit), so this is not a missing-diagnostic
(true P2.4) nor a silent-wrong-value defect (P1.1) nor a crash (P0). What
regressed is the diagnostic's *specificity*: CGen's documented "clean coverage
deferral" is supposed to name the exact missing `sys::` native (e.g. `sysSpawn`),
and a shipped test (`run_sysnatives.sh`) asserts that wording. The happy path is
correct and no supported construct misbehaves; the only symptom is a
diagnostic-quality regression surfacing as `sys_natives` FAILED in `ctest`.

**Repro:**

```
Process p = Process("/bin/echo", ["x"]);
p.exitCode().then((c) => console.writeln(c.toString()));
```

```
./build/leviathan --build /tmp/out /tmp/sp.lev
```

(same shape for a `Pty::Deterministic(...)` variant — see `tests/run_sysnatives.sh`
lines 344-414, which drive steps 10/11.)

**Expected:** nonzero exit with a diagnostic matching `native.*'sys` — CGen's
per-native coverage deferral naming the specific missing native (e.g. `sysSpawn`
for `Process`, the pty native for `Pty`).

**Actual:** nonzero exit, but the message is the generic catch-all:
`error: native backend does not yet cover this construct (objects/collections/closures/exceptions)`.

**Root-cause pointer (unconfirmed):** CGen appears to bail on the generic
"objects/collections/closures" unsupported-construct check *before* control
reaches the `sys`-native-specific coverage diagnostic for `Process`/`Pty`, so the
specific-native branch never fires. Not yet narrowed to a source line; filing
only per bug-reporting workflow.

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
