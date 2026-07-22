# Research: LLVM native codegen for `sysListDir`/`sysRemove`/`sysRename`

Research in support of a tech design implementing
`designs/requests/request-llvm-fs-dir-natives.md`. Everything below was verified
directly against the current tree (branch `agent3`), not assumed from the
request text. Line numbers are current as of this research pass and will
drift; treat them as pointers, re-grep before writing code.

## 0. Executive summary

The request undersells the change slightly. It frames the port as "`lvrt_sysmkdir`
+ the `rtSysMkdir` dispatch case is the template" — true, but that template is
two layers thinner than what these three natives actually need:

1. **The front end is already 100% done.** `src/Resolver.cpp:1145-1148` already
   declares `sysMkdir`/`sysRemove`/`sysRename`/`sysListDir` with full types in
   the prelude, the checker already accepts `Array<string>? == None`, `??`, and
   friends on `sysListDir`'s result today (proven independently by
   `JsonValue.asArray() -> Array<JsonValue>?`, which is already LLVM-covered).
   **No Resolver/Checker work is needed.**
2. **The oracle/IR reference implementation is done** (`src/RuntimeNatives.cpp:1837-1876`,
   quoted in full below) — this is genuinely just the porting spec, as the
   request says.
3. **The C runtime is NOT a flat file.** `sysMkdir`/`sysStat` do not call
   `::mkdir`/`::stat` directly from `lv_runtime.c` — they route through a
   platform-abstraction seam (`runtime/lv_plat.h` → `lv_plat_posix.c` /
   `lv_plat_win32.c`) that the request's snippet doesn't mention at all. Porting
   `sysRemove`/`sysRename`/`sysListDir` correctly means adding **three new
   functions to that seam, in both platform files**, not just one `lv_runtime.c`
   function each. This is the biggest gap between the request's proposed patch
   and what "matching the `sysMkdir` precedent" actually requires.
4. **The wasm capability gate table doesn't know about these three natives.**
   `sysStat`/`sysMkdir` are both already listed in `LlvmGen.cpp`'s
   `wasmGatedNative` table (`"no filesystem in a browser"`); `sysRemove`/
   `sysRename`/`sysListDir` are not, and the request never mentions wasm. This
   is a real gap the design needs to close, not a hypothetical.
5. **The "one wrinkle" (optional-array None representation) is not actually a
   design decision — it's already resolved by existing code**, on two
   independent lines of evidence (§5 below). The tech design can state the
   answer as a fact, not pose it as an open question.
6. **`X64Gen.cpp` (`--emit-elf`) and `CGen.cpp` (`--build`/`--emit-cpp`) should
   both be explicitly out of scope**, and the evidence is stronger than the
   request's "not investigated" hedge suggests: `X64Gen.cpp` never got
   `sysMkdir` either (only `sysStat`, at a much older cut), and `CGen.cpp` is
   further behind still (no `sysStat`/`sysMkdir` at all). `CMakeLists.txt:317-319`
   contains a maintainer comment stating outright that ELF ("`--emit-elf`") "is
   not a project target." Extending either backend for this ticket would be
   scope creep against the codebase's own stated direction.

## 1. The five-layer architecture this feature crosses

```
 Lev source (std::sysListDir(path))
        |
        v
 [1] src/Resolver.cpp          prelude native signature — DONE, no change needed
        |
        v
 [2] bytecode: Op::CallNativeFn { sname: "sysListDir", a: dst, c: argBase }
        |
        +---------------------------+---------------------------+
        v                           v                           v
 [3a] src/Eval.cpp            [3b] src/IrInterp.cpp        [3c] src/LlvmGen.cpp
   (tree-walk oracle)            (IR interpreter)             (native codegen)
        |                           |                           |
        +----------> src/RuntimeNatives.cpp <--------------------+  <- THIS IS DONE
        |            (shared native impl,                        |     (the spec)
        |             both interpreters call it)                 |
        |                                                         v
        |                                              [4] runtime/lv_abi.h (decl)
        |                                                  runtime/lv_runtime.c (impl)
        |                                                         |
        |                                                         v
        |                                              [5] runtime/lv_plat.h (interface)
        |                                                  runtime/lv_plat_posix.c
        |                                                  runtime/lv_plat_win32.c
        v
   process syscalls directly (opendir/unlink/rename in RuntimeNatives.cpp)
```

Layers 1–2 and the oracle/IR side of 3 are complete and untouched by this
ticket. **This ticket is entirely layers 4, 5, and the LLVM side of 3.**
`src/CGen.cpp` (emit-C++) and `src/X64Gen.cpp` (frozen ELF) are separate,
independent native-dispatch tables that also exist but are argued out of scope
in §7.

## 2. Layer 2/3a/3b: the reference spec (already landed, quote it verbatim)

`src/RuntimeNatives.cpp:1836-1876` — this is what oracle and IR interpreter both
already do, byte-identically (both route through this one function):

