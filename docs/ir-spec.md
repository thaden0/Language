# Leviathan IR Specification

> refactor_1 session 07 (designs/refactor_1/techdesign-07-ir-spec-conformance-opus.md).
> This document is the written contract between the front end (Lower.cpp) and
> every back end. **The reference semantics are what `src/backend/IrInterp.cpp`
> implements** — where another engine disagrees, IrInterp defines the IR and the
> disagreement is a bug filed to `/bug.md`, annotated here as `> bug #N`.
> The conformance corpus (`tests/conformance/`) checks the engines against each
> other; `tests/conformance/check_ir_spec.sh` (ctest `ir_spec_complete`) checks
> this document against the `Op` enum for 100% opcode coverage.

Source files: `src/ir/Ir.hpp` (the `Op` enum, `Inst`, `Handler`, `IrFunction`,
`IrModule`, `computeFnGlobalRefs`), `src/ir/Lower.cpp` (the producer),
`src/backend/IrInterp.cpp` (the reference executor). Consumers: `IrInterp`
(`--run`/`--ir`), `LlvmGen` (`--build-native`), `CGen` (`--emit-cpp`/`--build`)
and the **frozen** `X64Gen` (ELF; not a project target, never tested here).

---

## 1. The machine model

The IR is a **register machine with high-level object/collection ops** backed
by the shared runtime value model (`src/runtime/RuntimeValue.hpp`). Method,
accessor and constructor bodies — including the in-language stdlib from the
prelude — are themselves lowered to IR functions. Instructions carry
`Symbol*`/`Stmt*` pointers into the semantic model; this is an in-memory IR
tied to one compilation (serialization would index them).

### Register model

- Each `IrFunction` declares `nregs` registers; a frame is a
  `std::vector<Value>` of that size, all slots default-initialized to the
  `Void` value. Registers are **untyped** `Value` slots.
- The first `nparams` registers hold the inputs. For a member function
  (`hasThis == true`), **`this` is register 0** and declared parameters follow
  from r1. For a lowered lambda, **the closure value itself is register 0**
  (that is what `LoadCapture` reads); declared lambda parameters follow.
- The Lowerer allocates a **fresh register per temporary** — registers are
  written-once temps rewritten only before any read on every path.
  `IndexStore`'s COW discipline (§ opcode) depends on this: stale registers
  can be cleared without any other reader observing it.
