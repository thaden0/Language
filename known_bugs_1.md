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
| P1       | #78 |
| P2       | #72 |
| P3       | — |

Every open bug also carries a row in `docs/footguns.md` (workaround + debt sites) and,
once the composition corpus lands, a red-lane repro under `tests/corpus/composition/`
(`designs/techdesign-composition-corpus.md`). Fixing #N means: fix, delete the entry here,
sweep the footguns row's debt sites, promote red→green — one commit.

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

---

---


---

## #78 [P1] A bulk `uses NS;` import's function silently wins over a same-named top-level function declared in the IMPORTING file itself — no ambiguity error, no diagnostic

**Priority justification:** P1.1 — an actively-maintained engine (both
oracle and IR agree, so no engine contradicts the other) silently produces
a wrong value: a call resolves to the imported namespace's function instead
of the importing file's own declaration, exit 0, no diagnostic. Not capped
to P2 by the semantics-ruling override — which one *should* win is not a
live design question, it directly contradicts `info.md` §1's own stated
philosophy ("Honesty over hidden magic... resolution is by type, everywhere
... a bare read with no type in context is the one thing that cannot
resolve, and is therefore an error rather than a guess") and mirrors the
already-fixed bug #70 (`known_bugs_1.md` history via `docs/reference.md`
§6.6.6a): #70 fixed a local/parameter losing to a same-name namespace
function on **dispatch**; this is the same shape one level up — a
**top-level function declaration** loses to a same-name **bulk-imported
namespace function**, and #70's fix (`Lower.cpp`'s `lowerCall` NS::fn
fallback checking that the base name isn't a local/parameter) evidently
doesn't also check "isn't a top-level function in this file."

**Symptom.**

```lev
namespace NS {
    void greet() { console.writeln("from NS"); }
}
uses NS;
void greet() { console.writeln("from local"); }
void main() {
    greet();
}
main();
```

- **Expected:** `from local` — the file's own top-level `greet()` should
  win over (or at minimum, conflict loudly with) a same-named function
  merely *imported* via `uses`; a declaration in the file itself is a
  stronger claim than something pulled in by a bulk import.
- **Actual (oracle AND IR):** `from NS` — the call silently resolves to the
  imported namespace's function, exit 0, no diagnostic, on both engines.

**Root cause (pointer).** `src/Lower.cpp`'s `lowerCall` NS::fn fallback
(the code #70 touched, `known_bugs_1.md`/`docs/reference.md` §6.6.6a) tries
the enclosing/`uses`-imported namespaces as a fallback path for an
unqualified call name; it apparently doesn't check whether the base name
already resolves to a **top-level function declared in the current file**
before taking that fallback, the same way it now checks locals/parameters.

**Workaround (verified).** Never give a file's own top-level function the
same bare name as anything pulled in by one of its `uses` imports; when in
doubt, call the imported form qualified (`NS::fn(...)`). Adopted by
`harpoon/tests/assertions/main.lev`, which needs its own self-test entry
point in a file that also `uses harpoon;` (harpoon itself exports
`harpoon::main()` per the design's §6 runner) — the local entry point is
named `runSelfTest()`, never `main`, and reserves `harpoon::main()`
qualified for its actual designed caller.

**Found:** implementing `designs/techdesign-unit-test-library.md` M1
(`harpoon/tests/assertions/main.lev`), 2026-07-15 — the moment
`harpoon/src/runner.lev` added `harpoon::main()`, the test file's own
`void main() { ... } main();` (the standard convention used by nearly
every `.lev` example in this repo) started silently running the wrong
function. Any future Harpoon consumer that both `uses harpoon;` and
defines its own `main` hits this identically.

---

## #72 [P2] `std::HttpClient.requestTls` reports a plain TCP connect failure as a misleading empty-reason "TLS handshake" error

**STATUS: diagnostic FIXED (2026-07-15, commit e02678b + follow-up).** Both
`requestTls` and its sibling `request` now check `fd < 0` immediately after
`sysTcpConnect` and throw a clear `TCP connect failed (host '...', port N)`
before ever reaching `tlsConnect`/`sysSend`. Verified on oracle + IR;
`run_tls.sh` fully green. **STILL OPEN (native floor):** *why* `sysTcpConnect`
returns -1 for the compiled process while the shell reaches the host fine — see
the open question below; that part of #72 remains unresolved and keeps this
entry open at P2.

**Priority justification:** P2.4 (closest fit) — the happy path (successful
TCP connect + TLS handshake) is unaffected; only the failure-path diagnostic
is wrong, masking the real cause (a plaintext TCP connect failure) behind an
unrelated TLS-specific message. Not P0/P1: nothing silently corrupts or
misreports a *successful* operation, and there's a working escape hatch
(inspect `fd` / add your own pre-check) once the miswiring is known. May
warrant a bump if `sysTcpConnect` itself turns out to be genuinely broken
rather than just poorly reported (see open question below).

**Symptom.** `std::HttpClient.requestTls(...)` (`src/Resolver.cpp:2334-2356`):

```
void requestTls(string method, string host, int port, string path,
                HeaderMap headers, string body, (HttpResponse) => void onResp) {
    int fd = std::sysTcpConnect(host, port);
    ...
    std::tlsConnect(fd, host, "", "", 0, (cfd) => { ... });
}
```

never checks `fd` for failure before handing it to `tlsConnect`, even though
`sysTcpConnect` is documented to return `-1 on failure` (`src/Resolver.cpp:1417`).
`tlsConnect` (`src/Resolver.cpp:1727-1734`) arms a TLS session on that fd and,
when arming fails, throws
`RuntimeException("TLS handshake: " + std::sysTlsError(fd) + " (host '" + host + "', fd " + fd + ")")`.
With `fd == -1` there is no real handshake state for `sysTlsError` to report a
reason for, so the thrown message reads (note the double space where the
reason should be):

```
TLS handshake:  (host 'www.google.com', fd -1)
```

— i.e. a plain TCP connect failure is reported as an empty-reason TLS
failure, hiding what actually went wrong.

**Repro.** From `examples/recon` (`src/net/sender.lev:94-97` calls
`client.requestTls(...)`), send a GET to `https://www.google.com` from the
compiled app: the request fails immediately with the message above instead
of a clear "TCP connect to www.google.com:443 failed" error.

**Root cause (pointer):** the missing `fd < 0` guard in `requestTls` (and
its sibling `request`, `src/Resolver.cpp:2298-2320`, which has the same gap
but degrades more gracefully since there's no TLS-specific message to
obscure it) is a clear, mechanical fix — check `fd` immediately after
`sysTcpConnect` and throw a plain connect-failure exception before ever
calling `tlsConnect`. **Open question, not chased (house rule — framework
work stops at `src/**`):** *why* `sysTcpConnect` itself returned -1 for
`www.google.com:443` in the environment this was found in, when plain shell
`curl` to the same host succeeded during the same investigation. Could be
environment-specific (network namespace/permissions differing between the
compiled Leviathan process and the invoking shell) or a genuine native-floor
bug in `sysTcpConnect`'s connect/DNS-resolution logic — undetermined.

**Found:** investigating a user report against `examples/recon`
(`designs/sonar/sonar-bugs.md` #3), 2026-07-14.
