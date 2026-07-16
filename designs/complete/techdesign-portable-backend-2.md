# Tech Design — Portable Backend Pivot, Track B: Runtime v2, Link Driver, Platform Floor

**Status:** approved for implementation (owner decision, 2026-07-05)
**Authored by:** Fable-class model. **Implemented by:** Sonnet-class implementation agents.
**Companion:** `designs/complete/techdesign-portable-backend.md` (Track A: IR ARC ops + LLVM parity).
Track A's §0 (mission, tracks, merge rules), §2 (roadmap/timeline — authoritative), and
§3 (target architecture) apply to this track verbatim and are not repeated here.

---

## 0. Read this first

### 0.1 Role of this track

Track B builds everything **around** the LLVM code generator so Track A can reach parity:

1. **Runtime v2** — a portable, program-agnostic native runtime in **plain C (C17)**,
   replacing the throwaway `src/NativeRuntime.cpp` (which stays on disk, unused, once
   retired — nothing is deleted).
2. **The ABI contract** (§2) — the normative interface between generated code and the
   runtime. Track B owns the file `runtime/lv_abi.h`; Track A codes against it.
3. **The link driver** — one-step `lang --build-native out prog.ext` (emit object → link
   runtime archive), plus fixing the hardcoded `g++` in `--build`.
4. **The platform floor** — `lv_plat_*`: the only place the runtime touches an OS.
   POSIX now; Windows/macOS/Android per the roadmap.
5. **Target bring-up** — aarch64-linux cross builds, then macOS, then Windows.

### 0.2 STOP protocol (model escalation)

Identical to Track A §0.3 — read it now if you haven't. Summary: if the design is wrong
in a way that requires an architectural choice (an ABI/layout that can't work, a contract
change, a platform approach that fails) and you are a Sonnet-class model: **STOP**, write
findings in §10 (Implementation log), commit WIP to `track-b-runtime`, and state that a
Fable-class model must revise the design. Mechanical adaptation (a renamed symbol, a
missing flag, an offset corrected after verification against code): proceed and log.
**The ABI contract (§2) may not be changed unilaterally by either track — contract
changes are always STOP events.**

### 0.3 Frozen / do-not-touch (same as Track A §0.4)

`src/X64*.{hpp,cpp}` read-only reference; oracle semantics untouchable; never edit
`.expected` files; all existing test lanes green at every merge. Additionally for this
track: do not edit `src/LlvmGen.*`, `src/Lower.*`, `src/Ir.hpp` — those are Track A's.
Coordinate through the contract and the implementation logs.

### 0.4 File map (Track B owns)

```
runtime/lv_abi.h          # §2 — the normative contract (values, layouts, functions)
runtime/lv_runtime.c      # §3 — allocator, ARC, strings, collections, objects, natives
runtime/lv_loop.c         # §5 — event loop, timers, fd watches, await   (may fold into lv_runtime.c)
runtime/lv_plat.h         # §5 — platform floor interface
runtime/lv_plat_posix.c   # §5 — Linux/macOS implementation
runtime/lv_plat_win32.c   # §7 — Windows implementation (B-M5)
runtime/selftest.c        # §3.5 — standalone C tests, no LLVM needed
src/main.cpp              # §4 — driver additions (no Track A ordering constraint; A-M1 no longer touches main.cpp — see techdesign-portable-backend.md §0.2)
CMakeLists.txt            # Track-B region only (Track A doc §10.3)
```

---

## 1. Milestone plan and interlock with Track A

| Milestone | Deliverable | Must land before |
|---|---|---|
| **B-M1** | `lv_abi.h` + runtime core (alloc/ARC/strings/arrays/maps/objects/exception state) + selftest | Track A's A-M2 (it codes against the contract) |
| **B-M2** | `liblvrt.a` CMake target; `--build-native`; linker probe; `--build` g++ fix | A-M2's lane flip to the archive |
| **B-M3** | Event loop + sockets/files natives complete; platform-floor audit (no OS calls outside `lv_plat_posix.c`) | Track A's A-M5 |
| **B-M4** | aarch64-linux cross: triple plumbing, per-triple runtime archive, qemu-user lane (if qemu present) | Gate G2 (aarch64) |
| **B-M5** | macOS and Windows floors + bring-up (environment-gated) | Gate G2 (mac/win) |

B-M1 needs **no LLVM at all** — the runtime is plain C tested by `selftest.c`. Start
immediately; do not wait for Track A.

---

## 2. The ABI contract (normative — lives in `runtime/lv_abi.h`)

Everything in this section is the interface Track A emits against. Where a detail says
*extract*, the implementing agent extracts the fact from the named source (code is truth;
header comments can lag — see Track A hurdle H-2), records it as a comment in
`lv_abi.h`, and notes it in §10. The **shapes** below are normative; the *extracted
constants* fill them in.

### 2.1 Boundary rule

All values cross the generated↔runtime boundary as **`LvValue*` pointers** — out-param
first, then inputs. **No function in this ABI passes or returns `LvValue` by value.**
(Rationale: clang lowers 16-byte structs differently per target — SysV splits into two
scalars, Win64 passes by pointer — and hand-authored LLVM declarations would silently
mismatch. The pointer rule makes the ABI trivial on every target. This is Track A hurdle
H-1; it binds Track B equally.)

### 2.2 Fixed-width discipline

`lv_abi.h` uses only `<stdint.h>` types. Everything is little-endian, 8-byte aligned.
The header must compile as both C17 and C++ (`extern "C"` guards) — the compiler driver
may include it later.

### 2.3 Values

```c
typedef struct LvValue { int64_t tag; int64_t payload; } LvValue;   /* 16 bytes */

enum {  LV_VOID = 0, LV_INT = 1, LV_FLOAT = 2, LV_BOOL = 3, LV_STR = 4,
        LV_OBJ  = 5, LV_ARR = 6, LV_MAP  = 7, LV_NONE = 8, LV_CLO = 9 };
```

Numbering follows the CGen/X64Gen reference (`src/X64Gen.hpp:16-23`) — **not** the
`VKind` enum order (`src/RuntimeValue.hpp:22`), which differs (Track A hurdle H-3).
`LV_FLOAT` stores the `double` bit pattern in `payload` (`memcpy`, no casts through
`long`). `LV_BOOL`/`LV_INT` store the value; tags 4–7 and 9 store pointers per §2.4.
Tag-5 payloads may point **inline into a dense buffer** (value structs) — one reason
structs are never refcounted; never assume a tag-5 pointer has an allocation header.

### 2.4 Heap layouts

```c
/* Allocation header, immediately BEFORE every counted heap payload:           */
typedef struct LvHeader { int64_t rc; int64_t size; } LvHeader;  /* payload-16 */
/* rc: -1 = arena allocation (never counted); 0 = fresh/unowned; >=1 = owned.  */
/* Extract the exact rc transition rules (when 0->1, when release at 0 frees   */
/* vs. skips, the "concat consumes unowned rc-0 operands" behavior) from       */
/* X64Gen genRetain/genRelease/genHfree/emitStrTempFree and record them here.  */

/* string  (LV_STR):  payload -> { int64_t len; char bytes[len]; char nul; }   */
/*   Literals use the same descriptor shape in constant data with NO LvHeader; */
/*   they are exempt from ARC by the heap-region check (§2.5). Verify the      */
/*   descriptor layout against X64Gen internString before freezing.            */

/* object  (LV_OBJ):  payload -> { int64_t classId; int64_t dyn;               */
/*                                 LvValue slots[nslots]; }                    */
/*   slot i at byte offset 16 + 16*i (the §7 fixed-offset contract — matches   */
/*   the ELF backend's mov [obj+16+slot*16]; verify against genMkObj/$init).   */
/*   `dyn` heads the dynamic-key fallback list; extract its node shape.        */

/* array   (LV_ARR):  payload -> { int64_t len; LvValue elems[len]; }          */
/*   Dense value-struct arrays store flat records instead of LvValues —        */
/*   extract the discrimination + stride rules from genMkArr/genIdxSet.        */
/*   Arrays are exact-sized (allocator records requested size; append copies). */

/* map     (LV_MAP):  payload -> { int64_t len; LvPair entries[len]; }         */
/*   LvPair = { LvValue k, v; } (32B). Insertion order is observable — keep it.*/
/*   Missing-key insert copies the map; existing-key update follows COW.       */

/* closure (LV_CLO):  payload -> { int64_t fnIndex; captures... } — extract    */
/*   the capture record shape (named snapshot captures) from X64Gen            */
/*   MakeClosure/CaptureVar/genCallClosure handling.                           */
```

### 2.5 Memory API

```c
void* lvrt_halloc(int64_t size, int tier);   /* tier: LV_TIER_HEAP | LV_TIER_ARENA   */
void  lvrt_retain (const LvValue* v);
void  lvrt_release(const LvValue* v);        /* recursive free at rc 0->free         */
void  lvrt_vfree  (const LvValue* v);        /* dead standalone value-struct copy    */
int64_t lvrt_arena_save(void);               /* frame prologue                       */
void    lvrt_arena_restore(int64_t mark);    /* every return path ($init adopts the  */
                                             /* constructing frame's window — no     */
                                             /* save/restore inside $init)           */
int64_t lvrt_live_bytes(void);               /* churn-harness meter (live, peak)     */
int64_t lvrt_peak_bytes(void);
```

Implementation requirements (all ported behaviors, not inventions — the ELF backend is
the map): regions acquired from `lv_plat_map` (not malloc) and recorded in a **region
registry** so `lvrt_retain/release` can range-check whether a tag-4/5/6/7/9 payload is
runtime-owned at all — string literals (LLVM constant data) and out-of-region pointers
are silently skipped, exactly like the ELF backend's heap-range guard. Arena = its own
region with save/restore watermark; arena allocations carry `rc = -1`. Heap tier uses
**power-of-two size-class free lists** (X64Gen uses 28 classes, 2^4..2^31 —
`X64Gen.hpp:86`); a freed block returns to its class list. `lvrt_release` frees
**recursively** (objects release slots, arrays elements, maps entries, closures
captures — the "owner counts its contents" discipline; §15's hard-won bug list is the
checklist: boxed-array buffers retain elements; thrown exceptions owned while in flight;
catch rebind releases the previous binding; timer/watch registries own their callbacks).
An `LV_ARC_TRACE`-style env-gated trace hook (X64Gen has one — `X64Gen.hpp:60-61`) pays
for itself within a week; include it.

COW: `lvrt_idxset(LvValue* out, LvValue* base, LvValue* idx, LvValue* v)` mutates in
place when the base header's `rc == 1` (boxed arrays, dense arrays, existing-key map
updates) and copies otherwise. The *caller-side* pre-release that makes uniqueness
readable in loops is Track A's job (their §4.2); your job is the rc==1 test and both
paths.

### 2.6 Program registration (generated data → runtime)

Generated code is the only party that knows the program. At startup it hands the runtime:

```c
typedef void (*LvDispatchFn)(int64_t fnIndex, LvValue* ret, LvValue* args, int64_t argc);

/* member kinds — one shared table reproduces X64Gen's four separate dispatch
   switches (getm getters / setm setters / opm operators / callm methods):   */
enum { LV_M_METHOD = 0, LV_M_GET = 1, LV_M_SET = 2, LV_M_OP = 3 };

typedef struct LvClassInfo {
    const char*  name;
    int64_t      classId;
    int64_t      nslots;
    int32_t      isValue;              /* struct semantics: copy, never refcount */
    const char** slotNames;            /* index = slot                           */
    /* subtype closure for issub/IsType/catch: extract representation from      */
    /* X64Gen genIsSub (per-class base+interface id list is the expected shape) */
    const int64_t* subtypeIds; int64_t nSubtypeIds;
    /* member table for callm/getm/setm/opm dynamic residue; methodKinds is    */
    /* LV_M_* per entry (NULL == all LV_M_METHOD); LV_M_OP names are the       */
    /* operator symbol text ("+", "==", ...)                                   */
    const char** methodNames; const int64_t* methodFnIndex;
    const int8_t* methodKinds; int64_t nMethods;
} LvClassInfo;

void lvrt_register(const LvClassInfo* classes, int64_t nclasses, LvDispatchFn dispatch);
```

**Emission rule (learned the hard way at A-M2):** the generated constant table is an
array of the **full** struct — `{ptr,i64,i64,i32,ptr,ptr,i64,ptr,ptr,ptr,i64}` — never a
field-prefix subset: a shorter per-element struct changes the *array stride* and the
runtime reads garbage past entry 0. Unused tail columns are null/0.

**Key comparison (ruled 2026-07-05):** slot names, dyn-list keys, and capture keys
compare pointer-first with a content (strcmp) fallback. Codegen should still intern its
key globals (the fast path), but a missed dedup degrades to a slow lookup, never a
silent field miss.

