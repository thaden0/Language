# Track 02 — Control Flow: `break` / `continue`, `do-while`, `using`

**Status: DONE 2026-07-06.** M1 (break/continue), M2 (do-while), and M3 (`using`)
all landed in one pass, rev 2's design followed with one derived addition
(`LoopCtx::usingsFloor` — see the Implementation Log). 41/41 ctest passed; all five
engines verified byte-identical on every new corpus program. Two pre-existing,
unrelated bugs found and filed (bug.md #16, #17), not fixed (out of this track's
owned regions). **Date:** 2026-07-05. **Depends on:** Track 01 landing (landed —
designs/complete/techdesign-01).
**Source:** suggested-features.md §4.1, §4.2, §4.4.
**Owns (regions):** `Token.hpp/cpp` (append), `Parser.cpp` statement region
(`parseStatement` 706, `parseBlock` 631), `Ast.hpp` `StmtKind` (182) + `Stmt` (246),
`Checker.cpp` statement region (While at 1308), `Eval.cpp` statement exec (While 970,
ForIn 974, For 1004), `Lower.cpp` statement lowering (While 522, For 531, ForIn 544).
**No IR changes** (`Op::Jump`/`JumpIfFalse`/`JumpIfTrue`, Ir.hpp:36-38, suffice) — so
IrInterp, CGen, X64Gen, LLVM are covered **for free** via lowering for F1/F2; the
oracle (Eval) walks the AST and needs its own handling. F3 (`using`) desugars entirely
into existing ops (calls + jumps + handler-table entries), so the backends stay free
there too — but the *layout* of that desugar is load-bearing (see F3).

---

## 1. Features

### F1 — `break;` and `continue;`

**Spec:**
- `break;` exits the nearest enclosing loop (`while`, C-style `for`, `for..in`,
  `do-while`). `continue;` skips to the next iteration: in `while`/`do-while`, to
  the condition; in `for`, to the **step** (then condition); in `for..in`, to the
  next element.
- Outside any loop → compile error ("'break' outside a loop"), checked in the
  Checker with a loop-depth counter (a lambda/method body boundary **resets** the
  counter — a `break` inside a closure does not exit the enclosing function's
  loop; it is an error unless the closure body has its own loop).
- Unlabeled only. Labeled forms: **explicitly deferred** (roadmap, overview §5).
- Interaction with `match`: `match` is not a loop; `break` inside a match arm
  inside a loop breaks the loop (no C-switch legacy).

