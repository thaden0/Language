# Tech Design: LLVM Filesystem and Directory Native Parity

**Status:** IMPLEMENTED AND VERIFIED — archived after the active acceptance gates.
**Date:** 2026-07-19.
**Request:** `designs/requests/accepted/request-llvm-fs-dir-natives.md`.
**Research:** `designs/requests/research-llvm-fs-dir-natives.md`.
**Grounding commit:** `5ba1fe7` (`agent5`, equal to `origin/master` when audited).
**Primary owners:** LLVM code generation, the C runtime ABI, and the platform floor.

**Decision, up front:** add LLVM-native support for `std::sysRemove`,
`std::sysRename`, and `std::sysListDir` by extending the existing
`lv_plat_*` seam, the `lvrt_*` ABI, and `LlvmGen`'s native dispatch. Preserve
the already-landed oracle/IR behavior exactly. Use a single-pass,
floor-owned `LvDirEntries` buffer for directory enumeration. `sysListDir`
returns a fresh boxed `Array<string>` or `LV_NONE`; the LLVM call site applies
the normal unconditional `retainDst()`, which is already safe for `None`.

The implementation covers POSIX and supplies equivalent Win32 floor bodies.
The browser-wasm target remains intentionally filesystem-free: all three
natives join the existing `File` capability gate and receive link-safe wasm
floor stubs. The emit-C++ backend and the frozen pure-ELF backend are not
extended. In command terms, `leviathan --build-native` and package-level
`trident build` are in scope; `leviathan --build` is the emit-C++ path and is
not.

## Implementation record (2026-07-19)

The implementation landed as designed across the platform floor, runtime ABI,
LLVM dispatch, wasm capability gates, focused differential coverage, runtime
ownership tests, and reference documentation. There were no language-surface,
IR, emit-C++, pure-ELF, or Sonar source/test/runner changes. One stale Sonar
design-log paragraph was updated after the unchanged consumer test became
green.

As-built verification:

- baseline failures were reproduced first: the filesystem differential and
  `sonar_v2/tests/dom-dialogs` stopped specifically at LLVM `sysRemove`;
- `runtime_selftest`, its Valgrind lane, the platform audit, `sys_natives`, and
  the complete wasm corpus passed; the three new wasm fixtures also passed in
  isolation with the exact `File` capability diagnostic;
- the focused Sonar project passed oracle + IR + LLVM with its documented
  emit-C++ async/native-gap skip, and `trident build sonar_v2/tests/dom-dialogs`
  produced the package executable;
- the sanitizer-enabled runtime selftest passed from an out-of-tree build;
- AArch64 and Win32 runtime archives compiled; the cross corpus passed under
  QEMU and Wine, and a Win32 filesystem round trip executed successfully under
  Wine;
- the full 247-test CTest run plus a clean target-specific Windows rerun left
  every one of the 246 active tests green. The sole remaining failure is
  `corpus_churn_leak`'s frozen-ELF `field_cow_across_methods.ext` case. An
  isolated build of untouched `origin/master` at `4d4bcad` reproduced the exact
  `16640 -> 128640` byte result (`+160` bytes per iteration), proving it is
  pre-existing. `info.md` explicitly excludes frozen ELF from project targets
  and states that no design or task is gated on an ELF finding.

No required active test was skipped. The only as-built verification nuance was
that the AArch64 and Windows cross tests had to be invoked with target-specific
environments; exporting the AArch64 sysroot into the Wine test hides MinGW's
standard headers, while the clean Wine invocation passes.

---

## 0. Contents

1. Problem and desired outcome
2. Normative language-level contract
3. Current architecture and exact gap
4. Scope and target matrix
5. Ratified design decisions
6. Platform-floor API and ownership contract
7. POSIX and Win32 behavior
8. Runtime ABI and ARC construction
9. LLVM dispatch and wasm capability handling
10. Security, reliability, concurrency, and performance
11. File-by-file implementation plan
12. Milestones and delivery order
13. Verification matrix and acceptance commands
14. Rollout, compatibility, and rollback
15. Risks and failure handling
16. Alternatives considered
17. Request-acceptance mapping and definition of done
18. Source map

---

## 1. Problem and desired outcome

Track 08 F3 already exposes and implements this family in the front end and
both interpreters:

```lev
int std::sysMkdir(string path);
int std::sysRemove(string path);
int std::sysRename(string from, string to);
Array<string>? std::sysListDir(string path);
```

`sysMkdir` completed the trip through LLVM, the C runtime, and the platform
floor. The remaining three stopped at `src/RuntimeNatives.cpp`. Any LLVM AOT
program that reaches one of them therefore fails during code generation with
the generic unimplemented-native diagnostic. `sonar_v2/tests/dom-dialogs/`
is the first repository consumer that requires a real directory listing on
its LLVM lane, but the missing support also blocks file managers, installers,
build tools, and ordinary file-open dialogs.

This is backend parity work, not a new language feature. There is no new
syntax, type, prelude declaration, permission model, or high-level filesystem
API.

### 1.1 Goals

- Make the three existing natives compile, link, and run through LLVM AOT.
- Match the oracle/IR result values and filesystem effects for all specified
  success and failure cases.
- Keep every operating-system call below `runtime/lv_plat.h`.
- Make directory-buffer ownership and ARC transfer rules explicit and
  mechanically testable.
- Preserve the wasm-browser capability boundary with a deterministic
  compile-time diagnostic.