`dispatch` is the generated trampoline (Track A §5.3) — the **only** way the runtime
calls into generated code (closures, method dispatch, event-loop callbacks, await
continuations). Dispatch helpers the runtime provides over these tables:
`lvrt_getm/setm/opm/callm`, `lvrt_issub`, `lvrt_isvalueclass`, `lvrt_copyval`,
`lvrt_obj_new(out, classId)`. Ownership: `getm/opm/callm` results are **+1 (transfer)**
— generated code must not add a wrap retain on them; `setm` does the value-slot ARC
internally (release-old-then-retain-new) on its field path and none when dispatching a
declared setter. The scalar core / stringify / IO entry points the backend's scalar ops
emit (`lvrt_truth/not/neg/arith/to_string/print_val/print_nl/syswrite/readline`) are
ratified contract members — `lv_abi.h`'s "Scalar core" section is their normative text.

### 2.7 Natives and ownership contracts

One table in `lv_abi.h` lists every `CallNativeFn` name with its C entry point and its
**ownership contract** — this was the ELF work's nastiest bug class ("native
`at`/`keys`/`values` not honoring the +1 transfer contract", info.md §15), so the
contract column is mandatory, per parameter and return:

| native | entry | contract (seed rows — complete from IrInterp/RuntimeNatives at B-M1) |
|---|---|---|
| `length`, `has` | `lvrt_n_length`… | borrows args; returns unboxed |
| `at`, `keys`, `values` | … | borrows args; **returns +1 (transfer)** |
| `add` (array append) | … | consumes base when uniquely owned (MoveClear path); returns +1 |
| string ops (concat/subStr/case/toString/trim) | … | **consume unowned (rc-0) heap-string operands**; return fresh rc-0/+1 — extract exact rule |
| `sysWrite/sysRead*/sysOpen/sysClose/sysStat` | … | borrow; readLine/recv return fresh string +1; recv `None` = EOF |
| sockets (`sysTcp*`, `sysSend`, `sysRecv`) | … | borrow; see B-M3 |
| timers (`timerAdd/timerCancel`, now-ns) | … | registry **owns** the callback (+1 on add, release on cancel/fire-last) |

Enumerate the full name list with `grep -n '"' src/RuntimeNatives.cpp` +
`grep -n 'sname ==' src/IrInterp.cpp src/X64Gen.cpp` and reconcile with Track A's A-M2
step-0 list (both logs must show the same list).

**Behavioral reference: the oracle** (`src/RuntimeNatives.cpp`, `src/Eval.cpp`) — port
behavior from readable C++, and take *ownership* contracts from X64Gen/§15 notes.
Output formatting must byte-match `valueToString` (`src/RuntimeValue.hpp:77-111`); the
emit-C++ backend's embedded runtime (`src/CGen.cpp` `kRuntime`) is the existing portable
reference for print/format/equality code — crib from it.

### 2.8 Exception state

```c
void    lvrt_throw_set(const LvValue* v);   /* retains v (in-flight ownership) */
int32_t lvrt_throwing(void);
void    lvrt_thrown(LvValue* out);          /* peek (no transfer)              */
void    lvrt_catch_bind(LvValue* bindSlot); /* clear flag; release previous    */
                                            /* binding; transfer thrown value  */
void    lvrt_uncaught(void);                /* report byte-matching the oracle; exit */
void    lvrt_raise_oob(int64_t idx, int64_t len);   /* index OOB -> RuntimeException */
```

Pending-throw model only — no unwind tables, no landingpads (Track A §7 owns codegen).
Runtime failures raise real catchable exceptions (OOB, missing map key, unresolvable
dispatch), matching the interpreters' messages exactly.

### 2.9 Process entry and the event loop

The **runtime owns `main()`** (from A-M5's flip; before that the old scalar wrapper
still works):

```c
int main(int argc, char** argv) {
    lv_rt_init(argc, argv);      /* regions, registries, argv capture (argv.md) */
    extern void lv_main(LvValue* ret);
    LvValue r; lv_main(&r);
    while (lvrt_loop_has_work()) /* dispatch while live work remains (§13)      */
        lvrt_loop_step();
    return lv_rt_exit_code();
}
```

Loop API *(text updated at the 2026-07-05 maintenance pass #2 to the names B-M3
actually ratified into `lv_abi.h` — the sketch here predated B-M3)*: the natives are
`lvrt_systimerstart/systimercancel/syswatch/sysunwatch` (registry owns the callback,
§2.7), and the loop is driven by exactly two entry points,
`lvrt_loop_has_work()`/`lvrt_loop_step()`. There is **no `lvrt_await`** — `Await`'s
codegen pumps the loop inline, mirroring X64Gen's own inline pump
(`src/X64Gen.cpp:3573-3593`): read the promise's `ready` field, check
`lvrt_throwing()`, `has_work ? step : done`, then read `value`. Field names come from
IrInterp's `Await` case; the pointer-first/content-fallback key ruling makes the
`"ready"`/`"value"` key globals safe without shared interning. Callbacks are `LvValue`
closures invoked through the registered trampoline.

### 2.10 Inline hot-state globals (contract addition, ratified 2026-07-06 — Fable maintenance pass #4)

The two measured, STOP-gated perf levers from Track A's A-M6 closeout (doc-1 §12,
2026-07-06 entry: `lvrt_throwing()` after every call-like op, and the
`lvrt_arena_save`/`lvrt_arena_restore` pair around every value-struct-allocating frame,
are cross-`.o` calls; X64Gen reads/writes the equivalent state inline) are resolved by
exporting the two hot words directly:

```c
extern int32_t  lv_g_throwing;       /* THE throw flag lvrt_throwing() returns      */
extern uint8_t* lv_g_arena_cursor;   /* THE arena bump cursor; save reads it,       */
                                     /* restore writes it — nothing else            */
```

Contract:
- Generated code MAY **load** `lv_g_throwing` in place of calling `lvrt_throwing()`.
  It must never write it (throw_set/catch_bind own the transitions, including the
  thrown-value ownership those entail).
- Generated code MAY implement the save/restore pair as a direct load/store of
  `lv_g_arena_cursor` (today's `lvrt_arena_restore` is exactly `cursor = mark`, no
  poisoning, no bookkeeping — verified against `lv_runtime.c` before ratifying).
- Both are valid only after `lv_rt_init` — guaranteed unconditionally since the A-M5
  entry flip (`lv_entry.c` runs `lv_rt_init` before `lv_main`). Do not read them from
  code that can run pre-init (nothing generated can, post-flip).
- The function entry points (`lvrt_throwing`, `lvrt_arena_save`, `lvrt_arena_restore`)
  **stay in the ABI unchanged** — the selftest, `lv_loop.c`, `lv_entry.c`, and any
  non-inlining caller keep using them; the globals are a fast path, not a replacement.

Runtime-side implementation is a rename of the two internal statics to the exported
names (accessor functions kept, all internal uses kept). This narrow `runtime/lv_abi.h`
+ `runtime/lv_runtime.c` edit is **owner-authorized for the Track A agent implementing
A-M7** (doc-1 §9b) so the lever lands as one coherent change — the A-M5 precedent
(narrowly-scoped, logged, cross-verified runtime edits by Track A) applies; everything
else under `runtime/**` remains Track B's.

---

## 3. Milestone B-M1 — runtime core

Build order inside the milestone (each step selftest-covered before the next):

1. `lv_plat.h` + `lv_plat_posix.c` minimal slice: `map/unmap`, `write/read`,
   `now_ns`, `exit` — enough for the allocator and printing. (The floor *interface* is
   complete in §5; implement the slice you need, but only ever through the interface.)
2. Allocator: regions, registry, size-class free lists, arena save/restore, live/peak
   meters. Selftest: alloc/free/reuse patterns, arena reset bulk-free, range-check
   rejects a stack pointer and a static string.
3. ARC: retain/release/recursive-free over every layout; the trace hook. Selftest:
   build object graphs by hand (arrays of objects, maps, closures), release the root,
   assert live-bytes returns to baseline (a C-level mini-churn — the real churn corpus
   arrives with Track A at G1).
4. Strings: descriptor ops (concat/eq/subStr/indexOf/trim/case/int↔str) with the
   consume-unowned discipline. Print/format helpers byte-matched to `valueToString`.
5. Arrays/maps/ranges: cores + `lvrt_idxget/idxset` with the rc==1 COW split; dense
   value-struct arrays (after extracting the stride rules).
6. Objects + dispatch: `lvrt_obj_new`, slot get/set, dynamic-key fallback list,
   `getm/setm/opm/callm/issub` over the §2.6 tables (selftest registers a fake class
   table + a fake trampoline).
7. Exception state (§2.8).

**Acceptance:** `runtime_selftest` target green under
`gcc`/`clang` **and** under `-fsanitize=address,undefined` (a CMake option, not a
default); no direct OS calls outside `lv_plat_posix.c`
(`grep -nE '\b(mmap|munmap|open|read|write|socket|poll|clock_gettime)\s*\(' runtime/lv_runtime.c runtime/lv_loop.c`
returns nothing). Existing `ctest` suite untouched and green.

## 4. Milestone B-M2 — archive and link driver

*(Corrected at the 2026-07-05 maintenance pass — B-M1 already landed
`enable_language(C)` and the selftest target; `lv_loop.c` does not exist until B-M3.)*

1. CMake (Track-B region): `add_library(lvrt STATIC runtime/lv_runtime.c
   runtime/lv_plat_posix.c)` — **not** `lv_loop.c`, which is created at B-M3 and joins
   the archive then. C17, `-Wall -Wextra`, and link `m` (lvrt_arith float `%` uses
   `fmod`). Convert `runtime_selftest` from its ad hoc source list to link `lvrt` (same
   tests, now also proving the archive links).
2. Driver (`src/main.cpp` — no Track A ordering constraint, see companion doc §0.2):
   `lang --build-native <out> <file.ext>` = `LlvmGen::emitObject` to `<out>.o` → link
   `<out>.o` + `liblvrt.a` → `<out>` → remove the temp `.o` on success. The link step
   passes `-lm` (see item 1).
3. **Linker probe** (one function, reused): try `clang++`, then `g++`, then `cc` on
   PATH; clear diagnostic listing what was probed when none found. Use the same probe to
   fix the `popen("g++ …")` hardcode in `--build` (~`src/main.cpp:238`).
   (`ld.lld`-as-a-library is deliberately **not** v1 — see hurdle B-H5.)
4. Locating `liblvrt.a` at runtime: relative to the `lang` executable
   (`/proc/self/exe` on Linux for now — it's driver code, not runtime; portability of
   the *driver* rides Phase 2), overridable with `--runtime <path>`.