**Implementation:**
1. Tokens `KwBreak`, `KwContinue` (Token.hpp append after KwFalse; Token.cpp
   keyword map at 96 + `toString` switch). **P1 ran clean** (2026-07-06): all four
   words (`break`/`continue`/`do`/`using`) appear in `.ext` sources and the
   prelude only inside comments — so all four land as **hard keywords**; the
   contextual-`do` fallback (problem #6) is not needed.
2. `Ast.hpp`: `StmtKind::Break`, `StmtKind::Continue` (append inside the
   executable group at 182-189).
3. `Parser.cpp:706 parseStatement`: two trivial cases (keyword + `;`).
4. `Checker.cpp`: `int loopDepth_ = 0;` (Checker.hpp beside `returnType_`) —
   increment around While/DoWhile/For/ForIn **body** checks only (cond/init/step
   check at outer depth). Save/restore to 0 at every function-body boundary; the
   complete list (each already saves/restores `returnType_` — put the loop-depth
   save in the same lines): `checkFunction` (Checker.cpp:1480), the Bind factory
   body in `check()` (1362-1378) **and** in `walk()` (1513-1521), and
   `ExprKind::Lambda` in `typeOfInner` (518-536). `match` arms are NOT a boundary
   (spec: break in a match arm inside a loop breaks the loop). Error on
   Break/Continue at depth 0: `"'break' outside a loop"` /
   `"'continue' outside a loop"`.
5. `Eval.cpp` (oracle): add `bool breaking_ = false, continuing_ = false;` beside
   `returning_`/`throwing_` (Eval.hpp:62-67). Semantics:
   - `StmtKind::Break` → `breaking_ = true`. `Continue` → `continuing_ = true`.
   - `exec()` top guard (934) and the Block statement loop (939) both gain
     `|| breaking_ || continuing_`.
   - The **complete** audit of statement-sequencing guard sites (P2 ran
     2026-07-06; `grep -n 'exec(' src/Eval.cpp`): the only `exec` call sites are
     the four call frames (231, 384, 405, 435 — see flag confinement below), the
     Match-arm body at **803** (verified: `exec(arm.bodyBlock); return vvoid();`
     — returns immediately, flags propagate correctly, no edit needed), and the
     statement machinery itself (934-1030). Expression-evaluation code never
     sequences statements otherwise, and break/continue cannot occur
     mid-expression. Sites needing edits are exactly: 934, 939, and the four
     loops below.
   - While (970) — replace with an explicit sequence:
     ```cpp
     case StmtKind::While:
         while (!returning_ && !throwing_) {
             if (!eval(s->expr.get()).b || throwing_) break;
             exec(s->thenBranch.get());
             if (continuing_) continuing_ = false;
             if (breaking_) { breaking_ = false; break; }
         }
         break;
     ```
   - For (1004): keep the C++ `for` shape; inside the loop body, after
     `exec(s->thenBranch.get())`: `if (continuing_) continuing_ = false;` (the
     C++ for-step then runs the language step + cond — correct), then
     `if (breaking_) { breaking_ = false; break; }` (break skips the step —
     correct).
   - ForIn (974): all three per-kind loops (Range 981, Array 986, Map 994) get
     the same two lines after their `exec(s->thenBranch.get())`.
   - DoWhile (F2): see F2.
   - **Flags never cross a call**: the four frame sites that save/clear/restore
     `returning_` — `runCtor` (227-234), `callFunction` (378-388), `callClosure`
     (395-413), `callPrimMethod` (429-440) — save/clear/restore
     `breaking_`/`continuing_` identically. Checker prevents crossing statically;
     this is belt-and-braces.
6. `Lower.cpp`: a loop-context stack. **Correction to rev 1:** no For
   restructure is needed — today's For layout is already `top: cond; JumpIfFalse
   end; body; step; Jump top` (Lower.cpp:531-542), i.e. the step already sits
   after the body at a markable pc. Continue only needs the record-and-patch
   idiom, never a re-ordering, so problem #2's byte-identical-IR requirement is
   satisfied by construction for all loop kinds. Uniform shape:
   ```cpp
   struct LoopCtx { std::vector<int> breakJumps, continueJumps; };
   std::vector<LoopCtx> loops_;
   ```
   Always collect; patch per loop kind once the target pc is known (the
   `F().code[j].a = ...` idiom the If/While cases already use):
   - While (522): push ctx; lower as today; patch `continueJumps` → `top`,
     `breakJumps` → end (where `jEnd` is patched).
   - For (531): patch `continueJumps` → the step's first pc (mark it right after
     `lowerStmt(s->thenBranch...)`); `breakJumps` → end. A condition-less `for`
     (`s->expr` null) still patches break → end (pc after `Jump top`).
   - ForIn (544): both forms — mark the increment start (the `LoadConst one`
     emission right after the body); `continueJumps` → there; `breakJumps` → end.
   - DoWhile: see F2.
   - Nesting: push/pop per loop statement; Break/Continue lower to
     `emit(Op::Jump, 0)` recorded in `loops_.back()` (subject to the F3 stub-chain
     interception when active `using` scopes sit between the statement and the
     loop — see F3; with no usings the jump is recorded directly, byte-identical
     to a using-free compiler). Empty `loops_` at a Break/Continue in Lower =
     internal error (`fail()`); the checker already rejected it.
   - **Nested-function lowering**: `loops_` (and F3's `usings_` +
     `chainRetReg_`) must be saved/cleared/restored around `lowerLambda` and
     `lowerPending` bodies, the same way `freshStructRegs_` already is — a
     lambda inside a loop must not see the enclosing function's loop stack
     (P4's compile-side guarantee).

### F2 — `do stmt while (cond);`

- Token `KwDo` (hard keyword — P1 clean); `StmtKind::DoWhile`; `Stmt` reuses
  `expr` (cond) + `thenBranch` (body) like While (Ast.hpp:322 comment block).
- Parser: `do` + parseStatement (body-is-one-statement applies) + `expect KwWhile`
  + `(` expr `)` + `;`. No ambiguity: the body statement is parsed fully before
  the `while` is expected, so `do while (a) s; while (b);` parses as an inner
  while-body — unambiguous by construction.
- Eval:
  ```cpp
  case StmtKind::DoWhile:
      do {
          exec(s->thenBranch.get());
          if (continuing_) continuing_ = false;
          if (breaking_) { breaking_ = false; break; }
      } while (!returning_ && !throwing_ && eval(s->expr.get()).b);
      break;
  ```
  (continue falls through to the cond test — jumps to the cond, not the body top.)
- Lower: `top:` body; `contPos:` cond; `JumpIfTrue top`; end. Patch
  `continueJumps` → `contPos`, `breakJumps` → end. Pure F1 ctx machinery.
- Checker: mirror the While case (Checker.cpp:1308) minus the narrowing-facts
  application — the body runs *before* the first cond test, so cond facts must
  NOT narrow the body. Exactly: `check(body)` (with loop-depth++ around it), then
  `typeOf(cond)`. (While performs no explicit is-bool enforcement today; DoWhile
  deliberately matches While's exact strictness rather than inventing more.)
- **Traversal checklist** — every switch that has a `case StmtKind::While` gains
  `DoWhile` beside it, mirroring While's treatment in that file exactly
  (verified list, 2026-07-06): AstPrinter.cpp:269, Checker.cpp:1308,
  Project.cpp:464 (`isExecutable` in `validateEntry`), Lower.cpp:154
  (`collectBinds` body walk), Lower.cpp:522, Eval.cpp:970, Resolver.cpp:1230
  (scope walk), Rules.cpp:261 (rule-engine template walk), plus the Parser case.
  `Break`/`Continue` have no names or children: they need cases only in the
  Parser, Checker (the depth-0 error), Eval, Lower, and AstPrinter; every other
  walker's `default:` already ignores them correctly.
  (Observation, pre-existing and untouched by this track: Eval's top-level run
  loop at Eval.cpp:1078 executes only ExprStmt/Var/Try/Throw items, so a
  top-level `while` never runs in the oracle even though Project.cpp:464 calls it
  executable. DoWhile mirrors While in both places; the discrepancy itself is
  logged for the owner, not fixed here.)

### F3 — `using` declarations (deterministic resource cleanup)

**Spec (v1, deliberately narrow):**
```
interface IDisposable { void close(); }        // prelude addition: in `namespace std`
                                               // beside IException (Resolver.cpp:755);
                                               // `class File` gains `, IDisposable`
                                               // in its base list (its void close()
                                               // at Resolver.cpp:734 already conforms)

using File f = File(path, std::write);          // modifier on a local declaration
... // f.close() runs when the enclosing BLOCK exits: fallthrough, return,
    // throw-unwind, break/continue leaving the block
```
- `using` is a declaration modifier (like `const`; composes: `using const File f`).
- The declared type must implement `IDisposable` (checker error otherwise).
- Cleanup order: reverse declaration order within the block.
- `close()` throwing during *exceptional* unwind: the new exception **replaces**
  the in-flight one (documented; matching the simplest coherent rule — C#'s
  behavior). During normal exit it propagates normally.
- v1 restriction: allowed on **locals only** (not fields, not globals, not
  for-in bindings) — the block-scoped case is 95% of the need (sockets/files in
  handlers) and avoids object-lifetime entanglement with ARC.

**Why this is the hard feature.** The language has no `finally` and the design
must honor every block-exit edge:

| exit edge | mechanism (rev 2) |
|---|---|
| fallthrough | the resource's **cleanup group** at block end (see below) |
| `return` | jump to the resource's return stub in its cleanup group; stubs chain inner→outer and the outermost ends in `Ret`/`RetVoid` |
| `throw` / unwind | per-range handler tables (P3 **confirmed**: `Handler{start,end,handlerPc,bindReg,type}`, Ir.hpp:90, per-range not per-function — problem #5's fallback is dead). One implicit handler per `using`, landing pad = the pad stub in its cleanup group: close then rethrow |
| `break`/`continue` crossing the block | jump to the resource's break/continue stub in its cleanup group; stubs chain outward and the last hop lands in the F1 loop-ctx patch lists |

**The rev-1 design had a latent double-close bug here (problem #8).** Rev 1 said:
emit `close()` calls *inline* before `Ret` (return edge) and before the loop jump
(break/continue edges), plus a handler region covering the block for the throw
edge. But handler ranges are pure pc ranges — an inline `close()` emitted at a
return site sits *inside the resource's own handler range*, so a `close()` that
throws during a return is caught by that resource's own landing pad, which calls
`close()` a **second time** on the already-closed resource before rethrowing.
Nested usings had the analogous wrong-handler capture. The fix is structural:
**a resource's cleanup code must never be located inside that resource's own
handler range, but must remain inside every enclosing resource's range.** That
ordering requirement is what the cleanup-group layout below guarantees.

**The cleanup-group layout (normative).** At the end of each block, for each of
its `using` declarations **in reverse declaration order**, the lowerer emits one
*cleanup group*; the resource's handler range **ends exactly where its own group
begins**. For `{ using A a = ...; S1; using B b = ...; S2; }`:

```
        decl a                    ; construct + bind
aStart:                           ; a's handler range opens (pc AFTER init —
        S1                        ;   an init that throws never closes a)
        decl b
