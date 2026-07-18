# Summary: Remove `let` — it is a second spelling of `const`

`let` is not a distinct binding form. The parser folds it into the *same* `isConst` bit
that `const` sets, there is no `isLet` anywhere in the AST, and no phase downstream of the
parser can tell the two apart. A `let` binding *is* a const binding, in the literal sense
that by the end of `parseVarDecl` no evidence survives that the word `let` was typed. This
request proposes retiring the keyword and spelling the concept `const var` (or `const T`).

This is the §11 rule of [const.md](../complete/const.md) ("one word never means two
things") read in the other direction: two words currently mean exactly one thing. const.md
§5 itself flagged that shape as "an unprincipled duplication waiting for a distinction" —
it then proposed `let` ≡ `const var` as the resolution, and that proposal is what shipped.
The distinction it was waiting for never arrived, because the distinction it named
("single-assignment") turned out to belong to a different keyword. See §Known Warnings.

## Request Details

**The implementation is identical, not merely similar.** Three points of evidence, each
confirmed against the tree at the time of writing:

1. **The parser sets one flag for both.**
   [Parser.cpp:794–797](../../src/Parser.cpp#L794) is the whole of the difference:
   ```cpp
   if (accept(TokenKind::KwConst)) v->isConst = true;   // const.md §3
   if (at(TokenKind::KwVar) || at(TokenKind::KwLet)) {
       v->inferred = true;
       if (at(TokenKind::KwLet)) v->isConst = true;      // `let` == `const var` (const.md §5)
   ```
   `let` is `var` plus the const bit. The comment on :797 says so outright. The same
   two-line pattern is duplicated for `for-in` loop variables at
   [Parser.cpp:988–991](../../src/Parser.cpp#L988).

2. **The AST has no `let`.** `grep -rn "isLet" src/ include/` returns nothing. The only
   surviving mentions of `KwLet` past the lexer are the parser sites that consume it —
   the type-position inference marker ([:227](../../src/Parser.cpp#L227)), the two decl
   sites above, the statement dispatch ([:1020](../../src/Parser.cpp#L1020), which falls
   through to the *same* `parseVarDecl` as `KwVar` and `KwConst`), and two lookahead
   predicates ([:1786](../../src/Parser.cpp#L1786), [:1797](../../src/Parser.cpp#L1797)).
   Checker, IR, and every backend see only `isConst`.

3. **The diagnostic already calls it const.** Reassigning a `let` produces the const
   error verbatim — confirmed by running it:
   ```
   void main() {
       let x = 1;
       x = 2;
   }
   ```
   → `error: cannot assign to const 'x'` ([Checker.cpp:1978](../../src/Checker.cpp#L1978)).
   The compiler is already telling the user that `let` is `const`. The keyword survives
   only in the source text the user typed; the language does not model it.

**Why removal rather than leaving it alone.** A redundant keyword is not free. It costs a
reserved word ([reference.md:57](../../docs/reference.md#L57)) that can never mean anything
else; it costs every reader a moment deciding whether the author's choice of `let` over
`const` carried intent (it cannot — the compiler cannot see it); it costs two parse sites
that must be kept in lockstep forever, where a future const feature landing on `const` but
not `let` is a silent divergence between two spellings that are contractually identical;
and it costs the language its own stated principle. "Reducing redundancy" is the first
concept info.md names. `let` is redundancy with a keyword's worth of surface area and
exactly zero semantic content — the cleanest possible case for the principle, since
removal cannot break a single program's *meaning*, only its spelling.

**Migration cost is near zero, and grows.** const.md §5 measured this once and found
exactly **one** `let` in the tree. That measurement is now stale — there are **8 files**:
`tests/corpus/const.ext`, `tests/corpus/churn/closures.ext`,
`tests/corpus/threads_serial/spawn_join.lev`, `tests/corpus/threads_native/spawn_join.lev`,
`tests/corpus/tasks/park_storm.lev`, `tests/corpus/tasks/order_fifo_three.lev`,
`tests/corpus/tasks/order_two_awaits.lev`, and `harpoon/src/runner.lev`. All are
`let name = <expr>;` inferred bindings, none reassigned (they could not be). Every one
becomes `const var name = <expr>;` — a mechanical, meaning-preserving substitution. The
same argument const.md made in favour of acting then applies with more force now: the
migration is nearly free today and only gets more expensive with every file added.

## Requested Specific Feature

Remove `let` from the language:

- Drop `KwLet` from the keyword table ([Token.cpp:120](../../src/Token.cpp#L120)) and the
  `TokenKind` enum ([Token.hpp:36](../../src/Token.hpp#L36), and its
  [Token.cpp:31](../../src/Token.cpp#L31) name mapping).
- Delete the five parser sites listed above. Each is a disjunct or a two-line branch; none
  requires restructuring, and each site's `KwVar` path already handles the shape.
- `let` becomes an ordinary identifier — worth deciding explicitly (see Open Questions).
- Update [reference.md:57, :159–164, :842](../../docs/reference.md#L159) and const.md §5's
  status.
- Migrate the 8 files above to `const var`.

**Preferred spelling for the concept.** `const var x = 1;` — already legal today, already
what `let` compiles to, and it composes out of two existing words rather than reserving a
third. This is the sugar-shelf argument from const.md §5 run in reverse: `let` was
justified *as* the composed spelling of `const` + `var`; if that composition is the
meaning, the language can simply say it.

## Known Warnings

**The docs mislabel `let`, and this is the motivation, not the request.**
[reference.md:161–164](../../docs/reference.md#L161) and
[const.md:200](../complete/const.md#L200) both describe `let` as **"a single-assignment
inferred binding."** That is inaccurate. `let` is not single-assignment; it is
*initialize-at-declaration, never-assign-again* — plain const. Single-assignment (assigned
exactly once, possibly deferred past the declaration) is precisely the semantics of
**`readonly`** ([techdesign-readonly.md:34](../complete/techdesign-readonly.md#L34):
"its initializer **or** any declaring-class constructor, **exactly once**"), and for
`const` locals it is the still-unimplemented OQ1 in
[techdesign-const-system-extensions.md §2](../techdesign-const-system-extensions.md) ("Definite
single assignment for `const` locals", M-OQ1, *not started*).

So the docs currently attach `readonly`'s meaning to `let`'s name, describing a
distinction the implementation does not have and a semantics that lives elsewhere. That
mislabel is *why* this request exists — a keyword whose documentation has to invent a
difference to justify it is a keyword with no difference. **But the ask here is removal,
not a doc fix and not a re-spec of `let` into real single-assignment.** If `let` were
instead redefined to mean deferred-single-assignment it would collide with `readonly` and
with M-OQ1's plan to give `const` that behaviour directly — a second word for a third
thing, which is the §11 rule violated from a new angle.

Other notes for design:

- **`const.ext` is corpus.** `tests/corpus/const.ext` is a `.ext` file. Per the standing
  rule, do **not** create new `.ext` files — but this one exists and is being *edited*,
  not created. Check whether it is shared with the frozen X64Gen/ELF corpus before
  touching it, and do not gate this work on any ELF finding.
- **No bug.md entry.** This is a redundancy, not a defect — nothing miscompiles.
- **`let` as an identifier.** Freeing the word means `int let = 1;` starts parsing. That
  is a widening of the accepted language, so it wants an explicit decision rather than
  falling out of the removal by accident.
- **Error quality.** Removing the keyword outright turns existing `let x = 1;` into a
  generic parse error. See Open Questions for the deprecation path.

## Acceptance Criteria

1. `let` is no longer a keyword: `KwLet` is gone from the lexer table, the `TokenKind`
   enum, and all five parser sites.
2. `const var x = 1; x = 2;` still errors with `cannot assign to const 'x'` — the
   behaviour `let` provided is reachable, unchanged, under its honest spelling.
3. All 8 files listed above are migrated to `const var` and their tests pass on the
   engines they already run on (oracle/IR/LLVM; no new ELF work).
4. reference.md no longer documents `let`, and no longer attributes "single-assignment"
   to it. The reserved-word list at :57 drops it.
5. const.md §5's `let` proposal is marked withdrawn/superseded, with a pointer here.
6. The decision on `let`-as-identifier (§Open Questions) is recorded either way.

## Open Questions

1. **Deprecate first, or remove outright?** Keeping `let` for one cycle as an accepted
   alias that emits `warning: 'let' is deprecated; use 'const var'` gives users a
   migration signal better than a parse error. Costs: the parser sites stay, and a
   deprecation needs a warning channel to exist. Given the migration is 8 in-tree files
   and the corpus is the only consumer, **outright removal is the recommendation** — the
   deprecation window exists to protect code that does not exist here.
2. **Should `let` become a usable identifier, or stay reserved-but-unused?** Reserving it
   costs nothing and leaves room to give the word a real meaning later; freeing it is the
   more honest end state. Recommendation: free it — a word held in reserve for a
   distinction we just concluded does not exist is the same redundancy in a quieter form.
3. **Does this reopen M-OQ1?** If `const` locals eventually get definite-single-assignment
   (`const int x; if (c) x = 1; else x = 2;`), that is the feature `let`'s documentation
   was describing all along — landing on `const`, where it belongs. Worth confirming the
   design owner agrees the two are the same want, so `let` is not resurrected for it.

## Interim Fallback

None needed — nothing is blocked. `const var` is legal today and is exactly what `let`
lowers to, so any code that would have used `let` can be written correctly right now. The
8 in-tree `let` usages compile and pass as-is; they are a migration to schedule, not a
breakage to work around. No work is parked and no branch is pending on this request.