- Frames are private to a call. There is no register window sharing between
  caller and callee: call ops copy an **argument window** (a contiguous
  register range `[c, c+d)` of the *caller's* frame) into the callee's
  `regs[0..]` by value (`Value` copy = shared_ptr share for reference kinds).

### Value kinds

`VKind`: `Void, Int, Float, Bool, String, Object, Closure, Array, Map, None,
Char, Block, Ast`. Objects/arrays/maps/blocks/closures are references
(`shared_ptr`; assignment shares); primitives are values. `char` is an unboxed
Unicode scalar carried in the `i` payload. Value structs (`Symbol::isValue`)
are `Object`-kind at runtime but are deep-copied at bindings via `CopyVal`.

**Kind-mismatch policy (reference semantics).** The IR trusts the Checker: no
opcode performs a general kind check. On a mismatch the behavior is *defined
but meaningless*, never undefined: an op reads its documented payload field
(`.i`, `.b`, `.f`, `.s`, `.obj`, …) regardless of `kind`, and unpopulated
payloads read as zero/empty. The specific traps that DO exist (integer
division/modulo by zero, shift range, array bounds, unknown native, unresolved
call target) are listed per opcode; everything else silently computes on the
default payload. Ops that require an `Object`/`Array`/`Map` receiver and get
something else yield `Void` (reads) or no-op (writes), as noted per opcode.

### The `Inst` encoding

```cpp
struct Inst {
    Op op;
    int a = 0, b = 0, c = 0, d = 0;   // register numbers / indices / counts
    TokenKind tk = TokenKind::End;    // Arith: the operator token
    std::string sname;                // names: member/method/native/canonical type
    Symbol* sym = nullptr;            // class symbol (NewObject/MakeRange/IsType/ColGet)
    const Stmt* decl = nullptr;       // resolved decl (CallDyn/Arith operator method)
};
```

Operand columns in the opcode reference below name only the fields an opcode
reads; all others are ignored for that opcode.

### Constant table

Each `IrFunction` carries its own `consts` (`std::vector<Value>`) — there is
no module-level pool. `LoadConst` copies `consts[b]` into a register. Constants
are ordinary runtime `Value`s produced by the Lowerer (int/float/bool/string/
char literals, `None`, prelude-folded values). Copying a constant of reference
kind shares the underlying object — the Lowerer only places immutable values
in `consts`.

### Function and global layout (`IrModule`)

```cpp
struct IrModule {
    std::vector<IrFunction> functions;
    std::unordered_map<const Stmt*, int> byDecl;    // member/function decl -> fn index
    std::unordered_map<Symbol*, int> initByClass;   // class -> $init fn index
    const Sema* sema;                               // shapes/symbols for dynamic ops
    int entry;      // synthetic @main (top-level statements), -1 if none
    int ginit;      // prelude-globals init function, -1 if none
    int nglobals;   // size of the global slot array
    std::unordered_map<std::string, int> globalIndex;
    bool columnar;  // compiled with --columnar (SoA Array<struct> layout)
};
```

- **Functions** are referenced by dense index (`Call.b`, `MakeClosure.b`,
  `NewObject.b`). `byDecl` maps a source decl to its lowered index — it is how
  `CallDyn`'s resolved decl and dynamic lookup results are turned into calls.
  A decl **absent** from `byDecl` (an empty prelude body) is a **native
  intrinsic** dispatched by class+method name (see § calling convention).
- **Globals** are a flat `Value` array of `nglobals` slots, indexed by
  `LoadGlobal.b`/`StoreGlobal.b`; `globalIndex` maps source names to slots.
  All slots start as `Void`.
- **Execution order** (`IrInterp::run`): reset loop/exit state, zero the
  globals, then run `ginit` (prelude/global initializers), then `entry`, then
  drain the event loop (or, with tasks enabled — the default — run
  ginit+entry as task 0 under the scheduler; see `Await`). An uncaught throw
  prints `Uncaught <Class>[: <message>]` to stdout and sets exit code 1;
  `env.exit` rides the same unwind flagged clean with its recorded code.
- **`columnar`** is set by the Lowerer and read by LlvmGen (columnar
  descriptors + `lv_cfg_columnar` emission). Interpreters ignore it — they
  stay boxed; `ColGet` documents the equivalence.

### `computeFnGlobalRefs` init-ordering contract

`computeFnGlobalRefs(mod)` (inline in `Ir.hpp`) returns, per function index,
the sorted duplicate-free set of global slots the function references via
`LoadGlobal`/`StoreGlobal`, unioned **transitively over the closures it
CREATES** (`MakeClosure` targets) but **NOT over top-level functions it merely
CALLS**. That depth deliberately mirrors the interpreters' AST free-name walk:
a spawn body's nested lambdas ARE part of its body; a called function's body
is not. It exists for bug #35 (reject route A): a `std::spawn` body reaching a
Promise through a bare global must be rejected identically on every engine —
IrInterp consults the memoized result at the `sysThreadStart` call
(`spawnBodyGlobalPromise`), LlvmGen bakes it into the generated
`lv_spawn_global_check` switch. The `MakeClosure` graph is a DAG (a nested
lambda's fn index is always fresh, never an ancestor's), so the memoized walk
needs only an in-progress guard, no cycle handling.

### Calling convention: `Inst` streams and natives

Four call ops share one shape: `a` = destination register, `c` = start of the
argument window in the caller's frame, `d` = window length. The window is
copied; the callee gets `regs[0..d-1]` = window, remaining registers `Void`.
Returns are by value (`Ret`'s register, or `Void` from `RetVoid`/fallthrough).

Dispatch precedence when a call must be resolved at run time
(reference semantics, `IrInterp`):

1. **Lowered function** — `Call` by index; or `CallDyn` whose `decl` (or
   by-name lookup result) is in `byDecl`.
2. **Engine task natives** (`CallNativeFn` only) — `taskNative` intercepts
   `sysTaskRun`, `sysTaskJoinAll`, `sysAwaitAny2` before the shared
   dispatcher: these must spawn closures / park with the *engine's own* state
   discipline (fiber-carried regs/pc; `thrown_` saved/restored across parks;
   `LV_PARK_DRAINED` → RuntimeException, `LV_PARK_CANCELLED` →
   CancelledException; env.exit while parked resumes the clean-exit unwind).
   `sysThreadStart` additionally runs the bug #35 route-A global-Promise
   reject *before* dispatch (see `computeFnGlobalRefs`).
3. **Class-native intrinsics** — a `CallDyn` decl with an empty body (not in
   `byDecl`) dispatches to `nativeCall(className, methodName, self, rest)`
   (`src/runtime/RuntimeNatives.cpp`): the string/int/bool/float/Array/Map/
   Block method cores. A non-empty `err` out-param raises a RuntimeException.
4. **Native free functions** — `CallNativeFn`'s `sname` through
   `nativeFreeCall` (the `std::sys` floor: `sysWrite`, `sysOpen`,
   `sysTimerStart`, …). Interpreters pass their stdout capture as the sink;
   compiled backends link the C runtime instead. An unknown `sname` raises
   `unknown native '<name>'`.

**Closure convention.** A lowered closure is a `Closure` whose `env[0]`
carries the target function index as an int `Value` under the reserved key
`"@fn"`, plus one entry per captured name. Invoking it (`CallValue`, or the
`CallDyn` field-closure fallback, or the loop dispatcher `invokeClosure`)
passes the closure value itself as arg 0, so the callee's `LoadCapture` reads
resolve against r0. A `Closure` without `"@fn"` cannot be called from IR
(`cannot resolve call target`).

### Exception handling

Each function carries `handlers`, one `Handler` per catch clause **in clause
order**: protected pc range `[start, end)`, `handlerPc`, `bindReg`, and the
caught class `Symbol* type`. A throw sets the engine's `throwing_` flag and
`thrown_` value; the dispatch loop checks the flag after every instruction.
If the faulting pc lies in a handler's range and the thrown value is an
`Object` whose class `isSubclassOrSelf` the handler's type, the flag clears,
the thrown value is written to `bindReg`, and control jumps to `handlerPc`.
Non-`Object` thrown values match no typed handler. If no handler matches, the
frame unwinds: the call returns `Void` with `throwing_` still set, and the
caller's dispatch loop repeats the search. Engine-raised errors
(`raise`/`raiseClass`) construct a prelude exception object
(`RuntimeException`, `CancelledException`, …) with a `message` field, so
`catch` matching is uniformly by class (`IsType` semantics).

---

## 2. Opcode reference

Headings are the exact enumerator names (checked by `ir_spec_complete`).
"Registers" are frame slots of the executing function. Unless stated
otherwise an opcode neither touches control flow nor the heap beyond what its
operand description says.

## Data

### LoadConst

Operands: `a` = dst register, `b` = index into the function's `consts`.

Copies `consts[b]` into `regs[a]`. Reference-kind constants share their
payload (the Lowerer only pools immutable values). No traps; `b` is trusted
to be in range (the Lowerer guarantees it; out-of-range is C++ UB, not a
defined IR behavior).

### Default

Operands: `a` = dst register, `sname` = canonical type spelling.

Writes the §3 auto-construct default for `sname` (`defaultForCanonical`):
unions take `None` if any member is `None`, else the first member's default;
`int`→0, `char`→'\0', `bool`→false, `float`→0.0, `string`→"",
`Array…`→fresh empty array, `Map…`→fresh empty map, `None`→None; anything
else (including class types and an **empty** `sname`, which the Lowerer emits
for a valueless `match`) yields the `Void` value. No traps.

### Move

Operands: `a` = dst, `b` = src.

`regs[a] = regs[b]` — a plain value copy; reference kinds alias. `b` is
unchanged.

### MoveClear

Operands: `a` = dst, `b` = src.

Moves `regs[b]` into `regs[a]` and resets `regs[b]` to `Void` — an ownership
*transfer*. The Lowerer emits it where the source binding dies at the move
(e.g. `x = x.op(...)` hands its buffer to `op` uniquely), so a uniquely-owned
array/map reaches the callee with `use_count() == 1` and COW can mutate in
place (see `CallDyn`'s `b` flag and `IndexStore`).

### CopyVal

Operands: `a` = dst, `b` = src.

`regs[a] = copyValue(regs[b])`: a **deep copy iff** the source is a value
struct (`Object` whose class has `isValue`), recursing into struct-typed
fields while sharing reference-class fields; every other kind behaves exactly
like `Move`. This is the §9 value-struct binding copy. (Producer note: the
Lowerer sets `c = 1` on some `CopyVal`s as a marker for the ownership pass;
the reference executor ignores `c`.)

### Arith

Operands: `a` = dst, `b` = lhs, `c` = rhs, `tk` = operator token, `decl` =
checker-resolved operator method or null (object dispatch only).

Three-way dispatch, in order:

1. If `tk` is `==`/`!=` and **either** side is `None`: primitive None
   equality (None equals only None; `< > <= >=` with a None operand pin to
   `false`).
2. Else if lhs is a non-null `Object` whose class is not `Range`: **object
   operator dispatch** (`rtObjectArith`): use `decl` if set, else look up the
   class's operator method for `tk`'s symbol (`rtOpSymbol`); `(!=)` derives
   from `(==)` negated when only `(==)` exists; `(==)`/`(!=)` on a
   non-value class with no operator method fall back to reference identity;
   any other missing operator raises
   `no operator '<sym>' on '<Class>'`. The method is invoked as an ordinary
   two-register call (receiver, operand).
3. Else **primitive arithmetic** (`arithPrim`, shared by every engine):
   bool ==/!=; Block ==/!= by view identity; char comparisons by scalar;
   string `+` (stringifying the other operand via `valueToString`) and
   lexicographic comparisons; float arithmetic in double with int promotion
   (IEEE: /0 → ±inf/NaN, no float `%`/bitwise); otherwise the int leg over
   the `i` payloads: `+ - *`, trapping `/`÷0 (`division by zero`) and `%`÷0
   (`modulo by zero`) as catchable RuntimeExceptions, comparisons, `| & ^`,
   and `<< >>` trapping a count outside 0..63 (`shift count out of range`).
   Mismatched kinds fall through to the int leg and compute on unpopulated
   `i` payloads (defined-but-meaningless; the Checker prevents it).

Range objects deliberately skip object dispatch and take the primitive path.

### Not

Operands: `a` = dst, `b` = src.

`regs[a] = vbool(!regs[b].b)`. Reads the `b` (bool) payload regardless of
kind — a non-bool operand reads its unpopulated flag as `false` and yields
`true`. No traps.

### Neg

Operands: `a` = dst, `b` = src.

Float source → `vfloat(-f)`; any other kind → `vint(-i)` over the (possibly
unpopulated) int payload. No traps.

## Control

### Jump

Operands: `a` = target pc.

Unconditional branch: `pc = a`. Targets are instruction indices within the
same function.

### JumpIfFalse

Operands: `a` = condition register, `b` = target pc.

If `regs[a].b` is false, `pc = b`; else fall through. Reads the bool payload
regardless of kind (non-bool conditions read `false` and branch).

### JumpIfTrue

Operands: `a` = condition register, `b` = target pc.

If `regs[a].b` is true, `pc = b`; else fall through. Same payload rule as
`JumpIfFalse`.

### Ret

Operands: `a` = value register.

Returns `regs[a]` to the caller; the frame dies. Under `--ownership`
verification the frame's scope-owned allocations are checked dead at this
point (§15); that is a verifier concern, not an execution effect.

### RetVoid

Operands: none.

Returns the `Void` value. Falling off the end of a function's code is
equivalent to `RetVoid`.

### Throw

Operands: `a` = value register.

Sets the engine throw state (`thrown_ = regs[a]`, `throwing_ = true`). The
dispatch loop then performs handler search per § exception handling — within
this function first, else the frame unwinds returning `Void`. Any value kind
may be thrown, but only `Object` values can be caught by typed handlers.

## Calls

(Common shape and dispatch precedence: § calling convention.)

### Call

Operands: `a` = dst, `b` = function index, `c` = arg window start, `d` = argc.

Direct call of `functions[b]` with `regs[c..c+d)` as arguments. The result
(or `Void`) lands in `regs[a]` after the callee returns; a throw in the
callee unwinds into this frame's handler search.

### CallDyn

Operands: `a` = dst, `b` = consumed-receiver flag (see below), `c` = window
start where `window[0]` = receiver, `d` = 1+argc, `sname` = method name
(possibly `Source::name`-qualified), `decl` = checker-resolved decl or null.

Dynamic method call. The receiver's runtime class (`classOfValue`: an
object's `cls`; Array/Map/Block/int/char/string/bool/float map to their
prelude class symbols) determines dispatch. If `decl` is null it is resolved
by arity-aware name lookup (`rtFindMethod` — the bug.md #13 discipline:
prefer the unique arity match; tie or no match keeps first-found in the
flattened inheritance-inclusive walk order). A resolved decl runs as a
lowered function (via `byDecl`) or as a class-native intrinsic (empty body —
§ calling convention step 3). If **no method** of that name exists, the name
is read as a **field** on the receiver; if it holds a callable closure, that
is invoked instead (bug.md #2 — checker cannot distinguish method call from
field-closure call); otherwise raises `cannot resolve call target '<name>'`.

`b == 1` marks a **consumed self-append** (the Lowerer emitted `MoveClear` of
the receiver into the window): if the receiver is then a uniquely-referenced
Array (`use_count() == 1` measured *before* the window copy), the COW flag is
threaded to the native core so `x = x.op(...)` mutates the buffer in place
(bug.md #15). Purely an optimization contract: observable behavior is the
pure-value result either way.

### CallValue

Operands: `a` = dst, `c` = window start where `window[0]` = callable value,
`d` = 1+argc.

Calls a first-class closure: reads the function index from
`window[0].closure->env[0]["@fn"]` and calls it with the whole window (the
closure value stays arg 0 = callee r0, so its captures are addressable via
`LoadCapture`). A non-closure value, a closure without `"@fn"`, or an empty
env raises `cannot resolve call target`.

### CallNativeFn

Operands: `a` = dst, `c` = arg window start, `d` = argc, `sname` = native
free-function name (the `std::sys` floor).

Native free call, with the engine-level intercepts described in § calling
convention: (1) `sysThreadStart` runs the bug #35 global-Promise reject
first; (2) `sysTaskRun`/`sysTaskJoinAll`/`sysAwaitAny2` are handled by the
engine's `taskNative` (they park/spawn with engine state); (3) everything
else goes to the shared `nativeFreeCall` dispatcher, with the interpreter's
stdout capture as sink. A non-empty `err` raises a RuntimeException; an
unrecognized name raises `unknown native '<name>'`. After a successful
native return, an `env.exit` request (recorded by `sysExit`) converts into
the clean-exit unwind (`exiting_ = throwing_ = true`).

Backends: compiled backends do not share the interpreter's native table —
they lower each native they support against the C runtime (`liblvrt.a` for
LLVM, the emitted mini-runtime for CGen) and **reject at compile time** the
natives they don't (`native backend: native '<name>'`). The conformance
skip files record the current per-leg gaps (emit-C++: no file/task floor).

## Objects

### NewObject

Operands: `a` = dst, `b` = `$init` function index or -1, `sym` = class symbol.

Allocates a fresh `Object` with `cls = sym` and empty fields, writes it to
`regs[a]`, then (if `b >= 0`) calls `functions[b]` with the new object as its
single argument — the class's `$init` (field initializers; the lowered
constructor body is called separately as an ordinary `CallDyn`/`Call`). The
allocation is tracked by the ownership/memory verifiers when active.

### GetMember

Operands: `a` = dst, `b` = object register, `sname` = `"name"` or
`"Source::name"`.

Accessor-aware member read. If the receiver's class (or a base, depth-first
through `bases`) declares a **get accessor** for the plain name, it is
invoked (`callDecl(getter, {receiver})`) and its result is the value.
Otherwise a raw field read at the storage key (`rtKeyFor`, honoring the
`Source::` qualifier for distinct slots): missing key → `Void`. A non-Object
receiver yields `Void`. No traps.

### SetMember

Operands: `a` = **value** register, `b` = object register, `sname` = member
name (possibly qualified).

Accessor-aware member write (note the operand order: `a` is the value, not a
destination). A set accessor on the class (or base) is invoked with
(receiver, value); otherwise raw `fields[key] = value`. A non-Object receiver
is a silent no-op. No traps.

### RawGet

Operands: `a` = dst, `b` = object register, `sname` = key.

Raw field read **bypassing accessors** — the form the Lowerer emits inside a
class's own accessor bodies and `$init` (the IR carries no re-entrancy guard;
raw access is a distinct op instead). Missing key → `Void`; non-Object
receiver → `Void`. The Lowerer may set `d` = slot number as metadata for the
compiled backends; the reference executor ignores it.