- Supply Win32 parity bodies without overstating their verification status.
- Turn the existing environment-dependent filesystem scenario into a
  three-engine differential test.
- Unblock the existing Sonar DOM-dialog LLVM lane without changing Sonar.
- Correct the now-stale system-native engine-coverage documentation.

### 1.2 Non-goals

- No recursive deletion, copy, move-across-filesystems fallback, globbing,
  path normalization, canonicalization, or directory watching.
- No ordering guarantee for `sysListDir` and no sorting in the runtime.
- No rich `errno`, `GetLastError`, exception, or diagnostic payload at the
  language boundary; the existing `0`/`-1`/`None` contract remains binding.
- No `Dir`, `Path`, or `FileSystem` wrapper redesign.
- No checker, resolver, IR-op, bytecode, or interpreter feature work.
- No emit-C++ (`src/CGen.cpp`) support in this ticket.
- No pure-ELF (`src/X64Gen.cpp`) support. That backend is frozen and is never
  extended under `info.md`'s backend policy.
- No browser filesystem emulation, WASI preopens, File System Access API, or
  host callback for these operations.
- No claim that Win32 is release-certified until a MinGW/Wine or native
  Windows lane actually compiles and executes the focused cases.

### 1.3 Success signal

The same temporary-directory program must print byte-identical output on
`--run`, `--ir`, and an executable produced by `--build-native`. The focused
Sonar dialog project must then pass its existing oracle/IR/LLVM golden with no
package-side edits.

---

## 2. Normative language-level contract

The declarations in `src/Resolver.cpp` and the behavior in
`src/RuntimeNatives.cpp` are the semantic source of truth.

| Native | Success | Failure | ARC class |
|---|---|---|---|
| `sysRemove(path)` | `0`; removes one file, symlink, or empty directory | `-1`; missing path, permission failure, non-empty directory, or any other failure | scalar |
| `sysRename(from, to)` | `0`; delegates to the platform's rename/move primitive | `-1`; missing source, permissions, cross-device move, invalid replacement, or any other failure | scalar |
| `sysListDir(path)` | `Array<string>` containing entry names | `None` when a listing cannot be opened or staged | optional heap value |

### 2.1 `sysRemove`

On POSIX, behavior is deliberately ordered:

1. call `unlink(path)`;
2. return `0` if it succeeds;
3. only when the captured failure is `EISDIR` or `EPERM`, call
   `rmdir(path)`;
4. return `0` if that succeeds, otherwise `-1`;
5. for every other `unlink` failure, return `-1` without calling `rmdir`.

That sequence is part of the contract because it preserves ordinary unlink
semantics for files and symlinks while allowing an empty directory through
the same native. It is not recursive. A non-empty directory must remain and
return `-1`.

Win32 reaches the same observable `0`/`-1` result by choosing
`DeleteFileA` or `RemoveDirectoryA` from file attributes. The platform floor
is allowed to differ internally; callers do not observe an errno or Windows
error code.

### 2.2 `sysRename`

`sysRename` delegates to `rename(2)` on POSIX and to `MoveFileExA` with
`MOVEFILE_REPLACE_EXISTING` on Win32. It does not copy across volumes, create
parents, normalize either path, or retry. Replacement behavior is whatever
the target platform supports under those primitives, collapsed to `0` or
`-1`.

No stronger atomicity promise is introduced. POSIX rename atomicity remains
available where the OS provides it; the portable language contract is only
success/failure plus the resulting filesystem state.

### 2.3 `sysListDir`

The returned values are **entry names**, not joined or absolute paths.

- `.` and `..` are always omitted.
- Other dot-prefixed entries are ordinary entries and remain present.
- An empty directory returns `[]`.
- A missing path, a non-directory path, or a directory that cannot be opened
  returns `None`.
- Enumeration order is unspecified and follows the platform iterator. No
  caller may depend on lexical, creation, inode, or stable order.
- Duplicate names are not synthesized or removed; the platform iterator is
  authoritative.
- Directory mutation during enumeration has the platform's weakly consistent
  behavior. This API is not a snapshot or transaction.
- Names are copied into Leviathan byte strings. POSIX bytes and the existing
  Win32 ANSI-path convention are preserved; this ticket does not introduce a
  new encoding layer.
- Embedded NUL path behavior remains the existing C-floor behavior and is not
  expanded here.

The floor discards and frees any partially staged result if its own memory
allocation fails. Because `Array<string>?` has no detailed error channel,
that failure maps to `None`; no partial array escapes. Normal iterator
termination retains the entries already observed, matching the oracle's
`readdir` loop and its intentionally weak snapshot semantics.

### 2.4 Compatibility rule

These signatures and outcomes already exist. Adding LLVM coverage must not
change source resolution, overload selection, comptime I/O denial, oracle/IR
output, or the behavior of already-supported `sysStat`/`sysMkdir`.

---

## 3. Current architecture and exact gap

```text
Leviathan source
  std::sysListDir(path)
        |
        v
Resolver prelude signature                 already complete
        |
        v
Op::CallNativeFn("sysListDir")             already complete
        |
        +----------------------+----------------------+
        v                      v                      v
tree-walk oracle          IR interpreter          LLVM AOT
        |                      |                      |
        +---- RuntimeNatives.cpp (complete)           | missing dispatch
                                                       v
                                              lvrt_syslistdir   missing
                                                       |
                                                       v
                                              lv_plat_listdir   missing
                                               /      |       \
                                          POSIX    Win32     wasm stub
```

