# Research: Remove `let`

**Purpose:** source material for the tech design implementing
`designs/requests/request-remove-let.md` (moved to `designs/requests/accepted/` alongside this
doc). This document does not decide anything the request didn't already decide — it re-verifies
every factual claim in the request against the tree **today** (2026-07-23), corrects several that
have drifted, and surfaces a few facts the request didn't have. Where a finding changes something
the design needs to act on, it's flagged **Finding**.

The short version: the request's *reasoning and structure* hold up completely — `let` really is
zero-content redundancy with `const var`, the five-parser-site shape is exactly right, and the
live repro still reproduces verbatim. But the request was written before `refactor_1` restructured
`src/` into layered directories (`[[leviathan-refactor1-authored]]`), so **every file:line citation
in the request is stale** — files moved, and `Checker.cpp` split into pieces. And the migration
inventory is wrong in a way that matters for Acceptance Criterion 3: it's **4 files**, not 8.

---

## 1. File layout has moved — every citation needs re-pointing

The request cites `src/Parser.cpp`, `src/Token.hpp`, `src/Token.cpp`, `src/Checker.cpp`. None of
those paths exist anymore:

| Request's path | Current path |
|---|---|
| `src/Parser.cpp` | `src/frontend/Parser.cpp` |
| `src/Token.hpp` | `src/core/Token.hpp` |
| `src/Token.cpp` | `src/core/Token.cpp` |
| `src/Checker.cpp` (for the const-assignment error) | `src/sema/CheckerInfer.cpp` |
| `src/Ast.hpp` | `src/core/Ast.hpp` |

This is `[[leviathan-refactor1-authored]]` (landed 2026-07-21, after the request's writing but
before today) — a mechanical file move, not a semantic change; nothing below suggests the removal
itself is harder than the request assumed, only that its line numbers point at the wrong lines
now. The tech design should cite the paths in the table above.

## 2. The five parser sites — structure confirmed exactly right, line numbers corrected

`grep -rn "KwLet" src/ include/` today returns exactly 8 lines, which group into the same 5
conceptual sites the request describes (one of them — the type-position marker — the request
folds into its "AST has no `let`" enumeration rather than its "two decl sites" list, but it's the
same site set):

| # | Site | Request's line (stale) | Current line | Current path |
|---|---|---|---|---|
| 1 | Type-position inference marker (`parseTypePrimary`) | `:227` | `:202` | `src/frontend/Parser.cpp` |
| 2 | Local/global var-decl (`parseVarDecl`) | `:794–797` | `:369–372` | `src/frontend/Parser.cpp` |
| 3 | For-in loop variable | `:988–991` | `:619–622` | `src/frontend/Parser.cpp` |
| 4 | Statement dispatch (falls through to `parseVarDecl`) | `:1020` | `:650` | `src/frontend/Parser.cpp` |
| 5a | Comptime marker lookahead (`tryParseComptime`) | `:1786` | `:1013` | `src/frontend/Parser.cpp` |
| 5b | Comptime var-decl lookahead (`tryParseComptime`) | `:1797` | `:1024` | `src/frontend/Parser.cpp` |

Current text of sites 2 and 3 — confirming the "two-line pattern, duplicated" description is
exactly accurate, not approximate:

```cpp
// src/frontend/Parser.cpp:369-372 (parseVarDecl)
if (at(TokenKind::KwVar) || at(TokenKind::KwLet)) {
    v->inferred = true;
    if (at(TokenKind::KwLet)) v->isConst = true;      // `let` == `const var` (const.md §5)
    advance();
}
```
```cpp
// src/frontend/Parser.cpp:619-622 (for-in)
if (at(TokenKind::KwVar) || at(TokenKind::KwLet)) {
    s->inferred = true;
    if (at(TokenKind::KwLet)) s->isConst = true;
    advance();
}
```

Site 4, the statement dispatch, is a 3-way `switch` fallthrough — deleting the `KwLet` case is a
one-line removal, no restructuring:
```cpp
// src/frontend/Parser.cpp:649-652
case TokenKind::KwVar:
case TokenKind::KwLet:
case TokenKind::KwConst:
    return parseVarDecl();
```

