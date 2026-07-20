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
| P0       | #95, #97 |
| P1       | #93 |
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

## #93 [P1] — punctuation-only string literals inside inject templates are corrupted

**Found:** 2026-07-19, implementing ORM Track 06 (M1).
**Priority justification:** P1.1 — the corrupted expression compiles and runs,
silently producing a wrong value (no diagnostic); observed on the oracle, so
every engine inherits it.

**Repro:** a rule template containing a string literal that is only punctuation
or starts with `@`:

```
rule r {
    match @Table(t) on class C
    inject `string ctx() => $t.name + "." + $f.name;` at member of C
}
```

Under `--ast-after-rules` the injected body reads `(("users" + .) + "id")` —
the `"."` literal degraded to a bare `.` token; likewise `"@Row " + $f.name`
degraded to `(@Row  + "userId")`. The program still compiles and the concat
yields a wrong string (the corrupted operand contributes garbage/empty), so
any code building messages from template literals is silently wrong.

**Root-cause pointer:** the quasiquote template clone path (`Rules.cpp`
cloneExpr / template re-lex) appears to lose the string-literal kind for
tokens that would re-lex as punctuation/attribute markers.

**Workaround (debt sites):** move the literal into an ordinary helper function
and call it from the template — the ORM does this with
`Atlantis::Orm::ctx(table, col)` / `ctxRow(col)`
(`packages/atlantis/src/orm/orm.lev`); templates never carry punctuation-only
or `@`-leading string literals.

## #95 [P0] — atlantis routing corpus segfaults on LLVM (pre-existing at 2026-07-19 master)

**Found:** 2026-07-19, running the new `packages/atlantis/tests/runtests.sh`
across all corpus dirs during ORM Track 06 verification.
**Priority justification:** P0.3 — an actively-maintained engine (LLVM, the
primary backend) crashes mid-run on checker-accepted, previously-landed code
(the crash-later variant counts).

**Repro:**

```
./build/trident plan packages/atlantis/tests/corpus/routing --plan /tmp/r.lvplan --leviathan ./build/leviathan
./build/leviathan --build-native /tmp/r.bin --plan /tmp/r.lvplan
/tmp/r.bin        # prints 2 lines ("== M3 Era-A end-to-end ..." + "-- GET / --"), then dumps core
```

Oracle and IR runs of the same plan produce the full 40+-line expected output
(`routing.expected`). Verified present with ALL of this session's compiler
changes stashed (clean master `src/`), so it predates Track 06's work — the
routing corpus landed green on LLVM 2026-07-13 (Track 02), meaning something
since regressed it. Not diagnosed further here (out of Track 06 scope).

## #97 [P0] — a function taking a class-typed parameter that also returns from inside a `for` loop over `Array<Struct>` corrupts a LATER unrelated call on LLVM

**Found:** 2026-07-19, implementing Atlantis Track 08 (Auth & Security),
exercising the session-cookie strategy's `authenticate()` path on the native
binary.
**Priority justification:** P0.3 — an actively-maintained engine (LLVM) hits
silent state corruption whose symptom (a crash) surfaces at a later,
unrelated call site, not at the faulting expression itself — the classic
crash-later variant explicitly named in P0.3's definition.