The change begins at the LLVM `CallNativeFn` lowering and continues downward
through the runtime ABI and platform seam. Nothing above `CallNativeFn` needs
to change.

The research snapshot predated or overlooked one current-tree fact:
`runtime/lv_plat_wasm.c` now exists. It intentionally returns failure for
filesystem operations while the compiler capability-gates reachable calls.
The new floor interface therefore has **three**, not two, production
implementations to keep link-complete, plus the standalone wasm spike floor in
`tests/spike-wasm/lv_plat_spike.c`.

---

## 4. Scope and target matrix

| Execution path / target | State after implementation | Gate |
|---|---|---|
| tree-walk oracle | unchanged, supported | existing corpus |
| IR interpreter | unchanged, supported | existing corpus |
| LLVM host POSIX | newly supported and required | blocking acceptance |
| LLVM aarch64 POSIX | same C floor and ABI; compile/run when cross lane exists | existing feature-detected cross lane |
| LLVM Win32 | parity bodies added; conditional compile/run | non-blocking when toolchain absent; never claim verified if skipped |
| LLVM wasm-browser | deliberately unavailable | compile-time `File` capability diagnostic |
| emit-C++ / `leviathan --build` | unchanged clean coverage error | explicitly out of scope |
| package `trident build` | newly works because it drives LLVM `--build-native` | blocking consumer acceptance |
| pure ELF / `--emit-elf` | unchanged clean coverage error | frozen, never a target |

The request's phrase “`--build-native`/`--build` on a package” is resolved as
`leviathan --build-native` plus `trident build`. A literal
`leviathan --build` uses `CGen`/emit-C++ and conflicts with the repository's
documented system-layer boundary, so it is not silently pulled into scope.

---

## 5. Ratified design decisions

### D1. Use one single-pass, floor-owned directory buffer

`lv_plat_listdir` performs one OS enumeration and stages names in a growable
`char **`. The runtime then sizes the language array exactly once and copies
each name.

This is chosen over a two-pass count/fill protocol because directory contents
can change between passes, a second open can fail independently, and the API
would be more complex across POSIX and Win32. Directory listing is not a
per-frame hot path; one transient staging buffer is the better reliability
trade.

### D2. Keep `None` in the ordinary tagged-value representation

There is no optional-array wrapper and no LLVM branch around ARC. The runtime
writes either `{LV_ARR, payload}` or `{LV_NONE, 0}`. The dispatch calls
`retainDst()` unconditionally. `lvrt_retain` already excludes `LV_NONE` via
`lv_is_counted`, exactly as the LLVM-covered `sysEnv() -> string?` path does.

### D3. Keep OS interaction below `lv_plat.h`

`lv_runtime.c` may decode `LvValue`, allocate Leviathan strings/arrays, and
translate the floor result. It must not include `dirent.h`, call
`unlink`/`rename`, or reference Win32 APIs. The platform audit remains clean.

### D4. Use `File` for the wasm capability diagnostic

The three entries use:

```text
wrapper = "File"
why    = "no filesystem in a browser"
```

There is no `Dir` wrapper to name, and `File` is already the diagnostic owner
for `sysOpen`, `sysStat`, and `sysMkdir`. This keeps one stable capability
vocabulary for the full filesystem family.

### D5. Implement Win32 parity, but report verification honestly

The interface must not land with missing Win32 symbols. Bodies are written to
the current `*A` API convention and compile-tested when MinGW is present.
Absence of MinGW/Wine is an explicit skip, not evidence of support. The host
POSIX and LLVM gates remain the required delivery boundary for this request.

### D6. Do not extend emit-C++ or frozen ELF

`CGen` does not carry even the full `sysStat`/`sysMkdir` family, and the project
defines emit-C++ as excluding the system layer. `X64Gen` is frozen by policy.
Their existing deterministic coverage diagnostics are correct behavior.

### D7. Bundle tests and coverage documentation with the implementation

The test already exists and only lacks LLVM legs. Shipping code without
promoting that differential or without fixing the reference engine matrix
would recreate the original silent coverage drift.

---

## 6. Platform-floor API and ownership contract

Add the following next to the existing stat/mkdir block in
`runtime/lv_plat.h`:

```c
typedef struct LvDirEntries {
    char** names;
    int64_t count;
} LvDirEntries;

int  lv_plat_remove(const char* path);
int  lv_plat_rename(const char* from, const char* to);
int  lv_plat_listdir(const char* path, LvDirEntries* out);
void lv_plat_listdir_free(LvDirEntries* entries);
```

### 6.1 Scalar contracts

- `lv_plat_remove`: `0` on removal, `-1` otherwise.
- `lv_plat_rename`: `0` on rename/move, `-1` otherwise.
- Neither stores an error globally or logs a path.

### 6.2 List contract

`lv_plat_listdir` returns `0` with an owned result or `-1` with an empty
result.

Required invariants:

1. Validate `out` at the boundary used internally; callers always pass a
   non-null pointer.
2. Initialize `out->names = NULL` and `out->count = 0` before opening the
   directory.
3. On success, `count >= 0`; each `names[i]` is a separately owned,
   NUL-terminated C string.
4. Empty success is `{NULL, 0}`, not a fabricated empty string entry.
5. On failure, no directory/search handle and no allocation remains live,
   and the output is reset to `{NULL, 0}`.
6. `lv_plat_listdir_free` accepts `{NULL, 0}`, frees every staged name and the
   pointer table, then resets the structure. Calling it on an initialized
   empty result is safe.