```cpp
// --- Track 08 F3: dirs & fs metadata -----------------------------------
// Scalar 0/-1 results, the sysStat/sysOpen shape. mkdir mode 0755.
if (name == "sysMkdir") {
    const std::string& path = args.size() > 0 ? args[0].s : out.s;
    out = vint(::mkdir(path.c_str(), 0755) == 0 ? 0 : -1);
    return true;
}
// sysRemove: unlink a file; a directory reports EISDIR (EPERM on some
// systems), so fall back to rmdir (design F3). -1 if neither succeeds.
if (name == "sysRemove") {
    const std::string& path = args.size() > 0 ? args[0].s : out.s;
    if (::unlink(path.c_str()) == 0) { out = vint(0); return true; }
    if (errno == EISDIR || errno == EPERM) {
        out = vint(::rmdir(path.c_str()) == 0 ? 0 : -1); return true;
    }
    out = vint(-1);
    return true;
}
if (name == "sysRename") {
    const std::string& from = args.size() > 0 ? args[0].s : std::string();
    const std::string& to   = args.size() > 1 ? args[1].s : std::string();
    out = vint(::rename(from.c_str(), to.c_str()) == 0 ? 0 : -1);
    return true;
}
// sysListDir(path) -> Array<string>?  entry names, no "."/"..". None =
// not a directory / can't open (distinguishable from an empty directory,
// which is [] — the three-state fact, design F3).
if (name == "sysListDir") {
    const std::string& path = args.size() > 0 ? args[0].s : out.s;
    DIR* d = ::opendir(path.c_str());
    if (!d) { out = vnone(); return true; }
    std::vector<Value> names;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string nm = e->d_name;
        if (nm == "." || nm == "..") continue;
        names.push_back(vstr(nm));
    }
    ::closedir(d);
    out = varr(std::move(names));
    return true;
}
```

Every downstream C/LLVM implementation must match this byte-for-byte,
including the `EISDIR`/`EPERM` fallback order (unlink first, rmdir only on
those two errnos, `-1` for anything else) and the `.`/`..` filtering.

## 3. Layer 5: the platform floor — the part the request's snippet skips

`runtime/lv_plat.h` (header comment, line 1-12) states the rule the whole
runtime is built around:

> "Track B — the platform floor. The ONLY place the runtime touches an OS...
> `lv_runtime.c` / `lv_loop.c` must never call an OS primitive directly —
> everything routes through here so a new target (macOS, Windows, aarch64) is
> a new `lv_plat_*.c` file, nothing else."

This is not decorative. `lvrt_sysstat`/`lvrt_sysmkdir` in `lv_runtime.c` do not
call `::stat`/`::mkdir` — they call `lv_plat_stat_size`/`lv_plat_mkdir`:

```c
// runtime/lv_runtime.c:2635-2657
void lvrt_sysstat(LvValue* out, const LvValue* path, const LvValue* field) {
    const char* cpath = (const char*)(P8(path->payload) + 8);
    int64_t size = lv_plat_stat_size(cpath);
    out->tag = LV_INT;
    if (field->payload == 0) out->payload = size >= 0 ? 1 : 0;
    else if (field->payload == 1) out->payload = size;
    else if (field->payload == 2) out->payload = lv_plat_stat_mtime(cpath);
    else if (field->payload == 3) out->payload = lv_plat_stat_isdir(cpath);
    else out->payload = -1;
}

void lvrt_sysmkdir(LvValue* out, const LvValue* path) {
    const char* cpath = (const char*)(P8(path->payload) + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_mkdir(cpath);
}
```