bStart:                           ; b's range opens
        S2
bGroup:                           ; b's range ENDS here: [bStart, bGroup)
        close b                   ; fallthrough head
        Jump aGroup               ;   (forward-patched)
  bPad: close b ; Throw bind_b    ; landing pad: close, rethrow
  bRet: close b ; Jump aRet       ; return stub  (emitted only if used)
  bBrk: close b ; Jump ->chain    ; break stub   (emitted only if used)
  bCnt: close b ; Jump ->chain    ; continue stub (emitted only if used)
aGroup:                           ; a's range ENDS here: [aStart, aGroup)
        close a                   ; fallthrough head (patch target of bGroup's Jump)
        Jump out
  aPad: close a ; Throw bind_a
  aRet: close a ; Ret retReg      ; outermost-in-function: real Ret/RetVoid
  ...
out:
```

Why this is correct on every edge — the invariant to keep in mind while
implementing: *b's entire group lies inside a's range but outside b's own; a's
group lies outside both, but inside any enclosing resource's/user-try's range.*
- b's fallthrough/return/pad `close b` throws → pc ∈ a's range → a's pad closes
  a and rethrows → propagates. No double-close anywhere: no pc in b's group is
  in b's range.
- a's `close a` throws during *normal* exit → pc outside a's range → propagates
  to enclosing handlers (user `try` around the block catches it) — the spec's
  "during normal exit it propagates normally".
- a throw inside S2 → engines pick b's handler first (see engine table below),
  b's pad rethrows at a pc inside a's range → a's pad → rethrow escapes. Reverse
  declaration order falls out of handler *vector order*, not of any new rule.
- A `close()` throw during an exceptional unwind **replaces** the in-flight
  exception with no further ceremony: the pad rethrows `bind` only if the close
  call returned; if the close call itself threw, the new exception is already
  the pending one and dispatch continues from the pad's pc — outside the own
  range, inside enclosing ranges. Exactly the documented C#-style rule.
- **Uniform signal rule (pin this in reference §5): an exception thrown by
  `close()` wins over any in-flight control signal.** A return/break/continue
  travelling through a stub whose `close()` throws is abandoned; the exception
  propagates from that pc. The oracle must implement the same rule (below).

**Engine facts the layout depends on (all four verified 2026-07-06):**
| engine | dispatch | type test | null `h.type` |
|---|---|---|---|
| IrInterp.cpp:417-433 | first match in `fn.handlers` order, range test first | `isSubclassOrSelf` | **never matches** |
| CGen.cpp:400-414 | same | `issub` | `clsId 0` (undefined-ish) |
| X64Gen.cpp:3216-3242 | same | `issub` call | catch-all (skips test) |
| LlvmGen.cpp:1086-1108 | same | `rtIssub` | catch-all |
The null-type behaviors **diverge** — therefore a using handler's `type` is
always the **`IException` interface symbol** (`sema_.global->lookup`), never
null. Checker guarantees every thrown value implements IException, so IException
is a de-facto catch-all with identical behavior on all four engines. Handler
push order: a `using` handler is pushed at its group emission (block end), so
same-block usings push later-declared-first (innermost wins ✓); a `try` *inside*
the block pushed its handlers earlier (inner-try wins for its range ✓); an
enclosing `try` pushes after the whole block (using wins inside its range ✓).
First-match-in-order gives the right nesting with zero new machinery.

**Implementation shape (rev 2):**
1. **AST/Parser:** `using` is a hard keyword (`KwUsing` — P1 clean), accepted in
   `parseStatement` **only** (a top-level `using` is a parse error — top-level
   Vars are promoted to globals per bug.md #2, and v1 forbids using-globals).
   No new StmtKind: like `const`, `using` is a declaration modifier — parse
   `using` (+ optional `const`) + the parseVarDecl tail, producing a
   `StmtKind::Var` with **`bool isUsing`** (new flag beside `isConst`,
   Ast.hpp:289) and **`isConst` forced true** (the "may not be reassigned" rule
   rides the whole existing const machinery for free: `constBlockedWrite`,
   compound assignment, and the "needs an initializer" error at
   Checker.cpp:1275/1565 all fire without a line of new code). Also add
   `const Stmt* usingClose = nullptr;` to `Stmt` (the checker-resolved close
   decl — same precedent as `Expr::resolved`).
2. **Checker:** on a `Var` with `isUsing`:
   - **placement**: legal only as a *direct child of a Block* inside a function/
     lambda/ctor body. Track with a `bool usingOkHere_`-style flag: `check()`
     reads-then-clears it at entry; the Block case sets it before each direct
     child. A using as a bare loop body (`while (c) using File f = ...;`) or in
     any non-block position errors: `"a 'using' declaration must be a direct
     statement of a block"`. (Rationale: "the enclosing block" must exist and be
     the obvious cleanup scope; a one-statement body scope is a trap.)
   - **type**: the binding's static type must be a non-value class or interface
     implementing `IDisposable` (`isSubclass` against
     `scope_->lookup("IDisposable")`). Value structs are **rejected in v1**
     (`"v1: 'using' requires a reference type implementing IDisposable"`) — a
     close() mutating a struct copy poses ownership questions v1 does not need.
   - **resolve close now**: `methodOverloads(cls, "close")` + `pickOverload`
     with zero args; stash the chosen decl on `s->usingClose`. Error if absent
     (can't happen if the interface check passed, but refuse-to-guess).
3. **Eval (oracle):** in the Block case (Eval.cpp:937-941), collect
   `(Value, Symbol* cls, const Stmt* closeDecl)` for each `isUsing` Var **as it
   executes** (an S1 throw before `using b` must not close b — registration is
   acquisition). After the statement loop, **before** `env_.pop_back()`, run the
   list in reverse. No C++ RAII needed — the interpreter models all unwinding
   with flags, so straight-line code after the loop covers every edge. Each
   close call uses the **flag-neutral call pattern**:
   ```cpp
   // save ALL signals, run close in a clean context, then:
   //   close threw  -> keep the new exception, DROP the saved signals
   //   close normal -> restore the saved signals
   bool sRet = returning_, sBrk = breaking_, sCnt = continuing_, sThr = throwing_;
   Value sRv = returnValue_, sTv = thrownValue_;
   returning_ = breaking_ = continuing_ = throwing_ = false;
   /* call close: same dispatch as an explicit f.close() member call */
   if (!throwing_) {
       returning_ = sRet; breaking_ = sBrk; continuing_ = sCnt;
       throwing_ = sThr; returnValue_ = sRv; thrownValue_ = sTv;
   }
   ```
   (that conditional restore IS the uniform signal rule + the replace rule.)
   Skip the whole cleanup when `budgetExhausted_` (comptime abort is
   uncatchable and does not run user code). Dispatch: exactly what an explicit
   `f.close()` does in Eval (the ordinary member-call machinery) so oracle and
   IR can only diverge where ordinary calls already would.
4. **Lower:** a `std::vector<UsingCtx> usings_` beside `loops_`:
   ```cpp
   struct UsingCtx {
       int slotReg;                    // the bound local's register
       const Stmt* closeDecl;          // s->usingClose (for Inst::decl)
       int rangeStart;                 // pc after init (handler range open)
       size_t loopsDepthAtCreation;    // loops_.size() when pushed
       std::vector<int> retJumps, brkJumps, cntJumps;  // pending hops INTO my stubs
       bool needRet=false, needBrk=false, needCnt=false;
   };
   ```
   - **Var (isUsing)**: lower the init as today, then push a UsingCtx
     (`rangeStart = F().code.size()`).
   - **Block**: watermark `usings_.size()` at entry; after lowering children,
     emit the cleanup groups for entries above the watermark in reverse order,
     push their `Handler`s (range `[rangeStart, ownGroupStartPc)`, type =
     IException, fresh `bindReg` — engines already handle catch-in-loop rebind
     release on that reg), patch the fallthrough chain and every pending hop
     list, then truncate `usings_`. Emit ret/brk/cnt stubs only when their
     `need*` flag is set (a group with no crossing traffic is just
     `close; Jump next` + pad).
   - **close emission** (`emitCloseCall(slotReg, closeDecl)` helper): the exact
     shape an explicit `f.close()` takes through lowerCall's member path —
     `base = F().nregs; Move newReg() <- slotReg; dst = newReg();
     emit(Op::CallDyn, dst, 0, base, 1); last().sname = "close";
     last().decl = closeDecl;` (CallDyn-by-name is what a member call emits —
     Lower.cpp:1363 is the nullary template — so dynamic dispatch, ARC
     classification via `Inst::decl` at Lower.cpp:648-649, and engine behavior
     are all identical to user-written `f.close()`; works for interface-typed
     slots whose decl has no byDecl entry).
   - **Return**: if `usings_` is empty for the current function — **unchanged
     code path** (byte-identical for using-free programs). Otherwise: compute
     the value at the return site exactly as today (including the CopyVal
     value-struct dance + maybeVFree), `Move` it into a lazily-allocated
     per-function `chainRetReg_`, and record a `Jump` in the **innermost**
     UsingCtx's `retJumps` (+ set `needRet` on it). Each group's ret stub closes
     its own resource then records a `Jump` into the next-outer UsingCtx's
     `retJumps`; the ret stub of the function's outermost using emits the real
     `Ret chainRetReg_` (or `RetVoid` for void functions — a function mixes the
     two only as today's rules allow).
   - **Break/Continue**: the crossing set is every active UsingCtx with
     `loopsDepthAtCreation >= loops_.size()` (created inside the innermost
     loop). All such usings' stub traffic provably targets the same loop —
     an unlabeled break inside a deeper loop never crosses these usings — so one
     brk/cnt stub per using suffices. No crossing set → record straight into
     `loops_.back()` as F1 does. Else: record the site's Jump into the innermost
     crossing ctx's `brkJumps`/`cntJumps`; each stub closes and hops outward to
     the next crossing ctx; the **outermost** crossing ctx's stub records its
     final Jump into `loops_.back().breakJumps`/`.continueJumps` — from there
     F1's normal patching takes over.
   - **Throw edge**: nothing at throw sites — the pushed Handlers + pads do all
     of it.
5. Backends: consume the lowered form — **zero backend work**; every group is
   ordinary `CallDyn`/`Jump`/`Throw`/`Ret` plus ordinary Handler entries. The
   oracle (3) and the layout (4) enforce identical semantics on every edge, and
   the loops/using corpus runs differentially on all engines.

## 2. P-probes

- **P1 — RAN CLEAN (2026-07-06):** all `.ext` files repo-wide (165 under
  tests/examples + project.ext) + the prelude in Resolver.cpp: every occurrence
  of `break`/`continue`/`do`/`using` is inside a comment (examples/curl/cli.ext:4,
  ascii.ext:22, project.ext:156). **Verdict: all four are hard keywords.**
- **P2 — RAN (2026-07-06):** the guard-site list is *shorter* than expected and
  is enumerated exhaustively in F1 item 5 (sites 934, 939, plus the four loop
  bodies; frame confinement at 227/378/395/429; Match-arm site 803 verified
  correct as-is). The right grep is `grep -n 'exec(' src/Eval.cpp` — sequencing
  can only happen where statements execute.
- **P3 — RAN, static half (2026-07-06):** handler tables are **per-range**
  (`Handler{start,end,handlerPc,bindReg,type}` Ir.hpp:90; one entry per catch
  clause, Lower.cpp:486-506), so the primary F3 design stands and problem #5's
  fallback is dead. Dispatch order/type-match verified in all four engines (see
  the F3 engine table — the null-type divergence found there is why using
  handlers carry IException, never null). **Runtime half still owed as the first
  implementation step:** run the rethrow probe
  `try { throw RuntimeException("x"); } catch (IException e) { throw e; }` on
  all engines before building F3 on top of it.
- **P4:** closure-boundary probe: a lambda containing `while` + (after F1)
  `break` — must compile; a lambda containing bare `break` inside an enclosing
  loop — must error. (Compile-side is designed-in: Checker resets loopDepth_ at
  the four boundaries listed in F1 item 4; Lower clears `loops_`/`usings_`
  around nested-function lowering. The probe stays as the acceptance test.)

## 3. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Eval flag-audit misses a sequencing site** → break "leaks" upward and silently truncates an outer construct (worst-case a silent wrong-answer, the kind of bug differential testing exists for). | The P2 site list is the checklist; add a dedicated corpus program (`loops.ext`) with break/continue nested inside every construct combination (if/match/try inside loops, loops in loops, loop-in-catch). Oracle-vs-IR differential is the net: Lower's implementation is independent, so a missed Eval site diverges the engines and fails `run_corpus.sh`. |
| 2 | **For-loop continue-target restructure** silently changes step/cond ordering for existing code. | **Downgraded (2026-07-06):** no restructure exists — For already emits `body; step; Jump top` (Lower.cpp:531-542), so continue is pure record-and-patch and using-free/continue-free programs are byte-identical by construction. Keep the cheap assurance: diff `--ir` dumps of the existing corpus before/after. |
| 3 | **`break` inside `match`-statement arms**: match lowers to `is`-test jump chains (d4bb851); an arm's `break` must target the *loop*, not the match's own end-jumps. | Loop ctx is a separate stack from match lowering's internal jumps — they cannot collide as long as Break consults only `loops_`. Corpus case included in loops.ext. |
| 4 | **`using` + ARC**: closing is not freeing — the slot still releases via normal scope machinery after `close()`; double-release risk if close() itself releases. | `close()` is an ordinary method call; ARC ownership of the binding is untouched. Add a churn-corpus program (`using_churn.ext`) asserting +0B with using-in-a-loop, to catch interaction with the catch-bind release path (§15 fixed a leak exactly there). |
| 5 | **Handler-table granularity**: if handler regions are per-function (not per-range), a block-scoped catch-all-rethrow may be inexpressible. | **RESOLVED (2026-07-06):** P3 confirmed per-range handlers in the IR and all four engines. The fallback branch is dead; the primary design proceeds. |
| 6 | **`do` as identifier** in the wild (P1). | **RESOLVED (2026-07-06):** P1 ran clean — hard keyword, no contextual handling. |
| 7 | **Deferred-cleanup ordering vs break cleanup emission bloat**: break out of 3 nested using-blocks emits 3 close calls at every break site. | **Moot under rev 2:** break sites emit ONE Jump into the stub chain; the closes live once per block in the cleanup groups (the stub chain *is* the shared-epilogue follow-up rev 1 deferred). Residual cost is one stub set per using per exit kind actually used. |
| 8 | **(found 2026-07-06, pre-implementation) Return/handler-range collision**: rev 1's inline `close()` before `Ret`/loop-jumps sits inside the resource's own handler pc-range, so a throwing `close()` on those paths lands in the resource's own pad → double-close; nested usings could capture each other's cleanup throws in the wrong pad. | **Fixed structurally in rev 2** — the normative cleanup-group layout: all of a resource's cleanup code sits after its own range end but inside every enclosing range; exits reach cleanup by jumping *into* stubs, never by inlining closes. The `using.ext` corpus must include the three close-throws cases (during fallthrough with an enclosing catch; during unwind → replacement; during return/break → signal abandoned). |
| 9 | **(found 2026-07-06) Null-type handler divergence**: engines disagree on a null `Handler::type` — IrInterp never matches it, X64/LLVM treat it as catch-all, CGen feeds `clsId 0` to `issub`. A null-typed using handler would diverge per engine. | Never emit null: using handlers always carry the `IException` interface symbol — a de-facto catch-all (checker guarantees all thrown values implement it) with identical behavior everywhere. (The divergence itself is prelude-invisible today because user catches always have a type; noted for the owner, no action in this track.) |

## 4. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | F1 break/continue: tokens→checker→Eval→Lower; loops.ext corpus | build + ctest + run_corpus + run_elf + run_native (backends free via IR); checker tests (break outside loop, break in lambda) |
| M2 | F2 do-while | dowhile corpus cases folded into loops.ext; reference §5 updated |
| M3 | F3 `using` (primary design — P3 verdict in; fallback dead) | using.ext corpus covering ALL edges: fallthrough, throw, **return**, break, continue, nested blocks, two-usings-one-block, and the three close-throws cases from problem #8; runtime rethrow probe passes on all engines FIRST; using_churn.ext +0B; reference §5 + §12.6 note; IDisposable in §6 |

Target: **Jul 9 – Jul 12** (M1–M2 ~2 days, M3 ~2 days). M3 may slip to Jul 15
without endangering Gate A's loop items; if it slips, log it and re-gate `using`
into Phase C.

## 5. Reference-doc duty

reference §1.3 (keywords +4), §5 (statements: break/continue/do-while/using),
§6 (IDisposable), info.md §12.7 statement list, §19 #8 gets resolved-pointer.

## 6. STOP conditions

- ~~P3 reveals handler tables cannot express block-scoped catch-all~~ (resolved
  2026-07-06: per-range confirmed; fallback removed).
- The **runtime rethrow probe** (P3's owed half) fails on any engine — STOP
  before any F3 code: the pad's `close; Throw bind` is exactly that shape.
- Any temptation to add a new IR op or a new runtime flag visible across frames.
- ~~P1 identifier collisions~~ (resolved 2026-07-06: clean).
- The `--ir` corpus diff (problem #2 assurance) shows ANY instruction change for
  a program that uses none of the four features.

## 7. Implementation log

**2026-07-06 (design agent, rev 2 — pre-implementation review + probe pass):**
- Verified all rev-1 line anchors against the current tree and refreshed them in
  the header (drift ≤ 50 lines; every anchor still names the same construct).
- Ran P1 (clean — hard keywords), P2 (site list now exhaustive in F1 item 5),
  and the static half of P3 (per-range handlers confirmed; dispatch semantics
  extracted from all four engines and tabulated in F3).
- **Found problem #8** (return/handler-range collision → double-close) in rev
  1's F3 inline-cleanup scheme before any code was written; F3 lowering was
  redesigned around cleanup groups + stub chains (normative layout in F3). The
  return edge is now unconditional in M3's acceptance.
- Found problem #9 (null-`Handler::type` engine divergence); pinned IException
  as the mandatory using-handler type.
- Pre-pinned the judgment calls so implementation is mechanical: hard keywords
  for all four words; `using` = Var flag pair (`isUsing` + forced `isConst`), no
  new StmtKind, statement-position-only parse, direct-block-child placement
  check; value-struct receivers rejected in v1; `usingClose` resolved by the
  checker onto the Stmt; close emission = the ordinary member-call `CallDyn`
  shape (Lower.cpp:1363 template); uniform "close-throw beats any pending
  signal" rule with the oracle's flag-neutral call pattern spelled out; LoopCtx
  reduced to two patch lists (no continueTarget field — everything is
  record-and-patch); For needs no restructure (problem #2 downgraded); DoWhile
  traversal checklist enumerated (8 switch sites); `loops_`/`usings_`
  save/restore around nested-function lowering.
- Pre-existing observation for the owner (NOT this track's scope): Eval's
  top-level run loop (Eval.cpp:1078) executes only ExprStmt/Var/Try/Throw, so a
  top-level `while` (and now `do-while`, which mirrors it) never runs in the
  oracle, while Project.cpp:464 classifies both as executable. Filed here for
  visibility; bug.md if the owner wants it tracked.

**2026-07-06 (implementer, M1–M3 landed):**

- **P3's owed runtime half ran clean first**, per the STOP condition:
  `try { throw RuntimeException("x"); } catch (IException e) { throw e; }`
  matched byte-for-byte across all five engines (oracle/IR/emit-C++/ELF/LLVM)
  before any F3 code was written.
- **F1/F2 landed exactly per the rev-2 plan** — `loopDepth_`, the four
  function-body-boundary resets (checkFunction, both Bind-body sites, Lambda),
  the `loops_` two-list LoopCtx, and the traversal checklist all matched the
  design with no surprises. The For-loop "no restructure needed" call (problem
  #2 downgrade) held exactly as predicted.
- **F3's cleanup-group/stub-chain layout implemented as specified**, with one
  addition the rev-2 doc didn't spell out: `LoopCtx` needed a `usingsFloor`
  field (usings_.size() at loop entry) to correctly scope break/continue's
  "crossing set" — without it, a using declared OUTSIDE a loop (in the same
  function, before the loop starts) would be incorrectly targeted by a break
  inside the loop. Derived from first principles during implementation (the
  rev-2 doc's UsingCtx had no `loopsDepthAtCreation` field; putting the
  watermark on the LOOP instead of stamping the loop-depth on every USING
  turned out simpler and sufficient — the loop is always still on `loops_`
  when its own crossing usings' stubs are emitted, by construction of
  post-order block lowering). Verified via the `nestedLoops`/`ifInLoop` cases
  in `loops.ext` and the break/continue cases in `using.ext`/the churn corpus.
- **Found and fixed a REAL leak during F3 churn-testing, but not in F3**: the
  return-crossing path (`chainRetReg_`) initially appeared to leak ~128B/call
  on the ELF backend when a `using`-bound resource held an `Array` field. Full
  bisection (see bug.md #16) traced it to a **pre-existing, `using`-unrelated**
  bug — any class instance built by DIRECT INLINE construction (`T x = T();`)
  inside a function called repeatedly leaks its heap-tier fields on the ELF
  backend; a PLAIN local with no `using` at all reproduces it identically.
  Confirmed F3's own mechanism (fallthrough, return-crossing, break-crossing,
  continue-crossing) is fully leak-free once decoupled from that bug by
  routing construction through a factory function (the pre-existing,
  corpus-wide convention every OTHER churn program already uses) — see
  `using_churn.ext` and `using_return_churn.ext`, both flat +0B at N=100/800.
  Filed as bug.md #16, not fixed (out of Track 02's owned regions —
  `X64Gen.cpp` ARC internals).
- **Found and fixed, in passing**: the tracks-00 overview's explicit warning
  to check `Rules.cpp::cloneStmt`'s hand-rolled field copy for new
  Stmt fields (the same class of bug Track 01 found for `Expr::isRawSegment`)
  turned up that `cloneStmt` ALSO never copied `Stmt::isConst` (const.md,
  landed earlier, unrelated to this track) — fixed as a one-liner alongside
  `isUsing`/`usingClose` in the same commit; not independently tested (no
  rule-injection-of-const corpus case exists), noted as bug.md #17 for
  whoever owns §16.5.
- **All five engines verified in full agreement** on every corpus file
  (`loops.ext`, `using.ext`, both churn files) via direct diff, not just
  ctest's per-mode pass/fail — oracle, `--ir`, emit-C++ (where the construct
  is supported; `using.ext`'s File portion correctly SKIPs on emit-C++, a
  pre-existing native-backend gap unrelated to this track, same as
  `files.ext`), `--emit-elf`, and `--native-obj` (LLVM) all byte-identical.
- **Full ctest suite: 41/41 passed**, including the new
  `corpus_churn_leak`/`corpus_churn_leak_llvm` runs picking up the two new
  churn programs automatically (tests/corpus/churn/ is scanned, no CMakeLists
  change needed) and `checkertests` picking up the new break/continue/
  do-while/using ERRORS/CLEAN assertions (loop-depth-0 errors, lambda-boundary
  reset, match-non-boundary, do-while, using placement/type/reassignment/
  no-initializer checks).
- Reference-doc duty discharged: reference.md §1.3 (+4 keywords), new §5.2
  (break/continue/do-while/using prose + example), §6.6.65 (`IDisposable`);
  info.md §12.6 ("no finally" now points at `using`), §12.7 (statement list),
  §19 #8 marked resolved with a pointer back here.
- No STOP conditions were hit. No new IR op, no new cross-frame runtime flag
  (F3 desugars entirely into existing `CallDyn`/`Jump`/`Throw`/`Ret`/Handler
  machinery, exactly as designed).