7. Ownership is per call. There is no static buffer, cached handle, or shared
   mutable state, so concurrent callers do not race inside this helper.

### 6.3 Allocation discipline

- Grow the pointer table geometrically, beginning with a small capacity such
  as 16.
- Assign `realloc` to a temporary and publish it only on success; never lose
  the old pointer on allocation failure.
- Duplicate names with an explicit `malloc(strlen + 1)` plus `memcpy`, rather
  than depending on platform-specific `strdup` declarations.
- Guard `capacity * sizeof(char*)`, capacity doubling, `strlen + 1`, and the
  final conversion to `int64_t` against overflow.
- Do not impose an arbitrary entry-count cap in this parity ticket. The
  platform allocator and the existing runtime heap limit remain the resource
  bounds.
- On staging allocation failure, close the enumeration handle, free all
  staged names, reset `out`, and return `-1`; no partial list is exposed.

This interface is internal to the runtime build, not part of the language
value ABI. The three new `lvrt_*` symbols in §8 are the compiler-facing ABI.

---

## 7. POSIX and Win32 behavior

### 7.1 POSIX floor

`runtime/lv_plat_posix.c` adds `<dirent.h>` and implements the exact oracle
shape.

Normative pseudocode for remove:

```c
if (unlink(path) == 0) return 0;
int unlink_error = errno;
if (unlink_error == EISDIR || unlink_error == EPERM)
    return rmdir(path) == 0 ? 0 : -1;
return -1;
```

Capturing `errno` immediately is required; no allocation, logging, or helper
call may overwrite it before the fallback decision.

Rename is `rename(from, to) == 0 ? 0 : -1`.

List behavior:

1. `opendir(path)`; failure returns `-1` with an empty output.
2. Loop over `readdir`.
3. Skip names equal to `.` or `..` and no others.
4. Copy every accepted `d_name` into the growable result.
5. `closedir` on every success or failure path after a successful open.
6. Publish the result only after cleanup-sensitive work succeeds.

The output is not sorted. The design does not rely on `d_type`, so filesystems
that report `DT_UNKNOWN` behave identically.

### 7.2 Win32 floor

`runtime/lv_plat_win32.c` follows the file's existing ANSI API convention.

- Remove: inspect `GetFileAttributesA`; use `RemoveDirectoryA` for a directory
  and `DeleteFileA` otherwise. Invalid attributes return `-1`.
- Rename: use `MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING)`. Do not add
  `MOVEFILE_COPY_ALLOWED`, because a hidden cross-volume copy would diverge
  from POSIX `rename`'s `EXDEV` failure shape.
- List: build a dynamically allocated `path + "\\*"` search pattern, use
  `FindFirstFileA`/`FindNextFileA`, omit `.`/`..`, and always call
  `FindClose` after a successful first find.

A fixed `MAX_PATH` stack buffer is rejected: it can truncate silently and
turn a safe request into a listing of the wrong path. Dynamic construction
still inherits the repository's existing Win32 `A`-API and long-path limits;
moving the full floor to UTF-16/`*W` APIs is separate work.

The attribute-query/remove sequence has an unavoidable time-of-check/time-of-
use window. A concurrent type change may turn an otherwise valid request into
`-1`; it must never trigger recursive removal or a second unrelated target.

### 7.3 wasm-browser floor

`runtime/lv_plat_wasm.c` must define all four new interface symbols so the
archive remains link-complete:

- scalar operations return `-1`;
- `lv_plat_listdir` initializes `{NULL, 0}` and returns `-1`;
- `lv_plat_listdir_free` is a safe no-op/reset.

These bodies are defense in depth and satisfy unreachable prelude linkage.
A reachable user call is rejected by the compiler before it can invoke them.

`tests/spike-wasm/lv_plat_spike.c` receives matching stubs because it is a
second complete implementation of the same interface used by the standalone
wasm spike.

---

## 8. Runtime ABI and ARC construction

### 8.1 ABI declarations

Add beside `lvrt_sysstat`/`lvrt_sysmkdir` in `runtime/lv_abi.h`:

```c
void lvrt_sysremove(LvValue* out, const LvValue* path);
void lvrt_sysrename(LvValue* out, const LvValue* from, const LvValue* to);
void lvrt_syslistdir(LvValue* out, const LvValue* path);
```

Every argument remains an `LvValue*`; raw scalar/pointer shortcuts are not
introduced.

### 8.2 Scalar wrappers

`lvrt_sysremove` and `lvrt_sysrename` decode the runtime string payloads,
write `LV_INT`, and forward the platform return code. They allocate nothing
and must not retain either argument.

### 8.3 `sysListDir` wrapper

The wrapper is responsible for translating platform-owned C strings into one
runtime-owned boxed array:

```text
LvDirEntries entries = empty
if floor list fails:
    out = {LV_NONE, 0}
    return

out = fresh boxed Array<string>(entries.count), rc 0
for each entry:
    s = fresh runtime string copy, rc 0
    store s in the boxed element slot
    retain s once so the array owns the element, rc 1
free the floor-owned entries
return the array at rc 0
```

This deliberately copies the `lvrt_sysargs` construction pattern:

- `lvrt_arr_new` pre-sizes the array;
- `lv_st_val` writes the slot without hidden ARC;
- `lvrt_retain(&s)` gives the array its one element ownership count;
- the array itself leaves the runtime wrapper fresh/unowned at rc 0;
- LLVM `retainDst()` turns that into the destination register's +1 transfer.