`lv_plat.h:94-97` declares those four; `lv_plat_posix.c:271-298` and
`lv_plat_win32.c:244-276` each implement all four — one POSIX body, one Win32
body, same signature. **There is currently no `lv_plat_remove`/
`lv_plat_rename`/`lv_plat_listdir` in the interface at all** — zero precedent
in either platform file (confirmed by grep: no `dirent`/`opendir`/`readdir`/
`FindFirstFile` anywhere in `runtime/`). This is genuinely new code, not a
copy-paste — the one piece of this ticket that isn't pure mechanical porting,
because there's no existing multi-entry directory-listing helper to crib the
shape from inside the runtime (RuntimeNatives.cpp's C++ `<dirent.h>` usage is
the only precedent, and it's C++ `std::vector`/`std::string`, not the C ABI).

### 3.1 Proposed `lv_plat.h` additions

Insert alongside the existing fs block (`lv_plat.h:94-97`):

```c
int64_t lv_plat_stat_size(const char* path);
int64_t lv_plat_stat_mtime(const char* path);
int     lv_plat_stat_isdir(const char* path);
int     lv_plat_mkdir(const char* path);
/* NEW: */
int     lv_plat_remove(const char* path);   /* unlink, rmdir-fallback-on-EISDIR/EPERM; 0/-1 */
int     lv_plat_rename(const char* from, const char* to);   /* 0/-1 */
/* Directory listing needs an out-parameter shape, not a scalar return — see
 * §3.2/§3.3. lv_plat_listdir fills a caller-allocated growable buffer of
 * C strings; the lv_runtime.c wrapper turns that into an LvValue Array<string>. */
typedef struct LvDirEntries { char** names; int64_t count; } LvDirEntries;
int     lv_plat_listdir(const char* path, LvDirEntries* out);  /* 0 ok / -1 not-a-dir */
void    lv_plat_listdir_free(LvDirEntries* entries);
```

**Design note for the tech design to ratify**: `lv_plat_listdir`'s signature is
a judgment call, not dictated by precedent (no existing `lv_plat_*` function
returns a variable-length collection — sockets/spawn return fixed-shape
scalars or write into caller-owned fixed slots). Two reasonable shapes:

- **(a) two-pass count-then-fill**, mirroring `lvrt_arr_new`'s own two-pass
  convention seen elsewhere in `lv_runtime.c` (line 1628: "Two passes: count
  scalars, then fill (`lvrt_arr_new` pre-sizes exactly)") — `lv_plat_listdir`
  itself does one `opendir`/`readdir` pass to count, then the `lv_runtime.c`
  caller allocates the `LvValue` array and a second pass fills it directly via
  a callback or a second `lv_plat_listdir` call. Avoids a heap allocation
  inside the platform floor entirely.
- **(b) single-pass into a floor-owned growable buffer** (`malloc`/`realloc` of
  `char*[]`, freed by `lv_plat_listdir_free`), then `lv_runtime.c` copies
  `count` C strings into a freshly-sized `LvValue` array. Simpler code, one
  extra allocation-and-free pass, matches the sketch above.

Given `sysListDir` is not hot-path (directory listings, not per-frame calls),
(b)'s simplicity is likely the right trade — but this is exactly the kind of
small interface question a tech design should settle explicitly rather than
leave to whoever implements it, since it fixes the shape of both platform
files at once.

### 3.2 POSIX implementation sketch (`lv_plat_posix.c`)

Modeled directly on the existing `lv_plat_mkdir` (line 296-298) and the oracle
spec in §2:

```c
/* runtime/lv_plat_posix.c — alongside lv_plat_mkdir */
int lv_plat_remove(const char* path) {
    if (unlink(path) == 0) return 0;
    if (errno == EISDIR || errno == EPERM) return rmdir(path) == 0 ? 0 : -1;
    return -1;
}

int lv_plat_rename(const char* from, const char* to) {
    return rename(from, to) == 0 ? 0 : -1;
}

int lv_plat_listdir(const char* path, LvDirEntries* out) {
    DIR* d = opendir(path);
    if (!d) { out->names = NULL; out->count = 0; return -1; }
    size_t cap = 16, n = 0;
    char** names = malloc(cap * sizeof(char*));
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n == cap) { cap *= 2; names = realloc(names, cap * sizeof(char*)); }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    out->names = names; out->count = (int64_t)n;
    return 0;
}

void lv_plat_listdir_free(LvDirEntries* entries) {
    for (int64_t i = 0; i < entries->count; i++) free(entries->names[i]);
    free(entries->names);
}
```

`<dirent.h>` is not currently included in `lv_plat_posix.c` — check the header
block at the top of the file and add it alongside the existing `<sys/stat.h>`.
`errno`/`<errno.h>` is already included and already used for exactly this
kind of errno-branch elsewhere in the same file (`lv_plat_posix.c:373, 439,
442, 528, 537`), so `lv_plat_remove`'s `EISDIR`/`EPERM` check is idiomatic for
this file, not a new pattern.

### 3.3 Win32 implementation sketch (`lv_plat_win32.c`)

**Important caveat surfaced by this research, not in the request**:
`lv_plat_win32.c`'s file header (lines 1-9) says outright:

> "STATUS: PRE-LAND, COMPILE-UNVERIFIED. Written to spec on a host with no
> MinGW-w64 toolchain... It is in NO default CMake target — it compiles only
> when `runtime/build-triple.sh x86_64-pc-windows-gnu` ... selects it."

So `lv_plat_stat_*`/`lv_plat_mkdir`'s Win32 bodies (which the request treats as
"already covered, so this is just the porting template") are **themselves
unverified** — nobody has compiled them, not just this ticket's new functions.
Adding `lv_plat_remove`/`lv_plat_rename`/`lv_plat_listdir` here inherits that
exact same status: correct-per-spec, uncompiled, verified only if/when someone
has a MinGW-w64 toolchain (`corpus_win_wine` test lane, `CMakeLists.txt:1467-1480`,
is feature-detected and currently skips on hosts without `wine` + MinGW). The
tech design should treat Windows coverage for these three natives as
**best-effort parity, explicitly not gated on manual verification**, matching
how `lv_plat_stat_*`/`lv_plat_mkdir` already shipped.

Sketch, following the existing `GetFileAttributesExA`/`CreateDirectoryA` style
(`lv_plat_win32.c:244-276`):

```c
/* runtime/lv_plat_win32.c — alongside lv_plat_mkdir */
int lv_plat_remove(const char* path) {
    DWORD attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return -1;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryA(path) ? 0 : -1;
    return DeleteFileA(path) ? 0 : -1;
}

int lv_plat_rename(const char* from, const char* to) {
    /* MOVEFILE_REPLACE_EXISTING: POSIX rename(2) silently replaces an
     * existing `to`; plain MoveFileA does not and fails instead — needed
     * for byte-parity with the oracle's ::rename. */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}

int lv_plat_listdir(const char* path, LvDirEntries* out) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { out->names = NULL; out->count = 0; return -1; }
    size_t cap = 16, n = 0;
    char** names = malloc(cap * sizeof(char*));
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (n == cap) { cap *= 2; names = realloc(names, cap * sizeof(char*)); }
        names[n++] = _strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    out->names = names; out->count = (int64_t)n;
    return 0;
}

void lv_plat_listdir_free(LvDirEntries* entries) { /* identical body to POSIX */ }
```

Note Win32's remove semantics differ structurally from POSIX (query-then-act
via `GetFileAttributesA`, vs. POSIX's try-unlink-then-fall-back-on-errno) —
that's expected and fine (the platform floor's whole point is hiding exactly
this kind of divergence), but the tech design should say so explicitly rather
than imply a literal 1:1 translation.