**Acceptance:** `build/lang --build-native /tmp/t tests/corpus/core/arith.ext && /tmp/t`
prints the expected output; `--build` works without g++ when clang++ is present (probe
order verified by temporarily renaming). **Do not touch Track A's stub-linked LLVM test
lanes** (`corpus_core_llvm`, `corpus_llvm_objects` — they link
`tests/support/llvm_stub_runtime.c` until Track A's A-M3 relink): the
`run_native_llvm.sh` flip to the real runtime is Track A's A-M3 step 0, per the relink
checklist in their doc §12; they may flip to `liblvrt.a` directly if B-M2 has merged, or
to `runtime/lv_runtime.c runtime/lv_plat_posix.c` (compiled as C17, plus `-lm`) if not.

**Caveat for `--build-native` testing at B-M2:** generated programs from the current
LlvmGen call `lvrt_obj_new(classId, nslots)`-style stub signatures and emit a 5-field
LvClassInfo (see doc-1 §12 relink checklist) — a `--build-native` binary linked against
the real archive is **not expected to run object programs correctly until Track A's
relink lands.** B-M2's acceptance program (`arith.ext`) is scalar-only for exactly this
reason; keep acceptance to scalar/string programs and note the dependency in the log.

## 5. Milestone B-M3 — full platform floor + event loop + net natives

The complete floor interface (implement POSIX; the Win32 column is B-M5's spec):

| `lv_plat_*` | POSIX | Win32 (B-M5) |
|---|---|---|
| `map/unmap` | `mmap(MAP_PRIVATE\|MAP_ANONYMOUS)`/`munmap` | `VirtualAlloc/VirtualFree` |
| `write/read` | `write`/`read` | `WriteFile`/`ReadFile` (console: `GetStdHandle`) |
| `open/close/stat_size/stat_mtime` | `open/close/fstat`, `stat` (size/mtime) | `CreateFileA/CloseHandle`, `GetFileAttributesExA` (size/mtime) *(stat_mtime added 2026-07-06 — §10)* |
| `now_ns` | `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` |
| `tcp_connect/listen/accept` | BSD sockets, `AF_INET`, nonblocking accept path | Winsock2 + `WSAStartup` in `lv_rt_init` |
| `send/recv` | `send/recv` (recv 0 = EOF → `None`) | `send/recv` |
| `set_nonblock` | `fcntl(O_NONBLOCK)` | `ioctlsocket(FIONBIO)` |
| `poll(fds,n,timeout_ms)` | `poll` | `WSAPoll` (see B-H8) |
| `exit` | `_exit` | `ExitProcess` |

Port the loop semantics from `src/RuntimeLoop.cpp` (registries, due-time ordering,
dispatch-until-idle) and the socket natives' behavior from the oracle; X64Gen's
`genTcp*`/`genRecv`/`genWatch*` carry the ownership rules. Then the **audit**: every OS
touch in the runtime goes through `lv_plat_*` (the B-M1 grep, now over all runtime
files, stays in CI as a script if convenient).

**Acceptance:** loop-dependent programs run under a hand-driven harness if Track A's
A-M5 hasn't landed (selftest registers a timer closure via a fake trampoline and drives
`lv_loop_run`); once A-M5 lands, `corpus_llvm_full` (their lane) exercising
`timers/sockets/http/async` is the real gate — those tests **must** join the
`RESOURCE_LOCK corpus_net_ports` group (`CMakeLists.txt:149-151`); the http/async corpus
binds fixed loopback ports 8092/8093 and unlocked lanes hang each other under
`ctest -j`. Timer tests are a known flake surface (bug.md #6) — prefer generous
tolerances over tight ones.

## 6. Milestone B-M4 — aarch64-linux cross

1. ~~Parameterize the target triple end-to-end~~ **DONE at the 2026-07-05 maintenance
   pass** (see §10): `lang --target <triple>` → `LlvmGen::emitObject(path, triple)`
   (defaulted arg; host path byte-identical when absent), full target-registry init on
   the cross path, `Triple::normalize`. Verified: `--native-obj --target
   aarch64-linux-gnu` emits a genuine AArch64 relocatable ELF on this machine.
   `--build-native --target` is deliberately GATED with a clear error until items 2–3
   land (linking a cross object against the host archive would be garbage). B-M4 now
   touches no Track A file.
2. ~~Per-triple runtime archives: a small script (`runtime/build-triple.sh`) compiling the
   runtime with `clang -target aarch64-linux-gnu --sysroot=…` (or a distro cross-gcc)
   into `runtime/<triple>/liblvrt.a`; the driver resolves the archive by triple,
   `--runtime` overrides.~~ **DONE (2026-07-05 B-M4 entry, §10).**
3. ~~Link probe grows a cross mode: prefer `clang++ -target <triple>` when a triple is
   set.~~ **DONE (2026-07-05 B-M4 entry) — `clang++ -target <triple>` → `<triple>-g++` →
   `<triple>-gcc`; the `--build-native --target` gate is lifted.**
4. ~~Test lane (only if `qemu-aarch64` + a sysroot are present — feature-detect in CMake,
   skip otherwise): core corpus under qemu-user.~~ **DONE (2026-07-06 entry, §10) — a real
   cross toolchain + qemu-user were installed (owner-authorized); the shipped lane
   (`tests/run_qemu_cross.sh` + CMake `corpus_core_aarch64_qemu`) now runs for real and is
   green.**

**Acceptance — MET (2026-07-06, §10).** An aarch64 hello-world + `arith.ext` binary both
run correctly under qemu-user; the `corpus_core_aarch64_qemu` ctest lane is green (5/5
core corpus programs). B-M4 and Gate G2 (aarch64) are CLOSED.

## 7. Milestone B-M5 — macOS and Windows (environment-gated)

Hardware/CI availability decides scheduling (roadmap P2); the design is written down now
so bring-up is mechanical:

- **macOS:** `lv_plat_posix.c` is already ~correct (mmap/poll/BSD sockets/clock_gettime
  all present). Known deltas: linker is ld64/lld (`clang++` driver handles it); arm64
  requires code signing — ad-hoc signing happens automatically in modern
  linkers, do not add a signing step until a failure proves otherwise; no fully-static
  binaries on macOS (libSystem is always dynamic — fine, zero-dep is dropped);
  `CLOCK_MONOTONIC` exists (10.12+).
- **Windows:** `lv_plat_win32.c` per the §5 table; MinGW-w64 triple
  (`x86_64-pc-windows-gnu`) **first** — it cross-compiles from Linux and avoids the MSVC
  environment; MSVC triple later if wanted. `WSAStartup` in `lv_rt_init`. CRLF/console:
  write raw bytes, no text-mode translation (match corpus byte-for-byte). Paths in the
  corpus use `/` — leave file natives forward-slash-tolerant (Win32 APIs accept `/`).

**As-built status (2026-07-05 B-M5 first pass, §10):**
- **macOS** — no new floor file needed: `runtime/build-triple.sh <darwin-triple>`
  already selects `lv_plat_posix.c` (the floor is a straight reuse) and the driver's
  existing cross probe already prefers `clang++ -target <triple>` (the ld64/lld driver).
  Object emission for `arm64-apple-macos` / `x86_64-apple-darwin` verified (genuine
  Mach-O). Remaining bring-up is toolchain/hardware only.
- **Windows** — `lv_plat_win32.c` **COMPILED AND RUNNING (2026-07-06 entry, §10)**, closing
  the prior PRE-LANDED/COMPILE-UNVERIFIED status now that a MinGW-w64 toolchain exists
  (owner-authorized install). Compiled clean under `-Wall -Wextra` with **zero source
  changes needed** — the only fix required was a toolchain-naming mismatch in
  `build-triple.sh` (mechanical, logged). Implements the full §5 column (VirtualAlloc /
  GetStdHandle+WriteFile / `_open`+`_O_BINARY` / QueryPerformanceCounter / Winsock2 /
  WSAPoll) with the same caller contracts as POSIX, `WSAPoll`'s invalid-socket report
  mapped to `LV_POLLIN|LV_POLLNVAL` (lv_plat.h's retire-on-wake contract), and the
  fd-bridge design (§10, prior pass) held up under real compilation. The two deferred
  driver wiring items are **DONE**: `-lws2_32` + `.exe` output land in `src/main.cpp`'s
  `--build-native` path, and a feature-detected wine test lane
  (`tests/run_wine_cross.sh` + CMake `corpus_win_wine`) runs core (5) + objects (3)
  corpus programs under wine, byte-matching `.expected` on stdout.

**Acceptance — Windows core+objects subset MET (2026-07-06, §10);** macOS unchanged
(object emission re-verified genuine Mach-O; still toolchain/hardware-gated, no Darwin
cross environment on this host and none installed). Full corpus (timers/sockets/http/
async) stays untested on Windows — deliberately out of this pass's scope (would need the
`RESOURCE_LOCK corpus_net_ports` group `corpus_win_wine` does not carry). Divergences
(path/newline/console encoding) go to the log with exact repro before any corpus program
is marked platform-skipped — none found in the core+objects subset.

## 8. CMake / CI integration summary

Track-B region additions over the milestones: `enable_language(C)`; `lvrt` static lib;
`runtime_selftest` (+ optional sanitizer variant); qemu lane (feature-detected);
sanitizer option `LANG_RT_SANITIZE`. Reminder: touch nothing inside the Track-A region
(their doc §10.3), and keep `corpus_elf*`/`corpus_native*` lanes green — they are the
frozen anchors this pivot is measured against.

## 9. Suspected hurdles (Track B)

- **B-H1 — rc semantics are subtle and extracted, not invented.** `-1` arena sentinel,
  `0` fresh/unowned (consumable by string ops!), `≥1` owned; frees are recursive;
  release order at slot overwrite is release-old-*then*-retain-new. Extract each rule
  from the named X64Gen helpers before coding; a wrong guess here shows up as corpus
  heisenbugs weeks later.
- **B-H2 — regions, not malloc.** The ARC range-check design **requires** allocations to
  come from registered regions; `malloc` would leave literals indistinguishable from
  heap strings. All memory via `lv_plat_map` regions.
- **B-H3 — ownership contracts on natives** (+1 transfers) are the historically
  bug-densest spot (info.md §15's fixed-bug list is your regression checklist). The §2.7
  table is mandatory and every entry gets a selftest.
- **B-H4 — print/format byte-parity.** Use raw `lv_plat_write` (no stdio buffering) so
  program output and `sysWrite` interleave exactly as the oracle's; float formatting
  matches `valueToString` (crib CGen's `kRuntime`).
- **B-H5 — LLD-as-a-library is a trap in v1.** Its C++ entry points are semi-internal
  and distro packaging of `liblld*` is inconsistent; the probe-external-linker design is
  deliberate. Revisit as a P2+ improvement; do not burn days on it now.
- **B-H6 — archive location.** `/proc/self/exe` is Linux-only driver code; keep it
  isolated in one function so the mac/win driver port is one function.
- **B-H7 — event-loop lifetime.** "Exit when no live work remains" — mirror
  `RuntimeLoop.cpp` exactly (what counts as live: active timers, watches, pending
  promises). Getting it wrong hangs CI (with the port-lock held, cascading).
- **B-H8 — WSAPoll famously fails to report `connect()` failure** on older Windows.
  Mitigation when B-M5 arrives: nonblocking connect + poll for writability + check
  `SO_ERROR`. Written here so it isn't rediscovered.
- **B-H9 — sanitizers vs. the custom allocator.** ASan can't see into region
  sub-allocations; it still catches OOB on the runtime's own structures and
  use-after-unmap. Run selftests under ASan/UBSan, but don't expect it to replace the
  churn harness — the live-bytes meter is the real leak net.
- **B-H10 — C17, not C++.** The runtime is C by decision (trivial cross-ABI, compilable
  by `clang -target` for any triple, and the closest shape to its future Leviathan
  rewrite in P3). No C++ features sneak in via "just one helper."

## 10. Implementation log (append-only)

*(Implementation agents: date-stamped entries — extracted layout/rc facts, the completed
§2.7 contract table, native-name reconciliation with Track A, selftest inventory,
platform notes, and any STOP events with findings.)*

### 2026-07-05 — B-M1 complete (runtime core + selftest). Sonnet-class agent.

**Deliverables landed** (`runtime/`): `lv_abi.h` (the §2 contract), `lv_plat.h` +
`lv_plat_posix.c` (floor: map/unmap, write/read, now_ns, exit — the B-M1 slice; the
rest of §5's interface is declared, unimplemented, for B-M3), `lv_runtime.c`
(allocator + ARC + strings + collections + objects/dispatch + exceptions),
`selftest.c`. CMake Track-B region added: `enable_language(C)`, `runtime_selftest`
executable + `add_test`, `LANG_RT_SANITIZE` option (default OFF).

**Acceptance met.** `runtime_selftest` green under **gcc 13.3** and under
`-fsanitize=address,undefined`. Full `ctest` suite still 27/27 green (the 26 pre-existing
lanes untouched + the new one). OS-call audit grep over `lv_runtime.c` returns nothing
(all OS access via `lv_plat_*`). Meters at exit: peak=1600B, live=992B — the residual
992B is the selftest's own deliberately-unreleased fresh (rc-0) strings, not a runtime
leak (see the strings-test note below); the ARC object-graph test does assert a full
return to baseline and passes.

**Clang gap (mechanical, logged not blocking):** no `clang` in this environment, only
gcc. The runtime is strict C17 with no gcc-isms (designated initializers, fixed-width
types, `__attribute__`-free), so clang parity is expected; must be re-confirmed when a
clang toolchain is present (it will be needed anyway for B-M4 cross builds).

**Extracted layout / rc facts** (from the frozen `src/X64Gen.{hpp,cpp}` — code is truth,
per H-2; recorded as `lv_abi.h` comments and encoded as behavior):
- **rc sentinels:** `-1` arena (never counted/freed via release), `0` fresh/unowned,
  `>=1` owned. `genRelease` gates `<= 0` as a **no-op** (`X64Gen.cpp:617`) — an unaliased
  rc-0 value is *only* reclaimed by a later consumer (string ops) or by arena bulk-free,
  never by `release`. This is the single most counter-intuitive fact and is now asserted
  in the strings selftest.
- **ARC prefix:** 16 bytes immediately before every counted payload — `[P-16]=rc`,
  `[P-8]=meta`. `meta` is *total raw bytes* for strings/objects/maps/closures/dense
  arrays, but **capacity (a slot count)** for boxed arrays (`genMkArr`,
  `X64Gen.cpp:905`) — recomputed to bytes as `24 + 16*capacity` on free.
- **object:** `[classId][dyn][slot*n]`, slot i at `16 + 16*i`; `dyn` heads a 32-byte
  raw-node fallback list (`[next][keyPtr][valTag][valPay]`), keyPtr **interned /
  pointer-compared**.
- **array dense marker:** bit 63 of the length word; dense records are inline value-struct
  object bodies, never refcounted; a tag-5 element points *into* the buffer.
- **map:** `[len][LvPair*len]`, LvPair=32B, insertion-order, string keys compared by
  content (`str_eq`), inserts on a missing key always copy (no spare capacity).
- **closure:** reuses the object mechanism verbatim — `[fnIndex][captureHead]` with the
  same dyn-node list; `CaptureVar` retains (node owns the value).
- **size classes:** 28 classes, `16 << c` (2^4..2^31), free-list-per-class, rounding cap
  at class 27 for garbage sizes.

**Deviations from X64Gen (deliberate, all correctness-preserving):**
1. **String descriptors carry a trailing NUL** (X64Gen's do not) — for C-string interop;
   the normative §2.4 shape already specified it. Length-prefixed reads are unaffected.
2. **Dense-array `meta` stores true total bytes**, not `recBytes` as X64Gen does
   (X64Gen's genArrAppend overwrites its size slot with recBytes, a stale-accounting
   quirk that hands `hfree` the wrong size). Kept accurate here to avoid a real, if
   corpus-invisible, live-bytes skew.
3. **`lv_is_counted` region-checks *before* dereferencing** any payload (X64Gen only
   range-checks strings; objects/arrays it trusts). Required because this runtime hardens
   retain/release against out-of-region pointers per doc §2.5 — see the ASan finding.
4. **Method dispatch is table-driven (`strcmp` over `LvClassInfo.methodNames`)**, not the
   baked per-program switch X64Gen emits. There is no ground-truth mechanism to port — a
   shared program-agnostic runtime *must* look names up at runtime. Class/method tables
   arrive from generated code via `lvrt_register` (§2.6).
5. **Allocation tier passed explicitly** (`lvrt_halloc(size, tier)`) rather than through
   X64Gen's mutable `g_use_arena` global — matches the pointer-explicit spirit of the ABI.

**Two bugs caught during bring-up (both real, both fixed):**
- **ASan stack-buffer-overflow:** `lv_is_counted` read a tag-5 payload's classId before
  the region check, so a non-runtime pointer (stack address / bare literal) was
  dereferenced. Fixed by moving the region check ahead of every dereference. This is
  exactly the failure mode H-2/§2.5 warn about; the selftest now feeds retain/release a
  stack pointer and a static literal on purpose.
- **UBSan signed-shift UB:** `1LL << 63` for the dense marker overflows `int64`. Replaced
  with an unsigned `LV_DENSE_BIT_U` reinterpreted to signed.

**Contract addition (found writing the selftest, logged per §0.2 — a *completion* of the
contract, not a change to an existing shape):** field/capture keys are pointer-compared
(§2.4), so a `"message"` literal interned in a different translation unit than
`lvrt_raise` silently misses the RuntimeException message field. Added
`extern const char* const lv_key_message` as the one canonical interned pointer any code
reading a raised message must use. Track A's codegen for `catch` must intern the
exception's message key to this symbol (or the runtime hands catch blocks a void message).
Also added three closure constructors (`lvrt_closure_new/capture_set/capture_get`) the
§2.6 helper list omitted — closures reuse the object mechanism but need LV_CLO tagging
that `lvrt_obj_new` can't give. Both are marked non-normative-but-documented in `lv_abi.h`.

**Native-name enumeration (§2.7 seed table filled in `lv_abi.h`).** From
`grep '"' src/RuntimeNatives.cpp` (free/system natives) + `X64Gen::genCallNative`
(instance-method natives — its own comment "mirroring RuntimeNatives" confirms the same
logical set reached via a different IR op). Full list to reconcile with Track A's A-M2
step-0 enumeration before B-M3 wires them: `length isEmpty toString indexOf contains
subStr toInt trim charAt toUpper toLower startsWith endsWith` (string); `length at add`
(array); `length at has keys values` (map); `toString` (int/bool); `sysWrite sysReadLine
sysRead sysOpen sysClose sysStat sysTcpConnect sysTcpListen sysAccept sysSend sysRecv
sysWatch sysUnwatch sysTimerStart sysTimerCancel` (system). The B-M1 runtime implements
the string/array/map/int/bool natives' *cores* (concat/substr/idxget/etc.); the `sys*`
row is declared in the floor and wired at B-M3.

**Selftest inventory** (`runtime/selftest.c`, registers a fake 5-class table + a fake
dispatch trampoline): allocator (arena bump + save/restore bulk-free; heap size-class
reuse; retain/release no-op on stack + literal pointers); ARC object graph (array-of-obj,
map-keyed-by-string, closure capture — release the roots, assert live-bytes back to
baseline); strings (concat consume-unowned + free-list recycle, eq, substr, indexof,
trim, case, int↔str, zero); arrays/maps (boxed COW in-place at rc==1 vs copy-when-shared,
geometric append growth, dense value-struct append + read, map insert-copies +
update-in-place); dispatch (issub over a subtype-id closure); exceptions (raise_oob
message byte-match, uncaught report, catch_bind clears + transfers).

**Known limitations (built when needed, not speculatively):** one fixed mmap region per
tier (not a growing region *list* — matches X64Gen's single-mmap model; multi-region is a
mechanical extension); the dense-append path copies-and-grows without freeing the old
buffer, faithfully replicating X64Gen's genArrAppend and its declared `dense_index_set.ext`
XFAIL (parity means matching warts). Event loop / net natives / driver are B-M2/B-M3, not
started. No STOP events — the contract held; the two additions above are completions of
gaps, made and logged per the mechanical-adaptation rule, not architectural changes.

### 2026-07-05 — Cross-track maintenance pass (Fable). ABI reconciliation, B-M2 unblocked.

Audited Track A's A-M2 work (`tests/support/llvm_stub_runtime.c` + LlvmGen's emitted
`lvrt_*` set) against the canonical contract; both tracks were self-consistent but had
drifted from each other. **Rulings** (all encoded in `lv_abi.h`, implemented in
`lv_runtime.c`, selftested; the doc body above is updated to match — §2.6 struct, §4
B-M2 corrections):

1. **Scalar core / stringify / IO entry points ratified** into the contract:
   `lvrt_truth/not/neg/arith/to_string/print_val/print_nl/syswrite/readline`. They
   predate B-M1 as the old NativeRuntime/stub contract and were a B-M1 enumeration gap
   (LlvmGen already emits all of them). `lvrt_to_string` is the ts_build analogue at
   `valueToString` byte-parity. String `==`/`!=` across mixed types is **false** per
   the oracle — deliberately not the A-M2 stub's stringify-both-sides (which made
   `"1" == 1` true).
2. **`LvClassInfo` grows `methodKinds`** (`LV_M_METHOD/GET/SET/OP`): one shared table
   must reproduce X64Gen's four separate dispatch switches; without the kind column a
   field read could dispatch a plain method. **Emission stride warning** added: the
   constant table must be an array of the full 11-field struct, never a prefix (Track
   A's 5-field A-M2 emission reads garbage past entry 0 against the real runtime).
3. **Key comparison**: pointer-first + content fallback (fieldindex, dyn list,
   captures). A missed codegen dedup degrades to a slow lookup, never a silent field
   miss. `lv_key_message` demoted to optional fast path.
4. **Dispatch ownership**: `getm/opm/callm` results are +1 (transfer) — generated code
   must not wrap-retain them (would go +2 the moment getter dispatch exists); `setm`
   does the value-slot ARC internally (release-old-then-retain-new, B-H1 order) on the
   field path, none when dispatching a declared setter.
5. **Lazy allocator self-init**: until A-M5 flips entry to the runtime-owned `main()`,
   LlvmGen's own `main()` never calls `lv_rt_init`; the allocator now self-initializes
   on first use (one predictable branch).
6. **Track A's stub is retired at their A-M3 relink** (checklist in their doc §12) —
   it was declared temporary in its own header; B-M2 must not touch the stub-linked
   lanes meanwhile.

Verification: selftest green (gcc + ASan/UBSan) including new coverage for every ruling;
full `ctest` 27/27 on this branch and **28/28 on a local integration merge with
`track-a-llvm-parity`** (their stub-linked lanes coexist with the reconciled runtime).

**B-M2 is GO** for a Sonnet-class agent per the corrected §4. Inherited state: B-M1 +
this reconciliation on `track-b-runtime`; `enable_language(C)` + selftest target already
in CMake; `main.cpp` is Track B's exclusively (Track A's rescope — their doc §0.2).
Remember the §4 caveat: `--build-native` acceptance stays scalar-only until Track A's
relink lands.

### 2026-07-05 — B-M2 complete (archive, link driver, probe). Sonnet-class agent.

**Deliverables landed:**
- CMake (Track-B region): `lvrt` STATIC library (`runtime/lv_runtime.c
  runtime/lv_plat_posix.c`), C17, `-Wall -Wextra`, `target_link_libraries(lvrt PUBLIC m)`.
  `runtime_selftest` converted from its ad hoc 3-source build to `add_executable(
  runtime_selftest runtime/selftest.c)` + `target_link_libraries(... PRIVATE lvrt)` — same
  tests, now proving the archive links. `LANG_RT_SANITIZE` gates `-fsanitize=address,
  undefined` on **both** `lvrt` and `runtime_selftest` (it has to cover `lvrt` now that
  the instrumented code lives there, not inline in the test binary).
- `src/main.cpp`: `--build-native <out> <file.ext>` (new `BuildNative` mode) —
  `LlvmGen::emitObject` to `<out>.o`, link against `liblvrt.a` + `-lm`, remove the temp
  `.o` on success (also on link failure, so a failed build doesn't litter `<out>.o`).
  `--runtime <path>` overrides the archive location.
- `probeLinkerDriver()` (one function, reused): tries `clang++`, `g++`, `cc` via a
  PATH scan (`stat`+`access(X_OK)`, no subprocess spawn per candidate); returns the
  first hit or empty with the full tried-list for the diagnostic. Replaces the
  `popen("g++ -O2 -x c++ ...")` hardcode in `--build` (same function, same order).
- `findRuntimeArchive()`: `/proc/self/exe` → dirname → `liblvrt.a`; Linux-only, isolated
  in its own function per the file-map note (ported at B-M5).

**Acceptance verified:**
- `--build` still produces a working binary (arith.ext output byte-matches) and now
  goes through the probe instead of the g++ hardcode.
- **Probe order verified by PATH manipulation** (no clang++ on this machine, so this is
  the only way to exercise all three branches): (1) normal PATH → picks `g++` silently;
  (2) scratch dir with fake `g++`+`cc` wrapper scripts on an *isolated* PATH → `g++`
  wins (confirmed via a marker the fake script prints); (3) scratch dir with only a fake
  `cc` wrapper on a fully isolated PATH (no `/bin`, which is `usr-merged` and would
  otherwise leak a real `g++` back onto PATH — tripped me up on the first attempt,
  noting it here so the next person doesn't repeat it) → falls back to `cc` (confirmed
  via its marker; it then fails to find `as`, an artifact of the deliberately-broken
  PATH, not a probe defect); (4) empty PATH → clean diagnostic naming all three tried.
- `runtime_selftest` green under plain build and under `-DLANG_RT_SANITIZE=ON`
  (rebuilt both ways, ran both, flipped the cache back to OFF afterward per the
  documented default).
- Full `ctest`: **28/28**, unchanged.
- `--build-native` link mechanics verified directly: produces a valid PIE ELF, `nm -u`
  shows zero unresolved symbols against `liblvrt.a`, the temp `.o` is removed on both
  success and link failure (checked `--runtime` pointed at a nonexistent path: `ld`
  reports the missing file, driver reports `link failed`, temp `.o` still gets cleaned
  up), `--runtime <path>` override takes precedence over the `/proc/self/exe` lookup.

**Finding — the §4 caveat undersells the current blast radius (logged, not a STOP; see
below for why).** Doc-2 §4 frames the `--build-native` runtime gap as affecting
*"object programs"* and picks `tests/corpus/core/arith.ext` as a *scalar-only* example
expected to run correctly today. It does not. Root-caused with `--emit-llvm` + `gdb`:

- `declare ptr @lvrt_obj_new(i64, i64)` in every LLVM module Track A currently emits —
  the **stub-era two-i64 signature** (`classId, nslots`, returning the pointer), not the
  ABI's `void lvrt_obj_new(LvValue* out, int64_t classId)` (out-param first, per the
  boundary rule, §2.1/H-1). Linked against the real `liblvrt.a`, the caller's `classId`
  (e.g. `1`) lands in the callee's `out` register — `gdb` shows exactly this:
  `lvrt_obj_new (out=0x1, classId=1)`, a `SIGSEGV` on the first `out->tag = LV_OBJ`
  store in `lv_runtime.c`.
- This is **not gated on user code touching objects**. Every program's compiler-emitted
  `main()` unconditionally calls a `ginit`-like function (`f123`/`f124` in one build)
  that constructs a fixed set of prelude singletons (the modeled `Console` class among
  them — see `src/Resolver.cpp:670`, `src/Eval.cpp:629`) via `lvrt_obj_new`, before the
  user's own entry point ever runs. Verified with the minimal possible program,
  `int x = 5;` (no `console`, no classes, no output) — it emits the *same* 5
  `lvrt_obj_new` calls at ginit and crashes identically. So the affected set isn't
  "programs with objects," it's **every program**, unconditionally, until Track A's
  A-M3 relink retypes these call sites to the real ABI signature.
- **Why this is logged, not escalated as a STOP:** the ABI itself is not in question —
  `lv_abi.h`'s `lvrt_obj_new(LvValue*, int64_t)` is correct and already what
  `lv_runtime.c` implements and what the B-M1 selftest exercises. The break is entirely
  a not-yet-relinked call site in `src/LlvmGen.cpp`, a file I'm frozen out of (doc §0.3)
  and which Track A is already scheduled to fix at A-M3 (doc-2 §4's own trap note names
  this exact relink; doc-1 §12 logs "A-M3 opens with the relink"). Nothing here requires
  an architectural choice — it requires Track A's already-planned work landing. Per
  B-M1's own precedent (the clang-gap entry: "logged not blocking"), this is the same
  shape of finding.
- **What I verified instead**, to still ground "B-M2's own work is correct" in
  evidence rather than assertion: the link driver produces a valid, fully-resolved
  binary (see Acceptance above) and the crash reproduces at *exactly* the call site the
  IR dump predicts, with the exact argument corruption the signature mismatch predicts
  — i.e., the failure is Track A's, not a Track B regression.
- **Action for whoever picks up A-M3 / next verifies B-M2 end-to-end:** once the relink
  retypes `lvrt_obj_new` (and the rest of the stub-era `lvrt_*` call sites) to the real
  ABI, re-run `build/lang --build-native /tmp/t tests/corpus/core/arith.ext && /tmp/t`
  and diff against `arith.expected` — this was not possible to demonstrate today for
  *any* program, not just object-bearing ones, so it's still an open acceptance item,
  now correctly scoped.
- **RESOLVED same-day, at the B-M3 merge point.** Track A's A-M3 relink (their log
  entry, this doc's companion, 2026-07-05: "runtime-v2 relink + A-M3 implemented")
  landed on local `master` before this branch's B-M3 merge. Re-ran the exact command
  above post-merge: `arith.ext` now byte-matches `arith.expected` under
  `--build-native`. Went further and swept `tests/corpus/core/*.ext` +
  `tests/corpus/llvm_objects/*.ext` (8 programs, including object-bearing ones and a
  stdin-driven native-IO program) through `--build-native` — all 8 byte-match their
  `.expected` (one apparent mismatch on `echo.ext` was a sweep-script artifact, forgot
  to pipe its `.stdin`; re-ran with stdin piped and it matches too). B-M2's acceptance
  bullet is now fully satisfiable, not just the driver-mechanics-only proof done at the
  time.

**No STOP event.** The ABI held; every Track-B-owned deliverable (archive, driver,
probe, archive-location, sanitizer wiring) is implemented, tested, and green. The one
open item is a pre-existing, already-owned, already-scheduled Track A dependency, now
documented precisely instead of by the narrower guess in the original §4 text.

### 2026-07-05 — B-M3 complete to the hand-driven-harness acceptance ceiling (platform
floor, event loop, net natives). Sonnet-class agent.

**Deliverables landed:**
- `lv_plat_posix.c`: the full §5 POSIX slice — `open/close/stat_size` (portable
  1=read/2=write/4=append bits translated to `O_*` *inside* this file, never leaked
  into `lv_runtime.c`), `tcp_connect/tcp_listen/accept` (blocking connect then
  nonblocking, matching `src/RuntimeNatives.cpp`'s sysTcpConnect exactly; listen binds
  127.0.0.1 backlog 16), `send/recv`, `set_nonblock`, and `poll` — the last taking a new
  platform-opaque `LvPollFd{fd,events,revents}` (added to `lv_plat.h`, **not**
  `lv_abi.h` — this is Track B's own internal floor interface, not the generated-code
  contract, so refining a B-M1 placeholder `void*` into a concrete type here is not a
  §2 contract change) so a future Win32 floor can back it with `WSAPOLLFD` (whose
  `SOCKET` fd type is a different width than POSIX `int` — B-H8) without touching any
  caller.
- `lv_abi.h` §2.7 **B-M3 wiring** (the seed table was a table, not code, until now):
  `lvrt_sysopen/sysclose/sysstat/sysread` (file I/O, implemented in `lv_runtime.c`) and
  `lvrt_systcpconnect/systcplisten/sysaccept/syssend/sysrecv/systimerstart/
  systimercancel/syswatch/sysunwatch` plus the non-native loop-control pair
  `lvrt_loop_has_work/lvrt_loop_step` (implemented in the new `lv_loop.c`). All args
  cross as `LvValue*` per the boundary rule, matching the already-ratified
  `lvrt_syswrite` style even for scalars (fd/port/id) — see the header comment for the
  full ownership table. This is filling an explicitly pre-flagged gap ("this table is a
  SEED, not yet wired to code," §2.7), the same category as B-M1's closure-constructor
  addition — logged, not a STOP.
- `runtime/lv_loop.c` (new; joins the `lvrt` CMake target alongside `lv_runtime.c` and
  `lv_plat_posix.c`, per the B-M2-corrected §4 note that it "joins the archive" at this
  milestone): growable timer and fd-watch registries (no X64Gen-style fixed
  `kMaxTimers`/`kMaxWatch` cap — a real C runtime doesn't need one, and a silent cap is
  itself a bug class, not a feature). Ownership: registry retains on add, releases on
  cancel or one-shot completion (`§2.7`'s "registry OWNS the callback" ruling). Closure
  invocation goes through one runtime-internal bridge, `lv_rt_dispatch_fn()` (new,
  non-static accessor in `lv_runtime.c`, declared `extern` locally in `lv_loop.c` — not
  in `lv_abi.h`, generated code never calls it) exposing `lv_runtime.c`'s private
  `g_dispatch`; the closure is passed as the callee's own first argument (extracted from
  `X64Gen::genCallClosure`'s forwarding order, `src/X64Gen.cpp:1978-1998` — the callee's
  body reads its own captures via `lvrt_capture_get(self, ...)`, so `self` must be an
  argument, not implicit).
- `runtime_selftest` gains `test_event_loop` (one-shot timer, repeating timer +
  mid-flight cancel, fd watch via a real `pipe()`, all ARC-checked back to a `live_bytes`
  baseline), `test_sys_file_natives` (open/write/close/stat/open/read round-trip + a
  missing-path case), and `test_sockets` (a full loopback tcp connect/listen/accept/
  send/recv handshake, including the EOF→`LV_NONE` vs would-block→`""` narrowing).
- New CI check: `tests/run_rt_platform_audit.sh` + `add_test(rt_platform_audit ...)` —
  doc-2 §5's "the B-M1 grep, now over all runtime files, stays in CI as a script if
  convenient." Greps `lv_runtime.c`/`lv_loop.c` for direct OS calls or OS-specific
  header includes; clean.

**Two real bugs caught by the new selftest coverage (both in *my own* B-M3 code, not
inherited from anywhere — logged per the same discipline as B-M1's ASan/UBSan catches):**
1. **Use-after-free on one-shot timer fire.** My first cut released the registry's
   retained closure ref *before* invoking it ("deactivate and release, then fire" felt
   natural to write in that order). `lvrt_release` on the registry's rc-1 ref is a REAL
   free (not the rc-0 no-op case), and the allocator poisons freed blocks
   (`memset(...,0xFE,...)`), so by the time the callback ran, its `fnIndex` read back
   garbage — the callback silently no-op'd instead of firing. Fixed by invoking first,
   releasing after. `test_event_loop`'s exact tick-count/tick-value assertions caught
   this immediately (a looser "did it crash?" check would have missed it — it didn't
   crash, it silently did nothing).
2. **Truncating ns→ms conversion could stall a repeating timer indefinitely under a
   tight polling caller.** `(earliestDue - now) / 1000000` truncates toward zero, so any
   due time under 1ms away computed `timeoutMs=0`; the no-watches branch reads that as
   "already due" and skips sleeping, so `step()` returns having done nothing — and a
   caller with no per-call syscall overhead to accidentally burn the remainder (my
   selftest's raw C loop, unlike Track A's interpreter-driven `await`) never converges
   within any bounded iteration count. Fixed with ceiling division
   (`(remainNs + 999999) / 1000000`) so any nonzero remainder rounds up to at least 1ms.
   Note for whoever wires Track A's `Await` codegen to `lvrt_loop_step`: this was latent
   in a faithful port of `src/RuntimeLoop.cpp`'s own `duration_cast<milliseconds>`
   (which has the identical truncation), just never surfaced there because the oracle's
   interpreter loop has enough per-iteration overhead to accidentally cross sub-ms gaps.

**Acceptance status — hand-driven-harness ceiling reached, exactly as doc-2 §5
anticipated:**
- `runtime_selftest` green plain and under `-DLANG_RT_SANITIZE=ON` (ASan/UBSan clean
  through the new registries, the VLA-based `lv_plat_poll`/`lvrt_sysread`/`lvrt_sysrecv`
  buffers, and the `lv_invoke1` pointer-cast dispatch path).
- Full `ctest`: **29/29** (28 prior + the new `rt_platform_audit`).
- `lvrt_loop_has_work`/`lvrt_loop_step` are exactly what Await's codegen will pump
  (mirrors `X64Gen::genAwait`'s inline `has_work()`/`loop_step()` calls,
  `src/X64Gen.cpp:3573-3593`) — there's no separate "`lv_loop_run`" driver function by
  design; `await`'s own generated loop *is* the driver, matching the reference exactly
  rather than adding an extra layer the reference doesn't have.
- **The "real" gate — `corpus_llvm_full` exercising timers/sockets/http/async through
  actual LLVM-emitted code — remains unreachable**, for the same underlying reason
  logged at B-M2: Track A hasn't emitted calls to *any* of these B-M3 natives yet (no
  `Await`/event-loop codegen exists in `LlvmGen.cpp` today), and even the simpler
  scalar/object path is still blocked on their A-M3 relink (the `lvrt_obj_new`
  signature mismatch logged in the B-M2 entry above) — a program compiled today
  wouldn't reach a `sysTimerStart` call correctly even if one were emitted. This is not
  a new finding, just B-M2's already-logged Track A dependency extending to cover B-M3's
  acceptance too, exactly as the milestone table predicts (B-M3 "must land before
  Track A's A-M5").

**No STOP event.** Every Track-B-owned deliverable for B-M3 is implemented, ARC- and
sanitizer-verified, and green; the platform-floor audit holds. The stopping point here
is the same pre-existing, already-owned Track A dependency as B-M2 (their A-M3 relink,
then A-M5's event-loop codegen), not a new architectural gap — B-M4 (aarch64 cross) is
untouched pending that, and B-M4 item 1 is explicitly gated on a one-line change to
Track A's own frozen file (`src/LlvmGen.cpp:289`'s hardcoded triple) per the doc's own
instruction to "request it via the logs" rather than editing mid-flight, so it is the
next natural stop regardless.

### 2026-07-05 — Maintenance pass #2 (Fable): two runtime-facing bugs fixed, B-M4 unblocked.

All agents stopped; owner-directed maintenance. Both fixes verified across every
engine; full `ctest` **31/31**, lcurl fixture suite **48/48** (was 47/48), selftest
green plain + ASan/UBSan.

**Bug 1 — stale `TcpStream` handles wrote into recycled descriptors.** Found via the
lcurl suite's one red case (`--max-time cuts off /slow`, `--ir` lane): the fixture
server's leftover per-connection 3s timer wrote its payload into the NEXT client's
connection, so that client's transfer completed instantly and its `--max-time` never
tripped. The engine label was incidental — pure timing of which client owned the
recycled fd number when the stale timer fired (reproduced deterministically with two
sequential /slow requests ~1s apart, either engine). Root cause: the prelude's
`TcpStream.close()` left `fd` set and `<<`/`send()` unguarded, so any retained handle
kept raw access to an fd NUMBER the OS had already handed to someone else (POSIX
recycles the lowest free descriptor). Fix (prelude, `src/Resolver.cpp`): `close()` is
idempotent and invalidates (`fd = -1`); `<<`/`send()`/`pump()` no-op on a dead handle;
`TcpListener.stop()` got the same discipline. `File.close()` already did this —
`TcpStream` was the outlier. Regression: `tests/corpus/stale_stream.ext` (+`.expected`)
forces the recycle-then-stale-write shape with a 5s self-draining guard timer; runs on
all five engines (treewalk/ir/ir-verify/mem-verify/native/elf lanes — 29 corpus files
now, elf 0 skipped). Verified red pre-fix, green post-fix.

**Bug 2 — a watch on a closed fd busy-spun the event loop forever (POLLNVAL ignored).**
Exposed while building Bug 1's regression: close an fd UNDER an armed watch and
`poll()` returns instantly with `POLLNVAL`, which neither `RuntimeLoop.cpp`'s readiness
mask (`POLLIN|POLLHUP|POLLERR`) nor runtime v2's `lv_plat_poll` mapping consumed —
`nextBatch()`/`lvrt_loop_step()` returned empty forever at 100% CPU. **Ruling:**
`POLLNVAL` wakes the watch once (the callback observes the dead fd via its recv), then
the registry auto-retires it — a dead descriptor can never become readable, so keeping
the watch is meaningless by definition. Implemented in BOTH loops: `RuntimeLoop.cpp`
(batch + auto-`cancelWatch`) and `lv_loop.c` (fire-then-cancel, release-after-invoke per
the B-M3 UAF lesson) with `LV_POLLNVAL` added to `lv_plat.h`'s poll surface (Track B's
internal floor header, NOT the §2 ABI — no contract change). The B-M5 Win32 floor must
map WSAPoll's invalid-socket report to `LV_POLLNVAL` (noted in the header). Regressions:
a direct `RuntimeLoop` unit in `tests/test_eval.cpp` (verified red pre-fix: hasWork
stuck, 0 fires) and a dead-fd-retire case in `runtime/selftest.c` (wake exactly once,
registry ref released, live-bytes back to baseline). **Frozen-backend note:** X64Gen's
loop mask (`0x19`, `src/X64Gen.cpp:2430`) has the identical latent gap and stays frozen
— do NOT add corpus programs that leave a watch armed on a closed fd (the fixed prelude
no longer produces that shape; `stale_stream.ext` unwatches before closing).

**B-M4 item 1 landed** (cross-track dependency cleared while no agents are mid-flight —
the §6 note's own alternative to "request via the logs"): `LlvmGen::emitObject` grew a
defaulted `triple` argument (host emission byte-identical), full target-registry init on
the cross path, `--target <triple>` in the driver, and a hard gate on
`--build-native --target` until B-M4's per-triple archives + cross-probe land. Verified:
aarch64 relocatable ELF emitted on this machine; 31/31 suite unchanged. §6 item 1 struck
through accordingly. Note for B-M4: this machine has NO clang and NO cross-gcc — items
2–4 must feature-detect (`clang`/`aarch64-linux-gnu-gcc`/`qemu-aarch64`) and skip
gracefully; emission-level verification (`file` on the .o) is always available.

**Owner language feedback** (from `language-notes.md`, examples/curl usage): stdlib
needs filling out; `console.readln`-style sugar over `sysReadLine`; a design question on
namespace-scoped entry (`main()` at the bottom of a namespace in the entry file);
`string?` reading as "None-able" (it is — nullable-style syntax, None semantics). These
are owner-level language-design items, NOT scoped to either pivot track; parked here so
they are not lost.

### 2026-07-05 — B-M4 complete to the no-cross-toolchain acceptance ceiling (per-triple archive, cross link, qemu lane) + §2.9 entry pre-land. Fable-class agent.

Branch at/past `9f26cd7` (branch == master after maintenance pass #2, no merge). Baseline
re-verified **31/31** before any edit. All Track-B-owned; no Track A file touched
(`src/LlvmGen.*`, Track-A CMake region, `src/X64*` untouched — the triple parameter
`emitObject(path, triple)` A landed at the maintenance pass is the only cross-track
surface used, and only as a caller).

**Deliverables landed:**

- **`runtime/lv_entry.c` (NEW archive member) — the doc-2 §2.9 runtime-owned `main()`,
  pre-landed for the A-M5 interlock.** `lv_rt_init(argc,argv)` → `extern void
  lv_main(LvValue*)` → drain with `lvrt_loop_has_work()`/`lvrt_loop_step()` →
  `lv_rt_exit_code()`, exactly the §2.9 sketch. Joined the `lvrt` archive (CMake) and
  the platform-floor audit's file list (`run_rt_platform_audit.sh`) — it routes solely
  through `lv_rt_*`/`lvrt_*`, touches no OS primitive, audit stays clean. **Safe to ship
  today; both non-collision properties proven, not asserted:**
  1. *selftest's own `main()` beats the member* — `runtime_selftest` (which defines
     `main`) links and passes with `lv_entry.c.o` present in `liblvrt.a`; the member is
     never pulled (a static-archive member resolves only *undefined* symbols, and `main`
     is already strong from `selftest.o`).
  2. *a generated object's own `main()` beats the member* — `--build-native
     tests/corpus/core/arith.ext` still links and runs byte-identical to `arith.expected`;
     `nm` on the binary shows **no `lv_main`** (member unpulled) and **zero** unresolved
     symbols. The member's undefined `lv_main` therefore cannot leak into any link until
     A-M5 flips LlvmGen to emit `lv_main` in place of `main`, at which point the generated
     object's *undefined* `main` pulls this member and its own `lv_main` resolves the call
     — no driver/CMake change needed then. Confirmed the same property on the **cross**
     path too (the per-triple archive also carries `lv_entry.o`, yet the cross arith
     binary linked and ran with the generated `main` winning).

- **`runtime/build-triple.sh` (item 2)** — cross-compiles the four C runtime sources
  (`lv_runtime.c`, `lv_plat_posix.c`, `lv_loop.c`, `lv_entry.c`) into
  `runtime/<triple>/liblvrt.a`. Auto-detects the cross **C** compiler in the driver's own
  probe order — `clang -target <triple>` (with `LVRT_SYSROOT` → `--sysroot`), then
  `<triple>-gcc`; env overrides `LVRT_CROSS_CC`/`LVRT_SYSROOT`/`LVRT_AR`/`LVRT_OUT_DIR`.
  Archiver prefers `<triple>-ar`/`llvm-ar`, falls back to host `ar` (the ar container +
  its symbol index are architecture-neutral). **C, never C++** (B-H10). One real bug
  caught by the mock and fixed before land: the first cut used strict `-std=c17`, under
  which glibc gates `clock_gettime`/`CLOCK_MONOTONIC` behind `_DEFAULT_SOURCE` and the
  floor failed to compile — the main CMake build uses `-std=gnu17` (CMAKE_C_STANDARD 17 +
  default C_EXTENSIONS ON), so build-triple.sh must too, or it would build a *different*
  runtime than CI tests. Now `-std=gnu17`, verified against the main build's actual flag.

- **Driver, per-triple archive resolution (item 2)** — `findRuntimeArchiveForTriple()`
  (refactored `findRuntimeArchive` to share `exeDir()`): resolves
  `<exedir>/<triple>/liblvrt.a` (installed/colocated) then
  `<exedir>/../runtime/<triple>/liblvrt.a` (in-source dev build, matching
  build-triple.sh's default output reached from `build/`). `--runtime <path>` overrides
  entirely. Missing-archive diagnostic names both search paths and points at
  build-triple.sh.

- **Driver, cross link probe + gate lift (item 3)** — `probeCrossLinkerDriver()`:
  `clang++ -target <triple>` → `<triple>-g++` → `<triple>-gcc`, returning the full driver
  invocation prefix. The deliberate `--build-native --target` error gate is **removed**
  (the milestone's exit criterion); the cross path now emits the object for the triple,
  resolves the per-triple archive, and links via the cross probe. Host `--build-native`
  path unchanged (still `probeLinkerDriver` = `clang++`/`g++`/`cc`).

- **qemu-user test lane (item 4)** — `tests/run_qemu_cross.sh` (builds the per-triple
  archive via build-triple.sh, cross-builds each core-corpus program with
  `--build-native --target`, runs under qemu-user, diffs `.expected`) + a CMake test
  `corpus_core_aarch64_qemu` in the **Track-B region**, `find_program`-gated on
  `qemu-aarch64` **and** a cross C compiler (`clang`/`aarch64-linux-gnu-gcc`). Absent
  either — as on this host — the lane is silently skipped (`message(STATUS ... SKIPPED)`),
  suite stays 31/31.

**Verification (this machine has NO clang, NO cross-gcc, NO qemu — confirmed; all cross
mechanics proven by mocks, per the milestone's environment gating):**
- *Emission* — `--native-obj --target aarch64-linux-gnu arith.ext` → `file`: *ELF 64-bit
  LSB relocatable, ARM aarch64*. (Already-proven A-side path, re-confirmed.)
- *Gate lifted* — `--build-native --target aarch64-linux-gnu` no longer errors at a gate;
  it now reaches per-triple resolution (errors there only because no archive/toolchain
  exists).
- *Per-triple resolution* — dropped a **fake-triple archive** (copy of the host
  `liblvrt.a`) at each layout in turn; the driver resolved *colocated*, *in-source*, and
  *`--runtime` override*, advancing past archive resolution to the cross probe in all
  three; negative case (no archive) gives the correct diagnostic.
- *Cross probe precedence* — **fake `clang++`/`<triple>-g++`/`<triple>-gcc` wrapper
  scripts on a fully isolated PATH** (scratch dir only — NOT including `/bin`, which is
  usr-merged and leaks real `/usr/bin`, per the B-M2 log's note): clang++ wins when
  present; falls back to `<triple>-g++`, then `<triple>-gcc`; each selected driver was
  invoked with the correct `[-target] <obj> <per-triple-archive> -lm -o <out>` line.
- *Full end-to-end cross pipeline, host triple as aarch64 stand-in* —
  build-triple.sh (mock cross CC=gcc) → `runtime/<host>/liblvrt.a` (4 members incl.
  `lv_entry.o`) → `--build-native --target <host>` resolves it → cross probe finds a
  `<host>-g++` wrapper forwarding to real g++ → links → **binary runs, byte-identical to
  `arith.expected`.** Every stage of the cross path exercised except foreign-arch
  execution.
- Full `ctest` **31/31** (qemu lane skipped, not counted). `runtime_selftest` green plain
  **and** under `-DLANG_RT_SANITIZE=ON` (ASan/UBSan clean; cache flipped back to **OFF**
  and binaries restored per the documented default).

**Acceptance shortfall (honest, expected, not a failure):** the "**an aarch64 binary runs
under qemu-user**" bullet of §6 **remains open** — it requires a real aarch64 cross
toolchain and qemu-user, neither present on this host. Everything up to and including a
correctly-linked cross binary is proven by mocks (emission, per-triple archive build +
resolution, cross-probe selection, link-command formation, and a full host-as-cross
end-to-end run). When a cross env appears, the shipped `corpus_core_aarch64_qemu` lane
auto-enables and closes this bullet with no code change.

**No STOP event.** Every B-M4 Track-B deliverable (per-triple archive script, driver
resolution, cross probe, gate lift, feature-detected qemu lane) plus the §2.9 `lv_entry.c`
pre-land is implemented, mock-verified, and green; the ABI (§2) is untouched (`lv_entry.c`
consumes only already-ratified `lv_abi.h` entry points, and the `LvPollFd`/`LV_POLLNVAL`
floor surface in `lv_plat.h` is unchanged). The one open item is the environment-gated
qemu execution the milestone itself scopes as conditional — not an architectural gap.
B-M5 (macOS/Windows floors) is the next milestone; its Win32 floor must map WSAPoll's
invalid-socket report to `LV_POLLNVAL` (already noted in `lv_plat.h`).

### 2026-07-05 — B-M5 first pass: macOS = POSIX-floor reuse (verified), Windows floor pre-landed to spec (compile-unverified). Fable-class agent.

Environment-gated milestone (roadmap P2). Dev host has **no MinGW-w64**, **no macOS**,
**no clang** — confirmed. `wine`/`wine64` present but unusable without a MinGW toolchain
to produce a PE. So this pass does the verifiable macOS-readiness work and pre-lands the
Windows floor to spec; it does **not** claim a working Windows build.

**macOS — no new floor file needed; bring-up is toolchain-only:**
- `runtime/build-triple.sh <darwin-triple>` selects `lv_plat_posix.c` with zero
  special-casing (macOS's mmap/poll/BSD-sockets/clock_gettime floor *is* the POSIX one,
  per §7). Verified: `build-triple.sh arm64-apple-macos` (mock cross CC) archives the
  posix floor + the other three sources.
- The driver's B-M4 cross probe already prefers `clang++ -target <triple>` (the ld64/lld
  driver §7 calls for) — no macOS-specific driver code.
- Object emission verified on this host: `--native-obj --target arm64-apple-macos` and
  `x86_64-apple-darwin` both produce genuine **Mach-O** relocatables (arm64 / x86_64).
- Net: macOS needs only a Darwin cross toolchain (or a Mac) to close; the code is done.

**Windows — `runtime/lv_plat_win32.c` PRE-LANDED, COMPILE-UNVERIFIED (no MinGW here):**
- Same pattern as B-M4's `lv_entry.c` pre-land: a new file, **inert** — in no default
  CMake target, so the green suite (**31/31**, re-confirmed) proves zero blast radius;
  `build-triple.sh` source-swaps it in only for `*-windows-*`/`*-mingw*` triples (proven:
  a windows triple invokes the compiler on `lv_plat_win32.c`, not `lv_plat_posix.c`).
  It is correctly **excluded** from the platform-floor audit (it *is* a floor, like
  `lv_plat_posix.c`). Object emission for `x86_64-pc-windows-gnu` verified (genuine amd64
  **COFF**), so Track A's triple param already feeds the Windows path.
- Implements the full §5 Win32 column against the same `lv_plat.h` interface and the same
  caller contracts as POSIX (portable open bits → `_O_*`+`_O_BINARY`; nonblocking accept
  `-1`==none; recv `0`==EOF; `LV_POLL*` translation). `WSAPoll`'s invalid-socket report is
  mapped to `LV_POLLIN|LV_POLLNVAL` — the retire-on-wake contract this floor was
  explicitly tasked with (lv_plat.h's B-M-2-pass note; the X64Gen `0x19` mask stays frozen
  with the latent gap, so no corpus program may arm a watch on a closed fd).
- **DESIGN DECISION logged for review (this went beyond mechanical adaptation — the docs
  left it implicit).** The fd bridge: `lv_plat.h` is a unified `int fd` space, but Windows
  has three disjoint descriptor kinds — console HANDLEs (GetStdHandle), CRT file fds
  (`_open`), and Winsock `SOCKET`s (`UINT_PTR`, **64-bit on Win64 — NOT castable to `int`
  without truncation**, which a naive port would get silently wrong). Resolution:
  console = fixed 0/1/2 → GetStdHandle; files = CRT int fds (always `< 0x40000000`);
  sockets = a small runtime-owned table, fd handed back as `LV_WIN_SOCK_BIAS(0x40000000)
  + slot`, so socket fds are disjoint from CRT fds and the 64-bit SOCKET is preserved
  losslessly. `write/read/close` route by testing the bias (`close` is polymorphic across
  files and sockets via the shared `sysclose` — verified from the call sites); `send/recv/
  poll/accept/set_nonblock` look the SOCKET up. The `int fd` ABI is unchanged on every
  caller. `WSAStartup` is lazy (first socket use) to keep the file self-contained; a
  bring-up may hoist it to an explicit `lv_plat_init()` hook if preferred. `stat_size`
  uses `GetFileAttributesExA` (no CRT `_stat64` struct-name ambiguity; correct >4 GB).
  Blocking connect (mirrors POSIX) side-steps B-H8 (WSAPoll's connect-failure blind spot
  only bites the async-connect path, which this floor does not use).
- **Static assurance only** (no Windows compiler to compile it): all 16 floor functions
  defined, braces/parens balanced, interface matches `lv_plat.h`. This is the ceiling
  reachable without a toolchain.

**Acceptance shortfall (honest):** the Windows floor's **compile + run** stays OPEN until
a MinGW-w64 toolchain exists — it is written, statically checked, and design-complete, but
not compiled. Two wiring items remain for whoever brings it up: the driver link step needs
`-lws2_32` + `.exe` output (deliberately **not** added blind to the tested `src/main.cpp`),
and a feature-detected wine test lane. macOS's floor+driver are done; only its toolchain
is missing.

**No STOP event.** macOS reuse is verified; the Windows floor is a scoped, inert,
design-complete pre-land with its one real architectural question (the fd bridge) resolved
and documented for review. The §2 ABI and `lv_plat.h`'s interface are untouched (the
Win32 floor implements the existing declarations; no new floor entry point was added).
Remaining B-M5 work is environment-gated bring-up, exactly as §7 frames it.

### 2026-07-06 — Cross-track note: Track A's A-M5 fixed two runtime bugs (owner-authorized).

Track A's A-M5 (LLVM system layer, Gate G1 — doc-1 §12 same date) is the first time the
whole corpus + the churn leak net run through `runtime/**`, and it surfaced two runtime
bugs invisible to B-M1's unit selftest and to the ELF backend (which uses X64Gen's own
machine-code natives, not these). The owner authorized the Track A agent to fix them in
place rather than hand them back (each is a completion of a documented-but-unfinished
contract, not a design change; the §2 ABI surface is untouched). Both verified across
every engine + `runtime_selftest` + ASan/UBSan; full `ctest` 35/35.

1. **`lvrt_sysstat` field 2 (mtime) returned -1** instead of the real `st_mtime`, breaking
   its own §2.7 note ("field 2=mtime … byte-parity with RuntimeNatives.cpp's sysStat") —
   the platform floor only had `lv_plat_stat_size`. Added `lv_plat_stat_mtime`
   (`lv_plat.h` + `lv_plat_posix.c`, mirroring stat_size) and wired field 2 in
   `lvrt_sysstat`. files.ext's `attrs.modified() > 0` now matches the oracle. NOTE for
   B-M5's Windows floor: `lv_plat_win32.c` needs a matching `lv_plat_stat_mtime`
   (`GetFileAttributesExA`'s `ftLastWriteTime` → epoch seconds) when it is brought up —
   the POSIX floor now defines it and `lv_plat.h` declares it.
2. **`lv_arr_boxed_append` leaked a dead `fresh` array on every geometric grow** —
   `LvValue fresh; lvrt_arr_new(&fresh, 0); … (void)fresh;` allocated an empty array that
   was never used and never freed (dead code, likely a refactor leftover — the grow branch
   builds its own `payload`). ~32B per grow; the churn leak net (A-M5's live-bytes gate)
   caught it. Removed the two dead lines; the array/index-set churn programs go flat.

Neither touched the ABI or the platform-floor public surface beyond the additive
`lv_plat_stat_mtime` declaration. **Still open for Track B / A-M6:** scope-local
value-struct reclamation on the LLVM backend needs an arena tier (Track A's A-M6); until
then two churn programs are `XFAIL-LLVM` (green on ELF) — see doc-1 §12.

### 2026-07-06 — Maintenance pass #3 (Fable): Track B-facing notes

Companion to doc-1 §12's same-date entry (which has the full pass). Track B-relevant:

- **`lv_plat_win32.c` gained `lv_plat_stat_mtime`** (FILETIME 100 ns ticks → epoch
  seconds, `GetFileAttributesExA`), closing the drift the A-M5 POSIX mtime addition
  opened. Same status as the rest of that floor: pre-landed, compile-unverified (still
  no MinGW-w64 on this host) — the B-M5 bring-up checklist is unchanged, and the §5
  floor table row now lists `stat_mtime` in both columns.
- **Agent-3's language branch merged to master** (floats, checker loudness, bug.md #1/#2).
  Its runtime-facing claim is narrower than it sounds: the float fix relevant to "native"
  went to `src/NativeRuntime.cpp`, the orphaned pre-runtime-v2 support file — **runtime
  v2's `lvrt_arith`/`lvrt_to_string` float rows were verified already correct** against
  the new `floats.ext` (byte-identical through the LLVM lane after Track A added the one
  missing codegen dispatch row, doc-1 §12). No `runtime/**` change was needed.
- **`lvrt_arith` div/mod-by-zero:** bug.md #10 (int `/0`,`%0` silently 0) is filed and
  awaiting an owner ruling. Runtime v2's scalar core replicates the oracle today —
  when the ruling lands it is a cross-engine change (oracle + CGen + X64Gen-frozen? +
  runtime v2) and needs a Fable-authored plan; no Track B agent should act unilaterally.
- Branches `track-a-llvm-parity` and `track-b-runtime` both fast-forwarded to the
  post-merge master; the Language-agent2 worktree was fast-forwarded to match.

### 2026-07-06 — B-M5 environment re-check (no toolchain) + A-M6 arena hardening. Sonnet-class agent.

**B-M5: toolchain re-probed, unchanged from the 2026-07-05 pass.** Confirmed (again)
this host has no `x86_64-w64-mingw32-gcc`/MinGW-w64, no `clang`, no Darwin cross
toolchain; `wine`/`wine64` are present but unusable without a MinGW-produced PE, exactly
as the prior pass found. Per §7's environment-gated framing, did **not** add the
Windows driver link step (`-lws2_32`, `.exe`) or a wine test lane blind, and did not
attempt to install a toolchain (apt has `g++-mingw-w64-x86-64` available over the
network, but installing a system-wide cross compiler on the shared dev host is outside
this milestone's authorized scope, not a call for an implementation agent to make
unilaterally). Re-verified the emission-level claim instead of taking the prior log on
faith: `lang --native-obj --target x86_64-pc-windows-gnu` on a corpus program still
produces a genuine `file(1)`-confirmed amd64 COFF object, and
`arm64-apple-macos`/`x86_64-apple-darwin` still produce genuine Mach-O (arm64 and
x86_64) — LLVM's built-in target backends do this without any cross toolchain, which
is why this half of B-M5 doesn't need one. Reviewed `lv_plat_win32.c` in full,
including the `lv_plat_stat_mtime` maintenance-pass-#3 addition (FILETIME→epoch-seconds
arithmetic checked against the same `nFileSizeHigh/Low`-style high/low combine
`lv_plat_stat_size` already uses); found no defect. Still PRE-LAND,
COMPILE-UNVERIFIED — unchanged shortfall, honestly logged again rather than assumed.
Full suite reconfirmed 35/35 green before touching anything.

**A-M6 arena hardening (the actionable half of this pass).** Doc-1 §9 is about to wire
`lvrt_arena_save`/`lvrt_arena_restore` into every LLVM frame prologue/return path; until
now the arena tier had only ever been exercised by B-M1's standalone bump/restore check
(`test_allocator`) — never nested, never interleaved with heap traffic, never combined
with an in-flight exception, never asked to survive an escaping value-struct copy.
Added `test_arena_hardening` to `runtime/selftest.c` (plus an `arena_obj_new` whitebox
helper that hand-builds an ARC-prefixed value struct via the raw
`lvrt_halloc(size+16, LV_TIER_ARENA)` contract — the exact calling convention Track A's
escape analysis will emit, since `lvrt_obj_new` is heap-tier only and no codegen reaches
this path yet):
1. **Nested save/restore marks** (recursive-call shape): outer mark, inner mark, alloc
   in both, restore inner (cursor rewinds exactly to the inner mark, verified against
   the next allocation's address), restore outer (same). Also a 4-deep repeated
   save/restore-at-the-same-depth loop (the shape a for-loop's per-iteration frame
   produces) asserting zero cumulative drift — the mark is bit-identical every
   iteration.
2. **Arena-then-heap interleaving**: 50 iterations of {arena save, arena alloc, heap
   string alloc+retain+release, arena restore}, asserting (a) the arena mark is
   identical every iteration (the two allocators never bleed into each other) and (b)
   `lvrt_live_bytes()` returns exactly to its pre-loop baseline (arena churn is fully
   invisible to the heap meter, as §2.5 requires).
3. **Restore under a pending throw**: arena-save, arena-alloc a temp, `lvrt_raise`,
   confirm `lvrt_throwing()`, `lvrt_arena_restore` (the unwinding frame's exit path),
   reconfirm the throw state and the thrown message are untouched, then `catch_bind` +
   `release`, asserting a full return to the pre-throw live-bytes baseline.
4. **Value-struct copies inside a restored region, 0xFE poison-checked**: build a
   nested value struct (a `Pair` holding another `Pair`, doc-2's existing `CLS_PAIR_VALUE`
   selftest fixture) entirely in the arena, `lvrt_copyval` it to the heap tier
   (recursing through the nested field), `lvrt_arena_restore` the source, `memset` the
   reclaimed arena bytes to `0xFE` (mirroring `lv_free_raw`'s own poison pattern), then
   re-read every field through the heap copy and confirm the values are unaffected —
   proving `lvrt_copyval`'s existing deep-copy discipline already makes an escaping
   value struct safe (doc-1 §9 calls a mis-arena'd escaping struct a UAF; this is the
   regression net for that specific hazard). `lvrt_vfree` on the rc-0 heap copy at the
   end returns live-bytes exactly to its pre-copy baseline.

**Bug found and fixed while writing item 3 (own file, mechanical completion, no ABI
change — proceeding without a STOP per §0.2).** `lv_recursive_free`'s `LV_OBJ` branch
walked only an object's static slots, never its dyn-list fallback (§2.4's "raw-node
fallback list" at payload offset 8 — the mechanism `RuntimeException.message` uses in
this selftest's fake class table, registered with `nslots=0`). `LV_CLO`'s branch right
below it already walks its own dyn-list (`captureHead`) correctly; objects were the
asymmetric, incomplete case. Net effect: any field reachable only via the dyn-list
outlived its owning object forever — both the 32-byte node and, if it held a counted
value, that value's refcount (a permanent leak, not just a delayed free). Reproduced
concretely: item 3's `lvrt_live_bytes() == excBaseline` assertion failed by exactly 96
bytes (64 for the leaked message string's size class + 32 for the leaked dyn node) with
the walk removed, and passed exactly with it restored — confirmed by literally reverting
the fix, rerunning, observing the predicted failure, then reapplying. Fixed by adding
the identical release-value/free-node walk `LV_CLO` already has, over the object's dyn
head. This is a layout- and ownership-model-preserving completion (the dyn-list shape
and the "owner counts its contents" discipline are already normative, §2.4/§2.5) — no
`lv_abi.h` change. **Production impact assessed as currently latent, not silent**:
every real class's fields are checker-declared and land in `slotNames` as static slots
(verified via `RuntimeException`'s actual definition, `src/Resolver.cpp:585-593` —
`message` is a declared field on the `Exception` base, so LlvmGen's real class table
gives it a static slot; the dyn-list is reached only by this selftest's deliberately
pared-down fake table, `nslots=0`, built to exercise the fallback mechanism itself). No
corpus/churn lane exercises the dyn-list today (`exception_churn.ext`'s `Boom` class
also declares `message`/`code` as real fields), which is consistent with 35/35 staying
green before this fix. Logged in case a future codegen path (or another runtime-internal
helper) ever populates an object's dyn-list — the net is now closed either way.

**Acceptance.** `runtime_selftest` green under gcc 13.3 and under
`-fsanitize=address,undefined` (a scratch `-DLANG_RT_SANITIZE=ON` build, not the
tracked build dir). Full `ctest` suite **35/35**, unchanged from the pre-pass baseline —
this pass touched only `runtime/lv_runtime.c` (the one-branch fix) and
`runtime/selftest.c` (the new test); no other file changed.

**No STOP event.** No `lv_abi.h` change, no cross-track file touched, no design
correction — a mechanical bug fix in Track B's own file plus new regression coverage
for an API surface Track A is about to depend on. B-M5 remains environment-gated exactly
as before (unchanged shortfall, re-confirmed rather than re-asserted from memory).

### 2026-07-06 — Maintenance pass #4 (Fable): Track B-facing notes

Companion to doc-1 §12's same-date entry (which has the full pass). Track B-relevant:

- **Agent-3's Track 01 merged to master** (int shift/xor/complement, literals, escapes,
  string interpolation). Runtime v2 was brought to the ratified F1 semantics as part of
  the LLVM-parity fix: `LV_OP_XOR = 15` added to `lv_abi.h`'s operator enum;
  `lvrt_arith`'s int shift rows now **raise** (`"shift count out of range"`, counts
  outside 0..63) instead of the x86-style `& 63` mask — unreachable from LLVM-emitted
  code (its inline int-int path range-checks first) but semantics-of-record for any
  future caller; `lvrt_opm`'s `kOpNames` gains `"^"` (guard widened to `< 16`). All
  additive; no existing entry point changed shape. `runtime_selftest` green under gcc
  and under a scratch `-DLANG_RT_SANITIZE=ON` build; full `ctest` 36/36.
- **§2.10 contract addition ratified** (the STOP-gated levers doc-1's A-M6 logged):
  `lv_g_throwing` and `lv_g_arena_cursor` become exported globals. The runtime-side
  rename is owner-authorized to the Track A agent implementing A-M7 (doc-1 §9b) —
  read §2.10 for the exact contract; everything else under `runtime/**` stays Track B's.
- **bug.md #12 filed** (float `/0.0`): `lvrt_arith`'s float-row 0.0-override is now the
  odd engine out (interpreters, emit-C++, and the frozen ELF backend are all IEEE).
  Awaiting an owner ruling like #10 — no Track B agent should act unilaterally.
- **Next Track B work is unchanged**: B-M4's qemu-execution bullet and B-M5's
  Windows compile+run / macOS toolchain bring-up, all environment-gated. Installing
  cross toolchains on the shared dev host is an owner-authorization item; if granted,
  the aarch64 lane (`corpus_core_aarch64_qemu`) auto-enables at CMake reconfigure with
  no code change, and the Windows floor's two deferred wiring items (driver `-lws2_32`
  + `.exe` output, a feature-detected wine lane) become implementable.
- Branches `agent3`, `track-a-llvm-parity`, `track-b-runtime` fast-forwarded to the
  post-merge master; the Language-agent3 worktree updated in place.

### 2026-07-06 — B-M4 CLOSED (qemu-user execution) + B-M5 Windows CLOSED (core+objects
bring-up); macOS re-verified emission-only, no toolchain. Sonnet-class agent.

Owner-authorized cross toolchains installed on this shared dev host (previously out of
agent scope, gate lifted for this pass): `gcc-mingw-w64-x86-64`, `g++-mingw-w64-x86-64`,
`gcc-aarch64-linux-gnu`, `qemu-user`. Confirmed on PATH: `x86_64-w64-mingw32-gcc/g++`,
`aarch64-linux-gnu-gcc`, `qemu-aarch64`; `wine`/`wine64` were already present. Baseline
re-verified with a fresh reconfigure + rebuild before any edit: `ctest` **35/35** — note
this is one lower than the prior entry's claimed "36/36"; not investigated further (it
predates this pass, isn't in Track B's changed-file set, and doesn't affect any
acceptance criterion below, all of which were measured directly against this session's
own before/after runs rather than trusted from the log).

**B-M4 (aarch64) — CLOSED, Gate G2 (aarch64) CLOSED.** CMake's existing feature-detect
(`find_program(qemu-aarch64)` + `find_program(clang / aarch64-linux-gnu-gcc)`) picked up
the new toolchain on reconfigure and auto-enabled `corpus_core_aarch64_qemu` with no
CMake change needed for detection itself. Two real (mechanical, not architectural)
bugs surfaced getting it green, both logged per §0.2's "mechanical adaptation" carve-out
— no STOP:

1. **qemu couldn't find the aarch64 loader** (`Could not open '/lib/ld-linux-aarch64.so.1'`).
   The Debian/Ubuntu `aarch64-linux-gnu-gcc` cross package bakes its own sysroot
   (`/usr/aarch64-linux-gnu`) into the compiler driver — headers/libs resolve correctly
   at compile+link time with no `--sysroot` flag needed, which is why `build-triple.sh`
   and the driver's link step both worked on the first try. But `qemu-aarch64` doesn't
   consult gcc's specs; it needs an explicit `-L` to remap the binary's absolute loader
   path to where the libraries actually live. Fixed by auto-wiring
   `LVRT_SYSROOT=/usr/aarch64-linux-gnu` onto the `corpus_core_aarch64_qemu` ctest via
   `set_tests_properties(... PROPERTIES ENVIRONMENT ...)`, gated on the directory
   existing and the detected cross compiler not being clang (clang's own `-target` path
   already threads `LVRT_SYSROOT` itself per `build-triple.sh`'s existing logic).
2. **Every corpus diff failed by exactly one trailing line** even after fix 1:
   `[heap] escaping-tier peak=... live-at-exit=... bytes` — the §2.5 ARC meter
   `LlvmGen.cpp`'s `lv_main` always writes to fd 2 by design ("stdout is untouched, so
   the corpus differentials are unaffected"). `tests/run_qemu_cross.sh` merged stderr
   into the captured output (`2>&1`) before diffing against `.expected`, unlike
   `run_native_llvm.sh`'s established convention of diffing stdout only. Fixed by
   dropping the merge in `run_qemu_cross.sh` — brings it in line with the existing
   convention, not a new one.

Verified directly (outside ctest): a hand-written hello-world (`console.writeln("hello,
aarch64")`) and `tests/corpus/core/arith.ext` both cross-build and run correctly under
`qemu-aarch64` — satisfying §6's acceptance text verbatim ("an aarch64 hello +
`arith.ext` binary runs under qemu-user"). The `corpus_core_aarch64_qemu` ctest lane is
green (5/5 core corpus programs). §6's acceptance and item 4 updated above (strikethrough
+ DONE) to match.

**B-M5 Windows — core+objects corpus CLOSED.** `runtime/build-triple.sh` and
`probeCrossLinkerDriver` (`src/main.cpp`) both assumed the LLVM target triple string
doubles as the cross-toolchain's binary-name prefix — true for `aarch64-linux-gnu`, false
for Windows: the MinGW-w64 packages install as `x86_64-w64-mingw32-{gcc,g++,ar}`; no
`x86_64-pc-windows-gnu-*` binary exists under any package. Added a small triple → prefix
mapping (`gnu_prefix` in `build-triple.sh`; `gnuToolchainPrefix()` in `main.cpp`) used
only for the *compiler invocation* — the LLVM triple string stays the archive-directory
key (`findRuntimeArchiveForTriple` untouched), so the driver's resolution logic is
unaffected. Mechanical, logged, no STOP.

With that one fix, `lv_plat_win32.c` **compiled clean under `-Wall -Wextra` with zero
source changes** — surprising given its prior PRE-LANDED/COMPILE-UNVERIFIED status, but
directly verified (full `build-triple.sh` output inspected, no warnings emitted; all four
sources — `lv_runtime.c`, `lv_plat_win32.c`, `lv_loop.c`, `lv_entry.c` — archived into
`runtime/x86_64-pc-windows-gnu/liblvrt.a`). The fd-bridge design from the prior B-M5 pass
held up unchanged under real compilation.

Wired both deferred driver items in `src/main.cpp`'s `--build-native` path (new
`isWindowsTriple()` helper): `-lws2_32` appended to the link command for a Windows/mingw
target triple (Winsock2, per `lv_plat_win32.c`'s `WSAStartup`/socket calls), and `.exe`
appended to the output path when the caller's `--build-native <out>` didn't already
supply the extension (wine/Windows only execute a binary literally named `*.exe`).

End-to-end verified: `lang --build-native /tmp/t --target x86_64-pc-windows-gnu
tests/corpus/core/arith.ext` produces `/tmp/t.exe`, confirmed by `file` as a genuine
`PE32+ executable (console) x86-64, for MS Windows`; runs correctly under both `wine` and
`wine64`. Swept the full core corpus (5 programs) **and** `llvm_objects` corpus (3
programs — `basic_object`/`multi_field`/`nested_object`) through cross-build + wine — all
8 byte-match `.expected` (stdout only, same convention as the qemu lane; the
escaping-tier meter line on fd 2 is correctly excluded).

Added `tests/run_wine_cross.sh` (mirrors `run_qemu_cross.sh`'s structure: build the
per-triple archive, cross-build each corpus program, run the `.exe` under wine, diff
stdout against `.expected`; takes one or more corpus directories) and a CMake
`corpus_win_wine` test in the Track-B region, feature-detected on `find_program(wine)` +
`find_program(x86_64-w64-mingw32-gcc)` — skips cleanly (with a `message(STATUS ...
SKIPPED)`) on hosts without a Windows cross environment. Scope is explicitly core +
objects, per this milestone's own instruction: net-corpus programs (timers/sockets/http/
async) are excluded — they bind fixed loopback ports and would need the `RESOURCE_LOCK
corpus_net_ports` property, which this lane deliberately does not carry. §7 updated above
to match (Windows status line, Acceptance line).

**macOS — re-verified, unchanged, no toolchain installed.** Confirmed (again) no Darwin
cross toolchain exists on this host (checked for `osxcross`, `o64-clang`,
`{arch}-apple-darwin-clang` — none found) and did **not** install one — Darwin bring-up
was not in the owner's authorization for this pass (only mingw-w64/aarch64-gcc/qemu-user
were named). Re-ran the emission-only checks rather than trusting the prior pass's log:
`--native-obj --target arm64-apple-macos` and `x86_64-apple-darwin` on a corpus program
both still produce genuine Mach-O relocatables (`file`-confirmed, arm64 and x86_64
respectively). No change from the 2026-07-06 prior pass; bring-up remains
hardware/toolchain-gated exactly as §7 frames it.

**Full verification:**
- `ctest`: **37/37** (35 pre-existing + `corpus_core_aarch64_qemu` + `corpus_win_wine`),
  all green, via the tracked `build/` dir.
- `runtime_selftest`: green under the tracked plain build **and** under a scratch
  `-DLANG_RT_SANITIZE=ON` build (configured in the session scratchpad, not the tracked
  `build/`, per the established convention) — ASan/UBSan clean, exit 0, all fixture
  groups (allocator, ARC graph, strings, arrays/maps, dispatch, scalar core, exceptions,
  arena hardening, event loop, sys/file natives, sockets) reported OK.
- `rt_platform_audit`: clean, scope unchanged (`lv_runtime.c`/`lv_loop.c`/`lv_entry.c`
  only — `lv_plat_win32.c` and `lv_plat_posix.c` remain correctly excluded as floor
  files).
- No Track A file touched (confirmed via diff: only `CMakeLists.txt` Track-B region,
  `runtime/build-triple.sh`, `src/main.cpp`'s cross-probe/link functions,
  `tests/run_qemu_cross.sh`, and new `tests/run_wine_cross.sh`). No `.expected` file
  edited. Generated per-triple archives (`runtime/aarch64-linux-gnu/`,
  `runtime/x86_64-pc-windows-gnu/`) are untracked/gitignored (existing `*.a` rule) —
  left in place as ordinary build output, same as the tracked `build/` dir.

**No STOP event.** Every fix this pass was toolchain/environment plumbing — a compiler
binary-name mapping, a qemu sysroot flag, a stderr-redirect convention fix — none of it
an ABI (§2) or architectural change, squarely inside §0.2's "mechanical adaptation"
carve-out. B-M4 and B-M5 (Windows, core+objects) are now closed; remaining open items are
macOS hardware/toolchain access (unchanged, honestly re-logged) and Windows net-corpus
coverage (sockets/timers/http/async — out of this pass's explicit scope, would need the
`RESOURCE_LOCK corpus_net_ports` group first).