The floor buffer is freed only after every name has been copied. The runtime
allocator's established exhaustion path terminates loudly; it does not return
half-constructed values, so there is no recoverable branch between individual
string copies.

### 8.4 Why unconditional retain is correct

`CallNativeFn` has `destKind == 2`: the generic wrapper releases the old
destination value but does not retain the new one. Heap-returning native cases
must therefore perform their own `retainDst()`.

For success, the call changes the fresh array from rc 0 to rc 1. For failure,
`lvrt_retain` sees `LV_NONE`; `lv_is_counted` rejects it before any payload
dereference, so the retain is a no-op. This is the same proven pattern as
`sysEnv() -> string?`, `sysRecv`, and `sysRsaEncrypt`.

Calling `retainDst()` conditionally in LLVM is rejected because it duplicates
runtime tag logic and creates a new optional-value convention.

---

## 9. LLVM dispatch and wasm capability handling

### 9.1 Runtime callee declarations

Add `rtSysRemove`, `rtSysRename`, and `rtSysListDir` to `Gen`'s
`FunctionCallee` members and initialize them with:

| Callee | C symbol | LLVM argument shape |
|---|---|---|
| `rtSysRemove` | `lvrt_sysremove` | `(out, path)` |
| `rtSysRename` | `lvrt_sysrename` | `(out, from, to)` |
| `rtSysListDir` | `lvrt_syslistdir` | `(out, path)` |

All values cross as pointers to the 16-byte `LvValue` pair, matching the rest
of the native ABI.

### 9.2 Dispatch cases

Place the cases beside `sysStat`/`sysMkdir`:

```cpp
} else if (n == "sysRemove") {
    b.CreateCall(rtSysRemove, {regs[in.a], arg(0)});
} else if (n == "sysRename") {
    b.CreateCall(rtSysRename, {regs[in.a], arg(0), arg(1)});
} else if (n == "sysListDir") {
    b.CreateCall(rtSysListDir, {regs[in.a], arg(0)});
    retainDst(); // fresh Array<string> -> +1; None gate-skips
}
```

The scalar cases do not retain. The existing post-dispatch throw check and
generic destination release remain unchanged.

### 9.3 wasm gate entries

Add all three natives to the single `wasmGatedNative` table before normal
dispatch:

```cpp
{"sysRemove",  "File", "no filesystem in a browser"},
{"sysRename",  "File", "no filesystem in a browser"},
{"sysListDir", "File", "no filesystem in a browser"},
```

The gate continues to have two tiers:

- reachable from user code: deterministic compile-time error;
- unreachable prelude over-approximation: call `lvrt_unsupported`, preserving
  linkability without exposing the capability.

Each new table entry needs its own negative wasm fixture. One fixture that
mentions all three is insufficient because compilation stops at the first
diagnostic and would not prove the other two entries exist.

### 9.4 No front-end or other backend changes

`src/Resolver.cpp` and `src/RuntimeNatives.cpp` already contain the normative
surface and interpreter behavior. `src/CGen.cpp` and `src/X64Gen.cpp` retain
their current clean unsupported-native paths.

---

## 10. Security, reliability, concurrency, and performance

### 10.1 Security boundary

These are low-level `std::sys*` calls running with the process's filesystem
authority. They do not sandbox, authorize, canonicalize, resolve symlinks, or
prevent `..` traversal. A higher-level application must constrain untrusted
paths before calling them.

The runtime must not log raw paths or platform error text. Paths may contain
secrets; the established API deliberately exposes only `0`/`-1`/`None`.

`sysRemove` is non-recursive and never follows up with a tree walk. This keeps
the blast radius bounded to the named entry. Browser wasm remains denied at
compile time.

### 10.2 Reliability and cleanup

Every open directory/search handle closes on every exit path. Every staging
allocation is either transferred into `LvDirEntries` or freed before failure.
The free helper is safe for empty results. Integer-overflow checks precede
every allocation-size computation.

Tests must include repeated successful and failed listings under ASan/UBSan
and, when available, Valgrind. A loop must leave the runtime's live-byte count
flat after releasing each returned array; this catches missing array-element
ownership and double-retain errors.

### 10.3 Concurrency and filesystem races

All new floor state is stack- or call-owned, so calls are reentrant and can run
from different OS threads. No global cache or mutex is introduced.

The filesystem remains concurrently mutable. Listing is weakly consistent;
rename/remove can lose races and return `-1`. The implementation must not
retry against a re-resolved path, because that could act on a different entry
than the caller named.

### 10.4 Complexity and resource use

For `n` entries containing `b` bytes of names:

- floor enumeration: `O(n + b)` time;
- runtime conversion: `O(n + b)` time;
- pointer-table growth: amortized `O(n)`;
- transient floor memory: `O(n + b)`;
- returned language value: `O(n + b)`.

Peak construction holds the C staging copy and the runtime copy at once. This
is accepted for the v1 scalar directory-listing API in exchange for a simple,
single-pass, cross-platform boundary. A streaming `DirIterator` would be a new
language/runtime feature and is not smuggled into parity work.

No sort is performed, avoiding extra `O(n log n)` work and preserving oracle
semantics.

### 10.5 Operational visibility

There is no new production telemetry surface. Observable outcomes remain the
native return value, the existing LLVM compile diagnostic, and the runtime's
existing heap meter/debug modes. Verification logs must distinguish:

- host POSIX pass;
- wasm capability pass;
- Win32 pass or explicit toolchain skip;
- Sonar consumer pass.