### RawSet

Operands: `a` = value register, `b` = object register, `sname` = key.

Raw field write bypassing accessors: `fields[key] = regs[a]`. Non-Object
receiver: silent no-op. (Same `d`-slot metadata note as `RawGet`.)

### RawGetWeak

Operands: `a` = dst, `b` = object register, `sname` = key (a weak slot).

Weak-slot read: looks the key up in the object's `weakFields` (a
`weak_ptr<Object>` table, disjoint from `fields`). A live target produces an
ordinary strong `Object` value (+1 strong ref); a dead/absent slot produces
`None` — never a dangling reference. Non-Object receiver → `None`.

### RawSetWeak

Operands: `a` = value register, `b` = object register, `sname` = key.

Weak-slot write: storing an `Object` value records a non-owning
`weak_ptr` to it; storing anything else (including `None`) **erases** the
slot. The slot never keeps its target alive. Non-Object receiver: no-op.

## Collections

### NewArray

Operands: `a` = dst, `c` = element window start, `d` = element count.

Allocates a fresh array from `regs[c..c+d)`. A `Range` object element is
**spread**: its `start..end` ints (inclusive; empty when start > end … the
spread loop emits `lo..hi`) are appended elementwise instead of the Range
itself. Elements are copied by value (reference kinds share).