## 4. Layer 4: `lv_abi.h` + `lv_runtime.c` (the direct `lvrt_sysmkdir` analog)

`lv_abi.h:632-636` declares the natives that operate on this family (append
here, colocated with `lvrt_sysstat`/`lvrt_sysmkdir`):

```c
void    lvrt_sysstat(LvValue* out, const LvValue* path, const LvValue* field);
void    lvrt_sysmkdir(LvValue* out, const LvValue* path);
/* NEW: */
void    lvrt_sysremove(LvValue* out, const LvValue* path);
void    lvrt_sysrename(LvValue* out, const LvValue* from, const LvValue* to);
void    lvrt_syslistdir(LvValue* out, const LvValue* path);  /* Array<string> +1 | LV_NONE */
```

This confirms the request's proposed names verbatim — `lvrt_sysremove`,
`lvrt_sysrename`, `lvrt_syslistdir` match the codebase's existing
no-underscore-before-native-name convention exactly (c.f. `lvrt_systlsconnect`,
`lvrt_syssignalopen`, `lvrt_sysenv`) — no naming decision needed.

`lv_runtime.c` implementations, modeled on `lvrt_sysmkdir` (scalar natives) and
`lvrt_sysargs`/`lvrt_sysspawn` (the `Array<T>`-construction template):

```c
/* runtime/lv_runtime.c — alongside lvrt_sysmkdir */
void lvrt_sysremove(LvValue* out, const LvValue* path) {
    const char* cpath = (const char*)(P8(path->payload) + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_remove(cpath);
}

void lvrt_sysrename(LvValue* out, const LvValue* from, const LvValue* to) {
    const char* cfrom = (const char*)(P8(from->payload) + 8);
    const char* cto   = (const char*)(P8(to->payload) + 8);
    out->tag = LV_INT;
    out->payload = lv_plat_rename(cfrom, cto);
}

/* Array<string>? — the exact lvrt_sysargs boxed-array-construction shape
 * (lv_runtime.c:2881-2890), None substituted for the "not a directory" case. */
void lvrt_syslistdir(LvValue* out, const LvValue* path) {
    const char* cpath = (const char*)(P8(path->payload) + 8);
    LvDirEntries ents;
    if (lv_plat_listdir(cpath, &ents) != 0) {
        out->tag = LV_NONE; out->payload = 0;
        return;
    }
    lvrt_arr_new(out, ents.count);            /* rc 0, length n, slots void */
    for (int64_t i = 0; i < ents.count; i++) {
        LvValue s; lvrt_str_new(&s, ents.names[i], (int64_t)strlen(ents.names[i]));  /* rc 0 fresh */
        lv_st_val(out->payload, 8 + 16 * i, &s);
        lvrt_retain(&s);                       /* buffer owns element: rc 0->1 */
    }
    lv_plat_listdir_free(&ents);
}
```

This is a direct copy of `lvrt_sysargs`'s loop body (`lv_runtime.c:2881-2890`)
— same `lvrt_arr_new` pre-size, same `lv_st_val` + `lvrt_retain` per-element
pattern. The array itself is returned at **rc 0** (unowned) by convention —
"the caller/codegen retains it (+1 transfer)" per the `lvrt_sysargs` doc
comment (`lv_runtime.c:2879`) — which is exactly what `LlvmGen.cpp`'s
`retainDst()` does at the call site (§6).

## 5. The "one wrinkle" is already resolved — not an open design question

The request flags `Array<string>?`'s `None` representation as "the one piece
here that isn't pure plumbing" and asks the design to "resolve the
None-representation question against however another optional/
nullable-returning native already lowers on LLVM (if one exists)." **One does
exist, and the answer is unconditionally simple: no branching needed at all.**

Two independent lines of evidence:

**(a) `sysEnv` (`string?`) is already LLVM-covered** and does exactly this
(`LlvmGen.cpp:2789-2793`):

```cpp
} else if (n == "sysEnv") {
    // bug #68: env var read on the compiled backends —
    // fresh string (retain +1) or None (retain gate-skips).
    b.CreateCall(rtSysEnv, {regs[in.a], arg(0)});
    retainDst();
}
```

`lvrt_sysenv` (`lv_runtime.c:2792-2797`) sets `out->tag = LV_NONE; out->payload = 0;`
on the unset-variable path, with **no special-casing on the LLVM side** — the
same unconditional `retainDst()` call follows both the string-success and the
None path.