---

## 11. File-by-file implementation plan

| File | Required change |
|---|---|
| `runtime/lv_plat.h` | Add `LvDirEntries`, scalar declarations, list/free declarations, and ownership comments. |
| `runtime/lv_plat_posix.c` | Add `<dirent.h>`; implement remove, rename, list, and cleanup. |
| `runtime/lv_plat_win32.c` | Implement the same interface using Win32 APIs and dynamic search-pattern storage. |
| `runtime/lv_plat_wasm.c` | Add unavailable-capability stubs and safe empty cleanup. |
| `tests/spike-wasm/lv_plat_spike.c` | Keep the standalone spike floor interface-complete with matching stubs. |
| `runtime/lv_abi.h` | Declare the three compiler-facing `lvrt_*` functions and document ownership. |
| `runtime/lv_runtime.c` | Add scalar wrappers and boxed optional-array construction; no direct OS calls. |
| `src/LlvmGen.cpp` | Add callees, initializers, dispatch cases, `retainDst()` for list only, and wasm gate rows. |
| `runtime/selftest.c` | Extend file-native coverage for empty/non-empty list, rename, file removal, directory removal, missing-list `None`, failure sentinels, and ARC churn. |
| `tests/run_sysnatives.sh` | Add the LLVM leg to the existing F3 round trip and unreadable-directory list probe; update stale comments. |
| `tests/corpus/wasm/gated_fs_remove.lev` + `.expected` | Pin `sysRemove`'s wasm `File` diagnostic. |
| `tests/corpus/wasm/gated_fs_rename.lev` + `.expected` | Pin `sysRename`'s wasm `File` diagnostic. |
| `tests/corpus/wasm/gated_fs_listdir.lev` + `.expected` | Pin `sysListDir`'s wasm `File` diagnostic. |
| `CMakeLists.txt` | Update stale sys-native test comments; existing targets discover the extended runtime, sys-native, and wasm cases. |
| `docs/reference.md` | Replace the stale Track 08 coverage paragraph with the actual per-backend matrix. |
| `designs/sonar_v2/dom/techdesign-07-dialogs.md` | Replace the resolved LLVM-blocker note with the verified oracle/IR/LLVM result. |

No Sonar source, test, or runner change is expected. Its existing auto-
discovered `dom-dialogs` project is the consumer acceptance test.

### 11.1 Explicit no-change list

- `src/Resolver.cpp`: signatures and optional typing are already correct.
- `src/RuntimeNatives.cpp`: oracle/IR implementation remains the spec.
- `src/CGen.cpp`: emit-C++ system-layer boundary remains.
- `src/X64Gen.cpp`: frozen backend remains untouched.
- `sonar_v2/src/dom/dialogs.lev`: already uses the documented surface.
- `sonar_v2/tests/runtests.sh`: already auto-discovers and runs LLVM.

---

## 12. Milestones and delivery order

### M0 — baseline and ownership probes

- Confirm the current F3 script passes on oracle/IR and fails only LLVM.
- Confirm `dom-dialogs` fails at LLVM native dispatch, not earlier.
- Add/prepare the focused selftest assertions before wiring codegen.

**Exit:** the failure is reproduced and the test expectations are pinned.

### M1 — complete the platform and runtime ABI

- Land the floor interface in all production floor files and the wasm spike.
- Add `lvrt_*` declarations and implementations.
- Run `runtime_selftest`, sanitizer, and platform audit.

**Exit:** direct runtime calls construct and release correct values; no compiler
dispatch exists yet.

### M2 — wire LLVM and protect wasm

- Add the three LLVM callees and dispatch cases.
- Add all three wasm gate rows and negative fixtures.
- Rebuild the host `liblvrt.a` before diagnosing any link failure.

**Exit:** a focused source program builds natively on the host, while each
wasm fixture fails with the exact `File` diagnostic.

### M3 — differential and consumer acceptance

- Promote the existing filesystem round trip and locked-list probe to LLVM.
- Run the Sonar DOM-dialog project through its existing matrix.
- Run any available aarch64 and Win32 conditional lanes.

**Exit:** oracle/IR/LLVM outputs are byte-identical and Sonar is unblocked.

### M4 — documentation and full regression

- Update the reference coverage matrix and stale test comments.
- Run the full suite.
- Record exact skips; do not turn a skipped Windows lane into a support claim.

**Exit:** every required gate in §13 passes and the implementation can be
moved to `designs/complete/` only then.

### 12.1 Stop conditions

Stop and revise the design rather than patching around any of these findings:

- `Array<string>?` requires a representation other than the existing
  `LV_ARR`/`LV_NONE` pair.
- `retainDst()` produces anything other than rc 1 for success or a no-op for
  `None`.
- a required OS primitive would have to be called from `lv_runtime.c`.
- the existing oracle/IR outputs disagree with the contract in §2 for the
  focused cases.
- Sonar requires a source workaround instead of passing solely from backend
  parity.

An unavailable MinGW/Wine toolchain is not a design stop; it is an explicit
verification limitation.

---

## 13. Verification matrix and acceptance commands

### 13.1 Runtime-level tests

Extend `test_sys_file_natives()` to cover:

1. list a new empty directory: `LV_ARR`, length 0;
2. create a file and list: length 1, exact entry name;
3. rename it and list: old absent, new present;
4. remove the file: `LV_INT 0`;
5. remove the now-empty directory: `LV_INT 0`;
6. list the removed path: `LV_NONE`, payload 0;
7. remove/rename a missing path: `LV_INT -1`;
8. attempt to remove a non-empty directory: `LV_INT -1` and contents remain;
9. repeat list/release in a churn loop and return to the starting live-byte
   mark.

The existing `runtime_selftest_valgrind` target automatically strengthens the
same coverage when Valgrind is installed.

### 13.2 Three-engine differential

Extend the existing `fs.lev` block in `tests/run_sysnatives.sh`, using a fresh
temporary base for each engine:

```text
mkdir=0
count=1
has_a=true
rename=0
rmFile=0
rmDir=0
gone=true
```

Run it on:

- tree-walk `--run`;
- `--ir`;
- a host executable produced by `--build-native`.

The assertion uses count/contains, never directory order. When not running as
root, the existing mode-000 `locked_list=true` probe also gains an LLVM leg.
Root retains the existing explicit skip because root bypasses the permission
edge.

### 13.3 wasm negative tests

Each direct native has a separate `gated_*.lev` file with the expected
substring:

```text
wasm-browser: 'File' is not available on this target (no filesystem in a browser)
```

These tests require LLVM wasm object emission but not a working browser
runtime archive, matching the existing gate harness.

### 13.4 Consumer acceptance

Run only the focused project first:

```bash
LEVIATHAN="$PWD/build/leviathan" \
TRIDENT="$PWD/build/trident" \
LVRT="$PWD/build/liblvrt.a" \
bash sonar_v2/tests/runtests.sh sonar_v2/tests/dom-dialogs
```

Expected result: the existing golden passes on oracle, IR, and LLVM. emit-C++
may retain its documented native-gap skip. No Sonar file may change merely to
make this pass.

### 13.5 Required command gate

```bash
cmake --build build --target leviathan trident lvrt runtime_selftest
ctest --test-dir build --output-on-failure \
  -R '^(runtime_selftest|runtime_selftest_valgrind|rt_platform_audit|sys_natives|corpus_wasm)$'
bash sonar_v2/tests/runtests.sh sonar_v2/tests/dom-dialogs
ctest --test-dir build --output-on-failure
```

`runtime_selftest_valgrind` may be absent by feature detection. The other
named host tests are required. Run the repository's sanitizer build of
`runtime_selftest` when available.

Conditional target checks:

```bash
runtime/build-triple.sh aarch64-linux-gnu
runtime/build-triple.sh x86_64-pc-windows-gnu
ctest --test-dir build -R '^(corpus_core_aarch64_qemu|corpus_win_wine)$' --output-on-failure
```

These execute only where their toolchains are configured. Their result must
be recorded as pass or explicit skip.

### 13.6 Documentation validation

- Search for stale claims that `sysListDir` is absent from the LLVM floor.
- Search for a coverage paragraph claiming only `sysMonotonic` is LLVM-
  supported.
- Confirm the file map in §11 matches `git diff --name-status`.

---

## 14. Rollout, compatibility, and rollback

### 14.1 Atomic compiler/runtime delivery

The compiler starts emitting references to three new runtime symbols. A new
compiler paired with an old cached `liblvrt.a` will fail at link time. Therefore:

- land `LlvmGen`, `lv_abi.h`, runtime wrappers, and every floor body in one
  atomic change;
- rebuild the host `lvrt` target before focused LLVM tests;
- rebuild per-triple runtime archives before cross-target tests;
- do not publish a compiler binary separately from its matching runtime
  archive.

This is an additive ABI change. Existing binaries and source programs do not
need migration.

### 14.2 Rollout order

1. merge code and focused tests together;
2. run host required gates;
3. run wasm negatives;
4. run Sonar consumer acceptance;
5. run available cross lanes and full regression;
6. update the design status/archive only after all required gates pass.

No feature flag is needed: unsupported LLVM programs fail today and begin
working after the atomic compiler/runtime update. There is no stored data or
schema migration.

### 14.3 Rollback

Rollback reverts LLVM dispatch, runtime ABI, and floor additions together.
Because there is no persistent format change, rollback requires no data
repair. Programs using these natives return to a deterministic compile-time
coverage failure rather than silently changing filesystem behavior.

The wasm capability rows should be reverted only with the LLVM dispatch; a
half-rollback that leaves dispatch but removes the gate is forbidden.

---

## 15. Risks and failure handling

| Risk / failure | Required behavior / mitigation |
|---|---|
| Old runtime archive paired with new compiler | Link fails loudly; rebuild and ship compiler/runtime atomically. |
| Destination register previously owns a heap value | Existing `destKind == 2` wrapper releases it before accepting the new transfer. |
| `sysListDir` returns `None` | Unconditional retain is a no-op because `LV_NONE` is non-counted. |
| Entry retain omitted | Runtime selftest live-byte/element checks fail; copy `sysArgs` exactly. |
| Array retained twice | Churn/live-byte checks expose leaked arrays; only dispatch retains the array. |
| `realloc` failure loses the old table | Use a temporary, clean partial state, return `None`. |
| Huge directory overflows a size computation | Check before doubling/multiplying/converting; fail without partial output. |
| Directory/search handle leak | Single cleanup path; sanitizer/Valgrind and repeated listing coverage. |
| Directory mutates while listed | Weakly consistent OS result; no retry, sort, or snapshot claim. |
| Remove races with path type change | Return `-1`; never recurse or re-resolve to another target. |
| Win32 replacement differs from POSIX | Use `MOVEFILE_REPLACE_EXISTING`, omit cross-volume copy; document remaining platform limits. |
| Win32 code never compiled locally | Mark conditional lane skipped; do not claim verification. |
| wasm dispatch reaches a real floor stub | Three separate gate fixtures prevent this regression; stubs still fail closed. |
| Direct OS call leaks into `lv_runtime.c` | `rt_platform_audit` is a required gate. |
| Directory order makes tests flaky | Assert membership/count, never exact sequence. |
| Docs continue to understate LLVM coverage | Bundle the reference matrix update and stale-comment search. |