Site 1, the type-position marker (this is the one the request's point 2 calls "the type-position
inference marker \[:227\]" rather than listing among the "two decl sites" — same site, just
described in the AST-search paragraph instead):
```cpp
// src/frontend/Parser.cpp:202 (parseTypePrimary)
if (at(TokenKind::KwVar) || at(TokenKind::KwLet)) {
    auto t = mkType(TypeKind::Inferred, cur().span);
    advance();
    return t;
}
```
Removing `KwLet` here just drops it from the disjunct — `KwVar` alone still triggers the inferred-
type path. No other logic in this function touches `KwLet`.

Sites 5a/5b, both inside `tryParseComptime` (12 lines apart in the same function, not two
separately-located predicates as "two lookahead predicates" might suggest):
```cpp
// src/frontend/Parser.cpp:1012-1013
bool marker = n == TokenKind::KwIf || n == TokenKind::KwVar ||
              n == TokenKind::KwLet || parserDetail::canStartExpr(n);
...
// src/frontend/Parser.cpp:1024
if (at(TokenKind::KwVar) || at(TokenKind::KwLet) || looksLikeVarDecl()) {
```
Both drop cleanly to `KwVar` alone; `looksLikeVarDecl()` (a separate lookahead helper a few lines
above, checking `Type Identifier (=|;)`) already covers the explicit-type comptime-const case and
does not mention `KwLet` itself.

**Lexer sites**, current locations:
```cpp
// src/core/Token.hpp:36 (enum TokenKind)
KwVar,
KwLet,
```
```cpp
// src/core/Token.cpp:31 (name-mapping switch)
case TokenKind::KwVar:  return "KwVar";
case TokenKind::KwLet:  return "KwLet";
```
```cpp
// src/core/Token.cpp:120 (keyword table)
{"var",  TokenKind::KwVar},
{"let",  TokenKind::KwLet},
```

**AST — confirmed no `isLet`.** `grep -rn "isLet" src/ include/` returns nothing today, same as
when the request checked. `src/core/Ast.hpp:470`: `bool inferred = false;  // Var: type name =
init; (type null + inferred=true for var/let)` — the comment itself already describes `var`/`let`
as one case.

## 3. The live repro — re-verified against today's build, byte-identical

Built `build/leviathan` at current HEAD and ran the request's exact repro:
```
void main() {
    let x = 1;
    x = 2;
}
```
```
$ ./build/leviathan --run repro.lev
repro.lev:3:5: error: cannot assign to const 'x'
      x = 2;
      ^
1 error(s)
```
Matches the request's claim exactly (modulo the file being at `src/sema/CheckerInfer.cpp:1219/1223`
now instead of `Checker.cpp:1978` — same stale-line pattern as §1; `CheckerInfer.cpp` is where the
const-assignment check now lives post-`refactor_1`, still reachable from `Checker.cpp`'s expr
dispatch).

Also verified the request's **Open Question 2 premise directly**: `let` is genuinely reserved
today, not just "acts reserved" — `int let = 5;` fails to parse (`expected ';'` at the point where
the parser expects an identifier but instead sees the `let` keyword being consumed by the earlier
disjunct). Freeing it really would be a widening of the accepted grammar, exactly as OQ2 frames it,
not a no-op.

## 4. Migration inventory — the request's "8 files" is stale; it's 4 — Finding

The request measured 8 files via what was presumably a prior grep pass; re-running a precise
`let <ident> =` pattern (word-boundary, not matching the English word inside comments) across every
tracked `.lev`/`.ext` file today finds **7 hits total**, of which **3 are not actual `let`
declarations in compiled Leviathan source** and **1 file the request named no longer contains a
`let` at all**:

**Real migrations needed (4 files) — matches the request's own examples exactly:**
| File | Line | Code |
|---|---|---|
| `tests/corpus/const.ext` | 41 | `let localZ = 12;` |
| `tests/corpus/churn/closures.ext` | 12 | `let f = (x) => x + i;` |
| `tests/corpus/threads_serial/spawn_join.lev` | 53 | `let inner = (z) => z + 1;` |
| `tests/corpus/threads_native/spawn_join.lev` | 53 | `let inner = (z) => z + 1;` |