**(b) Why that's safe**: `lvrt_retain` (`lv_runtime.c:265-269`) begins with
`if (!lv_is_counted(v->tag, v->payload)) return;`, and `lv_is_counted`
(`lv_runtime.c:212-213`) explicitly excludes `LV_NONE`:

```c
static int lv_is_counted(int64_t tag, int64_t payload) {
    if (tag < LV_STR || tag == LV_NONE) return 0;  /* scalars, none */
    ...
}
```

So retaining a `{LV_NONE, 0}` value is a guaranteed no-op — `retainDst()` is
**unconditionally correct** to call after any natively-constructed
optional-heap-value, regardless of whether the specific call produced `Some`
or `None`. This is precisely the comment pattern already used at every other
optional-heap-return call site in `LlvmGen.cpp` (`sysRecv`: "fresh recv string
-> +1 (None/"" gate-skips)"; `sysRsaEncrypt`: "fresh string -> +1 (None
gate-skips)"). `sysListDir` needs the identical one-line comment, nothing more
— the request's proposed dispatch snippet (`retainDst()` unconditionally after
`CreateCall`) is already exactly correct as written.

**(c) Independent confirmation the general `Array<T>?` shape works on LLVM
today**, for a completely different native type: `JsonValue.asArray() ->
Array<JsonValue>?` (`src/Resolver.cpp:4991`, `kind == 4 ? items : None`) is
ordinary in-language ternary code, not a native at all, and `docs/reference.md:2262`
states JSON is "full-coverage on" all engines including LLVM. That proves the
LLVM value representation of "`Array<T>` or `None` in one register" is already
exercised and correct at the type level — `LV_ARR`-tagged payload vs.
`LV_NONE`-tagged zero payload, no third state, no wrapper struct. `sysListDir`
is just the first *native* (as opposed to in-language ternary) to produce that
shape.