**Repro (minimal, bisected down from Track 08's `Auth::cookieValue`):**

```
uses Atlantis::Http;      // brings in Context/HttpRequest

struct Pair2 { string name; string value;
    new Pair2(string name, string value) { this.name = name; this.value = value; } }

Array<Pair2> buildPairs(string s) {
    Array<Pair2> out = [];
    for (string part in s.split(";")) {
        int eq = part.indexOf("=");
        if (eq < 0) { continue; }
        out = out.add(Pair2(part.subStr(0, eq), part.subStr(eq + 1, part.length() - (eq + 1))));
    }
    return out;
}

// The bug needs BOTH: (a) a class-typed parameter (here Context — UNUSED in
// the body below, its mere presence in the signature is enough) and (b) an
// early `return` from inside a `for` loop over Array<Struct>.
string? myCookieValue(Context ctx, string name) {
    string header = "a=v1.r3GNDygAj3fpO3Bg1qXc-P7VcVEmOzHww38cKvLRGqc.1784511493991.Zg6jEz2MeRy95zjB7lzztmuETwvMmXh2ggW57RC6bVM";
    Array<Pair2> pairs = buildPairs(header);
    for (Pair2 p in pairs) {
        if (p.name == name) { return p.value; }     // <- early return, the second ingredient
    }
    return None;
}

HttpRequest mkReq() { HttpRequest r = HttpRequest(); r.parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"); return r; }

void main() {
    Context ctx = Context(mkReq());
    console.writeln(myCookieValue(ctx, "a") ?? "none");
    console.writeln("about to hash");
    string sig = digest::hmacSha256("key", "msg");   // <- ANY later heap-touching call
    console.writeln("sig len=" + sig.length().toString());   // never printed — core dump
}
```

Oracle and IR print all three lines correctly; the native binary prints
`"about to hash"` then dumps core inside the *unrelated* `hmacSha256` call —
the `Context ctx` parameter is never read in `myCookieValue`'s body, only
declared. Bisected by elimination while debugging Track 08's real
`cookieValue(Http::Context ctx, string name)` (identical shape: parses a
`Cookie:` header via `Http::reqHeader`, then `for (CookiePair p in pairs) {
if (p.name == name) { return p.value; } }`): removing the `Context`
parameter (switching to a plain `string` parameter) makes the crash
disappear with the early `return` left in place; separately, replacing the
early `return` with an accumulator variable (`string? found = None; ... found
= p.value; ... return found;`) makes it disappear with the `Context`
parameter left in place — so BOTH ingredients are individually necessary,
neither alone suffices, and merely binding the loop's iterable to a local
`Array` first (a workaround that DOES fix a superficially similar-looking
case, e.g. router.lev's "bind the intermediate to a local first" note) does
**not** fix this one. An existing landed pattern combining a `for` loop over
`Array<Struct>` (`Router.finalize`'s rebuild loop,
`packages/atlantis/src/routing/router.lev`) with NO early return and NO
class-typed parameter in the same function is unaffected, consistent with
both ingredients being required together. Not diagnosed further here (out of
Track 08 scope) — plausibly the same family as #90 (a loop-body early-exit
failing to release/finalize a live reference before the function returns,
here the class-typed parameter's own reference, corrupting the allocator for
the next heap-touching call), but unconfirmed.

**Workaround (debt sites):** in any function that also takes a class-typed
parameter (`Context`, `Router`, a strategy/store object, ...), replace an
early `return` from inside a `for` loop over `Array<Struct>` with an
accumulator variable, returned once after the loop. Applied in
`packages/atlantis/src/auth/cookie.lev` (`cookieValue`) — the one site Track
08 hit combining both ingredients (`formField` in `csrf.lev` has the early
return but no class-typed parameter, so it was very likely never affected;
converted to the same accumulator shape anyway as cheap insurance, not
because it was independently confirmed to crash).

**Relationship to #95:** applying this workaround did NOT make Track 08's
full auth corpus (`packages/atlantis/tests/corpus/auth/`) LLVM-clean — it
still crashes on LLVM, later in the run, on plain `Router`/`Context`
construction sequences that don't match this entry's specific two-ingredient
shape at all (confirmed: the crash point moves around depending on unrelated
prior heap activity, e.g. reordering which test function runs first changes
*where* it crashes, never *whether*). Directly re-verified in this session
that #95's own repro (the untouched, pre-existing `packages/atlantis/tests/
corpus/routing` corpus, no Track 08 code involved at all) still segfaults on
LLVM within the first two lines of output. Conclusion: this entry's repro is
a real, independently-reproducible, minimal instance, but it is very likely
one symptom of the SAME broader systemic defect #95 already tracks (general
heap corruption from some class of object construction/early-return sequence
on LLVM, not specific to Auth's code) — severe enough that essentially any
non-trivial Atlantis LLVM program touching `Router`/`Context` construction
is currently affected. Track 08's corpus therefore ships with oracle+IR as
its passing reference (both green and byte-identical) and its LLVM lane
left red, same posture #95 already established for the routing corpus —
not a new regression to chase down inside Track 08.

---

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
