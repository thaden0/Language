# Deferral Resolution — Labeled `break` / `continue`

**Status: design ready, not implemented.** **Date:** 2026-07-06.
**Resolves:** the explicit v1 deferral in
`designs/complete/techdesign-02-control-flow.md` F1 spec, line 36 ("Unlabeled
only. Labeled forms: **explicitly deferred** (roadmap, overview §5)") and the
roadmap entry in `designs/complete/techdesign-00-overview.md` §5 ("Deferred items placed
explicitly on the roadmap (not implicit): **labeled break/continue (02)**, ...").
**Depends on:** Track 02 landed (it is — DONE 2026-07-06); the compiler-file
merge-order rule (overview §2.1: 01 → 02 → 03 → 07 serialize). Track 03 is
currently STOPped (statics ruling, `/this_bug.mg`); this work touches the same
compiler files, so it must either land while 03 remains stopped or rebase after
03 lands — coordinate before starting (see §4).
**Owns (regions):** `Parser.cpp` statement region (`parseStatement` 706, the
Break/Continue cases 855-866, the loop cases 784/793/810/867, the default case
878), `Ast.hpp` `Stmt` (one new field; **no new StmtKind, no new TokenKind**),
`Checker.cpp` statement region (Break/Continue 1477-1482, the four loop cases
1483-1551, the four function-boundary resets 1330-1348 / 1578-1591 / 1702 /
1735) + `Checker.hpp` (beside `loopDepth_`, line 63), `Eval.cpp` statement exec
(957-1098) + `Eval.hpp` (beside `breaking_`/`continuing_`, 67-68), `Lower.cpp`
loop/break/using region (LoopCtx sites 666-785, `lowerUsingCleanupGroups`
429-504) + `Lower.hpp` (`LoopCtx` 49, `UsingCtx` 59-65), `Rules.cpp::cloneStmt`
(1635-1674, field copy-through), `AstPrinter.cpp` (269-281), the three corpus
runner scripts (`.lev` glob — see F7), new corpus/checker tests.
**No IR changes** — labeled forms lower to the same `Op::Jump` record-and-patch
idiom as unlabeled ones, so IrInterp, CGen, X64Gen, and LLVM are all covered
**for free** via lowering, exactly as Track 02's header claims for F1. The
X64Gen freeze is not merely respected but untouched: zero backend edits exist
to make (see F6).

---

## 1. Summary & the deferral

Track 02 shipped `break;` and `continue;` binding to the **nearest** enclosing
loop only. The labeled forms — naming a loop and exiting/continuing *that* loop
from arbitrary nesting depth — were deferred by name in two places:

- `designs/complete/techdesign-02-control-flow.md:36` — "Unlabeled only.
  Labeled forms: **explicitly deferred** (roadmap, overview §5)."
- `designs/complete/techdesign-00-overview.md` §5 (timeline, deferred list) — "labeled
  break/continue (02)".

This document resolves that deferral end-to-end: surface syntax, label binding,
checker validation, oracle semantics, IR lowering — **including the interaction
with `using`-block cleanup**, which is the one genuinely hard part. A labeled
break out of N nested using-blocks must close exactly the resources declared
inside the *target* loop (in reverse order, via the cleanup-group stub chain),
and none declared outside it. Track 02's stub chain is hardwired to the
innermost loop (`loops_.back()`, Lower.cpp:480/484/491/495); the design below
generalizes it to per-target chains without changing a single instruction of
the lowered output for label-free programs.

Target syntax (one label per loop, all four loop kinds):

```
outer: for (int i = 0; i < n; i = i + 1) {
    using File f = File(paths.get(i), std::read);
    inner: while (more()) {
        if (fatal())   break outer;      // closes f, exits both loops
        if (skipRow()) continue outer;   // closes f, next i
        if (skipCol()) continue inner;   // f stays open, next while-test
    }
}
```

## 2. Why it was deferred

Track 02's F1 was deliberately minimal: the unlabeled forms cover the
overwhelmingly common case with a one-integer checker (`loopDepth_`) and a
two-list `LoopCtx`, and v1's Gate-A deadline (overview §5, Phase A Jul 6-12)
rewarded shipping the 95% case. The labeled forms were *not* deferred because
they are conceptually hard in isolation — they were deferred because F3
(`using`) landed in the same track, and labeled exits multiply against the
cleanup-group stub-chain design: every exit-kind stub in a cleanup group
(Lower.cpp:470-497) terminates in `loops_.back()`, an assumption only valid
when every break crossing a given using targets the same (innermost) loop.
Getting that interaction right needed the F3 layout to exist and settle first.
It has: F3 landed 2026-07-06, five-engine byte-identical, with the
`usingsFloor` refinement in its implementation log. The ground is now stable
enough to build on.

## 3. Resolution design

### F1 — Surface syntax & parsing

**Spec:**
- A **loop label** is `identifier :` immediately before `while`, `for`
  (both forms), or `do`. One label per loop statement (no `a: b: while` —
  see problem #2). Labels on non-loop statements (blocks, ifs) are **not**
  in scope for this resolution (no labeled-block break; that would be a new
  deferral if ever wanted).
- `break identifier ;` and `continue identifier ;` name a label on a loop
  that **lexically encloses** the statement, within the same function body.
- Labels live in their own namespace: a label may share a name with a
  variable, class, or namespace with no interaction (labels appear only in
  the two positions above; nothing else ever resolves one).
- Unlabeled `break;` / `continue;` are untouched: nearest enclosing loop,
  labeled or not.

**Ambiguity analysis (grounded, this is why the syntax is safe):**
- `Colon` is a real token (Token.hpp:59) with **no infix binding power** —
  Parser.cpp:407-409 documents that `parseExpr(0)` stops at `:` naturally,
  the same property the ternary (`? then :`, Parser.cpp:506-514) and the
  `$for ... : elem` splice (Parser.cpp:419) rely on. In statement position an
  expression can therefore never *begin* `identifier :` — today
  `foo: while(...)` fails with "expected ';'" at the colon. The new form
  claims dead grammar.
- `::` lexes as one token (`ColonColon`, Token.hpp:58), so `foo::bar();`
  statements never see a bare `Colon` and are unaffected.
- `break foo;` is equally dead grammar today: the Break case
  (Parser.cpp:855-859) allows only `;` after the keyword.

**Implementation:**
1. **Zero new tokens, zero new keywords, zero new StmtKinds.** (The
   overview §2.1 merge-etiquette pain — enum append-points — does not arise.)
2. `Ast.hpp` `Stmt` (247-342): one new field beside `isUsing`/`usingClose`
   (295-296):
   ```cpp
   // Labeled loops (deferal-labeled-break-continue.md): on While/DoWhile/
   // For/ForIn — this loop's label ("" = unlabeled). On Break/Continue —
   // the target label ("" = unlabeled, nearest loop). `labelTarget` is the
   // checker-resolved target loop Stmt for a labeled Break/Continue (same
   // stash precedent as Expr::resolved / Stmt::usingClose); null until
   // checked, and NEVER copied by cloneStmt (see F2).
   std::string_view label;
   const Stmt* labelTarget = nullptr;
   ```
   Do **not** reuse `Stmt::name` — ForIn already stores the loop *variable*
   there (Parser.cpp:818), and Empty stores `@anchor` marker names
   (Parser.cpp:742).
3. `Parser.cpp:706 parseStatement`, new guard at the **top** of the switch
   (before `switch (cur().kind)` or as a pre-check):
   ```cpp
   if (at(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon &&
       (peek(2).kind == TokenKind::KwWhile || peek(2).kind == TokenKind::KwFor ||
        peek(2).kind == TokenKind::KwDo)) {
       std::string_view lbl = cur().text;
       advance(); advance();                       // ident, ':'
       StmtPtr s = parseStatement();               // hits the loop case
       if (s) s->label = lbl;
       return s;
   }
   ```
   The three-token guard makes the recursion total: `peek(2)` is a loop
   keyword, so the recursive parse lands in the While/For/DoWhile case and
   the returned kind is a loop by construction (For covers ForIn — the
   in-vs-semicolon scan at 798-808 runs inside the KwFor case as today).
4. Break case (855) / Continue case (861): after `advance()`, accept an
   optional label:
   ```cpp
   if (at(TokenKind::Identifier)) { s->label = cur().text; advance(); }
   expect(TokenKind::Semicolon, "';'");
   ```

### F2 — AST plumbing: clone + print

- `Rules.cpp::cloneStmt` (1635-1674): add `out->label = s->label;` beside the
  `isConst`/`isUsing` copies (1648-1652). This is the exact field-drop trap
  the overview §2.1 warns about (isRawSegment, isUsing, isConst all hit it) —
  it is a **must**, with a checker test that would catch the drop (a rule
  injecting a labeled loop + labeled break).
- **Deliberately do NOT copy `labelTarget`** (leave the clone's null). It is
  a checker product: rules run before pass 2 (Ast.hpp:315-316), the checker
  runs after splicing, so `labelTarget` is always null when cloneStmt runs —
  and copying a cross-node Stmt pointer into a clone would dangle into the
  template tree if that ever changed. Put a comment saying exactly this next
  to the `label` copy so a future "copy EVERY field" sweep doesn't "fix" it.
- `AstPrinter.cpp` (269-281): loop cases and Break/Continue print
  ` label=<x>` when `s->label` is non-empty. (Parser golden tests read this.)

### F3 — Label binding: where it lives, and the checker rules

**Decision: label binding lives in the Checker, not the Resolver.** Grounds:
the Resolver's statement walk (`resolveStmtTypes`, Resolver.cpp:1617-1632)
resolves *type names only* — the loop cases touch `s->type` / expr types and
nothing else; break/continue fall to its `default:` (1644) today and carry no
types tomorrow. Labels are pure lexical control-flow structure, and the
checker already owns exactly that structure: `loopDepth_` (Checker.hpp:63)
with its four loop increments (DoWhile 1484-86, While 1518-20, For 1529-31,
ForIn 1546-48) and its four function-body-boundary resets (lambda
1330-1348, Bind factory 1578-1591, `checkFunction` 1702, the walk()
Bind site 1735). The label stack is those same sites plus a name. Binding
the target once, in the checker, and stashing it on the Stmt (the
`Expr::resolved` / `Stmt::usingClose` precedent, Checker.cpp:1409/1471) means
shadowing/scoping rules exist in exactly one place; Eval and Lower consume
the pointer. **Resolver.cpp needs zero changes.**

**Checker implementation:**
1. `Checker.hpp`, beside `loopDepth_` (63):
   ```cpp
   struct LabelEntry { std::string_view name; const Stmt* stmt; };
   std::vector<LabelEntry> labelStack_;
   ```
2. Each of the four loop cases, when `s->label` is non-empty: scan
   `labelStack_` for the same name — hit ⇒
   `error(s->span, "label '<L>' is already used by an enclosing loop")`
   (Java's rule; see problem #2) — then push `{s->label, s}` right where
   `++loopDepth_` sits; pop right where `--loopDepth_` sits. Sibling loops
   reusing a label are therefore legal (the first pops before the second
   pushes).
3. Break (1477) / Continue (1480): when `s->label` is empty, the existing
   depth-0 check, unchanged. When non-empty, search `labelStack_` back-to-
   front; miss ⇒ `error(s->span, "no enclosing loop is labeled '<L>'")`
   (this message also covers labeled break outside any loop — the stack is
   empty); hit ⇒ `const_cast<Stmt*>(s)->labelTarget = entry.stmt`. No
   separate `loopDepth_` check is needed on the labeled path (a stack hit
   implies depth ≥ 1), but keep it anyway — belt-and-braces, zero cost.
4. The four function-boundary sites that save/zero/restore `loopDepth_`
   also save/clear/restore `labelStack_` (swap with an empty vector, swap
   back). A labeled break in a lambda naming an enclosing function's loop
   label must error with the not-found message, mirroring Track 02's
   closure-boundary rule (F1 item 4, P4).
5. `match` arms remain a non-boundary (Track 02 spec line 37-38): a labeled
   break in a match arm inside a labeled loop targets the loop. Falls out of
   doing nothing — match arms never touch `labelStack_`.

### F4 — Oracle (Eval) semantics

Track 02 models break/continue as frame-confined flags (`breaking_`,
`continuing_`, Eval.hpp:67-68) that every loop consumes at the innermost
level (the `if (continuing_) ... if (breaking_) ...` pairs at Eval.cpp
1042-43, 1049-50, 1063-64, 1071-72, 1082-83, 1095-96). Labels extend this
with **target pointers** that let a loop distinguish "mine" from "keep
unwinding":

1. `Eval.hpp`, beside the flags: `const Stmt* breakTarget_ = nullptr;`
   `const Stmt* continueTarget_ = nullptr;` (null = unlabeled = innermost).
2. `StmtKind::Break` (1004) / `Continue` (1007): also set
   `breakTarget_ = s->labelTarget;` / `continueTarget_ = s->labelTarget;`
   (null for unlabeled — no behavior change).
3. Every loop's consume-pair becomes the canonical pattern (shown for the
   general shape; each of the six sites keeps its surrounding structure):
   ```cpp
   exec(s->thenBranch.get());
   if (continuing_) {
       if (continueTarget_ && continueTarget_ != s) break;  // not mine: propagate
       continuing_ = false; continueTarget_ = nullptr;      // mine: next iteration
   }
   if (breaking_) {
       if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
       break;                                               // mine: clear+exit; else propagate
   }
   ```
   Propagation needs no new machinery: the C++ `break` leaves the flag set,
   the Block statement loop (976) and the `exec` top guard (958) already
   stop everything downstream on `breaking_ || continuing_`, and the next
   loop out runs the same pattern. A DoWhile propagating a foreign continue
   must exit **before** evaluating its condition — the pattern above does
   (the `break` fires before the `while (...)` test re-runs), matching the
   lowered semantics where the jump bypasses the inner cond entirely.
4. **Frame confinement:** the four call-frame sites that save/clear/restore
   `breaking_`/`continuing_` (Eval.cpp 228/238/243, 388/396/403, 411/419/431,
   448/457/464) save/clear/restore the two targets identically. The
   using-cleanup flag-neutral pattern (Eval.cpp:987-998) adds the targets to
   its save/restore set (`sBt`/`sCt` beside `sBrk`/`sCnt`) — a close() that
   throws during a labeled-break unwind abandons the labeled break exactly
   like an unlabeled one (Track 02's uniform "close-throw wins" rule,
   unchanged).
5. The using-registration Block logic (968-977) needs **no change**: it
   collects cleanups as statements execute and runs them on any flagged exit;
   a labeled break is just a flagged exit that stays flagged through more
   blocks than usual.

The P2-style audit from Track 02 (grep `exec(` in Eval.cpp) is re-run as a
probe (P2 below): the target pointers ride the exact flag lifecycle, so the
site list is the flag site list — but verify it against today's tree, not
Track 02's memory of it.

### F5 — Lowering: loop targets, and the using-cleanup interaction (the hard part)

**Current machinery (verified):** `LoopCtx { breakJumps, continueJumps,
usingsFloor }` (Lower.hpp:49), pushed per loop with
`usingsFloor = usings_.size()` (Lower.cpp:666/678/695/723/749). Break (765):
if `usings_.size() > loops_.back().usingsFloor`, the jump is recorded into
the innermost UsingCtx's `brkJumps` (the resource must close first);
otherwise straight into `loops_.back().breakJumps`. Each using's cleanup
group (429-504) emits at most one brk stub and one cnt stub, whose terminal
hop tests `i - 1 >= loops_.back().usingsFloor` to either chain outward into
`usings_[i-1]` or land in `loops_.back()`'s patch list (476-497).

**Why labels break this:** every clause above says `loops_.back()`. Both the
crossing-set test at the break site and the stub's terminal hop assume all
break traffic through a given using targets the innermost loop. A
`break outer` crossing the same using as a `break inner` needs (a) a longer
crossing set — every using down to *outer's* floor, including usings
declared in blocks between the two loops — and (b) a different terminal
patch list. One shared stub cannot serve two targets: its outgoing edge
differs. **Do not** solve this with a runtime "which target" register and a
dispatch in the stub — that adds IR shape, breaks the everything-is-a-Jump
property, and violates the spirit of Track 02's STOP condition on new
runtime mechanisms. Solve it structurally, the same way F3 solved
problem #7: more stubs, emitted lazily, only for traffic that exists.

**The generalization (normative): per-target exit chains.**

1. `Lower.hpp`: stamp the loop on its ctx, and key the using stub lists by
   target:
   ```cpp
   struct LoopCtx {
       std::vector<int> breakJumps, continueJumps;
       size_t usingsFloor = 0;
       const Stmt* stmt = nullptr;      // NEW: this loop's AST node
   };
   struct ExitChain {                    // NEW
       const Stmt* targetLoop = nullptr; // which loop this traffic exits to
       std::vector<int> jumps;           // pending hops INTO this stub
   };
   struct UsingCtx {
       ... // slotReg, closeDecl, rangeStart, retJumps, needRet unchanged
       std::vector<ExitChain> brkChains, cntChains;   // REPLACES brkJumps/
                                                      // cntJumps + needBrk/needCnt
   };
   ```
   (`retJumps`/`needRet` are untouched: a return crosses *all* active usings
   regardless of loops — it has exactly one target, the function exit, so the
   existing single chain is already fully general.)
2. Loop cases: `loops_.back().stmt = s;` beside the existing `usingsFloor`
   stamp. Save/clear/restore semantics around `lowerLambda`/`lowerPending`
   are already handled wholesale by `loops_.clear()` / `usings_.clear()`
   (Lower.cpp:289-290) — no new work.
3. **Break/Continue lowering** (765-785) — unified for labeled + unlabeled:
   ```cpp
   // resolve the target LoopCtx
   LoopCtx* target = nullptr;
   if (s->labelTarget) {
       for (auto& lc : loops_) if (lc.stmt == s->labelTarget) { target = &lc; break; }
       if (!target) { fail(s->span, "internal: labeled break target not on the loop stack"); break; }
   } else {
       if (loops_.empty()) { fail(s->span, "internal: 'break' outside a loop"); break; }
       target = &loops_.back();
   }
   // crossing set: every using at/above the TARGET's floor (not loops_.back()'s)
   if (usings_.size() > target->usingsFloor) {
       chainFor(usings_.back().brkChains, target->stmt).jumps.push_back(emit(Op::Jump, 0));
   } else {
       target->breakJumps.push_back(emit(Op::Jump, 0));
   }
   ```
   where `chainFor(vec, stmt)` finds-or-creates the ExitChain for that
   target. Continue is identical with `cntChains`/`continueJumps`. The
   target-loop pointer search is a ≤-depth linear scan of a tiny vector —
   fine. Note the one-line trap this design exists to name: the crossing-set
   test uses **`target->usingsFloor`**, never `loops_.back().usingsFloor`
   (problem #4).
4. **`lowerUsingCleanupGroups` (429-504)** — for each using `i` (reverse
   order, as today), after the fallthrough head, pad, and ret stub, emit one
   brk stub **per ExitChain in `brkChains`** and one cnt stub per entry in
   `cntChains`:
   ```cpp
   for (ExitChain& ch : ctx.brkChains) {
       int stubPc = (int)F().code.size();
       emitCloseCall(ctx.slotReg, ctx.closeDecl);
       for (int j : ch.jumps) F().code[j].a = stubPc;
       // terminal hop: does the NEXT-OUTER using also sit inside ch's target?
       LoopCtx* L = loopCtxFor(ch.targetLoop);          // must exist — see below
       if (i > 0 && i - 1 >= L->usingsFloor) {
           chainFor(usings_[i - 1].brkChains, ch.targetLoop)
               .jumps.push_back(emit(Op::Jump, 0));
       } else {
           L->breakJumps.push_back(emit(Op::Jump, 0));
       }
   }
   ```
   (cnt stubs identical with `cntChains`/`continueJumps`.) `loopCtxFor`
   scans `loops_` by stmt pointer and `fail()`s as an internal error on a
   miss. **Why the target is always still on `loops_`:** the group is
   emitted at the end of the block that declared the using; that block is
   (transitively) inside the target loop's body — the break's checker-
   validated lexical enclosure guarantees it — and blocks lower post-order,
   so the target loop's own case is still on the C++ stack and its LoopCtx
   still on `loops_`. This is the same post-order argument Track 02's
   implementation log used to justify `usingsFloor` (its log, "the loop is
   always still on `loops_` when its own crossing usings' stubs are
   emitted"). Cross-block chaining needs nothing new: hops recorded into
   `usings_[i-1]` where `i-1` is below the current watermark get patched
   when that enclosing block's groups emit, exactly like `retJumps` today
   (Lower.hpp:51-58 documents nesting order == vector order across blocks).
5. **Byte-identity for label-free programs (hard acceptance gate):** in a
   program with no labels, every recorded chain has
   `targetLoop == loops_.back().stmt` at record time, each using accumulates
   at most one brk chain and one cnt chain, stub emission order within a
   group is unchanged ([fallthrough, pad, ret, brk, cnt]), and the terminal
   condition `i-1 >= L->usingsFloor` evaluates identically to today's
   `i-1 >= loops_.back().usingsFloor`. The lowered IR is therefore
   byte-identical **by construction** — verified mechanically by `--ir`
   diffing the whole existing corpus before/after (Track 02 problem #2's
   assurance, reused verbatim; it is also a STOP condition).
6. **Correctness on the F3 edge table, labeled edition.** The cleanup-group
   invariant (techdesign-02 F3: a resource's cleanup code sits outside its
   own handler range, inside every enclosing one) is untouched — labeled brk
   stubs live in the same group region as today's stubs, so: a `close()`
   throwing inside a labeled-break stub lands in the *enclosing* resource's
   pad (never its own — no double-close), and the exception replaces the
   in-flight labeled break (uniform signal rule), byte-for-byte the same
   pad machinery. Labeled continue to a For/ForIn/DoWhile lands in that
   loop's `continueJumps` and inherits the correct continue point (step /
   increment / cond) from the existing per-kind patching (Lower.cpp
   671/681/698/726/752) with zero new decisions.

### F6 — Backends: LLVM primary, ELF under the freeze

**Zero backend work, by construction.** Labeled break/continue lowers to
`Op::Jump` instructions patched to loop-end / continue-point / stub pcs —
op codes the backends already consume:
- **LLVM (primary):** `Op::Jump` → `b.CreateBr(blocks[in.a])`
  (LlvmGen.cpp:1680-1682); every Jump target is already marked a block start
  by the jump-target map (LlvmGen.cpp:918-933), and handler pads are already
  jump targets via the handler scan (931-933). Nothing to add.
- **IrInterp / CGen:** same story — pc-indexed jumps, per-range handlers
  (dispatch facts tabulated in techdesign-02's F3 engine table, all verified
  2026-07-06).
- **ELF (X64Gen) — FROZEN:** no new X64Gen.cpp/X64.hpp work exists to
  propose, and none is proposed. The feature reaches ELF for free through
  existing ops. Policy for divergence: if the labeled corpus diverges on ELF
  only, that is by definition a pre-existing bug in an existing op's ELF
  implementation — file it in `/bug.md` with the repro, mark the specific
  corpus program ELF-SKIP with an explicit diagnostic (the overview §4.3
  rule: "deferred items need an explicit diagnostic, never silence"), and do
  not touch the frozen files. The feature's acceptance does not gate on ELF
  beyond running the suite.

### F7 — Tests & corpus (`.lev`, and the runner gap)

- New corpus programs are **`.lev`** (hard rule; the Track 02-era `.ext`
  files stay as they are). Grounded gap: all three runners glob `*.ext` only
  (`run_corpus.sh:5`, `run_elf.sh:7`, `run_native.sh:9`) and no
  `tests/corpus/*.lev` exists yet — so this track makes the minimal runner
  change: each loop gains the second glob
  (`for f in "$dir"/*.ext "$dir"/*.lev; do [ -e "$f" ] || continue; ...`)
  with the `.expected`/`.stdin` stem derivation handled per-extension.
  This is test infrastructure, not `src/`, and it unblocks every future
  track's `.lev` corpus files; keep it a separate commit so it can be
  cherry-picked.
- `tests/corpus/labeled_loops.lev` (+ `.expected`): labeled break/continue
  from all four loop kinds; label targeting outer/middle of 3-deep nesting;
  labeled break/continue from inside `if`, `match` arms, and `try`/`catch`
  bodies inside labeled loops; sibling label reuse; unlabeled forms mixed in
  (regression). All five engines.
- `tests/corpus/labeled_using.lev`: the hard cases —
  (a) `break outer` crossing one using; (b) crossing two usings in one
  block; (c) crossing usings in *different* blocks (one inside the inner
  loop, one between the loops); (d) `continue outer` versions of a-c;
  (e) same using crossed by `break inner` AND `break outer` (two chains,
  one resource); (f) close() throwing during a labeled-break unwind with an
  enclosing catch (signal abandoned, exception wins); (g) labeled break
  where an unlabeled break also crosses (chain coexistence). Close-order
  assertions via prints in close().
- `tests/corpus/churn/labeled_using_churn.lev`: labeled-break-crossing-using
  in a hot loop, +0B at N=100/800 (the F3 churn convention; construct via a
  factory function per the bug.md #16 convention, overview §2.1d).
- `tests/test_checker.cpp`: ERRORS — labeled break with unknown label;
  label on enclosing-shadowed loop (duplicate); labeled break inside a
  lambda naming an outer function's label; labeled continue outside any
  loop. CLEAN — sibling label reuse; label == variable name; labeled break
  in a match arm.
- Parser golden tests: labeled loop prints ` label=` (AstPrinter);
  `foo: return;` is a parse error (label guard requires a loop keyword);
  `a: b: while` is a parse error (see problem #2).
- Rule-injection test (test_meta or checker tests): a rule template
  containing a labeled loop + labeled break survives injection (the
  cloneStmt copy-through).

## 4. Dependencies

- **Track 02 landed** (it is; this design reads its as-built code, not its
  rev-2 plan — the `usingsFloor` field only exists in the tree).
- **Merge-order rule (overview §2.1):** this touches Parser/Ast/Checker/
  Eval/Lower — the serialized compiler-file family. Track 03 owns the same
  family and is currently STOPped on the statics ruling (memory:
  `/this_bug.mg`, commit affa8a4). Before implementation starts, confirm
  with the owner whether 03 resumes first; if this lands first, note in the
  log that 03 must rebase (the surface here is small: one Stmt field, no
  enum appends, so the rebase is near-trivial either way).
- No prelude, no natives, no IR, no runtime changes. No new git branches
  (three-branch rule): implement on an existing agent branch, push to master
  per the guardrail.

## 5. P-probes (run before any code)

- **P1 — dead-grammar probe:** confirm against `build/lang` that
  (a) `x: while (true) {}` at statement position is a parse error today,
  (b) `break x;` is a parse error today, and (c) grep all `.ext`/`.lev`
  sources repo-wide (tests, examples, prelude in Resolver.cpp) for any
  statement-initial `identifier :` sequence — expected: none outside
  comments/ternaries. If (c) finds a live one → the guard needs revisiting
  (problem #1) → STOP.
- **P2 — Eval flag-site audit, re-run:** `grep -n 'breaking_\|continuing_'
  src/Eval.cpp src/Eval.hpp` and confirm the site list matches F4's plan
  (six loop consume-pairs, guard 958, block loop 976, four frame sites,
  flag-neutral close 987-998). Any extra site that appeared since Track 02
  gets the target treatment too.
- **P3 — `--ir` baseline:** dump `--ir` for the entire existing corpus
  (esp. `loops.ext`, `using.ext`, both churn files) to a scratch dir BEFORE
  branching into Lower changes; this is the byte-identity oracle for F5
  item 5.
- **P4 — closure-boundary probe (labeled):** after M1, a lambda inside
  `outer: while` containing `break outer;` must error; the same lambda with
  its own `inner: while (..) { break inner; }` must compile.

## 6. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Statement-initial `ident :` grammar collision** — some current or future form (ternary in an ExprStmt, `$for` splice, map-ish literal) parses `ident :` at statement start and the label guard steals it. | Grounded as dead grammar today (F1 analysis: Colon has no infix power, ternary colon only follows `?`, `::` is one token; P1 confirms empirically). The guard requires **three** tokens (`Ident, Colon, loop-kw`), so even a future `ident :`-initial form only collides when followed by a loop keyword. P1 failure = STOP. |
| 2 | **Label scoping/shadowing semantics** — inner loop reusing an enclosing label makes `break L` ambiguous to readers even if "innermost wins" is well-defined; multi-labels (`a: b: while`) add grammar for near-zero value. | Pin Java's rules: duplicate-of-enclosing = compile error ("label 'L' is already used by an enclosing loop"); sibling reuse legal (stack pop order gives it for free); one label per loop, `a: b: while` is a parse error by the 3-token guard (peek(2)=Identifier, not a loop kw — falls to expression parsing and errors; acceptable message, documented). Labels are a separate namespace from values — no interaction, no checker work. |
| 3 | **Labeled `continue` vs `break` semantics divergence per loop kind** — continue-to-outer must land on the *outer* loop's step/cond/increment, not its top, and must abandon inner loops without running their steps. | By construction: the jump lands in the target's `continueJumps` and inherits that loop's existing per-kind continue-point patch (While→top, DoWhile→contPos, For→stepPos, ForIn→incPos; Lower.cpp:671/681/698/726/752). Inner loops are simply jumped out of — same as unlabeled break skipping the For step today (techdesign-02 F1 item 5, "break skips the step — correct"). Oracle mirrors it via target-pointer propagation (F4 pattern; the DoWhile propagate-before-cond ordering is called out). Corpus asserts iteration counts per loop kind. |
| 4 | **Cleanup-stub selection for multi-level exit** — the hard part: one using crossed by breaks with different target loops; today's single stub + `loops_.back()` terminal is wrong for all but the innermost; and the break-site crossing test using the wrong floor silently *skips closes* (resource leak / missed close) or *over-closes* (close on a loop exit that shouldn't). | The per-target ExitChain generalization (F5 items 1-4) — one stub per (using, target) actually used, terminal hop re-tested per chain against the **target's** `usingsFloor`. The two one-line traps are named explicitly: break-site test uses `target->usingsFloor` (F5 item 3); stub terminal uses `L->usingsFloor` for the chain's own target (F5 item 4). `labeled_using.lev` case (e) exercises two chains through one resource; close-order prints + churn +0B are the nets. |
| 5 | **Target LoopCtx not on `loops_` at stub-emission time** → `loopCtxFor` misses and the compile fails internally. | Argued impossible by post-order (F5 item 4): checker-validated lexical enclosure ⇒ the using's block is inside the target loop's body ⇒ the target's case is still live when the block's groups emit. Guarded by `fail()` as an internal error, and its firing on any corpus program is a STOP condition (it would mean the enclosure argument has a hole, e.g. some future construct lowering bodies out-of-line). |
| 6 | **Byte-identity regression for label-free programs** — the LoopCtx/UsingCtx refactor accidentally changes emission order or stub layout, breaking Track 02's "byte-identical for using-free/label-free programs" property and invalidating five-engine baselines. | P3's `--ir` baseline diff over the whole existing corpus is a hard acceptance gate AND a STOP condition (any diff on a label-free program = design violated, stop and re-derive). The refactor is shape-preserving on purpose: same stub order within groups, chains degenerate to today's single lists. |
| 7 | **Eval target-propagation leaks across frames or through close()** — a labeled break in a callee "continues" a caller's loop, or a close() during unwind resurrects/mangles the pending target. | Targets ride the existing flag lifecycle exactly: the four frame save/clear/restore sites and the flag-neutral close pattern each gain the two pointers (F4 item 4). Checker prevents cross-frame labels statically (F3 item 4); the Eval handling is belt-and-braces, same doctrine as Track 02. Differential testing is the net: Lower's implementation is independent, so a missed site diverges oracle-vs-IR and fails the corpus run. |
| 8 | **`cloneStmt` drops `label`** (the recurring Track 01/02 bug class) or, worse, a future sweep copies `labelTarget` and clones dangle into template trees. | `out->label = s->label` added with the explicit do-NOT-copy comment for `labelTarget` and the rationale (F2); rule-injection test in F7 pins it. This is the third consecutive track to hit this file — the comment is the institutional memory. |
| 9 | **ELF lane under the freeze** — labeled corpus diverges on ELF only (e.g. an existing jump/handler edge case in the frozen backend), tempting an X64Gen fix. | Never touch X64Gen.cpp/X64.hpp. The feature uses only existing ops, so an ELF-only divergence is a pre-existing backend bug: file to `/bug.md` with a minimal repro, ELF-SKIP the specific corpus program **with a printed diagnostic** (overview §4.3: never silence), acceptance proceeds on oracle/IR/CGen/LLVM. Precedent: Track 04's pattern, blessed in overview §2.1's stale-matrix note. |
| 10 | **Rule-template hygiene for labels** — a template with label `L` injected into user code that already sits inside an enclosing loop labeled `L` trips the duplicate-label error at the injection site, confusing the rule author. | Accept for now: it is the same class as other template-name collisions (Var hygiene renames exist, label hygiene does not). The error span points at the injected loop; document in the rules notes. If it ever bites for real, the fix is mechanical (extend `renames_` to labels) — logged as a known sharp edge, not built speculatively. |
| 11 | **Stub bloat** — K targets × M usings × 2 kinds could bloat hot functions. | Chains are lazy (only traffic that exists gets a stub — the `need*` discipline generalized), and real code has K ≤ 2 in practice. Each stub is a close-call + one Jump. Measure via the `--ir` dumps on `labeled_using.lev`; no budget mechanism needed at this scale. |

## 7. Milestones, timeline & acceptance

| M | deliverable | accept |
|---|---|---|
| M0 | Probes P1-P3 run + logged; owner ping on Track 03 serialization (§4) | P1 clean (dead grammar confirmed), P3 baseline stored |
| M1 | Front end: parser guard + break/continue label parse, `Stmt::label`/`labelTarget`, cloneStmt copy-through, AstPrinter, checker label stack + errors + 4 boundary resets; checker/parser tests | build clean; new checker ERRORS/CLEAN cases green; parser goldens; P4 probe passes |
| M2 | Semantics: Eval target pointers (6 loop sites, 4 frame sites, close pattern); Lower per-target chains (LoopCtx.stmt, ExitChain, break/continue rewrite, `lowerUsingCleanupGroups` per-chain stubs); runner `.lev` glob; `labeled_loops.lev` | **P3 `--ir` diff: zero changes on the entire existing corpus** (hard gate); labeled_loops green on all five engines (oracle/IR/CGen/ELF/LLVM byte-identical diff, not just pass/fail) |
| M3 | The using interaction: `labeled_using.lev` (cases a-g), `labeled_using_churn.lev` +0B; reference-doc duty (§8); full suite | ctest full count green; run_corpus/run_elf/run_native green (ELF per problem #9 policy if needed); churn flat at N=100/800; close-order prints exact |

Target: **3 days of implementation** once serialization with Track 03 is
resolved — M0+M1 one day, M2 one day, M3 one day. Given 03's STOP is
unresolved as of 2026-07-06, pencil **Jul 8 – Jul 11**; if 03 resumes first,
this re-slots after 03's compiler edits land and rebases (expected trivial —
no enum appends). Log either way.

## 8. Reference-doc duty

Same-commit rule (overview §2.2): `docs/reference.md` §5.2 (break/continue
prose gains the labeled forms, label rules: one per loop, sibling reuse OK,
enclosing-duplicate error, closure boundary; a labeled-break-out-of-usings
example), the overview §5 deferred list gets "labeled break/continue (02)"
struck with a pointer here, and this file moves to `designs/complete/` on
landing. `info.md` statement-list touch-up if it enumerates break/continue
forms.

## 9. STOP conditions

- **P1 finds live `ident :` statement grammar** anywhere in the tree —
  syntax needs an owner decision, do not improvise an alternative.
- **P3 `--ir` diff shows ANY instruction change for a label-free program**
  at any point during M2/M3 — the shape-preserving refactor failed; stop
  and re-derive rather than hand-patching emission order.
- **`loopCtxFor` internal error fires on any corpus program** — the
  post-order enclosure argument (F5 item 4 / problem #5) has a hole; the
  design needs correction, not a workaround.
- **Any temptation** to add a new IR op, a runtime target-dispatch register
  in cleanup stubs, or a new cross-frame Eval flag beyond the two
  frame-confined target pointers.
- **Any temptation to edit X64Gen.cpp/X64.hpp** for an ELF-only divergence
  (problem #9's filing path exists precisely so this never happens).
- Track 03 resumes mid-implementation with conflicting compiler-file edits —
  pause, sync with the owner on merge order, rebase; never develop the two
  concurrently against the same base (overview §2.1).

## 10. Implementation log

*(empty — filled by the implementing agent per overview §4.2)*