**Recommendation for the tech design**: state this as settled fact (cite
`sysEnv` as precedent + `lv_is_counted`'s None exclusion as the proof), not as
an open question to re-litigate.

## 6. Layer 3c: `LlvmGen.cpp` — the actual missing piece

### 6.1 `FunctionCallee` field declarations

Add to the member list (`LlvmGen.cpp:228`, alongside the existing
`rtSysStat, rtSysMkdir`):

```cpp
rtSysRead, rtSysOpen, rtSysClose, rtSysStat, rtSysMkdir, rtSysArgs, rtSysNow, rtSysMonotonic,
rtSysRemove, rtSysRename, rtSysListDir,   // NEW — Track 08 F3 dirs, LLVM parity
```

### 6.2 `FunctionCallee` initialization

Add to the constructor (`LlvmGen.cpp:393`, right after `rtSysMkdir`'s `fn(...)` call):

```cpp
rtSysMkdir     = fn("lvrt_sysmkdir", voidTy, {ptrTy, ptrTy});   // Track 08 F3 dirs
rtSysRemove    = fn("lvrt_sysremove", voidTy, {ptrTy, ptrTy});
rtSysRename    = fn("lvrt_sysrename", voidTy, {ptrTy, ptrTy, ptrTy});
rtSysListDir   = fn("lvrt_syslistdir", voidTy, {ptrTy, ptrTy});
```

(Signature shape — `{ptrTy, ptrTy}` = `(out, path)`, `{ptrTy, ptrTy, ptrTy}` =
`(out, from, to)` — mirrors `rtSysMkdir`/`rtSysTlsConnect`'s multi-arg pattern
exactly; every arg crosses as an `LvValue*`, per the boundary-rule comment at
`LlvmGen.cpp:204-205` and `2596-2597`.)

### 6.3 Dispatch cases

Add to the `Op::CallNativeFn` if/else chain (`LlvmGen.cpp:2654`, right after
the existing `sysMkdir` case) — this is verbatim the request's proposed
snippet, confirmed correct by §5:

```cpp
} else if (n == "sysMkdir") {
    b.CreateCall(rtSysMkdir, {regs[in.a], arg(0)});
} else if (n == "sysRemove") {
    b.CreateCall(rtSysRemove, {regs[in.a], arg(0)});
} else if (n == "sysRename") {
    b.CreateCall(rtSysRename, {regs[in.a], arg(0), arg(1)});
} else if (n == "sysListDir") {
    b.CreateCall(rtSysListDir, {regs[in.a], arg(0)});
    retainDst();               // fresh heap Array<string> -> +1 (None gate-skips, §5)
}
```

`sysRemove`/`sysRename` need **no** `retainDst()` call — they return a plain
scalar `LV_INT`, identical to `sysMkdir`/`sysStat` immediately above them in
the same chain (neither of those calls `retainDst()` either). Only
`sysListDir` needs it, because it's the only one of the three that can produce
a heap value.

### 6.4 Why no other change is needed: the `destKind`/ARC wrap

`Op::CallNativeFn` is universally `destKind() == 2` ("transfer") — see the
table at `LlvmGen.cpp:81-96` and its governing comment (`LlvmGen.cpp:57-67`):

| `dk` | Meaning | Generic wrap behavior |
|---|---|---|
| 0 | `a` isn't a destination for this op | no release, no retain |
| 1 | fresh alloc/read/alias, slot doesn't yet own it | release old value, **retain** new value |
| 2 | value already at +1 (a "transfer") | release old value only, **no** retain |

`CallNativeFn` is `dk==2` because native calls that DO produce heap values
already hand back a value at +1 conceptually (the runtime function either
constructed it fresh at rc 0 and the dispatch case's own `retainDst()` bumps
it to 1, or it's a plain scalar with no ARC weight at all). The generic wrap
around every op (`LlvmGen.cpp:1455-1476`) automatically releases whatever was
previously in the destination register before the switch body runs — **that
part needs no new code for this feature**; it's shared infrastructure that
already handles all three new natives correctly by construction. The only
per-native work is exactly what §6.3 shows: the `CreateCall` + an optional
`retainDst()`.

`emitThrowCheck((int)pc)` runs unconditionally after the entire if/else chain
(`LlvmGen.cpp:2866`) regardless of which branch fired — none of these three
natives can raise (confirmed against the oracle spec in §2: no `err = ...`
assignment anywhere in the three handlers), so this is inert for them, exactly
as it already is for `sysMkdir`/`sysStat`.

## 7. Scope boundary: `X64Gen.cpp` and `CGen.cpp` should be excluded

The request hedges on `X64Gen.cpp`: "Same three natives for the frozen
pure-ELF backend... if that backend is meant to carry the rest of the F3
family too — `sysStat`/`sysMkdir`'s own ELF coverage status (present or
deferred) is the precedent to match; not investigated as part of filing this
ticket." This research investigated it.

### 7.1 `X64Gen.cpp` (`--emit-elf`): `sysStat` present, `sysMkdir` absent

`X64Gen.cpp:3818-3820` has a `sysStat` case (fixup index `-38`), but grepping
the entire file for `sysMkdir`/`Mkdir` turns up **nothing** — no case, no
fixup index reserved (`-38` is `sysStat`, `-39` is reused for both `sysNow`
and monotonic clock reads, `-40` is `sysTimerStart`; there is no gap for
`sysMkdir` anywhere in that numbering). So the precedent the request asks to
match is not "present" — it's **mixed**: `sysStat` (an F2/read-only native)
made it into this backend at some earlier cut, `sysMkdir` (F3, mutating) never
did, and this backend has never picked up any native newer than roughly
`sysUnwatch` (the chain ends at `LlvmGen.cpp`'s dispatch equivalent of
`sysWatch`/`sysUnwatch`, `X64Gen.cpp:3851-3860`) — no `sysArgs`, `sysEnv`,
`sysRandom`, `sysSpawn`, threads, tasks, or TLS either.

### 7.2 `CGen.cpp` (`--build`/`--emit-cpp`): further behind still

`CGen.cpp`'s `Op::CallNativeFn` case (`CGen.cpp:1039-1092`) is an even smaller
allow-list — `sysWrite`, `sysReadLine`, `sysRead`, `byteToString`,
`charFromCode`, `floatFromBits`, `sysArgs`, `sysThreadTransfer`, `sysNow`,
terminal-raw-mode natives, exit natives, and the `libm` transcendentals. **No
`sysStat`, no `sysMkdir`, nothing from the F3 family at all** — anything else
falls to `sink_.error({}, "native backend: native '" + in.sname + "'"); ok_ = false;`
(`CGen.cpp:1092`), a clean compile-time diagnostic exactly like the LLVM/ELF
"native floor function" message.

### 7.3 The project's own stated intent

`CMakeLists.txt:317-319`:

```cmake
# techdesign-columnar C-M5: the frozen x86-64/ELF backend has NO columnar lane
# (X64Gen can't lower ColGet). ELF is not a project target, so force row-major
# regardless of the (now columnar-on) default — never gate ELF on a columnar op.
```

"ELF is not a project target" is a maintainer statement, not an inference.
Combined with `sysMkdir`'s absence there despite living one line above
`sysStat` in the oracle, and `CGen.cpp`'s even sparser coverage, the
conclusion is: **neither backend has been kept in step with the F3 (or any
recent) native family, and this ticket should not be the one to start.** Both
already produce a clean, correctly-worded compile-time diagnostic today for
these three natives (`X64Gen.cpp:3860` / `CGen.cpp:1092`'s "native floor
function"/"native backend: native" messages) — that's the existing, working,
intentional behavior; leave it. The tech design should say this explicitly
(with this evidence) so the "not investigated" hedge in the request doesn't
recur as an open question later.

## 8. Gap the request didn't mention: the wasm capability gate

`LlvmGen.cpp:111-164` gates a specific set of natives behind a two-tier
wasm-target diagnostic (`wasmGatedNative` table) rather than letting them
attempt (and silently fail or miscompile against) a browser target. The F3
family is **already partially represented** there:

```cpp
{"sysOpen",         "File",      "no filesystem in a browser"},
{"sysStat",         "File",      "no filesystem in a browser"},
{"sysMkdir",        "File",      "no filesystem in a browser"},
```

`sysRemove`/`sysRename`/`sysListDir` are conspicuously **absent**. Left
unaddressed, once this ticket lands, a wasm build calling `sysListDir` would
either hit the (now-passing-for-non-wasm) native dispatch path and try to
compile a call to `lvrt_syslistdir` against a wasm target with no
`opendir`/`readdir`/`FindFirstFileA` (the platform floor has no wasm
implementation — there is no `lv_plat_wasm.c` in this codebase at all, only
posix/win32), or produce some other non-diagnostic failure. Either way it's
exactly the "never a miscompile, always a clean diagnostic" invariant the
wasm gate exists to guarantee (`LlvmGen.cpp:111-120`'s comment names
"filesystem/dirs" explicitly as one of wasm's "Lost" subset categories) —
this ticket accidentally creates a hole in that guarantee unless it also adds:

```cpp
{"sysRemove",       "File",      "no filesystem in a browser"},
{"sysRename",       "File",      "no filesystem in a browser"},
{"sysListDir",      "File",      "no filesystem in a browser"},
```

**Open question for the tech design**: the `wrapper` field (`"File"` above) is
what the diagnostic tells the user to use instead — but no `Dir`/`File`
wrapper class actually wraps `sysListDir`/`sysRemove`/`sysRename` today (grep
confirms no `class Dir` in the prelude; `sonar_v2/src/dom/dialogs.lev` calls
`std::sysListDir` directly, ungated). Reusing `"File"` (the existing wrapper
name for `sysOpen`/`sysStat`/`sysMkdir`) is the closest existing precedent and
is probably fine, but the tech design should make that call explicitly rather
than leave the wrapper name to whoever writes the patch.

## 9. Testing: exactly where the acceptance-criteria differential tests go

Acceptance criterion 2 asks for "a differential corpus case (list a directory,
remove a file, remove a directory, rename a file, list a nonexistent path)...
byte-identical across oracle, IR, and LLVM." That test **already exists** for
oracle+IR and just needs a third leg — it should not be written from scratch.

`tests/run_sysnatives.sh:42-68` ("`--- 3. dirs (F3): mkdir / write / list /
rename / remove round-trip ---`") already runs exactly this scenario end to
end (mkdir, write+close, list+count+contains, rename, remove-file,
remove-dir, list-again-expect-None) on both `--run` (oracle) and `--ir`. It is
currently missing a `--build-native` leg. The idiomatic third-leg pattern to
copy is right below it in the same file — `isdir.lev`'s LLVM check
(`tests/run_sysnatives.sh:95-99`):

```bash
if "$bin" --build-native "$work/isdir_llvm" "$work/isdir.lev" >/dev/null 2>&1; then
  check "isDir llvm" "$ISDIR_EXPECT" "$(LEV_ISDIR_BASE="$d3" "$work/isdir_llvm" 2>/dev/null)"
else
  echo "FAIL llvm (expected isDir to compile natively)"; fail=1
fi
```

Adding the equivalent `d1`/`fs.lev`/`FS_EXPECT` triple as a third
`--build-native` check closes acceptance criterion 2 directly, using the
existing golden string (`FS_EXPECT`, line 64) unchanged. Two comments in that
same file currently document the LLVM gap and will need updating/removing
once this lands (they'll otherwise be stale, misleading comments after the
fix ships):

- `tests/run_sysnatives.sh:81`: "isDir alone (LLVM-coverable: `sysStat` is on
  the LLVM floor, `sysListDir` is not)."
- `tests/run_sysnatives.sh:100-102`: "The actual regression the request cites:
  `sysListDir(locked)!=None` misclassifies... Interpreters only (`sysListDir`
  is not on the LLVM/emit-C++ floor)." — the `locked_list` check right below it
  (`tests/run_sysnatives.sh:103-112`) should also gain a `--build-native` leg
  once this lands, since it's specifically testing the `sysListDir`
  regression this whole family exists to fix.
- `tests/run_sysnatives.sh:275`: "fd-table churn hygiene probed via
  `sysOpen`'s lowest-available fd number (`sysListDir` is not on the LLVM
  floor)" — the `spinPty`/`fdCount` helper at line 238-243 uses `sysListDir`
  on `/proc/self/fd` specifically because it wasn't LLVM-coverable before;
  worth a follow-up note (not necessarily in-scope for this ticket) that a
  more direct fd-count probe is now possible on LLVM too.

`tests/corpus/sys_natives/sys_natives.lev` (the environment-neutral golden,
run across oracle/IR/LLVM/emit-C++ by the standard corpus runner, not
`run_sysnatives.sh`) already has one `sysListDir` line (line 23, the
`None`-on-nonexistent-path check) but **zero** coverage of `sysMkdir`/
`sysRemove`/`sysRename` and zero coverage of `sysListDir`'s success path
(non-empty-array case) — everything filesystem-mutating was deliberately
routed to `run_sysnatives.sh` instead (its own header comment, lines 3-4:
"The environment-dependent bits... live in `tests/run_sysnatives.sh`" —
mutating a real directory isn't golden-corpus-safe). That's the right
existing split; no new corpus-file test is needed, only the
`run_sysnatives.sh` third leg described above.

### 9.1 `sonar_v2/tests/dom-dialogs/` needs no manual wiring

`sonar_v2/tests/runtests.sh` (header comment, lines 1-11) auto-discovers every
directory under `tests/` containing a `trident.toml` + a `*.expected` golden —
confirmed by reading the discovery logic (`golden_for()`/`run_one()`,
`runtests.sh:32-49`). `sonar_v2/tests/dom-dialogs/` (already present in the
working tree per `git status`, with `dialogs.expected` + `dialogs.lev` +
`trident.toml`) will automatically start passing its LLVM leg once this
ticket lands — **no change to `runtests.sh` itself is needed**, contrary to
what the auto-discovery-unfamiliar reader might assume from the request's
acceptance criterion 3 wording ("no changes needed on the Sonar DOM side" is
correct and slightly broader than stated — no changes are needed to the test
*runner* either).

## 10. Documentation debt this ticket should probably also close

`docs/reference.md:1520-1527` ("**Engine coverage.**" note for the whole
Track 08 F1-F7 native family) is **already stale**, independent of this
ticket:

> "All of the above run on the tree-walk (oracle) and IR interpreters...
> `sysMonotonic` additionally runs on LLVM-native binaries... The remaining
> natives on the compiled backends (emit-C++, LLVM) and the frozen pure-ELF
> backend keep clean coverage-errors naming the native."

This text implies only `sysMonotonic` runs on LLVM among the whole F1-F7
family, but `LlvmGen.cpp`'s dispatch table already covers `sysStat`,
`sysMkdir`, `sysArgs`, `sysEnv`, `sysRandom`, `sysIsTty`-adjacent terminal
natives, `sysSpawn`, TLS, and more — the note was evidently never updated as
those landed. This ticket will make it more wrong (adding `sysRemove`/
`sysRename`/`sysListDir` to the LLVM-covered set), so it's a natural,
low-cost place to fix the whole paragraph rather than add one more
now-also-incorrect sentence. Not strictly required by the request's acceptance
criteria, but worth the tech design flagging as a one-paragraph doc fix
bundled with the same PR.

## 11. Complete file-by-file change surface

| File | Change | Precedent to copy |
|---|---|---|
| `runtime/lv_plat.h` | Add `lv_plat_remove`/`lv_plat_rename`/`lv_plat_listdir` (+ `LvDirEntries` type) declarations | `lv_plat_mkdir` decl, line 97 |
| `runtime/lv_plat_posix.c` | Implement the three above; add `<dirent.h>` include | `lv_plat_mkdir`, lines 296-298 |
| `runtime/lv_plat_win32.c` | Implement the three above (inherits "compile-unverified" status) | `lv_plat_mkdir`, lines 274-276 |
| `runtime/lv_abi.h` | Declare `lvrt_sysremove`/`lvrt_sysrename`/`lvrt_syslistdir` | `lvrt_sysmkdir` decl, line 635 |
| `runtime/lv_runtime.c` | Implement the three above | `lvrt_sysmkdir` (scalar), `lvrt_sysargs` (array construction) |
| `src/LlvmGen.cpp` | 3 `FunctionCallee` fields, 3 `fn(...)` inits, 3 dispatch cases + `retainDst()` on `sysListDir` only | `rtSysMkdir`/`sysMkdir` end to end |
| `src/LlvmGen.cpp` (`wasmGatedNative`) | Add 3 entries gating these natives on wasm targets | `sysStat`/`sysMkdir` entries, lines 125-126 |
| `tests/run_sysnatives.sh` | Add `--build-native` leg to the existing `fs.lev`/`FS_EXPECT` block (§3) and the `locked_list` check (§3b); update/remove now-stale "not on the LLVM floor" comments | `isdir.lev`'s existing LLVM leg, lines 95-99 |
| `docs/reference.md` | Update the §6.6.58 "Engine coverage" note (currently understates LLVM coverage even pre-this-ticket) | — |
| `src/Resolver.cpp` | **No change** — signatures already present, lines 1145-1148 | — |
| `src/RuntimeNatives.cpp` | **No change** — reference implementation already complete, lines 1836-1876 | — |
| `src/X64Gen.cpp` | **Explicitly out of scope** — see §7.1 | — |
| `src/CGen.cpp` | **Explicitly out of scope** — see §7.2 | — |

## 12. Open questions the tech design must settle explicitly

1. **`lv_plat_listdir`'s interface shape** — two-pass count-then-fill vs.
   single-pass-into-a-floor-owned-buffer (§3.1). Recommendation: (b),
   single-pass with `LvDirEntries`/`lv_plat_listdir_free`, for code simplicity
   given this isn't a hot path.
2. **wasm gate wrapper name** for the three new `wasmGatedNative` entries —
   reuse `"File"` (no dedicated `Dir` class exists to point users at) (§8).
3. **Whether to bundle the `docs/reference.md` coverage-note fix** into this
   ticket or file it separately (§10) — it's pre-existing staleness this
   ticket will otherwise compound.
4. **Whether to also add a `--build-native` leg to the `locked_list` check**
   (`tests/run_sysnatives.sh:103-112`), since that's literally the regression
   the original `sysListDir` feature (`request-stat-isdir.md`) was filed
   against, and it's currently interpreter-only for the same "not on the LLVM
   floor" reason this ticket fixes elsewhere in the same file.