### NewArraySized

Operands: `a` = dst, `c` = arg window start, `d` = argc (0 or 2).

`Array()` / `Array(n, fill)` construction: `d == 0` → fresh empty array;
`d == 2` → `regs[c].i` copies of `regs[c+1]` (a reference-kind fill is shared
by every element, not cloned). Other argc values produce an empty array. A
negative count produces an empty array (the fill loop doesn't run).

### NewMap

Operands: `a` = dst.

Allocates a fresh empty map. Maps are **insertion-ordered vectors** of
key/value pairs with linear lookup; key equality is `keyEquals` (primitives
by value, floats through the canonical bit pattern `lv_canon`, value structs
field-wise recursive, reference classes/Blocks by identity).

### NewBlock

Operands: `a` = dst, `c` = size register.

`Block(n)` (Track 03 §3): a fresh zeroed byte buffer of length `regs[c].i`
(negative → length 0). Blocks are reference values; slices alias the same
backing store.

### NewBlockStr

Operands: `a` = dst, `c` = string register.

`Block::fromString(s)`: a fresh block whose bytes are the string's UTF-8
bytes; `off = 0`, `len = size`.

### MakeRange

Operands: `a` = dst, `b` = lo register, `c` = hi register, `sym` = the
prelude `Range` class symbol.

Allocates a `Range` object with fields `start = regs[b]`, `end = regs[c]`
(no accessors, no `$init`). Ranges print as `lo..hi`, spread in `NewArray`,
and iterate via `IterLen`/`IterAt`; as an `Arith` receiver they take the
primitive path (never operator dispatch).

### GetIndex

Operands: `a` = dst, `b` = base register, `c` = index register.

Indexed read `base[index]`. A `([])` **get accessor** on the base's class
(including the prelude's Map/Block classes, and user classes) wins;
otherwise the native Array path bounds-checks `regs[c].i` and raises
`index <i> out of bounds (length <n>)` as a catchable RuntimeException.
A base that is neither accessor-dispatched nor an Array yields `Void`.

### IndexStore

Operands: `a` = dst register receiving the **new base**, `b` = base, `c` =
index, `d` = value register.

Indexed write with **pure-value COW surface**: the result register `a` holds
the post-store base, which the Lowerer rebinds over the source variable.
Object bases dispatch a `([])` set accessor with (base, index, value) and
return the same reference. Array/Map bases mutate **in place** when uniquely
owned (`use_count() == 1`) and otherwise copy-then-store, returning the
fresh container (arrays: out-of-range int index is a silent no-op store;
maps: upsert by `keyEquals`). Any other base kind passes through unchanged.
To make unique ownership observable, the executor first clears `regs[a]` and
— when followed by the rebind chain `CopyVal t <- a; Move L <- t` — the
stale `t` from a previous loop iteration (registers are write-once temps, so
the clears are invisible to the program). This clearing is part of the op's
reference semantics: it is what lets a uniquely-owned base take the in-place
path on every engine identically.

### IterLen

Operands: `a` = dst, `b` = iterable register.

Iteration length: Array → element count; Map → entry count; `Range` object →
`hi - lo + 1` (0 when hi < lo); anything else → 0. Written as an int.

### IterAt

Operands: `a` = dst, `b` = iterable register, `c` = index register (int).

Iteration element at `regs[c].i`: Array → element (out of range → `Void`);
Map → a fresh prelude `Pair` object with `first`/`second` = the entry's
key/value (insertion order); `Range` → `start + idx` as int; anything else →
`Void`. `for (x in it)` lowers to an `IterLen`-bounded `IterAt` loop.

### ColGet

Operands: `a` = dst, `b` = array register, `c` = index register, `d` = column
slot number (slotK), `sname` = field name, `sym` = element struct class.

The fused element-field read of a columnar `Array<struct>`
(techdesign-columnar-arrays.md §5.4): `dst = arr[index].field`. **Reference
semantics are exactly `GetIndex` followed by `GetMember`** — the boxed
interpreters execute it precisely that way (bounds trap included). LlvmGen
consumes it specially under `--columnar` (the default): the field is read
straight from column `d` of the SoA layout with no element gather; the
boxed fallback (array still empty/boxed) reads the element field the boxed
way. The Lowerer pairs escaping gathers with `VFree` of the previous step's
temporary.

### IsType

Operands: `a` = dst (bool), `b` = value register, `sname` = tested canonical
type spelling, `sym` = class symbol or null.

Runtime type test (`is`, match arms, catch typing). Union spellings
(`"A | B"`) test each member (bracket-aware split) and OR the results. A
single member matches by kind: `None`/`int`/`char`/`string`/`bool`/`float`
by `VKind`; a spelling starting with `Array`/`Map` by container kind
(element types are **erased** — `Array<int>` matches any array); `Block` by
kind; otherwise, when `sym` is set and the value is an object,
`isSubclassOrSelf(value.cls, sym)` walks the declared bases. Anything else →
false. Never traps.

### Await

Operands: `a` = dst, `b` = promise register.

Suspends until the promise settles, then yields its value (LA-30). A
non-Object operand passes through unchanged (awaiting a plain value is the
value). With the task substrate enabled (the default): if the promise
already polls ready (`rtPollPromise` bit 0), proceed **without yielding**
(doc 5 C4); otherwise park the fiber on the promise (`lv_task_park_on` —
regs/pc are fiber-carried; `thrown_` is saved/restored around the park).
Park outcomes: `env.exit` while parked resumes the clean-exit unwind;
`LV_PARK_DRAINED` (loop drained, promise still pending) raises
`await: event loop drained with promise unresolved` (fabricating the
default value is the disease None exists to cure); `LV_PARK_CANCELLED`
raises `CancelledException` (B2 cancellation delivered at the park). Under
the legacy `LANG_PUMP=1` hatch the engine instead pumps the event loop
inline until the promise's `ready` field is true or work runs out. After
resumption: a Worker promise with `failed == true` rethrows its
`failMessage` as a RuntimeException (plain promises lack the field);
otherwise `regs[a]` = the promise's `value` field (`Void` if absent).
Backends: interp and LLVM implement the full park protocol; emit-C++ does
not lower `Await` at all (compile-time reject — see the conformance cgen
skip file). The frozen ELF backend is out of scope.

### LoadGlobal

Operands: `a` = dst, `b` = global slot index.

`regs[a] = globals[b]`. Slots are `Void` until first written (`ginit` runs
before `entry` — § function and global layout). Participates in the
`computeFnGlobalRefs` contract.

### StoreGlobal

Operands: `a` = **src** register, `b` = global slot index.

`globals[b] = regs[a]` (note: `a` is the source; there is no destination).
Participates in the `computeFnGlobalRefs` contract.

## Closures

### MakeClosure

Operands: `a` = dst, `b` = target function index.

Allocates a fresh `Closure` whose `env[0]` holds the reserved `"@fn"` entry
= `vint(b)`. Subsequent `CaptureVar`s populate the same env frame. The
resulting value is called via `CallValue` (or the `CallDyn` field-closure
fallback / loop dispatch), with itself as arg 0.

### CaptureVar

Operands: `a` = closure register, `b` = source register, `sname` = captured
name.

Snapshot capture: `closure.env[0][sname] = regs[b]` (by value; reference
kinds alias — mutation through a captured reference is visible, rebinding
the source variable is not). Non-closure `regs[a]` is a silent no-op.
Storage note: env keys are `string_view`s interned into the instruction's
`sname` — the `Inst` outlives every closure it builds.

### LoadCapture

Operands: `a` = dst, `sname` = captured name.

Reads a capture from **this-closure = r0**: `regs[a] =
regs[0].closure->env[0][sname]`, else `Void` when r0 is not a closure, has
an empty env, or lacks the name. Only meaningful inside functions lowered
from lambdas (where the calling convention places the closure in r0).

## Output

### Print

Operands: `a` = value register.

Emits `valueToString(regs[a])` to program stdout — in the interpreters,
appended to the run's capture buffer (`interpEmitStdout`; under terminal raw
mode the buffer drains and writes through to fd 1 so cursor-report
handshakes can't deadlock). Stringification is the shared engine-identical
`valueToString`: ints/floats via `std::to_string`, chars as UTF-8, bools as
`true`/`false`, Ranges as `lo..hi`, other objects as `<object>`, closures as
`<closure>`, arrays as `[a, b]`, maps as `{k: v}`, `None` as `None`, `Void`
as the empty string, Blocks as `Block(len=n)`.

### PrintNl

Operands: none.

Emits a single `\n` to program stdout (same channel as `Print`).
`console.writeln(x)` lowers to `Print x; PrintNl`.

## Escaping-tier ARC

### VFree

Operands: `a` = value register.

Declares a DEAD standalone value-struct copy — e.g. the return-site copy of
a struct-returning call already consumed (copied out) by the caller, or a
columnar gather temporary after its last field read (§15 escaping-tier ARC).
**GC'd/refcounted engines no-op** (the reference executor does nothing; the
`shared_ptr` frees the copy at frame death); the frozen ELF backend used it
to reclaim eagerly. It is a liveness annotation, never an observable effect:
no register content changes.

---

## 3. Backend notes (who consumes what specially)

| Concern | interp (`--run`/`--ir`) | LLVM (`--build-native`) | emit-C++ (`--build`) |
|---|---|---|---|
| `ColGet` | boxed `GetIndex`+`GetMember` | SoA column read when `IrModule::columnar` | boxed path |
| `Await` / task ops | full park protocol (lv_task) | full park protocol (liblvrt) | **not lowered** (compile reject) |
| `CallNativeFn` table | shared `nativeFreeCall` | per-native lowering vs liblvrt | smallest floor (see `skip.cgen.txt`) |
| `VFree` | no-op | no-op (ARC) | no-op |
| bug #35 spawn guard | `computeFnGlobalRefs` at call | baked `lv_spawn_global_check` | n/a (no spawn) |
| stdout | captured buffer, printed at end | direct fd 1 (+`[heap]` meter on fd 2) | direct fd 1 |

The conformance corpus (`tests/conformance/`, ctest `conf_<leg>_<area>_<name>`)
holds these legs byte-identical on stdout + exit code; per-leg gaps are the
justified entries in `tests/conformance/skip.<leg>.txt`.
