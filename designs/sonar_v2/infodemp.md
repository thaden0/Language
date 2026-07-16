# Sonar — Research Knowledge Dump (objective findings, single pass)

**Date:** 2026-07-12. **Purpose:** everything learned in the pre-design research sweep for the
6 Sonar-prerequisite language features and the framework's load-bearing dependencies. All
file:line references verified against the tree this session. This is DATA, not design.

---

## 1. Stale-doc corrections (ground truth vs design-doc headers)

- **`designs/complete/terminal-raw-mode.md:3` header says "Status: proposal (nothing implemented)" — STALE. Raw mode is FULLY IMPLEMENTED** on all four active backends with crash-safe restore-on-exit.
- **`designs/system-binds.md` claims bind/inject is "parse-only" (bug.md #9) — STALE.** bug.md #9 is gone (fixed via commits `768d623` → `5731d6d` → `3722ded` "bug.md #9: implement bind/inject DI core (§12.5)" → `1fe86cc` "bug.md #24 + #23: constructor injection fill; reject value-type binds"). bug.md now contains ONLY #3 [P3] and #35 [P2]; P0/P1 empty.
- **`Resolver.cpp:117-119` comment "No ELF/LLVM lane" on Block — STALE**; LLVM landed via LV_BLOCK ABI addendum.
- The old "field-closure dot-call" defect (LA-25 log's STOP #1, old bug.md #2) is no longer listed in bug.md. LA-25's corpus still uses the `var h = obj.field; h(args)` workaround pattern; the direct `obj.field(args)` spelling should be probed before relying on it.
- Metaprogramming is **Phases 1–4 ALL BUILT AND MERGED** (per `designs/complete/techdesign-metaprog-phase3.md` and `-phase4.md` §14): attributes, comptime, rules, `where`, `meta.*`, `$for` (array-literal position), `$_params`/`$_args`, expression macros `name!(…)`, anchors (ctor top/bottom, member of, body top/bottom, marker, namespace), Layer D `rewrites`/`replace`/`$body`, confluence M33, `reentrant` fixpoint, source-shaped `--expand` with round-trip test. Deliberately NOT built (demand-gated): provenanceId, incremental caching, `meta::Type`/`meta::Attr` structured reflection, statement-position `$for`, `Symbol::homeNs`, class-wide marker search, attribute named args, macro auto-hoisting.

---

## 2. Feature: Weak references (for the parent/children cycle)

### Current ARC implementation map
- **Three co-equal implementations**: runtime C (`runtime/lv_runtime.c`, LLVM backend), frozen X64Gen (`src/X64Gen.cpp` — DO NOT TOUCH), and the two host interpreters (`src/Eval.cpp` oracle + `src/IrInterp.cpp`) which use host C++ `std::shared_ptr` and do NOT refcount manually. `src/LlvmGen.cpp` emits ARC discipline; `src/CGen.cpp` (emit-C++) is scalar-core only, no ARC.
- **Header**: `LvHeader = { int64_t rc; int64_t meta; }`, 16 bytes at `payload-16` (`lv_abi.h:63-76`). Macro `HDR(payload)` at `lv_runtime.c:35`. rc semantics (`lv_abi.h:66-73`): `-1` = arena (never counted/freed); `0` = fresh/unowned (retain's target; release no-ops on rc≤0); `≥1` = owned.
- **Allocator**: `lv_halloc_prefixed(bodySize)` at `lv_runtime.c:167-174` (allocates body+16, hdr[0]=0, hdr[1]=total). Backed by `lv_alloc_heap` (`:119-135`, bump + power-of-two free-list reuse) or `lv_alloc_arena` (`:137-143`). Raw `lvrt_halloc` (`:145-147`) writes NO header.
- **Value repr**: `LvValue = { int64_t tag; int64_t payload; }` (`lv_abi.h:36`); tags enum `lv_abi.h:38-50` (`LV_CHAR=10`, `LV_BLOCK=11` at `:49`).
- **retain/release**: `lvrt_retain` `lv_runtime.c:220-226`; `lvrt_release` `:228-236` (at 0 → `lv_recursive_free`). `lv_recursive_free(tag,payload)` `:252-332`: objects walk dyn-fallback list at offset 8 (`:270-278`); closures walk captureHead (`:280-289`); boxed arrays release each elem (`:290-303`); maps release key+val (`:304-313`); blocks release parent (slice) or free dataPtr (root) (`:314-327`). `lvrt_vfree` `:238-250` for dead value-struct copies. `lv_free_raw` (`:151-160`) memset-poisons `0xFE` and returns block to free list — **freed memory is immediately reusable**.
- **Counted-kind gate**: `lv_is_counted(tag,payload)` `lv_runtime.c:200-216`: (1) tag < LV_STR(4) or LV_NONE(8) → not counted; (2) LV_CHAR(10) not counted (pure immediate); (3) only LV_STR, LV_OBJ, LV_ARR, LV_MAP, LV_CLO, LV_BLOCK pass; (4) **heap-address-range check `lv_in_region(payload)` `:92-97`** runs BEFORE any payload-16 deref — excludes data-segment literals and arena strings (no prefix); (5) LV_OBJ with `isvalueclass` classId → not counted (tag-5 pointer may point inline into a dense buffer, `lv_abi.h:58-60`).
- **ARC emission (LLVM)**: NO `Op::Retain`/`Op::Release` IR ops — ARC is implicit in op semantics, classified by `destKind(Op)` at `LlvmGen.cpp:81-98`: dk=0 (op writes other memory, does own ARC: SetMember, RawSet, StoreGlobal, CaptureVar, Throw, VFree); dk=1 (register owns fresh/aliased value → release-old-then-retain-new, the default); dk=2 (already +1: calls/Arith/MoveClear/NewObject/GetMember/NewArray → release-old only).
- **Register tier**: every register carries virtual +1; params retained on entry (`LlvmGen.cpp:1104`); `releaseAllRegs` at frame exit (`:1197-1199`) on every Ret/RetVoid/unwind.
- **Generic slot wrap** `wrapDest` (`:1177-1196`): stash old (tag,pay), run body, if new≠old release old + (dk==1) retain new; same-pointer guard `:1185-1189`.
- **Store paths** (the ones a weak slot must skip retain on): `RawSet` `:2919-2931` (load old→arcScratch, store new, rtRelease(old), rtRetain(new); fixed slot via `payAddr(...,16+(d-1)*16)` or dyn via `lvrt_getfield`/`lvrt_setfield`); `SetMember` `:2932-2939` (delegates `lvrt_setm`, release/retain internal per `lv_abi.h:275-280`); `StoreGlobal` `:2940-2951` (retains new, does NOT release old — deliberate X64Gen parity, globals ~write-once); `IndexStore` `:2632-2684` (`lvrt_idxset` COW-in-place at rc==1); `CaptureVar` `:2794-2798` (`lvrt_capture_set` retains internally, `lv_runtime.c:460-463`).
- **Read paths** (must return None on dead referent for weak): `RawGet` `:2484`, `GetMember`/`lvrt_getm` (+1 transfer, dk=2), `LoadGlobal` `:2501`, `LoadCapture` `:2800-2802` (dk=1 wrap retains borrowed value).
- **IR ops** (`src/Ir.hpp:24-80`): Move/MoveClear `:28-30`, CopyVal `:31`, SetMember/RawSet `:51-53`, StoreGlobal `:68`, IndexStore `:62`, CaptureVar `:71`, VFree `:77-79` (no-op on GC'd engines). Lower-side field-clear read protocol at `Lower.cpp:1332-1347`.
- **X64Gen (frozen, note only)**: genRetain `:648-680`, genRelease `:684-718`, genRecursiveFree `:722+`.
- **Interpreters**: shared `struct Value` (`RuntimeValue.hpp:39-51`) holds `shared_ptr<Object>`, `shared_ptr<Closure>`, `shared_ptr<vector<Value>>` (array), `shared_ptr<vector<pair<Value,Value>>>` (map), `shared_ptr<BlockData>`. `Object` = `{ Symbol* cls; unordered_map<string,Value> fields; }` (`:53-56`). Value-structs deep-copy via `copyValue` (`:94-101`). Host strong-ref cycles leak (accepted, `Eval.hpp:11-16`).
- **No weak/cycle infra exists.** Every "weak" token in-tree is `std::weak_ptr` used by the ownership liveness probe (`MemVerify.cpp:14`, `IrInterp.hpp:72-73`, `Ownership.hpp:24`) — soundness validation, not a language feature. info.md §15 (`info.md:1476-1484`): cycles observed-but-not-collected (Timer→buffer→handler→Timer); weak-vs-collector is §19 #10.

### Weak hook points (per engine)
- A weak slot must (a) NOT retain on store, (b) read as None once referent freed.
- **LLVM/runtime problem**: header has only {rc, meta} — no weak count, no side table; at rc→0 the block is immediately freed, poisoned, and free-listed, so a weak read after free touches freed/reused memory. **New liveness state required**: either a weak count (e.g. in the `meta` word or a widened header) keeping the *memory* alive until strong AND weak both drop, with a tombstone/dead flag when strong hits 0 (Swift/Rust `Weak` model — the free-list reuse in `lv_free_raw` must consult the weak count), or a zeroing-on-free side table keyed by object id. Hook sites: header layout `lv_abi.h:63`, allocator/free `lv_runtime.c:151/167`, retain/release/recursive_free `:220-332`, new `lvrt_weak_load`/`lvrt_weak_store`, destKind wiring + store/read paths in `LlvmGen.cpp` (above).
- **Oracle+IR**: natural fit — a weak slot holds `std::weak_ptr<Object>`; read = `.lock()` → None on expiry. Weak-ness must be carried in the `Value` (new kind/flag), NOT the field map (fields are `unordered_map<string,Value>`, `RuntimeValue.hpp:55`).
- **ELF/X64Gen**: frozen — feature does not ship there; never gate on it.
- Surface facts to resolve at design time: a weak read yields absence, so a weak slot's type is effectively `T?`; interaction with distinct/const/readonly modifiers; field-only vs general.
- **Risk class: HIGH — the only one of the six features that touches the ABI header + allocator.** All others are checker-only or additive-native.

---

## 3. Feature: Terminal floor — what exists vs what's missing

### Already implemented (do not redesign)
- **Raw mode, all four backends, with guaranteed restore**: floor `lv_plat_term_raw(int fd)` `lv_plat_posix.c:85` (tcgetattr save + `~(ECHO|ICANON|IEXTEN|ISIG)` + TCSAFLUSH), `lv_plat_term_restore` `:99`; Win32 `lv_plat_win32.c:188/:202`; interface `lv_plat.h:63-64`. Native `lvrt_systermraw` `lv_runtime.c:2197`; oracle `RuntimeNatives.cpp:1071` (direct termios `:46-67`); CGen `CGen.cpp:249-265,897`; LLVM `LlvmGen.cpp:310,2295-2296`. Language decl `int sysTermRaw(int fd)` `Resolver.cpp:1041`; wrapper `term::enableRaw() => std::sysTermRaw(0) == 0` `Resolver.cpp:2558`. **Restore wired on every exit path**: `lvrt_term_shutdown` (`lv_runtime.c:2211`) called from normal exit (`lv_entry.c:68`), the uncaught reporter, and `lv_die` (`lv_runtime.c:54`).
- Raw mode sets `~ISIG` → Ctrl-C arrives as byte `0x03`, not a signal.
- **`lv_plat.h` floor inventory** (current ops): map/unmap, write/read (`:25-26`), now_ns/now_realtime_ms, exit, open/close, term_raw/term_restore (`:63-64`), stat_size/mtime, tcp_connect/listen/accept/send/recv, cpu_count, sock_buffer, set_nonblock, random, tcp_connect_nb/connect_result, poll `lv_plat_poll` + `LvPollFd` (`:111-112`). **No terminal-size op, no signal op.**

### Missing (both have full pre-written proposals)
- **Winsize — NOT implemented.** No native; `Resolver.cpp:2560-2563` comments note `isRaw()` and the `\x1b[6n` fallback are deferred. Proposal `designs/terminal-winsize.md` (complete design): `lv_plat_term_size(fd,*rows,*cols)` via `ioctl(TIOCGWINSZ)` (0×0 report treated as failure); native `sysWinSize(fd, field)` (field 0=rows, 1=cols, -1 fail — the `sysStat` field-indexed shape); language `class WinSize { int rows; int cols; }` + `term::size()` with the in-language `\x1b[999C\x1b[999B\x1b[6n` cursor-report fallback (guarded on isRaw; parses `\x1b[<r>;<c>R` with indexOf/subStr/toInt), final default WinSize(24,80). Coverage: interpreters + LLVM + emit-C++; ELF defers via existing native fallthrough (`X64Gen.cpp:3854` pattern). Win32: `GetConsoleScreenBufferInfo` (srWindow). Testing: pty with `TIOCSWINSZ` set + read-back.
- **Signals — NOT implemented.** Only `sigaction` in tree is the scheduler's SIGSEGV crash reporter (`lv_task.c:379-384`). Proposal `designs/signals.md` (complete design): signal-as-system-stream, NEVER language code in signal context. Floor: `lv_plat_signal_open(const int* sigs, int n)` → readable fd (Linux `signalfd` primary; portable self-pipe fallback whose handler body is just `write(pipe_wr,&signo,1)`); `lv_plat_signal_next(fd)` → signo or -1; `lv_plat_signal_close(fd)`. Natives `sysSignalOpen(Array<int>)/sysSignalNext/sysSignalClose`, composing with EXISTING `sysWatch`/`sysUnwatch`. Language `namespace signal { const int INT=2, QUIT=3, TERM=15, HUP=1, USR1=10, WINCH=28; InStream<int> on(int sig); }` built like `Timer` over a `StreamBuffer`. Safety obligation: when raw mode is entered, runtime installs handlers for SIGTERM/HUP/INT/QUIT whose entire body is `lv_plat_term_restore` + re-raise default (async-signal-safe). Edge cases pre-decided in the doc: interpreter must unblock+close at program end (shares the `leviathan` process); SIGWINCH coalescing ("at least one tick after the last change"); no pre-subscription buffering; SIGKILL/SIGSTOP rejected; one signalfd per signal fanned to subscribers; with ISIG cleared, `signal.on(INT)` fires only from external kill.
- The techdesign-08 "signals deliberately avoided" caution is about handlers; the stream model defuses it (stated in signals.md header).

---

## 4. Feature: Bound method references

### How unbound (LA-25) works today — checker-only
- Zero code in Lower/Eval/IrInterp/LlvmGen/CGen for method refs. The checker **rewrites the `Member` node in place into an ordinary `Lambda`** (eta-expansion); every engine's existing lambda path runs it.
- Entry: `Checker::tryResolveMethodRef` `Checker.cpp:1062`, **self-gated `if (!e->colon) return unknown();` at `:1064`** — only `::` enters.
- Call sites: value/slot position via `typeOfMember` `:829-833`; assignment RHS with LHS expected type `typeOfBinary` `:1470-1473`; argument position `:1338, :2140, :2224, :2234` with deferral for overloaded refs via `isDeferredMethodRefArg` `:1144` (mirrors lambda deferral `:1225, :2206`).
- Rewrite: `rewriteAsMethodRef` `:989-1060` — `e->kind = ExprKind::Lambda` (`:1048`), synthesized params `$mr0..$mrN` (`methodRefParamName` `:147`), body = synthesized `Call` (`:1051`). Instance method: callee is `.`-Member `$mr0.name`, receiver = param 0; type via `methodRefCanonical` `:938` = `(recvCanon, params…) => ret`; **an empty `recvCanon` omits the receiver** (`:942`).
- Lowered by ordinary `lowerLambda` `Lower.cpp:1369`.

### Closure capture facts
- `lowerLambda` `Lower.cpp:1369-1457`. **Capture set = all currently visible locals, snapshotted by (name, reg)** (`:1380-1383`); `Op::MakeClosure` + `Op::CaptureVar` per capture (`:1451-1454`); body reloads via `Op::LoadCapture` (`:1426-1432`). **`this` is captured under the literal name "this"** (`:1389-1391`); `thisReg()` resolves it (`:34`).
- Note: interpreters over-capture (whole env) vs LLVM minimal capture — bug.md #3's divergence for spawn.

### The bound-ref delta (facts)
- **Parser: none needed.** `obj.method` already parses to `Member{colon:false}` (`Parser.cpp:443-463`); the `(` is a separate postfix iteration.
- **The exact checker branch**: `Checker.cpp:910-917`, esp. **`:915`** — instance-`.`-member that is a method currently returns lenient `wrap(unknown())`. So `var f = obj.method` silently types unknown today — NO "callable member not called" error exists on the `.` path.
- Downstream today, a bare `Member{colon:false}` value lowers via `lowerExpr`'s Member case (`Lower.cpp:1557-1605`) to `GetMember`/`RawGet` by name (`:1595-1602`) — reads a data slot, no closure synthesis (the `.`-analog of the P-2 latent hole `::` had).
- Bound variant of the rewrite: omit the `$mr0` receiver param (drop recvOffset/recvCanon prefix at `:1023, :1034-1037`); use **`e->a` (the receiver expression) as the synthesized call's callee base** — the free-function branch `:1002-1006` already reuses `e->a` verbatim as callee base. Capture then happens automatically via the capture-by-name sweep (a local receiver or `this` is snapshotted for free).
- Gates to extend to the `!e->colon` instance case: `tryResolveMethodRef:1064`, `typeOfMember:829`, `typeOfBinary:1470`, `isDeferredMethodRefArg:1145`.
- **The one open semantics question (LA-25 §8.4's capture/lifetime)**: a simple lvalue receiver (local / `this`) snapshots correctly with zero new lowering; an arbitrary-expression receiver (`getFoo().method`) left as `e->a` in the body **re-evaluates per call** instead of snapshotting once — needs either a v1 restriction to simple receivers or a hoist-to-local (which the in-place checker rewrite has no statement context to do).
- **Dispatch**: decided at rewrite time by `resolveDispatch` `Checker.cpp:282-294` (applied at `:1019-1020`): interface-typed receiver OR overridden-below → `resolved` nulled → runtime `CallDyn` by name; otherwise statically bound. Same rule as ordinary calls (`:1370, :1432`). A bound ref gives exactly the dispatch of the hand-written `(args) => obj.method(args)`.

---

## 5. Feature: Procedural macros (the `sonar!` gap)

### What template macros can and cannot do today
- `macro name(e) => \`template\`` body is **parsed once at parse time into a fixed AST**: `parseMacroDecl` `Parser.cpp:1220-1253`; body REQUIRED to be a QuasiLiteral; `a.tmplExpr = parseExprFragment(a.quasiSpan)` at `:1247`. No alternate body form.
- Call site `name!(...)` → `ExprKind::Call` with `isMacroCall` (`Parser.cpp:469-478`).
- Expansion `expandMacroCall` `Rules.cpp:737-827`: each arg binds as `Binding{ exprNode = call->list[i].get() }` (unevaluated subtree, `:800-812`); `cloneExpr` of the fixed template with hole splice; result assigned `slot = std::move(expanded)` at `:826`. **Pure tree-copy + hole-splice — the macro cannot branch on the argument, count children, or synthesize different structure.**
- Guards: M22 single-splice per param (`Rules.cpp:649-665`); M24 no-re-entry (`:816-823`). Expansion runs in a second walk post-imports-recompute, gated `macroExpansionEnabled_` (`Rules.cpp:55-64`; triggers at `:482, :286, :388, :471`).

### The three existing pieces, not connected through comptime
1. **Raw payload exists as bytes**: backtick lexes to one raw `TokenKind::QuasiLiteral` (`Lexer.cpp:99-111` `lexQuasi`; `Token.hpp:15`), payload recoverable as `file_.text[span.offset+1 .. end-1]`. **BUT QuasiLiteral is NOT a primary expression** — accepted only at `Parser.cpp:1244` (macro body) and `:1314` (rule action). **`sonar!(\`<App>…\`)` will not parse today.**
2. **String→AST engine exists**: fragment parsers `parseStmtsFragment :1388`, `parseMemberFragment :1415`, `parseExprFragment :1423`, `parseItemsFragment :1540` — each does `Lexer lex(file_, sink_, /*allowHoles=*/true); lex.tokenizeRange(span+1, end-1)` + sub-Parser. **Not reachable from comptime**: `Eval.cpp` has no Parser reference; no parse-as-code native exists (verified in RuntimeNatives.cpp/Eval.cpp).
3. **Splice substrate solved**: `Binding::exprNode` (`Rules.hpp:181-189`) splices arbitrary pre-built subtrees (`Rules.cpp:1914-1916`); clone machinery `cloneExpr :1865-2046`, `cloneStmtListBody :1846`, `cloneArrayElements`/`$for` `:1790-1823`. A computed tree can replace the `cloneExpr` result at `:826`.

### The hard boundary
- `reify()` (`Rules.cpp:514-614`) is the SOLE Value→AST bridge: Int/Float/Bool literals, String → StringLit, None → Name, Array → Array of reified elems, value-struct Object → ctor Call (`:574-609`); reference objects/closures/maps non-reifiable (`:611-612`). `foldExpr` (`:489-512`) is the only comptime→splice path and runs reify.
- **`VKind` has no code/AST carrier** — `RuntimeValue.hpp:35`: `{ Void, Int, Float, Bool, String, Object, Closure, Array, Map, None, Char, Block }`. Even if comptime built a tree there is no value type to hold it and no reify case to splice it. **This is the single largest missing piece.**

### Minimal new mechanism (the wire), per the code map
1. A new AST-carrying `VKind` in `RuntimeValue.hpp:35`.
2. A comptime "parse this string as code" primitive exposing the fragment parsers to `Eval.cpp` (parsers already take an arbitrary span; parsing a runtime-built string is the only extension).
3. A non-template macro body form (comptime code that receives the payload and returns constructed code) + QuasiLiteral as a primary expression yielding the raw payload string.
4. A `reify` sibling case splicing an Ast-valued result directly; M22/M24 reconsidered for computed output.
- Hygiene/def-site qualification/clone-time passes already exist and apply to injected clones (`expandMacrosInClone` overloads; def-site scoping lands in the rule's file per phase3 §7 5c).

---

## 6. Feature: Block bulk ops (fill/blit/equals)

- **Native repr**: tag `LV_BLOCK = 11` (`lv_abi.h:49`); body `{parentPtr@0, off@8, len@16, dataPtr@24}` (`lv_abi.h:144`, section `:386`). Byte address helper `lv_block_at(payload,i)` = `P8(dataPtr) + off + i` (`lv_runtime.c:1367`). `lv_block_alloc` `:1371` (48-byte record = 32 body + 16 ARC header).
- Existing natives (`lv_runtime.c`): `lvrt_block_new :1380`, `_fromstr :1387`, `_byteat :1394`, `_setbyte :1400`, `_slice :1411`, `_tostring :1425`, `_int32at :1433`, `_setint32 :1442`, `_int64at :1450`, `_setint64 :1459`. Print row "Block(len=N)" `:1795`.
- Interpreter repr: `struct BlockData { shared_ptr<vector<uint8_t>> bytes; size_t off; size_t len; }` `RuntimeValue.hpp:28`; `VKind::Block :35`; equality is reference identity `:237`. CGen repr `CGen.cpp:44`.
- **Slice aliases the root**: `lvrt_block_slice :1411` flattens to root (`root = parent ? parent : bp`, `:1418`), `off = b.off + o`, `dataPtr` = root base, then `lvrt_retain(&rootv)` `:1421`. Interp `RuntimeNatives.cpp:593-599` shares `bytes`; CGen `CGen.cpp:429-430` same.
- **Bounds discipline**: front-load `if (i<0 || i>=len) { lvrt_raise_oob(i,len); out->tag=LV_VOID; return; }` (`lvrt_raise_oob` `lv_runtime.c:2286` → `lvrt_raise` `:2265` constructs RuntimeException + `lvrt_throw_set`); setByte's 0..255 check builds a custom message `:1403-1407`. Interp `oob` lambda `RuntimeNatives.cpp:573-577` (sets err → thrown); CGen `oob` `CGen.cpp:423`. Always check against **each view's own `len`** (offset 16), never the underlying buffer size. Overlapping same-root blits need **memmove** semantics.
- **Recipe for a new bulk native (methods → NO new IR op; construction ops NewBlock/NewBlockStr `Ir.hpp:58-59`/`Lower.cpp:1099-1103` are ctor-only)**:
  1. Signature in `class Block` prelude decl `Resolver.cpp:120-132` (e.g. setByte at `:125`).
  2. C body in `lv_runtime.c` near `:1465` following the bounds discipline.
  3. One clause in the `cls == "Block"` branch of shared `nativeCall` `RuntimeNatives.cpp:565` (setByte at `:584-591`) — **covers oracle AND IrInterp at once** (callers `Eval.cpp:636`, `IrInterp.cpp:117`).
  4. One clause in CGen's `self.k == 11` branch `CGen.cpp:419` (setByte `:426-428`).
  5. LLVM triple: enum member + `fn(...)` binding (`LlvmGen.cpp:159-160`, `:270-279`, e.g. `rtBlockSetbyte = fn("lvrt_block_setbyte", voidTy, {ptrTy,i64Ty,i64Ty})` at `:273`); dispatch row in `emitNativeRows` (lambda `:1342`, `row(11, …)` defined `:1349`, Block rows `:1720-1724`); **add the name to the `kCovered[]` whitelist `:1765-1775`** (Block entries `:1773`; must stay in sync with the else-if chain per comment `:1753-1764`, else the call falls to class dispatch and fails). LlvmGen never inlines byte loops — work lives in the C function.
  6. Corpus under `tests/corpus/blocks/`; interpreters are the reference, LLVM checked against them. No ELF work (grep hits in X64Gen are unrelated tokens).
- Existing Block I/O (already landed, oracle+IR+LLVM): `sysRead(fd,Block,max)->int` `lv_runtime.c:2092` / decl `Resolver.cpp:1306`; string form `(fd,max)->string` `Resolver.cpp:1031` — arity-distinct.

---

## 7. Feature: Covariant-return interface satisfaction

- **`Container add(IComponent)` does NOT satisfy `IContainer add(IComponent)` today — hard compile error**, not a mis-accept.
- Satisfaction check: `Resolver::buildShape` `Resolver.cpp:5217-5290`; interface requirements collected at `:5258-5261` (interface bases contribute `interfaceReqs`, not merged slots); the loop `:5277-5285` requires `s.name == req.name && s.canonical == req.canonical` (**exact string equality on the full canonical**) else error `"does not satisfy interface: missing 'add : (IComponent) -> IContainer'"` (`:5281-5285`).
- Method canonical **embeds the return type**: `slotOf` `Resolver.cpp:5127-5150` builds `"(params) -> ret"` (`:5138-5144`). Only tolerance: generic-param renaming via `substituteSlotGenerics` `:5211-5215/:5253-5257`.
- Collision/merge: `mergeSlot` `Resolver.cpp:5152-5184` keys on name + full canonical (`:5156`); distinct keeps both `:5157-5161`; const/readonly disagreement errors `:5162-5176`; same-name different-canonical (e.g. return-type-only difference) **coexist as separate slots** (`:5183`). `Slot::distinct` at `Symbols.hpp:42`; `Slot` carries only the canonical string + source/decl (`Symbols.hpp:35-46`) — no structured return Type.
- Overload resolution: `pickOverload` `Checker.cpp:1607-1644` — arity filter `:1614`; per-arg scoring (generic unify +1 `:1619`, exact +2 `:1635`, assignable +1 `:1636`, else inapplicable `:1637`); ties first-declared `:1641`. **Return type never participates.** Candidate sets: `methodOverloads :1646-1652`, `ctorOverloads :1654-1660`, `functionOverloads :1662+`.
- Runtime dispatch is **name+arity only**: `IrInterp::findMethodByName` `IrInterp.cpp:27-47` (arity disambiguation `:40`; first-found fallback `:45-46`); CGen parallel `CGen.cpp:1197-1202`. Compile error for overridden overloads sharing an arity: `resolveDispatch` `Checker.cpp:282-294` + `overrideDispatchAmbiguous` `:269-280` (uses `isOverriddenBelow :253-262` / `buildOverrideIndex :239-251`); message at `:288-291`; spec `reference.md:312-347`.
- Assignability: `Checker::assignable` `Checker.cpp:296-318` (unions `:301`; canonical== `:306`; class→class via `isSubclass` `:315`); `isSubclass` `:212-219` walks `decl->bases` transitively; **interfaces live in `decl->bases`** (`Resolver.cpp:5229-5261`, `isInterface()` `Symbols.hpp:63`), so `isSubclass(Container, IContainer)` is TRUE and `assignable(Container, IContainer)` is TRUE already.
- **The decision point is the one equality at `Resolver.cpp:5280`**, but three interacting constraints: (1) `mergeSlot` unchanged would leave the covariant method and the interface-requirement canonical as two same-name same-arity slots; (2) that shape trips the §3.4a shares-its-arity compile error at overridden call sites (`Checker.cpp:269-291`); (3) runtime name+arity dispatch can never disambiguate by return type. The subtype predicate needed (`assignable`) lives on Checker (Type/Symbol level) while satisfaction lives on Resolver (canonical strings; `Slot` has no structured return) — a ruling must either give the resolver a symbol-level subtype check + split the canonical, or thread structured return types into `Slot`.

---

## 8. Framework-dependency ground truth (verified)

### DI (bind/inject) — IMPLEMENTED
- `Checker::pushBindScope` pre-scans direct-child factory binds `Checker.cpp:1680`; `popBindScope :1707`; `lookupBind` nearest-wins `:1712`; block-scoped push `:2260`.
- **Implicit injection resolves**: `pickInjecting` `:1735` matches unfilled trailing params against in-scope binds (`:1788-1789`) and synthesizes `ExprKind::Inject` arg nodes (`:1856-1859`); applied to calls `:1246-1314` and constructors `:1307-1314, :2210-2212`. Explicit `inject Type` checked `:657-662`. Runtime executes: `Eval.cpp:1034` runs the resolved factory via `callFunction`.
- **Value-type (struct) binds are REJECTED** with a diagnostic (`Checker.cpp:1684-1695`). Bind interfaces/reference classes only.
- `Bindings` objects / `bind someBindings;` NOT implemented (`reference.md:646-648`).

### Event loop / await / timers — IMPLEMENTED on interpreters AND LLVM
- Two loop registries: C++ `src/RuntimeLoop.cpp` (oracle + IR) and C `runtime/lv_loop.c` (LLVM/native); both do timers + read/write watches, dispatch-until-idle.
- Timers: `RuntimeLoop::addTimer :17`; `lvrt_systimerstart` `lv_loop.c:146`; decl `sysTimerStart(delayMs,intervalMs,(int)=>void)` `Resolver.cpp:1050-1051`.
- fd readiness: decls `sysWatch(fd,(int)=>void)` `Resolver.cpp:1310`, `sysUnwatch :1311`, `sysWatchWrite :1315`; registry `RuntimeLoop::addWatch :32`, poll dispatch `:78-139`; native `lvrt_syswatch` `lv_abi.h:627`. **This is the stdin-blocking mechanism.**
- **Keep-alive**: `hasWork()` = `!timers_.empty() || !watches_.empty()` `RuntimeLoop.cpp:49-50`; C mirror `lvrt_loop_has_work` `lv_loop.c:233`; native entry `lv_entry.c:59` `while (lvrt_loop_has_work()) lvrt_loop_step();`; interp mirrors `Eval.cpp:1558`, `IrInterp.cpp:736`. Program stays alive while any watch/timer pending (`RuntimeLoop.hpp:12`).
- await/Promise: prelude `Resolver.cpp:1075-1120` (`Worker<T> : Promise<T>`); `lvrt_await` `lv_abi.h:642`; park machinery `lv_task.c` (`LV_TASK_PARKED :109`, `lv_task_park_on :602`); `Op::Await` `IrInterp.cpp:397`; Eval pump `Eval.cpp:1154`. Tasks default-on since the M5 flip (LANG_PUMP=1 escape hatch).
- bug.md #35 [FIXED, reject route A]: a `std::spawn` body referencing a Promise through a bare top-level global (which used to bypass the A-1 flatten guard) now rejects loudly and byte-identically on oracle/IR/LLVM at the spawn call — the same reject as capturing it through a local. Pass a `Channel<T>` or await the `Worker<T>` handle. Not a single-threaded-loop issue.
- bug.md #3 [P3]: interpreter closures over-capture whole env → spawn rejects if a non-flattenable is merely in scope; LLVM minimal-captures fine.

### stdin bytes — byte-clean
- `lvrt_sysread` `lv_runtime.c:2068` → `lvrt_str_new` (memcpy + explicit length prefix at offset 0 + trailing NUL for C-interop) — strings hold `0x1b`, control bytes, embedded NULs verbatim. Oracle path `RuntimeNatives.cpp:1096` sized read + resize(n). Block overload `lvrt_sysread_block` `lv_runtime.c:2092` reads directly into `lv_block_at`.

### Dispatch rule (matters for a retained component tree)
- Concrete-class-typed receivers bind **statically**; ONLY interface-typed receivers (or methods overridden-below per `resolveDispatch` `Checker.cpp:282-294`) dispatch on the runtime object. Per landed class-method-dispatch design, an unqualified call devirtualizes when the candidate set is provably closed; overridden overload sets sharing an arity are a compile error at the call site (reference §3.4a).
- LA-25 §8.8 ruling (Fable, 2026-07-11): a method reference is definitionally its eta-expansion lambda and carries NO dispatch behavior of its own; `Animal::speak` binds statically like any Animal-typed call; `IAnimal::speak` dispatches dynamically. References become override-correct automatically if the class-method-dispatch pivot extends dynamism. Want polymorphism → reference the interface.

### Misc verified constraints
- `Map.with/.without` by-name missing on LLVM/ELF (bug #18 note in reference.md §6.4.5) — use `m[k]=v` bracket sugar.
- JSON (`json::parse`/JsonValue) LLVM-blocked by bug #30 (`Map<K, recursive-class>` corruption) — reference.md §6.11 coverage note.
- `enum` is full-coverage on ALL engines incl. LLVM (no ABI tag) — exhaustive `match` over enums works everywhere.
- No `static` keyword — class static sides have no declaration plumbing; namespace free functions / labeled ctors are the idiom (reference.md §6.1 byteToString note).
- Prelude is unchecked (Checker runs only on the user program); prelude code cannot rely on `T?`/union flow-narrowing (LLVM misreads it — use -1-sentinel int helpers). A framework shipped as a trident package IS checked user code.
- `.lev` files only; three-branch rule (master/agent1/agent2/agent3); X64Gen/ELF frozen — never extended, never gates anything.
- Attribute args: int/float/bool/string fields, positional, comptime-const (`reference.md` §6.9). Rules fire only in files importing the rule's namespace (bulk `uses` or selective `use`).
- comptime is hermetic (sys* denied; `import(path)` is the one sanctioned read, resolves against trident `assets=[...]`, `--assets` lists consumption); step-bounded (~100M, `--comptime-budget`).
- `--expand` re-emits compilable source and round-trips (24 meta programs pass; `@no-roundtrip` marks comptime-stdout programs).

---

## 9. Fable-model opinions carried forward (from the LA-25 ruling and this session's Fable review)

- **LA-25 §8.8 ruling rationale (Fable, on record):** making references a dynamic-dispatch exception would be "a value that behaves differently from its own definition" (hidden magic, info.md §1) — rejected; dispatch semantics belong to the language's call rule, so any future dispatch pivot flows into references for free. The same logic will govern bound references.
- **Fable review of overview-sonar.md (this session), points that are opinions but Fable-sourced:**
  - Retained-mode is the right architecture for this language: pure arrays + ARC punish per-frame rebuilds; stable identity is what the memory model rewards. The MI-mixin composition (§5, distinct-resolved `changed()` collisions) is the strongest showcase section.
  - The four design-vs-language conflicts identified: (1) handler examples use bound refs that don't exist; (2) the `sonar!` tag-grammar macro exceeds template-macro capability (procedural expansion needed; §8.3 id-binding and §8.6 reactivity additionally need member injection a macro can't reach — `member of` is a rule anchor, fired by attribute matches); (3) parent/children cycles leak by construction absent weak refs — "children hold the strong edge, parent is raw" is not expressible (a class field holding an object IS a counted ref); (4) fluent-API interface satisfaction unresolved (now measured: hard error, §7 above).
  - Smaller flags: events should be classes, not struct+`KeyEventBox` (identity + mutable `handled` is what reference types are for; struct copies lose `handled` propagation); `BorderStyle::None` nominally collides with the language's `None` unit — rename; `App::Current()` needs static plumbing that doesn't exist — use a namespace global; `Theme::FromToml` via JSON hits bug #30 on LLVM — parse TOML in-language; a hardware-cursor story (position/visibility for Input/TextBox focus) is absent from the overview and every TUI needs one (escape writes, no new native); frame-diff helper logic belongs in Sonar's `.lev`, not new natives — keep the native surface to fill/blit/equals primitives.
  - Interface types in the tree are load-bearing, not decorative: children must be held and traversed as `IComponent`/interface types so `paint`/`measure` dispatch on the runtime component; concrete-class-typed storage would statically bind. Do not gate on the class-dispatch pivot.
  - Detach discipline (remove()/clear() nulling parent links) is the workable interim until weak lands, but is fragile — weak refs are the real fix and Sonar is the forcing function for §19 #10.
  - Risk ordering of the six features: Block bulk ops lowest; winsize+signals low (pre-designed, established floor pattern); covariant-return low-but-subtle (the mergeSlot/arity-dispatch reconciliation is the entire design); bound refs low/medium (one real decision: receiver-expression snapshot semantics); procedural macros medium (genuinely new capability, bounded, no ABI change); weak references highest (ABI header + allocator + free-list interaction; only the interpreters get it "for free" via weak_ptr).
  - Much of Sonar's §8.5 attribute layer (`@Shortcut`/`@Timer`/`@Validator`) is buildable TODAY on landed Layer B/D rules; only the tag-grammar `sonar!` macro (§8.1-8.4) gates on procedural macros.

## 10. Design-convention facts (for the doc set)

- Atlantis (`designs/atlantis/techdesign-00-overview.md`) is the house template for framework design sets: a 00-overview anchor (rulings, frozen contracts C1..Cn, LA register, gates/waves, STOP protocol, verified-syntax cheat sheet) + numbered track docs with disjoint ownership + append-only implementation logs. Framework tracks are pure `.lev` packages; never touch `src/**`/`runtime/**`; bugs go to `/bug.md` with repro; completed designs move to a `complete/` subfolder; deferrals for missing features are filed as `designs/requests/request-<slug>.md` (template `request-example-feature.md`); accepted requests move to `designs/requests/accepted/`.
- Sonar deliverable shape agreed this session: 6 language-side tech designs (weak refs, terminal winsize+signals, bound method refs, procedural macros, Block bulk ops, covariant return) + a full framework design set under `designs/sonar/` (overview revision + core/layout/events/components/templates/DI-theming/run-loop/testing + v1.5 reactivity), designed assuming the features land (aggressive pursuit, no v1 hold-backs).