**In the request's list but NOT actually `let` declarations — the three other corpus files it
named (`tasks/park_storm.lev`, `tasks/order_fifo_three.lev`, `tasks/order_two_awaits.lev`) only
contain the English word "let" inside a `//` comment** (e.g. `// pump: let all three children
finish, then close()`). These need no code change at all — grep with a naive `\blet\b` (no
`=`-follows check) is exactly the kind of false positive that inflates a count like this; worth
noting in the design as the reason the number moved.

**`harpoon/src/runner.lev` doesn't exist anymore.** Harpoon was renamed to Sonar 2026-07-22
(`[[leviathan-toolchain-naming]]`/policies.md §Testing: *"Sonar is the unit-test framework... in
the `sonar/` folder (renamed from Harpoon 2026-07-22)"*) — the file is now `sonar/src/runner.lev`,
and it contains no actual `let` declaration either (only the English word inside two comments,
same false-positive shape as above). The request's citation predates the rename.

**Net: Acceptance Criterion 3 ("All 8 files listed above are migrated") should read 4 files** —
`const.ext`, `churn/closures.ext`, `threads_serial/spawn_join.lev`,
`threads_native/spawn_join.lev`. All four are exactly the shape the request describes: inferred,
never-reassigned, mechanical `let name = expr;` → `const var name = expr;`.

### 4.1 Three additional files contain the word `let` as data, not code — new to this research

Not in the request's list at all — found by the same repo-wide grep, worth the design owner
knowing about even though **none need to change**:

- **`examples/helm/tests/search/fixtures/proj/a.lev:2`** — `let needle_token = 1;`. This is a
  *search-fixture* file under `fixtures/proj/`, read by `WorkspaceSearch` (`examples/helm/src/
  search/workspacesearch.lev`) as **raw text for a literal/regex substring scan** — confirmed by
  reading the sibling fixture `fixtures/proj/sub/b.lev:1`, `fn f() { return another needle_token
  here; }`, which isn't even syntactically valid Leviathan (`fn`, bare `another`/`here`) — proof
  these fixtures are never compiled, only grepped. Also confirmed structurally: `examples/helm/
  trident.toml`'s `sources` list only globs `src/**/*.lev`; `tests/search/fixtures/` is never a
  build input for any `trident.toml` in the tree.
- **`examples/helm/tests/search/search_test.lev:56`** — `realFile.write("let needle_token =
  2;\n");` — a Leviathan *string literal* written to a scratch file at test runtime, then searched
  the same way. Not parsed as Leviathan source by anything in the test.
- **`examples/helm/tests/langloop/langloop_test.lev:45`** — `buf.insert(Pos(0, 0), "let x =");` —
  a string literal inserted into a `TextBuffer` for a diagnostics-controller test (`G-H3` golden);
  the test drives a `FakeLanguageService` that delivers *canned* diagnostic batches, it never
  actually compiles the buffer's text.

None of these three are load-bearing on the `let` keyword's existence — they'd search/insert
exactly the same whether `let` remains a keyword or not, since nothing compiles them. Leaving them
untouched is correct; the design doesn't need an owner's-call here, just doesn't need to list them
as migration targets either.

## 5. Documentation sites — reference.md and const.md confirmed; info.md is a fourth site the request missed — Finding

**`docs/reference.md`** — the request cites `:57, :159-164, :842`. Re-checked against current
content:
- `:57` — reserved-word list, confirmed present: `` get  set  return  var  let  await  bind
  inject  use  uses `` (§1.3 Keywords). Matches the request.
- `:161-166` (request said `:159-164`, off by ~2 lines — minor drift, same section): §2.1 Type
  expressions —
  ```
  var / let                     // inference markers (not types; the static type is inferred)
  ```
  ```
  `var` and `let` are both pure inference markers: the declared type is whatever the
  initializer's type is. They differ only in mutability — `let` is sugar for `const var`
  (§4.3b): a single-assignment inferred binding. `var` stays freely reassignable. Neither
  is a type; `var`/`let` never appear in a type position beyond the declaration site.
  ```
  This is the exact mislabel the request's "Known Warnings" section flags — "single-assignment"
  attributed to `let` when the implementation gives it plain const (initialize-once,
  never-reassign) semantics, not deferred-single-assignment.
- `:842` — **stale, does not exist.** Current `:835-850` is unrelated content (DI bind-activation
  rules, §5-ish territory). The remaining `var`/`let` mention in reference.md is actually at
  `:894`: `` Type name = expr;      var name = expr;       // declaration (var/let infer) `` — a
  syntax-summary table row. The design should retarget the request's third reference.md citation
  to `:894`, not `:842`.

**`designs/complete/const.md`** — the request's §5 citation confirmed, unchanged since the
request was written (this file predates `refactor_1` and isn't itself touched by the layered-`src/`
move — it cites old `Parser.cpp` line numbers internally too, e.g. `:200`, `:579`, `:742`,
`:1225`, all now stale by the same shift, but that's pre-existing drift in a file already marked
complete, not something this design needs to fix). §5's proposal text (`let` ≡ `const var`) is
exactly what shipped, confirming the request's central claim: *"that proposal is what shipped."*
§11's rule and its own §5 "unprincipled duplication waiting for a distinction" self-description are
both present and read exactly as the request quotes them.

**`info.md:928-935` — a fourth documentation site, not in the request's list:**
```
### `var` / `let` vs `any`

- **`var` / `let`** are **inference markers, not types** — the value has a fixed static type,
  only its spelling is inferred. Free, safe. Kept.
```
This is info.md's §-level treatment of the same var/let-vs-any distinction reference.md §2.1
covers. `const.md §7` (Implementation inventory) itself lists updating "info.md gets the §1 matrix"
as part of shipping `const` originally — the same doc that needs a matching update now. The
section heading itself (`` `var` / `let` vs `any` ``) names `let`, so this isn't just a body-text
edit; the design should decide the heading's replacement (` `var` vs `any` `, presumably) alongside
the body-text drop.

No other doc site exists: `docs/gotchas.md`, `docs/ir-spec.md`, `docs/archectecture.md` all have
zero `let` mentions (checked directly). No syntax-highlighter, LSP, or keyword-list dependency
exists in `tools/`, `examples/moby`, `examples/helm`, or `sonar/` (grepped for `"let"`, `KwLet`,
`kw_let` — nothing). No dedicated parser/lexer/checker unit test (`tests/test_parser.cpp`,
`tests/test_lexer.cpp`, `tests/test_checker.cpp`) hardcodes `let` or `KwLet` — the only test
exposure is the 4 corpus files in §4, run through the normal `--run`/`--ir`/`--build`/
`--build-native` corpus harness, not bespoke keyword tests.

## 6. Open Question 1 (deprecate vs. remove outright) — a warning channel already exists, contradicting the request's stated cost — Finding

The request frames deprecation's cost as: *"a deprecation needs a warning channel to exist"* — implying
one doesn't. **It already does, and is already used once elsewhere:**
```cpp
// src/core/Diagnostic.hpp:17-28
class DiagnosticSink {
public:
    void error(SourceSpan span, std::string message) { ... ++errorCount_; }
    void warning(SourceSpan span, std::string message) {
        diags_.push_back({Severity::Warning, std::move(message), span});
    }
    void note(SourceSpan span, std::string message) { ... }
```
```cpp
// src/meta/RulesExpand.cpp:908 — the one existing caller
sink_.warning(a.span, msg);
```
`Severity::Warning` diagnostics are collected and rendered exactly like errors, just without
incrementing `errorCount_` (so they never fail the build). Had the design wanted a deprecation
cycle, `if (at(TokenKind::KwLet)) sink_.warning(cur().span, "'let' is deprecated; use 'const
var'");` at site 2/3 (§2) before still setting `isConst` would have been a small, precedented
addition, not new infrastructure.

**This doesn't overturn the request's recommendation** — outright removal is still the better
call, and more so now that §4 shows the real migration surface is 4 files, not 8: the "protect
code that does not exist here" reasoning gets stronger, not weaker, once the count drops. This
finding just corrects the stated cost side of that tradeoff for whoever writes the design, in case
the smaller number changes the calculus.

## 7. Open Question 3 (does this reopen M-OQ1?) — already answered, independently of this request

The request asks whether a future `const` definite-single-assignment feature would "resurrect"
`let`. It already shipped, and not as `let`:
```
designs/complete/techdesign-const-system-extensions.md:3
Status: ALL FOUR items discharged. OQ2 + OQ3/OQ4 landed 2026-07-16; OQ1 (definite
single assignment) IMPLEMENTED 2026-07-18...
```
`const int x; if (c) x = 1; else x = 2;` (definite single assignment, deferred past the
declaration) is implemented as a **checker-only flow analysis on `const` locals directly**
(`[[leviathan-prelude-no-narrowing]]`'s neighbor feature, unrelated to `let`) — `let` was never
touched by it and never will be, since `let`'s only meaning was always plain const
(initialize-at-declaration), not deferred assignment. The design can cite this as closed rather
than open: M-OQ1 already landed on `const`, confirming — not just arguing — that `let`'s
documentation was describing a feature that belongs to a different keyword, exactly as the
request's "Known Warnings" section already concluded from first principles. Nothing here changes
the request's recommendation; it just upgrades OQ3 from a question to a confirmed fact worth citing
in the final design rather than re-litigating.

## 8. `readonly` — confirmed structurally distinct, no interaction

Cross-checked the request's claim that `readonly`, not `let`, is the "assigned exactly once,
possibly deferred" keyword:
```
designs/complete/techdesign-readonly.md (mutation-control matrix)
| keyword    | when may the slot be written?                          | when is the value known | scope              |
| `var`      | always                                                  | — (never fixed)         | anywhere           |
| `const`    | its initializer only                                   | compile time             | any slot incl. field |
| `readonly` | its initializer or any declaring-class ctor, exactly once | construction time      | instance fields only |
```
`readonly` is field-only, construction-time-fixed; `const` (and therefore `let`, which is just
spelled-differently `const var`) is compile-time-fixed. No code path connects `KwLet` to
`readonly` anywhere (`readonly` has its own dedicated parser branch,
`src/frontend/Parser.cpp:653`, that explicitly rejects it at local-declaration position with a
"use 'const' for a write-once local" diagnostic pointing at `const`, never `let`). Removing `let`
cannot perturb `readonly` in any way — confirms the request's Known Warnings section needed no
correction here, only the citations above did.

## 9. Summary for the design

| Request claim | Status |
|---|---|
| `let` is `const var` with one shared `isConst` bit, no `isLet` in the AST | **Confirmed**, structure and code unchanged since the request, only paths/lines moved (§1-2) |
| Five parser sites, each a disjunct or 2-line branch | **Confirmed exactly** — see §2 table for corrected paths/lines |
| Live repro: `let x=1; x=2;` → `cannot assign to const 'x'` | **Re-verified byte-identical** against current HEAD build (§3) |
| 8 files need migration | **Wrong — 4 files.** 3 of the 8 only had "let" in a comment; the 8th file (`harpoon/src/runner.lev`) was renamed to `sonar/src/runner.lev` in the 2026-07-22 rename and doesn't contain a `let` declaration either (§4) |
| 3 more files contain literal `"let..."` text | **New finding, not in request** — all inert (search fixtures / string literals never compiled), need no change (§4.1) |
| reference.md `:57, :159-164, :842` | `:57` confirmed; body text is actually `:161-166`; `:842` is stale — real third citation is `:894` (§5) |
| const.md §5 status | **Confirmed**, unchanged, own internal line citations separately stale but out of this design's scope |
| info.md also documents `var`/`let` | **New finding, not in request** — `info.md:928-935`, heading names `let` explicitly (§5) |
| Deprecation "needs a warning channel to exist" | **Wrong — channel exists today**, `DiagnosticSink::warning()`, one precedented caller. Doesn't change the outright-removal recommendation, corrects its stated cost (§6) |
| OQ3 (reopens M-OQ1?) | **Already answered** — M-OQ1 landed 2026-07-18 on `const` directly, independent of `let` (§7) |
| `readonly` is the real single-assignment keyword, unrelated to `let` | **Confirmed**, no code path connects them (§8) |
| `int let = 1;` currently fails to parse | **Verified live** — OQ2's premise (freeing `let` is a real grammar widening) holds (§3) |

Nothing in this research blocks or narrows the request's Requested Specific Feature, Acceptance
Criteria, or its two recommendations (outright removal; free `let` as an identifier) — every
correction above is a citation/count fix, not a new obstacle. The design can proceed directly from
the request's Acceptance Criteria with the file list in §4 substituted for the request's Criterion
3, and the citations in §2/§5 substituted for the request's stale ones.