---

## 16. Alternatives considered

### 16.1 Two-pass count then fill

Rejected. It avoids a floor-owned pointer table but opens a race between
passes, can observe two different directory states, doubles enumeration, and
requires a more complicated callback or fill interface.

### 16.2 Build the Leviathan array inside each platform file

Rejected. It couples POSIX/Win32 code to `LvValue`, ARC, and boxed-array
layout, violating the platform seam and duplicating ownership logic.

### 16.3 Call `opendir`/Win32 APIs directly from `lv_runtime.c`

Rejected. It violates the enforced architecture and would require conditional
OS code in the shared runtime.

### 16.4 Return a packed byte blob instead of `char **`

Viable but not chosen. A packed table can reduce allocations, but it needs
offset bookkeeping and resize fixups in both platform files. Directory
listing is not hot enough to justify the extra correctness surface for this
parity fix.

### 16.5 Sort entries for deterministic results

Rejected. The oracle does not sort, the public contract does not promise
order, and sorting adds work plus a new observable behavior.

### 16.6 Add a detailed filesystem error type

Rejected as a different language/API proposal. The existing F3 family
deliberately uses `0`/`-1` and `None`.

### 16.7 Implement emit-C++ and pure ELF at the same time

Rejected. emit-C++ intentionally omits the system layer, and pure ELF is
frozen. Expanding either would turn a bounded LLVM parity fix into backend
policy work.

### 16.8 Expose browser filesystem access on wasm

Rejected. Browser filesystem capabilities require an explicit host API,
permission UX, and async design. Returning native-looking synchronous results
through the current floor would be misleading. The existing capability gate
is the correct behavior.

---

## 17. Request-acceptance mapping and definition of done

| Request criterion | Design closure |
|---|---|
| LLVM native build succeeds and links | §§8–9 add all ABI and dispatch pieces; §13.5 builds and links. Package `trident build` rides the same LLVM path. |
| Differential list/remove/rename behavior | §13.2 promotes the existing exact round trip to oracle/IR/LLVM. |
| Sonar DOM dialogs pass LLVM with no Sonar changes | §13.4 runs the already-existing auto-discovered consumer project. |
| Preserve remove fallback and scalar errors | §§2.1 and 7.1 make the fallback order normative. |
| Resolve optional-array `None` representation | §§5 D2 and 8.4 settle it using existing tagged values and unconditional retain. |
| Decide ELF scope | §§4 and 5 D6 explicitly exclude the frozen backend. |

The implementation is complete only when all of the following are true:

- every file in §11 is updated as applicable;
- host POSIX runtime, platform audit, differential, wasm-gate, and focused
  Sonar tests pass;
- the full suite passes;
- no required test is silently skipped;
- conditional cross lanes are reported honestly;
- reference documentation matches the as-built backend matrix;
- the tech design records the as-built deltas and is moved to
  `designs/complete/` only after those gates, not merely after code compiles.

Moving the request to `designs/requests/accepted/` approves this plan. It does
not claim that the implementation has landed.

---

## 18. Source map

Grounded against commit `5ba1fe7`; symbol names are more durable than line
numbers.

- Language surface: `src/Resolver.cpp` (`sysMkdir`, `sysRemove`,
  `sysRename`, `sysListDir`).
- Oracle/IR reference: `src/RuntimeNatives.cpp`, Track 08 F3 directory block.
- LLVM wasm gate: `src/LlvmGen.cpp`, `wasmGatedNative`.
- LLVM ARC classification: `src/LlvmGen.cpp`, `destKind` and the
  `CallNativeFn` lowering.
- Existing LLVM precedent: `rtSysStat`/`rtSysMkdir`, `sysArgs`, `sysEnv`, and
  `retainDst` cases in `src/LlvmGen.cpp`.
- Runtime value/ARC proof: `runtime/lv_runtime.c`, `lv_is_counted`,
  `lvrt_retain`, `lvrt_arr_new`, `lvrt_str_new`, and `lvrt_sysargs`.
- Runtime ABI precedent: `runtime/lv_abi.h`, `lvrt_sysstat`/
  `lvrt_sysmkdir`/`lvrt_sysargs`.
- Platform seam: `runtime/lv_plat.h`; implementations in
  `runtime/lv_plat_posix.c`, `runtime/lv_plat_win32.c`, and
  `runtime/lv_plat_wasm.c`.
- Cross-runtime selection: `runtime/build-triple.sh`.
- Platform invariant: `tests/run_rt_platform_audit.sh`.
- Existing differential: `tests/run_sysnatives.sh`.
- Wasm gate harness: `tests/run_wasm.sh` and `tests/corpus/wasm/`.
- Consumer gate: `sonar_v2/tests/runtests.sh` and
  `sonar_v2/tests/dom-dialogs/`.
- Public surface and stale coverage note: `docs/reference.md` §6.6.58.
- Backend policy: `info.md` §0 and §17.
